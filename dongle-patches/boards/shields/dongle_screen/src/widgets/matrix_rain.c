/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Matrix rain animation widget.
 *
 * Thread-safety model
 * ───────────────────
 * LVGL is not thread-safe.  All lv_* calls must come from a single context.
 *
 * The k_timer callback (rain_timer_cb) is the single owner of all LVGL calls,
 * following the same pattern as mod_status.c in this module.
 *
 * Key event listener (ZMK event-manager thread) ONLY writes to an atomic
 * bitmask — zero LVGL calls.  The k_timer callback reads + clears that mask
 * each tick and spawns columns.
 *
 * Layer event listener also runs on the event-manager thread; it only writes
 * to cur_head_color (lv_color_t = 2 bytes, 16-bit write is atomic on Cortex-M).
 *
 * Layout
 * ──────
 * 23 columns × 12 px = 276 px wide, 155 px tall (y=0 → just above battery).
 * Positioned at TOP_LEFT (2, 0) — full-screen background behind all widgets.
 * Created FIRST in zmk_display_status_screen() so LVGL renders it behind
 * every subsequent widget (z-order = child creation order within parent).
 *
 * Each column has 1 head label + RAIN_TRAIL_LEN trail labels.
 * Total: 23 × 6 = 138 lv_label objects.
 * Uses lv_label_set_text_static with static char arrays → zero heap pressure
 * during animation.
 *
 * Trail
 * ─────
 * 5 trail labels per column, each RAIN_CHAR_H behind the previous.
 * Opacity decays: 180 → 100 → 51 → 20 → 8.
 * Every 3 ticks: trail chars rotate (trail[n] ← trail[n-1], trail[0] ← old
 * head char, head gets new random char) → cascading organic flicker effect.
 *
 * Behaviour
 * ─────────
 * • Key press  → sets bit(s) in spawn_pending; k_timer picks them up next tick.
 * • Active column → ignored on new keypress (previous drop finishes naturally).
 * • No keypresses → active drops finish their fall, zone goes dark.
 * • Head color tracks the active layer accent (base = #00FF41 Matrix green).
 * • Trail uses same color as head, dimmed via per-label text opacity.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/random/random.h>
#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/display.h>
#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/keymap.h>

#include "matrix_rain.h"

/* ── Geometry ────────────────────────────────────────────────────────── */

#define RAIN_COLS      23    /* 23 × 12 px = 276 px wide                  */
#define RAIN_CHAR_W    12    /* column pitch in px                         */
#define RAIN_CHAR_H    20    /* line height in px (Montserrat 12 + spacing)*/
#define RAIN_ZONE_H   155    /* widget height — stops just above battery   */
#define RAIN_TRAIL_LEN  5    /* trail labels per column                    */
#define RAIN_TICK_MS   80    /* k_timer period in ms (~12 fps)             */

/* Opacity for each trail level; index 0 = closest to head */
static const lv_opa_t trail_opa[RAIN_TRAIL_LEN] = {180, 100, 51, 20, 8};

/* ── Layer colour palette (mirrors layer_status.c) ──────────────────── */

static const lv_color_t layer_colors[] = {
    LV_COLOR_MAKE(0x00, 0xFF, 0x41), /* 0: Matrix green (base layer)  */
    LV_COLOR_MAKE(0x00, 0xFF, 0xFF), /* 1: Cyan                       */
    LV_COLOR_MAKE(0xFF, 0xFF, 0x00), /* 2: Yellow                     */
    LV_COLOR_MAKE(0xFF, 0x80, 0x00), /* 3: Orange                     */
    LV_COLOR_MAKE(0xFF, 0x00, 0xFF), /* 4: Magenta                    */
    LV_COLOR_MAKE(0x00, 0xFF, 0x00), /* 5: Green                      */
    LV_COLOR_MAKE(0xFF, 0x40, 0x40), /* 6: Red                        */
    LV_COLOR_MAKE(0x80, 0x80, 0xFF), /* 7: Purple                     */
};
#define LAYER_COLORS_COUNT ARRAY_SIZE(layer_colors)

/* ── Character set ───────────────────────────────────────────────────── */

static const char rain_chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!@#%&*=+?";
#define RAIN_CHARS_LEN (sizeof(rain_chars) - 1u)

/* ── PRNG (xorshift32 — ISR-safe, no blocking) ───────────────────────── */

static uint32_t prng_state = 0xDEADBEEFu;

static uint32_t fast_rand(void)
{
    prng_state ^= prng_state << 13;
    prng_state ^= prng_state >> 17;
    prng_state ^= prng_state << 5;
    return prng_state;
}

static char rand_char(void)
{
    return rain_chars[fast_rand() % RAIN_CHARS_LEN];
}

/* ── Per-column state ────────────────────────────────────────────────── */

typedef struct {
    int16_t head_y;    /* head top edge, px relative to widget top */
    uint8_t tick_mod;  /* ticks since last char rotation           */
    uint8_t speed;     /* px advanced per tick (1–3)               */
    bool    active;
} rain_col_t;

static rain_col_t  cols[RAIN_COLS];
static lv_obj_t   *head_lbl[RAIN_COLS];
static lv_obj_t   *trail_lbl[RAIN_COLS][RAIN_TRAIL_LEN];

/* Static 1-char buffers — lv_label_set_text_static stores a pointer,
 * no heap allocation on update → zero LVGL heap pressure during animation. */
static char head_char[RAIN_COLS][2];
static char trail_char[RAIN_COLS][RAIN_TRAIL_LEN][2];

/* Atomic bitmask: bit i means column i should spawn on the next timer tick.
 * Written only by key_listener (event thread), read+cleared by k_timer cb. */
static ATOMIC_DEFINE(spawn_pending, RAIN_COLS);

/* Current head colour — written by layer_listener, read by k_timer cb.
 * 2-byte write is atomic on Cortex-M4; no lock needed. */
static lv_color_t cur_head_color;

/* ── Colour helper ───────────────────────────────────────────────────── */

static void apply_layer_color(uint8_t layer)
{
    cur_head_color = layer_colors[layer % LAYER_COLORS_COUNT];
}

/* ── Column lifecycle (called only from k_timer cb) ─────────────────── */

static void spawn_col(int i)
{
    if (cols[i].active) return; /* previous drop still running — ignore */

    cols[i].head_y   = 0;
    cols[i].speed    = (uint8_t)(1u + fast_rand() % 3u);
    cols[i].tick_mod = 0;
    cols[i].active   = true;

    head_char[i][0] = rand_char();
    head_char[i][1] = '\0';
    for (int t = 0; t < RAIN_TRAIL_LEN; t++) {
        trail_char[i][t][0] = rand_char();
        trail_char[i][t][1] = '\0';
    }

    /* Apply current layer colour to head and all trail labels */
    lv_obj_set_style_text_color(head_lbl[i], cur_head_color, 0);
    lv_label_set_text_static(head_lbl[i], head_char[i]);
    lv_obj_set_pos(head_lbl[i], i * RAIN_CHAR_W, 0);
    lv_obj_clear_flag(head_lbl[i], LV_OBJ_FLAG_HIDDEN);

    for (int t = 0; t < RAIN_TRAIL_LEN; t++) {
        lv_obj_set_style_text_color(trail_lbl[i][t], cur_head_color, 0);
        lv_label_set_text_static(trail_lbl[i][t], trail_char[i][t]);
        lv_obj_add_flag(trail_lbl[i][t], LV_OBJ_FLAG_HIDDEN);
    }
}

static void tick_col(int i)
{
    if (!cols[i].active) return;

    cols[i].head_y += cols[i].speed;
    lv_obj_set_y(head_lbl[i], cols[i].head_y);

    /* Every 3 ticks: rotate trail chars and randomise head */
    if (++cols[i].tick_mod >= 3u) {
        cols[i].tick_mod = 0;

        /* Shift: trail[n] ← trail[n-1], trail[0] ← old head char */
        for (int t = RAIN_TRAIL_LEN - 1; t > 0; t--) {
            trail_char[i][t][0] = trail_char[i][t - 1][0];
            lv_label_set_text_static(trail_lbl[i][t], trail_char[i][t]);
        }
        trail_char[i][0][0] = head_char[i][0];
        lv_label_set_text_static(trail_lbl[i][0], trail_char[i][0]);

        /* New head char */
        head_char[i][0] = rand_char();
        lv_label_set_text_static(head_lbl[i], head_char[i]);
    }

    /* Position trail labels relative to head; hide those above widget top */
    for (int t = 0; t < RAIN_TRAIL_LEN; t++) {
        int16_t ty = cols[i].head_y - (int16_t)((t + 1) * RAIN_CHAR_H);
        if (ty >= 0) {
            lv_obj_set_pos(trail_lbl[i][t], i * RAIN_CHAR_W, ty);
            lv_obj_clear_flag(trail_lbl[i][t], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(trail_lbl[i][t], LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* Deactivate once head has fully exited the zone bottom */
    if (cols[i].head_y >= RAIN_ZONE_H) {
        cols[i].active = false;
        lv_obj_add_flag(head_lbl[i], LV_OBJ_FLAG_HIDDEN);
        for (int t = 0; t < RAIN_TRAIL_LEN; t++) {
            lv_obj_add_flag(trail_lbl[i][t], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/* ── k_timer callback — same pattern as mod_status.c ────────────────── */

static void rain_timer_cb(struct k_timer *timer)
{
    ARG_UNUSED(timer);

    /* 1. Spawn columns requested by key events (atomic read + clear) */
    for (int i = 0; i < RAIN_COLS; i++) {
        if (atomic_test_and_clear_bit(spawn_pending, i)) {
            spawn_col(i);
        }
    }

    /* 2. Advance all active columns */
    for (int i = 0; i < RAIN_COLS; i++) {
        tick_col(i);
    }
}

static K_TIMER_DEFINE(rain_timer, rain_timer_cb, NULL);

/* ── Event listeners ─────────────────────────────────────────────────── */

static int key_listener(const zmk_event_t *eh)
{
    const struct zmk_keycode_state_changed *ev = as_zmk_keycode_state_changed(eh);
    if (!ev || !ev->state) return ZMK_EV_EVENT_BUBBLE; /* key-down only */

    /* Mark up to 2 random columns for spawning on the next timer tick.
     * NO LVGL calls here — the k_timer callback owns all rendering. */
    int marked = 0, attempts = 0;
    while (marked < 2 && attempts < 16) {
        int col = (int)(sys_rand32_get() % (uint32_t)RAIN_COLS);
        if (!atomic_test_and_set_bit(spawn_pending, col)) {
            marked++;
        }
        attempts++;
    }
    return ZMK_EV_EVENT_BUBBLE;
}

static int layer_listener(const zmk_event_t *eh)
{
    ARG_UNUSED(eh);
    /* Update colour variable only — no LVGL calls. */
    apply_layer_color(zmk_keymap_highest_layer_active());
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(matrix_rain_key,   key_listener);
ZMK_LISTENER(matrix_rain_layer, layer_listener);
ZMK_SUBSCRIPTION(matrix_rain_key,   zmk_keycode_state_changed);
ZMK_SUBSCRIPTION(matrix_rain_layer, zmk_layer_state_changed);

/* ── Widget init ─────────────────────────────────────────────────────── */

int zmk_widget_matrix_rain_init(struct zmk_widget_matrix_rain *widget,
                                lv_obj_t *parent)
{
    widget->obj = lv_obj_create(parent);
    lv_obj_set_size(widget->obj, RAIN_COLS * RAIN_CHAR_W, RAIN_ZONE_H);
    lv_obj_set_style_bg_opa(widget->obj,       LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(widget->obj, 0,             LV_PART_MAIN);
    lv_obj_set_style_pad_all(widget->obj,      0,             LV_PART_MAIN);
    lv_obj_clear_flag(widget->obj, LV_OBJ_FLAG_SCROLLABLE);

    /* Seed PRNG from hardware entropy (called once at init, not from ISR) */
    prng_state = sys_rand32_get();
    if (prng_state == 0u) prng_state = 0xDEADBEEFu;

    apply_layer_color(0);

    for (int i = 0; i < RAIN_COLS; i++) {
        head_char[i][0] = 'A';
        head_char[i][1] = '\0';

        head_lbl[i] = lv_label_create(widget->obj);
        /* Set HIDDEN first to prevent invalidation flicker during init */
        lv_obj_add_flag(head_lbl[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_text_color(head_lbl[i], cur_head_color, 0);
        lv_obj_set_pos(head_lbl[i], i * RAIN_CHAR_W, 0);
        lv_label_set_text_static(head_lbl[i], head_char[i]);

        for (int t = 0; t < RAIN_TRAIL_LEN; t++) {
            trail_char[i][t][0] = 'A';
            trail_char[i][t][1] = '\0';

            trail_lbl[i][t] = lv_label_create(widget->obj);
            /* Set HIDDEN first — avoids 552 visible-object invalidations */
            lv_obj_add_flag(trail_lbl[i][t], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_text_color(trail_lbl[i][t], cur_head_color, 0);
            lv_obj_set_style_text_opa(trail_lbl[i][t],   trail_opa[t],   0);
            lv_obj_set_pos(trail_lbl[i][t], i * RAIN_CHAR_W, 0);
            lv_label_set_text_static(trail_lbl[i][t], trail_char[i][t]);
        }

        cols[i].active = false;
    }

    /* k_timer fires independently of the LVGL render loop, same as mod_status.c */
    k_timer_start(&rain_timer, K_MSEC(RAIN_TICK_MS), K_MSEC(RAIN_TICK_MS));

    return 0;
}

lv_obj_t *zmk_widget_matrix_rain_obj(struct zmk_widget_matrix_rain *widget)
{
    return widget->obj;
}

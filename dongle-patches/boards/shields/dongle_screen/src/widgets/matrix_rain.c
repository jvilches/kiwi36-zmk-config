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
 * lv_timer callbacks fire inside lv_task_handler() which runs on the display
 * thread — fully safe for all lv_* calls.
 *
 * Key event listener (ZMK event-manager thread) only writes to an atomic
 * bitmask.  The lv_timer callback (display thread) reads that mask each tick
 * and spawns columns.
 *
 * Layer event listener also runs on the event-manager thread; it only writes
 * to cur_head_color (lv_color_t = 2 bytes, 16-bit write is atomic on Cortex-M).
 *
 * Layout
 * ──────
 * 23 columns × 12 px = 276 px wide, 155 px tall (y=0 → just above battery).
 * Positioned at TOP_LEFT (2, 0) — full-screen background behind all widgets.
 * Each column has 1 head label + RAIN_TRAIL_LEN trail labels (138 lv_label
 * objects total; uses lv_label_set_text_static with static char arrays to
 * avoid any lv_mem_alloc pressure during animation).
 *
 * Trail
 * ─────
 * 5 trail labels per column, each one character-height behind the previous.
 * Opacity decays from 180 → 100 → 51 → 20 → 8.
 * Every 3 ticks the trail chars rotate: trail[n] ← trail[n-1], trail[0] ← old
 * head char, head gets a new random char → cascading organic flicker effect.
 *
 * Behaviour
 * ─────────
 * • Key press  → sets bit(s) in spawn_pending; lv_timer picks them up next tick.
 * • Active column → ignored on new keypress (previous drop finishes naturally).
 * • No keypresses → active drops finish their fall, zone goes dark.
 * • Head color tracks the active layer accent (base = #00FF41 Matrix green).
 * • Trail uses the same color as head, dimmed via opacity.
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
#define RAIN_CHAR_W    12    /* column pitch in px (≈ Montserrat 20 char)  */
#define RAIN_CHAR_H    20    /* line height in px  (Montserrat 20)         */
#define RAIN_ZONE_H   155    /* widget height — stops just above battery   */
#define RAIN_TRAIL_LEN  5    /* trail labels per column                    */
#define RAIN_TICK_MS   80    /* lv_timer period in ms (~12 fps)            */

/* Opacity for each trail level, index 0 = closest to head */
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

/* ── PRNG (xorshift32) ───────────────────────────────────────────────── */

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

/* Static char buffers — lv_label_set_text_static stores a pointer,
 * no lv_mem_alloc on update → zero heap pressure during animation. */
static char head_char[RAIN_COLS][2];
static char trail_char[RAIN_COLS][RAIN_TRAIL_LEN][2];

/* Atomic bitmask: bit i = column i should spawn on next lv_timer tick.
 * Written by key_listener (event thread), read+cleared by lv_timer. */
static ATOMIC_DEFINE(spawn_pending, RAIN_COLS);

/* Current head colour — written by layer_listener (event thread), read by
 * lv_timer (display thread).  2-byte write is atomic on Cortex-M. */
static lv_color_t cur_head_color;

/* ── Colour helper ───────────────────────────────────────────────────── */

static void apply_layer_color(uint8_t layer)
{
    cur_head_color = layer_colors[layer % LAYER_COLORS_COUNT];
}

/* ── Column lifecycle (called only from lv_timer — display thread) ───── */

static void spawn_col(int i)
{
    if (cols[i].active) return; /* previous drop still running — ignore */

    cols[i].head_y   = 0;
    cols[i].speed    = (uint8_t)(1u + fast_rand() % 3u);
    cols[i].tick_mod = 0;
    cols[i].active   = true;

    /* Seed head and all trail chars with random values */
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

    /* Position trail labels relative to head; hide if above widget top */
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

/* ── lv_timer — runs in display thread, safe for all LVGL calls ───────── */

static void rain_lv_timer_cb(lv_timer_t *timer)
{
    ARG_UNUSED(timer);

    /* 1. Process spawn requests from key events (atomic read + clear) */
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

/* ── Event listeners ─────────────────────────────────────────────────── */

static int key_listener(const zmk_event_t *eh)
{
    const struct zmk_keycode_state_changed *ev = as_zmk_keycode_state_changed(eh);
    if (!ev || !ev->state) return ZMK_EV_EVENT_BUBBLE; /* key-down only */

    /*
     * Mark up to 2 random columns to spawn on the next lv_timer tick.
     * NO LVGL calls here — the lv_timer (display thread) owns all rendering.
     */
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

    /* Seed PRNG from hardware entropy (safe here — init runs in display thread) */
    prng_state = sys_rand32_get();
    if (prng_state == 0) prng_state = 0xDEADBEEFu;

    apply_layer_color(0); /* start with base-layer colour */

    for (int i = 0; i < RAIN_COLS; i++) {
        head_char[i][0] = 'A';
        head_char[i][1] = '\0';

        head_lbl[i] = lv_label_create(widget->obj);
        lv_obj_set_style_text_color(head_lbl[i], cur_head_color, 0);
        lv_obj_set_pos(head_lbl[i], i * RAIN_CHAR_W, 0);
        lv_label_set_text_static(head_lbl[i], head_char[i]);
        lv_obj_add_flag(head_lbl[i], LV_OBJ_FLAG_HIDDEN);

        for (int t = 0; t < RAIN_TRAIL_LEN; t++) {
            trail_char[i][t][0] = 'A';
            trail_char[i][t][1] = '\0';

            trail_lbl[i][t] = lv_label_create(widget->obj);
            lv_obj_set_style_text_color(trail_lbl[i][t], cur_head_color, 0);
            lv_obj_set_style_text_opa(trail_lbl[i][t],   trail_opa[t],   0);
            lv_obj_set_pos(trail_lbl[i][t], i * RAIN_CHAR_W, 0);
            lv_label_set_text_static(trail_lbl[i][t], trail_char[i][t]);
            lv_obj_add_flag(trail_lbl[i][t], LV_OBJ_FLAG_HIDDEN);
        }

        cols[i].active = false;
    }

    /* lv_timer fires inside lv_task_handler() (display thread) — safe for LVGL */
    lv_timer_create(rain_lv_timer_cb, RAIN_TICK_MS, NULL);

    return 0;
}

lv_obj_t *zmk_widget_matrix_rain_obj(struct zmk_widget_matrix_rain *widget)
{
    return widget->obj;
}

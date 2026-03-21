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
 * We follow the mod_status.c pattern: the k_timer callback (ISR) owns all
 * LVGL updates.  The key event listener (ZMK event-manager thread) only
 * writes to an atomic bitmask — zero LVGL calls.  The timer reads that mask
 * each tick and does the actual spawn + tick rendering.
 *
 * Layout
 * ──────
 * 8 columns × 12 px = 96 px wide, 55 px tall.
 * Positioned at TOP_LEFT (20, 45) — fills the gap between the WPM row and
 * the layer name.  Each column has one head label and one trail label (16
 * lv_label objects total; no canvas buffer to avoid exhausting the pool).
 *
 * Behaviour
 * ─────────
 * • Key press  → sets bit(s) in spawn_pending; timer picks them up next tick.
 * • Active column → ignored on new keypress (previous drop finishes).
 * • No keypresses → active drops finish their fall, zone goes dark.
 * • Head color tracks the active layer accent (base = #00FF41 Matrix green).
 * • Trail color = head darkened to ~30 % brightness.
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

#define RAIN_COLS      8     /* number of columns                         */
#define RAIN_CHAR_W   12     /* column pitch in px (≈ Montserrat 20 char) */
#define RAIN_CHAR_H   20     /* line height in px  (Montserrat 20)        */
#define RAIN_ZONE_H   55     /* widget height in px                       */
#define RAIN_TICK_MS  80     /* timer period in ms (~12 fps)              */

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

/* ── ISR-safe PRNG (xorshift32) ─────────────────────────────────────── */
/*
 * sys_rand32_get() may block when backed by a hardware entropy source.
 * This lightweight PRNG is safe to call from the timer ISR.
 */
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
    uint8_t tick_mod;  /* ticks since last char randomisation       */
    uint8_t speed;     /* px advanced per tick (1–3)                */
    bool    active;
} rain_col_t;

static rain_col_t  cols[RAIN_COLS];
static lv_obj_t   *head_lbl[RAIN_COLS];
static lv_obj_t   *trail_lbl[RAIN_COLS];

/* Atomic bitmask: bit i set = column i should spawn on next timer tick.
 * Written by key_listener (event thread), read+cleared by timer ISR. */
static ATOMIC_DEFINE(spawn_pending, RAIN_COLS);

/* Current colours — written by layer_listener (event thread), read by
 * timer ISR.  lv_color_t is 2 bytes; 16-bit reads on Cortex-M are atomic. */
static lv_color_t cur_head_color;
static lv_color_t cur_trail_color;

/* ── Colour helpers ──────────────────────────────────────────────────── */

static lv_color_t darken(lv_color_t c)
{
    /* lv_color_mix(c1, c2, ratio): ratio 255→c1, 0→c2; 77/255 ≈ 30 % */
    return lv_color_mix(c, lv_color_black(), 77);
}

static void apply_layer_color(uint8_t layer)
{
    cur_head_color  = layer_colors[layer % LAYER_COLORS_COUNT];
    cur_trail_color = darken(cur_head_color);
}

/* ── Column lifecycle (called only from timer ISR) ───────────────────── */

static void spawn_col(int i)
{
    if (cols[i].active) return; /* previous drop still running — ignore */

    cols[i].head_y   = 0;
    cols[i].speed    = (uint8_t)(1u + fast_rand() % 3u); /* 1, 2, or 3  */
    cols[i].tick_mod = 0;
    cols[i].active   = true;

    char buf[2] = { rand_char(), '\0' };

    lv_obj_set_style_text_color(head_lbl[i],  cur_head_color,  0);
    lv_obj_set_style_text_color(trail_lbl[i], cur_trail_color, 0);
    lv_label_set_text(head_lbl[i],  buf);
    lv_label_set_text(trail_lbl[i], buf);

    lv_obj_set_pos(head_lbl[i], i * RAIN_CHAR_W, 0);
    lv_obj_clear_flag(head_lbl[i],  LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(trail_lbl[i],   LV_OBJ_FLAG_HIDDEN);
}

static void tick_col(int i)
{
    if (!cols[i].active) return;

    cols[i].head_y += cols[i].speed;
    lv_obj_set_y(head_lbl[i], cols[i].head_y);

    /* Randomise head character every 3 ticks */
    if (++cols[i].tick_mod >= 3u) {
        cols[i].tick_mod = 0;
        char buf[2] = { rand_char(), '\0' };
        lv_label_set_text(head_lbl[i], buf);
    }

    /* Show trail once it enters the zone (one char-height behind head) */
    int16_t trail_y = cols[i].head_y - RAIN_CHAR_H;
    if (trail_y >= 0) {
        lv_obj_set_pos(trail_lbl[i], i * RAIN_CHAR_W, trail_y);
        lv_obj_clear_flag(trail_lbl[i], LV_OBJ_FLAG_HIDDEN);
    }

    /* Deactivate once head has fully exited the zone bottom */
    if (cols[i].head_y >= RAIN_ZONE_H) {
        cols[i].active = false;
        lv_obj_add_flag(head_lbl[i],  LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(trail_lbl[i], LV_OBJ_FLAG_HIDDEN);
    }
}

/* ── Timer — single owner of all LVGL calls ─────────────────────────── */

static struct k_timer rain_timer;

static void rain_timer_cb(struct k_timer *timer)
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
     * Mark up to 2 random columns to spawn on the next timer tick.
     * NO LVGL calls here — the timer ISR owns all rendering.
     * atomic_test_and_set_bit returns the previous value; we only count
     * a column if the bit was previously clear (not already pending).
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
    /* Update colour variables only — no LVGL calls. */
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

    /* Seed PRNG with a value from the hardware entropy source */
    prng_state = sys_rand32_get();
    if (prng_state == 0) prng_state = 0xDEADBEEFu; /* xorshift needs non-zero */

    apply_layer_color(0); /* start with base-layer colour */

    for (int i = 0; i < RAIN_COLS; i++) {
        head_lbl[i] = lv_label_create(widget->obj);
        lv_obj_set_style_text_color(head_lbl[i], cur_head_color, 0);
        lv_obj_set_pos(head_lbl[i], i * RAIN_CHAR_W, 0);
        lv_label_set_text(head_lbl[i], "A");
        lv_obj_add_flag(head_lbl[i], LV_OBJ_FLAG_HIDDEN);

        trail_lbl[i] = lv_label_create(widget->obj);
        lv_obj_set_style_text_color(trail_lbl[i], cur_trail_color, 0);
        lv_obj_set_pos(trail_lbl[i], i * RAIN_CHAR_W, 0);
        lv_label_set_text(trail_lbl[i], "A");
        lv_obj_add_flag(trail_lbl[i], LV_OBJ_FLAG_HIDDEN);

        cols[i].active = false;
    }

    k_timer_init(&rain_timer, rain_timer_cb, NULL);
    k_timer_start(&rain_timer, K_MSEC(RAIN_TICK_MS), K_MSEC(RAIN_TICK_MS));

    return 0;
}

lv_obj_t *zmk_widget_matrix_rain_obj(struct zmk_widget_matrix_rain *widget)
{
    return widget->obj;
}

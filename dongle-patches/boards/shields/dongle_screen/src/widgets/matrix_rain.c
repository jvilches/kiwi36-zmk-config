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
 * k_timer callbacks run in ISR context — they CANNOT call LVGL (lv_label_set_text
 * internally calls lv_mem_alloc which uses a mutex; blocking in ISR hangs the system).
 *
 * Correct approach: use lv_timer_create().  LVGL timers fire inside lv_task_handler()
 * which runs on the display thread — fully safe for all lv_* calls.
 *
 * Key event listener (ZMK event-manager thread) only writes to an atomic bitmask.
 * The lv_timer callback (display thread) reads that mask each tick and spawns columns.
 *
 * Layout
 * ──────
 * 16 columns × 12 px = 192 px wide, 55 px tall.
 * Positioned at TOP_LEFT (20, 45) — fills the gap between the WPM row and
 * the layer name.  Each column has one head label and one trail label (32
 * lv_label objects total; no canvas buffer to avoid exhausting the pool).
 * Labels use lv_label_set_text_static with per-column static char arrays to
 * avoid lv_mem_alloc pressure and prevent heap-fragmentation display corruption.
 *
 * Behaviour
 * ─────────
 * • Key press  → sets bit(s) in spawn_pending; lv_timer picks them up next tick.
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

#define RAIN_COLS     16     /* number of columns (16 × 12 px = 192 px)   */
#define RAIN_CHAR_W   12     /* column pitch in px (≈ Montserrat 20 char) */
#define RAIN_CHAR_H   20     /* line height in px  (Montserrat 20)        */
#define RAIN_ZONE_H   55     /* widget height in px                       */
#define RAIN_TICK_MS  80     /* lv_timer period in ms (~12 fps)           */

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
/*
 * Called from the lv_timer callback (display thread) — not ISR.
 * Still using xorshift32 for speed; seeded from hardware entropy at init.
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

/* Static char buffers for lv_label_set_text_static — avoids lv_mem_alloc
 * on every text update, preventing heap fragmentation and display corruption. */
static char head_char[RAIN_COLS][2];
static char trail_char[RAIN_COLS][2];

/* Atomic bitmask: bit i set = column i should spawn on next lv_timer tick.
 * Written by key_listener (event thread), read+cleared by lv_timer (display thread). */
static ATOMIC_DEFINE(spawn_pending, RAIN_COLS);

/* Current colours — written by layer_listener (event thread), read by
 * lv_timer (display thread).  lv_color_t is 2 bytes; 16-bit reads on
 * Cortex-M are atomic so no lock needed. */
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

/* ── Column lifecycle (called only from lv_timer — display thread) ───── */

static void spawn_col(int i)
{
    if (cols[i].active) return; /* previous drop still running — ignore */

    cols[i].head_y   = 0;
    cols[i].speed    = (uint8_t)(1u + fast_rand() % 3u); /* 1, 2, or 3  */
    cols[i].tick_mod = 0;
    cols[i].active   = true;

    head_char[i][0]  = rand_char();
    head_char[i][1]  = '\0';
    trail_char[i][0] = head_char[i][0];
    trail_char[i][1] = '\0';

    lv_obj_set_style_text_color(head_lbl[i],  cur_head_color,  0);
    lv_obj_set_style_text_color(trail_lbl[i], cur_trail_color, 0);
    lv_label_set_text_static(head_lbl[i],  head_char[i]);
    lv_label_set_text_static(trail_lbl[i], trail_char[i]);

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
        head_char[i][0] = rand_char();
        lv_label_set_text_static(head_lbl[i], head_char[i]);
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
     * sys_rand32_get() is safe here (event thread, not ISR).
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

    /* Seed PRNG from hardware entropy (safe here — init runs in display thread) */
    prng_state = sys_rand32_get();
    if (prng_state == 0) prng_state = 0xDEADBEEFu; /* xorshift needs non-zero */

    apply_layer_color(0); /* start with base-layer colour */

    for (int i = 0; i < RAIN_COLS; i++) {
        head_char[i][0]  = 'A';
        head_char[i][1]  = '\0';
        trail_char[i][0] = 'A';
        trail_char[i][1] = '\0';

        head_lbl[i] = lv_label_create(widget->obj);
        lv_obj_set_style_text_color(head_lbl[i], cur_head_color, 0);
        lv_obj_set_pos(head_lbl[i], i * RAIN_CHAR_W, 0);
        lv_label_set_text_static(head_lbl[i], head_char[i]);
        lv_obj_add_flag(head_lbl[i], LV_OBJ_FLAG_HIDDEN);

        trail_lbl[i] = lv_label_create(widget->obj);
        lv_obj_set_style_text_color(trail_lbl[i], cur_trail_color, 0);
        lv_obj_set_pos(trail_lbl[i], i * RAIN_CHAR_W, 0);
        lv_label_set_text_static(trail_lbl[i], trail_char[i]);
        lv_obj_add_flag(trail_lbl[i], LV_OBJ_FLAG_HIDDEN);

        cols[i].active = false;
    }

    /* lv_timer runs inside lv_task_handler() (display thread) — safe for LVGL */
    lv_timer_create(rain_lv_timer_cb, RAIN_TICK_MS, NULL);

    return 0;
}

lv_obj_t *zmk_widget_matrix_rain_obj(struct zmk_widget_matrix_rain *widget)
{
    return widget->obj;
}

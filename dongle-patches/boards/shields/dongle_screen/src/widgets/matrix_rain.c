/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Matrix rain animation widget.
 *
 * Thread-safety model
 * ───────────────────
 * k_timer callback (rain_timer_cb) is the single owner of all LVGL calls,
 * following the same pattern as mod_status.c in this module.
 *
 * key_listener (event-manager thread):
 *   Only writes atomic bits in spawn_pending — zero LVGL calls.
 *
 * layer_listener (event-manager thread):
 *   Only writes volatile cur_layer_color_idx — zero LVGL calls.
 *   uint8_t aligned writes are atomic on Cortex-M4; volatile prevents
 *   compiler from caching values across the timer callback boundary.
 *
 * Layout
 * ──────
 * 23 columns × 12 px = 276 px wide, 155 px tall (y=0 → just above battery).
 * Positioned at TOP_LEFT (2, 0) — full-screen background behind all widgets.
 * Created FIRST in zmk_display_status_screen() — LVGL z-order = child creation
 * order; first child = bottom of stack → all other widgets render on top.
 *
 * Each column: 1 head label + RAIN_TRAIL_LEN trail labels.
 * Total: 23 × 4 = 92 lv_label objects.
 * lv_label_set_text_static with static char arrays → zero heap pressure
 * during animation.  Trail X coordinates fixed at init → lv_obj_set_y only
 * in tick loop (no redundant X updates).
 *
 * Trail
 * ─────
 * 3 trail labels per column, each RAIN_CHAR_H below the previous.
 * Opacity decays: 180 → 80 → 20.
 * Every 3 ticks: trail[n] ← trail[n-1], trail[0] ← old head char,
 * head gets a new random char → cascading organic flicker effect.
 * lv_label_set_text_static always calls lv_obj_invalidate (confirmed in
 * LVGL 8 source), so char updates are visible even with the same pointer.
 *
 * Colour cache
 * ────────────
 * lv_obj_set_style_text_color modifies LVGL local styles (heap op).  Each
 * column tracks the last colour index applied; spawn_col skips the style
 * call when the layer accent hasn't changed since the column last spawned.
 * At init all labels receive layer_colors[0], col_last_color_idx[] is 0
 * (BSS) → first spawn of each column is always correct.
 *
 * Behaviour
 * ─────────
 * • Key press  → sets bit(s) in spawn_pending; k_timer picks them up.
 * • Active col → new keypress ignored (previous drop finishes naturally).
 * • No keypresses → active drops finish their fall; zone goes dark.
 * • Head color tracks the active layer accent (base = #00FF41 Matrix green).
 * • Trail uses same color as head, dimmed via per-label text opacity.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/random/random.h>
#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

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
#define RAIN_TRAIL_LEN  3    /* trail labels per column (23×4=92 objects)  */
#define RAIN_TICK_MS   80    /* k_timer period in ms (~12 fps)             */

/* Opacity for each trail level; index 0 = closest to head */
static const lv_opa_t trail_opa[RAIN_TRAIL_LEN] = {180, 80, 20};

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

/* ── PRNG (xorshift32 — no blocking, no heap) ────────────────────────── */

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

/* Static 1-char buffers — lv_label_set_text_static stores the pointer.
 * lv_label_set_text_static always calls lv_obj_invalidate unconditionally
 * (confirmed in LVGL 8 source), so the updated char is always re-rendered
 * even when the pointer value doesn't change. */
static char head_char[RAIN_COLS][2];
static char trail_char[RAIN_COLS][RAIN_TRAIL_LEN][2];

/* Atomic bitmask: bit i = column i should spawn on the next timer tick.
 * Written by key_listener (event thread), read+cleared by rain_timer_cb. */
static ATOMIC_DEFINE(spawn_pending, RAIN_COLS);

/* Current layer colour index — volatile uint8_t: written by layer_listener,
 * read by rain_timer_cb.  uint8_t write is atomic on Cortex-M4; volatile
 * prevents the compiler from caching the value across the work boundary.
 * Avoids comparing lv_color_t internals (struct layout is LVGL-version-specific). */
static volatile uint8_t cur_layer_color_idx = 0;

/* Per-column colour index cache — avoids redundant lv_obj_set_style_text_color
 * calls (heap ops) when the layer accent hasn't changed since last spawn.
 * Initialised to 0 to match layer_colors[0] applied to all labels at init. */
static uint8_t col_last_color_idx[RAIN_COLS]; /* zero-initialised by BSS */

/* ── Colour helper ───────────────────────────────────────────────────── */

static void apply_layer_color(uint8_t layer)
{
    cur_layer_color_idx = layer % LAYER_COLORS_COUNT; /* volatile uint8_t — atomic write */
}

/* ── Column lifecycle (called only from k_timer callback) ────────────── */

static void spawn_col(int i)
{
    if (cols[i].active) return;

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

    /* Update label colours only when the layer accent changed since last spawn.
     * lv_obj_set_style_text_color modifies LVGL local styles (heap op) — skip
     * when the colour is already correct to reduce LVGL work per spawn.
     * Compare by index (uint8_t) rather than lv_color_t struct internals
     * to stay portable across LVGL versions. */
    uint8_t idx = cur_layer_color_idx; /* snapshot volatile uint8_t once */
    if (idx != col_last_color_idx[i]) {
        lv_color_t color = layer_colors[idx];
        lv_obj_set_style_text_color(head_lbl[i], color, 0);
        for (int t = 0; t < RAIN_TRAIL_LEN; t++) {
            lv_obj_set_style_text_color(trail_lbl[i][t], color, 0);
        }
        col_last_color_idx[i] = idx;
    }

    /* Reset head to top; X was set at init and never changes */
    lv_obj_set_y(head_lbl[i], 0);
    lv_label_set_text_static(head_lbl[i], head_char[i]);
    lv_obj_clear_flag(head_lbl[i], LV_OBJ_FLAG_HIDDEN);

    /* Trail starts hidden; tick_col will reveal them as head descends */
    for (int t = 0; t < RAIN_TRAIL_LEN; t++) {
        lv_label_set_text_static(trail_lbl[i][t], trail_char[i][t]);
        lv_obj_add_flag(trail_lbl[i][t], LV_OBJ_FLAG_HIDDEN);
    }
}

static void tick_col(int i)
{
    if (!cols[i].active) return;

    cols[i].head_y += cols[i].speed;

    /* Deactivate BEFORE updating position — head never moves beyond RAIN_ZONE_H */
    if (cols[i].head_y >= RAIN_ZONE_H) {
        cols[i].active = false;
        lv_obj_add_flag(head_lbl[i], LV_OBJ_FLAG_HIDDEN);
        for (int t = 0; t < RAIN_TRAIL_LEN; t++) {
            lv_obj_add_flag(trail_lbl[i][t], LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

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

        head_char[i][0] = rand_char();
        lv_label_set_text_static(head_lbl[i], head_char[i]);
    }

    /* Position trail labels; X is fixed — use lv_obj_set_y only */
    for (int t = 0; t < RAIN_TRAIL_LEN; t++) {
        int16_t ty = cols[i].head_y - (int16_t)((t + 1) * RAIN_CHAR_H);
        if (ty >= 0) {
            lv_obj_set_y(trail_lbl[i][t], ty);
            lv_obj_clear_flag(trail_lbl[i][t], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(trail_lbl[i][t], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/* ── k_timer callback — same pattern as mod_status.c ────────────────── */

static void rain_timer_cb(struct k_timer *timer)
{
    ARG_UNUSED(timer);

    /* Spawn columns requested by key events (atomic read + clear) */
    for (int i = 0; i < RAIN_COLS; i++) {
        if (atomic_test_and_clear_bit(spawn_pending, i)) {
            spawn_col(i);
        }
    }

    /* Advance all active columns */
    for (int i = 0; i < RAIN_COLS; i++) {
        tick_col(i);
    }
}

static K_TIMER_DEFINE(rain_timer, rain_timer_cb, NULL);

/* ── Event listeners ─────────────────────────────────────────────────── */

static int key_listener(const zmk_event_t *eh)
{
    const struct zmk_keycode_state_changed *ev = as_zmk_keycode_state_changed(eh);
    if (!ev || !ev->state) return ZMK_EV_EVENT_BUBBLE;

    /* Mark up to 2 random columns for spawning.
     * Uses fast_rand (xorshift32) — no blocking, no heap, ISR/thread safe.
     * Zero LVGL calls; rain_timer_cb (k_timer) owns all rendering. */
    int marked = 0, attempts = 0;
    while (marked < 2 && attempts < 16) {
        int col = (int)(fast_rand() % (uint32_t)RAIN_COLS);
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
    /* volatile write only — zero LVGL calls. */
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

    /* Seed PRNG with hardware entropy (init runs in display work queue thread
     * via initialize_display() → zmk_display_status_screen() — not ISR). */
    prng_state = sys_rand32_get();
    if (prng_state == 0u) prng_state = 0xDEADBEEFu;

    /* Initialise colour state to layer 0.
     * col_last_color_idx[] is zero-initialised by BSS — already matches idx=0.
     * All labels receive layer_colors[0] below, keeping the cache consistent. */
    apply_layer_color(0);
    lv_color_t init_color = layer_colors[0];

    for (int i = 0; i < RAIN_COLS; i++) {
        head_char[i][0] = 'A';
        head_char[i][1] = '\0';

        head_lbl[i] = lv_label_create(widget->obj);
        lv_obj_add_flag(head_lbl[i], LV_OBJ_FLAG_HIDDEN); /* hide first */
        lv_obj_set_style_text_color(head_lbl[i], init_color, 0);
        /* X is fixed for the lifetime of this column — set once here */
        lv_obj_set_pos(head_lbl[i], i * RAIN_CHAR_W, 0);
        lv_label_set_text_static(head_lbl[i], head_char[i]);

        for (int t = 0; t < RAIN_TRAIL_LEN; t++) {
            trail_char[i][t][0] = 'A';
            trail_char[i][t][1] = '\0';

            trail_lbl[i][t] = lv_label_create(widget->obj);
            lv_obj_add_flag(trail_lbl[i][t], LV_OBJ_FLAG_HIDDEN); /* hide first */
            lv_obj_set_style_text_color(trail_lbl[i][t], init_color, 0);
            lv_obj_set_style_text_opa(trail_lbl[i][t],   trail_opa[t], 0);
            /* X is fixed — set once here; tick_col uses lv_obj_set_y only */
            lv_obj_set_pos(trail_lbl[i][t], i * RAIN_CHAR_W, 0);
            lv_label_set_text_static(trail_lbl[i][t], trail_char[i][t]);
        }

        cols[i].active = false;
    }

    k_timer_start(&rain_timer, K_MSEC(RAIN_TICK_MS), K_MSEC(RAIN_TICK_MS));

    return 0;
}

lv_obj_t *zmk_widget_matrix_rain_obj(struct zmk_widget_matrix_rain *widget)
{
    return widget->obj;
}

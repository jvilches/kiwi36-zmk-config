/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Matrix rain animation widget.
 *
 * Fills the vertical gap between the WPM number and the layer name
 * (x=20–116, y=45–100 on screen) with falling single-character "drops".
 *
 * Behaviour:
 *   - Each key press spawns up to 2 drops in random inactive columns.
 *   - Active columns finish their fall then go dark (no looping).
 *   - If a key hits an already-active column it is ignored.
 *   - Head color tracks the active layer accent; trail is 30% brightness.
 *   - Base layer (0) uses classic Matrix green #00FF41 instead of white.
 *
 * Rendering: one head label + one trail label per column (16 lv_label
 * objects total).  No canvas buffer — avoids consuming the LVGL memory pool.
 *
 * Timer: k_timer at 80 ms (~12 fps), same pattern as mod_status.c.
 */

#include <zephyr/kernel.h>
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
#define RAIN_CHAR_W   12     /* column pitch in px (≈ Montserrat 20 width)*/
#define RAIN_CHAR_H   20     /* line height in px  (Montserrat 20)        */
#define RAIN_ZONE_H   55     /* widget height in px                       */
#define RAIN_TICK_MS  80     /* timer period in ms                        */

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
static lv_color_t  cur_head_color;
static lv_color_t  cur_trail_color;

/* ── Colour helpers ──────────────────────────────────────────────────── */

/* Darken a colour to ~30 % brightness by mixing with black. */
static lv_color_t darken(lv_color_t c)
{
    /* lv_color_mix(c1, c2, ratio): ratio=255 → c1, ratio=0 → c2 */
    return lv_color_mix(c, lv_color_black(), 77); /* 77/255 ≈ 0.30 */
}

static void apply_layer_color(uint8_t layer)
{
    cur_head_color  = layer_colors[layer % LAYER_COLORS_COUNT];
    cur_trail_color = darken(cur_head_color);
}

/* ── Character helper ────────────────────────────────────────────────── */

static char rand_char(void)
{
    return rain_chars[sys_rand32_get() % RAIN_CHARS_LEN];
}

/* ── Column lifecycle ────────────────────────────────────────────────── */

static void spawn_col(int i)
{
    if (cols[i].active) return; /* let the current drop finish */

    cols[i].head_y   = 0;
    cols[i].speed    = 1u + (uint8_t)(sys_rand32_get() % 3u); /* 1, 2, or 3 */
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

    /* Show trail once it has entered the zone (one char-height behind head) */
    int16_t trail_y = cols[i].head_y - RAIN_CHAR_H;
    if (trail_y >= 0) {
        lv_obj_set_pos(trail_lbl[i], i * RAIN_CHAR_W, trail_y);
        lv_obj_clear_flag(trail_lbl[i], LV_OBJ_FLAG_HIDDEN);
    }

    /* Deactivate once head has fully exited the bottom of the zone */
    if (cols[i].head_y >= RAIN_ZONE_H) {
        cols[i].active = false;
        lv_obj_add_flag(head_lbl[i],  LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(trail_lbl[i], LV_OBJ_FLAG_HIDDEN);
    }
}

/* ── Timer (80 ms, same approach as mod_status.c) ───────────────────── */

static struct k_timer rain_timer;

static void rain_timer_cb(struct k_timer *timer)
{
    ARG_UNUSED(timer);
    for (int i = 0; i < RAIN_COLS; i++) {
        tick_col(i);
    }
}

/* ── Event listeners ─────────────────────────────────────────────────── */

static int key_listener(const zmk_event_t *eh)
{
    const struct zmk_keycode_state_changed *ev = as_zmk_keycode_state_changed(eh);
    if (!ev || !ev->state) return ZMK_EV_EVENT_BUBBLE; /* key-down only */

    /* Try to spawn up to 2 drops; skip already-active columns */
    int spawned = 0, attempts = 0;
    while (spawned < 2 && attempts < 16) {
        int col = (int)(sys_rand32_get() % (uint32_t)RAIN_COLS);
        if (!cols[col].active) {
            spawn_col(col);
            spawned++;
        }
        attempts++;
    }
    return ZMK_EV_EVENT_BUBBLE;
}

static int layer_listener(const zmk_event_t *eh)
{
    ARG_UNUSED(eh);
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
    lv_obj_set_style_bg_opa(widget->obj,     LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(widget->obj, 0,           LV_PART_MAIN);
    lv_obj_set_style_pad_all(widget->obj,    0,             LV_PART_MAIN);
    lv_obj_clear_flag(widget->obj, LV_OBJ_FLAG_SCROLLABLE);

    apply_layer_color(0); /* start with base-layer colour */

    for (int i = 0; i < RAIN_COLS; i++) {
        /* Head label — bright, falls first */
        head_lbl[i] = lv_label_create(widget->obj);
        lv_obj_set_style_text_color(head_lbl[i], cur_head_color, 0);
        lv_obj_set_pos(head_lbl[i], i * RAIN_CHAR_W, 0);
        lv_label_set_text(head_lbl[i], "A");
        lv_obj_add_flag(head_lbl[i], LV_OBJ_FLAG_HIDDEN);

        /* Trail label — dim, one char-height behind head */
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

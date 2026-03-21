/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Peripheral connection dots widget.
 *
 * Displays one small colored circle per split peripheral, positioned in the
 * center of the top info band (between the WPM number and the USB/BLE text).
 *
 *   Connected    → bright green filled circle
 *   Disconnected → dark gray filled circle
 *
 * Connection state is inferred from battery events: level >= 1 means the
 * peripheral is connected and reporting, level < 1 means disconnected.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/display.h>
#include <zmk/event_manager.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/split/central.h>

#include "peripheral_status.h"

// Dot geometry
#define DOT_SIZE   14  // px — diameter of each circle
#define DOT_GAP     5  // px — horizontal gap between dots
#define DOT_COUNT  ZMK_SPLIT_CENTRAL_PERIPHERAL_COUNT

// Colors
#define COLOR_CONNECTED    lv_palette_main(LV_PALETTE_GREEN)
#define COLOR_DISCONNECTED lv_color_hex(0x2A2A2A)

static sys_slist_t widgets = SYS_SLIST_STATIC_INIT(&widgets);

// Static dot object array — same pattern as battery_objects[] in battery_status.c
static lv_obj_t *peripheral_dots[DOT_COUNT];

// Track last known battery level per peripheral
static int8_t last_levels[DOT_COUNT];

struct peripheral_event_state
{
    uint8_t source; // peripheral index (0-based)
    uint8_t level;  // battery level; < 1 means disconnected
};

/* ------------------------------------------------------------------ */
/* Update helpers                                                       */
/* ------------------------------------------------------------------ */

static void update_dot(lv_obj_t *dot, uint8_t level)
{
    lv_color_t color = (level >= 1) ? COLOR_CONNECTED : COLOR_DISCONNECTED;
    lv_obj_set_style_bg_color(dot, color, LV_PART_MAIN);
}

static void peripheral_status_update_cb(struct peripheral_event_state state)
{
    if (state.source >= DOT_COUNT)
        return;

    last_levels[state.source] = (int8_t)state.level;

    if (peripheral_dots[state.source] != NULL)
        update_dot(peripheral_dots[state.source], state.level);
}

static struct peripheral_event_state peripheral_status_get_state(const zmk_event_t *eh)
{
    const struct zmk_peripheral_battery_state_changed *ev =
        as_zmk_peripheral_battery_state_changed(eh);
    return (struct peripheral_event_state){
        .source = ev->source,
        .level  = ev->state_of_charge,
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_peripheral_status, struct peripheral_event_state,
                            peripheral_status_update_cb, peripheral_status_get_state)

ZMK_SUBSCRIPTION(widget_peripheral_status, zmk_peripheral_battery_state_changed);

/* ------------------------------------------------------------------ */
/* Init                                                                 */
/* ------------------------------------------------------------------ */

int zmk_widget_peripheral_status_init(struct zmk_widget_peripheral_status *widget,
                                      lv_obj_t *parent)
{
    int total_w = DOT_COUNT * DOT_SIZE + (DOT_COUNT - 1) * DOT_GAP;

    widget->obj = lv_obj_create(parent);
    lv_obj_set_size(widget->obj, total_w, DOT_SIZE);
    lv_obj_set_style_bg_opa(widget->obj, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(widget->obj, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(widget->obj, 0, LV_PART_MAIN);
    lv_obj_clear_flag(widget->obj, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < DOT_COUNT; i++)
    {
        lv_obj_t *dot = lv_obj_create(widget->obj);
        lv_obj_set_size(dot, DOT_SIZE, DOT_SIZE);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_border_width(dot, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(dot, 0, LV_PART_MAIN);
        lv_obj_set_style_bg_color(dot, COLOR_DISCONNECTED, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_align(dot, LV_ALIGN_LEFT_MID, i * (DOT_SIZE + DOT_GAP), 0);

        peripheral_dots[i] = dot;
        last_levels[i] = 0;
    }

    sys_slist_append(&widgets, &widget->node);

    widget_peripheral_status_init();
    return 0;
}

lv_obj_t *zmk_widget_peripheral_status_obj(struct zmk_widget_peripheral_status *widget)
{
    return widget->obj;
}

/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/display.h>
#include <zmk/display/widgets/layer_status.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/event_manager.h>
#include <zmk/endpoints.h>
#include <zmk/keymap.h>

static sys_slist_t widgets = SYS_SLIST_STATIC_INIT(&widgets);

// One distinct color per layer (cycles if more layers than entries)
static const lv_color_t layer_colors[] = {
    LV_COLOR_MAKE(0xFF, 0xFF, 0xFF), // 0: White  — Base
    LV_COLOR_MAKE(0x00, 0xFF, 0xFF), // 1: Cyan   — Nav / Arrows
    LV_COLOR_MAKE(0xFF, 0xFF, 0x00), // 2: Yellow — Numbers / Symbols
    LV_COLOR_MAKE(0xFF, 0x80, 0x00), // 3: Orange — Function keys
    LV_COLOR_MAKE(0xFF, 0x00, 0xFF), // 4: Magenta — Media / RGB
    LV_COLOR_MAKE(0x00, 0xFF, 0x00), // 5: Green  — Game / extra
    LV_COLOR_MAKE(0xFF, 0x40, 0x40), // 6: Red    — System / reset
    LV_COLOR_MAKE(0x80, 0x80, 0xFF), // 7: Purple — Misc
};
#define LAYER_COLORS_COUNT ARRAY_SIZE(layer_colors)

struct layer_status_state
{
    uint8_t index;
    const char *label;
};

static void set_layer_symbol(lv_obj_t *label, struct layer_status_state state)
{
    // Apply a distinct color for each layer index
    lv_color_t color = layer_colors[state.index % LAYER_COLORS_COUNT];
    lv_obj_set_style_text_color(label, color, 0);

    if (state.label == NULL)
    {
        char text[7] = {};

        sprintf(text, "%i", state.index);

        lv_label_set_text(label, text);
    }
    else
    {
        char text[13] = {};

        snprintf(text, sizeof(text), "%s", state.label);

        lv_label_set_text(label, text);
    }
}

static void layer_status_update_cb(struct layer_status_state state)
{
    struct zmk_widget_layer_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) { set_layer_symbol(widget->obj, state); }
}

static struct layer_status_state layer_status_get_state(const zmk_event_t *eh)
{
    uint8_t index = zmk_keymap_highest_layer_active();
    return (struct layer_status_state){
        .index = index,
        .label = zmk_keymap_layer_name(index)};
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_layer_status, struct layer_status_state, layer_status_update_cb,
                            layer_status_get_state)

ZMK_SUBSCRIPTION(widget_layer_status, zmk_layer_state_changed);

int zmk_widget_layer_status_init(struct zmk_widget_layer_status *widget, lv_obj_t *parent)
{
    widget->obj = lv_label_create(parent);

    lv_obj_set_style_text_font(widget->obj, &lv_font_montserrat_40, 0);

    sys_slist_append(&widgets, &widget->node);

    widget_layer_status_init();
    return 0;
}

lv_obj_t *zmk_widget_layer_status_obj(struct zmk_widget_layer_status *widget)
{
    return widget->obj;
}
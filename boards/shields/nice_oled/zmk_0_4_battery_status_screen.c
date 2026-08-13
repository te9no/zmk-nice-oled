/*
 * Copyright (c) 2026 te9no
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <stdio.h>

#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>

#include <lvgl.h>

#include <zmk/display.h>
#include <zmk/event_manager.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/split/central.h>

#define DISPLAY_NODE DT_CHOSEN(zephyr_display)
#define PHYSICAL_WIDTH DT_PROP(DISPLAY_NODE, width)
#define PHYSICAL_HEIGHT DT_PROP(DISPLAY_NODE, height)
#define PORTRAIT_WIDTH PHYSICAL_HEIGHT
#define PORTRAIT_HEIGHT PHYSICAL_WIDTH

BUILD_ASSERT(PHYSICAL_WIDTH == 128 && PHYSICAL_HEIGHT == 32,
             "The portrait battery screen currently supports 128x32 displays");

struct battery_widget {
    sys_snode_t node;
    lv_obj_t *canvas;
};

struct peripheral_battery_state {
    uint8_t source;
    uint8_t level;
    bool valid;
};

static sys_slist_t widgets = SYS_SLIST_STATIC_INIT(&widgets);
LV_DRAW_BUF_DEFINE_STATIC(battery_draw_buf, PHYSICAL_WIDTH, PHYSICAL_HEIGHT, LV_COLOR_FORMAT_I1);

/* Digits are stored row-first as compact 3x5 glyphs. */
static const uint8_t digit_rows[10][5] = {
    {0x7, 0x5, 0x5, 0x5, 0x7}, {0x2, 0x6, 0x2, 0x2, 0x7},
    {0x7, 0x1, 0x7, 0x4, 0x7}, {0x7, 0x1, 0x7, 0x1, 0x7},
    {0x5, 0x5, 0x7, 0x1, 0x1}, {0x7, 0x4, 0x7, 0x1, 0x7},
    {0x7, 0x4, 0x7, 0x5, 0x7}, {0x7, 0x1, 0x1, 0x1, 0x1},
    {0x7, 0x5, 0x7, 0x5, 0x7}, {0x7, 0x5, 0x7, 0x1, 0x7},
};

static void set_physical_pixel(lv_obj_t *canvas, int32_t x, int32_t y, bool on) {
    if (x < 0 || x >= PHYSICAL_WIDTH || y < 0 || y >= PHYSICAL_HEIGHT) {
        return;
    }

    lv_canvas_set_px(canvas, x, y, lv_color_hex(on ? 1 : 0), LV_OPA_COVER);
}

static void set_portrait_pixel(lv_obj_t *canvas, int32_t x, int32_t y, bool on) {
    if (x < 0 || x >= PORTRAIT_WIDTH || y < 0 || y >= PORTRAIT_HEIGHT) {
        return;
    }

#if IS_ENABLED(CONFIG_NICE_OLED_ZMK_0_4_ROTATION_270)
    set_physical_pixel(canvas, y, PHYSICAL_HEIGHT - 1 - x, on);
#else
    set_physical_pixel(canvas, PHYSICAL_WIDTH - 1 - y, x, on);
#endif
}

static void fill_portrait_rect(lv_obj_t *canvas, int32_t x, int32_t y, int32_t width,
                               int32_t height, bool on) {
    for (int32_t py = y; py < y + height; py++) {
        for (int32_t px = x; px < x + width; px++) {
            set_portrait_pixel(canvas, px, py, on);
        }
    }
}

static void draw_digit(lv_obj_t *canvas, uint8_t digit, int32_t x, int32_t y, uint8_t scale) {
    if (digit > 9) {
        return;
    }

    for (uint8_t row = 0; row < 5; row++) {
        for (uint8_t col = 0; col < 3; col++) {
            if ((digit_rows[digit][row] & BIT(2 - col)) != 0) {
                fill_portrait_rect(canvas, x + col * scale, y + row * scale, scale, scale, true);
            }
        }
    }
}

static void draw_dash(lv_obj_t *canvas, int32_t x, int32_t y, uint8_t scale) {
    fill_portrait_rect(canvas, x, y + 2 * scale, 3 * scale, scale, true);
}

static void draw_level(lv_obj_t *canvas, const struct peripheral_battery_state *state) {
    if (!state->valid) {
        draw_dash(canvas, 8, 27, 2);
        draw_dash(canvas, 18, 27, 2);
        return;
    }

    uint8_t level = MIN(state->level, 100);
    char text[4];
    int length = snprintf(text, sizeof(text), "%u", level);
    const int32_t glyph_width = 6;
    const int32_t spacing = 2;
    const int32_t text_width = length * glyph_width + (length - 1) * spacing;
    int32_t x = (PORTRAIT_WIDTH - text_width) / 2;

    for (int i = 0; i < length; i++) {
        draw_digit(canvas, text[i] - '0', x, 24, 2);
        x += glyph_width + spacing;
    }
}

static void draw_battery_outline(lv_obj_t *canvas, const struct peripheral_battery_state *state) {
    const int32_t x = 7;
    const int32_t y = 48;
    const int32_t width = 18;
    const int32_t height = 70;

    fill_portrait_rect(canvas, x + 6, y - 4, 6, 4, true);
    fill_portrait_rect(canvas, x, y, width, 2, true);
    fill_portrait_rect(canvas, x, y + height - 2, width, 2, true);
    fill_portrait_rect(canvas, x, y, 2, height, true);
    fill_portrait_rect(canvas, x + width - 2, y, 2, height, true);

    if (!state->valid) {
        return;
    }

    const int32_t inner_height = height - 6;
    int32_t fill_height = DIV_ROUND_CLOSEST(inner_height * MIN(state->level, 100), 100);
    if (fill_height > 0) {
        fill_portrait_rect(canvas, x + 3, y + height - 3 - fill_height, width - 6, fill_height,
                           true);
    }
}

static void draw_source_marker(lv_obj_t *canvas) {
    /* A compact R marks the right-side peripheral shown by Polaris' central half. */
    static const uint8_t rows[7] = {0xE, 0x9, 0x9, 0xE, 0xA, 0x9, 0x9};
    for (uint8_t row = 0; row < 7; row++) {
        for (uint8_t col = 0; col < 4; col++) {
            if ((rows[row] & BIT(3 - col)) != 0) {
                fill_portrait_rect(canvas, 12 + col * 2, 4 + row * 2, 2, 2, true);
            }
        }
    }
}

static void redraw(struct battery_widget *widget, const struct peripheral_battery_state *state) {
    lv_canvas_fill_bg(widget->canvas, lv_color_hex(0), LV_OPA_COVER);
    draw_source_marker(widget->canvas);
    draw_level(widget->canvas, state);
    draw_battery_outline(widget->canvas, state);
    lv_obj_invalidate(widget->canvas);
}

static void battery_status_update_cb(struct peripheral_battery_state state) {
    if (state.source != CONFIG_NICE_OLED_ZMK_0_4_PERIPHERAL_INDEX) {
        return;
    }

    struct battery_widget *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) { redraw(widget, &state); }
}

static struct peripheral_battery_state battery_status_get_state(const zmk_event_t *eh) {
    const struct zmk_peripheral_battery_state_changed *event =
        as_zmk_peripheral_battery_state_changed(eh);

    if (event != NULL) {
        return (struct peripheral_battery_state){
            .source = event->source,
            .level = event->state_of_charge,
            .valid = true,
        };
    }

    uint8_t level = 0;
    int err = zmk_split_central_get_peripheral_battery_level(
        CONFIG_NICE_OLED_ZMK_0_4_PERIPHERAL_INDEX, &level);
    return (struct peripheral_battery_state){
        .source = CONFIG_NICE_OLED_ZMK_0_4_PERIPHERAL_INDEX,
        .level = level,
        .valid = err == 0 && level > 0,
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_nice_oled_peripheral_battery,
                            struct peripheral_battery_state, battery_status_update_cb,
                            battery_status_get_state)
ZMK_SUBSCRIPTION(widget_nice_oled_peripheral_battery, zmk_peripheral_battery_state_changed);

lv_obj_t *zmk_display_status_screen(void) {
    static struct battery_widget widget;
    lv_obj_t *screen = lv_obj_create(NULL);

    lv_obj_remove_style_all(screen);
    lv_obj_set_style_bg_color(screen, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);

    LV_DRAW_BUF_INIT_STATIC(battery_draw_buf);
    widget.canvas = lv_canvas_create(screen);
    lv_canvas_set_draw_buf(widget.canvas, &battery_draw_buf);
    lv_canvas_set_palette(widget.canvas, 0, lv_color32_make(0xff, 0xff, 0xff, 0xff));
    lv_canvas_set_palette(widget.canvas, 1, lv_color32_make(0x00, 0x00, 0x00, 0xff));
    lv_obj_align(widget.canvas, LV_ALIGN_TOP_LEFT, 0, 0);

    sys_slist_append(&widgets, &widget.node);
    widget_nice_oled_peripheral_battery_init();

    return screen;
}

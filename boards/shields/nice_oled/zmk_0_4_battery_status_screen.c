/*
 * Copyright (c) 2026 te9no
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>

#include <lvgl.h>

#include <zmk/battery.h>
#include <zmk/display.h>
#include <zmk/event_manager.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/keymap.h>
#include <zmk/split/central.h>

#define DISPLAY_NODE DT_CHOSEN(zephyr_display)
#define PHYSICAL_WIDTH DT_PROP(DISPLAY_NODE, width)
#define PHYSICAL_HEIGHT DT_PROP(DISPLAY_NODE, height)
#define PORTRAIT_WIDTH PHYSICAL_HEIGHT
#define PORTRAIT_HEIGHT PHYSICAL_WIDTH

#define BATTERY_SOURCE_CENTRAL 0
#define BATTERY_SOURCE_PERIPHERAL 1
#define BATTERY_SOURCE_COUNT 2
#define BATTERY_SOURCE_IGNORE 0xff
#define LAYER_LABEL_MAX_LEN 8

BUILD_ASSERT(PHYSICAL_WIDTH == 128 && PHYSICAL_HEIGHT == 32,
             "The portrait battery screen currently supports 128x32 displays");

struct battery_state {
    uint8_t level;
    bool valid;
};

struct battery_widget {
    sys_snode_t node;
    lv_obj_t *canvas;
    struct battery_state batteries[BATTERY_SOURCE_COUNT];
    char layer_label[LAYER_LABEL_MAX_LEN + 1];
};

struct battery_update {
    uint8_t source;
    uint8_t level;
    bool valid;
};

struct layer_update {
    zmk_keymap_layer_index_t index;
    const char *label;
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

/* Uppercase letters use the same compact 3x5 cell as the battery digits. */
static const uint8_t letter_rows[26][5] = {
    {0x2, 0x5, 0x7, 0x5, 0x5}, {0x6, 0x5, 0x6, 0x5, 0x6},
    {0x7, 0x4, 0x4, 0x4, 0x7}, {0x6, 0x5, 0x5, 0x5, 0x6},
    {0x7, 0x4, 0x6, 0x4, 0x7}, {0x7, 0x4, 0x6, 0x4, 0x4},
    {0x7, 0x4, 0x5, 0x5, 0x7}, {0x5, 0x5, 0x7, 0x5, 0x5},
    {0x7, 0x2, 0x2, 0x2, 0x7}, {0x1, 0x1, 0x1, 0x5, 0x7},
    {0x5, 0x5, 0x6, 0x5, 0x5}, {0x4, 0x4, 0x4, 0x4, 0x7},
    {0x5, 0x7, 0x7, 0x5, 0x5}, {0x5, 0x7, 0x7, 0x7, 0x5},
    {0x7, 0x5, 0x5, 0x5, 0x7}, {0x6, 0x5, 0x6, 0x4, 0x4},
    {0x7, 0x5, 0x5, 0x7, 0x1}, {0x6, 0x5, 0x6, 0x5, 0x5},
    {0x7, 0x4, 0x7, 0x1, 0x7}, {0x7, 0x2, 0x2, 0x2, 0x2},
    {0x5, 0x5, 0x5, 0x5, 0x7}, {0x5, 0x5, 0x5, 0x5, 0x2},
    {0x5, 0x5, 0x7, 0x7, 0x5}, {0x5, 0x5, 0x2, 0x5, 0x5},
    {0x5, 0x5, 0x2, 0x2, 0x2}, {0x7, 0x1, 0x2, 0x4, 0x7},
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

static void draw_letter(lv_obj_t *canvas, char letter, int32_t x, int32_t y, uint8_t scale) {
    if (letter >= 'a' && letter <= 'z') {
        letter -= 'a' - 'A';
    }
    if (letter < 'A' || letter > 'Z') {
        return;
    }

    const uint8_t *rows = letter_rows[letter - 'A'];
    for (uint8_t row = 0; row < 5; row++) {
        for (uint8_t col = 0; col < 3; col++) {
            if ((rows[row] & BIT(2 - col)) != 0) {
                fill_portrait_rect(canvas, x + col * scale, y + row * scale, scale, scale, true);
            }
        }
    }
}

static void draw_text(lv_obj_t *canvas, const char *text, int32_t y) {
    size_t length = MIN(strlen(text), LAYER_LABEL_MAX_LEN);
    if (length == 0) {
        return;
    }

    const int32_t text_width = length * 3 + (length - 1);
    int32_t x = (PORTRAIT_WIDTH - text_width) / 2;

    for (size_t i = 0; i < length; i++) {
        if (text[i] >= '0' && text[i] <= '9') {
            draw_digit(canvas, text[i] - '0', x, y, 1);
        } else {
            draw_letter(canvas, text[i], x, y, 1);
        }
        x += 4;
    }
}

static void draw_dash(lv_obj_t *canvas, int32_t x, int32_t y) {
    fill_portrait_rect(canvas, x, y + 2, 3, 1, true);
}

static void draw_level(lv_obj_t *canvas, uint8_t source, const struct battery_state *state) {
    const int32_t column_width = PORTRAIT_WIDTH / BATTERY_SOURCE_COUNT;
    const int32_t column_x = source * column_width;

    if (!state->valid) {
        draw_dash(canvas, column_x + 4, 23);
        draw_dash(canvas, column_x + 9, 23);
        return;
    }

    uint8_t level = MIN(state->level, 100);
    char text[4];
    int length = snprintf(text, sizeof(text), "%u", level);
    const int32_t glyph_width = 3;
    const int32_t spacing = 1;
    const int32_t text_width = length * glyph_width + (length - 1) * spacing;
    int32_t x = column_x + (column_width - text_width) / 2;

    for (int i = 0; i < length; i++) {
        draw_digit(canvas, text[i] - '0', x, 23, 1);
        x += glyph_width + spacing;
    }
}

static void draw_battery_outline(lv_obj_t *canvas, uint8_t source,
                                 const struct battery_state *state) {
    const int32_t x = 3 + source * (PORTRAIT_WIDTH / BATTERY_SOURCE_COUNT);
    const int32_t y = 36;
    const int32_t width = 10;
    const int32_t height = 86;

    fill_portrait_rect(canvas, x + 3, y - 4, 4, 4, true);
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

static void draw_source_marker(lv_obj_t *canvas, uint8_t source) {
    const int32_t x = 6 + source * (PORTRAIT_WIDTH / BATTERY_SOURCE_COUNT);
    draw_letter(canvas, source == BATTERY_SOURCE_CENTRAL ? 'C' : 'P', x, 15, 1);
}

static void redraw(struct battery_widget *widget) {
    lv_canvas_fill_bg(widget->canvas, lv_color_hex(0), LV_OPA_COVER);

    draw_text(widget->canvas, widget->layer_label, 3);
    fill_portrait_rect(widget->canvas, 1, 11, PORTRAIT_WIDTH - 2, 1, true);

    for (uint8_t source = 0; source < BATTERY_SOURCE_COUNT; source++) {
        draw_source_marker(widget->canvas, source);
        draw_level(widget->canvas, source, &widget->batteries[source]);
        draw_battery_outline(widget->canvas, source, &widget->batteries[source]);
    }

    lv_obj_invalidate(widget->canvas);
}

static void layer_status_update_cb(struct layer_update update) {
    struct battery_widget *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) {
        if (update.label != NULL && update.label[0] != '\0') {
            snprintf(widget->layer_label, sizeof(widget->layer_label), "%.*s",
                     LAYER_LABEL_MAX_LEN, update.label);
        } else {
            snprintf(widget->layer_label, sizeof(widget->layer_label), "L%u", update.index);
        }
        redraw(widget);
    }
}

static struct layer_update layer_status_get_state(const zmk_event_t *eh) {
    zmk_keymap_layer_index_t index = zmk_keymap_highest_layer_active();
    return (struct layer_update){
        .index = index,
        .label = zmk_keymap_layer_name(zmk_keymap_layer_index_to_id(index)),
    };
}

static void battery_status_update_cb(struct battery_update update) {
    if (update.source >= BATTERY_SOURCE_COUNT) {
        return;
    }

    struct battery_widget *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) {
        widget->batteries[update.source] = (struct battery_state){
            .level = update.level,
            .valid = update.valid,
        };
        redraw(widget);
    }
}

static struct battery_update battery_status_get_state(const zmk_event_t *eh) {
    const struct zmk_peripheral_battery_state_changed *peripheral_event =
        as_zmk_peripheral_battery_state_changed(eh);

    if (peripheral_event != NULL) {
        if (peripheral_event->source != CONFIG_NICE_OLED_ZMK_0_4_PERIPHERAL_INDEX) {
            return (struct battery_update){.source = BATTERY_SOURCE_IGNORE};
        }

        return (struct battery_update){
            .source = BATTERY_SOURCE_PERIPHERAL,
            .level = peripheral_event->state_of_charge,
            .valid = true,
        };
    }

    const struct zmk_battery_state_changed *central_event = as_zmk_battery_state_changed(eh);
    return (struct battery_update){
        .source = BATTERY_SOURCE_CENTRAL,
        .level = central_event != NULL ? central_event->state_of_charge
                                       : zmk_battery_state_of_charge(),
        .valid = true,
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_nice_oled_battery, struct battery_update,
                            battery_status_update_cb, battery_status_get_state)
ZMK_SUBSCRIPTION(widget_nice_oled_battery, zmk_battery_state_changed);
ZMK_SUBSCRIPTION(widget_nice_oled_battery, zmk_peripheral_battery_state_changed);

ZMK_DISPLAY_WIDGET_LISTENER(widget_nice_oled_layer, struct layer_update,
                            layer_status_update_cb, layer_status_get_state)
ZMK_SUBSCRIPTION(widget_nice_oled_layer, zmk_layer_state_changed);

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
    widget_nice_oled_battery_init();
    widget_nice_oled_layer_init();

    uint8_t peripheral_level = 0;
    if (zmk_split_central_get_peripheral_battery_level(
            CONFIG_NICE_OLED_ZMK_0_4_PERIPHERAL_INDEX, &peripheral_level) == 0) {
        widget.batteries[BATTERY_SOURCE_PERIPHERAL] = (struct battery_state){
            .level = peripheral_level,
            .valid = true,
        };
        redraw(&widget);
    }

    return screen;
}

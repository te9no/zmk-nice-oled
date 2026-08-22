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
#include <zmk/activity.h>
#include <zmk/display.h>
#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>
#include <zmk/events/battery_state_changed.h>
#if IS_ENABLED(CONFIG_NICE_OLED_ZMK_0_4_BONGO_CAT)
#include "assets/bongo_cat_portrait.h"
#endif
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#include <zmk/ble.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/keymap.h>
#include <zmk/split/central.h>
#endif

#define DISPLAY_NODE DT_CHOSEN(zephyr_display)
#define PHYSICAL_WIDTH DT_PROP(DISPLAY_NODE, width)
#define PHYSICAL_HEIGHT DT_PROP(DISPLAY_NODE, height)
#define PORTRAIT_WIDTH PHYSICAL_HEIGHT
#define PORTRAIT_HEIGHT PHYSICAL_WIDTH

#define BATTERY_SOURCE_CENTRAL 0
#define BATTERY_SOURCE_PERIPHERAL 1
#define BATTERY_SOURCE_COUNT 2
#define BATTERY_SOURCE_IGNORE 0xff
#define LAYER_LABEL_MAX_LEN 4
#define CAT_FRAME_COUNT 4
#define CAT_ANIMATION_PERIOD_MS 600

BUILD_ASSERT(PHYSICAL_WIDTH == 128 && PHYSICAL_HEIGHT == 32,
             "The portrait battery screen currently supports 128x32 displays");

struct battery_state {
    uint8_t level;
    bool valid;
};

struct battery_widget {
    sys_snode_t node;
    lv_obj_t *canvas;
    lv_timer_t *cat_timer;
    struct battery_state batteries[BATTERY_SOURCE_COUNT];
    char layer_label[LAYER_LABEL_MAX_LEN + 1];
    uint8_t cat_frame;
    uint8_t bt_profile;
    bool bt_connected;
    bool bt_bonded;
    enum zmk_activity_state activity_state;
};

struct battery_update {
    uint8_t source;
    uint8_t level;
    bool valid;
};

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
struct layer_update {
    zmk_keymap_layer_index_t index;
    const char *label;
};

struct profile_update {
    uint8_t index;
    bool connected;
    bool bonded;
};
#endif

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

static int32_t text_width(size_t length, uint8_t scale) {
    return length == 0 ? 0 : length * 3 * scale + (length - 1) * scale;
}

static void draw_text(lv_obj_t *canvas, const char *text, int32_t y, uint8_t scale,
                      size_t max_length) {
    size_t length = MIN(strlen(text), max_length);
    if (length == 0) {
        return;
    }

    int32_t x = (PORTRAIT_WIDTH - text_width(length, scale)) / 2;

    for (size_t i = 0; i < length; i++) {
        if (text[i] >= '0' && text[i] <= '9') {
            draw_digit(canvas, text[i] - '0', x, y, scale);
        } else {
            draw_letter(canvas, text[i], x, y, scale);
        }
        x += 4 * scale;
    }
}

static void draw_dash(lv_obj_t *canvas, int32_t x, int32_t y, uint8_t scale) {
    fill_portrait_rect(canvas, x, y + 2 * scale, 3 * scale, scale, true);
}

static void draw_bluetooth_icon(lv_obj_t *canvas, int32_t x, int32_t y) {
    static const uint8_t rows[7] = {
        0x4, 0x5, 0x6, 0x4, 0x6, 0x5, 0x4,
    };

    for (uint8_t row = 0; row < ARRAY_SIZE(rows); row++) {
        for (uint8_t col = 0; col < 3; col++) {
            if ((rows[row] & BIT(2 - col)) != 0) {
                set_portrait_pixel(canvas, x + col, y + row, true);
            }
        }
    }
}

static void draw_connection_status(lv_obj_t *canvas, int32_t x, int32_t y, bool connected,
                                   bool bonded) {
    if (connected) {
        set_portrait_pixel(canvas, x, y + 3, true);
        set_portrait_pixel(canvas, x + 1, y + 4, true);
        set_portrait_pixel(canvas, x + 2, y + 3, true);
        set_portrait_pixel(canvas, x + 3, y + 2, true);
        set_portrait_pixel(canvas, x + 4, y + 1, true);
    } else if (bonded) {
        for (uint8_t i = 0; i < 5; i++) {
            set_portrait_pixel(canvas, x + i, y + i, true);
            set_portrait_pixel(canvas, x + 4 - i, y + i, true);
        }
    } else {
        set_portrait_pixel(canvas, x, y + 2, true);
        set_portrait_pixel(canvas, x + 2, y + 2, true);
        set_portrait_pixel(canvas, x + 4, y + 2, true);
    }
}

static void draw_cat(lv_obj_t *canvas, uint8_t frame) {
    const int32_t x = 4;
    const int32_t y = 18;

    /* A compact silhouette keeps the animation readable on a 32 px wide display. */
    fill_portrait_rect(canvas, x + 5, y + 2, 3, 6, true);
    fill_portrait_rect(canvas, x + 16, y + 2, 3, 6, true);
    fill_portrait_rect(canvas, x + 4, y + 6, 16, 15, true);
    fill_portrait_rect(canvas, x + 7, y + 21, 11, 21, true);

    /* Face: the third frame blinks. */
    if (frame == 2) {
        fill_portrait_rect(canvas, x + 7, y + 12, 3, 1, false);
        fill_portrait_rect(canvas, x + 14, y + 12, 3, 1, false);
    } else {
        fill_portrait_rect(canvas, x + 8, y + 11, 2, 2, false);
        fill_portrait_rect(canvas, x + 14, y + 11, 2, 2, false);
    }
    set_portrait_pixel(canvas, x + 11, y + 15, false);
    set_portrait_pixel(canvas, x + 12, y + 15, false);
    fill_portrait_rect(canvas, x + 10, y + 17, 4, 1, false);

    /* Belly and feet separate the silhouette from a solid rectangle. */
    fill_portrait_rect(canvas, x + 10, y + 26, 5, 11, false);
    fill_portrait_rect(canvas, x + 6, y + 40, 6, 4, true);
    fill_portrait_rect(canvas, x + 14, y + 40, 6, 4, true);

    /* Tail and one paw alternate between frames. */
    if ((frame & 1) == 0) {
        fill_portrait_rect(canvas, x + 18, y + 25, 3, 14, true);
        fill_portrait_rect(canvas, x + 20, y + 22, 5, 3, true);
    } else {
        fill_portrait_rect(canvas, x + 18, y + 29, 3, 10, true);
        fill_portrait_rect(canvas, x + 20, y + 27, 6, 3, true);
    }

    if (frame == 3) {
        fill_portrait_rect(canvas, x + 15, y + 32, 4, 8, false);
        fill_portrait_rect(canvas, x + 17, y + 27, 4, 8, true);
    }
}

static void draw_level(lv_obj_t *canvas, uint8_t source, const struct battery_state *state) {
    const int32_t y = source == BATTERY_SOURCE_CENTRAL ? 82 : 105;

    draw_letter(canvas, source == BATTERY_SOURCE_CENTRAL ? 'C' : 'P', 1, y, 2);

    if (!state->valid) {
        draw_dash(canvas, 18, y, 2);
        return;
    }

    uint8_t level = MIN(state->level, 100);
    char text[4];
    int length = snprintf(text, sizeof(text), "%u", level);
    int32_t x = PORTRAIT_WIDTH - text_width(length, 2) - 1;

    for (int i = 0; i < length; i++) {
        draw_digit(canvas, text[i] - '0', x, y, 2);
        x += 8;
    }
}

static void draw_battery_gauge(lv_obj_t *canvas, uint8_t source,
                               const struct battery_state *state) {
    const int32_t x = 1;
    const int32_t y = source == BATTERY_SOURCE_CENTRAL ? 94 : 117;
    const int32_t width = PORTRAIT_WIDTH - 2;
    const int32_t height = 6;

    fill_portrait_rect(canvas, x, y, width, 1, true);
    fill_portrait_rect(canvas, x, y + height - 1, width, 1, true);
    fill_portrait_rect(canvas, x, y, 1, height, true);
    fill_portrait_rect(canvas, x + width - 1, y, 1, height, true);

    if (!state->valid) {
        return;
    }

    const int32_t inner_width = width - 4;
    int32_t fill_width = DIV_ROUND_CLOSEST(inner_width * MIN(state->level, 100), 100);
    if (fill_width > 0) {
        fill_portrait_rect(canvas, x + 2, y + 2, fill_width, height - 4, true);
    }
}

#if IS_ENABLED(CONFIG_NICE_OLED_ZMK_0_4_BONGO_CAT)
static void draw_bongo_cat_frame(lv_obj_t *canvas, const uint8_t *pixels, int32_t y) {
    const uint8_t crop_offset =
        (BONGO_CAT_PORTRAIT_HEIGHT - PORTRAIT_WIDTH) / 2;

    for (uint8_t row = 0; row < BONGO_CAT_PORTRAIT_HEIGHT; row++) {
        if (row < crop_offset || row >= crop_offset + PORTRAIT_WIDTH) {
            continue;
        }

        for (uint8_t col = 0; col < BONGO_CAT_PORTRAIT_WIDTH; col++) {
            const uint8_t byte = pixels[row * BONGO_CAT_PORTRAIT_STRIDE + col / 8];
            if ((byte & BIT(7 - (col % 8))) != 0) {
                /* The legacy frames are pre-rotated for a landscape canvas. */
                const int32_t portrait_x = row - crop_offset;
                const int32_t portrait_y = y + BONGO_CAT_PORTRAIT_WIDTH - 1 - col;
                set_portrait_pixel(canvas, portrait_x, portrait_y, true);
            }
        }
    }
}

static void redraw_peripheral_companion(struct battery_widget *widget) {
    lv_canvas_fill_bg(widget->canvas, lv_color_hex(0), LV_OPA_COVER);

    if (widget->activity_state == ZMK_ACTIVITY_ACTIVE) {
        const uint8_t *frame = (widget->cat_frame & 1) == 0 ? bongo_cat_tap1_03_pixels
                                                            : bongo_cat_tap2_03_pixels;
        draw_bongo_cat_frame(widget->canvas, frame, 24);
    } else {
        draw_bongo_cat_frame(widget->canvas, bongo_cat_tap1_01_pixels, 24);
        draw_letter(widget->canvas, 'Z', 24, 13, 1);
        draw_letter(widget->canvas, 'Z', 20, 20, 1);
    }

    fill_portrait_rect(widget->canvas, 1, 100, PORTRAIT_WIDTH - 2, 1, true);
    draw_level(widget->canvas, BATTERY_SOURCE_PERIPHERAL,
               &widget->batteries[BATTERY_SOURCE_PERIPHERAL]);
    draw_battery_gauge(widget->canvas, BATTERY_SOURCE_PERIPHERAL,
                       &widget->batteries[BATTERY_SOURCE_PERIPHERAL]);

    lv_obj_invalidate(widget->canvas);
}
#endif

static void redraw(struct battery_widget *widget) {
#if !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) &&                                           \
    IS_ENABLED(CONFIG_NICE_OLED_ZMK_0_4_PERIPHERAL_ROLE_LABEL)
    redraw_peripheral_companion(widget);
    return;
#endif

    lv_canvas_fill_bg(widget->canvas, lv_color_hex(0), LV_OPA_COVER);

    draw_text(widget->canvas, widget->layer_label, 2, 2, LAYER_LABEL_MAX_LEN);
    fill_portrait_rect(widget->canvas, 1, 14, PORTRAIT_WIDTH - 2, 1, true);
#if IS_ENABLED(CONFIG_NICE_OLED_ZMK_0_4_BONGO_CAT)
    if (widget->activity_state == ZMK_ACTIVITY_ACTIVE) {
        const uint8_t *frame = (widget->cat_frame & 1) == 0 ? bongo_cat_tap1_03_pixels
                                                            : bongo_cat_tap2_03_pixels;
        draw_bongo_cat_frame(widget->canvas, frame, 15);
    } else {
        draw_bongo_cat_frame(widget->canvas, bongo_cat_tap1_01_pixels, 15);
        draw_letter(widget->canvas, 'Z', 27, 20, 1);
        draw_letter(widget->canvas, 'Z', 24, 27, 1);
    }
#else
    draw_cat(widget->canvas, widget->cat_frame);
#endif

    draw_bluetooth_icon(widget->canvas, 3, 65);
    draw_digit(widget->canvas, (widget->bt_profile + 1) % 10, 13, 66, 1);
    draw_connection_status(widget->canvas, 23, 66, widget->bt_connected, widget->bt_bonded);
    fill_portrait_rect(widget->canvas, 1, 76, PORTRAIT_WIDTH - 2, 1, true);

    for (uint8_t source = 0; source < BATTERY_SOURCE_COUNT; source++) {
        draw_level(widget->canvas, source, &widget->batteries[source]);
        draw_battery_gauge(widget->canvas, source, &widget->batteries[source]);
    }

    lv_obj_invalidate(widget->canvas);
}

static void cat_animation_timer_cb(lv_timer_t *timer) {
    struct battery_widget *widget = lv_timer_get_user_data(timer);

    widget->cat_frame = (widget->cat_frame + 1) % CAT_FRAME_COUNT;
    redraw(widget);
}

#if IS_ENABLED(CONFIG_NICE_OLED_ZMK_0_4_BONGO_CAT)
static void activity_status_update_cb(struct zmk_activity_state_changed update) {
    struct battery_widget *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) {
        widget->activity_state = update.state;
        if (widget->cat_timer != NULL) {
            if (update.state == ZMK_ACTIVITY_ACTIVE) {
                lv_timer_resume(widget->cat_timer);
            } else {
                lv_timer_pause(widget->cat_timer);
            }
        }
        redraw(widget);
    }
}

static struct zmk_activity_state_changed activity_status_get_state(const zmk_event_t *eh) {
    const struct zmk_activity_state_changed *event = as_zmk_activity_state_changed(eh);
    return (struct zmk_activity_state_changed){
        .state = event != NULL ? event->state : zmk_activity_get_state(),
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_nice_oled_activity, struct zmk_activity_state_changed,
                            activity_status_update_cb, activity_status_get_state)
ZMK_SUBSCRIPTION(widget_nice_oled_activity, zmk_activity_state_changed);
#endif

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
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

static void profile_status_update_cb(struct profile_update update) {
    struct battery_widget *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) {
        widget->bt_profile = update.index;
        widget->bt_connected = update.connected;
        widget->bt_bonded = update.bonded;
        redraw(widget);
    }
}

static struct profile_update profile_status_get_state(const zmk_event_t *eh) {
    const struct zmk_ble_active_profile_changed *event = as_zmk_ble_active_profile_changed(eh);
    int index = event != NULL ? event->index : zmk_ble_active_profile_index();

    return (struct profile_update){
        .index = index < 0 ? 0 : index,
        .connected = zmk_ble_active_profile_is_connected(),
        .bonded = !zmk_ble_active_profile_is_open(),
    };
}
#endif

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
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
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
#else
    const struct zmk_battery_state_changed *local_event = as_zmk_battery_state_changed(eh);
    return (struct battery_update){
        .source = BATTERY_SOURCE_PERIPHERAL,
#endif
        .level =
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
            central_event != NULL ? central_event->state_of_charge
#else
            local_event != NULL ? local_event->state_of_charge
#endif
                                       : zmk_battery_state_of_charge(),
        .valid = true,
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_nice_oled_battery, struct battery_update,
                            battery_status_update_cb, battery_status_get_state)
ZMK_SUBSCRIPTION(widget_nice_oled_battery, zmk_battery_state_changed);
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
ZMK_SUBSCRIPTION(widget_nice_oled_battery, zmk_peripheral_battery_state_changed);

ZMK_DISPLAY_WIDGET_LISTENER(widget_nice_oled_layer, struct layer_update,
                            layer_status_update_cb, layer_status_get_state)
ZMK_SUBSCRIPTION(widget_nice_oled_layer, zmk_layer_state_changed);

ZMK_DISPLAY_WIDGET_LISTENER(widget_nice_oled_profile, struct profile_update,
                            profile_status_update_cb, profile_status_get_state)
ZMK_SUBSCRIPTION(widget_nice_oled_profile, zmk_ble_active_profile_changed);
ZMK_SUBSCRIPTION(widget_nice_oled_profile, zmk_endpoint_changed);
#endif

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
    widget.activity_state = zmk_activity_get_state();
    widget_nice_oled_battery_init();
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    widget_nice_oled_layer_init();
    widget_nice_oled_profile_init();
#endif
    widget.cat_timer = lv_timer_create(cat_animation_timer_cb, CAT_ANIMATION_PERIOD_MS, &widget);
#if IS_ENABLED(CONFIG_NICE_OLED_ZMK_0_4_BONGO_CAT)
    widget_nice_oled_activity_init();
    if (widget.activity_state != ZMK_ACTIVITY_ACTIVE) {
        lv_timer_pause(widget.cat_timer);
    }
#endif

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    uint8_t peripheral_level = 0;
    if (zmk_split_central_get_peripheral_battery_level(
            CONFIG_NICE_OLED_ZMK_0_4_PERIPHERAL_INDEX, &peripheral_level) == 0) {
        widget.batteries[BATTERY_SOURCE_PERIPHERAL] = (struct battery_state){
            .level = peripheral_level,
            .valid = true,
        };
        redraw(&widget);
    }
#endif

    return screen;
}

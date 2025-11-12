#include <zephyr/kernel.h>
#include <stdio.h>
#include "battery.h"
#include "../assets/custom_fonts.h"

LV_IMG_DECLARE(bolt);

/* TODO: charging animation START

LV_IMG_DECLARE(battery);
LV_IMG_DECLARE(battery_mask);
LV_IMG_DECLARE(grid_black);

void draw_battery_status_charging_level_big(lv_obj_t *canvas, const struct status_state *state) {
    lv_draw_img_dsc_t img_dsc;
    lv_draw_img_dsc_init(&img_dsc);
    lv_draw_rect_dsc_t rect_dsc;
    init_rect_dsc(&rect_dsc, LVGL_FOREGROUND);
    lv_draw_label_dsc_t outline_dsc;
    init_label_dsc(&outline_dsc, LVGL_BACKGROUND, &pixel_operator_mono, LV_TEXT_ALIGN_CENTER);
    lv_draw_label_dsc_t label_dsc;
    init_label_dsc(&label_dsc, LVGL_FOREGROUND, &pixel_operator_mono, LV_TEXT_ALIGN_CENTER);

    lv_canvas_draw_img(canvas, 0, 65, &grid_black, &img_dsc);

    // TODO: move lv_canvas_draw_img(canvas, x, y, src, style);
    lv_canvas_draw_img(canvas, 0, 66, &battery, &img_dsc); // ref: 0, 0 (bateria)
    // TODO: move lv_canvas_draw_rect(canvas, x, y, width, heigth, &draw_dsc).
    // move 67
    lv_canvas_draw_rect(canvas, 4, 70, 54 * state->battery / 100, 23,
                        &rect_dsc); // ref: 4, 4 (blanco)
    // TODO: move 68 - 66
    lv_canvas_draw_img(canvas, 2, 68, &battery_mask, &img_dsc); // def: 2, 2 (fondo rallado)

    char text[10] = {};
    sprintf(text, "%i%%", state->battery);

    // TODO: move funciona
    const int y = 75; // def: 9
    const int w = 62; // def: 62

    for (int dx = -1; dx <= 1; dx++) {
        for (int dy = -1; dy <= 1; dy++) {
            if (dx != 0 || dy != 0) {
                lv_canvas_draw_text(canvas, dx, y + dy, w, &outline_dsc, text);
            }
        }
    }

    lv_canvas_draw_text(canvas, 0, y, w, &label_dsc, text);

    // if (state->charging) { }
}
TODO : charging animation END */

static void draw_level(lv_obj_t *canvas, const struct status_state *state) {
    lv_draw_label_dsc_t label_right_dsc;
    init_label_dsc(&label_right_dsc, LVGL_FOREGROUND, &pixel_operator_mono, LV_TEXT_ALIGN_RIGHT);

    char text[10] = {};

    sprintf(text, "%i%%", state->battery);
    lv_canvas_draw_text(canvas, 26, 19, 42, &label_right_dsc, text);
}

static void draw_charging_level(lv_obj_t *canvas, const struct status_state *state) {
    lv_draw_img_dsc_t img_dsc;
    lv_draw_img_dsc_init(&img_dsc);
    lv_draw_label_dsc_t label_right_dsc;
    init_label_dsc(&label_right_dsc, LVGL_FOREGROUND, &pixel_operator_mono, LV_TEXT_ALIGN_RIGHT);

    char text[10] = {};

    sprintf(text, "%i%%", state->battery);
    lv_canvas_draw_text(canvas, 26, 19, 35, &label_right_dsc, text);
    lv_canvas_draw_img(canvas, 62, 21, &bolt, &img_dsc);
}

#if defined(CONFIG_ZMK_SPLIT) && defined(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) &&                \
    defined(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
static void draw_peripheral_levels(lv_obj_t *canvas, const struct status_state *state) {
    lv_draw_label_dsc_t label_dsc;
    init_label_dsc(&label_dsc, LVGL_FOREGROUND, &pixel_operator_mono, LV_TEXT_ALIGN_LEFT);

    uint8_t line = 0;
    for (int i = 0; i < ZMK_SPLIT_BLE_PERIPHERAL_COUNT; i++) {
        if (!state->peripheral_battery_present[i]) {
            continue;
        }

        char text[14];
        snprintf(text, sizeof(text), "P%i %i%%", i + 1, state->peripheral_battery_level[i]);
        lv_canvas_draw_text(canvas, 0, 36 + (line * 12), 64, &label_dsc, text);
        line++;
    }
}
#endif

void draw_battery_status(lv_obj_t *canvas, const struct status_state *state) {
    lv_draw_label_dsc_t label_left_dsc;
    init_label_dsc(&label_left_dsc, LVGL_FOREGROUND, &pixel_operator_mono, LV_TEXT_ALIGN_LEFT);
    lv_canvas_draw_text(canvas, 0, 19, 25, &label_left_dsc, "BAT");

    if (state->charging) {
        // draw_battery_status_charging_level_big(canvas, state);
        draw_charging_level(canvas, state);
    } else {
        draw_level(canvas, state);
    }

#if defined(CONFIG_ZMK_SPLIT) && defined(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) &&                \
    defined(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
    draw_peripheral_levels(canvas, state);
#endif
}

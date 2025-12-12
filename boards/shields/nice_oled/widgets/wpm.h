#pragma once

#include "lvgl_compat.h"
#include "util.h"

struct wpm_status_state {
    uint8_t wpm;
};

void draw_wpm_status(lv_obj_t *canvas, const struct status_state *state);
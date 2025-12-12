#pragma once

#include "lvgl_compat.h"

#if LVGL_VERSION_MAJOR >= 9
void lv_canvas_draw_rect(lv_obj_t *canvas, lv_coord_t x, lv_coord_t y, lv_coord_t w,
                         lv_coord_t h, lv_draw_rect_dsc_t *draw_dsc);
void lv_canvas_draw_line(lv_obj_t *canvas, const lv_point_t points[], uint32_t point_cnt,
                         lv_draw_line_dsc_t *draw_dsc);
void lv_canvas_draw_text(lv_obj_t *canvas, lv_coord_t x, lv_coord_t y, lv_coord_t max_w,
                         lv_draw_label_dsc_t *draw_dsc, const char *txt);
void lv_canvas_draw_img(lv_obj_t *canvas, lv_coord_t x, lv_coord_t y,
                        const lv_image_dsc_t *src, lv_draw_image_dsc_t *draw_dsc);
#endif

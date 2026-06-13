#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    lv_obj_t *main;
    lv_obj_t *main_image;
    lv_obj_t *dialogue_box;
    lv_obj_t *volume_bar;
} objects_t;

extern objects_t objects;

void ui_init(void);
void ui_tick(void);

#ifdef __cplusplus
}
#endif

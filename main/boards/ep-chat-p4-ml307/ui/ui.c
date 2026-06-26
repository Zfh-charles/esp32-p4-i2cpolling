#include "ui.h"
#include "screens.h"

objects_t objects;

void loadScreen(void)
{
    objects.main = lv_obj_create(NULL);
    lv_obj_remove_style_all(objects.main);
    lv_obj_set_size(objects.main, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(objects.main, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(objects.main, LV_OPA_COVER, 0);

    objects.main_image = NULL;

    objects.dialogue_box = lv_label_create(objects.main);
    lv_obj_set_width(objects.dialogue_box, LV_PCT(90));
    lv_obj_align(objects.dialogue_box, LV_ALIGN_BOTTOM_MID, 0, -40);
    lv_label_set_long_mode(objects.dialogue_box, LV_LABEL_LONG_WRAP);
    lv_obj_add_flag(objects.dialogue_box, LV_OBJ_FLAG_HIDDEN);

    objects.volume_bar = lv_bar_create(objects.main);
    lv_obj_set_size(objects.volume_bar, LV_PCT(60), 8);
    lv_obj_align(objects.volume_bar, LV_ALIGN_TOP_MID, 0, 16);
    lv_obj_add_flag(objects.volume_bar, LV_OBJ_FLAG_HIDDEN);

    lv_scr_load(objects.main);
}

void ui_init(void)
{
    loadScreen();
}

void ui_tick(void)
{
}

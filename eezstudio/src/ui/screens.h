#ifndef EEZ_LVGL_UI_SCREENS_H
#define EEZ_LVGL_UI_SCREENS_H

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

// Screens

enum ScreensEnum {
    _SCREEN_ID_FIRST = 1,
    SCREEN_ID_MAIN = 1,
    SCREEN_ID_PAGE_PRESET_LIST = 2,
    _SCREEN_ID_LAST = 2
};

typedef struct _objects_t {
    lv_obj_t *main;
    lv_obj_t *page_preset_list;
    lv_obj_t *lbl_title_version;
    lv_obj_t *btn_panic;
    lv_obj_t *btn_next_preset;
    lv_obj_t *btn_previous_preset;
    lv_obj_t *lbl_preset_name;
    lv_obj_t *lbl_preset_number;
    lv_obj_t *lbl_psram_info;
    lv_obj_t *lbl_sysram_info;
    lv_obj_t *lbl_core0_util;
    lv_obj_t *lbl_core1_util;
    lv_obj_t *lbl_used_voices;
    lv_obj_t *lbl_underruns;
    lv_obj_t *lbl_midi_devicename;
    lv_obj_t *bar_core0_util;
    lv_obj_t *bar_core1_util;
    lv_obj_t *lbl_ip_address;
    lv_obj_t *bar_preset_loader;
    lv_obj_t *tbl_presets;
    lv_obj_t *btn_back;
    lv_obj_t *lbl_title_presets;
} objects_t;

extern objects_t objects;

void create_screen_main();
void tick_screen_main();

void create_screen_page_preset_list();
void tick_screen_page_preset_list();

void tick_screen_by_id(enum ScreensEnum screenId);
void tick_screen(int screen_index);

void create_screens();

#ifdef __cplusplus
}
#endif

#endif /*EEZ_LVGL_UI_SCREENS_H*/

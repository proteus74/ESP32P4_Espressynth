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
    SCREEN_ID_PAGE_SETTINGS = 3,
    _SCREEN_ID_LAST = 3
};

typedef struct _objects_t {
    lv_obj_t *main;
    lv_obj_t *page_preset_list;
    lv_obj_t *page_settings;
    lv_obj_t *lbl_title_version;
    lv_obj_t *btn_next_preset;
    lv_obj_t *btn_previous_preset;
    lv_obj_t *btn_panic;
    lv_obj_t *obj0;
    lv_obj_t *lbl_ip_address;
    lv_obj_t *lbl_midi_devicename;
    lv_obj_t *lbl_underruns;
    lv_obj_t *lbl_used_voices;
    lv_obj_t *lbl_psram_info;
    lv_obj_t *lbl_sysram_info;
    lv_obj_t *bar_core0_util;
    lv_obj_t *lbl_core0_util;
    lv_obj_t *lbl_core1_util;
    lv_obj_t *lbl_bpm;
    lv_obj_t *bar_core1_util;
    lv_obj_t *bar_master_volume;
    lv_obj_t *lbl_bpm_1;
    lv_obj_t *wndmain;
    lv_obj_t *lbl_preset_name;
    lv_obj_t *btn_pages;
    lv_obj_t *btn_settings;
    lv_obj_t *btn_save_parameter;
    lv_obj_t *tbl_presets;
    lv_obj_t *btn_back;
    lv_obj_t *lbl_title_presets;
    lv_obj_t *lbl_title_settings;
    lv_obj_t *btn_back_1;
    lv_obj_t *lbl_title_settings_1;
    lv_obj_t *lbl_title_settings_2;
    lv_obj_t *chk_settings_autosave;
    lv_obj_t *lbl_title_settings_3;
    lv_obj_t *settings_keyboard;
    lv_obj_t *txt_settings_ssid;
    lv_obj_t *txt_settings_password;
    lv_obj_t *btn_setting_scan;
} objects_t;

extern objects_t objects;

void create_screen_main();
void tick_screen_main();

void create_screen_page_preset_list();
void tick_screen_page_preset_list();

void create_screen_page_settings();
void tick_screen_page_settings();

void create_user_widget_knob(lv_obj_t *parent_obj, int startWidgetIndex);
void tick_user_widget_knob(int startWidgetIndex);

void create_user_widget_midi_button(lv_obj_t *parent_obj, int startWidgetIndex);
void tick_user_widget_midi_button(int startWidgetIndex);

void create_user_widget_envelope(lv_obj_t *parent_obj, int startWidgetIndex);
void tick_user_widget_envelope(int startWidgetIndex);

void tick_screen_by_id(enum ScreensEnum screenId);
void tick_screen(int screen_index);

void create_screens();

#ifdef __cplusplus
}
#endif

#endif /*EEZ_LVGL_UI_SCREENS_H*/
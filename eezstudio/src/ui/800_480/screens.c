#include <string.h>

#include "screens.h"
#include "images.h"
#include "fonts.h"
#include "actions.h"
#include "vars.h"
#include "styles.h"
#include "ui.h"

#include <string.h>

objects_t objects;

//
// Event handlers
//

lv_obj_t *tick_value_change_obj;

//
// Screens
//

void create_screen_main() {
    lv_obj_t *obj = lv_obj_create(0);
    objects.main = obj;
    lv_obj_set_pos(obj, 0, 0);
    lv_obj_set_size(obj, 800, 480);
    {
        lv_obj_t *parent_obj = obj;
        {
            // lbl_title_version
            lv_obj_t *obj = lv_label_create(parent_obj);
            objects.lbl_title_version = obj;
            lv_obj_set_pos(obj, 474, 0);
            lv_obj_set_size(obj, 326, 44);
            lv_obj_set_style_text_font(obj, &lv_font_montserrat_30, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_color(obj, lv_color_hex(0x2e2e2e), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_border_color(obj, lv_color_hex(0xc8c8c8), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_border_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_border_width(obj, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_border_side(obj, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_right(obj, 4, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text(obj, "ESPRESSYNTH 1.0.0");
        }
        {
            // btn_next_preset
            lv_obj_t *obj = lv_button_create(parent_obj);
            objects.btn_next_preset = obj;
            lv_obj_set_pos(obj, 620, 44);
            lv_obj_set_size(obj, 177, 61);
            add_style_button(obj);
            {
                lv_obj_t *parent_obj = obj;
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    lv_obj_set_pos(obj, 0, 0);
                    lv_obj_set_size(obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
                    lv_obj_set_style_align(obj, LV_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text_static(obj, "Next Preset");
                }
            }
        }
        {
            // btn_previous_preset
            lv_obj_t *obj = lv_button_create(parent_obj);
            objects.btn_previous_preset = obj;
            lv_obj_set_pos(obj, 620, 109);
            lv_obj_set_size(obj, 177, 61);
            add_style_button(obj);
            {
                lv_obj_t *parent_obj = obj;
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    lv_obj_set_pos(obj, 0, 0);
                    lv_obj_set_size(obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
                    lv_obj_set_style_align(obj, LV_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text_static(obj, "Previous Preset");
                }
            }
        }
        {
            lv_obj_t *obj = lv_obj_create(parent_obj);
            lv_obj_set_pos(obj, 0, 440);
            lv_obj_set_size(obj, 800, 40);
            lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_style_radius(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_opa(obj, 0, LV_PART_SCROLLBAR | LV_STATE_DEFAULT);
            {
                lv_obj_t *parent_obj = obj;
                {
                    // btn_panic
                    lv_obj_t *obj = lv_button_create(parent_obj);
                    objects.btn_panic = obj;
                    lv_obj_set_pos(obj, 594, -22);
                    lv_obj_set_size(obj, 181, 39);
                    lv_obj_set_style_bg_color(obj, lv_color_hex(0xf32121), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_radius(obj, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
                    {
                        lv_obj_t *parent_obj = obj;
                        {
                            lv_obj_t *obj = lv_label_create(parent_obj);
                            objects.obj0 = obj;
                            lv_obj_set_pos(obj, 0, 1);
                            lv_obj_set_size(obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
                            lv_obj_set_style_align(obj, LV_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_text_font(obj, &lv_font_montserrat_18, LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_text_color(obj, lv_color_hex(0xc7c7c7), LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_label_set_text_static(obj, "PANIC!");
                        }
                    }
                }
                {
                    // lbl_ip_address
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.lbl_ip_address = obj;
                    lv_obj_set_pos(obj, 136, 0);
                    lv_obj_set_size(obj, 105, 17);
                    lv_obj_set_style_text_font(obj, &lv_font_montserrat_12, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text_static(obj, "lbl_ip_address");
                }
                {
                    // lbl_midi_devicename
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.lbl_midi_devicename = obj;
                    lv_obj_set_pos(obj, 136, -19);
                    lv_obj_set_size(obj, 105, 17);
                    lv_obj_set_style_text_font(obj, &lv_font_montserrat_12, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text_static(obj, "lbl_midi_devicename");
                }
                {
                    // lbl_underruns
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.lbl_underruns = obj;
                    lv_obj_set_pos(obj, 241, -1);
                    lv_obj_set_size(obj, 103, 17);
                    lv_obj_set_style_text_font(obj, &lv_font_montserrat_12, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text_static(obj, "lbl_underruns");
                }
                {
                    // lbl_used_voices
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.lbl_used_voices = obj;
                    lv_obj_set_pos(obj, 241, -20);
                    lv_obj_set_size(obj, 103, 17);
                    lv_obj_set_style_text_font(obj, &lv_font_montserrat_12, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text_static(obj, "lbl_used_voices");
                }
                {
                    // lbl_psram_info
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.lbl_psram_info = obj;
                    lv_obj_set_pos(obj, 346, -20);
                    lv_obj_set_size(obj, 121, 17);
                    lv_obj_set_style_text_font(obj, &lv_font_montserrat_12, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text_static(obj, "lbl_psram_info");
                }
                {
                    // lbl_sysram_info
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.lbl_sysram_info = obj;
                    lv_obj_set_pos(obj, 346, -1);
                    lv_obj_set_size(obj, 121, 17);
                    lv_obj_set_style_text_font(obj, &lv_font_montserrat_12, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text_static(obj, "lbl_sysram_info");
                }
                {
                    // bar_core0_util
                    lv_obj_t *obj = lv_bar_create(parent_obj);
                    objects.bar_core0_util = obj;
                    lv_obj_set_pos(obj, 537, -15);
                    lv_obj_set_size(obj, 52, 7);
                    lv_bar_set_value(obj, 25, LV_ANIM_OFF);
                    lv_obj_set_style_radius(obj, 0, LV_PART_INDICATOR | LV_STATE_DEFAULT);
                    lv_obj_set_style_radius(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                {
                    // lbl_core0_util
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.lbl_core0_util = obj;
                    lv_obj_set_pos(obj, 475, -22);
                    lv_obj_set_size(obj, 53, 17);
                    lv_obj_set_style_text_font(obj, &lv_font_montserrat_12, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text_static(obj, "lbl_core0_util");
                }
                {
                    // lbl_core1_util
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.lbl_core1_util = obj;
                    lv_obj_set_pos(obj, 475, -3);
                    lv_obj_set_size(obj, 53, 17);
                    lv_obj_set_style_text_font(obj, &lv_font_montserrat_12, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text_static(obj, "lbl_core1_util");
                }
                {
                    // lbl_bpm
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.lbl_bpm = obj;
                    lv_obj_set_pos(obj, -16, -19);
                    lv_obj_set_size(obj, 134, 17);
                    lv_obj_set_style_text_font(obj, &lv_font_montserrat_12, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text_static(obj, "lbl_bpm");
                }
                {
                    // bar_core1_util
                    lv_obj_t *obj = lv_bar_create(parent_obj);
                    objects.bar_core1_util = obj;
                    lv_obj_set_pos(obj, 537, 3);
                    lv_obj_set_size(obj, 52, 7);
                    lv_bar_set_value(obj, 25, LV_ANIM_OFF);
                    lv_obj_set_style_radius(obj, 0, LV_PART_INDICATOR | LV_STATE_DEFAULT);
                    lv_obj_set_style_radius(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                {
                    // bar_master_volume
                    lv_obj_t *obj = lv_bar_create(parent_obj);
                    objects.bar_master_volume = obj;
                    lv_obj_set_pos(obj, 11, 3);
                    lv_obj_set_size(obj, 102, 7);
                    lv_bar_set_value(obj, 25, LV_ANIM_OFF);
                    lv_obj_set_style_radius(obj, 0, LV_PART_INDICATOR | LV_STATE_DEFAULT);
                    lv_obj_set_style_radius(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                {
                    // lbl_bpm_1
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.lbl_bpm_1 = obj;
                    lv_obj_set_pos(obj, -16, -2);
                    lv_obj_set_size(obj, 24, 17);
                    lv_obj_set_style_text_font(obj, &lv_font_montserrat_12, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text_static(obj, "Vol:");
                }
            }
        }
        {
            // wndmain
            lv_obj_t *obj = lv_win_create(parent_obj);
            objects.wndmain = obj;
            lv_obj_set_pos(obj, 0, 44);
            lv_obj_set_size(obj, 616, 396);
        }
        {
            // lbl_preset_name
            lv_obj_t *obj = lv_label_create(parent_obj);
            objects.lbl_preset_name = obj;
            lv_obj_set_pos(obj, 0, 0);
            lv_obj_set_size(obj, 474, 44);
            lv_obj_set_style_text_font(obj, &lv_font_montserrat_30, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_color(obj, lv_color_hex(0x2e2e2e), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_border_color(obj, lv_color_hex(0xc8c8c8), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_border_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_border_width(obj, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_border_side(obj, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_left(obj, 3, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text(obj, "lbl_preset_name");
        }
        {
            // btn_pages
            lv_obj_t *obj = lv_button_create(parent_obj);
            objects.btn_pages = obj;
            lv_obj_set_pos(obj, 616, 379);
            lv_obj_set_size(obj, 184, 61);
            add_style_button(obj);
            {
                lv_obj_t *parent_obj = obj;
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    lv_obj_set_pos(obj, 0, 0);
                    lv_obj_set_size(obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
                    lv_obj_set_style_align(obj, LV_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text_static(obj, "Pages");
                }
            }
        }
        {
            // btn_settings
            lv_obj_t *obj = lv_button_create(parent_obj);
            objects.btn_settings = obj;
            lv_obj_set_pos(obj, 620, 182);
            lv_obj_set_size(obj, 177, 61);
            add_style_button(obj);
            {
                lv_obj_t *parent_obj = obj;
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    lv_obj_set_pos(obj, 0, 0);
                    lv_obj_set_size(obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
                    lv_obj_set_style_align(obj, LV_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text_static(obj, "Settings");
                }
            }
        }
        {
            // btn_save_parameter
            lv_obj_t *obj = lv_button_create(parent_obj);
            objects.btn_save_parameter = obj;
            lv_obj_set_pos(obj, 620, 269);
            lv_obj_set_size(obj, 177, 61);
            add_style_button(obj);
            {
                lv_obj_t *parent_obj = obj;
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    lv_obj_set_pos(obj, 0, 0);
                    lv_obj_set_size(obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
                    lv_obj_set_style_align(obj, LV_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text_static(obj, "Save Parameter");
                }
            }
        }
    }
    
    tick_screen_main();
}

void tick_screen_main() {
}

void create_screen_page_preset_list() {
    lv_obj_t *obj = lv_obj_create(0);
    objects.page_preset_list = obj;
    lv_obj_set_pos(obj, 0, 0);
    lv_obj_set_size(obj, 800, 480);
    {
        lv_obj_t *parent_obj = obj;
        {
            // tbl_presets
            lv_obj_t *obj = lv_table_create(parent_obj);
            objects.tbl_presets = obj;
            lv_obj_set_pos(obj, 0, 56);
            lv_obj_set_size(obj, 800, 424);
        }
        {
            // btn_back
            lv_obj_t *obj = lv_button_create(parent_obj);
            objects.btn_back = obj;
            lv_obj_set_pos(obj, 0, 0);
            lv_obj_set_size(obj, 56, 56);
            {
                lv_obj_t *parent_obj = obj;
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    lv_obj_set_pos(obj, 0, 0);
                    lv_obj_set_size(obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
                    lv_obj_set_style_align(obj, LV_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_font(obj, &lv_font_montserrat_48, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text_static(obj, "<");
                }
            }
        }
        {
            // lbl_title_presets
            lv_obj_t *obj = lv_label_create(parent_obj);
            objects.lbl_title_presets = obj;
            lv_obj_set_pos(obj, 66, 6);
            lv_obj_set_size(obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
            lv_obj_set_style_text_font(obj, &lv_font_montserrat_40, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text_static(obj, "Presets");
        }
    }
    
    tick_screen_page_preset_list();
}

void tick_screen_page_preset_list() {
}

void create_screen_page_settings() {
    lv_obj_t *obj = lv_obj_create(0);
    objects.page_settings = obj;
    lv_obj_set_pos(obj, 0, 0);
    lv_obj_set_size(obj, 800, 480);
    {
        lv_obj_t *parent_obj = obj;
        {
            // lbl_title_settings
            lv_obj_t *obj = lv_label_create(parent_obj);
            objects.lbl_title_settings = obj;
            lv_obj_set_pos(obj, 66, 6);
            lv_obj_set_size(obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
            lv_obj_set_style_text_font(obj, &lv_font_montserrat_40, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text_static(obj, "Settings");
        }
        {
            // btn_back_1
            lv_obj_t *obj = lv_button_create(parent_obj);
            objects.btn_back_1 = obj;
            lv_obj_set_pos(obj, 1, 0);
            lv_obj_set_size(obj, 56, 56);
            {
                lv_obj_t *parent_obj = obj;
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    lv_obj_set_pos(obj, 0, 0);
                    lv_obj_set_size(obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
                    lv_obj_set_style_align(obj, LV_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_font(obj, &lv_font_montserrat_48, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text_static(obj, "<");
                }
            }
        }
        {
            // lbl_title_settings_1
            lv_obj_t *obj = lv_label_create(parent_obj);
            objects.lbl_title_settings_1 = obj;
            lv_obj_set_pos(obj, 35, 158);
            lv_obj_set_size(obj, 159, LV_SIZE_CONTENT);
            lv_obj_set_style_text_font(obj, &lv_font_montserrat_30, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text_static(obj, "SSID:");
        }
        {
            // lbl_title_settings_2
            lv_obj_t *obj = lv_label_create(parent_obj);
            objects.lbl_title_settings_2 = obj;
            lv_obj_set_pos(obj, 35, 207);
            lv_obj_set_size(obj, 159, LV_SIZE_CONTENT);
            lv_obj_set_style_text_font(obj, &lv_font_montserrat_30, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text_static(obj, "Password:");
        }
        {
            // chk_settings_autosave
            lv_obj_t *obj = lv_switch_create(parent_obj);
            objects.chk_settings_autosave = obj;
            lv_obj_set_pos(obj, 200, 97);
            lv_obj_set_size(obj, 50, 25);
        }
        {
            // lbl_title_settings_3
            lv_obj_t *obj = lv_label_create(parent_obj);
            objects.lbl_title_settings_3 = obj;
            lv_obj_set_pos(obj, 1, 92);
            lv_obj_set_size(obj, 193, LV_SIZE_CONTENT);
            lv_obj_set_style_text_font(obj, &lv_font_montserrat_30, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text_static(obj, "Autosave:");
        }
        {
            // settings_keyboard
            lv_obj_t *obj = lv_keyboard_create(parent_obj);
            objects.settings_keyboard = obj;
            lv_obj_set_pos(obj, 1, 257);
            lv_obj_set_size(obj, 799, 223);
            lv_obj_set_style_align(obj, LV_ALIGN_DEFAULT, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        {
            // txt_settings_ssid
            lv_obj_t *obj = lv_textarea_create(parent_obj);
            objects.txt_settings_ssid = obj;
            lv_obj_set_pos(obj, 200, 152);
            lv_obj_set_size(obj, 495, 44);
            lv_textarea_set_max_length(obj, 128);
            lv_textarea_set_one_line(obj, false);
            lv_textarea_set_password_mode(obj, false);
        }
        {
            // txt_settings_password
            lv_obj_t *obj = lv_textarea_create(parent_obj);
            objects.txt_settings_password = obj;
            lv_obj_set_pos(obj, 200, 202);
            lv_obj_set_size(obj, 495, 44);
            lv_textarea_set_max_length(obj, 128);
            lv_textarea_set_one_line(obj, false);
            lv_textarea_set_password_mode(obj, false);
        }
        {
            // btn_setting_scan
            lv_obj_t *obj = lv_button_create(parent_obj);
            objects.btn_setting_scan = obj;
            lv_obj_set_pos(obj, 700, 152);
            lv_obj_set_size(obj, 91, 44);
            {
                lv_obj_t *parent_obj = obj;
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    lv_obj_set_pos(obj, 0, 0);
                    lv_obj_set_size(obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
                    lv_obj_set_style_align(obj, LV_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text_static(obj, "Scan");
                }
            }
        }
    }
    
    tick_screen_page_settings();
}

void tick_screen_page_settings() {
}

void create_user_widget_knob(lv_obj_t *parent_obj, int startWidgetIndex) {
    (void)startWidgetIndex;
    lv_obj_t *obj = parent_obj;
    {
        lv_obj_t *parent_obj = obj;
        {
            lv_obj_t *obj = lv_obj_create(parent_obj);
            lv_obj_set_pos(obj, 0, 0);
            lv_obj_set_size(obj, 155, 95);
            lv_obj_set_style_radius(obj, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        {
            // knobbar
            lv_obj_t *obj = lv_bar_create(parent_obj);
            ((lv_obj_t **)&objects)[startWidgetIndex + 0] = obj;
            lv_obj_set_pos(obj, 2, 2);
            lv_obj_set_size(obj, 151, 45);
            lv_bar_set_value(obj, 25, LV_ANIM_OFF);
            lv_obj_set_style_radius(obj, 0, LV_PART_INDICATOR | LV_STATE_DEFAULT);
            lv_obj_set_style_radius(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        {
            // lblknobvalue
            lv_obj_t *obj = lv_label_create(parent_obj);
            ((lv_obj_t **)&objects)[startWidgetIndex + 1] = obj;
            lv_obj_set_pos(obj, 2, 24);
            lv_obj_set_size(obj, 75, 37);
            lv_label_set_long_mode(obj, LV_LABEL_LONG_CLIP);
            lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_font(obj, &lv_font_montserrat_24, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_letter_space(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text(obj, "lblknobvalue");
        }
        {
            // lblknobdevice
            lv_obj_t *obj = lv_label_create(parent_obj);
            ((lv_obj_t **)&objects)[startWidgetIndex + 2] = obj;
            lv_obj_set_pos(obj, 2, 61);
            lv_obj_set_size(obj, 151, 17);
            lv_label_set_long_mode(obj, LV_LABEL_LONG_CLIP);
            lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_font(obj, &lv_font_montserrat_16, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text(obj, "lblknobdevice");
        }
        {
            // lblknobport
            lv_obj_t *obj = lv_label_create(parent_obj);
            ((lv_obj_t **)&objects)[startWidgetIndex + 3] = obj;
            lv_obj_set_pos(obj, 2, 76);
            lv_obj_set_size(obj, 151, 17);
            lv_label_set_long_mode(obj, LV_LABEL_LONG_CLIP);
            lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_font(obj, &lv_font_montserrat_16, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text(obj, "lblknobport");
        }
        {
            // lblnodevalue
            lv_obj_t *obj = lv_label_create(parent_obj);
            ((lv_obj_t **)&objects)[startWidgetIndex + 4] = obj;
            lv_obj_set_pos(obj, 80, 24);
            lv_obj_set_size(obj, 73, 37);
            lv_label_set_long_mode(obj, LV_LABEL_LONG_CLIP);
            lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_font(obj, &lv_font_montserrat_24, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text(obj, "lblnodevalue");
        }
        {
            // lblknobname
            lv_obj_t *obj = lv_label_create(parent_obj);
            ((lv_obj_t **)&objects)[startWidgetIndex + 5] = obj;
            lv_obj_set_pos(obj, 2, 47);
            lv_obj_set_size(obj, 151, 17);
            lv_label_set_long_mode(obj, LV_LABEL_LONG_CLIP);
            lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_font(obj, &lv_font_montserrat_16, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text(obj, "lblknobname");
        }
    }
}

void tick_user_widget_knob(int startWidgetIndex) {
    (void)startWidgetIndex;
}

void create_user_widget_midi_button(lv_obj_t *parent_obj, int startWidgetIndex) {
    (void)startWidgetIndex;
    lv_obj_t *obj = parent_obj;
    {
        lv_obj_t *parent_obj = obj;
        {
            lv_obj_t *obj = lv_obj_create(parent_obj);
            lv_obj_set_pos(obj, 0, 0);
            lv_obj_set_size(obj, 155, 95);
            lv_obj_set_style_radius(obj, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        {
            lv_obj_t *obj = lv_button_create(parent_obj);
            lv_obj_set_pos(obj, 2, 2);
            lv_obj_set_size(obj, 151, 45);
            lv_obj_set_style_radius(obj, 6, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        {
            // lblknobbuttonvalue
            lv_obj_t *obj = lv_label_create(parent_obj);
            ((lv_obj_t **)&objects)[startWidgetIndex + 0] = obj;
            lv_obj_set_pos(obj, 4, 24);
            lv_obj_set_size(obj, 75, 40);
            lv_label_set_long_mode(obj, LV_LABEL_LONG_CLIP);
            lv_obj_set_style_text_font(obj, &lv_font_montserrat_24, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text_static(obj, "lblknobbuttonvalue");
        }
        {
            // lblknobdevice
            lv_obj_t *obj = lv_label_create(parent_obj);
            ((lv_obj_t **)&objects)[startWidgetIndex + 1] = obj;
            lv_obj_set_pos(obj, 4, 61);
            lv_obj_set_size(obj, 149, 17);
            lv_label_set_long_mode(obj, LV_LABEL_LONG_CLIP);
            lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_font(obj, &lv_font_montserrat_16, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text(obj, "lblknobdevice");
        }
        {
            // lblknobport
            lv_obj_t *obj = lv_label_create(parent_obj);
            ((lv_obj_t **)&objects)[startWidgetIndex + 2] = obj;
            lv_obj_set_pos(obj, 3, 76);
            lv_obj_set_size(obj, 149, 17);
            lv_label_set_long_mode(obj, LV_LABEL_LONG_CLIP);
            lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_font(obj, &lv_font_montserrat_16, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text(obj, "lblknobport");
        }
        {
            // lblknobname
            lv_obj_t *obj = lv_label_create(parent_obj);
            ((lv_obj_t **)&objects)[startWidgetIndex + 3] = obj;
            lv_obj_set_pos(obj, 3, 47);
            lv_obj_set_size(obj, 149, 17);
            lv_label_set_long_mode(obj, LV_LABEL_LONG_CLIP);
            lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_font(obj, &lv_font_montserrat_16, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text(obj, "lblknobname");
        }
        {
            // lblknobbuttonnodevalue
            lv_obj_t *obj = lv_label_create(parent_obj);
            ((lv_obj_t **)&objects)[startWidgetIndex + 4] = obj;
            lv_obj_set_pos(obj, 80, 24);
            lv_obj_set_size(obj, 70, 40);
            lv_label_set_long_mode(obj, LV_LABEL_LONG_CLIP);
            lv_obj_set_style_text_font(obj, &lv_font_montserrat_24, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text_static(obj, "lblknobbuttonvalue");
        }
    }
}

void tick_user_widget_midi_button(int startWidgetIndex) {
    (void)startWidgetIndex;
}

void create_user_widget_envelope(lv_obj_t *parent_obj, int startWidgetIndex) {
    (void)startWidgetIndex;
    lv_obj_t *obj = parent_obj;
    {
        lv_obj_t *parent_obj = obj;
        {
            // lblenvelopename
            lv_obj_t *obj = lv_label_create(parent_obj);
            ((lv_obj_t **)&objects)[startWidgetIndex + 0] = obj;
            lv_obj_set_pos(obj, 1, 0);
            lv_obj_set_size(obj, 199, 17);
            lv_label_set_long_mode(obj, LV_LABEL_LONG_CLIP);
            lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_font(obj, &lv_font_montserrat_16, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text(obj, "lblknobname");
        }
        {
            // envelopechart
            lv_obj_t *obj = lv_chart_create(parent_obj);
            ((lv_obj_t **)&objects)[startWidgetIndex + 1] = obj;
            lv_obj_set_pos(obj, 0, 17);
            lv_obj_set_size(obj, 200, 133);
        }
    }
}

void tick_user_widget_envelope(int startWidgetIndex) {
    (void)startWidgetIndex;
}

typedef void (*tick_screen_func_t)();
tick_screen_func_t tick_screen_funcs[] = {
    tick_screen_main,
    tick_screen_page_preset_list,
    tick_screen_page_settings,
};
void tick_screen(int screen_index) {
    if (screen_index >= 0 && screen_index < 3) {
        tick_screen_funcs[screen_index]();
    }
}
void tick_screen_by_id(enum ScreensEnum screenId) {
    tick_screen(screenId - 1);
}

//
// Fonts
//

ext_font_desc_t fonts[] = {
#if LV_FONT_MONTSERRAT_8
    { "MONTSERRAT_8", &lv_font_montserrat_8 },
#endif
#if LV_FONT_MONTSERRAT_10
    { "MONTSERRAT_10", &lv_font_montserrat_10 },
#endif
#if LV_FONT_MONTSERRAT_12
    { "MONTSERRAT_12", &lv_font_montserrat_12 },
#endif
#if LV_FONT_MONTSERRAT_14
    { "MONTSERRAT_14", &lv_font_montserrat_14 },
#endif
#if LV_FONT_MONTSERRAT_16
    { "MONTSERRAT_16", &lv_font_montserrat_16 },
#endif
#if LV_FONT_MONTSERRAT_18
    { "MONTSERRAT_18", &lv_font_montserrat_18 },
#endif
#if LV_FONT_MONTSERRAT_20
    { "MONTSERRAT_20", &lv_font_montserrat_20 },
#endif
#if LV_FONT_MONTSERRAT_22
    { "MONTSERRAT_22", &lv_font_montserrat_22 },
#endif
#if LV_FONT_MONTSERRAT_24
    { "MONTSERRAT_24", &lv_font_montserrat_24 },
#endif
#if LV_FONT_MONTSERRAT_26
    { "MONTSERRAT_26", &lv_font_montserrat_26 },
#endif
#if LV_FONT_MONTSERRAT_28
    { "MONTSERRAT_28", &lv_font_montserrat_28 },
#endif
#if LV_FONT_MONTSERRAT_30
    { "MONTSERRAT_30", &lv_font_montserrat_30 },
#endif
#if LV_FONT_MONTSERRAT_32
    { "MONTSERRAT_32", &lv_font_montserrat_32 },
#endif
#if LV_FONT_MONTSERRAT_34
    { "MONTSERRAT_34", &lv_font_montserrat_34 },
#endif
#if LV_FONT_MONTSERRAT_36
    { "MONTSERRAT_36", &lv_font_montserrat_36 },
#endif
#if LV_FONT_MONTSERRAT_38
    { "MONTSERRAT_38", &lv_font_montserrat_38 },
#endif
#if LV_FONT_MONTSERRAT_40
    { "MONTSERRAT_40", &lv_font_montserrat_40 },
#endif
#if LV_FONT_MONTSERRAT_42
    { "MONTSERRAT_42", &lv_font_montserrat_42 },
#endif
#if LV_FONT_MONTSERRAT_44
    { "MONTSERRAT_44", &lv_font_montserrat_44 },
#endif
#if LV_FONT_MONTSERRAT_46
    { "MONTSERRAT_46", &lv_font_montserrat_46 },
#endif
#if LV_FONT_MONTSERRAT_48
    { "MONTSERRAT_48", &lv_font_montserrat_48 },
#endif
};

//
// Color themes
//

uint32_t active_theme_index = 0;

//
//
//

void create_screens() {

// Set default LVGL theme
    lv_display_t *dispp = lv_display_get_default();
    lv_theme_t *theme = lv_theme_default_init(dispp, lv_palette_main(LV_PALETTE_BLUE), lv_palette_main(LV_PALETTE_RED), true, LV_FONT_DEFAULT);
    lv_display_set_theme(dispp, theme);
    
    // Initialize screens
    // Create screens
    create_screen_main();
    create_screen_page_preset_list();
    create_screen_page_settings();
}
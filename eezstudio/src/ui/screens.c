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
    lv_obj_set_size(obj, 1024, 600);
    {
        lv_obj_t *parent_obj = obj;
        {
            // lbl_title_version
            lv_obj_t *obj = lv_label_create(parent_obj);
            objects.lbl_title_version = obj;
            lv_obj_set_pos(obj, 0, 0);
            lv_obj_set_size(obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
            lv_obj_set_style_text_font(obj, &lv_font_montserrat_30, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text_static(obj, "Espressynth 1.0.0");
        }
        {
            // btn_panic
            lv_obj_t *obj = lv_button_create(parent_obj);
            objects.btn_panic = obj;
            lv_obj_set_pos(obj, 823, 8);
            lv_obj_set_size(obj, 187, 94);
            lv_obj_set_style_bg_color(obj, lv_color_hex(0xf32121), LV_PART_MAIN | LV_STATE_DEFAULT);
            {
                lv_obj_t *parent_obj = obj;
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    lv_obj_set_pos(obj, 0, 0);
                    lv_obj_set_size(obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
                    lv_obj_set_style_align(obj, LV_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text_static(obj, "PANIC!");
                }
            }
        }
        {
            // btn_next_preset
            lv_obj_t *obj = lv_button_create(parent_obj);
            objects.btn_next_preset = obj;
            lv_obj_set_pos(obj, 851, 422);
            lv_obj_set_size(obj, 177, 133);
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
            lv_obj_set_pos(obj, 5, 457);
            lv_obj_set_size(obj, 177, 133);
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
            // lbl_preset_name
            lv_obj_t *obj = lv_label_create(parent_obj);
            objects.lbl_preset_name = obj;
            lv_obj_set_pos(obj, 412, 329);
            lv_obj_set_size(obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
            lv_obj_set_style_text_font(obj, &lv_font_montserrat_30, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text_static(obj, "lbl_preset_name");
        }
        {
            // lbl_preset_number
            lv_obj_t *obj = lv_label_create(parent_obj);
            objects.lbl_preset_number = obj;
            lv_obj_set_pos(obj, 395, 362);
            lv_obj_set_size(obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
            lv_obj_set_style_text_font(obj, &lv_font_montserrat_30, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text_static(obj, "lbl_preset_number");
        }
        {
            // lbl_psram_info
            lv_obj_t *obj = lv_label_create(parent_obj);
            objects.lbl_psram_info = obj;
            lv_obj_set_pos(obj, 26, 102);
            lv_obj_set_size(obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
            lv_obj_set_style_text_font(obj, &lv_font_montserrat_18, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text_static(obj, "lbl_psram_info");
        }
        {
            // lbl_sysram_info
            lv_obj_t *obj = lv_label_create(parent_obj);
            objects.lbl_sysram_info = obj;
            lv_obj_set_pos(obj, 23, 123);
            lv_obj_set_size(obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
            lv_obj_set_style_text_font(obj, &lv_font_montserrat_18, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text_static(obj, "lbl_sysram_info");
        }
        {
            // lbl_core0_util
            lv_obj_t *obj = lv_label_create(parent_obj);
            objects.lbl_core0_util = obj;
            lv_obj_set_pos(obj, 717, 113);
            lv_obj_set_size(obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
            lv_obj_set_style_text_font(obj, &lv_font_montserrat_18, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text_static(obj, "lbl_core0_util");
        }
        {
            // lbl_core1_util
            lv_obj_t *obj = lv_label_create(parent_obj);
            objects.lbl_core1_util = obj;
            lv_obj_set_pos(obj, 718, 134);
            lv_obj_set_size(obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
            lv_obj_set_style_text_font(obj, &lv_font_montserrat_18, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text_static(obj, "lbl_core1_util");
        }
        {
            // lbl_used_voices
            lv_obj_t *obj = lv_label_create(parent_obj);
            objects.lbl_used_voices = obj;
            lv_obj_set_pos(obj, 23, 164);
            lv_obj_set_size(obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
            lv_obj_set_style_text_font(obj, &lv_font_montserrat_18, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text_static(obj, "lbl_used_voices");
        }
        {
            // lbl_underruns
            lv_obj_t *obj = lv_label_create(parent_obj);
            objects.lbl_underruns = obj;
            lv_obj_set_pos(obj, 24, 185);
            lv_obj_set_size(obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
            lv_obj_set_style_text_font(obj, &lv_font_montserrat_18, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text_static(obj, "lbl_underruns");
        }
        {
            // lbl_midi_devicename
            lv_obj_t *obj = lv_label_create(parent_obj);
            objects.lbl_midi_devicename = obj;
            lv_obj_set_pos(obj, 67, 63);
            lv_obj_set_size(obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
            lv_obj_set_style_text_font(obj, &lv_font_montserrat_18, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text_static(obj, "lbl_midi_devicename");
        }
        {
            // bar_core0_util
            lv_obj_t *obj = lv_bar_create(parent_obj);
            objects.bar_core0_util = obj;
            lv_obj_set_pos(obj, 859, 119);
            lv_obj_set_size(obj, 150, 10);
            lv_bar_set_value(obj, 25, LV_ANIM_OFF);
        }
        {
            // bar_core1_util
            lv_obj_t *obj = lv_bar_create(parent_obj);
            objects.bar_core1_util = obj;
            lv_obj_set_pos(obj, 860, 141);
            lv_obj_set_size(obj, 150, 10);
            lv_bar_set_value(obj, 25, LV_ANIM_OFF);
        }
        {
            // lbl_ip_address
            lv_obj_t *obj = lv_label_create(parent_obj);
            objects.lbl_ip_address = obj;
            lv_obj_set_pos(obj, 896, 569);
            lv_obj_set_size(obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
            lv_obj_set_style_text_font(obj, &lv_font_montserrat_18, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text_static(obj, "lbl_ip_address");
        }
        {
            // bar_preset_loader
            lv_obj_t *obj = lv_bar_create(parent_obj);
            objects.bar_preset_loader = obj;
            lv_obj_set_pos(obj, 401, 422);
            lv_obj_set_size(obj, 273, 48);
            lv_bar_set_value(obj, 25, LV_ANIM_OFF);
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
    lv_obj_set_size(obj, 1024, 600);
    {
        lv_obj_t *parent_obj = obj;
        {
            // tbl_presets
            lv_obj_t *obj = lv_table_create(parent_obj);
            objects.tbl_presets = obj;
            lv_obj_set_pos(obj, 0, 77);
            lv_obj_set_size(obj, 1022, 523);
        }
        {
            // btn_back
            lv_obj_t *obj = lv_button_create(parent_obj);
            objects.btn_back = obj;
            lv_obj_set_pos(obj, 0, 0);
            lv_obj_set_size(obj, 188, 77);
            {
                lv_obj_t *parent_obj = obj;
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    lv_obj_set_pos(obj, 0, 0);
                    lv_obj_set_size(obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
                    lv_obj_set_style_align(obj, LV_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text_static(obj, "<- BACK");
                }
            }
        }
        {
            // lbl_title_presets
            lv_obj_t *obj = lv_label_create(parent_obj);
            objects.lbl_title_presets = obj;
            lv_obj_set_pos(obj, 232, 23);
            lv_obj_set_size(obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
            lv_obj_set_style_text_font(obj, &lv_font_montserrat_30, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text_static(obj, "Presets");
        }
    }
    
    tick_screen_page_preset_list();
}

void tick_screen_page_preset_list() {
}

void create_user_widget_knob(lv_obj_t *parent_obj, int startWidgetIndex) {
    (void)startWidgetIndex;
    lv_obj_t *obj = parent_obj;
    {
        lv_obj_t *parent_obj = obj;
        {
            // arcKnob
            lv_obj_t *obj = lv_arc_create(parent_obj);
            ((lv_obj_t **)&objects)[startWidgetIndex + 0] = obj;
            lv_obj_set_pos(obj, 0, 0);
            lv_obj_set_size(obj, 150, 150);
            lv_arc_set_value(obj, 25);
        }
        {
            // lblknobvalue
            lv_obj_t *obj = lv_label_create(parent_obj);
            ((lv_obj_t **)&objects)[startWidgetIndex + 1] = obj;
            lv_obj_set_pos(obj, 0, 53);
            lv_obj_set_size(obj, 150, 37);
            lv_label_set_long_mode(obj, LV_LABEL_LONG_CLIP);
            lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_font(obj, &lv_font_montserrat_36, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text(obj, "lblknobvalue");
        }
        {
            // lblknobdevice
            lv_obj_t *obj = lv_label_create(parent_obj);
            ((lv_obj_t **)&objects)[startWidgetIndex + 2] = obj;
            lv_obj_set_pos(obj, 1, 159);
            lv_obj_set_size(obj, 149, 17);
            lv_label_set_long_mode(obj, LV_LABEL_LONG_CLIP);
            lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_font(obj, &lv_font_montserrat_16, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text(obj, "lblknobdevice");
        }
        {
            // lblknobport
            lv_obj_t *obj = lv_label_create(parent_obj);
            ((lv_obj_t **)&objects)[startWidgetIndex + 3] = obj;
            lv_obj_set_pos(obj, 2, 177);
            lv_obj_set_size(obj, 148, 17);
            lv_label_set_long_mode(obj, LV_LABEL_LONG_CLIP);
            lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_font(obj, &lv_font_montserrat_16, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text(obj, "lblknobport");
        }
        {
            // lblknobname
            lv_obj_t *obj = lv_label_create(parent_obj);
            ((lv_obj_t **)&objects)[startWidgetIndex + 4] = obj;
            lv_obj_set_pos(obj, 1, 134);
            lv_obj_set_size(obj, 149, 17);
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

typedef void (*tick_screen_func_t)();
tick_screen_func_t tick_screen_funcs[] = {
    tick_screen_main,
    tick_screen_page_preset_list,
};
void tick_screen(int screen_index) {
    if (screen_index >= 0 && screen_index < 2) {
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
}
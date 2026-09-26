#include "eez_ui_bridge.hpp"

#include "app_core.hpp"
#include "boards/board_config.hpp"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "lvgl.h"

extern "C" {
#include "actions.h"
#include "screens.h"
#include "ui.h"
}

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace {

lv_timer_t *s_timer = nullptr;
EezPresetList s_preset_list{};

void action_open_preset_list(lv_event_t *)
{
    Feature::EEZUI::show_preset_list();
}

void action_back_to_main(lv_event_t *)
{
    loadScreen(SCREEN_ID_MAIN);
}

void action_select_preset(lv_event_t *)
{
    uint32_t row = LV_TABLE_CELL_NONE;
    uint32_t column = LV_TABLE_CELL_NONE;
    lv_table_get_selected_cell(objects.tbl_presets, &row, &column);
    (void)column;
    if (row == LV_TABLE_CELL_NONE || row == 0 || row > s_preset_list.count) return;
    if (eez_synth_load_preset(s_preset_list.entries[row - 1].file_name)) loadScreen(SCREEN_ID_MAIN);
}

// lv_label_set_text() always invalidates (and so re-renders) the label, even for identical
// text - with a dozen status labels refreshed 8x per second that alone kept taskLVGL busy
// re-drawing unchanged pixels. Only touch a label when its text actually differs.
void set_label_text(lv_obj_t *label, const char *text)
{
    const char *current = lv_label_get_text(label);
    if (current == nullptr || strcmp(current, text) != 0) lv_label_set_text(label, text);
}

__attribute__((format(printf, 2, 3))) void set_label_fmt(lv_obj_t *label, const char *fmt, ...)
{
    char text[96];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    set_label_text(label, text);
}

void update_runtime_labels()
{
    EezSynthStatus synth{};
    eez_synth_status(&synth);
    set_label_text(objects.lbl_midi_devicename, synth.midi_name[0] != '\0' ? synth.midi_name : "Kein MIDI");

    // Preset loader bar: every timer tick, so the progress animation stays smooth.
    if (synth.preset_load_active) {
        lv_obj_clear_flag(objects.bar_preset_loader, LV_OBJ_FLAG_HIDDEN);
        lv_bar_set_value(objects.bar_preset_loader, synth.preset_load_progress, LV_ANIM_ON);
    } else {
        lv_obj_add_flag(objects.bar_preset_loader, LV_OBJ_FLAG_HIDDEN);
    }
    if (synth.preset_name[0] != '\0') set_label_text(objects.lbl_preset_name, synth.preset_name);
    if (synth.preset_count != 0)
        set_label_fmt(objects.lbl_preset_number, "Preset %u von %u", static_cast<unsigned>(synth.preset_number),
                      static_cast<unsigned>(synth.preset_count));
    else
        set_label_text(objects.lbl_preset_number, "Preset --");
    set_label_fmt(objects.lbl_used_voices, "Voices: %u / %u", synth.active_voices, synth.maximum_voices);
    set_label_fmt(objects.lbl_underruns, "Underruns: %lu", static_cast<unsigned long>(synth.underruns));

    // Load/memory/IP readouts: twice per second - they change on nearly every sample, so the
    // text comparison above wouldn't save their redraws at a faster rate. Also gives the
    // Core 0 figure a stable 500 ms averaging window.
    static int64_t previous_sample_us = 0;
    static uint32_t previous_idle_runtime = 0;
    const int64_t now_us = esp_timer_get_time();
    if (previous_sample_us != 0 && now_us - previous_sample_us < 500000) return;
    const uint32_t idle_runtime = ulTaskGetIdleRunTimeCounterForCore(App::MAIN_CORE);
    float core0 = 0.0f;
    if (previous_sample_us != 0) {
        core0 = 100.0f * (1.0f - static_cast<float>(idle_runtime - previous_idle_runtime) /
                                   static_cast<float>(now_us - previous_sample_us));
        if (core0 < 0.0f) core0 = 0.0f;
        if (core0 > 100.0f) core0 = 100.0f;
    }
    previous_sample_us = now_us;
    previous_idle_runtime = idle_runtime;

    const unsigned psram_free = static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);
    const unsigned psram_total = static_cast<unsigned>(heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / 1024);
    const unsigned sysram_free = static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024);
    const unsigned sysram_total = static_cast<unsigned>(heap_caps_get_total_size(MALLOC_CAP_INTERNAL) / 1024);
    set_label_fmt(objects.lbl_psram_info, "PSRAM: %u / %u KB", psram_free, psram_total);
    set_label_fmt(objects.lbl_sysram_info, "System: %u / %u KB", sysram_free, sysram_total);

    char ip_address[24] = {};
    eez_network_ip(ip_address, sizeof(ip_address));
    set_label_fmt(objects.lbl_ip_address, "IP: %s", ip_address);
    set_label_fmt(objects.lbl_core0_util, "Core 0: %u %%", static_cast<unsigned>(core0 + 0.5f));
    set_label_fmt(objects.lbl_core1_util, "Core 1: %u %%", static_cast<unsigned>(synth.audio_core_load + 0.5f));
    lv_bar_set_value(objects.bar_core0_util, static_cast<int32_t>(core0 + 0.5f), LV_ANIM_OFF);
    lv_bar_set_value(objects.bar_core1_util, static_cast<int32_t>(synth.audio_core_load + 0.5f), LV_ANIM_OFF);
}

void timer_callback(lv_timer_t *)
{
    update_runtime_labels();
    ui_tick();
}

} // namespace

extern "C" void action_panic(lv_event_t *)
{
    eez_synth_panic();
}

extern "C" void action_next_preset(lv_event_t *)
{
    eez_synth_step_preset(1);
}

extern "C" void action_previous_preset(lv_event_t *)
{
    eez_synth_step_preset(-1);
}

namespace Feature::EEZUI {

void init()
{
    ui_init();
    lv_bar_set_range(objects.bar_preset_loader, 0, 100);
    lv_obj_add_flag(objects.bar_preset_loader, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text_fmt(objects.lbl_title_version, "Espressynth - %s", CurrentBoard::BOARD_NAME);
    lv_obj_add_event_cb(objects.btn_panic, action_panic, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(objects.btn_next_preset, action_next_preset, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(objects.btn_previous_preset, action_previous_preset, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_flag(objects.lbl_preset_name, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(objects.lbl_preset_number, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(objects.lbl_preset_name, action_open_preset_list, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(objects.lbl_preset_number, action_open_preset_list, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(objects.tbl_presets, action_select_preset, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(objects.btn_back, action_back_to_main, LV_EVENT_CLICKED, nullptr);
}

void show_preset_list()
{
    if (!eez_list_presets(&s_preset_list)) return;
    lv_obj_t *table = objects.tbl_presets;
    lv_table_set_row_count(table, s_preset_list.count + 1);
    lv_table_set_column_count(table, 4);
    lv_table_set_column_width(table, 0, 55);
    lv_table_set_column_width(table, 1, 240);
    lv_table_set_column_width(table, 2, 470);
    lv_table_set_column_width(table, 3, 250);
    lv_table_set_cell_value(table, 0, 0, "#");
    lv_table_set_cell_value(table, 0, 1, "Preset");
    lv_table_set_cell_value(table, 0, 2, "Short Description");
    lv_table_set_cell_value(table, 0, 3, "Author");
    for (uint16_t i = 0; i < s_preset_list.count; ++i) {
        const EezPresetEntry *preset = &s_preset_list.entries[i];
        lv_table_set_cell_value_fmt(table, i + 1, 0, "%u", static_cast<unsigned>(i + 1));
        lv_table_set_cell_value(table, i + 1, 1, preset->display_name);
        lv_table_set_cell_value(table, i + 1, 2, preset->description);
        lv_table_set_cell_value(table, i + 1, 3, preset->author);
    }
    loadScreen(SCREEN_ID_PAGE_PRESET_LIST);
}

void start()
{
    if (s_timer != nullptr) return;
    update_runtime_labels();
    s_timer = lv_timer_create(timer_callback, 100, nullptr);
}

} // namespace Feature::EEZUI

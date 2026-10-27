#include "eez_ui_bridge.hpp"

#include "app_core.hpp"
#include "version.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "MemoryDiagnostics.h"
#include "KnobActivityOverlay.h"
#include "PageControlsView.h"

extern "C" {
#include "actions.h"
#include "screens.h"
#include "ui.h"
}

#include <cstdarg>
#include <atomic>
#include <cstdio>
#include <cstring>

namespace {

lv_timer_t *s_timer = nullptr;
// PSRAM, allocated on first use: 8.7 KB that only the preset table reads.
EezPresetList &preset_list()
{
    static auto *list = static_cast<EezPresetList *>(heap_caps_calloc(1, sizeof(EezPresetList), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    return *list;
}
EezTabList *s_tab_list = nullptr; // allocated in PSRAM on first use
lv_obj_t *s_pages_box = nullptr;  // open Pages popup, closed again on a preset change
lv_obj_t *s_page_loading = nullptr;
char s_page_id[sizeof(EezTabList::ids[0])] = {}; // page shown in wndmain, "" = none
PageControlsView s_page_view;
KnobActivityOverlay s_knob_overlay;
uint32_t s_knob_preset_sequence = UINT32_MAX;
EezPageControls *s_cached_pages[EEZ_TAB_CAPACITY] = {};
bool s_page_requested[EEZ_TAB_CAPACITY] = {};
bool s_page_fetching = false, s_tabs_fetching = false, s_presets_ready = false;
int64_t s_presets_loaded_us = 0;

int cached_page_index(const char *pageId)
{
    if (s_tab_list != nullptr)
        for (uint16_t i = 0; i < s_tab_list->count; ++i)
            if (strcmp(s_tab_list->ids[i], pageId) == 0) return i;
    return -1;
}

void clear_page_cache()
{
    s_page_view.clear();
    for (auto *&page : s_cached_pages) {
        heap_caps_free(page);
        page = nullptr;
    }
    memset(s_page_requested, 0, sizeof(s_page_requested));
    heap_caps_free(s_tab_list);
    s_tab_list = nullptr;
    s_page_fetching = s_tabs_fetching = false;
}

enum class DataKind : uint8_t { Page, Tabs, Presets };
struct DataRequest {
    DataKind kind;
    uint32_t generation;
    uint32_t presetSequence;
    char pageId[sizeof(s_page_id)];
};
struct DataResult {
    DataRequest request;
    void *data;
};
QueueHandle_t s_data_requests = nullptr, s_data_results = nullptr;
std::atomic<uint32_t> s_data_generation[3]{};
DataRequest s_pending_data[3]{};
bool s_data_pending[3]{};

// UI owns pending requests. Queue writes never block LVGL; newer selections replace
// requests not yet sent. The worker only owns file I/O and its result allocation.
bool request_data(DataKind kind, const char *pageId = nullptr)
{
    if (s_data_requests == nullptr) return false;
    const unsigned index = static_cast<unsigned>(kind);
    auto &request = s_pending_data[index];
    request = {kind, ++s_data_generation[index], s_knob_preset_sequence, {}};
    snprintf(request.pageId, sizeof(request.pageId), "%s", pageId != nullptr ? pageId : s_page_id);
    s_data_pending[index] = true;
    if (kind == DataKind::Page) s_page_fetching = true;
    if (kind == DataKind::Tabs) s_tabs_fetching = true;
    return true;
}

void prefetch_pages()
{
    if (s_tab_list == nullptr || s_page_fetching) return;
    for (uint16_t i = 0; i < s_tab_list->count; ++i)
        if (s_cached_pages[i] == nullptr && !s_page_requested[i]) {
            if (request_data(DataKind::Page, s_tab_list->ids[i])) s_page_requested[i] = true;
            return;
        }
}

void dispatch_data_requests()
{
    if (s_data_requests == nullptr) return;
    for (unsigned i = 0; i < 3; ++i)
        if (s_data_pending[i] && xQueueSend(s_data_requests, &s_pending_data[i], 0) == pdTRUE)
            s_data_pending[i] = false;
}

void data_worker(void *)
{
    DataRequest request{};
    for (;;) {
        xQueueReceive(s_data_requests, &request, portMAX_DELAY);
        const unsigned index = static_cast<unsigned>(request.kind);
        if (request.generation != s_data_generation[index].load()) continue;
        const size_t size = request.kind == DataKind::Page ? sizeof(EezPageControls)
                          : request.kind == DataKind::Tabs ? sizeof(EezTabList) : sizeof(EezPresetList);
        void *data = heap_caps_calloc(1, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        bool loaded = false;
        if (data != nullptr) {
            switch (request.kind) {
            case DataKind::Page: loaded = eez_list_page_controls(request.pageId, static_cast<EezPageControls *>(data)); break;
            case DataKind::Tabs: loaded = eez_list_tabs(static_cast<EezTabList *>(data)); break;
            case DataKind::Presets: loaded = eez_list_presets(static_cast<EezPresetList *>(data)); break;
            }
        }
        if (!loaded) {
            heap_caps_free(data);
            data = nullptr;
            ESP_LOGW("EEZUI", "Could not load display data (kind %u)", index);
        }
        DataResult result{request, data};
        // Only this worker may wait for result capacity. LVGL always polls with zero timeout.
        xQueueSend(s_data_results, &result, portMAX_DELAY);
    }
}

void action_open_preset_list(lv_event_t *)
{
    Feature::EEZUI::show_preset_list();
}

void action_back_to_main(lv_event_t *)
{
    lv_obj_add_flag(lv_win_get_header(objects.wndmain), LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(objects.tbl_presets, LV_OBJ_FLAG_HIDDEN);
    s_page_view.setHidden(false);
    if (s_page_loading != nullptr) lv_obj_remove_flag(s_page_loading, LV_OBJ_FLAG_HIDDEN);
}

// (Re)builds the controls of s_page_id in wndmain from the active preset.
void show_page()
{
    if (s_page_id[0] == '\0') return;
    const int index = cached_page_index(s_page_id);
    if (index >= 0 && s_cached_pages[index] != nullptr) {
        if (s_page_loading != nullptr) lv_obj_delete(s_page_loading);
        s_page_loading = nullptr;
        s_page_view.show(lv_win_get_content(objects.wndmain), *s_cached_pages[index], s_page_id);
        s_page_view.setHidden(!lv_obj_has_flag(objects.tbl_presets, LV_OBJ_FLAG_HIDDEN));
        return;
    }
    // Old controls must not send parameters to a newly selected preset while loading.
    s_page_view.setHidden(true);
    if (s_page_loading == nullptr)
        s_page_loading = lv_label_create(lv_win_get_content(objects.wndmain));
    lv_label_set_text(s_page_loading, request_data(DataKind::Page) ? "Lade Seite..." : "Seite konnte nicht geladen werden");
    if (!lv_obj_has_flag(objects.tbl_presets, LV_OBJ_FLAG_HIDDEN)) lv_obj_add_flag(s_page_loading, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(s_page_loading, LV_OBJ_FLAG_HIDDEN);
}

void action_select_page(lv_event_t *e)
{
    const auto index = reinterpret_cast<uintptr_t>(lv_event_get_user_data(e));
    if (s_tab_list == nullptr || index >= s_tab_list->count) return;
    snprintf(s_page_id, sizeof(s_page_id), "%s", s_tab_list->ids[index]);
    show_page();
    // Async: this handler runs on a button inside the box being closed.
    if (s_pages_box != nullptr) lv_msgbox_close_async(s_pages_box);
}

void populate_pages();

void action_open_pages(lv_event_t *)
{
    if (s_pages_box != nullptr) return;
    lv_obj_t *box = lv_msgbox_create(nullptr);
    s_pages_box = box;
    lv_obj_add_event_cb(box, [](lv_event_t *) { s_pages_box = nullptr; }, LV_EVENT_DELETE, nullptr);
    lv_obj_set_width(box, 560);
    lv_obj_set_style_text_font(lv_msgbox_add_title(box, "Pages"), &lv_font_montserrat_18, 0);
    // Tapping the dimmed backdrop (outside the box) closes the popup.
    lv_obj_add_event_cb(lv_obj_get_parent(box), [](lv_event_t *e) {
        if (lv_event_get_target(e) == lv_event_get_current_target(e))
            lv_msgbox_close(static_cast<lv_obj_t *>(lv_event_get_user_data(e)));
    }, LV_EVENT_CLICKED, box);

    lv_obj_t *content = lv_msgbox_get_content(box);
    lv_obj_set_scroll_dir(content, LV_DIR_VER);
    if (s_tab_list != nullptr) populate_pages();
    else lv_label_set_text(lv_label_create(content), (s_tabs_fetching || request_data(DataKind::Tabs)) ? "Lade Seiten..." : "Seiten konnten nicht geladen werden");
}

void populate_pages()
{
    if (s_pages_box == nullptr) return;
    lv_obj_t *content = lv_msgbox_get_content(s_pages_box);
    lv_obj_clean(content);
    lv_obj_t *list = lv_list_create(content);
    lv_obj_set_size(list, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(list, 460, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    for (uint16_t i = 0; i < s_tab_list->count; ++i) {
        lv_obj_t *button = lv_list_add_button(list, nullptr, s_tab_list->titles[i]);
        lv_obj_set_height(button, 60);
        lv_obj_set_style_text_font(button, &lv_font_montserrat_18, 0);
        lv_obj_add_event_cb(button, action_select_page, LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<uintptr_t>(i)));
    }
}

void populate_presets()
{
    lv_obj_t *table = objects.tbl_presets;
    lv_table_set_row_count(table, preset_list().count + 1);
    lv_table_set_column_count(table, 4);
    lv_table_set_column_width(table, 0, 55);
    lv_table_set_column_width(table, 1, 240);
    lv_table_set_column_width(table, 2, 470);
    lv_table_set_column_width(table, 3, 250);
    lv_table_set_cell_value(table, 0, 0, "#");
    lv_table_set_cell_value(table, 0, 1, "Preset");
    lv_table_set_cell_value(table, 0, 2, "Short Description");
    lv_table_set_cell_value(table, 0, 3, "Author");
    for (uint16_t i = 0; i < preset_list().count; ++i) {
        const EezPresetEntry *preset = &preset_list().entries[i];
        lv_table_set_cell_value_fmt(table, i + 1, 0, "%u", static_cast<unsigned>(i + 1));
        lv_table_set_cell_value(table, i + 1, 1, preset->display_name);
        lv_table_set_cell_value(table, i + 1, 2, preset->description);
        lv_table_set_cell_value(table, i + 1, 3, preset->author);
    }
}

void apply_data_results()
{
    if (s_data_results == nullptr) return;
    DataResult result{};
    while (xQueueReceive(s_data_results, &result, 0) == pdTRUE) {
        const auto &request = result.request;
        const unsigned index = static_cast<unsigned>(request.kind);
        if ((request.kind != DataKind::Page && request.generation != s_data_generation[index].load()) ||
            request.presetSequence != s_knob_preset_sequence) {
            heap_caps_free(result.data);
            continue;
        }
        switch (request.kind) {
        case DataKind::Page:
            if (request.generation == s_data_generation[index].load()) s_page_fetching = false;
            if (result.data != nullptr) {
                auto *controls = static_cast<EezPageControls *>(result.data);
                const int pageIndex = cached_page_index(request.pageId);
                if (pageIndex >= 0) {
                    heap_caps_free(s_cached_pages[pageIndex]);
                    s_cached_pages[pageIndex] = controls;
                    s_page_requested[pageIndex] = true;
                    result.data = nullptr; // PSRAM cache owns the loaded page.
                }
                if (strcmp(request.pageId, s_page_id) == 0) {
                    if (s_page_loading != nullptr) lv_obj_delete(s_page_loading);
                    s_page_loading = nullptr;
                    s_page_view.show(lv_win_get_content(objects.wndmain), *controls, s_page_id);
                    s_page_view.setHidden(!lv_obj_has_flag(objects.tbl_presets, LV_OBJ_FLAG_HIDDEN));
                } else if (pageIndex >= 0) {
                    s_page_view.prepare(lv_win_get_content(objects.wndmain), *controls, request.pageId);
                }
            } else if (strcmp(request.pageId, s_page_id) == 0 && s_page_loading != nullptr) lv_label_set_text(s_page_loading, "Seite konnte nicht geladen werden");
            break;
        case DataKind::Tabs:
            s_tabs_fetching = false;
            heap_caps_free(s_tab_list);
            s_tab_list = static_cast<EezTabList *>(result.data);
            result.data = nullptr; // Keep tab IDs alive for popup button callbacks.
            if (s_tab_list != nullptr) populate_pages();
            else if (s_pages_box != nullptr) {
                lv_obj_t *content = lv_msgbox_get_content(s_pages_box);
                lv_obj_clean(content);
                lv_label_set_text(lv_label_create(content), "Seiten konnten nicht geladen werden");
            }
            break;
        case DataKind::Presets:
            if (result.data != nullptr) {
                const bool changed = !s_presets_ready || memcmp(&preset_list(), result.data, sizeof(preset_list())) != 0;
                preset_list() = *static_cast<EezPresetList *>(result.data);
                s_presets_ready = true;
                s_presets_loaded_us = esp_timer_get_time();
                if (changed) populate_presets();
            } else if (!s_presets_ready) {
                preset_list().count = 0;
                lv_table_set_cell_value(objects.tbl_presets, 0, 0, "Presets konnten nicht geladen werden");
            }
            break;
        }
        heap_caps_free(result.data);
    }
}

void action_select_preset(lv_event_t *)
{
    uint32_t row = LV_TABLE_CELL_NONE;
    uint32_t column = LV_TABLE_CELL_NONE;
    lv_table_get_selected_cell(objects.tbl_presets, &row, &column);
    (void)column;
    if (row == LV_TABLE_CELL_NONE || row == 0 || row > preset_list().count) return;
    if (eez_synth_load_preset(preset_list().entries[row - 1].file_name)) action_back_to_main(nullptr);
}

// lv_label_set_text() always invalidates (and so re-renders) the label, even for identical
// text - with a dozen status labels refreshed every tick that alone kept taskLVGL busy
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

void update_control_activity(uint32_t presetSequence)
{
    // Bound queue processing so MIDI bursts leave time for touch and rendering.
    // Remaining display notifications stay queued; audio/button processing is independent.
    constexpr int64_t kActivityBudgetUs = 2000;
    const int64_t activityNowUs = esp_timer_get_time();
    KnobActivity activity{};
    for (uint16_t i = 0; i < 512 &&
         (i == 0 || esp_timer_get_time() - activityNowUs < kActivityBudgetUs) &&
         eez_read_knob_activity(&activity); ++i)
    {
        if (activity.presetSequence != presetSequence) continue;
        // Only state changes in this loop; each page card refreshes once afterwards.
        if ((activity.isEnvelope || !s_page_view.apply(activity, true)) && activityNowUs - activity.updatedUs < activity.durationUs())
            s_knob_overlay.update(activity, true);
    }
    s_page_view.tick();
    s_knob_overlay.tick(esp_timer_get_time());
}

void update_runtime_labels()
{
    EezSynthStatus synth{};
    eez_synth_status(&synth);
    if (s_knob_preset_sequence != synth.preset_change_sequence)
    {
        s_knob_overlay.clear();
        clear_page_cache();
        s_knob_preset_sequence = synth.preset_change_sequence;
        if (s_pages_box != nullptr) lv_msgbox_close(s_pages_box);
        show_page(); // same page of the new preset (empty if it has no such page)
        request_data(DataKind::Tabs);
        request_data(DataKind::Presets);
    }
    apply_data_results();
    prefetch_pages();
    dispatch_data_requests();
    update_control_activity(synth.preset_change_sequence);
    set_label_text(objects.lbl_midi_devicename, synth.midi_name[0] != '\0' ? synth.midi_name : "Kein MIDI");

    if (synth.preset_name[0] != '\0')
        set_label_fmt(objects.lbl_preset_name, "%u - %s", static_cast<unsigned>(synth.preset_number), synth.preset_name);
    set_label_fmt(objects.lbl_used_voices, "Voices: %u / %u", synth.active_voices, synth.maximum_voices);
    set_label_fmt(objects.lbl_underruns, "Underruns: %lu", static_cast<unsigned long>(synth.underruns));
    set_label_fmt(objects.lbl_bpm, "%.2f BPM", static_cast<double>(synth.master_clock_bpm));
    lv_bar_set_value(objects.bar_master_volume, static_cast<int32_t>(synth.master_volume * 100.0f + 0.5f), LV_ANIM_OFF);

    // Refresh CPU labels and bars together, averaging Core 0 over the elapsed interval. Every
    // redraw here measurably delays the audio render on core 1 (one Audio-Overrun per redraw at
    // 500 ms, likely LVGL evicting engine code from the shared L2 cache), so sample once per
    // second and redraw a core only when its value moved by at least kCpuDeadbandPercent.
    constexpr int kCpuDeadbandPercent = 3;
    static int64_t previous_sample_us = 0;
    static uint32_t previous_idle_runtime = 0;
    const int64_t now_us = esp_timer_get_time();
    if (previous_sample_us != 0 && now_us - previous_sample_us < 1000000) return;
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

    static int shown_core0 = -100;
    static int shown_core1 = -100;
    const int core0_percent = static_cast<int>(core0 + 0.5f);
    const int core1_percent = static_cast<int>(synth.audio_core_load + 0.5f);
    if (core0_percent - shown_core0 >= kCpuDeadbandPercent || shown_core0 - core0_percent >= kCpuDeadbandPercent) {
        shown_core0 = core0_percent;
        set_label_fmt(objects.lbl_core0_util, "%d %%", core0_percent);
        lv_bar_set_value(objects.bar_core0_util, core0_percent, LV_ANIM_OFF);
    }
    if (core1_percent - shown_core1 >= kCpuDeadbandPercent || shown_core1 - core1_percent >= kCpuDeadbandPercent) {
        shown_core1 = core1_percent;
        set_label_fmt(objects.lbl_core1_util, "%d %%", core1_percent);
        lv_bar_set_value(objects.bar_core1_util, core1_percent, LV_ANIM_OFF);
    }

    // Heap statistics and network details: every 5 s is plenty (same redraw cost as above).
    static int64_t previous_info_us = 0;
    if (previous_info_us != 0 && now_us - previous_info_us < 5000000) return;
    previous_info_us = now_us;

    // LVGL pool usage for /api/diagnostics/memory - must run on the LVGL task (we are).
    {
        lv_mem_monitor_t mon;
        lv_mem_monitor(&mon);
        memorydiag::setLvglStats(static_cast<uint32_t>(mon.total_size),
                                 static_cast<uint32_t>(mon.total_size - mon.free_size),
                                 static_cast<uint32_t>(mon.max_used), mon.frag_pct);
    }

    constexpr size_t mib = 1024 * 1024;
    const unsigned psram_free_tenths = static_cast<unsigned>((heap_caps_get_free_size(MALLOC_CAP_SPIRAM) * 10ULL + mib / 2) / mib);
    const unsigned psram_total_tenths = static_cast<unsigned>((heap_caps_get_total_size(MALLOC_CAP_SPIRAM) * 10ULL + mib / 2) / mib);
    const unsigned sysram_free = static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024);
    const unsigned sysram_total = static_cast<unsigned>(heap_caps_get_total_size(MALLOC_CAP_INTERNAL) / 1024);
    set_label_fmt(objects.lbl_psram_info, "PSRAM: %u.%u / %u.%u MB", psram_free_tenths / 10,
                  psram_free_tenths % 10, psram_total_tenths / 10, psram_total_tenths % 10);
    set_label_fmt(objects.lbl_sysram_info, "System: %u / %u KB", sysram_free, sysram_total);

    char ip_address[24] = {};
    eez_network_ip(ip_address, sizeof(ip_address));
    set_label_fmt(objects.lbl_ip_address, "IP: %s", ip_address);
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
    s_data_requests = xQueueCreate(3, sizeof(DataRequest));
    s_data_results = xQueueCreate(3, sizeof(DataResult));
    if (s_data_requests == nullptr || s_data_results == nullptr ||
        xTaskCreatePinnedToCore(data_worker, "uiStorage", 6 * 1024, nullptr, 2, nullptr, App::MAIN_CORE) != pdPASS) {
        if (s_data_requests != nullptr) vQueueDelete(s_data_requests);
        if (s_data_results != nullptr) vQueueDelete(s_data_results);
        s_data_requests = s_data_results = nullptr;
        ESP_LOGE("EEZUI", "Could not start display storage worker");
    }
    ui_init();
    s_knob_overlay.warmup();
    lv_obj_t *header = lv_win_get_header(objects.wndmain);
    lv_obj_t *content = lv_win_get_content(objects.wndmain);
    lv_obj_set_parent(objects.btn_back, header);
    lv_obj_set_parent(objects.lbl_title_presets, header);
    lv_obj_set_parent(objects.tbl_presets, content);
    lv_obj_set_height(header, 56);
    lv_obj_set_size(objects.btn_back, 150, LV_PCT(100));
    lv_obj_set_flex_grow(objects.lbl_title_presets, 1);
    lv_obj_set_style_pad_all(content, 0, 0);
    lv_obj_set_pos(objects.tbl_presets, 0, 0);
    lv_obj_set_size(objects.tbl_presets, LV_PCT(100), LV_PCT(100));
    action_back_to_main(nullptr);
    lv_label_set_text(objects.lbl_title_version, "ESPRESSYNTH " FIRMWARE_VERSION);
    lv_obj_add_event_cb(objects.btn_panic, action_panic, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(objects.btn_next_preset, action_next_preset, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(objects.btn_previous_preset, action_previous_preset, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_flag(objects.lbl_preset_name, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(objects.lbl_preset_name, action_open_preset_list, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(objects.tbl_presets, action_select_preset, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(objects.btn_back, action_back_to_main, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(objects.btn_pages, action_open_pages, LV_EVENT_CLICKED, nullptr);
    // Populate caches while the startup splash is still visible.
    s_timer = lv_timer_create(timer_callback, 33, nullptr);
}

void show_preset_list()
{
    lv_obj_t *table = objects.tbl_presets;
    if (!s_presets_ready) {
        lv_table_set_row_count(table, 1);
        lv_table_set_column_count(table, 1);
        lv_table_set_column_width(table, 0, 790);
        lv_table_set_cell_value(table, 0, 0, request_data(DataKind::Presets) ? "Lade Presets..." : "Presets konnten nicht geladen werden");
    } else if (esp_timer_get_time() - s_presets_loaded_us > 5000000) {
        request_data(DataKind::Presets); // Refresh stale metadata without hiding cached rows.
    }
    lv_obj_clear_flag(lv_win_get_header(objects.wndmain), LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(table, LV_OBJ_FLAG_HIDDEN);
    s_page_view.setHidden(true);
    if (s_page_loading != nullptr) lv_obj_add_flag(s_page_loading, LV_OBJ_FLAG_HIDDEN);
}

void start()
{
    if (s_timer != nullptr) return;
    update_runtime_labels();
    s_timer = lv_timer_create(timer_callback, 33, nullptr);
}

} // namespace Feature::EEZUI

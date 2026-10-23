#include "app_events.hpp"
#include "app_startup.hpp"
#include "features/display/eez_ui_bridge.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/ip4_addr.h"
#include "cJSON.h"
#include "esp_heap_caps.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <dirent.h>

#if defined(CONFIG_APP_ENABLE_SYNTH)
namespace {

bool read_json_string(const char *json, const char *field, char *out, size_t out_size) {
    if (out == nullptr || out_size == 0) return false;
    out[0] = '\0';
    char key[40] = {};
    snprintf(key, sizeof(key), "\"%s\"", field);
    const char *value = strstr(json, key);
    if (value == nullptr || (value = strchr(value + strlen(key), ':')) == nullptr) return false;
    while (*++value == ' ' || *value == '\t' || *value == '\r' || *value == '\n') {
    }
    if (*value++ != '\"') return false;

    size_t length = 0;
    while (*value != '\0' && *value != '\"' && length + 1 < out_size) {
        char c = *value++;
        if (c == '\\' && *value != '\0') {
            c = *value++;
            if (c == 'n' || c == 'r' || c == 't') c = ' ';
        }
        out[length++] = c;
    }
    out[length] = '\0';
    return *value == '\"';
}

void read_preset_metadata(const char *path, EezPresetEntry *entry) {
    static char json[4096];
    FILE *file = fopen(path, "rb");
    if (file == nullptr) return;
    const size_t length = fread(json, 1, sizeof(json) - 1, file);
    fclose(file);
    json[length] = '\0';

    char title[sizeof(entry->display_name)] = {};
    if (read_json_string(json, "title", title, sizeof(title)) && title[0] != '\0')
        snprintf(entry->display_name, sizeof(entry->display_name), "%s", title);
    read_json_string(json, "description", entry->description, sizeof(entry->description));
    read_json_string(json, "author", entry->author, sizeof(entry->author));
}

// Parses a single top-level array field straight out of the preset text, so the whole
// (up to 256 KB) preset never has to become a cJSON tree just to list its tabs.
cJSON *parse_json_array(const char *json, const char *field) {
    char key[24] = {};
    snprintf(key, sizeof(key), "\"%s\"", field);
    const char *value = strstr(json, key);
    if (value == nullptr) return nullptr;
    value += strlen(key);
    while (*value == ' ' || *value == '\t' || *value == '\r' || *value == '\n' || *value == ':') ++value;
    return *value == '[' ? cJSON_ParseWithOpts(value, nullptr, false) : nullptr;
}

char *read_text_file(const char *path) {
    FILE *file = fopen(path, "rb");
    if (file == nullptr) return nullptr;
    fseek(file, 0, SEEK_END);
    const long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    char *text = nullptr;
    if (size > 0 && static_cast<size_t>(size) <= synth::kMaxPresetBytes)
        text = static_cast<char *>(heap_caps_malloc(static_cast<size_t>(size) + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (text != nullptr) text[fread(text, 1, static_cast<size_t>(size), file)] = '\0';
    fclose(file);
    return text;
}

// Returns the end of the JSON object/array starting at `p` (one past its closing bracket).
const char *skip_json_container(const char *p, const char *end) {
    int depth = 0;
    bool in_string = false;
    for (; p < end; ++p) {
        if (in_string) {
            if (*p == '\\') ++p;
            else if (*p == '"') in_string = false;
        } else if (*p == '"') {
            in_string = true;
        } else if (*p == '{' || *p == '[') {
            ++depth;
        } else if ((*p == '}' || *p == ']') && --depth == 0) {
            return p + 1;
        }
    }
    return nullptr;
}

// Finds `"key":` inside [begin, end) and returns the start of its value.
const char *find_json_value(const char *begin, const char *end, const char *key) {
    char quoted[24] = {};
    const int length = snprintf(quoted, sizeof(quoted), "\"%s\"", key);
    for (const char *p = std::search(begin, end, quoted, quoted + length); p != end;
         p = std::search(p + 1, end, quoted, quoted + length)) {
        const char *value = p + length;
        while (value < end && (*value == ' ' || *value == '\t' || *value == '\r' || *value == '\n')) ++value;
        if (value >= end || *value != ':') continue;
        ++value;
        while (value < end && (*value == ' ' || *value == '\t' || *value == '\r' || *value == '\n')) ++value;
        return value;
    }
    return nullptr;
}

double json_number(const cJSON *object, const char *key, double fallback) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsNumber(item) ? item->valuedouble : fallback;
}

void add_widget_controls(const cJSON *widget, EezPageControls *out) {
    const char *type = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(widget, "type"));
    const char *id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(widget, "id"));
    if (type == nullptr || id == nullptr) return;
    const bool is_button = strcmp(type, "button") == 0;
    if (!is_button && strcmp(type, "knob") != 0) return;

    const char *label = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(widget, "label"));
    const cJSON *ui = cJSON_GetObjectItemCaseSensitive(widget, "ui");
    const cJSON *params = cJSON_GetObjectItemCaseSensitive(widget, "params");
    const size_t max_count = is_button ? synth::kButtonControlMax : synth::kKnobControlMax;
    const int count = static_cast<int>(json_number(params, is_button ? "buttonCount" : "knobCount", 1));
    const int layer_count = cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(params, "layers"));
    int layer = static_cast<int>(json_number(params, "activeLayer", 0));
    if (layer < 0 || layer >= layer_count) layer = 0;
    const cJSON *layer_entry = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(params, "layers"), layer);
    const cJSON *labels = cJSON_GetObjectItemCaseSensitive(layer_entry, "labels");
    const cJSON *entries = cJSON_GetObjectItemCaseSensitive(layer_entry, is_button ? "buttons" : "knobs");
    const cJSON *bindings = cJSON_GetObjectItemCaseSensitive(params, "bindings");

    for (int i = 0; i < count && static_cast<size_t>(i) < max_count && out->count < EEZ_PAGE_CONTROL_CAPACITY; ++i) {
        const cJSON *entry = cJSON_GetArrayItem(entries, i);
        EezPageControl &control = out->controls[out->count++];
        control = {};
        snprintf(control.widget_id, sizeof(control.widget_id), "%s", id);
        const char *widget_label = label != nullptr ? label : id;
        snprintf(control.widget_label, sizeof(control.widget_label), "%s", widget_label);
        const char *control_label = cJSON_GetStringValue(cJSON_GetArrayItem(labels, i));
        if (control_label != nullptr && control_label[0] != '\0')
            snprintf(control.name, sizeof(control.name), "%s", control_label);
        else if (count == 1)
            snprintf(control.name, sizeof(control.name), "%s", widget_label);
        else
            snprintf(control.name, sizeof(control.name), "%s %d", widget_label, i + 1);
        control.control_index = static_cast<uint8_t>(layer * max_count + i);
        control.is_button = is_button;
        control.ui_x = static_cast<int32_t>(json_number(ui, "x", 0));
        control.ui_y = static_cast<int32_t>(json_number(ui, "y", 0));

        const cJSON *list = cJSON_GetObjectItemCaseSensitive(entry, "valueList");
        const int list_count = std::min(cJSON_GetArraySize(list), static_cast<int>(synth::kValueListMax));
        if (is_button) {
            control.switch_mode = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(bindings, i), "switchMode"));
            control.was_above = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(entry, "wasAbove"));
            control.list_count = static_cast<uint8_t>(list_count);
            const int list_index = static_cast<int>(json_number(entry, "listIndex", 0));
            control.list_index = static_cast<uint8_t>(list_index >= 0 && list_index < list_count ? list_index : 0);
            // Same as ButtonControl::currentOutput().
            control.value = list_count >= 2
                                ? static_cast<float>(cJSON_GetArrayItem(list, control.list_index)->valuedouble)
                                : static_cast<float>(control.was_above ? json_number(entry, "value127", 10.0)
                                                                       : json_number(entry, "value0", 0.0));
        } else {
            const double raw = json_number(entry, "raw", 0);
            control.raw = static_cast<uint8_t>(raw < 0 ? 0 : raw > 127 ? 127 : raw + 0.5);
            nodes::knob::KnobControl knob{};
            knob.raw = control.raw;
            knob.point1Out = static_cast<float>(json_number(entry, "point1Out", 0));
            knob.point2Out = static_cast<float>(json_number(entry, "point2Out", 10));
            knob.valueListCount = static_cast<uint8_t>(list_count);
            for (int v = 0; v < list_count; ++v)
                knob.valueList[v] = static_cast<float>(cJSON_GetArrayItem(list, v)->valuedouble);
            control.value = knob.mappedValue();
        }
    }
}

// Parses each widget of one "pages.<section>.widgets" array on its own, so big non-control
// widgets (e.g. wavetables) never become cJSON trees.
void add_section_controls(const char *pages, const char *pages_end, const char *section, const char *page_id,
                          EezPageControls *out) {
    const char *object = find_json_value(pages, pages_end, section);
    const char *object_end = object != nullptr && *object == '{' ? skip_json_container(object, pages_end) : nullptr;
    const char *array = object_end != nullptr ? find_json_value(object, object_end, "widgets") : nullptr;
    if (array == nullptr || *array != '[') return;
    const char *array_end = skip_json_container(array, object_end);
    if (array_end == nullptr) return;

    for (const char *p = array + 1; p < array_end;) {
        if (*p != '{') {
            ++p;
            continue;
        }
        const char *widget_end = skip_json_container(p, array_end);
        if (widget_end == nullptr) return;
        const char *type = find_json_value(p, widget_end, "type");
        if (type != nullptr && (strncmp(type, "\"knob\"", 6) == 0 || strncmp(type, "\"button\"", 8) == 0)) {
            cJSON *widget = cJSON_ParseWithLength(p, static_cast<size_t>(widget_end - p));
            const char *widget_page = cJSON_GetStringValue(
                cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(widget, "ui"), "pageId"));
            // Presets saved before multi-page support have no pageId: the section is the page.
            if (strcmp(widget_page != nullptr ? widget_page : section, page_id) == 0) add_widget_controls(widget, out);
            cJSON_Delete(widget);
        }
        p = widget_end;
    }
}

// Refreshed by the UI storage worker; status polling never touches LittleFS.
std::atomic<uint32_t> s_preset_position{(1u << 16) | 1u};

// Only the display storage worker reads this snapshot. Tabs and all pages share
// one PSRAM copy instead of reopening the same preset for every page.
const char *display_preset_text() {
    static char *json = nullptr;
    static uint32_t sequence = UINT32_MAX;
    const uint32_t current = Feature::Synth::engine().presetChangeSequence();
    if (sequence != current || json == nullptr) {
        heap_caps_free(json);
        char path[300] = {}, name[presetpaths::kMaxNameLen] = {};
        presetpaths::resolveActivePath(path, sizeof(path), name, sizeof(name));
        json = read_text_file(path);
        sequence = current;
    }
    return json;
}

} // namespace
#endif

extern "C" bool eez_read_knob_activity(KnobActivity *activity) {
#if defined(CONFIG_APP_ENABLE_SYNTH)
    return activity != nullptr && Feature::Synth::engine().readKnobActivity(*activity);
#else
    return false;
#endif
}

extern "C" bool oled_read_knob_activity(KnobActivity *activity) {
#if defined(CONFIG_APP_ENABLE_SYNTH)
    return activity != nullptr && Feature::Synth::engine().readKnobActivity(*activity, 1);
#else
    return false;
#endif
}

extern "C" bool eez_synth_status(EezSynthStatus *status) {
    if (status == nullptr) return false;
    *status = {};
#if defined(CONFIG_APP_ENABLE_SYNTH)
    status->audio_core_load = Feature::Synth::engine().audioCoreLoadPercent();
    // Same effective tempo the Clock/Sequencer nodes run at (and the browser's BPM badge shows).
    const auto &mc = synth::masterClock();
    status->master_clock_bpm = mc.roundBpm ? roundf(mc.bpm) : mc.bpm;
    status->master_volume = synth::deviceSettings().volume;
    const SynthEngine::MidiVoiceUsageSnapshot voices = Feature::Synth::engine().midiVoiceUsage();
    status->active_voices = voices.active;
    status->maximum_voices = voices.maximum;
    status->underruns = Feature::Synth::dma_underrun_count();
    status->preset_change_sequence = Feature::Synth::engine().presetChangeSequence();
    Feature::Synth::engine().readCurrentPresetName(status->preset_name, sizeof(status->preset_name));
    const uint32_t position = s_preset_position.load(std::memory_order_relaxed);
    status->preset_number = position & 0xffffu;
    status->preset_count = position >> 16;
#if defined(CONFIG_APP_ENABLE_USB_HOST) && defined(CONFIG_USB_HOST_CLASS_MIDI)
    if (Feature::MIDI::is_connected()) snprintf(status->midi_name, sizeof(status->midi_name), "USB MIDI");
#endif
#if defined(CONFIG_APP_ENABLE_BLUETOOTH) && defined(CONFIG_BLE_ENABLE_MIDI_HOST)
    if (Feature::BLE::MIDI::connected_count() > 0)
        snprintf(status->midi_name, sizeof(status->midi_name), "BLE MIDI");
#endif
    return true;
#else
    return false;
#endif
}

extern "C" void eez_network_ip(char *out, size_t out_size) {
    if (out == nullptr || out_size == 0) return;
    snprintf(out, out_size, "--");

#if defined(CONFIG_APP_ENABLE_ETHERNET) || defined(CONFIG_APP_ENABLE_WIFI)
    esp_netif_ip_info_t info{};
#endif
#if defined(CONFIG_APP_ENABLE_ETHERNET)
    if (Feature::Ethernet::get_ip_info(info) == ESP_OK && info.ip.addr != 0)
        ip4addr_ntoa_r(reinterpret_cast<const ip4_addr_t *>(&info.ip), out, out_size);
#endif
#if defined(CONFIG_APP_ENABLE_WIFI)
    if (strcmp(out, "--") == 0 && Feature::WiFi::get_ip_info(info) == ESP_OK && info.ip.addr != 0)
        ip4addr_ntoa_r(reinterpret_cast<const ip4_addr_t *>(&info.ip), out, out_size);
#endif
}

extern "C" bool eez_list_presets(EezPresetList *list) {
    if (list == nullptr) return false;
    memset(list, 0, sizeof(*list));
#if defined(CONFIG_APP_ENABLE_SYNTH)
    list->count = 1;
    EezPresetEntry *current = &list->entries[0];
    snprintf(current->display_name, sizeof(current->display_name), "Current");
    read_preset_metadata(presetpaths::unnamedPath(), current);

    DIR *dir = opendir(presetpaths::kDir);
    dirent *entry;
    while (dir != nullptr && (entry = readdir(dir)) != nullptr && list->count < EEZ_PRESET_CAPACITY) {
        const char *file_name = entry->d_name;
        const size_t length = strlen(file_name);
        if (file_name[0] == '.' || length < 6 || strcmp(file_name, "current.json") == 0 ||
            strcmp(file_name + length - 5, ".json") != 0)
            continue;

        EezPresetEntry *preset = &list->entries[list->count];
        snprintf(preset->file_name, sizeof(preset->file_name), "%.*s", static_cast<int>(length - 5), file_name);
        if (!presetpaths::isSafeName(preset->file_name)) continue;
        snprintf(preset->display_name, sizeof(preset->display_name), "%.*s", static_cast<int>(length - 5), file_name);
        char path[300] = {};
        presetpaths::pathForName(preset->file_name, path, sizeof(path));
        read_preset_metadata(path, preset);
        ++list->count;
    }
    if (dir != nullptr) closedir(dir);
    char active_path[300] = {};
    char active_name[presetpaths::kMaxNameLen] = {};
    presetpaths::resolveActivePath(active_path, sizeof(active_path), active_name, sizeof(active_name));
    uint16_t number = 1;
    for (uint16_t i = 1; i < list->count; ++i)
        if (strcmp(active_name, list->entries[i].file_name) == 0) {
            number = i + 1;
            break;
        }
    s_preset_position.store((static_cast<uint32_t>(list->count) << 16) | number, std::memory_order_relaxed);
#endif
    return true;
}

extern "C" bool eez_list_tabs(EezTabList *list) {
    if (list == nullptr) return false;
    memset(list, 0, sizeof(*list));
#if defined(CONFIG_APP_ENABLE_SYNTH)
    const char *json = display_preset_text();
    cJSON *ui_pages = json != nullptr ? parse_json_array(json, "uiPages") : nullptr;
    cJSON *tab_order = json != nullptr ? parse_json_array(json, "uiTabOrder") : nullptr;

    // Same tabs main.js's restoreGeneratorPageStructure() builds: base sound page,
    // the extra sound pages from uiPages, then the fixed FX page.
    const char *ids[EEZ_TAB_CAPACITY] = {"generator"};
    const char *titles[EEZ_TAB_CAPACITY] = {"Sound Generation"};
    size_t tab_count = 1;
    auto find_tab = [&](const char *id) {
        for (size_t i = 0; i < tab_count; ++i)
            if (strcmp(ids[i], id) == 0) return i;
        return tab_count;
    };
    cJSON *entry;
    cJSON_ArrayForEach(entry, ui_pages) {
        const char *id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(entry, "id"));
        const char *title = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(entry, "title"));
        if (id == nullptr) continue;
        const bool has_title = title != nullptr && title[0] != '\0';
        if (strcmp(id, "generator") == 0) {
            if (has_title) titles[0] = title;
            continue;
        }
        if (strcmp(id, "fx") == 0 || find_tab(id) < tab_count || tab_count >= EEZ_TAB_CAPACITY - 1) continue;
        ids[tab_count] = id;
        titles[tab_count++] = has_title ? title : id;
    }
    ids[tab_count] = "fx";
    titles[tab_count++] = "FX & Masteroutput";

    // restorePageTabOrder(): tabs named in uiTabOrder first, then any it left out.
    bool added[EEZ_TAB_CAPACITY] = {};
    auto add_tab = [&](size_t i) {
        if (i >= tab_count || added[i]) return;
        added[i] = true;
        snprintf(list->ids[list->count], sizeof(list->ids[0]), "%s", ids[i]);
        snprintf(list->titles[list->count++], sizeof(list->titles[0]), "%s", titles[i]);
    };
    cJSON_ArrayForEach(entry, tab_order) {
        const char *id = cJSON_GetStringValue(entry);
        if (id != nullptr) add_tab(find_tab(id));
    }
    for (size_t i = 0; i < tab_count; ++i) add_tab(i);

    cJSON_Delete(ui_pages);
    cJSON_Delete(tab_order);
    return true;
#else
    return false;
#endif
}

extern "C" bool eez_list_page_controls(const char *page_id, EezPageControls *out) {
    if (page_id == nullptr || out == nullptr) return false;
    out->count = 0;
#if defined(CONFIG_APP_ENABLE_SYNTH)
    const char *json = display_preset_text();
    if (json == nullptr) return false;
    const char *end = json + strlen(json);
    const char *pages = find_json_value(json, end, "pages");
    const char *pages_end = pages != nullptr && *pages == '{' ? skip_json_container(pages, end) : nullptr;
    if (pages_end != nullptr) {
        add_section_controls(pages, pages_end, "generator", page_id, out);
        add_section_controls(pages, pages_end, "fx", page_id, out);
    }
    // Reading order of the browser canvas: top to bottom, then left to right.
    std::stable_sort(out->controls, out->controls + out->count, [](const EezPageControl &a, const EezPageControl &b) {
        return a.ui_y != b.ui_y ? a.ui_y < b.ui_y : a.ui_x < b.ui_x;
    });
    return true;
#else
    return false;
#endif
}

// Applies a touch change exactly like the browser's live knob/button edit: the same
// "paramUpdate" frame goes onto the render task's live-param queue.
extern "C" bool eez_send_control_param(const char *widget_id, bool is_button, uint8_t control_index, const char *key,
                                       float value) {
#if defined(CONFIG_APP_ENABLE_SYNTH)
    QueueHandle_t queue = Feature::Synth::live_param_queue();
    if (queue == nullptr || widget_id == nullptr || key == nullptr) return false;
    char body[160] = {};
    snprintf(body, sizeof(body), "\"widgetId\":\"%s\",\"params\":{\"%s\":%u,\"%s\":%g}}", widget_id,
             is_button ? "buttonIndex" : "knobIndex", static_cast<unsigned>(control_index + 1), key,
             static_cast<double>(value));
    LiveParamMsg msg{};
    snprintf(msg.json, sizeof(msg.json), "{\"type\":\"paramUpdate\",%s", body);
    if (xQueueSend(queue, &msg, 0) != pdTRUE) return false;
#if defined(CONFIG_APP_ENABLE_WEBSERVER)
    // Open browsers mirror the touch change (main.js applyDeviceParamUpdate) without echoing it back.
    snprintf(msg.json, sizeof(msg.json), "{\"type\":\"deviceParamUpdate\",%s", body);
    Feature::Synth::broadcast_json(msg.json);
#endif
    return true;
#else
    (void)widget_id;
    (void)is_button;
    (void)control_index;
    (void)key;
    (void)value;
    return false;
#endif
}

extern "C" bool eez_synth_load_preset(const char *name) {
#if defined(CONFIG_APP_ENABLE_SYNTH)
    return name != nullptr && name[0] != '\0' ? Feature::Synth::request_preset_name(name)
                                                : Feature::Synth::request_current_preset();
#else
    (void)name;
    return false;
#endif
}

extern "C" void eez_synth_panic(void) {
#if defined(CONFIG_APP_ENABLE_SYNTH)
    Feature::Synth::engine().requestPanic();
#endif
}

extern "C" void eez_synth_step_preset(int direction) {
#if defined(CONFIG_APP_ENABLE_SYNTH)
    if (Feature::Synth::s_presetNavQueue == nullptr) return;
    const int step = direction < 0 ? -1 : 1;
    xQueueOverwrite(Feature::Synth::s_presetNavQueue, &step);
#endif
}

extern "C" void app_main(void) {
    App::register_callbacks();
    App::init();
    App::start_background_tasks();

#if (defined(CONFIG_APP_ENABLE_USB_DEVICE) && defined(CONFIG_USB_DEV_ENABLE_UAC2)) || \
    defined(CONFIG_APP_ENABLE_SYNTH)
    uint32_t seconds = 0;
#if defined(CONFIG_APP_ENABLE_SYNTH)
    uint8_t synthTicks = 0;
#endif
    while (true) {
#if defined(CONFIG_APP_ENABLE_SYNTH)
        vTaskDelay(pdMS_TO_TICKS(50));
#if defined(CONFIG_APP_ENABLE_WEBSERVER)
        Feature::Synth::broadcast_cv_status_tick();
        Feature::Synth::broadcast_sampler_status_tick();
        Feature::Synth::broadcast_midi_tick();
#endif
        Feature::Synth::log_midi_cc_debug_tick();
        Feature::Synth::log_render_diagnostics_tick();
        if (++synthTicks < 20) continue;
        synthTicks = 0;
#else
        vTaskDelay(pdMS_TO_TICKS(1000));
#endif
        ++seconds;
#if defined(CONFIG_APP_ENABLE_USB_DEVICE) && defined(CONFIG_USB_DEV_ENABLE_UAC2)
        if (seconds % 5 == 0) {
            App::log_usb_audio_stats_tick();
        }
#endif
#if defined(CONFIG_APP_ENABLE_SYNTH) && defined(CONFIG_APP_ENABLE_WEBSERVER)
        Feature::Synth::broadcast_status_tick();
#endif
#if defined(CONFIG_APP_ENABLE_SYNTH)
        // Real DMA-ring underrun log + reset-on-reload/Panic - kept off the web-server gate
        // above so it still shows up on the serial log with no browser/UI involved.
        Feature::Synth::dma_underrun_tick();
        // Native P0 baseline diagnostic (documentation/native-synth-
        // optimization-plan-2026-09-09.md) - off the render task on
        // purpose, see log_p0_baseline_tick()'s comment (synth_manager.hpp).
        // if (seconds % 2 == 0) {
        //     Feature::Synth::log_p0_baseline_tick();
        // }
#endif
    }
#endif
}

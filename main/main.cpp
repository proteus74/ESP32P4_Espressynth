#include "app_events.hpp"
#include "app_startup.hpp"
#include "features/display/eez_ui_bridge.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/ip4_addr.h"

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

void update_preset_position(EezSynthStatus *status) {
    constexpr size_t kPresetCapacity = 32;
    static uint32_t seen_sequence = UINT32_MAX;
    static uint16_t number = 1;
    static uint16_t count = 1;
    static char names[kPresetCapacity][presetpaths::kMaxNameLen];

    const uint32_t sequence = Feature::Synth::engine().presetChangeSequence();
    if (sequence != seen_sequence) {
        seen_sequence = sequence;
        size_t found = 1; // index 0 = current.json
        names[0][0] = '\0';

        DIR *dir = opendir(presetpaths::kDir);
        if (dir != nullptr) {
            dirent *entry;
            while ((entry = readdir(dir)) != nullptr && found < kPresetCapacity) {
                const char *file_name = entry->d_name;
                const size_t length = strlen(file_name);
                if (file_name[0] == '.' || length < 6 || strcmp(file_name, "current.json") == 0 ||
                    strcmp(file_name + length - 5, ".json") != 0)
                    continue;

                char preset_name[presetpaths::kMaxNameLen] = {};
                snprintf(preset_name, sizeof(preset_name), "%.*s", static_cast<int>(length - 5), file_name);
                if (!presetpaths::isSafeName(preset_name)) continue;
                snprintf(names[found++], sizeof(names[0]), "%s", preset_name);
            }
            closedir(dir);
        }

        char active_path[300] = {};
        char active_name[presetpaths::kMaxNameLen] = {};
        presetpaths::resolveActivePath(active_path, sizeof(active_path), active_name, sizeof(active_name));
        size_t index = 0;
        for (size_t i = 1; i < found; ++i) {
            if (strcmp(active_name, names[i]) == 0) {
                index = i;
                break;
            }
        }
        number = static_cast<uint16_t>(index + 1);
        count = static_cast<uint16_t>(found);
    }

    status->preset_number = number;
    status->preset_count = count;
}

} // namespace
#endif

extern "C" bool eez_synth_status(EezSynthStatus *status) {
    if (status == nullptr) return false;
    *status = {};
#if defined(CONFIG_APP_ENABLE_SYNTH)
    status->audio_core_load = Feature::Synth::engine().audioCoreLoadPercent();
    const SynthEngine::MidiVoiceUsageSnapshot voices = Feature::Synth::engine().midiVoiceUsage();
    status->active_voices = voices.active;
    status->maximum_voices = voices.maximum;
    status->underruns = Feature::Synth::dma_underrun_count();
    status->preset_load_active = Feature::Synth::engine().presetLoadActive();
    status->preset_load_progress = Feature::Synth::engine().presetLoadProgress();
    Feature::Synth::engine().readCurrentPresetName(status->preset_name, sizeof(status->preset_name));
    update_preset_position(status);
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
    if (dir == nullptr) return true;
    dirent *entry;
    while ((entry = readdir(dir)) != nullptr && list->count < EEZ_PRESET_CAPACITY) {
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
    closedir(dir);
#endif
    return true;
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
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
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

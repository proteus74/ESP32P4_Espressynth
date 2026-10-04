#pragma once

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <new>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "LiveParamMsg.h"
#include "MemoryDiagnostics.h"
#include "PresetChangedMsg.h"
#include "PresetPaths.h"
#include "SamplePaths.h"
#include "UsbMidiEvent.h"
#include "driver/gpio.h"
#include "soc/soc_caps.h"

#include "features/filesystem/littlefs_manager.hpp"
#include "features/io/gpio_manager.hpp"
#include "features/io/led_strip_manager.hpp"
#include "features/io/i2c_device_registry.h"
#include "features/sd_card/sd_card_manager.hpp"
#include "features/web/web_server.hpp"
#include "synth_manager.hpp"
#include "version.h"

#if defined(CONFIG_APP_ENABLE_USB_HOST) && defined(CONFIG_USB_HOST_CLASS_MIDI)
    #include "features/usb/host/usb_midi_host.hpp"
#endif
#if defined(CONFIG_APP_ENABLE_BLUETOOTH)
    #include "features/ble/ble_manager.hpp"
#endif

// REST + WebSocket API for the synth engine's browser patch-editor UI (data/index.html,
// data/static/* - ported from JoLa-EspressynthP4's web/WebServer.cpp/.h, ~2200 lines there).
// Registers its routes on Feature::Web's shared httpd server; see
// Feature::Web::begin()/register_catch_all()'s comments for why register_web_routes() must run
// between those two calls.
//
// Deliberately NOT ported here: WebServer.cpp's /api/settings/network, /api/wifi/*, /api/ble/*
// routes and its UsbMidiClassDriver-based multi-device MIDI port list/connect/disconnect
// (/api/midi/connect, /api/midi/connect, /api/midi/disconnect) - this project already has its
// own Wi-Fi/Ethernet/BLE configuration surface, and its MIDI transports (features/usb/host/
// usb_midi_host.hpp, features/usb/device/usb_device_midi.hpp, features/ble/ble_midi_host.hpp)
// don't expose the multi-device tracking/selection API that port list needs. GET
// /api/midi/devices below reports simple per-transport availability instead.
namespace Feature::Synth {

namespace {
constexpr const char *WEB_API_TAG = "SynthWebApi";
constexpr size_t kMaxPathLen = 300;
constexpr size_t kMaxPresetBodyLen = 256 * 1024;
constexpr const char *kEmptyPresetJson = "{\"widgets\":[],\"connections\":[]}";
constexpr size_t kUploadSafetyMarginBytes = 4096; // headroom below reported free space
constexpr size_t kMaxWsClients = 12; // must cover Feature::Web::begin()'s config.max_open_sockets

struct BroadcastWork {
    // CV status can contain one live-voltage entry per widget.
    char json[4096];
};

void broadcast_work(void *arg) {
    BroadcastWork *work = static_cast<BroadcastWork *>(arg);
    httpd_handle_t server = Feature::Web::handle();
    int client_fds[kMaxWsClients];
    size_t fd_count = kMaxWsClients;
    if (httpd_get_client_list(server, &fd_count, client_fds) == ESP_OK) {
        httpd_ws_frame_t frame = {};
        frame.type = HTTPD_WS_TYPE_TEXT;
        frame.payload = reinterpret_cast<uint8_t *>(work->json);
        frame.len = strlen(work->json);
        for (size_t i = 0; i < fd_count; ++i) {
            const int fd = client_fds[i];
            if (httpd_ws_get_fd_info(server, fd) == HTTPD_WS_CLIENT_WEBSOCKET &&
                httpd_ws_send_frame_async(server, fd, &frame) != ESP_OK) {
                // Browser reloads and link loss can invalidate a socket between enumeration
                // and send. Stop broadcasting to it.
                httpd_sess_trigger_close(server, fd);
            }
        }
    }
    delete work;
}
} // namespace

/**
 * @brief Broadcasts an already-serialized JSON text frame to every connected /ws client. Safe
 *        to call from any task (e.g. the USB-MIDI dispatch task) - a no-op if the web server
 *        isn't running yet.
 */
inline void broadcast_json(const char *json) {
    httpd_handle_t server = Feature::Web::handle();
    if (server == nullptr) return;
    BroadcastWork *work = new (std::nothrow) BroadcastWork();
    if (work == nullptr) return;
    const int length = snprintf(work->json, sizeof(work->json), "%s", json);
    if (length < 0 || static_cast<size_t>(length) >= sizeof(work->json) ||
        httpd_queue_work(server, &broadcast_work, work) != ESP_OK) {
        delete work;
    }
}

/**
 * @brief Broadcasts an incoming MIDI event to the browser (the {status,data1,data2} shape
 *        shared by every transport - see dispatch_midi()) so the web UI's MIDI activity log
 *        updates live. Safe to call from any task; a no-op if the web server isn't running.
 */
inline void broadcast_midi_event(uint8_t status, uint8_t data1, uint8_t data2) {
    if (Feature::Web::handle() == nullptr) return;
    const UsbMidiEvent event = from_transport(status, data1, data2);
    char json[256];
    if (event.toJson(json, sizeof(json), static_cast<uint32_t>(esp_timer_get_time() / 1000), "MIDI") > 0) {
        broadcast_json(json);
    }
}

/** Broadcasts live CV input values and CV device state at the UI refresh rate. */
inline void broadcast_cv_status_tick() {
    if (Feature::Web::handle() == nullptr) return;

    static char json[4096];
    int offset = snprintf(json, sizeof(json), "{\"type\":\"cvStatus\",\"w\":[");
    const uint16_t count = engine().cvStatusCount();
    if (count == 0) return;

    bool first = true;
    for (uint16_t i = 0; i < count && offset > 0 && static_cast<size_t>(offset) < sizeof(json); ++i) {
        char widget_id[24];
        bool found = true;
        bool is_input = false;
        float volts = 0.0f;
        if (!engine().cvStatusAt(i, widget_id, sizeof(widget_id), &found, &is_input, &volts) ||
            (!is_input && found)) continue;
        offset += is_input
            ? snprintf(json + offset, sizeof(json) - static_cast<size_t>(offset),
                       "%s[\"%s\",%s,%.4f]", first ? "" : ",", widget_id,
                       found ? "true" : "false", static_cast<double>(volts))
            : snprintf(json + offset, sizeof(json) - static_cast<size_t>(offset),
                       "%s[\"%s\",false]", first ? "" : ",", widget_id);
        first = false;
    }
    if (offset > 0 && static_cast<size_t>(offset) < sizeof(json)) {
        snprintf(json + offset, sizeof(json) - static_cast<size_t>(offset), "]}");
        broadcast_json(json);
    }
}

/**
 * @brief Periodic engine-status broadcast: "cpuLoad" (matches main.js's updateCpuLoadDisplay()/
 *        updateMemStatsDisplay() field names exactly - core0/core1/peakUs/budgetUs/underruns/
 *        midiDrops/sfVoices/ps*Kb/sys*Kb/...), "nodeCpu" (per-widget load, main.js's
 *        updateNodeCpuDisplay()) and, on change, "voicesCapped". Call roughly once a second
 *        from the main loop (see main.cpp) - a no-op if the web server isn't running. "underruns"
 *        is the true DMA-ring-ran-dry count (Feature::Synth::dma_underrun_count(), an actual
 *        audible click) - deliberately the only one of the two the browser UI shows, since a
 *        render-budget miss (the other field, "renderOverruns" = SynthEngine::
 *        renderOverrunCount(), kept in the JSON for tools/benchmark_synth.mjs but intentionally
 *        NOT surfaced in index.html/main.js/display_lvgl.hpp) is usually absorbed by the DMA
 *        ring's headroom without audible effect and was getting mistaken for one - see its own
 *        "Audio-Overrun" serial log for that diagnostic instead.
 */
inline void broadcast_status_tick() {
    if (Feature::Web::handle() == nullptr) return;

    static uint32_t last_preset_change_sequence = 0;
    const uint32_t preset_change_sequence = engine().presetChangeSequence();
    if (preset_change_sequence != last_preset_change_sequence) {
        last_preset_change_sequence = preset_change_sequence;
        static char preset_name[presetpaths::kMaxNameLen] = {};
        engine().readCurrentPresetName(preset_name, sizeof(preset_name));
        static char json[160];
        snprintf(json, sizeof(json), "{\"type\":\"presetChanged\",\"name\":\"%s\"}", preset_name);
        broadcast_json(json);
    }

    // Master Clock (MasterClock.h) - device-wide, not tied to any preset, so
    // it needs its own change-detection independent of presetChanged above;
    // only fires on an actual change (a Learn/Save from any browser tab, or a
    // live MIDI-CC tick moving bpm) so this stays cheap at the ~1/s tick rate.
    {
        static float last_bpm = -1.0f, last_max_bpm = -1.0f;
        static bool last_round = false;
        static int last_cc = -2, last_channel = -2;
        const auto &mc = synth::masterClock();
        if (mc.bpm != last_bpm || mc.maxBpm != last_max_bpm || mc.roundBpm != last_round ||
            mc.midiCc != last_cc || mc.midiChannel != last_channel) {
            last_bpm = mc.bpm; last_max_bpm = mc.maxBpm; last_round = mc.roundBpm;
            last_cc = mc.midiCc; last_channel = mc.midiChannel;
            char json[160];
            if (synth::formatMasterClockJson(json, sizeof(json)) > 0) broadcast_json(json);
        }
    }

    static uint32_t last_voices_cap_id = 0;
    const uint32_t voices_cap_id = engine().voicesCapEventId();
    if (voices_cap_id != last_voices_cap_id) {
        last_voices_cap_id = voices_cap_id;
        char json[180];
        snprintf(json, sizeof(json), "{\"type\":\"voicesCapped\",\"requested\":%u,\"effective\":%u,\"reason\":\"%s\"}",
                 static_cast<unsigned>(engine().voicesCapRequested()), static_cast<unsigned>(engine().voicesCapEffective()),
                 engine().voicesCapReason() == 1 ? "cpu" : "psram");
        broadcast_json(json);
    }

    // Core 0 (Ethernet/USB/httpd/this task): generic FreeRTOS idle-task share, capped 0-100%.
    // Core 1 (the engine's render task, exclusive): audioCoreLoadPercent() - block-budget-based,
    // deliberately not replaced by the generic idle-task value since it can exceed 100% on a
    // real overrun (see SynthEngine::taskLoop).
    static int64_t previous_sample_us = 0;
    static uint32_t previous_idle_runtime = 0;
    const int64_t sample_us = esp_timer_get_time();
    const uint32_t idle_runtime = ulTaskGetIdleRunTimeCounterForCore(0);
    float core0_load = 0.0f;
    if (previous_sample_us != 0) {
        const uint32_t idle_delta = idle_runtime - previous_idle_runtime;
        const int64_t total_delta = sample_us - previous_sample_us;
        if (total_delta > 0) {
            core0_load = 100.0f * (1.0f - static_cast<float>(idle_delta) / static_cast<float>(total_delta));
            if (core0_load < 0.0f) core0_load = 0.0f;
            if (core0_load > 100.0f) core0_load = 100.0f;
        }
    }
    static configRUN_TIME_COUNTER_TYPE previous_task_runtime[3] = {};
    const char *task_names[] = {"taskLVGL", "httpd", "main"};
    float task_load[3] = {};
    for (size_t i = 0; i < 3; ++i) {
        const TaskHandle_t task = xTaskGetHandle(task_names[i]);
        if (task == nullptr) continue;
        TaskStatus_t status{};
        vTaskGetInfo(task, &status, pdFALSE, eInvalid);
        if (previous_sample_us != 0 && sample_us > previous_sample_us)
            task_load[i] = 100.0f * static_cast<float>(status.ulRunTimeCounter - previous_task_runtime[i]) /
                           static_cast<float>(sample_us - previous_sample_us);
        previous_task_runtime[i] = status.ulRunTimeCounter;
    }
    previous_idle_runtime = idle_runtime;
    previous_sample_us = sample_us;

    constexpr float kBlockBudgetUs = 1000000.0f * static_cast<float>(synth::kBlockSize) / static_cast<float>(synth::kSampleRate);
    const unsigned long ps_free_kb = static_cast<unsigned long>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);
    const unsigned long ps_total_kb = static_cast<unsigned long>(heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / 1024);
    const unsigned long sys_free_kb = static_cast<unsigned long>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024);
    const unsigned long sys_total_kb = static_cast<unsigned long>(heap_caps_get_total_size(MALLOC_CAP_INTERNAL) / 1024);

    const memorydiag::HeapSnapshot memory = memorydiag::snapshot();
    memorydiag::State memory_state;
    memorydiag::read(memory_state);
    const SynthEngine::TimingSnapshot timing = engine().consumeTimingSnapshot();
    const SynthEngine::MidiVoiceUsageSnapshot voices = engine().midiVoiceUsage();
    const SynthEngine::PhaseSnapshot phases = engine().phaseSnapshot();
    const SynthEngine::BlockStatsSnapshot stats = engine().blockStatsSnapshot();

    // Status tick runs on app_main's small stack. Keep both large JSON
    // staging buffers in BSS; placing this one beside node_cpu_json on the
    // stack caused an immediate stack-protection reset loop.
    static char json[1536];
    const int json_length = snprintf(json, sizeof(json),
             "{\"type\":\"cpuLoad\",\"core0\":%.1f,\"core1\":%.1f,"
             "\"tasks\":{\"lvgl\":%.1f,\"httpd\":%.1f,\"main\":%.1f},\"peakUs\":%lu,\"budgetUs\":%lu,"
             "\"p50Us\":%lu,\"p95Us\":%lu,\"p99Us\":%lu,\"timingSamples\":%lu,"
             "\"underruns\":%lu,\"renderOverruns\":%lu,\"midiDrops\":%lu,\"sfVoices\":%u,"
             "\"psFreeKb\":%lu,\"psTotalKb\":%lu,\"sysFreeKb\":%lu,\"sysTotalKb\":%lu,"
             "\"sysLargestKb\":%lu,\"sysMinKb\":%lu,\"psLargestKb\":%lu,\"psMinKb\":%lu,"
             "\"synthStackHwmBytes\":%lu,\"reloadGeneration\":%lu,\"reloadPhase\":%u,"
             "\"voiceActive\":%u,\"voiceMax\":%u,\"voiceCapRequested\":%u,\"voiceCapEffective\":%u,"
             "\"phases\":{\"activity\":%.2f,\"render\":%.2f,\"output\":%.2f},"
             "\"fm\":{\"pitchConst\":%lu,\"pitchAudio\":%lu,\"tuneConst\":%lu,\"tuneAudio\":%lu,"
             "\"ampConst\":%lu,\"ampAudio\":%lu,\"ampZero\":%lu,\"silent\":%lu,"
             "\"gainConverged\":%lu,\"modActive\":%lu,\"feedbackActive\":%lu,\"superSawActive\":%lu},"
             "\"dx\":{\"holding\":%lu,\"ramping\":%lu,\"boundary\":%lu,\"shapeConfigured\":%lu},"
             "\"instances\":{\"total\":%lu,\"rendered\":%lu,\"idleForced\":%lu}}",
             static_cast<double>(core0_load), static_cast<double>(engine().audioCoreLoadPercent()),
             static_cast<double>(task_load[0]), static_cast<double>(task_load[1]), static_cast<double>(task_load[2]),
             static_cast<unsigned long>(timing.peakUs), static_cast<unsigned long>(kBlockBudgetUs),
             static_cast<unsigned long>(timing.p50Us), static_cast<unsigned long>(timing.p95Us),
             static_cast<unsigned long>(timing.p99Us), static_cast<unsigned long>(timing.sampleCount),
             static_cast<unsigned long>(dma_underrun_count()), static_cast<unsigned long>(engine().renderOverrunCount()),
             static_cast<unsigned long>(engine().midiDropCount()),
             engine().activeSoundFontVoices(), ps_free_kb, ps_total_kb, sys_free_kb, sys_total_kb,
             static_cast<unsigned long>(memory.internalLargest / 1024), static_cast<unsigned long>(memory.internalMinimum / 1024),
             static_cast<unsigned long>(memory.psramLargest / 1024), static_cast<unsigned long>(memory.psramMinimum / 1024),
             static_cast<unsigned long>(memory.synthStackHighWaterBytes), static_cast<unsigned long>(memory_state.generation),
             static_cast<unsigned>(memory_state.phase), static_cast<unsigned>(voices.active),
             static_cast<unsigned>(voices.maximum), static_cast<unsigned>(engine().voicesCapRequested()),
             static_cast<unsigned>(engine().voicesCapEffective()), static_cast<double>(phases.activityPercent),
             static_cast<double>(phases.renderPercent), static_cast<double>(phases.outputPercent),
             static_cast<unsigned long>(stats.fmPitchConstant), static_cast<unsigned long>(stats.fmPitchAudioRate),
             static_cast<unsigned long>(stats.fmTuneConstant), static_cast<unsigned long>(stats.fmTuneAudioRate),
             static_cast<unsigned long>(stats.fmAmpConstant), static_cast<unsigned long>(stats.fmAmpAudioRate),
             static_cast<unsigned long>(stats.fmAmpConstantZero), static_cast<unsigned long>(stats.fmSilentSkipped),
             static_cast<unsigned long>(stats.fmGainConverged), static_cast<unsigned long>(stats.fmModActive),
             static_cast<unsigned long>(stats.fmFeedbackActive), static_cast<unsigned long>(stats.fmSuperSawActive),
             static_cast<unsigned long>(stats.dxHolding),
             static_cast<unsigned long>(stats.dxRamping), static_cast<unsigned long>(stats.dxBoundary),
             static_cast<unsigned long>(stats.dxShapeConfigured), static_cast<unsigned long>(stats.instancesTotal),
             static_cast<unsigned long>(stats.instancesRendered),
             static_cast<unsigned long>(stats.instancesRenderedWhileIdle));
    if (json_length > 0 && static_cast<size_t>(json_length) < sizeof(json)) broadcast_json(json);

    static char node_cpu_json[1536];
    int offset = snprintf(node_cpu_json, sizeof(node_cpu_json), "{\"type\":\"nodeCpu\",\"w\":[");
    const uint16_t widget_load_count = engine().widgetLoadCount();
    bool first = true;
    for (uint16_t i = 0; i < widget_load_count && offset > 0 && static_cast<size_t>(offset) < sizeof(node_cpu_json); ++i) {
        char widget_id[24];
        float widget_percent = 0.0f;
        if (!engine().widgetLoadAt(i, widget_id, sizeof(widget_id), &widget_percent)) continue;
        const float edge_percent = engine().widgetEdgeLoadPercentAt(i);
        offset += snprintf(node_cpu_json + offset, sizeof(node_cpu_json) - static_cast<size_t>(offset),
                            "%s[\"%s\",%.1f,%.1f]", first ? "" : ",", widget_id,
                            static_cast<double>(widget_percent), static_cast<double>(edge_percent));
        first = false;
    }
    if (offset > 0 && static_cast<size_t>(offset) < sizeof(node_cpu_json)) {
        snprintf(node_cpu_json + offset, sizeof(node_cpu_json) - static_cast<size_t>(offset), "]}");
        broadcast_json(node_cpu_json);
    }

}

namespace {

// ---- request helpers (adapted from JoLa-EspressynthP4's web/WebServer.cpp) ----

bool extract_query_param(httpd_req_t *req, const char *key, char *out_decoded, size_t out_size) {
    char query[256];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) return false;
    char encoded[kMaxPathLen];
    if (httpd_query_key_value(query, key, encoded, sizeof(encoded)) != ESP_OK) return false;
    presetpaths::urlDecode(encoded, out_decoded, out_size);
    return true;
}

bool extract_preset_name_param(httpd_req_t *req, char *out_name, size_t out_size) {
    return extract_query_param(req, "name", out_name, out_size);
}

bool is_safe_rel_path(const char *path) {
    if (path == nullptr) return false;
    if (strstr(path, "..") != nullptr) return false;
    if (path[0] == '/' || path[0] == '\\') return false;
    for (const char *p = path; *p != '\0'; ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);
        if (c == '\\' || c < 0x20) return false;
    }
    return true;
}

bool is_safe_bare_name(const char *name) {
    if (name == nullptr || name[0] == '\0') return false;
    const size_t len = strlen(name);
    if (len >= 128) return false;
    if (name[0] == '.') return false;
    for (size_t i = 0; i < len; ++i) {
        const unsigned char c = static_cast<unsigned char>(name[i]);
        if (c == '/' || c == '\\' || c == '"' || c < 0x20) return false;
    }
    return true;
}

// Streams `file` to `req` in chunks, then closes it. Buffer is MALLOC_CAP_INTERNAL, not
// PSRAM-preferred like a generic file server might use - LittleFS's lfs_bd_read() bypasses its
// own cache for large, block-aligned reads and has esp_partition_read() write straight into
// this buffer while the flash cache is disabled; ESP-IDF asserts the calling task's *stack* is
// entirely internal RAM at that point (see Feature::Web's static_file_handler for the identical
// hazard), and a buffer the cache-disabled DMA/read path itself writes into needs the same care.
esp_err_t stream_file_chunked(httpd_req_t *req, FILE *file) {
    constexpr size_t kBufferSize = 8192;
    auto *buffer = static_cast<char *>(heap_caps_malloc(kBufferSize, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (buffer == nullptr) {
        fclose(file);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_FAIL;
    }
    esp_err_t result = ESP_OK;
    size_t bytes_read;
    while ((bytes_read = fread(buffer, 1, kBufferSize, file)) > 0) {
        if (httpd_resp_send_chunk(req, buffer, bytes_read) != ESP_OK) {
            result = ESP_FAIL;
            break;
        }
    }
    if (result == ESP_OK) {
        httpd_resp_send_chunk(req, nullptr, 0);
    }
    heap_caps_free(buffer);
    fclose(file);
    return result;
}

enum class ExplorerVolume { Flash, Sd };

ExplorerVolume explorer_volume_from_request(httpd_req_t *req) {
    char volume[16] = {};
    extract_query_param(req, "volume", volume, sizeof(volume));
    return strcmp(volume, "sd") == 0 ? ExplorerVolume::Sd : ExplorerVolume::Flash;
}

bool explorer_volume_available(ExplorerVolume volume) {
    return volume == ExplorerVolume::Flash || Feature::SDCard::is_mounted();
}

const char *explorer_volume_base_path(ExplorerVolume volume) {
    return volume == ExplorerVolume::Sd ? Feature::SDCard::MOUNT_POINT : Feature::LittleFS::MOUNT_POINT;
}

// Builds "<selected volume base path>[/relPath]". The client selects only a fixed volume ID;
// it can never supply a mount path.
void build_storage_path(ExplorerVolume volume, const char *rel_path, char *out, size_t out_size) {
    if (rel_path == nullptr || rel_path[0] == '\0') {
        snprintf(out, out_size, "%s", explorer_volume_base_path(volume));
    } else {
        snprintf(out, out_size, "%s/%s", explorer_volume_base_path(volume), rel_path);
    }
}

// ---- /api/files/* (file explorer) ----

esp_err_t files_list_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    const ExplorerVolume volume = explorer_volume_from_request(req);
    if (!explorer_volume_available(volume)) {
        httpd_resp_sendstr(req, "{\"available\":false,\"dir\":\"\",\"entries\":[]}");
        return ESP_OK;
    }

    char dir[kMaxPathLen] = {};
    extract_query_param(req, "dir", dir, sizeof(dir));
    if (!is_safe_rel_path(dir)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid dir");
        return ESP_FAIL;
    }

    char fs_dir[kMaxPathLen + 16];
    build_storage_path(volume, dir, fs_dir, sizeof(fs_dir));

    DIR *d = opendir(fs_dir);
    if (d == nullptr) {
        httpd_resp_sendstr(req, "{\"dir\":\"\",\"entries\":[]}");
        return ESP_OK;
    }

    constexpr size_t kBufSize = 8192;
    auto *json = static_cast<char *>(heap_caps_malloc(kBufSize, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (json == nullptr) {
        closedir(d);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_FAIL;
    }

    int offset = snprintf(json, kBufSize, "{\"dir\":\"%s\",\"entries\":[", dir);
    bool first = true;
    struct dirent *entry;
    while ((entry = readdir(d)) != nullptr && offset > 0 && static_cast<size_t>(offset) < kBufSize) {
        const char *fname = entry->d_name;
        if (fname[0] == '.') continue; // hidden/marker files (e.g. presets/.last)

        char entry_path[kMaxPathLen + 16 + 256];
        snprintf(entry_path, sizeof(entry_path), "%s/%s", fs_dir, fname);
        struct stat st;
        if (stat(entry_path, &st) != 0) continue;
        const bool is_dir = S_ISDIR(st.st_mode);

        offset += snprintf(json + offset, kBufSize - static_cast<size_t>(offset),
                            "%s{\"name\":\"%s\",\"isDir\":%s,\"size\":%lu}", first ? "" : ",", fname,
                            is_dir ? "true" : "false", static_cast<unsigned long>(st.st_size));
        first = false;
    }
    closedir(d);

    if (offset > 0 && static_cast<size_t>(offset) < kBufSize) {
        snprintf(json + offset, kBufSize - static_cast<size_t>(offset), "]}");
        httpd_resp_sendstr(req, json);
    } else {
        httpd_resp_sendstr(req, "{\"dir\":\"\",\"entries\":[]}");
    }
    heap_caps_free(json);
    return ESP_OK;
}

// Cache for the file-explorer's storage-usage panel - esp_littlefs_info()/esp_vfs_fat_info()
// walk the whole filesystem to compute free space, expensive enough to matter if polled on
// every navigation/upload/delete/rename. Serves a couple-seconds-old answer instead of
// re-scanning every time.
struct FsInfoCache {
    bool valid = false;
    uint64_t total = 0;
    uint64_t used = 0;
    int64_t timestamp_us = 0;
};
FsInfoCache fs_info_cache[2]; // index: ExplorerVolume::Flash=0, Sd=1
constexpr int64_t kFsInfoCacheTtlUs = 3'000'000; // 3s

void build_files_info_json(ExplorerVolume volume, const char *volume_str, char *json, size_t json_size) {
    if (!explorer_volume_available(volume)) {
        snprintf(json, json_size,
                 "{\"type\":\"filesInfo\",\"volume\":\"%s\",\"available\":false,\"totalBytes\":0,\"usedBytes\":0,"
                 "\"freeBytes\":0}",
                 volume_str);
        return;
    }

    FsInfoCache &cache = fs_info_cache[volume == ExplorerVolume::Sd ? 1 : 0];
    const int64_t now_us = esp_timer_get_time();
    if (!cache.valid || (now_us - cache.timestamp_us) > kFsInfoCacheTtlUs) {
        uint64_t total = 0;
        uint64_t used = 0;
        if (volume == ExplorerVolume::Flash) {
            size_t lfs_total = 0;
            size_t lfs_used = 0;
            if (esp_littlefs_info(Feature::LittleFS::PARTITION_LABEL, &lfs_total, &lfs_used) != ESP_OK) {
                snprintf(json, json_size,
                         "{\"type\":\"filesInfo\",\"volume\":\"%s\",\"available\":false,\"totalBytes\":0,"
                         "\"usedBytes\":0,\"freeBytes\":0}",
                         volume_str);
                return;
            }
            total = lfs_total;
            used = lfs_used;
        } else {
            uint64_t fat_total = 0;
            uint64_t fat_free = 0;
            if (esp_vfs_fat_info(Feature::SDCard::MOUNT_POINT, &fat_total, &fat_free) != ESP_OK) {
                snprintf(json, json_size,
                         "{\"type\":\"filesInfo\",\"volume\":\"%s\",\"available\":true,\"totalBytes\":0,"
                         "\"usedBytes\":0,\"freeBytes\":0}",
                         volume_str);
                return;
            }
            total = fat_total;
            used = total > fat_free ? total - fat_free : 0;
        }
        cache.total = total;
        cache.used = used;
        cache.timestamp_us = now_us;
        cache.valid = true;
    }

    snprintf(json, json_size,
             "{\"type\":\"filesInfo\",\"volume\":\"%s\",\"available\":true,\"totalBytes\":%" PRIu64
             ",\"usedBytes\":%" PRIu64 ",\"freeBytes\":%" PRIu64 "}",
             volume_str, cache.total, cache.used, cache.total - cache.used);
}

esp_err_t files_upload_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    const ExplorerVolume volume = explorer_volume_from_request(req);
    if (!explorer_volume_available(volume)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "volume unavailable");
        return ESP_FAIL;
    }

    char dir[kMaxPathLen] = {};
    extract_query_param(req, "dir", dir, sizeof(dir));
    char name[128] = {};
    extract_query_param(req, "name", name, sizeof(name));
    if (!is_safe_rel_path(dir) || !is_safe_bare_name(name)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid path");
        return ESP_FAIL;
    }
    if (req->content_len == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
        return ESP_FAIL;
    }

    size_t total = 0;
    size_t used = 0;
    if (volume == ExplorerVolume::Flash &&
        esp_littlefs_info(Feature::LittleFS::PARTITION_LABEL, &total, &used) == ESP_OK) {
        const size_t free_bytes = total > used ? total - used : 0;
        if (free_bytes < kUploadSafetyMarginBytes || req->content_len > free_bytes - kUploadSafetyMarginBytes) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "not enough space");
            return ESP_FAIL;
        }
    }

    char rel_path[kMaxPathLen + 128];
    if (dir[0] == '\0') {
        snprintf(rel_path, sizeof(rel_path), "%s", name);
    } else {
        snprintf(rel_path, sizeof(rel_path), "%s/%s", dir, name);
    }
    char fs_path[kMaxPathLen + 128 + 16];
    build_storage_path(volume, rel_path, fs_path, sizeof(fs_path));

    FILE *file = fopen(fs_path, "wb");
    if (file == nullptr) {
        ESP_LOGE(WEB_API_TAG, "%s could not be opened for writing", fs_path);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "could not open file");
        return ESP_FAIL;
    }

    // Streamed straight to disk (unlike preset_post_handler's malloc-the-whole-body approach) -
    // uploaded files (WAV samples, SF2 banks) can be several MB, too large to buffer wholesale.
    char buffer[2048];
    size_t received = 0;
    bool write_error = false;
    while (received < req->content_len) {
        int to_read = static_cast<int>(req->content_len - received);
        if (to_read > static_cast<int>(sizeof(buffer))) to_read = static_cast<int>(sizeof(buffer));
        int ret = httpd_req_recv(req, buffer, to_read);
        if (ret <= 0) {
            write_error = true;
            break;
        }
        if (fwrite(buffer, 1, static_cast<size_t>(ret), file) != static_cast<size_t>(ret)) {
            write_error = true;
            break;
        }
        received += static_cast<size_t>(ret);
    }
    fclose(file);

    if (write_error) {
        unlink(fs_path); // don't leave a truncated/partial file behind
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "upload failed");
        return ESP_FAIL;
    }

    httpd_resp_sendstr(req, "{\"saved\":true}");
    return ESP_OK;
}

esp_err_t files_delete_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    const ExplorerVolume volume = explorer_volume_from_request(req);
    if (!explorer_volume_available(volume)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "volume unavailable");
        return ESP_FAIL;
    }

    char rel_path[kMaxPathLen] = {};
    if (!extract_query_param(req, "path", rel_path, sizeof(rel_path)) || !is_safe_rel_path(rel_path) ||
        rel_path[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid path");
        return ESP_FAIL;
    }

    char fs_path[kMaxPathLen + 16];
    build_storage_path(volume, rel_path, fs_path, sizeof(fs_path));

    struct stat st;
    if (stat(fs_path, &st) != 0 || S_ISDIR(st.st_mode)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "not a file");
        return ESP_FAIL;
    }
    if (unlink(fs_path) != 0) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "could not delete file");
        return ESP_FAIL;
    }
    httpd_resp_sendstr(req, "{\"deleted\":true}");
    return ESP_OK;
}

// Streams a file's raw bytes back - used by the browser-side (JS reference) synth engine to
// fetch .sf2/.wav sample data it can't play natively, unlike the on-device C++ engine which
// reads straight off the filesystem. Same volume/path resolution as the other /api/files/*
// handlers.
esp_err_t files_download_handler(httpd_req_t *req) {
    const ExplorerVolume volume = explorer_volume_from_request(req);
    if (!explorer_volume_available(volume)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "volume unavailable");
        return ESP_FAIL;
    }

    char rel_path[kMaxPathLen] = {};
    if (!extract_query_param(req, "path", rel_path, sizeof(rel_path)) || !is_safe_rel_path(rel_path) ||
        rel_path[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid path");
        return ESP_FAIL;
    }

    char fs_path[kMaxPathLen + 16];
    build_storage_path(volume, rel_path, fs_path, sizeof(fs_path));

    struct stat st;
    if (stat(fs_path, &st) != 0 || S_ISDIR(st.st_mode)) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "file not found");
        return ESP_FAIL;
    }

    FILE *file = fopen(fs_path, "rb");
    if (file == nullptr) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "file not found");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/octet-stream");
    return stream_file_chunked(req, file);
}

esp_err_t files_rename_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    const ExplorerVolume volume = explorer_volume_from_request(req);
    if (!explorer_volume_available(volume)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "volume unavailable");
        return ESP_FAIL;
    }

    char rel_path[kMaxPathLen] = {};
    char new_name[128] = {};
    if (!extract_query_param(req, "path", rel_path, sizeof(rel_path)) || !is_safe_rel_path(rel_path) ||
        rel_path[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid path");
        return ESP_FAIL;
    }
    extract_query_param(req, "newName", new_name, sizeof(new_name));
    if (!is_safe_bare_name(new_name)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid name");
        return ESP_FAIL;
    }

    char fs_path[kMaxPathLen + 16];
    build_storage_path(volume, rel_path, fs_path, sizeof(fs_path));

    struct stat st;
    if (stat(fs_path, &st) != 0 || S_ISDIR(st.st_mode)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "not a file");
        return ESP_FAIL;
    }

    // Rename stays within the same directory as the source - a plain-name swap, not a move.
    char rel_dir[kMaxPathLen];
    snprintf(rel_dir, sizeof(rel_dir), "%s", rel_path);
    char *last_slash = strrchr(rel_dir, '/');
    char new_rel_path[kMaxPathLen + 128];
    if (last_slash != nullptr) {
        *last_slash = '\0';
        snprintf(new_rel_path, sizeof(new_rel_path), "%s/%s", rel_dir, new_name);
    } else {
        snprintf(new_rel_path, sizeof(new_rel_path), "%s", new_name);
    }

    char new_fs_path[kMaxPathLen + 128 + 16];
    build_storage_path(volume, new_rel_path, new_fs_path, sizeof(new_fs_path));

    struct stat existing;
    if (stat(new_fs_path, &existing) == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "target already exists");
        return ESP_FAIL;
    }
    if (rename(fs_path, new_fs_path) != 0) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "could not rename file");
        return ESP_FAIL;
    }
    httpd_resp_sendstr(req, "{\"renamed\":true}");
    return ESP_OK;
}

// ---- /api/preset, /api/presets ----

esp_err_t preset_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");

    char raw_name[presetpaths::kMaxNameLen] = {};
    const bool has_name_param = extract_preset_name_param(req, raw_name, sizeof(raw_name));

    char resolved_path[kMaxPathLen];
    char resolved_name[presetpaths::kMaxNameLen] = {};
    if (has_name_param && presetpaths::isSafeName(raw_name)) {
        presetpaths::pathForName(raw_name, resolved_path, sizeof(resolved_path));
        snprintf(resolved_name, sizeof(resolved_name), "%s", raw_name);
    } else {
        presetpaths::resolveActivePath(resolved_path, sizeof(resolved_path), resolved_name, sizeof(resolved_name));
    }

    // Lets the browser learn which preset this is in the same round trip.
    httpd_resp_set_hdr(req, "X-Preset-Name", resolved_name);

    FILE *file = fopen(resolved_path, "rb");
    if (file == nullptr) {
        httpd_resp_sendstr(req, kEmptyPresetJson);
        return ESP_OK;
    }
    return stream_file_chunked(req, file);
}

esp_err_t preset_select_handler(httpd_req_t *req) {
    char raw_name[presetpaths::kMaxNameLen] = {};
    const bool has_name_param = extract_preset_name_param(req, raw_name, sizeof(raw_name));
    if (has_name_param && !presetpaths::isSafeName(raw_name)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid preset name");
        return ESP_FAIL;
    }

    char preset_path[kMaxPathLen] = {};
    if (has_name_param) {
        presetpaths::pathForName(raw_name, preset_path, sizeof(preset_path));
    } else {
        snprintf(preset_path, sizeof(preset_path), "%s", presetpaths::unnamedPath());
    }
    struct stat preset_stat = {};
    if (stat(preset_path, &preset_stat) != 0 || S_ISDIR(preset_stat.st_mode)) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "preset not found");
        return ESP_FAIL;
    }

    const bool queued = has_name_param ? Feature::Synth::request_preset_name(raw_name)
                                       : Feature::Synth::request_current_preset();
    if (!queued) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "preset reload unavailable");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"selected\":true}");
    return ESP_OK;
}

esp_err_t preset_post_handler(httpd_req_t *req) {
    if (req->content_len == 0 || req->content_len > kMaxPresetBodyLen) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid preset size");
        return ESP_FAIL;
    }

    char raw_name[presetpaths::kMaxNameLen] = {};
    const bool has_name_param = extract_preset_name_param(req, raw_name, sizeof(raw_name));
    if (has_name_param && !presetpaths::isSafeName(raw_name)) {
        // Unlike GET, a save with an invalid name is rejected outright rather than silently
        // falling back to the unnamed slot.
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid preset name");
        return ESP_FAIL;
    }

    auto *body = static_cast<char *>(heap_caps_malloc(req->content_len, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (body == nullptr) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_FAIL;
    }

    size_t received = 0;
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, body + received, req->content_len - received);
        if (ret <= 0) {
            heap_caps_free(body);
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) httpd_resp_send_408(req);
            return ESP_FAIL;
        }
        received += static_cast<size_t>(ret);
    }

    char target_path[kMaxPathLen];
    if (has_name_param) {
        presetpaths::pathForName(raw_name, target_path, sizeof(target_path));
    } else {
        snprintf(target_path, sizeof(target_path), "%s", presetpaths::unnamedPath());
    }

    // Ignore mkdir failure: directory is created by Feature::Synth::init() on first boot, this
    // is only a defensive fallback if a save races ahead of that.
    mkdir(presetpaths::kDir, 0775);

    FILE *file = fopen(target_path, "wb");
    if (file == nullptr) {
        heap_caps_free(body);
        ESP_LOGE(WEB_API_TAG, "%s could not be opened for writing", target_path);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "could not write preset");
        return ESP_FAIL;
    }
    fwrite(body, 1, received, file);
    fclose(file);
    heap_caps_free(body);

    const bool queued = has_name_param ? Feature::Synth::request_preset_name(raw_name)
                                       : Feature::Synth::request_current_preset();
    if (!queued) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "preset reload unavailable");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"saved\":true}");
    return ESP_OK;
}

// POST /api/wavetable?widget=<Wavetable widget id>, body {"wavetables":[...]} (same
// base64 int16 format as the preset, see Wavetable.h). Live audition for the
// web wavetable editor: the bank is swapped into the running graph (see
// SynthEngine::postLiveWavetable), nothing is written to flash - the preset
// save on "Apply" makes it permanent, "Cancel" posts the original bank back.
esp_err_t wavetable_live_handler(httpd_req_t *req) {
    // 16 tables x ~350 chars base64 + JSON overhead - generous headroom.
    constexpr size_t kMaxWavetableBodyLen = 16 * 1024;
    if (req->content_len == 0 || req->content_len > kMaxWavetableBodyLen) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid wavetable size");
        return ESP_FAIL;
    }
    char widget_id[24] = {};
    if (!extract_query_param(req, "widget", widget_id, sizeof(widget_id)) || widget_id[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing widget");
        return ESP_FAIL;
    }

    auto *body = static_cast<char *>(heap_caps_malloc(req->content_len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (body == nullptr) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_FAIL;
    }
    size_t received = 0;
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, body + received, req->content_len - received);
        if (ret <= 0) {
            heap_caps_free(body);
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) httpd_resp_send_408(req);
            return ESP_FAIL;
        }
        received += static_cast<size_t>(ret);
    }
    body[received] = '\0';

    cJSON *root = cJSON_ParseWithLength(body, received);
    heap_caps_free(body);
    wavetable::Bank *bank =
        root != nullptr ? wavetable::parse(cJSON_GetObjectItemCaseSensitive(root, "wavetables"), false) : nullptr;
    cJSON_Delete(root);
    if (bank == nullptr) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid wavetables");
        return ESP_FAIL;
    }
    // postLiveWavetable() owns the bank reference from here on, even on failure.
    if (!engine().postLiveWavetable(widget_id, bank)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "wavetable queue full");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"applied\":true}");
    return ESP_OK;
}

esp_err_t presets_list_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");

    DIR *dir = opendir(presetpaths::kDir);
    if (dir == nullptr) {
        httpd_resp_sendstr(req, "{\"presets\":[]}");
        return ESP_OK;
    }

    char json[1024];
    int offset = snprintf(json, sizeof(json), "{\"presets\":[");
    bool first = true;
    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr && offset > 0 && static_cast<size_t>(offset) < sizeof(json)) {
        const char *fname = entry->d_name;
        if (fname[0] == '.') continue;
        const size_t len = strlen(fname);
        if (len < 6 || strcmp(fname + len - 5, ".json") != 0) continue;
        if (strcmp(fname, "current.json") == 0) continue; // unnamed slot is not a "named" preset

        char name_only[presetpaths::kMaxNameLen];
        size_t name_len = len - 5;
        if (name_len >= sizeof(name_only)) name_len = sizeof(name_only) - 1;
        memcpy(name_only, fname, name_len);
        name_only[name_len] = '\0';

        offset += snprintf(json + offset, sizeof(json) - static_cast<size_t>(offset), "%s\"%s\"",
                            first ? "" : ",", name_only);
        first = false;
    }
    closedir(dir);

    if (offset > 0 && static_cast<size_t>(offset) < sizeof(json)) {
        snprintf(json + offset, sizeof(json) - static_cast<size_t>(offset), "]}");
        httpd_resp_sendstr(req, json);
    } else {
        httpd_resp_sendstr(req, "{\"presets\":[]}");
    }
    return ESP_OK;
}

// ---- /api/backup (full system backup: named presets + current + last-active marker) ----

namespace {
// Escapes '"' and '\' so a preset/marker name is safe to embed inside a JSON string literal.
// presetpaths::isSafeName() already excludes control characters and path separators but not
// quotes, so this stays defensive.
void json_escape_to_buf(const char *s, char *out, size_t out_size) {
    size_t o = 0;
    for (const char *p = s; *p != '\0' && o + 2 < out_size; ++p) {
        if (*p == '"' || *p == '\\') {
            out[o++] = '\\';
        }
        out[o++] = *p;
    }
    out[o] = '\0';
}
} // namespace

esp_err_t backup_export_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"espressynth-backup.json\"");

    char last_active[presetpaths::kMaxNameLen] = {};
    presetpaths::readLastMarker(last_active, sizeof(last_active));
    char last_active_escaped[presetpaths::kMaxNameLen * 2] = {};
    json_escape_to_buf(last_active, last_active_escaped, sizeof(last_active_escaped));

    char header[presetpaths::kMaxNameLen * 2 + 64];
    snprintf(header, sizeof(header), "{\"version\":1,\"lastActive\":\"%s\",\"presets\":{", last_active_escaped);
    if (httpd_resp_send_chunk(req, header, strlen(header)) != ESP_OK) {
        return ESP_FAIL;
    }

    DIR *dir = opendir(presetpaths::kDir);
    if (dir == nullptr) {
        httpd_resp_send_chunk(req, "}}", 2);
        httpd_resp_send_chunk(req, nullptr, 0);
        return ESP_OK;
    }

    auto *buffer = static_cast<char *>(heap_caps_malloc(4096, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (buffer == nullptr) {
        closedir(dir);
        httpd_resp_send_chunk(req, nullptr, 0);
        return ESP_FAIL;
    }

    bool first = true;
    bool write_error = false;
    struct dirent *entry;
    while (!write_error && (entry = readdir(dir)) != nullptr) {
        const char *fname = entry->d_name;
        if (fname[0] == '.') continue;
        const size_t len = strlen(fname);
        if (len < 6 || strcmp(fname + len - 5, ".json") != 0) continue;

        char entry_path[kMaxPathLen];
        snprintf(entry_path, sizeof(entry_path), "%s/%s", presetpaths::kDir, fname);
        FILE *file = fopen(entry_path, "rb");
        if (file == nullptr) continue;

        char name_escaped[presetpaths::kMaxNameLen * 2] = {};
        json_escape_to_buf(fname, name_escaped, sizeof(name_escaped));

        char key_prefix[presetpaths::kMaxNameLen * 2 + 8];
        snprintf(key_prefix, sizeof(key_prefix), "%s\"%s\":", first ? "" : ",", name_escaped);
        if (httpd_resp_send_chunk(req, key_prefix, strlen(key_prefix)) != ESP_OK) {
            write_error = true;
            fclose(file);
            break;
        }
        first = false;

        size_t bytes_read;
        while ((bytes_read = fread(buffer, 1, 4096, file)) > 0) {
            if (httpd_resp_send_chunk(req, buffer, bytes_read) != ESP_OK) {
                write_error = true;
                break;
            }
        }
        fclose(file);
    }
    closedir(dir);
    heap_caps_free(buffer);

    if (write_error) {
        httpd_resp_send_chunk(req, nullptr, 0);
        return ESP_FAIL;
    }

    httpd_resp_send_chunk(req, "}}", 2);
    httpd_resp_send_chunk(req, nullptr, 0);
    return ESP_OK;
}

constexpr size_t kMaxBackupBodyLen = 4 * 1024 * 1024;

esp_err_t backup_restore_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");

    if (req->content_len == 0 || req->content_len > kMaxBackupBodyLen) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid backup size");
        return ESP_FAIL;
    }

    auto *body = static_cast<char *>(heap_caps_malloc(req->content_len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (body == nullptr) {
        body = static_cast<char *>(heap_caps_malloc(req->content_len + 1, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
    if (body == nullptr) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_FAIL;
    }

    size_t received = 0;
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, body + received, req->content_len - received);
        if (ret <= 0) {
            heap_caps_free(body);
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) httpd_resp_send_408(req);
            return ESP_FAIL;
        }
        received += static_cast<size_t>(ret);
    }
    body[received] = '\0';

    cJSON *root = cJSON_ParseWithLength(body, received);
    heap_caps_free(body);
    if (root == nullptr) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
        return ESP_FAIL;
    }

    cJSON *presets = cJSON_GetObjectItemCaseSensitive(root, "presets");
    if (!cJSON_IsObject(presets)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing presets object");
        return ESP_FAIL;
    }

    // Ignore mkdir failure: directory is created by Feature::Synth::init() on first boot, this
    // is only a defensive fallback (see preset_post_handler's identical call).
    mkdir(presetpaths::kDir, 0775);

    int restored_count = 0;
    cJSON *entry = nullptr;
    cJSON_ArrayForEach(entry, presets) {
        const char *fname = entry->string;
        if (fname == nullptr) continue;

        char target_path[kMaxPathLen];
        const size_t flen = strlen(fname);
        if (strcmp(fname, "current.json") == 0) {
            snprintf(target_path, sizeof(target_path), "%s", presetpaths::unnamedPath());
        } else if (flen > 5 && strcmp(fname + flen - 5, ".json") == 0) {
            char name_only[presetpaths::kMaxNameLen];
            size_t name_len = flen - 5;
            if (name_len >= sizeof(name_only)) name_len = sizeof(name_only) - 1;
            memcpy(name_only, fname, name_len);
            name_only[name_len] = '\0';
            if (!presetpaths::isSafeName(name_only)) continue;
            presetpaths::pathForName(name_only, target_path, sizeof(target_path));
        } else {
            continue; // unrecognized key, skip defensively
        }

        char *serialized = cJSON_PrintUnformatted(entry);
        if (serialized == nullptr) continue;

        FILE *file = fopen(target_path, "wb");
        if (file != nullptr) {
            fwrite(serialized, 1, strlen(serialized), file);
            fclose(file);
            ++restored_count;
        }
        cJSON_free(serialized);
    }

    cJSON *last_active_json = cJSON_GetObjectItemCaseSensitive(root, "lastActive");
    if (cJSON_IsString(last_active_json) && last_active_json->valuestring[0] != '\0' &&
        presetpaths::isSafeName(last_active_json->valuestring)) {
        presetpaths::writeLastMarker(last_active_json->valuestring);
    }

    cJSON_Delete(root);

    // Reload whatever is now the active preset so the running engine reflects the restore.
    char active_path[kMaxPathLen];
    char active_name[presetpaths::kMaxNameLen] = {};
    presetpaths::resolveActivePath(active_path, sizeof(active_path), active_name, sizeof(active_name));
    if (active_name[0] != '\0') {
        Feature::Synth::request_preset_name(active_name);
    } else {
        Feature::Synth::request_current_preset();
    }

    char response[64];
    snprintf(response, sizeof(response), "{\"restored\":true,\"count\":%d}", restored_count);
    httpd_resp_sendstr(req, response);
    return ESP_OK;
}

// ---- /api/samples, /api/soundfonts ----

esp_err_t samples_list_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");

    DIR *dir = opendir(samplepaths::kDir);
    if (dir == nullptr) {
        httpd_resp_sendstr(req, "{\"samples\":[]}");
        return ESP_OK;
    }

    char json[1024];
    int offset = snprintf(json, sizeof(json), "{\"samples\":[");
    bool first = true;
    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr && offset > 0 && static_cast<size_t>(offset) < sizeof(json)) {
        const char *fname = entry->d_name;
        if (fname[0] == '.') continue;
        const size_t len = strlen(fname);
        if (len < 5 || strcmp(fname + len - 4, ".wav") != 0) continue;
        offset += snprintf(json + offset, sizeof(json) - static_cast<size_t>(offset), "%s\"%s\"",
                            first ? "" : ",", fname);
        first = false;
    }
    closedir(dir);

    if (offset > 0 && static_cast<size_t>(offset) < sizeof(json)) {
        snprintf(json + offset, sizeof(json) - static_cast<size_t>(offset), "]}");
        httpd_resp_sendstr(req, json);
    } else {
        httpd_resp_sendstr(req, "{\"samples\":[]}");
    }
    return ESP_OK;
}

bool has_sf2_extension(const char *name) {
    const size_t len = name != nullptr ? strlen(name) : 0;
    if (len < 4) return false;
    const char *ext = name + len - 4;
    return ext[0] == '.' && (ext[1] == 's' || ext[1] == 'S') && (ext[2] == 'f' || ext[2] == 'F') && ext[3] == '2';
}

void append_soundfonts(const char *root, const char *volume, char *json, size_t size, int &offset, bool &first) {
    char dir_path[256];
    snprintf(dir_path, sizeof(dir_path), "%s/soundfonts", root);
    DIR *dir = opendir(dir_path);
    if (dir == nullptr) return;
    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr && offset > 0 && static_cast<size_t>(offset) < size) {
        if (entry->d_name[0] == '.' || !has_sf2_extension(entry->d_name)) continue;
        // Headroom for GCC's -Wformat-truncation: dir_path (256) and entry->d_name (dirent's
        // own array, up to 255) both worst-cased by the compiler.
        char full_path[256 + 1 + 255];
        snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);
        struct stat st = {};
        if (stat(full_path, &st) != 0 || S_ISDIR(st.st_mode)) continue;
        offset += snprintf(json + offset, size - static_cast<size_t>(offset),
                            "%s{\"volume\":\"%s\",\"path\":\"soundfonts/%s\",\"name\":\"%s\",\"size\":%llu}",
                            first ? "" : ",", volume, entry->d_name, entry->d_name,
                            static_cast<unsigned long long>(st.st_size));
        first = false;
    }
    closedir(dir);
}

esp_err_t soundfonts_list_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    char json[4096];
    int offset = snprintf(json, sizeof(json), "{\"soundfonts\":[");
    bool first = true;
    append_soundfonts(Feature::LittleFS::MOUNT_POINT, "flash", json, sizeof(json), offset, first);
    if (Feature::SDCard::is_mounted()) {
        append_soundfonts(Feature::SDCard::MOUNT_POINT, "sd", json, sizeof(json), offset, first);
    }
    if (offset <= 0 || static_cast<size_t>(offset) >= sizeof(json) - 2) {
        httpd_resp_sendstr(req, "{\"soundfonts\":[]}");
        return ESP_OK;
    }
    snprintf(json + offset, sizeof(json) - static_cast<size_t>(offset), "]}");
    httpd_resp_sendstr(req, json);
    return ESP_OK;
}

// ---- /api/midi/devices ----

// The frontend (main.js's renderMidiDevices()) expects "devices" as a flat array of display-
// name strings - NOT objects - and treats any name starting with "BLE-MIDI " as a BLE chip
// (right-click to disconnect). USB Device MIDI (this board acting as a class-compliant MIDI
// device a PC/host would see) is deliberately not listed here - it is the reverse relationship
// of "an external keyboard connected to this board", which is what this list is for.
esp_err_t midi_devices_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    char json[256];
    int offset = snprintf(json, sizeof(json), "{\"devices\":[");
    bool first = true;
#if defined(CONFIG_APP_ENABLE_USB_HOST) && defined(CONFIG_USB_HOST_CLASS_MIDI)
    if (Feature::MIDI::is_connected()) {
        offset += snprintf(json + offset, sizeof(json) - static_cast<size_t>(offset), "%s\"USB MIDI (device %u)\"",
                            first ? "" : ",", static_cast<unsigned>(Feature::MIDI::connected_device_address()));
        first = false;
    }
#endif
#if defined(CONFIG_APP_ENABLE_BLUETOOTH) && defined(CONFIG_BLE_ENABLE_MIDI_HOST)
    const uint8_t ble_count = Feature::BLE::MIDI::connected_count();
    for (uint8_t i = 0; i < ble_count && offset > 0 && static_cast<size_t>(offset) < sizeof(json); ++i) {
        offset += snprintf(json + offset, sizeof(json) - static_cast<size_t>(offset), "%s\"BLE-MIDI %u\"",
                            first ? "" : ",", static_cast<unsigned>(i + 1));
        first = false;
    }
#endif
    if (offset > 0 && static_cast<size_t>(offset) < sizeof(json)) {
        snprintf(json + offset, sizeof(json) - static_cast<size_t>(offset), "]}");
        httpd_resp_sendstr(req, json);
    } else {
        httpd_resp_sendstr(req, "{\"devices\":[]}");
    }
    return ESP_OK;
}

#if defined(CONFIG_APP_ENABLE_BLUETOOTH)
// ---- /api/ble ----

// GET: BLE state for the Settings > Bluetooth dialog (main.js renderBleSettings()). Only
// registered when BLE is compiled in, so the frontend hides the section on a 404.
esp_err_t ble_get_handler(httpd_req_t *req) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ready", Feature::BLE::is_ready());
    cJSON_AddBoolToObject(root, "scanning", Feature::BLE::is_scanning());
    char mac[18];

    cJSON *known = cJSON_AddArrayToObject(root, "known");
    const int known_count = Feature::BLE::known_device_count();
    for (int i = 0; i < known_count; ++i) {
        const Feature::BLE::KnownDevice device = Feature::BLE::known_device_at(i);
        Feature::BLE::format_mac(device.addr, mac, sizeof(mac));
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "mac", mac);
        cJSON_AddStringToObject(item, "name", device.name);
        cJSON_AddBoolToObject(item, "auto", device.autoconnect);
        cJSON_AddBoolToObject(item, "connected", Feature::BLE::is_connected_mac(device.addr));
        cJSON_AddItemToArray(known, item);
    }

    cJSON *connected = cJSON_AddArrayToObject(root, "connected");
    const int connected_count = Feature::BLE::connected_device_count();
    for (int i = 0; i < connected_count; ++i) {
        const Feature::BLE::ConnectedDevice device = Feature::BLE::connected_device_at(i);
        Feature::BLE::format_mac(device.addr, mac, sizeof(mac));
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "mac", mac);
        cJSON_AddStringToObject(item, "name", device.name);
        cJSON_AddStringToObject(item, "cls", device.device_class);
        cJSON_AddItemToArray(connected, item);
    }

    cJSON *found = cJSON_AddArrayToObject(root, "found");
    const int found_count = Feature::BLE::discovered_device_count();
    for (int i = 0; i < found_count; ++i) {
        const Feature::BLE::DiscoveredDevice device = Feature::BLE::discovered_device_at(i);
        Feature::BLE::format_mac(device.addr, mac, sizeof(mac));
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "mac", mac);
        cJSON_AddStringToObject(item, "name", device.name);
        cJSON_AddStringToObject(item, "cls", device.device_class);
        cJSON_AddNumberToObject(item, "rssi", device.rssi);
        cJSON_AddItemToArray(found, item);
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == nullptr) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "encode failed");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    cJSON_free(json);
    return ESP_OK;
}

// POST /api/ble?action=scan | connect|disconnect|forget&mac=AA:.. | auto&mac=AA:..&on=0|1
// Non-blocking: a scan only starts here; the browser polls GET until "scanning" is false.
esp_err_t ble_post_handler(httpd_req_t *req) {
    char action[16] = {};
    char mac[24] = {};
    char on[4] = {};
    extract_query_param(req, "action", action, sizeof(action));
    extract_query_param(req, "mac", mac, sizeof(mac));
    extract_query_param(req, "on", on, sizeof(on));

    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (!Feature::BLE::is_ready()) err = ESP_ERR_INVALID_STATE;
    else if (strcmp(action, "scan") == 0) err = Feature::BLE::start_scan(5000);
    else if (strcmp(action, "connect") == 0) err = Feature::BLE::connect_by_mac(mac);
    else if (strcmp(action, "disconnect") == 0) err = Feature::BLE::disconnect_by_mac(mac);
    else if (strcmp(action, "forget") == 0) err = Feature::BLE::forget(mac);
    else if (strcmp(action, "auto") == 0) err = Feature::BLE::set_autoconnect(mac, strcmp(on, "1") == 0);

    httpd_resp_set_type(req, "application/json");
    if (err != ESP_OK) {
        char json[96];
        snprintf(json, sizeof(json), "{\"error\":\"%s\"}",
                 err == ESP_ERR_NOT_FOUND ? "unknown device" : esp_err_to_name(err));
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, json);
        return ESP_OK;
    }
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}
#endif

// ---- /api/masterclock ----

// One-shot hydration for a freshly-opened tab (initial page load, or any
// reconnect): plain synchronous GET, so - unlike trying to push a WS frame
// during the /ws upgrade handshake itself (tried first; the frame never
// actually reached the client, confirmed live) - there is no handshake-
// timing race to get wrong. Live updates while already connected still go
// out over the "masterClock" WS broadcast in broadcast_status_tick() below.
esp_err_t masterclock_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    char json[192];
    const int n = synth::formatMasterClockJson(json, sizeof(json));
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(json)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "encode failed");
        return ESP_FAIL;
    }
    httpd_resp_sendstr(req, json);
    return ESP_OK;
}

// ---- /api/settings/device ----

// One-shot hydration for the Settings dialog (device name + the two preset-
// nav Learn bindings, DeviceSettings.h) - same "plain GET, no WS handshake
// race" reasoning as masterclock_handler above. Saved via the "@deviceSettings"
// paramUpdate branch in the /ws handler below, not a POST here.
esp_err_t device_settings_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    char json[400];
    int n = synth::formatDeviceSettingsJson(json, sizeof(json));
    if (n <= 1 || static_cast<size_t>(n) >= sizeof(json)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "encode failed");
        return ESP_FAIL;
    }
    // WS2812 string (features/io/led_strip_manager.hpp) rides along here instead of its own
    // route - the URI handler table is nearly full. ledActiveCount is what the running driver
    // addresses (settings only apply after a reboot), which is what RGB LED widgets validate against.
    const auto &led = Feature::LedStrip::settings();
    snprintf(json + n - 1, sizeof(json) - static_cast<size_t>(n - 1),
             ",\"ledEnabled\":%s,\"ledPin\":%d,\"ledCount\":%u,\"ledActiveCount\":%u,\"ledMaxCount\":%u}",
             Feature::LedStrip::kEnabled ? "true" : "false", led.pin,
             static_cast<unsigned>(led.count), static_cast<unsigned>(Feature::LedStrip::active_count()),
             static_cast<unsigned>(Feature::LedStrip::kMaxLeds));
    httpd_resp_sendstr(req, json);
    return ESP_OK;
}

// ---- /api/version, /api/diagnostics/memory ----

esp_err_t version_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    const esp_app_desc_t *desc = esp_app_get_description();
    char json[320];
    snprintf(json, sizeof(json),
             "{\"version\":\"%s\",\"wm8960\":%s,\"ads1115\":%s,\"gp8413\":%s,\"gpioInput\":%s,"
             "\"gpioOutputs\":%s,\"gitVersion\":\"%s\"}",
             FIRMWARE_VERSION,
#if defined(CONFIG_ONBOARD_AUDIO_WM8960)
             "true",
#else
             "false",
#endif
#if defined(CONFIG_APP_ADS1115_SUPPORT)
             "true",
#else
             "false",
#endif
#if defined(CONFIG_APP_GP8413_SUPPORT)
             "true",
#else
             "false",
#endif
#if defined(CONFIG_APP_ENABLE_GPIO_INPUT)
             "true",
#else
             "false",
#endif
#if defined(CONFIG_APP_ENABLE_GPIO_OUTPUTS)
             "true",
#else
             "false",
#endif
             desc->version);
    httpd_resp_sendstr(req, json);
    return ESP_OK;
}

esp_err_t i2c_devices_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    char json[1536];
    int len = snprintf(json, sizeof(json), "{\"devices\":[");
    const size_t count = Feature::IO::I2CDeviceRegistry::count();
    for (size_t i = 0; i < count && len > 0 && static_cast<size_t>(len) < sizeof(json); ++i) {
        Feature::IO::DetectedI2CDevice dev;
        if (!Feature::IO::I2CDeviceRegistry::get(i, dev)) continue;
        len += snprintf(json + len, sizeof(json) - static_cast<size_t>(len),
                         "%s{\"name\":\"%s\",\"bus\":%u,\"address\":\"0x%02X\"}", i == 0 ? "" : ",", dev.name,
                         dev.bus_port, dev.address);
    }
    if (len < 0 || static_cast<size_t>(len) >= sizeof(json) - 2) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "encode failed");
        return ESP_FAIL;
    }
    snprintf(json + len, sizeof(json) - static_cast<size_t>(len), "]}");
    httpd_resp_sendstr(req, json);
    return ESP_OK;
}

// ---- /api/system/gpio, /api/settings/gpio ----

// Valid pin range + reserved pins (Board::is_pin_reserved / WM8960) + this device's currently
// configured Digital In/Out pins (GpioSettings.h) - the Settings dialog computes "free" pins
// as validRange minus reserved minus configured-by-a-different-pin. Static/compile-time data
// plus one small dynamic list - cheap enough for a plain GET, same reasoning as
// i2c_devices_handler/masterclock_handler.
esp_err_t gpio_info_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    char json[2048]; // reserved[] can list every SoC pin, configured[] up to kMaxGpioEntries
    int len = snprintf(json, sizeof(json), "{\"minPin\":0,\"maxPin\":%d,\"reserved\":[",
                        SOC_GPIO_PIN_COUNT - 1);
    bool first = true;
    for (int pin = 0; pin < SOC_GPIO_PIN_COUNT; ++pin) {
        const auto gpio = static_cast<gpio_num_t>(pin);
        if (!GPIO_IS_VALID_GPIO(gpio) ||
            (!Feature::IO::is_pin_reserved_for_gpio_settings(gpio) && pin != Feature::LedStrip::settings().pin)) continue;
        len += snprintf(json + len, sizeof(json) - static_cast<size_t>(len), "%s%d", first ? "" : ",", pin);
        first = false;
    }
    len += snprintf(json + len, sizeof(json) - static_cast<size_t>(len), "],\"configured\":[");
    first = true;
    const auto &gs = Feature::IO::gpioSettings();
    for (uint8_t i = 0; i < gs.count; ++i) {
        len += snprintf(json + len, sizeof(json) - static_cast<size_t>(len),
                         "%s{\"pin\":%d,\"direction\":\"%s\",\"pull\":\"%s\",\"invert\":%s}", first ? "" : ",",
                         gs.entries[i].pin,
                         gs.entries[i].direction == Feature::IO::GpioDirection::Output ? "out" : "in",
                         gs.entries[i].pull == Feature::IO::GpioPull::Up ? "up" :
                         gs.entries[i].pull == Feature::IO::GpioPull::Down ? "down" : "none",
                         gs.entries[i].invert ? "true" : "false");
        first = false;
    }
    snprintf(json + len, sizeof(json) - static_cast<size_t>(len), "]}");
    httpd_resp_sendstr(req, json);
    return ESP_OK;
}

// One-shot hydration for the Settings dialog's GPIO section (GpioSettings.h) - same "plain
// GET, no WS handshake race" reasoning as device_settings_handler above. Saved via the
// "@gpioSettings" paramUpdate branch in the /ws handler below, not a POST here.
esp_err_t gpio_settings_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    char json[1536]; // up to kMaxGpioEntries entries
    const int n = Feature::IO::formatGpioSettingsJson(json, sizeof(json));
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(json)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "encode failed");
        return ESP_FAIL;
    }
    httpd_resp_sendstr(req, json);
    return ESP_OK;
}

esp_err_t memory_diagnostics_handler(httpd_req_t *req) {
    memorydiag::State state;
    memorydiag::read(state);
    const memorydiag::HeapSnapshot current = memorydiag::snapshot();

    httpd_resp_set_type(req, "application/json");
    char json[768];
    int length = snprintf(
        json, sizeof(json),
        "{\"generation\":%" PRIu32 ",\"phase\":\"%s\",\"phaseId\":%u,\"inProgress\":%s,\"success\":%s,"
        "\"current\":{\"internalFree\":%" PRIu32 ",\"internalLargest\":%" PRIu32 ",\"internalMinimum\":%" PRIu32
        ",\"psramFree\":%" PRIu32 ",\"psramLargest\":%" PRIu32 ",\"psramMinimum\":%" PRIu32
        ",\"synthStackHighWaterBytes\":%" PRIu32 "},"
        "\"graph\":{\"instances\":%" PRIu32 ",\"nodeInstanceBytes\":%" PRIu32 ",\"blockBuffers\":%" PRIu32
        ",\"blockBufferBytes\":%" PRIu32 ",\"instancesInternal\":%s,\"outputPsramFallbacks\":%" PRIu32
        "},\"phases\":{",
        state.generation, memorydiag::phaseName(state.phase), static_cast<unsigned>(state.phase),
        state.inProgress ? "true" : "false", state.success ? "true" : "false", current.internalFree,
        current.internalLargest, current.internalMinimum, current.psramFree, current.psramLargest,
        current.psramMinimum, current.synthStackHighWaterBytes, state.graph.instanceCount,
        state.graph.nodeInstanceBytes, state.graph.blockBufferCount, state.graph.blockBufferBytes,
        state.graph.instancesInternal ? "true" : "false", state.graph.outputPsramFallbacks);
    if (length < 0 || static_cast<size_t>(length) >= sizeof(json) ||
        httpd_resp_send_chunk(req, json, static_cast<ssize_t>(length)) != ESP_OK) {
        return ESP_FAIL;
    }

    bool first = true;
    for (uint8_t i = 0; i < static_cast<uint8_t>(memorydiag::ReloadPhase::Count); ++i) {
        if ((state.validPhaseMask & (1U << i)) == 0) continue;
        const auto phase = static_cast<memorydiag::ReloadPhase>(i);
        const memorydiag::HeapSnapshot &snap = state.phases[i];
        length = snprintf(json, sizeof(json),
                           "%s\"%s\":{\"internalFree\":%" PRIu32 ",\"internalLargest\":%" PRIu32
                           ",\"internalMinimum\":%" PRIu32 ",\"psramFree\":%" PRIu32 ",\"psramLargest\":%" PRIu32
                           ",\"psramMinimum\":%" PRIu32 ",\"synthStackHighWaterBytes\":%" PRIu32 "}",
                           first ? "" : ",", memorydiag::phaseName(phase), snap.internalFree, snap.internalLargest,
                           snap.internalMinimum, snap.psramFree, snap.psramLargest, snap.psramMinimum,
                           snap.synthStackHighWaterBytes);
        if (length < 0 || static_cast<size_t>(length) >= sizeof(json) ||
            httpd_resp_send_chunk(req, json, static_cast<ssize_t>(length)) != ESP_OK) {
            return ESP_FAIL;
        }
        first = false;
    }
    if (httpd_resp_send_chunk(req, "}}", 2) != ESP_OK) return ESP_FAIL;
    return httpd_resp_send_chunk(req, nullptr, 0);
}

// ---- /ws ----

void log_client_connected(httpd_req_t *req) {
    ESP_LOGI(WEB_API_TAG, "WebSocket client connected (fd=%d)", httpd_req_to_sockfd(req));
}

esp_err_t websocket_handler(httpd_req_t *req) {
    if (req->method == HTTP_GET) {
        // GET on a ws:// URI only fires once, for the upgrade handshake - esp_http_server has
        // already completed it by the time we get here.
        log_client_connected(req);
        return ESP_OK;
    }

    httpd_ws_frame_t frame = {};
    esp_err_t ret = httpd_ws_recv_frame(req, &frame, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(WEB_API_TAG, "WebSocket frame length could not be read: %s", esp_err_to_name(ret));
        return ret;
    }
    if (frame.len == 0) return ESP_OK; // control frame (ping/pong/close) without payload

    // +1 for a trailing NUL so a text payload can be logged/parsed as a C string.
    auto *payload = static_cast<uint8_t *>(malloc(frame.len + 1));
    if (payload == nullptr) return ESP_ERR_NO_MEM;
    frame.payload = payload;
    ret = httpd_ws_recv_frame(req, &frame, frame.len);
    if (ret == ESP_OK && frame.type == HTTPD_WS_TYPE_TEXT) {
        payload[frame.len] = '\0';
        ESP_LOGD(WEB_API_TAG, "WebSocket message: %s", reinterpret_cast<char *>(payload));

        bool handled_locally = false;
        cJSON *root = cJSON_Parse(reinterpret_cast<char *>(payload));
        if (root != nullptr) {
            cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
            if (cJSON_IsString(type) && strcmp(type->valuestring, "midiTest") == 0) {
                cJSON *midi_type = cJSON_GetObjectItemCaseSensitive(root, "midiType");
                cJSON *channel = cJSON_GetObjectItemCaseSensitive(root, "channel");
                cJSON *note = cJSON_GetObjectItemCaseSensitive(root, "note");
                cJSON *velocity = cJSON_GetObjectItemCaseSensitive(root, "velocity");
                if (cJSON_IsString(midi_type) && cJSON_IsNumber(channel) && cJSON_IsNumber(note) &&
                    cJSON_IsNumber(velocity) && channel->valueint >= 0 && channel->valueint < 16 &&
                    note->valueint >= 0 && note->valueint < 128 && velocity->valueint >= 0 &&
                    velocity->valueint < 128) {
                    const bool on = strcmp(midi_type->valuestring, "note_on") == 0;
                    const uint8_t status = static_cast<uint8_t>((on ? 0x90 : 0x80) | channel->valueint);
                    const uint8_t data1 = static_cast<uint8_t>(note->valueint);
                    const uint8_t data2 = static_cast<uint8_t>(velocity->valueint);
                    dispatch_midi(status, data1, data2);

                    UsbMidiEvent event = from_transport(status, data1, data2);
                    char midi_json[256];
                    if (event.toJson(midi_json, sizeof(midi_json),
                                      static_cast<uint32_t>(esp_timer_get_time() / 1000),
                                      "Web MIDI test") > 0) {
                        httpd_ws_frame_t reply = {};
                        reply.type = HTTPD_WS_TYPE_TEXT;
                        reply.payload = reinterpret_cast<uint8_t *>(midi_json);
                        reply.len = strlen(midi_json);
                        httpd_ws_send_frame(req, &reply);
                    }
                    handled_locally = true;
                }
            } else if (cJSON_IsString(type) && strcmp(type->valuestring, "paramUpdate") == 0) {
                // Master Clock (MasterClock.h) is engine-global, not part of
                // the live graph - handled right here on the httpd worker
                // task instead of going through live_param_queue() to the
                // audio render task below, so its flash write (Save/Learn,
                // infrequent) never risks blocking a real-time audio block.
                cJSON *widget_id = cJSON_GetObjectItemCaseSensitive(root, "widgetId");
                cJSON *params = cJSON_GetObjectItemCaseSensitive(root, "params");
                if (cJSON_IsString(widget_id) && strcmp(widget_id->valuestring, "@masterClock") == 0 &&
                    cJSON_IsObject(params)) {
                    auto &mc = synth::masterClock();
                    cJSON *bpm = cJSON_GetObjectItemCaseSensitive(params, "bpm");
                    cJSON *maxBpm = cJSON_GetObjectItemCaseSensitive(params, "maxBpm");
                    cJSON *round = cJSON_GetObjectItemCaseSensitive(params, "round");
                    cJSON *cc = cJSON_GetObjectItemCaseSensitive(params, "cc");
                    cJSON *channel = cJSON_GetObjectItemCaseSensitive(params, "channel");
                    if (cJSON_IsNumber(bpm) && bpm->valuedouble >= 0.0) mc.bpm = static_cast<float>(bpm->valuedouble);
                    if (cJSON_IsNumber(maxBpm) && maxBpm->valuedouble > 0.0) mc.maxBpm = static_cast<float>(maxBpm->valuedouble);
                    if (cJSON_IsBool(round)) mc.roundBpm = cJSON_IsTrue(round);
                    if (cJSON_IsNumber(cc)) mc.midiCc = cc->valueint;
                    if (cJSON_IsNumber(channel)) mc.midiChannel = channel->valueint;
                    synth::saveMasterClockToFs();
                    handled_locally = true;
                } else if (cJSON_IsString(widget_id) && strcmp(widget_id->valuestring, "@deviceSettings") == 0 &&
                           cJSON_IsObject(params)) {
                    // Device name, preset-nav/master-volume Learn bindings (DeviceSettings.h) - same
                    // "engine-global, not a graph widget, save right here off the render
                    // task" reasoning as @masterClock above.
                    auto &ds = synth::deviceSettings();
                    cJSON *name = cJSON_GetObjectItemCaseSensitive(params, "name");
                    cJSON *prevCc = cJSON_GetObjectItemCaseSensitive(params, "prevCc");
                    cJSON *prevChannel = cJSON_GetObjectItemCaseSensitive(params, "prevChannel");
                    cJSON *nextCc = cJSON_GetObjectItemCaseSensitive(params, "nextCc");
                    cJSON *nextChannel = cJSON_GetObjectItemCaseSensitive(params, "nextChannel");
                    if (cJSON_IsString(name)) snprintf(ds.name, sizeof(ds.name), "%s", name->valuestring);
                    if (cJSON_IsNumber(prevCc)) ds.prevCc = prevCc->valueint;
                    if (cJSON_IsNumber(prevChannel)) ds.prevChannel = prevChannel->valueint;
                    if (cJSON_IsNumber(nextCc)) ds.nextCc = nextCc->valueint;
                    if (cJSON_IsNumber(nextChannel)) ds.nextChannel = nextChannel->valueint;
                    cJSON *volume = cJSON_GetObjectItemCaseSensitive(params, "volume");
                    cJSON *volumeCc = cJSON_GetObjectItemCaseSensitive(params, "volumeCc");
                    cJSON *volumeChannel = cJSON_GetObjectItemCaseSensitive(params, "volumeChannel");
                    if (cJSON_IsNumber(volumeCc)) ds.volumeCc = volumeCc->valueint >= 0 && volumeCc->valueint <= 127 ? volumeCc->valueint : -1;
                    if (cJSON_IsNumber(volumeChannel)) ds.volumeChannel = volumeChannel->valueint >= 0 && volumeChannel->valueint <= 15 ? volumeChannel->valueint : -1;
                    if (cJSON_IsNumber(volume) && !synth::hasLearnedMasterVolume()) ds.volume = synth::clampDeviceVolume(static_cast<float>(volume->valuedouble));
                    synth::saveDeviceSettingsToFs();
                    // WS2812 string pin/count - persisted only, applied at the next boot.
                    cJSON *ledPin = cJSON_GetObjectItemCaseSensitive(params, "ledPin");
                    cJSON *ledCount = cJSON_GetObjectItemCaseSensitive(params, "ledCount");
                    if (Feature::LedStrip::kEnabled && (cJSON_IsNumber(ledPin) || cJSON_IsNumber(ledCount))) {
                        auto &led = Feature::LedStrip::settings();
                        if (cJSON_IsNumber(ledPin)) {
                            const int pin = ledPin->valueint;
                            const bool usable = pin >= 0 && pin < SOC_GPIO_PIN_COUNT &&
                                                GPIO_IS_VALID_OUTPUT_GPIO(static_cast<gpio_num_t>(pin)) &&
                                                !Feature::IO::is_pin_reserved_for_gpio_settings(static_cast<gpio_num_t>(pin)) &&
                                                Feature::IO::findGpioEntry(static_cast<int8_t>(pin)) == nullptr;
                            led.pin = usable ? static_cast<int8_t>(pin) : -1;
                        }
                        if (cJSON_IsNumber(ledCount)) {
                            const int count = ledCount->valueint;
                            led.count = static_cast<uint16_t>(count < 0 ? 0 : count > Feature::LedStrip::kMaxLeds ? Feature::LedStrip::kMaxLeds : count);
                        }
                        Feature::LedStrip::save_settings_to_fs();
                    }
                    handled_locally = true;
                } else if (cJSON_IsString(widget_id) && strcmp(widget_id->valuestring, "@gpioSettings") == 0 &&
                           cJSON_IsObject(params)) {
                    // GPIO pin table (GpioSettings.h) - engine-global, not a graph widget, same
                    // "save right here off the render task" reasoning as @masterClock above.
                    // gpio_config() itself is a fast, non-blocking register op - safe to call
                    // directly on the httpd worker thread, same class of call as Board::init()'s
                    // own gpio_config() at boot.
                    auto &gs = Feature::IO::gpioSettings();
                    cJSON *entries = cJSON_GetObjectItemCaseSensitive(params, "entries");
                    if (cJSON_IsArray(entries)) {
                        gs.count = 0;
                        cJSON *entry;
                        cJSON_ArrayForEach(entry, entries) {
                            if (gs.count >= Feature::IO::kMaxGpioEntries) break;
                            cJSON *pin = cJSON_GetObjectItemCaseSensitive(entry, "pin");
                            cJSON *dir = cJSON_GetObjectItemCaseSensitive(entry, "direction");
                            cJSON *pull = cJSON_GetObjectItemCaseSensitive(entry, "pull");
                            cJSON *invert = cJSON_GetObjectItemCaseSensitive(entry, "invert");
                            if (!cJSON_IsNumber(pin)) continue;
                            Feature::IO::GpioEntry &e = gs.entries[gs.count++];
                            e.pin = static_cast<int8_t>(pin->valueint);
                            e.direction = (cJSON_IsString(dir) && strcmp(dir->valuestring, "out") == 0)
                                              ? Feature::IO::GpioDirection::Output
                                              : Feature::IO::GpioDirection::Input;
                            e.pull = (cJSON_IsString(pull) && strcmp(pull->valuestring, "up") == 0)
                                         ? Feature::IO::GpioPull::Up
                                     : (cJSON_IsString(pull) && strcmp(pull->valuestring, "down") == 0)
                                         ? Feature::IO::GpioPull::Down
                                         : Feature::IO::GpioPull::None;
                            e.invert = cJSON_IsTrue(invert);
                        }
                    }
                    Feature::IO::apply_gpio_settings();
                    Feature::IO::saveGpioSettingsToFs();
                    handled_locally = true;
                }
            } else if (cJSON_IsString(type) && strcmp(type->valuestring, "filesInfo") == 0) {
                cJSON *volume_item = cJSON_GetObjectItemCaseSensitive(root, "volume");
                const bool is_sd = cJSON_IsString(volume_item) && strcmp(volume_item->valuestring, "sd") == 0;
                const char *volume_str = is_sd ? "sd" : "flash";
                char info_json[192];
                build_files_info_json(is_sd ? ExplorerVolume::Sd : ExplorerVolume::Flash, volume_str, info_json,
                                       sizeof(info_json));
                httpd_ws_frame_t reply = {};
                reply.type = HTTPD_WS_TYPE_TEXT;
                reply.payload = reinterpret_cast<uint8_t *>(info_json);
                reply.len = strlen(info_json);
                httpd_ws_send_frame(req, &reply);
                handled_locally = true;
            }
            cJSON_Delete(root);
        }

        // Forward browser->device messages (currently only "paramUpdate") to the engine's
        // render task for parsing/application - kept minimal here so the httpd worker never
        // touches the live graph.
        QueueHandle_t param_queue = live_param_queue();
        if (!handled_locally && param_queue != nullptr && frame.len < sizeof(LiveParamMsg::json)) {
            LiveParamMsg msg;
            memcpy(msg.json, payload, frame.len + 1); // includes the NUL appended above
            xQueueSend(param_queue, &msg, 0);
        }
    }
    free(payload);
    return ret;
}

} // namespace

/**
 * @brief Registers the synth engine's WS + REST routes on Feature::Web's shared httpd server.
 *        Call between Feature::Web::begin() and Feature::Web::register_catch_all() - see both
 *        their comments for why the order matters.
 */
inline void register_web_routes() {
    httpd_handle_t server = Feature::Web::handle();
    if (server == nullptr) return;

    static const httpd_uri_t ws_uri = {
        .uri = "/ws",
        .method = HTTP_GET,
        .handler = &websocket_handler,
        .user_ctx = nullptr,
        .is_websocket = true,
        .handle_ws_control_frames = false,
        .supported_subprotocol = nullptr,
    };
    httpd_register_uri_handler(server, &ws_uri);

    static const httpd_uri_t preset_get_uri = {
        .uri = "/api/preset", .method = HTTP_GET, .handler = &preset_get_handler, .user_ctx = nullptr,
        .is_websocket = false, .handle_ws_control_frames = false, .supported_subprotocol = nullptr};
    httpd_register_uri_handler(server, &preset_get_uri);

    static const httpd_uri_t preset_post_uri = {
        .uri = "/api/preset", .method = HTTP_POST, .handler = &preset_post_handler, .user_ctx = nullptr,
        .is_websocket = false, .handle_ws_control_frames = false, .supported_subprotocol = nullptr};
    httpd_register_uri_handler(server, &preset_post_uri);

    static const httpd_uri_t preset_select_uri = {
        .uri = "/api/preset/select", .method = HTTP_POST, .handler = &preset_select_handler,
        .user_ctx = nullptr, .is_websocket = false, .handle_ws_control_frames = false,
        .supported_subprotocol = nullptr};
    httpd_register_uri_handler(server, &preset_select_uri);

    static const httpd_uri_t presets_list_uri = {
        .uri = "/api/presets", .method = HTTP_GET, .handler = &presets_list_handler, .user_ctx = nullptr,
        .is_websocket = false, .handle_ws_control_frames = false, .supported_subprotocol = nullptr};
    httpd_register_uri_handler(server, &presets_list_uri);

    static const httpd_uri_t backup_export_uri = {
        .uri = "/api/backup", .method = HTTP_GET, .handler = &backup_export_handler, .user_ctx = nullptr,
        .is_websocket = false, .handle_ws_control_frames = false, .supported_subprotocol = nullptr};
    httpd_register_uri_handler(server, &backup_export_uri);

    static const httpd_uri_t backup_restore_uri = {
        .uri = "/api/backup/restore", .method = HTTP_POST, .handler = &backup_restore_handler,
        .user_ctx = nullptr, .is_websocket = false, .handle_ws_control_frames = false,
        .supported_subprotocol = nullptr};
    httpd_register_uri_handler(server, &backup_restore_uri);

    static const httpd_uri_t samples_list_uri = {
        .uri = "/api/samples", .method = HTTP_GET, .handler = &samples_list_handler, .user_ctx = nullptr,
        .is_websocket = false, .handle_ws_control_frames = false, .supported_subprotocol = nullptr};
    httpd_register_uri_handler(server, &samples_list_uri);

    static const httpd_uri_t soundfonts_list_uri = {
        .uri = "/api/soundfonts", .method = HTTP_GET, .handler = &soundfonts_list_handler, .user_ctx = nullptr,
        .is_websocket = false, .handle_ws_control_frames = false, .supported_subprotocol = nullptr};
    httpd_register_uri_handler(server, &soundfonts_list_uri);

    static const httpd_uri_t files_list_uri = {
        .uri = "/api/files/list", .method = HTTP_GET, .handler = &files_list_handler, .user_ctx = nullptr,
        .is_websocket = false, .handle_ws_control_frames = false, .supported_subprotocol = nullptr};
    httpd_register_uri_handler(server, &files_list_uri);

    static const httpd_uri_t files_upload_uri = {
        .uri = "/api/files/upload", .method = HTTP_POST, .handler = &files_upload_handler, .user_ctx = nullptr,
        .is_websocket = false, .handle_ws_control_frames = false, .supported_subprotocol = nullptr};
    httpd_register_uri_handler(server, &files_upload_uri);

    static const httpd_uri_t files_delete_uri = {
        .uri = "/api/files/delete", .method = HTTP_POST, .handler = &files_delete_handler, .user_ctx = nullptr,
        .is_websocket = false, .handle_ws_control_frames = false, .supported_subprotocol = nullptr};
    httpd_register_uri_handler(server, &files_delete_uri);

    static const httpd_uri_t files_download_uri = {
        .uri = "/api/files/download", .method = HTTP_GET, .handler = &files_download_handler, .user_ctx = nullptr,
        .is_websocket = false, .handle_ws_control_frames = false, .supported_subprotocol = nullptr};
    httpd_register_uri_handler(server, &files_download_uri);

    static const httpd_uri_t files_rename_uri = {
        .uri = "/api/files/rename", .method = HTTP_POST, .handler = &files_rename_handler, .user_ctx = nullptr,
        .is_websocket = false, .handle_ws_control_frames = false, .supported_subprotocol = nullptr};
    httpd_register_uri_handler(server, &files_rename_uri);

    static const httpd_uri_t midi_devices_uri = {
        .uri = "/api/midi/devices", .method = HTTP_GET, .handler = &midi_devices_handler, .user_ctx = nullptr,
        .is_websocket = false, .handle_ws_control_frames = false, .supported_subprotocol = nullptr};
    httpd_register_uri_handler(server, &midi_devices_uri);

    static const httpd_uri_t version_uri = {
        .uri = "/api/version", .method = HTTP_GET, .handler = &version_handler, .user_ctx = nullptr,
        .is_websocket = false, .handle_ws_control_frames = false, .supported_subprotocol = nullptr};
    httpd_register_uri_handler(server, &version_uri);

    static const httpd_uri_t i2c_devices_uri = {
        .uri = "/api/system/i2c", .method = HTTP_GET, .handler = &i2c_devices_handler, .user_ctx = nullptr,
        .is_websocket = false, .handle_ws_control_frames = false, .supported_subprotocol = nullptr};
    httpd_register_uri_handler(server, &i2c_devices_uri);

    static const httpd_uri_t masterclock_uri = {
        .uri = "/api/masterclock", .method = HTTP_GET, .handler = &masterclock_handler, .user_ctx = nullptr,
        .is_websocket = false, .handle_ws_control_frames = false, .supported_subprotocol = nullptr};
    httpd_register_uri_handler(server, &masterclock_uri);

    static const httpd_uri_t device_settings_uri = {
        .uri = "/api/settings/device", .method = HTTP_GET, .handler = &device_settings_handler, .user_ctx = nullptr,
        .is_websocket = false, .handle_ws_control_frames = false, .supported_subprotocol = nullptr};
    httpd_register_uri_handler(server, &device_settings_uri);

    static const httpd_uri_t gpio_info_uri = {
        .uri = "/api/system/gpio", .method = HTTP_GET, .handler = &gpio_info_handler, .user_ctx = nullptr,
        .is_websocket = false, .handle_ws_control_frames = false, .supported_subprotocol = nullptr};
    httpd_register_uri_handler(server, &gpio_info_uri);

    static const httpd_uri_t gpio_settings_uri = {
        .uri = "/api/settings/gpio", .method = HTTP_GET, .handler = &gpio_settings_handler, .user_ctx = nullptr,
        .is_websocket = false, .handle_ws_control_frames = false, .supported_subprotocol = nullptr};
    httpd_register_uri_handler(server, &gpio_settings_uri);

    static const httpd_uri_t memory_diagnostics_uri = {
        .uri = "/api/diagnostics/memory", .method = HTTP_GET, .handler = &memory_diagnostics_handler,
        .user_ctx = nullptr, .is_websocket = false, .handle_ws_control_frames = false,
        .supported_subprotocol = nullptr};
    httpd_register_uri_handler(server, &memory_diagnostics_uri);

    static const httpd_uri_t wavetable_live_uri = {
        .uri = "/api/wavetable", .method = HTTP_POST, .handler = &wavetable_live_handler, .user_ctx = nullptr,
        .is_websocket = false, .handle_ws_control_frames = false, .supported_subprotocol = nullptr};
    httpd_register_uri_handler(server, &wavetable_live_uri);

#if defined(CONFIG_APP_ENABLE_BLUETOOTH)
    static const httpd_uri_t ble_get_uri = {
        .uri = "/api/ble", .method = HTTP_GET, .handler = &ble_get_handler, .user_ctx = nullptr,
        .is_websocket = false, .handle_ws_control_frames = false, .supported_subprotocol = nullptr};
    httpd_register_uri_handler(server, &ble_get_uri);

    static const httpd_uri_t ble_post_uri = {
        .uri = "/api/ble", .method = HTTP_POST, .handler = &ble_post_handler, .user_ctx = nullptr,
        .is_websocket = false, .handle_ws_control_frames = false, .supported_subprotocol = nullptr};
    httpd_register_uri_handler(server, &ble_post_uri);
#endif

    ESP_LOGI(WEB_API_TAG, "Synth web API routes registered");
}

} // namespace Feature::Synth

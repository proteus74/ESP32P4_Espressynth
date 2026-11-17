#pragma once

#include <atomic>
#include <cstdio>
#include <dirent.h>
#include <sys/stat.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"

#include "LiveParamMsg.h"
#include "PresetChangedMsg.h"
#include "PresetPaths.h"
#include "SynthEngine.h"
#include "UsbMidiEvent.h"
#include "features/filesystem/littlefs_manager.hpp"
#include "features/io/gpio_manager.hpp"
#include "features/io/led_strip_manager.hpp"

#if defined(CONFIG_ONBOARD_AUDIO_ES8311)
    #include "features/audio/es8311_manager.hpp"
#endif

#if defined(CONFIG_ONBOARD_AUDIO_WM8960)
    #include "features/audio/wm8960_manager.hpp"
#endif

#if defined(CONFIG_ONBOARD_AUDIO_NS4168)
    #include "features/audio/ns4168_manager.hpp"
#endif
#if defined(CONFIG_ONBOARD_AUDIO_PDM_MIC)
    #include "features/audio/pdm_mic_manager.hpp"
#endif

#if defined(CONFIG_APP_ENABLE_USB_DEVICE) && defined(CONFIG_USB_DEV_ENABLE_UAC2)
    #include "features/usb/device/usb_device_audio.hpp"
#endif

#if defined(CONFIG_APP_ENABLE_USB_HOST) && defined(CONFIG_USB_HOST_CLASS_MIDI)
    #include "features/usb/host/usb_midi_host.hpp"
#endif

#if defined(CONFIG_APP_ENABLE_BLUETOOTH) && defined(CONFIG_BLE_ENABLE_MIDI_HOST)
    #include "features/ble/ble_midi_host.hpp"
#endif

// Adapter layer wiring the ported node-graph synth engine (audio/engine/SynthEngine.h) into
// this project's codec managers, USB Audio device, and MIDI transports. The engine itself is
// transport-agnostic (drives esp_codec_dev for the primary/ES8311 sink and a raw I2S TX handle
// for the optional WM8960 sink, and only knows USB-MIDI-shaped {status,data1,data2} events) -
// everything here just adapts this project's existing feature APIs to that shape.
namespace Feature::Synth {

inline SynthEngine s_engine;
inline QueueHandle_t s_presetChangedQueue = nullptr;
inline QueueHandle_t s_liveParamQueue = nullptr;
// Depth-1 "only the latest press matters" queue (xQueueOverwrite, like
// s_presetChangedQueue) that hands a preset-nav direction (-1/+1) off to
// s_presetNavTask - see step_preset()'s comment for why that FS-heavy work
// can't run on the MIDI transport task that detects the button edge.
inline QueueHandle_t s_presetNavQueue = nullptr;
// esp_timer time of the last learned-CC master volume change not yet written
// to DeviceSettings.json (0 = nothing pending). Set on the MIDI transport
// task, persisted by preset_nav_task once the knob has rested for
// kMasterVolumeSaveDelayUs - one flash write per gesture, not per CC message.
inline std::atomic<int64_t> s_masterVolumeDirtyUs{0};
constexpr int64_t kMasterVolumeSaveDelayUs = 2000000;
// Settings files waiting to be written. A flash write disables the cache and stalls both
// cores, audible as a dropout - so, like PresetWriteCache, preset_nav_task writes them only
// once the audio output has been silent for presetcache::kSilentMs. Until then the new values
// live in RAM only (lost on a power cut during uninterrupted playing).
inline std::atomic<bool> s_deviceSettingsDirty{false};
inline std::atomic<bool> s_masterClockDirty{false};
inline std::atomic<bool> s_gpioSettingsDirty{false};
inline void request_device_settings_save() { s_deviceSettingsDirty.store(true); }
inline void request_master_clock_save() { s_masterClockDirty.store(true); }
inline void request_gpio_settings_save() { s_gpioSettingsDirty.store(true); }

/** @brief The live SynthEngine instance - valid once init() has returned successfully. */
inline SynthEngine &engine() {
    return s_engine;
}

/** @brief Queue POST /api/preset (synth_web_api.hpp) writes PresetChangedMsg onto. */
inline QueueHandle_t preset_changed_queue() {
    return s_presetChangedQueue;
}

/** @brief Queue the /ws handler (synth_web_api.hpp) writes raw paramUpdate JSON onto. */
inline QueueHandle_t live_param_queue() {
    return s_liveParamQueue;
}

/** @brief Requests a named preset reload from any non-render task (for example LVGL). */
inline bool request_preset_name(const char *name) {
    if (s_presetChangedQueue == nullptr || !presetpaths::isSafeName(name)) return false;
    PresetChangedMsg msg{};
    presetpaths::pathForName(name, msg.path, sizeof(msg.path));
    presetpaths::writeLastMarker(name);
    s_engine.beginPresetLoad();
    if (xQueueOverwrite(s_presetChangedQueue, &msg) == pdPASS) return true;
    s_engine.cancelPresetLoad();
    return false;
}

/** @brief Requests the unnamed current.json preset. */
inline bool request_current_preset() {
    if (s_presetChangedQueue == nullptr) return false;
    PresetChangedMsg msg{};
    snprintf(msg.path, sizeof(msg.path), "%s", presetpaths::unnamedPath());
    presetpaths::writeLastMarker("");
    s_engine.beginPresetLoad();
    if (xQueueOverwrite(s_presetChangedQueue, &msg) == pdPASS) return true;
    s_engine.cancelPresetLoad();
    return false;
}

/**
 * @brief Native, browser-independent P0 baseline log (plan section 8: "kein
 * Browser und keine JS-Engine als Referenz") - reads the render task's
 * published atomics (SynthEngine::phaseSnapshot()/blockStatsSnapshot()) and
 * prints them via ESP_LOGI from THIS caller's task, not the render task.
 * Deliberately not called from SynthEngine::taskLoop() itself: a real
 * hardware run showed the ESP_LOGI call (UART write, ~2ms) blocking long
 * enough there to single-handedly blow the ~2.9ms block budget and trigger
 * an Audio-Overrun on the very block it logged from. Call this instead from
 * a non-real-time task on roughly a multi-second cadence (see main.cpp).
 */
inline void log_p0_baseline_tick() {
    constexpr const char *kTag = "synth_p0";
    const SynthEngine::PhaseSnapshot phases = engine().phaseSnapshot();
    const SynthEngine::BlockStatsSnapshot stats = engine().blockStatsSnapshot();
    ESP_LOGI(kTag,
             "P0 baseline: phases activity=%.2f%% render=%.2f%% output=%.2f%% | osc pitch "
             "const/audioRate=%lu/%lu tune const/audioRate=%lu/%lu amp const/audioRate=%lu/%lu "
             "silentSkip=%lu gainConverged=%lu | fm pitch const/audioRate=%lu/%lu tune "
             "const/audioRate=%lu/%lu amp const/audioRate=%lu/%lu ampConstZero=%lu silentSkip=%lu "
             "gainConverged=%lu modActive=%lu feedbackActive=%lu superSawActive=%lu | instances "
             "total/rendered/idleForced=%lu/%lu/%lu | dx holding/ramping/boundary/shape=%lu/%lu/%lu/%lu",
             static_cast<double>(phases.activityPercent), static_cast<double>(phases.renderPercent),
             static_cast<double>(phases.outputPercent), static_cast<unsigned long>(stats.oscPitchConstant),
             static_cast<unsigned long>(stats.oscPitchAudioRate), static_cast<unsigned long>(stats.oscTuneConstant),
             static_cast<unsigned long>(stats.oscTuneAudioRate), static_cast<unsigned long>(stats.oscAmpConstant),
             static_cast<unsigned long>(stats.oscAmpAudioRate), static_cast<unsigned long>(stats.oscSilentSkipped),
             static_cast<unsigned long>(stats.oscGainConverged), static_cast<unsigned long>(stats.fmPitchConstant),
             static_cast<unsigned long>(stats.fmPitchAudioRate), static_cast<unsigned long>(stats.fmTuneConstant),
             static_cast<unsigned long>(stats.fmTuneAudioRate), static_cast<unsigned long>(stats.fmAmpConstant),
             static_cast<unsigned long>(stats.fmAmpAudioRate), static_cast<unsigned long>(stats.fmAmpConstantZero),
             static_cast<unsigned long>(stats.fmSilentSkipped), static_cast<unsigned long>(stats.fmGainConverged),
             static_cast<unsigned long>(stats.fmModActive), static_cast<unsigned long>(stats.fmFeedbackActive),
             static_cast<unsigned long>(stats.fmSuperSawActive),
             static_cast<unsigned long>(stats.instancesTotal),
             static_cast<unsigned long>(stats.instancesRendered),
             static_cast<unsigned long>(stats.instancesRenderedWhileIdle),
             static_cast<unsigned long>(stats.dxHolding), static_cast<unsigned long>(stats.dxRamping),
             static_cast<unsigned long>(stats.dxBoundary), static_cast<unsigned long>(stats.dxShapeConfigured));

    // Per-widget breakdown (same data the browser's node-CPU display uses,
    // see synth_web_api.hpp's broadcast_status_tick "nodeCpu" - printed here
    // too so the phases line above is actually attributable to a widget
    // without needing the browser open, see plan section 8). dspPercent is
    // that widget's own render() calls; edgePercent is summing its outgoing
    // edges into downstream instances (see Graph::widgetEdgeLoadPercent).
    const uint16_t widgetCount = engine().widgetLoadCount();
    for (uint16_t i = 0; i < widgetCount; ++i) {
        char widgetId[24];
        float dspPercent = 0.0f;
        if (!engine().widgetLoadAt(i, widgetId, sizeof(widgetId), &dspPercent)) continue;
        const float edgePercent = engine().widgetEdgeLoadPercentAt(i);
        ESP_LOGI(kTag, "  widget %-20s dsp=%.2f%% edge=%.2f%%", widgetId, static_cast<double>(dspPercent),
                 static_cast<double>(edgePercent));
    }
}

/**
 * @brief Prints the render task's overrun/stall/discontinuity reports (see
 *        SynthEngine::DiagReport) from the calling task - never from the render task itself.
 *        Poll from app_main's loop; each report is already rate-limited at the source.
 */
inline void log_render_diagnostics_tick()
{
    static uint32_t overrun_seq = 0, stall_seq = 0, discontinuity_seq = 0;
    SynthEngine::OverrunReport o{};
    if (engine().overrunReport.take(o, overrun_seq))
        ESP_LOGW("synth_engine",
                 "Audio-Overrun: %lu us > %lu us Budget (preset=%lu us, midi=%lu Ev/%lu us, liveParams=%lu/%lu us, "
                 "render=%lu us, postRender=%lu us, esConvert=%lu us, wmConvert=%lu us, nsConvert=%lu us)",
                 (unsigned long)o.processUs, (unsigned long)o.budgetUs, (unsigned long)o.presetUs,
                 (unsigned long)o.midiEvents, (unsigned long)o.midiUs, (unsigned long)o.liveParams,
                 (unsigned long)o.liveParamUs, (unsigned long)o.renderUs, (unsigned long)o.postRenderUs,
                 (unsigned long)o.esConvertUs, (unsigned long)o.wmConvertUs, (unsigned long)o.nsConvertUs);
    SynthEngine::StallReport s{};
    if (engine().stallReport.take(s, stall_seq))
        ESP_LOGW("synth_engine",
                 "Codec-Write-Stall: Iteration gesamt %lu us (>2x Budget) trotz process=%lu us - esWrite=%lu us, "
                 "wmWrite=%lu us, nsWrite=%lu us (vermutlich System-weiter Stall waehrend des Wartens auf DMA)",
                 (unsigned long)s.totalUs, (unsigned long)s.processUs, (unsigned long)s.esWriteUs,
                 (unsigned long)s.wmWriteUs, (unsigned long)s.nsWriteUs);
    SynthEngine::DiscontinuityReport d{};
    if (engine().discontinuityReport.take(d, discontinuity_seq))
        ESP_LOGW("synth_engine", "Signal-Discontinuity %s: maxStep=%ld", d.sink, (long)d.maxStep);
}

inline void log_midi_cc_debug_tick()
{
    static MidiCcDebugSnapshot snapshot{};
    static auto *pending = static_cast<MidiCcDebugSnapshot *>(
        heap_caps_calloc(16, sizeof(MidiCcDebugSnapshot), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    static int64_t previous_log_us = 0;
    const int64_t now_us = esp_timer_get_time();
    if (pending == nullptr || now_us - previous_log_us < 200000) return;
    previous_log_us = now_us;
    const uint32_t dropped = engine().takeControlDebugDropped();
    if (dropped != 0)
        ESP_LOGW("control", "%lu console log packets dropped (controls still applied)", static_cast<unsigned long>(dropped));
    // Collapse intermediate knob positions before formatting or writing to USB/UART.
    // Switch/button events and distinct fan-out pages retain their own packets.
    uint8_t count = 0;
    for (uint8_t packet = 0; packet < 16 && engine().readMidiCcDebugSnapshot(snapshot); ++packet)
    {
        uint8_t index = count;
        if (snapshot.isKnob)
            for (uint8_t i = 0; i < count; ++i)
                if (pending[i].isKnob && pending[i].controlIndex == snapshot.controlIndex &&
                    pending[i].fromUi == snapshot.fromUi &&
                    pending[i].continuation == snapshot.continuation &&
                    strcmp(pending[i].sourceId, snapshot.sourceId) == 0 &&
                    strcmp(pending[i].targets[0].targetId, snapshot.targets[0].targetId) == 0 &&
                    strcmp(pending[i].targets[0].paramName, snapshot.targets[0].paramName) == 0)
                {
                    index = i;
                    break;
                }
        pending[index] = snapshot;
        if (index == count) ++count;
    }
    for (uint8_t packet = 0; packet < count; ++packet)
    {
        snapshot = pending[packet];
        if (!snapshot.continuation)
        {
            if (snapshot.fromUi)
                ESP_LOGI("control", "Bildschirm: %s = %.3f V", snapshot.sourceLabel, static_cast<double>(snapshot.sourceValue));
            else
                ESP_LOGI("control", "MIDI: %s = %.3f V | channel=%u cc=%u raw=%u", snapshot.sourceLabel,
                         static_cast<double>(snapshot.sourceValue), static_cast<unsigned>(snapshot.channel + 1),
                         static_cast<unsigned>(snapshot.cc), static_cast<unsigned>(snapshot.rawValue));
            if (snapshot.targetCount == 0) ESP_LOGI("control", "  keine verbundenen Ziele");
        }
        for (uint8_t i = 0; i < snapshot.targetCount; ++i)
        {
            const auto &target = snapshot.targets[i];
            ESP_LOGI("control", "  verbunden mit %s.%s = %.3f%s", target.targetLabel, target.paramName,
                     static_cast<double>(target.value), target.raw ? " raw" : " (Eingangswert)");
        }
    }
}

#if defined(CONFIG_APP_ENABLE_USB_DEVICE) && defined(CONFIG_USB_DEV_ENABLE_UAC2)
namespace {
// Host (USB speaker, i.e. whatever the PC plays) -> onboard-codec mix-in ring, drained by
// tap_mix() on the engine's render task. Simple lock-free ring (one producer: the USB device
// task's speaker callback via usb_speaker_submit(); one consumer: the render task via
// setUsbAudioTap's TapMix) - same approach used upstream for the identical purpose
// (JoLa-EspressynthP4's UsbAudioDevice.cpp RingBuffer/mixHostAudioInto).
constexpr size_t kHostInRingSamples = 4096; // ~46ms at 44.1kHz stereo

struct HostInRing {
    int16_t data[kHostInRingSamples];
    volatile size_t head = 0;
    volatile size_t tail = 0;

    size_t available() const { return head - tail; }
    size_t free() const { return kHostInRingSamples - (head - tail); }

    void push(const int16_t *src, size_t count) {
        count = (count < free()) ? count : free();
        for (size_t i = 0; i < count; ++i) {
            data[(head + i) % kHostInRingSamples] = src[i];
        }
        head += count;
    }

    size_t pop(int16_t *dst, size_t count) {
        count = (count < available()) ? count : available();
        for (size_t i = 0; i < count; ++i) {
            dst[i] = data[(tail + i) % kHostInRingSamples];
        }
        tail += count;
        return count;
    }
};

inline HostInRing s_hostInRing;
} // namespace

/**
 * @brief Feeds a block of host (USB speaker, host->device) audio into the engine's mix-in
 *        ring. Call this - instead of writing the samples straight to a codec - from the UAC2
 *        speaker callback whenever CONFIG_APP_ENABLE_SYNTH is on: the engine's render task owns
 *        the codec write path once it's running, so a second writer would tear the output.
 */
inline void usb_speaker_submit(const int16_t *samples, size_t frame_count) {
    s_hostInRing.push(samples, frame_count * 2);
}

namespace {
// TapSubmit: engine -> USB microphone (device->host). write_mic() returns the number of
// frames actually queued (size_t); TapSubmit is void-returning, hence this thin wrapper.
void tap_submit(const int16_t *interleaved, size_t samples) {
    Feature::USBDevice::Audio::write_mic(interleaved, samples / 2);
}

// TapMix: mixes whatever host audio usb_speaker_submit() queued into the block being
// rendered, additively (so it layers with the synth voices rather than replacing them).
void tap_mix(float *blockL, float *blockR, size_t frames) {
    const size_t need = frames * 2;
    const size_t avail = s_hostInRing.available();
    const size_t take = (avail < need) ? avail : need;

    int16_t tmp[256 * 2];
    size_t taken = 0;
    while (taken < take) {
        const size_t chunk = (take - taken) < (sizeof(tmp) / sizeof(tmp[0])) ? (take - taken)
                                                                              : (sizeof(tmp) / sizeof(tmp[0]));
        const size_t got = s_hostInRing.pop(tmp, chunk);
        if (got == 0) break;
        for (size_t i = 0; i < got / 2; ++i) {
            blockL[i + taken / 2] += static_cast<float>(tmp[i * 2]) * (1.0f / 32768.0f);
            blockR[i + taken / 2] += static_cast<float>(tmp[i * 2 + 1]) * (1.0f / 32768.0f);
        }
        taken += got;
    }
}
} // namespace
#endif // CONFIG_APP_ENABLE_USB_DEVICE && CONFIG_USB_DEV_ENABLE_UAC2

#if defined(CONFIG_ONBOARD_AUDIO_WM8960)
namespace {
// WM8960-In widget -> codec input stages (see nodes/Wm8960InNode.h).
void apply_wm8960_input_settings(float pgaDb, int boostDb, float adcDb, bool micBias) {
    Feature::Audio::WM8960::request_input_settings(pgaDb, static_cast<float>(boostDb), adcDb, micBias);
}
} // namespace
#endif

#if defined(CONFIG_APP_MIDI_CC_FEEDBACK_OUT)
namespace {
// MidiCcOut sink (see SynthEngine::setMidiCcOutCallback): mirrors a MIDI-learned Knob's value
// out to every connected MIDI transport. Best-effort - a controller that doesn't support or
// want CC feedback just ignores the message, same as any other MIDI receiver; a transport that
// isn't connected right now silently drops it (Feature::MIDI::send()/BLE MIDI::send()'s own
// not-connected checks).
bool try_send_midi_cc_out(uint8_t channel, uint8_t cc, uint8_t value7bit) {
    const uint8_t message[3] = {
        static_cast<uint8_t>(0xB0 | (channel & 0x0F)),
        static_cast<uint8_t>(cc & 0x7F),
        static_cast<uint8_t>(value7bit & 0x7F),
    };
    bool connected = false, success = true;
#if defined(CONFIG_APP_ENABLE_USB_HOST) && defined(CONFIG_USB_HOST_CLASS_MIDI)
    if (Feature::MIDI::is_connected()) {
        connected = true;
        success = Feature::MIDI::send(message, sizeof(message)) == ESP_OK && success;
    }
#endif
#if defined(CONFIG_APP_ENABLE_BLUETOOTH) && defined(CONFIG_BLE_ENABLE_MIDI_HOST)
    for (const auto &conn : Feature::BLE::midi_connections) {
        if (conn.conn_handle != 0xffff && conn.chr_handle != 0) {
            connected = true;
            success = Feature::BLE::MIDI::send(conn.conn_handle, message, sizeof(message)) == ESP_OK && success;
        }
    }
#endif
    return connected && success;
}

void send_midi_cc_out(uint8_t channel, uint8_t cc, uint8_t value7bit) {
    try_send_midi_cc_out(channel, cc, value7bit);
}

uint32_t midi_connection_sequence() {
    uint32_t generation = 0;
#if defined(CONFIG_APP_ENABLE_USB_HOST) && defined(CONFIG_USB_HOST_CLASS_MIDI)
    generation += Feature::MIDI::connection_sequence();
#endif
#if defined(CONFIG_APP_ENABLE_BLUETOOTH) && defined(CONFIG_BLE_ENABLE_MIDI_HOST)
    generation += Feature::BLE::MIDI::connection_sequence();
#endif
    return generation;
}
} // namespace
#endif // CONFIG_APP_MIDI_CC_FEEDBACK_OUT

/**
 * @brief Converts a {status, data1, data2} MIDI message - the shape shared by every transport
 *        in this project (Feature::MIDI::Event, Feature::USBDevice::MIDI::Event,
 *        Feature::BLE::MIDI::Event all carry these three fields) - into the engine's
 *        UsbMidiEvent wire format. codeIndex is simply the status byte's high nibble, which
 *        matches the USB-MIDI Event Packet Code Index Number for every channel-voice message
 *        the engine handles (note on/off 0x8/0x9, poly aftertouch 0xA, CC 0xB, program change
 *        0xC, channel aftertouch 0xD, pitch bend 0xE - see UsbMidiEvent.h).
 */
inline UsbMidiEvent from_transport(uint8_t status, uint8_t data1, uint8_t data2) {
    UsbMidiEvent event{};
    event.cable = 0;
    event.codeIndex = static_cast<uint8_t>(status >> 4);
    event.status = status;
    event.data1 = data1;
    event.data2 = data2;
    return event;
}

/**
 * @brief Steps the active preset forward/backward through the same
 *        directory listing GET /api/presets (synth_web_api.hpp) exposes,
 *        with the unnamed current.json slot as index 0 - same ordering
 *        convention the browser's preset dropdown and the front-panel
 *        display (display_lvgl.hpp's select_preset()) already use. Does
 *        real filesystem I/O (opendir/readdir), so - like
 *        request_preset_name()/request_current_preset() below - this must
 *        only ever be called from a non-render task. In fact it needs more
 *        than "non-render": a real device crashed here with a stack-
 *        protection fault in usb_midi_task, whose 2048-byte stack
 *        (usb_midi_host.hpp) turned out too small for opendir/readdir plus
 *        the rest of the call chain even with every local buffer made
 *        `static` - see s_presetNavTask below, the only caller now, which
 *        exists solely to give this its own adequately-sized stack.
 *
 *        The name buffer is function-local `static` rather than on the
 *        stack (same call-site rule as display_lvgl.hpp's own
 *        s_preset_names). A human pressing a Learn-bound button from two
 *        transports in the same instant is the only way two calls could
 *        race on it, and the worst case is stepping to a stale index once -
 *        never a crash.
 */
inline bool step_preset(int direction) {
    constexpr size_t kCapacity = 32;
    static char names[kCapacity][presetpaths::kMaxNameLen];
    size_t count = 1; // index 0 = unnamed "Current" slot
    names[0][0] = '\0';

    DIR *dir = opendir(presetpaths::kDir);
    if (dir != nullptr) {
        struct dirent *entry;
        while ((entry = readdir(dir)) != nullptr && count < kCapacity) {
            const char *fname = entry->d_name;
            const size_t length = strlen(fname);
            if (fname[0] == '.' || length < 6 || strcmp(fname, "current.json") == 0 ||
                strcmp(fname + length - 5, ".json") != 0)
                continue;
            char preset_name[presetpaths::kMaxNameLen] = {};
            snprintf(preset_name, sizeof(preset_name), "%.*s", static_cast<int>(length - 5), fname);
            if (!presetpaths::isSafeName(preset_name)) continue;
            snprintf(names[count], sizeof(names[0]), "%s", preset_name);
            ++count;
        }
        closedir(dir);
    }

    static char active_path[300];
    char active_name[presetpaths::kMaxNameLen] = {};
    presetpaths::resolveActivePath(active_path, sizeof(active_path), active_name, sizeof(active_name));
    size_t index = 0;
    for (size_t i = 1; i < count; ++i) {
        if (strcmp(active_name, names[i]) == 0) {
            index = i;
            break;
        }
    }

    index = direction < 0 ? (index == 0 ? count - 1 : index - 1) : (index + 1) % count;
    return index == 0 ? request_current_preset() : request_preset_name(names[index]);
}

/**
 * @brief Dedicated task for step_preset()'s filesystem work (own 4096-byte
 *        stack - see step_preset()'s comment on why usb_midi_task's 2048
 *        bytes crashed running this inline). Blocks on s_presetNavQueue and
 *        steps the preset on whatever direction (-1/+1) it wakes up with.
 */
void preset_nav_task(void *) {
    int direction = 0;
    while (true) {
        // Wakes at least every 500 ms to persist a settled master volume (see s_masterVolumeDirtyUs).
        if (xQueueReceive(s_presetNavQueue, &direction, pdMS_TO_TICKS(500)) == pdTRUE) {
            step_preset(direction);
        }
        liveparam::freeRetired(); // JSON trees the render task has applied (LiveParamMsg.h)
        // Knob/Button changes to save with the preset (ParamJournal.hpp)
        paramjournal::tick(synth::deviceSettings().paramAutosave);
        int64_t dirtyUs = s_masterVolumeDirtyUs.load();
        // compare_exchange: a CC arriving during the check keeps the save pending for its own 2 s.
        if (dirtyUs != 0 && esp_timer_get_time() - dirtyUs >= kMasterVolumeSaveDelayUs &&
            s_masterVolumeDirtyUs.compare_exchange_strong(dirtyUs, 0)) {
            s_deviceSettingsDirty.store(true);
        }
        // Flash writes only into an output pause, see s_deviceSettingsDirty.
        if (s_engine.outputSilentFor(presetcache::kSilentMs)) {
            if (s_deviceSettingsDirty.exchange(false)) synth::saveDeviceSettingsToFs();
            if (s_masterClockDirty.exchange(false)) synth::saveMasterClockToFs();
            if (s_gpioSettingsDirty.exchange(false)) Feature::IO::saveGpioSettingsToFs();
        }
    }
}

/**
 * @brief Preset-navigation MIDI learn (DeviceSettings.h): reacts only on
 *        the learned CC's button-down edge (crossing up through 64), not on
 *        its live value - a held pad/button re-sends the same "on" value
 *        repeatedly and must not re-trigger a preset step on every message.
 *        Only queues a direction (cheap, tiny stack) for s_presetNavTask to
 *        actually act on - see step_preset()'s comment for why the real
 *        filesystem work can't run on the caller's task. Every current
 *        caller (dispatch_midi(), below) runs on the transport's own task
 *        (USB-MIDI host, BLE host, or the httpd worker for the browser's
 *        "Test MIDI" button), never the render task.
 */
inline void handleDeviceSettingsMidi(uint8_t status, uint8_t data1, uint8_t data2) {
    if ((status >> 4) != 0xB) return; // Control Change only
    auto &ds = synth::deviceSettings();
    const uint8_t channel = status & 0x0F;
    const bool above = data2 >= 64;
    const int64_t nowUs = esp_timer_get_time();

    // Many buttons send only the "on" value (127) and never a release (0), which would keep
    // the held flag set forever. So a binding also re-arms once its "on" messages pause for
    // kPresetNavRearmUs - a held pad's rapid repeats keep refreshing lastOnUs and still fire once.
    constexpr int64_t kPresetNavRearmUs = 100000;
    auto step = [&](bool &held, int64_t &lastOnUs, int direction) {
        if (above && (!held || nowUs - lastOnUs > kPresetNavRearmUs) && s_presetNavQueue != nullptr) {
            xQueueOverwrite(s_presetNavQueue, &direction);
        }
        held = above;
        if (above) lastOnUs = nowUs;
    };
    if (ds.prevCc >= 0 && static_cast<uint8_t>(ds.prevCc) == data1 &&
        static_cast<uint8_t>(ds.prevChannel) == channel) {
        step(ds.prevHeld, ds.prevLastOnUs, -1);
    }
    if (ds.nextCc >= 0 && static_cast<uint8_t>(ds.nextCc) == data1 &&
        static_cast<uint8_t>(ds.nextChannel) == channel) {
        step(ds.nextHeld, ds.nextLastOnUs, 1);
    }
    // Master volume follows the CC's live value (knob/fader), unlike the edge-only preset
    // step above. A plain float store - the render task only reads it once per block.
    if (synth::hasLearnedMasterVolume() && static_cast<uint8_t>(ds.volumeCc) == data1 &&
        static_cast<uint8_t>(ds.volumeChannel) == channel) {
        ds.volume = synth::clampDeviceVolume(static_cast<float>(data2) / 127.0f);
        s_masterVolumeDirtyUs.store(nowUs);
    }
}

/** @brief Forwards a MIDI message to the running engine. Non-blocking, safe from any task. */
inline void dispatch_midi(uint8_t status, uint8_t data1, uint8_t data2) {
    s_engine.onMidiEvent(from_transport(status, data1, data2));
    handleDeviceSettingsMidi(status, data1, data2);
}

namespace {
constexpr const char *TAG = "FeatureSynth";

// The "littlefs" partition ships blank (see partitions.csv - unlike "www", it is never
// re-imaged on flash) - create the directories the engine/web API expect on first boot.
// mkdir failure (already exists, every boot after the first) is silently ignored.
void ensure_storage_directories() {
    char dir[64];
    snprintf(dir, sizeof(dir), "%s/presets", Feature::LittleFS::MOUNT_POINT);
    mkdir(dir, 0775);
    snprintf(dir, sizeof(dir), "%s/samples", Feature::LittleFS::MOUNT_POINT);
    mkdir(dir, 0775);
    // FM8 patch/bank data (filled by the user, e.g. DX7 .syx banks).
    snprintf(dir, sizeof(dir), "%s/FM8", Feature::LittleFS::MOUNT_POINT);
    mkdir(dir, 0775);
}
} // namespace

/**
 * @brief True DMA-ring underrun count across both onboard codecs (ES8311 always-on primary,
 *        WM8960 optional secondary) since boot: an `on_send_q_ovf` event from the I2S driver
 *        (see the comment in es8311_manager.hpp/wm8960_manager.hpp), meaning the ring genuinely
 *        ran out of real audio and auto-cleared to silence - an actual audible click/dropout.
 *        Deliberately separate from SynthEngine::renderOverrunCount(): that one fires on a
 *        render-*budget* miss, which the DMA ring's ~AUDIO_I2S_DMA_DESC_NUM*
 *        AUDIO_I2S_DMA_FRAME_NUM frames of headroom usually absorb without this ever firing -
 *        see the "Audio-Overrun" log in SynthEngine.h for that one instead.
 */
inline uint32_t dma_underrun_count() {
    uint32_t total = 0;
#if defined(CONFIG_ONBOARD_AUDIO_ES8311)
    total += Feature::Audio::ES8311::underrunCount();
#endif
#if defined(CONFIG_ONBOARD_AUDIO_WM8960)
    total += Feature::Audio::WM8960::underrunCount();
#endif
#if defined(CONFIG_ONBOARD_AUDIO_NS4168)
    total += Feature::Audio::NS4168::underrunCount();
#endif
    return total;
}

/**
 * @brief Zeroes the true DMA-underrun counters on both onboard codecs - see
 *        dma_underrun_tick() below, the only caller (a fresh preset load or Panic deserves a
 *        clean baseline; on-demand callers are welcome too).
 */
inline void reset_dma_underrun_count() {
#if defined(CONFIG_ONBOARD_AUDIO_ES8311)
    Feature::Audio::ES8311::resetUnderrunCount();
#endif
#if defined(CONFIG_ONBOARD_AUDIO_WM8960)
    Feature::Audio::WM8960::resetUnderrunCount();
#endif
#if defined(CONFIG_ONBOARD_AUDIO_NS4168)
    Feature::Audio::NS4168::resetUnderrunCount();
#endif
}

/**
 * @brief Call roughly once a second from the main loop (see main.cpp), independent of whether
 *        the web server/UI is running. Two jobs, both cheap no-ops when nothing changed:
 *        - Zeroes the counters the moment engine().underrunResetSequence() changes, i.e. right
 *          after a real preset reload or Panic (see that getter's comment in SynthEngine.h) -
 *          so "underruns" in the UI reflects behavior since the current preset/patch, not a
 *          number left over from whatever was loaded before.
 *        - Logs a warning the moment dma_underrun_count() changes since the last call - i.e.
 *          exactly when the DMA ring actually ran dry and audibly clicked.
 */
inline void dma_underrun_tick() {
    static uint32_t s_last_reset_sequence = 0;
    static uint32_t s_last_logged_count = 0;
    const uint32_t reset_sequence = engine().underrunResetSequence();
    if (reset_sequence != s_last_reset_sequence) {
        s_last_reset_sequence = reset_sequence;
        reset_dma_underrun_count();
        s_last_logged_count = 0;
    }

    const uint32_t count = dma_underrun_count();
    if (count == s_last_logged_count) return;
    const uint32_t new_events = count - s_last_logged_count;
    s_last_logged_count = count;

    int64_t last_us = 0;
#if defined(CONFIG_ONBOARD_AUDIO_ES8311)
    if (Feature::Audio::ES8311::lastUnderrunUs() > last_us) last_us = Feature::Audio::ES8311::lastUnderrunUs();
#endif
#if defined(CONFIG_ONBOARD_AUDIO_WM8960)
    if (Feature::Audio::WM8960::lastUnderrunUs() > last_us) last_us = Feature::Audio::WM8960::lastUnderrunUs();
#endif
#if defined(CONFIG_ONBOARD_AUDIO_NS4168)
    if (Feature::Audio::NS4168::lastUnderrunUs() > last_us) last_us = Feature::Audio::NS4168::lastUnderrunUs();
#endif
    ESP_LOGW(TAG, "Audio-Underrun: DMA ring ran dry (+%lu, %lu total since last reset, last at t=%lld us)",
             static_cast<unsigned long>(new_events), static_cast<unsigned long>(count),
             static_cast<long long>(last_us));
}

/**
 * @brief Brings up the synth engine: creates the preset/sample/soundfont directories (first
 *        boot only), wires whichever codec(s)/USB-audio tap are enabled, and starts the
 *        engine's render + preset-loader tasks. Call once, after Feature::LittleFS::init() and
 *        the enabled codec manager(s)' init() have both already succeeded.
 */
inline esp_err_t init() {
#if !defined(CONFIG_ONBOARD_AUDIO_ES8311) && !defined(CONFIG_ONBOARD_AUDIO_WM8960) && \
    !defined(CONFIG_ONBOARD_AUDIO_NS4168)
    ESP_LOGE(TAG, "Synth engine requires an onboard audio output (ES8311, WM8960 or NS4168)");
    return ESP_ERR_NOT_SUPPORTED;
#else
    // Every hardware sink is optional; the engine needs at least one, since its blocking DMA
    // writes pace the render loop.
    esp_codec_dev_handle_t codec = nullptr;
    i2s_chan_handle_t secondaryI2sTxHandle = nullptr;
    i2s_chan_handle_t ns4168I2sTxHandle = nullptr;
    i2s_chan_handle_t esI2sTxHandle = nullptr;
#if defined(CONFIG_ONBOARD_AUDIO_ES8311)
    codec = Feature::Audio::ES8311::get_handle();
    esI2sTxHandle = Feature::Audio::ES8311::get_tx_handle();
#endif
#if defined(CONFIG_ONBOARD_AUDIO_WM8960)
    secondaryI2sTxHandle = Feature::Audio::WM8960::get_tx_handle();
#endif
#if defined(CONFIG_ONBOARD_AUDIO_NS4168)
    ns4168I2sTxHandle = Feature::Audio::NS4168::get_tx_handle();
#endif
    if (codec == nullptr && secondaryI2sTxHandle == nullptr && ns4168I2sTxHandle == nullptr) {
        ESP_LOGE(TAG, "No audio output initialized - cannot start synth engine");
        return ESP_ERR_INVALID_STATE;
    }

    ensure_storage_directories();

    s_presetChangedQueue = xQueueCreate(1, sizeof(PresetChangedMsg));
    // Each entry is one pointer to a parsed JSON tree (LiveParamMsg.h), so both queues are tiny.
    s_liveParamQueue = xQueueCreate(liveparam::kQueueDepth, sizeof(LiveParamMsg));
    liveparam::gRetiredQueue = xQueueCreate(liveparam::kQueueDepth * 2, sizeof(cJSON *));

    char initialPresetPath[300];
    char initialPresetName[presetpaths::kMaxNameLen];
    presetpaths::resolveActivePath(initialPresetPath, sizeof(initialPresetPath), initialPresetName,
                                   sizeof(initialPresetName));
#if defined(CONFIG_APP_ENABLE_USB_DEVICE) && defined(CONFIG_USB_DEV_ENABLE_UAC2)
    s_engine.setUsbAudioTap(&tap_submit, &tap_mix);
#endif

#if defined(CONFIG_ONBOARD_AUDIO_PDM_MIC)
    if (Feature::Audio::PdmMic::available()) {
        s_engine.setMicInput(&Feature::Audio::PdmMic::read_block);
    }
#endif

#if defined(CONFIG_ONBOARD_AUDIO_WM8960)
    if (Feature::Audio::WM8960::input_available()) {
        s_engine.setWm8960Input(&Feature::Audio::WM8960::read_block_stereo);
        nodes::wm8960in::applyHook = &apply_wm8960_input_settings;
    }
#endif

#if defined(CONFIG_APP_MIDI_CC_FEEDBACK_OUT)
    s_engine.setMidiCcOutCallback(&send_midi_cc_out);
    s_engine.setMidiFeedbackCallbacks(&try_send_midi_cc_out, &midi_connection_sequence);
#endif

    // Device-wide, independent of any preset (see MasterClock.h) - load before
    // the render task starts so the first Clock-node render already sees it.
    synth::loadMasterClockFromFs();
    synth::loadDeviceSettingsFromFs();
    Feature::IO::loadGpioSettingsFromFs();
    Feature::IO::apply_gpio_settings(); // gpio_config() every configured pin before any preset's DigitalIn/DigitalOut render()s
    Feature::LedStrip::init(); // after the GPIO table, so it can refuse a pin already claimed there

    // Core 0, low priority - same placement as SynthEngine's "preset_loader" task just below
    // (SynthEngine.h): pure background filesystem work, must never land on core 1, which
    // SynthEngine::begin()'s render task claims exclusively.
    s_presetNavQueue = xQueueCreate(1, sizeof(int));
    xTaskCreatePinnedToCore(&preset_nav_task, "preset_nav", 4096, nullptr, 3, nullptr, 0);

    // Deferred preset writes (PresetWriteCache.hpp): flash writes wait for an output pause.
    presetcache::init([](uint32_t ms) { return s_engine.outputSilentFor(ms); });
    paramjournal::init(); // before s_engine.begin(): the first preset build already attaches to it

    s_engine.begin(codec, secondaryI2sTxHandle, ns4168I2sTxHandle, s_presetChangedQueue, s_liveParamQueue, initialPresetPath,
                   esI2sTxHandle);

    ESP_LOGI(TAG, "Synth engine started (preset: %s)",
             initialPresetName[0] != '\0' ? initialPresetName : "current.json");
    return ESP_OK;
#endif
}

} // namespace Feature::Synth

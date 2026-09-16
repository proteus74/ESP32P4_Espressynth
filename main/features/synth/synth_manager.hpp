#pragma once

#include <cstdio>
#include <sys/stat.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"

#include "LiveParamMsg.h"
#include "PresetChangedMsg.h"
#include "PresetPaths.h"
#include "SynthEngine.h"
#include "UsbMidiEvent.h"
#include "features/filesystem/littlefs_manager.hpp"

#if defined(CONFIG_ONBOARD_AUDIO_ES8311)
    #include "features/audio/es8311_manager.hpp"
#endif

#if defined(CONFIG_ONBOARD_AUDIO_WM8960)
    #include "features/audio/wm8960_manager.hpp"
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
    return xQueueOverwrite(s_presetChangedQueue, &msg) == pdPASS;
}

/** @brief Requests the unnamed current.json preset. */
inline bool request_current_preset() {
    if (s_presetChangedQueue == nullptr) return false;
    PresetChangedMsg msg{};
    snprintf(msg.path, sizeof(msg.path), "%s", presetpaths::unnamedPath());
    presetpaths::writeLastMarker("");
    return xQueueOverwrite(s_presetChangedQueue, &msg) == pdPASS;
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

#if (defined(CONFIG_APP_ENABLE_USB_HOST) && defined(CONFIG_USB_HOST_CLASS_MIDI)) || \
    (defined(CONFIG_APP_ENABLE_BLUETOOTH) && defined(CONFIG_BLE_ENABLE_MIDI_HOST))
namespace {
// MidiCcOut sink (see SynthEngine::setMidiCcOutCallback): mirrors a MIDI-learned Knob's value
// out to every connected MIDI transport. Best-effort - a controller that doesn't support or
// want CC feedback just ignores the message, same as any other MIDI receiver; a transport that
// isn't connected right now silently drops it (Feature::MIDI::send()/BLE MIDI::send()'s own
// not-connected checks).
void send_midi_cc_out(uint8_t channel, uint8_t cc, uint8_t value7bit) {
    const uint8_t message[3] = {
        static_cast<uint8_t>(0xB0 | (channel & 0x0F)),
        static_cast<uint8_t>(cc & 0x7F),
        static_cast<uint8_t>(value7bit & 0x7F),
    };
#if defined(CONFIG_APP_ENABLE_USB_HOST) && defined(CONFIG_USB_HOST_CLASS_MIDI)
    Feature::MIDI::send(message, sizeof(message));
#endif
#if defined(CONFIG_APP_ENABLE_BLUETOOTH) && defined(CONFIG_BLE_ENABLE_MIDI_HOST)
    for (const auto &conn : Feature::BLE::midi_connections) {
        if (conn.conn_handle != 0xffff && conn.chr_handle != 0) {
            Feature::BLE::MIDI::send(conn.conn_handle, message, sizeof(message));
        }
    }
#endif
}
} // namespace
#endif // USB-host MIDI or BLE MIDI host enabled

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

/** @brief Forwards a MIDI message to the running engine. Non-blocking, safe from any task. */
inline void dispatch_midi(uint8_t status, uint8_t data1, uint8_t data2) {
    s_engine.onMidiEvent(from_transport(status, data1, data2));
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
    snprintf(dir, sizeof(dir), "%s/soundfonts", Feature::LittleFS::MOUNT_POINT);
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
#if !defined(CONFIG_ONBOARD_AUDIO_ES8311)
    ESP_LOGE(TAG, "Synth engine requires ONBOARD_AUDIO_ES8311 (primary output) to be enabled");
    return ESP_ERR_NOT_SUPPORTED;
#else
    esp_codec_dev_handle_t codec = Feature::Audio::ES8311::get_handle();
    if (codec == nullptr) {
        ESP_LOGE(TAG, "ES8311 codec not initialized - cannot start synth engine");
        return ESP_ERR_INVALID_STATE;
    }

    ensure_storage_directories();

    s_presetChangedQueue = xQueueCreate(1, sizeof(PresetChangedMsg));
    // Event-rate only (WebSocket live-parameter updates, not sample-rate) - PSRAM-backed to
    // keep this out of the internal-RAM budget (mirrors upstream JoLa-EspressynthP4).
    s_liveParamQueue = xQueueCreateWithCaps(32, sizeof(LiveParamMsg), MALLOC_CAP_SPIRAM);

    char initialPresetPath[300];
    char initialPresetName[presetpaths::kMaxNameLen];
    presetpaths::resolveActivePath(initialPresetPath, sizeof(initialPresetPath), initialPresetName,
                                   sizeof(initialPresetName));
    i2s_chan_handle_t secondaryI2sTxHandle = nullptr;
#if defined(CONFIG_ONBOARD_AUDIO_WM8960)
    secondaryI2sTxHandle = Feature::Audio::WM8960::get_tx_handle();
#endif

#if defined(CONFIG_APP_ENABLE_USB_DEVICE) && defined(CONFIG_USB_DEV_ENABLE_UAC2)
    s_engine.setUsbAudioTap(&tap_submit, &tap_mix);
#endif

#if (defined(CONFIG_APP_ENABLE_USB_HOST) && defined(CONFIG_USB_HOST_CLASS_MIDI)) || \
    (defined(CONFIG_APP_ENABLE_BLUETOOTH) && defined(CONFIG_BLE_ENABLE_MIDI_HOST))
    s_engine.setMidiCcOutCallback(&send_midi_cc_out);
#endif

    // Device-wide, independent of any preset (see MasterClock.h) - load before
    // the render task starts so the first Clock-node render already sees it.
    synth::loadMasterClockFromFs();

    s_engine.begin(codec, secondaryI2sTxHandle, s_presetChangedQueue, s_liveParamQueue, initialPresetPath);

    ESP_LOGI(TAG, "Synth engine started (preset: %s)",
             initialPresetName[0] != '\0' ? initialPresetName : "current.json");
    return ESP_OK;
#endif
}

} // namespace Feature::Synth

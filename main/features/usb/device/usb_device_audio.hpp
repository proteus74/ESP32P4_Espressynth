#pragma once

#include "usb_device_interfaces.hpp"
#include "tusb.h"
#include "class/audio/audio_device.h"
#include "esp_log.h"
#include <atomic>
#include <cstdint>
#include <cstddef>
#include <cstring>

namespace Feature::USBDevice::Audio {

// UAC2 entity IDs - shared between the descriptor builder and the control-request handlers.
constexpr uint8_t ENTITY_CLOCK           = 0x01;
constexpr uint8_t ENTITY_SPK_INPUT_TERM  = 0x02;
constexpr uint8_t ENTITY_SPK_FEATURE     = 0x03;
constexpr uint8_t ENTITY_SPK_OUTPUT_TERM = 0x04;
constexpr uint8_t ENTITY_MIC_INPUT_TERM  = 0x05;
constexpr uint8_t ENTITY_MIC_FEATURE     = 0x06;
constexpr uint8_t ENTITY_MIC_OUTPUT_TERM = 0x07;

// Speaker (host -> device) PCM block: 16-bit signed, interleaved stereo (L,R,L,R,...).
// Valid only for the duration of the callback.
using SpeakerCallback = void (*)(const int16_t* samples, size_t frame_count);

struct Statistics {
    uint32_t speaker_frames_received;
    uint32_t microphone_frames_requested;
    uint32_t microphone_frames_queued;
    uint32_t microphone_frames_dropped;
};

namespace {
constexpr const char* TAG = "USBDevAudio";
constexpr size_t CHANNEL_COUNT = USB_DEV_UAC2_CHANNELS;
constexpr size_t CHANNEL_STATE_COUNT = CHANNEL_COUNT + 1;
constexpr size_t FRAME_BYTES = CHANNEL_COUNT * sizeof(int16_t);
constexpr int16_t VOLUME_MIN = -90 * 256;
constexpr int16_t VOLUME_MAX = 0;
constexpr uint16_t VOLUME_RESOLUTION = 256;

struct FeatureState {
    int8_t mute[CHANNEL_STATE_COUNT] = {};
    int16_t volume[CHANNEL_STATE_COUNT] = {};
};

inline Feature::USBDevice::Audio::SpeakerCallback speaker_callback = nullptr;
inline bool spk_streaming = false;
inline bool mic_streaming = false;
inline int16_t speaker_block[USB_DEV_UAC2_EP_SIZE / sizeof(int16_t)];
inline FeatureState speaker_feature;
inline FeatureState microphone_feature;
inline std::atomic<uint32_t> speaker_frames_received{0};
inline std::atomic<uint32_t> microphone_frames_requested{0};
inline std::atomic<uint32_t> microphone_frames_queued{0};
inline std::atomic<uint32_t> microphone_frames_dropped{0};

inline FeatureState* feature_state(uint8_t entity_id) {
    if (entity_id == Feature::USBDevice::Audio::ENTITY_SPK_FEATURE) return &speaker_feature;
    if (entity_id == Feature::USBDevice::Audio::ENTITY_MIC_FEATURE) return &microphone_feature;
    return nullptr;
}
} // namespace

/**
 * @brief Register a callback invoked from the USB device task whenever a block of
 *        speaker (playback) audio arrives from the host. Pass nullptr to unregister.
 */
inline void set_speaker_callback(SpeakerCallback callback) {
    speaker_callback = callback;
}

/**
 * @brief Queue 16-bit signed interleaved stereo samples for the microphone (device -> host)
 *        stream. Returns the number of frames actually queued.
 */
inline size_t write_mic(const int16_t* samples, size_t frame_count) {
    if (!mic_streaming || samples == nullptr || frame_count == 0) return 0;
    microphone_frames_requested.fetch_add(frame_count, std::memory_order_relaxed);
    const uint16_t bytes = tud_audio_write(samples, static_cast<uint16_t>(frame_count * USB_DEV_UAC2_CHANNELS *
                                                                           sizeof(int16_t)));
    const size_t queued_frames = bytes / FRAME_BYTES;
    microphone_frames_queued.fetch_add(queued_frames, std::memory_order_relaxed);
    microphone_frames_dropped.fetch_add(frame_count - queued_frames, std::memory_order_relaxed);
    return queued_frames;
}

/**
 * @brief True when the host has activated the speaker streaming interface.
 */
inline bool is_speaker_streaming() {
    return spk_streaming;
}

/**
 * @brief True when the host has activated the microphone streaming interface.
 */
inline bool is_microphone_streaming() {
    return mic_streaming;
}

/**
 * @brief True when either audio direction is streaming.
 */
inline bool is_streaming() {
    return spk_streaming || mic_streaming;
}

inline Statistics get_statistics() {
    return {
        speaker_frames_received.load(std::memory_order_relaxed),
        microphone_frames_requested.load(std::memory_order_relaxed),
        microphone_frames_queued.load(std::memory_order_relaxed),
        microphone_frames_dropped.load(std::memory_order_relaxed),
    };
}

inline void reset_statistics() {
    speaker_frames_received.store(0, std::memory_order_relaxed);
    microphone_frames_requested.store(0, std::memory_order_relaxed);
    microphone_frames_queued.store(0, std::memory_order_relaxed);
    microphone_frames_dropped.store(0, std::memory_order_relaxed);
}

} // namespace Feature::USBDevice::Audio

// Polled from Feature::USBDevice::class_task(): drains speaker (host -> device) PCM as it arrives.
extern "C" __attribute__((used)) void usb_device_audio_poll() {
    using namespace Feature::USBDevice::Audio;
    while (tud_audio_available() >= FRAME_BYTES) {
        const uint16_t available = tud_audio_available();
        const uint16_t frame_aligned = available - (available % FRAME_BYTES);
        const uint16_t to_read = frame_aligned < sizeof(speaker_block) ? frame_aligned : sizeof(speaker_block);
        const uint16_t read = tud_audio_read(speaker_block, to_read);
        const size_t frame_count = read / FRAME_BYTES;
        speaker_frames_received.fetch_add(frame_count, std::memory_order_relaxed);
        if (speaker_callback != nullptr) {
            speaker_callback(speaker_block, frame_count);
        }
    }
}

extern "C" __attribute__((used)) bool tud_audio_set_itf_cb(uint8_t rhport, tusb_control_request_t const* p_request) {
    (void)rhport;
    using namespace Feature::USBDevice::Audio;
    const uint8_t itf = static_cast<uint8_t>(p_request->wIndex);
    const uint8_t alt = static_cast<uint8_t>(p_request->wValue);
    ESP_LOGI(TAG, "tud_audio_set_itf_cb: itf=%u alt=%u (SPK=%u MIC=%u)", itf, alt,
             UsbDeviceItf::AUDIO_STREAM_SPK, UsbDeviceItf::AUDIO_STREAM_MIC);
    if (itf == UsbDeviceItf::AUDIO_STREAM_SPK) {
        spk_streaming = (alt == 1);
        ESP_LOGI(TAG, "Speaker streaming: %s", spk_streaming ? "active" : "idle");
    } else if (itf == UsbDeviceItf::AUDIO_STREAM_MIC) {
        mic_streaming = (alt == 1);
        ESP_LOGI(TAG, "Microphone streaming: %s", mic_streaming ? "active" : "idle");
    }
    return true;
}

extern "C" __attribute__((used)) bool tud_audio_set_itf_close_ep_cb(uint8_t rhport, tusb_control_request_t const* p_request) {
    (void)rhport;
    using namespace Feature::USBDevice::Audio;
    const uint8_t itf = static_cast<uint8_t>(p_request->wIndex);
    if (itf == UsbDeviceItf::AUDIO_STREAM_SPK) {
        spk_streaming = false;
    } else if (itf == UsbDeviceItf::AUDIO_STREAM_MIC) {
        mic_streaming = false;
    }
    return true;
}

extern "C" __attribute__((used)) bool tud_audio_get_req_entity_cb(uint8_t rhport, tusb_control_request_t const* p_request) {
    using namespace Feature::USBDevice::Audio;
    audio_control_request_t const* request = reinterpret_cast<audio_control_request_t const*>(p_request);

    if (request->bEntityID == Feature::USBDevice::Audio::ENTITY_CLOCK) {
        if (request->bControlSelector == AUDIO_CS_CTRL_SAM_FREQ) {
            if (request->bRequest == AUDIO_CS_REQ_CUR) {
                audio_control_cur_4_t cur = {static_cast<int32_t>(USB_DEV_UAC2_SAMPLE_RATE)};
                return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &cur, sizeof(cur));
            }
            if (request->bRequest == AUDIO_CS_REQ_RANGE) {
                audio_control_range_4_n_t(1) range = {};
                range.wNumSubRanges = 1;
                range.subrange[0].bMin = static_cast<int32_t>(USB_DEV_UAC2_SAMPLE_RATE);
                range.subrange[0].bMax = static_cast<int32_t>(USB_DEV_UAC2_SAMPLE_RATE);
                range.subrange[0].bRes = 0;
                return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &range, sizeof(range));
            }
        } else if (request->bControlSelector == AUDIO_CS_CTRL_CLK_VALID) {
            if (request->bRequest == AUDIO_CS_REQ_CUR) {
                audio_control_cur_1_t cur = {1};
                return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &cur, sizeof(cur));
            }
        }
    }

    FeatureState* state = feature_state(request->bEntityID);
    if (state == nullptr || request->bChannelNumber >= CHANNEL_STATE_COUNT) return false;

    if (request->bControlSelector == AUDIO_FU_CTRL_MUTE && request->bRequest == AUDIO_CS_REQ_CUR) {
        audio_control_cur_1_t cur = {state->mute[request->bChannelNumber]};
        return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &cur, sizeof(cur));
    }
    if (request->bControlSelector == AUDIO_FU_CTRL_VOLUME) {
        if (request->bRequest == AUDIO_CS_REQ_CUR) {
            audio_control_cur_2_t cur = {state->volume[request->bChannelNumber]};
            return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &cur, sizeof(cur));
        }
        if (request->bRequest == AUDIO_CS_REQ_RANGE) {
            audio_control_range_2_n_t(1) range = {};
            range.wNumSubRanges = 1;
            range.subrange[0] = {VOLUME_MIN, VOLUME_MAX, VOLUME_RESOLUTION};
            return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &range, sizeof(range));
        }
    }

    return false;
}

extern "C" __attribute__((used)) bool tud_audio_set_req_entity_cb(uint8_t rhport, tusb_control_request_t const* p_request, uint8_t* buf) {
    (void)rhport;
    using namespace Feature::USBDevice::Audio;
    audio_control_request_t const* request = reinterpret_cast<audio_control_request_t const*>(p_request);
    FeatureState* state = feature_state(request->bEntityID);
    if (state == nullptr || request->bChannelNumber >= CHANNEL_STATE_COUNT || request->bRequest != AUDIO_CS_REQ_CUR) {
        return false;
    }
    if (request->bControlSelector == AUDIO_FU_CTRL_MUTE && request->wLength == sizeof(audio_control_cur_1_t)) {
        state->mute[request->bChannelNumber] = static_cast<int8_t>(buf[0] != 0);
        return true;
    }
    if (request->bControlSelector == AUDIO_FU_CTRL_VOLUME && request->wLength == sizeof(audio_control_cur_2_t)) {
        int16_t volume;
        std::memcpy(&volume, buf, sizeof(volume));
        if (volume < VOLUME_MIN) volume = VOLUME_MIN;
        if (volume > VOLUME_MAX) volume = VOLUME_MAX;
        state->volume[request->bChannelNumber] = volume;
        return true;
    }
    return false;
}

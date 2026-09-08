#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include "app_core.hpp"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "host/usbh.h"
#include "class/midi/midi_host.h"

namespace Feature::MIDI {

struct Event {
    uint8_t status;
    uint8_t data1;
    uint8_t data2;
    uint8_t channel;    // 0-15, decoded from status & 0x0f
};

using EventCallback = void (*)(const Event& event);

namespace {
constexpr const char* TAG = "USBMIDI";
struct midi_packet_t { uint8_t data[4]; };
inline QueueHandle_t queue = nullptr;
inline Feature::MIDI::EventCallback event_callback = nullptr;
inline uint8_t connected_idx = 0;
inline bool connected = false;

inline void midi_task(void*) {
    midi_packet_t packet{};
    while (xQueueReceive(queue, &packet, portMAX_DELAY) == pdTRUE) {
        if (event_callback != nullptr) {
            const Feature::MIDI::Event event{
                packet.data[1], packet.data[2], packet.data[3],
                static_cast<uint8_t>(packet.data[1] & 0x0f)
            };
            event_callback(event);
        }
    }
}
} // namespace

/**
 * @brief Register a callback invoked from the MIDI task for every received message.
 *        Pass nullptr to unregister. Only one callback is stored at a time.
 */
inline void set_callback(EventCallback callback) {
    event_callback = callback;
}

/**
 * @brief True if a USB MIDI device is currently mounted.
 */
inline bool is_connected() {
    return connected && tuh_midi_mounted(connected_idx);
}

/**
 * @brief Send raw MIDI stream bytes (e.g. {0x90, 0x3C, 0x7F} for Note On) to the
 *        connected device on the given virtual cable. Returns ESP_ERR_INVALID_STATE
 *        if no device is connected.
 */
inline esp_err_t send(const uint8_t* bytes, size_t length, uint8_t cable = 0) {
    if (bytes == nullptr || length == 0) return ESP_ERR_INVALID_ARG;
    if (!is_connected()) return ESP_ERR_INVALID_STATE;
    const uint32_t written = tuh_midi_stream_write(connected_idx, cable, bytes,
                                                     static_cast<uint32_t>(length));
    return written == length ? ESP_OK : ESP_FAIL;
}

} // namespace Feature::MIDI

extern "C" __attribute__((used)) void usb_midi_host_start() {
    using namespace Feature::MIDI;
    queue = xQueueCreate(64, sizeof(midi_packet_t));
    // MIDI is sound data, so this relay task runs on the audio core (see app_core.hpp),
    // separate from the mixed USB Host task (HID + raw MIDI byte parsing) that feeds it.
    xTaskCreatePinnedToCore(midi_task, "usb_midi_task", 3072, nullptr, 5, nullptr, App::AUDIO_CORE);
    ESP_LOGI(TAG, "TinyUSB MIDI host enabled");
}

extern "C" __attribute__((used)) void tuh_midi_mount_cb(uint8_t idx, const tuh_midi_mount_cb_t* data) {
    using namespace Feature::MIDI;
    ESP_LOGI(TAG, "MIDI mounted: address=%u index=%u", data->daddr, idx);
    connected_idx = idx;
    connected = true;
}

extern "C" __attribute__((used)) void tuh_midi_umount_cb(uint8_t idx) {
    using namespace Feature::MIDI;
    ESP_LOGI(TAG, "MIDI removed: index=%u", idx);
    if (connected && idx == connected_idx) connected = false;
}

extern "C" __attribute__((used)) void tuh_midi_rx_cb(uint8_t idx, uint32_t bytes) {
    using namespace Feature::MIDI;
    uint8_t packet[4];
    while (bytes >= 4 && tuh_midi_packet_read(idx, packet)) {
        if (queue != nullptr) {
            midi_packet_t event{};
            std::memcpy(event.data, packet, sizeof(event.data));
            xQueueSend(queue, &event, 0);
        }
        bytes -= 4;
    }
}

extern "C" __attribute__((used)) void tuh_midi_tx_cb(uint8_t idx, uint32_t bytes) {
    (void)idx;
    (void)bytes;
}

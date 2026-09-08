#pragma once

#include "app_core.hpp"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <atomic>

#if defined(CONFIG_APP_ENABLE_USB_HOST) && defined(CONFIG_USB_HOST_CLASS_HID)
    #include "features/usb/host/usb_hid_keyboard.hpp"
    #include "features/usb/host/usb_hid_mouse.hpp"
#endif

// LVGL's mouse cursor and keyboard/group input need feeding from whichever HID
// transport(s) are enabled -- USB, BLE, or both.
#if defined(CONFIG_APP_ENABLE_LVGL) && \
    ((defined(CONFIG_APP_ENABLE_USB_HOST) && defined(CONFIG_USB_HOST_CLASS_HID)) || \
     (defined(CONFIG_APP_ENABLE_BLUETOOTH) && defined(CONFIG_BLE_ENABLE_HID_HOST)))
    #include "features/display/display_lvgl.hpp"
#endif

#if defined(CONFIG_APP_ENABLE_USB_HOST) && defined(CONFIG_USB_HOST_CLASS_MIDI)
    #include "features/usb/host/usb_midi_host.hpp"
#endif

#if defined(CONFIG_APP_ENABLE_BLUETOOTH) && defined(CONFIG_BLE_ENABLE_HID_HOST)
    #include "features/ble/ble_hid_host.hpp"
#endif

#if defined(CONFIG_APP_ENABLE_BLUETOOTH) && defined(CONFIG_BLE_ENABLE_MIDI_HOST)
    #include "features/ble/ble_midi_host.hpp"
#endif

#if defined(CONFIG_APP_ENABLE_USB_DEVICE) && defined(CONFIG_USB_DEV_ENABLE_CDC)
    #include "features/usb/device/usb_device_cdc.hpp"
#endif

#if defined(CONFIG_APP_ENABLE_USB_DEVICE) && defined(CONFIG_USB_DEV_ENABLE_MIDI)
    #include "features/usb/device/usb_device_midi.hpp"
#endif

#if defined(CONFIG_APP_ENABLE_USB_DEVICE) && defined(CONFIG_USB_DEV_ENABLE_UAC2)
    #include "features/usb/device/usb_device_audio.hpp"
#endif

#if defined(CONFIG_ONBOARD_AUDIO_ES8311)
    #include "features/audio/es8311_manager.hpp"
#endif

#if defined(CONFIG_ONBOARD_AUDIO_WM8960)
    #include "features/audio/wm8960_manager.hpp"
#endif

#if defined(CONFIG_APP_ENABLE_SYNTH)
    #include "features/synth/synth_manager.hpp"
#if defined(CONFIG_APP_ENABLE_WEBSERVER)
    #include "features/synth/synth_web_api.hpp"
#endif
#endif

// Demo glue code: wires the optional feature modules (USB/BLE HID, MIDI, USB
// Device CDC/MIDI/Audio, onboard codecs) together so app_main() itself stays
// limited to App::init() and the main loop. Replace the handlers below with
// real application logic.
namespace App {

namespace {

constexpr const char* TAG = "MainApp";

#if defined(CONFIG_APP_ENABLE_LVGL) && \
    ((defined(CONFIG_APP_ENABLE_USB_HOST) && defined(CONFIG_USB_HOST_CLASS_HID)) || \
     (defined(CONFIG_APP_ENABLE_BLUETOOTH) && defined(CONFIG_BLE_ENABLE_HID_HOST)))
// Maps a HID keyboard usage keycode + decoded ascii to an LVGL key code (LV_KEY_* or a
// printable character). Shared by the USB and BLE keyboard handlers below -- both hand
// over the same USB HID usage keycodes, since BLE Boot Keyboard reuses that format.
// Returns 0 for keys LVGL has no use for here.
uint32_t hid_keycode_to_lv_key(uint8_t keycode, char ascii, bool shift) {
    if (ascii != '\0') {
        // Tab already lines up with LV_KEY_NEXT ('\t' == 9 == LV_KEY_NEXT); Shift+Tab is
        // the conventional "previous field" shortcut, so remap it to LV_KEY_PREV.
        if (ascii == '\t' && shift) return LV_KEY_PREV;
        return static_cast<uint32_t>(static_cast<unsigned char>(ascii));
    }
    switch (keycode) {
        case 0x4A: return LV_KEY_HOME;
        case 0x4C: return LV_KEY_DEL;
        case 0x4D: return LV_KEY_END;
        case 0x4F: return LV_KEY_RIGHT;
        case 0x50: return LV_KEY_LEFT;
        case 0x51: return LV_KEY_DOWN;
        case 0x52: return LV_KEY_UP;
        default:   return 0;
    }
}

// Shows the LVGL mouse cursor while at least one USB or BLE mouse is connected.
// Registered as the connection callback for whichever source(s) are enabled below.
void update_mouse_cursor_visibility(bool /*connected*/) {
    bool any_connected = false;
#if defined(CONFIG_APP_ENABLE_USB_HOST) && defined(CONFIG_USB_HOST_CLASS_HID)
    any_connected = any_connected || Feature::Mouse::is_connected();
#endif
#if defined(CONFIG_APP_ENABLE_BLUETOOTH) && defined(CONFIG_BLE_ENABLE_HID_HOST)
    any_connected = any_connected || Feature::BLE::Mouse::is_connected();
#endif
    Feature::LVGL::set_mouse_cursor_visible(any_connected);
}
#endif

#if defined(CONFIG_APP_ENABLE_USB_HOST) && defined(CONFIG_USB_HOST_CLASS_HID)

#if defined(CONFIG_USB_HOST_CLASS_MIDI)
// Demo: computer keyboard as a one-octave MIDI piano (a-k = C4-C5), monophonic.
int8_t midi_note_for_ascii(char ascii) {
    switch (ascii) {
        case 'a': return 60; // C4
        case 's': return 62; // D4
        case 'd': return 64; // E4
        case 'f': return 65; // F4
        case 'g': return 67; // G4
        case 'h': return 69; // A4
        case 'j': return 71; // B4
        case 'k': return 72; // C5
        default:  return -1;
    }
}

int8_t last_note = -1;

void play_midi_note(int8_t note) {
    if (!Feature::MIDI::is_connected()) return;
    if (last_note >= 0) {
        const uint8_t note_off[3] = {0x80, static_cast<uint8_t>(last_note), 0};
        Feature::MIDI::send(note_off, sizeof(note_off));
    }
    const uint8_t note_on[3] = {0x90, static_cast<uint8_t>(note), 100};
    Feature::MIDI::send(note_on, sizeof(note_on));
    last_note = note;
    ESP_LOGI(TAG, "MIDI out: note %d on (keyboard demo)", note);
}
#endif

void on_keyboard_event(const Feature::Keyboard::Event& event) {
    if (event.is_modifier) {
        ESP_LOGI(TAG, "Keyboard modifier: %s %s", Feature::Keyboard::modifier_name(event.keycode),
                 event.pressed ? "pressed" : "released");
        return;
    }
    ESP_LOGI(TAG, "Keyboard key: code=0x%02x char=%c", event.keycode, event.ascii ? event.ascii : '?');

#if defined(CONFIG_USB_HOST_CLASS_MIDI)
    const int8_t note = midi_note_for_ascii(event.ascii);
    if (note >= 0) play_midi_note(note);
#endif
#if defined(CONFIG_APP_ENABLE_LVGL)
    const uint32_t lv_key = hid_keycode_to_lv_key(event.keycode, event.ascii, event.shift);
    if (lv_key != 0) Feature::LVGL::feed_keyboard_event(lv_key, event.pressed);
#endif
}

void on_mouse_event(const Feature::Mouse::Event& event) {
    // ESP_LOGI(TAG, "Mouse: buttons=0x%02x dx=%d dy=%d wheel=%d",
    //          event.buttons, event.dx, event.dy, event.wheel);
#if defined(CONFIG_APP_ENABLE_LVGL)
    Feature::LVGL::feed_mouse_event(event.dx, event.dy, event.buttons);
#endif
}

#endif // CONFIG_USB_HOST_CLASS_HID

#if defined(CONFIG_APP_ENABLE_USB_HOST) && defined(CONFIG_USB_HOST_CLASS_MIDI)
void on_midi_event(const Feature::MIDI::Event& event) {
    // ESP_LOGI(TAG, "MIDI in: status=0x%02x data=%u,%u channel=%u",
    //          event.status, event.data1, event.data2, event.channel + 1);
#if defined(CONFIG_APP_ENABLE_SYNTH)
    Feature::Synth::dispatch_midi(event.status, event.data1, event.data2);
#if defined(CONFIG_APP_ENABLE_WEBSERVER)
    Feature::Synth::broadcast_midi_event(event.status, event.data1, event.data2);
#endif
#endif
}
#endif

#if defined(CONFIG_APP_ENABLE_BLUETOOTH) && defined(CONFIG_BLE_ENABLE_HID_HOST)
void on_ble_keyboard_event(const Feature::BLE::Keyboard::Event& event) {
    if (event.is_modifier) {
        ESP_LOGI(TAG, "BLE keyboard modifier: %s %s", Feature::BLE::Keyboard::modifier_name(event.keycode),
                 event.pressed ? "pressed" : "released");
        return;
    }
    ESP_LOGI(TAG, "BLE keyboard key: conn=%u code=0x%02x char=%c",
             event.conn_handle, event.keycode, event.ascii ? event.ascii : '?');
#if defined(CONFIG_APP_ENABLE_LVGL)
    const uint32_t lv_key = hid_keycode_to_lv_key(event.keycode, event.ascii, event.shift);
    if (lv_key != 0) Feature::LVGL::feed_keyboard_event(lv_key, event.pressed);
#endif
}

void on_ble_mouse_event(const Feature::BLE::Mouse::Event& event) {
    // ESP_LOGI(TAG, "BLE mouse: conn=%u buttons=0x%02x dx=%d dy=%d wheel=%d",
    //          event.conn_handle, event.buttons, event.dx, event.dy, event.wheel);
#if defined(CONFIG_APP_ENABLE_LVGL)
    Feature::LVGL::feed_mouse_event(event.dx, event.dy, event.buttons);
#endif
}

void on_ble_hid_report(const Feature::BLE::HID::Event& event) {
    ESP_LOGI(TAG, "BLE HID report: conn=%u %u bytes", event.conn_handle, event.length);
}
#endif

#if defined(CONFIG_APP_ENABLE_BLUETOOTH) && defined(CONFIG_BLE_ENABLE_MIDI_HOST)
void on_ble_midi_event(const Feature::BLE::MIDI::Event& event) {
    // ESP_LOGI(TAG, "BLE-MIDI in: conn=%u status=0x%02x data=%u,%u channel=%u",
    //          event.conn_handle, event.status, event.data1, event.data2, event.channel + 1);
#if defined(CONFIG_APP_ENABLE_SYNTH)
    Feature::Synth::dispatch_midi(event.status, event.data1, event.data2);
#if defined(CONFIG_APP_ENABLE_WEBSERVER)
    Feature::Synth::broadcast_midi_event(event.status, event.data1, event.data2);
#endif
#endif
}
#endif

#if defined(CONFIG_APP_ENABLE_USB_DEVICE) && defined(CONFIG_USB_DEV_ENABLE_CDC)
// Demo: echo whatever the host sends back to it, so a terminal on the PC side can verify
// the CDC-ACM link is alive.
void on_device_cdc_event(const Feature::USBDevice::CDC::Event& event) {
    ESP_LOGI(TAG, "USB Device CDC: %u bytes received, echoing back", (unsigned)event.length);
    Feature::USBDevice::CDC::send(event.data, event.length);
}
#endif

#if defined(CONFIG_APP_ENABLE_USB_DEVICE) && defined(CONFIG_USB_DEV_ENABLE_MIDI)
void on_device_midi_event(const Feature::USBDevice::MIDI::Event& event) {
    if (event.channel == 0xff) {
        // ESP_LOGI(TAG, "USB Device MIDI in: cable=%u cin=0x%x bytes=%u status=0x%02x data=%u,%u",
        //          event.cable, event.cin, event.length, event.status, event.data1, event.data2);
    } else {
        // ESP_LOGI(TAG, "USB Device MIDI in: cable=%u cin=0x%x bytes=%u status=0x%02x data=%u,%u channel=%u",
        //          event.cable, event.cin, event.length, event.status, event.data1, event.data2, event.channel + 1);
    }
    const uint8_t message[3] = {event.status, event.data1, event.data2};
    const esp_err_t result = Feature::USBDevice::MIDI::send(message, event.length);
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "USB Device MIDI echo failed: %s", esp_err_to_name(result));
    }
#if defined(CONFIG_APP_ENABLE_SYNTH)
    Feature::Synth::dispatch_midi(event.status, event.data1, event.data2);
#if defined(CONFIG_APP_ENABLE_WEBSERVER)
    Feature::Synth::broadcast_midi_event(event.status, event.data1, event.data2);
#endif
#endif
}
#endif

#if defined(CONFIG_APP_ENABLE_USB_DEVICE) && defined(CONFIG_USB_DEV_ENABLE_UAC2)
// Demo: loop whatever the host plays on the "speaker" straight back out on the "microphone",
// so a PC-side recorder shows exactly what was played - proof audio flows both ways.
// Test wiring: also play it out on the onboard ES8311 codec, if enabled, for a quick listen.
#if defined(CONFIG_ONBOARD_AUDIO_WM8960)
struct Wm8960PlaybackStatistics {
    std::atomic<uint32_t> callback_calls{0};
    std::atomic<uint32_t> nonzero_frames{0};
    std::atomic<uint32_t> bytes_requested{0};
    std::atomic<uint32_t> bytes_written{0};
    std::atomic<uint32_t> write_errors{0};
    std::atomic<uint16_t> peak_sample{0};
};

Wm8960PlaybackStatistics wm8960_playback_stats;
#endif

void on_device_speaker_audio(const int16_t* samples, size_t frame_count) {
#if defined(CONFIG_APP_ENABLE_SYNTH)
    // Once the synth engine is running it owns both the UAC2 mic-in tap (setUsbAudioTap,
    // see synth_manager.hpp) and the codec write path (its own render task) - a second writer
    // here would tear the output. Just hand the host's speaker audio to the engine's mix-in
    // ring instead; the engine mixes it into the render block and writes once, itself.
    Feature::Synth::usb_speaker_submit(samples, frame_count);
#else
    Feature::USBDevice::Audio::write_mic(samples, frame_count);
#if defined(CONFIG_ONBOARD_AUDIO_ES8311)
    esp_codec_dev_handle_t codec = Feature::Audio::ES8311::get_handle();
    if (codec != nullptr) {
        esp_codec_dev_write(codec, const_cast<int16_t*>(samples),
                             static_cast<int>(frame_count * 2 * sizeof(int16_t)));
    }
#endif
#if defined(CONFIG_ONBOARD_AUDIO_WM8960)
    const size_t sample_count = frame_count * 2;
    uint16_t block_peak = 0;
    for (size_t i = 0; i < sample_count; ++i) {
        const int32_t magnitude = samples[i] < 0 ? -static_cast<int32_t>(samples[i]) : samples[i];
        if (magnitude > block_peak) block_peak = static_cast<uint16_t>(magnitude);
    }
    wm8960_playback_stats.callback_calls.fetch_add(1, std::memory_order_relaxed);
    if (block_peak != 0) {
        wm8960_playback_stats.nonzero_frames.fetch_add(frame_count, std::memory_order_relaxed);
        uint16_t peak = wm8960_playback_stats.peak_sample.load(std::memory_order_relaxed);
        while (peak < block_peak &&
               !wm8960_playback_stats.peak_sample.compare_exchange_weak(
                   peak, block_peak, std::memory_order_relaxed)) {}
    }

    i2s_chan_handle_t tx = Feature::Audio::WM8960::get_tx_handle();
    if (tx != nullptr) {
        const size_t bytes_requested = sample_count * sizeof(int16_t);
        size_t bytes_written = 0;
        const esp_err_t result = i2s_channel_write(
            tx, samples, bytes_requested, &bytes_written, portMAX_DELAY);
        wm8960_playback_stats.bytes_requested.fetch_add(bytes_requested, std::memory_order_relaxed);
        wm8960_playback_stats.bytes_written.fetch_add(bytes_written, std::memory_order_relaxed);
        if (result != ESP_OK || bytes_written != bytes_requested) {
            wm8960_playback_stats.write_errors.fetch_add(1, std::memory_order_relaxed);
        }
    }
#endif
#endif // CONFIG_APP_ENABLE_SYNTH
}
#endif

#if defined(CONFIG_ONBOARD_AUDIO_ES8311) && defined(CONFIG_APP_ENABLE_USB_DEVICE) && defined(CONFIG_USB_DEV_ENABLE_UAC2)
// Demo: stream the onboard ES8311 microphone straight to the USB microphone (device -> host),
// so a PC-side recorder captures whatever the onboard mic picks up.
void es8311_mic_to_usb_task(void* arg) {
    (void)arg;
    constexpr size_t FRAME_COUNT = 128; // stereo frames per block, keeps latency low
    static int16_t buffer[FRAME_COUNT * 2];

    while (true) {
        if (!Feature::USBDevice::Audio::is_microphone_streaming()) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        esp_codec_dev_handle_t codec = Feature::Audio::ES8311::get_handle();
        if (codec == nullptr) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (esp_codec_dev_read(codec, buffer, sizeof(buffer)) == ESP_CODEC_DEV_OK) {
            Feature::USBDevice::Audio::write_mic(buffer, FRAME_COUNT);
        }
    }
}
#endif

#if defined(CONFIG_ONBOARD_AUDIO_WM8960) && defined(CONFIG_APP_ENABLE_USB_DEVICE) && defined(CONFIG_USB_DEV_ENABLE_UAC2)
// Demo: stream the WM8960 HAT's microphones straight to the USB microphone (device -> host),
// so a PC-side recorder captures whatever the HAT's onboard mics pick up.
void wm8960_mic_to_usb_task(void* arg) {
    (void)arg;
    constexpr size_t FRAME_COUNT = 128; // stereo frames per block, keeps latency low
    static int16_t buffer[FRAME_COUNT * 2];

    while (true) {
        if (!Feature::USBDevice::Audio::is_microphone_streaming()) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        i2s_chan_handle_t rx = Feature::Audio::WM8960::get_rx_handle();
        if (rx == nullptr) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        size_t bytes_read = 0;
        if (i2s_channel_read(rx, buffer, sizeof(buffer), &bytes_read, portMAX_DELAY) == ESP_OK) {
            Feature::USBDevice::Audio::write_mic(buffer, bytes_read / (2 * sizeof(int16_t)));
        }
    }
}
#endif

#if defined(CONFIG_ONBOARD_AUDIO_WM8960)
// Hook for the WM8960 HAT's onboard button. Fill in the desired action here.
void on_wm8960_button_event(const Feature::Audio::WM8960Button::Event& /*event*/) {
    ESP_LOGI(TAG, "WM8960 button pressed");
}
#endif

} // namespace

// Wires the optional feature modules' callbacks. Call before App::init(), since
// some features (BLE/USB Host reports) can fire as soon as init brings them up.
inline void register_callbacks() {
#if defined(CONFIG_APP_ENABLE_USB_HOST) && defined(CONFIG_USB_HOST_CLASS_HID)
    Feature::Keyboard::set_callback(on_keyboard_event);
    Feature::Mouse::set_callback(on_mouse_event);
#if defined(CONFIG_APP_ENABLE_LVGL)
    Feature::Mouse::set_connection_callback(update_mouse_cursor_visibility);
#endif
#endif

#if defined(CONFIG_APP_ENABLE_USB_HOST) && defined(CONFIG_USB_HOST_CLASS_MIDI)
    Feature::MIDI::set_callback(on_midi_event);
#endif

#if defined(CONFIG_APP_ENABLE_BLUETOOTH) && defined(CONFIG_BLE_ENABLE_HID_HOST)
    Feature::BLE::Keyboard::set_callback(on_ble_keyboard_event);
    Feature::BLE::Mouse::set_callback(on_ble_mouse_event);
    Feature::BLE::HID::set_callback(on_ble_hid_report);
#if defined(CONFIG_APP_ENABLE_LVGL)
    Feature::BLE::Mouse::set_connection_callback(update_mouse_cursor_visibility);
#endif
#endif

#if defined(CONFIG_APP_ENABLE_BLUETOOTH) && defined(CONFIG_BLE_ENABLE_MIDI_HOST)
    Feature::BLE::MIDI::set_callback(on_ble_midi_event);
#endif

#if defined(CONFIG_APP_ENABLE_USB_DEVICE) && defined(CONFIG_USB_DEV_ENABLE_CDC)
    Feature::USBDevice::CDC::set_callback(on_device_cdc_event);
#endif

#if defined(CONFIG_APP_ENABLE_USB_DEVICE) && defined(CONFIG_USB_DEV_ENABLE_MIDI)
    Feature::USBDevice::MIDI::set_callback(on_device_midi_event);
#endif

#if defined(CONFIG_APP_ENABLE_USB_DEVICE) && defined(CONFIG_USB_DEV_ENABLE_UAC2)
    Feature::USBDevice::Audio::set_speaker_callback(on_device_speaker_audio);
#endif

#if defined(CONFIG_ONBOARD_AUDIO_WM8960)
    Feature::Audio::WM8960Button::set_callback(on_wm8960_button_event);
#endif
}

// Starts the demo background tasks that bridge onboard codecs into the USB
// Audio microphone path. Call after App::init(), once the codecs are up.
inline void start_background_tasks() {
#if defined(CONFIG_ONBOARD_AUDIO_ES8311) && defined(CONFIG_APP_ENABLE_USB_DEVICE) && defined(CONFIG_USB_DEV_ENABLE_UAC2)
    xTaskCreatePinnedToCore(es8311_mic_to_usb_task, "es8311_mic_usb", 4096, nullptr, 5, nullptr,
                            AUDIO_CORE);
#endif

#if defined(CONFIG_ONBOARD_AUDIO_WM8960) && defined(CONFIG_APP_ENABLE_USB_DEVICE) && defined(CONFIG_USB_DEV_ENABLE_UAC2)
    xTaskCreatePinnedToCore(wm8960_mic_to_usb_task, "wm8960_mic_usb", 4096, nullptr, 5, nullptr,
                            AUDIO_CORE);
#endif
}

#if defined(CONFIG_APP_ENABLE_USB_DEVICE) && defined(CONFIG_USB_DEV_ENABLE_UAC2)
// Periodic USB Audio statistics log, meant to be called from app_main()'s main loop.
inline void log_usb_audio_stats_tick() {
    const auto stats = Feature::USBDevice::Audio::get_statistics();
    if (!Feature::USBDevice::Audio::is_streaming() && stats.speaker_frames_received == 0) {
        return;
    }
    ESP_LOGI(TAG, "USB Audio: speaker=%s mic=%s rx=%u requested=%u tx=%u dropped=%u frames",
             Feature::USBDevice::Audio::is_speaker_streaming() ? "active" : "idle",
             Feature::USBDevice::Audio::is_microphone_streaming() ? "active" : "idle",
             static_cast<unsigned>(stats.speaker_frames_received),
             static_cast<unsigned>(stats.microphone_frames_requested),
             static_cast<unsigned>(stats.microphone_frames_queued),
             static_cast<unsigned>(stats.microphone_frames_dropped));
#if defined(CONFIG_ONBOARD_AUDIO_WM8960)
    ESP_LOGI(TAG, "WM8960 USB playback: callbacks=%u nonzero=%u peak=%u requested=%u written=%u errors=%u",
             static_cast<unsigned>(wm8960_playback_stats.callback_calls.load(std::memory_order_relaxed)),
             static_cast<unsigned>(wm8960_playback_stats.nonzero_frames.load(std::memory_order_relaxed)),
             static_cast<unsigned>(wm8960_playback_stats.peak_sample.exchange(0, std::memory_order_relaxed)),
             static_cast<unsigned>(wm8960_playback_stats.bytes_requested.load(std::memory_order_relaxed)),
             static_cast<unsigned>(wm8960_playback_stats.bytes_written.load(std::memory_order_relaxed)),
             static_cast<unsigned>(wm8960_playback_stats.write_errors.load(std::memory_order_relaxed)));
#endif
}
#endif

} // namespace App

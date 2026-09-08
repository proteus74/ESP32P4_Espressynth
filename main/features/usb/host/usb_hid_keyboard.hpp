#pragma once

#include "usb_hid_mouse.hpp"
#include <cstdint>
#include <cstring>
#include "app_core.hpp"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "host/usbh.h"
#include "class/hid/hid_host.h"

namespace Feature::Keyboard {

struct Event {
    uint8_t address;
    uint8_t index;
    uint8_t keycode;
    char ascii;        // Printable character, '\0' if none (e.g. modifiers)
    bool shift;
    bool pressed;       // true = key down, false = key up (only modifiers report release)
    bool is_modifier;   // true if keycode is Ctrl/Shift/Alt/GUI (0xE0-0xE7)
};

using EventCallback = void (*)(const Event& event);

namespace {
constexpr const char* TAG = "USBHID";
constexpr uint8_t MAX_HID_REPORTS = 8;
constexpr TickType_t LED_TRANSFER_TIMEOUT = pdMS_TO_TICKS(250);
struct hid_interface_info_t {
    uint8_t report_count;
    tuh_hid_report_info_t reports[MAX_HID_REPORTS];
};
inline hid_interface_info_t hid_info[CFG_TUH_DEVICE_MAX + 1][CFG_TUH_HID]{};
inline uint8_t previous_keyboard[CFG_TUH_DEVICE_MAX + 1][CFG_TUH_HID][8]{};
struct alignas(CFG_TUH_MEM_DCACHE_LINE_SIZE) keyboard_led_report_t {
    uint8_t value;
    uint8_t padding[CFG_TUH_MEM_DCACHE_LINE_SIZE - 1];
};
inline keyboard_led_report_t keyboard_leds[CFG_TUH_DEVICE_MAX + 1][CFG_TUH_HID]{};
inline uint8_t keyboard_led_state[CFG_TUH_DEVICE_MAX + 1][CFG_TUH_HID]{};
inline bool keyboard_led_transfer_active[CFG_TUH_DEVICE_MAX + 1][CFG_TUH_HID]{};
inline bool keyboard_led_transfer_pending[CFG_TUH_DEVICE_MAX + 1][CFG_TUH_HID]{};
inline TickType_t keyboard_led_transfer_started[CFG_TUH_DEVICE_MAX + 1][CFG_TUH_HID]{};
inline QueueHandle_t keyboard_queue = nullptr;
inline QueueHandle_t mouse_queue = nullptr;
inline Feature::Keyboard::EventCallback keyboard_callback = nullptr;
inline Feature::Mouse::EventCallback mouse_callback = nullptr;
inline Feature::Mouse::ConnectionCallback mouse_connection_callback = nullptr;
inline bool mouse_interface_active[CFG_TUH_DEVICE_MAX + 1][CFG_TUH_HID]{};
inline uint8_t mouse_connection_count = 0;

// Switching the mouse to Report Protocol must NOT happen synchronously from within
// tuh_hid_mount_cb: at that exact point TinyUSB's own enumeration code (hub_port_get_status /
// hub_continue_port_scan, called from enum_new_device / enum_full_complete) may itself need a
// free host channel to keep discovering/enumerating other devices on the same hub.
constexpr int MOUSE_PROTOCOL_SWITCH_DELAY_TICKS = 100; // ~1s at the 10ms idle-poll rate
inline int mouse_protocol_switch_delay_remaining = 0; // 0 = nothing pending
inline uint8_t mouse_protocol_switch_address = 0;
inline uint8_t mouse_protocol_switch_index = 0;

inline void service_pending_mouse_protocol_switch() {
    if (mouse_protocol_switch_delay_remaining <= 0) return;
    if (--mouse_protocol_switch_delay_remaining > 0) return;
    ESP_LOGI(TAG, "Requesting Report Protocol for mouse: address=%u index=%u",
             mouse_protocol_switch_address, mouse_protocol_switch_index);
    const bool queued = tuh_hid_set_protocol(mouse_protocol_switch_address, mouse_protocol_switch_index,
                                              HID_PROTOCOL_REPORT);
    if (!queued) {
        ESP_LOGW(TAG, "Report Protocol request could not be queued, mouse stays in Boot Protocol (no wheel)");
    }
}

inline void submit_keyboard_led_report(uint8_t address, uint8_t index) {
    if (keyboard_led_transfer_active[address][index]) {
        keyboard_led_transfer_pending[address][index] = true;
        const TickType_t elapsed = xTaskGetTickCount() -
                                   keyboard_led_transfer_started[address][index];
        if (elapsed >= LED_TRANSFER_TIMEOUT) {
            const bool aborted = tuh_edpt_abort_xfer(address, 0);
            ESP_LOGW(TAG, "Keyboard LED transfer timed out: address=%u index=%u aborted=%s",
                     address, index, aborted ? "yes" : "no");
            keyboard_led_transfer_active[address][index] = false;
        }
        return;
    }

    keyboard_led_transfer_pending[address][index] = false;
    keyboard_leds[address][index].value = keyboard_led_state[address][index];
    keyboard_led_transfer_active[address][index] = true;
    keyboard_led_transfer_started[address][index] = xTaskGetTickCount();
    const bool queued = tuh_hid_set_report(
        address, index, 0, HID_REPORT_TYPE_OUTPUT,
        &keyboard_leds[address][index].value, 1);
    ESP_LOGI(TAG, "Keyboard LEDs: address=%u index=%u value=0x%02x buffer=%p queued=%s",
             address, index, keyboard_leds[address][index].value,
             static_cast<void*>(&keyboard_leds[address][index].value),
             queued ? "yes" : "no");
    if (!queued) {
        keyboard_led_transfer_active[address][index] = false;
        keyboard_led_transfer_pending[address][index] = true;
    }
}

inline void service_keyboard_led_reports() {
    for (uint8_t address = 1; address <= CFG_TUH_DEVICE_MAX; ++address) {
        for (uint8_t index = 0; index < CFG_TUH_HID; ++index) {
            if (keyboard_led_transfer_pending[address][index]) {
                submit_keyboard_led_report(address, index);
            }
        }
    }
}

inline bool contains_key(const uint8_t* report, uint8_t key) {
    for (size_t index = 2; index < 8; ++index) if (report[index] == key) return true;
    return false;
}

inline char key_to_ascii(uint8_t key, bool shift) {
    if (key >= 0x04 && key <= 0x1d) {
        const char value = static_cast<char>('a' + key - 0x04);
        return shift ? static_cast<char>(value - 'a' + 'A') : value;
    }
    static constexpr char normal[] = "1234567890\n\x1b\b\t -=[]\\#;'`,./";
    static constexpr char shifted[] = "!@#$%^&*()\n\x1b\b\t _+{}|~:\"~<>?";
    if (key >= 0x1e && key <= 0x38) return (shift ? shifted : normal)[key - 0x1e];
    return key == 0x2c ? ' ' : '\0';
}

inline bool resolve_report(uint8_t address, uint8_t index, const uint8_t*& report,
                           uint16_t& report_length, uint8_t& usage) {
    if (address > CFG_TUH_DEVICE_MAX || index >= CFG_TUH_HID) return false;

    const uint8_t protocol = tuh_hid_interface_protocol(address, index);
    usage = protocol == HID_ITF_PROTOCOL_KEYBOARD ? HID_USAGE_DESKTOP_KEYBOARD
            : protocol == HID_ITF_PROTOCOL_MOUSE ? HID_USAGE_DESKTOP_MOUSE : 0;

    if (protocol == HID_ITF_PROTOCOL_NONE) {
            const hid_interface_info_t& info = hid_info[address][index];
            const tuh_hid_report_info_t* report_info = nullptr;
            if (info.report_count == 1 && info.reports[0].report_id == 0) {
                report_info = &info.reports[0];
            } else if (report_length > 0) {
                for (uint8_t idx = 0; idx < info.report_count; ++idx) {
                    if (info.reports[idx].report_id == report[0]) {
                        report_info = &info.reports[idx];
                        break;
                    }
                }
                ++report;
                --report_length;
            }
            if (report_info != nullptr && report_info->usage_page == HID_USAGE_PAGE_DESKTOP) {
                usage = report_info->usage;
            }
    }
    return usage != 0;
}

inline void keyboard_task(void*) {
    Feature::Keyboard::Event event{};
    while (true) {
        if (xQueueReceive(keyboard_queue, &event, pdMS_TO_TICKS(10)) != pdTRUE) {
            service_keyboard_led_reports();
            service_pending_mouse_protocol_switch();
            continue;
        }
        if (event.is_modifier) {
            if (keyboard_callback != nullptr) keyboard_callback(event);
            continue;
        }

        bool shift = event.shift;
        if (event.keycode >= 0x04 && event.keycode <= 0x1d &&
            (keyboard_led_state[event.address][event.index] & 0x02) != 0) {
            shift = !shift;
        }
        event.shift = shift;
        event.ascii = key_to_ascii(event.keycode, shift);
        if (event.keycode == 0x39) keyboard_led_state[event.address][event.index] ^= 0x02;
        if (event.keycode == 0x53) keyboard_led_state[event.address][event.index] ^= 0x01;
        if (event.keycode == 0x47) keyboard_led_state[event.address][event.index] ^= 0x04;
        if (event.keycode == 0x39 || event.keycode == 0x53 || event.keycode == 0x47) {
            submit_keyboard_led_report(event.address, event.index);
        }
        if (keyboard_callback != nullptr) keyboard_callback(event);
    }
}

inline void mouse_task(void*) {
    Feature::Mouse::Event event{};
    while (xQueueReceive(mouse_queue, &event, portMAX_DELAY) == pdTRUE) {
        if (mouse_callback != nullptr) mouse_callback(event);
    }
}
} // namespace

/**
 * @brief Register a callback invoked from the keyboard task for every key event.
 *        Pass nullptr to unregister. Only one callback is stored at a time.
 */
inline void set_callback(EventCallback callback) {
    keyboard_callback = callback;
}

/**
 * @brief Human readable name for a modifier keycode (0xE0-0xE7), "Unknown" otherwise.
 */
inline const char* modifier_name(uint8_t keycode) {
    static constexpr const char* names[] = {
        "Left Ctrl", "Left Shift", "Left Alt", "Left GUI/Windows",
        "Right Ctrl", "Right Shift", "Right Alt", "Right GUI/Windows"
    };
    return keycode >= 0xe0 && keycode <= 0xe7 ? names[keycode - 0xe0] : "Unknown";
}

} // namespace Feature::Keyboard

namespace Feature::Mouse {

inline void set_callback(EventCallback callback) {
    Feature::Keyboard::mouse_callback = callback;
}

inline void set_connection_callback(ConnectionCallback callback) {
    Feature::Keyboard::mouse_connection_callback = callback;
}

inline bool is_connected() {
    return Feature::Keyboard::mouse_connection_count > 0;
}

} // namespace Feature::Mouse

extern "C" __attribute__((used)) void usb_hid_keyboard_start() {
    using namespace Feature::Keyboard;
    keyboard_queue = xQueueCreate(64, sizeof(Feature::Keyboard::Event));
    mouse_queue = xQueueCreate(1, sizeof(Feature::Mouse::Event));
    // HID input, not audio -- main core (see app_core.hpp).
    xTaskCreatePinnedToCore(keyboard_task, "usb_keyboard", 3072, nullptr, 4, nullptr, App::MAIN_CORE);
    xTaskCreatePinnedToCore(mouse_task, "usb_mouse", 3072, nullptr, 4, nullptr, App::MAIN_CORE);
    ESP_LOGI(TAG, "TinyUSB HID host enabled");
}

extern "C" __attribute__((used)) void tuh_hid_mount_cb(uint8_t address, uint8_t index,
                                        uint8_t const* descriptor, uint16_t length) {
    using namespace Feature::Keyboard;
    const uint8_t protocol = tuh_hid_interface_protocol(address, index);
    ESP_LOGI(TAG, "HID mounted: address=%u index=%u protocol=%u", address, index, protocol);
    if (address > CFG_TUH_DEVICE_MAX || index >= CFG_TUH_HID) return;

    std::memset(&hid_info[address][index], 0, sizeof(hid_info[address][index]));
    if (protocol == HID_ITF_PROTOCOL_NONE && descriptor != nullptr) {
        hid_info[address][index].report_count = tuh_hid_parse_report_descriptor(
            hid_info[address][index].reports, MAX_HID_REPORTS, descriptor, length);
        ESP_LOGI(TAG, "HID generic reports: address=%u index=%u count=%u", address, index,
                 hid_info[address][index].report_count);
    }
    std::memset(previous_keyboard[address][index], 0, sizeof(previous_keyboard[address][index]));
    keyboard_leds[address][index].value = 0;
    keyboard_led_state[address][index] = 0;
    keyboard_led_transfer_active[address][index] = false;
    keyboard_led_transfer_pending[address][index] = false;
    keyboard_led_transfer_started[address][index] = 0;
    tuh_hid_receive_report(address, index);

    if (protocol == HID_ITF_PROTOCOL_MOUSE) {
        mouse_protocol_switch_address = address;
        mouse_protocol_switch_index = index;
        mouse_protocol_switch_delay_remaining = MOUSE_PROTOCOL_SWITCH_DELAY_TICKS;
    }

    bool this_is_mouse = protocol == HID_ITF_PROTOCOL_MOUSE;
    if (!this_is_mouse && protocol == HID_ITF_PROTOCOL_NONE) {
        // Generic (report-protocol) HID device: a mouse looks like a Desktop/Mouse
        // usage in its parsed report descriptor rather than the boot-mouse protocol.
        const hid_interface_info_t& info = hid_info[address][index];
        for (uint8_t idx = 0; idx < info.report_count; ++idx) {
            if (info.reports[idx].usage_page == HID_USAGE_PAGE_DESKTOP &&
                info.reports[idx].usage == HID_USAGE_DESKTOP_MOUSE) {
                this_is_mouse = true;
                break;
            }
        }
    }
    mouse_interface_active[address][index] = this_is_mouse;
    if (this_is_mouse) {
        const bool was_disconnected = mouse_connection_count == 0;
        ++mouse_connection_count;
        // Bump the counter before firing the callback: it re-queries is_connected()
        // (see app_events.hpp's update_mouse_cursor_visibility()), which would still
        // see the pre-increment count of 0 otherwise and conclude nothing is connected.
        if (was_disconnected && mouse_connection_callback != nullptr) {
            mouse_connection_callback(true);
        }
    }
}

extern "C" __attribute__((used)) void tuh_hid_set_protocol_complete_cb(uint8_t address, uint8_t index, uint8_t protocol) {
    using namespace Feature::Keyboard;
    ESP_LOGI(TAG, "HID protocol set: address=%u index=%u protocol=%s", address, index,
             protocol == HID_PROTOCOL_REPORT ? "report" : "boot");
}

extern "C" __attribute__((used)) void tuh_hid_umount_cb(uint8_t address, uint8_t index) {
    using namespace Feature::Keyboard;
    ESP_LOGI(TAG, "HID removed: address=%u index=%u", address, index);
    if (address > CFG_TUH_DEVICE_MAX || index >= CFG_TUH_HID) return;
    std::memset(&hid_info[address][index], 0, sizeof(hid_info[address][index]));
    std::memset(previous_keyboard[address][index], 0, sizeof(previous_keyboard[address][index]));
    keyboard_led_state[address][index] = 0;
    keyboard_led_transfer_active[address][index] = false;
    keyboard_led_transfer_pending[address][index] = false;
    keyboard_led_transfer_started[address][index] = 0;

    if (mouse_interface_active[address][index]) {
        mouse_interface_active[address][index] = false;
        if (mouse_connection_count > 0) --mouse_connection_count;
        if (mouse_connection_count == 0 && mouse_connection_callback != nullptr) {
            mouse_connection_callback(false);
        }
    }
}

extern "C" __attribute__((used)) void tuh_hid_report_received_cb(uint8_t address, uint8_t index,
                                                  uint8_t const* report, uint16_t length) {
    using namespace Feature::Keyboard;
    if (length > CFG_TUH_HID_EPIN_BUFSIZE) {
        tuh_hid_receive_report(address, index);
        return;
    }

    uint8_t report_copy[CFG_TUH_HID_EPIN_BUFSIZE]{};
    std::memcpy(report_copy, report, length);
    tuh_hid_receive_report(address, index);

    report = report_copy;
    uint8_t usage = 0;
    if (resolve_report(address, index, report, length, usage) &&
        usage == HID_USAGE_DESKTOP_KEYBOARD && length >= 8) {
        bool valid = true;
        for (size_t offset = 2; offset < 8; ++offset) {
            if (report[offset] >= 1 && report[offset] <= 3) valid = false;
        }
        if (valid) {
            const bool shift = (report[0] & 0x22) != 0;
            const uint8_t changed_modifiers = report[0] ^ previous_keyboard[address][index][0];
            for (uint8_t bit = 0; bit < 8; ++bit) {
                if ((changed_modifiers & (1u << bit)) != 0) {
                    const Feature::Keyboard::Event event{
                        address, index, static_cast<uint8_t>(0xe0 + bit), '\0', shift,
                        (report[0] & (1u << bit)) != 0, true
                    };
                    if (keyboard_queue == nullptr || xQueueSend(keyboard_queue, &event, 0) != pdTRUE) {
                        ESP_LOGW(TAG, "Keyboard event queue full");
                    }
                }
            }
            for (size_t offset = 2; offset < 8; ++offset) {
                const uint8_t key = report[offset];
                if (key != 0 && !contains_key(previous_keyboard[address][index], key)) {
                    const Feature::Keyboard::Event event{address, index, key, '\0', shift, true, false};
                    if (keyboard_queue == nullptr || xQueueSend(keyboard_queue, &event, 0) != pdTRUE) {
                        ESP_LOGW(TAG, "Keyboard event queue full");
                    }
                }
            }
            std::memcpy(previous_keyboard[address][index], report, 8);
        }
    } else if (usage == HID_USAGE_DESKTOP_MOUSE && length >= 3 && mouse_queue != nullptr) {
        const Feature::Mouse::Event event{
            report[0], static_cast<int8_t>(report[1]), static_cast<int8_t>(report[2]),
            length >= 4 ? static_cast<int8_t>(report[3]) : static_cast<int8_t>(0)
        };
        xQueueOverwrite(mouse_queue, &event);
    }
}

extern "C" __attribute__((used)) void tuh_hid_report_sent_cb(uint8_t address, uint8_t index,
                                              uint8_t const* report, uint16_t length) {
    (void)address; (void)index; (void)report; (void)length;
}

extern "C" __attribute__((used)) void tuh_hid_set_report_complete_cb(uint8_t address, uint8_t index,
                                                      uint8_t report_id, uint8_t report_type,
                                                      uint16_t length) {
    using namespace Feature::Keyboard;
    ESP_LOGI(TAG, "Keyboard LEDs complete: address=%u index=%u id=%u type=%u length=%u",
             address, index, report_id, report_type, length);
    if (address > CFG_TUH_DEVICE_MAX || index >= CFG_TUH_HID) return;

    keyboard_led_transfer_active[address][index] = false;
    if (length == 0) {
        keyboard_led_transfer_pending[address][index] = true;
    }
    if (keyboard_led_transfer_pending[address][index]) {
        keyboard_led_transfer_pending[address][index] = false;
        submit_keyboard_led_report(address, index);
    } else {
        keyboard_led_transfer_pending[address][index] = false;
    }
}

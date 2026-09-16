#pragma once
#include "app_core.hpp"
#include "features/io/i2c_device_scan.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <atomic>
#include <cstdint>

// Core-0-only I2C polling/writing for "CV In"/"CV Out" widgets (ADS1115/GP8413). Never
// touches Core 1 - hands values to/from the render task via depth-1 "mailbox" queues
// (xQueueOverwrite/non-blocking xQueueReceive), same idiom as SynthEngine's own MIDI-event
// drain (SynthEngine.h). A mailbox, not a deep FIFO, is deliberate: CV-Out's render() produces
// a new target every audio block (~2.7ms) far faster than this task can physically write it
// (I2C-bound, see below), so only the latest value ever matters in either direction.
namespace Feature::IO {

constexpr size_t kCvInSlotCount = 16;  // 2 ports * 4 ADS1115 addresses * 2 channels
constexpr size_t kCvOutSlotCount = 32; // 2 ports * 8 GP8413 addresses * 2 channels

struct CvInSample {
    float volts = 0.0f;
    bool found = false;
};
struct CvOutTarget {
    float volts = 0.0f;
};

namespace {
constexpr const char* BUS_MGR_TAG = "I2CBusManager";

QueueHandle_t s_cvInMailbox[kCvInSlotCount] = {};
QueueHandle_t s_cvOutMailbox[kCvOutSlotCount] = {};
std::atomic<bool> s_cvOutFound[kCvOutSlotCount] = {};
float s_cvOutLastTarget[kCvOutSlotCount] = {}; // bus-manager task-owned only, never read elsewhere

// Which slots the currently-loaded preset actually references - the bus manager only issues
// real I2C transactions for these, never wastefully polling/writing unwired channels. A
// plain atomic (not a spinlock) suffices: each is a single integer written wholesale by the
// preset loader task and read wholesale here, both on Core 0 but different tasks.
std::atomic<uint16_t> s_activeCvInMask{0};
std::atomic<uint32_t> s_activeCvOutMask{0};
} // namespace

// port: 0/1. address: full 7-bit I2C address (0x48-0x4B / 0x58-0x5F). channel: 0/1.
// Returns -1 for an out-of-range combination.
inline int8_t resolve_cv_in_slot(int8_t port, uint8_t address, uint8_t channel) {
    if (port < 0 || port > 1 || channel > 1 || address < 0x48 || address > 0x4B) return -1;
    return static_cast<int8_t>(port * 8 + (address - 0x48) * 2 + channel);
}
inline int8_t resolve_cv_out_slot(int8_t port, uint8_t address, uint8_t channel) {
    if (port < 0 || port > 1 || channel > 1 || address < 0x58 || address > 0x5F) return -1;
    return static_cast<int8_t>(port * 16 + (address - 0x58) * 2 + channel);
}

inline void set_active_cv_in_mask(uint16_t mask) { s_activeCvInMask.store(mask, std::memory_order_relaxed); }
inline void set_active_cv_out_mask(uint32_t mask) { s_activeCvOutMask.store(mask, std::memory_order_relaxed); }

// --- Render-task-side API (called from nodes/CvInNode.h / nodes/CvOutNode.h) ---

// Drains the mailbox for `slot` (returns false, `out` untouched, if nothing arrived since
// the last call - CvInNode.h keeps its own last-known-good value in that case).
inline bool read_cv_in_sample(int8_t slot, CvInSample& out) {
    return slot >= 0 && xQueueReceive(s_cvInMailbox[slot], &out, 0) == pdTRUE;
}
inline void post_cv_out_target(int8_t slot, float volts) {
    if (slot < 0) return;
    CvOutTarget t{volts};
    xQueueOverwrite(s_cvOutMailbox[slot], &t);
}
inline bool cv_out_found(int8_t slot) {
    return slot >= 0 && s_cvOutFound[slot].load(std::memory_order_relaxed);
}

namespace {

void i2c_bus_manager_task(void*) {
    while (true) {
        const uint16_t inMask = s_activeCvInMask.load(std::memory_order_relaxed);
        const uint32_t outMask = s_activeCvOutMask.load(std::memory_order_relaxed);
        int didWork = 0;

        for (int slot = 0; slot < static_cast<int>(kCvInSlotCount); ++slot) {
            if ((inMask & (1u << slot)) == 0) continue;
            const int port = slot / 8;
            const int withinPort = slot % 8;
            const uint8_t address = static_cast<uint8_t>(0x48 + withinPort / 2);
            const uint8_t channel = static_cast<uint8_t>(withinPort % 2);
            ADS1115Driver* drv = ads1115(port, address);
            CvInSample sample;
            sample.found = drv != nullptr && drv->read_channel(channel, sample.volts) == ESP_OK;
            xQueueOverwrite(s_cvInMailbox[slot], &sample);
            ++didWork;
        }

        for (int slot = 0; slot < static_cast<int>(kCvOutSlotCount); ++slot) {
            if ((outMask & (1u << slot)) == 0) continue;
            const int port = slot / 16;
            const int withinPort = slot % 16;
            const uint8_t address = static_cast<uint8_t>(0x58 + withinPort / 2);
            const uint8_t channel = static_cast<uint8_t>(withinPort % 2);
            CvOutTarget target;
            if (xQueueReceive(s_cvOutMailbox[slot], &target, 0) == pdTRUE) {
                s_cvOutLastTarget[slot] = target.volts;
            }
            GP8413Driver* drv = gp8413(port, address);
            const bool found = drv != nullptr && drv->write_channel(channel, s_cvOutLastTarget[slot]) == ESP_OK;
            s_cvOutFound[slot].store(found, std::memory_order_relaxed);
            ++didWork;
        }

        // Nothing placed yet (or nothing configured) - back off instead of spinning. Once
        // something is active, loop right away: the I2C transactions themselves (each
        // read_channel()/write_channel() blocks a couple of ms) already pace this task, an
        // extra fixed delay here would only slow every configured channel down further.
        if (didWork == 0) vTaskDelay(pdMS_TO_TICKS(50));
    }
}

} // namespace

inline void i2c_bus_manager_init() {
    for (size_t i = 0; i < kCvInSlotCount; ++i) s_cvInMailbox[i] = xQueueCreate(1, sizeof(CvInSample));
    for (size_t i = 0; i < kCvOutSlotCount; ++i) s_cvOutMailbox[i] = xQueueCreate(1, sizeof(CvOutTarget));
    // Priority 5: same tier as the USB-MIDI relay task (usb_midi_host.hpp). Stack 4096:
    // starting budget matching usb_host_manager.hpp's own cyclic-poll task - check the
    // actual high-water-mark via the CLI "top" command after bring-up.
    xTaskCreatePinnedToCore(i2c_bus_manager_task, "i2c_bus_mgr", 4096, nullptr, 5, nullptr, App::MAIN_CORE);
    ESP_LOGI(BUS_MGR_TAG, "I2C bus manager task started on core %d", static_cast<int>(App::MAIN_CORE));
}

} // namespace Feature::IO

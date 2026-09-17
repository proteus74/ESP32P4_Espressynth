#pragma once
#include "app_core.hpp"
#include "features/io/i2c_device_scan.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <atomic>
#include <cstdint>

// One Core-0 scheduler task per enabled I2C bus handles "CV In"/"CV Out"
// (ADS1115/GP8413). The buses progress independently and never touch Core 1; values cross
// to/from the render task through depth-1 "mailbox" queues
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
uint16_t s_cvOutLastCode[kCvOutSlotCount] = {};
TickType_t s_cvOutNextAttempt[kCvOutSlotCount] = {};
bool s_cvOutHasWritten[kCvOutSlotCount] = {};
bool s_cvInStatusKnown[kCvInSlotCount] = {};
bool s_cvInLastFound[kCvInSlotCount] = {};
float s_cvInLastVolts[kCvInSlotCount] = {};
bool s_cvOutStatusKnown[kCvOutSlotCount] = {};
bool s_cvOutLastFound[kCvOutSlotCount] = {};
TaskHandle_t s_busWorkerTasks[2] = {};
int s_busWorkerPorts[2] = {0, 1};

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
    return slot >= 0 && slot < static_cast<int8_t>(kCvInSlotCount) && s_cvInMailbox[slot] != nullptr &&
           xQueueReceive(s_cvInMailbox[slot], &out, 0) == pdTRUE;
}
inline void post_cv_out_target(int8_t slot, float volts) {
    if (slot < 0 || slot >= static_cast<int8_t>(kCvOutSlotCount) || s_cvOutMailbox[slot] == nullptr) return;
    CvOutTarget t{volts};
    xQueueOverwrite(s_cvOutMailbox[slot], &t);
}
inline bool cv_out_found(int8_t slot) {
    return slot >= 0 && slot < static_cast<int8_t>(kCvOutSlotCount) &&
           s_cvOutFound[slot].load(std::memory_order_relaxed);
}

namespace {

inline unsigned count_bits(uint32_t value) {
    unsigned count = 0;
    while (value != 0) {
        value &= value - 1;
        ++count;
    }
    return count;
}

inline bool tick_due(TickType_t now, TickType_t deadline) {
    return static_cast<int32_t>(now - deadline) >= 0;
}

inline void log_device_status(const char* device, const char* operation, int port, uint8_t address,
                              uint8_t channel, esp_err_t err, bool& known, bool& lastFound) {
    const bool found = err == ESP_OK;
    if (!known || found != lastFound) {
        if (found) {
            ESP_LOGI(BUS_MGR_TAG, "%s %s active: I2C%d 0x%02X channel %u", device, operation,
                     port, address, static_cast<unsigned>(channel + 1));
        } else {
            ESP_LOGW(BUS_MGR_TAG, "%s %s unavailable: I2C%d 0x%02X channel %u (%s)", device,
                     operation, port, address, static_cast<unsigned>(channel + 1), esp_err_to_name(err));
        }
        known = true;
        lastFound = found;
    }
}

struct AdsSchedule {
    int8_t pendingChannel = -1;
    int8_t selectedChannel = -1;
    uint8_t nextChannel = 0;
    TickType_t readyAt = 0;
    TickType_t nextDue[2] = {};
};

void publish_ads_result(int slot, int port, uint8_t address, uint8_t channel,
                        esp_err_t err, float volts) {
    CvInSample sample{volts, err == ESP_OK};
    xQueueOverwrite(s_cvInMailbox[slot], &sample);
    if (sample.found) s_cvInLastVolts[slot] = volts;
    log_device_status("DFRobot ADS1115", "read", port, address, channel, err,
                      s_cvInStatusKnown[slot], s_cvInLastFound[slot]);
}

void i2c_bus_worker_task(void* arg) {
    const int port = *static_cast<const int*>(arg);
    const int inBase = port * 8;
    const int outBase = port * 16;
    uint16_t previousInMask = 0;
    uint32_t previousOutMask = 0;
    AdsSchedule adsSchedules[4];
    uint32_t adcReads = 0;
    uint32_t adcErrors = 0;
    uint32_t dacTargets = 0;
    uint32_t dacWrites = 0;
    uint32_t dacErrors = 0;
#if defined(CONFIG_APP_ADS1115_POLL_RATE_HZ)
    constexpr uint32_t adsPollRateHz = CONFIG_APP_ADS1115_POLL_RATE_HZ;
#else
    constexpr uint32_t adsPollRateHz = 20;
#endif
    const TickType_t adsPollPeriod = pdMS_TO_TICKS((1000u + adsPollRateHz - 1) / adsPollRateHz);
    const TickType_t adsSettlingTime = pdMS_TO_TICKS(ADS1115Driver::kChannelSettlingMs);
    TickType_t nextSummary = xTaskGetTickCount() + pdMS_TO_TICKS(5000);

    while (true) {
        const uint16_t inMask = static_cast<uint16_t>(
            (s_activeCvInMask.load(std::memory_order_relaxed) >> inBase) & 0xFFu);
        const uint32_t outMask =
            (s_activeCvOutMask.load(std::memory_order_relaxed) >> outBase) & 0xFFFFu;
        const TickType_t now = xTaskGetTickCount();

        if (inMask != previousInMask || outMask != previousOutMask) {
            const uint16_t newInputs = static_cast<uint16_t>(inMask & ~previousInMask);
            const uint32_t newOutputs = outMask & ~previousOutMask;
            for (int local = 0; local < 8; ++local) {
                const int slot = inBase + local;
                if ((newInputs & (1u << local)) != 0) {
                    s_cvInStatusKnown[slot] = false;
                    adsSchedules[local / 2].nextDue[local % 2] = now;
                }
            }
            for (int local = 0; local < 16; ++local) {
                if ((newOutputs & (1u << local)) == 0) continue;
                const int slot = outBase + local;
                s_cvOutStatusKnown[slot] = false;
                s_cvOutHasWritten[slot] = false;
                s_cvOutFound[slot].store(false, std::memory_order_relaxed);
            }
            ESP_LOGI(BUS_MGR_TAG,
                     "I2C%d routes: %u ADS1115 input(s), %u GP8413 output(s)", port,
                     count_bits(inMask), count_bits(outMask));
            previousInMask = inMask;
            previousOutMask = outMask;
        }

        // Each module owns one channel selector. Complete its pending read before selecting
        // another channel; the configured polling period controls the sampling cadence.
        for (int device = 0; device < 4; ++device) {
            AdsSchedule& schedule = adsSchedules[device];
            if (schedule.pendingChannel < 0) continue;
            const uint8_t channel = static_cast<uint8_t>(schedule.pendingChannel);
            const int local = device * 2 + channel;
            if ((inMask & (1u << local)) == 0) {
                schedule.pendingChannel = -1;
                schedule.selectedChannel = -1;
                continue;
            }
            if (!tick_due(now, schedule.readyAt)) continue;

            const uint8_t address = static_cast<uint8_t>(0x48 + device);
            ADS1115Driver* drv = ads1115(port, address);
            float volts = 0.0f;
            const esp_err_t err = drv != nullptr ? drv->read_selected(volts) : ESP_ERR_NOT_FOUND;
            if (drv != nullptr) ++adcReads;
            if (err != ESP_OK) {
                ++adcErrors;
                schedule.selectedChannel = -1;
                schedule.nextDue[channel] = now + pdMS_TO_TICKS(1000);
            } else {
                schedule.nextDue[channel] = now + adsPollPeriod;
            }
            publish_ads_result(inBase + local, port, address, channel, err, volts);
            schedule.pendingChannel = -1;
        }

        for (int device = 0; device < 4; ++device) {
            AdsSchedule& schedule = adsSchedules[device];
            if (schedule.pendingChannel >= 0) continue;
            for (int candidate = 0; candidate < 2; ++candidate) {
                const uint8_t channel = static_cast<uint8_t>((schedule.nextChannel + candidate) & 1u);
                const int local = device * 2 + channel;
                if ((inMask & (1u << local)) == 0 || !tick_due(now, schedule.nextDue[channel])) continue;

                const int slot = inBase + local;
                const uint8_t address = static_cast<uint8_t>(0x48 + device);
                ADS1115Driver* drv = ads1115(port, address);

                // Keep an unchanged channel selected and read its continuously refreshed data
                // register directly, so a single active route reaches its configured poll rate.
                if (schedule.selectedChannel == static_cast<int8_t>(channel)) {
                    float volts = 0.0f;
                    const esp_err_t err = drv != nullptr ? drv->read_selected(volts) : ESP_ERR_NOT_FOUND;
                    if (drv != nullptr) ++adcReads;
                    if (err != ESP_OK) {
                        ++adcErrors;
                        schedule.selectedChannel = -1;
                        schedule.nextDue[channel] = now + pdMS_TO_TICKS(1000);
                    } else {
                        schedule.nextDue[channel] = now + adsPollPeriod;
                    }
                    publish_ads_result(slot, port, address, channel, err, volts);
                    schedule.nextChannel = static_cast<uint8_t>(channel ^ 1u);
                    break;
                }

                const esp_err_t err = drv != nullptr ? drv->select_channel(channel) : ESP_ERR_NOT_FOUND;
                if (err == ESP_OK) {
                    schedule.pendingChannel = static_cast<int8_t>(channel);
                    schedule.selectedChannel = static_cast<int8_t>(channel);
                    schedule.readyAt = now + adsSettlingTime;
                    schedule.nextChannel = static_cast<uint8_t>(channel ^ 1u);
                } else {
                    ++adcErrors;
                    schedule.selectedChannel = -1;
                    schedule.nextDue[channel] = now + pdMS_TO_TICKS(1000);
                    publish_ads_result(slot, port, address, channel, err, 0.0f);
                }
                break;
            }
        }

        for (int local = 0; local < 16; ++local) {
            if ((outMask & (1u << local)) == 0) continue;
            const int slot = outBase + local;
            const uint8_t address = static_cast<uint8_t>(0x58 + local / 2);
            const uint8_t channel = static_cast<uint8_t>(local % 2);
            CvOutTarget target;
            const bool received = xQueueReceive(s_cvOutMailbox[slot], &target, 0) == pdTRUE;
            if (received) {
                s_cvOutLastTarget[slot] = target.volts;
                ++dacTargets;
            }
            GP8413Driver* drv = gp8413(port, address);
            if (drv == nullptr) {
                s_cvOutFound[slot].store(false, std::memory_order_relaxed);
                log_device_status("DFRobot GP8413", "write", port, address, channel, ESP_ERR_NOT_FOUND,
                                  s_cvOutStatusKnown[slot], s_cvOutLastFound[slot]);
                continue;
            }

            const uint16_t code = GP8413Driver::voltage_to_code(s_cvOutLastTarget[slot]);
            const bool changed = !s_cvOutHasWritten[slot] || code != s_cvOutLastCode[slot];
            const bool refreshDue = s_cvOutStatusKnown[slot] && s_cvOutLastFound[slot] &&
                                    tick_due(now, s_cvOutNextAttempt[slot]);
            const bool retryAllowed = !s_cvOutStatusKnown[slot] || s_cvOutLastFound[slot] ||
                                      tick_due(now, s_cvOutNextAttempt[slot]);
            if ((!changed && !refreshDue) || !retryAllowed) continue;

            const esp_err_t err = drv->write_channel(channel, s_cvOutLastTarget[slot]);
            ++dacWrites;
            if (err == ESP_OK) {
                s_cvOutLastCode[slot] = code;
                s_cvOutHasWritten[slot] = true;
                s_cvOutNextAttempt[slot] = now + pdMS_TO_TICKS(5000);
            } else {
                ++dacErrors;
                s_cvOutNextAttempt[slot] = now + pdMS_TO_TICKS(1000);
            }
            s_cvOutFound[slot].store(err == ESP_OK, std::memory_order_relaxed);
            log_device_status("DFRobot GP8413", "write", port, address, channel, err,
                              s_cvOutStatusKnown[slot], s_cvOutLastFound[slot]);
        }

        if (tick_due(now, nextSummary)) {
            if (inMask != 0 || outMask != 0) {
                // Debug-only (ESP_LOGD, hidden at the default log level) - this fires every 5s
                // for the whole session's runtime, unlike log_device_status()'s edge-triggered
                // found/unavailable transitions above, which stay ESP_LOGI/ESP_LOGW.
                ESP_LOGD(BUS_MGR_TAG,
                         "I2C%d activity (5s): ADS1115 routes=%u reads=%lu errors=%lu; "
                         "GP8413 routes=%u targets=%lu writes=%lu errors=%lu",
                         port, count_bits(inMask),
                         static_cast<unsigned long>(adcReads), static_cast<unsigned long>(adcErrors),
                         count_bits(outMask), static_cast<unsigned long>(dacTargets),
                         static_cast<unsigned long>(dacWrites), static_cast<unsigned long>(dacErrors));
                for (int local = 0; local < 8; ++local) {
                    const int slot = inBase + local;
                    if ((inMask & (1u << local)) == 0 || !s_cvInLastFound[slot]) continue;
                    ESP_LOGD(BUS_MGR_TAG, "DFRobot ADS1115 sample: I2C%d 0x%02X channel %u = %.4f V",
                             port, 0x48 + local / 2,
                             static_cast<unsigned>(local % 2 + 1),
                             static_cast<double>(s_cvInLastVolts[slot]));
                }
            }
            adcReads = adcErrors = dacTargets = dacWrites = dacErrors = 0;
            nextSummary = now + pdMS_TO_TICKS(5000);
        }

        vTaskDelay(pdMS_TO_TICKS((inMask == 0 && outMask == 0) ? 50 : 1));
    }
}

inline void delete_mailboxes() {
    for (QueueHandle_t& mailbox : s_cvInMailbox) {
        if (mailbox != nullptr) vQueueDelete(mailbox);
        mailbox = nullptr;
    }
    for (QueueHandle_t& mailbox : s_cvOutMailbox) {
        if (mailbox != nullptr) vQueueDelete(mailbox);
        mailbox = nullptr;
    }
}

} // namespace

inline esp_err_t i2c_bus_manager_init() {
    for (size_t i = 0; i < kCvInSlotCount; ++i) {
        s_cvInMailbox[i] = xQueueCreate(1, sizeof(CvInSample));
        if (s_cvInMailbox[i] == nullptr) {
            delete_mailboxes();
            ESP_LOGE(BUS_MGR_TAG, "Failed to allocate CV In mailbox %u", static_cast<unsigned>(i));
            return ESP_ERR_NO_MEM;
        }
    }
    for (size_t i = 0; i < kCvOutSlotCount; ++i) {
        s_cvOutMailbox[i] = xQueueCreate(1, sizeof(CvOutTarget));
        if (s_cvOutMailbox[i] == nullptr) {
            delete_mailboxes();
            ESP_LOGE(BUS_MGR_TAG, "Failed to allocate CV Out mailbox %u", static_cast<unsigned>(i));
            return ESP_ERR_NO_MEM;
        }
    }
    int workerCount = 0;
#if defined(CONFIG_APP_ENABLE_I2C0)
    if (xTaskCreatePinnedToCore(i2c_bus_worker_task, "i2c0_sched", 4096, &s_busWorkerPorts[0], 5,
                                &s_busWorkerTasks[0], App::MAIN_CORE) == pdPASS) {
        ++workerCount;
    }
#endif
#if defined(CONFIG_APP_ENABLE_I2C1)
    if (xTaskCreatePinnedToCore(i2c_bus_worker_task, "i2c1_sched", 4096, &s_busWorkerPorts[1], 5,
                                &s_busWorkerTasks[1], App::MAIN_CORE) == pdPASS) {
        ++workerCount;
    }
#endif
    const int expectedWorkers =
#if defined(CONFIG_APP_ENABLE_I2C0) && defined(CONFIG_APP_ENABLE_I2C1)
        2;
#elif defined(CONFIG_APP_ENABLE_I2C0) || defined(CONFIG_APP_ENABLE_I2C1)
        1;
#else
        0;
#endif
    if (workerCount != expectedWorkers) {
        for (TaskHandle_t& task : s_busWorkerTasks) {
            if (task != nullptr) vTaskDelete(task);
            task = nullptr;
        }
        delete_mailboxes();
        ESP_LOGE(BUS_MGR_TAG, "Failed to start all I2C scheduler tasks");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(BUS_MGR_TAG, "%d I2C scheduler task(s) started on core %d; ADS1115 target=%d polls/s",
             workerCount, static_cast<int>(App::MAIN_CORE),
#if defined(CONFIG_APP_ADS1115_POLL_RATE_HZ)
             CONFIG_APP_ADS1115_POLL_RATE_HZ
#else
             20
#endif
    );
    return ESP_OK;
}

} // namespace Feature::IO

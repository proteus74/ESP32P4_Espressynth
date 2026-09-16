#pragma once
#include "boards/board_init.hpp"
#include "features/io/i2c_device_registry.h"
#include "features/io/ads1115_driver.hpp"
#include "features/io/gp8413_driver.hpp"
#include "esp_log.h"

namespace Feature::IO {
namespace {

constexpr const char* SCAN_TAG = "I2CDeviceScan";

// Probes [addr_lo, addr_hi] on `bus`, collecting every ACKing address into out[] (capped at
// outCap). Unlike a first-match-only scan, this supports multiple physical units of the same
// chip on one bus (e.g. two ADS1115 boards at 0x48 and 0x49) - the CV widget's address dropdown
// needs to list all of them, not just the first. Same zero-byte i2c_master_probe() approach as
// board_init.cpp's i2c_scan_bus().
inline int probe_all_addresses(i2c_master_bus_handle_t bus, uint8_t addr_lo, uint8_t addr_hi,
                                uint8_t* out, int outCap) {
    int count = 0;
    for (uint16_t addr = addr_lo; addr <= addr_hi && count < outCap; ++addr) {
        if (i2c_master_probe(bus, static_cast<uint8_t>(addr), 20) == ESP_OK) {
            out[count++] = static_cast<uint8_t>(addr);
        }
    }
    return count;
}

struct Ads1115Slot {
    bool present = false;
    ADS1115Driver driver;
};
struct Gp8413Slot {
    bool present = false;
    GP8413Driver driver;
};
// [port][address offset from the chip's base address] - up to 4 ADS1115 (0x48-0x4B) / 8 GP8413
// (0x58-0x5F) per port, matching each chip's address-pin range.
inline Ads1115Slot s_ads1115[2][4];
inline Gp8413Slot s_gp8413[2][8];

inline void scan_ads1115(int port) {
    i2c_master_bus_handle_t bus = Board::get_i2c_bus_handle(port);
    if (bus == nullptr) {
        ESP_LOGW(SCAN_TAG, "ADS1115: I2C port %d not enabled - check I2C Subsystem Kconfig", port);
        return;
    }
    uint8_t found[4];
    const int count = probe_all_addresses(bus, 0x48, 0x4B, found, 4);
    if (count == 0) {
        ESP_LOGW(SCAN_TAG, "ADS1115: not found on I2C%d (0x48-0x4B)", port);
        return;
    }
    for (int i = 0; i < count; ++i) {
        const uint8_t addr = found[i];
        ESP_LOGI(SCAN_TAG, "ADS1115: found on I2C%d at 0x%02X", port, addr);
        I2CDeviceRegistry::add("ADS1115", static_cast<uint8_t>(port), addr);
        Ads1115Slot& slot = s_ads1115[port][addr - 0x48];
        if (slot.driver.init(bus, addr) == ESP_OK) {
            slot.present = true;
        } else {
            ESP_LOGW(SCAN_TAG, "ADS1115: found but driver init failed on I2C%d at 0x%02X", port, addr);
        }
    }
}

inline void scan_gp8413(int port) {
    i2c_master_bus_handle_t bus = Board::get_i2c_bus_handle(port);
    if (bus == nullptr) {
        ESP_LOGW(SCAN_TAG, "GP8413: I2C port %d not enabled - check I2C Subsystem Kconfig", port);
        return;
    }
    uint8_t found[8];
    const int count = probe_all_addresses(bus, 0x58, 0x5F, found, 8);
    if (count == 0) {
        ESP_LOGW(SCAN_TAG, "GP8413: not found on I2C%d (0x58-0x5F)", port);
        return;
    }
    for (int i = 0; i < count; ++i) {
        const uint8_t addr = found[i];
        ESP_LOGI(SCAN_TAG, "GP8413: found on I2C%d at 0x%02X", port, addr);
        I2CDeviceRegistry::add("GP8413", static_cast<uint8_t>(port), addr);
        Gp8413Slot& slot = s_gp8413[port][addr - 0x58];
        if (slot.driver.init(bus, addr) == ESP_OK) {
            slot.present = true;
        } else {
            ESP_LOGW(SCAN_TAG, "GP8413: found but driver init failed on I2C%d at 0x%02X", port, addr);
        }
    }
}

} // namespace

// Looked up by (port, address) - a CV widget's settings modal picks a specific physical unit,
// not just "whichever one is on this port". Returns nullptr for an out-of-range address or one
// that wasn't found/didn't init at boot.
inline ADS1115Driver* ads1115(int port, uint8_t address) {
    if (port < 0 || port > 1 || address < 0x48 || address > 0x4B) return nullptr;
    Ads1115Slot& slot = s_ads1115[port][address - 0x48];
    return (slot.present && slot.driver.is_ready()) ? &slot.driver : nullptr;
}
inline GP8413Driver* gp8413(int port, uint8_t address) {
    if (port < 0 || port > 1 || address < 0x58 || address > 0x5F) return nullptr;
    Gp8413Slot& slot = s_gp8413[port][address - 0x58];
    return (slot.present && slot.driver.is_ready()) ? &slot.driver : nullptr;
}

inline void scan_configured_devices() {
#if defined(CONFIG_APP_ADS1115_SEARCH_I2C0)
    scan_ads1115(0);
#endif
#if defined(CONFIG_APP_ADS1115_SEARCH_I2C1)
    scan_ads1115(1);
#endif
#if defined(CONFIG_APP_GP8413_SEARCH_I2C0)
    scan_gp8413(0);
#endif
#if defined(CONFIG_APP_GP8413_SEARCH_I2C1)
    scan_gp8413(1);
#endif
}

} // namespace Feature::IO

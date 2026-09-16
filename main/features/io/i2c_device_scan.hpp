#pragma once
#include "boards/board_init.hpp"
#include "features/io/i2c_device_registry.h"
#include "features/io/ads1115_driver.hpp"
#include "features/io/gp8413_driver.hpp"
#include "esp_log.h"

namespace Feature::IO {
namespace {

constexpr const char* SCAN_TAG = "I2CDeviceScan";

// Probes [addr_lo, addr_hi] on `bus`; returns the first ACKing address, or -1. Same
// zero-byte i2c_master_probe() approach as board_init.cpp's i2c_scan_bus(), restricted to
// one device's known address window.
inline int probe_address_range(i2c_master_bus_handle_t bus, uint8_t addr_lo, uint8_t addr_hi) {
    for (uint16_t addr = addr_lo; addr <= addr_hi; ++addr) {
        if (i2c_master_probe(bus, static_cast<uint8_t>(addr), 20) == ESP_OK) {
            return static_cast<int>(addr);
        }
    }
    return -1;
}

inline ADS1115Driver s_ads1115[2];
inline GP8413Driver s_gp8413[2];

inline void scan_ads1115(int port) {
    i2c_master_bus_handle_t bus = Board::get_i2c_bus_handle(port);
    if (bus == nullptr) {
        ESP_LOGW(SCAN_TAG, "ADS1115: I2C port %d not enabled - check I2C Subsystem Kconfig", port);
        return;
    }
    const int found = probe_address_range(bus, 0x48, 0x4B);
    if (found < 0) {
        ESP_LOGW(SCAN_TAG, "ADS1115: not found on I2C%d (0x48-0x4B)", port);
        return;
    }
    ESP_LOGI(SCAN_TAG, "ADS1115: found on I2C%d at 0x%02X", port, found);
    I2CDeviceRegistry::add("ADS1115", static_cast<uint8_t>(port), static_cast<uint8_t>(found));
    if (s_ads1115[port].init(bus, static_cast<uint8_t>(found)) != ESP_OK) {
        ESP_LOGW(SCAN_TAG, "ADS1115: found but driver init failed on I2C%d", port);
    }
}

inline void scan_gp8413(int port) {
    i2c_master_bus_handle_t bus = Board::get_i2c_bus_handle(port);
    if (bus == nullptr) {
        ESP_LOGW(SCAN_TAG, "GP8413: I2C port %d not enabled - check I2C Subsystem Kconfig", port);
        return;
    }
    const int found = probe_address_range(bus, 0x58, 0x5F);
    if (found < 0) {
        ESP_LOGW(SCAN_TAG, "GP8413: not found on I2C%d (0x58-0x5F)", port);
        return;
    }
    ESP_LOGI(SCAN_TAG, "GP8413: found on I2C%d at 0x%02X", port, found);
    I2CDeviceRegistry::add("GP8413", static_cast<uint8_t>(port), static_cast<uint8_t>(found));
    if (s_gp8413[port].init(bus, static_cast<uint8_t>(found)) != ESP_OK) {
        ESP_LOGW(SCAN_TAG, "GP8413: found but driver init failed on I2C%d", port);
    }
}

} // namespace

// Accessors for future use - not called anywhere yet.
inline ADS1115Driver* ads1115(int port) {
    return (port >= 0 && port < 2 && s_ads1115[port].is_ready()) ? &s_ads1115[port] : nullptr;
}
inline GP8413Driver* gp8413(int port) {
    return (port >= 0 && port < 2 && s_gp8413[port].is_ready()) ? &s_gp8413[port] : nullptr;
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

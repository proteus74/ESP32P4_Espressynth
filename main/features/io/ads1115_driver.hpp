#pragma once
#include "boards/board_init.hpp"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include <cstdint>

// Driver for DFRobot's DFR1184 ADS1115 0-10V module. Its onboard controller exposes a
// module-specific protocol instead of the ADS1115 register map: write channel 1/2 to 0x20,
// then read a big-endian 24-bit value in 0.01mV units from 0x31.
namespace Feature::IO {

class ADS1115Driver {
public:
    static constexpr uint32_t kChannelSettlingMs = 0;

    esp_err_t init(i2c_master_bus_handle_t bus, uint8_t address, uint32_t speed_hz) {
        i2c_device_config_t cfg = {};
        cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        cfg.device_address = address;
        cfg.scl_speed_hz = speed_hz;
        esp_err_t err = i2c_master_bus_add_device(bus, &cfg, &handle_);
        if (err != ESP_OK) return err;

        address_ = address;
        err = select_channel(0);
        float ignored = 0.0f;
        if (err == ESP_OK) err = read_selected(ignored);
        if (err != ESP_OK) {
            i2c_master_bus_rm_device(handle_);
            handle_ = nullptr;
            address_ = 0;
            return err;
        }
        return ESP_OK;
    }

    bool is_ready() const { return handle_ != nullptr; }
    uint8_t address() const { return address_; }
    i2c_master_dev_handle_t handle() const { return handle_; }

    // Selection and read are separate so the bus scheduler can serve other devices during
    // the module's settling time. Public channels are zero-based; wire values are 1 and 2.
    esp_err_t select_channel(uint8_t channel) {
        if (!is_ready() || channel > 1) return ESP_ERR_INVALID_STATE;
        const uint8_t select[2] = {0x20, static_cast<uint8_t>(channel + 1)};
        return i2c_master_transmit(handle_, select, sizeof(select), 100);
    }

    esp_err_t read_selected(float& outVolts) {
        if (!is_ready()) return ESP_ERR_INVALID_STATE;
        uint8_t reg = 0x31;
        uint8_t rx[3] = {};
        const esp_err_t err = i2c_master_transmit_receive(handle_, &reg, 1, rx, sizeof(rx), 100);
        if (err != ESP_OK) return err;
        const uint32_t raw = (static_cast<uint32_t>(rx[0]) << 16) |
                             (static_cast<uint32_t>(rx[1]) << 8) | rx[2];
        outVolts = static_cast<float>(raw) / 100000.0f;
        if (outVolts > 10.0f) outVolts = 10.0f;
        return ESP_OK;
    }

private:
    i2c_master_dev_handle_t handle_ = nullptr;
    uint8_t address_ = 0;
};

} // namespace Feature::IO

#pragma once
#include "boards/board_init.hpp"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include <cstdint>

// Driver for the GP8413 2-channel 15-bit I2C DAC (register layout per the official
// datasheet FN1601-79.2a and DFRobot's reference driver). VOUT0's data register is 0x02,
// VOUT1's is 0x04. The 15-bit value is left-aligned in the 16-bit little-endian payload.
// Register 0x01 selects the output range; 0x11 configures both channels for 0-10V.
namespace Feature::IO {

class GP8413Driver {
public:
    esp_err_t init(i2c_master_bus_handle_t bus, uint8_t address, uint32_t speed_hz) {
        i2c_device_config_t cfg = {};
        cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        cfg.device_address = address;
        cfg.scl_speed_hz = speed_hz;
        esp_err_t err = i2c_master_bus_add_device(bus, &cfg, &handle_);
        if (err != ESP_OK) return err;

        // GP8413 register reads are not part of the documented protocol. This checked
        // range-select write is both initialization and the device liveness check.
        const uint8_t range_10v[2] = {0x01, 0x11};
        err = i2c_master_transmit(handle_, range_10v, sizeof(range_10v), 100);
        if (err != ESP_OK) {
            i2c_master_bus_rm_device(handle_);
            handle_ = nullptr;
            return err;
        }
        address_ = address;
        return ESP_OK;
    }

    static uint16_t voltage_to_code(float volts) {
        if (volts < 0.0f) volts = 0.0f;
        if (volts > 10.0f) volts = 10.0f;
        return static_cast<uint16_t>(volts / 10.0f * 0x7FFF + 0.5f);
    }

    esp_err_t write_channel(uint8_t channel, float volts) {
        if (!is_ready() || channel > 1) return ESP_ERR_INVALID_STATE;
        const uint16_t payload = static_cast<uint16_t>(voltage_to_code(volts) << 1);
        const uint8_t reg = channel == 0 ? 0x02 : 0x04;
        const uint8_t tx[3] = {
            reg, static_cast<uint8_t>(payload & 0xFF), static_cast<uint8_t>(payload >> 8)};
        return i2c_master_transmit(handle_, tx, sizeof(tx), 100);
    }

    bool is_ready() const { return handle_ != nullptr; }
    uint8_t address() const { return address_; }
    i2c_master_dev_handle_t handle() const { return handle_; }

private:
    i2c_master_dev_handle_t handle_ = nullptr;
    uint8_t address_ = 0;
};

} // namespace Feature::IO

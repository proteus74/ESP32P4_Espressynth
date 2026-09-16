#pragma once
#include "driver/i2c_master.h"
#include "esp_err.h"
#include <cstdint>

// Driver for the GP8413 2-channel 15-bit I2C DAC (register layout per the official
// datasheet FN1601-79.2a, section 3.3.3/3.3.4). VOUT0's 15-bit code lives in register 0x02,
// VOUT1's in 0x04, each written as two bytes (low byte first, then high byte - a plain
// little-endian uint16_t split, top bit unused). VOUT = code/0x7FFF * full_scale.
// Assumes the board is wired/configured for the 0-10V range (matching this project's own
// 0-10V CV convention, see CvConventions.h) - the datasheet pages fetched here didn't
// surface a separate runtime range-select register, so 0-5V vs 0-10V may be a
// hardware/factory option rather than software-selectable; reconfirm against real hardware
// if output level looks halved.
namespace Feature::IO {

class GP8413Driver {
public:
    // Liveness check: read back VOUT0's own data register (0x02) - any 2-byte response
    // confirms the device ACKs register reads at this address.
    esp_err_t init(i2c_master_bus_handle_t bus, uint8_t address) {
        i2c_device_config_t cfg = {};
        cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        cfg.device_address = address;
        cfg.scl_speed_hz = 50000; // Board::I2C_BUS_FREQ_HZ
        esp_err_t err = i2c_master_bus_add_device(bus, &cfg, &handle_);
        if (err != ESP_OK) return err;

        uint8_t reg = 0x02;
        uint8_t rx[2] = {};
        err = i2c_master_transmit_receive(handle_, &reg, 1, rx, sizeof(rx), 100);
        if (err != ESP_OK) {
            i2c_master_bus_rm_device(handle_);
            handle_ = nullptr;
            return err;
        }
        address_ = address;
        return ESP_OK;
    }

    // Writes `volts` (clamped 0-10V) to `channel` (0 -> VOUT0/reg 0x02, 1 -> VOUT1/reg
    // 0x04). Caller must be a task that can afford to block briefly - never the audio
    // render task (see i2c_bus_manager.hpp).
    esp_err_t write_channel(uint8_t channel, float volts) {
        if (!is_ready() || channel > 1) return ESP_ERR_INVALID_STATE;
        if (volts < 0.0f) volts = 0.0f;
        if (volts > 10.0f) volts = 10.0f;
        const uint16_t code = static_cast<uint16_t>(volts / 10.0f * 0x7FFF + 0.5f);
        const uint8_t reg = channel == 0 ? 0x02 : 0x04;
        uint8_t tx[3] = {reg, static_cast<uint8_t>(code & 0xFF), static_cast<uint8_t>(code >> 8)};
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

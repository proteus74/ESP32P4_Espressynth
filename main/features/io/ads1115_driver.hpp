#pragma once
#include "driver/i2c_master.h"
#include "esp_err.h"
#include <cstdint>

// Minimal skeleton for the TI ADS1115 2-channel ADC. Not wired into the synth engine -
// this only proves the chip is present and its register interface is readable, so a
// future feature can build on top of it.
namespace Feature::IO {

class ADS1115Driver {
public:
    // Attaches an I2C device at `address` on `bus`, then reads back the 2-byte Config
    // register (0x01) as a minimal liveness check. The ADS1115 has no WHO_AM_I register,
    // so this only confirms *something* ACKs register reads at this address - the caller
    // already knows it's an ADS1115 from the address-range scan that found it.
    esp_err_t init(i2c_master_bus_handle_t bus, uint8_t address) {
        i2c_device_config_t cfg = {};
        cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        cfg.device_address = address;
        cfg.scl_speed_hz = 50000; // Board::I2C_BUS_FREQ_HZ
        esp_err_t err = i2c_master_bus_add_device(bus, &cfg, &handle_);
        if (err != ESP_OK) return err;

        uint8_t reg = 0x01; // Config register
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

    bool is_ready() const { return handle_ != nullptr; }
    uint8_t address() const { return address_; }
    i2c_master_dev_handle_t handle() const { return handle_; }

private:
    i2c_master_dev_handle_t handle_ = nullptr;
    uint8_t address_ = 0;
};

} // namespace Feature::IO

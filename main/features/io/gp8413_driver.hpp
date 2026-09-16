#pragma once
#include "driver/i2c_master.h"
#include "esp_err.h"
#include <cstdint>

// Minimal skeleton for the DFRobot GP8413 2-channel 15-bit DAC. Not wired into the synth
// engine - this only proves the chip is present, so a future feature can build on top of
// it. The exact register map (DAC data registers, output-range select) has NOT been
// verified against a datasheet here - confirm against real hardware/datasheet before
// relying on this for anything beyond "device ACKs".
namespace Feature::IO {

class GP8413Driver {
public:
    esp_err_t init(i2c_master_bus_handle_t bus, uint8_t address) {
        i2c_device_config_t cfg = {};
        cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        cfg.device_address = address;
        cfg.scl_speed_hz = 50000; // Board::I2C_BUS_FREQ_HZ
        esp_err_t err = i2c_master_bus_add_device(bus, &cfg, &handle_);
        if (err != ESP_OK) return err;

        // TODO: verify against DFRobot GP8413 datasheet. Placeholder liveness check:
        // read back one byte from register 0x02 (output-range select register per
        // DFRobot's Arduino reference code) to confirm the device ACKs register reads.
        uint8_t reg = 0x02;
        uint8_t rx = 0;
        err = i2c_master_transmit_receive(handle_, &reg, 1, &rx, sizeof(rx), 100);
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

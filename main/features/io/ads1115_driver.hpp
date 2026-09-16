#pragma once
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstdint>

// Driver for the TI ADS1115 2-channel ADC (register layout per TI SBAS444, cross-checked
// against the well-documented default config value 0x8583). Used by the Core-0 I2C bus
// manager (features/io/i2c_bus_manager.hpp) to service "CV In" widgets.
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

    // Starts a single-shot conversion on `channel` (0 -> AIN0, 1 -> AIN1, single-ended vs
    // GND) and blocks (~2ms: conversion time at 860SPS plus margin) until the result is
    // ready, then returns it in outVolts. Full-scale range is fixed at +/-4.096V (PGA=001).
    // Caller must be a task that can afford to block a couple of ms - never the audio
    // render task (see i2c_bus_manager.hpp, which calls this from its own Core-0 task).
    esp_err_t read_channel(uint8_t channel, float& outVolts) {
        if (!is_ready() || channel > 1) return ESP_ERR_INVALID_STATE;
        // OS=1 (start) | MUX=100/101 (AIN0/AIN1 vs GND) | PGA=001 (+/-4.096V) |
        // MODE=1 (single-shot) | DR=111 (860SPS) | COMP_QUE=11 (comparator disabled).
        const uint16_t mux = 0x4000u | (static_cast<uint16_t>(channel) << 12);
        const uint16_t config = 0x8000u | mux | 0x0200u | 0x0100u | 0x00E0u | 0x0003u;
        uint8_t tx[3] = {0x01, static_cast<uint8_t>(config >> 8), static_cast<uint8_t>(config & 0xFF)};
        esp_err_t err = i2c_master_transmit(handle_, tx, sizeof(tx), 100);
        if (err != ESP_OK) return err;
        vTaskDelay(pdMS_TO_TICKS(2));
        uint8_t reg = 0x00; // Conversion register
        uint8_t rx[2] = {};
        err = i2c_master_transmit_receive(handle_, &reg, 1, rx, sizeof(rx), 100);
        if (err != ESP_OK) return err;
        const int16_t raw = static_cast<int16_t>((rx[0] << 8) | rx[1]);
        outVolts = (static_cast<float>(raw) / 32768.0f) * 4.096f;
        return ESP_OK;
    }

private:
    i2c_master_dev_handle_t handle_ = nullptr;
    uint8_t address_ = 0;
};

} // namespace Feature::IO

#pragma once
#include "esp_err.h"
#include "driver/i2c_master.h"
#include <cstdint>

namespace Board {

// Shared by every I2C bus this project creates (see "I2C Subsystem" Kconfig). Only pins are
// user-configurable; the user asked for enable/disable + pins, not per-port frequency.
// Temporarily dropped from 400 kHz to 50 kHz (well under even Standard-mode's 100 kHz) while
// chasing an intermittent WM8960 dropout that turned out to correlate with touching the
// touchscreen (ground-bounce/noise coupling in from the touch panel, not a timing issue) --
// see main/features/audio/wm8960_manager.hpp's I2C_FREQ_HZ for the other half of this test.
constexpr uint32_t I2C_BUS_FREQ_HZ = 50000;

/**
 * @brief Initialize all hardware peripherals for the selected board
 *        (I2C master bus(es), power rails, codec enable pins, etc.)
 */
esp_err_t init();

/**
 * @brief Get the I2C master bus handle for the given port (0 or 1), set up in Board::init()
 *        if "I2C Subsystem > Enable I2C Port N" is on. Returns nullptr for a disabled or
 *        out-of-range port.
 */
i2c_master_bus_handle_t get_i2c_bus_handle(int port = 0);

} // namespace Board

#pragma once
#include "sdkconfig.h"
#include "esp_err.h"
#include "driver/i2c_master.h"
#include <cstdint>

namespace Board {

constexpr uint32_t i2c_bus_frequency_hz(int port) {
#if defined(CONFIG_APP_ENABLE_I2C)
    return port == 1 ? CONFIG_APP_I2C1_SPEED_HZ : CONFIG_APP_I2C0_SPEED_HZ;
#else
    (void)port;
    return 50000;
#endif
}

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

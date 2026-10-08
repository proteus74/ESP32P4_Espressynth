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

#if defined(CONFIG_BOARD_CROWPANEL_5INCH)
/** @brief Write one byte to an STC8H1K co-MCU register (see crowpanel_5inch.hpp for the map). */
esp_err_t stc8_write_reg(uint8_t reg, uint8_t value);
/** @brief Read len bytes starting at an STC8H1K co-MCU register. */
esp_err_t stc8_read_reg(uint8_t reg, uint8_t* data, size_t len);
/** @brief Set one STC8 GPIO output (STC8_GPIO_OUT_*). */
esp_err_t stc8_set_gpio(uint8_t gpio, bool level);
/** @brief LCD backlight 0-100 %: switches the STC8 backlight rail and sets its PWM duty. */
esp_err_t set_backlight_percent(int percent);
#endif

} // namespace Board

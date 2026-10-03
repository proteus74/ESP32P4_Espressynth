#pragma once
#include "boards/board_config.hpp"
#include "driver/gpio.h"
#include <cstddef>

namespace Board {

// Compile-time check used to catch a user-configurable pin (I2C0/I2C1, WM8960 I2S/button,
// ...) colliding with a pin already reserved elsewhere. GPIO_NUM_NC never conflicts.
template <size_t N>
constexpr bool pin_in_list(gpio_num_t pin, const gpio_num_t (&list)[N]) {
    if (pin == GPIO_NUM_NC) return false;
    for (gpio_num_t p : list) {
        if (p == pin) return true;
    }
    return false;
}

// Canonical "is this GPIO already claimed by the current board or a user-configured
// shared-bus pin" check - the single source of truth for both compile-time static_asserts
// (board_init.cpp, wm8960_manager.hpp) and the runtime GPIO-settings free-pin query
// (features/io/gpio_manager.hpp). Deliberately excludes feature-specific pins that need a
// self-referential check against this same set (e.g. WM8960's own pins) - those layer this
// as a base check instead of folding themselves in.
constexpr bool is_pin_reserved(gpio_num_t pin) {
    if (pin == GPIO_NUM_NC) return false;
    if (pin_in_list(pin, CurrentBoard::RESERVED_PINS)) return true;
#if defined(CONFIG_APP_DISPLAY_OUTPUT_SPI) && CONFIG_APP_DISPLAY_SPI_PIN_CLK >= 0
    const int display_pins[] = {CONFIG_APP_DISPLAY_SPI_PIN_CLK, CONFIG_APP_DISPLAY_SPI_PIN_MISO,
                               CONFIG_APP_DISPLAY_SPI_PIN_MOSI, CONFIG_APP_DISPLAY_SPI_PIN_DC,
                               CONFIG_APP_DISPLAY_SPI_PIN_CS, CONFIG_APP_DISPLAY_SPI_PIN_RESET,
                               CONFIG_APP_DISPLAY_SPI_PIN_BACKLIGHT};
    for (int display_pin : display_pins) {
        if (pin == display_pin) return true;
    }
#endif
#if defined(CONFIG_APP_ENABLE_USB_DEVICE)
    if (pin == GPIO_NUM_26 || pin == GPIO_NUM_27) return true;
#endif
#if defined(CONFIG_APP_ENABLE_I2C0)
    if (pin == static_cast<gpio_num_t>(CONFIG_I2C0_PIN_SDA) ||
        pin == static_cast<gpio_num_t>(CONFIG_I2C0_PIN_SCL)) return true;
#endif
#if defined(CONFIG_APP_ENABLE_I2C1)
    if (pin == static_cast<gpio_num_t>(CONFIG_I2C1_PIN_SDA) ||
        pin == static_cast<gpio_num_t>(CONFIG_I2C1_PIN_SCL)) return true;
#endif
    return false;
}

} // namespace Board

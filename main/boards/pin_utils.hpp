#pragma once
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

} // namespace Board

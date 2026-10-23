#pragma once
#include "driver/gpio.h"

namespace Board {

// One entry of a board's RESERVED_PINS list: the GPIO plus the name of the constant that
// claims it, so compile-time conflict errors can say *which* board pin is in the way.
struct ReservedPin {
    gpio_num_t pin;
    const char* name;
};

} // namespace Board

// Use inside RESERVED_PINS: BOARD_PIN(PIN_AUDIO_PA) -> {PIN_AUDIO_PA, "PIN_AUDIO_PA"}.
#define BOARD_PIN(x) ::Board::ReservedPin{x, #x}

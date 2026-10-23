#pragma once
#include "boards/board_config.hpp"
#include "boards/reserved_pin.hpp"
#include "driver/gpio.h"
#include <cstddef>

namespace Board {

// Compile-time check used to catch a user-configurable pin (I2C0/I2C1, WM8960 I2S/button,
// ...) colliding with a pin already reserved elsewhere. GPIO_NUM_NC never conflicts.
template <size_t N>
constexpr bool pin_in_list(gpio_num_t pin, const ReservedPin (&list)[N]) {
    if (pin == GPIO_NUM_NC) return false;
    for (const ReservedPin& entry : list) {
        if (entry.pin == pin) return true;
    }
    return false;
}

// Name of the board constant (e.g. "PIN_AUDIO_PA") that claims `pin`, or nullptr.
template <size_t N>
constexpr const char* reserved_pin_name(gpio_num_t pin, const ReservedPin (&list)[N]) {
    if (pin == GPIO_NUM_NC) return nullptr;
    for (const ReservedPin& entry : list) {
        if (entry.pin == pin) return entry.name;
    }
    return nullptr;
}

// Who owns a pin (see BOARD_ASSERT_PIN_FREE below).
enum class PinOwner { None, BoardReserved, DisplaySpi, UsbDevice, I2c0, I2c1 };

constexpr PinOwner pin_owner(gpio_num_t pin) {
    if (pin == GPIO_NUM_NC) return PinOwner::None;
    if (pin_in_list(pin, CurrentBoard::RESERVED_PINS)) return PinOwner::BoardReserved;
#if defined(CONFIG_APP_DISPLAY_OUTPUT_SPI) && CONFIG_APP_DISPLAY_SPI_PIN_CLK >= 0
    const int display_pins[] = {CONFIG_APP_DISPLAY_SPI_PIN_CLK, CONFIG_APP_DISPLAY_SPI_PIN_MISO,
                               CONFIG_APP_DISPLAY_SPI_PIN_MOSI, CONFIG_APP_DISPLAY_SPI_PIN_DC,
                               CONFIG_APP_DISPLAY_SPI_PIN_CS, CONFIG_APP_DISPLAY_SPI_PIN_RESET,
                               CONFIG_APP_DISPLAY_SPI_PIN_BACKLIGHT};
    for (int display_pin : display_pins) {
        if (pin == display_pin) return PinOwner::DisplaySpi;
    }
#endif
#if defined(CONFIG_APP_ENABLE_USB_DEVICE)
    if (pin == GPIO_NUM_26 || pin == GPIO_NUM_27) return PinOwner::UsbDevice;
#endif
#if defined(CONFIG_APP_ENABLE_I2C0)
    if (pin == static_cast<gpio_num_t>(CONFIG_I2C0_PIN_SDA) ||
        pin == static_cast<gpio_num_t>(CONFIG_I2C0_PIN_SCL)) return PinOwner::I2c0;
#endif
#if defined(CONFIG_APP_ENABLE_I2C1)
    if (pin == static_cast<gpio_num_t>(CONFIG_I2C1_PIN_SDA) ||
        pin == static_cast<gpio_num_t>(CONFIG_I2C1_PIN_SCL)) return PinOwner::I2c1;
#endif
    return PinOwner::None;
}

// Owner considering only the board's own RESERVED_PINS list (for features whose pins may
// legitimately share with the I2C/USB/display claims that pin_owner() also checks).
constexpr PinOwner board_pin_owner(gpio_num_t pin) {
    return pin_in_list(pin, CurrentBoard::RESERVED_PINS) ? PinOwner::BoardReserved : PinOwner::None;
}

// Canonical "is this GPIO already claimed by the current board or a user-configured
// shared-bus pin" check - the single source of truth for both compile-time static_asserts
// (board_init.cpp, wm8960_manager.hpp) and the runtime GPIO-settings free-pin query
// (features/io/gpio_manager.hpp). Deliberately excludes feature-specific pins that need a
// self-referential check against this same set (e.g. WM8960's own pins) - those layer this
// as a base check instead of folding themselves in.
constexpr bool is_pin_reserved(gpio_num_t pin) {
    return pin_owner(pin) != PinOwner::None;
}

constexpr const char* pin_owner_text(PinOwner owner) {
    switch (owner) {
        case PinOwner::BoardReserved: return "the board pin ";
        case PinOwner::DisplaySpi: return "the SPI display (Kconfig APP_DISPLAY_SPI_PIN_*)";
        case PinOwner::UsbDevice:  return "the USB device (GPIO26/27; disable APP_ENABLE_USB_DEVICE or use another pin)";
        case PinOwner::I2c0:       return "I2C0 (Kconfig I2C0_PIN_SDA / I2C0_PIN_SCL)";
        case PinOwner::I2c1:       return "I2C1 (Kconfig I2C1_PIN_SDA / I2C1_PIN_SCL)";
        default:                   return "nothing";
    }
}

// Fixed-size string usable as a C++26 static_assert message (needs data() and size()).
struct PinMessage {
    char text[256] = {};
    size_t length = 0;
    constexpr const char* data() const { return text; }
    constexpr size_t size() const { return length; }
    constexpr void add(const char* s) {
        while (*s && length < sizeof(text) - 1) text[length++] = *s++;
    }
    constexpr void add(int v) {
        if (v < 0) { add("-"); v = -v; }
        char digits[12] = {};
        int n = 0;
        do { digits[n++] = static_cast<char>('0' + v % 10); v /= 10; } while (v != 0);
        while (n > 0) {
            const char c = digits[--n];
            if (length < sizeof(text) - 1) text[length++] = c;
        }
    }
};

constexpr PinMessage pin_conflict_message(const char* option, int pin, PinOwner owner) {
    PinMessage m;
    m.add("PIN CONFLICT: ");
    m.add(option);
    m.add(" = GPIO");
    m.add(pin);
    m.add(" is already used by ");
    m.add(pin_owner_text(owner));
    if (owner == PinOwner::BoardReserved) {
        const char* name = reserved_pin_name(static_cast<gpio_num_t>(pin), CurrentBoard::RESERVED_PINS);
        m.add(name != nullptr ? name : "?");
        m.add(" (RESERVED_PINS in main/boards/<board>.hpp)");
    }
    return m;
}

} // namespace Board

// Compile-time "pin must be free" check. On failure the compiler prints one line, e.g.
//   static assertion failed: PIN CONFLICT: CONFIG_MY_PIN = GPIO53 is already used by the board pin PIN_AUDIO_PA (...)
// BOARD_ASSERT_PIN_FREE checks every claim (board, display, USB, I2C);
// BOARD_ASSERT_PIN_NOT_BOARD checks only the board's RESERVED_PINS list.
#define BOARD_ASSERT_PIN_FREE(pin) \
    static_assert(Board::pin_owner(static_cast<gpio_num_t>(pin)) == Board::PinOwner::None, \
                  Board::pin_conflict_message(#pin, static_cast<int>(pin), \
                                              Board::pin_owner(static_cast<gpio_num_t>(pin))))

#define BOARD_ASSERT_PIN_NOT_BOARD(pin) \
    static_assert(Board::board_pin_owner(static_cast<gpio_num_t>(pin)) == Board::PinOwner::None, \
                  Board::pin_conflict_message(#pin, static_cast<int>(pin), \
                                              Board::board_pin_owner(static_cast<gpio_num_t>(pin))))

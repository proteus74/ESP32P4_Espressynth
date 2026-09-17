#pragma once
#include "audio/engine/GpioSettings.h"
#include "boards/pin_utils.hpp"
#include "driver/gpio.h"

// Applies the user-configured GPIO pin table (GpioSettings.h) via gpio_config(), and gives
// DigitalInNode.h/DigitalOutNode.h direct render()-time access to the pins. Unlike
// i2c_bus_manager.hpp, there is no task/mailbox here: gpio_get_level()/gpio_set_level() are
// register reads/writes (~100ns), not bus transactions, so nothing is gained by amortizing
// them across a scheduler task - render() just calls them directly, once per block.
namespace Feature::IO {

// Whether `pin` is available to be picked in the GPIO Settings dialog - the generic
// board/bus reservation (Board::is_pin_reserved) plus the WM8960 codec's pins, which are
// user-configured via Kconfig but (deliberately) excluded from Board::is_pin_reserved()
// since that function also backs WM8960's own self-referential collision static_asserts.
inline bool is_pin_reserved_for_gpio_settings(gpio_num_t pin) {
    if (Board::is_pin_reserved(pin)) return true;
#if defined(CONFIG_ONBOARD_AUDIO_WM8960)
    if (pin == static_cast<gpio_num_t>(CONFIG_ONBOARD_AUDIO_WM8960_PIN_BCLK) ||
        pin == static_cast<gpio_num_t>(CONFIG_ONBOARD_AUDIO_WM8960_PIN_LRCK) ||
        pin == static_cast<gpio_num_t>(CONFIG_ONBOARD_AUDIO_WM8960_PIN_DIN) ||
        pin == static_cast<gpio_num_t>(CONFIG_ONBOARD_AUDIO_WM8960_PIN_DOUT) ||
        pin == static_cast<gpio_num_t>(CONFIG_ONBOARD_AUDIO_WM8960_PIN_BUTTON)) return true;
#endif
    return false;
}

// Applies gpio_config() for every entry in gpioSettings() - call once at boot (after
// loadGpioSettingsFromFs()) and again whenever settings are saved from the browser.
inline void apply_gpio_settings() {
    const auto &gs = gpioSettings();
    for (uint8_t i = 0; i < gs.count; ++i) {
        const GpioEntry &e = gs.entries[i];
        if (e.pin < 0 || !GPIO_IS_VALID_GPIO(static_cast<gpio_num_t>(e.pin))) continue;
        gpio_config_t cfg = {};
        cfg.pin_bit_mask = 1ULL << e.pin;
        cfg.mode = e.direction == GpioDirection::Output ? GPIO_MODE_OUTPUT : GPIO_MODE_INPUT;
        cfg.pull_up_en = (e.direction == GpioDirection::Input && e.pull == GpioPull::Up)
                             ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE;
        cfg.pull_down_en = (e.direction == GpioDirection::Input && e.pull == GpioPull::Down)
                             ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE;
        cfg.intr_type = GPIO_INTR_DISABLE; // no edge-triggered use here - see file comment
        gpio_config(&cfg);
    }
}

// Render-task-side accessors for DigitalInNode.h/DigitalOutNode.h. Deliberately not cached
// anywhere - see file comment.
inline bool gpio_read_level(int8_t pin) {
    if (pin < 0 || !GPIO_IS_VALID_GPIO(static_cast<gpio_num_t>(pin))) return false;
    return gpio_get_level(static_cast<gpio_num_t>(pin)) != 0;
}
inline void gpio_write_level(int8_t pin, bool level) {
    if (pin < 0 || !GPIO_IS_VALID_GPIO(static_cast<gpio_num_t>(pin))) return;
    gpio_set_level(static_cast<gpio_num_t>(pin), level ? 1 : 0);
}

} // namespace Feature::IO

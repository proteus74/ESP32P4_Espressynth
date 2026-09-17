#pragma once

#include <cstddef>

#include "../CvConventions.h"
#include "../NodeInstance.h"
#include "../PresetParser.h"
#include "features/io/gpio_manager.hpp"

// "Digital In" (External Hardware widget) - a live GPIO pin (Feature::IO::GpioSettings)
// exposed as a 0/10V audio-rate output ("value") any other widget can patch into, exactly
// like a Knob/LFO output. Unlike CV In, there's no bus-manager task/mailbox - gpio_get_level()
// is a direct register read, cheap enough to call straight from render() every block (see
// features/io/gpio_manager.hpp's file comment). No debounce/edge-detection here - block-rate
// granularity (~2.7ms) is treated as "debounced enough" for this widget; a fast/clean
// edge-triggered use case is the separate Button-GPIO-trigger feature.
namespace nodes::digitalin
{

inline void render(NodeInstance &inst, size_t frames)
{
    auto &s = inst.digitalIn;
    // Re-checked every block (cheap: linear scan over at most kMaxGpioEntries), not cached
    // from build() - unlike CV's ioDeviceFound (which reflects a live I2C bus poll result and
    // has nothing to re-derive), a Digital In's "configured" state is a pure function of
    // GpioSettings, which the user can change at any time from the Settings dialog without a
    // graph rebuild (see gpio_manager.hpp's "@gpioSettings" ws branch) - if this stayed
    // build()-only, removing the pin from Settings would leave this widget silently reporting
    // stale readings/"found" status until the next preset reload.
    const Feature::IO::GpioEntry *entry = Feature::IO::findGpioEntry(s.pin);
    inst.ioDeviceFound = entry != nullptr && entry->direction == Feature::IO::GpioDirection::Input;
    s.invert = entry != nullptr && entry->invert;

    bool level = Feature::IO::gpio_read_level(s.pin);
    if (s.invert) level = !level;
    s.lastLevel = level;
    const float value = level ? cv::kMaxVolts : 0.0f;
    for (size_t i = 0; i < frames; ++i)
    {
        inst.outL[i] = value;
    }
}

inline void build(NodeInstance &inst, const WidgetParams &p)
{
    auto &s = inst.digitalIn;
    s.pin = p.gpioPin;
    s.lastLevel = false;
    s.invert = false; // refreshed on the first render() call
    inst.ioDeviceFound = true; // optimistic until the first render() call
}

inline void parsePreset(cJSON *p, WidgetParams &out)
{
    out.gpioPin = static_cast<int8_t>(presetparser::getInt(p, "pin", -1));
}

} // namespace nodes::digitalin

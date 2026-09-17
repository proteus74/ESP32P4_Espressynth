#pragma once

#include <cstddef>

#include "../CvConventions.h"
#include "../NodeInstance.h"
#include "../PresetParser.h"
#include "features/io/gpio_manager.hpp"

// "Digital Out" (External Hardware widget) - forwards whatever's patched into its "in" port
// to a live GPIO pin (Feature::IO::GpioSettings) as a HIGH/LOW level. Unlike CV Out, there's
// no bus-manager task/mailbox - gpio_set_level() is a direct register write, cheap enough to
// call straight from render() every block (see features/io/gpio_manager.hpp's file comment).
namespace nodes::digitalout
{

inline void render(NodeInstance &inst, size_t frames)
{
    if (frames == 0) return;
    auto &s = inst.digitalOut;
    // Re-checked every block, not cached from build() - see DigitalInNode.h's render() comment
    // for why (GpioSettings can change live, without a graph rebuild).
    const Feature::IO::GpioEntry *entry = Feature::IO::findGpioEntry(s.pin);
    inst.ioDeviceFound = entry != nullptr && entry->direction == Feature::IO::GpioDirection::Output;
    s.invert = entry != nullptr && entry->invert;

    // Last sample of the block is this block's "settled" target, same reasoning as CvOutNode.
    // Written unconditionally even if no longer "found" (same as CvOutNode's post_cv_out_target)
    // - ioDeviceFound is a reporting flag, not a write gate.
    const float volts = noderender::portSample(inst, 0, frames - 1);
    bool level = volts >= cv::kMaxVolts * 0.5f;
    if (s.invert) level = !level;
    Feature::IO::gpio_write_level(s.pin, level);
}

inline void build(NodeInstance &inst, const WidgetParams &p)
{
    auto &s = inst.digitalOut;
    s.pin = p.gpioPin;
    s.invert = false; // refreshed on the first render() call
    inst.ioDeviceFound = true; // optimistic until the first render() call
}

inline void parsePreset(cJSON *p, WidgetParams &out)
{
    out.gpioPin = static_cast<int8_t>(presetparser::getInt(p, "pin", -1));
}

} // namespace nodes::digitalout

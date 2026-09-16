#pragma once

#include <cstddef>

#include "../NodeInstance.h"
#include "../PresetParser.h"
#include "features/io/i2c_bus_manager.hpp"

// "CV In" (External Hardware widget) - a live external ADC channel (ADS1115) exposed as a
// 0-10V audio-rate output ("value") any other widget can patch into, exactly like a
// Knob/LFO output. The actual I2C read happens on a separate Core-0 task (see
// features/io/i2c_bus_manager.hpp) - render() here only drains that task's latest sample
// from a non-blocking mailbox, once per block, mirroring how SynthEngine.h drains its MIDI
// event queue before renderBlock() runs.
namespace nodes::cvin
{

inline void render(NodeInstance &inst, size_t frames)
{
    auto &s = inst.cvIn;
    Feature::IO::CvInSample sample;
    if (Feature::IO::read_cv_in_sample(s.slotIndex, sample))
    {
        // A single dropped/NACK'd transaction (sample.found == false) keeps the last-good
        // reading instead of snapping the output to 0 - the missing-device badge (driven by
        // inst.ioDeviceFound) is the visible signal for that case, not a glitch in the value.
        if (sample.found)
        {
            s.lastVolts = sample.volts;
        }
        inst.ioDeviceFound = sample.found;
    }
    for (size_t i = 0; i < frames; ++i)
    {
        inst.outL[i] = s.lastVolts;
    }
}

inline void build(NodeInstance &inst, const WidgetParams &p)
{
    auto &s = inst.cvIn;
    s.port = p.cvIoPort;
    s.address = p.cvIoAddress;
    s.channel = p.cvIoChannel;
    s.slotIndex = Feature::IO::resolve_cv_in_slot(p.cvIoPort, p.cvIoAddress, p.cvIoChannel);
    s.lastVolts = 0.0f;
    inst.ioDeviceFound = true; // optimistic until the bus manager's first sample arrives
}

inline void parsePreset(cJSON *p, WidgetParams &out)
{
    out.cvIoPort = static_cast<int8_t>(presetparser::getInt(p, "port", -1));
    out.cvIoAddress = static_cast<uint8_t>(presetparser::getInt(p, "address", 0));
    out.cvIoChannel = static_cast<uint8_t>(presetparser::getInt(p, "channel", 0));
}

} // namespace nodes::cvin

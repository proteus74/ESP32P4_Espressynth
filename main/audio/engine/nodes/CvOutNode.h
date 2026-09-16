#pragma once

#include <cstddef>

#include "../CvConventions.h"
#include "../NodeInstance.h"
#include "../PresetParser.h"
#include "features/io/i2c_bus_manager.hpp"

// "CV Out" (External Hardware widget) - forwards whatever's patched into its "in" port to a
// live external DAC channel (GP8413). The actual I2C write happens on a separate Core-0 task
// (see features/io/i2c_bus_manager.hpp) - render() here only posts this block's settled
// value into a non-blocking, depth-1 "mailbox" (xQueueOverwrite), since the bus can't keep
// up with even one write per audio block anyway (see i2c_bus_manager.hpp's file comment) -
// only the latest target ever matters.
namespace nodes::cvout
{

inline void render(NodeInstance &inst, size_t frames)
{
    if (frames == 0) return;
    auto &s = inst.cvOut;
    // Last sample of the block is this block's "settled" target.
    float volts = noderender::portSample(inst, 0, frames - 1);
    if (volts < 0.0f) volts = 0.0f;
    if (volts > cv::kMaxVolts) volts = cv::kMaxVolts; // defensive clamp before it reaches hardware
    Feature::IO::post_cv_out_target(s.slotIndex, volts);
    inst.ioDeviceFound = Feature::IO::cv_out_found(s.slotIndex);
}

inline void build(NodeInstance &inst, const WidgetParams &p)
{
    auto &s = inst.cvOut;
    s.port = p.cvIoPort;
    s.address = p.cvIoAddress;
    s.channel = p.cvIoChannel;
    s.slotIndex = Feature::IO::resolve_cv_out_slot(p.cvIoPort, p.cvIoAddress, p.cvIoChannel);
    inst.ioDeviceFound = true; // optimistic until the bus manager's first write attempt
}

inline void parsePreset(cJSON *p, WidgetParams &out)
{
    out.cvIoPort = static_cast<int8_t>(presetparser::getInt(p, "port", -1));
    out.cvIoAddress = static_cast<uint8_t>(presetparser::getInt(p, "address", 0));
    out.cvIoChannel = static_cast<uint8_t>(presetparser::getInt(p, "channel", 0));
}

} // namespace nodes::cvout

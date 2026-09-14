#pragma once

#include <cstddef>
#include <cstring>

#include "../CvConventions.h"
#include "../NodeInstance.h"
#include "../PresetParser.h"

namespace nodes::samplehold
{

inline float clampThreshold(float value)
{
    return value < 0.0f ? 0.0f : (value > cv::kMaxVolts ? cv::kMaxVolts : value);
}

inline void render(NodeInstance &inst, size_t frames)
{
    using namespace noderender;
    auto &s = inst.sampleHold;
    const bool thresholdFed = controlPortConnected(inst, 2) ||
                              (inst.audioConnectedPortMask & synth::portBit(2)) != 0;

    for (size_t i = 0; i < frames; ++i)
    {
        const float threshold = thresholdFed ? portSample(inst, 2, i) : s.threshold;
        const bool triggerAbove = portSample(inst, 1, i) >= threshold;
        if (triggerAbove && !s.triggerWasAbove)
            s.heldValue = portSample(inst, 0, i);
        inst.outL[i] = s.heldValue;
        s.triggerWasAbove = triggerAbove;
    }
}

inline void build(NodeInstance &inst, const WidgetParams &p)
{
    inst.sampleHold.threshold = clampThreshold(p.sampleHoldThreshold);
    inst.sampleHold.heldValue = 0.0f;
    inst.sampleHold.triggerWasAbove = false;
}

inline bool setParam(NodeInstance &inst, const char *name, float value)
{
    if (name == nullptr || strcmp(name, "threshold") != 0)
        return false;
    inst.sampleHold.threshold = clampThreshold(value);
    return true;
}

inline void parsePreset(cJSON *p, WidgetParams &out)
{
    out.sampleHoldThreshold = clampThreshold(presetparser::getFloat(p, "threshold", 5.0f));
}

} // namespace nodes::samplehold

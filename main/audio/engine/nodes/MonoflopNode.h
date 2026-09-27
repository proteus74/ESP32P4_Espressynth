#pragma once

#include <cmath>
#include <cstring>
#include "../CvConventions.h"
#include "../NodeInstance.h"
#include "../PresetParser.h"
#include "../SynthConfig.h"

namespace nodes::monoflop
{
inline float clampThreshold(float value)
{
    return std::isfinite(value) ? fminf(cv::kMaxVolts, fmaxf(0.0f, value)) : 5.0f;
}

inline float clampPulseMs(float value)
{
    return std::isfinite(value) ? fminf(60000.0f, fmaxf(1.0f, value)) : 10.0f;
}

inline void render(NodeInstance &inst, size_t frames)
{
    using namespace noderender;
    auto &s = inst.monoflop;
    const bool thresholdFed = controlPortConnected(inst, 1) ||
                              (inst.audioConnectedPortMask & synth::portBit(1)) != 0;
    for (size_t i = 0; i < frames; ++i)
    {
        const float threshold = thresholdFed ? clampThreshold(portSample(inst, 1, i)) : s.threshold;
        const bool above = portSample(inst, 0, i) > threshold;
        // Ignore crossings during an active pulse; held-high input never repeats.
        if (above && !s.wasAbove && s.samplesLeft == 0)
            s.samplesLeft = static_cast<uint32_t>(ceilf(s.pulseMs * (synth::kSampleRate / 1000.0f)));
        inst.outL[i] = s.samplesLeft > 0 ? cv::kGateOnVolts : cv::kGateOffVolts;
        if (s.samplesLeft > 0) --s.samplesLeft;
        s.wasAbove = above;
    }
}

inline void build(NodeInstance &inst, const WidgetParams &p)
{
    inst.monoflop.threshold = clampThreshold(p.monoflopThreshold);
    inst.monoflop.pulseMs = clampPulseMs(p.monoflopPulseMs);
}

inline bool setParam(NodeInstance &inst, const char *name, float value)
{
    if (name == nullptr) return false;
    if (strcmp(name, "threshold") == 0) inst.monoflop.threshold = clampThreshold(value);
    else if (strcmp(name, "pulseMs") == 0) inst.monoflop.pulseMs = clampPulseMs(value);
    else return false;
    return true;
}

inline void parsePreset(cJSON *p, WidgetParams &out)
{
    out.monoflopThreshold = clampThreshold(presetparser::getFloat(p, "threshold", 5.0f));
    out.monoflopPulseMs = clampPulseMs(presetparser::getFloat(p, "pulseMs", 10.0f));
}
} // namespace nodes::monoflop

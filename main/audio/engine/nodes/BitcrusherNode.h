#pragma once

#include <cmath>
#include <cstddef>
#include <cstring>

#include "../CvConventions.h"
#include "../NodeInstance.h"
#include "../PresetParser.h"

namespace nodes::bitcrusher
{

inline float clampCrush(float value)
{
    return value < 0.0f ? 0.0f : (value > cv::kMaxVolts ? cv::kMaxVolts : value);
}

inline void render(NodeInstance &inst, size_t frames)
{
    using namespace noderender;
    auto &s = inst.bitcrusher;
    const bool bypassFed = controlPortConnected(inst, 1) ||
                           (inst.audioConnectedPortMask & synth::portBit(1)) != 0;
    const bool bypass = bypassFed ? portSample(inst, 1, 0) >= cv::kGateThresholdVolts : s.bypass;
    const bool crushFed = controlPortConnected(inst, 2) ||
                          (inst.audioConnectedPortMask & synth::portBit(2)) != 0;
    const float crush = clampCrush(crushFed ? portSample(inst, 2, 0) : s.crush);

    if (bypass || crush <= 0.0f)
    {
        for (size_t i = 0; i < frames; ++i)
            inst.outL[i] = portSample(inst, 0, i);
        return;
    }

    const float amount = crush / cv::kMaxVolts;
    const int bits = static_cast<int>(lroundf(16.0f - 14.0f * amount));
    const uint32_t holdSamples = static_cast<uint32_t>(lroundf(1.0f + 31.0f * amount * amount));
    const float levels = static_cast<float>((1u << (bits - 1)) - 1u);
    const float step = cv::kAudioPeakVolts / levels;

    for (size_t i = 0; i < frames; ++i)
    {
        if (s.holdCounter == 0)
        {
            s.heldSample = roundf(portSample(inst, 0, i) / step) * step;
            s.holdCounter = holdSamples;
        }
        inst.outL[i] = s.heldSample;
        --s.holdCounter;
    }
}

inline void build(NodeInstance &inst, const WidgetParams &p)
{
    inst.bitcrusher.crush = clampCrush(p.bitcrusherCrush);
    inst.bitcrusher.bypass = p.bitcrusherBypass;
    inst.bitcrusher.heldSample = 0.0f;
    inst.bitcrusher.holdCounter = 0;
}

inline bool setParam(NodeInstance &inst, const char *name, float value)
{
    if (name == nullptr)
        return false;
    if (strcmp(name, "crush") == 0)
    {
        inst.bitcrusher.crush = clampCrush(value);
        return true;
    }
    if (strcmp(name, "bypass") == 0)
    {
        inst.bitcrusher.bypass = value >= 0.5f;
        return true;
    }
    return false;
}

inline void parsePreset(cJSON *p, WidgetParams &out)
{
    out.bitcrusherCrush = clampCrush(presetparser::getFloat(p, "crush", 5.0f));
    out.bitcrusherBypass = presetparser::getBool(p, "bypass", false);
}

} // namespace nodes::bitcrusher

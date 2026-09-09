#pragma once

#include <cmath>
#include <cstddef>
#include <cstring>

#include "../CvConventions.h"
#include "../NodeInstance.h"
#include "../PresetParser.h"

// Stateless per-sample CV arithmetic ops (Multiplier/Adder/Divide/Abs) plus
// FrequencyDivider (the one member of this family with actual config -
// FrequencyDividerState in NodeState.h). Grouped in one file since they're
// mechanically near-identical thin wrappers around inPorts, not because they
// share code.
namespace nodes::arithmetic
{

inline void renderMultiplier(NodeInstance &inst, size_t frames)
{
    using namespace noderender;
    // When no input is audio-rate wired, portSample() returns the same
    // value for every i (ControlRoute fill or default - see
    // Graph::renderBlock) - compute once instead of rebuilding the same
    // product up to 128x (same pattern as Lfo's block-rate fast path).
    // controlRampPortMask must gate this too: an active MIDI-glide ramp
    // (Graph::renderBlock's midiPitchRampPort case) fills inPorts[] with a
    // genuinely per-sample-varying value WITHOUT ever setting
    // audioConnectedPortMask - reading only sample 0 would reduce that glide
    // to a single step for the whole block (plan section 6, finding 1:
    // "Glide durch CV-Rechennodes").
    if (inst.audioConnectedPortMask == 0 && inst.controlRampPortMask == 0)
    {
        float product = portSample(inst, 0, 0);
        for (int p = 1; p < inst.audioInPortCount; ++p)
        {
            product *= portSample(inst, p, 0);
        }
        for (size_t i = 0; i < frames; ++i)
        {
            inst.outL[i] = product;
        }
        return;
    }
    for (size_t i = 0; i < frames; ++i)
    {
        float product = portSample(inst, 0, i);
        for (int p = 1; p < inst.audioInPortCount; ++p)
        {
            product *= portSample(inst, p, i);
        }
        inst.outL[i] = product;
    }
}

inline void renderAdder(NodeInstance &inst, size_t frames)
{
    using namespace noderender;
    // See renderMultiplier()'s comment above - controlRampPortMask must gate
    // this fast path too, or an active MIDI-glide input gets reduced to its
    // block's first sample.
    if (inst.audioConnectedPortMask == 0 && inst.controlRampPortMask == 0)
    {
        float sum = 0.0f;
        for (int p = 0; p < inst.audioInPortCount; ++p)
        {
            sum += portSample(inst, p, 0);
        }
        for (size_t i = 0; i < frames; ++i)
        {
            inst.outL[i] = sum;
        }
        return;
    }
    for (size_t i = 0; i < frames; ++i)
    {
        float sum = 0.0f;
        for (int p = 0; p < inst.audioInPortCount; ++p)
        {
            sum += portSample(inst, p, i);
        }
        inst.outL[i] = sum;
    }
}

inline void renderDivide(NodeInstance &inst, size_t frames)
{
    using namespace noderender;
    // See renderMultiplier()'s comment above.
    if (inst.audioConnectedPortMask == 0 && inst.controlRampPortMask == 0)
    {
        const float numerator = portSample(inst, 0, 0);
        const float denominator = portSample(inst, 1, 0);
        const float result = denominator == 0.0f ? 0.0f : numerator / denominator;
        for (size_t i = 0; i < frames; ++i)
        {
            inst.outL[i] = result;
        }
        return;
    }
    for (size_t i = 0; i < frames; ++i)
    {
        const float numerator = portSample(inst, 0, i);
        const float denominator = portSample(inst, 1, i);
        inst.outL[i] = denominator == 0.0f ? 0.0f : numerator / denominator;
    }
}

inline float quantizeFrequencyDivider(float divider)
{
    constexpr float kSteps[] = {1.0f, 2.0f, 4.0f, 8.0f, 16.0f};
    float nearest = kSteps[0];
    float distance = std::fabs(divider - nearest);
    for (float step : kSteps)
    {
        const float candidateDistance = std::fabs(divider - step);
        if (candidateDistance < distance)
        {
            nearest = step;
            distance = candidateDistance;
        }
    }
    return nearest;
}

inline float frequencyDividerPitchOffset(float divider)
{
    // Exact nearest-step mapping for {1, 2, 4, 8, 16}. Besides being faster
    // than quantizeFrequencyDivider()+log2f(), this is exact: those steps
    // map to the integer 1 V/oct offsets {0, 1, 2, 3, 4}.
    if (divider <= 1.5f)
        return 0.0f;
    if (divider <= 3.0f)
        return 1.0f;
    if (divider <= 6.0f)
        return 2.0f;
    if (divider <= 12.0f)
        return 3.0f;
    return 4.0f;
}

inline float applyFrequencyDividerOffset(float inputCv, float pitchOffsetVolts)
{
    const float outputCv = inputCv - pitchOffsetVolts;
    return fminf(cv::kMaxVolts, fmaxf(0.0f, outputCv));
}

inline float dividePitchCv(float inputCv, float divider)
{
    return applyFrequencyDividerOffset(inputCv, frequencyDividerPitchOffset(divider));
}

inline void renderFrequencyDivider(NodeInstance &inst, size_t frames)
{
    using namespace noderender;
    const bool dividerConnected = controlPortConnected(inst, 1) ||
                                  (inst.audioConnectedPortMask & static_cast<uint8_t>(1u << 1)) != 0;
    // See renderMultiplier()'s comment above - a glide on the pitch input
    // (port 0) would otherwise be reduced to its block's first sample here,
    // exactly the case the plan's Frequency Divider diagnosis (section 2)
    // singles out.
    if (inst.audioConnectedPortMask == 0 && inst.controlRampPortMask == 0)
    {
        const float result = applyFrequencyDividerOffset(portSample(inst, 0, 0), inst.frequencyDivider.pitchOffsetVolts);
        for (size_t i = 0; i < frames; ++i)
        {
            inst.outL[i] = result;
        }
        return;
    }
    for (size_t i = 0; i < frames; ++i)
    {
        const float offset = dividerConnected ? frequencyDividerPitchOffset(portSample(inst, 1, i))
                                               : inst.frequencyDivider.pitchOffsetVolts;
        inst.outL[i] = applyFrequencyDividerOffset(portSample(inst, 0, i), offset);
    }
}

inline void renderAbs(NodeInstance &inst, size_t frames)
{
    using namespace noderender;
    // See renderMultiplier()'s comment above.
    if (inst.audioConnectedPortMask == 0 && inst.controlRampPortMask == 0)
    {
        const float result = fabsf(portSample(inst, 0, 0));
        for (size_t i = 0; i < frames; ++i)
        {
            inst.outL[i] = result;
        }
        return;
    }
    for (size_t i = 0; i < frames; ++i)
    {
        inst.outL[i] = fabsf(portSample(inst, 0, i));
    }
}

inline void buildFrequencyDivider(NodeInstance &inst, const WidgetParams &p)
{
    inst.frequencyDivider.divider = quantizeFrequencyDivider(p.frequencyDivider);
    inst.frequencyDivider.pitchOffsetVolts = frequencyDividerPitchOffset(inst.frequencyDivider.divider);
}

inline bool setParamFrequencyDivider(NodeInstance &inst, const char *name, float value)
{
    if (name != nullptr && strcmp(name, "divider") == 0)
    {
        inst.frequencyDivider.divider = quantizeFrequencyDivider(value);
        inst.frequencyDivider.pitchOffsetVolts = frequencyDividerPitchOffset(inst.frequencyDivider.divider);
        return true;
    }
    return false;
}

inline void parsePresetFrequencyDivider(cJSON *p, WidgetParams &out)
{
    out.frequencyDivider = quantizeFrequencyDivider(presetparser::getFloat(p, "divider", 2.0f));
}

// Shared by Multiplier and Adder (both configurable 2-8 inputs).
inline void parsePresetMultiplierOrAdder(cJSON *p, WidgetParams &out)
{
    const int inputCount = presetparser::getInt(p, "inputCount", 2);
    out.inputCount = static_cast<uint8_t>(inputCount < 2 ? 2 : (inputCount > 8 ? 8 : inputCount));
}

} // namespace nodes::arithmetic

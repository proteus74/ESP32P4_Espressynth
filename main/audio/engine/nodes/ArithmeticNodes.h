#pragma once

#include <cmath>
#include <cstddef>
#include <cstring>

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
    if (inst.audioConnectedPortMask == 0)
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
    if (inst.audioConnectedPortMask == 0)
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
    if (inst.audioConnectedPortMask == 0)
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

inline void renderFrequencyDivider(NodeInstance &inst, size_t frames)
{
    using namespace noderender;
    const bool dividerConnected = controlPortConnected(inst, 1) ||
                                  (inst.audioConnectedPortMask & static_cast<uint8_t>(1u << 1)) != 0;
    if (inst.audioConnectedPortMask == 0)
    {
        float divider = inst.frequencyDivider.divider;
        if (divider < 1.0f)
            divider = 1.0f;
        if (divider > 128.0f)
            divider = 128.0f;
        const float result = portSample(inst, 0, 0) / divider;
        for (size_t i = 0; i < frames; ++i)
        {
            inst.outL[i] = result;
        }
        return;
    }
    for (size_t i = 0; i < frames; ++i)
    {
        float divider = dividerConnected ? portSample(inst, 1, i) : inst.frequencyDivider.divider;
        if (divider < 1.0f)
            divider = 1.0f;
        if (divider > 128.0f)
            divider = 128.0f;
        inst.outL[i] = portSample(inst, 0, i) / divider;
    }
}

inline void renderAbs(NodeInstance &inst, size_t frames)
{
    using namespace noderender;
    if (inst.audioConnectedPortMask == 0)
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
    inst.frequencyDivider.divider = p.frequencyDivider;
}

inline bool setParamFrequencyDivider(NodeInstance &inst, const char *name, float value)
{
    if (name != nullptr && strcmp(name, "divider") == 0)
    {
        inst.frequencyDivider.divider = value;
        return true;
    }
    return false;
}

inline void parsePresetFrequencyDivider(cJSON *p, WidgetParams &out)
{
    out.frequencyDivider = presetparser::getFloat(p, "divider", 2.0f);
}

// Shared by Multiplier and Adder (both configurable 2-8 inputs).
inline void parsePresetMultiplierOrAdder(cJSON *p, WidgetParams &out)
{
    const int inputCount = presetparser::getInt(p, "inputCount", 2);
    out.inputCount = static_cast<uint8_t>(inputCount < 2 ? 2 : (inputCount > 8 ? 8 : inputCount));
}

} // namespace nodes::arithmetic

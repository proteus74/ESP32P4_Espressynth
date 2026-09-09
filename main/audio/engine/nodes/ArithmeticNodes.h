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
    // allPortsConstantForBlock() covers every case where every port resolves
    // to one repeated value this block - not audio-connected at all (a
    // ControlRoute fill or a type default, see Graph::renderBlock), a
    // genuine DSP edge that happens to be unchanging (e.g. a held note), and
    // an active MIDI-glide ramp is correctly recognized as NOT constant (it
    // scans the real buffer content rather than trusting
    // audioConnectedPortMask, see its comment in NodeInstance.h - this is
    // also what fixes plan section 6 finding 1, "Glide durch CV-
    // Rechennodes", for this node). Computing the product once, in the same
    // port order the loop below uses, is bit-identical to what every sample
    // would have produced (plan section 3, P2: "Frequency Divider, Detune,
    // Adder, Multiplier, Divide und Abs propagieren diese Information").
    float values[synth::kMaxAudioInPorts] = {};
    if (allPortsConstantForBlock(inst, inst.audioInPortCount, frames, values))
    {
        float product = values[0];
        for (int p = 1; p < inst.audioInPortCount; ++p)
        {
            product *= values[p];
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
    // See renderMultiplier()'s comment above.
    float values[synth::kMaxAudioInPorts] = {};
    if (allPortsConstantForBlock(inst, inst.audioInPortCount, frames, values))
    {
        float sum = 0.0f;
        for (int p = 0; p < inst.audioInPortCount; ++p)
        {
            sum += values[p];
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
    float numerator = 0.0f;
    float denominator = 0.0f;
    const bool numeratorConstant = audioPortConstantForBlock(inst, 0, frames, numerator);
    const bool denominatorConstant = audioPortConstantForBlock(inst, 1, frames, denominator);
    if (numeratorConstant && denominatorConstant)
    {
        const float result = denominator == 0.0f ? 0.0f : numerator / denominator;
        for (size_t i = 0; i < frames; ++i)
        {
            inst.outL[i] = result;
        }
        return;
    }
    if (denominatorConstant)
    {
        // Numerator varies, denominator doesn't - cache its reciprocal once
        // and multiply per sample instead of dividing (plan section 5,
        // Adder/Multiplier/Divide/Abs row: "konstante Nenner beim Divide
        // durch gecachten Kehrwert behandeln"). Same "compute 1/x once,
        // multiply from then on" technique this codebase already uses
        // elsewhere without -ffast-math (see Biquad::setCoeffs()'s
        // "Reciprocal trick" comment in DspPrimitives.h) - not literally
        // bit-identical to a fresh division every sample, but an already-
        // established, deliberate tradeoff in this codebase, unlike a
        // blanket -ffast-math flag. Division-by-zero keeps the existing
        // "outputs 0, not Inf/NaN" semantics.
        if (denominator == 0.0f)
        {
            memset(inst.outL, 0, frames * sizeof(float));
            return;
        }
        const float invDenominator = 1.0f / denominator;
        for (size_t i = 0; i < frames; ++i)
        {
            inst.outL[i] = portSample(inst, 0, i) * invDenominator;
        }
        return;
    }
    for (size_t i = 0; i < frames; ++i)
    {
        const float n = portSample(inst, 0, i);
        const float d = portSample(inst, 1, i);
        inst.outL[i] = d == 0.0f ? 0.0f : n / d;
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
    // singles out. dividerConnected's own branch (raw knob value vs. cached
    // pitchOffsetVolts) is preserved exactly, just evaluated once instead of
    // per sample when both ports turn out block-constant.
    float cvValue = 0.0f;
    float dividerValue = 0.0f;
    const bool cvConstant = audioPortConstantForBlock(inst, 0, frames, cvValue);
    const bool dividerConstant = audioPortConstantForBlock(inst, 1, frames, dividerValue);
    if (cvConstant && dividerConstant)
    {
        const float offset =
            dividerConnected ? frequencyDividerPitchOffset(dividerValue) : inst.frequencyDivider.pitchOffsetVolts;
        const float result = applyFrequencyDividerOffset(cvValue, offset);
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
    float value = 0.0f;
    if (audioPortConstantForBlock(inst, 0, frames, value))
    {
        const float result = fabsf(value);
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

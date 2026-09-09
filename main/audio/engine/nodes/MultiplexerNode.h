#pragma once

#include <cstddef>
#include <cstring>

#include "../CvConventions.h"
#include "../NodeInstance.h"
#include "../PresetParser.h"
#include "../SynthConfig.h"

// N-of-M (2-16) CV-selectable audio passthrough. "select" (port index 16,
// control-only - see PortTables' isControlOnlyInput) picks which of
// "in1".."in16" (indices 0-15) reaches "value": a genuine CV connection on
// "select" substitutes the widget's own vertical-slider override
// (MultiplexerState::manualSelect) outright, same "connected CV wins"
// convention as Oscillator's "tune"/Filter's cutoff. MultiplexerState (value
// member) stays in NodeState.h.
namespace nodes::multiplexer
{

namespace
{
constexpr int kSelectPort = 16;

// Splits 0-10V into `inputCount` equal bands, in input order - same idea as
// OscillatorNode.h's waveTypeFromCv (5 fixed bands), generalized to a
// runtime band count. 10V itself must still resolve to the last input, not
// fall off the end.
inline int selectFromCv(float volts, int inputCount)
{
    if (inputCount <= 1)
    {
        return 0;
    }
    int idx = static_cast<int>(volts / (cv::kMaxVolts / static_cast<float>(inputCount)));
    if (idx < 0)
        idx = 0;
    if (idx > inputCount - 1)
        idx = inputCount - 1;
    return idx;
}

inline int clampIdx(int idx, int inputCount)
{
    if (inputCount <= 0)
        return 0;
    if (idx < 0)
        return 0;
    if (idx > inputCount - 1)
        return inputCount - 1;
    return idx;
}

inline uint32_t softFadeSampleCount(float fadeMs)
{
    if (!(fadeMs > 0.0f))
        return 0;
    const float samples = fadeMs * static_cast<float>(synth::kSampleRate) / 1000.0f;
    return samples < 1.0f ? 1u : static_cast<uint32_t>(samples + 0.5f);
}
} // namespace

inline void render(NodeInstance &inst, size_t frames)
{
    using namespace noderender;
    auto &s = inst.multiplexer;
    const bool selectFed =
        controlPortConnected(inst, kSelectPort) || (inst.audioConnectedPortMask & synth::portBit(kSelectPort)) != 0;
    const uint8_t requestedTarget = static_cast<uint8_t>(
        selectFed ? selectFromCv(portSample(inst, kSelectPort, 0), s.inputCount)
                  : clampIdx(s.manualSelect, s.inputCount));
    // Lock target until current fade completes. Noisy/boundary CV must not
    // restart a long fade every block and keep the node permanently expensive.
    if (s.fadeSamples == 0 && requestedTarget != s.activeIndex)
    {
        s.fadeFrom = s.activeIndex;
        s.fadeTo = requestedTarget;
        s.fadePosition = 0;
        s.fadeSamples = softFadeSampleCount(s.softFadeMs);
        if (s.fadeSamples == 0)
        {
            s.activeIndex = requestedTarget;
            s.fadeFrom = requestedTarget;
            s.fadeTo = requestedTarget;
        }
    }
    for (size_t i = 0; i < frames; ++i)
    {
        if (s.fadeSamples == 0)
        {
            inst.outL[i] = portSample(inst, s.activeIndex, i);
            continue;
        }

        const float t = static_cast<float>(s.fadePosition + 1) / static_cast<float>(s.fadeSamples);
        const float from = portSample(inst, s.fadeFrom, i);
        const float to = portSample(inst, s.fadeTo, i);
        inst.outL[i] = from + (to - from) * t;
        ++s.fadePosition;
        if (s.fadePosition >= s.fadeSamples)
        {
            s.activeIndex = s.fadeTo;
            s.fadeSamples = 0;
            s.fadePosition = 0;
        }
    }
}

inline void build(NodeInstance &inst, const WidgetParams &p)
{
    inst.multiplexer.inputCount = p.inputCount;
    inst.multiplexer.manualSelect = p.multiplexerSelect;
    inst.multiplexer.softFadeMs = p.multiplexerSoftFadeMs < 0.0f
                                      ? 0.0f
                                      : (p.multiplexerSoftFadeMs > 100.0f ? 100.0f : p.multiplexerSoftFadeMs);
    inst.multiplexer.activeIndex = clampIdx(inst.multiplexer.manualSelect, inst.multiplexer.inputCount);
    inst.multiplexer.fadeFrom = inst.multiplexer.activeIndex;
    inst.multiplexer.fadeTo = inst.multiplexer.activeIndex;
    inst.multiplexer.fadeSamples = 0;
    inst.multiplexer.fadePosition = 0;
}

inline bool setParam(NodeInstance &inst, const char *name, float value)
{
    if (name != nullptr && strcmp(name, "select") == 0)
    {
        inst.multiplexer.manualSelect = static_cast<uint8_t>(value + 0.5f);
        return true;
    }
    if (name != nullptr && strcmp(name, "softFadeMs") == 0)
    {
        inst.multiplexer.softFadeMs = value < 0.0f ? 0.0f : (value > 100.0f ? 100.0f : value);
        return true;
    }
    return false;
}

inline void parsePreset(cJSON *p, WidgetParams &out)
{
    const int inputCount = presetparser::getInt(p, "inputCount", 2);
    out.inputCount = static_cast<uint8_t>(inputCount < 2 ? 2 : (inputCount > 16 ? 16 : inputCount));
    const int sel = presetparser::getInt(p, "select", 0);
    out.multiplexerSelect = static_cast<uint8_t>(sel < 0 ? 0 : sel);
    const float fadeMs = presetparser::getFloat(p, "softFadeMs", 5.0f);
    out.multiplexerSoftFadeMs = fadeMs < 0.0f ? 0.0f : (fadeMs > 100.0f ? 100.0f : fadeMs);
}

} // namespace nodes::multiplexer

#pragma once

#include <cstddef>
#include <cstring>

#include "../CvConventions.h"
#include "../NodeInstance.h"
#include "../PresetParser.h"
#include "../SynthConfig.h"

// N-of-M (2-8) CV-selectable audio passthrough. "select" (port index 8,
// control-only - see PortTables' isControlOnlyInput) picks which of
// "in1".."in8" (indices 0-7) reaches "value": a genuine CV connection on
// "select" substitutes the widget's own vertical-slider override
// (MultiplexerState::manualSelect) outright, same "connected CV wins"
// convention as Oscillator's "tune"/Filter's cutoff. MultiplexerState (value
// member) stays in NodeState.h.
namespace nodes::multiplexer
{

namespace
{
constexpr int kSelectPort = 8;

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
} // namespace

inline void render(NodeInstance &inst, size_t frames)
{
    using namespace noderender;
    auto &s = inst.multiplexer;
    const bool selectFed =
        controlPortConnected(inst, kSelectPort) || (inst.audioConnectedPortMask & synth::portBit(kSelectPort)) != 0;
    const int idx = selectFed ? selectFromCv(portSample(inst, kSelectPort, 0), s.inputCount)
                               : clampIdx(s.manualSelect, s.inputCount);
    for (size_t i = 0; i < frames; ++i)
    {
        inst.outL[i] = portSample(inst, idx, i);
    }
}

inline void build(NodeInstance &inst, const WidgetParams &p)
{
    inst.multiplexer.inputCount = p.inputCount;
    inst.multiplexer.manualSelect = p.multiplexerSelect;
}

inline bool setParam(NodeInstance &inst, const char *name, float value)
{
    if (name != nullptr && strcmp(name, "select") == 0)
    {
        inst.multiplexer.manualSelect = static_cast<uint8_t>(value + 0.5f);
        return true;
    }
    return false;
}

inline void parsePreset(cJSON *p, WidgetParams &out)
{
    const int inputCount = presetparser::getInt(p, "inputCount", 2);
    out.inputCount = static_cast<uint8_t>(inputCount < 2 ? 2 : (inputCount > 8 ? 8 : inputCount));
    const int sel = presetparser::getInt(p, "select", 0);
    out.multiplexerSelect = static_cast<uint8_t>(sel < 0 ? 0 : sel);
}

} // namespace nodes::multiplexer

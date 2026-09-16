#pragma once

#include <cstddef>
#include <cstring>

#include "../CvConventions.h"
#include "../DspPrimitives.h"
#include "../NodeInstance.h"
#include "../PresetParser.h"

// Equal-power stereo panner: single audio input, one CV input ("pan",
// 0-10V, center 5V = hard center) split to two outputs (out1/out2).
// Uses the shared quarter-cycle sine LUT (dsp::sineLookup) for the
// sin/cos pan law, same technique as the biquad coefficients in
// DspPrimitives.h - avoids sinf/cosf on this per-sample hot path.
// PanState itself lives in NodeState.h (in NodeInstance's inline union).
namespace nodes::pan
{

inline float clampCv(float value)
{
    return value < 0.0f ? 0.0f : (value > cv::kMaxVolts ? cv::kMaxVolts : value);
}

inline bool portFed(const NodeInstance &inst, uint8_t port)
{
    return noderender::controlPortConnected(inst, port) ||
           (inst.audioConnectedPortMask & synth::portBit(port)) != 0;
}

inline void render(NodeInstance &inst, size_t frames)
{
    using namespace noderender;
    auto &s = inst.pan;
    const bool panFed = portFed(inst, 1);

    for (size_t i = 0; i < frames; ++i)
    {
        const float panCv = clampCv(panFed ? portSample(inst, 1, i) : s.pan);
        const float phase = (panCv * cv::kInvMaxVolts) * 0.25f;
        const float gain2 = dsp::sineLookup(phase);
        const float gain1 = dsp::sineLookup(phase + 0.25f);
        const float dry = portSample(inst, 0, i);
        inst.outL[i] = dry * gain1;
        inst.outR[i] = dry * gain2;
    }
}

inline void build(NodeInstance &inst, const WidgetParams &p)
{
    inst.pan.pan = clampCv(p.panCv);
}

inline bool setParam(NodeInstance &inst, const char *name, float value)
{
    if (name == nullptr)
        return false;
    if (strcmp(name, "pan") == 0) inst.pan.pan = clampCv(value);
    else return false;
    return true;
}

inline void parsePreset(cJSON *p, WidgetParams &out)
{
    out.panCv = clampCv(presetparser::getFloat(p, "pan", 5.0f));
}

} // namespace nodes::pan

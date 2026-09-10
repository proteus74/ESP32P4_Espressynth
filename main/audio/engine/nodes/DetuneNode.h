#pragma once

#include <cstddef>
#include <cstring>

#include "../NodeInstance.h"
#include "../PresetParser.h"
#include "../SynthConfig.h"

// cv (port "cv", index 0) in -> f1/f2 (outL/outR) out: two pitch-CV signals
// spread symmetrically around cv, offset by +/-(amount*maxVolts). 1V/oct is
// already logarithmic, so unlike the old Hz-domain Cent math this is a plain
// volt addition. DetuneState (value member) stays in NodeState.h.
namespace nodes::detune
{

inline void render(NodeInstance &inst, size_t frames)
{
    using namespace noderender;
    // cv: audioPortConstantForBlock() (NodeInstance.h) covers every case
    // where it resolves to one repeated value this block - unconnected,
    // ControlRoute-fed, or a genuinely audio-connected edge that just
    // happens to be unchanging - and correctly recognizes a ControlRoute-fed
    // MIDI-glide mid-ramp as NOT constant (it scans the real buffer content,
    // see that function's comment; this is also what fixes plan section 6
    // finding 1, "Glide durch CV-Rechennodes", for this node). Plain volt
    // arithmetic below, no trig/pow involved, so per-sample evaluation in
    // the genuinely-varying case is effectively free either way.
    auto &s = inst.detune;
    const float amount = s.amountSmooth.process(s.amountTarget);
    const float deltaVolts = amount * s.maxVolts;
    float cvValue = 0.0f;
    if (audioPortConstantForBlock(inst, 0, frames, cvValue))
    {
        const float f1 = cvValue + deltaVolts;
        const float f2 = cvValue - deltaVolts;
        for (size_t i = 0; i < frames; ++i)
        {
            inst.outL[i] = f1;
            inst.outR[i] = f2;
        }
        return;
    }
    // cv is a tagged ramp (not block-constant, but a linear start+i*inc):
    // a constant offset add keeps it linear, both outputs share the same
    // inc since deltaVolts is block-constant.
    if (const NodeInstance::RampDescriptor *r = rampForPort(inst, 0))
    {
        float f1 = r->start + deltaVolts;
        float f2 = r->start - deltaVolts;
        for (size_t i = 0; i < frames; ++i)
        {
            inst.outL[i] = f1;
            inst.outR[i] = f2;
            f1 += r->inc;
            f2 += r->inc;
        }
        return;
    }
    for (size_t i = 0; i < frames; ++i)
    {
        const float cvIn = portSample(inst, 0, i);
        inst.outL[i] = cvIn + deltaVolts;
        inst.outR[i] = cvIn - deltaVolts;
    }
}

inline void build(NodeInstance &inst, const WidgetParams &p)
{
    inst.detune.amountTarget = p.detuneAmount;
    inst.detune.maxVolts = p.detuneMaxVolts;
    // One smoothing step per block (not per sample - f0 arrives as a
    // per-block-constant control value anyway, see Graph::renderBlock), so
    // the cutoff is expressed against the block rate, not kSampleRate.
    inst.detune.amountSmooth.setCutoff(
        20.0f, static_cast<float>(synth::kSampleRate) / static_cast<float>(synth::kBlockSize));
}

inline bool setParam(NodeInstance &inst, const char *name, float value)
{
    if (name == nullptr)
    {
        return false;
    }
    if (strcmp(name, "amount") == 0)
    {
        inst.detune.amountTarget = value;
        return true;
    }
    if (strcmp(name, "maxVolts") == 0)
    {
        inst.detune.maxVolts = value;
        return true;
    }
    return false;
}

inline void parsePreset(cJSON *p, WidgetParams &out)
{
    out.detuneAmount = presetparser::getFloat(p, "amount", 0.0f);
    out.detuneMaxVolts = presetparser::getFloat(p, "maxVolts", 0.5f);
}

} // namespace nodes::detune

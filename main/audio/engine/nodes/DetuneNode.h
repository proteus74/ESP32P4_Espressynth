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
    // cv: a genuine DSP edge (audioConnectedPortMask) is read per sample
    // below - plain volt arithmetic, no trig/pow involved, so per-sample
    // evaluation is effectively free. Unconnected (or ControlRoute-fed,
    // e.g. Knob by paramName - always block-constant) still only needs one
    // read per block.
    auto &s = inst.detune;
    const bool cvAudioFed = (inst.audioConnectedPortMask & static_cast<uint8_t>(1u << 0)) != 0;
    const float amount = s.amountSmooth.process(s.amountTarget);
    const float deltaVolts = amount * s.maxVolts;
    if (cvAudioFed)
    {
        for (size_t i = 0; i < frames; ++i)
        {
            const float cvIn = portSample(inst, 0, i);
            inst.outL[i] = cvIn + deltaVolts;
            inst.outR[i] = cvIn - deltaVolts;
        }
        return;
    }
    const float cvIn = portSample(inst, 0, 0);
    const float f1 = cvIn + deltaVolts;
    const float f2 = cvIn - deltaVolts;
    for (size_t i = 0; i < frames; ++i)
    {
        inst.outL[i] = f1;
        inst.outR[i] = f2;
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

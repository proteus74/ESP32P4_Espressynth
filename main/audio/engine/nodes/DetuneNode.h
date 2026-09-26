#pragma once

#include <cmath>
#include <cstddef>
#include <cstring>

#include "../CvConventions.h"
#include "../NodeInstance.h"
#include "../PresetParser.h"
#include "../SynthConfig.h"

// cv (port "cv", index 0) in -> out (outL) out: one pitch-CV signal, cv plus
// a settings-dialog-scaled/quantized offset - same knob convention as
// StepArpeggiator's per-step pitch knob (see DetuneState's comment in
// NodeState.h). DetuneState (value member) stays in NodeState.h.
namespace nodes::detune
{

// amountVolts: 0-10V, 5V = 0 offset - see DetuneState's comment. Returns the
// resulting pitch-CV offset in volts, already clamped (by construction, the
// normalized -1..1 factor) and rounded to the stepCents grid.
inline float detuneOffsetVolts(float amountVolts, float maxOffsetCents, float stepCents)
{
    const float normalized = (amountVolts - cv::kMaxVolts * 0.5f) / (cv::kMaxVolts * 0.5f);
    const float rawCents = normalized * maxOffsetCents;
    const float steppedCents = stepCents > 0.0f ? roundf(rawCents / stepCents) * stepCents : rawCents;
    return steppedCents / 1200.0f; // 1200 cents = 1 octave = 1V (kPitchCvVoltsPerOctave)
}

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
    // "amount" port (index 1): a wired source overrides the settings/Knob value.
    const bool amountFed = controlPortConnected(inst, 1) || (inst.audioConnectedPortMask & synth::portBit(1)) != 0;
    float amountIn = amountFed ? portSample(inst, 1, 0) : s.amountTarget;
    amountIn = amountIn < 0.0f ? 0.0f : (amountIn > cv::kMaxVolts ? cv::kMaxVolts : amountIn);
    const float amount = s.amountSmooth.process(amountIn);
    const float deltaVolts = detuneOffsetVolts(amount, s.maxOffsetCents, s.stepCents);
    float cvValue = 0.0f;
    if (audioPortConstantForBlock(inst, 0, frames, cvValue))
    {
        const float out = cvValue + deltaVolts;
        for (size_t i = 0; i < frames; ++i)
        {
            inst.outL[i] = out;
        }
        return;
    }
    // cv is a tagged ramp (not block-constant, but a linear start+i*inc): a
    // constant offset add keeps it linear.
    if (const NodeInstance::RampDescriptor *r = rampForPort(inst, 0))
    {
        float out = r->start + deltaVolts;
        for (size_t i = 0; i < frames; ++i)
        {
            inst.outL[i] = out;
            out += r->inc;
        }
        return;
    }
    for (size_t i = 0; i < frames; ++i)
    {
        inst.outL[i] = portSample(inst, 0, i) + deltaVolts;
    }
}

inline void build(NodeInstance &inst, const WidgetParams &p)
{
    inst.detune.amountTarget = p.detuneAmount;
    inst.detune.maxOffsetCents = p.detuneMaxOffsetCents;
    inst.detune.stepCents = p.detuneStepCents;
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
    if (strcmp(name, "maxOffsetCents") == 0)
    {
        inst.detune.maxOffsetCents = value;
        return true;
    }
    if (strcmp(name, "stepCents") == 0)
    {
        inst.detune.stepCents = value;
        return true;
    }
    return false;
}

inline void parsePreset(cJSON *p, WidgetParams &out)
{
    out.detuneAmount = presetparser::getFloat(p, "amount", 5.0f);
    out.detuneMaxOffsetCents = presetparser::getFloat(p, "maxOffsetCents", 1200.0f);
    out.detuneStepCents = presetparser::getFloat(p, "stepCents", 100.0f);
}

} // namespace nodes::detune

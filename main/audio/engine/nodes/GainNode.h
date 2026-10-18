#pragma once

#include <cmath>
#include <cstddef>
#include <cstring>

#include "../CvConventions.h"
#include "../NodeInstance.h"
#include "../PresetParser.h"
#include "../SynthConfig.h"

// Simple audio-rate VCA: multiplies "in" (Port 0) by "amount"/Output Gain
// (CV, Port 1 - real audio-rate, may be DSP-edge-fed) and by "curve" (CV,
// Port 2, independent second VCA stage - 5V unity, 0V silent, 10V = 2x),
// each falling back to its own manual scalar (amountCv/curveCv) when
// unwired. See GainState (NodeState.h) for the amount/curve/global/
// amountSmooth/curveSmooth fields.
namespace nodes::gain
{

inline void render(NodeInstance &inst, size_t frames)
{
    using namespace noderender;
    auto &s = inst.gain;
    // "amount"/"curve" (Port-Index 1/2) are real audio-rate ports (see
    // PortTables.h - unlike Detune's "cv" they are NOT listed in
    // ports::isControlOnlyInput), any source may feed them sample-accurately
    // via a DSP edge. When wired, the CV already arrives natively per sample
    // - no additional smoothing needed, that would just be another low-pass
    // over an already continuous signal. When NOT wired, each falls back to
    // its own manual/knob-driven scalar (amountCv/curveCv), smoothed PER
    // SAMPLE by amountSmooth/curveSmooth (see GainState) - without that, a
    // block-constant jump produces a ~344Hz staircase function that
    // modulates onto the signal as sidebands/metallic sound.
    //
    // "global" (Port-Index 3, control-only - see isControlOnlyInput): same
    // pattern as Lfo's "global". When active, makes voice 0 of this widget
    // authoritative for the gain - "in" (Port 0) always remains this voice's
    // own signal, only "amount"/"curve" (wired input port OR manual scalar)
    // then comes from src instead of inst.
    const bool modeFed = controlPortConnected(inst, 3) || (inst.audioConnectedPortMask & static_cast<uint8_t>(1u << 3)) != 0;
    const bool globalActive = modeFed ? (portSample(inst, 3, 0) >= cv::kGateThresholdVolts) : s.global;
    const NodeInstance &src = (globalActive && inst.primary != nullptr) ? *inst.primary : inst;

    const bool amountFed = (src.audioConnectedPortMask & static_cast<uint8_t>(1u << 1)) != 0;
    const bool curveFed = (src.audioConnectedPortMask & static_cast<uint8_t>(1u << 2)) != 0;
    // Resolved once per block (same aliasing reason as FilterNode.h: the
    // smoother writes through inst, so portSample()'s inPorts[] load would
    // be repeated every sample otherwise).
    const float *in = inst.inPorts[0];
    const float inScalar = inst.controlValue[0];

    if (!amountFed && !curveFed)
    {
        // Fully manual/knob-driven: fold both stages into one smoothed
        // multiplier - same fast-path pattern the single-stage VCA used
        // before (converge-and-snap, ramp-descriptor shortcut for a tagged
        // ramp on "in").
        const float target = cv::vcaGain(s.amountCv, false) * cv::curveGain(s.curveCv);
        // Smoothing convergence stop (plan section 5, Gain/VCA row: same
        // pattern already applied to Oscillator/Hammond's gain - see
        // OscillatorNode.h): once the smoother has settled within epsilon
        // of its target, snap it there and skip the per-sample
        // OnePoleLowpass::process() call for the rest of this block.
        constexpr float kConvergeEps = 1e-4f;
        const bool converged = fabsf(s.amountSmooth.current() - target) <= kConvergeEps;
        if (converged)
        {
            s.amountSmooth.snapTo(target);
            // combined gain is block-constant here (target) - if "in"
            // (port 0) is itself a tagged ramp, the product is still linear.
            if (const NodeInstance::RampDescriptor *r = rampForPort(inst, 0))
            {
                float acc = r->start * target;
                const float step = r->inc * target;
                for (size_t i = 0; i < frames; ++i)
                {
                    inst.outL[i] = acc;
                    acc += step;
                }
                return;
            }
        }
        if (converged)
        {
            for (size_t i = 0; i < frames; ++i)
            {
                inst.outL[i] = (in != nullptr ? in[i] : inScalar) * target;
            }
            return;
        }
        for (size_t i = 0; i < frames; ++i)
        {
            inst.outL[i] = (in != nullptr ? in[i] : inScalar) * s.amountSmooth.process(target);
        }
        return;
    }

    // At least one of "amount"/"curve" is audio-rate fed - compute per sample.
    const float *amountBuf = amountFed ? src.inPorts[1] : nullptr;
    const float *curveBuf = curveFed ? src.inPorts[2] : nullptr;
    const float amountTarget = cv::vcaGain(s.amountCv, false);
    const float curveTarget = cv::curveGain(s.curveCv);
    for (size_t i = 0; i < frames; ++i)
    {
        const float outputGain = amountBuf != nullptr ? cv::vcaGain(amountBuf[i], false)
                                                      : s.amountSmooth.process(amountTarget);
        const float curveGain = curveBuf != nullptr ? cv::curveGain(curveBuf[i])
                                                    : s.curveSmooth.process(curveTarget);
        inst.outL[i] = (in != nullptr ? in[i] : inScalar) * outputGain * curveGain;
    }
}

inline void build(NodeInstance &inst, const WidgetParams &p)
{
    inst.gain.amountCv = p.amount;
    inst.gain.curveCv = p.curve;
    inst.gain.global = p.gainGlobal;
    // Unlike Detune's amountSmooth (block rate) these smoothers run PER
    // SAMPLE (see render() above) - cutoff therefore against the real
    // sample rate, same pattern as ReverbLight's damping filter
    // (dampingFilter). 300Hz = ~0.5ms time constant: fast enough to follow a
    // turned knob/slider without noticeable delay, but without the
    // block-constant corners that caused the metallic modulation sound.
    inst.gain.amountSmooth.setCutoff(300.0f, static_cast<float>(synth::kSampleRate));
    inst.gain.curveSmooth.setCutoff(300.0f, static_cast<float>(synth::kSampleRate));
}

inline bool setParam(NodeInstance &inst, const char *name, float value)
{
    // "amount"/"curve" are raw CV (0-10V, see CvConventions.h) instead of a
    // direct multiplier. "global" is handled separately (see Graph.cpp's
    // applyLiveParams - bool field).
    if (name == nullptr) return false;
    if (strcmp(name, "amount") == 0)
    {
        inst.gain.amountCv = value;
        return true;
    }
    if (strcmp(name, "curve") == 0)
    {
        inst.gain.curveCv = value;
        return true;
    }
    return false;
}

inline void parsePreset(cJSON *p, WidgetParams &out)
{
    out.amount = presetparser::getFloat(p, "amount", 10.0f);
    out.curve = presetparser::getFloat(p, "curve", 5.0f);
    out.gainGlobal = presetparser::getBool(p, "global", false);
}

} // namespace nodes::gain

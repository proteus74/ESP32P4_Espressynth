#pragma once

#include <cmath>
#include <cstddef>
#include <cstring>

#include "../CvConventions.h"
#include "../NodeInstance.h"
#include "../PresetParser.h"
#include "../SynthConfig.h"

// Simple audio-rate VCA: multiplies "in" (Port 0) by "amount" (CV, Port 1 -
// real audio-rate, may be DSP-edge-fed) or the manual amountCv scalar
// fallback. See GainState (NodeState.h) for the amount/global/amountSmooth
// fields.
namespace nodes::gain
{

inline void render(NodeInstance &inst, size_t frames)
{
    using namespace noderender;
    auto &s = inst.gain;
    // "amount" (Port-Index 1) is a real audio-rate port (see PortTables.h -
    // unlike Detune's "cv" it is NOT listed in ports::isControlOnlyInput),
    // any source may feed it sample-accurately via a DSP edge. When wired,
    // the CV already arrives natively per sample - no additional smoothing
    // needed, that would just be another low-pass over an already continuous
    // signal. When NOT wired, it falls back to the manual/knob-driven
    // amountCv scalar, which amountSmooth smooths PER SAMPLE (see
    // GainState) - without that, a block-constant jump produces a ~344Hz
    // staircase function that modulates onto the signal as sidebands/
    // metallic sound.
    //
    // "global" (Port-Index 3, control-only - see isControlOnlyInput): same
    // pattern as Lfo's "global". When active, makes voice 0 of this widget
    // authoritative for the gain - "in" (Port 0) always remains this voice's
    // own signal, only "amount"/"curve" (wired input port OR manual scalar)
    // then comes from src instead of inst.
    const bool modeFed = controlPortConnected(inst, 3) || (inst.audioConnectedPortMask & static_cast<uint8_t>(1u << 3)) != 0;
    const bool globalActive = modeFed ? (portSample(inst, 3, 0) >= cv::kGateThresholdVolts) : s.global;
    const NodeInstance &src = (globalActive && inst.primary != nullptr) ? *inst.primary : inst;
    // "curve" (Port-Index 2, control-only) - discrete 2-position 0-10V
    // selector, same pattern as Filter's "filterType"/"slope": <5V linear,
    // >=5V exponential. Falls back to the manual/button-set
    // GainState::exponential when unwired.
    const bool curveFed = controlPortConnected(src, 2) || (src.audioConnectedPortMask & static_cast<uint8_t>(1u << 2)) != 0;
    const bool exponential = curveFed ? (portSample(src, 2, 0) >= cv::kMaxVolts * 0.5f) : src.gain.exponential;
    if ((src.audioConnectedPortMask & static_cast<uint8_t>(1u << 1)) != 0)
    {
        for (size_t i = 0; i < frames; ++i)
        {
            const float amount = cv::vcaGain(src.inPorts[1][i], exponential);
            inst.outL[i] = portSample(inst, 0, i) * amount;
        }
    }
    else
    {
        const float target = cv::vcaGain(s.amountCv, exponential);
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
            // "amount" is block-constant here (target) - if "in" (port 0)
            // is itself a tagged ramp, the product is still linear.
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
        for (size_t i = 0; i < frames; ++i)
        {
            const float amount = converged ? target : s.amountSmooth.process(target);
            inst.outL[i] = portSample(inst, 0, i) * amount;
        }
    }
}

inline void build(NodeInstance &inst, const WidgetParams &p)
{
    inst.gain.amountCv = p.amount;
    inst.gain.exponential = p.amountExponential;
    inst.gain.global = p.gainGlobal;
    // Unlike Detune's amountSmooth (block rate) this smoother runs PER
    // SAMPLE (see render() above) - cutoff therefore against the real
    // sample rate, same pattern as ReverbLight's damping filter
    // (dampingFilter). 300Hz = ~0.5ms time constant: fast enough to follow a
    // turned knob/slider without noticeable delay, but without the
    // block-constant corners that caused the metallic modulation sound.
    inst.gain.amountSmooth.setCutoff(300.0f, static_cast<float>(synth::kSampleRate));
}

inline bool setParam(NodeInstance &inst, const char *name, float value)
{
    // "amount" is raw CV (0-10V, see CvConventions.h) instead of a direct
    // multiplier. "curve" ("linear"/"exponential") and "global" are handled
    // separately (see Graph.cpp's applyLiveParams - string/bool fields).
    if (name != nullptr && strcmp(name, "amount") == 0)
    {
        inst.gain.amountCv = value;
        return true;
    }
    return false;
}

inline void parsePreset(cJSON *p, WidgetParams &out)
{
    out.amount = presetparser::getFloat(p, "amount", 10.0f);
    out.amountExponential = strcmp(presetparser::getString(p, "curve", "linear"), "exponential") == 0;
    out.gainGlobal = presetparser::getBool(p, "global", false);
}

} // namespace nodes::gain

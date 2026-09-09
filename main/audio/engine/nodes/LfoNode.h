#pragma once

#include <cstddef>
#include <cstring>

#include "../CvConventions.h"
#include "../NodeInstance.h"
#include "../PresetParser.h"
#include "../SynthConfig.h"

// Free-running LFO (sine/square/triangle/sawtooth/noise) with sync/one-shot/
// global modes. LfoState (value member) stays in NodeState.h.
namespace nodes::lfo
{

constexpr float kMaxRateHz = 50.0f;

inline float clampRate(float value)
{
    if (!(value >= 0.0f)) return 0.0f;
    return value > kMaxRateHz ? kMaxRateHz : value;
}

inline float clampRate(float value, float maxRate)
{
    const float limit = clampRate(maxRate);
    const float safe = value >= 0.0f ? value : 0.0f;
    return safe > limit ? limit : safe;
}

inline void render(NodeInstance &inst, size_t frames)
{
    using namespace noderender;
    const float kSampleRate = static_cast<float>(synth::kSampleRate);
    auto &s = inst.lfo;
    // "global" (port 2, control-only - see isControlOnlyInput): same
    // dual-source-fed check as sync/oneShot below. >=kGateThresholdVolts
    // (or the "Global" UI toggle when unconnected) makes voice 0 of this
    // widget authoritative - every other voice just mirrors voice 0's
    // already-rendered output instead of computing its own phase (see
    // Graph::renderBlock's matching force-active handling for voice 0 of a
    // global Lfo widget, needed so voice 0 keeps advancing even while its
    // own note is idle).
    const bool modeFed = controlPortConnected(inst, 2) || (inst.audioConnectedPortMask & static_cast<uint8_t>(1u << 2)) != 0;
    const bool globalActive = modeFed ? (portSample(inst, 2, 0) >= cv::kGateThresholdVolts) : s.global;
    if (globalActive && inst.primary != nullptr && inst.primary != &inst)
    {
        // A true buffer VIEW (aliasing inst.outL directly to inst.primary->
        // outL, plan section 5's literal wording) was considered and
        // rejected: Graph::renderBlock's per-instance idle-silence handling
        // (the "first silent block after activity" memset) operates on each
        // instance's OWN outL independently - if a non-primary voice went
        // idle while primary/other mirrored voices stayed active, that
        // memset would zero the SHARED buffer out from under all of them.
        // A plain bulk copy has no such hazard and is already about as
        // cheap as this gets - memcpy instead of a manual per-sample loop
        // lets the compiler/hardware use a wider copy than one float at a
        // time.
        memcpy(inst.outL, inst.primary->outL, frames * sizeof(float));
        return;
    }
    // The LFO's "value" output always swings within the project's 0-10V CV
    // window (matches every other CV-carrying port, see CvConventions.h).
    // amplitude (0-10V) is the peak-to-peak-ish swing: in "auto" zero-point
    // mode the output ranges [0, amplitude] (halfSwing = amplitude/2,
    // center = halfSwing); in "fixed" mode the same +/-halfSwing excursion
    // is centered on the user-set zeroPoint instead. Default amplitude=10V
    // + auto mode reproduces the old fixed-5V-center/full-swing behavior
    // exactly.
    const float halfSwing = s.amplitude * 0.5f;
    // Bipolar mode ignores zeroPointFixed/zeroPoint - center is always 0V,
    // and the output clamps to the audio domain's +/-kAudioPeakVolts instead
    // of the CV domain's 0-kMaxVolts (see LfoState::bipolar in NodeState.h).
    const float clampLo = s.bipolar ? -cv::kAudioPeakVolts : 0.0f;
    const float clampHi = s.bipolar ? cv::kAudioPeakVolts : cv::kMaxVolts;
    // "zeroPoint" (port 3, control-only - see isControlOnlyInput): same
    // dual-source-fed check as sync/oneShot/global above. A connected CV
    // SUBSTITUTES the "Fixed Zero Point" slider's own zeroPoint outright
    // (same pattern as Oscillator's tuneVolts), read once per block like
    // every other control-only input here - a center-shift has no need for
    // per-sample resolution. Not wired: falls back to the widget's own
    // zeroPoint slider, unchanged from before.
    const bool zeroPointFed = controlPortConnected(inst, 3) || (inst.audioConnectedPortMask & static_cast<uint8_t>(1u << 3)) != 0;
    const float zeroPointVolts = zeroPointFed ? portSample(inst, 3, 0) : s.zeroPoint;
    const float center = s.bipolar ? 0.0f : (s.zeroPointFixed ? zeroPointVolts : halfSwing);
    // Degrees -> 0..1 fraction, added on every phaseGen evaluation (see
    // PhaseOsc::currentSample/process) - so a sync reset
    // (phaseGen.resetPhase() below) automatically lands exactly at this
    // offset, without resetPhase() itself needing to know about it.
    const float phaseOffset = s.phaseOffsetDeg / 360.0f;
    // Sync (port 0) and One Shot (port 1), both control-rate: a port counts
    // as "fed" via either a ControlRoute (Knob/Button by paramName -
    // controlConnectedPortMask) or a genuine DSP edge (wired directly from
    // another node's output - audioConnectedPortMask), same dual-source
    // check as every other control-only input (see Oscillator's
    // pitchCv/tune). When One Shot's port is unconnected, its UI toggle
    // (s.oneShot) is the fallback - matches Reverb's bypass pattern.
    const bool syncFed = controlPortConnected(inst, 0) || (inst.audioConnectedPortMask & static_cast<uint8_t>(1u << 0)) != 0;
    const float syncVal = syncFed ? portSample(inst, 0, 0) : 0.0f;
    const bool syncAbove = syncVal >= cv::kGateThresholdVolts;
    const bool syncEdge = syncFed && syncAbove && !s.syncPrev;
    if (syncFed)
    {
        s.syncPrev = syncAbove;
    }
    const bool oneShotFed = controlPortConnected(inst, 1) || (inst.audioConnectedPortMask & static_cast<uint8_t>(1u << 1)) != 0;
    const bool oneShotOn = oneShotFed ? (portSample(inst, 1, 0) >= cv::kGateThresholdVolts) : s.oneShot;
    if (syncEdge)
    {
        s.phaseGen.resetPhase();
        if (oneShotOn)
        {
            // Re-arm: play exactly one fresh cycle from this edge.
            s.oneShotDone = false;
            s.oneShotAccum = 0.0f;
        }
    }
    // One Shot forces the per-sample path unconditionally (it must freeze
    // mid-block the instant a full cycle completes - the fast interpolated
    // Sine path below only samples phaseGen at the two block boundaries and
    // can't represent that). Any sync edge also forces per-sample even for
    // a Sine wave, so the interpolated path can't "swallow" a sync pulse
    // landing mid-block.
    if (s.wave != WaveType::Sine || syncEdge || oneShotOn)
    {
        for (size_t i = 0; i < frames; ++i)
        {
            if (oneShotOn && s.oneShotDone)
            {
                // Frozen (from an earlier sample this block, or an earlier
                // block entirely): the phase isn't advancing anymore, so
                // the waveform value is now constant until the next sync
                // edge re-arms it. Compute it once and fill the rest of
                // the block directly instead of recomputing the identical
                // value every remaining sample (plan section 5: "One-Shot-
                // Endwert direkt halten").
                const float frozenValue =
                    clampf(center + s.phaseGen.currentSample(s.wave, phaseOffset) * halfSwing, clampLo, clampHi);
                for (; i < frames; ++i)
                {
                    inst.outL[i] = frozenValue;
                }
                return;
            }
            const float raw = s.phaseGen.process(s.wave, s.rate, kSampleRate, phaseOffset);
            if (oneShotOn)
            {
                s.oneShotAccum += s.rate / kSampleRate;
                if (s.oneShotAccum >= 1.0f)
                {
                    s.oneShotDone = true;
                    s.oneShotAccum = 0.0f;
                }
            }
            inst.outL[i] = clampf(center + raw * halfSwing, clampLo, clampHi);
        }
        return;
    }
    const float start = clampf(center + s.phaseGen.currentSample(s.wave, phaseOffset) * halfSwing, clampLo, clampHi);
    s.phaseGen.advance(s.rate, kSampleRate, frames);
    const float end = clampf(center + s.phaseGen.currentSample(s.wave, phaseOffset) * halfSwing, clampLo, clampHi);
    const float increment = (end - start) / static_cast<float>(frames);
    float value = start;
    for (size_t i = 0; i < frames; ++i)
    {
        inst.outL[i] = value;
        value += increment;
    }
}

inline void build(NodeInstance &inst, const WidgetParams &p)
{
    inst.lfo.wave = p.waveType;
    inst.lfo.maxRateHz = clampRate(p.lfoMaxRate);
    inst.lfo.rate = clampRate(p.lfoRate, inst.lfo.maxRateHz);
    inst.lfo.amplitude = p.lfoAmplitude;
    inst.lfo.phaseOffsetDeg = p.lfoPhaseDeg;
    inst.lfo.zeroPointFixed = p.lfoZeroPointFixed;
    inst.lfo.zeroPoint = p.lfoZeroPoint;
    inst.lfo.bipolar = p.lfoBipolar;
    inst.lfo.oneShot = p.lfoOneShot;
    inst.lfo.global = p.lfoGlobal;
}

inline bool setParam(NodeInstance &inst, const char *name, float value)
{
    if (name == nullptr)
    {
        return false;
    }
    if (strcmp(name, "rate") == 0)
    {
        inst.lfo.rate = clampRate(value, inst.lfo.maxRateHz);
        return true;
    }
    if (strcmp(name, "maxRate") == 0)
    {
        inst.lfo.maxRateHz = clampRate(value);
        inst.lfo.rate = clampRate(inst.lfo.rate, inst.lfo.maxRateHz);
        return true;
    }
    if (strcmp(name, "amplitude") == 0)
    {
        inst.lfo.amplitude = value;
        return true;
    }
    if (strcmp(name, "phase") == 0)
    {
        inst.lfo.phaseOffsetDeg = value;
        return true;
    }
    if (strcmp(name, "zeroPoint") == 0)
    {
        inst.lfo.zeroPoint = value;
        return true;
    }
    if (strcmp(name, "bipolar") == 0)
    {
        inst.lfo.bipolar = value >= 0.5f;
        return true;
    }
    return false;
}

inline void parsePreset(cJSON *p, WidgetParams &out)
{
    out.waveType = waveTypeFromString(presetparser::getString(p, "waveType", "sine"));
    out.lfoMaxRate = clampRate(presetparser::getFloat(p, "maxRate", 50.0f));
    out.lfoRate = clampRate(presetparser::getFloat(p, "rate", 2.0f), out.lfoMaxRate);
    out.lfoAmplitude = presetparser::getFloat(p, "amplitude", 10.0f);
    out.lfoPhaseDeg = presetparser::getFloat(p, "phase", 0.0f);
    out.lfoZeroPointFixed = presetparser::getBool(p, "zeroPointFixed", false);
    out.lfoZeroPoint = presetparser::getFloat(p, "zeroPoint", 5.0f);
    out.lfoBipolar = presetparser::getBool(p, "bipolar", false);
    out.lfoOneShot = presetparser::getBool(p, "oneShot", false);
    out.lfoGlobal = presetparser::getBool(p, "global", false);
}

} // namespace nodes::lfo

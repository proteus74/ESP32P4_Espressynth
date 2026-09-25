#pragma once

#include <cmath>
#include <cstddef>
#include <cstring>

#include "../CvConventions.h"
#include "../NodeInstance.h"
#include "../PresetParser.h"
#include "../SynthConfig.h"
#include "../Wavetable.h"

// Free-running LFO (sine/square/triangle/sawtooth/noise, or a morphable
// wavetable - see Wavetable.h) with reset/one-shot/global modes. LfoState (value member) stays in NodeState.h.
namespace nodes::lfo
{

constexpr float kMaxRateHz = 500.0f;

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

// Global mode, called by Graph::renderBlock once per block for a voiced Lfo
// widget with several voice instances (`voices` = voice 0..count-1). Voice 0
// is the master whose output every other voice mirrors (see render()). The
// shared LFO belongs to the whole chord, not to voice 0's own note:
//  - it (re)starts (phase 0, One Shot re-armed) when the first note begins,
//    i.e. when "any voice sounds" rises, and when the OR over all voices'
//    reset inputs rises (so a gate patched to reset only fires for the first
//    note, later notes find it already high);
//  - while any voice still sounds it keeps running / holding its One Shot end
//    state for everybody, no matter which voice was played first or released;
//  - once every voice is silent it stops, and the next note starts it anew.
// Returns true when the master must be rendered this block. When global is
// off it just clears the flags and returns false (each voice runs alone).
inline bool globalUpdate(NodeInstance *voices, uint16_t count, bool anySounding)
{
    using namespace noderender;
    auto &s = voices[0].lfo;
    const bool modeFed = controlPortConnected(voices[0], 2) ||
                         (voices[0].audioConnectedPortMask & static_cast<uint8_t>(1u << 2)) != 0;
    const bool globalActive = modeFed ? (portSample(voices[0], 2, 0) >= cv::kGateThresholdVolts) : s.global;
    s.globalManaged = globalActive;
    if (!globalActive)
    {
        s.globalRunning = false;
        s.globalResetPrev = false;
        return false;
    }
    bool resetOr = false;
    for (uint16_t v = 0; v < count && !resetOr; ++v)
    {
        const NodeInstance &voice = voices[v];
        const bool resetFed = controlPortConnected(voice, 0) ||
                              (voice.audioConnectedPortMask & static_cast<uint8_t>(1u << 0)) != 0;
        resetOr = resetFed && portSample(voice, 0, 0) >= cv::kGateThresholdVolts;
    }
    const bool restart = (resetOr && !s.globalResetPrev) || (anySounding && !s.globalRunning);
    s.globalResetPrev = resetOr;
    s.globalRunning = anySounding;
    if (restart)
    {
        s.phaseGen.resetPhase();
        s.linearPos = 0.0f;
        const bool oneShotFed = controlPortConnected(voices[0], 1) ||
                                (voices[0].audioConnectedPortMask & static_cast<uint8_t>(1u << 1)) != 0;
        const bool oneShotOn = oneShotFed ? (portSample(voices[0], 1, 0) >= cv::kGateThresholdVolts) : s.oneShot;
        if (oneShotOn)
        {
            s.oneShotDone = false;
            s.oneShotAccum = 0.0f;
        }
    }
    return anySounding;
}

inline void render(NodeInstance &inst, size_t frames)
{
    using namespace noderender;
    const float kSampleRate = static_cast<float>(synth::kSampleRate);
    auto &s = inst.lfo;
    // "global" (port 2, control-only - see isControlOnlyInput): same
    // dual-source-fed check as reset/oneShot below. >=kGateThresholdVolts
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
    const float clampLo = 0.0f;
    const float clampHi = cv::kMaxVolts;
    // "zeroPoint" (port 3, control-only - see isControlOnlyInput): same
    // dual-source-fed check as reset/oneShot/global above. A connected CV
    // SUBSTITUTES the "Fixed" Offset slider's own zeroPoint outright (same
    // pattern as Oscillator's tuneVolts), read once per block like every
    // other control-only input here - a center-shift has no need for
    // per-sample resolution. Not wired: falls back to the widget's own
    // Offset slider, unchanged from before.
    const bool zeroPointFed = controlPortConnected(inst, 3) || (inst.audioConnectedPortMask & static_cast<uint8_t>(1u << 3)) != 0;
    const float zeroPointVolts = zeroPointFed ? portSample(inst, 3, 0) : s.zeroPoint;
    const float center = s.zeroPointFixed ? zeroPointVolts : halfSwing;
    // Degrees -> 0..1 fraction, added on every phaseGen evaluation (see
    // PhaseOsc::currentSample/process) - so a reset
    // (phaseGen.resetPhase() below) automatically lands exactly at this
    // offset, without resetPhase() itself needing to know about it.
    const float phaseOffset = s.phaseOffsetDeg / 360.0f;
    // Reset (port 0) and One Shot (port 1), both control-rate: a port counts
    // as "fed" via either a ControlRoute (Knob/Button by paramName -
    // controlConnectedPortMask) or a genuine DSP edge (wired directly from
    // another node's output - audioConnectedPortMask), same dual-source
    // check as every other control-only input (see Oscillator's
    // pitchCv/tune). When One Shot's port is unconnected, its UI toggle
    // (s.oneShot) is the fallback - matches ReverbLight's bypass pattern.
    // In global mode the reset input is consumed by globalUpdate() (OR over
    // all voices), not per instance.
    const bool resetFed = !s.globalManaged &&
                          (controlPortConnected(inst, 0) || (inst.audioConnectedPortMask & static_cast<uint8_t>(1u << 0)) != 0);
    const float resetVal = resetFed ? portSample(inst, 0, 0) : 0.0f;
    const bool resetAbove = resetVal >= cv::kGateThresholdVolts;
    const bool resetEdge = resetFed && resetAbove && !s.resetPrev;
    if (resetFed)
    {
        s.resetPrev = resetAbove;
    }
    const bool oneShotFed = controlPortConnected(inst, 1) || (inst.audioConnectedPortMask & static_cast<uint8_t>(1u << 1)) != 0;
    const bool oneShotOn = oneShotFed ? (portSample(inst, 1, 0) >= cv::kGateThresholdVolts) : s.oneShot;
    if (resetEdge)
    {
        s.phaseGen.resetPhase();
        s.linearPos = 0.0f;
        if (oneShotOn)
        {
            // Re-arm: play exactly one fresh cycle from this edge.
            s.oneShotDone = false;
            s.oneShotAccum = 0.0f;
        }
    }
    // Wavetable mode: same phase accumulator (so reset/global restarts and
    // the phase offset behave exactly like the classic shapes), but the raw
    // -1..+1 value comes from the widget's table bank, crossfaded by
    // "morph". Morph (port 4) is a real audio-rate input: a connected LFO/
    // envelope sweeps through the bank per sample; unconnected, the manual
    // Morph slider (s.morph) is used. One Shot plays exactly one cycle per
    // reset edge and then holds the phase - morph keeps acting on the held
    // position, so a frozen One-Shot LFO can still be scanned through the
    // bank.
    if (s.wavetable && s.wavetableBank != nullptr)
    {
        const wavetable::Bank &bank = *s.wavetableBank;
        const bool morphFed = controlPortConnected(inst, 4) ||
                              (inst.audioConnectedPortMask & static_cast<uint8_t>(1u << 4)) != 0;
        const uint32_t offsetInt = PhaseOsc::toPhaseInt(phaseOffset);
        const float step = s.rate / kSampleRate;
        const uint32_t inc = PhaseOsc::toPhaseInt(step);
        const float manualMorph = s.morph * cv::kInvMaxVolts;
        for (size_t i = 0; i < frames; ++i)
        {
            const float morph = morphFed ? portSample(inst, 4, i) * cv::kInvMaxVolts : manualMorph;
            const float raw = wavetable::sample(bank, s.phaseGen.phaseInt() + offsetInt, morph);
            inst.outL[i] = clampf(center + raw * halfSwing, clampLo, clampHi);
            if (oneShotOn && s.oneShotDone)
                continue;
            if (oneShotOn)
            {
                s.oneShotAccum += step;
                if (s.oneShotAccum >= 1.0f)
                {
                    // Land exactly on the cycle start (the reset position)
                    // instead of one increment past it.
                    s.phaseGen.resetPhase();
                    s.oneShotDone = true;
                    s.oneShotAccum = 0.0f;
                    continue;
                }
            }
            s.phaseGen.advancePhaseOnly(inc, 1);
        }
        return;
    }
    // LinearUp/LinearDown: free-running they repeat like a sawtooth (wrapping
    // ramp); with One Shot they run exactly one non-wrapping ramp from the
    // reset edge and then HOLD the end value (a Sawtooth would fall back to
    // its start instead). Tracked in linearPos because phaseGen wraps and
    // can't represent "finished".
    if (isLinearWave(s.wave))
    {
        const float step = s.rate / kSampleRate;
        const bool up = s.wave == WaveType::LinearUp;
        for (size_t i = 0; i < frames; ++i)
        {
            float pos = s.linearPos + phaseOffset;
            pos = oneShotOn ? clampf(pos, 0.0f, 1.0f) : pos - floorf(pos);
            const float raw = up ? 2.0f * pos - 1.0f : 1.0f - 2.0f * pos;
            inst.outL[i] = clampf(center + raw * halfSwing, clampLo, clampHi);
            if (oneShotOn && s.oneShotDone)
                continue;
            s.linearPos += step;
            if (oneShotOn)
            {
                if (s.linearPos > 1.0f)
                    s.linearPos = 1.0f;
                s.oneShotAccum += step;
                if (s.oneShotAccum >= 1.0f)
                {
                    s.oneShotDone = true;
                    s.oneShotAccum = 0.0f;
                }
            }
            else
            {
                s.linearPos -= floorf(s.linearPos);
            }
        }
        return;
    }
    // One Shot forces the per-sample path unconditionally (it must freeze
    // mid-block the instant a full cycle completes - the fast interpolated
    // Sine path below only samples phaseGen at the two block boundaries and
    // can't represent that). Any reset edge also forces per-sample even for
    // a Sine wave, so the interpolated path can't "swallow" a reset pulse
    // landing mid-block.
    if (s.wave != WaveType::Sine || resetEdge || oneShotOn)
    {
        for (size_t i = 0; i < frames; ++i)
        {
            if (oneShotOn && s.oneShotDone)
            {
                // Frozen (from an earlier sample this block, or an earlier
                // block entirely): the phase isn't advancing anymore, so
                // the waveform value is now constant until the next reset
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

inline float clampf01Volts(float volts)
{
    if (!(volts >= 0.0f)) return 0.0f;
    return volts > cv::kMaxVolts ? cv::kMaxVolts : volts;
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
    inst.lfo.oneShot = p.lfoOneShot;
    inst.lfo.global = p.lfoGlobal;
    inst.lfo.wavetable = p.lfoWavetableMode;
    inst.lfo.morph = clampf01Volts(p.lfoMorph);
    // Every voice instance holds its own reference to the one shared bank.
    inst.lfo.wavetableBank = wavetable::acquire(p.lfoWavetable);
}

inline void destroy(NodeInstance &inst)
{
    wavetable::release(inst.lfo.wavetableBank);
    inst.lfo.wavetableBank = nullptr;
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
    if (strcmp(name, "morph") == 0)
    {
        inst.lfo.morph = clampf01Volts(value);
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
    out.lfoOneShot = presetparser::getBool(p, "oneShot", false);
    out.lfoGlobal = presetparser::getBool(p, "global", false);
    out.lfoWavetableMode = presetparser::getBool(p, "wavetable", false);
    out.lfoMorph = clampf01Volts(presetparser::getFloat(p, "morph", 0.0f));
    // Bank is parsed whenever tables are stored, even with the mode off, so
    // a live "wavetable": true toggle (Graph::applyLiveParams) finds it.
    out.lfoWavetable = wavetable::parse(p != nullptr ? cJSON_GetObjectItemCaseSensitive(p, "wavetables") : nullptr,
                                        out.lfoWavetableMode);
}

} // namespace nodes::lfo

#pragma once

#include <cmath>
#include <cstddef>
#include <cstring>

#include "../CvConventions.h"
#include "../NodeInstance.h"
#include "../PresetParser.h"
#include "../SynthConfig.h"
#include "../Wavetable.h"
#include "LfoNode.h"

// Wavetable modulator: an LFO whose waveform is a morphable bank of
// self-drawn single-cycle tables (see Wavetable.h). Deliberately its own
// widget rather than an LFO mode, so ordinary LFO presets never carry the
// table payload. Reuses LfoState (phase accumulator, rate/maxRate, gain,
// reset/one-shot/global bookkeeping) and nodes::lfo::globalUpdate() - the
// port layout for those three matches the LFO's (reset 0, oneShot 1,
// global 2), so Graph::renderBlock drives both types the same way.
//
// Ports: reset(0), oneShot(1), global(2) - control-only like the LFO's -
// and morph(3), a genuine audio-rate input (0 V = first table, 10 V = last).
// Rate and Gain are named params (Knob/CV via ControlRoute), same as LFO.
// Output "value": 0..gain volts (center gain/2), like the LFO in auto
// zero-point mode.
namespace nodes::wavetable
{

constexpr int kPortReset = 0;
constexpr int kPortOneShot = 1;
constexpr int kPortGlobal = 2;
constexpr int kPortMorph = 3;

inline bool portFed(const NodeInstance &inst, int port)
{
    return noderender::controlPortConnected(inst, port) ||
           (inst.audioConnectedPortMask & synth::portBit(port)) != 0;
}

inline void render(NodeInstance &inst, size_t frames)
{
    using namespace noderender;
    const float kSampleRate = static_cast<float>(synth::kSampleRate);
    auto &s = inst.lfo;

    // Global: every voice but the master mirrors voice 0 (see LfoNode.h).
    const bool globalActive = portFed(inst, kPortGlobal) ? (portSample(inst, kPortGlobal, 0) >= cv::kGateThresholdVolts)
                                                         : s.global;
    if (globalActive && inst.primary != nullptr && inst.primary != &inst)
    {
        memcpy(inst.outL, inst.primary->outL, frames * sizeof(float));
        return;
    }
    if (s.wavetableBank == nullptr)
    {
        memset(inst.outL, 0, frames * sizeof(float));
        return;
    }

    // Reset (rising edge) and One Shot - identical semantics to the LFO: in
    // global mode the reset input is consumed by lfo::globalUpdate().
    const bool resetFed = !s.globalManaged && portFed(inst, kPortReset);
    const bool resetAbove = resetFed && portSample(inst, kPortReset, 0) >= cv::kGateThresholdVolts;
    const bool resetEdge = resetFed && resetAbove && !s.resetPrev;
    if (resetFed)
        s.resetPrev = resetAbove;
    const bool oneShotOn = portFed(inst, kPortOneShot) ? (portSample(inst, kPortOneShot, 0) >= cv::kGateThresholdVolts)
                                                       : s.oneShot;
    if (resetEdge)
    {
        s.phaseGen.resetPhase();
        if (oneShotOn)
        {
            s.oneShotDone = false;
            s.oneShotAccum = 0.0f;
        }
    }

    const ::wavetable::Bank &bank = *s.wavetableBank;
    lfo::applyPortOverrides(inst, false);
    const float halfSwing = s.gain * 0.5f;
    const bool morphFed = portFed(inst, kPortMorph);
    const float manualMorph = s.morph * cv::kInvMaxVolts;
    const float step = s.rate / kSampleRate;
    const uint32_t inc = PhaseOsc::toPhaseInt(step);
    for (size_t i = 0; i < frames; ++i)
    {
        const float morph = morphFed ? portSample(inst, kPortMorph, i) * cv::kInvMaxVolts : manualMorph;
        const float raw = ::wavetable::sample(bank, s.phaseGen.phaseInt(), morph);
        inst.outL[i] = clampf(halfSwing + raw * halfSwing, 0.0f, cv::kMaxVolts);
        if (oneShotOn && s.oneShotDone)
            continue; // held at the cycle start - morph still acts on it
        if (oneShotOn)
        {
            s.oneShotAccum += step;
            if (s.oneShotAccum >= 1.0f)
            {
                // Land exactly on the cycle start instead of one step past it.
                s.phaseGen.resetPhase();
                s.oneShotDone = true;
                s.oneShotAccum = 0.0f;
                continue;
            }
        }
        s.phaseGen.advancePhaseOnly(inc, 1);
    }
}

inline void build(NodeInstance &inst, const WidgetParams &p)
{
    inst.lfo.maxRateHz = lfo::clampRate(p.lfoMaxRate);
    inst.lfo.rate = lfo::clampRate(p.lfoRate, inst.lfo.maxRateHz);
    inst.lfo.gain = p.lfoGain;
    inst.lfo.oneShot = p.lfoOneShot;
    inst.lfo.global = p.lfoGlobal;
    inst.lfo.morph = p.wavetableMorph;
    // Every voice instance holds its own reference to the one shared bank.
    inst.lfo.wavetableBank = ::wavetable::acquire(p.wavetableBank);
}

inline void destroy(NodeInstance &inst)
{
    ::wavetable::release(inst.lfo.wavetableBank);
    inst.lfo.wavetableBank = nullptr;
}

inline bool setParam(NodeInstance &inst, const char *name, float value)
{
    if (name == nullptr)
        return false;
    if (strcmp(name, "morph") == 0)
    {
        inst.lfo.morph = lfo::clampVolts(value);
        return true;
    }
    // rate / maxRate / gain: same fields and clamping as the LFO.
    if (strcmp(name, "rate") == 0 || strcmp(name, "maxRate") == 0 || strcmp(name, "gain") == 0)
        return lfo::setParam(inst, name, value);
    return false;
}

inline void parsePreset(cJSON *p, WidgetParams &out)
{
    out.lfoMaxRate = lfo::clampRate(presetparser::getFloat(p, "maxRate", 50.0f));
    out.lfoRate = lfo::clampRate(presetparser::getFloat(p, "rate", 2.0f), out.lfoMaxRate);
    out.lfoGain = lfo::clampVolts(presetparser::getFloat(p, "gain", 10.0f));
    out.lfoOneShot = presetparser::getBool(p, "oneShot", false);
    out.lfoGlobal = presetparser::getBool(p, "global", false);
    out.wavetableMorph = lfo::clampVolts(presetparser::getFloat(p, "morph", 0.0f));
    out.wavetableBank =
        ::wavetable::parse(p != nullptr ? cJSON_GetObjectItemCaseSensitive(p, "wavetables") : nullptr, true);
}

} // namespace nodes::wavetable

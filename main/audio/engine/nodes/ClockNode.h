#pragma once

#include <cmath>
#include <cstddef>
#include <cstring>

#include "../CvConventions.h"
#include "../MasterClock.h"
#include "../NodeInstance.h"
#include "../PresetParser.h"
#include "../SynthConfig.h"

// Pulse output driven by the single device-wide synth::masterClock()
// (MasterClock.h) - mirrors main.js's Clock widget (fireClockPulse/
// restartClockTimer), but sample-accurate instead of per-JS setInterval.
// Each instance only owns its own pulse width (pulseMs) and phase
// accumulator (ClockState in NodeState.h); tempo comes from the shared
// master clock, so every Clock node in the graph stays in sync with every
// other one.
namespace nodes::clock
{

inline void render(NodeInstance &inst, size_t frames)
{
    auto &s = inst.clock;
    const auto &mc = synth::masterClock();
    float bpm = mc.roundBpm ? roundf(mc.bpm) : mc.bpm;
    const float periodSec = bpm > 0.0f ? 60.0f / bpm : 0.0f;
    const float pulseSec = s.pulseMs * 0.001f;
    const float dtSec = 1.0f / static_cast<float>(synth::kSampleRate);
    for (size_t i = 0; i < frames; ++i)
    {
        if (periodSec > 0.0f)
        {
            s.phaseSec += dtSec;
            if (s.phaseSec >= periodSec)
            {
                s.phaseSec -= periodSec; // retrigger, phase preserved (no drift)
            }
            s.pulseActive = s.phaseSec < pulseSec;
        }
        else
        {
            s.pulseActive = false;
        }
        inst.outL[i] = s.pulseActive ? cv::kGateOnVolts : cv::kGateOffVolts;
    }
}

inline void build(NodeInstance &inst, const WidgetParams &p)
{
    inst.clock.pulseMs = p.clockPulseMs;
}

inline bool setParam(NodeInstance &inst, const char *name, float value)
{
    if (name == nullptr)
    {
        return false;
    }
    if (strcmp(name, "pulseMs") == 0)
    {
        inst.clock.pulseMs = value;
        return true;
    }
    return false;
}

inline void parsePreset(cJSON *p, WidgetParams &out)
{
    out.clockPulseMs = presetparser::getFloat(p, "pulseMs", 100.0f);
}

} // namespace nodes::clock

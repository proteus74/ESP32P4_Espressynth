#pragma once

#include <cstddef>
#include <cstring>

#include "../CvConventions.h"
#include "../NodeInstance.h"
#include "../PresetParser.h"
#include "../SynthConfig.h"

// Free-running BPM pulse generator - mirrors main.js's Clock widget
// (fireClockPulse/restartClockTimer), but sample-accurate instead of per-JS
// setInterval. cvVolts (0-10V, "tempo" input) scales linearly between
// minBpm/maxBpm, phaseSec is the phase accumulator (0..period duration)
// instead of a FreeRTOS timer - hence does not drift and needs no periodic
// task switch. ClockState (value member) stays in NodeState.h.
namespace nodes::clock
{

inline void render(NodeInstance &inst, size_t frames)
{
    using namespace noderender;
    // "tempo" (Port-Index 0) is a real, control-only audio-rate port (see
    // PortTables.h), read once per block (a tempo jump produces no
    // amplitude click, an immediately continuing pulse phase accumulator is
    // enough).
    auto &s = inst.clock;
    if ((inst.audioConnectedPortMask & static_cast<uint8_t>(1u << 0)) != 0)
    {
        s.cvVolts = portSample(inst, 0, 0);
    }
    const float bpm = s.minBpm + (s.cvVolts / cv::kMaxVolts) * (s.maxBpm - s.minBpm);
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
    inst.clock.cvVolts = p.clockCvVolts;
    inst.clock.minBpm = p.clockMinBpm;
    inst.clock.maxBpm = p.clockMaxBpm;
    inst.clock.pulseMs = p.clockPulseMs;
}

inline bool setParam(NodeInstance &inst, const char *name, float value)
{
    if (name == nullptr)
    {
        return false;
    }
    if (strcmp(name, "cvVolts") == 0)
    {
        inst.clock.cvVolts = value;
        return true;
    }
    if (strcmp(name, "minBpm") == 0)
    {
        inst.clock.minBpm = value;
        return true;
    }
    if (strcmp(name, "maxBpm") == 0)
    {
        inst.clock.maxBpm = value;
        return true;
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
    out.clockCvVolts = presetparser::getFloat(p, "cvVolts", 0.0f);
    out.clockMinBpm = presetparser::getFloat(p, "minBpm", 0.0f);
    out.clockMaxBpm = presetparser::getFloat(p, "maxBpm", 240.0f);
    out.clockPulseMs = presetparser::getFloat(p, "pulseMs", 100.0f);
}

} // namespace nodes::clock

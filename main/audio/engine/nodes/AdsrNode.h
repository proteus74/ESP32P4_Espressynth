#pragma once

#include <cstddef>
#include <cstring>

#include "esp_heap_caps.h"

#include "../CvConventions.h"
#include "../DspPrimitives.h"
#include "../NodeInstance.h"
#include "../PresetParser.h"
#include "../SynthConfig.h"

// Retrigger-safe linear ADSR envelope (see DspPrimitives.h's RampScheduler).
// Heap-allocated per instance (see NodeInstance.h's pointer-member comment),
// so state, render, build, destroy and live-param-set all live together
// here. Every note-on gate always anchors the attack ramp at the envelope's
// current value (RampScheduler::gate()), never hard-resets to silence.
namespace nodes::adsr
{

struct AdsrState
{
    RampScheduler ramp;
    bool gatePrev = false;
    // Velocity dynamics: scales only the envelope peak (see render()),
    // never the attack/decay/sustain/release timing itself. Unset/no
    // velocity input connected -> stays at 1 (full level, as before this
    // field was introduced).
    float velocity = 1.0f;
    // "att"/"dec"/"sus"/"rel" are real, but control-only audio-rate ports
    // (see PortTables.h) - only used when the respective port is wired via
    // a DSP edge (see render() below); *MaxSeconds/*Log mirror main.js's
    // ADSR settings dialog (Sustain has no own setting there - pure
    // 0-10V->0..1 linear mapping). The unwired case remains the already
    // existing direct attack/decay/sustain/release-in-seconds delivery via
    // ParamDispatch (see ramp.configure()).
    float attackMaxSeconds = 4.0f;
    bool attackLog = false;
    float decayMaxSeconds = 4.0f;
    bool decayLog = false;
    float releaseMaxSeconds = 4.0f;
    bool releaseLog = false;
};

inline void render(NodeInstance &inst, size_t frames)
{
    using namespace noderender;
    const float kSampleRate = static_cast<float>(synth::kSampleRate);
    auto &s = *inst.adsr;
    // "att"/"dec"/"sus"/"rel" (Port-Index 0-3) are real, but control-only
    // audio-rate ports (see PortTables.h) - a connected "control-safe"
    // source (e.g. Lfo) is read ONCE PER BLOCK, not per sample: configure()
    // only sets the target times/levels for the next ramp segment, no
    // amplitude sample jump is possible, nobody needs full per-sample
    // resolution here. Not wired: the already existing direct
    // attack/decay/sustain/release-in-seconds delivery via ParamDispatch
    // (ramp.configure() there) remains.
    const bool attFed = (inst.audioConnectedPortMask & static_cast<uint8_t>(1u << 0)) != 0;
    const bool decFed = (inst.audioConnectedPortMask & static_cast<uint8_t>(1u << 1)) != 0;
    const bool susFed = (inst.audioConnectedPortMask & static_cast<uint8_t>(1u << 2)) != 0;
    const bool relFed = (inst.audioConnectedPortMask & static_cast<uint8_t>(1u << 3)) != 0;
    if (attFed || decFed || susFed || relFed)
    {
        const float attackSec = attFed
            ? cv::cvToEnvelopeSeconds(portSample(inst, 0, 0), s.attackMaxSeconds, s.attackLog)
            : s.ramp.attackSec();
        const float decaySec = decFed
            ? cv::cvToEnvelopeSeconds(portSample(inst, 1, 0), s.decayMaxSeconds, s.decayLog)
            : s.ramp.decaySec();
        const float sustainLevel = susFed
            ? (portSample(inst, 2, 0) / cv::kMaxVolts)
            : s.ramp.sustainLevel();
        const float releaseSec = relFed
            ? cv::cvToEnvelopeSeconds(portSample(inst, 3, 0), s.releaseMaxSeconds, s.releaseLog)
            : s.ramp.releaseSec();
        s.ramp.configure(attackSec, decaySec, sustainLevel, releaseSec, kSampleRate);
    }
    // "out" is 0-10V CV like every other CV-carrying port (see
    // CvConventions.h) - the envelope itself (0..1 internally, via
    // RampScheduler) times velocity times kMaxVolts. Consistent with
    // Oscillator's "amplitude" CV input: an unmodified ADSR->
    // Oscillator.amplitude wire sounds identical to before this change
    // (envelope*10 in, /10 back out via vcaGain), it's just genuinely
    // 0-10V CV now so any other CV-mappable target can consume it too.
    // Velocity*kMaxVolts is loop-invariant - hoisted once instead of
    // recomputed every sample (plan section 5, ADSR row: "Velocity*10
    // einmal vorberechnen").
    const float velocityScale = s.velocity * cv::kMaxVolts;
    // Cleared unconditionally up front so a Ramp-aware downstream consumer
    // (see NodeInstance.h's rampPortMask) never reads a stale true from a
    // previous block, regardless of which branch below actually runs.
    inst.outIsRamp = false;
    if (s.ramp.isHolding())
    {
        // Sustain plateau or fully idle - process() would just keep
        // returning the same current() value sample after sample. Compute
        // it once and fill the block directly (plan section 5: "Sustain
        // und Idle als konstante Ausgänge").
        const float held = s.ramp.current() * velocityScale;
        for (size_t i = 0; i < frames; ++i)
        {
            inst.outL[i] = held;
        }
        return;
    }
    float rampStart = 0.0f, rampInc = 0.0f;
    if (s.ramp.wholeBlockRamp(frames, rampStart, rampInc))
    {
        inst.outIsRamp = true;
        inst.outRamp.start = rampStart * velocityScale;
        inst.outRamp.inc = rampInc * velocityScale;
    }
    for (size_t i = 0; i < frames; ++i)
    {
        inst.outL[i] = s.ramp.process() * velocityScale;
    }
}

inline void build(NodeInstance &inst, const WidgetParams &p)
{
    inst.adsr->ramp.configure(p.attack, p.decay, p.sustain, p.release, static_cast<float>(synth::kSampleRate));
    inst.adsr->attackMaxSeconds = p.attackMaxSeconds;
    inst.adsr->decayMaxSeconds = p.decayMaxSeconds;
    inst.adsr->releaseMaxSeconds = p.releaseMaxSeconds;
    inst.adsr->attackLog = p.attackLog;
    inst.adsr->decayLog = p.decayLog;
    inst.adsr->releaseLog = p.releaseLog;
}

inline void destroy(NodeInstance &inst)
{
    if (inst.adsr != nullptr)
    {
        heap_caps_free(inst.adsr);
        inst.adsr = nullptr;
    }
}

inline bool setParam(NodeInstance &inst, const char *name, float value)
{
    if (name == nullptr)
    {
        return false;
    }
    auto &ramp = inst.adsr->ramp;
    const float sr = static_cast<float>(synth::kSampleRate);
    if (strcmp(name, "attack") == 0)
    {
        ramp.configure(value, ramp.decaySec(), ramp.sustainLevel(), ramp.releaseSec(), sr);
        return true;
    }
    if (strcmp(name, "decay") == 0)
    {
        ramp.configure(ramp.attackSec(), value, ramp.sustainLevel(), ramp.releaseSec(), sr);
        return true;
    }
    if (strcmp(name, "sustain") == 0)
    {
        ramp.configure(ramp.attackSec(), ramp.decaySec(), value, ramp.releaseSec(), sr);
        return true;
    }
    if (strcmp(name, "release") == 0)
    {
        ramp.configure(ramp.attackSec(), ramp.decaySec(), ramp.sustainLevel(), value, sr);
        return true;
    }
    if (strcmp(name, "velocity") == 0)
    {
        inst.adsr->velocity = value;
        return true;
    }
    if (strcmp(name, "attackMaxSeconds") == 0)
    {
        inst.adsr->attackMaxSeconds = value;
        return true;
    }
    if (strcmp(name, "decayMaxSeconds") == 0)
    {
        inst.adsr->decayMaxSeconds = value;
        return true;
    }
    if (strcmp(name, "releaseMaxSeconds") == 0)
    {
        inst.adsr->releaseMaxSeconds = value;
        return true;
    }
    // "attackCurve"/"decayCurve"/"releaseCurve" are strings ("linear"/
    // "log"), same pattern as Filter's "cutoffCurve" - see Graph.cpp's
    // applyLiveParams.
    return false;
}

inline void parsePreset(cJSON *p, WidgetParams &out)
{
    out.attack = presetparser::getFloat(p, "attack", 0.1f);
    out.decay = presetparser::getFloat(p, "decay", 0.1f);
    out.sustain = presetparser::getFloat(p, "sustain", 0.7f);
    out.release = presetparser::getFloat(p, "release", 0.2f);
    out.attackMaxSeconds = presetparser::getFloat(p, "attackMaxSeconds", 4.0f);
    out.decayMaxSeconds = presetparser::getFloat(p, "decayMaxSeconds", 4.0f);
    out.releaseMaxSeconds = presetparser::getFloat(p, "releaseMaxSeconds", 4.0f);
    out.attackLog = strcmp(presetparser::getString(p, "attackCurve", "linear"), "log") == 0;
    out.decayLog = strcmp(presetparser::getString(p, "decayCurve", "linear"), "log") == 0;
    out.releaseLog = strcmp(presetparser::getString(p, "releaseCurve", "linear"), "log") == 0;
}

} // namespace nodes::adsr

using nodes::adsr::AdsrState;

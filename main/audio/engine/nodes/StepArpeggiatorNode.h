#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "esp_heap_caps.h"

#include "../CvConventions.h"
#include "../MasterClock.h"
#include "../NodeInstance.h"
#include "../PresetParser.h"
#include "../SynthConfig.h"

// Monophonic 8/16-step CV sequencer with arpeggiator-style direction modes.
// External analog clock/gate drive playback (no MIDI, no voicing - this
// widget is never voiced, see GraphBuilder's classifyVoiced: nothing here
// traces back to a >1-voice MidiIn). "cv" is a genuine sample-and-hold
// output (see CvConventions.h's convention comment): it only changes at the
// instant of a qualifying clock/gate edge and is held constant otherwise,
// exactly like a real hardware step sequencer's pitch-CV output. Heap-
// allocated per instance (see NodeInstance.h's pointer-member comment) -
// stepVolts[16] alone is as large as most whole state structs in
// NodeState.h.
namespace nodes::steparp
{

constexpr int kClockPort = 0;
constexpr int kGatePort = 1;
constexpr int kPitchPort = 2;
constexpr int kDirectionPort = 3;
constexpr int kStep1Port = 4;
constexpr int kStepPortCount = 16;
constexpr int kPulseTimePort = 20;
// Direct BPM tempo input - see render()'s clockPortWired
// branch. Only ever consulted while "clock" (port 0) itself is unwired.
constexpr int kRatePort = 21;
// "pattern" / "switchAtEnd" - control-only, same semantics as the Drum
// Sequencer's ports of the same name (see DrumSequencerNode.h).
constexpr int kPatternPort = 22;
constexpr int kSwitchAtEndPort = 23;
constexpr uint8_t kNoPendingPattern = 0xFF;
static_assert(kSwitchAtEndPort < static_cast<int>(synth::kMaxAudioInPorts), "fits port budget");
static_assert(kStep1Port + kStepPortCount <= static_cast<int>(synth::kMaxAudioInPorts), "fits port budget");
static_assert(kPulseTimePort < static_cast<int>(synth::kMaxAudioInPorts), "fits port budget");
static_assert(kRatePort < static_cast<int>(synth::kMaxAudioInPorts), "fits port budget");

enum class Direction : uint8_t
{
    Forward,
    Backward,
    PingPong,
    Random,
};

// direction (port 3): 0-10V split into four fixed bands - see documentation/
// cv-step-arpeggiator-plan.md.
inline Direction directionFromCv(float volts)
{
    if (volts < 2.5f)
        return Direction::Forward;
    if (volts < 5.0f)
        return Direction::Backward;
    if (volts < 7.5f)
        return Direction::PingPong;
    return Direction::Random;
}

// xorshift32 - same algorithm as PhaseOsc::nextNoise() (DspPrimitives.h),
// kept as this node's own independent state (Random step selection is
// unrelated to oscillator noise, no shared RNG utility exists).
inline uint32_t nextRandom(uint32_t &state)
{
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

struct StepArpState
{
    // Local per-step knob values (0-10V pitch-CV, see CvConventions.h) -
    // only consulted while the matching "stepN" port has neither a genuine
    // audio-rate edge nor a ControlRoute targeting it (see stepCv() below) -
    // a wired step port always wins outright, never blended.
    // One row per pattern; activePattern is the one played (and edited).
    float stepVolts[synth::kStepArpMaxPatterns][kStepPortCount] = {
        {5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5}};
    uint8_t patternCount = 1;
    uint8_t activePattern = 0;
    uint8_t manualPatternSelect = 0;            // local slider / live param
    uint8_t pendingPattern = kNoPendingPattern; // armed by Switch at End
    bool switchAtEnd = false;
    uint8_t randomStepsInCycle = 0;             // Random has no natural end - count stepCount steps
    uint8_t stepCount = 16; // 8 or 16 - the "8/16 steps" switch
    // gateOut pulse width - local knob fallback, already in its final real-
    // world unit (ms), same "local value pre-mapped, wired CV mapped fresh
    // every time" split as Delay's delayTimeMs/timeMin/timeMax (DelayNode.h).
    // pulseTimeMinSec/MaxSec are the settings-dialog range that "pulseTime"
    // (port 20, 0-10V) maps to via cv::cvToRange whenever it IS wired - see
    // render() below.
    float pulseMs = 10.0f;
    float pulseTimeMinSec = 0.001f, pulseTimeMaxSec = 2.0f;

    // Settings-dialog cap on a step knob's pitch-CV OFFSET (see triggerStep's
    // pitchWired branch) in semitones, e.g. 12 to keep steps within one
    // octave. The offset is also always rounded to a whole semitone before
    // being added to Pitch, so the summed CV never lands off the semitone
    // grid (no out-of-tune notes from a knob not sitting exactly on a
    // semitone's voltage). Only the offset is capped/rounded - a step knob
    // used as an ABSOLUTE pitch-CV (pitch unwired) is untouched.
    float maxSemitoneOffset = 60.0f;

    uint8_t currentStep = 0;     // step that will be PLAYED on the next qualifying clock edge
    bool pingPongForward = true; // Ping-Pong sub-direction, persists across edges
    bool clockPrev = false;
    bool gatePrev = false;
    uint32_t pulseSamplesRemaining = 0;
    // Settings-dialog toggle: when a new step triggers while the PREVIOUS
    // step's gateOut pulse is still running (pulse width longer than the
    // clock interval), true forces a brief LOW gap first so a downstream
    // envelope generator sees a genuine falling+rising edge and retriggers;
    // false (default, previous behavior) just extends/replaces the pulse
    // outright, staying continuously high - a legato feel, no retrigger.
    // See render()'s retriggerGapRemaining/pendingPulseSamples handling.
    bool retriggerGateOut = false;
    bool skipOnC0 = false;
    bool snapToSemitones = false;
    uint32_t retriggerGapRemaining = 0; // samples left in the forced-low gap, 0 = none pending
    uint32_t pendingPulseSamples = 0;   // pulse length to apply once the gap above reaches 0
    uint32_t rngState = 0x9e3779b9u; // never 0 - a 0 state would make xorshift32 stick at 0
    // Sample-and-held CV output (see file comment) - updated only at a
    // qualifying edge, otherwise carried forward unchanged sample to sample,
    // including all the way through Gate Low (plan: "bei Gate Low bleibt die
    // zuletzt ausgegebene CV erhalten").
    float heldCv = 5.0f;

    // Legacy preset parameters. Generated steps now use direct BPM or
    // Master Clock; the private phase accumulator resyncs on Gate.
    float internalClockCvVolts = 0.0f;
    float internalClockMaxBpm = 120.0f;
    bool internalClockRoundBpm = false;
    float internalPhaseSec = 0.0f;
};

// Advances `current` to the following step per `dir`, given `stepCount`
// active steps. Ping-Pong keeps its own forward/backward sub-state
// (`pingPongForward`) so the endpoints are never played twice in a row (see
// the plan's direction table). Random may repeat immediately by design.
inline uint8_t nextStepIndex(uint8_t current, uint8_t stepCount, Direction dir, bool &pingPongForward,
                              uint32_t &rngState)
{
    if (stepCount <= 1)
    {
        return 0;
    }
    switch (dir)
    {
    case Direction::Forward:
    {
        const uint8_t next = static_cast<uint8_t>(current + 1);
        return next >= stepCount ? uint8_t{0} : next;
    }
    case Direction::Backward:
        return current == 0 ? static_cast<uint8_t>(stepCount - 1) : static_cast<uint8_t>(current - 1);
    case Direction::PingPong:
        if (pingPongForward)
        {
            if (current >= stepCount - 1)
            {
                pingPongForward = false;
                return static_cast<uint8_t>(current - 1);
            }
            return static_cast<uint8_t>(current + 1);
        }
        if (current == 0)
        {
            pingPongForward = true;
            return static_cast<uint8_t>(current + 1);
        }
        return static_cast<uint8_t>(current - 1);
    case Direction::Random:
    default:
        return static_cast<uint8_t>(nextRandom(rngState) % stepCount);
    }
}

// Splits 0-10V into `count` equal bands - same as the Drum Sequencer's
// patternIndexFromCv, so both widgets pick patterns identically.
inline uint8_t patternIndexFromCv(float volts, uint8_t count)
{
    if (count <= 1) return 0;
    int idx = static_cast<int>(volts / (cv::kMaxVolts / static_cast<float>(count)));
    if (idx < 0) idx = 0;
    if (idx > count - 1) idx = count - 1;
    return static_cast<uint8_t>(idx);
}

inline uint8_t clampPatternIndex(int idx, uint8_t count)
{
    if (count == 0 || idx < 0) return 0;
    return static_cast<uint8_t>(idx > count - 1 ? count - 1 : idx);
}

// Moves s.currentStep on per `dir` and, when that completes one pass through
// the sequence, applies a pattern switch armed by Switch at End. A pass ends
// when playback is back at step 1 (Forward, Backward and Ping-Pong all start
// there after a Gate reset); Random ends after stepCount steps.
inline void advanceStep(StepArpState &s, Direction dir)
{
    s.currentStep = nextStepIndex(s.currentStep, s.stepCount, dir, s.pingPongForward, s.rngState);
    bool cycleEnd;
    if (dir == Direction::Random)
    {
        cycleEnd = ++s.randomStepsInCycle >= s.stepCount;
        if (cycleEnd) s.randomStepsInCycle = 0;
    }
    else
    {
        cycleEnd = s.currentStep == 0;
    }
    if (cycleEnd && s.pendingPattern != kNoPendingPattern)
    {
        s.activePattern = s.pendingPattern;
        s.pendingPattern = kNoPendingPattern;
    }
}

// Resolves one step's pitch-CV: a genuine audio-rate edge OR a ControlRoute
// (e.g. a Knob cabled straight into "stepN") both win outright over the
// local knob - portSample() already implements exactly that fallback chain
// (inPorts else controlValue), so the only extra check needed here is
// "is ANY connection at all present", else fall back to the widget's own
// stepVolts[idx].
inline float stepCv(const NodeInstance &inst, const StepArpState &s, uint8_t idx, size_t i)
{
    const int port = kStep1Port + idx;
    const bool wired = ((inst.audioConnectedPortMask | inst.controlConnectedPortMask) & synth::portBit(port)) != 0;
    const float raw = wired ? noderender::portSample(inst, port, i) : s.stepVolts[s.activePattern][idx];
    if (!s.snapToSemitones) return raw;
    const float volts = std::isfinite(raw) ? fmaxf(0.0f, fminf(cv::kMaxVolts, raw)) : 0.0f;
    const bool pitchWired = ((inst.audioConnectedPortMask | inst.controlConnectedPortMask) & synth::portBit(kPitchPort)) != 0;
    if (pitchWired && s.maxSemitoneOffset > 0.0f)
    {
        const float center = cv::kMaxVolts * 0.5f;
        // floor(x + 0.5) matches browser Math.round(), including negative ties.
        const float semitones = floorf((volts - center) * s.maxSemitoneOffset / center + 0.5f);
        return fmaxf(0.0f, fminf(cv::kMaxVolts, center + semitones * center / s.maxSemitoneOffset));
    }
    return floorf(volts * 12.0f + 0.5f) / 12.0f;
}

// Plays s.currentStep (sample-and-holds its CV + starts the gateOut pulse,
// honoring retriggerGateOut if a previous pulse is still running - see
// StepArpState's comment), then advances s.currentStep to the FOLLOWING
// step per the current "direction" CV - the play-then-advance model this
// whole node follows (see the file comment). Shared by both trigger
// sources: a qualifying clock edge, AND (per the plan/user request) the
// Gate's own rising edge, which plays step 1 immediately instead of
// waiting for the first clock edge afterward.
inline void triggerStep(NodeInstance &inst, StepArpState &s, size_t i, float sampleRate, uint32_t retriggerGapSamples)
{
    using namespace noderender;
    const float stepVolts = stepCv(inst, s, s.currentStep, i);
    if (s.skipOnC0 && stepVolts <= 0.0f)
    {
        // A rest consumes this step, preserves held CV and stops any overlapping gate.
        s.pulseSamplesRemaining = 0;
        s.pendingPulseSamples = 0;
        s.retriggerGapRemaining = 0;
        advanceStep(s, directionFromCv(portSample(inst, kDirectionPort, i)));
        return;
    }
    // "pitch" (Port-Index 2) - when wired, a step's own knob value is a
    // +/-N-semitone OFFSET from its own 5V center, added on top of the
    // incoming Pitch CV (0 Halbtoene at the knob's 5V default + a 5V Pitch
    // signal must produce exactly 5V out, not 10V - see main.js's
    // updateStepArpKnobVisual for the matching display). Unwired, the knob
    // stays the note's ABSOLUTE pitch-CV, unchanged from before.
    const bool pitchWired = ((inst.audioConnectedPortMask | inst.controlConnectedPortMask) & synth::portBit(kPitchPort)) != 0;
    if (pitchWired)
    {
        // maxSemitoneOffset RESCALES the knob's full travel, it does not just
        // clamp it - 0V (knob fully down) always means -maxSemitoneOffset and
        // 10V (fully up) always means +maxSemitoneOffset, so the whole turn
        // stays usable no matter how small the cap is set. Rounded to a
        // whole semitone before adding to Pitch, so the resulting CV always
        // sums to an exact semitone - never a fraction that would play out
        // of tune.
        const float normalized = (stepVolts - cv::kMaxVolts * 0.5f) / (cv::kMaxVolts * 0.5f); // -1..+1
        const float offsetVolts = roundf(normalized * s.maxSemitoneOffset) / 12.0f;
        s.heldCv = portSample(inst, kPitchPort, i) + offsetVolts;
    }
    else
    {
        s.heldCv = stepVolts;
    }

    // "pulseTime" (Port-Index 20) - real, control-only audio-rate CV port
    // (see PortTables.h), same wired-CV-or-manual pattern as Delay's "time"
    // (DelayNode.h): wired, the raw 0-10V is mapped fresh via
    // cv::cvToRange every trigger; unwired, s.pulseMs (the local knob,
    // already in ms) is used directly.
    const bool pulseTimeWired = (inst.audioConnectedPortMask & synth::portBit(kPulseTimePort)) != 0;
    const float pulseSec = pulseTimeWired
        ? cv::cvToRange(portSample(inst, kPulseTimePort, i), s.pulseTimeMinSec, s.pulseTimeMaxSec, false)
        : s.pulseMs * 0.001f;
    const uint32_t rawPulseSamples = static_cast<uint32_t>(pulseSec * sampleRate + 0.5f);
    const uint32_t pulseSamples = rawPulseSamples < 1 ? 1 : rawPulseSamples;
    if (s.retriggerGateOut && s.pulseSamplesRemaining > 0)
    {
        // Previous step's pulse is still running (pulse width longer than
        // the clock interval) - force a brief LOW gap first instead of
        // just extending it, so a downstream envelope generator sees a
        // genuine falling+rising edge and retriggers for this new step
        // (see StepArpState's comment and the gap countdown in render()).
        s.pulseSamplesRemaining = 0;
        s.pendingPulseSamples = pulseSamples;
        s.retriggerGapRemaining = retriggerGapSamples;
    }
    else
    {
        s.pulseSamplesRemaining = pulseSamples;
        s.retriggerGapRemaining = 0;
    }

    advanceStep(s, directionFromCv(portSample(inst, kDirectionPort, i)));
}

inline void render(NodeInstance &inst, size_t frames)
{
    using namespace noderender;
    auto &s = *inst.stepArp;

    // Step-count/direction changes are UI-driven, not audio-rate - clamping
    // once per block (not per sample) is enough to keep currentStep safely
    // inside the active range after e.g. a 16->8 step-count switch (plan:
    // "wird der aktuelle Index sicher auf den aktiven Bereich normalisiert").
    if (s.currentStep >= s.stepCount)
    {
        s.currentStep = static_cast<uint8_t>(s.stepCount - 1);
    }

    // "pattern"/"switchAtEnd" are control-only - read once per block, same
    // rules as DrumSequencerNode.h's render().
    const bool patternWired = ((inst.audioConnectedPortMask | inst.controlConnectedPortMask) &
                               synth::portBit(kPatternPort)) != 0;
    const uint8_t requestedPattern = patternWired
        ? patternIndexFromCv(portSample(inst, kPatternPort, 0), s.patternCount)
        : clampPatternIndex(s.manualPatternSelect, s.patternCount);
    const bool switchAtEndFed = ((inst.audioConnectedPortMask | inst.controlConnectedPortMask) &
                                 synth::portBit(kSwitchAtEndPort)) != 0;
    const bool switchAtEndOn = switchAtEndFed
        ? portSample(inst, kSwitchAtEndPort, 0) >= cv::kGateThresholdVolts
        : s.switchAtEnd;
    if (requestedPattern != s.activePattern && requestedPattern != s.pendingPattern)
    {
        // Quantize only while running (gate high) - otherwise there is no
        // upcoming sequence end to wait for.
        if (switchAtEndOn && s.gatePrev)
            s.pendingPattern = requestedPattern;
        else
        {
            s.activePattern = requestedPattern;
            s.pendingPattern = kNoPendingPattern;
        }
    }

    const float sampleRate = static_cast<float>(synth::kSampleRate);
    const float dtSec = 1.0f / sampleRate;
    // ~1ms forced-low gap for retriggerGateOut (see StepArpState's comment) -
    // long enough that any reasonable downstream envelope actually observes
    // the falling edge, short enough to be inaudible/imperceptible as a gap
    // in the sequence itself. Computed once per block, not per sample.
    const uint32_t retriggerGapSamples = static_cast<uint32_t>(0.001f * sampleRate + 0.5f);

    // "clock" (Port-Index 0) - a genuine external cable always wins outright
    // over the internal fallback generator below (plan: "wenn nichts
    // angeschlossen ist am Clockport, dann soll [die eigene Clock]
    // genommen werden"). Read once per block, same block-rate reasoning as
    // ClockNode.h's own "tempo"/"sync" ports.
    const bool clockPortWired = ((inst.audioConnectedPortMask | inst.controlConnectedPortMask) &
                                  synth::portBit(kClockPort)) != 0;

    // BPM accepts the connected tempo directly; Master Clock is the fallback.
    const bool rateWired = ((inst.audioConnectedPortMask | inst.controlConnectedPortMask) & synth::portBit(kRatePort)) != 0;
    const auto &mc = synth::masterClock();
    const float tempo = rateWired ? portSample(inst, kRatePort, 0) : (mc.roundBpm ? roundf(mc.bpm) : mc.bpm);
    float internalBpm = std::isfinite(tempo) ? fmaxf(0.0f, tempo) : 0.0f;
    // A step is a 16th note (4 steps per beat), matching the drum sequencer.
    const float internalPeriodSec = internalBpm > 0.0f ? 15.0f / internalBpm : 0.0f;

    for (size_t i = 0; i < frames; ++i)
    {
        // Retrigger-gap countdown - must run before this sample's trigger
        // check below, so a gap STARTED by that check (right below, same
        // sample) never immediately expires again within the same sample;
        // it only ever expires on a LATER sample's turn through this loop.
        if (s.retriggerGapRemaining > 0)
        {
            --s.retriggerGapRemaining;
            if (s.retriggerGapRemaining == 0)
            {
                s.pulseSamplesRemaining = s.pendingPulseSamples;
            }
        }

        const bool gateNow = portSample(inst, kGatePort, i) >= cv::kGateThresholdVolts;
        bool triggeredByGate = false;
        if (gateNow && !s.gatePrev)
        {
            // Run/Reset rising edge: jump to step 1 and play it immediately
            // - CV, gateOut pulse, and the advance to the FOLLOWING step -
            // instead of waiting for the first clock edge afterward (that
            // first real clock edge now plays step 2, not step 1 again).
            // pulseSamplesRemaining is cleared first so triggerStep's own
            // retriggerGateOut check (any pulse still running from before
            // this reset) never fires here - a reset always starts the new
            // pulse directly, same as DxEnvelope/AdsrNode's own gate-reset.
            s.currentStep = 0;
            s.pingPongForward = true;
            s.randomStepsInCycle = 0;
            s.pulseSamplesRemaining = 0;
            s.retriggerGapRemaining = 0;
            // The internal clock always resyncs to Gate, whether or not it
            // is actually driving playback right now - if the external
            // cable is later unplugged, the fallback generator starts
            // clean instead of resuming mid-cycle from a stale phase.
            s.internalPhaseSec = 0.0f;
            triggerStep(inst, s, i, sampleRate, retriggerGapSamples);
            triggeredByGate = true;
        }
        else if (!gateNow && s.gatePrev)
        {
            // Gate falling edge: end any still-running gateOut pulse (and
            // cancel any pending retrigger gap) immediately, not just on
            // the next rising edge - a step's trigger must never outlast
            // the Gate that armed it.
            s.pulseSamplesRemaining = 0;
            s.retriggerGapRemaining = 0;
            // No running sequence left to quantize against.
            if (s.pendingPattern != kNoPendingPattern)
            {
                s.activePattern = s.pendingPattern;
                s.pendingPattern = kNoPendingPattern;
            }
        }
        s.gatePrev = gateNow;

        bool clockRising;
        if (clockPortWired)
        {
            const bool clockNow = portSample(inst, kClockPort, i) >= cv::kGateThresholdVolts;
            clockRising = clockNow && !s.clockPrev;
            s.clockPrev = clockNow;
        }
        else
        {
            // Internal fallback generator - same phase-accumulator math as
            // nodes::clock::render(), just private to this instance and
            // reset to 0 on every Gate rising edge above instead of a
            // separate "sync" input.
            clockRising = false;
            if (internalPeriodSec > 0.0f)
            {
                s.internalPhaseSec += dtSec;
                if (s.internalPhaseSec >= internalPeriodSec)
                {
                    s.internalPhaseSec -= internalPeriodSec; // retrigger, phase preserved (no drift)
                    clockRising = true;
                }
            }
            s.clockPrev = false; // keep clean if "clock" gets wired again later
        }

        // Clock edges while Gate is Low are ignored outright, never queued
        // for later (plan: "werden ignoriert und nicht nachgeholt"). A clock
        // edge landing on the SAME sample that Gate's own rising edge just
        // reset+played step 1 is ALSO ignored - without this, an external
        // clock that happens to pulse the instant Gate rises (e.g. wired to
        // the same source, or to Clock's own "Sync" input alongside Gate)
        // double-advances, playing step 2 instead of step 1 on that first
        // sample.
        if (!triggeredByGate && clockRising && gateNow)
        {
            triggerStep(inst, s, i, sampleRate, retriggerGapSamples);
        }

        inst.outL[i] = s.heldCv;
        inst.outR[i] = s.pulseSamplesRemaining > 0 ? cv::kGateOnVolts : cv::kGateOffVolts;
        if (s.pulseSamplesRemaining > 0)
        {
            --s.pulseSamplesRemaining;
        }
    }
}

inline void build(NodeInstance &inst, const WidgetParams &p)
{
    auto &s = *inst.stepArp;
    s.patternCount = p.stepArpPatternCount < 1 ? 1
        : (p.stepArpPatternCount > synth::kStepArpMaxPatterns ? static_cast<uint8_t>(synth::kStepArpMaxPatterns)
                                                              : p.stepArpPatternCount);
    memcpy(s.stepVolts, p.stepArpPatterns, sizeof(s.stepVolts));
    s.manualPatternSelect = clampPatternIndex(p.stepArpActivePatternIndex, s.patternCount);
    s.activePattern = s.manualPatternSelect;
    s.pendingPattern = kNoPendingPattern;
    s.switchAtEnd = p.stepArpSwitchAtEnd;
    s.randomStepsInCycle = 0;
    s.stepCount = p.stepArpStepCount <= 8 ? 8 : 16;
    s.pulseMs = p.stepArpPulseMs;
    s.pulseTimeMinSec = p.stepArpPulseTimeMinSec;
    s.pulseTimeMaxSec = p.stepArpPulseTimeMaxSec;
    s.maxSemitoneOffset = p.stepArpMaxSemitoneOffset;
    s.retriggerGateOut = p.stepArpRetriggerGateOut;
    s.skipOnC0 = p.stepArpSkipOnC0;
    s.snapToSemitones = p.stepArpSnapToSemitones;
    s.internalClockCvVolts = p.stepArpClockCvVolts;
    s.internalClockMaxBpm = p.stepArpClockMaxBpm;
    s.internalClockRoundBpm = p.stepArpClockRoundBpm;
    // "direction" (Port-Index 3) has no dedicated state field of its own -
    // unlike the sixteen step knobs, it needs no drawn-knob visual state,
    // just a plain local scalar - inst.controlValue[] (the same fallback
    // portSample() already consults for any unwired port) is enough, same
    // as a ControlRoute-delivered value would use. Only the INITIAL value
    // needs setting here; live updates go through setParam() below.
    inst.controlValue[kDirectionPort] = p.stepArpDirectionVolts;
    s.currentStep = 0;
    s.pingPongForward = true;
    s.clockPrev = false;
    s.gatePrev = false;
    s.pulseSamplesRemaining = 0;
    s.retriggerGapRemaining = 0;
    s.pendingPulseSamples = 0;
    s.rngState = 0x9e3779b9u;
    const float firstStep = s.stepVolts[s.activePattern][0];
    s.heldCv = s.skipOnC0 && firstStep <= 0.0f ? 5.0f : firstStep;
    s.internalPhaseSec = 0.0f;
}

inline void destroy(NodeInstance &inst)
{
    if (inst.stepArp != nullptr)
    {
        heap_caps_free(inst.stepArp);
        inst.stepArp = nullptr;
    }
}

namespace
{
// Matches "step1".."step16" (1-based in the name, 0-based return), else -1.
// Same one-or-two-digit parsing as PortTables.h's StepArpeggiator case -
// kept as a separate copy here (not shared), matching this codebase's
// per-node-file self-containment convention (see ArithmeticNodes.h).
inline int stepFieldIndex(const char *name)
{
    if (name == nullptr || strncmp(name, "step", 4) != 0)
    {
        return -1;
    }
    const char *numPart = name + 4;
    if (numPart[0] < '1' || numPart[0] > '9')
    {
        return -1;
    }
    int n = numPart[0] - '0';
    if (numPart[1] != '\0')
    {
        if (numPart[1] < '0' || numPart[1] > '9' || numPart[2] != '\0')
        {
            return -1;
        }
        n = n * 10 + (numPart[1] - '0');
    }
    return (n >= 1 && n <= kStepPortCount) ? (n - 1) : -1;
}
} // namespace

inline bool setParam(NodeInstance &inst, const char *name, float value)
{
    if (name == nullptr)
    {
        return false;
    }
    const int idx = stepFieldIndex(name);
    if (idx >= 0)
    {
        // Step edits target the pattern the widget shows, which the
        // frontend keeps in lockstep with activePattern.
        inst.stepArp->stepVolts[inst.stepArp->activePattern][idx] = value;
        return true;
    }
    if (strcmp(name, "activePatternIndex") == 0)
    {
        auto &s = *inst.stepArp;
        const uint8_t pattern = clampPatternIndex(static_cast<int>(value + 0.5f), s.patternCount);
        s.manualPatternSelect = pattern;
        if (s.switchAtEnd && s.gatePrev)
            s.pendingPattern = pattern;
        else
        {
            s.activePattern = pattern;
            s.pendingPattern = kNoPendingPattern;
        }
        return true;
    }
    if (strcmp(name, "switchAtEnd") == 0)
    {
        inst.stepArp->switchAtEnd = value >= 0.5f;
        return true;
    }
    if (strcmp(name, "pulseMs") == 0)
    {
        inst.stepArp->pulseMs = value;
        return true;
    }
    if (strcmp(name, "pulseTimeMinSec") == 0)
    {
        inst.stepArp->pulseTimeMinSec = value;
        return true;
    }
    if (strcmp(name, "pulseTimeMaxSec") == 0)
    {
        inst.stepArp->pulseTimeMaxSec = value;
        return true;
    }
    if (strcmp(name, "maxSemitoneOffset") == 0)
    {
        inst.stepArp->maxSemitoneOffset = value;
        return true;
    }
    if (strcmp(name, "retriggerGateOut") == 0)
    {
        inst.stepArp->retriggerGateOut = value >= 0.5f;
        return true;
    }
    if (strcmp(name, "skipOnC0") == 0)
    {
        inst.stepArp->skipOnC0 = value >= 0.5f;
        return true;
    }
    if (strcmp(name, "snapToSemitones") == 0)
    {
        inst.stepArp->snapToSemitones = value >= 0.5f;
        return true;
    }
    if (strcmp(name, "direction") == 0)
    {
        // Local "direction" slider - see build()'s comment. A genuine cable
        // (audioConnectedPortMask) still wins outright: portSample() only
        // ever falls back to controlValue[] when inPorts[] is null.
        inst.controlValue[kDirectionPort] = value;
        return true;
    }
    if (strcmp(name, "clockCvVolts") == 0)
    {
        inst.stepArp->internalClockCvVolts = value;
        return true;
    }
    if (strcmp(name, "clockMaxBpm") == 0)
    {
        inst.stepArp->internalClockMaxBpm = value;
        return true;
    }
    if (strcmp(name, "clockRoundBpm") == 0)
    {
        inst.stepArp->internalClockRoundBpm = value >= 0.5f;
        return true;
    }
    return false;
}

inline void parsePreset(cJSON *p, WidgetParams &out)
{
    for (auto &pattern : out.stepArpPatterns)
        for (float &volts : pattern) volts = 5.0f;
    cJSON *patterns = p == nullptr ? nullptr : cJSON_GetObjectItemCaseSensitive(p, "patterns");
    uint8_t patternCount = 0;
    cJSON *patternItem = nullptr;
    cJSON_ArrayForEach(patternItem, patterns)
    {
        if (patternCount >= synth::kStepArpMaxPatterns) break;
        presetparser::getFloatArray(patternItem, "steps", out.stepArpPatterns[patternCount],
                                    static_cast<uint8_t>(kStepPortCount));
        ++patternCount;
    }
    if (patternCount == 0)
    {
        // Presets saved before multi-pattern support have one top-level "steps" array.
        presetparser::getFloatArray(p, "steps", out.stepArpPatterns[0], static_cast<uint8_t>(kStepPortCount));
        patternCount = 1;
    }
    out.stepArpPatternCount = patternCount;
    out.stepArpActivePatternIndex = clampPatternIndex(presetparser::getInt(p, "activePatternIndex", 0), patternCount);
    out.stepArpSwitchAtEnd = presetparser::getBool(p, "switchAtEnd", false);
    const int stepCount = presetparser::getInt(p, "stepCount", 16);
    out.stepArpStepCount = static_cast<uint8_t>(stepCount <= 8 ? 8 : 16);
    out.stepArpPulseMs = presetparser::getFloat(p, "pulseMs", 10.0f);
    out.stepArpPulseTimeMinSec = presetparser::getFloat(p, "pulseTimeMinSec", 0.001f);
    out.stepArpPulseTimeMaxSec = presetparser::getFloat(p, "pulseTimeMaxSec", 2.0f);
    out.stepArpMaxSemitoneOffset = presetparser::getFloat(p, "maxSemitoneOffset", 60.0f);
    out.stepArpDirectionVolts = presetparser::getFloat(p, "direction", 0.0f);
    out.stepArpRetriggerGateOut = presetparser::getBool(p, "retriggerGateOut", false);
    out.stepArpSkipOnC0 = presetparser::getBool(p, "skipOnC0", false);
    out.stepArpSnapToSemitones = presetparser::getBool(p, "snapToSemitones", false);
    out.stepArpClockCvVolts = presetparser::getFloat(p, "clockCvVolts", 0.0f);
    out.stepArpClockMaxBpm = presetparser::getFloat(p, "clockMaxBpm", 120.0f);
    out.stepArpClockRoundBpm = presetparser::getBool(p, "clockRoundBpm", false);
}

} // namespace nodes::steparp

using nodes::steparp::StepArpState;

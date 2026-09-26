#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "esp_heap_caps.h"

#include "../CvConventions.h"
#include "../NodeInstance.h"
#include "../PresetParser.h"
#include "../SynthConfig.h"

namespace nodes::drumsequencer
{

constexpr int kClockPort = 0;
constexpr int kGatePort = 1;
constexpr int kPatternPort = 2;
constexpr int kRatePort = 3;
constexpr int kSwitchAtEndPort = 4;

inline uint8_t clampStepCount(int value)
{
    if (value <= 4) return 4;
    if (value <= 8) return 8;
    if (value <= 12) return 12;
    return 16;
}

inline uint8_t clampTrackCount(int value)
{
    return static_cast<uint8_t>(value < 1 ? 1 : (value > 8 ? 8 : value));
}

inline float clampPulseMs(float value)
{
    if (!std::isfinite(value)) return 50.0f;
    return value < 0.1f ? 0.1f : (value > 60000.0f ? 60000.0f : value);
}

inline void applyTrackPulseMs(cJSON *values, float *out)
{
    if (!cJSON_IsArray(values)) return;
    for (size_t track = 0; track < synth::kDrumSequencerMaxTracks; ++track)
    {
        const cJSON *item = cJSON_GetArrayItem(values, static_cast<int>(track));
        if (cJSON_IsNumber(item)) out[track] = clampPulseMs(static_cast<float>(item->valuedouble));
    }
}

inline float clampMaxBpm(float value)
{
    return value < 0.1f ? 0.1f : (value > 10000.0f ? 10000.0f : value);
}

inline uint8_t clampPatternIndex(int idx, int count)
{
    if (count <= 0) return 0;
    if (idx < 0) return 0;
    return static_cast<uint8_t>(idx > count - 1 ? count - 1 : idx);
}

// Splits 0-10V into `count` equal bands, same idea as MultiplexerNode.h's
// selectFromCv - 10V itself must still resolve to the last pattern.
inline int patternIndexFromCv(float volts, int count)
{
    if (count <= 1) return 0;
    int idx = static_cast<int>(volts / (cv::kMaxVolts / static_cast<float>(count)));
    if (idx < 0) idx = 0;
    if (idx > count - 1) idx = count - 1;
    return idx;
}

inline bool cellActive(const uint8_t *bits, size_t index)
{
    return (bits[index >> 3] & static_cast<uint8_t>(1u << (index & 7u))) != 0;
}

inline void setCellActive(uint8_t *bits, size_t index, bool active)
{
    const uint8_t mask = static_cast<uint8_t>(1u << (index & 7u));
    if (active)
        bits[index >> 3] |= mask;
    else
        bits[index >> 3] &= static_cast<uint8_t>(~mask);
}

inline uint8_t encodeVelocity(float volts)
{
    const float clamped = volts < 0.0f ? 0.0f : (volts > cv::kMaxVolts ? cv::kMaxVolts : volts);
    return static_cast<uint8_t>(lroundf((clamped / cv::kMaxVolts) * 255.0f));
}

inline float decodeVelocity(uint8_t value)
{
    return (static_cast<float>(value) / 255.0f) * cv::kMaxVolts;
}

inline void decodeCellValue(float encoded, bool &active, uint8_t &velocity)
{
    active = encoded >= 0.0f;
    const float volts = active ? encoded : -encoded - 1.0f;
    velocity = encodeVelocity(volts);
}

inline int indexedCell(const char *name)
{
    if (name == nullptr || strncmp(name, "cell", 4) != 0 || name[4] == '\0') return -1;
    int value = 0;
    for (const char *p = name + 4; *p != '\0'; ++p)
    {
        if (*p < '0' || *p > '9') return -1;
        value = value * 10 + (*p - '0');
        if (value >= static_cast<int>(synth::kDrumSequencerMaxCells)) return -1;
    }
    return value;
}

inline void triggerStep(DrumSequencerState &s, uint16_t totalSteps)
{
    const DrumSequencerPattern &pattern = s.patterns[s.activePattern];
    for (uint8_t track = 0; track < s.trackCount; ++track)
    {
        const size_t cell = static_cast<size_t>(track) * totalSteps + s.currentStep;
        if (!cellActive(pattern.active, cell)) continue;
        s.heldVelocity[track] = decodeVelocity(pattern.velocity[cell]);
        const uint32_t pulseSamples = static_cast<uint32_t>(s.trackPulseMs[track] * 0.001f * synth::kSampleRate + 0.5f);
        const uint32_t nextPulse = pulseSamples > 0 ? pulseSamples : 1;
        if (s.pulseSamplesRemaining[track] > 0)
        {
            s.pulseSamplesRemaining[track] = 0;
            s.pendingPulseSamples[track] = nextPulse;
            s.retriggerGapRemaining[track] = 1;
        }
        else
        {
            s.pulseSamplesRemaining[track] = nextPulse;
        }
    }
    ++s.currentStep;
    if (s.currentStep >= totalSteps)
    {
        s.currentStep = 0;
        if (s.pendingPattern != 0xFF)
        {
            s.activePattern = s.pendingPattern;
            s.pendingPattern = 0xFF;
        }
    }
}

inline void render(NodeInstance &inst, size_t frames)
{
    using namespace noderender;
    auto &s = *inst.drumSequencer;
    const uint16_t totalSteps = s.stepCount;
    if (s.currentStep >= totalSteps) s.currentStep = 0;

    const bool clockWired = ((inst.audioConnectedPortMask | inst.controlConnectedPortMask) &
                             synth::portBit(kClockPort)) != 0;
    const bool rateWired = ((inst.audioConnectedPortMask | inst.controlConnectedPortMask) &
                            synth::portBit(kRatePort)) != 0;
    const float rawRateCv = rateWired ? portSample(inst, kRatePort, 0) : s.internalClockCvVolts;

    // "pattern" is control-only (see PortTables::isControlOnlyInput) - read once
    // per render block, not per sample, matching Multiplexer's "select".
    const bool patternWired = controlPortConnected(inst, kPatternPort) ||
                               (inst.audioConnectedPortMask & synth::portBit(kPatternPort)) != 0;
    const uint8_t requestedPattern = patternWired
        ? clampPatternIndex(patternIndexFromCv(portSample(inst, kPatternPort, 0), s.patternCount), s.patternCount)
        : clampPatternIndex(s.manualPatternSelect, s.patternCount);

    // "switchAtEnd" is control-only, same dual-source-fed check as LFO's
    // oneShot/global: a connected CV substitutes the widget's own toggle.
    const bool switchAtEndFed = controlPortConnected(inst, kSwitchAtEndPort) ||
                                 (inst.audioConnectedPortMask & synth::portBit(kSwitchAtEndPort)) != 0;
    const bool switchAtEndOn = switchAtEndFed ? (portSample(inst, kSwitchAtEndPort, 0) >= cv::kGateThresholdVolts)
                                               : s.switchAtEnd;

    if (requestedPattern != s.activePattern && requestedPattern != s.pendingPattern)
    {
        // Quantizing only makes sense while the sequencer is actually
        // running (gate high) - with no gate, there is no upcoming step
        // boundary to wait for, so switch right away.
        if (switchAtEndOn && s.gatePrev)
            s.pendingPattern = requestedPattern;
        else
        {
            s.activePattern = requestedPattern;
            s.pendingPattern = 0xFF;
        }
    }
    const float rateCv = rawRateCv < 0.0f ? 0.0f : (rawRateCv > cv::kMaxVolts ? cv::kMaxVolts : rawRateCv);
    float bpm = (rateCv / cv::kMaxVolts) * s.internalClockMaxBpm;
    if (s.internalClockRoundBpm) bpm = roundf(bpm);
    // A step is a 16th note (4 steps per beat), not one beat itself - a
    // 16-step pattern is one 4/4 bar. 120 BPM must trigger 8 steps/sec.
    const float periodSec = bpm > 0.0f ? 15.0f / bpm : 0.0f;
    const float dtSec = 1.0f / static_cast<float>(synth::kSampleRate);

    for (size_t i = 0; i < frames; ++i)
    {
        for (uint8_t track = 0; track < s.trackCount; ++track)
        {
            if (s.retriggerGapRemaining[track] > 0)
            {
                --s.retriggerGapRemaining[track];
                if (s.retriggerGapRemaining[track] == 0)
                    s.pulseSamplesRemaining[track] = s.pendingPulseSamples[track];
            }
        }

        const bool gateNow = portSample(inst, kGatePort, i) >= cv::kGateThresholdVolts;
        bool triggeredByGate = false;
        if (gateNow && !s.gatePrev)
        {
            s.currentStep = 0;
            s.internalPhaseSec = 0.0f;
            memset(s.pulseSamplesRemaining, 0, sizeof(s.pulseSamplesRemaining));
            memset(s.pendingPulseSamples, 0, sizeof(s.pendingPulseSamples));
            memset(s.retriggerGapRemaining, 0, sizeof(s.retriggerGapRemaining));
            triggerStep(s, totalSteps);
            triggeredByGate = true;
        }
        else if (!gateNow && s.gatePrev)
        {
            memset(s.pulseSamplesRemaining, 0, sizeof(s.pulseSamplesRemaining));
            memset(s.pendingPulseSamples, 0, sizeof(s.pendingPulseSamples));
            memset(s.retriggerGapRemaining, 0, sizeof(s.retriggerGapRemaining));
            // Gate dropped while a quantized switch was still armed - there is
            // no more running playback to quantize against, so resolve it now.
            if (s.pendingPattern != 0xFF)
            {
                s.activePattern = s.pendingPattern;
                s.pendingPattern = 0xFF;
            }
        }
        s.gatePrev = gateNow;

        bool rising = false;
        if (clockWired)
        {
            const bool now = portSample(inst, kClockPort, i) >= cv::kGateThresholdVolts;
            rising = now && !s.clockPrev;
            s.clockPrev = now;
        }
        else
        {
            if (periodSec > 0.0f)
            {
                s.internalPhaseSec += dtSec;
                if (s.internalPhaseSec >= periodSec)
                {
                    s.internalPhaseSec -= periodSec;
                    rising = true;
                }
            }
            s.clockPrev = false;
        }

        if (!triggeredByGate && rising && gateNow) triggerStep(s, totalSteps);

        for (uint8_t track = 0; track < s.trackCount; ++track)
        {
            float *trigger = outputPortBuffer(inst, static_cast<uint8_t>(track * 2));
            float *velocity = outputPortBuffer(inst, static_cast<uint8_t>(track * 2 + 1));
            if (trigger != nullptr)
                trigger[i] = s.pulseSamplesRemaining[track] > 0 ? cv::kGateOnVolts : cv::kGateOffVolts;
            if (velocity != nullptr)
                velocity[i] = s.heldVelocity[track];
            if (s.pulseSamplesRemaining[track] > 0) --s.pulseSamplesRemaining[track];
        }
    }
}

inline void build(NodeInstance &inst, const WidgetParams &p)
{
    auto &s = *inst.drumSequencer;
    s.stepCount = clampStepCount(p.drumStepCount);
    s.trackCount = clampTrackCount(p.drumTrackCount);
    s.patternCount = p.drumPatternCount < 1
        ? 1
        : (p.drumPatternCount > synth::kDrumSequencerMaxPatterns
               ? static_cast<uint8_t>(synth::kDrumSequencerMaxPatterns)
               : p.drumPatternCount);
    for (uint8_t i = 0; i < s.patternCount; ++i)
    {
        memcpy(s.patterns[i].active, p.drumPatterns[i].active, sizeof(s.patterns[i].active));
        memcpy(s.patterns[i].velocity, p.drumPatterns[i].velocity, sizeof(s.patterns[i].velocity));
    }
    s.manualPatternSelect = clampPatternIndex(p.drumActivePatternIndex, s.patternCount);
    s.activePattern = s.manualPatternSelect;
    s.pendingPattern = 0xFF;
    s.switchAtEnd = p.drumSwitchAtEnd;
    for (size_t track = 0; track < synth::kDrumSequencerMaxTracks; ++track)
        s.trackPulseMs[track] = clampPulseMs(p.drumTrackPulseMs[track]);
    s.internalClockCvVolts = p.drumClockCvVolts < 0.0f ? 0.0f : (p.drumClockCvVolts > cv::kMaxVolts ? cv::kMaxVolts : p.drumClockCvVolts);
    s.internalClockMaxBpm = clampMaxBpm(p.drumClockMaxBpm);
    s.internalClockRoundBpm = p.drumClockRoundBpm;
    s.currentStep = 0;
    s.clockPrev = false;
    s.gatePrev = false;
    s.internalPhaseSec = 0.0f;
    memset(s.heldVelocity, 0, sizeof(s.heldVelocity));
    memset(s.pulseSamplesRemaining, 0, sizeof(s.pulseSamplesRemaining));
    memset(s.pendingPulseSamples, 0, sizeof(s.pendingPulseSamples));
    memset(s.retriggerGapRemaining, 0, sizeof(s.retriggerGapRemaining));
}

inline void destroy(NodeInstance &inst)
{
    if (inst.drumSequencer == nullptr) return;
    for (float *&buffer : inst.drumSequencer->outPorts)
    {
        heap_caps_free(buffer);
        buffer = nullptr;
    }
    heap_caps_free(inst.drumSequencer);
    inst.drumSequencer = nullptr;
}

inline bool setParam(NodeInstance &inst, const char *name, float value)
{
    if (name == nullptr || inst.drumSequencer == nullptr) return false;
    auto &s = *inst.drumSequencer;
    const int cell = indexedCell(name);
    if (cell >= 0)
    {
        bool active = false;
        uint8_t velocity = 0;
        decodeCellValue(value, active, velocity);
        // Cell edits always target the pattern the widget's grid is currently
        // showing, which the frontend keeps in lockstep with activePattern.
        DrumSequencerPattern &pattern = s.patterns[s.activePattern];
        setCellActive(pattern.active, static_cast<size_t>(cell), active);
        pattern.velocity[cell] = velocity;
        return true;
    }
    if (strcmp(name, "activePatternIndex") == 0)
    {
        const uint8_t idx = clampPatternIndex(static_cast<int>(value + 0.5f), s.patternCount);
        s.manualPatternSelect = idx;
        if (s.switchAtEnd && s.gatePrev)
            s.pendingPattern = idx;
        else
        {
            s.activePattern = idx;
            s.pendingPattern = 0xFF;
        }
    }
    else if (strcmp(name, "switchAtEnd") == 0)
        s.switchAtEnd = value >= 0.5f;
    else if (strcmp(name, "pulseMs") == 0)
    {
        // Legacy live updates still set every track together.
        for (float &pulseMs : s.trackPulseMs) pulseMs = clampPulseMs(value);
    }
    else if (strcmp(name, "clockCvVolts") == 0)
        s.internalClockCvVolts = value < 0.0f ? 0.0f : (value > cv::kMaxVolts ? cv::kMaxVolts : value);
    else if (strcmp(name, "clockMaxBpm") == 0)
        s.internalClockMaxBpm = clampMaxBpm(value);
    else if (strcmp(name, "clockRoundBpm") == 0)
        s.internalClockRoundBpm = value >= 0.5f;
    else
        return false;
    return true;
}

inline void parseCellsInto(cJSON *cells, DrumSequencerPatternParams &pattern, size_t limit)
{
    if (!cJSON_IsArray(cells)) return;
    size_t index = 0;
    cJSON *item = nullptr;
    cJSON_ArrayForEach(item, cells)
    {
        if (index >= limit) break;
        const float encoded = cJSON_IsNumber(item) ? static_cast<float>(item->valuedouble) : -11.0f;
        bool active = false;
        uint8_t velocity = 255;
        decodeCellValue(encoded, active, velocity);
        setCellActive(pattern.active, index, active);
        pattern.velocity[index] = velocity;
        ++index;
    }
}

inline void parsePreset(cJSON *p, WidgetParams &out)
{
    out.drumStepCount = clampStepCount(
        presetparser::getInt(p, "stepCount", presetparser::getInt(p, "barCount", 16)));
    out.drumTrackCount = clampTrackCount(presetparser::getInt(p, "trackCount", 1));
    const size_t limit = static_cast<size_t>(out.drumStepCount) * out.drumTrackCount;

    for (uint8_t i = 0; i < synth::kDrumSequencerMaxPatterns; ++i)
    {
        memset(out.drumPatterns[i].active, 0, sizeof(out.drumPatterns[i].active));
        memset(out.drumPatterns[i].velocity, 255, sizeof(out.drumPatterns[i].velocity));
    }

    cJSON *patternsArr = p == nullptr ? nullptr : cJSON_GetObjectItemCaseSensitive(p, "patterns");
    if (cJSON_IsArray(patternsArr) && cJSON_GetArraySize(patternsArr) > 0)
    {
        uint8_t patternIdx = 0;
        cJSON *patternItem = nullptr;
        cJSON_ArrayForEach(patternItem, patternsArr)
        {
            if (patternIdx >= synth::kDrumSequencerMaxPatterns) break;
            cJSON *cells = cJSON_GetObjectItemCaseSensitive(patternItem, "cells");
            parseCellsInto(cells, out.drumPatterns[patternIdx], limit);
            ++patternIdx;
        }
        out.drumPatternCount = patternIdx > 0 ? patternIdx : 1;
    }
    else
    {
        // Backward compatibility: a preset saved before multi-pattern support
        // only has a top-level "cells" array - load it as pattern 0.
        cJSON *cells = p == nullptr ? nullptr : cJSON_GetObjectItemCaseSensitive(p, "cells");
        parseCellsInto(cells, out.drumPatterns[0], limit);
        out.drumPatternCount = 1;
    }

    out.drumActivePatternIndex = clampPatternIndex(
        presetparser::getInt(p, "activePatternIndex", 0), out.drumPatternCount);
    out.drumSwitchAtEnd = presetparser::getBool(p, "switchAtEnd", false);

    // Old presets have one shared pulse width; new presets store all tracks.
    const float legacyPulseMs = clampPulseMs(presetparser::getFloat(p, "pulseMs", 50.0f));
    for (float &pulseMs : out.drumTrackPulseMs) pulseMs = legacyPulseMs;
    applyTrackPulseMs(p == nullptr ? nullptr : cJSON_GetObjectItemCaseSensitive(p, "trackPulseMs"), out.drumTrackPulseMs);
    const float clockCv = presetparser::getFloat(p, "clockCvVolts", 0.0f);
    out.drumClockCvVolts = clockCv < 0.0f ? 0.0f : (clockCv > cv::kMaxVolts ? cv::kMaxVolts : clockCv);
    out.drumClockMaxBpm = clampMaxBpm(presetparser::getFloat(p, "clockMaxBpm", 120.0f));
    out.drumClockRoundBpm = presetparser::getBool(p, "clockRoundBpm", false);
}

} // namespace nodes::drumsequencer

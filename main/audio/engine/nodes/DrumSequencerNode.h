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
constexpr int kRatePort = 1;

inline uint8_t clampBarCount(int value)
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

inline void render(NodeInstance &inst, size_t frames)
{
    using namespace noderender;
    auto &s = *inst.drumSequencer;
    const uint16_t totalSteps = static_cast<uint16_t>(s.barCount) * synth::kDrumSequencerStepsPerBar;
    if (s.currentStep >= totalSteps) s.currentStep = 0;

    const bool clockWired = ((inst.audioConnectedPortMask | inst.controlConnectedPortMask) &
                             synth::portBit(kClockPort)) != 0;
    const bool rateWired = ((inst.audioConnectedPortMask | inst.controlConnectedPortMask) &
                            synth::portBit(kRatePort)) != 0;
    const float rateCv = rateWired ? portSample(inst, kRatePort, 0) : s.internalClockCvVolts;
    float bpm = (rateCv / cv::kMaxVolts) * s.internalClockMaxBpm;
    if (s.internalClockRoundBpm) bpm = roundf(bpm);
    const float periodSec = bpm > 0.0f ? 60.0f / bpm : 0.0f;
    const float dtSec = 1.0f / static_cast<float>(synth::kSampleRate);
    const uint32_t pulseSamples = static_cast<uint32_t>(s.pulseMs * 0.001f * synth::kSampleRate + 0.5f);

    for (size_t i = 0; i < frames; ++i)
    {
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

        if (rising)
        {
            for (uint8_t track = 0; track < s.trackCount; ++track)
            {
                const size_t cell = static_cast<size_t>(track) * totalSteps + s.currentStep;
                if (cellActive(s.active, cell))
                {
                    s.heldVelocity[track] = decodeVelocity(s.velocity[cell]);
                    s.pulseSamplesRemaining[track] = pulseSamples > 0 ? pulseSamples : 1;
                }
            }
            ++s.currentStep;
            if (s.currentStep >= totalSteps) s.currentStep = 0;
        }

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
    s.barCount = clampBarCount(p.drumBarCount);
    s.trackCount = clampTrackCount(p.drumTrackCount);
    memcpy(s.active, p.drumActive, sizeof(s.active));
    memcpy(s.velocity, p.drumVelocity, sizeof(s.velocity));
    s.pulseMs = p.drumPulseMs > 0.0f ? p.drumPulseMs : 10.0f;
    s.internalClockCvVolts = p.drumClockCvVolts;
    s.internalClockMaxBpm = p.drumClockMaxBpm > 0.0f ? p.drumClockMaxBpm : 120.0f;
    s.internalClockRoundBpm = p.drumClockRoundBpm;
    s.currentStep = 0;
    s.clockPrev = false;
    s.internalPhaseSec = 0.0f;
    memset(s.heldVelocity, 0, sizeof(s.heldVelocity));
    memset(s.pulseSamplesRemaining, 0, sizeof(s.pulseSamplesRemaining));
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
    const int cell = indexedCell(name);
    if (cell >= 0)
    {
        bool active = false;
        uint8_t velocity = 0;
        decodeCellValue(value, active, velocity);
        setCellActive(inst.drumSequencer->active, static_cast<size_t>(cell), active);
        inst.drumSequencer->velocity[cell] = velocity;
        return true;
    }
    if (strcmp(name, "pulseMs") == 0)
        inst.drumSequencer->pulseMs = value > 0.0f ? value : 10.0f;
    else if (strcmp(name, "clockCvVolts") == 0)
        inst.drumSequencer->internalClockCvVolts = value;
    else if (strcmp(name, "clockMaxBpm") == 0)
        inst.drumSequencer->internalClockMaxBpm = value;
    else if (strcmp(name, "clockRoundBpm") == 0)
        inst.drumSequencer->internalClockRoundBpm = value >= 0.5f;
    else
        return false;
    return true;
}

inline void parsePreset(cJSON *p, WidgetParams &out)
{
    out.drumBarCount = clampBarCount(presetparser::getInt(p, "barCount", 4));
    out.drumTrackCount = clampTrackCount(presetparser::getInt(p, "trackCount", 1));
    memset(out.drumActive, 0, sizeof(out.drumActive));
    memset(out.drumVelocity, 255, sizeof(out.drumVelocity));
    cJSON *cells = p == nullptr ? nullptr : cJSON_GetObjectItemCaseSensitive(p, "cells");
    if (cJSON_IsArray(cells))
    {
        const size_t limit = static_cast<size_t>(out.drumBarCount) * synth::kDrumSequencerStepsPerBar * out.drumTrackCount;
        size_t index = 0;
        cJSON *item = nullptr;
        cJSON_ArrayForEach(item, cells)
        {
            if (index >= limit) break;
            const float encoded = cJSON_IsNumber(item) ? static_cast<float>(item->valuedouble) : -11.0f;
            bool active = false;
            uint8_t velocity = 255;
            decodeCellValue(encoded, active, velocity);
            setCellActive(out.drumActive, index, active);
            out.drumVelocity[index] = velocity;
            ++index;
        }
    }
    out.drumPulseMs = presetparser::getFloat(p, "pulseMs", 10.0f);
    out.drumClockCvVolts = presetparser::getFloat(p, "clockCvVolts", 0.0f);
    out.drumClockMaxBpm = presetparser::getFloat(p, "clockMaxBpm", 120.0f);
    out.drumClockRoundBpm = presetparser::getBool(p, "clockRoundBpm", false);
}

} // namespace nodes::drumsequencer

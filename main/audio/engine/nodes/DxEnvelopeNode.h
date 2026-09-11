#pragma once

#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <cstring>

#include "esp_heap_caps.h"

#include "../CvConventions.h"
#include "../NodeInstance.h"
#include "../PresetParser.h"
#include "../SynthConfig.h"

namespace nodes::dxenvelope
{

inline float clampLevel(float value)
{
    if (value < 0.0f)
        return 0.0f;
    if (value > cv::kMaxVolts)
        return cv::kMaxVolts;
    return value;
}

inline float clampTime(float value)
{
    return value < 0.001f ? 0.001f : value;
}

class DxEnvelopeScheduler
{
public:
    void configure(const float *levels, const float *times, float sampleRate)
    {
        const float oldReleaseLevel = levels_[3];
        for (size_t i = 0; i < 4; ++i)
        {
            levels_[i] = clampLevel(levels[i]);
            times_[i] = clampTime(times[i]);
        }
        sampleRate_ = sampleRate;
        if (!initialized_ || (phase_ == Phase::Idle && segCount_ == 0 && current_ == oldReleaseLevel))
        {
            current_ = levels_[3];
        }
        initialized_ = true;
    }

    void gateOn()
    {
        segCount_ = 0;
        phase_ = Phase::Running;
        pushSegment(levels_[0], times_[0]);
        pushSegment(levels_[1], times_[1]);
        pushSegment(levels_[2], times_[2]);
    }

    void gateOff()
    {
        segCount_ = 0;
        phase_ = Phase::Release;
        pushSegment(levels_[3], times_[3]);
    }

    float process()
    {
        if (segCount_ == 0)
            return current_;

        current_ += segments_[0].increment;
        if (--segments_[0].remaining == 0)
        {
            current_ = segments_[0].target;
            for (uint8_t i = 1; i < segCount_; ++i)
            {
                segments_[i - 1] = segments_[i];
            }
            --segCount_;
            if (segCount_ == 0)
            {
                phase_ = phase_ == Phase::Release ? Phase::Idle : Phase::Sustain;
            }
        }
        return current_;
    }

    bool isIdle() const { return phase_ == Phase::Idle && segCount_ == 0 && current_ == 0.0f; }
    bool isHolding() const { return segCount_ == 0; }
    float current() const { return current_; }
    float level(size_t index) const { return levels_[index]; }
    float time(size_t index) const { return times_[index]; }

    bool wholeBlockRamp(size_t frames, float &start, float &increment) const
    {
        if (segCount_ == 0 || frames == 0 || segments_[0].remaining < frames)
            return false;
        start = current_;
        increment = segments_[0].increment;
        return true;
    }

private:
    enum class Phase : uint8_t
    {
        Idle,
        Running,
        Sustain,
        Release,
    };

    struct Segment
    {
        float target = 0.0f;
        uint32_t remaining = 0;
        float increment = 0.0f;
    };

    void pushSegment(float target, float durationSeconds)
    {
        if (segCount_ >= 3)
            return;
        uint32_t samples = static_cast<uint32_t>(durationSeconds * sampleRate_);
        if (samples < 1)
            samples = 1;
        const float base = segCount_ == 0 ? current_ : segments_[segCount_ - 1].target;
        segments_[segCount_].target = target;
        segments_[segCount_].remaining = samples;
        segments_[segCount_].increment = (target - base) / static_cast<float>(samples);
        ++segCount_;
    }

    Segment segments_[3];
    float levels_[4] = {10.0f, 8.0f, 7.0f, 0.0f};
    float times_[4] = {0.1f, 0.25f, 0.4f, 0.6f};
    float current_ = 0.0f;
    float sampleRate_ = 44100.0f;
    uint8_t segCount_ = 0;
    Phase phase_ = Phase::Idle;
    bool initialized_ = false;
};

struct DxEnvelopeState
{
    DxEnvelopeScheduler scheduler;
    float timeMaxSeconds[4] = {4.0f, 4.0f, 4.0f, 4.0f};
    bool timeLog[4] = {false, false, false, false};
    float velocity = 1.0f;
    bool gatePrev = false;
};

inline void render(NodeInstance &inst, size_t frames)
{
    auto &state = *inst.dxEnvelope;
    float levels[4];
    float times[4];
    bool shapeConnected = false;
    for (size_t i = 0; i < 4; ++i)
    {
        levels[i] = state.scheduler.level(i);
        times[i] = state.scheduler.time(i);
        if ((inst.audioConnectedPortMask & synth::portBit(static_cast<int>(i))) != 0)
        {
            levels[i] = clampLevel(noderender::portSample(inst, static_cast<int>(i), 0));
            shapeConnected = true;
        }
        if ((inst.audioConnectedPortMask & synth::portBit(static_cast<int>(i + 4))) != 0)
        {
            times[i] = cv::cvToEnvelopeSeconds(
                noderender::portSample(inst, static_cast<int>(i + 4), 0),
                state.timeMaxSeconds[i], state.timeLog[i]);
            shapeConnected = true;
        }
    }
    if (shapeConnected)
    {
        state.scheduler.configure(levels, times, static_cast<float>(synth::kSampleRate));
    }

    inst.outIsRamp = false;
    inst.outIsConstant = false;
    if (state.scheduler.isHolding())
    {
        const float held = state.scheduler.current() * state.velocity;
        inst.outIsConstant = true;
        inst.outConstantValue = held;
        for (size_t i = 0; i < frames; ++i)
            inst.outL[i] = held;
        return;
    }

    float rampStart = 0.0f;
    float rampIncrement = 0.0f;
    if (state.scheduler.wholeBlockRamp(frames, rampStart, rampIncrement))
    {
        inst.outIsRamp = true;
        inst.outRamp.start = rampStart * state.velocity;
        inst.outRamp.inc = rampIncrement * state.velocity;
    }
    for (size_t i = 0; i < frames; ++i)
        inst.outL[i] = state.scheduler.process() * state.velocity;
}

inline void build(NodeInstance &inst, const WidgetParams &params)
{
    inst.dxEnvelope->scheduler.configure(params.dxLevel, params.dxTime, static_cast<float>(synth::kSampleRate));
    for (size_t i = 0; i < 4; ++i)
    {
        inst.dxEnvelope->timeMaxSeconds[i] = clampTime(params.dxTimeMaxSeconds[i]);
        inst.dxEnvelope->timeLog[i] = params.dxTimeLog[i];
    }
}

inline void destroy(NodeInstance &inst)
{
    if (inst.dxEnvelope != nullptr)
    {
        heap_caps_free(inst.dxEnvelope);
        inst.dxEnvelope = nullptr;
    }
}

inline int parameterIndex(const char *name, const char *prefix)
{
    if (name == nullptr || strncmp(name, prefix, strlen(prefix)) != 0)
        return -1;
    const char digit = name[strlen(prefix)];
    return digit >= '1' && digit <= '4' && name[strlen(prefix) + 1] == '\0' ? digit - '1' : -1;
}

inline bool setParam(NodeInstance &inst, const char *name, float value)
{
    if (name == nullptr)
        return false;
    auto &state = *inst.dxEnvelope;
    float levels[4];
    float times[4];
    for (size_t i = 0; i < 4; ++i)
    {
        levels[i] = state.scheduler.level(i);
        times[i] = state.scheduler.time(i);
    }

    const int levelIndex = parameterIndex(name, "level");
    if (levelIndex >= 0)
    {
        levels[levelIndex] = clampLevel(value);
        state.scheduler.configure(levels, times, static_cast<float>(synth::kSampleRate));
        return true;
    }
    const int timeIndex = parameterIndex(name, "time");
    if (timeIndex >= 0)
    {
        times[timeIndex] = clampTime(value);
        state.scheduler.configure(levels, times, static_cast<float>(synth::kSampleRate));
        return true;
    }
    if (strncmp(name, "time", 4) == 0 && name[4] >= '1' && name[4] <= '4' &&
        strcmp(name + 5, "MaxSeconds") == 0)
    {
        state.timeMaxSeconds[name[4] - '1'] = clampTime(value);
        return true;
    }
    if (strcmp(name, "velocity") == 0)
    {
        float normalized = value * cv::kInvMaxVolts;
        if (normalized < 0.0f)
            normalized = 0.0f;
        if (normalized > 1.0f)
            normalized = 1.0f;
        state.velocity = normalized;
        return true;
    }
    if (strcmp(name, "gate") == 0)
    {
        const bool gateNow = value >= cv::kGateThresholdVolts;
        if (gateNow != state.gatePrev)
        {
            gateNow ? state.scheduler.gateOn() : state.scheduler.gateOff();
            state.gatePrev = gateNow;
        }
        return true;
    }
    return false;
}

inline void parsePreset(cJSON *params, WidgetParams &out)
{
    for (int i = 0; i < 4; ++i)
    {
        char key[24];
        snprintf(key, sizeof(key), "level%d", i + 1);
        out.dxLevel[i] = clampLevel(presetparser::getFloat(params, key, out.dxLevel[i]));
        snprintf(key, sizeof(key), "time%dMaxSeconds", i + 1);
        out.dxTimeMaxSeconds[i] = clampTime(presetparser::getFloat(params, key, 4.0f));
        snprintf(key, sizeof(key), "time%d", i + 1);
        out.dxTime[i] = clampTime(presetparser::getFloat(params, key, out.dxTime[i]));
        if (out.dxTime[i] > out.dxTimeMaxSeconds[i])
            out.dxTime[i] = out.dxTimeMaxSeconds[i];
        snprintf(key, sizeof(key), "time%dCurve", i + 1);
        out.dxTimeLog[i] = strcmp(presetparser::getString(params, key, "linear"), "log") == 0;
    }
}

} // namespace nodes::dxenvelope

using nodes::dxenvelope::DxEnvelopeState;
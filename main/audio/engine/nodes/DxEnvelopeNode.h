#pragma once

#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <cstring>

#include "esp_heap_caps.h"

#include "../CvConventions.h"
#include "../EngineStats.h"
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
        if (!initialized_ || (phase_ == Phase::Idle && remaining_ == 0 && current_ == oldReleaseLevel))
        {
            current_ = levels_[3];
        }
        initialized_ = true;
    }

    void gateOn()
    {
        phase_ = Phase::Running;
        segmentIndex_ = 0;
        startSegment();
    }

    void gateOff()
    {
        phase_ = Phase::Release;
        segmentIndex_ = 3;
        startSegment();
    }

    float process()
    {
        if (remaining_ == 0)
            return current_;

        current_ += increment_;
        if (--remaining_ == 0)
        {
            current_ = target_;
            advanceSegment();
        }
        return current_;
    }

    bool isIdle() const { return phase_ == Phase::Idle && remaining_ == 0 && current_ == 0.0f; }
    bool isHolding() const { return remaining_ == 0; }
    float current() const { return current_; }
    float level(size_t index) const { return levels_[index]; }
    float time(size_t index) const { return times_[index]; }

    bool wholeBlockRamp(size_t frames, float &start, float &increment) const
    {
        // process() advances before returning its sample. Publish that first
        // returned value, not current_ (which is the sample before this
        // block). Require one sample beyond the block so the final value
        // cannot be the target snap/segment transition rather than a pure
        // repeated-add ramp.
        if (remaining_ <= frames || frames == 0)
            return false;
        start = current_ + increment_;
        increment = increment_;
        return true;
    }

    // Precondition: wholeBlockRamp(frames, ...) returned true. Keeps the
    // exact repeated float additions of process(), but hoists the remaining
    // test and all scheduler member traffic out of the sample loop.
    void processRampBlock(float *out, size_t frames, float scale)
    {
        float current = current_;
        const float increment = increment_;
        for (size_t i = 0; i < frames; ++i)
        {
            current += increment;
            out[i] = current * scale;
        }
        current_ = current;
        remaining_ -= static_cast<uint32_t>(frames);
    }

    // Same state transition as processRampBlock(), without materializing
    // samples. Repeated additions preserve scheduler state exactly; the
    // consumer reconstructs the advertised start/increment ramp itself.
    void advanceRampBlock(size_t frames)
    {
        float current = current_;
        const float increment = increment_;
        for (size_t i = 0; i < frames; ++i)
        {
            current += increment;
        }
        current_ = current;
        remaining_ -= static_cast<uint32_t>(frames);
    }

private:
    enum class Phase : uint8_t
    {
        Idle,
        Running,
        Sustain,
        Release,
    };

    void startSegment()
    {
        uint32_t samples = static_cast<uint32_t>(times_[segmentIndex_] * sampleRate_);
        if (samples < 1)
            samples = 1;
        remaining_ = samples;
        target_ = levels_[segmentIndex_];
        increment_ = (target_ - current_) / static_cast<float>(samples);
    }

    void advanceSegment()
    {
        if (phase_ == Phase::Running && segmentIndex_ < 2)
        {
            ++segmentIndex_;
            startSegment();
            return;
        }
        remaining_ = 0;
        phase_ = phase_ == Phase::Release ? Phase::Idle : Phase::Sustain;
    }

    float levels_[4] = {10.0f, 8.0f, 7.0f, 0.0f};
    float times_[4] = {0.1f, 0.25f, 0.4f, 0.6f};
    float current_ = 0.0f;
    float sampleRate_ = 44100.0f;
    float increment_ = 0.0f;
    float target_ = 0.0f;
    uint32_t remaining_ = 0;
    uint8_t segmentIndex_ = 0;
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

inline void updateParameters(NodeInstance &inst)
{
    auto &state = *inst.dxEnvelope;
    constexpr synth::PortMask kShapePortMask = 0xFFu;
    const auto shapePorts = inst.audioConnectedPortMask | inst.controlConnectedPortMask;
    if ((shapePorts & kShapePortMask) != 0)
    {
        float levels[4];
        float times[4];
        for (size_t i = 0; i < 4; ++i)
        {
            levels[i] = state.scheduler.level(i);
            times[i] = state.scheduler.time(i);
            if ((shapePorts & synth::portBit(static_cast<int>(i))) != 0)
            {
                levels[i] = clampLevel(noderender::portSample(inst, static_cast<int>(i), 0));
            }
            if ((shapePorts & synth::portBit(static_cast<int>(i + 4))) != 0)
            {
                times[i] = cv::cvToEnvelopeSeconds(
                    noderender::portSample(inst, static_cast<int>(i + 4), 0),
                    state.timeMaxSeconds[i], state.timeLog[i]);
            }
        }
        state.scheduler.configure(levels, times, static_cast<float>(synth::kSampleRate));
        ++enginestats::current.dxShapeConfigured;
    }
}

inline void render(NodeInstance &inst, size_t frames)
{
    auto &state = *inst.dxEnvelope;
    updateParameters(inst);

    // "gate" (Port-Index 8, real but control-only audio-rate port, see
    // PortTables.h) - lets a non-MIDI source (e.g. a CV Step-Arpeggiator's
    // "gateOut") retrigger this envelope directly, same fallback pattern as
    // AdsrNode.h's own new "gate" port: only consulted when actually wired,
    // Graph::deliverNoteControlRoutes' MIDI-note-driven gateOn()/gateOff()
    // delivery remains the path for every DX Envelope with no cable here.
    if ((inst.audioConnectedPortMask & synth::portBit(8)) != 0)
    {
        const bool gateNow = noderender::portSample(inst, 8, 0) >= cv::kGateThresholdVolts;
        if (gateNow != state.gatePrev)
        {
            gateNow ? state.scheduler.gateOn() : state.scheduler.gateOff();
            state.gatePrev = gateNow;
        }
    }
    // "velocity" (port 9): any wired source scales the output 0-10V -> 0..1, like Adsr's
    // velocity port. The MidiVelocity route writes the same value into this port.
    if (noderender::portIsFed(inst, 9))
    {
        state.velocity = noderender::clampToRange(noderender::portSample(inst, 9, 0) * cv::kInvMaxVolts, 0.0f, 1.0f);
    }

    inst.outIsRamp = false;
    inst.outIsConstant = false;
    if (state.scheduler.isHolding())
    {
        ++enginestats::current.dxHolding;
        const float held = state.scheduler.current() * state.velocity;
        inst.outIsConstant = true;
        inst.outConstantValue = held;
        if (inst.outputMetadataOnly)
            return;
        if (held == 0.0f)
            memset(inst.outL, 0, frames * sizeof(float));
        else
            for (size_t i = 0; i < frames; ++i)
                inst.outL[i] = held;
        return;
    }

    float rampStart = 0.0f;
    float rampIncrement = 0.0f;
    if (state.scheduler.wholeBlockRamp(frames, rampStart, rampIncrement))
    {
        ++enginestats::current.dxRamping;
        inst.outIsRamp = true;
        inst.outRamp.start = rampStart * state.velocity;
        inst.outRamp.inc = rampIncrement * state.velocity;
        if (inst.outputMetadataOnly)
            state.scheduler.advanceRampBlock(frames);
        else
            state.scheduler.processRampBlock(inst.outL, frames, state.velocity);
        return;
    }
    else
    {
        ++enginestats::current.dxBoundary;
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
        updateParameters(inst);
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

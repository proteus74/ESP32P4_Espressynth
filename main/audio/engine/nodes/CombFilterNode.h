#pragma once

#include <cmath>
#include <cstring>

#include "../CvConventions.h"
#include "../DspPrimitives.h"
#include "../NodeInstance.h"
#include "../PresetParser.h"
#include "../SynthConfig.h"

namespace nodes::combfilter
{
// Port order: audio, time, feedback, mix, polarity, damping, gain.
constexpr const char *kParams[] = {"time", "feedback", "mix", "polarity", "damping", "gain"};
constexpr float kDefaults[] = {0.5f, 5.0f, 5.0f, 0.0f, 10.0f, 10.0f};

inline float clampCv(float v)
{
    return std::isfinite(v) ? noderender::clampf(v, 0.0f, cv::kMaxVolts) : 0.0f;
}

struct CombFilterState
{
    DelayLine delay;
    float volts[6] = {0.5f, 5.0f, 5.0f, 0.0f, 10.0f, 10.0f};
    float delaySamples = 1.0f, feedback = 0.0f, mix = 0.5f, gain = 1.0f;
    float dampingCoeff = 0.0f, dampingState = 0.0f;
    float lastDamping = -1.0f, targetDampingCoeff = 0.0f;
    float smoothing = 0.0f;
    bool initialized = false;
};

inline void render(NodeInstance &inst, size_t frames)
{
    using namespace noderender;
    auto &s = *inst.combFilter;
    const float sampleRate = static_cast<float>(synth::kSampleRate);
    float values[6];
    for (int p = 0; p < 6; ++p)
        values[p] = clampCv(portIsFed(inst, p + 1) ? portSample(inst, p + 1, 0) : s.volts[p]);
    const float delayTarget = std::fmax(1.0f, values[0] * 0.01f * sampleRate);
    const float feedbackTarget = values[1] * 0.095f * (values[3] >= 5.0f ? -1.0f : 1.0f);
    const float mixTarget = values[2] / cv::kMaxVolts;
    if (values[4] != s.lastDamping)
    {
        s.lastDamping = values[4];
        // 10V disables damping; lower voltages set a logarithmic 20Hz..20kHz cutoff.
        const float cutoff = std::fmin(20000.0f, sampleRate * 0.45f);
        s.targetDampingCoeff = values[4] >= cv::kMaxVolts ? 0.0f :
            expf(-6.283185307179586f * 20.0f * powf(cutoff / 20.0f, values[4] / cv::kMaxVolts) / sampleRate);
    }
    if (!s.initialized)
    {
        s.delaySamples = delayTarget;
        s.feedback = feedbackTarget;
        s.mix = mixTarget;
        s.gain = values[5] / cv::kMaxVolts;
        s.dampingCoeff = s.targetDampingCoeff;
        s.initialized = true;
    }
    for (size_t i = 0; i < frames; ++i)
    {
        s.delaySamples += s.smoothing * (delayTarget - s.delaySamples);
        s.feedback += s.smoothing * (feedbackTarget - s.feedback);
        s.mix += s.smoothing * (mixTarget - s.mix);
        s.dampingCoeff += s.smoothing * (s.targetDampingCoeff - s.dampingCoeff);
        const float gainTarget = clampCv(portIsFed(inst, 6) ? portSample(inst, 6, i) : s.volts[5]) / cv::kMaxVolts;
        s.gain += s.smoothing * (gainTarget - s.gain);
        const float input = portSample(inst, 0, i);
        // DelayLine's tap 0 is the preceding sample: subtract one for exact delay timing.
        const float wet = s.delay.readInterpolated(s.delaySamples - 1.0f);
        s.dampingState = dsp::flushDenorm(wet * (1.0f - s.dampingCoeff) + s.dampingState * s.dampingCoeff);
        s.delay.write(dsp::flushDenorm(input + s.feedback * s.dampingState));
        inst.outL[i] = (input * (1.0f - s.mix) + wet * s.mix) * s.gain;
    }
}

inline void build(NodeInstance &inst, const WidgetParams &p, bool psram)
{
    auto &s = *inst.combFilter;
    for (int i = 0; i < 6; ++i) s.volts[i] = clampCv(p.combCv[i]);
    s.smoothing = 1.0f - expf(-1.0f / (0.005f * synth::kSampleRate));
    s.delay.initSamples(static_cast<uint32_t>(synth::kSampleRate / 10) + 2, psram);
}

inline void destroy(NodeInstance &inst)
{
    if (inst.combFilter == nullptr) return;
    inst.combFilter->delay.freeBuffer();
    heap_caps_free(inst.combFilter);
    inst.combFilter = nullptr;
}

inline bool setParam(NodeInstance &inst, const char *name, float value)
{
    if (name == nullptr) return false;
    for (int i = 0; i < 6; ++i)
        if (strcmp(name, kParams[i]) == 0)
        {
            inst.combFilter->volts[i] = clampCv(value);
            return true;
        }
    return false;
}

inline void parsePreset(cJSON *p, WidgetParams &out)
{
    for (int i = 0; i < 6; ++i)
        out.combCv[i] = clampCv(presetparser::getFloat(p, kParams[i], kDefaults[i]));
}
} // namespace nodes::combfilter

using nodes::combfilter::CombFilterState;

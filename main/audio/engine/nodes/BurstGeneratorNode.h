#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "../CvConventions.h"
#include "../NodeInstance.h"
#include "../PresetParser.h"
#include "../SynthConfig.h"

// See NodeState.h's BurstGeneratorState comment for the mode/param split.
namespace nodes::burstgenerator
{

inline bool portFed(const NodeInstance &inst, uint8_t port)
{
    return noderender::controlPortConnected(inst, port) ||
           (inst.audioConnectedPortMask & synth::portBit(port)) != 0;
}

// Fixed output pulse width - short enough to read as a "trigger" rather
// than a gate, long enough for a downstream envelope to actually see it.
constexpr float kOutPulseMs = 5.0f;

inline void render(NodeInstance &inst, size_t frames)
{
    using namespace noderender;
    auto &s = inst.burstGenerator;
    const float sampleRate = static_cast<float>(synth::kSampleRate);
    const uint32_t outPulseSamples = static_cast<uint32_t>(kOutPulseMs * 0.001f * sampleRate);
    const bool modeFed = portFed(inst, 1);
    const bool countFed = portFed(inst, 2);
    const bool timeFed = portFed(inst, 3);
    const bool decayFed = portFed(inst, 4);

    // Idle: nothing pending, no pulse running and a trigger input that holds
    // one value all block without a rising edge - the output stays 0.
    float trigger = 0.0f;
    if (s.pendingCount == 0 && s.outSamplesLeft == 0 && audioPortConstantForBlock(inst, 0, frames, trigger))
    {
        const bool triggerAbove = trigger >= cv::kGateThresholdVolts;
        if (!triggerAbove || s.prevTriggerAbove)
        {
            s.prevTriggerAbove = triggerAbove;
            memset(inst.outL, 0, frames * sizeof(float));
            return;
        }
    }

    for (size_t i = 0; i < frames; ++i)
    {
        const bool delayMode = (modeFed ? portSample(inst, 1, i) : s.modeCv) >= cv::kGateThresholdVolts;
        const bool triggerAbove = portSample(inst, 0, i) >= cv::kGateThresholdVolts;
        if (triggerAbove && !s.prevTriggerAbove)
        {
            if (delayMode)
            {
                const float delayMs =
                    cv::cvToRange(timeFed ? portSample(inst, 3, i) : s.timeCv, 1.0f, 2000.0f, true);
                s.pendingSamples[0] = static_cast<uint32_t>(delayMs * 0.001f * sampleRate);
                s.pendingAmplitude[0] = cv::kGateOnVolts;
                s.pendingCount = 1;
            }
            else
            {
                const int pulses = static_cast<int>(
                    cv::cvToRange(countFed ? portSample(inst, 2, i) : s.countCv, 1.0f,
                                  static_cast<float>(BurstGeneratorState::kMaxPending), false) +
                    0.5f);
                const float intervalMs =
                    cv::cvToRange(timeFed ? portSample(inst, 3, i) : s.timeCv, 5.0f, 300.0f, true);
                const float decay =
                    cv::cvToRange(decayFed ? portSample(inst, 4, i) : s.decayCv, 0.0f, 0.9f, false);
                const uint32_t intervalSamples = static_cast<uint32_t>(intervalMs * 0.001f * sampleRate);
                float amplitude = cv::kGateOnVolts;
                for (int p = 0; p < pulses; ++p)
                {
                    s.pendingSamples[p] = intervalSamples * static_cast<uint32_t>(p);
                    s.pendingAmplitude[p] = amplitude;
                    amplitude *= (1.0f - decay);
                }
                s.pendingCount = static_cast<uint8_t>(pulses);
            }
        }
        s.prevTriggerAbove = triggerAbove;

        // Advance every still-pending pulse; fire (swap-remove) any that
        // reach zero. Order among pending entries doesn't matter - only
        // when each one fires relative to now.
        for (uint8_t p = 0; p < s.pendingCount;)
        {
            if (s.pendingSamples[p] == 0)
            {
                s.outSamplesLeft = outPulseSamples;
                s.outAmplitude = s.pendingAmplitude[p];
                const uint8_t last = static_cast<uint8_t>(s.pendingCount - 1);
                s.pendingSamples[p] = s.pendingSamples[last];
                s.pendingAmplitude[p] = s.pendingAmplitude[last];
                s.pendingCount = last;
            }
            else
            {
                --s.pendingSamples[p];
                ++p;
            }
        }

        inst.outL[i] = s.outSamplesLeft > 0 ? s.outAmplitude : cv::kGateOffVolts;
        if (s.outSamplesLeft > 0)
            --s.outSamplesLeft;
    }
}

inline void build(NodeInstance &inst, const WidgetParams &p)
{
    inst.burstGenerator.modeCv = p.burstGeneratorMode;
    inst.burstGenerator.countCv = p.burstGeneratorCount;
    inst.burstGenerator.timeCv = p.burstGeneratorTime;
    inst.burstGenerator.decayCv = p.burstGeneratorDecay;
}

inline bool setParam(NodeInstance &inst, const char *name, float value)
{
    if (name == nullptr)
        return false;
    auto &s = inst.burstGenerator;
    if (strcmp(name, "mode") == 0) s.modeCv = value;
    else if (strcmp(name, "count") == 0) s.countCv = value;
    else if (strcmp(name, "time") == 0) s.timeCv = value;
    else if (strcmp(name, "decay") == 0) s.decayCv = value;
    else return false;
    return true;
}

inline void parsePreset(cJSON *p, WidgetParams &out)
{
    out.burstGeneratorMode = presetparser::getFloat(p, "mode", 0.0f);
    out.burstGeneratorCount = presetparser::getFloat(p, "count", 5.0f);
    out.burstGeneratorTime = presetparser::getFloat(p, "time", 5.0f);
    out.burstGeneratorDecay = presetparser::getFloat(p, "decay", 5.0f);
}

} // namespace nodes::burstgenerator

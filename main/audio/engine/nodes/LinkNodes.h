#pragma once

#include <cstddef>
#include <cstring>

#include "../CvConventions.h"
#include "../NodeInstance.h"
#include "../PresetParser.h"

// audio-output-link / audio-input-link: eight independent mono page-boundary
// buses. GraphBuilder wires matching bus indices directly. Audio Outputs
// additionally has a 0-10V level CV for every bus.
namespace nodes::link
{

inline int gainIndex(const char *name)
{
    if (name == nullptr || strncmp(name, "gain", 4) != 0)
    {
        return -1;
    }
    const char *number = name + 4;
    if (number[0] < '1' || number[0] > '8' || number[1] != '\0')
    {
        return -1;
    }
    return number[0] - '1';
}

inline void render(NodeInstance &inst, size_t frames)
{
    for (int bus = 0; bus < static_cast<int>(synth::kLinkBusCount); ++bus)
    {
        float *output = outputPortBuffer(inst, static_cast<uint8_t>(bus));
        const float *source = inst.inPorts[bus];
        const float sourceScalar = inst.controlValue[bus];

        if (inst.type != NodeType::AudioInputLink)
        {
            // AudioOutputLink: plain passthrough, no gain stage - unchanged.
            if (source != nullptr)
            {
                memcpy(output, source, frames * sizeof(float));
            }
            else if (sourceScalar == 0.0f)
            {
                memset(output, 0, frames * sizeof(float));
            }
            else
            {
                for (size_t i = 0; i < frames; ++i)
                {
                    output[i] = sourceScalar;
                }
            }
            continue;
        }

        // Unconditionally silent this whole block (no incoming edge and no
        // nonzero default) - skip the gain stage entirely, since 0 * gain
        // is still 0 for any bus that nothing actually uses (plan section
        // 3, P2: "Nur verwendete Link-Busse anlegen und transportieren" -
        // the render-side half; the builder still creates all eight bus
        // edges regardless of use, see that section).
        if (source == nullptr && sourceScalar == 0.0f)
        {
            memset(output, 0, frames * sizeof(float));
            continue;
        }

        // Copy and gain-multiply merged into one pass instead of a
        // memcpy/fill followed by a separate per-sample multiply loop
        // (plan section 5, AudioOutputLink/AudioInputLink row: "Kopieren
        // und Multiplikation zusammenfassen").
        const int gainPort = static_cast<int>(synth::kLinkBusCount) + bus;
        const bool gainConnected = (inst.audioConnectedPortMask & synth::portBit(gainPort)) != 0;
        if (gainConnected)
        {
            for (size_t i = 0; i < frames; ++i)
            {
                const float value = source != nullptr ? source[i] : sourceScalar;
                output[i] = value * cv::vcaGain(noderender::portSample(inst, gainPort, i), false);
            }
        }
        else
        {
            const float gain = cv::vcaGain(inst.link->gainCv[bus], false);
            if (source != nullptr)
            {
                for (size_t i = 0; i < frames; ++i)
                {
                    output[i] = source[i] * gain;
                }
            }
            else
            {
                const float value = sourceScalar * gain;
                for (size_t i = 0; i < frames; ++i)
                {
                    output[i] = value;
                }
            }
        }
    }
}

inline void build(NodeInstance &inst, const WidgetParams &p)
{
    if (inst.type != NodeType::AudioInputLink || inst.link == nullptr)
    {
        return;
    }
    for (size_t i = 0; i < synth::kLinkBusCount; ++i)
    {
        const float gain = p.linkGain[i];
        inst.link->gainCv[i] = gain < 0.0f ? 0.0f : (gain > cv::kMaxVolts ? cv::kMaxVolts : gain);
    }
}

inline bool setParam(NodeInstance &inst, const char *name, float value)
{
    const int index = gainIndex(name);
    if (inst.type != NodeType::AudioInputLink || inst.link == nullptr || index < 0)
    {
        return false;
    }
    inst.link->gainCv[index] = value < 0.0f ? 0.0f : (value > cv::kMaxVolts ? cv::kMaxVolts : value);
    return true;
}

inline void parsePreset(cJSON *p, WidgetParams &out)
{
    cJSON *gains = p != nullptr ? cJSON_GetObjectItemCaseSensitive(p, "gains") : nullptr;
    if (!cJSON_IsArray(gains))
    {
        return;
    }
    int i = 0;
    cJSON *gain = nullptr;
    cJSON_ArrayForEach(gain, gains)
    {
        if (i >= static_cast<int>(synth::kLinkBusCount))
        {
            break;
        }
        if (cJSON_IsNumber(gain))
        {
            const float value = static_cast<float>(gain->valuedouble);
            out.linkGain[i] = value < 0.0f ? 0.0f : (value > cv::kMaxVolts ? cv::kMaxVolts : value);
        }
        ++i;
    }
}

} // namespace nodes::link

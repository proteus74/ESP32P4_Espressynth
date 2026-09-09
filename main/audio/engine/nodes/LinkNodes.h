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
    auto copyChannel = [&](int port, float *output)
    {
        if (inst.inPorts[port] != nullptr)
        {
            memcpy(output, inst.inPorts[port], frames * sizeof(float));
            return;
        }
        const float value = inst.controlValue[port];
        if (value == 0.0f)
        {
            memset(output, 0, frames * sizeof(float));
            return;
        }
        for (size_t i = 0; i < frames; ++i)
        {
            output[i] = value;
        }
    };
    for (int bus = 0; bus < static_cast<int>(synth::kLinkBusCount); ++bus)
    {
        float *output = outputPortBuffer(inst, static_cast<uint8_t>(bus));
        copyChannel(bus, output);
        if (inst.type != NodeType::AudioInputLink)
        {
            continue;
        }
        const int gainPort = static_cast<int>(synth::kLinkBusCount) + bus;
        const bool gainConnected = (inst.audioConnectedPortMask & synth::portBit(gainPort)) != 0;
        if (gainConnected)
        {
            for (size_t i = 0; i < frames; ++i)
            {
                output[i] *= cv::vcaGain(noderender::portSample(inst, gainPort, i), false);
            }
        }
        else
        {
            const float gain = cv::vcaGain(inst.link->gainCv[bus], false);
            for (size_t i = 0; i < frames; ++i)
            {
                output[i] *= gain;
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

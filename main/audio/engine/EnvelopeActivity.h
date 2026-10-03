#pragma once

#include "features/synth/KnobActivity.h"
#include "nodes/AdsrNode.h"
#include "nodes/DxEnvelopeNode.h"

inline bool isEnvelopeShapeParameter(NodeType type, const char *name)
{
    if (name == nullptr) return false;
    if (type == NodeType::Adsr)
        return strncmp(name, "attack", 6) == 0 || strncmp(name, "decay", 5) == 0 ||
               strcmp(name, "sustain") == 0 || strncmp(name, "release", 7) == 0 ||
               strcmp(name, "att") == 0 || strcmp(name, "dec") == 0 ||
               strcmp(name, "sus") == 0 || strcmp(name, "rel") == 0;
    if (type == NodeType::DxEnvelope)
        return nodes::dxenvelope::parameterIndex(name, "level") >= 0 ||
               (strncmp(name, "time", 4) == 0 && name[4] >= '1' && name[4] <= '4');
    return false;
}

// Read effective parameters without configuring or advancing the audio scheduler.
// Idle voices may not have rendered since a scalar control change.
inline void fillEnvelopeActivity(KnobActivity &activity, const NodeInstance &inst)
{
    using namespace noderender;
    float levels[4], times[4], maximum[4];
    const bool adsr = inst.type == NodeType::Adsr;
    auto sample = [&](int port) {
        return (inst.audioConnectedPortMask & synth::portBit(port)) != 0
                   ? portSample(inst, port, 0) : inst.controlValue[port];
    };
    if (adsr)
    {
        const auto &s = *inst.adsr;
        maximum[0] = s.attackMaxSeconds; maximum[1] = s.decayMaxSeconds;
        maximum[2] = s.releaseMaxSeconds;
        times[0] = portIsFed(inst, 0) ? cv::cvToEnvelopeSeconds(sample(0), maximum[0], s.attackLog) : s.ramp.attackSec();
        times[1] = portIsFed(inst, 1) ? cv::cvToEnvelopeSeconds(sample(1), maximum[1], s.decayLog) : s.ramp.decaySec();
        times[2] = portIsFed(inst, 3) ? cv::cvToEnvelopeSeconds(sample(3), maximum[2], s.releaseLog) : s.ramp.releaseSec();
        levels[0] = 10; levels[1] = portIsFed(inst, 2) ? sample(2) : s.ramp.sustainLevel() * 10;
        levels[2] = levels[1]; levels[3] = 0;
    }
    else
    {
        const auto &s = *inst.dxEnvelope;
        for (int i = 0; i < 4; ++i)
        {
            maximum[i] = s.timeMaxSeconds[i];
            times[i] = portIsFed(inst, i + 4) ? cv::cvToEnvelopeSeconds(sample(i + 4), maximum[i], s.timeLog[i]) : s.scheduler.time(i);
            levels[i] = portIsFed(inst, i) ? sample(i) : s.scheduler.level(i);
        }
    }
    auto unit = [](float value) { return clampToRange(value, 0, 1); };
    auto point = [&](int index, float x, float level) {
        activity.envelope[index] = {static_cast<uint16_t>(x + 0.5f), static_cast<uint16_t>(unit(level * 0.1f) * 1000 + 0.5f)};
    };
    activity.isEnvelope = true;
    point(0, 0, adsr ? 0 : levels[3]);
    float x = 0;
    const int segments = adsr ? 3 : 4;
    for (int i = 0; i < segments - 1; ++i)
    {
        x += unit(times[i] / maximum[i]) * (918.0f / segments);
        point(i + 1, x, levels[i]);
    }
    x += 82; // same fixed sustain plateau as the web UI
    point(segments, x, levels[segments - 2]);
    x += unit(times[segments - 1] / maximum[segments - 1]) * (918.0f / segments);
    point(segments + 1, x, adsr ? 0 : levels[3]);
    if (adsr) activity.envelope[5] = activity.envelope[4];
}

#pragma once

#include <cmath>
#include <cstddef>
#include <cstring>

#include "../CvConventions.h"
#include "../NodeInstance.h"
#include "../PresetParser.h"

namespace nodes::envelopefollower
{

inline bool portFed(const NodeInstance &inst, uint8_t port)
{
    return noderender::controlPortConnected(inst, port) ||
           (inst.audioConnectedPortMask & synth::portBit(port)) != 0;
}

// Standard one-pole time-constant coefficient: after `ms` milliseconds the
// envelope has covered ~63% of the distance to a step input, matching
// every textbook attack/release follower.
inline float coeffFromMs(float ms, float sampleRate)
{
    return 1.0f - expf(-1.0f / (ms * 0.001f * sampleRate));
}

// Recomputes coeffAttack/coeffRelease/preGain from the three CV values -
// two expf() calls plus the log-curve cv::cvToRange() math, exactly the
// cost FilterState::coeffStride amortizes for cutoff/resonance (see
// render()'s audio-fed branch below).
inline void recomputeCoeffs(EnvelopeFollowerState &s, float attackCv, float releaseCv, float preGainCv,
                             float sampleRate)
{
    const float attackMs = cv::cvToRange(attackCv, 1.0f, 500.0f, true);
    const float releaseMs = cv::cvToRange(releaseCv, 10.0f, 2000.0f, true);
    s.coeffAttack = coeffFromMs(attackMs, sampleRate);
    s.coeffRelease = coeffFromMs(releaseMs, sampleRate);
    s.preGain = cv::cvToRange(preGainCv, 0.1f, 10.0f, true);
}

inline void render(NodeInstance &inst, size_t frames)
{
    using namespace noderender;
    auto &s = inst.envelopeFollower;
    const bool attackFed = portFed(inst, 1);
    const bool releaseFed = portFed(inst, 2);
    const bool preGainFed = portFed(inst, 3);
    const float sampleRate = static_cast<float>(synth::kSampleRate);
    const bool anyParamAudioFed =
        (inst.audioConnectedPortMask & static_cast<uint8_t>((1u << 1) | (1u << 2) | (1u << 3))) != 0;

    auto step = [&](float sampleAbs)
    {
        const float coeff = sampleAbs > s.envelope ? s.coeffAttack : s.coeffRelease;
        float envelope = s.envelope + coeff * (sampleAbs - s.envelope);
        envelope = envelope < 0.0f ? 0.0f : (envelope > cv::kMaxVolts ? cv::kMaxVolts : envelope);
        s.envelope = envelope;
        return envelope;
    };

    if (anyParamAudioFed)
    {
        // Same pattern as Filter's cutoffAudioFed/resonanceAudioFed branch
        // (FilterNode.h): coefficients recompute every coeffStride-th
        // sample, the actual envelope smoothing still runs every sample.
        const size_t stride = static_cast<size_t>(clampf(static_cast<float>(s.coeffStride), 1.0f, 16.0f));
        size_t nextCoeffSample = 0;
        for (size_t i = 0; i < frames; ++i)
        {
            if (i == nextCoeffSample)
            {
                nextCoeffSample += stride;
                recomputeCoeffs(s, attackFed ? portSample(inst, 1, i) : s.attackCv,
                                 releaseFed ? portSample(inst, 2, i) : s.releaseCv,
                                 preGainFed ? portSample(inst, 3, i) : s.preGainCv, sampleRate);
            }
            inst.outL[i] = step(fabsf(portSample(inst, 0, i) * s.preGain));
        }
        return;
    }
    // Manual/unwired params - recompute only when actually changed (see
    // setParam/build), same lazy pattern as Filter's coeffsDirty path.
    if (s.coeffsDirty)
    {
        recomputeCoeffs(s, s.attackCv, s.releaseCv, s.preGainCv, sampleRate);
        s.coeffsDirty = false;
    }
    for (size_t i = 0; i < frames; ++i)
    {
        inst.outL[i] = step(fabsf(portSample(inst, 0, i) * s.preGain));
    }
}

inline void build(NodeInstance &inst, const WidgetParams &p)
{
    inst.envelopeFollower.attackCv = p.envelopeFollowerAttack;
    inst.envelopeFollower.releaseCv = p.envelopeFollowerRelease;
    inst.envelopeFollower.preGainCv = p.envelopeFollowerPreGain;
    inst.envelopeFollower.coeffStride = p.envelopeFollowerCoeffStride;
    inst.envelopeFollower.coeffsDirty = true;
}

inline bool setParam(NodeInstance &inst, const char *name, float value)
{
    if (name == nullptr)
        return false;
    auto &s = inst.envelopeFollower;
    if (strcmp(name, "attack") == 0) { s.attackCv = value; s.coeffsDirty = true; }
    else if (strcmp(name, "release") == 0) { s.releaseCv = value; s.coeffsDirty = true; }
    else if (strcmp(name, "preGain") == 0) { s.preGainCv = value; s.coeffsDirty = true; }
    else if (strcmp(name, "coeffStride") == 0)
        s.coeffStride = static_cast<uint8_t>(noderender::clampf(value, 1.0f, 16.0f));
    else return false;
    return true;
}

inline void parsePreset(cJSON *p, WidgetParams &out)
{
    out.envelopeFollowerAttack = presetparser::getFloat(p, "attack", 5.0f);
    out.envelopeFollowerRelease = presetparser::getFloat(p, "release", 5.0f);
    out.envelopeFollowerPreGain = presetparser::getFloat(p, "preGain", 5.0f);
    const int stride = presetparser::getInt(p, "coeffStride", 4);
    out.envelopeFollowerCoeffStride = static_cast<uint8_t>(stride < 1 ? 1 : (stride > 16 ? 16 : stride));
}

} // namespace nodes::envelopefollower

#pragma once

#include <cmath>
#include <cstddef>
#include <cstring>

#include "../CvConventions.h"
#include "../NodeInstance.h"
#include "../PresetParser.h"
#include "../SynthConfig.h"

// The four hardware-sink output widgets (AudioOut/UsbOut/Es8311Out/
// Wm8960Out) - identical shape (left/right level trim, gain, mute with
// smoothing, a SoftLimiter), only which physical sink SynthEngine reads the
// rendered block from differs. AudioOutState (value member) stays in
// NodeState.h.
namespace nodes::output
{

inline void render(NodeInstance &inst, size_t frames)
{
    using namespace noderender;
    const float kSampleRate = static_cast<float>(synth::kSampleRate);
    auto &s = inst.audioOut;
    // Fixed 10ms mute-smoothing time constant (kSampleRate never changes at
    // runtime) - compute once instead of an expf() call on every single
    // block for every output sink.
    static const float muteCoeff = expf(-1.0f / (kSampleRate * 0.01f));
    const float targetGain = (inst.controlConnectedPortMask & (1u << 2)) != 0
                                 ? cv::cvToRange(inst.controlValue[2], 0.0f, 1.0f, false)
                                 : cv::cvToRange(s.gain, 0.0f, 1.0f, false);
    const bool muted = (inst.controlConnectedPortMask & (1u << 3)) != 0
                           ? inst.controlValue[3] >= cv::kGateThresholdVolts
                           : s.muted;
    const float targetMute = muted ? 0.0f : targetGain;
    // Already faded to silence (gain==0 and/or muted) AND staying there -
    // skip reading the upstream signal and running the limiter/multiply per
    // sample, using this node's own already-tracked muteSmoothed as the
    // convergence check (same pattern as Oscillator/SoundFontPlayer's
    // gain==0 fast path). Gated on THIS block's target being silent too, not
    // just the previous one, so a gain/mute change made this very block
    // still fades out through the normal per-sample path below exactly as
    // before - no click.
    constexpr float kSilentGain = 1e-4f;
    if (targetMute <= 0.0f && s.muteSmoothed <= kSilentGain)
    {
        memset(inst.outL, 0, frames * sizeof(float));
        memset(inst.outR, 0, frames * sizeof(float));
        return;
    }
    for (size_t i = 0; i < frames; ++i)
    {
        float l = portSample(inst, 0, i) * s.leftLevel;
        float r = portSample(inst, 1, i) * s.rightLevel;
        s.limiter.process(l, r);
        s.muteSmoothed = muteCoeff * s.muteSmoothed + (1.0f - muteCoeff) * targetMute;
        inst.outL[i] = l * s.muteSmoothed;
        inst.outR[i] = r * s.muteSmoothed;
    }
}

inline void build(NodeInstance &inst, const WidgetParams &p)
{
    inst.audioOut.leftLevel = p.left;
    inst.audioOut.rightLevel = p.right;
    inst.audioOut.gain = p.gain;
    inst.audioOut.muted = p.muted;
    inst.audioOut.limiter.configure(static_cast<float>(synth::kSampleRate));
}

inline bool setParam(NodeInstance &inst, const char *name, float value)
{
    if (name == nullptr)
    {
        return false;
    }
    if (strcmp(name, "left") == 0)
    {
        inst.audioOut.leftLevel = value;
        return true;
    }
    if (strcmp(name, "right") == 0)
    {
        inst.audioOut.rightLevel = value;
        return true;
    }
    if (strcmp(name, "gain") == 0)
    {
        inst.audioOut.gain = value < 0.0f ? 0.0f : (value > 10.0f ? 10.0f : value);
        return true;
    }
    if (strcmp(name, "muted") == 0)
    {
        inst.audioOut.muted = value >= 0.5f;
        return true;
    }
    return false;
}

inline void parsePreset(cJSON *p, WidgetParams &out)
{
    out.muted = presetparser::getBool(p, "muted", false);
    out.left = presetparser::getFloat(p, "left", 1.0f);
    out.right = presetparser::getFloat(p, "right", 1.0f);
    out.gain = presetparser::getFloat(p, "gain", cv::kMaxVolts);
    if (out.gain < 0.0f) out.gain = 0.0f;
    if (out.gain > cv::kMaxVolts) out.gain = cv::kMaxVolts;
}

} // namespace nodes::output

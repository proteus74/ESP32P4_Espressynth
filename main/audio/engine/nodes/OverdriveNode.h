#pragma once

#include <cmath>
#include <cstddef>
#include <cstring>

#include "../CvConventions.h"
#include "../NodeInstance.h"
#include "../PresetParser.h"
#include "../SynthConfig.h"

// Overdrive / Distortion: drive -> bias -> 2x oversampled waveshaper ->
// DC blocker -> one-pole tone low-pass -> auto-level -> dry/wet mix -> gain.
// Auto-level matches the wet signal's RMS to the input's, so Drive changes
// only the amount of distortion, never the loudness. Each mode has its own
// circuit flavour so they stay distinct at high drive (where any clipper
// alone converges to a square wave). Must match the "overdrive" processor in
// static/js/overdrive-core.js 1:1.
namespace nodes::overdrive
{

enum Mode : int
{
    Soft = 0, // Tube-Screamer style: clip only the >720 Hz part, add the clean signal back
    Hard = 1, // full-band hard clip (distortion)
    Tube = 2, // strongly asymmetric -> even harmonics
    Fuzz = 3, // near-square exponential saturation
};

inline float clampCv(float value)
{
    return value < 0.0f ? 0.0f : (value > cv::kMaxVolts ? cv::kMaxVolts : value);
}

// 0-10V stepped into four 2.5V bands.
inline int modeIndex(float modeCv)
{
    const int idx = static_cast<int>(clampCv(modeCv) * 0.4f);
    return idx > 3 ? 3 : idx;
}

// Rational tanh approximation (fastTanh-style): smooth knee,
// reaches +/-1 with zero slope at |x| = 3.
inline float softClip(float x)
{
    if (x > 3.0f)
        return 1.0f;
    if (x < -3.0f)
        return -1.0f;
    const float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

inline float shape(int mode, float x)
{
    switch (mode)
    {
    case Hard:
        return x > 1.0f ? 1.0f : (x < -1.0f ? -1.0f : x);
    case Tube:
        // Positive half compresses very gradually, negative half limits at -0.5.
        return x >= 0.0f ? x / (1.0f + x) : 0.5f * softClip(2.0f * x);
    case Fuzz:
        // exp(-4|x|) as 2^(-4*log2(e)*|x|) from the shared LUT: no libm call per sample.
        return x >= 0.0f ? 1.0f - dsp::pow2Lookup(-5.7707802f * x) : -(1.0f - dsp::pow2Lookup(5.7707802f * x));
    default:
        return softClip(x);
    }
}

inline bool portFed(const NodeInstance &inst, uint8_t port)
{
    return noderender::controlPortConnected(inst, port) ||
           (inst.audioConnectedPortMask & synth::portBit(port)) != 0;
}

inline float onePoleCoeff(float hz)
{
    // exp(-w) as 2^(-w*log2(e)) from the shared LUT - runs per sample while Tone is modulated.
    return 1.0f - dsp::pow2Lookup(-2.0f * 3.14159265f * 1.4426950f * hz / static_cast<float>(synth::kSampleRate));
}

inline void render(NodeInstance &inst, size_t frames)
{
    using namespace noderender;
    auto &s = inst.overdrive;
    const bool modeFed = portFed(inst, 1);
    const bool driveFed = portFed(inst, 2);
    const bool toneFed = portFed(inst, 3);
    const bool biasFed = portFed(inst, 4);
    const bool mixFed = portFed(inst, 5);
    const bool gainFed = portFed(inst, 6);
    // ~10 Hz DC blocker - removes the offset Bias/Tube add before the shaper.
    constexpr float kDcR = 1.0f - 2.0f * 3.14159265f * 10.0f / static_cast<float>(synth::kSampleRate);
    static const float kPreCoeff = onePoleCoeff(720.0f);
    // ~50 ms RMS window for auto-level; kLevelEps keeps silence at unity ratio.
    static const float kLevelCoeff = 1.0f - expf(-1.0f / (0.05f * static_cast<float>(synth::kSampleRate)));
    constexpr float kLevelEps = 1e-6f;
    constexpr float kMaxLevelGain = 4.0f;

    for (size_t i = 0; i < frames; ++i)
    {
        const int mode = modeIndex(modeFed ? portSample(inst, 1, i) : s.mode);
        const float driveCv = clampCv(driveFed ? portSample(inst, 2, i) : s.drive);
        const float toneCv = clampCv(toneFed ? portSample(inst, 3, i) : s.tone);
        const float biasCv = clampCv(biasFed ? portSample(inst, 4, i) : s.bias);
        const float mix = clampCv(mixFed ? portSample(inst, 5, i) : s.mix) * cv::kInvMaxVolts;
        const float gainCv = clampCv(gainFed ? portSample(inst, 6, i) : s.gain);
        if (driveCv != s.cachedDriveCv)
        {
            s.cachedDriveCv = driveCv;
            s.driveFactor = cv::cvToRangeLogFast(driveCv, 1.0f, 6.6438562f); // 1..100 (0..40 dB), log2(100)
        }
        if (toneCv != s.cachedToneCv)
        {
            s.cachedToneCv = toneCv;
            s.toneCoeff = onePoleCoeff(cv::cvToRangeLogFast(toneCv, 500.0f, 4.5849625f)); // 500..12000 Hz, log2(24)
        }

        const float dry = portSample(inst, 0, i);
        const float xn = dry * cv::kInvAudioPeakVolts;
        s.preLp += kPreCoeff * (xn - s.preLp);
        s.inPow += kLevelCoeff * (xn * xn - s.inPow);
        const float src = mode == Soft ? xn - s.preLp : xn;
        // Bias is applied before Drive and scaled by the input's peak estimate,
        // so it shifts the zero crossings (pulse width) at any input level and
        // any drive - a post-drive offset would vanish once the shaper squares
        // the wave and the DC blocker re-centres it. Tube/Fuzz carry a built-in
        // offset for their even-harmonic, pulse-like character.
        const float modeBias = mode == Tube ? 0.3f : (mode == Fuzz ? 0.6f : 0.0f);
        const float bias = ((biasCv - cv::kMaxVolts * 0.5f) * 0.1f + modeBias) * sqrtf(2.0f * s.inPow);
        const float x = (src + bias) * s.driveFactor;
        // chisle: 2x oversampling via linear-interpolated midpoint + 2-tap
        // average decimation; swap for a halfband FIR if aliasing is audible.
        float shaped = 0.5f * (shape(mode, 0.5f * (s.prevIn + x)) + shape(mode, x));
        s.prevIn = x;
        if (mode == Soft)
            shaped += xn;

        const float dc = shaped - s.dcX1 + kDcR * s.dcY1;
        s.dcX1 = shaped;
        s.dcY1 = dc;
        s.toneZ1 += s.toneCoeff * (dc - s.toneZ1);

        s.wetPow += kLevelCoeff * (s.toneZ1 * s.toneZ1 - s.wetPow);
        float level = sqrtf((s.inPow + kLevelEps) / (s.wetPow + kLevelEps));
        if (level > kMaxLevelGain)
            level = kMaxLevelGain;

        const float wet = s.toneZ1 * level * cv::kAudioPeakVolts;
        inst.outL[i] = (dry + (wet - dry) * mix) * cv::vcaGain(gainCv, false);
    }
}

inline void build(NodeInstance &inst, const WidgetParams &p)
{
    inst.overdrive.mode = clampCv(p.overdriveMode);
    inst.overdrive.drive = clampCv(p.overdriveDrive);
    inst.overdrive.tone = clampCv(p.overdriveTone);
    inst.overdrive.bias = clampCv(p.overdriveBias);
    inst.overdrive.mix = clampCv(p.overdriveMix);
    inst.overdrive.gain = clampCv(p.overdriveGain);
}

inline bool setParam(NodeInstance &inst, const char *name, float value)
{
    if (name == nullptr)
        return false;
    auto &s = inst.overdrive;
    if (strcmp(name, "mode") == 0) s.mode = clampCv(value);
    else if (strcmp(name, "drive") == 0) s.drive = clampCv(value);
    else if (strcmp(name, "tone") == 0) s.tone = clampCv(value);
    else if (strcmp(name, "bias") == 0) s.bias = clampCv(value);
    else if (strcmp(name, "mix") == 0) s.mix = clampCv(value);
    else if (strcmp(name, "gain") == 0) s.gain = clampCv(value);
    else return false;
    return true;
}

inline void parsePreset(cJSON *p, WidgetParams &out)
{
    out.overdriveMode = clampCv(presetparser::getFloat(p, "mode", 0.0f));
    out.overdriveDrive = clampCv(presetparser::getFloat(p, "drive", 5.0f));
    out.overdriveTone = clampCv(presetparser::getFloat(p, "tone", 7.0f));
    out.overdriveBias = clampCv(presetparser::getFloat(p, "bias", 5.0f));
    out.overdriveMix = clampCv(presetparser::getFloat(p, "mix", 10.0f));
    out.overdriveGain = clampCv(presetparser::getFloat(p, "gain", 10.0f));
}

} // namespace nodes::overdrive

#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "esp_heap_caps.h"

#include "../CvConventions.h"
#include "../DspPrimitives.h"
#include "../NodeInstance.h"
#include "../PresetParser.h"
#include "../SynthConfig.h"

// A compact digital model of the CEM3320's four independently usable filter
// sections. The stages are TPT one-poles with a feedback path and a small,
// deliberately asymmetric soft clip in that path. This keeps the four-pole
// Prophet-style low-pass response while allowing the two-pole low-pass,
// band-pass and high-pass taps exposed by the CEM3320's configurable wiring.
namespace nodes::curtisfilter
{

using noderender::clampf;

struct CurtisFilterState
{
    CurtisFilterMode mode = CurtisFilterMode::LowPass24;
    CurtisFilterMode renderedMode = CurtisFilterMode::None;
    float cutoffCv = 5.0f;
    float cutoffMinHz = 20.0f;
    float cutoffMaxHz = 20000.0f;
    bool cutoffLog = true;
    float resonanceCv = 5.0f;
    float resonanceMinQ = 0.1f;
    float resonanceMaxQ = 20.0f;
    bool resonanceLog = true;
    uint8_t coeffStride = 4;
    bool global = false;
    float cutoffLog2Ratio = 0.0f;
    float resonanceLog2Ratio = 0.0f;
    float stage[4] = {};
    float g = 0.1f;
    float feedback = 0.0f;
    bool coeffsDirty = true;
    // Last cutoff/resonance CV actually used to compute g/feedback under a
    // genuine audio-rate edge (see render() below) - lets a stride tick skip
    // updateCoefficients() entirely when the CV hasn't actually changed
    // since the last update, instead of unconditionally recomputing on every
    // stride-th sample (plan section 3/5, same as FilterNode.h's biquad
    // path: "Unveränderte CV-Werte trotz Audio-Kabel erkennen"). Distinct
    // from cutoffCv/resonanceCv above, which are the manual/unwired scalars.
    float lastAudioCutoffCv = 0.0f;
    float lastAudioResonanceCv = 0.0f;
    bool audioCoeffsInitialized = false;

    void refreshLog2Ratios()
    {
        const float cutoffLo = cutoffMinHz <= 0.0f ? 0.0001f : cutoffMinHz;
        cutoffLog2Ratio = log2f(cutoffMaxHz / cutoffLo);
        const float resonanceLo = resonanceMinQ <= 0.0f ? 0.0001f : resonanceMinQ;
        resonanceLog2Ratio = log2f(resonanceMaxQ / resonanceLo);
    }
};

namespace
{
inline float cutoffHz(const CurtisFilterState &s, float cutoffCv)
{
    const float sampleRate = static_cast<float>(synth::kSampleRate);
    float hz = s.cutoffLog ? cv::cvToRangeLogFast(cutoffCv, s.cutoffMinHz, s.cutoffLog2Ratio)
                           : cv::cvToRange(cutoffCv, s.cutoffMinHz, s.cutoffMaxHz, false);
    if (hz < 20.0f)
        hz = 20.0f;
    if (hz > sampleRate * 0.5f - 100.0f)
        hz = sampleRate * 0.5f - 100.0f;
    return hz;
}

inline float resonanceQ(const CurtisFilterState &s, float resonanceCv)
{
    const float q = s.resonanceLog ? cv::cvToRangeLogFast(resonanceCv, s.resonanceMinQ, s.resonanceLog2Ratio)
                                   : cv::cvToRange(resonanceCv, s.resonanceMinQ, s.resonanceMaxQ, false);
    return q < 0.05f ? 0.05f : q;
}

inline float tptG(float hz)
{
    // TPT one-pole uses tan(pi*f/Fs), i.e. half the cycle-normalized
    // frequency. Using f/Fs directly makes tan() change sign above Fs/4;
    // the defensive clamp then turns the filter into an almost-DC integrator
    // and high frequencies disappear around 11-17 kHz.
    const float phase = 0.5f * hz / static_cast<float>(synth::kSampleRate);
    const float sinW = dsp::sineLookup(phase);
    const float cosW = dsp::sineLookup(phase + 0.25f);
    const float tanW = sinW / (std::fabs(cosW) < 0.0001f ? 0.0001f : cosW);
    return tanW / (1.0f + tanW);
}

inline float processStage(CurtisFilterState &s, int index, float input)
{
    const float v = (input - s.stage[index]) * s.g;
    const float output = v + s.stage[index];
    s.stage[index] = output + v;
    return output;
}

inline float asymmetricClip(float value)
{
    const float normalized = value * cv::kInvAudioPeakVolts;
    const float drive = normalized >= 0.0f ? 1.08f : 0.96f;
    return cv::kAudioPeakVolts * dsp::tanhLookup(normalized * drive);
}

inline float feedbackFromQ(const CurtisFilterState &s, float q, CurtisFilterMode mode)
{
    const float qSpan = s.resonanceMaxQ - s.resonanceMinQ;
    const float normalized = qSpan > 0.0001f
                                 ? (q - s.resonanceMinQ) / qSpan
                                 : 0.0f;
    const float amount = clampf(normalized, 0.0f, 1.0f);
    const bool fourPole = mode == CurtisFilterMode::LowPass24;
    // The two-pole modes reach oscillation with less loop gain than the
    // four-pole ladder. Keep both ranges musical and bounded below unity.
    return amount * (fourPole ? 0.985f : 0.94f);
}

inline int poleCount(CurtisFilterMode mode)
{
    return mode == CurtisFilterMode::LowPass24 ? 4 : 2;
}

inline void updateCoefficients(CurtisFilterState &s, float cutoffCv, float resonanceCv, CurtisFilterMode mode)
{
    s.g = clampf(tptG(cutoffHz(s, cutoffCv)), 0.0001f, 0.9995f);
    s.feedback = feedbackFromQ(s, resonanceQ(s, resonanceCv), mode);
    s.coeffsDirty = false;
}

inline float outputForMode(CurtisFilterMode mode, float input, float stage1, float stage2, float stage4)
{
    switch (mode)
    {
    case CurtisFilterMode::None:
        return input;
    case CurtisFilterMode::LowPass24:
        return stage4;
    case CurtisFilterMode::LowPass12:
        return stage2;
    case CurtisFilterMode::BandPass12:
        return stage1 - stage2;
    case CurtisFilterMode::HighPass12:
        return input - 2.0f * stage1 + stage2;
    }
    return stage4;
}

inline CurtisFilterMode filterModeFromCv(float volts)
{
    int index = static_cast<int>(volts / (cv::kMaxVolts / 5.0f));
    if (index < 0)
        index = 0;
    if (index > 4)
        index = 4;
    return static_cast<CurtisFilterMode>(index);
}
} // namespace

inline void render(NodeInstance &inst, size_t frames)
{
    using namespace noderender;
    CurtisFilterState &s = *inst.curtisFilter;
    const bool typeFed = controlPortConnected(inst, 1) ||
                         (inst.audioConnectedPortMask & synth::portBit(1)) != 0;
    const CurtisFilterMode mode = typeFed ? filterModeFromCv(portSample(inst, 1, 0)) : s.mode;
    if (mode == CurtisFilterMode::None)
    {
        s.renderedMode = CurtisFilterMode::None;
        for (size_t i = 0; i < frames; ++i)
        {
            inst.outL[i] = portSample(inst, 0, i);
        }
        return;
    }
    if (mode != s.renderedMode)
    {
        s.renderedMode = mode;
        s.coeffsDirty = true;
        s.audioCoeffsInitialized = false;
    }
    const bool globalPortFed = controlPortConnected(inst, 4) ||
                               (inst.audioConnectedPortMask & static_cast<uint8_t>(1u << 4)) != 0;
    const bool globalActive = globalPortFed ? (portSample(inst, 4, 0) >= cv::kGateThresholdVolts) : s.global;
    const NodeInstance &source = (globalActive && inst.primary != nullptr) ? *inst.primary : inst;
    const bool cutoffAudioFed = (source.audioConnectedPortMask & static_cast<uint8_t>(1u << 2)) != 0;
    const bool resonanceAudioFed = (source.audioConnectedPortMask & static_cast<uint8_t>(1u << 3)) != 0;
    const size_t stride = static_cast<size_t>(clampf(static_cast<float>(s.coeffStride), 1.0f, 16.0f));

    if (!cutoffAudioFed && !resonanceAudioFed && s.coeffsDirty)
    {
        updateCoefficients(s, s.cutoffCv, s.resonanceCv, mode);
    }

    const int poles = poleCount(mode);
    for (size_t i = 0; i < frames; ++i)
    {
        if (cutoffAudioFed || resonanceAudioFed)
        {
            if (i % stride == 0)
            {
                const float cutoff = cutoffAudioFed ? source.inPorts[2][i] : s.cutoffCv;
                const float resonance = resonanceAudioFed ? source.inPorts[3][i] : s.resonanceCv;
                if (!s.audioCoeffsInitialized || cutoff != s.lastAudioCutoffCv || resonance != s.lastAudioResonanceCv)
                {
                    updateCoefficients(s, cutoff, resonance, mode);
                    s.lastAudioCutoffCv = cutoff;
                    s.lastAudioResonanceCv = resonance;
                    s.audioCoeffsInitialized = true;
                }
            }
        }

        const float input = portSample(inst, 0, i);
        const float feedbackTap = poles == 4 ? s.stage[3] : s.stage[1];
        const float drivenInput = asymmetricClip(input + asymmetricClip(feedbackTap * s.feedback));
        const float stage1 = processStage(s, 0, drivenInput);
        const float stage2 = processStage(s, 1, stage1);
        float stage4 = stage2;
        if (poles == 4)
        {
            // Fixed (plan section 6, finding 2): stage 3's actual output now
            // feeds stage 4, matching the intended 4-pole TPT ladder cascade
            // (each stage's output is the next stage's input). Previously
            // stage 3's return value was discarded and stage 4 was fed
            // s.stage[2] instead - that integrator's post-update internal
            // state, not its output (see processStage()'s v/output/state
            // relationship above) - a different, larger-magnitude signal.
            // Audibly changes LP24's frequency response/resonance character.
            const float stage3 = processStage(s, 2, stage2);
            stage4 = processStage(s, 3, stage3);
        }
        const float output = outputForMode(mode, input, stage1, stage2, stage4);
        // tanhLookup is intrinsically bounded to +/-1, so asymmetricClip is
        // already guaranteed to remain inside the audio voltage domain.
        inst.outL[i] = asymmetricClip(output);
    }
}

inline void build(NodeInstance &inst, const WidgetParams &p)
{
    CurtisFilterState &s = *inst.curtisFilter;
    s.mode = p.curtisFilterMode;
    s.cutoffCv = p.cutoffCv;
    s.cutoffMinHz = p.cutoffMinHz;
    s.cutoffMaxHz = p.cutoffMaxHz;
    s.cutoffLog = p.cutoffLog;
    s.resonanceCv = p.resonanceCv;
    s.resonanceMinQ = p.resonanceMinQ;
    s.resonanceMaxQ = p.resonanceMaxQ;
    s.resonanceLog = p.resonanceLog;
    s.coeffStride = p.coeffStride;
    s.global = p.filterGlobal;
    s.refreshLog2Ratios();
    s.coeffsDirty = true;
}

inline void destroy(NodeInstance &inst)
{
    if (inst.curtisFilter != nullptr)
    {
        heap_caps_free(inst.curtisFilter);
        inst.curtisFilter = nullptr;
    }
}

inline bool setParam(NodeInstance &inst, const char *name, float value)
{
    CurtisFilterState &s = *inst.curtisFilter;
    if (name == nullptr)
        return false;
    if (strcmp(name, "cutoff") == 0)
        s.cutoffCv = value;
    else if (strcmp(name, "cutoffMinHz") == 0)
        s.cutoffMinHz = value;
    else if (strcmp(name, "cutoffMaxHz") == 0)
        s.cutoffMaxHz = value;
    else if (strcmp(name, "resonance") == 0)
        s.resonanceCv = value;
    else if (strcmp(name, "resonanceMinQ") == 0)
        s.resonanceMinQ = value;
    else if (strcmp(name, "resonanceMaxQ") == 0)
        s.resonanceMaxQ = value;
    else if (strcmp(name, "coeffStride") == 0)
        s.coeffStride = static_cast<uint8_t>(clampf(value, 1.0f, 16.0f));
    else if (strcmp(name, "global") == 0)
        s.global = value >= 0.5f;
    else
        return false;
    s.refreshLog2Ratios();
    s.coeffsDirty = true;
    return true;
}

inline void setMode(NodeInstance &inst, CurtisFilterMode mode)
{
    inst.curtisFilter->mode = mode;
    inst.curtisFilter->coeffsDirty = true;
}

inline void parsePreset(cJSON *p, WidgetParams &out)
{
    out.curtisFilterMode = presetparser::getBool(p, "bypass", false)
                               ? CurtisFilterMode::None
                               : curtisFilterModeFromString(presetparser::getString(p, "filterType", "lp24"));
    out.cutoffCv = presetparser::getFloat(p, "cutoff", 5.0f);
    out.cutoffMinHz = presetparser::getFloat(p, "cutoffMinHz", 20.0f);
    out.cutoffMaxHz = presetparser::getFloat(p, "cutoffMaxHz", 20000.0f);
    out.cutoffLog = strcmp(presetparser::getString(p, "cutoffCurve", "log"), "log") == 0;
    out.resonanceCv = presetparser::getFloat(p, "resonance", 5.0f);
    out.resonanceMinQ = presetparser::getFloat(p, "resonanceMinQ", 0.1f);
    out.resonanceMaxQ = presetparser::getFloat(p, "resonanceMaxQ", 20.0f);
    out.resonanceLog = strcmp(presetparser::getString(p, "resonanceCurve", "log"), "log") == 0;
    const int stride = presetparser::getInt(p, "coeffStride", 4);
    out.coeffStride = static_cast<uint8_t>(stride < 1 ? 1 : (stride > 16 ? 16 : stride));
    out.filterGlobal = presetparser::getBool(p, "global", false);
}

} // namespace nodes::curtisfilter

using nodes::curtisfilter::CurtisFilterState;

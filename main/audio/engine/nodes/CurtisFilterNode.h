#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "esp_heap_caps.h"

#include "../ControlStride.h"
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

// Six cascadable one-pole sections, 6 dB each - the slope selects how many of
// them are in circuit (the real CEM3320 wires four; this model extends the same
// idea to the 36 dB setting the UI offers).
constexpr int kCurtisMaxPoles = 6;

struct CurtisFilterState
{
    FilterKind kind = FilterKind::LowPass;
    FilterKind renderedKind = FilterKind::None;
    uint8_t slopeDb = 24;
    float cutoffCv = 5.0f;
    float cutoffMinHz = 20.0f;
    float cutoffMaxHz = 20000.0f;
    bool cutoffLog = true;
    float resonanceCv = 5.0f;
    float resonanceMinQ = 0.1f;
    float resonanceMaxQ = 20.0f;
    bool resonanceLog = true;
    bool global = false;
    float cutoffLog2Ratio = 0.0f;
    float resonanceLog2Ratio = 0.0f;
    float stage[kCurtisMaxPoles] = {};
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

using cv::filterKindFromCv;
using cv::slopeDbFromCv;

inline int poleCount(uint8_t slopeDb)
{
    const int poles = slopeDb / 6;
    return poles < 1 ? 1 : (poles > kCurtisMaxPoles ? kCurtisMaxPoles : poles);
}

inline float feedbackFromQ(const CurtisFilterState &s, float q, int poles)
{
    const float qSpan = s.resonanceMaxQ - s.resonanceMinQ;
    const float normalized = qSpan > 0.0001f
                                 ? (q - s.resonanceMinQ) / qSpan
                                 : 0.0f;
    const float amount = clampf(normalized, 0.0f, 1.0f);
    // Shallower ladders reach oscillation with less loop gain than the deep
    // ones. Keep every range musical and bounded below unity.
    const float ceiling = poles >= 4 ? 0.985f : 0.94f;
    return amount * ceiling;
}

inline void updateCoefficients(CurtisFilterState &s, float cutoffCv, float resonanceCv, int poles)
{
    s.g = clampf(tptG(cutoffHz(s, cutoffCv)), 0.0001f, 0.9995f);
    s.feedback = isShelfKind(s.kind) ? 0.0f : feedbackFromQ(s, resonanceQ(s, resonanceCv), poles);
    s.coeffsDirty = false;
}

// Derives the requested response from the ladder taps. `last`/`prev` are the
// final two one-pole outputs, which behave like a two-pole section - the same
// identity the CEM3320's configurable wiring exploits (LP + 2*BP + HP = in).
inline float outputForKind(FilterKind kind, float input, float prev, float last, float shelfGainDb)
{
    const float lowpass = last;
    const float bandpass = prev - last;
    const float highpass = input - 2.0f * prev + last;
    switch (kind)
    {
    case FilterKind::None:
        return input;
    case FilterKind::LowPass:
        return lowpass;
    case FilterKind::BandPass:
        return bandpass;
    case FilterKind::HighPass:
        return highpass;
    case FilterKind::Notch:
        return lowpass + highpass;
    case FilterKind::AllPass:
        return input - 4.0f * bandpass;
    case FilterKind::LowShelf:
        return input + (powf(10.0f, shelfGainDb / 20.0f) - 1.0f) * lowpass;
    case FilterKind::HighShelf:
        return input + (powf(10.0f, shelfGainDb / 20.0f) - 1.0f) * highpass;
    }
    return lowpass;
}
} // namespace

inline void render(NodeInstance &inst, size_t frames)
{
    using namespace noderender;
    CurtisFilterState &s = *inst.curtisFilter;
    const bool typeFed = controlPortConnected(inst, 1) ||
                         (inst.audioConnectedPortMask & synth::portBit(1)) != 0;
    const FilterKind kind = typeFed ? filterKindFromCv(portSample(inst, 1, 0)) : s.kind;
    if (kind == FilterKind::None)
    {
        s.renderedKind = FilterKind::None;
        for (size_t i = 0; i < frames; ++i)
        {
            inst.outL[i] = portSample(inst, 0, i);
        }
        return;
    }
    // "slope" (Port-Index 2) - discrete 0-10V selector, six steps of 6 dB, one
    // ladder section per pole.
    const bool slopeFed = controlPortConnected(inst, 2) ||
                          (inst.audioConnectedPortMask & synth::portBit(2)) != 0;
    if (slopeFed)
    {
        const uint8_t fedSlope = slopeDbFromCv(portSample(inst, 2, 0));
        if (fedSlope != s.slopeDb)
        {
            s.slopeDb = fedSlope;
            s.coeffsDirty = true;
            s.audioCoeffsInitialized = false;
        }
    }
    if (kind != s.renderedKind)
    {
        s.renderedKind = kind;
        s.coeffsDirty = true;
        s.audioCoeffsInitialized = false;
    }
    const bool globalPortFed = controlPortConnected(inst, 5) ||
                               (inst.audioConnectedPortMask & static_cast<uint8_t>(1u << 5)) != 0;
    const bool globalActive = globalPortFed ? (portSample(inst, 5, 0) >= cv::kGateThresholdVolts) : s.global;
    const NodeInstance &source = (globalActive && inst.primary != nullptr) ? *inst.primary : inst;
    const bool cutoffAudioFed = (source.audioConnectedPortMask & static_cast<uint8_t>(1u << 3)) != 0;
    const bool resonanceAudioFed = (source.audioConnectedPortMask & static_cast<uint8_t>(1u << 4)) != 0;
    const size_t stride = synth::controlStride();
    const int poles = poleCount(s.slopeDb);

    if (!cutoffAudioFed && !resonanceAudioFed && s.coeffsDirty)
    {
        updateCoefficients(s, s.cutoffCv, s.resonanceCv, poles);
    }

    for (size_t i = 0; i < frames; ++i)
    {
        if (cutoffAudioFed || resonanceAudioFed)
        {
            if (i % stride == 0)
            {
                const float cutoff = cutoffAudioFed ? source.inPorts[3][i] : s.cutoffCv;
                const float resonance = resonanceAudioFed ? source.inPorts[4][i] : s.resonanceCv;
                if (!s.audioCoeffsInitialized || cutoff != s.lastAudioCutoffCv || resonance != s.lastAudioResonanceCv)
                {
                    updateCoefficients(s, cutoff, resonance, poles);
                    s.lastAudioCutoffCv = cutoff;
                    s.lastAudioResonanceCv = resonance;
                    s.audioCoeffsInitialized = true;
                }
            }
        }

        const float input = portSample(inst, 0, i);
        const float feedbackTap = s.stage[poles - 1];
        const float drivenInput = asymmetricClip(input + asymmetricClip(feedbackTap * s.feedback));
        // Each pole is one TPT one-pole section, every stage feeding the next.
        float chain = drivenInput;
        float previous = drivenInput;
        for (int p = 0; p < poles; ++p)
        {
            previous = chain;
            chain = processStage(s, p, chain);
        }
        const float shelfGainDb = isShelfKind(kind) ? shelfGainDbFromCv(s.resonanceCv) : 0.0f;
        const float output = outputForKind(kind, input, previous, chain, shelfGainDb);
        // tanhLookup is intrinsically bounded to +/-1, so asymmetricClip is
        // already guaranteed to remain inside the audio voltage domain.
        inst.outL[i] = asymmetricClip(output);
    }
}

inline void build(NodeInstance &inst, const WidgetParams &p)
{
    CurtisFilterState &s = *inst.curtisFilter;
    s.kind = p.filterType;
    s.slopeDb = p.filterSlopeDb;
    s.cutoffCv = p.cutoffCv;
    s.cutoffMinHz = p.cutoffMinHz;
    s.cutoffMaxHz = p.cutoffMaxHz;
    s.cutoffLog = p.cutoffLog;
    s.resonanceCv = p.resonanceCv;
    s.resonanceMinQ = p.resonanceMinQ;
    s.resonanceMaxQ = p.resonanceMaxQ;
    s.resonanceLog = p.resonanceLog;
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
    else if (strcmp(name, "slope") == 0)
        s.slopeDb = static_cast<uint8_t>(poleCount(static_cast<uint8_t>(clampf(value, 6.0f, 36.0f))) * 6);
    else if (strcmp(name, "global") == 0)
        s.global = value >= 0.5f;
    else
        return false;
    s.refreshLog2Ratios();
    s.coeffsDirty = true;
    return true;
}

inline void setKind(NodeInstance &inst, FilterKind kind)
{
    inst.curtisFilter->kind = kind;
    inst.curtisFilter->coeffsDirty = true;
}

inline void parsePreset(cJSON *p, WidgetParams &out)
{
    // Presets older than the shared type list stored type and slope in one
    // string ("lp24", "bp12", ...) - curtisLegacy* unpacks those.
    const char *typeName = presetparser::getString(p, "filterType", "lowpass");
    out.filterType = presetparser::getBool(p, "bypass", false)
                         ? FilterKind::None
                         : curtisLegacyFilterKind(typeName);
    out.filterSlopeDb = curtisLegacySlopeDb(typeName, presetparser::getInt(p, "slope", 24));
    out.cutoffCv = presetparser::getFloat(p, "cutoff", 5.0f);
    out.cutoffMinHz = presetparser::getFloat(p, "cutoffMinHz", 20.0f);
    out.cutoffMaxHz = presetparser::getFloat(p, "cutoffMaxHz", 20000.0f);
    out.cutoffLog = strcmp(presetparser::getString(p, "cutoffCurve", "log"), "log") == 0;
    out.resonanceCv = presetparser::getFloat(p, "resonance", 5.0f);
    out.resonanceMinQ = presetparser::getFloat(p, "resonanceMinQ", 0.1f);
    out.resonanceMaxQ = presetparser::getFloat(p, "resonanceMaxQ", 20.0f);
    out.resonanceLog = strcmp(presetparser::getString(p, "resonanceCurve", "log"), "log") == 0;
    out.filterGlobal = presetparser::getBool(p, "global", false);
}

} // namespace nodes::curtisfilter

using nodes::curtisfilter::CurtisFilterState;

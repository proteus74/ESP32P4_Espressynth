#pragma once

#include <cmath>
#include <cstddef>
#include <cstring>

#include "esp_heap_caps.h"

#include "../CvConventions.h"
#include "../DspPrimitives.h"
#include "../NodeInstance.h"
#include "../PresetParser.h"
#include "../SynthConfig.h"

// RBJ-cookbook biquad filter (lowpass/highpass/bandpass) with cutoff/
// resonance CV, optional audio-rate cutoff/resonance modulation and global
// (voice-0-authoritative) mode. Heap-allocated per instance (see
// NodeInstance.h's pointer-member comment), so state, render, build,
// destroy and live-param-set all live together here.
namespace nodes::filter
{

struct FilterState
{
    Biquad biquad;
    FilterKind kind = FilterKind::LowPass;
    // Raw CV (0-10V, see CvConventions.h) - the actual cutoff/Q is only
    // computed in render() below from this value + min/max/log, when
    // coeffsDirty is set.
    float cutoffCv = 5.0f;
    float cutoffMinHz = 20.0f;
    float cutoffMaxHz = 20000.0f;
    bool cutoffLog = true;
    float resonanceCv = 5.0f;
    float resonanceMinQ = 0.1f;
    float resonanceMaxQ = 20.0f;
    bool resonanceLog = true;
    bool coeffsDirty = true;
    bool bypass = false;

    // Cached log2(max/lo) for cv::cvToRangeLogFast (see filterCutoffHz/
    // filterResonanceQ below) - avoids powf()/log2f() PER SAMPLE, as soon
    // as a genuine DSP edge evaluates cutoff/resonance sample-accurately.
    // Must be recomputed after every change to cutoffMinHz/cutoffMaxHz/
    // resonanceMinQ/resonanceMaxQ - see refreshLog2Ratios() below, called
    // by build() (initial preset load), setParam() (live update of
    // min/max) and Graph.cpp (live update of cutoffCurve/resonanceCurve).
    // Always keep current, regardless of the current cutoffLog/
    // resonanceLog value - simply not read when linear.
    float cutoffLog2Ratio = 0.0f;
    float resonanceLog2Ratio = 0.0f;
    void refreshLog2Ratios()
    {
        const float cutoffLo = cutoffMinHz <= 0.0f ? 0.0001f : cutoffMinHz;
        cutoffLog2Ratio = log2f(cutoffMaxHz / cutoffLo);
        const float resonanceLo = resonanceMinQ <= 0.0f ? 0.0001f : resonanceMinQ;
        resonanceLog2Ratio = log2f(resonanceMaxQ / resonanceLo);
    }

    // How often (in samples) the biquad coefficients are recomputed under a
    // real audio-rate cutoff/resonance edge (see render() below) - 1 =
    // sample-accurate (most CPU), up to 16 = coarser, but cheaper
    // modulation. Settings-dialog field (main.js's filterCoeffStrideSelect),
    // default 4. No effect on coeffsDirty's block-rate path (there it is
    // only recomputed on actual change anyway, no per-sample reference).
    uint8_t coeffStride = 4;
    // Last cutoff/resonance CV actually used to compute the current biquad
    // coefficients under a genuine audio-rate edge (see render() below) -
    // lets a stride tick skip setLowpass/Highpass/Bandpass entirely when the
    // CV hasn't actually changed since the last update (e.g. an ADSR
    // sitting on its sustain plateau, or a paused/idle modulation source),
    // instead of unconditionally recomputing on every stride-th sample (plan
    // section 3/5: "Unveränderte CV-Werte trotz Audio-Kabel erkennen; nur
    // geänderte Cutoff-/Q-Abbildung berechnen"). Distinct from
    // cutoffCv/resonanceCv above, which are the manual/unwired scalars.
    float lastAudioCutoffCv = 0.0f;
    float lastAudioResonanceCv = 0.0f;
    bool audioCoeffsInitialized = false;
    // Global/per-voice switch, same pattern as GainState::global: false
    // (default) = each voice applies its own cutoff/resonance modulation
    // to its own "in" signal (current behavior). true = when
    // cutoff/resonance is genuinely DSP-edge-fed on voice 0, every other
    // voice borrows voice 0's raw CV instead of its own - "in" always
    // remains each voice's own signal, only the modulation source is
    // shared (see render() below).
    bool global = false;
};

namespace
{
// Filter cutoff/resonance CV->Hz/Q, LUT-accelerated (cv::cvToRangeLogFast)
// instead of powf() in the log-curve case (default for both) - necessary
// since a genuine DSP edge on cutoff/resonance calls this conversion PER
// SAMPLE (see render() below), not just once per block as before.
inline float filterCutoffHz(const FilterState &s, float cutoffCv)
{
    const float kSampleRate = static_cast<float>(synth::kSampleRate);
    float cutoff = s.cutoffLog ? cv::cvToRangeLogFast(cutoffCv, s.cutoffMinHz, s.cutoffLog2Ratio)
                                : cv::cvToRange(cutoffCv, s.cutoffMinHz, s.cutoffMaxHz, false);
    if (cutoff < 20.0f)
        cutoff = 20.0f;
    if (cutoff > kSampleRate / 2.0f - 100.0f)
        cutoff = kSampleRate / 2.0f - 100.0f;
    return cutoff;
}

inline float filterResonanceQ(const FilterState &s, float resonanceCv)
{
    const float qCv = s.resonanceLog ? cv::cvToRangeLogFast(resonanceCv, s.resonanceMinQ, s.resonanceLog2Ratio)
                                      : cv::cvToRange(resonanceCv, s.resonanceMinQ, s.resonanceMaxQ, false);
    return qCv < 0.05f ? 0.05f : qCv;
}
} // namespace

inline void render(NodeInstance &inst, size_t frames)
{
    using namespace noderender;
    const float kSampleRate = static_cast<float>(synth::kSampleRate);
    auto &s = *inst.filter;
    // "bypass" (Port-Index 1, control-only) - same wired-CV-or-manual
    // pattern as Reverb's bypass (see ReverbNode.h). Dry passthrough skips
    // the whole biquad, exactly like an unconnected/unmodified signal.
    const bool bypass = (inst.controlConnectedPortMask & (1u << 1)) != 0
                            ? inst.controlValue[1] >= cv::kGateThresholdVolts
                            : s.bypass;
    if (bypass)
    {
        for (size_t i = 0; i < frames; ++i)
        {
            inst.outL[i] = portSample(inst, 0, i);
        }
        return;
    }
    // "cutoff"/"resonance" (Port-Index 2/3): a genuine DSP edge
    // (audioConnectedPortMask) recomputes the biquad coefficients every
    // s.coeffStride-th sample below (1-16, Settings-Dialog-Feld, Default 4
    // - see FilterState::coeffStride) - close to audio-rate filter FM/sweep
    // at coeffStride=1, affordable at any setting since
    // Biquad::setLowpass/Highpass/Bandpass (DspPrimitives.h) uses the
    // shared sine LUT instead of sinf/cosf. biquad.process() itself (the
    // actual filtering) still runs every single sample regardless - only
    // the coefficient recompute rate is reduced. 128 (kBlockSize) is evenly
    // divisible by every coeffStride in [1,16] except 3/5/6/7/9/.../15 -
    // for those the recompute phase just drifts slightly across block
    // boundaries, still correct, no audible difference. clampf() guards
    // against a stray 0 from a not-yet-migrated/corrupt preset field
    // (would otherwise be a mod-by-zero). audioConnectedPortMask is fixed
    // for the lifetime of a loaded graph (set once in GraphBuilder::build,
    // never toggled at runtime), so a widget is either always in this
    // per-sample path or never. Not wired (only manual/knob-driven *Cv
    // scalar): falls back to the existing coeffsDirty lazy path (only
    // recompute on actual change, unchanged behavior, coeffStride has no
    // effect there).
    // "global" (Port-Index 4, control-only) - same pattern as Gain's
    // "global": only takes effect when cutoff/resonance is genuinely
    // DSP-edge-fed on voice 0 - every other voice then borrows voice 0's
    // raw cutoff/resonance CV instead of its own. "in" (Port 0) always
    // stays this voice's own signal (every voice filters its own audio) -
    // only the modulation source is shared. A purely manual (unwired)
    // cutoff/resonance is already the same widget-level value on every
    // voice by construction - Global has no extra effect there.
    const bool modeFed = controlPortConnected(inst, 4) ||
                          (inst.audioConnectedPortMask & static_cast<uint8_t>(1u << 4)) != 0;
    const bool globalActive = modeFed ? (portSample(inst, 4, 0) >= cv::kGateThresholdVolts) : s.global;
    const NodeInstance &src = (globalActive && inst.primary != nullptr) ? *inst.primary : inst;
    const bool cutoffAudioFed = (src.audioConnectedPortMask & static_cast<uint8_t>(1u << 2)) != 0;
    const bool resonanceAudioFed = (src.audioConnectedPortMask & static_cast<uint8_t>(1u << 3)) != 0;
    if (cutoffAudioFed || resonanceAudioFed)
    {
        const size_t coeffStride = static_cast<size_t>(clampf(static_cast<float>(s.coeffStride), 1.0f, 16.0f));
        size_t nextCoeffSample = 0;
        for (size_t i = 0; i < frames; ++i)
        {
            if (i == nextCoeffSample)
            {
                nextCoeffSample += coeffStride;
                const float cutoffCv = cutoffAudioFed ? src.inPorts[2][i] : s.cutoffCv;
                const float resonanceCv = resonanceAudioFed ? src.inPorts[3][i] : s.resonanceCv;
                if (!s.audioCoeffsInitialized || cutoffCv != s.lastAudioCutoffCv ||
                    resonanceCv != s.lastAudioResonanceCv)
                {
                    const float q = filterResonanceQ(s, resonanceCv);
                    const float cutoff = filterCutoffHz(s, cutoffCv);
                    switch (s.kind)
                    {
                    case FilterKind::LowPass:
                        s.biquad.setLowpass(cutoff, q, kSampleRate);
                        break;
                    case FilterKind::HighPass:
                        s.biquad.setHighpass(cutoff, q, kSampleRate);
                        break;
                    case FilterKind::BandPass:
                        s.biquad.setBandpass(cutoff, q, kSampleRate);
                        break;
                    }
                    s.lastAudioCutoffCv = cutoffCv;
                    s.lastAudioResonanceCv = resonanceCv;
                    s.audioCoeffsInitialized = true;
                }
            }
            inst.outL[i] = s.biquad.process(portSample(inst, 0, i));
        }
        return;
    }
    if (s.coeffsDirty)
    {
        const float q = filterResonanceQ(s, s.resonanceCv);
        const float cutoff = filterCutoffHz(s, s.cutoffCv);
        switch (s.kind)
        {
        case FilterKind::LowPass:
            s.biquad.setLowpass(cutoff, q, kSampleRate);
            break;
        case FilterKind::HighPass:
            s.biquad.setHighpass(cutoff, q, kSampleRate);
            break;
        case FilterKind::BandPass:
            s.biquad.setBandpass(cutoff, q, kSampleRate);
            break;
        }
        s.coeffsDirty = false;
    }
    for (size_t i = 0; i < frames; ++i)
    {
        inst.outL[i] = s.biquad.process(portSample(inst, 0, i));
    }
}

inline void build(NodeInstance &inst, const WidgetParams &p)
{
    inst.filter->kind = p.filterType;
    inst.filter->cutoffCv = p.cutoffCv;
    inst.filter->cutoffMinHz = p.cutoffMinHz;
    inst.filter->cutoffMaxHz = p.cutoffMaxHz;
    inst.filter->cutoffLog = p.cutoffLog;
    inst.filter->resonanceCv = p.resonanceCv;
    inst.filter->resonanceMinQ = p.resonanceMinQ;
    inst.filter->resonanceMaxQ = p.resonanceMaxQ;
    inst.filter->resonanceLog = p.resonanceLog;
    inst.filter->coeffStride = p.coeffStride;
    inst.filter->global = p.filterGlobal;
    inst.filter->bypass = p.filterBypass;
    inst.filter->refreshLog2Ratios();
    inst.filter->coeffsDirty = true;
}

inline void destroy(NodeInstance &inst)
{
    if (inst.filter != nullptr)
    {
        heap_caps_free(inst.filter);
        inst.filter = nullptr;
    }
}

inline bool setParam(NodeInstance &inst, const char *name, float value)
{
    if (name == nullptr)
    {
        return false;
    }
    // "cutoff"/"resonance" are raw 0-10V CV now (see CvConventions.h) -
    // stored as-is, converted to Hz/Q lazily in render() when coeffsDirty
    // is consumed. min/max/log are the per-widget curve settings from the
    // Filter's settings dialog (main.js openFilterSettings), pushed the
    // same way via a live param update.
    if (strcmp(name, "cutoff") == 0)
    {
        inst.filter->cutoffCv = value;
        inst.filter->coeffsDirty = true;
        return true;
    }
    if (strcmp(name, "cutoffMinHz") == 0)
    {
        inst.filter->cutoffMinHz = value;
        inst.filter->refreshLog2Ratios();
        inst.filter->coeffsDirty = true;
        return true;
    }
    if (strcmp(name, "cutoffMaxHz") == 0)
    {
        inst.filter->cutoffMaxHz = value;
        inst.filter->refreshLog2Ratios();
        inst.filter->coeffsDirty = true;
        return true;
    }
    if (strcmp(name, "resonance") == 0)
    {
        inst.filter->resonanceCv = value;
        inst.filter->coeffsDirty = true;
        return true;
    }
    if (strcmp(name, "resonanceMinQ") == 0)
    {
        inst.filter->resonanceMinQ = value;
        inst.filter->refreshLog2Ratios();
        inst.filter->coeffsDirty = true;
        return true;
    }
    if (strcmp(name, "resonanceMaxQ") == 0)
    {
        inst.filter->resonanceMaxQ = value;
        inst.filter->refreshLog2Ratios();
        inst.filter->coeffsDirty = true;
        return true;
    }
    if (strcmp(name, "coeffStride") == 0)
    {
        const int clamped = static_cast<int>(value) < 1 ? 1 : (static_cast<int>(value) > 16 ? 16 : static_cast<int>(value));
        inst.filter->coeffStride = static_cast<uint8_t>(clamped);
        return true;
    }
    if (strcmp(name, "bypass") == 0)
    {
        inst.filter->bypass = value >= 0.5f;
        return true;
    }
    // "cutoffCurve"/"resonanceCurve" are strings ("linear"/"log"), same
    // pattern as Gain's "curve" - see Graph.cpp's applyLiveParams.
    return false;
}

inline void parsePreset(cJSON *p, WidgetParams &out)
{
    out.filterType = filterKindFromString(presetparser::getString(p, "filterType", "lowpass"));
    // "cutoff"/"resonance" are now 0-10V CV (see CvConventions.h), only
    // converted to real Hz/Q in render() via cutoffMinHz/cutoffMaxHz/
    // cutoffLog - default CV 5V (scale center).
    out.cutoffCv = presetparser::getFloat(p, "cutoff", 5.0f);
    out.cutoffMinHz = presetparser::getFloat(p, "cutoffMinHz", 20.0f);
    out.cutoffMaxHz = presetparser::getFloat(p, "cutoffMaxHz", 20000.0f);
    out.cutoffLog = strcmp(presetparser::getString(p, "cutoffCurve", "log"), "log") == 0;
    out.resonanceCv = presetparser::getFloat(p, "resonance", 5.0f);
    out.resonanceMinQ = presetparser::getFloat(p, "resonanceMinQ", 0.1f);
    out.resonanceMaxQ = presetparser::getFloat(p, "resonanceMaxQ", 20.0f);
    out.resonanceLog = strcmp(presetparser::getString(p, "resonanceCurve", "log"), "log") == 0;
    {
        const int coeffStride = presetparser::getInt(p, "coeffStride", 4);
        out.coeffStride = static_cast<uint8_t>(coeffStride < 1 ? 1 : (coeffStride > 16 ? 16 : coeffStride));
    }
    out.filterGlobal = presetparser::getBool(p, "global", false);
    out.filterBypass = presetparser::getBool(p, "bypass", false);
}

} // namespace nodes::filter

using nodes::filter::FilterState;

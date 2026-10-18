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

// 6 dB per pole, up to six poles (cv::kFilterMaxPoles) - two poles per biquad.
constexpr int kFilterMaxSections = cv::kFilterMaxPoles / 2;

struct FilterState
{
    // Identically-configured biquad sections, cascaded according to slopeDb
    // (12 dB per section). An odd pole count adds onePole on top.
    Biquad biquad;
    Biquad sections[kFilterMaxSections - 1];
    OnePoleLowpass onePole;
    uint8_t slopeDb = 12;
    FilterKind kind = FilterKind::LowPass;
    FilterKind renderedKind = FilterKind::None;
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

using cv::filterKindFromCv;
using cv::slopeDbFromCv;

// Only low-/highpass have a meaningful single-pole section; every other type is
// inherently second-order, so an odd pole count rounds up to the next full
// biquad there instead of adding a one-pole stage.
inline bool kindSupportsOnePole(FilterKind kind)
{
    return kind == FilterKind::LowPass || kind == FilterKind::HighPass;
}

inline int polesFromSlope(uint8_t slopeDb)
{
    const int poles = slopeDb / 6;
    return poles < 1 ? 1 : (poles > cv::kFilterMaxPoles ? cv::kFilterMaxPoles : poles);
}

inline int biquadCount(FilterKind kind, uint8_t slopeDb)
{
    const int poles = polesFromSlope(slopeDb);
    const int count = kindSupportsOnePole(kind) ? poles / 2 : (poles + 1) / 2;
    return count < 1 && !kindSupportsOnePole(kind) ? 1 : count;
}

inline bool usesOnePole(FilterKind kind, uint8_t slopeDb)
{
    return kindSupportsOnePole(kind) && (polesFromSlope(slopeDb) % 2) != 0;
}

// Configures every cascaded section identically; how many of them actually run
// is decided by the slope in render() below.
inline void applyCoeffs(FilterState &s, FilterKind kind, float cutoffCv, float resonanceCv, float sampleRate)
{
    const float cutoff = filterCutoffHz(s, cutoffCv);
    s.onePole.setCutoff(cutoff, sampleRate);
    Biquad *stages[kFilterMaxSections] = {&s.biquad, &s.sections[0], &s.sections[1]};
    const int count = biquadCount(kind, s.slopeDb);
    if (count < 1)
        return;
    // Every section is configured identically: compute once on the first,
    // copy to the rest (sinCosW0 + two divisions per section otherwise, up
    // to 44100/coeffStride times per second per voice under audio-rate
    // cutoff modulation).
    if (isShelfKind(kind))
    {
        // Split across the active sections so the total shelf lift stays put
        // when the slope changes.
        const float gainDb = shelfGainDbFromCv(resonanceCv) / static_cast<float>(count);
        if (kind == FilterKind::LowShelf)
            stages[0]->setLowShelf(cutoff, gainDb, sampleRate);
        else
            stages[0]->setHighShelf(cutoff, gainDb, sampleRate);
    }
    else
    {
        const float q = filterResonanceQ(s, resonanceCv);
        switch (kind)
        {
        case FilterKind::LowPass:
            stages[0]->setLowpass(cutoff, q, sampleRate);
            break;
        case FilterKind::HighPass:
            stages[0]->setHighpass(cutoff, q, sampleRate);
            break;
        case FilterKind::BandPass:
            stages[0]->setBandpass(cutoff, q, sampleRate);
            break;
        case FilterKind::Notch:
            stages[0]->setNotch(cutoff, q, sampleRate);
            break;
        case FilterKind::AllPass:
            stages[0]->setAllpass(cutoff, q, sampleRate);
            break;
        default:
            break;
        }
    }
    for (int i = 1; i < count; ++i)
        stages[i]->copyCoeffsFrom(*stages[0]);
}

// Chain shape (section count / one-pole tail) is block-constant: resolved
// once per render() instead of recomputing biquadCount()'s division and
// usesOnePole()'s modulo, and rebuilding stages[], on every sample.
struct ChainShape
{
    Biquad *stages[kFilterMaxSections];
    int count;
    bool onePole;
    bool highPass;
};

inline ChainShape chainShape(FilterState &s, FilterKind kind)
{
    return {{&s.biquad, &s.sections[0], &s.sections[1]},
            biquadCount(kind, s.slopeDb),
            usesOnePole(kind, s.slopeDb),
            kind == FilterKind::HighPass};
}

// Two/three sections in ONE loop, skewed by one sample per section
// (section k works on sample i-k). A biquad's recurrence (y -> z1 -> next
// y) is a serial FPU-latency chain of ~3 dependent ops per sample; running
// the sections back to back (one pass each, or nested per sample) just
// strings those chains together. Skewing makes the per-iteration work of
// the sections mutually independent so the in-order core can interleave
// them. Same arithmetic, same order per section - bit-identical output.
// in == out allowed: in[i] is read before out[i-k] (k >= 1) is written.
inline void cascade2(Biquad &s0, Biquad &s1, const float *in, float *out, size_t n)
{
    Biquad::Regs r0 = s0.load(), r1 = s1.load();
    float y0 = Biquad::step(r0, in[0]);
    for (size_t i = 1; i < n; ++i)
    {
        const float y0n = Biquad::step(r0, in[i]);
        out[i - 1] = Biquad::step(r1, y0);
        y0 = y0n;
    }
    out[n - 1] = Biquad::step(r1, y0);
    s0.store(r0);
    s1.store(r1);
}

inline void cascade3(Biquad &s0, Biquad &s1, Biquad &s2, const float *in, float *out, size_t n)
{
    Biquad::Regs r0 = s0.load(), r1 = s1.load(), r2 = s2.load();
    float y0 = Biquad::step(r0, in[0]);
    if (n == 1)
    {
        out[0] = Biquad::step(r2, Biquad::step(r1, y0));
    }
    else
    {
        float y1 = Biquad::step(r1, y0);
        y0 = Biquad::step(r0, in[1]);
        for (size_t i = 2; i < n; ++i)
        {
            const float y0n = Biquad::step(r0, in[i]);
            const float y1n = Biquad::step(r1, y0);
            out[i - 2] = Biquad::step(r2, y1);
            y0 = y0n;
            y1 = y1n;
        }
        out[n - 2] = Biquad::step(r2, y1);
        out[n - 1] = Biquad::step(r2, Biquad::step(r1, y0));
    }
    s0.store(r0);
    s1.store(r1);
    s2.store(r2);
}

// Runs the cascade over n samples in block form (register-resident
// coefficients/state) instead of sample by sample through stages[]
// pointers. in == out allowed; the caller guarantees in != nullptr, n > 0.
inline void processChainBlock(FilterState &s, const ChainShape &c, const float *in, float *out, size_t n)
{
    switch (c.count)
    {
    case 3:
        cascade3(*c.stages[0], *c.stages[1], *c.stages[2], in, out, n);
        break;
    case 2:
        cascade2(*c.stages[0], *c.stages[1], in, out, n);
        break;
    case 1:
        c.stages[0]->processBlock(in, out, n);
        break;
    default:
        if (in != out)
            memcpy(out, in, n * sizeof(float));
        break;
    }
    if (c.onePole)
    {
        for (size_t i = 0; i < n; ++i)
        {
            const float lp = s.onePole.process(out[i]);
            out[i] = c.highPass ? out[i] - lp : lp;
        }
    }
}

// Once per block - see Biquad::process's comment on why not per sample.
inline void flushChain(const ChainShape &c)
{
    for (int i = 0; i < c.count; ++i)
        c.stages[i]->flushState();
}
} // namespace

inline void render(NodeInstance &inst, size_t frames)
{
    using namespace noderender;
    const float kSampleRate = static_cast<float>(synth::kSampleRate);
    auto &s = *inst.filter;
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
    // "slope" (Port-Index 2) - same discrete 0-10V selector treatment as
    // "filterType" above, six steps of 6 dB (see filterSlopeFromCv in main.js).
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
    // "global" (Port-Index 5, control-only) - same pattern as Gain's
    // "global": only takes effect when cutoff/resonance is genuinely
    // DSP-edge-fed on voice 0 - every other voice then borrows voice 0's
    // raw cutoff/resonance CV instead of its own. "in" (Port 0) always
    // stays this voice's own signal (every voice filters its own audio) -
    // only the modulation source is shared. A purely manual (unwired)
    // cutoff/resonance is already the same widget-level value on every
    // voice by construction - Global has no extra effect there.
    const bool modeFed = controlPortConnected(inst, 5) ||
                          (inst.audioConnectedPortMask & static_cast<uint8_t>(1u << 5)) != 0;
    const bool globalActive = modeFed ? (portSample(inst, 5, 0) >= cv::kGateThresholdVolts) : s.global;
    const NodeInstance &src = (globalActive && inst.primary != nullptr) ? *inst.primary : inst;
    const bool cutoffAudioFed = (src.audioConnectedPortMask & static_cast<uint8_t>(1u << 3)) != 0;
    const bool resonanceAudioFed = (src.audioConnectedPortMask & static_cast<uint8_t>(1u << 4)) != 0;
    // Input pointer resolved once: portSample()'s per-sample inPorts[] reload
    // can't be hoisted by the compiler because the biquad state writes
    // through the same NodeInstance (aliasing).
    const float *in = inst.inPorts[0];
    float *out = inst.outL;
    if (in == nullptr)
    {
        // No cable: the input is one block-constant value - materialize it
        // in outL and filter in place, so the block path below has a buffer.
        const float inScalar = inst.controlValue[0];
        for (size_t i = 0; i < frames; ++i)
            out[i] = inScalar;
        in = out;
    }
    const ChainShape chain = chainShape(s, kind);
    if (cutoffAudioFed || resonanceAudioFed)
    {
        // Coefficients are constant inside each coeffStride-long chunk, so
        // the cascade runs per chunk in block form - same update points and
        // same per-sample math as the previous sample loop.
        const size_t coeffStride = static_cast<size_t>(clampf(static_cast<float>(s.coeffStride), 1.0f, 16.0f));
        for (size_t i = 0; i < frames; i += coeffStride)
        {
            const float cutoffCv = cutoffAudioFed ? src.inPorts[3][i] : s.cutoffCv;
            const float resonanceCv = resonanceAudioFed ? src.inPorts[4][i] : s.resonanceCv;
            if (!s.audioCoeffsInitialized || cutoffCv != s.lastAudioCutoffCv ||
                resonanceCv != s.lastAudioResonanceCv)
            {
                applyCoeffs(s, kind, cutoffCv, resonanceCv, kSampleRate);
                s.lastAudioCutoffCv = cutoffCv;
                s.lastAudioResonanceCv = resonanceCv;
                s.audioCoeffsInitialized = true;
            }
            const size_t n = frames - i < coeffStride ? frames - i : coeffStride;
            processChainBlock(s, chain, in + i, out + i, n);
        }
        flushChain(chain);
        return;
    }
    if (s.coeffsDirty)
    {
        applyCoeffs(s, kind, s.cutoffCv, s.resonanceCv, kSampleRate);
        s.coeffsDirty = false;
    }
    processChainBlock(s, chain, in, out, frames);
    flushChain(chain);
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
    inst.filter->slopeDb = p.filterSlopeDb;
    inst.filter->global = p.filterGlobal;
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
    if (strcmp(name, "slope") == 0)
    {
        const int poles = static_cast<int>(value) / 6;
        inst.filter->slopeDb = static_cast<uint8_t>((poles < 1 ? 1 : (poles > cv::kFilterMaxPoles ? cv::kFilterMaxPoles : poles)) * 6);
        for (Biquad &section : inst.filter->sections)
            section.reset();
        inst.filter->coeffsDirty = true;
        inst.filter->audioCoeffsInitialized = false;
        return true;
    }
    if (strcmp(name, "coeffStride") == 0)
    {
        const int clamped = static_cast<int>(value) < 1 ? 1 : (static_cast<int>(value) > 16 ? 16 : static_cast<int>(value));
        inst.filter->coeffStride = static_cast<uint8_t>(clamped);
        return true;
    }
    // "cutoffCurve"/"resonanceCurve" are strings ("linear"/"log"), same
    // pattern as Gain's "curve" - see Graph.cpp's applyLiveParams.
    return false;
}

inline void parsePreset(cJSON *p, WidgetParams &out)
{
    out.filterType = presetparser::getBool(p, "bypass", false)
                         ? FilterKind::None
                         : filterKindFromString(presetparser::getString(p, "filterType", "lowpass"));
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
    {
        const int poles = presetparser::getInt(p, "slope", 12) / 6;
        out.filterSlopeDb = static_cast<uint8_t>((poles < 1 ? 1 : (poles > cv::kFilterMaxPoles ? cv::kFilterMaxPoles : poles)) * 6);
    }
    out.filterGlobal = presetparser::getBool(p, "global", false);
}

} // namespace nodes::filter

using nodes::filter::FilterState;

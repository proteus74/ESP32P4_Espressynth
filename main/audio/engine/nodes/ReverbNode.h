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

// Freeverb-style algorithmic reverb: full stereo ReverbState (8 combs + 4
// allpasses per channel, pre-delay) and the CPU-cheap mono ReverbLightState
// (4 combs + 2 allpasses, no pre-delay) - grouped together since
// ReverbLight is a straight cut-down variant sharing every preset field
// (minus preDelay) and every live-param name with Reverb. Both are heap-
// allocated per instance (see NodeInstance.h's pointer-member comment).
namespace nodes::reverb
{

// True stereo Freeverb (Schroeder/Moorer: 8 parallel combs + 4 series
// allpasses per channel, one-pole damping lowpass), plus a short pre-delay.
// Right-channel combs/allpasses use a fixed sample offset from the left
// ("stereo spread") so the two channels decorrelate into a real stereo tail
// instead of a panned-mono one - the classic Freeverb trick. Param semantics
// (room_size/damping 0..1, independent wet/dry level in dB, pre-delay in ms)
// follow Espressif's esp_ae_reverb documentation even though this is a
// from-scratch implementation (esp_audio_effects's reverb module needs
// ESP32-P4 chip revision >= 3.0, this board is v1.3 - see idf_component.yml).
struct ReverbState
{
    static constexpr int kCombs = 8;
    static constexpr int kAllpasses = 4;

    DelayLine combsL[kCombs];
    DelayLine combsR[kCombs];
    DelayLine allpassesL[kAllpasses];
    DelayLine allpassesR[kAllpasses];
    float combGain[kCombs] = {}; // shared feedback gain per comb slot (derived from roomSize), reused for L+R
    DelayLine preDelayL;
    DelayLine preDelayR;
    uint32_t preDelaySamples = 0;
    OnePoleLowpass dampingL;
    OnePoleLowpass dampingR;
    float roomSize = 0.5f;    // 0..1
    float damping = 0.5f;     // 0..1
    float wetLevelDb = -6.0f; // -96..0 dB
    float dryLevelDb = 0.0f;  // -96..0 dB
    float wetGain = 0.5011872f;
    float dryGain = 1.0f;
    float preDelayMs = 0.0f;  // 0..200
    bool bypass = false;
    // "roomSize"/"damping"/"wetLevel"/"dryLevel"/"preDelay" are real, but
    // control-only audio-rate CV ports (see PortTables.h) - *Min/*Max/*Log
    // mirror main.js's settings dialog (openEffectCvSettings/
    // EFFECT_CV_PARAMS), only used when the respective port is wired via a
    // DSP edge (see render() below).
    float roomSizeMin = 0.0f, roomSizeMax = 1.0f;
    bool roomSizeLog = false;
    float dampingMin = 0.0f, dampingMax = 1.0f;
    bool dampingLog = false;
    float wetLevelMin = -96.0f, wetLevelMax = 0.0f;
    bool wetLevelLog = false;
    float dryLevelMin = -96.0f, dryLevelMax = 0.0f;
    bool dryLevelLog = false;
    float preDelayMin = 0.0f, preDelayMax = 200.0f;
    bool preDelayLog = false;
};

// CPU-cheap reverb variant: ONE shared mono wet path (instead of separate
// L/R tanks like ReverbState) with fewer combs/allpasses and no pre-delay -
// ~12 instead of ~52 DelayLine accesses/sample. Dry stays stereo (see
// render() below). See optimization plan
// .claude/plans/schaue-die-die-ganze-zany-moore.md for the derivation.
struct ReverbLightState
{
    static constexpr int kCombs = 4;
    static constexpr int kAllpasses = 2;

    DelayLine combs[kCombs];
    DelayLine allpasses[kAllpasses];
    float combGain[kCombs] = {};
    OnePoleLowpass dampingFilter;
    float roomSize = 0.5f;    // 0..1
    float damping = 0.5f;     // 0..1
    float wetLevelDb = -6.0f; // -96..0 dB
    float dryLevelDb = 0.0f;  // -96..0 dB
    float wetGain = 0.5011872f;
    float dryGain = 1.0f;
    bool bypass = false;
    // Same field-name convention as ReverbState - see parsePresetReverbLight/
    // buildReverbLight (shares WidgetParams' roomSize*/damping*/wetLevel*/
    // dryLevel* fields with ReverbState, no preDelay).
    float roomSizeMin = 0.0f, roomSizeMax = 1.0f;
    bool roomSizeLog = false;
    float dampingMin = 0.0f, dampingMax = 1.0f;
    bool dampingLog = false;
    float wetLevelMin = -96.0f, wetLevelMax = 0.0f;
    bool wetLevelLog = false;
    float dryLevelMin = -96.0f, dryLevelMax = 0.0f;
    bool dryLevelLog = false;
};

inline float reverbDbToGain(float db)
{
    return powf(10.0f, db / 20.0f);
}

inline void refreshMixGains(ReverbState &s)
{
    s.wetGain = reverbDbToGain(s.wetLevelDb);
    s.dryGain = reverbDbToGain(s.dryLevelDb);
}

inline void refreshMixGains(ReverbLightState &s)
{
    s.wetGain = reverbDbToGain(s.wetLevelDb);
    s.dryGain = reverbDbToGain(s.dryLevelDb);
}

// Re-derives ReverbState's comb feedback gains (from roomSize, one powf()
// per comb - 8 real transcendental calls) - same formula as buildReverb()
// below, re-applied to the already-allocated (fixed-length) comb buffers,
// no reinit. Split from the damping-cutoff derivation below (plan section
// 5, Reverb row: "beide Ableitungen entkoppeln") so a damping-only change
// doesn't pay for 8 redundant powf() calls, and vice versa.
inline void recomputeReverbCombGains(ReverbState &s)
{
    const float sizeSec = 0.1f + s.roomSize * 7.9f;
    for (int c = 0; c < ReverbState::kCombs; ++c)
    {
        const uint32_t len = s.combsL[c].capacitySamples();
        if (len == 0)
        {
            continue;
        }
        float g = powf(10.0f, -3.0f * static_cast<float>(len) / (static_cast<float>(synth::kSampleRate) * sizeSec));
        if (g > 0.98f)
            g = 0.98f;
        if (g < 0.0f)
            g = 0.0f;
        s.combGain[c] = g;
    }
}

inline void recomputeReverbDamping(ReverbState &s)
{
    const float dampCutoffHz = 20000.0f - s.damping * 19800.0f;
    s.dampingL.setCutoff(dampCutoffHz, static_cast<float>(synth::kSampleRate));
    s.dampingR.setCutoff(dampCutoffHz, static_cast<float>(synth::kSampleRate));
}

// Convenience wrapper for callers (buildReverb, setParamReverb) that don't
// need the split - render() below calls the two halves independently.
inline void recomputeReverbDerived(ReverbState &s)
{
    recomputeReverbCombGains(s);
    recomputeReverbDamping(s);
}

// Same split as above, for the CPU-cheap mono ReverbLightState (fewer
// combs, one shared damping filter, no pre-delay).
inline void recomputeReverbLightCombGains(ReverbLightState &s)
{
    const float sizeSec = 0.1f + s.roomSize * 7.9f;
    for (int c = 0; c < ReverbLightState::kCombs; ++c)
    {
        const uint32_t len = s.combs[c].capacitySamples();
        if (len == 0)
        {
            continue;
        }
        float g = powf(10.0f, -3.0f * static_cast<float>(len) / (static_cast<float>(synth::kSampleRate) * sizeSec));
        if (g > 0.98f)
            g = 0.98f;
        if (g < 0.0f)
            g = 0.0f;
        s.combGain[c] = g;
    }
}

inline void recomputeReverbLightDamping(ReverbLightState &s)
{
    const float dampCutoffHz = 20000.0f - s.damping * 19800.0f;
    s.dampingFilter.setCutoff(dampCutoffHz, static_cast<float>(synth::kSampleRate));
}

inline void recomputeReverbLightDerived(ReverbLightState &s)
{
    recomputeReverbLightCombGains(s);
    recomputeReverbLightDamping(s);
}

inline void renderReverb(NodeInstance &inst, size_t frames)
{
    using namespace noderender;
    const float kSampleRate = static_cast<float>(synth::kSampleRate);
    auto &s = *inst.reverb;
    // "roomSize"/"damping"/"wetLevel"/"dryLevel"/"preDelay" (Port-Index 3-7)
    // are real, but control-only audio-rate CV ports (see PortTables.h) -
    // read once per block, same pattern as Delay. roomSize/damping feed the
    // comb feedback gains/damping cutoff only indirectly
    // (recomputeReverbDerived, same formula as buildReverb()) - hence only
    // recomputed once in a bundle, not per sample.
    // Only recompute when the resolved value actually changed, not merely
    // because the CV port is connected (plan section 5, Reverb row:
    // "RoomSize/Damping werden bei angeschlossenen CVs jeden Block neu
    // abgeleitet, auch bei gleichem Wert" - the wetLevel/dryLevel handling
    // just below already got this right; roomSize/damping didn't).
    // recomputeReverbCombGains()/recomputeReverbDamping() are independent
    // (see their comment), so a damping-only change no longer pays for 8
    // redundant powf() calls in the comb-gain loop, and vice versa.
    if ((inst.audioConnectedPortMask & static_cast<uint8_t>(1u << 3)) != 0)
    {
        const float roomSize = cv::cvToRange(portSample(inst, 3, 0), s.roomSizeMin, s.roomSizeMax, s.roomSizeLog);
        if (roomSize != s.roomSize)
        {
            s.roomSize = roomSize;
            recomputeReverbCombGains(s);
        }
    }
    if ((inst.audioConnectedPortMask & static_cast<uint8_t>(1u << 4)) != 0)
    {
        const float damping = cv::cvToRange(portSample(inst, 4, 0), s.dampingMin, s.dampingMax, s.dampingLog);
        if (damping != s.damping)
        {
            s.damping = damping;
            recomputeReverbDamping(s);
        }
    }
    bool mixGainsDirty = false;
    if ((inst.audioConnectedPortMask & static_cast<uint8_t>(1u << 5)) != 0)
    {
        const float wetLevelDb = cv::cvToRange(portSample(inst, 5, 0), s.wetLevelMin, s.wetLevelMax, s.wetLevelLog);
        mixGainsDirty |= wetLevelDb != s.wetLevelDb;
        s.wetLevelDb = wetLevelDb;
    }
    if ((inst.audioConnectedPortMask & static_cast<uint8_t>(1u << 6)) != 0)
    {
        const float dryLevelDb = cv::cvToRange(portSample(inst, 6, 0), s.dryLevelMin, s.dryLevelMax, s.dryLevelLog);
        mixGainsDirty |= dryLevelDb != s.dryLevelDb;
        s.dryLevelDb = dryLevelDb;
    }
    if (mixGainsDirty)
    {
        refreshMixGains(s);
    }
    if ((inst.audioConnectedPortMask & static_cast<uint8_t>(1u << 7)) != 0)
    {
        s.preDelayMs = cv::cvToRange(portSample(inst, 7, 0), s.preDelayMin, s.preDelayMax, s.preDelayLog);
        s.preDelaySamples = static_cast<uint32_t>(s.preDelayMs * 0.001f * kSampleRate);
    }
    constexpr float kAllpassG = 0.5f;
    const bool bypass = (inst.controlConnectedPortMask & (1u << 2)) != 0
                            ? inst.controlValue[2] >= cv::kGateThresholdVolts
                            : s.bypass;
    if (bypass)
    {
        for (size_t i = 0; i < frames; ++i)
        {
            inst.outL[i] = portSample(inst, 0, i);
            inst.outR[i] = portSample(inst, 1, i);
        }
        return;
    }
    for (size_t i = 0; i < frames; ++i)
    {
        const float xl = portSample(inst, 0, i);
        const float xr = portSample(inst, 1, i);

        s.preDelayL.write(xl);
        s.preDelayR.write(xr);
        const float pdl = s.preDelayL.readExact(s.preDelaySamples);
        const float pdr = s.preDelayR.readExact(s.preDelaySamples);

        float combSumL = 0.0f, combSumR = 0.0f;
        for (int c = 0; c < ReverbState::kCombs; ++c)
        {
            const float delayedL = s.combsL[c].readOldest();
            const float delayedR = s.combsR[c].readOldest();
            s.combsL[c].write(dsp::flushDenorm(pdl + delayedL * s.combGain[c]));
            s.combsR[c].write(dsp::flushDenorm(pdr + delayedR * s.combGain[c]));
            combSumL += delayedL;
            combSumR += delayedR;
        }
        combSumL *= (1.0f / static_cast<float>(ReverbState::kCombs));
        combSumR *= (1.0f / static_cast<float>(ReverbState::kCombs));

        float apL = combSumL, apR = combSumR;
        for (int a = 0; a < ReverbState::kAllpasses; ++a)
        {
            const float bufL = s.allpassesL[a].readOldest();
            const float bufR = s.allpassesR[a].readOldest();
            const float apOutL = -kAllpassG * apL + bufL;
            const float apOutR = -kAllpassG * apR + bufR;
            s.allpassesL[a].write(dsp::flushDenorm(apL + kAllpassG * bufL));
            s.allpassesR[a].write(dsp::flushDenorm(apR + kAllpassG * bufR));
            apL = apOutL;
            apR = apOutR;
        }

        const float wetL = s.dampingL.process(apL);
        const float wetR = s.dampingR.process(apR);
        inst.outL[i] = xl * s.dryGain + wetL * s.wetGain;
        inst.outR[i] = xr * s.dryGain + wetR * s.wetGain;
    }
}

inline void renderReverbLight(NodeInstance &inst, size_t frames)
{
    using namespace noderender;
    // CPU-cheap variant: ONE shared mono wet path instead of separate L/R
    // tanks, no pre-delay - see ReverbLightState/optimization plan
    // .claude/plans/schaue-die-die-ganze-zany-moore.md. Dry stays stereo
    // (xl/xr unchanged), only the reverb tail is mixed mono.
    auto &s = *inst.reverbLight;
    // See renderReverb()'s comment above - same value-comparison gating and
    // combGains/damping split.
    if ((inst.audioConnectedPortMask & static_cast<uint8_t>(1u << 3)) != 0)
    {
        const float roomSize = cv::cvToRange(portSample(inst, 3, 0), s.roomSizeMin, s.roomSizeMax, s.roomSizeLog);
        if (roomSize != s.roomSize)
        {
            s.roomSize = roomSize;
            recomputeReverbLightCombGains(s);
        }
    }
    if ((inst.audioConnectedPortMask & static_cast<uint8_t>(1u << 4)) != 0)
    {
        const float damping = cv::cvToRange(portSample(inst, 4, 0), s.dampingMin, s.dampingMax, s.dampingLog);
        if (damping != s.damping)
        {
            s.damping = damping;
            recomputeReverbLightDamping(s);
        }
    }
    bool mixGainsDirty = false;
    if ((inst.audioConnectedPortMask & static_cast<uint8_t>(1u << 5)) != 0)
    {
        const float wetLevelDb = cv::cvToRange(portSample(inst, 5, 0), s.wetLevelMin, s.wetLevelMax, s.wetLevelLog);
        mixGainsDirty |= wetLevelDb != s.wetLevelDb;
        s.wetLevelDb = wetLevelDb;
    }
    if ((inst.audioConnectedPortMask & static_cast<uint8_t>(1u << 6)) != 0)
    {
        const float dryLevelDb = cv::cvToRange(portSample(inst, 6, 0), s.dryLevelMin, s.dryLevelMax, s.dryLevelLog);
        mixGainsDirty |= dryLevelDb != s.dryLevelDb;
        s.dryLevelDb = dryLevelDb;
    }
    if (mixGainsDirty)
    {
        refreshMixGains(s);
    }
    constexpr float kAllpassG = 0.5f;
    const bool bypass = (inst.controlConnectedPortMask & (1u << 2)) != 0
                            ? inst.controlValue[2] >= cv::kGateThresholdVolts
                            : s.bypass;
    if (bypass)
    {
        for (size_t i = 0; i < frames; ++i)
        {
            inst.outL[i] = portSample(inst, 0, i);
            inst.outR[i] = portSample(inst, 1, i);
        }
        return;
    }
    for (size_t i = 0; i < frames; ++i)
    {
        const float xl = portSample(inst, 0, i);
        const float xr = portSample(inst, 1, i);

        const float mono = (xl + xr) * 0.5f;
        float combSum = 0.0f;
        for (int c = 0; c < ReverbLightState::kCombs; ++c)
        {
            const float delayed = s.combs[c].readOldest();
            s.combs[c].write(dsp::flushDenorm(mono + delayed * s.combGain[c]));
            combSum += delayed;
        }
        combSum *= (1.0f / static_cast<float>(ReverbLightState::kCombs));

        float ap = combSum;
        for (int a = 0; a < ReverbLightState::kAllpasses; ++a)
        {
            const float buf = s.allpasses[a].readOldest();
            const float apOut = -kAllpassG * ap + buf;
            s.allpasses[a].write(dsp::flushDenorm(ap + kAllpassG * buf));
            ap = apOut;
        }

        const float wet = s.dampingFilter.process(ap);
        inst.outL[i] = xl * s.dryGain + wet * s.wetGain;
        inst.outR[i] = xr * s.dryGain + wet * s.wetGain;
    }
}

// Freeverb-style tuned comb/allpass lengths (samples @ 44.1kHz, scaled to
// the engine's actual sample rate). Right-channel lengths get a fixed
// "stereo spread" offset - see ReverbState's comment for why.
inline void buildReverb(NodeInstance &inst, const WidgetParams &p, uint16_t &psramFallbacks)
{
    static constexpr uint32_t kCombLen44k[ReverbState::kCombs] = {1116, 1188, 1277, 1356, 1422, 1496, 1557, 1617};
    static constexpr uint32_t kAllpassLen44k[ReverbState::kAllpasses] = {556, 441, 341, 225};
    static constexpr uint32_t kStereoSpread = 23;
    const float scaleF = static_cast<float>(synth::kSampleRate) / 44100.0f;

    inst.reverb->roomSize = p.reverbRoomSize;
    inst.reverb->damping = p.reverbDamping;
    inst.reverb->wetLevelDb = p.reverbWetLevelDb;
    inst.reverb->dryLevelDb = p.reverbDryLevelDb;
    inst.reverb->preDelayMs = p.reverbPreDelayMs;
    inst.reverb->bypass = p.reverbBypass;
    inst.reverb->roomSizeMin = p.roomSizeMin;
    inst.reverb->roomSizeMax = p.roomSizeMax;
    inst.reverb->roomSizeLog = p.roomSizeLog;
    inst.reverb->dampingMin = p.dampingMin;
    inst.reverb->dampingMax = p.dampingMax;
    inst.reverb->dampingLog = p.dampingLog;
    inst.reverb->wetLevelMin = p.wetLevelMin;
    inst.reverb->wetLevelMax = p.wetLevelMax;
    inst.reverb->wetLevelLog = p.wetLevelLog;
    inst.reverb->dryLevelMin = p.dryLevelMin;
    inst.reverb->dryLevelMax = p.dryLevelMax;
    inst.reverb->dryLevelLog = p.dryLevelLog;
    inst.reverb->preDelayMin = p.preDelayMin;
    inst.reverb->preDelayMax = p.preDelayMax;
    inst.reverb->preDelayLog = p.preDelayLog;
    refreshMixGains(*inst.reverb);

    // roomSize (0..1, matches esp_ae_reverb_para_t's semantics) maps onto the
    // RT60-ish "tail length in seconds" the comb feedback gain formula below
    // needs - 0.1s (short) .. 8s (huge hall).
    const float sizeSec = 0.1f + p.reverbRoomSize * 7.9f;
    for (int c = 0; c < ReverbState::kCombs; ++c)
    {
        const uint32_t lenL = static_cast<uint32_t>(static_cast<float>(kCombLen44k[c]) * scaleF + 0.5f);
        const uint32_t lenR = lenL + kStereoSpread;
        // Internal-RAM-first, same reasoning/pattern as buildReverbLight() - a comb/allpass/
        // pre-delay buffer here is a dense per-sample-accessed hot buffer (Tier 4.2 of
        // .claude/plans/schaue-die-die-ganze-zany-moore.md flagged full Reverb's ~52
        // PSRAM-accesses/sample as the densest PSRAM hotspot in the whole graph). Falls back to
        // PSRAM only if it doesn't fit - counted the same as every other allocation so a
        // preset dense enough to force this shows up in the existing "output-PSRAM fallbacks"
        // diagnostic instead of silently degrading.
        if (!inst.reverb->combsL[c].initSamples(lenL, false))
        {
            inst.reverb->combsL[c].initSamples(lenL, true);
            ++psramFallbacks;
        }
        if (!inst.reverb->combsR[c].initSamples(lenR, false))
        {
            inst.reverb->combsR[c].initSamples(lenR, true);
            ++psramFallbacks;
        }
        float g = powf(10.0f, -3.0f * static_cast<float>(lenL) / (static_cast<float>(synth::kSampleRate) * sizeSec));
        if (g > 0.98f)
            g = 0.98f;
        if (g < 0.0f)
            g = 0.0f;
        inst.reverb->combGain[c] = g;
    }
    for (int a = 0; a < ReverbState::kAllpasses; ++a)
    {
        const uint32_t lenL = static_cast<uint32_t>(static_cast<float>(kAllpassLen44k[a]) * scaleF + 0.5f);
        const uint32_t lenR = lenL + kStereoSpread;
        if (!inst.reverb->allpassesL[a].initSamples(lenL, false))
        {
            inst.reverb->allpassesL[a].initSamples(lenL, true);
            ++psramFallbacks;
        }
        if (!inst.reverb->allpassesR[a].initSamples(lenR, false))
        {
            inst.reverb->allpassesR[a].initSamples(lenR, true);
            ++psramFallbacks;
        }
    }

    // damping (0..1) -> lowpass cutoff: 0 = bright/no damping (20kHz), 1 =
    // heavily damped (200Hz), matching "higher values cause faster
    // high-frequency decay" from esp_ae_reverb_para_t's documentation.
    const float dampCutoffHz = 20000.0f - p.reverbDamping * 19800.0f;
    inst.reverb->dampingL.setCutoff(dampCutoffHz, static_cast<float>(synth::kSampleRate));
    inst.reverb->dampingR.setCutoff(dampCutoffHz, static_cast<float>(synth::kSampleRate));

    // Pre-delay buffer is always allocated at the full 200ms max so live
    // preDelayMs updates (setParamReverb) never need to reallocate.
    const uint32_t preDelayCapacity = static_cast<uint32_t>(0.2f * static_cast<float>(synth::kSampleRate)) + 1;
    if (!inst.reverb->preDelayL.initSamples(preDelayCapacity, false))
    {
        inst.reverb->preDelayL.initSamples(preDelayCapacity, true);
        ++psramFallbacks;
    }
    if (!inst.reverb->preDelayR.initSamples(preDelayCapacity, false))
    {
        inst.reverb->preDelayR.initSamples(preDelayCapacity, true);
        ++psramFallbacks;
    }
    inst.reverb->preDelaySamples =
        static_cast<uint32_t>(p.reverbPreDelayMs * 0.001f * static_cast<float>(synth::kSampleRate));
}

// CPU-cheap reverb variant: half as many combs/allpasses (mono instead of
// stereo - see ReverbLightState/render()), no pre-delay. Uses the same
// preset fields as Reverb (roomSize/damping/wetLevelDb/dryLevelDb/bypass +
// the shared roomSizeMin/Max/Log etc. - see parsePresetReverbLight below).
// Significantly smaller memory footprint than Reverb (no 200ms pre-delay
// buffers, only 6 instead of 24 delay lines) - deliberately internal-RAM-
// preferred instead of blanket PSRAM like Chorus/Delay/Reverb, with a PSRAM
// fallback only if the internal heap is currently too tight.
inline void buildReverbLight(NodeInstance &inst, const WidgetParams &p, uint16_t &psramFallbacks)
{
    static constexpr uint32_t kCombLen44k[ReverbLightState::kCombs] = {1116, 1188, 1277, 1356};
    static constexpr uint32_t kAllpassLen44k[ReverbLightState::kAllpasses] = {556, 441};
    const float scaleF = static_cast<float>(synth::kSampleRate) / 44100.0f;

    inst.reverbLight->roomSize = p.reverbRoomSize;
    inst.reverbLight->damping = p.reverbDamping;
    inst.reverbLight->wetLevelDb = p.reverbWetLevelDb;
    inst.reverbLight->dryLevelDb = p.reverbDryLevelDb;
    inst.reverbLight->bypass = p.reverbBypass;
    inst.reverbLight->roomSizeMin = p.roomSizeMin;
    inst.reverbLight->roomSizeMax = p.roomSizeMax;
    inst.reverbLight->roomSizeLog = p.roomSizeLog;
    inst.reverbLight->dampingMin = p.dampingMin;
    inst.reverbLight->dampingMax = p.dampingMax;
    inst.reverbLight->dampingLog = p.dampingLog;
    inst.reverbLight->wetLevelMin = p.wetLevelMin;
    inst.reverbLight->wetLevelMax = p.wetLevelMax;
    inst.reverbLight->wetLevelLog = p.wetLevelLog;
    inst.reverbLight->dryLevelMin = p.dryLevelMin;
    inst.reverbLight->dryLevelMax = p.dryLevelMax;
    inst.reverbLight->dryLevelLog = p.dryLevelLog;
    refreshMixGains(*inst.reverbLight);

    const float sizeSec = 0.1f + p.reverbRoomSize * 7.9f;
    for (int c = 0; c < ReverbLightState::kCombs; ++c)
    {
        const uint32_t len = static_cast<uint32_t>(static_cast<float>(kCombLen44k[c]) * scaleF + 0.5f);
        if (!inst.reverbLight->combs[c].initSamples(len, false))
        {
            inst.reverbLight->combs[c].initSamples(len, true);
            // Counted in the same outputPsramFallbacks total GraphBuilder.h logs once per
            // preset build - otherwise this specific fallback (unlike every other buffer/
            // state allocation in the engine) was invisible: it silently trades ReverbLight's
            // whole reason for existing (avoiding Reverb's ~52 PSRAM accesses/sample) for the
            // same PSRAM latency on the hot render path, with no way to tell after the fact.
            ++psramFallbacks;
        }
        float g = powf(10.0f, -3.0f * static_cast<float>(len) / (static_cast<float>(synth::kSampleRate) * sizeSec));
        if (g > 0.98f)
            g = 0.98f;
        if (g < 0.0f)
            g = 0.0f;
        inst.reverbLight->combGain[c] = g;
    }
    for (int a = 0; a < ReverbLightState::kAllpasses; ++a)
    {
        const uint32_t len = static_cast<uint32_t>(static_cast<float>(kAllpassLen44k[a]) * scaleF + 0.5f);
        if (!inst.reverbLight->allpasses[a].initSamples(len, false))
        {
            inst.reverbLight->allpasses[a].initSamples(len, true);
            ++psramFallbacks;
        }
    }

    const float dampCutoffHz = 20000.0f - p.reverbDamping * 19800.0f;
    inst.reverbLight->dampingFilter.setCutoff(dampCutoffHz, static_cast<float>(synth::kSampleRate));
}

inline void destroyReverb(NodeInstance &inst)
{
    if (inst.reverb != nullptr)
    {
        for (auto &comb : inst.reverb->combsL)
        {
            comb.freeBuffer();
        }
        for (auto &comb : inst.reverb->combsR)
        {
            comb.freeBuffer();
        }
        for (auto &allpass : inst.reverb->allpassesL)
        {
            allpass.freeBuffer();
        }
        for (auto &allpass : inst.reverb->allpassesR)
        {
            allpass.freeBuffer();
        }
        inst.reverb->preDelayL.freeBuffer();
        inst.reverb->preDelayR.freeBuffer();
        heap_caps_free(inst.reverb);
        inst.reverb = nullptr;
    }
}

inline void destroyReverbLight(NodeInstance &inst)
{
    if (inst.reverbLight != nullptr)
    {
        for (auto &comb : inst.reverbLight->combs)
        {
            comb.freeBuffer();
        }
        for (auto &allpass : inst.reverbLight->allpasses)
        {
            allpass.freeBuffer();
        }
        heap_caps_free(inst.reverbLight);
        inst.reverbLight = nullptr;
    }
}

inline bool setParamReverb(NodeInstance &inst, const char *name, float value)
{
    if (name == nullptr)
    {
        return false;
    }
    if (strcmp(name, "bypass") == 0)
    {
        inst.reverb->bypass = value >= 0.5f;
        return true;
    }
    if (strcmp(name, "roomSize") == 0)
    {
        inst.reverb->roomSize = value;
        recomputeReverbDerived(*inst.reverb);
        return true;
    }
    if (strcmp(name, "damping") == 0)
    {
        inst.reverb->damping = value;
        recomputeReverbDerived(*inst.reverb);
        return true;
    }
    if (strcmp(name, "wetLevelDb") == 0)
    {
        inst.reverb->wetLevelDb = value;
        inst.reverb->wetGain = reverbDbToGain(value);
        return true;
    }
    if (strcmp(name, "dryLevelDb") == 0)
    {
        inst.reverb->dryLevelDb = value;
        inst.reverb->dryGain = reverbDbToGain(value);
        return true;
    }
    if (strcmp(name, "preDelayMs") == 0)
    {
        inst.reverb->preDelayMs = value;
        inst.reverb->preDelaySamples =
            static_cast<uint32_t>(value * 0.001f * static_cast<float>(synth::kSampleRate));
        return true;
    }
    if (strcmp(name, "roomSizeMin") == 0) { inst.reverb->roomSizeMin = value; return true; }
    if (strcmp(name, "roomSizeMax") == 0) { inst.reverb->roomSizeMax = value; return true; }
    if (strcmp(name, "dampingMin") == 0) { inst.reverb->dampingMin = value; return true; }
    if (strcmp(name, "dampingMax") == 0) { inst.reverb->dampingMax = value; return true; }
    if (strcmp(name, "wetLevelMin") == 0) { inst.reverb->wetLevelMin = value; return true; }
    if (strcmp(name, "wetLevelMax") == 0) { inst.reverb->wetLevelMax = value; return true; }
    if (strcmp(name, "dryLevelMin") == 0) { inst.reverb->dryLevelMin = value; return true; }
    if (strcmp(name, "dryLevelMax") == 0) { inst.reverb->dryLevelMax = value; return true; }
    if (strcmp(name, "preDelayMin") == 0) { inst.reverb->preDelayMin = value; return true; }
    if (strcmp(name, "preDelayMax") == 0) { inst.reverb->preDelayMax = value; return true; }
    // "roomSizeCurve"/"dampingCurve"/"wetLevelCurve"/"dryLevelCurve"/
    // "preDelayCurve" are strings, see Graph.cpp's applyLiveParams.
    return false;
}

inline bool setParamReverbLight(NodeInstance &inst, const char *name, float value)
{
    if (name == nullptr)
    {
        return false;
    }
    if (strcmp(name, "bypass") == 0)
    {
        inst.reverbLight->bypass = value >= 0.5f;
        return true;
    }
    if (strcmp(name, "roomSize") == 0)
    {
        inst.reverbLight->roomSize = value;
        recomputeReverbLightDerived(*inst.reverbLight);
        return true;
    }
    if (strcmp(name, "damping") == 0)
    {
        inst.reverbLight->damping = value;
        recomputeReverbLightDerived(*inst.reverbLight);
        return true;
    }
    if (strcmp(name, "wetLevelDb") == 0)
    {
        inst.reverbLight->wetLevelDb = value;
        inst.reverbLight->wetGain = reverbDbToGain(value);
        return true;
    }
    if (strcmp(name, "dryLevelDb") == 0)
    {
        inst.reverbLight->dryLevelDb = value;
        inst.reverbLight->dryGain = reverbDbToGain(value);
        return true;
    }
    if (strcmp(name, "roomSizeMin") == 0) { inst.reverbLight->roomSizeMin = value; return true; }
    if (strcmp(name, "roomSizeMax") == 0) { inst.reverbLight->roomSizeMax = value; return true; }
    if (strcmp(name, "dampingMin") == 0) { inst.reverbLight->dampingMin = value; return true; }
    if (strcmp(name, "dampingMax") == 0) { inst.reverbLight->dampingMax = value; return true; }
    if (strcmp(name, "wetLevelMin") == 0) { inst.reverbLight->wetLevelMin = value; return true; }
    if (strcmp(name, "wetLevelMax") == 0) { inst.reverbLight->wetLevelMax = value; return true; }
    if (strcmp(name, "dryLevelMin") == 0) { inst.reverbLight->dryLevelMin = value; return true; }
    if (strcmp(name, "dryLevelMax") == 0) { inst.reverbLight->dryLevelMax = value; return true; }
    // "roomSizeCurve"/"dampingCurve"/"wetLevelCurve"/"dryLevelCurve" are
    // strings, see Graph.cpp's applyLiveParams.
    return false;
}

inline void parsePresetReverb(cJSON *p, WidgetParams &out)
{
    out.reverbRoomSize = presetparser::getFloat(p, "roomSize", 0.5f);
    out.reverbDamping = presetparser::getFloat(p, "damping", 0.5f);
    out.reverbWetLevelDb = presetparser::getFloat(p, "wetLevelDb", -6.0f);
    out.reverbDryLevelDb = presetparser::getFloat(p, "dryLevelDb", 0.0f);
    out.reverbPreDelayMs = presetparser::getFloat(p, "preDelayMs", 0.0f);
    out.reverbBypass = presetparser::getBool(p, "bypass", false);
    out.roomSizeMin = presetparser::getFloat(p, "roomSizeMin", 0.0f);
    out.roomSizeMax = presetparser::getFloat(p, "roomSizeMax", 1.0f);
    out.roomSizeLog = strcmp(presetparser::getString(p, "roomSizeCurve", "linear"), "log") == 0;
    out.dampingMin = presetparser::getFloat(p, "dampingMin", 0.0f);
    out.dampingMax = presetparser::getFloat(p, "dampingMax", 1.0f);
    out.dampingLog = strcmp(presetparser::getString(p, "dampingCurve", "linear"), "log") == 0;
    out.wetLevelMin = presetparser::getFloat(p, "wetLevelMin", -96.0f);
    out.wetLevelMax = presetparser::getFloat(p, "wetLevelMax", 0.0f);
    out.wetLevelLog = strcmp(presetparser::getString(p, "wetLevelCurve", "linear"), "log") == 0;
    out.dryLevelMin = presetparser::getFloat(p, "dryLevelMin", -96.0f);
    out.dryLevelMax = presetparser::getFloat(p, "dryLevelMax", 0.0f);
    out.dryLevelLog = strcmp(presetparser::getString(p, "dryLevelCurve", "linear"), "log") == 0;
    out.preDelayMin = presetparser::getFloat(p, "preDelayMin", 0.0f);
    out.preDelayMax = presetparser::getFloat(p, "preDelayMax", 200.0f);
    out.preDelayLog = strcmp(presetparser::getString(p, "preDelayCurve", "linear"), "log") == 0;
}

inline void parsePresetReverbLight(cJSON *p, WidgetParams &out)
{
    // Shares the reverb*/roomSize*/damping*/wetLevel*/dryLevel* fields with
    // Reverb (see WidgetParams) - no preDelay.
    out.reverbRoomSize = presetparser::getFloat(p, "roomSize", 0.5f);
    out.reverbDamping = presetparser::getFloat(p, "damping", 0.5f);
    out.reverbWetLevelDb = presetparser::getFloat(p, "wetLevelDb", -6.0f);
    out.reverbDryLevelDb = presetparser::getFloat(p, "dryLevelDb", 0.0f);
    out.reverbBypass = presetparser::getBool(p, "bypass", false);
    out.roomSizeMin = presetparser::getFloat(p, "roomSizeMin", 0.0f);
    out.roomSizeMax = presetparser::getFloat(p, "roomSizeMax", 1.0f);
    out.roomSizeLog = strcmp(presetparser::getString(p, "roomSizeCurve", "linear"), "log") == 0;
    out.dampingMin = presetparser::getFloat(p, "dampingMin", 0.0f);
    out.dampingMax = presetparser::getFloat(p, "dampingMax", 1.0f);
    out.dampingLog = strcmp(presetparser::getString(p, "dampingCurve", "linear"), "log") == 0;
    out.wetLevelMin = presetparser::getFloat(p, "wetLevelMin", -96.0f);
    out.wetLevelMax = presetparser::getFloat(p, "wetLevelMax", 0.0f);
    out.wetLevelLog = strcmp(presetparser::getString(p, "wetLevelCurve", "linear"), "log") == 0;
    out.dryLevelMin = presetparser::getFloat(p, "dryLevelMin", -96.0f);
    out.dryLevelMax = presetparser::getFloat(p, "dryLevelMax", 0.0f);
    out.dryLevelLog = strcmp(presetparser::getString(p, "dryLevelCurve", "linear"), "log") == 0;
}

} // namespace nodes::reverb

using nodes::reverb::ReverbState;
using nodes::reverb::ReverbLightState;

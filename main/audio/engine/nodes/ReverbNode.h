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

// Stereo plate reverb after Jon Dattorro, "Effect Design Part 1" (JAES 1997): input
// bandwidth lowpass -> four input diffusion allpasses -> a "figure-eight" tank of two
// cross-fed halves (modulated allpass -> delay -> damping lowpass -> decay -> allpass ->
// delay). Damping sits inside the feedback loop, so highs decay faster than lows, and the
// slowly modulated tank allpasses smear the loop's resonant modes - both are what keeps the
// tail from ringing metallic (the previous 4-comb Schroeder design had neither). Left and
// right wet outputs are the paper's seven decorrelated taps each.
//
// The network runs at half the audio rate (kNetRate, like the paper's own 29.7 kHz): pairs of
// input samples are averaged in, the wet output is linearly interpolated back up. That halves
// CPU and memory; the wet signal tops out around 9 kHz, which a reverb tail never misses.
//
// The browser preview (audio-engine.js, "reverb-light") is a 1:1 port - keep them in sync.
// Heap-allocated per instance (see NodeInstance.h's pointer-member comment). The widget type
// stays "reverb-light" for preset compatibility; the UI calls it "Reverb".
namespace nodes::reverb
{

// Delay lengths/taps in samples at the paper's 29761 Hz, scaled to kSampleRate at build.
namespace dattorro
{
constexpr float kRefRate = 29761.0f;
constexpr uint32_t kDecimation = 2;
constexpr float kNetRate = static_cast<float>(synth::kSampleRate) / static_cast<float>(kDecimation);
constexpr uint32_t kInputAp[4] = {142, 107, 379, 277};
constexpr float kInputApGain[4] = {0.75f, 0.75f, 0.625f, 0.625f};
constexpr uint32_t kTankModAp[2] = {672, 908};   // left, right
constexpr uint32_t kTankDelay1[2] = {4453, 4217};
constexpr uint32_t kTankAp2[2] = {1800, 2656};
constexpr uint32_t kTankDelay2[2] = {3720, 3163};
constexpr float kModExcursion = 16.0f;           // samples at kRefRate
constexpr float kModRateHz[2] = {0.9f, 0.73f};   // slightly different per side
constexpr float kDecayDiffusion1 = 0.70f;
constexpr float kInputBandwidthHz = 9000.0f; // below kNetRate's Nyquist
constexpr float kOutputScale = 0.6f;
// Output taps: {line, position}; line 0..5 = L delay1, L ap2, L delay2, R delay1, R ap2, R delay2.
struct Tap
{
    uint8_t line;
    uint32_t pos;
    float sign;
};
constexpr Tap kTapsL[7] = {{3, 266, 1.0f},  {3, 2974, 1.0f}, {4, 1913, -1.0f}, {5, 1996, 1.0f},
                           {0, 1990, -1.0f}, {1, 187, -1.0f}, {2, 1066, -1.0f}};
constexpr Tap kTapsR[7] = {{0, 353, 1.0f},  {0, 3627, 1.0f}, {1, 1228, -1.0f}, {2, 2673, 1.0f},
                           {3, 2111, -1.0f}, {4, 335, -1.0f}, {5, 121, -1.0f}};
} // namespace dattorro

struct ReverbLightState
{
    DelayLine inputAp[4];
    DelayLine tankModAp[2];
    DelayLine tankDelay1[2];
    DelayLine tankAp2[2];
    DelayLine tankDelay2[2];
    uint32_t tankModApBase[2] = {}; // nominal modulated-allpass delay (samples)
    float modExcursion = 0.0f;
    float modPhase[2] = {0.0f, 0.25f};
    float modInc[2] = {};
    uint32_t tapPosL[7] = {};
    uint32_t tapPosR[7] = {};
    float bandwidthCoeff = 0.0f; // one-pole lowpass coefficient (input)
    float bandwidthState = 0.0f;
    float dampCoeff = 0.0f;      // one-pole lowpass coefficient inside the tank
    float dampState[2] = {};
    float decay = 0.5f;          // per decay multiplication, from roomSize (RT60)
    float decayDiffusion2 = 0.5f;
    uint32_t tankHalfSamples = 0; // at kNetRate
    float prevWetL = 0.0f;        // last network output, for the 2x upsampling interpolation
    float prevWetR = 0.0f;

    float roomSize = 0.5f;    // 0..1
    float damping = 0.5f;     // 0..1
    float wetLevelDb = -6.0f; // -96..0 dB
    float dryLevelDb = 0.0f;  // -96..0 dB
    float wetGain = 0.5011872f;
    float dryGain = 1.0f;
    bool bypass = false;
    // *Min/*Max/*Log mirror main.js's settings dialog (openEffectCvSettings/
    // EFFECT_CV_PARAMS) - see parsePresetReverbLight/buildReverbLight.
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

inline void refreshMixGains(ReverbLightState &s)
{
    s.wetGain = reverbDbToGain(s.wetLevelDb);
    s.dryGain = reverbDbToGain(s.dryLevelDb);
}

// roomSize 0..1 -> RT60 0.1..8 s (same range as before). One pass through a tank half
// applies `decay` twice (after the damping filter and at the cross-feed), so over that
// half's length the level must fall by 10^(-3 * T / RT60).
inline void recomputeReverbLightCombGains(ReverbLightState &s)
{
    const float rt60 = 0.1f + s.roomSize * 7.9f;
    const float halfSec = static_cast<float>(s.tankHalfSamples) / dattorro::kNetRate;
    float d = powf(10.0f, -3.0f * halfSec / (2.0f * rt60));
    if (d > 0.9995f) d = 0.9995f;
    if (d < 0.0f) d = 0.0f;
    s.decay = d;
    float dd2 = d + 0.15f; // Dattorro: decay diffusion 2 follows the decay
    s.decayDiffusion2 = dd2 < 0.25f ? 0.25f : (dd2 > 0.5f ? 0.5f : dd2);
}

// damping 0..1 -> in-loop lowpass cutoff 18 kHz .. 600 Hz (exponential; above the network's
// Nyquist the filter is simply fully open).
inline void recomputeReverbLightDamping(ReverbLightState &s)
{
    const float cutoffHz = 18000.0f * powf(600.0f / 18000.0f, s.damping);
    s.dampCoeff = expf(-2.0f * kDspPi * cutoffHz / dattorro::kNetRate);
}

inline void recomputeReverbLightDerived(ReverbLightState &s)
{
    recomputeReverbLightCombGains(s);
    recomputeReverbLightDamping(s);
}

namespace detail
{
// Block-local view of one DelayLine: raw buffer + a local write index, so the per-sample
// work is a load, a store and one wrap compare - no bounds clamping per access.
struct Ring
{
    float *b;
    uint32_t n;
    uint32_t w;

    explicit Ring(DelayLine &line) : b(line.data()), n(line.capacitySamples()), w(line.writeIndex()) {}
    void store(DelayLine &line) const { line.setWriteIndex(w); }
    float oldest() const { return b[w]; }
    void push(float v)
    {
        b[w] = v;
        if (++w == n) w = 0;
    }
    // Value written `pos` writes before the newest one (pos < n).
    float tap(uint32_t pos) const
    {
        uint32_t idx = w + n - 1 - pos;
        if (idx >= n) idx -= n;
        return b[idx];
    }
    float interp(float d) const
    {
        const uint32_t d0 = static_cast<uint32_t>(d);
        const float frac = d - static_cast<float>(d0);
        uint32_t i0 = w + n - 1 - d0;
        if (i0 >= n) i0 -= n;
        const uint32_t i1 = i0 == 0 ? n - 1 : i0 - 1;
        return b[i0] + (b[i1] - b[i0]) * frac;
    }
    // Allpass whose ring is exactly its delay length (oldest = v[n-D]).
    float allpass(float x, float g)
    {
        const float delayed = b[w];
        const float v = dsp::flushDenorm(x - g * delayed);
        push(v);
        return delayed + g * v;
    }
};
} // namespace detail

IRAM_ATTR inline void renderReverbLight(NodeInstance &inst, size_t frames)
{
    using namespace noderender;
    auto &s = *inst.reverbLight;
    // "roomSize"/"damping"/"wetLevel"/"dryLevel" (Port-Index 3-6) are real,
    // but control-only audio-rate CV ports (see PortTables.h) - read once
    // per block, same pattern as Delay. Only recomputed when the resolved
    // value actually changed, not merely because the CV port is connected.
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

    using namespace dattorro;
    using detail::Ring;
    if (s.inputAp[0].capacitySamples() == 0 || s.tankDelay2[1].capacitySamples() == 0)
    {
        for (size_t i = 0; i < frames; ++i)
        {
            inst.outL[i] = portSample(inst, 0, i) * s.dryGain;
            inst.outR[i] = portSample(inst, 1, i) * s.dryGain;
        }
        return; // allocation failed at build - dry only
    }
    Ring in0(s.inputAp[0]), in1(s.inputAp[1]), in2(s.inputAp[2]), in3(s.inputAp[3]);
    Ring mod[2] = {Ring(s.tankModAp[0]), Ring(s.tankModAp[1])};
    Ring d1[2] = {Ring(s.tankDelay1[0]), Ring(s.tankDelay1[1])};
    Ring ap2[2] = {Ring(s.tankAp2[0]), Ring(s.tankAp2[1])};
    Ring d2[2] = {Ring(s.tankDelay2[0]), Ring(s.tankDelay2[1])};
    const Ring *tapRings[6] = {&d1[0], &ap2[0], &d2[0], &d1[1], &ap2[1], &d2[1]};
    // Tank modulation is block-rate (a sub-Hz sine barely moves within one block).
    float modDelay[2];
    for (int side = 0; side < 2; ++side)
    {
        modDelay[side] = static_cast<float>(s.tankModApBase[side] - 1) +
                         s.modExcursion * (0.5f + 0.5f * dsp::sineLookup(s.modPhase[side]));
        s.modPhase[side] += s.modInc[side] * static_cast<float>(frames / kDecimation);
        if (s.modPhase[side] >= 1.0f) s.modPhase[side] -= 1.0f;
    }
    const float bw = s.bandwidthCoeff;
    const float damp = s.dampCoeff;
    const float decay = s.decay;
    const float dd2 = s.decayDiffusion2;
    const float wetScale = kOutputScale * s.wetGain;
    size_t i = 0;
    for (; i + 1 < frames; i += kDecimation)
    {
        const float xl0 = portSample(inst, 0, i);
        const float xr0 = portSample(inst, 1, i);
        const float xl1 = portSample(inst, 0, i + 1);
        const float xr1 = portSample(inst, 1, i + 1);
        const float mono = (xl0 + xr0 + xl1 + xr1) * 0.25f; // 2:1 decimation (boxcar)

        s.bandwidthState = dsp::flushDenorm(s.bandwidthState + (1.0f - bw) * (mono - s.bandwidthState));
        float x = in0.allpass(s.bandwidthState, kInputApGain[0]);
        x = in1.allpass(x, kInputApGain[1]);
        x = in2.allpass(x, kInputApGain[2]);
        x = in3.allpass(x, kInputApGain[3]);

        // Cross-feed: each half is fed by the other half's output (its last delay).
        const float leftOut = d2[0].oldest();
        const float rightOut = d2[1].oldest();
        const float tankIn[2] = {x + decay * rightOut, x + decay * leftOut};
        for (int side = 0; side < 2; ++side)
        {
            // Modulated decay-diffusion-1 allpass (sign-inverted coefficient, as in the paper).
            const float delayed = mod[side].interp(modDelay[side]);
            const float v = dsp::flushDenorm(tankIn[side] + kDecayDiffusion1 * delayed);
            mod[side].push(v);
            const float a = delayed - kDecayDiffusion1 * v;

            const float delay1Out = d1[side].oldest();
            d1[side].push(a);
            s.dampState[side] = dsp::flushDenorm(delay1Out + damp * (s.dampState[side] - delay1Out));
            const float c = ap2[side].allpass(s.dampState[side] * decay, dd2);
            d2[side].push(c);
        }

        float yl = 0.0f, yr = 0.0f;
        for (int t = 0; t < 7; ++t)
        {
            yl += kTapsL[t].sign * tapRings[kTapsL[t].line]->tap(s.tapPosL[t]);
            yr += kTapsR[t].sign * tapRings[kTapsR[t].line]->tap(s.tapPosR[t]);
        }
        yl *= wetScale;
        yr *= wetScale;
        // 1:2 upsampling: midpoint, then the new value.
        inst.outL[i] = xl0 * s.dryGain + (s.prevWetL + yl) * 0.5f;
        inst.outR[i] = xr0 * s.dryGain + (s.prevWetR + yr) * 0.5f;
        inst.outL[i + 1] = xl1 * s.dryGain + yl;
        inst.outR[i + 1] = xr1 * s.dryGain + yr;
        s.prevWetL = yl;
        s.prevWetR = yr;
    }
    for (; i < frames; ++i) // odd block length: hold the last wet value
    {
        inst.outL[i] = portSample(inst, 0, i) * s.dryGain + s.prevWetL;
        inst.outR[i] = portSample(inst, 1, i) * s.dryGain + s.prevWetR;
    }
    in0.store(s.inputAp[0]);
    in1.store(s.inputAp[1]);
    in2.store(s.inputAp[2]);
    in3.store(s.inputAp[3]);
    for (int side = 0; side < 2; ++side)
    {
        mod[side].store(s.tankModAp[side]);
        d1[side].store(s.tankDelay1[side]);
        ap2[side].store(s.tankAp2[side]);
        d2[side].store(s.tankDelay2[side]);
    }
}

namespace detail
{
// Small lines internal RAM first (hot, cheap), the four long tank delays (~46 KB) PSRAM first
// so they don't eat the internal heap the graph's block buffers need. Either way falls back.
inline void allocLine(DelayLine &line, uint32_t len, uint16_t &psramFallbacks)
{
    const bool preferPsram = len > 2048;
    if (!line.initSamples(len, preferPsram))
    {
        line.initSamples(len, !preferPsram);
        if (!preferPsram) ++psramFallbacks;
    }
}
} // namespace detail

inline void buildReverbLight(NodeInstance &inst, const WidgetParams &p, uint16_t &psramFallbacks)
{
    using namespace dattorro;
    auto &s = *inst.reverbLight;
    const float scale = kNetRate / kRefRate;
    auto scaled = [scale](uint32_t n) { return static_cast<uint32_t>(static_cast<float>(n) * scale + 0.5f); };

    s.roomSize = p.reverbRoomSize;
    s.damping = p.reverbDamping;
    s.wetLevelDb = p.reverbWetLevelDb;
    s.dryLevelDb = p.reverbDryLevelDb;
    s.bypass = p.reverbBypass;
    s.roomSizeMin = p.roomSizeMin;
    s.roomSizeMax = p.roomSizeMax;
    s.roomSizeLog = p.roomSizeLog;
    s.dampingMin = p.dampingMin;
    s.dampingMax = p.dampingMax;
    s.dampingLog = p.dampingLog;
    s.wetLevelMin = p.wetLevelMin;
    s.wetLevelMax = p.wetLevelMax;
    s.wetLevelLog = p.wetLevelLog;
    s.dryLevelMin = p.dryLevelMin;
    s.dryLevelMax = p.dryLevelMax;
    s.dryLevelLog = p.dryLevelLog;
    refreshMixGains(s);

    for (int a = 0; a < 4; ++a)
        detail::allocLine(s.inputAp[a], scaled(kInputAp[a]), psramFallbacks);
    s.modExcursion = kModExcursion * scale;
    for (int side = 0; side < 2; ++side)
    {
        s.tankModApBase[side] = scaled(kTankModAp[side]);
        detail::allocLine(s.tankModAp[side],
                          s.tankModApBase[side] + static_cast<uint32_t>(s.modExcursion) + 3, psramFallbacks);
        detail::allocLine(s.tankDelay1[side], scaled(kTankDelay1[side]), psramFallbacks);
        detail::allocLine(s.tankAp2[side], scaled(kTankAp2[side]), psramFallbacks);
        detail::allocLine(s.tankDelay2[side], scaled(kTankDelay2[side]), psramFallbacks);
        s.modInc[side] = kModRateHz[side] / kNetRate;
    }
    for (int t = 0; t < 7; ++t)
    {
        s.tapPosL[t] = scaled(kTapsL[t].pos);
        s.tapPosR[t] = scaled(kTapsR[t].pos);
    }
    s.tankHalfSamples = scaled(kTankModAp[0] + kTankDelay1[0] + kTankAp2[0] + kTankDelay2[0]);
    s.bandwidthCoeff = expf(-2.0f * kDspPi * kInputBandwidthHz / kNetRate);
    recomputeReverbLightDerived(s);
}

inline void destroyReverbLight(NodeInstance &inst)
{
    if (inst.reverbLight != nullptr)
    {
        auto &s = *inst.reverbLight;
        for (auto &line : s.inputAp) line.freeBuffer();
        for (int side = 0; side < 2; ++side)
        {
            s.tankModAp[side].freeBuffer();
            s.tankDelay1[side].freeBuffer();
            s.tankAp2[side].freeBuffer();
            s.tankDelay2[side].freeBuffer();
        }
        heap_caps_free(inst.reverbLight);
        inst.reverbLight = nullptr;
    }
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

inline void parsePresetReverbLight(cJSON *p, WidgetParams &out)
{
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

using nodes::reverb::ReverbLightState;

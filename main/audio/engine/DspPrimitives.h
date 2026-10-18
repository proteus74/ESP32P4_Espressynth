#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "esp_attr.h"
#include "esp_heap_caps.h"

#include "NodeTypes.h"
#include "SynthConfig.h"

// Pure DSP math, no graph/widget knowledge - each class here maps directly
// to one Web Audio API node/behavior from data/static/js/audio-engine.js.

namespace
{
constexpr float kDspPi = 3.14159265358979323846f;
}

// Shared sine lookup table (synth::kSineLutSize entries per cycle). Filled
// once by initSineLut() at startup (SynthEngine::begin(), before the render
// task is created) - no trig functions run in the audio-rate hot path after
// that. sineLookup() wraps phase to [0,1) and linearly interpolates between
// neighboring entries for sub-sample accuracy.
namespace dsp
{
// inline variable (C++17) - one shared instance across every translation
// unit that includes this header, replacing the old extern-declared-in-
// header/defined-in-.cpp split now that DspPrimitives has no .cpp.
inline float sineLutTable[synth::kSineLutSize] = {};

inline void initSineLut()
{
    for (size_t i = 0; i < synth::kSineLutSize; ++i)
    {
        sineLutTable[i] = sinf(2.0f * kDspPi * static_cast<float>(i) / static_cast<float>(synth::kSineLutSize));
    }
}

// IRAM_ATTR: confirmed hot on real ESP32-P4 hardware (see documentation/
// native-synth-optimization-plan-2026-09-09.md section 1c) - a 16-voice/
// 1-oscillator preset with every WaveGen already in its cheap constant-pitch
// fast path still spent ~21-22% of the whole block budget in this widget's
// render() alone, and disassembly showed it (and every node render()) living
// in .flash.text with no IRAM placement anywhere in the audio engine.
// Executing from IRAM instead of the flash cache removes exposure to cache
// pressure from other flash-resident code (e.g. the display's own PSRAM-
// backed framebuffer traffic, already suspected in Graph.h's allocate()
// comment) for exactly the functions called every single audio sample.
IRAM_ATTR inline float sineLookup(float phase)
{
    if (phase < 0.0f || phase >= 1.0f)
    {
        phase -= floorf(phase);
    }
    const float scaled = phase * static_cast<float>(synth::kSineLutSize);
    size_t i0 = static_cast<size_t>(scaled);
    if (i0 >= synth::kSineLutSize) // float rounding can push phase~1.0 up to the table size
    {
        i0 = synth::kSineLutSize - 1;
    }
    const size_t i1 = i0 + 1 == synth::kSineLutSize ? 0 : i0 + 1;
    const float frac = scaled - static_cast<float>(i0);
    return sineLutTable[i0] + (sineLutTable[i1] - sineLutTable[i0]) * frac;
}

// EXPERIMENTAL (not yet committed - user-requested try of the "Integer
// phase" idea documentation/fm-plan.md section 3/P3 explicitly deferred
// out of the first correction round; needs a listening test before it's
// kept). Integer-phase counterpart of sineLookup() above for PhaseOsc's
// internal uint32_t phase domain (see below): phaseInt is a 32-bit
// fixed-point fraction of one cycle (0 = phase 0.0, 2^32 wraps back to 0.0
// exactly via plain unsigned overflow - no floorf/branch needed to keep it
// in range, unlike the float phase_ this replaces). The table index is the
// top kSineLutBits bits - a pure right-shift, always exactly in
// [0, kSineLutSize) on its own, no clamp/bounds-check needed either, unlike
// sineLookup()'s float multiply+cast+clamp above (which stays untouched -
// this is a new, additional function, not a replacement; sinCosW0() below
// still calls the float version directly with its own, unrelated phase
// domain).
constexpr int constexprLog2(size_t n) { return n <= 1 ? 0 : 1 + constexprLog2(n >> 1); }
constexpr int kSineLutBits = constexprLog2(synth::kSineLutSize);
static_assert(size_t(1) << kSineLutBits == synth::kSineLutSize,
              "kSineLutSize must be a power of two for integer-phase LUT indexing");
constexpr int kSineLutFracBits = 32 - kSineLutBits;
constexpr float kSineLutFracScale = 1.0f / static_cast<float>(1u << kSineLutFracBits);

IRAM_ATTR inline float sineLookupInt(uint32_t phaseInt)
{
    const uint32_t i0 = phaseInt >> kSineLutFracBits;
    const uint32_t i1 = (i0 + 1) & (synth::kSineLutSize - 1);
    const float frac = static_cast<float>(phaseInt & ((1u << kSineLutFracBits) - 1)) * kSineLutFracScale;
    return sineLutTable[i0] + (sineLutTable[i1] - sineLutTable[i0]) * frac;
}

// Exponential VCA/envelope curve (10^v-1)/9 for v in [0,1] - see cv::vcaGain
// (CvConventions.h). Same LUT+lerp technique as sineLookup above, avoids a
// per-sample powf() on Gain's audio-rate "amount" path when the widget's
// exponential curve is selected (e.g. an ADSR driving a VCA). Filled once by
// initVcaExpLut() at startup.
inline float vcaExpLutTable[synth::kVcaExpLutSize] = {};

inline void initVcaExpLut()
{
    for (size_t i = 0; i < synth::kVcaExpLutSize; ++i)
    {
        const float v = static_cast<float>(i) / static_cast<float>(synth::kVcaExpLutSize - 1);
        vcaExpLutTable[i] = (powf(10.0f, v) - 1.0f) / 9.0f;
    }
}

IRAM_ATTR inline float vcaExpLookup(float v)
{
    if (v < 0.0f)
    {
        v = 0.0f;
    }
    else if (v > 1.0f)
    {
        v = 1.0f;
    }
    const float scaled = v * static_cast<float>(synth::kVcaExpLutSize - 1);
    size_t i0 = static_cast<size_t>(scaled);
    if (i0 >= synth::kVcaExpLutSize - 1) // float rounding can push v~1.0 up to the last index
    {
        i0 = synth::kVcaExpLutSize - 2;
    }
    const float frac = scaled - static_cast<float>(i0);
    return vcaExpLutTable[i0] + (vcaExpLutTable[i0 + 1] - vcaExpLutTable[i0]) * frac;
}

// 2^exponent for arbitrary (incl. negative) exponent - splits into integer +
// fractional exponent (2^(i+f) = 2^i * 2^f): the fractional part uses the
// same LUT+lerp technique as vcaExpLookup above, the integer part uses
// ldexpf (cheap float exponent-bit manipulation, not a second transcendental
// call). Avoids a per-sample powf() on cv::hzFromPitchCv's audio-rate path
// (CvConventions.h - an oscillator's pitchCv/tune input fed by a genuine DSP
// edge, see NodeInstance.cpp's Oscillator case). Filled once by
// initPow2Lut() at startup.
inline float pow2LutTable[synth::kPow2LutSize] = {};

inline void initPow2Lut()
{
    for (size_t i = 0; i < synth::kPow2LutSize; ++i)
    {
        const float f = static_cast<float>(i) / static_cast<float>(synth::kPow2LutSize - 1);
        pow2LutTable[i] = powf(2.0f, f);
    }
}

IRAM_ATTR inline float pow2Lookup(float exponent)
{
    const float intPartFloat = floorf(exponent);
    const int intPart = static_cast<int>(intPartFloat);
    const float frac = exponent - intPartFloat;
    const float scaled = frac * static_cast<float>(synth::kPow2LutSize - 1);
    size_t i0 = static_cast<size_t>(scaled);
    if (i0 >= synth::kPow2LutSize - 1) // float rounding can push frac~1.0 up to the last index
    {
        i0 = synth::kPow2LutSize - 2;
    }
    const float lerpFrac = scaled - static_cast<float>(i0);
    const float pow2Frac = pow2LutTable[i0] + (pow2LutTable[i0 + 1] - pow2LutTable[i0]) * lerpFrac;
    // Build the exact normal IEEE-754 power-of-two scale directly. This
    // removes libm's modff()/ldexpf() helpers from audio-rate pitch paths.
    if (intPart < -126)
        return 0.0f;
    if (intPart > 127)
        return INFINITY;
    const uint32_t scaleBits = static_cast<uint32_t>(intPart + 127) << 23;
    float integerScale;
    memcpy(&integerScale, &scaleBits, sizeof(integerScale));
    return pow2Frac * integerScale;
}

// tanh(x) lookup for audio-rate saturation. Only the positive half is
// stored because tanh is odd. Linear interpolation keeps the curve smooth;
// values outside +/-4 are saturated directly. This avoids the comparatively
// expensive libm tanhf() implementation on every sample and voice.
constexpr float kTanhLutMaxInput = 4.0f;
constexpr float kTanhLutScale = static_cast<float>(synth::kTanhLutSize - 1) / kTanhLutMaxInput;
inline float tanhLutTable[synth::kTanhLutSize] = {};

inline void initTanhLut()
{
    for (size_t i = 0; i < synth::kTanhLutSize; ++i)
    {
        const float x = static_cast<float>(i) / kTanhLutScale;
        tanhLutTable[i] = tanhf(x);
    }
}

IRAM_ATTR inline float tanhLookup(float value)
{
    const bool negative = value < 0.0f;
    const float magnitude = negative ? -value : value;
    if (magnitude >= kTanhLutMaxInput)
        return negative ? -1.0f : 1.0f;

    const float scaled = magnitude * kTanhLutScale;
    size_t i0 = static_cast<size_t>(scaled);
    if (i0 >= synth::kTanhLutSize - 1)
        i0 = synth::kTanhLutSize - 2;
    const float frac = scaled - static_cast<float>(i0);
    const float result = tanhLutTable[i0] + (tanhLutTable[i0 + 1] - tanhLutTable[i0]) * frac;
    return negative ? -result : result;
}

// Denormals (subnormal floats, |x| below ~1.18e-38) are IEEE-754-legal but
// many FPUs - RISC-V's RVF included - fall back to a much slower microcoded
// path for them. Any feedback loop that decays exponentially toward silence
// (reverb comb/allpass tanks, a one-pole/biquad's internal state, a
// self-resonant filter left ringing) asymptotically approaches 0 without
// ever hitting it exactly, so it can sit in denormal range indefinitely once
// its magnitude drops low enough - inaudible (well below -300dB) but
// expensive. Snapping anything under this threshold to exact 0.0f breaks the
// cycle before a value ever goes subnormal; used on every long-lived
// feedback state this engine keeps (Biquad/OnePoleLowpass::process,
// ReverbLight's comb+allpass writes, see NodeInstance.cpp).
inline float flushDenorm(float x) { return (x > -1e-15f && x < 1e-15f) ? 0.0f : x; }
} // namespace dsp

namespace
{
// w0/(2*pi) as a sineLookup() phase - sinf(w0)/cosf(w0) below are looked up
// from the shared sine LUT (dsp::sineLookup, quarter-cycle-shifted for
// cosine) instead of calling libm's transcendentals, so recomputing
// coefficients every sample (audio-rate cutoff/resonance modulation, see
// NodeInstance.cpp's Filter case) stays affordable across up to
// SYNTH_VOICE_COUNT simultaneous voices.
inline void sinCosW0(float cutoffHz, float sampleRate, float &sinW0, float &cosW0)
{
    const float phase = cutoffHz / sampleRate; // w0/(2*pi) = 2*pi*cutoffHz/sampleRate / (2*pi)
    sinW0 = dsp::sineLookup(phase);
    cosW0 = dsp::sineLookup(phase + 0.25f);
}
} // namespace

// RBJ-cookbook biquad, Direct Form II Transposed. Covers every filter type
// reachable from the UI per main.js: lowpass/highpass/bandpass/notch plus the
// two shelving types, which are shaped by a gain in dB rather than by Q.
class Biquad
{
public:
    void setLowpass(float cutoffHz, float q, float sampleRate)
    {
        float sinw0, cosw0;
        sinCosW0(cutoffHz, sampleRate, sinw0, cosw0);
        const float alpha = sinw0 / (2.0f * q);
        setCoeffs((1.0f - cosw0) / 2.0f, 1.0f - cosw0, (1.0f - cosw0) / 2.0f, 1.0f + alpha, -2.0f * cosw0, 1.0f - alpha);
    }

    void setHighpass(float cutoffHz, float q, float sampleRate)
    {
        float sinw0, cosw0;
        sinCosW0(cutoffHz, sampleRate, sinw0, cosw0);
        const float alpha = sinw0 / (2.0f * q);
        setCoeffs((1.0f + cosw0) / 2.0f, -(1.0f + cosw0), (1.0f + cosw0) / 2.0f, 1.0f + alpha, -2.0f * cosw0, 1.0f - alpha);
    }

    void setBandpass(float cutoffHz, float q, float sampleRate)
    {
        float sinw0, cosw0;
        sinCosW0(cutoffHz, sampleRate, sinw0, cosw0);
        const float alpha = sinw0 / (2.0f * q);
        setCoeffs(alpha, 0.0f, -alpha, 1.0f + alpha, -2.0f * cosw0, 1.0f - alpha);
    }

    // Flat magnitude, phase-only - the classic phaser/dispersion building block.
    void setAllpass(float cutoffHz, float q, float sampleRate)
    {
        float sinw0, cosw0;
        sinCosW0(cutoffHz, sampleRate, sinw0, cosw0);
        const float alpha = sinw0 / (2.0f * q);
        setCoeffs(1.0f - alpha, -2.0f * cosw0, 1.0f + alpha, 1.0f + alpha, -2.0f * cosw0, 1.0f - alpha);
    }

    void setNotch(float cutoffHz, float q, float sampleRate)
    {
        float sinw0, cosw0;
        sinCosW0(cutoffHz, sampleRate, sinw0, cosw0);
        const float alpha = sinw0 / (2.0f * q);
        setCoeffs(1.0f, -2.0f * cosw0, 1.0f, 1.0f + alpha, -2.0f * cosw0, 1.0f - alpha);
    }

    // Shelving filters take a gain in dB instead of a Q - the widget's
    // "resonance" control supplies it (see FilterNode.h and main.js).
    void setLowShelf(float cutoffHz, float gainDb, float sampleRate)
    {
        float sinw0, cosw0;
        sinCosW0(cutoffHz, sampleRate, sinw0, cosw0);
        const float a = powf(10.0f, gainDb / 40.0f);
        const float alpha = sinw0 * 0.70710678f;
        const float twoSqrtAAlpha = 2.0f * sqrtf(a) * alpha;
        setCoeffs(
            a * ((a + 1.0f) - (a - 1.0f) * cosw0 + twoSqrtAAlpha),
            2.0f * a * ((a - 1.0f) - (a + 1.0f) * cosw0),
            a * ((a + 1.0f) - (a - 1.0f) * cosw0 - twoSqrtAAlpha),
            (a + 1.0f) + (a - 1.0f) * cosw0 + twoSqrtAAlpha,
            -2.0f * ((a - 1.0f) + (a + 1.0f) * cosw0),
            (a + 1.0f) + (a - 1.0f) * cosw0 - twoSqrtAAlpha);
    }

    void setHighShelf(float cutoffHz, float gainDb, float sampleRate)
    {
        float sinw0, cosw0;
        sinCosW0(cutoffHz, sampleRate, sinw0, cosw0);
        const float a = powf(10.0f, gainDb / 40.0f);
        const float alpha = sinw0 * 0.70710678f;
        const float twoSqrtAAlpha = 2.0f * sqrtf(a) * alpha;
        setCoeffs(
            a * ((a + 1.0f) + (a - 1.0f) * cosw0 + twoSqrtAAlpha),
            -2.0f * a * ((a - 1.0f) + (a + 1.0f) * cosw0),
            a * ((a + 1.0f) + (a - 1.0f) * cosw0 - twoSqrtAAlpha),
            (a + 1.0f) - (a - 1.0f) * cosw0 + twoSqrtAAlpha,
            2.0f * ((a - 1.0f) - (a + 1.0f) * cosw0),
            (a + 1.0f) - (a - 1.0f) * cosw0 - twoSqrtAAlpha);
    }

    // No per-sample denormal flush here (was 4 compares + 2 selects per
    // sample per section) - the caller flushes once per block via
    // flushState() instead. Safe: flushDenorm's 1e-15 threshold sits ~23
    // decades above the subnormal range, and even a pole at 0.999 needs
    // >50k samples to decay that far, so a 128-sample block can never go
    // subnormal between two flushes.
    float process(float x)
    {
        const float y = b0_ * x + z1_;
        z1_ = b1_ * x - a1_ * y + z2_;
        z2_ = b2_ * x - a2_ * y;
        return y;
    }

    // Register-resident copy of one section for block loops: the compiler
    // can't keep b*/a*/z* in registers across out[] stores it cannot prove
    // non-aliasing with `this`, so block code loads once, steps on locals
    // and stores back (see processBlock below and FilterNode.h's cascades).
    struct Regs
    {
        float b0, b1, b2, a1, a2, z1, z2;
    };
    Regs load() const { return {b0_, b1_, b2_, a1_, a2_, z1_, z2_}; }
    void store(const Regs &r)
    {
        z1_ = r.z1;
        z2_ = r.z2;
    }
    static float step(Regs &r, float x)
    {
        const float y = r.b0 * x + r.z1;
        r.z1 = r.b1 * x - r.a1 * y + r.z2;
        r.z2 = r.b2 * x - r.a2 * y;
        return y;
    }

    // Block form of process(). in == out is fine (in-place cascade).
    void processBlock(const float *in, float *out, size_t n)
    {
        Regs r = load();
        for (size_t i = 0; i < n; ++i)
            out[i] = step(r, in[i]);
        store(r);
    }

    // Cascaded sections of one filter share identical coefficients - compute
    // them once on the first section, copy to the rest (state untouched).
    void copyCoeffsFrom(const Biquad &o)
    {
        b0_ = o.b0_;
        b1_ = o.b1_;
        b2_ = o.b2_;
        a1_ = o.a1_;
        a2_ = o.a2_;
    }

    void flushState()
    {
        z1_ = dsp::flushDenorm(z1_);
        z2_ = dsp::flushDenorm(z2_);
    }

    void reset()
    {
        z1_ = 0.0f;
        z2_ = 0.0f;
    }

private:
    // Reciprocal trick: 1 division instead of 5 - division is multi-cycle/
    // not pipeline-friendly on the P4 FPU compared to multiplication,
    // setCoeffs runs up to 48000x/s per voice under audio-rate cutoff/
    // resonance modulation (see NodeInstance.cpp's Filter case). Mathematically
    // identical to before (x/a0 == x*(1/a0)), purely a speed optimization.
    void setCoeffs(float b0, float b1, float b2, float a0, float a1, float a2)
    {
        const float invA0 = 1.0f / a0;
        b0_ = b0 * invA0;
        b1_ = b1 * invA0;
        b2_ = b2 * invA0;
        a1_ = a1 * invA0;
        a2_ = a2 * invA0;
    }

    float b0_ = 1.0f, b1_ = 0.0f, b2_ = 0.0f, a1_ = 0.0f, a2_ = 0.0f;
    float z1_ = 0.0f, z2_ = 0.0f;
};

// Simple one-pole lowpass, used for the reverb's damping stage.
class OnePoleLowpass
{
public:
    void setCutoff(float cutoffHz, float sampleRate) { coeff_ = expf(-2.0f * kDspPi * cutoffHz / sampleRate); }

    float process(float x)
    {
        z_ = dsp::flushDenorm(x * (1.0f - coeff_) + z_ * coeff_);
        return z_;
    }

    // Peeks the last output without advancing state - lets a caller check
    // whether a ramp toward 0 has already converged (e.g. to skip further
    // work once genuinely silent) without perturbing it.
    float current() const { return z_; }

    // Forces the smoothed state directly to `target`, bypassing the ramp.
    // For a caller that already established (via current()) that the ramp
    // has converged within its own tolerance of `target` this is a no-op in
    // effect, just removing the last epsilon of residual error - so the next
    // real change starts the ramp from the exact target instead of a stale
    // near-target remainder (see OscillatorNode.h's gain-smoothing fast
    // path).
    void snapTo(float target) { z_ = target; }

    void reset() { z_ = 0.0f; }

private:
    float coeff_ = 0.0f;
    float z_ = 0.0f;
};

// Retrigger-safe linear ADSR ramp scheduler, ported from audio-engine.js's
// cancelScheduledValues()+setValueAtTime()+two chained
// linearRampToValueAtTime() calls (gate-on: attack-to-1 then decay-to-
// sustain; gate-off: release-to-0; always anchored at the current value so
// retriggering mid-ramp doesn't click).
class RampScheduler
{
public:
    void configure(float attackSec, float decaySec, float sustainLevel, float releaseSec, float sampleRate)
    {
        attack_ = attackSec < 0.001f ? 0.001f : attackSec;
        decay_ = decaySec < 0.001f ? 0.001f : decaySec;
        sustain_ = sustainLevel;
        release_ = releaseSec < 0.001f ? 0.001f : releaseSec;
        sampleRate_ = sampleRate;
    }

    void gate(bool on)
    {
        segCount_ = 0; // cancel scheduled values, anchor at current_ (like JS's cancelScheduledValues+setValueAtTime)
        if (on)
        {
            pushSeg(1.0f, attack_);
            pushSeg(sustain_, decay_);
        }
        else
        {
            pushSeg(0.0f, release_);
        }
    }

    // advances one sample, returns the new current value
    float process()
    {
        if (segCount_ > 0)
        {
            current_ += segs_[0].inc;
            if (--segs_[0].remaining == 0)
            {
                current_ = segs_[0].target;
                segs_[0] = segs_[1];
                --segCount_;
            }
        }
        return current_;
    }

    float current() const { return current_; }

    // True once the ramp has fully settled at exact silence (no pending
    // segments, current value exactly 0) - used by Graph::renderBlock to
    // skip rendering a voice instance entirely once its envelope has really
    // reached zero (not merely "note released", which would cut off the
    // release tail early).
    bool isIdle() const { return segCount_ == 0 && current_ == 0.0f; }

    // True whenever process() would just return current_ unchanged, sample
    // after sample - no pending segment, regardless of whether current_
    // happens to be silence (isIdle() above) or a plateaued sustain level.
    // Lets a caller (AdsrNode.h's render()) fill a whole block with one
    // precomputed value instead of calling process() once per sample only
    // to hit its own no-op branch every time (plan section 5, ADSR row:
    // "Sustain und Idle als konstante Ausgänge").
    bool isHolding() const { return segCount_ == 0; }

    // True only when the CURRENT segment's remaining count covers the
    // WHOLE block - process() would compute current_ + k*segs_[0].inc for
    // every k in [0,frames) with no segment switch mid-block. False (safe
    // fallback: caller keeps materializing via the ordinary process()
    // loop) whenever a segment ends partway through, segCount_==0
    // (isHolding() already covers that faster), or frames==0. Lets a
    // caller (AdsrNode.h's render()) publish (start, inc) for Ramp-aware
    // downstream consumers instead of re-deriving the same thing via a
    // per-sample scan - see NodeInstance.h's rampPortMask.
    bool wholeBlockRamp(size_t frames, float &start, float &inc) const
    {
        if (segCount_ == 0 || frames == 0 || segs_[0].remaining <= frames)
        {
            return false;
        }
        // process() advances before returning the first sample of the block.
        start = current_ + segs_[0].inc;
        inc = segs_[0].inc;
        return true;
    }

    // Read back the currently-configured segment durations/level, so a live
    // single-field update (see ParamDispatch.cpp) can re-call configure()
    // with the other three unchanged instead of needing them passed in too.
    float attackSec() const { return attack_; }
    float decaySec() const { return decay_; }
    float sustainLevel() const { return sustain_; }
    float releaseSec() const { return release_; }

private:
    struct Seg
    {
        float target = 0.0f;
        uint32_t remaining = 0;
        float inc = 0.0f;
    };
    void pushSeg(float target, float durationSec)
    {
        if (segCount_ >= 2)
        {
            return;
        }
        uint32_t samples = static_cast<uint32_t>(durationSec * sampleRate_);
        if (samples < 1)
        {
            samples = 1;
        }
        const float base = (segCount_ == 0) ? current_ : segs_[segCount_ - 1].target;
        segs_[segCount_].target = target;
        segs_[segCount_].remaining = samples;
        segs_[segCount_].inc = (target - base) / static_cast<float>(samples);
        ++segCount_;
    }

    Seg segs_[2];
    uint8_t segCount_ = 0;
    float current_ = 0.0f;
    float attack_ = 0.1f, decay_ = 0.1f, sustain_ = 0.7f, release_ = 0.2f;
    float sampleRate_ = 44100.0f;
};

// Phase-accumulator oscillator (sine/square/triangle/sawtooth/white and pink noise).
// Non-band-limited by design (accepted known limitation, see plan).
//
// EXPERIMENTAL: phase_ is a uint32_t fixed-point fraction of one cycle
// (0 = phase 0.0, wraps back to 0.0 via plain unsigned overflow at 2^32 -
// documentation/fm-plan.md section 3/P3's "Integerphase" idea, explicitly
// deferred out of the first correction round there, now tried on user
// request). Was float phase_ in [0,1), wrapped via floorf every advance.
// Needs a listening test before being kept - not yet committed.
class PhaseOsc
{
public:
    // phaseOffset (in cycles, wraps, any sign/magnitude) is added on top of
    // the running phase_ at read time only - never folded into phase_
    // itself, so resetPhase() still means "start of cycle" and a caller can
    // re-evaluate at a different offset every call (see NodeInstance.cpp's
    // Lfo case, where this is how the "phase" param takes effect
    // immediately, including right after a sync edge, without any special-
    // casing in resetPhase()).
    IRAM_ATTR float process(WaveType wave, float freqHz, float sampleRate, float phaseOffset = 0.0f,
                             uint32_t dutyThresholdInt = 0x80000000u)
    {
        if (wave == WaveType::Noise)
        {
            return nextNoise();
        }
        if (wave == WaveType::PinkNoise)
        {
            return nextPinkNoise();
        }
        const float sample = currentSample(wave, phaseOffset, dutyThresholdInt);
        advance(freqHz, sampleRate, 1, isLinearWave(wave));
        return sample;
    }

    // dutyThresholdInt only affects WaveType::Square (see sampleAt() below) -
    // default 0x80000000 is the classic 50% square, unchanged for every
    // existing caller that doesn't pass anything (Oscillator, Lfo, every
    // FM-operator instance that hasn't set a custom duty cycle).
    IRAM_ATTR float currentSample(WaveType wave, float phaseOffset = 0.0f, uint32_t dutyThresholdInt = 0x80000000u) const
    {
        const uint32_t p = phaseOffset != 0.0f ? phase_ + toPhaseInt(phaseOffset) : phase_;
        return sampleAt(wave, p, dutyThresholdInt);
    }

    // Same as process(), but for a block where the caller has already
    // resolved pitch/tune to one constant Hz for the whole block (see
    // OscillatorNode.h's audioPortConstantForBlock fast path) and hoisted
    // `hz / sampleRate` to a single division outside the sample loop
    // (documentation/native-synth-optimization-plan-2026-09-09.md, P1 item
    // 2: "phaseIncrement = hz / sampleRate einmal vorberechnen"). Advances by
    // that precomputed increment directly - one add instead of advance()'s
    // multiply-then-add per sample. Still takes phaseIncrement as float and
    // converts every call (toPhaseInt's fmodf-based reduction) - unlike
    // processWithIncrementT below, this is on the "needsAudioRate" slow path
    // where the caller already recomputes hz fresh every sample anyway, so
    // one more per-sample conversion here is not a new cost class.
    IRAM_ATTR float processWithIncrement(WaveType wave, float phaseIncrement, float phaseOffset = 0.0f,
                                          uint32_t dutyThresholdInt = 0x80000000u)
    {
        if (wave == WaveType::Noise)
        {
            return nextNoise();
        }
        if (wave == WaveType::PinkNoise)
        {
            return nextPinkNoise();
        }
        const float sample = currentSample(wave, phaseOffset, dutyThresholdInt);
        const uint32_t inc = toPhaseInt(phaseIncrement);
        phase_ = isLinearWave(wave) ? saturatingAdd(phase_, inc) : phase_ + inc;
        return sample;
    }

    // Compile-time-specialized twin of processWithIncrement() above, for
    // oscillator fast paths. The regular oscillator instantiates the default
    // no-offset form; the FM operator selects UsePhaseOffset=true so waveform
    // and phase-offset handling are both resolved at compile time.
    // `wave` is already selected once per block by the caller (see
    // OscillatorNode.h's render()); W being a template parameter means the
    // `switch (wave)` inside currentSample() collapses to a single
    // `if constexpr` branch here, decided once at the call site (one
    // instantiation per waveform) rather than re-evaluated as a live branch
    // on every sample (plan section 3, P1 item 3: "Keine Wellenformauswahl
    // innerhalb der Sample-Schleife"). One shared definition, not six
    // hand-duplicated copies - the compiler generates the six
    // specializations from this single template.
    //
    // phaseIncrementInt is pre-converted to this class's internal 32-bit
    // domain by the CALLER (via toPhaseInt(), once per block - see
    // FmOscillatorNode.h/OscillatorNode.h's render()), not per sample here -
    // this is called once per SAMPLE with an unchanging increment across
    // the whole block, so redoing toPhaseInt()'s fmodf-based reduction on
    // every call would pay that cost right back on the hot path this and
    // P2 exist to avoid.
    template <WaveType W, bool UsePhaseOffset = false>
    IRAM_ATTR float processWithIncrementT(uint32_t phaseIncrementInt, float phaseOffset = 0.0f,
                                           uint32_t dutyThresholdInt = 0x80000000u)
    {
        if constexpr (W == WaveType::Noise)
        {
            return nextNoise();
        }
        else if constexpr (W == WaveType::PinkNoise)
        {
            return nextPinkNoise();
        }
        else
        {
            uint32_t p = phase_;
            if constexpr (UsePhaseOffset)
            {
                p += toPhaseOffsetInt(phaseOffset);
            }
            const float sample = sampleAtT<W>(p, dutyThresholdInt);
            if constexpr (W == WaveType::LinearUp || W == WaveType::LinearDown)
                phase_ = saturatingAdd(phase_, phaseIncrementInt);
            else
                phase_ += phaseIncrementInt;
            return sample;
        }
    }

    // Advances phase_ by phaseIncrementInt for `frames` samples without
    // computing any waveform sample. Unlike the float phase_ this replaces
    // (where a bulk `phase_ += frames * phaseIncrement` was NOT bit-
    // identical to `frames` individual additions under float rounding),
    // unsigned 32-bit addition is exactly associative/modular - one bulk
    // step is bit-EXACT here, no per-sample loop needed to match what
    // `frames` individual `phase_ += phaseIncrementInt` calls would have
    // produced. For a caller that already knows this block's audible
    // output is silence regardless (see FmOscillatorNode.h's P1 "stumme
    // Modulatoren" fast path, documentation/fm-plan.md section 3) but still
    // needs phase_ to land exactly where the normal per-sample loop would,
    // so the next audible block (e.g. right after a gate-on attack) picks
    // up mid-cycle instead of glitching/restarting.
    IRAM_ATTR inline void advancePhaseOnly(uint32_t phaseIncrementInt, size_t frames, bool saturate = false)
    {
        if (saturate)
            phase_ = saturatingAdd64(phase_, static_cast<uint64_t>(phaseIncrementInt) * frames);
        else
            phase_ += phaseIncrementInt * static_cast<uint32_t>(frames);
    }

    IRAM_ATTR uint32_t phaseInt() const { return phase_; }

    // Stateless saw evaluation used by FM SuperSaw's six side accumulators.
    IRAM_ATTR static float sawSampleAt(uint32_t phase)
    {
        return 2.0f * (static_cast<float>(phase) * kInvUint32Max) - 1.0f;
    }

    // Falling sawtooth: starts at +1 and ramps down to -1 over the cycle -
    // the vertical mirror of sawSampleAt() above, same phase reference.
    IRAM_ATTR static float sawDownSampleAt(uint32_t phase)
    {
        return -sawSampleAt(phase);
    }

    // sampleRate is in practice ALWAYS the same value (kSampleRate, fixed
    // 48000 - see all callers in NodeInstance.cpp) - cache the reciprocal
    // instead of dividing anew each time. The comparison is always false
    // after the first call (well-predictable branch), the public signature
    // stays unchanged.
    // saturate (linear waves): stop at the end of the cycle instead of wrapping.
    IRAM_ATTR void advance(float freqHz, float sampleRate, size_t samples = 1, bool saturate = false)
    {
        if (sampleRate != lastSampleRate_)
        {
            lastSampleRate_ = sampleRate;
            invSampleRate_ = 1.0f / sampleRate;
        }
        const uint32_t inc = toPhaseInt(freqHz * invSampleRate_);
        if (saturate)
            phase_ = saturatingAdd64(phase_, static_cast<uint64_t>(inc) * samples);
        else
            phase_ += inc * static_cast<uint32_t>(samples);
    }

    void resetPhase() { phase_ = 0u; }

    // Converts a phase value/increment/offset (in cycles - any sign or
    // magnitude; FM's phaseOffset can span several cycles, see
    // FmOscillatorNode.h's kFmIndexMaxPhaseDeviation) into this class's
    // internal 32-bit fixed-point domain (2^32 = one full cycle, wraps via
    // plain unsigned overflow). Reduce to [0,1) first, then convert either
    // half separately: RV32 can convert each [0,2^31) half directly to a
    // signed integer, while a single float->uint32 conversion over the full
    // range needs a much heavier helper. floorf compiles to an inline RISC-V
    // rounding instruction in this build; unlike fmodf it creates no
    // per-sample library call in an FM carrier.
    IRAM_ATTR static uint32_t toPhaseInt(float cycles)
    {
        const float wrapped = cycles - floorf(cycles);
        if (wrapped < 0.5f)
            return static_cast<uint32_t>(static_cast<int32_t>(wrapped * kPhaseIntScale));
        return 0x80000000u +
               static_cast<uint32_t>(static_cast<int32_t>((wrapped - 0.5f) * kPhaseIntScale));
    }

    // FM's per-sample phase offset normally stays inside a small number of
    // cycles. Convert that common range as signed 5.27 fixed point, then
    // shift into PhaseOsc's 0.32 domain. Unsigned wrap supplies the modulo
    // behavior for negative values without floorf. Float input precision is
    // already coarser than the five discarded low bits throughout this
    // range, so representable normal FM offsets retain their useful phase
    // precision. Arbitrary/extreme sources keep the general conversion.
    IRAM_ATTR static uint32_t toPhaseOffsetInt(float cycles)
    {
        constexpr float kFastLimitCycles = 16.0f;
        constexpr float kSignedPhaseScale = 134217728.0f; // 2^27
        if (cycles > -kFastLimitCycles && cycles < kFastLimitCycles)
        {
            const int32_t signedPhase = static_cast<int32_t>(cycles * kSignedPhaseScale);
            return static_cast<uint32_t>(signedPhase) << 5;
        }
        return toPhaseInt(cycles);
    }

    // Square-duty conversion is not cyclic: 0V means 0%, 10V means 100%.
    // It therefore clamps instead of using toPhaseInt(), whose deliberate
    // modulo semantics would otherwise map the upper endpoint back to zero.
    IRAM_ATTR static uint32_t dutyThresholdFromUnit(float unit)
    {
        if (unit <= 0.0f)
            return 0u;
        if (unit >= 1.0f)
            return 0xFFFFFFFFu;
        return toPhaseInt(unit);
    }

private:
    // Linear waves: reaching 0xFFFFFFFF (value exactly +/-1 at the end of the
    // ramp) and staying there until resetPhase().
    IRAM_ATTR static uint32_t saturatingAdd(uint32_t a, uint32_t b)
    {
        const uint32_t n = a + b;
        return n < a ? 0xFFFFFFFFu : n;
    }
    IRAM_ATTR static uint32_t saturatingAdd64(uint32_t a, uint64_t b)
    {
        const uint64_t n = static_cast<uint64_t>(a) + b;
        return n > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<uint32_t>(n);
    }

    // 2^32, exact in float (kPhaseIntScale) - the forward (cycles->int)
    // scale for toPhaseInt() above. Deliberately NOT the same constant as
    // kInvUint32Max below (1/(2^32-1)) - that one is the REVERSE (int-
    // >[0,1)-ish float) direction used by sampleAt()/sampleAtT() and the
    // pre-existing nextNoise(), where the 1-ULP difference from a true
    // 1/2^32 has always been negligible and isn't worth a second constant.
    static constexpr float kPhaseIntScale = 4294967296.0f;

    // Reciprocal instead of division - 4294967295.0f (UINT32_MAX) is a
    // compile-time literal, but without -ffast-math the compiler does not fold
    // it into a multiplication by itself (see CvConventions.h's
    // kInvMaxVolts comment for the same reason).
    static constexpr float kInvUint32Max = 1.0f / 4294967295.0f;

    // dutyThresholdInt (Square only): p < dutyThresholdInt is the "high"
    // (+1) portion of the cycle - 0x80000000 (phase 0.5) is the classic 50%
    // square every other waveform/caller keeps using via the default param
    // above. FM operator's "Duty Cycle" control (PortTables.h port 8, see
    // FmOscillatorNode.h) is the only current source of a non-default value.
    static IRAM_ATTR float sampleAt(WaveType wave, uint32_t p, uint32_t dutyThresholdInt = 0x80000000u)
    {
        switch (wave)
        {
        case WaveType::Sine:
            return dsp::sineLookupInt(p);
        case WaveType::Square:
            return p < dutyThresholdInt ? 1.0f : -1.0f;
        case WaveType::Triangle:
        {
            // Must mirror main.js's lfoWaveValue() triangle formula exactly
            // (4*|phase - Math.floor(phase+0.5)| - 1, phase=0 -> minimum -1,
            // peak +1 at phase=0.5) - see the float version's comment for
            // why (the old formula was phase-shifted by half a period).
            const float pf = static_cast<float>(p) * kInvUint32Max;
            const float nearest = pf < 0.5f ? 0.0f : 1.0f;
            return 4.0f * fabsf(pf - nearest) - 1.0f;
        }
        case WaveType::Sawtooth:
        case WaveType::LinearUp: // same shape as Sawtooth; the difference is saturating advance, not the sample
            return sawSampleAt(p);
        case WaveType::SawtoothDown:
        case WaveType::LinearDown:
            return sawDownSampleAt(p);
        case WaveType::Noise:
        case WaveType::PinkNoise:
        case WaveType::SuperSaw:
        default:
            return 0.0f;
        }
    }

    template <WaveType W>
    static IRAM_ATTR float sampleAtT(uint32_t p, uint32_t dutyThresholdInt = 0x80000000u)
    {
        if constexpr (W == WaveType::Sine)
        {
            return dsp::sineLookupInt(p);
        }
        else if constexpr (W == WaveType::Square)
        {
            return p < dutyThresholdInt ? 1.0f : -1.0f;
        }
        else if constexpr (W == WaveType::Triangle)
        {
            const float pf = static_cast<float>(p) * kInvUint32Max;
            const float nearest = pf < 0.5f ? 0.0f : 1.0f;
            return 4.0f * fabsf(pf - nearest) - 1.0f;
        }
        else if constexpr (W == WaveType::Sawtooth || W == WaveType::LinearUp)
        {
            return sawSampleAt(p);
        }
        else // SawtoothDown, LinearDown
        {
            return sawDownSampleAt(p);
        }
    }

    float nextNoise()
    {
        noiseState_ ^= noiseState_ << 13;
        noiseState_ ^= noiseState_ >> 17;
        noiseState_ ^= noiseState_ << 5;
        return static_cast<float>(noiseState_) * kInvUint32Max * 2.0f - 1.0f;
    }

    // Paul Kellet's economical three-pole pink-noise approximation. It
    // retains the roughly -3 dB/octave spectrum without the seven filters
    // of the full version. The final clamp keeps it in the oscillator's
    // native +/-1 waveform range like white noise and the periodic shapes.
    float nextPinkNoise()
    {
        const float white = nextNoise();
        pinkB0_ = 0.99765f * pinkB0_ + white * 0.0990460f;
        pinkB1_ = 0.96300f * pinkB1_ + white * 0.2965164f;
        pinkB2_ = 0.57000f * pinkB2_ + white * 1.0526913f;
        float pink = (pinkB0_ + pinkB1_ + pinkB2_ + white * 0.1848f) * 0.25f;
        if (pink > 1.0f)
            pink = 1.0f;
        else if (pink < -1.0f)
            pink = -1.0f;
        return pink;
    }

    uint32_t phase_ = 0u;
    uint32_t noiseState_ = 0x12345678u;
    float pinkB0_ = 0.0f;
    float pinkB1_ = 0.0f;
    float pinkB2_ = 0.0f;
    float lastSampleRate_ = 0.0f;
    float invSampleRate_ = 0.0f;
};

// Ring-buffer delay line, PSRAM- or internal-heap-backed. Used directly for
// chorus (fractional, LFO-modulated read via readInterpolated) and
// delay/echo (integer read via readExact at a runtime-variable tap), and
// with exact-length buffers for the reverb's comb/allpass network.
class DelayLine
{
public:
    bool init(float maxSeconds, float sampleRate, bool psram)
    {
        return allocate(static_cast<uint32_t>(maxSeconds * sampleRate) + 1, psram);
    }

    bool initSamples(uint32_t lengthSamples, bool psram) { return allocate(lengthSamples, psram); }

    void freeBuffer()
    {
        if (buffer_ != nullptr)
        {
            heap_caps_free(buffer_);
            buffer_ = nullptr;
        }
        length_ = 0;
        writeIdx_ = 0;
    }

    void write(float sample)
    {
        if (length_ == 0)
        {
            return;
        }
        buffer_[writeIdx_] = sample;
        if (++writeIdx_ == length_)
        {
            writeIdx_ = 0;
        }
    }

    float readExact(uint32_t delaySamples) const
    {
        if (length_ == 0)
        {
            return 0.0f;
        }
        if (delaySamples >= length_)
        {
            delaySamples = length_ - 1;
        }
        uint32_t idx = writeIdx_ + length_ - 1 - delaySamples;
        if (idx >= length_)
        {
            idx -= length_;
        }
        return buffer_[idx];
    }

    float readOldest() const
    {
        return length_ == 0 ? 0.0f : buffer_[writeIdx_];
    }

    float readInterpolated(float delaySamples) const
    {
        if (length_ < 2)
        {
            return readExact(0);
        }
        if (delaySamples < 0.0f)
        {
            delaySamples = 0.0f;
        }
        const float maxDelay = static_cast<float>(length_ - 2);
        if (delaySamples > maxDelay)
        {
            delaySamples = maxDelay;
        }
        const uint32_t d0 = static_cast<uint32_t>(delaySamples);
        const float frac = delaySamples - static_cast<float>(d0);
        uint32_t idx0 = writeIdx_ + length_ - 1 - d0;
        if (idx0 >= length_)
        {
            idx0 -= length_;
        }
        const uint32_t idx1 = idx0 == 0 ? length_ - 1 : idx0 - 1;
        const float s0 = buffer_[idx0];
        const float s1 = buffer_[idx1];
        return s0 + (s1 - s0) * frac;
    }

    uint32_t capacitySamples() const { return length_; }

    // Raw ring access for hot loops that keep their own local copy of the write index for a
    // whole block (see nodes/ReverbNode.h) - write the index back with setWriteIndex().
    float *data() { return buffer_; }
    uint32_t writeIndex() const { return writeIdx_; }
    void setWriteIndex(uint32_t idx) { writeIdx_ = idx; }

private:
    bool allocate(uint32_t lengthSamples, bool psram)
    {
        if (lengthSamples < 1)
        {
            lengthSamples = 1;
        }
        buffer_ = static_cast<float *>(heap_caps_malloc(
            static_cast<size_t>(lengthSamples) * sizeof(float),
            psram ? (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
        if (buffer_ == nullptr)
        {
            length_ = 0;
            return false;
        }
        memset(buffer_, 0, static_cast<size_t>(lengthSamples) * sizeof(float));
        length_ = lengthSamples;
        writeIdx_ = 0;
        fromPsram_ = psram;
        return true;
    }

    float *buffer_ = nullptr;
    uint32_t length_ = 0;
    uint32_t writeIdx_ = 0;
    bool fromPsram_ = false;
};

// Feed-forward soft-knee-ish limiter (threshold -1dB, ratio 20:1, hard
// knee, attack 3ms / release 250ms), linked stereo - matches audio-out's
// DynamicsCompressor defaults in audio-engine.js.
class SoftLimiter
{
public:
    void configure(float sampleRate)
    {
        attackCoeff_ = expf(-1.0f / (sampleRate * 0.003f));
        releaseCoeff_ = expf(-1.0f / (sampleRate * 0.25f));
    }

    void process(float &l, float &r)
    {
        constexpr float kThresholdLin = 0.89125094f; // -1 dB
        const float detector = fmaxf(fabsf(l), fabsf(r));
        if (detector <= kThresholdLin && smoothedGain_ == 1.0f)
        {
            return;
        }
        const float targetGainLin = detector > kThresholdLin ? powf(detector / kThresholdLin, -0.95f) : 1.0f;
        const float coeff = targetGainLin < smoothedGain_ ? attackCoeff_ : releaseCoeff_;
        smoothedGain_ = coeff * smoothedGain_ + (1.0f - coeff) * targetGainLin;
        l *= smoothedGain_;
        r *= smoothedGain_;
    }

private:
    float smoothedGain_ = 1.0f;
    float attackCoeff_ = 0.0f;
    float releaseCoeff_ = 0.0f;
};

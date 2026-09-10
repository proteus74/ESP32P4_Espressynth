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
    float intPart = 0.0f;
    float frac = modff(exponent, &intPart);
    if (frac < 0.0f)
    {
        frac += 1.0f;
        intPart -= 1.0f;
    }
    const float scaled = frac * static_cast<float>(synth::kPow2LutSize - 1);
    size_t i0 = static_cast<size_t>(scaled);
    if (i0 >= synth::kPow2LutSize - 1) // float rounding can push frac~1.0 up to the last index
    {
        i0 = synth::kPow2LutSize - 2;
    }
    const float lerpFrac = scaled - static_cast<float>(i0);
    const float pow2Frac = pow2LutTable[i0] + (pow2LutTable[i0 + 1] - pow2LutTable[i0]) * lerpFrac;
    return ldexpf(pow2Frac, static_cast<int>(intPart));
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
// feedback state this engine keeps (Biquad/OnePoleLowpass::process, Reverb/
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

// RBJ-cookbook biquad, Direct Form II Transposed (lowpass/highpass/bandpass
// only - the only filter types reachable from the UI per main.js).
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

    float process(float x)
    {
        const float y = b0_ * x + z1_;
        z1_ = dsp::flushDenorm(b1_ * x - a1_ * y + z2_);
        z2_ = dsp::flushDenorm(b2_ * x - a2_ * y);
        return y;
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
        if (segCount_ == 0 || frames == 0 || segs_[0].remaining < frames)
        {
            return false;
        }
        start = current_;
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
class PhaseOsc
{
public:
    // phaseOffset (0..1, wraps) is added on top of the running phase_ at
    // read time only - never folded into phase_ itself, so resetPhase()
    // still means "start of cycle" and a caller can re-evaluate at a
    // different offset every call (see NodeInstance.cpp's Lfo case, where
    // this is how the "phase" param takes effect immediately, including
    // right after a sync edge, without any special-casing in resetPhase()).
    IRAM_ATTR float process(WaveType wave, float freqHz, float sampleRate, float phaseOffset = 0.0f)
    {
        if (wave == WaveType::Noise)
        {
            return nextNoise();
        }
        if (wave == WaveType::PinkNoise)
        {
            return nextPinkNoise();
        }
        const float sample = currentSample(wave, phaseOffset);
        advance(freqHz, sampleRate);
        return sample;
    }

    IRAM_ATTR float currentSample(WaveType wave, float phaseOffset = 0.0f) const
    {
        float p = phase_ + phaseOffset;
        // floorf() measures ~16 cycles (almost like a division, see the FpuBench
        // diagnosis in chat) - phase_ itself has always kept advance() in
        // [0,1), phaseOffset is always 0.0f for Oscillator (no caller sets it
        // there), and usually small for Lfo too (phase-degrees/360).
        // Guard like in sineLookup() above: floorf() only in the real wrap
        // case (Lfo phaseOffset pushes over 1.0/below 0.0), no longer on
        // EVERY sample.
        if (p < 0.0f || p >= 1.0f)
            p -= floorf(p); // wrap to [0, 1) - phaseOffset may be negative or >1

        float sample;
        switch (wave)
        {
        case WaveType::Sine:
            sample = dsp::sineLookup(p);
            break;
        case WaveType::Square:
            sample = p < 0.5f ? 1.0f : -1.0f;
            break;
        case WaveType::Triangle:
        {
            // Must mirror main.js's lfoWaveValue() triangle formula exactly
            // (4*|phase - Math.floor(phase+0.5)| - 1, phase=0 -> minimum -1,
            // peak +1 at phase=0.5) - the old formula 4*|p-0.5|-1 was phase-
            // shifted by half a period (phase=0 -> +1 instead of -1), so e.g.
            // at the Gain/VCA a rising CV (per main.js/Voltmeter) actually
            // meant falling loudness and vice versa.
            const float nearest = p < 0.5f ? 0.0f : 1.0f;
            sample = 4.0f * fabsf(p - nearest) - 1.0f;
            break;
        }
        case WaveType::Sawtooth:
            sample = 2.0f * p - 1.0f;
            break;
        case WaveType::Noise:
        case WaveType::PinkNoise:
        default:
            sample = 0.0f;
            break;
        }
        return sample;
    }

    // Same as process(), but for a block where the caller has already
    // resolved pitch/tune to one constant Hz for the whole block (see
    // OscillatorNode.h's audioPortConstantForBlock fast path) and hoisted
    // `hz / sampleRate` to a single division outside the sample loop
    // (documentation/native-synth-optimization-plan-2026-09-09.md, P1 item
    // 2: "phaseIncrement = hz / sampleRate einmal vorberechnen"). Advances by
    // that precomputed increment directly - one add instead of advance()'s
    // multiply-then-add per sample.
    IRAM_ATTR float processWithIncrement(WaveType wave, float phaseIncrement, float phaseOffset = 0.0f)
    {
        if (wave == WaveType::Noise)
        {
            return nextNoise();
        }
        if (wave == WaveType::PinkNoise)
        {
            return nextPinkNoise();
        }
        const float sample = currentSample(wave, phaseOffset);
        phase_ += phaseIncrement;
        if (phase_ >= 1.0f)
        {
            phase_ -= floorf(phase_);
        }
        else if (phase_ < 0.0f)
        {
            phase_ -= floorf(phase_);
        }
        return sample;
    }

    // Compile-time-specialized twin of processWithIncrement() above, for
    // Oscillator's fast per-block-constant-pitch path only (never called
    // with a nonzero phaseOffset - that's an Lfo-only concept, see
    // currentSample()'s comment - so this drops the parameter entirely).
    // `wave` is already selected once per block by the caller (see
    // OscillatorNode.h's render()); W being a template parameter means the
    // `switch (wave)` inside currentSample() collapses to a single
    // `if constexpr` branch here, decided once at the call site (one
    // instantiation per waveform) rather than re-evaluated as a live branch
    // on every sample (plan section 3, P1 item 3: "Keine Wellenformauswahl
    // innerhalb der Sample-Schleife"). One shared definition, not six
    // hand-duplicated copies - the compiler generates the six
    // specializations from this single template.
    template <WaveType W>
    IRAM_ATTR float processWithIncrementT(float phaseIncrement)
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
            float p = phase_;
            if (p < 0.0f || p >= 1.0f)
                p -= floorf(p);
            float sample;
            if constexpr (W == WaveType::Sine)
            {
                sample = dsp::sineLookup(p);
            }
            else if constexpr (W == WaveType::Square)
            {
                sample = p < 0.5f ? 1.0f : -1.0f;
            }
            else if constexpr (W == WaveType::Triangle)
            {
                const float nearest = p < 0.5f ? 0.0f : 1.0f;
                sample = 4.0f * fabsf(p - nearest) - 1.0f;
            }
            else // Sawtooth
            {
                sample = 2.0f * p - 1.0f;
            }
            phase_ += phaseIncrement;
            if (phase_ >= 1.0f)
            {
                phase_ -= floorf(phase_);
            }
            else if (phase_ < 0.0f)
            {
                phase_ -= floorf(phase_);
            }
            return sample;
        }
    }

    // sampleRate is in practice ALWAYS the same value (kSampleRate, fixed
    // 48000 - see all callers in NodeInstance.cpp) - cache the reciprocal
    // instead of dividing anew each time. The comparison is always false
    // after the first call (well-predictable branch), the public signature
    // stays unchanged.
    IRAM_ATTR void advance(float freqHz, float sampleRate, size_t samples = 1)
    {
        if (sampleRate != lastSampleRate_)
        {
            lastSampleRate_ = sampleRate;
            invSampleRate_ = 1.0f / sampleRate;
        }
        phase_ += freqHz * static_cast<float>(samples) * invSampleRate_;
        if (phase_ >= 1.0f)
        {
            phase_ -= floorf(phase_);
        }
        else if (phase_ < 0.0f)
        {
            phase_ -= floorf(phase_); // keep well-defined for pathological negative freq
        }
    }

    void resetPhase() { phase_ = 0.0f; }

private:
    // Reciprocal instead of division - 4294967295.0f (UINT32_MAX) is a
    // compile-time literal, but without -ffast-math the compiler does not fold
    // it into a multiplication by itself (see CvConventions.h's
    // kInvMaxVolts comment for the same reason).
    static constexpr float kInvUint32Max = 1.0f / 4294967295.0f;

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

    float phase_ = 0.0f;
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

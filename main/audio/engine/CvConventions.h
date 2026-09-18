#pragma once

#include <cmath>

#include "DspPrimitives.h"

// Eurorack-style Control-Voltage convention: all control-rate ports (gate/
// velocity/knob output/pitch-CV/...) follow 0-10V instead of individual
// ad-hoc scales (0-1, 0-127, 0/1, ...). Every DEDICATED CV input/output of an
// audio-rate node also follows 0-10V: Lfo's "value" output (center 5V unless
// switched to bipolar, see LfoState::bipolar), Adsr's "out" output
// (envelope*velocity*10V) and Oscillator's "amplitude" input (mapped back
// to 0..1 via vcaGain()), so that e.g. an Adsr->Oscillator.amplitude cable
// works exactly like any other CV cable.
//
// The audio signal path itself (oscillator/filter/Hammond/sample-player
// waveform values) follows its OWN voltage convention, kAudioPeakVolts
// (+/-5V nominal peak) - deliberately half of kMaxVolts, so that audio
// (+/-5V) and CV (0-10V) share the same numeric neighborhood and can be
// freely mixed/patched together (any Mixer/Adder input accepts either kind
// today, see GraphBuilder.h's "audio and CV ports are treated equally").
// Audio-rate generators bake this in as a final multiply by kAudioPeakVolts
// (e.g. OscillatorNode.h, HammondNode.h's kDrawbarNorm); the hardware output
// stage (OutputNodes.h) divides back by kAudioPeakVolts before its
// SoftLimiter, so downstream code (SynthEngine.h's PCM conversion) is
// unaffected. Must match the JS reference in static/js/audio-engine.js
// (CV_MAX_VOLTS/AUDIO_PEAK_VOLTS/GATE_ON_VOLTS/GATE_OFF_VOLTS/
// GATE_THRESHOLD_VOLTS/noteToVoltage/velocityToVoltage) 1:1.
namespace cv
{
constexpr float kMaxVolts = 10.0f;
// Nominal peak amplitude of the AUDIO signal domain (oscillator/filter/
// Hammond/sample-player waveform values), as opposed to kMaxVolts (the CV
// domain). See the file-level comment above.
constexpr float kAudioPeakVolts = 5.0f;
constexpr float kInvAudioPeakVolts = 1.0f / kAudioPeakVolts; // same reciprocal-multiply reasoning as kInvMaxVolts below
// Reciprocal instead of division - without -ffast-math/-freciprocal-math
// (this project builds with plain -O2) the compiler does not fold
// "x / kMaxVolts" into a multiplication by itself, even though kMaxVolts is
// a compile-time literal. cvToRange/cvToRangeLogFast/cvToEnvelopeSeconds/
// vcaGain all use this as their first step - vcaGain() alone e.g. runs twice
// per oscillator sample (amp+gainMul, see NodeInstance.cpp), even when
// pitch/tune are not audio-rate modulated at all.
constexpr float kInvMaxVolts = 1.0f / kMaxVolts;
constexpr float kGateOnVolts = 10.0f;
constexpr float kGateOffVolts = 0.0f;
constexpr float kGateThresholdVolts = 5.0f; // replaces every previous 0.5f/0.5 gate threshold

// 1V/octave pitch-CV: MIDI note 60 (C4) sits at 5V (scale center), so the
// full 0-127 note range lies as symmetrically as possible in the 0-10V window.
constexpr float kPitchCvAnchorNote = 60.0f;
constexpr float kPitchCvVoltsPerOctave = 1.0f;

inline float noteToVoltage(float note)
{
    float v = (note - kPitchCvAnchorNote) / 12.0f * kPitchCvVoltsPerOctave + (kMaxVolts / 2.0f);
    if (v < 0.0f)
        v = 0.0f;
    if (v > kMaxVolts)
        v = kMaxVolts;
    return v;
}

inline float velocityToVoltage(float velocity127) { return (velocity127 / 127.0f) * kMaxVolts; }

// Inverse of noteToVoltage: turns a 1V/oct pitch-CV value back into Hz, using
// the same anchor (note 60/C4 at 5V) so a pitchCv fed straight from MidiIn
// plays back the exact MIDI note it represents. Used by Oscillator's pitchCv
// input (see NodeInstance.cpp's Oscillator case) - LUT-based (dsp::pow2Lookup)
// rather than powf(), since a genuine DSP edge on pitchCv/tune now re-evaluates
// this every sample per voice (audio-rate FM/vibrato), not just once per block.
inline float hzFromPitchCv(float volts)
{
    const float note = (volts - kMaxVolts / 2.0f) / kPitchCvVoltsPerOctave * 12.0f + kPitchCvAnchorNote;
    constexpr float kInvSemitonesPerOctave = 1.0f / 12.0f;
    return 440.0f * dsp::pow2Lookup((note - 69.0f) * kInvSemitonesPerOctave);
}

// Maps a 0-10V CV value onto [minVal, maxVal] - linear, or true logarithmic
// (equal ratio per volt, the natural curve for a frequency/Q-style range;
// requires minVal > 0, floored defensively) when `log` is set. Used by any
// widget-owned CV input with its own range/curve settings dialog (e.g.
// Filter cutoff/resonance) - kept separate from noteToVoltage/
// velocityToVoltage above, which are fixed system-wide conventions, not
// per-widget-configurable.
inline float cvToRange(float volts, float minVal, float maxVal, bool log)
{
    float v = volts * kInvMaxVolts;
    if (v < 0.0f)
        v = 0.0f;
    if (v > 1.0f)
        v = 1.0f;
    if (log)
    {
        const float lo = minVal <= 0.0f ? 0.0001f : minVal;
        return lo * powf(maxVal / lo, v);
    }
    return minVal + v * (maxVal - minVal);
}

// LUT-accelerated log-curve variant of cvToRange() for per-sample hot paths
// (e.g. filter cutoff/resonance under a genuine DSP edge, see NodeInstance.cpp)
// - powf(maxVal/lo, v) replaced by dsp::pow2Lookup(v*log2Ratio)
// (a^v = 2^(v*log2(a))), same technique as hzFromPitchCv above. The caller
// caches log2Ratio = log2f(maxVal/lo) once (only when the min/max settings
// change, see e.g. FilterState::refreshLog2Ratios) instead of recomputing it
// here every time - log2f() itself is just as expensive as powf(), the whole
// point lies in the caching, not in this function alone.
inline float cvToRangeLogFast(float volts, float minVal, float log2Ratio)
{
    float v = volts * kInvMaxVolts;
    if (v < 0.0f)
        v = 0.0f;
    if (v > 1.0f)
        v = 1.0f;
    const float lo = minVal <= 0.0f ? 0.0001f : minVal;
    return lo * dsp::pow2Lookup(v * log2Ratio);
}

// VCA gain curve for the Gain/VCA widget: 0-10V CV -> gain factor 0..1
// (0V = silent, 10V = unity gain). Linear (A = CV/10) fits modulation-depth
// CVs (e.g. an LFO controlling another modulation depth). Exponential
// (A = (10^v - 1) / 9, v = CV/10) fits envelopes (ADSR), because the human
// ear perceives loudness logarithmically - a linear envelope sounds even on
// an exponential VCA, but like a sudden "on/off" on a linear VCA. Must match
// the JS reference in static/js/audio-engine.js (vcaAmountFromCv) 1:1.
// Maps a 0-10V CV value onto an ADSR-style time (0..maxSeconds) - linear, or
// the same "3 decades" logarithmic taper main.js's cvToSecondsLog() uses
// (10V = maxSeconds, 0V = maxSeconds/1000, not true 0, avoiding a log(0)
// singularity while still feeling like "fully closed" at the low end). Used
// only when an ADSR's att/dec/rel CV input has a real audio-rate edge (see
// AdsrState/NodeInstance.cpp) - kept separate from cvToRange() above because
// the log taper formula differs (fixed 3-decade span vs. equal-ratio-per-
// volt) and the range always starts at 0, never an arbitrary minVal.
inline float cvToEnvelopeSeconds(float volts, float maxSeconds, bool log)
{
    float v = volts * kInvMaxVolts;
    if (v < 0.0f)
        v = 0.0f;
    if (v > 1.0f)
        v = 1.0f;
    if (log)
        return maxSeconds * powf(10.0f, 3.0f * (v - 1.0f));
    return v * maxSeconds;
}

inline float vcaGain(float volts, bool exponential)
{
    float v = volts * kInvMaxVolts;
    if (v < 0.0f)
        v = 0.0f;
    if (v > 1.0f)
        v = 1.0f;
    if (exponential)
        // LUT+lerp instead of powf() - this runs per sample on Gain's
        // audio-rate "amount" path (e.g. an ADSR driving a VCA), see
        // dsp::vcaExpLookup (DspPrimitives.h).
        return dsp::vcaExpLookup(v);
    return v;
}

// Both filter widgets expose "filterType" and "slope" as discrete 0-10V
// selectors, split into equal steps across the list. Mirrors main.js's
// filterTypeFromCv/filterSlopeFromCv, which must stay in the same order.
constexpr int kFilterKindCount = 8; // see FilterKind in NodeTypes.h
constexpr int kFilterMaxPoles = 6;  // 6 dB per pole, 6..36 dB

inline int discreteCvIndex(float volts, int steps)
{
    int index = static_cast<int>(volts / (kMaxVolts / static_cast<float>(steps)));
    if (index < 0)
        index = 0;
    if (index > steps - 1)
        index = steps - 1;
    return index;
}

inline FilterKind filterKindFromCv(float volts)
{
    return static_cast<FilterKind>(discreteCvIndex(volts, kFilterKindCount));
}

inline uint8_t slopeDbFromCv(float volts)
{
    return static_cast<uint8_t>((discreteCvIndex(volts, kFilterMaxPoles) + 1) * 6);
}
} // namespace cv

#pragma once

#include <cstddef>
#include <cstdint>

// Fixed at compile time, never changed at runtime - see project plan.
#define SYNTH_VOICE_COUNT 16

namespace synth
{
constexpr uint32_t kSampleRate = 44100;
constexpr size_t kBlockSize = 128;
constexpr size_t kMaxWidgets = 64;
constexpr size_t kMaxConnections = 256;
constexpr size_t kMaxAudioInPorts = 8; // widest is mixer/multiplier/adder in1..in8
constexpr size_t kValueListMax = 16;   // Knob/Button "value list" mode entry cap
constexpr size_t kMaxInstances = kMaxWidgets * SYNTH_VOICE_COUNT;
constexpr size_t kMaxPresetBytes = 256 * 1024;                 // matches WebServer's preset size cap

// Entries per full sine cycle in the shared sine LUT (see DspPrimitives'
// dsp::initSineLut/sineLookup) - large enough that linear interpolation
// between neighboring entries keeps quantization error inaudible.
constexpr size_t kSineLutSize = 16384;

// Entries spanning v=0..1 in the shared VCA exponential-curve LUT (see
// DspPrimitives' dsp::initVcaExpLut/vcaExpLookup) - the curve (10^v-1)/9 is
// smooth and monotonic (unlike a full sine cycle), so far fewer entries than
// kSineLutSize already keep interpolation error well below audible.
constexpr size_t kVcaExpLutSize = 1024;

// Entries spanning the fractional exponent f=0..1 of 2^f in the shared
// power-of-two LUT (see DspPrimitives' dsp::initPow2Lut/pow2Lookup) - smooth
// and monotonic like the VCA curve above, so the same entry count suffices;
// the integer part of the exponent is applied separately via ldexpf.
constexpr size_t kPow2LutSize = 1024;
} // namespace synth

#pragma once

#include <cstddef>
#include <cstdint>

#include "sdkconfig.h"

// Compile-time graph expansion ceiling. GraphBuilder may select fewer voices
// at runtime when the preset requests less or internal RAM cannot fit them.
#define SYNTH_VOICE_COUNT CONFIG_AUDIO_SYNTH_MAX_VOICES

namespace synth
{
constexpr uint32_t kSampleRate = 44100;
// Kconfig-configurable (Application Features > Audio Subsystem >
// AUDIO_SYNTH_BLOCK_SIZE) - see that option's help for the latency/overhead
// tradeoff.
constexpr size_t kBlockSize = CONFIG_AUDIO_SYNTH_BLOCK_SIZE;
constexpr size_t kMaxWidgets = 64;
constexpr size_t kMaxConnections = 256;
constexpr size_t kMaxRuntimeEdges = kMaxConnections * SYNTH_VOICE_COUNT + 2;
// Multiplexer has 16 audio inputs plus one control input (select) = 17 slots.
// Keep one bit per slot in NodeInstance's connection masks.
constexpr size_t kMaxAudioInPorts = 17;
constexpr size_t kValueListMax = 16;   // Knob/Button "value list" mode entry cap
constexpr size_t kMaxInstances = kMaxWidgets * SYNTH_VOICE_COUNT;
constexpr size_t kMaxPresetBytes = 256 * 1024;                 // matches WebServer's preset size cap

// Bit-per-audio-input-port mask type for NodeInstance's
// controlConnectedPortMask/audioConnectedPortMask/controlRampPortMask.
// Named rather than a bare uint32_t at each use site so a future widening
// past kMaxAudioInPorts is one edit here, caught by the static_assert.
using PortMask = uint32_t;
static_assert(kMaxAudioInPorts <= sizeof(PortMask) * 8,
              "PortMask needs at least one bit per audio-input port");

// The one correct way to build a port bit. A hardcoded
// static_cast<uint8_t>(1u << p) silently produces 0 for p >= 8 - this is
// what a runtime/variable port index must always go through instead.
inline constexpr PortMask portBit(int portIdx)
{
    return static_cast<PortMask>(1u << portIdx);
}

// Entries per full sine cycle in the shared sine LUT. Linear interpolation
// keeps maximum error below 3e-7 while using 48 KiB less internal RAM than
// the former 16384-entry table.
constexpr size_t kSineLutSize = 4096;

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

#pragma once

#include <cstdint>

// Preset-wide control stride ("Quality" = 1 ... "Performance" = 16): how
// often, in samples, nodes recompute expensive parameter-derived values
// (filter coefficients, envelope-follower coefficients, Hammond/oscillator
// pitch) while a parameter is modulated at audio rate by a cable. The audio
// itself is still processed every sample; slider/knob values are block-rate
// anyway and unaffected. Saved per preset as top-level "controlStride",
// applied when a preset's graph goes live and live via "@controlStride".
namespace synth
{

constexpr uint8_t kDefaultControlStride = 4;

// Only powers of two: the render loops use them as chunk sizes that divide
// the block evenly.
inline uint8_t clampControlStride(int value)
{
    if (value <= 1) return 1;
    if (value <= 2) return 2;
    if (value <= 4) return 4;
    if (value <= 8) return 8;
    return 16;
}

inline uint8_t &controlStride()
{
    static uint8_t stride = kDefaultControlStride;
    return stride;
}

} // namespace synth

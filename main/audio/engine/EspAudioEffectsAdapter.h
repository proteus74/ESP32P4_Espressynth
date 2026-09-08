#pragma once

#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "esp_heap_caps.h"

namespace espae
{

// Fixed block scratch used at the float graph / ESP_AUDIO_EFFECTS boundary.
// Channel buffers are allocated once while building the graph; render calls
// never allocate memory.
struct S32PlanarScratch
{
    int32_t *channels[2] = {};
    uint8_t channelCount = 0;
    size_t capacityFrames = 0;
};

inline void freeScratch(S32PlanarScratch &scratch)
{
    for (auto &channel : scratch.channels)
    {
        heap_caps_free(channel);
        channel = nullptr;
    }
    scratch.channelCount = 0;
    scratch.capacityFrames = 0;
}

inline bool allocateScratch(S32PlanarScratch &scratch, uint8_t channelCount, size_t capacityFrames)
{
    freeScratch(scratch);
    if (channelCount == 0 || channelCount > 2 || capacityFrames == 0)
    {
        return false;
    }

    // PSRAM-preferred, internal fallback: allocated once while building the graph, then read/
    // written at most once per block by the float<->S32 conversion helpers below and the
    // ESP_AUDIO_EFFECTS call itself - block-rate, not per-sample, so PSRAM latency is not a
    // render-budget risk here (same reasoning as GraphBuilder.h's allocateNodeState/
    // allocateBlockBuffer elsewhere in this engine).
    const size_t bytes = capacityFrames * sizeof(int32_t);
    for (uint8_t channel = 0; channel < channelCount; ++channel)
    {
        scratch.channels[channel] = static_cast<int32_t *>(
            heap_caps_aligned_calloc(16, 1, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (scratch.channels[channel] == nullptr)
        {
            scratch.channels[channel] = static_cast<int32_t *>(
                heap_caps_aligned_calloc(16, 1, bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        }
        if (scratch.channels[channel] == nullptr)
        {
            freeScratch(scratch);
            return false;
        }
    }
    scratch.channelCount = channelCount;
    scratch.capacityFrames = capacityFrames;
    return true;
}

inline void floatToS32(const float *input, int32_t *output, size_t frames)
{
    if (input == nullptr || output == nullptr)
    {
        return;
    }
    constexpr float kScale = 2147483647.0f;
    for (size_t i = 0; i < frames; ++i)
    {
        float sample = input[i];
        if (sample >= 1.0f)
        {
            output[i] = INT32_MAX;
        }
        else if (sample <= -1.0f)
        {
            output[i] = INT32_MIN;
        }
        else
        {
            output[i] = static_cast<int32_t>(std::lrintf(sample * kScale));
        }
    }
}

inline void s32ToFloat(const int32_t *input, float *output, size_t frames)
{
    if (input == nullptr || output == nullptr)
    {
        return;
    }
    constexpr float kScale = 1.0f / 2147483648.0f;
    for (size_t i = 0; i < frames; ++i)
    {
        output[i] = static_cast<float>(input[i]) * kScale;
    }
}

} // namespace espae

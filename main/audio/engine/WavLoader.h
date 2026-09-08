#pragma once

#include <cstdio>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "SamplePaths.h"
#include "SynthConfig.h"

// Minimal Microsoft-WAV (RIFF/WAVE) loader for the Wave Player node. Only
// PCM16 stereo is accepted; any sample rate is fine - a file that doesn't
// already match the engine's own sample rate (see SynthConfig.h) is
// resampled once here (linear interpolation) at load time, so the render
// path itself stays free of runtime samplerate conversion.
namespace wavloader
{
// Per-sample PSRAM budget - a widget's decoded buffer must fit under this,
// matching the precedent of synth::kMaxPresetBytes as a safety cap.
constexpr size_t kMaxSampleBytes = 4 * 1024 * 1024;

namespace detail
{
inline constexpr char kTag[] = "WavLoader";

// Process-wide cache of decoded WAV buffers, keyed by filename - see the
// header comment on wavloader::load()/sweepCache(). Sized for the
// theoretical worst case (every widget slot a distinct WavePlayer sample);
// GraphBuilder::build() keeps it from ever actually filling by sweeping
// stale entries before loading anything. `inline` (not an anonymous
// namespace) so every translation unit that includes this header shares the
// same single cache instance instead of each getting its own.
struct CachedWav
{
    char filename[samplepaths::kMaxNameLen + 1] = {0};
    const int16_t *data = nullptr;
    uint32_t frameCount = 0;
    uint8_t channels = 0;
    bool inUse = false;
};
inline CachedWav *gCache = nullptr;

inline CachedWav *cacheStorage()
{
    if (gCache != nullptr)
    {
        return gCache;
    }
    void *storage = heap_caps_malloc(synth::kMaxWidgets * sizeof(CachedWav), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (storage == nullptr)
    {
        storage = heap_caps_malloc(synth::kMaxWidgets * sizeof(CachedWav), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    gCache = static_cast<CachedWav *>(storage);
    if (gCache != nullptr)
    {
        for (size_t i = 0; i < synth::kMaxWidgets; ++i)
        {
            new (&gCache[i]) CachedWav();
        }
    }
    return gCache;
}

inline CachedWav *findCached(const char *filename)
{
    CachedWav *cache = cacheStorage();
    if (cache == nullptr)
    {
        return nullptr;
    }
    for (size_t i = 0; i < synth::kMaxWidgets; ++i)
    {
        CachedWav &entry = cache[i];
        if (entry.inUse && strcmp(entry.filename, filename) == 0) return &entry;
    }
    return nullptr;
}
} // namespace detail

// Loads "<samplepaths::kDir>/<filename>", validates its fmt chunk (PCM,
// stereo, 16-bit), decodes its data chunk, and resamples it to
// synth::kSampleRate if its own rate differs. Reuses a process-wide cache
// keyed by filename when the same file
// was already loaded, skipping the flash read/decode entirely (mirrors
// SoundFontPlayer.cpp's SF2 file cache - GraphBuilder::build() runs this
// synchronously on the audio render task for every widget on every preset
// save, so re-decoding unrelated-widget rebuilds from scratch was a real
// stall). The returned buffer is owned by the cache, NOT the caller - never
// free it directly; call sweepCache() to release entries. On any failure
// (missing file, bad RIFF/WAVE header, unsupported fmt, oversized/empty data
// chunk, truncated read, allocation failure) returns false, frees any
// partial allocation itself, logs the reason, and never crashes.
inline bool load(const char *filename, const int16_t *&outData, uint32_t &outFrameCount, uint8_t &outChannels)
{
    using namespace detail;
    outData = nullptr;
    outFrameCount = 0;
    outChannels = 0;

    if (!samplepaths::isSafeName(filename))
    {
        ESP_LOGE(kTag, "unsafe sample filename");
        return false;
    }

    if (CachedWav *cached = findCached(filename))
    {
        outData = cached->data;
        outFrameCount = cached->frameCount;
        outChannels = cached->channels;
        ESP_LOGI(kTag, "%s: reused from cache", filename);
        return true;
    }

    char path[96];
    samplepaths::pathForName(filename, path, sizeof(path));

    FILE *f = fopen(path, "rb");
    if (f == nullptr)
    {
        ESP_LOGE(kTag, "sample not found: %s", path);
        return false;
    }

    uint8_t riff[12];
    if (fread(riff, 1, 12, f) != 12 || memcmp(riff, "RIFF", 4) != 0 || memcmp(riff + 8, "WAVE", 4) != 0)
    {
        ESP_LOGE(kTag, "%s: not RIFF/WAVE", path);
        fclose(f);
        return false;
    }

    bool haveFmt = false;
    uint16_t audioFormat = 0, numChannels = 0, bitsPerSample = 0;
    uint32_t sampleRate = 0;
    int16_t *pcm = nullptr;
    uint32_t dataBytes = 0;

    // Chunks are scanned by ID/size rather than assumed at fixed offsets -
    // a WAV file may carry LIST/fact/etc. chunks before "data".
    for (int guard = 0; guard < 32 && pcm == nullptr; ++guard)
    {
        uint8_t hdr[8];
        if (fread(hdr, 1, 8, f) != 8)
        {
            break; // truncated/EOF before a data chunk appeared
        }
        uint32_t size;
        memcpy(&size, hdr + 4, 4); // little-endian, matches this target

        if (memcmp(hdr, "fmt ", 4) == 0 && size >= 16)
        {
            uint8_t fmt[16];
            if (fread(fmt, 1, 16, f) != 16)
            {
                break;
            }
            memcpy(&audioFormat, fmt + 0, 2);
            memcpy(&numChannels, fmt + 2, 2);
            memcpy(&sampleRate, fmt + 4, 4);
            memcpy(&bitsPerSample, fmt + 14, 2);
            haveFmt = true;
            fseek(f, static_cast<long>(size) - 16, SEEK_CUR); // skip any fmt extension bytes
        }
        else if (memcmp(hdr, "data", 4) == 0)
        {
            if (!haveFmt || audioFormat != 1 || numChannels != 2 || bitsPerSample != 16 ||
                sampleRate == 0 || sampleRate > 192000 || size == 0 || size > wavloader::kMaxSampleBytes)
            {
                ESP_LOGE(kTag, "%s: unsupported/oversized WAV (fmt=%u ch=%u bits=%u rate=%u size=%u)", path,
                          static_cast<unsigned>(audioFormat), static_cast<unsigned>(numChannels),
                          static_cast<unsigned>(bitsPerSample), static_cast<unsigned>(sampleRate),
                          static_cast<unsigned>(size));
                fclose(f);
                return false;
            }
            pcm = static_cast<int16_t *>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (pcm == nullptr)
            {
                ESP_LOGE(kTag, "%s: PSRAM alloc failed (%u B)", path, static_cast<unsigned>(size));
                fclose(f);
                return false;
            }
            if (fread(pcm, 1, size, f) != size)
            {
                ESP_LOGE(kTag, "%s: short read", path);
                heap_caps_free(pcm);
                fclose(f);
                return false;
            }
            dataBytes = size;
        }
        else
        {
            fseek(f, static_cast<long>(size + (size & 1)), SEEK_CUR); // RIFF word-pads odd chunk sizes
        }
    }
    fclose(f);

    if (pcm == nullptr)
    {
        ESP_LOGE(kTag, "%s: no valid data chunk found", path);
        return false;
    }

    // Not every sample library exports at this project's fixed 44.1kHz
    // render rate (see SynthConfig.h) - rather than reject every file that
    // doesn't already match exactly, resample once here (linear
    // interpolation is more than sufficient for one-shot playback samples)
    // so WavePlayer can use any standard-rate WAV, not just 44.1kHz ones.
    if (sampleRate != synth::kSampleRate)
    {
        const uint32_t origFrames = dataBytes / (2 * sizeof(int16_t));
        const uint32_t newFrames = static_cast<uint32_t>(
            (static_cast<uint64_t>(origFrames) * synth::kSampleRate) / sampleRate);
        const size_t newBytes = static_cast<size_t>(newFrames) * 2 * sizeof(int16_t);
        if (newFrames == 0 || newBytes > wavloader::kMaxSampleBytes)
        {
            ESP_LOGE(kTag, "%s: resampled size out of range (%lu -> %lu frames)", path,
                      static_cast<unsigned long>(origFrames), static_cast<unsigned long>(newFrames));
            heap_caps_free(pcm);
            return false;
        }
        int16_t *resampled = static_cast<int16_t *>(heap_caps_malloc(newBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (resampled == nullptr)
        {
            ESP_LOGE(kTag, "%s: PSRAM alloc failed for resample (%u B)", path, static_cast<unsigned>(newBytes));
            heap_caps_free(pcm);
            return false;
        }
        const float ratio = static_cast<float>(sampleRate) / static_cast<float>(synth::kSampleRate);
        for (uint32_t i = 0; i < newFrames; ++i)
        {
            const float srcPos = static_cast<float>(i) * ratio;
            uint32_t i0 = static_cast<uint32_t>(srcPos);
            if (i0 >= origFrames)
            {
                i0 = origFrames - 1;
            }
            const uint32_t i1 = (i0 + 1 < origFrames) ? i0 + 1 : i0;
            const float frac = srcPos - static_cast<float>(i0);
            for (int ch = 0; ch < 2; ++ch)
            {
                const float a = static_cast<float>(pcm[i0 * 2 + ch]);
                const float b = static_cast<float>(pcm[i1 * 2 + ch]);
                resampled[i * 2 + ch] = static_cast<int16_t>(a + (b - a) * frac);
            }
        }
        heap_caps_free(pcm);
        pcm = resampled;
        dataBytes = static_cast<uint32_t>(newBytes);
        ESP_LOGI(kTag, "%s: resampled %lu Hz -> %lu Hz (%lu -> %lu frames)", path,
                  static_cast<unsigned long>(sampleRate), static_cast<unsigned long>(synth::kSampleRate),
                  static_cast<unsigned long>(origFrames), static_cast<unsigned long>(newFrames));
    }

    CachedWav *cache = cacheStorage();
    if (cache == nullptr)
    {
        ESP_LOGE(kTag, "%s: WAV-Cache-Metadaten konnten nicht allokiert werden", filename);
        heap_caps_free(pcm);
        return false;
    }
    CachedWav *slot = nullptr;
    for (size_t i = 0; i < synth::kMaxWidgets; ++i)
    {
        CachedWav &entry = cache[i];
        if (!entry.inUse)
        {
            slot = &entry;
            break;
        }
    }
    if (slot == nullptr)
    {
        // Should not happen (cache is sized for kMaxWidgets, and
        // GraphBuilder::build() sweeps stale entries before loading
        // anything) - fall back to an uncached buffer rather than leaking it
        // or aborting; it will simply be re-decoded on the next rebuild.
        ESP_LOGW(kTag, "%s: WAV-Cache voll, ungecached verwendet", filename);
        outData = pcm;
        outChannels = 2;
        outFrameCount = dataBytes / (2 * sizeof(int16_t));
        return true;
    }
    snprintf(slot->filename, sizeof(slot->filename), "%s", filename);
    slot->data = pcm;
    slot->frameCount = dataBytes / (2 * sizeof(int16_t));
    slot->channels = 2;
    slot->inUse = true;

    outData = slot->data;
    outChannels = slot->channels;
    outFrameCount = slot->frameCount;
    return true;
}

// Frees every cached buffer whose filename is not present in `keep` (an
// array of `keepCount` sampleFile strings) - GraphBuilder::build() calls
// this once per pass, before loading anything, with every WavePlayer
// widget's sampleFile in the preset currently being built. Unlike
// SoundFontPlayer.cpp's single-slot SF2 cache, a preset can reference many
// distinct WAV files at once (many WavePlayer widgets, different samples),
// so this is a small mark-and-sweep cache instead of a single entry.
inline void sweepCache(const char *const *keep, size_t keepCount)
{
    using namespace detail;
    CachedWav *cache = cacheStorage();
    if (cache == nullptr)
    {
        return;
    }
    for (size_t cacheIndex = 0; cacheIndex < synth::kMaxWidgets; ++cacheIndex)
    {
        CachedWav &entry = cache[cacheIndex];
        if (!entry.inUse)
        {
            continue;
        }
        bool stillNeeded = false;
        for (size_t i = 0; i < keepCount; ++i)
        {
            if (keep[i] != nullptr && strcmp(entry.filename, keep[i]) == 0)
            {
                stillNeeded = true;
                break;
            }
        }
        if (!stillNeeded)
        {
            heap_caps_free(const_cast<int16_t *>(entry.data));
            entry = CachedWav{};
        }
    }
}
} // namespace wavloader

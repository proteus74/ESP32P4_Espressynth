#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "tsf.h"

#include "NodeState.h"
#include "PresetParser.h"
#include "SynthConfig.h"
#include "features/sd_card/sd_card_manager.hpp"
#include "UsbMidiEvent.h"

namespace
{
constexpr char kSoundFontTag[] = "soundfont";
constexpr size_t kMaxSf2FileBytes = 14U * 1024U * 1024U;

bool endsWithSf2(const char *path)
{
    const size_t len = path != nullptr ? strlen(path) : 0;
    if (len < 4) return false;
    const char *ext = path + len - 4;
    return ext[0] == '.' && (ext[1] == 's' || ext[1] == 'S') &&
           (ext[2] == 'f' || ext[2] == 'F') && ext[3] == '2';
}

bool safeRelativePath(const char *path)
{
    if (path == nullptr || path[0] == '\0' || path[0] == '/' || path[0] == '\\' || !endsWithSf2(path)) return false;
    if (strstr(path, "..") != nullptr || strchr(path, '\\') != nullptr || strchr(path, ':') != nullptr) return false;
    return true;
}
} // namespace

namespace soundfont
{
struct LoadConfig
{
    const char *volume;
    const char *relativePath;
    int bank;
    int program;
    int maxVoices;
};

// (volume, relativePath) pair identifying one cached SF2 file - see sweepFileCache().
struct FileKey
{
    const char *volume;
    const char *relativePath;
};

namespace detail
{
// Process-wide, mark-and-sweep cache of decoded .sf2 files (samples +
// preset/region tables - the part tsf_load_filename() spends real time
// reading off flash/SD and parsing, up to multiple seconds for a multi-MB GM
// bank), keyed by (volume, relativePath) - mirrors WavLoader.h's CachedWav
// cache for the identical reason: Graph::taskLoop() calls GraphBuilder::
// build() -> load() synchronously on the audio render task for EVERY preset
// save, even ones that never touched a SoundFont Player widget, and a preset
// can have multiple SoundFontPlayer widgets that happen to reference the
// same file.
//
// Each entry's `master` holds one tsf_copy() reference kept alive purely by
// this cache; every load() call for a matching (volume, relativePath) hands
// out a further tsf_copy() of it (cheap - copies the small tsf struct and
// bumps a refcount, no sample data duplication - see tsf_copy()/tsf_close()
// in tsf.h) for the calling widget's own NodeInstance to own and later
// close() independently, so N SoundFontPlayer widgets pointed at the same
// file share exactly one decoded copy in PSRAM. Sized like WavLoader's cache
// (kMaxWidgets entries - the theoretical worst case of every widget slot a
// distinct SF2 file) and kept from ever actually filling the same way:
// GraphBuilder::build() calls sweepFileCache() before loading anything, with
// every SoundFontPlayer widget's (volume, path) still in the preset being
// built, closing (decrementing the refcount of) any entry no longer
// referenced by any widget. `inline` (not an anonymous namespace) so every
// translation unit that includes this header shares the same single cache.
struct SoundFontFileCache
{
    char volume[8] = {0};
    char path[200] = {0};
    tsf *master = nullptr;
    bool inUse = false;
};
inline SoundFontFileCache *gFileCache = nullptr;

inline SoundFontFileCache *cacheStorage()
{
    if (gFileCache != nullptr)
    {
        return gFileCache;
    }
    void *storage = heap_caps_malloc(synth::kMaxWidgets * sizeof(SoundFontFileCache), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (storage == nullptr)
    {
        storage = heap_caps_malloc(synth::kMaxWidgets * sizeof(SoundFontFileCache), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    gFileCache = static_cast<SoundFontFileCache *>(storage);
    if (gFileCache != nullptr)
    {
        for (size_t i = 0; i < synth::kMaxWidgets; ++i)
        {
            new (&gFileCache[i]) SoundFontFileCache();
        }
    }
    return gFileCache;
}

inline SoundFontFileCache *findCachedFile(const char *volume, const char *relativePath)
{
    SoundFontFileCache *cache = cacheStorage();
    if (cache == nullptr)
    {
        return nullptr;
    }
    for (size_t i = 0; i < synth::kMaxWidgets; ++i)
    {
        SoundFontFileCache &entry = cache[i];
        if (entry.inUse && strcmp(entry.volume, volume != nullptr ? volume : "") == 0 &&
            strcmp(entry.path, relativePath != nullptr ? relativePath : "") == 0)
        {
            return &entry;
        }
    }
    return nullptr;
}
} // namespace detail

// Resolves flash|sd plus a safe relative .sf2 path into a VFS path.
inline bool resolvePath(const char *volume, const char *relativePath, char *out, size_t outSize)
{
    if (out == nullptr || outSize == 0 || !safeRelativePath(relativePath)) return false;
    const char *root = nullptr;
    if (volume != nullptr && strcmp(volume, "flash") == 0)
    {
        root = "/littlefs";
    }
    else if (volume != nullptr && strcmp(volume, "sd") == 0 && Feature::SDCard::is_mounted())
    {
        root = Feature::SDCard::MOUNT_POINT;
    }
    else
    {
        return false;
    }
    const int written = snprintf(out, outSize, "%s/%s", root, relativePath);
    return written > 0 && static_cast<size_t>(written) < outSize;
}

// Loads and owns an independent SoundFont bank instance for `config`,
// reusing a process-wide mark-and-sweep file cache when `volume`/
// `relativePath` match an already-loaded file - possibly from a different
// SoundFontPlayer widget - skipping the flash/SD read + SF2 parse entirely
// (see detail::SoundFontFileCache above; mirrors WavLoader.h's cache for the
// same reason). Returns nullptr on invalid path, missing/unmounted storage,
// malformed SF2, unavailable program, or OOM.
inline tsf *load(const LoadConfig &config, int &presetIndex)
{
    using namespace detail;
    presetIndex = -1;
    tsf *synth = nullptr;

    if (SoundFontFileCache *cached = findCachedFile(config.volume, config.relativePath))
    {
        // Same file another (or this) widget already has cached - skip the
        // flash/SD read and SF2 parse entirely, just hand out a fresh
        // independent handle sharing the cached decode.
        synth = tsf_copy(cached->master);
        if (synth == nullptr)
        {
            ESP_LOGE(kSoundFontTag, "SF2 copy from cache failed (OOM): %s:%s", config.volume, config.relativePath);
            return nullptr;
        }
        ESP_LOGI(kSoundFontTag, "SF2 reused from cache: %s:%s", config.volume, config.relativePath);
    }
    else
    {
        char path[256];
        if (!resolvePath(config.volume, config.relativePath, path, sizeof(path)))
        {
            ESP_LOGE(kSoundFontTag, "Invalid/unavailable SF2 path: %s:%s", config.volume, config.relativePath);
            return nullptr;
        }

        FILE *file = fopen(path, "rb");
        if (file == nullptr)
        {
            ESP_LOGE(kSoundFontTag, "SF2 not found: %s", path);
            return nullptr;
        }
        fseek(file, 0, SEEK_END);
        const long size = ftell(file);
        fclose(file);
        if (size <= 0 || static_cast<size_t>(size) > kMaxSf2FileBytes)
        {
            ESP_LOGE(kSoundFontTag, "SF2 size invalid/too large: %ld bytes (max %u)", size,
                     static_cast<unsigned>(kMaxSf2FileBytes));
            return nullptr;
        }

        tsf *freshLoad = tsf_load_filename(path);
        if (freshLoad == nullptr)
        {
            ESP_LOGE(kSoundFontTag, "SF2 could not be loaded: %s", path);
            return nullptr;
        }

        // Claim a free cache slot for this file so a later widget (or a
        // later rebuild) referencing the same (volume, relativePath) hits
        // the cache above instead of re-reading/re-parsing. If the cache is
        // genuinely full (every kMaxWidgets slot holds a distinct file -
        // pathological, since that's also the hard cap on total widgets),
        // freshLoad still works for this one widget, it just won't be
        // shared - drop the extra ref tsf_copy() below creates so it isn't
        // leaked.
        SoundFontFileCache *cache = cacheStorage();
        SoundFontFileCache *slot = nullptr;
        if (cache != nullptr)
        {
            for (size_t i = 0; i < synth::kMaxWidgets; ++i)
            {
                if (!cache[i].inUse)
                {
                    slot = &cache[i];
                    break;
                }
            }
        }
        if (slot != nullptr)
        {
            snprintf(slot->volume, sizeof(slot->volume), "%s", config.volume != nullptr ? config.volume : "");
            snprintf(slot->path, sizeof(slot->path), "%s", config.relativePath != nullptr ? config.relativePath : "");
            slot->master = freshLoad;
            slot->inUse = true;
        }
        else
        {
            ESP_LOGW(kSoundFontTag, "SF2 cache full, file will not be shared: %s:%s", config.volume, config.relativePath);
        }

        synth = tsf_copy(freshLoad);
        if (synth == nullptr)
        {
            ESP_LOGE(kSoundFontTag, "SF2 copy failed (OOM): %s", path);
            if (slot != nullptr)
            {
                tsf_close(freshLoad);
                *slot = SoundFontFileCache{};
            }
            else
            {
                tsf_close(freshLoad);
            }
            return nullptr;
        }
        if (slot == nullptr)
        {
            tsf_close(freshLoad); // no cache slot kept a reference - drop ours, synth alone still owns the data
        }
        ESP_LOGI(kSoundFontTag, "SF2 loaded: %s, %ld bytes", path, size);
    }

    presetIndex = tsf_get_presetindex(synth, config.bank, config.program);
    if (presetIndex < 0)
    {
        ESP_LOGE(kSoundFontTag, "Instrument not found: Bank %d, Program %d", config.bank, config.program);
        tsf_close(synth);
        return nullptr;
    }
    tsf_set_output(synth, TSF_STEREO_INTERLEAVED, synth::kSampleRate, 0.0f);
    if (!tsf_set_max_voices(synth, config.maxVoices))
    {
        ESP_LOGE(kSoundFontTag, "Voices could not be pre-allocated: %d", config.maxVoices);
        tsf_close(synth);
        return nullptr;
    }
    for (int channel = 0; channel < 16; ++channel)
    {
        tsf_channel_set_bank_preset(synth, channel, config.bank, config.program);
    }
    ESP_LOGI(kSoundFontTag, "SF2 configured: %s:%s, Bank %d Program %d (%s)", config.volume, config.relativePath,
             config.bank, config.program, tsf_get_presetname(synth, presetIndex));
    return synth;
}

// Creates an independent playback state sharing immutable bank/sample data.
inline tsf *copy(tsf *source, int maxVoices)
{
    if (source == nullptr) return nullptr;
    tsf *result = tsf_copy(source);
    if (result == nullptr) return nullptr;
    tsf_set_output(result, TSF_STEREO_INTERLEAVED, synth::kSampleRate, 0.0f);
    if (!tsf_set_max_voices(result, maxVoices))
    {
        tsf_close(result);
        return nullptr;
    }
    return result;
}

inline void close(tsf *synth)
{
    if (synth != nullptr) tsf_close(synth);
}

// Frees every cached SF2 bank whose (volume, relativePath) is not present in
// `keep` (`keepCount` entries) - GraphBuilder::build() calls this once per
// pass, before loading anything, with every SoundFontPlayer widget's
// volume/path in the preset currently being built (mirrors WavLoader.h's
// sweepCache()). A preset can have multiple SoundFontPlayer widgets, possibly
// sharing the same file (each load() call above then just hands out a cheap
// tsf_copy() of the one cached decode), so this is a small mark-and-sweep
// cache instead of a single entry.
inline void sweepFileCache(const FileKey *keep, size_t keepCount)
{
    using namespace detail;
    SoundFontFileCache *cache = cacheStorage();
    if (cache == nullptr)
    {
        return;
    }
    for (size_t cacheIndex = 0; cacheIndex < synth::kMaxWidgets; ++cacheIndex)
    {
        SoundFontFileCache &entry = cache[cacheIndex];
        if (!entry.inUse)
        {
            continue;
        }
        bool stillNeeded = false;
        for (size_t i = 0; i < keepCount; ++i)
        {
            if (keep[i].volume != nullptr && keep[i].relativePath != nullptr &&
                strcmp(entry.volume, keep[i].volume) == 0 && strcmp(entry.path, keep[i].relativePath) == 0)
            {
                stillNeeded = true;
                break;
            }
        }
        if (!stillNeeded)
        {
            tsf_close(entry.master);
            entry = SoundFontFileCache{};
        }
    }
}

inline void noteOn(SoundFontPlayerState &state, int note, float velocity)
{
    if (state.synth == nullptr || state.presetIndex < 0) return;
    if (velocity < 0.0f) velocity = 0.0f;
    if (velocity > 1.0f) velocity = 1.0f;
    tsf_note_on(state.synth, state.presetIndex, note < 0 ? 0 : (note > 127 ? 127 : note), velocity);
}

inline void noteOff(SoundFontPlayerState &state, int note)
{
    if (state.synth == nullptr || state.presetIndex < 0) return;
    tsf_note_off(state.synth, state.presetIndex, note < 0 ? 0 : (note > 127 ? 127 : note));
}

inline void handleMidi(SoundFontPlayerState &state, const UsbMidiEvent &event)
{
    if (state.synth == nullptr) return;
    const int channel = event.status & 0x0F;
    switch (event.codeIndex)
    {
    case 0x8:
        tsf_channel_note_off(state.synth, channel, event.data1);
        break;
    case 0x9:
        if (event.data2 == 0) tsf_channel_note_off(state.synth, channel, event.data1);
        else tsf_channel_note_on(state.synth, channel, event.data1, event.data2 / 127.0f);
        break;
    case 0xB:
        // Mod wheel is deliberately a separate permission from generic CC:
        // specifically MIDI channel 1 (zero-based channel 0), CC 1.
        if ((channel == 0 && event.data1 == 1) ? state.enableModWheel : state.enableCc)
            tsf_channel_midi_control(state.synth, channel, event.data1, event.data2);
        break;
    case 0xC:
        tsf_channel_set_presetnumber(state.synth, channel, event.data1, channel == 9);
        break;
    case 0xE:
        if (state.enablePitchWheel)
            tsf_channel_set_pitchwheel(state.synth, channel, (event.data2 << 7) | event.data1);
        break;
    default:
        break;
    }
}

inline bool setBankProgram(SoundFontPlayerState &state, int bank, int program)
{
    if (state.synth == nullptr) return false;
    if (bank < 0) bank = 0;
    if (bank > 128) bank = 128;
    if (program < 0) program = 0;
    if (program > 127) program = 127;
    const int presetIndex = tsf_get_presetindex(state.synth, bank, program);
    if (presetIndex < 0) return false;
    for (int channel = 0; channel < 16; ++channel)
        tsf_channel_set_bank_preset(state.synth, channel, bank, program);
    state.bank = bank;
    state.program = program;
    state.presetIndex = presetIndex;
    return true;
}

inline int activeVoiceCount(const SoundFontPlayerState &state)
{
    return state.synth != nullptr ? tsf_active_voice_count(state.synth) : 0;
}

inline void render(SoundFontPlayerState &state, float *left, float *right, size_t frames)
{
    if (state.synth == nullptr || frames > synth::kBlockSize)
    {
        memset(left, 0, frames * sizeof(float));
        memset(right, 0, frames * sizeof(float));
        return;
    }
    // Render task is single-threaded/core-pinned, so one shared block scratch
    // avoids adding 1 KiB to every NodeInstance in the engine's flat state.
    // Local static in an inline function - one shared instance across every
    // translation unit that includes this header (same ODR guarantee that
    // already applied when this lived in the single SoundFontPlayer.cpp TU).
    static float interleaved[synth::kBlockSize * 2];
    tsf_render_float(state.synth, interleaved, static_cast<int>(frames), 0);
    for (size_t i = 0; i < frames; ++i)
    {
        left[i] = interleaved[i * 2];
        right[i] = interleaved[i * 2 + 1];
    }
}

inline void parsePreset(cJSON *p, WidgetParams &out)
{
    snprintf(out.soundFontVolume, sizeof(out.soundFontVolume), "%s", presetparser::getString(p, "volume", "flash"));
    snprintf(out.soundFontPath, sizeof(out.soundFontPath), "%s", presetparser::getString(p, "path", ""));
    out.soundFontBank = presetparser::getInt(p, "bank", 0);
    out.soundFontProgram = presetparser::getInt(p, "program", 0);
    out.soundFontMaxVoices = presetparser::getInt(p, "maxVoices", 32);
    out.soundFontEnableCc = presetparser::getBool(p, "enableCc", false);
    out.soundFontEnableModWheel = presetparser::getBool(p, "enableModWheel", false);
    out.soundFontEnablePitchWheel = presetparser::getBool(p, "enablePitchWheel", false);
    // "gain" shares the field below with Oscillator/AudioOut/... (see WidgetParams).
    out.gain = presetparser::getFloat(p, "gain", 10.0f);
    if (out.soundFontMaxVoices < 4) out.soundFontMaxVoices = 4;
    if (out.soundFontMaxVoices > 128) out.soundFontMaxVoices = 128;
}
} // namespace soundfont

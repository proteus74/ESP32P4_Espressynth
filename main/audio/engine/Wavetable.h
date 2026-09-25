#pragma once

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>

#include "cJSON.h"
#include "esp_heap_caps.h"

// Wavetable bank for the LFO's Wavetable mode (see nodes/LfoNode.h). One
// bank = up to kMaxTables single-cycle waveforms of kTableSize samples each
// (-1..+1). The LFO morphs linearly between neighbouring tables (morph
// 0-10V spans table 0 .. table count-1) and interpolates linearly between
// neighbouring samples inside a table.
//
// Ownership: parsed once per preset into WidgetParams::lfoWavetable (see
// LfoNode.h's parsePreset), then shared read-only by every voice instance of
// that LFO widget. Reference counted, because the PresetDoc and the Graph
// built from it have independent lifetimes (GraphBuilder may even build
// several graphs from one doc while it searches a fitting voice count).
// The ~64KB payload lives in PSRAM - it is read at LFO rate, one or two
// cache lines per sample at most.
//
// Preset JSON: "wavetables" is an array of strings, one per table, each the
// base64 encoding of kTableSize little-endian int16 samples (full scale =
// +/-32767). The web editor (data/static/js/wavetable-core.js) writes the
// exact same format. A plain number array (-1..1) is accepted too.
namespace wavetable
{

constexpr size_t kTableSize = 1024;
constexpr size_t kMaxTables = 16;

struct Bank
{
    std::atomic<int> refs{1};
    uint8_t count = 0;
    float data[kMaxTables][kTableSize] = {};
};

inline Bank *create()
{
    void *storage = heap_caps_malloc(sizeof(Bank), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (storage == nullptr)
    {
        storage = heap_caps_malloc(sizeof(Bank), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    return storage != nullptr ? new (storage) Bank() : nullptr;
}

inline Bank *acquire(Bank *bank)
{
    if (bank != nullptr)
    {
        bank->refs.fetch_add(1, std::memory_order_relaxed);
    }
    return bank;
}

inline void release(Bank *bank)
{
    if (bank != nullptr && bank->refs.fetch_sub(1, std::memory_order_acq_rel) == 1)
    {
        bank->~Bank();
        heap_caps_free(bank);
    }
}

namespace detail
{
inline int base64Value(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

// Decodes base64 int16-LE samples straight into `out` (kTableSize floats).
// Returns false on a malformed/short string; missing trailing samples are
// never guessed - the table is rejected instead.
inline bool decodeTable(const char *text, float *out)
{
    uint8_t pending[2] = {};
    size_t pendingCount = 0;
    size_t sample = 0;
    uint32_t accum = 0;
    int bits = 0;
    for (const char *c = text; *c != '\0' && sample < kTableSize; ++c)
    {
        if (*c == '=')
            break;
        const int v = base64Value(*c);
        if (v < 0)
            continue; // whitespace/line breaks
        accum = (accum << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8)
        {
            bits -= 8;
            pending[pendingCount++] = static_cast<uint8_t>((accum >> bits) & 0xFFu);
            if (pendingCount == 2)
            {
                const int16_t raw = static_cast<int16_t>(static_cast<uint16_t>(pending[0]) |
                                                         (static_cast<uint16_t>(pending[1]) << 8));
                float value = static_cast<float>(raw) * (1.0f / 32767.0f);
                out[sample++] = value < -1.0f ? -1.0f : value;
                pendingCount = 0;
            }
        }
    }
    return sample == kTableSize;
}

inline bool readNumberTable(cJSON *arr, float *out)
{
    size_t sample = 0;
    cJSON *item = nullptr;
    cJSON_ArrayForEach(item, arr)
    {
        if (sample >= kTableSize)
            break;
        float v = cJSON_IsNumber(item) ? static_cast<float>(item->valuedouble) : 0.0f;
        out[sample++] = v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v);
    }
    return sample == kTableSize;
}
} // namespace detail

// Default bank used when a preset turns Wavetable mode on without any
// stored tables: sine -> triangle -> sawtooth -> square, the same four
// presets the web editor offers (wavetable-core.js's fillPreset()).
inline void fillDefault(Bank &bank)
{
    constexpr float kTwoPi = 6.28318530718f;
    bank.count = 4;
    for (size_t i = 0; i < kTableSize; ++i)
    {
        const float phase = static_cast<float>(i) / static_cast<float>(kTableSize);
        bank.data[0][i] = sinf(kTwoPi * phase);
        bank.data[1][i] = 1.0f - 4.0f * fabsf(phase - 0.25f - floorf(phase + 0.25f));
        bank.data[2][i] = 2.0f * phase - 1.0f;
        bank.data[3][i] = phase < 0.5f ? 1.0f : -1.0f;
    }
}

// Builds a bank from the preset's "wavetables" array. Returns nullptr when
// the key is absent/empty and `wanted` is false (LFO not in wavetable mode -
// no need to spend 64KB), a default bank when wanted but absent/unreadable.
inline Bank *parse(cJSON *arr, bool wanted)
{
    const bool present = cJSON_IsArray(arr) && cJSON_GetArraySize(arr) > 0;
    if (!present && !wanted)
        return nullptr;
    Bank *bank = create();
    if (bank == nullptr)
        return nullptr;
    if (present)
    {
        cJSON *item = nullptr;
        cJSON_ArrayForEach(item, arr)
        {
            if (bank->count >= kMaxTables)
                break;
            float *dst = bank->data[bank->count];
            const bool ok = cJSON_IsString(item) ? detail::decodeTable(item->valuestring, dst)
                          : cJSON_IsArray(item)  ? detail::readNumberTable(item, dst)
                                                 : false;
            if (ok)
                ++bank->count;
        }
    }
    if (bank->count == 0)
        fillDefault(*bank);
    return bank;
}

// phase: 32-bit fixed-point cycle position (PhaseOsc domain, 2^32 = one
// cycle). morph: 0..1 across the whole bank.
inline float sample(const Bank &bank, uint32_t phase, float morph)
{
    const uint32_t i0 = phase >> 22; // top 10 bits = 0..1023
    const uint32_t i1 = (i0 + 1u) & (kTableSize - 1u);
    const float frac = static_cast<float>(phase & 0x3FFFFFu) * (1.0f / 4194304.0f);
    if (bank.count <= 1)
    {
        const float *t = bank.data[0];
        return t[i0] + (t[i1] - t[i0]) * frac;
    }
    morph = morph < 0.0f ? 0.0f : (morph > 1.0f ? 1.0f : morph);
    const float pos = morph * static_cast<float>(bank.count - 1);
    uint32_t a = static_cast<uint32_t>(pos);
    if (a >= static_cast<uint32_t>(bank.count - 1))
        a = bank.count - 2u;
    const float mix = pos - static_cast<float>(a);
    const float *ta = bank.data[a];
    const float *tb = bank.data[a + 1];
    const float va = ta[i0] + (ta[i1] - ta[i0]) * frac;
    const float vb = tb[i0] + (tb[i1] - tb[i0]) * frac;
    return va + (vb - va) * mix;
}

} // namespace wavetable

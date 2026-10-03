#pragma once

#include <cstdio>
#include <cmath>
#include <cstdint>

#include "cJSON.h"

// Shared tempo for all Clock-type nodes. Presets restore their saved BPM;
// MIDI mapping/rounding remain device-wide. The separate file supplies boot
// defaults for legacy presets without a tempo.
namespace synth
{

struct MasterClockState
{
    float bpm = 120.0f;
    // BPM at a learned MIDI CC value of 127; CC 0 always maps to 0 BPM.
    float maxBpm = 240.0f;
    bool roundBpm = false;
    int midiCc = -1;      // -1 = unlearned
    int midiChannel = -1; // -1 = unlearned
    uint32_t midiFeedback = UINT32_MAX; // Runtime only; never persisted.

    int feedbackValue() const
    {
        if (midiCc < 0 || midiCc > 127 || midiChannel < 0 || midiChannel > 15 ||
            !std::isfinite(bpm) || !std::isfinite(maxBpm) || maxBpm <= 0.0f) return -1;
        const float tempo = roundBpm ? std::round(bpm) : bpm;
        return static_cast<int>(std::fmax(0.0f, std::fmin(127.0f, (tempo / maxBpm) * 127.0f)) + 0.5f);
    }
};

inline MasterClockState &masterClock()
{
    static MasterClockState state;
    return state;
}

constexpr const char *kMasterClockPath = "/littlefs/master_clock.json";

// Loads the persisted Master Clock settings, if any, into masterClock().
// Call once at boot, after LittleFS is mounted and before the render task
// starts (see synth_manager.hpp) - a missing/invalid file just keeps the
// struct's built-in defaults, same tolerance readPresetFile() has for a
// missing/invalid preset.
inline void loadMasterClockFromFs()
{
    FILE *f = fopen(kMasterClockPath, "rb");
    if (f == nullptr)
    {
        return;
    }
    char buf[256];
    const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (root == nullptr)
    {
        return;
    }
    auto &mc = masterClock();
    cJSON *bpm = cJSON_GetObjectItemCaseSensitive(root, "bpm");
    cJSON *maxBpm = cJSON_GetObjectItemCaseSensitive(root, "maxBpm");
    cJSON *round = cJSON_GetObjectItemCaseSensitive(root, "round");
    cJSON *cc = cJSON_GetObjectItemCaseSensitive(root, "cc");
    cJSON *channel = cJSON_GetObjectItemCaseSensitive(root, "channel");
    if (cJSON_IsNumber(bpm)) mc.bpm = static_cast<float>(bpm->valuedouble);
    if (cJSON_IsNumber(maxBpm)) mc.maxBpm = static_cast<float>(maxBpm->valuedouble);
    if (cJSON_IsBool(round)) mc.roundBpm = cJSON_IsTrue(round);
    if (cJSON_IsNumber(cc)) mc.midiCc = cc->valueint;
    if (cJSON_IsNumber(channel)) mc.midiChannel = channel->valueint;
    cJSON_Delete(root);
}

// Formats the current masterClock() state as the same "masterClock" WS
// message both call sites need: the new-client snapshot
// (synth_web_api.hpp's log_client_connected()) and the on-change broadcast
// (broadcast_status_tick()). Returns the formatted length, or <= 0 on
// truncation/error - same snprintf-return convention those call sites
// already use.
inline int formatMasterClockJson(char *buf, size_t bufSize)
{
    const auto &mc = masterClock();
    return snprintf(buf, bufSize, "{\"type\":\"masterClock\",\"bpm\":%.2f,\"maxBpm\":%.2f,\"round\":%s,\"cc\":%d,\"channel\":%d}",
                     mc.bpm, mc.maxBpm, mc.roundBpm ? "true" : "false", mc.midiCc, mc.midiChannel);
}

// Persists the current masterClock() state - called only from
// SynthEngine::applyOutputParam's "@masterClock" branch (the browser's
// Master Clock dialog Save/Learn actions), NEVER from the per-sample MIDI-CC
// live path (handleMasterClockMidi) - that would wear the flash for a value
// that isn't meaningful to restore across a reboot anyway (a real knob
// doesn't remember its last position either).
inline void saveMasterClockToFs()
{
    const auto &mc = masterClock();
    char json[192];
    const int n = snprintf(json, sizeof(json),
                            "{\"bpm\":%.2f,\"maxBpm\":%.2f,\"round\":%s,\"cc\":%d,\"channel\":%d}",
                            mc.bpm, mc.maxBpm, mc.roundBpm ? "true" : "false", mc.midiCc, mc.midiChannel);
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(json))
    {
        return;
    }
    FILE *f = fopen(kMasterClockPath, "wb");
    if (f == nullptr)
    {
        return;
    }
    fwrite(json, 1, static_cast<size_t>(n), f);
    fclose(f);
}

} // namespace synth

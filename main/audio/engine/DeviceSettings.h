#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "cJSON.h"

// Engine-global device identity, master volume + preset-navigation MIDI learn: a device
// name and two learned CC bindings ("previous preset" / "next preset") that
// react on the button-down edge only (see handleDeviceSettingsMidi() in
// synth_manager.hpp), not on the CC's live value - matching a physical
// panel button rather than a knob. Independent of any preset (same
// reasoning as MasterClock.h), and persisted to its own tiny file so a
// preset switch never touches it.
namespace synth
{

struct DeviceSettingsState
{
    char name[32] = "";
    int prevCc = -1;      // -1 = unlearned
    int prevChannel = -1; // -1 = unlearned
    int nextCc = -1;
    int nextChannel = -1;
    // Device-wide master volume (0..1), multiplied into every hardware output
    // sink after the per-preset output widgets' own gain (see SynthEngine's
    // render loop). Optionally driven by a learned CC (live value, not an edge).
    float volume = 1.0f;
    int volumeCc = -1;
    int volumeChannel = -1;
    // Runtime-only edge-detection state (NOT persisted) - true while the
    // learned CC is currently held above the press threshold, so a preset
    // step fires once on the rising edge instead of once per MIDI message.
    bool prevHeld = false;
    bool nextHeld = false;
    int64_t prevLastOnUs = 0; // esp_timer time of the last "on" message, see handleDeviceSettingsMidi()
    int64_t nextLastOnUs = 0;
};

inline DeviceSettingsState &deviceSettings()
{
    static DeviceSettingsState state;
    return state;
}

inline float clampDeviceVolume(float v)
{
    return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
}

constexpr const char *kDeviceSettingsPath = "/littlefs/DeviceSettings.json";

// Loads the persisted device settings, if any, into deviceSettings(). Call
// once at boot, after LittleFS is mounted - a missing/invalid file just
// keeps the struct's built-in defaults.
inline void loadDeviceSettingsFromFs()
{
    FILE *f = fopen(kDeviceSettingsPath, "rb");
    if (f == nullptr)
    {
        return;
    }
    char buf[384];
    const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (root == nullptr)
    {
        return;
    }
    auto &ds = deviceSettings();
    cJSON *name = cJSON_GetObjectItemCaseSensitive(root, "name");
    cJSON *prevCc = cJSON_GetObjectItemCaseSensitive(root, "prevCc");
    cJSON *prevChannel = cJSON_GetObjectItemCaseSensitive(root, "prevChannel");
    cJSON *nextCc = cJSON_GetObjectItemCaseSensitive(root, "nextCc");
    cJSON *nextChannel = cJSON_GetObjectItemCaseSensitive(root, "nextChannel");
    cJSON *volume = cJSON_GetObjectItemCaseSensitive(root, "volume");
    cJSON *volumeCc = cJSON_GetObjectItemCaseSensitive(root, "volumeCc");
    cJSON *volumeChannel = cJSON_GetObjectItemCaseSensitive(root, "volumeChannel");
    if (cJSON_IsString(name)) snprintf(ds.name, sizeof(ds.name), "%s", name->valuestring);
    if (cJSON_IsNumber(prevCc)) ds.prevCc = prevCc->valueint;
    if (cJSON_IsNumber(prevChannel)) ds.prevChannel = prevChannel->valueint;
    if (cJSON_IsNumber(nextCc)) ds.nextCc = nextCc->valueint;
    if (cJSON_IsNumber(nextChannel)) ds.nextChannel = nextChannel->valueint;
    if (cJSON_IsNumber(volume)) ds.volume = clampDeviceVolume(static_cast<float>(volume->valuedouble));
    if (cJSON_IsNumber(volumeCc)) ds.volumeCc = volumeCc->valueint;
    if (cJSON_IsNumber(volumeChannel)) ds.volumeChannel = volumeChannel->valueint;
    cJSON_Delete(root);
}

// Formats the current deviceSettings() state as JSON for GET
// /api/settings/device (synth_web_api.hpp).
inline int formatDeviceSettingsJson(char *buf, size_t bufSize)
{
    const auto &ds = deviceSettings();
    return snprintf(buf, bufSize,
                     "{\"name\":\"%s\",\"prevCc\":%d,\"prevChannel\":%d,\"nextCc\":%d,\"nextChannel\":%d,"
                     "\"volume\":%.3f,\"volumeCc\":%d,\"volumeChannel\":%d}",
                     ds.name, ds.prevCc, ds.prevChannel, ds.nextCc, ds.nextChannel,
                     static_cast<double>(ds.volume), ds.volumeCc, ds.volumeChannel);
}

// Persists the current deviceSettings() state - called only from the
// browser's Settings dialog Save/Learn actions (infrequent), never from the
// per-message MIDI path.
inline void saveDeviceSettingsToFs()
{
    char json[384];
    const int n = formatDeviceSettingsJson(json, sizeof(json));
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(json))
    {
        return;
    }
    FILE *f = fopen(kDeviceSettingsPath, "wb");
    if (f == nullptr)
    {
        return;
    }
    fwrite(json, 1, static_cast<size_t>(n), f);
    fclose(f);
}

} // namespace synth

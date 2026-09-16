#pragma once

#include <cstdio>
#include <cstring>

#include "cJSON.h"

// Engine-global device identity + preset-navigation MIDI learn: a device
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
    // Runtime-only edge-detection state (NOT persisted) - true while the
    // learned CC is currently held above the press threshold, so a preset
    // step fires once on the rising edge instead of once per MIDI message.
    bool prevHeld = false;
    bool nextHeld = false;
};

inline DeviceSettingsState &deviceSettings()
{
    static DeviceSettingsState state;
    return state;
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
    char buf[256];
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
    if (cJSON_IsString(name)) snprintf(ds.name, sizeof(ds.name), "%s", name->valuestring);
    if (cJSON_IsNumber(prevCc)) ds.prevCc = prevCc->valueint;
    if (cJSON_IsNumber(prevChannel)) ds.prevChannel = prevChannel->valueint;
    if (cJSON_IsNumber(nextCc)) ds.nextCc = nextCc->valueint;
    if (cJSON_IsNumber(nextChannel)) ds.nextChannel = nextChannel->valueint;
    cJSON_Delete(root);
}

// Formats the current deviceSettings() state as JSON for GET
// /api/settings/device (synth_web_api.hpp).
inline int formatDeviceSettingsJson(char *buf, size_t bufSize)
{
    const auto &ds = deviceSettings();
    return snprintf(buf, bufSize,
                     "{\"name\":\"%s\",\"prevCc\":%d,\"prevChannel\":%d,\"nextCc\":%d,\"nextChannel\":%d}",
                     ds.name, ds.prevCc, ds.prevChannel, ds.nextCc, ds.nextChannel);
}

// Persists the current deviceSettings() state - called only from the
// browser's Settings dialog Save/Learn actions (infrequent), never from the
// per-message MIDI path.
inline void saveDeviceSettingsToFs()
{
    char json[320];
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

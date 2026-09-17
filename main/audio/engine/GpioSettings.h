#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "cJSON.h"

// Engine-global GPIO pin table: which free GPIOs the user has dedicated to
// Digital In/Digital Out widgets, and how each is configured (direction,
// pull for inputs). Independent of any preset (same reasoning as
// DeviceSettings.h/MasterClock.h) - a Digital In/Out widget only stores
// *which* configured pin it uses (see nodes/DigitalInNode.h/DigitalOutNode.h),
// not the pin's direction/pull, so this table must survive preset switches.
namespace Feature::IO
{

constexpr size_t kMaxGpioEntries = 16;

enum class GpioDirection : uint8_t { Input = 0, Output = 1 };
enum class GpioPull : uint8_t { None = 0, Up = 1, Down = 2 }; // meaningful for Input only

struct GpioEntry
{
    int8_t pin = -1; // -1 = empty slot
    GpioDirection direction = GpioDirection::Input;
    GpioPull pull = GpioPull::None;
    // Flips the logical level DigitalInNode/DigitalOutNode read/write vs. the raw electrical
    // level - e.g. a pull-up input reads electrical LOW while the switch is pressed, so
    // "inverted" lets that surface as logical HIGH. Applied in software only (in the node's
    // render(), not gpio_config()) - orthogonal to pull, which only affects the resting state.
    bool invert = false;
};

struct GpioSettingsState
{
    GpioEntry entries[kMaxGpioEntries];
    uint8_t count = 0;
};

inline GpioSettingsState &gpioSettings()
{
    static GpioSettingsState state;
    return state;
}

constexpr const char *kGpioSettingsPath = "/littlefs/GpioSettings.json";

// Returns the direction/pull for `pin` if it's configured, or nullptr.
inline const GpioEntry *findGpioEntry(int8_t pin)
{
    const auto &gs = gpioSettings();
    for (uint8_t i = 0; i < gs.count; ++i)
    {
        if (gs.entries[i].pin == pin) return &gs.entries[i];
    }
    return nullptr;
}

// Formats the current gpioSettings() state as JSON for GET /api/settings/gpio
// (synth_web_api.hpp).
inline int formatGpioSettingsJson(char *buf, size_t bufSize)
{
    const auto &gs = gpioSettings();
    int len = snprintf(buf, bufSize, "{\"entries\":[");
    for (uint8_t i = 0; i < gs.count; ++i)
    {
        const GpioEntry &e = gs.entries[i];
        len += snprintf(buf + len, bufSize - static_cast<size_t>(len),
                         "%s{\"pin\":%d,\"direction\":\"%s\",\"pull\":\"%s\",\"invert\":%s}",
                         i == 0 ? "" : ",", e.pin,
                         e.direction == GpioDirection::Output ? "out" : "in",
                         e.pull == GpioPull::Up ? "up" : e.pull == GpioPull::Down ? "down" : "none",
                         e.invert ? "true" : "false");
    }
    len += snprintf(buf + len, bufSize - static_cast<size_t>(len), "]}");
    return len;
}

// Loads the persisted GPIO pin table, if any, into gpioSettings(). Call once
// at boot, after LittleFS is mounted - a missing/invalid file just keeps the
// struct empty. Does NOT apply the pins via gpio_config() - call
// Feature::IO::apply_gpio_settings() (gpio_manager.hpp) right after.
inline void loadGpioSettingsFromFs()
{
    FILE *f = fopen(kGpioSettingsPath, "rb");
    if (f == nullptr) return;
    char buf[1536]; // up to kMaxGpioEntries entries
    const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (root == nullptr) return;
    cJSON *entries = cJSON_GetObjectItemCaseSensitive(root, "entries");
    if (cJSON_IsArray(entries))
    {
        auto &gs = gpioSettings();
        gs.count = 0;
        cJSON *entry;
        cJSON_ArrayForEach(entry, entries)
        {
            if (gs.count >= kMaxGpioEntries) break;
            cJSON *pin = cJSON_GetObjectItemCaseSensitive(entry, "pin");
            cJSON *dir = cJSON_GetObjectItemCaseSensitive(entry, "direction");
            cJSON *pull = cJSON_GetObjectItemCaseSensitive(entry, "pull");
            cJSON *invert = cJSON_GetObjectItemCaseSensitive(entry, "invert");
            if (!cJSON_IsNumber(pin)) continue;
            GpioEntry &e = gs.entries[gs.count++];
            e.pin = static_cast<int8_t>(pin->valueint);
            e.direction = (cJSON_IsString(dir) && strcmp(dir->valuestring, "out") == 0)
                              ? GpioDirection::Output : GpioDirection::Input;
            e.pull = (cJSON_IsString(pull) && strcmp(pull->valuestring, "up") == 0) ? GpioPull::Up
                    : (cJSON_IsString(pull) && strcmp(pull->valuestring, "down") == 0) ? GpioPull::Down
                    : GpioPull::None;
            e.invert = cJSON_IsTrue(invert);
        }
    }
    cJSON_Delete(root);
}

// Persists the current gpioSettings() state - called only from the browser's
// Settings dialog Save action (infrequent), never from the render task.
inline void saveGpioSettingsToFs()
{
    char json[1536]; // up to kMaxGpioEntries entries
    const int n = formatGpioSettingsJson(json, sizeof(json));
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(json)) return;
    FILE *f = fopen(kGpioSettingsPath, "wb");
    if (f == nullptr) return;
    fwrite(json, 1, static_cast<size_t>(n), f);
    fclose(f);
}

} // namespace Feature::IO

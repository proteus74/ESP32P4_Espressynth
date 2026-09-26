#pragma once

#include <cstddef>
#include <cstdlib>

#include "../NodeInstance.h"
#include "../PresetParser.h"
#include "features/io/led_strip_manager.hpp"

// "RGB LED" - maps the voltage patched into "in" to a color and writes it to one LED of the
// WS2812 string (Feature::LedStrip). The mapping is a stop list sorted by volts: below the
// first / above the last stop that stop's color holds; between stop i and i+1 the color
// fades linearly if stop i has "fade" set, otherwise it stays hard at stop i's color. All
// stops fading = gradient, none = hard thresholds, mixed = free.
//
// render() only stores into Feature::LedStrip's pixel buffer (an atomic store); the actual
// RMT transfer happens on a Core-0 task at 20 Hz - see features/io/led_strip_manager.hpp.
namespace nodes::rgbled
{

inline uint32_t mapColor(const RgbLedState &s, float volts)
{
    if (s.stopCount == 0) return 0;
    if (volts <= s.stopVolts[0]) return s.stopColor[0];
    const uint8_t last = s.stopCount - 1;
    if (volts >= s.stopVolts[last]) return s.stopColor[last];
    uint8_t i = 0;
    while (i + 1 < last && volts >= s.stopVolts[i + 1]) ++i;
    const uint32_t a = s.stopColor[i];
    if (!s.stopFade[i]) return a;
    const uint32_t b = s.stopColor[i + 1];
    const float span = s.stopVolts[i + 1] - s.stopVolts[i];
    const float t = span > 0.0f ? (volts - s.stopVolts[i]) / span : 0.0f;
    uint32_t out = 0;
    for (int shift = 16; shift >= 0; shift -= 8)
    {
        const float ca = static_cast<float>((a >> shift) & 0xFF);
        const float cb = static_cast<float>((b >> shift) & 0xFF);
        out |= static_cast<uint32_t>(ca + (cb - ca) * t + 0.5f) << shift;
    }
    return out;
}

inline void render(NodeInstance &inst, size_t frames)
{
    if (frames == 0) return;
    auto &s = inst.rgbLed;
    // Last sample of the block is plenty - the strip only refreshes at 20 Hz anyway.
    Feature::LedStrip::set_pixel(s.ledIndex, mapColor(s, noderender::portSample(inst, 0, frames - 1)));
}

inline void build(NodeInstance &inst, const WidgetParams &p)
{
    auto &s = inst.rgbLed;
    s.ledIndex = p.rgbLedIndex;
    s.stopCount = p.rgbStopCount;
    for (uint8_t i = 0; i < p.rgbStopCount; ++i)
    {
        s.stopVolts[i] = p.rgbStopVolts[i];
        s.stopColor[i] = p.rgbStopColor[i];
        s.stopFade[i] = p.rgbStopFade[i];
    }
}

// params: { "ledIndex": 0, "stops": [ { "v": 0, "c": "#ff0000", "fade": true }, ... ] }
inline void parsePreset(cJSON *p, WidgetParams &out)
{
    out.rgbLedIndex = static_cast<int16_t>(presetparser::getInt(p, "ledIndex", 0));
    out.rgbStopCount = 0;
    cJSON *stops = p != nullptr ? cJSON_GetObjectItemCaseSensitive(p, "stops") : nullptr;
    cJSON *stop = nullptr;
    cJSON_ArrayForEach(stop, stops)
    {
        if (out.rgbStopCount >= kRgbLedMaxStops) break;
        const char *hex = presetparser::getString(stop, "c", "#000000");
        const float volts = presetparser::getFloat(stop, "v", 0.0f);
        const uint32_t color = static_cast<uint32_t>(strtoul(hex[0] == '#' ? hex + 1 : hex, nullptr, 16)) & 0xFFFFFFu;
        const bool fade = presetparser::getBool(stop, "fade", false);
        // Insertion sort by volts - at most 8 entries.
        uint8_t i = out.rgbStopCount++;
        while (i > 0 && out.rgbStopVolts[i - 1] > volts)
        {
            out.rgbStopVolts[i] = out.rgbStopVolts[i - 1];
            out.rgbStopColor[i] = out.rgbStopColor[i - 1];
            out.rgbStopFade[i] = out.rgbStopFade[i - 1];
            --i;
        }
        out.rgbStopVolts[i] = volts;
        out.rgbStopColor[i] = color;
        out.rgbStopFade[i] = fade;
    }
}

} // namespace nodes::rgbled

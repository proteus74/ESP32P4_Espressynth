#pragma once

#include <cstdint>

// Copied by the audio task; consumed independently of console logging by LVGL.
struct KnobActivity
{
    int64_t updatedUs = 0;
    uint32_t presetSequence = 0;
    uint8_t controlIndex = 0;
    uint8_t raw = 0;
    bool isButton = false; // Button node (MidiButton card) instead of a Knob
    bool isEnvelope = false;
    struct Point { uint16_t x = 0, y = 0; } envelope[6]; // normalized chart coordinates, 0..1000
    float sourceValue = 0.0f; // Button: its current output value
    char sourceId[24] = {};
    char name[48] = {};
    char targetId[24] = {};
    char device[48] = {};
    char port[16] = {};
    uint8_t targetType = 0;
    float targetValue = 0.0f; // current value at the target port (usually volts)
    bool targetValueRaw = false; // targetValue is an integer (raw CC or layer number), not volts
    int64_t durationUs() const { return isEnvelope ? 2000000 : 1000000; }
};

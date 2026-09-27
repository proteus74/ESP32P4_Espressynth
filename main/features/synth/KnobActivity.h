#pragma once

#include <cstdint>

// Copied by the audio task; consumed independently of console logging by LVGL.
struct KnobActivity
{
    int64_t updatedUs = 0;
    uint32_t presetSequence = 0;
    uint8_t controlIndex = 0;
    uint8_t raw = 0;
    char sourceId[24] = {};
    char name[48] = {};
    char targetId[24] = {};
    char device[48] = {};
    char port[16] = {};
};

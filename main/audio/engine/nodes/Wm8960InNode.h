#pragma once

#include <cstddef>

#include "../CvConventions.h"
#include "../SynthConfig.h"
#include "../NodeInstance.h"
#include "../PresetParser.h"

// "WM8960-In" (External Audio widget) - the WM8960 codec's ADC (HAT microphones / line input)
// as a stereo audio source ("left", "right"). SynthEngine fills wm8960InputBlock{L,R}() once
// per block, before renderBlock(), from the reader it was handed via setWm8960Input() (see
// features/audio/wm8960_manager.hpp); render() only scales those blocks to the engine's audio
// convention (+/- kAudioPeakVolts).
namespace nodes::wm8960in
{

// Latest ADC blocks, +/-1.0 full scale. Written by SynthEngine on the render task only.
inline float inputBlockLStorage[synth::kBlockSize] = {};
inline float inputBlockRStorage[synth::kBlockSize] = {};
inline float *inputBlockL() { return inputBlockLStorage; }
inline float *inputBlockR() { return inputBlockRStorage; }

// Sends the widget's input-stage settings to the codec. Installed by the firmware glue
// (synth_manager.hpp); must return quickly and never block - build() runs on the preset loader.
using ApplyHook = void (*)(float pgaDb, int boostDb, float adcDb, bool micBias);
inline ApplyHook applyHook = nullptr;

inline void render(NodeInstance &inst, size_t frames)
{
    const float *l = inputBlockL();
    const float *r = inputBlockR();
    for (size_t i = 0; i < frames; ++i)
    {
        inst.outL[i] = l[i] * cv::kAudioPeakVolts;
        inst.outR[i] = r[i] * cv::kAudioPeakVolts;
    }
}

inline void build(NodeInstance &, const WidgetParams &p)
{
    if (applyHook != nullptr)
        applyHook(p.wm8960InPgaDb, p.wm8960InBoostDb, p.wm8960InAdcDb, p.wm8960InMicBias);
}

inline void parsePreset(cJSON *p, WidgetParams &out)
{
    out.wm8960InPgaDb = presetparser::getFloat(p, "pgaDb", 24.0f);
    out.wm8960InBoostDb = static_cast<int16_t>(presetparser::getInt(p, "boostDb", 0));
    out.wm8960InAdcDb = presetparser::getFloat(p, "adcDb", 0.0f);
    out.wm8960InMicBias = presetparser::getBool(p, "micBias", true);
}

} // namespace nodes::wm8960in

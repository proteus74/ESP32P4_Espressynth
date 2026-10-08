#pragma once

#include <cstddef>

#include "../CvConventions.h"
#include "../SynthConfig.h"
#include "../NodeInstance.h"

// "Mic-In" (External Audio widget) - the board's onboard microphone as a mono audio source
// ("out"). SynthEngine fills micInputBlock() once per block, before renderBlock(), from the
// mic driver it was handed via setMicInput() (see features/audio/pdm_mic_manager.hpp); render()
// only scales that block to the engine's audio convention (+/- kAudioPeakVolts).
namespace nodes::micin
{

// Latest microphone block, +/-1.0 full scale. Written by SynthEngine on the render task only.
inline float micInputBlockStorage[synth::kBlockSize] = {};
inline float *micInputBlock() { return micInputBlockStorage; }

inline void render(NodeInstance &inst, size_t frames)
{
    const float *in = micInputBlock();
    for (size_t i = 0; i < frames; ++i)
    {
        inst.outL[i] = in[i] * cv::kAudioPeakVolts;
    }
}

} // namespace nodes::micin

#pragma once

#include <cmath>
#include <cstddef>
#include <cstring>

#include "../CvConventions.h"
#include "../NodeInstance.h"
#include "../PresetParser.h"

namespace nodes::wavefolder
{

inline float clampCv(float value)
{
    return value < 0.0f ? 0.0f : (value > cv::kMaxVolts ? cv::kMaxVolts : value);
}

// Triangle reflection around +/- kAudioPeakVolts. Unlike clipping this keeps
// folding for arbitrarily high Drive values without producing a DC jump.
inline float fold(float sample)
{
    const float limit = cv::kAudioPeakVolts;
    const float period = limit * 4.0f;
    float phase = fmodf(sample + limit, period);
    if (phase < 0.0f)
        phase += period;
    return limit - fabsf(phase - limit * 2.0f);
}

inline bool portFed(const NodeInstance &inst, uint8_t port)
{
    return noderender::controlPortConnected(inst, port) ||
           (inst.audioConnectedPortMask & synth::portBit(port)) != 0;
}

inline void render(NodeInstance &inst, size_t frames)
{
    using namespace noderender;
    auto &s = inst.waveFolder;
    const bool driveFed = portFed(inst, 1);
    const bool symmetryFed = portFed(inst, 2);
    const bool gainFed = portFed(inst, 3);
    const bool mixFed = portFed(inst, 4);

    for (size_t i = 0; i < frames; ++i)
    {
        const float driveCv = clampCv(driveFed ? portSample(inst, 1, i) : s.drive);
        const float symmetryCv = clampCv(symmetryFed ? portSample(inst, 2, i) : s.symmetry);
        const float gainCv = clampCv(gainFed ? portSample(inst, 3, i) : s.gain);
        const float mix = clampCv(mixFed ? portSample(inst, 4, i) : s.mix) / cv::kMaxVolts;
        const float drive = 1.0f + 7.0f * (driveCv / cv::kMaxVolts);
        const float bias = symmetryCv - cv::kMaxVolts * 0.5f;
        const float dry = portSample(inst, 0, i);
        const float wet = fold(dry * drive + bias);
        inst.outL[i] = (dry + (wet - dry) * mix) * cv::vcaGain(gainCv, false);
    }
}

inline void build(NodeInstance &inst, const WidgetParams &p)
{
    inst.waveFolder.drive = clampCv(p.waveFolderDrive);
    inst.waveFolder.symmetry = clampCv(p.waveFolderSymmetry);
    inst.waveFolder.gain = clampCv(p.waveFolderGain);
    inst.waveFolder.mix = clampCv(p.waveFolderMix);
}

inline bool setParam(NodeInstance &inst, const char *name, float value)
{
    if (name == nullptr)
        return false;
    if (strcmp(name, "drive") == 0) inst.waveFolder.drive = clampCv(value);
    else if (strcmp(name, "symmetry") == 0) inst.waveFolder.symmetry = clampCv(value);
    else if (strcmp(name, "gain") == 0) inst.waveFolder.gain = clampCv(value);
    else if (strcmp(name, "mix") == 0) inst.waveFolder.mix = clampCv(value);
    else return false;
    return true;
}

inline void parsePreset(cJSON *p, WidgetParams &out)
{
    out.waveFolderDrive = clampCv(presetparser::getFloat(p, "drive", 0.0f));
    out.waveFolderSymmetry = clampCv(presetparser::getFloat(p, "symmetry", 5.0f));
    out.waveFolderGain = clampCv(presetparser::getFloat(p, "gain", 10.0f));
    out.waveFolderMix = clampCv(presetparser::getFloat(p, "mix", 10.0f));
}

} // namespace nodes::wavefolder

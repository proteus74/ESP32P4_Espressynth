#pragma once

#include <cstddef>
#include <cstring>

#include "../CvConventions.h"
#include "../NodeInstance.h"
#include "../PresetParser.h"
#include "../SynthConfig.h"

// Mirror image of MultiplexerNode.h: one audio input ("in") routed to
// whichever of up to 16 outputs ("out1".."out16") is selected - same
// "select" CV-vs-manual-slider convention as MultiplexerState (a genuine CV
// connection wins outright over the widget's own manualSelect), just applied
// to the output side. No soft-fade (see OutputMultiplexerState's comment in
// NodeState.h): every non-selected output is simply left untouched each
// block, so its buffer still holds whatever render() last wrote there while
// it WAS active - the same "frozen by omission" trick OutputSwitchNode.h
// uses. held[] exists only to survive a graph rebuild (preset save/reload
// zeroes every buffer) - build() re-primes every configured output from it.
namespace nodes::outputmultiplexer
{

namespace
{
constexpr int kSelectPort = 1;

// Same band-split as MultiplexerNode.h's own selectFromCv, generalized to
// the output side's runtime outputCount.
inline int selectFromCv(float volts, int outputCount)
{
    if (outputCount <= 1)
        return 0;
    int idx = static_cast<int>(volts / (cv::kMaxVolts / static_cast<float>(outputCount)));
    if (idx < 0)
        idx = 0;
    if (idx > outputCount - 1)
        idx = outputCount - 1;
    return idx;
}

inline int clampIdx(int idx, int outputCount)
{
    if (outputCount <= 0)
        return 0;
    if (idx < 0)
        return 0;
    if (idx > outputCount - 1)
        return outputCount - 1;
    return idx;
}
} // namespace

inline void render(NodeInstance &inst, size_t frames)
{
    using namespace noderender;
    auto &s = *inst.outputMux;
    const bool selectFed =
        controlPortConnected(inst, kSelectPort) || (inst.audioConnectedPortMask & synth::portBit(kSelectPort)) != 0;
    const uint8_t target = static_cast<uint8_t>(selectFed ? selectFromCv(portSample(inst, kSelectPort, 0), s.outputCount)
                                                            : clampIdx(s.manualSelect, s.outputCount));
    s.activeIndex = target;
    float *active = outputPortBuffer(inst, target);
    if (active == nullptr)
    {
        return; // defensive - target's buffer wasn't allocated
    }
    float last = s.held[target];
    for (size_t i = 0; i < frames; ++i)
    {
        last = portSample(inst, 0, i);
        active[i] = last;
    }
    s.held[target] = last;
}

inline void build(NodeInstance &inst, const WidgetParams &p)
{
    auto &s = *inst.outputMux;
    s.outputCount = p.outputMuxCount < 2 ? 2 : (p.outputMuxCount > 16 ? 16 : p.outputMuxCount);
    s.manualSelect = clampIdx(p.outputMuxSelect, s.outputCount);
    s.activeIndex = s.manualSelect;
    for (int i = 0; i < 16; ++i)
    {
        s.held[i] = p.outputMuxHeld[i];
    }
    // Re-prime every configured output from its persisted held value so a
    // freshly rebuilt graph (preset load) shows the last-seen value
    // immediately instead of silence, until each output is actually
    // selected again.
    for (int i = 0; i < s.outputCount; ++i)
    {
        float *buf = outputPortBuffer(inst, static_cast<uint8_t>(i));
        if (buf == nullptr)
        {
            continue;
        }
        for (size_t f = 0; f < synth::kBlockSize; ++f)
        {
            buf[f] = s.held[i];
        }
    }
}

inline bool setParam(NodeInstance &inst, const char *name, float value)
{
    if (name != nullptr && strcmp(name, "select") == 0)
    {
        inst.outputMux->manualSelect = static_cast<uint8_t>(value + 0.5f);
        return true;
    }
    return false;
}

inline void parsePreset(cJSON *p, WidgetParams &out)
{
    const int outputCount = presetparser::getInt(p, "outputCount", 2);
    out.outputMuxCount = static_cast<uint8_t>(outputCount < 2 ? 2 : (outputCount > 16 ? 16 : outputCount));
    const int sel = presetparser::getInt(p, "select", 0);
    out.outputMuxSelect = static_cast<uint8_t>(sel < 0 ? 0 : sel);
    cJSON *held = p != nullptr ? cJSON_GetObjectItemCaseSensitive(p, "held") : nullptr;
    for (int i = 0; i < 16; ++i)
    {
        cJSON *v = cJSON_IsArray(held) ? cJSON_GetArrayItem(held, i) : nullptr;
        out.outputMuxHeld[i] = cJSON_IsNumber(v) ? static_cast<float>(v->valuedouble) : 0.0f;
    }
}

} // namespace nodes::outputmultiplexer

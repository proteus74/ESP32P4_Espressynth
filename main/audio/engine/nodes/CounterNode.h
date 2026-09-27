#pragma once

#include "../CvConventions.h"
#include "../NodeInstance.h"

namespace nodes::counter
{
inline void render(NodeInstance &inst, size_t frames)
{
    auto &s = inst.counter;
    for (size_t i = 0; i < frames; ++i)
    {
        const bool countHigh = noderender::portSample(inst, 0, i) >= cv::kGateThresholdVolts;
        const bool resetHigh = noderender::portSample(inst, 1, i) >= cv::kGateThresholdVolts;
        if (resetHigh)
            s.count = 0;
        else if (countHigh && !s.countWasHigh && s.count < UINT32_MAX)
            ++s.count;
        s.countWasHigh = countHigh;
        inst.outL[i] = static_cast<float>(s.count);
    }
}
} // namespace nodes::counter

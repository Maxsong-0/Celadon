/*
  ==============================================================================

    SegmentLedger.cpp
    T-038 House 档按小节记账（口径见 SegmentLedger.h）。阶段 1 只有骨架，阶段 2 由 B 实现。

  ==============================================================================
*/

#include "SegmentLedger.h"

namespace celadon::glaze
{
    void SegmentLedger::prepare (double newSampleRate)
    {
        sampleRate = newSampleRate;
        reset();
    }

    void SegmentLedger::reset() noexcept
    {
        mode = SegmentMode::none;
        heardSegments = 0;
    }
}

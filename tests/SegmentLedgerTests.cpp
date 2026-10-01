/*
  ==============================================================================

    SegmentLedgerTests.cpp
    T-038 House 档按小节记账（Source/dsp/SegmentLedger.h，设计规格-曲风-House学习方式 第 2 节）的自动测试。
    阶段 1 只核对骨架（复位后无段、分段方式为 none）；阶段 2 由 B 补全：锚点与整数样本边界、按位置覆盖、2 秒退路、容量上限、
    两道门、不同宿主分块大小下逐位相同（恒定速度）。

  ==============================================================================
*/

#include "TestHelpers.h"

#include "../Source/dsp/SegmentLedger.h"

namespace celadon::test
{
    class SegmentLedgerTests  : public juce::UnitTest
    {
    public:
        SegmentLedgerTests() : juce::UnitTest ("SegmentLedger", "Celadon") {}

        void runTest() override
        {
            beginTest ("skeleton: empty after prepare and reset");
            glaze::SegmentLedger ledger;
            ledger.prepare (48000.0);
            expect (ledger.getMode() == glaze::SegmentMode::none);
            expectEquals (ledger.getHeardSegments(), 0);

            ledger.reset();
            expectEquals (ledger.getHeardSegments(), 0);
        }
    };

    static SegmentLedgerTests segmentLedgerTests;
}

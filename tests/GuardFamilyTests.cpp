/*
  ==============================================================================

    GuardFamilyTests.cpp
    T-097 守门族的纯逻辑测试（设计规格-实时GLAZE-主段与增益 第 4 节第 4、5 条）。

    直接构造 100 ms 子块（LoudnessMeter::SubBlock）喂给守门，不经过链与表：均方由目标 LUFS 反算
    （ms = 10^((L + 0.691) / 10)），真峰值由目标 PSR 反算（peak = 10^((PSR + S) / 20)）。步长一律 prepare (0.1)。
    覆盖：
      - ShortTermWindow：不满 30 个子块无效；S（均方平均）与 PSR（3 s 内真峰值最大 − S）的数值；−70 LUFS 绝对门；
        非有限 / 负均方 / 非有限峰值断开窗口（要再攒 30 个，旧值不与新值拼）；
      - PsrGuard：低于门槛满 3 s 才退、每步 0.05 dB、到 −3 停；门槛到门槛 + 0.5 之间两边计时清零、退让保持；
        ≥ 门槛 + 0.5 满 5 s 才以每步 0.015 dB 回 0；读数无效（窗不满、近乎静音）与非有效音乐保持（计时也不动）；
        曲风关着且有效音乐每步放回 0.015；三档交付门槛 7.5 / 6.5 / 5.5；engaged 与 cause 语义；
        存读往返逐位、旧节点缺 aboveSeconds 按 0、损坏拒收且对象不动、读档后窗口重攒；
      - ContrastGuard：先攒满 3 s 窗才进账；遗忘直方图满 60 s 的时刻（729 个有效进账步 ≈ 72.9 s）；
        入口 P90 − P25 < 3 LU 不判定；loss 超门槛满 10 s 每步退 0.01 dB、到 −3 停；标准 1.2 与俱乐部 1.5 的差别；
        回差带保持且两边计时清零；低于门槛 − 0.3 满 30 s 每步回 0.005；曲风关着放回 0（直方图照攒）；
        非有效音乐 / 一侧窗断开时直方图、计时、退让全保持；格内线性插值的分位；同一串子块逐位确定；
        存读（新节点往返逐位、0.1.8.10 要的占位、旧环节点、损坏拒收）；
      - 两个守门的音频线程路径不分配；
      - 常数：守门开关出厂值、按曲风查询、快照四个字段、旧快照缺字段取出厂值、改开关 bitwiseEqual 判不同。

    所有「第几个子块到」都按实现的计时口径推：每步 + 0.1 s，满「阈值 − 半步」即到（PsrGuard / ContrastGuard::reached）。
    推导写在各用例开头；关键数字另在「计时口径」用例里钉住。

  ==============================================================================
*/

#include "TestHelpers.h"

#include "../Source/dsp/ContrastGuard.h"
#include "../Source/dsp/GlazeConstants.h"
#include "../Source/dsp/LoudnessMeter.h"
#include "../Source/dsp/ProfileMath.h"
#include "../Source/dsp/PsrGuard.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>

namespace celadon::test
{
    namespace
    {
        using glaze::ContrastGuard;
        using glaze::Delivery;
        using glaze::GuardCause;
        using glaze::GuardOutput;
        using glaze::PsrGuard;
        using glaze::ShortTermWindow;
        using SubBlock = LoudnessMeter::SubBlock;

        //==============================================================================
        // 共用口径
        constexpr double subBlockSeconds = 0.1;     // prepare (0.1)：一个 100 ms 子块走一步
        constexpr float guardFloorDb = -3.0f;       // 两个守门的 floorDb（规格：退到 −3 为止）
        constexpr int windowFillSubBlocks = ShortTermWindow::subBlocks - 1;   // 29：从空窗起，第 30 个子块 3 s 窗才满

        // IEEE 754 位模式（GlazeConstants 的 xmlio：float 8 位、double 16 位十六进制，高位在前）。
        constexpr const char* floatMinusOneHex = "bf800000";          // −1.0f
        constexpr const char* floatMinusThreeHex = "c0400000";        // −3.0f
        constexpr const char* floatMinusFourteenHex = "c1600000";     // −14.0f（旧版 128 笔环里的一个 S 值）
        constexpr const char* floatNanHex = "7fc00000";               // float NaN
        constexpr const char* doubleZeroHex = "0000000000000000";     // 0.0
        constexpr const char* doubleNineHex = "4022000000000000";     // 9.0 = 1.125 × 2^3
        constexpr const char* doubleTwentyNineHex = "403d000000000000";   // 29.0 = 1.8125 × 2^4
        constexpr const char* doubleMinusOneHex = "bff0000000000000";     // −1.0
        constexpr const char* doubleNanHex = "7ff8000000000000";          // double NaN

        // 目标 LUFS → K 加权均方（LUFS = −0.691 + 10·log10(ms) 的反算）。
        double meanSquareFor (double lufs)
        {
            return std::pow (10.0, (lufs + 0.691) / 10.0);
        }

        SubBlock makeSubBlock (double meanSquare, float truePeakLinear)
        {
            SubBlock block;
            block.meanSquare = meanSquare;
            block.truePeakLinear = truePeakLinear;
            block.endSample = 0;   // 守门不读时间戳（编排器按它配对两只表的子块）
            return block;
        }

        // 计时口径（PsrGuard / ContrastGuard::reached）：每步 + 0.1 s，seconds ≥ threshold − 半步即到。返回第几步到
        // （与实现同一累加顺序）。推导：start + 0.1·n ≥ T − 0.05 ⟺ n ≥ 10 (T − start) − 0.5，半步余量吸收累加误差
        // → 从 0 起 n = 10 T（3 s → 30，5 s → 50，10 s → 100，30 s → 300）；从 29.0 起到 30 s：n = 10。
        int stepsToReach (double thresholdSeconds, double startSeconds = 0.0)
        {
            auto seconds = startSeconds;
            auto steps = 0;

            while (! (seconds >= thresholdSeconds - 0.5 * subBlockSeconds))
            {
                seconds += subBlockSeconds;
                ++steps;
            }

            return steps;
        }

        //==============================================================================
        // PSR 守门：子块响度固定在 psrTestLufs，PSR 只靠真峰值调（peak = 10^((PSR + S) / 20)）。
        // 窗内均方全相同 → S = psrTestLufs（浮点误差 ~1e−13）；读数 = 20·log10(30 个子块真峰值最大) − S（峰值是 float，误差 ~1e−6 dB）。
        // 往上换 PSR 立刻生效（新子块的峰值就是最大值）；往下换要等旧的大峰值被挤出窗（30 个子块）。
        constexpr double psrTestLufs = -12.0;

        SubBlock psrBlock (double psrDb)
        {
            return makeSubBlock (meanSquareFor (psrTestLufs), static_cast<float> (std::pow (10.0, (psrDb + psrTestLufs) / 20.0)));
        }

        struct PsrFeeder
        {
            PsrFeeder()
            {
                guard.prepare (subBlockSeconds);
                guard.setFloorDb (guardFloorDb);
            }

            GuardOutput feedBlock (const SubBlock& block, bool music = true, bool enabled = true, Delivery delivery = Delivery::standard)
            {
                ++calls;
                return guard.process (block, music, enabled, delivery);
            }

            GuardOutput feed (double psrDb, bool music = true, bool enabled = true, Delivery delivery = Delivery::standard)
            {
                return feedBlock (psrBlock (psrDb), music, enabled, delivery);
            }

            float relief() const noexcept   { return guard.current().reliefDb; }

            PsrGuard guard;
            int calls = 0;   // 已喂的子块数（喂完第 k 个时 = k）
        };

        //==============================================================================
        // 落差守门：入口两档交替（轻 15 s、重 15 s），出口重档不变、轻档抬高 loss LU。
        // 档位都放在直方图格中心（−70 + (格 + 0.5) / 10），3 s 窗的 S（浮点误差 ~1e−13）不会落进邻格。
        // 为什么 loss 恰好等于「出口轻档 − 入口轻档」：
        //   - 段落切换时 3 s 窗有 29 个过渡值，都严格在两档之间，且离两档都超过半格：离重档最近的是
        //     重档 + 10·log10((29 + 10^(−Δ/10)) / 30)（Δ = 10 时 −0.13，Δ = 7 时 −0.12），离轻档最近的是
        //     轻档 + 10·log10((29 + 10^(Δ/10)) / 30) ≥ 轻档 + 0.5（Δ ≥ 7）。所以轻档格、重档格里只有稳态值，
        //     轻档格之下、重档格之上没有值；
        //   - 每 30 s 稳态各 121 个（150 − 29），各占约 40 %（遗忘下随相位约 35 ～ 45 %）：P25 在轻档格、P90 在重档格；
        //   - 入口、出口同一时刻进账、同样遗忘：两侧轻档格权重逐位相同，重档格之下的累计也相同（只差浮点求和顺序）
        //     → 格内插值位置相同，P25 之差 = 轻档格号之差 / 10，P90 相同 → loss = 出口轻档 − 入口轻档（误差 ~1e−6）。
        //   每侧 spread = 两档之差 + 格内插值零头（−0.1 ～ +0.1）。
        constexpr int sectionSubBlocks = 150;       // 每档 15 s
        constexpr double inputQuietLufs = -20.05;   // 格 499 中心（下沿 −20.1）
        constexpr double loudLufs = -10.05;         // 格 599 中心（下沿 −10.1）；入口、出口的重档
        constexpr float peakForLevels = 0.5f;       // 落差守门不看峰值，给一个有限值

        SubBlock levelBlock (double lufs)
        {
            return makeSubBlock (meanSquareFor (lufs), peakForLevels);
        }

        struct ContrastFeeder
        {
            // lossLu 取 0.1 的整数倍（出口轻档仍在格中心）。firstIndex：从图样的第几个子块接着喂（读档接续用）。
            explicit ContrastFeeder (double lossLu, bool clubDelivery = false, int firstIndex = 0)
                : outputQuietLufs (inputQuietLufs + lossLu), club (clubDelivery), nextIndex (firstIndex)
            {
                guard.prepare (subBlockSeconds);
                guard.setFloorDb (guardFloorDb);
            }

            GuardOutput feed (bool music = true, bool enabled = true)
            {
                const auto quiet = (nextIndex / sectionSubBlocks) % 2 == 0;
                ++nextIndex;
                return guard.process (levelBlock (quiet ? inputQuietLufs : loudLufs),
                                      levelBlock (quiet ? outputQuietLufs : loudLufs), music, enabled, club);
            }

            float relief() const noexcept   { return guard.current().reliefDb; }
            ContrastGuard::Reading reading() const noexcept   { return guard.getLastReading(); }

            ContrastGuard guard;
            double outputQuietLufs;
            bool club;
            int nextIndex;   // 下一个子块在图样里的下标；从 0 起喂时 = 已喂的子块数（喂完第 k 个时 = k）
        };

        // 直方图「满 60 s」要多少个有效进账步：histogramSeconds = histogramSeconds·d + 0.1（d = e^(−0.1/180)），
        // 即 0.1·Σ_{k<m} d^k = 0.1·(1 − d^m)/(1 − d)；要 ≥ 60 − 0.05 = 59.95：
        // 1 − d^m ≥ 599.5·(1 − d) = 0.33296 → m ≥ ln(0.66704) / ln(d) = 728.84 → m = 729（m = 728 时 59.894，m = 729 时 59.961）。
        int measuredStepsToFull()
        {
            const auto d = std::exp (-subBlockSeconds / ContrastGuard::forgetSeconds);
            auto seconds = 0.0;
            auto steps = 0;

            while (! (seconds >= ContrastGuard::fullSeconds - 0.5 * subBlockSeconds))
            {
                seconds = seconds * d + subBlockSeconds;
                ++steps;
            }

            return steps;
        }

        // m 个有效进账步之后的 histogramSeconds（闭式）。
        double histogramSecondsAfter (int measuredSteps)
        {
            const auto d = std::exp (-subBlockSeconds / ContrastGuard::forgetSeconds);
            return subBlockSeconds * (1.0 - std::pow (d, measuredSteps)) / (1.0 - d);
        }

        // 跑一个新守门到图样第 subBlockCount 个子块，交出存档与那一刻的读数。
        std::unique_ptr<juce::XmlElement> contrastNodeAfter (double lossLu, int subBlockCount, ContrastGuard::Reading& last)
        {
            ContrastFeeder a (lossLu);

            while (a.nextIndex < subBlockCount)
                a.feed();

            last = a.reading();
            return a.guard.createStateXml();
        }

        // 十六进制数组属性（每个 float 8 位）里第 item 个换成 hex8。
        void replaceArrayItem (juce::XmlElement& xml, const char* attribute, int item, const char* hex8)
        {
            const auto text = xml.getStringAttribute (attribute);
            xml.setAttribute (attribute, text.substring (0, item * 8) + hex8 + text.substring (item * 8 + 8));
        }
    }

    //==============================================================================
    class GuardFamilyTests  : public juce::UnitTest
    {
    public:
        GuardFamilyTests() : juce::UnitTest ("GLAZE guard family (T-097, pure logic)", "Celadon") {}

        void runTest() override
        {
            testShortTermWindow();
            testTimingArithmetic();

            testPsrRetreat();
            testPsrBand();
            testPsrRelease();
            testPsrHolds();
            testPsrDisabled();
            testPsrDeliveries();
            testPsrState();

            testContrastHistogramAndRetreat();
            testContrastSpreadPrecondition();
            testContrastThresholds();
            testContrastHysteresis();
            testContrastHolds();
            testContrastDisabled();
            testContrastPercentiles();
            testContrastDeterminism();
            testContrastState();

            testGuardsDoNotAllocate();
            testGuardSwitchConstants();
        }

    private:
        //==============================================================================
        void testShortTermWindow()
        {
            beginTest ("ShortTermWindow: 30 sub-blocks make 3 s; S = -0.691 + 10 log10(mean of mean squares); PSR = max true peak - S; gate; breaks");

            // 1. 不满 30 个无效（输出参数不动）；第 30 个起有效。全 −20 LUFS：S = −20（误差 ~1e−13，容差 1e−9）；
            //    峰值 0.1f：PSR = 20·log10(0.1) + 20 = 0（0.1f 的 float 误差 ~1.3e−7 dB，容差 1e−5）。
            {
                ShortTermWindow w;
                auto lufs = 123.0, psr = 456.0;
                expect (! w.isFull() && ! w.shortTermLufs (lufs) && ! w.psrDb (psr), "empty window is invalid");

                for (int i = 0; i < windowFillSubBlocks; ++i)
                    w.push (meanSquareFor (-20.0), 0.1f);

                expect (! w.isFull(), "29 sub-blocks are not 3 s");
                expect (! w.shortTermLufs (lufs) && lufs == 123.0, "29 sub-blocks: S invalid, output untouched");
                expect (! w.psrDb (psr) && psr == 456.0, "29 sub-blocks: PSR invalid, output untouched");

                w.push (meanSquareFor (-20.0), 0.1f);
                expect (w.isFull(), "the 30th sub-block fills the window");
                expect (w.shortTermLufs (lufs), "30 sub-blocks: S valid");
                expectWithinAbsoluteError (lufs, -20.0, 1.0e-9, "S of 30 equal -20 LUFS sub-blocks");
                expect (w.psrDb (psr), "30 sub-blocks: PSR valid");
                expectWithinAbsoluteError (psr, 0.0, 1.0e-5, "PSR = 20 log10(0.1) + 20");
            }

            // 2. 均方平均（不是 LUFS 平均）：15 个 −20 + 15 个 −10 → S = −20 + 10·log10((15·1 + 15·10) / 30) = −20 + 10·log10(5.5) = −12.5964；
            //    再压 30 个 −10 → 旧值全被挤出，S = −10；再压 10 个 −20 → 20 个 −10 + 10 个 −20：S = −10 + 10·log10(21 / 30) = −11.5490。
            {
                ShortTermWindow w;
                auto lufs = 0.0;

                for (int i = 0; i < 15; ++i)
                    w.push (meanSquareFor (-20.0), 0.1f);

                for (int i = 0; i < 15; ++i)
                    w.push (meanSquareFor (-10.0), 0.1f);

                expect (w.shortTermLufs (lufs));
                expectWithinAbsoluteError (lufs, -20.0 + 10.0 * std::log10 (5.5), 1.0e-9, "S averages mean squares, not LUFS");

                for (int i = 0; i < 30; ++i)
                    w.push (meanSquareFor (-10.0), 0.1f);

                expect (w.shortTermLufs (lufs));
                expectWithinAbsoluteError (lufs, -10.0, 1.0e-9, "ring: 30 new sub-blocks replace all old ones");

                for (int i = 0; i < 10; ++i)
                    w.push (meanSquareFor (-20.0), 0.1f);

                expect (w.shortTermLufs (lufs));
                expectWithinAbsoluteError (lufs, -10.0 + 10.0 * std::log10 (21.0 / 30.0), 1.0e-9, "ring: the 10 newest replace the 10 oldest");
            }

            // 3. PSR 取 3 s 内真峰值最大：30 个 −14 LUFS，其中一个峰值 0.5、其余 0.25 → PSR = 20·log10(0.5) + 14 = 7.9794；
            //    再压 30 个峰值 0.25 → 0.5 被挤出：PSR = 20·log10(0.25) + 14 = 1.9588。0.5f / 0.25f 精确，容差 1e−9。
            {
                ShortTermWindow w;
                auto psr = 0.0;

                for (int i = 0; i < 30; ++i)
                    w.push (meanSquareFor (-14.0), i == 7 ? 0.5f : 0.25f);

                expect (w.psrDb (psr));
                expectWithinAbsoluteError (psr, 20.0 * std::log10 (0.5) + 14.0, 1.0e-9, "PSR uses the largest true peak in the window");

                for (int i = 0; i < 30; ++i)
                    w.push (meanSquareFor (-14.0), 0.25f);

                expect (w.psrDb (psr));
                expectWithinAbsoluteError (psr, 20.0 * std::log10 (0.25) + 14.0, 1.0e-9, "an evicted peak no longer counts");
            }

            // 4. 绝对门 −70 LUFS：30 个 −70.1 → 满但无效（S、PSR 都 false、输出不动）；−69.9 有效；全 0 均方（−∞）无效；
            //    S 有效但 3 s 内峰值全 0 → PSR 无效。
            {
                ShortTermWindow below, above, silent, noPeak;

                for (int i = 0; i < 30; ++i)
                {
                    below.push (meanSquareFor (-70.1), 0.001f);
                    above.push (meanSquareFor (-69.9), 0.001f);
                    silent.push (0.0, 0.0f);
                    noPeak.push (meanSquareFor (-20.0), 0.0f);
                }

                auto lufs = 5.0, psr = 6.0;
                expect (below.isFull() && ! below.shortTermLufs (lufs) && ! below.psrDb (psr) && lufs == 5.0 && psr == 6.0,
                        "below -70 LUFS: full but invalid, outputs untouched");
                expect (above.shortTermLufs (lufs), "above -70 LUFS: valid");
                expectWithinAbsoluteError (lufs, -69.9, 1.0e-9);
                expect (silent.isFull() && ! silent.shortTermLufs (lufs) && ! silent.psrDb (psr), "digital silence (-inf LUFS) is invalid");
                expect (noPeak.shortTermLufs (lufs) && ! noPeak.psrDb (psr), "valid S with all-zero peaks: PSR invalid");
            }

            // 5. 非有限 / 负均方、非有限峰值：断开窗口（清空），要再攒 30 个；旧值不与新值拼
            //    （之前全 −10，之后 30 个 −20 → S 正好 −20；拼进旧值会偏高）。
            {
                const double badEnergies[] = { std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(), -1.0e-6 };
                const float badPeaks[] = { std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity() };

                for (int which = 0; which < 5; ++which)
                {
                    ShortTermWindow w;

                    for (int i = 0; i < 30; ++i)
                        w.push (meanSquareFor (-10.0), 0.1f);

                    if (which < 3)
                        w.push (badEnergies[which], 0.1f);
                    else
                        w.push (meanSquareFor (-10.0), badPeaks[which - 3]);

                    const auto label = "bad sub-block " + juce::String (which);
                    auto lufs = 0.0;
                    expect (! w.isFull() && ! w.shortTermLufs (lufs), label + " clears the window");

                    for (int i = 0; i < windowFillSubBlocks; ++i)
                        w.push (meanSquareFor (-20.0), 0.1f);

                    expect (! w.isFull(), label + ": 29 sub-blocks after it are not 3 s");

                    w.push (meanSquareFor (-20.0), 0.1f);
                    expect (w.shortTermLufs (lufs), label + ": valid again after 30");
                    expectWithinAbsoluteError (lufs, -20.0, 1.0e-9, label + ": old sub-blocks are not mixed in");
                }
            }
        }

        //==============================================================================
        void testTimingArithmetic()
        {
            beginTest ("timing arithmetic used below: n steps of 0.1 s reach T at n = 10 T; histograms full after 729 measured steps");

            expectEquals (stepsToReach (PsrGuard::sustainSeconds), 30);
            expectEquals (stepsToReach (PsrGuard::releaseAfterSeconds), 50);
            expectEquals (stepsToReach (ContrastGuard::triggerSeconds), 100);
            expectEquals (stepsToReach (ContrastGuard::releaseAfterSeconds), 300);
            expectEquals (stepsToReach (ContrastGuard::releaseAfterSeconds, 29.0), 10);
            expectEquals (measuredStepsToFull(), 729);
            expectWithinAbsoluteError (histogramSecondsAfter (728), 59.894092, 1.0e-5, "728 steps: short of 59.95");
            expectWithinAbsoluteError (histogramSecondsAfter (729), 59.960827, 1.0e-5, "729 steps: past 59.95");
        }

        //==============================================================================
        void testPsrRetreat()
        {
            beginTest ("PSR guard: below the trip for 3 s of valid readings, then 0.05 dB per sub-block down to the floor; engaged and cause");

            // 标准档 trip = floorFor 8 − 0.5 = 7.5，一直喂 PSR 6：
            //   第 1 ～ 29 个：窗不满，读数无效 → 保持（计时不动）；
            //   第 30 个起读数有效（≈ 6）且 < 7.5，belowSeconds = 0.1 × (k − 29)；第 29 + 30 = 59 个累到 3.0 ≥ 2.95
            //   → 第一次退 0.5 dB/s × 0.1 s = 0.05 dB；之后每个子块再退 0.05；
            //   60 步退满 3 dB（float 累加可能多一步）→ 第 59 + 61 = 120 个之后必为 −3（钳位逐位等于 floorDb），不再往下。
            const auto firstRetreat = windowFillSubBlocks + stepsToReach (PsrGuard::sustainSeconds);   // 59
            PsrFeeder f;
            auto psr = 0.0f;
            auto firstBad = 0;

            for (int k = 1; k <= windowFillSubBlocks; ++k)
            {
                const auto out = f.feed (6.0);

                if ((out.reliefDb != 0.0f || out.engaged || out.cause != GuardCause::none || f.guard.getLastPsrDb (psr)) && firstBad == 0)
                    firstBad = k;
            }

            expectEquals (firstBad, 0, "window filling: reading invalid, nothing moves");

            for (int k = windowFillSubBlocks + 1; k < firstRetreat; ++k)
            {
                const auto out = f.feed (6.0);
                const auto valid = f.guard.getLastPsrDb (psr);

                if ((out.reliefDb != 0.0f || out.engaged || ! valid || std::abs (psr - 6.0f) > 1.0e-4f) && firstBad == 0)
                    firstBad = k;
            }

            expectEquals (firstBad, 0, "valid PSR 6 below 7.5 for less than 3 s: no retreat yet");
            expectEquals (f.calls, firstRetreat - 1);

            auto out = f.feed (6.0);
            expectWithinAbsoluteError (out.reliefDb, -0.05f, 1.0e-6f, "first retreat at sub-block 59: 0.5 dB/s x 0.1 s");
            expect (out.engaged && out.cause == GuardCause::psr, "retreating: engaged, cause psr");

            const auto now = f.guard.current();
            expect (now.reliefDb == out.reliefDb && now.engaged == out.engaged && now.cause == out.cause, "current() returns the last step");

            // 第 59 + j 个：relief ≈ −0.05 (j + 1)，j = 1 … 57（到 −2.9，还没碰底）；每步差 −0.05（float 误差 ~3e−7）。
            auto previous = out.reliefDb;

            for (int j = 1; j <= 57; ++j)
            {
                out = f.feed (6.0);

                if ((std::abs (out.reliefDb - previous + 0.05f) > 1.0e-5f
                     || std::abs (out.reliefDb + 0.05f * static_cast<float> (j + 1)) > 1.0e-4f || ! out.engaged) && firstBad == 0)
                    firstBad = f.calls;

                previous = out.reliefDb;
            }

            expectEquals (firstBad, 0, "0.05 dB per sub-block while retreating, engaged");

            while (f.calls < firstRetreat + 61)
                out = f.feed (6.0);

            expectEquals (out.reliefDb, guardFloorDb, "60 (61 with float rounding) steps reach the floor exactly");

            for (int k = 0; k < 100; ++k)
                out = f.feed (6.0);

            expectEquals (out.reliefDb, guardFloorDb, "stops at the floor");
            expect (out.engaged && out.cause == GuardCause::psr, "at the floor and still below the trip: engaged");
        }

        //==============================================================================
        void testPsrBand()
        {
            beginTest ("PSR guard: between the trip and trip + 0.5 both timers clear and the relief holds");

            // 1. 计时清零：PSR 6 喂到第 54 个（有效 25 个，belowSeconds 2.5），第 55 个换 PSR 7.75（往上换立刻生效，落在 [7.5, 8) 带内）
            //    → belowSeconds 清零；之后再喂 PSR 6，但 7.75 的峰值要第 55 + 30 = 85 个才挤出窗：第 56 ～ 84 个仍读 7.75（带内），
            //    第 85 个起读 6、从 0 重新计时 → 第 85 + 29 = 114 个第一次退（不清零的话第 59 个就退了）。
            {
                PsrFeeder f;
                auto psr = 0.0f;
                auto firstBad = 0;

                while (f.calls < 54)
                    f.feed (6.0);

                f.feed (7.75);
                expect (f.guard.getLastPsrDb (psr) && std::abs (psr - 7.75f) < 1.0e-4f, "a higher peak shows at once");

                const auto firstRetreat = 55 + ShortTermWindow::subBlocks + stepsToReach (PsrGuard::sustainSeconds) - 1;   // 114

                while (f.calls < firstRetreat - 1)
                    if (f.feed (6.0).reliefDb != 0.0f && firstBad == 0)
                        firstBad = f.calls;

                expectEquals (firstBad, 0, "after one sub-block in the band the 3 s start over");
                expect (f.feed (6.0).reliefDb < 0.0f, "sub-block 114: 3 s of valid readings below the trip since the band");
                expectEquals (f.calls, firstRetreat);
            }

            // 2. 退让保持：退到约 −1 dB（第 59 + 19 = 78 个）或到底（第 200 个）后换 PSR 7.75：300 个子块 relief 逐位不变，
            //    engaged false，cause 仍是 psr。
            for (const auto retreatCalls : { 78, 200 })
            {
                PsrFeeder f;

                while (f.calls < retreatCalls)
                    f.feed (6.0);

                const auto held = f.relief();
                expect (held < 0.0f);
                auto firstBad = 0;

                for (int k = 0; k < 300; ++k)
                {
                    const auto out = f.feed (7.75);

                    if ((out.reliefDb != held || out.engaged || out.cause != GuardCause::psr) && firstBad == 0)
                        firstBad = f.calls;
                }

                expectEquals (firstBad, 0, "in the band the relief holds bit for bit (after " + juce::String (retreatCalls) + " sub-blocks)");
            }
        }

        //==============================================================================
        void testPsrRelease()
        {
            beginTest ("PSR guard: at or above trip + 0.5 for 5 s, then 0.015 dB per sub-block back to 0");

            // 先退到底（PSR 6 喂 200 个），再喂 PSR 9（≥ 8，往上换立刻生效）：aboveSeconds = 0.1·j，
            // 第 50 个（5.0 ≥ 4.95）第一次回 0.15 dB/s × 0.1 s = 0.015 dB；回满 3 dB 要 200 步（float 累加可能多一步）。
            // 下面共喂 49 + 1 + 150 + 60 = 260 个 PSR 9，回了 211 步 ≥ 201 → 必为 0（钳位逐位等于 0），cause 回 none。回程中 engaged = false。
            PsrFeeder f;

            while (f.calls < 200)
                f.feed (6.0);

            expectEquals (f.relief(), guardFloorDb);

            const auto firstRelease = stepsToReach (PsrGuard::releaseAfterSeconds);   // 50
            auto firstBad = 0;

            for (int j = 1; j < firstRelease; ++j)
                if (f.feed (9.0).reliefDb != guardFloorDb && firstBad == 0)
                    firstBad = j;

            expectEquals (firstBad, 0, "above trip + 0.5 for less than 5 s: still at the floor");

            auto out = f.feed (9.0);
            expectWithinAbsoluteError (out.reliefDb, guardFloorDb + 0.015f, 1.0e-5f, "first release step after 5 s: 0.15 dB/s x 0.1 s");
            expect (! out.engaged && out.cause == GuardCause::psr, "releasing: not engaged, cause still psr");

            auto previous = out.reliefDb;

            for (int j = 1; j <= 150; ++j)
            {
                out = f.feed (9.0);

                if ((std::abs (out.reliefDb - previous - 0.015f) > 1.0e-5f || out.engaged) && firstBad == 0)
                    firstBad = j;

                previous = out.reliefDb;
            }

            expectEquals (firstBad, 0, "0.015 dB per sub-block while releasing");

            for (int j = 0; j < 60; ++j)
                out = f.feed (9.0);

            expectEquals (out.reliefDb, 0.0f, "back to exactly 0");
            expect (! out.engaged && out.cause == GuardCause::none, "at 0: cause none, not engaged");
        }

        //==============================================================================
        void testPsrHolds()
        {
            beginTest ("PSR guard: invalid readings (window filling, near silence) and non-music hold both timers and the relief");

            // 1. 窗口断开（clearWindow）：先有效 20 个（belowSeconds 2.0），断开后 29 个读数无效、计时不动，
            //    再有效 10 个（2.0 + 1.0 = 3.0 ≥ 2.95）第一次退。计时被清零的话要 30 个，被继续累加的话会更早。
            {
                PsrFeeder f;

                while (f.calls < windowFillSubBlocks + 20)
                    f.feed (6.0);

                f.guard.clearWindow();
                auto psr = 0.0f;
                expect (! f.guard.getLastPsrDb (psr), "clearWindow drops the reading");

                auto firstBad = 0;

                for (int k = 1; k <= windowFillSubBlocks + 9; ++k)
                {
                    const auto out = f.feed (6.0);
                    const auto valid = f.guard.getLastPsrDb (psr);

                    if ((out.reliefDb != 0.0f || valid != (k > windowFillSubBlocks)) && firstBad == 0)
                        firstBad = k;
                }

                expectEquals (firstBad, 0, "29 invalid sub-blocks hold, then 9 valid ones are still short of 3 s");
                expect (f.feed (6.0).reliefDb < 0.0f, "the 10th valid sub-block completes 2.0 + 1.0 s");
            }

            // 2. 近乎静音（S < −70 LUFS）：退到约 −1 dB 后断开窗口、喂 100 个 −80 LUFS 子块（有效音乐标志照给 true）
            //    → 读数一直无效，relief 逐位不变。
            {
                PsrFeeder f;

                while (f.calls < 78)
                    f.feed (6.0);

                const auto held = f.relief();
                f.guard.clearWindow();
                auto psr = 0.0f;
                auto firstBad = 0;

                for (int k = 1; k <= 100; ++k)
                {
                    const auto out = f.feedBlock (makeSubBlock (meanSquareFor (-80.0), 1.0e-4f));

                    if ((out.reliefDb != held || f.guard.getLastPsrDb (psr)) && firstBad == 0)
                        firstBad = k;
                }

                expectEquals (firstBad, 0, "near silence: reading invalid, relief held bit for bit");
            }

            // 3. 非有效音乐：有效 20 个（2.0 s）后 50 个 musicActive = false（读数照算、照样有效）→ 计时与退让不动；
            //    恢复有效音乐后第 10 个第一次退。已经退了的 relief 在非有效音乐里也逐位不变（开着、关着都一样）。
            {
                PsrFeeder f;

                while (f.calls < windowFillSubBlocks + 20)
                    f.feed (6.0);

                auto psr = 0.0f;
                auto firstBad = 0;

                for (int k = 1; k <= 50; ++k)
                {
                    const auto out = f.feed (6.0, false);
                    const auto valid = f.guard.getLastPsrDb (psr);

                    if ((out.reliefDb != 0.0f || ! valid) && firstBad == 0)
                        firstBad = k;
                }

                expectEquals (firstBad, 0, "non-music: no retreat although PSR 6 is read");

                for (int k = 1; k <= 9; ++k)
                    if (f.feed (6.0).reliefDb != 0.0f && firstBad == 0)
                        firstBad = k;

                expectEquals (firstBad, 0, "music again: 9 more valid sub-blocks are still short of 3 s");
                expect (f.feed (6.0).reliefDb < 0.0f, "the 10th completes 2.0 + 1.0 s (the timer was held, not cleared)");

                while (f.calls < 150)
                    f.feed (6.0);

                const auto held = f.relief();
                expect (held < 0.0f);

                for (int k = 1; k <= 100; ++k)
                    if (f.feed (6.0, false).reliefDb != held && firstBad == 0)
                        firstBad = k;

                expectEquals (firstBad, 0, "non-music holds a retreated relief bit for bit");

                for (int k = 1; k <= 100; ++k)
                    if (f.feed (6.0, false, false).reliefDb != held && firstBad == 0)
                        firstBad = k;

                expectEquals (firstBad, 0, "non-music with the guard switched off also holds");
            }
        }

        //==============================================================================
        void testPsrDisabled()
        {
            beginTest ("PSR guard: switched off for this genre with music - timers clear, the relief goes back to 0 at 0.015 dB per sub-block");

            // 退到底后 enabled = false、PSR 照旧 6（开着会继续退）：每个子块 +0.015，第 1 个就动（不等 5 s）；
            // 200 步（float 可能多一步）回到 0，喂 211 个之后必为 0。再打开：计时已清零、窗口是满的 → 第 30 个有效子块才退。
            PsrFeeder f;

            while (f.calls < 200)
                f.feed (6.0);

            expectEquals (f.relief(), guardFloorDb);

            auto out = f.feed (6.0, true, false);
            expectWithinAbsoluteError (out.reliefDb, guardFloorDb + 0.015f, 1.0e-5f, "switched off: releases from the first sub-block");
            expect (! out.engaged, "switched off: not engaged");

            for (int k = 0; k < 210; ++k)
                out = f.feed (6.0, true, false);

            expectEquals (out.reliefDb, 0.0f, "switched off: back to exactly 0");
            expect (out.cause == GuardCause::none);

            auto firstBad = 0;

            for (int k = 1; k < stepsToReach (PsrGuard::sustainSeconds); ++k)
                if (f.feed (6.0).reliefDb != 0.0f && firstBad == 0)
                    firstBad = k;

            expectEquals (firstBad, 0, "switched on again: the 3 s start from 0");
            expect (f.feed (6.0).reliefDb < 0.0f, "the 30th valid sub-block below the trip retreats");
        }

        //==============================================================================
        void testPsrDeliveries()
        {
            beginTest ("PSR guard: delivery floors 8 / 7 / 6 dB, trips 7.5 / 6.5 / 5.5 dB");

            expectEquals (PsrGuard::floorFor (Delivery::standard), 8.0f);
            expectEquals (PsrGuard::floorFor (Delivery::streaming), 7.0f);
            expectEquals (PsrGuard::floorFor (Delivery::club), 6.0f);
            expectEquals (PsrGuard::tripFor (Delivery::standard), 7.5f);
            expectEquals (PsrGuard::tripFor (Delivery::streaming), 6.5f);
            expectEquals (PsrGuard::tripFor (Delivery::club), 5.5f);

            // 每档：trip − 0.2 → 第 59 个退（同 testPsrRetreat 的推导）；trip + 0.2（带内）与 trip + 0.7（≥ trip + 0.5）→ 200 个子块都不动。
            const auto firstRetreat = windowFillSubBlocks + stepsToReach (PsrGuard::sustainSeconds);   // 59

            for (const auto delivery : { Delivery::standard, Delivery::streaming, Delivery::club })
            {
                const auto trip = static_cast<double> (PsrGuard::tripFor (delivery));
                const auto label = "delivery " + juce::String (static_cast<int> (delivery));

                PsrFeeder below;

                while (below.calls < firstRetreat - 1)
                    below.feed (trip - 0.2, true, true, delivery);

                expectEquals (below.relief(), 0.0f, label + ": trip - 0.2 for less than 3 s");
                expect (below.feed (trip - 0.2, true, true, delivery).reliefDb < 0.0f, label + ": trip - 0.2 retreats at sub-block 59");

                for (const auto offset : { 0.2, 0.7 })
                {
                    PsrFeeder f;

                    while (f.calls < 200)
                        f.feed (trip + offset, true, true, delivery);

                    expectEquals (f.relief(), 0.0f, label + ": trip + " + juce::String (offset, 1) + " never retreats");
                }
            }
        }

        //==============================================================================
        void testPsrState()
        {
            beginTest ("PSR guard state: bitwise round trip, legacy node without aboveSeconds, corrupt nodes rejected, window refilled after restore");

            // A：退到底后喂 PSR 9 共 70 个 → 第 50 个起回，relief ≈ −3 + 0.015 × 21 = −2.685，aboveSeconds = 7.0（≥ 4.95）。
            PsrFeeder a;

            while (a.calls < 200)
                a.feed (6.0);

            for (int j = 0; j < 70; ++j)
                a.feed (9.0);

            expectWithinAbsoluteError (a.relief(), guardFloorDb + 0.015f * 21.0f, 1.0e-4f);

            const auto saved = a.guard.createStateXml();
            expect (saved != nullptr && saved->hasTagName ("PSR_GUARD"), "node tag");

            // 1. 往返逐位：createStateXml → 新对象 restore → 再 createStateXml。
            PsrFeeder b;
            expect (b.guard.restoreStateXml (*saved), "restores");
            const auto again = b.guard.createStateXml();
            expect (saved->isEquivalentTo (again.get(), false), "createStateXml after restore is bit for bit the saved node");
            expectEquals (b.relief(), a.relief());

            // 2. 读档后窗口是空的：前 29 个读数无效、退让不动；第 30 个起有效，aboveSeconds 接着 7.0 → 第 30 个就回（不用再等 5 s）。
            //    A 断开窗口后与 B 状态相同 → 同一串子块逐位同轨。
            a.guard.clearWindow();
            const auto restoredRelief = b.relief();
            auto psrA = 0.0f, psrB = 0.0f;
            auto firstBad = 0;

            for (int k = 1; k <= 300; ++k)
            {
                const auto value = k < 150 ? 9.0 : 6.0;
                const auto outA = a.feed (value);
                const auto outB = b.feed (value);
                const auto validA = a.guard.getLastPsrDb (psrA);
                const auto validB = b.guard.getLastPsrDb (psrB);

                if ((outA.reliefDb != outB.reliefDb || outA.engaged != outB.engaged || validA != validB
                     || validB != (k > windowFillSubBlocks)
                     || (k <= windowFillSubBlocks && outB.reliefDb != restoredRelief)
                     || (k == windowFillSubBlocks + 1 && ! (outB.reliefDb > restoredRelief))) && firstBad == 0)
                    firstBad = k;
            }

            expectEquals (firstBad, 0, "restored guard: 29 invalid sub-blocks hold, the 30th resumes the 7 s above-timer; same track as the original");

            // 3. 旧节点（T-097 之前没有 aboveSeconds）：读得进，aboveSeconds 按 0（写出的属性与新对象的 0 相同），退让照旧；
            //    行为上要重新攒满 5 s 才回：29 个攒窗 + 第 50 个有效子块 → 第 79 个第一次回。
            {
                juce::XmlElement legacy (*saved);
                legacy.removeAttribute ("aboveSeconds");

                PsrFeeder c, fresh;
                expect (c.guard.restoreStateXml (legacy), "legacy node without aboveSeconds restores");

                const auto cXml = c.guard.createStateXml();
                const auto freshXml = fresh.guard.createStateXml();
                expectEquals (cXml->getStringAttribute ("aboveSeconds"), freshXml->getStringAttribute ("aboveSeconds"), "missing aboveSeconds reads as 0");
                expectEquals (cXml->getStringAttribute ("reliefDb"), saved->getStringAttribute ("reliefDb"), "relief kept");

                const auto kept = c.relief();
                const auto firstRelease = windowFillSubBlocks + stepsToReach (PsrGuard::releaseAfterSeconds);   // 79
                auto bad = 0;

                while (c.calls < firstRelease - 1)
                    if (c.feed (9.0).reliefDb != kept && bad == 0)
                        bad = c.calls;

                expectEquals (bad, 0, "legacy node: no release before 5 s of valid readings above");
                expect (c.feed (9.0).reliefDb > kept, "legacy node: first release at sub-block 79");
            }

            // 4. 损坏拒收、对象不动：reliefDb NaN、belowSeconds NaN、aboveSeconds NaN、aboveSeconds 不是十六进制、缺 belowSeconds、标签不对。
            for (int which = 0; which < 6; ++which)
            {
                juce::XmlElement corrupt (*saved);

                switch (which)
                {
                    case 0:  corrupt.setAttribute ("reliefDb", floatNanHex); break;
                    case 1:  corrupt.setAttribute ("belowSeconds", doubleNanHex); break;
                    case 2:  corrupt.setAttribute ("aboveSeconds", doubleNanHex); break;
                    case 3:  corrupt.setAttribute ("aboveSeconds", "not-hex"); break;
                    case 4:  corrupt.removeAttribute ("belowSeconds"); break;
                    default: corrupt.setTagName ("CONTRAST_GUARD"); break;
                }

                PsrFeeder target;

                while (target.calls < 80)
                    target.feed (6.0);

                const auto before = target.guard.createStateXml();
                const auto label = "corrupt PSR node " + juce::String (which);
                expect (! target.guard.restoreStateXml (corrupt), label + " rejected");
                const auto after = target.guard.createStateXml();
                expect (before->isEquivalentTo (after.get(), false), label + " leaves the guard untouched");
            }
        }

        //==============================================================================
        void testContrastHistogramAndRetreat()
        {
            beginTest ("contrast guard: full 3 s windows first, histograms full after 729 measured steps, loss 3 LU over 1.2 for 10 s retreats 0.01 dB per sub-block to the floor");

            // 入口 −20.05 / −10.05、出口 −17.05 / −10.05 各 15 s 交替，loss = 3.0（见 ContrastFeeder 上方说明）：
            //   第 1 ～ 29 个：两侧 3 s 窗不满 → 不进账（无读数，histogramSeconds 0）；第 30 个第一次进账：histogramSeconds = 0·d + 0.1 = 0.1，
            //   此刻只有一档 → spread = (0.90 − 0.25) × 0.1 = 0.065（格内插值）；
            //   第 29 + m 个：histogramSeconds = 0.1·(1 − d^m)/(1 − d)；满 60 s 要 m = 729 → 第 758 个第一次判定（第 757 个 59.894 < 59.95）；
            //   判定前「前提不成立」按低于门槛计（relief 本来就是 0，不动）；
            //   第 758 个起 loss 3.0 > 1.2：overSeconds 第 100 步到 9.95 → 第 758 + 99 = 857 个第一次退 0.1 dB/s × 0.1 s = 0.01 dB；
            //   每步 0.01，300 步（float 可能多一步）退满 → 第 857 + 310 = 1167 个之后必为 −3。
            const auto fullSteps = measuredStepsToFull();                                                       // 729
            const auto firstJudged = windowFillSubBlocks + fullSteps;                                           // 758
            const auto firstRetreat = firstJudged + stepsToReach (ContrastGuard::triggerSeconds) - 1;           // 857

            ContrastFeeder f (3.0);
            auto firstBad = 0;

            for (int k = 1; k <= windowFillSubBlocks; ++k)
            {
                const auto out = f.feed();
                const auto reading = f.reading();

                if ((out.reliefDb != 0.0f || reading.judged || reading.histogramSeconds != 0.0) && firstBad == 0)
                    firstBad = k;
            }

            expectEquals (firstBad, 0, "windows filling: nothing enters the histograms");

            f.feed();
            expectWithinAbsoluteError (f.reading().histogramSeconds, 0.1, 1.0e-12, "sub-block 30: first entry, 0.1 s");
            expectWithinAbsoluteError (f.reading().inputSpreadLu, 0.065f, 1.0e-5f, "one level so far: spread (0.90 - 0.25) x 0.1 LU");

            for (int k = windowFillSubBlocks + 2; k < firstJudged; ++k)
            {
                const auto out = f.feed();
                const auto reading = f.reading();
                const auto measured = k - windowFillSubBlocks;

                if ((out.reliefDb != 0.0f || reading.judged
                     || std::abs (reading.histogramSeconds - histogramSecondsAfter (measured)) > 1.0e-9) && firstBad == 0)
                    firstBad = k;
            }

            expectEquals (firstBad, 0, "histogramSeconds follows 0.1 (1 - d^m) / (1 - d); not judged before 59.95 s");
            expectEquals (f.nextIndex, firstJudged - 1);

            f.feed();
            {
                const auto reading = f.reading();
                expect (reading.judged, "sub-block 758: both histograms full and input spread >= 3 -> judged");
                expectWithinAbsoluteError (reading.histogramSeconds, histogramSecondsAfter (fullSteps), 1.0e-9);
                expectWithinAbsoluteError (reading.lossLu, 3.0f, 1.0e-3f, "loss = output quiet - input quiet = 3.0 LU");
                expectWithinAbsoluteError (reading.inputSpreadLu, 10.0f, 0.1f, "input spread = 10 LU + in-bin remainder");
                expectWithinAbsoluteError (reading.outputSpreadLu, 7.0f, 0.1f, "output spread = 7 LU + the same remainder");
            }

            for (int k = firstJudged + 1; k < firstRetreat; ++k)
            {
                const auto out = f.feed();
                const auto reading = f.reading();

                if ((out.reliefDb != 0.0f || ! reading.judged || std::abs (reading.lossLu - 3.0f) > 1.0e-3f) && firstBad == 0)
                    firstBad = k;
            }

            expectEquals (firstBad, 0, "over the threshold for less than 10 s: no retreat; loss stays 3.0");

            auto out = f.feed();
            expectWithinAbsoluteError (out.reliefDb, -0.01f, 1.0e-6f, "sub-block 857: first retreat 0.1 dB/s x 0.1 s");
            expect (out.engaged && out.cause == GuardCause::contrast, "retreating: engaged, cause contrast");

            auto previous = out.reliefDb;

            for (int j = 1; j <= 290; ++j)
            {
                out = f.feed();

                if ((std::abs (out.reliefDb - previous + 0.01f) > 1.0e-5f || ! out.engaged) && firstBad == 0)
                    firstBad = f.nextIndex;

                previous = out.reliefDb;
            }

            expectEquals (firstBad, 0, "0.01 dB per sub-block while retreating");

            while (f.nextIndex < firstRetreat + 310)
                out = f.feed();

            expectEquals (out.reliefDb, guardFloorDb, "300 (301 with float rounding) steps reach the floor exactly");

            for (int k = 0; k < 200; ++k)
                out = f.feed();

            expectEquals (out.reliefDb, guardFloorDb, "stops at the floor");
            expect (out.engaged && out.cause == GuardCause::contrast, "at the floor with the loss still over: engaged");
        }

        //==============================================================================
        void testContrastSpreadPrecondition()
        {
            beginTest ("contrast guard: input P90 - P25 below 3 LU is never judged and never retreats, even with a large loss");

            // 入口 −14.05 / −12.05 各 15 s 交替（两档差 2 LU，过渡值离两档都超过半格 → 入口 spread = 2 ± 0.1），出口恒 −12.05（spread 0.065）：
            // loss ≈ 1.95 > 1.5，但入口 spread < 3 → 判定前提不成立 → 200 s 内一直不判定、不退。
            ContrastGuard guard;
            guard.prepare (subBlockSeconds);
            guard.setFloorDb (guardFloorDb);
            auto firstBad = 0;

            for (int i = 0; i < 2000; ++i)
            {
                const auto quiet = (i / sectionSubBlocks) % 2 == 0;
                const auto out = guard.process (levelBlock (quiet ? -14.05 : -12.05), levelBlock (-12.05), true, true, false);

                if ((out.reliefDb != 0.0f || guard.getLastReading().judged) && firstBad == 0)
                    firstBad = i + 1;
            }

            expectEquals (firstBad, 0, "never judged, never retreats");

            const auto reading = guard.getLastReading();
            expect (reading.histogramSeconds > ContrastGuard::fullSeconds, "the histograms are full");
            expectWithinAbsoluteError (reading.inputSpreadLu, 2.0f, 0.1f, "input spread ~ 2 LU (< 3)");
            expect (reading.lossLu > ContrastGuard::clubLossLu, "the loss alone would have tripped even the club threshold");
        }

        //==============================================================================
        void testContrastThresholds()
        {
            beginTest ("contrast guard: loss 1.3 retreats under standard / streaming (1.2) but sits in the club band (1.2 .. 1.5); loss 1.7 trips the club");

            expectEquals (ContrastGuard::standardLossLu, 1.2f);
            expectEquals (ContrastGuard::clubLossLu, 1.5f);
            expectEquals (ContrastGuard::releaseMarginLu, 0.3f);

            // 同 testContrastHistogramAndRetreat：超门槛时第 857 个第一次退。流媒体与标准同用 clubDelivery = false。
            // 俱乐部 loss 1.3：1.5 − 0.3 = 1.2 < 1.3 < 1.5 → 回差带：不退也不回（relief 0 保持 0），300 s 不动。
            const auto firstRetreat = windowFillSubBlocks + measuredStepsToFull() + stepsToReach (ContrastGuard::triggerSeconds) - 1;   // 857

            struct Case { double loss; bool club; bool retreats; };

            for (const auto& c : { Case { 1.3, false, true }, Case { 1.3, true, false }, Case { 1.7, true, true } })
            {
                ContrastFeeder f (c.loss, c.club);
                const auto label = juce::String (c.loss, 1) + (c.club ? " club" : " standard");
                auto firstBad = 0;

                while (f.nextIndex < firstRetreat - 1)
                    if (f.feed().reliefDb != 0.0f && firstBad == 0)
                        firstBad = f.nextIndex;

                expectEquals (firstBad, 0, label + ": nothing before sub-block 857");
                expect (f.reading().judged, label + ": judged");
                expectWithinAbsoluteError (f.reading().lossLu, static_cast<float> (c.loss), 1.0e-3f, label + ": loss");

                if (c.retreats)
                {
                    expect (f.feed().reliefDb < 0.0f, label + ": retreats at sub-block 857");
                }
                else
                {
                    while (f.nextIndex < 3000)
                        if (f.feed().reliefDb != 0.0f && firstBad == 0)
                            firstBad = f.nextIndex;

                    expectEquals (firstBad, 0, label + ": inside the band for 300 s: never retreats");
                }
            }
        }

        //==============================================================================
        void testContrastHysteresis()
        {
            beginTest ("contrast guard: loss between threshold - 0.3 and the threshold holds and clears both timers; below threshold - 0.3 for 30 s releases 0.005 dB per sub-block");

            // 都从「新守门跑到图样第 800 个」的存档出发（第 758 个已判定），改 reliefDb = −1 与计时再读进 B，B 接着图样第 800 个喂：
            //   B 的 3 s 窗是空的 → 第 801 ～ 829 个不进账、保持；第 830 个第一次进账（histogramSeconds = 存档值·d + 0.1）。
            //   1. loss 1.0（标准档，带 0.9 ～ 1.2 内），存档里 overSeconds 9.0、underSeconds 29.0：第 830 个两边计时清零（存档写回 0），
            //      relief 逐位保持 −1（再喂 1500 个）；
            //   2. loss 0.5（< 0.9），underSeconds 0：第 829 + 300 = 1129 个第一次回 0.05 dB/s × 0.1 s = 0.005 dB；
            //      underSeconds 29.0：29.0 + 0.1 n ≥ 29.95 → n = 10 → 第 839 个第一次回（读档接续了计时）；
            //      回满 1 dB 要 200 步（float 可能多一步），下面共回 211 步 → 必为 0。
            constexpr int resumeIndex = 800;
            const auto firstMeasured = resumeIndex + ShortTermWindow::subBlocks;   // 830
            const auto d = std::exp (-subBlockSeconds / ContrastGuard::forgetSeconds);

            {
                ContrastGuard::Reading at800;
                auto node = contrastNodeAfter (1.0, resumeIndex, at800);
                expect (at800.judged && std::abs (at800.lossLu - 1.0f) < 1.0e-3f, "loss 1.0 judged at sub-block 800");

                node->setAttribute ("reliefDb", floatMinusOneHex);
                node->setAttribute ("overSeconds", doubleNineHex);
                node->setAttribute ("underSeconds", doubleTwentyNineHex);

                ContrastFeeder b (1.0, false, resumeIndex);
                expect (b.guard.restoreStateXml (*node), "edited node restores");
                expectEquals (b.relief(), -1.0f);

                auto firstBad = 0;

                while (b.nextIndex < firstMeasured - 1)
                    if (b.feed().reliefDb != -1.0f && firstBad == 0)
                        firstBad = b.nextIndex;

                expectEquals (firstBad, 0, "windows refilling after restore: held");

                b.feed();
                const auto reading = b.reading();
                expect (reading.judged && std::abs (reading.lossLu - 1.0f) < 1.0e-3f, "sub-block 830: judged, loss 1.0 inside the band");
                expectWithinAbsoluteError (reading.histogramSeconds, at800.histogramSeconds * d + subBlockSeconds, 1.0e-9,
                                           "the restored histogramSeconds carry on");

                const auto state = b.guard.createStateXml();
                expectEquals (state->getStringAttribute ("overSeconds"), juce::String (doubleZeroHex), "band: over timer cleared");
                expectEquals (state->getStringAttribute ("underSeconds"), juce::String (doubleZeroHex), "band: under timer cleared");

                for (int k = 0; k < 1500; ++k)
                {
                    const auto out = b.feed();

                    if ((out.reliefDb != -1.0f || out.engaged || out.cause != GuardCause::contrast) && firstBad == 0)
                        firstBad = b.nextIndex;
                }

                expectEquals (firstBad, 0, "band: relief held bit for bit for 150 s");
            }

            for (const auto restoredUnder : { 0.0, 29.0 })
            {
                ContrastGuard::Reading at800;
                auto node = contrastNodeAfter (0.5, resumeIndex, at800);
                expect (at800.judged && std::abs (at800.lossLu - 0.5f) < 1.0e-3f, "loss 0.5 judged at sub-block 800");

                node->setAttribute ("reliefDb", floatMinusOneHex);
                node->setAttribute ("underSeconds", restoredUnder == 0.0 ? doubleZeroHex : doubleTwentyNineHex);

                ContrastFeeder b (0.5, false, resumeIndex);
                expect (b.guard.restoreStateXml (*node), "edited node restores");

                const auto firstRelease = firstMeasured - 1 + stepsToReach (ContrastGuard::releaseAfterSeconds, restoredUnder);   // 1129 / 839
                const auto label = "under timer " + juce::String (restoredUnder, 1) + ": ";
                auto firstBad = 0;

                while (b.nextIndex < firstRelease - 1)
                    if (b.feed().reliefDb != -1.0f && firstBad == 0)
                        firstBad = b.nextIndex;

                expectEquals (firstBad, 0, label + "held until 30 s below threshold - 0.3");

                auto out = b.feed();
                expectWithinAbsoluteError (out.reliefDb, -0.995f, 1.0e-5f, label + "first release step 0.05 dB/s x 0.1 s");
                expect (! out.engaged && out.cause == GuardCause::contrast, label + "releasing: not engaged");

                auto previous = out.reliefDb;

                for (int j = 1; j <= 150; ++j)
                {
                    out = b.feed();

                    if (std::abs (out.reliefDb - previous - 0.005f) > 1.0e-5f && firstBad == 0)
                        firstBad = b.nextIndex;

                    previous = out.reliefDb;
                }

                expectEquals (firstBad, 0, label + "0.005 dB per sub-block");

                for (int j = 0; j < 60; ++j)
                    out = b.feed();

                expectEquals (out.reliefDb, 0.0f, label + "back to exactly 0");
                expect (out.cause == GuardCause::none, label + "cause none at 0");
            }
        }

        //==============================================================================
        void testContrastHolds()
        {
            beginTest ("contrast guard: non-music, or one side's 3 s window broken, holds the histograms, the timers and the relief");

            // 主图样（loss 3.0）：第 758 个起超门槛；喂完第 807 个时 overSeconds = 5.0（50 步）。
            const auto firstJudged = windowFillSubBlocks + measuredStepsToFull();   // 758
            const auto pauseStart = firstJudged + 49;                                // 807
            const auto triggerSteps = stepsToReach (ContrastGuard::triggerSeconds);  // 100

            // 1. 第 808 ～ 907 个 musicActive = false（图样照走，窗口照推）：读数（histogramSeconds、loss、spread）逐位不变、relief 0；
            //    恢复后计时接着 5.0：再 50 步 → 第 907 + 50 = 957 个第一次退（计时清零的话要到 1006，接着累加的话更早）。
            //    之后到第 1100 个（退了 144 步，≈ −1.44）再停 100 个：relief 逐位不变；恢复后下一个就接着退（计时早已过 10 s）。
            {
                constexpr int pauseLength = 100;
                const auto firstRetreat = pauseStart + pauseLength + (triggerSteps - 50);   // 957

                ContrastFeeder f (3.0);

                while (f.nextIndex < pauseStart)
                    f.feed();

                const auto frozen = f.reading();
                expect (frozen.judged);
                auto firstBad = 0;

                for (int k = 0; k < pauseLength; ++k)
                {
                    const auto out = f.feed (false);
                    const auto reading = f.reading();

                    if ((out.reliefDb != 0.0f || reading.histogramSeconds != frozen.histogramSeconds || reading.lossLu != frozen.lossLu
                         || reading.inputSpreadLu != frozen.inputSpreadLu || reading.outputSpreadLu != frozen.outputSpreadLu) && firstBad == 0)
                        firstBad = f.nextIndex;
                }

                expectEquals (firstBad, 0, "non-music: histograms and reading frozen bit for bit, no retreat");

                while (f.nextIndex < firstRetreat - 1)
                    if (f.feed().reliefDb != 0.0f && firstBad == 0)
                        firstBad = f.nextIndex;

                expectEquals (firstBad, 0, "music again: the over timer resumes from 5.0 s");
                expect (f.feed().reliefDb < 0.0f, "sub-block 957 retreats");

                while (f.nextIndex < 1100)
                    f.feed();

                const auto held = f.relief();

                for (int k = 0; k < 100; ++k)
                    if (f.feed (false).reliefDb != held && firstBad == 0)
                        firstBad = f.nextIndex;

                expectEquals (firstBad, 0, "non-music holds a retreated relief bit for bit");
                expectWithinAbsoluteError (f.feed().reliefDb, held - 0.01f, 1.0e-5f, "music again: the next sub-block retreats on");
            }

            // 2. 第 808 个的出口子块非有限（链里出过 NaN）→ 出口 3 s 窗断开，入口照常：第 808 ～ 837 个不进账（出口窗第 809 ～ 838 个重攒 30 个），
            //    计时与退让保持；第 838 个起进账，over 接着 5.0 → 再 50 步 → 第 838 + 49 = 887 个第一次退。
            {
                const auto firstRetreat = pauseStart + 1 + ShortTermWindow::subBlocks + (triggerSteps - 50) - 1;   // 887

                ContrastFeeder f (3.0);

                while (f.nextIndex < pauseStart)
                    f.feed();

                const auto frozen = f.reading();
                const auto quiet = (f.nextIndex / sectionSubBlocks) % 2 == 0;
                f.guard.process (levelBlock (quiet ? inputQuietLufs : loudLufs),
                                 makeSubBlock (std::numeric_limits<double>::quiet_NaN(), peakForLevels), true, true, false);
                ++f.nextIndex;

                auto firstBad = 0;

                while (f.nextIndex < pauseStart + ShortTermWindow::subBlocks)   // 第 809 ～ 837 个
                {
                    const auto out = f.feed();

                    if ((out.reliefDb != 0.0f || f.reading().histogramSeconds != frozen.histogramSeconds) && firstBad == 0)
                        firstBad = f.nextIndex;
                }

                expectEquals (firstBad, 0, "output window refilling: histograms frozen, no retreat");

                while (f.nextIndex < firstRetreat - 1)
                    if (f.feed().reliefDb != 0.0f && firstBad == 0)
                        firstBad = f.nextIndex;

                expectEquals (firstBad, 0, "measured again from sub-block 838: the over timer resumes from 5.0 s");
                expect (f.feed().reliefDb < 0.0f, "sub-block 887 retreats");
            }
        }

        //==============================================================================
        void testContrastDisabled()
        {
            beginTest ("contrast guard: switched off for this genre with music - timers clear, relief back to 0 at 0.005 dB per sub-block, histograms keep learning");

            // 主图样到第 900 个：第 857 个起退了 44 步 → relief ≈ −0.44，overSeconds 14.3。
            // 第 901 个起 enabled = false：每个子块 +0.005（第 901 个 −0.435），88 步（float 可能多一步）回 0；
            // 直方图照攒：histogramSeconds 每步 h·d + 0.1、照样判定；存档里两个计时都是 0。
            // 第 1101 个起再打开：over 从 0 计 → 第 1100 + 100 = 1200 个第一次退。
            ContrastFeeder f (3.0);

            while (f.nextIndex < 900)
                f.feed();

            expectWithinAbsoluteError (f.relief(), -0.44f, 1.0e-4f, "44 retreat steps by sub-block 900");

            const auto d = std::exp (-subBlockSeconds / ContrastGuard::forgetSeconds);
            auto out = f.feed (true, false);
            expectWithinAbsoluteError (out.reliefDb, -0.435f, 1.0e-4f, "switched off: releases from the first sub-block");
            expect (! out.engaged, "switched off: not engaged");

            auto firstBad = 0;

            for (int k = 902; k <= 1100; ++k)
            {
                const auto previousSeconds = f.reading().histogramSeconds;
                out = f.feed (true, false);
                const auto reading = f.reading();

                if ((std::abs (reading.histogramSeconds - (previousSeconds * d + subBlockSeconds)) > 1.0e-12 || ! reading.judged) && firstBad == 0)
                    firstBad = k;
            }

            expectEquals (firstBad, 0, "switched off: the histograms keep learning and the reading keeps updating");
            expectEquals (out.reliefDb, 0.0f, "switched off: back to exactly 0");

            const auto state = f.guard.createStateXml();
            expectEquals (state->getStringAttribute ("overSeconds"), juce::String (doubleZeroHex), "switched off: over timer cleared");
            expectEquals (state->getStringAttribute ("underSeconds"), juce::String (doubleZeroHex), "switched off: under timer cleared");

            const auto firstRetreat = 1100 + stepsToReach (ContrastGuard::triggerSeconds);   // 1200

            while (f.nextIndex < firstRetreat - 1)
                if (f.feed().reliefDb != 0.0f && firstBad == 0)
                    firstBad = f.nextIndex;

            expectEquals (firstBad, 0, "switched on again: 10 s counted from 0");
            expect (f.feed().reliefDb < 0.0f, "sub-block 1200 retreats");
        }

        //==============================================================================
        void testContrastPercentiles()
        {
            beginTest ("contrast guard: P25 / P90 interpolate linearly inside the 0.1 LU bins");

            // 1. 单一档：所有权重在一格 → P25 = 格下沿 + 0.25 × 0.1，P90 = 格下沿 + 0.90 × 0.1 → spread = 0.065 LU
            //    （取格中心的算法会给 0）。
            {
                ContrastGuard guard;
                guard.prepare (subBlockSeconds);

                for (int k = 0; k < 200; ++k)
                    guard.process (levelBlock (-14.05), levelBlock (-30.05), true, true, false);

                const auto reading = guard.getLastReading();
                expectWithinAbsoluteError (reading.inputSpreadLu, 0.065f, 1.0e-5f, "one bin: (0.90 - 0.25) x 0.1 LU");
                expectWithinAbsoluteError (reading.outputSpreadLu, 0.065f, 1.0e-5f, "one bin (output)");
            }

            // 2. 两段、两格：入口先 A 进账 300 步，clearWindows() 断开（再攒 29 个不进账、不遗忘），再 B 进账 300 步；出口恒 −14.05。
            //    直方图只有两格：先进的权重 S·d^300（被后 300 步遗忘），后进的 S，S = 0.1 (1 − d^300)/(1 − d)，d^300 = e^(−1/6) = 0.846482。
            //    轻档 −20.05（格 499，下沿 −20.1）、重档 −10.05（格 599，下沿 −10.1），以 S 为单位 wQ、wL：
            //      P25：目标 0.25 (wQ + wL) ≤ wQ → 在轻档格：within25 = 0.25 (wQ + wL) / wQ；
            //      P90：目标 0.9 (wQ + wL) − wQ 落在重档格：within90 = (0.9 (wQ + wL) − wQ) / wL；
            //      spread = 10 + (within90 − within25) / 10。
            //    先轻后重：wQ = d^300、wL = 1 → within25 = 0.545340、within90 = 0.815352 → spread = 10.027001；
            //    先重后轻：wQ = 1、wL = d^300 → within25 = 0.461620、within90 = 0.781864 → spread = 10.032024。
            //    histogramSeconds = 0.1 (1 − d^600)/(1 − d) = 51.0385（< 59.95，不判定，读数照给）。
            //    直方图是 float、遗忘系数 (float) d：300 步的相对误差 ~1e−5 → spread 误差 ~1e−6，容差 1e−4（格中心算法差 0.027 以上）。
            const auto d300 = std::pow (std::exp (-subBlockSeconds / ContrastGuard::forgetSeconds), 300);
            const auto blocksPerSegment = windowFillSubBlocks + 300;   // 29 个攒窗 + 300 个进账

            for (const auto quietFirst : { true, false })
            {
                ContrastGuard guard;
                guard.prepare (subBlockSeconds);
                const auto firstLevel = quietFirst ? inputQuietLufs : loudLufs;
                const auto secondLevel = quietFirst ? loudLufs : inputQuietLufs;

                for (int k = 0; k < blocksPerSegment; ++k)
                    guard.process (levelBlock (firstLevel), levelBlock (-14.05), true, true, false);

                guard.clearWindows();

                for (int k = 0; k < blocksPerSegment; ++k)
                    guard.process (levelBlock (secondLevel), levelBlock (-14.05), true, true, false);

                const auto wQuiet = quietFirst ? d300 : 1.0;
                const auto wLoud = quietFirst ? 1.0 : d300;
                const auto within25 = 0.25 * (wQuiet + wLoud) / wQuiet;
                const auto within90 = (0.9 * (wQuiet + wLoud) - wQuiet) / wLoud;
                const auto expectedSpread = 10.0 + (within90 - within25) / 10.0;

                const auto reading = guard.getLastReading();
                const auto label = juce::String (quietFirst ? "quiet first" : "loud first");
                expectWithinAbsoluteError (static_cast<double> (reading.inputSpreadLu), expectedSpread, 1.0e-4, label + ": two-bin spread, in-bin interpolation");
                expectWithinAbsoluteError (static_cast<double> (reading.inputSpreadLu), quietFirst ? 10.027001 : 10.032024, 1.0e-4, label + ": hand value");
                expectWithinAbsoluteError (reading.histogramSeconds, histogramSecondsAfter (600), 1.0e-9, label + ": 600 measured steps");
                expect (! reading.judged, label + ": 51 s < 59.95 s, not judged");
                expectWithinAbsoluteError (reading.outputSpreadLu, 0.065f, 1.0e-5f, label + ": output one bin");
            }
        }

        //==============================================================================
        void testContrastDeterminism()
        {
            beginTest ("contrast guard: the same sub-blocks give bit-identical relief, readings and state in two new objects");

            ContrastFeeder a (3.0), b (3.0);
            auto firstBad = 0;

            for (int k = 1; k <= 1600; ++k)
            {
                const auto music = k % 97 != 0;
                const auto enabled = ! (k > 1000 && k < 1050);

                if (k == 1300)
                {
                    a.guard.clearWindows();
                    b.guard.clearWindows();
                }

                const auto outA = a.feed (music, enabled);
                const auto outB = b.feed (music, enabled);
                const auto ra = a.reading(), rb = b.reading();

                if ((outA.reliefDb != outB.reliefDb || outA.engaged != outB.engaged || ra.judged != rb.judged || ra.lossLu != rb.lossLu
                     || ra.histogramSeconds != rb.histogramSeconds) && firstBad == 0)
                    firstBad = k;
            }

            expectEquals (firstBad, 0, "two guards fed the same sub-blocks stay bit-identical");
            expect (a.relief() < 0.0f, "the run did retreat");

            const auto stateA = a.guard.createStateXml();
            const auto stateB = b.guard.createStateXml();
            expect (stateA->isEquivalentTo (stateB.get(), false), "and their saved states are identical");
        }

        //==============================================================================
        void testContrastState()
        {
            beginTest ("contrast guard state: bitwise round trip, placeholders for 0.1.8.10, legacy ring node, corrupt nodes rejected");

            // A：主图样到第 900 个（relief ≈ −0.44、overSeconds 14.3、两份直方图满）。
            ContrastFeeder a (3.0);

            while (a.nextIndex < 900)
                a.feed();

            const auto saved = a.guard.createStateXml();
            expect (saved != nullptr && saved->hasTagName ("CONTRAST_GUARD"), "node tag");

            // 1. 往返逐位。读档后窗口空 → 前 29 个不进账、relief 保持；第 30 个（图样第 930 个）起 loss 3.0、
            //    over 14.3 + 0.1 ≥ 9.95 → 立即接着退。A 断开窗口后与 B 状态相同 → 同一串子块逐位同轨。
            ContrastFeeder b (3.0, false, 900);
            expect (b.guard.restoreStateXml (*saved), "restores");
            const auto again = b.guard.createStateXml();
            expect (saved->isEquivalentTo (again.get(), false), "createStateXml after restore is bit for bit the saved node");

            a.guard.clearWindows();
            const auto restoredRelief = b.relief();
            auto firstBad = 0;

            for (int k = 1; k <= 400; ++k)
            {
                const auto outA = a.feed();
                const auto outB = b.feed();

                if ((outA.reliefDb != outB.reliefDb
                     || (k <= windowFillSubBlocks && outB.reliefDb != restoredRelief)
                     || (k == windowFillSubBlocks + 1 && ! (outB.reliefDb < restoredRelief))) && firstBad == 0)
                    firstBad = k;
            }

            expectEquals (firstBad, 0, "restored guard: 29 sub-blocks held, then retreats on along the original's track");
            const auto stateA = a.guard.createStateXml();
            const auto stateB = b.guard.createStateXml();
            expect (stateA->isEquivalentTo (stateB.get(), false), "same state after 400 sub-blocks");

            // 2. 新节点带 0.1.8.10 要的占位：ringCount 0、ringWrite 0、samplesUntilPush 4800、两个 128 个 0.0f 的环（每个 8 位 → 1024 个 '0'）；
            //    两份直方图各 800 × 8 = 6400 位。
            const auto zeroRing = juce::String::repeatedString ("0", 128 * 8);
            expectEquals (saved->getStringAttribute ("ringCount"), juce::String ("0"));
            expectEquals (saved->getStringAttribute ("ringWrite"), juce::String ("0"));
            expectEquals (saved->getStringAttribute ("samplesUntilPush"), juce::String ("4800"));
            expect (saved->getStringAttribute ("inputRing") == zeroRing && saved->getStringAttribute ("outputRing") == zeroRing,
                    "empty legacy rings for 0.1.8.10");
            expectEquals (saved->getStringAttribute ("inputHistogram").length(), glaze::histogramBins * 8);
            expectEquals (saved->getStringAttribute ("outputHistogram").length(), glaze::histogramBins * 8);

            // 3. 旧节点（0.1.8.10 写的：reliefDb / floorDb / engaged + 128 笔环，没有直方图与计时）：读得进，退让照旧（−1），
            //    计时与直方图从空开始（写出的属性与新对象相同、读数 histogramSeconds 0）。之后喂恒定 −14.05（入口 spread 0.065 < 3、
            //    直方图也不满 → 前提不成立 → 按低于门槛计）：under 从 0 起 → 第 29 + 300 = 329 个第一次回 0.005。
            juce::XmlElement legacy ("CONTRAST_GUARD");
            legacy.setAttribute ("reliefDb", floatMinusOneHex);
            legacy.setAttribute ("floorDb", floatMinusThreeHex);
            legacy.setAttribute ("engaged", "1");
            legacy.setAttribute ("ringCount", "40");
            legacy.setAttribute ("ringWrite", "40");
            legacy.setAttribute ("samplesUntilPush", "1200");
            legacy.setAttribute ("inputRing", juce::String::repeatedString (floatMinusFourteenHex, 128));
            legacy.setAttribute ("outputRing", juce::String::repeatedString (floatMinusFourteenHex, 128));

            {
                ContrastFeeder c (3.0), fresh (3.0);
                expect (c.guard.restoreStateXml (legacy), "legacy ring node restores");

                const auto now = c.guard.current();
                expect (now.reliefDb == -1.0f && now.cause == GuardCause::contrast, "legacy node: relief kept");
                expect (c.reading().histogramSeconds == 0.0 && ! c.reading().judged, "legacy node: empty reading");

                const auto cXml = c.guard.createStateXml();
                const auto freshXml = fresh.guard.createStateXml();

                for (const auto* attribute : { "overSeconds", "underSeconds", "histogramSeconds", "inputHistogram", "outputHistogram" })
                    expect (cXml->getStringAttribute (attribute) == freshXml->getStringAttribute (attribute),
                            juce::String ("legacy node: empty ") + attribute);

                expectEquals (cXml->getStringAttribute ("floorDb"), juce::String (floatMinusThreeHex), "legacy node: floor kept");
            }

            {
                ContrastGuard guard;
                guard.prepare (subBlockSeconds);
                guard.setFloorDb (guardFloorDb);
                expect (guard.restoreStateXml (legacy));

                const auto firstRelease = windowFillSubBlocks + stepsToReach (ContrastGuard::releaseAfterSeconds);   // 329
                auto bad = 0;

                for (int k = 1; k < firstRelease; ++k)
                {
                    const auto out = guard.process (levelBlock (-14.05), levelBlock (-14.05), true, true, false);

                    if ((out.reliefDb != -1.0f || guard.getLastReading().judged) && bad == 0)
                        bad = k;
                }

                expectEquals (bad, 0, "legacy node: not judged, relief held until 30 s of measured music");
                expectWithinAbsoluteError (guard.process (levelBlock (-14.05), levelBlock (-14.05), true, true, false).reliefDb, -0.995f, 1.0e-5f,
                                           "legacy node: first release at sub-block 329 (precondition not met counts as below)");
            }

            // 3b. 旧节点 + 曲风关着：第 1 个子块（窗还没满、不进账）就开始回。
            {
                ContrastGuard guard;
                guard.prepare (subBlockSeconds);
                guard.setFloorDb (guardFloorDb);
                expect (guard.restoreStateXml (legacy));
                expectWithinAbsoluteError (guard.process (levelBlock (-14.05), levelBlock (-14.05), true, false, false).reliefDb, -0.995f, 1.0e-5f,
                                           "switched off: releases even before the windows fill");
                expectEquals (guard.getLastReading().histogramSeconds, 0.0, "nothing measured yet");
            }

            // 4. 损坏拒收、对象不动：直方图里有负数 / NaN、histogramSeconds NaN、overSeconds 为负、直方图少一格、标签不对；
            //    旧节点里 reliefDb 是 NaN 也拒收。
            for (int which = 0; which < 7; ++which)
            {
                juce::XmlElement corrupt (which == 6 ? legacy : *saved);

                switch (which)
                {
                    case 0:  replaceArrayItem (corrupt, "inputHistogram", 0, floatMinusOneHex); break;
                    case 1:  replaceArrayItem (corrupt, "outputHistogram", 5, floatNanHex); break;
                    case 2:  corrupt.setAttribute ("histogramSeconds", doubleNanHex); break;
                    case 3:  corrupt.setAttribute ("overSeconds", doubleMinusOneHex); break;
                    case 4:  corrupt.setAttribute ("inputHistogram", saved->getStringAttribute ("inputHistogram").substring (8)); break;
                    case 5:  corrupt.setTagName ("PSR_GUARD"); break;
                    default: corrupt.setAttribute ("reliefDb", floatNanHex); break;
                }

                ContrastFeeder target (3.0);

                while (target.nextIndex < 100)
                    target.feed();

                const auto before = target.guard.createStateXml();
                const auto label = "corrupt contrast node " + juce::String (which);
                expect (! target.guard.restoreStateXml (corrupt), label + " rejected");
                const auto after = target.guard.createStateXml();
                expect (before->isEquivalentTo (after.get(), false), label + " leaves the guard untouched");
            }
        }

        //==============================================================================
        void testGuardsDoNotAllocate()
        {
            beginTest ("guards: process, current, readings, clear / snap / floor and window resets do not allocate");

            PsrGuard psrGuard;
            ContrastGuard contrastGuard;
            psrGuard.prepare (subBlockSeconds);
            contrastGuard.prepare (subBlockSeconds);

            auto sink = 0.0f;
            auto psrDb = 0.0f;
            const auto before = allocationCountOnThisThread();

            for (int k = 0; k < 3000; ++k)
            {
                const auto quiet = (k / sectionSubBlocks) % 2 == 0;
                const auto music = k % 53 != 0;
                const auto enabled = k % 701 > 30;
                sink += contrastGuard.process (levelBlock (quiet ? inputQuietLufs : loudLufs),
                                               levelBlock (quiet ? inputQuietLufs + 3.0 : loudLufs), music, enabled, k % 2 == 0).reliefDb;
                sink += psrGuard.process (psrBlock (k % 400 < 200 ? 6.0 : 9.0), music, enabled, glaze::deliveryFromIndex (k % 3)).reliefDb;
                sink += contrastGuard.current().reliefDb + psrGuard.current().reliefDb + contrastGuard.getLastReading().lossLu;

                if (psrGuard.getLastPsrDb (psrDb))
                    sink += psrDb;

                if (k % 997 == 0)
                {
                    contrastGuard.clearWindows();
                    psrGuard.clearWindow();
                }

                if (k % 1499 == 0)
                {
                    contrastGuard.snapRelief();
                    psrGuard.snapRelief();
                }

                if (k % 1201 == 0)
                {
                    contrastGuard.setFloorDb (-2.0f);
                    psrGuard.setFloorDb (-2.5f);
                }
            }

            contrastGuard.clear();
            psrGuard.clear();

            // 先把计数读进局部量再断言：实参求值顺序不定，提示文字的 juce::String 可能先构造、被算进计数。
            const auto allocations = static_cast<juce::int64> (allocationCountOnThisThread() - before);
            expectEquals (allocations, static_cast<juce::int64> (0), "no operator new on the guards' audio path");
            expect (std::isfinite (sink));
        }

        //==============================================================================
        void testGuardSwitchConstants()
        {
            beginTest ("constants: guard switches - factory House {contrast on, PSR off}, general both off; per-genre lookup; snapshot fields; legacy snapshot; bitwise");

            const auto& factory = glaze::factoryConstants();
            expect (factory.house.guards.contrast && ! factory.house.guards.psr, "factory House: contrast guard on, PSR guard off");
            expect (! factory.generalGuards.contrast && ! factory.generalGuards.psr, "factory general: both off");

            // 查询：通用 → generalGuards，House 三类 → house.guards（同一份）；跟着快照走，不是写死的出厂值。
            expect (&glaze::guardSwitchesFor (factory, glaze::Genre::general) == &factory.generalGuards, "general uses generalGuards");

            for (const auto genre : { glaze::Genre::deepHouse, glaze::Genre::techHouse, glaze::Genre::melodicProgressive })
                expect (&glaze::guardSwitchesFor (factory, genre) == &factory.house.guards,
                        "House genre " + juce::String (static_cast<int> (genre)) + " uses house.guards");

            auto custom = factory;
            custom.house.guards = { false, true };
            custom.generalGuards = { true, false };
            expect (! glaze::guardSwitchesFor (custom, glaze::Genre::techHouse).contrast && glaze::guardSwitchesFor (custom, glaze::Genre::techHouse).psr,
                    "the lookup follows the snapshot (House)");
            expect (glaze::guardSwitchesFor (custom, glaze::Genre::general).contrast && ! glaze::guardSwitchesFor (custom, glaze::Genre::general).psr,
                    "the lookup follows the snapshot (general)");

            // 快照字段：四个布尔属性（写 1 / 0）。
            const auto xml = glaze::toXml (factory);
            const char* const switchAttributes[] = { "house_contrastGuardOn", "house_psrGuardOn", "general_contrastGuardOn", "general_psrGuardOn" };

            for (const auto* attribute : switchAttributes)
                expect (xml->hasAttribute (attribute), juce::String ("snapshot has ") + attribute);

            expectEquals (xml->getStringAttribute ("house_contrastGuardOn"), juce::String ("1"));
            expectEquals (xml->getStringAttribute ("house_psrGuardOn"), juce::String ("0"));
            expectEquals (xml->getStringAttribute ("general_contrastGuardOn"), juce::String ("0"));
            expectEquals (xml->getStringAttribute ("general_psrGuardOn"), juce::String ("0"));

            // T-097 之前的快照没有这四项：读成出厂值（先把 out 弄乱，确认不是沿用 out 里原来的值）。
            {
                juce::XmlElement legacy (*xml);

                for (const auto* attribute : switchAttributes)
                    legacy.removeAttribute (attribute);

                auto out = factory;
                out.house.guards = { false, true };
                out.generalGuards = { true, true };
                expect (glaze::fromXml (legacy, out), "legacy snapshot without guard switches reads");
                expect (glaze::bitwiseEqual (out, factory), "missing guard switches take the factory values");
                expect (out.house.guards.contrast && ! out.house.guards.psr && ! out.generalGuards.contrast && ! out.generalGuards.psr);
            }

            // 改任一个开关 → bitwiseEqual 判不同；存读往返保住改过的值。
            for (int which = 0; which < 4; ++which)
            {
                auto changed = factory;

                switch (which)
                {
                    case 0:  changed.house.guards.contrast = false; break;
                    case 1:  changed.house.guards.psr = true; break;
                    case 2:  changed.generalGuards.contrast = true; break;
                    default: changed.generalGuards.psr = true; break;
                }

                const auto label = juce::String (switchAttributes[which]);
                expect (! glaze::bitwiseEqual (changed, factory), label + ": a changed switch is a different snapshot");

                auto back = factory;
                expect (glaze::fromXml (*glaze::toXml (changed), back) && glaze::bitwiseEqual (back, changed), label + ": round trip keeps it");
            }

            // 存在但损坏照样拒收，out 不变。
            {
                juce::XmlElement broken (*xml);
                broken.setAttribute ("general_psrGuardOn", "2");
                auto untouched = custom;
                expect (! glaze::fromXml (broken, untouched) && glaze::bitwiseEqual (untouched, custom), "a broken guard switch is rejected, out untouched");
            }
        }
    };

    static GuardFamilyTests guardFamilyTests;
}

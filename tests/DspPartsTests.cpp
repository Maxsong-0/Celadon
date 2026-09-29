/*
  ==============================================================================

    DspPartsTests.cpp
    Saturator、TruePeakDetector、LoudnessMeter 的单元测试。

    v0.5 起真峰值检测的实时路径是按块的向量核（processBlock / processBlockMax），逐样本的 processSample
    留作参照实现。这里逐位对照两者（多种信号 × 块长 × 交替调用 × 非规格化数模式：x86 上 FTZ / DAZ 四种组合），另加一条性能护栏，
    以及响度表「只算真峰值」模式的测试。

  ==============================================================================
*/

#include "TestHelpers.h"

#include "../Source/dsp/LoudnessMeter.h"
#include "../Source/dsp/Saturator.h"

#include <algorithm>
#include <cfenv>
#include <cstring>
#include <limits>
#include <memory>

#if defined (__SSE2__) || defined (_M_X64) || defined (_M_AMD64)
 #include <xmmintrin.h>   // _mm_getcsr / _mm_setcsr：FTZ、DAZ 分开开关
#endif

namespace celadon::test
{
    namespace
    {
        bool sameBits (float a, float b) noexcept
        {
            return std::memcmp (&a, &b, sizeof (float)) == 0;
        }

        struct NamedSignal
        {
            const char* name;
            Signal signal;
        };

        // 真峰值检测器的对照素材：两声道内容不同（声道状态必须互不相干），覆盖满幅、极大、极小、非规格化与非有限值。
        std::vector<NamedSignal> makeDetectorTestSignals()
        {
            constexpr double sampleRate = 48000.0;
            constexpr int length = 24000;
            std::vector<NamedSignal> signals;

            signals.push_back ({ "white noise",           makeNoise (2, length, 1.0f, 101) });
            signals.push_back ({ "full-scale square",     makeSquare (2, sampleRate, 3000.0, 1.0f, 0.5) });
            signals.push_back ({ "20 ms burst",           makeBurst (2, sampleRate, 1.0f, 0.5) });
            signals.push_back ({ "+60 dBFS noise",        makeNoise (2, length, 1000.0f, 102) });
            signals.push_back ({ "near-denormal noise",   makeNoise (2, length, 1.0e-38f, 103) });
            signals.push_back ({ "fs/4 sine at 45 deg",   makeSine (2, sampleRate, sampleRate / 4.0, 1.0f, 0.5, juce::MathConstants<double>::pi / 4.0) });
            signals.push_back ({ "0.45 fs sine",          makeSine (2, sampleRate, 0.45 * sampleRate, 0.9f, 0.5) });

            // 最小的非规格化数：±k · 2⁻¹⁴⁹。
            {
                auto tiny = makeSilence (2, length);
                juce::Random random (104);
                for (auto& channel : tiny)
                    for (auto& x : channel)
                        x = std::numeric_limits<float>::denorm_min() * (float) (random.nextInt (7) - 3);
                signals.push_back ({ "smallest denormals", std::move (tiny) });
            }

            // 极端值混在小噪声里：±1e20、±FLT_MAX、±0、FLT_MIN、非规格化、NaN、±Inf。
            {
                const float pool[] = { 1.0e20f, -1.0e20f, std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(),
                                       0.0f, -0.0f, std::numeric_limits<float>::min(), 1.0e-40f, -1.0e-45f,
                                       std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                                       -std::numeric_limits<float>::infinity() };
                auto extremes = makeNoise (2, length, 0.1f, 105);
                juce::Random random (106);

                for (auto& channel : extremes)
                    for (auto& x : channel)
                        if (random.nextInt (50) == 0)
                            x = pool[random.nextInt ((int) (sizeof (pool) / sizeof (pool[0])))];

                signals.push_back ({ "extremes incl. NaN / Inf", std::move (extremes) });
            }

            return signals;
        }

        // 参照：每个样本都走 processSample。
        Signal referencePeaksOf (const Signal& signal)
        {
            TruePeakDetector detector;
            detector.prepare ((int) signal.size());
            auto peaks = signal;

            for (size_t ch = 0; ch < signal.size(); ++ch)
                for (size_t i = 0; i < signal[ch].size(); ++i)
                    peaks[ch][i] = detector.processSample ((int) ch, signal[ch][i]);

            return peaks;
        }

        float jmaxFold (const std::vector<float>& peaks, int start, int length) noexcept
        {
            float peak = 0.0f;

            for (int i = start; i < start + length; ++i)
                peak = juce::jmax (peak, peaks[(size_t) i]);

            return peak;
        }

        // 逐位对照时的非规格化数模式。x86 的 MXCSR 里 FTZ（结果冲零）与 DAZ（输入当零）是两个独立的位，宿主可能只开其中一个，
        // juce::ScopedNoDenormals 两个都开——四种组合都比（BUG-001 只在 DAZ 开着时出现）。ARM 只有一个 FZ 位
        // （ScopedNoDenormals 开的就是它），比两种。
       #if defined (__SSE2__) || defined (_M_X64) || defined (_M_AMD64)
        constexpr unsigned int mxcsrFlushToZero      = 0x8000u;
        constexpr unsigned int mxcsrDenormalsAreZero = 0x0040u;

        struct DenormalMode
        {
            const char* name;
            unsigned int mxcsrBits;
        };

        constexpr DenormalMode denormalModes[] { { "IEEE denormals", 0u },
                                                 { "FTZ only", mxcsrFlushToZero },
                                                 { "DAZ only", mxcsrDenormalsAreZero },
                                                 { "FTZ + DAZ (ScopedNoDenormals)", mxcsrFlushToZero | mxcsrDenormalsAreZero } };

        class ScopedDenormalMode
        {
        public:
            explicit ScopedDenormalMode (const DenormalMode& mode) noexcept  : saved (_mm_getcsr())
            {
                _mm_setcsr ((saved & ~(mxcsrFlushToZero | mxcsrDenormalsAreZero)) | mode.mxcsrBits);
            }

            ~ScopedDenormalMode() noexcept   { _mm_setcsr (saved); }

        private:
            const unsigned int saved;

            JUCE_DECLARE_NON_COPYABLE (ScopedDenormalMode)
        };
       #else
        struct DenormalMode
        {
            const char* name;
            bool flush;
        };

        constexpr DenormalMode denormalModes[] { { "IEEE denormals", false }, { "FTZ (ScopedNoDenormals)", true } };

        class ScopedDenormalMode
        {
        public:
            explicit ScopedDenormalMode (const DenormalMode& mode)
            {
                if (mode.flush)
                    noDenormals = std::make_unique<juce::ScopedNoDenormals>();
            }

        private:
            std::unique_ptr<juce::ScopedNoDenormals> noDenormals;

            JUCE_DECLARE_NON_COPYABLE (ScopedDenormalMode)
        };
       #endif
    }

    class SaturatorTests  : public juce::UnitTest
    {
    public:
        SaturatorTests() : juce::UnitTest ("Saturator", "Celadon") {}

        void runTest() override
        {
            beginTest ("SoftClip: linear below knee, never above threshold, monotonic");
            {
                Saturator s;
                s.setCurve (Saturator::Curve::SoftClip);
                s.setThreshold (0.5f);

                float previous = -1.0f;

                for (int i = -20000; i <= 20000; ++i)
                {
                    const auto x = (float) i * 0.001f;   // −20 ～ +20
                    const auto y = s.processSample (x);

                    if (std::abs (x) <= 0.35f)
                        expectEquals (y, x);

                    expect (std::abs (y) <= 0.5f);
                    // 单调：饱和区里相邻两点的真值差不到 1 ulp，平台的 tanhf 只保证「忠实舍入」（glibc 恰好单调，
                    // MSVC UCRT / Apple libm 可能退 1 ulp），所以允许 1 ulp 的回落。
                    expect (y >= previous || previous - y <= std::numeric_limits<float>::epsilon() * std::abs (previous),
                            "SoftClip must be monotonic (1 ulp tolerance) at x = " + juce::String (x, 3));
                    previous = y;
                }
            }

            beginTest ("Tube: y(0) = 0 and unit slope at origin");
            {
                Saturator s;
                s.setCurve (Saturator::Curve::Tube);

                for (auto drive : { 1.0f, 5.5f, 10.0f })
                    for (auto asymmetry : { 0.0f, 0.075f, 0.15f })
                    {
                        s.setDrive (drive);
                        s.setAsymmetry (asymmetry);
                        expectWithinAbsoluteError (s.processSample (0.0f), 0.0f, 1.0e-7f);
                        expectWithinAbsoluteError (s.processSample (1.0e-4f) / 1.0e-4f, 1.0f, 2.0e-3f);
                    }
            }
        }
    };

    class TruePeakTests  : public juce::UnitTest
    {
    public:
        TruePeakTests() : juce::UnitTest ("TruePeakDetector", "Celadon") {}

        void runTest() override
        {
            beginTest ("phase 0 reproduces the sample exactly (latency = tapsPerPhase / 2)");
            {
                TruePeakDetector detector;
                detector.prepare (1);

                for (int i = 0; i < 2 * TruePeakDetector::tapsPerPhase; ++i)
                {
                    const auto peak = detector.processSample (0, i == 0 ? 0.75f : 0.0f);

                    if (i == TruePeakDetector::latencySamples)
                        expect (peak >= 0.75f);
                }
            }

            beginTest ("never below the sample peak");
            {
                const auto noise = makeNoise (1, 20000, 1.0f, 11);
                float samplePeak = 0.0f;
                for (auto x : noise[0])
                    samplePeak = juce::jmax (samplePeak, std::abs (x));

                expect (truePeakOf (noise) >= samplePeak);
            }

            // 只看稳态：信号起止的阶跃本身就有真实的带限过冲（理想重建约 +1.1 dB），不算检测误差。
            beginTest ("fs/4 sine at 45 deg reads +3 dB over its sample peak (steady state)");
            {
                for (auto sampleRate : { 44100.0, 48000.0 })
                {
                    const auto sine = makeSine (1, sampleRate, sampleRate / 4.0, 1.0f, 0.2, juce::MathConstants<double>::pi / 4.0);
                    // 只断言最大值：零交叉那一格的最大值本来就是采样点 0.707（−3 dB）。
                    expectWithinAbsoluteError (toDb (steadyTruePeakRange (sine).second), 0.0f, 0.05f);
                }
            }

            beginTest ("DC reads its own level (steady state)");
            {
                auto dc = makeSilence (1, 2000);
                std::fill (dc[0].begin(), dc[0].end(), 0.5f);
                const auto range = steadyTruePeakRange (dc);
                expectWithinAbsoluteError (range.first, 0.5f, 1.0e-5f);
                expectWithinAbsoluteError (range.second, 0.5f, 1.0e-5f);
            }

            beginTest ("a step really overshoots (known band-limited behaviour, logged)");
            {
                auto step = makeSilence (1, 2000);
                std::fill (step[0].begin() + 1000, step[0].end(), 0.5f);
                const auto overshootDb = toDb (truePeakOf (step)) - toDb (0.5);
                logMessage ("  step overshoot: +" + juce::String (overshootDb, 3) + " dB");
                expect (overshootDb > 0.5f && overshootDb < 1.5f);
            }

            testBlockMatchesSampleBitExactly();
            testInterleavedCallsBitExact();
            testPartialGroupPaddingIsZeroed();
            testBlockCallsDoNotAllocate();
            testPerformanceGuard();
        }

    private:
        // v0.5：按块（向量核）与逐样本（参照）逐位相同。块长 1 / 7 / 64 / 512 / 4096、跨段边界（内部每段 240）、
        // 随机块长；非规格化数按 IEEE 算与各种冲零模式都比（x86：FTZ、DAZ 各自单开与同开；ARM：FZ，见 denormalModes）。
        void testBlockMatchesSampleBitExactly()
        {
            beginTest ("processBlock / processBlockMax are bit-exact with processSample (" + juce::String (TruePeakDetector::getBlockKernelName()) + " kernel)");

            const auto signals = makeDetectorTestSignals();
            const std::vector<std::vector<int>> patterns { { 1 }, { 7 }, { 64 }, { 512 }, { 4096 },
                                                           { 239, 240, 241, 1, 11, 12, 13 },
                                                           { 0 } };   // 0 = 随机块长 1 ～ 1200

            for (const auto& mode : denormalModes)
            {
                const ScopedDenormalMode scopedMode (mode);

                for (const auto& named : signals)
                {
                    const auto reference = referencePeaksOf (named.signal);
                    const auto length = (int) named.signal.front().size();

                    for (size_t p = 0; p < patterns.size(); ++p)
                    {
                        TruePeakDetector blockDetector, maxDetector;
                        blockDetector.prepare (2);
                        maxDetector.prepare (2);
                        std::vector<float> peaks ((size_t) length);
                        juce::Random random (200 + (int) p);
                        int blockMismatches = 0, maxMismatches = 0;
                        size_t patternIndex = 0;

                        for (int start = 0; start < length;)
                        {
                            auto blockLength = patterns[p][patternIndex++ % patterns[p].size()];
                            if (blockLength == 0)
                                blockLength = 1 + random.nextInt (1200);
                            blockLength = juce::jmin (blockLength, length - start);

                            for (int ch = 0; ch < 2; ++ch)
                            {
                                const auto* in = named.signal[(size_t) ch].data() + start;
                                const auto& expected = reference[(size_t) ch];

                                blockDetector.processBlock (ch, in, peaks.data(), blockLength);
                                for (int i = 0; i < blockLength; ++i)
                                    blockMismatches += sameBits (peaks[(size_t) i], expected[(size_t) (start + i)]) ? 0 : 1;

                                maxMismatches += sameBits (maxDetector.processBlockMax (ch, in, blockLength),
                                                           jmaxFold (expected, start, blockLength)) ? 0 : 1;
                            }

                            start += blockLength;
                        }

                        const auto caseName = juce::String (named.name) + ", pattern " + juce::String ((int) p) + ", " + mode.name;
                        expectEquals (blockMismatches, 0, "processBlock differs from processSample: " + caseName);
                        expectEquals (maxMismatches, 0, "processBlockMax differs from the jmax fold: " + caseName);
                    }
                }
            }
        }

        // 同一个检测器上混着调：逐样本、按块、按块取最大、原地按块，结果与只用 processSample 逐位相同。
        void testInterleavedCallsBitExact()
        {
            beginTest ("processSample / processBlock / processBlockMax / in-place calls can be mixed on one detector");

            for (const auto& named : makeDetectorTestSignals())
            {
                const auto reference = referencePeaksOf (named.signal);
                const auto length = (int) named.signal.front().size();

                TruePeakDetector detector;
                detector.prepare (2);
                std::vector<float> scratch ((size_t) length);
                juce::Random random (300);
                int mismatches = 0;

                for (int start = 0; start < length;)
                {
                    const auto blockLength = juce::jmin (length - start, 1 + random.nextInt (700));
                    const auto mode = random.nextInt (4);

                    for (int ch = 0; ch < 2; ++ch)
                    {
                        const auto* in = named.signal[(size_t) ch].data() + start;
                        const auto& expected = reference[(size_t) ch];

                        if (mode == 0)
                        {
                            for (int i = 0; i < blockLength; ++i)
                                mismatches += sameBits (detector.processSample (ch, in[i]), expected[(size_t) (start + i)]) ? 0 : 1;
                        }
                        else if (mode == 1)
                        {
                            detector.processBlock (ch, in, scratch.data(), blockLength);
                            for (int i = 0; i < blockLength; ++i)
                                mismatches += sameBits (scratch[(size_t) i], expected[(size_t) (start + i)]) ? 0 : 1;
                        }
                        else if (mode == 2)
                        {
                            std::copy (in, in + blockLength, scratch.begin());
                            detector.processBlock (ch, scratch.data(), scratch.data(), blockLength);   // 原地
                            for (int i = 0; i < blockLength; ++i)
                                mismatches += sameBits (scratch[(size_t) i], expected[(size_t) (start + i)]) ? 0 : 1;
                        }
                        else
                        {
                            mismatches += sameBits (detector.processBlockMax (ch, in, blockLength),
                                                    jmaxFold (expected, start, blockLength)) ? 0 : 1;
                        }
                    }

                    start += blockLength;
                }

                expectEquals (mismatches, 0, named.name);
            }
        }

        // 凑整组的补位区必须清零。最后一组不满时向量核仍按整组算，多出来的几个输出直接丢掉，
        // 但它们的窗口伸进了本段输入之后的补位区：清零了，这几个输出只碰 0；不清零，碰到的是上一次调用
        // （可能是别的声道）留在工作区里的样本——残留的 Inf / NaN / 非规格化数会让核白做特殊值运算
        // （x86 没开 DAZ 时非规格化数慢几十倍）并置起浮点异常标志。丢掉的输出不影响结果，逐位对照看不出区别
        // （v0.5 变异 M7「去掉清零」逐位测试全过），所以这里看「无效运算」标志：上一次调用在补位区的位置
        // 放 +Inf，下一次只送 1 ～ 11 个有限样本，这一次调用期间不应置起这个标志。补位区里有两个以上 +Inf 时
        // （送 1 ～ 10 个样本），Inf 乘正负相间的抽头再相加 = Inf − Inf，不清零必置 FE_INVALID
        // （M7 实测 22 次调用里 20 次置起；只送 11 个时补位区只剩 1 个 Inf，不产生 Inf − Inf）。
        void testPartialGroupPaddingIsZeroed()
        {
            beginTest ("a partial last group is padded with zeros, never with samples left over from an earlier call");

            TruePeakDetector detector;
            detector.prepare (2);

            // 声道 0 每次先送 12 个样本（一整组），第 1 … 11 个是 +Inf：下一次调用的补位区正好是这 11 个位置。
            std::array<float, 12> poisoned {};
            poisoned.fill (std::numeric_limits<float>::infinity());
            poisoned[0] = 0.25f;

            // 声道 1 只送有限样本：它自己的历史里没有 Inf，只有补位区可能带进 Inf。
            const std::array<float, 11> clean { 0.5f, -0.25f, 0.125f, 0.75f, -0.5f, 0.0f, 0.3f, -0.9f, 0.6f, -0.1f, 0.2f };
            std::array<float, 12> peaks {};
            int flaggedCalls = 0, calls = 0;

            for (int length = 1; length <= (int) clean.size(); ++length)
            {
                for (int useBlockMax = 0; useBlockMax < 2; ++useBlockMax)
                {
                    detector.processBlock (0, poisoned.data(), peaks.data(), (int) poisoned.size());
                    std::feclearexcept (FE_ALL_EXCEPT);

                    if (useBlockMax == 0)
                        detector.processBlock (1, clean.data(), peaks.data(), length);
                    else
                        juce::ignoreUnused (detector.processBlockMax (1, clean.data(), length));

                    flaggedCalls += std::fetestexcept (FE_INVALID) != 0 ? 1 : 0;
                    ++calls;
                }
            }

            std::feclearexcept (FE_ALL_EXCEPT);
            expectEquals (calls, 22);
            expectEquals (flaggedCalls, 0, "the block kernel computed on stale samples (padding not zeroed)");
        }

        // 实时路径不分配内存（计数见 TestMain.cpp）；prepare 之后第一次调用就不分配。
        void testBlockCallsDoNotAllocate()
        {
            beginTest ("processBlock / processBlockMax do not allocate");

            const auto input = makeNoise (2, 5000, 0.8f, 7);
            std::vector<float> peaks (5000);
            TruePeakDetector detector;
            detector.prepare (2);

            const auto before = allocationCountOnThisThread();

            for (int ch = 0; ch < 2; ++ch)
            {
                detector.processBlock (ch, input[(size_t) ch].data(), peaks.data(), 5000);
                juce::ignoreUnused (detector.processBlockMax (ch, input[(size_t) ch].data(), 4097));
                detector.processBlock (ch, input[(size_t) ch].data(), peaks.data(), 1);
            }

            // 先把计数读进局部量再断言：函数实参的求值顺序不确定（GCC 从右往左），提示文字的 juce::String 会先构造、被算进计数。

            const auto allocationsInPath = allocationCountOnThisThread() - before;

            expectEquals ((int) (allocationsInPath), 0);
        }

        // 性能护栏：防止以后退化回逐样本串行乘加（v0.4 约 190 ～ 260 ns，v0.5 NEON 约 9 ～ 13 ns）。
        // 立体声、块 512、15 遍取最短（别的进程抢 CPU 只会让某一遍变慢）；门槛留 2 倍以上余量，防误报。
        // SSE2 没有融合乘加、乘和加各占一个发射口，同样的逐位写法约慢 3 倍，门槛单独放宽；纯标量平台只记录。
        void testPerformanceGuard()
        {
            const juce::String kernel (TruePeakDetector::getBlockKernelName());
            beginTest ("performance guard: " + kernel + " block detector per sample per channel");

            constexpr int length = 48000;
            const auto input = makeNoise (2, length, 0.9f, 5);
            std::vector<float> peaks ((size_t) length);
            TruePeakDetector detector;
            detector.prepare (2);

            double bestNs = std::numeric_limits<double>::max();

            for (int pass = 0; pass < 15; ++pass)
            {
                const auto start = juce::Time::getHighResolutionTicks();

                for (int ch = 0; ch < 2; ++ch)
                    for (int s = 0; s < length; s += 512)
                        detector.processBlock (ch, input[(size_t) ch].data() + s, peaks.data() + s, juce::jmin (512, length - s));

                const auto elapsed = juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - start);
                bestNs = juce::jmin (bestNs, elapsed * 1.0e9 / (2.0 * length));
            }

            logMessage ("  " + kernel + " processBlock: " + juce::String (bestNs, 2) + " ns per sample per channel (best of 15)");

           #if JUCE_DEBUG
            // 调试构建（Visual Studio 的 Debug、CMake Debug）不优化：绝对时间没有意义（与 ColorTests 的性能检查一样）。
            logMessage ("  (debug build: performance limits not asserted)");
           #else
            if (kernel == "NEON")
                expect (bestNs <= 30.0, "NEON block detector too slow: " + juce::String (bestNs, 2) + " ns");
            else if (kernel == "SSE2")
                expect (bestNs <= 80.0, "SSE2 block detector too slow: " + juce::String (bestNs, 2) + " ns");
            else
                logMessage ("  no SIMD kernel on this platform: guard is informational only");
           #endif
        }
    };

    class LoudnessMeterTests  : public juce::UnitTest
    {
    public:
        LoudnessMeterTests() : juce::UnitTest ("LoudnessMeter", "Celadon") {}

        void runTest() override
        {
            // 双声道 1 kHz 正弦 A dBFS → A LUFS（BS.1770：单声道 0 dBFS 1 kHz 正弦 = −3.01 LKFS）。
            beginTest ("stereo 1 kHz sine at -20 dBFS reads -20 LUFS");
            for (auto sampleRate : { 44100.0, 48000.0, 96000.0 })
            {
                LoudnessMeter meter;
                meter.prepare ({ sampleRate, 512, 2 });
                feed (meter, makeSine (2, sampleRate, 1000.0, juce::Decibels::decibelsToGain (-20.0f), 5.0));

                expectWithinAbsoluteError (meter.getShortTermLufs(), -20.0f, 0.05f);
                expectWithinAbsoluteError (meter.getIntegratedLufs(), -20.0f, 0.05f);
                expectWithinAbsoluteError (meter.getAndResetTruePeakDb(), -20.0f, 0.05f);
            }

            // EBU Tech 3341 思路：−36 / −23 / −36，相对门把两段 −36 排除 → 整体 −23。
            beginTest ("relative gate excludes quiet passages");
            {
                constexpr double sampleRate = 48000.0;
                LoudnessMeter meter;
                meter.prepare ({ sampleRate, 512, 2 });

                feed (meter, makeSine (2, sampleRate, 1000.0, juce::Decibels::decibelsToGain (-36.0f), 10.0));
                feed (meter, makeSine (2, sampleRate, 1000.0, juce::Decibels::decibelsToGain (-23.0f), 60.0));
                feed (meter, makeSine (2, sampleRate, 1000.0, juce::Decibels::decibelsToGain (-36.0f), 10.0));

                expectWithinAbsoluteError (meter.getIntegratedLufs(), -23.0f, 0.1f);
            }

            // 单声道按「左右相同的双声道」计量（设计规格-总体）：读数与双声道一致。
            beginTest ("mono reads like identical stereo");
            {
                constexpr double sampleRate = 48000.0;
                LoudnessMeter meter;
                meter.prepare ({ sampleRate, 512, 1 });
                feed (meter, makeSine (1, sampleRate, 1000.0, juce::Decibels::decibelsToGain (-20.0f), 5.0));
                expectWithinAbsoluteError (meter.getIntegratedLufs(), -20.0f, 0.05f);
            }

            beginTest ("absolute gate: silence has no reading; reset clears");
            {
                constexpr double sampleRate = 48000.0;
                LoudnessMeter meter;
                meter.prepare ({ sampleRate, 512, 2 });

                feed (meter, makeSilence (2, 96000));
                expectEquals (meter.getIntegratedLufs(), LoudnessMeter::noReading);

                feed (meter, makeSine (2, sampleRate, 1000.0, 0.1f, 2.0));
                expect (meter.getIntegratedLufs() > LoudnessMeter::noReading);

                meter.requestReset();
                feed (meter, makeSilence (2, 512));
                expectEquals (meter.getIntegratedLufs(), LoudnessMeter::noReading);
            }

            testTruePeakOnlyMode();
            testSwitchingLoudnessOffAndOn();
            testTruePeakIndependentOfHostBlockSize();
        }

    private:
        static void feed (LoudnessMeter& meter, const Signal& signal, int blockSize = 512)
        {
            const auto numChannels = (int) signal.size();
            const auto numSamples = (int) signal.front().size();
            juce::AudioBuffer<float> block (numChannels, blockSize);

            for (int start = 0; start < numSamples; start += blockSize)
            {
                const auto length = juce::jmin (blockSize, numSamples - start);
                block.setSize (numChannels, length, false, false, true);

                for (int ch = 0; ch < numChannels; ++ch)
                    block.copyFrom (ch, 0, signal[(size_t) ch].data() + start, length);

                meter.process (block);
            }
        }

        // 类音乐素材：两个正弦 + 噪声，中间一个超过 0 dBFS 的尖峰（让真峰值读数有起伏）。
        static Signal makeMusicLike (int numChannels, double sampleRate, double seconds)
        {
            auto signal = makeSine (numChannels, sampleRate, 997.0, 0.3f, seconds);
            const auto low = makeSine (numChannels, sampleRate, 61.0, 0.3f, seconds);
            const auto noise = makeNoise (numChannels, (int) signal.front().size(), 0.2f, 55);

            for (size_t ch = 0; ch < signal.size(); ++ch)
                for (size_t i = 0; i < signal[ch].size(); ++i)
                    signal[ch][i] += low[ch][i] + noise[ch][i];

            for (auto& channel : signal)
                channel[channel.size() / 3] = 1.4f;

            return signal;
        }

        // 只算真峰值（IN 表用）：真峰值读数与完整模式逐位相同，两个 LUFS 读数恒为 noReading。
        void testTruePeakOnlyMode()
        {
            beginTest ("true-peak-only mode: identical true peak, no loudness readings");

            for (auto sampleRate : { 44100.0, 48000.0 })
                for (auto blockSize : { 64, 333, 512 })
                {
                    LoudnessMeter full, peakOnly;
                    full.prepare ({ sampleRate, 512, 2 });
                    peakOnly.setLoudnessEnabled (false);
                    peakOnly.prepare ({ sampleRate, 512, 2 });
                    expect (full.isLoudnessEnabled() && ! peakOnly.isLoudnessEnabled());

                    const auto signal = makeMusicLike (2, sampleRate, 4.0);
                    const auto numSamples = (int) signal.front().size();
                    juce::AudioBuffer<float> block (2, blockSize);
                    int peakMismatches = 0, loudnessReadings = 0;

                    for (int start = 0; start < numSamples; start += blockSize)
                    {
                        const auto length = juce::jmin (blockSize, numSamples - start);
                        block.setSize (2, length, false, false, true);

                        for (int ch = 0; ch < 2; ++ch)
                            block.copyFrom (ch, 0, signal[(size_t) ch].data() + start, length);

                        full.process (block);
                        peakOnly.process (block);

                        peakMismatches += sameBits (full.getAndResetTruePeakDb(), peakOnly.getAndResetTruePeakDb()) ? 0 : 1;
                        loudnessReadings += (peakOnly.getShortTermLufs() != LoudnessMeter::noReading ? 1 : 0)
                                          + (peakOnly.getIntegratedLufs() != LoudnessMeter::noReading ? 1 : 0);
                    }

                    const auto caseName = "@" + juce::String (sampleRate) + " block " + juce::String (blockSize);
                    expectEquals (peakMismatches, 0, "true peak differs: " + caseName);
                    expectEquals (loudnessReadings, 0, "peak-only meter produced a loudness reading: " + caseName);
                    expect (full.getIntegratedLufs() > LoudnessMeter::noReading, "full meter must still measure loudness: " + caseName);
                }
        }

        // 播放中途关掉响度：读数立刻变成 noReading、真峰值照常；再打开：从头计量（不混进关掉之前的那段）。
        void testSwitchingLoudnessOffAndOn()
        {
            beginTest ("switching loudness off shows no reading at once; switching on measures from scratch");

            constexpr double sampleRate = 48000.0;
            LoudnessMeter meter;
            meter.prepare ({ sampleRate, 512, 2 });

            feed (meter, makeSine (2, sampleRate, 1000.0, juce::Decibels::decibelsToGain (-20.0f), 3.0));
            expectWithinAbsoluteError (meter.getIntegratedLufs(), -20.0f, 0.05f);

            meter.setLoudnessEnabled (false);
            expectEquals (meter.getIntegratedLufs(), LoudnessMeter::noReading);
            expectEquals (meter.getShortTermLufs(), LoudnessMeter::noReading);

            juce::ignoreUnused (meter.getAndResetTruePeakDb());
            feed (meter, makeSine (2, sampleRate, 1000.0, juce::Decibels::decibelsToGain (-20.0f), 1.0));
            expectEquals (meter.getIntegratedLufs(), LoudnessMeter::noReading);
            expectWithinAbsoluteError (meter.getAndResetTruePeakDb(), -20.0f, 0.05f);

            meter.setLoudnessEnabled (true);
            feed (meter, makeSine (2, sampleRate, 1000.0, juce::Decibels::decibelsToGain (-30.0f), 3.0));
            expectWithinAbsoluteError (meter.getIntegratedLufs(), -30.0f, 0.05f);
            expectWithinAbsoluteError (meter.getShortTermLufs(), -30.0f, 0.05f);
        }

        // 宿主给的块可以比 prepare 时大、也可以是单声道：真峰值读数与逐样本参照（processSample 的 jmax 折叠）逐位相同。
        void testTruePeakIndependentOfHostBlockSize()
        {
            beginTest ("true peak reading is bit-exact for host blocks larger than prepared, mono and stereo");

            constexpr double sampleRate = 48000.0;

            for (int numChannels : { 1, 2 })
                for (auto blockSize : { 1, 64, 4096 })
                {
                    const auto signal = makeMusicLike (numChannels, sampleRate, 1.0);
                    const auto reference = referencePeaksOf (signal);

                    float referencePeak = 0.0f;
                    for (const auto& channel : reference)
                        referencePeak = juce::jmax (referencePeak, jmaxFold (channel, 0, (int) channel.size()));

                    const auto expectedDb = juce::jmax (LoudnessMeter::noReading,
                                                        juce::Decibels::gainToDecibels (referencePeak, LoudnessMeter::noReading));

                    LoudnessMeter meter;
                    meter.prepare ({ sampleRate, 64, (juce::uint32) numChannels });
                    feed (meter, signal, blockSize);

                    expect (sameBits (meter.getAndResetTruePeakDb(), expectedDb),
                            juce::String (numChannels) + " channel(s), block " + juce::String (blockSize));
                }
        }
    };

    static SaturatorTests saturatorTests;
    static TruePeakTests truePeakTests;
    static LoudnessMeterTests loudnessMeterTests;
}

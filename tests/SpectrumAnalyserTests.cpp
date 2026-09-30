/*
  ==============================================================================

    SpectrumAnalyserTests.cpp
    频谱仪计算（Source/ui/SpectrumAnalyser，v0.1.8.0）的自动测试：正弦读数的频率与幅度（0 dBFS 正弦读 0 dB）、
    远处的泄漏足够低、RMS 读数、没有新样本时曲线落下、高采样率换更长的 FFT 仍读准；
    OUT 换来源（restartOutput）之后峰值线与平均线不带着前一种信号，攒满一个窗口后从新信号起步、IN 不受影响。

    T-032（2026-09-30）：
      - 每点的 bin 取法搬到 setSampleRate 预算成表（R7-05 / R9-10）：与改前 analyse 的逐点算式（原样抄在这里当参照）逐位相同，
        44.1 k ～ 192 k 六种采样率；另把四条曲线的逐位摘要打进日志，供改前改后在同一台机器上对照；
      - FFT 长度封顶 16384（R9-11）：192 k 也用 16384，正弦读数与泄漏照样达标；
      - IN 的频谱可以关（界面 IN 虚线关着时）：OUT 逐位不受影响、IN 留在底线、RMS 照算；重新打开后下一帧直接取测量值。

  ==============================================================================
*/

#include "TestHelpers.h"

#include "../Source/ui/SpectrumAnalyser.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace celadon::test
{
    class SpectrumAnalyserTests  : public juce::UnitTest
    {
    public:
        SpectrumAnalyserTests() : juce::UnitTest ("SpectrumAnalyser", "Celadon") {}

        // 四条曲线（OUT / IN 显示、平均、峰值）480 点的逐位摘要（FNV-1a，64 位，按 float 的位模式算）。
        // T-032（R7-05 / R9-10）把每点的 bin 区间与插值系数搬到 setSampleRate 预算，显示必须逐位不变：
        // 摘要打进日志，改前改后在同一台机器、同一编译器下对照（FFT 引擎不同的平台之间摘要本来就不同）。
        static juce::String curveDigest (const ui::SpectrumAnalyser& analyser)
        {
            std::uint64_t hash = 1469598103934665603ull;

            const auto mix = [&hash] (const std::vector<float>& values)
            {
                for (const auto value : values)
                {
                    std::uint32_t bits = 0;
                    std::memcpy (&bits, &value, sizeof (bits));

                    for (int shift = 0; shift < 32; shift += 8)
                    {
                        hash ^= (bits >> shift) & 0xffu;
                        hash *= 1099511628211ull;
                    }
                }
            };

            using Stream = ui::SpectrumAnalyser::Stream;
            mix (analyser.getDisplayDb (Stream::output));
            mix (analyser.getDisplayDb (Stream::input));
            mix (analyser.getAverageDb());
            mix (analyser.getPeakDb());
            return juce::String::toHexString ((juce::int64) hash);
        }

        // 确定性的宽带测试信号：三个正弦（97 Hz / 1 kHz / 6.1 kHz）+ 固定种子的白噪，让 480 个点都离开底线，
        // 低频的「两个 bin 之间插值」与高频的「区间里取最大」两种取法都用到。
        class Broadband
        {
        public:
            explicit Broadband (double sampleRate)
                : stepA (juce::MathConstants<double>::twoPi * 1000.0 / sampleRate),
                  stepB (juce::MathConstants<double>::twoPi * 97.0 / sampleRate),
                  stepC (juce::MathConstants<double>::twoPi * 6100.0 / sampleRate)
            {
            }

            void fill (std::vector<float>& block)
            {
                for (auto& sample : block)
                {
                    sample = (float) (0.3 * std::sin (phaseA) + 0.2 * std::sin (phaseB) + 0.1 * std::sin (phaseC))
                             + 0.02f * (random.nextFloat() * 2.0f - 1.0f);
                    phaseA += stepA;
                    phaseB += stepB;
                    phaseC += stepC;
                }
            }

        private:
            const double stepA, stepB, stepC;
            double phaseA = 0.0, phaseB = 0.0, phaseC = 0.0;
            juce::Random random { 20260929 };
        };

        // 按 1600 个样本一帧喂进去（与界面 48 kHz 下每帧到的样本数相当）。
        static void feedBroadband (ui::SpectrumAnalyser& analyser, double sampleRate, int frames, bool alsoInput)
        {
            using Stream = ui::SpectrumAnalyser::Stream;
            std::vector<float> block (1600);
            Broadband source (sampleRate);

            for (int frame = 0; frame < frames; ++frame)
            {
                source.fill (block);
                analyser.push (Stream::output, block.data(), (int) block.size());

                if (alsoInput)
                    analyser.push (Stream::input, block.data(), (int) block.size());

                analyser.update();
            }
        }

        static std::uint32_t bitsOf (float value) noexcept
        {
            std::uint32_t bits = 0;
            std::memcpy (&bits, &value, sizeof (bits));
            return bits;
        }

        static int countBitMismatches (const std::vector<float>& actual, const std::vector<float>& expected)
        {
            if (actual.size() != expected.size())
                return (int) juce::jmax (actual.size(), expected.size());

            int mismatches = 0;

            for (size_t i = 0; i < actual.size(); ++i)
                if (bitsOf (actual[i]) != bitsOf (expected[i]))
                    ++mismatches;

            return mismatches;
        }

        // 参照：T-032 之前 analyse 的整条算法原样抄下来（Hann 窗与相干增益、按时间顺序加窗、只算非负频率的幅度谱、
        // 每点每帧现算 bin 区间与插值系数）。recent = 最近 fftSize 个样本（最旧的在前）。同一平台、同一个 JUCE FFT 引擎下，
        // 新实现（setSampleRate 预算成表、analyse 查表）的测量值必须与它逐位相同。
        static std::vector<float> referenceMeasurement (const std::vector<float>& recent, double sampleRate, int fftOrder)
        {
            using A = ui::SpectrumAnalyser;
            const auto fftSize = 1 << fftOrder;
            juce::dsp::FFT fft (fftOrder);
            std::vector<float> window ((size_t) fftSize), buffer ((size_t) (2 * fftSize), 0.0f);
            double sum = 0.0;

            for (int i = 0; i < fftSize; ++i)
            {
                window[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) fftSize);
                sum += (double) window[(size_t) i];
            }

            const auto windowGain = (float) (2.0 / sum);

            for (int i = 0; i < fftSize; ++i)
                buffer[(size_t) i] = recent[(size_t) i] * window[(size_t) i];

            fft.performFrequencyOnlyForwardTransform (buffer.data(), true);

            const auto binWidth = sampleRate / (double) fftSize;
            const auto lastBin = fftSize / 2;
            const auto halfStep = 0.5 * std::log2 ((double) A::maxFrequency / (double) A::minFrequency) / (double) (A::numPoints - 1);
            const auto powerAt = [&buffer] (int bin) { const auto m = (double) buffer[(size_t) bin]; return m * m; };
            std::vector<float> spectrumDb ((size_t) A::numPoints);

            for (int i = 0; i < A::numPoints; ++i)
            {
                const auto f = (double) A::frequencyAt (i);
                const auto low = f * std::pow (2.0, -halfStep) / binWidth;
                const auto high = f * std::pow (2.0, halfStep) / binWidth;
                double power = 0.0;

                if (high - low < 1.0)
                {
                    const auto position = juce::jlimit (0.0, (double) lastBin, f / binWidth);
                    const auto below = juce::jlimit (0, lastBin, (int) std::floor (position));
                    const auto above = juce::jmin (lastBin, below + 1);
                    const auto t = position - (double) below;
                    power = powerAt (below) * (1.0 - t) + powerAt (above) * t;
                }
                else
                {
                    const auto first = juce::jlimit (0, lastBin, (int) std::floor (low));
                    const auto last = juce::jlimit (first, lastBin, (int) std::ceil (high));

                    for (int bin = first; bin <= last; ++bin)
                        power = juce::jmax (power, powerAt (bin));
                }

                const auto amplitude = std::sqrt (power) * (double) windowGain;
                spectrumDb[(size_t) i] = amplitude > 0.0 ? juce::jmax (A::floorDb, (float) (20.0 * std::log10 (amplitude))) : A::floorDb;
            }

            return spectrumDb;
        }

        static int fftOrderOf (int fftSize) noexcept
        {
            int order = 0;

            while ((1 << order) < fftSize)
                ++order;

            return order;
        }

        void runTest() override
        {
            using ui::SpectrumAnalyser;
            using Stream = SpectrumAnalyser::Stream;

            // 显示逐位不变的回归保护（R7-05 / R9-10）：一帧的测量值与改前的逐点算式逐位比。
            // OUT 从底线按「升」走一步（显示 = 底线 + (测量 − 底线) × 0.6，与 update 同一组 float 运算）；
            // IN 先关再开，下一帧直接取测量值——同时核对「重新打开取测量值」这一条。
            for (const auto sampleRate : { 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 })
            {
                beginTest ("the precomputed point ranges reproduce the old per-frame formula bit for bit @" + juce::String (sampleRate, 0));

                SpectrumAnalyser analyser;
                analyser.setSampleRate (sampleRate);
                analyser.setInputSpectrumEnabled (false);
                analyser.setInputSpectrumEnabled (true);

                const auto fftSize = analyser.getFftSize();
                std::vector<float> signal ((size_t) (fftSize + 1234));   // 多于一个窗口：环形缓冲写满，写指针不在 0
                Broadband (sampleRate).fill (signal);
                analyser.push (Stream::output, signal.data(), (int) signal.size());
                analyser.push (Stream::input, signal.data(), (int) signal.size());
                analyser.update();

                const std::vector<float> recent (signal.end() - fftSize, signal.end());
                const auto expected = referenceMeasurement (recent, sampleRate, fftOrderOf (fftSize));
                std::vector<float> expectedOutput (expected.size());

                for (size_t i = 0; i < expected.size(); ++i)
                {
                    auto shown = SpectrumAnalyser::floorDb;
                    shown += (expected[i] - shown) * (expected[i] > shown ? 0.6f : 0.15f);
                    expectedOutput[i] = shown;
                }

                int aboveFloor = 0;

                for (const auto value : expected)
                    if (value > SpectrumAnalyser::floorDb + 20.0f)
                        ++aboveFloor;

                expectEquals (aboveFloor, SpectrumAnalyser::numPoints, "every point carries signal");
                expect (analyser.hasInputSpectrum(), "IN was re-enabled and has been analysed");
                expectEquals (countBitMismatches (analyser.getDisplayDb (Stream::input), expected), 0, "IN = the measurement, bit for bit");
                expectEquals (countBitMismatches (analyser.getDisplayDb (Stream::output), expectedOutput), 0, "OUT = one rise step, bit for bit");
            }

            beginTest ("FFT length is 8192 up to 50 kHz and 16384 above (capped at 16384 for 176.4 / 192 kHz)");
            {
                SpectrumAnalyser analyser;

                for (const auto& [sampleRate, size] : { std::pair<double, int> { 44100.0, 8192 }, { 48000.0, 8192 }, { 88200.0, 16384 },
                                                        { 96000.0, 16384 }, { 176400.0, 16384 }, { 192000.0, 16384 } })
                {
                    analyser.setSampleRate (sampleRate);
                    expectEquals (analyser.getFftSize(), size, "@" + juce::String (sampleRate, 0));
                }
            }

            // 四条曲线的逐位摘要只打进日志（FFT 引擎不同的平台之间本来就不同，没法写死）：改前改后在同一台机器上对照。
            // 2026-09-30 107（x86-64，GCC 15.2，JUCE 回退 FFT）改前：48 k 2ce1118a1553e982、96 k bc392678f355527；
            // 192 k 改前 14c0b6cf9d12f58f，R9-11 把 192 k 的 FFT 从 32768 改成 16384，这一个按设计会变。
            for (const auto sampleRate : { 48000.0, 96000.0, 192000.0 })
            {
                beginTest ("curve digest @" + juce::String (sampleRate, 0) + " (bit-exact record, see the log)");

                SpectrumAnalyser analyser;
                analyser.setSampleRate (sampleRate);
                feedBroadband (analyser, sampleRate, 40, true);

                for (int frame = 0; frame < 5; ++frame)
                    analyser.update();

                logMessage ("  digest " + curveDigest (analyser) + ", 1 kHz " + juce::String (analyser.displayDbAt (Stream::output, 1000.0f), 4)
                            + " dB, 97 Hz " + juce::String (analyser.displayDbAt (Stream::output, 97.0f), 4) + " dB");
                expect (analyser.displayDbAt (Stream::output, 1000.0f) > -40.0f, "the broadband signal is visible");
                expect (analyser.displayDbAt (Stream::input, 1000.0f) > -40.0f, "on both streams");
            }

            for (const auto sampleRate : { 48000.0, 96000.0, 192000.0 })
            {
                beginTest ("a -6 dBFS 1 kHz sine reads -6 dB at 1 kHz @" + juce::String (sampleRate, 0));

                SpectrumAnalyser analyser;
                analyser.setSampleRate (sampleRate);

                std::vector<float> block (1600);
                double phase = 0.0;
                const auto increment = juce::MathConstants<double>::twoPi * 1000.0 / sampleRate;

                for (int frame = 0; frame < 60; ++frame)
                {
                    for (auto& sample : block)
                    {
                        sample = (float) (0.5 * std::sin (phase));
                        phase += increment;
                    }

                    analyser.push (Stream::output, block.data(), (int) block.size());
                    analyser.update();
                }

                const auto atTone = analyser.displayDbAt (Stream::output, 1000.0f);
                logMessage ("  1 kHz reads " + juce::String (atTone, 2) + " dB, 100 Hz " + juce::String (analyser.displayDbAt (Stream::output, 100.0f), 1)
                            + " dB, 10 kHz " + juce::String (analyser.displayDbAt (Stream::output, 10000.0f), 1) + " dB");
                // Hann 窗的扇贝损失：正弦落在两个 bin 之间最多低 1.42 dB；1 kHz 在 48 kHz / 8192 点时偏 0.33 个 bin（约 0.6 dB）。
                expect (std::abs (atTone + 6.0f) < 1.0f, "tone level: " + juce::String (atTone, 2));
                expect (analyser.displayDbAt (Stream::output, 100.0f) < -60.0f, "leakage one decade below");
                expect (analyser.displayDbAt (Stream::output, 10000.0f) < -60.0f, "leakage one decade above");
                expect (std::abs (analyser.getRecentRmsDb (Stream::output) + 9.03f) < 0.1f, "RMS of a 0.5 sine is -9 dBFS");
                expect (analyser.displayDbAt (Stream::input, 1000.0f) <= SpectrumAnalyser::floorDb + 0.01f, "the other stream stays empty");

                // 峰值线不低于显示曲线；平均线在正弦处接近显示值。
                const auto index = (int) std::round (SpectrumAnalyser::positionOf (1000.0f) * (float) (SpectrumAnalyser::numPoints - 1));
                expect (analyser.getPeakDb()[(size_t) index] >= analyser.getDisplayDb (Stream::output)[(size_t) index] - 1.0e-3f);

                // 没有新样本：曲线按动态往下落。
                for (int frame = 0; frame < 10; ++frame)
                    analyser.update();

                expect (analyser.displayDbAt (Stream::output, 1000.0f) < atTone - 5.0f, "the display falls without new samples");
                expectEquals (analyser.getRecentRmsDb (Stream::output), SpectrumAnalyser::floorDb);
            }

            beginTest ("restartOutput drops the previous signal's peak and average lines and leaves IN alone");
            {
                SpectrumAnalyser analyser;
                analyser.setSampleRate (48000.0);
                std::vector<float> block (1600);
                double phase = 0.0;
                const auto increment = juce::MathConstants<double>::twoPi * 1000.0 / 48000.0;

                const auto feed = [&] (float amplitude, int frames)
                {
                    for (int frame = 0; frame < frames; ++frame)
                    {
                        for (auto& sample : block)
                        {
                            sample = (float) (amplitude * std::sin (phase));
                            phase += increment;
                        }

                        analyser.push (Stream::input, block.data(), (int) block.size());
                        analyser.push (Stream::output, block.data(), (int) block.size());
                        analyser.update();
                    }
                };

                const auto index = (int) std::round (SpectrumAnalyser::positionOf (1000.0f) * (float) (SpectrumAnalyser::numPoints - 1));
                feed (0.5f, 40);   // −6 dBFS
                const auto loudPeak = analyser.getPeakDb()[(size_t) index];
                const auto inputBefore = analyser.displayDbAt (Stream::input, 1000.0f);

                analyser.restartOutput();
                expect (analyser.isRestartingOutput());
                expectEquals (analyser.getPeakDb()[(size_t) index], SpectrumAnalyser::floorDb, "peak line cleared");
                expectEquals (analyser.displayDbAt (Stream::output, 1000.0f), SpectrumAnalyser::floorDb, "OUT display cleared");
                expect (std::abs (analyser.displayDbAt (Stream::input, 1000.0f) - inputBefore) < 1.0e-6f, "IN untouched");

                // 新信号轻 40 dB：攒满一个 8192 点窗口之前两条线留在底线；之后从新信号的读数起步。
                feed (0.005f, 3);
                expect (analyser.isRestartingOutput(), "still collecting the first full window");
                expectEquals (analyser.getPeakDb()[(size_t) index], SpectrumAnalyser::floorDb);

                feed (0.005f, 10);
                expect (! analyser.isRestartingOutput(), "restart completes once the window is full");
                const auto quietPeak = analyser.getPeakDb()[(size_t) index];
                logMessage ("  1 kHz peak line: " + juce::String (loudPeak, 1) + " dB before, " + juce::String (quietPeak, 1) + " dB after the restart");
                expect (quietPeak < loudPeak - 30.0f, "the peak line no longer holds the previous signal");
                expect (std::abs (quietPeak + 46.0f) < 3.0f, "the peak line starts from the new signal: " + juce::String (quietPeak, 1));
                expect (analyser.getAverageDb()[(size_t) index] < loudPeak - 30.0f, "the average line starts from the new signal");
            }

            beginTest ("switching the IN spectrum off leaves OUT bit-exact and IN at the floor; switching it back on starts from the measurement");
            {
                SpectrumAnalyser reference, skipping;
                reference.setSampleRate (48000.0);
                skipping.setSampleRate (48000.0);

                std::vector<float> block (1600), inputHistory;
                Broadband source (48000.0);

                const auto feed = [&] (int frames)
                {
                    for (int frame = 0; frame < frames; ++frame)
                    {
                        source.fill (block);
                        inputHistory.insert (inputHistory.end(), block.begin(), block.end());

                        for (auto* analyser : { &reference, &skipping })
                        {
                            analyser->push (Stream::input, block.data(), (int) block.size());
                            analyser->push (Stream::output, block.data(), (int) block.size());
                            analyser->update();
                        }
                    }
                };

                feed (6);
                expect (skipping.displayDbAt (Stream::input, 1000.0f) > -40.0f, "IN is analysed by default");

                skipping.setInputSpectrumEnabled (false);
                expect (! skipping.isInputSpectrumEnabled() && ! skipping.hasInputSpectrum());
                expectEquals (countBitMismatches (skipping.getDisplayDb (Stream::input),
                                                  std::vector<float> ((size_t) SpectrumAnalyser::numPoints, SpectrumAnalyser::floorDb)), 0,
                              "switching off drops the IN curve to the floor at once");

                feed (14);
                expectEquals (countBitMismatches (skipping.getDisplayDb (Stream::output), reference.getDisplayDb (Stream::output)), 0, "OUT display untouched");
                expectEquals (countBitMismatches (skipping.getAverageDb(), reference.getAverageDb()), 0, "average line untouched");
                expectEquals (countBitMismatches (skipping.getPeakDb(), reference.getPeakDb()), 0, "peak line untouched");
                expectEquals (countBitMismatches (skipping.getDisplayDb (Stream::input),
                                                  std::vector<float> ((size_t) SpectrumAnalyser::numPoints, SpectrumAnalyser::floorDb)), 0,
                              "IN stays at the floor while switched off");
                expect (bitsOf (skipping.getRecentRmsDb (Stream::input)) == bitsOf (reference.getRecentRmsDb (Stream::input))
                            && skipping.getRecentRmsDb (Stream::input) > -40.0f, "IN RMS still measured");

                skipping.setInputSpectrumEnabled (true);
                expect (skipping.isInputSpectrumEnabled() && ! skipping.hasInputSpectrum(), "no IN curve until the next analysis");

                // 没有新样本的一帧不算「分析过」：IN 仍在底线、仍然没有曲线。
                reference.update();
                skipping.update();
                expect (! skipping.hasInputSpectrum());
                expectEquals (skipping.displayDbAt (Stream::input, 1000.0f), SpectrumAnalyser::floorDb);

                feed (1);
                expect (skipping.hasInputSpectrum());
                const std::vector<float> recent (inputHistory.end() - skipping.getFftSize(), inputHistory.end());
                expectEquals (countBitMismatches (skipping.getDisplayDb (Stream::input), referenceMeasurement (recent, 48000.0, 13)), 0,
                              "the first frame after switching back on shows the measurement itself");
                expectEquals (countBitMismatches (skipping.getDisplayDb (Stream::output), reference.getDisplayDb (Stream::output)), 0, "OUT still untouched");
            }

            beginTest ("frequency axis is 20 Hz to 20 kHz, logarithmic");
            {
                expect (std::abs (SpectrumAnalyser::frequencyAt (0) - 20.0f) < 1.0e-3f);
                expect (std::abs (SpectrumAnalyser::frequencyAt (SpectrumAnalyser::numPoints - 1) - 20000.0f) < 0.5f);
                expect (std::abs (SpectrumAnalyser::positionOf (632.456f) - 0.5f) < 1.0e-3f, "geometric centre");
            }
        }
    };

    static SpectrumAnalyserTests spectrumAnalyserTests;
}

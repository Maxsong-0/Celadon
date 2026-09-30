/*
  ==============================================================================

    SpectrumAnalyserTests.cpp
    频谱仪计算（Source/ui/SpectrumAnalyser，v0.1.8.0）的自动测试：正弦读数的频率与幅度（0 dBFS 正弦读 0 dB）、
    远处的泄漏足够低、RMS 读数、没有新样本时曲线落下、高采样率换更长的 FFT 仍读准；
    OUT 换来源（restartOutput）之后峰值线与平均线不带着前一种信号，攒满一个窗口后从新信号起步、IN 不受影响。

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

        // 确定性的宽带测试信号（三个正弦 + 固定种子的白噪）按 1600 个样本一帧喂进去，让 480 个点都离开底线。
        static void feedBroadband (ui::SpectrumAnalyser& analyser, double sampleRate, int frames, bool alsoInput)
        {
            using Stream = ui::SpectrumAnalyser::Stream;
            std::vector<float> block (1600);
            juce::Random random (20260929);
            double phaseA = 0.0, phaseB = 0.0, phaseC = 0.0;
            const auto stepA = juce::MathConstants<double>::twoPi * 1000.0 / sampleRate;
            const auto stepB = juce::MathConstants<double>::twoPi * 97.0 / sampleRate;
            const auto stepC = juce::MathConstants<double>::twoPi * 6100.0 / sampleRate;

            for (int frame = 0; frame < frames; ++frame)
            {
                for (auto& sample : block)
                {
                    sample = (float) (0.3 * std::sin (phaseA) + 0.2 * std::sin (phaseB) + 0.1 * std::sin (phaseC))
                             + 0.02f * (random.nextFloat() * 2.0f - 1.0f);
                    phaseA += stepA;
                    phaseB += stepB;
                    phaseC += stepC;
                }

                analyser.push (Stream::output, block.data(), (int) block.size());

                if (alsoInput)
                    analyser.push (Stream::input, block.data(), (int) block.size());

                analyser.update();
            }
        }

        void runTest() override
        {
            using ui::SpectrumAnalyser;
            using Stream = SpectrumAnalyser::Stream;

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

            for (const auto sampleRate : { 48000.0, 96000.0 })
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

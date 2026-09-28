/*
  ==============================================================================

    SpectrumAnalyserTests.cpp
    频谱仪计算（Source/ui/SpectrumAnalyser，v0.1.8.0）的自动测试：正弦读数的频率与幅度（0 dBFS 正弦读 0 dB）、
    远处的泄漏足够低、RMS 读数、没有新样本时曲线落下、高采样率换更长的 FFT 仍读准。

  ==============================================================================
*/

#include "TestHelpers.h"

#include "../Source/ui/SpectrumAnalyser.h"

#include <cmath>
#include <vector>

namespace celadon::test
{
    class SpectrumAnalyserTests  : public juce::UnitTest
    {
    public:
        SpectrumAnalyserTests() : juce::UnitTest ("SpectrumAnalyser", "Celadon") {}

        void runTest() override
        {
            using ui::SpectrumAnalyser;
            using Stream = SpectrumAnalyser::Stream;

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

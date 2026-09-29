/*
  ==============================================================================

    SpectrumAnalyser.cpp

  ==============================================================================
*/

#include "SpectrumAnalyser.h"

#include <cmath>

namespace celadon::ui
{
    namespace
    {
        constexpr float riseAmount   = 0.6f;    // 显示曲线：每帧向新测量靠近的比例（升）
        constexpr float fallAmount   = 0.15f;   // ……（落）：30 帧 / s 时约 0.2 s 的时间常数
        constexpr float idleFallDb   = 0.8f;    // 没有新样本时每帧落多少 dB（24 dB/s）
        constexpr float averageAmount = 0.04f;  // 平均线：约 0.8 s 的时间常数
        constexpr int   averageSmoothRadius = 6;
        constexpr int   peakHoldLength = 45;    // 1.5 s
        constexpr float peakFallDb = 0.4f;      // 12 dB/s

        // 沿频率方向的滑动平均（两端按实际点数平均）。
        void smooth (const std::vector<float>& source, std::vector<float>& destination, int radius)
        {
            const auto n = (int) source.size();
            destination.resize (source.size());

            for (int i = 0; i < n; ++i)
            {
                double sum = 0.0;
                int count = 0;

                for (int j = juce::jmax (0, i - radius); j <= juce::jmin (n - 1, i + radius); ++j)
                {
                    sum += (double) source[(size_t) j];
                    ++count;
                }

                destination[(size_t) i] = (float) (sum / (double) juce::jmax (1, count));
            }
        }
    }

    SpectrumAnalyser::SpectrumAnalyser()
    {
        setSampleRate (48000.0);
    }

    float SpectrumAnalyser::frequencyAt (int index) noexcept
    {
        const auto u = (float) juce::jlimit (0, numPoints - 1, index) / (float) (numPoints - 1);
        return minFrequency * std::pow (maxFrequency / minFrequency, u);
    }

    float SpectrumAnalyser::positionOf (float frequency) noexcept
    {
        if (! (frequency > 0.0f))
            return 0.0f;

        return std::log (frequency / minFrequency) / std::log (maxFrequency / minFrequency);
    }

    void SpectrumAnalyser::setSampleRate (double newSampleRate)
    {
        if (! (newSampleRate > 0.0))
            newSampleRate = 48000.0;

        const auto order = newSampleRate <= 50000.0 ? 13 : (newSampleRate <= 100000.0 ? 14 : 15);

        if (fft != nullptr && order == fftOrder && juce::approximatelyEqual (newSampleRate, sampleRate))
            return;

        sampleRate = newSampleRate;
        fftOrder = order;
        fftSize = 1 << order;
        fft = std::make_unique<juce::dsp::FFT> (fftOrder);

        // Hann 窗；相干增益（窗的和）用来把 bin 的幅度换回正弦的幅度。
        window.resize ((size_t) fftSize);
        double sum = 0.0;

        for (int i = 0; i < fftSize; ++i)
        {
            window[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) fftSize);
            sum += (double) window[(size_t) i];
        }

        windowGain = (float) (2.0 / sum);
        fftBuffer.assign ((size_t) (2 * fftSize), 0.0f);
        measured.assign ((size_t) numPoints, floorDb);

        for (auto& channel : channels)
            channel.history.assign ((size_t) fftSize, 0.0f);

        reset();
    }

    void SpectrumAnalyser::reset()
    {
        for (auto& channel : channels)
        {
            std::fill (channel.history.begin(), channel.history.end(), 0.0f);
            channel.writeIndex = 0;
            channel.newSamples = 0;
            channel.newEnergy = 0.0;
            channel.display.assign ((size_t) numPoints, floorDb);
            channel.recentRmsDb = floorDb;
        }

        average.assign ((size_t) numPoints, floorDb);
        averageSmoothed.assign ((size_t) numPoints, floorDb);
        peak.assign ((size_t) numPoints, floorDb);
        peakHoldFrames.assign ((size_t) numPoints, 0);
        outputSamplesSinceRestart = -1;
    }

    void SpectrumAnalyser::restartOutput()
    {
        auto& channel = channels[(size_t) Stream::output];
        std::fill (channel.history.begin(), channel.history.end(), 0.0f);
        channel.writeIndex = 0;
        channel.newSamples = 0;
        channel.newEnergy = 0.0;
        std::fill (channel.display.begin(), channel.display.end(), floorDb);
        channel.recentRmsDb = floorDb;

        std::fill (average.begin(), average.end(), floorDb);
        std::fill (averageSmoothed.begin(), averageSmoothed.end(), floorDb);
        std::fill (peak.begin(), peak.end(), floorDb);
        std::fill (peakHoldFrames.begin(), peakHoldFrames.end(), 0);
        outputSamplesSinceRestart = 0;
    }

    void SpectrumAnalyser::push (Stream stream, const float* samples, int numSamples)
    {
        auto& channel = channels[(size_t) stream];

        if (samples == nullptr || numSamples <= 0 || channel.history.empty())
            return;

        if (stream == Stream::output && outputSamplesSinceRestart >= 0)
            outputSamplesSinceRestart = juce::jmin (fftSize, outputSamplesSinceRestart + numSamples);

        const auto size = (int) channel.history.size();

        for (int i = 0; i < numSamples; ++i)
        {
            const auto sample = std::isfinite (samples[i]) ? samples[i] : 0.0f;
            channel.history[(size_t) channel.writeIndex] = sample;
            channel.writeIndex = (channel.writeIndex + 1) % size;
            channel.newEnergy += (double) sample * (double) sample;
        }

        channel.newSamples += numSamples;
    }

    void SpectrumAnalyser::analyse (Channel& channel, std::vector<float>& spectrumDb)
    {
        // 按时间顺序（最旧的在前）加窗拷进 FFT 缓冲。
        for (int i = 0; i < fftSize; ++i)
        {
            const auto source = (channel.writeIndex + i) % fftSize;
            fftBuffer[(size_t) i] = channel.history[(size_t) source] * window[(size_t) i];
        }

        std::fill (fftBuffer.begin() + fftSize, fftBuffer.end(), 0.0f);
        fft->performFrequencyOnlyForwardTransform (fftBuffer.data(), true);

        const auto binWidth = sampleRate / (double) fftSize;
        const auto lastBin = fftSize / 2;
        const auto halfStep = 0.5 * std::log2 ((double) maxFrequency / (double) minFrequency) / (double) (numPoints - 1);

        const auto powerAt = [this] (int bin) { const auto m = (double) fftBuffer[(size_t) bin]; return m * m; };

        for (int i = 0; i < numPoints; ++i)
        {
            const auto f = (double) frequencyAt (i);
            const auto low = f * std::pow (2.0, -halfStep) / binWidth;
            const auto high = f * std::pow (2.0, halfStep) / binWidth;
            double power = 0.0;

            if (high - low < 1.0)
            {
                // 低频：一个点比一个 bin 还窄——在相邻两个 bin 之间按功率插值。
                const auto position = juce::jlimit (0.0, (double) lastBin, f / binWidth);
                const auto below = juce::jlimit (0, lastBin, (int) std::floor (position));
                const auto above = juce::jmin (lastBin, below + 1);
                const auto t = position - (double) below;
                power = powerAt (below) * (1.0 - t) + powerAt (above) * t;
            }
            else
            {
                // 高频：一个点覆盖好几个 bin——取其中最大的（保住谐波的尖峰；取平均会把正弦压低几个 dB）。
                const auto first = juce::jlimit (0, lastBin, (int) std::floor (low));
                const auto last = juce::jlimit (first, lastBin, (int) std::ceil (high));

                for (int bin = first; bin <= last; ++bin)
                    power = juce::jmax (power, powerAt (bin));
            }

            const auto amplitude = std::sqrt (power) * (double) windowGain;
            spectrumDb[(size_t) i] = amplitude > 0.0 ? juce::jmax (floorDb, (float) (20.0 * std::log10 (amplitude))) : floorDb;
        }
    }

    void SpectrumAnalyser::update()
    {
        for (auto& channel : channels)
        {
            if (channel.newSamples > 0)
            {
                const auto meanSquare = channel.newEnergy / (double) channel.newSamples;
                channel.recentRmsDb = meanSquare > 0.0 ? juce::jmax (floorDb, (float) (10.0 * std::log10 (meanSquare))) : floorDb;

                // 不沿频率平滑（在 dB 上平均会把一两个点宽的谐波尖峰削掉十几 dB）；平滑只给平均线。
                analyse (channel, measured);

                for (int i = 0; i < numPoints; ++i)
                {
                    auto& shown = channel.display[(size_t) i];
                    const auto target = measured[(size_t) i];
                    shown += (target - shown) * (target > shown ? riseAmount : fallAmount);
                }
            }
            else
            {
                channel.recentRmsDb = floorDb;

                for (auto& shown : channel.display)
                    shown = juce::jmax (floorDb, shown - idleFallDb);
            }

            channel.newSamples = 0;
            channel.newEnergy = 0.0;
        }

        // 平均线与峰值线只跟 OUT。
        const auto& output = channels[(size_t) Stream::output].display;

        // restartOutput 之后：FFT 窗口里还有清零的部分时两条线不画（留在底线）；攒满之后从此刻的显示曲线起步。
        if (outputSamplesSinceRestart >= 0)
        {
            if (outputSamplesSinceRestart < fftSize)
                return;

            average = output;
            peak = output;
            std::fill (peakHoldFrames.begin(), peakHoldFrames.end(), peakHoldLength);
            outputSamplesSinceRestart = -1;
            smooth (average, scratch, averageSmoothRadius);
            smooth (scratch, averageSmoothed, averageSmoothRadius);
            return;
        }

        for (int i = 0; i < numPoints; ++i)
        {
            average[(size_t) i] += (output[(size_t) i] - average[(size_t) i]) * averageAmount;

            if (output[(size_t) i] >= peak[(size_t) i])
            {
                peak[(size_t) i] = output[(size_t) i];
                peakHoldFrames[(size_t) i] = peakHoldLength;
            }
            else if (peakHoldFrames[(size_t) i] > 0)
            {
                --peakHoldFrames[(size_t) i];
            }
            else
            {
                peak[(size_t) i] = juce::jmax (output[(size_t) i], peak[(size_t) i] - peakFallDb);
            }
        }

        smooth (average, scratch, averageSmoothRadius);
        smooth (scratch, averageSmoothed, averageSmoothRadius);
    }

    float SpectrumAnalyser::displayDbAt (Stream stream, float frequency) const noexcept
    {
        const auto& values = channels[(size_t) stream].display;
        const auto position = juce::jlimit (0.0f, 1.0f, positionOf (frequency)) * (float) (numPoints - 1);
        const auto below = juce::jlimit (0, numPoints - 1, (int) std::floor (position));
        const auto above = juce::jmin (numPoints - 1, below + 1);
        const auto t = position - (float) below;
        return values[(size_t) below] + (values[(size_t) above] - values[(size_t) below]) * t;
    }
}

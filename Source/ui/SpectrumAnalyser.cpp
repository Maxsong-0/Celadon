/*
  ==============================================================================

    SpectrumAnalyser.cpp

  ==============================================================================
*/

#include "SpectrumAnalyser.h"

#include <algorithm>
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

        // ≤ 50 kHz 用 8192 点（48 kHz 时窗口约 170 ms）；更高一律 16384（T-032 R9-11）：显示上限 20 kHz，高于它的内容本就不画，
        // 192 kHz 时再翻到 32768 只让没有 vDSP 的平台（Windows 的 JUCE 回退 FFT）每帧多花一倍；封顶后 192 kHz 的窗口约 85 ms，
        // 频率分辨率减半，低频的点仍按相邻两个 bin 插值，看起来差别很小。
        const auto order = newSampleRate <= 50000.0 ? 13 : 14;

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
        updatePointRanges();

        for (auto& channel : channels)
            channel.history.assign ((size_t) fftSize, 0.0f);

        reset();
    }

    void SpectrumAnalyser::updatePointRanges()
    {
        // T-032（R7-05 / R9-10）：每个点的 bin 区间与插值系数只随采样率变，以前在 analyse 里每帧每点重算（3 次 pow + floor / ceil）。
        // 这里用原来那组 double 算式、同样的顺序算一次存表（2^±halfStep 与点号无关，只算一次，值相同），analyse 查表的结果逐位不变。
        const auto binWidth = sampleRate / (double) fftSize;
        const auto lastBin = fftSize / 2;
        const auto halfStep = 0.5 * std::log2 ((double) maxFrequency / (double) minFrequency) / (double) (numPoints - 1);
        const auto lowRatio = std::pow (2.0, -halfStep);
        const auto highRatio = std::pow (2.0, halfStep);

        pointRanges.resize ((size_t) numPoints);

        for (int i = 0; i < numPoints; ++i)
        {
            const auto f = (double) frequencyAt (i);
            const auto low = f * lowRatio / binWidth;
            const auto high = f * highRatio / binWidth;
            auto& range = pointRanges[(size_t) i];
            range.interpolate = high - low < 1.0;

            if (range.interpolate)
            {
                // 低频：一个点比一个 bin 还窄——在相邻两个 bin 之间按功率插值。
                const auto position = juce::jlimit (0.0, (double) lastBin, f / binWidth);
                range.first = juce::jlimit (0, lastBin, (int) std::floor (position));
                range.last = juce::jmin (lastBin, range.first + 1);
                range.t = position - (double) range.first;
            }
            else
            {
                // 高频：一个点覆盖好几个 bin——取其中最大的（保住谐波的尖峰；取平均会把正弦压低几个 dB）。
                range.first = juce::jlimit (0, lastBin, (int) std::floor (low));
                range.last = juce::jlimit (range.first, lastBin, (int) std::ceil (high));
                range.t = 0.0;
            }
        }
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

    void SpectrumAnalyser::setInputSpectrumEnabled (bool shouldAnalyse) noexcept
    {
        if (shouldAnalyse == inputSpectrumEnabled)
            return;

        inputSpectrumEnabled = shouldAnalyse;
        inputSeedPending = shouldAnalyse;

        // 关：IN 的显示曲线落到底线（不再跟踪，不留一条停住的旧曲线）。开：下一次分析直接取测量值（见 update）。
        if (! shouldAnalyse)
        {
            auto& display = channels[(size_t) Stream::input].display;
            std::fill (display.begin(), display.end(), floorDb);
        }
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

        const auto powerAt = [this] (int bin) { const auto m = (double) fftBuffer[(size_t) bin]; return m * m; };

        // 每个点的取法查 setSampleRate 算好的表（updatePointRanges）。
        for (int i = 0; i < numPoints; ++i)
        {
            const auto& range = pointRanges[(size_t) i];
            double power = 0.0;

            if (range.interpolate)
            {
                power = powerAt (range.first) * (1.0 - range.t) + powerAt (range.last) * range.t;
            }
            else
            {
                for (int bin = range.first; bin <= range.last; ++bin)
                    power = juce::jmax (power, powerAt (bin));
            }

            const auto amplitude = std::sqrt (power) * (double) windowGain;
            spectrumDb[(size_t) i] = amplitude > 0.0 ? juce::jmax (floorDb, (float) (20.0 * std::log10 (amplitude))) : floorDb;
        }
    }

    void SpectrumAnalyser::update()
    {
        for (size_t index = 0; index < channels.size(); ++index)
        {
            auto& channel = channels[index];

            // IN 虚线关着时不做 IN 的频谱（setInputSpectrumEnabled）：RMS 照算，显示曲线留在底线。
            const auto isInput = index == (size_t) Stream::input;
            const auto analysed = ! isInput || inputSpectrumEnabled;

            if (channel.newSamples > 0)
            {
                const auto meanSquare = channel.newEnergy / (double) channel.newSamples;
                channel.recentRmsDb = meanSquare > 0.0 ? juce::jmax (floorDb, (float) (10.0 * std::log10 (meanSquare))) : floorDb;

                if (analysed)
                {
                    // 不沿频率平滑（在 dB 上平均会把一两个点宽的谐波尖峰削掉十几 dB）；平滑只给平均线。
                    analyse (channel, measured);

                    if (isInput && inputSeedPending)
                    {
                        // IN 刚重新打开：直接取这一帧的测量值——关着的那段时间没有跟踪，从底线按「升」爬上来会让人以为信号在变。
                        std::copy (measured.begin(), measured.end(), channel.display.begin());
                        inputSeedPending = false;
                    }
                    else
                    {
                        for (int i = 0; i < numPoints; ++i)
                        {
                            auto& shown = channel.display[(size_t) i];
                            const auto target = measured[(size_t) i];
                            shown += (target - shown) * (target > shown ? riseAmount : fallAmount);
                        }
                    }
                }
            }
            else
            {
                channel.recentRmsDb = floorDb;

                if (analysed)
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

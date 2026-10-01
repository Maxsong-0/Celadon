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
        // 推进来的样本数与 RMS 累计的上限：没人取的那一路（IN）不会无限累加（约 5.8 分钟 @ 48 kHz 重新起一段）。
        constexpr int maxPendingSamples = 1 << 24;

        // 底线（floorDb = −120 dB）对应的线性幅度：低于它的点直接读底线，不取对数。
        constexpr float floorMagnitude = 1.0e-6f;

        // 向「目标」按指数靠近的比例：经过 seconds 秒、时间常数 tau 秒。
        float approachAmount (double seconds, double tau) noexcept
        {
            return (float) (1.0 - std::exp (-seconds / tau));
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
        // 频率分辨率减半，低频的点仍在相邻两个 bin 之间插值，看起来差别很小。
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
        updatePointRanges();

        for (auto& channel : channels)
            channel.history.assign ((size_t) fftSize, 0.0f);

        reset();
    }

    void SpectrumAnalyser::updatePointRanges()
    {
        // 每个点的取法只随采样率变（T-032 R7-05 / R9-10 的做法）：中心频率在 bin 轴上的位置 → 插值的左 bin 与 Hermite 权重；
        // 点区间 [f · 2^−半步, f · 2^+半步]（半步 = 相邻两点间隔的一半，倍频程）里完整落入的 bin 范围。
        const auto binWidth = sampleRate / (double) fftSize;
        const auto lastBin = fftSize / 2;
        const auto halfStep = 0.5 * std::log2 ((double) maxFrequency / (double) minFrequency) / (double) (numPoints - 1);
        const auto lowRatio = std::pow (2.0, -halfStep);
        const auto highRatio = std::pow (2.0, halfStep);

        pointRanges.resize ((size_t) numPoints);
        lastBinUsed = 1;

        for (int i = 0; i < numPoints; ++i)
        {
            const auto f = (double) frequencyAt (i);
            const auto centre = juce::jlimit (0.0, (double) lastBin, f / binWidth);
            auto& range = pointRanges[(size_t) i];

            range.bin = juce::jlimit (0, lastBin - 1, (int) std::floor (centre));
            const auto t = juce::jlimit (0.0, 1.0, centre - (double) range.bin);
            const auto t2 = t * t, t3 = t2 * t;
            range.h00 = (float) (2.0 * t3 - 3.0 * t2 + 1.0);
            range.h10 = (float) (t3 - 2.0 * t2 + t);
            range.h01 = (float) (-2.0 * t3 + 3.0 * t2);
            range.h11 = (float) (t3 - t2);

            range.firstInside = juce::jmax (0, (int) std::ceil (f * lowRatio / binWidth));
            range.lastInside = juce::jmin (lastBin, (int) std::floor (f * highRatio / binWidth));

            lastBinUsed = juce::jmax (lastBinUsed, range.bin + 1, range.lastInside);
        }

        // 插值在 bin + 1 处要用斜率，斜率要看再上面一个 bin：多算一个（不超过奈奎斯特）。
        const auto binCount = juce::jmin (lastBin, lastBinUsed + 1) + 1;
        binMagnitude.assign ((size_t) binCount, 0.0f);
        binSlope.assign ((size_t) binCount, 0.0f);
    }

    void SpectrumAnalyser::reset()
    {
        for (auto& channel : channels)
        {
            std::fill (channel.history.begin(), channel.history.end(), 0.0f);
            channel.writeIndex = 0;
            channel.newSamples = 0;
            channel.secondsWithoutSamples = 0.0;
            channel.rmsEnergy = 0.0;
            channel.rmsCount = 0;
            channel.measured.assign ((size_t) numPoints, floorDb);
            channel.display.assign ((size_t) numPoints, floorDb);
        }

        average.assign ((size_t) numPoints, floorDb);
        averageSmoothed.assign ((size_t) numPoints, floorDb);
        scratch.assign ((size_t) numPoints, floorDb);
        prefix.assign ((size_t) numPoints + 1, 0.0);
        peak.assign ((size_t) numPoints, floorDb);
        peakHoldLeft.assign ((size_t) numPoints, 0.0);
        outputSamplesSinceRestart = -1;
    }

    void SpectrumAnalyser::setInputSpectrumEnabled (bool shouldAnalyse) noexcept
    {
        if (shouldAnalyse == inputSpectrumEnabled)
            return;

        inputSpectrumEnabled = shouldAnalyse;
        inputSeedPending = shouldAnalyse;

        // 关：IN 的测量与显示曲线落到底线（不再跟踪，不留一条停住的旧曲线）。开：下一次分析直接取测量值（见 update）。
        if (! shouldAnalyse)
        {
            auto& channel = channels[(size_t) Stream::input];
            std::fill (channel.measured.begin(), channel.measured.end(), floorDb);
            std::fill (channel.display.begin(), channel.display.end(), floorDb);
        }
    }

    void SpectrumAnalyser::restartOutput()
    {
        auto& channel = channels[(size_t) Stream::output];
        std::fill (channel.history.begin(), channel.history.end(), 0.0f);
        channel.writeIndex = 0;
        channel.newSamples = 0;
        channel.secondsWithoutSamples = 0.0;
        channel.rmsEnergy = 0.0;
        channel.rmsCount = 0;
        std::fill (channel.measured.begin(), channel.measured.end(), floorDb);
        std::fill (channel.display.begin(), channel.display.end(), floorDb);

        std::fill (average.begin(), average.end(), floorDb);
        std::fill (averageSmoothed.begin(), averageSmoothed.end(), floorDb);
        std::fill (peak.begin(), peak.end(), floorDb);
        std::fill (peakHoldLeft.begin(), peakHoldLeft.end(), 0.0);
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
        double energy = 0.0;

        for (int i = 0; i < numSamples; ++i)
        {
            const auto sample = std::isfinite (samples[i]) ? samples[i] : 0.0f;
            channel.history[(size_t) channel.writeIndex] = sample;

            if (++channel.writeIndex == size)
                channel.writeIndex = 0;

            energy += (double) sample * (double) sample;
        }

        if (channel.rmsCount >= maxPendingSamples)
        {
            channel.rmsEnergy = 0.0;
            channel.rmsCount = 0;
        }

        channel.rmsEnergy += energy;
        channel.rmsCount += numSamples;
        channel.newSamples = juce::jmin (maxPendingSamples, channel.newSamples + numSamples);
    }

    float SpectrumAnalyser::takeRecentRmsDb (Stream stream) noexcept
    {
        auto& channel = channels[(size_t) stream];
        const auto count = channel.rmsCount;
        const auto energy = channel.rmsEnergy;
        channel.rmsEnergy = 0.0;
        channel.rmsCount = 0;

        if (count <= 0)
            return floorDb;

        const auto meanSquare = energy / (double) count;
        return meanSquare > 0.0 ? juce::jmax (floorDb, (float) (10.0 * std::log10 (meanSquare))) : floorDb;
    }

    void SpectrumAnalyser::analyse (Channel& channel)
    {
        // 按时间顺序（最旧的在前）加窗拷进 FFT 缓冲：写指针处是最旧的样本。
        const auto oldest = channel.writeIndex;
        const auto firstPart = fftSize - oldest;

        for (int i = 0; i < firstPart; ++i)
            fftBuffer[(size_t) i] = channel.history[(size_t) (oldest + i)] * window[(size_t) i];

        for (int i = firstPart; i < fftSize; ++i)
            fftBuffer[(size_t) i] = channel.history[(size_t) (i - firstPart)] * window[(size_t) i];

        std::fill (fftBuffer.begin() + fftSize, fftBuffer.end(), 0.0f);
        fft->performFrequencyOnlyForwardTransform (fftBuffer.data(), true);

        // 各 bin 的幅度（线性，0 dBFS 的正弦在自己的 bin 上 = 1：bin 幅度 × 相干增益的倒数）。
        const auto binCount = (int) binMagnitude.size();

        for (int bin = 0; bin < binCount; ++bin)
            binMagnitude[(size_t) bin] = std::abs (fftBuffer[(size_t) bin]) * windowGain;

        // 三次插值的斜率（幅度 / bin），在幅度上插值（不在 dB 上）——Hann 主瓣在幅度上平滑：
        //   - 局部最高的 bin：Catmull-Rom（两侧之差的一半）。夹在两个 bin 之间的峰画成圆包、顶越过较高的 bin、接近真实电平；
        //     正好落在 bin 上的正弦两侧对称，斜率为 0，不过冲；
        //   - 局部最低的 bin（与平台）：0。谷底画在 bin 上，不往下戳；
        //   - 单调的一段：Catmull-Rom 按 Fritsch-Carlson 限到相邻两条割线较小者的 3 倍，段内单调。
        // 这样每一段都不低于两端较小的那个（不会在强峰旁冲到 0 以下、画出假的深缺口）。在 dB 上插值要么在零点旁甩出几 dB 的
        // 假峰（Catmull-Rom），要么把峰削平到较低的那个 bin（单调插值，1 kHz 正弦在 1 kHz 处少读 1.6 dB）。两端取 0。
        binSlope[0] = 0.0f;
        binSlope[(size_t) (binCount - 1)] = 0.0f;

        for (int bin = 1; bin < binCount - 1; ++bin)
        {
            const auto left = binMagnitude[(size_t) bin] - binMagnitude[(size_t) (bin - 1)];
            const auto right = binMagnitude[(size_t) (bin + 1)] - binMagnitude[(size_t) bin];
            const auto centred = 0.5f * (left + right);
            auto slope = 0.0f;

            if (left > 0.0f && right < 0.0f)
                slope = centred;
            else if (left * right > 0.0f)
                slope = left > 0.0f ? juce::jmin (centred, 3.0f * juce::jmin (left, right))
                                    : juce::jmax (centred, 3.0f * juce::jmax (left, right));

            binSlope[(size_t) bin] = slope;
        }

        // 每个点：bin 之间的三次插值，与完整落在点区间里的 bin 的最大值比大，最后换成 dB。
        for (int i = 0; i < numPoints; ++i)
        {
            const auto& range = pointRanges[(size_t) i];
            const auto b = (size_t) range.bin;
            auto value = range.h00 * binMagnitude[b] + range.h10 * binSlope[b] + range.h01 * binMagnitude[b + 1] + range.h11 * binSlope[b + 1];

            for (int bin = range.firstInside; bin <= range.lastInside; ++bin)
                value = juce::jmax (value, binMagnitude[(size_t) bin]);

            channel.measured[(size_t) i] = value > floorMagnitude ? juce::jmax (floorDb, 20.0f * std::log10 (value)) : floorDb;
        }
    }

    void SpectrumAnalyser::approach (Channel& channel, float rise, float fall) noexcept
    {
        for (int i = 0; i < numPoints; ++i)
        {
            auto& shown = channel.display[(size_t) i];
            const auto target = channel.measured[(size_t) i];
            shown += (target - shown) * (target > shown ? rise : fall);
        }
    }

    void SpectrumAnalyser::smoothAverage()
    {
        // 沿频率方向的滑动平均（两端按实际点数平均），两遍：前缀和做，O(点数)。
        const auto n = numPoints;
        const auto boxSmooth = [this, n] (const std::vector<float>& source, std::vector<float>& destination)
        {
            prefix[0] = 0.0;

            for (int i = 0; i < n; ++i)
                prefix[(size_t) i + 1] = prefix[(size_t) i] + (double) source[(size_t) i];

            for (int i = 0; i < n; ++i)
            {
                const auto low = juce::jmax (0, i - averageSmoothRadius);
                const auto high = juce::jmin (n - 1, i + averageSmoothRadius);
                destination[(size_t) i] = (float) ((prefix[(size_t) high + 1] - prefix[(size_t) low]) / (double) (high - low + 1));
            }
        };

        boxSmooth (average, scratch);
        boxSmooth (scratch, averageSmoothed);
    }

    void SpectrumAnalyser::update (double elapsedSeconds)
    {
        const auto seconds = std::isfinite (elapsedSeconds) ? juce::jlimit (0.0, 0.25, elapsedSeconds) : 0.0;
        const auto rise = approachAmount (seconds, riseSeconds);
        const auto fall = approachAmount (seconds, fallSeconds);
        const auto idleDrop = idleFallDbPerSecond * (float) seconds;

        for (size_t index = 0; index < channels.size(); ++index)
        {
            auto& channel = channels[index];

            // IN 虚线关着时不做 IN 的频谱（setInputSpectrumEnabled）：RMS 照算，显示曲线留在底线。刚重新打开、还没分析过的那段
            // 也不动（没有可靠近的测量值）。
            const auto isInput = index == (size_t) Stream::input;
            const auto analysed = ! isInput || inputSpectrumEnabled;
            const auto waitingForSeed = isInput && inputSeedPending;

            if (channel.newSamples > 0)
            {
                channel.secondsWithoutSamples = 0.0;

                if (analysed)
                {
                    // 不沿频率平滑（在 dB 上平均会把一两个点宽的谐波尖峰削掉十几 dB）；平滑只给平均线。
                    analyse (channel);

                    if (waitingForSeed)
                    {
                        // IN 刚重新打开：直接取这一帧的测量值——关着的那段时间没有跟踪，从底线按「升」爬上来会让人以为信号在变。
                        channel.display = channel.measured;
                        inputSeedPending = false;
                    }
                    else
                    {
                        approach (channel, rise, fall);
                    }
                }
            }
            else
            {
                channel.secondsWithoutSamples += seconds;

                if (analysed && ! waitingForSeed)
                {
                    // 两块音频之间的帧：继续靠近上一次的测量值。宿主停了（stallSeconds 都没有样本）才往下落。
                    if (channel.secondsWithoutSamples <= stallSeconds)
                        approach (channel, rise, fall);
                    else
                        for (auto& shown : channel.display)
                            shown = juce::jmax (floorDb, shown - idleDrop);
                }
            }

            channel.newSamples = 0;
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
            std::fill (peakHoldLeft.begin(), peakHoldLeft.end(), peakHoldSeconds);
            outputSamplesSinceRestart = -1;
            smoothAverage();
            return;
        }

        const auto averageAmount = approachAmount (seconds, averageSeconds);
        const auto peakDrop = peakFallDbPerSecond * (float) seconds;

        for (int i = 0; i < numPoints; ++i)
        {
            const auto shown = output[(size_t) i];
            average[(size_t) i] += (shown - average[(size_t) i]) * averageAmount;

            if (shown >= peak[(size_t) i])
            {
                peak[(size_t) i] = shown;
                peakHoldLeft[(size_t) i] = peakHoldSeconds;
            }
            else if (peakHoldLeft[(size_t) i] > 0.0)
            {
                peakHoldLeft[(size_t) i] = juce::jmax (0.0, peakHoldLeft[(size_t) i] - seconds);
            }
            else
            {
                peak[(size_t) i] = juce::jmax (shown, peak[(size_t) i] - peakDrop);
            }
        }

        smoothAverage();
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

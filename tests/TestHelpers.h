/*
  ==============================================================================

    TestHelpers.h
    自动测试的公共工具：造测试信号、分块跑模块、独立的高精度真峰值测量。

  ==============================================================================
*/

#pragma once

#include <juce_dsp/juce_dsp.h>

#include "../Source/dsp/LimiterModule.h"
#include "../Source/dsp/TruePeak.h"

#include <array>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace celadon::test
{
    // 当前线程自启动以来经全局 operator new 分配的次数（TestMain.cpp 替换了全局 new / delete）。
    // 用法：前后各读一次，差值为 0 = 这段代码没有经 operator new 分配。不含直接 std::malloc 的分配（JUCE HeapBlock）。
    std::uint64_t allocationCountOnThisThread() noexcept;

    using Signal = std::vector<std::vector<float>>;   // [声道][样本]

    inline Signal makeSilence (int numChannels, int numSamples)
    {
        return Signal ((size_t) numChannels, std::vector<float> ((size_t) numSamples, 0.0f));
    }

    inline Signal makeSine (int numChannels, double sampleRate, double frequency, float amplitude,
                            double seconds, double phase = 0.0)
    {
        const auto numSamples = (int) (seconds * sampleRate);
        auto signal = makeSilence (numChannels, numSamples);

        for (int i = 0; i < numSamples; ++i)
        {
            const auto value = amplitude * (float) std::sin (juce::MathConstants<double>::twoPi * frequency * i / sampleRate + phase);

            for (auto& channel : signal)
                channel[(size_t) i] = value;
        }

        return signal;
    }

    // 朴素（不限带）方波：采样点只有 ±amplitude，但插值后会有明显的采样点间过冲。
    inline Signal makeSquare (int numChannels, double sampleRate, double frequency, float amplitude, double seconds)
    {
        const auto numSamples = (int) (seconds * sampleRate);
        auto signal = makeSilence (numChannels, numSamples);
        const auto period = sampleRate / frequency;

        for (int i = 0; i < numSamples; ++i)
        {
            const auto value = std::fmod ((double) i, period) < period * 0.5 ? amplitude : -amplitude;

            for (auto& channel : signal)
                channel[(size_t) i] = value;
        }

        return signal;
    }

    inline Signal makeImpulse (int numChannels, int numSamples, int position, float amplitude)
    {
        auto signal = makeSilence (numChannels, numSamples);

        for (auto& channel : signal)
            channel[(size_t) position] = amplitude;

        return signal;
    }

    // 均匀白噪声；seed 固定，结果可复现。声道之间不相关。
    inline Signal makeNoise (int numChannels, int numSamples, float amplitude, juce::int64 seed)
    {
        juce::Random random (seed);
        auto signal = makeSilence (numChannels, numSamples);

        for (auto& channel : signal)
            for (auto& sample : channel)
                sample = amplitude * (random.nextFloat() * 2.0f - 1.0f);

        return signal;
    }

    // 20 ms 满幅 1 kHz 突发，前后静音。
    inline Signal makeBurst (int numChannels, double sampleRate, float amplitude, double seconds)
    {
        auto signal = makeSilence (numChannels, (int) (seconds * sampleRate));
        const auto start = (int) (0.1 * sampleRate);
        const auto length = (int) (0.02 * sampleRate);

        for (int i = 0; i < length; ++i)
        {
            const auto value = amplitude * (float) std::sin (juce::MathConstants<double>::twoPi * 1000.0 * i / sampleRate);

            for (auto& channel : signal)
                channel[(size_t) (start + i)] = value;
        }

        return signal;
    }

    inline void appendSilence (Signal& signal, int numSamples)
    {
        for (auto& channel : signal)
            channel.insert (channel.end(), (size_t) numSamples, 0.0f);
    }

    inline void scale (Signal& signal, float gain)
    {
        for (auto& channel : signal)
            for (auto& sample : channel)
                sample *= gain;
    }

    // 按指定块大小把整段信号送进限幅器，返回输出。
    inline Signal runLimiter (LimiterModule& limiter, const Signal& input, int blockSize)
    {
        const auto numChannels = (int) input.size();
        const auto numSamples = (int) input.front().size();
        auto output = input;

        juce::AudioBuffer<float> block (numChannels, blockSize);

        for (int start = 0; start < numSamples; start += blockSize)
        {
            const auto length = juce::jmin (blockSize, numSamples - start);
            block.setSize (numChannels, length, false, false, true);

            for (int ch = 0; ch < numChannels; ++ch)
                block.copyFrom (ch, 0, input[(size_t) ch].data() + start, length);

            limiter.process (block);

            for (int ch = 0; ch < numChannels; ++ch)
                std::copy (block.getReadPointer (ch), block.getReadPointer (ch) + length,
                           output[(size_t) ch].begin() + start);
        }

        return output;
    }

    // 用插件自己的 TruePeakDetector 量整段信号的真峰值（与界面上的表同一个算法）。
    inline float truePeakOf (const Signal& signal)
    {
        TruePeakDetector detector;
        detector.prepare ((int) signal.size());
        float peak = 0.0f;

        for (size_t ch = 0; ch < signal.size(); ++ch)
        {
            for (auto sample : signal[ch])
                peak = juce::jmax (peak, detector.processSample ((int) ch, sample));

            for (int i = 0; i < TruePeakDetector::tapsPerPhase; ++i)   // 把尾巴冲出来
                peak = juce::jmax (peak, detector.processSample ((int) ch, 0.0f));
        }

        return peak;
    }

    // 只量稳态：不补零、丢掉前 skip 个检测器输出（起始阶跃带来的真实过冲），返回 {最小, 最大}。
    // 检测器第 n 个输出只用到最近 tapsPerPhase 个样本，skip ≥ tapsPerPhase 就看不到起始沿。
    inline std::pair<float, float> steadyTruePeakRange (const Signal& signal, int skip = 2 * TruePeakDetector::tapsPerPhase)
    {
        TruePeakDetector detector;
        detector.prepare ((int) signal.size());
        float lowest = std::numeric_limits<float>::max(), highest = 0.0f;

        for (size_t ch = 0; ch < signal.size(); ++ch)
            for (size_t i = 0; i < signal[ch].size(); ++i)
            {
                const auto peak = detector.processSample ((int) ch, signal[ch][i]);

                if ((int) i >= skip)
                {
                    lowest = juce::jmin (lowest, peak);
                    highest = juce::jmax (highest, peak);
                }
            }

        return { lowest, highest };
    }

    // 独立的参考测量：16× 过采样、每侧 64 个样本的 Kaiser 窗 sinc（double 精度，系数表只算一次）。
    // 只在附近有足够大样本的位置插值，省时间；阈值取 minimumRelevant。
    inline double referenceTruePeakOf (const Signal& signal, double minimumRelevant)
    {
        constexpr int factor = 16;
        constexpr int halfTaps = 64;
        constexpr int numTaps = 2 * halfTaps;
        constexpr double beta = 10.0;

        static const auto table = []
        {
            auto besselI0 = [] (double x)
            {
                double sum = 1.0, term = 1.0;
                for (int k = 1; k < 64; ++k)
                {
                    const auto f = x * 0.5 / k;
                    term *= f * f;
                    sum += term;
                }
                return sum;
            };

            // table[p][j]：插值点 i + p/16 处，样本 i + k（k = j − halfTaps + 1）的权重。
            std::vector<std::array<double, (size_t) numTaps>> t ((size_t) factor);
            const auto i0Beta = besselI0 (beta);

            for (int p = 1; p < factor; ++p)
                for (int j = 0; j < numTaps; ++j)
                {
                    const auto k = j - halfTaps + 1;
                    const auto offset = (double) p / factor - k;
                    const auto x = juce::MathConstants<double>::pi * offset;
                    const auto r = offset / (double) halfTaps;
                    t[(size_t) p][(size_t) j] = std::sin (x) / x
                                              * besselI0 (beta * std::sqrt (juce::jmax (0.0, 1.0 - r * r))) / i0Beta;
                }

            return t;
        }();

        double peak = 0.0;

        for (const auto& channel : signal)
        {
            const auto n = (int) channel.size();

            for (int i = 0; i < n; ++i)
            {
                peak = juce::jmax (peak, (double) std::abs (channel[(size_t) i]));

                bool relevant = false;
                for (int k = juce::jmax (0, i - 2); k <= juce::jmin (n - 1, i + 3); ++k)
                    relevant = relevant || std::abs (channel[(size_t) k]) >= minimumRelevant;

                if (! relevant)
                    continue;

                for (int p = 1; p < factor; ++p)
                {
                    const auto& weights = table[(size_t) p];
                    double sum = 0.0;

                    for (int j = 0; j < numTaps; ++j)
                    {
                        const auto index = i + j - halfTaps + 1;
                        if (index >= 0 && index < n)
                            sum += channel[(size_t) index] * weights[(size_t) j];
                    }

                    peak = juce::jmax (peak, std::abs (sum));
                }
            }
        }

        return peak;
    }

    inline float maxAbsDifference (const Signal& a, const Signal& b, int offsetInB, int length)
    {
        float maxDiff = 0.0f;

        for (size_t ch = 0; ch < a.size(); ++ch)
            for (int i = 0; i < length; ++i)
                maxDiff = juce::jmax (maxDiff, std::abs (a[ch][(size_t) i] - b[ch][(size_t) (i + offsetInB)]));

        return maxDiff;
    }

    inline bool allFinite (const Signal& signal)
    {
        for (const auto& channel : signal)
            for (auto sample : channel)
                if (! std::isfinite (sample))
                    return false;

        return true;
    }

    inline float toDb (double linear)
    {
        return (float) juce::Decibels::gainToDecibels (linear, -200.0);
    }
}

//==============================================================================
// v0.1.7.0 T-012 阶段 4 追加（审核 V3-01）：中文子串断言。
#include <string>

namespace celadon::test
{
    // 为什么不用 juce::String::contains / startsWith / indexOf：JUCE 9.0.2 里它们对 UTF-8 needle 只比 needle.length()
    // （码点数）个「字节」——CharacterFunctions::compareUpTo 的 char 快路直接 std::strncmp (s1, s2, maxChars)，传进去的却是
    // 码点数（juce_String.cpp indexOf / startsWith → juce_CharacterFunctions.h compareUpTo）。三字节汉字的 needle 只比前 1/3
    // 的字节：「自动：」只比「自」、「已锁定」只比「已」，文案写错也拦不住（只会多报命中、不会漏报，所以原断言照样通过）。
    // 测试里凡是含中文的 needle 一律用下面两个按字节精确比较的版本（std::string::find / rfind (…, 0)）；ASCII needle 不受影响。
    // endsWith、operator==、containsChar 不走那条快路，照常可用。
    inline bool utf8Contains (const juce::String& text, const char* needle)
    {
        return text.toStdString().find (needle) != std::string::npos;
    }

    inline bool utf8Contains (const juce::String& text, const juce::String& needle)
    {
        return text.toStdString().find (needle.toStdString()) != std::string::npos;
    }

    inline bool utf8StartsWith (const juce::String& text, const char* needle)
    {
        return text.toStdString().rfind (needle, 0) == 0;
    }

    inline bool utf8StartsWith (const juce::String& text, const juce::String& needle)
    {
        return text.toStdString().rfind (needle.toStdString(), 0) == 0;
    }
}

/*
  ==============================================================================

    TestHelpers.h
    自动测试的公共工具：造测试信号、分块跑模块、独立的高精度真峰值测量。

  ==============================================================================
*/

#pragma once

#include <juce_dsp/juce_dsp.h>

#include "../Source/dsp/GlazeConstants.h"
#include "../Source/dsp/LimiterModule.h"
#include "../Source/dsp/TruePeak.h"

#include <array>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace celadon::test
{
    // 保护层机制测试（退让、卡住、吸收、冻结、搬出 carry……）用的限幅预算：固定成 0.1.8.x 的出厂值（常数版本 8：温润 1.0 / 2.5 / 4.0、
    // 冲击 0.8 / 2.0 / 3.0、通透 1.5 / 3.5 / 6.0 dB 均值 / P95 / 峰值）。这些用例靠「推热的素材超出预算、保护层出手」走到要测的路径，
    // 测的是机制、不是出厂预算的数；T-045（ADR-029）把出厂预算三项各放宽 +1 / +2 / +3 dB 后，同一段素材不再超预算，路径就走不到了。
    // 出厂预算本身的效果由决策器测试（GlazePlannerTests 的前馈对实测、风格倚重）与真歌渲染验收。
    inline glaze::Constants withTightLimitBudgets (glaze::Constants c) noexcept
    {
        c.warm.limitGrMeanCapDb = 1.0f;   c.warm.limitGrP95CapDb = 2.5f;   c.warm.limitGrPeakCapDb = 4.0f;
        c.punchy.limitGrMeanCapDb = 0.8f; c.punchy.limitGrP95CapDb = 2.0f; c.punchy.limitGrPeakCapDb = 3.0f;
        c.clean.limitGrMeanCapDb = 1.5f;  c.clean.limitGrP95CapDb = 3.5f;  c.clean.limitGrPeakCapDb = 6.0f;
        return c;
    }

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

    // 只有失谐 pad 的立体声段落（BUG-009 的 House breakdown，照 tools/render/make_synthetic_house.py 的 pad_bar）：和弦 Am – F – C – G
    // （中音区三和弦）每个 chordSeconds 秒循环；每个音 6 个分音、幅度 amplitude / h，左声道频率 × 0.997、右声道 × 1.003（宽），
    // 相位偏移同脚本；和弦之间 60 ms 升余弦交叉淡化（边界前后各 30 ms，同脚本），开头 60 ms 淡入（不让第一帧带宽带的起振）。最低的分音是 F 的 174.6 Hz 与 G 的 196 Hz——都在修坯低频组
    // （40 ～ 125 Hz，段 5 的格子到 152 Hz）之上，低频组只收到它们漏进 125 Hz 段的部分；左右失谐让漏进来的相关度随约 1 Hz 的拍频摆动。
    inline Signal makeDetunedPad (double sampleRate, double seconds, double chordSeconds, double amplitude)
    {
        static constexpr double chords[4][3] = { { 220.0, 261.626, 329.628 }, { 174.614, 220.0, 261.626 },
                                                 { 261.626, 329.628, 391.995 }, { 195.998, 246.942, 293.665 } };
        constexpr double detune[2] = { 0.997, 1.003 };
        constexpr double fadeSeconds = 0.03;
        const auto twoPi = juce::MathConstants<double>::twoPi;
        const auto numSamples = (int) (seconds * sampleRate);
        auto signal = makeSilence (2, numSamples);

        // 第 s 个和弦段的包络：[s·C − x, s·C + x] 升、[(s + 1)·C − x, (s + 1)·C + x] 降（升余弦），中间 1；第 0 段从 0 起升。
        auto envelope = [&] (int s, double t)
        {
            const auto start = (double) s * chordSeconds, end = start + chordSeconds;
            const auto rise = s == 0 ? juce::jlimit (0.0, 1.0, t / (2.0 * fadeSeconds))
                                     : juce::jlimit (0.0, 1.0, (t - (start - fadeSeconds)) / (2.0 * fadeSeconds));
            const auto fall = juce::jlimit (0.0, 1.0, ((end + fadeSeconds) - t) / (2.0 * fadeSeconds));
            const auto ramp = juce::jmin (rise, fall);
            return 0.5 - 0.5 * std::cos (juce::MathConstants<double>::pi * ramp);
        };

        for (int i = 0; i < numSamples; ++i)
        {
            const auto t = (double) i / sampleRate;
            const auto current = (int) std::floor (t / chordSeconds);

            for (int s = juce::jmax (0, current - 1); s <= current + 1; ++s)
            {
                const auto gain = envelope (s, t);

                if (gain <= 0.0)
                    continue;

                for (int ch = 0; ch < 2; ++ch)
                {
                    double value = 0.0;

                    for (int voice = 0; voice < 3; ++voice)
                    {
                        const auto frequency = chords[s % 4][voice] * detune[ch];
                        const auto offset = 0.37 * (double) (voice + 1) + 1.7 * (double) ch;

                        for (int h = 1; h <= 6; ++h)
                            value += std::sin (twoPi * frequency * (double) h * t + offset * (double) h) / (double) h;
                    }

                    signal[(size_t) ch][(size_t) i] += (float) (amplitude * gain * value);
                }
            }
        }

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

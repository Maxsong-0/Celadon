/*
  ==============================================================================

    SpectrumAnalyserTests.cpp
    频谱仪计算（Source/ui/SpectrumAnalyser，v0.1.8.0）的自动测试：正弦读数的频率与幅度（0 dBFS 正弦读 0 dB）、
    远处的泄漏足够低、RMS 读数、宿主停了之后曲线落下、高采样率换更长的 FFT 仍读准；
    OUT 换来源（restartOutput）之后峰值线与平均线不带着前一种信号，攒满一个窗口后从新信号起步、IN 不受影响。

    T-032（2026-09-30）：FFT 长度封顶 16384（R9-11）；IN 的频谱可以关（界面 IN 虚线关着时）：OUT 逐位不受影响、IN 留在底线、
    RMS 照算；重新打开后下一帧直接取测量值。

    T-036（2026-10-01，频谱仪更顺滑、响应更快）：算法按设计变了，T-032 那条「与改前逐点算式逐位相同」的回归保护作废，换成性质：
      - 曲线形状：bin 之间在幅度上做三次插值，正好落在 bin 上的正弦整条测量曲线不高于那个 bin（不过冲）、峰两侧单调；
        正弦读数取它附近几个像素里的最高处（峰落在哪个像素取决于它离哪个 bin 近，用户看到的是峰顶）；
      - 动态按时间算：同一段音频按 30 帧与 120 帧推，升 / 落 / 平均线的走向一致；
      - 两块音频之间的帧不掉（宿主 512 样本块、界面 120 帧时，原来没有新样本的那一帧落 0.8 dB）；宿主停够 0.25 s 才落；
      - 峰值线保持 1.5 s 后按 12 dB/s 落；RMS 是「上次取走以来」的全部样本。

  ==============================================================================
*/

#include "TestHelpers.h"

#include "../Source/ui/SpectrumAnalyser.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace celadon::test
{
    class SpectrumAnalyserTests  : public juce::UnitTest
    {
    public:
        SpectrumAnalyserTests() : juce::UnitTest ("SpectrumAnalyser", "Celadon") {}

        using Analyser = ui::SpectrumAnalyser;
        using Stream = Analyser::Stream;

        // 四条曲线（OUT / IN 显示、平均、峰值）的逐位摘要（FNV-1a，64 位，按 float 的位模式算），只打进日志：
        // 同一台机器、同一编译器下改前改后对照（FFT 引擎不同的平台之间摘要本来就不同）。
        static juce::String curveDigest (const Analyser& analyser)
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

            mix (analyser.getDisplayDb (Stream::output));
            mix (analyser.getDisplayDb (Stream::input));
            mix (analyser.getAverageDb());
            mix (analyser.getPeakDb());
            return juce::String::toHexString ((juce::int64) hash);
        }

        // 确定性的宽带测试信号：三个正弦（97 Hz / 1 kHz / 6.1 kHz）+ 固定种子的白噪，让所有点都离开底线。
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

        // 一个正弦（相位跨块连续）；amplitude = 0 就是静音。
        struct Tone
        {
            Tone (double frequency, double sampleRate, double amplitudeToUse)
                : step (juce::MathConstants<double>::twoPi * frequency / sampleRate), amplitude (amplitudeToUse) {}

            void fill (std::vector<float>& block)
            {
                for (auto& sample : block)
                {
                    sample = (float) (amplitude * std::sin (phase));
                    phase += step;
                }
            }

            double step, amplitude, phase = 0.0;
        };

        // 按帧率推 seconds 秒：每帧推进到期的样本（采样率 / 帧率，小数部分攒到下一帧），再 update (1 / 帧率)。
        template <typename Source>
        static void play (Analyser& analyser, Source& source, double sampleRate, double seconds, double fps, bool alsoInput = false)
        {
            const auto frames = (int) std::lround (seconds * fps);
            std::vector<float> block;
            double owed = 0.0;

            for (int frame = 0; frame < frames; ++frame)
            {
                owed += sampleRate / fps;
                const auto due = (int) owed;
                owed -= (double) due;
                block.resize ((size_t) due);
                source.fill (block);
                analyser.push (Stream::output, block.data(), due);

                if (alsoInput)
                    analyser.push (Stream::input, block.data(), due);

                analyser.update (1.0 / fps);
            }
        }

        static int indexOf (float frequency) noexcept
        {
            return (int) std::lround (Analyser::positionOf (frequency) * (float) (Analyser::numPoints - 1));
        }

        // 显示曲线在某频率附近（±2 个点，约 ±1 % 频率）的最高处：屏幕上那个峰的高度。
        static float peakNear (const Analyser& analyser, Stream stream, float frequency)
        {
            const auto centre = indexOf (frequency);
            auto best = Analyser::floorDb;

            for (int i = juce::jmax (0, centre - 2); i <= juce::jmin (Analyser::numPoints - 1, centre + 2); ++i)
                best = juce::jmax (best, analyser.getDisplayDb (stream)[(size_t) i]);

            return best;
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

        // 参照：对最近 fftSize 个样本（最旧的在前）照同一套 Hann 窗与相干增益做 FFT，返回各 bin 的线性幅度（0 dBFS 正弦 = 1）。
        static std::vector<double> referenceBinMagnitudes (const std::vector<float>& recent, int fftOrder)
        {
            const auto fftSize = 1 << fftOrder;
            juce::dsp::FFT fft (fftOrder);
            std::vector<float> window ((size_t) fftSize), buffer ((size_t) (2 * fftSize), 0.0f);
            double sum = 0.0;

            for (int i = 0; i < fftSize; ++i)
            {
                window[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) fftSize);
                sum += (double) window[(size_t) i];
            }

            for (int i = 0; i < fftSize; ++i)
                buffer[(size_t) i] = recent[(size_t) i] * window[(size_t) i];

            fft.performFrequencyOnlyForwardTransform (buffer.data(), true);
            std::vector<double> magnitudes ((size_t) (fftSize / 2 + 1));

            for (size_t bin = 0; bin < magnitudes.size(); ++bin)
                magnitudes[bin] = std::abs ((double) buffer[bin]) * 2.0 / sum;

            return magnitudes;
        }

        // T-036 之前（480 点）的高频读数口径：每点 [f · 2^−h, f · 2^+h]（h = 480 点点距的一半）里 floor ～ ceil 的 bin 取最大（dB）。
        // 噪声类内容「取最大」的读数随窗口里的 bin 数变；T-036 把点加密到 1033 后，取最大的窗口仍按这个宽度，读数与改前同口径。
        static float legacyPeakDb (const std::vector<double>& magnitudes, double sampleRate, int fftSize, float frequency)
        {
            const auto binWidth = sampleRate / (double) fftSize;
            const auto lastBin = fftSize / 2;
            const auto halfStep = 0.5 * std::log2 ((double) Analyser::maxFrequency / (double) Analyser::minFrequency) / 479.0;
            const auto first = juce::jlimit (0, lastBin, (int) std::floor ((double) frequency * std::pow (2.0, -halfStep) / binWidth));
            const auto last = juce::jlimit (first, lastBin, (int) std::ceil ((double) frequency * std::pow (2.0, halfStep) / binWidth));
            double magnitude = 0.0;

            for (int bin = first; bin <= last; ++bin)
                magnitude = juce::jmax (magnitude, magnitudes[(size_t) bin]);

            return magnitude > 0.0 ? (float) (20.0 * std::log10 (magnitude)) : Analyser::floorDb;
        }

        void runTest() override
        {
            beginTest ("FFT length is 8192 up to 50 kHz and 16384 above (capped at 16384 for 176.4 / 192 kHz)");
            {
                Analyser analyser;

                for (const auto& [sampleRate, size] : { std::pair<double, int> { 44100.0, 8192 }, { 48000.0, 8192 }, { 88200.0, 16384 },
                                                        { 96000.0, 16384 }, { 176400.0, 16384 }, { 192000.0, 16384 } })
                {
                    analyser.setSampleRate (sampleRate);
                    expectEquals (analyser.getFftSize(), size, "@" + juce::String (sampleRate, 0));
                }
            }

            beginTest ("frequency axis is 20 Hz to 20 kHz, logarithmic, one point per logical pixel");
            {
                expectEquals (Analyser::numPoints, 1033);
                expect (std::abs (Analyser::frequencyAt (0) - 20.0f) < 1.0e-3f);
                expect (std::abs (Analyser::frequencyAt (Analyser::numPoints - 1) - 20000.0f) < 0.5f);
                expect (std::abs (Analyser::positionOf (632.456f) - 0.5f) < 1.0e-3f, "geometric centre");
            }

            // 正弦读数：低频（bin 之间插值）、中频、高频（点区间取最大）三处，三种采样率。
            for (const auto sampleRate : { 44100.0, 48000.0, 96000.0, 192000.0 })
            {
                for (const auto frequency : { 50.0f, 1000.0f, 10000.0f })
                {
                    beginTest ("a -6 dBFS " + juce::String (frequency, 0) + " Hz sine reads -6 dB @" + juce::String (sampleRate, 0));

                    Analyser analyser;
                    analyser.setSampleRate (sampleRate);
                    Tone tone (frequency, sampleRate, 0.5);
                    play (analyser, tone, sampleRate, 2.0, 30.0);

                    const auto atTone = peakNear (analyser, Stream::output, frequency);
                    const auto below = analyser.displayDbAt (Stream::output, frequency / 10.0f);
                    const auto above = analyser.displayDbAt (Stream::output, juce::jmin (19000.0f, frequency * 10.0f));
                    logMessage ("  " + juce::String (frequency, 0) + " Hz reads " + juce::String (atTone, 2) + " dB; a decade below "
                                + juce::String (below, 1) + " dB, above " + juce::String (above, 1) + " dB");
                    // Hann 窗的扇贝损失：正弦落在两个 bin 之间最多低 1.42 dB。
                    expect (std::abs (atTone + 6.0f) < 1.5f, "tone level: " + juce::String (atTone, 2));
                    // 低于 20 Hz 的读数钳在 20 Hz 点上：50 Hz 的「低十倍」离它太近（Hann 旁瓣），只看上面。
                    if (frequency / 10.0f >= Analyser::minFrequency)
                        expect (below < -60.0f, "leakage a decade below: " + juce::String (below, 1));

                    if (frequency * 10.0f <= 19000.0f)
                        expect (above < -60.0f, "leakage a decade above: " + juce::String (above, 1));

                    expect (std::abs (analyser.takeRecentRmsDb (Stream::output) + 9.03f) < 0.1f, "RMS of a 0.5 sine is -9 dBFS");
                    expectEquals (analyser.takeRecentRmsDb (Stream::output), Analyser::floorDb, "RMS was taken: nothing new since");
                    expect (analyser.displayDbAt (Stream::input, frequency) <= Analyser::floorDb + 0.01f, "the other stream stays empty");
                    expect (analyser.getPeakDb()[(size_t) indexOf (frequency)] >= analyser.getDisplayDb (Stream::output)[(size_t) indexOf (frequency)] - 1.0e-3f,
                            "the peak line is never below the display");
                }
            }

            // 曲线形状：正好落在第 9 个 bin 上的正弦（48 kHz / 8192 点：52.734375 Hz）。Hann 窗下它占 bin 8 ～ 10（−6 / 0 / −6 dB），
            // bin 7、11 是零点。幅度上的三次插值：整条测量曲线不高于峰 bin（不过冲）；峰往两边到零点之前单调（不起伏）。
            beginTest ("the curve between bins is monotone and never overshoots the bins (low-frequency interpolation)");
            {
                constexpr double sampleRate = 48000.0;
                const auto binWidth = sampleRate / 8192.0;
                const auto frequency = (float) (9.0 * binWidth);

                Analyser analyser;
                analyser.setSampleRate (sampleRate);
                Tone tone (frequency, sampleRate, 0.5);
                play (analyser, tone, sampleRate, 1.0, 30.0);

                const auto& measured = analyser.getMeasuredDb (Stream::output);
                const auto peakIndex = indexOf (frequency);
                auto highest = Analyser::floorDb;

                for (const auto value : measured)
                    highest = juce::jmax (highest, value);

                logMessage ("  bin-centred " + juce::String (frequency, 3) + " Hz: highest point " + juce::String (highest, 3)
                            + " dB, at the tone " + juce::String (measured[(size_t) peakIndex], 3) + " dB");
                expect (highest <= -6.0f + 0.02f, "no overshoot above the -6 dB bin: " + juce::String (highest, 3));
                expect (std::abs (measured[(size_t) peakIndex] + 6.0f) < 0.05f, "the tone point sits on the bin");

                int risingBreaks = 0, fallingBreaks = 0;

                for (auto i = indexOf ((float) (7.2 * binWidth)); i < peakIndex; ++i)
                    if (measured[(size_t) i + 1] < measured[(size_t) i] - 1.0e-4f)
                        ++risingBreaks;

                for (auto i = peakIndex; i < indexOf ((float) (10.8 * binWidth)); ++i)
                    if (measured[(size_t) i + 1] > measured[(size_t) i] + 1.0e-4f)
                        ++fallingBreaks;

                expectEquals (risingBreaks, 0, "rises monotonically towards the peak");
                expectEquals (fallingBreaks, 0, "falls monotonically after the peak");
            }

            // 插值的两条关键性质（评审 R-08）：随机信号（多正弦、强弱相邻正弦、噪声、正弦 + 直流、脉冲）× 四种采样率，逐点核
            //   ① 不低于两端较小者：点值 ≥ min (bin k, bin k + 1)（k = 点的中心频率所在的 bin）——在幅度上用不限幅的 Catmull-Rom，
            //      强峰旁会冲到 0 以下、画出假的深缺口，这一条抓得到；
            //   ② 低频（取最大窗口比一个 bin 窄，点值只由插值决定）隆起有限：点值 ≤ max (bin k, bin k + 1) + 0.65 dB——夹在两个 bin
            //      之间的峰本来就比两个 bin 都高（Hann 扇贝损失最多 1.42 dB），圆包允许越过一点，但不能甩出假峰。
            for (const auto sampleRate : { 44100.0, 48000.0, 96000.0, 192000.0 })
            {
                beginTest ("interpolation never dips below the lower bin and barely bulges above the higher one @" + juce::String (sampleRate, 0));

                Analyser analyser;
                analyser.setSampleRate (sampleRate);
                const auto fftSize = analyser.getFftSize();
                const auto fftOrder = fftSize == 8192 ? 13 : 14;
                const auto binWidth = sampleRate / (double) fftSize;
                juce::Random random (20261001 + (int) sampleRate);
                int dips = 0, bulges = 0, checked = 0;
                float worstBulgeDb = 0.0f;

                for (int signal = 0; signal < 40; ++signal)
                {
                    std::vector<float> samples ((size_t) fftSize + 333, 0.0f);
                    const auto kind = signal % 5;

                    for (size_t n = 0; n < samples.size(); ++n)
                    {
                        const auto t = (double) n / sampleRate;
                        double value = 0.0;

                        if (kind == 0 || kind == 2)   // 几个随机正弦（kind 2 再加噪声）
                            for (int k = 0; k < 6; ++k)
                                value += (0.05 + 0.1 * (double) ((signal * 7 + k * 13) % 10) / 10.0)
                                         * std::sin (juce::MathConstants<double>::twoPi * (25.0 + (double) ((signal * 131 + k * 977) % 3000)) * t);

                        if (kind == 1)                 // 强弱相邻：0 dBFS 与 −80 dBFS 两个正弦隔 3 个 bin
                        {
                            const auto f = 40.0 + 7.3 * (double) signal;
                            value = std::sin (juce::MathConstants<double>::twoPi * f * t)
                                    + 1.0e-4 * std::sin (juce::MathConstants<double>::twoPi * (f + 3.0 * binWidth) * t);
                        }

                        if (kind == 2)
                            value += 0.01 * (double) (random.nextFloat() * 2.0f - 1.0f);

                        if (kind == 3)                 // 正弦 + 直流
                            value = 0.3 + 0.5 * std::sin (juce::MathConstants<double>::twoPi * (60.0 + (double) signal) * t);

                        if (kind == 4)                 // 一个脉冲
                            value = n == samples.size() / 2 + (size_t) signal ? 1.0 : 0.0;

                        samples[n] = (float) value;
                    }

                    analyser.reset();
                    analyser.push (Stream::output, samples.data(), (int) samples.size());
                    analyser.update (1.0 / 60.0);

                    const std::vector<float> recent (samples.end() - fftSize, samples.end());
                    const auto bins = referenceBinMagnitudes (recent, fftOrder);
                    const auto& measured = analyser.getMeasuredDb (Stream::output);

                    for (int i = 0; i < Analyser::numPoints; ++i)
                    {
                        const auto f = (double) Analyser::frequencyAt (i);
                        const auto k = juce::jlimit (0, fftSize / 2 - 1, (int) std::floor (f / binWidth));
                        const auto lower = juce::jmin (bins[(size_t) k], bins[(size_t) k + 1]);
                        const auto higher = juce::jmax (bins[(size_t) k], bins[(size_t) k + 1]);
                        const auto value = std::pow (10.0, (double) measured[(size_t) i] / 20.0);

                        if (lower <= 1.0e-6)   // 底线（−120 dB）以下不比
                            continue;

                        ++checked;

                        if (value < lower * (1.0 - 1.0e-3))
                            ++dips;

                        // 低频：取最大的窗口比一个 bin 窄（480 点点距的整宽 < 1 个 bin），点值只由插值决定。
                        const auto windowBins = f * (std::pow (2.0, 0.5 * std::log2 (1000.0) / 479.0) - std::pow (2.0, -0.5 * std::log2 (1000.0) / 479.0)) / binWidth;

                        if (windowBins < 1.0)
                        {
                            const auto bulge = (float) (20.0 * std::log10 (value / higher));
                            worstBulgeDb = juce::jmax (worstBulgeDb, bulge);

                            if (bulge > 0.65f)
                                ++bulges;
                        }
                    }
                }

                logMessage ("  " + juce::String (checked) + " points checked: " + juce::String (dips) + " dips below the lower bin, "
                            + juce::String (bulges) + " bulges over 0.65 dB (worst " + juce::String (worstBulgeDb, 3) + " dB)");
                expect (checked > 10000, "the check covers enough points");
                expectEquals (dips, 0, "never below the lower of the two bins");
                expectEquals (bulges, 0, "low-frequency bulge at most 0.65 dB");
            }

            // 高频读数口径（T-036）：白噪在 2 ～ 16 kHz 的读数与改前（480 点）同口径——取最大窗口跟着点距缩窄的话，这里会低 5 ～ 8 dB。
            for (const auto sampleRate : { 44100.0, 48000.0, 96000.0 })
            {
                beginTest ("white noise reads the same at high frequencies as before T-036 @" + juce::String (sampleRate, 0));

                Analyser analyser;
                analyser.setSampleRate (sampleRate);
                const auto fftSize = analyser.getFftSize();
                std::vector<float> noise ((size_t) fftSize + 777);
                juce::Random random (20261001);

                for (auto& sample : noise)
                    sample = 0.25f * (random.nextFloat() * 2.0f - 1.0f);

                analyser.push (Stream::output, noise.data(), (int) noise.size());
                analyser.update (1.0 / 60.0);

                const std::vector<float> recent (noise.end() - fftSize, noise.end());
                const auto bins = referenceBinMagnitudes (recent, fftSize == 8192 ? 13 : 14);
                double difference = 0.0;
                int count = 0;

                for (int i = indexOf (2000.0f); i <= indexOf (16000.0f); i += 3)
                {
                    difference += (double) (analyser.getMeasuredDb (Stream::output)[(size_t) i]
                                            - legacyPeakDb (bins, sampleRate, fftSize, Analyser::frequencyAt (i)));
                    ++count;
                }

                const auto meanDifference = (float) (difference / (double) count);
                logMessage ("  2 ~ 16 kHz white noise, new minus pre-T-036 reading: mean " + juce::String (meanDifference, 2) + " dB over "
                            + juce::String (count) + " points");
                expect (std::abs (meanDifference) < 1.5f, "same high-frequency level as before: " + juce::String (meanDifference, 2) + " dB");
            }

            // 动态按时间算：同一个测量目标，一帧 1 / 30 s 与四帧 1 / 120 s（后三帧没有新样本，继续靠近同一个测量值）走到同一处。
            // 一次推满一个窗口，测量值一步到位：比的只是动态本身（窗口装填过程中两种帧率看到的测量时刻本来就不同）。
            beginTest ("rise and fall follow time, not the frame rate");
            {
                constexpr double sampleRate = 48000.0;
                Analyser slow, fast;
                slow.setSampleRate (sampleRate);
                fast.setSampleRate (sampleRate);

                std::vector<float> sine (16384), zeros (16384, 0.0f);
                Tone (1000.0, sampleRate, 0.5).fill (sine);

                const auto step = [&] (const std::vector<float>& samples)
                {
                    for (auto* analyser : { &slow, &fast })
                        analyser->push (Stream::output, samples.data(), (int) samples.size());

                    slow.update (1.0 / 30.0);

                    for (int frame = 0; frame < 4; ++frame)
                        fast.update (1.0 / 120.0);

                    float worst = 0.0f;

                    for (size_t i = 0; i < (size_t) Analyser::numPoints; ++i)
                        worst = juce::jmax (worst, std::abs (slow.getDisplayDb (Stream::output)[i] - fast.getDisplayDb (Stream::output)[i]));

                    return worst;
                };

                const auto index = (size_t) indexOf (1000.0f);
                const auto rise = step (sine);
                const auto risen = slow.getDisplayDb (Stream::output)[index];
                const auto target = slow.getMeasuredDb (Stream::output)[index];
                const auto fall = step (zeros);
                logMessage ("  one 1/30 s frame vs four 1/120 s frames, worst difference over all points: rise " + juce::String (rise, 5)
                            + " dB (1 kHz at " + juce::String (risen, 2) + " of " + juce::String (target, 2) + " dB after 33 ms), fall "
                            + juce::String (fall, 5) + " dB");
                expect (rise < 0.01f, "rise: " + juce::String (rise, 5));
                expect (fall < 0.01f, "fall: " + juce::String (fall, 5));
                expect (risen > target - 0.04f * (target - Analyser::floorDb),
                        "33 ms after the step the display has covered more than 96 % of the way (rise 10 ms)");
            }

            // 平均线：同一段音频按 30 帧与 120 帧推（48 kHz：每帧 1600 / 400 个样本），走向一致；起音过程也接近。窗口装填的头 170 ms
            // 两种帧率看到的测量时刻不同、30 帧又按帧末的显示值积一整帧（前向欧拉），平均线早期会差约 1 dB，之后按 0.8 s 收拢。
            beginTest ("the average line and the rise while playing agree between 30 and 120 frames per second");
            {
                constexpr double sampleRate = 48000.0;
                Analyser slow, fast;
                slow.setSampleRate (sampleRate);
                fast.setSampleRate (sampleRate);
                Tone slowTone (1000.0, sampleRate, 0.5), fastTone (1000.0, sampleRate, 0.5);
                const auto index = (size_t) indexOf (1000.0f);
                float worstDisplay = 0.0f, worstAverage = 0.0f;

                for (int step = 0; step < 12; ++step)   // 12 × 100 ms 的正弦
                {
                    play (slow, slowTone, sampleRate, 0.1, 30.0);
                    play (fast, fastTone, sampleRate, 0.1, 120.0);
                    worstDisplay = juce::jmax (worstDisplay, std::abs (slow.getDisplayDb (Stream::output)[index] - fast.getDisplayDb (Stream::output)[index]));
                    worstAverage = juce::jmax (worstAverage, std::abs (slow.getAverageDb()[index] - fast.getAverageDb()[index]));
                }

                logMessage ("  30 vs 120 frames, worst difference at 1 kHz over 1.2 s: display " + juce::String (worstDisplay, 3)
                            + " dB, average " + juce::String (worstAverage, 3) + " dB");
                expect (std::abs (fast.getDisplayDb (Stream::output)[index] - fast.getMeasuredDb (Stream::output)[index]) < 0.01f,
                        "a steady tone: the display sits on the measurement");
                const auto finalAverage = std::abs (slow.getAverageDb()[index] - fast.getAverageDb()[index]);
                expect (worstDisplay < 1.5f, "display: " + juce::String (worstDisplay, 3));
                expect (worstAverage < 1.5f, "average: " + juce::String (worstAverage, 3));
                expect (finalAverage < 0.5f, "average after 1.2 s: " + juce::String (finalAverage, 3));
            }

            // 响应：起音 10 ms 量级。正弦开始后 0.2 s（窗口 171 ms 已装满）显示已在测量值上；原来 30 帧时升 0.6 / 帧，还差零点几 dB。
            beginTest ("the display reaches the measurement within a couple of frames");
            {
                constexpr double sampleRate = 48000.0;
                Analyser analyser;
                analyser.setSampleRate (sampleRate);
                Tone tone (1000.0, sampleRate, 0.5);
                play (analyser, tone, sampleRate, 0.2, 60.0);

                const auto index = (size_t) indexOf (1000.0f);
                const auto gap = analyser.getMeasuredDb (Stream::output)[index] - analyser.getDisplayDb (Stream::output)[index];
                logMessage ("  0.2 s after the tone starts (60 frames / s): display " + juce::String (gap, 3) + " dB below the measurement");
                expect (gap >= 0.0f && gap < 0.2f, "gap: " + juce::String (gap, 3));
            }

            // 宿主 512 样本一块（10.7 ms）、界面 120 帧（8.3 ms）：约每 5 帧有 1 帧没有新样本。那一帧不能往下掉。
            beginTest ("frames between two host blocks do not dip (512-sample blocks at 120 frames per second)");
            {
                constexpr double sampleRate = 48000.0;
                constexpr int blockSize = 512;
                Analyser analyser;
                analyser.setSampleRate (sampleRate);
                Tone tone (1000.0, sampleRate, 0.5);
                std::vector<float> block (blockSize);
                const auto index = (size_t) indexOf (1000.0f);
                std::int64_t pushed = 0;
                int framesWithoutSamples = 0;
                float lowest = 0.0f, highest = Analyser::floorDb;

                for (int frame = 1; frame <= 240; ++frame)   // 2 s
                {
                    const auto due = (std::int64_t) ((double) frame / 120.0 * sampleRate);
                    auto pushedThisFrame = false;

                    while (pushed + blockSize <= due)
                    {
                        tone.fill (block);
                        analyser.push (Stream::output, block.data(), blockSize);
                        pushed += blockSize;
                        pushedThisFrame = true;
                    }

                    analyser.update (1.0 / 120.0);

                    if (frame > 60)   // 头 0.5 s 让窗口装满、曲线升到位
                    {
                        framesWithoutSamples += pushedThisFrame ? 0 : 1;
                        lowest = juce::jmin (lowest, analyser.getDisplayDb (Stream::output)[index]);
                        highest = juce::jmax (highest, analyser.getDisplayDb (Stream::output)[index]);
                    }
                }

                logMessage ("  " + juce::String (framesWithoutSamples) + " of 180 frames had no new samples; 1 kHz stayed within "
                            + juce::String (highest - lowest, 4) + " dB");
                expect (framesWithoutSamples > 20, "the case is exercised");
                expect (highest - lowest < 0.05f, "no dips between blocks: " + juce::String (highest - lowest, 4));
            }

            // 宿主停了（不再送块）：0.25 s 之内曲线不动，之后按 24 dB/s 落；RMS 读底线。
            beginTest ("when the host stops sending audio the display holds for 0.25 s, then falls at 24 dB/s");
            {
                constexpr double sampleRate = 48000.0;
                Analyser analyser;
                analyser.setSampleRate (sampleRate);
                Tone tone (1000.0, sampleRate, 0.5);
                play (analyser, tone, sampleRate, 1.0, 60.0);
                const auto before = analyser.displayDbAt (Stream::output, 1000.0f);
                analyser.takeRecentRmsDb (Stream::output);

                for (int frame = 0; frame < 24; ++frame)   // 0.2 s，没有样本
                    analyser.update (1.0 / 120.0);

                const auto held = analyser.displayDbAt (Stream::output, 1000.0f);

                for (int frame = 0; frame < 36; ++frame)   // 再 0.3 s：越过 0.25 s 之后约 0.25 s 在落
                    analyser.update (1.0 / 120.0);

                const auto fallen = analyser.displayDbAt (Stream::output, 1000.0f);
                logMessage ("  " + juce::String (before, 2) + " dB → after 0.2 s " + juce::String (held, 2) + " dB → after 0.5 s " + juce::String (fallen, 2) + " dB");
                expect (std::abs (held - before) < 0.01f, "holds while the host may just be between blocks");
                expect (std::abs ((before - fallen) - 24.0f * 0.25f) < 0.6f, "then falls at 24 dB/s: " + juce::String (before - fallen, 2));
                expectEquals (analyser.takeRecentRmsDb (Stream::output), Analyser::floorDb, "no samples, no RMS");
            }

            beginTest ("the peak line holds for 1.5 s, then falls at 12 dB/s");
            {
                constexpr double sampleRate = 48000.0;
                Analyser analyser;
                analyser.setSampleRate (sampleRate);
                Tone tone (1000.0, sampleRate, 0.5), silence (1000.0, sampleRate, 0.0);
                play (analyser, tone, sampleRate, 1.0, 30.0);

                const auto index = (size_t) indexOf (1000.0f);
                const auto loud = analyser.getPeakDb()[index];
                play (analyser, silence, sampleRate, 1.4, 30.0);
                const auto held = analyser.getPeakDb()[index];
                play (analyser, silence, sampleRate, 0.6, 30.0);
                const auto fallen = analyser.getPeakDb()[index];

                logMessage ("  peak " + juce::String (loud, 2) + " dB → 1.4 s later " + juce::String (held, 2) + " dB → 2.0 s later " + juce::String (fallen, 2) + " dB");
                expect (std::abs (held - loud) < 1.0e-3f, "held for 1.4 s");
                expect (std::abs ((loud - fallen) - 12.0f * 0.5f) < 0.6f, "then 12 dB/s: fell " + juce::String (loud - fallen, 2) + " dB in ~0.5 s");
                expect (analyser.getDisplayDb (Stream::output)[index] < fallen, "the display fell faster than the peak line");
            }

            beginTest ("RMS covers every sample pushed since it was last taken, across several frames");
            {
                Analyser analyser;
                std::vector<float> loud (400, 0.5f), quiet (1200, 0.0f);
                analyser.push (Stream::output, loud.data(), (int) loud.size());
                analyser.update (1.0 / 120.0);
                analyser.push (Stream::output, quiet.data(), (int) quiet.size());
                analyser.update (1.0 / 120.0);

                // 400 个 0.5 + 1200 个 0：均方 = 0.25 × 400 / 1600 = 0.0625 → −12.04 dBFS。
                const auto rms = analyser.takeRecentRmsDb (Stream::output);
                expect (std::abs (rms + 12.04f) < 0.01f, "RMS over both frames: " + juce::String (rms, 3));
                expectEquals (analyser.takeRecentRmsDb (Stream::input), Analyser::floorDb, "the other stream got nothing");
            }

            beginTest ("odd frame durations are clamped: NaN and negative do not move the curves, a long pause counts as 0.25 s");
            {
                constexpr double sampleRate = 48000.0;
                Analyser analyser;
                analyser.setSampleRate (sampleRate);
                Tone tone (1000.0, sampleRate, 0.5);
                play (analyser, tone, sampleRate, 1.0, 60.0);
                const auto before = analyser.getDisplayDb (Stream::output);
                const auto index = (size_t) indexOf (1000.0f);

                analyser.update (std::numeric_limits<double>::quiet_NaN());
                analyser.update (-1.0);
                expectEquals (countBitMismatches (analyser.getDisplayDb (Stream::output), before), 0, "no movement");

                analyser.update (10.0);   // 当 0.25 s：正好到「宿主停了」的边上，还没开始落
                const auto atEdge = analyser.getDisplayDb (Stream::output)[index];
                expect (std::abs (atEdge - before[index]) < 0.01f, "held: " + juce::String (atEdge - before[index], 4));
                analyser.update (10.0);   // 再 0.25 s：落 24 × 0.25 = 6 dB
                const auto dropped = before[index] - analyser.getDisplayDb (Stream::output)[index];
                expect (std::abs (dropped - 6.0f) < 0.01f, "fell " + juce::String (dropped, 4) + " dB");
            }

            // 四条曲线的逐位摘要只打进日志（FFT 引擎不同的平台之间本来就不同，没法写死）：改前改后在同一台机器上对照。
            // T-036 起按设计与 T-032 的记录（48 k 2ce1118a1553e982 等）不同。
            for (const auto sampleRate : { 48000.0, 96000.0, 192000.0 })
            {
                beginTest ("curve digest @" + juce::String (sampleRate, 0) + " (bit-exact record, see the log)");

                Analyser analyser;
                analyser.setSampleRate (sampleRate);
                Broadband source (sampleRate);
                play (analyser, source, sampleRate, 40.0 / 30.0, 30.0, true);

                for (int frame = 0; frame < 5; ++frame)
                    analyser.update (1.0 / 30.0);

                logMessage ("  digest " + curveDigest (analyser) + ", 1 kHz " + juce::String (analyser.displayDbAt (Stream::output, 1000.0f), 4)
                            + " dB, 97 Hz " + juce::String (analyser.displayDbAt (Stream::output, 97.0f), 4) + " dB");
                expect (analyser.displayDbAt (Stream::output, 1000.0f) > -40.0f, "the broadband signal is visible");
                expect (analyser.displayDbAt (Stream::input, 1000.0f) > -40.0f, "on both streams");

                int aboveFloor = 0;

                for (const auto value : analyser.getMeasuredDb (Stream::output))
                    if (value > Analyser::floorDb + 20.0f)
                        ++aboveFloor;

                expectEquals (aboveFloor, Analyser::numPoints, "every point carries signal");
            }

            beginTest ("restartOutput drops the previous signal's peak and average lines and leaves IN alone");
            {
                constexpr double sampleRate = 48000.0;
                Analyser analyser;
                analyser.setSampleRate (sampleRate);
                Tone loud (1000.0, sampleRate, 0.5), quiet (1000.0, sampleRate, 0.005);

                const auto index = (size_t) indexOf (1000.0f);
                play (analyser, loud, sampleRate, 40.0 / 30.0, 30.0, true);   // −6 dBFS
                const auto loudPeak = analyser.getPeakDb()[index];
                const auto inputBefore = analyser.displayDbAt (Stream::input, 1000.0f);

                analyser.restartOutput();
                expect (analyser.isRestartingOutput());
                expectEquals (analyser.getPeakDb()[index], Analyser::floorDb, "peak line cleared");
                expectEquals (analyser.displayDbAt (Stream::output, 1000.0f), Analyser::floorDb, "OUT display cleared");
                expectEquals (analyser.getMeasuredDb (Stream::output)[index], Analyser::floorDb, "OUT measurement cleared");
                expect (std::abs (analyser.displayDbAt (Stream::input, 1000.0f) - inputBefore) < 1.0e-6f, "IN untouched");

                // 新信号轻 40 dB：攒满一个 8192 点窗口之前两条线留在底线；之后从新信号的读数起步。
                play (analyser, quiet, sampleRate, 3.0 / 30.0, 30.0, true);
                expect (analyser.isRestartingOutput(), "still collecting the first full window");
                expectEquals (analyser.getPeakDb()[index], Analyser::floorDb);

                play (analyser, quiet, sampleRate, 10.0 / 30.0, 30.0, true);
                expect (! analyser.isRestartingOutput(), "restart completes once the window is full");
                const auto quietPeak = analyser.getPeakDb()[index];
                logMessage ("  1 kHz peak line: " + juce::String (loudPeak, 1) + " dB before, " + juce::String (quietPeak, 1) + " dB after the restart");
                expect (quietPeak < loudPeak - 30.0f, "the peak line no longer holds the previous signal");
                expect (std::abs (quietPeak + 46.0f) < 3.0f, "the peak line starts from the new signal: " + juce::String (quietPeak, 1));
                expect (analyser.getAverageDb()[index] < loudPeak - 30.0f, "the average line starts from the new signal");
            }

            beginTest ("switching the IN spectrum off leaves OUT bit-exact and IN at the floor; switching it back on starts from the measurement");
            {
                constexpr double sampleRate = 48000.0;
                Analyser reference, skipping;
                reference.setSampleRate (sampleRate);
                skipping.setSampleRate (sampleRate);

                std::vector<float> block (1600);
                Broadband source (sampleRate);
                const auto floorCurve = std::vector<float> ((size_t) Analyser::numPoints, Analyser::floorDb);

                const auto feed = [&] (int frames)
                {
                    for (int frame = 0; frame < frames; ++frame)
                    {
                        source.fill (block);

                        for (auto* analyser : { &reference, &skipping })
                        {
                            analyser->push (Stream::input, block.data(), (int) block.size());
                            analyser->push (Stream::output, block.data(), (int) block.size());
                            analyser->update (1.0 / 30.0);
                        }
                    }
                };

                feed (6);
                expect (skipping.displayDbAt (Stream::input, 1000.0f) > -40.0f, "IN is analysed by default");

                skipping.setInputSpectrumEnabled (false);
                expect (! skipping.isInputSpectrumEnabled() && ! skipping.hasInputSpectrum());
                expectEquals (countBitMismatches (skipping.getDisplayDb (Stream::input), floorCurve), 0, "switching off drops the IN curve to the floor at once");

                reference.takeRecentRmsDb (Stream::input);
                skipping.takeRecentRmsDb (Stream::input);
                feed (14);
                expectEquals (countBitMismatches (skipping.getDisplayDb (Stream::output), reference.getDisplayDb (Stream::output)), 0, "OUT display untouched");
                expectEquals (countBitMismatches (skipping.getAverageDb(), reference.getAverageDb()), 0, "average line untouched");
                expectEquals (countBitMismatches (skipping.getPeakDb(), reference.getPeakDb()), 0, "peak line untouched");
                expectEquals (countBitMismatches (skipping.getDisplayDb (Stream::input), floorCurve), 0, "IN stays at the floor while switched off");

                const auto referenceRms = reference.takeRecentRmsDb (Stream::input), skippingRms = skipping.takeRecentRmsDb (Stream::input);
                expect (bitsOf (skippingRms) == bitsOf (referenceRms) && skippingRms > -40.0f, "IN RMS still measured");

                skipping.setInputSpectrumEnabled (true);
                expect (skipping.isInputSpectrumEnabled() && ! skipping.hasInputSpectrum(), "no IN curve until the next analysis");

                // 没有新样本的一帧不算「分析过」：IN 仍在底线、仍然没有曲线。
                reference.update (1.0 / 30.0);
                skipping.update (1.0 / 30.0);
                expect (! skipping.hasInputSpectrum());
                expectEquals (skipping.displayDbAt (Stream::input, 1000.0f), Analyser::floorDb);

                feed (1);
                expect (skipping.hasInputSpectrum());
                expectEquals (countBitMismatches (skipping.getDisplayDb (Stream::input), skipping.getMeasuredDb (Stream::input)), 0,
                              "the first frame after switching back on shows the measurement itself");
                expectEquals (countBitMismatches (skipping.getMeasuredDb (Stream::input), reference.getMeasuredDb (Stream::input)), 0,
                              "and that measurement is the same as an analyser that never switched off");
                expectEquals (countBitMismatches (skipping.getDisplayDb (Stream::output), reference.getDisplayDb (Stream::output)), 0, "OUT still untouched");
            }
        }
    };

    static SpectrumAnalyserTests spectrumAnalyserTests;
}

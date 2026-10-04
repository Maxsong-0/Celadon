/*
  ==============================================================================

    MusicAnalyzerTests.cpp
    v0.6 GLAZE A1：分析器（MusicAnalyzer）与它读的两处数据源——LoudnessMeter 的 100 ms 子块环形缓冲、
    Balance 的每帧原始段功率快照——的自动测试（04-协调/2026-09-25-v0.6-GLAZE实现计划 阶段 1）。

    依据：设计规格-GLAZE-分析器全文；设计规格-GLAZE-时间线与保存 第 3 节（跳播 / 回绕 / reset 的分析器一侧）；
    MusicAnalyzer.h / LoudnessMeter.h / BalanceModule.h 的接口注释。只用公开接口。
    期望值尽量取可精确算出的：BS.1770-4 标准系数表（48 kHz）给出的 K 加权响度、EBU Tech 3341 / 3342 的构造、
    直接从表的子块按 EBU 定义（排序、无直方图）算出的参照 I 与 LRA；容差都写明来由。

    表的读数与 Balance 的输出 / G 在改动前后逐位相同，另有前后对照（IMPL-A1「第 2、3 步」）与各自原有的测试类。

  ==============================================================================
*/

#include "TestHelpers.h"

#include "../Source/dsp/BalanceModule.h"
#include "../Source/dsp/ClayModule.h"
#include "../Source/dsp/GlazeProfile.h"
#include "../Source/dsp/LoudnessMeter.h"
#include "../Source/dsp/MusicAnalyzer.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>
#include <functional>
#include <memory>
#include <tuple>

namespace celadon::test
{
    namespace
    {
        using glaze::AuxiliaryFeatures;
        using glaze::Discontinuity;
        using glaze::Feature;
        using glaze::HostInfo;
        using glaze::MusicAnalyzer;
        using glaze::MusicProfile;

        constexpr int numChannels = 2;
        constexpr int preparedBlock = 512;
        constexpr int numBands = BalanceModule::numBands;

        // 学习中的性能护栏门槛（µs / 块，48 kHz、块 512）：通用档与 House 档两条共用这一个数，改就一起改（T-043 / T-038 约定）。
        // 沿革：ADR-012 先试后用起 60 µs（估 20 ～ 30 µs 留约 2 倍）。2026-10-01 合并 main fd486ba（T-043，ADR-027：学习期多两个守护混合损失
        // 候选，各一份 ClayModule::PeakGuard 加 K 加权双二阶）后实测：通用护栏（粉噪 + 鼓）约 55.6 ～ 56.8 µs，House 护栏（重低音的 House 曲）
        // 约 58.6 ～ 59.4 µs（107 / 109 各三遍）；同一首 House 曲在通用档是 58.5 µs——House 档自己的段落账只占约 0.3 µs，其余是混合损失候选在
        // 重低音素材上的开销。60 µs 只剩 1 ～ 2 % 余量，机器负载与测量抖动就会误报，所以改 75 µs：按设计新增的学习期工作量、实测最高约 59.4 µs，
        // 留约 25 %。只在学习期；75 µs / 块 ≈ 单核 0.7 %（192 kHz 时块长相同、按样本数线性，约 2.8 %）。不学习（常态）的门槛 20 µs 不变。
        constexpr double learningGuardMicroseconds = 75.0;

        //==============================================================================
        // 信号

        // 粉噪：Paul Kellet 的 7 极点近似（musicdsp.org「pink noise, refined」，9 Hz ～ 0.45 fs 内与 −3 dB/oct 偏差 ±0.05 dB），
        // 与 BalanceTests 同一写法；两声道不同种子，先空跑 8192 样本进稳态，再把每声道归一到指定 RMS。
        Signal makePinkNoise (int channels, int numSamples, float rmsDbfs, juce::int64 seed)
        {
            constexpr int warmUp = 8192;
            auto signal = makeSilence (channels, numSamples);
            const auto targetRms = (double) juce::Decibels::decibelsToGain (rmsDbfs);

            for (int ch = 0; ch < channels; ++ch)
            {
                juce::Random random (seed + (juce::int64) ch * 7919);
                double b0 = 0.0, b1 = 0.0, b2 = 0.0, b3 = 0.0, b4 = 0.0, b5 = 0.0, b6 = 0.0;
                auto& channel = signal[(size_t) ch];
                double sumOfSquares = 0.0;

                for (int i = -warmUp; i < numSamples; ++i)
                {
                    const auto white = (double) random.nextFloat() * 2.0 - 1.0;

                    b0 = 0.99886 * b0 + white * 0.0555179;
                    b1 = 0.99332 * b1 + white * 0.0750759;
                    b2 = 0.96900 * b2 + white * 0.1538520;
                    b3 = 0.86650 * b3 + white * 0.3104856;
                    b4 = 0.55000 * b4 + white * 0.5329522;
                    b5 = -0.7616 * b5 - white * 0.0168980;
                    const auto pink = b0 + b1 + b2 + b3 + b4 + b5 + b6 + white * 0.5362;
                    b6 = white * 0.115926;

                    if (i >= 0)
                    {
                        channel[(size_t) i] = (float) pink;
                        sumOfSquares += pink * pink;
                    }
                }

                const auto gain = (float) (targetRms / std::sqrt (sumOfSquares / (double) numSamples));

                for (auto& sample : channel)
                    sample *= gain;
            }

            return signal;
        }

        // 均匀白噪，每声道 RMS = rmsDbfs（均匀分布的 RMS = 幅度 / √3）。
        Signal makeWhiteNoise (int channels, int numSamples, float rmsDbfs, juce::int64 seed)
        {
            const auto amplitude = juce::Decibels::decibelsToGain (rmsDbfs) * std::sqrt (3.0f);
            return makeNoise (channels, numSamples, amplitude, seed);
        }

        void append (Signal& signal, const Signal& more)
        {
            for (size_t ch = 0; ch < signal.size(); ++ch)
                signal[ch].insert (signal[ch].end(), more[ch].begin(), more[ch].end());
        }

        void addInto (Signal& signal, const Signal& more)
        {
            for (size_t ch = 0; ch < signal.size(); ++ch)
                for (size_t i = 0; i < signal[ch].size() && i < more[ch].size(); ++i)
                    signal[ch][i] += more[ch][i];
        }

        // 转置直接 II 型双二阶（double），每声道一份状态。
        void applyBiquad (Signal& signal, double b0, double b1, double b2, double a1, double a2)
        {
            for (auto& channel : signal)
            {
                double s1 = 0.0, s2 = 0.0;

                for (auto& sample : channel)
                {
                    const auto x = (double) sample;
                    const auto y = b0 * x + s1;
                    s1 = b1 * x - a1 * y + s2;
                    s2 = b2 * x - a2 * y;
                    sample = (float) y;
                }
            }
        }

        // RBJ 峰值均衡（Q ≈ 4.318 = 1/3 倍频程带宽）。
        void applyPeaking (Signal& signal, double sampleRate, double frequency, double gainDb, double q = 4.318)
        {
            const auto a = std::pow (10.0, gainDb / 40.0);
            const auto w0 = juce::MathConstants<double>::twoPi * frequency / sampleRate;
            const auto alpha = std::sin (w0) / (2.0 * q);
            const auto a0 = 1.0 + alpha / a;
            applyBiquad (signal, (1.0 + alpha * a) / a0, -2.0 * std::cos (w0) / a0, (1.0 - alpha * a) / a0,
                         -2.0 * std::cos (w0) / a0, (1.0 - alpha / a) / a0);
        }

        // RBJ 二阶低通（Q = 0.7071），两级串联 = 24 dB/oct。
        void applyLowpass24 (Signal& signal, double sampleRate, double frequency)
        {
            const auto w0 = juce::MathConstants<double>::twoPi * frequency / sampleRate;
            const auto alpha = std::sin (w0) / (2.0 * 0.70710678118654752);
            const auto cosW0 = std::cos (w0);
            const auto a0 = 1.0 + alpha;

            for (int stage = 0; stage < 2; ++stage)
                applyBiquad (signal, (1.0 - cosW0) / 2.0 / a0, (1.0 - cosW0) / a0, (1.0 - cosW0) / 2.0 / a0,
                             -2.0 * cosW0 / a0, (1.0 - alpha) / a0);
        }

        // BS.1770-4 表 1 / 表 2（48 kHz）：K 加权两级双二阶在 frequency 处的功率增益 |H|²。
        double kWeightingPowerGain48k (double frequency)
        {
            const auto w = juce::MathConstants<double>::twoPi * frequency / 48000.0;
            const auto z1 = std::polar (1.0, -w), z2 = std::polar (1.0, -2.0 * w);

            auto power = [&] (double b0, double b1, double b2, double a1, double a2)
            {
                return std::norm ((b0 + b1 * z1 + b2 * z2) / (1.0 + a1 * z1 + a2 * z2));
            };

            return power (1.53512485958697, -2.69169618940638, 1.19839281085285, -1.69065929318241, 0.73248077421585)
                 * power (1.0, -2.0, 1.0, -1.99004745483398, 0.99007225036621);
        }

        // 立体声（两声道相同）1 kHz 正弦要读 lufs 所需的幅度（BS.1770：两声道均方之和 = 2 · A² / 2 · |H|²）。
        float sineAmplitudeForLufs (double lufs)
        {
            return (float) std::sqrt (std::pow (10.0, (lufs + 0.691) / 10.0) / kWeightingPowerGain48k (1000.0));
        }

        Signal makeLoudnessSine (double sampleRate, double lufs, double seconds)
        {
            return makeSine (numChannels, sampleRate, 1000.0, sineAmplitudeForLufs (lufs), seconds);
        }

        // 相位连续的分段 1 kHz 正弦（每段电平不同，切换在子块边界上）。
        Signal makeSteppedSine (double sampleRate, const std::vector<std::pair<double, double>>& lufsAndSeconds)
        {
            Signal signal (numChannels);
            double phase = 0.0;
            const auto increment = juce::MathConstants<double>::twoPi * 1000.0 / sampleRate;

            for (const auto& [lufs, seconds] : lufsAndSeconds)
            {
                const auto amplitude = (double) sineAmplitudeForLufs (lufs);
                const auto length = (int) std::lround (seconds * sampleRate);

                for (int i = 0; i < length; ++i)
                {
                    const auto value = (float) (amplitude * std::sin (phase));
                    phase += increment;

                    for (auto& channel : signal)
                        channel.push_back (value);
                }
            }

            return signal;
        }

        // 点击串：宽 48 样本、正负交替、峰值 amplitude 的短脉冲，第一击在 firstSeconds，之后每 1/rate 秒一击。
        Signal makeClickTrain (double sampleRate, double rate, double seconds, float amplitude, double firstSeconds, int& clicks)
        {
            auto signal = makeSilence (numChannels, (int) (seconds * sampleRate));
            clicks = 0;

            for (int k = 0;; ++k)
            {
                const auto start = (int) std::lround ((firstSeconds + k / rate) * sampleRate);

                if (start + 48 > (int) signal.front().size())
                    break;

                for (int i = 0; i < 48; ++i)
                    for (auto& channel : signal)
                        channel[(size_t) (start + i)] = (i % 2 == 0) ? amplitude : -amplitude;

                ++clicks;
            }

            return signal;
        }

        // 持续低音：锯齿式（1 ～ 10 次谐波、1/h 幅度）两个声部，第二声部失谐 detuneHz；可加振幅颤音。
        Signal makeSustainedBass (double sampleRate, double seconds, double frequency, double detuneHz, float dbfs,
                                  double tremoloHz = 0.0, double tremoloDepth = 0.0)
        {
            const auto numSamples = (int) (seconds * sampleRate);
            auto signal = makeSilence (numChannels, numSamples);
            const auto amplitude = 0.25 * (double) juce::Decibels::decibelsToGain (dbfs);
            const auto twoPi = juce::MathConstants<double>::twoPi;

            for (int i = 0; i < numSamples; ++i)
            {
                const auto t = (double) i / sampleRate;
                double value = 0.0;

                for (int h = 1; h <= 10; ++h)
                {
                    value += std::sin (twoPi * frequency * h * t) / h;
                    value += std::sin (twoPi * (frequency + detuneHz) * h * t + 0.3 * h) / h;
                }

                const auto tremolo = 1.0 + tremoloDepth * std::sin (twoPi * tremoloHz * t);

                for (auto& channel : signal)
                    channel[(size_t) i] = (float) (amplitude * tremolo * value);
            }

            return signal;
        }

        // 合成鼓型（120 BPM）：底鼓（下滑正弦，80 ms 衰减）每拍，军鼓（噪声 + 200 Hz，50 ms 衰减）叠在二、四拍，
        // 踩镲（一阶差分的高频噪声，10 ms 衰减，比军鼓低约 14 dB）每个十六分音符。
        Signal makeDrumPattern (double sampleRate, double seconds, juce::int64 seed)
        {
            const auto numSamples = (int) (seconds * sampleRate);
            auto signal = makeSilence (numChannels, numSamples);
            juce::Random random (seed);
            const auto twoPi = juce::MathConstants<double>::twoPi;
            const auto beat = 0.5 * sampleRate;

            auto addAt = [&] (int start, double length, auto&& shape)
            {
                for (int i = 0; i < (int) (length * sampleRate) && start + i < numSamples; ++i)
                {
                    const auto value = (float) shape ((double) i / sampleRate);

                    for (auto& channel : signal)
                        channel[(size_t) (start + i)] += value;
                }
            };

            for (int k = 0; k * beat < numSamples; ++k)
            {
                const auto start = (int) (k * beat);
                addAt (start, 0.3, [&] (double t) { return 0.6 * std::exp (-t / 0.08) * std::sin (twoPi * (50.0 + 100.0 * std::exp (-t / 0.02)) * t); });

                if (k % 2 == 1)
                    addAt (start, 0.2, [&] (double t) { return 0.4 * std::exp (-t / 0.05) * ((random.nextFloat() * 2.0 - 1.0) + 0.5 * std::sin (twoPi * 200.0 * t)); });
            }

            double previous = 0.0;

            for (int k = 0; k * beat / 4.0 < numSamples; ++k)
                addAt ((int) (k * beat / 4.0), 0.04, [&] (double t)
                {
                    const auto white = random.nextFloat() * 2.0 - 1.0;
                    const auto highPassed = white - previous;
                    previous = white;
                    return 0.08 * std::exp (-t / 0.01) * highPassed;
                });

            return signal;
        }

        //==============================================================================
        // 被测的一组对象：与链里的顺序相同——IN 表 → processInput → Balance → processSpectrum。
        struct Rig
        {
            LoudnessMeter meter;
            BalanceModule balance;
            MusicAnalyzer analyzer;
            int channels = numChannels;
            double sampleRate = 48000.0;
            std::int64_t fed = 0;

            // 测试自己也读表的子块（另一个游标），用来算 EBU 参照值。
            std::int64_t referenceCursor = 0;
            std::vector<LoudnessMeter::SubBlock> subBlocks;

            Rig (double rate, int numCh = numChannels, BalanceModule::Params balanceParams = {}) : channels (numCh), sampleRate (rate)
            {
                const juce::dsp::ProcessSpec spec { rate, (juce::uint32) preparedBlock, (juce::uint32) numCh };
                meter.prepare (spec);
                balance.setParams (balanceParams);
                balance.prepare (spec);
                balance.setParams (balanceParams);
                analyzer.prepare (spec);
            }

            // blockSize > 0：固定块长；≤ 0：随机块长 1 ～ 4096（种子 seed）。host 非空时每块先交宿主信息（参数：已送样本数、块长）。
            void feed (const Signal& input, int blockSize, juce::int64 seed = 1,
                       const std::function<HostInfo (std::int64_t, int)>& host = {},
                       const std::function<void (Rig&)>& beforeBlock = {})
            {
                juce::Random random (seed);
                const auto numSamples = (int) input.front().size();
                juce::AudioBuffer<float> block (channels, 4096);

                for (int start = 0; start < numSamples;)
                {
                    auto length = blockSize > 0 ? blockSize : 1 + random.nextInt (4096);
                    length = juce::jmin (length, numSamples - start);
                    block.setSize (channels, length, false, false, true);

                    for (int ch = 0; ch < channels; ++ch)
                        block.copyFrom (ch, 0, input[(size_t) juce::jmin (ch, (int) input.size() - 1)].data() + start, length);

                    if (beforeBlock)
                        beforeBlock (*this);

                    if (host)
                        analyzer.setHostInfo (host (fed, length));

                    processBlock (block);
                    start += length;
                    fed += length;
                }
            }

            void processBlock (juce::AudioBuffer<float>& block)
            {
                meter.process (block);
                analyzer.processInput (block, meter);
                balance.process (block);
                analyzer.processSpectrum (balance);

                LoudnessMeter::SubBlock buffer[16];

                for (;;)
                {
                    const auto got = meter.readSubBlocksSince (referenceCursor, buffer, 16);
                    subBlocks.insert (subBlocks.end(), buffer, buffer + std::abs (got));

                    if (std::abs (got) < 16)
                        break;
                }
            }

            const MusicProfile& profile() const   { return analyzer.getCandidateProfile(); }
        };

        HostInfo playingAt (std::int64_t position)
        {
            HostInfo info;
            info.hasPlayState = true;
            info.isPlaying = true;
            info.hasPosition = true;
            info.positionSamples = position;
            return info;
        }

        //==============================================================================
        // EBU 参照（直接按定义：排序、精确能量，无直方图）。输入 = 表的连续子块均方（全部在学习区间内）。
        double lufsOf (double meanSquare)
        {
            return -0.691 + 10.0 * std::log10 (meanSquare);
        }

        double referenceIntegrated (const std::vector<double>& meanSquares)
        {
            std::vector<double> blocks;

            for (size_t k = 3; k < meanSquares.size(); ++k)
            {
                const auto power = (meanSquares[k] + meanSquares[k - 1] + meanSquares[k - 2] + meanSquares[k - 3]) / 4.0;

                if (power > 0.0 && lufsOf (power) >= -70.0)
                    blocks.push_back (power);
            }

            double sum = 0.0;
            for (auto p : blocks)
                sum += p;

            const auto gate = lufsOf (sum / (double) blocks.size()) - 10.0;
            double gated = 0.0;
            int count = 0;

            for (auto p : blocks)
                if (lufsOf (p) >= gate)
                {
                    gated += p;
                    ++count;
                }

            return lufsOf (gated / count);
        }

        double referenceLoudnessRange (const std::vector<double>& meanSquares)
        {
            std::vector<double> values;   // 绝对门以上的 S（能量）

            for (size_t k = 29; k < meanSquares.size(); ++k)
            {
                double sum = 0.0;
                for (size_t j = k - 29; j <= k; ++j)
                    sum += meanSquares[j];

                const auto power = sum / 30.0;

                if (power > 0.0 && lufsOf (power) >= -70.0)
                    values.push_back (power);
            }

            double sum = 0.0;
            for (auto p : values)
                sum += p;

            const auto gate = lufsOf (sum / (double) values.size()) - 20.0;
            std::vector<double> gated;

            for (auto p : values)
                if (lufsOf (p) >= gate)
                    gated.push_back (lufsOf (p));

            std::sort (gated.begin(), gated.end());
            const auto n = gated.size();
            const auto low = gated[(size_t) ((double) (n - 1) * 0.10 + 0.5)];
            const auto high = gated[(size_t) ((double) (n - 1) * 0.95 + 0.5)];
            return high - low;
        }

        std::vector<double> meanSquaresOf (const std::vector<LoudnessMeter::SubBlock>& subBlocks)
        {
            std::vector<double> result;

            for (const auto& s : subBlocks)
                result.push_back (s.meanSquare);

            return result;
        }

        //==============================================================================
        bool sameFloat (float a, float b)
        {
            return std::memcmp (&a, &b, sizeof (float)) == 0;
        }

        bool sameDouble (double a, double b)
        {
            return std::memcmp (&a, &b, sizeof (double)) == 0;
        }

        bool sameFeature (const Feature& a, const Feature& b)
        {
            return sameFloat (a.value, b.value) && a.valid == b.valid && sameDouble (a.effectiveSeconds, b.effectiveSeconds)
                && sameFloat (a.confidence, b.confidence);
        }

        template <typename Array>
        bool sameArray (const Array& a, const Array& b)
        {
            for (size_t i = 0; i < a.size(); ++i)
                if (! sameFloat (a[i], b[i]))
                    return false;

            return true;
        }

        // 画像逐位相同（逐字段比，不比结构体的填充字节）。
        bool sameProfile (const MusicProfile& a, const MusicProfile& b)
        {
            return sameFeature (a.integratedLufs, b.integratedLufs) && sameFeature (a.loudnessRangeLu, b.loudnessRangeLu)
                && sameFeature (a.plrDb, b.plrDb) && sameFeature (a.truePeakDbtp, b.truePeakDbtp)
                && sameFeature (a.spectralSlopeDbPerOct, b.spectralSlopeDbPerOct) && sameFeature (a.lowShare, b.lowShare)
                && sameFeature (a.highShare, b.highShare) && sameFeature (a.lowBandRangeDb, b.lowBandRangeDb)
                && sameFeature (a.onsetDensityPerSecond, b.onsetDensityPerSecond) && sameFeature (a.onsetStrengthDb, b.onsetStrengthDb)
                && sameArray (a.bandLongTermDb, b.bandLongTermDb) && sameArray (a.tameEvidenceDb, b.tameEvidenceDb)
                && sameArray (a.recoverEvidenceDb, b.recoverEvidenceDb) && sameArray (a.bandConfidence, b.bandConfidence)
                && sameDouble (a.effectiveSeconds, b.effectiveSeconds) && sameFloat (a.representativeness, b.representativeness)
                && a.formatVersion == b.formatVersion;
        }

        bool sameAuxiliary (const AuxiliaryFeatures& a, const AuxiliaryFeatures& b)
        {
            return sameFeature (a.highBandFluxDb, b.highBandFluxDb)
                && sameFeature (a.highBandFluxOnsetsPerSecond, b.highBandFluxOnsetsPerSecond)
                && sameFloat (a.onsetAgreement, b.onsetAgreement) && sameFeature (a.lowBandRangeDb, b.lowBandRangeDb)
                && sameFeature (a.trialPeakP99Db, b.trialPeakP99Db);   // v0.1.7.0 先试后用的基准（ADR-012）
        }

        int onsetCountOf (const MusicProfile& profile)
        {
            return (int) std::lround ((double) profile.onsetDensityPerSecond.value * profile.onsetDensityPerSecond.effectiveSeconds);
        }

        // Balance 的 27 段精确中心（第 14 段 1 kHz，1/3 倍频程）。
        double bandCentreHz (int band)
        {
            return 1000.0 * std::pow (2.0, (band - 14) / 3.0);
        }
    }

    //==============================================================================
    class MusicAnalyzerTests  : public juce::UnitTest
    {
    public:
        MusicAnalyzerTests() : juce::UnitTest ("MusicAnalyzer", "Celadon") {}

        void runTest() override
        {
            testSubBlockRing();
            testSubBlockRingOverflowAndResets();
            testFrameSnapshots();
            testMomentaryShortTermIntegratedOfSine();
            testIntegratedGating();
            testLoudnessRange();
            testPeakToLoudness();
            testPinkAndWhiteNoise();
            testNotchAndPeakEvidence();
            testLiveTameAfterLearning();
            testLiveBroadbandSlope();
            testRecoverIsConservative();
            testChannelsCombineByPower();
            testClickTrains();
            testSustainedBassNeverTriggers();
            testDrumPattern();
            testEffectiveTime();
            testLearningInterval();
            testUnknownIsNotZero();
            testDiscontinuity();
            testHostAndModuleResets();
            testSpectrumDeliveryAcrossInputSlices();
            testRepresentativeness();
            testBlockSizeIndependence();
            testNoAllocation();
            testPerformanceGuard();
        }

    private:
        //==============================================================================
        // 依据：LoudnessMeter.h「子块」注释、设计规格-GLAZE-分析器第 2 节（M / S / I / LRA 从 100 ms 子块算）。
        // 子块时间戳 = 处理完本子块最后一个样本时的累计样本数（k × 子块长度）；由 30 个子块算出的 S 与表自己的短期读数一致
        // （同一批能量，只是求和的舍入顺序不同：容差 1e-4 LU）；子块真峰值的最大值与表的真峰值读数逐位相同（同一个检测器、
        // 同一批输出取最大，jmax 与分段无关）；getLastBlockTruePeakLinear 与独立检测器按块取最大逐位相同。单声道按双单声道计，
        // 与左右相同的立体声逐位相同。
        void testSubBlockRing()
        {
            beginTest ("sub-blocks: 100 ms K-weighted mean squares with sample timestamps; S and true peak agree with the meter");

            for (auto sampleRate : { 44100.0, 48000.0, 96000.0 })
            {
                const auto length = juce::roundToInt (sampleRate * 0.1);
                const auto input = makePinkNoise (numChannels, 31 * length, -20.0f, 3);

                LoudnessMeter meter;
                meter.prepare ({ sampleRate, (juce::uint32) preparedBlock, (juce::uint32) numChannels });
                expectEquals (meter.getSubBlockLength(), length);

                TruePeakDetector detector;
                detector.prepare (numChannels);

                std::vector<LoudnessMeter::SubBlock> subBlocks;
                std::int64_t cursor = 0;
                juce::AudioBuffer<float> block (numChannels, 333);
                int lastBlockMismatches = 0;

                for (int start = 0; start < 31 * length; start += 333)
                {
                    const auto count = juce::jmin (333, 31 * length - start);
                    block.setSize (numChannels, count, false, false, true);

                    for (int ch = 0; ch < numChannels; ++ch)
                        block.copyFrom (ch, 0, input[(size_t) ch].data() + start, count);

                    meter.process (block);

                    float expectedPeak = 0.0f;
                    for (int ch = 0; ch < numChannels; ++ch)
                        expectedPeak = juce::jmax (expectedPeak, detector.processBlockMax (ch, block.getReadPointer (ch), count));

                    lastBlockMismatches += sameFloat (meter.getLastBlockTruePeakLinear(), expectedPeak) ? 0 : 1;

                    LoudnessMeter::SubBlock buffer[4];
                    const auto got = meter.readSubBlocksSince (cursor, buffer, 4);
                    expect (got >= 0, "no sub-block may be dropped when reading every block");
                    subBlocks.insert (subBlocks.end(), buffer, buffer + std::abs (got));
                }

                const auto tag = "@" + juce::String (sampleRate);
                expectEquals (lastBlockMismatches, 0, "getLastBlockTruePeakLinear " + tag);
                expectEquals ((int) subBlocks.size(), 31, "sub-block count " + tag);
                expect (meter.getProcessedSamples() == (std::int64_t) 31 * length, "processed samples " + tag);

                int timestampErrors = 0;
                for (size_t k = 0; k < subBlocks.size(); ++k)
                    timestampErrors += subBlocks[k].endSample == (std::int64_t) (k + 1) * length ? 0 : 1;

                expectEquals (timestampErrors, 0, "timestamps are k x sub-block length " + tag);

                // S：最近 30 个子块（第 2 ～ 31 个）。
                double sum = 0.0;
                float ringPeak = 0.0f;

                for (size_t k = 1; k < subBlocks.size(); ++k)
                    sum += subBlocks[k].meanSquare;

                for (const auto& s : subBlocks)
                    ringPeak = juce::jmax (ringPeak, s.truePeakLinear);

                expectWithinAbsoluteError ((float) lufsOf (sum / 30.0), meter.getShortTermLufs(), 1.0e-4f, "S from sub-blocks " + tag);

                const auto ringPeakDb = juce::jmax (LoudnessMeter::noReading, juce::Decibels::gainToDecibels (ringPeak, LoudnessMeter::noReading));
                expect (sameFloat (ringPeakDb, meter.getAndResetTruePeakDb()), "max sub-block true peak equals the meter reading " + tag);
            }

            // 单声道 = 左右相同的立体声（能量计两次）。表的算式沿用 v0.5（单声道 2·y² 一次乘加、立体声 y² 加两次），
            // 末位舍入可以不同：均方相对差 < 1e-12；真峰值是各声道检测器输出的最大值，逐位相同。
            {
                const auto source = makePinkNoise (1, 12000, -18.0f, 9);
                const Signal stereo { source[0], source[0] };
                LoudnessMeter mono, both;
                mono.prepare ({ 48000.0, 512, 1 });
                both.prepare ({ 48000.0, 512, 2 });

                juce::AudioBuffer<float> monoBlock (1, 12000), stereoBlock (2, 12000);
                monoBlock.copyFrom (0, 0, source[0].data(), 12000);
                stereoBlock.copyFrom (0, 0, stereo[0].data(), 12000);
                stereoBlock.copyFrom (1, 0, stereo[1].data(), 12000);
                mono.process (monoBlock);
                both.process (stereoBlock);

                LoudnessMeter::SubBlock a[4], b[4];
                std::int64_t ca = 0, cb = 0;
                const auto na = mono.readSubBlocksSince (ca, a, 4);
                const auto nb = both.readSubBlocksSince (cb, b, 4);
                expectEquals (na, 2);
                expectEquals (nb, 2);

                for (int k = 0; k < juce::jmin (na, nb); ++k)
                    expect (std::abs (a[k].meanSquare - b[k].meanSquare) <= 1.0e-12 * b[k].meanSquare
                                && sameFloat (a[k].truePeakLinear, b[k].truePeakLinear) && a[k].endSample == b[k].endSample,
                            "mono sub-block equals identical stereo");
            }
        }

        // 依据：LoudnessMeter.h「readSubBlocksSince」注释。落后超过 128 个子块：跳到最旧可用的、返回负数；界面的 requestReset
        // 只清表的读数，不打断子块序列；响度计量关着时不产生子块（样本计数照走），重新打开后网格从那一刻重起；reset 后序号与
        // 时间戳都从零开始，旧游标从头读。
        void testSubBlockRingOverflowAndResets()
        {
            beginTest ("sub-block ring: overflow reports a negative count; requestReset keeps the sequence; loudness off produces none");

            constexpr double sampleRate = 48000.0;
            constexpr int length = 4800;
            LoudnessMeter meter;
            meter.prepare ({ sampleRate, 512, 2 });

            auto feedSeconds = [&] (double seconds)
            {
                const auto input = makePinkNoise (numChannels, (int) (seconds * sampleRate), -20.0f, 5);
                juce::AudioBuffer<float> block (numChannels, 512);

                for (int start = 0; start < (int) input.front().size(); start += 512)
                {
                    const auto count = juce::jmin (512, (int) input.front().size() - start);
                    block.setSize (numChannels, count, false, false, true);

                    for (int ch = 0; ch < numChannels; ++ch)
                        block.copyFrom (ch, 0, input[(size_t) ch].data() + start, count);

                    meter.process (block);
                }
            };

            std::vector<LoudnessMeter::SubBlock> out (256);

            // 20 s = 200 个子块，一个都没读：只剩最近 128 个。
            feedSeconds (20.0);
            std::int64_t cursor = 0;
            expectEquals (meter.readSubBlocksSince (cursor, out.data(), 256), -128);
            expect (out[0].endSample == (std::int64_t) 73 * length && out[127].endSample == (std::int64_t) 200 * length);
            expect (cursor == 200);
            expectEquals (meter.readSubBlocksSince (cursor, out.data(), 256), 0);

            // 恰好落后 128 个：不算丢。
            feedSeconds (12.8);
            expectEquals (meter.readSubBlocksSince (cursor, out.data(), 256), 128);

            // maxCount 限制：分两次读完。
            feedSeconds (1.0);
            expectEquals (meter.readSubBlocksSince (cursor, out.data(), 4), 4);
            expectEquals (meter.readSubBlocksSince (cursor, out.data(), 256), 6);

            // requestReset：表的读数清掉，子块序列照常（时间戳连续）。
            meter.requestReset();
            feedSeconds (1.0);
            expectEquals (meter.readSubBlocksSince (cursor, out.data(), 256), 10);
            expect (out[0].endSample == (std::int64_t) 339 * length && out[9].endSample == (std::int64_t) 348 * length,
                    "requestReset must not interrupt the sub-block sequence");

            // 响度计量关着：不产生子块，样本数照走、块真峰值照算；重新打开后第一个子块在打开那一刻之后整 100 ms 结束。
            meter.setLoudnessEnabled (false);
            const auto before = meter.getProcessedSamples();
            feedSeconds (1.0);
            expect (meter.getProcessedSamples() == before + 48000);
            expect (meter.getLastBlockTruePeakLinear() > 0.0f);
            expectEquals (meter.readSubBlocksSince (cursor, out.data(), 256), 0);

            meter.setLoudnessEnabled (true);
            const auto enabledAt = meter.getProcessedSamples();
            feedSeconds (0.2133);
            expectEquals (meter.readSubBlocksSince (cursor, out.data(), 256), 2);
            expect (out[0].endSample == enabledAt + length && out[1].endSample == enabledAt + 2 * length,
                    "after re-enabling loudness the grid restarts at that moment");

            // reset：序号与时间戳从零开始；旧游标从头读（按「丢过」处理，这时一个也没有，返回 0）。
            meter.reset();
            expect (meter.getProcessedSamples() == 0);
            expectEquals (meter.readSubBlocksSince (cursor, out.data(), 256), 0);
            expect (cursor == 0);
            feedSeconds (0.2);
            expectEquals (meter.readSubBlocksSince (cursor, out.data(), 256), 2);
            expect (out[0].endSample == length && out[1].endSample == 2 * length);
        }

        // 依据：BalanceModule.h「FrameSnapshot」注释、设计规格-GLAZE-分析器第 4 节。帧的时间戳 = 窗长 + k × hop（第一帧等满
        // 一整窗）；dB 与线性两份同源；拆片与否发布的内容逐位相同（随机块长）；旁通时照常发布；快照只看输入——Recover / Tame
        // 开满与全关的帧逐位相同（只读、不改音频路径）；落后超过 64 帧返回负数。
        void testFrameSnapshots()
        {
            beginTest ("frame snapshots: timestamps, both units, identical with and without slicing, bypass and settings");

            struct RateCase { double rate; int window; int hop; };

            for (const auto& rateCase : { RateCase { 44100.0, 2048, 512 }, RateCase { 48000.0, 2048, 512 },
                                          RateCase { 96000.0, 4096, 1024 }, RateCase { 192000.0, 8192, 2048 } })
            {
                const auto sampleRate = rateCase.rate;
                const auto window = rateCase.window;
                const auto hop = rateCase.hop;
                const auto input = makePinkNoise (numChannels, (int) (0.6 * sampleRate), -20.0f, 7);
                const auto tag = "@" + juce::String (sampleRate);

                auto collect = [&] (BalanceModule::Params params, bool slicing, juce::int64 seed)
                {
                    BalanceModule balance;
                    balance.setParams (params);
                    balance.prepare ({ sampleRate, 512, (juce::uint32) numChannels });
                    balance.setParams (params);
                    balance.setAnalysisSlicingForTesting (slicing);
                    expectEquals (balance.getAnalysisWindowSamples(), window, "window " + tag);
                    expectEquals (balance.getAnalysisHopSamples(), hop, "hop " + tag);

                    std::vector<BalanceModule::FrameSnapshot> frames;
                    std::int64_t cursor = 0;
                    juce::Random random (seed);
                    juce::AudioBuffer<float> block (numChannels, 4096);

                    for (int start = 0; start < (int) input.front().size();)
                    {
                        const auto count = juce::jmin (1 + random.nextInt (1500), (int) input.front().size() - start);
                        block.setSize (numChannels, count, false, false, true);

                        for (int ch = 0; ch < numChannels; ++ch)
                            block.copyFrom (ch, 0, input[(size_t) ch].data() + start, count);

                        balance.process (block);
                        BalanceModule::FrameSnapshot buffer[8];

                        for (;;)
                        {
                            const auto got = balance.readFramesSince (cursor, buffer, 8);
                            expect (got >= 0);
                            frames.insert (frames.end(), buffer, buffer + std::abs (got));

                            if (std::abs (got) < 8)
                                break;
                        }

                        start += count;
                    }

                    expect (balance.getProcessedSamples() == (std::int64_t) input.front().size());
                    return frames;
                };

                auto sameFrames = [&] (const std::vector<BalanceModule::FrameSnapshot>& a, const std::vector<BalanceModule::FrameSnapshot>& b)
                {
                    if (a.size() != b.size())
                        return false;

                    for (size_t k = 0; k < a.size(); ++k)
                        if (a[k].endSample != b[k].endSample || ! sameArray (a[k].bandPowerDb, b[k].bandPowerDb)
                            || ! sameArray (a[k].bandPower, b[k].bandPower))
                            return false;

                    return true;
                };

                BalanceModule::Params off, full, bypassed;
                full.recover = full.tame = 100.0f;
                full.brighten = 30.0f;
                bypassed = full;
                bypassed.bypass = true;

                const auto reference = collect (off, true, 1);
                const auto expectedFrames = ((int) input.front().size() - window) / hop + 1;

                // 拆片时最后一帧要等 hop 后第 1/8 hop 个样本（48 k 下 64，随采样率翻倍；T-032 R2-07）才发布：输入正好停在那之前时少一帧。
                expect ((int) reference.size() == expectedFrames || (int) reference.size() == expectedFrames - 1, "frame count " + tag);

                int timestampErrors = 0, unitErrors = 0;

                for (size_t k = 0; k < reference.size(); ++k)
                {
                    timestampErrors += reference[k].endSample == (std::int64_t) window + (std::int64_t) k * hop ? 0 : 1;

                    for (int b = 0; b < numBands; ++b)
                    {
                        const auto fromLinear = 10.0 * std::log10 (juce::jmax (1.0e-30, (double) reference[k].bandPower[(size_t) b]));
                        unitErrors += std::abs (fromLinear - (double) reference[k].bandPowerDb[(size_t) b]) < 1.0e-4 ? 0 : 1;
                    }
                }

                expectEquals (timestampErrors, 0, "frame timestamps " + tag);
                expectEquals (unitErrors, 0, "dB and linear band powers agree " + tag);

                auto unsliced = collect (off, false, 2);
                unsliced.resize (juce::jmin (unsliced.size(), reference.size()));
                expect (sameFrames (reference, unsliced), "sliced and one-shot analysis publish identical frames " + tag);
                expect (sameFrames (reference, collect (full, true, 3)), "frames do not depend on recover / tame / brighten " + tag);
                expect (sameFrames (reference, collect (bypassed, true, 4)), "frames are still published while bypassed " + tag);
            }

            // 落后超过 64 帧：返回负数，从最旧可用的一帧读起。
            {
                BalanceModule balance;
                balance.prepare ({ 48000.0, 512, 2 });
                const auto input = makePinkNoise (numChannels, 48000, -20.0f, 8);
                juce::AudioBuffer<float> block (numChannels, 48000);

                for (int ch = 0; ch < numChannels; ++ch)
                    block.copyFrom (ch, 0, input[(size_t) ch].data(), 48000);

                balance.process (block);
                std::vector<BalanceModule::FrameSnapshot> out (128);
                std::int64_t cursor = 0;
                const auto published = (48000 - 2048 - 64) / 512 + 1;
                expectEquals (balance.readFramesSince (cursor, out.data(), 128), -64);
                expect (out[63].endSample == 2048 + (std::int64_t) (published - 1) * 512 && out[0].endSample == out[63].endSample - 63 * 512);
                expect (cursor == published);
            }
        }

        //==============================================================================
        // 依据：分析器第 2 节（M 400 ms、S 3 s 不门限不平滑；I 按 EBU 门限积分）。立体声 1 kHz 正弦按 BS.1770-4 标准表
        // 算出应读的响度：48 kHz 用标准表本身，容差 0.005 LU（整数个周期，只剩起始瞬态的影响）；44.1 / 96 kHz 表按模拟原型
        // 换算系数，与 48 kHz 表在 1 kHz 处相差 < 0.01 dB，容差 0.02 LU。
        void testMomentaryShortTermIntegratedOfSine()
        {
            beginTest ("M / S / I of a stereo 1 kHz sine match the BS.1770-4 formula");

            for (auto sampleRate : { 48000.0, 44100.0, 96000.0 })
                for (auto lufs : { -20.0, -8.0, -36.0 })
                {
                    Rig rig (sampleRate);
                    rig.analyzer.startLearning();
                    rig.feed (makeLoudnessSine (sampleRate, lufs, 6.0), 512);

                    const auto tolerance = sampleRate == 48000.0 ? 0.005f : 0.02f;
                    const auto tag = "@" + juce::String (sampleRate) + " " + juce::String (lufs) + " LUFS";
                    const auto& view = rig.analyzer.getShortTermView();

                    expect (view.momentaryLufs.valid && view.shortTermLufs.valid, tag);
                    expectWithinAbsoluteError (view.momentaryLufs.value, (float) lufs, tolerance, "M " + tag);
                    expectWithinAbsoluteError (view.shortTermLufs.value, (float) lufs, tolerance, "S " + tag);
                    expect (rig.profile().integratedLufs.valid, tag);
                    expectWithinAbsoluteError (rig.profile().integratedLufs.value, (float) lufs, 2.0f * tolerance, "I " + tag);
                    expect (view.musicActive, "a steady sine is music " + tag);
                }
        }

        // 依据：分析器第 2 节（I：绝对门 −70 LUFS、相对门 −10 LU）；EBU Tech 3341 的构造（−36 / −23 / −36 → −23 ± 0.1）。
        // 期望值两份：
        //   - 理想值：各段按 BS.1770 应有的子块均方（电平切换都在子块边界上）按 EBU 定义算——跨越切换的三个 400 ms 门块
        //     能量是前段的 ¾ / ½ / ¼，在相对门之上的照样计入（例如 10 s −20 LUFS 接数字静音，I = −20.066 而不是 −20）；
        //     实际信号只多了电平切换处 K 加权滤波器的一点瞬态，容差 0.02 LU；
        //   - 参照值：直接用表的子块按定义算（排序、精确能量）。分析器的 0.1 LU 直方图只影响「门块在不在相对门之上」的判定，
        //     这里没有门块落在门边 0.05 LU 以内，容差 0.02 LU。
        void testIntegratedGating()
        {
            beginTest ("integrated loudness applies the absolute and relative gates (EBU R128 / Tech 3341)");

            struct Case { const char* name; std::vector<std::pair<double, double>> segments; };
            const Case cases[] = {
                { "-20 then -40 LUFS (relative gate drops the quiet half)", { { -20.0, 20.0 }, { -40.0, 20.0 } } },
                { "Tech 3341: -36 / -23 / -36", { { -36.0, 10.0 }, { -23.0, 60.0 }, { -36.0, 10.0 } } },
                { "-20 and -28 LUFS both above the relative gate", { { -20.0, 10.0 }, { -28.0, 10.0 } } },
                { "-20 LUFS then digital silence (absolute gate)", { { -20.0, 10.0 }, { -300.0, 10.0 } } },
            };

            for (const auto& c : cases)
            {
                std::vector<double> ideal;

                for (const auto& [lufs, seconds] : c.segments)
                    ideal.insert (ideal.end(), (size_t) std::lround (seconds * 10.0), std::pow (10.0, (lufs + 0.691) / 10.0));

                Rig rig (48000.0);
                rig.analyzer.startLearning();
                rig.feed (makeSteppedSine (48000.0, c.segments), 512);

                const auto& integrated = rig.profile().integratedLufs;
                expect (integrated.valid, c.name);
                expectWithinAbsoluteError (integrated.value, (float) referenceIntegrated (ideal), 0.02f, juce::String ("ideal: ") + c.name);
                expectWithinAbsoluteError (integrated.value, (float) referenceIntegrated (meanSquaresOf (rig.subBlocks)), 0.02f,
                                           juce::String ("reference from sub-blocks: ") + c.name);
            }

            // Tech 3341 的标称值与容差。
            Rig rig (48000.0);
            rig.analyzer.startLearning();
            rig.feed (makeSteppedSine (48000.0, { { -36.0, 10.0 }, { -23.0, 60.0 }, { -36.0, 10.0 } }), 512);
            expectWithinAbsoluteError (rig.profile().integratedLufs.value, -23.0f, 0.1f, "Tech 3341 nominal");
        }

        // 依据：分析器第 2 节（LRA = 门限后 S 分布的 P95 − P10，S 10 Hz 取样，绝对门 −70、相对门 −20）；EBU Tech 3342 的
        // 四个构造（标称值 ±1 LU）。与表子块直接算的参照（排序、百分位下标按 libebur128 的取整）相比，分析器的直方图
        // 在两端各取格中心：容差 0.1 LU。
        void testLoudnessRange()
        {
            beginTest ("loudness range follows EBU Tech 3342 (P95 - P10 of gated short-term values)");

            struct Case { const char* name; std::vector<std::pair<double, double>> segments; float nominal; };
            const Case cases[] = {
                { "3342 #1: -20 / -30", { { -20.0, 20.0 }, { -30.0, 20.0 } }, 10.0f },
                { "3342 #2: -20 / -15", { { -20.0, 20.0 }, { -15.0, 20.0 } }, 5.0f },
                { "3342 #3: -40 / -20", { { -40.0, 20.0 }, { -20.0, 20.0 } }, 20.0f },
                { "3342 #4: -50 / -35 / -20 / -35 / -50", { { -50.0, 20.0 }, { -35.0, 20.0 }, { -20.0, 20.0 }, { -35.0, 20.0 }, { -50.0, 20.0 } }, 15.0f },
            };

            for (const auto& c : cases)
            {
                Rig rig (48000.0);
                rig.analyzer.startLearning();
                rig.feed (makeSteppedSine (48000.0, c.segments), 512);

                const auto& range = rig.profile().loudnessRangeLu;
                expect (range.valid, c.name);
                expectWithinAbsoluteError (range.value, c.nominal, 1.0f, juce::String ("Tech 3342 nominal: ") + c.name);
                expectWithinAbsoluteError (range.value, (float) referenceLoudnessRange (meanSquaresOf (rig.subBlocks)), 0.1f,
                                           juce::String ("reference from sub-blocks: ") + c.name);
            }
        }

        // 依据：分析器第 3 节（PLR = 区间真峰值最大 − 同区间 I；PSR₃ₛ = 最近 3 s 真峰值最大 − S）。
        // 真峰值取自表的子块：输入正好是整数个子块时，区间最大值与表的真峰值读数是同一批检测器输出的最大值（容差只剩
        // float / double 换算 1e-4 dB）。纯正弦的 PSR = 0.691 − 10·log10|H(1 kHz)|² ≈ −0.007 dB（真峰值 = 幅度，响度按 BS.1770）；
        // 一个尖峰进入 3 s 窗时 PSR = 尖峰真峰值 − S，出窗后回到正弦的值。
        void testPeakToLoudness()
        {
            beginTest ("PLR and PSR come from the same true-peak detector as the meter");

            constexpr double sampleRate = 48000.0;
            auto input = makeLoudnessSine (sampleRate, -20.0, 10.0);

            for (auto& channel : input)
                channel[(size_t) (5.0 * sampleRate)] += 0.5f;

            Rig rig (sampleRate);
            rig.analyzer.startLearning();

            Signal head (numChannels), tail (numChannels);
            const auto split = (size_t) (6.0 * sampleRate);

            for (size_t ch = 0; ch < input.size(); ++ch)
            {
                head[ch].assign (input[ch].begin(), input[ch].begin() + (std::ptrdiff_t) split);
                tail[ch].assign (input[ch].begin() + (std::ptrdiff_t) split, input[ch].end());
            }

            rig.feed (head, 480);
            const auto& view = rig.analyzer.getShortTermView();
            const auto meterPeakDb = rig.meter.getAndResetTruePeakDb();
            expect (view.psr3sDb.valid);
            expectWithinAbsoluteError (view.psr3sDb.value, meterPeakDb - view.shortTermLufs.value, 1.0e-3f, "PSR with the spike inside the 3 s window");

            rig.feed (tail, 480);
            const auto sinePsr = (float) (0.691 - 10.0 * std::log10 (kWeightingPowerGain48k (1000.0)));
            expectWithinAbsoluteError (view.psr3sDb.value, sinePsr, 0.02f, "PSR of the sine once the spike left the window");

            const auto& profile = rig.profile();
            expect (profile.truePeakDbtp.valid && profile.plrDb.valid);
            expectWithinAbsoluteError (profile.truePeakDbtp.value, meterPeakDb, 1.0e-4f, "interval true peak equals the meter's");
            expectWithinAbsoluteError (profile.plrDb.value, profile.truePeakDbtp.value - profile.integratedLufs.value, 1.0e-4f, "PLR = TP - I");
            expect (profile.plrDb.value > 10.0f, "the spike dominates the peak-to-loudness ratio");
        }

        //==============================================================================
        // 依据：分析器第 4 节（口径 = 段内总功率：粉噪为平、白噪 +3 dB/oct；残差 = 段电平 − 平滑轮廓，超出容忍区间才记证据；
        // 低 / 高频占比按段边界）。粉噪的段电平估计标准差 < 0.6 dB（1 s 功率平均），远小于 ±2 dB 容忍区间：证据 < 0.05 dB；
        // 斜率 |·| < 0.2 dB/oct；占比 5 / 27 ± 0.02。白噪：斜率 10·log10 2 = 3.01 ± 0.1，占比按段宽的精确比例 ± 0.005。
        // 96 kHz（4096 点、hop 1024）也验一遍。
        void testPinkAndWhiteNoise()
        {
            beginTest ("spectrum: pink noise has no evidence, a flat slope and 5/27 shares; white noise tilts +3 dB/oct");

            for (auto sampleRate : { 48000.0, 96000.0 })
            {
                const auto tag = "@" + juce::String (sampleRate);

                {
                    Rig rig (sampleRate);
                    rig.analyzer.startLearning();
                    rig.feed (makePinkNoise (numChannels, (int) (20.0 * sampleRate), -20.0f, 3), 512);
                    const auto& p = rig.profile();

                    float worstTame = 0.0f, worstRecover = 0.0f;
                    for (int b = 0; b < numBands; ++b)
                    {
                        worstTame = juce::jmax (worstTame, p.tameEvidenceDb[(size_t) b]);
                        worstRecover = juce::jmax (worstRecover, p.recoverEvidenceDb[(size_t) b]);
                    }

                    logMessage ("  pink " + tag + ": worst tame " + juce::String (worstTame, 4) + " dB, worst recover "
                                + juce::String (worstRecover, 4) + " dB, slope " + juce::String (p.spectralSlopeDbPerOct.value, 3)
                                + " dB/oct, low " + juce::String (p.lowShare.value, 3) + ", high " + juce::String (p.highShare.value, 3));
                    expect (worstTame < 0.05f && worstRecover < 0.05f, "pink noise must not produce evidence " + tag);
                    expect (p.spectralSlopeDbPerOct.valid && std::abs (p.spectralSlopeDbPerOct.value) < 0.2f, "pink slope " + tag);
                    expectWithinAbsoluteError (p.lowShare.value, 5.0f / 27.0f, 0.02f, "pink low share " + tag);
                    expectWithinAbsoluteError (p.highShare.value, 5.0f / 27.0f, 0.02f, "pink high share " + tag);
                }

                {
                    Rig rig (sampleRate);
                    rig.analyzer.startLearning();
                    rig.feed (makeWhiteNoise (numChannels, (int) (20.0 * sampleRate), -20.0f, 4), 512);
                    const auto& p = rig.profile();

                    double low = 0.0, high = 0.0, total = 0.0;
                    for (int b = 0; b < numBands; ++b)
                    {
                        const auto width = bandCentreHz (b);   // 段宽与中心频率成正比
                        total += width;
                        low += b <= 4 ? width : 0.0;
                        high += b >= 22 ? width : 0.0;
                    }

                    expect (p.spectralSlopeDbPerOct.valid);
                    expectWithinAbsoluteError (p.spectralSlopeDbPerOct.value, 3.0103f, 0.1f, "white slope " + tag);
                    expectWithinAbsoluteError (p.lowShare.value, (float) (low / total), 0.005f, "white low share " + tag);
                    expectWithinAbsoluteError (p.highShare.value, (float) (high / total), 0.005f, "white high share " + tag);

                    float worst = 0.0f;
                    for (int b = 0; b < numBands; ++b)
                        worst = juce::jmax (worst, juce::jmax (p.tameEvidenceDb[(size_t) b], p.recoverEvidenceDb[(size_t) b]));

                    expect (worst < 0.05f, "a straight tilt is not a deviation from the local contour " + tag);
                }
            }
        }

        // 依据：分析器第 4 节（正偏离 → Tame 证据、负偏离 → Recover 证据，分开统计）。粉噪上 −12 dB 的 1/3 倍频程缺口（段 13
        // 精确中心 794 Hz）：该段 Recover 证据 ≥ 3 dB，其余各段两种证据都 < 0.2 dB（轮廓按 Huber 降权重算，缺口不会把邻段的
        // 轮廓拉低、凭空记上 Tame）；+12 dB 的峰（段 14，1 kHz）对称。
        void testLiveBroadbandSlope()
        {
            beginTest ("live broad-band slope follows stopped learning, and rejects narrow tones and silence");
            constexpr double rate = 48000.0;
            const auto flat = makePinkNoise (numChannels, (int) (8.0 * rate), -20.0f, 871);
            const auto bright = makeWhiteNoise (numChannels, (int) (8.0 * rate), -20.0f, 872);
            Rig rig (rate);
            rig.analyzer.startLearning();
            rig.feed (flat, 512);
            rig.analyzer.stopLearning();
            const auto frozen = rig.profile();
            const auto baseline = rig.analyzer.getShortTermView().liveSpectralSlopeDbPerOct;
            expect (baseline.valid && std::abs (baseline.value) < 0.4f);
            rig.feed (bright, 512);
            const auto slope = rig.analyzer.getShortTermView().liveSpectralSlopeDbPerOct;
            expect (slope.valid && slope.value > 2.5f && slope.value < 3.5f);
            expect (sameProfile (frozen, rig.profile()), "live slope never changes the committed learning result");
            rig.feed (makeSilence (numChannels, (int) (2.0 * rate)), 512);
            expect (! rig.analyzer.getShortTermView().liveSpectralSlopeDbPerOct.valid);
            rig.analyzer.handleDiscontinuity (Discontinuity::hostReset);
            rig.feed (makeSine (numChannels, rate, 1000.0, 0.1f, 5.0), 512);
            expect (! rig.analyzer.getShortTermView().liveSpectralSlopeDbPerOct.valid, "a narrow instrument is not a broad tonal balance");
        }

        void testLiveTameAfterLearning()
        {
            beginTest ("live Tame evidence follows a new narrow peak after learning, settles and releases without changing the profile");
            constexpr double rate = 48000.0;
            const auto flat = makePinkNoise (numChannels, (int) (8.0 * rate), -20.0f, 714);
            auto peak = makePinkNoise (numChannels, (int) (2.0 * rate), -20.0f, 715);
            applyPeaking (peak, rate, 2000.0, 14.0);

            Rig rig (rate);
            rig.analyzer.startLearning();
            rig.feed (flat, 512);
            rig.analyzer.stopLearning();
            const auto frozen = rig.profile();
            const auto baseline = rig.analyzer.getShortTermView().liveTameEvidenceDb;
            expect (baseline.valid);
            expect (baseline.value < 0.4f, "flat noise gives little live authority");
            rig.feed (peak, 512);
            const auto raised = rig.analyzer.getShortTermView().liveTameEvidenceDb;
            expect (raised.valid && raised.value > baseline.value + 0.5f, "a new persistent peak is recognised within two seconds");
            expect (! rig.analyzer.isLearning() && sameProfile (frozen, rig.profile()), "live observation never changes the frozen profile");

            rig.feed (flat, 512);
            const auto relaxed = rig.analyzer.getShortTermView().liveTameEvidenceDb.value;
            expect (relaxed < raised.value * 0.3f, "a flat section releases the extra authority");
            rig.feed (peak, 512);
            rig.feed (makeSilence (numChannels, (int) (4.0 * rate)), 512);
            expect (rig.analyzer.getShortTermView().liveTameEvidenceDb.value < 0.02f, "silence clears live evidence instead of keeping the old peak");
            expect (sameProfile (frozen, rig.profile()));

            rig.analyzer.handleDiscontinuity (Discontinuity::hostReset);
            expect (! rig.analyzer.getShortTermView().liveTameEvidenceDb.valid);
            rig.feed (makePinkNoise (numChannels, (int) (0.5 * rate), -20.0f, 716), 512);
            expect (! rig.analyzer.getShortTermView().liveTameEvidenceDb.valid, "a restarted estimator must settle before granting authority");

            rig.feed (peak, 512);
            const auto liveBeforeRelearn = rig.analyzer.getShortTermView().liveTameEvidenceDb;
            expect (liveBeforeRelearn.valid && liveBeforeRelearn.value > 0.5f);
            rig.analyzer.startLearning();
            const auto liveAfterRelearn = rig.analyzer.getShortTermView().liveTameEvidenceDb;
            expect (liveAfterRelearn.valid == liveBeforeRelearn.valid);
            expectEquals (liveAfterRelearn.value, liveBeforeRelearn.value, "relearning clears only the candidate, never the ongoing live detector");
            expectEquals (liveAfterRelearn.effectiveSeconds, liveBeforeRelearn.effectiveSeconds);
        }

        void testNotchAndPeakEvidence()
        {
            beginTest ("spectrum: a notch yields Recover evidence in its band only, a peak Tame evidence in its band only");

            constexpr double sampleRate = 48000.0;

            for (auto [band, gainDb] : { std::pair<int, double> { 13, -12.0 }, std::pair<int, double> { 14, 12.0 },
                                         std::pair<int, double> { 8, -12.0 }, std::pair<int, double> { 20, 12.0 } })
            {
                auto input = makePinkNoise (numChannels, (int) (20.0 * sampleRate), -20.0f, 5 + band);
                applyPeaking (input, sampleRate, bandCentreHz (band), gainDb);

                Rig rig (sampleRate);
                rig.analyzer.startLearning();
                rig.feed (input, 512);
                const auto& p = rig.profile();
                const auto tag = "band " + juce::String (band) + (gainDb < 0.0 ? " notch" : " peak");
                const auto& target = gainDb < 0.0 ? p.recoverEvidenceDb : p.tameEvidenceDb;
                const auto& other = gainDb < 0.0 ? p.tameEvidenceDb : p.recoverEvidenceDb;

                logMessage ("  " + tag + ": evidence " + juce::String (target[(size_t) band], 2) + " dB");
                expect (target[(size_t) band] >= 3.0f, tag);

                float strayTarget = 0.0f, strayOther = 0.0f;
                for (int b = 0; b < numBands; ++b)
                {
                    if (b != band)
                        strayTarget = juce::jmax (strayTarget, target[(size_t) b]);

                    strayOther = juce::jmax (strayOther, other[(size_t) b]);
                }

                expect (strayTarget < 0.2f, "no evidence of the same kind in other bands: " + tag + " (" + juce::String (strayTarget, 3) + ")");
                expect (strayOther < 0.2f, "no evidence of the opposite kind anywhere: " + tag + " (" + juce::String (strayOther, 3) + ")");
                expect (p.bandConfidence[(size_t) band] > 0.5f, tag);
            }
        }

        // 依据：分析器第 4 节「Recover 比 Tame 保守：只对持续缺失、且在有效带宽内的段；高频滚降、编曲留出的空不去填」。
        //   - 5 kHz、24 dB/oct 低通的粉噪：滚降区与其余各段都没有 Recover；
        //   - 两端的段没有外侧邻段（分不清缺口还是滚降）：16 kHz 段挖缺口也不给 Recover；
        //   - 只在 30 % 时间出现的缺口（缺失占比 < 50 %）：不给 Recover；同样占比的峰照样给 Tame（Tame 不要求持续）。
        void testRecoverIsConservative()
        {
            beginTest ("Recover stays conservative: roll-offs, edge bands and intermittent gaps are not filled");

            constexpr double sampleRate = 48000.0;
            auto run = [&] (const Signal& input)
            {
                auto rig = std::make_unique<Rig> (sampleRate);
                rig->analyzer.startLearning();
                rig->feed (input, 512);
                return rig;
            };

            {
                auto input = makePinkNoise (numChannels, (int) (20.0 * sampleRate), -20.0f, 13);
                applyLowpass24 (input, sampleRate, 5000.0);
                const auto rig = run (input);

                float worst = 0.0f;
                for (int b = 0; b < numBands; ++b)
                    worst = juce::jmax (worst, rig->profile().recoverEvidenceDb[(size_t) b]);

                expect (worst < 0.05f, "a steep roll-off is not a gap to fill (" + juce::String (worst, 3) + ")");
            }

            {
                auto input = makePinkNoise (numChannels, (int) (20.0 * sampleRate), -20.0f, 14);
                applyPeaking (input, sampleRate, bandCentreHz (26), -12.0);
                const auto rig = run (input);
                expectEquals (rig->profile().recoverEvidenceDb[26], 0.0f, "the 16 kHz edge band never gets Recover");
            }

            {
                const auto base = makePinkNoise (numChannels, (int) (20.0 * sampleRate), -20.0f, 17);
                auto notched = base, peaked = base, input = base;
                applyPeaking (notched, sampleRate, bandCentreHz (13), -12.0);
                applyPeaking (peaked, sampleRate, bandCentreHz (19), 12.0);

                for (size_t ch = 0; ch < input.size(); ++ch)
                    for (size_t i = 0; i < input[ch].size(); ++i)
                    {
                        const auto phase = std::fmod ((double) i / sampleRate, 1.0);

                        if (phase < 0.3)
                            input[ch][i] = notched[ch][i];
                        else if (phase >= 0.5 && phase < 0.8)
                            input[ch][i] = peaked[ch][i];
                    }

                const auto rig = run (input);
                expectEquals (rig->profile().recoverEvidenceDb[13], 0.0f, "a gap present 30 % of the time is not filled");
                expect (rig->profile().tameEvidenceDb[19] > 1.0f, "a peak present 30 % of the time still counts for Tame");
            }
        }

        // 依据：分析器第 4 节「两声道按功率合并，不先混成单声道（R = −L 时相加为零但并不安静）」；设计规格-总体「单声道按
        // 左右相同的双声道」。R = −L 与 R = L 的画像逐位相同（K 加权、FFT 都是线性的，取反只改符号）。单声道与左右相同的
        // 立体声：频谱（Balance 的功率平均是精确的 2 倍再乘 0.5）与起音逐位相同；响度类只差表能量的末位舍入（见子块用例），
        // 容差 1e-4。
        void testChannelsCombineByPower()
        {
            beginTest ("channels combine by power: R = -L equals R = L (bit-exact), mono equals identical stereo");

            constexpr double sampleRate = 48000.0;
            auto source = makePinkNoise (1, (int) (8.0 * sampleRate), -20.0f, 23);
            addInto (source, makeDrumPattern (sampleRate, 8.0, 3));   // makeDrumPattern 是双声道：只加第一声道

            const Signal same { source[0], source[0] };
            Signal inverted { source[0], source[0] };
            for (auto& x : inverted[1])
                x = -x;

            Rig a (sampleRate), b (sampleRate), m (sampleRate, 1);

            for (auto* rig : { &a, &b, &m })
                rig->analyzer.startLearning();

            a.feed (same, 512);
            b.feed (inverted, 512);
            m.feed (source, 512);

            expect (sameProfile (a.profile(), b.profile()), "R = -L must measure like R = L");
            expect (a.profile().integratedLufs.valid && a.profile().onsetDensityPerSecond.value > 0.0f);

            const auto& s = a.profile();
            const auto& mono = m.profile();
            expectWithinAbsoluteError (mono.integratedLufs.value, s.integratedLufs.value, 1.0e-4f, "mono I");
            expectWithinAbsoluteError (mono.loudnessRangeLu.value, s.loudnessRangeLu.value, 1.0e-4f, "mono LRA");
            expectWithinAbsoluteError (mono.plrDb.value, s.plrDb.value, 1.0e-4f, "mono PLR");
            expect (sameFeature (mono.truePeakDbtp, s.truePeakDbtp) && sameFeature (mono.spectralSlopeDbPerOct, s.spectralSlopeDbPerOct)
                    && sameFeature (mono.lowShare, s.lowShare) && sameFeature (mono.highShare, s.highShare)
                    && sameFeature (mono.onsetDensityPerSecond, s.onsetDensityPerSecond) && sameFeature (mono.onsetStrengthDb, s.onsetStrengthDb)
                    && sameArray (mono.bandLongTermDb, s.bandLongTermDb) && sameArray (mono.tameEvidenceDb, s.tameEvidenceDb)
                    && sameArray (mono.recoverEvidenceDb, s.recoverEvidenceDb),
                    "mono spectrum and onsets equal identical stereo");
        }

        //==============================================================================
        // 依据：分析器第 5 节（快 / 慢包络相对突起、约 50 ms 不应期、迟滞；输出次 / 秒）。点击串（48 样本、−6 dBFS）
        // 2 / 4 / 8 / 16 次每秒，第一击在 0.5 s（避开重启后 250 ms 的预热）：事件数与点击数完全相同，密度 = 事件数 / 有效时长。
        void testClickTrains()
        {
            beginTest ("onsets: click trains are counted exactly (2, 4, 8 and 16 per second)");

            constexpr double sampleRate = 48000.0;

            for (auto rate : { 2.0, 4.0, 8.0, 16.0 })
            {
                int clicks = 0;
                const auto input = makeClickTrain (sampleRate, rate, 20.0, 0.5f, 0.5, clicks);

                Rig rig (sampleRate);
                rig.analyzer.startLearning();
                rig.feed (input, 512);
                const auto& density = rig.profile().onsetDensityPerSecond;
                const auto tag = juce::String (rate, 0) + " per second";

                expect (density.valid, tag);
                expectEquals (onsetCountOf (rig.profile()), clicks, "every click is one event: " + tag);
                expectWithinAbsoluteError (density.value, (float) rate, (float) (0.03 * rate), "density: " + tag);
                expect (rig.profile().onsetStrengthDb.valid && rig.profile().onsetStrengthDb.value >= 6.0f, tag);
            }
        }

        // 依据：分析器第 5 节「必测反例：持续低频正弦、持续低音不得被判为起音」。20 / 30 / 40 / 55 Hz 正弦（−10 dBFS）、
        // 41 Hz 锯齿式低音（两个声部失谐 0 / 0.7 / 3 / 6 Hz 的拍频）、55 Hz 低音加 0.5 / 4 / 8 Hz 颤音（30 ～ 80 % 深度），各 20 s：
        // 事件数 0（密度有效且为 0，强度「不知道」）。同时核对第 4 节「持续低音与间歇重底鼓不走同一策略」要用的低频电平范围：
        // 持续低音 ≤ 8 dB（实测正弦 0、锯齿低音 3 ～ 6.5、颤音更大一些；间歇底鼓见鼓型用例 ≥ 15 dB）。
        void testSustainedBassNeverTriggers()
        {
            beginTest ("onsets: sustained low sines and sustained bass (beating, tremolo) never trigger");

            constexpr double sampleRate = 48000.0;
            std::vector<std::pair<juce::String, Signal>> cases;

            for (auto frequency : { 20.0, 30.0, 40.0, 55.0 })
                cases.push_back ({ "sine " + juce::String (frequency, 0) + " Hz",
                                   makeSine (numChannels, sampleRate, frequency, juce::Decibels::decibelsToGain (-10.0f), 20.0) });

            for (auto detune : { 0.0, 0.7, 3.0, 6.0 })
                cases.push_back ({ "bass 41 Hz detuned " + juce::String (detune, 1) + " Hz",
                                   makeSustainedBass (sampleRate, 20.0, 41.2, detune, -10.0f) });

            for (auto [rate, depth] : { std::pair<double, double> { 0.5, 0.3 }, std::pair<double, double> { 4.0, 0.5 },
                                        std::pair<double, double> { 8.0, 0.8 } })
                cases.push_back ({ "bass 55 Hz tremolo " + juce::String (rate, 1) + " Hz depth " + juce::String (depth, 1),
                                   makeSustainedBass (sampleRate, 20.0, 55.0, 0.0, -10.0f, rate, depth) });

            for (const auto& [caseName, input] : cases)
            {
                Rig rig (sampleRate);
                rig.analyzer.startLearning();
                rig.feed (input, 512);
                const auto& p = rig.profile();
                const auto& lowRange = rig.analyzer.getAuxiliaryFeatures().lowBandRangeDb;

                expect (p.onsetDensityPerSecond.valid, caseName);
                expectEquals (p.onsetDensityPerSecond.value, 0.0f, caseName);
                expect (! p.onsetStrengthDb.valid, "no events: strength is unknown, not 0 (" + caseName + ")");

                if (! caseName.contains ("tremolo"))
                    expect (lowRange.valid && lowRange.value <= 8.0f, "sustained bass keeps a narrow low-band range: " + caseName
                                                                        + " (" + juce::String (lowRange.value, 1) + " dB)");
            }
        }

        // 依据：分析器第 5 节（能量突起不等于鼓点，但鼓型的密度应当合理）。120 BPM：底鼓每拍、军鼓叠在二四拍、十六分音符踩镲
        // （比军鼓低约 14 dB）。明显的突起是每拍一次（2 次 / 秒）：密度在 1.8 ～ 2.2 之间；辅助谱通量的突起不少于包络事件
        // （踩镲在 250 Hz 以上很明显），一致性系数 = 1。第 4 节：间歇的底鼓让低频电平范围 ≥ 15 dB（实测 24 dB）。
        void testDrumPattern()
        {
            beginTest ("onsets: a dense drum pattern gives a plausible density, corroborated by the high-band flux");

            Rig rig (48000.0);
            rig.analyzer.startLearning();
            rig.feed (makeDrumPattern (48000.0, 20.0, 9), 512);
            const auto& p = rig.profile();
            const auto& aux = rig.analyzer.getAuxiliaryFeatures();

            logMessage ("  drums: density " + juce::String (p.onsetDensityPerSecond.value, 3) + " /s, strength "
                        + juce::String (p.onsetStrengthDb.value, 2) + " dB, flux onsets " + juce::String (aux.highBandFluxOnsetsPerSecond.value, 2)
                        + " /s, agreement " + juce::String (aux.onsetAgreement, 2) + ", low-band range "
                        + juce::String (aux.lowBandRangeDb.value, 1) + " dB");
            expect (p.onsetDensityPerSecond.valid && p.onsetDensityPerSecond.value >= 1.8f && p.onsetDensityPerSecond.value <= 2.2f);
            expect (p.onsetStrengthDb.valid && p.onsetStrengthDb.value >= 6.0f);
            expect (aux.highBandFluxOnsetsPerSecond.valid && aux.highBandFluxOnsetsPerSecond.value >= p.onsetDensityPerSecond.value);
            expectEquals (aux.onsetAgreement, 1.0f);
            expect (aux.lowBandRangeDb.valid && aux.lowBandRangeDb.value >= 15.0f,
                    "intermittent kicks give a wide low-band range (" + juce::String (aux.lowBandRangeDb.value, 1) + " dB)");
            expect (sameFeature (p.lowBandRangeDb, aux.lowBandRangeDb), "the profile carries the low-band range for the planner (Glue risk)");
        }

        //==============================================================================
        // 依据：时间线与保存第 2 节「累计有效音频样本（静音与门下的块不算）」；分析器第 2 节「按新增音频样本计」。
        // 5 s 粉噪 + 5 s 数字静音 + 5 s 粉噪：约 10 s（400 ms 窗的边缘最多多算 0.3 s）；5 s 粉噪 + 5 s 低 30 dB 的粉噪
        // （淡出、间歇，低于学习区间 I 20 LU 以上）：约 5 s。
        void testEffectiveTime()
        {
            beginTest ("effective time counts only new, audible samples (silence and passages 20 LU below I do not count)");

            constexpr double sampleRate = 48000.0;
            const auto part = makePinkNoise (numChannels, (int) (5.0 * sampleRate), -20.0f, 31);

            {
                auto input = part;
                append (input, makeSilence (numChannels, (int) (5.0 * sampleRate)));
                append (input, part);
                Rig rig (sampleRate);
                rig.analyzer.startLearning();
                rig.feed (input, 512);
                expectWithinAbsoluteError (rig.profile().effectiveSeconds, 10.0, 0.4, "silence does not count");
            }

            {
                auto quiet = part;
                scale (quiet, juce::Decibels::decibelsToGain (-30.0f));
                auto input = part;
                append (input, quiet);
                Rig rig (sampleRate);
                rig.analyzer.startLearning();
                rig.feed (input, 512);
                expectWithinAbsoluteError (rig.profile().effectiveSeconds, 5.0, 0.4, "a passage 30 dB down does not count");
            }
        }

        // 依据：MusicAnalyzer.h「startLearning 清空候选累计并开始累计；stopLearning 停止累计（保留结果）」「停止后固定」。
        // 学习之前的 −30 LUFS 不进区间：I = −20（容差同正弦用例）；停止后再送 −10 LUFS，画像逐位不变。不学习时短期视图照常更新。
        void testLearningInterval()
        {
            beginTest ("learning interval: data before startLearning is excluded, stopLearning freezes the profile");

            constexpr double sampleRate = 48000.0;
            Rig rig (sampleRate);
            rig.feed (makeLoudnessSine (sampleRate, -30.0, 10.0), 512);
            expect (! rig.analyzer.isLearning());
            expect (! rig.profile().integratedLufs.valid, "nothing is accumulated before learning starts");
            expectWithinAbsoluteError (rig.analyzer.getShortTermView().shortTermLufs.value, -30.0f, 0.005f, "the view updates while not learning");

            rig.analyzer.startLearning();
            expect (rig.analyzer.isLearning());
            rig.feed (makeLoudnessSine (sampleRate, -20.0, 10.0), 512);
            expectWithinAbsoluteError (rig.profile().integratedLufs.value, -20.0f, 0.01f, "I covers the learning interval only");
            expectWithinAbsoluteError (rig.profile().effectiveSeconds, 10.0, 0.001);

            rig.analyzer.stopLearning();
            expect (! rig.analyzer.isLearning());
            const auto frozen = rig.profile();
            const auto frozenAux = rig.analyzer.getAuxiliaryFeatures();
            rig.feed (makeLoudnessSine (sampleRate, -10.0, 5.0), 512);
            expect (sameProfile (frozen, rig.profile()) && sameAuxiliary (frozenAux, rig.analyzer.getAuxiliaryFeatures()),
                    "the profile is fixed after stopLearning");
            expectWithinAbsoluteError (rig.analyzer.getShortTermView().shortTermLufs.value, -10.0f, 0.005f);

            // 重新开始学习：候选累计清空。
            rig.analyzer.startLearning();
            expect (! rig.profile().integratedLufs.valid && rig.profile().effectiveSeconds == 0.0);
        }

        // 依据：分析器第 6 节「「不知道」不当 0」「LRA 置信度单列：前 60 s 标未稳定」；设计规格-GLAZE-时间线与保存
        // 第 2 节（主要特征置信度够才结束聆听）。宿主给了连续的播放位置（代表性 = 1）：20 s 后 I 置信度 1、LRA 置信度 < 0.35；
        // 70 s 后 LRA 置信度 1。刚开始什么都无效、置信度 0；纯正弦只有一两段在静音门上，斜率无效；没有起音时强度无效。
        void testUnknownIsNotZero()
        {
            beginTest ("unknown is not zero: invalid until measured; LRA confidence stays below 1 for the first 60 s");

            constexpr double sampleRate = 48000.0;
            Rig rig (sampleRate);
            rig.analyzer.startLearning();

            for (const auto* f : { &rig.profile().integratedLufs, &rig.profile().loudnessRangeLu, &rig.profile().plrDb,
                                   &rig.profile().truePeakDbtp, &rig.profile().spectralSlopeDbPerOct, &rig.profile().lowShare,
                                   &rig.profile().highShare, &rig.profile().onsetDensityPerSecond, &rig.profile().onsetStrengthDb })
                expect (! f->valid && f->confidence == 0.0f);

            expect (! rig.analyzer.getShortTermView().momentaryLufs.valid && ! rig.analyzer.getShortTermView().musicActive);

            auto host = [] (std::int64_t fed, int) { return playingAt (fed); };
            rig.feed (makeLoudnessSine (sampleRate, -20.0, 2.0), 512, 1, host);
            expect (rig.profile().integratedLufs.valid);
            expect (! rig.profile().spectralSlopeDbPerOct.valid, "a pure sine does not define a spectral slope");
            expect (rig.profile().onsetDensityPerSecond.valid && rig.profile().onsetDensityPerSecond.value == 0.0f);
            expect (! rig.profile().onsetStrengthDb.valid, "no events: strength unknown");
            expect (! rig.profile().loudnessRangeLu.valid, "LRA needs at least 1 s of 3 s short-term values");

            Rig music (sampleRate);
            music.analyzer.startLearning();
            auto input = makePinkNoise (numChannels, (int) (70.0 * sampleRate), -20.0f, 41);
            Signal first (numChannels), rest (numChannels);

            for (size_t ch = 0; ch < input.size(); ++ch)
            {
                first[ch].assign (input[ch].begin(), input[ch].begin() + (std::ptrdiff_t) (20.0 * sampleRate));
                rest[ch].assign (input[ch].begin() + (std::ptrdiff_t) (20.0 * sampleRate), input[ch].end());
            }

            music.feed (first, 512, 1, host);
            expectWithinAbsoluteError (music.profile().representativeness, 1.0f, 1.0e-6f);
            expectWithinAbsoluteError (music.profile().integratedLufs.confidence, 1.0f, 1.0e-6f);
            expect (music.profile().loudnessRangeLu.valid && music.profile().loudnessRangeLu.confidence < 0.35f,
                    "LRA is not stable after 20 s");

            music.feed (rest, 512, 1, host);
            expectWithinAbsoluteError (music.profile().loudnessRangeLu.confidence, 1.0f, 1.0e-6f, "LRA is stable after 60 s of values");
        }

        //==============================================================================
        // 依据：时间线与保存第 3 节「丢弃跨越不连续点的窗口，各特征在各自窗口重新填满后恢复（M 0.4 s、S 3 s）；已累计的有效时长保留」。
        // 跳播正好落在子块边界上：跨越它的子块（含 64 样本真峰值保护）丢掉，M 在 0.5 s 后、S 在 3.1 s 后恢复；有效时长接着累计。
        // 从 −40 dBFS 跳到 −10 dBFS：告知了不连续点时不算起音（拼接不是音乐里的起音），没告知时这一下电平台阶是一个事件。
        void testDiscontinuity()
        {
            beginTest ("discontinuities: windows refill (M 0.4 s, S 3 s), accumulations are kept, splices are not onsets");

            constexpr double sampleRate = 48000.0;
            auto seconds = [&] (double s) { return (int) std::lround (s * sampleRate); };

            Rig rig (sampleRate);
            rig.analyzer.startLearning();
            rig.feed (makePinkNoise (numChannels, seconds (10.0), -20.0f, 51), 480);
            const auto before = rig.profile().effectiveSeconds;
            expect (before > 9.9);

            rig.analyzer.handleDiscontinuity (Discontinuity::transportJump);
            const auto& view = rig.analyzer.getShortTermView();
            expect (! view.momentaryLufs.valid && ! view.shortTermLufs.valid && ! view.psr3sDb.valid && ! view.musicActive);

            const auto after = makePinkNoise (numChannels, seconds (3.3), -20.0f, 52);
            Signal part (numChannels);
            size_t done = 0;
            auto feedUntil = [&] (double s)
            {
                const auto end = (size_t) seconds (s);

                for (size_t ch = 0; ch < after.size(); ++ch)
                    part[ch].assign (after[ch].begin() + (std::ptrdiff_t) done, after[ch].begin() + (std::ptrdiff_t) end);

                rig.feed (part, 480);
                done = end;
            };

            feedUntil (0.45);
            expect (! view.momentaryLufs.valid, "M needs four clean sub-blocks after the jump");
            feedUntil (0.55);
            expect (view.momentaryLufs.valid, "M is back after 0.5 s");
            expect (! view.shortTermLufs.valid);
            feedUntil (3.05);
            expect (! view.shortTermLufs.valid, "S needs 30 clean sub-blocks after the jump");
            feedUntil (3.3);
            expect (view.shortTermLufs.valid, "S is back after 3.1 s");
            expect (rig.profile().effectiveSeconds > before + 2.5, "the effective time keeps what was learned and continues");

            for (int announce = 1; announce >= 0; --announce)
            {
                Rig splice (sampleRate);
                splice.analyzer.startLearning();
                auto quiet = makePinkNoise (numChannels, seconds (5.0), -40.0f, 53);
                splice.feed (quiet, 512);

                if (announce == 1)
                    splice.analyzer.handleDiscontinuity (Discontinuity::loopWrap);

                splice.feed (makePinkNoise (numChannels, seconds (5.0), -10.0f, 54), 512);
                expectEquals (onsetCountOf (splice.profile()), announce == 1 ? 0 : 1,
                              announce == 1 ? "an announced splice is not an onset" : "an unannounced level step is one onset");
            }
        }

        // 依据：时间线与保存第 3 节「宿主 reset：清运行状态，保留画像；若正在聆听，已累计的有效时长保留」；MusicAnalyzer.h
        // 「时间轴 = IN 表的累计样本数；Balance 单独被 reset（restartWithFadeIn 只清模块）也对得上；对不上就在本块起点断开」。
        //   a) 整条链 reset（表与 Balance 的样本计数归零）+ handleDiscontinuity (hostReset)：学习在新时间轴上继续；
        //   b) 同上但没告知分析器：自己核对到计数变少，照样接上；
        //   c) 只 reset Balance：帧流在那里断开、之后照常累计，子块流不受影响。
        void testHostAndModuleResets()
        {
            beginTest ("host resets and module-only resets: learning continues on the new time axis");

            constexpr double sampleRate = 48000.0;
            const auto first = makePinkNoise (numChannels, (int) (8.0 * sampleRate), -20.0f, 61);
            const auto second = makePinkNoise (numChannels, (int) (8.0 * sampleRate), -20.0f, 62);

            for (int variant = 0; variant < 3; ++variant)
            {
                Rig rig (sampleRate);
                rig.analyzer.startLearning();
                rig.feed (first, 512);
                const auto spectrumSecondsBefore = rig.profile().lowShare.effectiveSeconds;

                if (variant <= 1)
                {
                    rig.meter.reset();
                    rig.referenceCursor = 0;
                }

                rig.balance.reset();

                if (variant == 0)
                    rig.analyzer.handleDiscontinuity (Discontinuity::hostReset);

                rig.feed (second, 512);
                const auto& p = rig.profile();
                const auto tag = juce::String (variant == 0 ? "chain reset, announced" : variant == 1 ? "chain reset, not announced" : "balance only");

                expectWithinAbsoluteError (p.effectiveSeconds, 16.0, 0.5, "effective time continues: " + tag);
                expectWithinAbsoluteError (p.integratedLufs.value, (float) referenceIntegrated (meanSquaresOf (rig.subBlocks)), 0.3f, tag);
                expect (p.lowShare.effectiveSeconds > spectrumSecondsBefore + 6.5, "spectral statistics continue: " + tag);
                expect (rig.analyzer.getShortTermView().shortTermLufs.valid, "the view refills: " + tag);

                float worst = 0.0f;
                for (int b = 0; b < numBands; ++b)
                    worst = juce::jmax (worst, juce::jmax (p.tameEvidenceDb[(size_t) b], p.recoverEvidenceDb[(size_t) b]));

                expect (worst < 0.05f, "no spurious evidence around the reset: " + tag);
            }
        }

        void testSpectrumDeliveryAcrossInputSlices()
        {
            beginTest ("one spectral delivery per host block is independent of input statistics slices; resets and missing Balance remain detectable");
            constexpr double sampleRate = 48000.0;
            constexpr int hostSize = 1024;
            auto input = makePinkNoise (numChannels, (int) (8.0 * sampleRate), -20.0f, 638);
            applyPeaking (input, sampleRate, 1000.0, 12.0);
            Rig whole (sampleRate), sliced (sampleRate);
            whole.analyzer.startLearning();
            sliced.analyzer.startLearning();
            juce::AudioBuffer<float> block (numChannels, hostSize);
            auto feedHost = [&] (Rig& rig, int start, int length, bool fragment, bool skipBalance = false)
            {
                for (int offset = 0; offset < length;)
                {
                    const auto n = juce::jmin (length - offset, fragment ? 127 : hostSize);
                    block.setSize (numChannels, n, false, false, true);
                    for (int ch = 0; ch < numChannels; ++ch)
                        block.copyFrom (ch, 0, input[(size_t) ch].data() + start + offset, n);
                    rig.meter.process (block);
                    rig.analyzer.processInput (block, rig.meter);
                    if (! skipBalance)
                        rig.balance.process (block);
                    offset += n;
                }
                rig.analyzer.processSpectrum (rig.balance);
            };
            const auto initialLength = (int) (6.0 * sampleRate);
            for (int start = 0; start < initialLength; start += hostSize)
            {
                const auto n = juce::jmin (hostSize, initialLength - start);
                feedHost (whole, start, n, false);
                feedHost (sliced, start, n, true);
            }
            const auto& a = whole.analyzer.getShortTermView();
            const auto& b = sliced.analyzer.getShortTermView();
            expect (a.liveTameEvidenceDb.valid && b.liveTameEvidenceDb.valid,
                    "continuous evidence remains valid: whole=" + juce::String ((int) a.liveTameEvidenceDb.valid)
                    + ", sliced=" + juce::String ((int) b.liveTameEvidenceDb.valid));
            expectEquals (b.liveTameEvidenceDb.value, a.liveTameEvidenceDb.value, "Tame sees the identical delivered frames");
            expectEquals (b.liveSpectralSlopeDbPerOct.value, a.liveSpectralSlopeDbPerOct.value, "Tone sees the identical delivered frames");
            expectEquals (sliced.profile().lowShare.effectiveSeconds, whole.profile().lowShare.effectiveSeconds,
                          "no false discontinuities remove learned spectral duration");

            sliced.balance.reset();
            sliced.analyzer.processSpectrum (sliced.balance);
            expect (! sliced.analyzer.getShortTermView().liveTameEvidenceDb.valid, "a real Balance reset invalidates short spectral evidence");
            // A complete FFT window must arrive before the one-second spectral settling interval starts.
            // 1.25 s includes that window and hop quantisation; it does not change the production settling gate.
            const auto refillSamples = (int) (1.25 * sampleRate);
            for (int start = initialLength; start < initialLength + refillSamples; start += hostSize)
                feedHost (sliced, start, juce::jmin (hostSize, initialLength + refillSamples - start), true);
            expect (sliced.analyzer.getShortTermView().liveTameEvidenceDb.valid, "spectral evidence refills after reset");
            feedHost (sliced, 0, hostSize, true, true);
            expect (! sliced.analyzer.getShortTermView().liveTameEvidenceDb.valid, "missing Balance processing is still a discontinuity");

            // The meters/modules restart at zero while each analyzer retains its prior input timestamp.
            // A shorter final statistics slice must not move the reset boundary forward and discard extra FFT frames.
            whole.meter.reset();
            whole.balance.reset();
            sliced.meter.reset();
            sliced.balance.reset();
            for (int start = 0; start < refillSamples; start += hostSize)
            {
                const auto n = juce::jmin (hostSize, refillSamples - start);
                feedHost (whole, start, n, false);
                feedHost (sliced, start, n, true);
            }
            expect (a.liveTameEvidenceDb.valid && b.liveTameEvidenceDb.valid, "both input layouts refill after the timeline rolls back");
            expectEquals (b.liveTameEvidenceDb.value, a.liveTameEvidenceDb.value, "reset Tame delivery has the same boundary");
            expectEquals (b.liveSpectralSlopeDbPerOct.value, a.liveSpectralSlopeDbPerOct.value, "reset Tone delivery has the same boundary");
        }

        // 依据：分析器第 6 节「同一段反复循环不增加代表性置信度；拿不到位置时保守降级、不阻止学习」。
        //   - 连续播放 20 s（有位置）：代表性 1；
        //   - 4 s 循环播 5 遍（位置回绕、每次回绕告知 loopWrap）：代表性 ≈ 4 / 15（覆盖的 100 ms 格数；丢掉的跨越子块让它略少）；
        //     各特征的置信度不超过代表性；
        //   - 没有位置、或宿主停着（位置不走）：按有效时长计，但封顶 0.75，学习照常（画像有效）。
        void testRepresentativeness()
        {
            beginTest ("representativeness: repeated loops add nothing; without a host position it is capped at 0.75");

            constexpr double sampleRate = 48000.0;
            const auto input = makePinkNoise (numChannels, (int) (20.0 * sampleRate), -20.0f, 71);
            const auto loopLength = (std::int64_t) (4.0 * sampleRate);

            {
                Rig rig (sampleRate);
                rig.analyzer.startLearning();
                rig.feed (input, 512, 1, [] (std::int64_t fed, int) { return playingAt (fed); });
                expectWithinAbsoluteError (rig.profile().representativeness, 1.0f, 1.0e-6f, "linear playback");
            }

            {
                Rig rig (sampleRate);
                rig.analyzer.startLearning();
                rig.feed (input, 512, 1,
                          [&] (std::int64_t fed, int) { auto info = playingAt (fed % loopLength); info.hasLoop = info.looping = true;
                                                        info.loopEndSamples = loopLength; return info; },
                          [&] (Rig& r) { if (r.fed > 0 && r.fed % loopLength == 0) r.analyzer.handleDiscontinuity (Discontinuity::loopWrap); });

                const auto& p = rig.profile();
                logMessage ("  4 s loop x 5: representativeness " + juce::String (p.representativeness, 3) + ", effective "
                            + juce::String (p.effectiveSeconds, 2) + " s");
                expect (p.representativeness > 0.2f && p.representativeness <= 4.0f / 15.0f + 1.0e-6f, "a loop covers only 4 s");
                expect (p.effectiveSeconds > 19.0, "the loop still counts as effective (listened) time");
                expect (p.integratedLufs.valid && p.integratedLufs.confidence <= p.representativeness + 1.0e-6f);

                for (int b = 0; b < numBands; ++b)
                    expect (p.bandConfidence[(size_t) b] <= p.representativeness + 1.0e-6f);
            }

            for (int stopped = 0; stopped < 2; ++stopped)
            {
                Rig rig (sampleRate);
                rig.analyzer.startLearning();

                if (stopped == 1)
                    rig.feed (input, 512, 1, [] (std::int64_t, int) { auto info = playingAt (12345); info.isPlaying = false; return info; });
                else
                    rig.feed (input, 512);

                const auto& p = rig.profile();
                expectWithinAbsoluteError (p.representativeness, 0.75f, 1.0e-6f, stopped == 1 ? "transport stopped" : "no position");
                expect (p.integratedLufs.valid && p.lowShare.valid, "learning is not blocked without a position");
                expectWithinAbsoluteError (p.integratedLufs.confidence, 0.75f, 1.0e-6f);
            }
        }

        // 依据：MusicAnalyzer.h「三路数据各自按自己的时间戳累计，结果与宿主块长无关」；任务书「块长 1～4096 随机时结果与块长无关」。
        // 混合素材（粉噪 + 800 Hz 缺口 + 鼓型 + 失谐低音，12 s）：块长 512、1、333、4096 与两种随机块长（1 ～ 4096）的画像与
        // 辅助量逐位相同。
        void testBlockSizeIndependence()
        {
            beginTest ("the profile is bit-identical for any host block size (1 ... 4096, random)");

            constexpr double sampleRate = 48000.0;
            auto input = makePinkNoise (numChannels, (int) (12.0 * sampleRate), -26.0f, 81);
            applyPeaking (input, sampleRate, bandCentreHz (13), -12.0);
            addInto (input, makeDrumPattern (sampleRate, 12.0, 4));
            addInto (input, makeSustainedBass (sampleRate, 12.0, 55.0, 0.7, -16.0f));

            Rig reference (sampleRate);
            reference.analyzer.startLearning();
            reference.feed (input, 512);
            expect (reference.profile().integratedLufs.valid && reference.profile().onsetDensityPerSecond.value > 0.0f
                    && reference.profile().recoverEvidenceDb[13] > 1.0f, "the material must exercise all three streams");

            for (auto [blockSize, seed] : { std::pair<int, juce::int64> { 1, 0 }, std::pair<int, juce::int64> { 333, 0 },
                                            std::pair<int, juce::int64> { 4096, 0 }, std::pair<int, juce::int64> { 0, 5 },
                                            std::pair<int, juce::int64> { 0, 77 } })
            {
                Rig rig (sampleRate);
                rig.analyzer.startLearning();
                rig.feed (input, blockSize, seed);
                const auto tag = blockSize > 0 ? "block " + juce::String (blockSize) : "random blocks, seed " + juce::String (seed);
                expect (sameProfile (reference.profile(), rig.profile()), "profile: " + tag);
                expect (sameAuxiliary (reference.analyzer.getAuxiliaryFeatures(), rig.analyzer.getAuxiliaryFeatures()), "auxiliary: " + tag);
            }
        }

        // 依据：任务书「音频线程 0 分配、0 锁」；MusicAnalyzer.h「音频线程不分配」。prepare 之后，处理、读子块 / 帧、
        // 宿主信息、开始 / 停止学习、三种不连续点、读画像与视图都不经 operator new 分配（计数见 TestMain.cpp）。
        void testNoAllocation()
        {
            beginTest ("the audio-thread API does not allocate");

            constexpr double sampleRate = 48000.0;
            Rig rig (sampleRate);
            const auto input = makePinkNoise (numChannels, (int) (4.0 * sampleRate), -20.0f, 91);
            juce::AudioBuffer<float> block (numChannels, 4096);
            juce::Random random (3);
            std::uint64_t allocations = 0;
            float sink = 0.0f;

            for (int start = 0, index = 0; start < (int) input.front().size(); ++index)
            {
                const auto length = juce::jmin (1 + random.nextInt (4096), (int) input.front().size() - start);
                block.setSize (numChannels, length, false, false, true);

                for (int ch = 0; ch < numChannels; ++ch)
                    block.copyFrom (ch, 0, input[(size_t) ch].data() + start, length);

                const auto before = allocationCountOnThisThread();

                rig.analyzer.setHostInfo (playingAt (start));

                if (index == 0)
                    rig.analyzer.startLearning();

                if (index == 20)
                    rig.analyzer.handleDiscontinuity (Discontinuity::transportJump);

                if (index == 30)
                    rig.analyzer.handleDiscontinuity (Discontinuity::loopWrap);

                if (index == 40)
                {
                    rig.analyzer.handleDiscontinuity (Discontinuity::hostReset);
                    rig.meter.reset();
                    rig.balance.reset();
                }

                rig.meter.process (block);
                rig.analyzer.processInput (block, rig.meter);
                rig.balance.process (block);
                rig.analyzer.processSpectrum (rig.balance);
                sink += rig.analyzer.getCandidateProfile().integratedLufs.value + rig.analyzer.getShortTermView().momentaryLufs.value
                      + rig.analyzer.getAuxiliaryFeatures().onsetAgreement;

                if (index == 60)
                    rig.analyzer.stopLearning();

                allocations += allocationCountOnThisThread() - before;
                start += length;
            }

            expect (std::isfinite (sink));
            expectEquals ((int) allocations, 0);
        }

        // 性能护栏（依据：任务书「分析器每块 ≤ 5 µs」；设计规格-GLAZE-界面与实现第 4 节预算）：48 kHz 立体声、块 512，
        // 分析器两步（processInput + processSpectrum）每块多遍取最短再取均值。2026-09-25 本机（M2 Pro、接电源）实测约 2 µs，
        // v0.1.7.0 修坯后（阶段 3）学习中约 4.7 ～ 5.1 µs（本机与 109 的日志）；只拦「每块重算整张直方图、逐样本做 log」这类大退化。
        // v0.1.7.0 根治修订（ADR-012，设计规格-Clay-根治修订第 4 节第 5 条）按设计让学习期多做事：先试后用的 6 路候选（5 个 IIR 双二阶、
        // 4 组线性相位滑动和、M / S 延迟环、块峰值记账）只在学习中跑，另外真次声低通从 2 级变 4 级（学习与否都跑）。所以分两种量：
        //   - 学习中（候选在跑）：门槛 20 → 60 µs。按运算量估计约 20 ～ 30 µs（大头是每样本 4 次 LinearPhaseLowpass::process 的
        //     函数调用与 5 个双二阶；未实测——本代理不编译，服务器日志复核），门槛留约 2 倍；2026-10-01 T-043 后改 75 µs，依据见文件头
        //     learningGuardMicroseconds；
        //   - 不学习（常态，候选不跑）：保持原门槛 20 µs（估计约 6 ～ 8 µs）——候选若在学习区间外也跑，这一条先报出来。
        void testPerformanceGuard()
        {
            beginTest ("performance guard: analyzer per 512-sample block at 48 kHz (learning with the Clay trial; steady state)");

            constexpr double sampleRate = 48000.0;
            constexpr int blockLength = 512;
            auto input = makePinkNoise (numChannels, (int) (10.0 * sampleRate), -24.0f, 101);
            addInto (input, makeDrumPattern (sampleRate, 10.0, 5));
            const auto numBlocks = (int) input.front().size() / blockLength;
            juce::AudioBuffer<float> block (numChannels, blockLength);

            // 返回 {每块均值 µs, 最坏块 µs}：4 遍里每块取最短（排除调度抖动），再对块求均值 / 最大。
            auto measure = [&] (bool learnWhileMeasuring)
            {
                std::vector<juce::int64> best ((size_t) numBlocks, std::numeric_limits<juce::int64>::max());

                for (int pass = 0; pass < 4; ++pass)
                {
                    Rig rig (sampleRate);

                    if (learnWhileMeasuring)
                        rig.analyzer.startLearning();

                    for (int b = 0; b < numBlocks; ++b)
                    {
                        for (int ch = 0; ch < numChannels; ++ch)
                            block.copyFrom (ch, 0, input[(size_t) ch].data() + b * blockLength, blockLength);

                        rig.meter.process (block);
                        const auto t0 = juce::Time::getHighResolutionTicks();
                        rig.analyzer.processInput (block, rig.meter);
                        const auto t1 = juce::Time::getHighResolutionTicks();
                        rig.balance.process (block);
                        const auto t2 = juce::Time::getHighResolutionTicks();
                        rig.analyzer.processSpectrum (rig.balance);
                        const auto t3 = juce::Time::getHighResolutionTicks();
                        best[(size_t) b] = juce::jmin (best[(size_t) b], (t1 - t0) + (t3 - t2));
                    }
                }

                double sum = 0.0, worst = 0.0;

                for (auto ticks : best)
                {
                    const auto seconds = juce::Time::highResolutionTicksToSeconds (ticks);
                    sum += seconds;
                    worst = juce::jmax (worst, seconds);
                }

                return std::pair<double, double> { 1.0e6 * sum / numBlocks, 1.0e6 * worst };
            };

            const auto [learningMean, learningWorst] = measure (true);
            const auto [steadyMean, steadyWorst] = measure (false);

            logMessage ("  analyzer @48k block 512, learning (trial on): mean " + juce::String (learningMean, 2) + " us ("
                        + juce::String (100.0 * learningMean * 1.0e-6 / (blockLength / sampleRate), 3) + " % of one core), worst "
                        + juce::String (learningWorst, 2) + " us; not learning: mean " + juce::String (steadyMean, 2)
                        + " us, worst " + juce::String (steadyWorst, 2) + " us");
            expect (learningMean < learningGuardMicroseconds, "analyzer too slow while learning: " + juce::String (learningMean, 2) + " us per block");
            expect (steadyMean < 20.0, "analyzer too slow when not learning: " + juce::String (steadyMean, 2) + " us per block");
        }
    };

    static MusicAnalyzerTests musicAnalyzerTests;

    //==============================================================================
    // ---- v0.1.7.0 修坯 Clay（T-012 A4 追加）----
    // 分析器新增的超低频占比、立体声画像字段与实时立体声视图（设计规格-Clay-分析决策与验收 1.2 ～ 1.4、第 6 节第 3 条；
    // GlazeTypes.h 画像格式 2 的字段注释；MusicAnalyzer.h / MusicAnalyzer.cpp「四」节）。
    // 只在文件末尾追加：上面的旧用例与工具一个不改，另起一个测试类（名字 MusicAnalyzerClay），复用上面的信号、Rig 与逐位比较。
    // 立体声的期望值建立在 Balance 交叉功率的约定上（BalanceModule.h FrameSnapshot::bandCross）：左右逐位相同时 X = P、
    // R = −L 时 X = −P、单声道 X = P——Balance 每个 bin 的 re·re + im·im 与 reL·reR + imL·imR 是同一种运算（两个 float 的乘积在
    // double 里精确），段内同顺序、同权重求和，所以这几个等式逐位成立；下面「S/M ≤ −100 dB」「逐块衰减正好是 r」才写得这么紧。
    namespace
    {
        using glaze::ShortTermView;

        // 二阶 RBJ 高通（Q = 0.7071），两级串联 = 24 dB/oct（与上面的 applyLowpass24 对称）。
        void applyHighpass24 (Signal& signal, double sampleRate, double frequency)
        {
            const auto w0 = juce::MathConstants<double>::twoPi * frequency / sampleRate;
            const auto alpha = std::sin (w0) / (2.0 * 0.70710678118654752);
            const auto cosW0 = std::cos (w0);
            const auto a0 = 1.0 + alpha;

            for (int stage = 0; stage < 2; ++stage)
                applyBiquad (signal, (1.0 + cosW0) / 2.0 / a0, -(1.0 + cosW0) / a0, (1.0 + cosW0) / 2.0 / a0,
                             -2.0 * cosW0 / a0, (1.0 - alpha) / a0);
        }

        // 由两段单声道信号（各取第 0 声道）拼成立体声：L = common + opposite、R = common − opposite。
        // common 在两声道同相（纯 M），opposite 在两声道反相（纯 S）；长度取两者较短的。
        Signal midSide (const Signal& common, const Signal& opposite)
        {
            const auto length = juce::jmin (common.front().size(), opposite.front().size());
            auto out = makeSilence (numChannels, (int) length);

            for (size_t i = 0; i < length; ++i)
            {
                out[0][i] = common[0][i] + opposite[0][i];
                out[1][i] = common[0][i] - opposite[0][i];
            }

            return out;
        }

        // 各声道从 start 起取 length 个样本。
        Signal sliceOf (const Signal& signal, int start, int length)
        {
            Signal out (signal.size());

            for (size_t ch = 0; ch < signal.size(); ++ch)
                out[ch].assign (signal[ch].begin() + start, signal[ch].begin() + start + length);

            return out;
        }

        // 先试后用的峰值代价（ADR-012，画像格式 2 追加）与守护混合损失（T-043）逐位相同。
        bool sameTrialCosts (const MusicProfile& a, const MusicProfile& b)
        {
            for (size_t i = 0; i < a.monoPeakCostDb.size(); ++i)
                if (! sameFeature (a.monoPeakCostDb[i], b.monoPeakCostDb[i]))
                    return false;

            for (size_t i = 0; i < a.lowCutBlendLossDb.size(); ++i)
                if (! sameFeature (a.lowCutBlendLossDb[i], b.lowCutBlendLossDb[i]))
                    return false;

            return sameFeature (a.lowCutPeakCostDb, b.lowCutPeakCostDb) && sameFeature (a.widthPeakCostDb, b.widthPeakCostDb);
        }

        // 画像格式 2 新增的全部字段（含先试后用的代价）+ 格式版本逐位相同（旧字段用上面的 sameProfile）。
        bool sameClayProfile (const MusicProfile& a, const MusicProfile& b)
        {
            return sameFeature (a.subShareDb, b.subShareDb) && sameFeature (a.lowSideToMidDb, b.lowSideToMidDb)
                && sameFeature (a.lowCorrelation, b.lowCorrelation) && sameFeature (a.highSideToMidDb, b.highSideToMidDb)
                && sameFeature (a.highCorrelation, b.highCorrelation) && sameArray (a.bandSideToMidDb, b.bandSideToMidDb)
                && sameArray (a.bandCorrelation, b.bandCorrelation) && sameArray (a.bandStereoConfidence, b.bandStereoConfidence)
                && sameTrialCosts (a, b) && a.formatVersion == b.formatVersion;
        }

        // 实时立体声视图逐位相同（慢路、快路、高频两项）。
        bool sameLiveStereo (const ShortTermView& a, const ShortTermView& b)
        {
            return sameFeature (a.liveLowCorrelation, b.liveLowCorrelation)
                && sameFeature (a.liveLowCorrelationFast, b.liveLowCorrelationFast)
                && sameFloat (a.liveHighMidPower, b.liveHighMidPower)
                && sameFloat (a.liveHighSidePower, b.liveHighSidePower) && a.liveHighValid == b.liveHighValid;
        }

        // 双线性变换的 n 阶 Butterworth 低通（截止 cutoffHz）在 frequency 处的功率增益：
        // |H|² = 1 / (1 + (tan(πf / fs) / tan(πf₀ / fs))^(2n))——分析器各级 RBJ 低通（Q = 1 / (2·cos((2k − 1)·π / (2n)))，同一 f₀ 预畸变）
        // 串联的精确响应。n = 8、f₀ = 16 Hz 是现在的真次声低通（ADR-012）；n = 4、f₀ = 30 Hz 是原来的，只在测试里当对照。
        double butterworthPowerGain (int order, double frequency, double cutoffHz, double sampleRate)
        {
            const auto ratio = std::tan (juce::MathConstants<double>::pi * frequency / sampleRate)
                             / std::tan (juce::MathConstants<double>::pi * cutoffHz / sampleRate);
            return 1.0 / (1.0 + std::pow (ratio, 2.0 * (double) order));
        }

        // 周期性的「底鼓」：每 period 秒一击，一击 = length 秒长的 frequency 正弦（从相位 0 起），包络由 envelope(t) 给（t 是这一击里的秒）。
        template <typename Envelope>
        Signal makeKickTrain (double sampleRate, double seconds, double frequency, double length, double period, Envelope&& envelope)
        {
            const auto numSamples = (int) (seconds * sampleRate);
            const auto burst = (int) std::lround (length * sampleRate);
            const auto spacing = (int) std::lround (period * sampleRate);
            auto signal = makeSilence (1, numSamples);

            for (int start = 0; start + burst <= numSamples; start += spacing)
            {
                for (int i = 0; i < burst; ++i)
                {
                    const auto t = (double) i / sampleRate;
                    signal[0][(size_t) (start + i)] += (float) (envelope (t)
                                                                * std::sin (juce::MathConstants<double>::twoPi * frequency * t));
                }
            }

            return signal;
        }

        // 双声道：两个声道都取 source 的第 0 声道（左右逐位相同的「纯单声道」立体声）。
        Signal identicalStereo (const Signal& source)
        {
            return Signal { source[0], source[0] };
        }
    }

    //==============================================================================
    class MusicAnalyzerClayTests  : public juce::UnitTest
    {
    public:
        MusicAnalyzerClayTests() : juce::UnitTest ("MusicAnalyzerClay", "Celadon") {}

        void runTest() override
        {
            testClayUnknownAndFormatVersion();
            testClayMonoIsFullyCorrelated();
            testClayAntiPhaseLowBand();
            testClayUncorrelatedNoise();
            testClaySubShareIsTrueInfrasound();
            testClayLearningIntervalAndFreeze();
            testTrialBlendLoss();
            testClayLiveViewFollowsAndFreezes();
            testClayLiveViewIgnoresLeakageFromAbove();
            testClayBlockSizeIndependence();
            testClayNoAllocation();
            testClayTrialCosts();
            testClayTrialAlignment();
            testClayTrialLearningInterval();
            testClayTrialNoAllocation();
        }

    private:
        //==============================================================================
        // 依据：GlazeTypes.h（格式 2 的新字段「不知道」= valid false、数组 0；MusicProfile::formatVersion 默认 2）、MusicAnalyzer.h
        // 「不知道一律 valid = false」、任务书「产出的画像 formatVersion = 2」。
        //   - 刚开始学习：五个新特征与先试后用的六项代价都无效、数值与可信度 0，三个逐段数组全 0，实时立体声视图（慢路、快路、高频）
        //     无效且功率 0；
        //   - 3 s 两声道独立的粉噪之后（阳性对照）：格式版本仍是 2，五个特征都有了数值（超低频约 2.75 s ≥ 1 s；立体声约 1.97 s ≥ 1 s——
        //     第一帧要等满 2048 点窗，频谱平均还要稳定 1 s），实时视图有效（重启后已平滑满 300 ms）；代价仍无效（有效块要满 10 s：
        //     3 s 的 30 块里前 3 块落在 250 ms 预热里、最后一块的收单候选还没交齐，只记下 26 块 = 2.6 s）；
        //   - R = −L（M 恒为 0）：E_全 = 0 → subShareDb 无效、可信度 0（不当 −∞，也不当 0）；立体声照常：ρ = −1。
        void testClayUnknownAndFormatVersion()
        {
            beginTest ("Clay fields: unknown until measured, format version 2; M = 0 leaves the sub share unknown");

            constexpr double sampleRate = 48000.0;

            {
                Rig rig (sampleRate);
                rig.analyzer.startLearning();
                const auto& p = rig.profile();
                const auto& view = rig.analyzer.getShortTermView();

                expectEquals (p.formatVersion, 2, "a fresh candidate profile is format 2");

                for (const auto* f : { &p.subShareDb, &p.lowSideToMidDb, &p.lowCorrelation, &p.highSideToMidDb, &p.highCorrelation,
                                       &p.lowCutPeakCostDb, &p.monoPeakCostDb[0], &p.monoPeakCostDb[1], &p.monoPeakCostDb[2],
                                       &p.monoPeakCostDb[3], &p.widthPeakCostDb, &rig.analyzer.getAuxiliaryFeatures().trialPeakP99Db })
                    expect (! f->valid && f->value == 0.0f && f->confidence == 0.0f, "unknown is not zero");

                int nonZero = 0;

                for (int b = 0; b < numBands; ++b)
                {
                    const auto i = (size_t) b;
                    nonZero += (p.bandSideToMidDb[i] != 0.0f || p.bandCorrelation[i] != 0.0f || p.bandStereoConfidence[i] != 0.0f) ? 1 : 0;
                }

                expectEquals (nonZero, 0, "per-band stereo arrays start at zero");
                expect (! view.liveLowCorrelation.valid && ! view.liveLowCorrelationFast.valid && ! view.liveHighValid
                            && view.liveHighMidPower == 0.0f && view.liveHighSidePower == 0.0f,
                        "the live stereo view starts invalid and empty");

                rig.feed (makePinkNoise (numChannels, (int) (3.0 * sampleRate), -20.0f, 1001), 512);
                expectEquals (p.formatVersion, 2, "the analyzer produces format-2 profiles");
                expect (p.subShareDb.valid && p.lowSideToMidDb.valid && p.lowCorrelation.valid && p.highSideToMidDb.valid
                            && p.highCorrelation.valid, "3 s of stereo noise define every Clay feature (positive control)");
                expect (view.liveLowCorrelation.valid && view.liveLowCorrelationFast.valid && view.liveHighValid,
                        "the live stereo view is valid after 3 s");
                expect (! p.lowCutPeakCostDb.valid && ! p.monoPeakCostDb[0].valid && ! p.widthPeakCostDb.valid
                            && p.lowCutPeakCostDb.confidence == 0.0f && ! rig.analyzer.getAuxiliaryFeatures().trialPeakP99Db.valid,
                        "the peak costs need 10 s of valid blocks: still unknown after 3 s");
            }

            {
                const auto length = (int) (4.0 * sampleRate);
                Rig rig (sampleRate);
                rig.analyzer.startLearning();
                rig.feed (midSide (makeSilence (1, length), makePinkNoise (1, length, -20.0f, 1002)), 512);
                const auto& p = rig.profile();

                expect (! p.subShareDb.valid && p.subShareDb.confidence == 0.0f, "M = 0 (R = -L): the sub share is unknown, not -inf or 0");
                expect (p.lowCorrelation.valid && p.lowCorrelation.value <= -0.999f, "the stereo statistics still see R = -L");
            }
        }

        //==============================================================================
        // 依据：1.1 / 1.3（单声道 X = bandPower → ρ = 1、P_S = 0）；第 6 节第 3 条「纯单声道 ρ = 1、S/M ≤ −100 dB」。
        // 左右逐位相同的粉噪（Rig 把单声道信号复制到两个声道）与真单声道（1 声道）各 16 s：两组与每个有统计的段 ρ ≥ 1 − 1e-6、
        // S/M ≤ −100 dB（P_S 精确为 0，按 1e-12 封底读 −120 dB）；实时视图同样（高频组 P̃_S ≤ P̃_M × 1e-10）。
        // 阳性对照：粉噪 −20 dBFS 每段约 −35 dBFS，27 段全部在静音门上、全部有统计。真单声道 M = x 与左右相同的 M = (x + x) / 2
        // 逐位相同、起音的有效门也相同（功率 x² 与 2x² / 2），所以两者的超低频占比逐位相同（数值与有效时长；可信度含代表性，
        // 表的能量在单声道与立体声下末位舍入可以不同，不比）。
        void testClayMonoIsFullyCorrelated()
        {
            beginTest ("Clay stereo: identical channels and true mono give rho = 1 and S/M <= -100 dB (profile and live view)");

            constexpr double sampleRate = 48000.0;
            const auto source = makePinkNoise (1, (int) (16.0 * sampleRate), -20.0f, 1101);
            Rig stereo (sampleRate), mono (sampleRate, 1);

            for (auto* rig : { &stereo, &mono })
            {
                rig->analyzer.startLearning();
                rig->feed (source, 512);

                const auto& p = rig->profile();
                const auto& view = rig->analyzer.getShortTermView();
                const juce::String tag (rig == &mono ? "true mono" : "identical stereo");

                expect (p.lowCorrelation.valid && p.highCorrelation.valid && p.lowSideToMidDb.valid && p.highSideToMidDb.valid, tag);
                expect (p.lowCorrelation.value >= 0.999999f && p.highCorrelation.value >= 0.999999f,
                        "rho = 1 (" + tag + "): low " + juce::String (p.lowCorrelation.value, 7) + ", high "
                            + juce::String (p.highCorrelation.value, 7));
                expect (p.lowSideToMidDb.value <= -100.0f && p.highSideToMidDb.value <= -100.0f,
                        "S/M <= -100 dB (" + tag + "): low " + juce::String (p.lowSideToMidDb.value, 1) + " dB, high "
                            + juce::String (p.highSideToMidDb.value, 1) + " dB");

                int measuredBands = 0, wrongBands = 0;

                for (int b = 0; b < numBands; ++b)
                {
                    const auto i = (size_t) b;

                    if (p.bandStereoConfidence[i] <= 0.0f)
                        continue;

                    ++measuredBands;
                    wrongBands += (p.bandCorrelation[i] >= 0.999999f && p.bandSideToMidDb[i] <= -100.0f) ? 0 : 1;
                }

                expectEquals (measuredBands, numBands, "pink noise puts every band above the silence gate (" + tag + ")");
                expectEquals (wrongBands, 0, "every band: rho = 1 and S/M <= -100 dB (" + tag + ")");

                expect (view.liveLowCorrelation.valid && view.liveLowCorrelation.value >= 0.999999f, "live rho = 1 (" + tag + ")");
                expect (view.liveLowCorrelationFast.valid && view.liveLowCorrelationFast.value >= 0.999999f,
                        "live rho (120 ms fast path) = 1 (" + tag + ")");
                expect (view.liveHighValid && view.liveHighMidPower > 0.0f && view.liveHighSidePower <= view.liveHighMidPower * 1.0e-10f,
                        "live high side power is zero (" + tag + ")");
            }

            const auto& s = stereo.profile().subShareDb;
            const auto& m = mono.profile().subShareDb;
            expect (s.valid && m.valid && sameFloat (s.value, m.value) && sameDouble (s.effectiveSeconds, m.effectiveSeconds),
                    "true mono and identical stereo measure the same sub share (M = x)");
        }

        //==============================================================================
        // 依据：1.1 / 1.3（R = −L：X = −P → ρ = −1、P_M = 0）；第 6 节第 3 条「反相低频 ρ ≈ −1」；1.2（超低频只看 M）。
        // 低频：粉噪经 150 Hz 24 dB/oct 低通，两声道反相（纯 S）；高频：另一路粉噪经 1.5 kHz 24 dB/oct 高通，两声道同相（纯 M）。
        // 两路在对方组里都低 80 dB 以上（约 3.6 oct × 24 dB/oct ≈ 86 dB），所以低频组 ρ ≤ −0.999、S/M ≥ +40 dB；高频组 ρ ≥ 0.999、
        // S/M ≤ −40 dB——同一段信号里两组读出相反的结论（互为阳性对照）；逐段同样。反相的低频在 M 里正好抵消，高通后的高频在
        // 30 Hz 以下低 130 dB 以上：subShareDb ≤ −60 dB（反相的超低频不算超低频占比——它由收单处理，不由低切处理）。
        void testClayAntiPhaseLowBand()
        {
            beginTest ("Clay stereo: anti-phase lows read rho = -1 while in-phase highs read +1; anti-phase subs are no sub share");

            constexpr double sampleRate = 48000.0;
            const auto length = (int) (16.0 * sampleRate);
            auto lows = makePinkNoise (1, length, -20.0f, 1201);
            auto highs = makePinkNoise (1, length, -20.0f, 1202);
            applyLowpass24 (lows, sampleRate, 150.0);
            applyHighpass24 (highs, sampleRate, 1500.0);

            Rig rig (sampleRate);
            rig.analyzer.startLearning();
            rig.feed (midSide (highs, lows), 512);
            const auto& p = rig.profile();
            const auto& view = rig.analyzer.getShortTermView();

            logMessage ("  anti-phase lows / in-phase highs: low rho " + juce::String (p.lowCorrelation.value, 6) + ", S/M "
                        + juce::String (p.lowSideToMidDb.value, 1) + " dB; high rho " + juce::String (p.highCorrelation.value, 6)
                        + ", S/M " + juce::String (p.highSideToMidDb.value, 1) + " dB; sub share " + juce::String (p.subShareDb.value, 1)
                        + " dB");

            expect (p.lowCorrelation.valid && p.lowCorrelation.value <= -0.999f, "40 - 125 Hz: rho = -1");
            expect (p.lowSideToMidDb.valid && p.lowSideToMidDb.value >= 40.0f, "40 - 125 Hz: all side");
            expect (p.highCorrelation.valid && p.highCorrelation.value >= 0.999f, "2 - 16 kHz: rho = +1 in the same signal");
            expect (p.highSideToMidDb.valid && p.highSideToMidDb.value <= -40.0f, "2 - 16 kHz: all mid");

            int wrongLow = 0, wrongHigh = 0;

            for (int b = 0; b <= 5; ++b)
                wrongLow += (p.bandStereoConfidence[(size_t) b] > 0.0f && p.bandCorrelation[(size_t) b] <= -0.99f) ? 0 : 1;

            for (int b = 17; b < numBands; ++b)
                wrongHigh += (p.bandStereoConfidence[(size_t) b] > 0.0f && p.bandCorrelation[(size_t) b] >= 0.99f) ? 0 : 1;

            expectEquals (wrongLow, 0, "each 40 - 125 Hz band is anti-phase");
            expectEquals (wrongHigh, 0, "each 2 - 16 kHz band is in phase");

            expect (view.liveLowCorrelation.valid && view.liveLowCorrelation.value <= -0.99f, "live low rho = -1");
            expect (view.liveLowCorrelationFast.valid && view.liveLowCorrelationFast.value <= -0.99f, "live low rho (fast path) = -1");
            expect (view.liveHighValid && view.liveHighSidePower <= view.liveHighMidPower * 1.0e-4f, "live highs are mid");
            expect (p.subShareDb.valid && p.subShareDb.value <= -60.0f, "anti-phase sub-bass cancels in M: no sub share");
        }

        //==============================================================================
        // 依据：第 6 节第 3 条「两路不相关噪声 ρ ≈ 0」；1.3「置信度同 bandConfidence 口径」。两声道各自独立的粉噪（makePinkNoise
        // 每声道不同种子）24 s，带连续的宿主位置（代表性 1）。容差按估计量的统计散布定：不相关时 ρ̂ 的标准差 ≈ √(0.5 / M)，
        // M = 独立的「bin × 帧」数；hop 是窗长的 1/4、相邻帧重叠，独立帧按总帧数 / 1.9 算（约 23 s / 10.7 ms / 1.9 ≈ 1100）：
        //   - 低频组（按功率加权约 4 个 bin）σ ≈ 0.011 → 取 0.06；S/M ≈ −8.7·ρ dB → 取 0.6 dB；
        //   - 高频组（按功率加权约 450 个 bin）σ ≈ 0.001 → 取 0.01；S/M 取 0.1 dB；
        //   - 逐段最窄的 40 Hz 段只占约 0.4 个 bin → σ ≈ 0.02 → 取 0.2；
        //   - 实时视图只平滑 300 ms（等效约 29 个独立帧）：低频 σ ≈ 0.065 → |ρ̃| < 0.4；高频 S/M σ ≈ 0.05 dB → 取 0.5 dB；
        //     低频快路 120 ms（ADR-012）：σ ≈ 0.065·√(0.3 / 0.12) ≈ 0.10（numpy 按同一素材逐帧复算：σ 0.12、24 s 里 |ρ̃_快| 最大 0.37）
        //     → |ρ̃_快| < 0.5。
        // 可信度：门上观测约 23 s ≥ 10 s、代表性 1 → 两组与每段都是 1。阳性对照是单声道（ρ = 1）与反相（ρ = −1）两个用例。
        void testClayUncorrelatedNoise()
        {
            beginTest ("Clay stereo: two independent noises read rho ~ 0 and S/M ~ 0 dB; the confidence reaches 1");

            constexpr double sampleRate = 48000.0;
            Rig rig (sampleRate);
            rig.analyzer.startLearning();
            rig.feed (makePinkNoise (numChannels, (int) (24.0 * sampleRate), -20.0f, 1301), 512, 1,
                      [] (std::int64_t fed, int) { return playingAt (fed); });

            const auto& p = rig.profile();
            const auto& view = rig.analyzer.getShortTermView();
            float worstBand = 0.0f;
            int unsureBands = 0;

            for (int b = 0; b < numBands; ++b)
            {
                worstBand = juce::jmax (worstBand, std::abs (p.bandCorrelation[(size_t) b]));
                unsureBands += (p.bandStereoConfidence[(size_t) b] >= 1.0f - 1.0e-6f) ? 0 : 1;
            }

            const auto liveHighSideToMidDb = 10.0 * std::log10 ((double) view.liveHighSidePower / (double) view.liveHighMidPower);

            logMessage ("  independent noises: low rho " + juce::String (p.lowCorrelation.value, 4) + " (S/M "
                        + juce::String (p.lowSideToMidDb.value, 2) + " dB), high rho " + juce::String (p.highCorrelation.value, 5)
                        + " (S/M " + juce::String (p.highSideToMidDb.value, 3) + " dB), worst band |rho| " + juce::String (worstBand, 3)
                        + "; live low rho " + juce::String (view.liveLowCorrelation.value, 3) + " (fast "
                        + juce::String (view.liveLowCorrelationFast.value, 3) + "), live high S/M "
                        + juce::String (liveHighSideToMidDb, 3) + " dB");

            expect (p.lowCorrelation.valid && std::abs (p.lowCorrelation.value) <= 0.06f, "low-band rho ~ 0");
            expect (p.lowSideToMidDb.valid && std::abs (p.lowSideToMidDb.value) <= 0.6f, "low-band S/M ~ 0 dB");
            expect (p.highCorrelation.valid && std::abs (p.highCorrelation.value) <= 0.01f, "high-band rho ~ 0");
            expect (p.highSideToMidDb.valid && std::abs (p.highSideToMidDb.value) <= 0.1f, "high-band S/M ~ 0 dB");
            expect (worstBand <= 0.2f, "every band rho ~ 0");

            expectWithinAbsoluteError (p.lowCorrelation.confidence, 1.0f, 1.0e-6f, "group confidence = min(observation / 10 s, representativeness)");
            expectWithinAbsoluteError (p.highSideToMidDb.confidence, 1.0f, 1.0e-6f, "group confidence (high)");
            expectEquals (unsureBands, 0, "per-band stereo confidence reaches 1 like bandConfidence");

            expect (view.liveLowCorrelation.valid && std::abs (view.liveLowCorrelation.value) < 0.4f, "live low rho ~ 0");
            expect (view.liveLowCorrelationFast.valid && std::abs (view.liveLowCorrelationFast.value) < 0.5f, "live low rho (fast path) ~ 0");
            expect (view.liveHighValid && std::abs (liveHighSideToMidDb) < 0.5, "live high S/M ~ 0 dB");
        }

        //==============================================================================
        // 依据：设计规格-Clay-根治修订 3.1（ADR-012）「真次声：subShareDb 改为 M 过 8 阶 Butterworth 16 Hz 低通；有效门、学习区间、
        // 块长无关等口径照旧」；原第 6 节第 3 条的解析值口径（误差 ≤ 0.5 dB）。信号两声道相同（M 就是它），20 s，期望值都按解析式算：
        //  a) 10 Hz 正弦（幅度 A）+ 宽带内容 h（白噪 −12 dBFS 经 100 Hz 24 dB/oct 高通）——真次声。分母 E_全 取实际信号在分析区间
        //     （起音包络重启后 250 ms 预热之后）上的均方（正弦与 h 频率不重叠，交叉项 ≈ 0）；分子 = A² / 2 · |H(10 Hz)|²，
        //     |H|² = 1 / (1 + (tan(π·10/fs) / tan(π·16/fs))¹⁶) = 0.99946（−0.002 dB）；h 漏进低通的部分：100 Hz 4 阶高通在 16 Hz 以下
        //     低 64 dB 以上、又只占 16 Hz 宽，约为 h 的 −100 dB，忽略。A = 0.05 → 约 −17.1 dB；A 再低 10 dB → 约 −27.0 dB（都在决策器
        //     −40 ～ −28 dB 斜坡的上端 = 需要度 1）。有效时长 = 20 s − 0.25 s 预热（按 0.33 ms 的组取整，差 < 0.001 s）。
        //     反向阳性对照：只有 h 时 ≤ −60 dB（numpy 复核：−102 dB）。
        //  b) 以 45 Hz 底鼓为主（每 0.5 s 一击：45 Hz 正弦乘 0.2 s 的 Hann 窗、幅度 0.5；另加 h × 0.25）——不是次声。解析值按线谱：
        //     底鼓以 0.5 s 为周期，一个周期（N = 24000 样本）的 DFT 在 k·2 Hz 上，E_sub = (|X₀|²·|H(0)|² + 2·Σ_k |X_k|²·|H(k·2 Hz)|²) / N
        //     （k 取到 100 Hz：8 阶 16 Hz 在 50 Hz 处已低 79 dB，底鼓的主瓣在 35 ～ 55 Hz），E_全 = 一个周期的 Σ x² + h 部分的均方 × N。
        //     Hann 窗的旁瓣按 18 dB/oct 往下掉，落到 16 Hz 以下的只剩约 −58 dB（numpy 复核：实测 −58.38、线谱 −58.42 dB）。
        //     断言与线谱值差 ≤ 1 dB（分析区间是 39.5 个周期、不是整周期数，h 的那一份差约 1 %，≈ 0.01 dB），且 ≤ −40 dB（需要度 0：
        //     自动低切不会因为底鼓开启）。阳性对照（测试按同一线谱自己算，不经分析器）：原来的 4 阶 30 Hz 低通在 45 Hz 处只衰减 14 dB，
        //     同一素材会读成约 −14.5 dB（≥ −20 dB）——根治修订要修的正是这个「底鼓基音漏入」。
        void testClaySubShareIsTrueInfrasound()
        {
            beginTest ("Clay sub share: true infrasound (8th-order 16 Hz) - a 10 Hz sine reads its analytic share, a 45 Hz kick reads almost none");

            constexpr double sampleRate = 48000.0;
            constexpr double seconds = 20.0;
            const auto length = (int) (seconds * sampleRate);
            const auto analysedFrom = (int) (0.25 * sampleRate);
            const auto gainAt10Hz = butterworthPowerGain (8, 10.0, 16.0, sampleRate);

            auto broadband = makeWhiteNoise (1, length, -12.0f, 1401);
            applyHighpass24 (broadband, sampleRate, 100.0);

            // 分析区间（250 ms 预热之后）上的均方。
            // 不捕获：analysedFrom 是常量表达式初始化的 const，读它不算 odr-use（显式捕获 clang 报多余；MSVC 靠 /Zc:lambda）。
            auto analysedMeanSquare = [] (const Signal& material)
            {
                double sumOfSquares = 0.0;

                for (size_t i = (size_t) analysedFrom; i < material[0].size(); ++i)
                {
                    const auto x = (double) material[0][i];
                    sumOfSquares += x * x;
                }

                return sumOfSquares / (double) (material[0].size() - (size_t) analysedFrom);
            };

            // a) 10 Hz 真次声。
            for (auto amplitude : { 0.05f, 0.0158113883f })
            {
                auto input = makeSine (1, sampleRate, 10.0, amplitude, seconds);
                addInto (input, broadband);

                const auto sinePower = 0.5 * (double) amplitude * (double) amplitude;
                const auto expected = 10.0 * std::log10 (sinePower * gainAt10Hz / analysedMeanSquare (input));

                Rig rig (sampleRate);
                rig.analyzer.startLearning();
                rig.feed (input, 512);
                const auto& share = rig.profile().subShareDb;

                logMessage ("  10 Hz sine at amplitude " + juce::String (amplitude, 4) + ": sub share " + juce::String (share.value, 3)
                            + " dB, analytic " + juce::String (expected, 3) + " dB, effective " + juce::String (share.effectiveSeconds, 3)
                            + " s");
                expect (share.valid, "the sub share is measured");
                expectWithinAbsoluteError (share.value, (float) expected, 0.5f, "10 Hz sub share vs analytic value");
                expectWithinAbsoluteError (share.effectiveSeconds, seconds - 0.25, 0.01, "effective audio after the 250 ms warm-up");
            }

            {
                Rig rig (sampleRate);
                rig.analyzer.startLearning();
                rig.feed (broadband, 512);
                expect (rig.profile().subShareDb.valid && rig.profile().subShareDb.value <= -60.0f,
                        "broadband content above 100 Hz has no sub share (" + juce::String (rig.profile().subShareDb.value, 1) + " dB)");
            }

            // b) 45 Hz 底鼓为主。
            const auto kick = makeKickTrain (sampleRate, seconds, 45.0, 0.2, 0.5, [] (double t)
            {
                return 0.5 * 0.5 * (1.0 - std::cos (juce::MathConstants<double>::twoPi * t / 0.2));
            });

            auto quietBroadband = broadband;
            scale (quietBroadband, 0.25f);
            auto kickInput = kick;
            addInto (kickInput, quietBroadband);

            // 一个周期的线谱（直接按定义求 DFT 的前 51 个 bin，0 ～ 100 Hz）。
            const auto period = (int) std::lround (0.5 * sampleRate);
            double periodEnergy = 0.0, periodSubEnergy = 0.0, periodOldSubEnergy = 0.0;

            for (int n = 0; n < period; ++n)
            {
                const auto x = (double) kick[0][(size_t) n];
                periodEnergy += x * x;
            }

            for (int k = 0; k <= 50; ++k)
            {
                std::complex<double> bin (0.0, 0.0);

                for (int n = 0; n < period; ++n)
                    bin += (double) kick[0][(size_t) n]
                           * std::polar (1.0, -juce::MathConstants<double>::twoPi * (double) k * (double) n / (double) period);

                const auto lineEnergy = (k == 0 ? 1.0 : 2.0) * std::norm (bin) / (double) period;
                const auto lineHz = (double) k * sampleRate / (double) period;
                periodSubEnergy += lineEnergy * butterworthPowerGain (8, lineHz, 16.0, sampleRate);
                periodOldSubEnergy += lineEnergy * butterworthPowerGain (4, lineHz, 30.0, sampleRate);
            }

            const auto periodTotal = periodEnergy + analysedMeanSquare (quietBroadband) * (double) period;
            const auto expectedKick = 10.0 * std::log10 (periodSubEnergy / periodTotal);
            const auto oldFilterReading = 10.0 * std::log10 (periodOldSubEnergy / periodTotal);

            Rig rig (sampleRate);
            rig.analyzer.startLearning();
            rig.feed (kickInput, 512);
            const auto& kickShare = rig.profile().subShareDb;

            logMessage ("  45 Hz kick: sub share " + juce::String (kickShare.value, 2) + " dB, line-spectrum value "
                        + juce::String (expectedKick, 2) + " dB (the former 4th-order 30 Hz lowpass would read "
                        + juce::String (oldFilterReading, 2) + " dB)");
            expect (kickShare.valid, "the kick material's sub share is measured");
            expectWithinAbsoluteError (kickShare.value, (float) expectedKick, 1.0f, "45 Hz kick sub share vs line-spectrum value");
            expect (kickShare.value <= -40.0f, "a 45 Hz kick is not infrasound: no automatic low-cut need");
            expect (oldFilterReading >= -20.0, "positive control: the former 30 Hz lowpass read the kick fundamental as sub-bass");
        }

        //==============================================================================
        // T-043（ADR-027）先试后用的守护混合损失（lowCutBlendLossDb，候选 clayBlendCandidateHz 10 / 20 Hz；MusicAnalyzer.h「五」）：
        //   - 稳态 60 Hz 正弦（0.5，左右相同）学 12 s：两点都有效、|L| < 0.002 dB（守护不动作；只有起振的高通余量可能在头几百毫秒
        //     碰到守护，大半落在 250 ms 预热里）；
        //   - 偏置低音（49 Hz 0.4 + 98 Hz 0.15 + 4 Hz 0.15，左右相同；ClayTests 同一素材）学 12 s：两点都有效、L₂₀ > L₁₀ > 0、
        //     L₂₀ > 0.1 dB（109 渲染实测约 0.12 / 0.21 dB）；
        //     与链里的 ClayModule 在同一素材上过 20 Hz 低切量到的损失一致（±0.02 dB：分析器只计学习区间里过了预热的有效块，模块计全程；
        //     两边用的是同一份守护实现）；
        //   - 学 6 s（有效块不满 10 s）→ 两点都无效（与峰值代价同时）；
        //   - 块长 7 / 333 / 4096 与 512 两点逐位相同（sameTrialCosts 也已并进块长无关那条）。
        void testTrialBlendLoss()
        {
            beginTest ("trial guard blend loss (T-043): 0 on a steady tone, > 0.1 dB at 20 Hz on offset bass matching ClayModule, valid with the peak costs, block-size independent");

            constexpr double sampleRate = 48000.0;
            const auto learn = [&] (const Signal& signal, int blockSize)
            {
                Rig rig (sampleRate);
                rig.analyzer.startLearning();
                rig.feed (signal, blockSize);
                return rig.profile();
            };

            const auto steady = learn (makeSine (numChannels, sampleRate, 60.0, 0.5f, 12.0), 512);
            expect (steady.lowCutBlendLossDb[0].valid && steady.lowCutBlendLossDb[1].valid, "12 s of learning define both points");
            expect (std::abs (steady.lowCutBlendLossDb[0].value) < 0.002f && std::abs (steady.lowCutBlendLossDb[1].value) < 0.002f,
                    "steady tone: the guard does not blend, the loss is ~0 ("
                        + juce::String (steady.lowCutBlendLossDb[0].value, 6) + " / " + juce::String (steady.lowCutBlendLossDb[1].value, 6) + ")");

            auto offset = makeSine (numChannels, sampleRate, 49.0, 0.4f, 12.0);
            const auto harmonic = makeSine (numChannels, sampleRate, 98.0, 0.15f, 12.0);
            const auto infra = makeSine (numChannels, sampleRate, 4.0, 0.15f, 12.0);

            for (size_t ch = 0; ch < offset.size(); ++ch)
                for (size_t i = 0; i < offset[ch].size(); ++i)
                    offset[ch][i] += harmonic[ch][i] + infra[ch][i];

            const auto learned = learn (offset, 512);
            const auto& l10 = learned.lowCutBlendLossDb[0];
            const auto& l20 = learned.lowCutBlendLossDb[1];

            ClayModule clay;
            ClayModule::Params lowCut;
            lowCut.lowCutHz = 20.0f;
            clay.setParams (lowCut);
            clay.prepare ({ sampleRate, 512, (juce::uint32) numChannels });
            clay.setParams (lowCut);
            juce::AudioBuffer<float> block (numChannels, 512);

            for (int start = 0; start + 512 <= (int) offset[0].size(); start += 512)
            {
                for (int ch = 0; ch < numChannels; ++ch)
                    block.copyFrom (ch, 0, offset[(size_t) ch].data() + start, 512);

                clay.process (block);
            }

            const auto stats = clay.getAndResetPeakGuardStats();
            const auto moduleLoss = 10.0 * std::log10 (stats.blendIntendedEnergy / stats.blendOutputEnergy);
            logMessage ("  offset bass: trial blend loss 10 Hz " + juce::String (l10.value, 4) + " dB, 20 Hz " + juce::String (l20.value, 4)
                        + " dB; ClayModule at 20 Hz " + juce::String (moduleLoss, 4) + " dB");
            expect (l10.valid && l20.valid && l20.value > l10.value && l10.value > 0.0f && l20.value > 0.1f,
                    "offset bass: the loss grows with the cutoff and exceeds 0.1 dB at 20 Hz");
            expectWithinAbsoluteError ((double) l20.value, moduleLoss, 0.02, "the 20 Hz point agrees with the chain's ClayModule on the same material");

            const auto shortSpan = learn (sliceOf (offset, 0, (int) (6.0 * sampleRate)), 512);
            expect (! shortSpan.lowCutBlendLossDb[0].valid && ! shortSpan.lowCutBlendLossDb[1].valid && ! shortSpan.lowCutPeakCostDb.valid,
                    "fewer than 10 s of valid blocks: unknown, together with the peak costs");

            for (auto blockSize : { 7, 333, 4096 })
            {
                const auto other = learn (offset, blockSize);
                expect (sameFeature (other.lowCutBlendLossDb[0], l10) && sameFeature (other.lowCutBlendLossDb[1], l20),
                        "block size " + juce::String (blockSize) + ": both points bit-identical");
            }
        }

        //==============================================================================
        // 依据：MusicAnalyzer.h「学习区间从 startLearning 那一刻起按样本精确划分；stopLearning 按到达立即冻结」、1.3（画像只来自
        // 学习区间）、1.4（实时视图学习与否都更新，不进画像）。
        //   - 学习前 5 s：M 是 10 Hz 正弦（幅度 0.01；真次声低通在 10 Hz 处只差 0.002 dB，E_sub / E_全 ≈ 0 dB）、S 是 150 Hz 低通的
        //     粉噪（低频 ρ ≈ −0.99）。根治修订前这里用 20 Hz：8 阶 16 Hz 低通在 20 Hz 处衰减 15.6 dB，混进来也只到 −40 dB 左右，分不开了；
        //   - 学习 12 s：左右相同的粉噪经 100 Hz 高通（低频组 80 ～ 125 Hz 仍在门上，ρ = 1；真次声约 −78 dB）。
        //     学习前的若混进来：低频 ρ ≈ −0.3、真次声约 −24.5 dB（numpy 复核）——阈值 ρ ≥ 0.999、≤ −40 dB 分得开；
        //   - 停止学习后再送 5 s 学习前那种素材：修坯字段（含先试后用的代价——12 s 已有 11.6 s 有效块，是有效值）与旧字段都逐位不变，
        //     实时视图照常跟随（ρ̃ ≤ −0.9）。
        void testClayLearningIntervalAndFreeze()
        {
            beginTest ("Clay fields cover the learning interval only and freeze at stopLearning; the live view keeps following");

            constexpr double sampleRate = 48000.0;
            auto antiPhaseLows = makePinkNoise (1, (int) (5.0 * sampleRate), -20.0f, 1501);
            applyLowpass24 (antiPhaseLows, sampleRate, 150.0);
            const auto outside = midSide (makeSine (1, sampleRate, 10.0, 0.01f, 5.0), antiPhaseLows);

            auto learned = makePinkNoise (1, (int) (12.0 * sampleRate), -20.0f, 1502);
            applyHighpass24 (learned, sampleRate, 100.0);

            Rig rig (sampleRate);
            const auto& view = rig.analyzer.getShortTermView();
            rig.feed (outside, 512);
            expect (! rig.profile().lowCorrelation.valid && ! rig.profile().subShareDb.valid, "nothing is accumulated before learning");
            expect (view.liveLowCorrelation.valid && view.liveLowCorrelation.value <= -0.9f, "the live view follows while not learning");

            rig.analyzer.startLearning();
            rig.feed (learned, 512);
            const auto& p = rig.profile();
            logMessage ("  learned after an anti-phase lead-in: low rho " + juce::String (p.lowCorrelation.value, 5) + ", sub share "
                        + juce::String (p.subShareDb.value, 1) + " dB");
            expect (p.lowCorrelation.valid && p.lowCorrelation.value >= 0.999f, "anti-phase lows before startLearning are excluded");
            expect (p.subShareDb.valid && p.subShareDb.value <= -40.0f, "the 10 Hz sine before startLearning is excluded");
            expect (p.lowCutPeakCostDb.valid && p.widthPeakCostDb.valid, "12 s of learning define the peak costs (the freeze check covers them)");

            rig.analyzer.stopLearning();
            const auto frozen = rig.profile();
            rig.feed (outside, 512);
            expect (sameProfile (frozen, rig.profile()) && sameClayProfile (frozen, rig.profile()), "all fields are fixed after stopLearning");
            expect (view.liveLowCorrelation.valid && view.liveLowCorrelation.value <= -0.9f, "the live view keeps following after stopLearning");
        }

        //==============================================================================
        // 依据：1.4 与 MusicAnalyzer.h（实时立体声视图）、设计规格-Clay-根治修订 3.1（ADR-012）「liveLowCorrelation 只取 300 ms 慢路；
        // 新增 liveLowCorrelationFast（120 ms，同门限、同冻结）；帧门限之下（静音）两者都置无效」、任务书「300 ms 功率域一阶平滑
        // （按 hop 换算、块长无关）、门下的段不计、liveLowCorrelation.valid 仅当低频组门上有内容；静音时两路 valid = false 且回来后接着
        // 平滑」。48 kHz、块长 512 = hop：从第 5 块起每块正好读进一帧，块数就是帧数。不学习（实时视图照常更新，从不写画像）。
        // 素材：s = 100 Hz 正弦（幅度 0.1，99.8 % 落在 40 ～ 125 Hz 组）、h = 粉噪经 1.5 kHz 高通（落在 2 ～ 16 kHz 组，漏进低频组的低 80 dB 以上）。
        // 下面的块号都经 numpy 按 Balance 的 Hann 窗 / 段权重与分析器的平滑逐帧复算过（2026-09-26，109）。
        //   A  288 块  L = R = s + h（全同相）。
        //   B  282 块  L = s + h、R = −(s + h)（全反相，切换在块边界上）：
        //       - 高频组本帧 P_M 从第 5 块起精确为 0（R = −L 时 X = −P）→ liveHighMidPower 每块正好乘 r = exp(−512 / (48000 · 0.3))
        //         = 0.96507：逐块比值与 r 差 < 1e-4，第 9 ～ 61 块（52 帧）换算出的时间常数 = 0.300 ± 0.001 s；
        //       - 低频 ρ̃ 从 +1 走向 −1：等功率时 ρ̃ ≈ 1.9·r^m − 1（m = 窗中心越过切换点之后的帧数，窗中心约在第 2 块越过），过零在
        //         m = ln 1.9 / (512 / (48000·τ)) = 60.2·τ 处 → 第一次变负的块号 ≈ 2 + 60.2·τ 向上取整：
        //           慢路 τ = 0.3 → 第 21 块（numpy：21；阶段 3 之前只有这一路时 A4 报告推算的也是 21）。断言 20 ～ 22 ↔ τ ≈ 0.28 ～ 0.33 s；
        //           快路 τ = 0.12 → 第 10 块（numpy：10；审核 V1 变体 B 实测也是 10）。断言 9 ～ 11 ↔ τ ≈ 0.10 ～ 0.15 s
        //           （原 60 ms 快路在第 6 块，只有慢路时在第 21 块——两路都分得开）；
        //       - 282 块后两路都 ≤ −0.999；高频 P̃_S 回到 A 段末 P̃_M 的 ±15 %（同一个 h 从 M 换到 S；两次 300 ms 估计各约 ±1 %）。
        //   C  141 块  数字静音：窗全落进静音（第 4 块起）后帧在帧门限下 → 两路低频相关度都无效（ClayGuard 由此冻结反相一路），
        //       高频两项冻结在最后的值；进静音 48 块与 141 块时视图逐位相同。
        //   D  188 块  L = R = s + h → 两路都接着 C 段保留的平滑状态走，而不是清空重来（重来会立刻是 +1）：第 5 块慢路 ρ̃ ≈ −0.82
        //       （断言 ≤ −0.5，同原断言）、快路 ≈ −0.53（断言 ≤ 0，且快路先回来：> 慢路）；188 块后慢路 ≥ 0.98、快路 ≥ 0.999。
        //   E  375 块  h 同相 + 左右独立、−80 dBFS 的粉噪（低频每段约 −95 dBFS，远在 −60 dBFS 门下）：门下的段不计 → 低频组本帧和为 0，
        //       平滑值只按各自的系数衰减、比值不变（慢路第 32 块与第 141 块、快路第 32 块与第 60 块相差 < 1e-5；若计入不相关的底噪，
        //       ρ̃ 会掉到 0 附近）；平滑功率从约 5e-3 往下掉：快路按 120 ms 约第 98 块、慢路按 300 ms 约第 241 块落到 −60 dBFS 门下 →
        //       第 141 块时快路已无效、慢路仍有效，375 块时两路都无效（同门、各自的时间常数）。
        void testClayLiveViewFollowsAndFreezes()
        {
            beginTest ("Clay live view: 300 ms slow path and 120 ms fast path, gated bands do not count, silence invalidates both and keeps the smoothing");

            constexpr double sampleRate = 48000.0;
            constexpr int blockLength = 512;
            constexpr int blocksA = 288, blocksB = 282, blocksC = 141, blocksD = 188, blocksE = 375;
            constexpr int startB = blocksA * blockLength;
            constexpr int startC = startB + blocksB * blockLength;
            constexpr int startD = startC + blocksC * blockLength;
            constexpr int startE = startD + blocksD * blockLength;
            constexpr int total = startE + blocksE * blockLength;
            const auto perBlockRetain = std::exp (-(double) blockLength / (0.3 * sampleRate));

            const auto tone = makeSine (1, sampleRate, 100.0, 0.1f, (double) total / sampleRate + 0.1);
            auto hiss = makePinkNoise (1, total, -20.0f, 1601);
            applyHighpass24 (hiss, sampleRate, 1500.0);
            const auto floorNoise = makePinkNoise (numChannels, blocksE * blockLength, -80.0f, 1602);   // 两声道独立

            auto input = makeSilence (numChannels, total);

            for (int i = 0; i < total; ++i)
            {
                const auto index = (size_t) i;
                const auto content = tone[0][index] + hiss[0][index];

                if (i < startB || (i >= startD && i < startE))
                {
                    input[0][index] = content;
                    input[1][index] = content;
                }
                else if (i < startC)
                {
                    input[0][index] = content;
                    input[1][index] = -content;
                }
                else if (i >= startE)
                {
                    const auto e = (size_t) (i - startE);
                    input[0][index] = hiss[0][index] + floorNoise[0][e];
                    input[1][index] = hiss[0][index] + floorNoise[1][e];
                }
                // C 段保持数字静音。
            }

            Rig rig (sampleRate);
            const auto& view = rig.analyzer.getShortTermView();
            int cursor = 0;

            auto feedBlocks = [&] (int count)
            {
                rig.feed (sliceOf (input, cursor, count * blockLength), blockLength);
                cursor += count * blockLength;
            };

            // A：全同相。
            feedBlocks (blocksA);
            const auto highMidA = view.liveHighMidPower;
            expect (view.liveLowCorrelation.valid && view.liveLowCorrelation.value >= 0.999f, "A: in phase");
            expect (view.liveLowCorrelationFast.valid && view.liveLowCorrelationFast.value >= 0.999f, "A: in phase (fast path)");
            expect (view.liveHighValid && highMidA > 0.0f && view.liveHighSidePower <= highMidA * 1.0e-10f, "A: the highs are all mid");

            // B：全反相，逐块记录。
            std::vector<double> highMid;
            int invalidBlocks = 0, firstNegative = -1, firstNegativeFast = -1;

            for (int k = 0; k < blocksB; ++k)
            {
                feedBlocks (1);
                highMid.push_back ((double) view.liveHighMidPower);
                invalidBlocks += (view.liveLowCorrelation.valid && view.liveLowCorrelationFast.valid) ? 0 : 1;

                if (firstNegative < 0 && view.liveLowCorrelation.value < 0.0f)
                    firstNegative = k;

                if (firstNegativeFast < 0 && view.liveLowCorrelationFast.value < 0.0f)
                    firstNegativeFast = k;
            }

            double worstRatioError = 0.0;

            for (size_t k = 8; k < 60; ++k)
                worstRatioError = juce::jmax (worstRatioError, std::abs (highMid[k + 1] / highMid[k] - perBlockRetain));

            const auto measuredSeconds = -52.0 * (double) blockLength / (sampleRate * std::log (highMid[60] / highMid[8]));

            logMessage ("  live view: high P_M decay per block " + juce::String (highMid[21] / highMid[20], 6) + " (r = "
                        + juce::String (perBlockRetain, 6) + ", worst error " + juce::String (worstRatioError, 8) + "), time constant "
                        + juce::String (measuredSeconds, 5) + " s; low rho first negative at block " + juce::String (firstNegative)
                        + " (slow), " + juce::String (firstNegativeFast) + " (fast)");

            expectEquals (invalidBlocks, 0, "B: both live correlations stay valid through the switch");
            expect (worstRatioError < 1.0e-4, "B: a P_M that went to zero decays by exactly r = exp(-hop / 0.3 s) per frame");
            expectWithinAbsoluteError (measuredSeconds, 0.3, 1.0e-3, "B: 300 ms power smoothing");
            expect (firstNegative >= 20 && firstNegative <= 22,
                    "B: the slow path (300 ms, no longer min-with-fast) crosses zero at about block 21 (block "
                        + juce::String (firstNegative) + ")");
            expect (firstNegativeFast >= 9 && firstNegativeFast <= 11,
                    "B: the 120 ms fast path crosses zero at about block 10 (block " + juce::String (firstNegativeFast) + ")");
            expect (view.liveLowCorrelation.value <= -0.999f && view.liveLowCorrelationFast.value <= -0.999f, "B: steady anti-phase");
            expect (view.liveHighValid && std::abs ((double) view.liveHighSidePower / (double) highMidA - 1.0) <= 0.15,
                    "B: the same highs moved from M to S");

            // C：数字静音 → 两路低频相关度与高频两项都无效（平滑状态 / 功率值保留；ClayGuard 两路都冻结）。
            feedBlocks (48);
            const auto enteredSilence = view;
            feedBlocks (blocksC - 48);
            expect (sameLiveStereo (enteredSilence, view), "C: the live stereo view does not move in silence");
            expect (! enteredSilence.liveLowCorrelation.valid && ! enteredSilence.liveLowCorrelationFast.valid,
                    "C: under the frame gate both low correlations are invalid (ClayGuard freezes on that)");
            expect (! enteredSilence.liveHighValid && enteredSilence.liveHighSidePower > 0.0f,
                    "C: under the frame gate the highs are invalid but keep their last values (the width guard freezes)");

            // D：恢复同相 → 两路都接着 C 段保留的平滑状态走。
            feedBlocks (5);
            expect (view.liveLowCorrelation.valid && view.liveLowCorrelation.value <= -0.5f,
                    "D: the slow path continues from the kept state instead of restarting ("
                        + juce::String (view.liveLowCorrelation.value, 3) + ")");
            expect (view.liveLowCorrelationFast.valid && view.liveLowCorrelationFast.value <= 0.0f
                        && view.liveLowCorrelationFast.value > view.liveLowCorrelation.value,
                    "D: the fast path also continues from the kept state and comes back first ("
                        + juce::String (view.liveLowCorrelationFast.value, 3) + ")");
            feedBlocks (blocksD - 5);
            expect (view.liveLowCorrelation.value >= 0.98f && view.liveLowCorrelationFast.value >= 0.999f, "D: back in phase after 2 s");

            // E：低频只剩门下的不相关底噪。
            feedBlocks (32);
            const auto gatedEarly = view.liveLowCorrelation;
            const auto gatedEarlyFast = view.liveLowCorrelationFast;
            feedBlocks (60 - 32);
            const auto gatedMiddleFast = view.liveLowCorrelationFast;
            feedBlocks (141 - 60);
            const auto gatedLater = view.liveLowCorrelation;
            expect (gatedEarly.valid && gatedLater.valid && std::abs (gatedLater.value - gatedEarly.value) <= 1.0e-5f,
                    "E: bands under the silence gate do not count: rho " + juce::String (gatedEarly.value, 6) + " -> "
                        + juce::String (gatedLater.value, 6));
            expect (gatedEarlyFast.valid && gatedMiddleFast.valid && std::abs (gatedMiddleFast.value - gatedEarlyFast.value) <= 1.0e-5f,
                    "E: the fast path does not count gated bands either");
            expect (! view.liveLowCorrelationFast.valid && view.liveLowCorrelation.valid,
                    "E: same gate, own time constant: the fast path's smoothed power falls under the gate first");
            expect (view.liveHighValid, "E: the highs keep the frames above the frame gate");
            feedBlocks (blocksE - 141);
            expect (! view.liveLowCorrelation.valid && ! view.liveLowCorrelationFast.valid,
                    "E: once the smoothed low power falls under the gate the correlation is unknown");

            expectEquals (cursor, total);
            expect (! rig.profile().lowCorrelation.valid && ! rig.profile().subShareDb.valid, "the live view never writes the profile");
        }

        //==============================================================================
        // 依据：MusicAnalyzer.cpp「泄漏门」（BUG-009，T-042）——低频组没有低音、只有上方内容时，读数只是漏进 125 Hz 段（段 5 的格子
        // 到 152 Hz、Hann 主瓣再往上）的部分，不能当低频相关度。素材 makeDetunedPad（Am – F – C – G 每 2 s 一换，最低分音 174.6 / 196 Hz，
        // 左右失谐 ±0.3 %），48 kHz、块长 512、12 s，不学习（实时视图照常更新）。每块读一次视图，同时自己读 Balance 帧做阳性对照：
        //   a. 安静的 pad（基音幅度 0.03，与合成 House 曲 breakdown 的 pad 同量级）：两路低频相关度每一块都无效。阳性对照：有帧是
        //      「段 5 在 −60 dBFS 段门上、段 0 ～ 4 全在门下」——只靠逐段静音门（改前）这些帧算低频内容，合成曲上慢路随约 1 Hz 的拍频
        //      摆到 −0.95、反相提示亮了 breakdown 的 89 %；
        //   b. 响的 pad（基音幅度 0.5，+24 dB）：泄漏把段 0 ～ 4 也推到段门上（阳性对照：有帧的核心段在门上），绝对电平的门挡不住，
        //      挡住它的是「核心对上方 −35 dB」——两路照样每一块都无效；
        //   c. 同一个响的 pad 底下加 55 Hz 反相正弦（左 +、右 −，幅度 0.15：功率比 pad 每个基音低约 10 dB，核心 / 上方约 −15 dB）：
        //      真低音在核心里，门照常放行——最后 8 s 两路每一块都有效，慢路 ≤ −0.5、快路 ≤ −0.3（F 和弦时段 5 里还有 174.6 Hz 的泄漏，
        //      比低音低约 8 dB，最坏把 ρ 抬到约 −0.74，所以断言不写 −1）。
        void testClayLiveViewIgnoresLeakageFromAbove()
        {
            beginTest ("Clay live view: a bass-less detuned pad above 140 Hz only leaks into the 125 Hz band - the low correlation stays unknown; a real anti-phase sub under it still reads");

            constexpr double sampleRate = 48000.0;
            constexpr double seconds = 12.0;
            constexpr int blockLength = 512;
            const auto lateFromBlock = static_cast<int> (4.0 * sampleRate / blockLength);   // 最后 8 s

            struct Outcome
            {
                int blocks = 0, validBlocks = 0, lateBlocks = 0, lateValidBlocks = 0;
                float lateHighestSlow = -1.0f, lateHighestFast = -1.0f;
                int frames = 0, edgeOnlyFrames = 0, coreOverGateFrames = 0;
            };

            auto run = [&] (const Signal& input)
            {
                Rig rig (sampleRate);
                Outcome o;
                std::int64_t frameCursor = 0;
                std::array<BalanceModule::FrameSnapshot, 64> frames {};
                const auto& view = rig.analyzer.getShortTermView();

                // 自己的游标读 Balance 帧（与分析器读的是同一批帧）：只看逐段 −60 dBFS 门，数段 5 独自在门上、核心段有门上功率的帧。
                auto drainFrames = [&]
                {
                    for (;;)
                    {
                        const auto got = std::abs (rig.balance.readFramesSince (frameCursor, frames.data(), (int) frames.size()));

                        for (int f = 0; f < got; ++f)
                        {
                            const auto& power = frames[(size_t) f].bandPower;
                            bool coreOverGate = false;

                            for (int b = 0; b <= 4; ++b)
                                coreOverGate = coreOverGate || power[(size_t) b] >= 1.0e-6f;

                            ++o.frames;
                            o.coreOverGateFrames += coreOverGate ? 1 : 0;
                            o.edgeOnlyFrames += (! coreOverGate && power[(size_t) 5] >= 1.0e-6f) ? 1 : 0;
                        }

                        if (got < (int) frames.size())
                            return;
                    }
                };

                // 上一块处理完之后的视图（feed 在每块之前调这个钩子；最后一块在 feed 之后补看）。
                auto inspect = [&]
                {
                    const auto& slow = view.liveLowCorrelation;
                    const auto& fast = view.liveLowCorrelationFast;
                    o.validBlocks += (slow.valid || fast.valid) ? 1 : 0;

                    if (o.blocks >= lateFromBlock)
                    {
                        ++o.lateBlocks;

                        if (slow.valid && fast.valid)
                        {
                            ++o.lateValidBlocks;
                            o.lateHighestSlow = juce::jmax (o.lateHighestSlow, slow.value);
                            o.lateHighestFast = juce::jmax (o.lateHighestFast, fast.value);
                        }
                    }

                    ++o.blocks;
                };

                bool first = true;
                rig.feed (input, blockLength, 1, {}, [&] (Rig&)
                {
                    if (! first)
                        inspect();

                    first = false;
                    drainFrames();
                });
                inspect();
                drainFrames();
                return o;
            };

            auto percent = [] (int part, int whole) { return juce::String (100.0 * (double) part / (double) juce::jmax (1, whole), 1) + " %"; };

            const auto quietPad = makeDetunedPad (sampleRate, seconds, 2.0, 0.03);
            const auto loudPad = makeDetunedPad (sampleRate, seconds, 2.0, 0.5);
            auto padOverSub = loudPad;
            const auto sub = makeSine (1, sampleRate, 55.0, 0.15f, seconds);

            for (size_t i = 0; i < padOverSub.front().size(); ++i)
            {
                padOverSub[0][i] += sub[0][i];
                padOverSub[1][i] -= sub[0][i];
            }

            const auto quiet = run (quietPad);
            const auto loud = run (loudPad);
            const auto withSub = run (padOverSub);

            logMessage ("  quiet pad: blocks with a valid low correlation " + juce::String (quiet.validBlocks) + " / " + juce::String (quiet.blocks)
                        + "; frames where only the 125 Hz band is over the band gate " + percent (quiet.edgeOnlyFrames, quiet.frames));
            logMessage ("  loud pad: blocks with a valid low correlation " + juce::String (loud.validBlocks) + " / " + juce::String (loud.blocks)
                        + "; frames with 40 - 100 Hz leakage over the band gate " + percent (loud.coreOverGateFrames, loud.frames));
            logMessage ("  loud pad + anti-phase 55 Hz: last 8 s valid " + juce::String (withSub.lateValidBlocks) + " / "
                        + juce::String (withSub.lateBlocks) + ", highest slow rho " + juce::String (withSub.lateHighestSlow, 3)
                        + ", highest fast rho " + juce::String (withSub.lateHighestFast, 3));

            expectEquals (quiet.validBlocks, 0, "a: leakage of a bass-less pad never makes the low correlation valid");
            expect (quiet.edgeOnlyFrames > 0, "a: positive control - the per-band silence gate alone would have counted the 125 Hz leakage");
            expectEquals (loud.validBlocks, 0, "b: the gate is relative - a +24 dB pad's leakage is still not low content");
            expect (loud.coreOverGateFrames > 0, "b: positive control - the leakage does reach the 40 - 100 Hz bands over the band gate");
            expect (withSub.lateBlocks > 0 && withSub.lateValidBlocks == withSub.lateBlocks,
                    "c: a real sub under the pad keeps both live correlations valid");
            expect (withSub.lateHighestSlow <= -0.5f, "c: ... and the slow path reads the anti-phase sub");
            expect (withSub.lateHighestFast <= -0.3f, "c: ... and so does the fast path");
        }

        //==============================================================================
        // 依据：MusicAnalyzer.h「同一段音频、任意分块，画像逐位相同」；任务书「分块无关（逐位）」「画像（含代价）与块长无关逐位」。
        // 混合素材 12 s：部分相关的粉噪（L = a + b、R = a − b，b 比 a 低 6 dB → 宽带 ρ = (1 − 1/4) / (1 + 1/4) = 0.6）+ 25 Hz 正弦
        // + 低 12 dB 的鼓型（同相）。块长 512 为参照，1、333、4096 与两种随机块长（1 ～ 4096）下：修坯字段（画像——含先试后用的六项
        // 代价——与实时视图的慢路 / 快路 / 高频）、先试后用的基准 P99 与旧字段都逐位相同。
        // 阳性对照：参照里五个修坯特征、六项代价（12 s 有 11.6 s 有效块 ≥ 10 s）与实时视图都有效，低频 ρ 落在 0.3 ～ 0.98
        // （素材确实部分相关、每条路径都走到了），收单代价不为 0（S 有低频内容：收单确实改了峰值）。
        void testClayBlockSizeIndependence()
        {
            beginTest ("Clay fields (including the trial peak costs) and the live stereo view are bit-identical for any host block size");

            constexpr double sampleRate = 48000.0;
            const auto length = (int) (12.0 * sampleRate);
            auto input = midSide (makePinkNoise (1, length, -20.0f, 1701), makePinkNoise (1, length, -26.0206f, 1702));
            addInto (input, makeSine (numChannels, sampleRate, 25.0, 0.03f, 12.0));
            auto drums = makeDrumPattern (sampleRate, 12.0, 1703);
            scale (drums, 0.25f);
            addInto (input, drums);

            Rig reference (sampleRate);
            reference.analyzer.startLearning();
            reference.feed (input, 512);
            const auto& p = reference.profile();
            const auto& view = reference.analyzer.getShortTermView();

            const auto& trialBase = reference.analyzer.getAuxiliaryFeatures().trialPeakP99Db;

            logMessage ("  mixed material: sub share " + juce::String (p.subShareDb.value, 2) + " dB, low rho "
                        + juce::String (p.lowCorrelation.value, 3) + ", high rho " + juce::String (p.highCorrelation.value, 3)
                        + "; peak costs low-cut " + juce::String (p.lowCutPeakCostDb.value, 3) + " dB, mono 60 / 160 Hz "
                        + juce::String (p.monoPeakCostDb[0].value, 3) + " / " + juce::String (p.monoPeakCostDb[3].value, 3)
                        + " dB, width " + juce::String (p.widthPeakCostDb.value, 3) + " dB over " + juce::String (trialBase.effectiveSeconds, 2)
                        + " s of blocks");
            expect (p.subShareDb.valid && p.lowSideToMidDb.valid && p.lowCorrelation.valid && p.highSideToMidDb.valid
                        && p.highCorrelation.valid && view.liveLowCorrelation.valid && view.liveLowCorrelationFast.valid
                        && view.liveHighValid,
                    "the material must exercise every Clay path");
            expect (p.lowCutPeakCostDb.valid && p.monoPeakCostDb[0].valid && p.monoPeakCostDb[3].valid && p.widthPeakCostDb.valid
                        && trialBase.valid && p.monoPeakCostDb[3].value != 0.0f,
                    "the trial must run and measure something (12 s of learning = 11.6 s of valid blocks)");
            expect (p.lowCorrelation.value > 0.3f && p.lowCorrelation.value < 0.98f, "the lows are partially correlated");

            for (auto [blockSize, seed] : { std::pair<int, juce::int64> { 1, 0 }, std::pair<int, juce::int64> { 333, 0 },
                                            std::pair<int, juce::int64> { 4096, 0 }, std::pair<int, juce::int64> { 0, 5 },
                                            std::pair<int, juce::int64> { 0, 77 } })
            {
                Rig rig (sampleRate);
                rig.analyzer.startLearning();
                rig.feed (input, blockSize, seed);
                const auto tag = blockSize > 0 ? "block " + juce::String (blockSize) : "random blocks, seed " + juce::String (seed);
                expect (sameClayProfile (p, rig.profile()), "Clay profile fields (with the peak costs): " + tag);
                expect (sameFeature (trialBase, rig.analyzer.getAuxiliaryFeatures().trialPeakP99Db), "trial base P99: " + tag);
                expect (sameProfile (p, rig.profile()), "earlier profile fields: " + tag);
                expect (sameLiveStereo (view, rig.analyzer.getShortTermView()), "live stereo view: " + tag);
            }
        }

        //==============================================================================
        // 依据：任务书「0 分配」、TestMain.cpp 的 operator new 计数。修坯的三条路径（入口低通、立体声画像累计、实时视图）都走到：
        // 反相低频 + 同相高频 + 20 Hz 正弦，8 s，随机块长（1 ～ 4096）；途中开始 / 停止学习、三种不连续点（含表与 Balance 的 reset），
        // 每块读全部修坯字段。阳性对照：跑完时画像的立体声与超低频都有效、实时视图有过有效的时候（路径确实走到了）。
        void testClayNoAllocation()
        {
            beginTest ("Clay analysis paths do not allocate on the audio thread");

            constexpr double sampleRate = 48000.0;
            const auto length = (int) (8.0 * sampleRate);
            auto lows = makePinkNoise (1, length, -20.0f, 1801);
            auto highs = makePinkNoise (1, length, -20.0f, 1802);
            applyLowpass24 (lows, sampleRate, 150.0);
            applyHighpass24 (highs, sampleRate, 1500.0);
            auto input = midSide (highs, lows);
            addInto (input, makeSine (numChannels, sampleRate, 20.0, 0.05f, 8.0));

            Rig rig (sampleRate);
            juce::AudioBuffer<float> block (numChannels, 4096);
            juce::Random random (11);
            std::uint64_t allocations = 0;
            float sink = 0.0f;
            bool sawLiveView = false;

            for (int start = 0, index = 0; start < length; ++index)
            {
                const auto count = juce::jmin (1 + random.nextInt (4096), length - start);
                block.setSize (numChannels, count, false, false, true);

                for (int ch = 0; ch < numChannels; ++ch)
                    block.copyFrom (ch, 0, input[(size_t) ch].data() + start, count);

                const auto before = allocationCountOnThisThread();

                if (index == 0)
                    rig.analyzer.startLearning();

                if (index == 70)
                    rig.analyzer.handleDiscontinuity (Discontinuity::transportJump);

                if (index == 80)
                    rig.analyzer.handleDiscontinuity (Discontinuity::loopWrap);

                if (index == 90)
                {
                    rig.analyzer.handleDiscontinuity (Discontinuity::hostReset);
                    rig.meter.reset();
                    rig.balance.reset();
                }

                rig.meter.process (block);
                rig.analyzer.processInput (block, rig.meter);
                rig.balance.process (block);
                rig.analyzer.processSpectrum (rig.balance);

                const auto& p = rig.analyzer.getCandidateProfile();
                const auto& view = rig.analyzer.getShortTermView();
                sink += p.subShareDb.value + p.lowSideToMidDb.value + p.lowCorrelation.value + p.highSideToMidDb.value
                      + p.highCorrelation.value + p.bandSideToMidDb[3] + p.bandCorrelation[20] + p.bandStereoConfidence[10]
                      + view.liveLowCorrelation.value + view.liveLowCorrelationFast.value + view.liveHighMidPower
                      + view.liveHighSidePower + p.lowCutPeakCostDb.value + p.widthPeakCostDb.value;
                sawLiveView = sawLiveView || (view.liveLowCorrelation.valid && view.liveHighValid);

                if (index == 150)
                    rig.analyzer.stopLearning();

                allocations += allocationCountOnThisThread() - before;
                start += count;
            }

            const auto& learned = rig.profile();
            expect (std::isfinite (sink));
            expect (learned.subShareDb.valid && learned.lowCorrelation.valid && learned.highCorrelation.valid && sawLiveView,
                    "the run went through every Clay path");
            expectEquals ((int) allocations, 0);
        }

        //==============================================================================
        // 依据：设计规格-Clay-根治修订 3.2（ADR-012）「候选：低切 IIR 4 阶 20 Hz、线性相位收单 a = 1 × 4 个分频点、加宽 2 kHz 高架
        // +1.5 dB，各自跑在原始输入上；代价 = 20·log10(P99 候选块峰值) − 20·log10(P99 原始块峰值)；有效块满 10 s 才有效，置信度同其他
        // 特征」、第 4 节第 2 条（合成回归）；任务书「硬削顶的底鼓 lowCutPeakCostDb > 1 dB；纯单声道收单代价 |x| ≤ 0.05 dB；宽带素材
        // 加宽代价 > 0」。各 12 s（有效块 116 个 = 11.6 s：前 3 块在 250 ms 预热里、最后一块的收单候选还没交齐）。期望值都经 109 上的
        // numpy 按同一设计复算（同样的候选滤波器、延迟补偿的块网格、线性插值 P99）：
        //  a) 硬削顶的底鼓（模拟已限幅的成品）：每 0.5 s 一击 50 Hz 正弦（从相位 0 起、包络 e^(−t / 0.12 s)、长 0.45 s）× 0.5 + 粉噪
        //     −26 dBFS，整体 × 2 后在 ±0.5 削平，两声道相同。20 Hz 4 阶高通在 50 Hz 处的相位旋转把削平的顶错开、峰值重新冒出来：
        //     低切代价 numpy +5.59 dB，断言 > 1 dB；同一素材不削顶（× 1、不削）numpy −0.17 dB，断言 < 0.5 dB——代价来自削顶，
        //     不是低切本身抬峰。纯单声道（S = 0）：收单后逐位就是延迟了的原始（延迟补偿的块网格上同一段音频），加宽也不改变任何样本
        //     → 收单代价 |x| ≤ 0.05 dB（任务书口径）；更紧的「≤ 1e-6 dB、加宽 == 0」是这个设计的精确性质（线性 FIR 零进零出），
        //     块网格对不齐时就不成立（见下一个用例）。
        //  b) 两声道独立的粉噪（−20 dBFS）：宽带素材，S 在 2 kHz 以上被抬 1.5 dB → 峰值变高：加宽代价 numpy +0.28 dB，断言 > 0.05 dB
        //     （任务书「> 0」，留 5 倍余量）；S 的低频被收掉、能量变少 → 收单代价为负（numpy −0.81 ～ −1.10 dB，断言都 < −0.3 dB：
        //     收单这条路径确实量到了东西——阳性对照）。有效时长 11.6 s；置信度 = min(观测充分度 1, 代表性 0.75)（没有宿主位置时
        //     代表性封顶 0.75）。
        void testClayTrialCosts()
        {
            beginTest ("Clay trial: peak costs of the candidate processings (hard-clipped kick, pure mono, broadband stereo)");

            constexpr double sampleRate = 48000.0;
            constexpr double seconds = 12.0;
            const auto length = (int) (seconds * sampleRate);
            const auto pink = makePinkNoise (1, length, -26.0f, 2101);
            const auto kick = makeKickTrain (sampleRate, seconds, 50.0, 0.45, 0.5, [] (double t) { return std::exp (-t / 0.12); });

            // 底鼓 + 粉噪，× drive，clip 时在 ±0.5 削平；两声道相同。
            auto kickMaterial = [&] (float drive, bool clip)
            {
                auto material = makeSilence (1, length);

                for (size_t i = 0; i < material[0].size(); ++i)
                {
                    const auto x = drive * (0.5f * kick[0][i] + pink[0][i]);
                    material[0][i] = clip ? juce::jlimit (-0.5f, 0.5f, x) : x;
                }

                return identicalStereo (material);
            };

            auto learnedFrom = [&] (const Signal& input)
            {
                Rig rig (sampleRate);
                rig.analyzer.startLearning();
                rig.feed (input, 512);
                return std::make_pair (rig.profile(), rig.analyzer.getAuxiliaryFeatures());
            };

            // a) 削顶底鼓（纯单声道）与不削顶的对照。
            const auto clipped = learnedFrom (kickMaterial (2.0f, true));
            const auto unclipped = learnedFrom (kickMaterial (1.0f, false));
            const auto& c = clipped.first;

            logMessage ("  hard-clipped kick: low-cut cost " + juce::String (c.lowCutPeakCostDb.value, 3) + " dB (unclipped "
                        + juce::String (unclipped.first.lowCutPeakCostDb.value, 3) + " dB), mono costs "
                        + juce::String (c.monoPeakCostDb[0].value, 6) + " / " + juce::String (c.monoPeakCostDb[1].value, 6) + " / "
                        + juce::String (c.monoPeakCostDb[2].value, 6) + " / " + juce::String (c.monoPeakCostDb[3].value, 6)
                        + " dB, width cost " + juce::String (c.widthPeakCostDb.value, 6) + " dB, base P99 "
                        + juce::String (clipped.second.trialPeakP99Db.value, 3) + " dBFS");

            expect (c.lowCutPeakCostDb.valid && c.lowCutPeakCostDb.value > 1.0f,
                    "a) hard-clipped kick: the 20 Hz low-cut raises the P99 block peak by more than 1 dB");
            expect (unclipped.first.lowCutPeakCostDb.valid && unclipped.first.lowCutPeakCostDb.value < 0.5f,
                    "a) the same kick unclipped: the cost comes from the clipped (already limited) peaks");

            for (size_t i = 0; i < c.monoPeakCostDb.size(); ++i)
            {
                const auto& mono = c.monoPeakCostDb[i];
                const auto tag = juce::String (glaze::clayMonoCandidateHz[i]) + " Hz";
                expect (mono.valid && std::abs (mono.value) <= 0.05f, "a) pure mono: the linear-phase mono costs nothing at " + tag);
                expect (std::abs (mono.value) <= 1.0e-6f, "a) pure mono, delay-compensated grid: exactly nothing at " + tag);
            }

            expect (c.widthPeakCostDb.valid && c.widthPeakCostDb.value == 0.0f, "a) pure mono: widening the side changes no sample");

            // 基准：每 5 块里有一块含底鼓、削在 0.5，116 块的 P99 落在第 3 大与第 2 大之间——两者都是 0.5 → 精确的 −6.0206 dBFS。
            expectWithinAbsoluteError (clipped.second.trialPeakP99Db.value, (float) (20.0 * std::log10 (0.5)), 1.0e-4f,
                                       "a) the trial base of material clipped at 0.5 is 0.5");

            // b) 两声道独立的粉噪。
            const auto wide = learnedFrom (makePinkNoise (numChannels, length, -20.0f, 2201));
            const auto& w = wide.first;

            logMessage ("  independent pink noise: width cost " + juce::String (w.widthPeakCostDb.value, 4) + " dB, mono costs "
                        + juce::String (w.monoPeakCostDb[0].value, 4) + " / " + juce::String (w.monoPeakCostDb[1].value, 4) + " / "
                        + juce::String (w.monoPeakCostDb[2].value, 4) + " / " + juce::String (w.monoPeakCostDb[3].value, 4)
                        + " dB, low-cut cost " + juce::String (w.lowCutPeakCostDb.value, 4) + " dB, over "
                        + juce::String (w.widthPeakCostDb.effectiveSeconds, 2) + " s, confidence "
                        + juce::String (w.widthPeakCostDb.confidence, 3));

            expect (w.widthPeakCostDb.valid && w.widthPeakCostDb.value > 0.05f, "b) broadband stereo: widening raises the peaks");

            for (size_t i = 0; i < w.monoPeakCostDb.size(); ++i)
                expect (w.monoPeakCostDb[i].valid && w.monoPeakCostDb[i].value < -0.3f,
                        "b) collapsing the independent lows lowers the peaks (the mono path measures something) at "
                            + juce::String (glaze::clayMonoCandidateHz[i]) + " Hz");

            expectWithinAbsoluteError (w.widthPeakCostDb.effectiveSeconds, 11.6, 1.0e-9, "b) 116 valid blocks of 100 ms");
            expectWithinAbsoluteError (w.widthPeakCostDb.confidence, 0.75f, 1.0e-6f,
                                       "b) confidence = min(observation 1, representativeness 0.75 without a host position)");
            expect (wide.second.trialPeakP99Db.valid && wide.second.trialPeakP99Db.value < -6.5f,
                    "b) the trial base follows the material (numpy: -7.54 dBFS)");
        }

        //==============================================================================
        // 依据：同上（候选输出晚 D_c 个样本，它的第 k 块取延迟补偿后的同一段音频）。P99 只看最大的两三个块峰值，普通素材上块网格
        // 错开几个样本也未必看得出来；这里把峰值故意放在块边界两侧：纯单声道、−40 dBFS 的粉噪（块峰值 < 0.05）上，在第 20、40、60、
        // 80、100 块的最后一个样本与下一块的第一个样本各放一个单样本脉冲（0.9 / 0.85、0.8 / 0.75、0.7 / 0.65、0.6 / 0.55、0.5 / 0.45）。
        //   - 原始：每个脉冲各占一块。116 个有效块，P99 的位置 0.99 × 115 = 113.85，落在第 3 大（0.8）与第 2 大（0.85）之间：
        //     0.8 + 0.85 × 0.05 = 0.8425 → −1.4886 dBFS（基准 trialPeakP99Db，断言 ±0.001 dB）。块网格从重启起算、前 3 块预热、最后
        //     一块未交齐这几条都对，块数才是 116、插值位置才对；有效时长 11.6 s。
        //   - 收单候选若没做延迟补偿（或差 1 个样本），边界两侧的一对脉冲会落进同一个候选块，第 2、3 大的块峰值变成 0.8 / 0.7，
        //     P99 变成 0.785、代价约 −0.61 dB（numpy）；对齐时四个收单代价与加宽代价都是 0（断言 ≤ 1e-6 dB / == 0）。
        void testClayTrialAlignment()
        {
            beginTest ("Clay trial: the delayed mono candidates are measured on the same block grid (delay-compensated)");

            constexpr double sampleRate = 48000.0;
            constexpr int blockSamples = 4800;
            auto material = makePinkNoise (1, (int) (12.0 * sampleRate), -40.0f, 2301);
            const std::array<std::array<float, 2>, 5> pulses { { { 0.9f, 0.85f }, { 0.8f, 0.75f }, { 0.7f, 0.65f },
                                                                  { 0.6f, 0.55f }, { 0.5f, 0.45f } } };

            for (size_t j = 0; j < pulses.size(); ++j)
            {
                const auto lastOfBlock = 20 * (int) (j + 1);
                material[0][(size_t) ((lastOfBlock + 1) * blockSamples - 1)] = pulses[j][0];
                material[0][(size_t) ((lastOfBlock + 1) * blockSamples)] = pulses[j][1];
            }

            Rig rig (sampleRate);
            rig.analyzer.startLearning();
            rig.feed (material, 512);   // 单声道信号：Rig 把第 0 声道拷到两个声道
            const auto& p = rig.profile();
            const auto& base = rig.analyzer.getAuxiliaryFeatures().trialPeakP99Db;
            const auto expectedBase = 20.0 * std::log10 ((double) pulses[1][0] + 0.85 * ((double) pulses[0][1] - (double) pulses[1][0]));

            logMessage ("  pulse pairs across block edges: base P99 " + juce::String (base.value, 5) + " dBFS (expected "
                        + juce::String (expectedBase, 5) + ") over " + juce::String (base.effectiveSeconds, 2) + " s; mono costs "
                        + juce::String (p.monoPeakCostDb[0].value, 7) + " / " + juce::String (p.monoPeakCostDb[3].value, 7) + " dB");

            expect (base.valid, "the trial base is valid");
            expectWithinAbsoluteError (base.value, (float) expectedBase, 1.0e-3f, "each pulse in its own block: P99 between the 3rd and 2nd largest");
            expectWithinAbsoluteError (base.effectiveSeconds, 11.6, 1.0e-9, "116 valid blocks");

            for (size_t i = 0; i < p.monoPeakCostDb.size(); ++i)
                expect (p.monoPeakCostDb[i].valid && std::abs (p.monoPeakCostDb[i].value) <= 1.0e-6f,
                        "the " + juce::String (glaze::clayMonoCandidateHz[i]) + " Hz mono candidate sees each pulse in the same block");

            expect (p.widthPeakCostDb.valid && p.widthPeakCostDb.value == 0.0f, "the width candidate (no delay) too");
        }

        //==============================================================================
        // 依据：设计规格-Clay-根治修订 3.2「候选只在学习期间跑（不学习时不耗 CPU）」、任务书「学习区间外候选不更新；重启 / 不连续点按
        // 现有规则处理候选状态与块网格」。X = 两声道独立的粉噪 12 s（上一类用例 b：低切代价 numpy −0.96 dB）；Y = 削顶底鼓 5 s
        // （上一类用例 a 的素材：低切代价约 +5.4 dB）。
        //   - 只学 X；与「先送 Y（不学习）、再开始学习、学同一段 X」比较：学习起点正好落在块网格的边界上（5 s = 50 块），候选在学习开始时
        //     从零状态起跑，两边的有效块一一对应、送进候选的是同一段样本 → 六项代价与基准 P99 逐位相同（Y 若漏进候选或块记录，
        //     低切代价会被抬到 +5 dB 附近）；
        //   - 只学 X 的那一份停止学习后再送 5 s 的 Y：代价与基准逐位不变（学习区间外候选不跑、不记）。
        void testClayTrialLearningInterval()
        {
            beginTest ("Clay trial: the candidates run only inside the learning interval");

            constexpr double sampleRate = 48000.0;
            const auto x = makePinkNoise (numChannels, (int) (12.0 * sampleRate), -20.0f, 2201);
            const auto leadLength = (int) (5.0 * sampleRate);
            const auto pink = makePinkNoise (1, leadLength, -26.0f, 2101);
            const auto kick = makeKickTrain (sampleRate, 5.0, 50.0, 0.45, 0.5, [] (double t) { return std::exp (-t / 0.12); });
            auto clippedKick = makeSilence (1, leadLength);

            for (size_t i = 0; i < clippedKick[0].size(); ++i)
                clippedKick[0][i] = juce::jlimit (-0.5f, 0.5f, 2.0f * (0.5f * kick[0][i] + pink[0][i]));

            const auto y = identicalStereo (clippedKick);

            Rig learnOnly (sampleRate);
            learnOnly.analyzer.startLearning();
            learnOnly.feed (x, 512);
            const auto learned = learnOnly.profile();
            const auto learnedBase = learnOnly.analyzer.getAuxiliaryFeatures().trialPeakP99Db;

            Rig withLeadIn (sampleRate);
            withLeadIn.feed (y, 512);
            withLeadIn.analyzer.startLearning();
            withLeadIn.feed (x, 512);

            logMessage ("  learning interval: low-cut cost " + juce::String (learned.lowCutPeakCostDb.value, 4) + " dB (learned only), "
                        + juce::String (withLeadIn.profile().lowCutPeakCostDb.value, 4) + " dB (after a clipped-kick lead-in)");

            expect (learned.lowCutPeakCostDb.valid && learned.widthPeakCostDb.valid && learnedBase.valid,
                    "12 s of learning define the costs (positive control)");
            expect (sameTrialCosts (learned, withLeadIn.profile())
                        && sameFeature (learnedBase, withLeadIn.analyzer.getAuxiliaryFeatures().trialPeakP99Db),
                    "audio before startLearning never reaches the candidates or the block records (bit-identical costs)");

            learnOnly.analyzer.stopLearning();
            learnOnly.feed (y, 512);
            expect (sameTrialCosts (learned, learnOnly.profile())
                        && sameFeature (learnedBase, learnOnly.analyzer.getAuxiliaryFeatures().trialPeakP99Db),
                    "after stopLearning the candidates are not updated");
        }

        //==============================================================================
        // 依据：任务书「0 分配」、TestMain.cpp 的 operator new 计数。先试后用在音频线程上的全部路径：开始学习与重启时候选的清零、
        // 逐段处理、块记录与 P99、画像更新；途中一次跳播（组网格与候选重启）、一次停止学习与重新开始学习。素材：两声道独立的粉噪 +
        // 底鼓，在 ±0.5 削平（每个候选都有事可做），16 s，随机块长（1 ～ 4096）。阳性对照：第一段学习（到 14.5 s；跳播之后约 13 s 的
        // 有效块）里六项代价都变为有效过。
        void testClayTrialNoAllocation()
        {
            beginTest ("Clay trial paths do not allocate on the audio thread");

            constexpr double sampleRate = 48000.0;
            const auto length = (int) (16.0 * sampleRate);
            auto input = makePinkNoise (numChannels, length, -20.0f, 2401);
            const auto kick = makeKickTrain (sampleRate, 16.0, 50.0, 0.45, 0.5, [] (double t) { return std::exp (-t / 0.12); });

            for (auto& channel : input)
                for (size_t i = 0; i < channel.size(); ++i)
                    channel[i] = juce::jlimit (-0.5f, 0.5f, channel[i] + kick[0][i]);

            Rig rig (sampleRate);
            juce::AudioBuffer<float> block (numChannels, 4096);
            juce::Random random (13);
            std::uint64_t allocations = 0;
            float sink = 0.0f;
            bool sawCosts = false, stopped = false, restarted = false;
            const auto stopAt = (int) (14.5 * sampleRate);
            const auto restartAt = (int) (15.0 * sampleRate);

            for (int start = 0, index = 0; start < length; ++index)
            {
                const auto count = juce::jmin (1 + random.nextInt (4096), length - start);
                block.setSize (numChannels, count, false, false, true);

                for (int ch = 0; ch < numChannels; ++ch)
                    block.copyFrom (ch, 0, input[(size_t) ch].data() + start, count);

                const auto before = allocationCountOnThisThread();

                if (index == 0)
                    rig.analyzer.startLearning();

                if (index == 20)
                    rig.analyzer.handleDiscontinuity (Discontinuity::transportJump);

                if (! stopped && start >= stopAt)
                {
                    rig.analyzer.stopLearning();
                    stopped = true;
                }

                if (stopped && ! restarted && start >= restartAt)
                {
                    rig.analyzer.startLearning();
                    restarted = true;
                }

                rig.meter.process (block);
                rig.analyzer.processInput (block, rig.meter);
                rig.balance.process (block);
                rig.analyzer.processSpectrum (rig.balance);

                const auto& p = rig.analyzer.getCandidateProfile();
                sink += p.lowCutPeakCostDb.value + p.monoPeakCostDb[0].value + p.monoPeakCostDb[3].value + p.widthPeakCostDb.value
                      + rig.analyzer.getAuxiliaryFeatures().trialPeakP99Db.value;
                sawCosts = sawCosts || (p.lowCutPeakCostDb.valid && p.monoPeakCostDb[1].valid && p.monoPeakCostDb[2].valid
                                        && p.widthPeakCostDb.valid);

                allocations += allocationCountOnThisThread() - before;
                start += count;
            }

            expect (std::isfinite (sink));
            expect (sawCosts && stopped && restarted, "the run reached valid peak costs and went through stop / restart");
            expectEquals ((int) allocations, 0);
        }
    };

    static MusicAnalyzerClayTests musicAnalyzerClayTests;

    //==============================================================================
    // ---- T-038 House 档（阶段 2 B 追加）----
    // 分析器接上段落账（SegmentLedger）之后：House 档的候选画像由满编段门控算出（设计规格-曲风-House学习方式 第 2 节；MusicAnalyzer.cpp
    // 「六」节）。只在文件末尾追加：上面的旧用例与工具一个不改，另起一个测试类（名字 MusicAnalyzerHouse），复用上面的 Rig 与逐位比较。
    namespace
    {
        constexpr double houseTrackBpm = 124.0;

        // 恒定速度、4/4、在播的宿主：PPQ 与位置严格一致，小节起点按 PPQ ÷ 4。
        HostInfo tempoHost (std::int64_t position, double sampleRate)
        {
            auto info = playingAt (position);
            info.hasTempo = true;
            info.bpm = houseTrackBpm;
            info.hasPpq = true;
            info.ppqPosition = (double) position * houseTrackBpm / (60.0 * sampleRate);
            info.hasBarStart = true;
            info.ppqLastBarStart = std::floor (info.ppqPosition / 4.0) * 4.0;
            info.hasTimeSignature = true;
            info.timeSignatureNumerator = 4;
            info.timeSignatureDenominator = 4;
            return info;
        }

        // 合成的 House（124 BPM、4/4）：前奏 introBars 小节只有鼓（底鼓每拍 120 → 50 Hz 下滑、踩镲在反拍），之后 dropBars 小节 drop
        // （底鼓更响 + 持续低音 55 Hz 锯齿 1 ～ 6 次谐波 + 和声垫 + 踩镲）；drop 比前奏响约 8 LU（Max 曲库里「前奏 16 ～ 32 小节只有鼓」
        // 的典型，设计规格第 1 节）。两声道踩镲噪声不相关，其余左右相同。末尾多 extraSeconds 秒 drop，让最后一小节交齐、定稿。
        Signal makeHouseTrack (double sampleRate, int introBars, int dropBars, double extraSeconds, juce::int64 seed)
        {
            const auto beat = 60.0 / houseTrackBpm;
            const auto bar = 4.0 * beat;
            const auto numSamples = (int) std::lround (((double) (introBars + dropBars) * bar + extraSeconds) * sampleRate);
            const auto dropStart = (double) introBars * bar;
            const auto twoPi = juce::MathConstants<double>::twoPi;
            auto signal = makeSilence (numChannels, numSamples);

            for (int ch = 0; ch < numChannels; ++ch)
            {
                juce::Random random (seed + (juce::int64) ch * 7919);
                double previous = 0.0;
                auto& channel = signal[(size_t) ch];

                for (int i = 0; i < numSamples; ++i)
                {
                    const auto t = (double) i / sampleRate;
                    const auto inDrop = t >= dropStart;
                    const auto beatPosition = std::fmod (t, beat);
                    const auto offBeatPosition = std::fmod (t + 0.5 * beat, beat);
                    const auto kick = (inDrop ? 0.5 : 0.35) * std::exp (-beatPosition / 0.12)
                                    * std::sin (twoPi * (50.0 * beatPosition + 70.0 * 0.03 * (1.0 - std::exp (-beatPosition / 0.03))));
                    const auto white = (double) random.nextFloat() * 2.0 - 1.0;
                    const auto hat = (inDrop ? 0.10 : 0.08) * std::exp (-offBeatPosition / 0.025) * (white - previous);
                    previous = white;

                    double bass = 0.0, pad = 0.0;

                    if (inDrop)
                    {
                        for (int h = 1; h <= 6; ++h)
                            bass += std::sin (twoPi * 55.0 * h * t) / h;

                        bass *= 0.22;

                        for (const auto hz : { 220.0, 277.18, 329.63 })
                            pad += 0.05 * std::sin (twoPi * hz * t + 0.7 * ch);
                    }

                    channel[(size_t) i] = (float) (kick + hat + bass + pad);
                }
            }

            return signal;
        }

        bool sameHouseGate (const glaze::HouseGateSummary& a, const glaze::HouseGateSummary& b)
        {
            return a.valid == b.valid && a.mode == b.mode && sameFloat (a.referenceLufs, b.referenceLufs)
                && sameFloat (a.lowShareReference, b.lowShareReference) && sameFloat (a.gateWidthLu, b.gateWidthLu)
                && sameFloat (a.lowGateFactor, b.lowGateFactor) && a.heardSegments == b.heardSegments && a.fullSegments == b.fullSegments
                && sameDouble (a.heardSeconds, b.heardSeconds) && sameDouble (a.fullSeconds, b.fullSeconds) && a.reopenCount == b.reopenCount;
        }

        // House 画像逐位相同：通用画像的全部字段 + 格式 2 的修坯字段 + 门控结果。
        bool sameHouseProfile (const MusicProfile& a, const MusicProfile& b)
        {
            return sameProfile (a, b) && sameClayProfile (a, b) && sameHouseGate (a.houseGate, b.houseGate);
        }
    }

    class MusicAnalyzerHouseTests  : public juce::UnitTest
    {
    public:
        MusicAnalyzerHouseTests() : juce::UnitTest ("MusicAnalyzerHouse", "Celadon") {}

        void runTest() override
        {
            testIntroThenDrop();
            testGeneralUntouched();
            testBlockSizeIndependence();
            testLedgerAfterLearning();
            testNoAllocation();
            testPerformance();
        }

    private:
        // 依据：设计规格第 1、2 节。32 小节只有鼓的前奏接 16 小节 drop：House 档的画像取的是 drop——I、真峰值、低频占比与只听 drop
        // 的通用画像相近（I 差 ≤ 0.5 LU：满编段的门块逐个过相对门、通用画像按 0.1 LU 格中心，又多了前奏 → drop 交界的几个门块）；
        // 通用档的画像被前奏拉低。门控结果：已听 48 小节、满编 = drop 的 16 小节；代表性按「已听 48 ÷ 锁定所需 64」封顶 0.75；格式 3。
        void testIntroThenDrop()
        {
            beginTest ("House: a 32-bar drums-only intro, then the drop - the House profile is the drop's, the general one is pulled down");

            constexpr double sampleRate = 48000.0;
            const auto track = makeHouseTrack (sampleRate, 32, 16, 1.0, 11);
            const auto host = [] (std::int64_t fed, int) { return tempoHost (fed, sampleRate); };

            Rig house (sampleRate);
            house.analyzer.setGenre (glaze::Genre::deepHouse);
            house.analyzer.startLearning();
            house.feed (track, 512, 1, host);
            const auto houseProfile = house.profile();
            const auto& gate = houseProfile.houseGate;

            Rig general (sampleRate);
            general.analyzer.startLearning();
            general.feed (track, 512, 1, host);
            const auto& generalProfile = general.profile();

            Rig dropOnly (sampleRate);
            dropOnly.analyzer.startLearning();
            dropOnly.feed (makeHouseTrack (sampleRate, 0, 16, 1.0, 12), 512, 1, host);
            const auto& drop = dropOnly.profile();

            logMessage ("  House I " + juce::String (houseProfile.integratedLufs.value, 2) + " LUFS (drop alone "
                        + juce::String (drop.integratedLufs.value, 2) + ", general over the whole track "
                        + juce::String (generalProfile.integratedLufs.value, 2) + "); lowShare " + juce::String (houseProfile.lowShare.value, 3)
                        + " (drop " + juce::String (drop.lowShare.value, 3) + ", general " + juce::String (generalProfile.lowShare.value, 3)
                        + "); S_ref " + juce::String (gate.referenceLufs, 2) + ", heard " + juce::String (gate.heardSegments)
                        + ", full " + juce::String (gate.fullSegments) + ", representativeness " + juce::String (houseProfile.representativeness, 3));

            expect (houseProfile.formatVersion == glaze::houseProfileFormatVersion && gate.valid && gate.mode == glaze::SegmentMode::bars,
                    "House profile: format 3 with a valid bar-mode gate");
            expect (generalProfile.formatVersion == glaze::clayProfileFormatVersion && ! generalProfile.houseGate.valid,
                    "the general profile stays format 2");
            expectEquals (gate.heardSegments, 48);
            expectEquals (gate.fullSegments, 16, "the full segments are the 16 drop bars");
            expect (houseProfile.integratedLufs.valid && std::abs (houseProfile.integratedLufs.value - drop.integratedLufs.value) <= 0.5f,
                    "House I is the drop's I");
            expect (houseProfile.integratedLufs.value - generalProfile.integratedLufs.value > 1.5f,
                    "the general I is pulled down by the intro");
            expect (std::abs (houseProfile.lowShare.value - drop.lowShare.value) <= 0.03f, "House lowShare is the drop's");
            expect (std::abs (houseProfile.truePeakDbtp.value - drop.truePeakDbtp.value) <= 0.3f, "House true peak is the drop's");
            // 阶段 3 集成改：LRA 也只用满编段（起播位置无关），所以等于 drop 自己的 LRA，不再被前奏撑大。
            expect (houseProfile.loudnessRangeLu.valid && std::abs (houseProfile.loudnessRangeLu.value - drop.loudnessRangeLu.value) <= 1.0f,
                    "LRA is taken over the full segments only, so it is the drop's LRA, not stretched by the intro");
            expectWithinAbsoluteError (houseProfile.representativeness, 48.0f / 64.0f, 1.0e-6f);
            expect (houseProfile.integratedLufs.confidence <= houseProfile.representativeness + 1.0e-6f);
            expect (houseProfile.lowCutPeakCostDb.valid && houseProfile.monoPeakCostDb[1].valid && houseProfile.widthPeakCostDb.valid,
                    "the try-before-use costs come from the full segments' blocks");

            // 换底到 main fd486ba（T-043 守护混合损失）：House 画像的混合损失同样按满编段的块合并，与只听 drop 的通用画像相近。合并前
            // House 画像从空画像算起、没有这一项，决策器在 House 档就一直「判断不了」、不按混合损失封顶低切。
            for (size_t c = 0; c < houseProfile.lowCutBlendLossDb.size(); ++c)
            {
                const auto& h = houseProfile.lowCutBlendLossDb[c];
                const auto& d = drop.lowCutBlendLossDb[c];
                logMessage ("  blend loss candidate " + juce::String ((int) c) + ": House " + juce::String (h.value, 4) + " dB (valid "
                            + juce::String ((int) h.valid) + ", confidence " + juce::String (h.confidence, 4) + "), drop alone "
                            + juce::String (d.value, 4) + ", general " + juce::String (generalProfile.lowCutBlendLossDb[c].value, 4));
                expect (h.valid && d.valid, "the blend loss comes from the full segments' blocks, candidate " + juce::String ((int) c));
                expect (std::abs (h.value - d.value) <= 0.1f, "House blend loss is the drop's, candidate " + juce::String ((int) c));

                // T-049：置信度与低切代价同一口径（同一批块，min(代表性, 试算观测充分度)），两种画像都一样。原来 houseConfidences
                // 漏了这两项，House 画像里恒为 0。
                const auto& g = generalProfile.lowCutBlendLossDb[c];
                expect (h.confidence > 0.0f && sameFloat (h.confidence, houseProfile.lowCutPeakCostDb.confidence),
                        "House blend loss confidence = the low-cut cost's (same blocks, same rule), candidate " + juce::String ((int) c));
                expect (g.valid && sameFloat (g.confidence, generalProfile.lowCutPeakCostDb.confidence),
                        "general blend loss confidence = the low-cut cost's, candidate " + juce::String ((int) c));
            }
        }

        // 依据：任务书「通用档完全不走这些代码（逐位不变）」。通用档下交宿主拍点、显式 setGenre(通用)：画像与辅助量与不交拍点、
        // 从没调过 setGenre 的逐位相同；House 档里通用累计照常——学完换回通用档，候选画像与一直在通用档的逐位相同。
        void testGeneralUntouched()
        {
            beginTest ("general mode is bit-identical with or without tempo info; the House run leaves the general accumulation untouched");

            constexpr double sampleRate = 48000.0;
            const auto track = makeHouseTrack (sampleRate, 6, 6, 1.0, 21);

            Rig plain (sampleRate);
            plain.analyzer.startLearning();
            plain.feed (track, 512, 1, [] (std::int64_t fed, int) { return playingAt (fed); });

            Rig tempo (sampleRate);
            tempo.analyzer.setGenre (glaze::Genre::general);
            tempo.analyzer.startLearning();
            tempo.feed (track, 512, 1, [] (std::int64_t fed, int) { return tempoHost (fed, sampleRate); });

            expect (sameProfile (plain.profile(), tempo.profile()) && sameClayProfile (plain.profile(), tempo.profile()),
                    "general profile: tempo info changes nothing");
            expect (sameAuxiliary (plain.analyzer.getAuxiliaryFeatures(), tempo.analyzer.getAuxiliaryFeatures()));
            expectEquals (tempo.analyzer.getSegmentLedger().getNumStored(), 0, "the ledger is not touched in general mode");

            Rig house (sampleRate);
            house.analyzer.setGenre (glaze::Genre::techHouse);
            house.analyzer.startLearning();
            house.feed (track, 512, 1, [] (std::int64_t fed, int) { return tempoHost (fed, sampleRate); });
            expect (house.analyzer.getSegmentLedger().getNumStored() > 0);
            house.analyzer.setGenre (glaze::Genre::general);

            expect (sameProfile (plain.profile(), house.profile()) && sameClayProfile (plain.profile(), house.profile()),
                    "switching back to general returns the untouched general profile");
            expect (sameAuxiliary (plain.analyzer.getAuxiliaryFeatures(), house.analyzer.getAuxiliaryFeatures()));
        }

        // 依据：设计规格第 2.1 节「恒定速度下，任意分块得到的画像逐位相同（测试覆盖）」。6 小节前奏 + 6 小节 drop：块长 512、1、333、
        // 4096 与两种随机块长下 House 画像（含门控结果）与辅助量逐位相同。
        void testBlockSizeIndependence()
        {
            beginTest ("House profile and gate are bit-identical for any host block size at a constant tempo (1 ... 4096, random)");

            constexpr double sampleRate = 48000.0;
            const auto track = makeHouseTrack (sampleRate, 6, 6, 1.0, 31);
            const auto host = [] (std::int64_t fed, int) { return tempoHost (fed + 7777, sampleRate); };   // 不从小节线起播

            Rig reference (sampleRate);
            reference.analyzer.setGenre (glaze::Genre::melodicProgressive);
            reference.analyzer.startLearning();
            reference.feed (track, 512, 1, host);
            expect (reference.profile().houseGate.fullSegments > 0 && reference.profile().integratedLufs.valid
                    && reference.profile().onsetDensityPerSecond.valid, "the material exercises the gate and all three streams");

            for (const auto [blockSize, seed] : { std::pair<int, juce::int64> { 1, 0 }, std::pair<int, juce::int64> { 333, 0 },
                                                  std::pair<int, juce::int64> { 4096, 0 }, std::pair<int, juce::int64> { 0, 5 },
                                                  std::pair<int, juce::int64> { 0, 77 } })
            {
                Rig rig (sampleRate);
                rig.analyzer.setGenre (glaze::Genre::melodicProgressive);
                rig.analyzer.startLearning();
                rig.feed (track, blockSize, seed, host);
                const auto tag = blockSize > 0 ? "block " + juce::String (blockSize) : "random blocks, seed " + juce::String (seed);
                expect (sameHouseProfile (reference.profile(), rig.profile()), "House profile: " + tag);
                expect (sameAuxiliary (reference.analyzer.getAuxiliaryFeatures(), rig.analyzer.getAuxiliaryFeatures()), "auxiliary: " + tag);
            }
        }

        // 依据：设计规格第 4 节「锁定后继续按段记账」、MusicAnalyzer.h「不学习时冻结，编排器重开时 recomputeHouseProfile」。
        // 学完前奏停止学习，再播 drop：段落账照记、画像不动；recomputeHouseProfile 之后画像取的是 drop。startLearning 清空段落账。
        void testLedgerAfterLearning()
        {
            beginTest ("after stopLearning the ledger keeps recording and the profile stays frozen until recomputeHouseProfile");

            constexpr double sampleRate = 48000.0;
            const auto track = makeHouseTrack (sampleRate, 8, 8, 1.0, 41);
            const auto barSamples = (int) std::lround (4.0 * 60.0 / houseTrackBpm * sampleRate);
            const auto host = [] (std::int64_t fed, int) { return tempoHost (fed, sampleRate); };

            Rig rig (sampleRate);
            rig.analyzer.setGenre (glaze::Genre::deepHouse);
            rig.analyzer.startLearning();
            rig.feed (sliceOf (track, 0, 8 * barSamples), 512, 1, host);
            rig.analyzer.stopLearning();
            const auto introProfile = rig.profile();
            const auto generation = rig.analyzer.getHouseProfileGeneration();

            rig.feed (sliceOf (track, 8 * barSamples, (int) track.front().size() - 8 * barSamples), 512, 1, host);
            expect (sameHouseProfile (introProfile, rig.profile()), "not learning: the House profile is frozen");
            expect (rig.analyzer.getHouseProfileGeneration() == generation);
            expect (rig.analyzer.getSegmentLedger().getHeardSegments() >= 15, "but the ledger kept recording the drop");

            rig.analyzer.recomputeHouseProfile();
            const auto& regated = rig.profile();
            logMessage ("  intro-only House I " + juce::String (introProfile.integratedLufs.value, 2) + " -> regated "
                        + juce::String (regated.integratedLufs.value, 2) + " LUFS, full " + juce::String (regated.houseGate.fullSegments));
            expect (regated.integratedLufs.value > introProfile.integratedLufs.value + 4.0f, "regating picks the drop");
            expect (regated.houseGate.fullSegments >= 7 && regated.houseGate.fullSegments <= 8);

            rig.analyzer.startLearning();
            expectEquals (rig.analyzer.getSegmentLedger().getNumStored(), 0, "a relearn clears the ledger");
            expect (! rig.profile().houseGate.valid && ! rig.profile().integratedLufs.valid);
        }

        // 依据：MusicAnalyzer.h「音频线程不分配」。House 档：学习、三种不连续点、宿主 reset、停止学习、立即重算、清空段落账、换曲风，
        // 每块都不经 operator new。
        void testNoAllocation()
        {
            beginTest ("House: the audio-thread API does not allocate (ledger, gating, profile, genre switches)");

            constexpr double sampleRate = 48000.0;
            Rig rig (sampleRate);
            const auto input = makeHouseTrack (sampleRate, 4, 4, 0.5, 51);
            juce::AudioBuffer<float> block (numChannels, 4096);
            juce::Random random (3);
            std::uint64_t allocations = 0;
            float sink = 0.0f;

            for (int start = 0, index = 0; start < (int) input.front().size(); ++index)
            {
                const auto length = juce::jmin (1 + random.nextInt (4096), (int) input.front().size() - start);
                block.setSize (numChannels, length, false, false, true);

                for (int ch = 0; ch < numChannels; ++ch)
                    block.copyFrom (ch, 0, input[(size_t) ch].data() + start, length);

                const auto before = allocationCountOnThisThread();

                rig.analyzer.setHostInfo (index % 50 == 49 ? playingAt (start) : tempoHost (start, sampleRate));
                rig.analyzer.setGenre (index == 70 ? glaze::Genre::general : glaze::Genre::deepHouse);

                if (index == 0)
                    rig.analyzer.startLearning();

                if (index == 20)
                    rig.analyzer.handleDiscontinuity (Discontinuity::transportJump);

                if (index == 30)
                    rig.analyzer.handleDiscontinuity (Discontinuity::loopWrap);

                if (index == 40)
                {
                    rig.analyzer.handleDiscontinuity (Discontinuity::hostReset);
                    rig.meter.reset();
                    rig.balance.reset();
                }

                rig.meter.process (block);
                rig.analyzer.processInput (block, rig.meter);
                rig.balance.process (block);
                rig.analyzer.processSpectrum (rig.balance);
                sink += rig.analyzer.getCandidateProfile().integratedLufs.value + rig.analyzer.getCandidateProfile().houseGate.referenceLufs
                      + (float) rig.analyzer.getSegmentLedger().getCurrentSegmentSeconds()
                      + (rig.analyzer.getSegmentLedger().isCurrentSegmentFull() ? 1.0f : 0.0f);

                if (index == 60)
                {
                    rig.analyzer.stopLearning();
                    rig.analyzer.recomputeHouseProfile();
                }

                if (index == 80)
                    rig.analyzer.clearHouseLedger();

                allocations += allocationCountOnThisThread() - before;
                start += length;
            }

            expect (std::isfinite (sink));
            expectEquals ((int) allocations, 0);
        }

        // 性能护栏（记录为主）：House 档学习中每块 512 样本的分析器开销（与通用档同一种量法：多遍取每块最短再求均值 / 最坏），以及段落账
        // 满 512 段（全部满编、带先试后用的块）时一次 recomputeHouseProfile（门控之后的整份画像合并）的耗时——这是锁定后重开那一块的最坏情况。
        // 门槛只拦大退化：每块均值 < learningGuardMicroseconds（同通用档学习中的门槛，2026-10-01 起 75 µs，依据见文件头）、满账本一次重算 < 1 ms。
        void testPerformance()
        {
            beginTest ("performance guard: House mode per block (learning) and one recompute with a full 512-segment ledger");

            constexpr double sampleRate = 48000.0;
            constexpr int blockLength = 512;
            const auto input = makeHouseTrack (sampleRate, 2, 4, 0.0, 61);
            const auto numBlocks = (int) input.front().size() / blockLength;
            juce::AudioBuffer<float> block (numChannels, blockLength);

            std::vector<juce::int64> best ((size_t) numBlocks, std::numeric_limits<juce::int64>::max());

            for (int pass = 0; pass < 3; ++pass)
            {
                Rig rig (sampleRate);
                rig.analyzer.setGenre (glaze::Genre::deepHouse);
                rig.analyzer.startLearning();

                for (int b = 0; b < numBlocks; ++b)
                {
                    for (int ch = 0; ch < numChannels; ++ch)
                        block.copyFrom (ch, 0, input[(size_t) ch].data() + b * blockLength, blockLength);

                    rig.analyzer.setHostInfo (tempoHost ((std::int64_t) b * blockLength, sampleRate));
                    rig.meter.process (block);
                    const auto t0 = juce::Time::getHighResolutionTicks();
                    rig.analyzer.processInput (block, rig.meter);
                    const auto t1 = juce::Time::getHighResolutionTicks();
                    rig.balance.process (block);
                    const auto t2 = juce::Time::getHighResolutionTicks();
                    rig.analyzer.processSpectrum (rig.balance);
                    const auto t3 = juce::Time::getHighResolutionTicks();
                    best[(size_t) b] = juce::jmin (best[(size_t) b], (t1 - t0) + (t3 - t2));
                }
            }

            double sum = 0.0, worst = 0.0;

            for (auto ticks : best)
            {
                const auto seconds = juce::Time::highResolutionTicksToSeconds (ticks);
                sum += seconds;
                worst = juce::jmax (worst, seconds);
            }

            const auto mean = 1.0e6 * sum / numBlocks;

            // 满账本：把 2 秒一段的素材（不报拍点、带位置）喂满 512 段，学习一直开着（每段都有先试后用的块）。
            Rig full (sampleRate);
            full.analyzer.setGenre (glaze::Genre::techHouse);
            full.analyzer.startLearning();
            const auto loop = makeHouseTrack (sampleRate, 0, 8, 0.0, 62);

            for (int guard = 0; guard < 200 && full.analyzer.getSegmentLedger().getNumStored() < glaze::SegmentLedger::maxSegments; ++guard)
                full.feed (loop, 4096, 1, [] (std::int64_t fed, int) { return playingAt (fed); });

            juce::int64 bestRecompute = std::numeric_limits<juce::int64>::max();

            for (int k = 0; k < 10; ++k)
            {
                const auto t0 = juce::Time::getHighResolutionTicks();
                full.analyzer.recomputeHouseProfile();
                bestRecompute = juce::jmin (bestRecompute, juce::Time::getHighResolutionTicks() - t0);
            }

            const auto recomputeMicroseconds = 1.0e6 * juce::Time::highResolutionTicksToSeconds (bestRecompute);
            const auto& gate = full.profile().houseGate;
            logMessage ("  House analyzer @48k block 512, learning: mean " + juce::String (mean, 2) + " us, worst block "
                        + juce::String (1.0e6 * worst, 2) + " us (includes the per-bar gate / profile recompute); full ledger ("
                        + juce::String (full.analyzer.getSegmentLedger().getNumStored()) + " segments, " + juce::String (gate.fullSegments)
                        + " full, " + juce::String ((double) full.fed / sampleRate, 0) + " s fed): one recompute "
                        + juce::String (recomputeMicroseconds, 1) + " us");
            expect (mean < learningGuardMicroseconds, "House analyzer too slow while learning: " + juce::String (mean, 2) + " us per block");
            expect (gate.fullSegments > 400, "the full ledger is mostly full segments");
            expect (recomputeMicroseconds < 1000.0, "one recompute with a full ledger: " + juce::String (recomputeMicroseconds, 1) + " us");
        }
    };

    static MusicAnalyzerHouseTests musicAnalyzerHouseTests;
}

//==============================================================================
// ---- T-048 阶段 1：分析器实时模式（MusicAnalyzer.h 文件头「T-048」一节、MusicAnalyzer.cpp「七」节）----
// 只在文件末尾追加：上面的旧用例与工具一个不改，另起一个测试类（名字 MusicAnalyzerLive），复用上面的 Rig、信号、宿主与逐位比较。
// 链一级的对照（实时模式开 / 关时 CeladonChain 的输出逐位相同、直通在 prepare / reset / 读档之后保持）也在这里：它验的是分析器的
// 实时模式不影响编排器看到的任何东西。
#include "../Source/dsp/CeladonChain.h"

namespace celadon::test
{
    namespace
    {
        using glaze::SegmentLedger;

        // 收段的测试接收者：定稿的段原样存下（预先留好容量，音频线程上不分配；装满后只计数）。
        struct SegmentCollector final : glaze::SegmentSink
        {
            explicit SegmentCollector (size_t capacity = 512)   { segments.reserve (capacity); }

            void segmentFinalized (const SegmentLedger::Segment& segment) noexcept override
            {
                ++calls;

                if (segments.size() < segments.capacity())
                    segments.push_back (segment);
            }

            std::vector<SegmentLedger::Segment> segments;
            int calls = 0;
        };

        // 逐字节比（只给没有填充字节的标量与标量数组用：区分 ±0、NaN 的位型也算不同）。
        template <typename T>
        bool sameBytes (const T& a, const T& b)
        {
            return std::memcmp (&a, &b, sizeof (T)) == 0;
        }

        // 段的全部字段逐位相同（SegmentLedger.h 的 Segment，一项不漏）。
        bool sameSegmentAll (const SegmentLedger::Segment& a, const SegmentLedger::Segment& b)
        {
            return a.mode == b.mode && a.positioned == b.positioned && a.key == b.key && a.start == b.start && a.end == b.end
                && a.nominalLength == b.nominalLength && a.epoch == b.epoch
                && a.subBlocks == b.subBlocks && a.effectiveSubBlocks == b.effectiveSubBlocks && sameBytes (a.effectiveEnergy, b.effectiveEnergy)
                && sameBytes (a.truePeak, b.truePeak) && a.gatingCount == b.gatingCount && sameBytes (a.gatingPower, b.gatingPower)
                && a.rangeCount == b.rangeCount && sameBytes (a.rangeEnergy, b.rangeEnergy) && sameBytes (a.rangeBin, b.rangeBin)
                && a.frames == b.frames && sameBytes (a.frameSeconds, b.frameSeconds) && sameBytes (a.powerSum, b.powerSum)
                && sameBytes (a.tameSum, b.tameSum) && sameBytes (a.recoverSum, b.recoverSum) && sameBytes (a.deficitWeight, b.deficitWeight)
                && sameBytes (a.validWeight, b.validWeight) && sameBytes (a.stereoPowerSum, b.stereoPowerSum)
                && sameBytes (a.stereoCrossSum, b.stereoCrossSum) && sameBytes (a.stereoWeight, b.stereoWeight)
                && sameBytes (a.lowStereoWeight, b.lowStereoWeight) && sameBytes (a.highStereoWeight, b.highStereoWeight)
                && sameBytes (a.lowRangeCount, b.lowRangeCount) && a.lowRangeFirst == b.lowRangeFirst && a.lowRangeLast == b.lowRangeLast
                && a.fluxFrames == b.fluxFrames && a.musicFluxFrames == b.musicFluxFrames && a.fluxEvents == b.fluxEvents
                && sameBytes (a.fluxSum, b.fluxSum)
                && a.onsetEffectiveSamples == b.onsetEffectiveSamples && sameBytes (a.subEnergy, b.subEnergy) && sameBytes (a.midEnergy, b.midEnergy)
                && a.onsetEvents == b.onsetEvents && a.strengthCount == b.strengthCount && sameBytes (a.strengthBin, b.strengthBin)
                && a.trialBlocks == b.trialBlocks && sameBytes (a.trialTop, b.trialTop) && sameBytes (a.blendIntended, b.blendIntended)
                && sameBytes (a.blendOutput, b.blendOutput)
                && a.liveTrialBlocks == b.liveTrialBlocks && sameBytes (a.liveTrialTop, b.liveTrialTop)
                && sameBytes (a.liveBlendIntended, b.liveBlendIntended) && sameBytes (a.liveBlendOutput, b.liveBlendOutput)
                && sameBytes (a.effectiveSeconds, b.effectiveSeconds) && a.heard == b.heard && a.loudnessValid == b.loudnessValid
                && sameBytes (a.loudnessLufs, b.loudnessLufs) && a.lowShareValid == b.lowShareValid && sameBytes (a.lowShare, b.lowShare)
                && a.full == b.full;
        }

        bool sameSegments (const std::vector<SegmentLedger::Segment>& a, const std::vector<SegmentLedger::Segment>& b)
        {
            if (a.size() != b.size())
                return false;

            for (size_t i = 0; i < a.size(); ++i)
                if (! sameSegmentAll (a[i], b[i]))
                    return false;

            return true;
        }

        // 学习期专用的三项与实时的三项逐位相同（同一批块）。
        bool liveEqualsTrial (const SegmentLedger::Segment& s)
        {
            return s.trialBlocks == s.liveTrialBlocks && sameBytes (s.trialTop, s.liveTrialTop)
                && sameBytes (s.blendIntended, s.liveBlendIntended) && sameBytes (s.blendOutput, s.liveBlendOutput);
        }

        bool sameEvent (const SegmentLedger::Event& a, const SegmentLedger::Event& b)
        {
            return a.sequence == b.sequence && a.epoch == b.epoch && a.stored == b.stored && a.heard == b.heard && a.full == b.full
                && a.surge == b.surge && a.loudnessValid == b.loudnessValid && sameFloat (a.loudnessLufs, b.loudnessLufs)
                && a.lowShareValid == b.lowShareValid && sameFloat (a.lowShare, b.lowShare);
        }

        // House 档那一份段落账此刻能读到的一切（编排器、画像与状态读的全部读数）逐位相同。
        bool sameLedgerState (const SegmentLedger& a, const SegmentLedger& b)
        {
            if (a.getNumStored() != b.getNumStored() || a.isStoreFull() != b.isStoreFull() || a.getMode() != b.getMode()
                || a.getHeardSegments() != b.getHeardSegments() || a.getEpoch() != b.getEpoch() || a.getSurgeCount() != b.getSurgeCount()
                || a.getFinalizedCount() != b.getFinalizedCount() || a.getStoredRevision() != b.getStoredRevision()
                || a.getOpenSegments() != b.getOpenSegments() || a.getEventCount() != b.getEventCount()
                || a.getAcceptFrom() != b.getAcceptFrom() || a.isCurrentSegmentFull() != b.isCurrentSegmentFull()
                || ! sameDouble (a.getCurrentSegmentSeconds(), b.getCurrentSegmentSeconds()) || ! sameHouseGate (a.getSummary(), b.getSummary()))
                return false;

            float loudnessA = -1.0f, lowA = -1.0f, loudnessB = -1.0f, lowB = -1.0f;

            if (a.getCurrentReading (loudnessA, lowA) != b.getCurrentReading (loudnessB, lowB)
                || ! sameFloat (loudnessA, loudnessB) || ! sameFloat (lowA, lowB))
                return false;

            for (int i = 0; i < a.getNumStored(); ++i)
                if (! sameSegmentAll (a.getStored (i), b.getStored (i)))
                    return false;

            std::array<SegmentLedger::Event, (size_t) SegmentLedger::eventCapacity> eventsA {}, eventsB {};
            std::uint64_t cursorA = 0, cursorB = 0;
            const auto gotA = a.readEvents (cursorA, eventsA.data(), (int) eventsA.size());
            const auto gotB = b.readEvents (cursorB, eventsB.data(), (int) eventsB.size());

            if (gotA != gotB || cursorA != cursorB)
                return false;

            for (int i = 0; i < std::abs (gotA); ++i)
                if (! sameEvent (eventsA[(size_t) i], eventsB[(size_t) i]))
                    return false;

            return true;
        }

        bool sameShortTermView (const ShortTermView& a, const ShortTermView& b)
        {
            return sameFeature (a.momentaryLufs, b.momentaryLufs) && sameFeature (a.shortTermLufs, b.shortTermLufs)
                && sameFeature (a.psr3sDb, b.psr3sDb) && sameFeature (a.liveTameEvidenceDb, b.liveTameEvidenceDb)
                && sameFeature (a.liveSpectralSlopeDbPerOct, b.liveSpectralSlopeDbPerOct) && sameArray (a.liveSlopeWeights, b.liveSlopeWeights)
                && sameLiveStereo (a, b) && a.musicActive == b.musicActive;
        }

        // 分析器此刻能被编排器读到的一切逐位相同：候选画像（含修坯字段与门控结果）、辅助量、短期视图、House 档的段落账、画像版本号。
        juce::String differenceBetween (const MusicAnalyzer& a, const MusicAnalyzer& b)
        {
            if (! sameHouseProfile (a.getCandidateProfile(), b.getCandidateProfile()))
                return "candidate profile";

            if (! sameAuxiliary (a.getAuxiliaryFeatures(), b.getAuxiliaryFeatures()))
                return "auxiliary features";

            if (! sameShortTermView (a.getShortTermView(), b.getShortTermView()))
                return "short-term view";

            if (! sameLedgerState (a.getSegmentLedger(), b.getSegmentLedger()))
                return "House ledger";

            if (a.getHouseProfileGeneration() != b.getHouseProfileGeneration() || a.isLearning() != b.isLearning() || a.getGenre() != b.getGenre())
                return "generation / learning / genre";

            return {};
        }

        // 脚本的一步：在起点 ≥ at 的第一块之前对分析器做一件事（开始 / 停止学习、换曲风、不连续点……）。
        struct Step
        {
            std::int64_t at = 0;
            std::function<void (MusicAnalyzer&)> action;
        };

        // 宿主走带的分段线性映射：分析器时间 t → 宿主播放位置。每一项 = (从分析器时间起, 那一刻的宿主位置)，按时间先后。
        struct Transport
        {
            std::vector<std::pair<std::int64_t, std::int64_t>> pieces { { 0, 0 } };

            std::int64_t positionAt (std::int64_t t) const
            {
                auto position = t;

                for (const auto& [from, start] : pieces)
                    if (t >= from)
                        position = start + (t - from);

                return position;
            }
        };

        // 按脚本逐块喂一份或两份分析器（每块先做到期的步骤、交宿主信息，再处理；两份时每块之后比较）。返回第一处不同的块号（−1 = 处处相同），
        // what 写不同的是哪一项。
        int runScript (std::initializer_list<Rig*> rigs, const Signal& input, int blockSize,
                       const std::function<HostInfo (std::int64_t)>& hostAt, const std::vector<Step>& steps, juce::String& what)
        {
            juce::AudioBuffer<float> block (numChannels, blockSize);
            const auto numSamples = (int) input.front().size();
            size_t next = 0;
            auto index = 0;

            for (int start = 0; start < numSamples; start += blockSize, ++index)
            {
                const auto length = juce::jmin (blockSize, numSamples - start);

                for (; next < steps.size() && steps[next].at <= (std::int64_t) start; ++next)
                    for (auto* rig : rigs)
                        steps[next].action (rig->analyzer);

                for (auto* rig : rigs)
                {
                    block.setSize (numChannels, length, false, false, true);

                    for (int ch = 0; ch < numChannels; ++ch)
                        block.copyFrom (ch, 0, input[(size_t) juce::jmin (ch, (int) input.size() - 1)].data() + start, length);

                    rig->analyzer.setHostInfo (hostAt (start));
                    rig->processBlock (block);   // Balance 原地处理这一块：每份分析器各拷一份输入
                }

                if (rigs.size() == 2)
                {
                    what = differenceBetween ((*rigs.begin())->analyzer, (*(rigs.begin() + 1))->analyzer);

                    if (what.isNotEmpty())
                        return index;
                }
            }

            return -1;
        }

        std::int64_t samplesAt (double seconds)   { return (std::int64_t) std::llround (seconds * 48000.0); }

        Signal sliceTo (const Signal& signal, std::int64_t length)
        {
            return sliceOf (signal, 0, (int) juce::jmin ((std::int64_t) signal.front().size(), length));
        }

        // 链一级的对照用：先交参数再 prepare（与处理器的 prepareToPlay 同一顺序），每块报恒定 124 BPM 的拍点与位置。
        struct ChainRun
        {
            ChainRun (const glaze::Constants& constants, const ChainParams& p, glaze::SegmentSink* sink) : params (p)
            {
                chain->getGlazeController().setConstantsForTesting (constants);
                chain->setParams (params, false);
                chain->prepare ({ 48000.0, (juce::uint32) preparedBlock, (juce::uint32) numChannels });

                if (sink != nullptr)
                    chain->setLiveSegmentSink (sink);
            }

            void step (const Signal& input, std::int64_t time, std::int64_t position)
            {
                const auto length = (std::int64_t) input.front().size();
                buffer.setSize (numChannels, preparedBlock, false, false, true);

                for (int ch = 0; ch < numChannels; ++ch)
                    for (int i = 0; i < preparedBlock; ++i)
                        buffer.setSample (ch, i, input[(size_t) ch][(size_t) ((time + i) % length)]);

                chain->setHostInfo (tempoHost (position, 48000.0));
                chain->setParams (params, false);
                chain->process (buffer);
            }

            std::unique_ptr<CeladonChain> chain = std::make_unique<CeladonChain>();
            ChainParams params;
            juce::AudioBuffer<float> buffer { numChannels, preparedBlock };
        };

        bool sameBuffers (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
        {
            if (a.getNumChannels() != b.getNumChannels() || a.getNumSamples() != b.getNumSamples())
                return false;

            for (int ch = 0; ch < a.getNumChannels(); ++ch)
                if (std::memcmp (a.getReadPointer (ch), b.getReadPointer (ch), sizeof (float) * (size_t) a.getNumSamples()) != 0)
                    return false;

            return true;
        }

        // 聆听 2 ～ 3 s（通用档很快提交）、House 锁定条件缩到 8 小节 / 满编 3 小节，过渡与校准都缩短：60 多秒里走得完两轮聆听。
        glaze::Constants shortListenConstants()
        {
            auto c = glaze::factoryConstants();
            c.minListenSeconds = 2.0;
            c.maxListenSeconds = 3.0;
            c.planTransitionSeconds = 0.5;
            c.calibrationSettleSeconds = 0.5;
            c.calibrationWindowSeconds = 2.0;
            c.house.lockSegments = 8;
            c.house.requiredFullSegments = 3;
            return c;
        }
    }

    class MusicAnalyzerLiveTests  : public juce::UnitTest
    {
    public:
        MusicAnalyzerLiveTests() : juce::UnitTest ("MusicAnalyzerLive", "Celadon") {}

        void runTest() override
        {
            testOffEqualsOnAnalyzer();
            testOffEqualsOnChain();
            testSegmentsContiguous();
            testBlockSizeIndependence();
            testLiveTrialData();
            testNoAllocation();
            testChainPassThroughKeepsSetting();
            testPerformance();

            // T-048 阶段 1 修订（代理 B2；任务书 5 a ～ f）
            testLearningStopsOnRealBlockBoundaries();
            testChainLearningStopsOnRealBlockBoundaries();
            testLearningStateIndependence();
            testChainSecondPassMatchesFirst();
            testAdvanceToTimeline();
            testTurningOffKeepsLastFrames();
            testStartFromBarN();

            // 第「七」节 ⑥（Max 2026-10-03「持续偏轻就加快遗忘」）
            testQuietProgramReleasesGates();
        }

    private:
        //==============================================================================
        // 依据：任务书「实时模式关着时插件一切输出不变；关：sink 一次都不被调；开 vs 关，候选画像（通用档与 House 档）逐位相同」、
        // MusicAnalyzer.h「T-048」一节。两份分析器（关：交了接收者但没开实时模式；开）按同一份脚本逐块喂 65 s 合成 House 曲，**每一块之后**
        // 比较编排器能读到的一切（候选画像全部字段与门控结果、辅助量、短期视图、House 档那一份段落账的全部读数与每一段的全部字段、事件、
        // 计数）。三个脚本：House 档里学习 / 停止 / 立即重算 / 重新聆听 / 停止 / 清空段落账；通用档里学习 / 停止 / 重新聆听；混着来——
        // 通用 → House → 通用 → 另一个 House 子类，中间开始 / 停止学习、跳播与回绕（House 那一份段落账跨换曲风留着，实时模式不许动它）。
        void testOffEqualsOnAnalyzer()
        {
            beginTest ("live mode off vs on: everything the controller reads from the analyzer is bit-identical, block by block (65 s, three scripts)");

            const auto track = makeHouseTrack (48000.0, 4, 30, 0.0, 71);   // 34 小节 ≈ 65.8 s
            using G = glaze::Genre;
            const auto learn = [] (MusicAnalyzer& a) { a.startLearning(); };
            const auto stop = [] (MusicAnalyzer& a) { a.stopLearning(); };
            const auto genre = [] (G g) { return [g] (MusicAnalyzer& a) { a.setGenre (g); }; };

            struct Script
            {
                const char* name;
                Transport transport;
                std::vector<Step> steps;
            };

            Transport jumping;
            jumping.pieces = { { 0, 0 }, { samplesAt (24.0), samplesAt (29.0) }, { samplesAt (60.0), samplesAt (10.0) } };

            const std::vector<Script> scripts {
                { "House: learn, stop, recompute, relearn, stop, clear", Transport {},
                  { { 0, genre (G::deepHouse) }, { 0, learn }, { samplesAt (20.0), stop },
                    { samplesAt (22.0), [] (MusicAnalyzer& a) { a.recomputeHouseProfile(); } },
                    { samplesAt (40.0), learn }, { samplesAt (56.0), stop },
                    { samplesAt (60.0), [] (MusicAnalyzer& a) { a.clearHouseLedger(); } } } },
                { "general: learn, stop, relearn", Transport {},
                  { { 0, learn }, { samplesAt (18.0), stop }, { samplesAt (30.0), learn }, { samplesAt (50.0), stop } } },
                { "mixed: genre switches, learning, a jump and a loop wrap", jumping,
                  { { 0, learn }, { samplesAt (8.0), genre (G::deepHouse) }, { samplesAt (16.0), stop },
                    { samplesAt (24.0), [] (MusicAnalyzer& a) { a.handleDiscontinuity (Discontinuity::transportJump); } },
                    { samplesAt (30.0), genre (G::general) }, { samplesAt (34.0), learn }, { samplesAt (42.0), genre (G::techHouse) },
                    { samplesAt (50.0), stop }, { samplesAt (52.0), [] (MusicAnalyzer& a) { a.recomputeHouseProfile(); } },
                    { samplesAt (56.0), genre (G::general) },
                    { samplesAt (60.0), [] (MusicAnalyzer& a) { a.handleDiscontinuity (Discontinuity::loopWrap); } } } },
            };

            for (const auto& script : scripts)
            {
                SegmentCollector offCollector, onCollector;   // 先于分析器声明：分析器先析构（它持有接收者的指针）
                Rig off (48000.0), on (48000.0);
                off.analyzer.setSegmentSink (&offCollector);   // 交了接收者、没开实时模式
                on.analyzer.setSegmentSink (&onCollector);
                on.analyzer.setLiveMode (true);

                const auto& transport = script.transport;
                juce::String what;
                const auto firstDifference = runScript ({ &off, &on }, track, 512,
                                                        [&transport] (std::int64_t t) { return tempoHost (transport.positionAt (t), 48000.0); },
                                                        script.steps, what);

                logMessage ("  " + juce::String (script.name) + ": " + juce::String (onCollector.calls) + " live segments, House ledger "
                            + juce::String (on.analyzer.getSegmentLedger().getNumStored()) + " stored, "
                            + juce::String (on.analyzer.getSegmentLedger().getFinalizedCount()) + " finalized");
                expect (firstDifference < 0, juce::String (script.name) + ": first difference at block " + juce::String (firstDifference) + " (" + what + ")");
                expectEquals (offCollector.calls, 0, juce::String (script.name) + ": live mode off never calls the sink");
                expect (onCollector.calls >= 25, juce::String (script.name) + ": live mode on delivered the bars (positive control)");
            }
        }

        //==============================================================================
        // 依据：任务书「实时模式开 vs 关，CeladonChain 的输出逐位相同（House 档跑过学习 / 停止 / 重新聆听；时长 ≥ 60 s）」、
        // GlazeController::setLiveSegmentSink「编排器其余行为一律不变」。两条链（一条经 setLiveSegmentSink 开实时模式）按同一宿主走 69 s：
        // 首次聆听 → 提交 → 过渡 → 校准，30 s 处「重新聆听」，50 s 处跳播 3 s；每一块的输出逐位相同，编排器的计数也相同。通用档（GLAZE 100）
        // 与 Deep House（GLAZE 70）各一遍。
        void testOffEqualsOnChain()
        {
            beginTest ("live mode off vs on: CeladonChain output is bit-identical (general and Deep House, learn / commit / relearn / jump, 69 s)");

            const auto track = makeHouseTrack (48000.0, 4, 32, 0.0, 72);   // 36 小节 ≈ 69.7 s
            const auto blocks = (int) (track.front().size() / (size_t) preparedBlock);

            for (const auto genre : { 0, 1 })
            {
                ChainParams params;
                params.limit.ceilingDb = -1.0f;
                params.glazePercent = genre == 0 ? 100.0f : 70.0f;
                params.glazeGenre = genre;

                SegmentCollector collector;
                ChainRun off (shortListenConstants(), params, nullptr), on (shortListenConstants(), params, &collector);
                auto firstDifference = -1;
                std::int64_t position = 0;

                for (int b = 0; b < blocks; ++b)
                {
                    const auto time = (std::int64_t) b * preparedBlock;

                    if (time == samplesAt (30.0) - samplesAt (30.0) % preparedBlock)
                    {
                        off.chain->requestGlazeRelearn();
                        on.chain->requestGlazeRelearn();
                    }

                    if (time == samplesAt (50.0) - samplesAt (50.0) % preparedBlock)
                        position += samplesAt (3.0);   // 跳播：编排器自己从宿主位置认出来

                    off.step (track, time, position);
                    on.step (track, time, position);
                    position += preparedBlock;

                    if (firstDifference < 0 && ! sameBuffers (off.buffer, on.buffer))
                        firstDifference = b;
                }

                const auto probeOff = off.chain->getGlazeController().getProbeForTesting();
                const auto probeOn = on.chain->getGlazeController().getProbeForTesting();
                const auto tag = juce::String (genre == 0 ? "general" : "Deep House");
                logMessage ("  " + tag + ": " + juce::String (probeOn.learningStarts) + " learning starts, " + juce::String (probeOn.commits)
                            + " commits, " + juce::String (probeOn.transportJumps) + " jumps, " + juce::String (collector.calls) + " live segments");
                expect (firstDifference < 0, tag + ": first different output block " + juce::String (firstDifference));
                expect (probeOff.commits == probeOn.commits && probeOff.learningStarts == probeOn.learningStarts
                            && probeOff.planTransitions == probeOn.planTransitions && probeOff.calibrationBegins == probeOn.calibrationBegins
                            && probeOff.houseUpdates == probeOn.houseUpdates && probeOff.houseReopens == probeOn.houseReopens,
                        tag + ": the controller took the same decisions");
                expect (probeOn.learningStarts >= 2 && probeOn.commits >= 2 && probeOn.transportJumps >= 1,
                        tag + ": first listen, relearn and the jump all happened (positive control)");
                expect (collector.calls >= 30, tag + ": the live ledger delivered segments (positive control)");
            }
        }

        //==============================================================================
        // 依据：任务书「实时模式开：sink 收到的段按时间先后、每段恰好一次、[start, end) 首尾相接不漏（跨学习开始 / 停止 / 重新聆听、换曲风、
        // 跳播与回绕）；通用档与 House 档都覆盖」、MusicAnalyzer.cpp「七」节「只在不连续点断开」。48 s：6 s 开始学习、12 s 换 Deep House、
        // 18 s 停止、24 s 跳播（宿主位置 +5 s）、30 s 重新聆听、36 s 回绕（宿主位置回到 10 s）、42 s 换回通用、46 s 清空 House 段落账，
        // 48 s 处 reset（开着的段先交出去）。期望：第一段从 0 起、最后一段止于 48 s、相邻两段首尾相接（同一条时间轴）；不完整的段只出现在
        // 跳播 / 回绕 / 结尾三处的两侧，其余段都是整段（学习、换曲风、清空都不断开）；跳播与回绕之后段的连续时间段号变大。
        // 宿主两种：恒定 124 BPM 的拍点（按小节）、只有位置（按位置的 2 秒格）。
        void testSegmentsContiguous()
        {
            beginTest ("live mode: segments arrive in time order, once each, end-to-end contiguous across learning, genre switches, jumps and loops");

            const auto jumpAt = samplesAt (24.0), loopAt = samplesAt (36.0), endAt = samplesAt (48.0);
            const auto track = sliceTo (makeHouseTrack (48000.0, 4, 22, 0.0, 73), endAt);
            using G = glaze::Genre;

            Transport transport;
            transport.pieces = { { 0, 0 }, { jumpAt, jumpAt + samplesAt (5.0) }, { loopAt, samplesAt (10.0) } };

            const std::vector<Step> steps {
                { samplesAt (6.0), [] (MusicAnalyzer& a) { a.startLearning(); } },
                { samplesAt (12.0), [] (MusicAnalyzer& a) { a.setGenre (G::deepHouse); } },
                { samplesAt (18.0), [] (MusicAnalyzer& a) { a.stopLearning(); } },
                { jumpAt, [] (MusicAnalyzer& a) { a.handleDiscontinuity (Discontinuity::transportJump); } },
                { samplesAt (30.0), [] (MusicAnalyzer& a) { a.startLearning(); } },
                { loopAt, [] (MusicAnalyzer& a) { a.handleDiscontinuity (Discontinuity::loopWrap); } },
                { samplesAt (42.0), [] (MusicAnalyzer& a) { a.setGenre (G::general); } },
                { samplesAt (46.0), [] (MusicAnalyzer& a) { a.clearHouseLedger(); } },
            };

            for (const auto bars : { true, false })
            {
                SegmentCollector collector;
                Rig rig (48000.0);
                rig.analyzer.setSegmentSink (&collector);
                rig.analyzer.setLiveMode (true);
                juce::String unused;
                runScript ({ &rig }, track, 512, [&transport, bars] (std::int64_t t)
                {
                    const auto position = transport.positionAt (t);
                    return bars ? tempoHost (position, 48000.0) : playingAt (position);
                }, steps, unused);
                rig.analyzer.reset();   // 开着的段先就地定稿交出去

                const auto& s = collector.segments;
                const auto tag = juce::String (bars ? "bars" : "2-second cells");
                auto contiguous = ! s.empty() && s.front().start == 0 && s.back().end == endAt;
                auto partialOnlyAtCuts = true, modes = true, epochs = true;

                for (size_t i = 0; i < s.size(); ++i)
                {
                    contiguous = contiguous && s[i].start < s[i].end && (i + 1 == s.size() || s[i].end == s[i + 1].start);
                    const auto whole = s[i].end - s[i].start == s[i].nominalLength;
                    const auto atCut = s[i].start == jumpAt || s[i].start == loopAt || s[i].end == jumpAt || s[i].end == loopAt || s[i].end == endAt;
                    partialOnlyAtCuts = partialOnlyAtCuts && (whole || atCut);
                    modes = modes && s[i].mode == (bars ? glaze::SegmentMode::bars : glaze::SegmentMode::seconds) && s[i].positioned;

                    if (i > 0)
                        epochs = epochs && (s[i].start == jumpAt || s[i].start == loopAt ? s[i].epoch > s[i - 1].epoch : s[i].epoch == s[i - 1].epoch);
                }

                logMessage ("  " + tag + ": " + juce::String ((int) s.size()) + " segments over 48 s");
                expect (collector.calls == (int) s.size() && s.size() >= 20, tag + ": every call stored (no overflow)");
                expect (contiguous, tag + ": first starts at 0, last ends at 48 s, each one starts where the previous ended");
                expect (partialOnlyAtCuts, tag + ": learning, genre switches and clearing never cut a segment; only the jump, the loop and the end do");
                expect (modes, tag + ": segment mode follows the host");
                expect (epochs, tag + ": the continuous-time index moves on exactly at the jump and the loop");
            }
        }

        //==============================================================================
        // 依据：任务书「交给 sink 的段序列与宿主切块无关：块长 7、32、480、512、4096 逐位相同（逐字段比 Segment）」。事件都放在
        // 430080 样本（= 这五个块长的最小公倍数，8.96 s）的整数倍上，各种块长下发生在同一个样本：通用档起步，1 × 开始学习、2 × 换 Deep House、
        // 3 × 停止学习、4 × 跳播（宿主位置 +777777）、5 × 重新聆听、6 × 处 reset（交出开着的段）。
        void testBlockSizeIndependence()
        {
            beginTest ("live mode: the delivered segments are bit-identical for host blocks of 7, 32, 480, 512 and 4096 samples");

            constexpr std::int64_t unit = 430080;
            const auto track = sliceTo (makeHouseTrack (48000.0, 6, 22, 0.0, 74), 6 * unit);
            Transport transport;
            transport.pieces = { { 0, 0 }, { 4 * unit, 4 * unit + 777777 } };

            const std::vector<Step> steps {
                { 1 * unit, [] (MusicAnalyzer& a) { a.startLearning(); } },
                { 2 * unit, [] (MusicAnalyzer& a) { a.setGenre (glaze::Genre::deepHouse); } },
                { 3 * unit, [] (MusicAnalyzer& a) { a.stopLearning(); } },
                { 4 * unit, [] (MusicAnalyzer& a) { a.handleDiscontinuity (Discontinuity::transportJump); } },
                { 5 * unit, [] (MusicAnalyzer& a) { a.startLearning(); } },
            };

            const auto run = [&] (int blockSize)
            {
                auto rig = std::make_unique<Rig> (48000.0);
                auto collector = std::make_unique<SegmentCollector>();
                rig->analyzer.setSegmentSink (collector.get());
                rig->analyzer.setLiveMode (true);
                juce::String unused;
                runScript ({ rig.get() }, track, blockSize,
                           [&transport] (std::int64_t t) { return tempoHost (transport.positionAt (t), 48000.0); }, steps, unused);
                rig->analyzer.reset();
                return collector;
            };

            const auto reference = run (512);
            auto withTrials = 0;

            for (const auto& segment : reference->segments)
                withTrials += segment.liveTrialBlocks > 0 && segment.trialBlocks > 0 ? 1 : 0;

            expect (reference->segments.size() >= 25 && withTrials >= 5, "the run delivers bars, some with learning-period trial blocks");

            for (const auto blockSize : { 7, 32, 480, 4096 })
            {
                const auto other = run (blockSize);
                expect (sameSegments (reference->segments, other->segments), "block " + juce::String (blockSize) + " vs 512: every field of every segment");
            }
        }

        //==============================================================================
        // 依据：任务书「liveTrial* 在不学习时也有数据；学习期间实时模式关着时 live* 与 trial* 逐位相同」、MusicAnalyzer.cpp「五」「七」节。
        // 稳定的素材（粉噪 + 鼓，一直在有效门上），124 BPM 拍点，通用档：
        //   - 实时模式开、前 19 s 不学习：每一段的 liveTrialBlocks = 起点落在这一段里、且晚于开头 250 ms 预热的块数（48 kHz：块起点是 4800 的
        //     整数倍、第 3 块起有效）——每个有效块都记进了段；trialBlocks = 0；
        //   - 20 ～ 40 s 学习：学习区间里的段 trial* 与 live* 逐位相同；停止之后 live* 照记、trial* 不记；
        //   - 实时模式关、House 档学习 30 s：House 段落账里每一段的 live* 与 trial* 逐位相同，且确实有块（阳性对照）。
        void testLiveTrialData()
        {
            beginTest ("live trial data: every valid block lands in its segment when not learning; live* == trial* while learning (on and off)");

            constexpr double sampleRate = 48000.0;
            auto input = makePinkNoise (numChannels, (int) (50.0 * sampleRate), -24.0f, 75);
            addInto (input, makeDrumPattern (sampleRate, 50.0, 76));
            const auto learnAt = samplesAt (20.0), stopAt = samplesAt (40.0);

            {
                SegmentCollector collector;
                Rig rig (sampleRate);
                rig.analyzer.setSegmentSink (&collector);
                rig.analyzer.setLiveMode (true);
                juce::String unused;
                runScript ({ &rig }, input, 512, [] (std::int64_t t) { return tempoHost (t, 48000.0); },
                           { { learnAt, [] (MusicAnalyzer& a) { a.startLearning(); } }, { stopAt, [] (MusicAnalyzer& a) { a.stopLearning(); } } },
                           unused);

                auto idleExact = true, learningSame = true, afterStop = true;
                int idleSegments = 0, learningSegments = 0, afterSegments = 0;

                for (const auto& s : collector.segments)
                {
                    if (s.end <= samplesAt (19.0))
                    {
                        // 起点落在 [start, end) 里、且不早于第 3 块（14400）的块数。
                        const auto first = juce::jmax ((std::int64_t) 3, (s.start + 4799) / 4800);
                        const auto last = (s.end + 4799) / 4800;   // 不含
                        idleExact = idleExact && s.liveTrialBlocks == (int) juce::jmax ((std::int64_t) 0, last - first) && s.trialBlocks == 0;
                        ++idleSegments;
                    }
                    else if (s.start >= learnAt && s.end <= stopAt - 6000)
                    {
                        learningSame = learningSame && liveEqualsTrial (s) && s.trialBlocks > 0;
                        ++learningSegments;
                    }
                    else if (s.start >= stopAt + 6000)
                    {
                        afterStop = afterStop && s.trialBlocks == 0 && s.liveTrialBlocks >= 18;
                        ++afterSegments;
                    }
                }

                logMessage ("  live on: " + juce::String (idleSegments) + " idle bars, " + juce::String (learningSegments) + " learning bars, "
                            + juce::String (afterSegments) + " bars after the stop");
                expect (idleSegments >= 8 && idleExact, "not learning: liveTrialBlocks = the valid blocks starting in the bar; trialBlocks = 0");
                expect (learningSegments >= 8 && learningSame, "learning: live* == trial* bit for bit");
                expect (afterSegments >= 3 && afterStop, "after stopLearning the live trial keeps recording, the learning one does not");
            }

            {
                Rig rig (sampleRate);
                rig.analyzer.setGenre (glaze::Genre::deepHouse);
                rig.analyzer.startLearning();
                rig.feed (sliceOf (input, 0, (int) samplesAt (30.0)), 512, 1, [] (std::int64_t fed, int) { return tempoHost (fed, 48000.0); });

                const auto& ledger = rig.analyzer.getSegmentLedger();
                auto same = ledger.getNumStored() >= 12, someBlocks = false;

                for (int i = 0; i < ledger.getNumStored(); ++i)
                {
                    same = same && liveEqualsTrial (ledger.getStored (i));
                    someBlocks = someBlocks || ledger.getStored (i).liveTrialBlocks > 0;
                }

                expect (same && someBlocks, "live mode off, House learning: live* == trial* in every stored segment, with blocks");
            }
        }

        //==============================================================================
        // 依据：任务书「实时模式开着时 processInput / processSpectrum 不分配」、MusicAnalyzer.h「音频线程不分配」。实时模式开、随机块长：
        // 学习 / 停止 / 重新聆听、通用与 House 来回换、三种不连续点（含表与 Balance 的 reset）、立即重算、清空段落账、分析器 reset，
        // 每块都不经 operator new（接收者把段存进预先留好容量的 vector）。
        void testNoAllocation()
        {
            beginTest ("live mode: the audio-thread API does not allocate (both ledgers, sink delivery, genre switches, discontinuities)");

            constexpr double sampleRate = 48000.0;
            SegmentCollector collector (256);
            Rig rig (sampleRate);
            rig.analyzer.setSegmentSink (&collector);
            rig.analyzer.setLiveMode (true);
            const auto input = makeHouseTrack (sampleRate, 4, 8, 0.5, 77);
            juce::AudioBuffer<float> block (numChannels, 4096);
            juce::Random random (9);
            std::uint64_t allocations = 0;
            float sink = 0.0f;

            for (int start = 0, index = 0; start < (int) input.front().size(); ++index)
            {
                const auto length = juce::jmin (1 + random.nextInt (4096), (int) input.front().size() - start);
                block.setSize (numChannels, length, false, false, true);

                for (int ch = 0; ch < numChannels; ++ch)
                    block.copyFrom (ch, 0, input[(size_t) ch].data() + start, length);

                const auto before = allocationCountOnThisThread();

                rig.analyzer.setHostInfo (index % 40 == 39 ? playingAt (start) : tempoHost (start, sampleRate));
                rig.analyzer.setGenre (index >= 25 && index < 45 ? glaze::Genre::deepHouse
                                       : index >= 70 ? glaze::Genre::techHouse : glaze::Genre::general);

                if (index == 0 || index == 50)
                    rig.analyzer.startLearning();

                if (index == 20)
                    rig.analyzer.handleDiscontinuity (Discontinuity::transportJump);

                if (index == 30)
                    rig.analyzer.handleDiscontinuity (Discontinuity::loopWrap);

                if (index == 40)
                {
                    rig.analyzer.handleDiscontinuity (Discontinuity::hostReset);
                    rig.meter.reset();
                    rig.balance.reset();
                }

                if (index == 85)
                    rig.analyzer.reset();

                rig.meter.process (block);
                rig.analyzer.processInput (block, rig.meter);
                rig.balance.process (block);
                rig.analyzer.processSpectrum (rig.balance);
                sink += rig.analyzer.getCandidateProfile().integratedLufs.value + rig.analyzer.getShortTermView().momentaryLufs.value;

                if (index == 35 || index == 60)
                {
                    rig.analyzer.stopLearning();
                    rig.analyzer.recomputeHouseProfile();
                }

                if (index == 75)
                    rig.analyzer.clearHouseLedger();

                allocations += allocationCountOnThisThread() - before;
                start += length;
            }

            expect (std::isfinite (sink));
            expect (collector.calls > 5, "segments were delivered (positive control)");
            expectEquals ((int) allocations, 0);
        }

        //==============================================================================
        // 依据：GlazeController::setLiveSegmentSink「prepare / 宿主 reset / 读档之后保持设定；nullptr = 关（开着的段先交出去）」。一条链开着
        // 实时模式走 8 s，之后依次 reset（链的 restartWithFadeIn 之外的那种：编排器按宿主 reset 处理）、同规格 prepare、读回自己的存档，每次之后
        // 再走 6 s：段照样一直交来；setLiveSegmentSink (nullptr) 时把开着的段交出去，之后一段都不再交。
        void testChainPassThroughKeepsSetting()
        {
            beginTest ("chain pass-through: the live sink survives reset, prepare and a state restore; nullptr turns it off");

            ChainParams params;
            params.limit.ceilingDb = -1.0f;
            params.glazePercent = 50.0f;
            params.glazeGenre = 1;
            SegmentCollector collector;
            ChainRun run (shortListenConstants(), params, &collector);
            const auto track = makeHouseTrack (48000.0, 4, 12, 0.0, 78);
            std::int64_t time = 0;

            const auto play = [&] (double seconds)
            {
                for (int b = 0; b < (int) (seconds * 48000.0) / preparedBlock; ++b, time += preparedBlock)
                    run.step (track, time, time);
            };

            play (8.0);
            const auto afterStart = collector.calls;
            run.chain->reset();
            play (6.0);
            const auto afterReset = collector.calls;
            run.chain->setParams (params, false);
            run.chain->prepare ({ 48000.0, (juce::uint32) preparedBlock, (juce::uint32) numChannels });
            play (6.0);
            const auto afterPrepare = collector.calls;
            const auto state = run.chain->createGlazeStateXml();
            run.chain->restoreGlazeStateXml (state.get());
            play (6.0);
            const auto afterRestore = collector.calls;
            run.chain->setLiveSegmentSink (nullptr);
            const auto afterOff = collector.calls;
            play (6.0);

            logMessage ("  segments: " + juce::String (afterStart) + " after 8 s, " + juce::String (afterReset) + " after reset, "
                        + juce::String (afterPrepare) + " after prepare, " + juce::String (afterRestore) + " after restore, "
                        + juce::String (afterOff) + " after turning it off");
            expect (afterStart >= 3, "segments arrive while playing");
            expect (afterReset >= afterStart + 2, "still on after reset");
            expect (afterPrepare >= afterReset + 2, "still on after prepare");
            expect (afterRestore >= afterPrepare + 2, "still on after a state restore");
            expect (afterOff >= afterRestore + 1, "turning it off hands over the open segment");
            expectEquals (collector.calls, afterOff, "nothing after it is off");
        }

        //==============================================================================
        // 性能（依据：任务书「分析器每块均值（实时模式关：学习中 / 不学习；实时模式开），与现有 learningGuardMicroseconds 护栏对照」；
        // 不改现有护栏）：同上面通用档护栏的量法（48 kHz 立体声、块 512、4 遍取每块最短再求均值 / 最坏）。四种：关 · 学习中、关 · 不学习、
        // 开 · 不学习（通用档：候选一直跑 + 实时段落账）、开 · House 档学习中（候选 + 两份段落账 + 门控画像）。实时模式开着的两种都要低于
        // learningGuardMicroseconds（它的工作量上限就是「学习中」再加一份段落账）。
        void testPerformance()
        {
            beginTest ("performance: analyzer per 512-sample block at 48 kHz, live mode off (learning / not) and on (general / House learning)");

            constexpr double sampleRate = 48000.0;
            constexpr int blockLength = 512;
            auto input = makePinkNoise (numChannels, (int) (10.0 * sampleRate), -24.0f, 101);
            addInto (input, makeDrumPattern (sampleRate, 10.0, 5));
            const auto numBlocks = (int) input.front().size() / blockLength;
            juce::AudioBuffer<float> block (numChannels, blockLength);
            SegmentCollector collector (64);

            auto measure = [&] (bool live, bool learn, bool house)
            {
                std::vector<juce::int64> best ((size_t) numBlocks, std::numeric_limits<juce::int64>::max());

                for (int pass = 0; pass < 4; ++pass)
                {
                    Rig rig (sampleRate);

                    if (house)
                        rig.analyzer.setGenre (glaze::Genre::deepHouse);

                    if (live)
                    {
                        rig.analyzer.setSegmentSink (&collector);
                        rig.analyzer.setLiveMode (true);
                    }

                    if (learn)
                        rig.analyzer.startLearning();

                    for (int b = 0; b < numBlocks; ++b)
                    {
                        for (int ch = 0; ch < numChannels; ++ch)
                            block.copyFrom (ch, 0, input[(size_t) ch].data() + b * blockLength, blockLength);

                        rig.analyzer.setHostInfo (tempoHost ((std::int64_t) b * blockLength, sampleRate));
                        rig.meter.process (block);
                        const auto t0 = juce::Time::getHighResolutionTicks();
                        rig.analyzer.processInput (block, rig.meter);
                        const auto t1 = juce::Time::getHighResolutionTicks();
                        rig.balance.process (block);
                        const auto t2 = juce::Time::getHighResolutionTicks();
                        rig.analyzer.processSpectrum (rig.balance);
                        const auto t3 = juce::Time::getHighResolutionTicks();
                        best[(size_t) b] = juce::jmin (best[(size_t) b], (t1 - t0) + (t3 - t2));
                    }
                }

                double sum = 0.0, worst = 0.0;

                for (auto ticks : best)
                {
                    const auto seconds = juce::Time::highResolutionTicksToSeconds (ticks);
                    sum += seconds;
                    worst = juce::jmax (worst, seconds);
                }

                return std::pair<double, double> { 1.0e6 * sum / numBlocks, 1.0e6 * worst };
            };

            const auto offLearning = measure (false, true, false);
            const auto offIdle = measure (false, false, false);
            const auto liveIdle = measure (true, false, false);
            const auto liveHouse = measure (true, true, true);
            const auto text = [] (const std::pair<double, double>& m)
            {
                return "mean " + juce::String (m.first, 2) + " us, worst " + juce::String (m.second, 2) + " us";
            };

            logMessage ("  analyzer @48k block 512 - live off, learning: " + text (offLearning) + "; live off, not learning: " + text (offIdle)
                        + "; live on, general, not learning: " + text (liveIdle) + "; live on, House, learning: " + text (liveHouse)
                        + " (guard " + juce::String (learningGuardMicroseconds, 0) + " us)");
            expect (liveIdle.first < learningGuardMicroseconds, "live mode (general, not learning) too slow: " + juce::String (liveIdle.first, 2) + " us per block");
            expect (liveHouse.first < learningGuardMicroseconds, "live mode (House, learning) too slow: " + juce::String (liveHouse.first, 2) + " us per block");
            expect (collector.calls > 0, "the live runs delivered segments (positive control)");
        }

        //==============================================================================
        // ---- T-048 阶段 1 修订（代理 B2）：实时段落账与学习脱钩、advanceTo、关实时模式补帧、从第 N 小节起播 ----
        // 依据：任务书第 1 ～ 5 条、MusicAnalyzer.cpp「七」节（三道门的口径 ①～⑤、时间轴与 advanceTo、断开、关实时模式时不丢最后一块的帧）、
        // SegmentLedger.cpp anchor。下面的帮助类型只给这几个用例用（追加在类里，上面的工具一个不改）。

        // 收段又记 advanceTo 的接收者：段原样存下，同时记「这一段交来之前报过的最晚时刻」；报数是否单调不减；每次报数时，上次报数之后交来的段
        // end 是不是都不晚于它。预先留好容量，音频线程上不分配。
        struct TimelineCollector final : glaze::SegmentSink
        {
            explicit TimelineCollector (size_t capacity = 1024)
            {
                segments.reserve (capacity);
                reportedBefore.reserve (capacity);
            }

            void segmentFinalized (const SegmentLedger::Segment& segment) noexcept override
            {
                ++calls;
                pendingMaxEnd = juce::jmax (pendingMaxEnd, segment.end);

                if (segments.size() < segments.capacity())
                {
                    segments.push_back (segment);
                    reportedBefore.push_back (reports > 0 ? lastReport : std::numeric_limits<std::int64_t>::min());
                }
            }

            void advanceTo (std::int64_t analyzerTime) noexcept override
            {
                if (reports > 0 && analyzerTime < lastReport)
                    monotone = false;

                if (pendingMaxEnd > analyzerTime)
                    coversDelivered = false;

                pendingMaxEnd = std::numeric_limits<std::int64_t>::min();
                lastReport = analyzerTime;
                ++reports;
            }

            std::vector<SegmentLedger::Segment> segments;
            std::vector<std::int64_t> reportedBefore;
            std::int64_t lastReport = 0;
            std::int64_t pendingMaxEnd = std::numeric_limits<std::int64_t>::min();
            int calls = 0, reports = 0;
            bool monotone = true, coversDelivered = true;
        };

        // 段里只在学习期间记的三项（trialBlocks / trialTop / blend*：SegmentLedger.h 的约定，随学习的起止变，实时画像只读 live*）清掉，其余原样。
        static SegmentLedger::Segment withoutLearningTrial (SegmentLedger::Segment segment)
        {
            segment.trialBlocks = 0;
            segment.trialTop = {};
            segment.blendIntended = {};
            segment.blendOutput = {};
            return segment;
        }

        // 除学习期专用的三项外逐字段逐位相同（sameSegmentAll，一项不漏）；不同时 where 写第一处。
        static bool sameLiveSegments (const std::vector<SegmentLedger::Segment>& a, const std::vector<SegmentLedger::Segment>& b,
                                      juce::String& where)
        {
            if (a.size() != b.size())
            {
                where = "segment count " + juce::String ((int) a.size()) + " vs " + juce::String ((int) b.size());
                return false;
            }

            for (size_t i = 0; i < a.size(); ++i)
            {
                if (! sameSegmentAll (withoutLearningTrial (a[i]), withoutLearningTrial (b[i])))
                {
                    where = "segment " + juce::String ((int) i) + " [" + juce::String (a[i].start) + ", " + juce::String (a[i].end) + ")";
                    return false;
                }
            }

            return true;
        }

        // 有门边内容的合成 House（要考到三道相对门）：makeHouseTrack 24 小节（约 46.5 s），0 ～ 28 s 原样，28 ～ 34 s 压低 26 dB（比主体轻
        // 20 dB 以上：相对门挡住它，绝对门挡不住），34 ～ 38 s 原样，38 s 起 8 s 内按 dB 线性淡出到 −60 dB，之后静音。
        static Signal makeGatedTrack (juce::int64 seed)
        {
            auto track = makeHouseTrack (48000.0, 4, 20, 0.0, seed);

            for (auto& channel : track)
            {
                for (size_t i = 0; i < channel.size(); ++i)
                {
                    const auto t = (double) i / 48000.0;
                    const auto gainDb = t < 28.0 ? 0.0 : t < 34.0 ? -26.0 : t < 38.0 ? 0.0 : t < 46.0 ? -60.0 * (t - 38.0) / 8.0 : -200.0;
                    channel[i] *= (float) juce::Decibels::decibelsToGain (gainDb, -199.0);
                }
            }

            return track;
        }

        // 压低的那 6 s 里有绝对门之上的门块、却一个有效子块都没有的段：实时参考的相对门真的起了作用（阳性对照）。
        static bool quietSectionGated (const std::vector<SegmentLedger::Segment>& segments)
        {
            for (const auto& s : segments)
                if (s.start >= samplesAt (28.5) && s.end <= samplesAt (33.5) && s.gatingCount > 0 && s.effectiveSubBlocks == 0)
                    return true;

            return false;
        }

        // 一份分析器开着实时模式按 blockSize（≤ 0 = 随机 1 ～ 4096）喂完 input：宿主恒定 124 BPM，宿主位置 = startPosition + 已送样本数；
        // 每块之前 beforeBlock（已送样本数在 rig.fed）；最后 setLiveMode (false, &balance) 交出开着的段。houseStored 非空时另存 House 那一份的段。
        static std::unique_ptr<TimelineCollector> runLive (const Signal& input, int blockSize, glaze::Genre genre, std::int64_t startPosition,
                                                          const std::function<void (Rig&)>& beforeBlock,
                                                          std::vector<SegmentLedger::Segment>* houseStored = nullptr, juce::int64 seed = 1)
        {
            auto collector = std::make_unique<TimelineCollector>();
            auto rig = std::make_unique<Rig> (48000.0);
            rig->analyzer.setGenre (genre);
            rig->analyzer.setSegmentSink (collector.get());
            rig->analyzer.setLiveMode (true);
            rig->feed (input, blockSize, seed, [startPosition] (std::int64_t fed, int) { return tempoHost (startPosition + fed, 48000.0); },
                       beforeBlock);
            rig->analyzer.setLiveMode (false, &rig->balance);

            if (houseStored != nullptr)
            {
                const auto& ledger = rig->analyzer.getSegmentLedger();

                for (int i = 0; i < ledger.getNumStored(); ++i)
                    houseStored->push_back (ledger.getStored (i));
            }

            return collector;
        }

        // 链一级：开着实时模式（接收者 sink）按 blockSize 从宿主位置 0 走完 input（恒定 124 BPM 的拍点与位置）。链按 4096 准备，各块长同一份配置。
        struct LiveChain
        {
            LiveChain (const glaze::Constants& constants, const ChainParams& p, glaze::SegmentSink* sink) : params (p)
            {
                chain->getGlazeController().setConstantsForTesting (constants);
                chain->setParams (params, false);
                chain->prepare ({ 48000.0, (juce::uint32) 4096, (juce::uint32) numChannels });

                if (sink != nullptr)
                    chain->setLiveSegmentSink (sink);
            }

            void play (const Signal& input, int blockSize, std::int64_t until = -1)
            {
                const auto total = until >= 0 ? until : (std::int64_t) input.front().size();
                juce::AudioBuffer<float> block (numChannels, blockSize);

                while (position < total)
                {
                    const auto length = (int) juce::jmin ((std::int64_t) blockSize, total - position);
                    block.setSize (numChannels, length, false, false, true);

                    for (int ch = 0; ch < numChannels; ++ch)
                        block.copyFrom (ch, 0, input[(size_t) ch].data() + position, length);

                    step (block);
                    position += length;
                }
            }

            void step (juce::AudioBuffer<float>& block)
            {
                chain->setHostInfo (tempoHost (position, 48000.0));
                chain->setParams (params, false);
                chain->process (block);
            }

            std::unique_ptr<CeladonChain> chain = std::make_unique<CeladonChain>();
            ChainParams params;
            std::int64_t position = 0;
        };

        static ChainParams liveChainParams (int genre)
        {
            ChainParams params;
            params.limit.ceilingDb = -1.0f;
            params.glazePercent = genre == 0 ? 100.0f : 70.0f;
            params.glazeGenre = genre;
            return params;
        }

        //==============================================================================
        // 5 a（分析器一级）：学习在**真实的块边界**上停——各块长在 12.345 s 之后的第一个块边界上 stopLearning（不是各块长的公倍数，停的样本
        // 各不相同），从头开始学习。实时段落账交来的段在块长 7 / 32 / 480 / 512 / 4096 下除学习期专用三项外逐字段相同；通用档与 Deep House
        // 各一遍（House 那一份段落账同时在跑）。素材有一段压低 26 dB 与淡出（三道相对门的门边内容），阳性对照：压低那一段被实时参考的相对门挡住、
        // 各块长停学习的样本确实不同。
        void testLearningStopsOnRealBlockBoundaries()
        {
            beginTest ("B2 5a: learning stopped on real block boundaries - live segments identical for blocks 7 / 32 / 480 / 512 / 4096 (all but learning-period fields)");

            const auto track = makeGatedTrack (91);
            const auto stopAt = samplesAt (12.345);

            for (const auto genre : { glaze::Genre::general, glaze::Genre::deepHouse })
            {
                const auto tag = juce::String (genre == glaze::Genre::general ? "general" : "Deep House");
                std::vector<SegmentLedger::Segment> reference;
                std::int64_t referenceStop = -1;
                auto stopsDiffer = false;

                for (const auto blockSize : { 512, 7, 32, 480, 4096 })
                {
                    std::int64_t stoppedAt = -1;
                    const auto collector = runLive (track, blockSize, genre, 0, [&stoppedAt, stopAt] (Rig& r)
                    {
                        if (r.fed == 0)
                            r.analyzer.startLearning();

                        if (stoppedAt < 0 && r.fed >= stopAt)
                        {
                            r.analyzer.stopLearning();
                            stoppedAt = r.fed;
                        }
                    });

                    if (reference.empty())
                    {
                        reference = collector->segments;
                        referenceStop = stoppedAt;
                        expect (reference.size() >= 20, tag + ": the run delivers the bars");
                        expect (quietSectionGated (reference), tag + ": the quiet section is held off by the live relative gate (positive control)");
                        continue;
                    }

                    juce::String where;
                    stopsDiffer = stopsDiffer || stoppedAt != referenceStop;
                    expect (sameLiveSegments (reference, collector->segments, where),
                            tag + ", block " + juce::String (blockSize) + " vs 512: " + where);
                }

                expect (stopsDiffer, tag + ": learning stopped on different samples for different block sizes (positive control)");
            }
        }

        //==============================================================================
        // 5 a（链一级）：编排器自己决定什么时候停学习（通用档提交、Deep House 锁定，都在它看到的块边界上），宿主块长 7 / 32 / 480 / 512 / 4096。
        // 实时段落账交来的段除学习期专用三项外逐字段相同。setLiveSegmentSink (nullptr) 收尾（第 3 条：最后一块的帧也交了）。
        void testChainLearningStopsOnRealBlockBoundaries()
        {
            beginTest ("B2 5a: CeladonChain commits / locks on its own block boundaries - live segments identical for host blocks 7 / 32 / 480 / 512 / 4096");

            const auto track = makeGatedTrack (93);

            for (const auto genre : { 0, 1 })
            {
                const auto tag = juce::String (genre == 0 ? "general" : "Deep House");
                std::vector<SegmentLedger::Segment> reference;

                for (const auto blockSize : { 512, 7, 32, 480, 4096 })
                {
                    TimelineCollector collector;
                    LiveChain run (shortListenConstants(), liveChainParams (genre), &collector);
                    run.play (track, blockSize);
                    run.chain->setLiveSegmentSink (nullptr);
                    const auto probe = run.chain->getGlazeController().getProbeForTesting();

                    expect (probe.learningStarts >= 1 && probe.commits >= 1,
                            tag + ", block " + juce::String (blockSize) + ": the controller learned and committed (positive control)");

                    if (reference.empty())
                    {
                        reference = collector.segments;
                        expect (reference.size() >= 20, tag + ": the run delivers the bars");
                        continue;
                    }

                    juce::String where;
                    expect (sameLiveSegments (reference, collector.segments, where), tag + ", block " + juce::String (blockSize) + " vs 512: " + where);
                }
            }
        }

        //==============================================================================
        // 5 b（分析器一级）：同一段音频，「从头学习到底」与「读档后不学习」（从不学习；读档时编排器先来一次宿主 reset，这里在第一块之前同样调）
        // 交来的段除学习期专用三项外逐字段相同（连续时间段号也相同：第一块之前的断开什么都不做）。不学习的那一份 trialBlocks 处处为 0，学习的
        // 那一份有；不学习时实时参考的相对门照样挡住压低的那一段。Deep House 那一遍另核对阳性对照：House 那一份段落账参考学习区间，两种状态下
        // 确实不同（素材考到了门边）。
        void testLearningStateIndependence()
        {
            beginTest ("B2 5b: the same audio gives identical live segments learning from the start and not learning after a state load");

            const auto track = makeGatedTrack (92);

            for (const auto genre : { glaze::Genre::general, glaze::Genre::deepHouse })
            {
                const auto tag = juce::String (genre == glaze::Genre::general ? "general" : "Deep House");
                std::vector<SegmentLedger::Segment> houseLearning, houseIdle;
                const auto learningRun = runLive (track, 512, genre, 0, [] (Rig& r) { if (r.fed == 0) r.analyzer.startLearning(); }, &houseLearning);
                const auto idleRun = runLive (track, 512, genre, 0, [] (Rig& r)
                {
                    if (r.fed == 0)
                        r.analyzer.handleDiscontinuity (Discontinuity::hostReset);
                }, &houseIdle);

                auto learningTrials = false, idleTrials = false;

                for (const auto& s : learningRun->segments)
                    learningTrials = learningTrials || s.trialBlocks > 0;

                for (const auto& s : idleRun->segments)
                    idleTrials = idleTrials || s.trialBlocks > 0;

                juce::String where;
                expect (learningRun->segments.size() >= 20, tag + ": the run delivers the bars");
                expect (sameLiveSegments (learningRun->segments, idleRun->segments, where), tag + ": learning vs not learning: " + where);
                expect (learningTrials && ! idleTrials, tag + ": learning-period trial blocks only while learning");
                expect (quietSectionGated (idleRun->segments), tag + ": not learning, the live relative gate still holds off the quiet section");

                if (genre == glaze::Genre::deepHouse)
                {
                    auto houseSame = houseLearning.size() == houseIdle.size();

                    for (size_t i = 0; houseSame && i < houseLearning.size(); ++i)
                        houseSame = sameSegmentAll (withoutLearningTrial (houseLearning[i]), withoutLearningTrial (houseIdle[i]));

                    expect (! houseSame, tag + ": the House ledger (learning-referenced gates) does differ between the two states (positive control)");
                }
            }
        }

        //==============================================================================
        // 5 b（链一级，REPORT-C 2.5 的场景）：第一遍从头聆听、提交（学习），存档；第二遍新链读档（不学习）从头再走一遍。两遍交来的段除学习期
        // 专用三项外逐字段相同（含连续时间段号）。通用档 GLAZE 100、Deep House GLAZE 70。
        void testChainSecondPassMatchesFirst()
        {
            beginTest ("B2 5b: CeladonChain - the second pass (state restored, not learning) delivers the same live segments as the first (learning)");

            const auto track = makeGatedTrack (94);

            for (const auto genre : { 0, 1 })
            {
                const auto tag = juce::String (genre == 0 ? "general" : "Deep House");
                TimelineCollector first, second;
                std::unique_ptr<juce::XmlElement> state;
                int secondLearningStarts = -1;

                {
                    LiveChain run (shortListenConstants(), liveChainParams (genre), &first);
                    run.play (track, 512);
                    run.chain->setLiveSegmentSink (nullptr);
                    state = run.chain->createGlazeStateXml();
                    expect (run.chain->getGlazeController().getProbeForTesting().commits >= 1, tag + ": the first pass committed (positive control)");
                }

                {
                    LiveChain run (shortListenConstants(), liveChainParams (genre), &second);
                    run.chain->restoreGlazeStateXml (state.get());
                    run.play (track, 512);
                    run.chain->setLiveSegmentSink (nullptr);
                    secondLearningStarts = run.chain->getGlazeController().getProbeForTesting().learningStarts;
                }

                juce::String where;
                expect (secondLearningStarts == 0, tag + ": the second pass did not learn");
                expect (first.segments.size() >= 20, tag + ": the first pass delivers the bars");
                expect (sameLiveSegments (first.segments, second.segments, where), tag + ": first vs second pass: " + where);
            }
        }

        //==============================================================================
        // 5 c：advanceTo。随机块长 1 ～ 4096，36 s 里有跳播、表与 Balance 被 reset（分析器时间轴从 0 重新数）、分析器 reset、同规格 prepare、
        // 再一次表 reset + 分析器 reset，最后 setLiveMode (false)。期望：每次 processInput / processSpectrum 都报（报数 ≥ 2 × 块数）；单调不减；
        // 每次报 T 时，此前交来的段 end 都 ≤ T，之后交来的段 end > T（交齐位置的语义）；交来的段在实时时间轴上首尾相接（时间轴回退处也接上）；
        // 最后一次报数 = 最后一段的 end。
        void testAdvanceToTimeline()
        {
            beginTest ("B2 5c: advanceTo is called every processInput / processSpectrum, never goes back, and means 'every segment ending by T has been delivered'");

            const auto track = sliceTo (makeHouseTrack (48000.0, 4, 16, 0.0, 95), samplesAt (36.0) + 777);
            const juce::dsp::ProcessSpec spec { 48000.0, (juce::uint32) preparedBlock, (juce::uint32) numChannels };
            enum class Event { jump, meterReset, analyzerReset, prepare, meterAndAnalyzerReset };
            const std::vector<std::pair<double, Event>> events { { 10.0, Event::jump }, { 16.0, Event::meterReset },
                                                                 { 22.0, Event::analyzerReset }, { 26.0, Event::prepare },
                                                                 { 30.0, Event::meterAndAnalyzerReset } };
            size_t next = 0;
            int blocks = 0;

            // 每个事件在起点不早于它的第一块之前做（同 runScript）。
            const auto collector = runLive (track, 0, glaze::Genre::general, 0, [&] (Rig& r)
            {
                ++blocks;

                for (; next < events.size() && samplesAt (events[next].first) <= r.fed; ++next)
                {
                    const auto event = events[next].second;

                    if (event == Event::jump)
                        r.analyzer.handleDiscontinuity (Discontinuity::transportJump);

                    if (event == Event::meterReset || event == Event::meterAndAnalyzerReset)
                    {
                        r.analyzer.handleDiscontinuity (Discontinuity::hostReset);
                        r.meter.reset();
                        r.balance.reset();
                    }

                    if (event == Event::analyzerReset || event == Event::meterAndAnalyzerReset)
                        r.analyzer.reset();

                    if (event == Event::prepare)
                        r.analyzer.prepare (spec);
                }
            }, nullptr, 7);

            expect (next == events.size(), "every event happened");

            const auto& s = collector->segments;
            auto contiguous = ! s.empty(), afterReport = true;

            for (size_t i = 0; i < s.size(); ++i)
            {
                contiguous = contiguous && s[i].start < s[i].end && (i == 0 || s[i].start == s[i - 1].end);
                afterReport = afterReport && s[i].end > collector->reportedBefore[i];
            }

            logMessage ("  " + juce::String ((int) s.size()) + " segments, " + juce::String (collector->reports) + " reports over "
                        + juce::String (blocks) + " blocks; last report " + juce::String (collector->lastReport));
            expect (s.size() >= 15 && collector->calls == (int) s.size(), "segments arrived (no overflow)");
            expect (collector->reports >= 2 * blocks, "a report at the end of every processInput and processSpectrum");
            expect (collector->monotone, "reported times never go back (also across the meter resets, the analyzer reset and prepare)");
            expect (collector->coversDelivered, "each report covers every segment delivered before it (end <= T)");
            expect (afterReport, "every segment delivered after a report T ends after T");
            expect (contiguous, "segments are end-to-end contiguous on the live time line, across jumps, resets and prepare");
            expect (! s.empty() && collector->lastReport == s.back().end, "the last report is the end of the last segment (turning live mode off)");
        }

        //==============================================================================
        // 5 d：关实时模式时最后一块的帧进了最后一段。链在下一块开头才交 Balance 的帧；宿主块 4096，走 20.3 s 之后：
        //   - CeladonChain::setLiveSegmentSink (nullptr)（第 3 条：先把还没交的帧只交给实时段落账）；
        //   - 对照：先补一个 0 样本的块再关（渲染工具原来的变通）——两者交来的段逐字段相同；
        //   - 阳性对照：直接调编排器的 setLiveSegmentSink (nullptr)（不给 Balance = 原来的做法）——前面的段相同，最后一段少了最后一块的帧。
        void testTurningOffKeepsLastFrames()
        {
            beginTest ("B2 5d: turning live mode off hands the last host block's Balance frames to the last segment");

            const auto track = makeHouseTrack (48000.0, 4, 8, 0.5, 96);
            const auto until = samplesAt (20.3);
            TimelineCollector fixed, padded, old;

            const auto run = [&] (TimelineCollector& sink, int ending)
            {
                LiveChain chain (shortListenConstants(), liveChainParams (0), &sink);
                chain.play (track, 4096, until);

                if (ending == 1)
                {
                    juce::AudioBuffer<float> empty (numChannels, 0);
                    chain.step (empty);
                }

                if (ending == 2)
                    chain.chain->getGlazeController().setLiveSegmentSink (nullptr);
                else
                    chain.chain->setLiveSegmentSink (nullptr);
            };

            run (fixed, 0);
            run (padded, 1);
            run (old, 2);

            expect (fixed.segments.size() >= 8, "segments delivered");
            expect (sameSegments (fixed.segments, padded.segments), "turning off = padding a zero-sample block first (every field of every segment)");

            if (fixed.segments.size() == old.segments.size() && ! fixed.segments.empty())
            {
                const auto last = fixed.segments.size() - 1;
                const std::vector<SegmentLedger::Segment> fixedHead (fixed.segments.begin(), fixed.segments.begin() + (std::ptrdiff_t) last);
                const std::vector<SegmentLedger::Segment> oldHead (old.segments.begin(), old.segments.begin() + (std::ptrdiff_t) last);
                logMessage ("  last segment frames: " + juce::String (fixed.segments[last].frames) + " with the fix, "
                            + juce::String (old.segments[last].frames) + " without");
                expect (sameSegments (fixedHead, oldHead), "the segments before the last one do not depend on how it is turned off");
                expect (fixed.segments[last].frames > old.segments[last].frames, "without handing over the pending frames the last segment misses them (positive control)");
            }
            else
            {
                expect (false, "same number of segments either way");
            }
        }

        //==============================================================================
        // 5 f（第 4 条）：从第 N 小节起播，宿主起点 = round(小节线)（渲染工具 --listen-from-bar 的取法）。48 kHz、124 BPM 一小节 92903.2258 样本：
        // 第 33 小节线在起点之后 0.23 个样本、第 3 小节在 0.45 个样本之后、第 4 小节在 0.32 个样本之前。三种情况实时段落账的第一段都是这一小节、
        // 从 0 起（原来前两种第一段是 [0, 1) 的碎段，这一小节从样本 1 起）；之后各段首尾相接、整段（最后一段截在结尾），块长 512 与 4096 相同。
        void testStartFromBarN()
        {
            beginTest ("B2 5f: playback from bar N (start = the bar line rounded to a sample): the first live segment is bar N from sample 0, no 1-sample piece");

            const auto track = makeHouseTrack (48000.0, 2, 4, 0.0, 97);
            const auto barLength = 4.0 * 48000.0 * 60.0 / houseTrackBpm;

            for (const auto bar : { 33, 3, 4 })
            {
                const auto start = (std::int64_t) std::llround ((double) (bar - 1) * barLength);
                std::vector<SegmentLedger::Segment> reference;

                for (const auto blockSize : { 512, 4096 })
                {
                    const auto collector = runLive (track, blockSize, glaze::Genre::general, start, {});
                    const auto& s = collector->segments;
                    const auto tag = "bar " + juce::String (bar) + ", block " + juce::String (blockSize);
                    auto wholeAndContiguous = s.size() >= 3;

                    for (size_t i = 1; wholeAndContiguous && i < s.size(); ++i)
                        wholeAndContiguous = s[i].start == s[i - 1].end && (i + 1 == s.size() || s[i].end - s[i].start == s[i].nominalLength);

                    expect (! s.empty() && s.front().start == 0 && s.front().end == (std::int64_t) std::llround (barLength)
                                && s.front().nominalLength == s.front().end && s.front().key == (std::int64_t) (bar - 1) * 64,
                            tag + ": the first segment is bar " + juce::String (bar) + " from sample 0");
                    expect (wholeAndContiguous, tag + ": whole, contiguous bars after it");

                    if (reference.empty())
                        reference = s;
                    else
                        expect (sameSegments (reference, s), tag + " vs 512");
                }
            }
        }

        //==============================================================================
        // 第「七」节 ⑥（Max 2026-10-03「持续偏轻就加快遗忘」）：合成 House 前 60 s 原样，之后整段压低 28 dB（像同一实例换成一首轻得多的歌，
        // 绝对门挡不住、相对门挡住）。实时段落账：换了之后头 20 s 的小节一个有效子块都没有（还没满 liveGateQuietSeconds；阳性对照：新素材
        // 确实在相对门之下——用最稳的子块门判，帧门与起音门在底鼓上可能漏过几帧）；之后加快遗忘，换了 65 s 之后的整小节三道门都已放开：
        // 帧、有效子块、有效起音样本都不少于前面响的 drop 小节的 90 %。没有 ⑥ 时按 180 s 遗忘，子块门要约 13 分钟才放开，这一项考得到。
        // 块长 512 与 4096 交来的段逐字段相同（⑥ 也只看各路自己的数据）。
        void testQuietProgramReleasesGates()
        {
            beginTest ("live gates: a program 28 dB quieter stays gated for the first 20 s, then forgetting speeds up and all three gates open within 65 s");

            auto track = makeHouseTrack (48000.0, 4, 80, 0.0, 98);   // 4 小节鼓前奏 + 80 小节 drop ≈ 162.6 s
            const auto switchAt = samplesAt (60.0);

            for (auto& channel : track)
                for (size_t i = (size_t) switchAt; i < channel.size(); ++i)
                    channel[i] *= (float) juce::Decibels::decibelsToGain (-28.0);

            std::vector<SegmentLedger::Segment> reference;

            for (const auto blockSize : { 512, 4096 })
            {
                const auto collector = runLive (track, blockSize, glaze::Genre::general, 0, {});
                const auto& segments = collector->segments;
                const auto tag = "block " + juce::String (blockSize);
                const auto whole = [] (const SegmentLedger::Segment& s) { return s.end - s.start == s.nominalLength; };

                // 响的 drop 小节（前奏之后、换之前）各道门放过的量取最小值，作「全放开」的基准。
                std::int64_t loudFrames = std::numeric_limits<std::int64_t>::max(), loudOnset = loudFrames;
                int loudSubBlocks = std::numeric_limits<int>::max(), loudSegments = 0;

                for (const auto& s : segments)
                {
                    if (whole (s) && s.start >= samplesAt (12.0) && s.end <= switchAt)
                    {
                        ++loudSegments;
                        loudFrames = juce::jmin (loudFrames, s.frames);
                        loudOnset = juce::jmin (loudOnset, s.onsetEffectiveSamples);
                        loudSubBlocks = juce::jmin (loudSubBlocks, s.effectiveSubBlocks);
                    }
                }

                expect (loudSegments >= 20 && loudFrames > 0 && loudOnset > 0 && loudSubBlocks > 0, tag + ": the loud bars pass all three gates");

                int heldSegments = 0, heldOpen = 0, laterSegments = 0, laterClosed = 0;

                for (const auto& s : segments)
                {
                    if (s.start >= switchAt + samplesAt (2.0) && s.end <= switchAt + samplesAt (18.0))
                    {
                        ++heldSegments;
                        heldOpen += s.effectiveSubBlocks > 0 ? 1 : 0;
                    }
                    else if (whole (s) && s.start >= switchAt + samplesAt (65.0))
                    {
                        ++laterSegments;
                        const auto open = s.frames * 10 >= loudFrames * 9 && s.onsetEffectiveSamples * 10 >= loudOnset * 9
                                          && s.effectiveSubBlocks * 10 >= loudSubBlocks * 9;
                        laterClosed += open ? 0 : 1;
                    }
                }

                expect (heldSegments >= 6 && heldOpen == 0, tag + ": the first 20 s after the switch stay gated (" + juce::String (heldOpen) + " of "
                                                                + juce::String (heldSegments) + " bars have effective sub-blocks)");
                expect (laterSegments >= 15 && laterClosed == 0, tag + ": 65 s after the switch all three gates are open (" + juce::String (laterClosed)
                                                                    + " of " + juce::String (laterSegments) + " bars pass less than 90 % of a loud bar)");

                // 只记录、不断言：每道门实际从换了之后多少秒起一直放开（= 最后一个不到 90 % 的整小节的段尾），把 ⑥ 的「约 1 分钟」量出来。
                // 断言只卡上面两头（头 18 s 挡住、65 s 之后全放开），中间放开得早一点晚一点都不算错。
                double framesOpenAt = 0.0, subBlocksOpenAt = 0.0, onsetOpenAt = 0.0;

                for (const auto& s : segments)
                {
                    if (! whole (s) || s.start < switchAt)
                        continue;

                    const auto secondsAfterSwitch = (double) (s.end - switchAt) / 48000.0;

                    if (s.frames * 10 < loudFrames * 9)                    framesOpenAt = secondsAfterSwitch;
                    if (s.effectiveSubBlocks * 10 < loudSubBlocks * 9)     subBlocksOpenAt = secondsAfterSwitch;
                    if (s.onsetEffectiveSamples * 10 < loudOnset * 9)      onsetOpenAt = secondsAfterSwitch;
                }

                logMessage ("  " + tag + ": open for good (>= 90 % of a loud bar) from " + juce::String (framesOpenAt, 1) + " s (frames), "
                            + juce::String (subBlocksOpenAt, 1) + " s (effective sub-blocks), " + juce::String (onsetOpenAt, 1)
                            + " s (effective onset samples) after the switch");

                if (reference.empty())
                {
                    reference = segments;
                    continue;
                }

                juce::String where;
                expect (sameLiveSegments (reference, segments, where), tag + " vs 512: " + where);
            }
        }
    };

    static MusicAnalyzerLiveTests musicAnalyzerLiveTests;
}

/*
  ==============================================================================

    ColorTests.cpp
    设计规格-Color「验收 · 自动测试」四条 + 「修订」的补充验收
    （实测延迟、Bypass / Mix = 0 逐位、Glue = 0 不压、分块无关），外加 Output 与 Tone 的方向 / 量级检查。

    只按规格与 ColorModule.h 的公开接口写，不依赖任何实现细节：
    每条断言的依据写在对应函数的注释里，方便复核。

    v0.5 性能优化（研究 P3 R1 ～ R4）追加的用例在文件后半（「v0.5」分隔线之后）：
      - HalfBandOversampler 对 juce::dsp::Oversampling 的等价性（差 < −120 dBFS、同整数延迟）、块长无关、预热复原；
      - Saturator：原 processSample 逐位不变（Limit 的 SoftClip 在用），整段 fastTanh 的精度与位置无关；
      - 空闲整段跳过与不跳过逐位相同（多场景、多采样率、多块长，含 −0 / 非规格化数 / NaN / Inf 输入），
        参数自动化下输出仍与块长无关，Heat 长时间空闲后重开连续、与 v0.4 定义（直流阻断吃真实湿路）的差别有上界；
      - 立体声（L = R）与单声道逐位相同（ARM 上两声道打包的双二阶与标量写法一致）；
      - 音频路径零分配；性能回归（48 kHz / 512 下相对 v0.4 的量级）。

    T-032（2026-09-29，深度研究 R3 / R8 的 Color 部分）追加的用例在文件末尾（「T-032」分隔线之后）：
      - 输出指纹（FNV-1a）：逐位不变的改动改前改后对照日志；
      - Glue 实际施加的增益跟联动静态曲线（R3-01：中频不再有自己的检测器）；
      - Saturator::processTubeResidualBlock 与「整形后再减原样本」逐位相同（R3-04）；
      - setParams 对 NaN / ±Inf 的守卫（R8-02）：非有限值回默认、有限值仍钳制，与取默认值的模块逐位相同。

  ==============================================================================
*/

#include "TestHelpers.h"

#include "../Source/dsp/ColorModule.h"
#include "../Source/dsp/HalfBandOversampler.h"
#include "../Source/dsp/Saturator.h"

#include <cstring>
#include <functional>

namespace celadon::test
{
    namespace
    {
        constexpr int numChannels = 2;
        constexpr int maxBlock = 512;

        // −120 dBFS 的线性值就是 1e-6：规格里「null < −120 dBFS」的门槛。
        constexpr float nullThreshold = 1.0e-6f;

        ColorModule::Params makeParams (float toneDb, float glue, float heat, float mixPercent, float outputDb, bool bypass = false)
        {
            ColorModule::Params p;
            p.toneDb = toneDb;
            p.glue = glue;
            p.heat = heat;
            p.mixPercent = mixPercent;
            p.outputDb = outputDb;
            p.bypass = bypass;
            return p;
        }

        // Tone 0、Glue 0、Heat 0、Mix 100、Output 0：规格验收 1 的「全归零」。
        ColorModule::Params neutralParams()
        {
            return makeParams (0.0f, 0.0f, 0.0f, 100.0f, 0.0f);
        }

        // 先 setParams 再 prepare（与 LimiterTests 同一套路：prepare → reset 会把平滑器直接落到目标，
        // 从第一个样本起就是稳态）；prepare 之后再 set 一次是保险——目标相同不会起斜坡，
        // 但万一实现的 prepare 把参数恢复成默认值，这一步能把它纠正回来。
        void prepareColor (ColorModule& color, double sampleRate, const ColorModule::Params& p)
        {
            color.setParams (p);
            color.prepare ({ sampleRate, (juce::uint32) maxBlock, (juce::uint32) numChannels });
            color.setParams (p);
        }

        // 按指定块大小把整段信号送进 Color，返回输出（与 TestHelpers::runLimiter 相同，只是模块类型不同）。
        Signal runColor (ColorModule& color, const Signal& input, int blockSize)
        {
            const auto numSamples = (int) input.front().size();
            auto output = input;

            juce::AudioBuffer<float> block (numChannels, blockSize);

            for (int start = 0; start < numSamples; start += blockSize)
            {
                const auto length = juce::jmin (blockSize, numSamples - start);
                block.setSize (numChannels, length, false, false, true);

                for (int ch = 0; ch < numChannels; ++ch)
                    block.copyFrom (ch, 0, input[(size_t) ch].data() + start, length);

                color.process (block);

                for (int ch = 0; ch < numChannels; ++ch)
                    std::copy (block.getReadPointer (ch), block.getReadPointer (ch) + length,
                               output[(size_t) ch].begin() + start);
            }

            return output;
        }

        void appendSignal (Signal& destination, const Signal& source)
        {
            for (size_t ch = 0; ch < destination.size(); ++ch)
                destination[ch].insert (destination[ch].end(), source[ch].begin(), source[ch].end());
        }

        float samplePeakOf (const Signal& signal)
        {
            float peak = 0.0f;

            for (const auto& channel : signal)
                for (auto sample : channel)
                    peak = juce::jmax (peak, std::abs (sample));

            return peak;
        }

        // 「响」的 null 测试信号：0.3 s 噪声 + 60 / 1000 / 9000 / 18000 Hz 正弦，采样点峰值归一到 −1 dBFS，
        // 后接 0.2 s 整段都在 −1 dBFS 的 1 kHz 正弦。修订 1 要求 Glue = 0 时 −3 dBFS 以上也一点不压，
        // 所以信号必须持续超过 −3 dBFS，而不是只有偶尔的尖峰。
        Signal makeLoudNullSignal (double sampleRate)
        {
            const auto loud = juce::Decibels::decibelsToGain (-1.0f);

            auto composite = makeNoise (numChannels, (int) (0.3 * sampleRate), 0.15f, 42);

            for (auto frequency : { 60.0, 1000.0, 9000.0, 18000.0 })
            {
                const auto tone = makeSine (numChannels, sampleRate, frequency, 0.1f, 0.3);

                for (size_t ch = 0; ch < composite.size(); ++ch)
                    for (size_t i = 0; i < composite[ch].size(); ++i)
                        composite[ch][i] += tone[ch][i];
            }

            scale (composite, loud / samplePeakOf (composite));
            appendSignal (composite, makeSine (numChannels, sampleRate, 1000.0, loud, 0.2));
            return composite;
        }

        // 全部声道合在一起的 RMS（[start, start + length)）。
        double rmsOf (const Signal& signal, int start, int length)
        {
            double sumOfSquares = 0.0;

            for (const auto& channel : signal)
                for (int i = start; i < start + length; ++i)
                    sumOfSquares += (double) channel[(size_t) i] * (double) channel[(size_t) i];

            return std::sqrt (sumOfSquares / ((double) signal.size() * (double) length));
        }

        // 相干窗口的单频幅度（峰值单位）；同时投影 sin / cos，测量不受模块延迟或相位影响。
        double toneAmplitude (const std::vector<float>& signal, int start, int length, double sampleRate, double frequency)
        {
            double sine = 0.0, cosine = 0.0;
            for (int i = 0; i < length; ++i)
            {
                const auto phase = juce::MathConstants<double>::twoPi * frequency * i / sampleRate;
                sine += signal[(size_t) (start + i)] * std::sin (phase);
                cosine += signal[(size_t) (start + i)] * std::cos (phase);
            }
            return 2.0 * std::hypot (sine, cosine) / length;
        }

        double toneDistortionRatio (const std::vector<float>& signal, int start, int length, double sampleRate, double frequency)
        {
            double sum = 0.0, sumSquares = 0.0;
            for (int i = 0; i < length; ++i)
            {
                const auto x = (double) signal[(size_t) (start + i)];
                sum += x;
                sumSquares += x * x;
            }
            const auto mean = sum / length;
            const auto acPower = sumSquares / length - mean * mean;
            const auto fundamental = toneAmplitude (signal, start, length, sampleRate, frequency);
            const auto fundamentalPower = fundamental * fundamental * 0.5;
            return fundamentalPower > 0.0 ? std::sqrt (juce::jmax (0.0, acPower - fundamentalPower) / fundamentalPower) : 1.0;
        }

        // 冻结的 v0.1.6.2 全带并联 Heat 声学参照（2026-09-26 低频/M-S 修订之前）。
        // 仅限 Tone/Glue=0、固定整数 Heat 的测试，独立重现旧公开曲线、8x FIR、补偿和64样本干路；
        // 不调用 Color 私有实现，不依赖工作区外快照。用于在相同高频谐波量下比较互调，而非只比较相同旋钮值。
        Signal renderLegacyFullbandHeat (const Signal& input, double sampleRate, int heat)
        {
            const auto channels = (int) input.size();
            const auto h = (float) heat / 100.0f;
            Saturator tube;
            tube.setCurve (Saturator::Curve::Tube);
            tube.setDriveAndAsymmetry (1.0f + h, 0.04f * h);
            constexpr int calibrationPoints = 1024;
            std::array<float, calibrationPoints> calibration {};
            const auto amplitude = juce::Decibels::decibelsToGain (-18.0f);
            double inputPower = 0.0;
            for (int i = 0; i < calibrationPoints; ++i)
            {
                calibration[(size_t) i] = amplitude * (float) std::sin (juce::MathConstants<double>::twoPi * i / calibrationPoints);
                inputPower += (double) calibration[(size_t) i] * calibration[(size_t) i];
            }
            tube.processTubeBlock (calibration.data(), calibrationPoints);
            double sum = 0.0, power = 0.0;
            for (auto x : calibration) { sum += x; power += (double) x * x; }
            const auto mean = sum / calibrationPoints;
            const auto compensation = (float) std::sqrt ((inputPower / calibrationPoints)
                / juce::jmax (1.0e-30, power / calibrationPoints - mean * mean));

            HalfBandOversampler oversampler ((size_t) channels);
            oversampler.addOversamplingStage (0.05f, -90.0f, 0.06f, -75.0f);
            oversampler.addOversamplingStage (0.10f, -80.0f, 0.12f, -70.0f);
            oversampler.addOversamplingStage (0.10f, -65.0f, 0.15f, -55.0f);
            oversampler.initProcessing (maxBlock);
            const auto latency = juce::roundToInt (oversampler.getLatencyInSamples());
            const auto k = std::tan (juce::MathConstants<double>::pi * 5.0 / sampleRate);
            const auto b0 = 1.0 / (1.0 + k), b1 = -b0, a1 = (k - 1.0) / (k + 1.0);
            std::vector<double> dc ((size_t) channels, 0.0);
            auto output = input;
            juce::AudioBuffer<float> block (channels, maxBlock);
            for (int start = 0; start < (int) input.front().size(); start += maxBlock)
            {
                const auto length = juce::jmin (maxBlock, (int) input.front().size() - start);
                for (int ch = 0; ch < channels; ++ch)
                    block.copyFrom (ch, 0, input[(size_t) ch].data() + start, length);
                juce::dsp::AudioBlock<float> whole (block);
                auto part = whole.getSubBlock (0, (size_t) length);
                auto up = oversampler.processSamplesUp (part);
                for (int ch = 0; ch < channels; ++ch)
                    tube.processTubeBlock (up.getChannelPointer ((size_t) ch), (int) up.getNumSamples());
                oversampler.processSamplesDown (part);
                for (int ch = 0; ch < channels; ++ch)
                    for (int i = 0; i < length; ++i)
                    {
                        const auto x = (double) block.getReadPointer (ch)[i];
                        const auto y = b0 * x + dc[(size_t) ch];
                        dc[(size_t) ch] = b1 * x - a1 * y;
                        const auto dryIndex = start + i - latency;
                        const auto dry = dryIndex >= 0 ? input[(size_t) ch][(size_t) dryIndex] : 0.0f;
                        const auto wet = (float) y * compensation;
                        output[(size_t) ch][(size_t) (start + i)] = dry + (wet - dry) * (0.25f * h);
                    }
            }
            return output;
        }

        double highHarmonicRatio (const Signal& signal, int start, int length, double sampleRate)
        {
            const auto carrier = toneAmplitude (signal[0], start, length, sampleRate, 6000.0);
            const auto second = toneAmplitude (signal[0], start, length, sampleRate, 12000.0);
            const auto third = toneAmplitude (signal[0], start, length, sampleRate, 18000.0);
            return std::hypot (second, third) / juce::jmax (1.0e-30, carrier);
        }

        double highIntermodulationRatio (const Signal& signal, int start, int length, double sampleRate)
        {
            double power = 0.0;
            for (auto frequency : { 5880.0, 5940.0, 6060.0, 6120.0 })
            {
                const auto amplitude = toneAmplitude (signal[0], start, length, sampleRate, frequency);
                power += amplitude * amplitude;
            }
            return std::sqrt (power) / juce::jmax (1.0e-30, toneAmplitude (signal[0], start, length, sampleRate, 6000.0));
        }

        double sideToMidRatio (const Signal& signal, int start, int length)
        {
            double midPower = 0.0, sidePower = 0.0;
            for (int i = start; i < start + length; ++i)
            {
                const auto mid = 0.5 * ((double) signal[0][(size_t) i] + signal[1][(size_t) i]);
                const auto side = 0.5 * ((double) signal[0][(size_t) i] - signal[1][(size_t) i]);
                midPower += mid * mid;
                sidePower += side * side;
            }
            return std::sqrt (sidePower / juce::jmax (1.0e-30, midPower));
        }

        int peakIndexOf (const std::vector<float>& channel)
        {
            int peakIndex = 0;

            for (int i = 0; i < (int) channel.size(); ++i)
                if (std::abs (channel[(size_t) i]) > std::abs (channel[(size_t) peakIndex]))
                    peakIndex = i;

            return peakIndex;
        }

        //==============================================================================
        // v0.5 用例的公共工具

        // 参数随时间变化的「剧本」：每块开头按块首样本的时刻取一次参数（与宿主每块 setParams 一样）。
        using Schedule = std::function<ColorModule::Params (double seconds)>;

        // 按指定块大小跑整段信号，每块开头 setParams (paramsAt (块首时刻))；声道数取自 input（单声道也行）。
        Signal runColorScheduled (ColorModule& color, const Signal& input, double sampleRate, int blockSize, const Schedule& paramsAt)
        {
            const auto channels = (int) input.size();
            const auto numSamples = (int) input.front().size();
            auto output = input;

            juce::AudioBuffer<float> block (channels, blockSize);

            for (int start = 0; start < numSamples; start += blockSize)
            {
                const auto length = juce::jmin (blockSize, numSamples - start);
                block.setSize (channels, length, false, false, true);

                for (int ch = 0; ch < channels; ++ch)
                    block.copyFrom (ch, 0, input[(size_t) ch].data() + start, length);

                color.setParams (paramsAt ((double) start / sampleRate));
                color.process (block);

                for (int ch = 0; ch < channels; ++ch)
                    std::copy (block.getReadPointer (ch), block.getReadPointer (ch) + length, output[(size_t) ch].begin() + start);
            }

            return output;
        }

        // 按位比较（float 的位模式：±0 分得开、NaN 也逐位比）：返回位模式不同的样本数。「逐位相同」按字面意思验。
        long countBitDifferences (const Signal& a, const Signal& b)
        {
            long count = 0;

            for (size_t ch = 0; ch < a.size(); ++ch)
                for (size_t i = 0; i < a[ch].size(); ++i)
                    if (std::memcmp (&a[ch][i], &b[ch][i], sizeof (float)) != 0)
                        ++count;

            return count;
        }

        // 整段输出的 64 位 FNV-1a 指纹（按声道顺序、按每个 float 的位模式）：指纹相同即整段逐位相同。
        // 给「逐位不变」的改动做改前改后的对照（日志里打印，见 testOutputFingerprint）。
        std::uint64_t fingerprintOf (const Signal& signal)
        {
            std::uint64_t hash = 14695981039346656037ull;

            for (const auto& channel : signal)
                for (auto sample : channel)
                {
                    std::uint32_t bits;
                    std::memcpy (&bits, &sample, sizeof (bits));

                    for (int k = 0; k < 4; ++k)
                    {
                        hash ^= (bits >> (8 * k)) & 0xffu;
                        hash *= 1099511628211ull;
                    }
                }

            return hash;
        }

        // 「像音乐」的立体声测试信号（固定种子）：粉红噪声 + 55 Hz 底鼓 + 军鼓噪声 + 踩镲 + 三个和弦音，峰值钳在 ±0.99。
        // 与 v0.5 研究 P3 / 实现 I2 的等价实验用同一个配方：宽带、有瞬态、有低频，Glue 会压、Heat 会饱和。
        Signal makeMusicLike (double sampleRate, double seconds, int seed = 20260925)
        {
            const auto numSamples = (int) std::ceil (sampleRate * seconds);
            auto signal = makeSilence (numChannels, numSamples);
            juce::Random random (seed);
            double pinkState[2][7] = {};
            float previousWhite[2] = {};
            const auto twoPi = juce::MathConstants<double>::twoPi;

            for (int i = 0; i < numSamples; ++i)
            {
                const auto t = (double) i / sampleRate;
                const auto common = random.nextFloat() * 2.0f - 1.0f;
                const auto kickTime = std::fmod (t, 0.5);
                const auto kick = 0.55 * std::sin (twoPi * 55.0 * kickTime) * std::exp (-kickTime / 0.08);
                const auto snareEnvelope = 0.30 * std::exp (-std::fmod (t + 0.75, 1.0) / 0.03);
                const auto hatEnvelope = 0.12 * std::exp (-std::fmod (t, 0.125) / 0.01);
                const auto pad = 0.05 * (std::sin (twoPi * 220.0 * t) + std::sin (twoPi * 330.0 * t + 0.3) + std::sin (twoPi * 440.0 * t + 1.1));

                for (int ch = 0; ch < numChannels; ++ch)
                {
                    const auto own = random.nextFloat() * 2.0f - 1.0f;
                    const auto white = 0.8f * common + 0.2f * own;
                    auto* b = pinkState[ch];

                    // Paul Kellett 的粉红噪声滤波器
                    b[0] = 0.99886 * b[0] + white * 0.0555179;  b[1] = 0.99332 * b[1] + white * 0.0750759;
                    b[2] = 0.96900 * b[2] + white * 0.1538520;  b[3] = 0.86650 * b[3] + white * 0.3104856;
                    b[4] = 0.55000 * b[4] + white * 0.5329522;  b[5] = -0.7616 * b[5] - white * 0.0168980;
                    const auto pink = (b[0] + b[1] + b[2] + b[3] + b[4] + b[5] + b[6] + white * 0.5362) * 0.11;
                    b[6] = white * 0.115926;

                    const auto hat = (double) (white - previousWhite[ch]) * 0.5;
                    previousWhite[ch] = white;

                    const auto value = pink + kick + snareEnvelope * (double) white + hatEnvelope * hat + pad * (ch == 0 ? 1.0 : 0.9);
                    signal[(size_t) ch][(size_t) i] = (float) juce::jlimit (-0.99, 0.99, value);
                }
            }

            return signal;
        }

        // GLAZE 100 时 Color 的实际参数（ADR-006 偏移：Tone +0.75、Glue 40、Heat 30）。
        ColorModule::Params glaze100Params()
        {
            return makeParams (0.75f, 40.0f, 30.0f, 100.0f, 0.0f);
        }

        // 跳过 / 不跳过对照用的参数剧本：每条都让某一级在「空闲」与「工作」之间来回，覆盖各处跳过的进入与退出。
        struct NamedSchedule
        {
            const char* name;
            Schedule paramsAt;
            double seconds;
        };

        std::vector<NamedSchedule> makeIdleSchedules()
        {
            return {
                { "neutral",           [] (double) { return neutralParams(); }, 1.5 },
                { "tone 0/2/0/-1.5/0", [] (double t) { return makeParams (t < 0.3 ? 0.0f : t < 0.8 ? 2.0f : t < 1.3 ? 0.0f : t < 1.6 ? -1.5f : 0.0f,
                                                                           0.0f, 0.0f, 100.0f, 0.0f); }, 2.2 },
                { "glue 0/50/0/100/0", [] (double t) { return makeParams (0.0f, t < 0.3 ? 0.0f : t < 0.8 ? 50.0f : t < 1.2 ? 0.0f : t < 1.5 ? 100.0f : 0.0f,
                                                                           0.0f, 100.0f, 0.0f); }, 2.2 },
                { "mix / output / bypass", [] (double t)
                  {
                      auto p = makeParams (1.0f, 30.0f, 0.0f, t < 0.2 ? 100.0f : 50.0f, t < 0.5 ? 0.0f : 3.0f, t >= 0.8 && t < 1.1);
                      if (t >= 1.4) p.mixPercent = 0.0f;
                      if (t >= 1.7) { p.mixPercent = 100.0f; p.outputDb = 0.0f; }
                      return p;
                  }, 2.2 },
                { "heat 0/60 every 0.7 s", [] (double t) { return makeParams (0.0f, 0.0f, ((int) (t / 0.7)) % 2 == 1 ? 60.0f : 0.0f, 100.0f, 0.0f); }, 3.0 },
                { "heat 0/5 every 0.7 s",  [] (double t) { return makeParams (0.0f, 0.0f, ((int) (t / 0.7)) % 2 == 1 ? 5.0f : 0.0f, 100.0f, 0.0f); }, 3.0 },
                { "heat 0/60 + glue 60",   [] (double t) { return makeParams (-1.0f, 60.0f, ((int) (t / 0.45)) % 2 == 1 ? 60.0f : 0.0f, 100.0f, 0.0f); }, 3.0 },
                { "glaze automation", [] (double t)
                  {
                      const auto g = (float) (0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * t / 2.0));
                      const auto full = glaze100Params();
                      return makeParams (full.toneDb * g, full.glue * g, full.heat * g, 100.0f, 0.0f);
                  }, 2.5 },
                { "glaze 100 -> 0 -> 0.1", [] (double t)
                  {
                      if (t < 0.5) return glaze100Params();
                      if (t < 1.2) return neutralParams();
                      return makeParams (0.00075f, 0.04f, 0.03f, 100.0f, 0.0f);
                  }, 2.0 },
            };
        }
    }

    class ColorTests  : public juce::UnitTest
    {
    public:
        ColorTests() : juce::UnitTest ("ColorModule", "Celadon") {}

        void runTest() override
        {
            testNullWhenNeutral();
            testNoAliasingAtFullHeat();
            testHeatLoudnessCompensation();
            testHeatRemainsGentleOnHotInput();
            testHeatIntermodulationWithToneHotspots();
            testHeatFullRangeAutomationOnHotInput();
            testBassSafeHeatAtMatchedHarmonics();
            testHeatPreservesLowFundamentals();
            testHeatStereoSymmetryAndWidth();
            testHeatHighFrequencyProtection();
            testGlueGainReduction();
            testMultibandGlueLetsLowSitAboveMid();
            testMeasuredLatencyMatchesReported();
            testBypassIsBitExact();
            testMixZeroIsBitExact();
            testGlueZeroNeverCompresses();
            testBlockSizeIndependence();
            testOutputGain();
            testToneDirection();
            testLatencyIs64AtAllRates();
            testGlueOffIsSmoothAndReturnsToUnity();
            testHeatToggleIsSmooth();
            testBypassToggleMidStream();
            testNoGainReductionReportedWhenBypassed();
            testHeatOnsetIsContinuous();
            testGlueReenableFromOffHasNoDip();

            // v0.5
            testHalfBandMatchesJuceOversampling();
            testHalfBandBlockSizeIndependence();
            testHalfBandPrimingRestoresState();
            testSaturatorProcessSampleUnchanged();
            testFastTanhAccuracyAndTubeBlock();
            testIdleSkippingIsBitExact();
            testHeatIdleAfterPartialFadeBlock();
            testBlockSizeIndependenceUnderAutomation();
            testIdleSkippingWithSpecialValues();
            testHeatReenableAfterLongIdle();
            testStereoMatchesMono();
            testPackedStereoMatchesScalar();
            testNoAllocationsOnAudioPath();
            testPerformanceRegression();

            // T-032（2026-09-29）
            testOutputFingerprint();
            testGlueAppliedGainTracksLinkedCurve();
            testTubeResidualBlockMatchesTubeMinusInput();
            testNonFiniteParamsFallBackToDefaults();
        }

    private:
        void testHeatHighFrequencyProtection()
        {
            beginTest ("Heat high-frequency budget reduces generated upper harmonics while preserving the dry mix");
            for (const auto sr : { 44100.0, 48000.0, 96000.0 })
            {
                const auto input = makeSine (numChannels, sr, 4500.0, 1.4f, 1.5);
                ColorModule enabled, baseline;
                baseline.setHeatHighFrequencyProtectionEnabled (false);
                ColorModule::Params p;
                p.heat = 100.0f;
                prepareColor (enabled, sr, p);
                prepareColor (baseline, sr, p);
                const auto a = runColor (enabled, input, 333);
                const auto b = runColor (baseline, input, 333);
                const auto start = (int) sr, length = (int) (sr * 0.5);
                const auto harmonic = [&] (const Signal& s)
                {
                    return toneAmplitude (s[0], start, length, sr, 13500.0)
                         / toneAmplitude (s[0], start, length, sr, 4500.0);
                };
                const auto improvement = toDb (harmonic (b) / harmonic (a));
                const auto rmsDelta = toDb (rmsOf (a, start, length) / rmsOf (b, start, length));
                logMessage ("  Heat upper harmonic improvement=" + juce::String (improvement, 3)
                            + " dB, RMS delta=" + juce::String (rmsDelta, 3) + " dB @" + juce::String (sr, 0));
                expect (improvement > 1.0f, "harmonic ratio must improve, not merely lower the entire output");
                expect (std::abs (rmsDelta) < 0.75f, "retain comparable overall level");
                expect (allFinite (a));
                expectEquals (enabled.getLatencySamples(), baseline.getLatencySamples());

                ColorModule split;
                prepareColor (split, sr, p);
                expectEquals (countBitDifferences (a, runColor (split, input, 17)), 0L,
                              "linked residual guard must follow sample time across arbitrary block sizes");

                // Ordinary low-level colour should not be filtered merely
                // because the protection exists. This also checks its deadband.
                const auto gentle = makeSine (numChannels, sr, 1000.0, 0.1f, 0.5);
                p.heat = 30.0f;
                prepareColor (enabled, sr, p);
                prepareColor (baseline, sr, p);
                expectEquals (countBitDifferences (runColor (enabled, gentle, 64), runColor (baseline, gentle, 64)), 0L,
                              "low-level gentle colour stays unchanged");
            }
        }

        // 验收 1：Tone = Glue = Heat = 0、Mix 100、Output 0 → 与延迟对齐后的输入差 < −120 dBFS。
        // 依据：规格「验收 1」+ 修订 1（Glue = 0 彻底关闭，−3 dBFS 以上也不压）+ 修订 2（Heat = 0 走等长延迟干路）。
        // 三个采样率都测；信号见 makeLoudNullSignal。从第 0 个输出样本起就比：reset 把平滑器直接落到目标，没有起始斜坡。
        void testNullWhenNeutral()
        {
            beginTest ("null when tone = glue = heat = 0, mix 100, output 0 (loud input)");

            for (auto sampleRate : { 44100.0, 48000.0, 96000.0 })
            {
                ColorModule color;
                prepareColor (color, sampleRate, neutralParams());

                auto input = makeLoudNullSignal (sampleRate);
                expect (samplePeakOf (input) > juce::Decibels::decibelsToGain (-1.01f), "test signal must reach -1 dBFS");

                const auto length = (int) input.front().size();
                const auto latency = color.getLatencySamples();
                appendSilence (input, latency);
                const auto output = runColor (color, input, maxBlock);

                expect (allFinite (output));

                const auto diff = maxAbsDifference (input, output, latency, length);
                expect (diff < nullThreshold, "null residual " + juce::String (toDb (diff), 1) + " dBFS @" + juce::String (sampleRate));
                logMessage ("  @" + juce::String (sampleRate) + ": latency " + juce::String (latency)
                            + ", residual " + juce::String (toDb (diff), 1) + " dBFS");
            }
        }

        // 验收 2：48 kHz、10 kHz 正弦 −6 dBFS、Heat = 100（其余中性）→ 0 ～ 10 kHz 内的非谐波折返分量比基波低 60 dB 以上。
        // 判定方法：
        //   1. 跑 1 s，取最后 16384 个输出样本作稳态段（起点 0.66 s，远超 20 ms 参数平滑 + 5 Hz 直流阻断的 32 ms 时间常数 + 延迟）；
        //   2. 乘 4 项 Blackman-Harris 窗（旁瓣 −92 dB，主瓣半宽 4 个 bin），做 16384 点 FFT 取幅度谱（bin 宽 2.93 Hz）；
        //   3. 基波 = 10 kHz 对应 bin（3413）± 8 个 bin 内的最大幅度；
        //   4. 搜索区 = [20 Hz 对应的第一个 bin（7），基波 bin − 9]，即 0 ～ 10 kHz 去掉 < 20 Hz 与基波主瓣；
        //   5. 搜索区最大幅度 / 基波幅度 < −60 dB 即通过。10 kHz 的各次谐波都在 20 kHz 以上，
        //      所以 0 ～ 10 kHz 内除基波外的任何分量都只能是混叠折返（或直流阻断没压干净的偏移，已被 < 20 Hz 排除）。
        void testNoAliasingAtFullHeat()
        {
            beginTest ("no aliasing below 10 kHz at heat = 100 (10 kHz sine, -6 dBFS)");

            constexpr double sampleRate = 48000.0;
            constexpr int fftOrder = 14;
            constexpr int fftSize = 1 << fftOrder;   // 16384
            constexpr int mainLobeHalfWidth = 8;

            ColorModule color;
            prepareColor (color, sampleRate, makeParams (0.0f, 0.0f, 100.0f, 100.0f, 0.0f));

            const auto input = makeSine (numChannels, sampleRate, 10000.0, juce::Decibels::decibelsToGain (-6.0f), 1.0);
            const auto output = runColor (color, input, maxBlock);
            expect (allFinite (output));

            const auto totalSamples = (int) output.front().size();
            const auto segmentStart = totalSamples - fftSize;
            expect (segmentStart > (int) (0.5 * sampleRate), "steady-state segment must start well after the transients");

            std::vector<float> data ((size_t) (2 * fftSize), 0.0f);
            std::copy (output[0].begin() + segmentStart, output[0].begin() + segmentStart + fftSize, data.begin());

            juce::dsp::WindowingFunction<float> window ((size_t) fftSize, juce::dsp::WindowingFunction<float>::blackmanHarris, false);
            window.multiplyWithWindowingTable (data.data(), (size_t) fftSize);

            juce::dsp::FFT fft (fftOrder);
            fft.performFrequencyOnlyForwardTransform (data.data(), true);

            const auto binHz = sampleRate / fftSize;
            const auto fundamentalBin = juce::roundToInt (10000.0 / binHz);
            const auto lowestBin = (int) std::ceil (20.0 / binHz);

            float fundamental = 0.0f;
            for (int k = fundamentalBin - mainLobeHalfWidth; k <= fundamentalBin + mainLobeHalfWidth; ++k)
                fundamental = juce::jmax (fundamental, data[(size_t) k]);

            float worst = 0.0f;
            int worstBin = lowestBin;
            for (int k = lowestBin; k < fundamentalBin - mainLobeHalfWidth; ++k)
            {
                if (data[(size_t) k] > worst)
                {
                    worst = data[(size_t) k];
                    worstBin = k;
                }
            }

            expect (fundamental > 0.0f, "fundamental must be present");
            const auto worstRelativeDb = toDb ((double) worst / (double) fundamental);

            logMessage ("  fundamental bin " + juce::String (fundamentalBin) + ", worst in-band component "
                        + juce::String (worstRelativeDb, 1) + " dB re fundamental at " + juce::String (worstBin * binHz, 0) + " Hz");
            expect (worstRelativeDb < -60.0f, "aliasing component " + juce::String (worstRelativeDb, 1) + " dB re fundamental at "
                                                + juce::String (worstBin * binHz, 0) + " Hz (limit -60 dB)");
        }

        // 验收 3：−18 dBFS 1 kHz 正弦，Heat 0 → 100 每 10 一档，稳态 RMS 相对 Heat 0 的变化在 ±1 dB 内。
        // 依据：规格「自动响度补偿：prepare 时用 −18 dBFS 正弦对每个 Heat 值预算补偿系数」。
        // 稳态段 = 1 s 里的后 0.5 s（整 500 个周期，RMS 不受起止相位影响）；每档新建模块，互不干扰。
        void testHeatLoudnessCompensation()
        {
            beginTest ("heat loudness compensation keeps -18 dBFS sine within +/-1 dB");

            constexpr double sampleRate = 48000.0;
            const auto input = makeSine (numChannels, sampleRate, 1000.0, juce::Decibels::decibelsToGain (-18.0f), 1.0);
            const auto segmentStart = (int) (0.5 * sampleRate);
            const auto segmentLength = (int) (0.5 * sampleRate);

            double referenceRms = 0.0;

            for (int heat = 0; heat <= 100; heat += 10)
            {
                ColorModule color;
                prepareColor (color, sampleRate, makeParams (0.0f, 0.0f, (float) heat, 100.0f, 0.0f));

                const auto output = runColor (color, input, maxBlock);
                expect (allFinite (output), "heat " + juce::String (heat));

                const auto rms = rmsOf (output, segmentStart, segmentLength);

                if (heat == 0)
                {
                    referenceRms = rms;
                    expect (rms > 0.0, "reference RMS must be non-zero");
                    continue;
                }

                const auto deltaDb = toDb (rms / referenceRms);
                logMessage ("  heat " + juce::String (heat) + ": " + juce::String (deltaDb, 2) + " dB re heat 0");
                expect (std::abs (deltaDb) <= 1.0f, "heat " + juce::String (heat) + " changes RMS by " + juce::String (deltaDb, 2) + " dB");
            }
        }

        // 热母带回归：旧映射在 −0.5 dBFS、Heat 10 已约 15% THD，Heat 100 约 40%，并损失约 14 dB RMS。
        // 母带并联映射的档位上限：Heat 10 / 30 / 100 的 THD+残差 < 0.4% / 1.2% / 4%；
        // −18 / −6 / −0.5 dBFS 输入的稳态电平变化均在 −1.25 ～ +0.25 dB。保留可测的染色，防止用旁通通过测试。
        void testHeatRemainsGentleOnHotInput()
        {
            beginTest ("heat stays below 4% distortion and 1.25 dB level loss on near-full-scale input");

            constexpr double sampleRate = 48000.0;
            constexpr int start = 19200, length = 4800;   // 0.4 ～ 0.5 s，100 个整周期，DC 状态已稳定
            struct Setting { float heat; double distortionLimit; };

            for (auto inputDb : { -18.0f, -6.0f, -0.5f })
            {
                const auto input = makeSine (numChannels, sampleRate, 1000.0, juce::Decibels::decibelsToGain (inputDb), 0.5);
                const auto inputRms = rmsOf (input, start, length);

                for (const auto setting : { Setting { 10.0f, 0.004 }, Setting { 30.0f, 0.012 }, Setting { 100.0f, 0.04 } })
                {
                    ColorModule color;
                    prepareColor (color, sampleRate, makeParams (0.0f, 0.0f, setting.heat, 100.0f, 0.0f));
                    const auto output = runColor (color, input, maxBlock);
                    const auto distortion = toneDistortionRatio (output[0], start, length, sampleRate, 1000.0);
                    const auto levelDb = toDb (rmsOf (output, start, length) / inputRms);
                    const auto label = "input " + juce::String (inputDb, 1) + " dBFS, heat " + juce::String (setting.heat, 0);

                    expect (allFinite (output), "non-finite output: " + label);
                    expect (distortion < setting.distortionLimit, label + ": distortion " + juce::String (100.0 * distortion, 3) + "%");
                    expect (levelDb >= -1.25f && levelDb <= 0.25f, label + ": level change " + juce::String (levelDb, 3) + " dB");
                    if (setting.heat == 100.0f)
                        expect (distortion > 0.0005, label + ": full heat must retain measurable coloration");
                    logMessage ("  " + label + ": " + juce::String (100.0 * distortion, 3) + "% distortion, "
                                + juce::String (levelDb, 3) + " dB RMS change");
                }
            }
        }

        // 低频通过共享非线性调制高频：60 Hz + 6 kHz，Tone ±3 dB 覆盖先抬低频 / 高频再进入 Heat 的热点。
        // 量 6 kHz ±60 / ±120 Hz 四条互调旁带的合成幅度，相对载波在 Heat 30 < 3%、Heat 100 < 10%。
        // 与同 Tone、Heat 0 的参考比较，防止通过降低整体音量掩盖失真；也约束峰值突增与电平损失。
        void testHeatIntermodulationWithToneHotspots()
        {
            beginTest ("parallel heat bounds bass/treble intermodulation with tone +/-3 dB");

            constexpr double sampleRate = 48000.0;
            constexpr int start = 14400, length = 4800;   // 0.3 ～ 0.4 s：6 个低频周期与 600 个高频周期
            auto input = makeSine (numChannels, sampleRate, 60.0, 0.72f, 0.4);
            const auto high = makeSine (numChannels, sampleRate, 6000.0, 0.20f, 0.4);
            for (size_t ch = 0; ch < input.size(); ++ch)
                for (size_t i = 0; i < input[ch].size(); ++i)
                    input[ch][i] += high[ch][i];

            for (auto tone : { -3.0f, 3.0f })
            {
                ColorModule reference;
                prepareColor (reference, sampleRate, makeParams (tone, 0.0f, 0.0f, 100.0f, 0.0f));
                const auto dry = runColor (reference, input, maxBlock);
                const auto referenceRms = rmsOf (dry, start, length);

                for (auto heat : { 30.0f, 100.0f })
                {
                    ColorModule color;
                    prepareColor (color, sampleRate, makeParams (tone, 0.0f, heat, 100.0f, 0.0f));
                    const auto output = runColor (color, input, maxBlock);
                    const auto carrier = toneAmplitude (output[0], start, length, sampleRate, 6000.0);
                    double sidebandPower = 0.0;
                    for (auto frequency : { 5880.0, 5940.0, 6060.0, 6120.0 })
                    {
                        const auto amplitude = toneAmplitude (output[0], start, length, sampleRate, frequency);
                        sidebandPower += amplitude * amplitude;
                    }
                    const auto imd = carrier > 0.0 ? std::sqrt (sidebandPower) / carrier : 1.0;
                    const auto levelDb = toDb (rmsOf (output, start, length) / referenceRms);
                    const auto label = "tone " + juce::String (tone, 0) + ", heat " + juce::String (heat, 0);
                    expect (allFinite (output), "non-finite output: " + label);
                    expect (imd < (heat < 100.0f ? 0.03 : 0.10), label + ": IMD " + juce::String (100.0 * imd, 3) + "%");
                    expect (levelDb >= -2.0f && levelDb <= 0.25f, label + ": level change " + juce::String (levelDb, 3) + " dB");
                    expect (samplePeakOf (output) <= samplePeakOf (dry) * 1.05f, label + ": heat must not cause a peak burst");
                    logMessage ("  " + label + ": " + juce::String (100.0 * imd, 3) + "% IMD, "
                                + juce::String (levelDb, 3) + " dB RMS change");
                }
            }
        }

        // 热低频 + Tone 低频提升，扫过全范围、满档保持、在非零相位瞬间把旋钮回零。
        // 正常波形自身的最大相邻样本变化作参照，最多多 8%；一块内取消平滑会产生明显更大的跳变。
        void testHeatFullRangeAutomationOnHotInput()
        {
            beginTest ("full heat sweep and reset remain continuous with hot input and tone boost");

            constexpr double sampleRate = 48000.0;
            const auto input = makeSine (numChannels, sampleRate, 60.0, juce::Decibels::decibelsToGain (-0.5f), 0.9);
            const auto paramsAt = [] (double t)
            {
                const auto heat = t < 0.2 ? 0.0f : t < 0.5 ? (float) ((t - 0.2) * (100.0 / 0.3)) : t < 0.653 ? 100.0f : 0.0f;
                return makeParams (-3.0f, 0.0f, heat, 100.0f, 0.0f);
            };

            ColorModule color;
            prepareColor (color, sampleRate, paramsAt (0.0));
            const auto output = runColorScheduled (color, input, sampleRate, 64, paramsAt);
            ColorModule reference;
            prepareColor (reference, sampleRate, paramsAt (0.0));
            const auto dry = runColor (reference, input, 64);
            const auto dryStep = maxStep (dry, (int) (0.12 * sampleRate), (int) (0.18 * sampleRate));
            const auto sweepStep = maxStep (output, (int) (0.18 * sampleRate), (int) (0.80 * sampleRate));

            expect (allFinite (output));
            expect (sweepStep <= dryStep * 1.08f, "automation jump " + juce::String (sweepStep, 6)
                                                  + " exceeds dry reference " + juce::String (dryStep, 6));
            expect (samplePeakOf (output) <= samplePeakOf (dry) * 1.05f, "automation must not create a peak burst");
            expectEquals (maxDiffFrom (dry, output, 0, (int) (0.75 * sampleRate)), 0.0f,
                          "after heat returns to zero, output must be exactly the Tone-only reference");
        }

        void testBassSafeHeatAtMatchedHarmonics()
        {
            beginTest ("bass-safe Heat lowers 60 Hz / 6 kHz intermodulation at matched upper harmonics");
            constexpr double sampleRate = 48000.0;
            constexpr double seconds = 0.4;
            constexpr int start = 14400, length = 4800;
            constexpr int legacyHeat = 60;
            const auto highOnly = makeSine (numChannels, sampleRate, 6000.0, 0.20f, seconds);
            const auto legacyHigh = renderLegacyFullbandHeat (highOnly, sampleRate, legacyHeat);
            const auto target = highHarmonicRatio (legacyHigh, start, length, sampleRate);
            expect (target > 1.0e-4, "the old reference must have measurable coloration");

            const auto render = [&] (const Signal& input, float heat)
            {
                ColorModule color;
                prepareColor (color, sampleRate, makeParams (0.0f, 0.0f, heat, 100.0f, 0.0f));
                return runColor (color, input, maxBlock);
            };
            // 调节新 Heat 匹配同一 6 kHz 单音的 H2+H3/基波，而非把两边都设成一个旋钮刻度。
            expect (highHarmonicRatio (render (highOnly, 100.0f), start, length, sampleRate) >= target,
                    "new Heat must still be capable of the reference harmonic strength");
            float lower = 0.0f, upper = 100.0f;
            for (int iteration = 0; iteration < 10; ++iteration)
            {
                const auto midpoint = 0.5f * (lower + upper);
                const auto ratio = highHarmonicRatio (render (highOnly, midpoint), start, length, sampleRate);
                if (ratio < target) lower = midpoint;
                else upper = midpoint;
            }
            const auto heat = 0.5f * (lower + upper);
            const auto matched = render (highOnly, heat);
            const auto matchDb = toDb (highHarmonicRatio (matched, start, length, sampleRate) / target);
            expect (std::abs (matchDb) <= 0.5f, "upper harmonic strength must match within 0.5 dB");
            expect (heat > 10.0f, "comparison must exercise meaningful Heat rather than near-bypass");

            auto bassAndHigh = makeSine (numChannels, sampleRate, 60.0, 0.72f, seconds);
            for (size_t ch = 0; ch < bassAndHigh.size(); ++ch)
                for (size_t i = 0; i < bassAndHigh[ch].size(); ++i)
                    bassAndHigh[ch][i] += highOnly[ch][i];
            const auto legacy = renderLegacyFullbandHeat (bassAndHigh, sampleRate, legacyHeat);
            const auto output = render (bassAndHigh, heat);
            const auto oldImd = highIntermodulationRatio (legacy, start, length, sampleRate);
            const auto newImd = highIntermodulationRatio (output, start, length, sampleRate);
            const auto improvement = toDb (oldImd / juce::jmax (1.0e-30, newImd));
            logMessage ("  legacy Heat 60 vs new " + juce::String (heat, 3) + ": harmonic match " + juce::String (matchDb, 3)
                        + " dB, IMD " + juce::String (100.0 * oldImd, 4) + "% -> " + juce::String (100.0 * newImd, 4)
                        + "% (" + juce::String (improvement, 2) + " dB improvement)");
            expect (allFinite (output));
            expect (oldImd > 0.001, "reference low/high mixture must expose intermodulation");
            expect (improvement >= 6.0f, "bass protection must reduce IMD by at least 6 dB at matched coloration");
        }

        void testHeatPreservesLowFundamentals()
        {
            beginTest ("Heat preserves 40 / 60 / 100 Hz fundamentals on quiet and hot bass");
            for (auto sampleRate : { 44100.0, 48000.0, 96000.0 })
                for (auto frequency : { 40.0, 60.0, 100.0 })
                    for (auto inputDb : { -12.0f, -0.5f })
                    {
                        const auto input = makeSine (numChannels, sampleRate, frequency, juce::Decibels::decibelsToGain (inputDb), 0.6);
                        const auto start = (int) std::lround (0.4 * sampleRate);
                        const auto length = (int) std::lround (0.2 * sampleRate); // 所有低频整周期
                        const auto dryAmplitude = toneAmplitude (input[0], start, length, sampleRate, frequency);
                        for (auto heat : { 30.0f, 100.0f })
                        {
                            ColorModule color;
                            prepareColor (color, sampleRate, makeParams (0.0f, 0.0f, heat, 100.0f, 0.0f));
                            const auto output = runColor (color, input, maxBlock);
                            const auto delta = toDb (toneAmplitude (output[0], start, length, sampleRate, frequency) / dryAmplitude);
                            const auto distortion = toneDistortionRatio (output[0], start, length, sampleRate, frequency);
                            const auto tag = juce::String (frequency, 0) + " Hz @ " + juce::String (sampleRate, 0)
                                + ", input " + juce::String (inputDb, 1) + ", Heat " + juce::String (heat, 0);
                            expect (allFinite (output), tag);
                            expect (std::abs (delta) <= 0.25f, tag + ": fundamental drift " + juce::String (delta, 4) + " dB");
                            expect (distortion < 0.01, tag + ": low-frequency distortion " + juce::String (100.0 * distortion, 4) + "%");
                        }
                    }
        }

        void testHeatStereoSymmetryAndWidth()
        {
            beginTest ("Heat preserves pure Side, channel exchange symmetry and the Mid / Side energy ratio");
            constexpr double sampleRate = 48000.0;
            constexpr double seconds = 0.6;
            constexpr int start = 19200, length = 9600;
            auto sideOnly = makeSine (numChannels, sampleRate, 1000.0, 0.72f, seconds);
            const auto high = makeSine (numChannels, sampleRate, 6000.0, 0.12f, seconds);
            for (size_t i = 0; i < sideOnly[0].size(); ++i)
            {
                sideOnly[0][i] += high[0][i];
                sideOnly[1][i] = -sideOnly[0][i];
            }
            for (auto heat : { 30.0f, 100.0f })
            {
                const auto params = makeParams (0.0f, 0.0f, heat, 100.0f, 0.0f);
                ColorModule side;
                prepareColor (side, sampleRate, params);
                const auto output = runColor (side, sideOnly, 73);
                double midPeak = 0.0;
                for (size_t i = 0; i < output[0].size(); ++i)
                    midPeak = juce::jmax (midPeak, std::abs (0.5 * ((double) output[0][i] + output[1][i])));
                expect (midPeak < nullThreshold, "odd Side coloration must not generate a phantom Mid channel");

                auto source = makeMusicLike (sampleRate, seconds, 9312);
                auto swapped = source;
                std::swap (swapped[0], swapped[1]);
                ColorModule original, exchanged;
                prepareColor (original, sampleRate, params);
                prepareColor (exchanged, sampleRate, params);
                const auto a = runColor (original, source, 73);
                auto b = runColor (exchanged, swapped, 73);
                std::swap (b[0], b[1]);
                expectEquals ((int) countBitDifferences (a, b), 0, "left/right exchange must commute with Heat");

                for (auto midFrequency : { 60.0, 1200.0 })
                {
                    const auto mid = makeSine (numChannels, sampleRate, midFrequency, 0.65f, seconds);
                    const auto sideSignal = makeSine (numChannels, sampleRate, 3000.0, 0.25f, seconds);
                    auto stereo = mid;
                    for (size_t i = 0; i < stereo[0].size(); ++i)
                    {
                        stereo[0][i] += sideSignal[0][i];
                        stereo[1][i] -= sideSignal[0][i];
                    }
                    ColorModule color;
                    prepareColor (color, sampleRate, params);
                    const auto coloured = runColor (color, stereo, maxBlock);
                    const auto widthDrift = toDb (sideToMidRatio (coloured, start, length) / sideToMidRatio (stereo, start, length));
                    const auto tag = "Heat " + juce::String (heat, 0) + ", Mid " + juce::String (midFrequency, 0) + " Hz";
                    logMessage ("  " + tag + ": S/M change " + juce::String (widthDrift, 3) + " dB");
                    expect (std::abs (widthDrift) <= 0.75f, tag + ": stereo energy balance must remain within 0.75 dB");
                }
            }
        }

        // 验收 4：Glue = 100、−6 dBFS 1 kHz 正弦 → getAndResetGainReductionDb 在 3 ～ 10 dB 之间，输出无 NaN / Inf。
        // 区间按 2026-09-24 夜间修订：阈值 −24 dBFS、2:1、峰值检测时静态曲线的理论值就是 (−6 − (−24)) × (1 − 1/2) = 9 dB，
        // 初版写的 3 ～ 8 dB 把理论值本身排除在外（设计规格-Color「修订」）。实测约 8.5 dB（dB 域平滑让读数略低于静态值）。
        // 依据：规格「验收 4」原文。Glue 100 → 阈值 −24 dBFS，比率 2:1；读数是自上次读取以来的最大值，读完清零。
        void testMultibandGlueLetsLowSitAboveMid()
        {
            beginTest ("glue compresses a loud mid tone more than a quieter low tone");

            constexpr double sampleRate = 48000.0;
            constexpr double lowHz = 90.0;
            constexpr double midHz = 2500.0;
            constexpr double seconds = 2.0;
            const auto lowAmp = juce::Decibels::decibelsToGain (-16.0f);
            const auto midAmp = juce::Decibels::decibelsToGain (-8.0f);
            auto input = makeSine (numChannels, sampleRate, lowHz, lowAmp, seconds);
            const auto mid = makeSine (numChannels, sampleRate, midHz, midAmp, seconds, 0.4);

            for (int ch = 0; ch < numChannels; ++ch)
                for (size_t i = 0; i < input[(size_t) ch].size(); ++i)
                    input[(size_t) ch][i] += mid[(size_t) ch][i];

            ColorModule color;
            prepareColor (color, sampleRate, makeParams (0.0f, 100.0f, 0.0f, 100.0f, 0.0f));
            const auto output = runColor (color, input, maxBlock);
            const auto start = (int) (1.4 * sampleRate);
            const auto length = (int) (0.4 * sampleRate);
            const auto lowIn = toneAmplitude (input[0], start, length, sampleRate, lowHz);
            const auto midIn = toneAmplitude (input[0], start, length, sampleRate, midHz);
            const auto lowOut = toneAmplitude (output[0], start, length, sampleRate, lowHz);
            const auto midOut = toneAmplitude (output[0], start, length, sampleRate, midHz);
            const auto lowChange = toDb (lowOut / lowIn);
            const auto midChange = toDb (midOut / midIn);

            logMessage ("  low " + juce::String (lowChange, 2) + " dB, mid " + juce::String (midChange, 2) + " dB");
            expect (allFinite (output));
            expect (midChange < -1.0f, "the loud mid tone must be compressed");
            expect (lowChange > midChange + 1.0f, "the low tone must be compressed less than the mid tone");
        }

        void testGlueGainReduction()
        {
            beginTest ("glue = 100 on -6 dBFS sine: 3..10 dB gain reduction, finite output");

            constexpr double sampleRate = 48000.0;
            ColorModule color;
            prepareColor (color, sampleRate, makeParams (0.0f, 100.0f, 0.0f, 100.0f, 0.0f));

            const auto input = makeSine (numChannels, sampleRate, 1000.0, juce::Decibels::decibelsToGain (-6.0f), 1.0);
            const auto output = runColor (color, input, maxBlock);
            expect (allFinite (output));

            const auto gainReductionDb = color.getAndResetGainReductionDb();
            logMessage ("  gain reduction " + juce::String (gainReductionDb, 2) + " dB");
            expect (gainReductionDb >= 3.0f && gainReductionDb <= 10.0f,
                    "gain reduction " + juce::String (gainReductionDb, 2) + " dB outside 3..10 dB");
            expectEquals (color.getAndResetGainReductionDb(), 0.0f, "meter must clear after reading");
        }

        // 补充：脉冲实测延迟 = getLatencySamples()，Heat = 0（干路）与 Heat = 50（过采样湿路）两种，三个采样率。
        // 依据：修订「实测延迟 = 上报延迟」「延迟整数、不插 Thiran 全通」。湿路的上采样 FIR、tanh（无记忆、单调）、
        // 下采样 FIR 都关于脉冲中心对称，整数延迟下输出的最大样本就落在 position + latency。
        void testMeasuredLatencyMatchesReported()
        {
            beginTest ("impulse latency == reported latency (heat 0 and heat 50)");

            for (auto sampleRate : { 44100.0, 48000.0, 96000.0 })
             for (auto heat : { 0.0f, 50.0f })
            {
                ColorModule color;
                prepareColor (color, sampleRate, makeParams (0.0f, 0.0f, heat, 100.0f, 0.0f));

                const auto latency = color.getLatencySamples();
                expect (latency >= 0, "latency must not be negative");

                constexpr int position = 100;
                const auto input = makeImpulse (numChannels, position + latency + 256, position, 0.5f);
                const auto output = runColor (color, input, maxBlock);
                expect (allFinite (output));

                expectEquals (peakIndexOf (output[0]) - position, latency,
                              "@" + juce::String (sampleRate) + " heat=" + juce::String (heat));
            }
        }

        // 补充：Bypass 开 → 逐位等于延迟后的输入，哪怕 Tone / Glue / Heat / Output 全开满、输入 0 dBFS 噪声。
        // 依据：修订「完全旁通时逐位等于延迟后的输入」。bypass 在 prepare 前设好，交叉淡化从第 0 个样本起就是全干。
        void testBypassIsBitExact()
        {
            beginTest ("bypass is bit-exact delayed input");

            constexpr double sampleRate = 48000.0;
            ColorModule color;
            prepareColor (color, sampleRate, makeParams (3.0f, 100.0f, 100.0f, 100.0f, 6.0f, true));

            auto input = makeNoise (numChannels, 24000, 1.0f, 7);
            const auto length = (int) input.front().size();
            const auto latency = color.getLatencySamples();
            appendSilence (input, latency);
            const auto output = runColor (color, input, maxBlock);

            expectEquals (maxAbsDifference (input, output, latency, length), 0.0f);
        }

        // 补充：Mix = 0 → 逐位等于延迟后的输入（Tone / Glue / Heat 开满，Output 0 dB 即 × 1.0）。
        // 依据：规格「out = dry × (1 − mix) + wet × mix」，mix = 0 时 wet 项精确为 0；修订「Mix = 0 逐位等于延迟后的输入」。
        void testMixZeroIsBitExact()
        {
            beginTest ("mix = 0 is bit-exact delayed input");

            constexpr double sampleRate = 48000.0;
            ColorModule color;
            prepareColor (color, sampleRate, makeParams (3.0f, 100.0f, 100.0f, 0.0f, 0.0f));

            auto input = makeNoise (numChannels, 24000, 1.0f, 11);
            const auto length = (int) input.front().size();
            const auto latency = color.getLatencySamples();
            appendSilence (input, latency);
            const auto output = runColor (color, input, maxBlock);

            expect (allFinite (output));
            expectEquals (maxAbsDifference (input, output, latency, length), 0.0f);
        }

        // 补充：Glue = 0 时压缩器彻底关闭 → GR 读数精确为 0，哪怕信号持续在 −1 dBFS、Tone 与 Heat 开满。
        // 依据：修订 1「Glue = 0 彻底关闭压缩器，增益精确为 1、补偿为 0」。
        void testGlueZeroNeverCompresses()
        {
            beginTest ("glue = 0 reports zero gain reduction on loud input");

            constexpr double sampleRate = 48000.0;
            ColorModule color;
            prepareColor (color, sampleRate, makeParams (3.0f, 0.0f, 100.0f, 100.0f, 0.0f));

            const auto output = runColor (color, makeLoudNullSignal (sampleRate), maxBlock);
            expect (allFinite (output));
            expectEquals (color.getAndResetGainReductionDb(), 0.0f);
        }

        // 补充：不同块大小（含 1 与超过 prepare 块大小的 2048）输出逐位一致；全部级都开着，让平滑器、
        // 系数更新计数器、过采样、压缩包络都参与。
        // 依据：修订「输出不受块大小影响」。
        void testBlockSizeIndependence()
        {
            beginTest ("output independent of block size");

            constexpr double sampleRate = 48000.0;
            const auto params = makeParams (2.0f, 60.0f, 40.0f, 70.0f, 3.0f);
            const auto input = makeNoise (numChannels, 48000, 0.8f, 3);

            Signal reference;

            for (auto blockSize : { 512, 1, 17, 2048 })
            {
                ColorModule color;
                prepareColor (color, sampleRate, params);
                auto output = runColor (color, input, blockSize);
                expect (allFinite (output), "block size " + juce::String (blockSize));

                if (reference.empty())
                    reference = output;
                else
                    expectEquals (maxAbsDifference (reference, output, 0, (int) input.front().size()), 0.0f,
                                  "block size " + juce::String (blockSize));
            }
        }

        // 额外：Output +6 dB 且其余中性 → 输出 = 延迟后的输入 × 10^(6/20)。
        // 依据：规格参数表「Output −6 ～ +6 dB 输出微调」。跳过前 latency + 20 ms（平滑器的最长斜坡），
        // 容差 1e-5（相对 0.25 的幅度约 −88 dB）：只允许实现用 double 还是 float 算增益带来的舍入差。
        void testOutputGain()
        {
            beginTest ("output trim applies the requested gain");

            constexpr double sampleRate = 48000.0;
            ColorModule color;
            prepareColor (color, sampleRate, makeParams (0.0f, 0.0f, 0.0f, 100.0f, 6.0f));

            const auto input = makeSine (numChannels, sampleRate, 1000.0, 0.25f, 0.5);
            const auto latency = color.getLatencySamples();
            auto padded = input;
            appendSilence (padded, latency);
            const auto output = runColor (color, padded, maxBlock);

            const auto expectedGain = juce::Decibels::decibelsToGain (6.0f);
            const auto settled = latency + (int) (0.02 * sampleRate);
            float diff = 0.0f;

            for (size_t ch = 0; ch < output.size(); ++ch)
                for (int i = settled; i < (int) output[ch].size(); ++i)
                    diff = juce::jmax (diff, std::abs (output[ch][(size_t) i] - input[ch][(size_t) (i - latency)] * expectedGain));

            expect (diff <= 1.0e-5f, "output trim error " + juce::String (diff, 8));
        }

        // 额外：Tone 的方向与量级。Tone = +3：20 kHz（8 kHz 高搁架之上 2.5 倍）升 1.5 ～ 3.5 dB，
        // 24 Hz（100 Hz 低搁架之下 1/4）降 1.5 ～ 3.5 dB；Tone = −3 反过来。
        // 依据：规格「高搁架 8 kHz 增益 = +Tone，低搁架 100 Hz 增益 = −Tone，Q 0.707」。
        // 区间放到 ±1 dB 是因为搁架的「转折频率」定义不同（RBJ 在转折点只到一半增益）：离转折点 2.5 倍 / 1/4 处
        // 各种定义都落在 2.7 ～ 3 dB。RMS 取后 0.5 s：24 Hz 整 12 个周期、20 kHz 整 10000 个周期。
        void testToneDirection()
        {
            beginTest ("tone tilts: +tone brightens 20 kHz and dims 24 Hz, -tone the opposite");

            constexpr double sampleRate = 48000.0;
            const auto segmentStart = (int) (0.5 * sampleRate);
            const auto segmentLength = (int) (0.5 * sampleRate);

            for (auto toneDb : { 3.0f, -3.0f })
             for (auto frequency : { 24.0, 20000.0 })
            {
                ColorModule color;
                prepareColor (color, sampleRate, makeParams (toneDb, 0.0f, 0.0f, 100.0f, 0.0f));

                const auto input = makeSine (numChannels, sampleRate, frequency, 0.25f, 1.0);
                const auto output = runColor (color, input, maxBlock);
                expect (allFinite (output));

                const auto gainDb = toDb (rmsOf (output, segmentStart, segmentLength) / rmsOf (input, segmentStart, segmentLength));
                const auto expectedDb = frequency > 1000.0 ? toneDb : -toneDb;

                logMessage ("  tone " + juce::String (toneDb) + " @" + juce::String (frequency, 0) + " Hz: " + juce::String (gainDb, 2) + " dB");
                expectWithinAbsoluteError (gainDb, expectedDb, 1.0f,
                                           "tone " + juce::String (toneDb) + " @" + juce::String (frequency, 0) + " Hz");
            }
        }

        //==============================================================================
        // 以下 5 条为 2026-09-24 夜间评审（R1-02 / R1-03 / R1-04）补的回归用例。

        // 同一模块先跑 first，改参数，再跑 second；返回两段输出拼起来（与输入等长）。
        Signal runWithSwitch (ColorModule& color, const Signal& input, int switchAt, const ColorModule::Params& after)
        {
            Signal first, second;

            for (const auto& channel : input)
            {
                first.emplace_back (channel.begin(), channel.begin() + switchAt);
                second.emplace_back (channel.begin() + switchAt, channel.end());
            }

            auto output = runColor (color, first, maxBlock);
            color.setParams (after);
            const auto tail = runColor (color, second, maxBlock);

            for (size_t ch = 0; ch < output.size(); ++ch)
                output[ch].insert (output[ch].end(), tail[ch].begin(), tail[ch].end());

            return output;
        }

        // 从 fromIndex 起（到末尾），输出与「延迟 latency 后的输入」的最大差。
        static float maxDiffFrom (const Signal& input, const Signal& output, int latency, int fromIndex)
        {
            float diff = 0.0f;

            for (size_t ch = 0; ch < output.size(); ++ch)
                for (int i = fromIndex; i < (int) output[ch].size(); ++i)
                    diff = juce::jmax (diff, std::abs (output[ch][(size_t) i] - input[ch][(size_t) (i - latency)]));

            return diff;
        }

        // [from, to) 内相邻样本的最大跳变（左声道）。
        static float maxStep (const Signal& signal, int from, int to)
        {
            float step = 0.0f;

            for (int i = juce::jmax (1, from); i < to; ++i)
                step = juce::jmax (step, std::abs (signal[0][(size_t) i] - signal[0][(size_t) (i - 1)]));

            return step;
        }

        // Color 延迟恒为 64（Heat 8× 三级过采样，修订后），与采样率无关；192 kHz 下脉冲实测也一致。
        void testLatencyIs64AtAllRates()
        {
            beginTest ("latency is 64 samples at 44.1k / 48k / 96k / 192k");

            for (auto sampleRate : { 44100.0, 48000.0, 96000.0, 192000.0 })
            {
                ColorModule color;
                prepareColor (color, sampleRate, makeParams (0.0f, 0.0f, 50.0f, 100.0f, 0.0f));
                expectEquals (color.getLatencySamples(), 64, "@" + juce::String (sampleRate));

                constexpr int position = 200;
                auto input = makeImpulse (numChannels, 2048, position, 0.25f);
                const auto output = runColor (color, input, maxBlock);

                int peakIndex = 0;
                for (int i = 0; i < (int) output[0].size(); ++i)
                    if (std::abs (output[0][(size_t) i]) > std::abs (output[0][(size_t) peakIndex]))
                        peakIndex = i;

                expectEquals (peakIndex - position, 64, "impulse @" + juce::String (sampleRate));
            }
        }

        // Glue 从 100 直接拧到 0：20 ms 内平滑淡出，不先掉一个电平坑；之后很快逐位等于延迟后的输入。
        void testGlueOffIsSmoothAndReturnsToUnity()
        {
            beginTest ("glue 100 -> 0: no level dip, bit-exact again shortly after");

            constexpr double sampleRate = 48000.0;
            ColorModule color;
            prepareColor (color, sampleRate, makeParams (0.0f, 100.0f, 0.0f, 100.0f, 0.0f));

            const auto switchAt = (int) sampleRate;
            const auto input = makeSine (numChannels, sampleRate, 1000.0, juce::Decibels::decibelsToGain (-6.0f), 2.0);
            const auto output = runWithSwitch (color, input, switchAt, makeParams (0.0f, 0.0f, 0.0f, 100.0f, 0.0f));
            const auto latency = color.getLatencySamples();

            // 2 ms 窗 RMS：切换前的稳态、切换后的稳态、过渡里的最小值。
            const auto window = (int) (0.002 * sampleRate);
            auto rmsAt = [&] (int start)
            {
                double sum = 0.0;
                for (int i = start; i < start + window; ++i)
                    sum += (double) output[0][(size_t) i] * output[0][(size_t) i];
                return std::sqrt (sum / window);
            };

            const auto before = rmsAt (switchAt - 4 * window);
            const auto after  = rmsAt ((int) output[0].size() - 4 * window);
            auto lowest = juce::jmin (before, after);

            for (int start = switchAt; start < switchAt + latency + (int) (0.05 * sampleRate); start += window / 2)
                lowest = juce::jmin (lowest, rmsAt (start));

            const auto dipDb = toDb (lowest) - toDb (juce::jmin (before, after));
            logMessage ("  before " + juce::String (toDb (before), 2) + " dB, after " + juce::String (toDb (after), 2)
                        + " dB, deepest dip " + juce::String (dipDb, 2) + " dB");
            expect (dipDb > -0.1, "level dip " + juce::String (dipDb, 2) + " dB during glue switch-off");

            // 淡出 20 ms 之后（留 5 ms 余量）逐位等于延迟后的输入。
            const auto settledFrom = switchAt + latency + (int) (0.025 * sampleRate);
            expectEquals (maxDiffFrom (input, output, latency, settledFrom), 0.0f, "bit-exact after glue off");
        }

        // Heat 0 → 60 → 0：开、关的 20 ms 交叉淡化没有跳变；关掉之后逐位等于延迟后的输入。
        void testHeatToggleIsSmooth()
        {
            beginTest ("heat on / off crossfade has no jump, bit-exact after off");

            constexpr double sampleRate = 48000.0;
            ColorModule color;
            prepareColor (color, sampleRate, makeParams (0.0f, 0.0f, 0.0f, 100.0f, 0.0f));

            const auto input = makeSine (numChannels, sampleRate, 440.0, juce::Decibels::decibelsToGain (-12.0f), 1.5);
            const auto onAt  = (int) (0.5 * sampleRate);
            const auto offAt = (int) (1.0 * sampleRate);

            Signal a, b, c;
            for (const auto& channel : input)
            {
                a.emplace_back (channel.begin(), channel.begin() + onAt);
                b.emplace_back (channel.begin() + onAt, channel.begin() + offAt);
                c.emplace_back (channel.begin() + offAt, channel.end());
            }

            auto output = runColor (color, a, maxBlock);
            color.setParams (makeParams (0.0f, 0.0f, 60.0f, 100.0f, 0.0f));
            const auto ob = runColor (color, b, maxBlock);
            color.setParams (makeParams (0.0f, 0.0f, 0.0f, 100.0f, 0.0f));
            const auto oc = runColor (color, c, maxBlock);

            for (size_t ch = 0; ch < output.size(); ++ch)
            {
                output[ch].insert (output[ch].end(), ob[ch].begin(), ob[ch].end());
                output[ch].insert (output[ch].end(), oc[ch].begin(), oc[ch].end());
            }

            const auto latency = color.getLatencySamples();
            const auto fade = (int) (0.03 * sampleRate);
            const auto steadyDry = maxStep (output, onAt - 2 * fade, onAt);
            const auto steadyWet = maxStep (output, offAt - 2 * fade, offAt);
            const auto steady = juce::jmax (steadyDry, steadyWet);

            const auto onStep  = maxStep (output, onAt,  onAt  + latency + fade);
            const auto offStep = maxStep (output, offAt, offAt + latency + fade);

            logMessage ("  steady step " + juce::String (steady, 5) + ", on " + juce::String (onStep, 5)
                        + ", off " + juce::String (offStep, 5));
            expect (onStep  <= steady * 1.05f, "jump when heat turns on");
            expect (offStep <= steady * 1.05f, "jump when heat turns off");

            expectEquals (maxDiffFrom (input, output, latency, offAt + latency + (int) (0.025 * sampleRate)), 0.0f,
                          "bit-exact after heat off");
        }

        // 播放中途打开 Bypass（全部处理开满）：20 ms 交叉淡化后逐位等于延迟后的输入。
        void testBypassToggleMidStream()
        {
            beginTest ("bypass switched on mid-stream is bit-exact after the crossfade");

            constexpr double sampleRate = 48000.0;
            ColorModule color;
            prepareColor (color, sampleRate, makeParams (2.0f, 60.0f, 40.0f, 80.0f, 3.0f));

            const auto input = makeNoise (numChannels, (int) sampleRate, 0.5f, 77);
            const auto switchAt = (int) (0.5 * sampleRate);
            const auto output = runWithSwitch (color, input, switchAt, makeParams (2.0f, 60.0f, 40.0f, 80.0f, 3.0f, true));
            const auto latency = color.getLatencySamples();

            expectEquals (maxDiffFrom (input, output, latency, switchAt + (int) (0.025 * sampleRate)), 0.0f);
        }

        // 旁通时压缩没有作用到输出上：Glue 表不报数（R1-03）。
        void testNoGainReductionReportedWhenBypassed()
        {
            beginTest ("glue GR is not reported while bypassed");

            ColorModule color;
            prepareColor (color, 48000.0, makeParams (0.0f, 100.0f, 0.0f, 100.0f, 0.0f, true));
            runColor (color, makeSine (numChannels, 48000.0, 1000.0, juce::Decibels::decibelsToGain (-6.0f), 0.5), maxBlock);
            expectEquals (color.getAndResetGainReductionDb(), 0.0f);
        }

        // 热输入下 Heat 从 0 到 100 的稳态响应：保留最初 0.1 的开关回归，扩展检查 10 以上不再突入全湿。
        // 各档损失逐步增大、相邻测试档变化 < 0.4 dB、满档损失 < 1.25 dB；这些是听感手势的有界护栏，
        // 不要求输出逐位符合某个 drive / mix 公式，因此可继续微调映射。
        void testHeatOnsetIsContinuous()
        {
            beginTest ("heat stays gradual from 0.1 through 100 on a -0.5 dBFS sine");

            constexpr double sampleRate = 48000.0;
            const auto input = makeSine (numChannels, sampleRate, 1000.0, juce::Decibels::decibelsToGain (-0.5f), 1.0);
            const auto measureFrom   = (int) (0.5 * sampleRate);
            const auto measureLength = (int) (0.4 * sampleRate);

            auto levelDbAt = [&] (float heat)
            {
                ColorModule color;
                prepareColor (color, sampleRate, makeParams (0.0f, 0.0f, heat, 100.0f, 0.0f));
                const auto output = runColor (color, input, maxBlock);
                expect (allFinite (output), "heat " + juce::String (heat));
                return juce::Decibels::gainToDecibels (rmsOf (output, measureFrom, measureLength));
            };

            const auto baseDb = levelDbAt (0.0f);
            double previousMagnitude = 0.0;

            struct Step { float heat; double limitDb; };

            for (const auto step : { Step { 0.1f, 0.05 }, Step { 1.0f, 0.03 }, Step { 5.0f, 0.10 },
                                     Step { 10.0f, 0.20 }, Step { 30.0f, 0.35 }, Step { 50.0f, 0.60 },
                                     Step { 75.0f, 0.90 }, Step { 100.0f, 1.25 } })
            {
                const auto change = levelDbAt (step.heat) - baseDb;
                const auto label = "heat " + juce::String (step.heat, 1);

                logMessage ("  " + label + ": level change " + juce::String (change, 4) + " dB");
                expect (std::abs (change) < step.limitDb, label + ": level change " + juce::String (change, 4)
                                                          + " dB exceeds " + juce::String (step.limitDb, 2) + " dB");
                expect (std::abs (change) >= previousMagnitude - 1.0e-4,
                        label + ": change must grow monotonically with heat");
                expect (std::abs (change) - previousMagnitude < 0.4,
                        label + ": adjacent test settings must not introduce a level cliff");
                previousMagnitude = std::abs (change);
            }
        }

        // Glue 从完全关掉重新打开不陷坑：−6 dBFS 1 kHz 正弦，Glue 100（0.5 s，深压）→ 0（0.5 s，淡出早已走完）→ 1；
        // 重开后 500 ms 内每 10 ms 窗（10 个整周期）的 RMS 都不低于「一直是 Glue 1」的稳态 − 0.2 dB。
        // 依据：2026-09-25 评审 v0.4 R1-03——关着时检测器按旧映射在跑、包络停在旧的压缩量上，修复前重开会沿用旧映射
        // 与旧包络，先陷下去再慢慢释放；修复后从完全关掉重开按「从没开过」起步（ColorModule::restartGlueFromOff）。
        void testGlueReenableFromOffHasNoDip()
        {
            beginTest ("glue re-enabled after being fully off starts fresh: no dip below steady - 0.2 dB");

            constexpr double sampleRate = 48000.0;
            constexpr int window = 480;   // 10 ms = 1 kHz 的 10 个整周期
            const auto amplitude = juce::Decibels::decibelsToGain (-6.0f);

            // 参照：一直是 Glue 1，取最后 200 ms 的稳态电平。
            double steadyDb = 0.0;
            {
                ColorModule reference;
                prepareColor (reference, sampleRate, makeParams (0.0f, 1.0f, 0.0f, 100.0f, 0.0f));
                const auto output = runColor (reference, makeSine (numChannels, sampleRate, 1000.0, amplitude, 1.0), maxBlock);
                steadyDb = juce::Decibels::gainToDecibels (rmsOf (output, (int) (0.8 * sampleRate), (int) (0.2 * sampleRate)));
            }

            ColorModule color;
            prepareColor (color, sampleRate, makeParams (0.0f, 100.0f, 0.0f, 100.0f, 0.0f));

            // 三段连续的正弦（相位接续）：0.5 s Glue 100 → 0.5 s Glue 0 → 0.6 s Glue 1。
            const auto whole = makeSine (numChannels, sampleRate, 1000.0, amplitude, 1.6);
            auto segment = [&whole] (double fromSeconds, double toSeconds)
            {
                Signal part;
                for (const auto& channel : whole)
                    part.emplace_back (channel.begin() + (int) (fromSeconds * sampleRate), channel.begin() + (int) (toSeconds * sampleRate));
                return part;
            };

            runColor (color, segment (0.0, 0.5), maxBlock);
            color.setParams (makeParams (0.0f, 0.0f, 0.0f, 100.0f, 0.0f));
            runColor (color, segment (0.5, 1.0), maxBlock);
            color.setParams (makeParams (0.0f, 1.0f, 0.0f, 100.0f, 0.0f));
            const auto after = runColor (color, segment (1.0, 1.6), maxBlock);

            double lowestDb = 1000.0;
            for (int start = 0; start + window <= (int) (0.5 * sampleRate); start += window)
                lowestDb = juce::jmin (lowestDb, juce::Decibels::gainToDecibels (rmsOf (after, start, window)));

            logMessage ("  steady (glue 1) " + juce::String (steadyDb, 3) + " dB; lowest 10 ms window after re-enable "
                        + juce::String (lowestDb, 3) + " dB");
            expect (allFinite (after));
            expect (lowestDb >= steadyDb - 0.2, "dip after re-enable: lowest " + juce::String (lowestDb, 3)
                                                + " dB vs steady " + juce::String (steadyDb, 3) + " dB");
        }

        //==============================================================================
        // v0.5（研究 P3 R1 ～ R4、实现 I2）追加的用例

        struct HalfBandStage
        {
            float transitionUp, stopbandUp, transitionDown, stopbandDown;
        };

        // Color 的 Heat 用三级（8×，延迟 64）；Limit 的 Clip 级用前两级（4×，延迟 60，参数与 Heat 前两级相同）。
        static constexpr HalfBandStage halfBandStages[3] = { { 0.05f, -90.0f, 0.06f, -75.0f },
                                                              { 0.10f, -80.0f, 0.12f, -70.0f },
                                                              { 0.10f, -65.0f, 0.15f, -55.0f } };

        static void addStages (HalfBandOversampler& oversampler, int numStages)
        {
            for (int s = 0; s < numStages; ++s)
            {
                const auto& stage = halfBandStages[s];
                oversampler.addOversamplingStage (stage.transitionUp, stage.stopbandUp, stage.transitionDown, stage.stopbandDown);
            }
        }

        static void addStages (juce::dsp::Oversampling<float>& oversampler, int numStages)
        {
            oversampler.clearOversamplingStages();

            for (int s = 0; s < numStages; ++s)
            {
                const auto& stage = halfBandStages[s];
                oversampler.addOversamplingStage (juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple,
                                                  stage.transitionUp, stage.stopbandUp, stage.transitionDown, stage.stopbandDown);
            }
        }

        using Shaper = std::function<void (float*, int)>;

        // 整段信号按块送进过采样器：上采样 →（可选）在过采样域整形 → 下采样；收集上采样输出与下采样输出。
        // 两种过采样器（自写 / JUCE）接口相同，共用这一个模板。
        template <typename Oversampler>
        static void runOversampler (Oversampler& oversampler, const Signal& input, int blockSize, const Shaper& shaper,
                                    Signal& upOut, Signal& downOut)
        {
            const auto channels = (int) input.size();
            const auto numSamples = (int) input.front().size();
            upOut.assign ((size_t) channels, {});
            downOut = input;

            juce::AudioBuffer<float> buffer (channels, blockSize);

            for (int start = 0; start < numSamples; start += blockSize)
            {
                const auto length = juce::jmin (blockSize, numSamples - start);

                for (int ch = 0; ch < channels; ++ch)
                    buffer.copyFrom (ch, 0, input[(size_t) ch].data() + start, length);

                juce::dsp::AudioBlock<float> block (buffer);
                auto subBlock = block.getSubBlock (0, (size_t) length);
                auto upsampled = oversampler.processSamplesUp (subBlock);

                for (int ch = 0; ch < channels; ++ch)
                {
                    auto* data = upsampled.getChannelPointer ((size_t) ch);
                    const auto count = (int) upsampled.getNumSamples();
                    upOut[(size_t) ch].insert (upOut[(size_t) ch].end(), data, data + count);

                    if (shaper)
                        shaper (data, count);
                }

                oversampler.processSamplesDown (subBlock);

                for (int ch = 0; ch < channels; ++ch)
                    std::copy (buffer.getReadPointer (ch), buffer.getReadPointer (ch) + length, downOut[(size_t) ch].begin() + start);
            }
        }

        // 保留旧版 Heat 60 的重驱动曲线（d = 6.4，b = 0.09）作为过采样器压力测试；不是当前母带映射。
        static Shaper makeHeat60Shaper (Saturator& saturator)
        {
            saturator.setCurve (Saturator::Curve::Tube);
            saturator.setDriveAndAsymmetry (1.0f + 0.6f * 9.0f, 0.6f * 0.15f);
            return [&saturator] (float* data, int count) { saturator.processTubeBlock (data, count); };
        }

        // HalfBandOversampler 与 juce::dsp::Oversampling（filterHalfBandFIREquiripple，同参数）对同一段 0.99 幅白噪声：
        // 上采样输出、下采样输出（中间不整形 / 用 Heat 60 的 Tube 整形）的差都 < −120 dBFS；倍率、延迟完全相同且是整数。
        // 依据：v0.5 计划 I2「同系数、同线性相位、同整数延迟」、研究 P3 R1「与 JUCE 输出差 < −120 dBFS」。
        // 顺带核对自写实现依赖的系数结构：JUCE 设计函数给出的半带长 4n + 3、中心抽头恰为 0.5、偶数位左右对称
        // （JUCE 自己的 Oversampling 也只用这些；除中心外的奇数位理论上为 0，两边都不读）。
        void testHalfBandMatchesJuceOversampling()
        {
            beginTest ("HalfBandOversampler matches juce::dsp::Oversampling (< -120 dBFS) with the same integer latency");

            for (const auto& stage : halfBandStages)
            {
                for (const auto& design : { std::make_pair (stage.transitionUp, stage.stopbandUp),
                                            std::make_pair (stage.transitionDown, stage.stopbandDown) })
                {
                    const auto fir = juce::dsp::FilterDesign<float>::designFIRLowpassHalfBandEquirippleMethod (design.first, design.second);
                    const auto* h = fir->getRawCoefficients();
                    const auto length = (int) fir->getFilterOrder() + 1;
                    const auto n = (length - 3) / 4;

                    bool structureOk = length == 4 * n + 3 && h[2 * n + 1] == 0.5f;

                    for (int m = 0; m <= n; ++m)
                        structureOk = structureOk && h[2 * m] == h[length - 1 - 2 * m];

                    expect (structureOk, "half-band structure for " + juce::String (design.first) + " / "
                                         + juce::String (design.second) + " dB");
                }
            }

            const auto input = makeNoise (numChannels, 48000, 0.99f, 7);

            for (int numStages : { 3, 2 })
            {
                juce::dsp::Oversampling<float> reference ((size_t) numChannels);
                HalfBandOversampler mine ((size_t) numChannels);
                addStages (reference, numStages);
                addStages (mine, numStages);
                reference.initProcessing ((size_t) maxBlock);
                mine.initProcessing ((size_t) maxBlock);

                const auto label = juce::String (numStages) + " stages";
                expectEquals ((int) mine.getOversamplingFactor(), (int) reference.getOversamplingFactor(), label);
                expectEquals ((int) mine.getNumStages(), numStages, label);
                expectEquals (mine.getLatencyInSamples(), reference.getLatencyInSamples(), "latency, " + label);
                expectEquals (mine.getLatencyInSamples(), numStages == 3 ? 64.0f : 60.0f, "integer latency, " + label);

                for (bool shaped : { false, true })
                {
                    reference.reset();
                    mine.reset();

                    Saturator saturatorA, saturatorB;
                    const auto shaperA = shaped ? makeHeat60Shaper (saturatorA) : Shaper();
                    const auto shaperB = shaped ? makeHeat60Shaper (saturatorB) : Shaper();

                    Signal upReference, downReference, upMine, downMine;
                    runOversampler (reference, input, maxBlock, shaperA, upReference, downReference);
                    runOversampler (mine, input, maxBlock, shaperB, upMine, downMine);

                    const auto upDiff = maxAbsDifference (upReference, upMine, 0, (int) upReference.front().size());
                    const auto downDiff = maxAbsDifference (downReference, downMine, 0, (int) downReference.front().size());
                    const auto what = label + (shaped ? ", tube between" : ", plain");

                    logMessage ("  " + what + ": up " + juce::String (toDb (upDiff), 1) + " dBFS, down "
                                + juce::String (toDb (downDiff), 1) + " dBFS");
                    expect (upDiff < nullThreshold, "upsampled differs by " + juce::String (toDb (upDiff), 1) + " dBFS, " + what);
                    expect (downDiff < nullThreshold, "downsampled differs by " + juce::String (toDb (downDiff), 1) + " dBFS, " + what);
                }
            }
        }

        // 块长无关：同一段输入按块长 512 / 1 / 17 / 333 / 4096 处理（中间整段 Tube 整形），上采样与下采样输出逐位相同。
        // 依据：v0.5 计划 I2「任何块长同一段代码」「块长无关逐位」——ColorModule「输出不受块大小影响」建立在它上面。
        void testHalfBandBlockSizeIndependence()
        {
            beginTest ("HalfBandOversampler output is bit-identical for any block size");

            const auto input = makeNoise (numChannels, 24000, 0.99f, 5);

            for (int numStages : { 3, 2 })
            {
                Signal referenceUp, referenceDown;

                for (int blockSize : { 512, 1, 17, 333, 4096 })
                {
                    HalfBandOversampler oversampler ((size_t) numChannels);
                    addStages (oversampler, numStages);
                    oversampler.initProcessing (4096);

                    Saturator saturator;
                    Signal up, down;
                    runOversampler (oversampler, input, blockSize, makeHeat60Shaper (saturator), up, down);

                    if (referenceDown.empty())
                    {
                        referenceUp = up;
                        referenceDown = down;
                        continue;
                    }

                    const auto label = juce::String (numStages) + " stages, block " + juce::String (blockSize);
                    expectEquals ((int) countBitDifferences (referenceUp, up), 0, "upsampled, " + label);
                    expectEquals ((int) countBitDifferences (referenceDown, down), 0, "downsampled, " + label);
                }
            }
        }

        // 预热复原：一直在跑的过采样器 vs 在第 7000 个样本处清空、只把最近 getMemoryInSamples() 个（以及 256 个）输入重跑一遍的
        // 过采样器，之后的输出逐位相同。两种配置的记忆都 ≤ 256：Color 的 Heat 预热 256、Limit 的 Clip 预滚 256 都够。
        // 依据：ColorModule 的 Heat 湿路「空闲跳过、重开预热」与一直在跑逐位相同的前提（有限记忆 + 中间整形无记忆）。
        void testHalfBandPrimingRestoresState()
        {
            beginTest ("HalfBandOversampler: re-running the last getMemoryInSamples() inputs restores its state bit-exactly");

            const auto input = makeNoise (numChannels, 12000, 0.99f, 9);
            constexpr int splitAt = 7000;

            Signal first, rest;
            for (const auto& channel : input)
            {
                first.emplace_back (channel.begin(), channel.begin() + splitAt);
                rest.emplace_back (channel.begin() + splitAt, channel.end());
            }

            for (int numStages : { 3, 2 })
            {
                int memory = 0;
                {
                    HalfBandOversampler probe ((size_t) numChannels);
                    addStages (probe, numStages);
                    memory = probe.getMemoryInSamples();
                }

                logMessage ("  " + juce::String (numStages) + " stages: memory " + juce::String (memory) + " samples");
                expect (memory > 0 && memory <= 256, "memory " + juce::String (memory) + " must fit the 256-sample priming");

                for (int primeLength : { memory, 256 })
                {
                    HalfBandOversampler continuous ((size_t) numChannels), primed ((size_t) numChannels);
                    addStages (continuous, numStages);
                    addStages (primed, numStages);
                    continuous.initProcessing ((size_t) maxBlock);
                    primed.initProcessing ((size_t) maxBlock);

                    Saturator saturatorA, saturatorB;
                    const auto shaperA = makeHeat60Shaper (saturatorA);
                    const auto shaperB = makeHeat60Shaper (saturatorB);
                    Signal up, down;

                    runOversampler (continuous, first, 256, shaperA, up, down);
                    runOversampler (primed, first, 256, shaperB, up, down);

                    // 清空后只知道最近 primeLength 个输入：按原顺序重跑一遍（输出丢掉）。
                    primed.reset();
                    Signal prime;
                    for (const auto& channel : first)
                        prime.emplace_back (channel.end() - primeLength, channel.end());

                    runOversampler (primed, prime, 64, shaperB, up, down);

                    Signal upContinuous, downContinuous, upPrimed, downPrimed;
                    runOversampler (continuous, rest, 97, shaperA, upContinuous, downContinuous);
                    runOversampler (primed, rest, 97, shaperB, upPrimed, downPrimed);

                    const auto label = juce::String (numStages) + " stages, primed with " + juce::String (primeLength);
                    expectEquals ((int) countBitDifferences (upContinuous, upPrimed), 0, "upsampled, " + label);
                    expectEquals ((int) countBitDifferences (downContinuous, downPrimed), 0, "downsampled, " + label);
                }
            }
        }

        // v0.4 Saturator 的逐字副本（只留 processSample 用到的部分，函数体与 v0.4 一字不差）：
        // v0.5 给 Saturator 加了整段接口，但原 processSample 必须逐位不变——Limit 的 Clip 级（SoftClip）在用。
        struct SaturatorV04
        {
            bool isTube = false;
            float threshold = 1.0f, kneeStart = 0.7f, kneeWidth = 0.3f;
            float drive = 1.0f, asymmetry = 0.0f, tanhOfAsymmetry = 0.0f, inverseSlopeAtZero = 1.0f;

            void setDrive (float newDrive)          { drive = juce::jmax (1.0f, newDrive); updateTube(); }
            void setAsymmetry (float newAsymmetry)  { asymmetry = newAsymmetry; updateTube(); }

            void setThreshold (float newThreshold)
            {
                threshold = juce::jmax (1.0e-6f, newThreshold);
                kneeStart = threshold * 0.7f;
                kneeWidth = threshold - kneeStart;
            }

            void updateTube()
            {
                tanhOfAsymmetry = std::tanh (asymmetry);
                const auto slopeAtZero = drive * (1.0f - tanhOfAsymmetry * tanhOfAsymmetry);
                inverseSlopeAtZero = 1.0f / slopeAtZero;
            }

            float softClip (float x) const
            {
                const auto magnitude = std::abs (x);

                if (magnitude <= kneeStart)
                    return x;

                const auto shaped = kneeStart + kneeWidth * std::tanh ((magnitude - kneeStart) / kneeWidth);
                return std::copysign (shaped, x);
            }

            float tube (float x) const
            {
                return (std::tanh (drive * x + asymmetry) - tanhOfAsymmetry) * inverseSlopeAtZero;
            }

            float processSample (float x) const   { return isTube ? tube (x) : softClip (x); }
        };

        static std::vector<float> saturatorTestInputs()
        {
            std::vector<float> values;

            for (int k = -3072; k <= 3072; ++k)
                values.push_back ((float) k / 1024.0f);   // −3 … 3，步长 1/1024

            juce::Random random (31);
            for (int k = 0; k < 4000; ++k)
                values.push_back ((random.nextFloat() * 2.0f - 1.0f) * 2.5f);

            values.insert (values.end(), { 0.0f, -0.0f, std::numeric_limits<float>::denorm_min(), -std::numeric_limits<float>::denorm_min(),
                                           1.0e30f, -1.0e30f, std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(),
                                           std::numeric_limits<float>::quiet_NaN() });
            return values;
        }

        // 原 processSample 逐位不变：SoftClip（4 个阈值）与 Tube（6 档旧版 Heat 驱动，两种设参方式）对 v0.4 副本按位比较，
        // 输入含 ±0、非规格化数、±1e30、±Inf、NaN。依据：v0.5 计划 I2「Saturator 块式快速 tanh，原 processSample 逐位不变」。
        void testSaturatorProcessSampleUnchanged()
        {
            beginTest ("Saturator::processSample is bit-identical to v0.4 (soft clip and tube)");

            const auto inputs = saturatorTestInputs();

            for (auto threshold : { 1.0f, 0.891251f, 0.5f, 0.1f })
            {
                Saturator current;
                current.setCurve (Saturator::Curve::SoftClip);
                current.setThreshold (threshold);

                SaturatorV04 old;
                old.setThreshold (threshold);

                int differing = 0;
                for (auto x : inputs)
                {
                    const auto a = current.processSample (x);
                    const auto b = old.processSample (x);
                    differing += std::memcmp (&a, &b, sizeof (float)) != 0 ? 1 : 0;
                }

                expectEquals (differing, 0, "soft clip, threshold " + juce::String (threshold));
            }

            for (auto heat : { 0.0f, 0.1f, 5.0f, 30.0f, 60.0f, 100.0f })
            {
                const auto h = heat / 100.0f;
                const auto drive = 1.0f + h * 9.0f;
                const auto asymmetry = h * 0.15f;

                Saturator separately, together;
                separately.setCurve (Saturator::Curve::Tube);
                together.setCurve (Saturator::Curve::Tube);
                separately.setDrive (drive);
                separately.setAsymmetry (asymmetry);
                together.setDriveAndAsymmetry (drive, asymmetry);

                SaturatorV04 old;
                old.isTube = true;
                old.setDrive (drive);
                old.setAsymmetry (asymmetry);

                int differing = 0;
                for (auto x : inputs)
                {
                    const auto reference = old.processSample (x);
                    const auto a = separately.processSample (x);
                    const auto b = together.processSample (x);
                    differing += std::memcmp (&a, &reference, sizeof (float)) != 0 ? 1 : 0;
                    differing += std::memcmp (&b, &reference, sizeof (float)) != 0 ? 1 : 0;
                }

                expectEquals (differing, 0, "tube, heat " + juce::String (heat));
            }
        }

        // 整段快速 tanh：
        //   1. fastTanh 对 double tanh 的最大绝对误差 < 4.2e-7（−12 … 12 每 1e-4 一点；逐个 float 穷举 [0, 9] 的最大值是
        //      4.10e-7，在 x ≈ 5.83），奇对称逐位成立、|y| ≤ 1；有理式在 float 下不是逐 ulp 单调的（相邻 float 最多回落 8 ulp、
        //      4.8e-7，与逼近误差同量级），所以只要求任何回落 ≤ 5e-7；±0 保号，NaN 进 NaN 出，±Inf → ±1；
        //   2. processTubeBlock 与 processSample（std::tanh）的差 < −120 dBFS（旧版 Heat 0 / 5 / 30 / 60 / 100 驱动，−1.5 … 1.5）、y(0) 恰为 0；
        //   3. 任意长度都走同一段 4 路代码：整段处理与按 1 / 2 / 3 / 5 / 7 / 13 切段处理逐位相同，fastTanhBlock 与逐个 fastTanh 逐位相同。
        // 依据：v0.5 计划 I2「块式快速 tanh」；研究 P3 R2（混叠与响度补偿不变）；与原实现的差最坏约 −128.9 dBFS（v0.5 验收 V1 扫描，
        //       即 fastTanh 自身 4.1e-7 的误差；研究估的 −135 dBFS 偏乐观），门槛 −120 dBFS 留出余量。
        void testFastTanhAccuracyAndTubeBlock()
        {
            beginTest ("fast tanh is accurate, odd, bounded and NaN / Inf safe; tube block matches processSample within -120 dBFS");

            double worstError = 0.0, worstDrop = 0.0;
            float worstAt = 0.0f, previous = -2.0f;
            bool odd = true, bounded = true;

            for (int k = -120000; k <= 120000; ++k)
            {
                const auto x = (float) k * 1.0e-4f;
                const auto y = Saturator::fastTanh (x);
                const auto error = std::abs ((double) y - std::tanh ((double) x));

                if (error > worstError)
                {
                    worstError = error;
                    worstAt = x;
                }

                const auto mirrored = Saturator::fastTanh (-x);
                const auto negated = -y;
                odd = odd && std::memcmp (&mirrored, &negated, sizeof (float)) == 0;
                bounded = bounded && std::abs (y) <= 1.0f;
                worstDrop = juce::jmax (worstDrop, (double) previous - (double) y);
                previous = y;
            }

            logMessage ("  fastTanh max |error| " + juce::String (worstError, 10) + " at x = " + juce::String (worstAt, 4)
                        + ", largest drop between grid points " + juce::String (worstDrop, 10));
            expect (worstError < 4.2e-7, "fastTanh error " + juce::String (worstError, 10));
            expect (odd, "fastTanh must be odd (bit-exact)");
            expect (bounded, "fastTanh must stay within [-1, 1]");
            expect (worstDrop <= 5.0e-7, "fastTanh must be monotonic within its approximation error, drop " + juce::String (worstDrop, 10));

            const auto plusZero = Saturator::fastTanh (0.0f);
            const auto minusZero = Saturator::fastTanh (-0.0f);
            expect (plusZero == 0.0f && ! std::signbit (plusZero), "fastTanh (+0) = +0");
            expect (minusZero == 0.0f && std::signbit (minusZero), "fastTanh (-0) = -0");
            expect (std::isnan (Saturator::fastTanh (std::numeric_limits<float>::quiet_NaN())), "fastTanh (NaN) = NaN");
            expectEquals (Saturator::fastTanh (std::numeric_limits<float>::infinity()), 1.0f, "fastTanh (+Inf)");
            expectEquals (Saturator::fastTanh (-std::numeric_limits<float>::infinity()), -1.0f, "fastTanh (-Inf)");

            for (auto heat : { 0.0f, 5.0f, 30.0f, 60.0f, 100.0f })
            {
                const auto h = heat / 100.0f;
                Saturator tube;
                tube.setCurve (Saturator::Curve::Tube);
                tube.setDriveAndAsymmetry (1.0f + h * 9.0f, h * 0.15f);

                std::vector<float> block;
                for (int k = -15000; k <= 15000; ++k)
                    block.push_back ((float) k * 1.0e-4f);

                const auto original = block;
                tube.processTubeBlock (block.data(), (int) block.size());

                float worst = 0.0f;
                for (size_t i = 0; i < block.size(); ++i)
                    worst = juce::jmax (worst, std::abs (block[i] - tube.processSample (original[i])));

                logMessage ("  tube block vs processSample, heat " + juce::String (heat) + ": " + juce::String (toDb (worst), 1) + " dBFS");
                expect (worst < nullThreshold, "tube block differs by " + juce::String (toDb (worst), 1) + " dBFS at heat " + juce::String (heat));

                float zero = 0.0f;
                tube.processTubeBlock (&zero, 1);
                expectEquals (zero, 0.0f, "tube block maps 0 to exactly 0 at heat " + juce::String (heat));
            }

            {
                Saturator tube;
                tube.setCurve (Saturator::Curve::Tube);
                tube.setDriveAndAsymmetry (6.4f, 0.09f);

                std::vector<float> data (1000);
                juce::Random random (17);
                for (auto& v : data)
                    v = random.nextFloat() * 2.0f - 1.0f;

                auto whole = data;
                tube.processTubeBlock (whole.data(), (int) whole.size());

                auto pieces = data;
                const int lengths[] = { 1, 2, 3, 5, 7, 13 };
                int position = 0, which = 0;
                while (position < (int) pieces.size())
                {
                    const auto length = juce::jmin (lengths[which++ % 6], (int) pieces.size() - position);
                    tube.processTubeBlock (pieces.data() + position, length);
                    position += length;
                }

                expect (std::memcmp (whole.data(), pieces.data(), sizeof (float) * whole.size()) == 0,
                        "tube block result must not depend on where a sample sits in the block");

                auto viaBlock = data;
                Saturator::fastTanhBlock (viaBlock.data(), (int) viaBlock.size());
                int differing = 0;
                for (size_t i = 0; i < data.size(); ++i)
                {
                    const auto single = Saturator::fastTanh (data[i]);
                    differing += std::memcmp (&single, &viaBlock[i], sizeof (float)) != 0 ? 1 : 0;
                }
                expectEquals (differing, 0, "fastTanhBlock == fastTanh per sample");
            }
        }

        // 空闲整段跳过与不跳过（setIdleSkippingEnabled (false)）逐位相同：9 个参数剧本（每一级都在空闲与工作之间来回，
        // 含 GLAZE 自动化与 GLAZE 100 → 0 → 0.1）× 44.1k / 48k / 192k × 块 512 / 64 / 17，像音乐的立体声信号。
        // 另外确认中性参数下四处跳过路径真的走到了。
        // 依据：v0.5 计划 I2「空闲整段跳过……必须保留 v0.4 的行为」；研究 P3 R3（Tone / Glue / Mix-Output-Bypass 逐位，
        // Heat 湿路靠 256 样本预热复原过采样器）。
        void testIdleSkippingIsBitExact()
        {
            beginTest ("idle skipping is bit-identical to full processing (9 scenarios x 3 rates x 3 block sizes)");

            const auto schedules = makeIdleSchedules();

            for (auto sampleRate : { 44100.0, 48000.0, 192000.0 })
            {
                for (const auto& schedule : schedules)
                {
                    const auto input = makeMusicLike (sampleRate, schedule.seconds);

                    for (auto blockSize : { 512, 64, 17 })
                    {
                        ColorModule skipping, full;
                        prepareColor (skipping, sampleRate, schedule.paramsAt (0.0));
                        prepareColor (full, sampleRate, schedule.paramsAt (0.0));
                        full.setIdleSkippingEnabled (false);

                        const auto a = runColorScheduled (skipping, input, sampleRate, blockSize, schedule.paramsAt);
                        const auto b = runColorScheduled (full, input, sampleRate, blockSize, schedule.paramsAt);

                        expectEquals ((int) countBitDifferences (a, b), 0,
                                      juce::String (schedule.name) + " @" + juce::String (sampleRate) + " block " + juce::String (blockSize));
                    }
                }
            }

            // 宿主里 processBlock 开着 FTZ / DAZ（PluginProcessor 的 ScopedNoDenormals）：输入不含非规格化数时同样逐位相同。
            // （输入里真有非规格化数时，被跳过的级不做算术、不会把它冲成 0，差 < 1.2e-38——见 ColorModule.h。）
            {
                juce::ScopedNoDenormals noDenormals;

                for (const auto& schedule : schedules)
                {
                    const auto input = makeMusicLike (48000.0, schedule.seconds);
                    ColorModule skipping, full;
                    prepareColor (skipping, 48000.0, schedule.paramsAt (0.0));
                    prepareColor (full, 48000.0, schedule.paramsAt (0.0));
                    full.setIdleSkippingEnabled (false);

                    const auto a = runColorScheduled (skipping, input, 48000.0, 64, schedule.paramsAt);
                    const auto b = runColorScheduled (full, input, 48000.0, 64, schedule.paramsAt);
                    expectEquals ((int) countBitDifferences (a, b), 0, juce::String (schedule.name) + " with flush-to-zero");
                }
            }

            ColorModule color;
            prepareColor (color, 48000.0, neutralParams());
            runColor (color, makeMusicLike (48000.0, 0.1), maxBlock);
            const auto idle = color.getLastIdleState();
            expect (idle.tone && idle.glue && idle.heatWet && idle.output, "all four idle paths must be taken with neutral parameters");
        }

        void testHeatIdleAfterPartialFadeBlock()
        {
            beginTest ("Heat 0 / 60 with 333-sample blocks preserves the partial fade and delayed tail before idle skipping");
            constexpr double sampleRate = 48000.0;
            constexpr int blockSize = 333;
            constexpr int blockCount = 110;
            const auto input = makeMusicLike (sampleRate, (double) (blockSize * blockCount) / sampleRate, 20260927);
            const Schedule schedule = [] (double t)
            {
                const auto sampleIndex = (std::int64_t) std::llround (t * sampleRate);
                const auto blockIndex = (int) (sampleIndex / blockSize);
                const auto enabled = (blockIndex >= 10 && blockIndex < 30)
                                  || (blockIndex >= 34 && blockIndex < 54)
                                  || (blockIndex >= 58 && blockIndex < 78)
                                  || blockIndex >= 100;
                return makeParams (0.0f, 0.0f, enabled ? 60.0f : 0.0f, 100.0f, 0.0f);
            };
            // 20 ms = 960 samples；3×333 −960 =39，少于64样本补偿延迟，更未满足256样本预热历史。
            // 第三个淡出块完成后，下一333块不可提前视为全部空闲；随后重开会暴露被污染的DC/残差状态。
            // 两次关掉后恰好4×333就重开，避免DC误差在长静音里衰减消失；最后再覆盖长关后重开。
            ColorModule skipping, full;
            prepareColor (skipping, sampleRate, schedule (0.0));
            prepareColor (full, sampleRate, schedule (0.0));
            full.setIdleSkippingEnabled (false);
            const auto actual = runColorScheduled (skipping, input, sampleRate, blockSize, schedule);
            const auto reference = runColorScheduled (full, input, sampleRate, blockSize, schedule);
            expect (allFinite (actual) && allFinite (reference));
            expectEquals ((int) countBitDifferences (actual, reference), 0,
                          "partial fade, 64-sample tail and re-enabled Heat must be bit-identical with skipping disabled");
        }

        // 参数自动化下仍与块长无关：参数只在 512 的整数倍样本处变化（每块开头按「本块首样本向下取整到 512」的时刻取参数），
        // 块长 128 / 64 / 16 / 1 的输出与块 512 逐位相同——跳过的进入 / 退出、Heat 预热、近似链启停、平滑、系数更新计数都
        // 落在同样的绝对样本位置上。9 个参数剧本 × 44.1k / 48k。
        // 依据：修订「输出不受块大小影响」（原 testBlockSizeIndependence 只测了参数不变的情形）。注意样本号必须用整数算：
        // 用 floor (t · sr / 512) 会因 t · sr 比整数小一个 ulp 而在边界上少取一格（实现 I2 接续时查出的实验写法错误）。
        void testBlockSizeIndependenceUnderAutomation()
        {
            beginTest ("output independent of block size under automation (changes on 512-sample boundaries)");

            const auto schedules = makeIdleSchedules();

            for (auto sampleRate : { 44100.0, 48000.0 })
            {
                for (const auto& schedule : schedules)
                {
                    const auto input = makeMusicLike (sampleRate, schedule.seconds);
                    const auto& paramsAt = schedule.paramsAt;
                    const Schedule aligned = [&paramsAt, sampleRate] (double t)
                    {
                        const auto sampleIndex = (juce::int64) std::llround (t * sampleRate);
                        return paramsAt ((double) ((sampleIndex / 512) * 512) / sampleRate);
                    };

                    Signal reference;

                    for (auto blockSize : { 512, 128, 64, 16, 1 })
                    {
                        ColorModule color;
                        prepareColor (color, sampleRate, aligned (0.0));
                        const auto output = runColorScheduled (color, input, sampleRate, blockSize, aligned);

                        if (reference.empty())
                        {
                            reference = output;
                            continue;
                        }

                        expectEquals ((int) countBitDifferences (reference, output), 0,
                                      juce::String (schedule.name) + " @" + juce::String (sampleRate) + " block " + juce::String (blockSize));
                    }
                }
            }
        }

        // 特殊输入下跳过仍逐位相同：整段撒 −0（每 7 个样本）与非规格化数（每 11 个），以及单个 NaN / +Inf。
        // 直通 Tone 会把 −0 算成 +0、遇到 NaN / Inf 状态被污染（之后一直 NaN，直到处理器的 NaN 保护重置整条链）——
        // 跳过路径遇到这类块就照常处理，所以结果与不跳过按位相同（测试环境不开 FTZ）。
        void testIdleSkippingWithSpecialValues()
        {
            beginTest ("idle skipping stays bit-identical with -0, denormal, NaN and Inf input samples");

            constexpr double sampleRate = 48000.0;
            const auto base = makeMusicLike (sampleRate, 1.2);
            const auto denormal = std::numeric_limits<float>::denorm_min() * 1000.0f;

            auto withZeros = base;
            for (auto& channel : withZeros)
            {
                for (size_t i = 0; i < channel.size(); i += 7)
                    channel[i] = -0.0f;

                for (size_t i = 3; i < channel.size(); i += 11)
                    channel[i] = (i % 2 == 0) ? denormal : -denormal;
            }

            auto withNaN = base;
            withNaN[0][30000] = std::numeric_limits<float>::quiet_NaN();

            auto withInf = base;
            withInf[1][30000] = std::numeric_limits<float>::infinity();

            const auto schedules = makeIdleSchedules();

            struct Case { const char* name; const Signal* input; };
            for (const auto& testCase : { Case { "-0 / denormals", &withZeros }, Case { "NaN", &withNaN }, Case { "+Inf", &withInf } })
            {
                for (size_t which : { (size_t) 0, (size_t) 1, (size_t) 2, (size_t) 4 })   // 中性、Tone、Glue、Heat 来回
                {
                    const auto& schedule = schedules[which];

                    for (auto blockSize : { 512, 64 })
                    {
                        ColorModule skipping, full;
                        prepareColor (skipping, sampleRate, schedule.paramsAt (0.0));
                        prepareColor (full, sampleRate, schedule.paramsAt (0.0));
                        full.setIdleSkippingEnabled (false);

                        const auto a = runColorScheduled (skipping, *testCase.input, sampleRate, blockSize, schedule.paramsAt);
                        const auto b = runColorScheduled (full, *testCase.input, sampleRate, blockSize, schedule.paramsAt);

                        expectEquals ((int) countBitDifferences (a, b), 0, juce::String (testCase.name) + ", " + schedule.name
                                                                            + ", block " + juce::String (blockSize));
                    }
                }
            }
        }

        // Heat 长时间空闲（湿路整段跳过）后重开：
        //   1. 连续：−12 dBFS 440 Hz 正弦，Heat 0（1 s）→ 60（0.5 s）→ 0；44.1k / 96k / 192k × 块 512 / 64 / 17。重开前确实在跳过；
        //      开、关的交叉淡化里相邻样本跳变 ≤ 稳态 × 1.05；关掉 25 ms 后逐位等于延迟后的输入。
        //   2. 与 v0.4 的定义比（setHeatDcProxyEnabled (false) 且不跳过：5 Hz 直流阻断永远吃真实湿路）。v0.5 在 Heat 与湿路淡化都落到 0
        //      时让直流阻断吃「近似湿路」（空闲时真实湿路不算），这是与 v0.4 唯一不止舍入级的差别，只在重开后的几十毫秒里显现。
        //      Heat 0 → 60 → 0 → 30 反复开关：像音乐的信号 < −110 dBFS，满幅白噪声（最坏情形，四个采样率）< −80 dBFS。
        // 依据：研究 P3 R3d 的验收（「Heat 0 持续 ≥ 0.5 s 后重开」：与参照差 < −80 dBFS、跳变 ≤ 稳态 × 1.05）。
        void testHeatReenableAfterLongIdle()
        {
            beginTest ("heat re-enabled after a long idle stretch is continuous and stays close to the v0.4 dc-blocker definition");

            for (auto sampleRate : { 44100.0, 96000.0, 192000.0 })
            {
                const auto input = makeSine (numChannels, sampleRate, 440.0, juce::Decibels::decibelsToGain (-12.0f), 2.0);
                const auto onAt  = (int) (1.0 * sampleRate);
                const auto offAt = (int) (1.5 * sampleRate);

                for (auto blockSize : { 512, 64, 17 })
                {
                    ColorModule color;
                    prepareColor (color, sampleRate, neutralParams());

                    Signal a, b, c;
                    for (const auto& channel : input)
                    {
                        a.emplace_back (channel.begin(), channel.begin() + onAt);
                        b.emplace_back (channel.begin() + onAt, channel.begin() + offAt);
                        c.emplace_back (channel.begin() + offAt, channel.end());
                    }

                    auto output = runColor (color, a, blockSize);
                    const auto wasSkipping = color.getLastIdleState().heatWet;
                    color.setParams (makeParams (0.0f, 0.0f, 60.0f, 100.0f, 0.0f));
                    const auto ob = runColor (color, b, blockSize);
                    color.setParams (neutralParams());
                    const auto oc = runColor (color, c, blockSize);

                    for (size_t ch = 0; ch < output.size(); ++ch)
                    {
                        output[ch].insert (output[ch].end(), ob[ch].begin(), ob[ch].end());
                        output[ch].insert (output[ch].end(), oc[ch].begin(), oc[ch].end());
                    }

                    const auto label = "@" + juce::String (sampleRate) + " block " + juce::String (blockSize);
                    expect (wasSkipping, "heat wet path must be skipped before re-enable, " + label);

                    const auto latency = color.getLatencySamples();
                    const auto fade = (int) (0.03 * sampleRate);
                    const auto steady = juce::jmax (maxStep (output, onAt - 2 * fade, onAt), maxStep (output, offAt - 2 * fade, offAt));
                    const auto onStep  = maxStep (output, onAt,  onAt  + latency + fade);
                    const auto offStep = maxStep (output, offAt, offAt + latency + fade);

                    expect (onStep  <= steady * 1.05f, "jump when heat turns on after idle, " + label);
                    expect (offStep <= steady * 1.05f, "jump when heat turns off, " + label);
                    expectEquals (maxDiffFrom (input, output, latency, offAt + latency + (int) (0.025 * sampleRate)), 0.0f,
                                  "bit-exact after heat off, " + label);
                }
            }

            const Schedule toggling = [] (double t)
            {
                const auto heat = t < 0.6 ? 0.0f : t < 1.0 ? 60.0f : t < 1.6 ? 0.0f : 30.0f;
                return makeParams (0.0f, 0.0f, heat, 100.0f, 0.0f);
            };

            struct Material { const char* name; bool musicLike; double limitDbfs; };

            for (const auto& material : { Material { "music-like", true, -110.0 }, Material { "white noise 0.99", false, -80.0 } })
            {
                for (auto sampleRate : { 44100.0, 48000.0, 96000.0, 192000.0 })
                {
                    if (material.musicLike && sampleRate != 48000.0)
                        continue;

                    const auto input = material.musicLike ? makeMusicLike (sampleRate, 2.0)
                                                          : makeNoise (numChannels, (int) (2.0 * sampleRate), 0.99f, 1234);

                    ColorModule optimised, reference;
                    prepareColor (optimised, sampleRate, toggling (0.0));
                    prepareColor (reference, sampleRate, toggling (0.0));
                    reference.setIdleSkippingEnabled (false);
                    reference.setHeatDcProxyEnabled (false);

                    const auto a = runColorScheduled (optimised, input, sampleRate, maxBlock, toggling);
                    const auto b = runColorScheduled (reference, input, sampleRate, maxBlock, toggling);

                    const auto diffDb = toDb (maxAbsDifference (a, b, 0, (int) a.front().size()));
                    const auto label = juce::String (material.name) + " @" + juce::String (sampleRate);
                    logMessage ("  vs v0.4 dc-blocker definition, " + label + ": " + juce::String (diffDb, 1) + " dBFS");
                    expect (allFinite (a));
                    expect (diffDb < material.limitDbfs, label + ": " + juce::String (diffDb, 1) + " dBFS (limit "
                                                         + juce::String (material.limitDbfs, 0) + ")");
                }
            }
        }

        // 立体声（L = R）与单声道逐位相同：ARM 上 Tone 搁架与 Glue 侧链高通把两声道打包成 float64x2（显式融合乘加），
        // 单声道走标量 processBiquad（编译器在同一表达式内合并乘加）——两者逐位相同，打包写法才与 v0.4 的标量写法一致。
        // 剧本把 Tone / Glue / Heat / Mix / Output 都来回拨，空闲跳过也一并走到；块 512 与 17。
        void testStereoMatchesMono()
        {
            beginTest ("stereo with L = R is bit-identical to mono (packed stereo biquads == scalar biquads)");

            constexpr double sampleRate = 48000.0;
            const auto source = makeMusicLike (sampleRate, 2.0);
            const Signal dual { source[0], source[0] };
            const Signal mono { source[0] };

            const Schedule everything = [] (double t)
            {
                if (t < 0.4) return glaze100Params();
                if (t < 0.8) return makeParams (2.0f, 70.0f, 0.0f, 80.0f, 2.0f);
                if (t < 1.2) return neutralParams();
                if (t < 1.6) return makeParams (-2.0f, 0.0f, 60.0f, 100.0f, -1.0f);
                return makeParams (3.0f, 100.0f, 100.0f, 50.0f, 0.0f);
            };

            for (auto blockSize : { 512, 17 })
            {
                ColorModule stereoColor, monoColor;
                prepareColor (stereoColor, sampleRate, everything (0.0));
                monoColor.setParams (everything (0.0));
                monoColor.prepare ({ sampleRate, (juce::uint32) maxBlock, 1u });
                monoColor.setParams (everything (0.0));

                const auto stereoOut = runColorScheduled (stereoColor, dual, sampleRate, blockSize, everything);
                const auto monoOut = runColorScheduled (monoColor, mono, sampleRate, blockSize, everything);

                const auto label = "block " + juce::String (blockSize);
                expectEquals ((int) countBitDifferences (Signal { stereoOut[0] }, monoOut), 0, "left vs mono, " + label);
                expectEquals ((int) countBitDifferences (Signal { stereoOut[1] }, monoOut), 0, "right vs mono, " + label);
            }
        }

        // 左右不同的立体声（makeMusicLike 两个声道各自的噪声与声像）：打包的 Tone 搁架、侧链高通、Glue 分频（AArch64 的
        // NEON、x64 的 SSE2）与 setPackedStereoForTesting (false) 的逐声道标量双二阶比，输出每个样本逐位相同。
        // 上一个测试 L = R，查不出两条通道读错、写反这类错误；这里左右不同，参数表同上（Tone、Glue、Heat 都走一遍）。
        void testPackedStereoMatchesScalar()
        {
            beginTest ("stereo with L != R: packed stereo biquads (NEON / SSE2) are bit-identical to per-channel scalar biquads");

            constexpr double sampleRate = 48000.0;
            const auto input = makeMusicLike (sampleRate, 2.0);

            const Schedule everything = [] (double t)
            {
                if (t < 0.4) return glaze100Params();
                if (t < 0.8) return makeParams (2.0f, 70.0f, 0.0f, 80.0f, 2.0f);
                if (t < 1.2) return neutralParams();
                if (t < 1.6) return makeParams (-2.0f, 0.0f, 60.0f, 100.0f, -1.0f);
                return makeParams (3.0f, 100.0f, 100.0f, 50.0f, 0.0f);
            };

            expect (countBitDifferences (Signal { input[0] }, Signal { input[1] }) > (long) input[0].size() / 2, "channels must differ");

            for (auto blockSize : { 512, 17 })
            {
                ColorModule packed, scalar;
                prepareColor (packed, sampleRate, everything (0.0));
                prepareColor (scalar, sampleRate, everything (0.0));
                scalar.setPackedStereoForTesting (false);

                const auto packedOut = runColorScheduled (packed, input, sampleRate, blockSize, everything);
                const auto scalarOut = runColorScheduled (scalar, input, sampleRate, blockSize, everything);

                const auto label = "block " + juce::String (blockSize);
                expect (countBitDifferences (packedOut, input) > 0, "processing must change the signal, " + label);
                expectEquals ((int) countBitDifferences (packedOut, scalarOut), 0, "packed vs scalar, " + label);
            }
        }

        // 音频路径零分配：每块 setParams + process，剧本覆盖 Heat 开关（空闲跳过 → 预热重开）、Glue 从完全关掉重开、Tone、
        // Mix / Output / Bypass 淡化，中途还切换两个测试开关；从 prepare 之后第一块起经 operator new 的分配次数必须为 0。
        // 依据：v0.5 计划「音频线程 0 分配」；计数见 TestMain.cpp（直接 std::malloc 的 JUCE HeapBlock 不在计数内，这里也没有）。
        void testNoAllocationsOnAudioPath()
        {
            beginTest ("setParams / process allocate nothing (skipping, priming, glue restart, test switches)");

            constexpr double sampleRate = 48000.0;
            constexpr int blockSize = 64;
            const auto input = makeMusicLike (sampleRate, 3.0);

            const Schedule schedule = [] (double t)
            {
                const auto phase = (int) (t / 0.3);
                switch (phase % 6)
                {
                    case 0:  return neutralParams();
                    case 1:  return makeParams (1.5f, 60.0f, 40.0f, 100.0f, 0.0f);
                    case 2:  return makeParams (0.0f, 0.0f, 0.0f, 70.0f, 2.0f);
                    case 3:  return makeParams (-1.0f, 80.0f, 0.0f, 100.0f, 0.0f, true);
                    case 4:  return glaze100Params();
                    default: return makeParams (0.0f, 0.0f, 0.0f, 100.0f, 0.0f);
                }
            };

            ColorModule color;
            prepareColor (color, sampleRate, schedule (0.0));

            juce::AudioBuffer<float> block (numChannels, blockSize);
            const auto numSamples = (int) input.front().size();
            const auto before = allocationCountOnThisThread();

            for (int start = 0, index = 0; start + blockSize <= numSamples; start += blockSize, ++index)
            {
                for (int ch = 0; ch < numChannels; ++ch)
                    block.copyFrom (ch, 0, input[(size_t) ch].data() + start, blockSize);

                if (index == 500)  color.setIdleSkippingEnabled (false);
                if (index == 900)  color.setIdleSkippingEnabled (true);
                if (index == 1300) color.setHeatDcProxyEnabled (false);
                if (index == 1700) color.setHeatDcProxyEnabled (true);

                color.setParams (schedule ((double) start / sampleRate));
                color.process (block);
            }

            const auto allocations = allocationCountOnThisThread() - before;
            expectEquals ((int) allocations, 0, "operator new calls on the audio path");
        }

        // 性能回归：48 kHz 立体声、块 512、像音乐的信号，只给 process() 计时，每组先空跑 0.5 s，再交替跑 5 遍 × 3 s 取最短。
        // 同批再量 juce::dsp::Oversampling 三级（上 + 下采样、不整形）当参照工作量——v0.4 的 Color 有 3/4 花在它上面，
        // 相对它的比值与机器快慢、供电状态无关。门槛（优化构建才断言，Debug 只记日志）：
        //   默认参数 < 0.5 % 且 < 参照 × 0.25；GLAZE 100 < 1.2 % 且 < 参照 × 0.6。
        //   （研究 P3 ⑤：目标 0.2 % / 0.6 %，门槛取 2 倍余量；v0.4 为 2.3 % / 2.4 %，约为参照的 1.25 倍，退回去必红。
        //    空闲跳过失效时默认参数约为参照的 0.27 倍，也会红。）
        void testPerformanceRegression()
        {
            beginTest ("performance: Color at 48 kHz / 512 stays a fraction of the v0.4 cost (idle skipping + SIMD half-band)");

            constexpr double sampleRate = 48000.0;
            constexpr int blockSize = 512;
            constexpr double seconds = 3.0;
            constexpr int numPasses = 5;

            const auto input = makeMusicLike (sampleRate, seconds);
            const auto numBlocks = (int) input.front().size() / blockSize;
            const auto measuredSeconds = (double) (numBlocks * blockSize) / sampleRate;

            ColorModule colorDefault, colorGlaze, colorNoSkip;
            prepareColor (colorDefault, sampleRate, neutralParams());
            prepareColor (colorGlaze, sampleRate, glaze100Params());
            prepareColor (colorNoSkip, sampleRate, neutralParams());
            colorNoSkip.setIdleSkippingEnabled (false);

            juce::dsp::Oversampling<float> juceOversampler ((size_t) numChannels);
            addStages (juceOversampler, 3);
            juceOversampler.initProcessing ((size_t) blockSize);

            juce::AudioBuffer<float> block (numChannels, blockSize);

            // 跑 numBlocks 块（或空跑 warmUpBlocks 块），只给处理本身计时，返回秒。
            auto timeRun = [&] (const std::function<void (juce::AudioBuffer<float>&)>& processBlock, int blocksToRun)
            {
                juce::int64 ticks = 0;

                for (int k = 0; k < blocksToRun; ++k)
                {
                    const auto start = (k % numBlocks) * blockSize;
                    for (int ch = 0; ch < numChannels; ++ch)
                        block.copyFrom (ch, 0, input[(size_t) ch].data() + start, blockSize);

                    juce::ScopedNoDenormals noDenormals;
                    const auto t0 = juce::Time::getHighResolutionTicks();
                    processBlock (block);
                    ticks += juce::Time::getHighResolutionTicks() - t0;
                }

                return juce::Time::highResolutionTicksToSeconds (ticks);
            };

            const std::function<void (juce::AudioBuffer<float>&)> workloads[] = {
                [&] (juce::AudioBuffer<float>& b) { colorDefault.process (b); },
                [&] (juce::AudioBuffer<float>& b) { colorGlaze.process (b); },
                [&] (juce::AudioBuffer<float>& b) { colorNoSkip.process (b); },
                [&] (juce::AudioBuffer<float>& b)
                {
                    juce::dsp::AudioBlock<float> audioBlock (b);
                    auto upsampled = juceOversampler.processSamplesUp (audioBlock);
                    juce::ignoreUnused (upsampled);
                    juceOversampler.processSamplesDown (audioBlock);
                }
            };
            const char* names[] = { "Color default", "Color GLAZE 100", "Color default, skipping off", "juce Oversampling 3 stages (reference)" };
            constexpr int numWorkloads = 4;

            const auto warmUpBlocks = (int) (0.5 * sampleRate) / blockSize;
            for (const auto& workload : workloads)
                timeRun (workload, warmUpBlocks);

            double best[numWorkloads];
            for (auto& b : best)
                b = 1.0e30;

            for (int pass = 0; pass < numPasses; ++pass)
                for (int w = 0; w < numWorkloads; ++w)
                    best[w] = juce::jmin (best[w], timeRun (workloads[w], numBlocks));

            double loadPercent[numWorkloads];
            for (int w = 0; w < numWorkloads; ++w)
            {
                loadPercent[w] = 100.0 * best[w] / measuredSeconds;
                logMessage ("  " + juce::String (names[w]) + ": " + juce::String (loadPercent[w], 3) + " % of one core");
            }

            const auto reference = loadPercent[3];
            const auto defaultRatio = loadPercent[0] / reference;
            const auto glazeRatio = loadPercent[1] / reference;
            logMessage ("  ratio to reference: default " + juce::String (defaultRatio, 3) + ", GLAZE 100 " + juce::String (glazeRatio, 3)
                        + ", skipping off " + juce::String (loadPercent[2] / reference, 3));

           #if JUCE_DEBUG
            logMessage ("  (debug build: performance limits not asserted)");
           #else
            expect (loadPercent[0] < 0.5, "Color default " + juce::String (loadPercent[0], 3) + " % (limit 0.5 %)");
            expect (loadPercent[1] < 1.2, "Color GLAZE 100 " + juce::String (loadPercent[1], 3) + " % (limit 1.2 %)");
            expect (defaultRatio < 0.25, "Color default / reference " + juce::String (defaultRatio, 3) + " (limit 0.25)");
            expect (glazeRatio < 0.6, "Color GLAZE 100 / reference " + juce::String (glazeRatio, 3) + " (limit 0.6)");
           #endif
        }

        //==============================================================================
        // T-032（2026-09-29，深度研究 R3 / R8 的 Color 部分）追加的用例

        // 输出指纹：固定素材 / 参数剧本下整段输出位模式的 FNV-1a 64 位散列，打进日志，并断言两个新实例得到同一指纹（确定性）。
        // 用途：做「逐位不变」的改动（如 R3-04 打包 M/S 高通、残差直接相减）时对照改前改后的日志——指纹相同即整段逐位相同；
        // 有意改输出的改动（如 R3-01）只该动 Glue 开着的场景。场景 1 ～ 3 Glue 关（Tone / Heat / Mix / Output 与 Heat 开关在动，
        // 走到预热与近似链），场景 4 ～ 5 Glue 开。块长 333，让尾巴也走到。
        void testOutputFingerprint()
        {
            beginTest ("output fingerprint (FNV-1a of the output bits, logged; identical across two fresh instances)");

            constexpr double sampleRate = 48000.0;

            struct Scenario { const char* name; Schedule paramsAt; double seconds; };
            const Scenario scenarios[] = {
                { "heat 100 / tone +2 / glue off",             [] (double) { return makeParams (2.0f, 0.0f, 100.0f, 100.0f, 0.0f); }, 1.0 },
                { "heat 60 / tone -1.5 / mix 70 / out +3 / glue off", [] (double) { return makeParams (-1.5f, 0.0f, 60.0f, 70.0f, 3.0f); }, 1.0 },
                { "heat 0/60 every 0.4 s / glue off",         [] (double t) { return makeParams (0.0f, 0.0f, ((int) (t / 0.4)) % 2 == 1 ? 60.0f : 0.0f, 100.0f, 0.0f); }, 1.6 },
                { "glaze 100 (tone 0.75 / glue 40 / heat 30)", [] (double) { return glaze100Params(); }, 1.0 },
                { "glue 100 / heat 0",                         [] (double) { return makeParams (0.0f, 100.0f, 0.0f, 100.0f, 0.0f); }, 1.0 },
            };

            for (const auto& scenario : scenarios)
            {
                const auto input = makeMusicLike (sampleRate, scenario.seconds, 20260929);
                std::uint64_t hashes[2] = {};

                for (auto& hash : hashes)
                {
                    ColorModule color;
                    prepareColor (color, sampleRate, scenario.paramsAt (0.0));
                    hash = fingerprintOf (runColorScheduled (color, input, sampleRate, 333, scenario.paramsAt));
                }

                logMessage ("  " + juce::String (scenario.name) + ": " + juce::String::toHexString ((juce::int64) hashes[0]));
                expect (hashes[0] == hashes[1], juce::String (scenario.name) + ": two fresh instances must produce the same output");
            }
        }

        // R3-01：Glue 实际施加的增益必须跟着联动静态曲线走。分频施压（2026-09-28）只允许低 / 高两路在自己更轻时少挨压；
        // 对纯音，中频（180 Hz ～ 5 kHz）本该拿到「补偿 − 联动压缩量」这一个增益。修前 mid 检测器 = 侧链 − 低 − 高 不是互补分频
        // （二阶 Butterworth 在交叉点相移 −90°），读数在 229 Hz / 3969 Hz 附近比整段高 2.1 dB，中频包络比联动深约 1 dB、
        // 再被 −0.6 dB 钳位；加上低 / 高带 +3.2 dB 的抬升经二阶分裂在 300 Hz / 3 kHz 附近反相叠加（−0.77 dB 凹陷），
        // −6 dBFS 纯音在 Glue 100 下比联动多压到 1.13 dB、Glue 40（GLAZE 100 对应值）下多压 0.64 dB——而联动 GR 才 1.3 dB，
        // 表也不显示。修后中频直接用联动增益，剩下的偏差只来自分裂的相位凹陷：Glue 100 最坏 −0.65 dB（2.5 kHz）、Glue 40 ≤ 0.14 dB。
        // 量法：48 kHz、−6 dBFS 正弦 2 s，取最后 46 块 × 512（约 0.49 s）：输出 / 输入基波幅度（相干投影，不受 64 样本延迟影响）
        // 与「补偿 − 同一段按块长加权的平均压缩量 getBlockStats().glueGrMeanDb」之差。不用 getAndResetGainReductionDb：
        // 它是窗口内最大值，正弦包络的纹波会把偏差读大（核验 V3-01）。
        void testGlueAppliedGainTracksLinkedCurve()
        {
            beginTest ("glue applied gain tracks the linked static curve (mid band has no detector of its own)");

            constexpr double sampleRate = 48000.0;
            constexpr double seconds = 2.0;
            constexpr int measureFirstBlock = 141;   // 141 × 512 = 72192 ≈ 1.504 s：释放自适应（500 ms 平均）早已稳定
            constexpr int measureBlocks = 46;        // 46 × 512 = 23552 ≈ 0.49 s
            constexpr int measureStart = measureFirstBlock * maxBlock;
            constexpr int measureLength = measureBlocks * maxBlock;
            const auto amplitude = juce::Decibels::decibelsToGain (-6.0f);

            struct Setting { float glue; double limitDb; };

            for (const auto setting : { Setting { 100.0f, 0.75 }, Setting { 40.0f, 0.30 } })
            {
                // 自动补偿 = (0 − 阈值) × (1 − 1/2) × 0.5，阈值 = −24 × Glue / 100（规格「各级定义」）。
                const auto makeupDb = 24.0 * (double) setting.glue / 100.0 * 0.5 * 0.5;
                double worstDeviation = 0.0, worstFrequency = 0.0;

                for (auto frequency : { 60.0, 90.0, 150.0, 229.0, 300.0, 500.0, 1000.0, 2500.0, 3969.0, 7000.0, 10000.0 })
                {
                    const auto input = makeSine (numChannels, sampleRate, frequency, amplitude, seconds);
                    const auto numSamples = (int) input.front().size();
                    auto output = input;

                    ColorModule color;
                    prepareColor (color, sampleRate, makeParams (0.0f, setting.glue, 0.0f, 100.0f, 0.0f));

                    juce::AudioBuffer<float> block (numChannels, maxBlock);
                    double reductionSum = 0.0;
                    int reductionSamples = 0;

                    for (int start = 0, index = 0; start < numSamples; start += maxBlock, ++index)
                    {
                        const auto length = juce::jmin (maxBlock, numSamples - start);
                        block.setSize (numChannels, length, false, false, true);

                        for (int ch = 0; ch < numChannels; ++ch)
                            block.copyFrom (ch, 0, input[(size_t) ch].data() + start, length);

                        color.process (block);

                        for (int ch = 0; ch < numChannels; ++ch)
                            std::copy (block.getReadPointer (ch), block.getReadPointer (ch) + length, output[(size_t) ch].begin() + start);

                        if (index >= measureFirstBlock && index < measureFirstBlock + measureBlocks)
                        {
                            reductionSum += (double) color.getBlockStats().glueGrMeanDb * length;
                            reductionSamples += length;
                        }
                    }

                    const auto meanReductionDb = reductionSum / juce::jmax (1, reductionSamples);
                    const auto expectedGainDb = makeupDb - meanReductionDb;
                    const auto inputAmplitude = toneAmplitude (input[0], measureStart, measureLength, sampleRate, frequency);
                    const auto outputAmplitude = toneAmplitude (output[0], measureStart, measureLength, sampleRate, frequency);
                    const auto appliedGainDb = (double) toDb (outputAmplitude / inputAmplitude);
                    const auto deviation = appliedGainDb - expectedGainDb;
                    const auto label = "glue " + juce::String (setting.glue, 0) + " @" + juce::String (frequency, 0) + " Hz";

                    logMessage ("  " + label + ": linked GR " + juce::String (meanReductionDb, 3) + " dB, expected "
                                + juce::String (expectedGainDb, 3) + " dB, applied " + juce::String (appliedGainDb, 3)
                                + " dB, deviation " + juce::String (deviation, 3) + " dB");
                    expect (allFinite (output), label);

                    // 侧链先过 100 Hz 高通：Glue 40（阈值 −9.6 dBFS、拐点起点 −12.6 dBFS）下 60 / 90 Hz 的 −6 dBFS 纯音被挡在拐点之下，
                    // 联动本来就不压（0 / 0.12 dB）；那两点只核对「不压时增益 = 补偿」，其余频率联动必须在工作。
                    if (frequency >= 150.0)
                        expect (meanReductionDb > 0.5, label + ": the linked compressor must be working");

                    expect (std::abs (deviation) < setting.limitDb, label + ": applied gain deviates from the linked curve by "
                                                                      + juce::String (deviation, 3) + " dB (limit " + juce::String (setting.limitDb, 2) + ")");

                    if (std::abs (deviation) > std::abs (worstDeviation))
                    {
                        worstDeviation = deviation;
                        worstFrequency = frequency;
                    }
                }

                logMessage ("  glue " + juce::String (setting.glue, 0) + ": worst deviation " + juce::String (worstDeviation, 3)
                            + " dB @" + juce::String (worstFrequency, 0) + " Hz");
            }
        }

        // R3-04：Saturator::processTubeResidualBlock (x) 与「processTubeBlock (x) 再逐样本减 x」逐位相同——4 路里 tanh4 的结果
        // 先舍入成 float，再减同一个原样本，是同一个 IEEE 单精度减法、同一对操作数，只是省掉一次拷贝与第二遍循环
        // （ColorModule::processResidualBlock 改前就是「拷贝 → processTubeBlock → 逐样本相减」）。输入含 ±0、非规格化、±1e30、
        // ±Inf、NaN（saturatorTestInputs）与随机块；整段与按 1 / 2 / 3 / 5 / 7 / 13 切段；驱动量取现行映射 Heat 0 / 5 / 30 / 60 / 100
        // 的 Mid（1 + h、0.04h）与 Side（1 + 0.5h、奇对称），外加旧版重驱动（6.4 / 0.09）。
        void testTubeResidualBlockMatchesTubeMinusInput()
        {
            beginTest ("Saturator::processTubeResidualBlock is bit-identical to processTubeBlock minus the input");

            const auto special = saturatorTestInputs();
            const auto random = []
            {
                std::vector<float> values (4096);
                juce::Random rng (2026);
                for (auto& v : values)
                    v = (rng.nextFloat() * 2.0f - 1.0f) * 1.5f;
                return values;
            }();

            struct Drive { float drive, asymmetry; };
            std::vector<Drive> drives;

            for (auto heat : { 0.0f, 5.0f, 30.0f, 60.0f, 100.0f })
            {
                const auto h = heat / 100.0f;
                drives.push_back ({ 1.0f + h, 0.04f * h });      // Mid
                drives.push_back ({ 1.0f + 0.5f * h, 0.0f });    // Side
            }

            drives.push_back ({ 6.4f, 0.09f });

            for (const auto& d : drives)
            {
                Saturator tube;
                tube.setCurve (Saturator::Curve::Tube);
                tube.setDriveAndAsymmetry (d.drive, d.asymmetry);
                const auto label = "drive " + juce::String (d.drive, 3) + ", asymmetry " + juce::String (d.asymmetry, 4);

                for (const auto* source : { &special, &random })
                {
                    // 参照：整形后再逐样本减原样本。
                    auto reference = *source;
                    tube.processTubeBlock (reference.data(), (int) reference.size());
                    for (size_t i = 0; i < reference.size(); ++i)
                        reference[i] -= (*source)[i];

                    auto whole = *source;
                    tube.processTubeResidualBlock (whole.data(), (int) whole.size());
                    expect (std::memcmp (whole.data(), reference.data(), sizeof (float) * whole.size()) == 0,
                            label + ": residual block must equal tube minus input bit for bit (whole block)");

                    auto pieces = *source;
                    const int lengths[] = { 1, 2, 3, 5, 7, 13 };
                    int position = 0, which = 0;

                    while (position < (int) pieces.size())
                    {
                        const auto length = juce::jmin (lengths[which++ % 6], (int) pieces.size() - position);
                        tube.processTubeResidualBlock (pieces.data() + position, length);
                        position += length;
                    }

                    expect (std::memcmp (pieces.data(), reference.data(), sizeof (float) * pieces.size()) == 0,
                            label + ": residual block must not depend on where a sample sits in the block");
                }
            }
        }

        // R8-02：setParams 对非有限值（NaN / ±Inf）的守卫。juce::jlimit 对 NaN 两个比较都为假、原样返回：修前 Tone = NaN 让搁架
        // 系数与输出变 NaN（链逐块清零重启），Output = NaN 让 decibelsToGain 返回 0 而永久静音，Heat = NaN 的补偿表索引越界读；
        // APVTS 各层（"nan" 文本、NormalisableRange、setDenormalisedValue）都不拦，损坏 / 手改的工程文件会把它原样送到模块。
        // 守卫：每个 float 字段 非有限 → Params{} 的默认值，有限值仍按范围钳制（范围内恒等，逐位不变）。
        // 用例：① 每个字段分别喂 NaN / +Inf / −Inf——prepare 前设、播放中途切换两种——输出有限、非静音，并与「该字段取默认值」
        // 的模块逐位相同；② 越界的有限值（Glue 150、Tone −10 …）仍与钳到边界的模块逐位相同（钳制路径没被改掉）。
        void testNonFiniteParamsFallBackToDefaults()
        {
            beginTest ("non-finite parameters fall back to defaults and finite ones still clamp (bit-exact vs the sanitised module)");

            constexpr double sampleRate = 48000.0;
            constexpr int blockSize = 64;
            const auto input = makeMusicLike (sampleRate, 0.6, 4711);
            const auto numSamples = (int) input.front().size();
            const auto switchAt = numSamples / 2;
            const auto base = makeParams (1.5f, 60.0f, 40.0f, 80.0f, 2.0f);   // 每个字段都不在默认值上，回默认后差别可见

            using Field = float ColorModule::Params::*;
            struct FieldCase { const char* name; Field field; };
            const FieldCase fields[] = { { "toneDb", &ColorModule::Params::toneDb }, { "glue", &ColorModule::Params::glue },
                                         { "heat", &ColorModule::Params::heat }, { "mixPercent", &ColorModule::Params::mixPercent },
                                         { "outputDb", &ColorModule::Params::outputDb } };
            struct ValueCase { const char* name; float value; };
            const ValueCase values[] = { { "NaN", std::numeric_limits<float>::quiet_NaN() },
                                         { "+Inf", std::numeric_limits<float>::infinity() },
                                         { "-Inf", -std::numeric_limits<float>::infinity() } };

            // 同一模块：前半段按 before，中途 setParams (after)（同 runWithSwitch，块长 64）。
            auto runSwitched = [&] (const ColorModule::Params& before, const ColorModule::Params& after)
            {
                ColorModule color;
                prepareColor (color, sampleRate, before);

                Signal first, second;
                for (const auto& channel : input)
                {
                    first.emplace_back (channel.begin(), channel.begin() + switchAt);
                    second.emplace_back (channel.begin() + switchAt, channel.end());
                }

                auto output = runColor (color, first, blockSize);
                color.setParams (after);
                const auto tail = runColor (color, second, blockSize);

                for (size_t ch = 0; ch < output.size(); ++ch)
                    output[ch].insert (output[ch].end(), tail[ch].begin(), tail[ch].end());

                return output;
            };

            for (const auto& f : fields)
            {
                auto sanitised = base;
                sanitised.*(f.field) = ColorModule::Params {}.*(f.field);

                ColorModule expectedModule;
                prepareColor (expectedModule, sampleRate, sanitised);
                const auto expectedFromStart = runColor (expectedModule, input, blockSize);
                const auto expectedSwitched = runSwitched (base, sanitised);
                expect (allFinite (expectedFromStart) && allFinite (expectedSwitched), juce::String (f.name) + ": reference must be finite");

                for (const auto& v : values)
                {
                    auto broken = base;
                    broken.*(f.field) = v.value;
                    const auto label = juce::String (f.name) + " = " + v.name;

                    ColorModule color;
                    prepareColor (color, sampleRate, broken);
                    const auto fromStart = runColor (color, input, blockSize);
                    expect (allFinite (fromStart), label + ": output must stay finite");
                    expect (rmsOf (fromStart, 0, numSamples) > 0.01, label + ": output must not be silent");
                    expectEquals ((int) countBitDifferences (fromStart, expectedFromStart), 0,
                                  label + " from the start: must equal the module with that field at its default");

                    const auto switched = runSwitched (base, broken);
                    expect (allFinite (switched), label + " mid-stream: output must stay finite");
                    expectEquals ((int) countBitDifferences (switched, expectedSwitched), 0,
                                  label + " mid-stream: must equal switching to the default value");
                }
            }

            struct ClampCase { const char* name; ColorModule::Params outOfRange, clamped; };
            const ClampCase clamps[] = {
                { "above range", makeParams (10.0f, 150.0f, 250.0f, 130.0f, 9.0f), makeParams (3.0f, 100.0f, 100.0f, 100.0f, 6.0f) },
                { "below range", makeParams (-10.0f, -20.0f, -5.0f, -1.0f, -9.0f), makeParams (-3.0f, 0.0f, 0.0f, 0.0f, -6.0f) },
            };

            for (const auto& c : clamps)
            {
                ColorModule a, b;
                prepareColor (a, sampleRate, c.outOfRange);
                prepareColor (b, sampleRate, c.clamped);
                expectEquals ((int) countBitDifferences (runColor (a, input, blockSize), runColor (b, input, blockSize)), 0,
                              juce::String (c.name) + ": out-of-range finite values must still clamp to the boundary");
                expectEquals ((int) countBitDifferences (runSwitched (base, c.outOfRange), runSwitched (base, c.clamped)), 0,
                              juce::String (c.name) + " mid-stream: out-of-range finite values must still clamp to the boundary");
            }
        }
    };

    static ColorTests colorTests;
}

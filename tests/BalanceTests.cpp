/*
  ==============================================================================

    BalanceTests.cpp
    设计规格-Balance「验收 · 自动测试」四条 + v0.3 计划里定下的补充验收
    （零延迟、Bypass 逐位、中途旁通、分块无关、各采样率 null、静音门、段中心频率、封顶、Brighten 方向）
    + v0.5 性能优化的等价性（两声道打包 = 单声道、分析拆片 = 一口气做完、高采样率与小块的分块无关）
    与「Recover / Tame 拧回 0 后所有段停下、逐位直通」（修 v0.4 的 bug）。

    只按规格、v0.3 实现计划「规格没写死、写码前定下的细节」表与 BalanceModule.h 的公开接口写，
    不依赖任何实现细节：每条断言的依据写在对应函数的注释里，方便复核。

  ==============================================================================
*/

#include "TestHelpers.h"

#include "../Source/dsp/BalanceModule.h"

#include <cstring>

namespace celadon::test
{
    namespace
    {
        constexpr int numChannels = 2;
        constexpr int maxBlock = 512;

        // 计划：FFT 点数 = 2048 × ceil(fs / 48000) 取 2 的幂，hop = 点数 / 4 → 44.1k / 48k 下 hop = 512（≈ 10.7 ms）。
        // 需要「每 hop 读一次增益」的用例都在 48 kHz 下按 512 一块跑，每块读一次：块与 hop 等长，
        // 不会漏掉任何一次发布，也不会把同一次发布读成两笔。
        constexpr int hopAt48k = 512;

        using Params = BalanceModule::Params;
        using Speed = BalanceModule::Speed;
        using BandGains = BalanceModule::BandGains;

        constexpr int numBands = BalanceModule::numBands;

        // 40 起数：0 = 40 Hz … 13 = 800 Hz、14 = 1 kHz、15 = 1.25 kHz … 26 = 16 kHz。
        constexpr int bandOf1kHz = 14;

        Params makeParams (float recover, float tame, float brighten = 0.0f, Speed speed = Speed::mid, bool bypass = false)
        {
            Params p;
            p.recover = recover;
            p.tame = tame;
            p.brighten = brighten;
            p.speed = speed;
            p.bypass = bypass;
            return p;
        }

        // 先 setParams 再 prepare（与 Color / Limiter 测试同一套路：prepare → reset 把状态直接落到目标），
        // prepare 之后再 set 一次是保险——万一实现的 prepare 把参数恢复成默认值，这一步能纠正回来。
        void prepareBalance (BalanceModule& balance, double sampleRate, const Params& p)
        {
            balance.setParams (p);
            balance.prepare ({ sampleRate, (juce::uint32) maxBlock, (juce::uint32) numChannels });
            balance.setParams (p);
        }

        // 按指定块大小把整段信号送进 Balance，返回输出。
        // frames 非空指针时，每处理完一块就读一次 27 段增益，块起点 ≥ collectFromSample 的才记录。
        Signal runBalance (BalanceModule& balance, const Signal& input, int blockSize,
                           std::vector<BandGains>* frames = nullptr, int collectFromSample = 0)
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

                balance.process (block);

                for (int ch = 0; ch < numChannels; ++ch)
                    std::copy (block.getReadPointer (ch), block.getReadPointer (ch) + length,
                               output[(size_t) ch].begin() + start);

                if (frames != nullptr && start >= collectFromSample)
                {
                    BandGains gains {};
                    balance.getBandGainsDb (gains);
                    frames->push_back (gains);
                }
            }

            return output;
        }

        // 粉噪：Paul Kellet 的 7 极点近似（musicdsp.org「pink noise, refined」），
        // 9 Hz ～ 0.45 fs 内与 −3 dB/oct 的偏差 ±0.05 dB。白噪来自固定种子的 juce::Random，
        // 两声道用不同种子（互不相关）；先空跑 8192 样本让滤波器进稳态，再把每声道归一到指定 RMS。
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

        // 对数（指数）正弦扫频：瞬时频率 f(t) = f0 · (f1 / f0)^(t / T)，相位用解析积分算，double 精度。
        Signal makeLogSweep (int channels, double sampleRate, double startHz, double endHz, float amplitude, double seconds)
        {
            const auto numSamples = (int) (seconds * sampleRate);
            auto signal = makeSilence (channels, numSamples);
            const auto k = std::log (endHz / startHz);

            for (int i = 0; i < numSamples; ++i)
            {
                const auto t = (double) i / sampleRate;
                const auto phase = juce::MathConstants<double>::twoPi * startHz * seconds / k * (std::exp (t * k / seconds) - 1.0);
                const auto value = amplitude * (float) std::sin (phase);

                for (auto& channel : signal)
                    channel[(size_t) i] = value;
            }

            return signal;
        }

        void addSignal (Signal& destination, const Signal& source)
        {
            for (size_t ch = 0; ch < destination.size(); ++ch)
                for (size_t i = 0; i < destination[ch].size(); ++i)
                    destination[ch][i] += source[ch][i];
        }

        // 粉噪（−18 dBFS RMS）+ 60 / 1000 / 9000 Hz 正弦：频谱有起伏、两声道不相关，
        // 让 27 个滤波器与分析都真正参与——null / 旁通 / 分块无关用例的通用信号。
        Signal makeRichSignal (double sampleRate, double seconds, juce::int64 seed)
        {
            auto signal = makePinkNoise (numChannels, (int) (seconds * sampleRate), -18.0f, seed);

            for (auto frequency : { 60.0, 1000.0, 9000.0 })
                addSignal (signal, makeSine (numChannels, sampleRate, frequency, 0.05f, seconds));

            return signal;
        }

        // 独立的静态滤波器生成「中频缺口 + 自然高频滚降」素材，验证实际处理链而非伪造频谱状态。
        void applySpectrumShape (Signal& signal, double sampleRate, bool withNotch)
        {
            for (auto& channel : signal)
            {
                juce::dsp::IIR::Filter<float> lowpass;
                lowpass.coefficients = juce::dsp::IIR::Coefficients<float>::makeLowPass (sampleRate, 8000.0f);
                juce::dsp::IIR::Filter<float> notch;
                notch.coefficients = juce::dsp::IIR::Coefficients<float>::makePeakFilter (
                    sampleRate, 1000.0f, 4.318f, juce::Decibels::decibelsToGain (-12.0f));

                for (auto& sample : channel)
                {
                    sample = lowpass.processSample (sample);
                    if (withNotch)
                        sample = notch.processSample (sample);
                }
            }
        }

        double rmsOf (const std::vector<float>& channel)
        {
            double sumOfSquares = 0.0;

            for (auto sample : channel)
                sumOfSquares += (double) sample * (double) sample;

            return std::sqrt (sumOfSquares / (double) channel.size());
        }

        // 独立测实际输出：一个 1/3 倍频程附近的带通均方；丢弃前段让测量滤波器充分稳定。
        double bandRms (const Signal& signal, double sampleRate, float frequency, int fromSample)
        {
            double energy = 0.0;
            std::int64_t count = 0;
            for (const auto& channel : signal)
            {
                juce::dsp::IIR::Filter<float> bandpass;
                bandpass.coefficients = juce::dsp::IIR::Coefficients<float>::makeBandPass (sampleRate, frequency, 4.318f);
                for (int i = 0; i < (int) channel.size(); ++i)
                {
                    const auto sample = (double) bandpass.processSample (channel[(size_t) i]);
                    if (i >= fromSample)
                    {
                        energy += sample * sample;
                        ++count;
                    }
                }
            }
            return count > 0 ? std::sqrt (energy / (double) count) : 0.0;
        }

        double toneMagnitude (const Signal& signal, double sampleRate, double frequency, int fromSample, int toSample = -1)
        {
            double cosine = 0.0, sine = 0.0;
            const auto& channel = signal.front();
            const auto end = toSample < 0 ? (int) channel.size() : juce::jmin (toSample, (int) channel.size());
            for (int i = fromSample; i < end; ++i)
            {
                const auto phase = juce::MathConstants<double>::twoPi * frequency * (double) i / sampleRate;
                cosine += (double) channel[(size_t) i] * std::cos (phase);
                sine += (double) channel[(size_t) i] * std::sin (phase);
            }
            return std::hypot (cosine, sine);
        }

        // 2 ms 淡入/淡出避免用硬剪接的宽带点击冒充该频段的起音。
        void addToneEvent (Signal& signal, double sampleRate, double frequency, float amplitude,
                           double startSeconds, double durationSeconds)
        {
            const auto start = (int) std::lround (startSeconds * sampleRate);
            const auto end = juce::jmin ((int) signal.front().size(), start + (int) std::lround (durationSeconds * sampleRate));
            const auto ramp = sampleRate * 0.002;
            for (int i = start; i < end; ++i)
            {
                const auto envelope = juce::jmin (1.0, juce::jmin ((double) (i - start) / ramp, (double) (end - 1 - i) / ramp));
                const auto sample = (float) (amplitude * envelope * std::sin (juce::MathConstants<double>::twoPi * frequency * i / sampleRate));
                for (auto& channel : signal) channel[(size_t) i] += sample;
            }
        }

        float samplePeakOf (const Signal& signal)
        {
            float peak = 0.0f;

            for (const auto& channel : signal)
                for (auto sample : channel)
                    peak = juce::jmax (peak, std::abs (sample));

            return peak;
        }

        int peakIndexOf (const std::vector<float>& channel)
        {
            int peakIndex = 0;

            for (int i = 0; i < (int) channel.size(); ++i)
                if (std::abs (channel[(size_t) i]) > std::abs (channel[(size_t) peakIndex]))
                    peakIndex = i;

            return peakIndex;
        }

        // 从 fromIndex 起（到末尾），输出与输入的最大差（Balance 零延迟，不用对齐）。
        float maxDiffFrom (const Signal& input, const Signal& output, int fromIndex)
        {
            float diff = 0.0f;

            for (size_t ch = 0; ch < output.size(); ++ch)
                for (int i = fromIndex; i < (int) output[ch].size(); ++i)
                    diff = juce::jmax (diff, std::abs (output[ch][(size_t) i] - input[ch][(size_t) i]));

            return diff;
        }

        float maxAbsGainOf (const std::vector<BandGains>& frames, int band)
        {
            float worst = 0.0f;

            for (const auto& frame : frames)
                worst = juce::jmax (worst, std::abs (frame[(size_t) band]));

            return worst;
        }

        float meanGainOf (const std::vector<BandGains>& frames, int band)
        {
            double sum = 0.0;

            for (const auto& frame : frames)
                sum += (double) frame[(size_t) band];

            return frames.empty() ? 0.0f : (float) (sum / (double) frames.size());
        }

        // 梳状素材：粉噪（−24 dBFS RMS）+ 每隔一段的中心频率上一个 −20 dBFS 正弦（0.45 fs 以下），两声道同样的正弦、
        // 不相关的粉噪。Recover = Tame = 100 时突出的段被压、夹在中间的段被提，二十多段滤波器同时在工作。
        Signal makeCombSignal (int channels, double sampleRate, double seconds, juce::int64 seed)
        {
            auto signal = makePinkNoise (channels, (int) (seconds * sampleRate), -24.0f, seed);
            const auto amplitude = juce::Decibels::decibelsToGain (-20.0f) * juce::MathConstants<float>::sqrt2;

            for (int band = 0; band < numBands; band += 2)
            {
                const auto frequency = 1000.0 * std::pow (2.0, (double) (band - bandOf1kHz) / 3.0);

                if (frequency < 0.45 * sampleRate)
                    addSignal (signal, makeSine (channels, sampleRate, frequency, amplitude, seconds, 0.1 * band));
            }

            return signal;
        }

        // 计划「细节」表：FFT 点数 = 2048 × ceil(fs / 48000) 取 2 的幂，hop = 点数 / 4；第一次分析在第「点数」个样本。
        int fftSizeFor (double sampleRate)
        {
            int size = 2048;

            while ((double) size / 2048.0 * 48000.0 < sampleRate)
                size *= 2;

            return size;
        }

        // v0.5 起每 hop 的分析拆成三片，最后一片在 hop 后第 1/8 hop 处（= 系数重算间隔：48 k 下第 64 个样本，T-032 R2-07 起随
        // 采样率翻倍，192 k 下第 256 个）才发布 G：读数时刻落在 hop 后第 1 ～ 1/8 hop 个样本（含）之间时，拆片版读到的还是上一帧，
        // 一口气做完的版本已是新一帧。只晚发布，值不变。
        int publishDelayFor (double sampleRate)
        {
            return fftSizeFor (sampleRate) / 32;   // hop / 8
        }

        bool isInsidePublishWindow (int samplesProcessed, double sampleRate)
        {
            const auto fftSize = fftSizeFor (sampleRate);

            if (samplesProcessed <= fftSize)
                return false;

            const auto phase = (samplesProcessed - fftSize) % (fftSize / 4);
            return phase >= 1 && phase <= publishDelayFor (sampleRate);
        }

        struct BlockRun
        {
            Signal output;
            std::vector<BandGains> gains;   // 每块处理完读一次
            std::vector<int> readPositions; // 读数时已处理的样本数
        };

        // 任意声道数、按 blockSize 分块跑一遍；每块处理前先 setParams (paramsForBlock (块序号))。
        template <typename ParamsForBlock>
        BlockRun runBlocks (BalanceModule& balance, const Signal& input, int blockSize, ParamsForBlock&& paramsForBlock)
        {
            const auto channels = (int) input.size();
            const auto numSamples = (int) input.front().size();

            BlockRun run;
            run.output = input;
            juce::AudioBuffer<float> block (channels, blockSize);
            int blockIndex = 0;

            for (int start = 0; start < numSamples; start += blockSize, ++blockIndex)
            {
                const auto length = juce::jmin (blockSize, numSamples - start);
                block.setSize (channels, length, false, false, true);

                for (int ch = 0; ch < channels; ++ch)
                    block.copyFrom (ch, 0, input[(size_t) ch].data() + start, length);

                balance.setParams (paramsForBlock (blockIndex));
                balance.process (block);

                for (int ch = 0; ch < channels; ++ch)
                    std::copy (block.getReadPointer (ch), block.getReadPointer (ch) + length,
                               run.output[(size_t) ch].begin() + start);

                BandGains gains {};
                balance.getBandGainsDb (gains);
                run.gains.push_back (gains);
                run.readPositions.push_back (start + length);
            }

            return run;
        }

        // 逐位比较两段输出：不同的样本数（按位比，连 −0 / +0 也算不同）。
        int countDifferentSamples (const Signal& a, const Signal& b, int channelOfA = -1)
        {
            int different = 0;

            for (size_t ch = 0; ch < b.size(); ++ch)
            {
                const auto& x = a[channelOfA < 0 ? ch : (size_t) channelOfA];
                const auto& y = b[ch];

                for (size_t i = 0; i < y.size(); ++i)
                    different += std::memcmp (&x[i], &y[i], sizeof (float)) != 0 ? 1 : 0;
            }

            return different;
        }

        bool sameGains (const BandGains& a, const BandGains& b)
        {
            return std::memcmp (a.data(), b.data(), sizeof (BandGains)) == 0;
        }

        // 一段增益读数里「变化方向翻转」的次数：Δ_k = G_k − G_{k−1}，|Δ| < minStep 的视为没动（不给方向）；
        // 相邻两个有效 Δ 符号相反记一次。minStep 只用来滤掉 float 舍入级别的抖动，真正的振荡幅度远大于它。
        int countDirectionReversals (const std::vector<BandGains>& frames, int band, float minStep)
        {
            int reversals = 0;
            int lastDirection = 0;

            for (size_t k = 1; k < frames.size(); ++k)
            {
                const auto delta = frames[k][(size_t) band] - frames[k - 1][(size_t) band];

                if (std::abs (delta) < minStep)
                    continue;

                const auto direction = delta > 0.0f ? 1 : -1;

                if (lastDirection != 0 && direction != lastDirection)
                    ++reversals;

                lastDirection = direction;
            }

            return reversals;
        }
    }

    class BalanceTests  : public juce::UnitTest
    {
    public:
        BalanceTests() : juce::UnitTest ("BalanceModule", "Celadon") {}

        void runTest() override
        {
            testBandCentres();
            testNullWhenRecoverAndTameAreZero();
            testPinkNoiseConverges();
            testSweepDoesNotOscillate();
            testTamesOneKilohertzTone();
            testTameHasUsefulRange();
            testLatencyIsZero();
            testBypassIsBitExact();
            testBypassToggleMidStream();
            testBlockSizeIndependence();
            testSilenceGate();
            testBrightenTilt();
            testLevelStepsDoNotPumpAllBands();
            testLoudInputStaysFiniteAndCapped();
            testStereoMatchesMono();
            testPackedMatchesScalarWithDifferentChannels();
            testSlicedAnalysisMatchesUnsliced();
            testBlockSizeIndependenceAtHighRate();
            testReturnToZeroStopsAllBands();
            testAutomaticRecoverProtectsRollOff();
            testAutomaticRecoverPreservesManualPortion();
            testAutomaticRecoverSanitisesAndDoesNotAllocate();
            testAutomaticRecoverDoesNotSwallowBrighten();
            testAutomaticTameSelectivity();
            testAutomaticTameHarmonicProtection();
            testAutomaticTameHarmonicChannelCoherence();
            testAutomaticTameHarmonicReleaseAndIndependentPeak();
            testAutomaticTameManualAndSupport();
            testAutomaticTameBlockSizesAndAllocations();
            testUpwardMaskingLiftsBuriedNeighbour();
        }

    private:
        void testUpwardMaskingLiftsBuriedNeighbour()
        {
            beginTest ("upward masking lifts a buried higher neighbour only when Recover and Tame are both open");

            constexpr double sampleRate = 48000.0;
            constexpr double seconds = 8.0;
            constexpr int maskerBand = 11;     // 508 Hz
            constexpr int buriedBand = 12;     // 640 Hz
            const auto length = (int) (seconds * sampleRate);
            auto input = makePinkNoise (numChannels, length, -28.0f, 4408);
            const auto masker = makeSine (numChannels, sampleRate, 508.0, juce::Decibels::decibelsToGain (-8.0f), seconds);
            const auto buried = makeSine (numChannels, sampleRate, 640.0, juce::Decibels::decibelsToGain (-18.0f), seconds);

            for (int ch = 0; ch < numChannels; ++ch)
                for (int i = 0; i < length; ++i)
                {
                    input[(size_t) ch][(size_t) i] += masker[(size_t) ch][(size_t) i];
                    input[(size_t) ch][(size_t) i] += buried[(size_t) ch][(size_t) i];
                }

            const auto measure = [&] (float recover, float tame)
            {
                BalanceModule balance;
                prepareBalance (balance, sampleRate, makeParams (recover, tame, 0.0f, Speed::mid));
                std::vector<BandGains> frames;
                const auto output = runBalance (balance, input, hopAt48k, &frames, (int) (6.5 * sampleRate));
                expect (allFinite (output));
                return std::pair<float, float> (meanGainOf (frames, maskerBand), meanGainOf (frames, buriedBand));
            };

            const auto both = measure (100.0f, 100.0f);
            const auto tameOnly = measure (0.0f, 100.0f);
            const auto recoverOnly = measure (100.0f, 0.0f);

            logMessage ("  masker/buried gain, both " + juce::String (both.first, 2) + "/" + juce::String (both.second, 2)
                        + " dB, tame only " + juce::String (tameOnly.first, 2) + "/" + juce::String (tameOnly.second, 2)
                        + " dB, recover only " + juce::String (recoverOnly.first, 2) + "/" + juce::String (recoverOnly.second, 2) + " dB");

            expect (both.first < -4.0f, "the louder lower tone is still tamed");
            expect (both.second > tameOnly.second + 0.8f, "Recover gives the higher neighbour back level that Tame alone removes");
            expect (both.second > both.first + 0.4f, "after unmasking, the higher neighbour sits above the masker");
            expect (tameOnly.second <= both.second, "Tame alone does not out-lift the combined path");
            expect (recoverOnly.first > both.first + 0.3f, "without Tame the masker is not the thing being cut");
        }

        void testAutomaticTameSelectivity()
        {
            beginTest ("automatic Tame reduces sustained peaks but preserves short events in the same and other bands");
            constexpr double sampleRate = 48000.0;
            constexpr double seconds = 6.0;
            auto params = makeParams (0.0f, 100.0f);
            params.automaticTame = 100.0f;
            const auto noise = makePinkNoise (numChannels, (int) (seconds * sampleRate), -30.0f, 9301);
            auto sustained = noise;
            addToneEvent (sustained, sampleRate, 1000.0, 0.2f, 1.0, 5.0);
            // 持续 1 kHz 共振提高全局处理权限时，4 kHz 的正常短起音仍须由自己的逐段证据决定。
            addToneEvent (sustained, sampleRate, 4000.0, 0.2f, 4.0, 0.08);
            BalanceModule balance;
            prepareBalance (balance, sampleRate, params);
            std::vector<BandGains> frames;
            const auto output = runBalance (balance, sustained, maxBlock, &frames);
            const auto gainAt = [&] (const Signal& dry, const Signal& wet, double frequency, double start, double end)
            {
                const auto first = (int) std::lround (start * sampleRate);
                const auto last = (int) std::lround (end * sampleRate);
                return 20.0 * std::log10 (toneMagnitude (wet, sampleRate, frequency, first, last)
                                      / toneMagnitude (dry, sampleRate, frequency, first, last));
            };
            const auto sustainedGain = gainAt (sustained, output, 1000.0, 5.0, 5.9);
            const auto otherOnsetGain = gainAt (sustained, output, 4000.0, 4.0, 4.08);
            logMessage ("  sustained 1 kHz " + juce::String (sustainedGain, 3)
                        + " dB, concurrent 4 kHz onset " + juce::String (otherOnsetGain, 3) + " dB");
            expect (sustainedGain < -1.5 && sustainedGain > -3.5, "the real sustained resonance is reduced with bounded depth");
            expect (std::abs (otherOnsetGain) < 0.5, "another band's short attack is not sacrificed to the persistent resonance");
            float lowestGain = 0.0f, highestGain = 0.0f;
            for (const auto& frame : frames)
                for (auto gain : frame)
                {
                    lowestGain = juce::jmin (lowestGain, gain);
                    highestGain = juce::jmax (highestGain, gain);
                }
            expect (lowestGain >= -2.5001f && highestGain <= 0.0001f, "automatic-only correction has a 2.5 dB per-band cut ceiling");

            auto pulses = noise;
            for (auto start : { 1.0, 2.5, 4.0 }) addToneEvent (pulses, sampleRate, 1000.0, 0.2f, start, 0.08);
            BalanceModule pulsed;
            prepareBalance (pulsed, sampleRate, params);
            const auto pulseOutput = runBalance (pulsed, pulses, maxBlock);
            for (auto start : { 1.0, 2.5, 4.0 })
            {
                const auto gain = gainAt (pulses, pulseOutput, 1000.0, start, start + 0.08);
                expect (std::abs (gain) < 0.5, "short same-frequency events do not accumulate into a persistent resonance");
            }
            expect (allFinite (output) && allFinite (pulseOutput));
        }

        void testAutomaticTameHarmonicProtection()
        {
            beginTest ("automatic Tame protects coherent harmonics, not equally strong detuned peaks, and retains manual depth");
            struct Case { double rate, fundamental; };
            for (const auto c : { Case { 48000.0, 500.0 }, Case { 44100.0, 997.0 }, Case { 96000.0, 503.0 } })
            {
                constexpr double seconds = 5.0;
                auto harmonic = makePinkNoise (numChannels, (int) (seconds * c.rate), -30.0f, 9320);
                auto detuned = harmonic;
                // Both spectra put three similarly strong lines into the same broad bands. Only their fine
                // frequency relationship differs; a count of loud bands must not pass this comparison.
                addToneEvent (harmonic, c.rate, c.fundamental, 0.2f, 0.5, 4.5);
                addToneEvent (harmonic, c.rate, 2.0 * c.fundamental, 0.13f, 0.5, 4.5);
                addToneEvent (harmonic, c.rate, 3.0 * c.fundamental, 0.10f, 0.5, 4.5);
                addToneEvent (detuned, c.rate, c.fundamental, 0.2f, 0.5, 4.5);
                addToneEvent (detuned, c.rate, 2.12 * c.fundamental, 0.13f, 0.5, 4.5);
                addToneEvent (detuned, c.rate, 2.88 * c.fundamental, 0.10f, 0.5, 4.5);
                auto params = makeParams (0.0f, 100.0f);
                params.automaticTame = 100.0f;
                BalanceModule a, b;
                prepareBalance (a, c.rate, params);
                prepareBalance (b, c.rate, params);
                const auto harmonicOutput = runBalance (a, harmonic, maxBlock);
                const auto detunedOutput = runBalance (b, detuned, maxBlock);
                const auto gainAt = [&] (const Signal& dry, const Signal& wet, double frequency)
                {
                    const auto first = (int) std::lround (4.0 * c.rate), last = (int) std::lround (4.9 * c.rate);
                    return 20.0 * std::log10 (toneMagnitude (wet, c.rate, frequency, first, last)
                                          / toneMagnitude (dry, c.rate, frequency, first, last));
                };
                const auto protectedGain = gainAt (harmonic, harmonicOutput, c.fundamental);
                const auto detunedGain = gainAt (detuned, detunedOutput, c.fundamental);
                const auto tag = " @" + juce::String (c.rate, 0) + " Hz, f0=" + juce::String (c.fundamental, 0);
                logMessage ("  harmonic fundamental " + juce::String (protectedGain, 3)
                            + " dB, detuned fundamental " + juce::String (detunedGain, 3) + " dB" + tag);
                expect (protectedGain > -1.2 && protectedGain <= 0.05, "coherent fundamental retains its level" + tag);
                expect (detunedGain < -1.5, "three unrelated sustained peaks do not earn harmonic protection" + tag);
                expect (protectedGain > detunedGain + 1.0, "harmonic evidence makes an audible difference" + tag);
                for (const auto harmonicNumber : { 2, 3 })
                    expect (gainAt (harmonic, harmonicOutput, harmonicNumber * c.fundamental) > -1.2,
                            "supported overtone is protected too" + tag);
                expect (allFinite (harmonicOutput) && allFinite (detunedOutput));
                expectEquals (a.getLatencySamples(), 0, "harmonic protection adds no latency");

                if (c.rate == 48000.0)
                {
                    const auto manual = makeParams (0.0f, 40.0f);
                    auto mixed = makeParams (0.0f, 100.0f);
                    mixed.automaticTame = 60.0f;
                    BalanceModule manualOnly, manualAndAutomatic;
                    prepareBalance (manualOnly, c.rate, manual);
                    prepareBalance (manualAndAutomatic, c.rate, mixed);
                    const auto manualGain = gainAt (harmonic, runBalance (manualOnly, harmonic, maxBlock), c.fundamental);
                    const auto mixedGain = gainAt (harmonic, runBalance (manualAndAutomatic, harmonic, maxBlock), c.fundamental);
                    expect (manualGain < protectedGain - 1.0, "the manual share still intentionally cuts harmonic material");
                    expect (mixedGain <= manualGain + 0.05 && mixedGain > manualGain - 1.0,
                            "protection reduces only the extra automatic cut, never the manual share");
                }
            }
        }

        void testAutomaticTameHarmonicChannelCoherence()
        {
            beginTest ("harmonic protection requires a complete group in one channel, not unrelated lines panned apart");
            constexpr double sampleRate = 48000.0, seconds = 5.0;
            const auto samples = (int) (sampleRate * seconds);
            auto coherent = makePinkNoise (numChannels, samples, -30.0f, 9322);
            auto split = coherent;
            auto fundamental = makeSilence (1, samples), overtones = fundamental;
            addToneEvent (fundamental, sampleRate, 500.0, 0.2f, 0.5, 4.5);
            addToneEvent (overtones, sampleRate, 1000.0, 0.13f, 0.5, 4.5);
            addToneEvent (overtones, sampleRate, 1500.0, 0.10f, 0.5, 4.5);
            for (int i = 0; i < samples; ++i)
            {
                coherent[0][(size_t) i] += fundamental[0][(size_t) i] + overtones[0][(size_t) i];
                split[0][(size_t) i] += fundamental[0][(size_t) i];
                split[1][(size_t) i] += overtones[0][(size_t) i];
            }
            auto params = makeParams (0.0f, 100.0f);
            params.automaticTame = 100.0f;
            BalanceModule a, b;
            prepareBalance (a, sampleRate, params);
            prepareBalance (b, sampleRate, params);
            const auto coherentOutput = runBalance (a, coherent, maxBlock);
            const auto splitOutput = runBalance (b, split, maxBlock);
            const auto gainAt = [&] (const Signal& dry, const Signal& wet)
            {
                const auto first = (int) (4.0 * sampleRate), last = (int) (4.9 * sampleRate);
                return 20.0 * std::log10 (toneMagnitude (wet, sampleRate, 500.0, first, last)
                                      / toneMagnitude (dry, sampleRate, 500.0, first, last));
            };
            const auto coherentGain = gainAt (coherent, coherentOutput), splitGain = gainAt (split, splitOutput);
            logMessage ("  one-channel harmonic fundamental " + juce::String (coherentGain, 3)
                        + " dB, cross-channel unrelated fundamental " + juce::String (splitGain, 3) + " dB");
            expect (coherentGain > -1.2, "a genuine harmonic group panned to one side retains protection");
            expect (splitGain < -1.5 && splitGain > -3.5, "opposite-channel lines cannot exempt a sustained resonance");
            expect (coherentGain > splitGain + 1.0, "the audible distinction follows within-channel coherence");
            expect (allFinite (coherentOutput) && allFinite (splitOutput));

            // Both pan directions exercise the same rule; swapping channels cannot change linked processing.
            BalanceModule mirrored;
            prepareBalance (mirrored, sampleRate, params);
            const Signal swapped { split[1], split[0] };
            const auto mirroredOutput = runBalance (mirrored, swapped, 73);
            const Signal swappedBack { mirroredOutput[1], mirroredOutput[0] };
            expectEquals (countDifferentSamples (splitOutput, swappedBack), 0, "channel order and block partition are immaterial");
        }

        void testAutomaticTameHarmonicReleaseAndIndependentPeak()
        {
            beginTest ("harmonic protection remains local and releases when supporting overtones stop");
            constexpr double sampleRate = 48000.0;
            auto input = makePinkNoise (numChannels, (int) (6.0 * sampleRate), -30.0f, 9321);
            addToneEvent (input, sampleRate, 500.0, 0.2f, 0.5, 5.5);
            addToneEvent (input, sampleRate, 1000.0, 0.13f, 0.5, 2.5);
            addToneEvent (input, sampleRate, 1500.0, 0.10f, 0.5, 2.5);
            addToneEvent (input, sampleRate, 3200.0, 0.2f, 0.5, 5.5); // unrelated, persistent narrow peak
            auto params = makeParams (0.0f, 100.0f);
            params.automaticTame = 100.0f;
            BalanceModule balance;
            prepareBalance (balance, sampleRate, params);
            std::vector<BandGains> frames;
            const auto output = runBalance (balance, input, maxBlock, &frames);
            const auto gainAt = [&] (double frequency, double start, double end)
            {
                const auto first = (int) std::lround (start * sampleRate), last = (int) std::lround (end * sampleRate);
                return 20.0 * std::log10 (toneMagnitude (output, sampleRate, frequency, first, last)
                                      / toneMagnitude (input, sampleRate, frequency, first, last));
            };
            const auto supportedGain = gainAt (500.0, 2.0, 2.8);
            const auto unsupportedGain = gainAt (500.0, 5.0, 5.8);
            const auto independentGain = gainAt (3200.0, 2.0, 2.8);
            logMessage ("  supported fundamental " + juce::String (supportedGain, 3) + " dB, after support ends "
                        + juce::String (unsupportedGain, 3) + " dB, unrelated peak " + juce::String (independentGain, 3) + " dB");
            expect (supportedGain > -1.2, "the harmonic group remains protected beside a separate resonance");
            expect (independentGain < -1.5 && independentGain > -3.5, "an unrelated peak still receives the original bounded suppression");
            expect (unsupportedGain < -1.5 && unsupportedGain < supportedGain - 1.0,
                    "a remaining isolated resonance loses stale harmonic permission");
            for (size_t i = 1; i < frames.size(); ++i)
                for (size_t b = 0; b < (size_t) numBands; ++b)
                    expect (std::abs (frames[i][b] - frames[i - 1][b]) <= 1.00001f
                            && frames[i][b] >= -2.5001f && frames[i][b] <= 0.0001f,
                            "protection changes preserve the existing per-hop slope and automatic depth limits");
            expect (allFinite (output));
        }

        void testAutomaticTameManualAndSupport()
        {
            beginTest ("automatic Tame preserves the manual share and rejects isolated tones and unsupported edge bands");
            constexpr double sampleRate = 48000.0;
            auto automatic = makeParams (0.0f, 100.0f);
            automatic.automaticTame = 100.0f;
            for (auto frequency : { 40.0, 1000.0, 16000.0 })
            {
                const auto input = makeSine (numChannels, sampleRate, frequency, 0.2f, 3.0);
                BalanceModule isolated;
                prepareBalance (isolated, sampleRate, automatic);
                const auto output = runBalance (isolated, input, maxBlock);
                expectEquals (countDifferentSamples (input, output), 0, "an isolated sustained tone has no broadband reference at " + juce::String (frequency));
            }

            const auto input = makeSine (numChannels, sampleRate, 1000.0, 0.2f, 3.0);
            const auto manual = makeParams (0.0f, 40.0f, 30.0f);
            auto mixed = manual;
            mixed.tame = 80.0f;
            mixed.automaticTame = 40.0f;
            BalanceModule manualOnly, withAutomatic;
            prepareBalance (manualOnly, sampleRate, manual);
            prepareBalance (withAutomatic, sampleRate, mixed);
            const auto reference = runBalance (manualOnly, input, maxBlock);
            expectEquals (countDifferentSamples (reference, runBalance (withAutomatic, input, maxBlock)), 0,
                          "unpermitted automatic share leaves manual Tame and Brighten bit-exact");

            auto rich = makeRichSignal (sampleRate, 2.0, 9302);
            const auto legacy = makeParams (25.0f, 60.0f, 20.0f);
            BalanceModule oldParams;
            prepareBalance (oldParams, sampleRate, legacy);
            const auto oldOutput = runBalance (oldParams, rich, maxBlock);
            for (auto share : { 0.0f, -1.0f, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity() })
            {
                auto p = legacy;
                p.automaticTame = share;
                BalanceModule b;
                prepareBalance (b, sampleRate, p);
                expectEquals (countDifferentSamples (oldOutput, runBalance (b, rich, maxBlock)), 0,
                              "zero or invalid automatic share retains existing manual behavior");
            }
        }

        void testAutomaticTameBlockSizesAndAllocations()
        {
            beginTest ("automatic Tame is bounded, block-size independent and allocation-free");
            constexpr double sampleRate = 48000.0;
            auto input = makePinkNoise (numChannels, (int) (3.0 * sampleRate), -30.0f, 9303);
            addToneEvent (input, sampleRate, 1000.0, 0.2f, 0.5, 2.5);
            addToneEvent (input, sampleRate, 500.0, 0.25f, 0.5, 1.5);
            addToneEvent (input, sampleRate, 1500.0, 0.12f, 0.5, 1.5);
            addToneEvent (input, sampleRate, 4000.0, 0.2f, 2.0, 0.08);
            auto p = makeParams (0.0f, 60.0f);
            p.automaticTame = 60.0f;
            BalanceModule baseline;
            prepareBalance (baseline, sampleRate, p);
            const auto reference = runBalance (baseline, input, maxBlock);
            for (auto blockSize : { 73, 2048 })
            {
                BalanceModule b;
                prepareBalance (b, sampleRate, p);
                expectEquals (countDifferentSamples (reference, runBalance (b, input, blockSize)), 0,
                              "fixed-hop permission must not depend on host block size");
            }
            auto excessive = p;
            excessive.automaticTame = 1000.0f;
            BalanceModule bounded;
            prepareBalance (bounded, sampleRate, excessive);
            expectEquals (countDifferentSamples (reference, runBalance (bounded, input, maxBlock)), 0,
                          "the automatic share cannot exceed total Tame");

            juce::AudioBuffer<float> block (numChannels, maxBlock);
            std::uint64_t allocations = 0;
            bounded.reset();
            for (int start = 0; start + maxBlock <= (int) input.front().size(); start += maxBlock)
            {
                for (int ch = 0; ch < numChannels; ++ch)
                    block.copyFrom (ch, 0, input[(size_t) ch].data() + start, maxBlock);
                const auto before = allocationCountOnThisThread();
                bounded.setParams (p);
                bounded.process (block);
                allocations += allocationCountOnThisThread() - before;
            }
            expectEquals ((juce::int64) allocations, (juce::int64) 0, "automatic Tame never allocates on the audio thread");
        }

        void testAutomaticRecoverProtectsRollOff()
        {
            beginTest ("automatic Recover repairs an interior notch without filling natural roll-off or edge bands");

            constexpr double sampleRate = 48000.0;
            constexpr double seconds = 10.0;
            auto input = makePinkNoise (numChannels, (int) (seconds * sampleRate), -12.0f, 9051);
            applySpectrumShape (input, sampleRate, true);
            auto automaticParams = makeParams (100.0f, 0.0f);
            automaticParams.automaticRecover = 100.0f;

            BalanceModule automatic, manual;
            prepareBalance (automatic, sampleRate, automaticParams);
            prepareBalance (manual, sampleRate, makeParams (100.0f, 0.0f));
            std::vector<BandGains> automaticFrames, manualFrames;
            const auto automaticOutput = runBalance (automatic, input, maxBlock, &automaticFrames, (int) (8.0 * sampleRate));
            const auto manualOutput = runBalance (manual, input, maxBlock, &manualFrames, (int) (8.0 * sampleRate));

            expect (allFinite (automaticOutput) && allFinite (manualOutput));
            expect (meanGainOf (automaticFrames, bandOf1kHz) > 1.0f, "a sustained interior notch must still recover");
            expect (maxDiffFrom (input, automaticOutput, (int) (8.0 * sampleRate)) > 1.0e-4f,
                    "the automatic correction must reach the audio output");
            expectEquals (maxAbsGainOf (automaticFrames, 0), 0.0f, "automatic Recover never boosts the low edge");
            expectEquals (maxAbsGainOf (automaticFrames, numBands - 1), 0.0f, "automatic Recover never boosts the high edge");

            for (int band = 23; band < numBands; ++band)
                expect (maxAbsGainOf (automaticFrames, band) < 0.1f,
                        "natural high roll-off stays intact at band " + juce::String (band));

            expect (meanGainOf (manualFrames, numBands - 1) > 0.5f,
                    "control: unrestricted manual Recover would have lifted the high edge");

            BalanceModule differentlyBlocked;
            prepareBalance (differentlyBlocked, sampleRate, automaticParams);
            const auto otherOutput = runBalance (differentlyBlocked, input, 73);
            expectEquals (maxDiffFrom (automaticOutput, otherOutput, 0), 0.0f,
                          "automatic eligibility and persistence use sample time, not host blocks");
        }

        void testAutomaticRecoverPreservesManualPortion()
        {
            beginTest ("automatic Recover protection preserves the manual share and its Brighten response");

            constexpr double sampleRate = 48000.0;
            auto input = makePinkNoise (numChannels, (int) (6.0 * sampleRate), -12.0f, 9052);
            applySpectrumShape (input, sampleRate, false);

            for (auto brighten : { -100.0f, 100.0f })
            {
                auto mixedParams = makeParams (100.0f, 0.0f, brighten);
                mixedParams.automaticRecover = 60.0f;
                BalanceModule mixed, manual;
                prepareBalance (mixed, sampleRate, mixedParams);
                prepareBalance (manual, sampleRate, makeParams (40.0f, 0.0f, brighten));
                std::vector<BandGains> mixedFrames, manualFrames;
                runBalance (mixed, input, maxBlock, &mixedFrames, (int) (5.0 * sampleRate));
                runBalance (manual, input, maxBlock, &manualFrames, (int) (5.0 * sampleRate));
                const auto band = brighten > 0.0f ? numBands - 1 : 0;
                expect (meanGainOf (manualFrames, band) > 0.4f, "control: manual Brighten must lift its chosen edge");
                expectWithinAbsoluteError (meanGainOf (mixedFrames, band), meanGainOf (manualFrames, band), 1.0e-6f,
                                           "only the automatic 60 percent is removed at the edge");
            }
        }

        void testAutomaticRecoverDoesNotSwallowBrighten()
        {
            beginTest ("automatic Recover restrictions never suppress the independent manual Brighten control");
            constexpr double sampleRate = 48000.0;
            auto input = makePinkNoise (numChannels, (int) (6.0 * sampleRate), -12.0f, 9054);
            applySpectrumShape (input, sampleRate, false);
            auto automatic = makeParams (100.0f, 0.0f, 30.0f);
            automatic.automaticRecover = 100.0f;
            BalanceModule balance;
            prepareBalance (balance, sampleRate, automatic);
            std::vector<BandGains> frames;
            const auto output = runBalance (balance, input, maxBlock, &frames, (int) (5.0 * sampleRate));
            expectWithinAbsoluteError (meanGainOf (frames, numBands - 1), 0.3f * 3.0f * std::log10 (16.0f), 0.01f,
                                       "high edge has no Recover permission but retains Brighten");
            const auto highGain = 20.0 * std::log10 (bandRms (output, sampleRate, 10000.0f, (int) (5.0 * sampleRate))
                                                   / bandRms (input, sampleRate, 10000.0f, (int) (5.0 * sampleRate)));
            expect (highGain > 0.5, "independent Brighten reaches the audio despite automatic roll-off protection");
        }

        void testAutomaticRecoverSanitisesAndDoesNotAllocate()
        {
            beginTest ("automatic Recover is finite, bounded by total Recover and allocation-free");

            constexpr double sampleRate = 48000.0;
            auto input = makePinkNoise (numChannels, (int) (2.0 * sampleRate), -12.0f, 9053);
            applySpectrumShape (input, sampleRate, true);
            const auto referenceParams = makeParams (60.0f, 0.0f, 40.0f);
            BalanceModule reference;
            prepareBalance (reference, sampleRate, referenceParams);
            const auto referenceOutput = runBalance (reference, input, maxBlock);

            for (auto amount : { -5.0f, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity() })
            {
                auto p = referenceParams;
                p.automaticRecover = amount;
                BalanceModule balance;
                prepareBalance (balance, sampleRate, p);
                const auto output = runBalance (balance, input, maxBlock);
                expectEquals (maxDiffFrom (referenceOutput, output, 0), 0.0f, "invalid automatic share falls back to manual behavior");
            }

            auto full = referenceParams;
            full.automaticRecover = full.recover;
            auto excessive = full;
            excessive.automaticRecover = 1000.0f;
            BalanceModule bounded, outOfRange;
            prepareBalance (bounded, sampleRate, full);
            prepareBalance (outOfRange, sampleRate, excessive);
            const auto boundedOutput = runBalance (bounded, input, maxBlock);
            const auto excessiveOutput = runBalance (outOfRange, input, maxBlock);
            expectEquals (maxDiffFrom (boundedOutput, excessiveOutput, 0), 0.0f, "automatic share cannot exceed total Recover");

            juce::AudioBuffer<float> block (numChannels, maxBlock);
            std::uint64_t allocations = 0;
            for (int start = 0; start + maxBlock <= (int) input.front().size(); start += maxBlock)
            {
                for (int ch = 0; ch < numChannels; ++ch)
                    block.copyFrom (ch, 0, input[(size_t) ch].data() + start, maxBlock);
                const auto before = allocationCountOnThisThread();
                bounded.setParams (full);
                bounded.process (block);
                allocations += allocationCountOnThisThread() - before;
            }
            expectEquals ((juce::int64) allocations, (juce::int64) 0, "automatic spectrum protection does not allocate");
        }

        // 补充：getBandCentreHz 返回 27 个中心频率，40 … 16000，单调递增，且与 1 kHz 为基准的 1/3 倍频程
        // 标称值（40, 50, 63, 80, 100, 125 … 12500, 16000）一致：标称值与精确值 1000 × 2^((b − 14) / 3) 最多差 1.6 %
        // （12500 vs 12699），容差取 3 %。第 14 段必须正好是 1 kHz——验收 4 靠它定位。
        // 依据：规格「归成 27 个 1/3 倍频程频段，中心 40 Hz ～ 16 kHz」+ BalanceModule.h 注释。
        void testBandCentres()
        {
            beginTest ("27 band centres run 40 Hz .. 16 kHz in 1/3-octave steps");

            expectEquals (numBands, 27);
            expectWithinAbsoluteError (BalanceModule::getBandCentreHz (0), 40.0f, 0.5f, "first band");
            expectWithinAbsoluteError (BalanceModule::getBandCentreHz (numBands - 1), 16000.0f, 0.5f, "last band");
            expectWithinAbsoluteError (BalanceModule::getBandCentreHz (bandOf1kHz), 1000.0f, 0.5f, "band 14 must be 1 kHz");

            for (int band = 0; band < numBands; ++band)
            {
                const auto centre = BalanceModule::getBandCentreHz (band);
                const auto exact = 1000.0f * std::pow (2.0f, (float) (band - bandOf1kHz) / 3.0f);

                expect (centre > 0.0f, "band " + juce::String (band));
                expectWithinAbsoluteError (centre, exact, exact * 0.03f, "band " + juce::String (band) + " centre");

                if (band > 0)
                    expect (centre > BalanceModule::getBandCentreHz (band - 1), "band " + juce::String (band) + " must be above band " + juce::String (band - 1));
            }
        }

        // 三项均为零才是直通；Brighten 是独立色调控制。四个采样率、所有 Speed 从第 0 个样本起逐位比较。
        void testNullWhenRecoverAndTameAreZero()
        {
            beginTest ("bit-exact when recover = tame = brighten = 0 (all speeds, four sample rates)");

            for (auto sampleRate : { 44100.0, 48000.0, 96000.0, 192000.0 })
             for (auto speed : { Speed::slow, Speed::mid, Speed::fast })
            {
                BalanceModule balance;
                prepareBalance (balance, sampleRate, makeParams (0.0f, 0.0f, 0.0f, speed));

                const auto input = makeRichSignal (sampleRate, 1.0, 42);
                const auto output = runBalance (balance, input, maxBlock);

                const auto tag = "@" + juce::String (sampleRate) + " speed " + juce::String ((int) speed);
                expect (allFinite (output), tag);
                expectEquals (maxDiffFrom (input, output, 0), 0.0f, tag);
            }
        }

        // 验收 2：粉噪（−18 dBFS RMS，两声道不相关）、Recover = Tame = 100、Speed Mid，跑 12 s，
        // 最后 2 s 内每 hop 读一次 getBandGainsDb，所有段 |G| < 0.5 dB。
        // 依据：规格「验收 2：粉噪开满 Recover / Tame → 所有 G[b] 收敛到 |G| < 0.5 dB（粉噪本就平滑，
        // 参考『歌自己』不会自激）」。12 s 远超 Mid 的参考慢平滑 2 s（6 个时间常数）。
        // 48 kHz、块 = hop = 512，每块读一次就是每 hop 读一次。
        void testPinkNoiseConverges()
        {
            beginTest ("pink noise with recover = tame = 100 converges to |G| < 0.5 dB");

            constexpr double sampleRate = 48000.0;
            constexpr double seconds = 12.0;

            BalanceModule balance;
            prepareBalance (balance, sampleRate, makeParams (100.0f, 100.0f, 0.0f, Speed::mid));

            const auto input = makePinkNoise (numChannels, (int) (seconds * sampleRate), -18.0f, 1001);
            expectWithinAbsoluteError (toDb (rmsOf (input[0])), -18.0f, 0.1f, "pink noise RMS");

            std::vector<BandGains> frames;
            const auto output = runBalance (balance, input, hopAt48k, &frames, (int) ((seconds - 2.0) * sampleRate));
            expect (allFinite (output));
            expect ((int) frames.size() >= 180, "expected roughly 2 s / 10.7 ms of readings, got " + juce::String ((int) frames.size()));

            float worst = 0.0f;
            int worstBand = 0;

            for (int band = 0; band < numBands; ++band)
            {
                const auto bandWorst = maxAbsGainOf (frames, band);

                if (bandWorst > worst)
                {
                    worst = bandWorst;
                    worstBand = band;
                }
            }

            logMessage ("  worst |G| in the last 2 s: " + juce::String (worst, 3) + " dB at "
                        + juce::String (BalanceModule::getBandCentreHz (worstBand), 0) + " Hz");

            for (int band = 0; band < numBands; ++band)
                expect (maxAbsGainOf (frames, band) < 0.5f,
                        "band " + juce::String (BalanceModule::getBandCentreHz (band), 0) + " Hz |G| = "
                            + juce::String (maxAbsGainOf (frames, band), 3) + " dB (limit 0.5)");
        }

        // 验收 3：对数正弦扫频 30 Hz → 16 kHz、20 s、−12 dBFS，Recover = Tame = 100（Mid）：
        // 每段相邻读数的增益变化方向翻转次数 / 秒 < 3。读数每块（512 样本 ≈ 10.7 ms = 一个 hop）一次。
        // 依据：规格「验收 3：正弦扫频 → 任一段增益不出现周期性振荡（相邻 hop 符号反复翻转 < 3 次 / 秒）」。
        // 「符号」按 G 的变化量 Δ 算（振荡 = G 来回摆）。规格防的是自激：判断环节有 ±1 dB 死区、每 hop 最多变 1 dB，
        // 真的自激必然是 ≥ 0.1 dB 量级的来回摆；|Δ| < 0.05 dB（死区与速率限制的 1/20）只可能是分析帧之间的
        // 舍入 / 窗函数扇贝纹波，不算「翻转」。日志里同时给 0.01 dB 门槛下的计数，方便复核两者是否一致。
        void testSweepDoesNotOscillate()
        {
            beginTest ("log sweep 30 Hz .. 16 kHz: gain direction reverses < 3 times per second in every band");

            constexpr double sampleRate = 48000.0;
            constexpr double seconds = 20.0;
            constexpr float reversalStepDb = 0.05f;

            BalanceModule balance;
            prepareBalance (balance, sampleRate, makeParams (100.0f, 100.0f, 0.0f, Speed::mid));

            const auto input = makeLogSweep (numChannels, sampleRate, 30.0, 16000.0, juce::Decibels::decibelsToGain (-12.0f), seconds);

            std::vector<BandGains> frames;
            const auto output = runBalance (balance, input, hopAt48k, &frames, 0);
            expect (allFinite (output));
            expect ((int) frames.size() >= 1800, "expected roughly 20 s / 10.7 ms of readings, got " + juce::String ((int) frames.size()));

            int worstReversals = 0;
            int worstBand = 0;
            int worstReversalsFine = 0;
            float deepestCut = 0.0f;

            for (int band = 0; band < numBands; ++band)
            {
                const auto reversals = countDirectionReversals (frames, band, reversalStepDb);
                worstReversalsFine = juce::jmax (worstReversalsFine, countDirectionReversals (frames, band, 0.01f));

                if (reversals > worstReversals)
                {
                    worstReversals = reversals;
                    worstBand = band;
                }

                for (const auto& frame : frames)
                    deepestCut = juce::jmin (deepestCut, frame[(size_t) band]);

                const auto perSecond = (double) reversals / seconds;
                expect (perSecond < 3.0, "band " + juce::String (BalanceModule::getBandCentreHz (band), 0) + " Hz reverses "
                                             + juce::String (perSecond, 2) + " times per second (limit 3)");
            }

            logMessage ("  worst band: " + juce::String (BalanceModule::getBandCentreHz (worstBand), 0) + " Hz, "
                        + juce::String ((double) worstReversals / seconds, 2) + " reversals / s (step >= 0.05 dB); worst at step >= 0.01 dB: "
                        + juce::String ((double) worstReversalsFine / seconds, 2) + " / s; deepest cut "
                        + juce::String (deepestCut, 2) + " dB");

            // 扫频里的纯音明显高于参考轮廓，Tame 必须真的动过：否则「不振荡」只是因为什么都没做。
            expect (deepestCut < -1.0f, "tame never engaged during the sweep (deepest cut " + juce::String (deepestCut, 2) + " dB)");
        }

        // 验收 4：1 kHz 正弦（−12 dBFS RMS，比 −24 dBFS RMS 的粉噪高 12 dB）+ 粉噪，Tame = 100、Recover = 0、Mid，
        // 跑 8 s（参考慢平滑 2 s 的 4 倍），最后 1 s 每 hop 检查：1 kHz 段（第 14 段）G ≤ −3 dB，相邻两段 |G| < 1 dB。
        // 依据：规格「验收 4：单一 1 kHz 正弦 + 粉噪，Tame = 100 → 1 kHz 段增益 ≤ −3 dB，相邻段 |G| < 1 dB（定位准确）」。
        // 量级核对：粉噪每 1/3 倍频程约 −24 − 10·log10(31) ≈ −39 dBFS，正弦比它高 27 dB；参考轮廓把正弦按 4/16 权重抹进
        // 1 kHz 段，偏差 D 远超 7 dB，Tame 封顶 −8 dB；相邻段的偏差为负，Recover = 0 时不动。
        // Hann 窗旁瓣到相邻段已低于 −45 dB，正弦不会「漏」进邻段。
        void testTamesOneKilohertzTone()
        {
            beginTest ("1 kHz tone over pink noise: band 14 <= -3 dB, neighbours within 1 dB (tame = 100, recover = 0)");

            constexpr double sampleRate = 48000.0;
            constexpr double seconds = 8.0;

            BalanceModule balance;
            prepareBalance (balance, sampleRate, makeParams (0.0f, 100.0f, 0.0f, Speed::mid));

            auto input = makePinkNoise (numChannels, (int) (seconds * sampleRate), -24.0f, 2002);
            const auto toneAmplitude = juce::Decibels::decibelsToGain (-12.0f) * juce::MathConstants<float>::sqrt2;
            addSignal (input, makeSine (numChannels, sampleRate, 1000.0, toneAmplitude, seconds));

            std::vector<BandGains> frames;
            const auto output = runBalance (balance, input, hopAt48k, &frames, (int) ((seconds - 1.0) * sampleRate));
            expect (allFinite (output));
            expect (! frames.empty());

            float highestToneGain = -1000.0f;

            for (const auto& frame : frames)
                highestToneGain = juce::jmax (highestToneGain, frame[(size_t) bandOf1kHz]);

            const auto lowerNeighbour = maxAbsGainOf (frames, bandOf1kHz - 1);
            const auto upperNeighbour = maxAbsGainOf (frames, bandOf1kHz + 1);

            logMessage ("  1 kHz band G (least negative over last 1 s): " + juce::String (highestToneGain, 2)
                        + " dB; neighbours |G| max " + juce::String (lowerNeighbour, 2) + " / " + juce::String (upperNeighbour, 2) + " dB");

            expect (highestToneGain <= -3.0f, "1 kHz band gain " + juce::String (highestToneGain, 2) + " dB (must be <= -3 dB)");
            expect (lowerNeighbour < 1.0f, "800 Hz band |G| " + juce::String (lowerNeighbour, 2) + " dB (limit 1)");
            expect (upperNeighbour < 1.0f, "1.25 kHz band |G| " + juce::String (upperNeighbour, 2) + " dB (limit 1)");

            // 顺带核对静态封顶：Tame = 100 时任何段都不能压过 8 dB。
            for (int band = 0; band < numBands; ++band)
                expect (maxAbsGainOf (frames, band) <= 8.0f + 1.0e-3f, "band " + juce::String (band) + " exceeds the 8 dB cap");
        }

        void testTameHasUsefulRange()
        {
            beginTest ("Tame middle and full settings remove a narrow resonance beyond the old 3 / 6 dB limits");
            constexpr double sampleRate = 48000.0;
            constexpr double seconds = 8.0;
            auto input = makePinkNoise (numChannels, (int) (seconds * sampleRate), -30.0f, 2010);
            addSignal (input, makeSine (numChannels, sampleRate, 1000.0, 0.2f, seconds));
            const auto from = (int) (7.0 * sampleRate);
            const auto originalTone = toneMagnitude (input, sampleRate, 1000.0, from);
            double previous = 0.0;
            for (auto amount : { 25.0f, 50.0f, 100.0f })
            {
                BalanceModule balance;
                prepareBalance (balance, sampleRate, makeParams (0.0f, amount));
                const auto output = runBalance (balance, input, maxBlock);
                const auto reduction = 20.0 * std::log10 (toneMagnitude (output, sampleRate, 1000.0, from) / originalTone);
                expect (allFinite (output));
                expect (reduction < previous - 0.5, "each step must audibly reduce the actual resonance further");
                if (amount == 50.0f) expect (reduction < -3.2, "half Tame has more useful range than the former 3 dB cap");
                if (amount == 100.0f) expect (reduction < -6.2, "full Tame has more useful range than the former 6 dB cap");
                logMessage ("  Tame " + juce::String (amount) + ": actual 1 kHz attenuation " + juce::String (reduction, 2) + " dB");
                previous = reduction;
            }
        }

        // 补充：getLatencySamples() == 0，且脉冲实测零延迟（Recover = Tame = 0，四个采样率）。
        // 依据：规格「零延迟（分析比音频晚 10 ～ 40 ms 是『反应滞后』，不是上报延迟）」+ 头文件 getLatencySamples 恒 0。
        void testLatencyIsZero()
        {
            beginTest ("latency is zero: reported and measured with an impulse");

            for (auto sampleRate : { 44100.0, 48000.0, 96000.0, 192000.0 })
            {
                BalanceModule balance;
                prepareBalance (balance, sampleRate, makeParams (0.0f, 0.0f));
                expectEquals (balance.getLatencySamples(), 0, "@" + juce::String (sampleRate));

                constexpr int position = 100;
                const auto input = makeImpulse (numChannels, 2048, position, 0.5f);
                const auto output = runBalance (balance, input, maxBlock);

                expect (allFinite (output));
                expectEquals (peakIndexOf (output[0]) - position, 0, "impulse @" + juce::String (sampleRate));
                expectEquals (output[0][(size_t) position], 0.5f, "impulse amplitude @" + juce::String (sampleRate));
            }
        }

        // 补充：Bypass 开 → 逐位等于输入，哪怕 Recover / Tame 开满、Brighten 开满、信号有明显的单频突出。
        // 依据：规格「零延迟旁通」+ 计划「完全旁通逐位等于输入」。bypass 在 prepare 前设好，交叉淡化从第 0 个样本起就是全干。
        void testBypassIsBitExact()
        {
            beginTest ("bypass is bit-exact input");

            constexpr double sampleRate = 48000.0;
            BalanceModule balance;
            prepareBalance (balance, sampleRate, makeParams (100.0f, 100.0f, 100.0f, Speed::fast, true));

            auto input = makeRichSignal (sampleRate, 2.0, 7);
            addSignal (input, makeSine (numChannels, sampleRate, 1000.0, 0.3f, 2.0));

            const auto output = runBalance (balance, input, maxBlock);
            expect (allFinite (output));
            expectEquals (maxDiffFrom (input, output, 0), 0.0f);
        }

        // 补充：播放中途打开 Bypass（Recover / Tame 开满、正弦突出让 Tame 确实在动）：25 ms 之后逐位等于输入。
        // 依据：计划「旁通：零延迟，与处理后信号 20 ms 交叉淡化；完全旁通逐位等于输入」——20 ms 淡化 + 5 ms 余量。
        // 切换前必须确实改变过信号（差 > 0），否则「逐位」不说明任何问题。
        void testBypassToggleMidStream()
        {
            beginTest ("bypass switched on mid-stream is bit-exact after 25 ms");

            constexpr double sampleRate = 48000.0;
            const auto active = makeParams (100.0f, 100.0f, 0.0f, Speed::fast);

            BalanceModule balance;
            prepareBalance (balance, sampleRate, active);

            auto input = makePinkNoise (numChannels, (int) (2.0 * sampleRate), -24.0f, 77);
            addSignal (input, makeSine (numChannels, sampleRate, 1000.0, 0.35f, 2.0));

            const auto switchAt = (int) (1.5 * sampleRate);
            Signal first, second;

            for (const auto& channel : input)
            {
                first.emplace_back (channel.begin(), channel.begin() + switchAt);
                second.emplace_back (channel.begin() + switchAt, channel.end());
            }

            auto output = runBalance (balance, first, maxBlock);
            expect (allFinite (output));

            auto bypassed = active;
            bypassed.bypass = true;
            balance.setParams (bypassed);
            const auto tail = runBalance (balance, second, maxBlock);
            expect (allFinite (tail));

            for (size_t ch = 0; ch < output.size(); ++ch)
                output[ch].insert (output[ch].end(), tail[ch].begin(), tail[ch].end());

            const auto changedBefore = maxAbsDifference (input, output, 0, switchAt);
            logMessage ("  max change before the switch: " + juce::String (toDb (changedBefore), 1) + " dBFS");
            expect (changedBefore > 1.0e-3f, "processing must have altered the signal before bypass was engaged");

            expectEquals (maxDiffFrom (input, output, switchAt + (int) (0.025 * sampleRate)), 0.0f, "bit-exact after bypass crossfade");
        }

        // 补充：不同块大小（1、17、512、2048，含超过 prepare 块大小的 2048）输出逐位一致；Recover / Tame / Brighten 全开、
        // 信号里有明显单频突出，让分析、快跟踪、参考平滑、速率限制、每 64 样本的系数更新与逐样本插值都参与。
        // 依据：头文件「样本数可以超过 prepare 时的块大小（内部切块）」+ 计划「增益按样本线性插值、每 64 样本重算系数」
        // ——hop 与系数更新都按样本计数，与块边界无关。
        void testBlockSizeIndependence()
        {
            beginTest ("output independent of block size");

            constexpr double sampleRate = 48000.0;
            const auto params = makeParams (100.0f, 100.0f, 30.0f, Speed::fast);

            auto input = makePinkNoise (numChannels, (int) (3.0 * sampleRate), -24.0f, 3);
            addSignal (input, makeSine (numChannels, sampleRate, 1000.0, 0.35f, 3.0));

            Signal reference;

            for (auto blockSize : { 512, 1, 17, 2048 })
            {
                BalanceModule balance;
                prepareBalance (balance, sampleRate, params);
                auto output = runBalance (balance, input, blockSize);
                expect (allFinite (output), "block size " + juce::String (blockSize));

                if (reference.empty())
                {
                    reference = output;
                    expect (maxDiffFrom (input, output, 0) > 1.0e-3f, "processing must actually change the signal");
                }
                else
                {
                    expectEquals (maxAbsDifference (reference, output, 0, (int) input.front().size()), 0.0f,
                                  "block size " + juce::String (blockSize));
                }
            }
        }

        // 补充：静音门。(a) 数字静音 → 每 hop 所有 G 精确为 0，输出全零；(b) −96 dBFS RMS 白噪（远低于 −60 dBFS 门）
        // → 所有 G 精确为 0，且输出逐位等于输入（G 从 0 出发、目标 0，速率限制下不会离开 0 → 系数 b = a）。
        // 依据：规格「静音门：L[b] < −60 dBFS 的段 G[b] → 0，不把底噪提起来」+ 计划「G 恒为 0 → 逐位直通」。
        void testSilenceGate()
        {
            beginTest ("silence gate: digital silence and -96 dBFS noise leave every G at 0");

            constexpr double sampleRate = 48000.0;
            constexpr double seconds = 3.0;

            struct Case { const char* name; Signal input; };
            Case cases[] =
            {
                { "digital silence", makeSilence (numChannels, (int) (seconds * sampleRate)) },
                // 均匀白噪的 RMS = 幅度 / √3，所以幅度 = 10^(−96/20) × √3。
                { "-96 dBFS noise",  makeNoise (numChannels, (int) (seconds * sampleRate), juce::Decibels::decibelsToGain (-96.0f) * std::sqrt (3.0f), 5) },
            };

            for (const auto& c : cases)
            {
                BalanceModule balance;
                prepareBalance (balance, sampleRate, makeParams (100.0f, 100.0f, 0.0f, Speed::fast));

                std::vector<BandGains> frames;
                const auto output = runBalance (balance, c.input, hopAt48k, &frames, 0);
                const auto tag = juce::String (c.name);

                expect (allFinite (output), tag);
                expectEquals (maxDiffFrom (c.input, output, 0), 0.0f, tag + ": output must be bit-exact input");

                float worst = 0.0f;
                for (const auto& frame : frames)
                    for (auto g : frame)
                        worst = juce::jmax (worst, std::abs (g));

                expectEquals (worst, 0.0f, tag + ": every band gain must stay at exactly 0 dB");
            }
        }

        // Recover/Tame 都关掉也要有独立色调：检查目标量，也对实际输入/输出做独立带通均方测量。
        // 不把相邻滤波器的叠加频响误认为单段参数；只要求真实输出方向正确、单调，30 档已有明确变化。
        void testBrightenTilt()
        {
            beginTest ("Brighten independently tilts real audio monotonically with Recover and Tame both off");

            constexpr double sampleRate = 48000.0;
            constexpr double seconds = 8.0;
            constexpr float tolerance = 0.15f;
            const auto input = makePinkNoise (numChannels, (int) (seconds * sampleRate), -18.0f, 4004);

            const float topTiltDb    = 3.0f * std::log10 (16000.0f / 1000.0f);
            const float bottomTiltDb = 3.0f * std::log10 (1000.0f * std::pow (2.0f, -14.0f / 3.0f) / 1000.0f);
            const auto from = (int) ((seconds - 1.0) * sampleRate);
            const auto inputLow = bandRms (input, sampleRate, 100.0f, from);
            const auto inputHigh = bandRms (input, sampleRate, 10000.0f, from);
            double previousTilt = -100.0;

            for (auto brighten : { -100.0f, -30.0f, 0.0f, 30.0f, 100.0f })
            {
                BalanceModule balance;
                prepareBalance (balance, sampleRate, makeParams (0.0f, 0.0f, brighten, Speed::mid));

                std::vector<BandGains> frames;
                const auto output = runBalance (balance, input, hopAt48k, &frames, (int) ((seconds - 1.0) * sampleRate));
                expect (allFinite (output), "brighten " + juce::String (brighten));

                const auto k = brighten / 100.0f;
                const auto top = meanGainOf (frames, numBands - 1);
                const auto pivot = meanGainOf (frames, bandOf1kHz);
                const auto bottom = meanGainOf (frames, 0);

                logMessage ("  brighten " + juce::String (brighten, 0) + ": 16 kHz " + juce::String (top, 2)
                            + " dB (expect " + juce::String (topTiltDb * k, 2) + "), 1 kHz " + juce::String (pivot, 2)
                            + " dB, 40 Hz " + juce::String (bottom, 2) + " dB (expect " + juce::String (bottomTiltDb * k, 2) + ")");

                const auto label = " @ brighten " + juce::String (brighten, 0);
                expectWithinAbsoluteError (top, topTiltDb * k, tolerance, "16 kHz band" + label);
                expectWithinAbsoluteError (pivot, 0.0f, tolerance, "1 kHz pivot" + label);
                expectWithinAbsoluteError (bottom, bottomTiltDb * k, tolerance, "40 Hz band" + label);
                const auto actualLow = 20.0 * std::log10 (bandRms (output, sampleRate, 100.0f, from) / inputLow);
                const auto actualHigh = 20.0 * std::log10 (bandRms (output, sampleRate, 10000.0f, from) / inputHigh);
                const auto actualTilt = actualHigh - actualLow;
                expect (actualTilt > previousTilt + 0.5, "actual high/low response is strictly monotone" + label);
                if (brighten > 0.0f)
                    expect (actualHigh > 0.5 && actualLow < -0.5 && actualTilt > 1.0, "positive Brighten has audible output tilt" + label);
                else if (brighten < 0.0f)
                    expect (actualHigh < -0.5 && actualLow > 0.5 && actualTilt < -1.0, "negative Brighten has audible output tilt" + label);
                else
                    expectEquals (maxDiffFrom (input, output, 0), 0.0f, "all three controls at zero remain bit-exact");
                previousTilt = actualTilt;
                logMessage ("  actual audio: 100 Hz " + juce::String (actualLow, 2) + " dB, 10 kHz "
                            + juce::String (actualHigh, 2) + " dB, difference " + juce::String (actualTilt, 2) + " dB");
            }
        }

        // 整体电平阶跃不应让全部频段一起被压 / 提（那是宽带压缩，不是频谱平衡）。粉噪 −30 dBFS，6 s 处整体
        // ±6 dB（Slow、Mid 再加 ±10 dB），12 s 处回到原电平；Recover = Tame = 100；统计 6 ～ 18 s 每 hop 每段的 G：
        // 任一段 |G| > 1 dB 的累计时间 ≤ 0.2 s（持续的全段压 / 提才是「宽带压缩」），瞬时最坏 |G| ≤ 1.5 dB
        // （Fast 2.0 dB：它的测量自由度最少、本来就更抖，规格说 Fast「可能听出起伏」）。
        // 依据：规格修订 2「整首歌变响 / 变轻时共同电平立刻跟上」。2026-09-25 评审 R1-01 实测修复前 Mid ±6 dB
        // 就有 2.6 dB / 0.76 s、±10 dB 有 4.9 dB 的全段下陷 / 抬升、持续 0.4 ～ 3 s（两条门槛都会红）；修复后
        // （C 用所有段同速的 60 ms 快电平、形状 S 的目标减去经本段动态滞后的 C）本用例实测 Slow ≤ 0.45 dB、
        // Mid ≤ 1.30 dB / 0.085 s、Fast ±6 dB ≤ 1.76 dB / 0.075 s。Fast ±10 dB 不设门槛（另一组粉噪实测
        // 2.4 dB / 0.33 s，留给试听）。
        void testLevelStepsDoNotPumpAllBands()
        {
            beginTest ("level steps (+-6 / +-10 dB) do not pump every band (recover = tame = 100)");

            constexpr double sampleRate = 48000.0;
            constexpr double hopSeconds = (double) hopAt48k / sampleRate;
            const auto base = makePinkNoise (numChannels, (int) (18.0 * sampleRate), -30.0f, 1234);

            struct Case { Speed speed; float stepDb; };
            const Case cases[] = { { Speed::slow,  6.0f }, { Speed::slow, -6.0f }, { Speed::slow, 10.0f }, { Speed::slow, -10.0f },
                                   { Speed::mid,   6.0f }, { Speed::mid,  -6.0f }, { Speed::mid,  10.0f }, { Speed::mid,  -10.0f },
                                   { Speed::fast,  6.0f }, { Speed::fast, -6.0f } };
            const char* speedNames[] = { "slow", "mid", "fast" };

            for (const auto& c : cases)
            {
                auto input = base;
                const auto gain = std::pow (10.0f, c.stepDb / 20.0f);

                for (auto& channel : input)
                    for (auto i = (size_t) (6.0 * sampleRate); i < (size_t) (12.0 * sampleRate); ++i)
                        channel[i] *= gain;

                BalanceModule balance;
                prepareBalance (balance, sampleRate, makeParams (100.0f, 100.0f, 0.0f, c.speed));

                std::vector<BandGains> frames;
                const auto output = runBalance (balance, input, hopAt48k, &frames, (int) (6.0 * sampleRate));

                float worst = 0.0f;
                double longestOverOneDb = 0.0;

                for (int band = 0; band < numBands; ++band)
                {
                    int hopsOver = 0;

                    for (const auto& frame : frames)
                    {
                        worst = juce::jmax (worst, std::abs (frame[(size_t) band]));
                        hopsOver += std::abs (frame[(size_t) band]) > 1.0f ? 1 : 0;
                    }

                    longestOverOneDb = juce::jmax (longestOverOneDb, hopsOver * hopSeconds);
                }

                const auto label = juce::String (speedNames[(int) c.speed]) + " step " + juce::String (c.stepDb, 0) + " dB";
                logMessage ("  " + label + ": worst |G| " + juce::String (worst, 2) + " dB, |G| > 1 dB for "
                            + juce::String (longestOverOneDb, 3) + " s");

                expect (allFinite (output), label);
                const auto peakLimit = c.speed == Speed::fast ? 2.0f : 1.5f;
                expect (worst <= peakLimit, label + ": worst |G| " + juce::String (worst, 2) + " dB > " + juce::String (peakLimit, 1) + " dB");
                expect (longestOverOneDb <= 0.2, label + ": |G| > 1 dB for " + juce::String (longestOverOneDb, 3) + " s > 0.2 s");
            }
        }

        // 补充：0 dBFS 白噪（最响、最不「平滑」的输入）、Speed Fast、Recover / Tame / Brighten 全开，44.1 kHz：
        // 输出无 NaN / Inf，动态修正与独立 Brighten 的每段合计仍在 −8 ～ +6 dB 内。
        void testLoudInputStaysFiniteAndCapped()
        {
            beginTest ("0 dBFS white noise at speed fast: finite output, combined gains stay within -8 .. +6 dB");

            constexpr double sampleRate = 44100.0;
            BalanceModule balance;
            prepareBalance (balance, sampleRate, makeParams (100.0f, 100.0f, -100.0f, Speed::fast));

            const auto input = makeNoise (numChannels, (int) (3.0 * sampleRate), 1.0f, 9);

            std::vector<BandGains> frames;
            const auto output = runBalance (balance, input, hopAt48k, &frames, 0);

            expect (allFinite (output));
            expect (samplePeakOf (output) < 4.0f, "output peak " + juce::String (samplePeakOf (output), 3) + " is implausibly large");

            float worst = 0.0f;
            for (const auto& frame : frames)
                for (auto g : frame)
                {
                    expect (std::isfinite (g), "band gain must be finite");
                    expect (g >= -8.0f - 1.0e-3f && g <= 6.0f + 1.0e-3f, "combined band gain remains bounded");
                    worst = juce::jmax (worst, std::abs (g));
                }

            logMessage ("  worst |G| " + juce::String (worst, 2) + " dB");
            expect (worst <= 8.0f + 1.0e-3f, "band gain " + juce::String (worst, 2) + " dB exceeds the combined cap");
        }

        // v0.5 R3：两声道打包滤波（AArch64 上 NEON float64x2）与单声道走的标量路径逐位相同。两声道送同一个信号，与同一信号
        // 的单声道模块比：两个声道的输出都必须逐位等于单声道输出，每块读到的 27 段 G 也逐位相同。
        // 依据：打包路径每一步运算与标量 processBiquad 一一对应（y = fma (b0, x, s1)，其余各自舍入），通道之间没有交叉运算；
        // 两声道相同时功率谱之和正好是单声道的 2 倍，按声道数取平均后逐位相同（2 的幂次缩放是精确的）。
        // Recover / Tame 开满、Brighten 30、Fast，梳状素材（二十多段在工作）；48k 与 192k，块 17 与 512。
        void testStereoMatchesMono()
        {
            beginTest ("stereo with identical channels is bit-exact with mono (packed vs scalar filter path)");

            const auto params = makeParams (100.0f, 100.0f, 30.0f, Speed::fast);

            for (auto sampleRate : { 48000.0, 192000.0 })
             for (auto blockSize : { 17, 512 })
            {
                const auto source = makeCombSignal (1, sampleRate, 2.0, 21);
                const Signal stereoInput { source[0], source[0] };

                BalanceModule stereo, mono;
                stereo.setParams (params);
                stereo.prepare ({ sampleRate, (juce::uint32) maxBlock, 2 });
                mono.setParams (params);
                mono.prepare ({ sampleRate, (juce::uint32) maxBlock, 1 });

                const auto stereoRun = runBlocks (stereo, stereoInput, blockSize, [&] (int) { return params; });
                const auto monoRun   = runBlocks (mono, source, blockSize, [&] (int) { return params; });

                const auto tag = "@" + juce::String (sampleRate) + " block " + juce::String (blockSize);
                expect (maxDiffFrom (stereoInput, stereoRun.output, 0) > 1.0e-3f, "processing must change the signal " + tag);
                expectEquals (countDifferentSamples (monoRun.output, stereoRun.output, 0), 0, "stereo channels vs mono " + tag);

                int gainMismatches = 0;

                for (size_t k = 0; k < monoRun.gains.size(); ++k)
                    gainMismatches += sameGains (monoRun.gains[k], stereoRun.gains[k]) ? 0 : 1;

                expectEquals (gainMismatches, 0, "published band gains " + tag);
            }
        }

        // 左右不同的输入（每个声道各自的粉红噪声 + 同一组正弦）：打包滤波（AArch64 的 NEON、x64 的 SSE2）与
        // setPackedFilteringForTesting (false) 的逐声道标量滤波比，每个声道每个样本、每块读到的 27 段 G 都逐位相同。
        // 上一个测试两声道相同，查不出两条通道的状态读错、写反这类错误；这里左右不同、跨上百块，每块都要把状态拆开读回、
        // 配对写出，错一处就不相同。3 声道时前两个声道打包、第 3 个走标量，一起对照。48k / 192k，块 17 / 512。
        void testPackedMatchesScalarWithDifferentChannels()
        {
            beginTest ("different channels: packed filter path (NEON / SSE2) is bit-exact with the scalar path, 2 and 3 channels");

            const auto params = makeParams (100.0f, 100.0f, 30.0f, Speed::fast);

            for (auto channelCount : { 2, 3 })
             for (auto sampleRate : { 48000.0, 192000.0 })
              for (auto blockSize : { 17, 512 })
            {
                const auto input = makeCombSignal (channelCount, sampleRate, 2.0, 37);

                BalanceModule packed, scalar;

                for (auto* module : { &packed, &scalar })
                {
                    module->setParams (params);
                    module->prepare ({ sampleRate, (juce::uint32) maxBlock, (juce::uint32) channelCount });
                }

                scalar.setPackedFilteringForTesting (false);

                const auto packedRun = runBlocks (packed, input, blockSize, [&] (int) { return params; });
                const auto scalarRun = runBlocks (scalar, input, blockSize, [&] (int) { return params; });

                const auto tag = juce::String (channelCount) + " ch @" + juce::String (sampleRate) + " block " + juce::String (blockSize);
                expect (countDifferentSamples (input, input, 0) > (int) input[0].size() / 2, "channels must differ " + tag);
                expect (maxDiffFrom (input, packedRun.output, 0) > 1.0e-3f, "processing must change the signal " + tag);
                expectEquals (countDifferentSamples (scalarRun.output, packedRun.output), 0, "packed vs scalar output " + tag);

                int gainMismatches = 0;

                for (size_t k = 0; k < scalarRun.gains.size(); ++k)
                    gainMismatches += sameGains (scalarRun.gains[k], packedRun.gains[k]) ? 0 : 1;

                expectEquals (gainMismatches, 0, "published band gains " + tag);
            }
        }

        // v0.5 R5：每 hop 的分析拆成三片（hop 后第 0 / 半个 / 一个重算间隔处，48 k 下第 0 / 32 / 64 个样本）与一口气做完
        // （setAnalysisSlicingForTesting (false)，即 v0.4 的做法）逐位相同：输出每个样本都相同；每块读到的 27 段 G，除了读数落在
        // hop 后第 1 ～ 1/8 hop 个样本之间的那些（拆片后最后一片在 hop 后第 1/8 hop 处才发布，这段里读到的还是上一帧——只晚发布，
        // 值不变），全部逐位相同。用最坏的情形：每一块都换参数（Recover / Tame 在 0 与满之间跳、Brighten、Speed 轮换），每隔几块
        // 切一次旁通——参数在分析片之间变了，拆片版也必须只用 hop 边界上的快照。四个采样率 × 块长 1（只 48k）/ 7 / 32 / 33 / 64 /
        // 100 / 512 / 2048（超过 prepare 的 512，内部切块）。
        // 依据：新目标最早在 hop 后第一个重算间隔那次系数重算才用到；hop 边界上那次重算的插值比例是 0，只用到片 0 提交的起点。
        // 阳性对照（T-032 R2-07 起，重算间隔随采样率翻倍）：每个采样率下落在发布窗内的读数里必须有拆片版读到上一帧的——
        // 否则「跳过窗内读数」只是白跳，测不出发布真的晚了 1/8 hop（窗口写小了会在窗外红、写大了靠这条红）。
        void testSlicedAnalysisMatchesUnsliced()
        {
            beginTest ("analysis sliced over three points in each hop is bit-exact with the one-shot analysis");

            const Params patterns[] = {
                makeParams (100.0f, 100.0f,   30.0f, Speed::fast),
                makeParams ( 40.0f,  40.0f,    0.0f, Speed::mid),
                makeParams (  0.0f,   0.0f,    0.0f, Speed::mid),
                makeParams (100.0f,   0.0f,  -60.0f, Speed::slow),
                makeParams (100.0f, 100.0f,   30.0f, Speed::fast, true),
                makeParams ( 20.0f,  80.0f,  100.0f, Speed::mid),
                makeParams (  0.0f, 100.0f,    0.0f, Speed::fast),
            };
            constexpr int numPatterns = (int) (sizeof (patterns) / sizeof (patterns[0]));
            auto paramsForBlock = [&] (int blockIndex) { return patterns[(blockIndex * 5 + blockIndex / 3) % numPatterns]; };

            for (auto sampleRate : { 44100.0, 48000.0, 96000.0, 192000.0 })
            {
                const auto input = makeCombSignal (numChannels, sampleRate, sampleRate > 100000.0 ? 1.5 : 2.5, 31);
                int delayedReadings = 0;   // 落在发布窗内且拆片版读到上一帧的读数（阳性对照，见上）

                for (auto blockSize : { 1, 7, 32, 33, 64, 100, 512, 2048 })
                {
                    if (blockSize == 1 && sampleRate != 48000.0)
                        continue;

                    BalanceModule sliced, oneShot;

                    for (auto* balance : { &sliced, &oneShot })
                    {
                        balance->setParams (paramsForBlock (0));
                        balance->prepare ({ sampleRate, (juce::uint32) maxBlock, (juce::uint32) numChannels });
                    }

                    oneShot.setAnalysisSlicingForTesting (false);

                    const auto a = runBlocks (sliced, input, blockSize, paramsForBlock);
                    const auto b = runBlocks (oneShot, input, blockSize, paramsForBlock);

                    const auto tag = "@" + juce::String (sampleRate) + " block " + juce::String (blockSize);
                    expectEquals (countDifferentSamples (a.output, b.output), 0, "output " + tag);

                    int compared = 0, mismatches = 0;

                    for (size_t k = 0; k < a.gains.size(); ++k)
                    {
                        if (isInsidePublishWindow (a.readPositions[k], sampleRate))
                        {
                            delayedReadings += sameGains (a.gains[k], b.gains[k]) ? 0 : 1;
                            continue;
                        }

                        ++compared;
                        mismatches += sameGains (a.gains[k], b.gains[k]) ? 0 : 1;
                    }

                    expect (compared > 0, "some readings must be compared " + tag);
                    expectEquals (mismatches, 0, "published band gains " + tag);
                }

                expect (delayedReadings > 0, "positive control: readings inside the " + juce::String (publishDelayFor (sampleRate))
                                                 + "-sample publish window really see the previous frame @" + juce::String (sampleRate));
            }
        }

        // 补充（v0.5）：192 kHz（8192 点 FFT、hop 2048）下，块长 512 / 1 / 16 / 32 / 33 / 64 / 100 / 2048 输出逐位一致
        // （原「分块无关」只测 48 kHz）。分析拆片、系数重算、状态归零都落在从 reset 起算的固定位置上，与块边界无关。
        // 梳状素材、Recover / Tame 开满、Brighten 30、Fast，参数不变（块边界不同，参数只能在共同的位置变——这里干脆不变）。
        void testBlockSizeIndependenceAtHighRate()
        {
            beginTest ("output independent of block size at 192 kHz, including tiny blocks");

            constexpr double sampleRate = 192000.0;
            const auto params = makeParams (100.0f, 100.0f, 30.0f, Speed::fast);
            const auto input = makeCombSignal (numChannels, sampleRate, 2.0, 41);

            Signal reference;

            for (auto blockSize : { 512, 1, 16, 32, 33, 64, 100, 2048 })
            {
                BalanceModule balance;
                prepareBalance (balance, sampleRate, params);
                const auto run = runBlocks (balance, input, blockSize, [&] (int) { return params; });
                expect (allFinite (run.output), "block size " + juce::String (blockSize));

                if (reference.empty())
                {
                    reference = run.output;
                    expect (maxDiffFrom (input, run.output, 0) > 1.0e-3f, "processing must actually change the signal");
                }
                else
                {
                    expectEquals (countDifferentSamples (reference, run.output), 0, "block size " + juce::String (blockSize));
                }
            }
        }

        // v0.5 R4（修 v0.4 的 bug）：Recover / Tame 开过再拧回 0（100 / 100，以及 GLAZE 100 带来的 40 / 40——GLAZE 在链里
        // 叠加偏移，Balance 看到的就是这两个实际值），1.5 s 后所有段都停下（getActiveFilterCount() == 0）、输出逐位等于输入。
        // v0.4 实测：拧回 0 后状态卡在约 1e-13 的舍入噪声上（极点贴着单位圆，归零门槛 1e-30 永远到不了），mix 素材 26 s 后
        // 还有 5 段在跑，输出偶尔差 1 LSB。修法：增益精确为 0 的段状态低于 1e-10 就归零。
        // 素材：粉噪 + 41 / 55 Hz（最贴单位圆、最难停的低频段）+ 440 Hz + 3.3 kHz 正弦；48k 与 192k；开 4 s 再关 4 s。
        // 开着时必须真的改变过信号、有多段在工作（否则「逐位」「停下」不说明问题）。实测拧回 0 后约 0.5 s 全部停下，门槛 1.5 s。
        void testReturnToZeroStopsAllBands()
        {
            beginTest ("after recover / tame return to 0 every band stops and the output is bit-exact input again");

            constexpr double onSeconds = 4.0, offSeconds = 4.0, settleSeconds = 1.5;

            for (auto sampleRate : { 48000.0, 192000.0 })
             for (auto amount : { 100.0f, 40.0f })
            {
                auto input = makePinkNoise (numChannels, (int) ((onSeconds + offSeconds) * sampleRate), -24.0f, 51);
                addSignal (input, makeSine (numChannels, sampleRate, 41.2, 0.2f, onSeconds + offSeconds));
                addSignal (input, makeSine (numChannels, sampleRate, 55.0, 0.15f, onSeconds + offSeconds, 0.7));
                addSignal (input, makeSine (numChannels, sampleRate, 440.0, 0.05f, onSeconds + offSeconds, 1.3));
                addSignal (input, makeSine (numChannels, sampleRate, 3300.0, 0.03f, onSeconds + offSeconds, 2.1));

                const auto on  = makeParams (amount, amount, 0.0f, Speed::mid);
                const auto off = makeParams (0.0f, 0.0f, 0.0f, Speed::mid);
                const auto switchBlock = (int) (onSeconds * sampleRate) / maxBlock;
                const auto settledBlock = (int) ((onSeconds + settleSeconds) * sampleRate) / maxBlock;

                BalanceModule balance;
                prepareBalance (balance, sampleRate, on);

                const auto numSamples = (int) input.front().size();
                auto output = input;
                juce::AudioBuffer<float> block (numChannels, maxBlock);
                int mostActiveWhileOn = 0, activeAfterSettle = 0, differentAfterSettle = 0;
                double stoppedAfterSeconds = -1.0;

                for (int start = 0, blockIndex = 0; start < numSamples; start += maxBlock, ++blockIndex)
                {
                    const auto length = juce::jmin (maxBlock, numSamples - start);
                    block.setSize (numChannels, length, false, false, true);

                    for (int ch = 0; ch < numChannels; ++ch)
                        block.copyFrom (ch, 0, input[(size_t) ch].data() + start, length);

                    balance.setParams (blockIndex < switchBlock ? on : off);
                    balance.process (block);

                    for (int ch = 0; ch < numChannels; ++ch)
                        std::copy (block.getReadPointer (ch), block.getReadPointer (ch) + length,
                                   output[(size_t) ch].begin() + start);

                    const auto active = balance.getActiveFilterCount();

                    if (blockIndex < switchBlock)
                        mostActiveWhileOn = juce::jmax (mostActiveWhileOn, active);
                    else if (active == 0 && stoppedAfterSeconds < 0.0)
                        stoppedAfterSeconds = (double) (start + length) / sampleRate - onSeconds;

                    if (blockIndex >= settledBlock)
                    {
                        activeAfterSettle = juce::jmax (activeAfterSettle, active);

                        for (int ch = 0; ch < numChannels; ++ch)
                            for (int i = 0; i < length; ++i)
                            {
                                const auto x = input[(size_t) ch][(size_t) (start + i)];
                                const auto y = output[(size_t) ch][(size_t) (start + i)];
                                differentAfterSettle += std::memcmp (&x, &y, sizeof (float)) != 0 ? 1 : 0;
                            }
                    }
                }

                const auto tag = "@" + juce::String (sampleRate) + " recover / tame " + juce::String (amount, 0);
                logMessage ("  " + tag + ": up to " + juce::String (mostActiveWhileOn) + " bands active while on; all stopped "
                            + juce::String (stoppedAfterSeconds, 2) + " s after returning to 0");

                expect (maxAbsDifference (input, output, 0, switchBlock * maxBlock) > 1.0e-3f, "processing must have changed the signal " + tag);
                expect (mostActiveWhileOn >= 5, "several bands must have been active while on " + tag);
                expect (stoppedAfterSeconds >= 0.0 && stoppedAfterSeconds <= settleSeconds,
                        "all bands must stop within " + juce::String (settleSeconds, 1) + " s " + tag);
                expectEquals (activeAfterSettle, 0, "no band may restart after settling " + tag);
                expectEquals (differentAfterSettle, 0, "output must be bit-exact input after settling " + tag);
            }
        }
    };

    static BalanceTests balanceTests;
}

//==============================================================================
// v0.1.7.0 修坯 Clay（设计规格-Clay-分析决策与验收 1.1、第 6 节验收 1 / 3）：Balance 每帧快照新增的左右交叉功率 bandCross。
// 按「已有测试文件只在末尾追加」另开一个测试类 BalanceCrossPower（上面 BalanceModule 类一行不改；两类一起跑：
// celadon_tests --only BalanceModule,BalanceCrossPower），复用上面匿名命名空间里的工具（同一个编译单元）。
// 只按规格与 BalanceModule.h 的公开接口写，每条断言的依据写在对应函数的注释里。
namespace celadon::test
{
    namespace
    {
        using FrameSnapshot = BalanceModule::FrameSnapshot;

        struct FramedRun
        {
            Signal output;
            std::vector<FrameSnapshot> frames;   // 按发布顺序读出的全部帧
            std::vector<BandGains> gains;        // 每块处理完读一次的 27 段 G（界面口径）
            std::uint64_t allocations = 0;       // process 与 readFramesSince 经 operator new 分配的次数（TestMain.cpp 计数）
            int droppedReads = 0;                // readFramesSince 报告丢帧的次数（每块都读，应为 0）
        };

        // 把整段信号按块送进 Balance：blockSize > 0 时固定块长，否则按 blockSeed 取 1 ～ 1500 的随机块长（跨 hop、跨分析片、
        // 超过 prepare 的 512）。每块处理完先把新发布的帧全部读出来，再读一次 27 段 G。声道数取信号自己的（可以少于 prepare
        // 时的声道数）。只给 process / readFramesSince 计分配；把帧存进 vector 的分配在计数之外。
        FramedRun runCollectingFrames (BalanceModule& balance, const Signal& input, int blockSize, juce::int64 blockSeed = 1)
        {
            constexpr int maxRandomBlock = 1500;
            constexpr int readChunk = 8;

            const auto channels = (int) input.size();
            const auto totalSamples = (int) input.front().size();

            FramedRun run;
            run.output = input;
            juce::AudioBuffer<float> block (channels, blockSize > 0 ? blockSize : maxRandomBlock);
            std::array<FrameSnapshot, (size_t) readChunk> chunk {};
            juce::Random random (blockSeed);
            std::int64_t cursor = 0;

            for (int start = 0; start < totalSamples;)
            {
                const auto count = juce::jmin (blockSize > 0 ? blockSize : 1 + random.nextInt (maxRandomBlock), totalSamples - start);
                block.setSize (channels, count, false, false, true);

                for (int ch = 0; ch < channels; ++ch)
                    block.copyFrom (ch, 0, input[(size_t) ch].data() + start, count);

                auto before = allocationCountOnThisThread();
                balance.process (block);
                run.allocations += allocationCountOnThisThread() - before;

                for (int got = readChunk; got == readChunk;)
                {
                    before = allocationCountOnThisThread();
                    const auto result = balance.readFramesSince (cursor, chunk.data(), readChunk);
                    run.allocations += allocationCountOnThisThread() - before;
                    run.droppedReads += result < 0 ? 1 : 0;
                    got = std::abs (result);
                    run.frames.insert (run.frames.end(), chunk.begin(), chunk.begin() + got);
                }

                for (int ch = 0; ch < channels; ++ch)
                    std::copy (block.getReadPointer (ch), block.getReadPointer (ch) + count,
                               run.output[(size_t) ch].begin() + start);

                BandGains gains {};
                balance.getBandGainsDb (gains);
                run.gains.push_back (gains);
                start += count;
            }

            return run;
        }

        // 由一帧推 M / S 两路的段功率（设计规格-Clay-分析决策与验收 1.1：P_M = (P + X) / 2、P_S = (P − X) / 2），在 double 里算。
        double midPowerOf (const FrameSnapshot& frame, size_t band)
        {
            return 0.5 * ((double) frame.bandPower[band] + (double) frame.bandCross[band]);
        }

        double sidePowerOf (const FrameSnapshot& frame, size_t band)
        {
            return 0.5 * ((double) frame.bandPower[band] - (double) frame.bandCross[band]);
        }

        // 一组段（firstBand ～ lastBand）在全部帧上的 Σ X / Σ P：与画像 lowCorrelation / highCorrelation「先求和再相除」同一口径。
        double correlationOf (const std::vector<FrameSnapshot>& frames, int firstBand, int lastBand)
        {
            double cross = 0.0, power = 0.0;

            for (const auto& frame : frames)
                for (int band = firstBand; band <= lastBand; ++band)
                {
                    cross += (double) frame.bandCross[(size_t) band];
                    power += (double) frame.bandPower[(size_t) band];
                }

            return power > 0.0 ? cross / power : 0.0;
        }

        // 两帧逐位相同：时间戳与三组数组（按位比，连 −0 / +0 也算不同）。
        bool sameSnapshot (const FrameSnapshot& a, const FrameSnapshot& b)
        {
            return a.endSample == b.endSample
                && std::memcmp (a.bandPowerDb.data(), b.bandPowerDb.data(), sizeof (a.bandPowerDb)) == 0
                && std::memcmp (a.bandPower.data(), b.bandPower.data(), sizeof (a.bandPower)) == 0
                && std::memcmp (a.bandCross.data(), b.bandCross.data(), sizeof (a.bandCross)) == 0;
        }

        // 两串帧公共前缀里不逐位相同的帧数（拆片时最后一帧晚 1/8 hop 发布，输入正好停在那之前时会少一帧，所以只比公共前缀）。
        int countDifferentSnapshots (const std::vector<FrameSnapshot>& a, const std::vector<FrameSnapshot>& b)
        {
            int different = 0;

            for (size_t k = 0; k < a.size() && k < b.size(); ++k)
                different += sameSnapshot (a[k], b[k]) ? 0 : 1;

            return different;
        }

        // FNV-1a 64 位摘要，按内存字节序（AArch64 与 x86-64 都是小端）累进。只用来比较同一台机器、同一套编译选项下的两次构建
        // （不同平台的 FFT 引擎与乘加融合不同，摘要本来就不同）。
        struct Fnv1a
        {
            juce::uint64 value = 14695981039346656037ull;   // FNV-1a 64 位初值（offset basis）

            void addBytes (const void* data, size_t size) noexcept
            {
                const auto* bytes = static_cast<const unsigned char*> (data);

                for (size_t i = 0; i < size; ++i)
                {
                    value ^= (juce::uint64) bytes[i];
                    value *= 1099511628211ull;                  // FNV 64 位素数
                }
            }
        };

        // 输出：每个样本的位模式（声道 0 全部，再声道 1 全部）。
        juce::uint64 digestOf (const Signal& signal)
        {
            Fnv1a digest;

            for (const auto& channel : signal)
                digest.addBytes (channel.data(), channel.size() * sizeof (float));

            return digest.value;
        }

        // G：每次读数的 27 个 float。
        juce::uint64 digestOf (const std::vector<BandGains>& readings)
        {
            Fnv1a digest;

            for (const auto& gains : readings)
                digest.addBytes (gains.data(), sizeof (gains));

            return digest.value;
        }

        // 帧：只摘改动前就有的字段（endSample、bandPowerDb、bandPower），改动前后的构建可以直接比。
        juce::uint64 digestOf (const std::vector<FrameSnapshot>& frames)
        {
            Fnv1a digest;

            for (const auto& frame : frames)
            {
                digest.addBytes (&frame.endSample, sizeof (frame.endSample));
                digest.addBytes (frame.bandPowerDb.data(), sizeof (frame.bandPowerDb));
                digest.addBytes (frame.bandPower.data(), sizeof (frame.bandPower));
            }

            return digest.value;
        }
    }

    class BalanceCrossPowerTests  : public juce::UnitTest
    {
    public:
        BalanceCrossPowerTests() : juce::UnitTest ("BalanceCrossPower", "Celadon") {}

        void runTest() override
        {
            testMidOnlyAndSideOnly();
            testMidSideReconstruction();
            testUncorrelatedNoise();
            testMonoAndOtherChannelCounts();
            testSlicingSettingsAndBypass();
            testGainAndOutputUnchanged();
        }

    private:
        // 只有 M / 只有 S。依据：设计规格-Clay-分析决策与验收 1.1——bandPower = Σ w·(|X_L|² + |X_R|²) / 2、
        // bandCross = Σ w·Re(X_L · conj(X_R))，P_M = (P + X) / 2、P_S = (P − X) / 2。
        //  - 只有 M（左右逐位相同）：每个 bin 的 Re(X_L · conj(X_R)) 与 |X|² 是同一个值（float × float 在 double 里精确，两者都只在
        //    相加时舍入一次），X 与 P 只差段内求和的舍入（~1e-16）；快照存 float，最多再差 1 个 float ulp（相对 ≤ 2^-23 ≈ 1.2e-7）。
        //    门槛取规格的相对 1e-6。P_M 还要等于同一信号单声道分析的段功率：两声道相同时功率谱之和正好是单声道的 2 倍，按声道数
        //    取平均后逐位相同（testStereoMatchesMono 的同一依据），同一门槛；P_S 相对 P ≤ 1e-6。
        //  - 只有 S（R = −L，float 取负精确）：FFT 是线性运算、舍入对称，X_R = −X_L（逐值），于是 X = −P、P_S = 单声道段功率、
        //    P_M 相对 P ≤ 1e-6。
        // 四个采样率（FFT 2048 / 2048 / 4096 / 8192 点）、粉噪 −20 dBFS、1 s，每帧每段都比。阳性对照：每帧每段的单声道段功率
        // 都 > 0（真有东西可比），三次运行发布同样多、时间戳相同的帧。
        void testMidOnlyAndSideOnly()
        {
            beginTest ("mid-only input: bandCross = bandPower, side-only: bandCross = -bandPower; P_M / P_S equal a mono analysis");

            constexpr double relativeTolerance = 1.0e-6;

            for (auto sampleRate : { 44100.0, 48000.0, 96000.0, 192000.0 })
            {
                const auto source = makePinkNoise (1, (int) sampleRate, -20.0f, 7101);
                auto inverted = source[0];

                for (auto& sample : inverted)
                    sample = -sample;

                const Signal midOnly { source[0], source[0] };
                const Signal sideOnly { source[0], inverted };

                BalanceModule mono, mid, side;
                mono.prepare ({ sampleRate, (juce::uint32) maxBlock, 1 });
                mid.prepare ({ sampleRate, (juce::uint32) maxBlock, 2 });
                side.prepare ({ sampleRate, (juce::uint32) maxBlock, 2 });

                const auto monoRun = runCollectingFrames (mono, source, maxBlock);
                const auto midRun  = runCollectingFrames (mid, midOnly, maxBlock);
                const auto sideRun = runCollectingFrames (side, sideOnly, maxBlock);

                const auto tag = "@" + juce::String (sampleRate);
                expect ((int) monoRun.frames.size() >= 40, "enough frames to compare " + tag);
                expect (midRun.frames.size() == monoRun.frames.size() && sideRun.frames.size() == monoRun.frames.size(),
                        "the three runs publish the same number of frames " + tag);

                int silentCells = 0, misalignedFrames = 0, midErrors = 0, sideErrors = 0;
                double worstMid = 0.0, worstSide = 0.0;

                for (size_t k = 0; k < monoRun.frames.size() && k < midRun.frames.size() && k < sideRun.frames.size(); ++k)
                {
                    const auto& reference = monoRun.frames[k];
                    const auto& m = midRun.frames[k];
                    const auto& s = sideRun.frames[k];
                    misalignedFrames += (m.endSample == reference.endSample && s.endSample == reference.endSample) ? 0 : 1;

                    for (size_t band = 0; band < (size_t) numBands; ++band)
                    {
                        const auto expected = (double) reference.bandPower[band];

                        if (! (expected > 0.0))
                        {
                            ++silentCells;
                            continue;
                        }

                        // 只有 M：X = P、P_M = 单声道段功率、P_S = 0；只有 S：X = −P、P_S = 单声道段功率、P_M = 0（都相对该段功率）。
                        const auto midError = juce::jmax (std::abs ((double) m.bandCross[band] - (double) m.bandPower[band]),
                                                          std::abs (midPowerOf (m, band) - expected),
                                                          std::abs (sidePowerOf (m, band))) / expected;
                        const auto sideError = juce::jmax (std::abs ((double) s.bandCross[band] + (double) s.bandPower[band]),
                                                           std::abs (sidePowerOf (s, band) - expected),
                                                           std::abs (midPowerOf (s, band))) / expected;
                        worstMid = juce::jmax (worstMid, midError);
                        worstSide = juce::jmax (worstSide, sideError);
                        midErrors += (midError < relativeTolerance) ? 0 : 1;
                        sideErrors += (sideError < relativeTolerance) ? 0 : 1;
                    }
                }

                logMessage ("  " + tag + ": worst relative error, mid only " + juce::String (worstMid, 3, true)
                            + ", side only " + juce::String (worstSide, 3, true));
                expectEquals (misalignedFrames, 0, "frame timestamps line up " + tag);
                expectEquals (silentCells, 0, "positive control: every band of every frame carries power " + tag);
                expectEquals (midErrors, 0, "mid only: X = P, P_M = mono band power, P_S = 0 " + tag);
                expectEquals (sideErrors, 0, "side only: X = -P, P_S = mono band power, P_M = 0 " + tag);
            }
        }

        // 一般的 M + S。依据：同上 1.1。L = m + s、R = m − s 时逐个 bin 精确地有 (|X_L|² + |X_R|²) / 2 = |X_m|² + |X_s|²、
        // Re(X_L · conj(X_R)) = |X_m|² − |X_s|²（交叉项 X_s·conj(X_m) − X_m·conj(X_s) 是纯虚数），所以每一帧（不只是期望值）都有
        // P_M = |X_m|² 的段和、P_S = |X_s|² 的段和，与 m、s 相关与否无关——拿 m、s 各自的单声道分析当参照，逐帧逐段比。
        // 误差只来自 float：L、R 相加时的舍入（相对 6e-8）与 float FFT 的舍入（≈ ε·√log2 N，粉噪最高几段比全谱均值低约 7 dB，
        // 相对段功率 ~1e-6）。门槛取该段总功率 P 的 1e-4：比舍入大两个数量级；公式错（少除 2、Re 写成 Im、bin 错位、权重错段）
        // 都是 O(1) 的误差。m、s 取两路独立粉噪（−20 / −26 dBFS）。阳性对照：每帧每段 P > 0；全部帧段上 Σ P_S / Σ P 在 0.15 ～ 0.25
        // （S 比 M 低 6 dB，期望 0.25 / 1.25 = 0.2——两路都不小，不是「一路为 0」的平凡情形）。
        void testMidSideReconstruction()
        {
            beginTest ("P_M = (P + X) / 2 and P_S = (P - X) / 2 reproduce the band powers of M and S in every frame");

            constexpr double tolerance = 1.0e-4;

            for (auto sampleRate : { 44100.0, 48000.0, 96000.0, 192000.0 })
            {
                const auto totalSamples = (int) sampleRate;
                const auto mid = makePinkNoise (1, totalSamples, -20.0f, 7201);
                const auto side = makePinkNoise (1, totalSamples, -26.0f, 7202);
                auto stereo = makeSilence (2, totalSamples);

                for (size_t i = 0; i < (size_t) totalSamples; ++i)
                {
                    stereo[0][i] = mid[0][i] + side[0][i];
                    stereo[1][i] = mid[0][i] - side[0][i];
                }

                BalanceModule midAlone, sideAlone, both;
                midAlone.prepare ({ sampleRate, (juce::uint32) maxBlock, 1 });
                sideAlone.prepare ({ sampleRate, (juce::uint32) maxBlock, 1 });
                both.prepare ({ sampleRate, (juce::uint32) maxBlock, 2 });

                const auto midRun  = runCollectingFrames (midAlone, mid, maxBlock);
                const auto sideRun = runCollectingFrames (sideAlone, side, maxBlock);
                const auto run     = runCollectingFrames (both, stereo, maxBlock);

                const auto tag = "@" + juce::String (sampleRate);
                expect ((int) run.frames.size() >= 40 && run.frames.size() == midRun.frames.size()
                            && run.frames.size() == sideRun.frames.size(),
                        "the three runs publish the same frames " + tag);

                int errors = 0, emptyCells = 0;
                double worst = 0.0, sideSum = 0.0, powerSum = 0.0;

                for (size_t k = 0; k < run.frames.size() && k < midRun.frames.size() && k < sideRun.frames.size(); ++k)
                {
                    const auto& frame = run.frames[k];

                    for (size_t band = 0; band < (size_t) numBands; ++band)
                    {
                        const auto power = (double) frame.bandPower[band];

                        if (! (power > 0.0))
                        {
                            ++emptyCells;
                            continue;
                        }

                        const auto error = juce::jmax (std::abs (midPowerOf (frame, band) - (double) midRun.frames[k].bandPower[band]),
                                                       std::abs (sidePowerOf (frame, band) - (double) sideRun.frames[k].bandPower[band]))
                                           / power;
                        worst = juce::jmax (worst, error);
                        errors += (error < tolerance) ? 0 : 1;
                        sideSum += sidePowerOf (frame, band);
                        powerSum += power;
                    }
                }

                const auto sideShare = powerSum > 0.0 ? sideSum / powerSum : 0.0;
                logMessage ("  " + tag + ": worst reconstruction error " + juce::String (worst, 3, true)
                            + " of P, side share " + juce::String (sideShare, 3) + " (expected about 0.2)");
                expectEquals (emptyCells, 0, "positive control: every band of every frame carries power " + tag);
                expectEquals (errors, 0, "P_M / P_S match the mono analyses of M / S within 1e-4 of P " + tag);
                expect (sideShare > 0.15 && sideShare < 0.25, "positive control: both M and S are substantial " + tag);
            }
        }

        // 两路不相关噪声 ρ ≈ 0（验收 3）。ρ 取「全部帧的 Σ X / Σ P」（画像 lowCorrelation / highCorrelation 的口径），分三组：
        // 全部 27 段、40 ～ 125 Hz（0 ～ 5 段）、2 ～ 16 kHz（17 ～ 26 段）。
        // 阈值的统计依据（48 kHz、2048 点 Hann、hop 512、均匀白噪 20 s ≈ 1872 帧）：左右独立时一个 bin 的 Re(X_L · conj(X_R))
        // 均值 0、方差 = E|X_L|²·E|X_R|² / 2（只用到二阶矩，与噪声分布无关）。Hann 让相邻 bin 相关（窗平方的频谱系数
        // 0.375 : −0.25 : 0.0625），n 个整 bin 的段和上单帧 ρ 的标准差 ≈ √(0.1367·n) / (0.375·n) ≈ 0.99 / √n；相邻帧重叠 75 %，
        // 帧间相关（宽带：Σ w²(n)·w²(n + hop) / Σ w⁴ = 0.486，隔一帧 0.043）让 K 帧平均的方差放大约 2.06 倍。
        // 于是 2 ～ 16 kHz（约 690 个 bin）σ ≈ 0.0375 · √(2.06 / 1872) ≈ 0.0012；全部段（约 766 个 bin，白噪下由高段主导）
        // σ ≈ 0.0012；40 ～ 125 Hz 只有约 4.5 个 bin（边界 bin 按重叠计权），单帧 σ ≈ 0.43，平均后 ≈ 0.014。
        // 门槛：全部段与高频组 |ρ| < 0.01（≥ 8σ），低频组 |ρ| < 0.1（≥ 7σ）。
        // 阳性对照：同样两路噪声按 R = a·n1 + b·n2（a² + b² = 1）混合，ρ 的真值 = a（a = ±0.5，即 cos 60° / cos 120°），同样的门槛
        // 内测准（这时 δρ 的方差 = b⁴ / 2 倍单 bin 量，比不相关时还小）。另断言每帧每段有限且 |X| ≤ P（发布前的钳位）。
        void testUncorrelatedNoise()
        {
            beginTest ("uncorrelated noise reads rho near 0 within a statistical bound; a known mix reads cos(phi)");

            constexpr double sampleRate = 48000.0;
            const auto totalSamples = (int) (20.0 * sampleRate);
            const auto noise = makeNoise (2, totalSamples, 0.1f, 7301);   // 两声道独立的均匀白噪（RMS 0.1 / √3 ≈ −24.8 dBFS）

            struct Group { const char* label; int firstBand; int lastBand; double bound; };
            const Group groups[] = { { "all bands", 0, numBands - 1, 0.01 },
                                     { "40-125 Hz", 0, 5, 0.1 },
                                     { "2-16 kHz", 17, numBands - 1, 0.01 } };

            struct Mix { const char* label; float a; float b; double expectedRho; };
            const Mix mixes[] = { { "independent", 0.0f, 1.0f, 0.0 },
                                  { "cos 60 deg mix", 0.5f, 0.8660254f, 0.5 },
                                  { "cos 120 deg mix", -0.5f, 0.8660254f, -0.5 } };

            for (const auto& mix : mixes)
            {
                auto input = noise;

                for (size_t i = 0; i < (size_t) totalSamples; ++i)
                    input[1][i] = mix.a * noise[0][i] + mix.b * noise[1][i];

                BalanceModule balance;
                balance.prepare ({ sampleRate, (juce::uint32) maxBlock, 2 });
                const auto run = runCollectingFrames (balance, input, maxBlock);
                int outOfRange = 0;

                for (const auto& frame : run.frames)
                    for (size_t band = 0; band < (size_t) numBands; ++band)
                        outOfRange += (std::isfinite (frame.bandCross[band])
                                       && std::abs (frame.bandCross[band]) <= frame.bandPower[band]) ? 0 : 1;

                const auto mixLabel = juce::String (mix.label);
                expect ((int) run.frames.size() >= 1800, "about 20 s / 10.7 ms of frames (" + mixLabel + ")");
                expectEquals (outOfRange, 0, "finite and |X| <= P in every band of every frame (" + mixLabel + ")");

                for (const auto& group : groups)
                {
                    const auto rho = correlationOf (run.frames, group.firstBand, group.lastBand);
                    const auto label = mixLabel + ", " + group.label + ": rho = " + juce::String (rho, 5)
                                       + " (expected " + juce::String (mix.expectedRho, 2) + " +- " + juce::String (group.bound, 2) + ")";
                    logMessage ("  " + label);
                    expect (std::abs (rho - mix.expectedRho) < group.bound, label);
                }
            }
        }

        // 单声道与其他声道数。依据：BalanceModule.h「单声道时 bandCross = bandPower」；分析声道数为 1 时 bandCross 直接拷贝
        // bandPower，快照里两个 float 由同一个 double 转来——逐位相等。
        //  1. 按单声道 prepare（随机块长）。
        //  2. 按立体声 prepare，先送约 0.5 s 反相立体声、再只送单声道块。切换前的帧 X ≈ −P（阳性对照：立体声交叉确实在算，与
        //     单声道的拷贝截然不同）；切换后本帧分析声道数 = 1，而声道 1 的帧缓冲里还留着反相的旧数据，绝不能拿来算——切换后
        //     发布的帧必须 X == P 逐位。hop 边界上的分析看的是上一块送进来的声道数，所以 endSample == 切换点的那一帧还是立体声帧，
        //     endSample > 切换点的帧才是单声道帧（块长 = hop = 512，切换点在 hop 边界上）。
        //  3. 三声道（处理器不会给，只验安全）：只取声道 0 / 1。三个声道都相同 → X = P（相对 1e-6：三个相同功率取平均只多一两次
        //     舍入）；声道 2 换成独立噪声 → 仍有限且 |X| ≤ P（钳位保证）。
        void testMonoAndOtherChannelCounts()
        {
            beginTest ("mono analysis publishes bandCross == bandPower bit-exactly; stale or extra channels are handled safely");

            constexpr double sampleRate = 48000.0;
            const auto totalSamples = (int) sampleRate;
            const auto source = makePinkNoise (1, totalSamples, -20.0f, 7401);

            const auto crossEqualsPower = [] (const FrameSnapshot& frame)
            {
                return std::memcmp (frame.bandCross.data(), frame.bandPower.data(), sizeof (frame.bandCross)) == 0;
            };

            const auto everyBandCarriesPower = [] (const FrameSnapshot& frame)
            {
                for (auto power : frame.bandPower)
                    if (! (power > 0.0f))
                        return false;

                return true;
            };

            // 1. 单声道模块。
            {
                BalanceModule mono;
                mono.prepare ({ sampleRate, (juce::uint32) maxBlock, 1 });
                const auto run = runCollectingFrames (mono, source, 0, 7402);
                int different = 0, silent = 0;

                for (const auto& frame : run.frames)
                {
                    different += crossEqualsPower (frame) ? 0 : 1;
                    silent += everyBandCarriesPower (frame) ? 0 : 1;
                }

                expect ((int) run.frames.size() >= 80, "mono frames are published");
                expectEquals (silent, 0, "positive control: every band carries power (mono)");
                expectEquals (different, 0, "mono: bandCross == bandPower bit-exactly");
            }

            // 2. 立体声模块，中途改成只送单声道。
            {
                auto inverted = source[0];

                for (auto& sample : inverted)
                    sample = -sample;

                BalanceModule balance;
                balance.prepare ({ sampleRate, (juce::uint32) maxBlock, 2 });
                const auto switchSample = 48 * maxBlock;   // 24576：hop（512）的整数倍，约 0.51 s
                juce::AudioBuffer<float> block (2, maxBlock);
                std::array<FrameSnapshot, 8> chunk {};
                std::vector<FrameSnapshot> frames;
                std::int64_t cursor = 0;

                for (int start = 0; start + maxBlock <= totalSamples; start += maxBlock)
                {
                    const auto stereo = start < switchSample;
                    block.setSize (stereo ? 2 : 1, maxBlock, false, false, true);
                    block.copyFrom (0, 0, source[0].data() + start, maxBlock);

                    if (stereo)
                        block.copyFrom (1, 0, inverted.data() + start, maxBlock);

                    balance.process (block);

                    for (int got = (int) chunk.size(); got == (int) chunk.size();)
                    {
                        got = std::abs (balance.readFramesSince (cursor, chunk.data(), (int) chunk.size()));
                        frames.insert (frames.end(), chunk.begin(), chunk.begin() + got);
                    }
                }

                int stereoFrames = 0, monoFrames = 0, stereoErrors = 0, monoErrors = 0;

                for (const auto& frame : frames)
                {
                    if (frame.endSample <= (std::int64_t) switchSample)
                    {
                        ++stereoFrames;

                        for (size_t band = 0; band < (size_t) numBands; ++band)
                        {
                            const auto power = (double) frame.bandPower[band];
                            stereoErrors += (power > 0.0 && std::abs ((double) frame.bandCross[band] + power) <= 1.0e-6 * power) ? 0 : 1;
                        }
                    }
                    else
                    {
                        ++monoFrames;
                        monoErrors += (crossEqualsPower (frame) && everyBandCarriesPower (frame)) ? 0 : 1;
                    }
                }

                expect (stereoFrames >= 40 && monoFrames >= 40, "frames on both sides of the switch");
                expectEquals (stereoErrors, 0, "before the switch: anti-phase stereo reads X = -P");
                expectEquals (monoErrors, 0, "after the switch: mono frames read X == P bit-exactly, never the stale second channel");
            }

            // 3. 三声道。
            {
                const auto other = makePinkNoise (1, totalSamples, -20.0f, 7403);
                const Signal same { source[0], source[0], source[0] };
                const Signal mixed { source[0], source[0], other[0] };
                BalanceModule sameModule, mixedModule;
                sameModule.prepare ({ sampleRate, (juce::uint32) maxBlock, 3 });
                mixedModule.prepare ({ sampleRate, (juce::uint32) maxBlock, 3 });
                const auto sameRun = runCollectingFrames (sameModule, same, maxBlock);
                const auto mixedRun = runCollectingFrames (mixedModule, mixed, maxBlock);
                int sameErrors = 0, unsafe = 0;

                for (const auto& frame : sameRun.frames)
                    for (size_t band = 0; band < (size_t) numBands; ++band)
                    {
                        const auto power = (double) frame.bandPower[band];
                        sameErrors += (power > 0.0 && std::abs ((double) frame.bandCross[band] - power) <= 1.0e-6 * power) ? 0 : 1;
                    }

                for (const auto& frame : mixedRun.frames)
                    for (size_t band = 0; band < (size_t) numBands; ++band)
                        unsafe += (std::isfinite (frame.bandCross[band])
                                   && std::abs (frame.bandCross[band]) <= frame.bandPower[band]) ? 0 : 1;

                expect ((int) sameRun.frames.size() >= 80 && mixedRun.frames.size() == sameRun.frames.size(),
                        "three-channel frames are published");
                expectEquals (sameErrors, 0, "three identical channels: X = P");
                expectEquals (unsafe, 0, "three channels: bandCross finite and within +-bandPower");
            }
        }

        // 拆片 / 参数 / 旁通 / 块长 / 分配。依据：BalanceModule.h「FrameSnapshot」注释——拆片与一口气做完发布的内容逐位相同、
        // 旁通时照常发布；交叉功率只读输入的 FFT，与 Recover / Tame / Brighten / 自动份额 / 旁通都无关，也与宿主块长无关（分析落在
        // 从 reset 起算的固定位置上）。bandCross 在片 2（两声道都变换完之后）算，两种做法走同一段代码、同样的输入，逐位相同。
        // 四个采样率、1 s，每次运行换一个种子的随机块长（1 ～ 1500：跨 hop、跨分析片、超过 prepare 的 512）。素材 makeRichSignal：
        // 左右不相关的粉噪 + 两声道相同的 60 / 1000 / 9000 Hz 正弦。阳性对照（交叉功率不是平凡的 0 或 ±P）：1 kHz 段 ρ > 0.3
        // （正弦 −29 dBFS RMS 比该段粉噪高约 4 dB，ρ 约 0.7）；4 kHz 段只有不相关粉噪（约 40 个 bin、约 85 帧，
        // σ(ρ) ≈ 0.99 / √40 · √(2.06 / 85) ≈ 0.024），|ρ| < 0.3。
        // 另断言：整个过程 process / readFramesSince 0 分配（音频线程上的路径），每块都读时从不丢帧。
        void testSlicingSettingsAndBypass()
        {
            beginTest ("bandCross: sliced = one-shot bit-exactly, independent of settings and block sizes, published while bypassed, no allocation");

            auto full = makeParams (100.0f, 100.0f, 30.0f, Speed::fast);
            full.automaticRecover = 40.0f;
            full.automaticTame = 60.0f;
            auto bypassed = full;
            bypassed.bypass = true;

            std::uint64_t allocations = 0;
            int droppedReads = 0;

            for (auto sampleRate : { 44100.0, 48000.0, 96000.0, 192000.0 })
            {
                const auto input = makeRichSignal (sampleRate, 1.0, 7601);
                const auto tag = "@" + juce::String (sampleRate);

                const auto collect = [&] (const Params& p, bool slicing, juce::int64 seed)
                {
                    BalanceModule balance;
                    prepareBalance (balance, sampleRate, p);
                    balance.setAnalysisSlicingForTesting (slicing);
                    const auto run = runCollectingFrames (balance, input, 0, seed);
                    allocations += run.allocations;
                    droppedReads += run.droppedReads;
                    return run.frames;
                };

                const auto reference     = collect (Params {}, true, 1);
                const auto oneShot       = collect (Params {}, false, 2);
                const auto withSettings  = collect (full, true, 3);
                const auto whileBypassed = collect (bypassed, true, 4);

                expect ((int) reference.size() >= 80, "frames are published " + tag);
                expect (oneShot.size() == reference.size() || oneShot.size() == reference.size() + 1,
                        "one-shot analysis publishes the same frames (at most one more at the very end) " + tag);
                expectEquals (countDifferentSnapshots (reference, oneShot), 0, "sliced and one-shot frames are bit-exact " + tag);
                expect (withSettings.size() == reference.size() && whileBypassed.size() == reference.size(),
                        "same number of frames with any settings " + tag);
                expectEquals (countDifferentSnapshots (reference, withSettings), 0,
                              "frames do not depend on recover / tame / brighten / automatic shares " + tag);
                expectEquals (countDifferentSnapshots (reference, whileBypassed), 0, "frames are still published while bypassed " + tag);

                const auto toneBand = correlationOf (reference, bandOf1kHz, bandOf1kHz);
                const auto noiseBand = correlationOf (reference, bandOf1kHz + 6, bandOf1kHz + 6);
                logMessage ("  " + tag + ": rho 1 kHz band " + juce::String (toneBand, 3) + ", 4 kHz band " + juce::String (noiseBand, 3));
                expect (toneBand > 0.3 && std::abs (noiseBand) < 0.3, "positive control: the material has non-trivial cross power " + tag);
            }

            expectEquals ((juce::int64) allocations, (juce::int64) 0, "process / readFramesSince never allocate");
            expectEquals (droppedReads, 0, "no frame is dropped when reading after every block");
        }

        // 「G 与输出逐位不变」（设计规格-Clay-分析决策与验收 1.1、验收 6.1）。本机不许编译运行、测试服务器宕机，没法当场跑改动前的
        // 代码对照，所以两件事都做（论证与对照步骤见 docs/validation/2026-09-26-clay/impl/A3.md）：
        //  1. 摘要：对固定输入，把每个输出样本、每块读到的 27 段 G、每帧改动前就有的快照字段（endSample / bandPowerDb / bandPower）
        //     的位模式做 FNV-1a 64 位摘要，写进日志（grep「digest」）。算摘要的代码只用改动前就有的接口：把 BalanceModule.cpp 换回
        //     v0.1.6.2 基线（改动前的原样；新头文件里多出的私有声明没人调用，照样能链接），同一台机器、同一套编译选项再跑一次，
        //     两边日志里的摘要应逐字相同（基线构建里 bandCross 恒为 0，本类其余断言会红，只看摘要行）。
        //  2. 当场可断言的替代证据：R 整体取负（float 取负精确）。bandCross 随之整个变号（改动新增的量翻了过来），而 G 只看各声道
        //     功率（FFT 线性、舍入对称，|−X|² 与 |X|² 逐值相同）——G 与 bandPower 必须逐位不变，输出左声道逐位不变、右声道逐值取负
        //     （滤波对输入取负严格反对称，只有 ±0 的符号可能不同，所以右声道按值比）。交叉功率若漏进了 G 的任何计算，这里会红。
        //     另断言拆片与一口气做完的三种摘要相同（片 2 多了一步，两种做法都走它），三次运行 0 分配。
        // 素材：makeRichSignal + 500 / 1000 / 1500 Hz 谐波组（让自动 Tame 的谐波保护去读各声道的 FFT 缓冲），Recover / Tame 满、
        // 自动份额 40 / 60、Brighten 30、Fast：分析、跟踪、判断、谐波保护、滤波都在工作。四个采样率、2.5 s、块 512：48 / 44.1 kHz
        // 块长 = hop，读数正落在 hop 边界上；96 kHz 落在 hop 的 0 / ½ 处，192 kHz 落在 0 / ¼ / ½ / ¾ 处——都不在拆片的发布窗
        // （hop 后 1 ～ 1/8 hop 个样本：48 k 下 64、192 k 下 256）里，所以两种做法每块读到的 G 可以逐块比（isInsidePublishWindow 的
        // 同一依据）；2.5 s 在四个采样率下都不停在某个 hop 后 1/8 hop 以内，两种做法发布的帧数也相同。阳性对照：处理真的改了信号、
        // G 最大超过 1 dB、bandCross 真的翻过来（|X_A − X_B| / P 最大 > 1：谐波组那几段两声道相同，ρ 接近 1）。
        void testGainAndOutputUnchanged()
        {
            beginTest ("G and output unchanged: logged digests; flipping R reverses bandCross but leaves G bit-exact");

            auto params = makeParams (100.0f, 100.0f, 30.0f, Speed::fast);
            params.automaticRecover = 40.0f;
            params.automaticTame = 60.0f;

            for (auto sampleRate : { 44100.0, 48000.0, 96000.0, 192000.0 })
            {
                auto input = makeRichSignal (sampleRate, 2.5, 7501);
                addToneEvent (input, sampleRate, 500.0, 0.2f, 0.3, 2.0);
                addToneEvent (input, sampleRate, 1000.0, 0.13f, 0.3, 2.0);
                addToneEvent (input, sampleRate, 1500.0, 0.10f, 0.3, 2.0);

                auto flipped = input;

                for (auto& sample : flipped[1])
                    sample = -sample;

                BalanceModule plain, inverted, oneShot;
                prepareBalance (plain, sampleRate, params);
                prepareBalance (inverted, sampleRate, params);
                prepareBalance (oneShot, sampleRate, params);
                oneShot.setAnalysisSlicingForTesting (false);

                const auto a = runCollectingFrames (plain, input, maxBlock);
                const auto b = runCollectingFrames (inverted, flipped, maxBlock);
                const auto c = runCollectingFrames (oneShot, input, maxBlock);

                const auto tag = "@" + juce::String (sampleRate);
                const auto outputDigest = digestOf (a.output);
                const auto gainDigest = digestOf (a.gains);
                const auto frameDigest = digestOf (a.frames);
                logMessage ("  digest " + tag + " Hz: output 0x" + juce::String::toHexString (outputDigest)
                            + ", G 0x" + juce::String::toHexString (gainDigest)
                            + ", frames 0x" + juce::String::toHexString (frameDigest)
                            + " (" + juce::String ((int) a.gains.size()) + " G readings, "
                            + juce::String ((int) a.frames.size()) + " frames)");

                // 阳性对照：处理真的改了信号、G 真的动过；三次运行都不分配。
                float largestGain = 0.0f;

                for (const auto& gains : a.gains)
                    for (auto gain : gains)
                        largestGain = juce::jmax (largestGain, std::abs (gain));

                expect (maxDiffFrom (input, a.output, 0) > 1.0e-3f, "processing must change the signal " + tag);
                expect (largestGain > 1.0f, "G must move by more than 1 dB " + tag);
                expectEquals ((juce::int64) (a.allocations + b.allocations + c.allocations), (juce::int64) 0,
                              "no allocation on the audio path " + tag);

                // 1. 拆片与一口气做完：三种摘要相同。
                expect (digestOf (c.output) == outputDigest, "one-shot analysis: same output digest " + tag);
                expect (digestOf (c.gains) == gainDigest, "one-shot analysis: same G digest " + tag);
                expect (digestOf (c.frames) == frameDigest, "one-shot analysis: same frame digest " + tag);

                // 2. R 取负：G、bandPower、左声道逐位不变，右声道逐值取负，bandCross 变号。
                int gainMismatches = 0;

                for (size_t k = 0; k < a.gains.size() && k < b.gains.size(); ++k)
                    gainMismatches += sameGains (a.gains[k], b.gains[k]) ? 0 : 1;

                int leftMismatches = 0, rightMismatches = 0;

                for (size_t i = 0; i < a.output[0].size(); ++i)
                {
                    leftMismatches += std::memcmp (&a.output[0][i], &b.output[0][i], sizeof (float)) == 0 ? 0 : 1;
                    rightMismatches += (b.output[1][i] == -a.output[1][i]) ? 0 : 1;
                }

                int powerMismatches = 0, crossErrors = 0;
                double largestSwing = 0.0;

                for (size_t k = 0; k < a.frames.size() && k < b.frames.size(); ++k)
                {
                    const auto& x = a.frames[k];
                    const auto& y = b.frames[k];
                    powerMismatches += std::memcmp (x.bandPower.data(), y.bandPower.data(), sizeof (x.bandPower)) == 0 ? 0 : 1;

                    for (size_t band = 0; band < (size_t) numBands; ++band)
                    {
                        const auto power = (double) x.bandPower[band];

                        if (! (power > 0.0))
                            continue;

                        const auto crossA = (double) x.bandCross[band];
                        const auto crossB = (double) y.bandCross[band];
                        crossErrors += (std::abs (crossA + crossB) <= 1.0e-6 * power) ? 0 : 1;
                        largestSwing = juce::jmax (largestSwing, std::abs (crossA - crossB) / power);
                    }
                }

                expect (a.gains.size() == b.gains.size() && a.frames.size() == b.frames.size(), "same readings and frames " + tag);
                expectEquals (gainMismatches, 0, "R polarity flip: G bit-exact " + tag);
                expectEquals (leftMismatches, 0, "R polarity flip: left output bit-exact " + tag);
                expectEquals (rightMismatches, 0, "R polarity flip: right output exactly negated " + tag);
                expectEquals (powerMismatches, 0, "R polarity flip: bandPower bit-exact " + tag);
                expectEquals (crossErrors, 0, "R polarity flip: bandCross reverses sign " + tag);
                expect (largestSwing > 1.0, "positive control: bandCross really flips (largest |X_A - X_B| / P "
                                                + juce::String (largestSwing, 3) + ") " + tag);
            }
        }
    };

    static BalanceCrossPowerTests balanceCrossPowerTests;
}

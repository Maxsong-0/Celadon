/*
  ==============================================================================

    GlazeTests.cpp
    v0.6 GLAZE 整链测试（测试类 GlazeChain；v0.4 的 GlazeMacro 偏移表测试随 GlazeMacro 退役改写，ADR-008）：
      1. GLAZE 一直为 0：整链与「手工串起来的 Balance → Clay → Color → Limit」（编排器旁路的参照链；v0.1.7.0 起含 Clay 与它的
         固定延迟 D + G）逐位相同——
         3 采样率 × 单 / 双声道 × 5 种块长 × 3 种参数（含每块自动化、宿主旁通、reset / 重启淡入、非有限输入、跳播 / 回绕 / 离线、
         重新聆听请求、风格来回换），输出、延迟、GR 表、Balance 曲线、两块表的读数都比；
      2. GLAZE > 0：编排器只经「交给模块的实际参数」起作用（参照链喂同样的实际参数，输出逐位相同），链里没有第二条音频路径；
      3. 延迟不随 GLAZE 变；
      4. 拧 GLAZE 无咔哒：块率边带 ≤ −95 dBFS（v0.4 评审定的口径）+ 逐块一阶差分包络，负对照证明指标抓得到；
      5. 整条生命周期（聆听、过渡、校准、保护层、约束反馈）里输出真峰值 ≤ Ceiling + 0.01 dB；
      6. 学习后响度随 GLAZE 单调上升；
      7. 四种风格的方向（温润 Glue 最多、冲击 Clip 最多、Heat 上限温润 > 冲击 > 通透、自动 Heat 只给风格底量 × 强度、自动权重和为 1；
         T-032 R6-01 起「温润 Heat 最多、通透最少」不再靠自动 Heat 顶到上限成立）；
      8. T-032 R6-02（A7 分层，A11 收尾）：手动单独超某个模块的预算（受手动设置限制：Heat / Glue / Clip）时自动 Gain 照常推响，画像失配、
         保护层、校准、约束反馈照常工作，但保护层与上游监视只核对自动部分（每项有效上限 = max(风格上限, 手动单独用量)：手动单独超预算
         的那一项不设约束）——不把手动造成的超出当成自动的超限、不磨光自动增益；「手动超预算」的项变了清空窗口，拧回预算以内不因旧窗口
         误动作；电平类自动全部撤回（手动 Gain 已超限幅预算）时偏差、保护、校准、画像失配都不动作。

    GLAZE 0 的逐位对照为什么用参照链而不是把 v0.5 的输出摘要写成常数：摘要是 Mac（ARM、NEON 路径）上算的，Linux 服务器（x86、
    SSE2 / 标量路径）上同一份代码的浮点结果本来就可能不同，常数会让 CI 误报；参照链在每个平台上都验证「GLAZE 0 = 只有三个模块、
    参数就是手动值」这件事本身。「三个模块本身与 v0.5 逐位相同」由改动前后同一程序的摘要对照证明（A4 报告：整链 90 个、
    处理器 36 个场景全部相同），模块代码这一轮没有改动。

  ==============================================================================
*/

#include "TestHelpers.h"

#include "../Source/dsp/CeladonChain.h"
#include "../Source/dsp/GlazePlanner.h"
#include "../Source/dsp/GlazeProfile.h"
#include "../Source/dsp/LoudnessMeter.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace celadon::test
{
    namespace
    {
        constexpr int maxBlock = 512;

        using Params = CeladonChain::Params;

        juce::dsp::ProcessSpec makeSpec (double sampleRate, int numChannels, int blockSize = maxBlock)
        {
            return { sampleRate, (juce::uint32) blockSize, (juce::uint32) numChannels };
        }

        bool sameBits (float a, float b) noexcept
        {
            return std::memcmp (&a, &b, sizeof (float)) == 0;
        }

        bool sameParams (const Params& a, const Params& b) noexcept
        {
            return sameBits (a.balance.recover, b.balance.recover) && sameBits (a.balance.tame, b.balance.tame)
                && sameBits (a.balance.automaticRecover, b.balance.automaticRecover)
                && sameBits (a.balance.automaticTame, b.balance.automaticTame)
                && sameBits (a.balance.brighten, b.balance.brighten) && a.balance.speed == b.balance.speed
                && a.balance.bypass == b.balance.bypass
                && sameBits (a.color.toneDb, b.color.toneDb) && sameBits (a.color.glue, b.color.glue)
                && sameBits (a.color.heat, b.color.heat) && sameBits (a.color.mixPercent, b.color.mixPercent)
                && sameBits (a.color.outputDb, b.color.outputDb) && a.color.bypass == b.color.bypass
                && sameBits (a.limit.gainDb, b.limit.gainDb) && sameBits (a.limit.ceilingDb, b.limit.ceilingDb)
                && sameBits (a.limit.releaseMs, b.limit.releaseMs) && a.limit.autoRelease == b.limit.autoRelease
                && sameBits (a.limit.lookaheadMs, b.limit.lookaheadMs) && sameBits (a.limit.clip, b.limit.clip)
                && a.limit.listen == b.limit.listen && a.limit.bypass == b.limit.bypass
                && sameBits (a.glazePercent, b.glazePercent) && a.glazeStyle == b.glazeStyle;
        }

        // 可复现的「类音乐」合成信号，120 BPM：底鼓（55 Hz 指数衰减 + 敲击噪声）、拨弦贝斯（每拍换音、含二次谐波）、
        // 和声垫（4 个音、0.3 Hz 颤音、左右相位错开）、军鼓（2 / 4 拍，3 kHz 低通噪声）、踩镲（八分音符，6 kHz 高通噪声）。
        // 两声道噪声不相关；单声道取第一声道。整体归一到峰值 peak。
        Signal makeMusicLikeSignal (int numChannels, double sampleRate, double seconds, juce::int64 seed, float peak = 0.5f)
        {
            constexpr double beatSeconds = 0.5;
            constexpr double bassNotesHz[] = { 55.0, 73.42, 82.41, 61.74 };
            constexpr double padNotesHz[]  = { 220.0, 277.18, 329.63, 440.0 };

            const auto numSamples = (int) (seconds * sampleRate);
            auto signal = makeSilence (numChannels, numSamples);
            const auto twoPi = juce::MathConstants<double>::twoPi;
            const auto snareLowpassCoeff = std::exp (-twoPi * 3000.0 / sampleRate);
            const auto hatLowpassCoeff   = std::exp (-twoPi * 6000.0 / sampleRate);

            for (int ch = 0; ch < numChannels; ++ch)
            {
                juce::Random random (seed + (juce::int64) ch * 7919);
                auto& channel = signal[(size_t) ch];
                double snareLowpass = 0.0, hatLowpass = 0.0;

                for (int i = 0; i < numSamples; ++i)
                {
                    const auto t = (double) i / sampleRate;
                    const auto beatIndex = (int) (t / beatSeconds);
                    const auto beatPos = t - beatIndex * beatSeconds;
                    const auto eighthPos = std::fmod (t, beatSeconds * 0.5);
                    const auto white = (double) random.nextFloat() * 2.0 - 1.0;

                    snareLowpass = snareLowpassCoeff * snareLowpass + (1.0 - snareLowpassCoeff) * white;
                    hatLowpass   = hatLowpassCoeff * hatLowpass + (1.0 - hatLowpassCoeff) * white;

                    const auto kick  = 0.55 * std::sin (twoPi * 55.0 * beatPos) * std::exp (-beatPos / 0.09)
                                     + 0.15 * white * std::exp (-beatPos / 0.003);

                    const auto bassHz = bassNotesHz[(size_t) (beatIndex % 4)];
                    const auto bassEnvelope = juce::jmin (1.0, beatPos / 0.01) * std::exp (-beatPos / 0.6);
                    const auto bass  = bassEnvelope * (0.28 * std::sin (twoPi * bassHz * beatPos)
                                                       + 0.10 * std::sin (twoPi * 2.0 * bassHz * beatPos));

                    double pad = 0.0;
                    for (auto padHz : padNotesHz)
                        pad += 0.05 * std::sin (twoPi * padHz * t + 0.7 * ch);
                    pad *= 1.0 + 0.15 * std::sin (twoPi * 0.3 * t);

                    const auto snare = (beatIndex % 2 == 1) ? 0.6 * snareLowpass * std::exp (-beatPos / 0.12) : 0.0;
                    const auto hat   = 0.08 * (white - hatLowpass) * std::exp (-eighthPos / 0.03);

                    channel[(size_t) i] = (float) (kick + bass + pad + snare + hat);
                }
            }

            float maxAbs = 0.0f;
            for (const auto& channel : signal)
                for (auto sample : channel)
                    maxAbs = juce::jmax (maxAbs, std::abs (sample));

            scale (signal, peak / maxAbs);
            return signal;
        }

        Signal makeTones (int numChannels, double sampleRate, int numSamples)
        {
            auto tones = makeSilence (numChannels, numSamples);

            for (int i = 0; i < numSamples; ++i)
            {
                double value = 0.0;
                for (auto hz : { 220.0, 330.0, 440.0 })
                    value += 0.25 * std::sin (juce::MathConstants<double>::twoPi * hz * i / sampleRate);

                for (auto& channel : tones)
                    channel[(size_t) i] = (float) value;
            }

            return tones;
        }

        // 用插件自己的 TruePeakDetector 量真峰值，只算「第 skip 个输入样本之后」的部分；末尾冲出尾巴。
        float truePeakAfter (const Signal& signal, int skip)
        {
            TruePeakDetector detector;
            detector.prepare ((int) signal.size());
            float peak = 0.0f;

            for (size_t ch = 0; ch < signal.size(); ++ch)
            {
                const auto n = (int) signal[ch].size();

                for (int i = 0; i < n + TruePeakDetector::tapsPerPhase; ++i)
                {
                    const auto value = detector.processSample ((int) ch, i < n ? signal[ch][(size_t) i] : 0.0f);

                    if (i >= skip + TruePeakDetector::tapsPerPhase)
                        peak = juce::jmax (peak, value);
                }
            }

            return peak;
        }

        // 从 from 起截一段；头 64 个样本加升余弦淡入（只给 16× 参考插值用：它把范围外当作 0，硬切会带来假的过冲）。
        Signal sliceWithFadedHead (const Signal& signal, int from)
        {
            constexpr int fadeLength = 64;
            Signal slice;

            for (const auto& channel : signal)
            {
                slice.emplace_back (channel.begin() + from, channel.end());

                for (int i = 0; i < fadeLength && i < (int) slice.back().size(); ++i)
                    slice.back()[(size_t) i] *= 0.5f - 0.5f * std::cos (juce::MathConstants<float>::pi * (float) i / (float) fadeLength);
            }

            return slice;
        }

        float integratedLoudnessOf (const Signal& signal, double sampleRate, int from = 0)
        {
            const auto numChannels = (int) signal.size();
            LoudnessMeter meter;
            meter.prepare (makeSpec (sampleRate, numChannels));

            const auto numSamples = (int) signal.front().size();
            juce::AudioBuffer<float> block (numChannels, maxBlock);

            for (int start = from; start < numSamples; start += maxBlock)
            {
                const auto length = juce::jmin (maxBlock, numSamples - start);
                block.setSize (numChannels, length, false, false, true);

                for (int ch = 0; ch < numChannels; ++ch)
                    block.copyFrom (ch, 0, signal[(size_t) ch].data() + start, length);

                meter.process (block);
            }

            return meter.getIntegratedLufs();
        }

        //==============================================================================
        // 参照链：手工串起来的 Balance → Clay → Color → Limit（v0.1.7.0 起链里有 Clay：参照链同样要喂它实际参数，否则 GLAZE > 0、
        // Clay 自动介入时两边自然不同），照 CeladonChain 的约定处理宿主旁通（强制四个模块旁通）、reset、
        // 重启淡入（前三级 Balance + Clay + Color 的延迟算进静音段）与输出非有限值（清零 + 重启淡入）。编排器旁路：参数由调用方直接给。
        // 阶段 4（ADR-012）起 Clay 有固定延迟，ADR-014 起为 D + G：参照链里的 ClayModule 同样带着它，延迟、输出、表读数的逐位对照照旧成立；
        // 本文件其余用例都按链自己上报的 getLatencySamples() 对齐，不写死 282。
        struct ReferenceChain
        {
            void prepare (const juce::dsp::ProcessSpec& spec, const Params& p)
            {
                setParams (p, false);
                balance.prepare (spec);
                clay.prepare (spec);
                color.prepare (spec);
                limiter.prepare (spec);
            }

            void setParams (const Params& p, bool hostBypassed)
            {
                auto b = p.balance;
                auto k = p.clay;
                auto c = p.color;
                auto l = p.limit;
                b.bypass = b.bypass || hostBypassed;
                k.bypass = k.bypass || hostBypassed;
                c.bypass = c.bypass || hostBypassed;
                l.bypass = l.bypass || hostBypassed;
                balance.setParams (b);
                clay.setParams (k);
                color.setParams (c);
                limiter.setParams (l);
            }

            void reset()
            {
                balance.reset();
                clay.reset();
                color.reset();
                limiter.reset();
            }

            void restartWithFadeIn()
            {
                balance.reset();
                clay.reset();
                color.reset();
                limiter.restartWithFadeIn (balance.getLatencySamples() + clay.getLatencySamples() + color.getLatencySamples());
            }

            bool process (juce::AudioBuffer<float>& io)
            {
                balance.process (io);
                clay.process (io);
                color.process (io);
                limiter.process (io);

                bool nonFinite = false;
                for (int ch = 0; ch < io.getNumChannels(); ++ch)
                    for (int i = 0; i < io.getNumSamples(); ++i)
                        nonFinite = nonFinite || ! std::isfinite (io.getSample (ch, i));

                if (nonFinite)
                {
                    io.clear();
                    restartWithFadeIn();
                }

                return nonFinite;
            }

            int getLatencySamples() const noexcept
            {
                return balance.getLatencySamples() + clay.getLatencySamples() + color.getLatencySamples() + limiter.getLatencySamples();
            }

            BalanceModule balance;
            ClayModule clay;
            ColorModule color;
            LimiterModule limiter;
        };

        // 每块的参数：0 = 中性；1 = 各模块都开着的静态设置；2 = 每块都在动（照 ChainTests 的零分配用例）。GLAZE 由调用方定，风格来回换。
        Params paramsAt (int mode, int b, int numBlocks, float glazePercent)
        {
            Params p;
            p.limit.ceilingDb = -1.0f;

            if (mode == 1)
            {
                p.balance.recover = 40.0f; p.balance.tame = 30.0f; p.balance.brighten = 20.0f;
                p.balance.speed = BalanceModule::Speed::fast;
                p.color.toneDb = 1.0f; p.color.glue = 40.0f; p.color.heat = 35.0f; p.color.mixPercent = 90.0f; p.color.outputDb = 1.0f;
                p.limit.gainDb = 8.0f; p.limit.releaseMs = 150.0f; p.limit.autoRelease = false;
                p.limit.lookaheadMs = 3.0f; p.limit.clip = 30.0f;
                p.glazeStyle = 2;
            }
            else if (mode == 2)
            {
                const auto phase = (float) b / (float) juce::jmax (1, numBlocks);
                const auto wave = 0.5f + 0.5f * std::sin (juce::MathConstants<float>::twoPi * 3.0f * phase);

                p.balance.recover  = 100.0f * wave;
                p.balance.tame     = 100.0f * (1.0f - wave);
                p.balance.brighten = -100.0f + 200.0f * phase;
                p.balance.speed    = static_cast<BalanceModule::Speed> ((b / 50) % 3);
                p.balance.bypass   = (b / 70) % 2 == 1;
                p.color.toneDb     = -3.0f + 6.0f * wave;
                p.color.glue       = (b / 30) % 2 == 0 ? 0.0f : 100.0f * wave;
                p.color.heat       = (b / 45) % 2 == 0 ? 0.0f : 100.0f * wave;
                p.color.mixPercent = 100.0f * wave;
                p.color.outputDb   = -6.0f + 12.0f * phase;
                p.color.bypass     = (b / 80) % 2 == 1;
                p.limit.gainDb      = 24.0f * wave;
                p.limit.ceilingDb   = -3.0f * phase;
                p.limit.releaseMs   = 10.0f + 990.0f * phase;
                p.limit.autoRelease = (b / 25) % 2 == 0;
                p.limit.lookaheadMs = 1.0f + 0.5f * (float) ((b / 40) % 19);
                p.limit.clip        = (b / 35) % 2 == 0 ? 0.0f : 100.0f * wave;
                // 非 Listen 路径仍逐位对照三个裸模块；Listen 已是整链实测监听，独立用例在 ChainTests 验证。
                p.limit.listen      = false;
                p.limit.bypass      = (b / 110) % 2 == 1;
                p.glazeStyle        = (b / 20) % 4;
            }

            p.glazePercent = glazePercent;
            return p;
        }

        // 学一份画像存下来（GLAZE 0、出厂常数、带播放位置、素材不循环），给后面「从同一份存档起步」的用例用。
        std::unique_ptr<juce::XmlElement> learnState (const Signal& material, double sampleRate, int style = 0)
        {
            const auto numChannels = (int) material.size();
            CeladonChain chain;
            Params p;
            p.limit.ceilingDb = -1.0f;
            p.glazeStyle = style;
            chain.setParams (p, false);
            chain.prepare (makeSpec (sampleRate, numChannels));

            glaze::HostInfo host;
            host.hasPlayState = host.isPlaying = host.hasPosition = true;

            juce::AudioBuffer<float> block (numChannels, maxBlock);
            const auto numSamples = (int) material.front().size();

            for (int start = 0; start + maxBlock <= numSamples; start += maxBlock)
            {
                for (int ch = 0; ch < numChannels; ++ch)
                    block.copyFrom (ch, 0, material[(size_t) ch].data() + start, maxBlock);

                host.positionSamples = start;
                chain.setHostInfo (host);
                chain.setParams (p, false);
                chain.process (block);

                if (chain.getGlazeStatus().profileReady)
                    break;
            }

            return chain.createGlazeStateXml();
        }

        // 从存档起步的链，离线渲染（只用已提交方案：不开校准、不做约束反馈——纯前馈，结果只由方案决定）。
        struct RestoredChain
        {
            RestoredChain (const juce::XmlElement& state, double sampleRate, int numChannels, const Params& p)
                : params (p)
            {
                chain.restoreGlazeStateXml (&state);
                chain.setParams (params, false);
                chain.prepare (makeSpec (sampleRate, numChannels));
                host.offline = true;
                host.hasPlayState = host.isPlaying = true;
            }

            template <typename BeforeBlock>
            Signal render (const Signal& input, int blockSize, BeforeBlock&& beforeBlock)
            {
                const auto numChannels = (int) input.size();
                const auto numSamples = (int) input.front().size();
                auto output = input;
                juce::AudioBuffer<float> block (numChannels, blockSize);

                for (int start = 0, b = 0; start < numSamples; start += blockSize, ++b)
                {
                    const auto length = juce::jmin (blockSize, numSamples - start);
                    block.setSize (numChannels, length, false, false, true);

                    for (int ch = 0; ch < numChannels; ++ch)
                        block.copyFrom (ch, 0, input[(size_t) ch].data() + start, length);

                    beforeBlock (b);
                    chain.setHostInfo (host);
                    chain.setParams (params, false);
                    chain.process (block);

                    for (int ch = 0; ch < numChannels; ++ch)
                        std::copy (block.getReadPointer (ch), block.getReadPointer (ch) + length, output[(size_t) ch].begin() + start);
                }

                return output;
            }

            Signal render (const Signal& input, int blockSize)   { return render (input, blockSize, [] (int) {}); }

            CeladonChain chain;
            Params params;
            glaze::HostInfo host;
        };

        // 实时链（T-032 R6-02 用）：聆听、方案过渡、校准、保护层、约束反馈、画像失配都照常跑，时长常数缩短（与 GlazeControllerTests
        // 的 fastConstants 同一组：聆听 2 ～ 3 s、过渡 0.5 s、校准稳定 0.5 s + 窗 2 s；其余出厂值，含画像失配与保护层）。
        // 素材循环播放、带播放位置；sourceGain 调整体电平（画像失配用）。mismatch = false 关掉画像失配（门槛与回差设成极大，与
        // GlazeControllerTests 的 clayConstants 同一做法）：让「比画像响」的一段只交给保护层与约束反馈（R6-02 收尾 A11 的保护层用例）。
        struct LiveRig
        {
            LiveRig (Signal material, const Params& initial, double rate = 48000.0, bool mismatch = true)
                : params (initial), source (std::move (material)), sampleRate (rate)
            {
                auto c = glaze::factoryConstants();
                c.minListenSeconds = 2.0;
                c.maxListenSeconds = 3.0;
                c.planTransitionSeconds = 0.5;
                c.calibrationSettleSeconds = 0.5;
                c.calibrationWindowSeconds = 2.0;

                if (! mismatch)
                    c.mismatchThresholdLu = c.mismatchReleaseLu = 1.0e9f;

                chain.getGlazeController().setConstantsForTesting (c);

                host.hasPlayState = host.isPlaying = host.hasPosition = true;
                buffer.setSize ((int) source.size(), maxBlock);
                chain.setParams (params, false);
                chain.prepare (makeSpec (sampleRate, (int) source.size()));
            }

            void step()
            {
                const auto numChannels = (int) source.size();
                const auto length = source.front().size();

                for (int ch = 0; ch < numChannels; ++ch)
                    for (int i = 0; i < maxBlock; ++i)
                        buffer.setSample (ch, i, sourceGain * source[(size_t) ch][(sourceIndex + (size_t) i) % length]);

                sourceIndex = (sourceIndex + (size_t) maxBlock) % length;
                host.positionSamples = position;
                chain.setHostInfo (host);
                chain.setParams (params, false);
                chain.process (buffer);
                position += maxBlock;
            }

            template <typename PerBlock>
            void run (double seconds, PerBlock&& perBlock)
            {
                const auto blocks = juce::jmax (1, (int) std::ceil (seconds * sampleRate / maxBlock));

                for (int b = 0; b < blocks; ++b)
                {
                    step();
                    perBlock();
                }
            }

            void run (double seconds)   { run (seconds, [] {}); }

            glaze::GlazeController::TestProbe probe() const   { return chain.getGlazeController().getProbeForTesting(); }
            glaze::Status status() const                      { return chain.getGlazeStatus(); }

            CeladonChain chain;
            Params params;
            Signal source;
            double sampleRate;
            glaze::HostInfo host;
            float sourceGain = 1.0f;
            size_t sourceIndex = 0;
            std::int64_t position = 0;
            juce::AudioBuffer<float> buffer;
        };
    }

    class GlazeChainTests  : public juce::UnitTest
    {
    public:
        GlazeChainTests() : juce::UnitTest ("GlazeChain", "Celadon") {}

        void runTest() override
        {
            testGlazeZeroEqualsReferenceChain();
            testGlazeActsOnlyThroughEffectiveParams();
            testListenDoesNotChangeMastering();
            testLatencyUnchanged();
            testSweepHasNoBlockRateSidebands();
            testSweepHasNoClicks();
            testTruePeakNeverAboveCeiling();
            testLoudnessRisesWithGlaze();
            testStylesLeanOnTheirModules();
            testOneKnobFromFreshInsert();
            testNewSpectralProblemAfterLearning();

            // T-032 R6-02：手动单独超某个模块的预算 vs 电平类自动全部撤回
            testManualHeatOverBudget();
            testManualGlueOverBudget();
            testManualClipOverBudget();       // R6-02 收尾（A11）
            testAutomaticLevelWithdrawn();
        }

    private:
        //==============================================================================
        // 1. GLAZE 一直为 0：CeladonChain（两块表 + 编排器都在链里，照常学习、响应宿主事件）与参照链（只有三个模块、参数 = 手动值）
        //    在 3 采样率 × 单 / 双声道 × 块 512 / 64 / 333 / 随机 1 ～ 700 / 2048 × 中性 / 静态 / 每块自动化 下：
        //    交给模块的实际参数逐位 = 手动值；每块输出逐位相同；延迟、两条 GR 表、Balance 27 段修正量相同；OUT 表的真峰值 / S / I 与
        //    参照 OUT 表逐位相同；IN 表（v0.6 起开着响度计量）的真峰值读数与 v0.5 的配置（只算真峰值）逐位相同。
        //    自动化那一种另加：宿主旁通来回、reset、重启淡入、一块 +Inf 输入（非有限输出 → 两边都清零重启）、跳播 / 循环回绕 / 离线切换、
        //    「重新聆听」请求、风格每 20 块换一次——GLAZE 0 时这些都不许改变声音。
        // 依据：设计规格-GLAZE 验收总表第 1 条「从初始化起 GLAZE 一直为 0：整链输出与 v0.5 逐位相同」；GlazeController.h「GLAZE ≤ 0 时
        //    实际参数逐位等于手动值（不经任何额外运算）」；CeladonChain.h 的处理顺序。
        void testGlazeZeroEqualsReferenceChain()
        {
            beginTest ("GLAZE 0, Listen off: chain == hand-wired modules with the manual params, bit-exact, 90 scenarios");

            struct Pattern { const char* name; int fixed; int maxLength; };
            const Pattern patterns[] = { { "512", 512, 0 }, { "64", 64, 0 }, { "333", 333, 0 }, { "rand", 0, 700 }, { "2048", 2048, 0 } };

            int scenarios = 0, failures = 0;

            for (auto sampleRate : { 44100.0, 48000.0, 96000.0 })
             for (int numChannels = 1; numChannels <= 2; ++numChannels)
              for (const auto& pattern : patterns)
               for (int mode = 0; mode < 3; ++mode)
            {
                const auto tag = " @" + juce::String (sampleRate, 0) + " ch" + juce::String (numChannels) + " block " + pattern.name
                               + " mode " + juce::String (mode);
                const auto input = makeMusicLikeSignal (numChannels, sampleRate, 1.5, 1000 + mode, 0.7f);
                const auto numSamples = (int) input.front().size();

                std::vector<int> lengths;
                {
                    juce::Random random (4242 + mode);
                    for (int pos = 0; pos < numSamples;)
                    {
                        const auto len = juce::jmin (numSamples - pos, pattern.fixed > 0 ? pattern.fixed : 1 + random.nextInt (pattern.maxLength));
                        lengths.push_back (len);
                        pos += len;
                    }
                }

                const auto numBlocks = (int) lengths.size();
                const auto spec = makeSpec (sampleRate, numChannels);
                const auto first = paramsAt (mode, 0, numBlocks, 0.0f);

                CeladonChain chain;
                chain.setParams (first, false);
                chain.prepare (spec);

                ReferenceChain reference;
                reference.prepare (spec, first);

                LoudnessMeter referenceIn, referenceOut;
                referenceIn.setLoudnessEnabled (false);   // v0.5 处理器的 IN 表只算真峰值
                referenceIn.prepare (spec);
                referenceOut.prepare (spec);

                glaze::HostInfo host;
                host.hasPlayState = host.isPlaying = host.hasPosition = true;

                const auto largest = *std::max_element (lengths.begin(), lengths.end());
                juce::AudioBuffer<float> a (numChannels, largest), r (numChannels, largest);
                BalanceModule::BandGains gainsA {}, gainsR {};
                bool identical = true, manualEverywhere = true;
                juce::String firstMismatch;
                int pos = 0;
                std::int64_t position = 0;

                auto mismatch = [&] (bool ok, const juce::String& what, int block)
                {
                    if (! ok && identical)
                        firstMismatch = what + " at block " + juce::String (block);

                    identical = identical && ok;
                };

                for (int b = 0; b < numBlocks; ++b)
                {
                    const auto len = lengths[(size_t) b];
                    const auto p = paramsAt (mode, b, numBlocks, 0.0f);
                    const bool hostBypassed = mode == 2 && (b / 90) % 2 == 1;

                    a.setSize (numChannels, len, false, false, true);
                    r.setSize (numChannels, len, false, false, true);

                    for (int ch = 0; ch < numChannels; ++ch)
                    {
                        a.copyFrom (ch, 0, input[(size_t) ch].data() + pos, len);
                        r.copyFrom (ch, 0, input[(size_t) ch].data() + pos, len);
                    }

                    if (mode == 2)
                    {
                        if (b == numBlocks / 3)
                        {
                            for (int ch = 0; ch < numChannels; ++ch)
                            {
                                a.setSample (ch, len / 2, std::numeric_limits<float>::infinity());
                                r.setSample (ch, len / 2, std::numeric_limits<float>::infinity());
                            }
                        }

                        if (b == numBlocks / 4)  position += 96000;                    // 跳播
                        if (b == numBlocks / 2)  { host.hasLoop = host.looping = true; position -= 48000; }   // 回绕
                        host.offline = (b / 100) % 3 == 2;
                        if (b % 150 == 7) chain.requestGlazeRelearn();
                    }

                    host.positionSamples = position;
                    chain.setHostInfo (host);

                    if (mode == 2 && b == numBlocks / 2)
                    {
                        chain.setParams (p, hostBypassed);
                        chain.reset();
                        reference.setParams (p, hostBypassed);
                        reference.reset();
                    }

                    if (mode == 2 && b % 100 == 99)
                    {
                        chain.restartWithFadeIn();
                        reference.restartWithFadeIn();
                    }

                    chain.setParams (p, hostBypassed);
                    reference.setParams (p, hostBypassed);
                    manualEverywhere = manualEverywhere && sameParams (chain.getEffectiveParams(), p);

                    mismatch (chain.getLatencySamples() == reference.getLatencySamples(), "latency", b);

                    // 参照 IN 表吃的是链入口看到的同一块输入（非有限值那块：处理器会先清零，链与 v0.5 的链都不管输入）。
                    referenceIn.process (r);
                    const auto nonFiniteA = chain.process (a);
                    const auto nonFiniteR = reference.process (r);
                    referenceOut.process (r);

                    mismatch (nonFiniteA == nonFiniteR, "non-finite report", b);

                    for (int ch = 0; ch < numChannels; ++ch)
                        mismatch (std::memcmp (a.getReadPointer (ch), r.getReadPointer (ch), sizeof (float) * (size_t) len) == 0, "output", b);

                    mismatch (sameBits (chain.getAndResetLimitGainReductionDb(), reference.limiter.getAndResetGainReductionDb()), "limit GR", b);
                    mismatch (sameBits (chain.getAndResetGlueGainReductionDb(), reference.color.getAndResetGainReductionDb()), "glue GR", b);
                    mismatch (sameBits (chain.getAndResetOutputTruePeakDb(), referenceOut.getAndResetTruePeakDb()), "OUT true peak", b);
                    mismatch (sameBits (chain.getOutputShortTermLufs(), referenceOut.getShortTermLufs()), "OUT short-term", b);
                    mismatch (sameBits (chain.getOutputIntegratedLufs(), referenceOut.getIntegratedLufs()), "OUT integrated", b);
                    mismatch (sameBits (chain.getAndResetInputTruePeakDb(), referenceIn.getAndResetTruePeakDb()), "IN true peak", b);

                    if (b % 8 == 0)
                    {
                        chain.getBalanceBandGainsDb (gainsA);
                        reference.balance.getBandGainsDb (gainsR);
                        mismatch (std::memcmp (gainsA.data(), gainsR.data(), sizeof (float) * gainsA.size()) == 0, "balance gains", b);
                    }

                    pos += len;
                    position += len;
                }

                ++scenarios;

                if (! identical || ! manualEverywhere)
                {
                    ++failures;
                    expect (manualEverywhere, "effective params must equal the manual params at GLAZE 0" + tag);
                    expect (identical, "first mismatch: " + firstMismatch + tag);
                }
            }

            logMessage ("  " + juce::String (scenarios) + " scenarios, " + juce::String (failures) + " with any difference");
            expectEquals (scenarios, 90);
            expectEquals (failures, 0, "GLAZE 0 must be bit-exact with the hand-wired chain in every scenario");
        }

        // 2. GLAZE > 0：编排器只经「交给模块的实际参数」起作用——参照链每块喂 chain.getEffectiveParams()（同样的宿主旁通强制），
        //    整个生命周期（聆听、过渡、校准、保护层、约束反馈，素材中途推热）里输出逐位相同；先确认 GLAZE 真的改变了输出。
        //    48 kHz 立体声与 44.1 kHz 单声道各一遍。
        // 依据：GlazeController.h 的链调用顺序（beginBlock 的结果交给三个模块）；ADR-008「映射放在链里」。
        void testGlazeActsOnlyThroughEffectiveParams()
        {
            beginTest ("GLAZE > 0 acts only through the params handed to the modules (reference fed the same params is bit-exact)");

            for (const auto& [sampleRate, numChannels] : { std::pair<double, int> { 48000.0, 2 }, std::pair<double, int> { 44100.0, 1 } })
                checkEffectiveParamsOnly (sampleRate, numChannels);
        }

        void checkEffectiveParamsOnly (double sampleRate, int numChannels)
        {
            const auto tag = " @" + juce::String (sampleRate, 0) + " ch" + juce::String (numChannels);
            auto input = makeMusicLikeSignal (numChannels, sampleRate, 30.0, 77, 0.2f);

            for (size_t ch = 0; ch < input.size(); ++ch)          // 后一半推热 15.6 dB：学到的方案对这段素材偏猛，保护层会介入
                for (size_t i = input[ch].size() / 2; i < input[ch].size(); ++i)
                    input[ch][i] *= 6.0f;

            auto c = withTightLimitBudgets (glaze::factoryConstants());   // 要让保护层在推热的后一半出手（TestHelpers.h 的说明）
            c.minListenSeconds = 2.0;
            c.maxListenSeconds = 3.0;
            c.planTransitionSeconds = 0.5;
            c.calibrationSettleSeconds = 0.5;
            c.calibrationWindowSeconds = 2.0;
            // 画像失配关掉：推热 15.6 dB 会先触发它（按这一段直接压低上限），这里要覆盖的是保护层的退让路径。
            // 画像失配同样只经「交给模块的参数」起作用（它只是降净增益上限，与约束反馈同一条路），不影响本用例要证的事。
            c.mismatchThresholdLu = c.mismatchReleaseLu = 1.0e9f;

            Params p;
            p.limit.ceilingDb = -1.0f;
            p.glazePercent = 100.0f;
            p.glazeStyle = 2;

            CeladonChain chain;
            chain.getGlazeController().setConstantsForTesting (c);
            chain.setParams (p, false);
            chain.prepare (makeSpec (sampleRate, numChannels));

            ReferenceChain reference;
            reference.prepare (makeSpec (sampleRate, numChannels), chain.getEffectiveParams());

            glaze::HostInfo host;
            host.hasPlayState = host.isPlaying = host.hasPosition = true;

            juce::AudioBuffer<float> a (numChannels, maxBlock), r (numChannels, maxBlock);
            bool identical = true;
            float largestChange = 0.0f, deepestGuard = 0.0f;
            const auto numSamples = (int) input.front().size();

            for (int start = 0; start + maxBlock <= numSamples; start += maxBlock)
            {
                for (int ch = 0; ch < numChannels; ++ch)
                {
                    a.copyFrom (ch, 0, input[(size_t) ch].data() + start, maxBlock);
                    r.copyFrom (ch, 0, input[(size_t) ch].data() + start, maxBlock);
                }

                const bool hostBypassed = (start / maxBlock) % 400 > 380;
                host.positionSamples = start;
                chain.setHostInfo (host);
                chain.setParams (p, hostBypassed);
                reference.setParams (chain.getEffectiveParams(), hostBypassed);

                chain.process (a);
                reference.process (r);
                deepestGuard = juce::jmin (deepestGuard, chain.getGlazeController().getProbeForTesting().executedGuardDb);

                for (int ch = 0; ch < numChannels; ++ch)
                {
                    identical = identical && std::memcmp (a.getReadPointer (ch), r.getReadPointer (ch), sizeof (float) * maxBlock) == 0;

                    for (int i = 0; i < maxBlock; ++i)
                        largestChange = juce::jmax (largestChange, std::abs (a.getSample (ch, i) - input[(size_t) ch][(size_t) (start + i)]));
                }
            }

            const auto probe = chain.getGlazeController().getProbeForTesting();
            logMessage ("  " + tag + " lifecycle covered: commits " + juce::String (probe.commits) + ", calibration rounds "
                        + juce::String (probe.calibrationBegins) + ", feedback steps " + juce::String (probe.feedbackSteps)
                        + ", deepest executed protection " + juce::String (deepestGuard, 2) + " dB; largest change vs input "
                        + juce::String (largestChange, 3));

            expect (largestChange > 0.05f, "GLAZE must actually process" + tag);
            expect (probe.commits >= 1 && probe.calibrationBegins >= 1, "went through learning and calibration" + tag);
            expect (deepestGuard < 0.0f, "the guard acted on the hot half" + tag);
            expect (identical, "chain output == three modules fed the effective params, bit for bit" + tag);
        }

        void testListenDoesNotChangeMastering()
        {
            beginTest ("Listen toggles change monitoring only: GLAZE params, pre-Listen meters and calibration stay identical");
            constexpr double sampleRate = 48000.0;
            constexpr int numChannels = 2;
            auto constants = glaze::factoryConstants();
            constants.minListenSeconds = 2.0;
            constants.maxListenSeconds = 3.0;
            constants.planTransitionSeconds = 0.5;
            constants.calibrationSettleSeconds = 0.5;
            constants.calibrationWindowSeconds = 2.0;
            Params params;
            params.glazePercent = 80.0f;
            params.glazeStyle = 2;
            params.color.toneDb = 0.5f;
            params.color.heat = 15.0f;
            params.limit.ceilingDb = -1.0f;
            CeladonChain monitoring, reference;
            for (auto* chain : { &monitoring, &reference })
            {
                chain->getGlazeController().setConstantsForTesting (constants);
                chain->setParams (params, false);
                chain->prepare (makeSpec (sampleRate, numChannels));
            }
            auto input = makeMusicLikeSignal (numChannels, sampleRate, 12.0, 260926, 0.2f);
            for (auto& channel : input)
                for (size_t i = 3 * channel.size() / 4; i < channel.size(); ++i)
                    channel[i] *= 3.0f;
            juce::AudioBuffer<float> a (numChannels, maxBlock), b (numChannels, maxBlock);
            glaze::HostInfo host;
            host.hasPlayState = host.isPlaying = host.hasPosition = true;
            bool paramsIdentical = true, metersIdentical = true;
            float largestAutoGain = 0.0f, largestMonitorDifference = 0.0f;
            for (int start = 0, block = 0; start + maxBlock <= (int) input[0].size(); start += maxBlock, ++block)
            {
                for (int ch = 0; ch < numChannels; ++ch)
                {
                    a.copyFrom (ch, 0, input[(size_t) ch].data() + start, maxBlock);
                    b.copyFrom (ch, 0, input[(size_t) ch].data() + start, maxBlock);
                }
                host.positionSamples = start;
                monitoring.setHostInfo (host);
                reference.setHostInfo (host);
                auto listenParams = params;
                listenParams.limit.listen = (block / 47) % 2 == 1;
                monitoring.setParams (listenParams, false);
                reference.setParams (params, false);
                auto effective = monitoring.getEffectiveParams();
                effective.limit.listen = false; // only the explicit monitor request differs
                paramsIdentical = paramsIdentical && sameParams (effective, reference.getEffectiveParams());
                largestAutoGain = juce::jmax (largestAutoGain, effective.limit.gainDb - params.limit.gainDb);
                monitoring.process (a);
                reference.process (b);
                metersIdentical = metersIdentical
                    && sameBits (monitoring.getAndResetOutputTruePeakDb(), reference.getAndResetOutputTruePeakDb())
                    && sameBits (monitoring.getOutputShortTermLufs(), reference.getOutputShortTermLufs())
                    && sameBits (monitoring.getOutputIntegratedLufs(), reference.getOutputIntegratedLufs());
                for (int ch = 0; ch < numChannels; ++ch)
                    for (int i = 0; i < maxBlock; ++i)
                        largestMonitorDifference = juce::jmax (largestMonitorDifference, std::abs (a.getSample (ch, i) - b.getSample (ch, i)));
            }
            const auto aProbe = monitoring.getGlazeController().getProbeForTesting();
            const auto bProbe = reference.getGlazeController().getProbeForTesting();
            expect (paramsIdentical, "Listen toggles must not restart planning or alter effective mastering params");
            expect (metersIdentical, "uncompensated master OUT readings must remain bit-exact across Listen switches");
            expect (largestAutoGain > 0.2f && largestMonitorDifference > 0.001f, "both GLAZE and Listen must actually be active");
            expect (aProbe.commits >= 1 && aProbe.calibrationBegins >= 1, "test must cover committed planning and calibration");
            expectEquals (aProbe.calibrationBegins, bProbe.calibrationBegins, "Listen must not pause/restart calibration");
            expectEquals (aProbe.calibrationFinishes, bProbe.calibrationFinishes, "calibration progress is unaffected");
            expect (sameBits (aProbe.executedBiasDb, bProbe.executedBiasDb)
                    && sameBits (aProbe.executedGuardDb, bProbe.executedGuardDb), "Listen must not alter calibration bias or protection");
        }

        // 3. 延迟不随 GLAZE 变：同一 Lookahead、同一采样率下 GLAZE 0 与 100 的 getLatencySamples 相同；同一条链上拧 GLAZE 延迟不动。
        // 依据：ADR-008 / 决策第 5 节「Lookahead、Release、Ceiling … 不自动」；设计规格-总体「延迟」。
        void testLatencyUnchanged()
        {
            beginTest ("latency is the same at GLAZE 0 and 100 and while turning it");

            for (auto sampleRate : { 44100.0, 48000.0, 96000.0 })
             for (auto lookaheadMs : { 1.0f, 2.0f, 10.0f })
            {
                const auto tag = " @" + juce::String (sampleRate) + " la=" + juce::String (lookaheadMs);
                auto p = paramsAt (1, 0, 1, 0.0f);
                p.limit.lookaheadMs = lookaheadMs;

                CeladonChain atZero, atFull;
                atZero.setParams (p, false);
                atZero.prepare (makeSpec (sampleRate, 2));
                p.glazePercent = 100.0f;
                atFull.setParams (p, false);
                atFull.prepare (makeSpec (sampleRate, 2));

                const auto latency = atZero.getLatencySamples();
                expect (latency > 0, "latency must be positive" + tag);
                expectEquals (atFull.getLatencySamples(), latency, "GLAZE 100 vs 0" + tag);

                atZero.setParams (p, false);
                expectEquals (atZero.getLatencySamples(), latency, "after turning GLAZE up" + tag);
                p.glazePercent = 0.0f;
                atZero.setParams (p, false);
                expectEquals (atZero.getLatencySamples(), latency, "after turning GLAZE back down" + tag);
            }
        }

        // 4. 块率边带（抓拉链噪声）：稳态三音（220 / 330 / 440 Hz）、48 kHz、块 240（块率 200 Hz，避开 Balance hop 的倍数），
        //    先按这段素材学好画像（从存档起步、离线 = 纯前馈），GLAZE 在 2^18 个样本内逐块从 0 拧到 100（宿主式每块一个值）。
        //    输出加 Hann 窗做 2^18 点 FFT，各音 ± k × 200 Hz（k = 1 ～ 3，± 2 Hz）处的边带总功率 ≤ −95 dBFS（v0.4 评审定的口径）。
        //    负对照：GLAZE 0 的稳态输出乘「每块一个常数、不平滑」的 0 → 8 dB 增益，必须高于门槛。
        // 依据：时间线第 4 节「GLAZE 连续控制：按确定映射逐块更新自动增量，再经各参数自己的平滑（与模块现有平滑同级）」。
        void testSweepHasNoBlockRateSidebands()
        {
            beginTest ("GLAZE sweep leaves no block-rate sidebands (<= -95 dBFS), and the metric catches an unsmoothed control");

            constexpr double sampleRate = 48000.0;
            constexpr int numChannels = 2;
            constexpr int blockSize = 240;
            constexpr int fftOrder = 18;
            constexpr int analysisLength = 1 << fftOrder;
            constexpr double blockRateHz = sampleRate / blockSize;
            constexpr double thresholdDbfs = -95.0;
            const double toneHz[] = { 220.0, 330.0, 440.0 };
            const auto warmUp = (int) (0.5 * sampleRate);

            auto sidebandDbfs = [&] (const std::vector<float>& samples)
            {
                juce::dsp::FFT fft (fftOrder);
                std::vector<float> buffer ((size_t) analysisLength * 2, 0.0f);
                double windowSum = 0.0;

                for (int i = 0; i < analysisLength; ++i)
                {
                    const auto w = 0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * i / analysisLength);
                    buffer[(size_t) i] = (float) (w * samples[(size_t) i]);
                    windowSum += w;
                }

                fft.performFrequencyOnlyForwardTransform (buffer.data());

                const auto binHz = sampleRate / analysisLength;
                constexpr double hannNoiseBandwidthBins = 1.5;
                double power = 0.0;

                for (auto hz : toneHz)
                    for (int k = 1; k <= 3; ++k)
                        for (auto sign : { -1.0, 1.0 })
                        {
                            const auto centre = std::abs (hz + sign * k * blockRateHz);
                            const auto first = (int) std::floor ((centre - 2.0) / binHz);
                            const auto last  = (int) std::ceil  ((centre + 2.0) / binHz);

                            for (int bin = juce::jmax (1, first); bin <= last; ++bin)
                            {
                                const auto amplitude = 2.0 * (double) buffer[(size_t) bin] / windowSum;
                                power += amplitude * amplitude / hannNoiseBandwidthBins;
                            }
                        }

                return 10.0 * std::log10 (juce::jmax (1.0e-30, power));
            };

            const auto learnInput = makeTones (numChannels, sampleRate, (int) (32.0 * sampleRate));
            const auto state = learnState (learnInput, sampleRate);
            expect (state != nullptr);

            Params neutral;
            neutral.limit.ceilingDb = -1.0f;

            RestoredChain sweeping (*state, sampleRate, numChannels, neutral);
            const auto latency = sweeping.chain.getLatencySamples();
            const auto sweepLength = analysisLength + latency;
            const auto input = makeTones (numChannels, sampleRate, warmUp + sweepLength);
            const auto numSweepBlocks = (sweepLength + blockSize - 1) / blockSize;
            const auto firstSweepBlock = warmUp / blockSize;

            const auto swept = sweeping.render (input, blockSize, [&] (int blockIndex)
            {
                const auto b = blockIndex - firstSweepBlock;
                sweeping.params.glazePercent = b <= 0 ? 0.0f : juce::jmin (100.0f, 100.0f * (float) b / (float) numSweepBlocks);
            });

            expect (allFinite (swept));
            expect (sweeping.chain.getGlazeStatus().profileReady, "swept on a learned profile");

            const std::vector<float> sweptWindow (swept[0].begin() + warmUp + latency, swept[0].begin() + warmUp + latency + analysisLength);
            const auto realDbfs = sidebandDbfs (sweptWindow);

            RestoredChain steadyChain (*state, sampleRate, numChannels, neutral);
            const auto steady = steadyChain.render (input, blockSize);
            std::vector<float> steppedWindow (steady[0].begin() + warmUp + latency, steady[0].begin() + warmUp + latency + analysisLength);
            const auto numWindowBlocks = analysisLength / blockSize;

            for (int i = 0; i < analysisLength; ++i)
                steppedWindow[(size_t) i] *= juce::Decibels::decibelsToGain (8.0f * (float) (i / blockSize) / (float) numWindowBlocks);

            const auto steppedDbfs = sidebandDbfs (steppedWindow);

            logMessage ("  block-rate sidebands: real GLAZE sweep " + juce::String (realDbfs, 1) + " dBFS; unsmoothed control "
                        + juce::String (steppedDbfs, 1) + " dBFS; threshold " + juce::String (thresholdDbfs, 1) + " dBFS");
            expect (realDbfs < thresholdDbfs, "GLAZE sweep sidebands " + juce::String (realDbfs, 1) + " dBFS >= threshold");
            expect (steppedDbfs > thresholdDbfs, "the unsmoothed control must be caught (" + juce::String (steppedDbfs, 1) + " dBFS)");
        }

        // 5. 拧动无咔哒：学好画像后（存档起步、离线纯前馈），48 kHz、块 256，GLAZE 2 s 内 0 → 100 再 2 s 回 0，类音乐与稳态三音两种素材。
        //    逐块门槛：每块的最大一阶差分 ≤ 「GLAZE 恒为夹住本块值的两档（0 / 25 / 50 / 75 / 100）稳态输出在同一块的最大差分」× 1.5 + 0.02；
        //    负对照：在（延迟段之后）门槛最低的块里加一个 −20 dBFS 的单样本阶跃，逐块检查必须抓住。
        // 依据：同上（各参数自己的平滑接住逐块更新）；v0.4 同名用例的口径（2026-09-25 评审）。
        void testSweepHasNoClicks()
        {
            beginTest ("sweeping GLAZE 0 -> 100 -> 0 over 4 s on a learned profile: finite, no clicks (per-block bound, negative control)");

            constexpr double sampleRate = 48000.0;
            constexpr int numChannels = 2;
            constexpr int blockSize = 256;
            constexpr double rampSeconds = 2.0;
            constexpr float stationaryAmounts[] = { 0.0f, 25.0f, 50.0f, 75.0f, 100.0f };
            constexpr float injectedClick = 0.1f;
            const auto blocksPerRamp = (int) std::ceil (rampSeconds * sampleRate / blockSize);

            auto glazeAtBlock = [blocksPerRamp] (int blockIndex)
            {
                const auto position = blockIndex < blocksPerRamp ? blockIndex : 2 * blocksPerRamp - blockIndex;
                return juce::jlimit (0.0f, 100.0f, 100.0f * (float) position / (float) blocksPerRamp);
            };

            struct Material { const char* name; Signal learn; Signal input; };
            const Material materials[] =
            {
                { "music-like",   makeMusicLikeSignal (numChannels, sampleRate, 32.0, 777, 0.3f), makeMusicLikeSignal (numChannels, sampleRate, 2.0 * rampSeconds, 778, 0.3f) },
                { "steady tones", makeTones (numChannels, sampleRate, (int) (32.0 * sampleRate)),  makeTones (numChannels, sampleRate, (int) (2.0 * rampSeconds * sampleRate)) },
            };

            Params neutral;
            neutral.limit.ceilingDb = -1.0f;

            for (const auto& material : materials)
            {
                const auto tag = juce::String (" [") + material.name + "]";
                const auto state = learnState (material.learn, sampleRate);
                const auto numSamples = (int) material.input.front().size();
                const auto numBlocks = (numSamples + blockSize - 1) / blockSize;

                RestoredChain sweeping (*state, sampleRate, numChannels, neutral);
                const auto swept = sweeping.render (material.input, blockSize, [&] (int b) { sweeping.params.glazePercent = glazeAtBlock (b); });
                expect (allFinite (swept), "sweep output must be finite" + tag);

                std::vector<Signal> stationary;
                for (auto amount : stationaryAmounts)
                {
                    auto p = neutral;
                    p.glazePercent = amount;
                    RestoredChain chain (*state, sampleRate, numChannels, p);
                    stationary.push_back (chain.render (material.input, blockSize));
                }

                auto blockMaxDiff = [] (const Signal& signal, int blockIndex)
                {
                    float worst = 0.0f;
                    const auto start = (size_t) juce::jmax (1, blockIndex * blockSize);
                    const auto end = juce::jmin (signal.front().size(), (size_t) ((blockIndex + 1) * blockSize));

                    for (const auto& channel : signal)
                        for (auto i = start; i < end; ++i)
                            worst = juce::jmax (worst, std::abs (channel[i] - channel[i - 1]));

                    return worst;
                };

                const auto firstSettledBlock = (sweeping.chain.getLatencySamples() + (int) (0.05 * sampleRate)) / blockSize + 1;
                float worstRatio = 0.0f, lowestThreshold = std::numeric_limits<float>::max();
                int lowestThresholdBlock = firstSettledBlock, localFailures = 0;

                for (int b = 0; b < numBlocks; ++b)
                {
                    const auto amount = glazeAtBlock (b);
                    const auto lower = juce::jmin ((int) std::size (stationaryAmounts) - 1, (int) (amount / 25.0f));
                    const auto upper = juce::jmin ((int) std::size (stationaryAmounts) - 1, lower + 1);
                    const auto envelope = juce::jmax (blockMaxDiff (stationary[(size_t) lower], b), blockMaxDiff (stationary[(size_t) upper], b));
                    const auto threshold = envelope * 1.5f + 0.02f;
                    const auto sweptBlock = blockMaxDiff (swept, b);

                    localFailures += sweptBlock > threshold ? 1 : 0;

                    if (envelope > 0.0f)
                        worstRatio = juce::jmax (worstRatio, sweptBlock / envelope);

                    if (b >= firstSettledBlock && threshold < lowestThreshold)
                    {
                        lowestThreshold = threshold;
                        lowestThresholdBlock = b;
                    }
                }

                logMessage ("  " + juce::String (material.name) + ": worst sweep / stationary first-difference ratio " + juce::String (worstRatio, 3)
                            + ", lowest local threshold " + juce::String (lowestThreshold, 4));
                expectEquals (localFailures, 0, "blocks whose first difference exceeds the local bound" + tag);

                auto clicked = swept;
                const auto at = (size_t) (lowestThresholdBlock * blockSize + blockSize / 2);
                const auto step = clicked[0][at] - clicked[0][at - 1];
                clicked[0][at] += step >= 0.0f ? injectedClick : -injectedClick;
                expect (blockMaxDiff (clicked, lowestThresholdBlock) > lowestThreshold, "the per-block bound must catch a -20 dBFS click" + tag);
            }
        }

        // 6. 真峰值：从未学习开始走完整个生命周期（聆听、方案过渡、校准、保护层、约束反馈；后半段素材推热 14 dB），GLAZE 100、
        //    手动 Gain +6、Ceiling −1 dBTP，1 kHz 0 dBFS 正弦与类音乐两种素材：跳过开头「延迟 + 50 ms」后输出真峰值
        //    （插件自己的 TruePeakDetector）≤ −1 dBTP + 0.01 dB；16× 参考只记录。
        // 依据：设计规格-GLAZE 验收总表第 4 条（限幅器与 Ceiling 硬保证）；Limit 规格验收 1 对整条链同样成立。
        void testTruePeakNeverAboveCeiling()
        {
            beginTest ("true peak <= ceiling + 0.01 dB through the whole GLAZE lifecycle at GLAZE 100, user gain +6");

            constexpr double sampleRate = 48000.0;
            constexpr int numChannels = 2;
            constexpr float ceilingDb = -1.0f;

            auto c = glaze::factoryConstants();
            c.minListenSeconds = 2.0;
            c.maxListenSeconds = 3.0;
            c.planTransitionSeconds = 0.5;
            c.calibrationSettleSeconds = 0.5;
            c.calibrationWindowSeconds = 2.0;

            struct NamedSignal { const char* name; Signal signal; };
            NamedSignal signals[] =
            {
                { "1 kHz sine 0 dBFS", makeSine (numChannels, sampleRate, 1000.0, 1.0f, 30.0) },
                { "music-like",        makeMusicLikeSignal (numChannels, sampleRate, 30.0, 31, 0.2f) },
            };

            for (auto& ch : signals[1].signal)
                for (size_t i = ch.size() / 2; i < ch.size(); ++i)
                    ch[i] *= 5.0f;

            for (const auto& named : signals)
            {
                Params p;
                p.limit.gainDb = 6.0f;
                p.limit.ceilingDb = ceilingDb;
                p.glazePercent = 100.0f;

                CeladonChain chain;
                chain.getGlazeController().setConstantsForTesting (c);
                chain.setParams (p, false);
                chain.prepare (makeSpec (sampleRate, numChannels));

                auto input = named.signal;
                appendSilence (input, chain.getLatencySamples() + 64);
                auto output = input;
                const auto numSamples = (int) input.front().size();
                juce::AudioBuffer<float> block (numChannels, maxBlock);
                glaze::HostInfo host;
                host.hasPlayState = host.isPlaying = host.hasPosition = true;

                for (int start = 0; start < numSamples; start += maxBlock)
                {
                    const auto length = juce::jmin (maxBlock, numSamples - start);
                    block.setSize (numChannels, length, false, false, true);

                    for (int ch = 0; ch < numChannels; ++ch)
                        block.copyFrom (ch, 0, input[(size_t) ch].data() + start, length);

                    host.positionSamples = start;
                    chain.setHostInfo (host);
                    chain.setParams (p, false);
                    chain.process (block);

                    for (int ch = 0; ch < numChannels; ++ch)
                        std::copy (block.getReadPointer (ch), block.getReadPointer (ch) + length, output[(size_t) ch].begin() + start);
                }

                expect (allFinite (output), juce::String (named.name) + ": non-finite output");

                const auto skip = chain.getLatencySamples() + (int) (0.05 * sampleRate);
                const auto ownDb = toDb (truePeakAfter (output, skip)) - ceilingDb;
                const auto referenceDb = toDb (referenceTruePeakOf (sliceWithFadedHead (output, skip), 0.5)) - ceilingDb;
                const auto probe = chain.getGlazeController().getProbeForTesting();

                logMessage ("  " + juce::String (named.name) + ": overshoot own meter " + juce::String (ownDb, 4) + " dB, 16x reference "
                            + juce::String (referenceDb, 4) + " dB; commits " + juce::String (probe.commits) + ", calibration rounds "
                            + juce::String (probe.calibrationBegins) + ", feedback steps " + juce::String (probe.feedbackSteps));

                expect (ownDb <= 0.01f, juce::String (named.name) + ": overshoot " + juce::String (ownDb, 4) + " dB");
                expect (ownDb > -1.0f, juce::String (named.name) + ": the limiter must actually be working near the ceiling");
                expect (probe.commits >= 1, "the lifecycle ran");
            }
        }

        // 7. 学好的 LIVE 画像留着时，响度跟着 GLAZE 走，但 Limit Gain 上行只有 0.2 dB/s、G_trim 只有 0.05 dB/s
        //    （INTERFACE-SKETCH / GlazeController::Impl）。10 s 里 50 / 75 / 100 的已施加增益都停在 2.5 dB
        //    （2.0 + 0.5），积分响度贴在一起；目标净增益仍随档位上升。守门为 0，G_trim 停在这 10 s 的速率积分 0.5，
        //    不是 ±3 dB 钳位；真峰值离 Ceiling −1 还远。40 s 后各档拉开，逐级 > 0.1 LU，GLAZE 100 的已施加增益
        //    停在 10 dB（8 + 2），仍是同一条滑移，不是响度顶。
        //    先读档再 prepare 与先 prepare 再读档必须是同一条曲线。画像被第一次 prepare 清掉时，10 s 会走出另一条
        //    低置信曲线（50 档还能比 25 响约 0.5 LU），那不是这份合同。
        void testLoudnessRisesWithGlaze()
        {
            beginTest ("after learning, loudness follows the 0.2 dB/s slew: 10 s plateaus after GLAZE 25, 40 s steps rise, either restore order");

            constexpr double sampleRate = 48000.0;
            constexpr int numChannels = 2;
            constexpr float ceilingDb = -1.0f;
            const auto learn = makeMusicLikeSignal (numChannels, sampleRate, 32.0, 2026, 0.35f);
            const auto input10 = makeMusicLikeSignal (numChannels, sampleRate, 10.0, 2027, 0.35f);
            const auto input40 = makeMusicLikeSignal (numChannels, sampleRate, 40.0, 2027, 0.35f);
            const auto state = learnState (learn, sampleRate);
            expect (state != nullptr && state->getIntAttribute ("formatVersion") == glaze::liveContainerFormatVersion,
                    "learned a LIVE format-2 state");

            struct Reading
            {
                float lufs = 0.0f, target = 0.0f, effectiveGain = 0.0f, applied = 0.0f, trim = 0.0f, guard = 0.0f, truePeak = 0.0f;
            };

            auto measure = [&] (bool restoreFirst, float glaze, const Signal& input) -> Reading
            {
                CeladonChain chain;
                Params p;
                p.limit.ceilingDb = ceilingDb;
                p.glazePercent = glaze;
                chain.setParams (p, false);

                if (restoreFirst)
                    chain.restoreGlazeStateXml (state.get());

                chain.prepare (makeSpec (sampleRate, numChannels));

                if (! restoreFirst)
                    chain.restoreGlazeStateXml (state.get());

                glaze::HostInfo host;
                host.offline = true;
                host.hasPlayState = host.isPlaying = true;
                const auto numSamples = (int) input.front().size();
                juce::AudioBuffer<float> block (numChannels, maxBlock);
                auto output = input;

                for (int start = 0; start < numSamples; start += maxBlock)
                {
                    const auto length = juce::jmin (maxBlock, numSamples - start);
                    block.setSize (numChannels, length, false, false, true);

                    for (int ch = 0; ch < numChannels; ++ch)
                        block.copyFrom (ch, 0, input[(size_t) ch].data() + start, length);

                    host.positionSamples = start;
                    chain.setHostInfo (host);
                    chain.setParams (p, false);
                    chain.process (block);

                    for (int ch = 0; ch < numChannels; ++ch)
                        std::copy (block.getReadPointer (ch), block.getReadPointer (ch) + length,
                                   output[(size_t) ch].begin() + start);
                }

                expect (allFinite (output));
                const auto status = chain.getGlazeStatus();
                const auto probe = chain.getGlazeController().getProbeForTesting();
                Reading reading;
                reading.lufs = integratedLoudnessOf (output, sampleRate, (int) (0.5 * sampleRate));
                reading.target = status.effectiveTargetLufs;
                reading.effectiveGain = probe.targetPlan.effectiveGainDb;
                reading.applied = chain.getEffectiveParams().limit.gainDb;
                reading.trim = probe.appliedBiasDb;
                reading.guard = status.protectionDb;
                reading.truePeak = chain.getAndResetOutputTruePeakDb();
                return reading;
            };

            auto expectSame = [&] (const Reading& early, const Reading& late, const juce::String& what)
            {
                expect (std::abs (early.lufs - late.lufs) < 0.001f, what + ": loudness matches either order");
                expect (std::abs (early.applied - late.applied) < 0.001f, what + ": applied gain matches either order");
                expect (std::abs (early.effectiveGain - late.effectiveGain) < 0.001f, what + ": target gain matches either order");
            };

            juce::String summary = "  10 s";
            Reading early10[8], late10[8];
            const float steps10[] = { 0.0f, 0.1f, 1.0f, 5.0f, 25.0f, 50.0f, 75.0f, 100.0f };

            for (int i = 0; i < 8; ++i)
            {
                early10[i] = measure (true, steps10[i], input10);
                late10[i] = measure (false, steps10[i], input10);
                expectSame (early10[i], late10[i], "10 s GLAZE " + juce::String (steps10[i], 1));
                summary << " " << juce::String (steps10[i], 1) << " -> " << juce::String (early10[i].lufs, 3)
                        << " (eff " << juce::String (early10[i].effectiveGain, 2)
                        << ", applied " << juce::String (early10[i].applied, 2) << ");";
            }

            for (int i = 1; i <= 3; ++i)
                expect (early10[i].lufs >= early10[0].lufs - 0.02f,
                        "GLAZE " + juce::String (steps10[i], 1) + " must not be quieter than GLAZE 0");

            // 10 s：50/75/100 贴在 25 附近（滑移还没走到更高的目标），目标净增益仍逐档上升。
            for (int i = 5; i <= 7; ++i)
            {
                expect (std::abs (early10[i].lufs - early10[4].lufs) < 0.1f,
                        "10 s GLAZE " + juce::String (steps10[i], 0) + " stays with GLAZE 25 while the slew catches up");
                expect (std::abs (early10[i].applied - 2.5f) < 0.05f,
                        "10 s applied gain is the 0.2 dB/s + 0.05 dB/s budget (2.5 dB)");
                expect (std::abs (early10[i].trim - 0.5f) < 0.02f,
                        "10 s G_trim is the 0.05 dB/s integral, inside the +/-3 dB clamp");
                expect (std::abs (early10[i].guard) < 0.001f, "10 s guard relief is zero");
                expect (early10[i].truePeak <= ceilingDb + 0.01f, "10 s true peak stays at or under the ceiling");
                expect (early10[i].truePeak < ceilingDb - 2.0f, "10 s true peak is not what flattens the curve");
                expect (early10[i].effectiveGain > early10[i - 1].effectiveGain + 1.0f,
                        "10 s target gain still steps up at GLAZE " + juce::String (steps10[i], 0));
            }

            summary << " | 40 s";
            Reading early40[5], late40[5];
            const float steps40[] = { 0.0f, 25.0f, 50.0f, 75.0f, 100.0f };
            auto previous = 0.0f;

            for (int i = 0; i < 5; ++i)
            {
                early40[i] = measure (true, steps40[i], input40);
                late40[i] = measure (false, steps40[i], input40);
                expectSame (early40[i], late40[i], "40 s GLAZE " + juce::String (steps40[i], 0));
                summary << " " << juce::String (steps40[i], 0) << " -> " << juce::String (early40[i].lufs, 2)
                        << " (applied " << juce::String (early40[i].applied, 2) << ");";

                if (i > 0)
                    expect (early40[i].lufs - previous > 0.1f,
                            "40 s GLAZE " + juce::String (steps40[i], 0) + " is > 0.1 LU louder than the previous step");

                previous = early40[i].lufs;
                expect (early40[i].truePeak <= ceilingDb + 0.01f, "40 s true peak stays at or under the ceiling");
                expect (std::abs (early40[i].guard) < 0.001f, "40 s guard relief is zero");
            }

            expect (std::abs (early40[4].applied - 10.0f) < 0.05f,
                    "40 s GLAZE 100 applied gain is the 0.2 dB/s + 0.05 dB/s budget (10 dB)");
            expect (std::abs (early40[4].trim - 2.0f) < 0.05f,
                    "40 s G_trim is the 0.05 dB/s integral, still inside +/-3 dB");
            logMessage (summary);
        }

        // 新插入只调一个旋钮：无需手动 Gain、Heat 或学习按钮，整条链自动起效。
        void testOneKnobFromFreshInsert()
        {
            beginTest ("fresh insert: only GLAZE 50 activates loudness, spectral processing and colour automatically");
            constexpr double sr = 48000.0;
            const auto input = makeMusicLikeSignal (2, sr, 40.0, 260926, 0.3f);
            CeladonChain chain;
            Params p;
            p.glazePercent = 50.0f;
            chain.setParams (p, false);
            chain.prepare (makeSpec (sr, 2));
            juce::AudioBuffer<float> block (2, maxBlock);
            LoudnessMeter dryMeter;
            dryMeter.prepare (makeSpec (sr, 2));
            bool finite = true, hadGain = false, hadColour = false, hadBalance = false;
            glaze::HostInfo host;
            host.hasPosition = host.hasPlayState = host.isPlaying = true;
            for (int start = 0; start + maxBlock <= (int) input.front().size(); start += maxBlock)
            {
                for (int ch = 0; ch < 2; ++ch)
                    block.copyFrom (ch, 0, input[(size_t) ch].data() + start, maxBlock);
                dryMeter.process (block);
                host.positionSamples = start;
                chain.setHostInfo (host);
                chain.setParams (p, false);
                finite = ! chain.process (block) && finite;
                const auto& effective = chain.getEffectiveParams();
                hadGain = hadGain || effective.limit.gainDb > 0.25f;
                hadColour = hadColour || effective.color.heat > 0.1f;
                hadBalance = hadBalance || (effective.balance.tame > 0.0f && effective.balance.automaticRecover > 0.0f);
            }
            expect (finite && hadGain && hadColour && hadBalance, "all three automatic stages operate with default manual controls");
            expect (chain.getGlazeStatus().profileReady, "first learning finishes without pressing Relearn");
            const auto increase = chain.getOutputIntegratedLufs() - dryMeter.getIntegratedLufs();
            logMessage ("  fresh GLAZE 50 loudness increase: " + juce::String (increase, 2) + " LU");
            expect (increase > 1.0f, "fresh one-knob processing audibly increases measured integrated loudness");
            expect (chain.getAndResetOutputTruePeakDb() <= p.limit.ceilingDb + 0.01f);
        }

        void testNewSpectralProblemAfterLearning()
        {
            beginTest ("a learned profile with no spectral evidence still tames a new resonance, without relearning or allocation");
            constexpr double sr = 48000.0;
            const auto learned = learnState (makeMusicLikeSignal (2, sr, 32.0, 260927, 0.3f), sr);
            glaze::CommittedState committed;
            if (learned == nullptr || ! glaze::fromXml (*learned, committed) || ! committed.hasProfile)
            {
                expect (false, "learned state available");
                return;
            }
            committed.profile.tameEvidenceDb.fill (0.0f);
            committed.profile.recoverEvidenceDb.fill (0.0f);
            committed.profileId = glaze::computeProfileId (committed.profile);
            const auto state = glaze::toXml (committed);
            Params p;
            p.glazePercent = 100.0f;
            RestoredChain restored (*state, sr, 2, p);
            restored.host.offline = false;
            juce::AudioBuffer<float> block (2, maxBlock);
            constexpr int resonanceBand = 14; // 1 kHz
            bool finite = true;
            std::uint64_t allocations = 0;
            for (int start = 0; start < (int) (6.0 * sr); start += maxBlock)
            {
                for (int i = 0; i < maxBlock; ++i)
                {
                    const auto t = (double) (start + i) / sr;
                    double sample = 0.0;
                    for (int band = 0; band < BalanceModule::numBands; ++band)
                    {
                        const auto amplitude = band == resonanceBand && t >= 3.0 ? 0.032 : 0.008;
                        sample += amplitude * std::sin (juce::MathConstants<double>::twoPi * BalanceModule::getBandCentreHz (band) * t + band);
                    }
                    block.setSample (0, i, (float) sample);
                    block.setSample (1, i, (float) sample);
                }
                const auto before = allocationCountOnThisThread();
                restored.chain.setHostInfo (restored.host);
                restored.chain.setParams (p, false);
                finite = ! restored.chain.process (block) && finite;
                allocations += allocationCountOnThisThread() - before;
            }
            BalanceModule::BandGains gains;
            restored.chain.getBalanceBandGainsDb (gains);
            expect (finite);
            expect (allocations == 0, "automatic runtime processing adds no C++ heap allocations");
            expect (gains[(size_t) resonanceBand] < -0.1f, "new resonance is attenuated despite zero stored evidence");
            const auto afterXml = restored.chain.createGlazeStateXml();
            glaze::CommittedState after;
            expect (afterXml != nullptr && glaze::fromXml (*afterXml, after) && after.profileId == committed.profileId,
                    "live EQ response does not require changing the song profile");
        }

        // 8. 四种风格的方向（同一份画像、GLAZE 100、离线纯前馈）：温润的 Glue 增量最大、冲击的 Clip 增量最大；Heat 的风格取向由
        //    驱动上限体现（温润 > 冲击 > 通透），自动 Heat 只给各风格的底量 × 强度（T-032 R6-01，算法 8：出厂残差 Heat 表几乎不削峰，
        //    原来「温润 Heat 最多、通透最少」靠自动 Heat 顶到 heatDriveCap 70 / 50 / 20 的缺陷成立；出厂底量三种风格都 12，风格间的
        //    Heat 差异待 Max 试听标定为风格常数）；「自动」的锁定权重和为 1。实际模块用量（Glue / Limit GR 每块最大值的均值）只记录。
        // 依据：决策第 1 节风格表（温润 Heat、Glue 为主；冲击 Clip 为主；通透 Limit 为主、Heat 极少）；设计规格-GLAZE 7.1「自动 Heat 12（底量）」。
        void testStylesLeanOnTheirModules()
        {
            beginTest ("styles lean on the modules the spec names (warm: Glue, punchy: Clip; Heat stays at each style's floor, caps warm > punchy > clean)");

            constexpr double sampleRate = 48000.0;
            constexpr int numChannels = 2;
            const auto learn = makeMusicLikeSignal (numChannels, sampleRate, 32.0, 555, 0.3f);
            const auto input = makeMusicLikeSignal (numChannels, sampleRate, 6.0, 556, 0.3f);
            const auto state = learnState (learn, sampleRate);

            glaze::CommittedState committed;
            expect (state != nullptr && glaze::fromXml (*state, committed) && committed.hasProfile);

            const auto weights = committed.lockedWeights;
            expect (std::abs (weights.warm + weights.punchy + weights.clean - 1.0f) < 1.0e-5f, "automatic weights sum to one");

            struct Result { glaze::AutoIncrements inc; float glueGr = 0.0f, limitGr = 0.0f; };
            Result results[4];

            for (int style = 0; style < 4; ++style)
            {
                Params p;
                p.limit.ceilingDb = -1.0f;
                p.glazePercent = 100.0f;
                p.glazeStyle = style;

                RestoredChain chain (*state, sampleRate, numChannels, p);
                double glueSum = 0.0, limitSum = 0.0;
                int blocks = 0;

                chain.render (input, maxBlock, [&] (int)
                {
                    glueSum += chain.chain.getAndResetGlueGainReductionDb();
                    limitSum += chain.chain.getAndResetLimitGainReductionDb();
                    ++blocks;
                });

                results[style].inc = chain.chain.getGlazeController().getProbeForTesting().increments;
                results[style].glueGr = (float) (glueSum / juce::jmax (1, blocks));
                results[style].limitGr = (float) (limitSum / juce::jmax (1, blocks));

                const auto& inc = results[style].inc;
                logMessage ("  style " + juce::String (style) + ": glue " + juce::String (inc.colorGlue, 1) + " heat " + juce::String (inc.colorHeat, 1)
                            + " clip " + juce::String (inc.limitClip, 1) + " gain " + juce::String (inc.limitGainDb, 2) + " dB tone "
                            + juce::String (inc.colorToneDb, 2) + " | measured glue GR " + juce::String (results[style].glueGr, 2)
                            + " dB, limit GR " + juce::String (results[style].limitGr, 2) + " dB");
            }

            const auto& warm = results[1].inc;
            const auto& punchy = results[2].inc;
            const auto& clean = results[3].inc;

            const auto& fc = glaze::factoryConstants();
            expect (warm.colorGlue >= punchy.colorGlue && warm.colorGlue >= clean.colorGlue, "warm uses the most Glue");
            expect (punchy.limitClip >= warm.limitClip && punchy.limitClip >= clean.limitClip, "punchy uses the most Clip");
            expect (fc.warm.heatDriveCap > fc.punchy.heatDriveCap && fc.punchy.heatDriveCap > fc.clean.heatDriveCap,
                    "Heat caps: warm > punchy > clean");
            expect (warm.colorHeat > 0.0f && warm.colorHeat <= fc.warm.automaticHeatFloor + 1.0e-3f
                        && punchy.colorHeat > 0.0f && punchy.colorHeat <= fc.punchy.automaticHeatFloor + 1.0e-3f
                        && clean.colorHeat > 0.0f && clean.colorHeat <= fc.clean.automaticHeatFloor + 1.0e-3f,
                    "automatic Heat is each style's floor x strength, not its drive cap (the residual Heat table barely shaves peaks)");
        }

        //==============================================================================
        // 9. T-032 R6-02：手动单独超出某个模块的预算（limitedByManual：界面提示；该模块的自动增量撤回，自动 Gain 与其余模块照常展开）
        //    与「电平类自动全部撤回」（Plan::automaticLevelWithdrawn：手动已让限幅器超约束 / Limit 旁通 / 约束反馈上限低于手动链净增益）
        //    分开：画像失配、保护层、上游监视、校准与约束反馈都只在后者时停。R6-02 收尾（A11）之前保护层与上游监视按「手动 + 自动」的
        //    总量核对、分不出手动造成的超出，所以前三层只好在「受手动设置限制」时整体关掉（规格「学习、过渡、校准期间保护层同样生效」
        //    在这些组合下被静默取消）；照常开工又会把手动的超出当成自动的超限：保护层白白退让自动 Gain，约束反馈每约 3.5 s 降 1 dB 净增益
        //    上限，直到自动增益磨光（上限只降不升、随工程保存，IMPL-A7 第 3 节）。A11 起两者只核对自动部分：每项有效上限 = max(风格上限,
        //    手动单独用量)，手动链单独已超预算的那一项（Plan::manualBudgetMask）自动部分恒为 0、按不设约束核对，其余照风格上限；
        //    「手动超预算」的项变了就清空两个窗口。
        //    a. 通透、GLAZE 100、手动 Heat 25（通透 Heat 上限 20）：界面提示受手动限制；保护层在跑、只放开 Heat 一项；校准照常开轮；
        //       30 s 里上游监视不因手动 Heat 触发、上限不降、自动 Gain 不被磨掉（校准偏差可逆、不存档，不算「磨」）；画像失配照常判定；
        //       手动 Heat 拧回上限以内的头 4 s 不因旧的监视窗误触发约束反馈；关掉画像失配、放一段比画像响的音乐时保护层照常收回自动
        //       Gain，回到原电平后 30 s 不再降上限、自动 Gain 还在。
        // 依据：决策与校准第 8 节「手动部分本身已超约束时，自动先撤回自己的增量…不偷偷反向改旋钮」「绝不自动抵消你的修改」、
        //    第 7 节保护层「学习、过渡、校准期间保护层同样生效」、第 4 节约束反馈；实现修订第 5 节 F-01（画像失配）、F-02（逐模块钳住）；
        //    任务书 BRIEF-A11 第 1 ～ 4 条。

        // 保护层此刻的上限：只有 released* 指定的项是 0（不设约束），其余逐位是通透风格的上限（风格固定 = 通透，权重独热）。
        static bool onlyReleased (const glaze::StyleConstants& limits, bool releasedHeat, bool releasedGlue, bool releasedClip = false)
        {
            const auto& clean = glaze::factoryConstants().clean;
            const auto capOr0 = [] (bool released, float cap) { return released ? 0.0f : cap; };
            return sameBits (limits.heatDriveCap, capOr0 (releasedHeat, clean.heatDriveCap))
                && sameBits (limits.glueGrMeanCapDb, capOr0 (releasedGlue, clean.glueGrMeanCapDb))
                && sameBits (limits.glueGrP95CapDb, capOr0 (releasedGlue, clean.glueGrP95CapDb))
                && sameBits (limits.clipDepthCapDb, capOr0 (releasedClip, clean.clipDepthCapDb))
                && sameBits (limits.clipActivityCap, capOr0 (releasedClip, clean.clipActivityCap))
                && sameBits (limits.limitGrMeanCapDb, clean.limitGrMeanCapDb) && sameBits (limits.limitGrP95CapDb, clean.limitGrP95CapDb)
                && sameBits (limits.limitGrPeakCapDb, clean.limitGrPeakCapDb);
        }

        // 净增益里「可能被磨掉」的那部分：实际 Limit Gain − 校准偏差（= 手动 + 方案的自动增量 + 保护偏移）。校准偏差可逆、不存档，
        // 手动某一项超预算时方案的手动链估计不准，校准照常把它修回来，不算「磨」；约束反馈降上限（只降不升、随工程保存）才是。
        static float drainableGainDb (const LiveRig& rig)
        {
            return rig.chain.getEffectiveParams().limit.gainDb - rig.probe().executedBiasDb;
        }

        // 保护层对自动部分照常动作、且不磨光（任务书第 4 条「保护层动作过、校准开过，同时不磨光」）：同一组设置、关掉画像失配（只让
        // 保护层与约束反馈处理「比画像响」），学完、校准开过之后放 3 s 响 burst 倍的音乐——限幅超出时保护层退让（修前保护层整个关着，
        // g_保护 与 executedGuardDb 恒为 0）；回到原电平 5 s 后再看 30 s：不再降上限（反馈步数不变），自动 Limit Gain 还在（没被磨到 0）。
        // 响段里保护层退到下限进「方案受限」、上游 Glue 超出触发的反馈是自动部分的超限，照常算。executedOnAutomatic：实际执行的
        // 保护偏移也要 < 0——它只能收回此刻还在的自动部分（增量 + 校准偏差，设计决定 4），校准已把自动增量收回大半时（手动 Glue 100：
        // Glue 补偿本身就推响，自动 Gain 本来只有 1 dB 上下）只核对保护层自己的 g_保护（任务书「或探针等价量」）。
        void checkGuardActsOnAutomaticPart (const Params& p, juce::int64 seed, float peak, float burst, bool executedOnAutomatic,
                                            const juce::String& what)
        {
            constexpr double sr = 48000.0;
            LiveRig hot (makeMusicLikeSignal (2, sr, 8.0, seed, peak), p, sr, false);
            hot.run (12.0);
            const auto learned = hot.probe();

            float deepestExecuted = 0.0f, deepestGuard = 0.0f;
            hot.sourceGain = burst;
            hot.run (3.0, [&]
            {
                const auto probe = hot.probe();
                deepestExecuted = juce::jmin (deepestExecuted, probe.executedGuardDb);
                deepestGuard = juce::jmin (deepestGuard, probe.guardDb);
            });
            hot.sourceGain = 1.0f;
            hot.run (5.0);
            const auto afterBurst = hot.probe();

            float lowestAutomaticGain = afterBurst.increments.limitGainDb;
            hot.run (30.0, [&] { lowestAutomaticGain = juce::jmin (lowestAutomaticGain, hot.probe().increments.limitGainDb); });
            const auto settled = hot.probe();

            logMessage ("  " + what + ", louder x" + juce::String (burst, 1) + " for 3 s: deepest guard " + juce::String (deepestGuard, 2)
                        + " dB (executed " + juce::String (deepestExecuted, 2) + " dB; automatic Limit Gain " + juce::String (learned.increments.limitGainDb, 2)
                        + " dB, calibration offset " + juce::String (learned.executedBiasDb, 2) + " dB before the burst), calibration rounds before the burst "
                        + juce::String (learned.calibrationBegins) + ", feedback steps " + juce::String (learned.feedbackSteps) + " -> "
                        + juce::String (afterBurst.feedbackSteps) + " -> " + juce::String (settled.feedbackSteps) + " (upstream / guard-limit / budget / stuck "
                        + juce::String (settled.feedbackFromUpstream) + " / " + juce::String (settled.feedbackFromGuardLimit) + " / "
                        + juce::String (settled.feedbackFromBudget) + " / " + juce::String (settled.feedbackFromStuckGuard) + "), cap "
                        + juce::String (afterBurst.feedbackMaxGainDb, 2) + " -> " + juce::String (settled.feedbackMaxGainDb, 2)
                        + " dB, automatic Limit Gain lowest in the 30 s " + juce::String (lowestAutomaticGain, 2) + " dB");
            expect (learned.calibrationBegins >= 1, what + ": calibration ran on the automatic part before the burst (no round ever began before)");
            expect (deepestGuard < -0.1f, what + ": the guard retreats on a loud section (it was switched off whole before)");

            if (executedOnAutomatic)
                expect (deepestExecuted < -0.1f, what + ": and takes back automatic gain for it");

            expectEquals (settled.feedbackSteps, afterBurst.feedbackSteps, what + ": no further cap step in 30 s once the music is back to the learned level");
            expect (sameBits (settled.feedbackMaxGainDb, afterBurst.feedbackMaxGainDb), what + ": the cap holds for those 30 s");
            expect (lowestAutomaticGain > 0.5f, what + ": the automatic Limit Gain is not drained");
        }

        void testManualHeatOverBudget()
        {
            beginTest ("manual Heat over the style's budget: guard, calibration and feedback work on the automatic part only, mismatch still judged, no drain (T-032 R6-02)");

            constexpr double sr = 48000.0;
            Params p;
            p.limit.ceilingDb = -1.0f;
            p.glazePercent = 100.0f;
            p.glazeStyle = 3;         // 通透：Heat 驱动上限 20
            p.color.heat = 25.0f;
            LiveRig rig (makeMusicLikeSignal (2, sr, 8.0, 3201, 0.25f), p, sr);
            rig.run (6.0);            // 聆听 2 ～ 3 s → 提交 → 过渡 0.5 s

            const auto learned = rig.status();
            expect (learned.profileReady, "the profile is committed");
            expect (learned.limitedByManual, "status: limited by the manual Heat (UI hint)");
            const auto capAfterLearning = rig.probe().feedbackMaxGainDb;
            const auto gainAfterLearning = rig.chain.getEffectiveParams().limit.gainDb;
            const auto drainableAfterLearning = drainableGainDb (rig);
            expect (gainAfterLearning > 1.0f, "the automatic Limit Gain still pushes: " + juce::String (gainAfterLearning, 2) + " dB");

            // R6-02 收尾：保护层在跑，只放开 Heat 这一项（它的驱动量全是手动的），其余项照通透的风格上限；校准门的第 16 位只看「电平类
            // 自动全部撤回」（修前看界面提示，整轮不开）。
            const auto running = rig.probe();
            expect (running.guardLimitsSet, "the guard runs while only the manual Heat is over budget (it was switched off whole before)");
            expect (onlyReleased (running.guardLimits, true, false), "only the Heat drive item is released; every other item keeps the clean cap");
            expectEquals (running.calibrationGateMask & 16, 0, "calibration gate bit 16 is 'all automatic level withdrawn', not the UI hint");

            float lowestDrainable = drainableAfterLearning, deepestGuard = 0.0f, lowestBias = 0.0f, highestBias = 0.0f;
            rig.run (30.0, [&]
            {
                const auto probe = rig.probe();
                lowestDrainable = juce::jmin (lowestDrainable, drainableGainDb (rig));
                deepestGuard = juce::jmin (deepestGuard, probe.executedGuardDb);
                lowestBias = juce::jmin (lowestBias, probe.executedBiasDb);
                highestBias = juce::jmax (highestBias, probe.executedBiasDb);
            });

            const auto held = rig.probe();
            logMessage ("  manual Heat 25 (clean cap 20), 30 s: Limit Gain less calibration " + juce::String (drainableAfterLearning, 2) + " -> lowest "
                        + juce::String (lowestDrainable, 2) + " dB (calibration offset " + juce::String (lowestBias, 2) + " .. " + juce::String (highestBias, 2)
                        + " dB), cap " + juce::String (capAfterLearning, 2) + " -> " + juce::String (held.feedbackMaxGainDb, 2)
                        + ", feedback upstream / guard-limit / stuck " + juce::String (held.feedbackFromUpstream) + " / "
                        + juce::String (held.feedbackFromGuardLimit) + " / " + juce::String (held.feedbackFromStuckGuard)
                        + ", deepest executed guard " + juce::String (deepestGuard, 2) + " dB, calibration rounds " + juce::String (held.calibrationBegins));
            expectEquals (held.feedbackFromUpstream, 0, "the manual Heat excess is not an automatic upstream overshoot");
            expectEquals (held.feedbackSteps, 0, "no cap step at all in 30 s at the learned level");
            expect (held.feedbackMaxGainDb == capAfterLearning, "the net gain cap is not lowered by the manual Heat");
            expect (lowestDrainable > drainableAfterLearning - 0.5f, "the automatic Limit Gain is not drained");
            expect (held.calibrationBegins >= 1, "calibration runs on the automatic part (no round ever began before)");

            // 画像失配：整体响 12 dB（比画像的 I 高出 10 LU 以上、持续 1 s）→ 按「这一段当画像」重算的净增益作上限。
            const auto mismatchBefore = rig.probe().mismatchSteps;
            rig.sourceGain = 4.0f;
            rig.run (3.0);
            const auto mismatchAfter = rig.probe().mismatchSteps;
            logMessage ("  +12 dB section with manual Heat 25: mismatch steps " + juce::String (mismatchBefore) + " -> " + juce::String (mismatchAfter));
            expect (mismatchAfter > mismatchBefore, "profile mismatch is still judged while only the manual Heat is over budget");

            // 手动 Heat 拧回上限以内：「手动超预算」的项变了，两个窗口清空重判——上游监视窗里装满手动 Heat 25 的驱动量，按通透上限一判
            // 立即满「3 s 窗内累计 ≥ 1 s」，没有清空就会白降 1 dB（IMPL-A7 第 3 节末段）。看 0.9 s（上游监视要满 1 s 的新超出才触发）
            // 与 4 s（整个 3 s 窗换过一遍）。
            rig.sourceGain = 1.0f;
            rig.run (5.0);            // 过完画像失配的过渡与约束反馈的冷却（过渡 0.5 s + 观察 3 s）
            const auto upstreamBeforeExit = rig.probe().feedbackFromUpstream;
            const auto stepsBeforeExit = rig.probe().feedbackSteps;
            const auto capBeforeExit = rig.probe().feedbackMaxGainDb;
            rig.params.color.heat = 15.0f;
            rig.run (0.9);
            expect (! rig.status().limitedByManual, "manual Heat 15 is within the clean budget");
            expectEquals (rig.probe().feedbackFromUpstream, upstreamBeforeExit, "no feedback from a stale window of manual Heat excess");
            expect (rig.probe().feedbackMaxGainDb == capBeforeExit, "the cap does not step when the manual Heat comes back within budget");
            expect (onlyReleased (rig.probe().guardLimits, false, false), "back within budget: every item is checked against the clean cap again");
            rig.run (3.1);
            logMessage ("  manual Heat 25 -> 15: feedback steps " + juce::String (stepsBeforeExit) + " -> " + juce::String (rig.probe().feedbackSteps)
                        + " in 4 s (upstream " + juce::String (upstreamBeforeExit) + " -> " + juce::String (rig.probe().feedbackFromUpstream) + ")");
            expectEquals (rig.probe().feedbackFromUpstream, upstreamBeforeExit, "still no feedback 4 s after the manual Heat came back within budget");
            expectEquals (rig.probe().feedbackSteps, stepsBeforeExit, "no cap step of any kind in those 4 s");
            expect (rig.probe().feedbackMaxGainDb == capBeforeExit, "the cap still has not stepped 4 s later");

            checkGuardActsOnAutomaticPart (p, 3204, 0.25f, 8.0f, true, "manual Heat 25");
        }

        //    b. 通透、GLAZE 100、手动 Glue 100（超 Glue GR 上限）：保护层在跑、只放开 Glue 两项（GR 全是手动的，退让 Limit Gain、降方案都减
        //       不了它），校准照常开轮；稳态下保护层不为手动造成的 Glue 超出退让自动 Gain，上游监视不触发、上限不降；手动 Glue 拧回 20
        //       （预算以内）的头 4 s 不因旧窗口里手动造成的大 GR 退让或降上限；比画像响时保护层照常收回自动 Gain、之后不磨光。
        void testManualGlueOverBudget()
        {
            beginTest ("manual Glue over the Glue budget: the guard checks the automatic part only, calibration runs, no protection or cap step for a manual excess (T-032 R6-02)");

            constexpr double sr = 48000.0;
            Params p;
            p.limit.ceilingDb = -1.0f;
            p.glazePercent = 100.0f;
            p.glazeStyle = 3;
            p.color.glue = 100.0f;
            LiveRig rig (makeMusicLikeSignal (2, sr, 8.0, 3202, 0.9f), p, sr);
            rig.run (6.0);

            expect (rig.status().profileReady && rig.status().limitedByManual, "status: limited by the manual Glue (UI hint)");
            const auto cap = rig.probe().feedbackMaxGainDb;
            const auto gainAfterLearning = rig.chain.getEffectiveParams().limit.gainDb;
            const auto drainableAfterLearning = drainableGainDb (rig);
            expect (gainAfterLearning > 0.1f, "the automatic Limit Gain still pushes: " + juce::String (gainAfterLearning, 2) + " dB");

            const auto running = rig.probe();
            expect (running.guardLimitsSet, "the guard runs while only the manual Glue is over budget (it was switched off whole before)");
            expect (onlyReleased (running.guardLimits, false, true), "only the two Glue items are released; every other item keeps the clean cap");

            float deepestGuard = 0.0f, lowestDrainable = drainableAfterLearning;
            rig.run (30.0, [&]
            {
                deepestGuard = juce::jmin (deepestGuard, rig.probe().executedGuardDb);
                lowestDrainable = juce::jmin (lowestDrainable, drainableGainDb (rig));
            });

            const auto probe = rig.probe();
            logMessage ("  manual Glue 100 (clean), 30 s: deepest executed guard " + juce::String (deepestGuard, 2) + " dB, cap "
                        + juce::String (cap, 2) + " -> " + juce::String (probe.feedbackMaxGainDb, 2) + ", Limit Gain less calibration "
                        + juce::String (drainableAfterLearning, 2) + " -> lowest " + juce::String (lowestDrainable, 2) + " dB, feedback upstream / guard-limit "
                        + juce::String (probe.feedbackFromUpstream) + " / " + juce::String (probe.feedbackFromGuardLimit) + ", calibration rounds "
                        + juce::String (probe.calibrationBegins));
            expectEquals (deepestGuard, 0.0f, "the guard does not retreat the automatic gain for a manual Glue excess");
            expectEquals (probe.feedbackSteps, 0, "no cap step at all in 30 s");
            expect (probe.feedbackMaxGainDb == cap, "the net gain cap is not lowered by the manual Glue");
            expectEquals (probe.feedbackFromUpstream, 0, "the manual Glue excess is not an automatic upstream overshoot");
            expect (lowestDrainable > drainableAfterLearning - 0.05f, "the automatic Limit Gain is not drained");
            expect (probe.calibrationBegins >= 1, "calibration runs on the automatic part (no round ever began before)");

            // 手动 Glue 拧回 20：预算以内，两项重新按通透的风格上限核对；两个窗口清空重判，旧窗口里手动 Glue 100 的大 GR 不许让保护层
            // 退让、也不许让上游监视降上限。
            rig.params.color.glue = 20.0f;
            float exitGuard = 0.0f;
            rig.run (4.0, [&] { exitGuard = juce::jmin (exitGuard, rig.probe().executedGuardDb); });
            const auto exited = rig.probe();
            logMessage ("  manual Glue 100 -> 20: deepest executed guard in 4 s " + juce::String (exitGuard, 2) + " dB, cap "
                        + juce::String (probe.feedbackMaxGainDb, 2) + " -> " + juce::String (exited.feedbackMaxGainDb, 2) + ", feedback steps "
                        + juce::String (probe.feedbackSteps) + " -> " + juce::String (exited.feedbackSteps));
            expect (! rig.status().limitedByManual, "manual Glue 20 is within the clean budget");
            expect (onlyReleased (exited.guardLimits, false, false), "back within budget: the Glue items are checked against the clean cap again");
            expectEquals (exitGuard, 0.0f, "no retreat from a stale window of manual Glue reduction");
            expect (exited.feedbackMaxGainDb == probe.feedbackMaxGainDb && exited.feedbackSteps == probe.feedbackSteps,
                    "no cap step from a stale window of manual Glue reduction");

            checkGuardActsOnAutomaticPart (p, 3205, 0.5f, 8.0f, false, "manual Glue 100");
        }

        //    c. 通透、GLAZE 100、手动 Clip 100 + 手动 Gain 5（手动单独的削波深度超通透 1.5 dB 的 Clip 预算，限幅器没超——软削波已把峰值
        //       收住）：决策器撤回自动 Clip、只按限幅器核对自动 Gain（F-02 逐模块钳住），自动 Gain 推深的削波是方案许可的；保护层在跑、
        //       只放开 Clip 两项（与决策器一致；按风格上限、或按「max(风格上限, 手动单独深度)」核对都会把自动 Gain 推深的那一点当超限，
        //       退到底、约束反馈把自动增益磨光，IMPL-A11 第 4 节实测），校准照常开轮；30 s 不降上限、自动 Gain 不被磨掉。
        void testManualClipOverBudget()
        {
            beginTest ("manual Clip over the Clip budget: the guard leaves the clip items to the planner, checks the limiter, no drain (T-032 R6-02)");

            constexpr double sr = 48000.0;
            Params p;
            p.limit.ceilingDb = -1.0f;
            p.glazePercent = 100.0f;
            p.glazeStyle = 3;
            p.limit.clip = 100.0f;
            p.limit.gainDb = 5.0f;
            LiveRig rig (makeMusicLikeSignal (2, sr, 8.0, 3206, 0.7f), p, sr);
            rig.run (6.0);

            const auto learned = rig.probe();
            expect (rig.status().profileReady && rig.status().limitedByManual, "status: limited by the manual Clip (UI hint)");
            expect (! learned.targetPlan.automaticLevelWithdrawn && learned.increments.limitGainDb > 0.1f,
                    "only the Clip item is withdrawn: the automatic Limit Gain still pushes " + juce::String (learned.increments.limitGainDb, 2) + " dB");
            expect (learned.guardLimitsSet, "the guard runs while only the manual Clip is over budget (it was switched off whole before)");
            expect (onlyReleased (learned.guardLimits, false, false, true), "only the two Clip items are released; the limiter items keep the clean cap");

            const auto cap = learned.feedbackMaxGainDb;
            const auto drainableAfterLearning = drainableGainDb (rig);
            float deepestGuard = 0.0f, lowestDrainable = drainableAfterLearning;
            rig.run (30.0, [&]
            {
                deepestGuard = juce::jmin (deepestGuard, rig.probe().executedGuardDb);
                lowestDrainable = juce::jmin (lowestDrainable, drainableGainDb (rig));
            });

            const auto probe = rig.probe();
            logMessage ("  manual Clip 100 + Gain 5 (clean), 30 s: automatic Limit Gain " + juce::String (learned.increments.limitGainDb, 2)
                        + " dB, deepest executed guard " + juce::String (deepestGuard, 2) + " dB, cap " + juce::String (cap, 2) + " -> "
                        + juce::String (probe.feedbackMaxGainDb, 2) + ", Limit Gain less calibration " + juce::String (drainableAfterLearning, 2)
                        + " -> lowest " + juce::String (lowestDrainable, 2) + " dB, feedback guard-limit / upstream / budget / stuck "
                        + juce::String (probe.feedbackFromGuardLimit) + " / " + juce::String (probe.feedbackFromUpstream) + " / "
                        + juce::String (probe.feedbackFromBudget) + " / " + juce::String (probe.feedbackFromStuckGuard) + ", calibration rounds "
                        + juce::String (probe.calibrationBegins));
            expectEquals (probe.feedbackSteps, 0, "no cap step in 30 s: the manual clipping is not an automatic overshoot");
            expect (probe.feedbackMaxGainDb == cap, "the net gain cap is not lowered by the manual Clip");
            expect (lowestDrainable > drainableAfterLearning - 0.5f, "the automatic Limit Gain is not drained");
            expect (probe.calibrationBegins >= 1, "calibration runs on the automatic part (no round ever began before)");
        }

        //    d. 手动 Gain 20（手动已让限幅器远超约束 → 电平类自动全部撤回）：不施加校准偏差与保护偏移、不开校准轮，画像失配也不判
        //       （没有自动加上去的增益可压），实际 Limit Gain 就是手动值；保护层与上游监视不运行。
        void testAutomaticLevelWithdrawn()
        {
            beginTest ("manual Gain already over the limiter budget: all automatic level withdrawn, no bias, protection, calibration or mismatch");

            constexpr double sr = 48000.0;
            Params p;
            p.limit.ceilingDb = -1.0f;
            p.glazePercent = 100.0f;
            p.limit.gainDb = 20.0f;
            LiveRig rig (makeMusicLikeSignal (2, sr, 8.0, 3203, 0.9f), p, sr);
            rig.run (6.0);
            expect (rig.status().profileReady && rig.status().limitedByManual);

            rig.sourceGain = 4.0f;    // 响 12 dB 的一段：自动全撤时画像失配不判
            rig.run (3.0);
            rig.sourceGain = 1.0f;
            rig.run (3.0);

            const auto probe = rig.probe();
            expectEquals (probe.executedBiasDb, 0.0f, "no calibration offset");
            expectEquals (probe.executedGuardDb, 0.0f, "no protection offset");
            expectEquals (probe.calibrationBegins, 0, "nothing automatic to calibrate");
            expectEquals (probe.mismatchSteps, 0, "no automatic gain to cap: mismatch is not judged");
            expectEquals (rig.chain.getEffectiveParams().limit.gainDb, 20.0f, "Limit Gain is exactly the manual value");
            expect (! probe.guardLimitsSet, "the guard never runs: there is no automatic gain to take back");
            expectEquals (probe.calibrationGateMask & 16, 16, "calibration gate bit 16: all automatic level withdrawn");
        }
    };

    static GlazeChainTests glazeChainTests;
}

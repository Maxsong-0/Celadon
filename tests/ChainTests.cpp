/*
  ==============================================================================

    ChainTests.cpp
    CeladonChain（Balance → Color → Limit）的自动测试：延迟相加与脉冲实测、全中性 null、宿主旁通逐位、
    restartWithFadeIn 的静音 / 淡入 / 回到逐位、整条链的真峰值仍不超 Ceiling、表的转发、
    音频路径（setParams / process / restartWithFadeIn / 表读取）不经 operator new 分配。

    只按设计规格-总体与 CeladonChain.h 的公开接口写，不依赖实现细节。

    v0.1.7.0 起链为 Balance → Clay → Color → Limit（ADR-009）：文件末尾追加测试类 CeladonChainClay——修坯 Clay 的链级接线
    （顺序、GLAZE 0 逐位直通、三态在 GLAZE 0 的接线、宿主旁通、延迟、重置、Guard 切片路径、音频路径不分配）。
    阶段 4（ADR-012 线性相位收单）起 Clay 有固定延迟，ADR-014 峰值守护起为 D + G（48 kHz 574 + 528 = 1102 样本；下文 CeladonChainClay
    里的「D」都指 ClayModule 上报的整个延迟 D + G，与 ClayTests.cpp 同口径）：上面的用例都按链自己上报的 getLatencySamples() 对齐，
    只有「延迟 = 各模块之和」把 Clay 加进了求和；CeladonChainClay 的逐位对照改成「旧链在 Clay 的位置插一段 D 样本纯延迟」，
    延迟断言改成 282 + D（48 / 96 kHz 为 1384 / 2584）。

  ==============================================================================
*/

#include "TestHelpers.h"

#include "../Source/dsp/CeladonChain.h"
#include "../Source/dsp/ClayGuard.h"

#include <cstring>

namespace celadon::test
{
    namespace
    {
        constexpr int numChannels = 2;
        constexpr int maxBlock = 512;

        // −120 dBFS 的线性值就是 1e-6。
        constexpr float nullThreshold = 1.0e-6f;

        juce::dsp::ProcessSpec makeSpec (double sampleRate)
        {
            return { sampleRate, (juce::uint32) maxBlock, (juce::uint32) numChannels };
        }

        // 全中性：Color 默认值本身就是中性（Tone 0、Glue 0、Heat 0、Mix 100、Output 0）；
        // Limit Gain 0、Ceiling −1 dBTP、Clip 0、Auto 释放，Lookahead 可指定。
        CeladonChain::Params makeNeutralParams (float lookaheadMs = 2.0f)
        {
            CeladonChain::Params p;
            p.color = ColorModule::Params {};
            p.limit = LimiterModule::Params {};
            p.limit.gainDb = 0.0f;
            p.limit.ceilingDb = -1.0f;
            p.limit.releaseMs = 100.0f;
            p.limit.autoRelease = true;
            p.limit.lookaheadMs = lookaheadMs;
            p.limit.clip = 0.0f;
            p.limit.listen = false;
            p.limit.bypass = false;
            return p;
        }

        // 先 setParams 再 prepare（prepare → reset 把平滑器直接落到目标），prepare 后再 set 一次作保险
        // （目标相同不会起斜坡；万一实现的 prepare 把参数恢复成默认值，这一步能纠正回来）。
        void prepareChain (CeladonChain& chain, double sampleRate, const CeladonChain::Params& p, bool hostBypassed = false)
        {
            chain.setParams (p, hostBypassed);
            chain.prepare (makeSpec (sampleRate));
            chain.setParams (p, hostBypassed);
        }

        // 按指定块大小把整段信号送进整条链，返回输出。
        Signal runChain (CeladonChain& chain, const Signal& input, int blockSize)
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

                chain.process (block);

                for (int ch = 0; ch < numChannels; ++ch)
                    std::copy (block.getReadPointer (ch), block.getReadPointer (ch) + length,
                               output[(size_t) ch].begin() + start);
            }

            return output;
        }

        // 多个频率叠加 + 噪声，真峰值约 −4 dBTP，低于 −1 dBTP 的 Ceiling（与 LimiterTests 的 null 信号相同）。
        Signal makeBelowCeilingSignal (double sampleRate)
        {
            auto input = makeNoise (numChannels, (int) (0.5 * sampleRate), 0.15f, 42);

            for (auto frequency : { 60.0, 1000.0, 9000.0, 18000.0 })
            {
                const auto tone = makeSine (numChannels, sampleRate, frequency, 0.1f, 0.5);

                for (size_t ch = 0; ch < input.size(); ++ch)
                    for (size_t i = 0; i < input[ch].size(); ++i)
                        input[ch][i] += tone[ch][i];
            }

            return input;
        }

        int peakIndexOf (const std::vector<float>& channel)
        {
            int peakIndex = 0;

            for (int i = 0; i < (int) channel.size(); ++i)
                if (std::abs (channel[(size_t) i]) > std::abs (channel[(size_t) peakIndex]))
                    peakIndex = i;

            return peakIndex;
        }

        struct NamedSignal
        {
            const char* name;
            Signal signal;
        };

        std::vector<NamedSignal> makeLoudSignals (double sampleRate)
        {
            std::vector<NamedSignal> signals;
            signals.push_back ({ "1 kHz sine 0 dBFS", makeSine (numChannels, sampleRate, 1000.0, 1.0f, 0.5) });
            signals.push_back ({ "100 Hz square",     makeSquare (numChannels, sampleRate, 100.0, 1.0f, 0.5) });
            signals.push_back ({ "white noise 0 dBFS", makeNoise (numChannels, (int) (0.5 * sampleRate), 1.0f, 5) });
            signals.push_back ({ "20 ms burst",        makeBurst (numChannels, sampleRate, 1.0f, 0.3) });
            return signals;
        }

        // 监听验收独立测最终实际样本的最后 1 秒，不读取链的补偿前 OUT 表。
        float tailLufs (const Signal& signal, double sampleRate)
        {
            LoudnessMeter meter;
            meter.prepare (makeSpec (sampleRate));
            juce::AudioBuffer<float> block (numChannels, maxBlock);
            const auto samples = (int) signal.front().size();
            for (int pos = juce::jmax (0, samples - (int) sampleRate); pos < samples; pos += maxBlock)
            {
                const auto count = juce::jmin (maxBlock, samples - pos);
                block.setSize (numChannels, count, false, false, true);
                for (int ch = 0; ch < numChannels; ++ch)
                    block.copyFrom (ch, 0, signal[(size_t) ch].data() + pos, count);
                meter.process (block);
            }
            return meter.getIntegratedLufs();
        }

        Signal makeListenMaterial (double sampleRate, double seconds)
        {
            auto input = makeSine (numChannels, sampleRate, 880.0, 0.22f, seconds);
            for (const auto component : { std::pair<double, float> { 55.0, 0.12f }, { 4600.0, 0.04f } })
            {
                const auto tone = makeSine (numChannels, sampleRate, component.first, component.second, seconds);
                for (size_t ch = 0; ch < input.size(); ++ch)
                    for (size_t i = 0; i < input[ch].size(); ++i)
                        input[ch][i] += tone[ch][i];
            }
            return input;
        }
    }

    class ChainTests  : public juce::UnitTest
    {
    public:
        ChainTests() : juce::UnitTest ("CeladonChain", "Celadon") {}

        void runTest() override
        {
            testLatencyIsSumOfModules();
            testNullWhenNeutral();
            testHostBypassIsBitExact();
            testRestartWithFadeIn();
            testTruePeakStillBelowCeiling();
            testAudioPathDoesNotAllocate();
            testResetEqualsFreshPrepare();
            testListenMatchesWholeChain();
            testListenBlockSizeAndAttenuationOnly();
            testListenSparseTransients();
            testListenSilenceAutomationAndBypass();
            testFrozenGuardDoesNotFragmentDsp();
        }

    private:
        void testFrozenGuardDoesNotFragmentDsp()
        {
            beginTest ("freezing a Guard slot one sample before its boundary cannot fragment large callbacks into single samples");
            for (int mode = 0; mode < 3; ++mode)
            {
                CeladonChain chain;
                auto p = makeNeutralParams();
                p.glazePercent = 35.0f;
                chain.setParams (p, false);
                chain.prepare ({ 48000.0, 2048, 2 });
                const auto input = makeSine (2, 48000.0, 997.0, 0.1f, 2.0);
                runChain (chain, input, 480);
                juce::AudioBuffer<float> block (2, 2048);
                const auto partial = chain.getGlazeController().getGuardSamplesUntilBoundary() - 1;
                if (partial > 0)
                {
                    block.setSize (2, partial, false, false, true);
                    for (int ch = 0; ch < 2; ++ch)
                        for (int i = 0; i < partial; ++i) block.setSample (ch, i, 0.1f);
                    chain.setParams (p, false);
                    chain.process (block);
                }
                expectEquals (chain.getGlazeController().getGuardSamplesUntilBoundary(), 1,
                              "fixture must begin with an actual one-sample active slot remainder");
                if (mode == 1) p.color.bypass = true;
                if (mode == 2) p.limit.bypass = true;
                chain.setParams (p, mode == 0);
                block.setSize (2, 2048, false, false, true);
                block.clear();
                chain.process (block);
                expect (chain.getLastProcessSliceCountForTesting() <= 7,
                        "frozen detector must keep bounded work per callback, mode=" + juce::String (mode));
                expectEquals (chain.getGlazeController().getGuardSamplesUntilBoundary(), 480);
            }
        }

        void testListenMatchesWholeChain()
        {
            beginTest ("Listen measures whole-chain gain, Color and limiting; OUT stays before monitoring gain");
            for (const auto sampleRate : { 44100.0, 48000.0 })
            {
                for (int mode = 0; mode < 3; ++mode)
                {
                    auto params = makeNeutralParams();
                    params.limit.listen = true;
                    if (mode == 0) params.limit.gainDb = 18.0f;
                    if (mode == 1) params.color.outputDb = 6.0f;
                    if (mode == 2)
                    {
                        params.color.toneDb = 3.0f;
                        params.color.heat = 100.0f;
                        params.color.glue = 35.0f;
                        params.color.outputDb = 3.0f;
                        params.limit.gainDb = 12.0f;
                        params.limit.clip = 30.0f;
                    }
                    const auto input = mode == 2 ? makeListenMaterial (sampleRate, 4.0)
                                                 : makeSine (numChannels, sampleRate, 1000.0, 0.03f, 3.0);
                    CeladonChain chain;
                    prepareChain (chain, sampleRate, params);
                    const auto output = runChain (chain, input, 333);
                    const auto delta = tailLufs (output, sampleRate) - tailLufs (input, sampleRate);
                    const auto tag = "mode=" + juce::String (mode) + " @" + juce::String (sampleRate, 0);
                    expect (allFinite (output), tag);
                    expect (std::abs (delta) <= (mode == 2 ? 0.3f : 0.1f), "audible LU difference " + juce::String (delta, 3) + " " + tag);
                    expect (chain.getOutputShortTermLufs() > tailLufs (output, sampleRate) + 3.0f,
                            "OUT must measure the louder master before Listen " + tag);
                    if (mode == 2)
                        expect (chain.getAndResetLimitGainReductionDb() > 1.0f, "case must exercise actual limiting");
                    logMessage ("  measured Listen error " + juce::String (delta, 3) + " LU " + tag);
                }
            }
        }

        void testListenBlockSizeAndAttenuationOnly()
        {
            beginTest ("Listen is independent of host block partition and never boosts a quieter master");
            constexpr double sampleRate = 48000.0;
            auto params = makeNeutralParams();
            params.color.outputDb = 6.0f;
            params.limit.listen = true;
            const auto input = makeSine (numChannels, sampleRate, 1000.0, 0.08f, 3.0);
            CeladonChain small, oversized;
            prepareChain (small, sampleRate, params);
            prepareChain (oversized, sampleRate, params);
            const auto a = runChain (small, input, 17);
            const auto b = runChain (oversized, input, 2048); // prepare maxBlock is only 512
            expect (maxAbsDifference (a, b, 0, (int) input[0].size()) < 1.0e-6f,
                    "100 ms estimate events must occur at the same sample within different host blocks");

            params.color.outputDb = -6.0f;
            CeladonChain quieter, normal;
            prepareChain (quieter, sampleRate, params);
            params.limit.listen = false;
            prepareChain (normal, sampleRate, params);
            const auto monitored = runChain (quieter, input, maxBlock);
            const auto master = runChain (normal, input, maxBlock);
            expectEquals (maxAbsDifference (monitored, master, 0, (int) input[0].size()), 0.0f,
                          "quieter output is not boosted: preserve headroom instead of claiming exact matching");
            expect (tailLufs (monitored, sampleRate) < tailLufs (input, sampleRate) - 5.9f);
        }

        void testListenSparseTransients()
        {
            beginTest ("neutral Listen must not attenuate isolated impulses that cross a 100 ms window boundary");
            constexpr double sampleRate = 48000.0;
            auto params = makeNeutralParams();
            params.limit.listen = true;
            auto input = makeSilence (numChannels, (int) (4.5 * sampleRate));
            for (const auto seconds : { 0.5, 1.8, 3.1, 4.4 })
                for (auto& channel : input)
                    channel[(size_t) (seconds * sampleRate) - 2] = 0.5f;
            for (const auto block : { 64, 2048 })
            {
                CeladonChain chain;
                prepareChain (chain, sampleRate, params);
                const auto output = runChain (chain, input, block);
                const auto residual = maxAbsDifference (input, output, chain.getLatencySamples(),
                                                       (int) input[0].size() - chain.getLatencySamples());
                expect (residual < 1.0e-5f, "neutral sparse pulse changed by " + juce::String (residual, 6)
                                          + " block=" + juce::String (block));
            }
        }

        void testListenSilenceAutomationAndBypass()
        {
            beginTest ("Listen freezes in silence, follows edits, ramps toggles, and recovers from bypass/reset/latency changes");
            constexpr double sampleRate = 48000.0;
            auto params = makeNeutralParams();
            params.color.outputDb = 6.0f;
            params.limit.gainDb = 6.0f;
            params.limit.listen = true;
            CeladonChain chain;
            prepareChain (chain, sampleRate, params);
            const auto tone = makeSine (numChannels, sampleRate, 1000.0, 0.05f, 3.0);
            runChain (chain, tone, maxBlock);
            const auto silence = runChain (chain, makeSilence (numChannels, (int) sampleRate), 333);
            float silentTail = 0.0f;
            for (const auto& channel : silence)
                for (size_t i = channel.size() / 2; i < channel.size(); ++i)
                    silentTail = juce::jmax (silentTail, std::abs (channel[i]));
            expectEquals (silentTail, 0.0f, "silence must not produce a gain/noise burst");
            const auto onsetInput = makeSine (numChannels, sampleRate, 1000.0, 0.05f, 0.2);
            const auto onset = runChain (chain, onsetInput, 333);
            expect (maxAbsDifference (onsetInput, onset, chain.getLatencySamples(), 4000) < 0.001f,
                    "the learned gain must survive silence, including the next onset");

            params.color.outputDb = 0.0f; // ongoing Listen must relearn while the user edits the master
            chain.setParams (params, false);
            auto changed = runChain (chain, tone, maxBlock);
            const auto editedDelta = tailLufs (changed, sampleRate) - tailLufs (tone, sampleRate);
            logMessage ("  Listen after 6 dB output edit, final 1 s of 3 s: " + juce::String (editedDelta, 4) + " LU");
            expect (std::abs (editedDelta) < 0.15f,
                    "Listen must follow a gain edit without feeding its own attenuation into the estimate; error="
                    + juce::String (editedDelta, 4) + " LU");

            // 对照同一时刻的未补偿母带，量出增益斜坡；正弦过零处不相除。
            auto masterParams = params;
            masterParams.limit.listen = false;
            CeladonChain reference;
            prepareChain (reference, sampleRate, masterParams);
            runChain (reference, tone, maxBlock);
            const auto shortTone = makeSine (numChannels, sampleRate, 1000.0, 0.05f, 0.05);
            float previousGain = juce::Decibels::decibelsToGain (-6.0f);
            for (int toggle = 0; toggle < 4; ++toggle)
            {
                params.limit.listen = toggle % 2 == 1;
                chain.setParams (params, false);
                const auto listening = runChain (chain, shortTone, 64);
                const auto unmonitored = runChain (reference, shortTone, 64);
                float maxGainStep = 0.0f;
                for (size_t i = 0; i < listening[0].size(); ++i)
                    if (std::abs (unmonitored[0][i]) > 0.03f)
                    {
                        const auto gain = listening[0][i] / unmonitored[0][i];
                        maxGainStep = juce::jmax (maxGainStep, std::abs (gain - previousGain));
                        previousGain = gain;
                    }
                expect (maxGainStep < 0.01f, "Listen switch must ramp, max gain jump=" + juce::String (maxGainStep, 5));
            }

            chain.setParams (params, true);
            const auto bypassed = runChain (chain, tone, 333);
            const auto latency = chain.getLatencySamples();
            float bypassError = 0.0f;
            for (size_t i = (size_t) (sampleRate / 4); i < bypassed[0].size(); ++i)
                bypassError = juce::jmax (bypassError, std::abs (bypassed[0][i] - tone[0][i - (size_t) latency]));
            expectEquals (bypassError, 0.0f, "host bypass must return the unattenuated delayed input");
            chain.setParams (params, false);
            changed = runChain (chain, tone, maxBlock);
            expect (std::abs (tailLufs (changed, sampleRate) - tailLufs (tone, sampleRate)) < 0.15f, "resume after host bypass");
            params.limit.lookaheadMs = 7.0f;
            chain.setParams (params, false);
            chain.restartWithFadeIn();
            changed = runChain (chain, tone, maxBlock);
            expect (allFinite (changed));
            expect (std::abs (tailLufs (changed, sampleRate) - tailLufs (tone, sampleRate)) < 0.15f, "reset and latency change must relearn safely");

            // 一个块跨越 128 个 K 子块，确实覆盖读环；保持旧增益并丢弃旧配对，随后能正常学习。
            const auto hugeSilence = makeSilence (numChannels, (int) (13.0 * sampleRate));
            const auto hugeOutput = runChain (chain, hugeSilence, (int) hugeSilence[0].size());
            expect (allFinite (hugeOutput), "oversized block/ring loss must be safe");
            changed = runChain (chain, tone, 2048);
            expect (std::abs (tailLufs (changed, sampleRate) - tailLufs (tone, sampleRate)) < 0.15f, "recovery after lost metering history");
        }

        // 1. getLatencySamples() = Balance 延迟（0）+ Clay 延迟（固定 D + G，v0.1.7.0 阶段 4 / ADR-014）+ Color 延迟 + Limit 延迟（用同样
        //    参数单独建四个模块对照），脉冲实测峰值位置 = position + latency（Clay 中性 = 逐位纯延迟 D + G、Color 中性、Limit Gain 0：
        //    −6 dBFS 脉冲的真峰值就是它自己，不触发限幅，输出只是整数延迟的输入），三个采样率 × 三档 Lookahead；Lookahead 改动后
        //    setParams 立即反映在延迟上。
        // 依据：设计规格-总体「延迟」「总延迟 = 各模块之和；Lookahead 改变时立即重新上报」；设计规格-Clay-根治修订 第 2 节。
        void testLatencyIsSumOfModules()
        {
            beginTest ("latency == balance + clay + color + limit latency, impulse confirms it");

            for (auto sampleRate : { 44100.0, 48000.0, 96000.0 })
             for (auto lookaheadMs : { 1.0f, 2.0f, 10.0f })
            {
                const auto params = makeNeutralParams (lookaheadMs);

                CeladonChain chain;
                prepareChain (chain, sampleRate, params);

                BalanceModule balance;
                balance.setParams (params.balance);
                balance.prepare (makeSpec (sampleRate));

                ClayModule clay;
                clay.setParams (params.clay);
                clay.prepare (makeSpec (sampleRate));

                ColorModule color;
                color.setParams (params.color);
                color.prepare (makeSpec (sampleRate));

                LimiterModule limiter;
                limiter.setParams (params.limit);
                limiter.prepare (makeSpec (sampleRate));

                const auto latency = chain.getLatencySamples();
                const auto caseName = "@" + juce::String (sampleRate) + " la=" + juce::String (lookaheadMs);

                expectEquals (latency, balance.getLatencySamples() + clay.getLatencySamples() + color.getLatencySamples()
                                           + limiter.getLatencySamples(), caseName);
                expect (clay.getLatencySamples() > 0, "Clay contributes its fixed delay: " + caseName);
                expect (latency > 0, "chain latency must be positive: " + caseName);

                constexpr int position = 100;
                const auto input = makeImpulse (numChannels, position + latency + 256, position, 0.5f);
                const auto output = runChain (chain, input, maxBlock);

                expect (allFinite (output), caseName);
                expectEquals (peakIndexOf (output[0]) - position, latency, caseName);
                expectEquals (peakIndexOf (output[1]) - position, latency, caseName);
            }

            // Lookahead 2 → 7 ms：延迟增加 5 ms 对应的样本数，setParams 之后立即可读。
            {
                constexpr double sampleRate = 48000.0;
                auto params = makeNeutralParams (2.0f);

                CeladonChain chain;
                prepareChain (chain, sampleRate, params);
                const auto before = chain.getLatencySamples();

                params.limit.lookaheadMs = 7.0f;
                chain.setParams (params, false);
                expectEquals (chain.getLatencySamples() - before, juce::roundToInt (0.005 * sampleRate), "lookahead change");
            }
        }

        // 2. 全部中性（Color 中性、Limit Gain 0 Clip 0、输入真峰值低于 Ceiling）→ 与延迟后的输入差 < −120 dBFS。
        // 依据：设计规格-总体「目标 3 透明：任一模块参数归零时输出与输入零差」；Color 规格验收 1 + Limit 规格验收 2 串联。
        void testNullWhenNeutral()
        {
            beginTest ("null when everything is neutral and input is below ceiling");

            for (auto sampleRate : { 44100.0, 48000.0, 96000.0 })
             for (auto lookaheadMs : { 1.0f, 2.0f, 10.0f })
            {
                CeladonChain chain;
                prepareChain (chain, sampleRate, makeNeutralParams (lookaheadMs));

                auto input = makeBelowCeilingSignal (sampleRate);
                expect (truePeakOf (input) < juce::Decibels::decibelsToGain (-1.0f), "test signal must stay below ceiling");

                const auto length = (int) input.front().size();
                const auto latency = chain.getLatencySamples();
                appendSilence (input, latency);
                const auto output = runChain (chain, input, maxBlock);

                expect (allFinite (output));

                const auto diff = maxAbsDifference (input, output, latency, length);
                expect (diff < nullThreshold, "null residual " + juce::String (toDb (diff), 1) + " dBFS @" + juce::String (sampleRate)
                                                + " la=" + juce::String (lookaheadMs));
            }
        }

        // 3. 宿主旁通（hostBypassed = true）→ 输出逐位等于延迟后的输入，各模块参数全部开满、模块自己的 Bypass 都关着、
        //    输入 0 dBFS 噪声（远超 Ceiling）也一样。
        // 依据：CeladonChain.h「hostBypassed = 宿主旁通：强制旁通全部模块（各模块自带延迟补偿与交叉淡化）」；
        // Limit / Color 规格「完全旁通时逐位等于延迟后的输入」。旁通在 prepare 前就设好，交叉淡化从第 0 个样本起就是全干。
        void testHostBypassIsBitExact()
        {
            beginTest ("host bypass is bit-exact delayed input with every module maxed out");

            for (auto sampleRate : { 44100.0, 48000.0 })
            {
                auto params = makeNeutralParams (2.0f);
                params.balance.recover = 100.0f;     // Balance 也开满（2026-09-25 评审 R2-02：之前漏了，链级旁通没覆盖它）
                params.balance.tame = 100.0f;
                params.balance.brighten = 100.0f;
                params.balance.speed = BalanceModule::Speed::fast;
                params.balance.bypass = false;
                params.color.toneDb = 3.0f;
                params.color.glue = 100.0f;
                params.color.heat = 100.0f;
                params.color.mixPercent = 100.0f;
                params.color.outputDb = 6.0f;
                params.color.bypass = false;
                params.limit.gainDb = 24.0f;
                params.limit.ceilingDb = -3.0f;
                params.limit.clip = 100.0f;
                params.limit.bypass = false;

                CeladonChain chain;
                prepareChain (chain, sampleRate, params, true);

                auto input = makeNoise (numChannels, (int) (0.5 * sampleRate), 1.0f, 7);
                const auto length = (int) input.front().size();
                const auto latency = chain.getLatencySamples();
                appendSilence (input, latency);
                const auto output = runChain (chain, input, maxBlock);

                expectEquals (maxAbsDifference (input, output, latency, length), 0.0f, "@" + juce::String (sampleRate));
            }
        }

        // 4. restartWithFadeIn()：之后前 getLatencySamples() 个输出样本全为 0；再过 20 ms 后输出逐位等于延迟后的输入（中性参数）。
        // 依据：CeladonChain.h「清空全部模块，先静音『整条链的延迟』再 10 ms 淡入」。淡入 10 ms，放宽到 20 ms 后再比逐位；
        // 中性参数下 Color 是纯延迟、Limit 增益精确为 1，所以淡入结束后必须逐位相等（同 LimiterTests::testRestartWithFadeIn）。
        void testRestartWithFadeIn()
        {
            beginTest ("restartWithFadeIn: silence for the whole latency, then fade, then bit-exact again");

            constexpr double sampleRate = 48000.0;
            CeladonChain chain;
            prepareChain (chain, sampleRate, makeNeutralParams (2.0f));

            // 先跑一段让延迟线、包络都有内容，重置才有意义。
            runChain (chain, makeSine (numChannels, sampleRate, 440.0, 0.5f, 0.2), maxBlock);

            chain.restartWithFadeIn();
            const auto latency = chain.getLatencySamples();
            const auto input = makeSine (numChannels, sampleRate, 440.0, 0.5f, 0.5);
            const auto output = runChain (chain, input, maxBlock);

            expect (allFinite (output));

            float silentPart = 0.0f;
            for (const auto& channel : output)
                for (int i = 0; i < latency; ++i)
                    silentPart = juce::jmax (silentPart, std::abs (channel[(size_t) i]));

            expectEquals (silentPart, 0.0f, "first latency samples must be silent");

            const auto settled = latency + (int) (0.02 * sampleRate);
            float diff = 0.0f;
            for (size_t ch = 0; ch < output.size(); ++ch)
                for (int i = settled; i < (int) output[ch].size(); ++i)
                    diff = juce::jmax (diff, std::abs (output[ch][(size_t) i] - input[ch][(size_t) (i - latency)]));

            expectEquals (diff, 0.0f, "after the fade the chain is transparent again");

            // 淡入段逐样本步长不超过稳态正弦的步长（2026-09-24 夜间评审 R1-04）：若 restartWithFadeIn 漏掉
            // Color 的延迟，Color 清空后的第一个非零样本会在淡入进行到一半时突然出现，这里就会超。
            auto maxStepIn = [&] (int from, int to)
            {
                float step = 0.0f;
                for (int i = juce::jmax (1, from); i < to; ++i)
                    step = juce::jmax (step, std::abs (output[0][(size_t) i] - output[0][(size_t) (i - 1)]));
                return step;
            };

            const auto steadyStep = maxStepIn (settled, (int) output[0].size());
            const auto fadeStep   = maxStepIn (latency, settled);
            logMessage ("  fade step " + juce::String (fadeStep, 5) + " vs steady " + juce::String (steadyStep, 5));
            expect (fadeStep <= steadyStep * 1.05f, "fade-in must be smooth");
        }

        // 5. Color Heat 50 Glue 50 在前、Limit Gain +12 在后：输出真峰值仍 ≤ Ceiling + 0.01 dB（插件自己的表，ADR-004 口径），
        //    Clip 0 / 50 两档 × Balance 关 / 60·60（Balance 最多先把某些段抬 6 dB 再进 Color 与 Limit；
        //    2026-09-25 评审 R1-04），四种响信号；顺便确认两条 GR 表经链转发（读到正数，再读清零）。
        // 依据：Limit 规格验收 1 对整条链同样成立——Color 只是改变了 Limit 的输入，不能让 Limit 失守。
        void testTruePeakStillBelowCeiling()
        {
            beginTest ("true peak <= ceiling + 0.01 dB with color engaged before the limiter");

            constexpr double sampleRate = 48000.0;
            constexpr float ceilingDb = -1.0f;
            float worstDb = -100.0f;
            juce::String worstCase;

            for (auto balanceAmount : { 0.0f, 60.0f })
             for (auto clip : { 0.0f, 50.0f })
              for (const auto& named : makeLoudSignals (sampleRate))
            {
                auto params = makeNeutralParams (2.0f);
                params.balance.recover = balanceAmount;
                params.balance.tame = balanceAmount;
                params.color.heat = 50.0f;
                params.color.glue = 50.0f;
                params.limit.gainDb = 12.0f;
                params.limit.ceilingDb = ceilingDb;
                params.limit.clip = clip;

                CeladonChain chain;
                prepareChain (chain, sampleRate, params);

                auto input = named.signal;
                appendSilence (input, chain.getLatencySamples() + 64);
                const auto output = runChain (chain, input, maxBlock);

                const auto caseName = juce::String (named.name) + " clip=" + juce::String (clip)
                                    + " balance=" + juce::String (balanceAmount);
                expect (allFinite (output), "non-finite output: " + caseName);

                const auto overshootDb = toDb (truePeakOf (output)) - ceilingDb;
                expect (overshootDb <= 0.01f, "overshoot " + juce::String (overshootDb, 4) + " dB: " + caseName);

                if (overshootDb > worstDb)
                {
                    worstDb = overshootDb;
                    worstCase = caseName;
                }

            }

            logMessage ("  worst overshoot: " + juce::String (worstDb, 4) + " dB  [" + worstCase + "]");

            // 两条 GR 表经链转发（读到正数、再读清零）。用 Heat = 0：Heat 的 Tube 曲线按原点斜率归一，
            // 响信号经 Heat 50 后会低十几 dB，「0 dBFS 推 +12 dB 一定限幅」就不再成立（2026-09-24 集成时发现）。
            {
                auto params = makeNeutralParams (2.0f);
                params.color.glue = 50.0f;           // 阈值 −12 dBFS：0 dBFS 正弦一定在压
                params.limit.gainDb = 12.0f;         // Glue 输出约 −2.4 dBFS，再推 +12 dB 一定限幅
                params.limit.ceilingDb = ceilingDb;

                CeladonChain chain;
                prepareChain (chain, sampleRate, params);
                runChain (chain, makeSine (numChannels, sampleRate, 1000.0, 1.0f, 0.5), maxBlock);

                const auto limitGr = chain.getAndResetLimitGainReductionDb();
                const auto glueGr = chain.getAndResetGlueGainReductionDb();
                expect (limitGr > 0.0f, "limit GR must be reported through the chain");
                expect (glueGr > 0.0f, "glue GR must be reported through the chain");
                expectEquals (chain.getAndResetLimitGainReductionDb(), 0.0f, "limit GR clears after reading");
                expectEquals (chain.getAndResetGlueGainReductionDb(), 0.0f, "glue GR clears after reading");
                logMessage ("  GR through chain: limit " + juce::String (limitGr, 2) + " dB, glue " + juce::String (glueGr, 2) + " dB");
            }
        }

        // 6. 音频线程上会调的全部入口——每块 setParams、process、偶尔 restartWithFadeIn（宿主 Reset / NaN 恢复）——
        //    以及界面线程读表与曲线的三个函数，都不经 operator new 分配（计数见 TestMain.cpp）。
        //    参数每块都在变，并且故意走遍会切换内部路径的地方：Glue / Heat / Clip 过 0（开关交叉淡化）、
        //    Lookahead 换档（限幅器重置、延迟变化）、Auto / Listen / 各 Bypass / 宿主旁通来回切、Speed 三档轮换、
        //    GLAZE 在 0 与扫动之间来回（映射在 setParams 里算，同样不许分配）；
        //    块长也在 512 / 1 / 17 / 256 之间变（块缓冲在计数区外按最大块建好，setSize 不重新分配）。
        // 依据：设计规格-总体「音频线程零分配、零锁」；v0.3 计划 B6「断言无分配路径」（2026-09-25 评审 R2-03）。
        //    直接 std::malloc 的 JUCE HeapBlock 不在计数内（见 TestMain.cpp 说明）。
        void testAudioPathDoesNotAllocate()
        {
            beginTest ("setParams / process / restartWithFadeIn / meter reads never allocate");

            constexpr double sampleRate = 48000.0;
            constexpr int numBlocks = 480;
            constexpr int blockSizes[] = { maxBlock, 1, 17, 256 };

            CeladonChain chain;
            prepareChain (chain, sampleRate, makeNeutralParams (2.0f));

            const auto input = makeNoise (numChannels, numBlocks * maxBlock, 0.7f, 11);
            juce::AudioBuffer<float> block (numChannels, maxBlock);
            BalanceModule::BandGains bandGains {};

            std::uint64_t allocations = 0;
            bool finite = true;
            int readPosition = 0;

            for (int b = 0; b < numBlocks; ++b)
            {
                // ---- 计数区外：准备这一块的参数与输入 ----
                const auto phase = (float) b / (float) numBlocks;                 // 0 → 1
                const auto wave = 0.5f + 0.5f * std::sin (juce::MathConstants<float>::twoPi * 3.0f * phase);

                CeladonChain::Params params;
                params.balance.recover  = 100.0f * wave;
                params.balance.tame     = 100.0f * (1.0f - wave);
                params.balance.brighten = -100.0f + 200.0f * phase;
                params.balance.speed    = static_cast<BalanceModule::Speed> ((b / 50) % 3);
                params.balance.bypass   = (b / 70) % 2 == 1;

                params.color.toneDb     = -3.0f + 6.0f * wave;
                params.color.glue       = (b / 30) % 2 == 0 ? 0.0f : 100.0f * wave;   // 过 0：Glue 开关交叉淡化
                params.color.heat       = (b / 45) % 2 == 0 ? 0.0f : 100.0f * wave;   // 过 0：Heat 开关交叉淡化
                params.color.mixPercent = 100.0f * wave;
                params.color.outputDb   = -6.0f + 12.0f * phase;
                params.color.bypass     = (b / 80) % 2 == 1;

                params.limit.gainDb      = 24.0f * wave;
                params.limit.ceilingDb   = -3.0f * phase;
                params.limit.releaseMs   = 10.0f + 990.0f * phase;
                params.limit.autoRelease = (b / 25) % 2 == 0;
                params.limit.lookaheadMs = 1.0f + 0.5f * (float) ((b / 40) % 19);  // 1 ～ 10 ms 每 0.5 换档
                params.limit.clip        = (b / 35) % 2 == 0 ? 0.0f : 100.0f * wave;
                params.limit.listen      = (b / 60) % 2 == 1;
                params.limit.bypass      = (b / 110) % 2 == 1;

                params.glazePercent      = (b / 55) % 2 == 0 ? 0.0f : 100.0f * wave;     // GLAZE 过 0（v0.4）

                const bool hostBypassed = (b / 90) % 2 == 1;
                const auto length = blockSizes[(size_t) b % std::size (blockSizes)];

                block.setSize (numChannels, length, false, false, true);

                for (int ch = 0; ch < numChannels; ++ch)
                    block.copyFrom (ch, 0, input[(size_t) ch].data() + readPosition, length);

                readPosition = (readPosition + length) % (numBlocks * maxBlock - maxBlock);

                // ---- 计数区：只夹住链自己的入口 ----
                const auto before = allocationCountOnThisThread();

                if (b % 100 == 99)
                    chain.restartWithFadeIn();

                chain.setParams (params, hostBypassed);
                chain.process (block);
                juce::ignoreUnused (chain.getLatencySamples(),
                                    chain.getAndResetLimitGainReductionDb(),
                                    chain.getAndResetGlueGainReductionDb());
                chain.getBalanceBandGainsDb (bandGains);

                allocations += allocationCountOnThisThread() - before;

                for (int ch = 0; ch < numChannels; ++ch)
                    for (int i = 0; i < length; ++i)
                        finite = finite && std::isfinite (block.getSample (ch, i));
            }

            expect (finite, "output must stay finite while every parameter moves");
            expectEquals ((juce::int64) allocations, (juce::int64) 0, "audio path must not allocate");
        }

        // 7. 同规格重进 prepareToPlay 只做 setParams + reset（v0.5 R8）：「跑过 1.5 s 之后 setParams + reset」与「全新实例
        //    prepare」之后，同一段 1 s 输入的输出逐位相同。三个采样率 × 两套参数（全中性；各模块都在工作：Balance 60 / 60、
        //    Heat 50、Glue 50、Clip 30、Gain +9 dB、Lookahead 5 ms）。1.5 s 让 Balance 的分析与修正、Glue 的包络、
        //    限幅器的释放包络都已不在初始状态。
        //    GLAZE 在这里保持 0（v0.4 时这里用过 GLAZE 60）：v0.6 起 GLAZE > 0 时编排器有「学到的」状态，reset 按规格保留它
        //    （时间线与保存第 3 节「清运行状态，不删已提交的画像」；聆听已累计的时长也保留），所以跑过的链与全新的链本来就不同。
        //    GLAZE > 0 的对应性质——reset 之后与「从同一份存档读回的全新链」逐位相同——在 GlazeController 测试（保存读取用例）里验。
        void testResetEqualsFreshPrepare()
        {
            beginTest ("reset after running == fresh prepare, bit-exact (same-spec prepareToPlay only resets)");

            auto busy = makeNeutralParams (5.0f);
            busy.balance.recover = 60.0f;
            busy.balance.tame = 60.0f;
            busy.color.heat = 50.0f;
            busy.color.glue = 50.0f;
            busy.limit.gainDb = 9.0f;
            busy.limit.clip = 30.0f;
            busy.glazePercent = 0.0f;

            struct Case { const char* name; CeladonChain::Params params; };
            const Case cases[] = { { "neutral", makeNeutralParams() }, { "busy", busy } };

            for (auto sampleRate : { 44100.0, 96000.0, 192000.0 })
            {
                for (const auto& c : cases)
                {
                    const auto warmUp = makeNoise (numChannels, (int) (1.5 * sampleRate), 0.5f, 11);
                    const auto input  = makeNoise (numChannels, (int) (1.0 * sampleRate), 0.5f, 12);

                    CeladonChain used;
                    prepareChain (used, sampleRate, c.params);
                    runChain (used, warmUp, maxBlock);
                    used.setParams (c.params, false);
                    used.reset();

                    CeladonChain fresh;
                    prepareChain (fresh, sampleRate, c.params);

                    const auto a = runChain (used, input, maxBlock);
                    const auto b = runChain (fresh, input, maxBlock);

                    // 逐位：按字节比（连 +0 / −0 也要一样），不是按浮点数相等。
                    auto identical = a.size() == b.size();

                    for (size_t ch = 0; identical && ch < a.size(); ++ch)
                        identical = a[ch].size() == b[ch].size()
                                     && std::memcmp (a[ch].data(), b[ch].data(), a[ch].size() * sizeof (float)) == 0;

                    expect (identical, juce::String (c.name) + " @" + juce::String (sampleRate, 0)
                                           + ": output after reset differs from a freshly prepared chain");
                }
            }
        }
    };

    static ChainTests chainTests;

    //==============================================================================
    // v0.1.7.0 修坯 Clay 在整条链里的位置与接线（设计规格-Clay 第 3、6 节，设计规格-Clay-分析决策与验收 第 6 节，ADR-009；
    // 实现计划 T-012 代理 A7）。单独一个测试类 CeladonChainClay（--only 时与 CeladonChain 分开点名），上面的用例一行不动。
    // 分工：Clay 模块自己的信号处理验收在 ClayTests，实时保护与三态合成的数值在 ClayGuardTests，GLAZE 自动值的整链行为在
    // ClayGlazeTests；这里只验链的接线——顺序、GLAZE 0 逐位直通、三态在 GLAZE 0 的接线、宿主旁通、延迟、重置、Guard 切片路径、
    // 音频路径不分配。
    // 链交给 Clay 的实际参数由编排器用 glaze::resolveClayParams 按三态合成（ChainParams.h 文件头；GLAZE ≤ 0 也照算）。这里的
    // 参考链用同一个函数算 Clay 的参数，所以本类的结论只关乎「链怎么接」，与三态合成的数值细节无关。
    namespace
    {
        constexpr double clayTestRate = 48000.0;

        // 逐位相同：按字节比（连 +0 / −0 也要一样），不是按浮点数相等。
        bool bitIdentical (const Signal& a, const Signal& b)
        {
            if (a.size() != b.size())
                return false;

            for (size_t ch = 0; ch < a.size(); ++ch)
                if (a[ch].size() != b[ch].size()
                     || std::memcmp (a[ch].data(), b[ch].data(), a[ch].size() * sizeof (float)) != 0)
                    return false;

            return true;
        }

        // 两段等长信号逐样本差的最大绝对值（不错位）。
        float maxAbsDifferenceWhole (const Signal& a, const Signal& b)
        {
            return maxAbsDifference (a, b, 0, (int) juce::jmin (a.front().size(), b.front().size()));
        }

        // 宽立体声 + 超低频素材，让 Clay 三级都有事做：
        //   M（两声道同相）：25 Hz 正弦 0.2（超低频，给低切）+ 1 kHz 正弦 0.1；
        //   S（两声道反相）：60 Hz 正弦 0.15（低频侧声道，给收单）；
        //   再叠 L、R 各自独立的白噪声 ±0.15（两路不相关：M、S 各占一半能量，高频侧声道给加宽）。
        // 采样峰值 ≤ 0.6（约 −4.4 dBFS），低于 −1 dBTP 的 Ceiling。
        Signal makeWideMaterial (double sampleRate, double seconds, juce::int64 seed)
        {
            const auto numSamples = (int) (seconds * sampleRate);
            auto signal = makeNoise (numChannels, numSamples, 0.15f, seed);

            for (int i = 0; i < numSamples; ++i)
            {
                const auto radians = juce::MathConstants<double>::twoPi * (double) i / sampleRate;   // × 频率 = 相位
                const auto mid  = 0.2f * (float) std::sin (25.0 * radians) + 0.1f * (float) std::sin (1000.0 * radians);
                const auto side = 0.15f * (float) std::sin (60.0 * radians);
                signal[0][(size_t) i] += mid + side;
                signal[1][(size_t) i] += mid - side;
            }

            return signal;
        }

        // 20 Hz 与 1 kHz 两个正弦（两声道相同 = 纯 M，各 0.25；采样峰值 0.5，低于 −1 dBTP 的 Ceiling，限幅器不动作）。
        Signal makeTwoToneInput (double sampleRate, double seconds)
        {
            auto input = makeSine (numChannels, sampleRate, 20.0, 0.25f, seconds);
            const auto tone = makeSine (numChannels, sampleRate, 1000.0, 0.25f, seconds);

            for (size_t ch = 0; ch < input.size(); ++ch)
                for (size_t i = 0; i < input[ch].size(); ++i)
                    input[ch][i] += tone[ch][i];

            return input;
        }

        // 单频幅度：在 [start, start + length) 上与该频率的 cos / sin 做相关（double 累加），取模——与窗口起点的相位无关。
        // length 取几个频率周期的公倍数时这几个频率彼此正交、互不串扰。
        double toneAmplitude (const std::vector<float>& channel, int start, int length, double frequency, double sampleRate)
        {
            double re = 0.0, im = 0.0;

            for (int i = 0; i < length; ++i)
            {
                const auto phase = juce::MathConstants<double>::twoPi * frequency * (double) (start + i) / sampleRate;
                const auto x = (double) channel[(size_t) (start + i)];
                re += x * std::cos (phase);
                im += x * std::sin (phase);
            }

            return 2.0 * std::sqrt (re * re + im * im) / (double) length;
        }

        // 最后 1 s 上 {20 Hz 幅度, 1 kHz 幅度}（第一声道）。只在 48 kHz 用：48000 样本 = 20 Hz 的 20 个周期、1 kHz 的 1000 个周期。
        std::pair<double, double> lowAndToneAmplitudes (const Signal& signal, double sampleRate)
        {
            const auto length = (int) sampleRate;
            const auto start = (int) signal[0].size() - length;
            return { toneAmplitude (signal[0], start, length, 20.0, sampleRate),
                     toneAmplitude (signal[0], start, length, 1000.0, sampleRate) };
        }

        // 三项都「手动」的抽屉设置（Clay Bypass 关）。
        ClaySettings makeManualClay (float lowCutHz, float monoHz, float widthDb)
        {
            ClaySettings settings;
            settings.lowCutMode = ClayMode::manual;
            settings.monoMode   = ClayMode::manual;
            settings.widthMode  = ClayMode::manual;
            settings.lowCutHz = lowCutHz;
            settings.monoHz   = monoHz;
            settings.widthDb  = widthDb;
            settings.bypass   = false;
            return settings;
        }

        // GLAZE 0 时链交给 ClayModule 的实际参数：编排器用 resolveClayParams 合成（GlazeController.h「GLAZE ≤ 0 时也照算」），
        // GLAZE 0 下「自动」「关」为中性、「手动」为旋钮值，与自动值无关——所以这里自动值给中性的 ClayPlan{}。
        ClayModule::Params clayParamsAtGlazeZero (const ClaySettings& settings)
        {
            return glaze::resolveClayParams (settings, glaze::ClayPlan {}, 0.0f);
        }

        // 与处理器一样每块先 setParams 再 process（参数不变；宿主旁通按 hostBypassed），返回输出。
        Signal runChainEachBlock (CeladonChain& chain, const CeladonChain::Params& params, const Signal& input,
                                  int blockSize, bool hostBypassed = false)
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

                chain.setParams (params, hostBypassed);
                chain.process (block);

                for (int ch = 0; ch < numChannels; ++ch)
                    std::copy (block.getReadPointer (ch), block.getReadPointer (ch) + length,
                               output[(size_t) ch].begin() + start);
            }

            return output;
        }

        // 单独一个 ClayModule 按指定块长处理整段信号（参数已设好），返回输出。
        Signal runClayModule (ClayModule& clay, const Signal& input, int blockSize)
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

                clay.process (block);

                for (int ch = 0; ch < numChannels; ++ch)
                    std::copy (block.getReadPointer (ch), block.getReadPointer (ch) + length,
                               output[(size_t) ch].begin() + start);
            }

            return output;
        }

        // 链之外单独拼的参考链：四个独立模块，Clay 放在指定位置（absent = 没有 Clay，即加 Clay 之前的 Balance → Color → Limit；
        // delayOnly = Clay 的位置换成一段 D 样本的 float 纯延迟线，D = 同采样率 ClayModule 上报的延迟——阶段 4 起「Clay 中性」的
        // 期望形态），与链同样的参数、同样的「setParams → prepare → setParams」、同样每块先 setParams 再 process、同样的分块。
        // 依据：GLAZE 0 时编排器交给 Balance / Color / Limit 的就是手动值（ADR-008），整链把模块 Listen 强制关掉
        //（CeladonChain::setParams），这里照做；链里的两块表与编排器只读音频、不改样本，Listen 关着时监听补偿逐位直通——
        // 所以 Clay 在设计位置的参考链与链逐位相同。
        enum class ClayPosition { absent, delayOnly, beforeBalance, afterBalance, afterColor, afterLimit };

        Signal runModules (const CeladonChain::Params& p, const ClayModule::Params& clayParams, ClayPosition position,
                           double sampleRate, const Signal& input, int blockSize)
        {
            BalanceModule balance;
            ClayModule clay;
            ColorModule color;
            LimiterModule limiter;

            auto limitParams = p.limit;
            limitParams.listen = false;

            const auto setAll = [&]
            {
                balance.setParams (p.balance);
                clay.setParams (clayParams);
                color.setParams (p.color);
                limiter.setParams (limitParams);
            };

            const auto spec = makeSpec (sampleRate);
            setAll();
            balance.prepare (spec);
            clay.prepare (spec);
            color.prepare (spec);
            limiter.prepare (spec);
            setAll();

            const auto numSamples = (int) input.front().size();
            auto output = input;
            juce::AudioBuffer<float> block (numChannels, blockSize);

            // delayOnly 的纯延迟线：容量正好 D，先读出 D 个样本之前写的、再写本样本（初值 0 = 「此前输入全是 0」，同 ClayModule）。
            const auto delayLength = juce::jmax (1, clay.getLatencySamples());
            std::vector<std::vector<float>> delayLine ((size_t) numChannels, std::vector<float> ((size_t) delayLength, 0.0f));
            int delayPosition = 0;

            for (int start = 0; start < numSamples; start += blockSize)
            {
                const auto length = juce::jmin (blockSize, numSamples - start);
                block.setSize (numChannels, length, false, false, true);

                for (int ch = 0; ch < numChannels; ++ch)
                    block.copyFrom (ch, 0, input[(size_t) ch].data() + start, length);

                setAll();

                if (position == ClayPosition::beforeBalance) clay.process (block);
                balance.process (block);
                if (position == ClayPosition::afterBalance)  clay.process (block);

                if (position == ClayPosition::delayOnly)
                {
                    for (int i = 0; i < length; ++i)
                    {
                        for (int ch = 0; ch < numChannels; ++ch)
                        {
                            auto& slot = delayLine[(size_t) ch][(size_t) delayPosition];
                            const auto delayed = slot;
                            slot = block.getSample (ch, i);
                            block.setSample (ch, i, delayed);
                        }

                        delayPosition = delayPosition + 1 == delayLength ? 0 : delayPosition + 1;
                    }
                }

                color.process (block);
                if (position == ClayPosition::afterColor)    clay.process (block);
                limiter.process (block);
                if (position == ClayPosition::afterLimit)    clay.process (block);

                for (int ch = 0; ch < numChannels; ++ch)
                    std::copy (block.getReadPointer (ch), block.getReadPointer (ch) + length,
                               output[(size_t) ch].begin() + start);
            }

            return output;
        }
    }

    class ChainClayTests  : public juce::UnitTest
    {
    public:
        ChainClayTests() : juce::UnitTest ("CeladonChainClay", "Celadon") {}

        void runTest() override
        {
            testNeutralClayIsBitExactAtGlazeZero();
            testSignalOrder();
            testManualLowCutAtGlazeZero();
            testHostBypassForcesClayBypass();
            testLatencyIncludesClayDelay();
            testRestartAndResetClearClay();
            testGuardSlicesIncludeClay();
            testAudioPathDoesNotAllocateWithClay();
        }

    private:
        // 1. 逐位不变（设计规格-Clay-分析决策与验收 第 6 节第 1 条；阶段 4 起按设计规格-Clay-根治修订 第 2 节「逐位」「GLAZE 0 且
        //    抽屉默认」：Clay 中性 = 输入逐位延迟 D）。
        //    GLAZE 0，其余三个模块都在工作（Balance 60 / 60、Tone +1.5、Glue 40、Heat 40、Gain +6、Clip 20），宽立体声 + 超低频素材。
        //    以下设置下整链输出都必须与「加 Clay 之前的 Balance → Color → Limit，在 Clay 的位置插一段 D 样本纯延迟」（独立模块按同样
        //    分块拼，ClayPosition::delayOnly）逐位相同（memcmp，连 ±0）：抽屉默认（三态都「自动」）；三态「自动」但手动旋钮拧到头；
        //    三态都「关」且旋钮拧到头；只开 Clay Bypass（延迟补偿式旁通，干路同样延迟 D）；以及「自动」「关」两态下每块拖动三个手动旋钮
        //    （设计规格-Clay 第 5 节「旋钮可拖但不生效」——手动值在动也不许让 Clay 离开逐位纯延迟）。三种采样率，48 kHz 另跑块长 17。
        //    为什么参考链是「插一段纯延迟」而不是「旧链输出整体平移 D」：Glue 的释放时间与限幅器的自动释放每 32 样本（从 reset 起的
        //    固定网格）按信号重算一次，输入整体晚 D 个样本（D 不是 32 的倍数）后网格与信号的相对位置变了，非中性的 Color / Limit 输出
        //    不再是旧输出的逐位平移——那是它们自己的网格口径，与 Clay 无关。「平移 D 逐位相同」在 Color / Limit 为纯延迟时成立，见下面
        //    第二段（Balance 在 Clay 之前、照样在工作）。
        //    阳性对照：低切改成「手动」60 Hz（GLAZE 0 也生效，ADR-009），输出必须与参考链不同、且差 > 1e-3——素材里 25 Hz、0.2 的
        //    正弦被 60 Hz 4 阶 Butterworth 高通削掉 10·log10(1 + (60/25)^8) = 30.4 dB，差值量级 0.1。说明 Clay 确实在信号路径上、
        //    这套比较分得出它开没开。
        //    第二段（根治修订第 2 节「整链输出与改动前的输出平移 D 个样本后逐位相同」的字面口径）：Balance 60 / 60 照样工作、Color 与
        //    Limit 中性（纯延迟）、素材缩到 0.25 倍（采样峰值 ≤ 0.15，Balance 最多抬十来 dB 也远低于 −1 dBTP，限幅器不动作）——
        //    整链输出逐位等于「加 Clay 之前的链」输出整体延后 D 个样本（头 D 个样本为 0）；阳性对照：不平移时两者不同。
        void testNeutralClayIsBitExactAtGlazeZero()
        {
            beginTest ("GLAZE 0: drawer defaults / all Off / Clay Bypass are bit-exact with the pre-Clay chain plus a D-sample delay in Clay's slot");

            auto base = makeNeutralParams (2.0f);
            base.balance.recover = 60.0f;
            base.balance.tame = 60.0f;
            base.color.toneDb = 1.5f;
            base.color.glue = 40.0f;
            base.color.heat = 40.0f;
            base.limit.gainDb = 6.0f;
            base.limit.clip = 20.0f;
            base.glazePercent = 0.0f;

            // 三态仍是「自动」，手动旋钮拧到头：GLAZE 0 下自动值为中性，手动值必须不起作用。
            ClaySettings autoWithMaxKnobs;
            autoWithMaxKnobs.lowCutHz = ClayModule::maxLowCutHz;
            autoWithMaxKnobs.monoHz = ClayModule::maxMonoHz;
            autoWithMaxKnobs.widthDb = ClayModule::maxWidthDb;

            auto allOff = autoWithMaxKnobs;
            allOff.lowCutMode = ClayMode::off;
            allOff.monoMode = ClayMode::off;
            allOff.widthMode = ClayMode::off;

            ClaySettings bypassOnly;
            bypassOnly.bypass = true;

            struct Variant { const char* name; ClaySettings settings; };
            const Variant variants[] = { { "drawer defaults (all Auto)", ClaySettings {} },
                                         { "Auto with manual knobs at max", autoWithMaxKnobs },
                                         { "all Off with manual knobs at max", allOff },
                                         { "Clay Bypass on", bypassOnly } };

            struct Case { double sampleRate; int blockSize; };
            const Case cases[] = { { 44100.0, maxBlock }, { 48000.0, maxBlock }, { 48000.0, 17 }, { 96000.0, maxBlock } };

            for (const auto& c : cases)
            {
                const auto caseName = " @" + juce::String (c.sampleRate, 0) + " block=" + juce::String (c.blockSize);
                const auto input = makeWideMaterial (c.sampleRate, 1.0, 21);
                const auto reference = runModules (base, ClayModule::Params {}, ClayPosition::delayOnly, c.sampleRate, input, c.blockSize);
                expect (allFinite (reference), "reference must be finite" + caseName);

                for (const auto& variant : variants)
                {
                    auto params = base;
                    params.claySettings = variant.settings;

                    CeladonChain chain;
                    prepareChain (chain, c.sampleRate, params);
                    const auto output = runChainEachBlock (chain, params, input, c.blockSize);

                    expect (bitIdentical (output, reference),
                            juce::String (variant.name) + " must be bit-exact with the pre-Clay chain plus a D-sample delay, max diff "
                                + juce::String (maxAbsDifferenceWhole (output, reference), 9) + caseName);
                }

                // 「自动」「关」两态下每块拖动手动旋钮：三个手动值随块从一端扫到另一端（收单分频点反向扫，三者不同步）。
                for (auto mode : { ClayMode::automatic, ClayMode::off })
                {
                    auto params = base;
                    params.claySettings.lowCutMode = mode;
                    params.claySettings.monoMode = mode;
                    params.claySettings.widthMode = mode;

                    CeladonChain chain;
                    prepareChain (chain, c.sampleRate, params);

                    const auto numSamples = (int) input.front().size();
                    auto output = input;
                    juce::AudioBuffer<float> block (numChannels, c.blockSize);

                    for (int start = 0; start < numSamples; start += c.blockSize)
                    {
                        const auto length = juce::jmin (c.blockSize, numSamples - start);
                        const auto progress = (float) start / (float) numSamples;   // 0 → 1
                        params.claySettings.lowCutHz = ClayModule::minLowCutHz + (ClayModule::maxLowCutHz - ClayModule::minLowCutHz) * progress;
                        params.claySettings.monoHz   = ClayModule::maxMonoHz - (ClayModule::maxMonoHz - ClayModule::minMonoHz) * progress;
                        params.claySettings.widthDb  = ClayModule::minWidthDb + (ClayModule::maxWidthDb - ClayModule::minWidthDb) * progress;

                        block.setSize (numChannels, length, false, false, true);

                        for (int ch = 0; ch < numChannels; ++ch)
                            block.copyFrom (ch, 0, input[(size_t) ch].data() + start, length);

                        chain.setParams (params, false);
                        chain.process (block);

                        for (int ch = 0; ch < numChannels; ++ch)
                            std::copy (block.getReadPointer (ch), block.getReadPointer (ch) + length,
                                       output[(size_t) ch].begin() + start);
                    }

                    expect (bitIdentical (output, reference),
                            juce::String (mode == ClayMode::off ? "Off" : "Auto")
                                + " with the manual knobs moving must stay bit-exact, max diff "
                                + juce::String (maxAbsDifferenceWhole (output, reference), 9) + caseName);
                }

                // 阳性对照：低切「手动」60 Hz。
                auto lowCut = base;
                lowCut.claySettings.lowCutMode = ClayMode::manual;
                lowCut.claySettings.lowCutHz = 60.0f;

                CeladonChain chain;
                prepareChain (chain, c.sampleRate, lowCut);
                const auto output = runChainEachBlock (chain, lowCut, input, c.blockSize);
                const auto difference = maxAbsDifferenceWhole (output, reference);

                expect (! bitIdentical (output, reference) && difference > 1.0e-3f,
                        "positive control: a manual 60 Hz low cut must change the output, max diff "
                            + juce::String (difference, 6) + caseName);
            }

            // 第二段：Color / Limit 为纯延迟时，整链 = 加 Clay 之前的链输出整体延后 D（根治修订第 2 节的字面口径）。
            for (auto sampleRate : { 48000.0, 96000.0 })
            {
                const auto caseName = " @" + juce::String (sampleRate, 0);
                auto balanceOnly = makeNeutralParams (2.0f);
                balanceOnly.balance.recover = 60.0f;
                balanceOnly.balance.tame = 60.0f;
                balanceOnly.glazePercent = 0.0f;

                auto input = makeWideMaterial (sampleRate, 1.0, 23);
                scale (input, 0.25f);

                ClayModule standalone;
                standalone.prepare (makeSpec (sampleRate));
                const auto delay = standalone.getLatencySamples();

                const auto before = runModules (balanceOnly, ClayModule::Params {}, ClayPosition::absent, sampleRate, input, maxBlock);
                auto shifted = makeSilence (numChannels, (int) before.front().size());

                for (size_t ch = 0; ch < shifted.size(); ++ch)
                    std::copy (before[ch].begin(), before[ch].end() - delay, shifted[ch].begin() + delay);

                CeladonChain chain;
                prepareChain (chain, sampleRate, balanceOnly);
                const auto output = runChainEachBlock (chain, balanceOnly, input, maxBlock);

                expect (bitIdentical (output, shifted),
                        "Color / Limit neutral: the chain equals the pre-Clay output delayed by D, max diff "
                            + juce::String (maxAbsDifferenceWhole (output, shifted), 9) + caseName);
                expect (! bitIdentical (output, before), "positive control: without the shift they differ" + caseName);
                expect (truePeakOf (before) < juce::Decibels::decibelsToGain (-1.0f), "fixture: the limiter stays idle" + caseName);
            }
        }

        // 2. 链顺序 Balance → Clay → Color → Limit（设计规格-Clay 第 3 节、ADR-009）。GLAZE 0，Clay 三项都「手动」（低切 30 Hz、
        //    收单 150 Hz、加宽 +3 dB），其余三个模块都在做非线性 / 自适应处理（Balance 80 / 80、Tone +2、Glue 60、Heat 60、
        //    Gain +9、Clip 30），宽立体声 + 超低频素材 1.5 s。
        //    链输出必须与「Balance → Clay → Color → Limit」参考链逐位相同（memcmp）；Clay 放在另外三个位置（Balance 前 / Color 后 /
        //    Limit 后）的参考链都必须与设计顺序不同——这是阳性对照：说明这套构造分得出 Clay 的位置（链若接错了位置，它的输出就与
        //    那一种逐位相同、从而与设计顺序不同，第一条断言失败）。逻辑上「不逐位相同」已足够，另要求差 > 1e-5（float 舍入噪声的
        //    约 100 倍）以免只靠舍入分辨；实际差值打在日志里。
        //    为什么不用「只开 Clay 加宽 + 只开 Color Tone」：两者都是线性时不变（Tone 是 L / R 同一对搁架，加宽是只对 S 的高架），
        //    交换次序在数学上等价、只差浮点舍入；这里换成 Glue / Heat / Clip / 限幅器的非线性与 Balance 的自适应（Balance 看得到
        //    Clay 削掉的超低频与收掉的侧声道，Glue 的联动峰值检测看得到加宽后的高频侧声道），交换次序后的差是可听的量级。
        void testSignalOrder()
        {
            beginTest ("signal order is Balance -> Clay -> Color -> Limit (bit-exact with that order, distinguishable from every other)");

            constexpr double sampleRate = clayTestRate;
            auto params = makeNeutralParams (2.0f);
            params.balance.recover = 80.0f;
            params.balance.tame = 80.0f;
            params.color.toneDb = 2.0f;
            params.color.glue = 60.0f;
            params.color.heat = 60.0f;
            params.limit.gainDb = 9.0f;
            params.limit.clip = 30.0f;
            params.glazePercent = 0.0f;
            params.claySettings = makeManualClay (30.0f, 150.0f, 3.0f);

            const auto clayParams = clayParamsAtGlazeZero (params.claySettings);
            const auto input = makeWideMaterial (sampleRate, 1.5, 31);

            CeladonChain chain;
            prepareChain (chain, sampleRate, params);
            const auto output = runChainEachBlock (chain, params, input, maxBlock);
            expect (allFinite (output), "chain output must be finite");

            const auto designed = runModules (params, clayParams, ClayPosition::afterBalance, sampleRate, input, maxBlock);
            expect (bitIdentical (output, designed),
                    "chain must be bit-exact with Balance -> Clay -> Color -> Limit, max diff "
                        + juce::String (maxAbsDifferenceWhole (output, designed), 9));

            struct Alternative { const char* name; ClayPosition position; };
            const Alternative alternatives[] = { { "Clay -> Balance -> Color -> Limit", ClayPosition::beforeBalance },
                                                 { "Balance -> Color -> Clay -> Limit", ClayPosition::afterColor },
                                                 { "Balance -> Color -> Limit -> Clay", ClayPosition::afterLimit } };

            for (const auto& alternative : alternatives)
            {
                const auto other = runModules (params, clayParams, alternative.position, sampleRate, input, maxBlock);
                const auto difference = maxAbsDifferenceWhole (designed, other);
                logMessage ("  " + juce::String (alternative.name) + " vs designed order: max diff "
                            + juce::String (toDb (difference), 1) + " dBFS");
                expect (! bitIdentical (designed, other) && difference > 1.0e-5f,
                        juce::String (alternative.name) + " must be distinguishable from the designed order, max diff "
                            + juce::String (difference, 9));
            }
        }

        // 3. 三态接线（ADR-009）：GLAZE 0 时「手动」低切生效，「自动」「关」不处理。其余模块全中性、Lookahead 2 ms，
        //    输入 = 20 Hz 与 1 kHz 两个正弦（两声道相同，各 0.25；峰值 0.5 低于 Ceiling，限幅器不动作），旋钮都在 60 Hz，
        //    量输出最后 1 s（20 Hz 的 20 个周期、1 kHz 的 1000 个周期，两个频率在这一窗上正交）的单频幅度：
        //      - 手动：20 Hz 应衰减 10·log10(1 + (60/20)^8) = 38.2 dB（4 阶 Butterworth），断言 ≥ 30 dB；1 kHz 应衰减
        //        10·log10(1 + (60/1000)^8) ≈ 7e-10 dB，断言 |变化| ≤ 0.05 dB（是高通，不是整体衰减）；
        //      - 自动（GLAZE 0 → 中性）与关：其余模块中性时整链是纯延迟，两个频率的幅度变化都 ≤ 0.01 dB。
        //    另核对编排器交给链的实际参数 getEffectiveParams().clay：低切 = resolveClayParams 的结果（手动 60、其余 0），
        //    收单量、加宽为 0（这两项留在「自动」），Bypass 关——测试失败时据此分清是编排器没合成还是链没接。
        void testManualLowCutAtGlazeZero()
        {
            beginTest ("GLAZE 0: a Manual low cut is applied; Auto and Off leave the sub band untouched");

            constexpr double sampleRate = clayTestRate;
            const auto input = makeTwoToneInput (sampleRate, 2.0);
            const auto inputAmplitudes = lowAndToneAmplitudes (input, sampleRate);

            struct Case { const char* name; ClayMode mode; };
            const Case cases[] = { { "Manual", ClayMode::manual }, { "Auto", ClayMode::automatic }, { "Off", ClayMode::off } };

            for (const auto& c : cases)
            {
                auto params = makeNeutralParams (2.0f);
                params.glazePercent = 0.0f;
                params.claySettings.lowCutMode = c.mode;
                params.claySettings.lowCutHz = 60.0f;

                CeladonChain chain;
                prepareChain (chain, sampleRate, params);
                const auto output = runChainEachBlock (chain, params, input, maxBlock);
                expect (allFinite (output), c.name);

                const auto expected = clayParamsAtGlazeZero (params.claySettings);
                const auto& effective = chain.getEffectiveParams().clay;
                expectEquals (effective.lowCutHz, expected.lowCutHz, juce::String (c.name) + ": effective low cut");
                expectEquals (effective.monoAmount, 0.0f, juce::String (c.name) + ": effective mono amount");
                expectEquals (effective.widthDb, 0.0f, juce::String (c.name) + ": effective width");
                expect (! effective.bypass, juce::String (c.name) + ": effective Clay bypass");

                const auto outputAmplitudes = lowAndToneAmplitudes (output, sampleRate);
                const auto lowChangeDb  = toDb (outputAmplitudes.first / inputAmplitudes.first);
                const auto toneChangeDb = toDb (outputAmplitudes.second / inputAmplitudes.second);
                logMessage ("  low cut " + juce::String (c.name) + " (knob 60 Hz): 20 Hz " + juce::String (lowChangeDb, 2)
                            + " dB, 1 kHz " + juce::String (toneChangeDb, 4) + " dB");

                if (c.mode == ClayMode::manual)
                {
                    expectEquals (expected.lowCutHz, 60.0f, "Manual low cut must equal the knob value (ADR-009)");
                    expect (lowChangeDb <= -30.0f, "Manual 60 Hz low cut must cut 20 Hz by >= 30 dB (4th-order Butterworth: 38.2 dB), got "
                                                    + juce::String (lowChangeDb, 2) + " dB");
                    expect (std::abs (toneChangeDb) <= 0.05f, "1 kHz must pass the low cut, change "
                                                                + juce::String (toneChangeDb, 4) + " dB");
                }
                else
                {
                    expectEquals (expected.lowCutHz, 0.0f, juce::String (c.name) + " at GLAZE 0 must leave the low cut off");
                    expect (std::abs (lowChangeDb) <= 0.01f, juce::String (c.name) + " must not touch 20 Hz, change "
                                                               + juce::String (lowChangeDb, 4) + " dB");
                    expect (std::abs (toneChangeDb) <= 0.01f, juce::String (c.name) + " must not touch 1 kHz, change "
                                                                + juce::String (toneChangeDb, 4) + " dB");
                }
            }
        }

        // 4. 宿主旁通强制 Clay 旁通（CeladonChain.h「hostBypassed = 宿主旁通：强制旁通全部模块（含 Clay）」）。
        //    Clay 三项「手动」开到头（低切 60 Hz、收单 300 Hz、加宽 +3 dB）、Clay Bypass 关，其余模块也开满（同 CeladonChain 的
        //    宿主旁通用例），素材放大到采样峰值约 0.96（远超 −3 dBTP 的 Ceiling）：
        //      a) 从 prepare 起就旁通：输出逐位等于延迟后的输入；
        //      b) 播放 0.5 s 后切到旁通：各模块 20 ms 交叉淡化走完之后（切换后 0.25 s 起，同 CeladonChain 的 Listen / 旁通用例口径）
        //         输出逐位等于延迟后的输入。
        //    阳性对照：同样的 Clay 设置、其余模块中性、不旁通 → 输出与延迟后的输入差 > 1e-3（Clay 确实在处理：25 Hz 超低频被削
        //    约 30 dB、60 Hz 侧声道收单、高频侧声道 +3 dB）——Clay 若没被强制旁通，a) b) 一定看得见。
        void testHostBypassForcesClayBypass()
        {
            beginTest ("host bypass forces Clay into bypass: bit-exact delayed input with Clay fully engaged");

            const auto engagedClay = makeManualClay (ClayModule::maxLowCutHz, ClayModule::maxMonoHz, ClayModule::maxWidthDb);

            auto everythingOn = makeNeutralParams (2.0f);
            everythingOn.balance.recover = 100.0f;
            everythingOn.balance.tame = 100.0f;
            everythingOn.balance.brighten = 100.0f;
            everythingOn.balance.speed = BalanceModule::Speed::fast;
            everythingOn.color.toneDb = 3.0f;
            everythingOn.color.glue = 100.0f;
            everythingOn.color.heat = 100.0f;
            everythingOn.color.outputDb = 6.0f;
            everythingOn.limit.gainDb = 24.0f;
            everythingOn.limit.ceilingDb = -3.0f;
            everythingOn.limit.clip = 100.0f;
            everythingOn.glazePercent = 0.0f;
            everythingOn.claySettings = engagedClay;

            for (auto sampleRate : { 44100.0, 48000.0 })
            {
                const auto caseName = " @" + juce::String (sampleRate, 0);
                auto input = makeWideMaterial (sampleRate, 1.5, 51);
                scale (input, 1.6f);
                const auto length = (int) input.front().size();

                // a) 从 prepare 起就旁通。
                {
                    CeladonChain chain;
                    prepareChain (chain, sampleRate, everythingOn, true);
                    const auto latency = chain.getLatencySamples();

                    auto padded = input;
                    appendSilence (padded, latency);
                    const auto output = runChainEachBlock (chain, everythingOn, padded, maxBlock, true);

                    expectEquals (maxAbsDifference (padded, output, latency, length), 0.0f,
                                  "bypassed from prepare: must be the bit-exact delayed input" + caseName);
                }

                // b) 播放中途切到宿主旁通。
                {
                    CeladonChain chain;
                    prepareChain (chain, sampleRate, everythingOn);
                    const auto latency = chain.getLatencySamples();
                    const auto switchAt = (int) (0.5 * sampleRate);

                    auto output = input;
                    juce::AudioBuffer<float> block (numChannels, maxBlock);

                    for (int start = 0; start < length; start += maxBlock)
                    {
                        const auto count = juce::jmin (maxBlock, length - start);
                        block.setSize (numChannels, count, false, false, true);

                        for (int ch = 0; ch < numChannels; ++ch)
                            block.copyFrom (ch, 0, input[(size_t) ch].data() + start, count);

                        chain.setParams (everythingOn, start >= switchAt);
                        chain.process (block);

                        for (int ch = 0; ch < numChannels; ++ch)
                            std::copy (block.getReadPointer (ch), block.getReadPointer (ch) + count,
                                       output[(size_t) ch].begin() + start);
                    }

                    const auto settled = switchAt + (int) (0.25 * sampleRate);
                    float worstError = 0.0f;

                    for (size_t ch = 0; ch < output.size(); ++ch)
                        for (int i = settled; i < length; ++i)
                            worstError = juce::jmax (worstError, std::abs (output[ch][(size_t) i] - input[ch][(size_t) (i - latency)]));

                    expectEquals (worstError, 0.0f, "switched to host bypass mid-stream: bit-exact delayed input after the fades" + caseName);
                }
            }

            // 阳性对照：只开 Clay、不旁通。
            {
                auto clayOnly = makeNeutralParams (2.0f);
                clayOnly.glazePercent = 0.0f;
                clayOnly.claySettings = engagedClay;

                CeladonChain chain;
                prepareChain (chain, clayTestRate, clayOnly);
                const auto latency = chain.getLatencySamples();

                auto input = makeWideMaterial (clayTestRate, 1.0, 52);
                const auto length = (int) input.front().size();
                appendSilence (input, latency);
                const auto output = runChainEachBlock (chain, clayOnly, input, maxBlock);
                const auto difference = maxAbsDifference (input, output, latency, length);

                expect (difference > 1.0e-3f, "positive control: fully engaged Clay must change the delayed input, max diff "
                                                  + juce::String (difference, 6));
            }
        }

        // 5. 延迟 = 旧值 + Clay 的固定延迟（设计规格-Clay-根治修订 第 2 节、第 4 节第 4 条「整链上报 = 旧值 + D」；ADR-014 起 Clay 的
        //    固定延迟 = D + 峰值守护前瞻 G；旧值 282 / 378 = v0.1.6.2 实测，设计规格-Clay-分析决策与验收 第 5 节）。
        //    ClayModule 自己报 D + G（48 / 96 kHz 为 1102 / 2206）；抽屉默认与三项「手动」全开两种设置下，链的延迟都等于 1384 / 2584，也等于
        //    独立 Balance + Clay + Color + Limit 之和（旧的三项之和再加 D）——延迟与 Clay 的参数无关；脉冲实测：−6 dBFS 单样本脉冲、
        //    两声道相同（纯 M：收单、加宽只动 S，不碰它；30 Hz 低切的冲激响应首样本约 0.995、其后只有很小的负尾巴，限幅器不动作），
        //    峰值落在 position + 延迟。
        void testLatencyIncludesClayDelay()
        {
            beginTest ("chain latency = 282 + Clay = 1384 samples @48 kHz / 378 + Clay = 2584 @96 kHz with Lookahead 2 ms, default or fully Manual");

            struct Case { double sampleRate; int expectedLatency; int expectedClayDelay; };
            const Case cases[] = { { 48000.0, 1384, 1102 }, { 96000.0, 2584, 2206 } };   // ADR-014：Clay = D + 峰值守护前瞻 G

            for (const auto& c : cases)
             for (int variant = 0; variant < 2; ++variant)   // 0 = 抽屉默认；1 = 三项「手动」全开
            {
                auto params = makeNeutralParams (2.0f);
                params.glazePercent = 0.0f;

                if (variant == 1)
                    params.claySettings = makeManualClay (30.0f, 120.0f, 2.0f);

                const auto caseName = " @" + juce::String (c.sampleRate, 0) + (variant == 1 ? " Clay manual" : " Clay default");

                CeladonChain chain;
                prepareChain (chain, c.sampleRate, params);

                BalanceModule balance;
                balance.setParams (params.balance);
                balance.prepare (makeSpec (c.sampleRate));

                ClayModule clay;
                clay.setParams (clayParamsAtGlazeZero (params.claySettings));
                clay.prepare (makeSpec (c.sampleRate));

                ColorModule color;
                color.setParams (params.color);
                color.prepare (makeSpec (c.sampleRate));

                LimiterModule limiter;
                limiter.setParams (params.limit);
                limiter.prepare (makeSpec (c.sampleRate));

                const auto latency = chain.getLatencySamples();
                expectEquals (clay.getLatencySamples(), c.expectedClayDelay, "ClayModule reports its fixed delay D" + caseName);
                expectEquals (latency, c.expectedLatency, "chain latency" + caseName);
                expectEquals (latency, balance.getLatencySamples() + clay.getLatencySamples() + color.getLatencySamples()
                                           + limiter.getLatencySamples(),
                              "latency = the pre-Clay sum plus Clay's D" + caseName);

                constexpr int position = 100;
                const auto input = makeImpulse (numChannels, position + latency + 256, position, 0.5f);
                const auto output = runChainEachBlock (chain, params, input, maxBlock);

                expect (allFinite (output), caseName);
                expectEquals (peakIndexOf (output[0]) - position, latency, "impulse peak L" + caseName);
                expectEquals (peakIndexOf (output[1]) - position, latency, "impulse peak R" + caseName);
            }
        }

        // 6. restartWithFadeIn 与 reset 都清空 Clay 的状态（CeladonChain.h「清空全部模块」；设计规格-Clay 第 4 节「reset / NaN 恢复清
        //    滤波器状态」）。Clay 三项「手动」（低切 20 Hz、收单 150 Hz、加宽 +3 dB），其余模块中性，GLAZE 0：
        //      a) 链 A 先用宽立体声 + 超低频素材跑 1 s，restartWithFadeIn，再跑 0.5 s 新素材；链 B 全新 prepare、同样 restartWithFadeIn、
        //         跑同一段新素材——两者逐位相同（memcmp，含静音段与淡入段）；
        //      b) 同样的对照换成「setParams + reset()」（同规格 prepareToPlay 的路径）对全新 prepare。
        //    Balance / Color / Limit 的清空已由 CeladonChain 的 reset 用例证明（restartWithFadeIn 调的是同样的 reset），GLAZE 0 时编排器
        //    的学习状态不影响音频，所以这里差的只能是 Clay。
        //    阳性对照：独立 ClayModule 用同样参数与同样的预热素材跑完后不 reset、接着处理新素材，与全新模块处理同一段新素材相比，
        //    从它自己的第 D + 10 ms 个输出起仍差 > 1e-3。为什么从 D + 10 ms：Clay 有固定延迟 D（阶段 4 起），链重启后限幅器把整条链的
        //    延迟（含 D）都当静音段、之后才 10 ms 淡入，所以 Clay 的第 D + n 个输出落在淡入开始后第 n 个样本；D 之前的输出（延迟线里
        //    预热留下的旧音乐）在链里被静音段盖住，不算。D + 10 ms 之后还差的是 20 Hz 4 阶高通的状态：最慢一对极点的衰减率
        //    sin(π/8)·2π·20 ≈ 48 /s（时间常数约 21 ms），预热留下的状态在淡入之后还响着——链若漏了 clay.reset()，a) b) 一定看得见。
        void testRestartAndResetClearClay()
        {
            beginTest ("restartWithFadeIn and reset clear Clay's state (bit-exact with a freshly prepared chain)");

            constexpr double sampleRate = clayTestRate;
            auto params = makeNeutralParams (2.0f);
            params.glazePercent = 0.0f;
            params.claySettings = makeManualClay (20.0f, 150.0f, 3.0f);

            const auto warmUp = makeWideMaterial (sampleRate, 1.0, 71);
            const auto after  = makeWideMaterial (sampleRate, 0.5, 72);

            // a) restartWithFadeIn
            {
                CeladonChain used, clean;
                prepareChain (used, sampleRate, params);
                prepareChain (clean, sampleRate, params);
                runChainEachBlock (used, params, warmUp, maxBlock);

                used.restartWithFadeIn();
                clean.restartWithFadeIn();

                const auto fromUsed = runChainEachBlock (used, params, after, maxBlock);
                const auto fromClean = runChainEachBlock (clean, params, after, maxBlock);

                expect (allFinite (fromUsed), "output after restartWithFadeIn must be finite");
                expect (bitIdentical (fromUsed, fromClean),
                        "restartWithFadeIn must clear Clay: differs from a fresh chain by "
                            + juce::String (maxAbsDifferenceWhole (fromUsed, fromClean), 9));
            }

            // b) setParams + reset
            {
                CeladonChain used, clean;
                prepareChain (used, sampleRate, params);
                runChainEachBlock (used, params, warmUp, maxBlock);
                used.setParams (params, false);
                used.reset();

                prepareChain (clean, sampleRate, params);

                const auto fromUsed = runChainEachBlock (used, params, after, maxBlock);
                const auto fromClean = runChainEachBlock (clean, params, after, maxBlock);

                expect (bitIdentical (fromUsed, fromClean),
                        "reset must clear Clay: differs from a fresh chain by "
                            + juce::String (maxAbsDifferenceWhole (fromUsed, fromClean), 9));
            }

            // 阳性对照：独立 ClayModule 预热后不 reset。
            {
                const auto clayParams = clayParamsAtGlazeZero (params.claySettings);
                ClayModule warmed, pristine;

                for (auto* clayModule : { &warmed, &pristine })
                {
                    clayModule->setParams (clayParams);
                    clayModule->prepare (makeSpec (sampleRate));
                    clayModule->setParams (clayParams);
                }

                runClayModule (warmed, warmUp, maxBlock);
                const auto continued = runClayModule (warmed, after, maxBlock);
                const auto fresh = runClayModule (pristine, after, maxBlock);

                const auto fadeEnd = warmed.getLatencySamples() + (int) (0.01 * sampleRate);
                float leak = 0.0f;

                for (size_t ch = 0; ch < continued.size(); ++ch)
                    for (int i = fadeEnd; i < (int) continued[ch].size(); ++i)
                        leak = juce::jmax (leak, std::abs (continued[ch][(size_t) i] - fresh[ch][(size_t) i]));

                logMessage ("  Clay state left by the warm-up, D + 10 ms after a missed reset: " + juce::String (toDb (leak), 1) + " dBFS");
                expect (leak > 1.0e-3f, "positive control: Clay's warm-up state must still be audible D + 10 ms after the restart, leak "
                                            + juce::String (leak, 6));
            }
        }

        // 7. GLAZE > 0 时链按 Guard 的 10 ms 时隙切片处理（CeladonChain::process），切片路径同样经过 Clay：GLAZE 35、低切「手动」60 Hz、
        //    宿主块 2048（48 kHz 下一个时隙 480 样本，核对确实切成了多片），输入同第 3 条（20 Hz + 1 kHz 各 0.25）跑 3 s。
        //    最后 1 s 的「20 Hz 幅度 / 1 kHz 幅度」比同一条链低切「关」时下降 ≥ 25 dB（60 Hz 4 阶高通在 20 Hz 衰减 38.2 dB）。
        //    两条链的 GLAZE 自动增量来源相同（分析器只看链入口与 Balance 帧，都在 Clay 之前）；Tone 搁架等线性部分对两个频率的
        //    比值两条链一样，Glue / 限幅器的增益对两个频率同时作用、在比值里消掉；余量留给两条链之间校准与保护的细小差别。
        //    同时也验了「手动」在 GLAZE > 0 时照样严格按旋钮值（ADR-009）。
        void testGuardSlicesIncludeClay()
        {
            beginTest ("GLAZE > 0: the Guard-slot sliced path runs Clay too (a Manual low cut applies while GLAZE is active)");

            constexpr double sampleRate = clayTestRate;
            constexpr int hostBlock = 2048;
            const auto input = makeTwoToneInput (sampleRate, 3.0);
            const auto numSamples = (int) input.front().size();
            std::array<double, 2> lowToToneDb {};

            for (int variant = 0; variant < 2; ++variant)   // 0 = 低切「关」（对照）；1 = 低切「手动」60 Hz
            {
                auto params = makeNeutralParams (2.0f);
                params.glazePercent = 35.0f;
                params.claySettings.lowCutMode = variant == 0 ? ClayMode::off : ClayMode::manual;
                params.claySettings.lowCutHz = 60.0f;

                CeladonChain chain;
                chain.setParams (params, false);
                chain.prepare ({ sampleRate, (juce::uint32) hostBlock, (juce::uint32) numChannels });
                chain.setParams (params, false);

                auto output = input;
                juce::AudioBuffer<float> block (numChannels, hostBlock);
                int mostSlices = 0;

                for (int start = 0; start < numSamples; start += hostBlock)
                {
                    const auto count = juce::jmin (hostBlock, numSamples - start);
                    block.setSize (numChannels, count, false, false, true);

                    for (int ch = 0; ch < numChannels; ++ch)
                        block.copyFrom (ch, 0, input[(size_t) ch].data() + start, count);

                    chain.setParams (params, false);
                    chain.process (block);
                    mostSlices = juce::jmax (mostSlices, chain.getLastProcessSliceCountForTesting());

                    for (int ch = 0; ch < numChannels; ++ch)
                        std::copy (block.getReadPointer (ch), block.getReadPointer (ch) + count,
                                   output[(size_t) ch].begin() + start);
                }

                const auto amplitudes = lowAndToneAmplitudes (output, sampleRate);
                lowToToneDb[(size_t) variant] = 20.0 * std::log10 (amplitudes.first / amplitudes.second);

                const auto caseName = juce::String (variant == 0 ? " (low cut Off)" : " (low cut Manual 60 Hz)");
                expect (allFinite (output), "output must be finite" + caseName);
                expect (mostSlices >= 2, "GLAZE 35 with 2048-sample host blocks must take the sliced path, slices="
                                             + juce::String (mostSlices) + caseName);
                logMessage ("  20 Hz / 1 kHz at GLAZE 35" + caseName + ": " + juce::String (lowToToneDb[(size_t) variant], 2)
                            + " dB, slices per block up to " + juce::String (mostSlices));
            }

            expect (lowToToneDb[0] - lowToToneDb[1] >= 25.0,
                    "Manual 60 Hz low cut must act on the sliced path: 20 Hz / 1 kHz dropped by "
                        + juce::String (lowToToneDb[0] - lowToToneDb[1], 2) + " dB (expected about 38 dB)");
        }

        // 8. 音频路径不分配（设计规格-总体「音频线程零分配、零锁」；设计规格-Clay-分析决策与验收 第 6 节第 2 条「音频线程 0 分配 0 锁」）：
        //    与 CeladonChain 的分配用例同一写法，这次 Clay 的七个设置每块都在变——三态在 自动 / 关 / 手动 间轮换（周期 7 / 11 / 13 块，
        //    互质，组合都走到）、手动值扫过全范围（频率对数、宽度线性）、Clay Bypass 与宿主旁通来回切、GLAZE 在 0 与扫动之间来回
        //    （GLAZE > 0 走 Guard 时隙切片、编排器每块合成 Clay 实际值并跑 ClayGuard）、块长 512 / 1 / 17 / 256 / 2048
        //    （2048 超过 prepare 的块大小，模块内部切块；块缓冲在计数区外按 2048 建好，setSize 不重新分配），每 100 块一次 restartWithFadeIn。
        //    直接 std::malloc 的 JUCE HeapBlock 不在计数内（见 TestMain.cpp 说明）。
        void testAudioPathDoesNotAllocateWithClay()
        {
            beginTest ("setParams / process / restartWithFadeIn never allocate while every Clay setting moves");

            constexpr double sampleRate = clayTestRate;
            constexpr int numBlocks = 600;
            constexpr int largestBlock = 2048;
            constexpr int blockSizes[] = { maxBlock, 1, 17, 256, largestBlock };

            CeladonChain chain;
            prepareChain (chain, sampleRate, makeNeutralParams (2.0f));

            const auto input = makeWideMaterial (sampleRate, 8.0, 61);
            const auto inputLength = (int) input.front().size();
            juce::AudioBuffer<float> block (numChannels, largestBlock);

            std::uint64_t allocations = 0;
            bool finite = true;
            int readPosition = 0;

            for (int b = 0; b < numBlocks; ++b)
            {
                // ---- 计数区外：准备这一块的参数与输入 ----
                const auto phase = (float) b / (float) numBlocks;   // 0 → 1
                const auto wave = 0.5f + 0.5f * std::sin (juce::MathConstants<float>::twoPi * 3.0f * phase);

                auto params = makeNeutralParams (2.0f);
                params.balance.recover = 40.0f;
                params.balance.tame = 40.0f;
                params.color.glue = 30.0f;
                params.color.heat = 30.0f;
                params.limit.gainDb = 6.0f;

                params.claySettings.lowCutMode = static_cast<ClayMode> ((b / 7) % 3);
                params.claySettings.monoMode   = static_cast<ClayMode> ((b / 11) % 3);
                params.claySettings.widthMode  = static_cast<ClayMode> ((b / 13) % 3);
                params.claySettings.lowCutHz = ClayModule::minLowCutHz * std::pow (ClayModule::maxLowCutHz / ClayModule::minLowCutHz, wave);
                params.claySettings.monoHz   = ClayModule::minMonoHz * std::pow (ClayModule::maxMonoHz / ClayModule::minMonoHz, phase);
                params.claySettings.widthDb  = ClayModule::minWidthDb + (ClayModule::maxWidthDb - ClayModule::minWidthDb) * wave;
                params.claySettings.bypass   = (b / 37) % 2 == 1;
                params.glazePercent = (b / 55) % 2 == 0 ? 0.0f : 100.0f * wave;

                const bool hostBypassed = (b / 90) % 2 == 1;
                const auto length = blockSizes[(size_t) b % std::size (blockSizes)];

                if (readPosition + length > inputLength)
                    readPosition = 0;

                block.setSize (numChannels, length, false, false, true);

                for (int ch = 0; ch < numChannels; ++ch)
                    block.copyFrom (ch, 0, input[(size_t) ch].data() + readPosition, length);

                readPosition += length;

                // ---- 计数区：只夹住链自己的入口 ----
                const auto before = allocationCountOnThisThread();

                if (b % 100 == 99)
                    chain.restartWithFadeIn();

                chain.setParams (params, hostBypassed);
                chain.process (block);
                juce::ignoreUnused (chain.getLatencySamples());

                allocations += allocationCountOnThisThread() - before;

                for (int ch = 0; ch < numChannels; ++ch)
                    for (int i = 0; i < length; ++i)
                        finite = finite && std::isfinite (block.getSample (ch, i));
            }

            expect (finite, "output must stay finite while every Clay setting moves");
            expectEquals ((juce::int64) allocations, (juce::int64) 0, "audio path must not allocate");
        }
    };

    static ChainClayTests chainClayTests;
}

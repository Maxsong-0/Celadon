/*
  ==============================================================================

    ChainTests.cpp
    CeladonChain（Balance → Color → Limit）的自动测试：延迟相加与脉冲实测、全中性 null、宿主旁通逐位、
    A / B 听原声与试听差值（v0.1.8.0）、
    restartWithFadeIn 的静音 / 淡入 / 回到逐位、整条链的真峰值仍不超 Ceiling、表的转发、
    音频路径（setParams / process / restartWithFadeIn / 表读取）不经 operator new 分配。

    只按设计规格-总体与 CeladonChain.h 的公开接口写，不依赖实现细节。

    v0.1.7.0 起链为 Balance → Clay → Color → Limit（ADR-009）：文件末尾追加测试类 CeladonChainClay——修坯 Clay 的链级接线
    （顺序、GLAZE 0 逐位直通、三态在 GLAZE 0 的接线、宿主旁通、延迟、重置、Guard 切片路径、音频路径不分配）。
    阶段 4（ADR-012 线性相位收单）起 Clay 有固定延迟，ADR-014 峰值守护起为 D + G（48 kHz 574 + 528 = 1102 样本；下文 CeladonChainClay
    里的「D」都指 ClayModule 上报的整个延迟 D + G，与 ClayTests.cpp 同口径）：上面的用例都按链自己上报的 getLatencySamples() 对齐，
    只有「延迟 = 各模块之和」把 Clay 加进了求和；CeladonChainClay 的逐位对照改成「旧链在 Clay 的位置插一段 D 样本纯延迟」，
    延迟断言改成 282 + D（48 / 96 kHz 为 1384 / 2584）。

    T-032（2026-09-29，A4-chain）起文件末尾再追加两个测试类（上面的用例一行不动）：CeladonChainMonitoring（退出预听不阶跃、
    听原声冻结 OUT 整体响度、预听中点表 Reset / 整链 reset 保住衰减、表对 NaN 自愈、实际参数无损回喂；只用改动前就有的接口）与
    CeladonChainGuards（表的暂停积分、发行端的读数保持、新实例默认值真源、非有限参数的链级守卫）。

  ==============================================================================
*/

#include "TestHelpers.h"

#include "../Source/dsp/CeladonChain.h"
#include "../Source/dsp/ClayGuard.h"

#include <algorithm>
#include <cstring>
#include <limits>

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
            for (const auto& component : { std::pair<double, float> { 55.0, 0.12f }, { 4600.0, 0.04f } })
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
            testCompareOriginal();
            testAuditionDifference();
            testAuditionDifferenceIsSuppressedAndReversible();
            testFrozenGuardDoesNotFragmentDsp();
        }

    private:
        // 界面的 A / B 对比（v0.1.8.0）：听 B（原声）时输出与宿主旁通逐位相同；Listen 测到的「处理结果 − 原声」响度差
        // 报给界面、听原声时冻结；Listen 打开时两边等响、都只衰减：处理结果更响时原声原样，原声更响时只把原声衰减下来。
        // 依据：CeladonChain.h 的 setParams / getListenDifferenceDb 说明。
        void testCompareOriginal()
        {
            beginTest ("compare-original plays the delayed dry input, freezes the Listen estimate and matches loudness by attenuation only");
            constexpr double sampleRate = 48000.0;
            const auto tone = makeSine (numChannels, sampleRate, 1000.0, 0.05f, 3.0);

            // ① 处理结果更响（Output +6 dB）：差值约 +6 dB；听原声逐位等于延迟后的输入（Listen 开着也不抬高原声）。
            {
                auto params = makeNeutralParams();
                params.color.outputDb = 6.0f;
                params.limit.listen = true;

                CeladonChain chain;
                prepareChain (chain, sampleRate, params);
                expect (! chain.hasListenDifference(), "no difference is reported before the first estimate");

                runChain (chain, tone, maxBlock);
                expect (chain.hasListenDifference(), "a difference is reported after 3 s of music");
                const auto processedLouder = chain.getListenDifferenceDb();
                logMessage ("  processed - original: " + juce::String (processedLouder, 3) + " dB (Output +6 dB)");
                expect (std::abs (processedLouder - 6.0f) < 0.2f, "difference follows the Output gain: " + juce::String (processedLouder, 3));

                chain.setParams (params, false, true);
                const auto original = runChain (chain, tone, 333);
                const auto latency = chain.getLatencySamples();
                float dryError = 0.0f;

                for (size_t ch = 0; ch < original.size(); ++ch)
                    for (size_t i = (size_t) (sampleRate / 4); i < original[ch].size(); ++i)
                        dryError = juce::jmax (dryError, std::abs (original[ch][i] - tone[ch][i - (size_t) latency]));

                expectEquals (dryError, 0.0f, "listening to the original returns the unattenuated delayed input");
                expectEquals (chain.getListenDifferenceDb(), processedLouder, "the estimate is frozen while the original plays");

                // 回到 A：Listen 照常把处理结果衰减到原声的响度，没有从零重新学（估计留着）。
                chain.setParams (params, false, false);
                const auto back = runChain (chain, tone, maxBlock);
                expect (allFinite (back));
                expect (std::abs (tailLufs (back, sampleRate) - tailLufs (tone, sampleRate)) < 0.15f, "A is matched again after the comparison");
            }

            // ② 原声更响（Output −6 dB）：Listen 打开时原声衰减到处理结果的响度；Listen 关着时原声原样（宿主旁通同理不补偿）。
            {
                auto params = makeNeutralParams();
                params.color.outputDb = -6.0f;
                params.limit.listen = true;

                CeladonChain chain;
                prepareChain (chain, sampleRate, params);
                const auto processed = runChain (chain, tone, maxBlock);
                const auto processedQuieter = chain.getListenDifferenceDb();
                expect (std::abs (processedQuieter + 6.0f) < 0.2f, "difference is negative when processing is quieter: " + juce::String (processedQuieter, 3));

                chain.setParams (params, false, true);
                const auto matchedOriginal = runChain (chain, tone, maxBlock);
                const auto matchError = tailLufs (matchedOriginal, sampleRate) - tailLufs (processed, sampleRate);
                logMessage ("  matched original vs processed: " + juce::String (matchError, 3) + " LU");
                expect (std::abs (matchError) < 0.15f, "the louder original is attenuated to the processed loudness");

                params.limit.listen = false;
                chain.setParams (params, false, true);
                const auto plainOriginal = runChain (chain, tone, maxBlock);
                expect (std::abs (tailLufs (plainOriginal, sampleRate) - tailLufs (tone, sampleRate)) < 0.05f, "without Listen the original is not matched");

                params.limit.listen = true;
                chain.setParams (params, true, true);
                const auto hostBypassed = runChain (chain, tone, maxBlock);
                expect (std::abs (tailLufs (hostBypassed, sampleRate) - tailLufs (tone, sampleRate)) < 0.05f, "host bypass is never compensated");
            }

            // ③ 默认参数（compareOriginal 省略）与显式 false 逐位相同：老调用方不受影响。
            {
                auto params = makeNeutralParams();
                params.limit.gainDb = 6.0f;
                CeladonChain implicitChain, explicitChain;
                implicitChain.setParams (params, false);
                implicitChain.prepare (makeSpec (sampleRate));
                implicitChain.setParams (params, false);
                explicitChain.setParams (params, false, false);
                explicitChain.prepare (makeSpec (sampleRate));
                explicitChain.setParams (params, false, false);
                const auto a = runChain (implicitChain, tone, maxBlock);
                const auto b = runChain (explicitChain, tone, maxBlock);
                expectEquals (maxAbsDifference (a, b, 0, (int) tone[0].size()), 0.0f);
            }
        }

        // 试听差值（v0.1.8.0）：送出去的是限幅器削掉的部分，所有测点照常按母带算。中性链（Balance / Clay / Color 都是纯延迟或直通）
        // 上「正常输出 + Gain × 试听输出」= 加了 Gain 的延迟输入；两条链的 OUT 表、整体响度、真峰值与 Listen 的估计逐位相同；
        // 宿主块超过 prepare 的块大小时（链按差值缓冲切片）与按 prepare 块大小送的结果逐位相同。依据：CeladonChain.h 的 setParams 说明。
        void testAuditionDifference()
        {
            beginTest ("difference audition swaps only the monitored signal: meters, loudness and Listen see the master");
            constexpr double sampleRate = 48000.0;
            const auto gainDb = 9.0f;
            const auto gain = juce::Decibels::decibelsToGain (gainDb);

            auto input = makeListenMaterial (sampleRate, 3.0);
            scale (input, 2.5f);   // 峰值约 0 dBFS：+9 dB 推进去结实地限幅

            // Listen 关着：正常链的输出就是限幅器的输出（下面拼回输入时不必扣监听增益）；估计照常在算。
            auto params = makeNeutralParams();
            params.limit.gainDb = gainDb;

            CeladonChain normal, audition;
            prepareChain (normal, sampleRate, params);
            audition.setParams (params, false, false, true);
            audition.prepare (makeSpec (sampleRate));
            audition.setParams (params, false, false, true);

            const auto master = runChain (normal, input, maxBlock);
            const auto removed = runChain (audition, input, maxBlock);
            const auto latency = normal.getLatencySamples();
            const auto length = (int) input.front().size();

            float reconstruction = 0.0f;
            for (int ch = 0; ch < numChannels; ++ch)
                for (int i = latency + (int) (sampleRate / 4); i < length; ++i)
                    reconstruction = juce::jmax (reconstruction, std::abs (master[(size_t) ch][(size_t) i] + gain * removed[(size_t) ch][(size_t) i]
                                                                           - gain * input[(size_t) ch][(size_t) (i - latency)]));

            logMessage ("  reconstruction error " + juce::String (toDb (reconstruction), 1) + " dBFS; original "
                        + juce::String (tailLufs (input, sampleRate), 2) + " LUFS, master " + juce::String (tailLufs (master, sampleRate), 2)
                        + " LUFS, difference " + juce::String (tailLufs (removed, sampleRate), 2) + " LUFS");
            expect (reconstruction < 1.0e-5f, "master + gain * difference must rebuild the gained input: " + juce::String (reconstruction));
            // 差值按推响前的电平：纯增益衰减时 = 输入 ×（1 − 衰减），比原声轻（这里 +9 dB 推进去、约 9.6 dB 衰减，已是极端设置）。
            expect (tailLufs (removed, sampleRate) < tailLufs (input, sampleRate) - 1.0f, "the difference is quieter than the original");
            expect (tailLufs (removed, sampleRate) > -40.0f, "the fixture must actually be limited");

            expectEquals (audition.getOutputIntegratedLufs(), normal.getOutputIntegratedLufs(), "integrated loudness is measured on the master");
            expectEquals (audition.getOutputShortTermLufs(), normal.getOutputShortTermLufs(), "short-term loudness is measured on the master");
            expectEquals (audition.getAndResetOutputTruePeakDb(), normal.getAndResetOutputTruePeakDb(), "OUT true peak is measured on the master");
            expectEquals (audition.getAndResetLimitGainReductionDb(), normal.getAndResetLimitGainReductionDb(), "GR meter is unchanged");
            expect (audition.hasListenDifference() && normal.hasListenDifference());
            expectEquals (audition.getListenDifferenceDb(), normal.getListenDifferenceDb(), "the Listen estimate keeps using the master");

            // 默认参数（auditionDifference 省略）与显式 false 逐位相同。
            {
                CeladonChain implicitChain, explicitChain;
                prepareChain (implicitChain, sampleRate, params);
                explicitChain.setParams (params, false, false, false);
                explicitChain.prepare (makeSpec (sampleRate));
                explicitChain.setParams (params, false, false, false);
                const auto a = runChain (implicitChain, input, maxBlock);
                const auto b = runChain (explicitChain, input, maxBlock);
                expectEquals (maxAbsDifference (a, b, 0, length), 0.0f, "omitted flag == explicit false");
                expectEquals (maxAbsDifference (a, master, 0, length), 0.0f, "no audition == the plain chain");
            }

            // 宿主块比 prepare 的块大小大（2048 > 512）：试听期间链按差值缓冲切片，结果与 512 的块逐位相同（GLAZE 0 时各模块分块无关）。
            {
                CeladonChain large;
                large.setParams (params, false, false, true);
                large.prepare (makeSpec (sampleRate));
                large.setParams (params, false, false, true);
                const auto sliced = runChain (large, input, 4 * maxBlock);
                expectEquals (maxAbsDifference (sliced, removed, 0, length), 0.0f, "oversized host blocks give the same difference");

                juce::AudioBuffer<float> big (numChannels, 4 * maxBlock);
                big.clear();
                large.process (big);
                expectEquals (large.getLastProcessSliceCountForTesting(), 4, "a 2048-sample block is sliced into four while auditioning");
            }

            // Listen 打开时差值也不乘监听增益（它本来就在推响前的电平）：试听输出与 Listen 关着时逐位相同。
            {
                auto listening = params;
                listening.limit.listen = true;
                CeladonChain chain;
                chain.setParams (listening, false, false, true);
                chain.prepare (makeSpec (sampleRate));
                chain.setParams (listening, false, false, true);
                const auto withListen = runChain (chain, input, maxBlock);
                expectEquals (maxAbsDifference (withListen, removed, 0, length), 0.0f, "the difference ignores the Listen gain");
            }
        }

        // 试听差值（续）：听原声、宿主旁通、LIMIT 旁通时不生效（20 ms 淡回，之后与不试听的链逐位相同）；
        // 中途打开再关掉，关掉 20 ms 之后输出又与从没试听过的链逐位相同（母带一直照常在算）；全程有限。
        void testAuditionDifferenceIsSuppressedAndReversible()
        {
            beginTest ("difference audition is suppressed by A / B, host bypass and LIMIT bypass, and fully reversible");
            constexpr double sampleRate = 48000.0;
            auto input = makeListenMaterial (sampleRate, 2.0);
            scale (input, 2.5f);
            const auto length = (int) input.front().size();
            const auto settle = (int) (0.05 * sampleRate);   // 20 ms 交叉淡化 + 余量

            auto params = makeNeutralParams();
            params.limit.gainDb = 9.0f;

            struct Case { const char* name; bool hostBypassed, compareOriginal, limitBypass; };
            for (const auto& c : { Case { "A / B original", false, true, false },
                                   Case { "host bypass", true, false, false },
                                   Case { "LIMIT bypass", false, false, true } })
            {
                auto caseParams = params;
                caseParams.limit.bypass = c.limitBypass;

                CeladonChain reference, audition;
                prepareChain (reference, sampleRate, params);
                prepareChain (audition, sampleRate, params);
                reference.setParams (params, false);
                audition.setParams (params, false, false, true);
                runChain (reference, input, maxBlock);
                runChain (audition, input, maxBlock);

                reference.setParams (caseParams, c.hostBypassed, c.compareOriginal);
                audition.setParams (caseParams, c.hostBypassed, c.compareOriginal, true);
                const auto a = runChain (reference, input, maxBlock);
                const auto b = runChain (audition, input, maxBlock);

                float afterFade = 0.0f;
                for (int ch = 0; ch < numChannels; ++ch)
                    for (int i = settle; i < length; ++i)
                        afterFade = juce::jmax (afterFade, std::abs (a[(size_t) ch][(size_t) i] - b[(size_t) ch][(size_t) i]));

                expectEquals (afterFade, 0.0f, juce::String ("audition must not apply during ") + c.name);
                expect (allFinite (b));
            }

            // 中途打开 0.5 s 再关掉：关掉之后（淡出走完）与从没试听的链逐位相同；试听期间确实换了输出。
            CeladonChain reference, audition;
            prepareChain (reference, sampleRate, params);
            prepareChain (audition, sampleRate, params);

            juce::AudioBuffer<float> a (numChannels, maxBlock), b (numChannels, maxBlock);
            const auto onAt = (int) (0.5 * sampleRate), offAt = (int) (1.0 * sampleRate);
            float duringAudition = 0.0f, afterAudition = 0.0f;
            bool finite = true;

            for (int start = 0; start < length; start += maxBlock)
            {
                const auto blockLength = juce::jmin (maxBlock, length - start);
                a.setSize (numChannels, blockLength, false, false, true);
                b.setSize (numChannels, blockLength, false, false, true);

                for (int ch = 0; ch < numChannels; ++ch)
                {
                    a.copyFrom (ch, 0, input[(size_t) ch].data() + start, blockLength);
                    b.copyFrom (ch, 0, input[(size_t) ch].data() + start, blockLength);
                }

                reference.setParams (params, false);
                audition.setParams (params, false, false, start >= onAt && start < offAt);
                reference.process (a);
                audition.process (b);

                for (int ch = 0; ch < numChannels; ++ch)
                    for (int i = 0; i < blockLength; ++i)
                    {
                        const auto delta = std::abs (a.getSample (ch, i) - b.getSample (ch, i));
                        finite = finite && std::isfinite (b.getSample (ch, i));

                        if (start + i >= onAt + settle && start + i < offAt)
                            duringAudition = juce::jmax (duringAudition, delta);
                        else if (start + i >= offAt + settle)
                            afterAudition = juce::jmax (afterAudition, delta);
                    }
            }

            expect (finite);
            expect (duringAudition > 0.05f, "the monitored signal changes while auditioning");
            expectEquals (afterAudition, 0.0f, "after the audition the output is bit-identical to a chain that never auditioned");
        }

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
                const bool audition = (b / 45) % 3 == 1;   // v0.1.8.0 试听差值：开 / 关的交叉淡化也在计数区里
                const auto length = blockSizes[(size_t) b % std::size (blockSizes)];

                block.setSize (numChannels, length, false, false, true);

                for (int ch = 0; ch < numChannels; ++ch)
                    block.copyFrom (ch, 0, input[(size_t) ch].data() + readPosition, length);

                readPosition = (readPosition + length) % (numBlocks * maxBlock - maxBlock);

                // ---- 计数区：只夹住链自己的入口 ----
                const auto before = allocationCountOnThisThread();

                if (b % 100 == 99)
                    chain.restartWithFadeIn();

                chain.setParams (params, hostBypassed, false, audition);
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
                const bool audition = (b / 45) % 3 == 1;   // 试听差值：含 2048 > prepare 块大小时的切片路径
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

                chain.setParams (params, hostBypassed, false, audition);
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

//==============================================================================
// T-032（2026-09-29，A4-chain）：监听、表与实际参数的修复用例（研究 / 核验：docs/validation/2026-09-29-T032-deep-research，
// 任务书 BRIEF-A4-chain.md）。两个测试类追加在文件末尾，上面的用例一行不动：
//   CeladonChainMonitoring —— 只用 e6b445b（0.1.8.5）就有的公开接口：同一段用例原样放回改动前的源码也能编。改前 16 条断言失败
//     （证明用例抓得住缺陷；「Listen 开时退出预听」与「reset 仍把抖动序列回到种子」是对照 / 回归项，改前改后都过），改后全部通过
//     （2026-10-01 在云端容器 GCC 13.3 上对照，数字见 IMPL-A4-chain.md；下面注释里的「改前实测」都出自这次对照）；
//   CeladonChainGuards —— 用到本次新增的接口（LoudnessMeter::setIntegrationPaused、ReleaseSafety 的保持、ParameterDefaults.h）
//     与非有限参数的链级用例（R8-02），放在文件最末（下面有分隔标记）。
namespace celadon::test
{
    namespace
    {
        constexpr double t032Rate = 48000.0;

        // 稳定的测试音：1 kHz 0.14 + 3.3 kHz 0.06（两声道相同）。一个 100 ms 子块正好 100 / 330 个周期，每个子块的 K 加权能量相同，
        // 整体响度读数稳定；母带 +6 dB（Color Output）后采样峰值 0.4（−8 dBFS），远低于 Ceiling −1 dBTP，限幅器不动作
        //（试听差值 = 限幅器削掉的部分 = 0）。
        Signal makeSteadyTone (double seconds)
        {
            auto tone = makeSine (numChannels, t032Rate, 1000.0, 0.14f, seconds);
            const auto upper = makeSine (numChannels, t032Rate, 3300.0, 0.06f, seconds, 0.7);

            for (size_t ch = 0; ch < tone.size(); ++ch)
                for (size_t i = 0; i < tone[ch].size(); ++i)
                    tone[ch][i] += upper[ch][i];

            return tone;
        }

        // 一块的监听状态：宿主旁通 / 听原声 / 试听差值（setParams 的后三个参数）。
        struct Monitor { bool hostBypassed = false, compareOriginal = false, auditionDifference = false; };

        // 从 start 起处理 length 个样本（块长 blockSize），每块先 setParams 再 process（与处理器相同：预听开关、A / B、旁通都在
        // setParams 里生效），输出写回 output 的同一位置。
        void processSpan (CeladonChain& chain, const CeladonChain::Params& p, const Signal& input, Signal& output,
                          int start, int length, int blockSize, Monitor monitor)
        {
            juce::AudioBuffer<float> block (numChannels, blockSize);

            for (int offset = start; offset < start + length; offset += blockSize)
            {
                const auto count = juce::jmin (blockSize, start + length - offset);
                block.setSize (numChannels, count, false, false, true);

                for (int ch = 0; ch < numChannels; ++ch)
                    block.copyFrom (ch, 0, input[(size_t) ch].data() + offset, count);

                chain.setParams (p, monitor.hostBypassed, monitor.compareOriginal, monitor.auditionDifference);
                chain.process (block);

                for (int ch = 0; ch < numChannels; ++ch)
                    std::copy (block.getReadPointer (ch), block.getReadPointer (ch) + count, output[(size_t) ch].begin() + offset);
            }
        }

        // 受测链对参照链的逐样本增益比（左声道；参照 |x| ≤ 0.03 的样本不相除，记 NaN——正弦过零处不相除，同上面「Listen switch must ramp」）。
        struct GainTrace
        {
            std::vector<float> ratio;

            // [from, to) 里相邻两个可用样本之间 |Δ增益| / Δ样本数 的最大值：每样本的增益变化率。阶跃 = 一个样本变很多。
            float maxSlope (int from, int to) const
            {
                float worst = 0.0f;
                int previous = -1;

                for (int i = juce::jmax (0, from); i < juce::jmin (to, (int) ratio.size()); ++i)
                {
                    if (std::isnan (ratio[(size_t) i]))
                        continue;

                    if (previous >= 0)
                        worst = juce::jmax (worst, std::abs (ratio[(size_t) i] - ratio[(size_t) previous]) / (float) (i - previous));

                    previous = i;
                }

                return worst;
            }

            // [from, to) 里可用样本的平均；一个都没有时返回 NaN。
            float mean (int from, int to) const
            {
                double sum = 0.0;
                int count = 0;

                for (int i = juce::jmax (0, from); i < juce::jmin (to, (int) ratio.size()); ++i)
                    if (! std::isnan (ratio[(size_t) i]))
                    {
                        sum += (double) ratio[(size_t) i];
                        ++count;
                    }

                return count > 0 ? (float) (sum / (double) count) : std::numeric_limits<float>::quiet_NaN();
            }
        };

        GainTrace gainTrace (const Signal& monitored, const Signal& reference)
        {
            GainTrace trace;
            trace.ratio.resize (reference[0].size());

            for (size_t i = 0; i < reference[0].size(); ++i)
                trace.ratio[i] = std::abs (reference[0][i]) > 0.03f ? monitored[0][i] / reference[0][i]
                                                                    : std::numeric_limits<float>::quiet_NaN();

            return trace;
        }

        // 预听场景的参数：母带 +6 dB（Color Output），预听目标 −20 LUFS → 稳态预听衰减约 −10 dB。
        CeladonChain::Params makePreviewParams (bool listen)
        {
            auto p = makeNeutralParams();
            p.color.outputDb = 6.0f;
            p.limit.listen = listen;
            p.mastering.previewTargetLufs = -20.0f;
            return p;
        }
    }

    class ChainMonitoringTests  : public juce::UnitTest
    {
    public:
        ChainMonitoringTests() : juce::UnitTest ("CeladonChainMonitoring", "Celadon") {}

        void runTest() override
        {
            testPreviewExitNeverSteps();
            testOriginalFreezesIntegrated();
            testMeterResetKeepsPreview();
            testResetKeepsPreviewGain();
            testDitherRestartsOnReset();
            testNonFiniteReachingMeters();
            testEffectiveParamsReplay();
        }

    private:
        // R8-01 / R10-03：退出归一化预听时监听增益不能单样本阶跃。受测链预听开着稳定 3 s，参照链从没开过预听；两条链在同一块做同样的
        // 切换（关预听 / 听原声 / 宿主旁通 / 试听差值），逐样本比「受测 / 参照」的增益：阶跃在一个样本里变很多（改前实测这组参数
        // ——稳态预听 −10.32 dB——下 0.695；研究在 −6 dB 下测到 0.499），斜坡每样本 < 0.01（沿用「Listen switch must ramp」的判据）。
        // Listen 关、链照常出母带时是发行端自己的 50 ms 斜坡（每样本的变化不超过 50 ms 线性斜坡的理论步长）；其余情形交给 Listen 的
        // 20 ms 平滑器。斜坡走完两条链监听同一个增益。
        // 宿主旁通：预听增益读数从第一个旁通块起是 0 dB（R10-03 核验保留的原意）。Listen 开的一行改前就交接给 Listen（对照）。
        void testPreviewExitNeverSteps()
        {
            struct ExitCase { const char* name; bool listen; Monitor after; bool userTurnsPreviewOff; bool rampsItself; };
            const ExitCase cases[] =
            {
                { "Listen off, preview switched off", false, {},                     true,  true  },
                { "Listen on, preview switched off",  true,  {},                     true,  false },
                { "A/B original",                     false, { false, true, false }, false, false },
                { "host bypass",                      false, { true, false, false }, false, false },
                { "difference audition",              false, { false, false, true }, false, false },
            };

            constexpr int blockSize = 128;
            const auto switchAt = (int) (3.0 * t032Rate);           // 128 的整数倍：切换落在块边界
            const auto total = switchAt + (int) (0.2 * t032Rate);
            const auto input = makeSteadyTone ((double) total / t032Rate);
            const auto window = (int) (0.05 * t032Rate);

            for (const auto& c : cases)
            {
                beginTest (juce::String ("leaving the normalisation preview never steps the monitor gain: ") + c.name);

                const auto p = makePreviewParams (c.listen);
                CeladonChain previewed, reference;
                prepareChain (previewed, t032Rate, p);
                prepareChain (reference, t032Rate, p);
                previewed.setReleasePreviewEnabled (true);

                auto a = makeSilence (numChannels, total);
                auto r = a;
                processSpan (previewed, p, input, a, 0, switchAt, blockSize, {});
                processSpan (reference, p, input, r, 0, switchAt, blockSize, {});
                const auto steadyGainDb = previewed.getMasteringStatus().previewGainDb;

                if (c.userTurnsPreviewOff)
                    previewed.setReleasePreviewEnabled (false);

                processSpan (previewed, p, input, a, switchAt, blockSize, blockSize, c.after);
                processSpan (reference, p, input, r, switchAt, blockSize, blockSize, c.after);
                const auto gainDbAfterSwitch = previewed.getMasteringStatus().previewGainDb;

                processSpan (previewed, p, input, a, switchAt + blockSize, total - switchAt - blockSize, blockSize, c.after);
                processSpan (reference, p, input, r, switchAt + blockSize, total - switchAt - blockSize, blockSize, c.after);

                const auto trace = gainTrace (a, r);
                const auto before = trace.mean (switchAt - window, switchAt);
                const auto slope = trace.maxSlope (switchAt - window, total);
                const auto settled = trace.mean (switchAt + (int) ((c.rampsItself ? 0.07 : 0.04) * t032Rate), total);
                logMessage ("  " + juce::String (c.name) + ": steady preview " + juce::String (steadyGainDb, 2) + " dB, gain ratio before "
                            + juce::String (before, 4) + ", after " + juce::String (settled, 4) + ", max change per sample "
                            + juce::String (slope, 6));

                expect (steadyGainDb < -3.0f, "the preview really attenuated before the switch: " + juce::String (steadyGainDb, 2) + " dB");
                expect (slope < 0.01f, juce::String (c.name) + ": the monitor gain must ramp, max change per sample "
                                           + juce::String (slope, 6));

                if (c.after.auditionDifference)
                {
                    // 差值 = 限幅器削掉的部分（这里为 0）：交叉淡化走完两条链都送出差值，逐样本相同。
                    float tail = 0.0f;
                    const auto tailStart = switchAt + (int) (0.1 * t032Rate);

                    for (int ch = 0; ch < numChannels; ++ch)
                        for (int i = tailStart; i < total; ++i)
                            tail = juce::jmax (tail, std::abs (a[(size_t) ch][(size_t) i] - r[(size_t) ch][(size_t) i]));

                    expect (tail < 1.0e-6f, "after the cross-fade both chains send the same difference signal, max diff " + juce::String (tail));
                }
                else
                {
                    expect (std::abs (settled - 1.0f) < 0.01f, juce::String (c.name) + ": after the hand-over both chains monitor the same gain, ratio "
                                                                   + juce::String (settled, 4));
                }

                if (c.rampsItself)
                {
                    // Listen 关、在线、链照常出母带：发行端自己的 50 ms 线性斜坡（ReleaseSafety::prepare 的 0.05 s）。
                    const auto theory = (1.0f - juce::Decibels::decibelsToGain (steadyGainDb)) / (float) (0.05 * t032Rate);
                    expect (slope <= theory * 1.05f + 1.0e-5f, "the 50 ms release-safety ramp, not a faster one: "
                                                                  + juce::String (slope, 7) + " > " + juce::String (theory, 7));
                }

                if (c.after.hostBypassed)
                    expectEquals (gainDbAfterSwitch, 0.0f, "host bypass: the preview gain reads 0 dB from the first bypassed block");
            }
        }

        // R8-03：听原声（A / B 的 B）时 OUT 表的整体响度只积分母带——母带 6 s → 原声 6 s → 母带 6 s：原声期间整体读数与切换前相差
        // < 0.01 LU（改前实测被原声拉低 2.03 LU），短期读数是此刻听到的原声（与单独量原声的表相差 < 0.1 LU）；回到母带后整体读数
        // 接着母带的值；OUT 表的样本计数一直在走（子块环不断，校准器与 Listen 的配对不经直方图）。宿主旁通见 CeladonChainGuards。
        void testOriginalFreezesIntegrated()
        {
            beginTest ("A/B original freezes the OUT integrated loudness at the master's value; short-term shows what is heard");

            auto p = makeNeutralParams();
            p.color.outputDb = 6.0f;
            const auto six = (int) (6.0 * t032Rate);
            const auto input = makeSteadyTone (18.0);
            auto output = makeSilence (numChannels, 3 * six);

            CeladonChain chain;
            prepareChain (chain, t032Rate, p);
            processSpan (chain, p, input, output, 0, six, maxBlock, {});
            const auto masterIntegrated = chain.getOutputIntegratedLufs();

            processSpan (chain, p, input, output, six, six, maxBlock, { false, true, false });
            const auto originalIntegrated = chain.getOutputIntegratedLufs();
            const auto originalShortTerm = chain.getOutputShortTermLufs();

            processSpan (chain, p, input, output, 2 * six, six, maxBlock, {});
            const auto backIntegrated = chain.getOutputIntegratedLufs();

            LoudnessMeter dry;
            dry.prepare (makeSpec (t032Rate));
            juce::AudioBuffer<float> block (numChannels, maxBlock);

            for (int start = 0; start < six; start += maxBlock)
            {
                const auto count = juce::jmin (maxBlock, six - start);
                block.setSize (numChannels, count, false, false, true);

                for (int ch = 0; ch < numChannels; ++ch)
                    block.copyFrom (ch, 0, input[(size_t) ch].data() + start, count);

                dry.process (block);
            }

            logMessage ("  master " + juce::String (masterIntegrated, 3) + " LUFS; while listening to the original: integrated "
                        + juce::String (originalIntegrated, 3) + ", short-term " + juce::String (originalShortTerm, 3) + " (original alone "
                        + juce::String (dry.getShortTermLufs(), 3) + "); back on the master " + juce::String (backIntegrated, 3));

            expect (masterIntegrated - dry.getIntegratedLufs() > 5.0f, "the master really is louder than the original (+6 dB)");
            expect (std::abs (originalIntegrated - masterIntegrated) < 0.01f,
                    "listening to the original must not pull the integrated reading: "
                    + juce::String (originalIntegrated - masterIntegrated, 4) + " LU");
            expect (std::abs (originalShortTerm - dry.getShortTermLufs()) < 0.1f, "short-term shows the original that is heard");
            expect (std::abs (backIntegrated - masterIntegrated) < 0.01f,
                    "back on the master the integrated reading continues from the master's value: "
                    + juce::String (backIntegrated - masterIntegrated, 4) + " LU");
            expect (chain.getOutputMeter().getProcessedSamples() == (std::int64_t) (3 * six),
                    "the OUT meter's sample clock keeps running through the original");
        }

        // R8-04：预听中点表 Reset，整体读数要 400 ms 才回来——这段时间预听保持上一次的衰减，不回 0 dB 再落回（改前实测这组参数下
        // Reset 后预听增益回到 0.000 dB，稳态 −10.32 dB；研究在 −6 dB 下测到 0.4 s 的 +6 dB）：Reset 后 2 s 内预听增益最高不超过
        // 稳态 + 0.5 dB。从未有过读数时照旧是 0 dB（头 300 ms）。
        void testMeterResetKeepsPreview()
        {
            beginTest ("a meter Reset during the preview keeps the attenuation instead of jumping back to 0 dB for 0.4 s");

            const auto p = makePreviewParams (false);
            const auto input = makeSteadyTone (5.0);
            auto output = makeSilence (numChannels, (int) input[0].size());
            constexpr int blockSize = 128;

            CeladonChain chain;
            prepareChain (chain, t032Rate, p);
            chain.setReleasePreviewEnabled (true);

            const auto noReadingEnd = (int) (0.3 * t032Rate) / blockSize * blockSize;
            processSpan (chain, p, input, output, 0, noReadingEnd, blockSize, {});
            const auto noReadingGainDb = chain.getMasteringStatus().previewGainDb;

            const auto resetAt = (int) (3.0 * t032Rate);
            processSpan (chain, p, input, output, noReadingEnd, resetAt - noReadingEnd, blockSize, {});
            const auto steadyGainDb = chain.getMasteringStatus().previewGainDb;

            chain.requestMeterReset();
            auto highest = -1000.0f;

            for (int start = resetAt; start < resetAt + (int) (2.0 * t032Rate); start += blockSize)
            {
                processSpan (chain, p, input, output, start, blockSize, blockSize, {});
                highest = juce::jmax (highest, chain.getMasteringStatus().previewGainDb);
            }

            logMessage ("  no reading yet " + juce::String (noReadingGainDb, 3) + " dB; steady " + juce::String (steadyGainDb, 3)
                        + " dB; highest in the 2 s after Reset " + juce::String (highest, 3) + " dB");

            expectEquals (noReadingGainDb, 0.0f, "never had a reading: the preview stays at unity");
            expect (steadyGainDb < -3.0f, "the preview really attenuated");
            expect (highest <= steadyGainDb + 0.5f, "Reset must not bring the preview back to unity: highest "
                                                        + juce::String (highest, 3) + " dB, steady " + juce::String (steadyGainDb, 3) + " dB");
        }

        // 核验漏项：同规格 prepareToPlay（setParams + reset）与播放中途的 restartWithFadeIn 都不把预听增益归一（改前先回 0 dB 再 50 ms
        // 线性滑回：改前实测这组参数下之后 0.5 s 的预听增益读数最高 −0.33 dB，稳态 −10.32 dB）。两块表不清、整体读数还在，衰减接着生效。
        void testResetKeepsPreviewGain()
        {
            for (int variant = 0; variant < 2; ++variant)
            {
                beginTest (variant == 0 ? "same-spec prepareToPlay (setParams + reset) keeps the preview attenuation"
                                        : "restartWithFadeIn keeps the preview attenuation");

                const auto p = makePreviewParams (false);
                const auto input = makeSteadyTone (4.0);
                auto output = makeSilence (numChannels, (int) input[0].size());
                constexpr int blockSize = 128;

                CeladonChain chain;
                prepareChain (chain, t032Rate, p);
                chain.setReleasePreviewEnabled (true);

                const auto resetAt = (int) (3.0 * t032Rate);
                processSpan (chain, p, input, output, 0, resetAt, blockSize, {});
                const auto steadyGainDb = chain.getMasteringStatus().previewGainDb;

                if (variant == 0)
                {
                    chain.setParams (p, false);
                    chain.reset();
                }
                else
                {
                    chain.restartWithFadeIn();
                }

                auto highest = -1000.0f;

                for (int start = resetAt; start < resetAt + (int) (0.5 * t032Rate); start += blockSize)
                {
                    processSpan (chain, p, input, output, start, blockSize, blockSize, {});
                    highest = juce::jmax (highest, chain.getMasteringStatus().previewGainDb);
                }

                logMessage ("  steady " + juce::String (steadyGainDb, 3) + " dB; highest in the 0.5 s after "
                            + (variant == 0 ? juce::String ("reset ") : juce::String ("restartWithFadeIn ")) + juce::String (highest, 3) + " dB");
                expect (steadyGainDb < -3.0f, "the preview really attenuated");
                expect (highest <= steadyGainDb + 0.1f, "the preview attenuation survives: highest " + juce::String (highest, 3) + " dB");
                expect (chain.isReleasePreviewEnabled(), "the preview is still on");
            }
        }

        // 发行端的 reset 拆成两半之后（核验漏项），整链 reset 仍要把 TPDF 随机序列回到固定种子：抖动开着时，跑过一段再 reset 的链与
        // 新 prepare 的链逐位相同（导出可复现）。
        void testDitherRestartsOnReset()
        {
            beginTest ("reset still restarts the TPDF sequence: with dither on, reset after running == fresh prepare, bit-exact");

            auto p = makeNeutralParams();
            p.limit.gainDb = 3.0f;
            p.mastering.dither = DitherMode::bits24;

            const auto warmUp = makeNoise (numChannels, (int) (1.0 * t032Rate), 0.4f, 31);
            const auto input = makeNoise (numChannels, (int) (1.0 * t032Rate), 0.4f, 32);

            CeladonChain used, fresh;
            prepareChain (used, t032Rate, p);
            runChain (used, warmUp, maxBlock);
            used.setParams (p, false);
            used.reset();
            prepareChain (fresh, t032Rate, p);

            expect (bitIdentical (runChain (used, input, maxBlock), runChain (fresh, input, maxBlock)),
                    "dithered output after reset must equal a freshly prepared chain");
        }

        // R10-08：链自己不管输入（守卫在处理器），但一个 NaN 进了表不能永久污染它。NaN 之后都把信号推响 +6 dB：读数要跟着走，
        // 冻结在出事前的值就过不了（改前正是这样——整体读数停在出事前、短期读数从此 −100「无读数」，本容器对照实测见 IMPL-A4-chain.md）。
        //   - 单独一块 LoudnessMeter：稳定音 2 s → 含 NaN 的一块 → 同一个音 +6 dB 4 s：短期读数等于单独量 +6 dB 那段的表
        //     （差 < 0.1 LU），整体读数比出事前高出 3 LU 以上（两段按能量平均约高 4.9 LU）；
        //   - 整条链（GLAZE 35，聆听中）：中途一块含 NaN——它过完链延迟到达输出的那一块被清零、整链重启淡入（链的输出守卫，改动前就有），
        //     输出全部有限；之后 1 s 内 IN 表短期读数恢复，结尾时 IN 表整体读数比出事前高出 2 LU 以上（后面那段 +6 dB），
        //     GLAZE 聆听继续推进（改前 IN 表每个子块都是 NaN，分析器全部跳过，聆听时长停住不再增加）。
        void testNonFiniteReachingMeters()
        {
            beginTest ("a NaN reaching a LoudnessMeter no longer poisons it: short-term and integrated keep following the signal");
            {
                LoudnessMeter meter, louderAlone;
                meter.prepare (makeSpec (t032Rate));
                louderAlone.prepare (makeSpec (t032Rate));
                const auto tone = makeSteadyTone (2.5);   // 只用到 NaN 那一块的末尾（约 2.005 s）
                auto louder = makeSteadyTone (4.0);
                scale (louder, 2.0f);   // +6.02 dB
                juce::AudioBuffer<float> block (numChannels, maxBlock);
                const auto nanBlockStart = (int) (2.0 * t032Rate) / maxBlock * maxBlock;

                const auto feed = [&block] (LoudnessMeter& target, const Signal& source, int start, int length, bool poison)
                {
                    const auto count = juce::jmin (maxBlock, length);
                    block.setSize (numChannels, count, false, false, true);

                    for (int ch = 0; ch < numChannels; ++ch)
                        block.copyFrom (ch, 0, source[(size_t) ch].data() + start, count);

                    if (poison)
                        block.setSample (0, 100, std::numeric_limits<float>::quiet_NaN());

                    target.process (block);
                };

                for (int start = 0; start < nanBlockStart; start += maxBlock)
                    feed (meter, tone, start, nanBlockStart - start, false);

                const auto beforeIntegrated = meter.getIntegratedLufs();
                const auto beforeShortTerm = meter.getShortTermLufs();
                feed (meter, tone, nanBlockStart, maxBlock, true);

                for (int start = 0; start < (int) louder[0].size(); start += maxBlock)
                {
                    feed (meter, louder, start, (int) louder[0].size() - start, false);
                    feed (louderAlone, louder, start, (int) louder[0].size() - start, false);
                }

                const auto integrated = meter.getIntegratedLufs();
                const auto shortTerm = meter.getShortTermLufs();
                logMessage ("  meter before the NaN: I " + juce::String (beforeIntegrated, 3) + ", S " + juce::String (beforeShortTerm, 3)
                            + "; 4 s of +6 dB after it: I " + juce::String (integrated, 3) + ", S " + juce::String (shortTerm, 3)
                            + " (the +6 dB part alone: S " + juce::String (louderAlone.getShortTermLufs(), 3) + ")");
                expect (std::isfinite (beforeIntegrated) && beforeIntegrated > -70.0f, "the meter had a reading before the NaN");
                expect (std::isfinite (shortTerm) && std::abs (shortTerm - louderAlone.getShortTermLufs()) < 0.1f,
                        "short-term loudness follows the signal after a NaN sample");
                expect (std::isfinite (integrated) && integrated > beforeIntegrated + 3.0f,
                        "integrated loudness keeps integrating after a NaN sample (not frozen at the old value)");
            }

            beginTest ("a NaN input block no longer stalls the chain's IN meter and GLAZE listening");
            {
                auto p = makeNeutralParams();
                p.glazePercent = 35.0f;
                auto input = makeWideMaterial (t032Rate, 8.0, 41);
                auto output = makeSilence (numChannels, (int) input[0].size());
                const auto nanAt = (int) (3.0 * t032Rate) / maxBlock * maxBlock;

                // NaN 那一块之后整段 +6 dB：IN 表的整体读数要跟着抬高（冻结就过不了）。
                for (auto& channel : input)
                    for (size_t i = (size_t) (nanAt + maxBlock); i < channel.size(); ++i)
                        channel[i] *= 2.0f;

                CeladonChain chain;
                prepareChain (chain, t032Rate, p);
                processSpan (chain, p, input, output, 0, nanAt, maxBlock, {});
                const auto inIntegratedBefore = chain.getInputMeter().getIntegratedLufs();

                auto poisoned = input;
                poisoned[0][(size_t) nanAt + 17] = std::numeric_limits<float>::quiet_NaN();
                processSpan (chain, p, poisoned, output, nanAt, maxBlock, maxBlock, {});

                const auto oneSecond = (int) (1.0 * t032Rate) / maxBlock * maxBlock;
                processSpan (chain, p, input, output, nanAt + maxBlock, oneSecond, maxBlock, {});
                const auto inShortTerm = chain.getInputMeter().getShortTermLufs();
                const auto listenedAfterOne = chain.getGlazeStatus().listenedSeconds;

                const auto rest = (int) input[0].size() - (nanAt + maxBlock + oneSecond);
                processSpan (chain, p, input, output, nanAt + maxBlock + oneSecond, rest, maxBlock, {});
                const auto inIntegratedAtEnd = chain.getInputMeter().getIntegratedLufs();
                const auto listenedAtEnd = chain.getGlazeStatus().listenedSeconds;

                // NaN 在链延迟之后才到输出：到达的那一块被清零、整链重启——先静音整条链的延迟，再 10 ms 淡入（链的输出守卫）。
                const auto latency = chain.getLatencySamples();
                int longestSilence = 0, run = 0;

                for (int i = nanAt; i < juce::jmin ((int) output[0].size(), nanAt + 2 * latency + 4 * maxBlock); ++i)
                {
                    run = (output[0][(size_t) i] == 0.0f && output[1][(size_t) i] == 0.0f) ? run + 1 : 0;
                    longestSilence = juce::jmax (longestSilence, run);
                }

                logMessage ("  IN I before the NaN block " + juce::String (inIntegratedBefore, 3) + "; 1 s after: IN S " + juce::String (inShortTerm, 3)
                            + "; at the end (+6 dB since the NaN): IN I " + juce::String (inIntegratedAtEnd, 3) + "; GLAZE listened "
                            + juce::String (listenedAfterOne, 2) + " s → " + juce::String (listenedAtEnd, 2) + " s at the end");
                expect (allFinite (output), "every output sample is finite");
                expect (longestSilence >= latency, "the block the NaN reaches is cleared and the chain restarts: "
                                                       + juce::String (longestSilence) + " silent samples, latency " + juce::String (latency));
                expect (std::isfinite (inShortTerm) && inShortTerm > -70.0f, "the IN meter's short-term reading is back within 1 s");
                expect (std::isfinite (inIntegratedAtEnd) && inIntegratedAtEnd > inIntegratedBefore + 2.0f,
                        "the IN meter's integrated reading keeps integrating after the NaN block (+6 dB pulls it up)");
                expect (listenedAtEnd > listenedAfterOne + 2.0f, "GLAZE keeps listening after the NaN block");
            }
        }

        // R10-01：getEffectiveParams() 回写的是交给模块的实际值——GLAZE 35、推响不刺 / 目标频谱两项「自动」、编码余量 0.5 dB 的链，
        // 逐块把它的实际参数喂给一条 GLAZE 0 的链（离线工具的同决策重放，ADR-016），输出逐位相同（改前 mastering 回写的是原始设置：
        // GLAZE 0 的链把两项「自动」算成 0，编码余量再扣一次）。对照有牙：把 mastering 换回原始设置重放，输出明显不同。
        void testEffectiveParamsReplay()
        {
            beginTest ("getEffectiveParams() fed back into a GLAZE 0 chain reproduces GLAZE 35 + automatic advanced mastering + codec reserve, bit for bit");

            auto source = makeNeutralParams();
            source.glazePercent = 35.0f;
            source.limit.gainDb = 4.0f;
            source.mastering.harshMode = HarshMode::automatic;
            source.mastering.harshAmount = 50.0f;
            source.mastering.transientPreserve = 50.0f;
            source.mastering.targetMode = TargetMode::automatic;
            source.mastering.targetAmount = 35.0f;
            source.mastering.codecMarginDb = 0.5f;
            source.claySettings.lowCutMode = source.claySettings.monoMode = source.claySettings.widthMode = ClayMode::off;

            const auto input = makeWideMaterial (t032Rate, 6.0, 43);
            const auto numSamples = (int) input[0].size();
            std::vector<CeladonChain::Params> executed;
            auto automatic = makeSilence (numChannels, numSamples);

            {
                CeladonChain chain;
                prepareChain (chain, t032Rate, source);
                juce::AudioBuffer<float> block (numChannels, maxBlock);

                for (int start = 0; start < numSamples; start += maxBlock)
                {
                    const auto count = juce::jmin (maxBlock, numSamples - start);
                    block.setSize (numChannels, count, false, false, true);

                    for (int ch = 0; ch < numChannels; ++ch)
                        block.copyFrom (ch, 0, input[(size_t) ch].data() + start, count);

                    chain.setParams (source, false);
                    executed.push_back (chain.getEffectiveParams());
                    chain.process (block);

                    for (int ch = 0; ch < numChannels; ++ch)
                        std::copy (block.getReadPointer (ch), block.getReadPointer (ch) + count, automatic[(size_t) ch].begin() + start);
                }
            }

            const auto replay = [&input, numSamples] (const std::vector<CeladonChain::Params>& executedLog, bool rawMastering,
                                                      const MasteringSettings& raw)
            {
                const auto asInput = [&] (CeladonChain::Params p)
                {
                    p.glazePercent = 0.0f;                  // 编排器不决策：实际参数 = 喂进去的参数
                    p.limit.listen = false;
                    p.clay = ClayModule::Params {};         // 作为输入时编排器忽略（ChainParams.h）

                    if (rawMastering)
                        p.mastering = raw;

                    return p;
                };

                auto output = makeSilence (numChannels, numSamples);
                CeladonChain chain;
                prepareChain (chain, t032Rate, asInput (executedLog.front()));
                juce::AudioBuffer<float> block (numChannels, maxBlock);
                std::size_t index = 0;

                for (int start = 0; start < numSamples; start += maxBlock, ++index)
                {
                    const auto count = juce::jmin (maxBlock, numSamples - start);
                    block.setSize (numChannels, count, false, false, true);

                    for (int ch = 0; ch < numChannels; ++ch)
                        block.copyFrom (ch, 0, input[(size_t) ch].data() + start, count);

                    chain.setParams (asInput (executedLog[juce::jmin (index, executedLog.size() - 1)]), false);
                    chain.process (block);

                    for (int ch = 0; ch < numChannels; ++ch)
                        std::copy (block.getReadPointer (ch), block.getReadPointer (ch) + count, output[(size_t) ch].begin() + start);
                }

                return output;
            };

            const auto replayed = replay (executed, false, source.mastering);
            const auto rawReplayed = replay (executed, true, source.mastering);

            const auto masteringBlocks = std::count_if (executed.begin(), executed.end(), [] (const CeladonChain::Params& e)
            {
                return e.balance.harshAmount > 0.0f && e.balance.targetAmount > 0.0f;
            });

            logMessage ("  " + juce::String ((int) executed.size()) + " blocks, advanced mastering active in "
                        + juce::String ((int) masteringBlocks) + "; executed ceiling " + juce::String (executed.back().limit.ceilingDb, 3)
                        + " dBTP; replay max diff " + juce::String (maxAbsDifferenceWhole (automatic, replayed))
                        + ", replay with the raw settings max diff " + juce::String (maxAbsDifferenceWhole (automatic, rawReplayed)));

            expect (masteringBlocks > 0, "advanced mastering really acts in the GLAZE 35 chain");
            expectEquals (executed.back().limit.ceilingDb, -1.5f, "the executed Ceiling already includes the 0.5 dB codec reserve");
            expect (bitIdentical (automatic, replayed), "feeding the executed params back reproduces the chain bit for bit");
            expect (maxAbsDifferenceWhole (automatic, rawReplayed) > 1.0e-6f,
                    "control: replaying the raw mastering settings does not reproduce it (the resolved amounts are what make it exact)");
        }
    };

    static ChainMonitoringTests chainMonitoringTests;
}

//==============================================================================
// ---- T-032 CeladonChainGuards（用到本次新增的接口；对照改动前的源码时从这一行起整段去掉）----
#include "../Source/ParameterDefaults.h"
#include "../Source/ParameterRanges.h"

namespace celadon::test
{
    namespace
    {
        // Color / Limit 的非有限参数守卫在 A2 / A3 线（ColorModule::setParams / LimiterModule::setParams）。A2 / A3 已与本线合在一起（T-032 集成），
        // 用例对这两组字段同样判定（T-032 R8-02）；只在单独检出 A4 线、没有 A2 / A3 时才需要改回 false。
        constexpr bool colorLimitGuardsMerged = true;

        // 在 ReleaseSafety 上按固定读数跑 seconds 秒（块长 127，两声道 1 kHz 0.4）。
        void renderWithReading (ReleaseSafety& module, float lufs, double seconds)
        {
            juce::AudioBuffer<float> block (numChannels, 127);

            for (int done = 0; done < (int) (seconds * t032Rate); done += block.getNumSamples())
            {
                for (int ch = 0; ch < numChannels; ++ch)
                    for (int i = 0; i < block.getNumSamples(); ++i)
                        block.setSample (ch, i, 0.4f * (float) std::sin (juce::MathConstants<double>::twoPi * 1000.0 * (done + i) / t032Rate));

                module.process (block, lufs);
            }
        }

        // 把 input 的 [start, start + length) 送进一块表（块长 blockSize）。
        void meterSpan (LoudnessMeter& meter, const Signal& input, int start, int length, int blockSize)
        {
            juce::AudioBuffer<float> block (numChannels, blockSize);

            for (int offset = start; offset < start + length; offset += blockSize)
            {
                const auto count = juce::jmin (blockSize, start + length - offset);
                block.setSize (numChannels, count, false, false, true);

                for (int ch = 0; ch < numChannels; ++ch)
                    block.copyFrom (ch, 0, input[(size_t) ch].data() + offset, count);

                meter.process (block);
            }
        }
    }

    class ChainGuardsTests  : public juce::UnitTest
    {
    public:
        ChainGuardsTests() : juce::UnitTest ("CeladonChainGuards", "Celadon") {}

        void runTest() override
        {
            testHostBypassIntegrationPolicy();
            testPauseExcludesTouchedSubBlocks();
            testReleaseSafetyHold();
            testFactoryDefaults();
            testNonFiniteParams();
        }

    private:
        // R8-03 的宿主旁通一半接在同一个开关上，口径待 Max 确认（CeladonChain::pauseIntegrationOnHostBypass）：
        // 现在（false）宿主旁通期间照旧积分——整体读数被原声拉低；改成 true 后与听原声一样冻结。
        void testHostBypassIntegrationPolicy()
        {
            beginTest (juce::String ("host bypass: the OUT integrated loudness ")
                       + (CeladonChain::pauseIntegrationOnHostBypass ? "freezes (confirmed policy)" : "keeps integrating until Max confirms the freeze"));

            auto p = makeNeutralParams();
            p.color.outputDb = 6.0f;
            const auto six = (int) (6.0 * t032Rate);
            const auto input = makeSteadyTone (12.0);
            auto output = makeSilence (numChannels, 2 * six);

            CeladonChain chain;
            prepareChain (chain, t032Rate, p);
            processSpan (chain, p, input, output, 0, six, maxBlock, {});
            const auto masterIntegrated = chain.getOutputIntegratedLufs();
            processSpan (chain, p, input, output, six, six, maxBlock, { true, false, false });
            const auto bypassedIntegrated = chain.getOutputIntegratedLufs();

            logMessage ("  master " + juce::String (masterIntegrated, 3) + " LUFS, after 6 s of host bypass " + juce::String (bypassedIntegrated, 3));

            if constexpr (CeladonChain::pauseIntegrationOnHostBypass)
                expect (std::abs (bypassedIntegrated - masterIntegrated) < 0.01f, "host bypass freezes the integrated reading");
            else
                expect (bypassedIntegrated < masterIntegrated - 0.5f, "host bypass still integrates the dry signal (policy pending)");
        }

        // LoudnessMeter::setIntegrationPaused：只要有一个样本是在暂停中处理的，这个 100 ms 子块就不进任何门块——包括暂停开始 / 结束
        // 那一块里跨边界的半截子块。暂停期间送一段 +20 dB 的突发：漏进一个门块，整体读数就会抬高约 0.8 LU；全挡住时整体读数与
        // 只听稳定音的表相差 < 0.01 LU。三种暂停：短于一个子块且落在子块中间（暂停里一个子块也没结束）、跨很多子块且两头都在子块
        // 中间、两头正好对齐子块边界。另核：短期读数照常反映暂停期间的突发；子块环不受暂停影响（时间戳连续）。
        void testPauseExcludesTouchedSubBlocks()
        {
            struct PauseCase { const char* name; int start; int length; };
            constexpr int sub = 4800;   // 48 kHz 的 100 ms 子块
            const PauseCase cases[] =
            {
                { "short pause inside one sub-block", 50 * sub + 1234, 1000 },
                { "long pause, both edges mid-sub-block", 50 * sub + 2000, 13 * sub + 777 },
                { "pause aligned to sub-block boundaries", 50 * sub, 3 * sub },
            };

            const auto tone = makeSteadyTone (12.0);
            auto burst = tone;
            scale (burst, 10.0f);   // +20 dB

            LoudnessMeter reference;
            reference.prepare (makeSpec (t032Rate));
            meterSpan (reference, tone, 0, (int) tone[0].size(), 512);

            for (const auto& c : cases)
            {
                beginTest (juce::String ("setIntegrationPaused keeps every touched sub-block out of the integrated loudness: ") + c.name);

                LoudnessMeter meter;
                meter.prepare (makeSpec (t032Rate));
                meterSpan (meter, tone, 0, c.start, 512);
                const auto shortTermBefore = meter.getShortTermLufs();

                meter.setIntegrationPaused (true);
                meterSpan (meter, burst, c.start, c.length, 512);
                const auto shortTermDuring = meter.getShortTermLufs();
                meter.setIntegrationPaused (false);

                meterSpan (meter, tone, c.start + c.length, (int) tone[0].size() - c.start - c.length, 512);

                std::int64_t cursor = 0;
                std::vector<LoudnessMeter::SubBlock> blocks ((size_t) LoudnessMeter::subBlockRingCapacity);
                const auto got = meter.readSubBlocksSince (cursor, blocks.data(), (int) blocks.size());
                auto contiguous = got > 0;

                for (int i = 1; i < juce::jmax (0, got); ++i)
                    contiguous = contiguous && blocks[(size_t) i].endSample - blocks[(size_t) i - 1].endSample == (std::int64_t) sub;

                logMessage ("  " + juce::String (c.name) + ": integrated " + juce::String (meter.getIntegratedLufs(), 4) + " (tone only "
                            + juce::String (reference.getIntegratedLufs(), 4) + "), short-term before / during the pause "
                            + juce::String (shortTermBefore, 2) + " / " + juce::String (shortTermDuring, 2));

                expect (std::abs (meter.getIntegratedLufs() - reference.getIntegratedLufs()) < 0.01f,
                        juce::String (c.name) + ": nothing from the pause leaks into the integrated loudness ("
                        + juce::String (meter.getIntegratedLufs() - reference.getIntegratedLufs(), 4) + " LU)");

                if (c.length >= sub)
                    expect (shortTermDuring > shortTermBefore + 3.0f, "short-term keeps showing what passes during the pause");

                expect (contiguous, "the sub-block ring is not affected by the pause");
                expect (! meter.isIntegrationPaused(), "resumed");
            }
        }

        // R8-04（ReleaseSafety 单元）：暂时没有读数（≤ −90 LUFS）时保持上一次有效读数算出的衰减；从未有过读数、reset()、关掉预听、
        // 非有限的坏读数都回到 1 并忘掉记忆（坏读数之后再来「没有读数」也不会跳回旧衰减）；resetDitherSequence 只动随机序列。
        void testReleaseSafetyHold()
        {
            beginTest ("release safety: a missing reading holds the last attenuation; never-read / reset / disabled / bad readings return to unity");

            ReleaseSafety module;
            module.prepare (t032Rate);
            module.setParams (true, -14.0f, 0);

            renderWithReading (module, -100.0f, 0.2);
            expectEquals (module.getPreviewGainDb(), 0.0f, "never had a reading: unity");
            expect (! module.holdsLastValidTargetForTesting());

            renderWithReading (module, -8.0f, 0.2);
            expectWithinAbsoluteError (module.getPreviewGainDb(), -6.0f, 0.001f, "a valid reading attenuates");
            renderWithReading (module, -100.0f, 1.0);
            expectWithinAbsoluteError (module.getPreviewGainDb(), -6.0f, 0.001f, "a missing reading holds the last attenuation");
            expect (module.holdsLastValidTargetForTesting());

            renderWithReading (module, std::numeric_limits<float>::quiet_NaN(), 0.1);
            expectEquals (module.getPreviewGainDb(), 0.0f, "a non-finite reading returns to unity");
            expect (! module.holdsLastValidTargetForTesting(), "… and forgets the last reading");
            renderWithReading (module, -100.0f, 0.2);
            expectEquals (module.getPreviewGainDb(), 0.0f, "no jump back to the old attenuation after a bad reading");

            renderWithReading (module, -8.0f, 0.2);
            module.resetDitherSequence();
            expectWithinAbsoluteError (module.getPreviewGainDb(), -6.0f, 0.001f, "resetDitherSequence keeps the gain");
            expect (module.holdsLastValidTargetForTesting(), "… and the remembered reading");

            module.reset();
            expectEquals (module.getPreviewGainDb(), 0.0f, "reset returns to unity immediately");
            renderWithReading (module, -100.0f, 0.2);
            expectEquals (module.getPreviewGainDb(), 0.0f, "reset forgets the last reading");

            renderWithReading (module, -8.0f, 0.2);
            module.setParams (false, -14.0f, 0);
            expect (! module.holdsLastValidTargetForTesting(), "disabling the preview forgets the last reading");
            renderWithReading (module, -8.0f, 0.2);
            expectEquals (module.getPreviewGainDb(), 0.0f, "disabled preview ramps back to unity");
            module.setParams (true, -14.0f, 0);
            renderWithReading (module, -100.0f, 0.2);
            expectEquals (module.getPreviewGainDb(), 0.0f, "re-enabled without a reading: unity, not the old attenuation");
        }

        // R10-01：新实例默认值的一处真源。逐项核对规格的默认值（设计规格各模块「参数」表、高级母带第 2 节、ADR-020），浮点参数的默认值
        // 落在 ParameterRanges.h 的范围里、是合法格点。高级母带的新实例默认是两项「自动」——与 MasteringSettings{} 的中性默认（关）不同，
        // 离线工具以前用的正是后者。处理器 Debug 版构造时再把真正的参数表与它逐项核对（PluginProcessor.cpp）。
        void testFactoryDefaults()
        {
            beginTest ("ParameterDefaults.h: the new-instance defaults, each a legal value of its parameter range");

            namespace ranges = param::ranges;
            const auto d = param::defaults::factoryChainParams();
            // 在范围里、落在步进格点上（连续刻度 = 对数刻度不吸附，只看范围）。不用 snapToLegalValue 比：它按 float 重算格点，
            // 0.01 这类步长会差 1 ulp（−3 + 0.01f × 300 ≠ 0）。
            const auto legal = [] (const juce::NormalisableRange<float>& range, float value)
            {
                if (value < range.start || value > range.end)
                    return false;

                if (range.interval <= 0.0f)
                    return true;

                const auto steps = (value - range.start) / range.interval;
                return std::abs (steps - std::round (steps)) < 1.0e-3f;
            };

            expect (legal (ranges::limitGain(), d.limit.gainDb) && legal (ranges::limitCeiling(), d.limit.ceilingDb)
                    && legal (ranges::limitRelease(), d.limit.releaseMs) && legal (ranges::limitLookahead(), d.limit.lookaheadMs)
                    && legal (ranges::limitClip(), d.limit.clip), "Limit defaults inside their ranges");
            expect (legal (ranges::colorTone(), d.color.toneDb) && legal (ranges::colorGlue(), d.color.glue)
                    && legal (ranges::colorHeat(), d.color.heat) && legal (ranges::colorMix(), d.color.mixPercent)
                    && legal (ranges::colorOutput(), d.color.outputDb), "Color defaults inside their ranges");
            expect (legal (ranges::balanceRecover(), d.balance.recover) && legal (ranges::balanceTame(), d.balance.tame)
                    && legal (ranges::balanceBrighten(), d.balance.brighten), "Balance defaults inside their ranges");
            expect (legal (ranges::glazeAmount(), d.glazePercent), "GLAZE default inside its range");
            expect (legal (ranges::clayLowCut(), d.claySettings.lowCutHz) && legal (ranges::clayMono(), d.claySettings.monoHz)
                    && legal (ranges::clayWidth(), d.claySettings.widthDb), "Clay manual defaults inside their ranges");

            expect (juce::exactlyEqual (d.limit.ceilingDb, -1.0f) && juce::exactlyEqual (d.limit.lookaheadMs, 2.0f) && d.limit.autoRelease
                    && ! d.limit.listen && ! d.limit.bypass, "Limit: Ceiling −1 dBTP, Lookahead 2 ms, auto release");
            expect (juce::exactlyEqual (d.color.mixPercent, 100.0f) && d.balance.speed == BalanceModule::Speed::mid, "Color Mix 100 %, Balance Mid");
            expect (juce::exactlyEqual (d.glazePercent, 35.0f) && d.glazeStyle == 0, "GLAZE 35 %, style Auto");
            expect (d.claySettings.lowCutMode == ClayMode::automatic && d.claySettings.monoMode == ClayMode::automatic
                    && d.claySettings.widthMode == ClayMode::automatic, "Clay: three times Auto");

            const auto& m = d.mastering;
            expect (m.harshMode == HarshMode::automatic && juce::exactlyEqual (m.harshAmount, 50.0f)
                    && juce::exactlyEqual (m.transientPreserve, 50.0f), "harshness protection Auto 50 %, transient preserve 50 %");
            expect (m.targetMode == TargetMode::automatic && juce::exactlyEqual (m.targetAmount, 35.0f), "spectrum target Auto 35 %");
            expect (juce::exactlyEqual (m.codecMarginDb, 0.0f) && m.dither == DitherMode::off && juce::exactlyEqual (m.previewTargetLufs, -14.0f),
                    "codec reserve 0, dither off, preview −14 LUFS");
            expect (m.harshResolved < 0.0f && m.targetResolved < 0.0f, "new-instance params carry no resolved amounts");

            const MasteringSettings neutral {};
            expect (neutral.harshMode == HarshMode::off && neutral.targetMode == TargetMode::off,
                    "the struct defaults stay neutral (old callers), which is exactly why the tools must use factoryChainParams()");
        }

        // R8-02 链级：ChainParams 的每个浮点字段分别喂 NaN / +Inf / −Inf（其余参数把整条链推满：Limit Gain +24 dB、Glue / Heat / Clip、
        // 手动推响不刺 / 暖色目标 / 编码余量、修坯三项手动、预听开着）——输出全部有限、不静音、真峰值 ≤ 实际 Ceiling + 0.01 dB，并与该字段
        // 取「回退值」的链逐位相同。回退值 = 该字段的默认值（ChainParams / 各模块 Params / MasteringSettings 的默认）；例外是修坯的三个手动值：
        // ClayModule 按文档回退中性（低切关 = 0、加宽 0、收单分频点沿用模块初值 80 Hz）。GLAZE、高级母带、Balance、修坯的字段与
        // Limit Ceiling（链先扣编码余量，守卫在链里：这里编码余量 0.3 dB，改前 NaN Ceiling 扣完还是 NaN）现在就判定。
        // 其余 Color / Limit 字段要等 A2 / A3 的模块守卫合进来（colorLimitGuardsMerged）才跑——之前连跑都不能跑：本线源码上把开关改成 true，
        // color.toneDb / color.glue 的坏值与回退值不逐位相同，跑到 color.heat 时测试进程段错误（ColorModule::heatCompensationFor 里 jlimit
        // 放过 NaN，(int) NaN 越界查表；2026-10-01 在云端容器上复现）。
        void testNonFiniteParams()
        {
            beginTest ("every float field of ChainParams fed NaN / +Inf / -Inf: finite, audible, true peak <= Ceiling + 0.01 dB, bit-identical to its fallback");

            using Accessor = float& (*) (CeladonChain::Params&);
            struct Field { const char* name; Accessor field; float fallback; const char* pendingOwner; };

            const CeladonChain::Params defaults {};
            auto base = makeNeutralParams();
            base.balance.recover = 30.0f;  base.balance.tame = 30.0f;  base.balance.brighten = 10.0f;
            base.color.toneDb = 0.5f;  base.color.glue = 30.0f;  base.color.heat = 30.0f;  base.color.mixPercent = 90.0f;
            base.limit.gainDb = 24.0f;  base.limit.clip = 20.0f;  base.limit.autoRelease = false;  base.limit.releaseMs = 150.0f;
            base.mastering.harshMode = HarshMode::manual;  base.mastering.harshAmount = 60.0f;  base.mastering.transientPreserve = 40.0f;
            base.mastering.targetMode = TargetMode::warm;  base.mastering.targetAmount = 40.0f;  base.mastering.codecMarginDb = 0.3f;
            base.claySettings.lowCutMode = base.claySettings.monoMode = base.claySettings.widthMode = ClayMode::manual;
            base.claySettings.lowCutHz = 30.0f;  base.claySettings.monoHz = 150.0f;  base.claySettings.widthDb = 1.0f;

            const char* const colorOwner = "A2 (ColorModule::setParams)";
            const char* const limitOwner = "A3 (LimiterModule::setParams)";
            const Field fields[] =
            {
                { "balance.recover",            [] (CeladonChain::Params& p) -> float& { return p.balance.recover; },            defaults.balance.recover, nullptr },
                { "balance.tame",               [] (CeladonChain::Params& p) -> float& { return p.balance.tame; },               defaults.balance.tame, nullptr },
                { "balance.brighten",           [] (CeladonChain::Params& p) -> float& { return p.balance.brighten; },           defaults.balance.brighten, nullptr },
                { "balance.automaticRecover",   [] (CeladonChain::Params& p) -> float& { return p.balance.automaticRecover; },   defaults.balance.automaticRecover, nullptr },
                { "balance.automaticTame",      [] (CeladonChain::Params& p) -> float& { return p.balance.automaticTame; },      defaults.balance.automaticTame, nullptr },
                { "balance.harshAmount",        [] (CeladonChain::Params& p) -> float& { return p.balance.harshAmount; },        defaults.balance.harshAmount, nullptr },
                { "balance.transientPreserve",  [] (CeladonChain::Params& p) -> float& { return p.balance.transientPreserve; },  defaults.balance.transientPreserve, nullptr },
                { "balance.targetAmount",       [] (CeladonChain::Params& p) -> float& { return p.balance.targetAmount; },       defaults.balance.targetAmount, nullptr },
                { "balance.targetSlopeDbPerOct",[] (CeladonChain::Params& p) -> float& { return p.balance.targetSlopeDbPerOct; },defaults.balance.targetSlopeDbPerOct, nullptr },
                { "color.toneDb",               [] (CeladonChain::Params& p) -> float& { return p.color.toneDb; },               defaults.color.toneDb, colorOwner },
                { "color.glue",                 [] (CeladonChain::Params& p) -> float& { return p.color.glue; },                 defaults.color.glue, colorOwner },
                { "color.heat",                 [] (CeladonChain::Params& p) -> float& { return p.color.heat; },                 defaults.color.heat, colorOwner },
                { "color.mixPercent",           [] (CeladonChain::Params& p) -> float& { return p.color.mixPercent; },           defaults.color.mixPercent, colorOwner },
                { "color.outputDb",             [] (CeladonChain::Params& p) -> float& { return p.color.outputDb; },             defaults.color.outputDb, colorOwner },
                { "limit.gainDb",               [] (CeladonChain::Params& p) -> float& { return p.limit.gainDb; },               defaults.limit.gainDb, limitOwner },
                { "limit.ceilingDb",            [] (CeladonChain::Params& p) -> float& { return p.limit.ceilingDb; },            defaults.limit.ceilingDb, nullptr },
                { "limit.releaseMs",            [] (CeladonChain::Params& p) -> float& { return p.limit.releaseMs; },            defaults.limit.releaseMs, limitOwner },
                { "limit.lookaheadMs",          [] (CeladonChain::Params& p) -> float& { return p.limit.lookaheadMs; },          defaults.limit.lookaheadMs, limitOwner },
                { "limit.clip",                 [] (CeladonChain::Params& p) -> float& { return p.limit.clip; },                 defaults.limit.clip, limitOwner },
                { "glazePercent",               [] (CeladonChain::Params& p) -> float& { return p.glazePercent; },               defaults.glazePercent, nullptr },
                { "mastering.harshAmount",      [] (CeladonChain::Params& p) -> float& { return p.mastering.harshAmount; },      defaults.mastering.harshAmount, nullptr },
                { "mastering.transientPreserve",[] (CeladonChain::Params& p) -> float& { return p.mastering.transientPreserve; },defaults.mastering.transientPreserve, nullptr },
                { "mastering.targetAmount",     [] (CeladonChain::Params& p) -> float& { return p.mastering.targetAmount; },     defaults.mastering.targetAmount, nullptr },
                { "mastering.codecMarginDb",    [] (CeladonChain::Params& p) -> float& { return p.mastering.codecMarginDb; },    defaults.mastering.codecMarginDb, nullptr },
                { "mastering.previewTargetLufs",[] (CeladonChain::Params& p) -> float& { return p.mastering.previewTargetLufs; },defaults.mastering.previewTargetLufs, nullptr },
                { "mastering.harshResolved",    [] (CeladonChain::Params& p) -> float& { return p.mastering.harshResolved; },    defaults.mastering.harshResolved, nullptr },
                { "mastering.targetResolved",   [] (CeladonChain::Params& p) -> float& { return p.mastering.targetResolved; },   defaults.mastering.targetResolved, nullptr },
                { "claySettings.lowCutHz",      [] (CeladonChain::Params& p) -> float& { return p.claySettings.lowCutHz; },      0.0f, nullptr },
                { "claySettings.monoHz",        [] (CeladonChain::Params& p) -> float& { return p.claySettings.monoHz; },        ClayModule::Params {}.monoHz, nullptr },
                { "claySettings.widthDb",       [] (CeladonChain::Params& p) -> float& { return p.claySettings.widthDb; },       0.0f, nullptr },
                { "clay.lowCutHz",              [] (CeladonChain::Params& p) -> float& { return p.clay.lowCutHz; },              defaults.clay.lowCutHz, nullptr },
                { "clay.monoHz",                [] (CeladonChain::Params& p) -> float& { return p.clay.monoHz; },                defaults.clay.monoHz, nullptr },
                { "clay.monoAmount",            [] (CeladonChain::Params& p) -> float& { return p.clay.monoAmount; },            defaults.clay.monoAmount, nullptr },
                { "clay.widthDb",               [] (CeladonChain::Params& p) -> float& { return p.clay.widthDb; },               defaults.clay.widthDb, nullptr },
            };

            const auto input = makeWideMaterial (t032Rate, 1.0, 51);
            const auto render = [&input] (const CeladonChain::Params& p, float& ceilingDb)
            {
                CeladonChain chain;
                prepareChain (chain, t032Rate, p);
                chain.setReleasePreviewEnabled (true);
                auto output = makeSilence (numChannels, (int) input[0].size());
                processSpan (chain, p, input, output, 0, (int) input[0].size(), maxBlock, {});
                ceilingDb = chain.getEffectiveParams().limit.ceilingDb;
                return output;
            };

            const float badValues[] = { std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                                        -std::numeric_limits<float>::infinity() };
            const char* const badNames[] = { "NaN", "+Inf", "-Inf" };
            int judged = 0, pendingSkipped = 0;

            for (const auto& f : fields)
            {
                if (f.pendingOwner != nullptr && ! colorLimitGuardsMerged)
                {
                    logMessage ("  skipped until " + juce::String (f.pendingOwner) + " is merged: " + f.name);
                    ++pendingSkipped;
                    continue;
                }

                auto fallbackParams = base;
                f.field (fallbackParams) = f.fallback;
                float fallbackCeilingDb = 0.0f;
                const auto expected = render (fallbackParams, fallbackCeilingDb);

                for (size_t v = 0; v < std::size (badValues); ++v)
                {
                    auto badParams = base;
                    f.field (badParams) = badValues[v];
                    float ceilingDb = 0.0f;
                    const auto got = render (badParams, ceilingDb);

                    double energy = 0.0;

                    for (const auto& channel : got)
                        for (auto sample : channel)
                            energy += (double) sample * (double) sample;

                    const auto finite = allFinite (got);
                    const auto audible = energy > 1.0;   // 1 s 立体声推满的输出能量在 1e4 量级；静音为 0
                    const auto peakDb = toDb (truePeakOf (got));
                    const auto underCeiling = finite && peakDb <= fallbackCeilingDb + 0.01f;
                    const auto identical = bitIdentical (got, expected);
                    const auto tag = juce::String (f.name) + " = " + badNames[v];

                    ++judged;
                    expect (finite, tag + ": output must stay finite");
                    expect (audible, tag + ": output must not go silent");
                    expect (underCeiling, tag + ": true peak " + juce::String (peakDb, 3) + " dBTP must stay under the Ceiling "
                                              + juce::String (fallbackCeilingDb, 3) + " + 0.01 dB");
                    expect (identical, tag + ": must behave exactly like its fallback value");
                }
            }

            logMessage ("  " + juce::String (judged) + " field / value pairs judged; " + juce::String (pendingSkipped)
                        + " Color / Limit fields skipped until their module guards are merged (A2 / A3)");
            expect (judged == 3 * (int) (std::size (fields) - (size_t) pendingSkipped), "every non-pending field was judged with all three values");
        }
    };

    static ChainGuardsTests chainGuardsTests;
}

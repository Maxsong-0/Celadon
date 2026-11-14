/*
  ==============================================================================

    GlazeControllerTests.cpp
    GLAZE 编排器（T-048 阶段 2 起的连续编排器，ADR-030）经 CeladonChain 驱动的自动测试（两块表与编排器都在链里，和插件里完全一样的
    接线）。聆听 / 锁定 / 校准的产品契约（七条状态行、速率、棘轮、旧工程丢弃）在 tests/Phase2OrchestratorTests.cpp 与
    GlazeStatusTextTests.cpp。

    三个测试类：
      - GlazeController：连续编排器的阶段与计数、GLAZE 0 的落零与速率、换风格、跳播 / 回绕 / 宿主 reset、LIVE 往返与旧格式冷启动、
        离线冷导出、同规格重 prepare、确定性重放、增益棘轮、prepare 与读档的先后、手动优先、非有限输出、音频线程零分配、
        GLAZE 0 时 Clay 中性。阶段 1 留下的聆听 → 锁定 → 校准用例（v0.6 A4、v0.1.7.0 Clay A6、T-026 参考条件、T-032 摘要）
        T-048 起就不再调用，T-098 连同只给它们用的工具一起删掉（历史见 git）。
      - GlazeControllerTrimFreeze（T-097）：G_trim 冻结、已删守门的存档节点、零分配。
      - GlazeControllerTrimLoop（T-106）：G_trim 换误差口径之后的闭环。
    时长常数用 setConstantsForTesting 缩短（fastConstants）；每个用例的「依据」写在注释里。

  ==============================================================================
*/

#include "TestHelpers.h"

#include "../Source/dsp/CeladonChain.h"
#include "../Source/dsp/ClayGuard.h"
#include "../Source/dsp/GlazePlanner.h"
#include "../Source/dsp/GlazeProfile.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

namespace celadon::test
{
    namespace
    {
        using glaze::Phase;
        using Probe = glaze::GlazeController::TestProbe;

        constexpr int numChannels = 2;
        constexpr int blockSize = 512;
        constexpr double defaultRate = 48000.0;

        glaze::Constants fastConstants()
        {
            auto c = glaze::factoryConstants();
            c.minListenSeconds = 2.0;
            c.maxListenSeconds = 3.0;
            c.planTransitionSeconds = 0.5;
            c.calibrationSettleSeconds = 0.5;
            c.calibrationWindowSeconds = 2.0;
            return c;
        }

        // 可复现的类音乐素材（120 BPM：底鼓、贝斯、和声垫、军鼓、踩镲，两声道噪声不相关），峰值归一到 peak。
        Signal makeMusic (double sampleRate, double seconds, juce::int64 seed, float peak)
        {
            constexpr double beatSeconds = 0.5;
            constexpr double bassNotesHz[] = { 55.0, 73.42, 82.41, 61.74 };
            constexpr double padNotesHz[]  = { 220.0, 277.18, 329.63, 440.0 };

            const auto numSamples = (int) (seconds * sampleRate);
            auto signal = makeSilence (numChannels, numSamples);
            const auto twoPi = juce::MathConstants<double>::twoPi;
            const auto snareCoeff = std::exp (-twoPi * 3000.0 / sampleRate);
            const auto hatCoeff   = std::exp (-twoPi * 6000.0 / sampleRate);

            for (int ch = 0; ch < numChannels; ++ch)
            {
                juce::Random random (seed + (juce::int64) ch * 7919);
                auto& channel = signal[(size_t) ch];
                double snare = 0.0, hat = 0.0;

                for (int i = 0; i < numSamples; ++i)
                {
                    const auto t = (double) i / sampleRate;
                    const auto beat = (int) (t / beatSeconds);
                    const auto beatPos = t - beat * beatSeconds;
                    const auto white = (double) random.nextFloat() * 2.0 - 1.0;
                    snare = snareCoeff * snare + (1.0 - snareCoeff) * white;
                    hat   = hatCoeff * hat + (1.0 - hatCoeff) * white;

                    const auto kick = 0.55 * std::sin (twoPi * 55.0 * beatPos) * std::exp (-beatPos / 0.09)
                                    + 0.15 * white * std::exp (-beatPos / 0.003);
                    const auto bassHz = bassNotesHz[(size_t) (beat % 4)];
                    const auto bass = juce::jmin (1.0, beatPos / 0.01) * std::exp (-beatPos / 0.6)
                                      * (0.28 * std::sin (twoPi * bassHz * beatPos) + 0.10 * std::sin (twoPi * 2.0 * bassHz * beatPos));

                    double pad = 0.0;
                    for (auto hz : padNotesHz)
                        pad += 0.05 * std::sin (twoPi * hz * t + 0.7 * ch);

                    const auto snareHit = (beat % 2 == 1) ? 0.6 * snare * std::exp (-beatPos / 0.12) : 0.0;
                    const auto hatHit = 0.08 * (white - hat) * std::exp (-std::fmod (t, beatSeconds * 0.5) / 0.03);

                    channel[(size_t) i] = (float) (kick + bass + pad + snareHit + hatHit);
                }
            }

            float maxAbs = 0.0f;
            for (const auto& channel : signal)
                for (auto x : channel)
                    maxAbs = juce::jmax (maxAbs, std::abs (x));

            scale (signal, peak / maxAbs);
            return signal;
        }

        bool sameBits (float a, float b) noexcept
        {
            return std::memcmp (&a, &b, sizeof (float)) == 0;
        }

        bool sameIncrements (const glaze::AutoIncrements& a, const glaze::AutoIncrements& b) noexcept
        {
            return sameBits (a.balanceRecover, b.balanceRecover) && sameBits (a.balanceTame, b.balanceTame)
                && sameBits (a.colorToneDb, b.colorToneDb) && sameBits (a.colorGlue, b.colorGlue)
                && sameBits (a.colorHeat, b.colorHeat) && sameBits (a.limitGainDb, b.limitGainDb)
                && sameBits (a.limitClip, b.limitClip);
        }

        // 交给模块的参数逐字段逐位相同（浮点按位、布尔与枚举按值）。
        bool sameParams (const ChainParams& a, const ChainParams& b) noexcept
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

        ChainParams defaultParams (float glazePercent = 0.0f, int style = 0)
        {
            ChainParams p;
            p.limit.ceilingDb = -1.0f;
            p.glazePercent = glazePercent;
            p.glazeStyle = style;
            return p;
        }

        //==============================================================================
        // 一条链 + 宿主模拟：每块从素材里循环取样、报播放位置（默认在播、位置随块前进），交参数，处理。
        struct Rig
        {
            // initial：prepare 之前交给链的参数（与处理器 prepareToPlay 一样先交参数再 prepare；风格等在第一块之前就定好，
            // 不会在第一块被当成「换风格」）。
            Rig (const glaze::Constants& constants, Signal material, const ChainParams& initial = defaultParams(),
                 double rate = defaultRate, int block = blockSize)
                : params (initial), source (std::move (material)), sampleRate (rate), blockLength (block)
            {
                chain.getGlazeController().setConstantsForTesting (constants);

                host.hasPlayState = true;
                host.isPlaying = true;
                host.hasPosition = true;

                // 与处理器的 prepareToPlay 同一顺序：先交参数再 prepare（prepare 之后不再交：第一块由 step 按宿主信息交）。
                chain.setParams (params, false);
                chain.prepare ({ sampleRate, (juce::uint32) blockLength, (juce::uint32) numChannels });
            }

            void reprepare (double rate)
            {
                sampleRate = rate;
                buffer.setSize (numChannels, blockLength);
                chain.setParams (params, false);
                chain.prepare ({ sampleRate, (juce::uint32) blockLength, (juce::uint32) numChannels });
            }

            // 一块：取素材 → 报宿主信息 → 交参数 → 处理。返回链是否报告了非有限输出。
            bool step()
            {
                const auto length = (size_t) source.front().size();
                buffer.setSize (numChannels, blockLength, false, false, true);

                for (int ch = 0; ch < numChannels; ++ch)
                    for (int i = 0; i < blockLength; ++i)
                        buffer.setSample (ch, i, sourceGain * source[(size_t) ch][(sourceIndex + (size_t) i) % length]);

                sourceIndex = (sourceIndex + (size_t) blockLength) % length;

                host.positionSamples = position;

                // T-038 起：tempoBpm > 0 时同时报恒定速度、4/4 的拍点（PPQ 与位置严格一致，小节起点按 PPQ ÷ 4；实时段落账按小节分段）；默认 0 = 不报。
                if (tempoBpm > 0.0)
                {
                    host.hasTempo = true;
                    host.bpm = tempoBpm;
                    host.hasPpq = true;
                    host.ppqPosition = (double) position * tempoBpm / (60.0 * sampleRate);
                    host.hasBarStart = true;
                    host.ppqLastBarStart = std::floor (host.ppqPosition / 4.0) * 4.0;
                    host.hasTimeSignature = true;
                    host.timeSignatureNumerator = 4;
                    host.timeSignatureDenominator = 4;
                }

                chain.setHostInfo (host);
                chain.setParams (params, hostBypassed);
                const auto nonFinite = chain.process (buffer);

                if ((host.isPlaying || host.offline) && advancePosition)
                    position += blockLength;

                ++blocksRun;
                return nonFinite;
            }

            int blocksFor (double seconds) const   { return juce::jmax (1, (int) std::ceil (seconds * sampleRate / blockLength)); }

            template <typename PerBlock>
            void run (double seconds, PerBlock&& perBlock)
            {
                const auto blocks = blocksFor (seconds);

                for (int b = 0; b < blocks; ++b)
                {
                    step();
                    perBlock (b);
                }
            }

            void run (double seconds)   { run (seconds, [] (int) {}); }

            template <typename Done>
            bool runUntil (double maxSeconds, Done&& done)
            {
                const auto blocks = blocksFor (maxSeconds);

                for (int b = 0; b < blocks; ++b)
                {
                    step();

                    if (done())
                        return true;
                }

                return false;
            }

            Probe probe() const             { return chain.getGlazeController().getProbeForTesting(); }
            glaze::Status status() const    { return chain.getGlazeStatus(); }
            glaze::CommittedState committed() const   { return chain.getGlazeController().getCommittedStateForTesting(); }

            CeladonChain chain;
            ChainParams params;
            glaze::HostInfo host;
            Signal source;
            float sourceGain = 1.0f;
            size_t sourceIndex = 0;
            double sampleRate;
            int blockLength;
            std::int64_t position = 0;
            bool advancePosition = true;
            bool hostBypassed = false;
            int blocksRun = 0;
            double tempoBpm = 0.0;   // T-038：> 0 时每块报拍点（见 step）
            juce::AudioBuffer<float> buffer { numChannels, blockSize };
        };
    }

    //==============================================================================
    // ---- v0.1.7.0 修坯 Clay（A6）用的工具 ----
    namespace
    {
        // 中性 = 三级都不处理（低切关、收单 0、加宽 0）：ClayModule 逐位直通（收单为 0 时分频点无意义，不看）。
        bool isNeutralClay (const ClayModule::Params& p) noexcept
        {
            return p.lowCutHz == 0.0f && p.monoAmount == 0.0f && p.widthDb == 0.0f;
        }

        // Clay 用例的常数：短时长（fastConstants）且关掉画像失配——合成素材与画像同电平，但不能让它碰巧插进一次方案过渡或
        // 压低增益上限（过渡混合的逐块核对要求期间没有别的方案切换）。
        glaze::Constants clayConstants()
        {
            auto c = fastConstants();
            c.mismatchThresholdLu = c.mismatchReleaseLu = 1.0e9f;
            return c;
        }

        // 修坯用的合成素材（4 s；60 / 110 Hz 在 4 s 里都是整数周期，Rig 循环播放没有接缝）：
        //   低频 = 60 Hz（0.25）+ 110 Hz（0.15）两个正弦：落在 40 ～ 125 Hz 的统计段里，远在静音门之上；
        //   高频 = 白噪声 ±0.05：能量大半在 2 kHz 以上，2 ～ 16 kHz 十段都在静音门之上。
        //   lowAntiPhase：右声道低频反相（低频相关度 ≈ −1 < −0.2：反相提示的触发条件，Clay 子文档第 3 节、根治修订 3.4）；
        //   highWide：左右噪声不相关（高频 P_S ≈ P_M：预测加宽后相关度 (1 − γ²) / (1 + γ²) < 0.25，宽度保护的触发条件）。
        //   两项都为假 = 单声道素材（S = 0：反相提示不该亮、宽度保护不该动）。
        Signal makeClayMaterial (bool lowAntiPhase, bool highWide, juce::int64 seed)
        {
            const auto numSamples = (int) (4.0 * defaultRate);
            auto signal = makeSilence (numChannels, numSamples);
            juce::Random left (seed), right (seed + 1);
            const auto twoPi = juce::MathConstants<double>::twoPi;

            for (int i = 0; i < numSamples; ++i)
            {
                const auto t = (double) i / defaultRate;
                const auto low = (float) (0.25 * std::sin (twoPi * 60.0 * t) + 0.15 * std::sin (twoPi * 110.0 * t));
                const auto noiseLeft = 0.05f * (left.nextFloat() * 2.0f - 1.0f);
                const auto noiseRight = highWide ? 0.05f * (right.nextFloat() * 2.0f - 1.0f) : noiseLeft;
                signal[0][(size_t) i] = low + noiseLeft;
                signal[1][(size_t) i] = (lowAntiPhase ? -low : low) + noiseRight;
            }

            return signal;
        }

    }

    class GlazeControllerTests  : public juce::UnitTest
    {
    public:
        GlazeControllerTests() : juce::UnitTest ("GlazeController", "Celadon") {}

        void runTest() override
        {
            testRestoredLiveSurvivesFirstPrepare();
            // 0.1.8.9 起 GLAZE 是连续编排器。原来断言聆听 → 锁定 → 校准的用例早已不调，T-098 连代码删掉（历史见 git）；
            // 等价行为在下面这些用例里，对照 Phase2Contract 与 GlazeController::Impl。
            testContinuousPhase();
            testGlazeZeroSnapAndSlew();
            testStyleChangeDoesNotCalibrate();
            testTransportAndHostReset();
            testLiveRoundTrip();
            testLegacyListenStateColdStarts();
            testOfflineColdExport();
            testSameSpecReprepareKeepsLive();
            testDeterministicReplay();
            testRatchetBlocksGainRise();
            testPrepareOrderLogsFirstPrepare();
            testManualComesFirst();
            testNonFiniteOutputAndInputMeter();
            testAudioThreadDoesNotAllocate();
            testClayNeutralAtGlazeZero();
        }

        // 连续编排器：状态里聆听 / 锁定 / 校准那几项停在阶段 2 的默认值，GLAZE > 0 发布有限的连续目标。
        // T-098 删掉了探针里恒为 0 的聆听 / 提交 / 校准 / 约束反馈计数和 requestRelearn（空操作），这里不再核对它们。
        void testContinuousPhase()
        {
            beginTest ("phase stays unlearned; the retired listen flags stay at their defaults; the target is continuous");

            Rig rig (fastConstants(), makeMusic (defaultRate, 8.0, 11, 0.4f), defaultParams (50.0f, 0));
            auto flagsHeld = true;

            rig.run (4.0, [&] (int)
            {
                const auto s = rig.status();
                flagsHeld = flagsHeld && s.phase == Phase::unlearned && ! s.calibrationValid && ! s.profileReady && ! s.planReady
                            && s.listenedSeconds == 0.0f && ! s.relearnInProgress
                            && ! s.profileMismatch;
            });

            expect (flagsHeld, "phase, calibration and the retired listen flags stay at their phase-2 defaults");
            expect (std::isfinite (rig.status().effectiveTargetLufs), "GLAZE > 0 publishes a finite continuous target");
        }

        // GLAZE 0 每块等于手动值；拧起来增益按 0.2 / 1.0 dB/s 滑动；拧回 0 时 trim 与守门归零。
        void testGlazeZeroSnapAndSlew()
        {
            beginTest ("GLAZE 0 matches the manual params; turning it up slews; back to 0 snaps trim and the guard");

            Rig rig (fastConstants(), makeMusic (defaultRate, 12.0, 12, 0.35f), defaultParams (0.0f, 1));
            auto exact = true;

            rig.run (2.0, [&] (int)
            {
                const auto& effective = rig.chain.getEffectiveParams();
                const auto probe = rig.probe();
                exact = exact && sameParams (effective, rig.params) && isNeutralClay (effective.clay)
                        && probe.appliedBiasDb == 0.0f && probe.executedGuardDb == 0.0f;
            });

            expect (exact, "GLAZE 0: effective params match the manual ones, Clay stays neutral, trim and guard stay 0");

            rig.params.glazePercent = 70.0f;
            const auto dt = (double) blockSize / defaultRate;
            auto slewOk = true;
            auto previous = rig.probe().increments.limitGainDb;
            auto rose = 0.0f;

            rig.run (3.0, [&] (int)
            {
                const auto gain = rig.probe().increments.limitGainDb;
                const auto delta = gain - previous;

                if (delta > (float) (0.2 * dt) + 1.0e-4f || delta < (float) (-1.0 * dt) - 1.0e-4f)
                    slewOk = false;

                rose += juce::jmax (0.0f, delta);
                previous = gain;
            });

            logMessage ("  upward slew over 3 s: " + juce::String (rose, 3) + " dB");
            expect (slewOk, "limit-gain steps stay within 0.2 dB/s up and 1.0 dB/s down");
            expect (rose > 0.2f, "gain actually slews upward while GLAZE is on");
            expect (std::isfinite (rig.status().effectiveTargetLufs), "the continuous target stays finite");

            rig.params.glazePercent = 0.0f;
            rig.step();
            expectEquals (rig.probe().appliedBiasDb, 0.0f, "GLAZE 0 snaps G_trim to 0");
            expectEquals (rig.probe().executedGuardDb, 0.0f, "GLAZE 0 snaps the executed guard to 0");
            expect (sameParams (rig.chain.getEffectiveParams(), rig.params), "GLAZE 0 snaps back to the manual params");
        }

        void testStyleChangeDoesNotCalibrate()
        {
            beginTest ("a style change slews the applied increments and does not start calibration");

            Rig rig (fastConstants(), makeMusic (defaultRate, 8.0, 13, 0.4f), defaultParams (60.0f, 1));
            rig.run (2.0);
            const auto before = rig.probe().increments;
            rig.params.glazeStyle = 2;
            rig.run (2.0);
            const auto after = rig.probe().increments;
            const auto moved = std::abs (after.colorGlue - before.colorGlue) + std::abs (after.limitClip - before.limitClip)
                             + std::abs (after.colorHeat - before.colorHeat);
            logMessage ("  glue/clip/heat L1 move after warm -> punchy: " + juce::String (moved, 3));
            expect (moved > 0.05f, "applied glue / clip / heat slew toward the new style");

            Rig automatic (fastConstants(), makeMusic (defaultRate, 8.0, 131, 0.4f), defaultParams (60.0f, 0));
            automatic.run (3.0);
            const auto weights = automatic.committed().lockedWeights;
            expect (std::abs (weights.warm + weights.punchy + weights.clean - 1.0f) < 1.0e-4f,
                    "automatic weights stay normalised while they slew");
        }

        void testTransportAndHostReset()
        {
            beginTest ("a stopped cursor move is not a jump; a playing jump and a loop wrap are counted; host reset keeps confidence");

            Rig rig (fastConstants(), makeMusic (defaultRate, 8.0, 14, 0.4f), defaultParams (40.0f));
            rig.run (1.0);

            rig.host.isPlaying = false;
            const auto jumpsWhileStopped = rig.probe().transportJumps;
            rig.position += 48000;
            rig.step();
            expectEquals (rig.probe().transportJumps, jumpsWhileStopped, "moving the cursor while stopped is not a jump");

            rig.host.isPlaying = true;
            rig.position += 96000;
            rig.step();
            expectEquals (rig.probe().transportJumps, jumpsWhileStopped + 1, "a jump while playing is counted");

            rig.host.hasLoop = true;
            rig.host.looping = true;
            const auto wrapsBefore = rig.probe().loopWraps;
            const auto jumpsBeforeWrap = rig.probe().transportJumps;
            rig.position -= 24000;
            rig.step();
            expectEquals (rig.probe().loopWraps, wrapsBefore + 1, "a backward move inside a loop is a wrap");
            expectEquals (rig.probe().transportJumps, jumpsBeforeWrap, "a loop wrap is not also a jump");

            const auto resets = rig.probe().hostResets;
            const auto confidence = rig.status().confidence;
            rig.chain.restartWithFadeIn();
            rig.step();
            expectEquals (rig.probe().hostResets, resets + 1, "restartWithFadeIn counts a host reset");
            expect (sameBits (rig.status().confidence, confidence), "host reset does not clear confidence");
        }

        void testLiveRoundTrip()
        {
            beginTest ("LIVE format 2 restores the applied increments; two restores replay bit-identically");

            const auto material = makeMusic (defaultRate, 8.0, 15, 0.4f);
            Rig donor (fastConstants(), material, defaultParams (45.0f, 1));
            donor.run (6.0);
            const auto xml = donor.chain.createGlazeStateXml();
            expect (xml != nullptr && xml->getIntAttribute ("formatVersion") == glaze::liveContainerFormatVersion, "the container is LIVE format 2");
            expect (xml->getChildByName ("PLAN") == nullptr && xml->getChildByName ("CALIBRATION") == nullptr
                        && xml->getChildByName (glaze::profileTag) == nullptr,
                    "the v2 container does not keep listen/lock nodes");

            const auto savedInc = donor.probe().increments;
            const auto savedTrim = donor.probe().appliedBiasDb;
            Rig restored (fastConstants(), material, donor.params);
            restored.chain.restoreGlazeStateXml (xml.get());
            expect (sameIncrements (restored.probe().increments, savedInc), "restore puts the applied increments back before the next block");
            expectEquals (restored.probe().appliedBiasDb, savedTrim, "restore puts G_trim back");
            expect (! restored.status().legacySessionDiscarded, "a v2 restore clears the legacy notice");

            Rig a (fastConstants(), material, donor.params);
            Rig b (fastConstants(), material, donor.params);
            a.chain.restoreGlazeStateXml (xml.get());
            b.chain.restoreGlazeStateXml (xml.get());
            auto identical = true;

            for (int i = 0; i < a.blocksFor (2.0); ++i)
            {
                a.step();
                b.step();

                for (int ch = 0; ch < numChannels; ++ch)
                    identical = identical && std::memcmp (a.buffer.getReadPointer (ch), b.buffer.getReadPointer (ch),
                                                          sizeof (float) * (size_t) blockSize) == 0;
            }

            expect (identical, "two restores of the same LIVE replay bit-identically");

            donor.chain.restoreGlazeStateXml (nullptr);
            donor.step();
            expect (! donor.status().legacySessionDiscarded, "a null restore cold-starts without the legacy notice");
        }

        void testLegacyListenStateColdStarts()
        {
            beginTest ("a v1 listen/lock tree cold-starts and raises legacySessionDiscarded");

            Rig rig (fastConstants(), makeMusic (defaultRate, 4.0, 16, 0.3f), defaultParams (40.0f));
            rig.run (1.0);
            auto xml = std::make_unique<juce::XmlElement> (glaze::stateTag);
            xml->setAttribute ("formatVersion", 1);
            xml->createNewChildElement (glaze::profileTag);
            xml->createNewChildElement ("PLAN");
            xml->createNewChildElement ("CALIBRATION");
            rig.chain.restoreGlazeStateXml (xml.get());
            rig.step();

            expect (rig.status().legacySessionDiscarded, "v1 listen/lock state raises the legacy notice");
            expect (! rig.status().calibrationValid && ! rig.status().profileReady && ! rig.status().clayNeedsRelearn,
                    "the old profile and calibration are not kept, and Clay does not ask for a relearn");
        }

        void testOfflineColdExport()
        {
            beginTest ("offline while confidence < 0.5 sets coldExport; it clears once confidence reaches 0.5");

            Rig rig (fastConstants(), makeMusic (defaultRate, 30.0, 17, 0.35f), defaultParams (40.0f));
            rig.tempoBpm = 120.0;
            rig.host.offline = true;
            rig.run (2.0);
            expect (rig.status().confidence < 0.5f && rig.status().coldExport, "cold export while the profile is still starting");
            expect (rig.chain.getAndResetOutputTruePeakDb() <= rig.params.limit.ceilingDb + 0.01f,
                    "true peak stays within the ceiling during the cold export");

            rig.host.offline = false;
            const auto ready = rig.runUntil (24.0, [&] { return rig.status().confidence >= 0.5f; });
            logMessage ("  confidence after playback " + juce::String (rig.status().confidence, 3));
            expect (ready, "confidence reaches 0.5 on continued playback");
            expect (! rig.status().coldExport, "online playback is not a cold export");

            rig.host.offline = true;
            rig.step();
            expect (rig.status().confidence >= 0.5f && ! rig.status().coldExport, "offline with confidence >= 0.5 is not a cold export");
        }

        void testSameSpecReprepareKeepsLive()
        {
            beginTest ("preparing again at the same spec keeps the LIVE profile");

            Rig rig (fastConstants(), makeMusic (defaultRate, 24.0, 18, 0.35f), defaultParams (50.0f));
            rig.tempoBpm = 120.0;
            const auto heard = rig.runUntil (20.0, [&] { return rig.chain.getLiveProfile().getProfile().integratedLufs.valid; });
            const auto before = rig.chain.getLiveProfile().getProfile().integratedLufs;
            expect (heard && before.valid, "fixture produced an integrated loudness");
            rig.reprepare (defaultRate);
            const auto after = rig.chain.getLiveProfile().getProfile().integratedLufs;
            expect (after.valid && sameBits (after.value, before.value), "same-spec prepare restores I bit-exactly");
        }

        void testDeterministicReplay()
        {
            beginTest ("two identical runs are bit-identical and stay under the true-peak ceiling");

            const auto material = makeMusic (defaultRate, 6.0, 19, 0.4f);
            const auto params = defaultParams (55.0f, 2);
            Rig a (fastConstants(), material, params);
            Rig b (fastConstants(), material, params);
            auto identical = true;

            for (int i = 0; i < a.blocksFor (4.0); ++i)
            {
                a.step();
                b.step();

                for (int ch = 0; ch < numChannels; ++ch)
                    identical = identical && std::memcmp (a.buffer.getReadPointer (ch), b.buffer.getReadPointer (ch),
                                                          sizeof (float) * (size_t) blockSize) == 0;
            }

            expect (identical, "same input, block size and params replay bit-identically");
            expect (a.chain.getAndResetOutputTruePeakDb() <= params.limit.ceilingDb + 0.01f, "true peak stays within the ceiling");
        }

        void testRatchetBlocksGainRise()
        {
            beginTest ("while current mainness is below 0.5, automatic limit gain does not rise");

            Rig rig (fastConstants(), makeMusic (defaultRate, 8.0, 20, 0.5f), defaultParams (70.0f));
            rig.tempoBpm = 124.0;
            rig.run (6.0);

            auto quiet = makeSilence (numChannels, (int) (8.0 * defaultRate));
            juce::Random random (20);

            for (auto& channel : quiet)
                for (auto& sample : channel)
                    sample = (random.nextFloat() * 2.0f - 1.0f) * 1.0e-4f;

            rig.source = std::move (quiet);
            rig.sourceIndex = 0;
            auto sawQuiet = false, held = true;
            auto ceiling = rig.probe().increments.limitGainDb;

            rig.run (8.0, [&] (int)
            {
                if (rig.status().currentMainness >= 0.5f)
                    return;

                const auto gain = rig.probe().increments.limitGainDb;

                if (! sawQuiet)
                    ceiling = gain;

                sawQuiet = true;
                held = held && gain <= ceiling + 1.0e-4f;
            });

            logMessage ("  quiet section mainness " + juce::String (rig.status().currentMainness, 3)
                        + ", saw m < 0.5 " + juce::String ((int) sawQuiet));
            expect (sawQuiet, "fixture: the quiet section drops mainness below 0.5");
            expect (held, "ratchet: limit gain does not rise while mainness < 0.5");
        }

        // 正确的宿主顺序是先 prepare 再读档。先读档再第一次 prepare 会丢掉 LIVE（CeladonChain::prepare 只在 hasPrepared 时留档）。
        // 这里只断言能保住的那条路，并把另一条路的读数打进日志，不把它写成通过条件。
        void testPrepareOrderLogsFirstPrepare()
        {
            beginTest ("prepare then restore keeps LIVE; restore before the first prepare is only logged");

            const auto material = makeMusic (defaultRate, 16.0, 21, 0.4f);
            const auto params = defaultParams (40.0f);
            Rig donor (fastConstants(), material, params);
            donor.tempoBpm = 120.0;
            const auto heard = donor.runUntil (16.0, [&] { return donor.chain.getLiveProfile().getProfile().integratedLufs.valid; });
            const auto xml = donor.chain.createGlazeStateXml();
            const auto kept = donor.chain.getLiveProfile().getProfile().integratedLufs;
            expect (heard && xml != nullptr && kept.valid, "fixture has a LIVE profile to restore");

            Rig good (fastConstants(), material, params);
            good.chain.restoreGlazeStateXml (xml.get());
            const auto restored = good.chain.getLiveProfile().getProfile().integratedLufs;
            expect (restored.valid && sameBits (restored.value, kept.value), "prepare then restore keeps I bit-exactly");

            CeladonChain wiped;
            wiped.getGlazeController().setConstantsForTesting (fastConstants());
            wiped.restoreGlazeStateXml (xml.get());
            wiped.setParams (params, false);
            wiped.prepare ({ defaultRate, (juce::uint32) blockSize, (juce::uint32) numChannels });
            const auto wipedI = wiped.getLiveProfile().getProfile().integratedLufs;
            logMessage ("  restore-before-first-prepare I valid " + juce::String ((int) wipedI.valid)
                        + " value " + juce::String (wipedI.value, 3) + "; prepare-then-restore I " + juce::String (restored.value, 3));
        }

        void testClayNeutralAtGlazeZero()
        {
            beginTest ("Clay automatic is neutral at GLAZE 0; a manual low cut is exact and audible");

            auto manual = defaultParams (0.0f, 0);
            manual.claySettings.lowCutMode = ClayMode::manual;
            manual.claySettings.lowCutHz = 30.0f;
            const auto material = makeClayMaterial (false, true, 22);
            Rig automatic (clayConstants(), material, defaultParams (0.0f));
            Rig manualRig (clayConstants(), material, manual);
            auto neutral = true, manualExact = true, differ = false;

            for (int i = 0; i < automatic.blocksFor (1.0); ++i)
            {
                automatic.step();
                manualRig.step();
                neutral = neutral && isNeutralClay (automatic.chain.getEffectiveParams().clay);
                manualExact = manualExact && manualRig.chain.getEffectiveParams().clay.lowCutHz == 30.0f;

                for (int ch = 0; ch < numChannels; ++ch)
                    for (int sample = 0; sample < blockSize; ++sample)
                        differ = differ || automatic.buffer.getSample (ch, sample) != manualRig.buffer.getSample (ch, sample);
            }

            expect (neutral, "automatic Clay is neutral on every block at GLAZE 0");
            expect (manualExact, "the manual low cut is exactly 30 Hz at GLAZE 0");
            expect (differ, "the manual low cut changes the output at GLAZE 0");
        }

    private:
        // 打开工程的宿主顺序是 setStateInformation 再第一次 prepareToPlay。0.1.8.9（以及阶段 2 合入后、
        // 版本号还印着 0.1.8.8 的构建）写出的是 LIVE format 2，两种顺序必须留下同一个 I。
        // 发行的 0.1.8.8 仍是 format 1 聆听树：两种顺序都冷启动，I 同样无效，旧工程提示还在。
        // 第二次 prepare 换采样率和块长：采样率缓存重算，画像按存档折算，两种顺序仍然一致。
        void testRestoredLiveSurvivesFirstPrepare()
        {
            beginTest ("restore before the first prepare keeps LIVE; a later rate and block change keeps it too");

            const auto material = makeMusic (defaultRate, 20.0, 21, 0.4f);
            const auto params = defaultParams (40.0f);
            Rig donor (fastConstants(), material, params);
            donor.tempoBpm = 120.0;
            const auto heard = donor.runUntil (18.0, [&]
            {
                return donor.chain.getLiveProfile().getProfile().integratedLufs.valid;
            });
            const auto xml = donor.chain.createGlazeStateXml();
            const auto kept = donor.chain.getLiveProfile().getProfile().integratedLufs;
            const auto keptTrim = donor.probe().appliedBiasDb;
            const auto keptInc = donor.probe().increments;
            expect (heard && xml != nullptr && xml->getIntAttribute ("formatVersion") == glaze::liveContainerFormatVersion && kept.valid,
                    "0.1.8.9 fixture has a LIVE profile to restore");

            Rig preparedFirst (fastConstants(), material, params);
            preparedFirst.chain.restoreGlazeStateXml (xml.get());
            const auto afterPrepare = preparedFirst.chain.getLiveProfile().getProfile().integratedLufs;
            const auto preparedProfile = preparedFirst.chain.getLiveProfile().createStateXml();

            CeladonChain restoredFirst;
            restoredFirst.getGlazeController().setConstantsForTesting (fastConstants());
            restoredFirst.restoreGlazeStateXml (xml.get());
            restoredFirst.setParams (params, false);
            restoredFirst.prepare ({ defaultRate, (juce::uint32) blockSize, (juce::uint32) numChannels });
            const auto afterRestore = restoredFirst.getLiveProfile().getProfile().integratedLufs;
            const auto restoredProfile = restoredFirst.getLiveProfile().createStateXml();
            const auto restoredProbe = restoredFirst.getGlazeController().getProbeForTesting();

            logMessage ("  restore-before-first-prepare I valid " + juce::String ((int) afterRestore.valid)
                        + " value " + juce::String (afterRestore.value, 3)
                        + "; prepare-then-restore I " + juce::String (afterPrepare.value, 3));

            expect (afterPrepare.valid && sameBits (afterPrepare.value, kept.value), "prepare then restore keeps I bit-exactly");
            expect (afterRestore.valid && sameBits (afterRestore.value, afterPrepare.value),
                    "restore before the first prepare keeps the same I");
            expect (preparedProfile != nullptr && restoredProfile != nullptr
                        && preparedProfile->isEquivalentTo (restoredProfile.get(), false),
                    "the restored LIVE_PROFILE matches either order");
            expect (sameIncrements (restoredProbe.increments, keptInc), "persisted increments survive the first prepare");
            expect (sameBits (restoredProbe.appliedBiasDb, keptTrim), "persisted G_trim survives the first prepare");
            expect (sameIncrements (preparedFirst.probe().increments, restoredProbe.increments)
                        && sameBits (preparedFirst.probe().appliedBiasDb, restoredProbe.appliedBiasDb),
                    "increments and G_trim match the prepare-then-restore chain");

            constexpr double secondRate = 44100.0;
            constexpr int secondBlock = 128;
            preparedFirst.chain.setParams (params, false);
            preparedFirst.chain.prepare ({ secondRate, (juce::uint32) secondBlock, (juce::uint32) numChannels });
            restoredFirst.setParams (params, false);
            restoredFirst.prepare ({ secondRate, (juce::uint32) secondBlock, (juce::uint32) numChannels });

            const auto preparedAgain = preparedFirst.chain.getLiveProfile().getProfile().integratedLufs;
            const auto restoredAgain = restoredFirst.getLiveProfile().getProfile().integratedLufs;
            const auto preparedAgainProfile = preparedFirst.chain.getLiveProfile().createStateXml();
            const auto restoredAgainProfile = restoredFirst.getLiveProfile().createStateXml();
            logMessage ("  second prepare @" + juce::String (secondRate, 0) + " / " + juce::String (secondBlock)
                        + " I " + juce::String (restoredAgain.value, 3));
            expect (preparedAgain.valid && restoredAgain.valid && sameBits (preparedAgain.value, restoredAgain.value),
                    "a second prepare at another rate and block size keeps the same I either order");
            expect (sameBits (restoredAgain.value, kept.value), "rescaling the profile does not change I");
            expect (preparedAgainProfile != nullptr && restoredAgainProfile != nullptr
                        && preparedAgainProfile->isEquivalentTo (restoredAgainProfile.get(), false),
                    "LIVE_PROFILE still matches after the rate and block change");

            CeladonChain cold;
            cold.prepare ({ defaultRate, (juce::uint32) blockSize, (juce::uint32) numChannels });
            expect (! cold.getLiveProfile().getProfile().integratedLufs.valid,
                    "a first prepare with nothing restored is still a cold start");

            beginTest ("0.1.8.8 listen state and 0.1.8.9 LIVE load the same value either order");

            auto listenState = std::make_unique<juce::XmlElement> (glaze::stateTag);
            listenState->setAttribute ("formatVersion", 1);
            listenState->createNewChildElement (glaze::profileTag);
            listenState->createNewChildElement ("PLAN");
            listenState->createNewChildElement ("CALIBRATION");
            listenState->createNewChildElement (glaze::executionTag);

            auto openListen = [&] (bool restoreBeforePrepare)
            {
                auto chain = std::make_unique<CeladonChain>();
                chain->setParams (params, false);

                if (restoreBeforePrepare)
                    chain->restoreGlazeStateXml (listenState.get());

                chain->prepare ({ defaultRate, (juce::uint32) blockSize, (juce::uint32) numChannels });

                if (! restoreBeforePrepare)
                    chain->restoreGlazeStateXml (listenState.get());

                return chain;
            };

            const auto listenEarly = openListen (true);
            const auto listenLate = openListen (false);
            const auto listenEarlyI = listenEarly->getLiveProfile().getProfile().integratedLufs;
            const auto listenLateI = listenLate->getLiveProfile().getProfile().integratedLufs;
            expect (! listenEarlyI.valid && ! listenLateI.valid && sameBits (listenEarlyI.value, listenLateI.value),
                    "a 0.1.8.8 listen/lock project cold-starts to the same LIVE either order");
            expect (listenEarly->getGlazeStatus().legacySessionDiscarded && listenLate->getGlazeStatus().legacySessionDiscarded,
                    "the discarded-listen notice survives a restore that happens before the first prepare");

            expect (afterRestore.valid && sameBits (afterRestore.value, afterPrepare.value),
                    "a 0.1.8.9 LIVE project (same container the phase-2 0.1.8.8 builds wrote) keeps I either order");
        }

        // 13. 手动优先：实际 Limit Gain 永远不低于手动值（保护层与负的校准偏差只能收回自动加上去的部分）；手动本身已超约束时
        //     （受手动设置限制）不施加校准偏差与保护偏移，实际值就是手动值 + 已撤回的增量（= 手动值）。
        // 依据：决策第 8 节「绝不自动抵消你的修改」「手动部分本身已超约束时，自动先撤回自己的增量」；PHASE2 设计决定 4。
        void testManualComesFirst()
        {
            beginTest ("manual first: effective Limit Gain never below the manual value; limited-by-manual adds nothing automatic");

            {
                // 连续编排器的增益按 0.2 dB/s 爬，6 s 里加不上旧锁定方案那几 dB。素材再推 18 dB，
                // 限幅 GR 才会越过收紧后的峰值预算，保护层才有得退。断言本身不放宽：退让必须出现，增益不得低于手动值，真峰值守住 Ceiling。
                Rig rig (withTightLimitBudgets (fastConstants()), makeMusic (defaultRate, 8.0, 25, 0.3f), defaultParams (100.0f, 2));
                rig.params.limit.gainDb = 3.0f;
                rig.run (2.0);
                rig.sourceGain = 8.0f;

                bool neverBelow = true;
                float deepestGuard = 0.0f;

                rig.run (8.0, [&] (int)
                {
                    neverBelow = neverBelow && rig.chain.getEffectiveParams().limit.gainDb >= rig.params.limit.gainDb;
                    deepestGuard = juce::jmin (deepestGuard, rig.probe().executedGuardDb);
                });

                const auto peak = rig.chain.getAndResetOutputTruePeakDb();
                logMessage ("  deepest executed protection offset " + juce::String (deepestGuard, 2) + " dB with manual gain 3 dB; true peak "
                            + juce::String (peak, 2) + " dBTP");
                expect (deepestGuard < 0.0f, "the guard did act");
                expect (neverBelow, "effective Limit Gain >= manual Limit Gain on every block");
                expect (peak <= rig.params.limit.ceilingDb + 0.01f, "true peak stays within the ceiling while the guard retreats");
            }

            {
                Rig rig (fastConstants(), makeMusic (defaultRate, 8.0, 26, 0.9f));
                rig.params.glazePercent = 80.0f;
                rig.params.limit.gainDb = 20.0f;   // 手动本身已让限幅器远超约束
                rig.run (8.0);

                const auto s = rig.status();
                expect (s.limitedByManual, "status: limited by the manual setting");
                expectEquals (rig.probe().executedBiasDb, 0.0f, "no calibration offset");
                expectEquals (rig.probe().executedGuardDb, 0.0f, "no protection offset");
                expectEquals (rig.chain.getEffectiveParams().limit.gainDb, rig.params.limit.gainDb, "Limit Gain is exactly the manual value");
            }
        }

        // 14. 输出非有限值：链清零、整链重启淡入并告诉处理器（返回 true），编排器按宿主 reset 清运行状态、保留已提交版本；
        //     IN 表开着响度计量（GLAZE 的分析器与校准器要它的子块）。
        // 依据：CeladonChain.h「返回 true = 这一块输出出现了非有限值」；PHASE2 A1 → A4「IN 表必须开 LUFS」。
        void testNonFiniteOutputAndInputMeter()
        {
            beginTest ("non-finite output: cleared, restarted, reported; the IN meter measures loudness");

            Rig rig (fastConstants(), makeMusic (defaultRate, 8.0, 27, 0.5f));
            rig.params.glazePercent = 40.0f;
            rig.run (1.0);

            expect (rig.chain.getInputMeter().isLoudnessEnabled(), "IN meter has loudness on");
            expect (rig.chain.getInputMeter().getProcessedSamples() > 0);

            rig.buffer.setSize (numChannels, blockSize, false, false, true);
            for (int ch = 0; ch < numChannels; ++ch)
                for (int i = 0; i < blockSize; ++i)
                    rig.buffer.setSample (ch, i, std::numeric_limits<float>::infinity());

            rig.chain.setParams (rig.params, false);
            const auto resets = rig.probe().hostResets;
            auto reported = rig.chain.process (rig.buffer);

            // Clay 让整链多了固定延迟 D + G（线性相位收单 D，ADR-012；峰值守护前瞻 G，ADR-014；48 kHz 574 + 528 = 1102 样本，
            // 比一块 512 长）：Balance 已被 Inf 污染，但非有限值要晚 D + G 个样本才到输出，所以检测最迟在「延迟 / 块长 + 1」块之后。
            // 断言的是这个上限内一定检测到——不是放宽检测本身。
            const auto blocksToReachOutput = rig.chain.getLatencySamples() / rig.blockLength + 1;
            int extraBlocks = 0;

            while (! reported && extraBlocks < blocksToReachOutput)
            {
                reported = rig.step();
                ++extraBlocks;
            }

            logMessage ("  non-finite reached the output after " + juce::String (extraBlocks) + " more block(s) (latency "
                        + juce::String (rig.chain.getLatencySamples()) + " samples)");
            expect (reported, "the chain reports non-finite output within the chain latency");
            bool cleared = true;
            for (int ch = 0; ch < numChannels; ++ch)
                for (int i = 0; i < blockSize; ++i)
                    cleared = cleared && rig.buffer.getSample (ch, i) == 0.0f;

            expect (cleared, "output cleared");
            expectEquals (rig.probe().hostResets, resets + 1, "the controller cleared its runtime state");

            bool finiteAfter = true, reportedAfter = false;
            for (int b = 0; b < 50; ++b)
            {
                reportedAfter = reportedAfter || rig.step();

                for (int ch = 0; ch < numChannels; ++ch)
                    for (int i = 0; i < blockSize; ++i)
                        finiteAfter = finiteAfter && std::isfinite (rig.buffer.getSample (ch, i));
            }

            expect (finiteAfter && ! reportedAfter, "recovers on the next blocks");
        }

        // 15. 音频线程零分配：一条链走完实时画像跟随、推热（预算环退让）、换风格、跳播 / 回绕 / reset、离线切换、
        //     读取存档（解析在消息线程、计数区外）、GLAZE 回 0 再拧起，setHostInfo / setParams / process / getStatus 都不经 operator new。
        // 依据：设计规格-总体「音频线程零分配、零锁」；界面与实现第 4 节。
        void testAudioThreadDoesNotAllocate()
        {
            beginTest ("setHostInfo / setParams / process / getStatus never allocate through every GLAZE path");

            // 冲击风格、跑起来后推热 14 dB：预算环会退让。
            Rig rig (fastConstants(), makeMusic (defaultRate, 8.0, 28, 0.2f), defaultParams (80.0f, 2));

            Rig donor (fastConstants(), makeMusic (defaultRate, 8.0, 29, 0.4f));
            donor.params.glazePercent = 50.0f;
            donor.run (6.0);
            const auto savedState = donor.chain.createGlazeStateXml();

            const auto totalBlocks = rig.blocksFor (60.0);
            std::uint64_t allocations = 0;
            bool finite = true;

            for (int b = 0; b < totalBlocks; ++b)
            {
                // ---- 计数区外：准备这一块（换素材电平、宿主事件、读取存档的解析）----
                if (b == rig.blocksFor (8.0))  rig.sourceGain = 5.0f;           // 推热：预算环退让
                if (b == rig.blocksFor (20.0)) rig.params.glazeStyle = 1;        // 换风格
                if (b == rig.blocksFor (24.0)) rig.position += 96000;            // 跳播
                if (b == rig.blocksFor (25.0)) { rig.host.hasLoop = rig.host.looping = true; rig.position -= 24000; }
                if (b == rig.blocksFor (26.0)) rig.host.offline = true;
                if (b == rig.blocksFor (28.0)) rig.host.offline = false;
                if (b == rig.blocksFor (40.0)) rig.params.glazePercent = 0.0f;
                if (b == rig.blocksFor (42.0)) rig.params.glazePercent = 65.0f;
                if (b == rig.blocksFor (50.0)) rig.chain.restoreGlazeStateXml (savedState.get());

                const auto length = (size_t) rig.source.front().size();
                rig.buffer.setSize (numChannels, blockSize, false, false, true);
                for (int ch = 0; ch < numChannels; ++ch)
                    for (int i = 0; i < blockSize; ++i)
                        rig.buffer.setSample (ch, i, rig.sourceGain * rig.source[(size_t) ch][(rig.sourceIndex + (size_t) i) % length]);
                rig.sourceIndex = (rig.sourceIndex + (size_t) blockSize) % length;
                rig.host.positionSamples = rig.position;

                // ---- 计数区：只夹住音频线程的入口（与界面线程的 getStatus）----
                const auto before = allocationCountOnThisThread();

                if (b == rig.blocksFor (35.0))
                    rig.chain.restartWithFadeIn();

                rig.chain.setHostInfo (rig.host);
                rig.chain.setParams (rig.params, false);
                rig.chain.process (rig.buffer);
                juce::ignoreUnused (rig.chain.getGlazeStatus());

                allocations += allocationCountOnThisThread() - before;

                rig.position += blockSize;

                for (int ch = 0; ch < numChannels; ++ch)
                    for (int i = 0; i < blockSize; ++i)
                        finite = finite && std::isfinite (rig.buffer.getSample (ch, i));
            }

            const auto probe = rig.probe();
            logMessage ("  paths exercised: resets " + juce::String (probe.hostResets)
                        + ", jumps " + juce::String (probe.transportJumps) + ", wraps " + juce::String (probe.loopWraps));

            expect (finite, "output finite throughout");
            expect (probe.transportJumps >= 1 && probe.loopWraps >= 1 && probe.hostResets >= 1,
                    "the run still covers jumps, loop wraps and a host reset");
            expectEquals ((juce::int64) allocations, (juce::int64) 0, "audio thread must not allocate");
        }
    };

    static GlazeControllerTests glazeControllerTests;

}

//==============================================================================
// ---- T-097 G_trim 冻结（A2 追加；T-108 删掉落差 / PSR 两扇守门后只剩预算环）----
// G_trim 抗积分饱和的冻结口径。都经 CeladonChain 驱动（与插件同一接线）。依据：设计规格-实时GLAZE-主段与增益 第 4 节（下称「规格 4」）；
// Max 2026-10-10 Q11「有退让量就冻结」。另起一个测试类（GlazeControllerTrimFreeze；T-108 前叫 GlazeControllerGuardFamily）。
//   1a. G_trim 冻结（规格 4 + Q11）：预算环退让量 < 0、或本块改过退让量、或 1 s 内改过参数 → 本块 G_trim 不积分（appliedBiasDb 逐位
//       不变）；trimFrozen 与这三条原因逐块一致。块长 480（= 预算环 10 ms 时隙）：链从不在块内切片，探针即那一次 endBlock 的结果。
//   1b. 改参数冻结（规格 4「参数在 1 s 内变化过时冻结 G_trim」）：GLAZE 100 → 90 之后 100 块（1 s）内冻结、逐位不变、距改参数 < 1 s；
//       之后退让为 0 时 G_trim 恢复积分。
//   5.  已删守门的存档节点（T-108）：新档写 0.1.8.10 读得懂的占位；T-097 / 3a7a367 形状、没有、坏数据都不冷启动、声音一样。
//   6.  音频线程零分配（设计规格-总体「音频线程零分配、零锁」）：推热、冻结、跳播、回绕、宿主 reset、GLAZE 0、读档。
//   （原 2 ～ 4：通用档两个新守门 +0、100 ms 子块网格、冻结与不连续时子块的推进——随两个守门删掉。）
// 场景的数（素材电平、推热倍数、时长）都集中在各用例开头的具名常量里，注释写明为什么够用、DGX 2026-10-23 的实测值。
namespace celadon::test
{
    namespace
    {
        // 块长 480 = 预算环一个 10 ms 时隙（48 kHz）；100 ms 子块 = 10 个这样的块。时隙只按有效音乐计数、冻结时丢掉半格，但每片都是整
        // 480 个样本，所以每块开头时隙进度总是 0：链从不在块内再切（getLastProcessSliceCountForTesting = 1），每块正好一次 endBlock——
        // 「上一块 endBlock 之后」的探针就是 G_trim 这一步的判定结果。块长 512 时几乎每块都切成两片，片内的判定从块外看不到。
        constexpr int t097TrimBlock = 480;

        // 规格 4：参数在 1 s 内变化过时冻结 G_trim（GlazeController.cpp changeFreezeSeconds）。
        constexpr double t097ParamFreezeSeconds = 1.0;

        // −3.0f 在存档里的写法（xmlio：float 写 IEEE 位的 8 位小写十六进制）：符号 1、指数 128（0x80）、尾数 .1b → 0xc0400000。
        // 这是 0.1.8.10 两个守门的底，新档给它写的占位节点（GlazeController.cpp legacyContrastGuardStub / legacyPsrGuardStub）floorDb 恒是这个值。
        constexpr const char* t097LegacyFloorHex = "c0400000";

        // 「放宽预算让守门不动」：预算环核对的七项上限（Glue 均值 / P95、Clip 深度 / 活动、限幅均值 / P95 / 峰值）在三种风格里都置 0。
        // GlazeGuard 与 GlazePlanner 的 enforced(cap) = 有限且 > 0，0 = 不设约束（编排器 replan 放开手动超预算的项也是置 0）；
        // 俱乐部档只给 > 0 的限幅上限加量，0 仍是 0。
        glaze::Constants t097WithoutBudgetCaps (glaze::Constants c) noexcept
        {
            for (auto* style : { &c.warm, &c.punchy, &c.clean })
            {
                style->glueGrMeanCapDb = style->glueGrP95CapDb = 0.0f;
                style->clipDepthCapDb = style->clipActivityCap = 0.0f;
                style->limitGrMeanCapDb = style->limitGrP95CapDb = style->limitGrPeakCapDb = 0.0f;
            }

            return c;
        }

        // 预算环有退让量（guardDb；在退、停住、回程中都 < 0）。T-108 起只剩这一扇守门（落差 / PSR 两扇删掉）。
        bool t097AnyRelief (const Probe& p) noexcept
        {
            return p.guardDb < 0.0f;
        }

        // 退让量与上一块不同（位比较）。
        bool t097ReliefMoved (const Probe& now, const Probe& before) noexcept
        {
            return ! sameBits (now.guardDb, before.guardDb);
        }

        // 退让量是 +0（位模式；−0 也算不一致）。
        bool t097AllReliefsZero (const Probe& p) noexcept
        {
            return sameBits (p.guardDb, 0.0f);
        }

        // GLAZE_STATE（容器格式 2）里压缩的 LIVE 解开成 LIVE_BODY；没有或解不开返回空。
        std::unique_ptr<juce::XmlElement> t097LiveBody (const juce::XmlElement& state)
        {
            const auto* live = state.getChildByName (glaze::liveTag);
            return live != nullptr ? glaze::decompressLiveBody (*live) : std::unique_ptr<juce::XmlElement> {};
        }

        // 两份 LIVE_BODY 的同名子节点逐属性相等（属性顺序也要相同：写出顺序是固定的）。
        bool t097SameChild (const juce::XmlElement* a, const juce::XmlElement* b, juce::StringRef tag)
        {
            const auto* x = a != nullptr ? a->getChildByName (tag) : nullptr;
            const auto* y = b != nullptr ? b->getChildByName (tag) : nullptr;
            return x != nullptr && y != nullptr && x->isEquivalentTo (y, false);
        }
    }

    class GlazeControllerTrimFreezeTests  : public juce::UnitTest
    {
    public:
        GlazeControllerTrimFreezeTests() : juce::UnitTest ("GlazeControllerTrimFreeze", "Celadon") {}

        void runTest() override
        {
            testTrimFreezesWhileTheBudgetGuardHolds();
            testTrimFreezesForOneSecondAfterAParamChange();
            testRetiredGuardNodes();
            testTrimLoopNeverAllocates();
        }

        // 1a. 依据：规格 4「守门退让量 < 0（T-108 起只有预算环；在退、停住、回程中都算）、或这一块刚改过退让量、或 1 s 内改过
        //     参数 → 不积分」；Max 2026-10-10 Q11「有退让量就冻结」。GlazeController.cpp endBlock：trimFrozen = guardHolding || recentlyChanged；
        //     guardHolding = 预算环本块在退 || 退让量 < 0 || 本块变了；只有 trimFrozen 为假时 gTrim 才积分；appliedBiasDb = gTrim。
        //     断言（每块；块长 480，一块一片）：
        //       - 有退让量 ⇒ trimFrozen（「trimFrozen 与有退让量一致」）；
        //       - trimFrozen ⇔ 有退让量 || 退让量与上一块不同 || 上一块之后 secondsSinceParamChange < 1 s（本段不改参数，beginBlock 不清零，
        //         这一块判定时的「距改参数」就是上一块之后的值）；预算环「本块在退」= g_保护本块变小，已含在「退让量变了」里；
        //       - 有退让量或 trimFrozen ⇒ appliedBiasDb 与上一块逐位相同。
        void testTrimFreezesWhileTheBudgetGuardHolds()
        {
            beginTest ("G_trim holds bit-exactly on every block that ends with any guard relief below 0 (Q11); trimFrozen matches its reasons");

            // 推热配方同主类 testManualComesFirst（紧预算、冲击风格、makeMusic 种子 25 峰值 0.3、×8；那里另加手动 Gain 3 dB，
            // 8 s 内保护层必退）。这里不加手动 Gain，限幅器入口少 3 dB：峰值 0.3 × 8 = 2.4（+7.6 dBFS）对 −1 dBTP 的 Ceiling 仍有约 8.6 dB
            // 峰值 GR，远超冲击风格紧预算的峰值 3 dB / P95 2 dB，预期 1 ～ 2 s 内预算环开始退、之后一直有退让量。
            // T-106（ADR-038）：预热从 2 s 加到 200 s。G_trim 只在方案到位之后积分（GlazeController.cpp planInPlace），这份素材约 58 s
            // 到位、攒够 3 s 证据后约 61.5 s 起积；预热 2 s 时整段 G_trim 都是 +0、不冻结也不会动，「逐位不变」证明不了冻结。预热到它
            // 在积之后再推热：限幅吃掉响度、实测净增益比方案少、误差为正，G_trim 按 0.05 dB/s 往上走，不到 1 s 预算环起退把它拦下。
            // 前提：推热前 2 s 里 G_trim 在动；起退之前、直到起退的前一块它都在动——起退那一块起逐位不变才是冻结拦下的。
            // （为什么是 200 s 而不是刚过 61.5 s：见本用例后半段「放回 0 之后恢复积分」。）
            // DGX 2026-11-12 实测见用例日志：推热前 G_trim +0.212 dB，推热后 0.81 s（81 块，块块在动）积到 +0.253 dB 时预算环起退，
            // 之后 1200 块里 1119 块有退让、全部冻结；预算环最深 −3.000 dB。
            constexpr float hotGain = 8.0f;
            constexpr double warmSeconds = 200.0;
            constexpr double movingWindowSeconds = 2.0;
            constexpr double hotSeconds = 12.0;

            Rig rig (withTightLimitBudgets (fastConstants()), makeMusic (defaultRate, 8.0, 25, 0.3f), defaultParams (100.0f, 2),
                     defaultRate, t097TrimBlock);
            const auto warmBlocks = rig.blocksFor (warmSeconds);
            const auto movingFrom = warmBlocks - rig.blocksFor (movingWindowSeconds);
            auto previous = rig.probe();
            auto previousMoved = false;
            int movingBlocks = 0;

            for (int b = 0; b < warmBlocks; ++b)
            {
                rig.step();
                const auto p = rig.probe();
                previousMoved = ! sameBits (p.appliedBiasDb, previous.appliedBiasDb);

                if (b >= movingFrom && ! p.trimFrozen && previousMoved)
                    ++movingBlocks;

                previous = p;
            }

            rig.sourceGain = hotGain;

            const auto trimAtStart = previous.appliedBiasDb;
            auto singleSlice = true, reliefFreezes = true, reasonsMatch = true, heldBitExact = true;
            auto seenRelief = false, integratingAtOnset = false;
            int reliefBlocks = 0, frozenBlocks = 0, firstFailure = -1, movedBeforeOnset = 0, onsetBlock = -1;
            auto lowestBudget = 0.0f;
            auto trimAtOnset = trimAtStart;

            rig.run (hotSeconds, [&] (int b)
            {
                const auto p = rig.probe();
                const auto oneSlice = rig.chain.getLastProcessSliceCountForTesting() == 1;
                const auto moved = ! sameBits (p.appliedBiasDb, previous.appliedBiasDb);
                singleSlice = singleSlice && oneSlice;

                if (oneSlice)
                {
                    const auto relief = t097AnyRelief (p);
                    const auto expectedFrozen = relief || t097ReliefMoved (p, previous)
                                             || previous.secondsSinceParamChange < t097ParamFreezeSeconds;
                    const auto freezesOk = ! relief || p.trimFrozen;
                    const auto reasonOk = p.trimFrozen == expectedFrozen;
                    const auto heldOk = ! (relief || p.trimFrozen) || ! moved;
                    reliefFreezes = reliefFreezes && freezesOk;
                    reasonsMatch = reasonsMatch && reasonOk;
                    heldBitExact = heldBitExact && heldOk;

                    if (! (freezesOk && reasonOk && heldOk) && firstFailure < 0)
                        firstFailure = b;

                    if (! seenRelief)
                    {
                        if (relief)
                        {
                            seenRelief = true;
                            onsetBlock = b;
                            integratingAtOnset = previousMoved;   // 起退的前一块 G_trim 还在动
                            trimAtOnset = p.appliedBiasDb;
                        }
                        else
                        {
                            movedBeforeOnset += moved ? 1 : 0;
                        }
                    }

                    reliefBlocks += relief ? 1 : 0;
                    frozenBlocks += p.trimFrozen ? 1 : 0;
                }

                lowestBudget = juce::jmin (lowestBudget, p.guardDb);
                previousMoved = moved;
                previous = p;
            });

            logMessage ("  hot material x" + juce::String (hotGain, 1) + ": blocks with a relief " + juce::String (reliefBlocks) + " / frozen "
                        + juce::String (frozenBlocks) + " of " + juce::String (rig.blocksFor (hotSeconds)) + "; lowest budget relief "
                        + juce::String (lowestBudget, 3) + " dB; G_trim " + juce::String (trimAtStart, 5) + " dB at the push, moved on "
                        + juce::String (movedBeforeOnset) + " blocks before the first relief (block " + juce::String (onsetBlock) + ", "
                        + juce::String (trimAtOnset, 5) + " dB), " + juce::String (previous.appliedBiasDb, 5) + " dB at the end; first failing block "
                        + juce::String (firstFailure));
            expect (singleSlice, "fixture: with 480-sample blocks every host block is exactly one slice (one endBlock per probe)");
            expect (movingBlocks > 0, "fixture: before the push G_trim does integrate on unfrozen blocks (moving blocks "
                                      + juce::String (movingBlocks) + ")");
            expect (movedBeforeOnset > 0 && integratingAtOnset,
                    "fixture: G_trim is integrating right up to the block where the budget ring starts to retreat (so holding it from that "
                    "block on is the freeze at work, not a coincidence)");
            expect (std::abs (trimAtOnset) < 3.0f - 0.05f, "fixture: G_trim sits away from its +-3 dB clamp when the freeze starts");
            expect (reliefBlocks > rig.blocksFor (1.0), "fixture: the hot material keeps the budget guard retreated for more than a second");
            expect (reliefFreezes, "any relief below 0 after a block means trimFrozen on that block");
            expect (reasonsMatch, "trimFrozen == relief below 0 || a relief changed this block || a parameter changed within 1 s");
            expect (heldBitExact, "a block that ends with a relief below 0 (or trimFrozen) leaves appliedBiasDb bit-identical");

            // ---- 放回 0 之后恢复积分（复查第 2 轮 R2-F4：完成标准第 1 条「守门全部放回 0 且参数 1 s 没动后恢复积分」）----
            // 素材回到原增益：限幅 GR 回到预算以内，预算环安全满 5 s 后以 0.3 dB/s 回（GlazeGuard），约 −3 dB 要 15 s 左右回到 +0。
            // 断言：回到 0 的那一块（上一块还有退让量、这一块是 +0）退让量变了 → 仍 trimFrozen、appliedBiasDb 逐位不变；
            // 之后第一个「退让量是 +0、上一块也是、不冻结」的块（这一段不改参数，secondsSinceParamChange 一直封顶在 1 s）
            // appliedBiasDb 变了：G_trim 恢复积分。
            // T-106：G_trim 积分另要这一块是主段、方案到位、证据满 3 s（探针 trimMainNow / trimEvidenceSeconds；前提一并断言）。
            // 预热取 200 s 也是为这个：预热短时，×8 那 12 s 把实时画像的参考电平抬得太高，放回后的素材要过几十秒才又算主段，
            // 退让回到 0 时 G_trim 还积不了（DGX 2026-11-12 探查，每 5 s 记一行：预热 75 s，放回后 60 ～ 65 s 才又动；100 s 是
            // 40 ～ 45 s；150 s 时证据满 3 s（19.62 s）只比退让回到 +0（21.39 s）早 1.8 s）。预热 200 s：放回后 0.91 s 又是主段且
            // 方案到位、3.90 s 证据满 3 s，20.96 s 退让回到 +0，下一块（20.97 s）就积。这时方案的净增益是负的（画像里记着那 12 s
            // 的响），实测净增益约 0，G_trim 往下走，把自动加的那零点一 dB 抵完为止（usableTrimRange）。
            constexpr double releaseWithinSeconds = 40.0;
            constexpr double minimumEvidenceSeconds = 3.0;   // GlazeController.cpp trimEvidenceMinimumSeconds
            rig.sourceGain = 1.0f;
            previous = rig.probe();
            auto zeroAt = -1, resumeAt = -1;
            auto frozenAtZero = false, heldAtZero = false, movedOnResume = false, gateOpenOnResume = false;
            auto evidenceOnResume = 0.0;
            const auto releaseBlocks = rig.blocksFor (releaseWithinSeconds);

            for (int b = 0; b < releaseBlocks && resumeAt < 0; ++b)
            {
                rig.step();
                const auto p = rig.probe();

                if (zeroAt < 0)
                {
                    if (t097AnyRelief (previous) && t097AllReliefsZero (p))
                    {
                        zeroAt = b;
                        frozenAtZero = p.trimFrozen;
                        heldAtZero = sameBits (p.appliedBiasDb, previous.appliedBiasDb);
                    }
                }
                else if (t097AllReliefsZero (previous) && t097AllReliefsZero (p) && ! p.trimFrozen)
                {
                    resumeAt = b;
                    movedOnResume = ! sameBits (p.appliedBiasDb, previous.appliedBiasDb);
                    evidenceOnResume = p.trimEvidenceSeconds;
                    gateOpenOnResume = p.trimMainNow && p.trimEvidenceSeconds >= minimumEvidenceSeconds;
                }

                previous = p;
            }

            logMessage ("  back at x1: all reliefs back to +0 at " + juce::String ((double) (zeroAt + 1) * t097TrimBlock / defaultRate, 2)
                        + " s (frozen " + juce::String ((int) frozenAtZero) + "), first unfrozen block "
                        + juce::String (resumeAt < 0 ? -1.0 : (double) (resumeAt - zeroAt) * t097TrimBlock / defaultRate, 2) + " s later (main section with "
                        + juce::String (evidenceOnResume, 2) + " s of evidence: " + juce::String ((int) gateOpenOnResume) + "), G_trim "
                        + juce::String (previous.appliedBiasDb, 5) + " dB");
            expect (zeroAt >= 0, "fixture: back at the original gain the budget ring releases to +0 within " + juce::String (releaseWithinSeconds, 0) + " s");
            expect (frozenAtZero && heldAtZero, "the block whose relief returns to 0 is still trimFrozen and leaves appliedBiasDb bit-identical");
            expect (resumeAt == zeroAt + 1, "the very next block (every relief +0 on both, no parameter change) is not frozen");
            expect (gateOpenOnResume, "fixture: on that block the open segment is a main section with at least 3 s of evidence (T-106's own gates are open)");
            expect (movedOnResume, "... and G_trim integrates on it (appliedBiasDb changes)");
        }

        // 1b. 依据：规格 4「参数在 1 s 内变化过时冻结 G_trim」（beginBlock 的 changed：风格 / 曲风 / 交付 / GLAZE / 规划相关手动值）；
        //     GlazeController.cpp：changed 时 secondsSinceChange = 0；endBlock 判定 recentlyChanged = secondsSinceChange < 1 之后才 += dt。
        //     推导（块长 480、一块一片，dt = 480 / 48000 = 0.01 s）：改参数那一块记为第 1 块，第 k 块判定时距改参数 = (k − 1) × 0.01：
        //       - k = 1 ～ 100：≤ 0.99 < 1 → 冻结，appliedBiasDb 与改之前逐位相同；
        //       - 第 k 块之后探针 secondsSinceParamChange = min(1, k × 0.01)：k ≤ 99 时 ≤ 0.99 < 1；第 1 块之后正好是 dt（同一个算式）；
        //       - k = 101：判定时是 100 个 0.01 的累加，舍入可能差一个 ulp 落在 1 的任一边，不断言；k ≥ 102 一定不再因改参数冻结。
        //     对照：放宽预算（预算环不设约束），退让量是 +0，冻结只能来自改参数。
        void testTrimFreezesForOneSecondAfterAParamChange()
        {
            beginTest ("a planning parameter change freezes G_trim for 1 s; afterwards, with every guard at 0, G_trim integrates again");

            // 素材峰值 0.2、放宽预算：限幅器不动作，链给的净增益就是方案的净增益，G_trim 只剩 Limit Gain 死区里差的那零点一 dB 要补。
            // T-106（ADR-038）：热身从 6 s 加到 80 s。G_trim 只在方案到位之后积分（GlazeController.cpp planInPlace），这份素材 Limit Gain
            // 以 0.2 dB/s 爬约 63 s 才进死区，攒够 3 s 证据后 66.01 s 起积；热身 6 s 时它一直是 +0，「改参数后逐位不变」证明不了冻结。
            // 首块不算改参数（复查 F1）；最后 2 s 里应能看到不冻结且 G_trim 在动的块。DGX 2026-11-12 实测：改参数时 G_trim +0.09274 dB；
            // GLAZE 100 → 90 后 Limit Gain 以 1 dB/s 往下走约 1.1 s 进新的死区（这段时间方案没到位，本来也不积），冻结满 1 s、
            // 方案到位后约 0.1 s 恢复积分，往 −0.10 dB 走（新死区里差的那一点换了方向）。T-106 之前这里是从 +0 按速率上限一路积：
            // 改参数时 0.30000 dB。日志里的 secondsSinceParamChange 是探针封顶后的 1 s（复查第 2 轮 R2-F6）。
            constexpr double warmSeconds = 80.0;
            constexpr double movingWindowSeconds = 2.0;
            constexpr double resumeWithinSeconds = 3.0;

            const auto constants = t097WithoutBudgetCaps (fastConstants());

            Rig rig (constants, makeMusic (defaultRate, 8.0, 33, 0.2f), defaultParams (100.0f, 0), defaultRate, t097TrimBlock);
            const auto blockSeconds = (double) t097TrimBlock / defaultRate;                                  // 0.01 s，同 endBlock 的 dt
            const auto freezeBlocks = (int) (t097ParamFreezeSeconds * defaultRate) / t097TrimBlock;           // 48000 / 480 = 100
            const auto warmBlocks = rig.blocksFor (warmSeconds);
            const auto movingFrom = warmBlocks - rig.blocksFor (movingWindowSeconds);

            auto previous = rig.probe();
            auto guardsQuiet = true;
            int movingBlocks = 0;

            for (int b = 0; b < warmBlocks; ++b)
            {
                rig.step();
                const auto p = rig.probe();
                guardsQuiet = guardsQuiet && t097AllReliefsZero (p);

                if (b >= movingFrom && ! p.trimFrozen && ! sameBits (p.appliedBiasDb, previous.appliedBiasDb))
                    ++movingBlocks;

                previous = p;
            }

            expect (guardsQuiet, "fixture: with no budget caps the budget relief stays +0");
            expect (movingBlocks > 0, "fixture: before the change G_trim does integrate on unfrozen blocks (moving blocks "
                                      + juce::String (movingBlocks) + ")");

            // ---- 改一个规划相关参数：GLAZE 100 → 90 ----
            const auto atChange = rig.probe();
            rig.params.glazePercent = 90.0f;
            auto frozenHeld = true, sinceBelowOne = true;
            double sinceAfterFirst = -1.0;

            for (int k = 1; k <= freezeBlocks; ++k)
            {
                rig.step();
                const auto p = rig.probe();
                frozenHeld = frozenHeld && p.trimFrozen && sameBits (p.appliedBiasDb, atChange.appliedBiasDb) && t097AllReliefsZero (p);

                if (k == 1)
                    sinceAfterFirst = p.secondsSinceParamChange;

                if (k < freezeBlocks)
                    sinceBelowOne = sinceBelowOne && p.secondsSinceParamChange < t097ParamFreezeSeconds;
            }

            expectEquals (sinceAfterFirst, blockSeconds, "the change block resets the clock to 0, its endBlock adds one block (0.01 s)");
            expect (std::abs (atChange.appliedBiasDb) < 3.0f - 0.05f, "fixture: G_trim sits away from its +-3 dB clamp at the change");
            expect (frozenHeld, "for " + juce::String (freezeBlocks) + " blocks (1 s) after the change: trimFrozen and appliedBiasDb bit-identical");
            expect (sinceBelowOne, "secondsSinceParamChange stays below 1 s for the first " + juce::String (freezeBlocks - 1) + " blocks");

            // 第 101 块正好落在 1 s 边界上（见上面的推导），跳过不断言。
            rig.step();
            previous = rig.probe();
            auto resumed = false;
            const auto searchBlocks = rig.blocksFor (resumeWithinSeconds);

            for (int b = 0; b < searchBlocks && ! resumed; ++b)
            {
                rig.step();
                const auto p = rig.probe();
                resumed = ! p.trimFrozen && t097AllReliefsZero (p) && ! sameBits (p.appliedBiasDb, previous.appliedBiasDb);
                previous = p;
            }

            logMessage ("  G_trim at the change " + juce::String (atChange.appliedBiasDb, 5) + " dB, now " + juce::String (previous.appliedBiasDb, 5)
                        + " dB; secondsSinceParamChange " + juce::String (previous.secondsSinceParamChange, 3));
            expect (resumed, "after the 1 s freeze, with every guard at 0, G_trim integrates again within "
                             + juce::String (resumeWithinSeconds, 0) + " s");
        }

        // 5. T-108（ADR-039 Q5 A）：落差守门与 PSR 守门删掉了。依据：GlazeController.cpp createStateXml / restoreBody。
        //    - 新档照旧写 CONTRAST_GUARD / PSR_GUARD 两个节点，内容是 0.1.8.10 读得懂的「没有退让」：退让 +0、底 −3（0.1.8.10 读档直接拿它
        //      当底）、没在退；落差守门的两只 128 笔环全 0、samplesUntilPush 4800，PSR 的 belowSeconds 0。新档拿回 0.1.8.10 打开不冷启动、不退。
        //    - 读档不再读这两个节点：T-097 形状（main fa381f2 写的：带 reliefDbExact、直方图与计时）、main 3a7a367 形状（0.1.8.10 写的；
        //      CONTROL 也没有 secondsSinceParamChange）、两个节点都没有、节点里是坏数据（NaN），都不冷启动，读回之后的声音与原样读回的新档
        //      逐位相同；再存时两个节点回到上面的占位。旧工程里存着的落差 / PSR 退让量就此作废（声音会变，属于 Q2 认可的变化）。
        //    「不冷启动」的判据同主类 testLiveRoundTrip：增量回到存档值、LIVE 画像 I 仍有效、没有旧工程提示。
        void testRetiredGuardNodes()
        {
            beginTest ("T-108: a new LIVE writes 0.1.8.10 placeholders for the retired guards; T-097, 3a7a367, missing or broken guard "
                       "nodes all restore without a cold start and sound the same");

            constexpr double donorSeconds = 24.0;
            constexpr double afterSeconds = 2.0;
            const auto material = makeMusic (defaultRate, 20.0, 43, 0.4f);
            const auto params = defaultParams (40.0f);

            Rig donor (fastConstants(), material, params);
            donor.tempoBpm = 120.0;
            donor.run (donorSeconds);
            const auto saved = donor.chain.createGlazeStateXml();
            const auto donorProbe = donor.probe();
            const auto donorI = donor.chain.getLiveProfile().getProfile().integratedLufs;
            auto savedBody = saved != nullptr ? t097LiveBody (*saved) : std::unique_ptr<juce::XmlElement> {};
            expect (savedBody != nullptr && donorI.valid, "fixture: the donor has a LIVE profile");

            if (savedBody == nullptr)
                return;

            // ---- 新档的占位节点（xmlio：float 写 8 位十六进制、double 16 位，int / bool 写十进制）----
            const auto* contrast = savedBody->getChildByName ("CONTRAST_GUARD");
            const auto* psr = savedBody->getChildByName ("PSR_GUARD");
            expect (contrast != nullptr && psr != nullptr, "new LIVE: CONTRAST_GUARD and PSR_GUARD are still written (for 0.1.8.10)");

            if (contrast == nullptr || psr == nullptr)
                return;

            const auto zeroRing = juce::String::repeatedString ("0", 128 * 8);
            expectEquals (contrast->getNumAttributes(), 8, "CONTRAST_GUARD: exactly the 0.1.8.10 attributes");
            expectEquals (psr->getNumAttributes(), 4, "PSR_GUARD: exactly the 0.1.8.10 attributes");
            expect (contrast->getStringAttribute ("reliefDb") == "00000000" && psr->getStringAttribute ("reliefDb") == "00000000",
                    "placeholders: relief +0");
            expect (contrast->getStringAttribute ("floorDb") == t097LegacyFloorHex && psr->getStringAttribute ("floorDb") == t097LegacyFloorHex,
                    "placeholders: floorDb -3 dB (the value 0.1.8.10 reads as its floor)");
            expect (contrast->getStringAttribute ("engaged") == "0" && psr->getStringAttribute ("engaged") == "0", "placeholders: not engaged");
            expect (contrast->getStringAttribute ("ringCount") == "0" && contrast->getStringAttribute ("ringWrite") == "0"
                        && contrast->getStringAttribute ("samplesUntilPush") == "4800"
                        && contrast->getStringAttribute ("inputRing") == zeroRing && contrast->getStringAttribute ("outputRing") == zeroRing,
                    "placeholders: empty contrast rings, the next push one full 100 ms sub-block away");
            expect (psr->getStringAttribute ("belowSeconds") == "0000000000000000", "placeholders: PSR timer 0");

            // ---- create → restore → create：LIVE 里 T-097 之后的节点逐位相同 ----
            {
                Rig fresh (fastConstants(), material, params);
                fresh.chain.restoreGlazeStateXml (saved.get());
                const auto again = fresh.chain.createGlazeStateXml();
                auto againBody = again != nullptr ? t097LiveBody (*again) : std::unique_ptr<juce::XmlElement> {};

                for (const char* tag : { "CONTROL", "CONTRAST_GUARD", "PSR_GUARD", "OUTPUT_LEVEL" })
                    expect (t097SameChild (savedBody.get(), againBody.get(), tag), "new LIVE: " + juce::String (tag) + " round-trips bit-identically");
            }

            // 读档之前先走一块，再把 GLAZE 拧下 10 再拧回（各一块）：插件刚开的第一块不算改参数（复查 F1），要真改一次参数，
            // secondsSinceParamChange 才停在一块的时长；读档后看它是不是换成了存档里的值（缺的按 1 s）。
            const auto withBody = [&] (const juce::XmlElement& body)
            {
                auto state = std::make_unique<juce::XmlElement> (*saved);
                state->removeChildElement (state->getChildByName (glaze::liveTag), true);
                state->addChildElement (glaze::compressLiveBody (body, glaze::factoryConstants().version).release());
                return state;
            };

            struct Restored
            {
                Probe probe;
                bool profileValid = false;
                bool legacyNotice = false;
                std::unique_ptr<juce::XmlElement> body;   // 读回之后马上再存的 LIVE_BODY
                std::vector<float> output;                // 读回之后 afterSeconds 的输出（声道交错）
            };

            const auto restoreInto = [&] (const juce::XmlElement& state)
            {
                Restored result;
                Rig target (fastConstants(), material, params);
                target.tempoBpm = 120.0;
                target.step();
                target.params.glazePercent -= 10.0f;
                target.step();
                target.params.glazePercent = params.glazePercent;
                target.step();
                target.chain.restoreGlazeStateXml (&state);
                result.probe = target.probe();
                result.profileValid = target.chain.getLiveProfile().getProfile().integratedLufs.valid;
                result.legacyNotice = target.status().legacySessionDiscarded;

                if (const auto again = target.chain.createGlazeStateXml())
                    result.body = t097LiveBody (*again);

                target.run (afterSeconds, [&] (int)
                {
                    for (int i = 0; i < target.buffer.getNumSamples(); ++i)
                        for (int ch = 0; ch < numChannels; ++ch)
                            result.output.push_back (target.buffer.getSample (ch, i));
                });

                return result;
            };

            const auto reference = restoreInto (*saved);
            expect (reference.output.size() > 0, "fixture: the reference run after the restore produced audio");

            const auto check = [&] (const juce::XmlElement& body, const juce::String& label)
            {
                const auto state = withBody (body);
                const auto restored = restoreInto (*state);
                expect (sameIncrements (restored.probe.increments, donorProbe.increments), label + ": the applied increments are restored (no cold start)");
                expect (restored.profileValid, label + ": the LIVE profile is kept (I still valid: no cold start)");
                expect (! restored.legacyNotice, label + ": no legacy-session notice");
                expectEquals (restored.probe.secondsSinceParamChange, reference.probe.secondsSinceParamChange,
                              label + ": the change clock is the saved one (1 s when missing)");

                for (const char* tag : { "CONTRAST_GUARD", "PSR_GUARD" })
                    expect (t097SameChild (savedBody.get(), restored.body.get(), tag), label + ": saved again, " + juce::String (tag) + " is the placeholder");

                auto sameOutput = restored.output.size() == reference.output.size();

                for (size_t i = 0; sameOutput && i < restored.output.size(); ++i)
                    sameOutput = sameBits (restored.output[i], reference.output[i]);

                expect (sameOutput, label + ": the " + juce::String (afterSeconds, 0) + " s after the restore are bit-identical to the new LIVE restored as is");
            };

            // T-097 形状：带退让与计时（落差 −1.25 dB、PSR −0.75 dB，都在 reliefDbExact 里）——读档不看，声音同新档。
            {
                juce::XmlElement body (*savedBody);
                auto* c = body.getChildByName ("CONTRAST_GUARD");
                auto* p = body.getChildByName ("PSR_GUARD");
                c->setAttribute ("reliefDb", "bfa00000");
                c->setAttribute ("reliefDbExact", "bfa00000");
                c->setAttribute ("engaged", "1");
                c->setAttribute ("overSeconds", "4024000000000000");
                c->setAttribute ("underSeconds", "0000000000000000");
                c->setAttribute ("histogramSeconds", "4034000000000000");
                c->setAttribute ("inputHistogram", "00");
                c->setAttribute ("outputHistogram", "00");
                p->setAttribute ("reliefDb", "bf400000");
                p->setAttribute ("reliefDbExact", "bf400000");
                p->setAttribute ("engaged", "1");
                p->setAttribute ("aboveSeconds", "3ff0000000000000");
                check (body, "T-097 shape");
            }

            // main 3a7a367 形状：两个节点是 0.1.8.10 的属性（退让非零），CONTROL 没有 secondsSinceParamChange。
            {
                juce::XmlElement body (*savedBody);
                body.getChildByName ("CONTRAST_GUARD")->setAttribute ("reliefDb", "bfa00000");
                body.getChildByName ("PSR_GUARD")->setAttribute ("reliefDb", "bf400000");
                body.getChildByName ("CONTROL")->removeAttribute ("secondsSinceParamChange");
                check (body, "3a7a367 shape");
            }

            // 两个节点都没有。
            {
                juce::XmlElement body (*savedBody);
                body.removeChildElement (body.getChildByName ("CONTRAST_GUARD"), true);
                body.removeChildElement (body.getChildByName ("PSR_GUARD"), true);
                check (body, "no guard nodes");
            }

            // 坏数据（退让 NaN = 0x7fc00000）：T-097 时整份冷启动，现在不读、不冷启动。
            {
                juce::XmlElement body (*savedBody);
                body.getChildByName ("CONTRAST_GUARD")->setAttribute ("reliefDb", "7fc00000");
                body.getChildByName ("PSR_GUARD")->setAttribute ("reliefDb", "7fc00000");
                check (body, "broken guard nodes");
            }
        }

        // 6. 依据：设计规格-总体「音频线程零分配、零锁」。预算环退让、宿主旁通冻结、跳播、循环回绕、GLAZE 0 再拧起、宿主 reset、
        //    读档（解析在计数区外）。计数区只夹 setHostInfo / setParams / process / getStatus（同主类的写法）。
        //    T-108：原来另有 House 俱乐部一遍与换曲风 / 交付（守门开关开 ↔ 关），随曲风、交付与两个守门删掉。
        void testTrimLoopNeverAllocates()
        {
            beginTest ("setHostInfo / setParams / process / getStatus never allocate through freeze, jump, wrap, reset and restore (GLAZE 100)");

            // 紧预算 + 8 s 起推热 ×8（同用例 1a），预算环退让路径也走到。
            const auto constants = withTightLimitBudgets (fastConstants());
            const auto material = makeMusic (defaultRate, 8.0, 25, 0.3f);
            const auto params = defaultParams (100.0f, 2);

            Rig donor (constants, material, params);
            donor.run (6.0);
            const auto saved = donor.chain.createGlazeStateXml();
            expect (saved != nullptr, "fixture: donor state");

            if (saved == nullptr)
                return;

            Rig rig (constants, material, params);
            const auto result = sweepAllocations (rig, *saved, 8.0f);
            const auto p = rig.probe();
            logMessage ("  budget retreated " + juce::String ((int) result.budgetRetreated) + "; jumps " + juce::String (p.transportJumps)
                        + ", wraps " + juce::String (p.loopWraps) + ", resets " + juce::String (p.hostResets));
            expect (result.finite, "output finite throughout");
            expect (p.transportJumps >= 1 && p.loopWraps >= 1 && p.hostResets >= 1, "the run went through a jump, a loop wrap and a host reset");
            expect (result.budgetRetreated, "fixture: the budget guard retreated on the hot material");
            expectEquals ((juce::int64) result.allocations, (juce::int64) 0, "audio thread must not allocate");
        }

    private:
        struct SweepResult
        {
            std::uint64_t allocations = 0;
            bool finite = true;
            bool budgetRetreated = false;
        };

        // 95 s 的路径扫描（时间表见下）。计数区外：素材、宿主信息、事件与读档；计数区：setHostInfo / setParams / process / getStatus。
        SweepResult sweepAllocations (Rig& rig, const juce::XmlElement& saved, float hotGain)
        {
            SweepResult result;
            const auto total = rig.blocksFor (95.0);

            for (int b = 0; b < total; ++b)
            {
                // ---- 计数区外：准备这一块 ----
                if (b == rig.blocksFor (8.0))  rig.sourceGain = hotGain;                                   // 推热：预算环退让
                if (b == rig.blocksFor (80.0)) rig.position += 96000;                                     // 跳播
                if (b == rig.blocksFor (82.0)) { rig.host.hasLoop = rig.host.looping = true; rig.position -= 48000; }   // 循环回绕
                if (b == rig.blocksFor (84.0)) rig.host.hasLoop = rig.host.looping = false;
                if (b == rig.blocksFor (85.0)) rig.hostBypassed = true;                                   // 冻结
                if (b == rig.blocksFor (86.0)) rig.hostBypassed = false;
                if (b == rig.blocksFor (87.0)) rig.params.glazePercent = 0.0f;                            // GLAZE 0
                if (b == rig.blocksFor (88.0)) rig.params.glazePercent = 100.0f;
                if (b == rig.blocksFor (92.0)) rig.chain.restoreGlazeStateXml (&saved);                  // 读档（解析在计数区外）

                const auto length = (size_t) rig.source.front().size();
                rig.buffer.setSize (numChannels, blockSize, false, false, true);

                for (int ch = 0; ch < numChannels; ++ch)
                    for (int i = 0; i < blockSize; ++i)
                        rig.buffer.setSample (ch, i, rig.sourceGain * rig.source[(size_t) ch][(rig.sourceIndex + (size_t) i) % length]);

                rig.sourceIndex = (rig.sourceIndex + (size_t) blockSize) % length;
                rig.host.positionSamples = rig.position;
                rig.host.hasTempo = rig.host.hasPpq = rig.host.hasBarStart = rig.host.hasTimeSignature = rig.tempoBpm > 0.0;

                if (rig.tempoBpm > 0.0)
                {
                    rig.host.bpm = rig.tempoBpm;
                    rig.host.ppqPosition = (double) rig.position * rig.tempoBpm / (60.0 * defaultRate);
                    rig.host.ppqLastBarStart = std::floor (rig.host.ppqPosition / 4.0) * 4.0;
                }

                // ---- 计数区：只夹住音频线程的入口与界面线程的 getStatus ----
                const auto before = allocationCountOnThisThread();

                if (b == rig.blocksFor (89.0))
                    rig.chain.restartWithFadeIn();                                                        // 宿主 reset

                rig.chain.setHostInfo (rig.host);
                rig.chain.setParams (rig.params, rig.hostBypassed);
                rig.chain.process (rig.buffer);
                juce::ignoreUnused (rig.chain.getGlazeStatus());
                result.allocations += allocationCountOnThisThread() - before;

                rig.position += blockSize;

                for (int ch = 0; ch < numChannels; ++ch)
                    for (int i = 0; i < blockSize; ++i)
                        result.finite = result.finite && std::isfinite (rig.buffer.getSample (ch, i));

                result.budgetRetreated = result.budgetRetreated || rig.probe().guardDb < 0.0f;
            }

            return result;
        }

    };

    static GlazeControllerTrimFreezeTests glazeControllerTrimFreezeTests;
}


// ===== T-106（ADR-038）：G_trim 换误差口径 =====
// 只在文件末尾追加，另起一个测试类。上面随行为改写的现有用例各自标了 T-106：GlazeControllerTrimFreeze（原 GuardFamily）的 1a、1b，
// GlazeTests.cpp 的「响度随 GLAZE 上升」（原来还有 GuardFamily 的 5 与 GuardScenarios 的两条，T-108 随守门删掉）。
//
// GlazeControllerTrimLoop：环路行为。
//   1. 恒定素材：G_trim 不到钳位，收敛后不动。
//   2. 非主段里逐位不动；回到主段后接着积。
//   3. 改 GLAZE 之后 60 s 内的变化 ≤ 1 dB。
//   4. 前奏轻、后面响（通用曲风）：G_trim 全程很小——误差两侧用同一批段。
//   5. 自动增量还没到位的块不积分；到位后直接走向稳态，不先冲高。
//   6. 读到 0.1.8.10 形状的 OUTPUT_LEVEL：G_trim 从 +0 重新收敛、历史为空，其余不冷启动；新形状逐位往返。
//   7. 证据不足 3 s 不积分；满 3 s 的那一块起积。
//   8. 增益变了 3 dB 以上之后，旧增益下录的段不再当证据（仍是主段）。
//   9. 抗饱和：G_trim 用不上的时候（手动 Limit Gain 已在 24 dB）积分状态不动。
//  10. 回看重判：素材变响、参考跟上之后，之前录的段不再算主段。
//  11. 「方案到位」逐项看：Recover / Tame / Glue / Heat / Clip / Tone 任一项单独没到位，G_trim 都不动，到位后才积。
//  12. 一个宿主块切成多片时，到没到位按块首那份设置判一次、各片沿用。
//  13. 误差里的方案净增益用这一个宿主块执行的那份方案，不用这一块里刚重算出来的。
//  14. 读数折算用的 Limit Gain 自动增量是这一个宿主块执行的那份，不是这一块里被速率限制推过的。
//  15. 读档：CONTROL 里的浮点不是有限值、G_trim 超出 ±3 dB、CLAY 的 pressureScale 不是有限值，整份拒收（外部第二意见 F3）。
//  16. 出口电平的段跟着输入侧段落账切，不连续点处也是（外部第二意见 F1）。
//  17. 抗饱和的下界：G_trim 不低于「负的自动部分」；读回时已在范围外的，不往更外面走，也不被一下子拽回来。
//  18. 手动 Limit Gain 抬高、已施加的增量还没退到位（手动 + 增量 > 24 dB）：自动部分按钳进 24 dB 之后的算，G_trim 不多往下走。
// 前五条只用 T-106 之前就有的探针字段，同一份用例能原样放到修之前的代码（3a16132）上跑：在那里都失败（DGX 2026-11-12 实测，
// 数值写在各条的注释里），修之后都通过。第 6 ～ 18 条要读 T-106 的探针与存档属性，只在修之后的代码上有意义；它们各盯一个
// T-106 新加的条件，把那个条件改错（去掉、放宽、收紧）对应的用例就红（验证报告里有逐条的改错结果）。
// 除第 5、9、17 条外，素材与设置同 T-097「改参数冻结」那一条：makeMusic 种子 33、峰值 0.2、通用曲风、放宽预算——三个守门全程 +0，
// 限幅器不动作，链给的净增益就是方案的净增益，G_trim 的稳态只剩 Limit Gain 死区里差的那 ≤ 0.1 dB。
//
// 出口电平跟踪器本身（T-106 的读数、入口配对、回看重判、存读档）单独测，在 tests/OutputLevelTrackerTests.cpp。
namespace celadon::test
{
    namespace
    {
        class GlazeControllerTrimLoopTests  : public juce::UnitTest
        {
        public:
            GlazeControllerTrimLoopTests() : juce::UnitTest ("GlazeControllerTrimLoop", "Celadon") {}

            void runTest() override
            {
                testConstantMaterialSettles();
                testHoldsOutsideMainSections();
                testGlazeChangeDoesNotSwingTrim();
                testQuietIntroDoesNotPullTrim();
                testWaitsForThePlanToBeInPlace();
                testLegacyOutputLevelShape();
                testIntegratesOnlyOnThreeSecondsOfEvidence();
                testOldSegmentsStopCountingAfterAGainChange();
                testDoesNotWindUpWhereItCannotAct();
                testHistoryIsRejudgedAgainstTheCurrentReference();
                testWaitsForEveryIncrement();
                testJudgesInPlaceOncePerHostBlock();
                testErrorUsesThePlanTheBlockExecuted();
                testReferenceUsesTheIncrementTheBlockExecuted();
                testRestoreRejectsBadControlValues();
                testOutputSegmentsFollowTheInputLedger();
                testDoesNotWindUpBelowWhatItCanUse();
                testRaisedManualGainUsesTheClampedLimitGain();
            }

        private:
            static Rig makeQuietRig (float glazePercent = 100.0f, int block = blockSize)
            {
                return Rig (t097WithoutBudgetCaps (fastConstants()), makeMusic (defaultRate, 8.0, 33, 0.2f), defaultParams (glazePercent, 0),
                            defaultRate, block);
            }

            static juce::String db (float value)   { return juce::String (value, 4); }

            // 「方案到位」的规矩照探针里的量重算一遍（GlazeController.cpp planInPlace：有目标方案、Limit Gain 增量在死区里、
            // Recover / Tame / Glue / Heat / Clip 离目标 ≤ 1 个单位（trimPlanUnitTolerance）、Tone ≤ 0.1 dB（trimPlanToneToleranceDb））。
            static bool planInPlaceBy (const Probe& p)
            {
                constexpr float unitTolerance = 1.0f;
                constexpr float toneToleranceDb = 0.1f;

                if (! p.haveTargetPlan || ! p.gainInDeadband)
                    return false;

                const auto within = [] (float appliedValue, float targetValue, float tolerance)
                {
                    return std::isfinite (appliedValue) && std::isfinite (targetValue) && std::abs (targetValue - appliedValue) <= tolerance;
                };
                const auto& applied = p.increments;
                const auto& target = p.targetPlan.increments;
                return within (applied.balanceRecover, target.balanceRecover, unitTolerance) && within (applied.balanceTame, target.balanceTame, unitTolerance)
                    && within (applied.colorGlue, target.colorGlue, unitTolerance) && within (applied.colorHeat, target.colorHeat, unitTolerance)
                    && within (applied.limitClip, target.limitClip, unitTolerance) && within (applied.colorToneDb, target.colorToneDb, toneToleranceDb);
            }

            // 1. 依据：规格第 4 节第 3 条「G_trim 以 ≤ 0.05 dB/s 积分 e，钳在 ±3 dB」；链做到方案时 e 应为 0。
            //    修之后：全程 |G_trim| ≤ 0.11 dB，90 s 之后只动 0.003 dB。
            //    修之前：出口历史各段是在当时的增益下录的，G_trim 看不到自己的动作——60 s 推到 +3.00，约 100 s 起往回滑。
            void testConstantMaterialSettles()
            {
                beginTest ("constant material: G_trim stays far from the clamp and stops moving once settled");

                constexpr double runSeconds = 150.0;
                constexpr double settledFromSeconds = 90.0;
                constexpr float farFromClampDb = 0.5f;
                constexpr float settledRangeDb = 0.05f;

                auto rig = makeQuietRig();
                const auto settledFromBlock = rig.blocksFor (settledFromSeconds);
                auto largest = 0.0f;
                auto low = std::numeric_limits<float>::max(), high = std::numeric_limits<float>::lowest();
                auto integrated = false;

                rig.run (runSeconds, [&] (int b)
                {
                    const auto trim = rig.probe().appliedBiasDb;
                    largest = juce::jmax (largest, std::abs (trim));
                    integrated = integrated || trim != 0.0f;

                    if (b >= settledFromBlock)
                    {
                        low = juce::jmin (low, trim);
                        high = juce::jmax (high, trim);
                    }
                });

                logMessage ("  largest |G_trim| " + db (largest) + " dB; from " + juce::String (settledFromSeconds, 0) + " s on it stays within "
                            + db (low) + " .. " + db (high) + " dB");
                expect (integrated, "fixture: G_trim integrates at some point (the loop is live, not parked at 0)");
                expect (largest <= farFromClampDb, "with the chain delivering the planned gain G_trim stays within " + db (farFromClampDb)
                                                   + " dB of 0 (before T-106 it ran to the +3 dB clamp)");
                expect (high - low <= settledRangeDb, "once settled G_trim moves no more than " + db (settledRangeDb) + " dB in a minute");
            }

            // 2. 依据：T-106「只在此刻是主段、且这一块有主段样本进账时才积」（GlazeController.cpp endBlock 的 mainNow）。
            //    素材先放 110 s（方案到位、G_trim 在积），再 ×0.1（−20 dB，段落账暂判成非主段）30 s，再放回。
            //    断言：安静段开始 5 s 之后（留给正在开着的那一段翻成非主段）到结束，appliedBiasDb 逐位不变；放回后 30 s 内又动了。
            //    修之前：不看是不是主段，安静段里照积（这一段它正从 +3.00 往回滑）。
            void testHoldsOutsideMainSections()
            {
                beginTest ("outside main sections G_trim holds bit-exactly; back in a main section it integrates again");

                constexpr double musicSeconds = 110.0;
                constexpr double watchBeforeSeconds = 10.0;
                constexpr double flipSeconds = 5.0;
                constexpr double quietSeconds = 30.0;
                constexpr double resumeWithinSeconds = 30.0;
                constexpr float quietGain = 0.1f;

                auto rig = makeQuietRig();
                const auto watchFromBlock = rig.blocksFor (musicSeconds - watchBeforeSeconds);
                auto previous = 0.0f;
                auto movedBefore = false;

                rig.run (musicSeconds, [&] (int b)
                {
                    const auto trim = rig.probe().appliedBiasDb;
                    movedBefore = movedBefore || (b >= watchFromBlock && ! sameBits (trim, previous));
                    previous = trim;
                });

                rig.sourceGain = quietGain;
                rig.run (flipSeconds);
                const auto held = rig.probe().appliedBiasDb;
                auto heldBitExact = true, mainWhileQuiet = false;

                rig.run (quietSeconds - flipSeconds, [&] (int)
                {
                    heldBitExact = heldBitExact && sameBits (rig.probe().appliedBiasDb, held);
                    mainWhileQuiet = mainWhileQuiet || rig.status().currentMainness >= 0.5f;
                });

                rig.sourceGain = 1.0f;
                const auto resumed = rig.runUntil (resumeWithinSeconds, [&] { return ! sameBits (rig.probe().appliedBiasDb, held); });

                logMessage ("  G_trim " + db (previous) + " dB going into the quiet stretch, " + db (held) + " dB held through it, "
                            + db (rig.probe().appliedBiasDb) + " dB after the music is back");
                expect (movedBefore, "fixture: G_trim is integrating in the " + juce::String (watchBeforeSeconds, 0) + " s before the quiet stretch");
                expect (! mainWhileQuiet, "fixture: the quiet stretch is not a main section (currentMainness < 0.5 from "
                                          + juce::String (flipSeconds, 0) + " s in)");
                expect (heldBitExact, "G_trim is bit-identical on every block of the quiet stretch (before T-106 it kept integrating)");
                expect (resumed, "back in a main section G_trim integrates again within " + juce::String (resumeWithinSeconds, 0) + " s");
            }

            // 3. 依据：T-106 计划第 2 节「改参数后 60 s 内 G_trim 变化 ≤ 1 dB」。GLAZE 100 → 50：方案净增益与 Limit Gain 增量都减半。
            //    修之后：旧段的录制增益离新的参考增益 6 dB 多、不再当证据，新证据攒够 3 s 再积，G_trim 从 +0.10 走到 −0.10
            //    （两头都是死区里差的那一点），60 s 内变 0.20 dB。
            //    修之前：历史还是 GLAZE 100 时录的电平，目标已经低了 6 dB——60 s 内从 +3.00 掉到 +0.05（2.95 dB）。
            void testGlazeChangeDoesNotSwingTrim()
            {
                beginTest ("a GLAZE change moves G_trim by no more than 1 dB over the next 60 s");

                constexpr double beforeSeconds = 100.0;
                constexpr double afterSeconds = 60.0;
                constexpr float limitDb = 1.0f;

                auto rig = makeQuietRig();
                rig.run (beforeSeconds);
                const auto planBefore = rig.probe().targetPlan.effectiveGainDb;
                const auto atChange = rig.probe().appliedBiasDb;
                rig.params.glazePercent = 50.0f;
                auto low = atChange, high = atChange;

                rig.run (afterSeconds, [&] (int)
                {
                    const auto trim = rig.probe().appliedBiasDb;
                    low = juce::jmin (low, trim);
                    high = juce::jmax (high, trim);
                });

                const auto planAfter = rig.probe().targetPlan.effectiveGainDb;
                logMessage ("  planned net gain " + juce::String (planBefore, 3) + " -> " + juce::String (planAfter, 3) + " dB; G_trim "
                            + db (atChange) + " dB at the change, then within " + db (low) + " .. " + db (high) + " dB");
                expect (planBefore - planAfter > 3.0f, "fixture: halving GLAZE lowers the planned net gain by more than 3 dB");
                expect (high - low <= limitDb, "G_trim moves no more than " + db (limitDb) + " dB in the " + juce::String (afterSeconds, 0)
                                               + " s after the change (before T-106: 2.95 dB)");
            }

            // 4. 依据：T-106「误差 = 方案净增益 − 实测净增益，两侧是出口历史里的同一批段」。通用曲风的输入响度 I 是全部有效音频的门控
            //    积分（规格第 1 节表）：前奏 ×0.3（−10.5 dB）40 s 之后放回 ×1，I 还带着前奏、一两分钟才追上，出口历史里已经只剩响段。
            //    修之后：全程 |G_trim| ≤ 0.10 dB。只折算历史、仍拿出口电平去比「I + 方案净增益」时掉到 −1.27 dB、再用 100 s 回来。
            //    修之前：到 +3.00。
            void testQuietIntroDoesNotPullTrim()
            {
                beginTest ("quiet intro then the full level: G_trim does not chase the input loudness catching up");

                constexpr double introSeconds = 40.0;
                constexpr double bodySeconds = 160.0;
                constexpr float introGain = 0.3f;
                constexpr float limitDb = 0.5f;

                auto rig = makeQuietRig();
                auto low = 0.0f, high = 0.0f;
                auto integrated = false;
                const auto track = [&] (int)
                {
                    const auto trim = rig.probe().appliedBiasDb;
                    low = juce::jmin (low, trim);
                    high = juce::jmax (high, trim);
                    integrated = integrated || trim != 0.0f;
                };

                rig.sourceGain = introGain;
                rig.run (introSeconds, track);
                rig.sourceGain = 1.0f;
                rig.run (bodySeconds, track);

                logMessage ("  G_trim over the whole run within " + db (low) + " .. " + db (high) + " dB");
                expect (integrated, "fixture: G_trim integrates at some point");
                expect (low >= -limitDb && high <= limitDb, "G_trim stays within +-" + db (limitDb) + " dB (before T-106: +3.00; with the "
                                                            "history folded but the target still built on the profile's I: -1.27)");
            }

            // 5. 依据：T-106「方案还没到位的块不当证据、也不积分」（GlazeController.cpp planInPlace：Recover / Tame / Glue / Heat / Clip 的
            //    已施加量离目标 ≤ 1 个单位、Tone ≤ 0.1 dB、Limit Gain 进了死区）。出厂预算、素材峰值 0.5：冷启动时 Glue / Heat / Clip 与
            //    Limit Gain 一起爬，这段时间实测净增益比方案少 1.5 ～ 2.3 dB，差的是还没施加的方案。
            //    断言（块长 480：一块一片，探针就是那一次 endBlock 之后的状态）：上一块结束时某一项离目标还远，这一块 appliedBiasDb
            //    就与上一块逐位相同。「还远」取得比到位的容差略宽，只挑肯定没到位的块：Limit Gain 差 > 0.31 dB（死区的出界门槛 0.3，
            //    差这么多一定不在死区里），单位量差 > 1.02，Tone 差 > 0.11 dB。到位以后 G_trim 动了；全程不冲高。
            //    修之后：约 50 s 到位、53.4 s 起积，G_trim 从 0 直接往下走（90 s 时 −0.52 dB）。没有这一条时先冲到 +1.15 再回来。
            //    修之前：从第一块就积，到 +2.25。
            void testWaitsForThePlanToBeInPlace()
            {
                beginTest ("G_trim holds while the automatic increments are still slewing to the plan, then goes straight to its steady state");

                constexpr double runSeconds = 90.0;
                constexpr float gainFarDb = 0.31f;
                constexpr float unitFar = 1.02f;
                constexpr float toneFarDb = 0.11f;
                constexpr float overshootDb = 0.2f;

                Rig rig (fastConstants(), makeMusic (defaultRate, 8.0, 33, 0.5f), defaultParams (100.0f, 0), defaultRate, t097TrimBlock);
                const auto farFromPlan = [&] (const Probe& p)
                {
                    const auto& applied = p.increments;
                    const auto& target = p.targetPlan.increments;
                    const auto far = [] (float a, float b, float limit) { return std::abs (b - a) > limit; };
                    return far (applied.limitGainDb, target.limitGainDb, gainFarDb) || far (applied.colorToneDb, target.colorToneDb, toneFarDb)
                        || far (applied.balanceRecover, target.balanceRecover, unitFar) || far (applied.balanceTame, target.balanceTame, unitFar)
                        || far (applied.colorGlue, target.colorGlue, unitFar) || far (applied.colorHeat, target.colorHeat, unitFar)
                        || far (applied.limitClip, target.limitClip, unitFar);
                };

                auto previous = rig.probe();
                auto singleSlice = true, heldWhileFar = true;
                auto farBlocks = 0, movedBlocks = 0, firstFailure = -1, firstMove = -1;
                auto highest = 0.0f;

                rig.run (runSeconds, [&] (int b)
                {
                    const auto p = rig.probe();
                    singleSlice = singleSlice && rig.chain.getLastProcessSliceCountForTesting() == 1;
                    const auto moved = ! sameBits (p.appliedBiasDb, previous.appliedBiasDb);

                    if (farFromPlan (previous))
                    {
                        ++farBlocks;

                        if (moved && firstFailure < 0)
                            firstFailure = b;

                        heldWhileFar = heldWhileFar && ! moved;
                    }

                    if (moved)
                    {
                        ++movedBlocks;

                        if (firstMove < 0)
                            firstMove = b;
                    }

                    highest = juce::jmax (highest, p.appliedBiasDb);
                    previous = p;
                });

                logMessage ("  blocks far from the plan " + juce::String (farBlocks) + " of " + juce::String (rig.blocksFor (runSeconds))
                            + "; G_trim first moves at " + juce::String (firstMove < 0 ? -1.0 : (double) firstMove * t097TrimBlock / defaultRate, 2)
                            + " s, highest " + db (highest) + " dB, ends at " + db (previous.appliedBiasDb) + " dB; first failing block "
                            + juce::String (firstFailure));
                expect (singleSlice, "fixture: with 480-sample blocks every host block is exactly one slice (one endBlock per probe)");
                expect (farBlocks > rig.blocksFor (10.0), "fixture: the cold start keeps some increment away from the plan for more than 10 s");
                expect (heldWhileFar, "a block that starts with an increment still away from the plan leaves appliedBiasDb bit-identical");
                expect (movedBlocks > 0, "once the plan is in place G_trim integrates");
                expect (highest <= overshootDb, "G_trim does not shoot up during the cold start (without the rule: +1.15 dB; before T-106: +2.25)");
            }

            // 6. 依据：T-106 存读档。OUTPUT_LEVEL 另写十个属性（OutputLevelTracker.cpp createStateXml 末尾那一组：每段折回 0 dB 的出口
            //    能量、入口能量、输入侧读数，全部已收的段），旧属性照旧格式写着。读到没有这十个属性的旧存档（0.1.8.10 及更早）：出口电平历史不知道是在
            //    什么增益下录的、折算不了，清空；G_trim 是旧口径积出来的，从 +0 重新收敛；其余照旧、不冷启动（GlazeController.cpp
            //    restoreBody，ADR-035「GLAZE > 0 的旧工程允许变声」）。
            //    供体：恒定素材放 100 s（G_trim 已收敛到约 +0.10 dB，出口历史里有三十多秒主段）。
            //    断言：
            //      新形状读回——G_trim 与它读的三样（出口 / 入口主段电平、证据时长）同供体逐位相同；马上再存，OUTPUT_LEVEL 与 CONTROL
            //      两个节点逐位相同。
            //      旧形状读回——增量同供体逐位相同、G_trim 是 +0（不是存档里的值）、出口历史为空、LIVE 画像的 I 仍有效、没有旧工程提示；
            //      接着放 60 s，G_trim 又积起来，回到供体那个值的 ±0.05 dB 之内（历史从头攒、照样收敛）。
            //      最后把存档读回一个正在别的增量上跑的 Rig：「这一块执行的是什么」的快照不留着（用例末尾那一段）。
            void testLegacyOutputLevelShape()
            {
                beginTest ("OUTPUT_LEVEL in the 0.1.8.10 shape: G_trim restarts from +0 with an empty history and nothing else cold-starts; "
                           "the new shape round-trips bit-exactly");

                constexpr double donorSeconds = 100.0;
                constexpr double afterSeconds = 60.0;
                constexpr double minimumEvidenceSeconds = 3.0;   // GlazeController.cpp trimEvidenceMinimumSeconds
                constexpr float reconvergedWithinDb = 0.05f;

                auto donor = makeQuietRig();
                donor.run (donorSeconds);
                const auto saved = donor.chain.createGlazeStateXml();
                const auto donorProbe = donor.probe();
                const auto donorI = donor.chain.getLiveProfile().getProfile().integratedLufs;
                const auto* savedLive = saved != nullptr ? saved->getChildByName (glaze::liveTag) : nullptr;
                auto body = saved != nullptr ? t097LiveBody (*saved) : std::unique_ptr<juce::XmlElement> {};
                auto* levelNode = body != nullptr ? body->getChildByName ("OUTPUT_LEVEL") : nullptr;

                logMessage ("  donor: G_trim " + db (donorProbe.appliedBiasDb) + " dB, evidence " + juce::String (donorProbe.trimEvidenceSeconds, 2)
                            + " s (main history " + juce::String (donorProbe.trimMainSeconds, 2) + " s), output " + juce::String (donorProbe.trimLevelLufs, 3)
                            + " / input " + juce::String (donorProbe.trimInputLufs, 3) + " LUFS");
                expect (savedLive != nullptr && levelNode != nullptr && donorI.valid, "fixture: the donor has a LIVE profile and an OUTPUT_LEVEL node");
                expect (donorProbe.appliedBiasDb != 0.0f && donorProbe.trimEvidenceSeconds >= minimumEvidenceSeconds,
                        "fixture: the donor's G_trim has integrated and has evidence to integrate on");

                if (savedLive == nullptr || levelNode == nullptr)
                    return;

                // ---- 新形状 ----
                {
                    auto fresh = makeQuietRig();
                    fresh.chain.restoreGlazeStateXml (saved.get());
                    const auto p = fresh.probe();
                    expect (sameBits (p.appliedBiasDb, donorProbe.appliedBiasDb), "new shape: G_trim is restored bit-identically");
                    expect (sameBits (p.trimLevelLufs, donorProbe.trimLevelLufs) && sameBits (p.trimInputLufs, donorProbe.trimInputLufs)
                                && p.trimEvidenceSeconds == donorProbe.trimEvidenceSeconds && p.trimMainSeconds == donorProbe.trimMainSeconds,
                            "new shape: the readings G_trim integrates on (output / input main level, evidence) are restored bit-identically");

                    const auto again = fresh.chain.createGlazeStateXml();
                    auto againBody = again != nullptr ? t097LiveBody (*again) : std::unique_ptr<juce::XmlElement> {};

                    for (const char* tag : { "OUTPUT_LEVEL", "CONTROL" })
                        expect (t097SameChild (body.get(), againBody.get(), tag), "new shape: " + juce::String (tag) + " round-trips bit-identically");
                }

                // ---- 旧形状：删掉 T-106 的十个属性 ----
                for (const char* attribute : { "openUnityEnergy", "openInputEnergy", "openInputLufsSum", "openLowShareSum", "openInputSamples",
                                               "openPairedSamples", "sampleRate", "historyCount", "history", "historyExtra" })
                {
                    expect (levelNode->hasAttribute (attribute), "new shape: OUTPUT_LEVEL carries " + juce::String (attribute));
                    levelNode->removeAttribute (attribute);
                }

                auto oldShape = levelNode->getNumAttributes() == 8;

                for (const char* attribute : { "openEnergy", "openMainness", "closedCount", "closedWrite", "haveOpenMainness", "openWasMain", "openSamples", "closed" })
                    oldShape = oldShape && levelNode->hasAttribute (attribute);

                expect (oldShape, "fixture: OUTPUT_LEVEL now has exactly the eight attributes 0.1.8.10 writes");

                auto legacy = std::make_unique<juce::XmlElement> (*saved);
                legacy->removeChildElement (legacy->getChildByName (glaze::liveTag), true);
                legacy->addChildElement (glaze::compressLiveBody (*body, savedLive->getIntAttribute ("constantsVersion")).release());

                auto target = makeQuietRig();
                target.chain.restoreGlazeStateXml (legacy.get());
                const auto restored = target.probe();
                const auto restoredI = target.chain.getLiveProfile().getProfile().integratedLufs;
                expect (sameIncrements (restored.increments, donorProbe.increments), "old shape: the applied increments are restored (no cold start)");
                expect (sameBits (restored.appliedBiasDb, 0.0f), "old shape: G_trim restarts from +0, not the saved " + db (donorProbe.appliedBiasDb) + " dB");
                expect (restored.trimMainSeconds == 0.0 && restored.trimEvidenceSeconds == 0.0 && ! restored.trimMainNow,
                        "old shape: the output level history starts empty");
                expect (restoredI.valid, "old shape: the LIVE profile is kept (I still valid: no cold start)");
                expect (! target.status().legacySessionDiscarded, "old shape: no legacy-session notice");

                auto movedAfter = false;
                auto previousTrim = restored.appliedBiasDb;
                target.run (afterSeconds, [&] (int)
                {
                    const auto trim = target.probe().appliedBiasDb;
                    movedAfter = movedAfter || ! sameBits (trim, previousTrim);
                    previousTrim = trim;
                });

                const auto after = target.probe();
                logMessage ("  old shape: G_trim +0 at the restore, " + db (after.appliedBiasDb) + " dB " + juce::String (afterSeconds, 0)
                            + " s later (donor " + db (donorProbe.appliedBiasDb) + " dB), evidence " + juce::String (after.trimEvidenceSeconds, 2) + " s");
                expect (movedAfter, "old shape: G_trim integrates again once new history has built up");
                expect (std::abs (after.appliedBiasDb - donorProbe.appliedBiasDb) <= reconvergedWithinDb,
                        "old shape: " + juce::String (afterSeconds, 0) + " s later G_trim is back within " + db (reconvergedWithinDb)
                        + " dB of the donor's value");

                // ---- 读档连「这一块执行的是什么」的快照一起作废（GlazeController.cpp clearExecutedPlan）----
                // 供体把 GLAZE 改成 50 再放 20 s（Limit Gain 增量比存档时低 3 dB 以上，方案到位），然后把一开始存的那份读回自己。
                // 断言：到没到位、方案净增益要等下一次 beginBlock 重新定（探针：trimPlanInPlace 为假、trimPlanGainDb 是 NaN、没有
                // 目标方案）；读数按存档里的增量折算、同存的时候逐位相同——读档前最后一块执行的增量不留着（留着的话参考增益差
                // 3 dB 以上，存档里的段全不当证据）。
                constexpr double otherGlazeSeconds = 20.0;
                constexpr float otherGlazePercent = 50.0f;
                constexpr float minimumIncrementGapDb = 3.0f;

                donor.params.glazePercent = otherGlazePercent;
                donor.run (otherGlazeSeconds);
                const auto elsewhere = donor.probe();
                donor.chain.restoreGlazeStateXml (saved.get());
                const auto reread = donor.probe();
                expect (elsewhere.trimPlanInPlace
                            && std::abs (elsewhere.increments.limitGainDb - donorProbe.increments.limitGainDb) >= minimumIncrementGapDb,
                        "fixture: before this restore the donor runs in place with a Limit Gain increment at least "
                        + db (minimumIncrementGapDb) + " dB away from the saved one");
                expect (! reread.trimPlanInPlace && std::isnan (reread.trimPlanGainDb) && ! reread.haveTargetPlan,
                        "a restore drops the executed-plan snapshot together with the target plan");
                expect (sameBits (reread.trimLevelLufs, donorProbe.trimLevelLufs) && sameBits (reread.trimInputLufs, donorProbe.trimInputLufs)
                            && reread.trimEvidenceSeconds == donorProbe.trimEvidenceSeconds,
                        "... and the readings are referred to the restored increment, not to the one the last block before the restore executed");
            }

            // 7. 依据：T-106「加权后的证据不足 3 s（一个短期响度窗）不积分」（GlazeController.cpp trimEvidenceMinimumSeconds）。
            //    恒定素材、块长 480（一块一片，一块 0.01 s）。DGX 2026-11-12 实测：方案 63.01 s 到位，那一块起出口历史才开始攒，
            //    66.01 s 那一块攒满 3.00 s，G_trim 就在那一块第一次动。
            //    断言：探针读到的证据还不到 3 s 的块（留半块余量），appliedBiasDb 与上一块逐位相同；G_trim 第一次动的那一块证据
            //    满 3 s、上一块还不满。少了这个门槛，63.01 s 起就积；门槛设大了，满 3 s 的那一块还不动。
            void testIntegratesOnlyOnThreeSecondsOfEvidence()
            {
                beginTest ("G_trim does not integrate on less than 3 s of evidence, and starts on the block that completes the 3 s");

                constexpr double runSeconds = 90.0;
                constexpr double minimumEvidenceSeconds = 3.0;   // GlazeController.cpp trimEvidenceMinimumSeconds
                constexpr double blockSeconds = (double) t097TrimBlock / defaultRate;

                auto rig = makeQuietRig (100.0f, t097TrimBlock);
                auto previous = rig.probe();
                auto singleSlice = true, heldWhileShort = true;
                auto shortBlocks = 0, firstMove = -1;
                auto evidenceAtFirstMove = 0.0, evidenceBeforeFirstMove = 0.0;

                rig.run (runSeconds, [&] (int b)
                {
                    const auto p = rig.probe();
                    singleSlice = singleSlice && rig.chain.getLastProcessSliceCountForTesting() == 1;
                    const auto moved = ! sameBits (p.appliedBiasDb, previous.appliedBiasDb);

                    if (p.trimEvidenceSeconds > 0.0 && p.trimEvidenceSeconds < minimumEvidenceSeconds - 0.5 * blockSeconds)
                    {
                        ++shortBlocks;
                        heldWhileShort = heldWhileShort && ! moved;
                    }

                    if (moved && firstMove < 0)
                    {
                        firstMove = b;
                        evidenceAtFirstMove = p.trimEvidenceSeconds;
                        evidenceBeforeFirstMove = previous.trimEvidenceSeconds;
                    }

                    previous = p;
                });

                logMessage ("  blocks ending with 0 < evidence < 3 s: " + juce::String (shortBlocks) + "; G_trim first moves at "
                            + juce::String (firstMove < 0 ? -1.0 : (double) (firstMove + 1) * blockSeconds, 2) + " s with "
                            + juce::String (evidenceAtFirstMove, 4) + " s of evidence (" + juce::String (evidenceBeforeFirstMove, 4) + " s one block earlier)");
                expect (singleSlice, "fixture: with 480-sample blocks every host block is exactly one slice (one endBlock per probe)");
                expect (shortBlocks >= 250, "fixture: the evidence builds up over nearly 3 s of blocks before it is enough");
                expect (heldWhileShort, "a block that ends with less than 3 s of evidence leaves appliedBiasDb bit-identical");
                expect (firstMove >= 0, "once there are 3 s of evidence G_trim integrates");
                expect (evidenceBeforeFirstMove < minimumEvidenceSeconds && evidenceAtFirstMove >= minimumEvidenceSeconds
                            && evidenceAtFirstMove < minimumEvidenceSeconds + 1.5 * blockSeconds,
                        "G_trim first moves on the very block that completes 3 s of evidence");
            }

            // 8. 依据：T-106「每段按录它时的 Limit Gain 离参考增益多远计权：差 ≤ 1 dB 全算、≥ 3 dB 不算」（GlazeController.cpp
            //    trimEvidenceFullDb / trimEvidenceZeroDb 交给 OutputLevelTracker::evidenceAt；计权本身见 OutputLevelTrackerTests 第 2 条）。
            //    恒定素材放 100 s：各段都是在同一个增益下录的（只差 G_trim 走过的那零点一 dB），证据 = 主段历史的全部，约 37 s。
            //    GLAZE 100 → 50：Limit Gain 增量从 12.6 dB 降到 6.4 dB，旧段离新的参考增益 6 dB 多，不再当证据——它们仍是主段，
            //    主段历史不减；新增益下录的段从头攒。DGX 2026-11-12 实测：改之后 15 s，证据 8.85 s、主段历史 45.86 s，差 37.01 s。
            //    断言：改之前证据与主段历史相等；改之后 15 s，主段历史比证据多出的正是改之前那些（±0.5 s），证据在 3 ～ 15 s 之间。
            //    不降权的话改之后证据仍等于主段历史；降得太狠（差零点几 dB 就不算）的话改之前证据就比主段历史少。
            void testOldSegmentsStopCountingAfterAGainChange()
            {
                beginTest ("segments recorded more than 3 dB away from the current Limit Gain stop counting as evidence (they stay main sections)");

                constexpr double beforeSeconds = 100.0;
                constexpr double afterSeconds = 15.0;

                auto rig = makeQuietRig();
                rig.run (beforeSeconds);
                const auto before = rig.probe();
                rig.params.glazePercent = 50.0f;
                rig.run (afterSeconds);
                const auto after = rig.probe();
                const auto oldSeconds = after.trimMainSeconds - after.trimEvidenceSeconds;

                logMessage ("  before the change: evidence " + juce::String (before.trimEvidenceSeconds, 2) + " s of " + juce::String (before.trimMainSeconds, 2)
                            + " s main history, Limit Gain increment " + juce::String (before.increments.limitGainDb, 2) + " dB; "
                            + juce::String (afterSeconds, 0) + " s after: evidence " + juce::String (after.trimEvidenceSeconds, 2) + " s of "
                            + juce::String (after.trimMainSeconds, 2) + " s, increment " + juce::String (after.increments.limitGainDb, 2) + " dB");
                expect (before.trimMainSeconds >= 30.0, "fixture: half a minute of main history before the change");
                expect (before.increments.limitGainDb - after.increments.limitGainDb > 3.5f,
                        "fixture: halving GLAZE moves the Limit Gain increment by more than the 3 dB beyond which a segment no longer counts");
                expect (std::abs (before.trimEvidenceSeconds - before.trimMainSeconds) <= 0.02,
                        "before the change every main segment counts in full (all were recorded within a fraction of a dB of the current gain)");
                expect (std::abs (oldSeconds - before.trimMainSeconds) <= 0.5,
                        "after the change the segments recorded at the old gain are still main sections but no longer evidence");
                expect (after.trimEvidenceSeconds >= 3.0 && after.trimEvidenceSeconds <= afterSeconds,
                        "... and the evidence is what has been recorded at the new gain");
            }

            // 9. 依据：T-106 抗饱和「积分状态不往用不上的一侧多走」（GlazeController.cpp usableTrimRange：负的最多撤掉自动加上去的部分，
            //    正的不把 Limit Gain 顶过 24 dB）。手动 Limit Gain 拧到 24 dB（上限）、素材很轻（峰值 0.05，+24 dB 之后仍到不了
            //    Ceiling，限幅器不动作）：自动增量和 G_trim 都加不上去，G_trim 能用的范围是 [0, 0]。
            //    DGX 2026-11-12 实测：方案净增益 24.00 dB；5.5 s 起是主段、8.5 s 起证据满 3 s，积分的条件此后一直满足（100 s 里
            //    91.5 s）。实测净增益头二十来秒比方案多（误差到 −0.05 dB），之后比方案少（到 +0.07 dB）：往下、往上都被要求过。
            //    没有抗饱和的话 G_trim 会跟着误差以最多 0.05 dB/s 积下去，上限是 ±3 dB——一点也用不上，手动值往回拧时它会一下子
            //    加出来。
            //    断言：全程 appliedBiasDb 逐位 +0。前提：条件都满足的块超过 60 s；这些块上误差到过 −0.02 dB 以下、也到过
            //    +0.02 dB 以上；执行的 Limit Gain 一直是 24 dB。
            void testDoesNotWindUpWhereItCannotAct()
            {
                beginTest ("anti-windup: with the manual Limit Gain already at 24 dB G_trim cannot act, and its state stays at +0");

                constexpr double runSeconds = 100.0;
                constexpr double minimumEvidenceSeconds = 3.0;   // GlazeController.cpp trimEvidenceMinimumSeconds
                constexpr float manualGainDb = 24.0f;            // Limit Gain 的上限（ParameterRanges；applyLimitExtras 的钳位）

                auto params = defaultParams (100.0f, 0);
                params.limit.gainDb = manualGainDb;
                Rig rig (t097WithoutBudgetCaps (fastConstants()), makeMusic (defaultRate, 8.0, 33, 0.05f), params, defaultRate, t097TrimBlock);
                auto stayedZero = true, gainAtCeiling = true;
                auto openBlocks = 0;
                auto lowestError = 0.0f, highestError = 0.0f;

                rig.run (runSeconds, [&] (int)
                {
                    const auto p = rig.probe();
                    stayedZero = stayedZero && sameBits (p.appliedBiasDb, 0.0f);
                    gainAtCeiling = gainAtCeiling && rig.chain.getEffectiveParams().limit.gainDb == manualGainDb;

                    // 积分的条件全列上（GlazeController.cpp endBlock）：没冻结、块首到位且有那份方案的净增益、开着的是主段、
                    // 证据满 3 s 且两侧都有读数、电平类自动没撤回。误差按积分器用的那一份方案净增益算。
                    if (! p.trimFrozen && p.trimMainNow && p.trimPlanInPlace && std::isfinite (p.trimPlanGainDb)
                        && p.trimEvidenceSeconds >= minimumEvidenceSeconds && p.trimLevelLufs > -100.0f && p.trimInputLufs > -100.0f
                        && ! p.targetPlan.automaticLevelWithdrawn)
                    {
                        ++openBlocks;
                        const auto error = p.trimPlanGainDb - (p.trimLevelLufs - p.trimInputLufs);
                        lowestError = juce::jmin (lowestError, error);
                        highestError = juce::jmax (highestError, error);
                    }
                });

                const auto p = rig.probe();
                logMessage ("  blocks on which G_trim would integrate: " + juce::String (openBlocks) + " of " + juce::String (rig.blocksFor (runSeconds))
                            + "; planned net gain " + juce::String (p.targetPlan.effectiveGainDb, 3) + " dB; error on those blocks "
                            + juce::String (lowestError, 3) + " .. " + juce::String (highestError, 3) + " dB; G_trim " + db (p.appliedBiasDb) + " dB");
                expect (gainAtCeiling, "fixture: the executed Limit Gain is 24 dB on every block (nothing can be added on top)");
                expect (openBlocks > rig.blocksFor (60.0), "fixture: for more than 60 s every other condition for integrating holds "
                                                           "(main section, plan in place, 3 s of evidence, nothing frozen)");
                expect (lowestError < -0.02f && highestError > 0.02f, "fixture: the error asks G_trim to go down at some point and up at another");
                expect (stayedZero, "G_trim stays at +0 bit-exactly: it does not integrate where it cannot act");
            }

            // 10. 依据：T-106 回看重判「出口历史里的段每块按实时画像此刻的参考重判，只有此刻仍算主段的段才进读数」（GlazeController.cpp
            //     endBlock：outputLevel.rejudge 配 LiveProfile::mainnessFor；重判本身见 OutputLevelTrackerTests 第 4 条）。规格第 4 节
            //     第 3 条「输出侧用同一组段边界、同一个 m」；输入侧的实时画像 2026-10-02 起就是回看重判。
            //     恒定素材放 100 s（主段历史约 37 s），然后 ×3（响 9.5 dB）放 60 s：参考电平跟上之后，前 100 s 录的段比参考轻
            //     9.5 dB、不再算主段，主段历史里只剩变响之后录的。DGX 2026-11-12 实测：变响后 10 ～ 12 s 旧段出局（主段历史
            //     40.0 → 0 s），之后从头攒，60 s 时约 43.5 s。不重判的话旧段一直留着，60 s 时是 40 + 43.5 = 83.5 s。
            //     断言：变响前主段历史 ≥ 30 s（参考没变，段都留着）；变响后 60 s，主段历史 ≥ 10 s（新段攒起来了）且 ≤ 60 s
            //     （只剩变响之后的，不可能比变响后的时间还长）。
            void testHistoryIsRejudgedAgainstTheCurrentReference()
            {
                beginTest ("output history is rejudged against the live reference: once louder material sets the reference, earlier segments stop counting as main");

                constexpr double beforeSeconds = 100.0;
                constexpr double afterSeconds = 60.0;
                constexpr float louder = 3.0f;

                auto rig = makeQuietRig();
                rig.run (beforeSeconds);
                const auto mainBefore = rig.probe().trimMainSeconds;
                rig.sourceGain = louder;
                auto lowestAfter = mainBefore;

                rig.run (afterSeconds, [&] (int)
                {
                    lowestAfter = juce::jmin (lowestAfter, rig.probe().trimMainSeconds);
                });

                const auto mainAfter = rig.probe().trimMainSeconds;
                logMessage ("  main history " + juce::String (mainBefore, 2) + " s before the material gets louder; lowest afterwards "
                            + juce::String (lowestAfter, 2) + " s; " + juce::String (mainAfter, 2) + " s " + juce::String (afterSeconds, 0) + " s later");
                expect (mainBefore >= 30.0, "while the reference stands, the recorded main segments stay in the history");
                expect (mainAfter >= 10.0, "fixture: the louder material builds a main history of its own");
                expect (mainAfter <= afterSeconds, "the segments recorded before the material got louder no longer count as main "
                                                   "(the history cannot be longer than the time since)");
            }

            // 11. 依据：T-106「方案到位才当证据、才积分」逐项（GlazeController.cpp planInPlace：已施加的 Recover / Tame / Glue / Heat / Clip
            //     离目标 ≤ 1 个单位（trimPlanUnitTolerance），Tone ≤ 0.1 dB（trimPlanToneToleranceDb））。冷启动时这几项和 Limit Gain 一起爬、
            //     Limit Gain 最慢，第 5 条盯不住单独一项（去掉其中任一项第 5 条照样过；外部第二意见指出改错 C18 要六项一起去掉才红）。
            //     这里每次只把一项拎出来：恒定素材放 100 s（方案到位、G_trim 在积），存档；把 CONTROL 里已施加的这一项改离目标、别的一个字
            //     不动，读回新的链接着放 10 s。对照：同一份存档原样读回。
            //       单位项：目标 ≥ 10.5 时改低 10（往上 2 /s，约 4.5 s 才进容差），否则改高 40（往下 10 /s，约 3.9 s）；
            //       Tone 改低 1 dB（往上 0.2 dB/s，约 4.5 s）。
            //     断言（块长 480：一块一片，探针就是那一次 endBlock 之后的状态）：
            //       对照——读回后 1 s 内 G_trim 就动了（读档本身不拦它：证据、主段、冻结都放行），也没有哪一项离得远。
            //       每一项——上一块结束时这一项离目标超出容差的 1.1 倍（只挑肯定没到位的块），这一块 appliedBiasDb 逐位不变；这样的块
            //       不少于 3 s；这些块上别的几项与 Limit Gain 都在到位的容差里（只差这一项）；这一项到位后 G_trim 又动了。
            //     DGX 2026-11-13 实测：供体 G_trim +0.094 dB；对照读回后第 2 块（0.02 s）就积。Recover 17.04（改成 7.04）离目标 4.49 s、4.56 s
            //     起积；Tame 24 → 14 与 Heat 12 → 2 各 4.44 s、4.51 s；Glue、Clip 目标 0（改成 40）各 3.89 s、3.92 s；Tone 0 → −1 dB 4.45 s、
            //     4.52 s（只测 Tone 的那一版 2026-11-12 是同样的数）。
            void testWaitsForEveryIncrement()
            {
                beginTest ("G_trim waits for every increment: with only one of Recover / Tame / Glue / Heat / Clip / Tone away from the plan "
                           "it holds bit-exactly, then integrates");

                constexpr double donorSeconds = 100.0;
                constexpr double afterSeconds = 10.0;
                constexpr double controlMovesWithinSeconds = 1.0;
                constexpr double minimumHeldSeconds = 3.0;
                constexpr double minimumEvidenceSeconds = 3.0;   // GlazeController.cpp trimEvidenceMinimumSeconds
                constexpr float farFactor = 1.1f;                // 「肯定没到位」= 超出容差的 1.1 倍
                constexpr float gainNearDb = 0.3f;               // Limit Gain 死区的出界门槛
                constexpr double blockSeconds = (double) t097TrimBlock / defaultRate;

                using Increments = glaze::AutoIncrements;

                struct Item
                {
                    const char* attribute;        // CONTROL 里的属性名
                    float Increments::* member;
                    float tolerance;              // planInPlace 的容差
                };

                static constexpr Item items[] = { { "balanceRecover", &Increments::balanceRecover, 1.0f },
                                                  { "balanceTame", &Increments::balanceTame, 1.0f },
                                                  { "colorGlue", &Increments::colorGlue, 1.0f },
                                                  { "colorHeat", &Increments::colorHeat, 1.0f },
                                                  { "limitClip", &Increments::limitClip, 1.0f },
                                                  { "colorToneDb", &Increments::colorToneDb, 0.1f } };

                auto donor = makeQuietRig (100.0f, t097TrimBlock);
                donor.run (donorSeconds);
                const auto saved = donor.chain.createGlazeStateXml();
                const auto donorProbe = donor.probe();

                logMessage ("  donor: G_trim " + db (donorProbe.appliedBiasDb) + " dB, evidence " + juce::String (donorProbe.trimEvidenceSeconds, 2) + " s");
                expect (saved != nullptr && planInPlaceBy (donorProbe) && donorProbe.appliedBiasDb != 0.0f
                            && donorProbe.trimEvidenceSeconds >= minimumEvidenceSeconds,
                        "fixture: at the donor the plan is in place and G_trim has integrated on enough evidence");

                if (saved == nullptr)
                    return;

                struct Outcome
                {
                    bool restoredAsSaved = false;   // 读回后这一项（对照：全部增量）就是存档里那个值，G_trim 同供体
                    int firstMove = -1;             // G_trim 第一次动的块（−1 = 没动过）
                    int farBlocks = 0;              // 这一项（对照：任一项）离目标还远的块数
                    bool heldWhileFar = true;       // 这些块上 G_trim 逐位不变
                    bool onlyThisAway = true;       // 这些块上别的几项与 Limit Gain 都在容差里
                    float awayAtEnd = 0.0f, trimAtEnd = 0.0f;
                };

                // item 为空 = 对照（原样读回）；否则 expected = 读回后这一项该是的值。
                const auto follow = [&] (const juce::XmlElement* state, const Item* item, float expected)
                {
                    Outcome out;
                    auto rig = makeQuietRig (100.0f, t097TrimBlock);
                    rig.chain.restoreGlazeStateXml (state);
                    auto previous = rig.probe();
                    out.restoredAsSaved = (item == nullptr ? sameIncrements (previous.increments, donorProbe.increments)
                                                           : sameBits (previous.increments.*(item->member), expected))
                                       && sameBits (previous.appliedBiasDb, donorProbe.appliedBiasDb);

                    rig.run (afterSeconds, [&] (int b)
                    {
                        const auto p = rig.probe();
                        const auto moved = ! sameBits (p.appliedBiasDb, previous.appliedBiasDb);

                        // 第 0 块之前的探针是读档后的（目标方案还没算出来），从第 1 块起「上一块」才带着这一轮的目标方案。
                        if (b >= 1)
                        {
                            const auto& applied = previous.increments;
                            const auto& target = previous.targetPlan.increments;
                            const auto away = [&] (const Item& i) { return std::abs (target.*(i.member) - applied.*(i.member)); };
                            auto far = false;
                            auto othersIn = std::abs (target.limitGainDb - applied.limitGainDb) <= gainNearDb;

                            for (const auto& i : items)
                            {
                                if (item == nullptr || &i == item)
                                    far = far || away (i) > farFactor * i.tolerance;
                                else
                                    othersIn = othersIn && away (i) <= i.tolerance;
                            }

                            if (far)
                            {
                                ++out.farBlocks;
                                out.heldWhileFar = out.heldWhileFar && ! moved;
                                out.onlyThisAway = out.onlyThisAway && othersIn;
                            }
                        }

                        if (moved && out.firstMove < 0)
                            out.firstMove = b;

                        previous = p;
                    });

                    out.awayAtEnd = item != nullptr ? std::abs (previous.targetPlan.increments.*(item->member) - previous.increments.*(item->member)) : 0.0f;
                    out.trimAtEnd = previous.appliedBiasDb;
                    return out;
                };

                const auto seconds = [=] (int block) { return block < 0 ? -1.0 : (double) (block + 1) * blockSeconds; };
                const auto asSaved = follow (saved.get(), nullptr, 0.0f);
                logMessage ("  restored as saved: G_trim first moves " + juce::String (seconds (asSaved.firstMove), 2) + " s after the restore");
                expect (asSaved.restoredAsSaved && asSaved.farBlocks == 0 && asSaved.firstMove >= 0
                            && seconds (asSaved.firstMove) <= controlMovesWithinSeconds,
                        "control: restored as saved, nothing is away from the plan and G_trim integrates again within "
                        + juce::String (controlMovesWithinSeconds, 0) + " s (the restore itself does not hold it)");

                for (const auto& item : items)
                {
                    const auto applied = donorProbe.increments.*(item.member);
                    const auto target = donorProbe.targetPlan.increments.*(item.member);
                    const auto edited = item.tolerance < 1.0f ? applied - 1.0f
                                                              : (target >= 10.5f ? applied - 10.0f : juce::jmin (100.0f, applied + 40.0f));
                    const auto state = editedState (*saved, [&] (juce::XmlElement& body)
                    {
                        auto* control = body.getChildByName ("CONTROL");

                        if (control == nullptr || ! control->hasAttribute (item.attribute))
                            return false;

                        glaze::xmlio::Writer (*control).field (item.attribute, edited);
                        return true;
                    });

                    const auto label = juce::String (item.attribute);
                    expect (state != nullptr, "fixture: CONTROL carries " + label);

                    if (state == nullptr)
                        continue;

                    const auto lagging = follow (state.get(), &item, edited);
                    logMessage ("  " + label + ": applied " + db (applied) + ", planned " + db (target) + ", edited to " + db (edited) + "; away for "
                                + juce::String (lagging.farBlocks * blockSeconds, 2) + " s, G_trim first moves at " + juce::String (seconds (lagging.firstMove), 2)
                                + " s; after " + juce::String (afterSeconds, 0) + " s " + db (lagging.awayAtEnd) + " from the plan, G_trim " + db (lagging.trimAtEnd) + " dB");
                    expect (lagging.restoredAsSaved, label + ": fixture: the rig starts from the saved G_trim with the edited value");
                    expect (lagging.farBlocks * blockSeconds >= minimumHeldSeconds,
                            label + ": fixture: the edit keeps it away from the plan for at least " + juce::String (minimumHeldSeconds, 0) + " s");
                    expect (lagging.onlyThisAway, label + ": fixture: on those blocks every other increment and the Limit Gain are within their tolerance");
                    expect (lagging.heldWhileFar, label + ": a block that starts with only this increment away from the plan leaves appliedBiasDb bit-identical");
                    expect (lagging.awayAtEnd <= item.tolerance && lagging.firstMove >= 0, label + ": once it is in place G_trim integrates");
                }
            }

            // 12. 依据：T-106「到没到位在 beginBlock 定一次，这一个宿主块切出来的各片沿用」（GlazeController.cpp noteExecutedPlan /
            //     executedPlanInPlace）。GLAZE > 0 时链把宿主块按 10 ms 时隙切片、每片调一次 endBlock（CeladonChain::process）：
            //     前面的片已经把已施加增量往方案推了一步，这一块的音频用的却还是块首交给各模块的那份设置。
            //     出厂预算的冷启动（同第 5 条：Glue / Heat / Clip 与 Limit Gain 一起爬），宿主块长 4800（100 ms，约 10 片）。
            //     断言（每个宿主块）：
            //       探针的 trimPlanInPlace（这一块 beginBlock 记下的）= 拿上一块结束时的探针按「到位」的规矩算出来的值
            //       （上一块结束时的状态就是这一块 beginBlock 看到的）；
            //       trimPlanInPlace 为假的块：块末跟踪器开着的不是主段（这一块没有哪一片按主段进账）、主段历史没变长、
            //       appliedBiasDb 逐位不变。
            //     前提：多数宿主块切成了多片；有「块首没到位、块末已到位」的块（跨线的那一块）；之后有到位的块、G_trim 动过。
            //     每片现判的话，跨线那一块里跨线之后的几片会按主段记进出口历史——音频执行的还是没到位的设置。
            void testJudgesInPlaceOncePerHostBlock()
            {
                beginTest ("a host block cut into several slices is judged in place once, on the settings it started with");

                constexpr double runSeconds = 90.0;
                constexpr int hostBlock = 4800;

                Rig rig (fastConstants(), makeMusic (defaultRate, 8.0, 33, 0.5f), defaultParams (100.0f, 0), defaultRate, hostBlock);
                auto previous = rig.probe();
                auto multiSliceBlocks = 0, mismatched = 0, crossingBlocks = 0, inPlaceBlocks = 0;
                auto mainAtEndWhileAway = 0, grewWhileAway = 0, movedWhileAway = 0, movedBlocks = 0;
                auto firstMismatch = -1, firstCrossing = -1, firstFailure = -1;
                auto slicesInCrossing = 0;

                rig.run (runSeconds, [&] (int b)
                {
                    const auto p = rig.probe();
                    const auto slices = rig.chain.getLastProcessSliceCountForTesting();
                    const auto startedInPlace = planInPlaceBy (previous);
                    const auto moved = ! sameBits (p.appliedBiasDb, previous.appliedBiasDb);
                    multiSliceBlocks += slices > 1 ? 1 : 0;
                    inPlaceBlocks += p.trimPlanInPlace ? 1 : 0;
                    movedBlocks += moved ? 1 : 0;

                    if (p.trimPlanInPlace != startedInPlace)
                    {
                        ++mismatched;

                        if (firstMismatch < 0)
                            firstMismatch = b;
                    }

                    if (! startedInPlace && planInPlaceBy (p))
                    {
                        ++crossingBlocks;

                        if (firstCrossing < 0)
                        {
                            firstCrossing = b;
                            slicesInCrossing = slices;
                        }
                    }

                    if (! p.trimPlanInPlace)
                    {
                        const auto failed = p.trimMainNow || p.trimMainSeconds > previous.trimMainSeconds || moved;
                        mainAtEndWhileAway += p.trimMainNow ? 1 : 0;
                        grewWhileAway += p.trimMainSeconds > previous.trimMainSeconds ? 1 : 0;
                        movedWhileAway += moved ? 1 : 0;

                        if (failed && firstFailure < 0)
                            firstFailure = b;
                    }

                    previous = p;
                });

                const auto blocks = rig.blocksFor (runSeconds);
                logMessage ("  " + juce::String (multiSliceBlocks) + " of " + juce::String (blocks) + " host blocks were cut into several slices; "
                            + juce::String (inPlaceBlocks) + " started in place; the plan first comes into place during block "
                            + juce::String (firstCrossing) + " (" + juce::String (slicesInCrossing) + " slices), " + juce::String (crossingBlocks)
                            + " such blocks in all; G_trim moved on " + juce::String (movedBlocks) + " blocks, ends at " + db (previous.appliedBiasDb)
                            + " dB; first mismatch " + juce::String (firstMismatch) + ", first failing block " + juce::String (firstFailure));
                expect (multiSliceBlocks > blocks / 2, "fixture: most 4800-sample host blocks are cut into several slices");
                expect (crossingBlocks > 0 && slicesInCrossing > 1, "fixture: the plan comes into place in the middle of a multi-slice host block");
                expect (inPlaceBlocks > 0 && movedBlocks > 0, "fixture: later host blocks start in place and G_trim integrates");
                expect (mismatched == 0, "the in-place flag of a host block is what its starting state (the previous block's end) says");
                expect (mainAtEndWhileAway == 0, "a host block that did not start in place ends with no main segment open (none of its slices was booked as main)");
                expect (grewWhileAway == 0, "... and adds nothing to the main history");
                expect (movedWhileAway == 0, "... and leaves appliedBiasDb bit-identical");
            }

            // 13. 依据：T-106「误差里的方案净增益用块首记下的那一份」（GlazeController.cpp executedPlanGainDb）：这一块的音频执行的是
            //     它、出口历史里到位的片也是照它到位的；这一块里刚重算出来的方案下一个宿主块才开始执行。
            //     恒定素材放 100 s（G_trim 收敛，误差 ≈ 0），然后不改任何参数、只把常数换成出厂预算（测试接口 setConstantsForTesting；
            //     不算改参数，不触发 1 s 冻结）：下一次按 1 s 网格重算出来的方案净增益变掉 0.5 dB 以上。
            //     断言（块长 480：一块一片）——重算的那一块（探针里目标方案的净增益第一次变的那一块）：
            //       前提：块首到位、没冻结、块首块末开着的都是主段、证据 ≥ 3 s、块首的误差（旧方案净增益 − 实测净增益）≤ 0.02 dB；
            //       探针的 trimPlanGainDb 逐位等于旧方案的净增益；
            //       这一块 G_trim 的变化不到一步上限（0.05 dB/s × 0.01 s = 0.0005 dB）的一半——按旧方案算，误差 ≈ 0，几乎不动。
            //     拿刚重算的新方案去减实测净增益的话，误差是零点几 dB 以上，这一块就按上限走满一步。
            void testErrorUsesThePlanTheBlockExecuted()
            {
                beginTest ("the error is taken against the plan the host block executed, not the one recomputed during it");

                constexpr double settleSeconds = 100.0;
                constexpr double watchSeconds = 3.0;
                constexpr double minimumEvidenceSeconds = 3.0;   // GlazeController.cpp trimEvidenceMinimumSeconds
                constexpr float minimumPlanStepDb = 0.5f;
                constexpr float settledErrorDb = 0.02f;
                constexpr double blockSeconds = (double) t097TrimBlock / defaultRate;
                constexpr float fullStepDb = 0.05f * (float) blockSeconds;   // trimDbPerSecond × 一块

                auto rig = makeQuietRig (100.0f, t097TrimBlock);
                rig.run (settleSeconds);
                rig.chain.getGlazeController().setConstantsForTesting (fastConstants());

                auto previous = rig.probe();
                Probe before = previous, after = previous;
                auto replanBlock = -1;
                auto singleSlice = true;

                rig.run (watchSeconds, [&] (int b)
                {
                    const auto p = rig.probe();
                    singleSlice = singleSlice && rig.chain.getLastProcessSliceCountForTesting() == 1;

                    if (replanBlock < 0 && ! sameBits (p.targetPlan.effectiveGainDb, previous.targetPlan.effectiveGainDb))
                    {
                        replanBlock = b;
                        before = previous;
                        after = p;
                    }

                    previous = p;
                });

                const auto errorBefore = before.targetPlan.effectiveGainDb - (before.trimLevelLufs - before.trimInputLufs);
                const auto step = after.appliedBiasDb - before.appliedBiasDb;
                logMessage ("  replan in block " + juce::String (replanBlock) + ": planned net gain " + juce::String (before.targetPlan.effectiveGainDb, 4)
                            + " -> " + juce::String (after.targetPlan.effectiveGainDb, 4) + " dB; error against the old plan "
                            + juce::String (errorBefore, 5) + " dB; G_trim " + juce::String (before.appliedBiasDb, 6) + " -> "
                            + juce::String (after.appliedBiasDb, 6) + " dB (step " + juce::String (step, 7) + ", a full step is "
                            + juce::String (fullStepDb, 7) + "); plan gain used " + juce::String (after.trimPlanGainDb, 4) + " dB");
                expect (singleSlice, "fixture: with 480-sample blocks every host block is exactly one slice");
                expect (replanBlock >= 0, "fixture: the planned net gain changes within " + juce::String (watchSeconds, 0) + " s of swapping the constants");

                if (replanBlock < 0)
                    return;

                expect (std::abs (after.targetPlan.effectiveGainDb - before.targetPlan.effectiveGainDb) >= minimumPlanStepDb,
                        "fixture: the recomputed plan's net gain differs by at least " + db (minimumPlanStepDb) + " dB");
                expect (planInPlaceBy (before) && after.trimPlanInPlace, "fixture: the block of the replan started in place");
                expect (! after.trimFrozen && before.trimMainNow && after.trimMainNow && before.trimEvidenceSeconds >= minimumEvidenceSeconds,
                        "fixture: nothing is frozen, a main segment is open and there is enough evidence (G_trim integrates on this block)");
                expect (std::abs (errorBefore) <= settledErrorDb, "fixture: against the old plan the loop is settled (error within "
                                                                  + db (settledErrorDb) + " dB)");
                expect (sameBits (after.trimPlanGainDb, before.targetPlan.effectiveGainDb),
                        "on the block of the replan the error is taken against the old plan's net gain (the plan the block executed)");
                expect (std::abs (step) < 0.5f * fullStepDb,
                        "... so G_trim barely moves on that block (against the recomputed plan it would take a full step)");
            }

            // 14. 依据：T-106「读数折算到的 Limit Gain 用这一个宿主块执行的那份自动增量」（GlazeController.cpp trimReferenceGainDb /
            //     executedIncrementGainDb）。这一块的音频是照块首的增量录的；块内重算把增量的目标挪出死区之后，已施加的增量在
            //     剩下的片里按速率往新目标走——那是下一个宿主块才执行的。
            //     同第 13 条的换常数（恒定素材收敛后把常数换成出厂预算：方案净增益 12.7 → 18.1 dB，不算改参数），但宿主块长
            //     36000（0.75 s，约 75 片），换常数的时刻取在 1 s 网格点之前 0.25 s：重算落在下一块的第 12000 个样本，之后这一块
            //     还有 0.5 s 的片，已施加的 Limit Gain 增量在这 0.5 s 里按 0.2 dB/s 往上走约 0.1 dB。
            //     对照：同样的 Rig 不换常数，取同一块。
            //     断言——换常数之后的第一块：
            //       前提：两个 Rig 块首的 G_trim 逐位相同且积过；方案净增益在这一块里变了 ≥ 0.5 dB；块首到位、没冻结、块末开着
            //       主段、切成 ≥ 50 片；已施加的 Limit Gain 增量在这一块里走了 ≥ 0.08 dB，对照里逐位没动；
            //       G_trim 在这一块的变化和对照差 ≤ 0.001 dB（按块首的增量折算，两个 Rig 这一块的读数是同一个）。
            //     参考增益跟着此刻的增量走的话，读数被抬高同样多，误差过了 −0.05 dB 之后每片都按上限往下走：这一块比对照多走约
            //     0.017 dB。
            void testReferenceUsesTheIncrementTheBlockExecuted()
            {
                beginTest ("the reading is referred to the Limit Gain increment the host block executed, not the one slewed during it");

                constexpr int hostBlock = 36000;     // 0.75 s
                constexpr int settleBlocks = 133;    // 99.75 s：下一个 1 s 网格点（第 100 s）落在下一块的第 12000 个样本
                constexpr int minimumSlices = 50;
                constexpr float minimumPlanStepDb = 0.5f;
                constexpr float minimumIncrementMoveDb = 0.08f;
                constexpr float sameStepWithinDb = 0.001f;

                auto rig = makeQuietRig (100.0f, hostBlock);
                auto control = makeQuietRig (100.0f, hostBlock);

                for (int b = 0; b < settleBlocks; ++b)
                {
                    rig.step();
                    control.step();
                }

                const auto before = rig.probe();
                const auto controlBefore = control.probe();
                rig.chain.getGlazeController().setConstantsForTesting (fastConstants());
                rig.step();
                control.step();
                const auto after = rig.probe();
                const auto controlAfter = control.probe();
                const auto slices = rig.chain.getLastProcessSliceCountForTesting();

                const auto step = after.appliedBiasDb - before.appliedBiasDb;
                const auto controlStep = controlAfter.appliedBiasDb - controlBefore.appliedBiasDb;
                const auto incrementMove = after.increments.limitGainDb - before.increments.limitGainDb;

                logMessage ("  block of " + juce::String (slices) + " slices: planned net gain " + juce::String (before.targetPlan.effectiveGainDb, 4)
                            + " -> " + juce::String (after.targetPlan.effectiveGainDb, 4) + " dB, applied Limit Gain increment "
                            + juce::String (before.increments.limitGainDb, 4) + " -> " + juce::String (after.increments.limitGainDb, 4)
                            + " dB; G_trim " + juce::String (before.appliedBiasDb, 6) + " -> " + juce::String (after.appliedBiasDb, 6)
                            + " dB (step " + juce::String (step, 6) + "), control step " + juce::String (controlStep, 6));
                expect (sameBits (before.appliedBiasDb, controlBefore.appliedBiasDb) && before.appliedBiasDb != 0.0f,
                        "fixture: both rigs reach the block with the same G_trim, and it has integrated");
                expect (std::abs (after.targetPlan.effectiveGainDb - before.targetPlan.effectiveGainDb) >= minimumPlanStepDb,
                        "fixture: the plan is recomputed during the block and its net gain differs by at least " + db (minimumPlanStepDb) + " dB");
                expect (planInPlaceBy (before) && after.trimPlanInPlace && ! after.trimFrozen && after.trimMainNow && slices >= minimumSlices,
                        "fixture: the block starts in place, nothing is frozen, a main segment is open and it is cut into at least "
                        + juce::String (minimumSlices) + " slices");
                expect (incrementMove >= minimumIncrementMoveDb,
                        "fixture: during the block the applied Limit Gain increment slews at least " + db (minimumIncrementMoveDb)
                        + " dB towards the new plan (the replan leaves most of the block still to run)");
                expect (sameBits (controlAfter.increments.limitGainDb, controlBefore.increments.limitGainDb),
                        "fixture: in the control the applied Limit Gain increment does not move");
                expect (std::abs (step - controlStep) <= sameStepWithinDb,
                        "G_trim moves as in the control: the reading stays referred to the increment the block executed "
                        "(referred to the slewed increment it would take about 0.017 dB more)");
            }

            // 解开存档里压缩的 LIVE、交给 edit 改、再打包成整份存档（同 GLAZE_STATE 的写法）。edit 返回 false 或解不开时返回空。
            template <typename Edit>
            static std::unique_ptr<juce::XmlElement> editedState (const juce::XmlElement& saved, Edit&& edit)
            {
                const auto* savedLive = saved.getChildByName (glaze::liveTag);
                auto body = t097LiveBody (saved);

                if (savedLive == nullptr || body == nullptr || ! edit (*body))
                    return {};

                auto state = std::make_unique<juce::XmlElement> (saved);
                state->removeChildElement (state->getChildByName (glaze::liveTag), true);
                state->addChildElement (glaze::compressLiveBody (*body, savedLive->getIntAttribute ("constantsVersion")).release());
                return state;
            }

            // 存档里出口电平历史的出口能量整体乘 factor：实录能量与折回 0 dB 的能量一起乘，录制增益不变，出口电平高 10·log10 factor dB。
            // 布局见 OutputLevelTracker.h：history 每段 24 字节（energy 在 +0），historyExtra 每段 48 字节（unityEnergy 在 +0），段数
            // 是 historyCount。给 0.1.8.10 的 closed（32 段 × 24 字节）这一版不读，也一起乘，让改过的存档前后一致。
            static bool scaleOutputHistory (juce::XmlElement& body, double factor)
            {
                auto* level = body.getChildByName ("OUTPUT_LEVEL");

                if (level == nullptr)
                    return false;

                const auto scaleBlock = [&] (const char* attribute, size_t stride, size_t segments)
                {
                    juce::MemoryOutputStream decoded;

                    if (! juce::Base64::convertFromBase64 (decoded, level->getStringAttribute (attribute)) || decoded.getDataSize() != segments * stride)
                        return false;

                    juce::MemoryBlock bytes (decoded.getData(), decoded.getDataSize());

                    for (size_t i = 0; i < segments; ++i)
                    {
                        auto* at = static_cast<char*> (bytes.getData()) + i * stride;
                        double value;
                        std::memcpy (&value, at, sizeof (value));
                        value *= factor;
                        std::memcpy (at, &value, sizeof (value));
                    }

                    level->setAttribute (attribute, juce::Base64::toBase64 (bytes.getData(), bytes.getSize()));
                    return true;
                };

                auto openEnergy = 0.0, openUnityEnergy = 0.0;
                auto historyCount = 0;
                glaze::xmlio::Reader reader (*level);
                reader.field ("openEnergy", openEnergy);
                reader.field ("openUnityEnergy", openUnityEnergy);
                reader.field ("historyCount", historyCount);

                if (! reader.ok || historyCount <= 0 || ! scaleBlock ("history", 24, (size_t) historyCount)
                    || ! scaleBlock ("historyExtra", 48, (size_t) historyCount) || ! scaleBlock ("closed", 24, 32))
                    return false;

                glaze::xmlio::Writer writer (*level);
                writer.field ("openEnergy", openEnergy * factor);
                writer.field ("openUnityEnergy", openUnityEnergy * factor);
                return true;
            }

            // 15. 依据：T-106（外部第二意见 F3，Max 2026-11-13 定在 T-106 里修）GlazeController.cpp restoreBody「CONTROL 里的浮点逐项查
            //     有限值，G_trim 另查 ±trimLimitDb」，CLAY 的 pressureScale 查有限值；不合规矩整份拒收、冷启动（同别的节点）。改前 xmlio
            //     按位读，NaN / ∞ 也算读到了：G_trim 停在 NaN（之后不再积分），或越过 ±3 dB 直接加到限幅器上。
            //     供体放 100 s、存档；每次只改一项，读进新的链：
            //       收下（实时画像在、已施加的增量同供体）——原样；gTrim 正好 +3、−3 dB（界上），读回的 G_trim 就是它。
            //       拒收（冷启动：实时画像清空、增量与 G_trim 归 +0）——gTrim 3.0001、−3.5、NaN、+∞、−∞；其余十三个浮点任一项 NaN 或 +∞；
            //       CLAY 的 pressureScale NaN。
            void testRestoreRejectsBadControlValues()
            {
                beginTest ("restore: a non-finite CONTROL value, a G_trim beyond +-3 dB or a non-finite Clay pressure scale is rejected (cold start); "
                           "+-3 dB is accepted");

                auto donor = makeQuietRig();
                donor.run (100.0);
                const auto saved = donor.chain.createGlazeStateXml();
                const auto donorProbe = donor.probe();
                expect (saved != nullptr && donorProbe.appliedBiasDb != 0.0f && donor.chain.getLiveProfile().getProfile().integratedLufs.valid,
                        "fixture: the donor has a LIVE profile and a G_trim that has integrated");

                if (saved == nullptr)
                    return;

                const auto withValue = [&] (const char* node, const char* attribute, float value)
                {
                    return editedState (*saved, [&] (juce::XmlElement& body)
                    {
                        auto* child = body.getChildByName (node);

                        if (child == nullptr || ! child->hasAttribute (attribute))
                            return false;

                        glaze::xmlio::Writer (*child).field (attribute, value);
                        return true;
                    });
                };

                struct Restored
                {
                    bool accepted = false;      // 实时画像读回来了、已施加的增量同供体
                    bool coldStarted = false;   // 实时画像清空、增量与 G_trim 都是 +0
                    float trim = 0.0f;
                };

                const auto restore = [&] (const juce::XmlElement* state)
                {
                    Restored r;
                    auto target = makeQuietRig();
                    target.chain.restoreGlazeStateXml (state);
                    const auto p = target.probe();
                    const auto profileKept = target.chain.getLiveProfile().getProfile().integratedLufs.valid;
                    r.accepted = state != nullptr && profileKept && sameIncrements (p.increments, donorProbe.increments);
                    r.coldStarted = state != nullptr && ! profileKept && sameBits (p.appliedBiasDb, 0.0f) && sameIncrements (p.increments, glaze::AutoIncrements {});
                    r.trim = p.appliedBiasDb;
                    return r;
                };

                const auto asSaved = restore (saved.get());
                expect (asSaved.accepted && sameBits (asSaved.trim, donorProbe.appliedBiasDb), "control: the state as saved is accepted with its G_trim");

                for (const auto trim : { 3.0f, -3.0f })
                {
                    const auto r = restore (withValue ("CONTROL", "gTrim", trim).get());
                    expect (r.accepted && sameBits (r.trim, trim), "gTrim " + juce::String (trim, 1) + " dB (on the limit): accepted as it is");
                }

                const auto nan = std::numeric_limits<float>::quiet_NaN();
                const auto inf = std::numeric_limits<float>::infinity();

                for (const auto trim : { 3.0001f, -3.5f, nan, inf, -inf })
                {
                    const auto r = restore (withValue ("CONTROL", "gTrim", trim).get());
                    expect (r.coldStarted && ! r.accepted, "gTrim " + juce::String (trim, 4) + ": rejected, cold start");
                }

                for (const auto* attribute : { "balanceRecover", "balanceTame", "colorToneDb", "colorGlue", "colorHeat", "limitGainDb", "limitClip",
                                               "warm", "punchy", "clean", "liveToneOffsetDb", "liveHeatReduction", "liveGlueReduction" })
                    for (const auto value : { nan, inf })
                    {
                        const auto r = restore (withValue ("CONTROL", attribute, value).get());
                        expect (r.coldStarted && ! r.accepted, "CONTROL " + juce::String (attribute) + " = " + juce::String (value) + ": rejected, cold start");
                    }

                const auto r = restore (withValue ("CLAY", "pressureScale", nan).get());
                expect (r.coldStarted && ! r.accepted, "CLAY pressureScale NaN: rejected, cold start");
            }

            // 16. 依据：T-106（外部第二意见 F1）GlazeController.cpp endBlock「输入侧开了新的一段，出口这边也收段——两侧用同一组段边界」；
            //     规格第 4 节第 3 条「输出侧用同一组段边界、同一个 m」。改前出口这边只在 m′ 跨 0.5、满 8 s 时切段：连着几段主段并成一段，
            //     段里记的输入读数是几段的平均，回看重判时分不开。
            //     读法：存档 OUTPUT_LEVEL 的 openSamples = 出口这边正开着的段已记的样本数（主段非主段都记），切段就从 0 记起。
            //     (a) 恒定素材、宿主不报速度（实时段落账 2 s 一段），放 10 s 后每 0.1 s 读一次、读 20 s：开着的段从不超过 2 s + 一块，
            //         期间切了不少于 9 次；也到过 1.9 s 以上（不是每块都切）。改前要满 8 s 才切。
            //     (b) 不连续点：从头放 1 s，(i) 宿主 reset，(ii) 往后跳播 5 s，(iii) 循环回绕 0.8 s，再放 0.5 s：开着的段只有这 0.5 s，
            //         不连续点之前那 1 s 没并进来。编排器不在这几处另收段：分析器在那里断开实时段落账，下一块开新段，段起点变了就收。
            //     (c) 读档进正在放的链：读回的开着的段在读档后第一块收掉（见下面那一段的说明）。
            //     DGX 2026-11-13 实测：(a) 最长 2.000 s、切 10 次；(b) 三种都是 1.000 s → 0.500 s；(c) 读回 1.000 s，下一块之后 480 个样本。
            void testOutputSegmentsFollowTheInputLedger()
            {
                beginTest ("output level segments are cut where the input segment ledger starts a new segment, discontinuities included");

                constexpr double settleSeconds = 10.0;
                constexpr double watchSeconds = 20.0;
                constexpr int readEveryBlocks = 10;
                constexpr double ledgerSegmentSeconds = 2.0;   // SegmentLedger：宿主不报速度时 2 s 一段
                constexpr int minimumCuts = 9;
                constexpr double beforeBreakSeconds = 1.0;
                constexpr double afterBreakSeconds = 0.5;

                const auto openSamples = [] (Rig& rig) -> std::int64_t
                {
                    const auto state = rig.chain.createGlazeStateXml();
                    auto body = state != nullptr ? t097LiveBody (*state) : std::unique_ptr<juce::XmlElement> {};
                    const auto* level = body != nullptr ? body->getChildByName ("OUTPUT_LEVEL") : nullptr;
                    return level != nullptr && level->hasAttribute ("openSamples") ? level->getStringAttribute ("openSamples").getLargeIntValue() : -1;
                };

                const auto samplesOf = [] (double seconds) { return (std::int64_t) std::llround (seconds * defaultRate); };

                {
                    auto rig = makeQuietRig (100.0f, t097TrimBlock);
                    rig.run (settleSeconds);
                    auto previous = openSamples (rig);
                    auto longest = previous;
                    auto readable = previous >= 0;
                    auto cuts = 0;

                    for (int b = 0; b < rig.blocksFor (watchSeconds); ++b)
                    {
                        rig.step();

                        if ((b + 1) % readEveryBlocks != 0)
                            continue;

                        const auto now = openSamples (rig);
                        readable = readable && now >= 0;
                        longest = std::max (longest, now);
                        cuts += now < previous ? 1 : 0;
                        previous = now;
                    }

                    logMessage ("  (a) over " + juce::String (watchSeconds, 0) + " s the open output segment reached " + juce::String ((double) longest / defaultRate, 3)
                                + " s at most and was cut " + juce::String (cuts) + " times");
                    expect (readable, "(a) fixture: OUTPUT_LEVEL carries openSamples");
                    expect (longest >= samplesOf (ledgerSegmentSeconds) - readEveryBlocks * t097TrimBlock,
                            "(a) fixture: segments run to about 2 s (they are not cut on every block)");
                    expect (longest <= samplesOf (ledgerSegmentSeconds) + t097TrimBlock,
                            "(a) an output segment never runs longer than the input ledger's 2 s segment (before T-106's F1 fix: 8 s)");
                    expect (cuts >= minimumCuts, "(a) ... and is cut at each of the ledger's boundaries: at least " + juce::String (minimumCuts) + " cuts in 20 s");
                }

                for (const auto kind : { 0, 1, 2 })
                {
                    static constexpr const char* names[] = { "(b-i) host reset", "(b-ii) transport jump", "(b-iii) loop wrap" };
                    const auto label = juce::String (names[kind]);
                    auto rig = makeQuietRig (100.0f, t097TrimBlock);
                    rig.run (beforeBreakSeconds);
                    const auto before = openSamples (rig);
                    const auto counted = [&] { const auto p = rig.probe(); return p.hostResets + p.transportJumps + p.loopWraps; };
                    const auto countedBefore = counted();

                    if (kind == 0)
                    {
                        rig.chain.reset();
                    }
                    else if (kind == 1)
                    {
                        rig.position += samplesOf (5.0);
                    }
                    else
                    {
                        rig.host.hasLoop = true;
                        rig.host.looping = true;
                        rig.position -= samplesOf (0.8);
                    }

                    rig.run (afterBreakSeconds);
                    const auto after = openSamples (rig);
                    logMessage ("  " + label + ": open output segment " + juce::String ((double) before / defaultRate, 3) + " s before, "
                                + juce::String ((double) after / defaultRate, 3) + " s " + juce::String (afterBreakSeconds, 1) + " s after");
                    expect (counted() == countedBefore + 1, label + ": fixture: the controller counts exactly one discontinuity");
                    expect (before >= samplesOf (beforeBreakSeconds) - t097TrimBlock, label + ": fixture: before it the open segment holds about 1 s");
                    expect (after > 0 && after <= samplesOf (afterBreakSeconds) + t097TrimBlock,
                            label + ": afterwards the open segment holds only what came after it");
                }

                // (c) 读档进正在放的链（GlazeController.cpp clearExecutedPlan 把「上一片看到的段起点」作废）：读回的那个开着的段属于存档时
                //     的时间轴，读档后第一块就收掉，不接着往里记。放 21 s（开着的段 [20 s, 22 s) 已记 1 s）存档，再放 0.3 s（还在同一段里），
                //     读回那一份，放一块：开着的段只有这一块。不作废的话段起点没变，读回的 1 s 接着记成 1.01 s。
                {
                    auto rig = makeQuietRig (100.0f, t097TrimBlock);
                    rig.run (21.0);
                    const auto saved = rig.chain.createGlazeStateXml();
                    const auto savedOpen = openSamples (rig);
                    rig.run (0.3);
                    rig.chain.restoreGlazeStateXml (saved.get());
                    const auto restoredOpen = openSamples (rig);
                    rig.step();
                    const auto after = openSamples (rig);
                    logMessage ("  (c) restore into a running chain: saved open segment " + juce::String ((double) savedOpen / defaultRate, 3) + " s, "
                                + juce::String ((double) restoredOpen / defaultRate, 3) + " s right after the restore, "
                                + juce::String (after) + " samples after the next block");
                    expect (savedOpen >= samplesOf (beforeBreakSeconds) - t097TrimBlock && restoredOpen == savedOpen,
                            "(c) fixture: the restore brings back the saved open segment (about 1 s)");
                    expect (after == t097TrimBlock, "(c) the first block after the restore starts a new output segment (the restored one is closed)");
                }
            }

            // 17. 依据：T-106 抗饱和的下界与「已经在外面的不往回拽」（GlazeController.cpp usableTrimRange 下界 = −min(3, 自动加上去的部分)；
            //     endBlock 往下走时新值不低于 min(当前, 下界)）。第 9 条只盯了上界（手动 24 dB，范围 [0, 0]）。
            //     手动 Limit Gain 23 dB、素材峰值 0.05（+24 dB 之后仍到不了 Ceiling，限幅器不动作）：规划器把 Limit Gain 增量算到正好顶 24 dB
            //     （1 dB），已施加的停在死区里——自动部分不到 1 dB（DGX 2026-11-13 实测 0.882 dB），G_trim 往下最多用得上这么多。下界按每块
            //     探针里已施加的增量现算（−(min(24, 23 + 增量) − 23)），不写死。供体放 100 s、存档。
            //     存档里出口电平历史的能量整体 ×2（+3.01 dB，见 scaleOutputHistory），读回新的链：旧段读成「出口比方案响 3 dB」，误差为负，
            //     G_trim 往下积；新录的段是真值，旧段占的比例降下去之前误差一直为负。
            //     (a) G_trim 从存档值出发，放 60 s：每块都不低于下界，也到过下界（± 0.001 dB，下界真起了作用）；停在下界上、误差仍 < −0.05 dB
            //         的块合计 ≥ 10 s（没有下界的话这些块上还会接着往下）。
            //     (b) 同一份历史、CONTROL 的 gTrim 改成 −2.5 dB（在用得上的范围外面）：放 20 s，G_trim 逐位停在 −2.5——误差要它往下，它不往
            //         更外面走，也不被一下子拽回下界。
            //     (c) 对照：gTrim −2.5、历史不改：误差为正，G_trim 往回走（积分开着，(b) 的不动归功于下界），每块不超过 0.05 dB/s × 0.01 s。
            //     DGX 2026-11-13 实测：供体主段历史 65 s；(a) 误差最低 −2.92 dB，G_trim 最低 −0.8820 dB = 下界，停在下界上误差仍 < −0.05 dB 的
            //     有 23.3 s；(b) 20 s 逐位 −2.5；(c) 20 s 回到 −1.50 dB，每块正好 0.0005 dB。
            void testDoesNotWindUpBelowWhatItCanUse()
            {
                beginTest ("anti-windup, lower side: G_trim does not go below minus the automatic part, and one restored beyond it is not yanked back");

                constexpr double donorSeconds = 100.0;
                constexpr float manualGainDb = 23.0f;
                constexpr double louderFactor = 2.0;             // +3.01 dB
                constexpr double runSeconds = 60.0;
                constexpr double heldSeconds = 20.0;
                constexpr float outsideDb = -2.5f;
                constexpr double minimumPushedSeconds = 10.0;
                constexpr float pushingErrorDb = -0.05f;
                constexpr double blockSeconds = (double) t097TrimBlock / defaultRate;
                constexpr float maximumStepDb = 0.05f * (float) blockSeconds;   // GlazeController.cpp trimDbPerSecond × 一块

                auto params = defaultParams (100.0f, 0);
                params.limit.gainDb = manualGainDb;
                const auto makeRig = [&] { return Rig (t097WithoutBudgetCaps (fastConstants()), makeMusic (defaultRate, 8.0, 33, 0.05f), params, defaultRate, t097TrimBlock); };
                // usableTrimRange 的下界：负的「自动加上去的部分」（Limit Gain 先按手动 + 已施加增量钳进 0 ～ 24 dB）。
                const auto lowerBoundOf = [] (const Probe& p) { return -(juce::jlimit (0.0f, 24.0f, manualGainDb + p.increments.limitGainDb) - manualGainDb); };

                auto donor = makeRig();
                donor.run (donorSeconds);
                const auto saved = donor.chain.createGlazeStateXml();
                const auto donorProbe = donor.probe();
                logMessage ("  donor: Limit Gain increment " + db (donorProbe.increments.limitGainDb) + " dB (planned " + db (donorProbe.targetPlan.increments.limitGainDb)
                            + "), planned net gain " + db (donorProbe.targetPlan.effectiveGainDb) + " dB, executed Limit Gain "
                            + db (donor.chain.getEffectiveParams().limit.gainDb) + " dB, G_trim " + db (donorProbe.appliedBiasDb) + " dB, main history "
                            + juce::String (donorProbe.trimMainSeconds, 2) + " s");
                expect (saved != nullptr && lowerBoundOf (donorProbe) < -0.1f && lowerBoundOf (donorProbe) > -2.9f,
                        "fixture: the automatic part is between 0 and 3 dB, so the lower bound is minus it and not the -3 dB clamp: "
                        + db (-lowerBoundOf (donorProbe)) + " dB");
                expect (donorProbe.trimEvidenceSeconds >= 10.0, "fixture: the donor has at least 10 s of evidence to carry over");

                if (saved == nullptr)
                    return;

                const auto louder = editedState (*saved, [&] (juce::XmlElement& body) { return scaleOutputHistory (body, louderFactor); });
                const auto louderOutside = editedState (*saved, [&] (juce::XmlElement& body)
                {
                    auto* control = body.getChildByName ("CONTROL");

                    if (control == nullptr || ! scaleOutputHistory (body, louderFactor))
                        return false;

                    glaze::xmlio::Writer (*control).field ("gTrim", outsideDb);
                    return true;
                });
                const auto outside = editedState (*saved, [&] (juce::XmlElement& body)
                {
                    auto* control = body.getChildByName ("CONTROL");

                    if (control == nullptr)
                        return false;

                    glaze::xmlio::Writer (*control).field ("gTrim", outsideDb);
                    return true;
                });
                expect (louder != nullptr && louderOutside != nullptr && outside != nullptr, "fixture: the three edited states are built");

                if (louder == nullptr || louderOutside == nullptr || outside == nullptr)
                    return;

                const auto errorOf = [] (const Probe& p) { return p.trimPlanGainDb - (p.trimLevelLufs - p.trimInputLufs); };

                // ---- (a) ----
                {
                    auto rig = makeRig();
                    rig.chain.restoreGlazeStateXml (louder.get());
                    auto lowest = rig.probe().appliedBiasDb;
                    auto pushedBlocks = 0;
                    auto mostNegativeError = 0.0f;
                    auto reachedBound = false, belowBound = false;
                    auto bound = 0.0f;

                    rig.run (runSeconds, [&] (int)
                    {
                        const auto p = rig.probe();
                        bound = lowerBoundOf (p);
                        lowest = juce::jmin (lowest, p.appliedBiasDb);
                        reachedBound = reachedBound || p.appliedBiasDb <= bound + 0.001f;
                        belowBound = belowBound || p.appliedBiasDb < bound - 1.0e-6f;
                        const auto integrating = ! p.trimFrozen && p.trimMainNow && p.trimPlanInPlace && std::isfinite (p.trimPlanGainDb)
                                              && p.trimEvidenceSeconds >= 3.0 && p.trimLevelLufs > -100.0f && p.trimInputLufs > -100.0f;

                        if (integrating)
                            mostNegativeError = juce::jmin (mostNegativeError, errorOf (p));

                        if (integrating && p.appliedBiasDb <= bound + 0.001f && errorOf (p) < pushingErrorDb)
                            ++pushedBlocks;
                    });

                    logMessage ("  (a) history 3 dB louder: G_trim lowest " + db (lowest) + " dB (bound at the end " + db (bound) + " dB), error down to "
                                + db (mostNegativeError) + " dB, " + juce::String (pushedBlocks * blockSeconds, 2) + " s at the bound with the error still below "
                                + db (pushingErrorDb) + " dB");
                    expect (reachedBound, "(a) fixture: G_trim reaches the bound (the edited history pushes it down that far)");
                    expect (pushedBlocks * blockSeconds >= minimumPushedSeconds,
                            "(a) fixture: at the bound the error keeps asking for more for at least " + juce::String (minimumPushedSeconds, 0) + " s");
                    expect (! belowBound, "(a) G_trim never goes below minus the automatic part");
                }

                // ---- (b) ----
                {
                    auto rig = makeRig();
                    rig.chain.restoreGlazeStateXml (louderOutside.get());
                    auto held = sameBits (rig.probe().appliedBiasDb, outsideDb);
                    auto pushedBlocks = 0;

                    rig.run (heldSeconds, [&] (int)
                    {
                        const auto p = rig.probe();
                        held = held && sameBits (p.appliedBiasDb, outsideDb);
                        const auto integrating = ! p.trimFrozen && p.trimMainNow && p.trimPlanInPlace && std::isfinite (p.trimPlanGainDb)
                                              && p.trimEvidenceSeconds >= 3.0 && p.trimLevelLufs > -100.0f && p.trimInputLufs > -100.0f;
                        pushedBlocks += integrating && errorOf (p) < pushingErrorDb ? 1 : 0;
                    });

                    logMessage ("  (b) restored at " + db (outsideDb) + " dB with the louder history: " + juce::String (pushedBlocks * blockSeconds, 2)
                                + " s with the error asking for more; G_trim at the end " + db (rig.probe().appliedBiasDb) + " dB");
                    expect (pushedBlocks * blockSeconds >= minimumPushedSeconds, "(b) fixture: the error keeps asking G_trim to go down");
                    expect (held, "(b) G_trim stays at -2.5 dB bit-exactly: not further out, not yanked back to the bound");
                }

                // ---- (c) ----
                {
                    auto rig = makeRig();
                    rig.chain.restoreGlazeStateXml (outside.get());
                    auto previous = rig.probe().appliedBiasDb;
                    auto largestStep = 0.0f;
                    auto lowest = previous;

                    rig.run (heldSeconds, [&] (int)
                    {
                        const auto trim = rig.probe().appliedBiasDb;
                        largestStep = juce::jmax (largestStep, std::abs (trim - previous));
                        lowest = juce::jmin (lowest, trim);
                        previous = trim;
                    });

                    logMessage ("  (c) restored at " + db (outsideDb) + " dB with the history as saved: G_trim " + db (previous) + " dB after "
                                + juce::String (heldSeconds, 0) + " s, largest step " + juce::String (largestStep, 6) + " dB");
                    expect (previous > outsideDb + 0.1f, "(c) control: with the error positive G_trim integrates back from -2.5 dB");
                    expect (lowest >= outsideDb && largestStep <= maximumStepDb + 1.0e-6f,
                            "(c) ... at no more than 0.05 dB/s, never below where it was restored");
                }
            }

            // 18. 依据：G_trim 的范围、读数折算、守门的界都先把「手动 + 已施加增量」钳进 0 ～ 24 dB（GlazeController.cpp gainAfterIncrementDb；
            //     限幅器执行的也是钳过的值）。规划器给的增量本来就让手动 + 增量 ≤ 24 dB，第 17 条碰不到这道钳。用户在 GLAZE 开着时把手动
            //     Limit Gain 往上拧，已施加的增量按速率退下来，退进死区时还比新目标高出不到 0.1 dB（gainDeadbandExitDb）、之后就停在那里：
            //     这时手动 + 增量 > 24 dB，不钳的话「自动部分」多算这一截，G_trim 往下多走这一截。
            //     供体：手动 20 dB，其余同第 17 条，放 100 s、存档。存档里出口电平历史的能量 ×2（+3.01 dB，误差为负），CONTROL 的 gTrim 改成
            //     −1 dB：手动 23 dB 时自动部分最多 24 − 23 = 1 dB，钳过之后下界最低就到 −1 dB。
            //     (a) 读进手动 23 dB 的链（供体留下的增量比新目标高 2 dB 以上），放 60 s：G_trim 一块也不低于 −1 dB（不留容差）。
            //         夹具：积分开着、手动 + 已施加增量 > 24 dB、误差 < −0.05 dB 的块合计 ≥ 5 s（不钳的话这些块上 G_trim 会往 −1 dB 以下走）。
            //     (b) 对照：同一份存档读进手动仍是 20 dB 的链：下界是 −3 dB，G_trim 从 −1 dB 往下走过 −1.1 dB（误差确实要它往下，积分开着）。
            void testRaisedManualGainUsesTheClampedLimitGain()
            {
                beginTest ("manual Limit Gain raised while an increment is applied: G_trim's range uses manual + increment clamped to 24 dB");

                constexpr double donorSeconds = 100.0;
                constexpr float donorManualDb = 20.0f;
                constexpr float raisedManualDb = 23.0f;
                constexpr float restoredTrimDb = -1.0f;   // −(24 − 23)：手动 23 dB 时，钳过之后的下界最低到这里
                constexpr double louderFactor = 2.0;      // +3.01 dB
                constexpr double runSeconds = 60.0;
                constexpr double minimumPushedSeconds = 5.0;
                constexpr float pushingErrorDb = -0.05f;
                constexpr double blockSeconds = (double) t097TrimBlock / defaultRate;

                const auto makeRig = [] (float manualDb)
                {
                    auto params = defaultParams (100.0f, 0);
                    params.limit.gainDb = manualDb;
                    return Rig (t097WithoutBudgetCaps (fastConstants()), makeMusic (defaultRate, 8.0, 33, 0.05f), params, defaultRate, t097TrimBlock);
                };
                const auto errorOf = [] (const Probe& p) { return p.trimPlanGainDb - (p.trimLevelLufs - p.trimInputLufs); };
                const auto integrating = [] (const Probe& p)
                {
                    return ! p.trimFrozen && p.trimMainNow && p.trimPlanInPlace && std::isfinite (p.trimPlanGainDb) && p.trimEvidenceSeconds >= 3.0
                        && p.trimLevelLufs > -100.0f && p.trimInputLufs > -100.0f;
                };

                auto donor = makeRig (donorManualDb);
                donor.run (donorSeconds);
                const auto saved = donor.chain.createGlazeStateXml();
                const auto donorProbe = donor.probe();
                logMessage ("  donor (manual " + db (donorManualDb) + " dB): Limit Gain increment " + db (donorProbe.increments.limitGainDb) + " dB (planned "
                            + db (donorProbe.targetPlan.increments.limitGainDb) + "), executed Limit Gain " + db (donor.chain.getEffectiveParams().limit.gainDb)
                            + " dB, G_trim " + db (donorProbe.appliedBiasDb) + " dB, main history " + juce::String (donorProbe.trimMainSeconds, 2) + " s");
                expect (saved != nullptr && raisedManualDb + donorProbe.increments.limitGainDb > 25.0f,
                        "fixture: the donor's increment is above 2 dB, so manual 23 dB + it starts well above 24 dB: " + db (donorProbe.increments.limitGainDb) + " dB");

                if (saved == nullptr)
                    return;

                const auto edited = editedState (*saved, [&] (juce::XmlElement& body)
                {
                    auto* control = body.getChildByName ("CONTROL");

                    if (control == nullptr || ! scaleOutputHistory (body, louderFactor))
                        return false;

                    glaze::xmlio::Writer (*control).field ("gTrim", restoredTrimDb);
                    return true;
                });
                expect (edited != nullptr, "fixture: the edited state is built");

                if (edited == nullptr)
                    return;

                // ---- (a) ----
                {
                    auto rig = makeRig (raisedManualDb);
                    rig.chain.restoreGlazeStateXml (edited.get());
                    const auto restored = rig.probe();
                    auto lowest = restored.appliedBiasDb;
                    auto pushedAboveBlocks = 0;
                    auto highestWhileIntegrating = 0.0f;
                    auto finalIncrement = restored.increments.limitGainDb;

                    rig.run (runSeconds, [&] (int)
                    {
                        const auto p = rig.probe();
                        const auto manualPlusIncrement = raisedManualDb + p.increments.limitGainDb;
                        lowest = juce::jmin (lowest, p.appliedBiasDb);
                        finalIncrement = p.increments.limitGainDb;

                        if (integrating (p))
                            highestWhileIntegrating = juce::jmax (highestWhileIntegrating, manualPlusIncrement);

                        pushedAboveBlocks += integrating (p) && manualPlusIncrement > 24.0f && errorOf (p) < pushingErrorDb ? 1 : 0;
                    });

                    logMessage ("  (a) restored into manual " + db (raisedManualDb) + " dB: increment " + db (restored.increments.limitGainDb) + " -> "
                                + db (finalIncrement) + " dB, manual + increment up to " + db (highestWhileIntegrating) + " dB while integrating, "
                                + juce::String (pushedAboveBlocks * blockSeconds, 2) + " s integrating above 24 dB with the error below " + db (pushingErrorDb)
                                + " dB; G_trim lowest " + db (lowest) + " dB");
                    expect (sameBits (restored.appliedBiasDb, restoredTrimDb), "(a) fixture: G_trim is restored at -1 dB");
                    expect (pushedAboveBlocks * blockSeconds >= minimumPushedSeconds,
                            "(a) fixture: integrating with manual + increment above 24 dB and the error asking for more for at least "
                            + juce::String (minimumPushedSeconds, 0) + " s");
                    expect (lowest >= restoredTrimDb, "(a) G_trim never goes below -1 dB: the automatic part counts only up to the 24 dB the limiter executes");
                }

                // ---- (b) ----
                {
                    auto rig = makeRig (donorManualDb);
                    rig.chain.restoreGlazeStateXml (edited.get());
                    auto lowest = rig.probe().appliedBiasDb;

                    rig.run (runSeconds, [&] (int) { lowest = juce::jmin (lowest, rig.probe().appliedBiasDb); });

                    logMessage ("  (b) control, restored into manual " + db (donorManualDb) + " dB: G_trim lowest " + db (lowest) + " dB");
                    expect (lowest < restoredTrimDb - 0.1f, "(b) control: with the lower bound at -3 dB the same history pulls G_trim below -1.1 dB");
                }
            }
        };

        static GlazeControllerTrimLoopTests glazeControllerTrimLoopTests;
    }
}

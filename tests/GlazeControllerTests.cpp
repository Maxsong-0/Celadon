/*
  ==============================================================================

    GlazeControllerTests.cpp
    T-048 阶段 2：聆听 / 锁定 / 校准的产品契约在 tests/Phase2OrchestratorTests.cpp 与 GlazeStatusTextTests.cpp
    （七条状态行、速率、棘轮、旧工程丢弃、Q12 死字段）。本文件仍驱动阶段 1 的 GlazeController——
    代理 A 未合入，requestRelearn / Phase / calibrationValid 还在。合入时按 Phase2Contract.h 改这些驱动，
    不要把锁定断言加回去。校准器单测已从 GlazeCalibrationTests 删除。

    v0.6 GLAZE 编排器（A4）的自动测试：状态转移全覆盖与三个就绪标志、首次聆听与重新聆听（候选 / 已提交、取消、中途保存）、
    跳播 / 回绕 / 宿主 reset 各清什么、离线导出（不学习不校准、未学习全程固定保守方案）、保存再读取（立即生效、同起点逐位重渲染）、
    采样率改变、GLAZE 连续控制（不重学不重做过渡不重启校准、回 0 偏差归零）、换风格、旧算法方案、约束反馈闭环收敛不振荡、
    手动优先（实际 Limit Gain 不低于手动值）、非有限输出、音频线程零分配。

    都经 CeladonChain 驱动（两块表与编排器都在链里，和插件里完全一样的接线）；时长常数用 setConstantsForTesting 缩短：
    聆听 2 ～ 3 s、方案过渡 0.5 s、校准 settle 0.5 s + 每轮 2 s（出厂 15 ～ 30 s / 3 s / 3 s + 10 s）。
    每个用例的「依据」写在注释里：设计规格-GLAZE-时间线与保存（下称「时间线」）、决策与校准（「决策」）、PHASE2-NOTES 设计决定 1 ～ 4。

    v0.1.7.0 修坯 Clay 的编排器接线与存档（A6 追加，「Clay 1 ～ 8」）：三态在 GLAZE 0 / > 0 的语义、画像格式 1 / 2 的兼容
    （格式 1 的画像号与 v0.1.6.2 逐位相同，与照抄的旧实现对照）、旧画像与旧算法方案的自动 Clay 中性、方案过渡混合、
    ClayGuard 经编排器生效、执行快照往返、manualParamsHash 并入 Clay 设置（默认时与旧哈希逐位相同）、音频线程零分配。
    Clay 用例用合成素材（makeClayMaterial：低频正弦 + 白噪声，可选低频反相 / 高频左右不相关）与「按规格最需要处理」的
    格式 2 画像（withClayNeeds），数值依据写在各自注释里。依据：设计规格-Clay（下称「Clay」）、设计规格-Clay-分析决策与验收
    （「Clay 子文档」）、ADR-009。
    T-026（BUG-004，ADR-018）：重开工程时宿主对参数做归一化往返，对数刻度的三个旋钮（Limit Release、Clay 低切 / 收单分频点）
    会漂几个 ulp。用真参数范围（ParameterRanges.h）按 JUCE RangedAudioParameter 的换算做往返，参考条件必须仍然一致（任一旋钮挪
    一个显示步进必须不一致）；Rig 存档后按漂移后的值重开（新格式、改造成旧格式两种），校准与执行快照必须照样适用。
    T-012 阶段 4（ADR-012 / 设计规格-Clay-根治修订）：withClayNeeds 按新决策公式给数并补「先试后用」的代价（代价无效的画像三项都
    中性）；追加 Clay 9（代价 mask 经编排器到状态快照：只报自动态、GLAZE > 0、过渡中取目标方案的）与 Clay 10（审核 V2-03：恢复后
    第一块只在方案源支持自动 Clay 时才用快照值）。

  ==============================================================================
*/

#include "TestHelpers.h"

#include "../Source/dsp/CeladonChain.h"
#include "../Source/dsp/ClayGuard.h"
#include "../Source/dsp/GlazePlanner.h"
#include "../Source/dsp/GlazeProfile.h"
#include "../Source/ParameterRanges.h"

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

        bool sameSignal (const Signal& a, const Signal& b)
        {
            if (a.size() != b.size())
                return false;

            for (size_t ch = 0; ch < a.size(); ++ch)
                if (a[ch].size() != b[ch].size() || std::memcmp (a[ch].data(), b[ch].data(), a[ch].size() * sizeof (float)) != 0)
                    return false;

            return true;
        }

        // 决策器在「已提交版本 + 本块 GLAZE / 手动值」下直接算出的方案（编排器在 GLAZE 连续控制时应当逐块等于它）。
        glaze::Plan planFor (const glaze::CommittedState& committed, glaze::Style style, float glaze, const ChainParams& manual,
                             float maxGainDb)
        {
            glaze::PlanInputs in;
            in.profile = &committed.profile;
            in.style = style;
            in.glazePercent = glaze;
            in.manual = manual;
            in.constants = &committed.constants;
            in.maxGainDb = maxGainDb;
            in.lockedWeights = &committed.lockedWeights;
            return glaze::makePlan (in);
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

                // T-038：tempoBpm > 0 时同时报恒定速度、4/4 的拍点（PPQ 与位置严格一致，小节起点按 PPQ ÷ 4）；默认 0 = 不报（原有用例不变）。
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
            Phase phase() const             { return chain.getGlazeController().getPhaseForTesting(); }
            glaze::CommittedState committed() const   { return chain.getGlazeController().getCommittedStateForTesting(); }

            // 走完「首次聆听 → 提交 → 过渡 → 校准 → 锁定」。返回是否在 maxSeconds 之内锁定且校准有效。
            bool learnAndCalibrate (double maxSeconds = 40.0)
            {
                return runUntil (maxSeconds, [this]
                {
                    const auto s = status();
                    return phase() == Phase::locked && s.profileReady && s.calibrationValid;
                });
            }

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

        const char* phaseName (Phase p)
        {
            switch (p)
            {
                case Phase::unlearned:      return "unlearned";
                case Phase::listening:      return "listening";
                case Phase::planTransition: return "planTransition";
                case Phase::calibrating:    return "calibrating";
                case Phase::locked:         return "locked";
                default:                    return "?";
            }
        }
    }

    //==============================================================================
    // ---- v0.1.7.0 修坯 Clay（A6）用的工具 ----
    namespace
    {
        bool sameClayPlan (const glaze::ClayPlan& a, const glaze::ClayPlan& b) noexcept
        {
            return sameBits (a.lowCutHz, b.lowCutHz) && sameBits (a.monoHz, b.monoHz)
                && sameBits (a.monoAmount, b.monoAmount) && sameBits (a.widthDb, b.widthDb);
        }

        bool sameClayParams (const ClayModule::Params& a, const ClayModule::Params& b) noexcept
        {
            return sameBits (a.lowCutHz, b.lowCutHz) && sameBits (a.monoHz, b.monoHz)
                && sameBits (a.monoAmount, b.monoAmount) && sameBits (a.widthDb, b.widthDb) && a.bypass == b.bypass;
        }

        bool sameClayGuard (const glaze::ClayGuardState& a, const glaze::ClayGuardState& b) noexcept
        {
            return sameBits (a.phaseFloor, b.phaseFloor) && sameBits (a.phaseHoldSeconds, b.phaseHoldSeconds)
                && sameBits (a.widthCapDb, b.widthCapDb) && a.phaseActive == b.phaseActive && a.widthActive == b.widthActive;
        }

        // 中性 = 三级都不处理（低切关、收单 0、加宽 0）：ClayModule 逐位直通（收单为 0 时分频点无意义，不看）。
        bool isNeutralClay (const ClayModule::Params& p) noexcept
        {
            return p.lowCutHz == 0.0f && p.monoAmount == 0.0f && p.widthDb == 0.0f;
        }

        bool isNeutralClayPlan (const glaze::ClayPlan& p) noexcept
        {
            return p.lowCutHz == 0.0f && p.monoAmount == 0.0f && p.widthDb == 0.0f;
        }

        juce::String describeClay (float lowCutHz, float monoHz, float monoAmount, float widthDb)
        {
            return "lowCut " + juce::String (lowCutHz, 4) + " Hz, mono " + juce::String (monoAmount, 4) + " @ "
                 + juce::String (monoHz, 2) + " Hz, width " + juce::String (widthDb, 4) + " dB";
        }

        juce::String describe (const ClayModule::Params& p)   { return describeClay (p.lowCutHz, p.monoHz, p.monoAmount, p.widthDb); }
        juce::String describe (const glaze::ClayPlan& p)      { return describeClay (p.lowCutHz, p.monoHz, p.monoAmount, p.widthDb); }

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

        // 带次声偏置的低音（T-044，BUG-011 / ADR-028 的端到端夹具；4 s，两声道相同，各分量在 4 s 里都是整数周期，循环播放没有接缝）：
        //   低音 = 50 Hz（0.45）+ 100 Hz（0.2）+ 150 Hz（0.08）+ 均匀噪声 ±0.035，次声 = 2.5 Hz 正弦（infraAmplitude）。
        //   limited = 再过一个前瞻限幅器（±5 ms 窗内峰值超过 0.6 就按比例压到 0.6，一阶 50 ms 释放；在三遍拼接上算、取中间一遍，
        //   循环处也连续）——模拟成品母带：限幅器把次声一起算进去了，切掉次声后原来被它压住的那一侧峰值冒出来（真歌 Jamendo deep、
        //   somggggg 的机理）。不限幅时顶峰来自低音与次声同向叠加，切掉次声顶峰反而下降（Jamendo tech、woqu 一类）。
        //   109 上的守护模型（docs/validation/2026-10-01-T044-clay-guard-loud-masters/tools/fixture_proto.py，20 Hz 低切）：
        //   limited、0.06 → 先试口径代价 +0.61 dB、旧压力格 2 s 平均最大 0.047 dB、混合损失最大 0.05 dB；不限幅、0.08 → 代价 −1.18 dB、
        //   守护 25 % 的时长 g < 1。
        Signal makeInfraBassMaterial (bool limited, double infraAmplitude)
        {
            const auto numSamples = (int) (4.0 * defaultRate);
            const auto twoPi = juce::MathConstants<double>::twoPi;
            juce::Random noise (4407);
            std::vector<double> raw ((size_t) numSamples);

            for (int i = 0; i < numSamples; ++i)
            {
                const auto t = (double) i / defaultRate;
                raw[(size_t) i] = 0.45 * std::sin (twoPi * 50.0 * t) + 0.2 * std::sin (twoPi * 100.0 * t + 0.7)
                                + 0.08 * std::sin (twoPi * 150.0 * t + 1.9) + 0.035 * (2.0 * (double) noise.nextFloat() - 1.0)
                                + infraAmplitude * std::sin (twoPi * 2.5 * t);
            }

            auto out = raw;

            if (limited)
            {
                constexpr double ceiling = 0.6;
                const auto look = (int) std::lround (0.005 * defaultRate);
                const auto release = std::exp (-1.0 / (0.05 * defaultRate));
                const auto total = 3 * numSamples;
                const auto at = [&raw, numSamples] (int i) { return raw[(size_t) (((i % numSamples) + numSamples) % numSamples)]; };
                std::vector<double> gain ((size_t) total);
                auto current = 1.0;

                for (int i = 0; i < total; ++i)
                {
                    auto peak = 0.0;

                    for (int k = i - look; k <= i + look; ++k)
                        peak = juce::jmax (peak, std::abs (at (k)));

                    const auto wanted = peak > ceiling ? ceiling / peak : 1.0;
                    current = wanted < current ? wanted : 1.0 - (1.0 - current) * release;
                    gain[(size_t) i] = current;
                }

                for (int i = 0; i < numSamples; ++i)
                    out[(size_t) i] = raw[(size_t) i] * gain[(size_t) (numSamples + i)];
            }

            auto signal = makeSilence (numChannels, numSamples);

            for (int i = 0; i < numSamples; ++i)
                signal[0][(size_t) i] = signal[1][(size_t) i] = (float) out[(size_t) i];

            return signal;
        }

        // 守护回收指示用的低音素材（T-074；4 s）：M 与 S 各自独立的低频噪声（白噪声过两级 90 Hz 一阶低通），S 比 M 低 10 dB（幅度 0.316），
        // L = M + S、R = M − S，峰值归一到 0.5（−6 dBFS，远在守护地板 −40 dBFS 之上）。手动收单 120 Hz 去掉 S 的低频：在每个局部峰值上
        // 守护都没有余量（界 B = 干路自己的局部峰值），效果大半被收回——与 ClayTests 的夹具 B（T-032 R4-02）同一机理、这里不用粉噪声。
        // 循环播放的接缝处噪声不连续，只有一个样本，不影响统计。
        Signal makeBassNoiseMidSide (juce::int64 seed)
        {
            const auto numSamples = (int) (4.0 * defaultRate);
            const auto pole = std::exp (-juce::MathConstants<double>::twoPi * 90.0 / defaultRate);
            juce::Random midNoise (seed), sideNoise (seed + 1);
            std::vector<double> mid ((size_t) numSamples), side ((size_t) numSamples);
            double midState[2] {}, sideState[2] {};

            for (int i = 0; i < numSamples; ++i)
            {
                const auto m = 2.0 * (double) midNoise.nextFloat() - 1.0;
                const auto s = 2.0 * (double) sideNoise.nextFloat() - 1.0;
                midState[0] = (1.0 - pole) * m + pole * midState[0];
                midState[1] = (1.0 - pole) * midState[0] + pole * midState[1];
                sideState[0] = (1.0 - pole) * s + pole * sideState[0];
                sideState[1] = (1.0 - pole) * sideState[0] + pole * sideState[1];
                mid[(size_t) i] = midState[1];
                side[(size_t) i] = sideState[1];
            }

            const auto rms = [numSamples] (const std::vector<double>& v)
            {
                auto sum = 0.0;

                for (const auto x : v)
                    sum += x * x;

                return std::sqrt (sum / (double) numSamples);
            };

            const auto sideGain = 0.316 * rms (mid) / rms (side);
            auto signal = makeSilence (numChannels, numSamples);
            auto peak = 0.0;

            for (int i = 0; i < numSamples; ++i)
            {
                const auto m = mid[(size_t) i], s = sideGain * side[(size_t) i];
                signal[0][(size_t) i] = (float) (m + s);
                signal[1][(size_t) i] = (float) (m - s);
                peak = juce::jmax (peak, std::abs (m + s), std::abs (m - s));
            }

            const auto scale = (float) (0.5 / peak);

            for (auto& channel : signal)
                for (auto& sample : channel)
                    sample *= scale;

            return signal;
        }

        // 守护不动作的低音素材（T-043）：单一 60 Hz 稳态正弦（0.3，两声道相同；4 s 里整数周期，循环播放没有接缝）。低切只转它的相位、
        // 不改幅度，峰值的前移（20 Hz 低切在 60 Hz 约 51°，约 2.4 ms）落在守护 ±8 ms 的局部峰值窗里，守护从不动作（ClayTests 稳态单音
        // 那条），混合损失为 0——用作「Clay 开着、守护与压力反馈都不该动」的播放素材。makeClayMaterial 的 60 + 110 Hz 谐波对经低切
        // 抬峰，守护频繁回收、回收时与干路相位不同处互相抵消（混合损失），压力反馈的第二个触发量（ADR-027）会降档。
        Signal makeSteadyBass()
        {
            const auto numSamples = (int) (4.0 * defaultRate);
            auto signal = makeSilence (numChannels, numSamples);
            const auto twoPi = juce::MathConstants<double>::twoPi;

            for (int i = 0; i < numSamples; ++i)
                signal[0][(size_t) i] = signal[1][(size_t) i] = (float) (0.3 * std::sin (twoPi * 60.0 * (double) i / defaultRate));

            return signal;
        }

        // 按 Clay 规格「最需要处理」造的画像（格式 2，特征全部有效、置信度 1；数字按 Clay 子文档第 2 节与根治修订 3.3 算）：
        //   真次声占比 −10 dB → need_sub = ramp(−10, −40, −28) = 1 → 低切给满（通透 20 Hz、温润 16 Hz）；
        //   低频 S/M −30 dB、ρ 0.98 → 收单 0（ADR-015 起自动不收单；按 v0.1.7.0 的公式单声道回放低音损失 −0.04 dB，need_mono 也是 0）；
        //   高频 S/M −16 dB → need_w = ramp(−16, −10, −18) = 0.75 → GLAZE 60 通透加宽 1.5 × 0.75 × 0.6 × 1.0 = 0.675 dB
        //   （上限 γ² ≤ 0.6 × 10^1.6 ≈ 23.9，不受限）；
        //   「先试后用」的代价（ADR-012）全部有效且 ≤ 0.2 dB → 低切 −0.5、加宽 0 dB 的代价系数 1（照做）；收单 −0.3 dB 照给，
        //   决策器不用（ADR-015）。代价无效的画像三项都中性（旧画像同理），所以这里必须给。
        // 用例拿它当阳性对照（「自动」确实给出非中性值：低切、加宽）；反相素材上只亮提示、收单仍是 0（ADR-015）。
        glaze::MusicProfile withClayNeeds (glaze::MusicProfile p)
        {
            p.formatVersion = glaze::clayProfileFormatVersion;
            p.subShareDb      = { -10.0f, true, 20.0, 1.0f };
            p.lowSideToMidDb  = { -30.0f, true, 20.0, 1.0f };
            p.lowCorrelation  = { 0.98f, true, 20.0, 1.0f };
            p.highSideToMidDb = { -16.0f, true, 20.0, 1.0f };
            p.highCorrelation = { 0.95f, true, 20.0, 1.0f };

            for (size_t b = 0; b < (size_t) glaze::numBands; ++b)
            {
                const auto high = b >= 17;   // 第 17 段 = 2 kHz（40 Hz 起 1/3 倍频程）
                p.bandSideToMidDb[b] = high ? -16.0f : -30.0f;
                p.bandCorrelation[b] = high ? 0.95f : 0.98f;
                p.bandStereoConfidence[b] = 1.0f;
            }

            p.lowCutPeakCostDb = { -0.5f, true, 20.0, 1.0f };
            for (auto& cost : p.monoPeakCostDb)
                cost = { -0.3f, true, 20.0, 1.0f };
            p.widthPeakCostDb = { 0.0f, true, 20.0, 1.0f };

            return p;
        }

        // 同上，但「试过」的代价让两项受限（ADR-012 设计规格-Clay-根治修订 3.3，代价系数 f = ramp(代价, 0.6, 0.2)）：
        //   低切代价 0.4 dB → f = 0.5（减量：低切仍开，频率低于给满时）；加宽代价 0.8 dB → f = 0（否决：加宽 0）；
        //   收单代价 −0.3 dB 决策器不用（ADR-015：自动不收单）。决策器应把低切、加宽记进 costLimitedMask。
        glaze::MusicProfile withCostLimitedClay (glaze::MusicProfile p)
        {
            p = withClayNeeds (p);
            p.lowCutPeakCostDb = { 0.4f, true, 20.0, 1.0f };
            p.widthPeakCostDb = { 0.8f, true, 20.0, 1.0f };
            return p;
        }

        // 学一段真实素材得到的已提交版本，把画像换成上面的 Clay 画像：方案清空（读回后按当前算法重算）、校准作废。
        glaze::CommittedState clayStateFrom (const glaze::CommittedState& learned)
        {
            auto state = learned;
            state.profile = withClayNeeds (state.profile);
            state.profileId = glaze::computeProfileId (state.profile);
            state.plan = glaze::Plan {};
            state.calibration = glaze::Calibration {};
            return state;
        }

        // v0.1.6.2 的 computeProfileId 原样照抄（「不含新字段」的参照实现）：格式 1 的画像号必须与它逐位相同，否则旧工程存档里
        // 校准参考条件的画像号对不上、校准失效、声音会变（Clay 第 7 节）。量化步长：电平 0.01、占比与置信度 1e-4、秒 0.01、
        // 起音密度 0.001；非有限值各有固定编码。
        std::uint32_t legacyProfileId (const glaze::MusicProfile& p) noexcept
        {
            struct Hash
            {
                std::uint32_t value = 2166136261u;

                void addWord (std::uint32_t word) noexcept
                {
                    for (int i = 0; i < 4; ++i)
                    {
                        value ^= (word >> (8 * i)) & 0xffu;
                        value *= 16777619u;
                    }
                }

                void addQuantized (double x, double step) noexcept
                {
                    constexpr std::int64_t nanCode = std::numeric_limits<std::int32_t>::min();
                    constexpr std::int64_t negativeInfinityCode = nanCode + 1;
                    constexpr std::int64_t positiveInfinityCode = std::numeric_limits<std::int32_t>::max();
                    std::int64_t code;

                    if (std::isnan (x))
                        code = nanCode;
                    else if (std::isinf (x))
                        code = x > 0.0 ? positiveInfinityCode : negativeInfinityCode;
                    else
                        code = (std::int64_t) std::llround (juce::jlimit ((double) negativeInfinityCode + 1.0,
                                                                          (double) positiveInfinityCode - 1.0, x / step));

                    addWord ((std::uint32_t) (std::int32_t) code);
                }

                void addFeature (const glaze::Feature& f, double step) noexcept
                {
                    addQuantized ((double) f.value, step);
                    addWord (f.valid ? 1u : 0u);
                    addQuantized (f.effectiveSeconds, 0.01);
                    addQuantized ((double) f.confidence, 1.0e-4);
                }

                void addBands (const std::array<float, (size_t) glaze::numBands>& bands, double step) noexcept
                {
                    for (const auto x : bands)
                        addQuantized ((double) x, step);
                }
            };

            Hash h;
            h.addWord ((std::uint32_t) p.formatVersion);
            h.addFeature (p.integratedLufs, 0.01);
            h.addFeature (p.loudnessRangeLu, 0.01);
            h.addFeature (p.plrDb, 0.01);
            h.addFeature (p.truePeakDbtp, 0.01);
            h.addFeature (p.spectralSlopeDbPerOct, 0.01);
            h.addFeature (p.lowShare, 1.0e-4);
            h.addFeature (p.highShare, 1.0e-4);
            h.addFeature (p.lowBandRangeDb, 0.01);
            h.addFeature (p.onsetDensityPerSecond, 0.001);
            h.addFeature (p.onsetStrengthDb, 0.01);
            h.addBands (p.bandLongTermDb, 0.01);
            h.addBands (p.tameEvidenceDb, 0.01);
            h.addBands (p.recoverEvidenceDb, 0.01);
            h.addBands (p.bandConfidence, 1.0e-4);
            h.addQuantized (p.effectiveSeconds, 0.01);
            h.addQuantized ((double) p.representativeness, 1.0e-4);
            return h.value;
        }

        // v0.1.6.2 编排器的 conditionsHash（校准参考条件的 manualParamsHash）原样照抄：Clay 三项都「自动」且没旁通时，
        // 本版本的哈希必须与它逐位相同——旧工程存档里的校准参考条件用的就是它，不同 = 重开后校准不适用、偏差撤回、声音变。
        std::uint32_t legacyManualParamsHash (const ChainParams& m, float feedbackMaxGainDb) noexcept
        {
            struct Hash
            {
                std::uint32_t value = 2166136261u;

                void add (std::uint64_t v) noexcept
                {
                    for (int i = 0; i < 8; ++i)
                    {
                        value ^= (std::uint32_t) ((v >> (8 * i)) & 0xffu);
                        value *= 16777619u;
                    }
                }

                void addQuantized (float x, double scale) noexcept
                {
                    if (! std::isfinite (x))
                    {
                        add (0x7ff8dead7ff8deadULL);
                        return;
                    }

                    add ((std::uint64_t) std::llround (juce::jlimit (-1.0e12, 1.0e12, (double) x * scale)));
                }

                void addFlag (bool flag) noexcept   { add (flag ? 1u : 0u); }
            };

            constexpr double q = 1.0e4;
            Hash h;
            h.addQuantized (m.balance.recover, q);
            h.addQuantized (m.balance.tame, q);
            h.addQuantized (m.balance.brighten, q);
            h.add ((std::uint64_t) (int) m.balance.speed);
            h.addFlag (m.balance.bypass);
            h.addQuantized (m.color.toneDb, q);
            h.addQuantized (m.color.glue, q);
            h.addQuantized (m.color.heat, q);
            h.addQuantized (m.color.mixPercent, q);
            h.addQuantized (m.color.outputDb, q);
            h.addFlag (m.color.bypass);
            h.addQuantized (m.limit.gainDb, q);
            h.addQuantized (m.limit.releaseMs, q);
            h.addFlag (m.limit.autoRelease);
            h.addQuantized (m.limit.clip, q);
            h.addFlag (m.limit.bypass);
            h.addQuantized (feedbackMaxGainDb, 1.0e3);
            return h.value;
        }
    }

    //==============================================================================
    // ---- T-026（BUG-004，ADR-018）用的工具 ----
    namespace
    {
        // JUCE 9.0.2 RangedAudioParameter 的换算（juce_RangedAudioParameter.cpp）：值 → 吸附 → 0 ～ 1；0 ～ 1 → 值 → 吸附。
        // 宿主存 / 读工程时 APVTS 走的就是它：读工程 setDenormalisedValue → convertTo0to1 → setValue（convertFrom0to1），
        // 处理器读到的 getRawParameterValue 又是 convertFrom0to1 (getValue())——一次保存再重开，处理器看到的值走了两趟往返。
        float toNormalised (const juce::NormalisableRange<float>& range, float value)
        {
            return range.convertTo0to1 (range.snapToLegalValue (value));
        }

        float fromNormalised (const juce::NormalisableRange<float>& range, float normalised)
        {
            return range.snapToLegalValue (range.convertFrom0to1 (juce::jlimit (0.0f, 1.0f, normalised)));
        }

        float roundTrip (const juce::NormalisableRange<float>& range, float value)
        {
            return fromNormalised (range, toNormalised (range, value));
        }

        // 校准参考条件用到的每个浮点手动参数：真范围与它在 ChainParams 里的位置；clayMode 非空 = Clay 项（只在「手动」且 Clay 非默认时计入）。
        struct ReferenceParameter
        {
            const char* name;
            juce::NormalisableRange<float> range;
            float& (*field) (ChainParams&);
            ClayMode ClaySettings::* clayMode = nullptr;
            bool referenceItem = false;   // GLAZE、Ceiling、Lookahead：参考条件自己的项，不在手动参数哈希里
        };

        std::vector<ReferenceParameter> referenceParameters()
        {
            namespace r = param::ranges;
            return {
                { "limit gain",       r::limitGain(),       [] (ChainParams& p) -> float& { return p.limit.gainDb; } },
                { "limit ceiling",    r::limitCeiling(),    [] (ChainParams& p) -> float& { return p.limit.ceilingDb; }, nullptr, true },
                { "limit release",    r::limitRelease(),    [] (ChainParams& p) -> float& { return p.limit.releaseMs; } },
                { "limit lookahead",  r::limitLookahead(),  [] (ChainParams& p) -> float& { return p.limit.lookaheadMs; }, nullptr, true },
                { "limit clip",       r::limitClip(),       [] (ChainParams& p) -> float& { return p.limit.clip; } },
                { "color tone",       r::colorTone(),       [] (ChainParams& p) -> float& { return p.color.toneDb; } },
                { "color glue",       r::colorGlue(),       [] (ChainParams& p) -> float& { return p.color.glue; } },
                { "color heat",       r::colorHeat(),       [] (ChainParams& p) -> float& { return p.color.heat; } },
                { "color mix",        r::colorMix(),        [] (ChainParams& p) -> float& { return p.color.mixPercent; } },
                { "color output",     r::colorOutput(),     [] (ChainParams& p) -> float& { return p.color.outputDb; } },
                { "balance recover",  r::balanceRecover(),  [] (ChainParams& p) -> float& { return p.balance.recover; } },
                { "balance tame",     r::balanceTame(),     [] (ChainParams& p) -> float& { return p.balance.tame; } },
                { "balance brighten", r::balanceBrighten(), [] (ChainParams& p) -> float& { return p.balance.brighten; } },
                { "glaze",            r::glazeAmount(),     [] (ChainParams& p) -> float& { return p.glazePercent; }, nullptr, true },
                { "clay low cut",     r::clayLowCut(),      [] (ChainParams& p) -> float& { return p.claySettings.lowCutHz; }, &ClaySettings::lowCutMode },
                { "clay mono",        r::clayMono(),        [] (ChainParams& p) -> float& { return p.claySettings.monoHz; }, &ClaySettings::monoMode },
                { "clay width",       r::clayWidth(),       [] (ChainParams& p) -> float& { return p.claySettings.widthDb; }, &ClaySettings::widthMode },
            };
        }

        // 修 BUG-004 之前编排器的 conditionsHash 原样照抄（v0.1.8.0 的 Clay 三态段 + v0.1.9.0 的高级母带段；main b951d20）：
        // 新口径的参考里仍存这个旧口径哈希，它必须与旧版本逐位相同——旧存档里的参考、旧版本读新存档，比的都是它。
        std::uint32_t preFixManualParamsHash (const ChainParams& m, float feedbackMaxGainDb) noexcept
        {
            struct Fnv1a
            {
                std::uint32_t h = 2166136261u;

                void add (std::uint64_t v) noexcept
                {
                    for (int i = 0; i < 8; ++i)
                    {
                        h ^= (std::uint32_t) ((v >> (8 * i)) & 0xffu);
                        h *= 16777619u;
                    }
                }

                void addQuantized (float x, double scale) noexcept
                {
                    if (! std::isfinite (x))
                    {
                        add (0x7ff8dead7ff8deadULL);
                        return;
                    }

                    add ((std::uint64_t) std::llround (juce::jlimit (-1.0e12, 1.0e12, (double) x * scale)));
                }

                void addFlag (bool b) noexcept   { add (b ? 1u : 0u); }
            };

            const auto addClayItem = [] (Fnv1a& h, ClayMode mode, float manualValue, double scale) noexcept
            {
                h.add ((std::uint64_t) (int) mode);

                if (mode == ClayMode::manual)
                    h.addQuantized (manualValue, scale);
            };

            constexpr double q = 1.0e4;
            Fnv1a h;
            h.addQuantized (m.balance.recover, q);
            h.addQuantized (m.balance.tame, q);
            h.addQuantized (m.balance.brighten, q);
            h.add ((std::uint64_t) (int) m.balance.speed);
            h.addFlag (m.balance.bypass);
            h.addQuantized (m.color.toneDb, q);
            h.addQuantized (m.color.glue, q);
            h.addQuantized (m.color.heat, q);
            h.addQuantized (m.color.mixPercent, q);
            h.addQuantized (m.color.outputDb, q);
            h.addFlag (m.color.bypass);
            h.addQuantized (m.limit.gainDb, q);
            h.addQuantized (m.limit.releaseMs, q);
            h.addFlag (m.limit.autoRelease);
            h.addQuantized (m.limit.clip, q);
            h.addFlag (m.limit.bypass);
            h.addQuantized (feedbackMaxGainDb, 1.0e3);

            const auto& clay = m.claySettings;
            const auto clayDefault = clay.lowCutMode == ClayMode::automatic && clay.monoMode == ClayMode::automatic
                                  && clay.widthMode == ClayMode::automatic && ! clay.bypass;

            if (! clayDefault)
            {
                h.add (0x436c6179ULL);
                addClayItem (h, clay.lowCutMode, clay.lowCutHz, q);
                addClayItem (h, clay.monoMode, clay.monoHz, q);
                addClayItem (h, clay.widthMode, clay.widthDb, q);
                h.addFlag (clay.bypass);
            }

            const auto& advanced = m.balance;
            if (advanced.harshAmount > 0.0f || advanced.targetAmount > 0.0f)
            {
                h.add (0x4d617374ULL);
                h.addQuantized (advanced.harshAmount, q);
                if (advanced.harshAmount > 0.0f) h.addQuantized (advanced.transientPreserve, q);
                h.addQuantized (advanced.targetAmount, q);
                if (advanced.targetAmount > 0.0f)
                {
                    h.addFlag (advanced.hasTargetReference);
                    if (advanced.hasTargetReference)
                        for (const auto value : advanced.targetReferenceDb) h.addQuantized (value, q);
                    else h.addQuantized (advanced.targetSlopeDbPerOct, q);
                }
            }

            return h.h;
        }

        // 编排器的 makeReference 同一口径（手动参数部分由 setManualConditions 填；GLAZE、Ceiling、Lookahead 是参考条件自己的项）。
        glaze::CalibrationReference referenceFor (const ChainParams& p, float feedbackMaxGainDb)
        {
            glaze::CalibrationReference r;
            r.profileId = 0x2601u;
            r.style = glaze::Style::warm;
            r.glazePercent = p.glazePercent;
            r.ceilingDb = p.limit.ceilingDb;
            r.lookaheadMs = p.limit.lookaheadMs;
            r.sampleRate = defaultRate;
            glaze::setManualConditions (r, p, feedbackMaxGainDb);
            return r;
        }

        // 这个参数此刻计不计入参考条件（Clay 项只在该项「手动」且 Clay 非默认设置时计入；其余一直计入）。
        bool counted (const ReferenceParameter& parameter, const ChainParams& p)
        {
            if (parameter.clayMode == nullptr)
                return true;

            const auto& clay = p.claySettings;
            const auto clayDefault = clay.lowCutMode == ClayMode::automatic && clay.monoMode == ClayMode::automatic
                                  && clay.widthMode == ClayMode::automatic && ! clay.bypass;
            return ! clayDefault && clay.*(parameter.clayMode) == ClayMode::manual;
        }

        // 挪一个显示步进（往区间里面挪）：线性项一个吸附步进；对数刻度项显示整数 ms / Hz，挪 1。
        float oneDisplayStep (const ReferenceParameter& parameter, float value)
        {
            const auto step = parameter.range.interval > 0.0f ? parameter.range.interval : 1.0f;
            const auto up = value + step;
            return up <= parameter.range.end ? up : value - step;
        }

        // 往返两趟（一次保存再重开）后旧口径量化码会翻转的一个旋钮位置：从真范围里按归一化位置从小到大找第一个。
        struct DriftingValue
        {
            bool found = false;
            float saved = 0.0f, reopened = 0.0f;
        };

        DriftingValue findDriftingValue (const juce::NormalisableRange<float>& range)
        {
            for (int i = 1; i < 100000; ++i)
            {
                const auto saved = fromNormalised (range, (float) i / 100000.0f);
                const auto reopened = roundTrip (range, roundTrip (range, saved));

                if (std::llround ((double) saved * 1.0e4) != std::llround ((double) reopened * 1.0e4))
                    return { true, saved, reopened };
            }

            return {};
        }

        // 把新版本写出的存档改成 v0.1.8.0 的样子：CALIBRATION 与执行快照的 REFERENCE 去掉新口径的属性（旧版本只存旧口径哈希）。
        void stripContinuousConditions (juce::XmlElement& element)
        {
            if (element.hasTagName ("CALIBRATION") || element.hasTagName ("REFERENCE"))
                for (auto* name : { "conditionsFormat", "discreteParamsHash", "limitReleaseMs", "clayLowCutHz", "clayMonoHz" })
                    element.removeAttribute (name);

            for (auto* child : element.getChildIterator())
                stripContinuousConditions (*child);
        }
    }

    class GlazeControllerTests  : public juce::UnitTest
    {
    public:
        GlazeControllerTests() : juce::UnitTest ("GlazeController", "Celadon") {}

        void runTest() override
        {
            testRestoredLiveSurvivesFirstPrepare();
            // 0.1.8.9 起 GLAZE 是连续编排器。本类里其余私有方法仍断言已删除的聆听 → 锁定 → 校准，
            // 不再调用。等价行为在下面这些用例里，对照 Phase2Contract 与 GlazeController::Impl。
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

        // 连续编排器：阶段停在 unlearned，聆听 / 锁定 / 校准计数保持 0，requestRelearn 是空操作。
        void testContinuousPhase()
        {
            beginTest ("phase stays unlearned; listen/lock counters stay 0; requestRelearn is a no-op");

            Rig rig (fastConstants(), makeMusic (defaultRate, 8.0, 11, 0.4f), defaultParams (50.0f, 0));
            auto flagsHeld = true;

            rig.run (4.0, [&] (int)
            {
                const auto s = rig.status();
                flagsHeld = flagsHeld && s.phase == Phase::unlearned && ! s.calibrationValid && ! s.profileReady && ! s.planReady
                            && s.listenedSeconds == 0.0f && ! s.houseLockPending && ! s.houseOfflineListening && ! s.relearnInProgress
                            && ! s.profileMismatch;
            });

            const auto before = rig.probe();
            const auto confidence = rig.status().confidence;
            rig.chain.requestGlazeRelearn();
            rig.step();
            const auto after = rig.probe();

            expect (flagsHeld, "phase, calibration and the retired listen flags stay at their phase-2 defaults");
            expectEquals (before.calibrationBegins, 0, "no calibration round");
            expectEquals (before.commits, 0, "no profile commit");
            expectEquals (before.learningStarts, 0, "learning is not started");
            expectEquals (before.feedbackSteps, 0, "constraint feedback is not stepped");
            expectEquals (after.commits, before.commits, "requestRelearn does not commit");
            expectEquals (after.calibrationBegins, before.calibrationBegins, "requestRelearn does not calibrate");
            expect (sameBits (rig.status().confidence, confidence), "requestRelearn does not clear confidence");
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
            expectEquals (rig.probe().calibrationBegins, 0, "a style change does not start calibration");
            expectEquals (rig.probe().commits, 0, "a style change does not commit a profile");
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
            expect (rig.phase() == Phase::unlearned, "phase stays unlearned across the reset");
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
            expect (donor.phase() == Phase::unlearned, "a null restore stays unlearned");
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
            expectEquals (rig.probe().commits, 0, "the cold start does not commit");
            expectEquals (rig.probe().calibrationBegins, 0, "the cold start does not calibrate");
            rig.chain.requestGlazeRelearn();
            rig.step();
            expectEquals (rig.probe().commits, 0, "requestRelearn does not commit a replacement profile");
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
        // 发行的 0.1.8.8 仍是 format 1 聆听树：两种顺序都冷启动，I 同样无效，旧聆听提示还在。
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

        //==============================================================================
        // 1. 状态转移：宿主停着 → 未学习（不聆听）；开始播放 → 聆听中（有效时长往上走，要求秒数满 min 后换成 max）；
        //    聆听结束 → 生成方案（3 s 过渡，此处 0.5 s）→ 校准中 → 已锁定；三个就绪标志依次置位；阶段严格按顺序出现、不跳。
        //    状态快照（getStatus）与测试口径（getPhaseForTesting）一致；effectiveTarget 未学习时是 NaN。
        // 依据：时间线第 1、2 节（状态机表、「聆听中 | 开始播放即自动进入」「校准中 | 方案过渡结束」）；PHASE2 A5 → A4 的 Status 语义。
        void testPhasesAndFlags()
        {
            beginTest ("phases: unlearned until playback, listening, plan transition, calibrating, locked; ready flags follow");

            Rig rig (fastConstants(), makeMusic (defaultRate, 8.0, 11, 0.5f));
            rig.params.glazePercent = 50.0f;
            rig.host.isPlaying = false;

            rig.run (1.0);
            expect (rig.phase() == Phase::unlearned, "stopped host: stays unlearned");
            expectEquals (rig.probe().learningStarts, 0, "no listening while the host is stopped");
            expect (! rig.status().profileReady && ! rig.status().planReady && ! rig.status().calibrationValid);
            expect (std::isnan (rig.status().effectiveTargetLufs), "target unknown before learning -> NaN");
            expect (rig.chain.getEffectiveParams().limit.gainDb > 0.0f, "GLAZE > 0 before learning: conservative plan already adds gain");

            rig.host.isPlaying = true;

            std::vector<Phase> sequence { rig.phase() };
            float lastListened = -1.0f;
            bool listenedMonotone = true, sawMaxRequirement = false, statusAgrees = true;

            const auto locked = rig.runUntil (40.0, [&]
            {
                const auto s = rig.status();
                statusAgrees = statusAgrees && s.phase == rig.phase();

                if (sequence.back() != s.phase)
                    sequence.push_back (s.phase);

                if (s.phase == Phase::listening)
                {
                    listenedMonotone = listenedMonotone && s.listenedSeconds >= lastListened;
                    lastListened = s.listenedSeconds;
                    sawMaxRequirement = sawMaxRequirement || juce::approximatelyEqual (s.requiredSeconds, 3.0f);
                    expect (! s.profileReady, "profile not ready while listening for the first time");
                }

                if (s.phase == Phase::planTransition)
                    expect (s.profileReady && s.planReady && ! s.calibrationValid, "plan transition: profile and plan ready, calibration pending");

                return s.phase == Phase::locked && s.calibrationValid;
            });

            juce::String seen;
            for (auto p : sequence)
                seen << phaseName (p) << " ";

            logMessage ("  phase sequence: " + seen + "| probe: learning starts " + juce::String (rig.probe().learningStarts)
                        + ", commits " + juce::String (rig.probe().commits) + ", calibration rounds begun "
                        + juce::String (rig.probe().calibrationBegins));

            expect (locked, "must reach locked with a valid calibration");
            const std::vector<Phase> expected { Phase::unlearned, Phase::listening, Phase::planTransition, Phase::calibrating, Phase::locked };
            expect (sequence == expected, "phases in order without skipping: " + seen);
            expect (statusAgrees, "status snapshot phase == controller phase");
            expect (listenedMonotone, "listened seconds never go down while listening");
            expect (sawMaxRequirement, "after the minimum listen time the requirement switches to the maximum");
            expectEquals (rig.probe().learningStarts, 1);
            expectEquals (rig.probe().commits, 1);

            const auto s = rig.status();
            expect (s.profileReady && s.planReady && s.calibrationValid);
            expect (std::isfinite (s.effectiveTargetLufs), "locked with GLAZE > 0: the target is known");
            expect (! s.relearnInProgress && ! s.calibrationPaused && ! s.unlearnedOfflineWarning);
        }

        // 2. GLAZE 0：照常聆听并提交（前两个就绪标志），不开校准（待校准）；交给模块的参数每一块都逐位等于手动值。
        //    之后拧到 60：条件稳定后开始校准并锁定；再回 0：同一块起逐位等于手动值，校准偏差与保护偏移归零。
        // 依据：时间线第 1 节「GLAZE 为 0 时照常学习，可以完成前两项；校准标待校准、自动修正量保持 0」、第 4 节「GLAZE 回到 0 时
        //    自动校准偏移一并归零」；GlazeController.h「GLAZE ≤ 0 时实际参数逐位等于手动值」。
        void testGlazeZeroLearnsButLeavesManualUntouched()
        {
            beginTest ("GLAZE 0 learns but never calibrates or touches the manual values; turning it up calibrates; back to 0 is bit-exact");

            auto initial = defaultParams (0.0f, 1);
            initial.balance.recover = 12.5f;
            initial.color.glue = 20.0f;
            initial.limit.gainDb = 2.0f;
            Rig rig (fastConstants(), makeMusic (defaultRate, 8.0, 12, 0.5f), initial);

            bool bitExact = true;
            rig.run (8.0, [&] (int) { bitExact = bitExact && sameParams (rig.chain.getEffectiveParams(), rig.params); });

            expect (bitExact, "GLAZE 0: effective params == manual, bit for bit, every block");
            expect (rig.status().profileReady && rig.status().planReady, "learning completes at GLAZE 0");
            expect (! rig.status().calibrationValid, "no calibration at GLAZE 0 (pending)");
            expect (rig.phase() == Phase::locked);
            expectEquals (rig.probe().calibrationBegins, 0, "calibration never starts at GLAZE 0");

            rig.params.glazePercent = 60.0f;
            expect (rig.learnAndCalibrate (30.0), "turning GLAZE up: calibrates once the conditions are stable");
            expect (rig.probe().calibrationBegins >= 1);

            rig.params.glazePercent = 0.0f;
            bitExact = true;
            rig.run (1.0, [&] (int) { bitExact = bitExact && sameParams (rig.chain.getEffectiveParams(), rig.params); });

            expect (bitExact, "back to GLAZE 0: bit-exact manual values from the first block");
            expectEquals (rig.probe().appliedBiasDb, 0.0f, "calibration offset back to zero");
            expectEquals (rig.probe().guardDb, 0.0f, "protection offset back to zero");
        }

        // 3. 首次聆听中点「重新聆听」= 从 0 重新开始这一轮（不是取消）；getStatus 在音频线程接到请求之前就反映出来；
        //    relearnInProgress 在首次聆听里始终为假。
        // 依据：PHASE2 设计决定 2；A5 → A4「getStatus() 最好立刻反映挂起的 requestRelearn」。
        void testFirstListenButtonRestarts()
        {
            beginTest ("first listen: the button restarts the round from zero, visible in the status before the next block");

            Rig rig (fastConstants(), makeMusic (defaultRate, 8.0, 13, 0.5f));
            rig.params.glazePercent = 40.0f;
            rig.run (1.5);

            expect (rig.phase() == Phase::listening);
            expect (rig.status().listenedSeconds > 1.0f, "listened for a while");

            rig.chain.requestGlazeRelearn();
            const auto pending = rig.status();
            expect (pending.phase == Phase::listening && ! pending.relearnInProgress, "still listening, not a relearn");
            expectEquals (pending.listenedSeconds, 0.0f, "status shows the restart at once");

            rig.step();
            expectEquals (rig.probe().learningStarts, 2, "restarted the listening round");
            expect (rig.status().listenedSeconds < 0.2f, "accumulated time dropped back to ~0");
            expect (! rig.status().relearnInProgress);
        }

        // 4. 重新聆听：候选在后台建，旧的已提交版本照常生效（已提交状态与交给模块的参数都不变）；期间拧 GLAZE 按旧画像即时响应；
        //    中途保存 = 旧的完整已提交版本；再点一次取消（状态立刻回到原阶段）；再开一次、听完后整体替换并做方案过渡。
        // 依据：时间线第 2 节「重新聆听」、第 4 节「重新聆听期间拧 GLAZE，按旧的已提交画像即时响应」、第 5 节「聆听中途保存」。
        void testRelearnCandidateAndCancel()
        {
            beginTest ("relearn builds a candidate while the committed version plays on; cancel, mid-listen save, swap with a transition");

            Rig rig (fastConstants(), makeMusic (defaultRate, 8.0, 14, 0.5f));
            rig.params.glazePercent = 50.0f;
            expect (rig.learnAndCalibrate(), "first learn");

            const auto before = rig.committed();
            const auto effectiveBefore = rig.chain.getEffectiveParams();
            const auto plannedBefore = rig.probe().increments;
            const auto tameCap = glaze::blendConstants (before.constants, before.plan.weights).tameCap * rig.params.glazePercent * 0.01f;
            const auto transitionsBefore = rig.probe().planTransitions;

            rig.chain.requestGlazeRelearn();
            {
                const auto s = rig.status();
                expect (s.phase == Phase::listening && s.relearnInProgress && s.listenedSeconds == 0.0f,
                        "status shows the relearn at once");
            }

            // 候选换一段同电平、内容不同的素材听：画像会不同，但已提交计划仍完全不变。
            // 实时 Tame 可以继续跟随当前音乐，其他实际参数必须保持原值。
            rig.source = makeMusic (defaultRate, 8.0, 99, 0.5f);
            bool committedUnchanged = true, planUnchanged = true, effectiveUnchanged = true, tameBounded = true;

            rig.run (1.0, [&] (int)
            {
                committedUnchanged = committedUnchanged && glaze::bitwiseEqual (rig.committed(), before);
                planUnchanged = planUnchanged && sameIncrements (rig.probe().increments, plannedBefore);
                auto effective = rig.chain.getEffectiveParams();
                tameBounded = tameBounded && effective.balance.tame >= rig.params.balance.tame + plannedBefore.balanceTame - 1.0e-4f
                                          && effective.balance.tame <= juce::jmin (100.0f, rig.params.balance.tame + tameCap) + 1.0e-4f;
                effective.balance.tame = effectiveBefore.balance.tame;
                effective.balance.automaticTame = effectiveBefore.balance.automaticTame;
                effectiveUnchanged = effectiveUnchanged && sameParams (effective, effectiveBefore);
            });

            expect (rig.probe().relearning, "candidate in progress");
            expect (committedUnchanged, "the committed version does not change while the candidate builds");
            expect (planUnchanged, "every automatic plan increment remains the committed version while relearning");
            expect (effectiveUnchanged, "all effective parameters except the independent live Tame authority remain unchanged");
            expect (tameBounded, "live Tame stays between the committed authority and the existing style/macro cap");

            // 中途保存：旧的完整已提交版本。
            {
                const auto xml = rig.chain.createGlazeStateXml();
                glaze::CommittedState parsed;
                expect (xml != nullptr && glaze::fromXml (*xml, parsed), "state saved mid-listen parses");
                expect (glaze::bitwiseEqual (parsed, before), "mid-listen save == the old complete committed version");
            }

            // 期间拧 GLAZE：按旧画像即时响应（这一块就等于决策器在旧画像上的方案）。
            rig.params.glazePercent = 70.0f;
            rig.step();
            {
                const auto expected = planFor (before, glaze::Style::automatic, 70.0f, rig.params, rig.probe().feedbackMaxGainDb);
                expect (sameIncrements (rig.probe().increments, expected.increments), "GLAZE responds at once on the old profile");
            }

            rig.params.glazePercent = 50.0f;

            // 取消：状态立刻回到原来的阶段；已提交版本不受影响。
            rig.chain.requestGlazeRelearn();
            {
                const auto s = rig.status();
                expect (! s.relearnInProgress && s.phase != Phase::listening, "cancel shows at once");
            }

            rig.run (0.5);
            expect (! rig.probe().relearning, "candidate cancelled");
            expect (glaze::bitwiseEqual (rig.committed(), before), "cancel leaves the committed version alone");
            expectEquals (rig.probe().planTransitions, transitionsBefore, "no plan transition from a cancelled relearn");

            // 再开一次，听完：整体替换（画像不同）+ 方案过渡；过渡期间增量逐块连续变化（没有跳变）。
            rig.chain.requestGlazeRelearn();
            float worstStep = 0.0f;
            glaze::AutoIncrements last = rig.probe().increments;

            const auto swapped = rig.runUntil (10.0, [&]
            {
                const auto inc = rig.probe().increments;
                worstStep = juce::jmax (worstStep, std::abs (inc.limitGainDb - last.limitGainDb), std::abs (inc.colorGlue - last.colorGlue) / 10.0f);
                last = inc;
                return rig.probe().commits == 2;
            });

            expect (swapped, "the candidate is committed after listening");
            expect (rig.committed().profileId != before.profileId, "a new profile replaced the old one");
            expectEquals (rig.probe().planTransitions, transitionsBefore + 1, "one plan transition for the swap");

            rig.run (1.0, [&] (int)
            {
                const auto inc = rig.probe().increments;
                worstStep = juce::jmax (worstStep, std::abs (inc.limitGainDb - last.limitGainDb), std::abs (inc.colorGlue - last.colorGlue) / 10.0f);
                last = inc;
            });

            logMessage ("  relearn swap: worst per-block step " + juce::String (worstStep, 4) + " dB (Limit Gain; Glue / 10)");
            expect (worstStep < 0.5f, "the swap is a smooth transition, not a jump");
        }

        // 5. 跳播 / 循环回绕 / 宿主 reset 各清各的：
        //    - 连续播放、停着挪光标、没有播放位置：都不算不连续点；
        //    - 跳播：计一次，聆听已累计的时长保留，保护层不动；
        //    - 循环回绕（循环打开、位置往回跳）：计一次回绕；
        //    - 宿主 reset：保护偏移归 0、过渡直接到终点，聆听时长与已提交版本保留。
        // 依据：时间线第 3 节（三个不同的清理函数，「清运行状态，不删已提交的画像与静态方案」「若正在聆听，已累计的有效时长保留」）。
        void testDiscontinuitiesClearTheirOwnState()
        {
            beginTest ("transport jump, loop wrap and host reset each clear their own runtime state");

            // 聆听中：三种不连续点都保留已累计的时长。
            {
                Rig rig (fastConstants(), makeMusic (defaultRate, 8.0, 15, 0.5f));
                rig.params.glazePercent = 30.0f;

                auto c = fastConstants();
                c.minListenSeconds = 20.0;
                c.maxListenSeconds = 20.0;
                rig.chain.getGlazeController().setConstantsForTesting (c);
                rig.reprepare (defaultRate);

                rig.run (1.0);
                expectEquals (rig.probe().transportJumps + rig.probe().loopWraps + rig.probe().hostResets, 0,
                              "continuous playback is not a discontinuity");

                // 停着挪光标、再从新位置开始播：不算跳播。
                rig.host.isPlaying = false;
                rig.run (0.2);
                rig.position += 480000;
                rig.run (0.2);
                rig.host.isPlaying = true;
                rig.run (0.2);
                expectEquals (rig.probe().transportJumps, 0, "moving the cursor while stopped is not a jump");

                const auto listened = rig.status().listenedSeconds;
                rig.position += 96000;          // 跳播
                rig.step();
                expectEquals (rig.probe().transportJumps, 1, "a jump while playing is detected");
                expect (rig.status().listenedSeconds >= listened, "listening keeps what it has accumulated across a jump");

                rig.host.hasLoop = true;
                rig.host.looping = true;
                rig.run (0.3);
                rig.position -= 48000;          // 回绕
                rig.step();
                expectEquals (rig.probe().loopWraps, 1, "a backward jump inside a loop is a wrap");
                expectEquals (rig.probe().transportJumps, 1, "... and not counted as a jump");

                const auto beforeReset = rig.status().listenedSeconds;
                rig.chain.restartWithFadeIn();
                rig.step();
                expectEquals (rig.probe().hostResets, 1);
                expect (rig.status().listenedSeconds >= beforeReset, "host reset keeps the accumulated listening time");
                expect (rig.phase() == Phase::listening);

                // 没有播放位置：不判断。
                rig.host.hasPosition = false;
                rig.run (0.3);
                rig.position += 1234567;
                rig.run (0.3);
                expectEquals (rig.probe().transportJumps, 1, "no position info, no jump detection");
            }

            // 锁定后：跳播不动保护层，宿主 reset 把它归 0、过渡直接到终点。保护层靠一段推热的素材逼出退让
            //（冲击风格的限幅上限最紧：峰值 3 dB、p95 2 dB）。画像失配关掉：+14 dB 会先触发它直接压低上限，保护层就不必退了。
            {
                auto constants = withTightLimitBudgets (fastConstants());   // 保护层要出手（TestHelpers.h 的说明）
                constants.mismatchThresholdLu = constants.mismatchReleaseLu = 1.0e9f;
                Rig rig (constants, makeMusic (defaultRate, 8.0, 16, 0.2f), defaultParams (100.0f, 2));
                rig.run (6.0);

                rig.sourceGain = 5.0f;           // 学完之后素材推热 ~14 dB：方案超约束，保护层退让
                const auto engaged = rig.runUntil (8.0, [&] { return rig.probe().guardDb < -0.2f; });
                expect (engaged, "the guard retreats on material much hotter than the learned profile");

                const auto guardBefore = rig.probe().guardDb;
                rig.position += 96000;
                rig.step();
                expectEquals (rig.probe().transportJumps, 1);
                expect (std::abs (rig.probe().guardDb - guardBefore) <= 0.02f, "a jump leaves the guard as it is (slow protection carries on)");

                const auto committedBefore = rig.committed();
                rig.chain.restartWithFadeIn();
                expectEquals (rig.probe().guardDb, 0.0f, "host reset: protection offset back to 0");
                expect (! rig.probe().transitionActive, "host reset: transitions land on their target");
                expect (glaze::bitwiseEqual (rig.committed(), committedBefore), "host reset keeps the committed version");
            }
        }

        void testInternalSliceTimeline()
        {
            beginTest ("internal input slices retain host-position coverage, looping and missing-position semantics");
            auto listen = fastConstants();
            listen.minListenSeconds = listen.maxListenSeconds = 6.0;
            const auto material = makeMusic (defaultRate, 8.0, 4419, 0.2f);
            for (int mode = 0; mode < 3; ++mode)
            {
                Rig large (listen, material, defaultParams (50.0f, 3), defaultRate, 9600);
                Rig small (listen, material, large.params, defaultRate, 480);
                for (auto* testRig : { &large, &small })
                {
                    testRig->host.hasPosition = mode != 2;
                    testRig->host.hasLoop = testRig->host.looping = mode == 1;
                    testRig->host.loopStartSamples = 0;
                    testRig->host.loopEndSamples = (std::int64_t) defaultRate;
                    for (int b = 0; b < testRig->blocksFor (8.0); ++b)
                    {
                        if (mode == 1)
                            testRig->position %= (std::int64_t) defaultRate;
                        testRig->step();
                    }
                }
                const auto tag = "host-position mode " + juce::String (mode);
                expect (large.committed().hasProfile && small.committed().hasProfile, tag + ": genuine learning completes");
                expectWithinAbsoluteError (large.committed().profile.representativeness, small.committed().profile.representativeness,
                                           0.014f, tag + ": each 100ms cell maps to the correct song position");
                expectEquals (large.probe().transportJumps, 0, tag + ": internal slices do not create transport jumps");
                expectEquals (large.probe().loopWraps, small.probe().loopWraps, tag + ": genuine wraps are still identified");
                if (mode == 0)
                    expect (large.committed().profile.representativeness >= 0.38f,
                            "large callbacks count all distinct six seconds; actual " + juce::String (large.committed().profile.representativeness, 5));
                if (mode == 1)
                    expect (large.committed().profile.representativeness < 0.10f, "replaying the same one-second loop is not new material");
            }
        }

        // v5: the offline flag is not a different mastering strategy. Both paths advance on sample counts.
        void expectSameContinuation (Rig& realtime, Rig& render, double seconds, const juce::String& label)
        {
            bool paramsEqual = true;
            float maximumAudioDifference = 0.0f, maximumGainDifference = 0.0f;
            for (int b = 0; b < realtime.blocksFor (seconds); ++b)
            {
                realtime.step();
                render.step();
                paramsEqual = paramsEqual && sameParams (realtime.chain.getEffectiveParams(), render.chain.getEffectiveParams());
                maximumGainDifference = juce::jmax (maximumGainDifference,
                    std::abs (realtime.chain.getEffectiveParams().limit.gainDb - render.chain.getEffectiveParams().limit.gainDb));
                for (int ch = 0; ch < numChannels; ++ch)
                    for (int i = 0; i < realtime.blockLength; ++i)
                        maximumAudioDifference = juce::jmax (maximumAudioDifference,
                            std::abs (realtime.buffer.getSample (ch, i) - render.buffer.getSample (ch, i)));
            }
            expect (paramsEqual, label + ": all executed parameters equal; max gain difference " + juce::String (maximumGainDifference, 8));
            expectEquals (maximumAudioDifference, 0.0f, label + ": sample-identical output");
            expectEquals (render.probe().commits, realtime.probe().commits, label + ": same genuine learning commits");
        }

        void testOfflineDoesNotLearnOrCalibrate()
        {
            beginTest ("realtime and offline use the same learning, calibration and loud-section safety state machine");
            const auto music = makeMusic (defaultRate, 8.0, 17, 0.2f);
            Rig realtime (fastConstants(), music, defaultParams (60.0f, 2));
            Rig offline (fastConstants(), music, realtime.params);
            offline.host.offline = true;
            offline.host.isPlaying = false; // Real hosts may report a stopped transport throughout a render.
            expectSameContinuation (realtime, offline, 15.0, "new instances from the beginning");
            expect (offline.committed().hasProfile && offline.probe().calibrationBegins > 0);
            expect (! offline.status().unlearnedOfflineWarning);
            expect (! offline.status().waitingForPlayback);
            realtime.sourceGain = offline.sourceGain = 5.0f;
            expectSameContinuation (realtime, offline, 8.0, "loud chorus after the profile locks");
            expectEquals (offline.probe().mismatchSteps, realtime.probe().mismatchSteps);
            expectEquals (offline.probe().feedbackSteps, realtime.probe().feedbackSteps);

            auto slow = fastConstants();
            slow.minListenSeconds = 15.0;
            slow.maxListenSeconds = 30.0;
            Rig a (slow, music, defaultParams (60.0f, 2)), b (slow, music, a.params);
            expectSameContinuation (a, b, 5.0, "first five seconds of listening");
            expect (! a.committed().hasProfile);
            b.host.offline = true;
            expectSameContinuation (a, b, 18.0, "switch to offline while learning");
        }

        void testExecutionSnapshots()
        {
            beginTest ("execution snapshot preserves unfinished learning, applied calibration and deterministic replay");
            const auto music = makeMusic (defaultRate, 8.0, 172, 0.2f);
            auto slow = fastConstants();
            slow.minListenSeconds = 15.0;
            slow.maxListenSeconds = 30.0;
            Rig donor (slow, music, defaultParams (60.0f, 3));
            donor.run (5.0);
            const auto heard = donor.chain.getEffectiveParams();
            const auto xml = donor.chain.createGlazeStateXml();
            expect (xml->getChildByName (glaze::executionTag) != nullptr);
            expect (! donor.committed().hasProfile && ! donor.status().calibrationValid);
            Rig a (slow, music, donor.params), b (slow, music, donor.params);
            a.chain.restoreGlazeStateXml (xml.get());
            b.chain.restoreGlazeStateXml (xml.get());
            b.host.offline = true;
            a.step(); b.step();
            expect (a.probe().executionSnapshotActive && ! a.status().profileReady && ! a.status().calibrationValid,
                    "provisional execution does not pretend to be a finished profile or calibration");
            expectWithinAbsoluteError (a.chain.getEffectiveParams().limit.gainDb, heard.limit.gainDb, 1.0e-5f, "first block keeps the audible Gain");
            expectWithinAbsoluteError (a.chain.getEffectiveParams().color.heat, heard.color.heat, 1.0e-5f, "first block keeps the audible Heat");
            expectSameContinuation (a, b, 18.0, "two provisional restores, realtime versus offline");
            expect (a.committed().hasProfile, "new evidence eventually produces a genuine committed profile");

            Rig changed (slow, music, donor.params);
            changed.params.glazePercent += 10.0f;
            changed.chain.restoreGlazeStateXml (xml.get());
            changed.step();
            expect (! changed.probe().executionSnapshotActive, "different manual/macro conditions invalidate execution");
            Rig changedRate (slow, music, donor.params);
            changedRate.reprepare (44100.0);
            changedRate.chain.restoreGlazeStateXml (xml.get());
            changedRate.step();
            expect (! changedRate.probe().executionSnapshotActive, "a different sample rate invalidates execution");

            // Strong manual Tone gives the real chain a measurable loudness change absent from the feed-forward level model.
            auto params = defaultParams (75.0f, 3);
            params.color.toneDb = 2.0f;
            Rig calibrating (fastConstants(), music, params), uninterrupted (fastConstants(), music, params);
            bool foundAppliedBias = false;
            for (int i = 0; i < calibrating.blocksFor (35.0); ++i)
            {
                calibrating.step(); uninterrupted.step();
                if (calibrating.phase() == Phase::calibrating && std::abs (calibrating.probe().executedBiasDb) > 0.02f)
                {
                    foundAppliedBias = true;
                    break;
                }
            }
            expect (foundAppliedBias, "capture a real unfinished correction; executed bias " + juce::String (calibrating.probe().executedBiasDb, 6));
            const auto biasXml = calibrating.chain.createGlazeStateXml();
            const auto biasGain = calibrating.chain.getEffectiveParams().limit.gainDb;
            const auto executedBias = calibrating.probe().executedBiasDb;
            calibrating.host.offline = true;
            expectSameContinuation (uninterrupted, calibrating, 8.0, "switch offline during an applied calibration correction");
            Rig c (fastConstants(), music, params), d (fastConstants(), music, params);
            c.chain.restoreGlazeStateXml (biasXml.get());
            d.chain.restoreGlazeStateXml (biasXml.get());
            d.host.offline = true;
            c.step(); d.step();
            expectWithinAbsoluteError (c.chain.getEffectiveParams().limit.gainDb, biasGain, 1.0e-5f, "restoring an unfinished round does not drop audible compensation");
            expectWithinAbsoluteError (c.probe().executedBiasDb, executedBias, 1.0e-5f, "the actually applied bias is retained");
            expectSameContinuation (c, d, 10.0, "two unfinished-calibration restores");

            beginTest ("a completed calibration cannot replace the saved executed bias before its slew has finished");
            auto oneRound = fastConstants();
            oneRound.calibrationRounds = 1;
            auto boostedBass = defaultParams (60.0f, 3);
            boostedBass.color.toneDb = -3.0f;
            Rig finishing (oneRound, music, boostedBass);
            const auto finished = finishing.runUntil (25.0, [&] { return finishing.probe().calibrationFinishes > 0; });
            const auto finishedBias = finishing.probe().executedBiasDb;
            const auto finishedGain = finishing.chain.getEffectiveParams().limit.gainDb;
            const auto correctionStillPending = std::abs (finishing.committed().calibration.gainBiasDb - finishedBias);
            expect (finished && finishing.committed().calibration.valid && correctionStillPending > 0.02f,
                    "real audio produces a finished correction before it is applied; executed " + juce::String (finishedBias, 6)
                    + ", target " + juce::String (finishing.committed().calibration.gainBiasDb, 6)
                    + ", finishes " + juce::String (finishing.probe().calibrationFinishes));
            const auto finishedXml = finishing.chain.createGlazeStateXml();
            Rig completedRestore (oneRound, music, boostedBass), completedOffline (oneRound, music, boostedBass);
            completedRestore.chain.restoreGlazeStateXml (finishedXml.get());
            completedOffline.chain.restoreGlazeStateXml (finishedXml.get());
            completedOffline.host.offline = true;
            completedRestore.step();
            completedOffline.step();
            expectWithinAbsoluteError (completedRestore.probe().executedBiasDb, finishedBias, 1.0e-5f,
                                       "completed target cannot overwrite the last heard correction");
            expectWithinAbsoluteError (completedRestore.chain.getEffectiveParams().limit.gainDb, finishedGain, 1.0e-5f,
                                       "save on the finishing block preserves actual Gain");

            float largestBiasStep = 0.0f, replayDifference = 0.0f;
            auto previousBias = completedRestore.probe().executedBiasDb;
            bool continuationEqual = true;
            for (int i = 0; i < completedRestore.blocksFor (correctionStillPending + 0.1); ++i)
            {
                completedRestore.step(); completedOffline.step();
                const auto bias = completedRestore.probe().executedBiasDb;
                largestBiasStep = juce::jmax (largestBiasStep, std::abs (bias - previousBias));
                previousBias = bias;
                continuationEqual = continuationEqual && sameParams (completedRestore.chain.getEffectiveParams(), completedOffline.chain.getEffectiveParams());
                for (int ch = 0; ch < numChannels; ++ch)
                    for (int sample = 0; sample < blockSize; ++sample)
                        replayDifference = juce::jmax (replayDifference,
                            std::abs (completedRestore.buffer.getSample (ch, sample) - completedOffline.buffer.getSample (ch, sample)));
            }
            expect (largestBiasStep <= (float) (blockSize / defaultRate) + 1.0e-6f,
                    "completed correction resumes at no more than 1 dB/s; largest block step " + juce::String (largestBiasStep, 8));
            expectWithinAbsoluteError (completedRestore.probe().executedBiasDb, finishing.committed().calibration.gainBiasDb, 1.0e-5f,
                                       "the pending completed correction actually reaches its target");
            expectEquals (completedRestore.probe().calibrationBegins, 0, "an unchanged completed plan does not need another measurement round");
            expect (continuationEqual, "finished correction continuation has identical real-time/offline parameters");
            expectEquals (replayDifference, 0.0f, "finished correction continuation has sample-identical real-time/offline output");

            // The guard may have consumed both the base increment and part of a positive calibration correction.
            glaze::ExecutionState protectedExecution;
            const auto* finishedExecution = finishedXml->getChildByName (glaze::executionTag);
            expect (finishedExecution != nullptr);
            if (finishedExecution != nullptr && glaze::fromXml (*finishedExecution, protectedExecution))
            {
                protectedExecution.plan.increments.limitGainDb = 0.1f;
                protectedExecution.gainBiasDb = 0.3f;
                protectedExecution.protectionDb = -0.2f;
                auto protectedXml = std::make_unique<juce::XmlElement> (*finishedXml);
                protectedXml->removeChildElement (protectedXml->getChildByName (glaze::executionTag), true);
                protectedXml->addChildElement (glaze::toXml (protectedExecution).release());
                Rig protectedRestore (oneRound, music, boostedBass);
                protectedRestore.chain.restoreGlazeStateXml (protectedXml.get());
                protectedRestore.step();
                expectWithinAbsoluteError (protectedRestore.chain.getEffectiveParams().limit.gainDb,
                                           boostedBass.limit.gainDb + 0.2f, 1.0e-5f,
                                           "protection beyond the saved base increment remains absorbed in the held correction");
                expect (protectedRestore.committed().calibration.valid && ! protectedRestore.status().calibrationValid,
                        "rebased execution needs verification without rewriting the saved completed calibration record");
                const auto rebasedXml = protectedRestore.chain.createGlazeStateXml();
                glaze::ExecutionState rebasedExecution;
                expect (glaze::fromXml (*rebasedXml->getChildByName (glaze::executionTag), rebasedExecution)
                        && rebasedExecution.requiresCalibration && rebasedExecution.protectionDb == 0.0f,
                        "pending verification survives serialization after the protection was absorbed");
                Rig rebasedRealtime (oneRound, music, boostedBass), rebasedOffline (oneRound, music, boostedBass);
                rebasedRealtime.chain.restoreGlazeStateXml (rebasedXml.get());
                rebasedOffline.chain.restoreGlazeStateXml (rebasedXml.get());
                rebasedOffline.host.offline = true;
                expect (! rebasedRealtime.status().calibrationValid, "a second reopen still reports the rebased execution as unverified");
                rebasedRealtime.step(); rebasedOffline.step();
                expectWithinAbsoluteError (rebasedRealtime.chain.getEffectiveParams().limit.gainDb,
                                           boostedBass.limit.gainDb + 0.2f, 1.0e-5f, "a second reopen retains the protected first-block Gain");
                expectSameContinuation (rebasedRealtime, rebasedOffline, 8.0, "protected execution verification after a second reopen");
                expect (rebasedRealtime.probe().calibrationBegins > 0 && rebasedRealtime.probe().calibrationFinishes > 0,
                        "protection-rebased execution completes a genuine verification round; begins "
                        + juce::String (rebasedRealtime.probe().calibrationBegins) + ", finishes "
                        + juce::String (rebasedRealtime.probe().calibrationFinishes));

                beginTest ("malformed execution increments cannot override manual controls");
                protectedExecution.plan.increments.balanceRecover = -100.0f;
                glaze::ExecutionState ignored;
                expect (! glaze::fromXml (*glaze::toXml (protectedExecution), ignored), "negative Recover rejected");
                protectedExecution.plan.increments.balanceRecover = 0.0f;
                protectedExecution.plan.effectiveGainDb = std::numeric_limits<float>::infinity();
                expect (! glaze::fromXml (*glaze::toXml (protectedExecution), ignored), "non-finite plan metadata rejected");
            }
        }

        // 7. 保存再读取：createStateXml → 两条全新的链各自 restoreStateXml → 从同一起点渲染同一段素材，输出逐位相同；
        //    读回的链第一块就是锁定、不聆听、没有过渡、增量 = 原链的稳态增量、校准偏差直接到位；与原链 reset 之后的渲染也逐位相同。
        //    读取之后、第一块之前 createStateXml 返回刚恢复的内容；读空 / 读坏 = 未学习（getStatus 立刻反映）。
        // 依据：时间线第 5、6 节（重开工程立即恢复、不重学、不从默认方案过渡、平滑器同时设当前值与目标值、从同一起点重渲染可复现）；
        //    GlazeController.h「restoreStateXml 之后、第一块音频之前调 createStateXml，返回的是刚恢复的内容」。
        void testSaveAndRestore()
        {
            beginTest ("save / restore: takes effect at once, re-renders bit-exactly from the same start; empty or broken data = unlearned");

            const auto material = makeMusic (defaultRate, 8.0, 19, 0.5f);

            Rig original (fastConstants(), material);
            original.params.glazePercent = 60.0f;
            expect (original.learnAndCalibrate(), "original learns and calibrates");

            const auto xml = original.chain.createGlazeStateXml();
            expect (xml != nullptr && xml->hasTagName (glaze::stateTag));

            const auto steadyIncrements = original.probe().increments;
            const auto steadyBias = original.probe().executedBiasDb;

            auto restoredRig = [&]
            {
                auto rig = std::make_unique<Rig> (fastConstants(), material);
                rig->params = original.params;
                rig->chain.restoreGlazeStateXml (xml.get());
                return rig;
            };

            auto b = restoredRig();
            auto c = restoredRig();

            // 读取之后、第一块之前：存出来的就是刚读进去的。
            {
                const auto again = b->chain.createGlazeStateXml();
                expect (again != nullptr && again->isEquivalentTo (xml.get(), false), "createStateXml right after restore returns the restored content");
                expect (b->status().phase == Phase::locked && b->status().profileReady, "status reflects the restored state before any audio");
            }

            b->step();
            expect (b->phase() == Phase::locked, "restored: locked on the first block");
            expectEquals (b->probe().learningStarts, 0, "no relearning after a restore");
            expect (! b->probe().transitionActive, "no plan transition after a restore");
            expect (sameIncrements (b->probe().increments, steadyIncrements), "first block already uses the saved plan's increments");
            expectEquals (b->probe().executedBiasDb, steadyBias, "actually executed calibration offset in place on the first block");

            // 从同一起点重渲染（宿主在渲染前 reset）：两条读回的链逐位相同；原链 reset 之后也一样。
            c->step();

            for (auto* rig : { &original, b.get(), c.get() })
            {
                rig->chain.reset();
                rig->sourceIndex = 0;
                rig->position = 0;
                rig->step();
            }

            const auto resetOriginal = original.chain.getEffectiveParams();
            const auto resetRestored = b->chain.getEffectiveParams();
            expect (sameParams (resetOriginal, resetRestored),
                    "reset and restore cold-start from the same executed parameters; Gain "
                    + juce::String (resetOriginal.limit.gainDb - resetRestored.limit.gainDb, 8)
                    + ", bias " + juce::String (original.probe().executedBiasDb - b->probe().executedBiasDb, 8)
                    + ", Tone " + juce::String (resetOriginal.color.toneDb - resetRestored.color.toneDb, 8)
                    + ", Tame " + juce::String (resetOriginal.balance.tame - resetRestored.balance.tame, 8));

            const auto compareBlocks = b->blocksFor (4.0);
            auto outB = makeSilence (numChannels, compareBlocks * blockSize);
            auto outC = outB, outOriginal = outB;
            int firstParameterDifference = -1, firstAudioDifference = -1;
            for (int block = 0; block < compareBlocks; ++block)
            {
                b->step(); c->step(); original.step();
                const auto& aParams = original.chain.getEffectiveParams();
                const auto& bParams = b->chain.getEffectiveParams();
                if (firstParameterDifference < 0 && ! sameParams (aParams, bParams))
                {
                    firstParameterDifference = block;
                    logMessage ("  first reset/restore PARAM mismatch block=" + juce::String (block)
                        + " Gain=" + juce::String (aParams.limit.gainDb, 9) + "/" + juce::String (bParams.limit.gainDb, 9)
                        + " Tame=" + juce::String (aParams.balance.tame, 9) + "/" + juce::String (bParams.balance.tame, 9)
                        + " Tone=" + juce::String (aParams.color.toneDb, 9) + "/" + juce::String (bParams.color.toneDb, 9)
                        + " Heat=" + juce::String (aParams.color.heat, 9) + "/" + juce::String (bParams.color.heat, 9)
                        + " Glue=" + juce::String (aParams.color.glue, 9) + "/" + juce::String (bParams.color.glue, 9)
                        + " bias=" + juce::String (original.probe().executedBiasDb, 9) + "/" + juce::String (b->probe().executedBiasDb, 9)
                        + " colorReduction=" + juce::String (original.probe().liveHeatReduction, 9) + "/" + juce::String (b->probe().liveHeatReduction, 9)
                        + " slices=" + juce::String (original.chain.getLastProcessSliceCountForTesting())
                        + "/" + juce::String (b->chain.getLastProcessSliceCountForTesting()));
                }
                for (int ch = 0; ch < numChannels; ++ch)
                    for (int sample = 0; sample < blockSize; ++sample)
                    {
                        const auto index = (size_t) (block * blockSize + sample);
                        outB[(size_t) ch][index] = b->buffer.getSample (ch, sample);
                        outC[(size_t) ch][index] = c->buffer.getSample (ch, sample);
                        outOriginal[(size_t) ch][index] = original.buffer.getSample (ch, sample);
                        if (firstAudioDifference < 0 && ! sameBits (outB[(size_t) ch][index], outOriginal[(size_t) ch][index]))
                        {
                            firstAudioDifference = block;
                            logMessage ("  first reset/restore AUDIO mismatch block=" + juce::String (block)
                                + " sample=" + juce::String (sample) + " parametersEqual=" + juce::String ((int) sameParams (aParams, bParams))
                                + " slices=" + juce::String (original.chain.getLastProcessSliceCountForTesting())
                                + "/" + juce::String (b->chain.getLastProcessSliceCountForTesting()));
                        }
                    }
            }
            logMessage ("  reset/restore trajectory: first parameter block=" + juce::String (firstParameterDifference)
                        + ", first audio block=" + juce::String (firstAudioDifference));

            expect (sameSignal (outB, outC), "two restores render bit-identically from the same start");
            float largestResetDifference = 0.0f;
            for (size_t ch = 0; ch < outB.size(); ++ch)
                for (size_t i = 0; i < outB[ch].size(); ++i)
                    largestResetDifference = juce::jmax (largestResetDifference, std::abs (outB[ch][i] - outOriginal[ch][i]));
            expect (sameSignal (outB, outOriginal), "a restore renders like the original after a reset (same start, same state); max sample difference "
                    + juce::String (largestResetDifference, 9));
            expectEquals (b->probe().guardDb, 0.0f, "(the comparison ran with the guard at rest)");

            // 读空 / 读坏 = 未学习（状态立刻反映）。
            b->chain.restoreGlazeStateXml (nullptr);
            expect (b->status().phase == Phase::unlearned && ! b->status().profileReady, "null state -> unlearned, visible at once");
            b->step();
            expect (! b->committed().hasProfile);

            juce::XmlElement broken (glaze::stateTag);
            broken.setAttribute ("formatVersion", 1);
            c->chain.restoreGlazeStateXml (&broken);
            c->step();
            expect (! c->committed().hasProfile, "broken state -> unlearned");
        }

        // 8. 采样率改变：画像（画像号）与锁定的风格权重保留，校准标「待验证」（不适用），条件稳定后自动再校准，不重听。
        // 依据：时间线第 5 节「采样率改变」。
        void testSampleRateChange()
        {
            beginTest ("sample-rate change keeps profile and style weights, marks calibration pending, recalibrates when stable");

            Rig rig (fastConstants(), makeMusic (defaultRate, 8.0, 20, 0.5f));
            rig.params.glazePercent = 55.0f;
            expect (rig.learnAndCalibrate(), "learned at 48 kHz");

            const auto before = rig.committed();
            const auto begins = rig.probe().calibrationBegins;

            rig.source = makeMusic (96000.0, 8.0, 20, 0.5f);
            rig.sourceIndex = 0;
            rig.reprepare (96000.0);
            rig.step();

            const auto after = rig.committed();
            expectEquals ((juce::int64) after.profileId, (juce::int64) before.profileId, "profile kept");
            expect (sameBits (after.lockedWeights.warm, before.lockedWeights.warm) && sameBits (after.lockedWeights.clean, before.lockedWeights.clean),
                    "locked style weights kept");
            expect (! rig.status().calibrationValid, "calibration pending after the rate change");
            expectEquals (rig.probe().learningStarts, 1, "no relistening");

            expect (rig.learnAndCalibrate (30.0), "recalibrates at the new rate");
            expect (rig.probe().calibrationBegins > begins);
            expectEquals ((juce::int64) rig.committed().profileId, (juce::int64) before.profileId, "still the same profile");

            // 只换最大块长（处理器照样整体重建链）：分析器与块长无关，不重建——进行中的重新聆听不作废（v0.6 验收 V2-05）；
            // 采样率一变照旧作废（上面已学完，这里再起一轮重新聆听来看）。
            rig.chain.requestGlazeRelearn();
            rig.run (1.0);
            const auto listenedBefore = rig.status().listenedSeconds;
            expect (rig.status().relearnInProgress && listenedBefore > 0.5f, "a relearn is running");

            rig.chain.setParams (rig.params, false);
            rig.chain.prepare ({ rig.sampleRate, (juce::uint32) (2 * rig.blockLength), (juce::uint32) numChannels });
            rig.step();
            expect (rig.status().relearnInProgress, "a block-size-only rebuild keeps the relearn going");
            expect (rig.status().listenedSeconds >= listenedBefore, "and keeps what it has listened to");

            rig.reprepare (48000.0);
            rig.source = makeMusic (48000.0, 8.0, 20, 0.5f);
            rig.sourceIndex = 0;
            rig.step();
            expect (! rig.status().relearnInProgress, "a sample-rate change voids the ongoing relearn");
        }

        void testSameSpecReprepareKeepsHeardExecution()
        {
            beginTest ("a block-size-only rebuild retains actual Guard retreat, not the unprotected plan");
            auto guardedConstants = withTightLimitBudgets (fastConstants());   // 保护层要出手（TestHelpers.h 的说明）
            guardedConstants.mismatchThresholdLu = guardedConstants.mismatchReleaseLu = 1.0e9f;
            Rig guarded (guardedConstants, makeMusic (defaultRate, 8.0, 16, 0.2f), defaultParams (100.0f, 2));
            guarded.run (6.0);
            guarded.sourceGain = 5.0f;
            const auto retreated = guarded.runUntil (8.0, [&] { return guarded.probe().executedGuardDb < -0.2f; });
            const auto protectedParams = guarded.chain.getEffectiveParams();
            const auto protectionBefore = guarded.probe().executedGuardDb;
            const auto profileBefore = guarded.committed().profileId;
            expect (retreated, "real audio has executed protection before rebuilding: " + juce::String (protectionBefore, 7));

            // Match Processor::prepareToPlay: this prepares the NEXT block's controls before rebuilding.
            // Only the previously rendered block is authoritative for the cold-start execution snapshot.
            guarded.chain.setParams (guarded.params, false);
            guarded.chain.prepare ({ defaultRate, (juce::uint32) (2 * blockSize), (juce::uint32) numChannels });
            expectEquals (guarded.probe().guardDb, 0.0f, "the Guard detector itself still restarts empty");
            guarded.step();
            expectWithinAbsoluteError (guarded.chain.getEffectiveParams().limit.gainDb, protectedParams.limit.gainDb, 1.0e-5f,
                                       "absorbed retreat preserves the last heard Gain");
            expectEquals (guarded.probe().executedGuardDb, 0.0f, "cold-start protection is absorbed into the saved execution");
            expectEquals (guarded.chain.getEffectiveParams().color.heat, protectedParams.color.heat, "heard Heat survives the rebuild");
            expectEquals (guarded.chain.getEffectiveParams().color.glue, protectedParams.color.glue, "heard Glue survives the rebuild");
            expectEquals ((juce::int64) guarded.committed().profileId, (juce::int64) profileBefore, "the learned profile is retained");

            beginTest ("a block-size-only rebuild resumes a completed correction from the last heard bias, then finishes its slew");
            auto oneRound = fastConstants();
            oneRound.calibrationRounds = 1;
            auto params = defaultParams (60.0f, 3);
            params.color.toneDb = -3.0f;
            Rig finishing (oneRound, makeMusic (defaultRate, 8.0, 172, 0.2f), params);
            const auto finished = finishing.runUntil (25.0, [&] { return finishing.probe().calibrationFinishes > 0; });
            const auto heardBias = finishing.probe().executedBiasDb;
            const auto heardGain = finishing.chain.getEffectiveParams().limit.gainDb;
            const auto targetBias = finishing.committed().calibration.gainBiasDb;
            const auto remaining = std::abs (targetBias - heardBias);
            const auto begins = finishing.probe().calibrationBegins;
            expect (finished && finishing.committed().calibration.valid && remaining > 0.02f,
                    "real completed calibration still has unapplied correction: heard=" + juce::String (heardBias, 7)
                    + ", target=" + juce::String (targetBias, 7));
            finishing.chain.setParams (finishing.params, false);
            expect (std::abs (finishing.probe().executedBiasDb - heardBias) > 1.0e-6f,
                    "pre-prepare setParams has advanced the unheard next-block bias: " + juce::String (finishing.probe().executedBiasDb, 7));
            finishing.chain.prepare ({ defaultRate, (juce::uint32) (2 * blockSize), (juce::uint32) numChannels });
            finishing.step();
            expectWithinAbsoluteError (finishing.probe().executedBiasDb, heardBias, 1.0e-5f, "first rebuilt block retains the heard bias");
            expectWithinAbsoluteError (finishing.chain.getEffectiveParams().limit.gainDb, heardGain, 1.0e-5f, "first rebuilt block retains the heard Gain");
            auto previousBias = finishing.probe().executedBiasDb;
            auto maximumStep = 0.0f;
            finishing.run (remaining + 0.1, [&] (int)
            {
                maximumStep = juce::jmax (maximumStep, std::abs (finishing.probe().executedBiasDb - previousBias));
                previousBias = finishing.probe().executedBiasDb;
            });
            expect (maximumStep <= (float) (blockSize / defaultRate) + 1.0e-6f,
                    "completed correction still slews at 1 dB/s; maximum block step=" + juce::String (maximumStep, 8));
            expectWithinAbsoluteError (finishing.probe().executedBiasDb, targetBias, 1.0e-5f, "the already verified target is eventually reached");
            expectEquals (finishing.probe().calibrationBegins, begins, "rebuilding does not invent another calibration round");
        }

        // 9. GLAZE 连续控制（宿主每块自动化）：不重学、不做方案过渡、不开新的校准；每一块的自动增量逐位等于决策器在已提交画像、
        //    锁定权重、本块 GLAZE 与手动值下的方案（确定映射）；自动化期间校准不适用（只用前馈），GLAZE 回到参考值后重新适用。
        // 依据：时间线第 4 节（两条变化路径表、「连续自动化期间没有适用校准就只用前馈」）；决策第 6 节参考条件。
        void testGlazeAutomationPath()
        {
            beginTest ("GLAZE automation: no relearn, no transition, no new calibration; increments follow the deterministic mapping");

            Rig rig (fastConstants(), makeMusic (defaultRate, 8.0, 21, 0.4f));
            rig.params.glazePercent = 60.0f;
            expect (rig.learnAndCalibrate(), "learned and calibrated at GLAZE 60");

            const auto committed = rig.committed();
            const auto before = rig.probe();

            bool mappingExact = true, neverApplicable = true;
            int blocks = 0;
            const auto automationBlocks = rig.blocksFor (3.0);

            for (int b = 0; b < automationBlocks; ++b)
            {
                rig.params.glazePercent = 55.0f + 25.0f * std::sin (0.05f * (float) b) + 0.01f;   // 每块都在变、从不停在 60
                rig.step();
                ++blocks;

                const auto expected = planFor (committed, glaze::Style::automatic, rig.params.glazePercent, rig.params,
                                               rig.probe().feedbackMaxGainDb);
                mappingExact = mappingExact && sameIncrements (rig.probe().increments, expected.increments);
                neverApplicable = neverApplicable && ! rig.status().calibrationValid;
            }

            const auto after = rig.probe();
            expect (mappingExact, "each block's increments == makePlan(committed profile, GLAZE of that block)");
            expect (neverApplicable, "the calibration does not apply while GLAZE moves (feed-forward only)");
            expectEquals (after.learningStarts, before.learningStarts, "no relearning");
            expectEquals (after.commits, before.commits);
            expectEquals (after.planTransitions, before.planTransitions, "no plan transition");
            expectEquals (after.calibrationBegins, before.calibrationBegins, "no calibration restart while automating");
            expect (glaze::bitwiseEqual (rig.committed().profile, committed.profile), "profile unchanged");
            logMessage ("  automated " + juce::String (blocks) + " blocks; applied bias now " + juce::String (after.appliedBiasDb, 3) + " dB");

            // 回到参考值 60：已提交的校准重新适用（不需要重新校准）。
            rig.params.glazePercent = 60.0f;
            rig.run (0.2);
            expect (rig.status().calibrationValid, "back at the reference GLAZE the stored calibration applies again");
        }

        // 10. 换风格：一次方案过渡（阶段显示为过渡），过渡后按新条件重新校准；约束反馈上限回到不限。
        // 依据：时间线第 4 节「方案切换 | 新画像提交、切换离散风格 | 约 3 s 整体过渡」；决策第 6 节（风格是参考条件）。
        void testStyleChangeIsAPlanTransition()
        {
            beginTest ("switching style is one plan transition, then calibration for the new conditions");

            Rig rig (fastConstants(), makeMusic (defaultRate, 8.0, 22, 0.4f));
            rig.params.glazePercent = 70.0f;
            expect (rig.learnAndCalibrate(), "learned (auto style)");

            const auto before = rig.probe();
            rig.params.glazeStyle = 2;   // 冲击
            rig.step();

            expectEquals (rig.probe().planTransitions, before.planTransitions + 1, "one transition for the style change");
            expect (rig.probe().transitionActive && rig.phase() == Phase::planTransition, "transition in progress");
            expect (! rig.status().calibrationValid, "the old calibration does not apply to the new style");

            const auto done = rig.runUntil (0.6, [&] { return ! rig.probe().transitionActive; });
            expect (done, "the transition lasts about planTransitionSeconds (0.5 s)");

            expect (rig.learnAndCalibrate (30.0), "recalibrated for the new style");
            expect (rig.probe().calibrationBegins > before.calibrationBegins);
            expect (rig.status().resolvedStyle == glaze::Style::punchy, "resolved style follows the discrete choice");
        }

        // 11. 旧算法版本保存的方案：读回后照用保存的自动增量（不按新算法重算），状态标出来；拧 GLAZE 之后才按新算法重算（一次方案过渡）。
        // 依据：时间线第 5 节「版本不同时照用保存的自动增量数值，只有你主动重学、改风格或拧 GLAZE 时才按新算法重算，并在界面提示一次」。
        void testOlderAlgorithmPlanIsKept()
        {
            beginTest ("a plan saved by an older algorithm keeps its saved increments until GLAZE is turned");

            Rig original (fastConstants(), makeMusic (defaultRate, 8.0, 23, 0.4f));
            original.params.glazePercent = 50.0f;
            expect (original.learnAndCalibrate(), "learned");

            auto state = original.committed();
            state.plan.algorithmVersion = glaze::oldestCompatibleAlgorithmVersion - 1;   // 比兼容下限还旧：照用保存的增量
            state.plan.increments.colorGlue = 17.25f;          // 与新算法算出来的不同，便于分辨
            state.plan.increments.balanceRecover = 20.0f;
            state.plan.increments.limitGainDb = 1.5f;
            state.calibration = glaze::Calibration {};
            const auto xml = glaze::toXml (state);

            Rig rig (fastConstants(), makeMusic (defaultRate, 8.0, 23, 0.4f));
            rig.params = original.params;
            rig.chain.restoreGlazeStateXml (xml.get());
            expect (rig.status().savedPlanFromOlderAlgorithm, "status flags the older plan right after loading");

            rig.run (1.0);
            expect (rig.probe().savedPlanMode, "saved-plan mode");
            expectEquals (rig.chain.getEffectiveParams().balance.automaticRecover, 0.0f,
                          "older saved plans retain the original per-band Recover behavior");
            expect (sameIncrements (rig.probe().increments, state.plan.increments), "the saved increments are used as they are");
            expect (rig.status().savedPlanFromOlderAlgorithm);

            const auto transitions = rig.probe().planTransitions;
            rig.params.glazePercent = 55.0f;
            rig.run (1.0);

            expect (! rig.probe().savedPlanMode, "turning GLAZE switches to the current algorithm");
            expect (rig.chain.getEffectiveParams().balance.automaticRecover > 0.0f,
                    "newly recalculated automatic Recover uses the per-band protection");
            expectEquals (rig.probe().planTransitions, transitions + 1, "with one plan transition");
            const auto expected = planFor (rig.committed(), glaze::Style::automatic, 55.0f, rig.params, rig.probe().feedbackMaxGainDb);
            expect (sameIncrements (rig.probe().increments, expected.increments), "now the current algorithm's plan");
            expect (! rig.status().savedPlanFromOlderAlgorithm);
        }

        // 11b. v0.1.6.2 的工程（算法 6、画像格式 1）：算法 7 只多了 Clay，所以照常按当前算法采用，不进「照用旧增量」模式，
        // Clay 中性、其余自动增量与当前算法算出的相同（GlazeTypes.h isCompatibleAlgorithm；T-012 阶段 2 主进程裁决）。
        void testAlgorithmSixProjectIsCurrent()
        {
            beginTest ("an algorithm-6 (v0.1.6.2) project is adopted as current: no saved-plan mode, Clay neutral");

            Rig original (fastConstants(), makeMusic (defaultRate, 8.0, 23, 0.4f));
            original.params.glazePercent = 50.0f;
            expect (original.learnAndCalibrate(), "learned");

            auto state = original.committed();
            state.profile.formatVersion = 1;                     // v0.1.6.2 的画像：没有立体声 / 超低频字段
            state.profile.subShareDb = {};
            state.profile.lowSideToMidDb = {};
            state.profile.lowCorrelation = {};
            state.profile.highSideToMidDb = {};
            state.profile.highCorrelation = {};
            state.profile.bandSideToMidDb = {};
            state.profile.bandCorrelation = {};
            state.profile.bandStereoConfidence = {};
            state.plan.algorithmVersion = 6;
            state.plan.clay = glaze::ClayPlan {};
            state.constants.algorithmVersion = 6;
            expect (glaze::isCompatibleAlgorithm (6) && ! glaze::isCompatibleAlgorithm (5), "6 is the oldest compatible algorithm");

            Rig rig (fastConstants(), makeMusic (defaultRate, 8.0, 23, 0.4f));
            rig.params = original.params;
            rig.chain.restoreGlazeStateXml (glaze::toXml (state).get());
            expect (! rig.status().savedPlanFromOlderAlgorithm, "an algorithm-6 plan is not flagged as older");

            rig.run (1.0);
            expect (! rig.probe().savedPlanMode, "no saved-plan mode for algorithm 6");
            expect (isNeutralClay (rig.chain.getEffectiveParams().clay), "a format-1 profile gives neutral Clay");
            const auto expected = planFor (rig.committed(), glaze::Style::automatic, rig.params.glazePercent, rig.params,
                                           rig.probe().feedbackMaxGainDb);
            expect (sameIncrements (rig.probe().increments, expected.increments), "increments equal the current algorithm's plan");
            expect (isNeutralClayPlan (expected.clay), "the recomputed plan for a format-1 profile has neutral Clay");
        }

        void testOldHeatModelIsMigrated()
        {
            beginTest ("old Heat model snapshots are replanned with current compensation, preserving the profile and manual controls");
            Rig learned (fastConstants(), makeMusic (defaultRate, 8.0, 902, 0.2f));
            learned.run (4.0);
            auto state = learned.committed();
            expect (state.hasProfile);
            const auto profileId = state.profileId;
            const auto profile = state.profile;
            state.constants.algorithmVersion = 5;
            state.constants.heatLoudnessDb.fill (-20.0f);
            state.constants.heatPeakDb.fill (-24.0f);
            state.plan.algorithmVersion = 5;
            state.plan.increments.colorHeat = 60.0f;
            state.plan.increments.limitGainDb = 23.0f;
            state.calibration.valid = true;
            state.calibration.gainBiasDb = 3.0f;
            auto xml = glaze::toXml (state);
            auto* controller = xml->createNewChildElement ("GLAZE_CONTROLLER");
            controller->setAttribute ("formatVersion", 1);
            controller->setAttribute ("feedbackMaxGainDb", "00000000");

            auto params = defaultParams (50.0f, 3);
            params.limit.gainDb = 1.25f;
            params.color.toneDb = -0.2f;
            Rig restored (fastConstants(), learned.source, params);
            restored.chain.restoreGlazeStateXml (xml.get());
            expect (! restored.status().savedPlanFromOlderAlgorithm, "obsolete Heat gain is never exposed as the playback plan");
            expect (! restored.status().calibrationValid, "calibration from the obsolete curve is discarded");
            restored.step();
            const auto migrated = restored.committed();
            expectEquals (migrated.constants.algorithmVersion, glaze::currentAlgorithmVersion);
            expect (glaze::bitwiseEqual (migrated.constants, fastConstants()));
            expect (glaze::bitwiseEqual (migrated.profile, profile));
            expectEquals ((juce::int64) migrated.profileId, (juce::int64) profileId);
            expect (! restored.probe().savedPlanMode && restored.probe().learningStarts == 0);
            expect (restored.probe().feedbackMaxGainDb > 1.0e6f, "obsolete feedback cap is not carried to the new model");
            const auto expected = planFor (migrated, glaze::Style::clean, 50.0f, params, restored.probe().feedbackMaxGainDb);
            expect (sameIncrements (restored.probe().increments, expected.increments));
            expect (! sameBits (restored.probe().increments.limitGainDb, 23.0f), "the old large compensating Gain is replaced");
            expect (sameParams (restored.params, params), "migration never rewrites manual controls");
        }

        void testListenDoesNotPauseCalibration()
        {
            beginTest ("Listen keeps calibrating, while host bypass plus Listen still pauses calibration and protection");
            Rig rig (fastConstants(), makeMusic (defaultRate, 8.0, 11, 0.5f));
            rig.params.glazePercent = 50.0f;
            rig.params.limit.listen = true;
            expect (rig.learnAndCalibrate(), "a fresh instance learns and completes calibration with Listen enabled");
            expect (! rig.status().calibrationPaused);

            const auto before = rig.probe();
            rig.hostBypassed = true;
            rig.params.glazePercent = 55.0f;
            rig.run (0.1);
            const auto protection = rig.status().protectionDb;
            rig.run (3.0);
            expect (rig.status().calibrationPaused, "host bypass is recognised even when Listen is also enabled");
            expectEquals (rig.probe().calibrationBegins, before.calibrationBegins);
            expectEquals (rig.status().protectionDb, protection, "dry bypass statistics do not drive the guard");
            expect (! rig.status().calibrationValid);

            rig.hostBypassed = false;
            int blocks = 0;
            const auto resumed = rig.runUntil (30.0, [&]
            {
                // Switching a monitoring control during a round must not invalidate the measurement or restart the plan.
                rig.params.limit.listen = (++blocks / rig.blocksFor (0.2)) % 2 == 0;
                return rig.phase() == Phase::locked && rig.status().calibrationValid;
            });
            expect (resumed, "calibration resumes with the new knob setting while Listen keeps toggling");
            expect (! rig.status().calibrationPaused);
            expect (rig.probe().calibrationFinishes > before.calibrationFinishes);
            expectEquals (rig.probe().planTransitions, before.planTransitions, "Listen never changes the processing plan");
        }

        void testLiveColorAfterProfileLocks()
        {
            beginTest ("dense bright sections gently reduce only planned automatic Heat/Glue and sparse sections recover");
            auto constants = fastConstants();
            constants.minListenSeconds = 10.0;
            constants.maxListenSeconds = 12.0;
            constants.mismatchThresholdLu = constants.mismatchReleaseLu = 1.0e9f;
            const auto dense = makeNoise (numChannels, (int) (8.0 * defaultRate), 0.075f, 627);
            auto sparse = dense;
            const auto retain = std::exp (-juce::MathConstants<double>::twoPi * 800.0 / defaultRate);
            float peak = 0.0f;
            for (auto& channel : sparse)
            {
                double z = 0.0;
                for (size_t i = 0; i < channel.size(); ++i)
                {
                    z = channel[i] + (z - channel[i]) * retain;
                    const auto beatPosition = std::fmod ((double) i / defaultRate, 0.5);
                    channel[i] = (float) (z * (0.15 + 0.85 * std::exp (-beatPosition / 0.07)));
                    peak = juce::jmax (peak, std::abs (channel[i]));
                }
            }
            scale (sparse, 0.3f / peak);
            auto params = defaultParams (0.0f, 1);
            params.color.heat = 4.0f;
            params.color.glue = 5.0f;
            Rig learned (constants, sparse, params);
            learned.run (14.0);
            expect (learned.committed().hasProfile, "real sparse audio establishes the fixed reference profile");
            learned.params.glazePercent = params.glazePercent = 60.0f;
            learned.step();
            const auto donorXml = learned.chain.createGlazeStateXml();
            glaze::ExecutionState execution;
            const auto* donorExecution = donorXml->getChildByName (glaze::executionTag);
            expect (donorExecution != nullptr);
            if (donorExecution == nullptr || ! glaze::fromXml (*donorExecution, execution))
                return;
            auto state = learned.committed();
            // A saved execution with both automatic controls available isolates section adaptation from planner allocation.
            state.plan = execution.plan;
            state.plan.increments.colorHeat = 30.0f;
            state.plan.increments.colorGlue = 25.0f;
            state.plan.increments.limitGainDb = 0.0f;
            state.plan.effectiveGainDb = 0.0f;
            state.calibration.valid = true;
            state.calibration.gainBiasDb = 0.0f;
            state.calibration.reference = execution.reference;
            execution.plan = state.plan;
            execution.gainBiasDb = execution.protectionDb = 0.0f;
            execution.liveHeatReduction = execution.liveGlueReduction = 0.0f;
            execution.requiresCalibration = false;
            auto saved = glaze::toXml (state);
            saved->addChildElement (glaze::toXml (execution).release());
            Rig realtime (constants, dense, params), offline (constants, dense, params);
            realtime.chain.restoreGlazeStateXml (saved.get());
            offline.chain.restoreGlazeStateXml (saved.get());
            offline.host.offline = true;
            realtime.step(); offline.step();
            const auto original = realtime.chain.getEffectiveParams();
            float heatStep = 0.0f, glueStep = 0.0f, audioDifference = 0.0f;
            auto previous = original;
            bool paramsEqual = true, manualKept = true;
            std::uint64_t allocations = 0;
            for (int b = 0; b < realtime.blocksFor (10.0); ++b)
            {
                const auto before = allocationCountOnThisThread();
                realtime.step(); offline.step();
                allocations += allocationCountOnThisThread() - before;
                const auto effective = realtime.chain.getEffectiveParams();
                heatStep = juce::jmax (heatStep, std::abs (effective.color.heat - previous.color.heat));
                glueStep = juce::jmax (glueStep, std::abs (effective.color.glue - previous.color.glue));
                manualKept = manualKept && effective.color.heat >= params.color.heat && effective.color.glue >= params.color.glue;
                paramsEqual = paramsEqual && sameParams (effective, offline.chain.getEffectiveParams());
                for (int ch = 0; ch < numChannels; ++ch)
                    for (int sample = 0; sample < blockSize; ++sample)
                        audioDifference = juce::jmax (audioDifference,
                            std::abs (realtime.buffer.getSample (ch, sample) - offline.buffer.getSample (ch, sample)));
                previous = effective;
            }
            const auto reduced = realtime.chain.getEffectiveParams();
            expect (reduced.color.heat < original.color.heat - 2.0f && reduced.color.glue < original.color.glue - 1.0f,
                    "both automatic controls retreat on sustained dense/bright material; Heat " + juce::String (original.color.heat, 3)
                    + " -> " + juce::String (reduced.color.heat, 3) + ", Glue " + juce::String (original.color.glue, 3)
                    + " -> " + juce::String (reduced.color.glue, 3));
            expect (reduced.color.heat >= params.color.heat + 30.0f * (1.0f - constants.liveHeatMaxReduction) - 1.0e-5f);
            expect (reduced.color.glue >= params.color.glue + 25.0f * (1.0f - constants.liveGlueMaxReduction) - 1.0e-5f);
            expect (heatStep <= 30.0f * constants.liveColorAttackPerSecond * (float) (blockSize / defaultRate) + 1.0e-4f,
                    "Heat per-block step " + juce::String (heatStep, 7));
            expect (glueStep <= 25.0f * constants.liveColorAttackPerSecond * (float) (blockSize / defaultRate) + 1.0e-4f,
                    "Glue per-block step " + juce::String (glueStep, 7));
            expect (manualKept && paramsEqual, "manual parts stay intact and realtime/offline parameters match");
            expectEquals (audioDifference, 0.0f, "realtime/offline dynamic coloration is sample identical");
            expectEquals ((int) allocations, 0, "section adaptation allocates no audio-thread memory");
            expectEquals ((juce::int64) realtime.committed().profileId, (juce::int64) state.profileId);
            expect (glaze::bitwiseEqual (realtime.committed().profile, state.profile));
            expectEquals (realtime.probe().targetPlan.effectiveTargetLufs, state.plan.effectiveTargetLufs);
            expect (sameIncrements (realtime.probe().increments, state.plan.increments), "long-term plan remains unchanged");

            beginTest ("live coloration survives save/reset and old missing execution fields default to neutral");
            const auto adaptedXml = realtime.chain.createGlazeStateXml();
            Rig loudSection (constants, dense, params), quietSection (constants, dense, params);
            loudSection.chain.restoreGlazeStateXml (adaptedXml.get());
            quietSection.chain.restoreGlazeStateXml (adaptedXml.get());
            quietSection.sourceGain = 0.25f;
            double loudEnergy = 0.0, quietEnergy = 0.0;
            const auto sectionBlocks = loudSection.blocksFor (6.0);
            for (int b = 0; b < sectionBlocks; ++b)
            {
                loudSection.step(); quietSection.step();
                if (b >= sectionBlocks - loudSection.blocksFor (1.0))
                    for (int ch = 0; ch < numChannels; ++ch)
                        for (int sample = 0; sample < blockSize; ++sample)
                        {
                            const auto x = loudSection.buffer.getSample (ch, sample);
                            const auto y = quietSection.buffer.getSample (ch, sample);
                            loudEnergy += (double) x * x;
                            quietEnergy += (double) y * y;
                        }
            }
            const auto outputContrastDb = (float) (10.0 * std::log10 (quietEnergy / loudEnergy));
            expectWithinAbsoluteError (outputContrastDb, juce::Decibels::gainToDecibels (0.25f), 0.5f,
                                       "a quieter verse retains its 12 dB input contrast instead of being raised toward the chorus");
            expectEquals (quietSection.probe().targetPlan.effectiveTargetLufs, loudSection.probe().targetPlan.effectiveTargetLufs);
            expectEquals (quietSection.chain.getEffectiveParams().limit.gainDb, loudSection.chain.getEffectiveParams().limit.gainDb,
                          "section coloration does not add a quiet-section Gain boost");
            Rig restored (constants, dense, params);
            restored.chain.restoreGlazeStateXml (adaptedXml.get());
            restored.step();
            expectEquals (restored.chain.getEffectiveParams().color.heat, reduced.color.heat, "restore keeps actually heard Heat");
            expectEquals (restored.chain.getEffectiveParams().color.glue, reduced.color.glue, "restore keeps actually heard Glue");
            realtime.chain.reset();
            realtime.step();
            expectEquals (realtime.chain.getEffectiveParams().color.heat, reduced.color.heat, "reset keeps actually heard Heat");
            expectEquals (realtime.chain.getEffectiveParams().color.glue, reduced.color.glue, "reset keeps actually heard Glue");
            auto legacyExecutionXml = glaze::toXml (execution);
            legacyExecutionXml->removeAttribute ("liveHeatReduction");
            legacyExecutionXml->removeAttribute ("liveGlueReduction");
            glaze::ExecutionState legacy;
            expect (glaze::fromXml (*legacyExecutionXml, legacy));
            expectEquals (legacy.liveHeatReduction, 0.0f);
            expectEquals (legacy.liveGlueReduction, 0.0f);
            auto legacyConstantsXml = glaze::toXml (constants);
            legacyConstantsXml->removeAttribute ("liveHeatMaxReduction");
            legacyConstantsXml->removeAttribute ("liveGlueMaxReduction");
            glaze::Constants legacyConstants;
            expect (glaze::fromXml (*legacyConstantsXml, legacyConstants));
            expectEquals (legacyConstants.liveHeatMaxReduction, 0.0f);
            expectEquals (legacyConstants.liveGlueMaxReduction, 0.0f);

            beginTest ("dynamic color remains compatible with calibration and unfinished execution replay");
            auto calibrationState = state;
            auto calibrationExecution = execution;
            calibrationState.plan.increments.limitGainDb = 3.0f;
            calibrationState.plan.effectiveGainDb = 3.0f;
            calibrationState.calibration = glaze::Calibration {};
            calibrationExecution.plan = calibrationState.plan;
            auto calibratingXml = glaze::toXml (calibrationState);
            calibratingXml->addChildElement (glaze::toXml (calibrationExecution).release());
            Rig calibrating (constants, dense, params);
            calibrating.chain.restoreGlazeStateXml (calibratingXml.get());
            calibrating.run (4.0);
            const auto duringCalibration = calibrating.chain.createGlazeStateXml();
            const auto heardDuringCalibration = calibrating.chain.getEffectiveParams();
            Rig resumedRealtime (constants, dense, params), resumedOffline (constants, dense, params);
            resumedRealtime.chain.restoreGlazeStateXml (duringCalibration.get());
            resumedOffline.chain.restoreGlazeStateXml (duringCalibration.get());
            resumedOffline.host.offline = true;
            resumedRealtime.step(); resumedOffline.step();
            expectEquals (resumedRealtime.chain.getEffectiveParams().color.heat, heardDuringCalibration.color.heat,
                          "unfinished calibration snapshot restores the actually executed Heat fraction");
            expectEquals (resumedRealtime.chain.getEffectiveParams().color.glue, heardDuringCalibration.color.glue,
                          "unfinished calibration snapshot restores the actually executed Glue fraction");
            expectSameContinuation (resumedRealtime, resumedOffline, 20.0, "adaptive color and calibration from the same unfinished snapshot");
            expect (resumedRealtime.probe().calibrationFinishes > 0 && resumedRealtime.status().calibrationValid,
                    "slow section color does not leave calibration stuck; begins "
                    + juce::String (resumedRealtime.probe().calibrationBegins) + ", finishes "
                    + juce::String (resumedRealtime.probe().calibrationFinishes)
                    + ", phase " + phaseName (resumedRealtime.phase()));
            expectEquals (resumedRealtime.probe().targetPlan.effectiveTargetLufs, calibrationState.plan.effectiveTargetLufs,
                          "calibrating section color keeps the fixed loudness target");

            // Measure whether a correction learned during reduced Glue is carried too far into its later recovery.
            auto fixedState = calibrationState;
            fixedState.constants.liveHeatMaxReduction = fixedState.constants.liveGlueMaxReduction = 0.0f;
            auto fixedExecution = calibrationExecution;
            fixedExecution.constants = fixedState.constants;
            auto fixedXml = glaze::toXml (fixedState);
            fixedXml->addChildElement (glaze::toXml (fixedExecution).release());
            Rig fixedColor (fixedState.constants, dense, params);
            fixedColor.chain.restoreGlazeStateXml (fixedXml.get());
            fixedColor.run (24.0);
            auto measureNetGain = [&] (const Signal& material, double seconds)
            {
                resumedRealtime.source = fixedColor.source = material;
                resumedRealtime.sourceIndex = fixedColor.sourceIndex = 0;
                LoudnessMeter inputMeter, adaptiveMeter, fixedMeter;
                for (auto* meter : { &inputMeter, &adaptiveMeter, &fixedMeter })
                    meter->prepare ({ defaultRate, (juce::uint32) blockSize, (juce::uint32) numChannels });
                juce::AudioBuffer<float> input (numChannels, blockSize);
                const auto blocks = resumedRealtime.blocksFor (seconds);
                for (int b = 0; b < blocks; ++b)
                {
                    if (b >= blocks - resumedRealtime.blocksFor (3.2))
                    {
                        for (int ch = 0; ch < numChannels; ++ch)
                            for (int sample = 0; sample < blockSize; ++sample)
                                input.setSample (ch, sample, material[(size_t) ch]
                                    [(resumedRealtime.sourceIndex + (size_t) sample) % material[(size_t) ch].size()]);
                        inputMeter.process (input);
                    }
                    resumedRealtime.step(); fixedColor.step();
                    if (b >= blocks - resumedRealtime.blocksFor (3.2))
                    {
                        adaptiveMeter.process (resumedRealtime.buffer);
                        fixedMeter.process (fixedColor.buffer);
                    }
                }
                return std::array<float, 2> { adaptiveMeter.getIntegratedLufs() - inputMeter.getIntegratedLufs(),
                                              fixedMeter.getIntegratedLufs() - inputMeter.getIntegratedLufs() };
            };
            const auto denseNet = measureNetGain (dense, 4.0);
            const auto recoveredNet = measureNetGain (sparse, 20.0);
            logMessage ("  adaptive/fixed color net gain: dense " + juce::String (denseNet[0], 4) + "/" + juce::String (denseNet[1], 4)
                        + " dB, recovered sparse " + juce::String (recoveredNet[0], 4) + "/" + juce::String (recoveredNet[1], 4)
                        + " dB; target " + juce::String (calibrationState.plan.effectiveGainDb, 4)
                        + " dB; applied bias " + juce::String (resumedRealtime.probe().executedBiasDb, 4)
                        + "/" + juce::String (fixedColor.probe().executedBiasDb, 4) + " dB");
            // Fixture deliberately allocates 25 automatic Glue: 35% removal changes its 6 dB/full makeup by at most .525 dB.
            expect (std::abs (recoveredNet[0] - recoveredNet[1]) < 0.75f,
                    "calibration learned during color reduction does not cause more than 0.75 dB additional error after recovery; measured "
                    + juce::String (recoveredNet[0] - recoveredNet[1], 5));

            realtime.source = makeSilence (numChannels, (int) defaultRate);
            realtime.sourceIndex = 0;
            realtime.run (1.0);
            const auto silent = realtime.chain.getEffectiveParams();
            realtime.run (2.0);
            expectEquals (realtime.chain.getEffectiveParams().color.heat, silent.color.heat, "silence freezes Heat");
            expectEquals (realtime.chain.getEffectiveParams().color.glue, silent.color.glue, "silence freezes Glue");
            realtime.source = sparse;
            realtime.sourceIndex = 0;
            realtime.run (18.0);
            const auto recovered = realtime.chain.getEffectiveParams();
            expect (recovered.color.heat > reduced.color.heat + 1.0f && recovered.color.glue > reduced.color.glue + 0.5f,
                    "sparse/darker material restores planned color slowly; Heat " + juce::String (recovered.color.heat, 3)
                    + ", Glue " + juce::String (recovered.color.glue, 3));
            expect (recovered.color.heat <= original.color.heat && recovered.color.glue <= original.color.glue,
                    "recovery never exceeds the planned automatic amount");
            realtime.params.glazePercent = 0.0f;
            realtime.step();
            expect (sameParams (realtime.chain.getEffectiveParams(), realtime.params), "GLAZE 0 remains exactly manual");

            // Untrusted learning and bypass cannot request a new section-dependent change.
            state.profile.plrDb.confidence = 0.1f;
            execution.profile = state.profile;
            auto lowConfidenceXml = glaze::toXml (state);
            lowConfidenceXml->addChildElement (glaze::toXml (execution).release());
            Rig lowConfidence (constants, dense, params);
            lowConfidence.chain.restoreGlazeStateXml (lowConfidenceXml.get());
            lowConfidence.run (5.0);
            expectEquals (lowConfidence.probe().liveHeatReduction, 0.0f, "low PLR confidence cannot drive Heat");
            expectEquals (lowConfidence.probe().liveGlueReduction, 0.0f, "low PLR confidence cannot drive Glue");
            restored.hostBypassed = true;
            const auto heldHeat = restored.probe().liveHeatReduction;
            const auto heldGlue = restored.probe().liveGlueReduction;
            restored.run (4.0);
            expectEquals (restored.probe().liveHeatReduction, heldHeat, "host bypass freezes section Heat");
            expectEquals (restored.probe().liveGlueReduction, heldGlue, "host bypass freezes section Glue");
        }

        void testLiveToneAfterProfileLocks()
        {
            beginTest ("locked style follows broad tonal changes slowly, freezes in silence, and preserves legacy Tone");
            const auto bright = makeNoise (numChannels, (int) (8.0 * defaultRate), 0.08f, 147);
            auto dark = bright;
            // One-pole low pass changes the broad slope without creating an isolated spectral peak.
            const auto retain = std::exp (-juce::MathConstants<double>::twoPi * 250.0 / defaultRate);
            double sourcePower = 0.0, darkPower = 0.0;
            for (size_t ch = 0; ch < dark.size(); ++ch)
            {
                double z = 0.0;
                for (size_t i = 0; i < dark[ch].size(); ++i)
                {
                    z = bright[ch][i] + (z - bright[ch][i]) * retain;
                    dark[ch][i] = (float) z;
                    sourcePower += (double) bright[ch][i] * bright[ch][i];
                    darkPower += z * z;
                }
            }
            scale (dark, (float) std::sqrt (sourcePower / darkPower));
            Rig learned (fastConstants(), bright);
            learned.run (4.0);
            auto state = learned.committed();
            expect (state.hasProfile);
            state.profile.spectralSlopeDbPerOct.confidence = 1.0f;
            state.profile.bandConfidence.fill (1.0f);
            state.profile.representativeness = 1.0f;
            state.profile.integratedLufs.confidence = state.profile.truePeakDbtp.confidence = 1.0f;
            state.profileId = glaze::computeProfileId (state.profile);
            state.plan = glaze::Plan {};
            state.calibration = glaze::Calibration {};
            const auto saved = glaze::toXml (state);
            float realtimeTone = 0.0f;
            std::vector<float> realtimeToneTrajectory;
            for (const auto offline : { false, true })
            {
                Rig rig (fastConstants(), bright, defaultParams (50.0f, 3));
                rig.host.offline = offline;
                rig.chain.restoreGlazeStateXml (saved.get());
                rig.run (4.0);
                const auto baseline = rig.chain.getEffectiveParams().color.toneDb;
                const auto plan = rig.probe().targetPlan;
                expect (baseline < -0.1f, "bright learned material receives a negative Tone correction");
                rig.source = dark;
                rig.sourceIndex = 0;
                rig.run (0.1);
                expect (std::abs (rig.chain.getEffectiveParams().color.toneDb - baseline) < 0.02f, "a short event cannot swing the tone");
                auto previous = rig.chain.getEffectiveParams().color.toneDb;
                bool bounded = true, smooth = true;
                std::uint64_t allocations = 0;
                for (int b = 0; b < rig.blocksFor (14.0); ++b)
                {
                    // The second pass switches only Listen while following the same samples: the Tone path must remain identical.
                    rig.params.limit.listen = offline && (b / rig.blocksFor (0.5)) % 2 == 0;
                    const auto before = allocationCountOnThisThread();
                    rig.step();
                    allocations += allocationCountOnThisThread() - before;
                    const auto tone = rig.chain.getEffectiveParams().color.toneDb;
                    bounded = bounded && std::abs (tone) <= state.constants.clean.toneMaxDb * 0.5f + 1.0e-6f;
                    smooth = smooth && std::abs (tone - previous) <= 0.011f;
                    previous = tone;
                    if (offline)
                        expectEquals (tone, realtimeToneTrajectory[(size_t) b], "Listen does not pause or alter tonal adaptation");
                    else
                        realtimeToneTrajectory.push_back (tone);
                }
                expect (previous > 0.1f, "sustained dark section changes the direction without relearning");
                expect (bounded && smooth);
                expectEquals ((int) allocations, 0);
                expect (glaze::bitwiseEqual (rig.committed().profile, state.profile));
                expectEquals ((juce::int64) rig.committed().profileId, (juce::int64) state.profileId);
                expectEquals (rig.probe().targetPlan.weights.clean, plan.weights.clean);
                expectEquals (rig.probe().targetPlan.effectiveTargetLufs, plan.effectiveTargetLufs);
                if (offline)
                    expectWithinAbsoluteError (previous, realtimeTone, 1.0e-5f);
                else
                    realtimeTone = previous;

                rig.params.limit.listen = true;
                rig.hostBypassed = true;
                rig.source = bright;
                rig.sourceIndex = 0;
                rig.step();
                const auto bypassedTone = rig.chain.getEffectiveParams().color.toneDb;
                rig.run (2.0);
                expectEquals (rig.chain.getEffectiveParams().color.toneDb, bypassedTone, "host bypass plus Listen freezes live Tone");
                rig.hostBypassed = false;

                rig.source = makeSilence (numChannels, (int) defaultRate);
                rig.sourceIndex = 0;
                rig.run (1.0);
                const auto silentTone = rig.chain.getEffectiveParams().color.toneDb;
                rig.run (3.0);
                expectEquals (rig.chain.getEffectiveParams().color.toneDb, silentTone, "silence freezes tonal correction");
                const auto heardBeforeReset = rig.chain.getEffectiveParams().color.toneDb;
                rig.chain.reset();
                rig.step();
                expectWithinAbsoluteError (rig.chain.getEffectiveParams().color.toneDb, heardBeforeReset, 1.0e-6f,
                                           "host reset preserves the last heard Tone while resetting spectral windows");
                rig.params.glazePercent = 0.0f;
                rig.step();
                expect (sameParams (rig.chain.getEffectiveParams(), rig.params));
                rig.params.glazePercent = 50.0f;
                rig.params.color.bypass = true;
                rig.step();
                expectEquals (rig.chain.getEffectiveParams().color.toneDb, rig.params.color.toneDb);
            }

            // Same broad spectrum plus a stationary narrow peak must not create a mismatch between the two regression methods.
            auto peaked = bright;
            const auto sine = makeSine (numChannels, defaultRate, 2000.0, 0.12f, 8.0);
            for (size_t ch = 0; ch < peaked.size(); ++ch)
                for (size_t i = 0; i < peaked[ch].size(); ++i)
                    peaked[ch][i] += sine[ch][i];
            Rig peakLearner (fastConstants(), peaked);
            peakLearner.run (4.0);
            auto peakState = peakLearner.committed();
            peakState.profile.spectralSlopeDbPerOct.confidence = 1.0f;
            peakState.profile.bandConfidence.fill (1.0f);
            peakState.profileId = glaze::computeProfileId (peakState.profile);
            peakState.plan = glaze::Plan {};
            // Neutral reference leaves room in both directions, so cap saturation cannot hide drift.
            peakState.constants.clean.toneReferenceSlopeDbPerOct = peakState.profile.spectralSlopeDbPerOct.value;
            Rig steady (fastConstants(), peaked, defaultParams (50.0f, 3));
            steady.chain.restoreGlazeStateXml (glaze::toXml (peakState).get());
            steady.run (12.0);
            expectWithinAbsoluteError (steady.chain.getEffectiveParams().color.toneDb, steady.probe().increments.colorToneDb, 0.015f,
                                       "identical spectrum with a narrow peak does not cause false broad-band drift");

            state.constants.algorithmVersion = glaze::currentAlgorithmVersion;
            state.plan.algorithmVersion = 3;
            state.plan.increments.colorToneDb = 0.23f;
            state.plan.increments.balanceTame = 7.0f;
            Rig legacy (fastConstants(), dark, defaultParams (50.0f, 3));
            legacy.chain.restoreGlazeStateXml (glaze::toXml (state).get());
            legacy.run (10.0);
            expect (legacy.probe().savedPlanMode);
            expectEquals (legacy.chain.getEffectiveParams().color.toneDb, 0.23f);
            expectEquals (legacy.chain.getEffectiveParams().balance.automaticTame, 0.0f);
        }

        void testLiveTameAfterProfileLocks()
        {
            beginTest ("locked profile: equal-loudness spectral change increases live Tame within two seconds, with identical realtime/offline authority");
            const auto flat = makeNoise (numChannels, (int) (8.0 * defaultRate), 0.04f, 903);
            auto peaked = flat;
            const auto tone = makeSine (numChannels, defaultRate, 2000.0, 0.16f, 8.0);
            for (size_t ch = 0; ch < peaked.size(); ++ch)
                for (size_t i = 0; i < peaked[ch].size(); ++i)
                    peaked[ch][i] += tone[ch][i];

            const auto measuredLufs = [] (const Signal& input)
            {
                LoudnessMeter meter;
                meter.prepare ({ defaultRate, (juce::uint32) blockSize, (juce::uint32) numChannels });
                juce::AudioBuffer<float> buffer (numChannels, blockSize);
                for (int start = 0; start + blockSize <= (int) input.front().size(); start += blockSize)
                {
                    for (int ch = 0; ch < numChannels; ++ch)
                        buffer.copyFrom (ch, 0, input[(size_t) ch].data() + start, blockSize);
                    meter.process (buffer);
                }
                return meter.getIntegratedLufs();
            };
            const auto flatLufs = measuredLufs (flat);
            scale (peaked, juce::Decibels::decibelsToGain (flatLufs - measuredLufs (peaked)));
            expectWithinAbsoluteError (measuredLufs (peaked), flatLufs, 0.01f);

            Rig learned (fastConstants(), flat);
            learned.run (4.0);
            auto state = learned.committed();
            expect (state.hasProfile);
            state.profile.tameEvidenceDb.fill (0.0f);
            state.profileId = glaze::computeProfileId (state.profile);
            state.calibration = glaze::Calibration {};
            state.plan = glaze::Plan {};
            const auto saved = glaze::toXml (state);

            float realtimePeakAuthority = 0.0f;
            for (const auto offline : { false, true })
            {
                Rig rig (fastConstants(), flat, defaultParams (50.0f, 3));
                rig.host.offline = offline;
                rig.chain.restoreGlazeStateXml (saved.get());
                rig.run (3.0);
                const auto baseline = rig.chain.getEffectiveParams().balance.tame;
                const auto before = rig.probe();
                rig.source = peaked;
                rig.sourceIndex = 0;
                auto highest = baseline;
                std::uint64_t allocations = 0;
                for (int b = 0; b < rig.blocksFor (2.0); ++b)
                {
                    const auto allocationStart = allocationCountOnThisThread();
                    rig.step();
                    allocations += allocationCountOnThisThread() - allocationStart;
                    highest = juce::jmax (highest, rig.chain.getEffectiveParams().balance.tame);
                }
                logMessage (juce::String (offline ? "  offline" : "  realtime") + ": baseline Tame " + juce::String (baseline, 3) + ", highest "
                            + juce::String (highest, 3) + " (margin over +2.0: " + juce::String (highest - baseline - 2.0f, 3) + "), profile kappa "
                            + juce::String (glaze::confidenceScale (&state.profile, state.constants), 3));
                expect (highest > baseline + 2.0f, "current resonance gains authority despite no stored evidence");
                expect (highest <= 0.5f * state.constants.clean.tameCap + 1.0e-4f, "current style and macro still cap live authority");
                expectEquals (rig.chain.getEffectiveParams().balance.automaticTame,
                              rig.chain.getEffectiveParams().balance.tame - rig.params.balance.tame);
                expectEquals ((int) allocations, 0);
                expectEquals ((juce::int64) rig.committed().profileId, (juce::int64) state.profileId);
                expect (glaze::bitwiseEqual (rig.committed().profile, state.profile));
                expectEquals (rig.probe().targetPlan.effectiveTargetLufs, before.targetPlan.effectiveTargetLufs);
                expectEquals (rig.probe().learningStarts, before.learningStarts);
                if (offline)
                    expectWithinAbsoluteError (highest, realtimePeakAuthority, 1.0e-4f);
                else
                    realtimePeakAuthority = highest;

                rig.source = flat;
                rig.sourceIndex = 0;
                rig.run (8.0);
                expect (rig.chain.getEffectiveParams().balance.tame < highest - 2.0f, "flat material releases live authority");
                rig.source = makeSilence (numChannels, blockSize);
                rig.run (4.0);
                expectWithinAbsoluteError (rig.chain.getEffectiveParams().balance.tame, rig.probe().increments.balanceTame, 0.1f);

                rig.source = peaked;
                rig.params.glazePercent = 0.0f;
                rig.run (2.0);
                expect (sameParams (rig.chain.getEffectiveParams(), rig.params), "GLAZE zero is still exactly manual");
                rig.params.glazePercent = 50.0f;
                rig.params.balance.bypass = true;
                rig.run (2.0);
                expectEquals (rig.chain.getEffectiveParams().balance.tame, rig.params.balance.tame, "Balance bypass gets no live authority");
            }

            auto legacyPlan = state;
            legacyPlan.plan.algorithmVersion = glaze::oldestCompatibleAlgorithmVersion - 1;   // 比兼容下限还旧：照用保存的增量
            legacyPlan.plan.increments.balanceTame = 7.0f;
            Rig legacy (fastConstants(), peaked, defaultParams (50.0f, 3));
            legacy.chain.restoreGlazeStateXml (glaze::toXml (legacyPlan).get());
            legacy.run (3.0);
            expect (legacy.probe().savedPlanMode);
            expectEquals (legacy.chain.getEffectiveParams().balance.tame, 7.0f, "saved-plan compatibility excludes live Tame");
        }

        // 12. 约束反馈闭环：学完之后素材推热 ~10 dB（方案按学到的画像给的增益远超这段素材的约束），保护层退到底「方案受限」、
        //     或上游约束 / 校准预算触发 → 下调约束反馈上限重做方案。断言：上限只降不升、步数有限、最后一段时间不再动（收敛）、
        //     保护层最后不在退让与恢复之间来回拉锯、净增益不低于手动链、全程输出真峰值 ≤ Ceiling + 0.01 dB。
        // 依据：决策第 4 节「超限就降低请求增益、重新求满足全部约束的方案」；PHASE2 设计决定 1（每次 −1 dB，隔一个过渡 + 3 s 观察，
        //     下限 0 dB 净提升；预算受限同样处理）。
        void testConstraintFeedbackConverges()
        {
            beginTest ("constraint feedback lowers the plan step by step, converges and does not oscillate");

            // 冲击：限幅上限最紧（峰值 3 dB、p95 2 dB、均值 0.8 dB）。
            Rig rig (fastConstants(), makeMusic (defaultRate, 8.0, 24, 0.2f), defaultParams (100.0f, 2));
            rig.run (6.0);
            expect (rig.committed().hasProfile, "learned on the quiet material");

            rig.sourceGain = 5.0f;       // ≈ +14 dB

            std::vector<float> maxGainHistory;
            std::vector<float> guardHistory;
            float peak = 0.0f;
            const auto ceilingGain = juce::Decibels::decibelsToGain (-1.0f + 0.01f);
            TruePeakDetector detector;
            detector.prepare (numChannels);

            rig.run (90.0, [&] (int)
            {
                maxGainHistory.push_back (rig.probe().feedbackMaxGainDb);
                guardHistory.push_back (rig.probe().guardDb);

                for (int ch = 0; ch < numChannels; ++ch)
                    peak = juce::jmax (peak, detector.processBlockMax (ch, rig.buffer.getReadPointer (ch), rig.buffer.getNumSamples()));
            });

            const auto probe = rig.probe();
            bool nonIncreasing = true;
            int changes = 0, lastChange = 0;

            for (size_t i = 1; i < maxGainHistory.size(); ++i)
            {
                nonIncreasing = nonIncreasing && maxGainHistory[i] <= maxGainHistory[i - 1];

                if (maxGainHistory[i] != maxGainHistory[i - 1])
                {
                    ++changes;
                    lastChange = (int) i;
                }
            }

            // 最后 20 s：上限不再动，保护层的方向变化（退让 ↔ 恢复）不超过一次。
            const auto tailBlocks = rig.blocksFor (20.0);
            int directionChanges = 0, lastDirection = 0;

            for (size_t i = guardHistory.size() - (size_t) tailBlocks; i < guardHistory.size(); ++i)
            {
                const auto d = guardHistory[i] > guardHistory[i - 1] ? 1 : (guardHistory[i] < guardHistory[i - 1] ? -1 : 0);

                if (d != 0)
                {
                    if (lastDirection != 0 && d != lastDirection)
                        ++directionChanges;

                    lastDirection = d;
                }
            }

            logMessage ("  feedback steps " + juce::String (probe.feedbackSteps) + " (budget " + juce::String (probe.feedbackFromBudget)
                        + ", guard limited " + juce::String (probe.feedbackFromGuardLimit) + ", upstream " + juce::String (probe.feedbackFromUpstream)
                        + ", stuck guard " + juce::String (probe.feedbackFromStuckGuard) + "); max gain now " + juce::String (probe.feedbackMaxGainDb, 2) + " dB; last change at "
                        + juce::String ((double) lastChange * blockSize / defaultRate, 1) + " s; guard " + juce::String (probe.guardDb, 2)
                        + " dB; tail direction changes " + juce::String (directionChanges) + "; output true peak "
                        + juce::String (toDb (peak), 3) + " dBTP");

            expect (probe.feedbackSteps >= 1, "the loop must act on material this hot");
            expect (nonIncreasing, "the gain ceiling only goes down");
            expect (changes <= 12, "a bounded number of steps");
            expect ((int) maxGainHistory.size() - lastChange >= tailBlocks, "converged: no change in the last 20 s");
            expect (directionChanges <= 1, "the guard does not keep swinging between retreat and recovery");
            expect (peak <= ceilingGain, "output true peak stays within the ceiling throughout");

            const auto floorPlan = planFor (rig.committed(), glaze::Style::punchy, 0.0f, rig.params, 1.0e9f);
            expect (probe.feedbackMaxGainDb >= floorPlan.effectiveGainDb - 1.0e-3f, "never below the manual chain's own net gain");
        }

        // 12b. 锁定后保护层卡在部分退让上：已提交的校准对此刻适用，不会再开校准轮，也就走不到「预算受限 → 吸收」；
        //      按规格「不无限等待」同样吸收进方案（主进程阶段 3 定，PHASE2 遗留 1）：上限 = 此刻实际净提升，保护层清零、
        //      原退让随方案过渡淡出。结束时不应再有「锁定、校准适用、却一直带着退让」的状态。
        // 依据：决策第 7 节「回到 0 表示恢复参考方案」、第 6 节「不无限等待」；PHASE2 设计决定 1。
        void testStuckGuardIsAbsorbed()
        {
            beginTest ("locked with an applicable calibration: a guard stuck on a partial retreat is absorbed into the plan");

            // ×5（约 +14 dB）：限幅量超出冲击的上限，保护层退到约 −0.9 dB 后停在回差区；×2（约 +6 dB）：Clip 与限幅器还放得下，
            // 保护层不动——对照组，方案不许被降。
            // 画像失配关掉（门槛无限大）：+14 dB 会先触发它（按这一段直接压低上限），这里要测的是保护层卡住后的吸收路径。
            auto constants = withTightLimitBudgets (fastConstants());   // 保护层要出手（TestHelpers.h 的说明）
            constants.mismatchThresholdLu = constants.mismatchReleaseLu = 1.0e9f;

            for (const auto gain : { 2.0f, 5.0f })
            {
                Rig rig (constants, makeMusic (defaultRate, 8.0, 24, 0.2f), defaultParams (100.0f, 2));
                expect (rig.learnAndCalibrate(), "locks with a valid calibration on the quiet material");

                rig.sourceGain = gain;
                auto lowestGuard = 0.0f;
                auto peak = 0.0f;
                TruePeakDetector detector;
                detector.prepare (numChannels);

                rig.run (60.0, [&] (int)
                {
                    lowestGuard = juce::jmin (lowestGuard, rig.probe().guardDb);

                    for (int ch = 0; ch < numChannels; ++ch)
                        peak = juce::jmax (peak, detector.processBlockMax (ch, rig.buffer.getReadPointer (ch), rig.buffer.getNumSamples()));
                });

                const auto probe = rig.probe();
                const auto s = rig.status();
                const auto tag = "source x" + juce::String (gain, 1);
                logMessage ("  " + tag + ": feedback steps " + juce::String (probe.feedbackSteps)
                            + " (stuck " + juce::String (probe.feedbackFromStuckGuard) + ", budget " + juce::String (probe.feedbackFromBudget)
                            + ", guard limited " + juce::String (probe.feedbackFromGuardLimit) + ", upstream " + juce::String (probe.feedbackFromUpstream)
                            + "); lowest guard " + juce::String (lowestGuard, 2) + " dB; now guard " + juce::String (probe.guardDb, 2)
                            + " / executed " + juce::String (probe.executedGuardDb, 2) + " dB; output true peak " + juce::String (toDb (peak), 3) + " dBTP");

                if (gain < 3.0f)
                {
                    expectEquals (probe.feedbackSteps, 0, tag + ": within the limits the plan is left alone");
                    expectEquals (lowestGuard, 0.0f, tag + ": the guard never had to act");
                }
                else
                {
                    expect (lowestGuard < -0.1f, tag + ": the guard did retreat");
                    expect (probe.feedbackFromStuckGuard >= 1, tag + ": the stuck retreat was absorbed into the plan");
                }

                expect (rig.phase() == Phase::locked && s.calibrationValid, tag + ": ends locked with a valid calibration");
                expect (! (probe.executedGuardDb < -0.01f), tag + ": no retreat left hanging under a locked, calibrated plan");
                expect (peak <= juce::Decibels::decibelsToGain (-1.0f + 0.01f), tag + ": output true peak within the ceiling");
            }
        }

        // 12d. BUG-003（T-015）：吸收型约束反馈（校准结束为「预算受限」、锁定后保护层卡住；step = 0）只把保护层已执行的退让并进方案，
        //      工作点不变，校准结论（往上被保护层挡住）对新方案照样成立——已提交的校准改绑到新的参考条件、继续适用，不再开新一轮。
        //      修前：上限在参考条件里，吸收一次校准就失效、过渡后从偏差 0 重开一轮，又测到同样的欠量、又往上推回保护层，
        //      「预算受限 → 反馈 → 重校」一圈约 30 s，真歌 30 ～ 60 s 的整首都停在「校准中」（门槛 88 组里 27 组，含 193 s 的歌）。
        //      断言：两条吸收路径都真的走到；从吸收那一刻起、直到下一次非吸收型反馈（方案受限 / 上游超限 / 画像失配，工作点真的变了，
        //      重测合理）之前，校准轮一次都不开，阶段不回到「校准中」；最后「已锁定 + 校准有效」。
        // 依据：时间线第 2 节「已锁定 | 校准结束（或预算受限、或保守未完全校准）| 方案固定 | 稳定」；决策第 6 节「有结束条件，不做持续闭环」；
        //      实现修订第 3 节「预算受限和卡住的退让不额外减 1 dB，原退让随过渡淡出，净增益连续」。
        void testAbsorbingFeedbackKeepsCalibration()
        {
            beginTest ("absorbing feedback (budget limited / stuck guard) keeps the calibration and does not restart it (BUG-003)");

            // 素材推热 ×5（约 +14 dB，同 12b）：限幅量超出冲击风格的上限，保护层退让后停在回差区。
            // 「预算受限」一例：一轮开始就推热，校准窗放长到 6 s（预算同长），保证保护层在轮中出手、预算用完；
            // 「卡住」一例：先锁定再推热（同 12b）。
            struct Case { const char* name; bool raiseDuringRound; double windowSeconds; };

            for (const auto& c : { Case { "budget limited during a round", true, 6.0 },
                                   Case { "stuck guard while locked", false, 2.0 } })
            {
                auto constants = withTightLimitBudgets (fastConstants());   // 保护层要出手（TestHelpers.h 的说明）
                constants.mismatchThresholdLu = constants.mismatchReleaseLu = 1.0e9f;   // 要测的是保护层路径，不让画像失配先动手
                constants.calibrationWindowSeconds = c.windowSeconds;

                Rig rig (constants, makeMusic (defaultRate, 8.0, 24, 0.2f), defaultParams (100.0f, 2));

                if (c.raiseDuringRound)
                    expect (rig.runUntil (40.0, [&rig] { return rig.probe().calibrationBegins >= 1; }), juce::String (c.name) + ": a round began");
                else
                    expect (rig.learnAndCalibrate(), juce::String (c.name) + ": locks with a valid calibration on the quiet material");

                rig.sourceGain = 5.0f;

                auto last = rig.probe();
                bool afterAbsorb = false;
                int absorbing = 0, restartsAfterAbsorb = 0, calibratingBlocksAfterAbsorb = 0;

                rig.run (90.0, [&] (int)
                {
                    const auto p = rig.probe();
                    const auto absorbedNow = (p.feedbackFromBudget - last.feedbackFromBudget) + (p.feedbackFromStuckGuard - last.feedbackFromStuckGuard);
                    const auto loweredNow = (p.feedbackFromGuardLimit - last.feedbackFromGuardLimit)
                                          + (p.feedbackFromUpstream - last.feedbackFromUpstream) + (p.mismatchSteps - last.mismatchSteps);

                    if (loweredNow > 0)
                        afterAbsorb = false;   // 工作点真的变了：之后重测是合理的

                    if (absorbedNow > 0)
                    {
                        afterAbsorb = true;
                        absorbing += absorbedNow;
                    }
                    else if (afterAbsorb)
                    {
                        restartsAfterAbsorb += p.calibrationBegins - last.calibrationBegins;

                        if (rig.phase() == Phase::calibrating)
                            ++calibratingBlocksAfterAbsorb;
                    }

                    last = p;
                });

                const auto s = rig.status();
                const auto p = rig.probe();
                logMessage ("  " + juce::String (c.name) + ": absorbing feedback " + juce::String (absorbing) + " (budget " + juce::String (p.feedbackFromBudget)
                            + ", stuck " + juce::String (p.feedbackFromStuckGuard) + "), lowering " + juce::String (p.feedbackFromGuardLimit + p.feedbackFromUpstream)
                            + ", rounds " + juce::String (p.calibrationBegins) + ", restarts after absorbing " + juce::String (restartsAfterAbsorb)
                            + ", calibrating after absorbing " + juce::String ((double) calibratingBlocksAfterAbsorb * blockSize / defaultRate, 2) + " s; ends "
                            + phaseName (rig.phase()) + (s.calibrationValid ? " / valid" : " / not valid"));

                expect (absorbing >= 1, juce::String (c.name) + ": the absorbing path was exercised");
                expectEquals (restartsAfterAbsorb, 0, juce::String (c.name) + ": no calibration round restarts after an absorbing feedback");
                expectEquals (calibratingBlocksAfterAbsorb, 0, juce::String (c.name) + ": the phase does not go back to calibrating");
                expect (rig.phase() == Phase::locked && s.calibrationValid, juce::String (c.name) + ": ends locked with a valid calibration");
            }
        }

        // 12c. 画像失配：首次聆听只听到安静前奏，画像把整首歌当成很轻；响段进来后编排器按「这一段当画像」直接压低上限
        //      （短过渡），不必等保护层 1 dB/s 退满、约束反馈每 6 s 才降 1 dB；状态行提示「建议重新聆听」，不自动重学。
        //      对照组：同样的响段、画像本来就是它，不触发。
        // 依据：v0.6 验收 F-01（18 s 前奏 −26 LUFS + 响段 −10 LUFS：原来限幅 GR 峰 7.4 dB、80 s 后仍 4 ～ 6 dB）；
        //      设计规格-GLAZE-实现修订 第 3 节；时间线第 2 节「不自动重学」。
        void testProfileMismatch()
        {
            beginTest ("a profile learned on a quiet intro is capped by the loud section at once, with a relearn hint");

            // 前奏 20 s（很轻）接响段 20 s；工厂常数：首次聆听满 15 s 就在前奏里提交。
            const auto intro = makeMusic (defaultRate, 20.0, 31, 0.03f);
            const auto loud = makeMusic (defaultRate, 20.0, 32, 0.9f);
            Signal song (numChannels);

            for (int ch = 0; ch < numChannels; ++ch)
            {
                song[(size_t) ch] = intro[(size_t) ch];
                song[(size_t) ch].insert (song[(size_t) ch].end(), loud[(size_t) ch].begin(), loud[(size_t) ch].end());
            }

            auto constants = glaze::factoryConstants();
            Rig rig (constants, std::move (song), defaultParams (50.0f, 0));
            rig.run (19.5);

            const auto learnedLufs = rig.committed().profile.integratedLufs.value;
            expect (rig.committed().hasProfile, "the first listen finished inside the intro");
            expect (! rig.status().profileMismatch);

            // 响段：记每 100 ms 的限幅 GR 峰值（链的 GR 表读完清零）。
            (void) rig.chain.getAndResetLimitGainReductionDb();
            std::vector<float> grPerTenth;
            int blocksPerTenth = juce::jmax (1, (int) (0.1 * defaultRate / blockSize)), counter = 0;
            auto grWindow = 0.0f;
            double mismatchAt = -1.0;

            rig.run (15.0, [&] (int block)
            {
                grWindow = juce::jmax (grWindow, rig.chain.getAndResetLimitGainReductionDb());

                if (mismatchAt < 0.0 && rig.probe().mismatchSteps > 0)
                    mismatchAt = (double) (block + 1) * blockSize / defaultRate;

                if (++counter == blocksPerTenth)
                {
                    grPerTenth.push_back (grWindow);
                    grWindow = 0.0f;
                    counter = 0;
                }
            });

            const auto probe = rig.probe();
            const auto status = rig.status();
            auto lateGr = 0.0f;   // 响段 5 s 之后的 GR 峰值

            for (size_t i = 50; i < grPerTenth.size(); ++i)
                lateGr = juce::jmax (lateGr, grPerTenth[i]);

            logMessage ("  learned I " + juce::String (learnedLufs, 1) + " LUFS; mismatch steps " + juce::String (probe.mismatchSteps)
                        + " (first at +" + juce::String (mismatchAt, 2) + " s into the loud part); feedback cap " + juce::String (probe.feedbackMaxGainDb, 2)
                        + " dB; limiter GR peak after 5 s " + juce::String (lateGr, 2) + " dB; status mismatch " + juce::String ((int) status.profileMismatch));

            expect (probe.mismatchSteps >= 1, "the loud section is recognised as not matching the profile");
            expect (mismatchAt > 0.0 && mismatchAt <= 2.5, "recognised within 2.5 s of the loud section");
            expect (status.profileMismatch, "the status line asks for a relearn");
            expect (rig.committed().profile.integratedLufs.value == learnedLufs, "no automatic relearn: the committed profile is kept");
            expect (lateGr <= 4.0f, "after the cap the limiter works within its usual range (was 4 to 7 dB for over a minute)");

            // 对照：画像本来就是响段（直接学响段），同样的音乐不触发。
            Rig control (glaze::factoryConstants(), makeMusic (defaultRate, 20.0, 32, 0.9f), defaultParams (50.0f, 0));
            control.run (35.0);
            expectEquals (control.probe().mismatchSteps, 0, "a profile that matches the music never triggers");
            expect (! control.status().profileMismatch);
        }

        // 12d. 宿主时间线不规整：FL Studio 的 Pattern 模式（几秒一循环、不报循环信息，每次回到开头都像跳播）、播放头小抖动、
        //      每几块报一次旧位置——聆听照样完成，校准照样结束并锁定。停止播放时状态行写「校准暂停（未播放）」（见状态行测试）。
        // 依据：v0.6 验收后 Max 报告「一直显示在校准，一直校准不好」；诊断：4 s 一循环时 150 s 仍在校准（跳播作废子窗再等 settle），
        //      按 1 ms 判跳播时抖动的播放头上聆听永远凑不满。设计规格-GLAZE-实现修订 第 7 节。
        void testIrregularHostTimeline()
        {
            beginTest ("pattern loops without loop info, a jittery playhead and stale positions still learn, calibrate and lock");

            const char* names[] = { "pattern loop 4 s", "jitter +-64 samples", "stale position every 4th block" };

            for (int mode = 0; mode < 3; ++mode)
            {
                Rig rig (glaze::factoryConstants(), makeMusic (defaultRate, 8.0, 41, 0.5f), defaultParams (60.0f, 0));
                rig.advancePosition = false;

                const auto loop = (std::int64_t) (4.0 * defaultRate);
                const auto sourceLength = (std::int64_t) rig.source.front().size();
                std::int64_t timeline = 0, counter = 0;

                const auto locked = rig.runUntil (150.0, [&]
                {
                    timeline += rig.blockLength;
                    ++counter;

                    if (mode == 0)
                    {
                        timeline %= loop;                                   // 位置与音频一起回到 pattern 开头
                        rig.position = timeline;
                        rig.sourceIndex = (size_t) (timeline % sourceLength);
                    }
                    else if (mode == 1)
                    {
                        rig.position = timeline + (counter * 37) % 129 - 64;
                    }
                    else
                    {
                        rig.position = counter % 4 == 3 ? timeline - rig.blockLength : timeline;
                    }

                    const auto s = rig.status();
                    return rig.phase() == Phase::locked && s.calibrationValid;
                });

                const auto probe = rig.probe();
                logMessage (juce::String ("  ") + names[mode] + ": locked " + juce::String ((int) locked) + ", jumps " + juce::String (probe.transportJumps)
                            + ", commits " + juce::String (probe.commits) + ", calibration rounds " + juce::String (probe.calibrationBegins));
                expect (locked, juce::String (names[mode]) + ": learns, calibrates and locks within 150 s");

                if (mode == 0)
                    expect (probe.transportJumps >= 3, "the pattern wraps are seen as jumps (and no longer void the calibration)");
                else
                    expectEquals (probe.transportJumps, 0, juce::String (names[mode]) + ": small playhead irregularities are not jumps");
            }
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
                expectEquals (rig.probe().calibrationBegins, 0, "nothing automatic to calibrate");
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
            expect (rig.phase() == Phase::unlearned && ! rig.status().calibrationValid,
                    "phase stays unlearned; the non-finite restart does not begin calibration");
        }

        // 15. 音频线程零分配：一条链走完聆听、提交、过渡、校准、换风格、约束反馈、跳播 / 回绕 / reset、离线切换、重新聆听请求、
        //     读取存档（解析在消息线程、计数区外）、GLAZE 回 0 再拧起，setHostInfo / setParams / process / getStatus 都不经 operator new。
        // 依据：设计规格-总体「音频线程零分配、零锁」；界面与实现第 4 节。
        void testAudioThreadDoesNotAllocate()
        {
            beginTest ("setHostInfo / setParams / process / getStatus never allocate through every GLAZE path");

            // 冲击风格、学完后推热 14 dB：保护层退让、校准预算受限、约束反馈都会走到。
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
                if (b == rig.blocksFor (8.0))  rig.sourceGain = 5.0f;           // 推热：保护层 / 约束反馈
                if (b == rig.blocksFor (20.0)) rig.params.glazeStyle = 1;        // 换风格
                if (b == rig.blocksFor (24.0)) rig.position += 96000;            // 跳播
                if (b == rig.blocksFor (25.0)) { rig.host.hasLoop = rig.host.looping = true; rig.position -= 24000; }
                if (b == rig.blocksFor (26.0)) rig.host.offline = true;
                if (b == rig.blocksFor (28.0)) rig.host.offline = false;
                if (b == rig.blocksFor (30.0)) rig.chain.requestGlazeRelearn();
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
            logMessage ("  paths exercised: learning starts " + juce::String (probe.learningStarts) + ", commits " + juce::String (probe.commits)
                        + ", plan transitions " + juce::String (probe.planTransitions) + ", calibration rounds " + juce::String (probe.calibrationBegins)
                        + ", feedback steps " + juce::String (probe.feedbackSteps) + ", resets " + juce::String (probe.hostResets)
                        + ", jumps " + juce::String (probe.transportJumps) + ", wraps " + juce::String (probe.loopWraps));

            expect (finite, "output finite throughout");
            expect (probe.commits == 0 && probe.calibrationBegins == 0 && probe.learningStarts == 0 && probe.feedbackSteps == 0,
                    "listen/lock/calibration/feedback counters stay at 0 on the continuous orchestrator");
            expect (probe.transportJumps >= 1 && probe.loopWraps >= 1 && probe.hostResets >= 1,
                    "the run still covers jumps, loop wraps and a host reset");
            expect (rig.phase() == Phase::unlearned, "phase stays unlearned through the path sweep");
            expectEquals ((juce::int64) allocations, (juce::int64) 0, "audio thread must not allocate");
        }

        //==============================================================================
        // ---- v0.1.7.0 修坯 Clay（A6）----

        // Clay 1. 三态（ADR-009）：GLAZE 0 时全「自动」= 中性（Clay 逐位直通），原有 7 个参数仍逐位等于手动值；「手动」在 GLAZE 0
        //    照样生效（严格等于旋钮值，收单比例 1，负的加宽 = 收窄也照用）；「关」永远中性；Bypass 原样透传；状态快照报的就是
        //    交给 ClayModule 的实际值。GLAZE 60 时「关」同样中性——阳性对照：同一画像、同一素材下「自动」给的是非中性值。
        // 依据：Clay 第 5 节三态表、第 4 节「逐位直通」；ChainParams.h「GLAZE 为 0 也要算」；ClayGuard.h resolveClayParams
        //    （手动 → 低切 = 旋钮值、收单 = 旋钮分频点 + 比例 1、加宽 = 旋钮值）。
        void testClayThreeStates()
        {
            beginTest ("Clay three states: automatic is neutral at GLAZE 0, manual applies at GLAZE 0, off is always neutral");

            auto initial = defaultParams (0.0f, 3);
            initial.balance.recover = 12.5f;
            initial.color.glue = 20.0f;
            initial.limit.gainDb = 2.0f;
            initial.claySettings.lowCutHz = 40.0f;   // 「自动」态下旋钮值不生效：拨到别处也必须是中性
            initial.claySettings.widthDb = -2.0f;
            Rig rig (clayConstants(), makeClayMaterial (false, false, 71), initial);

            bool neutral = true, bitExact = true;
            rig.run (4.0, [&] (int)
            {
                const auto& effective = rig.chain.getEffectiveParams();
                neutral = neutral && isNeutralClay (effective.clay) && ! effective.clay.bypass;
                bitExact = bitExact && sameParams (effective, rig.params);
            });
            expect (neutral, "GLAZE 0, all automatic: Clay gets neutral values on every block (bit-exact pass-through)");
            expect (bitExact, "GLAZE 0: the seven original parameters stay bit-exact manual values");
            expect (rig.committed().hasProfile, "learning still completes at GLAZE 0");
            expect (isNeutralClayPlan (rig.probe().liveClay) && ! rig.probe().clayGuard.phaseActive && ! rig.probe().clayGuard.widthActive,
                    "GLAZE 0: no automatic value and a neutral guard");
            expect (! rig.status().clayNeedsRelearn, "GLAZE 0 never asks for a relearn");

            // 「手动」：GLAZE 0 也严格按旋钮值生效（规格原文「严格等于旋钮值，GLAZE 为 0 时也生效」）。
            auto& settings = rig.params.claySettings;
            settings.lowCutMode = settings.monoMode = settings.widthMode = ClayMode::manual;
            settings.lowCutHz = 20.0f;
            settings.monoHz = 150.0f;
            settings.widthDb = 1.0f;
            bool manualApplied = true;
            bitExact = true;
            rig.run (0.5, [&] (int)
            {
                const auto& effective = rig.chain.getEffectiveParams();
                manualApplied = manualApplied && effective.clay.lowCutHz == 20.0f && effective.clay.monoHz == 150.0f
                             && effective.clay.monoAmount == 1.0f && effective.clay.widthDb == 1.0f && ! effective.clay.bypass;
                bitExact = bitExact && sameParams (effective, rig.params);
            });
            expect (manualApplied, "manual items apply exactly at GLAZE 0: " + describe (rig.chain.getEffectiveParams().clay));
            expect (bitExact, "manual Clay leaves the other seven parameters bit-exact");
            {
                const auto s = rig.status();
                expect (s.clayLowCutHz == 20.0f && s.clayMonoHz == 150.0f && s.clayMonoAmount == 1.0f && s.clayWidthDb == 1.0f,
                        "the status reports the values handed to ClayModule");
                expect (! s.clayPhaseGuardActive && ! s.clayWidthGuardActive && ! s.clayNeedsRelearn);
            }

            settings.widthDb = -2.5f;   // 收窄只能手动（Clay 第 4 节）：照样严格按旋钮值
            rig.step();
            expectEquals (rig.chain.getEffectiveParams().clay.widthDb, -2.5f, "manual narrowing applies as set");

            // 「关」：永远中性；Bypass 原样交给 ClayModule。
            settings.lowCutMode = settings.monoMode = settings.widthMode = ClayMode::off;
            settings.bypass = true;
            neutral = true;
            bool bypassPassed = true;
            rig.run (0.5, [&] (int)
            {
                const auto& clay = rig.chain.getEffectiveParams().clay;
                neutral = neutral && isNeutralClay (clay);
                bypassPassed = bypassPassed && clay.bypass;
            });
            expect (neutral, "off items are neutral");
            expect (bypassPassed, "clay_bypass is passed through to ClayModule");

            // GLAZE 60：「关」照样中性。阳性对照：同一画像、同一素材下「自动」给的是非中性值（低切与加宽，见 withClayNeeds）。
            const auto saved = glaze::toXml (clayStateFrom (rig.committed()));
            Rig automatic (clayConstants(), makeClayMaterial (false, false, 71), defaultParams (60.0f, 3));
            automatic.chain.restoreGlazeStateXml (saved.get());
            automatic.run (0.5);
            const auto automaticClay = automatic.chain.getEffectiveParams().clay;
            expect (automaticClay.lowCutHz > 0.0f && automaticClay.widthDb > 0.0f,
                    "positive control: automatic Clay acts on this profile at GLAZE 60: " + describe (automaticClay));

            auto offParams = defaultParams (60.0f, 3);
            offParams.claySettings.lowCutMode = offParams.claySettings.monoMode = offParams.claySettings.widthMode = ClayMode::off;
            Rig switchedOff (clayConstants(), makeClayMaterial (false, false, 71), offParams);
            switchedOff.chain.restoreGlazeStateXml (saved.get());
            neutral = true;
            switchedOff.run (1.0, [&] (int) { neutral = neutral && isNeutralClay (switchedOff.chain.getEffectiveParams().clay); });
            expect (neutral, "GLAZE 60, all off: always neutral");
            expect (! isNeutralClayPlan (switchedOff.probe().liveClay), "the automatic value still exists while off; it is just not used");
            expect (! switchedOff.status().clayNeedsRelearn, "no hint when no item is automatic");
        }

        // Clay 2. 画像格式 1 / 2 的存取与画像号（Clay 第 7 节；GlazeProfile.cpp 文件头）：
        //    - 格式 1 的画像号与 v0.1.6.2 逐位相同（与照抄的旧实现 legacyProfileId 对照），新字段不进哈希、不写进 XML；
        //    - 读格式 1：新字段一律「不知道」（valid = false、数组 0），就算读进来的目标原本有值也清掉；读完画像号不变；
        //    - 格式 2：新字段写全、往返逐位，改任一新字段画像号就变（阳性对照），缺新字段整份拒收且目标不变；
        //    - 方案的 Clay 四项往返逐位；旧方案缺这些属性 = 中性；属性在但损坏照样拒收。
        void testClayProfileFormatCompatibility()
        {
            beginTest ("profile format 2 carries the Clay features; format 1 keeps its v0.1.6.2 id and reads them as unknown");

            Rig learned (clayConstants(), makeMusic (defaultRate, 8.0, 53, 0.4f));
            learned.run (4.0);
            expect (learned.committed().hasProfile);
            const auto current = withClayNeeds (learned.committed().profile);   // 格式 2、新字段有效

            // 画像号：格式 1 与旧实现逐位相同，新字段完全不参与（内存里有值也一样）；格式 2 跟着新字段走。
            auto legacy = current;
            legacy.formatVersion = 1;
            const auto legacyId = glaze::computeProfileId (legacy);
            expectEquals ((juce::int64) legacyId, (juce::int64) legacyProfileId (legacy), "format-1 id == the v0.1.6.2 implementation");
            auto legacyChanged = legacy;
            legacyChanged.highSideToMidDb.value = 3.0f;
            legacyChanged.subShareDb.valid = false;
            legacyChanged.bandCorrelation[20] = -0.5f;
            expectEquals ((juce::int64) glaze::computeProfileId (legacyChanged), (juce::int64) legacyId,
                          "Clay fields do not enter a format-1 id");

            const auto currentId = glaze::computeProfileId (current);
            expect (currentId != legacyProfileId (current), "format 2 hashes more than the v0.1.6.2 fields");
            {
                auto changed = current;
                changed.highSideToMidDb.value = 3.0f;
                expect (glaze::computeProfileId (changed) != currentId, "positive control: a format-2 id follows the Clay features");
                changed = current;
                changed.lowCorrelation.valid = false;
                expect (glaze::computeProfileId (changed) != currentId, "... their valid flags");
                changed = current;
                changed.bandStereoConfidence[3] = 0.5f;
                expect (glaze::computeProfileId (changed) != currentId, "... and the per-band arrays");
            }

            // XML：格式 1 不写新字段；读格式 1 清掉目标里原有的新字段；画像号不变。
            const auto legacyXml = glaze::toXml (legacy);
            expect (! legacyXml->hasAttribute ("subShareDb_value") && ! legacyXml->hasAttribute ("bandCorrelation")
                    && legacyXml->getIntAttribute ("formatVersion") == 1,
                    "a format-1 profile is saved as format 1 (no invented Clay fields)");
            const auto legacyText = juce::parseXML (legacyXml->toString());
            auto read = current;   // 目标原本带着有效的新字段
            expect (legacyText != nullptr && glaze::fromXml (*legacyText, read), "a format-1 profile reads");
            expectEquals (read.formatVersion, 1);
            expect (! read.subShareDb.valid && ! read.lowSideToMidDb.valid && ! read.lowCorrelation.valid
                    && ! read.highSideToMidDb.valid && ! read.highCorrelation.valid, "format 1: every Clay feature is unknown");
            bool zeroBands = true;
            for (size_t b = 0; b < (size_t) glaze::numBands; ++b)
                zeroBands = zeroBands && read.bandSideToMidDb[b] == 0.0f && read.bandCorrelation[b] == 0.0f && read.bandStereoConfidence[b] == 0.0f;
            expect (zeroBands, "format 1: the per-band Clay arrays are zero");
            expect (glaze::bitwiseEqual (read, legacy), "format-1 fields round-trip bit-exactly");
            expectEquals ((juce::int64) glaze::computeProfileId (read), (juce::int64) legacyId, "reading a format-1 profile keeps its id");

            // 格式 2：写全、往返逐位；缺一个新字段整份拒收、目标不变。
            const auto currentXml = glaze::toXml (current);
            expect (currentXml->hasAttribute ("subShareDb_value") && currentXml->hasAttribute ("highCorrelation_confidence")
                    && currentXml->hasAttribute ("bandStereoConfidence"), "format 2 writes the Clay features");
            glaze::MusicProfile back;
            expect (glaze::fromXml (*currentXml, back) && glaze::bitwiseEqual (back, current), "format 2 round-trips");
            expect (back.highSideToMidDb.valid && sameBits (back.highSideToMidDb.value, current.highSideToMidDb.value)
                    && sameBits (back.bandCorrelation[20], current.bandCorrelation[20]), "... bit for bit");
            expectEquals ((juce::int64) glaze::computeProfileId (back), (juce::int64) currentId);
            auto missing = std::make_unique<juce::XmlElement> (*currentXml);
            missing->removeAttribute ("lowCorrelation_valid");
            auto untouched = legacy;
            expect (! glaze::fromXml (*missing, untouched), "a format-2 profile missing a Clay field is broken data");
            expect (untouched.formatVersion == 1 && untouched.highSideToMidDb.valid
                    && sameBits (untouched.highSideToMidDb.value, legacy.highSideToMidDb.value), "... and the output is left alone");

            // 方案的 Clay 四项：往返逐位；旧方案（没有这些属性）= 中性；属性在但损坏拒收。
            auto state = learned.committed();
            state.plan.clay = { 28.0f, 120.0f, 0.5f, 0.75f, 0 };
            const auto stateXml = glaze::toXml (state);
            glaze::CommittedState stateBack;
            expect (glaze::fromXml (*stateXml, stateBack) && glaze::bitwiseEqual (stateBack, state)
                    && sameClayPlan (stateBack.plan.clay, state.plan.clay), "the plan's Clay values round-trip bit-exactly");
            auto olderPlanXml = std::make_unique<juce::XmlElement> (*stateXml);
            for (const auto* attribute : { "clayLowCutHz", "clayMonoHz", "clayMonoAmount", "clayWidthDb", "clayCostLimitedMask" })
                olderPlanXml->getChildByName ("PLAN")->removeAttribute (attribute);
            auto olderPlan = state;   // 目标原本带着非中性的 Clay
            expect (glaze::fromXml (*olderPlanXml, olderPlan) && sameClayPlan (olderPlan.plan.clay, glaze::ClayPlan {}),
                    "an older plan without Clay attributes reads as neutral");
            olderPlanXml->getChildByName ("PLAN")->setAttribute ("clayWidthDb", "oops");
            expect (! glaze::fromXml (*olderPlanXml, olderPlan), "a present but corrupted Clay attribute is rejected");
        }

        // Clay 3. 旧工程（画像格式 1）：Clay 自动值恒为中性、保护与反相提示都不动（就算素材低频反相）——旧工程重新聆听之前 Clay 逐位直通、
        //    声音不变；状态提示「点重新聆听」（GLAZE > 0 且至少一项自动时）。两种旧存档都试：旧算法方案（照用保存的增量，还特意
        //    存了一份非中性的 Clay 值，确认它不被采用；拧 GLAZE 按新算法重算后仍中性）与当前算法方案（拧过 GLAZE 后再存的）。
        //    点「重新聆听」提交格式 2 的新画像后自动 Clay 启用、上述提示消失（同一段反相低频：反相提示亮起，收单不动——ADR-015）。
        //    阳性对照也见 Clay 5：同样的反相素材配格式 2 画像时反相提示会亮（收单不动）。
        // 依据：Clay 第 7 节「旧画像没有立体声特征 → Clay 自动增量为 0 → 打开后声音不变；点重新聆听后启用」、第 5 节悬停气泡第 3 行；
        //    时间线第 5 节旧算法方案；任务书「clayNeedsRelearn = GLAZE > 0 且至少一项为自动 且 已提交画像缺 Clay 所需特征」。
        void testClayOldProfileStaysNeutral()
        {
            beginTest ("old format-1 profiles keep automatic Clay neutral (no guard either) and ask for a relearn");

            const auto antiPhase = makeClayMaterial (true, false, 75);
            Rig learned (clayConstants(), antiPhase, defaultParams (0.0f, 3));
            learned.run (4.0);
            auto state = learned.committed();
            expect (state.hasProfile);
            state.profile = withClayNeeds (state.profile);   // 就算内存里有值……
            state.profile.formatVersion = 1;                  // ……格式 1 的存档里根本没有（读回来全是「不知道」）
            state.profileId = glaze::computeProfileId (state.profile);
            state.calibration = glaze::Calibration {};

            for (const auto olderAlgorithm : { true, false })
            {
                const auto tag = juce::String (olderAlgorithm ? "older-algorithm plan" : "current-algorithm plan");
                auto saved = state;
                saved.plan = planFor (state, glaze::Style::clean, 60.0f, defaultParams (60.0f, 3), 1.0e9f);

                if (olderAlgorithm)
                {
                    saved.plan.algorithmVersion = glaze::oldestCompatibleAlgorithmVersion - 1;   // 比兼容下限还旧：照用保存的增量
                    saved.plan.clay = { 40.0f, 150.0f, 0.8f, 1.0f, 0 };   // 旧算法方案里的 Clay 值绝不采用
                }

                Rig rig (clayConstants(), antiPhase, defaultParams (60.0f, 3));
                rig.chain.restoreGlazeStateXml (glaze::toXml (saved).get());

                bool neutral = true, guardIdle = true, hint = true;
                rig.run (3.0, [&] (int)
                {
                    const auto probe = rig.probe();
                    neutral = neutral && isNeutralClay (rig.chain.getEffectiveParams().clay) && isNeutralClayPlan (probe.liveClay)
                           && isNeutralClayPlan (probe.clayPlanned);
                    guardIdle = guardIdle && ! probe.clayGuard.phaseActive && ! probe.clayGuard.widthActive && ! probe.clayAutomaticSupported;
                    hint = hint && rig.status().clayNeedsRelearn;
                });
                expectEquals (rig.committed().profile.formatVersion, 1, tag + ": the committed profile stays format 1");
                expect (rig.probe().savedPlanMode == olderAlgorithm, tag + ": saved-plan mode as expected");
                expect (neutral, tag + ": automatic Clay stays neutral on every block (bit-exact pass-through)");
                expect (guardIdle, tag + ": the guard does not act on a format-1 profile, even on anti-phase bass");
                expect (hint, tag + ": the status asks for a relearn");

                if (olderAlgorithm)
                {
                    // 拧 GLAZE：按新算法重算（一次方案过渡），画像仍是格式 1 → 仍中性、仍提示。
                    rig.params.glazePercent = 61.0f;
                    neutral = true;
                    rig.run (1.5, [&] (int) { neutral = neutral && isNeutralClay (rig.chain.getEffectiveParams().clay); });
                    expect (! rig.probe().savedPlanMode, tag + ": turning GLAZE recomputes with the current algorithm");
                    expect (neutral && ! rig.probe().clayAutomaticSupported, tag + ": ... still neutral on a format-1 profile");
                    expect (rig.status().clayNeedsRelearn, tag + ": ... and still asks for a relearn");
                }

                // 提示的条件：GLAZE 0 不提示；没有一项是「自动」不提示。
                const auto glazeBefore = rig.params.glazePercent;
                rig.params.glazePercent = 0.0f;
                rig.step();
                expect (! rig.status().clayNeedsRelearn, tag + ": no hint at GLAZE 0");
                rig.params.glazePercent = glazeBefore;
                rig.params.claySettings.lowCutMode = rig.params.claySettings.monoMode = rig.params.claySettings.widthMode = ClayMode::off;
                rig.step();
                expect (! rig.status().clayNeedsRelearn, tag + ": no hint when no item is automatic");
                rig.params.claySettings = ClaySettings {};
                rig.step();
                expect (rig.status().clayNeedsRelearn, tag + ": the hint returns with the automatic items");

                if (! olderAlgorithm)
                {
                    // 重新聆听：提交格式 2 的新画像 → 自动 Clay 启用、提示消失（同一段反相低频：反相提示亮起，收单不动——ADR-015）。
                    rig.chain.requestGlazeRelearn();
                    const auto committedNew = rig.runUntil (8.0, [&] { return rig.probe().commits >= 1; });
                    expect (committedNew, tag + ": the relearn commits a new profile");
                    expectEquals (rig.committed().profile.formatVersion, glaze::clayProfileFormatVersion,
                                  tag + ": the relearned profile is format 2");
                    rig.run (2.0);
                    expect (rig.probe().clayAutomaticSupported, tag + ": automatic Clay is enabled after the relearn");
                    expect (! rig.status().clayNeedsRelearn, tag + ": the hint disappears after the relearn");
                    expect (rig.probe().clayGuard.phaseActive && rig.status().clayPhaseGuardActive,
                            tag + ": and warns about the anti-phase bass");
                    expectEquals (rig.chain.getEffectiveParams().clay.monoAmount, 0.0f,
                                  tag + ": without touching the mono amount (ADR-015): " + describe (rig.chain.getEffectiveParams().clay));
                }
            }
        }

        // Clay 4. 方案过渡中 Clay 自动值与自动增量同一过渡进度混合（任务书；时间线第 4 节）：频率在对数域、比例与 dB 线性；
        //    「关」（0）的一端按 clayLowCutFloorHz 参与对数混合（两端都关 = 0）；收单比例为 0 的一端用另一端的分频点。
        //    先按规格数字核对纯函数 blendClayPlans，再在编排器里走两种过渡、逐块逐位核对（t = 已走样本 / 过渡长度，
        //    与 currentIncrements 同一个式子；过渡 0.5 s = 24000 样本、块 512）：
        //      a) 旧算法方案（Clay 中性）→ 拧 GLAZE 按新算法重算：低切从「关」经 floor 起步；
        //      b) 换风格（通透 → 温润）：两端都开，低切 20 → 16 Hz（根治修订 3.3 的风格目标）、加宽按风格系数 1.0 → 0.4 线性。
        //    单声道素材下反相提示不亮、宽度保护不动：保护之后 = 方案值，实际参数 = 三态合成。
        void testClayTransitionBlend()
        {
            beginTest ("plan transitions blend Clay with the increments' progress: log-domain frequencies, an off end joins at the floor");

            const auto floorConstant = clayConstants().clayLowCutFloorHz;
            const auto floorHz = std::isfinite (floorConstant) && floorConstant > 0.0f
                                     ? juce::jlimit (ClayModule::minLowCutHz, ClayModule::maxLowCutHz, floorConstant)
                                     : ClayModule::minLowCutHz;   // 与编排器 clayLowCutFloorHz() 同一口径（出厂 10 Hz）

            // 纯函数。
            const glaze::ClayPlan off {};
            const glaze::ClayPlan on { 28.0f, 120.0f, 0.6f, 1.2f, 0 };
            expect (sameClayPlan (glaze::blendClayPlans (off, on, 0.0f, floorHz), off), "t = 0 is exactly the start");
            expect (sameClayPlan (glaze::blendClayPlans (off, on, 1.0f, floorHz), on), "t = 1 is exactly the end");
            expect (sameClayPlan (glaze::blendClayPlans (off, on, std::numeric_limits<float>::quiet_NaN(), floorHz), off),
                    "NaN progress counts as not started");
            const auto half = glaze::blendClayPlans (off, on, 0.5f, floorHz);
            expectWithinAbsoluteError (half.lowCutHz, std::sqrt (floorHz * 28.0f), 1.0e-3f,
                                       "an off end joins the log-domain mix at the floor: sqrt (floor x 28) Hz");
            expectWithinAbsoluteError (half.monoHz, 120.0f, 1.0e-3f, "no mono at one end: the crossover does not sweep");
            expectWithinAbsoluteError (half.monoAmount, 0.3f, 1.0e-6f, "mono amount is linear");
            expectWithinAbsoluteError (half.widthDb, 0.6f, 1.0e-6f, "width is linear in dB");
            expectEquals (glaze::blendClayPlans (off, off, 0.5f, floorHz).lowCutHz, 0.0f, "both ends off stays off");

            // 代价 mask（ADR-012，只给界面解释）：过渡中取目标的，两端逐位原样（GlazeController.h blendClayPlans 注释）。
            {
                auto maskedFrom = off;
                maskedFrom.costLimitedMask = glaze::clayCostLimitedWidth;
                auto maskedTo = on;
                maskedTo.costLimitedMask = glaze::clayCostLimitedLowCut;
                expectEquals ((int) glaze::blendClayPlans (maskedFrom, maskedTo, 0.0f, floorHz).costLimitedMask,
                              (int) glaze::clayCostLimitedWidth, "t = 0: the start's mask (the start, bit for bit)");
                expectEquals ((int) glaze::blendClayPlans (maskedFrom, maskedTo, 0.3f, floorHz).costLimitedMask,
                              (int) glaze::clayCostLimitedLowCut, "inside the transition: the target's mask");
                expectEquals ((int) glaze::blendClayPlans (maskedFrom, maskedTo, 1.0f, floorHz).costLimitedMask,
                              (int) glaze::clayCostLimitedLowCut, "t = 1: the target's mask");
            }
            const auto towardsOff = glaze::blendClayPlans (on, off, 0.25f, floorHz);
            expectWithinAbsoluteError (towardsOff.lowCutHz, 28.0f * std::pow (floorHz / 28.0f, 0.25f), 1.0e-3f,
                                       "towards off: log mix down to the floor (ClayModule's 20 ms fade takes the last step to off)");
            expectWithinAbsoluteError (towardsOff.monoHz, 120.0f, 1.0e-3f);
            const glaze::ClayPlan other { 20.0f, 60.0f, 0.2f, 0.0f, 0 };
            const auto quarter = glaze::blendClayPlans (on, other, 0.25f, floorHz);
            expectWithinAbsoluteError (quarter.lowCutHz, 28.0f * std::pow (20.0f / 28.0f, 0.25f), 1.0e-3f, "both on: geometric interpolation");
            expectWithinAbsoluteError (quarter.monoHz, 120.0f * std::pow (60.0f / 120.0f, 0.25f), 1.0e-3f);
            expectWithinAbsoluteError (quarter.monoAmount, 0.6f + (0.2f - 0.6f) * 0.25f, 1.0e-6f);
            expectWithinAbsoluteError (quarter.widthDb, 1.2f * 0.75f, 1.0e-6f);
            bool monotone = true;
            auto previousHz = on.lowCutHz;
            for (int i = 1; i <= 100; ++i)
            {
                const auto hz = glaze::blendClayPlans (on, other, (float) i / 100.0f, floorHz).lowCutHz;
                monotone = monotone && hz <= previousHz && hz >= other.lowCutHz;
                previousHz = hz;
            }
            expect (monotone, "the low cut moves monotonically between the two ends");

            // 编排器：格式 2 画像 + 一份旧算法方案。
            const auto material = makeClayMaterial (false, false, 72);
            Rig learned (clayConstants(), material, defaultParams (0.0f, 3));
            learned.run (4.0);
            expect (learned.committed().hasProfile);
            const auto state = clayStateFrom (learned.committed());
            auto saved = state;
            saved.plan = planFor (state, glaze::Style::clean, 60.0f, defaultParams (60.0f, 3), 1.0e9f);
            saved.plan.algorithmVersion = glaze::oldestCompatibleAlgorithmVersion - 1;   // 比兼容下限还旧：照用保存的增量
            saved.plan.clay = { 40.0f, 150.0f, 0.8f, 1.0f, 0 };   // 旧算法方案里的 Clay 值不采用

            // 播放守护不动作的稳态低音（T-043）：谐波素材上守护的混合损失会让压力反馈降档，方案就不再逐块等于过渡混合。
            Rig rig (clayConstants(), makeSteadyBass(), defaultParams (60.0f, 3));
            rig.chain.restoreGlazeStateXml (glaze::toXml (saved).get());
            rig.run (0.5);
            expect (rig.probe().savedPlanMode && rig.status().savedPlanFromOlderAlgorithm, "the older plan is kept as saved");
            expect (isNeutralClayPlan (rig.probe().clayPlanned) && isNeutralClay (rig.chain.getEffectiveParams().clay),
                    "older-algorithm plan: automatic Clay is neutral (the plan's own Clay values are never used)");

            constexpr std::int64_t transitionSamples = 24000;   // clayConstants().planTransitionSeconds 0.5 s × 48 kHz
            const auto checkTransition = [&] (const glaze::ClayPlan& from, const glaze::ClayPlan& to, const juce::String& label)
            {
                bool exact = true, followed = true;
                int firstMismatch = -1;
                glaze::ClayPlan got, wanted;

                for (int k = 0; k < 60; ++k)
                {
                    rig.step();
                    const auto elapsed = (std::int64_t) k * blockSize;
                    const auto expected = elapsed < transitionSamples
                        ? glaze::blendClayPlans (from, to, (float) juce::jlimit (0.0, 1.0, (double) elapsed / (double) transitionSamples), floorHz)
                        : to;
                    const auto probe = rig.probe();

                    if (! sameClayPlan (probe.clayPlanned, expected) && firstMismatch < 0)
                    {
                        exact = false;
                        firstMismatch = k;
                        got = probe.clayPlanned;
                        wanted = expected;
                    }

                    followed = followed && sameClayPlan (probe.liveClay, probe.clayPlanned)
                            && sameClayParams (rig.chain.getEffectiveParams().clay,
                                               glaze::resolveClayParams (rig.params.claySettings, probe.liveClay, rig.params.glazePercent));
                }

                expect (exact, label + ": Clay follows the plan transition block by block; first mismatch at block "
                               + juce::String (firstMismatch) + ": got " + describe (got) + ", wanted " + describe (wanted));
                expect (followed, label + ": ... through the idle guard into the three-state resolution");
                expect (! rig.probe().transitionActive, label + ": the transition has finished");
            };

            // a) 拧 GLAZE：旧方案（中性）→ 新算法方案。
            rig.params.glazePercent = 61.0f;
            const auto target = planFor (rig.committed(), glaze::Style::clean, 61.0f, rig.params, rig.probe().feedbackMaxGainDb).clay;
            expect (target.lowCutHz > 0.0f && target.widthDb > 0.0f,
                    "positive control: the current algorithm plans low cut and widening for this profile: " + describe (target));
            checkTransition (glaze::ClayPlan {}, target, "older plan -> current algorithm");
            expect (! rig.probe().savedPlanMode);

            // b) 换风格：通透 → 温润（两端低切都开）。
            const auto from = rig.probe().clayPlanned;
            expect (sameClayPlan (from, target), "settled on the current plan");
            rig.params.glazeStyle = 1;
            const auto warm = planFor (rig.committed(), glaze::Style::warm, 61.0f, rig.params, 1.0e9f).clay;
            expect (from.lowCutHz > 0.0f && warm.lowCutHz > 0.0f && ! sameClayPlan (warm, from),
                    "the two styles plan different Clay values with the low cut on at both ends: " + describe (from) + " -> " + describe (warm));
            checkTransition (from, warm, "style clean -> warm");
        }

        // Clay 6. 修坯压力反馈（ADR-014）：方案要低切（clayStateFrom 的画像：need_sub = 1，通透 20 Hz；代价 −0.5 dB → 照做），
        //    素材却是 40 Hz 方波（±0.5，单声道）——4 阶高通让方波平顶倾斜、每个边沿都冲过 ±0.5，ClayModule 的峰值守护每半个周期
        //    都要把低切的改动量拉回来，回收时与干路相位不同处互相抵消（混合损失）。压力反馈（两个触发量共用档位与冷却）把自动 Clay
        //    的缩放降一档（1 → 0.5 → 0.25 → 0），每档走方案过渡：
        //      - 40 s 内至少降两档（缩放 ≤ 0.25），状态报低切「因代价减量 / 跳过」（costLimitedMask 的低切位）；自动低切比学完时低。
        //        T-044 实测：两档都是第二触发量（混合损失，2 s 平滑）先到——第一触发量（峰值块 P99，ADR-028）每档要先攒满 10 s，
        //        攒不满就随降档清空了；第一触发量另由 Clay 6b 专测；
        //      - 存档带着缩放（控制器节点的 clayPressureScale 属性），读回新链后缩放与方案一致；删掉这个属性的旧存档 = 1；
        //    阳性对照：同一画像、守护不动作的稳态低音（makeSteadyBass）20 s 不降档。
        //    T-043（ADR-027）：原先的阳性对照素材（60 / 110 Hz + 白噪声）峰值超出很小、守护却频繁回收——ADR-014 当时以为「回收量夸大了
        //    伤害」；按混合损失看，回收时低切后的信号与干路在 60 Hz 差约 51°、互相抵消，低音实打实变小，第二个触发量在 20 s 内降档
        //    （第二个触发量 2 s 平滑先到；第一个要先攒满 10 s 峰值块，T-044）。
        void testClayPressureFeedback()
        {
            beginTest ("Clay pressure feedback (ADR-014): a guard that keeps pulling the low cut back steps the automatic Clay scale down, and it is saved");

            const auto normal = makeClayMaterial (false, false, 83);
            Rig learned (clayConstants(), normal, defaultParams (0.0f, 3));
            learned.run (4.0);
            expect (learned.committed().hasProfile);
            const auto saved = glaze::toXml (clayStateFrom (learned.committed()));

            auto square = makeSilence (numChannels, (int) (4.0 * defaultRate));   // 40 Hz 在 4 s 里是整数周期：循环播放没有接缝

            for (size_t i = 0; i < square[0].size(); ++i)
            {
                const auto phase = std::fmod (40.0 * (double) i / defaultRate, 1.0);
                square[0][i] = square[1][i] = phase < 0.5 ? 0.5f : -0.5f;
            }

            // 阳性对照：守护不动作的素材不降档。
            {
                Rig calm (clayConstants(), makeSteadyBass(), defaultParams (60.0f, 3));
                calm.chain.restoreGlazeStateXml (saved.get());
                calm.run (20.0);
                expect (calm.probe().clayAutomaticSupported);
                expectEquals (calm.probe().clayPressureSteps, 0, "steady bass: no pressure step (peak cost "
                                                                 + juce::String (calm.probe().clayPeakCostDb, 4) + " dB, blend loss "
                                                                 + juce::String (calm.probe().clayBlendLossSmoothedDb, 4) + " dB)");
                expectEquals (calm.probe().clayPressureScale, 1.0f);
                expect (calm.probe().clayBlendLossSmoothedDb < 0.01, "the guard never blends on a steady tone");
            }

            // T-043：谐波素材——峰值超出很小，混合损失却持续超过 0.15 dB → 第二个触发量降档。
            {
                Rig gentle (clayConstants(), normal, defaultParams (60.0f, 3));
                gentle.chain.restoreGlazeStateXml (saved.get());
                auto peakBlend = 0.0;
                gentle.runUntil (20.0, [&]
                {
                    peakBlend = juce::jmax (peakBlend, gentle.probe().clayBlendLossSmoothedDb);
                    return gentle.probe().clayPressureSteps > 0;
                });
                logMessage ("  60 + 110 Hz material: blend loss up to " + juce::String (peakBlend, 3) + " dB, steps "
                            + juce::String (gentle.probe().clayPressureSteps) + ", peak cost " + juce::String (gentle.probe().clayPeakCostDb, 4));
                expect (gentle.probe().clayPressureSteps >= 1 && peakBlend > glaze::ClayPressureMonitor::blendThreshold,
                        "the guard's blending costs > 0.15 dB of bass on the harmonic pair: the second trigger steps down");
            }

            Rig rig (clayConstants(), square, defaultParams (60.0f, 3));
            rig.chain.restoreGlazeStateXml (saved.get());
            rig.run (0.5);
            const auto initialLowCut = rig.probe().clayPlanned.lowCutHz;
            expect (initialLowCut > 15.0f, "positive control: the restored profile plans a real low cut: " + juce::String (initialLowCut));

            auto squareCost = 0.0;
            const auto stepped = rig.runUntil (40.0, [&]
            {
                squareCost = juce::jmax (squareCost, rig.probe().clayPeakCostDb);
                return rig.probe().clayPressureScale <= 0.25f;
            });
            rig.run (4.0);   // 让最后一次方案过渡走完

            const auto probe = rig.probe();
            logMessage ("  square wave: peak cost up to " + juce::String (squareCost, 3) + " dB; pressure steps " + juce::String (probe.clayPressureSteps)
                        + ", scale " + juce::String (probe.clayPressureScale)
                        + ", planned low cut " + juce::String (probe.clayPlanned.lowCutHz) + " Hz (was " + juce::String (initialLowCut) + ")");
            expect (stepped && probe.clayPressureSteps >= 2, "the low cut's sustained peak cost steps the scale down at least twice within 40 s");
            expect (probe.clayPlanned.lowCutHz < initialLowCut, "the automatic low cut is lower (or off) after the steps");
            expect ((rig.status().clayCostLimitedMask & glaze::clayCostLimitedLowCut) != 0, "the status explains the reduction (low-cut cost bit)");

            // 存档与读回。
            const auto xml = rig.chain.createGlazeStateXml();
            const auto* controllerNode = xml != nullptr ? xml->getChildByName ("GLAZE_CONTROLLER") : nullptr;
            expect (controllerNode != nullptr && controllerNode->hasAttribute ("clayPressureScale"), "the scale is saved with the project");

            Rig reopened (clayConstants(), square, defaultParams (60.0f, 3));
            reopened.chain.restoreGlazeStateXml (xml.get());
            reopened.step();
            expectEquals (reopened.probe().clayPressureScale, probe.clayPressureScale, "reopening keeps the scale");

            auto legacy = std::make_unique<juce::XmlElement> (*xml);

            if (auto* node = legacy->getChildByName ("GLAZE_CONTROLLER"))
                node->removeAttribute ("clayPressureScale");

            Rig older (clayConstants(), square, defaultParams (60.0f, 3));
            older.chain.restoreGlazeStateXml (legacy.get());
            older.step();
            expectEquals (older.probe().clayPressureScale, 1.0f, "a project saved without the attribute reads as 1");
        }

        // Clay 6b. 压力反馈第一触发量与先试同口径（T-044，BUG-011 / ADR-028）。同一份要低切 20 Hz 的画像（Clay 6），播放
        //    makeInfraBassMaterial：
        //      - 限过幅、次声 0.06：切掉被限幅器算进去的次声，顶峰真被抬（链里约 +0.3 dB：先试只会「减量」的区间，要满 512 块 =
        //        51.2 s 才算）。70 s 内降档，且是第一触发量降的——降之前代价 > 0.2 dB 且攒满 512 块、混合损失 < 0.15 dB。T-044 之前的
        //        「10 ms 格超界平均」在这份素材上 2 s 平均最大约 0.03 dB，从不降档（改前源码实测，见 T-044 REPORT）；
        //      - 不限幅、次声 0.08：顶峰来自低音与次声同向叠加，切掉次声顶峰下降——守护照样有活干（局部峰被抬），但第一触发量的代价
        //        为负、30 s 不因它降档（滑动短窗口径会在这类歌的安静段误降，见 ClayGuard.h）。
        void testClayPressurePeakCost()
        {
            beginTest ("Clay pressure feedback, first trigger (T-044 / ADR-028): the trial's P99 peak-block cost steps a low cut that lifts the top peaks of a limited master");

            const auto normal = makeClayMaterial (false, false, 83);
            Rig learned (clayConstants(), normal, defaultParams (0.0f, 3));
            learned.run (4.0);
            expect (learned.committed().hasProfile);
            const auto saved = glaze::toXml (clayStateFrom (learned.committed()));

            {
                Rig rig (clayConstants(), makeInfraBassMaterial (true, 0.06), defaultParams (60.0f, 3));
                rig.chain.restoreGlazeStateXml (saved.get());
                rig.run (0.5);
                const auto initialLowCut = rig.probe().clayPlanned.lowCutHz;
                expect (initialLowCut > 15.0f, "positive control: the restored profile plans a real low cut: " + juce::String (initialLowCut));

                auto costBeforeStep = 0.0, blendBeforeStep = 0.0;
                auto blocksBeforeStep = 0;
                const auto stepped = rig.runUntil (70.0, [&]
                {
                    const auto probe = rig.probe();

                    if (probe.clayPressureSteps > 0)
                        return true;

                    costBeforeStep = probe.clayPeakCostDb;
                    blocksBeforeStep = probe.clayPeakCostBlocks;
                    blendBeforeStep = juce::jmax (blendBeforeStep, probe.clayBlendLossSmoothedDb);
                    return false;
                });

                logMessage ("  limited bass + infrasound: step " + juce::String (stepped ? "yes" : "no") + ", peak cost before it "
                            + juce::String (costBeforeStep, 3) + " dB over " + juce::String (0.1 * blocksBeforeStep, 1) + " s, blend loss max "
                            + juce::String (blendBeforeStep, 3) + " dB");
                expect (stepped, "the low cut lifts the top peaks of the limited material: the first trigger steps down within 70 s");
                expect (costBeforeStep > glaze::ClayPressureMonitor::threshold && blocksBeforeStep >= glaze::ClayPressureMonitor::confirmBlocks,
                        "... on the trial's measure (P99 peak-block cost above 0.2 dB over at least 51.2 s)");
                expect (blendBeforeStep < glaze::ClayPressureMonitor::blendThreshold, "... and not through the blend loss (second trigger)");

                rig.run (4.0);   // 让这一档的方案过渡走完
                logMessage ("  planned low cut " + juce::String (rig.probe().clayPlanned.lowCutHz, 2) + " Hz (was " + juce::String (initialLowCut, 2)
                            + "), scale " + juce::String (rig.probe().clayPressureScale, 2));
                expect (rig.probe().clayPlanned.lowCutHz < initialLowCut, "the automatic low cut is lower (or off) after the step");
                expect ((rig.status().clayCostLimitedMask & glaze::clayCostLimitedLowCut) != 0, "the status explains the reduction (low-cut cost bit)");
            }

            {
                Rig rig (clayConstants(), makeInfraBassMaterial (false, 0.08), defaultParams (60.0f, 3));
                rig.chain.restoreGlazeStateXml (saved.get());
                std::int64_t guarded = 0;
                rig.run (30.0, [&] (int) { guarded += rig.chain.getClayForTesting().getAndResetPeakGuardStats().guardedSamples; });
                const auto probe = rig.probe();
                logMessage ("  unlimited bass + infrasound: peak cost " + juce::String (probe.clayPeakCostDb, 3) + " dB over "
                            + juce::String (0.1 * probe.clayPeakCostBlocks, 1) + " s, steps " + juce::String (probe.clayPressureSteps)
                            + ", guard g < 1 on " + juce::String (100.0 * (double) guarded / (30.0 * defaultRate), 1) + " % of samples");
                expect (guarded > 0, "positive control: the guard does pull the low cut back on this material");
                expect (probe.clayPeakCostBlocks >= glaze::ClayPressureMonitor::minimumBlocks && probe.clayPeakCostDb < 0.0,
                        "the low cut lowers the top peaks: the first trigger's cost is negative");
                expectEquals (probe.clayPressureSteps, 0, "no step: neither the peak cost nor the blend loss is above its threshold");
            }
        }

        // T-074 守护回收指示经链与编排器生效（ClayModule 的保留比统计 → ModuleStats → ClayGuardKeepMonitor → Status::clayPeakGuardActive / Kept）：
        //   a) 低音噪声（M / S 各自独立）+ GLAZE 0 + 手动收单 120 Hz：守护大半时间在拉（阳性对照：g < 1 的样本很多），保留比 < 75 % 持续 1 s 后状态亮
        //      （0.5 s 时还没亮：要连续满 1 s）、保留比读数 < 0.75；GLAZE 0 也报——手动项恰恰没有别的反馈；
        //   b) 同一素材、Clay 全「自动」（GLAZE 0 = 中性）：每一块都不亮、保留比 1（阳性对照：同样的素材与链，只差手动收单）；
        //   c) 手动收单改回「关」：几秒内熄灭、保留比回到 1；
        //   d) 稳态 60 Hz 正弦 + 手动低切 20 Hz（守护不动作，g = 1）：不亮、保留比 1——低切在做（实际值 20 Hz）、守护没收，没有东西可指示；
        //   e) 链整体重建（prepare）：监测器的两份平滑能量与计时清零，不带着旧状态的「亮」继续。
        // 依据：ClayGuard.h ClayGuardKeepMonitor（75 % / 1 s 亮、85 % / 1 s 熄；2 s 平滑）、设计稿 R4-03、V4（手动收单 78 % 样本被回收、界面全绿）。
        void testClayPeakGuardIndicatorThroughController()
        {
            beginTest ("the peak guard recovery indicator through the chain (T-074): a manually mono-ed bass pulled back by the guard lights the status; neutral Clay and an unguarded tone do not");

            const auto bass = makeBassNoiseMidSide (4407);

            {
                Rig rig (clayConstants(), bass, defaultParams (0.0f, 3));
                auto& settings = rig.params.claySettings;
                settings.monoMode = ClayMode::manual;
                settings.monoHz = 120.0f;
                std::int64_t guarded = 0;
                rig.run (0.5, [&] (int) { guarded += rig.chain.getClayForTesting().getAndResetPeakGuardStats().guardedSamples; });
                expect (! rig.status().clayPeakGuardActive, "not lit after 0.5 s: the ratio must stay below the line for a full second");
                rig.run (5.5, [&] (int) { guarded += rig.chain.getClayForTesting().getAndResetPeakGuardStats().guardedSamples; });
                const auto status = rig.status();
                logMessage ("  manual mono 120 Hz on independent mid / side bass noise, GLAZE 0: guard g < 1 on " + juce::String (100.0 * (double) guarded / (6.0 * defaultRate), 1)
                            + " % of samples, kept " + juce::String (status.clayPeakGuardKept, 3) + ", lit " + (status.clayPeakGuardActive ? "yes" : "no"));
                expect (status.clayMonoAmount == 1.0f && status.clayMonoHz == 120.0f, "the manual mono is what Clay got");
                expect (guarded > (std::int64_t) (0.3 * 6.0 * defaultRate), "positive control: the guard pulls the mono back on a large share of the samples");
                expect (status.clayPeakGuardActive, "lit: the guard keeps well under 75 % of the manual mono (GLAZE 0 reports too)");
                expect (status.clayPeakGuardKept < 0.75f, "the reported keep ratio is below the 75 % line");

                // c) 收单改回「关」：几秒内熄灭、保留比回到 1。
                settings.monoMode = ClayMode::off;
                rig.run (6.0);   // 快门约 2 s 看到「没有改动」、再满 1 s 熄
                const auto after = rig.status();
                expect (! after.clayPeakGuardActive && after.clayPeakGuardKept > 0.99f, "mono off: the indicator clears and the keep ratio returns to 1");
            }

            {
                // b) 阳性对照：同一素材与链，Clay 全「自动」（GLAZE 0：中性）。
                Rig rig (clayConstants(), bass, defaultParams (0.0f, 3));
                auto everLit = false;
                float lowestKept = 1.0f;
                rig.run (6.0, [&] (int)
                {
                    const auto status = rig.status();
                    everLit = everLit || status.clayPeakGuardActive;
                    lowestKept = juce::jmin (lowestKept, status.clayPeakGuardKept);
                });
                expect (! everLit && lowestKept == 1.0f, "neutral Clay (GLAZE 0, all automatic): never lit, keep ratio always 1");
            }

            {
                // d) 守护不动作的稳态单音 + 手动低切：不亮。
                Rig rig (clayConstants(), makeSine (numChannels, defaultRate, 60.0, 0.5f, 4.0), defaultParams (0.0f, 3));
                auto& settings = rig.params.claySettings;
                settings.lowCutMode = ClayMode::manual;
                settings.lowCutHz = 20.0f;
                std::int64_t guarded = 0;
                auto everLit = false;
                rig.run (6.0, [&] (int)
                {
                    guarded += rig.chain.getClayForTesting().getAndResetPeakGuardStats().guardedSamples;
                    everLit = everLit || rig.status().clayPeakGuardActive;
                });
                expect (rig.status().clayLowCutHz == 20.0f, "the manual low cut is what Clay got");
                expect (guarded < (std::int64_t) (0.01 * 6.0 * defaultRate), "positive control: the guard (almost) never engages on a steady tone");
                expect (! everLit && rig.status().clayPeakGuardKept > 0.95f, "an unguarded low cut is not flagged");
            }

            {
                // e) 链整体重建（prepare：采样率 / 块长 / 声道数变了）：保留比的统计从零起——两份平滑能量与计时清零，不带着旧状态的「亮」继续。
                Rig rig (clayConstants(), bass, defaultParams (0.0f, 3));
                auto& settings = rig.params.claySettings;
                settings.monoMode = ClayMode::manual;
                settings.monoHz = 120.0f;
                rig.run (6.0);
                const auto before = rig.probe();
                expect (rig.status().clayPeakGuardActive && before.clayGuardKeepChange > 0.0 && before.clayGuardKeepKept > 0.0,
                        "positive control: lit, with smoothed energy in the monitor");
                rig.reprepare (defaultRate);
                const auto after = rig.probe();
                expect (after.clayGuardKeepChange == 0.0 && after.clayGuardKeepKept == 0.0 && after.clayGuardKeepBelowSeconds == 0.0,
                        "a rebuilt chain (prepare) starts the guard statistics over");
            }
        }

        // Clay 5. 实时反相提示与宽度保护经编排器生效（分析器看真实素材 → ClayGuard → 三态合成）：
        //    a) 反相：格式 2 画像的方案收单为 0；素材切到低频反相后只亮提示（提示强度升到 1，状态「低频反相：单声道播放会丢低音」），
        //       收单每块仍是 0（ADR-015）；切回正常素材后提示先保持再熄灭；「关」态收单同样不动，也不报提示灯（提示照样在算）；
        //    b) 宽度：方案要加宽（画像很窄），素材的高频却很宽（预测加宽后相关度 < 0.25）→ 自动加宽被收到 0（状态「宽度保护中」），
        //       方案本身不变；换回窄素材后放开、回到方案值。
        // 依据：Clay 子文档第 3 节保护表、第 6 节第 5 条（「手动」「关」态不受保护改动）；ADR-015（反相只提示）；ClayGuard.h。
        //    时间只放宽上限（分析器 300 ms 平滑 + 资格期 0.15 s + 攻击；保持 2 s、释放 2 s），核对反相提示「会亮、会保持、会熄灭、
        //    收单不动、灯只报自动态」与宽度保护「会介入、会退回」，精确时序由 ClayGuard / ClayGlaze 测试核对。
        void testClayGuardThroughController()
        {
            beginTest ("the Clay guard acts through the controller: anti-phase bass lights the warning (mono untouched), wide highs cap automatic widening");

            const auto normal = makeClayMaterial (false, false, 73);
            const auto antiPhase = makeClayMaterial (true, false, 73);
            const auto wide = makeClayMaterial (false, true, 73);
            Rig learned (clayConstants(), normal, defaultParams (0.0f, 3));
            learned.run (4.0);
            expect (learned.committed().hasProfile);
            const auto saved = glaze::toXml (clayStateFrom (learned.committed()));

            // a) 反相。
            for (const auto mode : { ClayMode::automatic, ClayMode::off })
            {
                const auto tag = juce::String (mode == ClayMode::automatic ? "mono automatic" : "mono off");
                auto params = defaultParams (60.0f, 3);
                params.claySettings.monoMode = mode;
                Rig rig (clayConstants(), normal, params);
                rig.chain.restoreGlazeStateXml (saved.get());
                rig.run (2.0);

                const auto planned = rig.probe().clayPlanned;
                expect (rig.probe().clayAutomaticSupported, tag + ": a format-2 profile supports automatic Clay");
                expectEquals (planned.monoAmount, 0.0f, tag + ": the automatic plan never monos (ADR-015)");
                expect (! rig.probe().clayGuard.phaseActive && ! rig.status().clayPhaseGuardActive, tag + ": in-phase bass, no warning");

                rig.source = antiPhase;
                rig.sourceIndex = 0;
                bool monoUntouched = true;
                const auto engaged = rig.runUntil (1.5, [&]
                {
                    monoUntouched = monoUntouched && rig.probe().liveClay.monoAmount == 0.0f && rig.chain.getEffectiveParams().clay.monoAmount == 0.0f;
                    return rig.probe().clayGuard.phaseActive && rig.probe().clayGuard.phaseFloor >= 0.99f;
                });
                expect (engaged, tag + ": anti-phase bass brings the warning to full level within 1.5 s");
                expect (monoUntouched, tag + ": ADR-015, the mono amount stays 0 (automatic and ClayModule) on every block");

                if (mode == ClayMode::automatic)
                    expect (rig.status().clayPhaseGuardActive, tag + ": the status lamp shows the warning");
                else
                    expect (! rig.status().clayPhaseGuardActive, tag + ": the lamp only warns for an automatic mono item");

                rig.source = normal;
                rig.sourceIndex = 0;
                rig.run (1.0);
                expect (rig.probe().clayGuard.phaseFloor >= 0.99f, tag + ": held after the condition clears (2 s hold)");
                const auto released = rig.runUntil (8.0, [&]
                {
                    return ! rig.probe().clayGuard.phaseActive && rig.probe().liveClay.monoAmount == planned.monoAmount;
                });
                expect (released, tag + ": then fades out; now " + describe (rig.probe().liveClay));
            }

            // b) 宽度。
            {
                Rig rig (clayConstants(), normal, defaultParams (60.0f, 3));
                rig.chain.restoreGlazeStateXml (saved.get());
                rig.run (2.0);
                const auto planned = rig.probe().clayPlanned;
                expect (planned.widthDb > 0.0f, "positive control: this narrow profile plans widening: " + describe (planned));
                expect (! rig.probe().clayGuard.widthActive && sameBits (rig.probe().liveClay.widthDb, planned.widthDb),
                        "mono highs: widening passes unchanged");

                rig.source = wide;
                rig.sourceIndex = 0;
                const auto capped = rig.runUntil (3.0, [&]
                {
                    return rig.probe().clayGuard.widthActive && rig.probe().liveClay.widthDb <= 0.02f;
                });
                expect (capped, "wide highs cap automatic widening to about 0 within 3 s; now " + describe (rig.probe().liveClay));
                expect (rig.chain.getEffectiveParams().clay.widthDb <= 0.02f && rig.status().clayWidthGuardActive,
                        "ClayModule gets the capped value and the lamp reports it");
                expect (sameBits (rig.probe().clayPlanned.widthDb, planned.widthDb), "the plan itself is unchanged (the protection is live only)");

                rig.source = normal;
                rig.sourceIndex = 0;
                const auto reopened = rig.runUntil (6.0, [&]
                {
                    return ! rig.probe().clayGuard.widthActive && sameBits (rig.probe().liveClay.widthDb, planned.widthDb);
                });
                expect (reopened, "narrow highs again: widening returns to the planned value; now " + describe (rig.probe().liveClay));
            }
        }

        // Clay 7. 执行快照：保存 liveClay（保护之后实际执行的自动值）、方案里过渡混合后的 plan.clay 与 ClayGuard 状态；读回（实时 /
        //    离线各一条）后第一块逐位用快照值（反相提示灯照样亮），之后提示从快照的保持期接着走（不是从中性重来），两条完全一致；
        //    ADR-015：提示不改收单，快照里与读回后的收单都是方案值 0；
        //    宿主 reset 也从同一执行量冷启动；旧快照缺字段 = 中性、照样能用；坏值整份拒收。
        // 依据：Clay 第 7 节「执行快照加存 Clay 四个实际值与保护状态……旧快照缺字段按 0（中性）读」；任务书「恢复后首块用快照值」；
        //    设计规格-GLAZE 第 7.3 节（同一快照 + 同一输入起点 + 同一块时间线逐样本可复现）。
        void testClaySnapshotRoundTrip()
        {
            beginTest ("execution snapshot keeps the executed Clay values and guard state; the first restored block uses them");

            const auto normal = makeClayMaterial (false, false, 74);
            const auto antiPhase = makeClayMaterial (true, false, 74);
            Rig learned (clayConstants(), normal, defaultParams (0.0f, 3));
            learned.run (4.0);
            const auto params = defaultParams (60.0f, 3);

            // 捐赠者：反相提示满强度后切回正常素材 0.5 s，停在保持期里（状态里有提示强度、剩余保持时间、提示标志）。
            Rig donor (clayConstants(), antiPhase, params);
            donor.chain.restoreGlazeStateXml (glaze::toXml (clayStateFrom (learned.committed())).get());
            const auto engaged = donor.runUntil (3.0, [&] { return donor.probe().clayGuard.phaseFloor >= 0.99f; });
            donor.source = normal;
            donor.sourceIndex = 0;
            donor.run (0.5);
            const auto heardClay = donor.chain.getEffectiveParams().clay;
            const auto heardLive = donor.probe().liveClay;
            const auto heardPlanned = donor.probe().clayPlanned;
            const auto heardGuard = donor.probe().clayGuard;
            expect (engaged && heardGuard.phaseActive && heardGuard.phaseFloor >= 0.99f && heardLive.monoAmount == 0.0f,
                    "the donor is inside the phase-warning hold when saved (mono untouched); level " + juce::String (heardGuard.phaseFloor, 4)
                    + ", hold left " + juce::String (heardGuard.phaseHoldSeconds, 4) + " s");

            const auto xml = donor.chain.createGlazeStateXml();
            const auto* executionXml = xml->getChildByName (glaze::executionTag);
            glaze::ExecutionState execution;
            expect (executionXml != nullptr && glaze::fromXml (*executionXml, execution), "the execution snapshot parses");
            expect (sameClayPlan (execution.liveClay, heardLive) && sameClayGuard (execution.clayGuard, heardGuard)
                    && sameClayPlan (execution.plan.clay, heardPlanned), "executed Clay, guard state and planned Clay are saved bit-exactly");

            // 读回：实时 / 离线两条。
            Rig a (clayConstants(), normal, params), b (clayConstants(), normal, params);
            a.chain.restoreGlazeStateXml (xml.get());
            b.chain.restoreGlazeStateXml (xml.get());
            b.host.offline = true;
            a.step();
            b.step();
            expect (a.probe().executionSnapshotActive && a.probe().clayRestoredFirstBlock, "the first block after the restore uses the snapshot");
            expect (sameClayParams (a.chain.getEffectiveParams().clay, heardClay), "first block: the Clay parameters heard before saving, bit for bit");
            expect (sameClayPlan (a.probe().liveClay, heardLive) && sameClayGuard (a.probe().clayGuard, heardGuard), "... with the guard state restored");
            expect (a.status().clayPhaseGuardActive, "the phase lamp is lit straight after reopening");
            a.step();
            b.step();
            expect (! a.probe().clayRestoredFirstBlock && a.probe().clayGuard.phaseFloor >= 0.99f && a.status().clayPhaseGuardActive,
                    "the warning resumes inside its hold (not from neutral)");

            bool paramsEqual = true, releasedAfterRestore = false;
            float audioDifference = 0.0f;
            for (int block = 0; block < a.blocksFor (6.0); ++block)
            {
                a.step();
                b.step();
                paramsEqual = paramsEqual && sameParams (a.chain.getEffectiveParams(), b.chain.getEffectiveParams())
                           && sameClayParams (a.chain.getEffectiveParams().clay, b.chain.getEffectiveParams().clay);

                for (int ch = 0; ch < numChannels; ++ch)
                    for (int i = 0; i < blockSize; ++i)
                        audioDifference = juce::jmax (audioDifference, std::abs (a.buffer.getSample (ch, i) - b.buffer.getSample (ch, i)));

                releasedAfterRestore = releasedAfterRestore || ! a.probe().clayGuard.phaseActive;
            }
            expect (paramsEqual, "realtime and offline restores continue with identical parameters, Clay included");
            expectEquals (audioDifference, 0.0f, "... and sample-identical output");
            expect (releasedAfterRestore, "the restored hold runs out and the warning fades out");

            // 宿主 reset 沿用同一执行量：reset 后第一块的 Clay = reset 前最后一块的。
            const auto beforeResetLive = a.probe().liveClay;
            const auto beforeResetClay = a.chain.getEffectiveParams().clay;
            a.chain.reset();
            a.step();
            expect (a.probe().clayRestoredFirstBlock && sameClayPlan (a.probe().liveClay, beforeResetLive)
                    && sameClayParams (a.chain.getEffectiveParams().clay, beforeResetClay),
                    "a host reset cold-starts Clay from the last executed block");

            // 旧快照（没有任何 Clay 属性）：缺 = 中性，其余执行量照用。
            auto legacyXml = std::make_unique<juce::XmlElement> (*xml);
            auto* legacyExecution = legacyXml->getChildByName (glaze::executionTag);
            expect (legacyExecution != nullptr);
            if (legacyExecution == nullptr)
                return;
            for (const auto* attribute : { "liveClayLowCutHz", "liveClayMonoHz", "liveClayMonoAmount", "liveClayWidthDb",
                                           "liveClayCostLimitedMask",
                                           "clayGuardPhaseFloor", "clayGuardPhaseHoldSeconds", "clayGuardWidthCapDb",
                                           "clayGuardPhaseActive", "clayGuardWidthActive" })
                legacyExecution->removeAttribute (attribute);
            for (const auto* attribute : { "clayLowCutHz", "clayMonoHz", "clayMonoAmount", "clayWidthDb", "clayCostLimitedMask" })
                legacyExecution->getChildByName ("PLAN")->removeAttribute (attribute);
            glaze::ExecutionState legacy;
            expect (glaze::fromXml (*legacyExecution, legacy), "an older snapshot without Clay attributes still parses");
            expect (isNeutralClayPlan (legacy.liveClay) && isNeutralClayPlan (legacy.plan.clay)
                    && sameClayGuard (legacy.clayGuard, glaze::ClayGuardState {}), "missing Clay fields read as neutral");
            Rig older (clayConstants(), normal, params);
            older.chain.restoreGlazeStateXml (legacyXml.get());
            older.step();
            expect (older.probe().executionSnapshotActive, "the rest of the older snapshot is still used");
            expect (isNeutralClay (older.chain.getEffectiveParams().clay) && ! older.probe().clayGuard.phaseActive,
                    "its first block restores neutral Clay values and a neutral guard");

            beginTest ("malformed Clay values in an execution snapshot are rejected");
            const auto rejects = [] (const glaze::ExecutionState& damaged)
            {
                glaze::ExecutionState ignored;
                return ! glaze::fromXml (*glaze::toXml (damaged), ignored);
            };
            expect (! rejects (execution), "positive control: the untouched snapshot parses");
            auto bad = execution;
            bad.liveClay.monoAmount = 1.5f;
            expect (rejects (bad), "mono amount above 1");
            bad = execution;
            bad.liveClay.lowCutHz = 5.0f;
            expect (rejects (bad), "a low cut between off and 10 Hz");
            bad = execution;
            bad.plan.clay.widthDb = std::numeric_limits<float>::quiet_NaN();
            expect (rejects (bad), "a non-finite planned width");
            bad = execution;
            bad.plan.clay.monoHz = 1000.0f;
            expect (rejects (bad), "a crossover beyond 300 Hz");
            bad = execution;
            bad.clayGuard.phaseFloor = -0.5f;
            expect (rejects (bad), "a negative phase warning level (phaseFloor)");
            bad = execution;
            bad.clayGuard.widthCapDb = std::numeric_limits<float>::infinity();
            expect (rejects (bad), "an infinite width cap");
            auto corrupted = glaze::toXml (execution);
            corrupted->setAttribute ("liveClayWidthDb", "oops");
            glaze::ExecutionState ignored;
            expect (! glaze::fromXml (*corrupted, ignored), "a present but corrupted Clay attribute is not an older snapshot");
        }

        // Clay 7. 校准参考条件的 manualParamsHash 并入 Clay 三态设置（Clay 第 6 节「计入 manualParamsHash，改了手动值校准标待验证」）：
        //    - 默认（三项自动、没旁通）时哈希与 v0.1.6.2 逐位相同（与照抄的旧实现对照）——旧工程存档里的校准重开后照样适用；
        //    - 改任一项的态、「手动」项的旋钮值或 Bypass → 哈希变、已提交的校准不适用（待验证）；各种设置的哈希互不相同；
        //      改回默认 → 同一哈希、校准重新适用（不开新一轮）；
        //    - 「自动」态下拖旋钮不生效 → 哈希不变、校准仍适用。
        void testClaySettingsJoinCalibrationReference()
        {
            beginTest ("Clay settings join the calibration reference; the default settings keep the v0.1.6.2 hash");

            Rig rig (fastConstants(), makeMusic (defaultRate, 8.0, 62, 0.4f));
            rig.params.glazePercent = 60.0f;
            expect (rig.learnAndCalibrate(), "learned and calibrated with the default Clay settings");

            const auto reference = rig.probe().manualParamsHash;
            expectEquals ((juce::int64) reference, (juce::int64) legacyManualParamsHash (rig.params, rig.probe().feedbackMaxGainDb),
                          "default Clay settings: the manual-parameter hash equals v0.1.6.2's");
            expectEquals ((juce::int64) rig.committed().calibration.reference.manualParamsHash, (juce::int64) reference,
                          "the committed calibration was measured under that hash");
            const auto begins = rig.probe().calibrationBegins;

            ClaySettings lowCutManual;   lowCutManual.lowCutMode = ClayMode::manual;
            auto lowCutManualOther = lowCutManual;
            lowCutManualOther.lowCutHz = 30.0f;
            ClaySettings monoManual;     monoManual.monoMode = ClayMode::manual;
            ClaySettings widthManual;    widthManual.widthMode = ClayMode::manual;
            ClaySettings lowCutOff;      lowCutOff.lowCutMode = ClayMode::off;
            ClaySettings monoOff;        monoOff.monoMode = ClayMode::off;
            ClaySettings widthOff;       widthOff.widthMode = ClayMode::off;
            ClaySettings bypassed;       bypassed.bypass = true;
            const std::vector<std::pair<const char*, ClaySettings>> variants {
                { "low cut manual", lowCutManual }, { "low cut manual at another value", lowCutManualOther },
                { "mono manual", monoManual }, { "width manual", widthManual }, { "low cut off", lowCutOff },
                { "mono off", monoOff }, { "width off", widthOff }, { "bypass", bypassed } };

            std::vector<std::uint32_t> seen { reference };
            for (const auto& variant : variants)
            {
                const auto label = juce::String (variant.first);
                rig.params.claySettings = variant.second;
                rig.step();
                const auto hash = rig.probe().manualParamsHash;
                expect (std::find (seen.begin(), seen.end(), hash) == seen.end(), label + ": a distinct reference hash");
                seen.push_back (hash);
                expect (! rig.status().calibrationValid, label + ": the stored calibration no longer applies (pending)");
            }

            rig.params.claySettings = ClaySettings {};
            rig.step();
            expectEquals ((juce::int64) rig.probe().manualParamsHash, (juce::int64) reference, "back to the defaults: the same hash");
            expect (rig.status().calibrationValid, "... and the stored calibration applies again");

            rig.params.claySettings.lowCutHz = 45.0f;   // 「自动」态：旋钮不生效
            rig.params.claySettings.monoHz = 200.0f;
            rig.params.claySettings.widthDb = -2.5f;
            rig.step();
            expectEquals ((juce::int64) rig.probe().manualParamsHash, (juce::int64) reference,
                          "dragging the knobs of automatic items does not change the hash");
            expect (rig.status().calibrationValid, "... nor invalidate the calibration");
            expectEquals (rig.probe().calibrationBegins, begins, "no calibration round was started by any of this");
        }

        // Clay 8. 音频线程零分配：Clay 的各条路径——自动 + 反相提示 + 宽度保护、三态切换（手动 / 关 / 自动）、GLAZE 0 下的手动项、
        //    换风格过渡、宿主旁通、宿主 reset、读取存档后的首块（执行快照）、读取旧格式画像——setHostInfo / setParams / process /
        //    getStatus 都不经 operator new（存档的生成与解析在计数区外：那是消息线程的事）。
        // 依据：设计规格-总体「音频线程零分配、零锁」；Clay 子文档第 4 节硬规则。
        void testClayAudioThreadDoesNotAllocate()
        {
            beginTest ("every Clay path through the controller is allocation-free on the audio thread");

            const auto normal = makeClayMaterial (false, false, 81);
            const auto antiPhase = makeClayMaterial (true, false, 81);
            const auto wide = makeClayMaterial (false, true, 81);
            Rig learned (clayConstants(), normal, defaultParams (0.0f, 3));
            learned.run (4.0);
            const auto state = clayStateFrom (learned.committed());
            auto oldState = state;
            oldState.profile.formatVersion = 1;
            oldState.profileId = glaze::computeProfileId (oldState.profile);
            const auto saved = glaze::toXml (state);
            const auto old = glaze::toXml (oldState);

            Rig rig (clayConstants(), normal, defaultParams (60.0f, 3));
            rig.chain.restoreGlazeStateXml (saved.get());

            const auto totalBlocks = rig.blocksFor (24.0);
            std::unique_ptr<juce::XmlElement> snapshot;
            std::uint64_t allocations = 0;
            bool finite = true, sawPhaseGuard = false, sawWidthGuard = false, sawRestoredBlock = false, sawTransition = false;

            for (int b = 0; b < totalBlocks; ++b)
            {
                // ---- 计数区外：准备这一块（换素材、改设置、存档的生成与解析）----
                auto& clay = rig.params.claySettings;
                if (b == rig.blocksFor (2.0))  { rig.source = antiPhase; rig.sourceIndex = 0; }
                if (b == rig.blocksFor (5.0))  { rig.source = wide; rig.sourceIndex = 0; }
                if (b == rig.blocksFor (8.0))  { clay.lowCutMode = clay.monoMode = clay.widthMode = ClayMode::manual; clay.widthDb = -1.0f; }
                if (b == rig.blocksFor (9.0))  clay.lowCutMode = clay.monoMode = clay.widthMode = ClayMode::off;
                if (b == rig.blocksFor (10.0)) clay = ClaySettings {};
                if (b == rig.blocksFor (11.0)) { rig.params.glazePercent = 0.0f; clay.lowCutMode = ClayMode::manual; }
                if (b == rig.blocksFor (12.0)) { rig.params.glazePercent = 60.0f; clay = ClaySettings {}; rig.source = normal; rig.sourceIndex = 0; }
                if (b == rig.blocksFor (13.0)) rig.params.glazeStyle = 1;
                if (b == rig.blocksFor (15.0)) rig.hostBypassed = true;
                if (b == rig.blocksFor (16.0)) rig.hostBypassed = false;
                if (b == rig.blocksFor (18.0)) snapshot = rig.chain.createGlazeStateXml();
                if (b == rig.blocksFor (19.0)) rig.chain.restoreGlazeStateXml (snapshot.get());
                if (b == rig.blocksFor (21.0)) rig.chain.restoreGlazeStateXml (old.get());

                const auto length = (size_t) rig.source.front().size();
                rig.buffer.setSize (numChannels, blockSize, false, false, true);
                for (int ch = 0; ch < numChannels; ++ch)
                    for (int i = 0; i < blockSize; ++i)
                        rig.buffer.setSample (ch, i, rig.source[(size_t) ch][(rig.sourceIndex + (size_t) i) % length]);
                rig.sourceIndex = (rig.sourceIndex + (size_t) blockSize) % length;
                rig.host.positionSamples = rig.position;
                const auto resetHere = b == rig.blocksFor (17.0);

                // ---- 计数区：只夹住音频线程的入口（与界面线程的 getStatus）----
                const auto before = allocationCountOnThisThread();

                if (resetHere)
                    rig.chain.restartWithFadeIn();

                rig.chain.setHostInfo (rig.host);
                rig.chain.setParams (rig.params, rig.hostBypassed);
                rig.chain.process (rig.buffer);
                juce::ignoreUnused (rig.chain.getGlazeStatus());

                allocations += allocationCountOnThisThread() - before;

                rig.position += blockSize;
                const auto probe = rig.probe();
                sawPhaseGuard = sawPhaseGuard || probe.clayGuard.phaseActive;
                sawWidthGuard = sawWidthGuard || probe.clayGuard.widthActive;
                sawRestoredBlock = sawRestoredBlock || probe.clayRestoredFirstBlock;
                sawTransition = sawTransition || probe.transitionActive;

                for (int ch = 0; ch < numChannels; ++ch)
                    for (int i = 0; i < blockSize; ++i)
                        finite = finite && std::isfinite (rig.buffer.getSample (ch, i));
            }

            expect (finite, "output finite throughout");
            expect (sawPhaseGuard && sawWidthGuard && sawRestoredBlock && sawTransition,
                    "the run went through the phase warning, the width guard, a restored first block and a plan transition");
            expectEquals ((juce::int64) allocations, (juce::int64) 0, "the audio thread must not allocate on any Clay path");
        }

        // Clay 9.「先试后用」的代价 mask 经编排器到状态快照（ADR-012；设计规格-Clay-根治修订 3.3 / 3.5）。画像 withCostLimitedClay
        //    （低切代价 0.4 dB → 减量、加宽代价 0.8 dB → 否决）：
        //    a) GLAZE 60、三项自动：Status.clayCostLimitedMask == 决策器直接算的方案 mask，且含低切、加宽两位（阳性对照：这份画像
        //       给得出）；探针 clayPlanned 带着同一个 mask（收拾时没丢）；实际值：低切仍 > 0（减量不是否决）、加宽为 0（否决）；
        //    b) 只报「自动」态：低切改「手动」、加宽改「关」→ 两位都不报；改回自动又报；
        //    c) GLAZE 0 → 不报；回到 60 又报；
        //    d) 旧算法方案（Clay 中性、mask 0）→ 拧 GLAZE 按新算法重算：过渡中逐块 = blendClayPlans(起点, 目标, t) 的 mask
        //      （t = 0 那一块取起点的 0，之后取目标的），与 Clay 4 同一个 t 的式子。
        // 依据：GlazeController.cpp 文件头 ADR-012 一节；GlazeController.h blendClayPlans 注释。
        void testClayCostMaskReachesStatus()
        {
            beginTest ("the try-before-use cost mask reaches the status: automatic items only, GLAZE > 0 only, the target plan's mask in transitions");

            const auto material = makeClayMaterial (false, false, 91);
            Rig learned (clayConstants(), material, defaultParams (0.0f, 3));
            learned.run (4.0);
            expect (learned.committed().hasProfile);

            auto state = learned.committed();
            state.profile = withCostLimitedClay (state.profile);
            state.profileId = glaze::computeProfileId (state.profile);
            state.plan = glaze::Plan {};
            state.calibration = glaze::Calibration {};

            const auto lowCutBit = (int) glaze::clayCostLimitedLowCut;
            const auto monoBit = (int) glaze::clayCostLimitedMono;
            const auto widthBit = (int) glaze::clayCostLimitedWidth;

            // a) 三项自动。
            Rig rig (clayConstants(), material, defaultParams (60.0f, 3));
            rig.chain.restoreGlazeStateXml (glaze::toXml (state).get());
            rig.run (1.0);
            const auto planned = planFor (rig.committed(), glaze::Style::clean, 60.0f, rig.params, rig.probe().feedbackMaxGainDb).clay;
            const auto plannedMask = (int) planned.costLimitedMask;
            expect ((plannedMask & lowCutBit) != 0 && (plannedMask & widthBit) != 0,
                    "positive control: the planner marks the reduced low cut and the vetoed widening; mask " + juce::String (plannedMask));
            expectEquals ((int) rig.status().clayCostLimitedMask, plannedMask, "the status reports the plan's mask");
            expectEquals ((int) rig.probe().clayPlanned.costLimitedMask, plannedMask, "the planned automatic values carry the mask");
            {
                const auto& executed = rig.chain.getEffectiveParams().clay;
                expect (executed.lowCutHz > 0.0f && executed.widthDb == 0.0f,
                        "a reduced low cut still acts, a vetoed widening is 0: " + describe (executed));
            }

            // b) 只报自动态。
            rig.params.claySettings.lowCutMode = ClayMode::manual;
            rig.params.claySettings.widthMode = ClayMode::off;
            rig.step();
            expectEquals ((int) rig.status().clayCostLimitedMask, plannedMask & monoBit, "manual / off items are not reported");
            rig.params.claySettings = ClaySettings {};
            rig.step();
            expectEquals ((int) rig.status().clayCostLimitedMask, plannedMask, "back to automatic: reported again");

            // c) GLAZE 0。
            rig.params.glazePercent = 0.0f;
            rig.step();
            expectEquals ((int) rig.status().clayCostLimitedMask, 0, "GLAZE 0: nothing automatic, nothing to explain");
            rig.params.glazePercent = 60.0f;
            rig.step();
            expectEquals ((int) rig.status().clayCostLimitedMask, plannedMask, "GLAZE back up: reported again");

            // d) 过渡中取目标方案的 mask。
            auto older = state;
            older.plan = planFor (state, glaze::Style::clean, 60.0f, defaultParams (60.0f, 3), 1.0e9f);
            older.plan.algorithmVersion = glaze::oldestCompatibleAlgorithmVersion - 1;   // 比兼容下限还旧：照用保存的增量，Clay 中性
            Rig transition (clayConstants(), material, defaultParams (60.0f, 3));
            transition.chain.restoreGlazeStateXml (glaze::toXml (older).get());
            transition.run (0.5);
            expect (transition.probe().savedPlanMode, "fixture: the older plan is kept");
            expectEquals ((int) transition.status().clayCostLimitedMask, 0, "older plan: neutral Clay, nothing to explain");

            transition.params.glazePercent = 61.0f;
            const auto target = planFor (transition.committed(), glaze::Style::clean, 61.0f, transition.params,
                                         transition.probe().feedbackMaxGainDb).clay;
            expect (target.costLimitedMask != 0, "positive control: the target plan is cost-limited");

            constexpr std::int64_t transitionSamples = 24000;   // clayConstants().planTransitionSeconds 0.5 s × 48 kHz
            bool maskFollows = true;
            int firstMismatch = -1;

            for (int k = 0; k < 60; ++k)
            {
                transition.step();
                const auto elapsed = (std::int64_t) k * blockSize;
                const auto expectedMask = elapsed < transitionSamples
                    ? (int) glaze::blendClayPlans (glaze::ClayPlan {}, target,
                                                   (float) juce::jlimit (0.0, 1.0, (double) elapsed / (double) transitionSamples), 10.0f).costLimitedMask
                    : (int) target.costLimitedMask;
                const auto ok = (int) transition.probe().clayPlanned.costLimitedMask == expectedMask
                             && (int) transition.status().clayCostLimitedMask == expectedMask;

                if (! ok && firstMismatch < 0)
                    firstMismatch = k;

                maskFollows = maskFollows && ok;
            }

            expect (maskFollows, "the mask follows the transition (start at t = 0, the target's after); first mismatch at block "
                                 + juce::String (firstMismatch));
            expect (! transition.probe().transitionActive, "the transition has finished");
        }

        // Clay 10. 恢复后第一块只在方案源支持自动 Clay 时才用快照值（审核 V2-03）：手工把执行快照里的画像改成格式 1、liveClay 改成
        //     非中性值（正常存档里不会出现）→ 读回后第一块 Clay 中性、没用快照值、保护中性，之后一直中性（快照值作废，不会在以后
        //     某块冒出来）。阳性对照：同一份存档不改画像格式时，第一块逐位用快照值。
        // 依据：审核 V2-03 建议「clayRestorePending 分支加 && claySupported（不支持时走中性）」；设计规格-Clay 第 7 节「旧工程声音不变」。
        void testClayRestoreNeedsSupport()
        {
            beginTest ("a restored first block uses the snapshot's Clay values only when the plan source supports automatic Clay");

            const auto normal = makeClayMaterial (false, false, 92);
            Rig learned (clayConstants(), normal, defaultParams (0.0f, 3));
            learned.run (4.0);
            const auto params = defaultParams (60.0f, 3);

            Rig donor (clayConstants(), normal, params);
            donor.chain.restoreGlazeStateXml (glaze::toXml (clayStateFrom (learned.committed())).get());
            donor.run (2.0);
            const auto xml = donor.chain.createGlazeStateXml();
            const auto* donorExecution = xml->getChildByName (glaze::executionTag);
            glaze::ExecutionState execution;
            const auto parsed = donorExecution != nullptr && glaze::fromXml (*donorExecution, execution) && execution.valid;
            expect (parsed, "fixture: the donor saved a valid execution snapshot");

            if (! parsed)
                return;

            // 手工塞进的非中性自动值（两份存档共用；逐项赋值，不靠聚合初始化补齐 mask）。
            execution.liveClay.lowCutHz = 30.0f;
            execution.liveClay.monoHz = 120.0f;
            execution.liveClay.monoAmount = 0.8f;
            execution.liveClay.widthDb = 1.0f;

            const auto withExecution = [&xml] (const glaze::ExecutionState& e)
            {
                auto copy = std::make_unique<juce::XmlElement> (*xml);
                copy->removeChildElement (copy->getChildByName (glaze::executionTag), true);
                copy->addChildElement (glaze::toXml (e).release());
                return copy;
            };

            {
                Rig supported (clayConstants(), normal, params);
                supported.chain.restoreGlazeStateXml (withExecution (execution).get());
                supported.step();
                expect (supported.probe().executionSnapshotActive && supported.probe().clayRestoredFirstBlock
                        && sameClayPlan (supported.probe().liveClay, execution.liveClay),
                        "positive control: a format-2 source uses the snapshot on the first block: " + describe (supported.probe().liveClay));
            }

            auto formatOne = execution;
            formatOne.profile.formatVersion = 1;   // 读回时格式 1 的画像把 Clay 特征全部清掉 → 方案源不支持自动 Clay
            Rig rig (clayConstants(), normal, params);
            rig.chain.restoreGlazeStateXml (withExecution (formatOne).get());
            rig.step();
            expect (rig.probe().executionSnapshotActive, "fixture: the rest of the snapshot is adopted");
            expect (! rig.probe().clayAutomaticSupported, "fixture: a format-1 source does not support automatic Clay");
            expect (! rig.probe().clayRestoredFirstBlock && isNeutralClay (rig.chain.getEffectiveParams().clay)
                    && isNeutralClayPlan (rig.probe().liveClay) && sameClayGuard (rig.probe().clayGuard, glaze::ClayGuardState {}),
                    "unsupported source: the first block is neutral, not the snapshot's " + describe (execution.liveClay));

            bool neutral = true;
            rig.run (1.0, [&] (int) { neutral = neutral && isNeutralClay (rig.chain.getEffectiveParams().clay) && ! rig.probe().clayRestoredFirstBlock; });
            expect (neutral, "... and stays neutral (the stale snapshot value never reappears)");
        }

        //==============================================================================
        // T-026 1. 真参数范围上的往返（BUG-004）：随机归一化位置（Clay 三态、各开关也随机）→ 按 JUCE RangedAudioParameter 的换算
        //    往返 1 ～ 8 趟（一次保存重开 = 两趟），参考条件必须一直一致；旧口径哈希在其中一部分位置上确实会翻转（修前丢校准的就是它们）。
        //    旧口径的参考（旧存档读回）按一次重开后的值升格，必须升上去、升完一致。任一计入的旋钮挪一个显示步进，必须不一致。
        //    参考里存的旧口径哈希必须与修前的实现（照抄）逐位相同（含 Clay 三态、高级母带各项随机的各种设置）。
        // 依据：BUG-004；ParameterRanges.h（往返漂移实测）；决策第 6 节「参考条件任一改变即失效」。
        void testReferenceSurvivesParameterRoundTrip()
        {
            beginTest ("reference conditions survive the host's normalised round trip through the real parameter ranges; a real change never does");

            const auto parameters = referenceParameters();
            juce::Random random (0x26b04);
            constexpr int iterations = 60000;
            constexpr int maxTrips = 8;

            int roundTripMismatches = 0, legacyFlips = 0, upgradeChecks = 0, upgradeFailures = 0, changeChecks = 0, changesMissed = 0;
            int oldHashDifferences = 0;
            juce::String firstOldHash, firstRoundTrip, firstUpgrade, firstChange;   // 每类第一例

            const auto noteFailure = [] (juce::String& first, const juce::String& what)
            {
                if (first.isEmpty())
                    first = what;
            };

            for (int iteration = 0; iteration < iterations; ++iteration)
            {
                ChainParams p;

                for (const auto& parameter : parameters)
                    parameter.field (p) = fromNormalised (parameter.range, random.nextFloat());

                p.balance.speed = (BalanceModule::Speed) random.nextInt (3);
                p.balance.bypass = random.nextInt (8) == 0;
                p.color.bypass = random.nextInt (8) == 0;
                p.limit.autoRelease = random.nextBool();
                p.limit.bypass = random.nextInt (8) == 0;
                p.claySettings.lowCutMode = (ClayMode) random.nextInt (3);
                p.claySettings.monoMode = (ClayMode) random.nextInt (3);
                p.claySettings.widthMode = (ClayMode) random.nextInt (3);
                p.claySettings.bypass = random.nextInt (8) == 0;

                // v0.1.9.0 高级母带（链按参数与 GLAZE 算好的实际值交给编排器；不是往返的对象，重开前后不变）：一半的位置开着。
                if (random.nextBool())
                {
                    p.balance.harshAmount = random.nextBool() ? 0.0f : random.nextFloat();
                    p.balance.transientPreserve = random.nextFloat();
                    p.balance.targetAmount = random.nextBool() ? 0.0f : random.nextFloat();
                    p.balance.hasTargetReference = random.nextBool();
                    p.balance.targetSlopeDbPerOct = random.nextBool() ? -0.75f : -1.5f;

                    for (auto& value : p.balance.targetReferenceDb)
                        value = -60.0f * random.nextFloat();
                }

                // 约束反馈上限不是参数（随存档逐位保存）：不限，或某个随机的有限值。
                const auto cap = random.nextBool() ? 1.0e9f : random.nextFloat() * 12.0f;
                const auto saved = referenceFor (p, cap);

                if (saved.manualParamsHash != preFixManualParamsHash (p, cap))
                {
                    ++oldHashDifferences;
                    noteFailure (firstOldHash, "the old-format hash differs from the pre-fix implementation at iteration " + juce::String (iteration));
                }

                auto reopened = p;

                for (int trip = 1; trip <= maxTrips; ++trip)
                {
                    for (const auto& parameter : parameters)
                        parameter.field (reopened) = roundTrip (parameter.range, parameter.field (reopened));

                    const auto now = referenceFor (reopened, cap);

                    if (! glaze::referenceMatches (saved, now) || ! glaze::referenceMatches (now, saved))
                    {
                        ++roundTripMismatches;
                        noteFailure (firstRoundTrip, "round trip " + juce::String (trip) + ": release " + juce::String (p.limit.releaseMs, 9)
                                     + " -> " + juce::String (reopened.limit.releaseMs, 9));
                    }

                    if (now.manualParamsHash != saved.manualParamsHash)
                        ++legacyFlips;

                    // 旧存档：只有旧口径哈希。一次保存重开（两趟）之后的值必须能把它升上去。
                    if (trip <= 2 && iteration % 8 == 0)
                    {
                        auto legacy = glaze::CalibrationReference {};
                        legacy.profileId = saved.profileId;
                        legacy.style = saved.style;
                        legacy.glazePercent = saved.glazePercent;
                        legacy.manualParamsHash = saved.manualParamsHash;
                        legacy.ceilingDb = saved.ceilingDb;
                        legacy.lookaheadMs = saved.lookaheadMs;
                        legacy.sampleRate = saved.sampleRate;

                        ++upgradeChecks;

                        if (glaze::upgradeLegacyConditions ({ &legacy }, reopened, cap) != 1
                            || legacy.conditionsFormat != glaze::continuousConditionsFormat
                            || ! glaze::referenceMatches (legacy, now))
                        {
                            ++upgradeFailures;
                            noteFailure (firstUpgrade, "legacy upgrade after " + juce::String (trip) + " trips failed");
                        }
                    }
                }

                // 真实改动：随机挑一个此刻计入的旋钮挪一个显示步进。
                for (int attempt = 0; attempt < 8; ++attempt)
                {
                    const auto& parameter = parameters[(size_t) random.nextInt ((int) parameters.size())];

                    if (! counted (parameter, p))
                        continue;

                    auto changed = p;
                    parameter.field (changed) = oneDisplayStep (parameter, parameter.field (p));
                    ++changeChecks;

                    if (glaze::referenceMatches (saved, referenceFor (changed, cap)))
                    {
                        ++changesMissed;
                        noteFailure (firstChange, juce::String (parameter.name) + " moved one display step from " + juce::String (parameter.field (p), 9)
                                     + " but the reference still matches");
                    }

                    // 旧存档参考也不能被手动参数的一次真改动「升格」成一致（候选邻域只覆盖往返漂移）。
                    if (iteration % 8 == 0 && ! parameter.referenceItem)
                    {
                        auto legacy = glaze::CalibrationReference {};
                        legacy.manualParamsHash = saved.manualParamsHash;

                        if (glaze::upgradeLegacyConditions ({ &legacy }, changed, cap) != 0
                            || legacy.conditionsFormat != glaze::legacyConditionsFormat)
                        {
                            ++changesMissed;
                            noteFailure (firstChange, juce::String (parameter.name) + " moved one display step, yet a legacy reference was upgraded");
                        }
                    }

                    break;
                }
            }

            // 越界的坏值（插件里走不到：参数都钳在范围内）：候选组合超上限，不升格、不在音频线程上长时间枚举。
            {
                ChainParams wild;
                wild.limit.releaseMs = 1.0e7f;
                wild.claySettings.lowCutMode = wild.claySettings.monoMode = ClayMode::manual;
                wild.claySettings.lowCutHz = wild.claySettings.monoHz = 1.0e8f;

                auto legacy = referenceFor (wild, 1.0e9f);
                legacy.conditionsFormat = glaze::legacyConditionsFormat;
                expectEquals (glaze::upgradeLegacyConditions ({ &legacy }, wild, 1.0e9f), 0,
                              "out-of-range values exceed the candidate cap: no upgrade (and no long enumeration)");
                expect (legacy.conditionsFormat == glaze::legacyConditionsFormat, "... the reference stays legacy");
            }

            logMessage ("  round trip: " + juce::String (iterations) + " positions x " + juce::String (maxTrips) + " trips, old exact hash flipped "
                        + juce::String (legacyFlips) + " times; legacy upgrades checked " + juce::String (upgradeChecks)
                        + ", real changes checked " + juce::String (changeChecks));

            expectEquals (oldHashDifferences, 0, "manualParamsHash is still the pre-fix hash (v0.1.8.0 / v0.1.9.0), bit for bit. First: " + firstOldHash);
            expectEquals (roundTripMismatches, 0, "the round trip never invalidates the reference. First: " + firstRoundTrip);
            expect (legacyFlips > 0, "the old exact hash does flip on some of these positions (the scenario of BUG-004 is exercised)");
            expectEquals (upgradeFailures, 0, "a legacy reference upgrades after one save and reopen. First: " + firstUpgrade);
            expect (upgradeChecks > 0 && changeChecks > iterations / 2, "enough checks ran");
            expectEquals (changesMissed, 0, "one display step on any counted knob is a real change. First: " + firstChange);
        }

        // T-026 2. 保存重开的整条路径（BUG-004 的原始现象）：Limit Release 与「手动」收单分频点取「一次保存重开后旧口径会翻转」的位置，
        //    校准锁定后存档；新实例按往返两趟后的值读回，第一块起已提交的校准就适用、执行快照照样沿用、执行的偏差与存档一致。
        //    存档改造成 v0.1.8.0 的样子（去掉新口径属性）同样如此——旧口径参考读回后按邻域升格。旋钮真的挪了（+1 %）则两种格式都不适用。
        // 依据：BUG-004；时间线第 6 节「重开工程：不重学，从同一起点重渲染」。
        void testCalibrationSurvivesReopenDrift()
        {
            beginTest ("reopen: drifted log-scale knobs keep the calibration and the execution snapshot, for new and v0.1.8.0 saves (BUG-004)");

            const auto release = findDriftingValue (param::ranges::limitRelease());
            const auto mono = findDriftingValue (param::ranges::clayMono());
            expect (release.found && mono.found, "found knob positions whose old exact hash flips after one save and reopen");

            if (! release.found || ! mono.found)
                return;

            logMessage ("  release " + juce::String (release.saved, 9) + " -> " + juce::String (release.reopened, 9)
                        + " ms, mono " + juce::String (mono.saved, 9) + " -> " + juce::String (mono.reopened, 9) + " Hz");

            const auto material = makeMusic (defaultRate, 8.0, 26, 0.5f);

            Rig original (fastConstants(), material);
            original.params.glazePercent = 60.0f;
            original.params.limit.releaseMs = release.saved;
            original.params.claySettings.monoMode = ClayMode::manual;
            original.params.claySettings.monoHz = mono.saved;
            expect (original.learnAndCalibrate(), "original learns and calibrates with the drifting knob positions");

            const auto xml = original.chain.createGlazeStateXml();
            expect (xml != nullptr, "saved");

            if (xml == nullptr)
                return;

            const auto savedBias = original.probe().executedBiasDb;
            const auto savedCalibration = original.committed().calibration;
            expect (savedCalibration.valid && savedCalibration.reference.conditionsFormat == glaze::continuousConditionsFormat,
                    "the committed calibration is stored in the new format");

            juce::XmlElement legacyXml (*xml);
            stripContinuousConditions (legacyXml);

            // 存档格式：新口径的参考（CALIBRATION、执行快照的 REFERENCE）写口径号 2 与四项，读回逐位相同；四项缺一、口径号不认识 →
            // 整份拒收；旧格式读成旧口径，再写出来与读进去逐字相同（不凭空补新属性）。
            {
                const auto sameConditions = [] (const glaze::CalibrationReference& a, const glaze::CalibrationReference& b)
                {
                    return a.conditionsFormat == b.conditionsFormat && a.manualParamsHash == b.manualParamsHash
                        && a.discreteParamsHash == b.discreteParamsHash && sameBits (a.limitReleaseMs, b.limitReleaseMs)
                        && sameBits (a.clayLowCutHz, b.clayLowCutHz) && sameBits (a.clayMonoHz, b.clayMonoHz);
                };
                const juce::StringArray newAttributes { "discreteParamsHash", "limitReleaseMs", "clayLowCutHz", "clayMonoHz" };

                auto* executionXml = xml->getChildByName (glaze::executionTag);
                expect (executionXml != nullptr && executionXml->getChildByName ("REFERENCE") != nullptr, "the execution snapshot is saved");

                if (executionXml == nullptr || executionXml->getChildByName ("REFERENCE") == nullptr)
                    return;

                for (const auto* element : { xml->getChildByName ("CALIBRATION"), executionXml->getChildByName ("REFERENCE") })
                {
                    auto complete = element != nullptr && element->getIntAttribute ("conditionsFormat") == 2;

                    for (const auto& attribute : newAttributes)
                        complete = complete && element->hasAttribute (attribute);

                    expect (complete, "the new-format reference writes conditionsFormat 2 and its four values");
                }

                glaze::CommittedState parsed;
                expect (glaze::fromXml (*xml, parsed) && sameConditions (parsed.calibration.reference, savedCalibration.reference),
                        "the committed calibration's reference round-trips bit for bit");

                glaze::ExecutionState execution;
                expect (glaze::fromXml (*executionXml, execution) && execution.reference.conditionsFormat == glaze::continuousConditionsFormat
                        && sameBits (execution.reference.limitReleaseMs, release.saved) && sameBits (execution.reference.clayMonoHz, mono.saved)
                        && execution.reference.clayLowCutHz == 0.0f,
                        "the execution snapshot's reference carries the knob values (low cut is automatic, so 0)");

                for (const auto& attribute : newAttributes)
                {
                    juce::XmlElement brokenState (*xml);
                    brokenState.getChildByName ("CALIBRATION")->removeAttribute (attribute);
                    glaze::CommittedState ignoredState;
                    expect (! glaze::fromXml (brokenState, ignoredState), "calibration without " + attribute + " is rejected");

                    juce::XmlElement brokenExecution (*executionXml);
                    brokenExecution.getChildByName ("REFERENCE")->removeAttribute (attribute);
                    glaze::ExecutionState ignoredExecution;
                    expect (! glaze::fromXml (brokenExecution, ignoredExecution), "execution reference without " + attribute + " is rejected");
                }

                for (const auto* format : { "3", "0", "x" })
                {
                    juce::XmlElement unknown (*xml);
                    unknown.getChildByName ("CALIBRATION")->setAttribute ("conditionsFormat", format);
                    glaze::CommittedState ignoredState;
                    expect (! glaze::fromXml (unknown, ignoredState), "conditionsFormat " + juce::String (format) + " is rejected");
                }

                glaze::CommittedState legacyParsed;
                expect (glaze::fromXml (legacyXml, legacyParsed)
                        && legacyParsed.calibration.reference.conditionsFormat == glaze::legacyConditionsFormat
                        && legacyParsed.calibration.reference.manualParamsHash == savedCalibration.reference.manualParamsHash,
                        "a v0.1.8.0 save reads as a legacy reference with the old hash");
                const auto rewritten = glaze::toXml (legacyParsed);
                expect (rewritten->getChildByName ("CALIBRATION")->isEquivalentTo (legacyXml.getChildByName ("CALIBRATION"), false),
                        "a legacy reference is written back word for word (no new attributes appear)");
            }

            auto reopen = [&] (const juce::XmlElement& state, float releaseMs, float monoHz)
            {
                auto rig = std::make_unique<Rig> (fastConstants(), material);
                rig->params = original.params;
                rig->params.limit.releaseMs = releaseMs;
                rig->params.claySettings.monoHz = monoHz;
                rig->chain.restoreGlazeStateXml (&state);
                rig->step();
                return rig;
            };

            for (const auto legacyFormat : { false, true })
            {
                const auto& state = legacyFormat ? legacyXml : *xml;
                const juce::String label = legacyFormat ? "v0.1.8.0 save: " : "new save: ";

                auto drifted = reopen (state, release.reopened, mono.reopened);
                expect (drifted->status().calibrationValid, label + "the committed calibration applies on the first block after reopening");
                expect (drifted->probe().executionSnapshotActive, label + "the execution snapshot is kept, not dropped");
                expect (drifted->probe().clayRestoredFirstBlock, label + "the first block uses the snapshot's Clay values");
                expectEquals (drifted->probe().executedBiasDb, savedBias, label + "the executed calibration bias is the saved one");
                expectEquals (drifted->probe().legacyConditionsUpgrades, legacyFormat ? 3 : 0,
                              label + "legacy references upgraded (calibration, execution, held bias)");

                drifted->run (2.0);
                expect (drifted->status().calibrationValid && drifted->phase() == Phase::locked,
                        label + "still locked with the calibration applied two seconds later");
                expectEquals (drifted->probe().calibrationBegins, 0, label + "no recalibration was started");

                // 刚接到存档就同规格重建（处理器 prepareToPlay：先交参数、后 prepare）：prepare 沿用已听到的执行快照时，拿到的是
                // 还没刷新的存档副本（旧格式时是旧口径），同样要按漂移后的值对上，Clay 保护状态与首块值不丢。
                {
                    auto rebuilt = std::make_unique<Rig> (fastConstants(), material);
                    rebuilt->params = original.params;
                    rebuilt->params.limit.releaseMs = release.reopened;
                    rebuilt->params.claySettings.monoHz = mono.reopened;
                    rebuilt->chain.restoreGlazeStateXml (&state);
                    rebuilt->reprepare (defaultRate);
                    rebuilt->step();
                    expect (rebuilt->probe().clayRestoredFirstBlock, label + "restore then same-spec rebuild: the snapshot's Clay values are still used");
                    expect (rebuilt->status().calibrationValid && rebuilt->probe().executionSnapshotActive,
                            label + "restore then same-spec rebuild: calibration and execution snapshot kept");
                    expectEquals (rebuilt->probe().executedBiasDb, savedBias, label + "restore then same-spec rebuild: the saved bias");
                }

                auto moved = reopen (state, release.saved * 1.01f, mono.reopened);
                expect (! moved->status().calibrationValid, label + "a knob that really moved (release +1 %) does not keep the calibration");
                expect (! moved->probe().executionSnapshotActive, label + "... nor the execution snapshot");
            }
        }

        // T-026 3. 慢速自动化：对数刻度旋钮在动，就不算「条件稳定」——稳定计时照修前逐块精确比，不用参考条件的容差。两种速度：
        //    每块相对 2e-5（单看一块在容差 1e-4 之内）与每块 5e-7（每秒约 4.7e-5，比容差还慢：按容差对起点比会误判成稳定、
        //    校准轮开了又放弃）。扫的过程中都不开新一轮；停下来之后照常开。
        // 依据：决策第 6 节（条件稳定才校准）；BUG-004 修复不改变这条规则（评审指出按容差比会放宽约 300 倍）。
        void testSlowLogKnobSweepIsNotStable()
        {
            beginTest ("a slow log-scale knob sweep is not \"stable conditions\", even slower than the tolerance; stopping it is");

            for (const auto perBlock : { 2.0e-5f, 5.0e-7f })
            {
                const auto label = "sweep x(1 + " + juce::String (perBlock, 7) + ") per block: ";

                Rig rig (fastConstants(), makeMusic (defaultRate, 8.0, 27, 0.5f));
                rig.params.glazePercent = 60.0f;
                expect (rig.learnAndCalibrate(), label + "learned and calibrated");

                const auto begins = rig.probe().calibrationBegins;
                rig.params.limit.releaseMs *= 1.5f;   // 真改动：已提交的校准不再适用
                rig.step();
                expect (! rig.status().calibrationValid, label + "a real release change invalidates the calibration");

                bool startedDuringSweep = false;
                rig.run (6.0, [&] (int)
                {
                    rig.params.limit.releaseMs *= 1.0f + perBlock;
                    startedDuringSweep = startedDuringSweep || rig.probe().calibrationBegins != begins;
                });
                expect (! startedDuringSweep, label + "no calibration round starts while the knob keeps moving");

                const auto startedAfterStop = rig.runUntil (4.0, [&] { return rig.probe().calibrationBegins != begins; });
                expect (startedAfterStop, label + "once the knob stops, the conditions are stable and a new round starts");
            }
        }

        //==============================================================================
        // T-027 1. 参考条件手动参数部分的缓存：makeReference 按输入（samePlanningManual + 约束反馈上限逐位相同）缓存
        //    setManualConditions 的结果，命中时拷贝。同一个编排器逐块改一项——参考条件计入的每一项（各挪一个改得动量化码的量）、
        //    Limit Release 与「手动」Clay 分频点挪 1 ulp、正负零、不计入的 Listen 与「自动」态 Clay 旋钮、Ceiling、v0.1.9.0 高级
        //    母带控制（去刺耳 / 频谱目标，哈希里的「Mast」段）——每项停两块（第一块重算、第二块命中），再原路一步步改回去
        //    （A → B → A：缓存不能停在后来的值上）；每块之后核对：探针的 manualParamsHash 与本块发布的执行快照参考（手动参数
        //    那 6 项）都与不经缓存、按编排器这一块拿到的手动参数与约束反馈上限直接重算（glaze::setManualConditions）的逐位相同。
        //    约束反馈上限单独变（手动参数一项不动）：同一实例读回只改了上限的存档，缓存必须跟着失效。
        // 依据：T-027（输出逐位不变）；BUG-004 / ADR-018 的参考条件口径。
        void testManualConditionsCacheIsBitExact()
        {
            beginTest ("the cached manual part of the reference conditions is bit-identical to recomputing it, block by block (T-027)");

            Rig rig (fastConstants(), makeMusic (defaultRate, 8.0, 271, 0.5f));
            rig.params.glazePercent = 60.0f;
            expect (rig.learnAndCalibrate(), "learned and calibrated");

            const auto sameManualConditions = [] (const glaze::CalibrationReference& a, const glaze::CalibrationReference& b)
            {
                return a.conditionsFormat == b.conditionsFormat && a.manualParamsHash == b.manualParamsHash
                    && a.discreteParamsHash == b.discreteParamsHash && sameBits (a.limitReleaseMs, b.limitReleaseMs)
                    && sameBits (a.clayLowCutHz, b.clayLowCutHz) && sameBits (a.clayMonoHz, b.clayMonoHz);
            };

            int probeChecks = 0, probeMismatches = 0, publishedChecks = 0, publishedMismatches = 0;
            juce::String firstMismatch;

            const auto noteMismatch = [&firstMismatch] (const juce::String& what)
            {
                if (firstMismatch.isEmpty())
                    firstMismatch = what;
            };

            // 走一块，再对照：探针（makeReference 的旧口径哈希）与本块发布的执行快照参考 vs. 不经缓存的重算。
            const auto stepAndCheck = [&] (const juce::String& label)
            {
                rig.step();

                // 编排器这一块拿到的手动参数：链先按 mastering 设置合成 Balance 的高级项（v0.1.9.0）再交给它，合成值原样留在
                // 实际参数里（Ceiling 也可能按编码余量下调，但它不在这部分参考条件里）。
                auto seen = rig.params;
                const auto& effective = rig.chain.getEffectiveParams().balance;
                seen.balance.harshAmount = effective.harshAmount;
                seen.balance.transientPreserve = effective.transientPreserve;
                seen.balance.targetAmount = effective.targetAmount;
                seen.balance.targetSlopeDbPerOct = effective.targetSlopeDbPerOct;
                seen.balance.hasTargetReference = effective.hasTargetReference;
                seen.balance.targetReferenceDb = effective.targetReferenceDb;

                const auto expected = referenceFor (seen, rig.probe().feedbackMaxGainDb);

                ++probeChecks;

                if (rig.probe().manualParamsHash != expected.manualParamsHash)
                {
                    ++probeMismatches;
                    noteMismatch (label + " (probe)");
                }

                const auto xml = rig.chain.createGlazeStateXml();
                const auto* executionXml = xml != nullptr ? xml->getChildByName (glaze::executionTag) : nullptr;
                glaze::ExecutionState execution;

                if (executionXml != nullptr && glaze::fromXml (*executionXml, execution))
                {
                    ++publishedChecks;

                    if (! sameManualConditions (execution.reference, expected))
                    {
                        ++publishedMismatches;
                        noteMismatch (label + " (published execution snapshot)");
                    }
                }
            };

            const auto upOneUlp = [] (float x) { return std::nextafter (x, std::numeric_limits<float>::infinity()); };

            using Change = std::pair<const char*, std::function<void (ChainParams&)>>;
            const std::vector<Change> changes {
                { "balance recover",           [] (ChainParams& p) { p.balance.recover = 20.0f; } },
                { "balance tame",              [] (ChainParams& p) { p.balance.tame = 15.0f; } },
                { "balance brighten",          [] (ChainParams& p) { p.balance.brighten = -10.0f; } },
                { "balance speed",             [] (ChainParams& p) { p.balance.speed = BalanceModule::Speed::fast; } },
                { "balance bypass",            [] (ChainParams& p) { p.balance.bypass = true; } },
                { "color tone",                [] (ChainParams& p) { p.color.toneDb = 1.5f; } },
                { "color glue",                [] (ChainParams& p) { p.color.glue = 30.0f; } },
                { "color heat",                [] (ChainParams& p) { p.color.heat = 25.0f; } },
                { "color mix",                 [] (ChainParams& p) { p.color.mixPercent = 80.0f; } },
                { "color output",              [] (ChainParams& p) { p.color.outputDb = -1.0f; } },
                { "color output -0",           [] (ChainParams& p) { p.color.outputDb = -0.0f; } },
                { "color output +0",           [] (ChainParams& p) { p.color.outputDb = 0.0f; } },
                { "color bypass",              [] (ChainParams& p) { p.color.bypass = true; } },
                { "limit gain",                [] (ChainParams& p) { p.limit.gainDb = 2.0f; } },
                { "limit release",             [] (ChainParams& p) { p.limit.releaseMs = 250.0f; } },
                { "limit release +1 ulp",      [upOneUlp] (ChainParams& p) { p.limit.releaseMs = upOneUlp (p.limit.releaseMs); } },
                { "limit auto release off",    [] (ChainParams& p) { p.limit.autoRelease = false; } },
                { "limit clip",                [] (ChainParams& p) { p.limit.clip = 20.0f; } },
                { "limit bypass",              [] (ChainParams& p) { p.limit.bypass = true; } },
                { "limit ceiling (not hashed)", [] (ChainParams& p) { p.limit.ceilingDb = -0.5f; } },
                { "limit listen (not counted)", [] (ChainParams& p) { p.limit.listen = true; } },
                { "Clay automatic knobs (not counted)",
                                               [] (ChainParams& p) { p.claySettings.lowCutHz = 45.0f; p.claySettings.monoHz = 200.0f;
                                                                     p.claySettings.widthDb = -2.5f; } },
                { "Clay low cut manual",       [] (ChainParams& p) { p.claySettings.lowCutMode = ClayMode::manual; } },
                { "Clay low cut value",        [] (ChainParams& p) { p.claySettings.lowCutHz = 30.0f; } },
                { "Clay low cut +1 ulp",       [upOneUlp] (ChainParams& p) { p.claySettings.lowCutHz = upOneUlp (p.claySettings.lowCutHz); } },
                { "Clay mono manual",          [] (ChainParams& p) { p.claySettings.monoMode = ClayMode::manual; } },
                { "Clay mono value",           [] (ChainParams& p) { p.claySettings.monoHz = 150.0f; } },
                { "Clay mono +1 ulp",          [upOneUlp] (ChainParams& p) { p.claySettings.monoHz = upOneUlp (p.claySettings.monoHz); } },
                { "Clay width manual",         [] (ChainParams& p) { p.claySettings.widthMode = ClayMode::manual; } },
                { "Clay width value",          [] (ChainParams& p) { p.claySettings.widthDb = 1.5f; } },
                { "Clay bypass",               [] (ChainParams& p) { p.claySettings.bypass = true; } },
                { "Clay low cut off",          [] (ChainParams& p) { p.claySettings.lowCutMode = ClayMode::off; } },
                { "Clay mono off",             [] (ChainParams& p) { p.claySettings.monoMode = ClayMode::off; } },
                { "Clay width off",            [] (ChainParams& p) { p.claySettings.widthMode = ClayMode::off; } },
                { "de-harsh manual",           [] (ChainParams& p) { p.mastering.harshMode = HarshMode::manual; } },
                { "de-harsh amount",           [] (ChainParams& p) { p.mastering.harshAmount = 40.0f; } },
                { "transient preserve",        [] (ChainParams& p) { p.mastering.transientPreserve = 70.0f; } },
                { "de-harsh automatic (x GLAZE)", [] (ChainParams& p) { p.mastering.harshMode = HarshMode::automatic; } },
                { "GLAZE (scales automatic de-harsh)", [] (ChainParams& p) { p.glazePercent = 55.0f; } },
                { "target neutral",            [] (ChainParams& p) { p.mastering.targetMode = TargetMode::neutral; } },
                { "target amount",             [] (ChainParams& p) { p.mastering.targetAmount = 60.0f; } },
                { "target warm (slope)",       [] (ChainParams& p) { p.mastering.targetMode = TargetMode::warm; } },
                { "target automatic (style)",  [] (ChainParams& p) { p.mastering.targetMode = TargetMode::automatic; } },
                { "de-harsh off",              [] (ChainParams& p) { p.mastering.harshMode = HarshMode::off; } } };

            std::vector<ChainParams> history { rig.params };
            std::vector<std::uint32_t> forwardHashes { rig.probe().manualParamsHash };

            for (const auto& change : changes)
            {
                change.second (rig.params);
                history.push_back (rig.params);
                stepAndCheck (juce::String (change.first));
                stepAndCheck (juce::String (change.first) + ", next block");
                forwardHashes.push_back (rig.probe().manualParamsHash);
            }

            // 列表末尾的高级母带控制 10 步，每一步都改变了参考条件：这一段确实测到了「Mast」段的缓存失效（不是空跑）。
            constexpr size_t masteringSteps = 10;
            auto masteringChangesHash = forwardHashes.size() > masteringSteps;

            for (auto i = forwardHashes.size() - masteringSteps; masteringChangesHash && i < forwardHashes.size(); ++i)
                masteringChangesHash = forwardHashes[i] != forwardHashes[i - 1];

            expect (masteringChangesHash, "every advanced-mastering step changes the reference hash (the Mast segment is exercised)");

            for (auto i = (int) history.size() - 2; i >= 0; --i)
            {
                rig.params = history[(size_t) i];
                stepAndCheck ("back to step " + juce::String (i));
                stepAndCheck ("back to step " + juce::String (i) + ", next block");
            }

            // 约束反馈上限单独变：手动参数不动，读回只改了上限的存档（存档里按 float 位存十六进制）。两个上限的旧口径哈希必须不同，
            // 否则这一段测不出缓存失效；改回第一个上限，哈希回到第一个。
            const auto bitsHex = [] (float value)
            {
                std::uint32_t bits = 0;
                std::memcpy (&bits, &value, sizeof (bits));
                return juce::String::toHexString ((juce::int64) bits).paddedLeft ('0', 8);
            };

            std::vector<std::uint32_t> capHashes;

            for (const auto cap : { 18.5f, 21.25f, 18.5f })
            {
                const auto xml = rig.chain.createGlazeStateXml();
                auto* controllerNode = xml != nullptr ? xml->getChildByName ("GLAZE_CONTROLLER") : nullptr;
                expect (controllerNode != nullptr, "the controller state is saved");

                if (controllerNode == nullptr)
                    return;

                controllerNode->setAttribute ("feedbackMaxGainDb", bitsHex (cap));
                rig.chain.restoreGlazeStateXml (xml.get());
                stepAndCheck ("constraint cap " + juce::String (cap));
                expect (sameBits (rig.probe().feedbackMaxGainDb, cap), "the restored constraint cap " + juce::String (cap) + " is in effect");
                capHashes.push_back (rig.probe().manualParamsHash);
                stepAndCheck ("constraint cap " + juce::String (cap) + ", next block");
            }

            expect (capHashes.size() == 3 && capHashes[0] != capHashes[1] && capHashes[2] == capHashes[0],
                    "the two constraint caps give different hashes, and going back gives the first one again");

            logMessage ("  " + juce::String (probeChecks) + " blocks checked through the probe, " + juce::String (publishedChecks)
                        + " through the published execution snapshot");

            expectEquals (probeMismatches, 0, "the probe's hash equals the uncached recomputation. First: " + firstMismatch);
            expectEquals (publishedMismatches, 0, "the published reference equals the uncached recomputation. First: " + firstMismatch);
            expect (publishedChecks >= probeChecks / 2, "most blocks published an execution snapshot to compare");
        }

        //==============================================================================
        // ---- T-032（2026-09-29 深度研究 R5-01 / 02 / 03 / 04 / 06 / 08、R6-04、核验 M1 / M2；实施 A6；
        //      报告 docs/validation/2026-09-29-T032-deep-research/IMPL-A6-glaze-controller.md）----

        // 两份风格常数逐字段逐位相同（保护层与上游监视拿到的是整份混合后的常数）。
        static bool sameStyleConstants (const glaze::StyleConstants& a, const glaze::StyleConstants& b) noexcept
        {
            return sameBits (a.preferredLufs, b.preferredLufs) && sameBits (a.heatDriveCap, b.heatDriveCap)
                && sameBits (a.glueGrMeanCapDb, b.glueGrMeanCapDb) && sameBits (a.glueGrP95CapDb, b.glueGrP95CapDb)
                && sameBits (a.clipDepthCapDb, b.clipDepthCapDb) && sameBits (a.clipActivityCap, b.clipActivityCap)
                && sameBits (a.limitGrMeanCapDb, b.limitGrMeanCapDb) && sameBits (a.limitGrP95CapDb, b.limitGrP95CapDb)
                && sameBits (a.limitGrPeakCapDb, b.limitGrPeakCapDb) && sameBits (a.recoverCap, b.recoverCap)
                && sameBits (a.tameCap, b.tameCap) && sameBits (a.toneMaxDb, b.toneMaxDb)
                && sameBits (a.shareHeat, b.shareHeat) && sameBits (a.shareGlue, b.shareGlue)
                && sameBits (a.shareClip, b.shareClip) && sameBits (a.shareLimit, b.shareLimit)
                && sameBits (a.heatPeakReductionCapDb, b.heatPeakReductionCapDb)
                && sameBits (a.toneReferenceSlopeDbPerOct, b.toneReferenceSlopeDbPerOct)
                && sameBits (a.clayLowCutTargetHz, b.clayLowCutTargetHz) && sameBits (a.clayWidthStyleFactor, b.clayWidthStyleFactor);
        }

        // T-032 R5-01（含核验 M1）：放弃的校准轮、换方案丢掉的轮都不算「对同一条件试过（结束为无效）」。校准中把 GLAZE 拧到 0、
        // 或切 Limit Bypass 做 A/B ≥ 3 s（本轮被放弃，期间开不了新一轮），或换风格跑满过渡再切回：条件回到原样后必须再开一轮并锁定为
        // 有效。修前 beginRound 一开轮就记「上次尝试」，放弃 / 换方案丢轮都不清，条件逐位相同 → 开轮门第 512 位永远为真，状态行写
        // 「稳定后自动校准」却永远不校准（这三例修前都停在 gate mask & 512）。
        // 对照：Color Bypass 往返 4 s——它在参考条件里但不挡开轮：第 3 s 放弃的那一块旁通条件已稳定 3 s，当即按旁通条件开了新一轮，
        // 修前也因此覆盖了「上次尝试」，切回后照常再校准。研究以为它也会卡住，核过不会；留作对照（修前修后都应恢复）。
        // 依据：决策第 6 节「任一改变即失效，退回纯前馈，条件稳定后再校准」；GlazeController.h 校准段。
        void testAbandonedRoundIsNotAFailedAttempt()
        {
            beginTest ("an abandoned or dropped calibration round is not a failed attempt: GLAZE 0, Limit bypass and style round trips calibrate again");

            enum class Detour { glazeZero, limitBypass, style, colorBypass };
            struct Case { Detour detour; const char* name; bool blocksNewRounds; };

            for (const auto& c : { Case { Detour::glazeZero,   "GLAZE 0 for 4 s",                true },
                                   Case { Detour::limitBypass, "Limit bypass for 4 s",           true },
                                   Case { Detour::style,       "style warm and back",            true },
                                   Case { Detour::colorBypass, "Color bypass for 4 s (control)", false } })
            {
                const auto tag = juce::String (c.name);
                Rig rig (fastConstants(), makeMusic (defaultRate, 8.0, 301, 0.4f), defaultParams (60.0f, 0));
                expect (rig.runUntil (40.0, [&] { return rig.phase() == Phase::calibrating; }), tag + ": a calibration round began");
                const auto begins = rig.probe().calibrationBegins;
                expectEquals (begins, 1, tag + ": exactly one round so far");
                expect (! rig.committed().calibration.valid, tag + ": nothing is calibrated yet");

                switch (c.detour)
                {
                    case Detour::glazeZero:
                        rig.params.glazePercent = 0.0f;
                        rig.run (4.0);   // > calibrationAbandonSeconds（3 s）：本轮被放弃；GLAZE 0 开不了新一轮
                        rig.params.glazePercent = 60.0f;
                        break;

                    case Detour::limitBypass:
                        rig.params.limit.bypass = true;   // 在参考条件的哈希里；Limit 旁通 = 电平类自动全部撤回，也开不了新一轮
                        rig.run (4.0);
                        rig.params.limit.bypass = false;
                        break;

                    case Detour::colorBypass:
                        rig.params.color.bypass = true;   // 在哈希里，但不挡开轮
                        rig.run (4.0);
                        rig.params.color.bypass = false;
                        break;

                    case Detour::style:
                    default:
                        rig.params.glazeStyle = 1;   // 温润：换方案丢掉本轮；跑满过渡（0.5 s）就切回——新风格下还没稳定满 1 s，不会开轮
                        rig.step();
                        expect (rig.runUntil (0.6, [&] { return ! rig.probe().transitionActive; }), tag + ": the style transition finished");
                        rig.params.glazeStyle = 0;
                        break;
                }

                const auto duringDetour = rig.probe().calibrationBegins - begins;

                if (c.blocksNewRounds)
                {
                    expect (! rig.probe().roundActive, tag + ": the round was abandoned / dropped");
                    expectEquals (duringDetour, 0, tag + ": no round could begin during the detour");
                }
                else
                {
                    expectEquals (duringDetour, 1, tag + ": a round began under the stable bypass when the first one was abandoned");
                }

                const auto recalibrated = rig.learnAndCalibrate (30.0);
                const auto probe = rig.probe();
                logMessage ("  " + tag + ": begins " + juce::String (probe.calibrationBegins) + " (" + juce::String (duringDetour)
                            + " during the detour), finishes " + juce::String (probe.calibrationFinishes) + ", gate mask now "
                            + juce::String (probe.calibrationGateMask) + ", ends " + phaseName (rig.phase())
                            + (rig.status().calibrationValid ? " / valid" : " / not valid"));
                expect (recalibrated, tag + ": back at the original conditions a round runs and locks with a valid calibration");
                expectEquals (probe.calibrationBegins, begins + duringDetour + 1, tag + ": exactly one more round, on the original conditions");
                expectEquals (probe.calibrationGateMask & 512, 0, tag + ": 'tried these conditions' does not block a round that was merely abandoned");
            }
        }

        // T-032 R5-02：保护层（与上游监视）生效的约束上限逐次跟上目标方案：提交后、每次换风格后、重新聆听提交后都等于
        // blendConstants(已提交常数, 目标方案的权重)。修前按「方案槽自己的重算计数」去重，两槽计数每隔一次方案切换撞一次，换风格 / 提交后
        // 有一半几率沿用上一份方案的上限（温润限幅峰值 GR 上限 4.0 dB 对通透 6.0），直到用户碰任一进方案的旋钮。
        // 依据：决策第 4 节「各模块用各自的量约束」、第 7 节（保护层按本方案的上限判定）。
        void testGuardLimitsFollowEveryPlanChange()
        {
            beginTest ("guard and upstream-monitor limits follow the target plan on commit, on every style change and on a relearn (global evaluation counter)");

            Rig rig (fastConstants(), makeMusic (defaultRate, 8.0, 302, 0.4f), defaultParams (70.0f, 0));
            expect (rig.learnAndCalibrate(), "learned and calibrated (automatic style)");

            int stale = 0, checks = 0;

            const auto check = [&] (const juce::String& label)
            {
                const auto probe = rig.probe();
                const auto expected = glaze::blendConstants (rig.committed().constants, probe.targetPlan.weights);
                const auto ok = probe.guardLimitsSet && sameStyleConstants (probe.guardLimits, expected);
                stale += ok ? 0 : 1;
                ++checks;
                expect (ok, label + ": guard limits == blendConstants(committed constants, target plan weights); guard limit GR peak / mean "
                            + juce::String (probe.guardLimits.limitGrPeakCapDb, 2) + " / " + juce::String (probe.guardLimits.limitGrMeanCapDb, 2)
                            + " dB, expected " + juce::String (expected.limitGrPeakCapDb, 2) + " / " + juce::String (expected.limitGrMeanCapDb, 2) + " dB");
                return expected;
            };

            // 换方案源后跑满过渡，再多一块（保护层在 endBlock 里按目标方案设上限）。
            const auto settle = [&] (const juce::String& label)
            {
                rig.step();
                expect (rig.runUntil (1.0, [&] { return ! rig.probe().transitionActive; }), label + ": the plan transition finished");
                rig.step();
            };

            check ("after the commit (automatic)");
            glaze::StyleConstants warmCaps, cleanCaps;

            for (const auto styleIndex : { 1, 3, 2, 1, 0, 3, 2, 0 })
            {
                rig.params.glazeStyle = styleIndex;
                const auto label = "style " + juce::String (styleIndex);
                settle (label);
                const auto caps = check (label);

                if (styleIndex == 1) warmCaps = caps;
                if (styleIndex == 3) cleanCaps = caps;
            }

            // 各风格的上限确实不同（否则上面的核对没有区分度）：温润与通透的限幅峰值 GR 上限不同。
            expect (! sameBits (warmCaps.limitGrPeakCapDb, cleanCaps.limitGrPeakCapDb), "warm and clean limits differ");

            // 重新聆听提交（自动风格：锁定的权重跟着新画像换）：保护层跟上新方案。
            const auto commits = rig.probe().commits;
            rig.chain.requestGlazeRelearn();
            expect (rig.runUntil (20.0, [&] { return rig.probe().commits > commits; }), "the relearn committed a new profile");
            settle ("after the relearn commit");
            check ("after the relearn commit");

            logMessage ("  stale guard limit sets: " + juce::String (stale) + " of " + juce::String (checks) + " checks");
            expectEquals (stale, 0, "never a stale limit set");
        }

        // T-032 R5-03 与核验 M2：「保护层卡住」与约束反馈的观察期都按保护层自己的时间轴（有效音乐、不旁通）计，不按宿主样本时间。
        // ×5 热素材让保护层退让后冻结 8 s（静音 / 宿主旁通 / Color 旁通）：「没变」的计数不走、不吸收、不反馈；冻结结束的头 0.5 s 也不吸收
        //（Color 旁通时校准不适用、吸收不了，修前计数却照走，切回那一块就把退让吸收进方案）。音乐回来后照样按音乐时间判定、吸收。
        // 修前：静音 / 宿主旁通 6 s 就判「卡住」，把一次临时退让永久并进方案上限（只降不升、随工程保存）。
        // M2：吸收之后再静音 8 s，观察期（过渡 + 3 s）不走——修前按宿主时间走完，之后按过渡期装进窗里的旧数据判定。
        // 依据：决策第 7 节「静音、停播不计入恢复时间」；实现修订第 3 节（卡住 = 超过它自己的恢复等待 + 1 s；两次反馈之间至少隔过渡 + 3 s 观察）。
        void testFrozenGuardTimeIsNotHostTime()
        {
            beginTest ("silence, host bypass and Color bypass neither make a retreated guard 'stuck' nor run the feedback observation (guard time, not host time)");

            auto constants = withTightLimitBudgets (fastConstants());   // 保护层要出手（TestHelpers.h 的说明）
            constants.mismatchThresholdLu = constants.mismatchReleaseLu = 1.0e9f;   // 同 12b：要测的是保护层路径
            const auto music = makeMusic (defaultRate, 8.0, 24, 0.2f);
            const auto second = (std::int64_t) defaultRate;

            enum class Freeze { silence, hostBypass, colorBypass };

            for (const auto freeze : { Freeze::silence, Freeze::hostBypass, Freeze::colorBypass })
            {
                const auto tag = juce::String (freeze == Freeze::silence ? "silence" : (freeze == Freeze::hostBypass ? "host bypass" : "Color bypass"));

                const auto setFrozen = [freeze] (Rig& r, bool on)
                {
                    if (freeze == Freeze::silence)
                        r.sourceGain = on ? 0.0f : 5.0f;
                    else if (freeze == Freeze::hostBypass)
                        r.hostBypassed = on;
                    else
                        r.params.color.bypass = on;
                };

                Rig rig (constants, music, defaultParams (100.0f, 2));
                expect (rig.learnAndCalibrate(), tag + ": locks with a valid calibration on the quiet material");

                rig.sourceGain = 5.0f;   // ≈ +14 dB：限幅量超出冲击的上限，保护层退让
                expect (rig.runUntil (10.0, [&] { return rig.probe().guardDb < -0.3f; }), tag + ": the guard retreated on the hot material");
                const auto before = rig.probe();
                expectEquals (before.feedbackSteps, 0, tag + ": no feedback yet when the freeze starts");

                setFrozen (rig, true);
                rig.run (8.0);   // 比「卡住」门槛（恢复等待 5 s + 1 s）长
                const auto frozen = rig.probe();
                logMessage ("  " + tag + ": guard " + juce::String (before.guardDb, 3) + " dB when frozen; after 8 s frozen: steady "
                            + juce::String ((double) frozen.guardSteadySamples / defaultRate, 2) + " s (stuck threshold "
                            + juce::String ((double) frozen.guardStuckSamples / defaultRate, 2) + " s), guard " + juce::String (frozen.guardDb, 3)
                            + " dB, feedback steps " + juce::String (frozen.feedbackSteps) + ", cap " + juce::String (frozen.feedbackMaxGainDb, 2) + " dB");
                expect (frozen.guardSteadySamples < frozen.guardStuckSamples, tag + ": 8 s frozen do not reach the 6 s 'stuck' threshold");
                expect (frozen.guardSteadySamples <= before.guardSteadySamples + second,
                        tag + ": the steady counter froze (within a second: the analyser's music gate closing)");
                expectEquals (frozen.feedbackSteps, 0, tag + ": no constraint feedback of any kind while frozen");
                expect (sameBits (frozen.feedbackMaxGainDb, before.feedbackMaxGainDb), tag + ": the gain cap is untouched while frozen");
                expect (frozen.guardDb < -0.3f, tag + ": the frozen guard keeps its retreat");

                setFrozen (rig, false);
                rig.run (0.5);
                expectEquals (rig.probe().feedbackSteps, 0, tag + ": nothing is absorbed right after the freeze ends");

                // 按音乐时间照常判定：卡住的退让照样被吸收（规格「不无限等待」）。
                const auto absorbed = rig.runUntil (40.0, [&] { return rig.probe().feedbackFromStuckGuard > 0; });
                logMessage ("  " + tag + ": after the freeze: stuck absorptions " + juce::String (rig.probe().feedbackFromStuckGuard)
                            + ", feedback steps " + juce::String (rig.probe().feedbackSteps) + ", cap " + juce::String (rig.probe().feedbackMaxGainDb, 2) + " dB");
                expect (absorbed, tag + ": once the music runs again the stuck retreat is absorbed on guard time");
                rig.run (1.0);   // 吸收型反馈的方案过渡（0.5 s）走完，阶段回到锁定

                if (freeze == Freeze::silence)
                {
                    // 核验 M2：反馈之后的「过渡 + 3 s 观察」只在有效音乐里走（上游监视窗也只在这时加时隙）。
                    const auto observed = rig.probe();
                    rig.sourceGain = 0.0f;
                    rig.run (8.0);
                    const auto quiet = rig.probe();
                    logMessage ("  M2: observation " + juce::String ((double) observed.samplesSinceFeedback / defaultRate, 2) + " s before the silence, "
                                + juce::String ((double) quiet.samplesSinceFeedback / defaultRate, 2) + " s after 8 s of silence");
                    expect (quiet.samplesSinceFeedback <= observed.samplesSinceFeedback + second, "M2: the observation period does not advance in silence");
                    expectEquals (quiet.feedbackSteps, observed.feedbackSteps, "M2: no feedback while silent");
                    rig.sourceGain = 5.0f;
                    rig.run (1.0);
                    expect (rig.probe().samplesSinceFeedback > quiet.samplesSinceFeedback, "M2: the observation period advances again with the music");
                }

                expect (rig.phase() == Phase::locked && rig.status().calibrationValid, tag + ": ends locked with a valid calibration");
            }
        }

        // T-032 R5-04：解除执行快照的冻结（保存重开 / 宿主 reset 沿用执行量之后，第一次拧 GLAZE 或换风格）时，快照里并进冻结方案的保护
        // 退让不再一步丢掉，而是由 carry 接着施加、按方案过渡同长淡出：净增益连续，GLAZE 旋钮仍即时（方案本身不做过渡）。修前净增益
        // 一块内阶跃 |absorbed|（最多 3 dB），换风格时过渡起点同样阶跃。另：carry 淡完之前不开新的校准轮（过渡放长到 3 s 时，解除
        // 冻结 1 s 后用户条件已稳定、校准不适用——没有这一条就在淡出中途开轮采集）。
        // 依据：时间线第 4 节「拧 GLAZE 不重做方案过渡」、第 6 节（reset 与保存重开同一套执行快照）；决策第 6 节「保护层改了实际增益时
        // 子窗作废」；实现修订第 3 节（吸收退让时原退让随过渡淡出，净增益连续）。
        void testReleasingAFrozenExecutionCarriesTheAbsorbedRetreat()
        {
            beginTest ("releasing a frozen execution (reopen / host reset, then GLAZE or style) carries its absorbed guard retreat out over a transition");

            auto constants = withTightLimitBudgets (fastConstants());   // 保护层要出手（TestHelpers.h 的说明）
            constants.mismatchThresholdLu = constants.mismatchReleaseLu = 1.0e9f;   // 要测的是保护层路径，不让画像失配先动手
            const auto music = makeMusic (defaultRate, 8.0, 24, 0.2f);
            const auto params = defaultParams (100.0f, 2);
            const auto blockSeconds = (double) blockSize / defaultRate;
            const auto slewStep = (float) blockSeconds;   // 校准偏差、保护层退让都是 1 dB/s：各自每块最多的变化

            // 供体：锁定后推热，保护层退让超过 0.5 dB（还没有任何约束反馈）。
            const auto retreat = [&] (Rig& donor, const juce::String& tag)
            {
                expect (donor.learnAndCalibrate(), tag + ": the donor locks with a valid calibration on the quiet material");
                donor.sourceGain = 5.0f;
                expect (donor.runUntil (10.0, [&] { return donor.probe().executedGuardDb < -0.5f && donor.probe().feedbackSteps == 0; }),
                        tag + ": the donor's guard retreated by more than 0.5 dB before any feedback");
            };

            // 存档里的执行快照并进冻结方案的退让（adoptExecution：max(−自动 Limit Gain, 保护退让)）。
            const auto absorbedIn = [&] (const juce::XmlElement& xml, const juce::String& tag)
            {
                glaze::ExecutionState saved;
                const auto* executionXml = xml.getChildByName (glaze::executionTag);
                const auto ok = executionXml != nullptr && glaze::fromXml (*executionXml, saved) && saved.valid;
                expect (ok, tag + ": the snapshot carries an execution");
                expect (ok && saved.protectionDb < -0.5f, tag + ": the saved execution carries the retreat: " + juce::String (saved.protectionDb, 4) + " dB");
                return ok ? juce::jmax (-saved.plan.increments.limitGainDb, saved.protectionDb) : 0.0f;
            };

            // 已沿用 2 s 的链（安静素材 ×1：保护层自己不必再动，只看 carry）改条件解除冻结，逐块核对净增益。
            const auto checkRelease = [&] (Rig& rig, float absorbed, const juce::String& tag, float glazeAfter, int styleAfter)
            {
                expect (rig.probe().executionSnapshotActive, tag + ": the execution is held");
                expectWithinAbsoluteError (rig.probe().absorbedGuardDb, absorbed, 1.0e-6f, tag + ": the absorbed retreat is remembered");
                expectEquals (rig.probe().executedGuardDb, 0.0f, tag + ": while held the retreat sits inside the frozen plan, not in the guard");

                const auto committed = rig.committed();
                const auto cap = rig.probe().feedbackMaxGainDb;
                const auto styleBefore = rig.params.glazeStyle;
                const auto fromPlan = planFor (committed, static_cast<glaze::Style> (styleBefore), rig.params.glazePercent, rig.params, cap);
                auto changed = rig.params;
                changed.glazePercent = glazeAfter;
                changed.glazeStyle = styleAfter;
                const auto toPlan = planFor (committed, static_cast<glaze::Style> (styleAfter), glazeAfter, changed, cap);
                const auto transitionSeconds = constants.planTransitionSeconds;
                const auto carryStep = (float) (std::abs (absorbed) * blockSeconds / transitionSeconds);   // carry 每块淡出的量
                const auto transitionStep = styleAfter == styleBefore ? 0.0f   // 换风格：增量每块最多混合多少
                    : (float) (std::abs (toPlan.increments.limitGainDb - fromPlan.increments.limitGainDb) * blockSeconds / transitionSeconds);
                const auto gainBefore = rig.chain.getEffectiveParams().limit.gainDb;

                rig.params = changed;
                rig.step();
                const auto first = rig.probe();
                const auto gainFirst = rig.chain.getEffectiveParams().limit.gainDb;
                const auto mappingDelta = std::abs (first.increments.limitGainDb - fromPlan.increments.limitGainDb);   // GLAZE 映射本身的变化
                logMessage ("  " + tag + ": absorbed " + juce::String (absorbed, 4) + " dB; first released block: gain step "
                            + juce::String (gainFirst - gainBefore, 5) + " dB (mapping " + juce::String (mappingDelta, 5)
                            + "), executed guard " + juce::String (first.executedGuardDb, 4) + " dB");
                expect (! first.executionSnapshotActive, tag + ": the changed conditions release the snapshot");
                expectWithinAbsoluteError (first.executedGuardDb, absorbed, 1.0e-4f, tag + ": the first released block still executes the whole absorbed retreat");
                expect (std::abs (gainFirst - gainBefore) <= mappingDelta + slewStep + 1.0e-4f,
                        tag + ": no step beyond the GLAZE mapping and the bias slew; step " + juce::String (gainFirst - gainBefore, 5));

                auto previous = gainFirst;
                auto largestStep = 0.0f;
                rig.run (transitionSeconds + 0.1, [&] (int)
                {
                    const auto gain = rig.chain.getEffectiveParams().limit.gainDb;
                    largestStep = juce::jmax (largestStep, std::abs (gain - previous));
                    previous = gain;
                });
                const auto after = rig.probe();
                const auto bound = carryStep + 2.0f * slewStep + transitionStep;
                logMessage ("  " + tag + ": largest block step while the carry fades " + juce::String (largestStep, 5) + " dB (bound "
                            + juce::String (bound, 5) + "); carry now " + juce::String (after.guardCarryDb, 5)
                            + " dB, executed guard " + juce::String (after.executedGuardDb, 4) + " dB, gain " + juce::String (previous, 3) + " dB");
                expect (largestStep <= bound + 1.0e-4f, tag + ": the retreat fades at most at |absorbed| / transition (plus the slews); largest step "
                                                         + juce::String (largestStep, 5));
                expectEquals (after.guardCarryDb, 0.0f, tag + ": the carry has faded out completely after one transition length");
                expect (after.executedGuardDb > -0.05f, tag + ": on the quiet material nothing is left retreating once the carry is gone");
            };

            // (1)(2) 保存重开：存档 → 新链读回，沿用 2 s → 拧 GLAZE / 换风格。
            Rig donor (constants, music, params);
            retreat (donor, "reopen");
            const auto heardGain = donor.chain.getEffectiveParams().limit.gainDb;
            const auto xml = donor.chain.createGlazeStateXml();
            const auto absorbed = absorbedIn (*xml, "reopen");

            struct Release { const char* name; float glazeAfter; int styleAfter; };

            for (const auto& release : { Release { "reopen, GLAZE 100 -> 99", 99.0f, 2 }, Release { "reopen, style punchy -> warm", 100.0f, 1 } })
            {
                const auto tag = juce::String (release.name);
                Rig restored (constants, music, params);
                restored.chain.restoreGlazeStateXml (xml.get());
                restored.step();
                expectWithinAbsoluteError (restored.chain.getEffectiveParams().limit.gainDb, heardGain, 1.0e-5f, tag + ": the first block keeps the heard Gain");
                restored.run (2.0);
                checkRelease (restored, absorbed, tag, release.glazeAfter, release.styleAfter);
            }

            // (3) 宿主 reset 沿用执行量：同一条链推热退让后换回安静素材、reset（与保存重开同一个执行快照入口），沿用 2 s → 拧 GLAZE。
            {
                Rig reset (constants, music, params);
                retreat (reset, "host reset");
                const auto snapshot = reset.chain.createGlazeStateXml();   // 与 reset 沿用的执行量同一份（上一块的执行快照）
                const auto resetAbsorbed = absorbedIn (*snapshot, "host reset");
                reset.sourceGain = 1.0f;
                reset.chain.reset();
                reset.step();
                reset.run (2.0);
                checkRelease (reset, resetAbsorbed, "host reset, GLAZE 100 -> 99", 99.0f, 2);
            }

            // (4) carry 淡完之前不开校准轮：过渡放长到 3 s（carry 同长）。读回后 0.5 s（稳定不足 1 s，沿用期间还没开轮）解除冻结；
            //     1.5 s 后用户条件已稳定、校准不适用，只剩 carry 挡着（门掩码 32）；carry 淡完才开轮。
            {
                auto slowFade = constants;
                slowFade.planTransitionSeconds = 3.0;
                Rig gated (slowFade, music, params);
                gated.chain.restoreGlazeStateXml (xml.get());
                gated.step();
                gated.run (0.5);
                expect (gated.probe().executionSnapshotActive && ! gated.probe().roundActive, "fade gate: held, no round yet");
                const auto begins = gated.probe().calibrationBegins;

                gated.params.glazePercent = 99.0f;
                gated.step();
                expect (gated.probe().guardCarryDb < -0.5f, "fade gate: the release starts a carry of " + juce::String (gated.probe().guardCarryDb, 4) + " dB");
                gated.run (1.5);
                const auto waiting = gated.probe();
                logMessage ("  fade gate: 1.5 s after the release carry " + juce::String (waiting.guardCarryDb, 4) + " dB, gate mask "
                            + juce::String (waiting.calibrationGateMask) + ", rounds begun " + juce::String (waiting.calibrationBegins - begins));
                expect (waiting.guardCarryDb < 0.0f, "fade gate: the carry is still fading");
                expectEquals (waiting.calibrationBegins, begins, "fade gate: no round while the carry fades");
                expectEquals (waiting.calibrationGateMask & (32 | 128 | 256), 32, "fade gate: only the carry holds the round back (stable, not applicable)");

                auto carryAtBegin = 1.0f;
                const auto began = gated.runUntil (8.0, [&]
                {
                    const auto p = gated.probe();
                    carryAtBegin = p.guardCarryDb;
                    return p.calibrationBegins > begins;
                });
                expect (began, "fade gate: a round opens once the carry has faded");
                expectEquals (carryAtBegin, 0.0f, "fade gate: the round opens with no carry left");
            }
        }

        // T-032 R5-08：读档与提交两条路径的运行态初始化对称。
        //   (a) 播放中读档：读档前累计的画像失配能量属于旧画像，读回响度低很多的画像后不能拿它去比——修前读档时分析器短期窗重来，
        //       约 0.4 s 后 M 一恢复有效就按旧累计触发、给新方案降上限（不满持续时长）；修后累计从读档那一块重来，满持续时长才触发。
        //       「建议重新聆听」的提示说的也是旧画像，再读回别的画像时一并清掉（修前一直挂着）；
        //   (b) 旧快照的保护层时长不同（守护窗 6 s、恢复等待 10 s）读回后按它折算，重新聆听提交新画像后回到本版本常数
        //      （3 s → 30 格；5 + 1 = 6 s）；修前停在旧快照的 60 格 / 11 s。
        // 依据：时间线第 6 节「运行时状态按统一规则初始化」；实现修订第 3 节（卡住门槛、上游监视窗都按标定快照的时长）。
        void testRestoreAndCommitInitialiseRuntimeAlike()
        {
            beginTest ("restore clears the profile-mismatch accumulation and hint; commit re-derives the guard / monitor timing from the new snapshot");

            const auto music = makeMusic (defaultRate, 8.0, 303, 0.5f);

            // (a) 门槛 12 / 回差 6 LU：低 10 LU 的画像一直在累计、永不触发；低 15 LU 的画像一满持续时长就触发。
            auto constants = fastConstants();
            constants.mismatchThresholdLu = 12.0f;
            constants.mismatchReleaseLu = 6.0f;
            Rig rig (constants, music, defaultParams (50.0f, 0));
            expect (rig.learnAndCalibrate(), "learned (a)");
            const auto learned = rig.committed();
            const auto sustain = (std::int64_t) std::llround (constants.mismatchSustainSeconds * defaultRate);

            const auto quieterProfile = [&] (float byLu)
            {
                auto state = learned;
                state.profile.integratedLufs.value -= byLu;
                state.profileId = glaze::computeProfileId (state.profile);
                state.plan = glaze::Plan {};
                state.calibration = glaze::Calibration {};
                return glaze::toXml (state);
            };

            rig.chain.restoreGlazeStateXml (quieterProfile (10.0f).get());
            rig.run (3.0);
            const auto accumulating = rig.probe();
            logMessage ("  10 LU below: accumulated " + juce::String ((double) accumulating.mismatchSamples / defaultRate, 2) + " s, steps "
                        + juce::String (accumulating.mismatchSteps));
            expectEquals (accumulating.mismatchSteps, 0, "10 LU below the music (under the 12 LU threshold): accumulates without triggering");
            expect (accumulating.mismatchSamples >= sustain, "the accumulation already exceeds the sustain time when the next profile is read");
            expect (rig.committed().profile.integratedLufs.value < learned.profile.integratedLufs.value - 9.5f, "the quieter profile is in effect");

            rig.chain.restoreGlazeStateXml (quieterProfile (15.0f).get());
            rig.step();
            expectEquals (rig.probe().mismatchSteps, 0, "the block that adopts a profile restored mid-play does not reuse the old accumulation");
            expect (rig.probe().mismatchSamples <= (std::int64_t) blockSize, "the accumulation restarted with the restore");
            int blocks = 0;
            const auto recognised = rig.runUntil (3.0, [&] { ++blocks; return rig.probe().mismatchSteps > 0; });
            const auto recognisedAfter = (double) (blocks + 1) * blockSize / defaultRate;
            logMessage ("  15 LU mismatch recognised " + juce::String (recognisedAfter, 2) + " s after the restore");
            expect (recognised, "after the sustain time the 15 LU mismatch is recognised as usual");
            expect (recognisedAfter >= constants.mismatchSustainSeconds - 0.05, "not before the sustain time has been accumulated again");
            expect (rig.status().profileMismatch, "the relearn hint is shown for the mismatched profile");

            rig.chain.restoreGlazeStateXml (glaze::toXml (learned).get());
            rig.step();
            expect (! rig.status().profileMismatch, "restoring another profile clears the hint that belonged to the previous one");

            // (b)
            Rig old (fastConstants(), music, defaultParams (50.0f, 0));
            expect (old.learnAndCalibrate(), "learned (b)");
            auto state = old.committed();
            state.constants.guardWindowSeconds = 6.0;
            state.constants.guardRecoverAfterSeconds = 10.0;
            old.chain.restoreGlazeStateXml (glaze::toXml (state).get());
            old.step();
            expectEquals (old.probe().monitorWindowSlots, 60, "the restored snapshot's 6 s monitor window is in effect (60 slots)");
            expectEquals ((juce::int64) old.probe().guardStuckSamples, (juce::int64) (11.0 * defaultRate), "the restored snapshot's 10 + 1 s stuck threshold is in effect");

            old.chain.requestGlazeRelearn();
            const auto commits = old.probe().commits;
            expect (old.runUntil (20.0, [&] { return old.probe().commits > commits; }), "the relearn commits a new profile with this version's snapshot");
            expectEquals (old.probe().monitorWindowSlots, 30, "after the commit the monitor window is back to this version's 3 s (30 slots)");
            expectEquals (old.probe().monitorTriggerSlots, 10, "and its trigger time to 1 s (10 slots)");
            expectEquals ((juce::int64) old.probe().guardStuckSamples, (juce::int64) (6.0 * defaultRate), "after the commit the stuck threshold is back to 5 + 1 s");
        }

        // T-032 R6-04：实时 Tame 许可 = 上限 × GLAZE × κ × 证据，且只在已提交画像之后放开（与方案侧 GLAZE × κ、updateLiveTone 同口径；
        // 规格 7.2「学习结束后」）。修前不乘 κ、未学习 / 首次聆听中也按满置信度放许可：未学习 GLAZE 100 方案的 Tame 增量约 8，实时路径
        // 却可给到通透的上限 45。κ = 1 时与修前逐位相同（见 testT032ControllerDigests 的对照摘要）。
        void testLiveTameNeedsAProfileAndScalesWithConfidence()
        {
            beginTest ("live Tame authority needs a committed profile and scales with the profile's confidence (kappa)");

            const auto flat = makeNoise (numChannels, (int) (8.0 * defaultRate), 0.04f, 903);
            auto peaked = flat;
            const auto tone = makeSine (numChannels, defaultRate, 2000.0, 0.16f, 8.0);
            for (size_t ch = 0; ch < peaked.size(); ++ch)
                for (size_t i = 0; i < peaked[ch].size(); ++i)
                    peaked[ch][i] += tone[ch][i];

            // (a) 未学习（首次聆听中）：GLAZE 100 + 强 Tame 证据，实际 Tame 不超过方案自己的自动增量（没有实时许可）。
            {
                auto slow = fastConstants();
                slow.minListenSeconds = 15.0;
                slow.maxListenSeconds = 30.0;
                Rig rig (slow, peaked, defaultParams (100.0f, 3));
                auto largestExtra = 0.0f, largestAutomatic = 0.0f;
                rig.run (6.0, [&] (int)
                {
                    const auto& effective = rig.chain.getEffectiveParams().balance;
                    largestAutomatic = juce::jmax (largestAutomatic, effective.automaticTame);
                    largestExtra = juce::jmax (largestExtra, effective.automaticTame - rig.probe().increments.balanceTame);
                });
                expect (! rig.committed().hasProfile && rig.probe().learning, "still in the first listen after 6 s");
                logMessage ("  unlearned: largest automatic Tame " + juce::String (largestAutomatic, 3) + ", largest amount beyond the plan "
                            + juce::String (largestExtra, 5) + " (clean tameCap " + juce::String (slow.clean.tameCap, 1) + ")");
                expect (largestExtra <= 1.0e-4f, "no live authority beyond the plan's own increment before a profile is committed");
            }

            // (b) 已提交画像：同一素材、同一证据，κ = 0.675（代表性 0.4 / fullConfidence 0.8 → 置信 0.5）与 κ = 1 两份画像，
            //     实时许可按 κ 缩放：上限 = tameCap × GLAZE × κ，κ = 1 那份到得更高。
            Rig learned (fastConstants(), flat);
            learned.run (4.0);
            auto base = learned.committed();
            expect (base.hasProfile, "learned the flat noise");
            base.profile.tameEvidenceDb.fill (0.0f);
            base.profile.integratedLufs.confidence = 1.0f;
            base.profile.truePeakDbtp.confidence = 1.0f;
            base.calibration = glaze::Calibration {};
            base.plan = glaze::Plan {};

            float highest[2] = { 0.0f, 0.0f }, kappas[2] = { 0.0f, 0.0f };

            for (int variant = 0; variant < 2; ++variant)
            {
                auto state = base;
                state.profile.representativeness = variant == 0 ? 0.4f : 1.0f;
                state.profileId = glaze::computeProfileId (state.profile);
                kappas[variant] = glaze::confidenceScale (&state.profile, state.constants);
                const auto saved = glaze::toXml (state);

                Rig rig (fastConstants(), flat, defaultParams (50.0f, 3));
                rig.chain.restoreGlazeStateXml (saved.get());
                rig.run (3.0);
                const auto baseline = rig.chain.getEffectiveParams().balance.automaticTame;
                rig.source = peaked;
                rig.sourceIndex = 0;
                auto peak = baseline;
                rig.run (2.0, [&] (int) { peak = juce::jmax (peak, rig.chain.getEffectiveParams().balance.automaticTame); });
                highest[variant] = peak;
                const auto cap = 0.5f * state.constants.clean.tameCap * kappas[variant];
                logMessage ("  kappa " + juce::String (kappas[variant], 3) + ": baseline automatic Tame " + juce::String (baseline, 3)
                            + ", highest " + juce::String (peak, 3) + ", cap " + juce::String (cap, 3));
                expect (peak <= cap + 1.0e-4f, "live authority is capped by tameCap x GLAZE x kappa");
                expect (peak > baseline + 1.0f, "the resonance still earns live authority");
            }

            expectWithinAbsoluteError (kappas[1], 1.0f, 1.0e-6f, "the fully confident profile has kappa 1");
            expect (kappas[0] > 0.6f && kappas[0] < 0.7f, "the less confident profile has kappa about 0.675: " + juce::String (kappas[0], 4));
            expect (highest[1] > highest[0] + 1.0f, "kappa 1 reaches visibly more live Tame than kappa 0.675 on the same evidence");
        }

        // T-032 R5-06 与逐位契约的对照摘要。几段场景覆盖「同一次调用里同输入的纯函数只算一次」的每条路径——Tame 与 Tone 的混合上限
        // 和 κ（computeEffective）、偏差目标的修正范围与「校准适用」（biasTarget / finishRound）、音色与密度跟随共用的斜率回归
        //（updateLiveTone / updateLiveColor）——另加 GLAZE 0。逐块把交给模块的实际参数、执行的校准偏差 / 保护偏移、实时 Heat / Glue
        // 回收量与输出样本按位并进 64 位 FNV-1a，打进日志。同一份用例分别在「去重前 / 去重后」「修前 / 修后」的构建里跑，对照日志里的
        // 摘要（IMPL-A6 报告）：去重前后每段都必须相同；修前修后 GLAZE 0 与 κ = 1 两段必须相同（κ < 1 的段因 R6-04 本来就变）。
        // 用例自己断言：每条路径确实走到、同一场景跑两遍摘要相同、GLAZE 0 每块实际参数逐位等于手动值。
        void testT032ControllerDigests()
        {
            beginTest ("T-032 digests: live Tame / Tone / Color, calibration bias and GLAZE 0 hashed block by block (bit-exact comparison across builds)");

            struct Digest
            {
                std::uint64_t h = 0xcbf29ce484222325ULL;

                void word (std::uint32_t w) noexcept
                {
                    for (int i = 0; i < 4; ++i)
                    {
                        h ^= (std::uint64_t) ((w >> (8 * i)) & 0xffu);
                        h *= 0x100000001b3ULL;
                    }
                }

                void add (float v) noexcept
                {
                    std::uint32_t w = 0;
                    std::memcpy (&w, &v, sizeof (w));
                    word (w);
                }

                juce::String hex() const   { return "0x" + juce::String::toHexString ((juce::int64) h).paddedLeft ('0', 16); }
            };

            const auto digestBlock = [] (Digest& d, const Rig& r)
            {
                const auto& e = r.chain.getEffectiveParams();
                for (const auto v : { e.balance.recover, e.balance.tame, e.balance.automaticRecover, e.balance.automaticTame,
                                      e.color.toneDb, e.color.glue, e.color.heat, e.limit.gainDb, e.limit.clip })
                    d.add (v);

                const auto p = r.probe();
                for (const auto v : { p.appliedBiasDb, p.executedBiasDb, p.executedGuardDb, p.liveHeatReduction, p.liveGlueReduction })
                    d.add (v);

                for (int ch = 0; ch < r.buffer.getNumChannels(); ++ch)
                    for (int i = 0; i < r.buffer.getNumSamples(); ++i)
                        d.add (r.buffer.getSample (ch, i));
            };

            auto constants = fastConstants();
            constants.mismatchThresholdLu = constants.mismatchReleaseLu = 1.0e9f;   // 换段不触发画像失配（这里只比逐位）

            // 素材：亮噪声（学它）、同功率的暗版本（音色跟随）、亮噪声 + 2 kHz 共振（实时 Tame 证据）。
            const auto bright = makeNoise (numChannels, (int) (8.0 * defaultRate), 0.08f, 147);
            auto dark = bright;
            {
                const auto retain = std::exp (-juce::MathConstants<double>::twoPi * 250.0 / defaultRate);
                double sourcePower = 0.0, darkPower = 0.0;
                for (size_t ch = 0; ch < dark.size(); ++ch)
                {
                    double z = 0.0;
                    for (size_t i = 0; i < dark[ch].size(); ++i)
                    {
                        z = bright[ch][i] + (z - bright[ch][i]) * retain;
                        dark[ch][i] = (float) z;
                        sourcePower += (double) bright[ch][i] * bright[ch][i];
                        darkPower += z * z;
                    }
                }
                scale (dark, (float) std::sqrt (sourcePower / darkPower));
            }
            auto peaked = bright;
            {
                const auto tone = makeSine (numChannels, defaultRate, 2000.0, 0.16f, 8.0);
                for (size_t ch = 0; ch < peaked.size(); ++ch)
                    for (size_t i = 0; i < peaked[ch].size(); ++i)
                        peaked[ch][i] += tone[ch][i];
            }

            Rig learner (fastConstants(), bright);
            learner.run (4.0);
            auto base = learner.committed();
            expect (base.hasProfile, "learned the bright noise");
            base.profile.spectralSlopeDbPerOct.confidence = 1.0f;
            base.profile.bandConfidence.fill (1.0f);
            base.profile.integratedLufs.confidence = base.profile.truePeakDbtp.confidence = 1.0f;
            base.profile.tameEvidenceDb.fill (0.0f);
            base.plan = glaze::Plan {};
            base.calibration = glaze::Calibration {};

            // 锁定的画像读回后换段播放：音色跟随（暗段）、实时 Tame（共振段）、校准（手动 Tone +2 dB 让前馈估不准，偏差非 0）。
            struct Seen { bool tone = false, tame = false, bias = false; };

            const auto runLocked = [&] (float representativeness, Seen& seen)
            {
                auto state = base;
                state.profile.representativeness = representativeness;
                state.profileId = glaze::computeProfileId (state.profile);
                auto params = defaultParams (50.0f, 3);
                params.color.toneDb = 2.0f;
                Rig rig (constants, bright, params);
                rig.chain.restoreGlazeStateXml (glaze::toXml (state).get());
                Digest d;

                struct Segment { const Signal* source; double seconds; };
                const Segment segments[] = { { &bright, 3.0 }, { &dark, 6.0 }, { &peaked, 3.0 }, { &bright, 3.0 } };

                for (const auto& segment : segments)
                {
                    rig.source = *segment.source;
                    rig.sourceIndex = 0;
                    rig.run (segment.seconds, [&] (int)
                    {
                        digestBlock (d, rig);
                        const auto& e = rig.chain.getEffectiveParams();
                        const auto p = rig.probe();
                        const auto plannedTone = juce::jlimit (-3.0f, 3.0f, rig.params.color.toneDb + p.increments.colorToneDb);
                        seen.tone = seen.tone || std::abs (e.color.toneDb - plannedTone) > 0.005f;
                        seen.tame = seen.tame || e.balance.automaticTame > p.increments.balanceTame + 0.5f;
                        seen.bias = seen.bias || std::abs (p.executedBiasDb) > 1.0e-3f;
                    });
                }

                logMessage ("  locked, representativeness " + juce::String (representativeness, 2) + " (kappa "
                            + juce::String (glaze::confidenceScale (&state.profile, state.constants), 3) + "): calibration finishes "
                            + juce::String (rig.probe().calibrationFinishes) + ", executed bias now " + juce::String (rig.probe().executedBiasDb, 4) + " dB");
                return d;
            };

            Seen full, fullAgain, partial;
            const auto kappaOne = runLocked (1.0f, full);
            const auto kappaOneAgain = runLocked (1.0f, fullAgain);
            const auto kappaPartial = runLocked (0.4f, partial);
            expect (full.tone && full.tame && full.bias, juce::String ("kappa 1 exercises live Tone / live Tame / bias: ")
                    + (full.tone ? "tone " : "") + (full.tame ? "tame " : "") + (full.bias ? "bias" : ""));
            expect (partial.tone && partial.tame, "kappa 0.675 exercises live Tone / live Tame");
            expectEquals (kappaOneAgain.hex(), kappaOne.hex(), "the same scenario hashes identically twice");

            // 密度跟随：稀疏素材上学，GLAZE 60 温润换成密集白噪声（与 testLiveColorAfterProfileLocks 同一对素材）。
            Digest colorDigest;
            auto colorSeen = false;
            {
                auto colorConstants = constants;
                colorConstants.minListenSeconds = 10.0;
                colorConstants.maxListenSeconds = 12.0;
                const auto dense = makeNoise (numChannels, (int) (8.0 * defaultRate), 0.075f, 627);
                auto sparse = dense;
                const auto retain = std::exp (-juce::MathConstants<double>::twoPi * 800.0 / defaultRate);
                float peak = 0.0f;
                for (auto& channel : sparse)
                {
                    double z = 0.0;
                    for (size_t i = 0; i < channel.size(); ++i)
                    {
                        z = channel[i] + (z - channel[i]) * retain;
                        const auto beatPosition = std::fmod ((double) i / defaultRate, 0.5);
                        channel[i] = (float) (z * (0.15 + 0.85 * std::exp (-beatPosition / 0.07)));
                        peak = juce::jmax (peak, std::abs (channel[i]));
                    }
                }
                scale (sparse, 0.3f / peak);

                auto params = defaultParams (0.0f, 1);
                params.color.heat = 4.0f;
                params.color.glue = 5.0f;
                Rig rig (colorConstants, sparse, params);
                rig.run (14.0);
                expect (rig.committed().hasProfile, "learned the sparse material");
                rig.params.glazePercent = 60.0f;
                rig.source = dense;
                rig.sourceIndex = 0;
                rig.run (10.0, [&] (int)
                {
                    digestBlock (colorDigest, rig);
                    colorSeen = colorSeen || rig.probe().liveHeatReduction > 0.0f || rig.probe().liveGlueReduction > 0.0f;
                });
            }
            expect (colorSeen, "the dense section exercises the live Heat / Glue reduction (shared slope regression)");

            // GLAZE 0：边学边播，每块实际参数逐位等于手动值（ADR-006 / 时间线：GLAZE 0 = v0.5 手动处理），输出进摘要。
            Digest zeroDigest;
            auto zeroExact = true;
            {
                Rig rig (constants, makeMusic (defaultRate, 8.0, 401, 0.3f), defaultParams (0.0f, 0));
                rig.run (8.0, [&] (int)
                {
                    digestBlock (zeroDigest, rig);
                    zeroExact = zeroExact && sameParams (rig.chain.getEffectiveParams(), rig.params);
                });
                expect (rig.committed().hasProfile, "GLAZE 0 still learns");
            }
            expect (zeroExact, "GLAZE 0: every block's actual parameters equal the manual ones bit for bit");

            logMessage ("  T-032 digest kappa-1 locked:     " + kappaOne.hex());
            logMessage ("  T-032 digest kappa-0.675 locked: " + kappaPartial.hex());
            logMessage ("  T-032 digest live Color:         " + colorDigest.hex());
            logMessage ("  T-032 digest GLAZE 0:            " + zeroDigest.hex());
        }
    };

    static GlazeControllerTests glazeControllerTests;

    //==============================================================================
    // ---- T-038 House 档（阶段 2 B 追加）----
    // 编排器的 House 档（设计规格-曲风-House学习方式 第 3、4、7 节；GlazeController.cpp 开头「T-038」一节）：锁定条件、暂定方案与新响段的
    // 快过渡、锁定后防漏网与重开次数上限、换曲风 / 交付、通用档逐位不变、状态字段、音频线程零分配。都经 CeladonChain（与插件同一接线），
    // Rig 报恒定 124 BPM 的拍点（tempoBpm）。常数：fastConstants（聆听 2 ～ 3 s：通用档的置信度条件早早满足，House 的锁定条件才是卡口）
    // 加短的锁定条件（lockSegments / requiredFullSegments）。只在文件末尾追加：上面的用例与工具一个不改，另起一个测试类（GlazeControllerHouse）。
    namespace
    {
        constexpr double houseBpm = 124.0;
        const double houseBarSeconds = 4.0 * 60.0 / houseBpm;   // 1.935 s

        glaze::Constants houseConstants (int lockSegments, int requiredFullSegments)
        {
            auto c = fastConstants();
            c.house.lockSegments = lockSegments;
            c.house.requiredFullSegments = requiredFullSegments;
            return c;
        }

        ChainParams houseParams (float glazePercent, int genre = 1)
        {
            auto p = defaultParams (glazePercent, 0);
            p.glazeGenre = genre;
            return p;
        }

        // 合成的 House（124 BPM、4/4），按段给：bars 小节，drop = 有低音（55 Hz 锯齿 1 ～ 6 次谐波）与和声垫，否则只有鼓（底鼓每拍
        // 120 → 50 Hz 下滑、踩镲在反拍），gain = 这一段的增益。增益 1 的 drop 约 −12 LUFS、只有鼓的前奏约 −21 LUFS。
        struct HouseSection
        {
            int bars = 0;
            bool drop = false;
            float gain = 1.0f;
        };

        Signal makeHouseSong (double sampleRate, std::initializer_list<HouseSection> sections, juce::int64 seed)
        {
            const auto beat = 60.0 / houseBpm;
            const auto bar = 4.0 * beat;
            auto totalBars = 0;

            for (const auto& s : sections)
                totalBars += s.bars;

            const auto numSamples = (int) std::lround ((double) totalBars * bar * sampleRate);
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
                    auto barIndex = (int) (t / bar);
                    HouseSection section;

                    for (const auto& s : sections)
                    {
                        section = s;

                        if (barIndex < s.bars)
                            break;

                        barIndex -= s.bars;
                    }

                    const auto beatPosition = std::fmod (t, beat);
                    const auto offBeatPosition = std::fmod (t + 0.5 * beat, beat);
                    const auto kick = (section.drop ? 0.5 : 0.35) * std::exp (-beatPosition / 0.12)
                                    * std::sin (twoPi * (50.0 * beatPosition + 70.0 * 0.03 * (1.0 - std::exp (-beatPosition / 0.03))));
                    const auto white = (double) random.nextFloat() * 2.0 - 1.0;
                    const auto hat = 0.09 * std::exp (-offBeatPosition / 0.025) * (white - previous);
                    previous = white;

                    double bass = 0.0, pad = 0.0;

                    if (section.drop)
                    {
                        for (int h = 1; h <= 6; ++h)
                            bass += std::sin (twoPi * 55.0 * h * t) / h;

                        bass *= 0.22;

                        for (const auto hz : { 220.0, 277.18, 329.63 })
                            pad += 0.05 * std::sin (twoPi * hz * t + 0.7 * ch);
                    }

                    channel[(size_t) i] = (float) ((double) section.gain * (kick + hat + bass + pad));
                }
            }

            return signal;
        }

        double secondsOf (int blocksDone)   { return (double) blocksDone * blockSize / defaultRate; }
    }

    class GlazeControllerHouseTests  : public juce::UnitTest
    {
    public:
        GlazeControllerHouseTests() : juce::UnitTest ("GlazeControllerHouse", "Celadon") {}

        void runTest() override
        {
            // House 的锁定 / 重开 / 暂定方案已经删掉。下面的旧方法不再调用。
            testHouseStaysUnlocked();
            testBeatReleaseCap();
            testTempoDeterminism();
            testDeliveryChangeDoesNotCalibrate();
            testHouseColdExportAndTruePeak();
            testHouseAudioThreadAllocation();
        }

        void testHouseStaysUnlocked()
        {
            beginTest ("House plays continuously: no lock, no commit, houseLockPending stays false");

            const auto song = makeHouseSong (defaultRate, { { 8, false, 1.0f }, { 12, true, 1.0f } }, 61);
            Rig rig (houseConstants (12, 4), song, houseParams (60.0f));
            rig.tempoBpm = houseBpm;
            auto flagsHeld = true;

            rig.run (12.0, [&] (int)
            {
                const auto s = rig.status();
                flagsHeld = flagsHeld && s.houseActive && s.phase == Phase::unlearned && ! s.houseLockPending
                            && ! s.houseOfflineListening && ! s.profileReady && ! s.calibrationValid;
            });

            const auto probe = rig.probe();
            expect (flagsHeld, "House stays active without the retired lock flags");
            expectEquals (probe.commits, 0, "House does not commit a candidate");
            expectEquals (probe.calibrationBegins, 0, "House does not calibrate");
            expectEquals (probe.houseReopens, 0, "House does not reopen");
            expect (! rig.chain.getGlazeController().getCommittedStateForTesting().profile.houseGate.valid,
                    "the followed profile does not carry a house gate");
        }

        void testBeatReleaseCap()
        {
            beginTest ("House plus tempo sets the beat-release cap; general plus tempo leaves it at 0");

            const auto song = makeHouseSong (defaultRate, { { 4, true, 1.0f } }, 62);
            Rig house (fastConstants(), song, houseParams (40.0f, 1));
            house.tempoBpm = houseBpm;
            house.step();
            const auto expected = (float) (60000.0 / houseBpm / 3.0);
            expectWithinAbsoluteError (house.chain.getEffectiveParams().limit.beatReleaseCapMs, expected, 0.01f,
                                       "House at 124 BPM caps the slow release at 60000/124/3 ms");

            Rig general (fastConstants(), song, houseParams (40.0f, 0));
            general.tempoBpm = houseBpm;
            general.step();
            expectEquals (general.chain.getEffectiveParams().limit.beatReleaseCapMs, 0.0f,
                          "general plus tempo does not cap the release");

            Rig untapped (fastConstants(), song, houseParams (40.0f, 1));
            untapped.step();
            expectEquals (untapped.chain.getEffectiveParams().limit.beatReleaseCapMs, 0.0f,
                          "House without tempo does not cap the release");
        }

        void testTempoDeterminism()
        {
            beginTest ("two tempo renders are bit-identical; general audio may differ when tempo is supplied");

            const auto song = makeHouseSong (defaultRate, { { 4, true, 1.0f } }, 63);
            const auto render = [&] (int genre, double tempo)
            {
                Rig rig (fastConstants(), song, houseParams (35.0f, genre));
                rig.tempoBpm = tempo;
                std::vector<float> out ((size_t) rig.blocksFor (2.0) * (size_t) blockSize);

                for (int b = 0; b < rig.blocksFor (2.0); ++b)
                {
                    rig.step();
                    std::memcpy (out.data() + (size_t) b * (size_t) blockSize, rig.buffer.getReadPointer (0),
                                 sizeof (float) * (size_t) blockSize);
                }

                return out;
            };

            const auto tempoA = render (0, houseBpm);
            const auto tempoB = render (0, houseBpm);
            const auto plain = render (0, 0.0);
            const auto houseA = render (1, houseBpm);
            const auto houseB = render (1, houseBpm);
            expect (tempoA == tempoB, "repeating the general tempo render is bit-identical");
            expect (houseA == houseB, "repeating the House tempo render is bit-identical");
            logMessage ("  general with tempo vs without: " + juce::String (tempoA == plain ? "bit-identical" : "different")
                        + " (phase 2 may consume PPQ for every genre; the release cap stays 0)");
        }

        void testDeliveryChangeDoesNotCalibrate()
        {
            beginTest ("a delivery change replans both targets and does not start calibration");

            const auto song = makeHouseSong (defaultRate, { { 8, true, 1.0f } }, 64);
            Rig rig (fastConstants(), song, houseParams (50.0f, 1));
            rig.tempoBpm = houseBpm;
            rig.run (3.0);
            const auto before = rig.status().effectiveTargetLufs;
            rig.params.glazeDelivery = 1;
            rig.run (2.0);
            const auto after = rig.status().effectiveTargetLufs;
            logMessage ("  target " + juce::String (before, 2) + " -> " + juce::String (after, 2) + " LUFS");
            expect (std::isfinite (before) && std::isfinite (after), "both deliveries publish a finite target");
            expectEquals (rig.probe().calibrationBegins, 0, "a delivery change does not calibrate");
            expectEquals (rig.probe().commits, 0, "a delivery change does not commit");
        }

        void testHouseColdExportAndTruePeak()
        {
            beginTest ("House offline export is cold while confidence is low, and the true peak holds");

            const auto song = makeHouseSong (defaultRate, { { 16, true, 1.0f } }, 65);
            Rig rig (fastConstants(), song, houseParams (45.0f, 1));
            rig.tempoBpm = houseBpm;
            rig.host.offline = true;
            rig.run (2.0);
            expect (rig.status().coldExport && rig.status().confidence < 0.5f, "offline House with low confidence is a cold export");
            expect (rig.chain.getAndResetOutputTruePeakDb() <= rig.params.limit.ceilingDb + 0.01f,
                    "true peak stays within the ceiling");
            expectEquals (rig.probe().commits, 0, "the cold export does not lock");
        }

        void testHouseAudioThreadAllocation()
        {
            beginTest ("House setHostInfo / setParams / process / getStatus do not allocate");

            const auto song = makeHouseSong (defaultRate, { { 8, false, 1.0f }, { 8, true, 1.0f } }, 66);
            Rig rig (fastConstants(), song, houseParams (50.0f, 1));
            rig.tempoBpm = houseBpm;
            std::uint64_t allocations = 0;
            auto finite = true;

            for (int b = 0; b < rig.blocksFor (8.0); ++b)
            {
                if (b == rig.blocksFor (3.0))
                    rig.params.glazeDelivery = 2;

                if (b == rig.blocksFor (5.0))
                    rig.position += 96000;

                const auto length = (size_t) rig.source.front().size();
                rig.buffer.setSize (numChannels, blockSize, false, false, true);

                for (int ch = 0; ch < numChannels; ++ch)
                    for (int i = 0; i < blockSize; ++i)
                        rig.buffer.setSample (ch, i, rig.source[(size_t) ch][(rig.sourceIndex + (size_t) i) % length]);

                rig.sourceIndex = (rig.sourceIndex + (size_t) blockSize) % length;
                rig.host.positionSamples = rig.position;
                rig.host.hasTempo = true;
                rig.host.bpm = rig.tempoBpm;
                rig.host.hasPpq = true;
                rig.host.ppqPosition = (double) rig.position * rig.tempoBpm / (60.0 * defaultRate);
                rig.host.hasBarStart = true;
                rig.host.ppqLastBarStart = std::floor (rig.host.ppqPosition / 4.0) * 4.0;
                rig.host.hasTimeSignature = true;

                const auto before = allocationCountOnThisThread();
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

            expect (finite, "output finite throughout");
            expectEquals (rig.probe().commits, 0, "the allocation sweep does not commit");
            expect (rig.probe().transportJumps >= 1, "the sweep covered a transport jump");
            expectEquals ((juce::int64) allocations, (juce::int64) 0, "audio thread must not allocate");
        }

    private:
        // 依据：第 3.1 节三条锁定条件、第 3.2 节「暂定期间不跑校准轮」。8 小节只有鼓的前奏接 drop，锁定条件 12 / 4：通用档 3 s 就提交，
        // House 档要等到已听 12 小节且满编 4 小节（第 12 小节定稿，约 23.2 s）才提交，期间暂定方案每小节更新、一轮校准都没开，
        // 状态行「锁定条件还没满足」；提交的画像是格式 3，满编段是 drop 的 4 小节；提交后照常校准。
        void testLockConditions()
        {
            beginTest ("House: commits only when heard >= lockSegments bars and full >= requiredFullSegments; provisional updates, no calibration before");

            const auto song = makeHouseSong (defaultRate, { { 8, false, 1.0f }, { 12, true, 1.0f } }, 61);

            Rig general (houseConstants (12, 4), song, defaultParams (60.0f, 0));
            general.tempoBpm = houseBpm;
            const auto generalCommitted = general.runUntil (6.0, [&] { return general.probe().commits > 0; });
            expect (generalCommitted, "control: the general genre commits after its 2 - 3 s listen");

            Rig rig (houseConstants (12, 4), song, houseParams (60.0f));
            rig.tempoBpm = houseBpm;
            double commitAt = -1.0;
            auto calibratedBeforeCommit = false, pendingWhileListening = true;
            auto updatesAtCommit = 0;

            for (int b = 0; b < rig.blocksFor (40.0); ++b)
            {
                rig.step();
                const auto probe = rig.probe();

                if (commitAt >= 0.0)
                    continue;

                calibratedBeforeCommit = calibratedBeforeCommit || probe.calibrationBegins > 0;

                if (probe.commits > 0)
                {
                    commitAt = secondsOf (b + 1);
                    updatesAtCommit = probe.houseUpdates;
                }
                else if (secondsOf (b + 1) > 1.0)
                {
                    const auto s = rig.status();
                    pendingWhileListening = pendingWhileListening && s.houseActive && s.houseLockPending && s.phase == Phase::listening;
                }
            }

            const auto committed = rig.committed();
            const auto& gate = committed.profile.houseGate;
            const auto status = rig.status();
            logMessage ("  House commit at " + juce::String (commitAt, 2) + " s (12 bars = " + juce::String (12.0 * houseBarSeconds, 2)
                        + " s); heard " + juce::String (gate.heardSegments) + ", full " + juce::String (gate.fullSegments) + ", I "
                        + juce::String (committed.profile.integratedLufs.value, 2) + " LUFS, S_ref " + juce::String (gate.referenceLufs, 2)
                        + "; provisional updates " + juce::String (updatesAtCommit) + "; calibration rounds " + juce::String (rig.probe().calibrationBegins));

            expect (commitAt >= 12.0 * houseBarSeconds && commitAt <= 12.0 * houseBarSeconds + 1.0, "commit when the 12th bar is finalized");
            expect (committed.hasProfile && committed.profile.formatVersion == glaze::houseProfileFormatVersion && gate.valid);
            expect (gate.heardSegments >= 12 && gate.fullSegments >= 4 && gate.fullSegments < gate.heardSegments,
                    "the full segments are the drop bars, not the intro");
            expectWithinAbsoluteError (committed.profile.integratedLufs.value, gate.referenceLufs, 1.0f);
            expect (! calibratedBeforeCommit, "no calibration round during the provisional phase");
            expect (pendingWhileListening, "status: House, lock pending, listening");
            expect (updatesAtCommit >= 8, "the provisional plan follows each newly gated bar");
            expect (rig.probe().calibrationBegins >= 1 && ! status.houseLockPending, "after the lock: calibrated, nothing pending");
        }

        // 依据：第 3.2 节「出现比当前 S_ref 高 ΔG 以上、而且过了低频门的新段，用 0.5 秒快过渡」。方案过渡 3 s（出厂）：前奏里暂定方案
        // 每小节一次 3 s 过渡；drop 的第一小节定稿时（约 drop 起点 + 1.94 s）记一次快过渡，正在走的过渡 0.5 s 内走完，接着就是 0.5 s 的过渡。
        void testFastTransitionAtTheDrop()
        {
            beginTest ("House: a new loud section brings a 0.5 s plan transition (the running one hurried along), ordinary updates take 3 s");

            auto constants = houseConstants (64, 8);   // 本用例里不锁定
            constants.planTransitionSeconds = 3.0;
            Rig rig (constants, makeHouseSong (defaultRate, { { 8, false, 1.0f }, { 8, true, 1.0f } }, 62), houseParams (60.0f));
            rig.tempoBpm = houseBpm;

            const auto dropAt = 8.0 * houseBarSeconds;
            const auto slow = (std::int64_t) std::llround (3.0 * defaultRate);
            const auto fast = (std::int64_t) std::llround (0.5 * defaultRate);
            const auto learningStep = (std::int64_t) std::llround (1.0 * defaultRate);   // 首次聆听开始时的那一次 1 s 过渡
            double firstFastAt = -1.0, fastRunningAt = -1.0;
            auto ordinaryBeforeDrop = true, noFastBeforeDrop = true;
            auto updatesBeforeDrop = 0;

            for (int b = 0; b < rig.blocksFor (dropAt + 4.0); ++b)
            {
                rig.step();
                const auto probe = rig.probe();
                const auto t = secondsOf (b + 1);

                if (t < dropAt)
                {
                    updatesBeforeDrop = probe.houseUpdates;
                    noFastBeforeDrop = noFastBeforeDrop && probe.houseFastTransitions == 0;

                    if (probe.transitionActive)
                        ordinaryBeforeDrop = ordinaryBeforeDrop && (probe.transitionLengthSamples == slow || probe.transitionLengthSamples == learningStep);
                }

                if (firstFastAt < 0.0 && probe.houseFastTransitions > 0)
                    firstFastAt = t;

                if (firstFastAt >= 0.0 && fastRunningAt < 0.0 && probe.transitionActive && probe.transitionLengthSamples == fast)
                    fastRunningAt = t;
            }

            logMessage ("  drop at " + juce::String (dropAt, 2) + " s; first fast update at " + juce::String (firstFastAt, 2)
                        + " s; 0.5 s transition running from " + juce::String (fastRunningAt, 2) + " s; updates in the intro "
                        + juce::String (updatesBeforeDrop));
            expect (updatesBeforeDrop >= 5, "the intro bars update the provisional plan");
            expect (ordinaryBeforeDrop && noFastBeforeDrop, "in the intro every update is an ordinary 3 s transition");
            expect (firstFastAt >= dropAt && firstFastAt <= dropAt + houseBarSeconds + 1.0, "the first drop bar triggers the fast transition");
            expect (fastRunningAt >= firstFastAt && fastRunningAt <= firstFastAt + 0.55, "the running transition is hurried: 0.5 s later the fast one runs");
        }

        // 依据：第 4 节。锁定（6 小节、满编 2）后，每 8 小节响 5 LU：连续 4 小节「≥ 画像 I + 3 LU、低频 ≥ 画像 × 0.6」→ 按段落账门控
        // 重算画像、快过渡、重新校准；每份画像最多重开 2 次（第三个更响的段落不再重开）。
        void testReopenAfterLock()
        {
            beginTest ("House: after the lock, four louder and fuller bars reopen the profile (fast transition, recalibration), at most twice");

            const auto song = makeHouseSong (defaultRate, { { 8, true, 0.25f }, { 8, true, 0.45f }, { 8, true, 0.8f }, { 8, true, 1.4f } }, 63);
            Rig rig (houseConstants (6, 2), song, houseParams (60.0f));
            rig.tempoBpm = houseBpm;

            std::vector<float> committedLufs;
            std::vector<double> reopenedAt;
            auto lastCommits = 0, lastReopens = 0, fastAfterReopen = 0, roundsAtReopen = 0;
            auto calibratedBetween = true;
            double waitingFastFrom = -1.0;

            for (int b = 0; b < rig.blocksFor (31.5 * houseBarSeconds); ++b)
            {
                rig.step();
                const auto probe = rig.probe();
                const auto t = secondsOf (b + 1);

                if (probe.commits != lastCommits)
                {
                    lastCommits = probe.commits;
                    committedLufs.push_back (rig.committed().profile.integratedLufs.value);
                }

                if (probe.houseReopens != lastReopens)
                {
                    lastReopens = probe.houseReopens;
                    reopenedAt.push_back (t);
                    waitingFastFrom = t;
                    calibratedBetween = calibratedBetween && probe.calibrationBegins > roundsAtReopen;   // 两次重开之间校准过
                    roundsAtReopen = probe.calibrationBegins;
                }

                if (waitingFastFrom >= 0.0 && probe.transitionActive && probe.transitionLengthSamples == (std::int64_t) std::llround (0.5 * defaultRate))
                {
                    ++fastAfterReopen;
                    waitingFastFrom = -1.0;
                }
            }

            const auto committed = rig.committed();
            juce::String levels;

            for (auto l : committedLufs)
                levels << juce::String (l, 2) << " ";

            logMessage ("  commits " + juce::String (rig.probe().commits) + " (I: " + levels + "LUFS); reopens " + juce::String (rig.probe().houseReopens)
                        + (reopenedAt.size() >= 2 ? " at " + juce::String (reopenedAt[0], 1) + " / " + juce::String (reopenedAt[1], 1) + " s" : juce::String())
                        + "; calibration rounds " + juce::String (rig.probe().calibrationBegins));

            expectEquals (rig.probe().houseReopens, 2, "two reopens, the third louder section is past the cap");
            expectEquals (committed.profile.houseGate.reopenCount, 2);
            expectEquals (rig.probe().commits, 3, "the first lock plus two reopens");
            expect (committedLufs.size() == 3 && committedLufs[1] > committedLufs[0] + 3.0f && committedLufs[2] > committedLufs[1] + 3.0f,
                    "each reopen moves the profile to the louder section");
            expect (reopenedAt.size() == 2 && reopenedAt[0] >= 12.0 * houseBarSeconds && reopenedAt[0] <= 13.0 * houseBarSeconds
                    && reopenedAt[1] >= 20.0 * houseBarSeconds && reopenedAt[1] <= 21.0 * houseBarSeconds,
                    "each reopen follows the 4th louder bar");
            expectEquals (fastAfterReopen, 2, "each reopen switches with the fast transition");
            expect (calibratedBetween && rig.probe().calibrationBegins > roundsAtReopen, "calibration restarts after each reopen");
        }

        // 依据：第 4 节「只往更满、更响的方向重开，变轻不重开」。锁定在响的段上，之后的安静段与同样响的段都不重开。
        void testQuieterSectionsNeverReopen()
        {
            beginTest ("House: quieter (or equally loud) sections after the lock never reopen");

            const auto song = makeHouseSong (defaultRate, { { 8, true, 0.8f }, { 8, true, 0.25f }, { 8, true, 0.8f } }, 64);
            Rig rig (houseConstants (6, 2), song, houseParams (60.0f));
            rig.tempoBpm = houseBpm;
            rig.run (23.5 * houseBarSeconds);

            expectEquals (rig.probe().commits, 1);
            expectEquals (rig.probe().houseReopens, 0);
            expect (! rig.probe().houseReopenArmed);
        }

        // 依据：设计规格-曲风与交付 第 4.3 节「从通用切到 House：画像按 House 档的门控重算；House 的锁定条件还没满足时继续聆听，不清空
        // 已有的画像」；设计规格第 5 节「旧格式 1 / 2 画像在 House 档下视为锁定条件未满足」。
        //   - 通用档学完（格式 2）后切到 House：开一轮聆听，旧画像照常生效（画像号不变），够 House 锁定条件后换成格式 3；
        //   - 再切回通用、再切到另一个 House 子类：House 锁定过的画像不再开聆听；
        //   - 自动开的那一轮还没完就切回通用：取消，旧画像照常。
        void testGenreSwitches()
        {
            beginTest ("genre switches: general -> House listens with the old profile in effect; back to general cancels the House listen");

            const auto song = makeHouseSong (defaultRate, { { 24, true, 0.5f } }, 65);

            {
                Rig rig (houseConstants (6, 2), song, defaultParams (60.0f, 0));
                rig.tempoBpm = houseBpm;
                rig.run (5.0);
                const auto general = rig.committed();
                expect (general.hasProfile && general.profile.formatVersion == glaze::clayProfileFormatVersion, "general profile, format 2");

                rig.params.glazeGenre = 1;
                rig.run (0.1);
                expectEquals (rig.probe().houseListenStarts, 1, "switching to House starts a listen");
                expect (rig.status().relearnInProgress && rig.status().houseLockPending);

                auto oldKept = true;
                double houseCommitAfter = -1.0;

                for (int b = 0; b < rig.blocksFor (20.0) && houseCommitAfter < 0.0; ++b)
                {
                    rig.step();
                    const auto c = rig.committed();

                    if (c.profile.formatVersion == glaze::houseProfileFormatVersion)
                        houseCommitAfter = secondsOf (b + 1);
                    else
                        oldKept = oldKept && c.profileId == general.profileId;
                }

                logMessage ("  general -> House: House profile committed " + juce::String (houseCommitAfter, 2) + " s after the switch");
                expect (oldKept, "the general profile stays in effect while listening");
                expect (houseCommitAfter >= 6.0 * houseBarSeconds - 0.6 && houseCommitAfter <= 8.0 * houseBarSeconds,
                        "the House profile replaces it once the lock conditions hold");

                const auto house = rig.committed();
                rig.params.glazeGenre = 0;
                rig.run (1.0);
                expect (! rig.probe().learning && rig.committed().profileId == house.profileId, "back to general: the House profile is kept");

                rig.params.glazeGenre = 2;
                rig.run (1.0);
                expectEquals (rig.probe().houseListenStarts, 1, "a House-locked profile needs no new listen");
                expect (! rig.probe().learning);
            }

            {
                Rig rig (houseConstants (6, 2), song, defaultParams (60.0f, 0));
                rig.tempoBpm = houseBpm;
                rig.run (5.0);
                const auto general = rig.committed();
                rig.params.glazeGenre = 3;
                rig.run (4.0);
                expect (rig.probe().relearning, "listening for House");
                rig.params.glazeGenre = 0;
                rig.run (0.1);
                expect (! rig.probe().learning, "back to general cancels the automatic House listen");
                rig.run (15.0);
                expect (rig.committed().profileId == general.profileId, "the general profile is untouched");
                expectEquals (rig.probe().houseListenStarts, 1);
            }
        }

        // 依据：设计规格-曲风与交付 第 4.3 节「换交付或曲风都不重新聆听：方案在已锁定的画像上按新目标重算，过渡 3 秒，再校准两轮」。
        // House 档锁定并校准后换交付（标准 → 俱乐部）、再换子类（Deep → Tech）：每次一个方案过渡、校准重新开始，不重新聆听、画像不变。
        void testDeliveryChangeRecalibrates()
        {
            beginTest ("House, locked and calibrated: a delivery or sub-genre change is one plan transition and a new calibration, no relearn");

            Rig rig (houseConstants (6, 2), makeHouseSong (defaultRate, { { 24, true, 0.5f } }, 66), houseParams (60.0f));
            rig.tempoBpm = houseBpm;
            const auto locked = rig.runUntil (40.0, [&] { return rig.phase() == Phase::locked && rig.status().calibrationValid
                                                                && rig.committed().profile.formatVersion == glaze::houseProfileFormatVersion; });
            expect (locked, "House profile locked and calibrated");

            for (const auto change : { 0, 1 })
            {
                const auto before = rig.probe();
                const auto profileId = rig.committed().profileId;

                if (change == 0)
                    rig.params.glazeDelivery = 2;
                else
                    rig.params.glazeGenre = 2;

                rig.run (0.1);
                expectEquals (rig.probe().planTransitions, before.planTransitions + 1, change == 0 ? "delivery: one transition" : "genre: one transition");
                const auto recalibrated = rig.runUntil (10.0, [&] { return rig.probe().calibrationBegins > before.calibrationBegins; });
                expect (recalibrated, "a new calibration round follows");
                expect (rig.committed().profileId == profileId && rig.probe().learningStarts == before.learningStarts, "no relearn");
            }
        }

        // 依据：任务书「通用 + 标准必须与现在逐位相同」。通用档下宿主报不报拍点，交给模块的参数与链输出逐块逐位相同（聆听、提交、
        // 过渡、校准都走到）；状态里的 House 各项保持默认。
        void testGeneralBitExactWithTempo()
        {
            beginTest ("general genre: tempo info from the host changes nothing (parameters and output bit-exact, house status fields default)");

            const auto song = makeHouseSong (defaultRate, { { 4, false, 1.0f }, { 8, true, 0.6f } }, 67);
            Rig plain (fastConstants(), song, defaultParams (60.0f, 0));
            Rig tempo (fastConstants(), song, defaultParams (60.0f, 0));
            tempo.tempoBpm = houseBpm;

            auto sameAll = true, houseDefault = true;
            const glaze::Status defaults;

            for (int b = 0; b < plain.blocksFor (20.0); ++b)
            {
                plain.step();
                tempo.step();
                sameAll = sameAll && sameParams (plain.chain.getEffectiveParams(), tempo.chain.getEffectiveParams());

                for (int ch = 0; ch < numChannels; ++ch)
                    sameAll = sameAll && std::memcmp (plain.buffer.getReadPointer (ch), tempo.buffer.getReadPointer (ch),
                                                      sizeof (float) * (size_t) blockSize) == 0;

                const auto s = tempo.status();
                houseDefault = houseDefault && ! s.houseActive && s.houseHeardSegments == defaults.houseHeardSegments
                            && s.houseSegmentMode == defaults.houseSegmentMode && ! s.houseLockPending && ! s.housePhaseHint;
            }

            expect (plain.probe().commits == 1 && plain.probe().calibrationBegins >= 1, "the run went through learning and calibration");
            expect (sameAll, "parameters and output are bit-identical with and without tempo info");
            expect (houseDefault, "house status fields stay at their defaults");
            expectEquals (tempo.chain.getGlazeController().getProbeForTesting().houseUpdates, 0);
        }

        // 依据：第 7 节状态行（已听 / 目标小节、主段落已听、拿不到拍点时按秒）。
        void testStatusFields()
        {
            beginTest ("House status: bars and targets with tempo, seconds without; main section heard once enough bars are full");

            auto constants = houseConstants (64, 4);
            const auto song = makeHouseSong (defaultRate, { { 4, false, 1.0f }, { 8, true, 1.0f } }, 68);

            {
                Rig rig (constants, song, houseParams (40.0f));
                rig.tempoBpm = houseBpm;
                rig.run (3.0 * houseBarSeconds + 0.5);
                const auto early = rig.status();
                rig.run (6.0 * houseBarSeconds);
                const auto later = rig.status();

                logMessage ("  bars: heard " + juce::String (early.houseHeardSegments) + " -> " + juce::String (later.houseHeardSegments)
                            + " / " + juce::String (later.houseTargetSegments) + ", full " + juce::String (later.houseFullSegments)
                            + " / " + juce::String (later.houseRequiredFullSegments));
                expect (early.houseActive && early.houseSegmentMode == glaze::SegmentMode::bars && early.houseLockPending);
                expectEquals (early.houseHeardSegments, 3);
                expectEquals (later.houseTargetSegments, 64);
                expectEquals (later.houseRequiredFullSegments, 4);
                expect (later.houseHeardSegments >= 8 && later.houseFullSegments >= 4 && later.houseMainSectionHeard);
                expect (! early.houseMainSectionHeard || early.houseFullSegments >= 4);
            }

            {
                Rig rig (constants, song, houseParams (40.0f));   // 不报拍点：2 秒段
                rig.run (9.0);
                const auto s = rig.status();
                logMessage ("  seconds: heard " + juce::String (s.houseHeardSeconds, 1) + " / " + juce::String (s.houseTargetSeconds, 0) + " s");
                expect (s.houseActive && s.houseSegmentMode == glaze::SegmentMode::seconds);
                expectWithinAbsoluteError (s.houseTargetSeconds, 124.0f, 1.0e-4f);
                expect (s.houseHeardSeconds >= 6.0f && s.houseHeardSeconds <= 9.0f, "heard seconds follow the effective audio");
            }
        }

        // 集成时追加（B 报告遗留一条；设计规格-曲风-House学习方式 第 9 节）：House 反相提示端到端——编排器判「满编段」并按 4 小节换成秒、
        // ClayGuard 判门限与持续时长、Status 带出来。前奏 4 小节只有鼓，接 12 小节 drop：低音右声道比左声道相移 90°（低频相关度约 0，
        // 只看低音时单声道损失约 3 dB；底鼓是单声道，合起来相关度约 0.3）→ drop 里持续 4 小节以上后提示亮、损失深于门限对应的 −0.97 dB；
        // 对照：低音两声道同相 → 不亮；通用档同一首 → 不亮。
        void testHousePhaseHintEndToEnd()
        {
            beginTest ("House phase hint end to end: a wide (90-degree) bass in the main section lights it; an in-phase bass or the General genre never does");

            const auto makeSong = [] (bool wideBass)
            {
                const auto beat = 60.0 / houseBpm;
                const auto bar = 4.0 * beat;
                const auto introBars = 4, totalBars = 16;
                const auto numSamples = (int) std::lround ((double) totalBars * bar * defaultRate);
                const auto twoPi = juce::MathConstants<double>::twoPi;
                auto signal = makeSilence (numChannels, numSamples);

                for (int i = 0; i < numSamples; ++i)
                {
                    const auto t = (double) i / defaultRate;
                    const auto drop = (int) (t / bar) >= introBars;
                    const auto beatPosition = std::fmod (t, beat);
                    const auto kick = (drop ? 0.5 : 0.35) * std::exp (-beatPosition / 0.12)
                                    * std::sin (twoPi * (50.0 * beatPosition + 70.0 * 0.03 * (1.0 - std::exp (-beatPosition / 0.03))));
                    double bassLeft = 0.0, bassRight = 0.0;

                    if (drop)
                    {
                        for (int h = 1; h <= 6; ++h)
                        {
                            bassLeft += std::sin (twoPi * 55.0 * h * t) / h;
                            bassRight += (wideBass ? std::cos (twoPi * 55.0 * h * t) : std::sin (twoPi * 55.0 * h * t)) / h;
                        }
                    }

                    signal[0][(size_t) i] = (float) (kick + 0.22 * bassLeft);
                    signal[1][(size_t) i] = (float) (kick + 0.22 * bassRight);
                }

                return signal;
            };

            const auto runFor = [&makeSong] (bool wideBass, int genre, glaze::Status& out, float& peakLossDb, int& litBlocks)
            {
                Rig rig (houseConstants (64, 4), makeSong (wideBass), houseParams (40.0f, genre));
                rig.tempoBpm = houseBpm;
                peakLossDb = 0.0f;
                litBlocks = 0;
                rig.run (15.0 * houseBarSeconds, [&] (int)
                {
                    const auto s = rig.status();
                    if (s.housePhaseHint)
                    {
                        ++litBlocks;
                        peakLossDb = juce::jmin (peakLossDb, s.housePhaseMonoLossDb);
                    }
                });
                out = rig.status();
            };

            glaze::Status wide, inPhase, general;
            float wideLoss = 0.0f, inPhaseLoss = 0.0f, generalLoss = 0.0f;
            int wideLit = 0, inPhaseLit = 0, generalLit = 0;
            runFor (true, 1, wide, wideLoss, wideLit);
            runFor (false, 1, inPhase, inPhaseLoss, inPhaseLit);
            runFor (true, 0, general, generalLoss, generalLit);

            logMessage ("  wide bass: lit " + juce::String (wideLit) + " blocks, deepest mono loss " + juce::String (wideLoss, 2)
                        + " dB; in-phase lit " + juce::String (inPhaseLit) + "; General lit " + juce::String (generalLit));
            expect (wideLit > 0 && wide.housePhaseHint, "a wide bass held for more than 4 bars of main section lights the hint");
            // 低频段里还有单声道的底鼓：合起来的相关度约 0.3（不是只有低音时的 0），提示里的损失应深于门限 0.6 对应的 −0.97 dB、浅于 −3 dB。
            expect (wideLoss < 10.0f * std::log10 ((1.0f + 0.6f) / 2.0f) && wideLoss > -3.1f,
                    "the hint reports a mono bass loss beyond the 0.6-correlation threshold (about -1 to -3 dB)");
            expectEquals (inPhaseLit, 0, "an in-phase bass never lights it");
            expectEquals (generalLit, 0, "the General genre never lights it");
            expect (! general.houseActive);
        }

        // 评审 F1（T-038 集成）：House 门控画像只代表满编段，校准只在主段落采样。4 小节鼓前奏 → 6 小节 drop → 8 小节很轻的鼓（breakdown，
        // 比 drop 轻约 20 dB）→ 10 小节 drop，锁定条件 10 / 4：从头播时第 10 个已听小节（drop 的最后一小节）一定稿就锁定，正好进 breakdown。
        // 修前校准在 breakdown 里采样（限幅器不工作，测到的净提升偏高，偏差给成负值）；修后 breakdown 里轮中每块都作废当前子窗（中断位 64）、
        // 不在轮中时不开新一轮（门位 1024），等到第二段 drop 才校准——最终偏差与「前奏后全是 drop」的同一首（锁定与校准都在 drop 里）相差
        // ≤ 0.2 dB。俱乐部档（限幅最多，修前偏差最大）、GLAZE 100。
        void testCalibrationOnlyInMainSection()
        {
            beginTest ("House: calibration samples only the main section; a lock landing in a breakdown waits for the next drop");

            const auto constants = houseConstants (10, 4);
            auto params = houseParams (100.0f, 1);
            params.glazeDelivery = 2;
            const auto withBreakdown = makeHouseSong (defaultRate, { { 4, false, 1.0f }, { 6, true, 1.0f }, { 8, false, 0.1f }, { 10, true, 1.0f } }, 91);
            const auto allDrop = makeHouseSong (defaultRate, { { 4, false, 1.0f }, { 24, true, 1.0f } }, 91);
            const auto breakdownStart = 10.0 * houseBarSeconds, breakdownEnd = 18.0 * houseBarSeconds;

            int sampledInBreakdown = 0, waitedInBreakdown = 0;
            float biasWithBreakdown = 0.0f, biasAllDrop = 0.0f;
            bool validWithBreakdown = false, validAllDrop = false;

            {
                Rig rig (constants, withBreakdown, params);
                rig.tempoBpm = houseBpm;
                rig.run (28.0 * houseBarSeconds - 0.1, [&] (int b)
                {
                    const auto t = secondsOf (b + 1);
                    if (t > breakdownStart + 1.0 && t < breakdownEnd)
                    {
                        const auto pr = rig.probe();
                        const auto outside = (pr.calibrationInterruptMask & 64) != 0 || (pr.calibrationGateMask & 1024) != 0;
                        (outside ? waitedInBreakdown : sampledInBreakdown) += 1;
                    }
                });
                const auto c = rig.committed();
                biasWithBreakdown = c.calibration.gainBiasDb;
                validWithBreakdown = c.calibration.valid && c.profile.houseGate.valid;
            }

            {
                Rig rig (constants, allDrop, params);
                rig.tempoBpm = houseBpm;
                rig.run (28.0 * houseBarSeconds - 0.1);
                const auto c = rig.committed();
                biasAllDrop = c.calibration.gainBiasDb;
                validAllDrop = c.calibration.valid && c.profile.houseGate.valid;
            }

            logMessage ("  breakdown blocks: waited " + juce::String (waitedInBreakdown) + ", sampled " + juce::String (sampledInBreakdown)
                        + "; bias with breakdown " + juce::String (biasWithBreakdown, 3) + " dB, all drop " + juce::String (biasAllDrop, 3) + " dB");
            expect (waitedInBreakdown > 0, "the breakdown blocks are recognised as outside the main section");
            expectEquals (sampledInBreakdown, 0, "no calibration block is sampled in the breakdown");
            expect (validWithBreakdown && validAllDrop, "both runs end with a valid calibration of a House profile");
            expect (std::abs (biasWithBreakdown - biasAllDrop) <= 0.2f, "the calibration bias does not depend on a breakdown after the lock");
        }

        // 评审 F7（Max 2026-10-01「加状态提示」）：House 档还在聆听时离线导出，Status::houseOfflineListening 亮起，导出结束（回到实时）后
        // 照样亮着、直到锁定；通用档同样的操作不亮。
        void testOfflineExportWhileListeningIsFlagged()
        {
            beginTest ("House: an offline export while still listening is flagged and the flag stays until the lock; General never flags");

            const auto song = makeHouseSong (defaultRate, { { 4, false, 1.0f }, { 12, true, 1.0f } }, 77);

            for (const auto genre : { 1, 0 })
            {
                Rig rig (houseConstants (12, 4), song, houseParams (40.0f, genre));
                rig.tempoBpm = houseBpm;
                rig.host.offline = true;
                rig.run (3.0 * houseBarSeconds);
                const auto exporting = rig.status();
                rig.host.offline = false;
                rig.run (1.0 * houseBarSeconds);
                const auto afterExport = rig.status();
                rig.run (14.0 * houseBarSeconds);
                const auto locked = rig.status();

                if (genre == 1)
                {
                    expect (exporting.houseOfflineListening, "offline while House is listening: flagged");
                    expect (afterExport.houseOfflineListening && afterExport.houseLockPending, "back to real time, still listening: still flagged");
                    expect (! locked.houseLockPending && ! locked.houseOfflineListening, "once locked: the flag clears");
                }
                else
                {
                    expect (! exporting.houseOfflineListening && ! afterExport.houseOfflineListening, "General genre: never flagged");
                }
            }
        }

        // 依据：任务书「音频线程 0 分配、0 锁的现有检查覆盖新代码」。一条 House 链走完：暂定方案与快过渡、锁定、重开、重新聆听、
        // 换交付 / 子类 / 回通用再回来、宿主不报拍点、跳播与回绕、GLAZE 回 0 再拧起、读取存档（解析在计数区外），
        // setHostInfo / setParams / process / getStatus 都不经 operator new。
        void testHouseAudioThreadDoesNotAllocate()
        {
            beginTest ("House: setHostInfo / setParams / process / getStatus never allocate through every House path");

            // 安静的前奏 → 首次聆听里 drop 进来（新响段的快过渡）→ 锁定 → 两段各响约 5 LU（两次重开），之后循环。
            const auto song = makeHouseSong (defaultRate, { { 4, false, 0.5f }, { 4, true, 0.6f }, { 6, true, 1.0f }, { 6, true, 1.8f } }, 69);
            Rig rig (houseConstants (6, 2), song, houseParams (60.0f));
            rig.tempoBpm = houseBpm;

            Rig donor (houseConstants (6, 2), song, houseParams (50.0f));
            donor.tempoBpm = houseBpm;
            donor.run (16.0);
            const auto savedState = donor.chain.createGlazeStateXml();

            std::uint64_t allocations = 0;
            auto finite = true;
            const auto total = rig.blocksFor (95.0);

            for (int b = 0; b < total; ++b)
            {
                // ---- 计数区外：准备这一块 ----
                if (b == rig.blocksFor (45.0)) rig.chain.requestGlazeRelearn();
                if (b == rig.blocksFor (52.0)) rig.params.glazeDelivery = 2;
                if (b == rig.blocksFor (55.0)) rig.params.glazeGenre = 0;
                if (b == rig.blocksFor (57.0)) rig.params.glazeGenre = 3;
                if (b == rig.blocksFor (60.0)) rig.tempoBpm = 0.0;
                if (b == rig.blocksFor (64.0)) rig.position += 96000;
                if (b == rig.blocksFor (66.0)) { rig.host.hasLoop = rig.host.looping = true; rig.position -= 48000; }
                if (b == rig.blocksFor (68.0)) { rig.tempoBpm = houseBpm; rig.host.hasLoop = rig.host.looping = false; }
                if (b == rig.blocksFor (72.0)) rig.params.glazePercent = 0.0f;
                if (b == rig.blocksFor (74.0)) rig.params.glazePercent = 70.0f;
                if (b == rig.blocksFor (80.0)) rig.chain.restoreGlazeStateXml (savedState.get());

                const auto length = (size_t) rig.source.front().size();
                rig.buffer.setSize (numChannels, blockSize, false, false, true);

                for (int ch = 0; ch < numChannels; ++ch)
                    for (int i = 0; i < blockSize; ++i)
                        rig.buffer.setSample (ch, i, rig.source[(size_t) ch][(rig.sourceIndex + (size_t) i) % length]);

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
            logMessage ("  House paths: updates " + juce::String (probe.houseUpdates) + " (fast " + juce::String (probe.houseFastTransitions)
                        + "), commits " + juce::String (probe.commits) + ", reopens " + juce::String (probe.houseReopens) + ", House listens "
                        + juce::String (probe.houseListenStarts) + ", jumps " + juce::String (probe.transportJumps) + ", wraps " + juce::String (probe.loopWraps));
            expect (finite, "output finite throughout");
            expect (probe.houseUpdates > 0 && probe.houseFastTransitions > 0 && probe.commits >= 2 && probe.houseReopens >= 1
                    && probe.transportJumps >= 1 && probe.loopWraps >= 1, "the run went through the House paths it claims to cover");
            expectEquals ((juce::int64) allocations, (juce::int64) 0, "audio thread must not allocate");
        }

        // 依据：GlazeController.cpp 开头「T-038」一节「新段里先试后用的代价……再不够就沿用已提交画像的代价」。读档后段落账从空开始、锁定后
        // 候选不跑，重开时新算的画像一个试算块都没有：峰值代价沿用已提交画像的。T-049：混合损失（T-043）与代价出自同一批块，要一起沿用——
        // 原来漏了，重开后的画像混合损失无效，决策器就不按它给低切封顶（capLowCutForBlend 两个候选缺一个就判断不了）。
        void testReopenAfterReloadCarriesBlendLoss()
        {
            beginTest ("House: a reopen after reloading the project carries the blend losses over together with the try-before-use costs");

            const auto song = makeHouseSong (defaultRate, { { 10, true, 0.45f }, { 8, true, 1.4f } }, 78);
            const auto sameFeature = [] (const glaze::Feature& a, const glaze::Feature& b)
            {
                return sameBits (a.value, b.value) && a.valid == b.valid && a.effectiveSeconds == b.effectiveSeconds
                    && sameBits (a.confidence, b.confidence);
            };

            // 第一次锁定在较轻的段上（还没听到更响的段），存档。
            Rig donor (houseConstants (8, 4), song, houseParams (60.0f));
            donor.tempoBpm = houseBpm;
            const auto locked = donor.runUntil (10.0 * houseBarSeconds, [&] { return donor.probe().commits > 0; });
            const auto saved = donor.committed().profile;
            const auto xml = donor.chain.createGlazeStateXml();
            expect (locked && saved.lowCutPeakCostDb.valid && saved.lowCutBlendLossDb[0].valid && saved.lowCutBlendLossDb[1].valid,
                    "the first lock (on the quieter section) measured the costs and both blend losses");

            // 读档后从头播：较轻的段之后，更响的段连续 4 小节 → 重开一次。
            Rig rig (houseConstants (8, 4), song, houseParams (60.0f));
            rig.tempoBpm = houseBpm;
            rig.chain.restoreGlazeStateXml (xml.get());
            rig.step();
            expect (glaze::bitwiseEqual (rig.committed().profile, saved), "the reloaded project carries the saved profile");

            rig.run (17.0 * houseBarSeconds);
            const auto reopened = rig.committed().profile;
            logMessage ("  reopens " + juce::String (rig.probe().houseReopens) + "; I " + juce::String (saved.integratedLufs.value, 2) + " -> "
                        + juce::String (reopened.integratedLufs.value, 2) + " LUFS; blend loss 10 / 20 Hz "
                        + juce::String (reopened.lowCutBlendLossDb[0].value, 4) + " / " + juce::String (reopened.lowCutBlendLossDb[1].value, 4)
                        + " dB (valid " + juce::String ((int) reopened.lowCutBlendLossDb[0].valid) + " / "
                        + juce::String ((int) reopened.lowCutBlendLossDb[1].valid) + ")");
            expectEquals (rig.probe().houseReopens, 1, "the louder section reopens the reloaded profile once");
            expect (reopened.houseGate.reopenCount == 1 && reopened.integratedLufs.value > saved.integratedLufs.value + 3.0f,
                    "the reopened profile is the louder section's");
            expect (sameFeature (reopened.lowCutPeakCostDb, saved.lowCutPeakCostDb) && sameFeature (reopened.widthPeakCostDb, saved.widthPeakCostDb),
                    "no trial blocks since reloading: the committed costs are carried over");

            for (size_t c = 0; c < saved.lowCutBlendLossDb.size(); ++c)
                expect (sameFeature (reopened.lowCutBlendLossDb[c], saved.lowCutBlendLossDb[c]),
                        "the blend loss is carried over with the costs, candidate " + juce::String ((int) c));
        }
    };

    static GlazeControllerHouseTests glazeControllerHouseTests;
}

//==============================================================================
// ---- T-097 守护族真链机制（A2 追加）----
// 落差守门（ContrastGuard）与 PSR 守门（PsrGuard）按 100 ms 子块推进、G_trim 抗积分饱和的冻结口径。都经 CeladonChain 驱动（与插件同一
// 接线）。依据：设计规格-实时GLAZE-主段与增益 第 4 节（下称「规格 4」）、第 5 节（窗口状态不存）；Max 2026-10-10 Q11「有退让量就冻结」。
// 只在文件末尾追加：上面的用例与工具一个不改，另起一个测试类（GLAZE controller guard family (T-097)）。
//   1a. G_trim 冻结（规格 4 + Q11）：任一守门退让量 < 0、或本块改过退让量、或 1 s 内改过参数 → 本块 G_trim 不积分（appliedBiasDb 逐位
//       不变）；trimFrozen 与这三条原因逐块一致。块长 480（= 预算环 10 ms 时隙；100 ms 子块 = 10 块）：链从不在块内切片，探针即那一次 endBlock。
//   1b. 改参数冻结（规格 4「参数在 1 s 内变化过时冻结 G_trim」）：GLAZE 100 → 90 之后 100 块（1 s）内冻结、逐位不变、距改参数 < 1 s；
//       之后三个守门都为 0 时 G_trim 恢复积分。
//   2.  通用档（规格 4 曲风开关：通用两个守门都关，GlazeConstants generalGuards 出厂 {false, false}）：GLAZE 100、三种交付、推热到预算环
//       退让，落差 / PSR 退让每一块都是 +0（位模式）。
//   3.  子块网格（规格 4「两个守门每个 100 ms 子块走一步」，与宿主块长、切片方式无关）：块长 1 ～ 4096，推进次数 = ⌊总样本 / 4800⌋、
//       最后一个推进的子块终点 = 其 × 4800、不丢；每块最多推进一个（块长 ≤ 4096 < 4800）；终点永远在 4800 的整数倍上。
//   4.  冻结与不连续（规格 4「冻结：三个守门与 G_trim 都不推进」「GLAZE ≤ 0：自动部分逐位归零，守门不推进」；GlazeController.cpp
//       stepSubBlockGuards）：宿主旁通期间不推进、退让保持；恢复后跨过冻结段的子块丢掉（+1）；跳播后出口 3 s 窗重攒（≥ 29 个推进的子块
//       读数无效）；GLAZE 0 不推进、退让全 +0。
//   5.  旧档兼容（ContrastGuard.h / PsrGuard.h 文件头的存档说明；规格第 5 节）：main 3a7a367 形状的 LIVE（CONTRAST_GUARD 只有 128 笔环，
//       PSR_GUARD 没有 aboveSeconds，CONTROL 没有 secondsSinceParamChange）读回不冷启动、退让照旧、直方图从空开始、缺的
//       secondsSinceParamChange 按 1 s；新档 create → restore → create 时 T-097 的三个节点（CONTROL / CONTRAST_GUARD / PSR_GUARD）
//       逐位相同（GlazeController.h「restoreStateXml 之后、第一块之前 createStateXml 返回刚恢复的内容」）。
//   6.  音频线程零分配（设计规格-总体「音频线程零分配、零锁」）：House 俱乐部 GLAZE 100、通用 GLAZE 100 两种设置走过冻结、跳播、回绕、
//       宿主 reset、GLAZE 0、换交付与曲风、读档。
// 【待上机核】的数（素材电平、推热倍数、时长）都集中在各用例开头的具名常量里，注释写明为什么预期够用。
namespace celadon::test
{
    namespace
    {
        // 子块长度：IN / OUT 表与两个守门都是 round(0.1 × 采样率) 个样本一个子块（LoudnessMeter::getSubBlockLength，GlazeController 的
        // prepare 同一算式）；48 kHz → 4800。prepare 之后两只表的时间轴从 0 起，第 n 个子块的终点是 n × 4800。
        constexpr std::int64_t t097SubBlockSamples = 4800;

        // 块长 480 = 预算环一个 10 ms 时隙（48 kHz）；100 ms 子块 = 10 个这样的块。时隙只按有效音乐计数、冻结时丢掉半格，但每片都是整
        // 480 个样本，所以每块开头时隙进度总是 0：链从不在块内再切（getLastProcessSliceCountForTesting = 1），每块正好一次 endBlock——
        // 「上一块 endBlock 之后」的探针就是 G_trim 这一步的判定结果。块长 512 时几乎每块都切成两片，片内的判定从块外看不到。
        constexpr int t097TrimBlock = 480;

        // 规格 4：参数在 1 s 内变化过时冻结 G_trim（GlazeController.cpp changeFreezeSeconds）。
        constexpr double t097ParamFreezeSeconds = 1.0;

        // 规格 4：出口 3 s 窗 = 30 个 100 ms 子块（ProfileMath ShortTermWindow::subBlocks），满 30 个才有 S / PSR 读数。
        constexpr int t097WindowSubBlocks = 30;

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

        // 三个守门任一有退让量（预算 guardDb、落差、PSR；在退、停住、回程中都 < 0）。
        bool t097AnyRelief (const Probe& p) noexcept
        {
            return p.guardDb < 0.0f || p.contrastReliefDb < 0.0f || p.psrReliefDb < 0.0f;
        }

        // 三个退让量任一与上一块不同（位比较）。
        bool t097ReliefMoved (const Probe& now, const Probe& before) noexcept
        {
            return ! sameBits (now.guardDb, before.guardDb) || ! sameBits (now.contrastReliefDb, before.contrastReliefDb)
                || ! sameBits (now.psrReliefDb, before.psrReliefDb);
        }

        // 三个退让量都是 +0（位模式；−0 也算不一致）。
        bool t097AllReliefsZero (const Probe& p) noexcept
        {
            return sameBits (p.guardDb, 0.0f) && sameBits (p.contrastReliefDb, 0.0f) && sameBits (p.psrReliefDb, 0.0f);
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

    class GlazeControllerGuardFamilyTests  : public juce::UnitTest
    {
    public:
        GlazeControllerGuardFamilyTests() : juce::UnitTest ("GLAZE controller guard family (T-097)", "Celadon") {}

        void runTest() override
        {
            testTrimFreezesWhileAnyGuardHolds();
            testTrimFreezesForOneSecondAfterAParamChange();
            testGeneralGenreKeepsNewGuardsAtZero();
            testSubBlockGridIndependentOfBlockSize();
            testFreezeAndDiscontinuities();
            testLegacyGuardNodesAndRoundTrip();
            testGuardFamilyNeverAllocates();
        }

        // 1a. 依据：规格 4「任一守门退让量 < 0（预算 / 落差 / PSR，在退、停住、回程中都算）、或这一块刚改过某个退让量、或 1 s 内改过
        //     参数 → 不积分」；Max 2026-10-10 Q11「有退让量就冻结」。GlazeController.cpp endBlock：trimFrozen = guardHolding || recentlyChanged；
        //     guardHolding = 预算环本块在退 || 三个退让量任一 < 0 || 三个任一本块变了；只有 trimFrozen 为假时 gTrim 才积分；appliedBiasDb = gTrim。
        //     断言（每块；块长 480，一块一片）：
        //       - 有退让量 ⇒ trimFrozen（「trimFrozen 与有退让量一致」）；
        //       - trimFrozen ⇔ 有退让量 || 退让量与上一块不同 || 上一块之后 secondsSinceParamChange < 1 s（本段不改参数，beginBlock 不清零，
        //         这一块判定时的「距改参数」就是上一块之后的值）；预算环「本块在退」= g_保护本块变小，已含在「退让量变了」里；
        //       - 有退让量或 trimFrozen ⇒ appliedBiasDb 与上一块逐位相同。
        void testTrimFreezesWhileAnyGuardHolds()
        {
            beginTest ("G_trim holds bit-exactly on every block that ends with any guard relief below 0 (Q11); trimFrozen matches its reasons");

            // 【待上机核】推热配方同主类 testManualComesFirst（紧预算、冲击风格、makeMusic 种子 25 峰值 0.3、×8；那里另加手动 Gain 3 dB，
            // 8 s 内保护层必退）。这里不加手动 Gain，限幅器入口少 3 dB：峰值 0.3 × 8 = 2.4（+7.6 dBFS）对 −1 dBTP 的 Ceiling 仍有约 8.6 dB
            // 峰值 GR，远超冲击风格紧预算的峰值 3 dB / P95 2 dB，预期 1 ～ 2 s 内预算环开始退、之后一直有退让量。
            constexpr float hotGain = 8.0f;
            constexpr double warmSeconds = 2.0;
            constexpr double hotSeconds = 12.0;

            Rig rig (withTightLimitBudgets (fastConstants()), makeMusic (defaultRate, 8.0, 25, 0.3f), defaultParams (100.0f, 2),
                     defaultRate, t097TrimBlock);
            rig.run (warmSeconds);
            rig.sourceGain = hotGain;

            auto previous = rig.probe();
            const auto trimAtStart = previous.appliedBiasDb;
            auto singleSlice = true, reliefFreezes = true, reasonsMatch = true, heldBitExact = true;
            int reliefBlocks = 0, frozenBlocks = 0, firstFailure = -1;
            auto lowestBudget = 0.0f;

            rig.run (hotSeconds, [&] (int b)
            {
                const auto p = rig.probe();
                const auto oneSlice = rig.chain.getLastProcessSliceCountForTesting() == 1;
                singleSlice = singleSlice && oneSlice;

                if (oneSlice)
                {
                    const auto relief = t097AnyRelief (p);
                    const auto expectedFrozen = relief || t097ReliefMoved (p, previous)
                                             || previous.secondsSinceParamChange < t097ParamFreezeSeconds;
                    const auto freezesOk = ! relief || p.trimFrozen;
                    const auto reasonOk = p.trimFrozen == expectedFrozen;
                    const auto heldOk = ! (relief || p.trimFrozen) || sameBits (p.appliedBiasDb, previous.appliedBiasDb);
                    reliefFreezes = reliefFreezes && freezesOk;
                    reasonsMatch = reasonsMatch && reasonOk;
                    heldBitExact = heldBitExact && heldOk;

                    if (! (freezesOk && reasonOk && heldOk) && firstFailure < 0)
                        firstFailure = b;

                    reliefBlocks += relief ? 1 : 0;
                    frozenBlocks += p.trimFrozen ? 1 : 0;
                }

                lowestBudget = juce::jmin (lowestBudget, p.guardDb);
                previous = p;
            });

            logMessage ("  hot material x" + juce::String (hotGain, 1) + ": blocks with a relief " + juce::String (reliefBlocks) + " / frozen "
                        + juce::String (frozenBlocks) + " of " + juce::String (rig.blocksFor (hotSeconds)) + "; lowest budget relief "
                        + juce::String (lowestBudget, 3) + " dB; G_trim " + juce::String (trimAtStart, 5) + " -> "
                        + juce::String (previous.appliedBiasDb, 5) + " dB; first failing block " + juce::String (firstFailure));
            expect (singleSlice, "fixture: with 480-sample blocks every host block is exactly one slice (one endBlock per probe)");
            expect (reliefBlocks > rig.blocksFor (1.0), "fixture: the hot material keeps the budget guard retreated for more than a second");
            expect (reliefFreezes, "any relief below 0 after a block means trimFrozen on that block");
            expect (reasonsMatch, "trimFrozen == relief below 0 || a relief changed this block || a parameter changed within 1 s");
            expect (heldBitExact, "a block that ends with a relief below 0 (or trimFrozen) leaves appliedBiasDb bit-identical");
        }

        // 1b. 依据：规格 4「参数在 1 s 内变化过时冻结 G_trim」（beginBlock 的 changed：风格 / 曲风 / 交付 / GLAZE / 规划相关手动值）；
        //     GlazeController.cpp：changed 时 secondsSinceChange = 0；endBlock 判定 recentlyChanged = secondsSinceChange < 1 之后才 += dt。
        //     推导（块长 480、一块一片，dt = 480 / 48000 = 0.01 s）：改参数那一块记为第 1 块，第 k 块判定时距改参数 = (k − 1) × 0.01：
        //       - k = 1 ～ 100：≤ 0.99 < 1 → 冻结，appliedBiasDb 与改之前逐位相同；
        //       - 第 k 块之后探针 secondsSinceParamChange = min(1, k × 0.01)：k ≤ 99 时 ≤ 0.99 < 1；第 1 块之后正好是 dt（同一个算式）；
        //       - k = 101：判定时是 100 个 0.01 的累加，舍入可能差一个 ulp 落在 1 的任一边，不断言；k ≥ 102 一定不再因改参数冻结。
        //     对照：放宽预算（预算环不设约束）、通用档（两个新守门出厂关），三个退让量都是 +0，冻结只能来自改参数。
        void testTrimFreezesForOneSecondAfterAParamChange()
        {
            beginTest ("a planning parameter change freezes G_trim for 1 s; afterwards, with every guard at 0, G_trim integrates again");

            // 【待上机核】素材峰值 0.2：输出远低于 GLAZE 100 的目标响度，G_trim 的误差一直为正、按 0.05 dB/s 往上积（十几秒远到不了 +3 dB
            // 上限）。热身 6 s：开头 1 s 是 prepare 后第一块的「改参数」冻结，目标响度要等方案有了画像才有限（主类 testContinuousPhase 在
            // 4 s 时断言有限），之后 2 s 里应能看到不冻结且 G_trim 在动的块。
            constexpr double warmSeconds = 6.0;
            constexpr double movingWindowSeconds = 2.0;
            constexpr double resumeWithinSeconds = 3.0;

            const auto constants = t097WithoutBudgetCaps (fastConstants());
            const auto& switches = glaze::guardSwitchesFor (constants, glaze::Genre::general);
            expect (! switches.contrast && ! switches.psr, "fixture: both new guards are off for the general genre (factory switches)");

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

            expect (guardsQuiet, "fixture: with no budget caps and the general genre all three reliefs stay +0");
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

        // 2. 依据：规格 4 曲风开关——落差守门「4/4 型开、其余先关」、PSR 守门「House / 通用关」；GlazeConstants generalGuards 出厂
        //    {false, false}；关着的守门照常量测，已有退让按回程速率放回 0（从 0 起：min(0, 0 + 步长) = +0，位模式不变）。
        //    断言：通用档、GLAZE 100、三种交付各跑一段（含推热、预算环会退的素材），每一块之后 contrastReliefDb、psrReliefDb 的位模式都是 +0；
        //    两个守门确实在推进（guardSubBlocks > 0：+0 是它们算出来的，不是没走）。
        void testGeneralGenreKeepsNewGuardsAtZero()
        {
            beginTest ("general genre at GLAZE 100: contrast and PSR reliefs are +0 on every block for all three deliveries, even on hot material");

            // 【待上机核】同用例 1a 的推热配方。俱乐部档的限幅上限另加 DeliveryConstants::clubLimitGr*ExtraDb，约 8.6 dB 的峰值 GR 预期仍超出。
            constexpr float hotGain = 8.0f;
            constexpr double warmSeconds = 2.0;
            constexpr double hotSeconds = 12.0;
            const char* const deliveryNames[] = { "standard", "streaming", "club" };

            for (int delivery = 0; delivery < glaze::numDeliveries; ++delivery)
            {
                const juce::String tag (deliveryNames[delivery]);
                auto params = defaultParams (100.0f, 2);
                params.glazeDelivery = delivery;
                Rig rig (withTightLimitBudgets (fastConstants()), makeMusic (defaultRate, 8.0, 25, 0.3f), params);

                auto positiveZero = true;
                auto lowestBudget = 0.0f;
                auto lowestPsr = std::numeric_limits<float>::max();

                const auto check = [&] (int)
                {
                    const auto p = rig.probe();
                    positiveZero = positiveZero && sameBits (p.contrastReliefDb, 0.0f) && sameBits (p.psrReliefDb, 0.0f);
                    lowestBudget = juce::jmin (lowestBudget, p.guardDb);

                    if (p.outputPsrValid)
                        lowestPsr = juce::jmin (lowestPsr, p.outputPsrDb);
                };

                rig.run (warmSeconds, check);
                rig.sourceGain = hotGain;
                rig.run (hotSeconds, check);

                const auto p = rig.probe();
                logMessage ("  " + tag + ": lowest budget relief " + juce::String (lowestBudget, 3) + " dB; lowest output 3 s PSR "
                            + (lowestPsr < std::numeric_limits<float>::max() ? juce::String (lowestPsr, 2) + " dB" : juce::String ("(never valid)"))
                            + "; sub-blocks stepped " + juce::String ((juce::int64) p.guardSubBlocks));
                expect (positiveZero, tag + ": contrastReliefDb and psrReliefDb are +0 (bit pattern) after every block");
                expect (lowestBudget < 0.0f, tag + ": fixture: the hot material makes the budget guard retreat");
                expect (p.guardSubBlocks > 0, tag + ": the sub-block guards were stepped (the zeros are their output, not a skipped step)");
            }
        }

        // 3. 依据：规格 4「两个守门每个 100 ms 子块走一步」；GlazeController.cpp stepSubBlockGuards：每片 endBlock 读两只表新完成的子块，
        //    链按 getGuardSamplesUntilBoundary 切片，每个子块都在某一片的末尾完成、在那一片里推进一次。
        //    推导：prepare 之后两只表从 0 计，跑了 N = 块数 × 块长 个样本，完成的子块 = ⌊N / 4800⌋ 个、最后一个的终点 = ⌊N / 4800⌋ × 4800；
        //    全程 GLAZE 100、不冻结、无跳播 → 一个都不丢；块长 ≤ 4096 < 4800 → 一块里最多完成一个子块；推进的子块终点都在 4800 的整数倍上。
        void testSubBlockGridIndependentOfBlockSize()
        {
            beginTest ("the sub-block guards step exactly once per 100 ms OUT sub-block, whatever the host block size");

            constexpr double runSeconds = 12.0;
            // 块长 1：每个样本一次整链 process，12 s 就是 57.6 万次调用，太慢；只跑 3 s（30 个子块）——网格断言与时长无关。
            constexpr double singleSampleSeconds = 3.0;
            const auto material = makeMusic (defaultRate, runSeconds, 35, 0.35f);

            for (const auto block : { 1, 7, 32, 64, 480, 512, 4096 })
            {
                const auto tag = "block " + juce::String (block);
                Rig rig (fastConstants(), material, defaultParams (100.0f), defaultRate, block);
                auto previous = rig.probe();
                auto atMostOne = true, onGrid = true;
                std::int64_t largestStep = 0;

                rig.run (block == 1 ? singleSampleSeconds : runSeconds, [&] (int)
                {
                    const auto p = rig.probe();
                    const auto delta = p.guardSubBlocks - previous.guardSubBlocks;
                    largestStep = juce::jmax (largestStep, delta);
                    atMostOne = atMostOne && delta >= 0 && delta <= 1;
                    onGrid = onGrid && p.lastGuardSubBlockEnd % t097SubBlockSamples == 0;
                    previous = p;
                });

                const auto p = rig.probe();
                const auto totalSamples = (std::int64_t) rig.blocksRun * block;
                const auto expectedSubBlocks = totalSamples / t097SubBlockSamples;
                logMessage ("  " + tag + ": " + juce::String ((juce::int64) totalSamples) + " samples -> stepped "
                            + juce::String ((juce::int64) p.guardSubBlocks) + " sub-blocks (last end " + juce::String ((juce::int64) p.lastGuardSubBlockEnd)
                            + "), dropped " + juce::String ((juce::int64) p.guardSubBlocksDropped) + ", largest step per block "
                            + juce::String ((juce::int64) largestStep));
                expectEquals ((juce::int64) p.guardSubBlocks, (juce::int64) expectedSubBlocks,
                              tag + ": stepped sub-blocks == floor(total samples / 4800)");
                expectEquals ((juce::int64) p.lastGuardSubBlockEnd, (juce::int64) (expectedSubBlocks * t097SubBlockSamples),
                              tag + ": the last stepped sub-block ends at floor(total / 4800) x 4800");
                expectEquals ((juce::int64) p.guardSubBlocksDropped, (juce::int64) 0, tag + ": no sub-block is dropped");
                expect (atMostOne, tag + ": at most one sub-block is stepped per host block");
                expect (onGrid, tag + ": lastGuardSubBlockEnd is always a multiple of 4800");
            }
        }

        // 4. 依据：规格 4「冻结（宿主旁通、听原声、Color / Limit 旁通）：三个守门与 G_trim 都不推进」「GLAZE ≤ 0：所有自动部分逐位归零，
        //    守门不推进」；GlazeController.cpp stepSubBlockGuards：不推进的片把 guardTaintedThrough 推到片尾、断开两个守门的 3 s 窗（计时与
        //    退让保持）；之后起点早于它的子块断开窗口并丢掉（guardSubBlocksDropped +1）；跳播置 guardWindowBreakPending，窗口同样断开重攒。
        //    素材与开关：通用档的 PSR 守门出厂关着，这里打开它（只改测试常数），冻结前要有一份非零的 PSR 退让可「保持」。素材是均匀白噪声：
        //    幅度 a 时峰值 a、每声道均方 a²/3；K 加权的高架（约 1.5 kHz 以上 +4 dB）罩住白噪声绝大部分能量，均方约 ×2.5；两声道相加
        //    → S ≈ −0.691 + 10·log10(2 × 2.5 × a²/3)，a = 0.3 时约 −8.9 LUFS；采样峰值 −10.5 dBFS，样本间过冲按 +3 dB 算真峰值也只有约 −7.5 dBTP：PSR 至多约 1.5 dB，
        //    远低于标准档门槛 8 − 0.5 = 7.5 dB；限幅只会把它压得更低。时间：窗满 3 s + 持续 3 s 后以 0.5 dB/s 退，约 6.4 s 退过 −0.2 dB。
        void testFreezeAndDiscontinuities()
        {
            beginTest ("host bypass freezes the sub-block guards; the straddling sub-block is dropped; a jump refills the 3 s window; GLAZE 0 keeps them at 0");

            constexpr double psrRetreatWithinSeconds = 15.0;
            constexpr double freezeSeconds = 2.0;
            constexpr double refillSeconds = 4.0;           // > 3 s 窗
            constexpr double jumpSeconds = 10.0;            // 跳播距离（远大于 50 ms 容差）
            constexpr double revalidateWithinSeconds = 6.0;
            constexpr double glazeZeroSeconds = 2.0;

            auto constants = fastConstants();
            constants.generalGuards.psr = true;
            Rig rig (constants, makeNoise (numChannels, (int) (8.0 * defaultRate), 0.3f, 51), defaultParams (100.0f));

            const auto retreated = rig.runUntil (psrRetreatWithinSeconds, [&] { return rig.probe().psrReliefDb < -0.2f; });
            expect (retreated && rig.probe().outputPsrValid, "fixture: on white noise the output PSR is valid and the PSR guard retreats below -0.2 dB");

            // ---- 宿主旁通：不推进、不丢、退让保持 ----
            const auto before = rig.probe();
            rig.hostBypassed = true;
            auto held = true;

            const auto checkHeld = [&]
            {
                const auto p = rig.probe();
                held = held && p.guardSubBlocks == before.guardSubBlocks && p.guardSubBlocksDropped == before.guardSubBlocksDropped
                       && sameBits (p.contrastReliefDb, before.contrastReliefDb) && sameBits (p.psrReliefDb, before.psrReliefDb);
            };

            rig.run (freezeSeconds, [&] (int) { checkHeld(); });

            // 冻结段终点正好落在子块边界上就没有「跨过冻结段」的子块：再冻一块（512 < 4800，相邻两个块尾不会都在边界上）。
            if (((std::int64_t) rig.blocksRun * blockSize) % t097SubBlockSamples == 0)
            {
                rig.step();
                checkHeld();
            }

            const auto freezeEnd = (std::int64_t) rig.blocksRun * blockSize;
            expect (held, "host bypass: no sub-block is stepped or dropped, both reliefs are held bit-exactly");
            expect (before.psrReliefDb < 0.0f, "fixture: the held PSR relief is non-zero");

            // ---- 恢复：跨过冻结段的子块丢掉，下一个子块是恢复后第一个推进的 ----
            // 推导：freezeEnd 不在子块边界上 → 子块 [⌊freezeEnd / 4800⌋ × 4800, +4800) 起点在冻结段里，丢掉（+1）；下一个子块终点
            // (⌊freezeEnd / 4800⌋ + 2) × 4800 推进守门（+1）。两者相隔 4800 样本，不在同一个 512 的块里。窗口被断开过，这一步的读数无效。
            rig.hostBypassed = false;
            const auto frozen = rig.probe();
            const auto resumed = rig.runUntil (1.0, [&] { return rig.probe().guardSubBlocks > frozen.guardSubBlocks; });
            const auto after = rig.probe();
            expect (resumed, "after the bypass the guards step again within a second");
            expectEquals ((juce::int64) (after.guardSubBlocksDropped - frozen.guardSubBlocksDropped), (juce::int64) 1,
                          "the sub-block straddling the end of the freeze is dropped");
            expectEquals ((juce::int64) (after.guardSubBlocks - frozen.guardSubBlocks), (juce::int64) 1, "then exactly one sub-block is stepped");
            expectEquals ((juce::int64) after.lastGuardSubBlockEnd,
                          (juce::int64) ((freezeEnd / t097SubBlockSamples + 2) * t097SubBlockSamples),
                          "the first stepped sub-block is the one after the straddling one");
            expect (! after.outputPsrValid, "the 3 s window was broken by the freeze: the first reading after it is invalid");

            // ---- 跳播：出口 3 s 窗重攒 ----
            // 推导：窗 30 个子块（规格 4），清空后第 1 ～ 29 个推进的子块读数无效、第 30 个起有效（白噪声 S ≫ −70 LUFS）；跨过跳点的那个子块
            // 被丢、不算推进。所以跳播后至少 29 个推进的子块 outputPsrValid 为假，之后恢复有效。
            rig.run (refillSeconds);
            const auto beforeJump = rig.probe();
            expect (beforeJump.outputPsrValid, "fixture: the window is full again before the jump");

            rig.position += (std::int64_t) (jumpSeconds * defaultRate);
            auto last = beforeJump;
            int invalidSteps = 0;
            auto validAgain = false;
            const auto jumpBlocks = rig.blocksFor (revalidateWithinSeconds);

            for (int b = 0; b < jumpBlocks && ! validAgain; ++b)
            {
                rig.step();
                const auto p = rig.probe();

                if (p.guardSubBlocks > last.guardSubBlocks)
                {
                    if (p.outputPsrValid)
                        validAgain = true;
                    else
                        invalidSteps += (int) (p.guardSubBlocks - last.guardSubBlocks);
                }

                last = p;
            }

            logMessage ("  freeze end " + juce::String ((juce::int64) freezeEnd) + "; after the jump " + juce::String (invalidSteps)
                        + " stepped sub-blocks read an invalid PSR before it turned valid again");
            expectEquals (rig.probe().transportJumps, beforeJump.transportJumps + 1, "the playhead move is counted as one jump");
            expect (invalidSteps >= t097WindowSubBlocks - 1, "after a jump at least 29 stepped sub-blocks read an invalid output PSR (window refills)");
            expect (validAgain, "the output PSR turns valid again once the window is full");

            // ---- GLAZE 0：不推进，三个退让量与 G_trim 都是 +0 ----
            const auto beforeZero = rig.probe();
            expect (beforeZero.psrReliefDb < 0.0f, "fixture: a PSR relief is in place before GLAZE goes to 0");
            rig.params.glazePercent = 0.0f;
            auto zeroHeld = true;

            rig.run (glazeZeroSeconds, [&] (int)
            {
                const auto p = rig.probe();
                zeroHeld = zeroHeld && p.guardSubBlocks == beforeZero.guardSubBlocks && t097AllReliefsZero (p) && sameBits (p.appliedBiasDb, 0.0f);
            });

            expect (zeroHeld, "GLAZE 0: no sub-block is stepped; budget / contrast / PSR reliefs and G_trim are +0 on every block");
        }

        // 5. 依据：ContrastGuard.h「读到没有直方图的旧节点：退让照旧，计时与直方图从空开始（不冷启动整份 LIVE）」；PsrGuard.h「aboveSeconds
        //    旧档缺这个属性按 0 读」；GlazeController.cpp restoreBody「旧存档没有 secondsSinceParamChange → 按很久以前改的读（1 s，不冻结）」；
        //    规格第 5 节「窗口状态不存」；GlazeController.h「restoreStateXml 之后、第一块音频之前调 createStateXml，返回的是刚恢复的内容」。
        //    main 3a7a367 的存档形状（git show 3a7a367 核对过）：CONTRAST_GUARD = reliefDb / floorDb / engaged / ringCount / ringWrite /
        //    samplesUntilPush / inputRing / outputRing；PSR_GUARD = reliefDb / floorDb / belowSeconds / engaged；CONTROL 没有
        //    secondsSinceParamChange；其余节点（LIVE_PROFILE、BUDGET_GUARD、OUTPUT_LEVEL、CLAY）与本分支同形。新写法把这些旧属性原样留着
        //    （128 笔环写 0），所以从新档里删掉 T-097 新增的属性，得到的就是 main 形状（环里的数旧版本身也不读）。
        //    「不冷启动」的判据同主类 testLiveRoundTrip：增量与 G_trim 回到存档值、LIVE 画像 I 仍有效、没有旧工程提示；外加两个退让量回到
        //    写进去的非零值（冷启动会清成 0）。
        void testLegacyGuardNodesAndRoundTrip()
        {
            beginTest ("a LIVE in the main 3a7a367 shape restores without a cold start; the T-097 nodes of a new LIVE round-trip bit-exactly");

            // 读档后再跑的时长。直方图推导：两侧 3 s 窗满之后每个有效子块「先按 e^(−0.1/180) 遗忘、再进账 0.1 s」，从空开始跑 t 秒至多
            // 积到 t（留一个子块的余量）；供体跑 24 s 约 180 × (1 − e^(−21/180)) ≈ 19.8 s，带直方图读回的一份再跑 t 秒至少还剩
            // 19.8 × e^(−t/180)，t = 6 时 ≈ 19.1 s > t。
            constexpr double afterSeconds = 6.0;
            constexpr double donorSeconds = 24.0;
            const auto material = makeMusic (defaultRate, 20.0, 43, 0.4f);
            const auto params = defaultParams (40.0f);

            Rig donor (fastConstants(), material, params);
            donor.tempoBpm = 120.0;
            donor.run (donorSeconds);
            const auto saved = donor.chain.createGlazeStateXml();
            const auto donorProbe = donor.probe();
            const auto donorI = donor.chain.getLiveProfile().getProfile().integratedLufs;
            std::unique_ptr<juce::XmlElement> savedBody;

            if (saved != nullptr)
                savedBody = t097LiveBody (*saved);

            logMessage ("  donor: I valid " + juce::String ((int) donorI.valid) + ", contrast histogram "
                        + juce::String (donorProbe.contrastHistogramSeconds, 2) + " s, G_trim " + juce::String (donorProbe.appliedBiasDb, 5) + " dB");
            expect (saved != nullptr && savedBody != nullptr && donorI.valid && donorProbe.contrastHistogramSeconds > 2.0 * afterSeconds,
                    "fixture: the donor has a LIVE profile and more than " + juce::String (2.0 * afterSeconds, 0) + " s of contrast histogram");

            if (saved == nullptr || savedBody == nullptr)
                return;

            // ---- 新档：create → restore → create，T-097 的三个节点逐位相同 ----
            {
                Rig fresh (fastConstants(), material, params);
                fresh.chain.restoreGlazeStateXml (saved.get());
                const auto again = fresh.chain.createGlazeStateXml();
                std::unique_ptr<juce::XmlElement> againBody;

                if (again != nullptr)
                    againBody = t097LiveBody (*again);

                juce::String differing;

                for (auto* child : savedBody->getChildIterator())
                    if (! t097SameChild (savedBody.get(), againBody.get(), child->getTagName()))
                        differing << " " << child->getTagName();

                // 整份 LIVE 逐位相同只记日志、不断言：LIVE_PROFILE 读档时把存档里还没发布的段并进画像（LiveProfileTests「e」：
                // 「读回的副本已经并进来了」），再存时 pending 不同，那不是 T-097 的事（见返回的「待用户拍板」）。T-097 的三个节点逐位断言。
                logMessage ("  new LIVE round trip: whole container identical " + juce::String ((int) (again != nullptr && again->isEquivalentTo (saved.get(), false)))
                            + "; differing nodes:" + (differing.isEmpty() ? juce::String (" none") : differing));

                for (const char* tag : { "CONTROL", "CONTRAST_GUARD", "PSR_GUARD" })
                    expect (t097SameChild (savedBody.get(), againBody.get(), tag), "new LIVE: " + juce::String (tag) + " round-trips bit-identically");
            }

            // ---- 改成 main 3a7a367 形状 ----
            auto body = t097LiveBody (*saved);
            auto* controlNode = body != nullptr ? body->getChildByName ("CONTROL") : nullptr;
            auto* contrastNode = body != nullptr ? body->getChildByName ("CONTRAST_GUARD") : nullptr;
            auto* psrNode = body != nullptr ? body->getChildByName ("PSR_GUARD") : nullptr;
            expect (controlNode != nullptr && contrastNode != nullptr && psrNode != nullptr, "fixture: the LIVE body has CONTROL / CONTRAST_GUARD / PSR_GUARD");

            if (controlNode == nullptr || contrastNode == nullptr || psrNode == nullptr)
                return;

            controlNode->removeAttribute ("secondsSinceParamChange");

            for (const char* attribute : { "overSeconds", "underSeconds", "histogramSeconds", "inputHistogram", "outputHistogram" })
                contrastNode->removeAttribute (attribute);

            psrNode->removeAttribute ("aboveSeconds");

            // 退让写成可辨认的非零值（xmlio：float 写 IEEE 位的 8 位小写十六进制）：
            //   −1.25 = −1.01b × 2⁰：符号 1、指数 127（0x7f）、尾数 .01b → 0x200000 → 0x80000000 | 0x3f800000 | 0x00200000 = 0xbfa00000；
            //   −0.75 = −1.1b × 2⁻¹：符号 1、指数 126（0x7e）、尾数 .1b → 0x400000 → 0x80000000 | 0x3f000000 | 0x00400000 = 0xbf400000。
            //   都在两个守门的下限 −3 dB 之上。
            constexpr float legacyContrastRelief = -1.25f;
            constexpr float legacyPsrRelief = -0.75f;
            contrastNode->setAttribute ("reliefDb", "bfa00000");
            psrNode->setAttribute ("reliefDb", "bf400000");

            auto oldShape = contrastNode->getNumAttributes() == 8 && psrNode->getNumAttributes() == 4
                         && ! controlNode->hasAttribute ("secondsSinceParamChange");

            for (const char* attribute : { "reliefDb", "floorDb", "engaged", "ringCount", "ringWrite", "samplesUntilPush", "inputRing", "outputRing" })
                oldShape = oldShape && contrastNode->hasAttribute (attribute);

            for (const char* attribute : { "reliefDb", "floorDb", "belowSeconds", "engaged" })
                oldShape = oldShape && psrNode->hasAttribute (attribute);

            expect (oldShape, "fixture: CONTRAST_GUARD / PSR_GUARD / CONTROL now have exactly the main 3a7a367 attributes");

            auto legacy = std::make_unique<juce::XmlElement> (*saved);
            legacy->removeChildElement (legacy->getChildByName (glaze::liveTag), true);
            legacy->addChildElement (glaze::compressLiveBody (*body, glaze::factoryConstants().version).release());

            // 先走一块，再把 GLAZE 拧下 10 再拧回（各一块）：插件刚开的第一块不算改参数（复查 F1），要真改一次参数，
            // secondsSinceParamChange 才停在一块的时长；读档后看它是不是换成了旧档的默认 1 s。参数是 GLAZE 40。
            Rig target (fastConstants(), material, params);
            target.tempoBpm = 120.0;
            target.step();
            target.params.glazePercent -= 10.0f;
            target.step();
            target.params.glazePercent = params.glazePercent;
            target.step();
            const auto beforeRestore = target.probe();
            target.chain.restoreGlazeStateXml (legacy.get());
            const auto restored = target.probe();
            const auto restoredI = target.chain.getLiveProfile().getProfile().integratedLufs;

            expect (beforeRestore.secondsSinceParamChange < t097ParamFreezeSeconds, "fixture: the target's change clock is below 1 s before the restore");
            expect (sameIncrements (restored.increments, donorProbe.increments) && sameBits (restored.appliedBiasDb, donorProbe.appliedBiasDb),
                    "old shape: the applied increments and G_trim are restored (no cold start)");
            // I 只断言有效（冷启动会清成无效）；是否逐位等于供体只记日志——读档时并进未发布的段可能让 I 动一点（同上）。
            logMessage ("  old shape restored: I " + juce::String (restoredI.value, 4) + " LUFS (donor " + juce::String (donorI.value, 4)
                        + "), bit-identical " + juce::String ((int) sameBits (restoredI.value, donorI.value)));
            expect (restoredI.valid, "old shape: the LIVE profile is kept (I still valid: no cold start)");
            expect (! target.status().legacySessionDiscarded, "old shape: no legacy-session notice");
            expect (sameBits (restored.contrastReliefDb, legacyContrastRelief), "old shape: the contrast relief is the saved -1.25 dB");
            expect (sameBits (restored.psrReliefDb, legacyPsrRelief), "old shape: the PSR relief is the saved -0.75 dB");
            expectEquals (restored.contrastHistogramSeconds, 0.0, "old shape: the contrast histograms start empty");
            expectEquals (restored.secondsSinceParamChange, t097ParamFreezeSeconds,
                          "old shape: the missing secondsSinceParamChange reads as 1 s (long ago, not frozen)");

            // 读完旧档马上存：已经是新形状，退让照旧，计时与直方图是空的。
            // 十六进制：double 0 = 16 个 0；double 1.0 = 0x3ff0000000000000。
            {
                const auto upgraded = target.chain.createGlazeStateXml();
                std::unique_ptr<juce::XmlElement> upgradedBody;

                if (upgraded != nullptr)
                    upgradedBody = t097LiveBody (*upgraded);

                const auto* upContrast = upgradedBody != nullptr ? upgradedBody->getChildByName ("CONTRAST_GUARD") : nullptr;
                const auto* upPsr = upgradedBody != nullptr ? upgradedBody->getChildByName ("PSR_GUARD") : nullptr;
                const auto* upControl = upgradedBody != nullptr ? upgradedBody->getChildByName ("CONTROL") : nullptr;
                expect (upContrast != nullptr && upContrast->getStringAttribute ("reliefDb") == "bfa00000"
                            && upContrast->getStringAttribute ("histogramSeconds") == "0000000000000000"
                            && upContrast->getStringAttribute ("overSeconds") == "0000000000000000"
                            && upContrast->hasAttribute ("inputHistogram") && upContrast->hasAttribute ("outputHistogram"),
                        "old shape saved again: CONTRAST_GUARD is upgraded (relief kept, empty timers and histograms)");
                expect (upPsr != nullptr && upPsr->getStringAttribute ("reliefDb") == "bf400000"
                            && upPsr->getStringAttribute ("aboveSeconds") == "0000000000000000",
                        "old shape saved again: PSR_GUARD keeps the relief and writes aboveSeconds = 0");
                expect (upControl != nullptr && upControl->getStringAttribute ("secondsSinceParamChange") == "3ff0000000000000",
                        "old shape saved again: CONTROL writes secondsSinceParamChange = 1 s");
            }

            // 对照：同一份供体存档原样（带直方图）读回、再跑同样长。
            Rig carried (fastConstants(), material, params);
            carried.tempoBpm = 120.0;
            carried.chain.restoreGlazeStateXml (saved.get());

            target.run (afterSeconds);
            carried.run (afterSeconds);
            const auto legacyHistogram = target.probe().contrastHistogramSeconds;
            const auto carriedHistogram = carried.probe().contrastHistogramSeconds;
            logMessage ("  " + juce::String (afterSeconds, 0) + " s after the restore: contrast histogram " + juce::String (legacyHistogram, 2)
                        + " s (old shape) vs " + juce::String (carriedHistogram, 2) + " s (new shape)");
            expect (legacyHistogram <= afterSeconds + 0.1, "old shape: the histograms really started empty (at most the time run since)");
            expect (carriedHistogram > afterSeconds + 0.1, "new shape: the saved histograms are carried over (control for the line above)");
        }

        // 6. 依据：设计规格-总体「音频线程零分配、零锁」；任务书「现有零分配检查覆盖新代码」。两种设置各走一遍：子块推进（落差守门的
        //    直方图与分位、PSR 的 3 s 窗）、预算环退让、宿主旁通冻结、跳播、循环回绕、GLAZE 0 再拧起、宿主 reset、换交付与曲风（守门开关
        //    在开 / 关之间切换）、读档（解析在计数区外）。计数区只夹 setHostInfo / setParams / process / getStatus（同主类与 House 的写法）。
        void testGuardFamilyNeverAllocates()
        {
            beginTest ("guard family: setHostInfo / setParams / process / getStatus never allocate (House club and general, GLAZE 100)");

            {
                // House（Deep House）、俱乐部交付：落差守门开（出厂 house.guards = {true, false}）。破段只有鼓（增益 0.6）、drop 满编：
                // 直方图满 60 s（遗忘后 180 × (1 − e^(−t/180)) = 60 → 约 73 s 有效音乐）之后才判定，95 s 里能走到判定与退 / 回的分支。
                auto params = houseParams (100.0f, 1);
                params.glazeDelivery = 2;
                const auto song = makeHouseSong (defaultRate, { { 8, false, 0.6f }, { 16, true, 1.0f } }, 97);

                Rig donor (fastConstants(), song, params);
                donor.tempoBpm = houseBpm;
                donor.run (16.0);
                const auto saved = donor.chain.createGlazeStateXml();
                expect (saved != nullptr, "fixture: House donor state");

                if (saved != nullptr)
                {
                    Rig rig (fastConstants(), song, params);
                    rig.tempoBpm = houseBpm;
                    const auto result = sweepAllocations (rig, *saved, 1.0f);
                    reportSweep ("House club", rig, result, false);
                }
            }

            {
                // 通用、标准交付：两个新守门出厂都关（照常量测，退让 +0）；紧预算 + 8 s 起推热 ×8（同用例 1a），预算环退让路径也走到。
                const auto constants = withTightLimitBudgets (fastConstants());
                const auto material = makeMusic (defaultRate, 8.0, 25, 0.3f);
                const auto params = defaultParams (100.0f, 2);

                Rig donor (constants, material, params);
                donor.run (6.0);
                const auto saved = donor.chain.createGlazeStateXml();
                expect (saved != nullptr, "fixture: general donor state");

                if (saved != nullptr)
                {
                    Rig rig (constants, material, params);
                    const auto result = sweepAllocations (rig, *saved, 8.0f);
                    reportSweep ("general", rig, result, true);
                }
            }
        }

    private:
        struct SweepResult
        {
            std::uint64_t allocations = 0;
            bool finite = true;
            bool budgetRetreated = false;
            bool contrastJudged = false;
            bool psrValid = false;
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
                if (b == rig.blocksFor (90.0)) rig.params.glazeDelivery = (rig.params.glazeDelivery + 1) % glaze::numDeliveries;
                if (b == rig.blocksFor (91.0)) rig.params.glazeGenre = rig.params.glazeGenre == 0 ? 1 : 0;   // 守门开关开 ↔ 关
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

                const auto p = rig.probe();
                result.budgetRetreated = result.budgetRetreated || p.guardDb < 0.0f;
                result.contrastJudged = result.contrastJudged || p.contrastJudged;
                result.psrValid = result.psrValid || p.outputPsrValid;
            }

            return result;
        }

        void reportSweep (const juce::String& tag, const Rig& rig, const SweepResult& result, bool expectBudgetRetreat)
        {
            const auto p = rig.probe();
            logMessage ("  " + tag + ": sub-blocks stepped " + juce::String ((juce::int64) p.guardSubBlocks) + ", dropped "
                        + juce::String ((juce::int64) p.guardSubBlocksDropped) + "; budget retreated " + juce::String ((int) result.budgetRetreated)
                        + ", contrast judged " + juce::String ((int) result.contrastJudged) + ", output PSR valid " + juce::String ((int) result.psrValid)
                        + "; jumps " + juce::String (p.transportJumps) + ", wraps " + juce::String (p.loopWraps) + ", resets " + juce::String (p.hostResets));
            expect (result.finite, tag + ": output finite throughout");
            expect (p.guardSubBlocks > 0 && p.guardSubBlocksDropped > 0 && result.psrValid && p.transportJumps >= 1 && p.loopWraps >= 1
                        && p.hostResets >= 1,
                    tag + ": the run went through sub-block steps, dropped sub-blocks, PSR readings, a jump, a loop wrap and a host reset");

            if (expectBudgetRetreat)
                expect (result.budgetRetreated, tag + ": fixture: the budget guard retreated on the hot material");

            expectEquals ((juce::int64) result.allocations, (juce::int64) 0, tag + ": audio thread must not allocate");
        }
    };

    static GlazeControllerGuardFamilyTests glazeControllerGuardFamilyTests;
}


//==============================================================================
// ---- T-097 守门闭环验收场景（A3 追加：Max Q9 PSR 闭环、Q10 落差守门）----
// 依据：设计规格-实时GLAZE-主段与增益 第 4 节（下称「规格 4」）；Max 10-10 Q9（PSR 闭环判据）、Q10（落差守门判据）、Q11（有退让量就冻结 G_trim）；
// tools/render/house_gate.py ③（breakdown → drop 落差的量法与俱乐部门槛 1.5 LU）。
//
// 用法：原样追加到 tests/GlazeControllerTests.cpp 末尾（文件最后那个「}」之后）。本段自带 namespace celadon::test，重开同一个翻译单元
// 里的匿名命名空间，直接用上面的 Rig、defaultParams、numChannels、blockSize、defaultRate 与 TestHelpers.h 的 Signal 工具（makeSilence、
// makeDetunedPad、scale）。本段自己的工具都放在嵌套命名空间 t097scenario 里，不与上面的名字相撞。
//
// 两份文件：closed-loop-tests.cpp（新版，追加进 T-097 分支）与 closed-loop-tests-main.cpp（main 3a7a367 版，只用来在 main 上证明改前
// 失败，不提交）。两份除「版本差异块」外逐字相同——场景构造、量法、判据数值都共用；差异块里只有三样：
//   guardsBySpec           判据方向：新版按「改后应通过」判；main 版按「改前失败」反过来判（同一段量法、同一组阈值）；
//   applyScenarioSwitches  T-097 的曲风守门开关（GuardSwitches，main 上没有这个结构）；
//   readGuards             守门读数：新版读 T-097 新探针（逐子块的退让量）；main 只有状态行，近似。
//
// 量法只依赖链的输入素材与输出音频：入口、出口各接一只独立的 LoudnessMeter（与链里 IN / OUT 表同一个类、同一套 K 加权与 100 ms 子块），
// 逐块喂、读子块，按子块算 3 s（30 子块）窗的 S 与 PSR、段落响度——main 上同样能量，判据不读新探针（新探针只进日志）。
//
// 场景 1（Q9）：8 s 一循环的持续型素材（入口 S −20 LUFS、3 s PSR 约 10 dB），通用、流媒体、GLAZE 100、冲击，有效 Ceiling −4 dBTP，
//   PSR 守门用常数强开，三种风格的限幅预算放到 30 / 40 / 60 dB（预算环不退）。判据：从 PSR 守门第一次退让 20 s 之后起，输出 3 s PSR
//   ≥ F − 0.5（F = 7，流媒体下限）的时间占比 ≥ 95 %，且最低不低于 F − 1。main：守门不动作（它读入口，入口 PSR 健康），输出 PSR 在同一段
//   里一直低于 F − 0.5。
// 场景 2（Q10）：124 BPM breakdown / drop 交替 6 段（16 小节一段，约 3 分 6 秒），Tech House、俱乐部、GLAZE 100、冲击。最后一对
//   breakdown → drop 的落差损失（入口落差 − 出口落差，段落响度 = 段内子块 K 加权能量平均，出口按链延迟对齐）两轮都 ≤ 1.5 LU：① 出厂常数；
//   ② 限幅预算放宽（落差守门成为唯一起作用的守门）。main：第②轮损失 > 1.5 LU（G_trim 抵消了落差守门的退让），第①轮只记录。
//   新版另查：第②轮之后换成通用档（落差守门出厂关），退让量每个推进的子块回 0.005 dB（或在非有效音乐时保持），中间不跳变。
// 每 5 s 打一行轨迹（时间、退让量、输出 PSR / 3 s 响度、G_trim、预算环），给 DGX 上调参用。拿不准的数都在用例开头的具名常量里，标【待上机核】。

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace celadon::test
{
    namespace
    {
        namespace t097scenario
        {
            using SubBlock = LoudnessMeter::SubBlock;

            constexpr double absoluteGateLufs = -70.0;   // BS.1770 绝对门：有效子块与 3 s 窗读数的下限（同 ShortTermWindow::absoluteGateLufs）
            constexpr int shortTermSubBlocks = 30;       // 3 s 窗 = 30 个 100 ms 子块（规格 4；ShortTermWindow::subBlocks）
            constexpr double logEverySeconds = 5.0;      // 轨迹日志间隔

            double lufsOf (double meanSquare) noexcept
            {
                // LoudnessMeter::SubBlock 的口径：LUFS = −0.691 + 10·log10(K 加权均方，各声道之和)。
                return meanSquare > 0.0 && std::isfinite (meanSquare) ? -0.691 + 10.0 * std::log10 (meanSquare)
                                                                      : -std::numeric_limits<double>::infinity();
            }

            // 子块长度：与 LoudnessMeter::prepare 同一个式子（48 kHz → 4800）。
            int subBlockLengthFor (double sampleRate) noexcept
            {
                return juce::jmax (1, juce::roundToInt (sampleRate * 0.1));
            }

            juce::String num (double value, int decimals = 2)
            {
                return std::isfinite (value) ? juce::String (value, decimals) : juce::String ("n/a");
            }

            //==============================================================================
            // 一只独立的响度表：只读喂给它的音频，攒下全部完成的 100 ms 子块。每块喂完就读，环（128 个）永远不会追不上。
            struct SubBlockTap
            {
                explicit SubBlockTap (double sampleRate)
                {
                    meter.prepare ({ sampleRate, (juce::uint32) blockSize, (juce::uint32) numChannels });
                }

                void feed (const juce::AudioBuffer<float>& audio)
                {
                    meter.process (audio);

                    for (;;)
                    {
                        const auto count = meter.readSubBlocksSince (cursor, scratch.data(), (int) scratch.size());

                        if (count < 0)
                            dropped = true;

                        const auto got = std::abs (count);

                        for (int i = 0; i < got; ++i)
                            blocks.push_back (scratch[(size_t) i]);

                        if (got < (int) scratch.size())
                            break;
                    }
                }

                LoudnessMeter meter;
                std::int64_t cursor = 0;
                std::array<SubBlock, (size_t) LoudnessMeter::subBlockRingCapacity> scratch {};
                std::vector<SubBlock> blocks;
                bool dropped = false;   // 读者落后过（不该发生：每块都读）
            };

            // 3 s 窗的读数。口径同 T-097 的 ShortTermWindow（ProfileMath.h）与分析器的 psr3sDb，这里独立写一份（main 上没有 ShortTermWindow）：
            // S = 30 个子块均方的平均 → LUFS（不门限、不平滑）；PSR = 20·log10(窗内子块真峰值最大) − S；S < −70 LUFS 或峰值为 0 → 无效。
            struct ShortTerm
            {
                bool valid = false;
                double lufs = 0.0;
                double peakDbtp = 0.0;
                double psrDb = 0.0;
            };

            ShortTerm shortTermEndingAt (const std::vector<SubBlock>& blocks, size_t last)
            {
                ShortTerm reading;
                const auto window = (size_t) shortTermSubBlocks;

                if (last >= blocks.size() || last + 1 < window)
                    return reading;

                double sum = 0.0;
                float peak = 0.0f;

                for (size_t j = last + 1 - window; j <= last; ++j)
                {
                    sum += blocks[j].meanSquare;
                    peak = juce::jmax (peak, blocks[j].truePeakLinear);
                }

                const auto loudness = lufsOf (sum / (double) window);

                if (! (loudness >= absoluteGateLufs) || ! (peak > 0.0f))
                    return reading;

                reading.valid = true;
                reading.lufs = loudness;
                reading.peakDbtp = 20.0 * std::log10 ((double) peak);
                reading.psrDb = reading.peakDbtp - loudness;
                return reading;
            }

            ShortTerm latestShortTerm (const SubBlockTap& tap)
            {
                return tap.blocks.empty() ? ShortTerm {} : shortTermEndingAt (tap.blocks, tap.blocks.size() - 1);
            }

            // 段落响度（house_gate.py section_loudness 的口径：段内有效子块的 K 加权能量平均，按时长加权——子块等长，即算术平均）：
            // 完全落在 [begin, end)（样本）里、且 ≥ −70 LUFS 的子块。子块覆盖 [endSample − length, endSample)。没有有效子块 → NaN。
            double sectionLoudness (const std::vector<SubBlock>& blocks, int length, std::int64_t begin, std::int64_t end)
            {
                double energy = 0.0;
                int count = 0;

                for (const auto& block : blocks)
                {
                    if (block.endSample - length < begin || block.endSample > end || ! (lufsOf (block.meanSquare) >= absoluteGateLufs))
                        continue;

                    energy += block.meanSquare;
                    ++count;
                }

                return count > 0 ? lufsOf (energy / (double) count) : std::numeric_limits<double>::quiet_NaN();
            }

            // 跑一块：先按 Rig::step 同一个式子取出这块的入口样本喂给 in，再跑链，把出口（rig.buffer，链处理后的整块）喂给 out。
            void stepMeasured (Rig& rig, SubBlockTap& in, SubBlockTap& out, juce::AudioBuffer<float>& inputBlock)
            {
                const auto length = rig.source.front().size();
                inputBlock.setSize (numChannels, rig.blockLength, false, false, true);

                for (int ch = 0; ch < numChannels; ++ch)
                    for (int i = 0; i < rig.blockLength; ++i)
                        inputBlock.setSample (ch, i, rig.sourceGain * rig.source[(size_t) ch][(rig.sourceIndex + (size_t) i) % length]);

                in.feed (inputBlock);
                rig.step();
                out.feed (rig.buffer);
            }

            // 整段离线量一遍（另起一只表，块长同 Rig），返回子块序列；repeats 遍首尾相接（循环素材：第二遍起 K 加权状态已是循环稳态）。
            std::vector<SubBlock> subBlocksOf (const Signal& signal, double sampleRate, int repeats)
            {
                SubBlockTap tap (sampleRate);
                const auto length = (std::int64_t) signal.front().size();
                const auto total = length * repeats;
                juce::AudioBuffer<float> block (numChannels, blockSize);

                for (std::int64_t start = 0; start < total; start += blockSize)
                {
                    const auto count = (int) juce::jmin ((std::int64_t) blockSize, total - start);
                    block.setSize (numChannels, count, false, false, true);

                    for (int ch = 0; ch < numChannels; ++ch)
                        for (int i = 0; i < count; ++i)
                            block.setSample (ch, i, signal[(size_t) ch][(size_t) ((start + i) % length)]);

                    tap.feed (block);
                }

                return std::move (tap.blocks);
            }

            // 预算环放宽（T-097 Q16）：预算环核对的七项上限（Glue 均值 / P95、Clip 深度 / 活动、限幅均值 / P95 / 峰值）在三种风格里各 × 4，
            // Clip 活动占比封到 1。仍是约束（> 0，决策器照样按它规划），但实际 GR 到不了，预算环不退。
            // 只放宽限幅三项时 Glue / Clip 预算照样触发预算环；七项全置 0 = 不设约束，方案整个变轻、没有落差可保（DGX 14:4x 实测）。
            // 出厂为 0（不约束）的项 × 4 仍是 0。
            glaze::Constants withRelaxedLimitBudgets (glaze::Constants c) noexcept
            {
                constexpr float relax = 4.0f;

                for (auto* style : { &c.warm, &c.punchy, &c.clean })
                {
                    style->glueGrMeanCapDb *= relax;
                    style->glueGrP95CapDb *= relax;
                    style->clipDepthCapDb *= relax;
                    style->clipActivityCap = juce::jmin (1.0f, style->clipActivityCap * relax);
                    style->limitGrMeanCapDb *= relax;
                    style->limitGrP95CapDb *= relax;
                    style->limitGrPeakCapDb *= relax;
                }

                return c;
            }

            //==============================================================================
            // 场景 1 的素材：8 s 一循环（120 BPM 4 小节），持续型——和声垫一直在，底鼓 / 贝斯 / 踩镲每拍一样，任何 3 s 窗的 S 与 PSR 都几乎相同。
            // 元件同 makeMusic（去掉军鼓），底鼓乘 kickGain、和声垫乘 padGain；循环点无缝：pad 频率都取 1/8 Hz 的整数倍（8 s 内整周期），
            // 底鼓 / 贝斯 / 踩镲按拍内位置（16 拍整），贝斯在拍尾 10 ms 收到 0，噪声就是这同一段 8 s 循环。响度另按实测归一（见场景 1）。
            Signal makeSteadyLoop (double sampleRate, double loopSeconds, float kickGain, float padGain, juce::int64 seed)
            {
                constexpr double beatSeconds = 0.5;
                constexpr double bassNotesHz[] = { 55.0, 73.5, 82.5, 61.75 };
                constexpr double padNotesHz[]  = { 220.0, 277.125, 329.625, 440.0 };

                const auto numSamples = (int) std::lround (loopSeconds * sampleRate);
                auto signal = makeSilence (numChannels, numSamples);
                const auto twoPi = juce::MathConstants<double>::twoPi;
                const auto hatCoeff = std::exp (-twoPi * 6000.0 / sampleRate);

                for (int ch = 0; ch < numChannels; ++ch)
                {
                    juce::Random random (seed + (juce::int64) ch * 7919);
                    auto& channel = signal[(size_t) ch];
                    double hat = 0.0;

                    for (int i = 0; i < numSamples; ++i)
                    {
                        const auto t = (double) i / sampleRate;
                        const auto beat = (int) (t / beatSeconds);
                        const auto beatPos = t - beat * beatSeconds;
                        const auto white = (double) random.nextFloat() * 2.0 - 1.0;
                        hat = hatCoeff * hat + (1.0 - hatCoeff) * white;

                        const auto kick = 0.55 * std::sin (twoPi * 55.0 * beatPos) * std::exp (-beatPos / 0.09)
                                        + 0.15 * white * std::exp (-beatPos / 0.003);
                        const auto bassHz = bassNotesHz[(size_t) (beat % 4)];
                        const auto bassEnvelope = juce::jmin (1.0, beatPos / 0.01) * juce::jmin (1.0, (beatSeconds - beatPos) / 0.01)
                                                * std::exp (-beatPos / 0.6);
                        const auto bass = bassEnvelope * (0.28 * std::sin (twoPi * bassHz * beatPos) + 0.10 * std::sin (twoPi * 2.0 * bassHz * beatPos));

                        double pad = 0.0;

                        for (auto hz : padNotesHz)
                            pad += 0.05 * std::sin (twoPi * hz * t + 0.7 * ch);

                        const auto hatHit = 0.08 * (white - hat) * std::exp (-std::fmod (t, beatSeconds * 0.5) / 0.03);
                        channel[(size_t) i] = (float) ((double) kickGain * kick + bass + (double) padGain * pad + hatHit);
                    }
                }

                return signal;
            }

            //==============================================================================
            // 场景 2 的曲子：124 BPM、4/4，16 小节一段 × 6 段（偶数段 breakdown、奇数段 drop：breakdown, drop, breakdown, drop, breakdown, drop），
            // 约 3 分 6 秒。元件照 tools/render/make_synthetic_house.py（T-045 实测「合成 House 曲」的配方）：drop = 扫频底鼓（158 → 48 Hz、
            // 110 ms 衰减）+ offbeat bass（八分音符长、Am–F–C–G 根音、基音 + 两个泛音）+ 2、4 拍的拍手（一阶差分噪声、30 ms 衰减）+ offbeat
            // 踩镲（7 kHz 一阶高通噪声、12 ms 衰减）+ 失谐 pad（makeDetunedPad，同脚本 pad_bar）；breakdown = pad + 轻踩镲。两类段落分别按实测
            // 归一（normaliseContrastSong）：drop 的段落响度 = dropLufs，breakdown 再轻 breakdownDepthLu。
            // 离线估算（Python 照同一公式：K 加权同 LoudnessMeter、8× 真峰值，噪声换成 numpy 的均匀白噪声）：单位增益下 drop 段落响度
            // −10.2 LUFS、3 s PSR 中位 10.6 dB（10.1 ～ 12.0），breakdown −18.6 LUFS、PSR 7.9 dB（本来就轻 8.4 LU）。【待上机核】
            struct ContrastSongDesign
            {
                double bpm = 124.0;
                int barsPerSection = 16;
                int sections = 12;            // T-097 Q16：6 段（3 分钟）时守门约 76 s 才判、歌就放完了；12 段约 6 分钟
                double kick = 1.0;             // × 0.8 的扫频底鼓（脚本 kick()）
                double bass = 0.7;             // offbeat bass（脚本 bass_note gain 0.7）
                double clap = 0.10;            // 拍手（均匀噪声的一阶差分，峰值 ≤ 2 × 本项）【待上机核：drop 的 PSR 主要靠它与底鼓】
                double dropHat = 0.06;         // offbeat 踩镲（脚本 hats 0.06）
                double breakdownHat = 0.02;    // 「轻踩镲」= drop 的 1/3
                double pad = 0.06;             // makeDetunedPad 的幅度（脚本 pad_bar gain 0.06）
                double dropLufs = -14.0;       // drop 的段落响度（按实测归一）【待上机核】：只决定要推多少，不进判据
                double breakdownDepthLu = 9.0; // breakdown 比 drop 轻（任务书：8 ～ 10 LU）

                double beatSeconds() const noexcept      { return 60.0 / bpm; }
                double barSeconds() const noexcept       { return 4.0 * beatSeconds(); }
                double sectionSeconds() const noexcept   { return barSeconds() * barsPerSection; }
                double songSeconds() const noexcept      { return sectionSeconds() * sections; }
                static bool isDrop (int section) noexcept { return section % 2 == 1; }

                // 第几段（生成与归一、量法都用这一个式子，段落边界逐样本一致）。
                int sectionAt (double seconds) const noexcept
                {
                    const auto bar = (int) (seconds / barSeconds());
                    return juce::jmin (sections - 1, bar / barsPerSection);
                }

                // 第 k 段的第一个样本（k = sections → 整首的样本数）：先按 floor 估，再前后挪到 sectionAt 的边界上。
                std::int64_t sectionStart (int k, double sampleRate, std::int64_t songSamples) const noexcept
                {
                    if (k <= 0)
                        return 0;

                    if (k >= sections)
                        return songSamples;

                    auto start = (std::int64_t) std::floor ((double) k * sectionSeconds() * sampleRate);

                    while (start < songSamples && sectionAt ((double) start / sampleRate) < k)
                        ++start;

                    while (start > 0 && sectionAt ((double) (start - 1) / sampleRate) >= k)
                        --start;

                    return start;
                }
            };

            Signal makeContrastSong (double sampleRate, const ContrastSongDesign& d, juce::int64 seed)
            {
                constexpr double bassRootsHz[] = { 55.0, 43.654, 65.406, 48.999 };   // 同脚本 BASS_ROOTS（Am – F – C – G）
                const auto beat = d.beatSeconds();
                const auto bar = d.barSeconds();
                const auto noteSeconds = 0.45 * beat;
                const auto twoPi = juce::MathConstants<double>::twoPi;
                const auto hatCoeff = std::exp (-twoPi * 7000.0 / sampleRate);

                // pad 打底（和弦每小节换一个，换和弦处 60 ms 交叉淡化）；鼓、贝斯按段加在上面。样本数 = (int) (整首秒数 × 采样率)。
                auto signal = makeDetunedPad (sampleRate, d.songSeconds(), bar, d.pad);
                const auto numSamples = (int) signal.front().size();

                for (int ch = 0; ch < numChannels; ++ch)
                {
                    juce::Random random (seed + (juce::int64) ch * 7919);
                    auto& channel = signal[(size_t) ch];
                    double hatLowpass = 0.0, previousWhite = 0.0;

                    for (int i = 0; i < numSamples; ++i)
                    {
                        const auto t = (double) i / sampleRate;
                        const auto beatIndex = (int) (t / beat);
                        const auto beatPos = t - beatIndex * beat;
                        const auto offPos = std::fmod (t + 0.5 * beat, beat);   // 从 offbeat 起算的拍内位置
                        const auto barIndex = (int) (t / bar);
                        const auto white = (double) random.nextFloat() * 2.0 - 1.0;
                        hatLowpass = hatCoeff * hatLowpass + (1.0 - hatCoeff) * white;
                        const auto difference = white - previousWhite;
                        previousWhite = white;
                        const auto hat = (white - hatLowpass) * std::exp (-offPos / 0.012);
                        double value = 0.0;

                        if (ContrastSongDesign::isDrop (d.sectionAt (t)))
                        {
                            const auto kick = 0.8 * std::sin (twoPi * (48.0 * beatPos + 110.0 * 0.035 * (1.0 - std::exp (-beatPos / 0.035))))
                                            * std::exp (-beatPos / 0.11) * juce::jmin (1.0, beatPos / 0.002);
                            const auto clap = beatIndex % 2 == 1 ? difference * std::exp (-beatPos / 0.03) * juce::jmin (1.0, beatPos / 0.0005) : 0.0;
                            double bass = 0.0;

                            if (offPos < noteSeconds)
                            {
                                const auto hz = bassRootsHz[(size_t) (barIndex % 4)];
                                const auto envelope = juce::jmin (1.0, offPos / 0.004) * std::exp (-offPos / (1.2 * noteSeconds))
                                                    * juce::jmin (1.0, (noteSeconds - offPos) / 0.005);
                                bass = envelope * (std::sin (twoPi * hz * offPos) + 0.25 * std::sin (2.0 * twoPi * hz * offPos)
                                                   + 0.1 * std::sin (3.0 * twoPi * hz * offPos));
                            }

                            value = d.kick * kick + d.bass * bass + d.clap * clap + d.dropHat * hat;
                        }
                        else
                        {
                            value = d.breakdownHat * hat;
                        }

                        channel[(size_t) i] += (float) value;
                    }
                }

                return signal;
            }

            struct SongLevels
            {
                double dropUnitLufs = 0.0, breakdownUnitLufs = 0.0;   // 单位增益下两类段落的段落响度
            };

            // 两类段落分别归一：离线量单位增益的曲子，按类取完全落在段内的有效子块的能量平均，再按段乘增益（段落边界同 sectionAt）。
            SongLevels normaliseContrastSong (Signal& song, const ContrastSongDesign& d, double sampleRate)
            {
                const auto songSamples = (std::int64_t) song.front().size();
                const auto blocks = subBlocksOf (song, sampleRate, 1);
                const auto length = subBlockLengthFor (sampleRate);
                double energy[2] = { 0.0, 0.0 };   // [0] breakdown、[1] drop
                int count[2] = { 0, 0 };

                for (int k = 0; k < d.sections; ++k)
                {
                    const auto begin = d.sectionStart (k, sampleRate, songSamples);
                    const auto end = d.sectionStart (k + 1, sampleRate, songSamples);
                    const auto kind = ContrastSongDesign::isDrop (k) ? 1 : 0;

                    for (const auto& block : blocks)
                    {
                        if (block.endSample - length < begin || block.endSample > end || ! (lufsOf (block.meanSquare) >= absoluteGateLufs))
                            continue;

                        energy[kind] += block.meanSquare;
                        ++count[kind];
                    }
                }

                SongLevels levels;
                levels.breakdownUnitLufs = lufsOf (energy[0] / juce::jmax (1, count[0]));
                levels.dropUnitLufs = lufsOf (energy[1] / juce::jmax (1, count[1]));
                const auto dropGain = (float) juce::Decibels::decibelsToGain (d.dropLufs - levels.dropUnitLufs);
                const auto breakdownGain = (float) juce::Decibels::decibelsToGain (d.dropLufs - d.breakdownDepthLu - levels.breakdownUnitLufs);

                for (auto& channel : song)
                    for (size_t i = 0; i < channel.size(); ++i)
                        channel[i] *= ContrastSongDesign::isDrop (d.sectionAt ((double) i / sampleRate)) ? dropGain : breakdownGain;

                return levels;
            }

            //==============================================================================
            // 守门读数（readGuards 填；main 上只有 exact = false 的近似）。
            struct GuardReadout
            {
                bool exact = false;                  // true：T-097 探针的逐子块退让量；false：状态行近似（只看得到正在退的那一扇）
                float psrReliefDb = 0.0f;
                float contrastReliefDb = 0.0f;
                std::int64_t guardSubBlocks = -1;    // 推进守门的子块数（新版探针）；main 没有 → −1
                bool trimFrozen = false;             // 以下只进日志
                bool guardPsrValid = false;
                float guardPsrDb = 0.0f;             // PSR 守门自己读到的出口 3 s PSR（与本段的量法对照）
                bool contrastJudged = false;
                float contrastLossLu = 0.0f;         // 落差守门的直方图损失读数
            };

            // 换通用档之后的一块：这一块推进守门的子块数与前后的落差退让量（判据在用例里按常数核对）。
            struct ReleaseSample
            {
                std::int64_t stepped = 0;
                float beforeDb = 0.0f;
                float afterDb = 0.0f;
            };

            struct ContrastRound
            {
                std::vector<double> inputLufs, outputLufs;   // 各段段落响度（出口按链延迟对齐）
                double inputDepthLu = std::numeric_limits<double>::quiet_NaN();    // 最后一对：drop − breakdown（入口）
                double outputDepthLu = std::numeric_limits<double>::quiet_NaN();   //           drop − breakdown（出口）
                double lossLu = std::numeric_limits<double>::quiet_NaN();          //           入口落差 − 出口落差
                int latencySamples = 0;
                bool budgetActed = false, psrActed = false, dropped = false;
                float deepestContrastReliefDb = 0.0f, endContrastReliefDb = 0.0f;
                bool releaseObserved = false;                // 换通用档之后的回程读得到（新版探针）
                float reliefAtSwitchDb = 0.0f;
                std::vector<ReleaseSample> release;
            };

            //==============================================================================
            // ==== 版本差异块（开始）：新版（T-097 分支）。main 3a7a367 版只换这一块，其余逐字相同 ====

            // 判据方向：新版按规格「改后应通过」判。
            constexpr bool guardsBySpec = true;

            // 场景 1：通用档的 PSR 守门出厂关（generalGuards {false, false}，规格 4「House / 通用关，留给以后的曲风」），用常数强开。
            // 场景 2：House 三类的出厂开关（house.guards {true, false}：落差开、PSR 关），不动。
            void applyScenarioSwitches (glaze::Constants& constants, bool psrScenario)
            {
                if (psrScenario)
                    constants.generalGuards.psr = true;
            }

            // T-097 新探针：两个守门的退让量、推进的子块数（GlazeController.h TestProbe 末尾）。
            GuardReadout readGuards (const Rig& rig)
            {
                const auto p = rig.probe();
                GuardReadout r;
                r.exact = true;
                r.psrReliefDb = p.psrReliefDb;
                r.contrastReliefDb = p.contrastReliefDb;
                r.guardSubBlocks = p.guardSubBlocks;
                r.trimFrozen = p.trimFrozen;
                r.guardPsrValid = p.outputPsrValid;
                r.guardPsrDb = p.outputPsrDb;
                r.contrastJudged = p.contrastJudged;
                r.contrastLossLu = p.contrastLossLu;
                return r;
            }

            // ==== 版本差异块（结束）====
        }
    }

    class GlazeControllerGuardScenarioTests  : public juce::UnitTest
    {
    public:
        GlazeControllerGuardScenarioTests() : juce::UnitTest ("GLAZE guard closed-loop scenarios (T-097)", "Celadon") {}

        void runTest() override
        {
            testPsrClosedLoop();
            testBreakdownDropContrast();
        }

    private:
        //==============================================================================
        // 场景 1（Max Q9）：PSR 闭环。
        void testPsrClosedLoop()
        {
            using namespace t097scenario;
            beginTest (juce::String ("PSR closed loop (Q9): quiet input, healthy input PSR, streaming push; output 3 s PSR held at the floor")
                       + (guardsBySpec ? "" : " [main: expected to fail]"));

            // ---- 素材 ----
            constexpr double loopSeconds = 8.0;
            constexpr juce::int64 loopSeed = 20261011;
            // 配比：离线估算（Python 照 makeSteadyLoop 同一公式，K 加权同 LoudnessMeter、8× 真峰值）单位增益下整段 S −12.2 LUFS、
            // 3 s PSR 10.2 ～ 10.5 dB（中位 10.3）；底鼓 0.5 时峰值由底鼓 + pad 叠加定。【待上机核：入口 PSR 不在 9 ～ 11.5 就调这两个】
            constexpr float kickGain = 0.5f, padGain = 2.0f;
            constexpr double inputLufs = -20.0;                       // 入口 3 s S：按实测归一（「偏轻」）
            constexpr double inputLufsToleranceLu = 0.5;              // 前提：各 3 s 窗的 S 都在 −20 ± 0.5（持续型素材，离线估算起伏 ±0.03）
            constexpr double inputPsrLow = 9.0, inputPsrHigh = 11.5;  // 前提：入口 PSR 约 10 dB，「健康」（远高于流媒体门槛 6.5）【待上机核】

            // ---- 设置 ----
            // 有效 Ceiling = 用户 Ceiling − 编码余量 = −3 − 1 = −4 dBTP（参数下限 −3；比流媒体封顶 −2 低，封顶不起作用）。
            // 为什么要压低：流媒体目标 −10 LUFS（冲击，风格偏移 0；持续型素材 LRA ≈ 0、不修正），出厂 −2 dBTP 封顶下限幅器从 Ceiling − 0.35 dB
            // 起限（LimiterModule.h 真峰值重建余量），输出 PSR 最多 −2.35 − (−10) = 7.65 dB，到不了门槛 6.5——守门无事可做。压到 −4 后：
            // 不受守门时输出 PSR ≈ −4.35 − (−10) = 5.65 dB（比门槛低 0.85 dB），这才是「推响把 PSR 压塌」的场景。
            constexpr float userCeilingDb = -3.0f;
            constexpr float codecMarginDb = 1.0f;

            // ---- 判据（规格 4、Max Q9）----
            constexpr double floorDb = 7.0;                  // F：规格 floorFor(流媒体) = 7 dB
            constexpr double tripDb = floorDb - 0.5;         // 6.5：守门门槛，也是判据线
            constexpr double worstDb = floorDb - 1.0;        // 6.0：判据窗内最低
            constexpr double minFractionAbove = 0.95;
            constexpr double evalDelaySeconds = 20.0;        // 从第一次退让 20 s 之后起算
            // 预计轨迹（新版）：通用档响度型 M0 = 20 s、满置信 0.8 → 方案满强度要 −20·ln(1 − 0.8) ≈ 32 s 有效音频；PSR 在增益离满强度约 0.85 dB
            // 时跌破 6.5（≈ 25 s），再持续 3 s → 第一次退让 ≈ 25 ～ 30 s【待上机核】；此后 0.5 dB/s 退到 PSR ≥ 6.5，3 s 窗滞后让读数冲到约 7.2，
            // 满 5 s ≥ 7.0 后 0.15 dB/s 回到 7.0 以下停住——稳态 PSR 在 6.5 ～ 7.2，退让约 −1.1 dB（加上冻结前 G_trim 多推的 ≤ 1 dB，离下限 −3 还远）。
            // 判据窗 ≈ [45 ～ 50 s, 150 s]，≥ 100 s：95 % 允许约 5 s 低于门槛（多于一次 3 s 持续判定）。
            constexpr double runSeconds = 150.0;
            constexpr double minEvalSeconds = 60.0;          // 判据窗短于这个不算数（第一次退让来得太晚）
            // main：守门读入口 PSR（≈ 10.3，健康）始终不退；G_trim 把 S 推到目标 −10 → 输出 PSR ≈ 5.65 dB。没有「第一次退让」，判据窗从这里起：
            // 方案满强度 ≈ 32 s，G_trim（0.05 dB/s）补限幅掉的响度要再十几到二十几秒。【待上机核】
            constexpr double evalFallbackFromSeconds = 60.0;

            auto constants = withRelaxedLimitBudgets (glaze::factoryConstants());
            applyScenarioSwitches (constants, true);

            auto loop = makeSteadyLoop (defaultRate, loopSeconds, kickGain, padGain, loopSeed);
            const auto loopSubBlocks = (size_t) std::lround (loopSeconds / 0.1);
            const auto unitBlocks = subBlocksOf (loop, defaultRate, 2);

            expect (unitBlocks.size() >= loopSubBlocks, "the offline loudness pass covers a whole loop");

            if (unitBlocks.size() < loopSubBlocks)
                return;

            double unitEnergy = 0.0;

            for (size_t j = unitBlocks.size() - loopSubBlocks; j < unitBlocks.size(); ++j)   // 第二遍：K 加权状态已是循环稳态
                unitEnergy += unitBlocks[j].meanSquare;

            const auto unitLufs = lufsOf (unitEnergy / (double) loopSubBlocks);
            scale (loop, (float) juce::Decibels::decibelsToGain (inputLufs - unitLufs));
            logMessage ("  loop at unit gain " + num (unitLufs) + " LUFS -> scaled by " + num (inputLufs - unitLufs) + " dB");

            auto params = defaultParams (100.0f, 2);   // GLAZE 100、冲击（风格偏移 0 → 流媒体目标 −10 LUFS）
            params.glazeGenre = 0;                     // 通用
            params.glazeDelivery = 1;                  // 流媒体
            params.limit.ceilingDb = userCeilingDb;
            params.mastering.codecMarginDb = codecMarginDb;
            Rig rig (constants, loop, params);

            SubBlockTap inputTap (rig.sampleRate), outputTap (rig.sampleRate);
            juce::AudioBuffer<float> inputBlock (numChannels, rig.blockLength);
            const auto noValue = std::numeric_limits<double>::quiet_NaN();
            auto firstRetreatSeconds = -1.0;
            auto psrActed = false, budgetActed = false;
            auto deepestReliefDb = 0.0f;
            auto nextLog = logEverySeconds;
            const auto blocks = rig.blocksFor (runSeconds);

            for (int b = 0; b < blocks; ++b)
            {
                stepMeasured (rig, inputTap, outputTap, inputBlock);
                const auto now = (double) (b + 1) * (double) rig.blockLength / rig.sampleRate;
                const auto guards = readGuards (rig);
                const auto probe = rig.probe();
                budgetActed = budgetActed || probe.guardDb < 0.0f;
                deepestReliefDb = juce::jmin (deepestReliefDb, guards.psrReliefDb);

                if (guards.psrReliefDb < 0.0f)
                {
                    psrActed = true;

                    if (firstRetreatSeconds < 0.0)
                        firstRetreatSeconds = now;
                }

                if (now >= nextLog)
                {
                    nextLog += logEverySeconds;
                    const auto out = latestShortTerm (outputTap);
                    logMessage ("  t " + num (now, 1) + " s | PSR relief " + num (guards.psrReliefDb) + " dB"
                                + " | out S " + num (out.valid ? out.lufs : noValue) + " LUFS, PSR " + num (out.valid ? out.psrDb : noValue) + " dB"
                                + (guards.exact ? " (guard reads " + (guards.guardPsrValid ? num (guards.guardPsrDb) : juce::String ("n/a"))
                                                      + ", trim " + (guards.trimFrozen ? "frozen" : "free") + ")"
                                                : juce::String())
                                + " | G_trim " + num (probe.appliedBiasDb) + " dB | budget " + num (probe.guardDb) + " dB"
                                + " | target " + num (rig.status().effectiveTargetLufs) + " LUFS | limit gain "
                                + num (rig.chain.getEffectiveParams().limit.gainDb) + " dB");
                }
            }

            // ---- 入口（前提）：跳过前 3 s，之后每个 3 s 窗 ----
            auto inputPsrMin = std::numeric_limits<double>::infinity(), inputPsrMax = -std::numeric_limits<double>::infinity();
            auto inputSMin = inputPsrMin, inputSMax = inputPsrMax;

            for (size_t j = (size_t) shortTermSubBlocks - 1; j < inputTap.blocks.size(); ++j)
            {
                const auto reading = shortTermEndingAt (inputTap.blocks, j);

                if (! reading.valid)
                    continue;

                inputPsrMin = juce::jmin (inputPsrMin, reading.psrDb);
                inputPsrMax = juce::jmax (inputPsrMax, reading.psrDb);
                inputSMin = juce::jmin (inputSMin, reading.lufs);
                inputSMax = juce::jmax (inputSMax, reading.lufs);
            }

            // ---- 出口：判据窗里每个子块一个 3 s 读数 ----
            const auto evalFromSeconds = firstRetreatSeconds >= 0.0 ? firstRetreatSeconds + evalDelaySeconds : evalFallbackFromSeconds;
            auto evalCount = 0, aboveCount = 0, invalidCount = 0;
            auto minPsr = std::numeric_limits<double>::infinity(), maxPsr = -std::numeric_limits<double>::infinity(), sumPsr = 0.0;

            for (size_t j = (size_t) shortTermSubBlocks - 1; j < outputTap.blocks.size(); ++j)
            {
                if ((double) outputTap.blocks[j].endSample / rig.sampleRate < evalFromSeconds)
                    continue;

                const auto reading = shortTermEndingAt (outputTap.blocks, j);

                if (! reading.valid)
                {
                    ++invalidCount;
                    continue;
                }

                ++evalCount;
                aboveCount += reading.psrDb >= tripDb ? 1 : 0;
                minPsr = juce::jmin (minPsr, reading.psrDb);
                maxPsr = juce::jmax (maxPsr, reading.psrDb);
                sumPsr += reading.psrDb;
            }

            const auto fractionAbove = evalCount > 0 ? (double) aboveCount / (double) evalCount : 0.0;
            logMessage ("  input: S " + num (inputSMin) + " .. " + num (inputSMax) + " LUFS, 3 s PSR " + num (inputPsrMin) + " .. " + num (inputPsrMax)
                        + " dB | effective ceiling " + num (rig.chain.getEffectiveParams().limit.ceilingDb) + " dBTP");
            logMessage ("  PSR guard " + juce::String (psrActed ? "acted" : "never acted") + ", first retreat "
                        + (firstRetreatSeconds >= 0.0 ? num (firstRetreatSeconds, 1) + " s" : juce::String ("never"))
                        + ", deepest relief " + num (deepestReliefDb) + " dB; budget ring " + (budgetActed ? "retreated" : "never retreated")
                        + " | window from " + num (evalFromSeconds, 1) + " s: "
                        + juce::String (evalCount) + " readings, " + num (100.0 * fractionAbove, 1) + " % >= " + num (tripDb, 1)
                        + " dB, min " + num (minPsr) + ", mean " + num (evalCount > 0 ? sumPsr / evalCount : noValue) + ", max " + num (maxPsr) + " dB");

            // ---- 前提（两版同）----
            expect (! inputTap.dropped && ! outputTap.dropped, "both taps read every sub-block");
            expectWithinAbsoluteError (rig.chain.getEffectiveParams().limit.ceilingDb, userCeilingDb - codecMarginDb, 1.0e-4f,
                                       "premise: effective ceiling = user ceiling - codec margin = -4 dBTP (the streaming cap -2 does not bind)");
            expect (inputSMin >= inputLufs - inputLufsToleranceLu && inputSMax <= inputLufs + inputLufsToleranceLu,
                    "premise: input 3 s loudness stays at -20 LUFS: " + num (inputSMin) + " .. " + num (inputSMax));
            expect (inputPsrMin >= inputPsrLow && inputPsrMax <= inputPsrHigh,
                    "premise: input 3 s PSR is healthy, about 10 dB: " + num (inputPsrMin) + " .. " + num (inputPsrMax));
            expect (! budgetActed, "premise: the budget ring never retreats (all seven budget caps relaxed x 4)");
            expectEquals (invalidCount, 0, "every output 3 s reading in the window is valid");
            expect (evalCount >= (int) (minEvalSeconds * 10.0), "the judged window is long enough: " + juce::String (evalCount) + " readings");

            if constexpr (guardsBySpec)
            {
                expect (firstRetreatSeconds >= 0.0, "the PSR guard (reading the output) retreats");
                expect (fractionAbove >= minFractionAbove, "from first retreat + 20 s, output 3 s PSR >= F - 0.5 = 6.5 dB at least 95 % of the time: "
                                                           + num (100.0 * fractionAbove, 1) + " %");
                expect (minPsr >= worstDb, "... and never below F - 1 = 6.0 dB: min " + num (minPsr));
            }
            else
            {
                expect (! psrActed, "main: the PSR guard reads the healthy input PSR and never acts");
                expect (maxPsr < tripDb, "main: output 3 s PSR stays below F - 0.5 = 6.5 dB over the same window: max " + num (maxPsr));
            }
        }

        //==============================================================================
        // 场景 2（Max Q10）：落差守门。
        void testBreakdownDropContrast()
        {
            using namespace t097scenario;
            beginTest (juce::String ("Contrast guard (Q10): House club GLAZE 100 keeps the last breakdown -> drop loss within 1.5 LU")
                       + (guardsBySpec ? "" : " [main: expected to fail]"));

            // ---- 素材（配比与推导见 ContrastSongDesign）----
            const ContrastSongDesign design {};
            constexpr juce::int64 songSeed = 20261011;
            constexpr double depthToleranceLu = 0.5;   // 前提：最后一对的入口落差 = 9 ± 0.5（两类段落按实测归一，应几乎正好 9）

            // ---- 判据 ----
            constexpr double clubLossLimitLu = 1.5;    // house_gate ③ 俱乐部档（Max 2026-10-01）；同 ContrastGuard 的 clubLossLu
            constexpr double compareEpsilon = 1.0e-9;  // 同 house_gate 的比较余量
            // 预计（【待上机核】）：
            //   目标：俱乐部 −7 LUFS（冲击偏移 0）再减 LRA 修正（画像 LRA 若按 breakdown / drop 约 9 LU 算，(9 − 6) ÷ 3 = 1 → −8）；轨迹日志打实际目标。
            //   ① 出厂：限幅预算（冲击 1.8 / 4 / 6 dB）让决策器封顶，T-045 实测同配方的合成 House 曲俱乐部档少 1.34 ～ 1.39 LU（阶段 1 编排器）→ ≤ 1.5。
            //   ② 放宽：不封顶推到目标，T-045 记录「预算再放宽一档」已少 2.2 LU → 不受守门约 2.2 LU 以上（main 第②轮应 > 1.5）。新版：两份直方图
            //      攒满 60 s（按 180 s 遗忘，要 180·ln(1.5) ≈ 73 s 有效音乐，加 3 s 窗）≈ 76 s 开始判定，超门槛满 10 s → 约 86 s 起以 0.1 dB/s 退，
            //      约 116 s 退到下限 −3 dB，早于最后一个 breakdown（124 s）；G_trim 冻结，最后一对按 −3 dB 演奏：限幅少压，损失约降 0.4 ～ 0.5 LU/dB
            //      × 3 dB → 约 1.0 LU。第一个 breakdown 开头被当主段推响，会让输出直方图低端变宽、损失读数偏小，退让可能晚一些。
            //   换通用：落差守门出厂关 → 每个推进的子块回 0.05 dB/s × 0.1 s = 0.005 dB；20 s = 200 个子块 → 从约 −3 回到约 −2 dB，不会碰到 0。
            constexpr double switchSeconds = 20.0;
            constexpr float releaseStepDb = 0.005f;            // 规格 4：0.05 dB/s × 0.1 s
            constexpr float releaseToleranceDb = 1.0e-5f;      // float 累加：|退让| ≤ 3 dB 时一步的舍入 ≤ 2.4e-7 dB
            constexpr double minReleaseFraction = 0.9;         // 推进的子块里至少九成真的回了一步（其余只许是非有效音乐时的保持）
            constexpr float minReliefToObserveDb = -0.5f;      // 换档时退让至少这么深，回程才看得清

            auto song = makeContrastSong (defaultRate, design, songSeed);
            const auto levels = normaliseContrastSong (song, design, defaultRate);
            logMessage ("  song " + num (design.songSeconds(), 1) + " s; unit-gain drop " + num (levels.dropUnitLufs) + " LUFS, breakdown "
                        + num (levels.breakdownUnitLufs) + " LUFS -> drop " + num (design.dropLufs, 1) + ", breakdown "
                        + num (design.dropLufs - design.breakdownDepthLu, 1) + " LUFS");

            auto factory = glaze::factoryConstants();
            applyScenarioSwitches (factory, false);
            auto relaxed = withRelaxedLimitBudgets (glaze::factoryConstants());
            applyScenarioSwitches (relaxed, false);

            const auto factoryRound = runContrastRound (factory, song, design, "1 factory", false, 0.0);
            const auto relaxedRound = runContrastRound (relaxed, song, design, "2 relaxed", guardsBySpec, switchSeconds);

            // ---- 前提（两版同）----
            expect (std::abs (relaxedRound.inputDepthLu - design.breakdownDepthLu) <= depthToleranceLu,
                    "premise: the last breakdown is " + num (design.breakdownDepthLu, 1) + " LU below the drop at the input: "
                    + num (relaxedRound.inputDepthLu));
            expect (! factoryRound.dropped && ! relaxedRound.dropped, "all taps read every sub-block");
            expect (! relaxedRound.budgetActed, "premise: round 2 budget ring never retreats (limit budgets relaxed)");
            expect (! relaxedRound.psrActed, "premise: round 2 PSR guard never acts");

            if constexpr (guardsBySpec)
            {
                expect (factoryRound.lossLu <= clubLossLimitLu + compareEpsilon,
                        "round 1 (factory): last breakdown -> drop loses <= 1.5 LU: " + num (factoryRound.lossLu));
                expect (relaxedRound.deepestContrastReliefDb < 0.0f,
                        "round 2: the contrast guard actually retreats (otherwise the round proves nothing - retune the song)");
                expect (relaxedRound.lossLu <= clubLossLimitLu + compareEpsilon,
                        "round 2 (relaxed budgets, contrast guard alone): loses <= 1.5 LU: " + num (relaxedRound.lossLu));

                // ---- 换通用档之后的回程 ----
                int releaseSteps = 0, heldSteps = 0, badSteps = 0, steppedWhileRetreated = 0;
                auto largestMoveDb = 0.0f;

                for (const auto& s : relaxedRound.release)
                {
                    const auto delta = s.afterDb - s.beforeDb;
                    largestMoveDb = juce::jmax (largestMoveDb, std::abs (delta));

                    if (s.stepped <= 0)
                    {
                        if (delta != 0.0f)   // 这一块没推进守门：退让量不该动
                            ++badSteps;
                    }
                    else if (s.beforeDb < -(float) s.stepped * releaseStepDb)
                    {
                        // 离 0 还远：每个推进的子块要么回一步（有效音乐），要么保持（非有效音乐），别的都不对。
                        const auto steps = (int) std::lround (delta / releaseStepDb);
                        steppedWhileRetreated += (int) s.stepped;

                        if (steps >= 0 && steps <= (int) s.stepped && std::abs (delta - (float) steps * releaseStepDb) <= (float) s.stepped * releaseToleranceDb)
                        {
                            releaseSteps += steps;
                            heldSteps += (int) s.stepped - steps;
                        }
                        else
                        {
                            ++badSteps;
                        }
                    }
                    else if (! (delta >= 0.0f && delta <= (float) s.stepped * releaseStepDb + releaseToleranceDb && s.afterDb <= 0.0f))
                    {
                        ++badSteps;   // 最后一步收到 0：只许往 0 走、不越过 0
                    }
                }

                const auto reliefAtEndDb = relaxedRound.release.empty() ? relaxedRound.reliefAtSwitchDb : relaxedRound.release.back().afterDb;
                const auto expectedSteps = std::floor (juce::jmin (switchSeconds * 10.0, (double) -relaxedRound.reliefAtSwitchDb / (double) releaseStepDb));
                logMessage ("  general after round 2: relief " + num (relaxedRound.reliefAtSwitchDb, 3) + " -> " + num (reliefAtEndDb, 3) + " dB over "
                            + num (switchSeconds, 1) + " s; " + juce::String (releaseSteps) + " release steps, " + juce::String (heldSteps) + " held, "
                            + juce::String (badSteps) + " bad, largest move " + num (largestMoveDb, 6) + " dB");

                expect (relaxedRound.releaseObserved, "the release after switching to general is observable (T-097 probe)");
                expect (relaxedRound.reliefAtSwitchDb <= minReliefToObserveDb,
                        "premise: round 2 ends with a contrast relief to release: " + num (relaxedRound.reliefAtSwitchDb, 3) + " dB");
                expectEquals (badSteps, 0, "general (contrast guard off): every guard sub-block releases 0.005 dB or holds; no other move");
                expect (largestMoveDb <= releaseStepDb + releaseToleranceDb, "no jump: the largest per-block move is one step");
                expect ((double) releaseSteps >= minReleaseFraction * expectedSteps && releaseSteps >= (int) (minReleaseFraction * steppedWhileRetreated),
                        "the relief keeps releasing: " + juce::String (releaseSteps) + " of " + juce::String (steppedWhileRetreated)
                        + " guard sub-blocks (expected about " + juce::String ((int) expectedSteps) + ")");

                if (reliefAtEndDb < -releaseStepDb)
                    expectWithinAbsoluteError (reliefAtEndDb, relaxedRound.reliefAtSwitchDb + (float) releaseSteps * releaseStepDb,
                                               (float) (releaseSteps + 1) * releaseToleranceDb, "relief moved by exactly the counted steps");
            }
            else
            {
                logMessage ("  main: round 1 (factory) loss " + num (factoryRound.lossLu) + " LU (recorded only)");
                expect (relaxedRound.lossLu > clubLossLimitLu,
                        "main: round 2 loses more than 1.5 LU (G_trim cancels the contrast guard's retreat): " + num (relaxedRound.lossLu));
            }
        }

        // 一轮：Tech House、俱乐部、GLAZE 100、冲击，124 BPM 报拍点，从第一个样本放到整首结束（多 0.2 s 让最后一段的出口按链延迟完整出来），
        // 入口 / 出口各一只表。thenSwitchToGeneral：放完后换成通用档再跑 switchSeconds，逐块记下落差退让量（新版探针）。
        t097scenario::ContrastRound runContrastRound (const glaze::Constants& constants, const Signal& song,
                                                      const t097scenario::ContrastSongDesign& design, const juce::String& label,
                                                      bool thenSwitchToGeneral, double switchSeconds)
        {
            using namespace t097scenario;
            ContrastRound result;
            const auto noValue = std::numeric_limits<double>::quiet_NaN();

            auto params = defaultParams (100.0f, 2);   // GLAZE 100、冲击（风格偏移 0）
            params.glazeGenre = 2;                     // Tech House（House 三类共用 house.guards）
            params.glazeDelivery = 2;                  // 俱乐部
            Rig rig (constants, song, params);
            rig.tempoBpm = design.bpm;

            SubBlockTap inputTap (rig.sampleRate), outputTap (rig.sampleRate);
            juce::AudioBuffer<float> inputBlock (numChannels, rig.blockLength);
            const auto songSamples = (std::int64_t) song.front().size();
            const auto songSeconds = (double) songSamples / rig.sampleRate;
            const auto blocks = rig.blocksFor (songSeconds + 0.2);
            auto nextLog = logEverySeconds;

            for (int b = 0; b < blocks; ++b)
            {
                stepMeasured (rig, inputTap, outputTap, inputBlock);
                const auto now = (double) (b + 1) * (double) rig.blockLength / rig.sampleRate;
                const auto guards = readGuards (rig);
                const auto probe = rig.probe();
                result.budgetActed = result.budgetActed || probe.guardDb < 0.0f;
                result.psrActed = result.psrActed || guards.psrReliefDb < 0.0f;
                result.deepestContrastReliefDb = juce::jmin (result.deepestContrastReliefDb, guards.contrastReliefDb);

                if (now >= nextLog)
                {
                    nextLog += logEverySeconds;
                    const auto section = design.sectionAt (juce::jmin (now, songSeconds - 1.0e-3));
                    const auto in = latestShortTerm (inputTap);
                    const auto out = latestShortTerm (outputTap);
                    logMessage ("  [" + label + "] t " + num (now, 1) + " s " + (ContrastSongDesign::isDrop (section) ? "drop     " : "breakdown")
                                + " | contrast relief " + num (guards.contrastReliefDb) + " dB"
                                + (guards.exact ? " (judged " + juce::String (guards.contrastJudged ? "yes" : "no") + ", loss reading "
                                                      + num (guards.contrastLossLu) + " LU, trim " + (guards.trimFrozen ? "frozen" : "free") + ")"
                                                : juce::String())
                                + " | in S " + num (in.valid ? in.lufs : noValue) + " / out S " + num (out.valid ? out.lufs : noValue) + " LUFS"
                                + " | G_trim " + num (probe.appliedBiasDb) + " dB | budget " + num (probe.guardDb) + " dB | PSR relief "
                                + num (guards.psrReliefDb) + " dB | target " + num (rig.status().effectiveTargetLufs) + " LUFS | limit gain "
                                + num (rig.chain.getEffectiveParams().limit.gainDb) + " dB");
                }
            }

            // ---- 段落响度（house_gate ③）：入口按段，出口按链延迟往后挪 ----
            result.latencySamples = rig.chain.getLatencySamples();
            result.dropped = inputTap.dropped || outputTap.dropped;
            result.endContrastReliefDb = readGuards (rig).contrastReliefDb;
            const auto length = subBlockLengthFor (rig.sampleRate);
            const auto latency = (std::int64_t) result.latencySamples;
            result.inputLufs.assign ((size_t) design.sections, noValue);
            result.outputLufs.assign ((size_t) design.sections, noValue);

            for (int k = 0; k < design.sections; ++k)
            {
                const auto begin = design.sectionStart (k, rig.sampleRate, songSamples);
                const auto end = design.sectionStart (k + 1, rig.sampleRate, songSamples);
                result.inputLufs[(size_t) k] = sectionLoudness (inputTap.blocks, length, begin, end);
                result.outputLufs[(size_t) k] = sectionLoudness (outputTap.blocks, length, begin + latency, end + latency);
            }

            for (int k = 0; k + 1 < design.sections; k += 2)
            {
                const auto inputDepth = result.inputLufs[(size_t) k + 1] - result.inputLufs[(size_t) k];
                const auto outputDepth = result.outputLufs[(size_t) k + 1] - result.outputLufs[(size_t) k];
                logMessage ("  [" + label + "] breakdown " + juce::String (k / 2 + 1) + " -> drop: in " + num (result.inputLufs[(size_t) k]) + " -> "
                            + num (result.inputLufs[(size_t) k + 1]) + " LUFS (" + num (inputDepth) + " LU), out " + num (result.outputLufs[(size_t) k])
                            + " -> " + num (result.outputLufs[(size_t) k + 1]) + " LUFS (" + num (outputDepth) + " LU), loss " + num (inputDepth - outputDepth)
                            + " LU");
            }

            const auto lastDrop = (size_t) design.sections - 1;
            result.inputDepthLu = result.inputLufs[lastDrop] - result.inputLufs[lastDrop - 1];
            result.outputDepthLu = result.outputLufs[lastDrop] - result.outputLufs[lastDrop - 1];
            result.lossLu = result.inputDepthLu - result.outputDepthLu;
            logMessage ("  [" + label + "] latency " + juce::String (result.latencySamples) + " samples; deepest contrast relief "
                        + num (result.deepestContrastReliefDb) + " dB, at the end " + num (result.endContrastReliefDb) + " dB; budget ring "
                        + (result.budgetActed ? "retreated" : "never retreated") + "; PSR guard " + (result.psrActed ? "acted" : "never acted"));

            // ---- 换通用档之后的回程（新版）----
            if (thenSwitchToGeneral)
            {
                rig.params.glazeGenre = 0;   // 通用：generalGuards 出厂 {false, false}，落差守门关 → 已有退让按 0.05 dB/s 放回 0
                auto before = readGuards (rig);
                result.releaseObserved = before.exact;
                result.reliefAtSwitchDb = before.contrastReliefDb;
                const auto switchBlocks = rig.blocksFor (switchSeconds);
                result.release.reserve ((size_t) switchBlocks);

                for (int b = 0; b < switchBlocks; ++b)
                {
                    rig.step();
                    const auto after = readGuards (rig);
                    ReleaseSample sample;
                    sample.stepped = after.guardSubBlocks - before.guardSubBlocks;
                    sample.beforeDb = before.contrastReliefDb;
                    sample.afterDb = after.contrastReliefDb;
                    result.release.push_back (sample);
                    before = after;
                }
            }

            return result;
        }
    };

    static GlazeControllerGuardScenarioTests glazeControllerGuardScenarioTests;
}

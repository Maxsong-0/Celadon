/*
  ==============================================================================

    GlazeControllerTests.cpp
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
#include "../Source/dsp/GlazeCalibrator.h"
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
        //   lowAntiPhase：右声道低频反相（低频相关度 ≈ −1 < 0：反相保护的触发条件，Clay 子文档第 3 节）；
        //   highWide：左右噪声不相关（高频 P_S ≈ P_M：预测加宽后相关度 (1 − γ²) / (1 + γ²) < 0.25，宽度保护的触发条件）。
        //   两项都为假 = 单声道素材（S = 0：两种保护都不该动）。
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

        // 按 Clay 规格「最需要处理」造的画像（格式 2，特征全部有效、置信度 1；数字按 Clay 子文档第 2 节与根治修订 3.3 算）：
        //   真次声占比 −10 dB → need_sub = ramp(−10, −40, −28) = 1 → 低切给满（通透 20 Hz、温润 16 Hz）；
        //   低频 S/M −30 dB、ρ 0.98 → 单声道回放低音损失 10·log10((1 + 0.98) / 2) = −0.04 dB → need_mono = ramp(−0.04, −0.5, −3) = 0
        //   → 收单 0；
        //   高频 S/M −16 dB → need_w = ramp(−16, −10, −18) = 0.75 → GLAZE 60 通透加宽 1.5 × 0.75 × 0.6 × 1.0 = 0.675 dB
        //   （上限 γ² ≤ 0.6 × 10^1.6 ≈ 23.9，不受限）；
        //   「先试后用」的代价（ADR-012）全部有效且 ≤ 0.2 dB → 代价系数 1（照做）：低切 −0.5、收单 −0.3、加宽 0 dB。代价无效的
        //   画像三项都中性（旧画像同理），所以这里必须给。
        // 用例拿它当阳性对照（「自动」确实给出非中性值）；反相保护把收单从 0 拉到 1，一眼可辨。
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
        //   收单代价 −0.3 dB → f = 1（本来也不需要：need_mono = 0）。决策器应把低切、加宽记进 costLimitedMask。
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
            testPhasesAndFlags();
            testGlazeZeroLearnsButLeavesManualUntouched();
            testFirstListenButtonRestarts();
            testRelearnCandidateAndCancel();
            testDiscontinuitiesClearTheirOwnState();
            testInternalSliceTimeline();
            testOfflineDoesNotLearnOrCalibrate();
            testExecutionSnapshots();
            testSaveAndRestore();
            testSampleRateChange();
            testSameSpecReprepareKeepsHeardExecution();
            testGlazeAutomationPath();
            testStyleChangeIsAPlanTransition();
            testOlderAlgorithmPlanIsKept();
            testAlgorithmSixProjectIsCurrent();
            testOldHeatModelIsMigrated();
            testLiveTameAfterProfileLocks();
            testLiveToneAfterProfileLocks();
            testLiveColorAfterProfileLocks();
            testListenDoesNotPauseCalibration();
            testConstraintFeedbackConverges();
            testStuckGuardIsAbsorbed();
            testAbsorbingFeedbackKeepsCalibration();
            testProfileMismatch();
            testIrregularHostTimeline();
            testManualComesFirst();
            testNonFiniteOutputAndInputMeter();
            testAudioThreadDoesNotAllocate();

            // ---- v0.1.7.0 修坯 Clay（A6）----
            testClayThreeStates();
            testClayProfileFormatCompatibility();
            testClayOldProfileStaysNeutral();
            testClayTransitionBlend();
            testClayGuardThroughController();
            testClayPressureFeedback();
            testClaySnapshotRoundTrip();
            testClaySettingsJoinCalibrationReference();
            testClayAudioThreadDoesNotAllocate();

            // ---- T-012 阶段 4（ADR-012 修坯根治；审核 V2-03）----
            testClayCostMaskReachesStatus();
            testClayRestoreNeedsSupport();

            // ---- T-026（BUG-004，ADR-018）----
            testReferenceSurvivesParameterRoundTrip();
            testCalibrationSurvivesReopenDrift();
            testSlowLogKnobSweepIsNotStable();

            // ---- T-027（参考条件手动参数部分的缓存）----
            testManualConditionsCacheIsBitExact();
        }

    private:
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
                auto constants = fastConstants();
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
            auto guardedConstants = fastConstants();
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
            auto constants = fastConstants();
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
                auto constants = fastConstants();
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
                Rig rig (fastConstants(), makeMusic (defaultRate, 8.0, 25, 0.3f));
                rig.params.glazePercent = 100.0f;
                rig.params.limit.gainDb = 3.0f;
                rig.run (6.0);
                rig.sourceGain = 3.0f;

                bool neverBelow = true;
                float deepestGuard = 0.0f;

                rig.run (20.0, [&] (int)
                {
                    neverBelow = neverBelow && rig.chain.getEffectiveParams().limit.gainDb >= rig.params.limit.gainDb;
                    deepestGuard = juce::jmin (deepestGuard, rig.probe().executedGuardDb);
                });

                logMessage ("  deepest executed protection offset " + juce::String (deepestGuard, 2) + " dB with manual gain 3 dB");
                expect (deepestGuard < 0.0f, "the guard did act");
                expect (neverBelow, "effective Limit Gain >= manual Limit Gain on every block");
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

            // Clay 线性相位收单让整链多了固定延迟 D（48 kHz 574 样本，比一块 512 长；ADR-012）：Balance 已被 Inf 污染，但非有限值要
            // 晚 D 个样本才到输出，所以检测最迟在「延迟 / 块长 + 1」块之后。断言的是这个上限内一定检测到——不是放宽检测本身。
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
            expect (rig.phase() == Phase::listening, "still listening");
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
            expect (probe.commits >= 1 && probe.planTransitions >= 2 && probe.calibrationBegins >= 1 && probe.feedbackSteps >= 1
                    && probe.transportJumps >= 1 && probe.loopWraps >= 1 && probe.hostResets >= 1,
                    "the run went through the paths it claims to cover");
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

        // Clay 3. 旧工程（画像格式 1）：Clay 自动值恒为中性、保护不动（就算素材低频反相）——旧工程重新聆听之前 Clay 逐位直通、
        //    声音不变；状态提示「点重新聆听」（GLAZE > 0 且至少一项自动时）。两种旧存档都试：旧算法方案（照用保存的增量，还特意
        //    存了一份非中性的 Clay 值，确认它不被采用；拧 GLAZE 按新算法重算后仍中性）与当前算法方案（拧过 GLAZE 后再存的）。
        //    点「重新聆听」提交格式 2 的新画像后自动 Clay 启用（同一段反相低频，反相保护把收单拉到 1）、提示消失。
        //    阳性对照也见 Clay 5：同样的反相素材配格式 2 画像时保护会介入。
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
        //    单声道素材下两种保护都不动：保护之后 = 方案值，实际参数 = 三态合成。
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

            Rig rig (clayConstants(), material, defaultParams (60.0f, 3));
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

        // Clay 5. 实时保护经编排器生效（分析器看真实素材 → ClayGuard → 三态合成）：
        //    a) 反相：格式 2 画像的方案收单为 0；素材切到低频反相后「自动」的收单比例被拉到 1（状态「反相保护中」），切回正常素材后
        //       先保持再退回方案值；「关」态完全不受影响，也不报保护灯（保护照样在算，只是不作用）；
        //    b) 宽度：方案要加宽（画像很窄），素材的高频却很宽（预测加宽后相关度 < 0.25）→ 自动加宽被收到 0（状态「宽度保护中」），
        //       方案本身不变；换回窄素材后放开、回到方案值。
        // 依据：Clay 子文档第 3 节保护表、第 6 节第 5 条（「手动」「关」态不受保护改动）；ClayGuard.h。时间只放宽上限（分析器
        //    300 ms 平滑 + 资格期 0.15 s + 攻击；保持 2 s、释放 2 s），核对「会介入、会保持、会退回、只动自动态」，精确时序由
        //    ClayGuard / ClayGlaze 测试核对。
        // Clay 6. 修坯压力反馈（ADR-014）：方案要低切（clayStateFrom 的画像：need_sub = 1，通透 20 Hz；代价 −0.5 dB → 照做），
        //    素材却是 40 Hz 方波（±0.5，单声道）——4 阶高通让方波平顶倾斜、每个边沿都冲过 ±0.5，ClayModule 的峰值守护每半个周期
        //    都要把低切的改动量拉回来。编排器按有效音乐时间监测回收量，持续超过 0.1 就把自动 Clay 的缩放降一档（1 → 0.5 → 0.25 → 0），
        //    每档走方案过渡：
        //      - 25 s 内至少降两档（缩放 ≤ 0.25），状态报低切「因代价减量 / 跳过」（costLimitedMask 的低切位）；自动低切比学完时低；
        //      - 存档带着缩放（控制器节点的 clayPressureScale 属性），读回新链后缩放与方案一致；删掉这个属性的旧存档 = 1；
        //    阳性对照：同一画像、单声道正弦素材（60 / 110 Hz，守护几乎不动作）20 s 不降档。
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

            // 阳性对照：守护几乎不动作的素材不降档。
            {
                Rig calm (clayConstants(), normal, defaultParams (60.0f, 3));
                calm.chain.restoreGlazeStateXml (saved.get());
                calm.run (20.0);
                expect (calm.probe().clayAutomaticSupported);
                expectEquals (calm.probe().clayPressureSteps, 0, "gentle material: no pressure step (smoothed pull-back "
                                                                 + juce::String (calm.probe().clayPressureSmoothed, 4) + ")");
                expectEquals (calm.probe().clayPressureScale, 1.0f);
            }

            Rig rig (clayConstants(), square, defaultParams (60.0f, 3));
            rig.chain.restoreGlazeStateXml (saved.get());
            rig.run (0.5);
            const auto initialLowCut = rig.probe().clayPlanned.lowCutHz;
            expect (initialLowCut > 15.0f, "positive control: the restored profile plans a real low cut: " + juce::String (initialLowCut));

            const auto stepped = rig.runUntil (25.0, [&] { return rig.probe().clayPressureScale <= 0.25f; });
            rig.run (4.0);   // 让最后一次方案过渡走完

            const auto probe = rig.probe();
            logMessage ("  pressure steps " + juce::String (probe.clayPressureSteps) + ", scale " + juce::String (probe.clayPressureScale)
                        + ", planned low cut " + juce::String (probe.clayPlanned.lowCutHz) + " Hz (was " + juce::String (initialLowCut) + ")");
            expect (stepped && probe.clayPressureSteps >= 2, "the guard's sustained pull-back steps the scale down at least twice within 25 s");
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

        // Clay 6. 执行快照：保存 liveClay（保护之后实际执行的自动值）、方案里过渡混合后的 plan.clay 与 ClayGuard 状态；读回（实时 /
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
            expect (rejects (bad), "a negative guard floor");
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

        // Clay 8. 音频线程零分配：Clay 的各条路径——自动 + 反相保护 + 宽度保护、三态切换（手动 / 关 / 自动）、GLAZE 0 下的手动项、
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
                    "the run went through the phase guard, the width guard, a restored first block and a plan transition");
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

                    if (! glaze::GlazeCalibrator::referenceMatches (saved, now) || ! glaze::GlazeCalibrator::referenceMatches (now, saved))
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
                            || ! glaze::GlazeCalibrator::referenceMatches (legacy, now))
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

                    if (glaze::GlazeCalibrator::referenceMatches (saved, referenceFor (changed, cap)))
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
    };

    static GlazeControllerTests glazeControllerTests;
}

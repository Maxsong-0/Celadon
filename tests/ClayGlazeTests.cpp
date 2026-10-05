/*
  ==============================================================================

    ClayGlazeTests.cpp
    v0.1.7.0 修坯 Clay 的整链端到端集成测试（设计规格-Clay-分析决策与验收 第 6 节第 1、4 ～ 8 条的整链部分；ADR-009）。
    T-012 阶段 4 按 ADR-012 / 设计规格-Clay-根治修订 改写（Clay 固定延迟 D、先试后用、反相保护新触发规则），并补第 4 节第 2 条的合成回归：
      1. GLAZE 0：抽屉默认（三态都「自动」）、三项全「关」、Clay Bypass——整链输出逐位相同，并且与「改动前」自己串起来的
         Balance → Color → Limit 三模块链在 Clay 的位置插 D 样本纯延迟后逐位相同（D = Clay 上报的整个固定延迟：
         ADR-012 收单 D + ADR-014 峰值守护前瞻 G，48 kHz 574 + 528 = 1102；本文件下文的「D」都指它；整链延迟 = 旧值 + D）；
         Color / Limit 不动作时与原样三模块链平移 D 逐位相同；每块 effective.clay 中性。阳性对照：GLAZE 0 下手动低切，输出不同。
      2. GLAZE 60、合成立体声素材（宽带音乐 + 约 10 Hz 真次声 + 宽低频 + 很窄的高频，低切、加宽都「值得做」：需要度 > 0、试过的代价
         都 ≤ 0.2 dB）：学完后低切、加宽自动值都 > 0 且在规格范围内、收单 = 0 且分频点为中性默认值（ADR-015：自动不收单）、
         不报代价受限；同一素材三项全「关」时三项为 0；输出 40 ～ 125 Hz 相关度与全关相差 < 0.02（不收单，低频立体声不被改动）；
         OUT 表真峰值 ≤ Ceiling + 0.01 dB。
      3. 旧工程（算法 6、画像格式 1、存档里没有任何 Clay 字段）：Clay 实际值中性、clayNeedsRelearn、校准照旧适用；
         拧 GLAZE 仍中性；与「Clay 全关」逐位相同；点「重新聆听」后自动 Clay 启用。
      4. 反相提示（ADR-015 起只提示、不改收单）：学完正常素材后插入 2 s 低频反相段 → 资格期 0.15 s + 分析帧与块的滞后内亮灯、
         再 0.1 s 提示强度到 1；段后至少保持 2 s、再按 2 s 线性退回 0；每一块的收单都不变；「关」「手动」态同样不变。
      5. 手动态：GLAZE 0 时手动低切 30 Hz 生效（实际值恰为 30、15 Hz 分量降约 24 dB、200 Hz 以上不变）；T-032 起另有手动加宽
         +3 dB 在正常电平（−12 dBFS 宽粉噪）下 4 kHz 以上的 S/M 真的抬高 3 ± 0.3 dB（峰值守护不再回收加宽，研究 R4-01）。
      6. 块长与离线：Clay 生效时 64 / 1024 样本块 × 实时 / 离线各渲染 60 s——同块长实时与离线逐样本相同；
         64 与 1024 每个 3 s 窗的短期响度差 < 0.02 LU（Codex rendercheck 同口径）。
      7. 执行快照：学习 + 反相提示满强度时保存 → 新链读取 → 首块 effective.clay 与保存前相同；实时 / 离线续渲逐样本相同，
         读回的提示照样按时退回。
      8. Clay 生效时整链音频路径 0 分配（三态轮换、手动扫值、Bypass、反相保护、宿主旁通、GLAZE 过 0、离线、reset、
         重新聆听、读取存档都走到）。
      9. 根治修订 4(a) 已限幅（硬削顶）的底鼓素材：学完低切被「先试后用」否决（实际值 0、mask 置位、代价 ≥ 0.6 dB 而真次声
         需要度 > 0），四个收单候选代价 ≤ 0.3 dB；与修坯全关相比整链限幅 GR 最大增加 ≤ 0.3 dB。阳性对照：同一素材手动 20 Hz 低切
         让 GR 最大增加 > 1 dB（被拦下的确实是会抬高峰值的处理）。
     10. 根治修订 4(b) 长期 ρ ≈ 0.8 的宽低音、单声道底鼓 + 两声道独立宽垫（审核 V1-01 用例）：30 s 内反相保护 0 次触发。
         阳性对照：整体反相的低音在同一套计数下会触发。
     11. 根治修订 4(c) 整体反相低频（右声道的底鼓与贝斯整体反相、等功率）：从反相音频出现起到 a = 1 在推导上限之内
         （资格期 0.15 s + 攻击 0.1 s + 分析窗 + 平滑过零 + 块粒度；规格写的「250 ms」与 3.4 自相矛盾，见用例注释与 P4 报告）。
     12. 根治修订 4(d) 约 10 Hz 次声 + 音乐：低切开启、试过的代价 ≤ 0、不报代价受限；整链限幅器入口峰值不升（GR 不增）。

    都经 CeladonChain 驱动（与插件同一接线：两块表、编排器、四个模块都在链里）；只按公开接口与阶段 0 / 4 冻结的头文件写
    （ChainParams::claySettings / clay、glaze::Status 的 clay* 字段、GlazeProfile.h 的 ExecutionState::liveClay / clayGuard、
    GlazeTypes.h 的代价字段与 mask、LinearPhaseLowpass.h 的延迟公式），不依赖各单元的内部实现。时长常数用 setConstantsForTesting 缩短：
    聆听 2 ～ 3 s（或 12 s，见 confidentConstants）、方案过渡 0.5 s、校准 settle 0.5 s + 每轮 2 s（出厂 15 ～ 30 s / 3 s / 3 s + 10 s）。
    每个用例的「依据」与阈值写在用例注释里；新素材的特征与代价在测试服务器上用 numpy 按规格公式预先复算过（P4 报告附数）。

  ==============================================================================
*/

#include "TestHelpers.h"

#include "../Source/dsp/CeladonChain.h"
#include "../Source/dsp/GlazeProfile.h"
#include "../Source/dsp/LinearPhaseLowpass.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <memory>
#include <vector>

namespace celadon::test
{
    namespace
    {
        using glaze::Phase;

        constexpr int kChannels = 2;
        constexpr int kBlock = 512;
        constexpr double kRate = 48000.0;
        constexpr float kCeilingDb = -1.0f;

        // 「到 1」的判定：保护按块线性爬升、末块钳到 1；留 0.1 % 给浮点累加的舍入。
        constexpr float kAtOne = 0.999f;

        // 反相段：50 Hz 纯 S 正弦（左 +、右 −），功率 = 正常素材 40 ～ 125 Hz 功率的 25 倍（理由见用例 4 的时序推导），两端 5 ms 斜坡。
        constexpr double kAntiHz = 50.0;
        constexpr double kAntiPowerRatio = 25.0;
        constexpr double kRampSeconds = 0.005;

        // 反相保护整链时序推导用的分析器口径（MusicAnalyzer：实时低频相关度慢路 300 ms、快路 120 ms 的一阶功率平滑）与
        // 分析滞后（Balance 分析窗 2048 样本 + 一个 hop 512 样本 + 帧在 hop 后第 64 样本发布）。
        constexpr double kLiveSlowSeconds = 0.3;
        constexpr double kLiveFastSeconds = 0.12;
        constexpr double kAnalysisSeconds = (2048.0 + 512.0 + 64.0) / kRate;

        // v0.1.6.2 存档的方案 / 标定快照算法版本（GlazeTypes.h：7 = v0.1.7.0 修坯）。
        constexpr int kLegacyAlgorithmVersion = 6;

        //==============================================================================
        // 与 GlazeControllerTests 同一套短时长：聆听 2 ～ 3 s、方案过渡 0.5 s、校准 settle 0.5 s + 每轮 2 s。
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

        // 聆听满 12 s：立体声 / 超低频特征「所需观测 10 s」（分析决策与验收 1.3），观测充分度到 1、代表性 12 / 15 = 0.8（有播放
        // 位置）= 出厂满置信度；「先试后用」的代价要有效块满 10 s 才有效（根治修订 3.2），而候选从学习起点要先预热 0.25 s——
        // 只听 10 s 时代价差一点点不够、三项全部中性（决策器：代价无效 → 该项中性），所以留 2 s 余量。
        // Clay 的自动值与置信度 c 成正比（第 2 节），置信度高些，数值与音频上的差别都更明显；也不依赖「几秒后特征才算有效」。
        glaze::Constants confidentConstants()
        {
            auto c = fastConstants();
            c.minListenSeconds = 12.0;
            c.maxListenSeconds = 12.0;
            return c;
        }

        // Clay 的固定延迟（设计规格-Clay-根治修订 第 2 节 D = 2(L_max − 1)，L_max 按手动收单下限 40 Hz；ADR-014 再加峰值守护前瞻 G；
        // 48 kHz 为 574 + 528 = 1102 样本）。用 ClayModule 的公开公式算，不读模块内部。
        int clayDelaySamples (double sampleRate)
        {
            return ClayModule::latencyFor (sampleRate);
        }

        ChainParams glazeParams (float glazePercent)
        {
            ChainParams p;
            p.limit.ceilingDb = kCeilingDb;
            p.glazePercent = glazePercent;
            p.glazeStyle = 0;   // 自动风格
            return p;
        }

        ClaySettings allOff()
        {
            ClaySettings settings;
            settings.lowCutMode = ClayMode::off;
            settings.monoMode = ClayMode::off;
            settings.widthMode = ClayMode::off;
            return settings;
        }

        //==============================================================================
        // 合成立体声素材的配方：每一项的立体声关系都是确定的，便于按规格第 2 节的公式预估画像与自动值。
        struct ClayRecipe
        {
            float gain = 0.4f;               // 整体增益（峰值约 −6 dBFS）
            double bassPhaseDegrees = 0.0;   // 贝斯右声道相对左声道的相位差：0 = 单声道；60 = 宽（ρ = cos 60° = 0.5，S/M = tan² 30° ≈ −4.8 dB）
            bool narrowHighs = false;        // true：军鼓 / 踩镲两声道同一噪声 + 各自 0.08 倍（−22 dB）的不相关噪声 → 高频 S/M ≈ −25 dB；
                                             // false：两声道各自独立的噪声（S/M ≈ 0 dB，本来就宽）
            double subHz = 20.0;             // 超低频正弦（两声道同相）
            float subAmplitude = 0.0f;       // 增益前的幅度；0 = 无
            bool lowRightInverted = false;   // true：右声道的底鼓、贝斯、超低频整体反相（等功率的「整体反相低频」，用例 11）
        };

        // 用例 2、3、6、8 的素材：宽带音乐 + 约 10 Hz 真次声 + 宽低频 + 很窄的高频，按根治修订 3.3 三项都「值得做」（需要度 > 0、
        // 试过的代价 ≤ 0.2 dB）。测试服务器上用 numpy 按规格公式复算（种子 101 / 202 / 303 / 505 / 606 / 808，学习 12 s）：
        //   - 真次声：M 的 16 Hz 以下占比 ≈ −2.4 dB → need_sub = ramp(−2.4, −40, −28) = 1；IIR 20 Hz 低切的代价 −2.1 ～ −0.4 dB
        //     （去掉次声压低的峰值多于相位旋转抬高的）→ 照做。次声取 10.3 Hz：正好 10 Hz 时每拍（0.5 s = 5 个周期）落在同一相位，
        //     代价只由那一个相位决定、忽正忽负（实测 +0.04 ～ +0.48 dB）；10.3 Hz 的相位在各拍间漂移，代价稳定为负；
        //   - 低频：贝斯左右差 75°（贝斯自身 ρ = cos 75° ≈ 0.26，底鼓单声道）→ 40 ～ 125 Hz ρ ≈ 0.58 → 单声道回放低音损失
        //     10·log10((1 + 0.58) / 2) ≈ −1.0 dB → need_mono = ramp(−1.0, −0.5, −3) ≈ 0.21；四个分频点的收单代价 −0.7 ～ −0.2 dB → 照做；
        //     实时慢路最低约 0.44（> 快路门 0.3）、快路最低约 0.35：反相保护不会触发（60° 时收单需要度只有 0.06，太小看不出效果；
        //     90° 时慢路最低 0.25、进了快路门）；
        //   - 2 ～ 16 kHz：S/M ≈ −25 dB → need_w = ramp(−25, −10, −18) = 1；加宽代价 −0.01 ～ +0.02 dB → 照做；预测加宽后 ρ' 远高于 0.25。
        ClayRecipe stereoTestRecipe()
        {
            ClayRecipe recipe;
            recipe.gain = 0.4f;
            recipe.bassPhaseDegrees = 75.0;
            recipe.narrowHighs = true;
            recipe.subHz = 10.3;
            recipe.subAmplitude = 0.3f;
            return recipe;
        }

        // 可复现的类音乐素材（120 BPM：底鼓、贝斯、和声垫、军鼓、踩镲；结构同 GlazeControllerTests::makeMusic），按配方给立体声关系。
        // 底鼓与它的咔嗒是单声道（两声道同一噪声；lowRightInverted 时右声道的底鼓、贝斯、超低频整体取反）。不按峰值归一：
        // 配方里的电平关系原样保留（反相段按实测低频功率定电平）。
        Signal makeClayMusic (double seconds, juce::int64 seed, const ClayRecipe& recipe)
        {
            constexpr double beatSeconds = 0.5;
            constexpr double bassNotesHz[] = { 55.0, 73.42, 82.41, 61.74 };
            constexpr double padNotesHz[]  = { 220.0, 277.18, 329.63, 440.0 };

            const auto numSamples = static_cast<int> (seconds * kRate);
            auto signal = makeSilence (kChannels, numSamples);
            const auto twoPi = juce::MathConstants<double>::twoPi;
            const auto snareCoeff = std::exp (-twoPi * 3000.0 / kRate);
            const auto hatCoeff   = std::exp (-twoPi * 6000.0 / kRate);
            const auto bassPhase  = juce::degreesToRadians (recipe.bassPhaseDegrees);
            const auto gain = static_cast<double> (recipe.gain);

            juce::Random common (seed), left (seed + 7919), right (seed + 2 * 7919);
            std::array<double, static_cast<size_t> (kChannels)> snare {}, hat {};

            for (int i = 0; i < numSamples; ++i)
            {
                const auto t = static_cast<double> (i) / kRate;
                const auto beat = static_cast<int> (t / beatSeconds);
                const auto beatPos = t - static_cast<double> (beat) * beatSeconds;

                const auto shared = static_cast<double> (common.nextFloat()) * 2.0 - 1.0;
                const std::array<double, static_cast<size_t> (kChannels)> own { static_cast<double> (left.nextFloat()) * 2.0 - 1.0,
                                                                                static_cast<double> (right.nextFloat()) * 2.0 - 1.0 };

                const auto kick = 0.55 * std::sin (twoPi * 55.0 * beatPos) * std::exp (-beatPos / 0.09)
                                + 0.15 * shared * std::exp (-beatPos / 0.003);
                const auto bassHz = bassNotesHz[static_cast<size_t> (beat % 4)];
                const auto bassEnvelope = juce::jmin (1.0, beatPos / 0.01) * std::exp (-beatPos / 0.6);
                const auto sub = static_cast<double> (recipe.subAmplitude) * std::sin (twoPi * recipe.subHz * t);

                for (int ch = 0; ch < kChannels; ++ch)
                {
                    const auto index = static_cast<size_t> (ch);
                    const auto phase = ch == 0 ? 0.0 : bassPhase;
                    const auto lowSign = (ch == 1 && recipe.lowRightInverted) ? -1.0 : 1.0;
                    const auto bass = bassEnvelope * (0.28 * std::sin (twoPi * bassHz * beatPos + phase)
                                                      + 0.10 * std::sin (twoPi * 2.0 * bassHz * beatPos + phase));

                    double pad = 0.0;
                    for (auto hz : padNotesHz)
                        pad += 0.05 * std::sin (twoPi * hz * t + 0.7 * static_cast<double> (ch));

                    const auto noise = recipe.narrowHighs ? shared + 0.08 * own[index] : own[index];
                    snare[index] = snareCoeff * snare[index] + (1.0 - snareCoeff) * noise;
                    hat[index]   = hatCoeff * hat[index] + (1.0 - hatCoeff) * noise;
                    const auto snareHit = (beat % 2 == 1) ? 0.6 * snare[index] * std::exp (-beatPos / 0.12) : 0.0;
                    const auto hatHit = 0.08 * (noise - hat[index]) * std::exp (-std::fmod (t, beatSeconds * 0.5) / 0.03);

                    signal[index][static_cast<size_t> (i)] = static_cast<float> (gain * (lowSign * (kick + bass + sub) + pad + snareHit + hatHit));
                }
            }

            return signal;
        }

        // 从 source 的 startIndex 起无缝接着往下放（按素材长度循环），并在 [segmentStart, segmentStart + segmentLength) 上叠一段
        // 低频反相正弦（左 +、右 −：纯 S，M 不变）。两端各 5 ms 升余弦斜坡：起止处的阶跃本身会带来宽带瞬态，与反相无关。
        Signal makeAntiPhaseContinuation (const Signal& source, size_t startIndex, int totalSamples,
                                          int segmentStart, int segmentLength, float amplitude, double hz)
        {
            auto out = makeSilence (kChannels, totalSamples);
            const auto length = source.front().size();
            const auto ramp = static_cast<int> (kRampSeconds * kRate);

            for (int i = 0; i < totalSamples; ++i)
            {
                double anti = 0.0;
                const auto k = i - segmentStart;

                if (k >= 0 && k < segmentLength)
                {
                    const auto edge = juce::jmin (k, segmentLength - 1 - k);
                    const auto fade = edge >= ramp ? 1.0
                                                   : 0.5 - 0.5 * std::cos (juce::MathConstants<double>::pi * static_cast<double> (edge)
                                                                           / static_cast<double> (ramp));
                    anti = static_cast<double> (amplitude) * fade
                         * std::sin (juce::MathConstants<double>::twoPi * hz * static_cast<double> (k) / kRate);
                }

                for (int ch = 0; ch < kChannels; ++ch)
                {
                    const auto x = static_cast<double> (source[static_cast<size_t> (ch)][(startIndex + static_cast<size_t> (i)) % length]);
                    out[static_cast<size_t> (ch)][static_cast<size_t> (i)] = static_cast<float> (ch == 0 ? x + anti : x - anti);
                }
            }

            return out;
        }

        //==============================================================================
        // 测量用双二阶（RBJ cookbook 公式，double，直接 I 型）：与被测代码无关的独立实现。
        struct MeasureBiquad
        {
            MeasureBiquad (bool highPass, double hz, double q)
            {
                const auto w0 = juce::MathConstants<double>::twoPi * hz / kRate;
                const auto cosW0 = std::cos (w0);
                const auto alpha = std::sin (w0) / (2.0 * q);
                const auto a0 = 1.0 + alpha;
                const auto k = highPass ? 1.0 + cosW0 : 1.0 - cosW0;
                b0 = 0.5 * k / a0;
                b1 = (highPass ? -k : k) / a0;
                b2 = b0;
                a1 = -2.0 * cosW0 / a0;
                a2 = (1.0 - alpha) / a0;
            }

            double process (double x) noexcept
            {
                const auto y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
                x2 = x1;
                x1 = x;
                y2 = y1;
                y1 = y;
                return y;
            }

            double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
            double x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0;
        };

        // 4 阶 Butterworth = 两级双二阶，Q = 0.5412 / 1.3066。
        struct FourthOrder
        {
            FourthOrder (bool highPass, double hz) : first (highPass, hz, 0.54119610), second (highPass, hz, 1.30656296) {}

            double process (double x) noexcept   { return second.process (first.process (x)); }

            MeasureBiquad first, second;
        };

        // 一个声道过「4 阶高通 lowHz + 4 阶低通 highHz」；lowHz ≤ 0 不高通、highHz ≤ 0 不低通。
        std::vector<double> bandFiltered (const std::vector<float>& channel, double lowHz, double highHz)
        {
            std::vector<double> out (channel.size());
            FourthOrder highPass (true, lowHz > 0.0 ? lowHz : 1.0), lowPass (false, highHz > 0.0 ? highHz : 1.0);

            for (size_t i = 0; i < channel.size(); ++i)
            {
                auto x = static_cast<double> (channel[i]);

                if (lowHz > 0.0)
                    x = highPass.process (x);

                if (highHz > 0.0)
                    x = lowPass.process (x);

                out[i] = x;
            }

            return out;
        }

        // 某频段的平均功率（两声道平均；跳过开头 skip 个样本，让测量滤波器的起振过去）。
        double bandPower (const Signal& signal, double lowHz, double highHz, int skip)
        {
            double sum = 0.0;
            std::int64_t count = 0;

            for (const auto& channel : signal)
            {
                const auto y = bandFiltered (channel, lowHz, highHz);

                for (size_t i = static_cast<size_t> (skip); i < y.size(); ++i)
                {
                    sum += y[i] * y[i];
                    ++count;
                }
            }

            return count > 0 ? sum / static_cast<double> (count) : 0.0;
        }

        // 某频段的左右相关度 ρ = Σlr / √(Σl² · Σr²)。
        double bandCorrelation (const Signal& signal, double lowHz, double highHz, int skip)
        {
            const auto l = bandFiltered (signal[0], lowHz, highHz);
            const auto r = bandFiltered (signal[1], lowHz, highHz);
            double lr = 0.0, ll = 0.0, rr = 0.0;

            for (size_t i = static_cast<size_t> (skip); i < l.size(); ++i)
            {
                lr += l[i] * r[i];
                ll += l[i] * l[i];
                rr += r[i] * r[i];
            }

            return ll > 0.0 && rr > 0.0 ? lr / std::sqrt (ll * rr) : 0.0;
        }

        double powerRatioDb (double numerator, double denominator)
        {
            return 10.0 * std::log10 (juce::jmax (numerator, 1.0e-30) / juce::jmax (denominator, 1.0e-30));
        }

        // 单声道的 M = (L + R) / 2（sign = +1）或 S = (L − R) / 2（sign = −1），double 里算（T-032 加宽用例量 S/M 用）。
        Signal midOrSide (const Signal& signal, double sign)
        {
            Signal out = makeSilence (1, static_cast<int> (signal[0].size()));

            for (size_t i = 0; i < signal[0].size(); ++i)
                out[0][i] = static_cast<float> (0.5 * (static_cast<double> (signal[0][i]) + sign * static_cast<double> (signal[1][i])));

            return out;
        }

        // 4 kHz 以上的 S/M（dB）。整链里 Balance 的 EQ、Glue / 限幅器的联动增益对 S 与 M 一视同仁，S/M 不受它们影响。
        double highSideToMidDb (const Signal& signal, int skip)
        {
            return powerRatioDb (bandPower (midOrSide (signal, -1.0), 4000.0, 0.0, skip), bandPower (midOrSide (signal, 1.0), 4000.0, 0.0, skip));
        }

        //==============================================================================
        bool sameBits (float a, float b) noexcept
        {
            return std::memcmp (&a, &b, sizeof (float)) == 0;
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

        // delayed 比 reference 晚 shift 个样本：delayed[n + shift] 与 reference[n] 逐字节相同（n 覆盖两者都有的部分）。
        bool sameShifted (const Signal& delayed, const Signal& reference, int shift)
        {
            if (delayed.size() != reference.size() || shift < 0)
                return false;

            for (size_t ch = 0; ch < delayed.size(); ++ch)
            {
                const auto length = juce::jmin (delayed[ch].size(), reference[ch].size());

                if (static_cast<size_t> (shift) >= length)
                    return false;

                const auto count = length - static_cast<size_t> (shift);

                if (std::memcmp (delayed[ch].data() + shift, reference[ch].data(), count * sizeof (float)) != 0)
                    return false;
            }

            return true;
        }

        bool sameBuffers (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
        {
            if (a.getNumChannels() != b.getNumChannels() || a.getNumSamples() != b.getNumSamples())
                return false;

            for (int ch = 0; ch < a.getNumChannels(); ++ch)
                if (std::memcmp (a.getReadPointer (ch), b.getReadPointer (ch), static_cast<size_t> (a.getNumSamples()) * sizeof (float)) != 0)
                    return false;

            return true;
        }

        // 三项实际值都为 0（设计规格-Clay 第 4 节：此时 Clay 逐位直通；monoHz 在 monoAmount = 0 时无意义，不看）。
        bool isNeutral (const ClayModule::Params& p) noexcept
        {
            return p.lowCutHz == 0.0f && p.monoAmount == 0.0f && p.widthDb == 0.0f;
        }

        // 三项实际值都 > 0：低切、收单、加宽都在工作。
        bool allActive (const ClayModule::Params& p) noexcept
        {
            return p.lowCutHz > 0.0f && p.monoAmount > 0.0f && p.widthDb > 0.0f;
        }

        // 自动态能动的两项（ADR-015 起自动不收单）：低切与加宽都 > 0。
        bool automaticActive (const ClayModule::Params& p) noexcept
        {
            return p.lowCutHz > 0.0f && p.widthDb > 0.0f;
        }

        bool sameClay (const ClayModule::Params& a, const ClayModule::Params& b) noexcept
        {
            return sameBits (a.lowCutHz, b.lowCutHz) && sameBits (a.monoHz, b.monoHz) && sameBits (a.monoAmount, b.monoAmount)
                && sameBits (a.widthDb, b.widthDb) && a.bypass == b.bypass;
        }

        // 交给三个老模块的实际参数与 GLAZE 两项逐字段逐位相同（浮点按位、布尔与枚举按值；同 GlazeControllerTests::sameParams）。
        bool sameModules (const ChainParams& a, const ChainParams& b) noexcept
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

        // 交给四个模块的实际参数全部逐位相同（再加 Clay）。
        bool sameExecuted (const ChainParams& a, const ChainParams& b) noexcept
        {
            return sameModules (a, b) && sameClay (a.clay, b.clay);
        }

        juce::String clayText (const ClayModule::Params& p)
        {
            return "low cut " + juce::String (p.lowCutHz, 2) + " Hz, mono " + juce::String (p.monoAmount, 3)
                 + " below " + juce::String (p.monoHz, 1) + " Hz, width " + juce::String (p.widthDb, 3) + " dB"
                 + (p.bypass ? ", bypass" : "");
        }

        juce::String statusText (const glaze::Status& s)
        {
            return "low cut " + juce::String (s.clayLowCutHz, 2) + " Hz, mono " + juce::String (s.clayMonoAmount, 3)
                 + " below " + juce::String (s.clayMonoHz, 1) + " Hz, width " + juce::String (s.clayWidthDb, 3) + " dB"
                 + ", phase guard " + juce::String (static_cast<int> (s.clayPhaseGuardActive))
                 + ", width guard " + juce::String (static_cast<int> (s.clayWidthGuardActive))
                 + ", needs relearn " + juce::String (static_cast<int> (s.clayNeedsRelearn));
        }

        juce::String featureText (const char* label, const glaze::Feature& f)
        {
            return juce::String (label) + " " + juce::String (f.value, 2) + " (valid " + juce::String (static_cast<int> (f.valid))
                 + ", c " + juce::String (f.confidence, 2) + ")";
        }

        // ---- ADR-012「先试后用」的画像代价（GlazeTypes.h MusicProfile：低切、收单 ×4、加宽）----
        juce::String costText (const glaze::MusicProfile& p)
        {
            auto text = featureText ("lowCut", p.lowCutPeakCostDb);

            for (size_t i = 0; i < p.monoPeakCostDb.size(); ++i)
                text += ", " + featureText ("mono", p.monoPeakCostDb[i]) + " @ "
                      + juce::String (glaze::clayMonoCandidateHz[i], 0) + " Hz";

            return text + ", " + featureText ("width", p.widthPeakCostDb);
        }

        bool allCostsValid (const glaze::MusicProfile& p)
        {
            auto valid = p.lowCutPeakCostDb.valid && p.widthPeakCostDb.valid;

            for (const auto& cost : p.monoPeakCostDb)
                valid = valid && cost.valid;

            return valid;
        }

        // 五个代价里最大的（dB；都无效时 −∞）。
        float maxCost (const glaze::MusicProfile& p)
        {
            auto largest = -std::numeric_limits<float>::infinity();

            for (const auto* cost : { &p.lowCutPeakCostDb, &p.widthPeakCostDb, &p.monoPeakCostDb[0], &p.monoPeakCostDb[1],
                                      &p.monoPeakCostDb[2], &p.monoPeakCostDb[3] })
                if (cost->valid)
                    largest = juce::jmax (largest, cost->value);

            return largest;
        }

        // 把一块输出（buffer 的全部样本）存进整段信号的第 block 块（每块长度相同）。
        void storeBlock (Signal& out, const juce::AudioBuffer<float>& buffer, int block)
        {
            const auto length = buffer.getNumSamples();

            for (int ch = 0; ch < kChannels; ++ch)
                std::copy (buffer.getReadPointer (ch), buffer.getReadPointer (ch) + length,
                           out[static_cast<size_t> (ch)].begin() + static_cast<std::ptrdiff_t> (block) * length);
        }

        //==============================================================================
        // 一条链 + 宿主模拟（同 GlazeControllerTests::Rig）：每块从素材里循环取样、报播放位置（默认在播、位置随块前进），交参数，处理。
        struct ClayRig
        {
            // 与处理器的 prepareToPlay 同一顺序：先交参数再 prepare（第一块由 step 按宿主信息再交一次）。
            ClayRig (const glaze::Constants& constants, Signal material, const ChainParams& initial, int block = kBlock)
                : params (initial), source (std::move (material)), blockLength (block), buffer (kChannels, block)
            {
                chain.getGlazeController().setConstantsForTesting (constants);
                host.hasPlayState = true;
                host.isPlaying = true;
                host.hasPosition = true;
                chain.setParams (params, false);
                chain.prepare ({ kRate, static_cast<juce::uint32> (blockLength), static_cast<juce::uint32> (kChannels) });
            }

            // 一块：取素材 → 报宿主信息 → 交参数 → 处理。返回链是否报告了非有限输出。
            bool step()
            {
                const auto length = source.front().size();
                buffer.setSize (kChannels, blockLength, false, false, true);

                for (int ch = 0; ch < kChannels; ++ch)
                {
                    const auto& channel = source[static_cast<size_t> (ch)];
                    auto* out = buffer.getWritePointer (ch);

                    for (int i = 0; i < blockLength; ++i)
                        out[i] = channel[(sourceIndex + static_cast<size_t> (i)) % length];
                }

                sourceIndex = (sourceIndex + static_cast<size_t> (blockLength)) % length;
                host.positionSamples = position;
                chain.setHostInfo (host);
                chain.setParams (params, hostBypassed);
                const auto nonFinite = chain.process (buffer);

                if (host.isPlaying || host.offline)
                    position += blockLength;

                return nonFinite;
            }

            int blocksFor (double seconds) const
            {
                return juce::jmax (1, static_cast<int> (std::ceil (seconds * kRate / static_cast<double> (blockLength))));
            }

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

            // 走完「聆听 → 提交 → 过渡 → 校准 → 锁定」。返回是否在 maxSeconds 之内锁定且校准有效。
            bool learnAndCalibrate (double maxSeconds)
            {
                return runUntil (maxSeconds, [this]
                {
                    const auto s = status();
                    return phase() == Phase::locked && s.profileReady && s.calibrationValid;
                });
            }

            // 渲染 blocks 块并收集输出；perBlock 在每块处理之后调（参数是块序号）。
            template <typename PerBlock>
            Signal capture (int blocks, PerBlock&& perBlock)
            {
                auto out = makeSilence (kChannels, blocks * blockLength);

                for (int b = 0; b < blocks; ++b)
                {
                    step();
                    storeBlock (out, buffer, b);
                    perBlock (b);
                }

                return out;
            }

            Signal capture (int blocks)   { return capture (blocks, [] (int) {}); }

            glaze::Status status() const                        { return chain.getGlazeStatus(); }
            Phase phase() const                                 { return chain.getGlazeController().getPhaseForTesting(); }
            glaze::CommittedState committed() const             { return chain.getGlazeController().getCommittedStateForTesting(); }
            glaze::GlazeController::TestProbe probe() const     { return chain.getGlazeController().getProbeForTesting(); }
            const ClayModule::Params& clay() const              { return chain.getEffectiveParams().clay; }

            CeladonChain chain;
            ChainParams params;
            glaze::HostInfo host;
            Signal source;
            size_t sourceIndex = 0;
            int blockLength;
            std::int64_t position = 0;
            bool hostBypassed = false;
            juce::AudioBuffer<float> buffer;
        };

        //==============================================================================
        // 「改动前」的链：v0.1.6.2 的 CeladonChain 在 GLAZE 0 时把手动值原样交给三个模块、按 Balance → Color → Limit 处理
        // （两块表与编排器只测量；Listen 关着时不碰样本；交给限幅器的 listen 恒为 false）。用来直接对照
        // 「GLAZE 0 且抽屉默认 → 整链与改动前逐位相同」（分析决策与验收第 6 节第 1 条）。
        struct PreClayChain
        {
            // claySlotDelaySamples > 0：在 Balance 与 Color 之间（Clay 的位置）插一段这么长的 float 纯延迟——ClayModule 三项为 0 时的
            // 真实契约是「逐位纯延迟 D + G」（ClayModule.h；即本文件的 D），整链因此等于「旧链在 Clay 的位置插 D 样本纯延迟」，与 Color / Limit 设置无关。
            explicit PreClayChain (const ChainParams& p, int claySlotDelaySamples = 0)
                : balanceParams (p.balance), colorParams (p.color), limitParams (p.limit)
            {
                limitParams.listen = false;
                const juce::dsp::ProcessSpec spec { kRate, static_cast<juce::uint32> (kBlock), static_cast<juce::uint32> (kChannels) };

                // 与链相同：先交参数再 prepare（prepare 末尾自己 reset，平滑器落到目标）。
                balance.setParams (balanceParams);
                color.setParams (colorParams);
                limiter.setParams (limitParams);
                balance.prepare (spec);
                color.prepare (spec);
                limiter.prepare (spec);

                if (claySlotDelaySamples > 0)
                    delayLines.assign (static_cast<size_t> (kChannels), std::vector<float> (static_cast<size_t> (claySlotDelaySamples), 0.0f));
            }

            // 与链相同：每块先交参数，再按固定顺序处理。
            void process (juce::AudioBuffer<float>& io)
            {
                balance.setParams (balanceParams);
                color.setParams (colorParams);
                limiter.setParams (limitParams);
                balance.process (io);
                delayClaySlot (io);
                color.process (io);
                limiter.process (io);
            }

            // 各声道同一个环形读写位置：读出 D 个样本之前写进去的值，再写本样本（环初值 0 = 前 D 个输出是 0）。
            void delayClaySlot (juce::AudioBuffer<float>& io)
            {
                if (delayLines.empty())
                    return;

                const auto length = delayLines.front().size();

                for (int ch = 0; ch < io.getNumChannels(); ++ch)
                {
                    auto& line = delayLines[static_cast<size_t> (ch)];
                    auto* data = io.getWritePointer (ch);
                    auto index = position;

                    for (int i = 0; i < io.getNumSamples(); ++i)
                    {
                        const auto delayed = line[index];
                        line[index] = data[i];
                        data[i] = delayed;
                        index = (index + 1) % length;
                    }
                }

                position = (position + static_cast<size_t> (io.getNumSamples())) % length;
            }

            BalanceModule::Params balanceParams;
            ColorModule::Params colorParams;
            LimiterModule::Params limitParams;
            BalanceModule balance;
            ColorModule color;
            LimiterModule limiter;
            std::vector<std::vector<float>> delayLines;   // 空 = 不插延迟
            size_t position = 0;
        };

        //==============================================================================
        // rendercheck 的响度口径（tools/perf/bench.mm runRenderCheck 的 changedBlockRender）：输出送进一块独立的 LoudnessMeter，
        // 每个 3 s 边界（144000 样本）处切开、读一次短期响度（最近 3 s = 这一窗）；最后读整体响度。
        struct WindowedLoudness
        {
            WindowedLoudness()
            {
                meter.prepare ({ kRate, static_cast<juce::uint32> (kBlock), static_cast<juce::uint32> (kChannels) });
            }

            void add (juce::AudioBuffer<float>& block, int count)
            {
                for (int done = 0; done < count;)
                {
                    const auto windowEnd = (fed / windowSamples + 1) * windowSamples;
                    const auto take = static_cast<int> (juce::jmin (static_cast<std::int64_t> (count - done), windowEnd - fed));
                    juce::AudioBuffer<float> slice (block.getArrayOfWritePointers(), kChannels, done, take);
                    meter.process (slice);
                    done += take;
                    fed += take;

                    if (fed == windowEnd)
                        shortTerm.push_back (meter.getShortTermLufs());
                }
            }

            static constexpr std::int64_t windowSamples = 144000;   // 3 s @ 48 kHz

            LoudnessMeter meter;
            std::int64_t fed = 0;
            std::vector<float> shortTerm;
        };

        struct RenderResult
        {
            std::vector<float> shortTermLufs;          // 每个 3 s 窗的短期响度（窗末读）
            float integratedLufs = LoudnessMeter::noReading;
            bool realtimeEqualsOffline = true;         // 同块长的实时与离线：每块输出逐字节相同
            bool sameControls = true;                  // ……每块交给四个模块的实际参数逐位相同
            bool clayActiveOnFirstBlock = false;       // 读取存档后第一块三项 Clay 实际值都 > 0（「Clay 生效时」的前提）
            ClayModule::Params lastClay;               // 最后一块的 Clay 实际值（日志）
        };

        // 同一块长的实时与离线两条链各读同一份存档、从同一起点渲染 seconds 秒（锁步：每块比一次），实时那条的输出送进 3 s 窗响度表。
        RenderResult renderRealtimeAndOffline (const glaze::Constants& constants, const Signal& material, const ChainParams& params,
                                               const juce::XmlElement& state, int block, double seconds)
        {
            ClayRig realtime (constants, material, params, block);
            ClayRig offline (constants, material, params, block);
            offline.host.offline = true;
            offline.host.isPlaying = false;   // 真实宿主导出时常报「未播放」（同 GlazeControllerTests 的离线用例）
            realtime.chain.restoreGlazeStateXml (&state);
            offline.chain.restoreGlazeStateXml (&state);

            RenderResult result;
            WindowedLoudness loudness;
            const auto total = static_cast<std::int64_t> (seconds * kRate);

            for (std::int64_t rendered = 0; rendered < total; rendered += block)
            {
                realtime.step();
                offline.step();

                if (rendered == 0)
                    result.clayActiveOnFirstBlock = allActive (realtime.clay()) && allActive (offline.clay());

                result.realtimeEqualsOffline = result.realtimeEqualsOffline && sameBuffers (realtime.buffer, offline.buffer);
                result.sameControls = result.sameControls
                                   && sameExecuted (realtime.chain.getEffectiveParams(), offline.chain.getEffectiveParams());
                loudness.add (realtime.buffer, static_cast<int> (juce::jmin (static_cast<std::int64_t> (block), total - rendered)));
            }

            result.shortTermLufs = loudness.shortTerm;
            result.integratedLufs = loudness.meter.getIntegratedLufs();
            result.lastClay = realtime.clay();
            return result;
        }

        //==============================================================================
        // 反相保护用例（4、7）的素材：正常段 = 单声道低频（底鼓 + 贝斯两声道相同）、宽高频、无超低频——自动收单本来 ≈ 0；
        // 反相段 = 同一段音乐 + 50 Hz 纯 S 正弦，功率按构造 = 正常段 40 ～ 125 Hz 功率的 25 倍（用例 4 的时序推导要这个比例）。
        struct AntiPhaseFixture
        {
            Signal normal;
            double normalLowPower = 0.0;   // 正常段 40 ～ 125 Hz 的平均功率（每声道）
            float antiAmplitude = 0.0f;    // 反相正弦幅度：A² / 2 = 25 × normalLowPower
        };

        // 反相保护用例的正常素材配方：单声道低频（底鼓 + 贝斯两声道相同）、宽高频、无超低频——自动收单本来 ≈ 0。
        ClayRecipe antiPhaseFixtureRecipe()
        {
            ClayRecipe recipe;
            recipe.gain = 0.35f;
            recipe.bassPhaseDegrees = 0.0;
            recipe.narrowHighs = false;
            recipe.subAmplitude = 0.0f;
            return recipe;
        }

        AntiPhaseFixture makeAntiPhaseFixture()
        {
            AntiPhaseFixture fixture;
            fixture.normal = makeClayMusic (8.0, 404, antiPhaseFixtureRecipe());
            fixture.normalLowPower = bandPower (fixture.normal, 40.0, 125.0, 4096);
            fixture.antiAmplitude = static_cast<float> (std::sqrt (2.0 * kAntiPowerRatio * fixture.normalLowPower));
            return fixture;
        }

        //==============================================================================
        // ---- 根治修订第 4 节第 2 条（用例 9 ～ 12）的素材 ----

        // 一个声道过「4 阶 Butterworth 高通 lowHz + 低通 highHz」（double 版的 bandFiltered；≤ 0 = 不做那一边）。
        std::vector<double> bandLimited (std::vector<double> x, double lowHz, double highHz)
        {
            FourthOrder highPass (true, lowHz > 0.0 ? lowHz : 1.0), lowPass (false, highHz > 0.0 ? highHz : 1.0);

            for (auto& sample : x)
            {
                if (lowHz > 0.0)
                    sample = highPass.process (sample);

                if (highHz > 0.0)
                    sample = lowPass.process (sample);
            }

            return x;
        }

        // 按 RMS 缩放到 rmsDbfs（全 0 原样返回）。
        std::vector<double> withRms (std::vector<double> x, double rmsDbfs)
        {
            double sum = 0.0;

            for (const auto sample : x)
                sum += sample * sample;

            const auto rms = std::sqrt (sum / static_cast<double> (juce::jmax (static_cast<size_t> (1), x.size())));

            if (rms > 0.0)
                for (auto& sample : x)
                    sample *= std::pow (10.0, rmsDbfs / 20.0) / rms;

            return x;
        }

        // 粉噪（Paul Kellet 滤波器，审核 V1 用例同款；juce::Random 种子固定、可复现），RMS 归一到 rmsDbfs。前 8192 个样本预热不要。
        std::vector<double> makePinkNoise (int numSamples, double rmsDbfs, juce::int64 seed)
        {
            constexpr int warmUp = 8192;
            juce::Random random (seed);
            std::vector<double> out (static_cast<size_t> (numSamples), 0.0);
            double b0 = 0.0, b1 = 0.0, b2 = 0.0, b3 = 0.0, b4 = 0.0, b5 = 0.0, b6 = 0.0;

            for (int i = -warmUp; i < numSamples; ++i)
            {
                const auto white = static_cast<double> (random.nextFloat()) * 2.0 - 1.0;
                b0 = 0.99886 * b0 + white * 0.0555179;
                b1 = 0.99332 * b1 + white * 0.0750759;
                b2 = 0.96900 * b2 + white * 0.1538520;
                b3 = 0.86650 * b3 + white * 0.3104856;
                b4 = 0.55000 * b4 + white * 0.5329522;
                b5 = -0.7616 * b5 - white * 0.0168980;
                const auto pink = b0 + b1 + b2 + b3 + b4 + b5 + b6 + white * 0.5362;
                b6 = white * 0.115926;

                if (i >= 0)
                    out[static_cast<size_t> (i)] = pink;
            }

            return withRms (std::move (out), rmsDbfs);
        }

        // 底鼓：每 0.5 s 一下，50 Hz 指数衰减正弦（τ 120 ms），起音处音高从 90 Hz 扫落（常见合成底鼓；审核 V1 同款）。未归一。
        double kickSample (double beatPosition)
        {
            const auto twoPi = juce::MathConstants<double>::twoPi;
            return std::exp (-beatPosition / 0.12)
                 * std::sin (twoPi * (50.0 * beatPosition + 40.0 * 0.02 * (1.0 - std::exp (-beatPosition / 0.02))));
        }

        // 用例 9 的已限幅底鼓素材：单声道「底鼓 + 低音」总线按 RMS 归一到 −12 dBFS、推 +9 dB，加上单声道中频（粉噪 300 ～ 8000 Hz，
        // −24 dBFS）后一起硬削顶到 ±0.9（像已经母带限幅过的成品）；再叠 −34 dBFS 的 10.3 Hz 次声（两声道同相：真次声 → 低切「需要」）
        // 与两声道独立的 60 ～ 150 Hz 宽垫（各 −30 dBFS：让收单候选量得到东西）。
        Signal makeClippedKickMusic (double seconds, juce::int64 seed)
        {
            constexpr double bassNotesHz[] = { 55.0, 73.42, 82.41, 61.74 };
            const auto n = static_cast<int> (seconds * kRate);
            const auto twoPi = juce::MathConstants<double>::twoPi;
            std::vector<double> bus (static_cast<size_t> (n), 0.0);

            for (int i = 0; i < n; ++i)
            {
                const auto t = static_cast<double> (i) / kRate;
                const auto beat = static_cast<int> (t / 0.5);
                const auto beatPos = t - static_cast<double> (beat) * 0.5;
                const auto bass = 0.35 * juce::jmin (1.0, beatPos / 0.01) * std::exp (-beatPos / 0.6)
                                * std::sin (twoPi * bassNotesHz[static_cast<size_t> (beat % 4)] * beatPos);
                bus[static_cast<size_t> (i)] = kickSample (beatPos) + bass;
            }

            bus = withRms (std::move (bus), -12.0);
            const auto drive = std::pow (10.0, 9.0 / 20.0);
            const auto mids = withRms (bandLimited (makePinkNoise (n, -24.0, seed), 300.0, 8000.0), -24.0);
            const auto padLeft = withRms (bandLimited (makePinkNoise (n, -30.0, seed + 1), 60.0, 150.0), -30.0);
            const auto padRight = withRms (bandLimited (makePinkNoise (n, -30.0, seed + 2), 60.0, 150.0), -30.0);
            const auto rumbleAmplitude = std::sqrt (2.0) * std::pow (10.0, -34.0 / 20.0);
            auto out = makeSilence (kChannels, n);

            for (int i = 0; i < n; ++i)
            {
                const auto index = static_cast<size_t> (i);
                const auto clipped = juce::jlimit (-0.9, 0.9, bus[index] * drive + mids[index]);
                const auto rumble = rumbleAmplitude * std::sin (twoPi * 10.3 * static_cast<double> (i) / kRate);
                out[0][index] = static_cast<float> (clipped + rumble + padLeft[index]);
                out[1][index] = static_cast<float> (clipped + rumble + padRight[index]);
            }

            return out;
        }

        // 用例 10 的宽低音素材（审核 V1-01 的 ReviewV1Analyzer 用例改写，限带改用本文件的 4 阶 Butterworth）。中频都是单声道粉噪
        // （−22 dBFS、300 Hz 以上），让帧门与有效音乐门都在门上；低频：
        //   tonalRho08：单声道 55 Hz 音调低音（8 次谐波 1/h，−14 dBFS）+ 两声道独立的 30 ～ 150 Hz 低频噪声（−14 dBFS × 0.5，
        //               功率比 1 : 0.25 → 长期 ρ = 1 / 1.25 = 0.8）；
        //   kickAndPad：单声道底鼓（120 BPM，RMS −12 dBFS）+ 两声道独立的 60 ～ 150 Hz 宽垫（各 −20 dBFS）；
        //   antiPhase ：阳性对照，音调低音右声道反相（ρ = −1）。
        enum class WideBass { tonalRho08, kickAndPad, antiPhase };

        Signal makeWideBassMaterial (WideBass kind, double seconds)
        {
            const auto n = static_cast<int> (seconds * kRate);
            const auto twoPi = juce::MathConstants<double>::twoPi;
            const auto mids = bandLimited (makePinkNoise (n, -22.0, 501), 300.0, 0.0);

            std::vector<double> common (static_cast<size_t> (n), 0.0);
            auto left = common, right = common;

            if (kind == WideBass::kickAndPad)
            {
                for (int i = 0; i < n; ++i)
                {
                    const auto t = static_cast<double> (i) / kRate;
                    common[static_cast<size_t> (i)] = kickSample (t - 0.5 * std::floor (t / 0.5));
                }

                common = withRms (std::move (common), -12.0);
                left = withRms (bandLimited (makePinkNoise (n, -20.0, 801), 60.0, 150.0), -20.0);
                right = withRms (bandLimited (makePinkNoise (n, -20.0, 802), 60.0, 150.0), -20.0);
            }
            else
            {
                for (int i = 0; i < n; ++i)
                {
                    const auto t = static_cast<double> (i) / kRate;
                    double tone = 0.0;

                    for (int h = 1; h <= 8; ++h)
                        tone += std::sin (twoPi * 55.0 * static_cast<double> (h) * t + 0.3 * static_cast<double> (h)) / static_cast<double> (h);

                    common[static_cast<size_t> (i)] = tone;
                }

                common = withRms (std::move (common), -14.0);

                if (kind == WideBass::tonalRho08)
                {
                    const auto noiseGain = std::sqrt ((1.0 - 0.8) / 0.8);
                    left = withRms (bandLimited (makePinkNoise (n, -14.0, 601), 30.0, 150.0), -14.0);
                    right = withRms (bandLimited (makePinkNoise (n, -14.0, 602), 30.0, 150.0), -14.0);

                    for (auto* channel : { &left, &right })
                        for (auto& sample : *channel)
                            sample *= noiseGain;
                }
            }

            auto out = makeSilence (kChannels, n);

            for (int i = 0; i < n; ++i)
            {
                const auto index = static_cast<size_t> (i);
                const auto rightCommon = kind == WideBass::antiPhase ? -common[index] : common[index];
                out[0][index] = static_cast<float> (common[index] + left[index] + mids[index]);
                out[1][index] = static_cast<float> (rightCommon + right[index] + mids[index]);
            }

            return out;
        }

        // 渲染 seconds 秒，返回这段里限幅器 GR 的最大值（dB）；peakMinusGain 另给「GR − 这一块实际的 Limit Gain」的最大值
        //（只算 GR > 0.05 dB 的块：限幅时 GR = 入口峰值 + Limit Gain − Ceiling，扣掉增益就是入口峰值 − Ceiling；一块都没限幅时为 −∞）。
        struct LimiterReading
        {
            float grMaxDb = 0.0f;
            float peakMinusGainDb = -std::numeric_limits<float>::infinity();
        };

        LimiterReading readLimiter (ClayRig& rig, double seconds)
        {
            rig.chain.getAndResetLimitGainReductionDb();   // 清掉之前累计的最大值
            LimiterReading reading;

            rig.run (seconds, [&] (int)
            {
                const auto gr = rig.chain.getAndResetLimitGainReductionDb();
                reading.grMaxDb = juce::jmax (reading.grMaxDb, gr);

                if (gr > 0.05f)
                    reading.peakMinusGainDb = juce::jmax (reading.peakMinusGainDb, gr - rig.chain.getEffectiveParams().limit.gainDb);
            });

            return reading;
        }

        //==============================================================================
        // ---- 旧工程（v0.1.6.2，算法 6、画像格式 1）存档的构造 ----

        // v0.1.6.2 存档各节点的全部属性——逐字取自改动前的字段表（源码备份 source-baseline-2026-09-26-v0.1.6.2：
        // GlazeProfile.cpp 的 visitFeature / visitProfile / visitPlan / visitCalibration / toXml (CommittedState、ExecutionState)，
        // GlazeConstants.cpp 的 visitStyle / visitConstants，GlazeController.cpp 的 createStateXml）。旧工程的存档里只有这些属性。
        juce::StringArray legacyAttributes (const juce::String& tag)
        {
            juce::StringArray names;
            const auto add = [&names] (std::initializer_list<const char*> items)
            {
                for (auto* item : items)
                    names.add (item);
            };

            if (tag == glaze::stateTag)
            {
                add ({ "formatVersion", "hasProfile", "profileId", "lockedWarm", "lockedPunchy", "lockedClean" });
            }
            else if (tag == glaze::profileTag)
            {
                add ({ "formatVersion" });

                for (auto* feature : { "integratedLufs", "loudnessRangeLu", "plrDb", "truePeakDbtp", "spectralSlopeDbPerOct",
                                       "lowShare", "highShare", "lowBandRangeDb", "onsetDensityPerSecond", "onsetStrengthDb" })
                    for (auto* suffix : { "value", "valid", "seconds", "confidence" })
                        names.add (juce::String (feature) + "_" + suffix);

                add ({ "bandLongTermDb", "tameEvidenceDb", "recoverEvidenceDb", "bandConfidence", "effectiveSeconds", "representativeness" });
            }
            else if (tag == "PLAN")
            {
                add ({ "requestedTargetLufs", "effectiveTargetLufs", "requestedGainDb", "effectiveGainDb", "estimatedExcessDb",
                       "balanceRecover", "balanceTame", "colorToneDb", "colorGlue", "colorHeat", "limitGainDb", "limitClip",
                       "weightWarm", "weightPunchy", "weightClean", "limitedByConstraints", "limitedByManual",
                       "algorithmVersion", "constantsVersion" });
            }
            else if (tag == "CALIBRATION" || tag == "REFERENCE")
            {
                add ({ "gainBiasDb", "valid", "partial", "budgetLimited", "profileId", "style", "glazePercent",
                       "manualParamsHash", "ceilingDb", "lookaheadMs", "sampleRate" });
            }
            else if (tag == glaze::constantsTag)
            {
                add ({ "formatVersion", "version", "algorithmVersion" });

                for (auto* style : { "warm", "punchy", "clean" })
                    for (auto* field : { "preferredLufs", "heatDriveCap", "glueGrMeanCapDb", "glueGrP95CapDb", "clipDepthCapDb",
                                         "clipActivityCap", "limitGrMeanCapDb", "limitGrP95CapDb", "limitGrPeakCapDb", "recoverCap",
                                         "tameCap", "toneMaxDb", "shareHeat", "shareGlue", "shareClip", "shareLimit",
                                         "heatPeakReductionCapDb", "toneReferenceSlopeDbPerOct" })
                        names.add (juce::String (style) + "_" + field);

                add ({ "lraReferenceLu", "lraSlopeDbPerLu", "minPreferredLufs", "maxPreferredLufs", "ceilingForEstimateDb",
                       "fullConfidence", "minListenSeconds", "maxListenSeconds", "lraStableSeconds", "calibrationSettleSeconds",
                       "calibrationWindowSeconds", "calibrationMaxDb", "calibrationRounds", "guardMinDb", "guardAttackDbPerSecond",
                       "guardReleaseDbPerSecond", "guardTriggerSeconds", "guardWindowSeconds", "guardRecoverAfterSeconds",
                       "planTransitionSeconds",
                       "unlearnedInputLufs", "unlearnedPlrDb", "unlearnedStrength", "autoBaseWeight", "autoTransientStrengthLowDb",
                       "autoTransientStrengthHighDb", "autoTransientDensityLow", "autoTransientDensityHigh", "autoDynamicLraLowLu",
                       "autoDynamicLraHighLu", "autoBrightRangeDbPerOct", "autoWarmSustainWeight", "autoWarmBrightWeight",
                       "autoPunchyDynamicDamping", "glueThresholdAtFullDb", "glueRatio", "glueKneeDb", "glueMakeupFraction",
                       "glueDetectorOffsetDb", "glueP95OffsetDb", "glueLoudnessPerGrDb", "gluePeakPerGrDb", "glueLowRiskShareLow",
                       "glueLowRiskShareHigh", "glueLowRiskRangeLowDb", "glueLowRiskRangeHighDb", "glueLowRiskMaxCut",
                       "mismatchThresholdLu", "mismatchReleaseLu", "mismatchSustainSeconds", "mismatchTransitionSeconds",
                       "mismatchCooldownSeconds", "heatTableTruePeakDbtp", "heatTableAmount", "heatLoudnessDb", "heatPeakDb",
                       "clipOffThresholdDb", "clipKneeRatio", "clipActivityCoeff", "clipActivityExponent", "limitLossCoeff",
                       "limitLossExponent", "clipLossFloor", "limitMeanCoeff", "limitP95KneeDb", "clipRampMinFraction",
                       "clipMinimumDepthDb", "toneDbPerSlopeDb", "recoverEvidenceFullDb", "tameEvidenceFullDb",
                       "minimumFullDriveDb", "automaticRecoverFloor", "automaticTameFloor", "automaticHeatFloor",
                       "liveHeatMaxReduction", "liveGlueMaxReduction", "liveColorDensityDeadbandDb", "liveColorDensityRangeDb",
                       "liveColorBrightDeadbandDbPerOct", "liveColorBrightRangeDbPerOct", "liveColorAttackPerSecond",
                       "liveColorReleasePerSecond" });
            }
            else if (tag == "GLAZE_CONTROLLER")
            {
                add ({ "formatVersion", "feedbackMaxGainDb" });
            }
            else if (tag == glaze::executionTag)
            {
                add ({ "formatVersion", "valid", "hasProfile", "provisional", "requiresCalibration", "gainBiasDb", "protectionDb",
                       "liveToneOffsetDb", "liveTame", "liveHeatReduction", "liveGlueReduction" });
            }

            return names;
        }

        // v0.1.6.2 存档各节点下的子节点（其余一律是新版本才有的）。
        juce::StringArray legacyChildren (const juce::String& tag)
        {
            if (tag == glaze::stateTag)
                return { glaze::profileTag, "PLAN", "CALIBRATION", glaze::constantsTag, "GLAZE_CONTROLLER", glaze::executionTag };

            if (tag == glaze::executionTag)
                return { "REFERENCE", glaze::profileTag, glaze::constantsTag, "PLAN" };

            return {};
        }

        // 把新版本写出的存档改造成 v0.1.6.2 的样子：每个节点只留旧字段表里的属性、formatVersion 一律写回 1、删掉旧版本没有的子节点。
        void convertToLegacy (juce::XmlElement& element)
        {
            const auto allowed = legacyAttributes (element.getTagName());

            for (int i = element.getNumAttributes(); --i >= 0;)
            {
                const auto attribute = element.getAttributeName (i);

                if (! allowed.contains (attribute))
                    element.removeAttribute (attribute);
            }

            if (element.hasAttribute ("formatVersion"))
                element.setAttribute ("formatVersion", 1);

            const auto children = legacyChildren (element.getTagName());

            for (auto* child = element.getFirstChildElement(); child != nullptr;)
            {
                auto* next = child->getNextElement();

                if (children.contains (child->getTagName()))
                    convertToLegacy (*child);
                else
                    element.removeChildElement (child, true);

                child = next;
            }
        }

        // 构造出的旧存档里，旧字段表的属性一个不缺（缺了说明新版本改了旧字段的名字，旧工程读不回来）。返回缺的个数。
        int missingLegacyAttributes (const juce::XmlElement& element)
        {
            int missing = 0;

            for (const auto& attributeName : legacyAttributes (element.getTagName()))
                missing += element.hasAttribute (attributeName) ? 0 : 1;

            for (auto* child = element.getFirstChildElement(); child != nullptr; child = child->getNextElement())
                missing += missingLegacyAttributes (*child);

            return missing;
        }

        // 画像降成格式 1：新字段全部回到「读格式 1 时」的样子（valid = false、数组 0，GlazeTypes.h MusicProfile 注释）。
        glaze::MusicProfile toFormatOne (glaze::MusicProfile profile)
        {
            profile.subShareDb = glaze::Feature {};
            profile.lowSideToMidDb = glaze::Feature {};
            profile.lowCorrelation = glaze::Feature {};
            profile.highSideToMidDb = glaze::Feature {};
            profile.highCorrelation = glaze::Feature {};
            profile.bandSideToMidDb.fill (0.0f);
            profile.bandCorrelation.fill (0.0f);
            profile.bandStereoConfidence.fill (0.0f);
            profile.formatVersion = 1;
            return profile;
        }

        // 由本版本存下的状态造一份「v0.1.6.2 存的」状态：画像降成格式 1（画像号按格式 1 重算，校准参考条件跟着改），方案与标定快照
        // 标算法 6、Clay 自动值中性；执行快照同样降级。最后按旧字段表只留旧属性、formatVersion 写回 1。
        // keepCalibration = false：校准记为无效、不带执行快照——对照组用：两条链都没有依赖「手动参数哈希」的已存状态
        //（A6：Clay 三态并入 manualParamsHash，默认设置与 v0.1.6.2 逐位相同，「全关」不同，已存校准与执行快照对「全关」不适用）。
        std::unique_ptr<juce::XmlElement> makeLegacyState (const juce::XmlElement& modern, bool keepCalibration,
                                                           glaze::MusicProfile& profileOut)
        {
            glaze::CommittedState state;

            if (! glaze::fromXml (modern, state) || ! state.hasProfile)
                return nullptr;

            const auto profile = toFormatOne (state.profile);
            const auto profileId = glaze::computeProfileId (profile);
            state.profile = profile;
            state.profileId = profileId;
            state.plan.algorithmVersion = kLegacyAlgorithmVersion;
            state.plan.clay = glaze::ClayPlan {};
            state.constants.algorithmVersion = kLegacyAlgorithmVersion;
            state.calibration.reference.profileId = profileId;

            if (! keepCalibration)
                state.calibration = glaze::Calibration {};

            auto xml = glaze::toXml (state);

            if (const auto* controller = modern.getChildByName ("GLAZE_CONTROLLER"))
                xml->addChildElement (new juce::XmlElement (*controller));

            if (keepCalibration)
            {
                if (const auto* executionXml = modern.getChildByName (glaze::executionTag))
                {
                    glaze::ExecutionState execution;

                    if (glaze::fromXml (*executionXml, execution) && execution.valid)
                    {
                        execution.hasProfile = true;
                        execution.provisional = false;
                        execution.requiresCalibration = false;
                        execution.protectionDb = 0.0f;
                        execution.profile = profile;
                        execution.constants = state.constants;
                        execution.plan = state.plan;
                        execution.reference = state.calibration.reference;
                        execution.liveClay = glaze::ClayPlan {};
                        execution.clayGuard = glaze::ClayGuardState {};
                        xml->addChildElement (glaze::toXml (execution).release());
                    }
                }
            }

            convertToLegacy (*xml);
            profileOut = profile;
            return xml;
        }
    }

    //==============================================================================
    class ClayGlazeTests  : public juce::UnitTest
    {
    public:
        ClayGlazeTests() : juce::UnitTest ("ClayGlaze", "Celadon") {}

        void runTest() override
        {
            testGlazeZeroIsBitExact();
            testAutomaticClayOnStereoMusic();
            testLegacyProject();
            testPhaseProtection();
            testManualLowCutAtGlazeZero();
            testManualWidthAtNormalLevel();
            testBlockSizeAndOfflineRender();
            testExecutionSnapshot();
            testAudioPathDoesNotAllocate();

            // ---- T-012 阶段 4：设计规格-Clay-根治修订 第 4 节第 2 条的合成回归 ----
            testClippedKickVetoesLowCut();
            testWideBassNeverTriggersPhaseGuard();
            testWholeLowBandInversion();
            testInfrasoundLowCutHelps();
            testOffsetBassBlendLossCap();

            // ---- T-042 / BUG-009：只有 pad 的 breakdown ----
            testBreakdownPadDoesNotLightPhaseWarning();
        }

    private:
        //==============================================================================
        // 1. GLAZE 0 逐位不变（验收第 1 条；根治修订第 2 节）：同一段立体声素材（有约 10 Hz 次声、宽低频、窄高频——Clay 一旦动手就会
        //    改变输出）、同一组「各模块都在工作」的手动值（Balance 30 / 30 / Brighten 10、Tone +1、Glue 25、Heat 20、Limit Gain +6、
        //    Clip 20）、GLAZE 0：
        //      a. 抽屉默认（三态都「自动」）；b. 三项全「关」；c. 三态默认 + Clay Bypass；
        //      d. 「改动前」的链：自己按 Balance → Color → Limit 串三个模块、交同样的手动值（PreClayChain），并在 Clay 的位置
        //         （Balance 与 Color 之间）插一段 D 样本 float 纯延迟——三项为 0 时 ClayModule 就是这样一段纯延迟（真实契约）。
        //    a、b、c 整段输出逐位相同（按字节比，三者延迟相同），且与 d 逐位相同；整链延迟 = 三个模块的延迟 + D。
        //    字面口径「整链输出与改动前的输出平移 D 个样本后逐位相同」另用一对核对：Balance 照样在工作、Color / Limit 中性、素材压低
        //    12 dB（限幅器不动作），链的输出 = 原样三模块链的输出平移 D。Color / Limit 在工作时字面平移不成立：Glue 与限幅器的
        //    自适应释放按从 reset 起每 32 样本的网格重算，D = 1102（574 + 528）不是 32 的倍数（P1 报告第 3 节第 1 条，听感无差别）。
        //    a、b、c 每一块 effective.clay 中性（低切 0、收单 0、宽度 0），c 带 bypass。
        //    阳性对照 e：同样 GLAZE 0，只把低切改成「手动 30 Hz」——手动态在 GLAZE 0 也生效（ADR-009 三态），输出必须不同
        //   （约 10 Hz 次声被 4 阶 30 Hz 高通削约 38 dB，样本差远大于 1e-3）。
        //    跑 6 s：聆听 2 ～ 3 s 就提交（GLAZE 0 照常学习，时间线第 1 节）——画像已有立体声特征，中性不是「没学到」的结果。
        // 依据：设计规格-Clay 第 4 节「逐位直通」、第 5 节三态表「自动：GLAZE 为 0 时为 0（不处理）」；分析决策与验收第 6 节第 1 条；
        //    设计规格-Clay-根治修订 第 2 节「逐位」「GLAZE 0 且抽屉默认」、第 4 节第 4 条「整链上报 = 旧值 + D」。
        void testGlazeZeroIsBitExact()
        {
            beginTest ("GLAZE 0: default drawer, all Off and Clay Bypass render bit-identically = the pre-Clay chain with a D-sample delay in Clay's slot");

            const auto material = makeClayMusic (8.0, 101, stereoTestRecipe());
            const auto clayDelay = clayDelaySamples (kRate);

            auto busy = glazeParams (0.0f);
            busy.balance.recover = 30.0f;
            busy.balance.tame = 30.0f;
            busy.balance.brighten = 10.0f;
            busy.color.toneDb = 1.0f;
            busy.color.glue = 25.0f;
            busy.color.heat = 20.0f;
            busy.limit.gainDb = 6.0f;
            busy.limit.clip = 20.0f;

            auto offParams = busy;
            offParams.claySettings = allOff();
            auto bypassParams = busy;
            bypassParams.claySettings.bypass = true;
            auto manualParams = busy;
            manualParams.claySettings.lowCutMode = ClayMode::manual;
            manualParams.claySettings.lowCutHz = 30.0f;

            // 字面「平移 D」：Color / Limit 中性（只有 Balance 在工作、素材压低 12 dB 让限幅器不动作）——Glue 与限幅器的自适应释放
            // 在从 reset 起每 32 样本的固定网格上按信号重算（ColorModule / LimiterModule），D = 1102 ≡ 14 (mod 32)，一旦它们在工作，
            // 输入整体晚 D 个样本后网格与信号的相对位置就变了（P1 报告第 3 节第 1 条）；听感一样，但逐位平移只在它们不动作时成立。
            auto quiet = material;
            scale (quiet, 0.25f);
            auto neutralParams = glazeParams (0.0f);
            neutralParams.balance = busy.balance;

            ClayRig automatic (fastConstants(), material, busy);
            ClayRig allOffRig (fastConstants(), material, offParams);
            ClayRig bypassRig (fastConstants(), material, bypassParams);
            ClayRig manualRig (fastConstants(), material, manualParams);
            ClayRig neutralRig (fastConstants(), quiet, neutralParams);
            PreClayChain reference (busy, clayDelay);          // 改动前的链 + Clay 位置的 D 样本纯延迟
            PreClayChain neutralReference (neutralParams);     // 改动前的链（原样）

            const auto blocks = automatic.blocksFor (6.0);   // 6 s < 8 s 素材：各条链都从素材开头顺序取样、不回绕
            auto outAuto = makeSilence (kChannels, blocks * kBlock);
            auto outOff = outAuto, outBypass = outAuto, outManual = outAuto, outReference = outAuto;
            auto outNeutral = outAuto, outNeutralReference = outAuto;
            juce::AudioBuffer<float> referenceBlock (kChannels, kBlock), neutralBlock (kChannels, kBlock);
            bool neutral = true, bypassFlagged = true, manualApplied = true;

            for (int b = 0; b < blocks; ++b)
            {
                for (int ch = 0; ch < kChannels; ++ch)
                {
                    const auto offset = static_cast<size_t> (b) * kBlock;
                    referenceBlock.copyFrom (ch, 0, material[static_cast<size_t> (ch)].data() + offset, kBlock);
                    neutralBlock.copyFrom (ch, 0, quiet[static_cast<size_t> (ch)].data() + offset, kBlock);
                }

                reference.process (referenceBlock);
                neutralReference.process (neutralBlock);
                automatic.step();
                allOffRig.step();
                bypassRig.step();
                manualRig.step();
                neutralRig.step();

                storeBlock (outReference, referenceBlock, b);
                storeBlock (outNeutralReference, neutralBlock, b);
                storeBlock (outAuto, automatic.buffer, b);
                storeBlock (outOff, allOffRig.buffer, b);
                storeBlock (outBypass, bypassRig.buffer, b);
                storeBlock (outManual, manualRig.buffer, b);
                storeBlock (outNeutral, neutralRig.buffer, b);

                neutral = neutral && isNeutral (automatic.clay()) && ! automatic.clay().bypass
                                  && isNeutral (allOffRig.clay()) && isNeutral (bypassRig.clay()) && isNeutral (neutralRig.clay());
                bypassFlagged = bypassFlagged && bypassRig.clay().bypass;
                manualApplied = manualApplied && manualRig.clay().lowCutHz == 30.0f;
            }

            const auto total = blocks * kBlock;
            const auto preClayLatency = reference.balance.getLatencySamples() + reference.color.getLatencySamples()
                                      + reference.limiter.getLatencySamples();
            logMessage ("  Clay delay D = " + juce::String (clayDelay) + " samples; chain latency " + juce::String (automatic.chain.getLatencySamples())
                        + " = pre-Clay " + juce::String (preClayLatency) + " + D");
            expectEquals (clayDelay, 1102, "Clay delay at 48 kHz (spec: D 574 + peak-guard lookahead 528 = 1102 samples)");
            expectEquals (automatic.chain.getLatencySamples(), preClayLatency + clayDelay, "chain latency = old latency + D");
            expect (allFinite (outAuto), "finite output");
            expect (neutral, "GLAZE 0: effective.clay is neutral on every block (default drawer, all Off, Bypass)");
            expect (bypassFlagged, "Clay Bypass reaches the module");
            expect (sameSignal (outAuto, outReference),
                    "GLAZE 0 + default drawer == the pre-Clay chain with a D-sample pure delay in Clay's slot, bit for bit; max difference "
                    + juce::String (maxAbsDifference (outAuto, outReference, 0, total), 9));
            expect (sameShifted (outNeutral, outNeutralReference, clayDelay),
                    "Color / Limit idle: the output is the pre-Clay output shifted by D, bit for bit; max difference "
                    + juce::String (maxAbsDifference (outNeutralReference, outNeutral, clayDelay, total - clayDelay), 9));
            expect (! sameShifted (outNeutral, outNeutralReference, 0), "positive control: without the shift they differ (the delay is real)");
            expect (sameSignal (outAuto, outOff), "all Off == default drawer, bit for bit");
            expect (sameSignal (outAuto, outBypass), "Clay Bypass == default drawer, bit for bit");

            const auto controlDifference = maxAbsDifference (outAuto, outManual, 0, total);
            logMessage ("  positive control (manual 30 Hz low cut at GLAZE 0): max sample difference " + juce::String (controlDifference, 5));
            expect (manualApplied && controlDifference > 1.0e-3f,
                    "positive control: the manual low cut acts at GLAZE 0 and changes the output; difference " + juce::String (controlDifference, 6));

            const auto s = automatic.status();
            logMessage ("  default drawer at GLAZE 0: " + statusText (s));
            expect (s.profileReady, "fixture: GLAZE 0 still learns the stereo profile (neutral is not for lack of a profile)");
            expect (s.clayLowCutHz == 0.0f && s.clayMonoAmount == 0.0f && s.clayWidthDb == 0.0f
                    && ! s.clayPhaseGuardActive && ! s.clayWidthGuardActive,
                    "Status: every Clay value neutral at GLAZE 0 with the default drawer");
        }

        //==============================================================================
        // 2. GLAZE 60 自动修坯（验收第 4 条的整链部分 + 第 7 条真峰值）：stereoTestRecipe 的素材按根治修订 3.3 低切、加宽都「值得做」
        //    （见其注释：需要度 > 0、试过的代价都 ≤ 0.2 dB），聆听 12 s（立体声特征观测充分、代价有效块满 10 s）、学完并校准锁定后：
        //      - 画像的五个代价都有效且 ≤ clayCostFullDb（0.2 dB）；Status 不报代价受限（mask 0）；
        //      - Status 低切、加宽 > 0，且落在规格范围：10 Hz < 低切 ≤ 风格低切目标的最大值（lowCutHz = 10·(f_t/10)^k，0 < k ≤ 1）、
        //        0 < 加宽 ≤ clayWidthMaxDb（自动只加不减、≤ +1.5 dB）；收单 = 0、分频点是中性默认值（ADR-015：自动不收单）；
        //        不提示重新聆听（格式 2 画像）、两种保护都不介入（慢路 ρ 最低约 0.44，在快路门 0.3 之外；高频很窄，预测 ρ' 远高于 0.25）；
        //        Status 的 clay* 与同一块交给 ClayModule 的实际值逐位相同（GlazeTypes.h Status「上一块交给 ClayModule 的实际值」）。
        //      - 同一素材、同一 GLAZE、三项全「关」：Status 与实际值三项都是 0。
        //      - 输出 40 ～ 125 Hz 的左右相关度（分析器 lowCorrelation 的同一频段）与全关时相差 < 0.02：自动不收单，低频的立体声不被改动
        //        （ADR-015；原来自动收单会把它抬高约 0.12，那正是立体声听众丢掉的侧声道低音）。
        //      - 两条链全程（聆听、过渡、校准、稳态）OUT 表真峰值 ≤ Ceiling + 0.01 dB（插件自己的 8× 表口径，ADR-004）。
        // 依据：分析决策与验收第 2 节决策公式、第 6 节第 4、7 条；设计规格-Clay 第 5 节三态表「关：永远 0」；根治修订 3.2 / 3.3。
        void testAutomaticClayOnStereoMusic()
        {
            beginTest ("GLAZE 60 on stereo music with ~10 Hz infrasound, wide bass and narrow highs: low cut and width engaged, no automatic mono; all Off stays at zero");

            const auto constants = confidentConstants();
            const auto material = makeClayMusic (8.0, 202, stereoTestRecipe());
            auto offParams = glazeParams (60.0f);
            offParams.claySettings = allOff();

            ClayRig automatic (constants, material, glazeParams (60.0f));
            ClayRig allOffRig (constants, material, offParams);
            expect (automatic.learnAndCalibrate (60.0), "automatic drawer: learns, calibrates and locks");
            expect (allOffRig.learnAndCalibrate (60.0), "all Off: learns, calibrates and locks");

            {
                const auto profile = automatic.committed().profile;
                logMessage ("  profile: " + featureText ("subShareDb", profile.subShareDb) + ", "
                            + featureText ("lowSideToMidDb", profile.lowSideToMidDb) + ", "
                            + featureText ("lowCorrelation", profile.lowCorrelation) + ", "
                            + featureText ("highSideToMidDb", profile.highSideToMidDb) + ", "
                            + featureText ("highCorrelation", profile.highCorrelation) + ", format " + juce::String (profile.formatVersion));
                logMessage ("  costs: " + costText (profile));
                expect (allCostsValid (profile), "the try-before-use costs are valid after 12 s of listening");
                expect (maxCost (profile) <= constants.clayCostFullDb,
                        "fixture: every tried processing is harmless on this material (cost <= 0.2 dB); max " + juce::String (maxCost (profile), 3));
            }

            const auto blocks = automatic.blocksFor (4.0);   // 4 s = 两个完整的贝斯音型（4 个音 × 0.5 s）
            const auto outAuto = automatic.capture (blocks);
            const auto outOff = allOffRig.capture (blocks);

            const auto s = automatic.status();
            const auto& executed = automatic.clay();
            const auto maxTargetHz = juce::jmax (constants.warm.clayLowCutTargetHz, constants.punchy.clayLowCutTargetHz,
                                                 constants.clean.clayLowCutTargetHz);
            logMessage ("  automatic: " + statusText (s) + " | handed to ClayModule: " + clayText (executed));

            expect (s.clayLowCutHz > 0.0f && s.clayWidthDb > 0.0f, "after learning, automatic low cut and width are engaged");
            expect (s.clayMonoAmount == 0.0f && s.clayMonoHz == glaze::ClayPlan {}.monoHz, "ADR-015: no automatic mono, neutral crossover");
            expect (s.clayLowCutHz > ClayModule::minLowCutHz && s.clayLowCutHz <= maxTargetHz + 1.0e-3f,
                    "automatic low cut within (10 Hz, style target]: " + juce::String (s.clayLowCutHz, 3));
            expect (s.clayWidthDb <= constants.clayWidthMaxDb + 1.0e-4f, "automatic width only widens and stays <= +1.5 dB");
            expect (! s.clayNeedsRelearn && ! s.clayPhaseGuardActive && ! s.clayWidthGuardActive,
                    "format-2 profile: no relearn hint; correlated bass and narrow highs trip neither protection");
            expectEquals (static_cast<int> (s.clayCostLimitedMask), 0, "nothing was reduced or vetoed by its cost");
            expect (sameBits (s.clayLowCutHz, executed.lowCutHz) && sameBits (s.clayMonoHz, executed.monoHz)
                    && sameBits (s.clayMonoAmount, executed.monoAmount) && sameBits (s.clayWidthDb, executed.widthDb),
                    "Status clay* == the values handed to ClayModule in the same block");

            const auto so = allOffRig.status();
            logMessage ("  all Off: " + statusText (so));
            expect (so.clayLowCutHz == 0.0f && so.clayMonoAmount == 0.0f && so.clayWidthDb == 0.0f && isNeutral (allOffRig.clay()),
                    "all Off: every Clay value is 0 on the same music and GLAZE");

            const auto skip = static_cast<int> (0.5 * kRate);
            const auto rhoAuto = bandCorrelation (outAuto, 40.0, 125.0, skip);
            const auto rhoOff = bandCorrelation (outOff, 40.0, 125.0, skip);
            const auto rhoInput = bandCorrelation (material, 40.0, 125.0, skip);
            logMessage ("  40-125 Hz correlation: input " + juce::String (rhoInput, 3) + ", all Off " + juce::String (rhoOff, 3)
                        + ", automatic " + juce::String (rhoAuto, 3));
            expect (std::abs (rhoAuto - rhoOff) < 0.02, "ADR-015: the automatic Clay leaves the low-band stereo as it is: "
                                                        + juce::String (rhoAuto, 3) + " vs " + juce::String (rhoOff, 3));

            // T-032（R4-01，2026-09-29）：自动加宽确实到了整链输出上（端到端的通路检查）。量 4 kHz 以上的 S/M（dB）：Balance 的 EQ 与
            // Glue / 限幅器的联动增益对 S 与 M 一视同仁、S/M 不受它们影响，Heat 的非线性很轻；高架 16 kHz = widthDb、2 kHz = widthDb / 2，
            // 4 kHz 以上的踩镲噪声接近平台——自动链比全关高 ≥ 0.6 × 交给模块的 widthDb。
            // 注意它不是 R4-01 的阳性对照：这份素材的高频很窄（S/M 约 −25 dB），加宽几乎不改变 L / R 的峰值，守护回收加宽的旧版本在这里
            // 也只少一点（e6b445b 实测 +0.435 dB / widthDb 0.454 dB，同样过这条）。那条缺陷的阳性对照在用例 5b（整链、手动 +3 dB、
            // 宽粉噪）与 ClayTests 的 testWidthIsNotRecoveredByPeakGuard（模块：旧版本 +3 dB 只剩 +0.316 dB）。
            {
                const auto highAuto = highSideToMidDb (outAuto, skip);
                const auto highOff = highSideToMidDb (outOff, skip);
                const auto widthHanded = static_cast<double> (executed.widthDb);
                logMessage ("  side / mid above 4 kHz: all Off " + juce::String (highOff, 3) + " dB, automatic " + juce::String (highAuto, 3)
                            + " dB (+" + juce::String (highAuto - highOff, 3) + " dB for a width of " + juce::String (widthHanded, 3) + " dB)");
                expect (highAuto - highOff >= 0.6 * widthHanded,
                        "T-032: the automatic width really reaches the output (side / mid above 4 kHz up by at least 0.6 x widthDb): +"
                            + juce::String (highAuto - highOff, 3) + " dB for " + juce::String (widthHanded, 3) + " dB");
            }

            const auto peakAuto = automatic.chain.getAndResetOutputTruePeakDb();
            const auto peakOff = allOffRig.chain.getAndResetOutputTruePeakDb();
            logMessage ("  OUT true peak over the whole run: automatic " + juce::String (peakAuto, 3) + " dBTP, all Off "
                        + juce::String (peakOff, 3) + " dBTP (ceiling " + juce::String (kCeilingDb, 1) + ")");
            expect (peakAuto <= kCeilingDb + 0.01f, "automatic: OUT true peak <= ceiling + 0.01 dB");
            expect (peakOff <= kCeilingDb + 0.01f, "all Off: OUT true peak <= ceiling + 0.01 dB");
        }

        //==============================================================================
        // 3. 旧工程（验收第 1 条「旧工程未重新聆听」+ 第 6 条「旧快照缺字段按中性读」）：先在本版本学完同一首歌（阳性对照：格式 2
        //    画像下低切、加宽自动值都 > 0；收单自动态不做），再把存档改造成 v0.1.6.2 的样子（makeLegacyState：画像格式 1、
        //    方案 / 标定算法 6、执行快照降级，按旧字段表只留旧属性）。
        //      a. 抽屉默认、GLAZE 60 读入：读成已学习的格式 1 画像、画像号按格式 1；clayNeedsRelearn；Clay 实际值与 Status 中性；
        //         已存校准照旧适用（画像号不变、默认抽屉不改参考条件 → 打开后声音不变）；3 s 里每块中性、一直提示。
        //         拧 GLAZE 到 70（按新算法重算，仍是旧画像）：仍中性、仍提示。点「重新聆听」→ 提交格式 2 画像后低切、加宽自动值都 > 0、
        //         收单 0、不再提示。
        //      b. 同一旧存档、三项全「关」：没有「自动」项 → 不提示重新聆听；实际值中性。
        //      c. 逐位对照：不带校准与执行快照的旧存档（两条链都没有依赖手动参数哈希的已存状态），抽屉默认 vs 全关，锁步 4 s：
        //         输出逐位相同、交给三个老模块的参数逐位相同；阳性对照：同一旧存档 + 手动低切 30 Hz → 输出不同。
        // 依据：设计规格-Clay 第 7 节「旧画像 valid = false → Clay 自动增量为 0 → 打开后声音不变；点『重新聆听』后启用」；
        //    GlazeTypes.h Status::clayNeedsRelearn「有自动态的项，但已提交画像缺立体声 / 超低频特征」；实现计划「computeProfileId
        //    对格式 1 画像结果不变（校准不失效）」。
        void testLegacyProject()
        {
            beginTest ("v0.1.6.2 project (algorithm 6, profile format 1, no Clay fields): neutral Clay, relearn hint, calibration kept, equal to all Off");

            const auto constants = confidentConstants();
            const auto material = makeClayMusic (8.0, 303, stereoTestRecipe());
            const auto params = glazeParams (60.0f);

            ClayRig donor (constants, material, params);
            expect (donor.learnAndCalibrate (60.0), "donor learns and calibrates");
            {
                const auto s = donor.status();
                logMessage ("  donor (format-2 profile): " + statusText (s));
                expect (automaticActive (donor.clay()) && ! s.clayNeedsRelearn,
                        "positive control: with a format-2 profile of the same music the automatic Clay (low cut, width) does engage");
            }

            const auto donorXml = donor.chain.createGlazeStateXml();
            glaze::MusicProfile legacyProfile, bareProfile;
            const auto legacy = makeLegacyState (*donorXml, true, legacyProfile);
            const auto legacyBare = makeLegacyState (*donorXml, false, bareProfile);
            expect (legacy != nullptr && legacyBare != nullptr, "fixture: the donor state parses and has a profile");

            if (legacy == nullptr || legacyBare == nullptr)
                return;

            expectEquals (missingLegacyAttributes (*legacy), 0,
                          "fixture: the converted state carries every v0.1.6.2 attribute (the writer kept the old names)");
            {
                const auto* profileXml = legacy->getChildByName (glaze::profileTag);
                expect (profileXml != nullptr && profileXml->getIntAttribute ("formatVersion") == 1, "fixture: profile format 1");
            }

            // ---- a. 抽屉默认读旧存档 ----
            ClayRig restored (constants, material, params);
            restored.chain.restoreGlazeStateXml (legacy.get());
            restored.step();
            {
                const auto s = restored.status();
                const auto c = restored.committed();
                logMessage ("  legacy restore, default drawer: " + statusText (s) + " | " + clayText (restored.clay()));
                expect (s.profileReady && c.hasProfile && c.profile.formatVersion == 1 && glaze::bitwiseEqual (c.profile, legacyProfile),
                        "the old state restores as a learned format-1 profile (new fields invalid, arrays zero)");
                expectEquals (static_cast<juce::int64> (c.profileId), static_cast<juce::int64> (glaze::computeProfileId (legacyProfile)),
                              "profile id as saved (format-1 hash)");
                expect (s.clayNeedsRelearn, "GLAZE > 0 with automatic items on an old profile: Status asks for a relearn");
                expect (isNeutral (restored.clay()), "old profile: automatic Clay handed to the module is neutral");
                expect (s.clayLowCutHz == 0.0f && s.clayMonoAmount == 0.0f && s.clayWidthDb == 0.0f
                        && ! s.clayPhaseGuardActive && ! s.clayWidthGuardActive, "old profile: Status shows neutral Clay, no protection");
                expect (s.calibrationValid, "the saved calibration still applies (format-1 id unchanged, default drawer keeps the reference)");
            }

            bool neutral = true, flagged = true;
            restored.run (3.0, [&] (int)
            {
                neutral = neutral && isNeutral (restored.clay());
                flagged = flagged && restored.status().clayNeedsRelearn;
            });
            expect (neutral && flagged, "3 s of playback: neutral on every block, the relearn hint stays");
            expect (restored.status().calibrationValid, "... and the old calibration keeps applying");

            restored.params.glazePercent = 70.0f;
            neutral = true;
            flagged = true;
            restored.run (1.0, [&] (int)
            {
                neutral = neutral && isNeutral (restored.clay());
                flagged = flagged && restored.status().clayNeedsRelearn;
            });
            expect (neutral && flagged, "turning GLAZE replans on the old profile: still neutral, still asks to relearn");

            restored.chain.requestGlazeRelearn();
            const auto relearned = restored.runUntil (30.0, [&] { return restored.probe().commits >= 1; });
            restored.run (1.0);   // 方案过渡 0.5 s 走完
            {
                const auto s = restored.status();
                logMessage ("  after relearning: " + statusText (s) + ", profile format " + juce::String (restored.committed().profile.formatVersion));
                expect (relearned && restored.committed().profile.formatVersion >= 2, "relearning commits a format-2 profile");
                expect (! s.clayNeedsRelearn && s.clayLowCutHz > 0.0f && s.clayWidthDb > 0.0f && s.clayMonoAmount == 0.0f,
                        "after relearning the automatic Clay engages (low cut, width; no automatic mono) and the hint clears");
            }

            // ---- b. 全关读同一旧存档 ----
            auto offParams = params;
            offParams.claySettings = allOff();
            ClayRig offRig (constants, material, offParams);
            offRig.chain.restoreGlazeStateXml (legacy.get());
            offRig.step();
            expect (offRig.status().profileReady && ! offRig.status().clayNeedsRelearn && isNeutral (offRig.clay()),
                    "all Off on the old state: no automatic item, so no relearn hint; neutral");

            // ---- c. 逐位对照：抽屉默认 vs 全关（不带校准与执行快照的旧存档）----
            auto manualParams = params;
            manualParams.claySettings.lowCutMode = ClayMode::manual;
            manualParams.claySettings.lowCutHz = 30.0f;
            ClayRig bareAuto (constants, material, params), bareOff (constants, material, offParams), bareManual (constants, material, manualParams);

            for (auto* rig : { &bareAuto, &bareOff, &bareManual })
                rig->chain.restoreGlazeStateXml (legacyBare.get());

            bool identical = true, sameControls = true;
            float controlDifference = 0.0f;

            for (int b = 0; b < bareAuto.blocksFor (4.0); ++b)
            {
                bareAuto.step();
                bareOff.step();
                bareManual.step();
                identical = identical && sameBuffers (bareAuto.buffer, bareOff.buffer);
                sameControls = sameControls && sameModules (bareAuto.chain.getEffectiveParams(), bareOff.chain.getEffectiveParams())
                                            && isNeutral (bareAuto.clay());

                for (int ch = 0; ch < kChannels; ++ch)
                    for (int i = 0; i < kBlock; ++i)
                        controlDifference = juce::jmax (controlDifference,
                                                        std::abs (bareAuto.buffer.getSample (ch, i) - bareManual.buffer.getSample (ch, i)));
            }

            logMessage ("  old state, default drawer vs all Off: identical " + juce::String (static_cast<int> (identical))
                        + "; manual 30 Hz control differs by " + juce::String (controlDifference, 5));
            expect (bareAuto.status().profileReady, "fixture: the bare old state restores as learned");
            expect (identical, "old project, default drawer == Clay all Off, bit for bit");
            expect (sameControls, "... with identical controls for Balance / Color / Limit and neutral Clay");
            expect (controlDifference > 1.0e-3f, "positive control: a manual low cut on the same old state changes the output");
        }

        //==============================================================================
        // 4. 反相提示（验收第 5 条；根治修订 3.4 的时序；ADR-015 起只提示、不改收单）：学完正常素材（单声道低频）后，从当前位置
        //    无缝接一段 2 s 的 50 Hz 反相正弦（纯 S，功率 = 正常 40 ～ 125 Hz 的 25 倍），再接 8 s 正常素材。逐块记录：
        //      a. 亮灯：Status.clayPhaseGuardActive 的第一块，结束时刻 ≤ 上限（推导见下）、≥ 资格期 0.15 s（资格期是真的）；
        //         提示强度（探针 phaseFloor）到 1 的第一块 ≤ 上限 + 攻击 0.1 s；
        //      b. 保持：从到 1 起直到「段末 + 2 s − 50 ms」一直 ≥ 0.999 且灯亮（条件在段末之后才消失，保持 2 s 从那时算）；
        //      c. 退回：之后单调不增，每块降幅 ≤ 块长 / 2 s，在段后 7 s 内回到 0、灯灭；
        //      d. ADR-015：每一块 Status 与交给 ClayModule 的收单比例都等于插入前的方案值（自动不收单 = 0）——提示从不改声音。
        //    亮灯上限（整链从音频起算，量到 Status 亮灯的那一块结束）= 资格期 clayGuardPhaseMinSeconds（0.15 s）
        //      + 一个分析帧（Balance 分析窗 2048 样本 = 42.7 ms：反相成分要占满窗才显出负的交叉功率）+ 一个 hop（512 样本）
        //      + 帧在 hop 后第 64 样本发布（BalanceModule.h「分析拆片」）+ 反相段 5 ms 淡入斜坡
        //      + 实时视图慢路 300 ms 功率平滑把 ρ 拉到阈值以下的时间：一阶平滑下 u = 1 − e^(−t/τ)，ρ̃ = (P_n − P_a·u) / (P_n + P_a·u)
        //        < θ（−0.2）⟺ u > (1 − θ) / (r·(1 + θ))（r = P_a / P_n）；只用慢路这一支就够（快路门内那一支只会更早），
        //        r 按一半（12.5 倍，照顾 1/3 倍频程段边界与测量滤波器的出入）从严折算 ≈ 38 ms
        //      + 块粒度 4 块（帧在下一块开头才交给分析器、保护每块更新一次、资格期按整块推进、状态在块末发布）。
        //    段后退回的时限 7 s = 平滑回到阈值以上（反相段比正常段响 25 倍，一阶约 0.85 s，放宽到 3 s）+ 保持 2 s + 退回 2 s。
        //    「关」「手动」态（同一流程、段后 2.5 s）：关 → 每块实际收单与 Status 恒为 0；三项全手动（低切 30 Hz、收单 150 Hz、
        //    宽度 −1.5 dB）→ 每块实际值恒为手动值（收单比例 1）。阳性对照 = 本用例的自动态确实亮灯。
        //    画像失配关掉：反相段把低频功率抬高约 14 dB，失配判定会改 Limit 的上限；这里只看 Clay。
        // 依据：分析决策与验收第 3 节保护表（保持 2 s、2 s 退回）、第 6 节第 5 条；ADR-015「反相只提示」；设计规格-Clay 第 5 节三态表。
        void testPhaseProtection()
        {
            beginTest ("anti-phase bass after learning: the warning lights within qualification + analysis lag, holds 2 s, fades over 2 s; mono never changes");

            auto constants = fastConstants();
            constants.mismatchThresholdLu = constants.mismatchReleaseLu = 1.0e9f;
            const auto fixture = makeAntiPhaseFixture();
            const auto segmentSamples = static_cast<int> (2.0 * kRate);
            const auto segmentSeconds = static_cast<double> (segmentSamples) / kRate;
            const auto blockSeconds = static_cast<double> (kBlock) / kRate;
            logMessage ("  normal 40-125 Hz power " + juce::String (fixture.normalLowPower, 6) + ", anti-phase amplitude "
                        + juce::String (fixture.antiAmplitude, 4) + " (power x" + juce::String (kAntiPowerRatio, 0) + ")");

            ClayRig rig (constants, fixture.normal, glazeParams (60.0f));
            expect (rig.learnAndCalibrate (40.0), "learns the normal (mono-bass) material");
            rig.run (1.0);
            const auto before = rig.status();
            logMessage ("  before the section: " + statusText (before));
            expect (before.clayMonoAmount == 0.0f && ! before.clayPhaseGuardActive,
                    "fixture: ADR-015, the automatic plan does not mono; no warning on the normal material");

            const auto tailSeconds = 8.0;
            const auto continuationSamples = segmentSamples + static_cast<int> (tailSeconds * kRate);
            rig.source = makeAntiPhaseContinuation (rig.source, rig.sourceIndex, continuationSamples, 0, segmentSamples,
                                                    fixture.antiAmplitude, kAntiHz);
            rig.sourceIndex = 0;

            const auto totalBlocks = continuationSamples / kBlock;   // 不回绕到续段开头（那里又是反相段）
            const auto holdEndSeconds = segmentSeconds + constants.clayGuardPhaseHoldSeconds - 0.05;
            int firstLit = -1, firstAtOne = -1, returnedAt = -1;
            bool heldThroughHold = true, activeThroughHold = true, monotoneRelease = true, monoUntouched = true;
            float largestReleaseStep = 0.0f, previous = 1.0f;

            for (int b = 0; b < totalBlocks; ++b)
            {
                rig.step();
                const auto s = rig.status();
                const auto level = rig.probe().clayGuard.phaseFloor;
                const auto endSeconds = static_cast<double> (b + 1) * blockSeconds;
                monoUntouched = monoUntouched && s.clayMonoAmount == before.clayMonoAmount && rig.clay().monoAmount == 0.0f;

                if (firstLit < 0 && s.clayPhaseGuardActive)
                    firstLit = b;

                if (firstAtOne < 0)
                {
                    if (level >= kAtOne && s.clayPhaseGuardActive)
                    {
                        firstAtOne = b;
                        previous = level;
                    }

                    continue;
                }

                if (endSeconds <= holdEndSeconds)
                {
                    heldThroughHold = heldThroughHold && level >= kAtOne;
                    activeThroughHold = activeThroughHold && s.clayPhaseGuardActive;
                }
                else
                {
                    const auto drop = previous - level;
                    monotoneRelease = monotoneRelease && drop >= -1.0e-6f;
                    largestReleaseStep = juce::jmax (largestReleaseStep, drop);

                    if (returnedAt < 0 && level <= 1.0e-4f && ! s.clayPhaseGuardActive)
                        returnedAt = b;
                }

                previous = level;
            }

            const auto effectiveRatio = 0.5 * kAntiPowerRatio;
            const auto threshold = static_cast<double> (constants.clayGuardPhaseCorrelation);
            const auto crossingSeconds = -kLiveSlowSeconds * std::log (1.0 - (1.0 - threshold) / (effectiveRatio * (1.0 + threshold)));
            const auto litBound = constants.clayGuardPhaseMinSeconds + kAnalysisSeconds + kRampSeconds + crossingSeconds + 4.0 * blockSeconds;
            const auto fullBound = litBound + constants.clayGuardPhaseAttackSeconds;
            const auto litLatency = firstLit >= 0 ? static_cast<double> (firstLit + 1) * blockSeconds : -1.0;
            const auto fullLatency = firstAtOne >= 0 ? static_cast<double> (firstAtOne + 1) * blockSeconds : -1.0;
            const auto returnSeconds = returnedAt >= 0 ? static_cast<double> (returnedAt + 1) * blockSeconds : -1.0;
            const auto releaseStepBound = static_cast<float> (blockSeconds / constants.clayGuardPhaseReleaseSeconds) + 1.0e-4f;
            const auto after = rig.status();

            logMessage ("  warning lit after " + juce::String (litLatency * 1000.0, 1) + " ms (bound " + juce::String (litBound * 1000.0, 1)
                        + " ms), full level after " + juce::String (fullLatency * 1000.0, 1) + " ms (bound " + juce::String (fullBound * 1000.0, 1)
                        + " ms); off again at " + juce::String (returnSeconds, 2) + " s after the section start (section 2 s, bound "
                        + juce::String (segmentSeconds + 7.0, 1) + " s); largest release step " + juce::String (largestReleaseStep, 5)
                        + " per block (bound " + juce::String (releaseStepBound, 5) + ")");

            expect (firstLit >= 0, "the phase warning lights on anti-phase bass");
            expect (litLatency > 0.0 && litLatency <= litBound,
                    "the warning lights within qualification + analysis lag: " + juce::String (litLatency * 1000.0, 1) + " ms");
            expect (litLatency >= constants.clayGuardPhaseMinSeconds, "and not before the 0.15 s qualification could have run (the qualification is real)");
            expect (fullLatency > 0.0 && fullLatency <= fullBound, "the warning level reaches 1 within the bound + 0.1 s attack");
            expect (heldThroughHold && activeThroughHold, "held at 1 and lit until 2 s after the section ends");
            expect (monotoneRelease && largestReleaseStep <= releaseStepBound, "then fades monotonically, no faster than linear over 2 s");
            expect (returnedAt >= 0 && returnSeconds <= segmentSeconds + 7.0, "off again within 7 s after the section");
            expect (monoUntouched, "ADR-015: the mono amount (Status and ClayModule) never changes, on every block");
            expect (after.clayMonoAmount == before.clayMonoAmount && ! after.clayPhaseGuardActive, "ends with the plan value and the warning off");

            // 「关」「手动」态：同一流程，保护不改它们。
            for (int mode = 0; mode < 2; ++mode)
            {
                auto p = glazeParams (60.0f);

                if (mode == 0)
                {
                    p.claySettings.monoMode = ClayMode::off;
                }
                else
                {
                    p.claySettings.lowCutMode = ClayMode::manual;
                    p.claySettings.lowCutHz = 30.0f;
                    p.claySettings.monoMode = ClayMode::manual;
                    p.claySettings.monoHz = 150.0f;
                    p.claySettings.widthMode = ClayMode::manual;
                    p.claySettings.widthDb = -1.5f;
                }

                const auto tag = juce::String (mode == 0 ? "mono Off" : "all manual");
                ClayRig other (constants, fixture.normal, p);
                expect (other.learnAndCalibrate (40.0), tag + ": learns the normal material");
                const auto otherSamples = segmentSamples + static_cast<int> (3.0 * kRate);
                other.source = makeAntiPhaseContinuation (other.source, other.sourceIndex, otherSamples, 0, segmentSamples,
                                                          fixture.antiAmplitude, kAntiHz);
                other.sourceIndex = 0;

                bool untouched = true;
                other.run (segmentSeconds + 2.5, [&] (int)
                {
                    const auto& executedClay = other.clay();
                    const auto st = other.status();

                    if (mode == 0)
                        untouched = untouched && executedClay.monoAmount == 0.0f && st.clayMonoAmount == 0.0f;
                    else
                        untouched = untouched && executedClay.lowCutHz == 30.0f && executedClay.monoHz == 150.0f
                                              && executedClay.monoAmount == 1.0f && executedClay.widthDb == -1.5f
                                              && st.clayMonoAmount == 1.0f;
                });

                logMessage ("  " + tag + " at the end: " + clayText (other.clay()));
                expect (untouched, tag + ": the warning never changes it, on every block through the section and after");
            }
        }

        //==============================================================================
        // 5. 手动态在 GLAZE 0 生效（验收第 1、2 条的整链部分）：素材 = 音乐 + 15 Hz 同相正弦，GLAZE 0、各模块中性（Limit Gain 0、
        //    输入真峰值低于 −1 dBTP 的 Ceiling：整链只剩延迟与 Clay）。低切「手动 30 Hz」vs 抽屉默认，各 4 s，跳过前 1 s：
        //      - 每块实际值：lowCutHz 恰为 30（ClayModule 的范围内，不改值）、收单与宽度仍是自动态的 0；Status 同样是 30；
        //      - 输出 20 Hz 以下能量（4 阶 20 Hz 低通）比默认降 ≥ 18 dB：4 阶 Butterworth 30 Hz 高通在 15 Hz 的衰减
        //        = 10·log10(1 + 2⁸) ≈ 24.1 dB，留 6 dB 给音乐自身的 20 Hz 以下成分；
        //      - 阳性对照：默认的 20 Hz 以下能量与输入相差 < 0.1 dB（GLAZE 0 自动态不动它，测量也确实量得到 15 Hz 分量）；
        //      - 200 Hz 以上能量两者相差 < 0.05 dB（30 Hz 4 阶高通在 200 Hz 的衰减 ≈ 1e-6 dB）。
        // 依据：设计规格-Clay 第 5 节三态表「手动：严格等于旋钮值，GLAZE 为 0 时也生效」、第 4 节 ① 低切（4 阶 Butterworth）。
        void testManualLowCutAtGlazeZero()
        {
            beginTest ("manual 30 Hz low cut at GLAZE 0: exactly 30 Hz, the 15 Hz content drops ~24 dB, content above 200 Hz untouched");

            auto recipe = stereoTestRecipe();
            recipe.gain = 0.3f;
            recipe.subHz = 15.0;
            recipe.subAmplitude = 0.3f;
            const auto material = makeClayMusic (8.0, 505, recipe);
            const auto inputPeakDb = toDb (truePeakOf (material));
            expect (inputPeakDb < kCeilingDb - 1.0f, "fixture: input true peak below the ceiling (limiter transparent at Gain 0): "
                                                     + juce::String (inputPeakDb, 2) + " dBTP");

            auto manual = glazeParams (0.0f);
            manual.claySettings.lowCutMode = ClayMode::manual;
            manual.claySettings.lowCutHz = 30.0f;

            ClayRig automatic (fastConstants(), material, glazeParams (0.0f));
            ClayRig cut (fastConstants(), material, manual);

            const auto blocks = automatic.blocksFor (4.0);
            bool exact = true;
            const auto outAuto = automatic.capture (blocks);
            const auto outCut = cut.capture (blocks, [&] (int)
            {
                const auto& executedClay = cut.clay();
                exact = exact && executedClay.lowCutHz == 30.0f && executedClay.monoAmount == 0.0f
                              && executedClay.widthDb == 0.0f && ! executedClay.bypass;
            });

            auto input = makeSilence (kChannels, blocks * kBlock);

            for (size_t ch = 0; ch < input.size(); ++ch)
                std::copy (material[ch].begin(), material[ch].begin() + static_cast<std::ptrdiff_t> (blocks * kBlock), input[ch].begin());

            const auto skip = static_cast<int> (kRate);
            const auto inputLow = bandPower (input, 0.0, 20.0, skip);
            const auto autoLow = bandPower (outAuto, 0.0, 20.0, skip);
            const auto cutLow = bandPower (outCut, 0.0, 20.0, skip);
            const auto autoHigh = bandPower (outAuto, 200.0, 0.0, skip);
            const auto cutHigh = bandPower (outCut, 200.0, 0.0, skip);
            const auto dropDb = powerRatioDb (cutLow, autoLow);
            const auto controlDb = powerRatioDb (autoLow, inputLow);
            const auto highDb = powerRatioDb (cutHigh, autoHigh);

            logMessage ("  below 20 Hz: manual cut vs default " + juce::String (dropDb, 2) + " dB (analytic 15 Hz: -24.1 dB); default vs input "
                        + juce::String (controlDb, 3) + " dB; above 200 Hz: " + juce::String (highDb, 4) + " dB");
            expect (exact, "effective.clay: low cut exactly 30 Hz on every block, automatic mono / width neutral at GLAZE 0");
            expectEquals (cut.status().clayLowCutHz, 30.0f, "Status shows the manual 30 Hz");
            expect (dropDb <= -18.0, "the manual low cut removes the sub-20 Hz content at GLAZE 0");
            expect (std::abs (controlDb) < 0.1, "positive control: the default drawer leaves the sub-20 Hz content as it came in");
            expect (std::abs (highDb) < 0.05, "content above 200 Hz is untouched by the low cut");
        }

        //==============================================================================
        // 5b. T-032（研究 R4-01 / 核验 V4，2026-09-29）：手动加宽在正常电平下真的到了整链输出上——R4-01 在整链层面的阳性对照
        //    （用例 2 的自动加宽素材高频很窄，修订前的守护在那里也只回收一点，分不出新旧）。素材 = 两声道各自独立的粉噪（高频本来就宽，
        //    S/M ≈ 0 dB），峰值归一到 −12 dBFS（远在守护地板 −40 dBFS 之上；真峰值低于 Ceiling，GLAZE 0、各模块中性：整链只剩延迟与
        //    Clay）。加宽「手动 +3 dB」vs 抽屉默认，各 4 s，跳过前 1 s：
        //      - 每块实际值：widthDb 恰为 3、低切与收单仍是自动态的 0；
        //      - 输出 4 kHz 以上的 S/M 比默认高 3 ± 0.3 dB（高架 16 kHz = widthDb、2 kHz = widthDb / 2，4 kHz 以上的粉噪按能量加权约
        //        0.99 widthDb，同 ClayTests 的 testWidthIsNotRecoveredByPeakGuard）；4 kHz 以上的 M 不变（< 0.05 dB）；
        //      - 阳性对照：默认的 S/M 与输入相差 < 0.05 dB（GLAZE 0 自动态不动它，测量也量得到侧声道）。
        //    修订前（e6b445b）峰值守护把加宽当成修坯的改动量回收，同一用例只剩零点几 dB（T-032 A8 报告实测），S/M 那一条不过。
        // 依据：ADR-014 的 T-032 修订（守护只管低切 / 收单，③ 的增量不经守护）；设计规格-Clay 第 5 节三态表「手动：严格等于旋钮值，
        //    GLAZE 为 0 时也生效」。
        void testManualWidthAtNormalLevel()
        {
            beginTest ("manual +3 dB width at GLAZE 0 on -12 dBFS wide noise (T-032): the side above 4 kHz gains the full 3 dB at the chain output");

            const auto numSamples = static_cast<int> (8.0 * kRate);
            auto material = makeSilence (kChannels, numSamples);
            float peak = 0.0f;

            for (int ch = 0; ch < kChannels; ++ch)
            {
                const auto noise = makePinkNoise (numSamples, -18.0, 707 + ch);
                auto& channel = material[static_cast<size_t> (ch)];

                for (size_t i = 0; i < channel.size(); ++i)
                {
                    channel[i] = static_cast<float> (noise[i]);
                    peak = juce::jmax (peak, std::abs (channel[i]));
                }
            }

            scale (material, juce::Decibels::decibelsToGain (-12.0f) / peak);
            const auto inputPeakDb = toDb (truePeakOf (material));
            expect (inputPeakDb < kCeilingDb - 1.0f, "fixture: input true peak below the ceiling (limiter transparent at Gain 0): "
                                                     + juce::String (inputPeakDb, 2) + " dBTP");

            auto manual = glazeParams (0.0f);
            manual.claySettings.widthMode = ClayMode::manual;
            manual.claySettings.widthDb = 3.0f;

            ClayRig automatic (fastConstants(), material, glazeParams (0.0f));
            ClayRig widened (fastConstants(), material, manual);

            const auto blocks = automatic.blocksFor (4.0);
            bool exact = true;
            const auto outAuto = automatic.capture (blocks);
            const auto outWide = widened.capture (blocks, [&] (int)
            {
                const auto& executedClay = widened.clay();
                exact = exact && executedClay.widthDb == 3.0f && executedClay.lowCutHz == 0.0f
                              && executedClay.monoAmount == 0.0f && ! executedClay.bypass;
            });

            auto input = makeSilence (kChannels, blocks * kBlock);

            for (size_t ch = 0; ch < input.size(); ++ch)
                std::copy (material[ch].begin(), material[ch].begin() + static_cast<std::ptrdiff_t> (blocks * kBlock), input[ch].begin());

            const auto skip = static_cast<int> (kRate);
            const auto gainDb = highSideToMidDb (outWide, skip) - highSideToMidDb (outAuto, skip);
            const auto midDb = powerRatioDb (bandPower (midOrSide (outWide, 1.0), 4000.0, 0.0, skip), bandPower (midOrSide (outAuto, 1.0), 4000.0, 0.0, skip));
            const auto controlDb = highSideToMidDb (outAuto, skip) - highSideToMidDb (input, skip);
            logMessage ("  side / mid above 4 kHz: manual +3 dB vs default " + juce::String (gainDb, 3) + " dB (mid above 4 kHz "
                        + juce::String (midDb, 4) + " dB); default vs input " + juce::String (controlDb, 4) + " dB");
            expect (exact, "effective.clay: width exactly +3 dB on every block, automatic low cut / mono neutral at GLAZE 0");
            expectWithinAbsoluteError (gainDb, 3.0, 0.3, "the manual +3 dB width reaches the chain output at normal level (side / mid above 4 kHz)");
            expect (std::abs (midDb) < 0.05, "the mid above 4 kHz is untouched by the width");
            expect (std::abs (controlDb) < 0.05, "positive control: the default drawer leaves the side / mid as it came in");
        }

        //==============================================================================
        // 6. 块长与离线（验收第 6 条 rendercheck 加 Clay 场景）：同一首歌学完并校准锁定（聆听 10 s，低切、加宽自动值都 > 0，
        //    收单用手动 120 Hz，见下），存档；
        //    64 与 1024 样本块 × 实时（在播）与离线（HostInfo.offline、宿主报未播放）四条新链各读这份存档、从同一起点渲染 60 s：
        //      - 同块长的实时与离线：每块输出逐字节相同、每块交给四个模块的实际参数逐位相同（rendercheck「realtime/offline audio
        //        matches exactly」，规格「逐样本一致」）；
        //      - 64 与 1024：20 个 3 s 窗的短期响度逐窗差 < 0.02 LU、整体响度差 < 0.02 LU。口径同 tools/perf/bench.mm 的
        //        changedBlockRender（独立 LoudnessMeter、3 s 边界切开读短期）；那里的门限是窗 0.2 LU / 整体 0.1 LU，实测
        //        0.0137 / 0.0073 / 0 LU（docs/validation/2026-09-26-colour-render/rendercheck-final.log），这里按任务书收紧到 0.02 LU。
        //        自适应控制按块量化是块长之间唯一的出入；Clay 自身的参数这里是常数（方案给定、无保护介入），与块长无关。
        //    前提：读取后第一块 Clay 三项实际值都 > 0（「Clay 生效时」）。ADR-015 起自动不收单：收单改「手动 120 Hz」，线性相位收单
        //    这条路照样在跑（低切、加宽仍是自动）。
        void testBlockSizeAndOfflineRender()
        {
            beginTest ("Clay engaged: 64 / 1024 sample blocks x realtime / offline, 60 s each: identical per block size, 3 s windows within 0.02 LU");

            const auto constants = confidentConstants();
            const auto material = makeClayMusic (8.0, 606, stereoTestRecipe());
            auto params = glazeParams (60.0f);
            params.claySettings.monoMode = ClayMode::manual;   // ADR-015：自动不收单，收单路径用手动覆盖
            params.claySettings.monoHz = 120.0f;

            ClayRig donor (constants, material, params);
            expect (donor.learnAndCalibrate (60.0), "donor learns and calibrates");
            const auto state = donor.chain.createGlazeStateXml();

            const auto render64 = renderRealtimeAndOffline (constants, material, params, *state, 64, 60.0);
            const auto render1024 = renderRealtimeAndOffline (constants, material, params, *state, 1024, 60.0);

            expect (render64.clayActiveOnFirstBlock && render1024.clayActiveOnFirstBlock,
                    "fixture: Clay engaged (automatic low cut and width, manual mono: all > 0) from the first restored block");
            expect (render64.realtimeEqualsOffline && render64.sameControls, "64-sample blocks: realtime and offline render identically");
            expect (render1024.realtimeEqualsOffline && render1024.sameControls, "1024-sample blocks: realtime and offline render identically");
            expectEquals (static_cast<int> (render64.shortTermLufs.size()), 20, "64: twenty 3 s windows in 60 s");
            expectEquals (static_cast<int> (render1024.shortTermLufs.size()), 20, "1024: twenty 3 s windows in 60 s");

            float largestWindowDelta = 0.0f;
            bool readable = true;

            for (size_t i = 0; i < juce::jmin (render64.shortTermLufs.size(), render1024.shortTermLufs.size()); ++i)
            {
                largestWindowDelta = juce::jmax (largestWindowDelta, std::abs (render64.shortTermLufs[i] - render1024.shortTermLufs[i]));
                readable = readable && render64.shortTermLufs[i] > -70.0f && render1024.shortTermLufs[i] > -70.0f;
            }

            const auto integratedDelta = render1024.integratedLufs - render64.integratedLufs;
            logMessage ("  60 s 64-vs-1024: max 3 s window delta " + juce::String (largestWindowDelta, 5) + " LU, integrated delta "
                        + juce::String (integratedDelta, 5) + " LU (" + juce::String (render64.integratedLufs, 2) + " LUFS); last Clay 64: "
                        + clayText (render64.lastClay) + " / 1024: " + clayText (render1024.lastClay));
            expect (readable, "every window has a real reading (the comparison is not between silences)");
            expect (largestWindowDelta < 0.02f, "64 vs 1024: every 3 s window within 0.02 LU");
            expect (std::abs (integratedDelta) < 0.02f, "64 vs 1024: integrated loudness within 0.02 LU");
        }

        //==============================================================================
        // 7. 执行快照（验收第 6 条）：同用例 4 的流程，反相提示满强度（phaseFloor = 1、灯亮）后再过 0.1 s（仍在反相段里）存档：
        //      - 存档的 EXECUTION 带着提示状态：phaseActive、phaseFloor ≥ 0.999；liveClay 的收单仍是方案值 0（ADR-015：提示不改收单）
        //        （规格第 3 节「保护状态进执行快照」、第 7 节「EXECUTION 加存 Clay 四个实际值与保护状态」）；
        //      - 两条新链（实时 / 离线）从存档时的素材位置与播放位置读回：第一块 effective.clay 四项与保存前最后一块相同（容差 1e-5，
        //        同 GlazeControllerTests 首块 Gain / Heat 的口径）、bypass 相同，Status 的灯仍亮、收单仍是 0；
        //      - 续渲到反相段结束后 ≥ 8.5 s：实时与离线每块输出逐字节相同、实际参数逐位相同；读回的提示照样保持、退回，最后灯灭
        //        （退回时限同用例 4）。
        // 依据：设计规格-Clay 第 7 节「执行快照……保证实时 / 离线、保存重开的可复现口径与现有一致」；分析决策与验收第 6 条。
        void testExecutionSnapshot()
        {
            beginTest ("execution snapshot during an active phase warning: first restored block hands ClayModule the same values; replay identical and the warning still fades");

            auto constants = fastConstants();
            constants.mismatchThresholdLu = constants.mismatchReleaseLu = 1.0e9f;
            const auto fixture = makeAntiPhaseFixture();
            const auto params = glazeParams (60.0f);

            ClayRig donor (constants, fixture.normal, params);
            expect (donor.learnAndCalibrate (40.0), "donor learns the normal material");
            donor.run (1.0);
            const auto planValue = donor.status().clayMonoAmount;

            const auto segmentSamples = static_cast<int> (2.0 * kRate);
            const auto continuationSamples = segmentSamples + static_cast<int> (9.0 * kRate);
            donor.source = makeAntiPhaseContinuation (donor.source, donor.sourceIndex, continuationSamples, 0, segmentSamples,
                                                      fixture.antiAmplitude, kAntiHz);
            donor.sourceIndex = 0;

            const auto engaged = donor.runUntil (1.0, [&]
            {
                return donor.probe().clayGuard.phaseFloor >= kAtOne && donor.status().clayPhaseGuardActive;
            });
            expect (engaged, "fixture: the phase warning reaches full level in the donor");
            donor.run (0.1);

            const auto savedClay = donor.clay();
            const auto xml = donor.chain.createGlazeStateXml();
            logMessage ("  saved while warning: " + clayText (savedClay) + " at " + juce::String (static_cast<double> (donor.sourceIndex) / kRate, 3)
                        + " s into the section");

            {
                glaze::ExecutionState execution;
                const auto* executionXml = xml->getChildByName (glaze::executionTag);
                const auto parsed = executionXml != nullptr && glaze::fromXml (*executionXml, execution) && execution.valid;
                expect (parsed, "the save carries a valid execution snapshot");
                expect (parsed && execution.clayGuard.phaseActive && execution.clayGuard.phaseFloor >= kAtOne
                        && execution.liveClay.monoAmount == 0.0f,
                        "the execution snapshot carries the active phase warning; the automatic mono stays 0 (ADR-015)");
            }

            ClayRig realtime (constants, fixture.normal, params), offline (constants, fixture.normal, params);

            for (auto* rig : { &realtime, &offline })
            {
                rig->source = donor.source;
                rig->sourceIndex = donor.sourceIndex;
                rig->position = donor.position;
                rig->chain.restoreGlazeStateXml (xml.get());
            }

            offline.host.offline = true;
            offline.host.isPlaying = false;
            realtime.step();
            offline.step();

            const auto& first = realtime.clay();
            logMessage ("  first restored block: " + clayText (first));
            expectWithinAbsoluteError (first.lowCutHz, savedClay.lowCutHz, 1.0e-5f, "first block: same low cut as before saving");
            expectWithinAbsoluteError (first.monoHz, savedClay.monoHz, 1.0e-5f, "first block: same mono crossover");
            expectWithinAbsoluteError (first.monoAmount, savedClay.monoAmount, 1.0e-5f, "first block: same mono amount");
            expectWithinAbsoluteError (first.widthDb, savedClay.widthDb, 1.0e-5f, "first block: same width");
            expect (first.bypass == savedClay.bypass, "first block: same bypass");
            expect (sameClay (first, offline.clay()), "realtime and offline restores start from the same Clay values");
            expect (realtime.status().clayMonoAmount == 0.0f && realtime.status().clayPhaseGuardActive,
                    "the restored Status still shows the warning, with no mono");

            const auto remainingBlocks = static_cast<int> ((static_cast<size_t> (continuationSamples) - realtime.sourceIndex)
                                                           / static_cast<size_t> (kBlock));
            bool identical = true, sameControls = true;

            for (int b = 0; b < remainingBlocks; ++b)
            {
                realtime.step();
                offline.step();
                identical = identical && sameBuffers (realtime.buffer, offline.buffer);
                sameControls = sameControls && sameExecuted (realtime.chain.getEffectiveParams(), offline.chain.getEffectiveParams());
            }

            const auto finalStatus = realtime.status();
            logMessage ("  after the replay: " + statusText (finalStatus) + " (plan value before the section " + juce::String (planValue, 4) + ")");
            expect (identical, "restored realtime and offline continue sample-identically");
            expect (sameControls, "... with identical controls on every block");
            expect (std::abs (finalStatus.clayMonoAmount - planValue) <= 1.0e-4f && ! finalStatus.clayPhaseGuardActive,
                    "the restored warning still holds and fades out; the mono amount stays at the plan value");
        }

        //==============================================================================
        // 8. 音频线程零分配（设计规格-总体「音频线程零分配、零锁」；验收第 2 条「音频线程 0 分配」的整链部分）：一条链跑 60 s，
        //    setHostInfo / setParams / process / getGlazeStatus 都不经 operator new（计数见 TestMain.cpp）。块长在 512 / 1 / 17 /
        //    256 / 64 / 333 之间轮换（块缓冲在计数区外按最大块建好，setSize 不重新分配），途中：
        //      0 ～ 14 s 三态全「自动」（聆听 10 s 后三项自动值生效），12 ～ 14 s 反相段（反相保护介入）；
        //      14 ～ 46 s 三项各自在「自动 / 关 / 手动」间轮换、手动值扫满范围、Clay Bypass 来回切；
        //      26 s restartWithFadeIn；30 ～ 31 s 宿主旁通；32 ～ 34 s GLAZE 0（「手动」照样生效的那条路）；36 ～ 38 s 离线；
        //      42 s 请求重新聆听；46 s 起回到全「自动」；50 s 读取存档（解析在计数区外，下一块接到它在计数区内）。
        //    最后核对这些路径确实走到了（自动 Clay 非中性、手动、关、Bypass、反相保护都出现过）。
        void testAudioPathDoesNotAllocate()
        {
            beginTest ("setHostInfo / setParams / process / getGlazeStatus never allocate while Clay works: modes, sweeps, bypass, protection, reset, restore");

            const auto constants = confidentConstants();
            const auto material = makeClayMusic (8.0, 808, stereoTestRecipe());
            const auto length = material.front().size();
            const auto lowPower = bandPower (material, 40.0, 125.0, 4096);
            const auto antiPhase = makeAntiPhaseContinuation (material, 0, static_cast<int> (length), 0, static_cast<int> (length),
                                                              static_cast<float> (std::sqrt (2.0 * kAntiPowerRatio * lowPower)), kAntiHz);

            ClayRig donor (constants, material, glazeParams (45.0f));
            donor.run (5.0);
            const auto savedState = donor.chain.createGlazeStateXml();

            CeladonChain chain;
            chain.getGlazeController().setConstantsForTesting (constants);
            auto params = glazeParams (60.0f);
            chain.setParams (params, false);
            chain.prepare ({ kRate, static_cast<juce::uint32> (kBlock), static_cast<juce::uint32> (kChannels) });

            glaze::HostInfo host;
            host.hasPlayState = true;
            host.isPlaying = true;
            host.hasPosition = true;

            constexpr int blockSizes[] = { kBlock, 1, 17, 256, 64, 333 };
            juce::AudioBuffer<float> block (kChannels, kBlock);
            const auto total = static_cast<std::int64_t> (60.0 * kRate);

            std::uint64_t allocations = 0;
            bool finite = true, restartDone = false, relearnDone = false, restoreDone = false;
            int automaticBlocks = 0, manualBlocks = 0, offBlocks = 0, bypassBlocks = 0, protectedBlocks = 0;
            size_t readIndex = 0;
            std::int64_t processed = 0;

            for (int b = 0; processed < total; ++b)
            {
                // ---- 计数区外：这一块的参数、宿主事件、输入（与读取存档的解析）----
                const auto seconds = static_cast<double> (processed) / kRate;
                const auto wave = 0.5f + 0.5f * std::sin (0.013f * static_cast<float> (b));
                const auto cycling = seconds >= 14.0 && seconds < 46.0;
                auto& settings = params.claySettings;
                settings.lowCutMode = cycling ? static_cast<ClayMode> ((b / 97) % 3) : ClayMode::automatic;
                settings.monoMode   = cycling ? static_cast<ClayMode> ((b / 131) % 3) : ClayMode::automatic;
                settings.widthMode  = cycling ? static_cast<ClayMode> ((b / 113) % 3) : ClayMode::automatic;
                settings.lowCutHz = 10.0f + 50.0f * wave;
                settings.monoHz = 40.0f + 260.0f * wave;
                settings.widthDb = -3.0f + 6.0f * wave;
                settings.bypass = cycling && (b / 170) % 2 == 1;
                params.glazePercent = (seconds >= 32.0 && seconds < 34.0) ? 0.0f : 60.0f;
                const auto hostBypassed = seconds >= 30.0 && seconds < 31.0;
                host.offline = seconds >= 36.0 && seconds < 38.0;
                const auto& source = (seconds >= 12.0 && seconds < 14.0) ? antiPhase : material;

                if (! relearnDone && seconds >= 42.0)
                {
                    chain.requestGlazeRelearn();
                    relearnDone = true;
                }

                if (! restoreDone && seconds >= 50.0)
                {
                    chain.restoreGlazeStateXml (savedState.get());
                    restoreDone = true;
                }

                const auto count = blockSizes[static_cast<size_t> (b) % std::size (blockSizes)];
                block.setSize (kChannels, count, false, false, true);

                for (int ch = 0; ch < kChannels; ++ch)
                    for (int i = 0; i < count; ++i)
                        block.setSample (ch, i, source[static_cast<size_t> (ch)][(readIndex + static_cast<size_t> (i)) % length]);

                readIndex = (readIndex + static_cast<size_t> (count)) % length;
                host.positionSamples = processed;

                // ---- 计数区：只夹住音频线程的入口（与界面线程的 getGlazeStatus）----
                const auto before = allocationCountOnThisThread();

                if (! restartDone && seconds >= 26.0)
                {
                    chain.restartWithFadeIn();
                    restartDone = true;
                }

                chain.setHostInfo (host);
                chain.setParams (params, hostBypassed);
                chain.process (block);
                const auto status = chain.getGlazeStatus();

                allocations += allocationCountOnThisThread() - before;

                // ---- 计数区外：记下走过的路径 ----
                const auto& executed = chain.getEffectiveParams().clay;
                const auto allAutomatic = settings.lowCutMode == ClayMode::automatic && settings.monoMode == ClayMode::automatic
                                       && settings.widthMode == ClayMode::automatic;
                automaticBlocks += (allAutomatic && params.glazePercent > 0.0f && ! isNeutral (executed)) ? 1 : 0;
                manualBlocks += (settings.lowCutMode == ClayMode::manual || settings.monoMode == ClayMode::manual
                                 || settings.widthMode == ClayMode::manual) ? 1 : 0;
                offBlocks += (settings.lowCutMode == ClayMode::off || settings.monoMode == ClayMode::off
                              || settings.widthMode == ClayMode::off) ? 1 : 0;
                bypassBlocks += executed.bypass ? 1 : 0;
                protectedBlocks += status.clayPhaseGuardActive ? 1 : 0;

                for (int ch = 0; ch < kChannels; ++ch)
                    for (int i = 0; i < count; ++i)
                        finite = finite && std::isfinite (block.getSample (ch, i));

                processed += count;
            }

            logMessage ("  paths: automatic Clay engaged " + juce::String (automaticBlocks) + " blocks, manual " + juce::String (manualBlocks)
                        + ", off " + juce::String (offBlocks) + ", bypass " + juce::String (bypassBlocks) + ", phase protection "
                        + juce::String (protectedBlocks));
            expect (finite, "output finite throughout");
            expect (automaticBlocks > 0 && manualBlocks > 0 && offBlocks > 0 && bypassBlocks > 0 && protectedBlocks > 0
                    && restartDone && relearnDone && restoreDone,
                    "the run went through the Clay paths it claims to cover");
            expectEquals (static_cast<juce::int64> (allocations), static_cast<juce::int64> (0), "audio thread must not allocate");
        }

        //==============================================================================
        // 9. 根治修订 4(a) 已限幅的底鼓素材（makeClippedKickMusic）。测试服务器 numpy 按规格公式复算（种子 1101 ～ 1103、学习 12 s）：
        //    真次声占比约 −25 dB → need_sub = 1；IIR 20 Hz 低切的代价 +4.6 ～ +4.8 dB（削平的底鼓被低切的相位旋转错开，峰值重新冒出来——
        //    真歌 Ansun 同一机理，+4.4 dB）→ 否决；四个收单候选 −0.2 ～ −0.06 dB（线性相位只削侧声道、不错开峰）。
        //    GLAZE 50、聆听 12 s、学完并校准锁定后：
        //      - 画像：真次声有效且高于 claySubShareLowDb（低切是「需要」的）、低切代价有效且 ≥ clayCostZeroDb（0.6 dB）——否决的是代价；
        //        四个收单代价都有效且 ≤ 0.3 dB（根治修订 4(a)）；
        //      - Status：低切实际值 0，clayCostLimitedMask 含低切位（界面据此写「本曲试过：低切会抬高峰值，已自动跳过」）；
        //      - 与同一素材、同一 GLAZE 的「修坯全关」相比，锁定后 6 s 里整链限幅 GR 最大增加 ≤ 0.3 dB（根治修订第 4 节第 1 条真歌门槛
        //        的同一口径）。
        //    「被否决的确实是会抬高峰值的处理」由画像里的代价（≥ 0.6 dB，候选与 ClayModule 同一实现，审核 V4 核过）给出。
        //    ADR-014 起修坯有峰值守护：GLAZE 0、手动 Limit Gain +2 dB（没有 GLAZE 的自适应，两条链只差这一处），同一素材手动 20 Hz 低切
        //    的 GR 最大也不比全关多 0.3 dB 以上；阳性对照：守护在这段素材上确实在工作（g < 1 的样本 > 0、最小 g < 0.5）——没有它，
        //    这条手动低切会像改动前那样多压 > 1 dB（阶段 4 版本实测）。
        // 依据：设计规格-Clay-根治修订 3.2（代价口径）、3.3（f = ramp(代价, 0.6, 0.2)，≥ 0.6 dB 不做）、4(a)、4(1)；ADR-012。
        void testClippedKickVetoesLowCut()
        {
            beginTest ("clipped kick material: the tried low cut is vetoed by its peak cost; the chain's limiter GR stays within 0.3 dB of Clay off");

            const auto constants = confidentConstants();
            const auto material = makeClippedKickMusic (8.0, 1101);
            auto offParams = glazeParams (50.0f);
            offParams.claySettings = allOff();

            ClayRig automatic (constants, material, glazeParams (50.0f));
            ClayRig allOffRig (constants, material, offParams);
            expect (automatic.learnAndCalibrate (60.0), "automatic: learns, calibrates and locks");
            expect (allOffRig.learnAndCalibrate (60.0), "all Off: learns, calibrates and locks");

            const auto profile = automatic.committed().profile;
            logMessage ("  profile: " + featureText ("subShareDb", profile.subShareDb) + "; costs: " + costText (profile));
            expect (profile.subShareDb.valid && profile.subShareDb.value > constants.claySubShareLowDb,
                    "fixture: the material carries infrasound, so the low cut is needed");
            expect (profile.lowCutPeakCostDb.valid && profile.lowCutPeakCostDb.value >= constants.clayCostZeroDb,
                    "trying the IIR low cut raises the clipped peaks by >= 0.6 dB: " + juce::String (profile.lowCutPeakCostDb.value, 2) + " dB");

            bool monoCostsSmall = true;

            for (const auto& cost : profile.monoPeakCostDb)
                monoCostsSmall = monoCostsSmall && cost.valid && cost.value <= 0.3f;

            expect (monoCostsSmall, "the linear-phase mono candidates cost <= 0.3 dB at all four crossovers");

            const auto s = automatic.status();
            logMessage ("  automatic: " + statusText (s) + ", cost mask " + juce::String (static_cast<int> (s.clayCostLimitedMask)));
            expectEquals (s.clayLowCutHz, 0.0f, "the low cut is vetoed: 0");
            expectEquals (automatic.clay().lowCutHz, 0.0f, "... in the parameters handed to ClayModule too");
            expect ((s.clayCostLimitedMask & glaze::clayCostLimitedLowCut) != 0, "and flagged as cost-limited for the tooltip");

            const auto withClay = readLimiter (automatic, 6.0);
            const auto withoutClay = readLimiter (allOffRig, 6.0);
            logMessage ("  limiter GR max over 6 s: automatic " + juce::String (withClay.grMaxDb, 3) + " dB, all Off "
                        + juce::String (withoutClay.grMaxDb, 3) + " dB (input peak - ceiling: " + juce::String (withClay.peakMinusGainDb, 3)
                        + " / " + juce::String (withoutClay.peakMinusGainDb, 3) + " dB)");
            expect (withClay.grMaxDb <= withoutClay.grMaxDb + 0.3f, "the automatic Clay adds at most 0.3 dB of limiter GR over Clay off");

            // 阳性对照（GLAZE 0：只差手动低切）。
            auto fixedGain = glazeParams (0.0f);
            fixedGain.limit.gainDb = 2.0f;
            auto fixedOff = fixedGain;
            fixedOff.claySettings = allOff();
            auto fixedCut = fixedGain;
            fixedCut.claySettings.lowCutMode = ClayMode::manual;
            fixedCut.claySettings.lowCutHz = 20.0f;

            ClayRig plainOff (fastConstants(), material, fixedOff);
            ClayRig plainCut (fastConstants(), material, fixedCut);
            plainOff.run (1.0);
            plainCut.run (1.0);
            plainCut.chain.getClayForTesting().getAndResetPeakGuardStats();
            const auto grPlainOff = readLimiter (plainOff, 6.0).grMaxDb;
            const auto grPlainCut = readLimiter (plainCut, 6.0).grMaxDb;
            const auto guardStats = plainCut.chain.getClayForTesting().getAndResetPeakGuardStats();
            logMessage ("  GLAZE 0 / Limit Gain +2 dB: GR max off " + juce::String (grPlainOff, 3) + " dB, manual 20 Hz low cut "
                        + juce::String (grPlainCut, 3) + " dB; peak guard engaged on " + juce::String (guardStats.guardedSamples)
                        + " samples, min g " + juce::String (guardStats.minGain, 3));
            expect (grPlainCut <= grPlainOff + 0.3f, "ADR-014: with the peak guard even a manual 20 Hz IIR low cut adds at most 0.3 dB of limiter GR");
            expect (guardStats.guardedSamples > 0 && guardStats.minGain < 0.5f,
                    "positive control: the peak guard really is pulling that low cut back on this material");
        }

        //==============================================================================
        // 10. 根治修订 4(b) 宽立体声低音不触发反相保护（审核 V1-01 的回归门）。makeWideBassMaterial 的两种素材各 30 s（不循环），
        //     GLAZE 60、抽屉默认，从第一块起数（聆听 2 ～ 3 s 期间方案源是候选画像，保护同样在工作）：
        //       a. 长期 ρ ≈ 0.8 的宽低音（音调低音 + 两声道独立的低频噪声）；
        //       b. 单声道底鼓（120 BPM，−12 dBFS）+ 两声道独立的 60 ～ 150 Hz 宽垫（各 −20 dBFS）：鼓与鼓之间只剩不相关的垫。
        //     断言：Status「反相保护中」从未亮过、保护单元的下限始终为 0（一次攻击都没开始）。两种素材的收单需要度都是 0
        //     （单声道回放低音损失只有约 −0.46 / −0.3 dB，不到 −0.5 dB），方案不收单，能动收单的只有保护。
        //     V1 实测（旧规则：60 ms 快路与慢路取小、阈值 0）：b 在 30 s 里触发 6 次、38 % 的时间收单 ≥ 0.5。新规则下慢路最低约
        //     0.70 ～ 0.72（numpy 按同一分析口径复算），远在快路门 0.3 之外，快路怎么散布都进不了判定。
        //     阳性对照：整体反相的低音（右声道 = −左声道的低音）用同一套计数，5 s 内触发、下限到 1。
        // 依据：设计规格-Clay-根治修订 3.4、4(b)；审核 V1-01 建议「长期 ρ ≥ 0.8 的素材 30 s 内 0 次触发」进仓库测试当回归门。
        void testWideBassNeverTriggersPhaseGuard()
        {
            beginTest ("wide stereo bass (long-term rho ~0.8) and mono kick + independent wide pad: the phase guard never triggers in 30 s");

            struct Case
            {
                const char* label;
                WideBass kind;
                double seconds;
                bool mustTrigger;
            };

            const Case cases[] =
            {
                { "tonal bass + independent low noise, long-term rho 0.8",                  WideBass::tonalRho08, 30.0, false },
                { "mono kick (120 BPM, -12 dBFS) + independent wide pad 60-150 Hz (-20 dBFS)", WideBass::kickAndPad, 30.0, false },
                { "positive control: anti-phase bass",                                      WideBass::antiPhase,   5.0, true  },
            };

            for (const auto& testCase : cases)
            {
                const auto material = makeWideBassMaterial (testCase.kind, testCase.seconds + 1.0);   // 多 1 s：渲染不回绕到开头
                ClayRig rig (fastConstants(), material, glazeParams (60.0f));
                int events = 0, lampBlocks = 0;
                bool previous = false;
                float largestFloor = 0.0f;

                rig.run (testCase.seconds, [&] (int)
                {
                    const auto lamp = rig.status().clayPhaseGuardActive;
                    events += (lamp && ! previous) ? 1 : 0;
                    lampBlocks += lamp ? 1 : 0;
                    previous = lamp;
                    largestFloor = juce::jmax (largestFloor, rig.probe().clayGuard.phaseFloor);
                });

                const auto label = juce::String (testCase.label);
                logMessage ("  [" + label + "] phase-guard events " + juce::String (events) + ", lamp blocks " + juce::String (lampBlocks)
                            + ", largest floor " + juce::String (largestFloor, 3) + ", long-term 40-125 Hz rho "
                            + juce::String (bandCorrelation (material, 40.0, 125.0, 4096), 3));

                if (testCase.mustTrigger)
                    expect (events >= 1 && largestFloor > 0.99f, label + ": the guard does trigger (the count is not vacuous)");
                else
                    expect (events == 0 && largestFloor == 0.0f, label + ": no phase-guard trigger in 30 s");
            }
        }

        //==============================================================================
        // 11. 根治修订 4(c) 整体反相低频：学完正常素材（antiPhaseFixtureRecipe：单声道底鼓与贝斯、宽高频）后，从下一个拍点起把
        //     右声道的底鼓、贝斯整体反相（lowRightInverted：等功率，40 ～ 125 Hz 的 ρ 从约 +0.9 翻到约 −0.9；右声道 5 ms 升余弦过渡），
        //     量从反相音频出现到提示强度到 1 且 Status 灯亮的那一块结束（ADR-015：只提示，收单全程不变）。上限 =
        //       资格期 clayGuardPhaseMinSeconds（0.15 s）+ 攻击 clayGuardPhaseAttackSeconds（0.1 s）+ 分析滞后（2048 + 512 + 64 样本）
        //     + 5 ms 过渡 + 条件成立前的平滑时间：稳态估计下（ρ 从 +1 翻到 −1）慢路 ρ̃ = 2e^(−t/τ) − 1 降到快路门 0.3 要
        //       τ·ln(2 / 1.3) ≈ 129 ms（300 ms 慢路），快路降到 −0.2 要 τ_f·ln(2 / 0.8) ≈ 110 ms（120 ms 快路），门内那一支要两者都满足、
        //       取较大的；只靠慢路降到 −0.2 要 275 ms；两支取较小的
        //     + 块粒度 4 块；合计约 0.48 s。从拍点起反相：新的一拍底鼓一出来就以反相为主，实际比稳态估计快（numpy 按同一分析口径
        //     复算约 0.29 s；反相落在两拍之间、底鼓已衰减时约 0.55 s——那时条件要等下一拍才成立，不是保护慢）。
        //     规格 4(c) 写「从音频起 250 ms 内 a = 1」，但 3.4 规定资格期 0.15 s + 攻击 0.1 s，从条件成立起就已经用满 0.25 s，再加
        //     分析窗与平滑做不到——两条自相矛盾。这里按 3.4 的机理推导上限、记下实测值，冲突写进 P4 报告交主进程裁决；「从判定起
        //     0.25 s 到 1」由 ClayGuardTests（资格期 d）直接量。另断言不早于 0.25 s（资格期确实在起作用）。
        void testWholeLowBandInversion()
        {
            beginTest ("whole low band inverted (equal power): the warning reaches full level within qualification + attack + analysis and smoothing lag");

            auto constants = fastConstants();
            constants.mismatchThresholdLu = constants.mismatchReleaseLu = 1.0e9f;   // 同用例 4：只看 Clay 的保护
            auto recipe = antiPhaseFixtureRecipe();
            const auto normal = makeClayMusic (8.0, 404, recipe);
            recipe.lowRightInverted = true;
            const auto inverted = makeClayMusic (8.0, 404, recipe);

            ClayRig rig (constants, normal, glazeParams (60.0f));
            expect (rig.learnAndCalibrate (40.0), "learns the normal (mono-bass) material");
            rig.run (1.0);
            const auto before = rig.status();
            expect (before.clayMonoAmount == 0.0f && ! before.clayPhaseGuardActive, "fixture: no warning on the normal material; ADR-015, no automatic mono");

            // 接续：从当前位置无缝往下放，到下一个拍点（素材每 0.5 s 一拍、从 0 起）换成低频反相的版本；左声道两个版本相同。
            const auto length = normal.front().size();
            const auto beatSamples = static_cast<size_t> (0.5 * kRate);
            const auto flipAt = static_cast<int> ((beatSamples - rig.sourceIndex % beatSamples) % beatSamples);
            const auto total = flipAt + static_cast<int> (3.0 * kRate);
            const auto ramp = static_cast<int> (kRampSeconds * kRate);
            auto continuation = makeSilence (kChannels, total);

            for (int i = 0; i < total; ++i)
            {
                const auto index = (rig.sourceIndex + static_cast<size_t> (i)) % length;
                const auto k = i - flipAt;
                const auto w = k < 0 ? 0.0
                                     : (k >= ramp ? 1.0
                                                  : 0.5 - 0.5 * std::cos (juce::MathConstants<double>::pi * static_cast<double> (k)
                                                                          / static_cast<double> (ramp)));
                continuation[0][static_cast<size_t> (i)] = normal[0][index];
                continuation[1][static_cast<size_t> (i)] = static_cast<float> ((1.0 - w) * static_cast<double> (normal[1][index])
                                                                               + w * static_cast<double> (inverted[1][index]));
            }

            rig.source = continuation;
            rig.sourceIndex = 0;

            const auto blockSeconds = static_cast<double> (kBlock) / kRate;
            const auto totalBlocks = total / kBlock;
            double latency = -1.0;
            bool monoUntouched = true;

            for (int b = 0; b < totalBlocks && latency < 0.0; ++b)
            {
                rig.step();
                const auto st = rig.status();
                monoUntouched = monoUntouched && st.clayMonoAmount == 0.0f && rig.clay().monoAmount == 0.0f;

                if (rig.probe().clayGuard.phaseFloor >= kAtOne && st.clayPhaseGuardActive)
                    latency = (static_cast<double> ((b + 1) * kBlock) - static_cast<double> (flipAt)) / kRate;
            }

            expect (monoUntouched, "ADR-015: the warning never changes the mono amount");

            const auto threshold = static_cast<double> (constants.clayGuardPhaseCorrelation);
            const auto gate = static_cast<double> (constants.clayGuardPhaseFastGate);
            const auto gatedBranch = juce::jmax (kLiveSlowSeconds * std::log (2.0 / (1.0 + gate)),
                                                 kLiveFastSeconds * std::log (2.0 / (1.0 + threshold)));
            const auto slowBranch = kLiveSlowSeconds * std::log (2.0 / (1.0 + threshold));
            const auto crossingSeconds = juce::jmin (gatedBranch, slowBranch);
            const auto bound = constants.clayGuardPhaseMinSeconds + constants.clayGuardPhaseAttackSeconds + kAnalysisSeconds
                             + kRampSeconds + crossingSeconds + 4.0 * blockSeconds;

            logMessage ("  whole low band inverted at a beat: warning at full level after " + juce::String (latency * 1000.0, 1) + " ms (bound "
                        + juce::String (bound * 1000.0, 1) + " ms; spec 4(c) says 250 ms from the audio, which the 0.15 s + 0.1 s of 3.4 "
                        + "already use up from the moment the condition holds)");
            expect (latency > 0.0, "the phase warning fires on the whole inverted low band");
            expect (latency <= bound, "the warning reaches full level within qualification + attack + analysis and smoothing lag: "
                                      + juce::String (latency * 1000.0, 1) + " ms");
            expect (latency >= constants.clayGuardPhaseMinSeconds + constants.clayGuardPhaseAttackSeconds,
                    "and not before the 0.15 s qualification + 0.1 s attack could have run");
        }

        //==============================================================================
        // 13. BUG-009（T-042）只有失谐 pad 的 breakdown 不亮反相提示。素材照合成 House 曲（tools/render/make_synthetic_house.py）连成一首
        //     52 s 的歌：drop 0 ～ 20 s = 单声道底鼓（120 BPM，RMS −12 dBFS）+ 单声道 55 Hz 音调低音（8 次谐波 1/h，−14 dBFS）+ 失谐 pad
        //     （makeDetunedPad，基音幅度 0.05，和弦每 2 s 一换，整首连续）；breakdown 20 ～ 44 s = 只有 pad；44 ～ 52 s 再回 drop。鼓与低音
        //     在两个段界各用 20 ms 升余弦收放。GLAZE 60、抽屉默认，只在 drop 段上循环学完并锁定，再从当前位置无缝接着放这首歌到结尾，
        //     逐块数 Status 的反相灯与保护单元的提示强度。画像失配关掉（同用例 4：breakdown 的响度与频谱都和 drop 差很多，这里只看 Clay）。
        //     断言：breakdown 与回到 drop 的整段里灯从未亮过、提示强度始终为 0；breakdown 开头 5 s 之后慢路低频相关度（探针）一直无效
        //     （低音停下后平滑功率约 3.5 s 落到段门下，之前停在 drop 的 +1 上；之后泄漏帧不计）。
        //     改前（只有逐段静音门）：合成曲 synthetic-house-124 的 breakdown 里慢路随约 1 Hz 的拍频摆到 −0.95、灯亮了 breakdown 的 89 %
        //     （全曲 14.4 %；T-042 在 109 上用 celadon_render --trace 复现）。
        //     阳性对照：同一流程，breakdown 里换成「pad + 右声道反相的同一条音调低音」——灯要在 breakdown 里亮起、提示强度到 1
        //     （门对真反相照常放行，计数不是空转）。
        // 依据：MusicAnalyzer.cpp「泄漏门」、BUG-009；ADR-015（反相只提示）。
        void testBreakdownPadDoesNotLightPhaseWarning()
        {
            beginTest ("bass-less detuned pad breakdown between two drops (BUG-009): the phase warning never lights; an anti-phase bass in the breakdown does");

            auto constants = fastConstants();
            constants.mismatchThresholdLu = constants.mismatchReleaseLu = 1.0e9f;   // 同用例 4：只看 Clay 的保护

            constexpr double dropSeconds = 20.0, breakdownSeconds = 24.0, returnSeconds = 8.0;
            constexpr double edgeSeconds = 0.02;
            const auto songSeconds = dropSeconds + breakdownSeconds + returnSeconds;
            const auto n = static_cast<int> (songSeconds * kRate);
            const auto twoPi = juce::MathConstants<double>::twoPi;

            // breakdownBass：breakdown 里低音的符号（0 = 没有低音，−1 = 右声道反相）。
            auto makeSong = [&] (double breakdownBass)
            {
                std::vector<double> kick (static_cast<size_t> (n), 0.0), tone (static_cast<size_t> (n), 0.0);

                for (int i = 0; i < n; ++i)
                {
                    const auto t = static_cast<double> (i) / kRate;
                    kick[static_cast<size_t> (i)] = kickSample (t - 0.5 * std::floor (t / 0.5));

                    for (int h = 1; h <= 8; ++h)
                        tone[static_cast<size_t> (i)] += std::sin (twoPi * 55.0 * static_cast<double> (h) * t + 0.3 * static_cast<double> (h))
                                                        / static_cast<double> (h);
                }

                kick = withRms (std::move (kick), -12.0);
                tone = withRms (std::move (tone), -14.0);

                // drop 的权重：段界前后各 edgeSeconds 升余弦，breakdown 里 0。
                auto dropWeight = [&] (double t)
                {
                    auto ramp = [&] (double edge, double sign)
                    {
                        const auto u = juce::jlimit (0.0, 1.0, 0.5 + sign * (t - edge) / (2.0 * edgeSeconds));
                        return 0.5 - 0.5 * std::cos (juce::MathConstants<double>::pi * u);
                    };

                    return ramp (dropSeconds, -1.0) + ramp (dropSeconds + breakdownSeconds, 1.0);
                };

                auto song = makeDetunedPad (kRate, songSeconds, 2.0, 0.05);

                for (int i = 0; i < n; ++i)
                {
                    const auto index = static_cast<size_t> (i);
                    const auto w = dropWeight (static_cast<double> (i) / kRate);
                    const auto common = w * (kick[index] + tone[index]);
                    song[0][index] += static_cast<float> (common + (1.0 - w) * std::abs (breakdownBass) * tone[index]);
                    song[1][index] += static_cast<float> (common + (1.0 - w) * breakdownBass * tone[index]);
                }

                return song;
            };

            for (const auto breakdownBass : { 0.0, -1.0 })
            {
                const auto control = breakdownBass != 0.0;
                const auto song = makeSong (breakdownBass);
                const auto dropSamples = static_cast<int> (dropSeconds * kRate);
                const auto label = juce::String (control ? "positive control (anti-phase bass in the breakdown)" : "pad-only breakdown");

                auto drop = makeSilence (kChannels, dropSamples);

                for (int ch = 0; ch < kChannels; ++ch)
                    std::copy_n (song[static_cast<size_t> (ch)].begin(), dropSamples, drop[static_cast<size_t> (ch)].begin());

                ClayRig rig (constants, std::move (drop), glazeParams (60.0f));
                expect (rig.learnAndCalibrate (40.0), label + ": learns the drop");
                rig.run (1.0);
                expect (! rig.status().clayPhaseGuardActive, label + ": fixture - no warning on the drop (mono bass)");

                // 从当前位置无缝接着放整首歌（学习用的循环素材就是这首歌的前 20 s），一直到结尾、不回绕。
                const auto startIndex = rig.sourceIndex;
                rig.source = song;
                rig.sourceIndex = startIndex;
                const auto totalBlocks = static_cast<int> ((static_cast<size_t> (n) - startIndex) / static_cast<size_t> (kBlock));
                const auto startSeconds = static_cast<double> (startIndex) / kRate;
                int lampBlocks = 0, validSlowLateBlocks = 0;
                float largestFloor = 0.0f, largestFloorInBreakdown = 0.0f;
                bool lampInBreakdown = false;

                for (int b = 0; b < totalBlocks; ++b)
                {
                    rig.step();
                    const auto t = startSeconds + static_cast<double> ((b + 1) * kBlock) / kRate;   // 本块结束时在歌里的位置
                    const auto lamp = rig.status().clayPhaseGuardActive;
                    const auto level = rig.probe().clayGuard.phaseFloor;
                    const auto inBreakdown = t >= dropSeconds && t < dropSeconds + breakdownSeconds;
                    lampBlocks += lamp ? 1 : 0;
                    largestFloor = juce::jmax (largestFloor, level);

                    if (inBreakdown)
                    {
                        lampInBreakdown = lampInBreakdown || lamp;
                        largestFloorInBreakdown = juce::jmax (largestFloorInBreakdown, level);

                        if (t >= dropSeconds + 5.0 && rig.probe().liveLowCorrelation.valid)
                            ++validSlowLateBlocks;
                    }
                }

                logMessage ("  [" + label + "] lamp blocks " + juce::String (lampBlocks) + " / " + juce::String (totalBlocks)
                            + ", largest floor " + juce::String (largestFloor, 3) + "; breakdown blocks (after 5 s) with a valid slow low correlation "
                            + juce::String (validSlowLateBlocks));

                if (control)
                {
                    expect (lampInBreakdown && largestFloorInBreakdown >= kAtOne,
                            label + ": the warning does light on a real inversion under the pad (the count is not vacuous)");
                }
                else
                {
                    expectEquals (lampBlocks, 0, label + ": the phase warning never lights");
                    expect (largestFloor == 0.0f, label + ": the guard never starts an attack");
                    expectEquals (validSlowLateBlocks, 0, label + ": the pad's leakage never makes the slow low correlation valid");
                }
            }
        }

        //==============================================================================
        // 12. 根治修订 4(d) 约 10 Hz 次声 + 音乐（makeClayMusic：单声道低音、宽高频——收单与加宽都不需要，只看低切；10.3 Hz、幅度 0.4，
        //     10.3 而不是 10 的理由同 stereoTestRecipe）。numpy 复算（种子 909 ～ 911）：真次声占比约 −1.9 dB → need_sub = 1；IIR 20 Hz
        //     低切的代价 −1.0 ～ −0.8 dB（去掉次声，峰值变低）→ 照做。GLAZE 60、聆听 12 s、学完并校准锁定后：
        //       - 画像：低切代价有效且 ≤ 0（根治修订 4(d)「代价 ≤ 0」）；Status：低切 > 0（开启）、不报代价受限；
        //       - 与同一素材的「修坯全关」相比，锁定后 6 s 里限幅器入口的最大峰值不升：比「GR − 这一块实际的 Limit Gain」的最大值
        //         （readLimiter），自动 ≤ 全关 + 0.05 dB。为什么不直接比 GR：两条链各自校准、各自的保护层按自己的 GR 退让，Limit Gain
        //         可能不同（全关那条峰值高、更可能退让）；扣掉增益只剩 Clay 低切对入口峰值的影响。0.05 dB 容差 = 两条链校准偏差的舍入
        //         （响度只差次声的 K 加权份额，< 0.02 LU）。原始 GR 另记日志。
        // 依据：设计规格-Clay-根治修订 3.1（真次声）、3.3（低切：need · c · 强度 · f）、4(d)。
        void testInfrasoundLowCutHelps()
        {
            beginTest ("~10 Hz infrasound under music: the tried low cut is cheap (cost <= 0), engaged, and does not raise the limiter's input peaks");

            const auto constants = confidentConstants();
            ClayRecipe recipe;
            recipe.gain = 0.4f;
            recipe.bassPhaseDegrees = 0.0;
            recipe.narrowHighs = false;
            recipe.subHz = 10.3;
            recipe.subAmplitude = 0.4f;
            const auto material = makeClayMusic (8.0, 909, recipe);
            auto offParams = glazeParams (60.0f);
            offParams.claySettings = allOff();

            ClayRig automatic (constants, material, glazeParams (60.0f));
            ClayRig allOffRig (constants, material, offParams);
            expect (automatic.learnAndCalibrate (60.0), "automatic: learns, calibrates and locks");
            expect (allOffRig.learnAndCalibrate (60.0), "all Off: learns, calibrates and locks");

            const auto profile = automatic.committed().profile;
            logMessage ("  profile: " + featureText ("subShareDb", profile.subShareDb) + "; costs: " + costText (profile));
            expect (profile.subShareDb.valid && profile.subShareDb.value > constants.claySubShareHighDb,
                    "fixture: strong infrasound (low-cut need = 1)");
            expect (profile.lowCutPeakCostDb.valid && profile.lowCutPeakCostDb.value <= 0.0f,
                    "trying the low cut lowers (or keeps) the peaks: cost " + juce::String (profile.lowCutPeakCostDb.value, 2) + " dB <= 0");

            const auto s = automatic.status();
            logMessage ("  automatic: " + statusText (s) + ", cost mask " + juce::String (static_cast<int> (s.clayCostLimitedMask)));
            expect (s.clayLowCutHz > ClayModule::minLowCutHz, "the low cut is engaged: " + juce::String (s.clayLowCutHz, 2) + " Hz");
            expectEquals (static_cast<int> (s.clayCostLimitedMask & glaze::clayCostLimitedLowCut), 0, "and not flagged as cost-limited");

            const auto withClay = readLimiter (automatic, 6.0);
            const auto withoutClay = readLimiter (allOffRig, 6.0);
            logMessage ("  over 6 s: limiter input peak - ceiling " + juce::String (withClay.peakMinusGainDb, 3) + " dB with Clay, "
                        + juce::String (withoutClay.peakMinusGainDb, 3) + " dB all Off; raw GR max " + juce::String (withClay.grMaxDb, 3)
                        + " / " + juce::String (withoutClay.grMaxDb, 3) + " dB");

            if (withoutClay.grMaxDb > 0.05f)
                expect (withClay.peakMinusGainDb <= withoutClay.peakMinusGainDb + 0.05f,
                        "the automatic low cut does not raise the peaks reaching the limiter");
            else
                expect (withClay.grMaxDb <= 0.05f, "neither chain limits: nothing to raise");
        }

        //==============================================================================
        // 13. T-043（BUG-010 / ADR-027）偏置低音（真歌 woqu 的机制）：49 Hz 贝斯 0.45 + 98 Hz 二次谐波 0.07 + 4 Hz 次声偏置 0.10
        //     （两声道相同）+ −40 dBFS 白噪声；8 s 里 49 / 98 / 4 Hz 都是整数周期，循环播放没有接缝。20 Hz 低切改变贝斯的波峰形状并去掉
        //     偏置，越过守护局部界的峰很多，守护大半时间在拉，回收时与干路互相抵消——不经守护的低切峰值反而更低（代价 ≤ 0.2 dB，照做），
        //     ADR-014 的两层都看不出来。109 渲染实测（30 s 同素材、GLAZE 60）：试用损失 10 / 20 Hz = 0.085 / 0.106 dB → 封到 12.05 Hz、
        //     链内 0.084 dB。GLAZE 60、学完并校准锁定后：
        //       - 画像：先试后用的两点混合损失有效、20 Hz 那点 > 门槛 0.09 dB（夹具确实会抵消）；峰值代价 ≤ 0.2 dB（代价系数 1：低切
        //         若被减量，只可能是混合损失封顶）；
        //       - 方案：低切 ≤ 本文件按定义算的越过点 10·(0.09 / L₁₀)^(1/q)（q = log₂(L₂₀ / L₁₀)）、低于 20 Hz，越过点在地板 10 Hz 之下
        //         则为 0（否决）——两种都置低切代价位（L₁₀ 离门槛只有约 0.005 dB，环境略变就可能走到否决那一支，断言对两支都成立）；
        //       - 链里（低切开着时）：锁定后 6 s，ClayModule 统计的守护混合损失 ≤ 0.12 dB（门槛 0.09 + 试用（原始输入）对链内的偏差）。
        // 依据：BUG-010、ADR-027；GlazePlanner.cpp capLowCutForBlend；MusicAnalyzer「五」。
        void testOffsetBassBlendLossCap()
        {
            beginTest ("offset bass (T-043): the trial's guard blend loss caps the automatic low cut, and the chain's guard then loses <= 0.12 dB");

            const auto constants = confidentConstants();
            const auto numSamples = static_cast<int> (8.0 * kRate);
            auto material = makeSilence (kChannels, numSamples);
            juce::Random noise (4343);
            const auto twoPi = juce::MathConstants<double>::twoPi;

            for (int i = 0; i < numSamples; ++i)
            {
                const auto t = static_cast<double> (i) / kRate;
                const auto value = static_cast<float> (0.45 * std::sin (twoPi * 49.0 * t) + 0.07 * std::sin (twoPi * 98.0 * t)
                                                       + 0.10 * std::sin (twoPi * 4.0 * t) + 0.01 * (noise.nextDouble() * 2.0 - 1.0));

                for (auto& channel : material)
                    channel[static_cast<size_t> (i)] = value;
            }

            ClayRig rig (constants, material, glazeParams (60.0f));
            expect (rig.learnAndCalibrate (60.0), "learns, calibrates and locks");

            const auto profile = rig.committed().profile;
            const auto& l10 = profile.lowCutBlendLossDb[0];
            const auto& l20 = profile.lowCutBlendLossDb[1];
            logMessage ("  profile: " + featureText ("subShareDb", profile.subShareDb) + "; costs: " + costText (profile)
                        + "; blend loss 10 Hz " + juce::String (l10.value, 4) + ", 20 Hz " + juce::String (l20.value, 4) + " dB");
            expect (l10.valid && l20.valid && l20.value > constants.clayBlendLossLimitDb && l10.value > 0.0f,
                    "fixture: the trial measures a blend loss above the 0.09 dB limit at 20 Hz");
            expect (profile.lowCutPeakCostDb.valid && profile.lowCutPeakCostDb.value <= constants.clayCostFullDb,
                    "fixture: the unguarded low cut does not raise the peaks (the peak-cost layer lets it through)");

            const auto q = std::log2 ((double) l20.value / (double) l10.value);
            const auto crossing = 10.0 * std::pow ((double) constants.clayBlendLossLimitDb / (double) l10.value, 1.0 / q);
            const auto s = rig.status();
            logMessage ("  automatic: " + statusText (s) + ", predicted crossing " + juce::String (crossing, 3) + " Hz");
            if (crossing < (double) constants.clayLowCutFloorHz)
                expectEquals (s.clayLowCutHz, 0.0f, "the predicted crossing lies below the floor: no low cut");
            else
                expect ((double) s.clayLowCutHz <= crossing * 1.0001 && s.clayLowCutHz < 20.0f,
                        "the planned low cut is capped at the predicted 0.09 dB crossing: " + juce::String (s.clayLowCutHz, 3) + " Hz");

            expect ((s.clayCostLimitedMask & glaze::clayCostLimitedLowCut) != 0, "the status explains it (low-cut cost bit)");

            if (! (s.clayLowCutHz > 0.0f))
                return;

            rig.chain.getClayForTesting().getAndResetPeakGuardStats();
            rig.run (6.0);
            const auto stats = rig.chain.getClayForTesting().getAndResetPeakGuardStats();
            const auto chainLoss = stats.blendOutputEnergy > 0.0 ? 10.0 * std::log10 (stats.blendIntendedEnergy / stats.blendOutputEnergy) : 0.0;
            logMessage ("  chain over 6 s: guard blend loss " + juce::String (chainLoss, 4) + " dB, guarded "
                        + juce::String (100.0 * (double) stats.guardedSamples / (6.0 * kRate), 1) + " % of samples");
            expect (stats.blendSamples > 0 && chainLoss <= 0.12, "the chain's guard now loses <= 0.12 dB");
        }
    };

    static ClayGlazeTests clayGlazeTests;
}

/*
  ==============================================================================

    GlazePlanner.cpp
    GLAZE 决策器（设计规格-GLAZE-决策与校准第 1～5、8 节；ADR-008）。纯函数：只读输入、只写返回值，
    不分配、不加锁、没有静态可变状态；同一份输入逐位同一份输出。

    一次 makePlan 的步骤：

      1. 风格权重 w（离散风格独热；自动 = 画像锁定的权重）→ 混合常数 sc；
      2. 画像特征按置信度向保守默认值收拢（不知道不当 0）；强度 κ = 底强度 + (1 − 底强度) × 置信度（含代表性）；
      3. 目标：L_pref = clamp(L_style − f(LRA), −14, −7)，满强度请求 G_full = max(最小推响量, L_pref − I_in)；
      4. 满强度方案（GLAZE 100、手动的 7 个参数为 0，其余用户设置照用）：
           超限估计 E = max(0, TP_in + G − Ceiling) 按 Heat / Clip / Limit 三项权重分摊削峰（请求为 0 时照样分摊输入自己的超限）；
           Glue 实测不降峰均比，它的权重解释为「每 dB 超限给多少 dB 平均 GR」（密度）；
           任一模块按自己的量（Heat 驱动与削峰量、Clip 深度与活动、Limit 峰值 / p95 / 平均 GR）放不下它那一份
           → 降低 G 重新求（不把负担甩给别的模块），得到受约束的 G_full′；
           换算成参数：Glue 阈值按侧链检测电平定、Heat 查换算表、Clip 阈值 = Ceiling + 限幅器那一份；
           Heat（算法 8 起，T-032 R6-01）只承担换算表里该电平、上限以内真能削掉的峰均比，削不到 heatUsefulCrestDb 就只留风格底量
           automaticHeatFloor（残差 Heat 表几乎不削峰：不能再用「达不到就给最大」把 Heat 顶到风格上限）；
           自动 Limit Gain 的量程是 24 − 手动 Gain（算法 8 起，T-032 R6-06）：请求放不进量程 → G_full′ 回算成量程顶值能交付的净增益、标受约束；
           Glue 的补偿本身就比 G_full′ 还多（偏响的素材）→ 不用自动 Glue（否则低强度时 Gain 得为负）；
      5. 按强度 s = GLAZE × κ 展开：Glue / Heat / Tone / Recover / Tame 的增量 = 满强度值 × s；
         Limit Gain 走 5 个折点的单调折线（各折点按前向模型求「净增益 = 中性链 + s × G_full′」所需的 Gain），
         中段的前馈误差也在 ±0.2 dB 内（实测，见 IMPL-A2）；Clip 在前段从 0 爬到满强度值，爬完之前峰值还到不了阈值的线性段；
      6. 叠加手动值后逐模块核对约束（前向链模型；输入本身已超的量不算手动的，也不因此停摆）：
         手动本身已超 → 撤回该模块的自动增量、标 limitedByManual；叠加后超 → 在固定网格上二分出最大可行强度 s_c，
         与 GLAZE 无关，最终强度 = min(s, s_c)，所以随 GLAZE 严格单调；约束反馈 maxGainDb 同样折成强度上限；
         手动已让限幅器超约束 / Limit 旁通 / maxGainDb 低于手动链净增益 → 电平类自动全部撤回，另标 automaticLevelWithdrawn
         （T-032 R6-02：limitedByManual 是界面提示，Heat / Glue / Clip 单独被撤回时其余自动增量照常展开；编排器的画像失配、
         保护层、校准与约束反馈只看 automaticLevelWithdrawn）；手动单独已超预算、自动部分不再为它负责的项另记 manualBudgetMask
         （A11，manualBudgetMaskFor：编排器的保护层与上游监视不核对它们，只核对自动部分；手动中性时为 0）；
      7. 目标：requestedGain = 手动链的预计净增益 + GLAZE × G_full（风格与 GLAZE 想要的）；
               effectiveGain = 手动链的预计净增益 + s × G_full′（校准器用它作目标净提升，含手动部分，所以不会去抵消手动）。
      8. 修坯 Clay（v0.1.7.0，算法 7；设计规格-Clay-分析决策与验收第 2 节，ADR-012 / 设计规格-Clay-根治修订 3.3 修订）：与 1 ～ 7
         完全独立的三项绝对值（低切、收单、加宽），只看画像的立体声 / 次声特征与「先试后用」量到的峰值代价、GLAZE 与风格权重
         （见 planClay）。不进前向链模型、不受约束反馈与手动值影响，1 ～ 7 也不看 Clay 特征：画像多了 Clay 特征，方案的其余字段逐位不变。

    模型都是「保守初值」（规格第 5 节）：数值来自真实模块在合成素材上的实测（GlazeConstants.h 的【实测】项），
    偏差由校准器（固定增益偏差）与保护层（实测约束）收口。

  ==============================================================================
*/

#include "GlazePlanner.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace celadon::glaze
{
    namespace
    {
        constexpr float infinity = std::numeric_limits<float>::infinity();

        // 约束核对的容差：模型自身的舍入与近似不应让方案在自己的边界上来回翻（dB 类 / 占比类）。
        constexpr float capToleranceDb = 0.05f;
        constexpr float capToleranceRatio = 1.02f;

        // 强度网格：GLAZE × κ 落到 2^12 格（每格 0.024 %）；约束边界 s_c 在更粗的 2^7 格上二分（7 次前向估计，每格 0.8 %，
        // 对 15 dB 的方案约 0.12 dB，偏保守的一侧）。两个网格互为整数倍，取小时不会因舍入倒退（见 strengthBound）。
        constexpr int strengthGridSteps = 4096;
        constexpr int boundGridSteps = 128;

        // 「限幅器损失 → Gain → 峰值 → 损失」的定点迭代次数（损失对峰值的斜率 < 0.5，3 次后剩余误差 < 0.01 dB，由校准收口）。
        constexpr int lossIterations = 3;
        constexpr int knotIterations = 2;   // 折点只定中段的形状（之后还要取单调包络、夹在 0 与满强度值之间），2 次足够

        // T-032（2026-09-29）的两条规则从算法 8 起（GlazeTypes.h currentAlgorithmVersion 的说明）：R6-01 自动 Heat 只承担真能削掉的量、
        // R6-06 满强度增益按 Limit Gain 量程回算。看的是**快照**的 algorithmVersion（与下面 capacityAt 的 ≥ 3 同一写法）：算法 6 / 7
        // 的存档重开时用存档里的快照重算，走旧分支，增量逐位同存档。
        constexpr int heatAndGainRangeAlgorithmVersion = 8;

        // Limit Gain 参数的上限（ParameterRanges / applyIncrements 的钳位）：手动 + 自动之和不会超过它。
        constexpr float limitGainRangeDb = 24.0f;

        //==============================================================================
        bool isFinite (float x) noexcept              { return std::isfinite (x); }

        bool sameBits (float a, float b) noexcept
        {
            std::uint32_t x, y;
            std::memcpy (&x, &a, sizeof (x));
            std::memcpy (&y, &b, sizeof (y));
            return x == y;
        }
        float clamp01 (float x) noexcept              { return x > 0.0f ? (x < 1.0f ? x : 1.0f) : 0.0f; }   // NaN → 0
        float nonNegative (float x) noexcept          { return x > 0.0f ? x : 0.0f; }                       // NaN → 0
        float clampTo (float x, float lo, float hi) noexcept { return x < lo ? lo : (x > hi ? hi : x); }

        float ramp (float x, float lo, float hi) noexcept
        {
            if (! (hi > lo))
                return x >= hi ? 1.0f : 0.0f;

            return clamp01 ((x - lo) / (hi - lo));
        }

        float dbToGain (float db) noexcept            { return std::exp (db * 0.115129254649702f); }
        float gainToDb (float gain) noexcept          { return 8.68588963806504f * std::log (gain); }

        // 约束是否生效（与 GlazeGuard 同一约定：≤ 0 或非有限值 = 不设约束）。
        bool enforced (float cap) noexcept            { return isFinite (cap) && cap > 0.0f; }

        bool exceedsDb (float usage, float cap) noexcept          { return enforced (cap) && usage > cap + capToleranceDb; }
        bool exceedsFraction (float usage, float cap) noexcept    { return enforced (cap) && usage > cap * capToleranceRatio; }

        float lerp (float from, float to, float amount) noexcept  { return from + (to - from) * amount; }

        //==============================================================================
        // 风格权重：非有限值与负数按 0，归一；全为 0 → 通透（最保守）。
        StyleWeights sanitise (const StyleWeights& w) noexcept
        {
            const auto warm = nonNegative (w.warm);
            const auto punchy = nonNegative (w.punchy);
            const auto clean = nonNegative (w.clean);
            const auto sum = warm + punchy + clean;

            if (! (sum > 0.0f) || ! isFinite (sum))
                return { 0.0f, 0.0f, 1.0f };

            if (sum == 1.0f)
                return { warm, punchy, clean };

            return { warm / sum, punchy / sum, clean / sum };
        }

        //==============================================================================
        // Glue 静态曲线（比率 R、软拐点宽 W）：x = 检测电平 − 阈值，返回 GR（dB，正数）。
        struct GlueCurve
        {
            float slope = 0.5f;       // 1 − 1/R
            float kneeWidth = 6.0f;

            float reduction (float x) const noexcept
            {
                if (! (slope > 0.0f))
                    return 0.0f;

                const auto half = 0.5f * kneeWidth;

                if (x <= -half)
                    return 0.0f;

                if (x >= half || ! (kneeWidth > 0.0f))
                    return slope * nonNegative (x);

                const auto t = x + half;
                return slope * t * t / (2.0f * kneeWidth);
            }

            // reduction 的反函数：给定 GR（> 0）求 x。
            float inverse (float gr) const noexcept
            {
                const auto half = 0.5f * kneeWidth;

                if (! (gr > 0.0f))
                    return -half;

                if (! (kneeWidth > 0.0f) || gr >= slope * half)
                    return gr / slope;

                return std::sqrt (2.0f * kneeWidth * gr / slope) - half;
            }
        };

        //==============================================================================
        // Heat 换算表在某个入口真峰值处的一行（沿电平方向线性插值，超出格点范围按端点）。
        struct HeatRow
        {
            std::array<float, (size_t) heatTableAmounts> amount {};
            std::array<float, (size_t) heatTableAmounts> loudness {};
            std::array<float, (size_t) heatTableAmounts> peak {};
            std::array<float, (size_t) heatTableAmounts> crest {};       // 峰均比下降，沿 Heat 方向取累计最大（单调、≥ 0）
            std::array<float, (size_t) heatTableAmounts> reduction {};   // 真峰值被削掉的量，同样累计最大

            HeatRow (const Constants& c, float truePeakDb) noexcept
            {
                const auto& levels = c.heatTableTruePeakDbtp;
                int lower = 0;
                float fraction = 0.0f;

                if (! (truePeakDb > levels[0]))
                {
                    lower = 0;
                }
                else if (! (truePeakDb < levels[(size_t) heatTableLevels - 1]))
                {
                    lower = heatTableLevels - 2;
                    fraction = 1.0f;
                }
                else
                {
                    while (lower < heatTableLevels - 2 && truePeakDb >= levels[(size_t) lower + 1])
                        ++lower;

                    const auto span = levels[(size_t) lower + 1] - levels[(size_t) lower];
                    fraction = span > 0.0f ? clamp01 ((truePeakDb - levels[(size_t) lower]) / span) : 0.0f;
                }

                float crestSoFar = 0.0f, reductionSoFar = 0.0f;

                for (int j = 0; j < heatTableAmounts; ++j)
                {
                    const auto a = (size_t) (lower * heatTableAmounts + j);
                    const auto b = a + (size_t) heatTableAmounts;

                    amount[(size_t) j] = c.heatTableAmount[(size_t) j];
                    loudness[(size_t) j] = lerp (c.heatLoudnessDb[a], c.heatLoudnessDb[b], fraction);
                    peak[(size_t) j] = lerp (c.heatPeakDb[a], c.heatPeakDb[b], fraction);

                    crestSoFar = juce::jmax (crestSoFar, nonNegative (loudness[(size_t) j] - peak[(size_t) j]));
                    reductionSoFar = juce::jmax (reductionSoFar, nonNegative (-peak[(size_t) j]));
                    crest[(size_t) j] = crestSoFar;
                    reduction[(size_t) j] = reductionSoFar;
                }
            }

            // 表里 Heat 量为 heat 时的插值：返回 lower 段号与段内位置。
            void locate (float heat, int& segment, float& fraction) const noexcept
            {
                segment = 0;
                fraction = 0.0f;

                if (! (heat > amount[0]))
                    return;

                if (! (heat < amount[(size_t) heatTableAmounts - 1]))
                {
                    segment = heatTableAmounts - 2;
                    fraction = 1.0f;
                    return;
                }

                while (segment < heatTableAmounts - 2 && heat >= amount[(size_t) segment + 1])
                    ++segment;

                const auto span = amount[(size_t) segment + 1] - amount[(size_t) segment];
                fraction = span > 0.0f ? clamp01 ((heat - amount[(size_t) segment]) / span) : 0.0f;
            }

            float at (const std::array<float, (size_t) heatTableAmounts>& column, float heat) const noexcept
            {
                int segment;
                float fraction;
                locate (heat, segment, fraction);
                return at (column, segment, fraction);
            }

            static float at (const std::array<float, (size_t) heatTableAmounts>& column, int segment, float fraction) noexcept
            {
                return lerp (column[(size_t) segment], column[(size_t) segment + 1], fraction);
            }

            // 单调列（crest / reduction）的反查：达到 target 所需的最小 Heat 量；达不到返回表的最大 Heat 量。
            float inverse (const std::array<float, (size_t) heatTableAmounts>& column, float target) const noexcept
            {
                if (! (target > column[0]))
                    return amount[0];

                for (int j = 1; j < heatTableAmounts; ++j)
                {
                    const auto hi = column[(size_t) j];

                    if (hi >= target)
                    {
                        const auto lo = column[(size_t) j - 1];
                        const auto fraction = hi > lo ? clamp01 ((target - lo) / (hi - lo)) : 0.0f;
                        return lerp (amount[(size_t) j - 1], amount[(size_t) j], fraction);
                    }
                }

                return amount[(size_t) heatTableAmounts - 1];
            }
        };

        // estimateColor 用的轻量查表：只算 Heat 量 heat 所在那一段的响度 / 峰值，以及到这一段为止的削峰量累计最大
        // （与 HeatRow 同一插值，结果逐位相同；省掉整行 4 个数组的构造——约束二分时每次都要走这里）。
        struct HeatPoint
        {
            float loudnessDb = 0.0f, peakDb = 0.0f, peakReductionDb = 0.0f;
        };

        HeatPoint heatAt (const Constants& c, float truePeakDb, float heat) noexcept
        {
            const auto& levels = c.heatTableTruePeakDbtp;
            const auto& amounts = c.heatTableAmount;
            int lower = 0;
            float levelFraction = 0.0f;

            if (! (truePeakDb > levels[0]))
            {
                lower = 0;
            }
            else if (! (truePeakDb < levels[(size_t) heatTableLevels - 1]))
            {
                lower = heatTableLevels - 2;
                levelFraction = 1.0f;
            }
            else
            {
                while (lower < heatTableLevels - 2 && truePeakDb >= levels[(size_t) lower + 1])
                    ++lower;

                const auto span = levels[(size_t) lower + 1] - levels[(size_t) lower];
                levelFraction = span > 0.0f ? clamp01 ((truePeakDb - levels[(size_t) lower]) / span) : 0.0f;
            }

            int segment = 0;
            float fraction = 0.0f;

            if (! (heat > amounts[0]))
            {
                segment = 0;
            }
            else if (! (heat < amounts[(size_t) heatTableAmounts - 1]))
            {
                segment = heatTableAmounts - 2;
                fraction = 1.0f;
            }
            else
            {
                while (segment < heatTableAmounts - 2 && heat >= amounts[(size_t) segment + 1])
                    ++segment;

                const auto span = amounts[(size_t) segment + 1] - amounts[(size_t) segment];
                fraction = span > 0.0f ? clamp01 ((heat - amounts[(size_t) segment]) / span) : 0.0f;
            }

            const auto columnAt = [lower, levelFraction] (const std::array<float, (size_t) heatTableSize>& table, int j) noexcept
            {
                const auto a = (size_t) (lower * heatTableAmounts + j);
                return lerp (table[a], table[a + (size_t) heatTableAmounts], levelFraction);
            };

            float reductionSoFar = 0.0f, reductionAtSegment = 0.0f;

            for (int j = 0; j <= segment + 1; ++j)
            {
                reductionSoFar = juce::jmax (reductionSoFar, nonNegative (-columnAt (c.heatPeakDb, j)));

                if (j == segment)
                    reductionAtSegment = reductionSoFar;
            }

            HeatPoint point;
            point.loudnessDb = lerp (columnAt (c.heatLoudnessDb, segment), columnAt (c.heatLoudnessDb, segment + 1), fraction);
            point.peakDb = lerp (columnAt (c.heatPeakDb, segment), columnAt (c.heatPeakDb, segment + 1), fraction);
            point.peakReductionDb = lerp (reductionAtSegment, reductionSoFar, fraction);
            return point;
        }

        //==============================================================================
        // 规划要用到的环境：已经收拢过的特征、混合后的风格常数、用户的非 GLAZE 设置。
        struct Context
        {
            const Constants* c = nullptr;
            StyleConstants sc;
            GlueCurve glue;

            float inputLufs = -14.0f;      // 决策用（按置信度收拢过）
            float truePeakDb = -1.0f;
            float ceilingDb = -1.0f;
            float detectorDb = -14.5f;     // Glue 侧链检测电平（响段）

            float mix = 1.0f;              // Color Mix（0 ～ 1）
            float colorOutputDb = 0.0f;
            bool colorBypassed = false;
            bool limitBypassed = false;

            float clipKneeGapDb = 3.1f;    // SoftClip 线性段到阈值的距离（dB）
            float glueLowRiskScale = 1.0f; // 自动 Glue 份额的低频风险折扣（1 = 不折，见 makePlan「Glue 的低频风险」）
            float manualGainDb = 0.0f;     // 手动 Limit Gain（已钳到参数范围）：自动 Limit Gain 的量程是 24 − 它（T-032 R6-06）
        };

        // 一组「会影响电平的 4 个参数」的取值（实际值：手动 + 自动，或只有其中一方）。Tone / Recover / Tame 按不改变整体电平计。
        struct Settings
        {
            float glue = 0.0f, heat = 0.0f, gainDb = 0.0f, clip = 0.0f;
        };

        // Color 级的估计（Glue → Heat → Mix → Output）。
        struct ColorEstimate
        {
            float loudnessDb = 0.0f;          // 响度变化
            float peakDb = 0.0f;              // 真峰值变化
            float glueGrMeanDb = 0.0f, glueGrP95Db = 0.0f;
            float heatAmount = 0.0f, heatPeakReductionDb = 0.0f;
        };

        // 整条链的估计：净响度变化与各模块用量（与 ModuleStats / 保护层的约束同一口径）。
        struct ChainEstimate
        {
            ColorEstimate color;
            float netDb = 0.0f;               // 输出响度 − 输入响度
            float clipDepthDb = 0.0f, clipActivity = 0.0f;
            float limitGrPeakDb = 0.0f, limitGrMeanDb = 0.0f, limitGrP95Db = 0.0f;
        };

        float glueThresholdDb (const Context& ctx, float glue) noexcept
        {
            return ctx.c->glueThresholdAtFullDb * glue * 0.01f;
        }

        float glueMakeupDb (const Context& ctx, float glue) noexcept
        {
            return -glueThresholdDb (ctx, glue) * ctx.glue.slope * ctx.c->glueMakeupFraction;
        }

        // glueMakeupDb 的反函数：补偿为 makeupDb 时的 Glue（补偿与 Glue 成正比）。
        float glueForMakeup (const Context& ctx, float makeupDb) noexcept
        {
            const auto perGlue = -ctx.c->glueThresholdAtFullDb * 0.01f * ctx.glue.slope * ctx.c->glueMakeupFraction;

            if (! (perGlue > 0.0f))
                return 100.0f;

            return clampTo (nonNegative (makeupDb) / perGlue, 0.0f, 100.0f);
        }

        // Glue 级的真峰值变化（Heat 的入口 = Glue 之后，Mix / Output 之前）。
        float gluePeakDb (const Context& ctx, float glue) noexcept
        {
            if (ctx.colorBypassed || ! (glue > 0.0f))
                return 0.0f;

            const auto gr = ctx.glue.reduction (ctx.detectorDb - glueThresholdDb (ctx, glue));
            return glueMakeupDb (ctx, glue) - ctx.c->gluePeakPerGrDb * gr;
        }

        // Clip 参数 → 削波阈值（dBTP）。
        float clipThresholdDb (const Context& ctx, float clip) noexcept
        {
            const auto off = ctx.c->clipOffThresholdDb;
            return off + (ctx.ceilingDb - off) * clip * 0.01f;
        }

        // 削波阈值 → Clip 参数。
        float clipForThreshold (const Context& ctx, float thresholdDb) noexcept
        {
            const auto off = ctx.c->clipOffThresholdDb;
            const auto span = off - ctx.ceilingDb;

            if (! (span > 0.0f))
                return 0.0f;

            return clampTo (100.0f * (off - thresholdDb) / span, 0.0f, 100.0f);
        }

        // SoftClip 对超出阈值 depth dB 的峰值削掉多少（dB）。线性段以下为 0。
        float softClipReductionDb (const Context& ctx, float depthDb) noexcept
        {
            if (depthDb <= -ctx.clipKneeGapDb)
                return 0.0f;

            const auto knee = ctx.c->clipKneeRatio;
            const auto width = 1.0f - knee;
            const auto ratio = dbToGain (depthDb);   // 峰值 / 阈值

            if (! (width > 0.0f) || ratio <= knee)
                return 0.0f;

            const auto shaped = knee + width * std::tanh ((ratio - knee) / width);
            return nonNegative (gainToDb (ratio / shaped));
        }

        float limitLossDb (const Context& ctx, float excessDb, float limiterShare) noexcept
        {
            if (! (excessDb > 0.0f))
                return 0.0f;

            const auto& c = *ctx.c;
            const auto power = c.limitLossExponent == 2.0f ? excessDb * excessDb : std::pow (excessDb, c.limitLossExponent);
            const auto base = c.limitLossCoeff * power;
            const auto floor = clamp01 (c.clipLossFloor);
            return nonNegative (base * (floor + (1.0f - floor) * clamp01 (limiterShare)));
        }

        //==============================================================================
        // 前向模型（只用加减乘除与少量 exp / log / tanh，不分配）。
        ColorEstimate estimateColor (const Context& ctx, float glue, float heat) noexcept
        {
            const auto& c = *ctx.c;
            ColorEstimate e;

            if (ctx.colorBypassed)
                return e;

            float loudness = 0.0f, peak = 0.0f;

            if (glue > 0.0f)
            {
                const auto x = ctx.detectorDb - glueThresholdDb (ctx, glue);
                const auto gr = ctx.glue.reduction (x);
                const auto makeup = glueMakeupDb (ctx, glue);

                e.glueGrMeanDb = gr;
                e.glueGrP95Db = ctx.glue.reduction (x + c.glueP95OffsetDb);
                loudness += makeup - c.glueLoudnessPerGrDb * gr;
                peak += makeup - c.gluePeakPerGrDb * gr;
            }

            if (heat > 0.0f)
            {
                const auto point = heatAt (c, ctx.truePeakDb + peak, heat);   // Heat 入口 = Glue 之后
                e.heatAmount = heat;
                e.heatPeakReductionDb = point.peakReductionDb;
                loudness += point.loudnessDb;
                peak += point.peakDb;
            }

            // Mix：干路与湿路同相叠加（幅度域）；Mix 100 % 时就是湿路本身。
            if (ctx.mix < 1.0f)
            {
                const auto dry = 1.0f - ctx.mix;
                loudness = gainToDb (dry + ctx.mix * dbToGain (loudness));
                peak = gainToDb (dry + ctx.mix * dbToGain (peak));
            }

            e.loudnessDb = loudness + ctx.colorOutputDb;
            e.peakDb = peak + ctx.colorOutputDb;
            return e;
        }

        // withUsage = false：只要净响度（求 Gain 时用），省掉只给约束核对用的量（活动占比的 pow 等）。
        ChainEstimate estimateLimit (const Context& ctx, const ColorEstimate& color, float gainDb, float clip, bool withUsage = true) noexcept
        {
            const auto& c = *ctx.c;
            ChainEstimate e;
            e.color = color;

            if (ctx.limitBypassed)
            {
                e.netDb = color.loudnessDb;
                return e;
            }

            const auto clipInput = ctx.truePeakDb + color.peakDb + gainDb;
            auto limiterInput = clipInput;

            if (clip > 0.0f)
            {
                const auto depth = clipInput - clipThresholdDb (ctx, clip);
                limiterInput -= softClipReductionDb (ctx, depth);
                e.clipDepthDb = nonNegative (depth);

                if (withUsage && e.clipDepthDb > 0.0f)
                    e.clipActivity = c.clipActivityCoeff * std::pow (e.clipDepthDb, c.clipActivityExponent);
            }

            const auto excess = nonNegative (clipInput - ctx.ceilingDb);
            const auto limiterGr = nonNegative (limiterInput - ctx.ceilingDb);

            e.limitGrPeakDb = limiterGr;
            e.limitGrMeanDb = c.limitMeanCoeff * limiterGr * limiterGr;
            e.limitGrP95Db = limiterGr > 0.0f ? limiterGr * limiterGr / (limiterGr + nonNegative (c.limitP95KneeDb)) : 0.0f;

            const auto limiterShare = excess > 0.0f ? limiterGr / excess : 1.0f;
            e.netDb = color.loudnessDb + gainDb - limitLossDb (ctx, excess, limiterShare);
            return e;
        }

        ChainEstimate estimateChain (const Context& ctx, const Settings& s) noexcept
        {
            return estimateLimit (ctx, estimateColor (ctx, s.glue, s.heat), s.gainDb, s.clip);
        }

        // 给定 Color 级与 Clip，求使「净响度 = targetNetDb」的 Limit Gain（不钳位，可能为负：说明 Color 自己已经太响）。
        // 损失随 Gain 变，定点迭代（损失对 Gain 的斜率远小于 1）。
        float solveLimitGain (const Context& ctx, const ColorEstimate& color, float clip, float targetNetDb) noexcept
        {
            auto gain = targetNetDb - color.loudnessDb;

            for (int i = 0; i < knotIterations; ++i)
            {
                const auto net = estimateLimit (ctx, color, nonNegative (gain), clip, false).netDb;
                gain += targetNetDb - net;
            }

            return gain;
        }

        //==============================================================================
        // 约束核对（与保护层同一口径）。baseline = 同一输入下「手动中性」时的用量：输入本身已让某模块超上限时，
        // 方案只要不比它更糟就算可行（GLAZE 不为输入本身的问题背锅，也不因此停摆）。
        struct Violations
        {
            bool heat = false, glue = false, clip = false, limit = false;

            bool any() const noexcept   { return heat || glue || clip || limit; }
        };

        float allowance (float cap, float baselineUsage) noexcept
        {
            return juce::jmax (cap, baselineUsage);
        }

        // heatBudget：自动在用 Heat 时，Heat 的驱动上限与削峰上限才算约束（它们是「自动许用多少」的预算）。
        Violations check (const Context& ctx, const ChainEstimate& e, const ChainEstimate& baseline, bool heatBudget) noexcept
        {
            const auto& sc = ctx.sc;
            Violations v;

            v.heat = heatBudget && e.color.heatAmount > 0.0f
                     && (e.color.heatAmount > sc.heatDriveCap + 10.0f * capToleranceDb
                         || e.color.heatPeakReductionDb > sc.heatPeakReductionCapDb + capToleranceDb);

            v.glue = (enforced (sc.glueGrMeanCapDb) && exceedsDb (e.color.glueGrMeanDb, allowance (sc.glueGrMeanCapDb, baseline.color.glueGrMeanDb)))
                  || (enforced (sc.glueGrP95CapDb)  && exceedsDb (e.color.glueGrP95Db,  allowance (sc.glueGrP95CapDb,  baseline.color.glueGrP95Db)));

            v.clip = (enforced (sc.clipDepthCapDb)  && exceedsDb (e.clipDepthDb, allowance (sc.clipDepthCapDb, baseline.clipDepthDb)))
                  || (enforced (sc.clipActivityCap) && exceedsFraction (e.clipActivity, allowance (sc.clipActivityCap, baseline.clipActivity)));

            v.limit = (enforced (sc.limitGrPeakCapDb) && exceedsDb (e.limitGrPeakDb, allowance (sc.limitGrPeakCapDb, baseline.limitGrPeakDb)))
                   || (enforced (sc.limitGrP95CapDb)  && exceedsDb (e.limitGrP95Db,  allowance (sc.limitGrP95CapDb,  baseline.limitGrP95Db)))
                   || (enforced (sc.limitGrMeanCapDb) && exceedsDb (e.limitGrMeanDb, allowance (sc.limitGrMeanCapDb, baseline.limitGrMeanDb)));
            return v;
        }

        //==============================================================================
        // 满强度方案（GLAZE 100、手动的 7 个参数为 0，其余用户设置照用）与它随强度的展开方式。
        constexpr int gainKnots = 5;   // Limit Gain 随强度的折线：s = 0, 1/4, 1/2, 3/4, 1

        struct FullPlan
        {
            float requestedGainDb = 0.0f;   // G_full（风格想要的）
            float gainDb = 0.0f;            // G_full′（约束后）
            float glue = 0.0f, heat = 0.0f, clip = 0.0f;
            float clipThresholdDb = 0.0f;   // clip 对应的削波阈值（Ceiling + 限幅器那一份）
            float clipRamp = 1.0f;          // Clip 参数在强度 [0, clipRamp] 内线性爬到 clip（手动把峰值推高时由 makePlan 缩短）
            std::array<float, (size_t) gainKnots> limitGainDb {};   // 各折点的 Limit Gain 增量（单调不减）
            bool constrained = false;       // 满强度请求因模块容量或 Limit Gain 量程（rangeLimited）降低过
            bool rangeLimited = false;      // 算法 8（T-032 R6-06）：最后一次 completeFullPlan 的请求放不进 Limit Gain 量程，G_full′ 已回算
            ChainEstimate neutral;          // 手动中性时的链（约束核对的基线）
        };

        // Clip / 限幅器能放下的「Limit 入口超出量」E_rem（按限幅器份额 λ 分摊）。
        float clipLimitCapacity (const Context& ctx, float limiterFraction) noexcept
        {
            const auto& sc = ctx.sc;
            const auto& c = *ctx.c;

            auto limiterCap = infinity;

            if (enforced (sc.limitGrPeakCapDb))
                limiterCap = juce::jmin (limiterCap, sc.limitGrPeakCapDb);

            if (enforced (sc.limitGrP95CapDb))
            {
                // p95 = p² / (p + k) ≤ cap  ⟺  p ≤ (cap + √(cap² + 4·k·cap)) / 2
                const auto cap = sc.limitGrP95CapDb;
                const auto k = nonNegative (c.limitP95KneeDb);
                limiterCap = juce::jmin (limiterCap, 0.5f * (cap + std::sqrt (cap * cap + 4.0f * k * cap)));
            }

            if (enforced (sc.limitGrMeanCapDb) && c.limitMeanCoeff > 0.0f)
                limiterCap = juce::jmin (limiterCap, std::sqrt (sc.limitGrMeanCapDb / c.limitMeanCoeff));

            auto clipCap = infinity;

            if (enforced (sc.clipDepthCapDb))
                clipCap = juce::jmin (clipCap, sc.clipDepthCapDb);

            if (enforced (sc.clipActivityCap) && c.clipActivityCoeff > 0.0f && c.clipActivityExponent > 0.0f)
                clipCap = juce::jmin (clipCap, std::pow (sc.clipActivityCap / c.clipActivityCoeff, 1.0f / c.clipActivityExponent));

            auto capacity = infinity;

            if (limiterFraction > 0.0f)
                capacity = juce::jmin (capacity, limiterCap / limiterFraction);

            if (limiterFraction < 1.0f)
                capacity = juce::jmin (capacity, clipCap / (1.0f - limiterFraction));

            return capacity;
        }

        // Glue：每 dB 超限给多少 dB 平均 GR（密度），再按两个 GR 上限封顶；换算成 Glue 参数。
        float glueForExcess (const Context& ctx, float glueShare, float excessDb) noexcept
        {
            const auto& sc = ctx.sc;
            const auto& c = *ctx.c;

            if (ctx.colorBypassed || ! (glueShare > 0.0f) || ! (excessDb > 0.0f) || ! (ctx.glue.slope > 0.0f)
                || ! (c.glueThresholdAtFullDb < 0.0f))
                return 0.0f;

            auto target = glueShare * excessDb;

            if (enforced (sc.glueGrMeanCapDb))
                target = juce::jmin (target, sc.glueGrMeanCapDb);

            if (enforced (sc.glueGrP95CapDb))
                target = juce::jmin (target, ctx.glue.reduction (ctx.glue.inverse (sc.glueGrP95CapDb) - c.glueP95OffsetDb));

            if (! (target > 0.0f))
                return 0.0f;

            // GR = reduction(检测电平 − 阈值) → 阈值 = 检测电平 − inverse(GR) → Glue = 100 · 阈值 / 满量程阈值
            const auto threshold = ctx.detectorDb - ctx.glue.inverse (target);
            return clampTo (100.0f * threshold / c.glueThresholdAtFullDb, 0.0f, 100.0f);
        }

        // Glue 的有效上限（参数值）：平均 GR 与 p95 GR 两个上限都不超的最大 Glue，与 check() 同一模型（GR = reduction(检测电平 − 阈值)，
        // p95 按「检测电平 + glueP95OffsetDb」）。不设约束 → 100。
        float glueCap (const Context& ctx) noexcept
        {
            const auto& sc = ctx.sc;
            const auto& c = *ctx.c;

            if (ctx.colorBypassed || ! (ctx.glue.slope > 0.0f) || ! (c.glueThresholdAtFullDb < 0.0f))
                return 100.0f;

            // 阈值越低 GR 越大：两个上限各给出一个最低许用阈值，取高的那个；阈值 = 满量程阈值 × Glue / 100。
            auto lowestThresholdDb = -infinity;

            if (enforced (sc.glueGrMeanCapDb))
                lowestThresholdDb = juce::jmax (lowestThresholdDb, ctx.detectorDb - ctx.glue.inverse (sc.glueGrMeanCapDb));

            if (enforced (sc.glueGrP95CapDb))
                lowestThresholdDb = juce::jmax (lowestThresholdDb, ctx.detectorDb + c.glueP95OffsetDb - ctx.glue.inverse (sc.glueGrP95CapDb));

            if (! isFinite (lowestThresholdDb))
                return 100.0f;

            return clampTo (100.0f * lowestThresholdDb / c.glueThresholdAtFullDb, 0.0f, 100.0f);
        }

        // Heat 的有效上限：驱动上限与「入口真峰值处削峰量」上限取小（都是自动许用的预算，≤ 0 = 不许用）。
        float heatCap (const StyleConstants& sc, const HeatRow& row) noexcept
        {
            if (! (sc.heatDriveCap > 0.0f) || ! (sc.heatPeakReductionCapDb > 0.0f))
                return 0.0f;

            return juce::jmin (juce::jmin (sc.heatDriveCap, 100.0f), row.inverse (row.reduction, sc.heatPeakReductionCapDb));
        }

        float clipAtStrength (const FullPlan& full, float strength, float ramp) noexcept
        {
            return full.clip * juce::jmin (1.0f, strength / ramp);
        }

        // Limit Gain 折线在强度 s 处的值（折点单调不减，线性插值 → 随 s 单调）。
        float limitGainAtStrength (const FullPlan& full, float strength) noexcept
        {
            const auto position = clamp01 (strength) * (float) (gainKnots - 1);
            const auto segment = juce::jmin ((int) position, gainKnots - 2);
            const auto fraction = position - (float) segment;
            return lerp (full.limitGainDb[(size_t) segment], full.limitGainDb[(size_t) segment + 1], fraction);
        }

        // Clip 参数随强度爬坡的终点：爬完之前，Limit 入口峰值（从 startPeak 线性走到 endPeak 估计）还到不了阈值的线性段，
        // 爬坡本身不削波，限幅器也不会在爬坡途中先吃到比方案更多的量（各模块用量随强度单调，约束二分才成立）。
        // 起点就已经进了线性段以上（输入或手动把峰值推得很高）：第一个网格格点就到位（等于在 GLAZE 离开 0 时直接就位）。
        float clipRampFor (const Context& ctx, float thresholdDb, float startPeakDb, float endPeakDb, float minimumRamp) noexcept
        {
            const auto firstStep = 1.0f / (float) strengthGridSteps;
            const auto kneeStart = thresholdDb - ctx.clipKneeGapDb;

            if (! (startPeakDb < kneeStart))
                return juce::jmax (firstStep, minimumRamp);   // 也按最小爬坡走：GLAZE 离开 0 时不一步到位（v0.6 验收 F-06：原来热素材 0.1 % 就跳到 79）

            if (! (endPeakDb > startPeakDb))
                return juce::jmax (firstStep, minimumRamp);

            return clampTo ((kneeStart - startPeakDb) / (endPeakDb - startPeakDb), juce::jmax (firstStep, minimumRamp), 1.0f);
        }

        // 按给定的 Glue / Heat 求满强度的 Gain、Clip 分工与各折点。gainDb = 容量约束后的 G_full′（makeFullPlan 第 2 步），每次调用都从它
        // 起算（同一次尝试里可能先带底量、再撤回底量调两次：后一次不能沿用前一次回算过的值）。返回 false = 某个强度下要的 Gain 为负
        // （Color 自己已经比目标响）。
        // 算法 8 起（T-032 R6-06）自动 Limit Gain 的量程是 24 − 手动 Gain（combine / applyIncrements 都把手动 + 自动钳到 24）：
        // 请求的 Gain 放不进量程时，G_full′（full.gainDb）回算成量程顶值下前向模型真能交付的净增益、置 rangeLimited（makeFullPlan 并进
        // constrained）——目标显示与校准目标不再宣称交付不了的净提升（原来照写请求：I −46 LUFS 请求 36 dB 时宣称 36，Gain 钳在 24、连 Glue
        // 补偿只交付得了约 30，校准目标永远追不上、每首都「部分校准」；手动 Gain + 自动请求超过 24 dB 时同样虚报）。算法 ≤ 7 的快照照旧：
        // 只钳折点顶值到 24、G_full′ 不动（逐位不变）。
        bool completeFullPlan (const Context& ctx, FullPlan& full, float gainDb, float limiterFraction) noexcept
        {
            full.gainDb = gainDb;
            full.rangeLimited = false;

            const auto color = estimateColor (ctx, full.glue, full.heat);
            const auto target = full.neutral.netDb + full.gainDb;
            const auto rangeAware = ctx.c->algorithmVersion >= heatAndGainRangeAlgorithmVersion;
            const auto capGain = rangeAware ? nonNegative (limitGainRangeDb - ctx.manualGainDb) : limitGainRangeDb;

            // Clip / 限幅器分工与 Gain 一起定点迭代：限幅器拿 λ 份，其余交给 Clip（太浅就不开）。量程内时 appliedGain 逐位等于
            // 原来的 nonNegative (gain)；超出量程时按量程顶值算超出量（Clip 阈值、折点与真能交付的 Gain 同一口径）。
            auto gain = target - color.loudnessDb;
            auto limiterGr = 0.0f, clipDepth = 0.0f;

            for (int i = 0; i < lossIterations; ++i)
            {
                const auto appliedGain = rangeAware ? juce::jmin (nonNegative (gain), capGain) : nonNegative (gain);
                const auto remaining = nonNegative (ctx.truePeakDb + color.peakDb + appliedGain - ctx.ceilingDb);
                limiterGr = juce::jmin (limiterFraction * remaining, remaining);
                clipDepth = remaining - limiterGr;

                if (clipDepth < ctx.c->clipMinimumDepthDb)
                {
                    clipDepth = 0.0f;
                    limiterGr = remaining;
                }

                gain = target - color.loudnessDb + limitLossDb (ctx, remaining, remaining > 0.0f ? limiterGr / remaining : 1.0f);
            }

            const auto fullGain = juce::jmin (gain, capGain);
            full.clip = 0.0f;
            full.clipRamp = 1.0f;

            if (clipDepth > 0.0f)
            {
                // 阈值 = Ceiling + 限幅器那一份；强度前段线性爬到这个参数：爬完之前峰值还到不了阈值的线性段，爬坡不削波。
                const auto threshold = ctx.ceilingDb + limiterGr;
                const auto startPeak = ctx.truePeakDb + full.neutral.color.peakDb;
                const auto endPeak = ctx.truePeakDb + color.peakDb + nonNegative (fullGain);
                const auto minimumRamp = clamp01 (ctx.c->clipRampMinFraction);

                full.clip = clipForThreshold (ctx, threshold);
                full.clipThresholdDb = threshold;
                full.clipRamp = clipRampFor (ctx, threshold, startPeak, endPeak, minimumRamp);
            }

            // 请求放不进量程（算法 8）：G_full′ = 量程顶值下的净增益（同一前向模型，含 Color 的贡献、Clip 与限幅损失）− 中性链；只降不升，
            // 标受约束。折点与目标随之按交付得了的量算（顶折点就是量程顶值，s = 1 时净增益 = 中性链 + G_full′，与校准目标同一口径）。
            // 判定「放不进」与不钳量程时逐位相同：上面的定点序列在越过 capGain 之前与原来一样，越过之后只会更大（损失随超出量单调）。
            if (rangeAware && gain > capGain)
            {
                full.rangeLimited = true;
                const auto deliverable = estimateLimit (ctx, color, capGain, full.clip, false).netDb - full.neutral.netDb;

                if (isFinite (deliverable) && deliverable < full.gainDb)
                    full.gainDb = deliverable;
            }

            // 折点：各强度下让净增益 = 中性链 + s × G_full′ 所需的 Gain（Glue / Heat / Clip 也按该强度），再取累计最大保证单调。
            full.limitGainDb[0] = 0.0f;
            full.limitGainDb[(size_t) gainKnots - 1] = nonNegative (fullGain);
            bool nonNegativeEverywhere = fullGain >= 0.0f;

            for (int k = 1; k < gainKnots - 1; ++k)
            {
                const auto s = (float) k / (float) (gainKnots - 1);
                const auto knotColor = estimateColor (ctx, full.glue * s, full.heat * s);
                const auto needed = solveLimitGain (ctx, knotColor, clipAtStrength (full, s, full.clipRamp), full.neutral.netDb + s * full.gainDb);

                nonNegativeEverywhere = nonNegativeEverywhere && needed >= -capToleranceDb;
                full.limitGainDb[(size_t) k] = clampTo (needed, full.limitGainDb[(size_t) k - 1], full.limitGainDb[(size_t) gainKnots - 1]);
            }

            return nonNegativeEverywhere;
        }

        FullPlan makeFullPlan (const Context& ctx, float requestedGainDb) noexcept
        {
            const auto& sc = ctx.sc;
            FullPlan full;
            full.requestedGainDb = requestedGainDb;
            full.neutral = estimateChain (ctx, Settings {});

            // Limit 旁通：没有限幅器兜底，自动不推响度（makePlan 标受手动限制）。
            // 请求为 0（已经够响）照样往下走：不提响，但输入自己的超限仍按风格分给各模块。
            if (ctx.limitBypassed)
                return full;

            // 削峰分摊：Heat / Clip / Limit 三项归一（Color 旁通时 Heat 不可用，剩下两项重新归一）。
            auto shareHeat = ctx.colorBypassed ? 0.0f : nonNegative (sc.shareHeat);
            auto shareClip = nonNegative (sc.shareClip);
            auto shareLimit = nonNegative (sc.shareLimit);
            const auto shareSum = shareHeat + shareClip + shareLimit;

            if (shareSum > 0.0f)
            {
                shareHeat /= shareSum;
                shareClip /= shareSum;
                shareLimit /= shareSum;
            }
            else
            {
                shareLimit = 1.0f;
            }

            const auto glueShare = nonNegative (sc.shareGlue) * ctx.glueLowRiskScale;
            const auto clipLimitShare = shareClip + shareLimit;
            const auto limiterFraction = clipLimitShare > 0.0f ? shareLimit / clipLimitShare : 1.0f;

            // ---- 1. 满请求下的超限估计与各模块容量 ----
            const auto excessAtRequest = nonNegative (ctx.truePeakDb + requestedGainDb - ctx.ceilingDb);

            // Clip / 限幅器能放下的 Limit 入口超出量 E_rem，折回到 E：E_rem = (Clip + Limit 份额) × E + 损失补偿 + Glue 抬高的峰均比
            // （补偿削峰损失而多加的 Gain 同样抬高峰值；Glue 让峰均比升高约 0.1 dB / dB GR，按 Glue 上限计，偏保守）。
            auto clipLimitCap = infinity;

            if (clipLimitShare > 0.0f)
            {
                const auto remainingCap = clipLimitCapacity (ctx, limiterFraction);
                const auto glueCrestRise = nonNegative (ctx.c->glueLoudnessPerGrDb - ctx.c->gluePeakPerGrDb)
                                           * (enforced (sc.glueGrMeanCapDb) ? sc.glueGrMeanCapDb : 0.0f);
                clipLimitCap = nonNegative (remainingCap - limitLossDb (ctx, remainingCap, limiterFraction) - glueCrestRise) / clipLimitShare;
            }

            // Heat 的容量取决于它的入口峰值，而入口峰值又含 Glue 的补偿（Glue 在 Heat 前面、它的量随超限估计变）：
            // 容量是超限估计的函数 cap(E)。
            const auto capacityAt = [&] (float excess) noexcept
            {
                auto cap = clipLimitCap;

                if (shareHeat > 0.0f)
                {
                    const HeatRow row (*ctx.c, ctx.truePeakDb + gluePeakDb (ctx, glueForExcess (ctx, glueShare, excess)));
                    const auto heatCrestCapacity = row.at (row.crest, heatCap (sc, row));
                    if (ctx.c->algorithmVersion >= 3)
                    {
                        // 并联 Heat 的份额是染色偏好，不是必须承担的削峰配额。安静输入可能几乎不削峰，
                        // 不能因此锁死仍有余量的限幅器。先保留 Clip/Limit 自身容量，再加 Heat 实际能承担的部分；
                        // 最终仍由真实前向模型和各模块预算检查约束，绝不假定 Heat 已完成请求的削峰量。
                        const auto downstreamCapacity = clipLimitShare > 0.0f ? clipLimitCap * clipLimitShare : 0.0f;
                        cap = downstreamCapacity + juce::jmin (shareHeat * excess, heatCrestCapacity);
                    }
                    else
                        cap = juce::jmin (cap, heatCrestCapacity / shareHeat);
                }

                return cap;
            };

            // ---- 2. 放不下就降低请求（不把负担甩给别的模块）：求 E = cap(E) 的不动点（从 cap(E_req) 起迭代 3 次；cap 只经
            //         Glue 补偿间接随 E 变，变化很慢）。不动点与请求大小无关，所以请求越多、得到的只会越多或一样多
            //         （置信度低 → 请求小 → 方案不会反而更大）。E ≤ 容量 ⟺ G ≤ 容量 + Ceiling − TP。 ----
            auto gain = requestedGainDb;
            auto excessCap = capacityAt (excessAtRequest);

            if (excessAtRequest > excessCap)
            {
                for (int i = 0; i < 3; ++i)
                    excessCap = capacityAt (juce::jmin (excessCap, excessAtRequest));

                gain = clampTo (excessCap + ctx.ceilingDb - ctx.truePeakDb, 0.0f, requestedGainDb);
                full.constrained = gain < requestedGainDb;
            }

            full.gainDb = gain;

            // 各模块按受约束的 E 换算参数；输入本身已超 Ceiling 太多（G 降到 0 仍放不下）时，各模块只拿容量内的份额。
            const auto excess = juce::jmin (nonNegative (ctx.truePeakDb + gain - ctx.ceilingDb), excessCap);

            // ---- 3. Glue、Heat，再定 Gain / Clip / 折点。Glue 的补偿比整条方案要的增益还多（偏响的素材）时，
            //         低强度下 Gain 就得是负数才能对上目标（补偿随强度线性、GR 要到后段才起来）→ 把 Glue 降到「补偿 ≤ 满强度增益」
            //         为止（密度让位于响度目标）。连续地降，不是「用 / 不用」的开关：开关在画像或置信度略变时翻转，方案会跳，
            //         还会让置信度低的方案反而用上 Glue（v0.6 验收 F-05）。----
            for (int attempt = 0; attempt < 2; ++attempt)
            {
                full.glue = attempt == 0 ? juce::jmin (glueForExcess (ctx, glueShare, excess), glueForMakeup (ctx, gain)) : 0.0f;
                full.heat = 0.0f;

                if (shareHeat > 0.0f && excess > 0.0f)
                {
                    const HeatRow row (*ctx.c, ctx.truePeakDb + gluePeakDb (ctx, full.glue));
                    const auto cap = heatCap (sc, row);

                    if (ctx.c->algorithmVersion >= heatAndGainRangeAlgorithmVersion)
                    {
                        // 算法 8（T-032 R6-01）：Heat 只承担它在这个入口电平、上限以内真能削掉的峰均比（row.at (crest, cap)，与 capacityAt
                        // 同一口径）。inverse 的「达不到就给表的最大 Heat」是旧削峰型 Heat 的兜底；算法 6 起的残差 Heat 表几乎不削峰
                        // （出厂表 crest 列 ≤ 0.00023 dB），那个兜底会让自动 Heat 与音乐无关地顶到 heatDriveCap（70 / 50 / 20）、在 E = 0
                        // 处跳 58 个单位，底量、shareHeat、heatPeakReductionCapDb 全部失效。能削的量不到 heatUsefulCrestDb（出厂 0.1 dB；
                        // ≤ 0 或非有限按「削不了」）→ 不为削峰开 Heat，交给下面的风格底量。
                        const auto achievable = row.at (row.crest, cap);
                        const auto useful = ctx.c->heatUsefulCrestDb;

                        if (useful > 0.0f && isFinite (useful) && achievable >= useful)
                            full.heat = juce::jmin (row.inverse (row.crest, juce::jmin (shareHeat * excess, achievable)), cap);
                    }
                    else
                    {
                        full.heat = juce::jmin (row.inverse (row.crest, shareHeat * excess), cap);   // 算法 ≤ 7 的快照：原样（旧工程逐位不变）
                    }
                }

                // 染色不只承担削峰：有余量时保留轻微 Heat（底量：算法 8 起是风格常数，按方案权重混合；旧快照三种风格都是原来的全局值）。
                // 先进入同一前向模型再求 Gain，不能在方案后面补加。
                // 若补偿会要求负 Gain，撤回底量，保留原有削峰分工（尤其是旧快照或已被保护层收紧到 0 的方案）。
                const auto peakHeat = full.heat;
                if (! ctx.colorBypassed && ctx.mix > 0.0f && isFinite (sc.automaticHeatFloor))
                {
                    const HeatRow row (*ctx.c, ctx.truePeakDb + gluePeakDb (ctx, full.glue));
                    full.heat = juce::jmax (peakHeat, juce::jmin (nonNegative (sc.automaticHeatFloor), heatCap (sc, row)));
                }

                if (full.heat > peakHeat)
                {
                    if (completeFullPlan (ctx, full, gain, limiterFraction))
                        break;
                    full.heat = peakHeat;
                }

                if (completeFullPlan (ctx, full, gain, limiterFraction) || ! (full.glue > 0.0f))
                    break;
            }

            // 量程回算（算法 8）只看最后采用的那一次 completeFullPlan：被撤回的尝试里置过的不留下（算法 ≤ 7 恒为 false，逐位不变）。
            full.constrained = full.constrained || full.rangeLimited;
            return full;
        }

        //==============================================================================
        // 方案在一组具体手动值下的展开：撤回了哪些模块、Clip 爬坡多长。
        struct Schedule
        {
            Violations withdrawn;
            float clipRamp = 1.0f;
            // 自动 Heat / Glue 在手动值之上还能加多少（各自的有效上限减去手动值，见 makePlan「逐模块钳住」）。
            float heatAllowance = 1.0e9f;
            float glueAllowance = 1.0e9f;
        };

        // 强度 s 下交给模块的自动增量（只含会动电平的 4 个）。
        Settings autoSettingsAt (const FullPlan& full, float strength, const Schedule& schedule) noexcept
        {
            Settings s;
            s.glue = schedule.withdrawn.glue ? 0.0f : juce::jmin (full.glue * strength, schedule.glueAllowance);
            s.heat = schedule.withdrawn.heat ? 0.0f : juce::jmin (full.heat * strength, schedule.heatAllowance);
            s.gainDb = limitGainAtStrength (full, strength);
            s.clip = schedule.withdrawn.clip ? 0.0f : clipAtStrength (full, strength, schedule.clipRamp);
            return s;
        }

        Settings combine (const Settings& manual, const Settings& automatic) noexcept
        {
            return { clampTo (manual.glue + automatic.glue, 0.0f, 100.0f),
                     clampTo (manual.heat + automatic.heat, 0.0f, 100.0f),
                     clampTo (manual.gainDb + automatic.gainDb, 0.0f, 24.0f),
                     clampTo (manual.clip + automatic.clip, 0.0f, 100.0f) };
        }

        bool feasible (const Context& ctx, const FullPlan& full, const Settings& manual, const Schedule& schedule, float strength) noexcept
        {
            const auto& withdrawn = schedule.withdrawn;
            const auto actual = combine (manual, autoSettingsAt (full, strength, schedule));

            // Heat 与 Glue 的上限不在这里核对：makePlan 已把它们逐模块钳住（自动部分只填到「有效上限 − 手动值」）。
            // 它们若进这里的可行性，就会拖着 Glue / Heat / Clip / Gain 共用的强度一起降：手动 Heat 接近上限时整个方案的
            // 响度被撤回，越过上限（改走撤回该模块）时又一步加回（v0.6 验收 F-02：整链输出跳 18 dB）。
            // 这里只核对随 Gain 一起走的 Clip 与限幅器；已撤回的模块不再核对（手动本身就超，撤回后与方案无关）。
            ChainEstimate estimate;
            estimate.color = estimateColor (ctx, actual.glue, actual.heat);
            const auto v = check (ctx, estimateLimit (ctx, estimate.color, actual.gainDb, actual.clip), full.neutral, false);
            return ! ((v.clip && ! withdrawn.clip) || v.limit);
        }

        // 叠加手动值后的最大可行强度 s_c，与 GLAZE 无关：s = 1 可行就取 1；否则在固定网格 [0, 1] 上二分（不变式：lo 可行、hi 不可行）。
        // 最终强度 = min(GLAZE × κ 落到网格上, s_c)——两者都与对方无关，所以随 GLAZE 严格单调，约束边界上也不会倒退。
        // 用量随强度单调时（方案按此构造：Clip 爬坡在峰值到阈值之前走完），s_c 就是真正的可行边界；手动设置很怪、
        // 用量不单调时，二分找到的是某一个边界（仍可行、偏保守），单调性照样成立。
        // lo = 0 一定可行：强度 0 时实际值就是手动值，手动本身超约束的模块已撤回、不再核对，Limit 超约束已在调用前处理。
        float strengthBound (const Context& ctx, const FullPlan& full, const Settings& manual, const Schedule& schedule) noexcept
        {
            if (feasible (ctx, full, manual, schedule, 1.0f))
                return 1.0f;

            int lo = 0, hi = boundGridSteps;

            while (hi - lo > 1)
            {
                const auto mid = (lo + hi) / 2;

                if (feasible (ctx, full, manual, schedule, (float) mid / (float) boundGridSteps))
                    lo = mid;
                else
                    hi = mid;
            }

            return (float) lo / (float) boundGridSteps;
        }

        // 强度落到网格上（向下取整）：与 strengthBound 同一网格，二者取小不会因舍入差一格而倒退。
        float toGrid (float strength) noexcept
        {
            return std::floor (clamp01 (strength) * (float) strengthGridSteps) / (float) strengthGridSteps;
        }

        //==============================================================================
        // 证据的置信加权均方根（dB）。
        float evidenceScore (const MusicProfile& p, const std::array<float, (size_t) numBands>& evidence) noexcept
        {
            float weighted = 0.0f, weights = 0.0f;

            for (size_t b = 0; b < (size_t) numBands; ++b)
            {
                const auto w = clamp01 (p.bandConfidence[b]);
                const auto e = isFinite (evidence[b]) ? nonNegative (evidence[b]) : 0.0f;
                weighted += w * e * e;
                weights += w;
            }

            return weights > 0.0f ? std::sqrt (weighted / weights) : 0.0f;
        }

        float meanBandConfidence (const MusicProfile& p) noexcept
        {
            float sum = 0.0f;

            for (auto w : p.bandConfidence)
                sum += clamp01 (w);

            return sum / (float) numBands;
        }

        // 特征的可信度 0 ～ 1（到 fullConfidence 算满）；无效、非有限值 → 0。
        float trustOf (const MusicProfile* p, const Feature MusicProfile::* member, float fullConfidence) noexcept
        {
            if (p == nullptr)
                return 0.0f;

            const auto& f = p->*member;

            if (! f.valid || ! isFinite (f.value))
                return 0.0f;

            return clamp01 (f.confidence / fullConfidence);
        }

        float valueOr (const MusicProfile* p, const Feature MusicProfile::* member, float fallback) noexcept
        {
            if (p == nullptr)
                return fallback;

            const auto& f = p->*member;
            return f.valid && isFinite (f.value) ? f.value : fallback;
        }

        //==============================================================================
        // 修坯 Clay（v0.1.7.0，算法 7；设计规格-Clay-分析决策与验收第 2 节）。

        // 规格记号里的斜坡：ramp(x, x0, x1) = clamp((x − x0) / (x1 − x0), 0, 1)，x1 < x0 即反向斜坡（低频相关度 0.6 → 0、
        // 高频侧中比 −10 → −18 dB 都是「越低越需要」）。与上面的 ramp 不同：两端重合或跨度非有限（坏快照；旧快照缺字段按 0 读时
        // 两端都是 0）→ 0，即「不需要」、该项中性，不当成阶跃；x 为 NaN 同样 → 0。
        float clayRamp (float x, float from, float to) noexcept
        {
            const auto span = to - from;

            if (span == 0.0f || ! isFinite (span))
                return 0.0f;

            return clamp01 ((x - from) / span);
        }

        // 加宽上限（第 2 节③）：画像的高频侧中比 r = P_S / P_M = 10^(dB/10)，加宽 γ = 10^(w/20) 之后预测的相关度
        // ρ' = (P_M − γ²P_S) / (P_M + γ²P_S) = (1 − γ²r) / (1 + γ²r)。ρ' ≥ ρ_min ⟺ γ² ≤ k / r，k = (1 − ρ_min) / (1 + ρ_min)
        // （ρ_min = 0.25 → k = 0.6）⟺ w ≤ 10·log10(k) − 侧中比 dB。在 dB 域算：极窄的高频（侧中比 −100 dB）不会让 10^(dB/10) 下溢。
        // ρ_min ≥ 1 或非有限 → 0（一点都不许加）；ρ_min ≤ −1 → 不设上限。结果可能为负，由调用方与下限 0 取大。
        // 出厂常数下它其实碰不到：要加宽得侧中比 < −10 dB，此时上限 > 10·log10(0.6) + 10 = 7.8 dB，远大于 clayWidthMaxDb 1.5 dB；
        // 留着给标定改阈值时兜底（ClayGuard 用同一个式子、换成实时的 P_M / P_S 做实时保护）。
        float clayWidthCapDb (float sideToMidDb, float minCorrelation) noexcept
        {
            const auto k = clayWidthPowerScale (minCorrelation);   // GlazeConstants.h：与 ClayGuard 同一处

            if (! (k > 0.0))
                return 0.0f;

            if (std::isinf (k))
                return infinity;

            return static_cast<float> (10.0 * std::log10 (k)) - sideToMidDb;
        }

        //==============================================================================
        // 先试后用（ADR-012，设计规格-Clay-根治修订 3.2 / 3.3）：分析器在学习区间里把三项的候选处理跑在原始输入上，量 100 ms
        // 块峰值 P99 的变化（代价，dB，正 = 峰值变高），随画像保存；决策器按代价给每项乘一个系数 f ∈ [0, 1]。

        // 代价特征能用：有效且数值有限。置信度不参与——代价只回答「这首歌真做了峰值会不会变高」，分析器有效块满 10 s 才给
        // valid（GlazeTypes.h MusicProfile）；需要度那一侧的可信度 c 已按置信度收紧。
        bool usableCost (const Feature& cost) noexcept
        {
            return cost.valid && isFinite (cost.value);
        }

        // 代价系数 f(代价) = ramp(代价, clayCostZeroDb, clayCostFullDb)（反向斜坡：出厂 ≤ 0.2 dB → 1 照做、≥ 0.6 dB → 0 不做、其间线性；
        // 峰值反而变低的负代价 → 1）。只认真正的反向斜坡（两端有限且 Zero > Full），否则返回 false =「判断不了」：旧快照缺字段读成
        // 0 / 0（v0.1.7.0 开发期的快照有 clay* 却没有代价常数）、两端颠倒（会变成代价越大越照做）、非有限。调用方把它与「代价特征
        // 无效」同样处理——该项中性、不标代价位：宁可不做，也不按判断不了的结果去做，界面也不说「试过会抬高峰值」。
        bool costFactorOf (float costDb, const Constants& c, float& factor) noexcept
        {
            const auto zeroDb = c.clayCostZeroDb;
            const auto fullDb = c.clayCostFullDb;

            if (! isFinite (costDb) || ! isFinite (zeroDb) || ! isFinite (fullDb) || ! (zeroDb > fullDb))
                return false;

            factor = clamp01 ((costDb - zeroDb) / (fullDb - zeroDb));
            return true;
        }

        // 守护混合损失的低切频率封顶（T-043，ADR-027）。画像里两个候选频率（clayBlendCandidateHz：10 / 20 Hz）的混合损失 L₁、L₂（dB；
        // 两个都有效且有限才用，少一个就判断不了）。预测 L(f)：两个都 > 0 → 幂律 L₁·(f / f₁)^q，q = ln(L₂ / L₁) / ln(f₂ / f₁)（log-log
        // 直线，理由见 GlazeTypes.h clayBlendCandidateHz）；否则（有 ≤ 0 的：几乎没损失或量化噪声）→ L 在 ln f 上的直线。两种都对 f 单调，
        // 两端之外照同一条线外推。门槛 limit：地板处预测已超 → 0（不低切）；hz 处没超 → hz 原样（逐位）；否则解出预测恰为 limit 的频率。
        // 「越过点」只取决于画像、门槛与地板，结果 = min(hz, 越过点)，随 hz 单调不减（planClay 的 GLAZE 单调保留）。门槛 ≤ 0 / 非有限、
        // 地板 ≤ 0 / 非有限 → 判断不了 → hz 原样。double 里算，不分配。
        float capLowCutForBlend (const MusicProfile& p, float hz, float floorHz, float limitDb) noexcept
        {
            if (! (limitDb > 0.0f) || ! isFinite (limitDb) || ! (floorHz > 0.0f) || ! isFinite (floorHz) || ! (hz > 0.0f))
                return hz;

            const auto& first = p.lowCutBlendLossDb[0];
            const auto& second = p.lowCutBlendLossDb[1];

            if (! first.valid || ! second.valid || ! isFinite (first.value) || ! isFinite (second.value))
                return hz;

            const auto x1 = std::log (static_cast<double> (clayBlendCandidateHz[0]));
            const auto x2 = std::log (static_cast<double> (clayBlendCandidateHz[1]));
            const auto l1 = static_cast<double> (first.value);
            const auto l2 = static_cast<double> (second.value);
            const auto powerLaw = l1 > 0.0 && l2 > 0.0;

            // 预测按 u(L) 在 x = ln f 上的直线：幂律 u = ln L，否则 u = L。
            const auto u1 = powerLaw ? std::log (l1) : l1;
            const auto u2 = powerLaw ? std::log (l2) : l2;
            const auto slope = (u2 - u1) / (x2 - x1);
            const auto limit = static_cast<double> (limitDb);
            const auto uLimit = powerLaw ? std::log (limit) : limit;
            const auto predicted = [&] (double x) noexcept { return u1 + slope * (x - x1); };

            const auto from = std::log (static_cast<double> (floorHz));
            const auto to = std::log (static_cast<double> (hz));

            if (predicted (from) > uLimit)
                return 0.0f;

            if (! (to > from) || ! (predicted (to) > uLimit))   // predicted(from) ≤ uLimit < predicted(to) ⟹ slope > 0
                return hz;

            return static_cast<float> (std::exp (x1 + (uLimit - u1) / slope));
        }

        // 修坯 Clay 的自动值（第 2 节，ADR-012 / 根治修订 3.3 修订）。g = GLAZE / 100（> 0）；c = 各项需要度特征的可信度（trustOf：
        // 到 fullConfidence 算满，与 Tone、Glue 低频风险同一口径；分析器给的置信度已含代表性）；f_t、s_w 取 sc（blendConstants 按
        // 方案的风格权重混合，与其他各项同一份）；f(代价) 见 costFactorOf。
        //
        //   ① 低切：need = ramp(subShareDb, −40, −28)（真次声 < 16 Hz 的能量占比）；
        //          k = need · c · min(1, g / clayGlazeFullAt) · f(lowCutPeakCostDb)（清理不是口味项，GLAZE 50 起给满）；
        //          k ≤ clayLowCutMinK 或 f_t ≤ 地板 → 关（0）；否则 lowCutHz = 地板 · (f_t / 地板)^k（地板 10 Hz：k 刚过门槛时从 0
        //          跳到略高于 10 Hz，10 Hz 的 4 阶高通在 20 Hz 只衰减 0.02 dB，起步无感）；f_t 出厂 16 / 18 / 20 Hz；
        //          再按守护混合损失封顶（T-043，capLowCutForBlend）：预测损失超门槛的频率不用，封到越过点、地板处已超就关，置代价位；
        //   ② 收单：自动态恒为中性（ADR-015：收单只改立体声、单声道回放不变，「为单声道回放收单」不成立）——monoAmount 0、分频点
        //          ClayPlan 的默认值，与 ClayPlan{} 逐位相同；画像里的收单代价（monoPeakCostDb）照测照存，决策不用；
        //   ③ 加宽：need = ramp(highSideToMidDb, −10, −18)，s_w 按 0 ～ 1 用；原公式 clayWidthMaxDb · need · c · s_w · g（口味项，
        //          随 g 线性）与 clayWidthCapDb 取小、钳到 [0, clayWidthMaxDb]（自动只加宽、不收窄），再 × f(widthPeakCostDb)。
        //
        // 代价的两条规矩：代价特征无效（格式 1 旧画像没有、候选的有效块不满 10 s）或代价常数判断不了 → 该项中性（没试过就不用）；
        // 某项「按需要度本应动作（代价之前的值 > 0）、但 f < 1 让它减量或否决」→ costLimitedMask 置该项的位（只给界面解释，
        // 不影响处理）。没试过、或本来就不需要的项不置位。f = 1 时三项与不乘 f 逐位相同（乘 1 精确）。
        //
        // 单调：三项都是「与 g 无关的非负量 × g 的单调函数」（float 乘除对固定非负因子保序；f 只看画像与常数、与 g 无关；低切的幂在
        // double 里算，底 > 1 时随 k 严格增，回到 float 仍保序——float 的 powf 只保证误差、不保证单调），再与「与 g 无关的上限」取小
        // → 随 GLAZE 单调不减。所需特征无效或不可信（c = 0）→ 该项中性。只做定长算术，不分配。常数的约定见 GlazeConstants.h 的
        // clay* 段（旧快照全按 0 读时三项全部中性）。
        ClayPlan planClay (const MusicProfile* profile, const Constants& c, const StyleConstants& sc, float glaze, float fullConfidence) noexcept
        {
            ClayPlan clay;

            if (profile == nullptr || ! (glaze > 0.0f))
                return clay;

            const auto& p = *profile;
            auto limited = std::uint8_t { 0 };   // costLimitedMask：因代价被减量 / 否决的项
            const auto markLimited = [&limited] (std::uint8_t bit) noexcept { limited = static_cast<std::uint8_t> (limited | bit); };

            // 清理项（低切、收单）的 GLAZE 强度；clayGlazeFullAt ≤ 0 或非有限 = 不自动清理。
            const auto fullAt = c.clayGlazeFullAt;
            const auto cleanupStrength = fullAt > 0.0f && isFinite (fullAt) ? juce::jmin (1.0f, glaze / fullAt) : 0.0f;

            // ---- ① 低切 ----
            {
                const auto trust = trustOf (profile, &MusicProfile::subShareDb, fullConfidence);
                const auto wanted = clayRamp (p.subShareDb.value, c.claySubShareLowDb, c.claySubShareHighDb) * trust * cleanupStrength;
                const auto floorHz = c.clayLowCutFloorHz;
                const auto targetHz = sc.clayLowCutTargetHz;
                const auto minK = c.clayLowCutMinK;

                // 「开着」= k 过门槛；k > 0 单列：门槛被标成负数（坏快照）时，特征无效（k = 0）也不许开到地板频率。
                const auto engaged = [minK] (float candidateK) noexcept { return candidateK > 0.0f && candidateK > minK; };
                auto factor = 0.0f;

                // 代价之前就开着（且风格目标高于地板）才谈得上「因代价减量 / 否决」；代价用不了 → 中性、不置位。
                if (engaged (wanted) && floorHz > 0.0f && targetHz > floorHz && isFinite (targetHz)
                    && usableCost (p.lowCutPeakCostDb) && costFactorOf (p.lowCutPeakCostDb.value, c, factor))
                {
                    if (factor < 1.0f)
                        markLimited (clayCostLimitedLowCut);

                    const auto k = wanted * factor;

                    if (engaged (k))
                    {
                        const auto hz = static_cast<double> (floorHz)
                                      * std::pow (static_cast<double> (targetHz) / static_cast<double> (floorHz), static_cast<double> (k));
                        auto lowCutHz = clampTo (static_cast<float> (hz), ClayModule::minLowCutHz, ClayModule::maxLowCutHz);

                        // 守护混合损失封顶（T-043）：没越过门槛时原样（逐位）；封到模块下限以下就关。
                        const auto capped = capLowCutForBlend (p, lowCutHz, floorHz, c.clayBlendLossLimitDb);

                        if (capped < lowCutHz)
                        {
                            markLimited (clayCostLimitedLowCut);
                            lowCutHz = capped >= ClayModule::minLowCutHz ? capped : 0.0f;
                        }

                        clay.lowCutHz = lowCutHz;
                    }
                }
            }

            // ---- ② 收单：自动态不做（ADR-015）----
            // 收单只去掉低频的侧声道 S；单声道回放听的是 M = (L + R) / 2，收不收单都一样——「单声道回放丢低音」救不回来，立体声听众
            // 却丢了低音（真歌实测：l_kskdhjf 平均 −2.4 dB、最多 −7.6 dB；will we be okay 的反相段 −1 ～ −2.5 dB，单声道只差 0.04 dB）。
            // 所以自动方案的收单恒为中性（monoAmount 0、分频点默认）；手动收单照旧（ClayGuard.h resolveClayParams），作音色工具用。

            // ---- ③ 加宽 ----
            {
                const auto maxDb = c.clayWidthMaxDb;
                const auto trust = trustOf (profile, &MusicProfile::highSideToMidDb, fullConfidence);
                auto factor = 0.0f;

                if (maxDb > 0.0f && isFinite (maxDb) && trust > 0.0f)
                {
                    const auto sideToMidDb = p.highSideToMidDb.value;   // trust > 0 ⟹ 有效且有限
                    const auto wanted = maxDb * clayRamp (sideToMidDb, c.clayWidthNarrowStartDb, c.clayWidthNarrowFullDb) * trust
                                        * clamp01 (sc.clayWidthStyleFactor) * glaze;
                    const auto capped = juce::jmin (wanted, clayWidthCapDb (sideToMidDb, c.clayMinCorrelationAfterWidth));
                    const auto original = clampTo (capped, 0.0f, juce::jmin (maxDb, ClayModule::maxWidthDb));   // 代价之前的原公式

                    if (original > 0.0f && usableCost (p.widthPeakCostDb) && costFactorOf (p.widthPeakCostDb.value, c, factor))
                    {
                        if (factor < 1.0f)
                            markLimited (clayCostLimitedWidth);

                        clay.widthDb = original * factor;
                    }
                }
            }

            // 上面每一步都只用有限值（有效特征、有限常数）；这里只兜底：万一出了非有限值，该项回中性，不许流进模块参数。
            if (! isFinite (clay.lowCutHz))
                clay.lowCutHz = 0.0f;

            if (! isFinite (clay.widthDb))
                clay.widthDb = 0.0f;

            if (! isFinite (clay.monoAmount) || ! isFinite (clay.monoHz))
            {
                clay.monoAmount = 0.0f;
                clay.monoHz = ClayPlan {}.monoHz;
            }

            clay.costLimitedMask = limited;
            return clay;
        }
    }

    //==============================================================================
    StyleWeights resolveAutomaticWeights (const MusicProfile& p, const Constants& c) noexcept
    {
        const auto full = c.fullConfidence > 0.0f ? c.fullConfidence : 1.0f;

        const auto strengthTrust = trustOf (&p, &MusicProfile::onsetStrengthDb, full);
        const auto densityTrust = trustOf (&p, &MusicProfile::onsetDensityPerSecond, full);
        const auto lraTrust = trustOf (&p, &MusicProfile::loudnessRangeLu, full);
        const auto slopeTrust = trustOf (&p, &MusicProfile::spectralSlopeDbPerOct, full);

        // 特征不可信时按「没有这个特征」算（冲劲 0、动态 0、不偏亮），再整体按置信度偏向通透。
        const auto transient = ramp (valueOr (&p, &MusicProfile::onsetStrengthDb, 0.0f), c.autoTransientStrengthLowDb, c.autoTransientStrengthHighDb)
                             * ramp (valueOr (&p, &MusicProfile::onsetDensityPerSecond, 0.0f), c.autoTransientDensityLow, c.autoTransientDensityHigh)
                             * juce::jmin (strengthTrust, densityTrust);
        const auto dynamic = ramp (valueOr (&p, &MusicProfile::loudnessRangeLu, 0.0f), c.autoDynamicLraLowLu, c.autoDynamicLraHighLu) * lraTrust;
        const auto bright = ramp (valueOr (&p, &MusicProfile::spectralSlopeDbPerOct, -100.0f) - c.clean.toneReferenceSlopeDbPerOct,
                                  0.0f, c.autoBrightRangeDbPerOct) * slopeTrust;

        // 冲击：鼓点驱动（有冲劲且密），动态大时打折（留给通透）；通透：动态大；温润：不靠鼓点、动态不大的持续型素材，偏亮的加一点。
        const auto base = nonNegative (c.autoBaseWeight);
        const StyleWeights raw { base + (1.0f - transient) * (1.0f - dynamic) * nonNegative (c.autoWarmSustainWeight)
                                      + nonNegative (c.autoWarmBrightWeight) * bright,
                                 base + transient * (1.0f - clamp01 (c.autoPunchyDynamicDamping) * dynamic),
                                 base + dynamic };
        const auto mixed = sanitise (raw);

        // 起音强度与密度都可信才允许明确的音乐取向；没有可靠起音证据仍退回通透。
        // LRA 的观测窗长达 60 s，而首次画像在 15～30 s 就提交；它只约束上面的 dynamic 项，
        // 不能再全局否决已经可靠的起音证据，否则自动风格会把早期的通透偏置永久锁住。
        const auto confidence = juce::jmin (strengthTrust, densityTrust);

        return sanitise ({ mixed.warm * confidence,
                           mixed.punchy * confidence,
                           mixed.clean * confidence + (1.0f - confidence) });
    }

    StyleWeights weightsFor (Style style, const StyleWeights& automaticWeights) noexcept
    {
        switch (style)
        {
            case Style::warm:      return { 1.0f, 0.0f, 0.0f };
            case Style::punchy:    return { 0.0f, 1.0f, 0.0f };
            case Style::clean:     return { 0.0f, 0.0f, 1.0f };
            case Style::automatic:
            default:               return sanitise (automaticWeights);
        }
    }

    StyleConstants blendConstants (const Constants& c, const StyleWeights& weights) noexcept
    {
        const auto w = sanitise (weights);

        // 独热权重直接返回那一种风格（逐位相同，不经 0 × x 的运算）。
        if (w.warm == 1.0f)    return c.warm;
        if (w.punchy == 1.0f)  return c.punchy;
        if (w.clean == 1.0f)   return c.clean;

        const auto blend = [&w] (float a, float b, float d) { return w.warm * a + w.punchy * b + w.clean * d; };
        const auto& a = c.warm;
        const auto& b = c.punchy;
        const auto& d = c.clean;

        StyleConstants s;
        s.preferredLufs = blend (a.preferredLufs, b.preferredLufs, d.preferredLufs);
        s.heatDriveCap = blend (a.heatDriveCap, b.heatDriveCap, d.heatDriveCap);
        s.glueGrMeanCapDb = blend (a.glueGrMeanCapDb, b.glueGrMeanCapDb, d.glueGrMeanCapDb);
        s.glueGrP95CapDb = blend (a.glueGrP95CapDb, b.glueGrP95CapDb, d.glueGrP95CapDb);
        s.clipDepthCapDb = blend (a.clipDepthCapDb, b.clipDepthCapDb, d.clipDepthCapDb);
        s.clipActivityCap = blend (a.clipActivityCap, b.clipActivityCap, d.clipActivityCap);
        s.limitGrMeanCapDb = blend (a.limitGrMeanCapDb, b.limitGrMeanCapDb, d.limitGrMeanCapDb);
        s.limitGrP95CapDb = blend (a.limitGrP95CapDb, b.limitGrP95CapDb, d.limitGrP95CapDb);
        s.limitGrPeakCapDb = blend (a.limitGrPeakCapDb, b.limitGrPeakCapDb, d.limitGrPeakCapDb);
        s.recoverCap = blend (a.recoverCap, b.recoverCap, d.recoverCap);
        s.tameCap = blend (a.tameCap, b.tameCap, d.tameCap);
        s.toneMaxDb = blend (a.toneMaxDb, b.toneMaxDb, d.toneMaxDb);
        s.shareHeat = blend (a.shareHeat, b.shareHeat, d.shareHeat);
        s.shareGlue = blend (a.shareGlue, b.shareGlue, d.shareGlue);
        s.shareClip = blend (a.shareClip, b.shareClip, d.shareClip);
        s.shareLimit = blend (a.shareLimit, b.shareLimit, d.shareLimit);
        s.heatPeakReductionCapDb = blend (a.heatPeakReductionCapDb, b.heatPeakReductionCapDb, d.heatPeakReductionCapDb);
        s.toneReferenceSlopeDbPerOct = blend (a.toneReferenceSlopeDbPerOct, b.toneReferenceSlopeDbPerOct, d.toneReferenceSlopeDbPerOct);
        // v0.1.7.0 修坯 Clay：f_t = Σ w·低切目标、s_w = Σ w·加宽系数（设计规格-Clay-分析决策与验收第 2 节①③）。
        s.clayLowCutTargetHz = blend (a.clayLowCutTargetHz, b.clayLowCutTargetHz, d.clayLowCutTargetHz);
        s.clayWidthStyleFactor = blend (a.clayWidthStyleFactor, b.clayWidthStyleFactor, d.clayWidthStyleFactor);
        // 算法 8（T-032 R6-01）：自动 Heat 底量。三种风格逐位相同时直接取那一份、不经加权求和：v7 及以前的快照只有一个全局底量，
        // 读回时三种风格都是它，混合权重下也必须逐位等于原值（Σ w × 12 在 float 里不一定正好是 12；旧工程逐位不变）；出厂 12 / 12 / 12
        // 同理。三者不同（Max 标定后）才按权重线性混合。
        s.automaticHeatFloor = sameBits (a.automaticHeatFloor, b.automaticHeatFloor) && sameBits (a.automaticHeatFloor, d.automaticHeatFloor)
                                   ? a.automaticHeatFloor
                                   : blend (a.automaticHeatFloor, b.automaticHeatFloor, d.automaticHeatFloor);
        return s;
    }

    //==============================================================================
    float confidenceScale (const MusicProfile* profile, const Constants& c) noexcept
    {
        // κ = 底强度 + (1 − 底强度) × 置信度；置信度 = 输入响度、真峰值、代表性三者里最没把握的那个（到 fullConfidence 算满）。
        const auto full = c.fullConfidence > 0.0f ? c.fullConfidence : 1.0f;
        const auto representativeness = profile != nullptr && isFinite (profile->representativeness)
                                             ? clamp01 (profile->representativeness / full) : 0.0f;
        const auto confidence = juce::jmin (juce::jmin (trustOf (profile, &MusicProfile::integratedLufs, full),
                                                        trustOf (profile, &MusicProfile::truePeakDbtp, full)),
                                            representativeness);
        const auto floorStrength = clamp01 (c.unlearnedStrength);
        return floorStrength + (1.0f - floorStrength) * confidence;
    }

    //==============================================================================
    // 方案的风格权重：离散风格独热；「自动」用锁定的权重，没锁时按画像判断（置信不足偏向通透）。
    static StyleWeights planWeights (const PlanInputs& inputs, const Constants& c) noexcept
    {
        StyleWeights automaticWeights { 0.0f, 0.0f, 1.0f };

        if (inputs.lockedWeights != nullptr)
            automaticWeights = *inputs.lockedWeights;
        else if (inputs.profile != nullptr)
            automaticWeights = resolveAutomaticWeights (*inputs.profile, c);

        const auto styleIndex = (int) inputs.style;
        const auto style = styleIndex >= (int) Style::automatic && styleIndex <= (int) Style::clean ? inputs.style : Style::automatic;
        return weightsFor (style, automaticWeights);
    }

    // 决策用的环境：混合后的风格常数、按置信度收拢过的特征、用户的非 GLAZE 设置。makePlanAtConfidence 与 estimateManualChainUsage
    // 共用这一份构造（T-032 A11），两处的手动链估计逐位相同；ctx.manualGainDb 由调用方按手动值另填。
    static Context planningContext (const PlanInputs& inputs, const Constants& c, const StyleWeights& weights) noexcept
    {
        const auto* profile = inputs.profile;
        const auto& manual = inputs.manual;
        const auto defaultInputLufs = isFinite (c.unlearnedInputLufs) ? c.unlearnedInputLufs : factoryConstants().unlearnedInputLufs;
        const auto measuredInputLufs = valueOr (profile, &MusicProfile::integratedLufs, defaultInputLufs);

        // ---- 环境：特征按置信度向保守默认值收拢 ----
        Context ctx;
        ctx.c = &c;
        ctx.sc = blendConstants (c, weights);
        ctx.glue.slope = c.glueRatio > 1.0f ? 1.0f - 1.0f / c.glueRatio : 0.0f;
        ctx.glue.kneeWidth = nonNegative (c.glueKneeDb);

        const auto full = c.fullConfidence > 0.0f ? c.fullConfidence : 1.0f;
        const auto loudnessTrust = trustOf (profile, &MusicProfile::integratedLufs, full);

        // 置信不足时向保守默认值收拢（分析器第 6 节），只动输入响度一项、且只往「更响」一侧收：请求增益只会变小，
        // 超限估计 E = TP + G − Ceiling 与受约束的增益 min(G, 容量 + Ceiling − TP) 也就只会变小——置信度低的方案，
        // 净增益与各模块处理量都不比置信度满时大，再乘 κ 就是按比例收紧（决策与校准第 2 节）。
        //   - 真峰值测到了就照用，不收拢：往高收会压低受约束的增益、却放大超限估计与削峰量；往低收正相反。两头都不保守。
        //     没测到时取「输入响度 + 默认 PLR」（假设已经偏密：处理少，增益由强度 κ 收紧）；
        //   - LRA 测到了就照用（它只会压低目标，本来就是保守的一侧），没测到按参考值（不修正）。
        ctx.inputLufs = clampTo (measuredInputLufs + (1.0f - loudnessTrust) * nonNegative (defaultInputLufs - measuredInputLufs), -70.0f, 0.0f);
        ctx.truePeakDb = clampTo (valueOr (profile, &MusicProfile::truePeakDbtp, ctx.inputLufs + nonNegative (c.unlearnedPlrDb)), -70.0f, 12.0f);
        ctx.ceilingDb = isFinite (manual.limit.ceilingDb) ? clampTo (manual.limit.ceilingDb, -4.5f, 0.0f)
                                                          : clampTo (c.ceilingForEstimateDb, -3.0f, 0.0f);
        ctx.detectorDb = ctx.inputLufs + c.glueDetectorOffsetDb;
        ctx.mix = isFinite (manual.color.mixPercent) ? clamp01 (manual.color.mixPercent * 0.01f) : 1.0f;
        ctx.colorOutputDb = isFinite (manual.color.outputDb) ? clampTo (manual.color.outputDb, -6.0f, 6.0f) : 0.0f;
        ctx.colorBypassed = manual.color.bypass;
        ctx.limitBypassed = manual.limit.bypass;
        ctx.clipKneeGapDb = c.clipKneeRatio > 0.0f && c.clipKneeRatio < 1.0f ? -gainToDb (c.clipKneeRatio) : 0.0f;

        // Glue 的低频风险（分析器第 4 节）：低频占比高、且低频逐帧起伏大（间歇的重底鼓）时自动 Glue 份额打折，持续低音不折。
        // 只会减少 Glue 的量、与 GLAZE 无关，所以不影响单调与上限；特征不可信时风险按可信度收小（处理量本来就被 κ 收紧）。
        {
            const auto shareTrust = trustOf (profile, &MusicProfile::lowShare, full);
            const auto rangeTrust = trustOf (profile, &MusicProfile::lowBandRangeDb, full);
            const auto lowRisk = ramp (valueOr (profile, &MusicProfile::lowShare, 0.0f), c.glueLowRiskShareLow, c.glueLowRiskShareHigh)
                               * ramp (valueOr (profile, &MusicProfile::lowBandRangeDb, 0.0f), c.glueLowRiskRangeLowDb, c.glueLowRiskRangeHighDb)
                               * juce::jmin (shareTrust, rangeTrust);
            ctx.glueLowRiskScale = 1.0f - clamp01 (c.glueLowRiskMaxCut) * lowRisk;
        }

        return ctx;
    }

    // 手动值里会动电平的 4 个参数（钳到参数范围；非有限按 0）：「只有手动值」的链就按它估。
    static Settings manualSettingsOf (const ChainParams& manual) noexcept
    {
        return { isFinite (manual.color.glue) ? clampTo (manual.color.glue, 0.0f, 100.0f) : 0.0f,
                 isFinite (manual.color.heat) ? clampTo (manual.color.heat, 0.0f, 100.0f) : 0.0f,
                 isFinite (manual.limit.gainDb) ? clampTo (manual.limit.gainDb, 0.0f, 24.0f) : 0.0f,
                 isFinite (manual.limit.clip) ? clampTo (manual.limit.clip, 0.0f, 100.0f) : 0.0f };
    }

    // 手动单独已超预算的项（Plan::manualBudgetMask 的位，GlazeTypes.h；T-032 R6-02 收尾，A11）。manual = 手动值里动电平的 4 个参数；
    // withdrawn = 决策器对「只有手动值的链」的约束核对（makePlanAtConfidence 的 check：Clip 这一项与 heatBudget 无关）。
    //   - Heat：手动 Heat > 风格的驱动上限（旋钮单位；上游监视按同一上限换算成驱动量）。这时 heatCap(sc, row) ≤ 驱动上限 < 手动值，
    //     自动 Heat 的余量为 0（被撤回同样是 0）。不看 heatBudget：自动没在用 Heat 时 check 不撤回、也不标受手动限制，但 Heat 驱动
    //     照样全是手动的，上游监视照样会把它当成超限（例如 Color Mix 0 时手动 Heat 50：修前每「过渡 + 3 s」白降 1 dB）；
    //   - Glue：手动 Glue 超过 glueCap（这份素材上两个 GR 上限都不超的最大 Glue）——即手动链单独的 Glue 均值 / p95 GR 有一项超过风格上限
    //     （不加容差）。判定直接用算自动余量的同一个 glueCap：余量 = max(0, glueCap − 手动) 恰为 0，撤回的更是 0，所以置了位自动 Glue 就
    //     恰为 0，不改方案的任何输出。与「手动单独 GR > 上限」只在上限边界的浮点舍入（10⁻⁶ dB 量级）上可能差一格；
    //   - Clip：决策器撤回了自动 Clip（withdrawn.clip，带容差）——只有这时它才不按 Clip 预算核对这份方案（feasible 跳过已撤回的模块，
    //     自动 Gain 只受限幅器约束），保护层与它一致；没撤回时方案按 Clip 预算核对过，保护层照样按风格上限核对。
    //   三项都与 GLAZE、约束反馈上限无关；手动中性（Glue / Heat / Clip 为 0）时恒为 0。
    static std::uint8_t manualBudgetMaskFor (const Context& ctx, const Settings& manual, const Violations& withdrawn) noexcept
    {
        const auto& sc = ctx.sc;
        auto mask = std::uint8_t { 0 };

        if (enforced (sc.heatDriveCap) && manual.heat > sc.heatDriveCap)
            mask = static_cast<std::uint8_t> (mask | manualBudgetHeat);

        if (manual.glue > glueCap (ctx))
            mask = static_cast<std::uint8_t> (mask | manualBudgetGlue);

        if (withdrawn.clip)
            mask = static_cast<std::uint8_t> (mask | manualBudgetClip);

        return mask;
    }

    //==============================================================================
    // 按画像此刻的置信度生成方案（makePlan 再用「置信度拉满」的同一份方案给它封顶）。
    static Plan makePlanAtConfidence (const PlanInputs& inputs) noexcept
    {
        const auto& c = inputs.constants != nullptr ? *inputs.constants : factoryConstants();
        const auto* profile = inputs.profile;
        const auto& manual = inputs.manual;

        // ---- 风格 ----
        Plan plan;
        plan.weights = planWeights (inputs, c);
        plan.algorithmVersion = currentAlgorithmVersion;
        plan.constantsVersion = c.version;

        // 显示用的输入响度：画像测到了就用测到的（不管置信度），否则保守默认值。
        const auto defaultInputLufs = isFinite (c.unlearnedInputLufs) ? c.unlearnedInputLufs : factoryConstants().unlearnedInputLufs;
        const auto measuredInputLufs = valueOr (profile, &MusicProfile::integratedLufs, defaultInputLufs);

        // GLAZE ≤ 0 或 NaN 按 0（「> 0」的写法把 NaN 一并挡掉）：不生成任何增量，目标 = 手动链本身的结果。
        const auto glaze = inputs.glazePercent > 0.0f ? juce::jmin (inputs.glazePercent, 100.0f) * 0.01f : 0.0f;

        // ---- 环境：特征按置信度向保守默认值收拢（planningContext）----
        auto ctx = planningContext (inputs, c, plan.weights);

        const auto full = c.fullConfidence > 0.0f ? c.fullConfidence : 1.0f;
        const auto lraTrust = trustOf (profile, &MusicProfile::loudnessRangeLu, full);
        const auto slopeTrust = trustOf (profile, &MusicProfile::spectralSlopeDbPerOct, full);

        const auto kappa = confidenceScale (profile, c);

        // ---- 目标 ----
        const auto lra = lraTrust > 0.0f ? valueOr (profile, &MusicProfile::loudnessRangeLu, c.lraReferenceLu) : c.lraReferenceLu;
        const auto lraCorrection = nonNegative (c.lraSlopeDbPerLu) * nonNegative (lra - c.lraReferenceLu);
        const auto preferred = clampTo (ctx.sc.preferredLufs - lraCorrection, c.minPreferredLufs, c.maxPreferredLufs);
        const auto driveFloor = isFinite (c.minimumFullDriveDb) ? clampTo (c.minimumFullDriveDb, 0.0f, 24.0f) : 0.0f;
        const auto requestFull = juce::jmax (driveFloor, nonNegative (preferred - ctx.inputLufs));

        // ---- 手动值：只有它时的链（手动链的预计净增益、手动本身是否已超约束）----
        const auto manualSettings = manualSettingsOf (manual);
        const auto manualChain = estimateChain (ctx, manualSettings);
        const auto manualNetDb = manualChain.netDb;
        ctx.manualGainDb = manualSettings.gainDb;   // 满强度方案的自动 Limit Gain 量程 = 24 − 它（completeFullPlan，算法 8）

        // GLAZE 0：没有方案。净增益与目标就是手动链本身（手动中性、输入不超 Ceiling 时 = 0 与输入响度），
        // 这样 GLAZE 从 0 拧起来时净增益从手动链的值连续地往上走（随 GLAZE 单调）。Clay 保持 ClayPlan{}（全中性）。
        if (! (glaze > 0.0f))
        {
            plan.requestedGainDb = plan.effectiveGainDb = isFinite (manualNetDb) ? manualNetDb : 0.0f;
            plan.requestedTargetLufs = plan.effectiveTargetLufs = measuredInputLufs + plan.effectiveGainDb;
            plan.estimatedExcessDb = nonNegative (ctx.truePeakDb + plan.effectiveGainDb - ctx.ceilingDb);
            return plan;
        }

        // ---- 修坯 Clay：与下面的电平方案互不相干（不看手动值、约束反馈与强度 κ，也不进前向模型），见 planClay ----
        plan.clay = planClay (profile, c, ctx.sc, glaze, full);

        // ---- 满强度方案 ----
        const auto fullPlan = makeFullPlan (ctx, requestFull);

        // 手动本身已超约束（与「手动中性」时比：输入本身造成的超限不算手动的）→ 撤回该模块的自动增量、标受手动限制。
        Schedule schedule;
        auto& withdrawn = schedule.withdrawn;
        withdrawn = check (ctx, manualChain, fullPlan.neutral, fullPlan.heat > 0.0f);

        // 手动单独已超预算、自动部分不再为它负责的项（T-032 R6-02 收尾，A11）：编排器的保护层与上游监视不核对它们，只核对自动部分。
        plan.manualBudgetMask = manualBudgetMaskFor (ctx, manualSettings, withdrawn);

        // 逐模块钳住（Heat、Glue 的上限只收它们自己的自动增量，不拖共用强度，见 feasible()）：手动 + 自动 ≤ 有效上限。
        // 手动越接近上限，余量连续地降到 0；手动单独超上限时上面已撤回该模块——两条路在上限处接上，没有台阶。
        // Heat 的上限取决于它的入口峰值，入口在 Glue 之后：按手动 + 满强度自动 Glue 估（Glue 补偿让峰值偏高，偏保守）。
        if (fullPlan.heat > 0.0f && ! withdrawn.heat)
        {
            const HeatRow row (*ctx.c, ctx.truePeakDb + gluePeakDb (ctx, clampTo (manualSettings.glue + fullPlan.glue, 0.0f, 100.0f)));
            schedule.heatAllowance = nonNegative (heatCap (ctx.sc, row) - manualSettings.heat);
        }

        if (fullPlan.glue > 0.0f && ! withdrawn.glue)
            schedule.glueAllowance = nonNegative (glueCap (ctx) - manualSettings.glue);

        // Clip 爬坡按「手动值下」的 Limit 入口峰值重算（手动 Gain / Output 把峰值推高时爬坡要更早结束）。
        schedule.clipRamp = fullPlan.clipRamp;

        if (fullPlan.clip > 0.0f)
        {
            const auto manualStart = ctx.truePeakDb + manualChain.color.peakDb + manualSettings.gainDb;
            const auto neutralStart = ctx.truePeakDb + fullPlan.neutral.color.peakDb;

            if (manualStart > neutralStart)
            {
                const auto fullColor = estimateColor (ctx, fullPlan.glue, fullPlan.heat);
                const auto neutralEnd = ctx.truePeakDb + fullColor.peakDb + fullPlan.limitGainDb[(size_t) gainKnots - 1];
                schedule.clipRamp = juce::jmin (schedule.clipRamp,
                                                clipRampFor (ctx, fullPlan.clipThresholdDb, manualStart, manualStart + (neutralEnd - neutralStart),
                                                             clamp01 (c.clipRampMinFraction)));
            }
        }

        // ---- 强度：GLAZE × κ，再受约束反馈与叠加手动后的约束限制 ----
        auto strength = glaze * kappa;
        bool constrained = fullPlan.constrained;
        bool limitedByManual = withdrawn.any() || ctx.limitBypassed;
        // 电平类自动全部撤回（T-032 R6-02）：只有下面三种情况——手动已让限幅器超约束、Limit 旁通、约束反馈上限低于手动链净增益——
        // 才没有自动加上去的增益。手动 Heat / Glue / Clip 单独超预算时只撤回那一个模块（v0.6 F-02 逐模块钳住），自动 Gain / 其余模块
        // 照常展开。limitedByManual 是界面提示（任一模块）；编排器怎样用这两个标志见 GlazeTypes.h Plan::automaticLevelWithdrawn。
        bool automaticLevelWithdrawn = withdrawn.limit || ctx.limitBypassed;

        if (withdrawn.limit || ctx.limitBypassed)
        {
            strength = 0.0f;   // 手动已让限幅器超约束（或 Limit 旁通）：自动不再加任何会动电平的量
        }
        else
        {
            // 约束反馈：净提升 = 手动链净增益 + s × G_full′ ≤ maxGainDb。NaN 按「不许再加」处理。
            const auto maxGain = inputs.maxGainDb;

            if (! (maxGain == maxGain))
            {
                strength = 0.0f;
                constrained = true;
            }
            else if (maxGain < manualNetDb)
            {
                strength = 0.0f;           // 只剩手动部分就已超过允许的净提升：自动全部撤回
                constrained = true;
                limitedByManual = true;
                automaticLevelWithdrawn = true;
            }
            else if (fullPlan.gainDb > 0.0f && maxGain < manualNetDb + strength * fullPlan.gainDb)
            {
                strength = juce::jmin (strength, clamp01 ((maxGain - manualNetDb) / fullPlan.gainDb));
                constrained = true;
            }

            strength = toGrid (strength);

            if (strength > 0.0f)
            {
                const auto bound = strengthBound (ctx, fullPlan, manualSettings, schedule);

                if (bound < strength)
                {
                    strength = bound;
                    constrained = true;
                }
            }
        }

        const auto autoLevel = autoSettingsAt (fullPlan, strength, schedule);

        // ---- 频谱类增量（与电平约束无关，只按 GLAZE × κ 缩放）----
        const auto spectralStrength = glaze * kappa;
        auto& inc = plan.increments;

        inc.colorGlue = autoLevel.glue;
        inc.colorHeat = autoLevel.heat;
        inc.limitGainDb = autoLevel.gainDb;
        inc.limitClip = autoLevel.clip;

        if (! ctx.colorBypassed && ctx.sc.toneMaxDb > 0.0f)
        {
            const auto slope = lerp (ctx.sc.toneReferenceSlopeDbPerOct,
                                     valueOr (profile, &MusicProfile::spectralSlopeDbPerOct, ctx.sc.toneReferenceSlopeDbPerOct), slopeTrust);
            const auto difference = ctx.sc.toneReferenceSlopeDbPerOct - slope;
            const auto outside = std::copysign (nonNegative (std::abs (difference) - automaticToneSlopeDeadbandDbPerOct), difference);
            const auto toneFull = clampTo (outside * nonNegative (c.toneDbPerSlopeDb), -ctx.sc.toneMaxDb, ctx.sc.toneMaxDb);
            inc.colorToneDb = toneFull * spectralStrength;
        }

        if (! manual.balance.bypass)
        {
            const auto bandTrust = profile != nullptr ? clamp01 (meanBandConfidence (*profile) / full) : 0.0f;
            // 底量是实时处理权限，当前频谱没有偏离时 Balance 不动作。旧画像没有问题不应把未来段落的处理永久关掉。
            const auto baseline = [] (float floor, float cap) noexcept
            {
                return isFinite (floor) ? juce::jmin (nonNegative (floor), juce::jmin (cap, 100.0f)) : 0.0f;
            };

            if (ctx.sc.recoverCap > 0.0f)
            {
                const auto evidence = profile != nullptr && c.recoverEvidenceFullDb > 0.0f
                    ? juce::jmin (ctx.sc.recoverCap, 100.0f) * clamp01 (evidenceScore (*profile, profile->recoverEvidenceDb) / c.recoverEvidenceFullDb) * bandTrust : 0.0f;
                inc.balanceRecover = juce::jmax (baseline (c.automaticRecoverFloor, ctx.sc.recoverCap), evidence) * spectralStrength;
            }

            if (ctx.sc.tameCap > 0.0f)
            {
                const auto evidence = profile != nullptr && c.tameEvidenceFullDb > 0.0f
                    ? juce::jmin (ctx.sc.tameCap, 100.0f) * clamp01 (evidenceScore (*profile, profile->tameEvidenceDb) / c.tameEvidenceFullDb) * bandTrust : 0.0f;
                inc.balanceTame = juce::jmax (baseline (c.automaticTameFloor, ctx.sc.tameCap), evidence) * spectralStrength;
            }
        }

        // ---- 目标（净增益含手动链的预计净增益：校准器拿它作目标，不会去抵消手动）----
        plan.requestedGainDb = manualNetDb + glaze * fullPlan.requestedGainDb;
        plan.effectiveGainDb = manualNetDb + strength * fullPlan.gainDb;
        plan.requestedTargetLufs = measuredInputLufs + plan.requestedGainDb;
        plan.effectiveTargetLufs = measuredInputLufs + plan.effectiveGainDb;
        plan.estimatedExcessDb = nonNegative (ctx.truePeakDb + manualNetDb + glaze * fullPlan.requestedGainDb - ctx.ceilingDb);
        plan.limitedByConstraints = constrained;
        plan.limitedByManual = limitedByManual;
        plan.automaticLevelWithdrawn = automaticLevelWithdrawn;

        // 标定快照是随工程读回来的数据（读取只查格式、不查取值）：坏快照里的 NaN / Inf 不许流进模块参数与校准目标。
        // 正常快照下这里什么都不改；出了非有限值就按「这一项不动」处理（增量 0、净增益退回手动链）。
        auto finiteOrZero = [] (float& x) noexcept { if (! isFinite (x)) x = 0.0f; };
        finiteOrZero (inc.balanceRecover);
        finiteOrZero (inc.balanceTame);
        finiteOrZero (inc.colorToneDb);
        finiteOrZero (inc.colorGlue);
        finiteOrZero (inc.colorHeat);
        finiteOrZero (inc.limitGainDb);
        finiteOrZero (inc.limitClip);

        if (! isFinite (plan.requestedGainDb) || ! isFinite (plan.effectiveGainDb) || ! isFinite (plan.estimatedExcessDb))
        {
            const auto fallback = isFinite (manualNetDb) ? manualNetDb : 0.0f;
            plan.requestedGainDb = isFinite (plan.requestedGainDb) ? plan.requestedGainDb : fallback;
            plan.effectiveGainDb = isFinite (plan.effectiveGainDb) ? plan.effectiveGainDb : fallback;
            plan.estimatedExcessDb = isFinite (plan.estimatedExcessDb) ? plan.estimatedExcessDb : 0.0f;
            plan.requestedTargetLufs = measuredInputLufs + plan.requestedGainDb;
            plan.effectiveTargetLufs = measuredInputLufs + plan.effectiveGainDb;
        }

        return plan;
    }

    //==============================================================================
    namespace
    {
        // 实际值 = clamp(手动 + 自动)。增量为 0（含 −0、NaN）时原样返回手动值、不做任何运算：GLAZE 0 逐位等于 v0.5。
        float addClamped (float manual, float increment, float lo, float hi) noexcept
        {
            if (! (increment != 0.0f) || ! isFinite (increment))
                return manual;

            const auto sum = manual + increment;

            if (! (sum == sum))
                return manual;

            return clampTo (sum, lo, hi);
        }
    }

    //==============================================================================
    // 逐项取小：plan 的每项增量、净增益与目标都不超过 bound 的；受限标志取或。权重保留 plan 自己的。
    static void limitPlanTo (Plan& plan, const Plan& bound) noexcept
    {
        auto& inc = plan.increments;
        const auto& cap = bound.increments;
        inc.balanceRecover = juce::jmin (inc.balanceRecover, cap.balanceRecover);
        inc.balanceTame = juce::jmin (inc.balanceTame, cap.balanceTame);
        // 带符号的 Tone 按幅度保守封顶；另一份方案方向相反时撤回，不能靠 min 把负向修正越夹越强。
        inc.colorToneDb = inc.colorToneDb * cap.colorToneDb > 0.0f
            ? std::copysign (juce::jmin (std::abs (inc.colorToneDb), std::abs (cap.colorToneDb)), inc.colorToneDb) : 0.0f;
        inc.colorGlue = juce::jmin (inc.colorGlue, cap.colorGlue);
        inc.colorHeat = juce::jmin (inc.colorHeat, cap.colorHeat);
        inc.limitGainDb = juce::jmin (inc.limitGainDb, cap.limitGainDb);
        inc.limitClip = juce::jmin (inc.limitClip, cap.limitClip);

        plan.requestedGainDb = juce::jmin (plan.requestedGainDb, bound.requestedGainDb);
        plan.effectiveGainDb = juce::jmin (plan.effectiveGainDb, bound.effectiveGainDb);
        plan.requestedTargetLufs = juce::jmin (plan.requestedTargetLufs, bound.requestedTargetLufs);
        plan.effectiveTargetLufs = juce::jmin (plan.effectiveTargetLufs, bound.effectiveTargetLufs);
        plan.estimatedExcessDb = juce::jmin (plan.estimatedExcessDb, bound.estimatedExcessDb);
        plan.limitedByConstraints = plan.limitedByConstraints || bound.limitedByConstraints;
        plan.limitedByManual = plan.limitedByManual || bound.limitedByManual;
        plan.automaticLevelWithdrawn = plan.automaticLevelWithdrawn || bound.automaticLevelWithdrawn;   // T-032 R6-02：同样取或（保守）

        // 手动链单独已超预算的项（T-032 A11，GlazeTypes.h manualBudget*）取或，与上面三个受限标志同一口径：任一份的手动链单独超了某一项，
        // 那一份的该项自动增量（Heat / Glue / Clip）就是 0，取小之后仍是 0——这一项的用量全是手动的（Clip 另加两份都许可的自动 Gain 推深
        // 的部分），保护层与上游监视不该替它退让自动 Gain。取与（只在两份都超时放开）会在两份前向模型一份说超、一份说没超时按风格上限
        // 核对：实际手动单独已超时保护层退到底、约束反馈把自动增益磨光——正是要修的那种（置信度低的方案输入响度往「更响」收，Glue GR
        // 估得多、峰值估得低，手动 Clip 的削波深度两份可以分处上限两侧，GlazePlannerTests 有构造）。
        plan.manualBudgetMask = static_cast<std::uint8_t> (plan.manualBudgetMask | bound.manualBudgetMask);

        // 修坯 Clay：三项幅度逐项取小（低切 0 = 关，本来就是最小值；三项都随 GLAZE 单调、都不超上限，取小后照样成立）。
        // 分频点只由画像的逐段统计与常数决定，两份方案相同，保留 plan 自己的；取小后收单为 0 时回到中性默认值（与 planClay 一致）。
        auto& clay = plan.clay;
        clay.lowCutHz = juce::jmin (clay.lowCutHz, bound.clay.lowCutHz);
        clay.monoAmount = juce::jmin (clay.monoAmount, bound.clay.monoAmount);
        clay.widthDb = juce::jmin (clay.widthDb, bound.clay.widthDb);

        if (! (clay.monoAmount > 0.0f))
            clay.monoHz = ClayPlan {}.monoHz;

        // 代价位取与：两份方案用同一份代价、同一个分频点（封顶画像只拉满 1 ～ 7 用的置信度，Clay 特征不动），同一项的 f 相同；
        // 位不同只可能是某一份「代价之前就不动作」（权重不同时风格低切目标不高于地板、加宽系数为 0 这类坏快照），取小后那一项
        // 正是被它压成 0 的——原因不是代价，界面不该说「试过会抬高峰值」。
        clay.costLimitedMask = static_cast<std::uint8_t> (clay.costLimitedMask & bound.clay.costLimitedMask);
    }

    //==============================================================================
    Plan makePlan (const PlanInputs& inputs) noexcept
    {
        auto plan = makePlanAtConfidence (inputs);

        if (inputs.profile == nullptr || ! (inputs.glazePercent > 0.0f))
            return plan;

        // 置信度低的方案不许比置信度满的更激进（决策与校准第 2 节「置信度低时正向增益与处理量按比例收紧」）。
        // 置信度只经「输入响度往更响一侧收拢、强度 κ」进方案，本来是收紧的；但真峰值照用、Glue / Heat 按各自上限钳住等
        // 非线性让个别画像与手动组合反了过来（v0.6 验收 F-05：净增益最坏多 22 dB）。所以同一份画像把置信度拉满再算一次，
        // 逐项取小：两份都随 GLAZE 单调、都不超上限、GLAZE 0 时都为 0，取小后这三条照样成立；风格权重用同一份。
        //
        // 修坯 Clay 的五个特征不在下面「拉满」之列（逐段的 bandStereoConfidence 更不拉：它只当「这一段有没有统计」用，拉满会把
        // 没有统计、S/M 与 ρ 都记 0 的段当成宽段）：Clay 三项本来就随各自特征的可信度单调，拉满只会更大，取小时恒取这一份，
        // 白算；而把它们加进来会让「只有 Clay 特征不满」的画像也去算封顶方案，改动方案其余字段的逐位结果（Tone 的 −0 归 +0）。
        // ADR-012 的三类代价特征同样不拉：代价系数不看置信度（只看 valid），拉了也不变。
        // Clay 真正要封的只有下面第二次：「自动」且权重没锁时，置信度低 → 权重偏通透 → 通透的低切目标与加宽系数最大，
        // 与 F-05 同理，limitPlanTo 对 Clay 三项同样逐项取小。
        const auto& c = inputs.constants != nullptr ? *inputs.constants : factoryConstants();
        const auto full = c.fullConfidence > 0.0f ? c.fullConfidence : 1.0f;

        auto confident = *inputs.profile;
        auto raised = false;

        const auto raise = [&] (float& confidence) noexcept
        {
            if (! (confidence >= full))
            {
                confidence = full;
                raised = true;
            }
        };

        for (auto* feature : { &confident.integratedLufs, &confident.loudnessRangeLu, &confident.plrDb, &confident.truePeakDbtp,
                               &confident.spectralSlopeDbPerOct, &confident.lowShare, &confident.highShare, &confident.lowBandRangeDb,
                               &confident.onsetDensityPerSecond, &confident.onsetStrengthDb })
            if (feature->valid)
                raise (feature->confidence);

        for (auto& band : confident.bandConfidence)
            raise (band);

        raise (confident.representativeness);

        if (! raised)
            return plan;

        // 封顶用的方案：同一份画像把置信度拉满，风格权重用这份方案已定的（离散风格的独热权重、或锁定的自动权重）。
        auto reference = inputs;
        reference.profile = &confident;
        reference.lockedWeights = &plan.weights;
        reference.style = Style::automatic;   // 权重已定（plan.weights）：按锁定权重算，离散风格的独热权重同样适用
        limitPlanTo (plan, makePlanAtConfidence (reference));

        // 「自动」且权重没锁（首次聆听中的候选方案）：权重本身随置信度变（不确定时偏向通透），而通透的限幅上限比冲击宽，
        // 置信度低反而放得下更多增益（v0.6 验收 F-05 复核：最坏多 3 dB）。再用「置信度拉满、权重也按它重新判断」的方案封一次。
        if (inputs.style == Style::automatic && inputs.lockedWeights == nullptr)
        {
            auto confidentWeights = inputs;
            confidentWeights.profile = &confident;
            limitPlanTo (plan, makePlanAtConfidence (confidentWeights));
        }

        return plan;
    }

    //==============================================================================
    ManualChainUsage estimateManualChainUsage (const PlanInputs& inputs) noexcept
    {
        // 与 makePlanAtConfidence 同一份环境（风格权重、按置信度收拢的特征、手动的非电平设置）与同一前向模型；GLAZE、maxGainDb 不参与。
        const auto& c = inputs.constants != nullptr ? *inputs.constants : factoryConstants();
        auto ctx = planningContext (inputs, c, planWeights (inputs, c));
        const auto manual = manualSettingsOf (inputs.manual);
        ctx.manualGainDb = manual.gainDb;

        const auto manualChain = estimateChain (ctx, manual);
        const auto neutral = estimateChain (ctx, Settings {});   // = makeFullPlan 的 full.neutral（约束核对的基线）

        ManualChainUsage usage;
        usage.heatAmount = manual.heat;
        usage.glueGrMeanDb = manualChain.color.glueGrMeanDb;
        usage.glueGrP95Db = manualChain.color.glueGrP95Db;
        usage.clipDepthDb = manualChain.clipDepthDb;
        usage.clipActivity = manualChain.clipActivity;
        usage.limitGrPeakDb = manualChain.limitGrPeakDb;
        usage.limitGrMeanDb = manualChain.limitGrMeanDb;
        usage.limitGrP95Db = manualChain.limitGrP95Db;
        usage.overBudgetMask = manualBudgetMaskFor (ctx, manual, check (ctx, manualChain, neutral, false));
        return usage;
    }

    ChainParams applyIncrements (const ChainParams& manual, const AutoIncrements& inc) noexcept
    {
        auto p = manual;
        p.balance.recover = addClamped (manual.balance.recover, inc.balanceRecover, 0.0f, 100.0f);
        if (isFinite (inc.balanceRecover) && inc.balanceRecover > 0.0f)
            p.balance.automaticRecover = nonNegative (p.balance.recover - manual.balance.recover);
        p.balance.tame    = addClamped (manual.balance.tame,    inc.balanceTame,    0.0f, 100.0f);
        if (isFinite (inc.balanceTame) && inc.balanceTame > 0.0f)
            p.balance.automaticTame = nonNegative (p.balance.tame - manual.balance.tame);
        p.color.toneDb    = addClamped (manual.color.toneDb,    inc.colorToneDb,   -3.0f,   3.0f);
        p.color.glue      = addClamped (manual.color.glue,      inc.colorGlue,      0.0f, 100.0f);
        p.color.heat      = addClamped (manual.color.heat,      inc.colorHeat,      0.0f, 100.0f);
        p.limit.gainDb    = addClamped (manual.limit.gainDb,    inc.limitGainDb,    0.0f,  24.0f);
        p.limit.clip      = addClamped (manual.limit.clip,      inc.limitClip,      0.0f, 100.0f);
        return p;
    }
}

/*
  ==============================================================================

    LiveProfile.cpp
    T-048 实时画像：回看重判（规则与时间约定见 LiveProfile.h 文件头；设计规格-实时GLAZE-主段与增益 第 1～3、5 节；T-048 阶段 1 BRIEF
    第 3 节与阶段 1 修订任务书）。

    一、数据
      - 段环 slots（windowCapacity + queueCapacity 格）：第 n 个交来的段（reset 以来从 0 数）转成定长的 Entry 存在 n mod 格数。Entry 只留
        画像要的量（试算块峰值只留所在的格号）。已发布那一份画像的窗口 = 它最近的 windowCapacity 段，之后是还没发布的段（≤ queueCapacity）；
        新段只会盖掉比已发布窗口更老的格，所以已发布与在途那一遍读的段都不会被改（队列满时见 makeRoomInQueue）。
      - 每一段进账（admit）时定下、之后不变的：进账判断（按上一份已发布画像的参考）、换歌的票、两个事件时钟的印记。
      - 事件时钟（对数域，Counters::mainClock / referenceClock）：换歌软遗忘加 ln(1 ÷ keep)（主段与参考同加；keep = 0 记 800，exp(−800) = 0），
        长时间无主段加「超出秒数 ÷ noMainDecaySeconds」（只主段）。段 i 的全局因子 = exp(−(这一份画像的时钟 − 印记_i))：乘在事件发生时
        窗口里全部更老的段上，不碰它们存的量，之后重判也不撤销。

    二、一遍重算（第 k 份画像 = 前 k 段；按序的工作步，见 Step）
      admit（第 k 段进账）→ referenceSweep（新 → 老：参考权重 ρ、全部段权重 a、段响度直方图）→ referencePick（S_ref）→ lowSweep（按 S_ref 的
      响度因子 λ、lowShare 直方图）→ lowPick（lowRef）→ judgeSweep（新 → 老：m′、主段时钟、主段权重 f）→ accumulate（老 → 新：全部累计，
      顺序同 House 按存进账本的先后求和）→ trialPick（每路一步：P99 与代价）→ gateSweep（老 → 新：I 的第二遍门限，顺手把这一遍写过的
      试算格清零）→ finalize（画像、诊断、提交记录）→ clearHistograms（其余直方图清零）。
      每步有一个工作量估计（纳秒，costOf）；一片做到估计用完 sliceBudgetNanoseconds 为止（至少一步）。估计只决定 CPU 怎么摊：一遍的结果
      是窗口（段、印记、事件时钟）与常数的纯函数，分几片、几时做都一样，所以已发布的画像与分片无关。

    三、权重（LiveProfile.h 文件头第一节的公式；e = 有效时长，W_j = 第 j 段按它的分段方式换算的 W_mem）
      ρ_i = exp(−Σ_{j>i} e_j ÷ referenceMemorySeconds − Δ参考时钟_i)、参考权重 r_i = e_i × ρ_i；a_i = exp(−Σ_{j>i} e_j ÷ W_j − Δ参考时钟_i)；
      f_i = m′_i × exp(−Σ_{j>i} m′_j e_j ÷ W_j − Δ主段时钟_i)。遗忘与事件关掉时 ρ = a = 1、f = m′（exp(−0) = 1 精确）：硬门、冻结参考下 f
      就是 House 的「满编 1 / 否则 0」，累计逐位同 House 的按段求和。

    四、与 House 画像逐项的对应（MusicAnalyzer.cpp「六」节 computeHouseProfile / houseLoudnessProfile / houseSpectrumProfile /
      houseStereoGroup / houseOnsetProfile / houseTrialProfile / houseSubProfile / houseConfidences；House 的整数计数在这里是加权的实数，
      权重全为 1 时退回 House 的整数公式）。与 House 按设计的差别：
        - I：4/4 型主段权重、响度型 / 被动型全部段权重；两遍门限都逐块做（窗口里有每个门块），与 House 同一公式、同一求和顺序；
        - LRA 按曲风分口径（a3c1b0e）：4/4 型主段权重（同 House「只用满编段」），响度型 / 被动型参考权重（全部段）；
        - effectiveSeconds = M、representativeness = c、各特征 confidence = min(c, 观测充分度)（混合损失也给，同 T-049 后的 House）；
        - 真峰值：主段加权的「段最大值」直方图（0.1 dB 一格，每格记最大线性值），取其上主段质量 ≥ peakTailSeconds 的最高一格的最大值；
        - 试算 P99：每路一张加权直方图（0.01 dB 一格），每段交来的「每路最大的 8 个」按 f 计入、其余块只计入总块数，numpy 线性插值的
          加权推广（trialPercentileOf）；每个 P99 误差 ≤ 半格 0.005 dB；
        - formatVersion = clayProfileFormatVersion（2）、houseGate 不填（理由见 finalizeProfile）。

    五、老段（挤出窗口）：直接丢弃，不进「老账」。丢掉的那一截权重（相对于无限记忆）：
        - 主段：稳态下窗口最老一段之后有约 512 × φ 个主段小节（φ = 主段小节占比），它的 f ≈ exp(−512 φ × 1.94 s ÷ W_mem)，整截尾巴约占
          M 的 exp(−10.4 φ)（出厂 W_mem = 48 小节）：φ = 1 时 3e-5、0.5 时 0.6 %、0.25 时 7 %——只在主段很稀的长素材里显出来，丢的是
          16 分钟以前的素材。
        - 参考（τ = 180 s）：窗口约 993 s，尾巴约占 exp(−993 ÷ 180) ≈ 0.4 %；全部段（τ = W_mem = 90 s）约 1.6e-5。
      LiveDiagnostics::oldestMainShare / oldestReferenceShare 报此刻最老一段的份额（窗口满时下一段就丢掉它）。

    六、状态 XML（格式版本 2，节点 LIVE_PROFILE）
      已发布那一份画像的事件时钟与计时（Counters）、窗口里的段与还没发布的段（逐段的原始量与印记，小端二进制 + base64），采样率。派生量
      （参考、画像、判断）读回后同步重算——与保存时逐位相同；还没发布的段按序进账、重算、立刻发布。读的时候严格：缺属性、位数不对、
      base64 坏、长度不对、非有限值、计数或格号越界、格式版本不认识 → 冷启动并返回 false。

    实时约束：全部容量在构造（Impl）时一次分配；prepare / reset / 音频线程接口不分配、不加锁。

  ==============================================================================
*/

#include "LiveProfile.h"

#include "GlazeProfile.h"

#include <juce_audio_basics/juce_audio_basics.h>   // juce::Decibels：真峰值与 House 画像用同一个换算

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <tuple>
#include <type_traits>
#include <vector>

namespace celadon::glaze
{
    namespace
    {
        //==============================================================================
        // 画像口径的常数——逐个同 MusicAnalyzer.cpp 匿名命名空间里的同名常数（那边不在共享头里，这里抄一份；能对上 SegmentLedger.h
        // 公开常数的用下面的 static_assert 钉住，其余改动时两边一起改）。
        constexpr double integratedRelativeGateLu = -10.0;
        constexpr double lraRelativeGateLu        = -20.0;
        constexpr double lraLowFraction  = 0.10;
        constexpr double lraHighFraction = 0.95;
        constexpr double minimumLraValues = 10.0;       // 分析器：至少 10 个 3 s 取样；这里是加权的个数
        constexpr double histogramMinLufs = -70.0;
        constexpr int histogramBinsPerLu = 10;
        constexpr int histogramBins = 80 * histogramBinsPerLu;
        constexpr double loudnessFullSeconds = 10.0;
        constexpr double lraFullSeconds      = 60.0;
        constexpr double spectrumFullSeconds = 10.0;
        constexpr double onsetFullSeconds    = 10.0;
        constexpr double minimumSpectrumSeconds = 1.0;
        constexpr double minimumOnsetSeconds    = 1.0;
        constexpr double minimumOnsetEvents     = 3.0;  // 同上：加权的个数
        constexpr double bandGateDb = -60.0;
        constexpr double recoverPersistenceStart = 0.5;
        constexpr double recoverPersistenceFull  = 0.8;
        constexpr double rollOffStepDb = 3.0;
        constexpr int lowShareLastBand   = 4;
        constexpr int highShareFirstBand = 22;
        constexpr int minimumSlopeBands  = 6;
        constexpr double noDataDb = -150.0;
        constexpr double lowRangeBinDb = 0.5;
        constexpr int lowRangeBins = 280;
        constexpr double lowRangeLowFraction  = 0.10;
        constexpr double lowRangeHighFraction = 0.90;
        constexpr double onsetStrengthCapDb = 30.0;
        constexpr double strengthBinDb = 0.25;
        constexpr int strengthBins = (int) (onsetStrengthCapDb / strengthBinDb) + 1;
        constexpr double minimumOnsetAgreement = 0.5;
        constexpr double subShareFloor = 1.0e-15;
        constexpr double minimumSubSeconds = 1.0;
        constexpr double subFullSeconds = 10.0;
        constexpr int lowStereoFirstBand = 0;
        constexpr int lowStereoLastBand = 5;
        constexpr int highStereoFirstBand = 17;
        constexpr int highStereoLastBand = 26;
        constexpr double stereoRatioFloor = 1.0e-12;
        constexpr double minimumStereoSeconds = 1.0;
        constexpr double stereoFullSeconds = 10.0;
        constexpr double onsetGroupSeconds = 1.0 / 3000.0;
        constexpr int maximumGroupLength = 128;
        constexpr double trialBlockSeconds = 0.1;
        constexpr double trialMinimumSeconds = 10.0;
        constexpr double trialFullSeconds = 10.0;
        constexpr double trialPercentile = 0.99;
        constexpr double trialPeakFloorRatio = 1.0e-6;
        constexpr int numMonoTrials = (int) clayMonoCandidateHz.size();
        constexpr int trialLowCut = 0, trialFirstMono = 1, trialWidth = 1 + numMonoTrials;
        constexpr int numTrials = 2 + numMonoTrials;
        constexpr int numBlendTrials = (int) clayBlendCandidateHz.size();

        static_assert (SegmentLedger::lowRangeBins == lowRangeBins && SegmentLedger::lowShareLastBand == lowShareLastBand,
                       "段落账的低频直方图与低频段边界必须与分析器（这里抄的一份）相同");
        static_assert (SegmentLedger::trialSeries == numTrials + 1 && SegmentLedger::blendSeries == numBlendTrials,
                       "段落账的试算路数必须与分析器（这里抄的一份）相同");
        static_assert (SegmentLedger::loudnessHistogramMinLufs == histogramMinLufs && SegmentLedger::loudnessBinsPerLu == histogramBinsPerLu
                           && SegmentLedger::loudnessBins == histogramBins,
                       "段落账的响度直方图与分析器 I / LRA 的直方图同格（S_ref、LRA 两张表都用它）");
        static_assert (strengthBins <= 256, "段里的起音强度格号按 8 位存");
        static_assert (std::tuple_size<decltype (MusicProfile::monoPeakCostDb)>::value == (size_t) numMonoTrials
                           && std::tuple_size<decltype (MusicProfile::lowCutBlendLossDb)>::value == (size_t) numBlendTrials,
                       "每个试算候选都要有一个画像特征");
        static_assert (clayProfileFormatVersion == 2, "实时画像按格式 2 交（含立体声 / 超低频字段，不含 houseGate）");

        //==============================================================================
        // 实时画像自己的直方图（「分位数一律用定长直方图」；格宽的理由写在各自旁边）。
        constexpr int lowShareBins = SegmentLedger::lowShareBins;   // lowRef：0 … 1、0.001 一格（同 SegmentLedger）

        // 真峰值：段最大值（线性）按 dB 分格，−100 … +40 dB、0.1 dB 一格（格只决定「哪些段并在一起算质量」）；每格另记落进来的最大线性值，
        // 读数取那一格记下的最大值——遗忘关掉时就是 House 的最大值逐位。−100 dB 以下并进第 0 格（gainToDecibels 本身在 −100 dB 封底）。
        constexpr double peakMinDb = -100.0;
        constexpr int peakBinsPerDb = 10;
        constexpr int peakBins = 140 * peakBinsPerDb;

        // 试算块峰值：−80 … +24 dBFS、0.01 dB 一格（有效块的峰值不低于约 −70 dBFS）。P99 按格中心算：每个 P99 误差 ≤ 半格 0.005 dB，
        // 代价（两个 P99 之差）≤ 0.01 dB，比决策器代价斜坡的两端（0.2 / 0.6 dB）小一个量级以上。越界的块峰值钳进两端的格。
        constexpr double trialMinDb = -80.0;
        constexpr int trialBinsPerDb = 100;
        constexpr int trialBins = 104 * trialBinsPerDb;
        constexpr int trialSeries = numTrials + 1;   // [0] 原始，[1 + c] 候选 c（同 Segment::liveTrialTop）
        constexpr int trialTopCount = SegmentLedger::trialTopCount;
        static_assert (trialBins <= 65536, "试算格号按 16 位存");

        // 段的 27 段频谱累计：8 张表按这个顺序放在 Entry::spectrum / Sums::spectrum 里（一个循环累加，编译器好向量化）。
        enum SpectrumTable : int
        {
            powerTable = 0, tameTable, recoverTable, deficitTable, validTable, stereoPowerTable, stereoCrossTable, stereoWeightTable,
            spectrumTables
        };

        // 换歌软遗忘 keep = 0 时事件时钟加这么多（exp(−800) 在 double 里就是 0，又不会让时钟变成 ∞ 之后相减出 NaN）。
        constexpr double maximumLogKeep = 800.0;

        // 一遍重算每一步的工作量估计（纳秒，107 实测标定，见 REPORT-A2；只决定一片做几步，不影响结果）。
        constexpr double costAdmit = 2500.0;               // 进账判断、换歌的回看（平常 ≤ 十几段）
        constexpr double costReferenceEntry = 45.0;        // 一段的参考权重（一个 exp）与直方图
        constexpr double costAllEntry = 25.0;              // 响度型 / 被动型多一个 exp
        constexpr double costPick = 1800.0;                // 一次分位（扫 800 / 1000 格）加清零
        constexpr double costLowEntry = 15.0;
        constexpr double costJudgeEntry = 45.0;            // m′ 与主段权重（一个 exp）
        constexpr double costAccumulateBase = 220.0;       // 一段的 27 段 × 8 张表与零散的和
        constexpr double costPerValue = 2.0;               // 每个门块 / 短期响度 / 低频格 / 起音强度 / 试算值
        constexpr double costTrialSeries = 600.0;          // 一路试算 P99（从顶往下扫几格）
        constexpr double costGateEntry = 12.0;             // 一段的第二遍门限（另按门块数加）与清试算格
        constexpr double costFinalize = 6000.0;            // 画像（频谱、LRA、起音、置信度）、诊断、提交记录
        constexpr double costClear = 3000.0;               // 其余直方图清零（约 30 KB）

        constexpr const char* liveStateTag = "LIVE_PROFILE";
        constexpr int liveStateFormatVersion = 2;

        //==============================================================================
        // 帮助函数：口径同 MusicAnalyzer.cpp 匿名命名空间里的同名函数（那边不导出，这里各写一个小的）。

        double energyToLufs (double meanSquare) noexcept
        {
            return meanSquare > 0.0 ? -0.691 + 10.0 * std::log10 (meanSquare)
                                    : -std::numeric_limits<double>::infinity();
        }

        // 同 MusicAnalyzer.cpp firstBinAtOrAbove：格中心 ≥ gate 的第一格（= histogramBins：没有一格在门之上）。NaN 当最低（第 0 格）。
        int firstBinAtOrAbove (double gate) noexcept
        {
            const auto position = (gate - histogramMinLufs) * (double) histogramBinsPerLu - 0.5;

            if (! (position > 0.0))
                return 0;

            if (position >= (double) histogramBins)
                return histogramBins;

            return (int) std::ceil (position);
        }

        double binCentreLufs (int bin) noexcept
        {
            return histogramMinLufs + ((double) bin + 0.5) / (double) histogramBinsPerLu;
        }

        float fraction (double numerator, double fullScale) noexcept
        {
            return fullScale > 0.0 ? (float) juce::jlimit (0.0, 1.0, numerator / fullScale) : 0.0f;
        }

        void invalidate (Feature& feature) noexcept
        {
            feature = Feature {};
        }

        void setFeature (Feature& feature, double value, double effectiveSeconds) noexcept
        {
            feature.value = (float) value;
            feature.valid = true;
            feature.effectiveSeconds = effectiveSeconds;
        }

        double sideToMidDbOf (double power, double cross) noexcept
        {
            const auto floorPower = stereoRatioFloor * power;
            const auto mid = juce::jmax (floorPower, 0.5 * (power + cross));
            const auto side = juce::jmax (floorPower, 0.5 * (power - cross));
            return 10.0 * std::log10 (side / mid);
        }

        double correlationOf (double power, double cross) noexcept
        {
            return juce::jlimit (-1.0, 1.0, cross / power);
        }

        // 同 SegmentLedger.cpp weightedPercentileBin：累计权重第一次达到 share × 总权重的那一格（相对 1e-9 的容差）；没有权重 → −1。
        template <size_t N>
        int weightedPercentileBin (const std::array<double, N>& weights, double total, double share) noexcept
        {
            if (! (total > 0.0))
                return -1;

            const auto target = share * total - 1.0e-9 * total;
            double cumulative = 0.0;

            for (size_t bin = 0; bin < N; ++bin)
            {
                cumulative += weights[bin];

                if (weights[bin] > 0.0 && cumulative >= target)
                    return (int) bin;
            }

            for (size_t bin = N; bin > 0; --bin)   // 舍入让最后也没达到：取有权重的最高一格
                if (weights[bin - 1] > 0.0)
                    return (int) (bin - 1);

            return -1;
        }

        // 规格第 2 节的斜坡 ramp_w(x) = clamp(0.5 + x ÷ w, 0, 1)（w 已清洗为正）。NaN → 0（读数坏了不当主段）。
        double ramp (double x, double width) noexcept
        {
            const auto y = 0.5 + x / width;
            return y > 0.0 ? (y < 1.0 ? y : 1.0) : 0.0;
        }

        int loudnessBinOf (float lufs) noexcept   // 同 SegmentLedger.cpp recomputeGate 的格
        {
            return juce::jlimit (0, histogramBins - 1, (int) std::floor (((double) lufs - histogramMinLufs) * (double) histogramBinsPerLu));
        }

        int lowShareBinOf (float share) noexcept
        {
            return juce::jlimit (0, lowShareBins - 1, (int) std::floor ((double) share * (double) lowShareBins));
        }

        int peakBinOf (float peak) noexcept
        {
            const auto db = juce::Decibels::gainToDecibels ((double) peak);   // ≥ −100
            return juce::jlimit (0, peakBins - 1, (int) std::floor ((db - peakMinDb) * (double) peakBinsPerDb));
        }

        int trialBinOf (double peak) noexcept
        {
            if (! (peak > 0.0))
                return 0;

            const auto db = 20.0 * std::log10 (peak);
            return juce::jlimit (0, trialBins - 1, (int) std::floor ((db - trialMinDb) * (double) trialBinsPerDb));
        }

        double trialBinLinear (int bin) noexcept
        {
            return std::pow (10.0, (trialMinDb + ((double) bin + 0.5) / (double) trialBinsPerDb) / 20.0);
        }

        std::int64_t saturatingAdd (std::int64_t a, std::int64_t b) noexcept
        {
            if (b > 0 && a > std::numeric_limits<std::int64_t>::max() - b)
                return std::numeric_limits<std::int64_t>::max();

            if (b < 0 && a < std::numeric_limits<std::int64_t>::min() - b)
                return std::numeric_limits<std::int64_t>::min();

            return a + b;
        }

        // 网格 = grid 的整数倍（grid ≥ 1）。t 之前（含）最近的网格点，与严格晚于 t 的第一个网格点（饱和在 int64 的两端）。
        std::int64_t gridQuotient (std::int64_t t, std::int64_t grid) noexcept
        {
            auto quotient = t / grid;

            if (t < 0 && quotient * grid != t)
                --quotient;   // 向下取整

            return quotient;
        }

        std::int64_t gridPointAtOrBefore (std::int64_t t, std::int64_t grid) noexcept
        {
            const auto quotient = gridQuotient (t, grid);
            return quotient <= std::numeric_limits<std::int64_t>::min() / grid ? std::numeric_limits<std::int64_t>::min() : quotient * grid;
        }

        std::int64_t gridPointAfter (std::int64_t t, std::int64_t grid) noexcept
        {
            const auto quotient = gridQuotient (t, grid);

            if (quotient >= std::numeric_limits<std::int64_t>::max() / grid - 1)
                return std::numeric_limits<std::int64_t>::max();

            return (quotient + 1) * grid;
        }

        //==============================================================================
        // 段的数据能不能用：计数在定长数组的范围里、和都是有限值、该非负的非负。分析器交来的段本来就满足；不满足的段（坏数据）只记一条
        // 提交记录（m = 0、有效时长 0），不进任何量——一个 NaN 进了累计，之后的画像就全坏了。
        template <size_t N>
        bool finiteArray (const std::array<double, N>& values, bool nonNegative) noexcept
        {
            for (const auto x : values)
                if (! std::isfinite (x) || (nonNegative && x < 0.0))
                    return false;

            return true;
        }

        bool finiteNonNegative (double x) noexcept
        {
            return std::isfinite (x) && x >= 0.0;
        }

        bool usableSegment (const SegmentLedger::Segment& s) noexcept
        {
            if (! finiteNonNegative (s.effectiveSeconds) || ! finiteNonNegative (s.frameSeconds)
                || ! std::isfinite (s.truePeak) || s.truePeak < 0.0f
                || (s.loudnessValid && ! std::isfinite (s.loudnessLufs)) || (s.lowShareValid && ! std::isfinite (s.lowShare)))
                return false;

            if (s.gatingCount < 0 || s.gatingCount > SegmentLedger::maxSubBlocks || s.rangeCount < 0 || s.rangeCount > SegmentLedger::maxSubBlocks
                || s.strengthCount < 0 || s.strengthCount > SegmentLedger::maxEvents || s.liveTrialBlocks < 0
                || s.frames < 0 || s.fluxFrames < 0 || s.fluxEvents < 0 || s.onsetEffectiveSamples < 0 || s.onsetEvents < 0
                || s.effectiveSubBlocks < 0)
                return false;

            if (s.lowRangeLast >= s.lowRangeFirst && (s.lowRangeFirst < 0 || s.lowRangeLast >= lowRangeBins))
                return false;

            for (int k = 0; k < s.gatingCount; ++k)
                if (! std::isfinite (s.gatingPower[(size_t) k]) || s.gatingPower[(size_t) k] < 0.0f)
                    return false;

            for (int k = 0; k < s.rangeCount; ++k)
                if ((int) s.rangeBin[(size_t) k] >= histogramBins)
                    return false;

            for (int k = 0; k < s.strengthCount; ++k)
                if ((int) s.strengthBin[(size_t) k] >= strengthBins)
                    return false;

            if (! finiteNonNegative (s.rangeEnergy) || ! finiteNonNegative (s.subEnergy) || ! finiteNonNegative (s.midEnergy)
                || ! finiteNonNegative (s.lowStereoWeight) || ! finiteNonNegative (s.highStereoWeight))
                return false;

            if (! finiteArray (s.powerSum, true) || ! finiteArray (s.tameSum, true) || ! finiteArray (s.recoverSum, true)
                || ! finiteArray (s.deficitWeight, true) || ! finiteArray (s.validWeight, true) || ! finiteArray (s.stereoPowerSum, true)
                || ! finiteArray (s.stereoCrossSum, false) || ! finiteArray (s.stereoWeight, true)
                || ! finiteArray (s.liveBlendIntended, true) || ! finiteArray (s.liveBlendOutput, true))
                return false;

            for (const auto& top : s.liveTrialTop)
                if (! finiteArray (top, true))
                    return false;

            return true;
        }

        //==============================================================================
        // 状态 XML 的编码（文件头第六节）：标量写 IEEE 位模式的十六进制（16 位）、计数写十进制；段写小端二进制再 base64。
        constexpr const char* hexDigits = "0123456789abcdef";

        juce::String hexOf (double x)
        {
            std::uint64_t bits = 0;
            std::memcpy (&bits, &x, sizeof (bits));
            char buffer[16];

            for (int i = 15; i >= 0; --i)
            {
                buffer[i] = hexDigits[(size_t) (bits & 15u)];
                bits >>= 4;
            }

            return juce::String (buffer, 16);
        }

        int hexValue (char c) noexcept
        {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        }

        // 恰好 16 位十六进制、有限值。
        bool parseHexDouble (const juce::String& text, double& out) noexcept
        {
            const auto* p = text.toRawUTF8();
            std::uint64_t bits = 0;

            for (int i = 0; i < 16; ++i)
            {
                const auto digit = hexValue (p[i]);

                if (digit < 0)
                    return false;

                bits = (bits << 4) | (std::uint64_t) digit;
            }

            if (p[16] != 0)
                return false;

            double value = 0.0;
            std::memcpy (&value, &bits, sizeof (value));

            if (! std::isfinite (value))
                return false;

            out = value;
            return true;
        }

        // 十进制无符号整数（严格：只有数字、不溢出）。
        bool parseUnsigned (const juce::String& text, std::uint64_t maximum, std::uint64_t& out) noexcept
        {
            const char* p = text.toRawUTF8();

            if (*p == 0)
                return false;

            std::uint64_t value = 0;

            for (; *p != 0; ++p)
            {
                if (*p < '0' || *p > '9')
                    return false;

                const auto digit = (std::uint64_t) (*p - '0');

                if (digit > maximum || value > (maximum - digit) / 10)   // value × 10 + digit ≤ maximum（先挡 digit > maximum：无符号减法不下溢）
                    return false;

                value = value * 10 + digit;
            }

            out = value;
            return true;
        }

        // 小端二进制（juce::OutputStream / InputStream 的 writeInt / writeDouble … 都是小端，与平台无关）。读的一侧数着剩余字节，
        // 不够就记失败（juce 读过头只返回 0，不报错）。
        class BlobReader
        {
        public:
            BlobReader (const void* data, size_t size) noexcept : stream (data, size, false) {}

            bool enough (size_t bytes) noexcept
            {
                if (ok && (size_t) stream.getNumBytesRemaining() < bytes)
                    ok = false;

                return ok;
            }

            std::int64_t i64() noexcept   { return enough (8) ? stream.readInt64() : 0; }
            int i32() noexcept            { return enough (4) ? stream.readInt() : 0; }
            std::uint16_t u16() noexcept  { return enough (2) ? (std::uint16_t) (unsigned short) stream.readShort() : (std::uint16_t) 0; }
            std::uint8_t u8() noexcept    { return enough (1) ? (std::uint8_t) (unsigned char) stream.readByte() : (std::uint8_t) 0; }
            bool flag() noexcept          { const auto x = u8(); ok = ok && x <= 1; return x == 1; }

            double f64() noexcept
            {
                const auto x = enough (8) ? stream.readDouble() : 0.0;
                ok = ok && std::isfinite (x);
                return x;
            }

            float f32() noexcept
            {
                const auto x = enough (4) ? stream.readFloat() : 0.0f;
                ok = ok && std::isfinite (x);
                return x;
            }

            bool atEnd() noexcept   { return stream.getNumBytesRemaining() == 0; }

            bool ok = true;

        private:
            juce::MemoryInputStream stream;
        };
    }

    //==============================================================================
    MainSectionModel mainSectionModelFor (Genre genre) noexcept
    {
        return isHouse (genre) ? MainSectionModel::fourOnFloor : MainSectionModel::loudness;
    }

    bool isInLiveConstantDomain (double value, LiveConstantDomain domain) noexcept
    {
        switch (domain)
        {
            case LiveConstantDomain::finite:            return std::isfinite (value);
            case LiveConstantDomain::positiveFinite:    return std::isfinite (value) && value > 0.0;
            case LiveConstantDomain::percentile:        return value > 0.0 && value <= 1.0;
            case LiveConstantDomain::notNan:            return ! std::isnan (value);
            case LiveConstantDomain::positive:          return value > 0.0;
            case LiveConstantDomain::unit:              return value >= 0.0 && value <= 1.0;
            case LiveConstantDomain::nonNegative:       return value >= 0.0;
            case LiveConstantDomain::nonNegativeFinite: return std::isfinite (value) && value >= 0.0;
        }

        return false;
    }

    LiveConstants sanitiseLiveConstants (const LiveConstants& constants) noexcept
    {
        auto clean = constants;
        const LiveConstants factory;

        visitLiveConstantPairs (clean, factory, [] (const char*, const char*, auto& field, const auto& fallback, LiveConstantDomain domain)
        {
            if (! isInLiveConstantDomain ((double) field, domain))
                field = fallback;
        });

        return clean;
    }

    //==============================================================================
    struct LiveProfile::Impl
    {
        static constexpr int slotCount = windowCapacity + queueCapacity;

        // 一段（定长；只留画像要的量，文件头第一节）。
        struct Entry
        {
            std::int64_t start = 0, end = 0, nominalLength = 0;
            std::int64_t due = 0;                   // 发布时刻 D
            SegmentMode mode = SegmentMode::none;
            bool usable = false;                    // usableSegment；坏段只记提交，不进任何量
            bool pinned = false;                    // commitWithMainness：每次重判都用 pinnedMainness
            float pinnedMainness = 0.0f;

            double effectiveSeconds = 0.0;          // 坏段 0
            bool loudnessValid = false, lowShareValid = false;
            float loudnessLufs = 0.0f, lowShare = 0.0f;
            float truePeak = 0.0f;

            int gatingCount = 0;
            std::array<float, (size_t) SegmentLedger::maxSubBlocks> gatingPower {};
            int rangeCount = 0;
            std::array<std::uint16_t, (size_t) SegmentLedger::maxSubBlocks> rangeBin {};
            double rangeEnergy = 0.0;

            double frames = 0.0;
            std::array<std::array<double, (size_t) numBands>, (size_t) spectrumTables> spectrum {};
            double lowStereoWeight = 0.0, highStereoWeight = 0.0;
            int lowRangeFirst = lowRangeBins, lowRangeLast = -1;
            std::array<std::uint16_t, (size_t) lowRangeBins> lowRangeCount {};
            double fluxFrames = 0.0, fluxEvents = 0.0;

            double onsetSamples = 0.0, onsetEvents = 0.0;
            int strengthCount = 0;
            std::array<std::uint8_t, (size_t) SegmentLedger::maxEvents> strengthBin {};
            double subEnergy = 0.0, midEnergy = 0.0;

            int trialBlocks = 0;                    // 有效块数（全部块，含没交数值的）
            std::array<std::array<std::uint16_t, (size_t) trialTopCount>, (size_t) trialSeries> trialBin {};   // 每路交来的 min(块数, 8) 个块峰值的格
            std::array<double, (size_t) numBlendTrials> blendIntended {}, blendOutput {};

            int subBlockLength = 0, hopLength = 0;  // 由段还原（0 = 还原不了；见 makeEntry）

            // 派生（进队与读档时算）
            int loudnessBin = 0, lowShareBin = 0, peakBin = 0;
            double accumulateCost = 0.0;

            // 进账（admit 时定下，之后不变）
            bool admitted = false;
            float admissionLoudnessFactor = 0.0f, admissionLowFactor = 0.0f, admissionMainness = 0.0f;
            std::uint8_t vote = 0;                  // LiveCommitRecord::newProgramVote
            bool timeDecay = false, newProgram = false, newProgramRangeValid = false;
            float newProgramRangeLu = 0.0f;
            double mainStamp = 0.0, referenceStamp = 0.0;   // 进账（含这一段触发的事件）之后的事件时钟
        };

        // 跟着一份画像走的事件时钟与计时（这一份画像最新那一段进账之后）。
        struct Counters
        {
            double mainClock = 0.0, referenceClock = 0.0;
            double secondsSinceMain = 0.0;          // 最近一次进账 m ≥ mainThreshold 之后的有效音频
            double runSeconds = 0.0;                // 换歌：这一串「像换歌」的段的有效音频（从 runStart 起）
            std::uint64_t runStart = 0;             // 这一串的第一段（到达序号）
            std::uint64_t newProgramCount = 0;
            double activeSeconds = 0.0;
        };

        // 一份画像的输入：前 count 段，窗口 = [first, first + size)。
        struct Context
        {
            std::uint64_t count = 0, first = 0;
            int size = 0;
            Counters counters;
        };

        // 一遍重算的累计（实值）。口径同 MusicAnalyzer.cpp HouseSums 的满编段部分，整数计数换成加权实数。
        struct Sums
        {
            double mass = 0.0;                      // M = Σ f × e（House 的 fullSeconds）
            double gatingEnergy = 0.0, gatingCount = 0.0;          // 主段：门块功率与个数（相对门）
            double rangeEnergy = 0.0, rangeCount = 0.0;            // 4/4 型 LRA
            double frames = 0.0;
            std::array<std::array<double, (size_t) numBands>, (size_t) spectrumTables> spectrum {};
            double lowStereoWeight = 0.0, highStereoWeight = 0.0;
            double fluxFrames = 0.0, fluxEvents = 0.0;
            double onsetSamples = 0.0, onsetEvents = 0.0;
            double subEnergy = 0.0, midEnergy = 0.0;
            double trialBlocks = 0.0;
            std::array<double, (size_t) numBlendTrials> blendIntended {}, blendOutput {};
            double allMass = 0.0, allGatingEnergy = 0.0, allGatingCount = 0.0;   // 响度型 / 被动型的 I（全部段权重）
            double referenceRangeEnergy = 0.0, referenceRangeCount = 0.0;        // 响度型 / 被动型的 LRA（参考权重）
            double referenceMass = 0.0;             // Σ r（可用的段）
        };

        // House 画像各特征的观测充分度（同 MusicAnalyzer.cpp HouseMeasured）。
        struct Measured
        {
            float integrated = 0.0f, range = 0.0f, peak = 0.0f, spectrum = 0.0f, onset = 0.0f;
            float sub = 0.0f, lowStereo = 0.0f, highStereo = 0.0f, trial = 0.0f;
            std::array<float, (size_t) numBands> band {}, stereoBand {};
        };

        struct Factors
        {
            float loudness = 0.0f, low = 0.0f;
        };

        // 一份画像的派生结果（工作区算好、发布时整份拷过去）。
        struct Result
        {
            MusicProfile profile;
            bool referenceValid = false, lowReferenceValid = false;
            float referenceLufs = 0.0f, lowReference = 0.0f;
            double mainMass = 0.0;
            float confidence = 0.0f;
            float oldestMainShare = 0.0f, oldestReferenceShare = 0.0f;
        };

        enum class Step : std::uint8_t
        {
            idle, admit, referenceSweep, referencePick, lowSweep, lowPick, judgeSweep, accumulate, trialPick, gateSweep, finalize,
            clearHistograms, complete
        };

        Impl()
            : slots ((size_t) slotCount),
              lambda ((size_t) windowCapacity, 0.0f), mainness ((size_t) windowCapacity, 0.0f),
              referenceFactor ((size_t) windowCapacity, 0.0), allFactor ((size_t) windowCapacity, 0.0),
              mainFactor ((size_t) windowCapacity, 0.0),
              trialWeights ((size_t) trialSeries * (size_t) trialBins, 0.0),
              publishedMainness ((size_t) windowCapacity, 0.0f), publishedWeight ((size_t) windowCapacity, 0.0)
        {
            trialTop.fill (-1);
            trialLow.fill (trialBins);
            configure (48000.0);
            coldStart();
        }

        //==============================================================================
        // 配置
        double sampleRate = 48000.0;
        int trialBlockLength = 4800;            // 同分析器：一块 = 整数个起音组（48 kHz 4800 样本）
        int defaultSubBlockLength = 4800;       // 同分析器 / IN 表：round(0.1 s × 采样率)
        std::int64_t gridSamples = 240;
        std::int64_t delaySamples = 24000;
        LiveConstants constants;                // 已清洗
        MainSectionModel model = MainSectionModel::loudness;
        bool frozen = false;                    // setFrozenReferencesForTesting
        float frozenReference = 0.0f, frozenLowReference = 0.0f;

        // 段环
        std::vector<Entry> slots;
        std::uint64_t arrived = 0;              // 交来的段（reset 以来）
        std::uint64_t admittedCount = 0;        // 已进账的段
        std::int64_t lastDue = std::numeric_limits<std::int64_t>::min();
        Counters counters;                      // 最近一次进账之后

        // 时间
        bool haveNow = false;
        std::int64_t now = 0;
        std::int64_t nextGrid = 0;

        // 在途的一遍
        Step step = Step::idle;
        Context work;
        int cursor = 0;
        float passReference = 0.0f, passLowReference = 0.0f;
        bool passReferenceValid = false, passLowReferenceValid = false;
        double sweepEffective = 0.0, sweepAll = 0.0, sweepMain = 0.0;   // 新 → 老扫时「比 i 新的段」的和
        int passHop = 0, passSubBlock = 0;      // 窗口里最新一段还原出的 hop / 子块长
        double oldestReferenceWeight = 0.0;
        Sums sums;
        std::array<double, (size_t) histogramBins> loudnessWeights {};
        double loudnessTotal = 0.0;
        std::array<double, (size_t) lowShareBins> lowShareWeights {};
        double lowShareTotal = 0.0;
        std::array<double, (size_t) histogramBins> mainRange {}, referenceRange {};
        std::array<double, (size_t) peakBins> peakWeight {};
        std::array<float, (size_t) peakBins> peakMax {};
        int peakTop = -1;
        std::array<double, (size_t) strengthBins> strengthWeights {};
        std::array<double, (size_t) lowRangeBins> lowRangeWeights {};
        std::vector<float> lambda, mainness;    // 按窗口位置（0 = 最老）
        std::vector<double> referenceFactor, allFactor, mainFactor;
        std::vector<double> trialWeights;       // [路 × trialBins]
        std::array<int, (size_t) trialSeries> trialTop {}, trialLow {};   // 有数的格的范围（空 = −1 / trialBins）
        int trialDirtyEnd = 0;                  // 试算格：窗口位置 [trialCleanEnd, trialDirtyEnd) 的段写过、还没清
        int trialCleanEnd = 0;
        double gatePower = 0.0, gatedEnergy = 0.0, gatedCount = 0.0;
        bool gateValid = false;
        std::array<Feature, (size_t) numTrials> trialCost {};   // trialPick 算好的代价（finalize 时放进画像）
        std::array<Feature, (size_t) numBlendTrials> blendLoss {};
        float trialMeasured = 0.0f;
        Result result;
        LiveCommitRecord pendingRecord;

        // 已发布
        Context published;
        Result publishedResult;
        std::vector<float> publishedMainness;
        std::vector<double> publishedWeight;
        std::int64_t publishedTime = 0;

        // 提交记录（不进状态）
        std::array<LiveCommitRecord, (size_t) commitCapacity> records {};
        std::uint64_t recordTotal = 0;
        PublicationListener* listener = nullptr;

        //==============================================================================
        Entry& entryAt (std::uint64_t sequence) noexcept               { return slots[(size_t) (sequence % (std::uint64_t) slotCount)]; }
        const Entry& entryAt (std::uint64_t sequence) const noexcept   { return slots[(size_t) (sequence % (std::uint64_t) slotCount)]; }

        void configure (double newSampleRate) noexcept
        {
            sampleRate = newSampleRate > 0.0 && std::isfinite (newSampleRate) ? newSampleRate : 48000.0;

            // 同 MusicAnalyzer.cpp configure：起音组长、试算块 = 整数个组、IN 表子块长。
            const auto groupLength = juce::jlimit (1, maximumGroupLength, juce::roundToInt (sampleRate * onsetGroupSeconds));
            const auto groupSeconds = (double) groupLength / sampleRate;
            trialBlockLength = juce::jmax (1, juce::roundToInt (trialBlockSeconds / groupSeconds)) * groupLength;
            defaultSubBlockLength = juce::jmax (1, juce::roundToInt (sampleRate * 0.1));
            gridSamples = juce::jmax ((std::int64_t) 1, (std::int64_t) std::llround (sliceGridSeconds * sampleRate));
            refreshDelay();
        }

        void refreshDelay() noexcept
        {
            delaySamples = (std::int64_t) std::llround (juce::jmin (constants.publishDelaySeconds, 86400.0) * sampleRate);
        }

        const LiveModelConstants& modelConstants() const noexcept
        {
            return model == MainSectionModel::fourOnFloor ? constants.fourOnFloor
                 : model == MainSectionModel::passive ? constants.passive
                                                      : constants.loudness;
        }

        bool fourOnFloor() const noexcept       { return model == MainSectionModel::fourOnFloor; }
        bool needsAllDomain() const noexcept    { return model != MainSectionModel::fourOnFloor; }

        double nominalSecondsOf (const Entry& e) const noexcept
        {
            return e.nominalLength > 0 ? (double) e.nominalLength / sampleRate : 0.0;
        }

        // W_mem：按小节分段 = memoryBars × 小节秒数（段的标称长度），否则 memorySeconds（可为 +∞ = 不遗忘）。
        double memorySecondsOf (const Entry& e) const noexcept
        {
            const auto& mc = modelConstants();
            const auto nominal = nominalSecondsOf (e);

            if (e.mode == SegmentMode::bars && mc.memoryBars > 0.0 && nominal > 0.0)
                return mc.memoryBars * nominal;

            return mc.memorySeconds;
        }

        // M0：同 W_mem 的换算（confidenceBars / confidenceSeconds），按窗口里最新一段的分段方式。
        double confidenceSecondsOf (const Entry* newest) const noexcept
        {
            const auto& mc = modelConstants();

            if (newest != nullptr && newest->mode == SegmentMode::bars && mc.confidenceBars > 0.0 && nominalSecondsOf (*newest) > 0.0)
                return mc.confidenceBars * nominalSecondsOf (*newest);

            return mc.confidenceSeconds;
        }

        //==============================================================================
        // 冷启动（不分配）：窗口、队列、事件、时间轴、已发布的画像。提交序号与环形缓冲不动。
        void coldStart() noexcept
        {
            discardPass();
            arrived = admittedCount = 0;
            lastDue = std::numeric_limits<std::int64_t>::min();
            counters = Counters {};
            haveNow = false;
            now = 0;
            nextGrid = 0;
            step = Step::idle;
            work = Context {};
            published = Context {};
            publishedTime = 0;
            recomputePublished();
        }

        //==============================================================================
        // 进队（segmentFinalized / commitWithMainness）：转成 Entry、定发布时刻。只做定长拷贝与几十次对数（试算格号），O(1)。
        void enqueue (const SegmentLedger::Segment& segment, bool pinned, float pinnedMainness) noexcept
        {
            makeRoomInQueue();
            auto& e = entryAt (arrived);
            makeEntry (segment, e);
            e.pinned = pinned;
            e.pinnedMainness = pinnedMainness;
            e.due = juce::jmax (saturatingAdd (segment.end, delaySamples), lastDue);
            lastDue = e.due;
            ++arrived;
        }

        // 队列满（调用方一直不调 advanceTo）：最早那一段就地算完、立刻发布（LiveProfile.h 文件头第二节最后一条）。新段只能盖掉比已发布
        // 窗口更老的格，所以要先腾出一格。
        void makeRoomInQueue() noexcept
        {
            while (arrived - published.count >= (std::uint64_t) queueCapacity)
            {
                if (step == Step::idle)
                    startNextPass();

                finishPass();
                publish();
            }
        }

        void makeEntry (const SegmentLedger::Segment& s, Entry& e) const noexcept
        {
            e = Entry {};
            e.start = s.start;
            e.end = s.end;
            e.nominalLength = juce::jmax ((std::int64_t) 0, s.nominalLength);
            e.mode = s.mode == SegmentMode::bars || s.mode == SegmentMode::seconds ? s.mode : SegmentMode::none;
            e.usable = usableSegment (s);
            e.loudnessValid = s.loudnessValid && std::isfinite (s.loudnessLufs);
            e.loudnessLufs = e.loudnessValid ? s.loudnessLufs : 0.0f;
            e.lowShareValid = s.lowShareValid && std::isfinite (s.lowShare);
            e.lowShare = e.lowShareValid ? s.lowShare : 0.0f;

            if (! e.usable)
            {
                deriveEntry (e);
                return;
            }

            e.effectiveSeconds = s.effectiveSeconds;
            e.truePeak = s.truePeak;
            e.gatingCount = s.gatingCount;
            e.gatingPower = s.gatingPower;
            e.rangeCount = s.rangeCount;
            e.rangeBin = s.rangeBin;
            e.rangeEnergy = s.rangeEnergy;

            e.frames = (double) s.frames;
            e.spectrum[(size_t) powerTable] = s.powerSum;
            e.spectrum[(size_t) tameTable] = s.tameSum;
            e.spectrum[(size_t) recoverTable] = s.recoverSum;
            e.spectrum[(size_t) deficitTable] = s.deficitWeight;
            e.spectrum[(size_t) validTable] = s.validWeight;
            e.spectrum[(size_t) stereoPowerTable] = s.stereoPowerSum;
            e.spectrum[(size_t) stereoCrossTable] = s.stereoCrossSum;
            e.spectrum[(size_t) stereoWeightTable] = s.stereoWeight;
            e.lowStereoWeight = s.lowStereoWeight;
            e.highStereoWeight = s.highStereoWeight;

            if (s.lowRangeLast >= s.lowRangeFirst)
            {
                e.lowRangeFirst = s.lowRangeFirst;
                e.lowRangeLast = s.lowRangeLast;
                e.lowRangeCount = s.lowRangeCount;
            }

            e.fluxFrames = (double) s.fluxFrames;
            e.fluxEvents = (double) s.fluxEvents;
            e.onsetSamples = (double) s.onsetEffectiveSamples;
            e.onsetEvents = (double) s.onsetEvents;
            e.strengthCount = s.strengthCount;
            e.strengthBin = s.strengthBin;
            e.subEnergy = s.subEnergy;
            e.midEnergy = s.midEnergy;

            // 试算：每路交来的最大 8 个（不足 8 块就是全部块）只留格号；全部块计入总块数（没交数值的块都在这 8 个之下）。
            e.trialBlocks = s.liveTrialBlocks;
            const auto values = juce::jmin (s.liveTrialBlocks, trialTopCount);

            for (size_t series = 0; series < (size_t) trialSeries; ++series)
                for (int k = 0; k < values; ++k)
                    e.trialBin[series][(size_t) k] = (std::uint16_t) trialBinOf (s.liveTrialTop[series][(size_t) k]);

            e.blendIntended = s.liveBlendIntended;
            e.blendOutput = s.liveBlendOutput;

            // Segment 不带分析器的子块长与 Balance 的 hop：由段还原（逐个加起来的时长 ÷ 个数 × 采样率，舍入误差远小于半个样本），
            // 让频谱 / LRA 的时长与 House 画像同一个公式（个数 × 长度 ÷ 采样率）。
            if (s.effectiveSubBlocks > 0 && s.effectiveSeconds > 0.0)
            {
                const auto length = std::llround (s.effectiveSeconds * sampleRate / (double) s.effectiveSubBlocks);
                e.subBlockLength = length >= 1 && length <= (long long) std::numeric_limits<int>::max() ? (int) length : 0;
            }

            if (s.frames > 0 && s.frameSeconds > 0.0)
            {
                const auto length = std::llround (s.frameSeconds * sampleRate / (double) s.frames);
                e.hopLength = length >= 1 && length <= (long long) std::numeric_limits<int>::max() ? (int) length : 0;
            }

            deriveEntry (e);
        }

        // 派生的格号与工作量估计（与常数无关；进队与读档后算）。
        static void deriveEntry (Entry& e) noexcept
        {
            e.loudnessBin = e.loudnessValid ? loudnessBinOf (e.loudnessLufs) : 0;
            e.lowShareBin = e.lowShareValid ? lowShareBinOf (e.lowShare) : 0;
            e.peakBin = e.truePeak > 0.0f ? peakBinOf (e.truePeak) : 0;
            const auto lowRangeSpan = e.lowRangeLast >= e.lowRangeFirst ? e.lowRangeLast - e.lowRangeFirst + 1 : 0;
            const auto trialValues = (int) trialSeries * juce::jmin (e.trialBlocks, trialTopCount);
            e.accumulateCost = costAccumulateBase
                             + costPerValue * (double) (2 * e.gatingCount + 2 * e.rangeCount + lowRangeSpan + e.strengthCount + trialValues);
        }

        //==============================================================================
        // 时间（LiveProfile.h 文件头第二节）。
        void advanceTo (std::int64_t t) noexcept
        {
            if (haveNow && t <= now)
                return;   // 时间只增不减：重复或更早的调用什么都不做

            if (! haveNow)
            {
                // 冷启动后的第一次：时间轴从这里起算，第一片落在 t 之前（含）最近的网格点上（不补冷启动之前的网格点）。
                haveNow = true;
                nextGrid = gridPointAtOrBefore (t, gridSamples);
            }

            for (;;)
            {
                if (step == Step::idle && arrived > published.count)
                    startNextPass();

                if (step == Step::idle)
                    break;

                const auto due = entryAt (work.count - 1).due;

                if (step != Step::complete)
                {
                    if (nextGrid <= t && nextGrid < due)
                    {
                        runSlice();
                        nextGrid = saturatingAdd (nextGrid, gridSamples);
                        continue;
                    }

                    if (due <= t)
                    {
                        finishPass();   // 补完欠的片（大块、离线、成批交段）
                        continue;
                    }

                    break;
                }

                if (due <= t)
                {
                    publish();
                    continue;
                }

                break;
            }

            now = t;

            // 没有在做的一遍时网格点不攒着：下一片落在 t 之后的第一个网格点上。
            if (step == Step::idle || step == Step::complete)
                nextGrid = juce::jmax (nextGrid, gridPointAfter (t, gridSamples));
        }

        // 把还没发布的段全部算完、按序发布（发布时刻照记 D；时间轴不动）。
        void flush() noexcept
        {
            for (;;)
            {
                if (step == Step::idle && arrived > published.count)
                    startNextPass();

                if (step == Step::idle)
                    return;

                finishPass();
                publish();
            }
        }

        // 下一遍：第 published.count + 1 份画像。进账（admit）落在第一片里。
        void startNextPass() noexcept
        {
            work.count = published.count + 1;
            work.size = (int) juce::jmin (work.count, (std::uint64_t) windowCapacity);
            work.first = work.count - (std::uint64_t) work.size;
            step = admittedCount < work.count ? Step::admit : Step::referenceSweep;
            cursor = 0;

            if (step == Step::referenceSweep)
                beginReferenceSweep();
        }

        void runSlice() noexcept
        {
            double spent = 0.0;

            while (step != Step::complete)
            {
                const auto cost = costOfNextStep();

                if (spent > 0.0 && spent + cost > sliceBudgetNanoseconds)
                    break;

                doStep();
                spent += cost;
            }
        }

        void finishPass() noexcept
        {
            while (step != Step::complete)
                doStep();
        }

        double costOfNextStep() const noexcept
        {
            switch (step)
            {
                case Step::admit:           return costAdmit;
                case Step::referenceSweep:  return costReferenceEntry + (needsAllDomain() ? costAllEntry : 0.0);
                case Step::referencePick:
                case Step::lowPick:         return costPick;
                case Step::lowSweep:        return costLowEntry;
                case Step::judgeSweep:      return costJudgeEntry;
                case Step::accumulate:      return cursor < work.size ? entryAt (work.first + (std::uint64_t) cursor).accumulateCost : 0.0;
                case Step::trialPick:       return costTrialSeries;
                case Step::gateSweep:       return cursor < work.size ? costGateEntry + costPerValue * (double) entryAt (work.first + (std::uint64_t) cursor).gatingCount
                                                                      : 0.0;
                case Step::finalize:        return costFinalize;
                case Step::clearHistograms: return costClear;
                case Step::idle:
                case Step::complete:        return 0.0;
            }

            return 0.0;
        }

        // 一步（文件头第二节）。sweep 每步一段；窗口空（冷启动）时各 sweep 一步也不做，直接到下一阶段。
        void doStep() noexcept
        {
            switch (step)
            {
                case Step::admit:
                    admitNewest();
                    step = Step::referenceSweep;
                    cursor = 0;
                    beginReferenceSweep();
                    return;

                case Step::referenceSweep:
                    if (cursor < work.size)
                        referenceOne (work.size - 1 - cursor++);

                    if (cursor >= work.size)
                        step = Step::referencePick;

                    return;

                case Step::referencePick:
                    pickReference();
                    step = Step::lowSweep;
                    cursor = 0;
                    return;

                case Step::lowSweep:
                    if (cursor < work.size)
                        lowOne (cursor++);

                    if (cursor >= work.size)
                        step = Step::lowPick;

                    return;

                case Step::lowPick:
                    pickLowReference();
                    step = Step::judgeSweep;
                    cursor = 0;
                    sweepMain = 0.0;
                    return;

                case Step::judgeSweep:
                    if (cursor < work.size)
                        judgeOne (work.size - 1 - cursor++);

                    if (cursor >= work.size)
                    {
                        step = Step::accumulate;
                        cursor = 0;
                        beginAccumulate();
                    }

                    return;

                case Step::accumulate:
                    if (cursor < work.size)
                        accumulateOne (cursor++);

                    if (cursor >= work.size)
                    {
                        step = Step::trialPick;
                        cursor = 0;
                    }

                    return;

                case Step::trialPick:
                    trialOne (cursor++);

                    if (cursor >= trialSeries)
                    {
                        step = Step::gateSweep;
                        cursor = 0;
                        beginGateSweep();
                    }

                    return;

                case Step::gateSweep:
                    if (cursor < work.size)
                        gateOne (cursor++);

                    if (cursor >= work.size)
                        step = Step::finalize;

                    return;

                case Step::finalize:
                    finalizeProfile();
                    step = Step::clearHistograms;
                    return;

                case Step::clearHistograms:
                    clearHistograms();
                    step = Step::complete;
                    return;

                case Step::idle:
                case Step::complete:
                    return;
            }
        }

        //==============================================================================
        // 进账（文件头第一节）：按上一份已发布画像的参考判一次，定换歌与无主段的事件、印记。
        void admitNewest() noexcept
        {
            const auto sequence = work.count - 1;
            auto& e = entryAt (sequence);
            const auto f = factorsFor (e.loudnessValid, e.loudnessLufs, e.lowShareValid, e.lowShare);
            const auto m = ! e.usable ? 0.0f : e.pinned ? e.pinnedMainness : f.loudness * f.low;
            e.admissionLoudnessFactor = f.loudness;
            e.admissionLowFactor = f.low;
            e.admissionMainness = m;
            e.vote = 0;
            e.timeDecay = e.newProgram = e.newProgramRangeValid = false;
            e.newProgramRangeLu = 0.0f;

            if (e.usable)
            {
                noteNewProgram (sequence, e);
                noteMainness (m, e);
                counters.activeSeconds += e.effectiveSeconds;
            }
            else
            {
                e.vote = 3;   // 坏段：不算数也不清零
            }

            e.mainStamp = counters.mainClock;
            e.referenceStamp = counters.referenceClock;
            e.admitted = true;
            admittedCount = work.count;
            work.counters = counters;
        }

        // 主段度的两个因子：参考 = 已发布那一份（或冻结的）；没有参考时以这份读数自己为参考（两道参考各自冷启动）。
        Factors factorsFor (bool loudnessValid, float loudnessLufs, bool lowShareValid, float lowShare) const noexcept
        {
            const auto referenceValid = frozen || publishedResult.referenceValid;
            const auto reference = frozen ? frozenReference : publishedResult.referenceLufs;
            const auto lowReferenceValid = frozen || publishedResult.lowReferenceValid;
            const auto lowReference = frozen ? frozenLowReference : publishedResult.lowReference;
            Factors f;

            if (! loudnessValid || ! std::isfinite (loudnessLufs))
                return f;   // 没有有效音频：不是主段

            if (model == MainSectionModel::passive)
            {
                f.loudness = f.low = 1.0f;
                return f;
            }

            const auto& mc = modelConstants();
            const auto gate = (double) mc.gateWidthLu;
            const auto above = referenceValid ? (double) loudnessLufs - ((double) reference - gate) : gate;
            f.loudness = (float) ramp (above, (double) mc.loudnessRampLu);
            f.low = lowFactorOf (lowShareValid, lowShare, lowReferenceValid, lowReference);
            return f;
        }

        // 4/4 型的低频因子（其余模型、或低频门系数 ≤ 0：1）。低频读不出：过不了低频门（同 SegmentLedger）。
        float lowFactorOf (bool lowShareValid, float lowShare, bool lowReferenceValid, float lowReference) const noexcept
        {
            const auto& mc = modelConstants();

            if (! fourOnFloor() || ! (mc.lowGateFactor > 0.0f))
                return 1.0f;

            if (! lowShareValid || ! std::isfinite (lowShare))
                return 0.0f;

            const auto ratio = lowReferenceValid ? (double) lowShare / (double) lowReference : 1.0;
            return (float) ramp (ratio - (double) mc.lowGateFactor, (double) mc.lowRampWidth);
        }

        // 换歌软遗忘（Max 2026-10-02「响的算、轻的要有低音」+「轻的那一支要电平稳」）：
        //   票 1 = 比上一份画像的 I 响 newProgramLu 以上；票 2 = 4/4 型里轻 newProgramLu 以上且低频因子 ≥ 0.5；票 0 = 都不是（清零重计）；
        //   票 3 = 段响度或 I 读不出（不算数也不清零）。这一串（票 1 / 2）的有效音频累计满 newProgramSeconds 之后，每来一段量一次「最近
        //   newProgramSeconds 的那一截」：全是票 1，或段响度起伏（最大 − 最小）≤ newProgramRangeLu，才触发——带低音的淡出一路往下掉不算。
        //   触发：两个事件时钟加 ln(1 ÷ keep)（全部更老的段 × keep），这一串清零重来。比较都是严格大于。
        void noteNewProgram (std::uint64_t sequence, Entry& e) noexcept
        {
            const auto& level = publishedResult.profile.integratedLufs;

            if (! e.loudnessValid || ! level.valid || ! std::isfinite (level.value))
            {
                e.vote = 3;
                return;
            }

            const auto threshold = (double) constants.newProgramLu;
            const auto difference = (double) e.loudnessLufs - (double) level.value;

            if (difference > threshold)
                e.vote = 1;
            else if (fourOnFloor() && -difference > threshold && e.admissionLowFactor >= 0.5f)
                e.vote = 2;
            else
                e.vote = 0;

            if (e.vote == 0)
            {
                counters.runSeconds = 0.0;
                counters.runStart = sequence + 1;
                return;
            }

            counters.runSeconds += e.effectiveSeconds;

            if (! (counters.runSeconds >= constants.newProgramSeconds))
                return;

            // 最近 newProgramSeconds 的那一截：从这一段往回，跳过票 3，最早到这一串的开头（也不早于窗口的开头）。
            const auto earliest = juce::jmax (counters.runStart, work.first);
            double seconds = 0.0;
            auto lowest = std::numeric_limits<double>::infinity(), highest = -std::numeric_limits<double>::infinity();
            auto onlyLouder = true;

            for (auto s = sequence + 1; s > earliest; --s)
            {
                const auto& previous = entryAt (s - 1);

                if (previous.vote == 3)
                    continue;

                seconds += previous.effectiveSeconds;
                lowest = juce::jmin (lowest, (double) previous.loudnessLufs);
                highest = juce::jmax (highest, (double) previous.loudnessLufs);
                onlyLouder = onlyLouder && previous.vote == 1;

                if (seconds >= constants.newProgramSeconds)
                    break;
            }

            e.newProgramRangeValid = true;
            e.newProgramRangeLu = (float) (highest - lowest);

            if (! (seconds >= constants.newProgramSeconds) || ! (onlyLouder || highest - lowest <= (double) constants.newProgramRangeLu))
                return;

            const auto keep = (double) constants.newProgramKeep;
            const auto logKeep = keep > 0.0 ? juce::jmin (-std::log (keep), maximumLogKeep) : maximumLogKeep;
            counters.mainClock += logKeep;
            counters.referenceClock += logKeep;
            ++counters.newProgramCount;
            counters.runSeconds = 0.0;
            counters.runStart = sequence + 1;
            e.newProgram = true;
        }

        // 长时间无主段：上一次进账 m ≥ mainThreshold 之后的有效音频超过 noMainSeconds 的那一部分，按 ÷ noMainDecaySeconds 加进主段时钟
        // （乘在更老的主段上）。只算越过门槛之后的秒数，所以与段怎么切无关。
        void noteMainness (float m, Entry& e) noexcept
        {
            if (m >= constants.mainThreshold)
            {
                counters.secondsSinceMain = 0.0;
                return;
            }

            const auto before = counters.secondsSinceMain;
            counters.secondsSinceMain += e.effectiveSeconds;
            const auto excess = juce::jmax (0.0, counters.secondsSinceMain - juce::jmax (before, constants.noMainSeconds));

            if (excess > 0.0)
            {
                counters.mainClock += excess / constants.noMainDecaySeconds;
                e.timeDecay = true;
            }
        }

        //==============================================================================
        // 参考（新 → 老）：参考权重 ρ、全部段权重 a、段响度直方图；窗口里最新一段的 hop / 子块长。
        void beginReferenceSweep() noexcept
        {
            sweepEffective = sweepAll = 0.0;
            loudnessTotal = 0.0;
            passHop = passSubBlock = 0;
            oldestReferenceWeight = 0.0;
            sums.referenceMass = 0.0;
        }

        void referenceOne (int position) noexcept
        {
            const auto& e = entryAt (work.first + (std::uint64_t) position);
            const auto effective = e.usable ? e.effectiveSeconds : 0.0;
            const auto clockGap = work.counters.referenceClock - e.referenceStamp;
            const auto rho = std::exp (-(sweepEffective / constants.referenceMemorySeconds) - clockGap);
            referenceFactor[(size_t) position] = rho;
            allFactor[(size_t) position] = needsAllDomain() ? std::exp (-sweepAll - clockGap) : 0.0;
            const auto weight = effective * rho;

            if (e.usable)
            {
                sums.referenceMass += weight;

                if (e.loudnessValid && weight > 0.0)
                {
                    loudnessWeights[(size_t) e.loudnessBin] += weight;
                    loudnessTotal += weight;
                }

                if (passHop == 0 && e.hopLength > 0)
                    passHop = e.hopLength;

                if (passSubBlock == 0 && e.subBlockLength > 0)
                    passSubBlock = e.subBlockLength;
            }

            if (position == 0)
                oldestReferenceWeight = weight;

            sweepEffective += effective;
            sweepAll += effective / memorySecondsOf (e);
        }

        void pickReference() noexcept
        {
            if (frozen)
            {
                passReferenceValid = true;
                passReference = frozenReference;
            }
            else
            {
                const auto bin = weightedPercentileBin (loudnessWeights, loudnessTotal, (double) modelConstants().referencePercentile);
                passReferenceValid = bin >= 0;
                passReference = passReferenceValid ? (float) binCentreLufs (bin) : 0.0f;
            }

            loudnessWeights.fill (0.0);
            lowShareTotal = 0.0;
        }

        // 按此刻 S_ref 的响度因子 λ；λ ≥ 0.5 的段的 lowShare 进 lowRef 的直方图（Max 2026-10-02：不再由「当时过没过响度门」决定）。
        void lowOne (int position) noexcept
        {
            const auto& e = entryAt (work.first + (std::uint64_t) position);
            auto factor = 0.0f;

            if (e.usable && e.loudnessValid)
            {
                if (model == MainSectionModel::passive)
                {
                    factor = 1.0f;
                }
                else
                {
                    const auto& mc = modelConstants();
                    const auto gate = (double) mc.gateWidthLu;
                    const auto above = passReferenceValid ? (double) e.loudnessLufs - ((double) passReference - gate) : gate;
                    factor = (float) ramp (above, (double) mc.loudnessRampLu);
                }
            }

            lambda[(size_t) position] = factor;
            const auto weight = (e.usable ? e.effectiveSeconds : 0.0) * referenceFactor[(size_t) position];

            if (factor >= 0.5f && e.lowShareValid && weight > 0.0)
            {
                lowShareWeights[(size_t) e.lowShareBin] += weight;
                lowShareTotal += weight;
            }
        }

        void pickLowReference() noexcept
        {
            if (frozen)
            {
                passLowReferenceValid = true;
                passLowReference = frozenLowReference;
            }
            else
            {
                const auto bin = weightedPercentileBin (lowShareWeights, lowShareTotal, (double) modelConstants().referencePercentile);
                passLowReferenceValid = bin >= 0;
                passLowReference = passLowReferenceValid ? (float) (((double) bin + 0.5) / (double) lowShareBins) : 0.0f;
            }

            lowShareWeights.fill (0.0);
        }

        // 重判（新 → 老）：m′ 与主段权重 f = m′ × exp(−比它新的主段进账 ÷ W_mem − Δ主段时钟)。
        void judgeOne (int position) noexcept
        {
            const auto& e = entryAt (work.first + (std::uint64_t) position);
            auto m = 0.0f;

            if (e.usable)
                m = e.pinned ? e.pinnedMainness
                             : lambda[(size_t) position] * lowFactorOf (e.lowShareValid, e.lowShare, passLowReferenceValid, passLowReference);

            mainness[(size_t) position] = m;
            mainFactor[(size_t) position] = m > 0.0f ? (double) m * std::exp (-sweepMain - (work.counters.mainClock - e.mainStamp)) : 0.0;
            sweepMain += (double) m * (e.usable ? e.effectiveSeconds : 0.0) / memorySecondsOf (e);
        }

        //==============================================================================
        // 累计（老 → 新，顺序同 House 按存进账本的先后求和）。
        void beginAccumulate() noexcept
        {
            const auto referenceMass = sums.referenceMass;
            sums = Sums {};
            sums.referenceMass = referenceMass;
            peakTop = -1;
            trialTop.fill (-1);
            trialLow.fill (trialBins);
            trialDirtyEnd = trialCleanEnd = 0;
        }

        void accumulateOne (int position) noexcept
        {
            const auto& e = entryAt (work.first + (std::uint64_t) position);
            const auto f = mainFactor[(size_t) position];

            if (f > 0.0)
                addMain (e, f);

            trialDirtyEnd = position + 1;

            if (needsAllDomain() && e.usable)
            {
                const auto a = allFactor[(size_t) position];
                sums.allMass += a * e.effectiveSeconds;

                for (int k = 0; k < e.gatingCount; ++k)
                {
                    sums.allGatingEnergy += a * (double) e.gatingPower[(size_t) k];
                    sums.allGatingCount += a;
                }

                const auto rho = referenceFactor[(size_t) position];

                for (int k = 0; k < e.rangeCount; ++k)
                    referenceRange[(size_t) e.rangeBin[(size_t) k]] += rho;

                sums.referenceRangeEnergy += e.rangeEnergy * rho;
                sums.referenceRangeCount += (double) e.rangeCount * rho;
            }
        }

        // 这一段 × f 并进主段的和。逐项同 MusicAnalyzer.cpp computeHouseProfile 对一个满编段做的累加（f = 1 时逐位相同）。
        void addMain (const Entry& e, double f) noexcept
        {
            auto& s = sums;
            const auto weight = f * e.effectiveSeconds;
            s.mass += weight;

            for (int k = 0; k < e.gatingCount; ++k)
            {
                s.gatingEnergy += f * (double) e.gatingPower[(size_t) k];
                s.gatingCount += f;
            }

            // 4/4 型的 LRA（Max 2026-10-02「按曲风分口径」）：3 s 短期响度的格号按 f 计入（同 House「LRA 只用满编段」）。
            for (int k = 0; k < e.rangeCount; ++k)
                mainRange[(size_t) e.rangeBin[(size_t) k]] += f;

            s.rangeEnergy += e.rangeEnergy * f;
            s.rangeCount += (double) e.rangeCount * f;

            if (e.truePeak > 0.0f && weight > 0.0)
            {
                peakWeight[(size_t) e.peakBin] += weight;
                peakMax[(size_t) e.peakBin] = juce::jmax (peakMax[(size_t) e.peakBin], e.truePeak);
                peakTop = juce::jmax (peakTop, e.peakBin);
            }

            s.frames += e.frames * f;

            for (size_t table = 0; table < (size_t) spectrumTables; ++table)
                for (size_t b = 0; b < (size_t) numBands; ++b)
                    s.spectrum[table][b] += e.spectrum[table][b] * f;

            s.lowStereoWeight += e.lowStereoWeight * f;
            s.highStereoWeight += e.highStereoWeight * f;

            for (int bin = e.lowRangeFirst; bin <= e.lowRangeLast; ++bin)
                lowRangeWeights[(size_t) bin] += (double) e.lowRangeCount[(size_t) bin] * f;

            s.fluxFrames += e.fluxFrames * f;
            s.fluxEvents += e.fluxEvents * f;
            s.onsetSamples += e.onsetSamples * f;
            s.onsetEvents += e.onsetEvents * f;

            for (int k = 0; k < e.strengthCount; ++k)
                strengthWeights[(size_t) e.strengthBin[(size_t) k]] += f;

            s.subEnergy += e.subEnergy * f;
            s.midEnergy += e.midEnergy * f;

            if (e.trialBlocks > 0)
            {
                s.trialBlocks += (double) e.trialBlocks * f;
                const auto values = juce::jmin (e.trialBlocks, trialTopCount);

                for (size_t series = 0; series < (size_t) trialSeries; ++series)
                {
                    auto* weights = trialWeights.data() + series * (size_t) trialBins;

                    for (int k = 0; k < values; ++k)
                    {
                        const auto bin = (int) e.trialBin[series][(size_t) k];
                        weights[bin] += f;
                        trialTop[series] = juce::jmax (trialTop[series], bin);
                        trialLow[series] = juce::jmin (trialLow[series], bin);
                    }
                }
            }

            for (size_t c = 0; c < (size_t) numBlendTrials; ++c)
            {
                s.blendIntended[c] += e.blendIntended[c] * f;
                s.blendOutput[c] += e.blendOutput[c] * f;
            }
        }

        //==============================================================================
        // 试算（同 houseTrialProfile：块数 = 加权的有效块数，P99 由每路的直方图按 trialPercentileOf 求）。每步一路：第 0 步是原始。
        void trialOne (int series) noexcept
        {
            const auto n = sums.trialBlocks;
            const auto seconds = n * (double) trialBlockLength / sampleRate;

            if (series == 0)
            {
                trialMeasured = fraction (seconds, trialFullSeconds);
                trialOriginal = n > 0.0 ? trialPercentileOf (0, n) : 0.0;
                trialEnough = seconds >= trialMinimumSeconds && trialOriginal > 0.0;

                for (size_t c = 0; c < (size_t) numBlendTrials; ++c)
                {
                    auto& loss = blendLoss[c];

                    if (trialEnough && sums.blendIntended[c] > 0.0 && sums.blendOutput[c] > 0.0)
                        setFeature (loss, 10.0 * std::log10 (sums.blendIntended[c] / sums.blendOutput[c]), seconds);
                    else
                        invalidate (loss);
                }

                return;
            }

            auto& cost = trialCost[(size_t) (series - 1)];

            if (! trialEnough)
            {
                invalidate (cost);
                return;
            }

            const auto candidate = trialPercentileOf (series, n);
            setFeature (cost, 20.0 * std::log10 (juce::jmax (candidate, trialPeakFloorRatio * trialOriginal) / trialOriginal), seconds);
        }

        double trialOriginal = 0.0;
        bool trialEnough = false;

        // 第 series 路的加权 P99（线性幅度）：numpy 线性插值（= houseTrialProfile 的 percentile）对权重的推广。
        //   条目 = 有权重的格（值取格中心），按值升序在「质量坐标」上排开：条目 k 的起点 S_k = 它下面的质量（总块数 W 减去从顶往下累计到
        //   它为止的质量；W 里含没交数值的块，它们都在每段交来的 8 个之下）。条目先占 max(0, u_k − 1) 长的平台（值不变），再用 min(u_k, 1)
        //   的长度线性过渡到上面一格的值；目标位置 = q × (W − min(u_顶, 1))。等权（u = 1）时平台长 0、过渡长 1、目标 = q × (n − 1)：
        //   就是 numpy（与 House 同式）；整数权重时与「拆成 u 个等权条目」相同，所以几个块落进同一格不改变结果（只差格中心与真值）。
        //   从顶往下扫，几格就到。目标落进「没交数值的块」里（只在一段占了尾部超过 8 块时）取记下的最低一格。
        double trialPercentileOf (int series, double total) const noexcept
        {
            const auto top = trialTop[(size_t) series];

            if (top < 0)
                return 0.0;

            const auto* weights = trialWeights.data() + (size_t) series * (size_t) trialBins;
            const auto topMass = weights[top];
            const auto target = trialPercentile * (total - juce::jmin (topMass, 1.0));
            double above = 0.0;
            int previous = -1;

            for (int bin = top; bin >= trialLow[(size_t) series]; --bin)
            {
                const auto mass = weights[bin];

                if (! (mass > 0.0))
                    continue;

                const auto start = total - (above + mass);

                if (start <= target)
                {
                    const auto offset = target - start;
                    const auto plateau = juce::jmax (0.0, mass - 1.0);
                    const auto value = trialBinLinear (bin);

                    if (offset <= plateau || previous < 0)
                        return value;

                    return value + (trialBinLinear (previous) - value) * ((offset - plateau) / (mass - plateau));
                }

                above += mass;
                previous = bin;
            }

            return previous >= 0 ? trialBinLinear (previous) : 0.0;
        }

        //==============================================================================
        // I 的第二遍门限（老 → 新）：同 House computeHouseProfile——门块功率逐个与相对门比（4/4 型主段权重，响度型 / 被动型全部段权重）；
        // 顺手把这一段写过的试算格清零（下一遍从干净的直方图起）。
        void beginGateSweep() noexcept
        {
            const auto energy = fourOnFloor() ? sums.gatingEnergy : sums.allGatingEnergy;
            const auto count = fourOnFloor() ? sums.gatingCount : sums.allGatingCount;
            gateValid = count > 0.0;
            gatedEnergy = gatedCount = 0.0;

            if (gateValid)
            {
                const auto relativeGate = energyToLufs (energy / count) + integratedRelativeGateLu;
                gatePower = std::pow (10.0, 0.1 * (relativeGate + 0.691));
            }
        }

        void gateOne (int position) noexcept
        {
            const auto& e = entryAt (work.first + (std::uint64_t) position);
            const auto weight = fourOnFloor() ? mainFactor[(size_t) position] : (e.usable ? allFactor[(size_t) position] : 0.0);

            if (gateValid && weight > 0.0)
                for (int k = 0; k < e.gatingCount; ++k)
                {
                    const auto power = (double) e.gatingPower[(size_t) k];

                    if (power >= gatePower)
                    {
                        gatedEnergy += weight * power;
                        gatedCount += weight;
                    }
                }

            clearTrialBinsOf (position);
            trialCleanEnd = position + 1;
        }

        void clearTrialBinsOf (int position) noexcept
        {
            const auto& e = entryAt (work.first + (std::uint64_t) position);

            if (! (mainFactor[(size_t) position] > 0.0) || e.trialBlocks <= 0)
                return;

            const auto values = juce::jmin (e.trialBlocks, trialTopCount);

            for (size_t series = 0; series < (size_t) trialSeries; ++series)
            {
                auto* weights = trialWeights.data() + series * (size_t) trialBins;

                for (int k = 0; k < values; ++k)
                    weights[e.trialBin[series][(size_t) k]] = 0.0;
            }
        }

        // 其余直方图清零（下一遍从干净的起）。
        void clearHistograms() noexcept
        {
            mainRange.fill (0.0);
            referenceRange.fill (0.0);

            if (peakTop >= 0)
            {
                std::fill (peakWeight.begin(), peakWeight.begin() + peakTop + 1, 0.0);
                std::fill (peakMax.begin(), peakMax.begin() + peakTop + 1, 0.0f);
            }

            peakTop = -1;
            strengthWeights.fill (0.0);
            lowRangeWeights.fill (0.0);
            trialTop.fill (-1);
            trialLow.fill (trialBins);
        }

        // 在途的一遍作废（reset / setConstants / 读档）：把它写过、还没清的直方图清干净。窗口里的段此刻都还在（不会被盖掉）。
        void discardPass() noexcept
        {
            if (step == Step::accumulate || step == Step::trialPick || step == Step::gateSweep)
                for (auto position = trialCleanEnd; position < trialDirtyEnd; ++position)
                    clearTrialBinsOf (position);

            trialDirtyEnd = trialCleanEnd = 0;
            loudnessWeights.fill (0.0);
            lowShareWeights.fill (0.0);
            peakTop = juce::jmax (peakTop, peakBins - 1);   // 不知道写到哪一格：整张清
            clearHistograms();
        }

        //==============================================================================
        // 画像（文件头第四节）。同 MusicAnalyzer.cpp computeHouseProfile 的分工。
        //
        // formatVersion = clayProfileFormatVersion（2）、houseGate 不填（valid = false）：决策器（GlazePlanner.cpp）两项都不读——只读各特征的
        // value / valid / confidence、representativeness、bandConfidence；编排器（GlazeController.cpp）用 formatVersion ≥ 2 判「方案源带
        // Clay 特征」——实时画像带着，所以是 2；用 formatVersion ≥ 3 且 houseGate.valid 判「House 档锁定过的画像」——那是 T-038 学习 → 锁定
        // 的机制，实时画像没有锁定；S_ref / lowRef 从 getDiagnostics 读。
        void finalizeProfile() noexcept
        {
            auto& r = result;
            r.profile = MusicProfile {};
            r.profile.formatVersion = clayProfileFormatVersion;
            r.referenceValid = passReferenceValid;
            r.referenceLufs = passReference;
            r.lowReferenceValid = passLowReferenceValid;
            r.lowReference = passLowReference;
            r.mainMass = sums.mass;

            const auto* newest = work.size > 0 ? &entryAt (work.count - 1) : nullptr;
            r.confidence = sums.mass > 0.0 ? (float) (1.0 - std::exp (-sums.mass / confidenceSecondsOf (newest))) : 0.0f;
            r.oldestMainShare = work.size > 0 && sums.mass > 0.0 ? (float) (mainFactor[0] * entryAt (work.first).effectiveSeconds / sums.mass) : 0.0f;
            r.oldestReferenceShare = sums.referenceMass > 0.0 ? (float) (oldestReferenceWeight / sums.referenceMass) : 0.0f;

            Measured measured;
            loudnessProfile (measured);
            const auto fluxOnsetsValid = spectrumProfile (measured);
            onsetProfile (measured);
            subProfile (measured);

            r.profile.lowCutPeakCostDb = trialCost[(size_t) trialLowCut];
            r.profile.widthPeakCostDb = trialCost[(size_t) trialWidth];

            for (size_t c = 0; c < (size_t) numMonoTrials; ++c)
                r.profile.monoPeakCostDb[c] = trialCost[(size_t) trialFirstMono + c];

            r.profile.lowCutBlendLossDb = blendLoss;
            measured.trial = work.size > 0 ? trialMeasured : 0.0f;
            confidences (measured, fluxOnsetsValid);

            if (work.size > 0)
                writeRecord (*newest);
        }

        // 这一份画像最新那一段的提交记录（发布时写进环形缓冲）。
        void writeRecord (const Entry& e) noexcept
        {
            const auto position = (size_t) (work.size - 1);
            auto& record = pendingRecord;
            record = LiveCommitRecord {};
            record.start = e.start;
            record.end = e.end;
            record.publishTime = e.due;
            record.nominalSeconds = nominalSecondsOf (e);
            record.mode = e.mode;
            record.effectiveSeconds = e.usable ? e.effectiveSeconds : 0.0;
            record.loudnessValid = e.loudnessValid;
            record.loudnessLufs = e.loudnessLufs;
            record.lowShareValid = e.lowShareValid;
            record.lowShare = e.lowShare;
            record.admissionLoudnessFactor = e.admissionLoudnessFactor;
            record.admissionLowFactor = e.admissionLowFactor;
            record.admissionMainness = e.admissionMainness;
            record.loudnessFactor = e.usable ? lambda[position] : 0.0f;
            record.lowFactor = e.usable ? lowFactorOf (e.lowShareValid, e.lowShare, passLowReferenceValid, passLowReference) : 0.0f;
            record.mainness = mainness[position];
            record.weightSeconds = (double) mainness[position] * record.effectiveSeconds;
            record.referenceValid = passReferenceValid;
            record.referenceLufs = passReference;
            record.lowReferenceValid = passLowReferenceValid;
            record.lowReference = passLowReference;
            record.mainLevelValid = result.profile.integratedLufs.valid;
            record.mainLevelLufs = result.profile.integratedLufs.value;
            record.mainMassSeconds = result.mainMass;
            record.confidence = result.confidence;
            record.timeDecay = e.timeDecay;
            record.newProgram = e.newProgram;
            record.newProgramVote = e.vote;
            record.newProgramRangeValid = e.newProgramRangeValid;
            record.newProgramRangeLu = e.newProgramRangeLu;
        }

        // 同 houseLoudnessProfile：I（gateSweep 的结果）、LRA（4/4 型主段，响度型 / 被动型参考权重）、真峰值、PLR。
        void loudnessProfile (Measured& measured) noexcept
        {
            auto& p = result.profile;
            const auto mainSeconds = sums.mass;
            p.effectiveSeconds = mainSeconds;

            const auto integratedSeconds = fourOnFloor() ? mainSeconds : sums.allMass;
            const auto integratedValid = gateValid && gatedCount > 0.0;
            const auto integrated = integratedValid ? energyToLufs (gatedEnergy / gatedCount) : 0.0;

            if (integratedValid)
                setFeature (p.integratedLufs, integrated, integratedSeconds);
            else
                invalidate (p.integratedLufs);

            measured.integrated = fraction (integratedSeconds, loudnessFullSeconds);

            // LRA：同 houseLoudnessProfile 的 LRA 段（整数个数换成加权的个数：index = ⌊(n − 1) × 分位 + 0.5⌋，累计 > index 的那一格）。
            const auto& range = fourOnFloor() ? mainRange : referenceRange;
            const auto rangeEnergy = fourOnFloor() ? sums.rangeEnergy : sums.referenceRangeEnergy;
            const auto rangeCount = fourOnFloor() ? sums.rangeCount : sums.referenceRangeCount;
            const auto subBlock = passSubBlock > 0 ? passSubBlock : defaultSubBlockLength;
            const auto rangeSeconds = rangeCount * (double) subBlock / sampleRate;
            invalidate (p.loudnessRangeLu);

            if (rangeCount > 0.0)
            {
                const auto relativeGate = energyToLufs (rangeEnergy / rangeCount) + lraRelativeGateLu;
                const auto firstBin = firstBinAtOrAbove (relativeGate);
                double gated = 0.0;

                for (int bin = firstBin; bin < histogramBins; ++bin)
                    gated += range[(size_t) bin];

                if (gated >= minimumLraValues)
                {
                    const auto lowIndex = std::floor ((gated - 1.0) * lraLowFraction + 0.5);
                    const auto highIndex = std::floor ((gated - 1.0) * lraHighFraction + 0.5);
                    double low = 0.0, high = 0.0, cumulative = 0.0;
                    bool haveLow = false;

                    for (int bin = firstBin; bin < histogramBins; ++bin)
                    {
                        cumulative += range[(size_t) bin];

                        if (! haveLow && cumulative > lowIndex)
                        {
                            low = binCentreLufs (bin);
                            haveLow = true;
                        }

                        if (cumulative > highIndex)
                        {
                            high = binCentreLufs (bin);
                            break;
                        }
                    }

                    setFeature (p.loudnessRangeLu, high - low, rangeSeconds);
                }
            }

            p.loudnessRangeLu.effectiveSeconds = rangeSeconds;
            measured.range = fraction (rangeSeconds, lraFullSeconds);

            const auto peakBin = peakTailBin();

            if (peakBin >= 0)
                setFeature (p.truePeakDbtp, juce::Decibels::gainToDecibels ((double) peakMax[(size_t) peakBin]), mainSeconds);
            else
                invalidate (p.truePeakDbtp);

            measured.peak = fraction (mainSeconds, loudnessFullSeconds);

            if (p.integratedLufs.valid && p.truePeakDbtp.valid)
                setFeature (p.plrDb, (double) p.truePeakDbtp.value - integrated, mainSeconds);
            else
                invalidate (p.plrDb);
        }

        // 真峰值的格：从高往低累计主段质量，第一次 ≥ peakTailSeconds 的那一格；全部加起来都不到（冷启动）取最高一格（= 最大值）。
        int peakTailBin() const noexcept
        {
            if (peakTop < 0)
                return -1;

            double cumulative = 0.0;

            for (int bin = peakTop; bin >= 0; --bin)
            {
                const auto weight = peakWeight[(size_t) bin];

                if (! (weight > 0.0))
                    continue;

                cumulative += weight;

                if (cumulative >= constants.peakTailSeconds)
                    return bin;
            }

            return peakTop;
        }

        // 同 houseSpectrumProfile（主段加权的和）。返回辅助量「通量突起密度」是否有效（起音一致性要用；它本身不进画像）。
        bool spectrumProfile (Measured& measured) noexcept
        {
            auto& p = result.profile;
            const auto& s = sums;
            const auto& powerSum = s.spectrum[(size_t) powerTable];
            const auto hopSeconds = passHop > 0 ? (double) passHop / sampleRate : 0.0;
            const auto frameSeconds = s.frames * hopSeconds;
            std::array<double, (size_t) numBands> meanPower {}, levelDb {};
            double total = 0.0, low = 0.0, high = 0.0;

            for (int b = 0; b < numBands; ++b)
            {
                const auto i = (size_t) b;
                meanPower[i] = s.frames > 0.0 ? powerSum[i] / s.frames : 0.0;
                levelDb[i] = meanPower[i] > 0.0 ? juce::jmax (noDataDb, 10.0 * std::log10 (meanPower[i])) : noDataDb;
                p.bandLongTermDb[i] = (float) levelDb[i];

                total += meanPower[i];

                if (b <= lowShareLastBand)
                    low += meanPower[i];

                if (b >= highShareFirstBand)
                    high += meanPower[i];
            }

            const auto enoughFrames = frameSeconds >= minimumSpectrumSeconds && total > 0.0;

            if (enoughFrames)
            {
                setFeature (p.lowShare, low / total, frameSeconds);
                setFeature (p.highShare, high / total, frameSeconds);
            }
            else
            {
                invalidate (p.lowShare);
                invalidate (p.highShare);
            }

            {
                double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
                int bands = 0;

                for (int b = 0; b < numBands; ++b)
                {
                    if (levelDb[(size_t) b] < bandGateDb)
                        continue;

                    const auto x = (double) b / 3.0;
                    const auto y = levelDb[(size_t) b];
                    sx += x;
                    sy += y;
                    sxx += x * x;
                    sxy += x * y;
                    ++bands;
                }

                const auto denominator = (double) bands * sxx - sx * sx;

                if (enoughFrames && bands >= minimumSlopeBands && denominator > 0.0)
                    setFeature (p.spectralSlopeDbPerOct, ((double) bands * sxy - sx * sy) / denominator, frameSeconds);
                else
                    invalidate (p.spectralSlopeDbPerOct);
            }

            std::array<bool, (size_t) numBands> inBand {};
            inBand.fill (true);
            inBand[0] = false;
            inBand[(size_t) numBands - 1] = false;

            for (int b = numBands - 1; b > 0 && levelDb[(size_t) b] < levelDb[(size_t) b - 1] - rollOffStepDb; --b)
                inBand[(size_t) b] = false;

            for (int b = 0; b < numBands - 1 && levelDb[(size_t) b] < levelDb[(size_t) b + 1] - rollOffStepDb; ++b)
                inBand[(size_t) b] = false;

            for (int b = 1; b < numBands - 1; ++b)
            {
                const auto i = (size_t) b;

                if (levelDb[i] < bandGateDb || levelDb[i - 1] < bandGateDb || levelDb[i + 1] < bandGateDb)
                    inBand[i] = false;
            }

            const auto& tameSum = s.spectrum[(size_t) tameTable];
            const auto& recoverSum = s.spectrum[(size_t) recoverTable];
            const auto& deficitWeight = s.spectrum[(size_t) deficitTable];
            const auto& validWeight = s.spectrum[(size_t) validTable];

            for (size_t b = 0; b < (size_t) numBands; ++b)
            {
                const auto weight = validWeight[b];

                if (weight <= 0.0)
                {
                    p.tameEvidenceDb[b] = 0.0f;
                    p.recoverEvidenceDb[b] = 0.0f;
                    measured.band[b] = 0.0f;
                    continue;
                }

                p.tameEvidenceDb[b] = (float) (tameSum[b] / weight);

                const auto persistence = deficitWeight[b] / weight;
                const auto persistenceGain = juce::jlimit (0.0, 1.0, (persistence - recoverPersistenceStart)
                                                                         / (recoverPersistenceFull - recoverPersistenceStart));
                p.recoverEvidenceDb[b] = inBand[b] ? (float) (recoverSum[b] / weight * persistenceGain) : 0.0f;
                measured.band[b] = fraction (weight * hopSeconds, spectrumFullSeconds);
            }

            measured.spectrum = fraction (frameSeconds, spectrumFullSeconds);

            // 辅助量里只有「通量突起密度是否有效」影响画像（起音一致性）；通量均值与它们的置信度不进画像，不算。
            const auto fluxSeconds = s.fluxFrames * hopSeconds;
            const auto fluxOnsetsValid = fluxSeconds >= minimumSpectrumSeconds;

            // 低频起伏：P90 − P10（同 houseSpectrumProfile；整数个数换成加权的个数，口径同 LRA）。
            if (frameSeconds >= minimumSpectrumSeconds)
            {
                const auto lowIndex = std::floor ((s.frames - 1.0) * lowRangeLowFraction + 0.5);
                const auto highIndex = std::floor ((s.frames - 1.0) * lowRangeHighFraction + 0.5);
                double cumulative = 0.0;
                int lowBin = -1, highBin = -1;

                for (int bin = 0; bin < lowRangeBins && highBin < 0; ++bin)
                {
                    cumulative += lowRangeWeights[(size_t) bin];

                    if (lowBin < 0 && cumulative > lowIndex)
                        lowBin = bin;

                    if (cumulative > highIndex)
                        highBin = bin;
                }

                if (lowBin >= 0 && highBin >= 0)
                    setFeature (p.lowBandRangeDb, (double) (highBin - lowBin) * lowRangeBinDb, frameSeconds);
                else
                    invalidate (p.lowBandRangeDb);
            }
            else
            {
                invalidate (p.lowBandRangeDb);
            }

            // 立体声（同 houseSpectrumProfile 的立体声段）。
            const auto& stereoPowerSum = s.spectrum[(size_t) stereoPowerTable];
            const auto& stereoCrossSum = s.spectrum[(size_t) stereoCrossTable];
            const auto& stereoWeight = s.spectrum[(size_t) stereoWeightTable];

            for (size_t b = 0; b < (size_t) numBands; ++b)
            {
                const auto summedPower = stereoPowerSum[b];

                if (stereoWeight[b] > 0.0 && summedPower > 0.0)
                {
                    p.bandSideToMidDb[b] = (float) sideToMidDbOf (summedPower, stereoCrossSum[b]);
                    p.bandCorrelation[b] = (float) correlationOf (summedPower, stereoCrossSum[b]);
                    measured.stereoBand[b] = fraction (stereoWeight[b] * hopSeconds, stereoFullSeconds);
                }
                else
                {
                    p.bandSideToMidDb[b] = 0.0f;
                    p.bandCorrelation[b] = 0.0f;
                    measured.stereoBand[b] = 0.0f;
                }
            }

            stereoGroup (lowStereoFirstBand, lowStereoLastBand, s.lowStereoWeight * hopSeconds,
                         p.lowSideToMidDb, p.lowCorrelation, measured.lowStereo);
            stereoGroup (highStereoFirstBand, highStereoLastBand, s.highStereoWeight * hopSeconds,
                         p.highSideToMidDb, p.highCorrelation, measured.highStereo);
            return fluxOnsetsValid;
        }

        // 同 houseStereoGroup。
        void stereoGroup (int firstBand, int lastBand, double observedSeconds, Feature& sideToMid, Feature& correlation,
                          float& measured) const noexcept
        {
            const auto& stereoPowerSum = sums.spectrum[(size_t) stereoPowerTable];
            const auto& stereoCrossSum = sums.spectrum[(size_t) stereoCrossTable];
            double summedPower = 0.0, summedCross = 0.0;

            for (int b = firstBand; b <= lastBand; ++b)
            {
                summedPower += stereoPowerSum[(size_t) b];
                summedCross += stereoCrossSum[(size_t) b];
            }

            if (observedSeconds >= minimumStereoSeconds && summedPower > 0.0)
            {
                setFeature (sideToMid, sideToMidDbOf (summedPower, summedCross), observedSeconds);
                setFeature (correlation, correlationOf (summedPower, summedCross), observedSeconds);
            }
            else
            {
                invalidate (sideToMid);
                invalidate (correlation);
            }

            measured = fraction (observedSeconds, stereoFullSeconds);
        }

        // 同 houseOnsetProfile（事件数、中位数的「个数」是加权的：middle = ⌊(E − 1) ÷ 2⌋，E 为整数时就是分析器的整数除法）。
        void onsetProfile (Measured& measured) noexcept
        {
            auto& p = result.profile;
            const auto& s = sums;
            const auto seconds = s.onsetSamples / sampleRate;

            if (seconds >= minimumOnsetSeconds)
                setFeature (p.onsetDensityPerSecond, s.onsetEvents / seconds, seconds);
            else
                invalidate (p.onsetDensityPerSecond);

            if (s.onsetEvents >= minimumOnsetEvents)
            {
                const auto middle = std::floor ((s.onsetEvents - 1.0) / 2.0);
                double cumulative = 0.0;
                int medianBin = 0;

                for (int bin = 0; bin < strengthBins; ++bin)
                {
                    cumulative += strengthWeights[(size_t) bin];

                    if (cumulative > middle)
                    {
                        medianBin = bin;
                        break;
                    }
                }

                setFeature (p.onsetStrengthDb, ((double) medianBin + 0.5) * strengthBinDb, seconds);
            }
            else
            {
                invalidate (p.onsetStrengthDb);
            }

            p.onsetDensityPerSecond.effectiveSeconds = seconds;
            p.onsetStrengthDb.effectiveSeconds = seconds;
            measured.onset = fraction (seconds, onsetFullSeconds);
        }

        // 同 houseSubProfile。
        void subProfile (Measured& measured) noexcept
        {
            const auto& s = sums;
            const auto seconds = s.onsetSamples / sampleRate;

            if (seconds >= minimumSubSeconds && s.midEnergy > 0.0)
                setFeature (result.profile.subShareDb, 10.0 * std::log10 (juce::jlimit (subShareFloor, 1.0, s.subEnergy / s.midEnergy)), seconds);
            else
                invalidate (result.profile.subShareDb);

            measured.sub = fraction (seconds, subFullSeconds);
        }

        // 同 houseConfidences，代表性换成 c（混合损失也给，同 T-049 之后的 House 与通用画像）。
        void confidences (const Measured& measured, bool fluxOnsetsValid) noexcept
        {
            auto& p = result.profile;
            const auto& s = sums;
            const auto rep = result.confidence;
            p.representativeness = rep;

            auto finalize = [rep] (Feature& feature, float observed, float factor = 1.0f)
            {
                feature.confidence = feature.valid ? juce::jmin (rep, observed) * factor : 0.0f;
            };

            finalize (p.integratedLufs, measured.integrated);
            finalize (p.loudnessRangeLu, measured.range);
            finalize (p.truePeakDbtp, measured.peak);
            finalize (p.plrDb, juce::jmin (measured.integrated, measured.peak));
            finalize (p.spectralSlopeDbPerOct, measured.spectrum);
            finalize (p.lowShare, measured.spectrum);
            finalize (p.highShare, measured.spectrum);

            auto agreement = 1.0f;

            if (s.onsetEvents > 0.0 && fluxOnsetsValid && s.fluxEvents < s.onsetEvents)
            {
                const auto ratio = s.fluxEvents / s.onsetEvents;
                agreement = (float) (minimumOnsetAgreement + (1.0 - minimumOnsetAgreement) * ratio);
            }

            finalize (p.onsetDensityPerSecond, measured.onset, agreement);
            finalize (p.onsetStrengthDb, measured.onset, agreement);
            finalize (p.lowBandRangeDb, measured.spectrum);

            finalize (p.subShareDb, measured.sub);
            finalize (p.lowSideToMidDb, measured.lowStereo);
            finalize (p.lowCorrelation, measured.lowStereo);
            finalize (p.highSideToMidDb, measured.highStereo);
            finalize (p.highCorrelation, measured.highStereo);

            finalize (p.lowCutPeakCostDb, measured.trial);
            finalize (p.widthPeakCostDb, measured.trial);

            for (auto& cost : p.monoPeakCostDb)
                finalize (cost, measured.trial);

            for (auto& loss : p.lowCutBlendLossDb)
                finalize (loss, measured.trial);

            for (size_t b = 0; b < (size_t) numBands; ++b)
            {
                p.bandConfidence[b] = juce::jmin (rep, measured.band[b]);
                p.bandStereoConfidence[b] = juce::jmin (rep, measured.stereoBand[b]);
            }
        }

        //==============================================================================
        // 发布（文件头第二节）：工作区的结果整份拷进「已发布」，提交记录进环形缓冲，通知监听者。
        void publish() noexcept
        {
            published = work;
            publishedResult = result;
            std::swap (publishedMainness, mainness);
            std::swap (publishedWeight, mainFactor);

            for (int position = 0; position < published.size; ++position)
                publishedWeight[(size_t) position] *= entryAt (published.first + (std::uint64_t) position).effectiveSeconds;

            publishedTime = entryAt (published.count - 1).due;
            step = Step::idle;

            pendingRecord.sequence = recordTotal;
            const auto& record = records[(size_t) (recordTotal % (std::uint64_t) commitCapacity)] = pendingRecord;
            ++recordTotal;

            if (listener != nullptr)
                listener->livePublished (record);
        }

        // 按此刻的常数把已发布那一份重算一遍、就地替换（setConstants / 冻结参考 / 冷启动 / 读档）：不发提交记录。在途的那一遍作废，
        // 从重算开头重来（进账判断不重做）。
        void recomputePublished() noexcept
        {
            const auto hadPass = step != Step::idle;
            discardPass();

            const auto inFlight = work;
            work = published;
            step = Step::referenceSweep;
            cursor = 0;
            beginReferenceSweep();
            finishPass();

            publishedResult = result;
            std::swap (publishedMainness, mainness);
            std::swap (publishedWeight, mainFactor);

            for (int position = 0; position < published.size; ++position)
                publishedWeight[(size_t) position] *= entryAt (published.first + (std::uint64_t) position).effectiveSeconds;

            work = inFlight;
            step = Step::idle;

            if (hadPass)
                startNextPass();
        }

        //==============================================================================
        LiveDiagnostics diagnostics() const noexcept
        {
            const auto& r = publishedResult;
            const auto& c = published.counters;
            LiveDiagnostics d;
            d.commits = published.count;
            d.referenceValid = r.referenceValid;
            d.referenceLufs = r.referenceLufs;
            d.lowReferenceValid = r.lowReferenceValid;
            d.lowReference = r.lowReference;
            d.mainMassSeconds = r.mainMass;
            d.confidence = r.confidence;
            d.activeSeconds = c.activeSeconds;
            d.secondsSinceMain = c.secondsSinceMain;
            d.timeDecayActive = c.secondsSinceMain >= constants.noMainSeconds;
            d.newProgramCount = (std::uint32_t) juce::jmin (c.newProgramCount, (std::uint64_t) std::numeric_limits<std::uint32_t>::max());
            d.windowSegments = published.size;
            d.droppedSegments = published.first;
            d.oldestMainShare = r.oldestMainShare;
            d.oldestReferenceShare = r.oldestReferenceShare;
            d.publishTime = publishedTime;
            d.pendingSegments = (int) (arrived - published.count);
            return d;
        }

        // 同 SegmentLedger::readEvents。
        int readCommits (std::uint64_t& cursorToMove, LiveCommitRecord* out, int maxCount) const noexcept
        {
            auto lost = false;

            if (cursorToMove > recordTotal)
                cursorToMove = recordTotal;

            if (recordTotal - cursorToMove > (std::uint64_t) commitCapacity)
            {
                cursorToMove = recordTotal - (std::uint64_t) commitCapacity;
                lost = true;
            }

            auto count = 0;

            while (out != nullptr && cursorToMove < recordTotal && count < maxCount)
            {
                out[count++] = records[(size_t) (cursorToMove % (std::uint64_t) commitCapacity)];
                ++cursorToMove;
            }

            return lost ? -count : count;
        }

        int readJudgements (LiveSegmentJudgement* out, int maxCount) const noexcept
        {
            if (out == nullptr)
                return published.size;

            const auto count = juce::jmin (maxCount, published.size);

            for (int position = 0; position < count; ++position)
            {
                const auto sequence = published.first + (std::uint64_t) position;
                const auto& e = entryAt (sequence);
                auto& j = out[position];
                j.sequence = sequence;
                j.start = e.start;
                j.end = e.end;
                j.mainness = publishedMainness[(size_t) position];
                j.weightSeconds = publishedWeight[(size_t) position];
            }

            return juce::jmax (0, count);
        }

        std::int64_t nextPublicationTime() const noexcept
        {
            return arrived > published.count ? entryAt (published.count).due : noPublication;
        }

        //==============================================================================
        // 状态 XML（文件头第六节）。
        void writeEntry (juce::MemoryOutputStream& out, const Entry& e, bool admittedPart) const
        {
            out.writeInt64 (e.start);
            out.writeInt64 (e.end);
            out.writeInt64 (e.nominalLength);
            out.writeByte ((char) e.mode);
            out.writeByte ((char) (e.usable ? 1 : 0));
            out.writeByte ((char) (e.pinned ? 1 : 0));
            out.writeFloat (e.pinnedMainness);
            out.writeDouble (e.effectiveSeconds);
            out.writeByte ((char) (e.loudnessValid ? 1 : 0));
            out.writeFloat (e.loudnessLufs);
            out.writeByte ((char) (e.lowShareValid ? 1 : 0));
            out.writeFloat (e.lowShare);
            out.writeFloat (e.truePeak);

            out.writeInt (e.gatingCount);

            for (int k = 0; k < e.gatingCount; ++k)
                out.writeFloat (e.gatingPower[(size_t) k]);

            out.writeInt (e.rangeCount);

            for (int k = 0; k < e.rangeCount; ++k)
                out.writeShort ((short) e.rangeBin[(size_t) k]);

            out.writeDouble (e.rangeEnergy);
            out.writeDouble (e.frames);

            for (const auto& table : e.spectrum)
                for (const auto x : table)
                    out.writeDouble (x);

            out.writeDouble (e.lowStereoWeight);
            out.writeDouble (e.highStereoWeight);
            out.writeInt (e.lowRangeFirst);
            out.writeInt (e.lowRangeLast);

            for (int bin = e.lowRangeFirst; bin <= e.lowRangeLast; ++bin)
                out.writeShort ((short) e.lowRangeCount[(size_t) bin]);

            out.writeDouble (e.fluxFrames);
            out.writeDouble (e.fluxEvents);
            out.writeDouble (e.onsetSamples);
            out.writeDouble (e.onsetEvents);
            out.writeInt (e.strengthCount);

            for (int k = 0; k < e.strengthCount; ++k)
                out.writeByte ((char) e.strengthBin[(size_t) k]);

            out.writeDouble (e.subEnergy);
            out.writeDouble (e.midEnergy);
            out.writeInt (e.trialBlocks);

            for (const auto& series : e.trialBin)
                for (int k = 0; k < juce::jmin (e.trialBlocks, trialTopCount); ++k)
                    out.writeShort ((short) series[(size_t) k]);

            for (size_t c = 0; c < (size_t) numBlendTrials; ++c)
            {
                out.writeDouble (e.blendIntended[c]);
                out.writeDouble (e.blendOutput[c]);
            }

            out.writeInt (e.subBlockLength);
            out.writeInt (e.hopLength);

            // 进账的部分只写已发布窗口里的段；还没发布的段读回后重新进账。
            out.writeByte ((char) (admittedPart ? 1 : 0));

            if (admittedPart)
            {
                out.writeFloat (e.admissionLoudnessFactor);
                out.writeFloat (e.admissionLowFactor);
                out.writeFloat (e.admissionMainness);
                out.writeByte ((char) e.vote);
                out.writeByte ((char) ((e.timeDecay ? 1 : 0) | (e.newProgram ? 2 : 0) | (e.newProgramRangeValid ? 4 : 0)));
                out.writeFloat (e.newProgramRangeLu);
                out.writeDouble (e.mainStamp);
                out.writeDouble (e.referenceStamp);
            }
        }

        bool readEntry (BlobReader& in, Entry& e, bool expectAdmitted) const noexcept
        {
            e = Entry {};
            e.start = in.i64();
            e.end = in.i64();
            e.nominalLength = in.i64();
            const auto mode = in.u8();
            e.mode = mode <= (std::uint8_t) SegmentMode::seconds ? static_cast<SegmentMode> (mode) : SegmentMode::none;
            in.ok = in.ok && mode <= (std::uint8_t) SegmentMode::seconds && e.nominalLength >= 0;
            e.usable = in.flag();
            e.pinned = in.flag();
            e.pinnedMainness = in.f32();
            e.effectiveSeconds = in.f64();
            e.loudnessValid = in.flag();
            e.loudnessLufs = in.f32();
            e.lowShareValid = in.flag();
            e.lowShare = in.f32();
            e.truePeak = in.f32();
            in.ok = in.ok && e.pinnedMainness >= 0.0f && e.pinnedMainness <= 1.0f && e.effectiveSeconds >= 0.0 && e.truePeak >= 0.0f;

            e.gatingCount = in.i32();
            in.ok = in.ok && e.gatingCount >= 0 && e.gatingCount <= SegmentLedger::maxSubBlocks;

            for (int k = 0; in.ok && k < e.gatingCount; ++k)
            {
                e.gatingPower[(size_t) k] = in.f32();
                in.ok = in.ok && e.gatingPower[(size_t) k] >= 0.0f;
            }

            e.rangeCount = in.i32();
            in.ok = in.ok && e.rangeCount >= 0 && e.rangeCount <= SegmentLedger::maxSubBlocks;

            for (int k = 0; in.ok && k < e.rangeCount; ++k)
            {
                e.rangeBin[(size_t) k] = in.u16();
                in.ok = in.ok && (int) e.rangeBin[(size_t) k] < histogramBins;
            }

            e.rangeEnergy = in.f64();
            e.frames = in.f64();
            in.ok = in.ok && e.rangeEnergy >= 0.0 && e.frames >= 0.0;

            for (size_t table = 0; table < (size_t) spectrumTables; ++table)
                for (auto& x : e.spectrum[table])
                {
                    x = in.f64();
                    in.ok = in.ok && (table == (size_t) stereoCrossTable || x >= 0.0);
                }

            e.lowStereoWeight = in.f64();
            e.highStereoWeight = in.f64();
            e.lowRangeFirst = in.i32();
            e.lowRangeLast = in.i32();
            in.ok = in.ok && e.lowStereoWeight >= 0.0 && e.highStereoWeight >= 0.0
                 && ((e.lowRangeFirst == lowRangeBins && e.lowRangeLast == -1)
                     || (e.lowRangeFirst >= 0 && e.lowRangeLast >= e.lowRangeFirst && e.lowRangeLast < lowRangeBins));

            for (int bin = e.lowRangeFirst; in.ok && bin <= e.lowRangeLast; ++bin)
                e.lowRangeCount[(size_t) bin] = in.u16();

            e.fluxFrames = in.f64();
            e.fluxEvents = in.f64();
            e.onsetSamples = in.f64();
            e.onsetEvents = in.f64();
            e.strengthCount = in.i32();
            in.ok = in.ok && e.fluxFrames >= 0.0 && e.fluxEvents >= 0.0 && e.onsetSamples >= 0.0 && e.onsetEvents >= 0.0
                 && e.strengthCount >= 0 && e.strengthCount <= SegmentLedger::maxEvents;

            for (int k = 0; in.ok && k < e.strengthCount; ++k)
            {
                e.strengthBin[(size_t) k] = in.u8();
                in.ok = in.ok && (int) e.strengthBin[(size_t) k] < strengthBins;
            }

            e.subEnergy = in.f64();
            e.midEnergy = in.f64();
            e.trialBlocks = in.i32();
            in.ok = in.ok && e.subEnergy >= 0.0 && e.midEnergy >= 0.0 && e.trialBlocks >= 0;

            for (auto& series : e.trialBin)
                for (int k = 0; in.ok && k < juce::jmin (e.trialBlocks, trialTopCount); ++k)
                {
                    series[(size_t) k] = in.u16();
                    in.ok = in.ok && (int) series[(size_t) k] < trialBins;
                }

            for (size_t c = 0; c < (size_t) numBlendTrials; ++c)
            {
                e.blendIntended[c] = in.f64();
                e.blendOutput[c] = in.f64();
                in.ok = in.ok && e.blendIntended[c] >= 0.0 && e.blendOutput[c] >= 0.0;
            }

            e.subBlockLength = in.i32();
            e.hopLength = in.i32();
            in.ok = in.ok && e.subBlockLength >= 0 && e.hopLength >= 0;

            const auto admittedPart = in.flag();
            in.ok = in.ok && admittedPart == expectAdmitted;

            if (in.ok && admittedPart)
            {
                e.admissionLoudnessFactor = in.f32();
                e.admissionLowFactor = in.f32();
                e.admissionMainness = in.f32();
                e.vote = in.u8();
                const auto flags = in.u8();
                e.timeDecay = (flags & 1) != 0;
                e.newProgram = (flags & 2) != 0;
                e.newProgramRangeValid = (flags & 4) != 0;
                e.newProgramRangeLu = in.f32();
                e.mainStamp = in.f64();
                e.referenceStamp = in.f64();
                e.admitted = true;
                in.ok = in.ok && e.vote <= 3 && flags <= 7;
            }

            // 坏段只有身份与读数（makeEntry）：其余的量必须是空的。
            in.ok = in.ok && (e.usable || (e.effectiveSeconds == 0.0 && e.gatingCount == 0 && e.rangeCount == 0 && e.frames == 0.0
                                           && e.trialBlocks == 0 && e.strengthCount == 0));
            deriveEntry (e);
            return in.ok;
        }

        std::unique_ptr<juce::XmlElement> createState() const
        {
            auto xml = std::make_unique<juce::XmlElement> (liveStateTag);
            xml->setAttribute ("formatVersion", liveStateFormatVersion);
            xml->setAttribute ("sampleRate", hexOf (sampleRate));

            const auto& c = published.counters;
            xml->setAttribute ("mainClock", hexOf (c.mainClock));
            xml->setAttribute ("referenceClock", hexOf (c.referenceClock));
            xml->setAttribute ("secondsSinceMain", hexOf (c.secondsSinceMain));
            xml->setAttribute ("runSeconds", hexOf (c.runSeconds));
            xml->setAttribute ("activeSeconds", hexOf (c.activeSeconds));
            xml->setAttribute ("runStart", juce::String ((juce::uint64) c.runStart));
            xml->setAttribute ("newProgramCount", juce::String ((juce::uint64) c.newProgramCount));
            xml->setAttribute ("commits", juce::String ((juce::uint64) published.count));
            xml->setAttribute ("window", published.size);
            xml->setAttribute ("pending", (int) (arrived - published.count));

            juce::MemoryOutputStream blob;

            for (auto sequence = published.first; sequence < arrived; ++sequence)
                writeEntry (blob, entryAt (sequence), sequence < published.count);

            xml->setAttribute ("segments", juce::Base64::toBase64 (blob.getData(), blob.getDataSize()));
            return xml;
        }

        bool restoreState (const juce::XmlElement& xml)
        {
            coldStart();

            if (xml.getTagName() != liveStateTag || xml.getStringAttribute ("formatVersion") != juce::String (liveStateFormatVersion))
                return false;

            auto ok = true;
            const auto scalar = [&xml, &ok] (const char* name, double& value)
            {
                if (! xml.hasAttribute (name) || ! parseHexDouble (xml.getStringAttribute (name), value))
                    ok = false;
            };

            const auto count = [&xml, &ok] (const char* name, std::uint64_t maximum, std::uint64_t& value)
            {
                if (! xml.hasAttribute (name) || ! parseUnsigned (xml.getStringAttribute (name), maximum, value))
                    ok = false;
            };

            double savedRate = 0.0;
            Counters c;
            std::uint64_t commits = 0, window = 0, pending = 0;
            scalar ("sampleRate", savedRate);
            scalar ("mainClock", c.mainClock);
            scalar ("referenceClock", c.referenceClock);
            scalar ("secondsSinceMain", c.secondsSinceMain);
            scalar ("runSeconds", c.runSeconds);
            scalar ("activeSeconds", c.activeSeconds);
            count ("runStart", std::numeric_limits<std::uint64_t>::max(), c.runStart);
            count ("newProgramCount", std::numeric_limits<std::uint64_t>::max(), c.newProgramCount);
            count ("commits", std::numeric_limits<std::uint64_t>::max() / 2, commits);
            count ("window", (std::uint64_t) windowCapacity, window);
            count ("pending", (std::uint64_t) queueCapacity, pending);

            ok = ok && savedRate > 0.0 && c.mainClock >= 0.0 && c.referenceClock >= 0.0 && c.secondsSinceMain >= 0.0 && c.runSeconds >= 0.0
              && c.activeSeconds >= 0.0 && window <= commits && window == juce::jmin (commits, (std::uint64_t) windowCapacity)
              && c.runStart <= commits && xml.hasAttribute ("segments");

            if (! ok)
                return false;

            juce::MemoryOutputStream decoded;

            if (! juce::Base64::convertFromBase64 (decoded, xml.getStringAttribute ("segments")))
                return false;

            // 读进来的段先放进环（顺序同保存时：窗口在前、还没发布的在后），读完全部再换算采样率。
            BlobReader in (decoded.getData(), decoded.getDataSize());
            const auto first = commits - window;
            const auto ratio = sampleRate / savedRate;

            for (std::uint64_t k = 0; k < window + pending && ok; ++k)
            {
                auto& e = entryAt (first + k);
                ok = readEntry (in, e, k < window);

                if (ok && ratio != 1.0)
                {
                    // 采样率变了：按样本计的量换成新采样率下的同一段时长（其余都按秒或按个数存）。
                    e.start = (std::int64_t) std::llround ((double) e.start * ratio);
                    e.end = (std::int64_t) std::llround ((double) e.end * ratio);
                    e.nominalLength = (std::int64_t) std::llround ((double) e.nominalLength * ratio);
                    e.onsetSamples *= ratio;
                    e.subBlockLength = e.subBlockLength > 0 ? juce::jmax (1, juce::roundToInt ((double) e.subBlockLength * ratio)) : 0;
                    e.hopLength = e.hopLength > 0 ? juce::jmax (1, juce::roundToInt ((double) e.hopLength * ratio)) : 0;
                }
            }

            if (! ok || ! in.atEnd())
            {
                coldStart();
                return false;
            }

            // 已发布那一份：前 commits 段、窗口 [first, commits)；按此刻的常数重算（与保存时逐位相同：同一批段、印记、时钟）。
            arrived = commits + pending;
            admittedCount = commits;
            counters = c;
            published.count = commits;
            published.first = first;
            published.size = (int) window;
            published.counters = c;
            work = published;
            recomputePublished();

            // 保存时还没发布的段：按序进账、重算、立刻发布（时间轴重新起算；不另发提交记录）。
            for (std::uint64_t k = 0; k < pending; ++k)
            {
                startNextPass();
                finishPass();
                publishQuietly();
            }

            return true;
        }

        // 发布但不写提交记录、不通知（读档时把保存时还没发布的段并进来）。
        void publishQuietly() noexcept
        {
            const auto total = recordTotal;
            const auto saved = records[(size_t) (total % (std::uint64_t) commitCapacity)];
            auto* const savedListener = listener;
            listener = nullptr;
            publish();
            listener = savedListener;
            recordTotal = total;
            records[(size_t) (total % (std::uint64_t) commitCapacity)] = saved;
        }
    };

    //==============================================================================
    LiveProfile::LiveProfile() : impl (std::make_unique<Impl>()) {}
    LiveProfile::~LiveProfile() = default;

    void LiveProfile::prepare (double sampleRate)
    {
        impl->configure (sampleRate);
        impl->coldStart();
    }

    void LiveProfile::reset() noexcept
    {
        impl->coldStart();
    }

    void LiveProfile::setConstants (const LiveConstants& constants, MainSectionModel model) noexcept
    {
        impl->constants = sanitiseLiveConstants (constants);
        impl->model = model == MainSectionModel::fourOnFloor || model == MainSectionModel::passive ? model : MainSectionModel::loudness;
        impl->refreshDelay();
        impl->recomputePublished();
    }

    void LiveProfile::segmentFinalized (const SegmentLedger::Segment& segment) noexcept
    {
        impl->enqueue (segment, false, 0.0f);
    }

    void LiveProfile::advanceTo (std::int64_t analyzerTime) noexcept
    {
        impl->advanceTo (analyzerTime);
    }

    void LiveProfile::flush() noexcept
    {
        impl->flush();
    }

    void LiveProfile::commitWithMainness (const SegmentLedger::Segment& segment, float mainness) noexcept
    {
        impl->enqueue (segment, true, std::isfinite (mainness) ? juce::jlimit (0.0f, 1.0f, mainness) : 0.0f);
    }

    float LiveProfile::mainnessFor (bool loudnessValid, float loudnessLufs, bool lowShareValid, float lowShare) const noexcept
    {
        const auto f = impl->factorsFor (loudnessValid, loudnessLufs, lowShareValid, lowShare);
        return f.loudness * f.low;
    }

    std::int64_t LiveProfile::getNextPublicationTime() const noexcept   { return impl->nextPublicationTime(); }

    void LiveProfile::setPublicationListener (PublicationListener* listener) noexcept   { impl->listener = listener; }

    const MusicProfile& LiveProfile::getProfile() const noexcept   { return impl->publishedResult.profile; }
    LiveDiagnostics LiveProfile::getDiagnostics() const noexcept   { return impl->diagnostics(); }

    int LiveProfile::readCommits (std::uint64_t& cursor, LiveCommitRecord* out, int maxCount) const noexcept
    {
        return impl->readCommits (cursor, out, maxCount);
    }

    int LiveProfile::readJudgements (LiveSegmentJudgement* out, int maxCount) const noexcept
    {
        return impl->readJudgements (out, maxCount);
    }

    std::unique_ptr<juce::XmlElement> LiveProfile::createStateXml() const
    {
        return impl->createState();
    }

    bool LiveProfile::restoreStateXml (const juce::XmlElement& xml)
    {
        return impl->restoreState (xml);
    }

    void LiveProfile::setFrozenReferencesForTesting (bool enabled, float referenceLufs, float lowReference) noexcept
    {
        impl->frozen = enabled;
        impl->frozenReference = referenceLufs;
        impl->frozenLowReference = lowReference;
        impl->recomputePublished();
    }
}

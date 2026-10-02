/*
  ==============================================================================

    LiveProfile.cpp
    T-048 实时画像（接口与约定见 LiveProfile.h；设计规格-实时GLAZE-主段与增益 第 1～3、5 节；T-048 阶段 1 BRIEF 第 3 节）。

    一、累计与遗忘（「全局缩放因子 + 周期性重规范」）
      三个遗忘域各有一个全局缩放因子 g：实值 = 存储值 × g。
        - 主段域（按进账遗忘）：并入一段先 g × exp(−(w ÷ W_mem + 无主段的时间衰减))，再把这一段 × m 加进去（存储值加 m ÷ g 倍）。
          装着 4/4 型 I 的门块、真峰值、频谱 / 立体声 / 低频起伏 / 通量、起音、次声、试算与混合损失、主段质量 M。
        - 参考域（全部段，不看 m）：每段 g × exp(−有效时长 ÷ referenceMemorySeconds)：S_ref、lowRef、LRA 的直方图。
        - 全部段域（全部段，不看 m）：每段 g × exp(−有效时长 ÷ W_mem)：响度型 / 被动型的 I（门块）与它的有效时长。
      遗忘只改 g（一次乘法），存储值不逐格乘，不会一路衰减进次正规数（x86 没开 FTZ 时慢几十倍）。g 低于 renormaliseBelow（1e-30）时
      把这一域的存储值一次乘回实值、g 回到 1（重规范），实值绝对值低于 flushBelow（1e-250）的归零——之后再乘 g ≥ 1e-30 也落不进
      次正规数。是否重规范只在并入一段时按 g 判定，所以重规范的位置只取决于交来的段序列（与宿主分块无关）。
      W_mem 与参考遗忘都取无穷（等价性测试）时 g 恒为 1：存储值就是 Σ m × 段的量；m = 1 时与 House 画像按段求和逐位相同
      （同一顺序，× 1 与 ÷ 1 都精确）。

    二、主段度 m 与参考（规格第 2 节；数值在 LiveConstants，清洗见 sanitise）
      ramp_w(x) = clamp(0.5 + x ÷ w, 0, 1)。4/4 型：m = ramp(L_seg − (S_ref − ΔG)) × ramp(lowShare ÷ lowRef − g_low)（g_low ≤ 0 时
      没有低频因子）；响度型：只有响度因子；被动型：有效音频 m = 1。段响度读不出（没有有效音频）的段 m = 0；4/4 型低频占比读不出的段
      低频因子 = 0（同 SegmentLedger 门控：读不出的那道门过不了）。
      S_ref = 参考域段响度直方图（0.1 LU 一格，按有效时长加权）的 referencePercentile 分位格中心；lowRef = 响度因子 ≥ 0.5 的段的
      lowShare 直方图（0.001 一格，同样加权）的同分位格中心；分位的取法同 SegmentLedger.cpp weightedPercentileBin。
      m 用「这一段并入之前」的参考算（因果；LiveCommitRecord 记的就是它）。
      冷启动（还没有 S_ref / lowRef）：以这一段自己为参考——S_ref = L_seg、lowRef = lowShare，即响度因子 = ramp(ΔG)、低频因子 =
      ramp(1 − g_low)（出厂常数下都是 1：第一段当主段）。理由：此刻它是唯一的证据；规格第 5 节已接受「冷启动时 DJ 前奏会被当成主段」，
      置信度 c 从 0 起爬、决策器按 c 收紧，之后真主段一来 S_ref 上移、前奏掉出门外，前奏的累计按进账遗忘淡出。不取 m = 0：那样
      第一段白丢、c 晚一段起爬，对只有一种段落的素材（整段都是 drop 的片段）也没有好处；用公式本身（以自己为参考）而不另设一个数，
      常数改了也自洽（例如 ΔG = 0 时第一段 m = 0.5）。两道参考分开冷启动：S_ref 有了、lowRef 还没有时只有低频因子按自己算。

    三、换歌软遗忘与长时间无主段（规格第 3 节）
      - 换歌：段响度与「这一段并入之前」画像的 I 相差超过 newProgramLu 的段，有效音频累计满 newProgramSeconds → 三个域的 g 一起
        × newProgramKeep（在这一段并入之前做：新素材按原权重进来），计数清零重来。夹一段不超的清零重计；段响度或 I 读不出的段不算数
        也不清零（没有证据）。不看宿主走带。
      - 无主段：上一次 m ≥ mainThreshold 之后的有效音频（secondsSinceMain）超过 noMainSeconds 的部分，按 exp(−超出 ÷ noMainDecaySeconds)
        乘在主段域上，与这一段按进账的遗忘相乘（两个乘性因子叠加：无主段时额外遗忘，按进账照旧）。只算越过门槛之后的那部分有效音频，
        所以与段怎么切无关。

    四、画像（逐项同 MusicAnalyzer.cpp「六」节 houseLoudnessProfile / houseSpectrumProfile / houseStereoGroup / houseOnsetProfile /
        houseTrialProfile / houseSubProfile / houseConfidences；House 的整数计数在这里是加权的实数，推广写在各函数旁边，
        权重全为 1 时退回 House 的整数公式、逐位相同）。与 House 画像按设计的差别：
        - effectiveSeconds = 主段质量 M（House 是全部段的有效时长）；representativeness = c = 1 − exp(−M ÷ M0)；各特征 confidence =
          min(c, 观测充分度) × 起音一致性（House 是 min(代表性, 观测充分度)）；
        - LRA 用全部段（参考域，按有效音频遗忘；House 阶段 3 起只用满编段）；响度型 / 被动型的 I 用全部段（全部段域）；
        - I 的相对门按 0.1 LU 格中心判（同通用画像；House 逐个门块比——相差只在相对门那一格里的门块，≤ 0.05 LU）；
        - 真峰值：主段加权的「段最大值」直方图（0.1 dB 一格，每格另记落进来的最大线性值），取「其上至少有 peakTailSeconds 主段质量」的
          最高一格里的最大值；遗忘关掉、每个 m > 0 的段 w ≥ peakTailSeconds 时就是 House 的满编段最大值（逐位）；
        - 试算 P99：每一路一张加权直方图（0.01 dB 一格），每段交来的「每路最大的 8 个块峰值」按 m 计入、其余块只计入总块数，按格中心
          做 numpy 线性插值的加权推广（trialPercentileOf）；每个 P99 误差 ≤ 半格（0.005 dB）。块数不设 512 的上限（靠遗忘）；
        - 混合损失的 confidence 照通用画像 finalizeConfidences 给（houseConfidences 没给、恒为 0；决策器只看 valid，不受影响）；
        - formatVersion = clayProfileFormatVersion、houseGate 不填（理由见 computeProfile）。

    五、状态 XML（格式版本 1，节点 LIVE_PROFILE）
      三个域的全部存储值与缩放因子、不遗忘的计数与时钟、由段还原的换算（子块长、hop）与采样率；派生量（S_ref / lowRef、画像）读回后
      重算，所以读回后 getProfile / getDiagnostics 逐位等于保存时（常数与主段模型相同的前提下：常数不是状态）。double / float 写成
      IEEE 位模式的十六进制（16 / 8 位），数组里连续的 +0.0 写成「z个数」。读的时候严格：缺属性、位数不对、非法字符、长度不对、
      非有限值、该非负的是负数、缩放因子不在 (0, 1]、格式版本不认识 → 冷启动并返回 false。

    实时约束：全部容量在构造（Impl）时一次分配（试算直方图约 0.58 MB，其余约 0.1 MB），prepare / reset / 并入 / 读数都不分配、不加锁。
    每次并入 O(直方图格数)：几千次加乘（107 实测中位 5 µs，见 REPORT-A）；冷启动与重规范（数小时的主段素材才一次）各扫一遍三个域，
    试算直方图只扫每一路有数的那一段（整张扫一遍约 50 µs）。

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
                       "段落账的响度直方图与分析器 I / LRA 的直方图同格（S_ref、LRA、I 三张表都用它）");
        static_assert (strengthBins <= 256, "段里的起音强度格号按 8 位存");
        static_assert (std::tuple_size<decltype (MusicProfile::monoPeakCostDb)>::value == (size_t) numMonoTrials
                           && std::tuple_size<decltype (MusicProfile::lowCutBlendLossDb)>::value == (size_t) numBlendTrials,
                       "每个试算候选都要有一个画像特征");
        static_assert (clayProfileFormatVersion == 2, "实时画像按格式 2 交（含立体声 / 超低频字段，不含 houseGate）");

        //==============================================================================
        // 实时画像自己的直方图（「分位数一律用定长直方图」；格宽的理由写在各自旁边）。
        constexpr int lowShareBins = SegmentLedger::lowShareBins;   // lowRef：0 … 1、0.001 一格（同 SegmentLedger）

        // 真峰值：段最大值（线性）按 dB 分格，−100 … +40 dB、0.1 dB 一格（与响度同一格宽：格只决定「哪些段并在一起算质量」）；每格另记
        // 落进来的最大线性值，读数取那一格记下的最大值——遗忘关掉时就是 House 的最大值逐位，遗忘开着时最多高估一格以内（偏保守）。
        // −100 dB 以下并进第 0 格（gainToDecibels 本身在 −100 dB 封底）。
        constexpr double peakMinDb = -100.0;
        constexpr int peakBinsPerDb = 10;
        constexpr int peakBins = 140 * peakBinsPerDb;

        // 试算块峰值：−80 … +24 dBFS、0.01 dB 一格（有效块的峰值不低于约 −70 dBFS：块里每个组都要过「慢包络 ≥ −70 dBFS」的有效门）。
        // P99 按格中心算：每个 P99 误差 ≤ 半格 0.005 dB，代价（两个 P99 之差）≤ 0.01 dB，比决策器代价斜坡的两端（0.2 / 0.6 dB）小一个
        // 量级以上；再细就要几 MB。越界的块峰值钳进两端的格（实际碰不到）。
        constexpr double trialMinDb = -80.0;
        constexpr int trialBinsPerDb = 100;
        constexpr int trialBins = 104 * trialBinsPerDb;
        constexpr int trialSeries = numTrials + 1;   // [0] 原始，[1 + c] 候选 c（同 Segment::liveTrialTop）

        // 重规范门槛与归零门槛（文件头第一节）。
        constexpr double renormaliseBelow = 1.0e-30;
        constexpr double flushBelow = 1.0e-250;

        constexpr const char* liveStateTag = "LIVE_PROFILE";
        constexpr int liveStateFormatVersion = 1;

        //==============================================================================
        // 帮助函数：口径同 MusicAnalyzer.cpp 匿名命名空间里的同名函数（那边不导出，这里各写一个小的）。

        // 同 MusicAnalyzer.cpp energyToLufs。
        double energyToLufs (double meanSquare) noexcept
        {
            return meanSquare > 0.0 ? -0.691 + 10.0 * std::log10 (meanSquare)
                                    : -std::numeric_limits<double>::infinity();
        }

        // 同 MusicAnalyzer.cpp histogramBinOf（越界钳进两端）；另把 ±∞ / NaN 钳进两端（那边的调用方保证有限，这里的门块功率可能是 0）。
        int histogramBinOf (double lufs) noexcept
        {
            if (! std::isfinite (lufs))
                return lufs > 0.0 ? histogramBins - 1 : 0;

            return juce::jlimit (0, histogramBins - 1, (int) std::floor ((lufs - histogramMinLufs) * (double) histogramBinsPerLu));
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

        // 同 MusicAnalyzer.cpp binCentreLufs。
        double binCentreLufs (int bin) noexcept
        {
            return histogramMinLufs + ((double) bin + 0.5) / (double) histogramBinsPerLu;
        }

        // 同 MusicAnalyzer.cpp fraction。
        float fraction (double numerator, double fullScale) noexcept
        {
            return fullScale > 0.0 ? (float) juce::jlimit (0.0, 1.0, numerator / fullScale) : 0.0f;
        }

        // 同 MusicAnalyzer.cpp invalidate / setFeature。
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

        // 同 MusicAnalyzer.cpp sideToMidDbOf / correlationOf。
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
        // 权重是按同一缩放存的（比例与 g 无关），直接用存储值。
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

        // 真峰值的格（−100 … +40 dB、0.1 dB 一格）。
        int peakBinOf (float peak) noexcept
        {
            const auto db = juce::Decibels::gainToDecibels ((double) peak);   // ≥ −100
            return juce::jlimit (0, peakBins - 1, (int) std::floor ((db - peakMinDb) * (double) peakBinsPerDb));
        }

        // 试算块峰值的格（−80 … +24 dBFS、0.01 dB 一格）与格中心的线性值。
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

        // 重规范时的一个存储值：乘回实值，太小的归零（文件头第一节）。
        double rescaled (double stored, double scale) noexcept
        {
            const auto value = stored * scale;
            return std::abs (value) < flushBelow ? 0.0 : value;
        }

        //==============================================================================
        // 常数清洗：坏值（NaN、该正的不正）按出厂值（LiveConstants 的默认值就是出厂值），合法范围内原样用；+∞ 是合法的「不遗忘 /
        // 永不触发」（等价性测试就这么关掉遗忘）。
        LiveModelConstants sanitise (const LiveModelConstants& c, const LiveModelConstants& factory) noexcept
        {
            auto s = c;

            if (! std::isfinite (s.gateWidthLu))
                s.gateWidthLu = factory.gateWidthLu;

            if (! (s.loudnessRampLu > 0.0f) || ! std::isfinite (s.loudnessRampLu))
                s.loudnessRampLu = factory.loudnessRampLu;

            if (! std::isfinite (s.lowGateFactor))
                s.lowGateFactor = factory.lowGateFactor;

            if (! (s.lowRampWidth > 0.0f) || ! std::isfinite (s.lowRampWidth))
                s.lowRampWidth = factory.lowRampWidth;

            if (! (s.referencePercentile > 0.0f && s.referencePercentile <= 1.0f))
                s.referencePercentile = factory.referencePercentile;

            if (std::isnan (s.memoryBars))
                s.memoryBars = factory.memoryBars;

            if (! (s.memorySeconds > 0.0))
                s.memorySeconds = factory.memorySeconds;

            if (std::isnan (s.confidenceBars))
                s.confidenceBars = factory.confidenceBars;

            if (! (s.confidenceSeconds > 0.0))
                s.confidenceSeconds = factory.confidenceSeconds;

            return s;
        }

        LiveConstants sanitise (const LiveConstants& c) noexcept
        {
            const LiveConstants factory;
            auto s = c;
            s.fourOnFloor = sanitise (c.fourOnFloor, factory.fourOnFloor);
            s.loudness = sanitise (c.loudness, factory.loudness);
            s.passive = sanitise (c.passive, factory.passive);

            if (! (s.referenceMemorySeconds > 0.0))
                s.referenceMemorySeconds = factory.referenceMemorySeconds;

            if (! (s.mainThreshold >= 0.0f && s.mainThreshold <= 1.0f))
                s.mainThreshold = factory.mainThreshold;

            if (! (s.noMainSeconds >= 0.0))
                s.noMainSeconds = factory.noMainSeconds;

            if (! (s.noMainDecaySeconds > 0.0))
                s.noMainDecaySeconds = factory.noMainDecaySeconds;

            if (! (s.newProgramLu >= 0.0f))
                s.newProgramLu = factory.newProgramLu;

            if (! (s.newProgramSeconds > 0.0))
                s.newProgramSeconds = factory.newProgramSeconds;

            if (! (s.newProgramKeep >= 0.0f && s.newProgramKeep <= 1.0f))
                s.newProgramKeep = factory.newProgramKeep;

            if (std::isnan (s.peakTailSeconds))
                s.peakTailSeconds = factory.peakTailSeconds;

            return s;
        }

        //==============================================================================
        // 段的数据能不能用：计数在定长数组的范围里、和都是有限值、该非负的非负。分析器交来的段本来就满足；不满足的段（坏数据）只记一条
        // 提交记录（m = 0、有效时长 0），不碰任何累计——一个 NaN 进了累计，之后的画像就全坏了。
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
                || s.frames < 0 || s.fluxFrames < 0 || s.fluxEvents < 0 || s.onsetEffectiveSamples < 0 || s.onsetEvents < 0)
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
        // 状态 XML 的编码（文件头第五节）。
        constexpr const char* hexDigits = "0123456789abcdef";

        template <typename Bits>
        void appendHex (std::string& out, Bits bits)
        {
            constexpr int digits = (int) sizeof (Bits) * 2;
            char buffer[digits];

            for (int i = digits - 1; i >= 0; --i)
            {
                buffer[i] = hexDigits[(size_t) (bits & 15u)];
                bits = (Bits) (bits >> 4);
            }

            out.append (buffer, (size_t) digits);
        }

        int hexValue (char c) noexcept
        {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        }

        // 恰好 sizeof (Bits) × 2 位十六进制、后面是空格或结尾。
        template <typename Bits>
        bool parseHex (const char*& p, Bits& bits) noexcept
        {
            constexpr int digits = (int) sizeof (Bits) * 2;
            Bits value = 0;

            for (int i = 0; i < digits; ++i)
            {
                const auto digit = hexValue (p[i]);

                if (digit < 0)
                    return false;

                value = (Bits) ((value << 4) | (Bits) digit);
            }

            if (p[digits] != 0 && p[digits] != ' ')
                return false;

            p += digits;
            bits = value;
            return true;
        }

        template <typename Real>
        using BitsOf = typename std::conditional<sizeof (Real) == 8, std::uint64_t, std::uint32_t>::type;

        template <typename Real>
        juce::String encodeReals (const Real* values, size_t count)
        {
            std::string out;
            size_t zeros = 0;

            const auto flushZeros = [&]
            {
                if (zeros == 0)
                    return;

                if (! out.empty())
                    out += ' ';

                out += 'z';
                out += std::to_string (zeros);
                zeros = 0;
            };

            for (size_t i = 0; i < count; ++i)
            {
                BitsOf<Real> bits;
                std::memcpy (&bits, &values[i], sizeof (bits));

                if (bits == 0)   // 只有 +0.0（−0.0 的位模式不是 0，照常写出）
                {
                    ++zeros;
                    continue;
                }

                flushZeros();

                if (! out.empty())
                    out += ' ';

                appendHex (out, bits);
            }

            flushZeros();
            return juce::String (out);
        }

        // 长度必须正好 count；非法字符、非有限值 → false。
        template <typename Real>
        bool decodeReals (const juce::String& text, Real* values, size_t count) noexcept
        {
            const char* p = text.toRawUTF8();
            size_t filled = 0;

            while (*p != 0)
            {
                if (*p == ' ')
                {
                    ++p;
                    continue;
                }

                if (*p == 'z')
                {
                    ++p;
                    size_t run = 0;
                    auto digits = 0;

                    while (*p >= '0' && *p <= '9')
                    {
                        run = run * 10 + (size_t) (*p - '0');

                        if (++digits > 9 || run > count)
                            return false;

                        ++p;
                    }

                    if (digits == 0 || run == 0 || run > count - filled || (*p != 0 && *p != ' '))
                        return false;

                    std::fill (values + filled, values + filled + run, Real (0));
                    filled += run;
                    continue;
                }

                BitsOf<Real> bits = 0;

                if (filled >= count || ! parseHex (p, bits))
                    return false;

                Real value;
                std::memcpy (&value, &bits, sizeof (value));

                if (! std::isfinite (value))
                    return false;

                values[filled++] = value;
            }

            return filled == count;
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

        // 写：浮点写位模式，计数写十进制。
        class StateWriter
        {
        public:
            explicit StateWriter (juce::XmlElement& target) noexcept : element (target) {}

            void value (const char* name, const double& x)          { scalar (name, x); }
            void scalar (const char* name, double x)                { element.setAttribute (name, encodeReals (&x, 1)); }
            void count (const char* name, std::uint64_t x)          { element.setAttribute (name, juce::String ((juce::uint64) x)); }

            template <typename Array>
            void array (const char* name, const Array& values)      { element.setAttribute (name, encodeReals (values.data(), values.size())); }

            template <typename Array>
            void signedArray (const char* name, const Array& values) { array (name, values); }

            template <typename Vector, typename Ranges>
            void trial (const char* name, const Vector& values, const Ranges&, const Ranges&)   { array (name, values); }

        private:
            juce::XmlElement& element;
        };

        // 读：缺属性、格式不对、非有限值、该非负的是负数都记失败（ok = false）。
        class StateReader
        {
        public:
            explicit StateReader (const juce::XmlElement& source) noexcept : element (source) {}

            void value (const char* name, double& x)
            {
                scalar (name, x);

                if (ok && x < 0.0)
                    ok = false;
            }

            void scalar (const char* name, double& x)
            {
                double parsed = 0.0;

                if (! element.hasAttribute (name) || ! decodeReals (element.getStringAttribute (name), &parsed, 1))
                    ok = false;
                else
                    x = parsed;
            }

            void count (const char* name, std::uint64_t maximum, std::uint64_t& x)
            {
                std::uint64_t parsed = 0;

                if (! element.hasAttribute (name) || ! parseUnsigned (element.getStringAttribute (name), maximum, parsed))
                    ok = false;
                else
                    x = parsed;
            }

            template <typename Array>
            void array (const char* name, Array& values)
            {
                signedArray (name, values);

                if (ok)
                    for (const auto x : values)
                        if (x < 0.0)
                            ok = false;
            }

            template <typename Array>
            void signedArray (const char* name, Array& values)
            {
                if (! element.hasAttribute (name) || ! decodeReals (element.getStringAttribute (name), values.data(), values.size()))
                    ok = false;
            }

            // 读进来的数可以落在任何格：有数的范围由调用方读完后整张重扫（Impl::recomputeTrialRanges）。
            template <typename Vector, typename Ranges>
            void trial (const char* name, Vector& values, const Ranges&, const Ranges&)   { array (name, values); }

            bool ok = true;

        private:
            const juce::XmlElement& element;
        };

        // 遗忘域字段表的另两个访问者：清零、重规范（存档读写用上面两个）。试算直方图（7 路 × 10400 格，0.58 MB，整张扫一遍约 50 µs）
        // 只扫每一路有数的那一段 [low, top]（Impl 跟踪着；空的路 low > top），冷启动与重规范在音频线程上就不会因它冒尖。
        struct Clearer
        {
            void value (const char*, double& x) noexcept   { x = 0.0; }

            template <typename Array>
            void array (const char*, Array& values) noexcept   { std::fill (values.begin(), values.end(), 0.0); }

            template <typename Array>
            void signedArray (const char* name, Array& values) noexcept   { array (name, values); }

            template <typename Vector, typename Ranges>
            void trial (const char*, Vector& values, const Ranges& low, const Ranges& top) noexcept
            {
                for (size_t series = 0; series < low.size(); ++series)
                    if (low[series] <= top[series])
                    {
                        auto* first = values.data() + series * (size_t) trialBins;
                        std::fill (first + low[series], first + top[series] + 1, 0.0);
                    }
            }
        };

        struct Rescaler
        {
            double scale = 1.0;

            void value (const char*, double& x) const noexcept   { x = rescaled (x, scale); }

            template <typename Array>
            void array (const char*, Array& values) const noexcept
            {
                for (auto& x : values)
                    x = rescaled (x, scale);
            }

            template <typename Array>
            void signedArray (const char* name, Array& values) const noexcept   { array (name, values); }

            template <typename Vector, typename Ranges>
            void trial (const char*, Vector& values, const Ranges& low, const Ranges& top) const noexcept
            {
                for (size_t series = 0; series < low.size(); ++series)
                    for (auto bin = low[series]; bin <= top[series]; ++bin)
                    {
                        auto& x = values[series * (size_t) trialBins + (size_t) bin];
                        x = rescaled (x, scale);
                    }
            }
        };
    }

    //==============================================================================
    MainSectionModel mainSectionModelFor (Genre genre) noexcept
    {
        return isHouse (genre) ? MainSectionModel::fourOnFloor : MainSectionModel::loudness;
    }

    //==============================================================================
    struct LiveProfile::Impl
    {
        // I 的门块直方图（−70 … +10 LUFS、0.1 LU 一格，同通用画像的门块直方图）：每格的能量和与块数；另有逐块累加的总和——相对门用它，
        // 求和顺序与 House 画像逐块求和相同（遗忘关掉时相对门逐位相同）。
        struct GatingHistogram
        {
            std::array<double, (size_t) histogramBins> energy {}, count {};
            double totalEnergy = 0.0, totalCount = 0.0;

            void add (double power, double gain) noexcept
            {
                const auto bin = (size_t) histogramBinOf (energyToLufs (power));
                energy[bin] += power * gain;
                count[bin] += gain;
                totalEnergy += power * gain;
                totalCount += gain;
            }
        };

        // 主段域的存储值（× mainForgetting.scale 才是实值）。口径同 MusicAnalyzer.cpp HouseSums 的满编段部分，整数计数换成加权实数。
        struct MainStore
        {
            MainStore() : trialWeights ((size_t) trialSeries * (size_t) trialBins, 0.0) {}

            double mass = 0.0;                                             // 主段质量 M = Σ w（House 的 fullSeconds）
            GatingHistogram gating;                                        // 4/4 型的 I
            std::array<double, (size_t) peakBins> peakWeight {};           // 真峰值：每格 Σ w
            std::array<float, (size_t) peakBins> peakMax {};               // 每格落进来的最大线性值（不缩放；重规范后权重归零的格清掉）
            double frames = 0.0;                                           // 帧门之上的帧
            std::array<double, (size_t) numBands> powerSum {}, tameSum {}, recoverSum {}, deficitWeight {}, validWeight {};
            std::array<double, (size_t) numBands> stereoPowerSum {}, stereoCrossSum {}, stereoWeight {};
            double lowStereoWeight = 0.0, highStereoWeight = 0.0;
            std::array<double, (size_t) lowRangeBins> lowRangeCount {};
            double fluxFrames = 0.0, fluxEvents = 0.0;
            double onsetSamples = 0.0, onsetEvents = 0.0;
            std::array<double, (size_t) strengthBins> strengthCount {};
            double subEnergy = 0.0, midEnergy = 0.0;
            double trialBlocks = 0.0;                                      // 有效块数（全部块，含没交数值的）
            std::vector<double> trialWeights;                              // [路 × trialBins]：每路交来的块峰值（每段最大的 8 个）按格的 Σ m
            std::array<double, (size_t) numBlendTrials> blendIntended {}, blendOutput {};
        };

        // 参考域（全部段，按有效音频遗忘）。
        struct ReferenceStore
        {
            std::array<double, (size_t) histogramBins> loudness {};        // 段响度，权重 = 有效时长
            double loudnessTotal = 0.0;
            std::array<double, (size_t) lowShareBins> lowShare {};         // 响度因子 ≥ 0.5 的段的 lowShare，权重 = 有效时长
            double lowShareTotal = 0.0;
            std::array<double, (size_t) histogramBins> range {};           // LRA：3 s 短期响度的格号，每个取样权重 1
            double rangeEnergy = 0.0, rangeCount = 0.0;
        };

        // 全部段域（全部段，按有效音频、τ = W_mem 遗忘）：响度型 / 被动型的 I。
        struct AllStore
        {
            double mass = 0.0;                                             // Σ 有效时长
            GatingHistogram gating;
        };

        // House 画像各特征的观测充分度（同 MusicAnalyzer.cpp HouseMeasured）。
        struct Measured
        {
            float integrated = 0.0f, range = 0.0f, peak = 0.0f, spectrum = 0.0f, onset = 0.0f;
            float sub = 0.0f, lowStereo = 0.0f, highStereo = 0.0f, trial = 0.0f;
            std::array<float, (size_t) numBands> band {}, stereoBand {};
        };

        // 主段域此刻的实值（存储值 × g）：画像各函数读它，公式就与 House 画像读 HouseSums 一样。
        struct Sums
        {
            double mass = 0.0, frames = 0.0;
            std::array<double, (size_t) numBands> powerSum {}, tameSum {}, recoverSum {}, deficitWeight {}, validWeight {};
            std::array<double, (size_t) numBands> stereoPowerSum {}, stereoCrossSum {}, stereoWeight {};
            double lowStereoWeight = 0.0, highStereoWeight = 0.0;
            double fluxFrames = 0.0, fluxEvents = 0.0;
            double onsetSamples = 0.0, onsetEvents = 0.0;
            double subEnergy = 0.0, midEnergy = 0.0;
            double trialBlocks = 0.0;
            std::array<double, (size_t) numBlendTrials> blendIntended {}, blendOutput {};
        };

        struct Factors
        {
            float loudness = 0.0f, low = 0.0f;
        };

        Impl()
        {
            trialTopBin.fill (-1);           // 构造时整张都是 0：范围为空
            trialLowBin.fill (trialBins);
            configure (48000.0);
            clearAll();
        }

        //==============================================================================
        // 配置
        double sampleRate = 48000.0;
        int trialBlockLength = 4800;            // 同分析器：一块 = 整数个起音组（48 kHz 4800 样本）
        int defaultSubBlockLength = 4800;       // 同分析器 / IN 表：round(0.1 s × 采样率)
        LiveConstants constants;                // 已清洗
        MainSectionModel model = MainSectionModel::loudness;

        // 由段还原的换算（文件头第四节「频谱」）：分析器的子块长与 Balance 的 hop。
        int subBlockLength = 4800;
        int hopLength = 0;                      // 0 = 还没见过有帧的段（同 House 画像在 Balance 没 prepare 时 hopSeconds = 0）

        // 三个遗忘域
        double mainScale = 1.0, referenceScale = 1.0, allScale = 1.0;
        MainStore mainStore;
        ReferenceStore referenceStore;
        AllStore allStore;
        int peakTopBin = -1;                                       // 真峰值有权重的最高一格（派生，−1 = 没有）
        // 试算直方图每一路有数的格的范围 [trialLowBin, trialTopBin]（空 = trialBins / −1）：并入时扩、重规范后缩、读档后整张重扫。
        // 清零与重规范只扫这一段，所以它必须罩住全部非零的格（读档失败的半截数据也一样：先重扫再清，见 restoreState）。
        std::array<int, (size_t) trialSeries> trialTopBin {}, trialLowBin {};

        // 不遗忘的计数与时钟
        std::uint64_t commits = 0;              // 冷启动以来并入的段
        double activeSeconds = 0.0;             // 并入过的有效音频（不遗忘）
        double secondsSinceMain = 0.0;          // 最近一次 m ≥ mainThreshold 之后的有效音频
        double newProgramSeconds = 0.0;         // 换歌：连续「相差超过 newProgramLu」的段的有效音频
        std::uint64_t newProgramCount = 0;
        SegmentMode lastMode = SegmentMode::none;   // 最近一段的分段方式与标称长度：M0 / W_mem 按小节换算用
        double lastNominalSeconds = 0.0;

        // 派生（每次并入后、读档后重算）
        bool referenceValid = false, lowReferenceValid = false;
        float referenceLufs = 0.0f, lowReference = 0.0f;
        float confidence = 0.0f;
        MusicProfile profile;

        // 提交记录（不进状态）
        std::array<LiveCommitRecord, (size_t) commitCapacity> records {};
        std::uint64_t recordTotal = 0;

        //==============================================================================
        void configure (double newSampleRate) noexcept
        {
            sampleRate = newSampleRate > 0.0 && std::isfinite (newSampleRate) ? newSampleRate : 48000.0;

            // 同 MusicAnalyzer.cpp configure：起音组长、试算块 = 整数个组、IN 表子块长。
            const auto groupLength = juce::jlimit (1, maximumGroupLength, juce::roundToInt (sampleRate * onsetGroupSeconds));
            const auto groupSeconds = (double) groupLength / sampleRate;
            trialBlockLength = juce::jmax (1, juce::roundToInt (trialBlockSeconds / groupSeconds)) * groupLength;
            defaultSubBlockLength = juce::jmax (1, juce::roundToInt (sampleRate * 0.1));
        }

        // 拷贝常数与主段模型，再按新常数重算派生的参考与画像（不分配，几 µs）：S_ref / lowRef 的分位、I 取哪个域、M0 都随模型变，
        // 不重算的话换模型后的第一段会按旧分位的 S_ref 算 m（头注释「之后的段按新模型算 m」），读数也还是旧模型的。累计一项不动。
        void setConstants (const LiveConstants& newConstants, MainSectionModel newModel) noexcept
        {
            constants = sanitise (newConstants);
            model = newModel == MainSectionModel::fourOnFloor || newModel == MainSectionModel::passive ? newModel : MainSectionModel::loudness;
            refreshReferences();
            computeProfile();
        }

        const LiveModelConstants& modelConstants() const noexcept
        {
            return model == MainSectionModel::fourOnFloor ? constants.fourOnFloor
                 : model == MainSectionModel::passive ? constants.passive
                                                      : constants.loudness;
        }

        // W_mem：按小节分段 = memoryBars × 小节秒数（段的标称长度），否则 memorySeconds（可为 +∞ = 不遗忘）。
        double memorySecondsFor (SegmentMode mode, double nominalSeconds) const noexcept
        {
            const auto& mc = modelConstants();

            if (mode == SegmentMode::bars && mc.memoryBars > 0.0 && nominalSeconds > 0.0)
                return mc.memoryBars * nominalSeconds;

            return mc.memorySeconds;
        }

        // M0：同 W_mem 的换算（confidenceBars / confidenceSeconds），按最近一段的分段方式。
        double confidenceSecondsNow() const noexcept
        {
            const auto& mc = modelConstants();

            if (lastMode == SegmentMode::bars && mc.confidenceBars > 0.0 && lastNominalSeconds > 0.0)
                return mc.confidenceBars * lastNominalSeconds;

            return mc.confidenceSeconds;
        }

        //==============================================================================
        // 字段表：三个遗忘域里按缩放存的全部量（冷启动、重规范、存档都走它；增减字段只改这里）。peakMax 不缩放，单独处理。
        template <typename Self, typename Visitor>
        static void visitMain (Self& self, Visitor& v)
        {
            auto& s = self.mainStore;
            v.value ("mainMass", s.mass);
            v.array ("mainGatingEnergy", s.gating.energy);
            v.array ("mainGatingCount", s.gating.count);
            v.value ("mainGatingTotalEnergy", s.gating.totalEnergy);
            v.value ("mainGatingTotalCount", s.gating.totalCount);
            v.array ("peakWeight", s.peakWeight);
            v.value ("frames", s.frames);
            v.array ("powerSum", s.powerSum);
            v.array ("tameSum", s.tameSum);
            v.array ("recoverSum", s.recoverSum);
            v.array ("deficitWeight", s.deficitWeight);
            v.array ("validWeight", s.validWeight);
            v.array ("stereoPowerSum", s.stereoPowerSum);
            v.signedArray ("stereoCrossSum", s.stereoCrossSum);
            v.array ("stereoWeight", s.stereoWeight);
            v.value ("lowStereoWeight", s.lowStereoWeight);
            v.value ("highStereoWeight", s.highStereoWeight);
            v.array ("lowRangeCount", s.lowRangeCount);
            v.value ("fluxFrames", s.fluxFrames);
            v.value ("fluxEvents", s.fluxEvents);
            v.value ("onsetSamples", s.onsetSamples);
            v.value ("onsetEvents", s.onsetEvents);
            v.array ("strengthCount", s.strengthCount);
            v.value ("subEnergy", s.subEnergy);
            v.value ("midEnergy", s.midEnergy);
            v.value ("trialBlocks", s.trialBlocks);
            v.trial ("trialWeights", s.trialWeights, self.trialLowBin, self.trialTopBin);
            v.array ("blendIntended", s.blendIntended);
            v.array ("blendOutput", s.blendOutput);
        }

        template <typename Self, typename Visitor>
        static void visitReference (Self& self, Visitor& v)
        {
            auto& s = self.referenceStore;
            v.array ("referenceLoudness", s.loudness);
            v.value ("referenceLoudnessTotal", s.loudnessTotal);
            v.array ("referenceLowShare", s.lowShare);
            v.value ("referenceLowShareTotal", s.lowShareTotal);
            v.array ("rangeWeights", s.range);
            v.value ("rangeEnergy", s.rangeEnergy);
            v.value ("rangeCount", s.rangeCount);
        }

        template <typename Self, typename Visitor>
        static void visitAll (Self& self, Visitor& v)
        {
            auto& s = self.allStore;
            v.value ("allMass", s.mass);
            v.array ("allGatingEnergy", s.gating.energy);
            v.array ("allGatingCount", s.gating.count);
            v.value ("allGatingTotalEnergy", s.gating.totalEnergy);
            v.value ("allGatingTotalCount", s.gating.totalCount);
        }

        //==============================================================================
        // 冷启动：清空全部累计与参考（不分配）；提交序号与环形缓冲不动。
        void clearAll() noexcept
        {
            Clearer clearer;
            visitMain (*this, clearer);
            visitReference (*this, clearer);
            visitAll (*this, clearer);
            mainStore.peakMax.fill (0.0f);
            mainScale = referenceScale = allScale = 1.0;

            commits = 0;
            activeSeconds = secondsSinceMain = newProgramSeconds = 0.0;
            newProgramCount = 0;
            lastMode = SegmentMode::none;
            lastNominalSeconds = 0.0;
            subBlockLength = defaultSubBlockLength;
            hopLength = 0;

            peakTopBin = -1;                 // 全清了：范围为空（不用重扫）
            trialTopBin.fill (-1);
            trialLowBin.fill (trialBins);
            refreshReferences();
            computeProfile();
        }

        // 真峰值有权重的最高一格（1400 格，从上往下扫到第一个就停）。
        void refreshPeakTop() noexcept
        {
            peakTopBin = -1;

            for (int bin = peakBins - 1; bin >= 0; --bin)
                if (mainStore.peakWeight[(size_t) bin] != 0.0)
                {
                    peakTopBin = bin;
                    break;
                }
        }

        // 试算直方图有数的范围：整张重扫（读档后，消息线程）。按「非零」算，读进来的负数（随后判坏数据）也罩住。
        void recomputeTrialRanges() noexcept
        {
            for (int series = 0; series < trialSeries; ++series)
            {
                const auto* weights = trialWeightsOf (series);
                auto top = -1, low = trialBins;

                for (int bin = 0; bin < trialBins; ++bin)
                    if (weights[bin] != 0.0)
                    {
                        low = juce::jmin (low, bin);
                        top = bin;
                    }

                trialTopBin[(size_t) series] = top;
                trialLowBin[(size_t) series] = low;
            }
        }

        // 重规范之后两头可能归零了：只在原范围里往里收。
        void shrinkTrialRanges() noexcept
        {
            for (int series = 0; series < trialSeries; ++series)
            {
                const auto* weights = trialWeightsOf (series);
                auto& low = trialLowBin[(size_t) series];
                auto& top = trialTopBin[(size_t) series];

                while (low <= top && weights[low] == 0.0)
                    ++low;

                while (top >= low && weights[top] == 0.0)
                    --top;

                if (low > top)
                {
                    low = trialBins;
                    top = -1;
                }
            }
        }

        double* trialWeightsOf (int series) noexcept               { return mainStore.trialWeights.data() + (size_t) series * (size_t) trialBins; }
        const double* trialWeightsOf (int series) const noexcept   { return mainStore.trialWeights.data() + (size_t) series * (size_t) trialBins; }

        //==============================================================================
        // 遗忘：只乘缩放因子；低于门槛就重规范（文件头第一节）。factor ∈ [0, 1]。
        void decayMain (double factor) noexcept
        {
            if (! (factor < 1.0))
                return;

            mainScale *= juce::jmax (0.0, factor);

            if (mainScale < renormaliseBelow)
                renormaliseMain();
        }

        void decayReference (double factor) noexcept
        {
            if (! (factor < 1.0))
                return;

            referenceScale *= juce::jmax (0.0, factor);

            if (referenceScale < renormaliseBelow)
            {
                Rescaler rescaler { referenceScale };
                visitReference (*this, rescaler);
                referenceScale = 1.0;
            }
        }

        void decayAll (double factor) noexcept
        {
            if (! (factor < 1.0))
                return;

            allScale *= juce::jmax (0.0, factor);

            if (allScale < renormaliseBelow)
            {
                Rescaler rescaler { allScale };
                visitAll (*this, rescaler);
                allScale = 1.0;
            }
        }

        void renormaliseMain() noexcept
        {
            Rescaler rescaler { mainScale };
            visitMain (*this, rescaler);
            mainScale = 1.0;

            for (size_t bin = 0; bin < (size_t) peakBins; ++bin)
                if (! (mainStore.peakWeight[bin] > 0.0))
                    mainStore.peakMax[bin] = 0.0f;

            refreshPeakTop();
            shrinkTrialRanges();
        }

        // 换歌软遗忘：三个域一起 × keep。
        void forgetAll (double keep) noexcept
        {
            decayMain (keep);
            decayReference (keep);
            decayAll (keep);
        }

        //==============================================================================
        // 主段度（文件头第二节）。用此刻的参考；mainnessFor 与并入共用这一个函数。
        Factors factorsFor (bool loudnessValid, float loudnessLufs, bool lowShareValid, float lowShare) const noexcept
        {
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
            const auto above = referenceValid ? (double) loudnessLufs - ((double) referenceLufs - gate)
                                              : gate;   // 冷启动：以这一段自己为参考（S_ref = L_seg）
            f.loudness = (float) ramp (above, (double) mc.loudnessRampLu);

            if (model != MainSectionModel::fourOnFloor || ! (mc.lowGateFactor > 0.0f))
            {
                f.low = 1.0f;
                return f;
            }

            if (! lowShareValid || ! std::isfinite (lowShare))
                return f;   // 低频读不出：过不了低频门（同 SegmentLedger）

            const auto ratio = lowReferenceValid ? (double) lowShare / (double) lowReference
                                                 : 1.0;   // 冷启动：lowRef = 这一段自己的 lowShare
            f.low = (float) ramp (ratio - (double) mc.lowGateFactor, (double) mc.lowRampWidth);
            return f;
        }

        static float mainnessOf (const Factors& f) noexcept
        {
            return f.loudness * f.low;
        }

        //==============================================================================
        // 并入一段（segmentFinalized / commitWithMainness 共用；文件头第一～三节）。
        void commit (const SegmentLedger::Segment& segment, float mainness, const Factors& factors) noexcept
        {
            const auto usable = usableSegment (segment);
            const auto effective = usable ? segment.effectiveSeconds : 0.0;
            const auto m = usable ? mainness : 0.0f;
            const auto w = (double) m * effective;
            const auto nominalSeconds = segment.nominalLength > 0 ? (double) segment.nominalLength / sampleRate : 0.0;
            const auto memory = memorySecondsFor (segment.mode, nominalSeconds);

            LiveCommitRecord record;
            record.sequence = recordTotal;
            record.start = segment.start;
            record.end = segment.end;
            record.nominalSeconds = nominalSeconds;
            record.mode = segment.mode;
            record.effectiveSeconds = effective;
            record.loudnessValid = segment.loudnessValid;
            record.loudnessLufs = segment.loudnessLufs;
            record.lowShareValid = segment.lowShareValid;
            record.lowShare = segment.lowShare;
            record.loudnessFactor = factors.loudness;
            record.lowFactor = factors.low;
            record.mainness = m;
            record.weightSeconds = w;
            record.referenceValid = referenceValid;
            record.referenceLufs = referenceLufs;
            record.lowReferenceValid = lowReferenceValid;
            record.lowReference = lowReference;

            if (usable)
            {
                record.newProgram = noteNewProgram (segment);

                if (record.newProgram)
                    forgetAll ((double) constants.newProgramKeep);
            }

            // 主段域：按进账遗忘 × 无主段的时间衰减，再把这一段 × m 加进去。
            const auto excess = noteMainness (m, effective);
            record.timeDecay = excess > 0.0;
            const auto exponent = w / memory + excess / constants.noMainDecaySeconds;

            if (exponent > 0.0)
                decayMain (std::exp (-exponent));

            if (m > 0.0f)
                addMain (segment, (double) m / mainScale);

            // 参考域与全部段域：全部段、按有效音频遗忘。
            if (usable)
            {
                if (effective > 0.0)
                {
                    decayReference (std::exp (-effective / constants.referenceMemorySeconds));
                    decayAll (std::exp (-effective / memory));
                }

                addReference (segment, factors);
                addAll (segment);
                recoverLengths (segment);
            }

            ++commits;
            activeSeconds += effective;
            lastMode = segment.mode;
            lastNominalSeconds = nominalSeconds;

            refreshReferences();
            computeProfile();

            record.mainLevelValid = profile.integratedLufs.valid;
            record.mainLevelLufs = profile.integratedLufs.value;
            record.mainMassSeconds = mainStore.mass * mainScale;
            record.confidence = confidence;
            records[(size_t) (recordTotal % (std::uint64_t) commitCapacity)] = record;
            ++recordTotal;
        }

        // 换歌（文件头第三节）：返回这一段是否触发软遗忘。
        bool noteNewProgram (const SegmentLedger::Segment& segment) noexcept
        {
            const auto& level = profile.integratedLufs;

            if (! segment.loudnessValid || ! level.valid || ! std::isfinite (level.value))
                return false;   // 没有可比的读数：不算数也不清零

            if (! (std::abs ((double) segment.loudnessLufs - (double) level.value) > (double) constants.newProgramLu))
            {
                newProgramSeconds = 0.0;
                return false;
            }

            newProgramSeconds += segment.effectiveSeconds;

            if (! (newProgramSeconds >= constants.newProgramSeconds))
                return false;

            newProgramSeconds = 0.0;
            ++newProgramCount;
            return true;
        }

        // 长时间无主段（文件头第三节）：返回这一段里越过 noMainSeconds 之后的有效音频（秒）。
        double noteMainness (float m, double effective) noexcept
        {
            if (m >= constants.mainThreshold)
            {
                secondsSinceMain = 0.0;
                return 0.0;
            }

            const auto before = secondsSinceMain;
            secondsSinceMain += effective;
            return juce::jmax (0.0, secondsSinceMain - juce::jmax (before, constants.noMainSeconds));
        }

        // 这一段 × m 并进主段域（gain = m ÷ g）。逐项同 MusicAnalyzer.cpp computeHouseProfile 对一个满编段做的累加（同一顺序）。
        void addMain (const SegmentLedger::Segment& segment, double gain) noexcept
        {
            auto& s = mainStore;
            const auto weight = segment.effectiveSeconds * gain;   // w ÷ g
            s.mass += weight;

            for (int k = 0; k < segment.gatingCount; ++k)
                s.gating.add ((double) segment.gatingPower[(size_t) k], gain);

            if (segment.truePeak > 0.0f && weight > 0.0)
            {
                const auto bin = peakBinOf (segment.truePeak);
                s.peakWeight[(size_t) bin] += weight;
                s.peakMax[(size_t) bin] = juce::jmax (s.peakMax[(size_t) bin], segment.truePeak);
                peakTopBin = juce::jmax (peakTopBin, bin);
            }

            s.frames += (double) segment.frames * gain;

            for (size_t b = 0; b < (size_t) numBands; ++b)
            {
                s.powerSum[b] += segment.powerSum[b] * gain;
                s.tameSum[b] += segment.tameSum[b] * gain;
                s.recoverSum[b] += segment.recoverSum[b] * gain;
                s.deficitWeight[b] += segment.deficitWeight[b] * gain;
                s.validWeight[b] += segment.validWeight[b] * gain;
                s.stereoPowerSum[b] += segment.stereoPowerSum[b] * gain;
                s.stereoCrossSum[b] += segment.stereoCrossSum[b] * gain;
                s.stereoWeight[b] += segment.stereoWeight[b] * gain;
            }

            s.lowStereoWeight += segment.lowStereoWeight * gain;
            s.highStereoWeight += segment.highStereoWeight * gain;

            for (int bin = segment.lowRangeFirst; bin <= segment.lowRangeLast; ++bin)
                s.lowRangeCount[(size_t) bin] += (double) segment.lowRangeCount[(size_t) bin] * gain;

            s.fluxFrames += (double) segment.fluxFrames * gain;
            s.fluxEvents += (double) segment.fluxEvents * gain;
            s.onsetSamples += (double) segment.onsetEffectiveSamples * gain;
            s.onsetEvents += (double) segment.onsetEvents * gain;

            for (int k = 0; k < segment.strengthCount; ++k)
                s.strengthCount[(size_t) segment.strengthBin[(size_t) k]] += gain;

            s.subEnergy += segment.subEnergy * gain;
            s.midEnergy += segment.midEnergy * gain;

            // 试算：每路交来的最大 8 个（不足 8 块就是全部块）按格计入；全部块计入总块数（没交数值的块都在这 8 个之下）。
            if (segment.liveTrialBlocks > 0)
            {
                s.trialBlocks += (double) segment.liveTrialBlocks * gain;
                const auto values = juce::jmin (segment.liveTrialBlocks, SegmentLedger::trialTopCount);

                for (int series = 0; series < trialSeries; ++series)
                {
                    auto* weights = trialWeightsOf (series);

                    for (int k = 0; k < values; ++k)
                    {
                        const auto bin = trialBinOf (segment.liveTrialTop[(size_t) series][(size_t) k]);
                        weights[bin] += gain;
                        trialTopBin[(size_t) series] = juce::jmax (trialTopBin[(size_t) series], bin);
                        trialLowBin[(size_t) series] = juce::jmin (trialLowBin[(size_t) series], bin);
                    }
                }
            }

            for (size_t c = 0; c < (size_t) numBlendTrials; ++c)
            {
                s.blendIntended[c] += segment.liveBlendIntended[c] * gain;
                s.blendOutput[c] += segment.liveBlendOutput[c] * gain;
            }
        }

        // 参考域：段响度（权重 = 有效时长）、过响度门的 lowShare、LRA 的 3 s 短期响度（全部段）。
        void addReference (const SegmentLedger::Segment& segment, const Factors& factors) noexcept
        {
            auto& s = referenceStore;
            const auto gain = 1.0 / referenceScale;
            const auto weight = segment.effectiveSeconds * gain;

            if (segment.loudnessValid && weight > 0.0)
            {
                // 同 SegmentLedger.cpp recomputeGate 的格。
                const auto bin = juce::jlimit (0, histogramBins - 1,
                                               (int) std::floor (((double) segment.loudnessLufs - histogramMinLufs) * (double) histogramBinsPerLu));
                s.loudness[(size_t) bin] += weight;
                s.loudnessTotal += weight;
            }

            if (factors.loudness >= 0.5f && segment.lowShareValid && weight > 0.0)
            {
                const auto bin = juce::jlimit (0, lowShareBins - 1, (int) std::floor ((double) segment.lowShare * (double) lowShareBins));
                s.lowShare[(size_t) bin] += weight;
                s.lowShareTotal += weight;
            }

            for (int k = 0; k < segment.rangeCount; ++k)
                s.range[(size_t) segment.rangeBin[(size_t) k]] += gain;

            s.rangeEnergy += segment.rangeEnergy * gain;
            s.rangeCount += (double) segment.rangeCount * gain;
        }

        // 全部段域：门块（权重 1）与有效时长。
        void addAll (const SegmentLedger::Segment& segment) noexcept
        {
            auto& s = allStore;
            const auto gain = 1.0 / allScale;
            s.mass += segment.effectiveSeconds * gain;

            for (int k = 0; k < segment.gatingCount; ++k)
                s.gating.add ((double) segment.gatingPower[(size_t) k], gain);
        }

        // Segment 不带分析器的子块长与 Balance 的 hop：由段还原（逐个加起来的时长 ÷ 个数 × 采样率，舍入误差远小于半个样本），
        // 让频谱 / LRA 的时长与 House 画像同一个公式（个数 × 长度 ÷ 采样率）。
        void recoverLengths (const SegmentLedger::Segment& segment) noexcept
        {
            if (segment.effectiveSubBlocks > 0 && segment.effectiveSeconds > 0.0)
            {
                const auto length = std::llround (segment.effectiveSeconds * sampleRate / (double) segment.effectiveSubBlocks);

                if (length >= 1 && length <= (long long) std::numeric_limits<int>::max())
                    subBlockLength = (int) length;
            }

            if (segment.frames > 0 && segment.frameSeconds > 0.0)
            {
                const auto length = std::llround (segment.frameSeconds * sampleRate / (double) segment.frames);

                if (length >= 1 && length <= (long long) std::numeric_limits<int>::max())
                    hopLength = (int) length;
            }
        }

        // S_ref 与 lowRef（文件头第二节；同 SegmentLedger.cpp recomputeGate 的格中心）。分位按此刻的主段模型。
        void refreshReferences() noexcept
        {
            const auto share = (double) modelConstants().referencePercentile;
            const auto& s = referenceStore;
            const auto loudnessBin = weightedPercentileBin (s.loudness, s.loudnessTotal, share);
            referenceValid = loudnessBin >= 0;
            referenceLufs = referenceValid ? (float) (histogramMinLufs + ((double) loudnessBin + 0.5) / (double) histogramBinsPerLu) : 0.0f;

            const auto lowBin = weightedPercentileBin (s.lowShare, s.lowShareTotal, share);
            lowReferenceValid = lowBin >= 0;
            lowReference = lowReferenceValid ? (float) (((double) lowBin + 0.5) / (double) lowShareBins) : 0.0f;
        }

        //==============================================================================
        // 画像（文件头第四节）。同 MusicAnalyzer.cpp computeHouseProfile 的分工。
        //
        // formatVersion = clayProfileFormatVersion（2）、houseGate 不填（valid = false）：决策器（GlazePlanner.cpp）两项都不读——只读各特征的
        // value / valid / confidence、representativeness（confidenceScale 取 I、真峰值、代表性三者最没把握的那个）、bandConfidence；编排器
        // （GlazeController.cpp）用 formatVersion ≥ 2 判「方案源带 Clay 特征」（sourceSupportsClay、clayNeedsRelearn）——实时画像带着，所以是 2；
        // 用 formatVersion ≥ 3 且 houseGate.valid 判「House 档锁定过的画像」（锁定、重开、只在主段落校准）——那是 T-038 学习 → 锁定的
        // 机制，实时画像没有锁定，它的段数也不去重，填进去会让那几条逻辑按不可比的数走；S_ref / lowRef 从 getDiagnostics 读。
        void computeProfile() noexcept
        {
            profile = MusicProfile {};
            profile.formatVersion = clayProfileFormatVersion;
            confidence = currentConfidence();

            const auto s = realSums();
            Measured measured;
            liveLoudnessProfile (s, measured);
            const auto fluxOnsetsValid = liveSpectrumProfile (s, measured);
            liveOnsetProfile (s, measured);
            liveTrialProfile (s, measured);
            liveSubProfile (s, measured);
            liveConfidences (s, measured, fluxOnsetsValid);
        }

        // c = 1 − exp(−M ÷ M0)（规格第 3 节）。
        float currentConfidence() const noexcept
        {
            const auto mass = mainStore.mass * mainScale;
            return mass > 0.0 ? (float) (1.0 - std::exp (-mass / confidenceSecondsNow())) : 0.0f;
        }

        Sums realSums() const noexcept
        {
            const auto& m = mainStore;
            const auto g = mainScale;
            Sums s;
            s.mass = m.mass * g;
            s.frames = m.frames * g;

            for (size_t b = 0; b < (size_t) numBands; ++b)
            {
                s.powerSum[b] = m.powerSum[b] * g;
                s.tameSum[b] = m.tameSum[b] * g;
                s.recoverSum[b] = m.recoverSum[b] * g;
                s.deficitWeight[b] = m.deficitWeight[b] * g;
                s.validWeight[b] = m.validWeight[b] * g;
                s.stereoPowerSum[b] = m.stereoPowerSum[b] * g;
                s.stereoCrossSum[b] = m.stereoCrossSum[b] * g;
                s.stereoWeight[b] = m.stereoWeight[b] * g;
            }

            s.lowStereoWeight = m.lowStereoWeight * g;
            s.highStereoWeight = m.highStereoWeight * g;
            s.fluxFrames = m.fluxFrames * g;
            s.fluxEvents = m.fluxEvents * g;
            s.onsetSamples = m.onsetSamples * g;
            s.onsetEvents = m.onsetEvents * g;
            s.subEnergy = m.subEnergy * g;
            s.midEnergy = m.midEnergy * g;
            s.trialBlocks = m.trialBlocks * g;

            for (size_t c = 0; c < (size_t) numBlendTrials; ++c)
            {
                s.blendIntended[c] = m.blendIntended[c] * g;
                s.blendOutput[c] = m.blendOutput[c] * g;
            }

            return s;
        }

        // BS.1770 门控积分（−10 LU 相对门）：相对门按逐块总和（同 House 逐块求和），「在不在门上」按格中心（同通用画像），门上的格
        // 能量与块数求和。比值与缩放因子无关，直接用存储值。没有门块 / 门上没有块 → false。
        static bool gatedIntegral (const GatingHistogram& h, double& lufs) noexcept
        {
            if (! (h.totalCount > 0.0))
                return false;

            const auto relativeGate = energyToLufs (h.totalEnergy / h.totalCount) + integratedRelativeGateLu;
            double gatedEnergy = 0.0, gatedCount = 0.0;

            for (auto bin = (size_t) firstBinAtOrAbove (relativeGate); bin < (size_t) histogramBins; ++bin)
            {
                gatedEnergy += h.energy[bin];
                gatedCount += h.count[bin];
            }

            if (! (gatedCount > 0.0))
                return false;

            lufs = energyToLufs (gatedEnergy / gatedCount);
            return true;
        }

        // 同 houseLoudnessProfile：I（4/4 型主段域；响度型 / 被动型全部段域）、LRA（参考域）、真峰值（主段域）、PLR。
        void liveLoudnessProfile (const Sums& s, Measured& measured) noexcept
        {
            auto& p = profile;
            const auto mainSeconds = s.mass;
            p.effectiveSeconds = mainSeconds;

            const auto fourOnFloor = model == MainSectionModel::fourOnFloor;
            const auto integratedSeconds = fourOnFloor ? mainSeconds : allStore.mass * allScale;
            double integrated = 0.0;

            if (gatedIntegral (fourOnFloor ? mainStore.gating : allStore.gating, integrated))
                setFeature (p.integratedLufs, integrated, integratedSeconds);
            else
                invalidate (p.integratedLufs);

            measured.integrated = fraction (integratedSeconds, loudnessFullSeconds);

            // LRA：同 houseLoudnessProfile 的 LRA 段（全部段；整数个数换成加权的个数：index = ⌊(n − 1) × 分位 + 0.5⌋，累计 > index 的那一格）。
            const auto& r = referenceStore;
            const auto rangeTotal = r.rangeCount * referenceScale;
            const auto rangeSeconds = rangeTotal * (double) subBlockLength / sampleRate;
            invalidate (p.loudnessRangeLu);

            if (rangeTotal > 0.0)
            {
                const auto relativeGate = energyToLufs (r.rangeEnergy / r.rangeCount) + lraRelativeGateLu;
                const auto firstBin = firstBinAtOrAbove (relativeGate);
                double gated = 0.0;

                for (int bin = firstBin; bin < histogramBins; ++bin)
                    gated += r.range[(size_t) bin] * referenceScale;

                if (gated >= minimumLraValues)
                {
                    const auto lowIndex = std::floor ((gated - 1.0) * lraLowFraction + 0.5);
                    const auto highIndex = std::floor ((gated - 1.0) * lraHighFraction + 0.5);
                    double low = 0.0, high = 0.0, cumulative = 0.0;
                    bool haveLow = false;

                    for (int bin = firstBin; bin < histogramBins; ++bin)
                    {
                        cumulative += r.range[(size_t) bin] * referenceScale;

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
                setFeature (p.truePeakDbtp, juce::Decibels::gainToDecibels ((double) mainStore.peakMax[(size_t) peakBin]), mainSeconds);
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
            if (peakTopBin < 0)
                return -1;

            double cumulative = 0.0;

            for (int bin = peakTopBin; bin >= 0; --bin)
            {
                const auto weight = mainStore.peakWeight[(size_t) bin];

                if (! (weight > 0.0))
                    continue;

                cumulative += weight * mainScale;

                if (cumulative >= constants.peakTailSeconds)
                    return bin;
            }

            return peakTopBin;
        }

        // 同 houseSpectrumProfile（主段加权的和）。返回辅助量「通量突起密度」是否有效（起音一致性要用；它本身不进画像）。
        bool liveSpectrumProfile (const Sums& s, Measured& measured) noexcept
        {
            auto& p = profile;
            const auto hopSeconds = hopLength > 0 ? (double) hopLength / sampleRate : 0.0;
            const auto frameSeconds = s.frames * hopSeconds;
            std::array<double, (size_t) numBands> meanPower {}, levelDb {};
            double total = 0.0, low = 0.0, high = 0.0;

            for (int b = 0; b < numBands; ++b)
            {
                const auto i = (size_t) b;
                meanPower[i] = s.frames > 0.0 ? s.powerSum[i] / s.frames : 0.0;
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

            for (size_t b = 0; b < (size_t) numBands; ++b)
            {
                const auto weight = s.validWeight[b];

                if (weight <= 0.0)
                {
                    p.tameEvidenceDb[b] = 0.0f;
                    p.recoverEvidenceDb[b] = 0.0f;
                    measured.band[b] = 0.0f;
                    continue;
                }

                p.tameEvidenceDb[b] = (float) (s.tameSum[b] / weight);

                const auto persistence = s.deficitWeight[b] / weight;
                const auto persistenceGain = juce::jlimit (0.0, 1.0, (persistence - recoverPersistenceStart)
                                                                         / (recoverPersistenceFull - recoverPersistenceStart));
                p.recoverEvidenceDb[b] = inBand[b] ? (float) (s.recoverSum[b] / weight * persistenceGain) : 0.0f;
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
                    cumulative += mainStore.lowRangeCount[(size_t) bin] * mainScale;

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
            for (size_t b = 0; b < (size_t) numBands; ++b)
            {
                const auto summedPower = s.stereoPowerSum[b];

                if (s.stereoWeight[b] > 0.0 && summedPower > 0.0)
                {
                    p.bandSideToMidDb[b] = (float) sideToMidDbOf (summedPower, s.stereoCrossSum[b]);
                    p.bandCorrelation[b] = (float) correlationOf (summedPower, s.stereoCrossSum[b]);
                    measured.stereoBand[b] = fraction (s.stereoWeight[b] * hopSeconds, stereoFullSeconds);
                }
                else
                {
                    p.bandSideToMidDb[b] = 0.0f;
                    p.bandCorrelation[b] = 0.0f;
                    measured.stereoBand[b] = 0.0f;
                }
            }

            liveStereoGroup (s, lowStereoFirstBand, lowStereoLastBand, s.lowStereoWeight * hopSeconds,
                             p.lowSideToMidDb, p.lowCorrelation, measured.lowStereo);
            liveStereoGroup (s, highStereoFirstBand, highStereoLastBand, s.highStereoWeight * hopSeconds,
                             p.highSideToMidDb, p.highCorrelation, measured.highStereo);
            return fluxOnsetsValid;
        }

        // 同 houseStereoGroup。
        static void liveStereoGroup (const Sums& s, int firstBand, int lastBand, double observedSeconds, Feature& sideToMid,
                                     Feature& correlation, float& measured) noexcept
        {
            double summedPower = 0.0, summedCross = 0.0;

            for (int b = firstBand; b <= lastBand; ++b)
            {
                summedPower += s.stereoPowerSum[(size_t) b];
                summedCross += s.stereoCrossSum[(size_t) b];
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
        void liveOnsetProfile (const Sums& s, Measured& measured) noexcept
        {
            auto& p = profile;
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
                    cumulative += mainStore.strengthCount[(size_t) bin] * mainScale;

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

        // 同 houseTrialProfile：块数 = 加权的有效块数，P99 由每路的直方图按 trialPercentileOf 求。
        void liveTrialProfile (const Sums& s, Measured& measured) noexcept
        {
            auto& p = profile;
            const auto n = s.trialBlocks;
            const auto seconds = n * (double) trialBlockLength / sampleRate;
            measured.trial = fraction (seconds, trialFullSeconds);

            const auto original = n > 0.0 ? trialPercentileOf (0, n) : 0.0;
            const auto enough = seconds >= trialMinimumSeconds && original > 0.0;

            for (int slot = 0; slot < numTrials; ++slot)
            {
                auto& cost = slot == trialLowCut ? p.lowCutPeakCostDb
                           : slot == trialWidth ? p.widthPeakCostDb
                                                : p.monoPeakCostDb[(size_t) (slot - trialFirstMono)];

                if (! enough)
                {
                    invalidate (cost);
                    continue;
                }

                const auto candidate = trialPercentileOf (slot + 1, n);
                setFeature (cost, 20.0 * std::log10 (juce::jmax (candidate, trialPeakFloorRatio * original) / original), seconds);
            }

            for (size_t c = 0; c < (size_t) numBlendTrials; ++c)
            {
                auto& loss = p.lowCutBlendLossDb[c];

                if (enough && s.blendIntended[c] > 0.0 && s.blendOutput[c] > 0.0)
                    setFeature (loss, 10.0 * std::log10 (s.blendIntended[c] / s.blendOutput[c]), seconds);
                else
                    invalidate (loss);
            }
        }

        // 第 series 路的加权 P99（线性幅度）：numpy 线性插值（= houseTrialProfile 的 percentile）对权重的推广。
        //   条目 = 有权重的格（值取格中心），按值升序在「质量坐标」上排开：条目 k 的起点 S_k = 它下面的质量（总块数 W 减去从顶往下累计到
        //   它为止的质量；W 里含没交数值的块，它们都在每段交来的 8 个之下）。条目先占 max(0, u_k − 1) 长的平台（值不变），再用 min(u_k, 1)
        //   的长度线性过渡到上面一格的值；目标位置 = q × (W − min(u_顶, 1))。等权（u = 1）时平台长 0、过渡长 1、目标 = q × (n − 1)：
        //   就是 numpy（与 House 同式）；整数权重时与「拆成 u 个等权条目」相同，所以几个块落进同一格不改变结果（只差格中心与真值）。
        //   从顶往下扫，几格就到。目标落进「没交数值的块」里（只在一段占了尾部超过 8 块时）取记下的最低一格。
        double trialPercentileOf (int series, double total) const noexcept
        {
            const auto top = trialTopBin[(size_t) series];

            if (top < 0)
                return 0.0;

            const auto* weights = trialWeightsOf (series);
            const auto topMass = weights[top] * mainScale;
            const auto target = trialPercentile * (total - juce::jmin (topMass, 1.0));
            double above = 0.0;
            int previous = -1;

            for (int bin = top; bin >= trialLowBin[(size_t) series]; --bin)
            {
                const auto mass = weights[bin] * mainScale;

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

        // 同 houseSubProfile。
        void liveSubProfile (const Sums& s, Measured& measured) noexcept
        {
            const auto seconds = s.onsetSamples / sampleRate;

            if (seconds >= minimumSubSeconds && s.midEnergy > 0.0)
                setFeature (profile.subShareDb, 10.0 * std::log10 (juce::jlimit (subShareFloor, 1.0, s.subEnergy / s.midEnergy)), seconds);
            else
                invalidate (profile.subShareDb);

            measured.sub = fraction (seconds, subFullSeconds);
        }

        // 同 houseConfidences，代表性换成 c；混合损失照通用画像 finalizeConfidences 也给置信度。
        void liveConfidences (const Sums& s, const Measured& measured, bool fluxOnsetsValid) noexcept
        {
            auto& p = profile;
            const auto rep = confidence;
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
        LiveDiagnostics diagnostics() const noexcept
        {
            LiveDiagnostics d;
            d.commits = commits;
            d.referenceValid = referenceValid;
            d.referenceLufs = referenceLufs;
            d.lowReferenceValid = lowReferenceValid;
            d.lowReference = lowReference;
            d.mainMassSeconds = mainStore.mass * mainScale;
            d.confidence = confidence;
            d.activeSeconds = activeSeconds;
            d.secondsSinceMain = secondsSinceMain;
            d.timeDecayActive = secondsSinceMain >= constants.noMainSeconds;
            d.newProgramCount = (std::uint32_t) juce::jmin (newProgramCount, (std::uint64_t) std::numeric_limits<std::uint32_t>::max());
            return d;
        }

        // 同 SegmentLedger::readEvents。
        int readCommits (std::uint64_t& cursor, LiveCommitRecord* out, int maxCount) const noexcept
        {
            auto lost = false;

            if (cursor > recordTotal)
                cursor = recordTotal;

            if (recordTotal - cursor > (std::uint64_t) commitCapacity)
            {
                cursor = recordTotal - (std::uint64_t) commitCapacity;
                lost = true;
            }

            auto count = 0;

            while (out != nullptr && cursor < recordTotal && count < maxCount)
            {
                out[count++] = records[(size_t) (cursor % (std::uint64_t) commitCapacity)];
                ++cursor;
            }

            return lost ? -count : count;
        }

        //==============================================================================
        // 状态 XML（文件头第五节）。
        std::unique_ptr<juce::XmlElement> createState() const
        {
            auto xml = std::make_unique<juce::XmlElement> (liveStateTag);
            xml->setAttribute ("formatVersion", liveStateFormatVersion);

            StateWriter writer (*xml);
            writer.scalar ("sampleRate", sampleRate);
            writer.count ("subBlockLength", (std::uint64_t) subBlockLength);
            writer.count ("hopLength", (std::uint64_t) hopLength);
            writer.scalar ("mainScale", mainScale);
            writer.scalar ("referenceScale", referenceScale);
            writer.scalar ("allScale", allScale);
            writer.count ("commits", commits);
            writer.scalar ("activeSeconds", activeSeconds);
            writer.scalar ("secondsSinceMain", secondsSinceMain);
            writer.scalar ("newProgramSeconds", newProgramSeconds);
            writer.count ("newProgramCount", newProgramCount);
            writer.count ("lastMode", (std::uint64_t) lastMode);
            writer.scalar ("lastNominalSeconds", lastNominalSeconds);

            visitMain (*this, writer);
            visitReference (*this, writer);
            visitAll (*this, writer);
            xml->setAttribute ("peakMax", encodeReals (mainStore.peakMax.data(), mainStore.peakMax.size()));
            return xml;
        }

        bool restoreState (const juce::XmlElement& xml)
        {
            clearAll();

            if (xml.getTagName() != liveStateTag || xml.getStringAttribute ("formatVersion") != juce::String (liveStateFormatVersion))
                return false;

            StateReader reader (xml);
            double savedRate = 0.0;
            std::uint64_t subBlocks = 0, hop = 0, mode = 0;
            reader.scalar ("sampleRate", savedRate);
            reader.count ("subBlockLength", (std::uint64_t) std::numeric_limits<int>::max(), subBlocks);
            reader.count ("hopLength", (std::uint64_t) std::numeric_limits<int>::max(), hop);
            reader.scalar ("mainScale", mainScale);
            reader.scalar ("referenceScale", referenceScale);
            reader.scalar ("allScale", allScale);
            reader.count ("commits", std::numeric_limits<std::uint64_t>::max(), commits);
            reader.value ("activeSeconds", activeSeconds);
            reader.value ("secondsSinceMain", secondsSinceMain);
            reader.value ("newProgramSeconds", newProgramSeconds);
            reader.count ("newProgramCount", std::numeric_limits<std::uint64_t>::max(), newProgramCount);
            reader.count ("lastMode", (std::uint64_t) SegmentMode::seconds, mode);
            reader.value ("lastNominalSeconds", lastNominalSeconds);

            visitMain (*this, reader);
            visitReference (*this, reader);
            visitAll (*this, reader);

            if (! xml.hasAttribute ("peakMax") || ! decodeReals (xml.getStringAttribute ("peakMax"), mainStore.peakMax.data(), mainStore.peakMax.size()))
                reader.ok = false;

            const auto scaleOk = [] (double g) { return g > 0.0 && g <= 1.0; };
            auto ok = reader.ok && savedRate > 0.0 && subBlocks >= 1 && scaleOk (mainScale) && scaleOk (referenceScale) && scaleOk (allScale);

            for (const auto peak : mainStore.peakMax)
                ok = ok && peak >= 0.0f;

            // 读进来的试算直方图可以落在任何格（读到一半失败的也是）：先整张重扫有数的范围，清零才罩得住全部非零格。
            recomputeTrialRanges();

            if (! ok)
            {
                clearAll();
                return false;
            }

            subBlockLength = (int) subBlocks;
            hopLength = (int) hop;
            lastMode = static_cast<SegmentMode> (mode);

            // 采样率变了：按样本计的量换成新采样率下的同一段时长（其余都按秒或按个数存）。
            if (savedRate != sampleRate)
            {
                const auto ratio = sampleRate / savedRate;
                mainStore.onsetSamples *= ratio;
                subBlockLength = juce::jmax (1, juce::roundToInt ((double) subBlockLength * ratio));
                hopLength = hopLength > 0 ? juce::jmax (1, juce::roundToInt ((double) hopLength * ratio)) : 0;
            }

            refreshPeakTop();
            refreshReferences();
            computeProfile();
            return true;
        }
    };

    //==============================================================================
    LiveProfile::LiveProfile() : impl (std::make_unique<Impl>()) {}
    LiveProfile::~LiveProfile() = default;

    void LiveProfile::prepare (double sampleRate)
    {
        impl->configure (sampleRate);
        impl->clearAll();
    }

    void LiveProfile::reset() noexcept
    {
        impl->clearAll();
    }

    void LiveProfile::setConstants (const LiveConstants& constants, MainSectionModel model) noexcept
    {
        impl->setConstants (constants, model);
    }

    void LiveProfile::segmentFinalized (const SegmentLedger::Segment& segment) noexcept
    {
        const auto factors = impl->factorsFor (segment.loudnessValid, segment.loudnessLufs, segment.lowShareValid, segment.lowShare);
        impl->commit (segment, Impl::mainnessOf (factors), factors);
    }

    void LiveProfile::commitWithMainness (const SegmentLedger::Segment& segment, float mainness) noexcept
    {
        const auto factors = impl->factorsFor (segment.loudnessValid, segment.loudnessLufs, segment.lowShareValid, segment.lowShare);
        const auto clamped = std::isfinite (mainness) ? juce::jlimit (0.0f, 1.0f, mainness) : 0.0f;
        impl->commit (segment, clamped, factors);
    }

    float LiveProfile::mainnessFor (bool loudnessValid, float loudnessLufs, bool lowShareValid, float lowShare) const noexcept
    {
        return Impl::mainnessOf (impl->factorsFor (loudnessValid, loudnessLufs, lowShareValid, lowShare));
    }

    const MusicProfile& LiveProfile::getProfile() const noexcept   { return impl->profile; }
    LiveDiagnostics LiveProfile::getDiagnostics() const noexcept   { return impl->diagnostics(); }

    int LiveProfile::readCommits (std::uint64_t& cursor, LiveCommitRecord* out, int maxCount) const noexcept
    {
        return impl->readCommits (cursor, out, maxCount);
    }

    std::unique_ptr<juce::XmlElement> LiveProfile::createStateXml() const
    {
        return impl->createState();
    }

    bool LiveProfile::restoreStateXml (const juce::XmlElement& xml)
    {
        return impl->restoreState (xml);
    }
}

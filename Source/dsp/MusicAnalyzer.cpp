/*
  ==============================================================================

    MusicAnalyzer.cpp
    GLAZE 分析器（设计规格-GLAZE-分析器全文；设计规格-GLAZE-时间线与保存第 3 节的分析器一侧）。

    三路数据，各按自己的时间戳独立累计（所以结果与宿主块长无关）：
      一、子块流：IN 表的 100 ms 子块（K 加权均方 + 子块真峰值）→ M / S / I / LRA / 真峰值 / PLR / PSR、有效时长、代表性；
      二、帧流：Balance 每帧原始段功率（约 10 ms 一帧）→ 长期段电平、频谱正负证据、斜率、低 / 高频占比、辅助谱通量；
              v0.1.7.0 起另读同一帧的左右交叉功率 → 修坯 Clay 的立体声画像字段与实时立体声视图；
      三、样本流：链入口原始样本 → 起音包络（功率域）与事件；v0.1.7.0 起另有 M 的 8 阶 16 Hz 低通 → 修坯 Clay 的真次声占比，
              学习期间另把修坯的候选处理跑在原始样本上 → 先试后用的峰值代价（第「五」节，ADR-012）。
    修坯 Clay 的特征（下面第「四」「五」节）挂在帧流与样本流上，不另起时间轴、不新增 FFT。
    画像只由各路的累计量按纯函数算出（每次有新数据就重算），停止学习后固定。
    T-038 House 档（第「六」节）：三路数据另按段归进段落账（SegmentLedger），候选画像改由满编段门控算出；通用档不进第「六」节的任何代码。

    实时约束：全部状态在 Impl 里定长（构造时一次分配；先试后用的收单候选低通与 M / S 延迟环随采样率定长，prepare 时分配），
    音频线程上的函数不分配、不加锁、不做系统调用；时间常数一律按样本数折算。reset 逐项清零，不在栈上构造整个 Impl（约 40 KB）。

  ==============================================================================
*/

#include "MusicAnalyzer.h"

#include "GlazeProfile.h"
#include "LinearPhaseLowpass.h"
#include "TruePeak.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace celadon::glaze
{
    static_assert (numBands == BalanceModule::numBands, "GLAZE 画像的频段与 Balance 的分析频段必须一致");

    namespace
    {
        //==============================================================================
        // 一、标准测量（设计规格-GLAZE-分析器第 2 节；EBU R128 / Tech 3341 / Tech 3342）。读数不做任何额外平滑。
        constexpr int momentarySubBlocks = 4;      // M：400 ms 滑动窗，不门限
        constexpr int shortTermSubBlocks = 30;     // S：3 s 滑动窗，不门限（也是 PSR 的真峰值窗）
        constexpr int gatingSubBlocks    = 4;      // I 的门块：400 ms、75 % 重叠（每个子块出一个）
        constexpr double absoluteGateLufs         = -70.0;
        constexpr double integratedRelativeGateLu = -10.0;
        constexpr double lraRelativeGateLu        = -20.0;
        constexpr double lraLowFraction  = 0.10;   // P10
        constexpr double lraHighFraction = 0.95;   // P95
        constexpr int minimumLraValues = 10;       // 至少 1 s 的 S 取样（10 Hz）才给 LRA 数值；更少时「不知道」

        // I 与 LRA 的固定容量直方图：−70 … +10 LUFS，0.1 LU 一格。门块能量逐块精确累加，只有「在不在相对门之上」
        // 按格中心判断（误差 ≤ 0.05 LU）；LRA 的百分位取格中心（误差 ≤ 0.05 LU）。内存与耗时不随时长增长，
        // 相对门变了，历史门块自动重新参与判定。
        constexpr double histogramMinLufs = -70.0;
        constexpr int histogramBinsPerLu = 10;
        constexpr int histogramBins = 80 * histogramBinsPerLu;

        // 「有效音频」门限：400 ms 响度 ≥ −70 LUFS，且不低于学习区间 I 以下 20 LU（与 LRA 同一相对门）。
        // 有效时长、代表性、musicActive 都用它；淡出、段间静音、底噪不算学习时间。
        constexpr double effectiveRelativeGateLu = -20.0;

        // 不连续点之后丢掉起点落在「不连续点 + 64 样本」之前的子块：表的 8× 真峰值检测器窗长 64，拼接处的阶跃过冲只在这之内。
        constexpr int discontinuityGuardSamples = TruePeakDetector::tapsPerPhase;

        //==============================================================================
        // 置信度：各特征「观测充分」所需的有效时长（秒）。置信度 = min(观测充分度, 代表性)。
        constexpr double loudnessFullSeconds = 10.0;    // I、真峰值、PLR
        constexpr double lraFullSeconds      = 60.0;    // LRA：规格「前 60 s 标未稳定」
        constexpr double spectrumFullSeconds = 10.0;    // 频谱证据、斜率、占比、每段可信度
        constexpr double onsetFullSeconds    = 10.0;    // 起音密度 / 强度
        constexpr double minimumSpectrumSeconds = 1.0;  // 斜率 / 占比 / 通量至少 1 s 有效帧才给数值
        constexpr double minimumOnsetSeconds    = 1.0;  // 起音密度至少 1 s 有效时长才给数值
        constexpr int minimumOnsetEvents = 3;           // 起音强度至少 3 个事件才给数值（取中位数）

        // 代表性：不重复的有效素材达到 15 s（规格「15～30 s 快速片段画像」的下限）即为 1。拿不到宿主位置时无法识别重复，
        // 按有效时长计但封顶 0.75——低于出厂满置信度 0.8（GlazeConstants::fullConfidence）：保守降级，不阻止学习。
        constexpr double representativeSeconds = 15.0;
        constexpr double representativenessWithoutPosition = 0.75;

        // 覆盖图：按宿主播放位置记录听过哪些 100 ms 格；60 分钟（超出按取模折回，只会少算、不会多算）。
        constexpr int coverageCells = 36000;
        constexpr int coverageWords = (coverageCells + 63) / 64;

        //==============================================================================
        // 二、频谱证据（第 4 节）。口径同 Balance：1/3 倍频程段内总功率（粉噪为平），两声道功率平均。
        //   - 残差 r = 段电平 − 平滑轮廓。段电平先在功率域做一阶平均（所有段同一时间常数 1 s：单帧里 40 Hz 段只有一个
        //     FFT bin，逐帧读数标准差约 5.6 dB，不平均的话粉噪也满是「偏离」；各段时间常数相同，整体变响 / 变轻时各段同步，
        //     形状不被拉歪——Balance 各段时间常数不同，是靠一整套滞后参考补回来的，这里没必要）。重新开始平均后 1 s 才评估。
        //   - 平滑轮廓：每段用两侧 ±3 段（权重 3 / 2 / 1，不含本段）做加权局部直线拟合，取本段处的值：线性谱（粉噪、白噪、
        //     任意倾斜）残差恒为 0，两端的段也不偏。再按第一遍的残差给邻段做 Huber 降权（|r| > 容忍时权重 × 容忍 / |r|）
        //     重算一遍：否则一个 12 dB 的缺口会把两旁的轮廓拉低 3 dB，给邻段凭空记上 Tame 证据。
        //   - 超出 ±2 dB 容忍区间的正 / 负偏离分别累计（按段静音门权重），不互相抵消。
        constexpr double spectrumAveragingSeconds = 1.0;
        constexpr double spectrumSettleSeconds    = 1.0;
        constexpr double liveTameAttackSeconds    = 0.25;
        constexpr double liveTameReleaseSeconds   = 0.60;
        constexpr double liveTamePersistenceSeconds = 0.15;
        constexpr double liveTamePresentRatio     = 0.25;  // 原始帧至少仍有平滑功率的四分之一，防已结束的单个尖峰拖着权限走。
        constexpr double toleranceDb   = 2.0;
        constexpr double bandGateDb    = -60.0;    // 段静音门（与 Balance 相同）
        constexpr double bandGateFadeDb = 6.0;     // 门上 6 dB 内权重线性淡入
        constexpr double contourKernel[] = { 0.0, 3.0, 2.0, 1.0 };   // 下标 = 与本段的距离（本段自己不参与）
        constexpr int contourRadius = 3;
        constexpr double frameAbsoluteGatePower = 1.0e-7;   // 帧总功率 −70 dBFS 绝对门
        constexpr double frameRelativeGateRatio = 0.01;     // 帧总功率低于学习区间均值 20 dB 的帧不计（淡出、间歇）

        // Recover 比 Tame 保守：只对持续缺失（缺失帧占比 50 % 起算、80 % 满额）、且在有效带宽内的中间段。
        // 有效带宽：两端各自往里数，逐段比里侧邻段低 3 dB 以上的连续段是滚降区（歌本身的高频滚降、低频下潜），不填；
        // 两端的 40 Hz 与 16 kHz 段本身没有外侧邻段，分不清「缺口」还是「滚降」，一律不给 Recover。
        constexpr double recoverPersistenceStart = 0.5;
        constexpr double recoverPersistenceFull  = 0.8;
        constexpr double rollOffStepDb = 3.0;

        // 低频 / 高频占比的段边界（第 4 节）：低频 = 100 Hz 段及以下（段 0 ～ 4，上边界 112 Hz），
        // 高频 = 6.3 kHz 段及以上（段 22 ～ 26，下边界 5.6 kHz），总能量取全部 27 段；未加权线性功率。
        constexpr int lowShareLastBand   = 4;
        constexpr int highShareFirstBand = 22;
        constexpr int minimumSlopeBands  = 6;
        constexpr double noDataDb = -150.0;         // 长期电平没有数据 / 数字静音时写的值（有限数，存档可用）

        // 长时间静音里一阶平滑的状态会一路衰减进非规格化数（x86 没开 FTZ 时慢几十倍）：低于 −300 dB 就归零
        // （与各模块的 snapToZero 同一门槛；这么小的值对任何读数都没有影响）。
        constexpr double snapThreshold = 1.0e-30;

        void snapToZero (double& value) noexcept
        {
            if (std::abs (value) < snapThreshold)
                value = 0.0;
        }

        // 低频随时间的变化（第 4 节「低频占比对 Glue 只作风险提示，与低频随时间的变化……一起判断；持续低音与间歇重底鼓
        // 不走同一策略」）：有效帧上低频（段 0 ～ 4）总功率的逐帧电平，0.5 dB 一格的直方图，取 P90 − P10。
        // 持续低音几 dB（单帧估计本身的起伏），间歇重底鼓十几 dB 以上（鼓与鼓之间低频是空的）。
        constexpr double lowRangeMinDb = -120.0;
        constexpr double lowRangeBinDb = 0.5;
        constexpr int lowRangeBins = 280;           // −120 … +20 dB
        constexpr double lowRangeLowFraction = 0.10;
        constexpr double lowRangeHighFraction = 0.90;

        //==============================================================================
        // 三、起音活动（第 5 节）。输出事件密度（次 / 秒）与事件强度（dB）。
        //   - 功率 = 各声道样本平方的平均（两声道按功率合并）。先按约 0.33 ms 一组求均值（从重启起算的固定网格，与块边界无关），
        //     包络按组更新：比逐样本省十几倍，时间分辨率对起音绰绰有余。
        //   - 快包络 10 ms、慢包络 400 ms（功率域一阶平滑，重启后按运行均值起步）。快包络 ≥ 10 ms：20 Hz 正弦的功率纹波
        //     （40 Hz）只剩 ±37 %（+1.4 dB），40 Hz 正弦 ±20 %，都远低于 +6 dB 的触发门。
        //   - 快 / 慢 ≥ +6 dB 触发；触发后 50 ms 不应期（同一事件只计一次），不应期内的最大突起 = 事件强度；
        //     再回到 +3 dB 以下才重新待命（迟滞）。两个等幅失谐正弦的拍频（持续低音最常见的起伏）：快包络至多是均值的 2 倍，
        //     慢包络在 0.5 ～ 10 Hz 的拍频下跟得上一部分，按两个一阶响应推算快 / 慢最高约 +3 dB，再叠上 20 Hz 正弦的功率纹波
        //     也不到 +4.5 dB，+6 dB 留有余量；点击串到 16 次 / 秒仍有约 +7 dB（强度实测 7.4 dB）。
        //   - 有效信号门限：快包络低于 −60 dBFS 不触发；慢包络按 −70 dBFS 封底（静音后第一击的突起按它算，强度封顶 30 dB）。
        //     计入密度的时长与事件只取「慢包络 ≥ −70 dBFS 且不低于学习区间平均功率 20 dB 以上」的部分。
        //   - 重启（reset、不连续点）后 250 ms 只跑包络、不判事件。
        constexpr double onsetGroupSeconds      = 1.0 / 3000.0;
        constexpr int maximumGroupLength        = 128;          // 384 kHz 以下都是整 0.33 ms；再高就缩短组长（只会更细）
        constexpr double onsetFastSeconds       = 0.010;
        constexpr double onsetSlowSeconds       = 0.400;
        constexpr double onsetTriggerDb         = 6.0;
        constexpr double onsetRearmDb           = 3.0;
        constexpr double onsetRefractorySeconds = 0.050;
        constexpr double onsetWarmUpSeconds     = 0.250;
        constexpr double onsetTriggerFloorPower = 1.0e-6;      // −60 dBFS
        constexpr double onsetSlowFloorPower    = 1.0e-7;      // −70 dBFS
        constexpr double onsetEffectiveRelativeRatio = 0.01;   // −20 dB
        constexpr double onsetStrengthCapDb     = 30.0;
        constexpr double strengthBinDb          = 0.25;
        constexpr int strengthBins              = (int) (onsetStrengthCapDb / strengthBinDb) + 1;

        // 辅助量：250 Hz 以上段（段 8 ～ 26）的每帧正向谱通量，功率域：F = 10·log10(1 + Σ max(0, P本帧 − R) / Σ R)，
        // R = 该段前 3 帧（约 32 ms）功率的最大值——「这一帧 250 Hz 以上比刚才涨了多少 dB（只算涨的段）」。
        //   - 参考取前 3 帧的最大值而不是上一帧（SuperFlux 的做法）：低音的谐波只隔 1.8 个 FFT bin，主瓣互相交叠，
        //     一个完全稳定的锯齿低音在 250 ～ 450 Hz 各段的逐帧读数也会随相位交替起伏 2 ～ 4 dB（实测），逐帧相比会凑出每秒
        //     十几次假突起；真正的敲击之前 32 ms 是更安静的，照样算得出来。
        //   - 在功率域求和：谱线之间的低电平段起伏几个 dB 几乎不影响总和。参考总功率按每段 −60 dBFS 封底，从静音里冒出来时有界。
        // 27 个数恢复不了已被时间平滑掉的瞬态，只作辅助：「通量突起」= 高于 2 s 滑动均值的 3 倍且至少 3 dB、50 ms 不应期
        // （滑动均值含静音帧，稀疏的点击串也判得出）。学习区间内包络事件数 E 与通量突起数 F 比较：F < E 时起音两项的置信度
        // 乘 0.5 + 0.5·F / E——事件只在低频起伏（持续低音的拍频、慢颤音）而没有宽带瞬态时，置信度最低降到一半。
        constexpr int fluxFirstBand = 8;
        constexpr int fluxReferenceFrames = 3;
        constexpr double fluxFloorPowerPerBand = 1.0e-6;   // −60 dBFS
        constexpr double fluxMeanSeconds   = 2.0;
        constexpr double fluxWarmUpSeconds = 0.5;
        constexpr double fluxThresholdRatio = 3.0;
        constexpr double fluxMinimumDb = 3.0;
        constexpr double fluxRefractorySeconds = 0.050;
        constexpr double minimumOnsetAgreement = 0.5;

        //==============================================================================
        // 四、修坯 Clay 的立体声与超低频（v0.1.7.0；设计规格-Clay-分析决策与验收 1.2 ～ 1.4、设计规格-Clay-根治修订 3.1，
        //     GlazeTypes.h 画像格式 2 的字段注释）。
        //   - 真次声占比（样本流）：M = (L + R) / 2 过 8 阶 Butterworth 16 Hz 低通（四级 RBJ 低通，Q = 1 / (2·cos((2k − 1)·π/16))，
        //     k = 1 ～ 4，double），E_sub = Σ 低通输出²、E_全 = Σ M²，subShareDb = 10·log10(E_sub / E_全)。为什么是 8 阶 16 Hz
        //     （ADR-012）：原来的 4 阶 30 Hz 在底鼓基音 45 Hz 处只衰减 14 dB，真歌上测到的 −11 ～ −23 dB 主要是底鼓漏入；8 阶 16 Hz
        //     在 45 Hz 处衰减 72 dB、在 10 Hz 处只差 0.002 dB，测到的才是低切真正该清的次声（< 16 Hz，6 首真歌都在 −25 dB 以下）。
        //     累计与起音包络同一套边界：同一个 0.33 ms 组网格（从重启起算，与块边界无关）、学习区间按组样本精确（组的第一个样本
        //     不早于学习起点）、有效音频门 = 起音那一份（慢包络 ≥ −70 dBFS 且不低于学习区间平均功率 20 dB），重启后 250 ms 预热不计
        //     （Q = 2.56 那一级的振铃时间常数约 51 ms，250 ms 后的起跑瞬态在 −43 dB 以下，且只占预热后很短一段）。
        //     不直接用子块的 musicActive：它按 100 ms 子块判定、读到子块时才知道，套在样本上结果会随宿主块长变；起音那一份是
        //     同一设计（400 ms 电平、绝对 −70、相对 −20）在样本流上的实现。M 恒为 0（R = −L）时 E_全 = 0 → 不知道。
        //   - 立体声画像（帧流）：Balance 每帧的段功率 P 与交叉功率 X（钳在 [−P, P]），与频谱证据同一批帧（学习区间内、帧门限之上、
        //     1 s 平均已稳定）、同一逐段静音门权重（1 s 平均电平 −60 dBFS，门上 6 dB 淡入）做功率域时间和；
        //     P_M = (P + X) / 2、P_S = (P − X) / 2 → S/M = 10·log10(ΣP_S / ΣP_M)、ρ = ΣX / ΣP；组特征按组内各段的和。
        //     观测时长：逐段 = 门权重之和 × hop（与 bandConfidence 同口径）；组 = 每帧组内最大门权重之和 × hop（组里有内容的时间）。
        //   - 实时视图（帧流，不进画像）：帧门限之上的每一帧，低频组的 ΣX、ΣP 与高频组的 ΣP_M、ΣP_S（只计本帧段功率在
        //     −60 dBFS 门上的段）各做 300 ms 功率域一阶平滑，系数按 hop 换算、运行均值起步；重启（不连续点、丢帧、hop 变化）清空，
        //     重新平滑满 300 ms、且组内平滑功率仍在段静音门之上才有效。低频组另有一路 120 ms 快路（liveLowCorrelationFast：同门、
        //     同运行均值起步、同重启规则，平滑满 120 ms 才有效），liveLowCorrelation 只取慢路（ADR-012：不再取两路较小者，
        //     快路怎么用由 ClayGuard 定——只在慢路已明显离开同相时参与）。
        //     帧门限下（静音）两路低频相关度都标无效、平滑状态保留，回到门上接着平滑：ClayGuard 见到慢路无效就冻结反相一路，
        //     与这里的帧门是同一个口径（审核 V1-03：原来视图冻结时仍标有效，保护时钟却按编排器的另一个门走，可能拿过期的 ρ 攻击）。
        //     高频两项（宽度保护读）同理：帧门限下 liveHighValid = false、功率值保留，ClayGuard 见到无效就冻结宽度一路（T-012 阶段 4
        //     主进程按「同类一起修」补上：原来静音时高频仍标有效，宽度保护时钟与反相一路不同口径）。
        constexpr double subCutoffHz = 16.0;
        constexpr std::array<double, 4> subStageQ { 0.5097955791041592, 0.6013448869350453,    // 8 阶 Butterworth 四对极点的 Q
                                                    0.8999762231364156, 2.5629154477415055 };  // = 1 / (2·cos((2k − 1)·π/16))
        constexpr double subShareFloor = 1.0e-15;       // E_sub 按 E_全 的 1e-15 封底：subShareDb ≥ −150 dB（= noDataDb），防 −∞
        constexpr double minimumSubSeconds = 1.0;       // 至少 1 s 有效音频才给数值（同起音密度）
        constexpr double subFullSeconds = 10.0;         // 观测充分所需时长（同起音、频谱）
        constexpr int lowStereoFirstBand = 0;           // 低频组：段 0 ～ 5（40 ～ 125 Hz）
        constexpr int lowStereoLastBand = 5;
        constexpr int highStereoFirstBand = 17;         // 高频组：段 17 ～ 26（2 ～ 16 kHz）
        constexpr int highStereoLastBand = 26;
        constexpr double stereoRatioFloor = 1.0e-12;    // P_S、P_M 各按总功率的 1e-12 封底：S/M 限在 ±120 dB（纯单声道读 −120 dB，防 −∞）
        constexpr double minimumStereoSeconds = 1.0;    // 组特征至少 1 s 门上观测才给数值（同斜率 / 占比）
        constexpr double stereoFullSeconds = 10.0;      // 观测充分所需时长（同 bandConfidence）
        constexpr double liveStereoSeconds = 0.3;       // 实时视图的平滑时间常数；重启后平滑满这么久才标有效
        // 低频相关度快路（ADR-012，设计规格-Clay-根治修订 3.1 / 3.4；审核 V1-01）：120 ms，重启后平滑满 120 ms 才标有效。
        // 阶段 2 的 60 ms 快路与慢路取小：40 ～ 125 Hz 在 2048 点 FFT 里只有 4 ～ 5 个 bin，60 ms 平滑的估计散布约 ±0.5，宽立体声
        // 低音被大面积判成反相（V1：底鼓 + 独立宽垫、长期 ρ 0.87，30 s 触发 6 次；真歌 kskdhjf 75 %、KINOSEI 57 % 的时间在保护）。
        // 120 ms 把散布压到约 ±0.35（∝ 1 / √τ）；整体反相时它约 83 ms 过零（τ·ln 2），慢路约 210 ms。
        constexpr double liveFastLowSeconds = 0.12;
        constexpr double liveBandGatePower = 1.0e-6;    // 实时视图的逐段静音门：本帧段功率 −60 dBFS（与 bandGateDb 同一电平）

        static_assert (lowStereoLastBand < highStereoFirstBand && highStereoLastBand == numBands - 1,
                       "修坯的低频组 / 高频组段号必须落在 27 段之内且不重叠");

        //==============================================================================
        // 五、修坯 Clay 的先试后用（ADR-012，设计规格-Clay-根治修订 3.2）。为什么：最小相位 IIR 在低频的相位旋转会把已经很密 /
        //     削平的波形的峰值重新错开冒出来（真歌试渲染：低切让成品歌峰值 +4 dB、末级限幅多压、响度反而 −0.9 LU）；决策不能只看
        //     特征，要先量「这首歌真做了之后峰值怎样」。学习区间内把候选处理跑在链入口的原始样本上，量 100 ms 块峰值 P99 的变化。
        //   - 候选（各自单独作用在原始输入上；系数公式与 ClayModule 一致，double）：
        //       0      低切：L、R 各两级 RBJ 高通（4 阶 Butterworth，Q 同 ClayModule），20 Hz（自动低切目标的上限）；
        //       1 ～ 4 收单：线性相位收单 a = 1，分频点 clayMonoCandidateHz（60 / 80 / 120 / 160 Hz），与 Clay 同一个 LinearPhaseLowpass：
        //              S' = S 延迟 D_c − LP(S)、M 延迟 D_c，L' = M_d + S'、R' = M_d − S'。D_c = 2(L_c − 1) 是候选自己的群延迟——
        //              只需与 LP 的输出对齐，不必用 Clay 的最大延迟；
        //       5      加宽：S 过 2 kHz 高架（斜率 S = 1）+1.5 dB（自动加宽上限），L' = M + S''、R' = M − S''。
        //     max(|M + x|, |M − x|) = |M| + |x|，浮点下也精确（较大的那个本来就是 |M| + |x| 的一次舍入），收单与加宽直接按它算块峰值。
        //   - 块网格：从重启起算、与起音的组网格对齐（一块 = round(0.1 s / 组长) 个整组，48 kHz 下正好 4800 样本），与起音 / 真次声
        //     同一套边界——块里每个组都「过了重启预热、在学习区间内、在有效音频门上」才是有效块；另外候选（开始学习、重启时）从零状态
        //     起跑后 250 ms 内开始的块不算（IIR 起跑瞬态：20 Hz 那级高通振铃约 21 ms 的时间常数，250 ms 后低于 −100 dB）。
        //     候选输出晚 D_c 个样本：它的第 k 块取输出的 [k·块长 + D_c, (k + 1)·块长 + D_c)，与原始的第 k 块是同一段音频（延迟补偿）——
        //     纯单声道素材（S = 0）收单后逐位就是延迟了的原始，所以代价精确为 0。原始与 6 个候选都交齐了这一块才记下（D_c < 块长，
        //     任何时刻最多两块没交齐：2 个记录槽按块号奇偶轮用）。
        //   - P99 = 线性插值的百分位（定义同 numpy.percentile 默认；逐位口径以「存全部 + nth_element + 同一插值」为准——numpy 的 lerp 在 t ≥ 0.5 时换了写法，末位可能不同，审核 V4-04）：位置 0.99·(n − 1) 两侧的顺序统计量插值。最多记 512 个有效块
        //     （51.2 s：聆听最长 maxListenSeconds 30 s ≈ 300 块，余量给有效门口径的差别；满了之后的块不再记，代价按前 512 块算）。
        //     n ≤ 512 时 P99 只用到从大往小数第 7 个以内的值，所以每个序列只留最大的 8 个块峰值（有序插入）就能逐位得到与「存下全部
        //     块峰值再 std::nth_element」相同的结果——后者每记一块要对 7 × 最多 512 个数做选择，192 kHz / 32 样本块下是单块约 10 µs
        //     的尖峰（整插件最坏块验收会看到），这里每块只有几十次比较。
        //     代价 = 20·log10(P99 候选) − 20·log10(P99 原始)（dB，正 = 峰值变高）；有效块满 10 s 才有效，置信度同其他特征：
        //     min(观测充分度 = 有效块时长 / 10 s, 代表性)。
        //   - 候选只在学习期间跑（不学习时不耗 CPU）；每次开始学习、每次重启都从零状态重新起跑。
        constexpr double trialBlockSeconds = 0.1;
        constexpr int trialCapacity = 512;
        constexpr int trialTopCount = 8;
        constexpr double trialMinimumSeconds = 10.0;
        constexpr double trialFullSeconds = 10.0;
        constexpr double trialPercentile = 0.99;
        constexpr double trialWarmUpSeconds = onsetWarmUpSeconds;
        constexpr double trialLowCutHz = 20.0;
        constexpr std::array<double, 2> trialCutQ { 0.54119610014619698, 1.3065629648763766 };   // 同 ClayModule 的 cutFirstQ / cutSecondQ
        constexpr double trialWidthShelfHz = 2000.0;    // 同 ClayModule::widthShelfHz
        constexpr double trialWidthDb = 1.5;            // 自动加宽上限（GlazeConstants 的 clayWidthMaxDb 出厂值）
        constexpr double trialPeakFloorRatio = 1.0e-6;  // 候选 P99 按原始 P99 的 −120 dB 封底（例如低切把纯直流削成 0），代价不出 −∞
        constexpr int numMonoTrials = (int) clayMonoCandidateHz.size();
        constexpr int trialLowCut = 0, trialFirstMono = 1, trialWidth = 1 + numMonoTrials;
        constexpr int numTrials = 2 + numMonoTrials;    // 低切、收单 ×4、加宽

        // n ≤ 容量时，P99 的下侧顺序统计量从大往小数是第 n − 1 − ⌊0.99·(n − 1)⌋ ≤ 0.01·(n − 1) + 1 个（0 起），上侧再大一个。
        static_assert ((1.0 - trialPercentile) * (double) (trialCapacity - 1) + 1.0 < (double) (trialTopCount - 1),
                       "每个序列留的最大值个数必须覆盖容量内 P99 用到的两个顺序统计量");

        //==============================================================================
        // 六、T-038 House 档（设计规格-曲风-House学习方式 第 2 节；段的切分、定稿与门控见 SegmentLedger.h）。只在 setGenre 交来 House 子类时走。
        //   - 归段：三路数据各按自己的时间戳找所在的段，跨段边界的按起点归属——子块按子块起点（门块与 3 s 短期响度随产生它们的那个子块，
        //     窗口最早的样本早于段落账的起点就不收，同学习区间的口径）；帧按分析窗起点；起音组按组起点，起音事件按触发的那一组；
        //     先试后用的块按块起点（与原始同一个块网格，块网格的原点 = 起音组网格的原点）。
        //   - 每路数据进段的门与通用画像相同：子块用现有「有效音频」门（段响度只算过门的子块）；帧用帧门（绝对 −70 dBFS、不低于学习区间
        //     帧均值 20 dB；锁定后停止学习，相对门按学习区间冻结的均值接着用）；起音组与次声用起音那一份有效门；频谱证据与立体声同一批帧、
        //     同一逐段静音门权重。
        //   - 定稿：段尾早于「四路都越过的时刻」的段才定稿——子块（最后读到的子块末尾；表的响度计量关着时按此刻 − 两个子块）、帧（最后一帧
        //     分析窗起点 + hop；Balance 还没 prepare 时不等它）、起音（正在进行的事件的触发时刻，否则当前组的起点）、先试后用（学习中：
        //     还没交齐的最早一块的起点 = 已跑过的样本 − 最长的候选延迟，按块向下取整）。所以每段拿到的数据与宿主分块无关。
        //   - 画像（computeHouseProfile）只由段落账按纯函数算出，各字段的公式与通用画像的同名函数逐项相同（update*Profile、finalizeConfidences），
        //     只是累计量换成「满编段的和」（有效时长、代表性换成「全部段的和」；LRA 阶段 3 集成起也用满编段，起播位置无关）：
        //       I：满编段的 400 ms 门块按 BS.1770 相对门（−10 LU）重新门限——门块功率逐个存着，相对门逐个比（通用画像按 0.1 LU 格中心比，
        //          差 ≤ 0.05 LU）；真峰值 = 满编段的最大值；
        //       先试后用：满编段按格序整段整段地取，满 512 块为止（同通用画像的 512 块上限），每一路合并各段最大的 8 个块峰值——≤ 512 块时
        //          P99 只用到前 7 个，所以合并结果与把这些块全部存下再求 P99 逐位相同。满编段的块不够 10 s（锁定后才进来的段：学习停了，
        //          候选不跑）时改用全部段的块；
        //       代表性：全部段不重复的有效素材 / 15 s（不按位置的段封顶 0.75，同通用画像），再与「已听 ÷ 锁定所需」取小（小节：
        //          已听小节 ÷ lockSegments；2 秒段：已听秒数 ÷ fallbackLockSeconds）。
        static_assert (SegmentLedger::lowRangeBins == lowRangeBins && SegmentLedger::lowShareLastBand == lowShareLastBand,
                       "段落账的低频直方图与低频段边界必须与分析器相同");
        static_assert (SegmentLedger::trialSeries == numTrials + 1 && SegmentLedger::trialTopCount == trialTopCount
                       && SegmentLedger::trialCapacity == trialCapacity, "段落账的先试后用记账必须与分析器同口径");
        static_assert (strengthBins <= 256, "起音强度的格号按 8 位存");
        static_assert (SegmentLedger::loudnessHistogramMinLufs == histogramMinLufs && SegmentLedger::loudnessBinsPerLu == histogramBinsPerLu,
                       "段落账的响度直方图与分析器 I / LRA 的直方图同格");

        //==============================================================================
        double energyToLufs (double meanSquare) noexcept
        {
            return meanSquare > 0.0 ? -0.691 + 10.0 * std::log10 (meanSquare)
                                    : -std::numeric_limits<double>::infinity();
        }

        float toReading (double lufsOrDb) noexcept
        {
            // 数字静音的 −∞ 写成表的「没有读数」下限（有限数，存档与相减都安全）。
            return (float) juce::jmax ((double) LoudnessMeter::noReading, lufsOrDb);
        }

        int histogramBinOf (double lufs) noexcept
        {
            return juce::jlimit (0, histogramBins - 1, (int) std::floor ((lufs - histogramMinLufs) * (double) histogramBinsPerLu));
        }

        // 格中心 ≥ gate 的第一格（可能等于 histogramBins：没有一格在门之上）。
        int firstBinAtOrAbove (double gate) noexcept
        {
            const auto position = (gate - histogramMinLufs) * (double) histogramBinsPerLu - 0.5;

            if (position <= 0.0)
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

        // 修坯 Clay 的立体声比值（第「四」节）。power = 段内（或组内）功率和 ΣP > 0，cross = 交叉功率和 ΣX（逐帧已钳在 [−P, P]）。
        // S/M：P_M = (P + X) / 2、P_S = (P − X) / 2，两者各按 P 的 1e-12 封底——纯单声道（X = P）读 −120 dB、纯侧声道读 +120 dB，
        // 不出 ±∞；多帧求和的舍入让 X 略越出 [−P, P] 时也不会取负数的对数。
        double sideToMidDbOf (double power, double cross) noexcept
        {
            const auto floorPower = stereoRatioFloor * power;
            const auto mid = juce::jmax (floorPower, 0.5 * (power + cross));
            const auto side = juce::jmax (floorPower, 0.5 * (power - cross));
            return 10.0 * std::log10 (side / mid);
        }

        // ρ = ΣX / ΣP，钳在 [−1, 1]（同上，只挡舍入）。
        double correlationOf (double power, double cross) noexcept
        {
            return juce::jlimit (-1.0, 1.0, cross / power);
        }
    }

    //==============================================================================
    struct MusicAnalyzer::Impl
    {
        Impl()   { configure (48000.0, 2); allocate(); resetAll(); }

        //==============================================================================
        // 配置（prepare）
        double sampleRate = 48000.0;
        int numChannels = 2;
        int subBlockLength = 4800;
        int groupLength = 16;
        double fastRetain = 0.0, slowRetain = 0.0;
        std::int64_t warmUpGroups = 0;
        int refractoryGroups = 1;
        double triggerRatio = 0.0, rearmRatio = 0.0;

        // 修坯 Clay 的双二阶（样本流，第「四」「五」节：真次声低通、先试后用的低切高通与加宽高架），系数按采样率算
        // （转置直接 II 型，double）。
        struct BiquadCoefficients
        {
            double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
        };

        struct BiquadState
        {
            double s1 = 0.0, s2 = 0.0;
        };

        std::array<BiquadCoefficients, subStageQ.size()> subLowpass {};

        // 先试后用（第「五」节）的配置：块网格、各候选的系数与延迟（configure 里按采样率算）。
        int trialBlockGroups = 1, trialBlockLength = 1;         // 一块 = trialBlockGroups 个整组 = trialBlockLength 个样本
        std::int64_t trialWarmUpSamples = 0;                    // 候选从零状态起跑后，这么多样本之内开始的块不算
        std::array<BiquadCoefficients, trialCutQ.size()> trialCut {};   // 低切：两级高通（L、R 同一组系数）
        BiquadCoefficients trialShelf;                          // 加宽：S 的高架
        std::array<int, (size_t) numMonoTrials> trialMonoLength {};     // 收单：各分频点的滑动平均长度 L_c
        std::array<int, (size_t) numTrials> trialDelay {};      // 各候选输出相对原始的延迟：低切 / 加宽 0，收单 D_c = 2(L_c − 1)

        // 帧流的常数随 Balance 的 hop 定（第一次读到帧时算，hop 变了重算）
        int hop = 0, frameWindow = 0;
        double spectrumRetain = 0.0, fluxMeanRetain = 0.0;
        double liveTameAttackRetain = 0.0, liveTameReleaseRetain = 0.0;
        int settleFrames = 1, fluxWarmUpFrames = 1, fluxRefractoryFrames = 1;
        double liveStereoRetain = 0.0;      // 修坯 Clay 实时视图 300 ms 平滑的每帧保留系数
        double liveFastLowRetain = 0.0;     // 低频相关度快路 120 ms 平滑的每帧保留系数
        int liveStereoSettleFrames = 1;     // 重启后平滑满 300 ms 所需的帧数
        int liveFastSettleFrames = 1;       // 重启后快路平滑满 120 ms 所需的帧数

        //==============================================================================
        // 时间轴：IN 表的累计样本数
        bool haveTime = false;
        std::int64_t now = 0;                    // 上一次 processInput 之后的时间戳
        int lastBlockSamples = 0;
        std::int64_t subBlockCursor = 0;
        bool haveBalanceTime = false;
        std::int64_t lastBalanceSamples = 0;
        std::int64_t lastSpectrumInputSamples = 0;
        std::int64_t frameCursor = 0;
        std::int64_t discardSubBlocksBefore = 0; // 起点早于它的子块丢弃（跨越不连续点）
        std::int64_t discardFramesBefore = 0;    // 分析窗起点早于它的帧丢弃
        HostInfo host;

        bool learning = false;
        std::int64_t learnStart = 0;

        //==============================================================================
        // 一、子块流
        struct SubBlockEntry
        {
            double meanSquare = 0.0;
            float truePeak = 0.0f;
        };

        std::array<SubBlockEntry, (size_t) shortTermSubBlocks> window {};
        int windowWrite = 0;                     // 下一个写入位置
        int windowCount = 0;                     // 连续的子块数（≤ 30）
        std::int64_t lastSubBlockEnd = -1;       // 最近一个接受的子块的 endSample（-1 = 没有）

        std::array<double, (size_t) histogramBins> integratedEnergy {};
        std::array<std::uint32_t, (size_t) histogramBins> integratedCount {};
        double integratedTotalEnergy = 0.0;
        std::uint64_t integratedTotalCount = 0;
        int integratedTopBin = -1;
        bool learnIntegratedValid = false;
        double learnIntegratedLufs = 0.0;

        std::array<std::uint32_t, (size_t) histogramBins> rangeCount {};
        double rangeTotalEnergy = 0.0;
        std::uint64_t rangeTotalCount = 0;
        int rangeTopBin = -1;

        float learnPeak = 0.0f;
        std::int64_t effectiveSamples = 0;
        std::array<std::uint64_t, (size_t) coverageWords> coverage {};
        std::int64_t coveredCells = 0;
        std::int64_t unpositionedSamples = 0;
        bool loudnessDirty = false;

        //==============================================================================
        // 二、帧流
        std::array<double, (size_t) numBands> averagedPower {};
        std::array<double, (size_t) numBands> livePositiveSeconds {};
        double liveTameEvidence = 0.0;
        std::int64_t framesSinceRestart = 0;
        bool havePreviousFrame = false;
        std::int64_t previousFrameEnd = 0;
        std::array<std::array<double, (size_t) numBands>, (size_t) fluxReferenceFrames> recentFramePower {};   // 前几帧的段功率（环形）
        int recentFrameWrite = 0;
        int recentFrameCount = 0;
        double fluxMean = 0.0;
        std::int64_t fluxFrames = 0;
        std::int64_t framesSinceFluxEvent = 0;

        std::array<double, (size_t) numBands> powerSum {};
        std::int64_t learnFrames = 0;
        double learnTotalPowerSum = 0.0;
        std::array<double, (size_t) numBands> tameSum {}, recoverSum {}, deficitWeight {}, validWeight {};
        double learnFluxSum = 0.0;
        std::int64_t learnFluxFrames = 0, learnMusicFluxFrames = 0, learnFluxEvents = 0;
        std::array<std::uint32_t, (size_t) lowRangeBins> lowRangeCount {};
        bool spectrumDirty = false;

        // 修坯 Clay 的立体声（第「四」节）：画像累计（学习区间内，按段静音门加权的功率域时间和）与实时视图的平滑状态
        std::array<double, (size_t) numBands> stereoPowerSum {}, stereoCrossSum {}, stereoWeight {};
        double lowStereoWeight = 0.0, highStereoWeight = 0.0;   // 组里有内容的观测：每帧取组内最大的门权重
        double liveLowCross = 0.0, liveLowPower = 0.0;          // 低频组 ΣX、ΣP 的 300 ms 平滑（慢路）
        double liveLowCrossFast = 0.0, liveLowPowerFast = 0.0;  // 同上的 120 ms 快路
        double liveHighMid = 0.0, liveHighSide = 0.0;           // 高频组 ΣP_M、ΣP_S 的 300 ms 平滑
        std::int64_t liveStereoFrames = 0;                      // 重启以来更新过的帧数（运行均值起步、「平滑满 300 ms」用）

        std::array<BalanceModule::FrameSnapshot, 8> frameScratch {};
        std::array<LoudnessMeter::SubBlock, 16> subBlockScratch {};

        //==============================================================================
        // 三、样本流（起音）
        enum class OnsetState { armed, inEvent, waitingRearm };

        std::array<double, (size_t) maximumGroupLength> groupPower {};
        int groupFill = 0;
        double fast = 0.0, slow = 0.0;
        std::int64_t groupsSinceRestart = 0;
        OnsetState onsetState = OnsetState::armed;
        int refractoryLeft = 0;
        double eventPeakRatio = 0.0;
        bool eventCounts = false;

        std::int64_t onsetEffectiveSamples = 0;
        std::int64_t onsetEvents = 0;
        std::array<std::uint32_t, (size_t) strengthBins> strengthCount {};
        double onsetPowerSum = 0.0;
        std::int64_t onsetPowerGroups = 0;
        bool onsetDirty = false;

        // 修坯 Clay 的真次声占比（第「四」节）：与起音同一个组网格、同一套边界；有效时长就是 onsetEffectiveSamples
        std::array<BiquadState, subStageQ.size()> subState {};
        double groupSubEnergy = 0.0, groupMidEnergy = 0.0;   // 本组（还没完成）的 Σ 低通输出²、Σ M²
        double subEnergy = 0.0, midEnergy = 0.0;             // 学习区间内有效组的累计 E_sub、E_全

        //==============================================================================
        // 五、先试后用（样本流，只在学习期间跑）
        // 候选的滤波器状态（每次起跑清零）。收单候选的低通与 M / S 延迟环随采样率定长：allocate（prepare）里分配，音频线程只清零。
        std::array<LinearPhaseLowpass, (size_t) numMonoTrials> trialMonoLowpass;
        std::vector<double> trialMidHistory, trialSideHistory;      // M、S 的延迟环（2 的幂长度 > 最长的 D_c）
        int trialHistoryMask = 0;
        int trialHistoryWrite = 0;
        std::array<std::array<BiquadState, trialCutQ.size()>, 2> trialCutState {};   // [声道][级]
        BiquadState trialShelfState;
        bool trialRestartPending = true;    // 下一个学习中的样本前先让候选从零状态重新起跑（开始学习、重启、坏样本之后）
        std::int64_t trialRunSamples = 0;   // 候选起跑以来跑过的样本数（封顶在预热长度：只用来判断「过了预热」）

        // 块网格上的记账：原始的当前块（随组推进，块末在 processGroup 里交），各候选的当前块（按自己的延迟对齐，按样本倒数）。
        double trialOriginalMax = 0.0;      // 原始当前块的峰值 max(|L|, |R|)
        bool trialBlockWarm = false;        // 原始当前块开始时候选已过预热
        bool trialBlockValid = false;       // 原始当前块到目前为止的每个组都算数
        std::array<double, (size_t) numTrials> trialCandidateMax {};         // 各候选当前块的峰值
        std::array<int, (size_t) numTrials> trialCandidateLeft {};           // 各候选当前块还差几个输出交齐
        std::array<std::int64_t, (size_t) numTrials> trialCandidateBlock {}; // 各候选当前块的块号（与原始同一个网格）

        // 一块的交齐记录：原始与 6 个候选各写一次，写满才记下（有效的话）。2 个槽按块号奇偶轮用（D_c < 块长）。
        struct TrialRecord
        {
            std::int64_t block = -1;
            double original = 0.0;
            bool valid = false;
            int writes = 0;
            std::array<double, (size_t) numTrials> candidates {};
        };

        std::array<TrialRecord, 2> trialRecords {};

        // 记下的有效块：[0] 原始、[1 + c] 候选 c 各自最大的 trialTopCount 个块峰值（降序），与块数一起足以求 P99（见第「五」节）。
        std::array<std::array<double, (size_t) trialTopCount>, (size_t) numTrials + 1> trialTop {};
        int trialCount = 0;                 // 记下的有效块数（封顶 trialCapacity）
        bool trialDirty = false;            // 记下了新块，画像的代价字段待重算

        // 一段（≤ 一个组长）的逐样本暂存：M、S、各收单候选的 LP(S)、各候选的逐样本峰值。定长，不分配。
        std::array<double, (size_t) maximumGroupLength> trialMidScratch {}, trialSideScratch {};
        std::array<std::array<double, (size_t) maximumGroupLength>, (size_t) numMonoTrials> trialLowScratch {};
        std::array<std::array<double, (size_t) maximumGroupLength>, (size_t) numTrials> trialPeakScratch {};

        //==============================================================================
        // 输出
        MusicProfile profile;
        ShortTermView view;
        AuxiliaryFeatures aux;
        float representativeness = 0.0f;
        float measuredIntegrated = 0.0f, measuredRange = 0.0f, measuredPeak = 0.0f;
        float measuredSpectrum = 0.0f, measuredOnset = 0.0f, measuredFlux = 0.0f;
        std::array<float, (size_t) numBands> measuredBand {};
        float measuredSub = 0.0f, measuredLowStereo = 0.0f, measuredHighStereo = 0.0f;   // 修坯 Clay 各特征的观测充分度
        float measuredTrial = 0.0f;                                                       // 先试后用：有效块时长 / 10 s
        std::array<float, (size_t) numBands> measuredStereoBand {};

        //==============================================================================
        // 六、House 档（第「六」节；通用档不读写下面任何一项）
        Genre genre = Genre::general;
        bool houseActive = false;                 // isHouse (genre)
        HouseConstants houseConstants;
        SegmentLedger ledger;
        MusicProfile houseProfile;                // House 档的候选画像（getCandidateProfile 在 House 档返回它）
        AuxiliaryFeatures houseAux;
        bool houseDirty = false;                  // 段落账有新定稿的段，画像还没按它重算（不学习时留着，等 recomputeHouseProfile）
        std::uint32_t houseGeneration = 0;
        int houseSubHint = -1, houseFrameHint = -1, houseOnsetHint = -1, houseTrialHint = -1;   // 各路找段的缓存
        std::int64_t houseSubBlockPosition = 0;   // 子块流读到的位置（最后一个子块的末尾）
        std::int64_t houseFramePosition = 0;      // 帧流读到的位置（最后一帧分析窗起点 + hop）
        std::int64_t onsetGridOrigin = 0;         // 起音组网格（也是先试后用的块网格）原点的分析器时间
        bool houseGridKnown = false;              // 上次重启以来已在 House 档里跑过样本（原点已知）
        bool houseEventPending = false;           // 正在进行、要计入段落账的起音事件（触发时在有效门上）
        std::int64_t houseEventTime = 0;          // 它的触发组的起点
        int maxTrialDelay = 0;                    // 先试后用候选里最长的延迟（样本）

        // 画像合并用的累计量（computeHouseProfile 每次从段落账重新合并；口径同通用画像的同名累计量）。
        struct HouseSums
        {
            double effectiveSeconds = 0.0, positionedSeconds = 0.0, unpositionedSeconds = 0.0, fullSeconds = 0.0;
            float peak = 0.0f;
            std::array<std::uint32_t, (size_t) histogramBins> rangeCount {};
            double rangeTotalEnergy = 0.0;
            std::uint64_t rangeTotalCount = 0;
            int rangeTopBin = -1;
            std::int64_t frames = 0;
            std::array<double, (size_t) numBands> powerSum {}, tameSum {}, recoverSum {}, deficitWeight {}, validWeight {};
            std::array<double, (size_t) numBands> stereoPowerSum {}, stereoCrossSum {}, stereoWeight {};
            double lowStereoWeight = 0.0, highStereoWeight = 0.0;
            std::array<std::uint32_t, (size_t) lowRangeBins> lowRangeCount {};
            std::int64_t fluxFrames = 0, musicFluxFrames = 0, fluxEvents = 0;
            double fluxSum = 0.0;
            std::int64_t onsetEffectiveSamples = 0, onsetEvents = 0;
            std::array<std::uint32_t, (size_t) strengthBins> strengthCount {};
            double subEnergy = 0.0, midEnergy = 0.0;
            std::array<std::array<double, (size_t) trialTopCount>, (size_t) numTrials + 1> trialTop {};
            int trialCount = 0;
        };

        // House 画像各特征的观测充分度（口径同通用画像的 measured*）。
        struct HouseMeasured
        {
            float integrated = 0.0f, range = 0.0f, peak = 0.0f, spectrum = 0.0f, onset = 0.0f, flux = 0.0f;
            float sub = 0.0f, lowStereo = 0.0f, highStereo = 0.0f, trial = 0.0f;
            std::array<float, (size_t) numBands> band {}, stereoBand {};
        };

        HouseSums houseSums;
        HouseMeasured houseMeasured;
        float houseRepresentativeness = 0.0f;

        //==============================================================================
        void configure (double newSampleRate, int newNumChannels) noexcept
        {
            sampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;
            numChannels = juce::jmax (1, newNumChannels);
            subBlockLength = juce::jmax (1, juce::roundToInt (sampleRate * 0.1));   // 与 LoudnessMeter 同一公式

            groupLength = juce::jlimit (1, maximumGroupLength, juce::roundToInt (sampleRate * onsetGroupSeconds));
            const auto groupSeconds = (double) groupLength / sampleRate;
            fastRetain = std::exp (-groupSeconds / onsetFastSeconds);
            slowRetain = std::exp (-groupSeconds / onsetSlowSeconds);
            warmUpGroups = (std::int64_t) std::ceil (onsetWarmUpSeconds / groupSeconds);
            refractoryGroups = juce::jmax (1, (int) std::ceil (onsetRefractorySeconds / groupSeconds));
            triggerRatio = std::pow (10.0, onsetTriggerDb * 0.1);
            rearmRatio = std::pow (10.0, onsetRearmDb * 0.1);

            // 修坯 Clay 的真次声低通（截止频率封顶 0.45 fs 只防病态采样率；正常采样率下恒为 16 Hz）。
            const auto subFrequency = juce::jmin (subCutoffHz, 0.45 * sampleRate);

            for (size_t stage = 0; stage < subLowpass.size(); ++stage)
                subLowpass[stage] = makeLowpass (sampleRate, subFrequency, subStageQ[stage]);

            // 先试后用（第「五」节）：块 = 整数个组（与起音的组网格对齐：44.1 / 48 / 96 / 192 kHz 下正好 100 ms）。
            trialBlockGroups = juce::jmax (1, juce::roundToInt (trialBlockSeconds / groupSeconds));
            trialBlockLength = trialBlockGroups * groupLength;
            trialWarmUpSamples = (std::int64_t) std::ceil (trialWarmUpSeconds * sampleRate);

            for (size_t stage = 0; stage < trialCut.size(); ++stage)
                trialCut[stage] = makeHighpass (sampleRate, trialLowCutHz, trialCutQ[stage]);

            trialShelf = makeHighShelf (sampleRate, trialWidthShelfHz, trialWidthDb);
            trialDelay.fill (0);

            for (size_t c = 0; c < trialMonoLength.size(); ++c)
            {
                trialMonoLength[c] = LinearPhaseLowpass::lengthFor (sampleRate, (double) clayMonoCandidateHz[c]);
                trialDelay[(size_t) trialFirstMono + c] = LinearPhaseLowpass::groupDelayFor (trialMonoLength[c]);

                // 记录槽按块号奇偶轮用的前提：候选的延迟短于一块（60 Hz 的 D_c ≈ 0.008·fs，一块 ≈ 0.1·fs，任何采样率都成立）。
                jassert (trialDelay[(size_t) trialFirstMono + c] < trialBlockLength);
            }

            maxTrialDelay = 0;

            for (const auto delay : trialDelay)
                maxTrialDelay = juce::jmax (maxTrialDelay, delay);

            hop = 0;
            frameWindow = 0;
        }

        // prepare（消息线程，也在构造时）：先试后用里随采样率定长的部分——收单候选的线性相位低通（按各自的 L_c）与
        // M / S 延迟环（2 的幂长度，大于最长的 D_c）。之后音频线程上只清零，不再分配。
        void allocate()
        {
            auto longestDelay = 0;

            for (size_t c = 0; c < trialMonoLowpass.size(); ++c)
            {
                trialMonoLowpass[c].prepare (trialMonoLength[c]);
                longestDelay = juce::jmax (longestDelay, trialDelay[(size_t) trialFirstMono + c]);
            }

            const auto historyLength = juce::nextPowerOfTwo (longestDelay + 1);
            trialMidHistory.assign ((size_t) historyLength, 0.0);
            trialSideHistory.assign ((size_t) historyLength, 0.0);
            trialHistoryMask = historyLength - 1;
            trialHistoryWrite = 0;
            trialRestartPending = true;

            // T-038 House 档的段落账（第「六」节）：按采样率分配，之后音频线程只拷贝、清零（先交常数：prepare 按新采样率折算段长）。
            ledger.setConstants (houseConstants);
            ledger.prepare (sampleRate, subBlockLength);
        }

        void updateFrameConstants (int newHop, int newWindow) noexcept
        {
            hop = newHop;
            frameWindow = newWindow;
            const auto hopSeconds = (double) hop / sampleRate;
            spectrumRetain = std::exp (-hopSeconds / spectrumAveragingSeconds);
            liveTameAttackRetain = std::exp (-hopSeconds / liveTameAttackSeconds);
            liveTameReleaseRetain = std::exp (-hopSeconds / liveTameReleaseSeconds);
            fluxMeanRetain = std::exp (-hopSeconds / fluxMeanSeconds);
            settleFrames = juce::jmax (1, (int) std::ceil (spectrumSettleSeconds / hopSeconds));
            fluxWarmUpFrames = juce::jmax (1, (int) std::ceil (fluxWarmUpSeconds / hopSeconds));
            fluxRefractoryFrames = juce::jmax (1, (int) std::ceil (fluxRefractorySeconds / hopSeconds));
            liveStereoRetain = std::exp (-hopSeconds / liveStereoSeconds);
            liveFastLowRetain = std::exp (-hopSeconds / liveFastLowSeconds);
            liveStereoSettleFrames = juce::jmax (1, (int) std::ceil (liveStereoSeconds / hopSeconds));
            liveFastSettleFrames = juce::jmax (1, (int) std::ceil (liveFastLowSeconds / hopSeconds));
        }

        //==============================================================================
        void resetAll() noexcept
        {
            haveTime = false;
            now = 0;
            lastBlockSamples = 0;
            subBlockCursor = 0;
            haveBalanceTime = false;
            lastBalanceSamples = 0;
            lastSpectrumInputSamples = 0;
            frameCursor = 0;
            discardSubBlocksBefore = 0;
            discardFramesBefore = 0;
            host = HostInfo {};
            learning = false;
            learnStart = 0;

            clearWindows();
            restartSpectrum();
            restartOnsets();
            clearLearning();
            clearHouse (0);
        }

        void clearLearning() noexcept
        {
            integratedEnergy.fill (0.0);
            integratedCount.fill (0);
            integratedTotalEnergy = 0.0;
            integratedTotalCount = 0;
            integratedTopBin = -1;
            learnIntegratedValid = false;
            learnIntegratedLufs = 0.0;

            rangeCount.fill (0);
            rangeTotalEnergy = 0.0;
            rangeTotalCount = 0;
            rangeTopBin = -1;

            learnPeak = 0.0f;
            effectiveSamples = 0;
            coverage.fill (0);
            coveredCells = 0;
            unpositionedSamples = 0;

            powerSum.fill (0.0);
            learnFrames = 0;
            learnTotalPowerSum = 0.0;
            tameSum.fill (0.0);
            recoverSum.fill (0.0);
            deficitWeight.fill (0.0);
            validWeight.fill (0.0);
            learnFluxSum = 0.0;
            learnFluxFrames = 0;
            learnMusicFluxFrames = 0;
            learnFluxEvents = 0;
            lowRangeCount.fill (0);

            onsetEffectiveSamples = 0;
            onsetEvents = 0;
            strengthCount.fill (0);
            onsetPowerSum = 0.0;
            onsetPowerGroups = 0;

            // 修坯 Clay 的候选累计（实时视图与低通状态不在这里：它们与学习无关，重新聆听不打断）。
            stereoPowerSum.fill (0.0);
            stereoCrossSum.fill (0.0);
            stereoWeight.fill (0.0);
            lowStereoWeight = 0.0;
            highStereoWeight = 0.0;
            subEnergy = 0.0;
            midEnergy = 0.0;

            // 先试后用：记下的块清空；候选只在学习期间跑，新的学习区间从零状态重新起跑（下一个学习中的样本前）。
            for (auto& top : trialTop)
                top.fill (0.0);

            trialCount = 0;
            trialDirty = false;
            trialRestartPending = true;

            loudnessDirty = spectrumDirty = onsetDirty = false;

            profile = MusicProfile {};   // 格式版本 = MusicProfile 的默认值 2（v0.1.7.0 起分析器产出含修坯字段的画像）
            aux = AuxiliaryFeatures {};
            representativeness = 0.0f;
            measuredIntegrated = measuredRange = measuredPeak = 0.0f;
            measuredSpectrum = measuredOnset = measuredFlux = 0.0f;
            measuredBand.fill (0.0f);
            measuredSub = measuredLowStereo = measuredHighStereo = 0.0f;
            measuredTrial = 0.0f;
            measuredStereoBand.fill (0.0f);
        }

        // 子块窗口（M / S / 门块 / PSR）断开：之后按新到的子块重新填满。
        void clearWindows() noexcept
        {
            windowWrite = 0;
            windowCount = 0;
            lastSubBlockEnd = -1;
            invalidate (view.momentaryLufs);
            invalidate (view.shortTermLufs);
            invalidate (view.psr3sDb);
            view.musicActive = false;
        }

        // 帧流的时间平均（段电平、谱通量统计）重新开始；已完成的累计不动。
        void restartSpectrum() noexcept
        {
            averagedPower.fill (0.0);
            livePositiveSeconds.fill (0.0);
            liveTameEvidence = 0.0;
            invalidate (view.liveTameEvidenceDb);
            invalidate (view.liveSpectralSlopeDbPerOct);
            view.liveSlopeWeights.fill (0.0f);
            framesSinceRestart = 0;
            havePreviousFrame = false;
            recentFrameWrite = 0;
            recentFrameCount = 0;
            previousFrameEnd = 0;
            fluxMean = 0.0;
            fluxFrames = 0;
            framesSinceFluxEvent = 0;

            // 修坯 Clay 的实时立体声视图同其他实时频谱项：跨不连续点的平滑不再代表此刻，清空后重新平滑满各自的时间常数
            // （慢路与高频 300 ms、快路 120 ms）才有效。
            liveLowCross = liveLowPower = 0.0;
            liveLowCrossFast = liveLowPowerFast = 0.0;
            liveHighMid = liveHighSide = 0.0;
            liveStereoFrames = 0;
            invalidate (view.liveLowCorrelation);
            invalidate (view.liveLowCorrelationFast);
            view.liveHighMidPower = 0.0f;
            view.liveHighSidePower = 0.0f;
            view.liveHighValid = false;
        }

        void restartOnsets() noexcept
        {
            groupFill = 0;
            fast = 0.0;
            slow = 0.0;
            groupsSinceRestart = 0;
            onsetState = OnsetState::armed;
            refractoryLeft = 0;
            eventPeakRatio = 0.0;
            eventCounts = false;

            // 修坯 Clay 的真次声低通与本组能量随组网格一起重起（重启后的起始瞬态落在 250 ms 预热里，不计入）。
            subState.fill (BiquadState {});
            groupSubEnergy = 0.0;
            groupMidEnergy = 0.0;

            // 先试后用的块网格随组网格一起从这里重新数；候选在下一个学习中的样本前从零状态重新起跑（清零在音频线程上做，
            // 不学习时不做——候选反正不跑）。已记下的有效块不动（同其他累计）。
            trialRestartPending = true;

            // House 档（第「六」节）：进行中的事件作废（与上面的事件状态一起），组网格的原点等下一个样本再定。
            houseEventPending = false;
            houseGridKnown = false;
        }

        // 候选从零状态重新起跑（第「五」节）。position = 下一个要跑的样本从重启起算的序号（组网格上的位置）。只清零、不分配。
        void restartTrial (std::int64_t position) noexcept
        {
            trialRestartPending = false;

            for (auto& channelStates : trialCutState)
                channelStates.fill (BiquadState {});

            trialShelfState = BiquadState {};

            for (size_t c = 0; c < trialMonoLowpass.size(); ++c)
                trialMonoLowpass[c].reset (trialMonoLength[c]);

            std::fill (trialMidHistory.begin(), trialMidHistory.end(), 0.0);
            std::fill (trialSideHistory.begin(), trialSideHistory.end(), 0.0);
            trialHistoryWrite = 0;
            trialRunSamples = 0;

            // 原始的当前块在候选起跑之前就开始了：不算。各候选的当前块按自己的延迟对到同一个网格上——候选此刻的输出对应原始的
            // 第 position − D_c 个样本；早于重启点的那 D_c 个输出（延迟环里的 0）并进第 0 块，而第 0 块落在重启后 250 ms 的预热里。
            trialOriginalMax = 0.0;
            trialBlockWarm = false;
            trialBlockValid = false;
            const auto blockSamples = (std::int64_t) trialBlockLength;

            for (size_t c = 0; c < (size_t) numTrials; ++c)
            {
                const auto source = position - (std::int64_t) trialDelay[c];
                trialCandidateMax[c] = 0.0;
                trialCandidateBlock[c] = source >= 0 ? source / blockSamples : 0;
                trialCandidateLeft[c] = (int) (source >= 0 ? blockSamples - source % blockSamples : blockSamples - source);
            }

            for (auto& record : trialRecords)
                record = TrialRecord {};
        }

        // 在时间戳 at 处断开：丢弃跨越它的全部窗口（子块、帧、包络）。已完成的累计（直方图、证据、事件数、有效时长）不动。
        void beginDiscontinuity (std::int64_t at) noexcept
        {
            discardSubBlocksBefore = at + (at > 0 ? discontinuityGuardSamples : 0);
            discardFramesBefore = at;
            clearWindows();
            restartSpectrum();
            restartOnsets();

            // House 档：跨越不连续点的数据上面都丢了，段落账里没定稿的段已经交齐，就地定稿；下一块重新锚定。
            if (houseActive)
                houseEndEpoch (at);
        }

        //==============================================================================
        // 一、子块流
        //==============================================================================
        void processInput (const juce::AudioBuffer<float>& input, const LoudnessMeter& meter) noexcept
        {
            const auto numSamples = input.getNumSamples();
            const auto meterNow = meter.getProcessedSamples();
            const auto blockStart = meterNow - (std::int64_t) numSamples;

            // 子块长度以表为准（两边同一公式；表若按别的采样率准备过，跟着它，窗口重新填）。
            if (meter.getSubBlockLength() != subBlockLength)
            {
                subBlockLength = juce::jmax (1, meter.getSubBlockLength());
                ledger.setSubBlockLength (subBlockLength);   // House 档段落账的「有效时长」按它折算（只存一个数）
                beginDiscontinuity (blockStart);
            }

            // 时间轴核对：本块之后表的累计样本数应 = 上一块之后 + 本块样本数。对不上就在本块起点断开：
            // 比上一块之后还少 = 表被 reset 过（游标归零、学习区间的起点挪到新时间轴上）；其余（漏调了几块、
            // 表这块没处理）只断开窗口，游标照旧（环形缓冲自己会报「丢过」）。
            if (! haveTime)
            {
                haveTime = true;

                if (meterNow != (std::int64_t) numSamples)
                    beginDiscontinuity (juce::jmax ((std::int64_t) 0, blockStart));   // 表比分析器早开始：之前的子块不属于这次分析
            }
            else if (meterNow != now + (std::int64_t) numSamples)
            {
                if (meterNow < now)
                {
                    subBlockCursor = 0;
                    learnStart = juce::jmax ((std::int64_t) 0, blockStart);
                }

                beginDiscontinuity (juce::jmax ((std::int64_t) 0, blockStart));
            }

            now = meterNow;
            lastBlockSamples = numSamples;

            // House 档：先按宿主拍点把这一块盖进段里（锚点与整数段边界，第「六」节），三路数据再各自找段。
            if (houseActive)
                ledger.beginBlock (blockStart, numSamples, host);

            runOnsets (input, blockStart);

            // 新完成的子块（通常 0 或 1 个；大块时几个）。读者落后超过容量（返回负数）= 中间丢过：窗口断开。
            for (int guard = 0; guard <= LoudnessMeter::subBlockRingCapacity / (int) subBlockScratch.size() + 1; ++guard)
            {
                const auto got = meter.readSubBlocksSince (subBlockCursor, subBlockScratch.data(), (int) subBlockScratch.size());
                const auto count = std::abs (got);

                if (got < 0)
                    clearWindows();

                for (int i = 0; i < count; ++i)
                    consumeSubBlock (subBlockScratch[(size_t) i]);

                if (count < (int) subBlockScratch.size())
                    break;
            }

            // 两个子块长度都没有新子块（表的响度计量关着、或时间轴断了）：短期视图不再代表此刻，标无效。
            if (lastSubBlockEnd < 0 || now - lastSubBlockEnd > 2 * (std::int64_t) subBlockLength)
                clearWindows();

            if (learning && (loudnessDirty || onsetDirty || trialDirty))
            {
                if (loudnessDirty)
                    updateLoudnessProfile();

                if (onsetDirty)
                {
                    updateOnsetProfile();
                    updateSubProfile();   // 修坯 Clay 的真次声占比：与起音同一批组计入，同一个脏标志
                }

                if (trialDirty)
                    updateTrialProfile();   // 先试后用：本块记下了新的有效块

                finalizeConfidences();
            }

            if (houseActive)
                houseAdvance();
        }

        double windowSum (int newestCount) const noexcept
        {
            // 最近 newestCount 个子块的均方之和（从新到旧，顺序固定）。
            double sum = 0.0;

            for (int back = 1; back <= newestCount; ++back)
                sum += window[(size_t) ((windowWrite - back + shortTermSubBlocks) % shortTermSubBlocks)].meanSquare;

            return sum;
        }

        float windowPeak (int newestCount) const noexcept
        {
            float peak = 0.0f;

            for (int back = 1; back <= newestCount; ++back)
                peak = juce::jmax (peak, window[(size_t) ((windowWrite - back + shortTermSubBlocks) % shortTermSubBlocks)].truePeak);

            return peak;
        }

        void consumeSubBlock (const LoudnessMeter::SubBlock& subBlock) noexcept
        {
            const auto length = (std::int64_t) subBlockLength;
            const auto start = subBlock.endSample - length;

            // House 档：子块流读到这里了（丢掉的子块同样算读过：之后不会再有起点早于它的子块）。
            if (houseActive)
                houseSubBlockPosition = juce::jmax (houseSubBlockPosition, subBlock.endSample);

            // 跨越不连续点、或数值坏了的子块：丢掉，窗口从下一个干净的子块重新填。
            if (start < discardSubBlocksBefore
                || ! std::isfinite (subBlock.meanSquare) || subBlock.meanSquare < 0.0
                || ! std::isfinite (subBlock.truePeakLinear))
            {
                clearWindows();
                return;
            }

            // 与上一个子块不相邻（中间有空档：表的响度计量关过）：窗口从这里重新填。
            if (windowCount > 0 && subBlock.endSample - lastSubBlockEnd != length)
                windowCount = 0;

            window[(size_t) windowWrite] = { subBlock.meanSquare, juce::jmax (0.0f, subBlock.truePeakLinear) };
            windowWrite = (windowWrite + 1) % shortTermSubBlocks;
            windowCount = juce::jmin (windowCount + 1, shortTermSubBlocks);
            lastSubBlockEnd = subBlock.endSample;

            // ---- 短期视图：M / S / PSR（EBU 定义，不门限、不平滑）----
            const auto gatingCount = juce::jmin (windowCount, gatingSubBlocks);
            const auto gatingPower = windowSum (gatingCount) / (double) gatingCount;
            const auto gatingLufs = energyToLufs (gatingPower);

            if (windowCount >= momentarySubBlocks)
                setFeature (view.momentaryLufs, toReading (gatingLufs), 0.1 * momentarySubBlocks);
            else
                invalidate (view.momentaryLufs);

            view.momentaryLufs.confidence = view.momentaryLufs.valid ? 1.0f : 0.0f;

            const auto haveShortTerm = windowCount >= shortTermSubBlocks;
            const auto shortTermPower = haveShortTerm ? windowSum (shortTermSubBlocks) / (double) shortTermSubBlocks : 0.0;
            const auto shortTermLufs = energyToLufs (shortTermPower);

            if (haveShortTerm)
                setFeature (view.shortTermLufs, toReading (shortTermLufs), 0.1 * shortTermSubBlocks);
            else
                invalidate (view.shortTermLufs);

            view.shortTermLufs.confidence = view.shortTermLufs.valid ? 1.0f : 0.0f;

            // PSR₃ₛ = 最近 3 s 真峰值最大值 − S（同一时间范围）；S 在绝对门下（近乎静音）时无意义。
            const auto recentPeak = windowPeak (juce::jmin (windowCount, shortTermSubBlocks));

            if (haveShortTerm && shortTermLufs >= absoluteGateLufs && recentPeak > 0.0f)
                setFeature (view.psr3sDb, juce::Decibels::gainToDecibels ((double) recentPeak) - shortTermLufs, 0.1 * shortTermSubBlocks);
            else
                invalidate (view.psr3sDb);

            view.psr3sDb.confidence = view.psr3sDb.valid ? 1.0f : 0.0f;

            // ---- 有效信号门限 ----
            bool haveReference = false;
            double reference = 0.0;

            if (learning && learnIntegratedValid)
            {
                haveReference = true;
                reference = learnIntegratedLufs;
            }
            else if (! learning && profile.integratedLufs.valid)
            {
                haveReference = true;
                reference = profile.integratedLufs.value;
            }

            const auto effective = gatingLufs >= absoluteGateLufs
                                   && (! haveReference || gatingLufs >= reference + effectiveRelativeGateLu);
            view.musicActive = effective;

            // House 档：同一个子块按起点归段（学习与否都记：锁定后防漏网要看新段）。
            if (houseActive)
                noteHouseSubBlock (subBlock, start, effective, gatingPower, gatingLufs, haveShortTerm, shortTermPower, shortTermLufs);

            if (! learning)
                return;

            // ---- 学习区间内的累计（窗口最早的样本不早于 learnStart 才算）----
            const auto subBlockInside = start >= learnStart;

            if (subBlockInside)
            {
                learnPeak = juce::jmax (learnPeak, juce::jmax (0.0f, subBlock.truePeakLinear));
                loudnessDirty = true;
            }

            // I：400 ms 门块（每个子块一个，75 % 重叠），绝对门 −70 LUFS。
            if (windowCount >= gatingSubBlocks && subBlock.endSample - gatingSubBlocks * length >= learnStart
                && gatingLufs >= absoluteGateLufs)
            {
                const auto bin = histogramBinOf (gatingLufs);
                integratedEnergy[(size_t) bin] += gatingPower;
                ++integratedCount[(size_t) bin];
                integratedTotalEnergy += gatingPower;
                ++integratedTotalCount;
                integratedTopBin = juce::jmax (integratedTopBin, bin);
                refreshIntegrated();
                loudnessDirty = true;
            }

            // LRA：S 每 100 ms 取一个（10 Hz），绝对门 −70 LUFS；相对门与百分位在读数时算。
            if (haveShortTerm && subBlock.endSample - shortTermSubBlocks * length >= learnStart
                && shortTermLufs >= absoluteGateLufs)
            {
                const auto bin = histogramBinOf (shortTermLufs);
                ++rangeCount[(size_t) bin];
                rangeTotalEnergy += shortTermPower;
                ++rangeTotalCount;
                rangeTopBin = juce::jmax (rangeTopBin, bin);
                loudnessDirty = true;
            }

            // 有效时长（按新增样本计：每个子块只算一次它自己的 100 ms）与代表性覆盖。
            if (subBlockInside && effective)
            {
                effectiveSamples += length;
                cover (subBlock.endSample);
                loudnessDirty = true;
            }
        }

        // 学习区间的 I（门块直方图）：绝对门以上的平均 − 10 LU 作相对门，再取相对门以上的门块平均（EBU R128）。
        void refreshIntegrated() noexcept
        {
            if (integratedTotalCount == 0)
            {
                learnIntegratedValid = false;
                return;
            }

            const auto relativeGate = energyToLufs (integratedTotalEnergy / (double) integratedTotalCount) + integratedRelativeGateLu;
            double energy = 0.0;
            std::uint64_t count = 0;

            for (int bin = firstBinAtOrAbove (relativeGate); bin <= integratedTopBin; ++bin)
            {
                energy += integratedEnergy[(size_t) bin];
                count += integratedCount[(size_t) bin];
            }

            learnIntegratedValid = count > 0;
            learnIntegratedLufs = count > 0 ? energyToLufs (energy / (double) count) : 0.0;
        }

        void cover (std::int64_t subBlockEnd) noexcept
        {
            // 宿主给了位置且在播放：按子块中点的歌曲位置记一格（100 ms）。同一格再听不算新素材。
            // 位置按「本块起点的宿主位置 + 与本块起点的样本差」外推：跨越跳播 / 回绕的子块已被当作不连续点丢掉了。
            const auto length = (std::int64_t) subBlockLength;
            const auto positioned = host.hasPosition && (! host.hasPlayState || host.isPlaying);

            if (positioned)
            {
                const auto blockStart = now - (std::int64_t) lastBlockSamples;
                const auto position = host.positionSamples + (subBlockEnd - length / 2 - blockStart);

                if (position >= 0)
                {
                    const auto cell = (position / length) % (std::int64_t) coverageCells;
                    auto& word = coverage[(size_t) (cell / 64)];
                    const auto bit = (std::uint64_t) 1 << (cell % 64);

                    if ((word & bit) == 0)
                    {
                        word |= bit;
                        ++coveredCells;
                    }

                    return;
                }
            }

            unpositionedSamples += length;
        }

        //==============================================================================
        // 三、样本流（起音）
        //==============================================================================
        void runOnsets (const juce::AudioBuffer<float>& input, std::int64_t blockStart) noexcept
        {
            const auto numSamples = input.getNumSamples();
            const auto channelsToUse = juce::jmin (numChannels, input.getNumChannels());
            const auto* const* channels = input.getArrayOfReadPointers();

            // 先试后用只在学习期间跑（本块内不变：开始 / 停止学习只发生在块之间）；单声道时 L、R 取同一个声道。
            const auto trialRunning = learning && channelsToUse > 0;
            const auto trialStereo = channelsToUse >= 2;

            // 按组切段：一段 = 到本组结束（或本块结束）为止。段内逐样本做起音功率与真次声；学习中再把候选跑一遍这一段；组满再结组。
            // 每个样本仍是「起音 / 真次声 → 候选 → （组末）结组」的先后，与逐样本处理逐位相同，所以照样与块长无关。
            for (int segmentStart = 0; segmentStart < numSamples;)
            {
                const auto segmentLength = juce::jmin (numSamples - segmentStart, groupLength - groupFill);
                const auto segmentPosition = groupsSinceRestart * (std::int64_t) groupLength + groupFill;   // 从重启起算的样本序号

                // House 档：组网格（也是先试后用的块网格）原点的分析器时间 = 这一段首样本的时间 − 它在网格上的序号（两次重启之间不变）。
                if (houseActive)
                {
                    onsetGridOrigin = blockStart + segmentStart - segmentPosition;
                    houseGridKnown = true;
                }

                for (int i = segmentStart; i < segmentStart + segmentLength; ++i)
                {
                    // 各声道样本平方之和（除以声道数在组均值里做）；逐样本同一表达式，与分块无关。
                    double power = 0.0;

                    for (int ch = 0; ch < channelsToUse; ++ch)
                    {
                        const auto x = (double) channels[ch][i];
                        power += x * x;
                    }

                    groupPower[(size_t) groupFill] = power;
                    ++groupFill;

                    // 修坯 Clay 的真次声：M 过 8 阶 16 Hz 低通，本组的两份能量逐样本累加（同样是逐样本同一表达式，与分块无关）。
                    const auto mid = midSampleOf (channels, channelsToUse, i);
                    auto low = mid;

                    for (size_t stage = 0; stage < subLowpass.size(); ++stage)
                        low = runBiquad (low, subLowpass[stage], subState[stage]);

                    groupSubEnergy += low * low;
                    groupMidEnergy += mid * mid;
                }

                if (trialRunning)
                    runTrial (channels[0] + segmentStart, channels[trialStereo ? 1 : 0] + segmentStart, trialStereo,
                              segmentLength, segmentPosition);

                segmentStart += segmentLength;

                if (groupFill == groupLength)
                {
                    processGroup (sumGroup() / (double) (groupLength * juce::jmax (1, channelsToUse)), blockStart + segmentStart);
                    groupFill = 0;
                    groupSubEnergy = 0.0;
                    groupMidEnergy = 0.0;

                    // 长时间静音里 IIR 状态会一路衰减进非规格化数：每组末尾按同一门槛归零（组网格与块边界无关，结果仍与分块无关）。
                    for (auto& stage : subState)
                    {
                        snapToZero (stage.s1);
                        snapToZero (stage.s2);
                    }

                    if (trialRunning)
                        snapTrialStates();
                }
            }
        }

        // 修坯 Clay 的样本流工具（第「四」「五」节）。
        // RBJ 二阶低通（Audio EQ Cookbook 的 LPF），按 a0 归一。1 − cos w0 写成 2·sin²(w0 / 2)：16 Hz / 192 kHz 时 1 − cos w0 ≈ 1.4e-7，
        // 直接相减会丢掉一半以上的有效数字。四级（Q = 1 / (2·cos((2k − 1)·π/16))）串联 = 双线性变换的 8 阶 Butterworth：
        // |H|² = 1 / (1 + (tan(πf / fs) / tan(πf₀ / fs))¹⁶)（测试据此算解析值）。
        static BiquadCoefficients makeLowpass (double rate, double frequency, double q) noexcept
        {
            const auto w0 = juce::MathConstants<double>::twoPi * frequency / rate;
            const auto halfSine = std::sin (0.5 * w0);
            const auto oneMinusCos = 2.0 * halfSine * halfSine;
            const auto alpha = std::sin (w0) / (2.0 * q);
            const auto a0 = 1.0 + alpha;

            BiquadCoefficients c;
            c.b0 = 0.5 * oneMinusCos / a0;
            c.b1 = oneMinusCos / a0;
            c.b2 = c.b0;
            c.a1 = -2.0 * std::cos (w0) / a0;
            c.a2 = (1.0 - alpha) / a0;
            return c;
        }

        // RBJ 二阶高通：与 ClayModule::designHighpass 同式（频率钳在 0.45 fs 以下）。先试后用的低切候选用它，两级 Q 同 ClayModule。
        static BiquadCoefficients makeHighpass (double rate, double frequency, double q) noexcept
        {
            const auto w0 = juce::MathConstants<double>::twoPi * juce::jmin (frequency, 0.45 * rate) / rate;
            const auto cosW0 = std::cos (w0);
            const auto alpha = std::sin (w0) / (2.0 * q);
            const auto a0 = 1.0 + alpha;

            BiquadCoefficients c;
            c.b0 = (1.0 + cosW0) * 0.5 / a0;
            c.b1 = -(1.0 + cosW0) / a0;
            c.b2 = c.b0;
            c.a1 = -2.0 * cosW0 / a0;
            c.a2 = (1.0 - alpha) / a0;
            return c;
        }

        // RBJ 高架（斜率 S = 1）：与 ClayModule::designWidthShelf 同式——A = 10^(dB/40)，平台增益 A²，α = sin(w0)/2 · √2
        // （S = 1 时根号里恒为 2）；频率钳在 0.45 fs 以下。先试后用的加宽候选用它。
        static BiquadCoefficients makeHighShelf (double rate, double frequency, double gainDb) noexcept
        {
            const auto w0 = juce::MathConstants<double>::twoPi * juce::jmin (frequency, 0.45 * rate) / rate;
            const auto cosW0 = std::cos (w0);
            const auto alpha = std::sin (w0) / 2.0 * std::sqrt (2.0);
            const auto a = std::pow (10.0, gainDb / 40.0);
            const auto aPlus1 = a + 1.0;
            const auto aMinus1 = a - 1.0;
            const auto twoSqrtAAlpha = 2.0 * std::sqrt (a) * alpha;

            const auto b0 =        a * (aPlus1 + aMinus1 * cosW0 + twoSqrtAAlpha);
            const auto b1 = -2.0 * a * (aMinus1 + aPlus1 * cosW0);
            const auto b2 =        a * (aPlus1 + aMinus1 * cosW0 - twoSqrtAAlpha);
            const auto a0 =            (aPlus1 - aMinus1 * cosW0 + twoSqrtAAlpha);
            const auto a1 =      2.0 * (aMinus1 - aPlus1 * cosW0);
            const auto a2 =            (aPlus1 - aMinus1 * cosW0 - twoSqrtAAlpha);

            BiquadCoefficients c;
            c.b0 = b0 / a0;
            c.b1 = b1 / a0;
            c.b2 = b2 / a0;
            c.a1 = a1 / a0;
            c.a2 = a2 / a0;
            return c;
        }

        // 转置直接 II 型双二阶（double）。
        static double runBiquad (double x, const BiquadCoefficients& c, BiquadState& s) noexcept
        {
            const auto y = c.b0 * x + s.s1;
            s.s1 = c.b1 * x - c.a1 * y + s.s2;
            s.s2 = c.b2 * x - c.a2 * y;
            return y;
        }

        // M = (L + R) / 2。单声道 M = 该声道：与「单声道按左右相同的双声道」一致（(x + x) / 2 与 x 逐位相同）。
        // 多于两个声道只取前两个（L / R，与 Balance 交叉功率取的是同一对）。
        static double midSampleOf (const float* const* samples, int channelsToUse, int index) noexcept
        {
            if (channelsToUse >= 2)
                return 0.5 * ((double) samples[0][index] + (double) samples[1][index]);

            return channelsToUse == 1 ? (double) samples[0][index] : 0.0;
        }

        double sumGroup() const noexcept
        {
            // 四路交错累加（组内下标 mod 4，组总从重启起算的固定网格上开始 → 与块边界无关），缩短加法依赖链。
            double lanes[4] = { 0.0, 0.0, 0.0, 0.0 };
            int k = 0;

            for (; k + 4 <= groupLength; k += 4)
            {
                lanes[0] += groupPower[(size_t) k];
                lanes[1] += groupPower[(size_t) k + 1];
                lanes[2] += groupPower[(size_t) k + 2];
                lanes[3] += groupPower[(size_t) k + 3];
            }

            for (; k < groupLength; ++k)
                lanes[0] += groupPower[(size_t) k];

            return (lanes[0] + lanes[1]) + (lanes[2] + lanes[3]);
        }

        void processGroup (double meanPower, std::int64_t groupEnd) noexcept
        {
            if (! std::isfinite (meanPower))
            {
                restartOnsets();
                return;
            }

            // 一阶平滑，重启后按运行均值起步（第 n 组的保留系数取 min(常数, (n − 1) / n)）：不必等一个时间常数才可用。
            ++groupsSinceRestart;
            const auto n = (double) groupsSinceRestart;
            const auto runningRetain = (n - 1.0) / n;
            const auto fr = juce::jmin (fastRetain, runningRetain);
            const auto sr = juce::jmin (slowRetain, runningRetain);
            fast = meanPower + (fast - meanPower) * fr;
            slow = meanPower + (slow - meanPower) * sr;
            snapToZero (fast);
            snapToZero (slow);

            if (groupsSinceRestart <= warmUpGroups)
            {
                noteTrialGroup (false);   // 重启预热里的组：先试后用的块同样不算
                return;
            }

            const auto inside = learning && groupEnd - (std::int64_t) groupLength >= learnStart;

            // 有效时长：慢包络在绝对门之上，且不低于学习区间平均功率 20 dB 以上。
            const auto relativeGate = onsetPowerGroups > 0 ? onsetEffectiveRelativeRatio * onsetPowerSum / (double) onsetPowerGroups : 0.0;
            const auto effective = slow >= onsetSlowFloorPower && slow >= relativeGate;

            if (inside && slow >= onsetSlowFloorPower)
            {
                onsetPowerSum += meanPower;
                ++onsetPowerGroups;
            }

            if (inside && effective)
            {
                onsetEffectiveSamples += groupLength;
                onsetDirty = true;

                // 修坯 Clay 的真次声占比：同一组、同一套边界（学习区间按组样本精确、有效音频门同上）整组计入。
                subEnergy += groupSubEnergy;
                midEnergy += groupMidEnergy;
            }

            // House 档：同一组按组起点归段（有效门同上；学习与否都记）。
            if (houseActive && effective)
                noteHouseGroup (groupEnd - (std::int64_t) groupLength);

            // 先试后用：同一套边界决定这一组算不算（块里每个组都算，块才算）。
            noteTrialGroup (inside && effective);

            const auto denominator = juce::jmax (slow, onsetSlowFloorPower);

            switch (onsetState)
            {
                case OnsetState::armed:
                    if (fast >= triggerRatio * denominator && fast >= onsetTriggerFloorPower)
                    {
                        onsetState = OnsetState::inEvent;
                        refractoryLeft = refractoryGroups;
                        eventPeakRatio = fast / denominator;
                        eventCounts = inside && effective;

                        // House 档：事件归触发的那一组所在的段（在有效门上才计；事件结束前那一段不定稿）。
                        if (houseActive)
                        {
                            houseEventPending = effective;
                            houseEventTime = groupEnd - (std::int64_t) groupLength;
                        }
                    }
                    break;

                case OnsetState::inEvent:
                    eventPeakRatio = juce::jmax (eventPeakRatio, fast / denominator);

                    if (--refractoryLeft <= 0)
                    {
                        finishEvent();
                        onsetState = fast < rearmRatio * denominator ? OnsetState::armed : OnsetState::waitingRearm;
                    }
                    break;

                case OnsetState::waitingRearm:
                    if (fast < rearmRatio * denominator)
                        onsetState = OnsetState::armed;
                    break;
            }
        }

        void finishEvent() noexcept
        {
            if (houseEventPending)
                noteHouseEvent();

            if (! (eventCounts && learning))
                return;

            const auto strengthDb = juce::jlimit (0.0, onsetStrengthCapDb, 10.0 * std::log10 (eventPeakRatio));
            const auto bin = juce::jlimit (0, strengthBins - 1, (int) std::floor (strengthDb / strengthBinDb));
            ++strengthCount[(size_t) bin];
            ++onsetEvents;
            onsetDirty = true;
        }

        //==============================================================================
        // 五、先试后用（样本流，学习期间；第「五」节）
        //==============================================================================
        // 一段 = 同一个组里连续的 length 个样本（≤ 组长）。left / right = 原始 L / R（单声道时两者是同一个声道）；
        // position = 段首从重启起算的样本序号（候选需要重新起跑时用来对齐块网格）。
        void runTrial (const float* left, const float* right, bool stereo, int length, std::int64_t position) noexcept
        {
            if (trialRestartPending)
                restartTrial (position);

            // 坏样本（NaN / Inf）会把候选的滤波器状态弄坏：只跑它之前的样本，候选从下一段起从零重来。那一组的起音功率也非有限，
            // 组末整体重启（restartOnsets），跨越它的块本来就作废。
            auto count = 0;

            while (count < length && std::isfinite (left[count]) && std::isfinite (right[count]))
                ++count;

            if (count < length)
                trialRestartPending = true;

            if (count == 0)
                return;

            // 1. M / S（单声道时 right 就是 left：M = 该声道、S = 0，与 midSampleOf 同一口径）。
            for (int k = 0; k < count; ++k)
            {
                const auto l = (double) left[k];
                const auto r = (double) right[k];
                trialMidScratch[(size_t) k] = 0.5 * (l + r);
                trialSideScratch[(size_t) k] = 0.5 * (l - r);
            }

            // 2. 收单候选的线性相位低通。函数调用单独成环：调用会冲掉寄存器，第 3 步的 IIR 状态这样才能整段留在寄存器里。
            for (size_t c = 0; c < trialMonoLowpass.size(); ++c)
            {
                auto& lowpass = trialMonoLowpass[c];
                auto& lowSide = trialLowScratch[c];

                for (int k = 0; k < count; ++k)
                    lowSide[(size_t) k] = lowpass.process (trialSideScratch[(size_t) k]);
            }

            // 3. 逐样本：原始的块峰值累计；六个候选各自此刻的输出峰值先写暂存（第 4 步按各自的块边界累计）。
            const auto cutFirst = trialCut[0];
            const auto cutSecond = trialCut[1];
            const auto shelf = trialShelf;
            auto leftFirst = trialCutState[0][0];
            auto leftSecond = trialCutState[0][1];
            auto rightFirst = trialCutState[1][0];
            auto rightSecond = trialCutState[1][1];
            auto shelfState = trialShelfState;
            auto originalMax = trialOriginalMax;
            auto write = trialHistoryWrite;
            const auto mask = trialHistoryMask;
            auto* midHistory = trialMidHistory.data();
            auto* sideHistory = trialSideHistory.data();

            for (int k = 0; k < count; ++k)
            {
                const auto i = (size_t) k;
                const auto l = (double) left[k];
                const auto mid = trialMidScratch[i];
                const auto side = trialSideScratch[i];

                // 原始：max(|L|, |R|)。0 低切：L、R 各两级高通，max(|L'|, |R'|)（单声道只跑一路：两路逐位相同）。
                auto inputPeak = std::abs (l);
                auto cutPeak = std::abs (runBiquad (runBiquad (l, cutFirst, leftFirst), cutSecond, leftSecond));

                if (stereo)
                {
                    const auto r = (double) right[k];
                    inputPeak = juce::jmax (inputPeak, std::abs (r));
                    cutPeak = juce::jmax (cutPeak, std::abs (runBiquad (runBiquad (r, cutFirst, rightFirst), cutSecond, rightSecond)));
                }

                originalMax = juce::jmax (originalMax, inputPeak);
                trialPeakScratch[(size_t) trialLowCut][i] = cutPeak;

                // 5 加宽：S'' = 高架(S)，峰值 |M| + |S''|。
                trialPeakScratch[(size_t) trialWidth][i] = std::abs (mid) + std::abs (runBiquad (side, shelf, shelfState));

                // 1 ～ 4 收单：S' = S 延迟 D_c − LP(S)、M 延迟 D_c，峰值 |M_d| + |S'|。先写延迟环再读（D_c ≥ 2，读不到刚写的这一格）。
                midHistory[write] = mid;
                sideHistory[write] = side;

                for (size_t c = 0; c < (size_t) numMonoTrials; ++c)
                {
                    const auto delayed = (write + mask + 1 - trialDelay[(size_t) trialFirstMono + c]) & mask;
                    trialPeakScratch[(size_t) trialFirstMono + c][i] = std::abs (midHistory[delayed])
                                                                       + std::abs (sideHistory[delayed] - trialLowScratch[c][i]);
                }

                write = (write + 1) & mask;
            }

            trialCutState[0][0] = leftFirst;
            trialCutState[0][1] = leftSecond;
            trialCutState[1][0] = rightFirst;
            trialCutState[1][1] = rightSecond;
            trialShelfState = shelfState;
            trialOriginalMax = originalMax;
            trialHistoryWrite = write;
            trialRunSamples = juce::jmin (trialRunSamples + (std::int64_t) count, trialWarmUpSamples);

            // 4. 各候选按自己（延迟补偿后）的块边界累计块峰值；交齐一块就写进这一块的记录。
            for (size_t c = 0; c < (size_t) numTrials; ++c)
            {
                const auto& peaks = trialPeakScratch[c];
                auto blockMax = trialCandidateMax[c];
                auto remaining = trialCandidateLeft[c];

                for (int k = 0; k < count;)
                {
                    const auto take = juce::jmin (count - k, remaining);

                    for (int e = k; e < k + take; ++e)
                        blockMax = juce::jmax (blockMax, peaks[(size_t) e]);

                    k += take;
                    remaining -= take;

                    if (remaining == 0)
                    {
                        auto& record = trialRecordFor (trialCandidateBlock[c]);
                        record.candidates[c] = blockMax;
                        countTrialWrite (record);
                        ++trialCandidateBlock[c];
                        blockMax = 0.0;
                        remaining = trialBlockLength;
                    }
                }

                trialCandidateMax[c] = blockMax;
                trialCandidateLeft[c] = remaining;
            }
        }

        // 候选的 IIR 状态在长时间静音里同样会衰减进非规格化数：每组末尾归零（与真次声低通同一门槛、同一网格）。
        void snapTrialStates() noexcept
        {
            for (auto& channelStates : trialCutState)
            {
                for (auto& stageState : channelStates)
                {
                    snapToZero (stageState.s1);
                    snapToZero (stageState.s2);
                }
            }

            snapToZero (trialShelfState.s1);
            snapToZero (trialShelfState.s2);
        }

        // 原始一侧（processGroup 每组调一次）：本组按起音那套边界算不算；块的最后一组结组时交出这一块。
        // 候选没在跑（不在学习、或正等着重新起跑）时不记。
        void noteTrialGroup (bool counts) noexcept
        {
            if (! learning || trialRestartPending)
                return;

            trialBlockValid = trialBlockValid && counts;

            if (groupsSinceRestart % trialBlockGroups != 0)
                return;

            auto& record = trialRecordFor (groupsSinceRestart / trialBlockGroups - 1);
            record.original = trialOriginalMax;
            record.valid = trialBlockWarm && trialBlockValid;
            countTrialWrite (record);

            // 下一块：开始时候选已跑满预热才算。
            trialOriginalMax = 0.0;
            trialBlockValid = true;
            trialBlockWarm = trialRunSamples >= trialWarmUpSamples;
        }

        TrialRecord& trialRecordFor (std::int64_t block) noexcept
        {
            auto& record = trialRecords[(size_t) (block & 1)];

            if (record.block != block)
            {
                record = TrialRecord {};
                record.block = block;
            }

            return record;
        }

        // 原始与 6 个候选都写过这一块才算交齐：有效的记下（封顶 trialCapacity，满了之后不再记），然后腾出槽位。
        void countTrialWrite (TrialRecord& record) noexcept
        {
            if (++record.writes < numTrials + 1)
                return;

            // House 档：交齐的有效块按块起点归段（每段每一路留最大的 8 个，第「六」节）。
            if (houseActive && record.valid)
                noteHouseTrial (record);

            if (record.valid && trialCount < trialCapacity)
            {
                insertTop (trialTop[0], trialCount, record.original);

                for (size_t c = 0; c < (size_t) numTrials; ++c)
                    insertTop (trialTop[c + 1], trialCount, record.candidates[c]);

                ++trialCount;
                trialDirty = true;
            }

            record = TrialRecord {};
        }

        // top[0 .. filled) 降序、最多留 trialTopCount 个：插入 value，挤掉最小的。
        static void insertTop (std::array<double, (size_t) trialTopCount>& top, int filled, double value) noexcept
        {
            auto slot = juce::jmin (filled, trialTopCount - 1);

            if (filled >= trialTopCount && ! (value > top[(size_t) slot]))
                return;

            for (; slot > 0 && top[(size_t) slot - 1] < value; --slot)
                top[(size_t) slot] = top[(size_t) slot - 1];

            top[(size_t) slot] = value;
        }

        //==============================================================================
        // 二、帧流
        //==============================================================================
        void processSpectrum (const BalanceModule& balance) noexcept
        {
            const auto balanceNow = balance.getProcessedSamples();
            const auto inputAdvance = now - lastSpectrumInputSamples;
            // 本轮频谱对应两次交帧之间的整个输入区间，不是最后一个统计切片。
            // 输入时间轴回退时 processInput 已确定新时间轴上的断点，不能再用旧时间戳或末片起点覆盖它。
            const auto intervalStart = inputAdvance < 0 ? discardFramesBefore
                                                       : juce::jmax (lastSpectrumInputSamples, discardFramesBefore);

            // Balance 的累计样本数与表对不上：Balance 单独被 reset 过（链的 restartWithFadeIn 只清模块不清表）或漏调：
            // 帧流在本块起点断开；Balance 被 reset 时游标归零。帧的时间戳按「表 − Balance」的差换算到表的时间轴。
            // 输入可被统计层分片，而频谱每宿主块只读一次；比较两次读帧间的输入总推进量，不是最后一片长度。
            if (! haveBalanceTime)
            {
                haveBalanceTime = true;

                if (balanceNow != now)
                    frameDiscontinuity (intervalStart);
            }
            else if (balanceNow < lastBalanceSamples || inputAdvance < 0
                     || balanceNow != lastBalanceSamples + inputAdvance)
            {
                if (balanceNow < lastBalanceSamples)
                    frameCursor = 0;

                frameDiscontinuity (intervalStart);
            }

            lastBalanceSamples = balanceNow;
            lastSpectrumInputSamples = now;
            const auto offset = now - balanceNow;

            const auto hopNow = balance.getAnalysisHopSamples();
            const auto windowNow = balance.getAnalysisWindowSamples();

            if (hopNow <= 0 || windowNow <= 0)
                return;   // Balance 还没 prepare

            if (hopNow != hop || windowNow != frameWindow)
            {
                updateFrameConstants (hopNow, windowNow);
                frameDiscontinuity (intervalStart);
            }

            for (int guard = 0; guard <= BalanceModule::frameRingCapacity / (int) frameScratch.size() + 1; ++guard)
            {
                const auto got = balance.readFramesSince (frameCursor, frameScratch.data(), (int) frameScratch.size());
                const auto count = std::abs (got);

                if (got < 0)
                    restartSpectrum();

                for (int i = 0; i < count; ++i)
                    consumeFrame (frameScratch[(size_t) i], frameScratch[(size_t) i].endSample + offset);

                if (count < (int) frameScratch.size())
                    break;
            }

            if (learning && spectrumDirty)
            {
                updateSpectrumProfile();
                finalizeConfidences();
            }

            if (houseActive)
                houseAdvance();
        }

        void frameDiscontinuity (std::int64_t at) noexcept
        {
            discardFramesBefore = at;
            restartSpectrum();

            // House 档：分析窗早于 at 的帧不会再来了，帧流就算读到了 at。
            if (houseActive)
                houseFramePosition = juce::jmax (houseFramePosition, at);
        }

        void consumeFrame (const BalanceModule::FrameSnapshot& frame, std::int64_t end) noexcept
        {
            const auto windowStart = end - (std::int64_t) frameWindow;

            if (windowStart < discardFramesBefore)
            {
                restartSpectrum();   // 分析窗跨越不连续点
                return;
            }

            if (havePreviousFrame && end - previousFrameEnd != (std::int64_t) hop)
                restartSpectrum();   // 丢过帧：时间平均与谱通量都从这一帧重新开始

            // House 档：帧流读到这里了（之后的帧分析窗起点都不早于它）。
            if (houseActive)
                houseFramePosition = juce::jmax (houseFramePosition, windowStart + (std::int64_t) hop);

            std::array<double, (size_t) numBands> power {}, cross {};
            double total = 0.0;

            for (size_t b = 0; b < (size_t) numBands; ++b)
            {
                power[b] = juce::jmax (0.0, (double) frame.bandPower[b]);
                total += power[b];
            }

            if (! std::isfinite (total))
            {
                restartSpectrum();
                return;
            }

            // 修坯 Clay：同一帧、同一段边界的左右交叉功率（FrameSnapshot::bandCross）。物理上 |X| ≤ P（逐 bin 有
            // |Re(L·R*)| ≤ (|L|² + |R|²) / 2，非负权重求和后照样成立），按它再钳一次：Balance 已钳过，这里只防读者一侧的舍入，
            // 也保证 P_M、P_S 都不为负。非有限值按 0（Balance 遇到非有限输入时本段功率也已记 0，钳到 [−0, 0] 同样是 0）。
            for (size_t b = 0; b < (size_t) numBands; ++b)
            {
                const auto measuredCross = (double) frame.bandCross[b];
                cross[b] = std::isfinite (measuredCross) ? juce::jlimit (-power[b], power[b], measuredCross) : 0.0;
            }

            // 各段功率域一阶平均（运行均值起步）。
            ++framesSinceRestart;
            const auto n = (double) framesSinceRestart;
            const auto retain = juce::jmin (spectrumRetain, (n - 1.0) / n);

            for (size_t b = 0; b < (size_t) numBands; ++b)
            {
                averagedPower[b] = power[b] + (averagedPower[b] - power[b]) * retain;
                snapToZero (averagedPower[b]);
            }

            // 帧门限：总功率 ≥ −70 dBFS，且学习中不低于学习区间帧均值 20 dB 以上。
            const auto inside = learning && windowStart >= learnStart;
            const auto relativeOk = ! inside || learnFrames == 0
                                    || total >= frameRelativeGateRatio * learnTotalPowerSum / (double) learnFrames;
            const auto music = total >= frameAbsoluteGatePower && relativeOk;

            // House 档：这一帧按分析窗起点归段。帧门同上，只是相对门不看「在不在学习区间里」——锁定后停止学习，按学习区间冻结的帧均值
            // 接着用（学习中、窗在区间里时与 music 逐项相同；所以 houseMusic 为真时 music 也为真，频谱证据照常算到）。
            SegmentLedger::Segment* houseSegment = nullptr;
            auto houseMusic = false;

            if (houseActive)
            {
                houseSegment = ledger.openSegmentAt (windowStart, houseFrameHint);
                houseMusic = total >= frameAbsoluteGatePower
                          && (learnFrames == 0 || total >= frameRelativeGateRatio * learnTotalPowerSum / (double) learnFrames);
            }

            // 修坯 Clay 的实时立体声视图：帧门限之上才更新（学习与否都更新）。门限下（静音）两路低频相关度标无效、平滑状态保留，
            // 回到门上接着平滑（ClayGuard 见到慢路无效就冻结反相一路：与这里的帧门同一口径，审核 V1-03）；高频两项同样标无效、值保留
            // （宽度一路随之冻结；回到门上由 updateLiveStereo 按「已平滑满且门上有内容」重新置有效）。
            if (music)
            {
                updateLiveStereo (power, cross);
            }
            else
            {
                invalidate (view.liveLowCorrelation);
                invalidate (view.liveLowCorrelationFast);
                view.liveHighValid = false;
            }

            // 辅助：250 Hz 以上段的正向谱通量（与上一帧相邻时才有；静音帧也算）。
            bool haveFlux = false, fluxEvent = false;
            double flux = 0.0;

            if (havePreviousFrame)
            {
                double rise = 0.0, referenceTotal = 0.0;

                for (size_t b = (size_t) fluxFirstBand; b < (size_t) numBands; ++b)
                {
                    auto reference = 0.0;

                    for (int k = 0; k < recentFrameCount; ++k)
                        reference = juce::jmax (reference, recentFramePower[(size_t) k][b]);

                    rise += juce::jmax (0.0, power[b] - reference);
                    referenceTotal += reference;
                }

                const auto floorTotal = fluxFloorPowerPerBand * (double) (numBands - fluxFirstBand);
                flux = 10.0 * std::log10 (1.0 + rise / juce::jmax (referenceTotal, floorTotal));

                haveFlux = true;
                ++fluxFrames;

                const auto warmedUp = fluxFrames > fluxWarmUpFrames;
                const auto threshold = juce::jmax (fluxMinimumDb, fluxThresholdRatio * fluxMean);
                fluxEvent = warmedUp && framesSinceFluxEvent >= fluxRefractoryFrames && flux > threshold;
                framesSinceFluxEvent = fluxEvent ? 0 : juce::jmin (framesSinceFluxEvent + 1, (std::int64_t) 1 << 30);

                // 滑动均值（指数加权、运行均值起步）。突起本身按门限截断后再进均值，免得几次大突起把门限抬走。
                const auto x = warmedUp ? juce::jmin (flux, threshold) : flux;
                const auto m = (double) fluxFrames;
                const auto r = juce::jmin (fluxMeanRetain, (m - 1.0) / m);
                fluxMean = x + (fluxMean - x) * r;
                snapToZero (fluxMean);
            }

            // 突起密度按学习区间内的全部帧计时（含静音帧），与包络事件同一段区间比较。
            if (inside && haveFlux)
            {
                ++learnFluxFrames;

                if (fluxEvent)
                    ++learnFluxEvents;

                spectrumDirty = true;
            }

            if (houseSegment != nullptr && haveFlux)   // House 档：同上，按段（含静音帧）
            {
                ++houseSegment->fluxFrames;

                if (fluxEvent)
                    ++houseSegment->fluxEvents;
            }

            if (inside && music)
            {
                ++learnFrames;
                learnTotalPowerSum += total;

                double lowPower = 0.0;

                for (size_t b = 0; b <= (size_t) lowShareLastBand; ++b)
                    lowPower += power[b];

                const auto lowDb = 10.0 * std::log10 (juce::jmax (lowPower, 1.0e-30));
                ++lowRangeCount[(size_t) juce::jlimit (0, lowRangeBins - 1, (int) std::floor ((lowDb - lowRangeMinDb) / lowRangeBinDb))];

                for (size_t b = 0; b < (size_t) numBands; ++b)
                    powerSum[b] += power[b];

                if (haveFlux)
                {
                    learnFluxSum += flux;
                    ++learnMusicFluxFrames;
                }

                spectrumDirty = true;
            }

            if (houseSegment != nullptr && houseMusic)   // House 档：同上，按段
                noteHouseMusicFrame (*houseSegment, power, haveFlux, flux);

            // 长期画像按原来的学习边界累计；短期突出证据与学习状态无关，实时播放和离线渲染走同一路径。
            // 共用一轮稳健轮廓计算，不增加 FFT，也不重新启动画像学习。House 档的段在同一轮里顺带累计（第「六」节）。
            if (music && framesSinceRestart >= settleFrames)
                accumulateEvidence (inside, power, cross, houseMusic ? houseSegment : nullptr);
            else
            {
                livePositiveSeconds.fill (0.0);
                updateLiveTame (0.0);
                invalidate (view.liveSpectralSlopeDbPerOct);
            }

            recentFramePower[(size_t) recentFrameWrite] = power;
            recentFrameWrite = (recentFrameWrite + 1) % fluxReferenceFrames;
            recentFrameCount = juce::jmin (recentFrameCount + 1, fluxReferenceFrames);
            previousFrameEnd = end;
            havePreviousFrame = true;
        }

        // 本段处的平滑轮廓：两侧 ±3 段（不含本段）按 kernel × weights 加权做直线拟合，取本段处的截距。
        // 只有一侧一个位置（拟合不出斜率）时退回加权平均；一个邻段都没有返回 false（本段不评估）。
        static bool contourAt (int band, const std::array<double, (size_t) numBands>& level,
                               const std::array<double, (size_t) numBands>& weights, double& contour) noexcept
        {
            double sw = 0.0, swx = 0.0, swxx = 0.0, swy = 0.0, swxy = 0.0;

            for (int distance = 1; distance <= contourRadius; ++distance)
            {
                for (int side = -1; side <= 1; side += 2)
                {
                    const auto neighbour = band + side * distance;

                    if (neighbour < 0 || neighbour >= numBands)
                        continue;

                    const auto w = contourKernel[distance] * weights[(size_t) neighbour];

                    if (w <= 0.0)
                        continue;

                    const auto x = (double) (side * distance);
                    const auto y = level[(size_t) neighbour];
                    sw += w;
                    swx += w * x;
                    swxx += w * x * x;
                    swy += w * y;
                    swxy += w * x * y;
                }
            }

            if (sw <= 0.0)
                return false;

            const auto determinant = sw * swxx - swx * swx;
            contour = determinant > 1.0e-9 * sw * sw ? (swy * swxx - swx * swxy) / determinant
                                                     : swy / sw;
            return true;
        }

        void updateLiveTame (double target) noexcept
        {
            const auto retain = target > liveTameEvidence ? liveTameAttackRetain : liveTameReleaseRetain;
            liveTameEvidence = target + (liveTameEvidence - target) * retain;
            snapToZero (liveTameEvidence);

            if (framesSinceRestart >= settleFrames)
            {
                setFeature (view.liveTameEvidenceDb, liveTameEvidence, (double) framesSinceRestart * hop / sampleRate);
                view.liveTameEvidenceDb.confidence = 1.0f;
            }
        }

        // house = House 档这一帧所在的段（帧门之上才给，第「六」节）：频谱证据与立体声在同一轮里按同一口径另加进段里；通用档恒为空。
        void accumulateEvidence (bool collectLearning, const std::array<double, (size_t) numBands>& currentPower,
                                 const std::array<double, (size_t) numBands>& currentCross, SegmentLedger::Segment* house = nullptr) noexcept
        {
            std::array<double, (size_t) numBands> level {}, gate {}, weights {}, firstResidual {};
            std::array<bool, (size_t) numBands> evaluated {};

            for (size_t b = 0; b < (size_t) numBands; ++b)
            {
                level[b] = 10.0 * std::log10 (juce::jmax (1.0e-30, averagedPower[b]));
                gate[b] = juce::jlimit (0.0, 1.0, (level[b] - bandGateDb) / bandGateFadeDb);
            }

            // 修坯 Clay 的立体声画像（1.3）：与频谱证据同一批帧（学习区间内、帧门限之上、1 s 平均已稳定）、同一逐段静音门权重。
            if (collectLearning)
                accumulateStereo (currentPower, currentCross, gate);

            if (house != nullptr)
                accumulateHouseStereo (*house, currentPower, currentCross, gate);

            // 第一遍：邻段只按静音门加权。
            for (int b = 0; b < numBands; ++b)
            {
                double contour = 0.0;
                const auto i = (size_t) b;
                evaluated[i] = gate[i] > 0.0 && contourAt (b, level, gate, contour);
                firstResidual[i] = evaluated[i] ? level[i] - contour : 0.0;
            }

            // Huber 降权：第一遍残差超出容忍区间的段，作为邻段时的权重按 容忍 / |r| 缩小。
            for (size_t b = 0; b < (size_t) numBands; ++b)
            {
                const auto magnitude = std::abs (firstResidual[b]);
                weights[b] = gate[b] * (magnitude > toleranceDb ? toleranceDb / magnitude : 1.0);
            }

            // 用同一份电平与稳健权重估计宽带斜率。至少 16 段、跨 4 oct，且当下仍有能量；
            // 独奏窄音、已结束的镲片和静音尾巴不能被当作整首歌的明暗。只做 27 段回归，无新 FFT。
            double sw = 0.0, sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
            int slopeBands = 0, firstSlopeBand = numBands, lastSlopeBand = 0;
            for (int b = 0; b < numBands; ++b)
            {
                const auto i = (size_t) b;
                view.liveSlopeWeights[i] = 0.0f;
                if (gate[i] < 0.5 || currentPower[i] < averagedPower[i] * liveTamePresentRatio)
                    continue;
                const auto w = weights[i];
                view.liveSlopeWeights[i] = (float) w;
                const auto x = (double) b / 3.0;
                sw += w; sx += w * x; sy += w * level[i]; sxx += w * x * x; sxy += w * x * level[i];
                ++slopeBands;
                firstSlopeBand = juce::jmin (firstSlopeBand, b);
                lastSlopeBand = b;
            }
            const auto denominator = sw * sxx - sx * sx;
            if (slopeBands >= 16 && lastSlopeBand - firstSlopeBand >= 12 && sw >= 12.0 && denominator > 1.0e-9)
            {
                setFeature (view.liveSpectralSlopeDbPerOct, (sw * sxy - sx * sy) / denominator,
                            (double) framesSinceRestart * hop / sampleRate);
                view.liveSpectralSlopeDbPerOct.confidence = (float) juce::jlimit (0.0, 1.0, sw / 20.0);
            }
            else
                invalidate (view.liveSpectralSlopeDbPerOct);

            // 第二遍：残差 → 超出容忍区间的正 / 负偏离分别累计（按本段静音门权重）。
            double liveSumSquares = 0.0, liveWeights = 0.0;
            const auto hopSeconds = (double) hop / sampleRate;
            for (int b = 0; b < numBands; ++b)
            {
                const auto i = (size_t) b;
                double contour = 0.0;

                if (! evaluated[i] || ! contourAt (b, level, weights, contour))
                {
                    livePositiveSeconds[i] = 0.0;
                    continue;
                }

                const auto residual = level[i] - contour;
                const auto positive = juce::jmax (0.0, residual - toleranceDb);
                if (collectLearning)
                {
                    tameSum[i] += gate[i] * positive;
                    recoverSum[i] += gate[i] * juce::jmax (0.0, -residual - toleranceDb);

                    if (residual < -toleranceDb)
                        deficitWeight[i] += gate[i];

                    validWeight[i] += gate[i];
                }

                if (house != nullptr)   // House 档：同一份残差、同一权重，加进段里
                {
                    house->tameSum[i] += gate[i] * positive;
                    house->recoverSum[i] += gate[i] * juce::jmax (0.0, -residual - toleranceDb);

                    if (residual < -toleranceDb)
                        house->deficitWeight[i] += gate[i];

                    house->validWeight[i] += gate[i];
                }

                if (positive > 0.0 && currentPower[i] >= averagedPower[i] * liveTamePresentRatio)
                    livePositiveSeconds[i] = juce::jmin (liveTamePersistenceSeconds, livePositiveSeconds[i] + hopSeconds);
                else
                    livePositiveSeconds[i] = 0.0;

                if (livePositiveSeconds[i] >= liveTamePersistenceSeconds)
                    liveSumSquares += gate[i] * positive * positive;
                liveWeights += gate[i];
            }
            updateLiveTame (liveWeights > 0.0 ? std::sqrt (liveSumSquares / liveWeights) : 0.0);
        }

        // 修坯 Clay 的立体声画像累计（1.3）：每段按静音门权重累计 ΣgP、ΣgX 与观测 Σg；两组的观测取每帧组内最大的门权重
        // （组里只要有一段有内容，这一帧就算观测到了这一组）。
        void accumulateStereo (const std::array<double, (size_t) numBands>& currentPower,
                               const std::array<double, (size_t) numBands>& currentCross,
                               const std::array<double, (size_t) numBands>& gate) noexcept
        {
            double lowGate = 0.0, highGate = 0.0;

            for (int b = 0; b < numBands; ++b)
            {
                const auto i = (size_t) b;
                stereoPowerSum[i] += gate[i] * currentPower[i];
                stereoCrossSum[i] += gate[i] * currentCross[i];
                stereoWeight[i] += gate[i];

                if (b >= lowStereoFirstBand && b <= lowStereoLastBand)
                    lowGate = juce::jmax (lowGate, gate[i]);

                if (b >= highStereoFirstBand && b <= highStereoLastBand)
                    highGate = juce::jmax (highGate, gate[i]);
            }

            lowStereoWeight += lowGate;
            highStereoWeight += highGate;
        }

        // 修坯 Clay 的实时立体声视图（1.4 / 根治修订 3.1）：只在帧门限之上的帧调（静音帧见 consumeFrame）。
        void updateLiveStereo (const std::array<double, (size_t) numBands>& power,
                               const std::array<double, (size_t) numBands>& cross) noexcept
        {
            // 本帧两组的和：只计段功率在 −60 dBFS 门上的段（门下的段不计——例如门下不相关的底噪不能把低频相关度拉向 0）。
            // 整组都在门下时本帧的和为 0，平滑值按系数衰减、比值不变。
            double lowCrossNow = 0.0, lowPowerNow = 0.0, highMidNow = 0.0, highSideNow = 0.0;

            for (int b = lowStereoFirstBand; b <= lowStereoLastBand; ++b)
            {
                const auto i = (size_t) b;

                if (power[i] >= liveBandGatePower)
                {
                    lowCrossNow += cross[i];
                    lowPowerNow += power[i];
                }
            }

            for (int b = highStereoFirstBand; b <= highStereoLastBand; ++b)
            {
                const auto i = (size_t) b;

                if (power[i] >= liveBandGatePower)
                {
                    highMidNow += 0.5 * (power[i] + cross[i]);
                    highSideNow += 0.5 * (power[i] - cross[i]);
                }
            }

            // 功率域一阶平滑（慢路与高频 300 ms、低频快路 120 ms），系数按 hop 换算（与块长无关）；重启后按运行均值起步
            // （第 n 帧保留系数取 min(常数, (n − 1) / n)）。
            ++liveStereoFrames;
            const auto n = (double) liveStereoFrames;
            const auto retain = juce::jmin (liveStereoRetain, (n - 1.0) / n);
            liveLowCross = lowCrossNow + (liveLowCross - lowCrossNow) * retain;
            liveLowPower = lowPowerNow + (liveLowPower - lowPowerNow) * retain;
            const auto lowFastRetain = juce::jmin (liveFastLowRetain, (n - 1.0) / n);
            liveLowCrossFast = lowCrossNow + (liveLowCrossFast - lowCrossNow) * lowFastRetain;
            liveLowPowerFast = lowPowerNow + (liveLowPowerFast - lowPowerNow) * lowFastRetain;
            liveHighMid = highMidNow + (liveHighMid - highMidNow) * retain;
            liveHighSide = highSideNow + (liveHighSide - highSideNow) * retain;
            snapToZero (liveLowCross);
            snapToZero (liveLowPower);
            snapToZero (liveLowCrossFast);
            snapToZero (liveLowPowerFast);
            snapToZero (liveHighMid);
            snapToZero (liveHighSide);

            // 有效：重启后已平滑满一个时间常数（运行均值覆盖满，单帧的估计散布太大，保护层不能拿它触发），且组内平滑功率仍在
            // 段静音门之上（门上有内容）。内容停了之后比值保持最后的内容，平滑功率落到门下才标无效。
            const auto settled = liveStereoFrames >= liveStereoSettleFrames;
            const auto seconds = n * (double) hop / sampleRate;

            // 慢路（300 ms）。ADR-012：只取慢路，不再与快路取小——快路怎么用由 ClayGuard 定。
            if (settled && liveLowPower >= liveBandGatePower)
            {
                setFeature (view.liveLowCorrelation, correlationOf (liveLowPower, liveLowCross), seconds);
                view.liveLowCorrelation.confidence = 1.0f;
            }
            else
            {
                invalidate (view.liveLowCorrelation);
            }

            // 快路（120 ms）：同门、同运行均值起步、同重启规则，平滑满它自己的时间常数才有效（低频内容停了之后它的平滑功率
            // 按 120 ms 衰减，比慢路早落到门下）。
            if (liveStereoFrames >= liveFastSettleFrames && liveLowPowerFast >= liveBandGatePower)
            {
                setFeature (view.liveLowCorrelationFast, correlationOf (liveLowPowerFast, liveLowCrossFast), seconds);
                view.liveLowCorrelationFast.confidence = 1.0f;
            }
            else
            {
                invalidate (view.liveLowCorrelationFast);
            }

            view.liveHighMidPower = (float) liveHighMid;
            view.liveHighSidePower = (float) liveHighSide;
            view.liveHighValid = settled && liveHighMid + liveHighSide >= liveBandGatePower;
        }

        //==============================================================================
        // 画像：只由累计量按纯函数算出
        //==============================================================================
        void updateLoudnessProfile() noexcept
        {
            loudnessDirty = false;

            const auto effectiveSeconds = (double) effectiveSamples / sampleRate;
            profile.effectiveSeconds = effectiveSeconds;

            // I
            if (learnIntegratedValid)
                setFeature (profile.integratedLufs, learnIntegratedLufs, effectiveSeconds);
            else
                invalidate (profile.integratedLufs);

            measuredIntegrated = fraction (effectiveSeconds, loudnessFullSeconds);

            // LRA：S 取样的相对门（绝对门以上 S 的能量平均 − 20 LU），门上 S 的 P95 − P10（EBU Tech 3342）。
            const auto rangeSeconds = (double) rangeTotalCount * (double) subBlockLength / sampleRate;
            invalidate (profile.loudnessRangeLu);

            if (rangeTotalCount > 0)
            {
                const auto relativeGate = energyToLufs (rangeTotalEnergy / (double) rangeTotalCount) + lraRelativeGateLu;
                const auto firstBin = firstBinAtOrAbove (relativeGate);
                std::uint64_t gated = 0;

                for (int bin = firstBin; bin <= rangeTopBin; ++bin)
                    gated += rangeCount[(size_t) bin];

                if (gated >= (std::uint64_t) minimumLraValues)
                {
                    const auto lowIndex = (std::uint64_t) ((double) (gated - 1) * lraLowFraction + 0.5);
                    const auto highIndex = (std::uint64_t) ((double) (gated - 1) * lraHighFraction + 0.5);
                    double low = 0.0, high = 0.0;
                    bool haveLow = false;
                    std::uint64_t cumulative = 0;

                    for (int bin = firstBin; bin <= rangeTopBin; ++bin)
                    {
                        cumulative += rangeCount[(size_t) bin];

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

                    setFeature (profile.loudnessRangeLu, high - low, rangeSeconds);
                }
            }

            profile.loudnessRangeLu.effectiveSeconds = rangeSeconds;
            measuredRange = fraction (rangeSeconds, lraFullSeconds);

            // 真峰值与 PLR（同一学习区间）。
            if (learnPeak > 0.0f)
                setFeature (profile.truePeakDbtp, juce::Decibels::gainToDecibels ((double) learnPeak), effectiveSeconds);
            else
                invalidate (profile.truePeakDbtp);

            measuredPeak = fraction (effectiveSeconds, loudnessFullSeconds);

            if (profile.integratedLufs.valid && profile.truePeakDbtp.valid)
                setFeature (profile.plrDb, (double) profile.truePeakDbtp.value - learnIntegratedLufs, effectiveSeconds);
            else
                invalidate (profile.plrDb);

            // 代表性：不重复的有效素材 / 15 s。有拿不到位置的素材时封顶按其占比在 1 与 0.75 之间插值（全无位置 = 0.75）。
            const auto coveredSeconds = (double) coveredCells * (double) subBlockLength / sampleRate;
            const auto unpositionedSeconds = (double) unpositionedSamples / sampleRate;
            const auto distinctSeconds = coveredSeconds + unpositionedSeconds;
            auto cap = 1.0;

            if (unpositionedSeconds > 0.0)
                cap = 1.0 - (1.0 - representativenessWithoutPosition) * unpositionedSeconds / distinctSeconds;

            representativeness = (float) juce::jmin (cap, distinctSeconds / representativeSeconds);
        }

        void updateSpectrumProfile() noexcept
        {
            spectrumDirty = false;

            const auto hopSeconds = (double) hop / sampleRate;
            const auto frameSeconds = (double) learnFrames * hopSeconds;
            std::array<double, (size_t) numBands> meanPower {}, levelDb {};
            double total = 0.0, low = 0.0, high = 0.0;

            for (int b = 0; b < numBands; ++b)
            {
                const auto i = (size_t) b;
                meanPower[i] = learnFrames > 0 ? powerSum[i] / (double) learnFrames : 0.0;
                levelDb[i] = meanPower[i] > 0.0 ? juce::jmax (noDataDb, 10.0 * std::log10 (meanPower[i])) : noDataDb;
                profile.bandLongTermDb[i] = (float) levelDb[i];

                total += meanPower[i];

                if (b <= lowShareLastBand)
                    low += meanPower[i];

                if (b >= highShareFirstBand)
                    high += meanPower[i];
            }

            const auto enoughFrames = frameSeconds >= minimumSpectrumSeconds && total > 0.0;

            if (enoughFrames)
            {
                setFeature (profile.lowShare, low / total, frameSeconds);
                setFeature (profile.highShare, high / total, frameSeconds);
            }
            else
            {
                invalidate (profile.lowShare);
                invalidate (profile.highShare);
            }

            // 频谱斜率：段静音门以上各段长期电平对 log2(频率) 的最小二乘直线斜率（dB / oct）。
            {
                double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
                int count = 0;

                for (int b = 0; b < numBands; ++b)
                {
                    if (levelDb[(size_t) b] < bandGateDb)
                        continue;

                    const auto x = (double) b / 3.0;   // 相邻段相差 1/3 倍频程
                    const auto y = levelDb[(size_t) b];
                    sx += x;
                    sy += y;
                    sxx += x * x;
                    sxy += x * y;
                    ++count;
                }

                const auto denominator = (double) count * sxx - sx * sx;

                if (enoughFrames && count >= minimumSlopeBands && denominator > 0.0)
                    setFeature (profile.spectralSlopeDbPerOct, ((double) count * sxy - sx * sy) / denominator, frameSeconds);
                else
                    invalidate (profile.spectralSlopeDbPerOct);
            }

            // 有效带宽：两端的滚降区（逐段比里侧邻段低 3 dB 以上的连续段）与静音门下的段不给 Recover。
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
                const auto weight = validWeight[b];

                if (weight <= 0.0)
                {
                    profile.tameEvidenceDb[b] = 0.0f;
                    profile.recoverEvidenceDb[b] = 0.0f;
                    measuredBand[b] = 0.0f;
                    continue;
                }

                profile.tameEvidenceDb[b] = (float) (tameSum[b] / weight);

                const auto persistence = deficitWeight[b] / weight;
                const auto persistenceGain = juce::jlimit (0.0, 1.0, (persistence - recoverPersistenceStart)
                                                                         / (recoverPersistenceFull - recoverPersistenceStart));
                profile.recoverEvidenceDb[b] = inBand[b] ? (float) (recoverSum[b] / weight * persistenceGain) : 0.0f;
                measuredBand[b] = fraction (weight * hopSeconds, spectrumFullSeconds);
            }

            measuredSpectrum = fraction (frameSeconds, spectrumFullSeconds);

            // 辅助量：高频谱通量的均值（有效帧上）与突起密度（学习区间全部帧的时长上）。
            const auto musicFluxSeconds = (double) learnMusicFluxFrames * hopSeconds;
            const auto fluxSeconds = (double) learnFluxFrames * hopSeconds;

            if (musicFluxSeconds >= minimumSpectrumSeconds)
                setFeature (aux.highBandFluxDb, learnFluxSum / (double) learnMusicFluxFrames, musicFluxSeconds);
            else
                invalidate (aux.highBandFluxDb);

            if (fluxSeconds >= minimumSpectrumSeconds)
                setFeature (aux.highBandFluxOnsetsPerSecond, (double) learnFluxEvents / fluxSeconds, fluxSeconds);
            else
                invalidate (aux.highBandFluxOnsetsPerSecond);

            measuredFlux = fraction (fluxSeconds, spectrumFullSeconds);

            // 低频电平范围：P90 − P10（格中心，误差 ≤ 0.5 dB）。
            if (frameSeconds >= minimumSpectrumSeconds)
            {
                const auto lowIndex = (std::uint64_t) ((double) (learnFrames - 1) * lowRangeLowFraction + 0.5);
                const auto highIndex = (std::uint64_t) ((double) (learnFrames - 1) * lowRangeHighFraction + 0.5);
                std::uint64_t cumulative = 0;
                int lowBin = -1, highBin = -1;

                for (int bin = 0; bin < lowRangeBins && highBin < 0; ++bin)
                {
                    cumulative += lowRangeCount[(size_t) bin];

                    if (lowBin < 0 && cumulative > lowIndex)
                        lowBin = bin;

                    if (cumulative > highIndex)
                        highBin = bin;
                }

                if (lowBin >= 0 && highBin >= 0)
                    setFeature (aux.lowBandRangeDb, (double) (highBin - lowBin) * lowRangeBinDb, frameSeconds);
                else
                    invalidate (aux.lowBandRangeDb);
            }
            else
            {
                invalidate (aux.lowBandRangeDb);
            }

            // 修坯 Clay 的立体声画像字段（1.3）。
            updateStereoProfile (hopSeconds);
        }

        // 修坯 Clay 的立体声画像字段（1.3）：逐段与两组，只由累计量按纯函数算出。
        void updateStereoProfile (double hopSeconds) noexcept
        {
            for (size_t b = 0; b < (size_t) numBands; ++b)
            {
                const auto summedPower = stereoPowerSum[b];

                if (stereoWeight[b] > 0.0 && summedPower > 0.0)
                {
                    profile.bandSideToMidDb[b] = (float) sideToMidDbOf (summedPower, stereoCrossSum[b]);
                    profile.bandCorrelation[b] = (float) correlationOf (summedPower, stereoCrossSum[b]);
                    measuredStereoBand[b] = fraction (stereoWeight[b] * hopSeconds, stereoFullSeconds);
                }
                else
                {
                    // 没有有效统计：数值 0、可信度 0（GlazeTypes.h 的约定）。0 dB / ρ = 0 看上去像「很宽」，读的人必须先看
                    // bandStereoConfidence（决策器选收单分频点时尤其要跳过可信度 0 的段）。
                    profile.bandSideToMidDb[b] = 0.0f;
                    profile.bandCorrelation[b] = 0.0f;
                    measuredStereoBand[b] = 0.0f;
                }
            }

            updateStereoGroup (lowStereoFirstBand, lowStereoLastBand, lowStereoWeight * hopSeconds,
                               profile.lowSideToMidDb, profile.lowCorrelation, measuredLowStereo);
            updateStereoGroup (highStereoFirstBand, highStereoLastBand, highStereoWeight * hopSeconds,
                               profile.highSideToMidDb, profile.highCorrelation, measuredHighStereo);
        }

        // 组特征：按组内各段的功率和算（功率大的段分量重）。有效 = 组里有内容的观测 ≥ 1 s 且功率和 > 0（同斜率 / 占比的口径），
        // 有效时长 = 该观测时长；置信度在 finalizeConfidences 里按 min(观测 / 10 s, 代表性) 给。
        void updateStereoGroup (int firstBand, int lastBand, double observedSeconds, Feature& sideToMid, Feature& correlation,
                                float& measured) noexcept
        {
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

        void updateOnsetProfile() noexcept
        {
            onsetDirty = false;

            const auto seconds = (double) onsetEffectiveSamples / sampleRate;

            if (seconds >= minimumOnsetSeconds)
                setFeature (profile.onsetDensityPerSecond, (double) onsetEvents / seconds, seconds);
            else
                invalidate (profile.onsetDensityPerSecond);

            // 强度取中位数（静音后第一击的突起可达封顶 30 dB，均值会被它拉偏）。
            if (onsetEvents >= minimumOnsetEvents)
            {
                const auto middle = (std::uint64_t) (onsetEvents - 1) / 2;
                std::uint64_t cumulative = 0;
                int medianBin = 0;

                for (int bin = 0; bin < strengthBins; ++bin)
                {
                    cumulative += strengthCount[(size_t) bin];

                    if (cumulative > middle)
                    {
                        medianBin = bin;
                        break;
                    }
                }

                setFeature (profile.onsetStrengthDb, ((double) medianBin + 0.5) * strengthBinDb, seconds);
            }
            else
            {
                invalidate (profile.onsetStrengthDb);
            }

            profile.onsetDensityPerSecond.effectiveSeconds = seconds;
            profile.onsetStrengthDb.effectiveSeconds = seconds;
            measuredOnset = fraction (seconds, onsetFullSeconds);
        }

        // 先试后用（第「五」节）：代价字段只由记下的有效块按纯函数算出。每记下一块重算一次（至多每 100 ms 音频一次，O(候选数)）。
        void updateTrialProfile() noexcept
        {
            trialDirty = false;

            const auto seconds = (double) trialCount * (double) trialBlockLength / sampleRate;
            measuredTrial = fraction (seconds, trialFullSeconds);

            const auto original = trialCount > 0 ? percentileOfTop (trialTop[0]) : 0.0;
            const auto enough = seconds >= trialMinimumSeconds && original > 0.0;

            if (enough)
                setFeature (aux.trialPeakP99Db, 20.0 * std::log10 (original), seconds);
            else
                invalidate (aux.trialPeakP99Db);

            for (int slot = 0; slot < numTrials; ++slot)
            {
                auto& cost = trialFeatureOf (slot);

                if (! enough)
                {
                    invalidate (cost);
                    continue;
                }

                const auto candidate = percentileOfTop (trialTop[(size_t) slot + 1]);
                setFeature (cost, 20.0 * std::log10 (juce::jmax (candidate, trialPeakFloorRatio * original) / original), seconds);
            }
        }

        // 线性插值的 P99（定义同 numpy.percentile 默认，逐位口径见文件头「五」）：位置 0.99·(n − 1) 两侧的顺序统计量插值。top 降序，第 j 小（0 起）= top[n − 1 − j]。
        double percentileOfTop (const std::array<double, (size_t) trialTopCount>& top) const noexcept
        {
            const auto n = trialCount;
            const auto position = trialPercentile * (double) (n - 1);
            const auto lower = (int) std::floor (position);
            const auto below = top[(size_t) (n - 1 - lower)];

            if (lower + 1 >= n)
                return below;

            const auto above = top[(size_t) (n - 2 - lower)];
            return below + (above - below) * (position - (double) lower);
        }

        Feature& trialFeatureOf (int slot) noexcept
        {
            if (slot == trialLowCut)
                return profile.lowCutPeakCostDb;

            if (slot == trialWidth)
                return profile.widthPeakCostDb;

            return profile.monoPeakCostDb[(size_t) (slot - trialFirstMono)];
        }

        // 修坯 Clay 的真次声占比（1.2 / 根治修订 3.1）：只由累计量按纯函数算出。有效时长与起音同一份（同一批组计入）。
        void updateSubProfile() noexcept
        {
            const auto seconds = (double) onsetEffectiveSamples / sampleRate;

            // E_全 = 0（例如 R = −L：M 恒为 0）→ 不知道（valid = false），不当 −∞ 也不当 0。E_sub 按 E_全 的 1e-15 封底（−150 dB，
            // 防 −∞）；低通对各频率 |H| ≤ 1，比值只会因起始瞬态略超 1，钳到 1（0 dB）。
            if (seconds >= minimumSubSeconds && midEnergy > 0.0)
                setFeature (profile.subShareDb, 10.0 * std::log10 (juce::jlimit (subShareFloor, 1.0, subEnergy / midEnergy)), seconds);
            else
                invalidate (profile.subShareDb);

            measuredSub = fraction (seconds, subFullSeconds);
        }

        void finalizeConfidences() noexcept
        {
            const auto rep = representativeness;
            profile.representativeness = rep;

            auto finalize = [rep] (Feature& feature, float measured, float factor = 1.0f)
            {
                feature.confidence = feature.valid ? juce::jmin (rep, measured) * factor : 0.0f;
            };

            finalize (profile.integratedLufs, measuredIntegrated);
            finalize (profile.loudnessRangeLu, measuredRange);
            finalize (profile.truePeakDbtp, measuredPeak);
            finalize (profile.plrDb, juce::jmin (measuredIntegrated, measuredPeak));
            finalize (profile.spectralSlopeDbPerOct, measuredSpectrum);
            finalize (profile.lowShare, measuredSpectrum);
            finalize (profile.highShare, measuredSpectrum);

            // 起音与辅助谱通量的一致性：学习区间内包络事件数 E 多于 250 Hz 以上的通量突起数 F 时，多出来的事件多半是
            // 低频起伏（持续低音的拍频、慢颤音）而不是宽带的起音——系数 = 0.5 + 0.5 · F / E（F ≥ E 时为 1）。
            auto agreement = 1.0f;

            if (onsetEvents > 0 && aux.highBandFluxOnsetsPerSecond.valid && learnFluxEvents < onsetEvents)
            {
                const auto ratio = (double) learnFluxEvents / (double) onsetEvents;
                agreement = (float) (minimumOnsetAgreement + (1.0 - minimumOnsetAgreement) * ratio);
            }

            aux.onsetAgreement = agreement;
            finalize (profile.onsetDensityPerSecond, measuredOnset, agreement);
            finalize (profile.onsetStrengthDb, measuredOnset, agreement);
            finalize (aux.highBandFluxDb, measuredFlux);
            finalize (aux.highBandFluxOnsetsPerSecond, measuredFlux);
            finalize (aux.lowBandRangeDb, measuredSpectrum);
            profile.lowBandRangeDb = aux.lowBandRangeDb;   // 决策器要用（Glue 风险），进画像随工程保存；辅助量里的一份留给诊断

            // 修坯 Clay（v0.1.7.0）：同一口径 min(观测充分度, 代表性)；逐段立体声可信度与 bandConfidence 同口径。
            finalize (profile.subShareDb, measuredSub);
            finalize (profile.lowSideToMidDb, measuredLowStereo);
            finalize (profile.lowCorrelation, measuredLowStereo);
            finalize (profile.highSideToMidDb, measuredHighStereo);
            finalize (profile.highCorrelation, measuredHighStereo);

            // 先试后用的代价（ADR-012）：观测充分度 = 有效块时长 / 10 s（有效时恒为 1），同样再与代表性取小。
            finalize (profile.lowCutPeakCostDb, measuredTrial);
            finalize (profile.widthPeakCostDb, measuredTrial);
            finalize (aux.trialPeakP99Db, measuredTrial);

            for (auto& cost : profile.monoPeakCostDb)
                finalize (cost, measuredTrial);

            for (size_t b = 0; b < (size_t) numBands; ++b)
            {
                profile.bandConfidence[b] = juce::jmin (rep, measuredBand[b]);
                profile.bandStereoConfidence[b] = juce::jmin (rep, measuredStereoBand[b]);
            }
        }

        //==============================================================================
        // 六、House 档（T-038；口径见本文件开头第「六」节与 SegmentLedger.h）。通用档不进这里的任何函数。
        //==============================================================================
        // 段落账清空：账本、事件之外的读数归零，画像回到「什么都不知道」。不分配（段落账只把计数归零）。
        void clearHouse (std::int64_t acceptFrom) noexcept
        {
            ledger.reset (acceptFrom);
            houseDirty = false;
            houseSubBlockPosition = houseFramePosition = acceptFrom;
            houseEventPending = false;
            houseSubHint = houseFrameHint = houseOnsetHint = houseTrialHint = -1;

            houseProfile = MusicProfile {};
            houseProfile.formatVersion = houseProfileFormatVersion;
            houseProfile.houseGate = ledger.getSummary();
            houseAux = AuxiliaryFeatures {};
            houseRepresentativeness = 0.0f;
            ++houseGeneration;
        }

        // 不连续点 / 换曲风：没定稿的段就地定稿，各路的位置从 at 重新数（宿主 reset 后表的时间轴从 0 起）。
        void houseEndEpoch (std::int64_t at) noexcept
        {
            const auto before = ledger.getStoredRevision();   // 同 houseAdvance（评审 F3）：账本真变了才置脏
            ledger.endEpoch (at);

            if (ledger.getStoredRevision() != before)
                houseDirty = true;

            houseSubBlockPosition = houseFramePosition = at;
            houseEventPending = false;
            houseSubHint = houseFrameHint = houseOnsetHint = houseTrialHint = -1;
        }

        // 四路数据都已越过的时刻：段尾不晚于它的段，数据已经交齐（第「六」节「定稿」）。
        std::int64_t houseCompleteBefore() const noexcept
        {
            // 子块：最后读到的子块末尾；表的响度计量关着（两个子块长都没有新子块）时按此刻 − 两个子块，免得段永远定不了稿。
            auto position = juce::jmax (houseSubBlockPosition, now - 2 * (std::int64_t) subBlockLength);

            // 帧：Balance 还没 prepare（hop 未知）时不等它。
            if (hop > 0)
                position = juce::jmin (position, houseFramePosition);

            // 起音：正在进行的事件归它触发的那一段；否则当前组的起点。
            if (houseEventPending)
                position = juce::jmin (position, houseEventTime);
            else if (houseGridKnown)
                position = juce::jmin (position, onsetGridOrigin + groupsSinceRestart * (std::int64_t) groupLength);

            // 先试后用（学习中候选在跑）：第 k 块的最后一笔在网格位置 (k + 1)·块长 + 最长延迟交齐，还没交齐的最早一块的起点。
            if (learning && ! trialRestartPending && houseGridKnown)
            {
                const auto ran = groupsSinceRestart * (std::int64_t) groupLength + groupFill - (std::int64_t) maxTrialDelay;
                const auto blocks = ran > 0 ? ran / (std::int64_t) trialBlockLength : 0;
                position = juce::jmin (position, onsetGridOrigin + blocks * (std::int64_t) trialBlockLength);
            }

            return position;
        }

        // 每次 processInput / processSpectrum 末尾：定稿交齐的段；学习中有新定稿就重算画像（不学习时留着，等 recomputeHouseProfile）。
        void houseAdvance() noexcept
        {
            // T-038 评审 F3：只在账本真变了时置脏（新存进一段或覆盖了旧段）；没存进的段（静音、账本满、重放不如旧值）不触发 O(段数) 的重算。
            const auto before = ledger.getStoredRevision();
            ledger.finalizeBefore (houseCompleteBefore());

            if (ledger.getStoredRevision() != before)
                houseDirty = true;

            if (houseDirty && learning)
                refreshHouseProfile();
        }

        void refreshHouseProfile() noexcept
        {
            computeHouseProfile();
            houseDirty = false;
            ++houseGeneration;
        }

        // ---- 归段（三路数据各自调；口径与通用画像的同一处累计相同）----

        // 子块（consumeSubBlock）：子块数、有效子块的 K 加权能量、真峰值；门块功率与 3 s 短期响度随产生它们的子块，窗口最早的样本
        // 早于段落账的起点就不收（同学习区间）。
        void noteHouseSubBlock (const LoudnessMeter::SubBlock& subBlock, std::int64_t start, bool effective, double gatingPower,
                                double gatingLufs, bool haveShortTerm, double shortTermPower, double shortTermLufs) noexcept
        {
            auto* segment = ledger.openSegmentAt (start, houseSubHint);

            if (segment == nullptr)
                return;

            const auto length = (std::int64_t) subBlockLength;
            const auto acceptFrom = ledger.getAcceptFrom();
            ++segment->subBlocks;
            segment->truePeak = juce::jmax (segment->truePeak, juce::jmax (0.0f, subBlock.truePeakLinear));

            if (effective)
            {
                ++segment->effectiveSubBlocks;
                segment->effectiveEnergy += subBlock.meanSquare;
            }

            if (windowCount >= gatingSubBlocks && subBlock.endSample - gatingSubBlocks * length >= acceptFrom
                && gatingLufs >= absoluteGateLufs && segment->gatingCount < SegmentLedger::maxSubBlocks)
                segment->gatingPower[(size_t) segment->gatingCount++] = (float) gatingPower;

            if (haveShortTerm && subBlock.endSample - shortTermSubBlocks * length >= acceptFrom
                && shortTermLufs >= absoluteGateLufs && segment->rangeCount < SegmentLedger::maxSubBlocks)
            {
                segment->rangeBin[(size_t) segment->rangeCount++] = (std::uint16_t) histogramBinOf (shortTermLufs);
                segment->rangeEnergy += shortTermPower;
            }
        }

        // 帧门之上的帧（consumeFrame）：长期段功率、低频起伏直方图、谱通量（同 consumeFrame 里「inside && music」那一段）。
        void noteHouseMusicFrame (SegmentLedger::Segment& segment, const std::array<double, (size_t) numBands>& power,
                                  bool haveFlux, double flux) noexcept
        {
            ++segment.frames;
            segment.frameSeconds += (double) hop / sampleRate;

            double lowPower = 0.0;

            for (size_t b = 0; b <= (size_t) lowShareLastBand; ++b)
                lowPower += power[b];

            const auto lowDb = 10.0 * std::log10 (juce::jmax (lowPower, 1.0e-30));
            const auto bin = juce::jlimit (0, lowRangeBins - 1, (int) std::floor ((lowDb - lowRangeMinDb) / lowRangeBinDb));
            ++segment.lowRangeCount[(size_t) bin];   // 一段最多 6.4 s 的帧，16 位够
            segment.lowRangeFirst = juce::jmin (segment.lowRangeFirst, bin);
            segment.lowRangeLast = juce::jmax (segment.lowRangeLast, bin);

            for (size_t b = 0; b < (size_t) numBands; ++b)
                segment.powerSum[b] += power[b];

            if (haveFlux)
            {
                segment.fluxSum += flux;
                ++segment.musicFluxFrames;
            }
        }

        // 立体声（accumulateEvidence 里，与频谱证据同一批帧、同一逐段静音门权重；同 accumulateStereo）。
        static void accumulateHouseStereo (SegmentLedger::Segment& segment, const std::array<double, (size_t) numBands>& currentPower,
                                           const std::array<double, (size_t) numBands>& currentCross,
                                           const std::array<double, (size_t) numBands>& gate) noexcept
        {
            double lowGate = 0.0, highGate = 0.0;

            for (int b = 0; b < numBands; ++b)
            {
                const auto i = (size_t) b;
                segment.stereoPowerSum[i] += gate[i] * currentPower[i];
                segment.stereoCrossSum[i] += gate[i] * currentCross[i];
                segment.stereoWeight[i] += gate[i];

                if (b >= lowStereoFirstBand && b <= lowStereoLastBand)
                    lowGate = juce::jmax (lowGate, gate[i]);

                if (b >= highStereoFirstBand && b <= highStereoLastBand)
                    highGate = juce::jmax (highGate, gate[i]);
            }

            segment.lowStereoWeight += lowGate;
            segment.highStereoWeight += highGate;
        }

        // 起音组（processGroup，在有效门上）：有效时长与真次声的两份能量（同「inside && effective」那一段）。
        void noteHouseGroup (std::int64_t groupStart) noexcept
        {
            auto* segment = ledger.openSegmentAt (groupStart, houseOnsetHint);

            if (segment == nullptr)
                return;

            segment->onsetEffectiveSamples += groupLength;
            segment->subEnergy += groupSubEnergy;
            segment->midEnergy += groupMidEnergy;
        }

        // 起音事件结束（finishEvent）：计入触发的那一组所在的段；强度的格同 finishEvent。
        void noteHouseEvent() noexcept
        {
            houseEventPending = false;
            auto* segment = ledger.openSegmentAt (houseEventTime, houseOnsetHint);

            if (segment == nullptr || segment->strengthCount >= SegmentLedger::maxEvents)
                return;

            const auto strengthDb = juce::jlimit (0.0, onsetStrengthCapDb, 10.0 * std::log10 (eventPeakRatio));
            const auto bin = juce::jlimit (0, strengthBins - 1, (int) std::floor (strengthDb / strengthBinDb));
            segment->strengthBin[(size_t) segment->strengthCount++] = (std::uint8_t) bin;
            ++segment->onsetEvents;
        }

        // 先试后用交齐的有效块（countTrialWrite）：按块起点归段，每一路插进段里最大的 8 个（同 insertTop）。
        void noteHouseTrial (const TrialRecord& record) noexcept
        {
            const auto time = onsetGridOrigin + record.block * (std::int64_t) trialBlockLength;
            auto* segment = ledger.openSegmentAt (time, houseTrialHint);

            if (segment == nullptr)
                return;

            const auto filled = segment->trialBlocks;
            insertTop (segment->trialTop[0], filled, record.original);

            for (size_t c = 0; c < (size_t) numTrials; ++c)
                insertTop (segment->trialTop[c + 1], filled, record.candidates[c]);

            ++segment->trialBlocks;
        }

        // ---- 画像（只由段落账按纯函数算出；每个字段的公式与通用画像的同名函数逐项相同，只是累计量换成满编段 / 全部段的和）----

        void clearHouseSums() noexcept
        {
            auto& s = houseSums;
            s.effectiveSeconds = s.positionedSeconds = s.unpositionedSeconds = s.fullSeconds = 0.0;
            s.peak = 0.0f;
            s.rangeCount.fill (0);
            s.rangeTotalEnergy = 0.0;
            s.rangeTotalCount = 0;
            s.rangeTopBin = -1;
            s.frames = 0;

            for (auto* sums : { &s.powerSum, &s.tameSum, &s.recoverSum, &s.deficitWeight, &s.validWeight,
                                &s.stereoPowerSum, &s.stereoCrossSum, &s.stereoWeight })
                sums->fill (0.0);

            s.lowStereoWeight = s.highStereoWeight = 0.0;
            s.lowRangeCount.fill (0);
            s.fluxFrames = s.musicFluxFrames = s.fluxEvents = 0;
            s.fluxSum = 0.0;
            s.onsetEffectiveSamples = s.onsetEvents = 0;
            s.strengthCount.fill (0);
            s.subEnergy = s.midEnergy = 0.0;

            for (auto& top : s.trialTop)
                top.fill (0.0);

            s.trialCount = 0;
        }

        // 先试后用的块：按格序整段整段地取（满 trialCapacity 块为止），每一路合并各段最大的 8 个（第「六」节）。
        void mergeHouseTrial (bool fullOnly) noexcept
        {
            auto& s = houseSums;

            for (auto& top : s.trialTop)
                top.fill (0.0);

            s.trialCount = 0;
            auto inserted = 0;   // 已插入的个数（insertTop 的 filled 口径；每一路相同）

            for (int i = 0; i < ledger.getNumStored(); ++i)
            {
                const auto& segment = ledger.getStored (i);

                if ((fullOnly && ! segment.full) || segment.trialBlocks <= 0)
                    continue;

                if (s.trialCount + segment.trialBlocks > trialCapacity)
                    break;

                const auto values = juce::jmin (segment.trialBlocks, trialTopCount);

                for (int k = 0; k < values; ++k)
                {
                    for (size_t series = 0; series < s.trialTop.size(); ++series)
                        insertTop (s.trialTop[series], inserted, segment.trialTop[series][(size_t) k]);

                    ++inserted;
                }

                s.trialCount += segment.trialBlocks;
            }
        }

        // 小节：已听小节 ÷ lockSegments；2 秒段：已听秒数 ÷ fallbackLockSeconds（0 ～ 1）。暂定期代表性按它封顶。
        double houseLockProgress (const HouseGateSummary& gate) const noexcept
        {
            if (gate.mode == SegmentMode::seconds)
            {
                const auto needed = houseConstants.fallbackLockSeconds;
                return std::isfinite (needed) && needed > 0.0 ? juce::jlimit (0.0, 1.0, gate.heardSeconds / needed) : 1.0;
            }

            const auto needed = houseConstants.lockSegments;
            return needed > 0 ? juce::jlimit (0.0, 1.0, (double) gate.heardSegments / (double) needed) : 1.0;
        }

        void computeHouseProfile() noexcept
        {
            const auto& gate = ledger.getSummary();
            auto& s = houseSums;
            clearHouseSums();

            const auto count = ledger.getNumStored();
            double gatingTotal = 0.0;
            std::uint64_t gatingCount = 0;

            for (int i = 0; i < count; ++i)
            {
                const auto& segment = ledger.getStored (i);

                // 全部段：有效时长、代表性。
                s.effectiveSeconds += segment.effectiveSeconds;
                (segment.positioned ? s.positionedSeconds : s.unpositionedSeconds) += segment.effectiveSeconds;

                if (! segment.full)
                    continue;

                // 满编段：LRA（3 s 短期响度）也只用满编段（T-038 阶段 3 集成改，原为全部段）。全部段的 LRA 取决于锁定前听到了哪些段落
                // （从第 1 小节播时 breakdown 还没出现，从 drop 播时已经听到），LRA 修正因此随起播位置变，合成 Tech 曲上方案增益差 1.4 dB，
                // 违反验收「从第 1 小节播与从 drop 播增益差 ≤ 0.5 dB」；只用满编段后两者听到的是同一批 drop。
                for (int k = 0; k < segment.rangeCount; ++k)
                {
                    const auto bin = (int) segment.rangeBin[(size_t) k];
                    ++s.rangeCount[(size_t) bin];
                    s.rangeTopBin = juce::jmax (s.rangeTopBin, bin);
                }

                s.rangeTotalEnergy += segment.rangeEnergy;
                s.rangeTotalCount += (std::uint64_t) segment.rangeCount;

                // 满编段：其余全部。
                s.fullSeconds += segment.effectiveSeconds;
                s.peak = juce::jmax (s.peak, segment.truePeak);

                for (int k = 0; k < segment.gatingCount; ++k)
                {
                    gatingTotal += (double) segment.gatingPower[(size_t) k];
                    ++gatingCount;
                }

                s.frames += segment.frames;

                for (size_t b = 0; b < (size_t) numBands; ++b)
                {
                    s.powerSum[b] += segment.powerSum[b];
                    s.tameSum[b] += segment.tameSum[b];
                    s.recoverSum[b] += segment.recoverSum[b];
                    s.deficitWeight[b] += segment.deficitWeight[b];
                    s.validWeight[b] += segment.validWeight[b];
                    s.stereoPowerSum[b] += segment.stereoPowerSum[b];
                    s.stereoCrossSum[b] += segment.stereoCrossSum[b];
                    s.stereoWeight[b] += segment.stereoWeight[b];
                }

                s.lowStereoWeight += segment.lowStereoWeight;
                s.highStereoWeight += segment.highStereoWeight;

                for (int bin = segment.lowRangeFirst; bin <= segment.lowRangeLast; ++bin)
                    s.lowRangeCount[(size_t) bin] += segment.lowRangeCount[(size_t) bin];

                s.fluxFrames += segment.fluxFrames;
                s.musicFluxFrames += segment.musicFluxFrames;
                s.fluxEvents += segment.fluxEvents;
                s.fluxSum += segment.fluxSum;
                s.onsetEffectiveSamples += segment.onsetEffectiveSamples;
                s.onsetEvents += segment.onsetEvents;

                for (int k = 0; k < segment.strengthCount; ++k)
                    ++s.strengthCount[(size_t) segment.strengthBin[(size_t) k]];

                s.subEnergy += segment.subEnergy;
                s.midEnergy += segment.midEnergy;
            }

            // 先试后用：满编段的块不够 10 s（锁定后才进来的段，候选不跑）就改用全部段的块。
            mergeHouseTrial (true);

            if ((double) s.trialCount * (double) trialBlockLength / sampleRate < trialMinimumSeconds)
                mergeHouseTrial (false);

            // I：满编段的门块按 BS.1770 相对门重新门限（门块功率逐个与相对门比，第二遍）。
            auto integratedValid = false;
            double integrated = 0.0;

            if (gatingCount > 0)
            {
                const auto relativeGate = energyToLufs (gatingTotal / (double) gatingCount) + integratedRelativeGateLu;
                const auto gatePower = std::pow (10.0, 0.1 * (relativeGate + 0.691));
                double gatedEnergy = 0.0;
                std::uint64_t gated = 0;

                for (int i = 0; i < count; ++i)
                {
                    const auto& segment = ledger.getStored (i);

                    if (! segment.full)
                        continue;

                    for (int k = 0; k < segment.gatingCount; ++k)
                    {
                        const auto power = (double) segment.gatingPower[(size_t) k];

                        if (power >= gatePower)
                        {
                            gatedEnergy += power;
                            ++gated;
                        }
                    }
                }

                if (gated > 0)
                {
                    integratedValid = true;
                    integrated = energyToLufs (gatedEnergy / (double) gated);
                }
            }

            houseProfile = MusicProfile {};
            houseProfile.formatVersion = houseProfileFormatVersion;
            houseProfile.houseGate = gate;
            houseAux = AuxiliaryFeatures {};
            houseMeasured = HouseMeasured {};

            houseLoudnessProfile (gate, integratedValid, integrated);
            houseSpectrumProfile();
            houseOnsetProfile();
            houseTrialProfile();
            houseSubProfile();
            houseConfidences();
        }

        // 同 updateLoudnessProfile：I / 真峰值 / PLR / LRA 用满编段（LRA 阶段 3 集成改，见 computeHouseProfile），代表性另按锁定进度封顶。
        void houseLoudnessProfile (const HouseGateSummary& gate, bool integratedValid, double integrated) noexcept
        {
            auto& p = houseProfile;
            const auto& s = houseSums;
            auto& m = houseMeasured;
            const auto fullSeconds = s.fullSeconds;
            p.effectiveSeconds = s.effectiveSeconds;

            if (integratedValid)
                setFeature (p.integratedLufs, integrated, fullSeconds);
            else
                invalidate (p.integratedLufs);

            m.integrated = fraction (fullSeconds, loudnessFullSeconds);

            const auto rangeSeconds = (double) s.rangeTotalCount * (double) subBlockLength / sampleRate;
            invalidate (p.loudnessRangeLu);

            if (s.rangeTotalCount > 0)
            {
                const auto relativeGate = energyToLufs (s.rangeTotalEnergy / (double) s.rangeTotalCount) + lraRelativeGateLu;
                const auto firstBin = firstBinAtOrAbove (relativeGate);
                std::uint64_t gated = 0;

                for (int bin = firstBin; bin <= s.rangeTopBin; ++bin)
                    gated += s.rangeCount[(size_t) bin];

                if (gated >= (std::uint64_t) minimumLraValues)
                {
                    const auto lowIndex = (std::uint64_t) ((double) (gated - 1) * lraLowFraction + 0.5);
                    const auto highIndex = (std::uint64_t) ((double) (gated - 1) * lraHighFraction + 0.5);
                    double low = 0.0, high = 0.0;
                    bool haveLow = false;
                    std::uint64_t cumulative = 0;

                    for (int bin = firstBin; bin <= s.rangeTopBin; ++bin)
                    {
                        cumulative += s.rangeCount[(size_t) bin];

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
            m.range = fraction (rangeSeconds, lraFullSeconds);

            if (s.peak > 0.0f)
                setFeature (p.truePeakDbtp, juce::Decibels::gainToDecibels ((double) s.peak), fullSeconds);
            else
                invalidate (p.truePeakDbtp);

            m.peak = fraction (fullSeconds, loudnessFullSeconds);

            if (p.integratedLufs.valid && p.truePeakDbtp.valid)
                setFeature (p.plrDb, (double) p.truePeakDbtp.value - integrated, fullSeconds);
            else
                invalidate (p.plrDb);

            // 代表性：全部段不重复的有效素材 / 15 s（不按位置的段封顶按其占比在 1 与 0.75 之间插值），再按锁定进度封顶。
            const auto distinctSeconds = s.positionedSeconds + s.unpositionedSeconds;
            auto cap = 1.0;

            if (s.unpositionedSeconds > 0.0)
                cap = 1.0 - (1.0 - representativenessWithoutPosition) * s.unpositionedSeconds / distinctSeconds;

            houseRepresentativeness = (float) juce::jmin (juce::jmin (cap, distinctSeconds / representativeSeconds), houseLockProgress (gate));
        }

        // 同 updateSpectrumProfile + updateStereoProfile（满编段的和）。
        void houseSpectrumProfile() noexcept
        {
            auto& p = houseProfile;
            auto& a = houseAux;
            const auto& s = houseSums;
            auto& m = houseMeasured;

            const auto hopSeconds = hop > 0 ? (double) hop / sampleRate : 0.0;
            const auto frameSeconds = (double) s.frames * hopSeconds;
            std::array<double, (size_t) numBands> meanPower {}, levelDb {};
            double total = 0.0, low = 0.0, high = 0.0;

            for (int b = 0; b < numBands; ++b)
            {
                const auto i = (size_t) b;
                meanPower[i] = s.frames > 0 ? s.powerSum[i] / (double) s.frames : 0.0;
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
                    m.band[b] = 0.0f;
                    continue;
                }

                p.tameEvidenceDb[b] = (float) (s.tameSum[b] / weight);

                const auto persistence = s.deficitWeight[b] / weight;
                const auto persistenceGain = juce::jlimit (0.0, 1.0, (persistence - recoverPersistenceStart)
                                                                         / (recoverPersistenceFull - recoverPersistenceStart));
                p.recoverEvidenceDb[b] = inBand[b] ? (float) (s.recoverSum[b] / weight * persistenceGain) : 0.0f;
                m.band[b] = fraction (weight * hopSeconds, spectrumFullSeconds);
            }

            m.spectrum = fraction (frameSeconds, spectrumFullSeconds);

            const auto musicFluxSeconds = (double) s.musicFluxFrames * hopSeconds;
            const auto fluxSeconds = (double) s.fluxFrames * hopSeconds;

            if (musicFluxSeconds >= minimumSpectrumSeconds)
                setFeature (a.highBandFluxDb, s.fluxSum / (double) s.musicFluxFrames, musicFluxSeconds);
            else
                invalidate (a.highBandFluxDb);

            if (fluxSeconds >= minimumSpectrumSeconds)
                setFeature (a.highBandFluxOnsetsPerSecond, (double) s.fluxEvents / fluxSeconds, fluxSeconds);
            else
                invalidate (a.highBandFluxOnsetsPerSecond);

            m.flux = fraction (fluxSeconds, spectrumFullSeconds);

            if (frameSeconds >= minimumSpectrumSeconds)
            {
                const auto lowIndex = (std::uint64_t) ((double) (s.frames - 1) * lowRangeLowFraction + 0.5);
                const auto highIndex = (std::uint64_t) ((double) (s.frames - 1) * lowRangeHighFraction + 0.5);
                std::uint64_t cumulative = 0;
                int lowBin = -1, highBin = -1;

                for (int bin = 0; bin < lowRangeBins && highBin < 0; ++bin)
                {
                    cumulative += s.lowRangeCount[(size_t) bin];

                    if (lowBin < 0 && cumulative > lowIndex)
                        lowBin = bin;

                    if (cumulative > highIndex)
                        highBin = bin;
                }

                if (lowBin >= 0 && highBin >= 0)
                    setFeature (a.lowBandRangeDb, (double) (highBin - lowBin) * lowRangeBinDb, frameSeconds);
                else
                    invalidate (a.lowBandRangeDb);
            }
            else
            {
                invalidate (a.lowBandRangeDb);
            }

            // 立体声（同 updateStereoProfile / updateStereoGroup）。
            for (size_t b = 0; b < (size_t) numBands; ++b)
            {
                const auto summedPower = s.stereoPowerSum[b];

                if (s.stereoWeight[b] > 0.0 && summedPower > 0.0)
                {
                    p.bandSideToMidDb[b] = (float) sideToMidDbOf (summedPower, s.stereoCrossSum[b]);
                    p.bandCorrelation[b] = (float) correlationOf (summedPower, s.stereoCrossSum[b]);
                    m.stereoBand[b] = fraction (s.stereoWeight[b] * hopSeconds, stereoFullSeconds);
                }
                else
                {
                    p.bandSideToMidDb[b] = 0.0f;
                    p.bandCorrelation[b] = 0.0f;
                    m.stereoBand[b] = 0.0f;
                }
            }

            houseStereoGroup (lowStereoFirstBand, lowStereoLastBand, s.lowStereoWeight * hopSeconds,
                              p.lowSideToMidDb, p.lowCorrelation, m.lowStereo);
            houseStereoGroup (highStereoFirstBand, highStereoLastBand, s.highStereoWeight * hopSeconds,
                              p.highSideToMidDb, p.highCorrelation, m.highStereo);
        }

        void houseStereoGroup (int firstBand, int lastBand, double observedSeconds, Feature& sideToMid, Feature& correlation,
                               float& measured) noexcept
        {
            const auto& s = houseSums;
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

        // 同 updateOnsetProfile。
        void houseOnsetProfile() noexcept
        {
            auto& p = houseProfile;
            const auto& s = houseSums;
            const auto seconds = (double) s.onsetEffectiveSamples / sampleRate;

            if (seconds >= minimumOnsetSeconds)
                setFeature (p.onsetDensityPerSecond, (double) s.onsetEvents / seconds, seconds);
            else
                invalidate (p.onsetDensityPerSecond);

            if (s.onsetEvents >= minimumOnsetEvents)
            {
                const auto middle = (std::uint64_t) (s.onsetEvents - 1) / 2;
                std::uint64_t cumulative = 0;
                int medianBin = 0;

                for (int bin = 0; bin < strengthBins; ++bin)
                {
                    cumulative += s.strengthCount[(size_t) bin];

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
            houseMeasured.onset = fraction (seconds, onsetFullSeconds);
        }

        // 同 updateTrialProfile / percentileOfTop（块数 = 合并进来的块数）。
        void houseTrialProfile() noexcept
        {
            auto& p = houseProfile;
            const auto& s = houseSums;
            const auto n = s.trialCount;
            const auto seconds = (double) n * (double) trialBlockLength / sampleRate;
            houseMeasured.trial = fraction (seconds, trialFullSeconds);

            const auto percentile = [n] (const std::array<double, (size_t) trialTopCount>& top) noexcept
            {
                const auto position = trialPercentile * (double) (n - 1);
                const auto lower = (int) std::floor (position);
                const auto below = top[(size_t) (n - 1 - lower)];

                if (lower + 1 >= n)
                    return below;

                const auto above = top[(size_t) (n - 2 - lower)];
                return below + (above - below) * (position - (double) lower);
            };

            const auto original = n > 0 ? percentile (s.trialTop[0]) : 0.0;
            const auto enough = seconds >= trialMinimumSeconds && original > 0.0;

            if (enough)
                setFeature (houseAux.trialPeakP99Db, 20.0 * std::log10 (original), seconds);
            else
                invalidate (houseAux.trialPeakP99Db);

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

                const auto candidate = percentile (s.trialTop[(size_t) slot + 1]);
                setFeature (cost, 20.0 * std::log10 (juce::jmax (candidate, trialPeakFloorRatio * original) / original), seconds);
            }
        }

        // 同 updateSubProfile。
        void houseSubProfile() noexcept
        {
            const auto& s = houseSums;
            const auto seconds = (double) s.onsetEffectiveSamples / sampleRate;

            if (seconds >= minimumSubSeconds && s.midEnergy > 0.0)
                setFeature (houseProfile.subShareDb, 10.0 * std::log10 (juce::jlimit (subShareFloor, 1.0, s.subEnergy / s.midEnergy)), seconds);
            else
                invalidate (houseProfile.subShareDb);

            houseMeasured.sub = fraction (seconds, subFullSeconds);
        }

        // 同 finalizeConfidences（代表性用 House 的那一份）。
        void houseConfidences() noexcept
        {
            auto& p = houseProfile;
            auto& a = houseAux;
            const auto& s = houseSums;
            const auto& m = houseMeasured;
            const auto rep = houseRepresentativeness;
            p.representativeness = rep;

            auto finalize = [rep] (Feature& feature, float measured, float factor = 1.0f)
            {
                feature.confidence = feature.valid ? juce::jmin (rep, measured) * factor : 0.0f;
            };

            finalize (p.integratedLufs, m.integrated);
            finalize (p.loudnessRangeLu, m.range);
            finalize (p.truePeakDbtp, m.peak);
            finalize (p.plrDb, juce::jmin (m.integrated, m.peak));
            finalize (p.spectralSlopeDbPerOct, m.spectrum);
            finalize (p.lowShare, m.spectrum);
            finalize (p.highShare, m.spectrum);

            auto agreement = 1.0f;

            if (s.onsetEvents > 0 && a.highBandFluxOnsetsPerSecond.valid && s.fluxEvents < s.onsetEvents)
            {
                const auto ratio = (double) s.fluxEvents / (double) s.onsetEvents;
                agreement = (float) (minimumOnsetAgreement + (1.0 - minimumOnsetAgreement) * ratio);
            }

            a.onsetAgreement = agreement;
            finalize (p.onsetDensityPerSecond, m.onset, agreement);
            finalize (p.onsetStrengthDb, m.onset, agreement);
            finalize (a.highBandFluxDb, m.flux);
            finalize (a.highBandFluxOnsetsPerSecond, m.flux);
            finalize (a.lowBandRangeDb, m.spectrum);
            p.lowBandRangeDb = a.lowBandRangeDb;

            finalize (p.subShareDb, m.sub);
            finalize (p.lowSideToMidDb, m.lowStereo);
            finalize (p.lowCorrelation, m.lowStereo);
            finalize (p.highSideToMidDb, m.highStereo);
            finalize (p.highCorrelation, m.highStereo);

            finalize (p.lowCutPeakCostDb, m.trial);
            finalize (p.widthPeakCostDb, m.trial);
            finalize (a.trialPeakP99Db, m.trial);

            for (auto& cost : p.monoPeakCostDb)
                finalize (cost, m.trial);

            for (size_t b = 0; b < (size_t) numBands; ++b)
            {
                p.bandConfidence[b] = juce::jmin (rep, m.band[b]);
                p.bandStereoConfidence[b] = juce::jmin (rep, m.stereoBand[b]);
            }
        }
    };

    //==============================================================================
    MusicAnalyzer::MusicAnalyzer() : impl (std::make_unique<Impl>()) {}
    MusicAnalyzer::~MusicAnalyzer() = default;

    void MusicAnalyzer::prepare (const juce::dsp::ProcessSpec& spec)
    {
        impl->configure (spec.sampleRate, (int) spec.numChannels);
        impl->allocate();   // 先试后用随采样率定长的部分（收单候选低通、M / S 延迟环）：只在这里分配
        impl->resetAll();
    }

    void MusicAnalyzer::reset()
    {
        impl->resetAll();
    }

    void MusicAnalyzer::handleDiscontinuity (Discontinuity)
    {
        // 跳播、循环回绕、宿主 reset 在分析器这一侧做同一件事：在此刻断开全部窗口（M 0.4 s、S 3 s、门块、PSR、
        // 频谱平均、谱通量、起音包络），各自重新填满后恢复；已完成的累计与学习区间不动。宿主 reset 时两块表与 Balance
        // 的样本计数会归零，下一块 processInput / processSpectrum 自己核对到后把游标与学习起点挪到新时间轴上。
        impl->beginDiscontinuity (impl->now);
    }

    void MusicAnalyzer::processInput (const juce::AudioBuffer<float>& input, const LoudnessMeter& inputMeter) noexcept
    {
        impl->processInput (input, inputMeter);
    }

    void MusicAnalyzer::processSpectrum (const BalanceModule& balance) noexcept
    {
        impl->processSpectrum (balance);
    }

    void MusicAnalyzer::setHostInfo (const HostInfo& info) noexcept
    {
        impl->host = info;
    }

    void MusicAnalyzer::startLearning() noexcept
    {
        // 候选累计全部清空；学习区间从此刻（上一块末尾）起。窗口不断开：之后到达的窗口只要最早的样本不早于此刻就计入。
        // 先试后用的候选从下一个样本起从零状态重新起跑（clearLearning 置位，runTrial 在音频线程上清零，不分配）。
        impl->clearLearning();
        impl->learning = true;
        impl->learnStart = impl->now;

        // House 档：「重新聆听」时段落账清空、从头再来（设计规格-曲风-House学习方式 第 4 节）；起点与学习区间相同。
        if (impl->houseActive)
            impl->clearHouse (impl->now);
    }

    void MusicAnalyzer::stopLearning() noexcept
    {
        impl->learning = false;   // 画像每次有新数据都已重算，停下即固定
    }

    bool MusicAnalyzer::isLearning() const noexcept
    {
        return impl->learning;
    }

    // House 档换成段落账门控算出的那一份（文件头第「六」节）；通用档逐位同改前。
    const MusicProfile& MusicAnalyzer::getCandidateProfile() const noexcept     { return impl->houseActive ? impl->houseProfile : impl->profile; }
    const ShortTermView& MusicAnalyzer::getShortTermView() const noexcept       { return impl->view; }
    const AuxiliaryFeatures& MusicAnalyzer::getAuxiliaryFeatures() const noexcept { return impl->houseActive ? impl->houseAux : impl->aux; }

    //==============================================================================
    // T-038 House 档
    void MusicAnalyzer::setGenre (Genre genre) noexcept
    {
        auto& s = *impl;

        if (genre == s.genre)
            return;

        s.genre = genre;
        const auto house = isHouse (genre);

        if (house == s.houseActive)
            return;   // 子类之间换（学习方式相同）：段落账照记

        if (s.houseActive)
        {
            // 出 House：没定稿的段就地定稿，账留着（换回 House 时接着用）；之后三路数据不再归段。
            s.houseEndEpoch (s.now);
            s.houseActive = false;
            return;
        }

        // 进 House：从此刻起分段（此刻之前的窗口不收）；画像按段落账重算（学习中下一次定稿时）。
        s.houseActive = true;
        s.ledger.setSubBlockLength (s.subBlockLength);
        s.houseEndEpoch (s.now);
        s.houseGridKnown = false;
        s.houseDirty = true;
    }

    Genre MusicAnalyzer::getGenre() const noexcept   { return impl->genre; }

    void MusicAnalyzer::setHouseConstants (const HouseConstants& constants) noexcept
    {
        impl->houseConstants = constants;
        impl->ledger.setConstants (constants);
    }

    void MusicAnalyzer::recomputeHouseProfile() noexcept
    {
        if (impl->houseActive)
            impl->refreshHouseProfile();
    }

    void MusicAnalyzer::clearHouseLedger() noexcept
    {
        impl->clearHouse (impl->now);
    }

    const SegmentLedger& MusicAnalyzer::getSegmentLedger() const noexcept   { return impl->ledger; }
    std::uint32_t MusicAnalyzer::getHouseProfileGeneration() const noexcept  { return impl->houseGeneration; }
}

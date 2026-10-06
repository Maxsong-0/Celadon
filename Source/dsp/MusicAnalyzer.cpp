/*
  ==============================================================================

    MusicAnalyzer.cpp
    GLAZE 分析器（设计规格-GLAZE-分析器全文；设计规格-GLAZE-时间线与保存第 3 节的分析器一侧）。

    三路数据，各按自己的时间戳独立累计（所以结果与宿主块长无关）：
      一、子块流：IN 表的 100 ms 子块（K 加权均方 + 子块真峰值）→ M / S / I / LRA / 真峰值 / PLR / PSR、有效时长、代表性；
      二、帧流：Balance 每帧原始段功率（约 10 ms 一帧）→ 长期段电平、频谱正负证据、斜率、低 / 高频占比、辅助谱通量；
              v0.1.7.0 起另读同一帧的左右交叉功率 → 修坯 Clay 的立体声画像字段与实时立体声视图；
      三、样本流：链入口原始样本 → 起音包络（功率域）与事件；v0.1.7.0 起另有 M 的 8 阶 16 Hz 低通 → 修坯 Clay 的真次声占比，
              学习期间（实时模式开着时一直）另把修坯的候选处理跑在原始样本上 → 先试后用的峰值代价（第「五」节，ADR-012；
              候选本身在 ClayTrials，T-048 阶段 1 原样抽出）。
    修坯 Clay 的特征（下面第「四」「五」节）挂在帧流与样本流上，不另起时间轴、不新增 FFT。
    画像只由各路的累计量按纯函数算出（每次有新数据就重算），停止学习后固定。
    T-038 House 档（第「六」节）：三路数据另按段归进段落账（SegmentLedger），候选画像改由满编段门控算出；通用档不进第「六」节的任何代码。
    T-048 实时模式（第「七」节，默认关）：另一份实时段落账，所有曲风都按第「六」节同一口径分段、记账，定稿的段交给 SegmentSink；
    关着时实时段落账不分配、不跑，一切输出与 T-048 之前逐位相同。

    实时约束：全部状态在 Impl 里定长（构造时一次分配；先试后用的收单候选低通与 M / S 延迟环随采样率定长，prepare 时分配；
    实时段落账在打开实时模式与 prepare 时分配），音频线程上的函数不分配、不加锁、不做系统调用；时间常数一律按样本数折算。
    reset 逐项清零，不在栈上构造整个 Impl（约 160 KB：其中 House 与实时两份段落账的定长成员各约 50 KB，实时一侧另有三道门的参考与
    交段时换算用的一份 Segment 约 17 KB）。

  ==============================================================================
*/

#include "MusicAnalyzer.h"

#include "ClayTrials.h"
#include "GlazeProfile.h"
#include "ProfileMath.h"
#include "TruePeak.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <type_traits>
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
        // integratedRelativeGateLu / lra* / minimumLraValues：ProfileMath.h

        // I 与 LRA 的固定容量直方图：−70 … +10 LUFS，0.1 LU 一格。门块能量逐块精确累加，只有「在不在相对门之上」
        // 按格中心判断（误差 ≤ 0.05 LU）；LRA 的百分位取格中心（误差 ≤ 0.05 LU）。内存与耗时不随时长增长，
        // 相对门变了，历史门块自动重新参与判定。
        // histogramMinLufs / histogramBins*：ProfileMath.h

        // 「有效音频」门限：400 ms 响度 ≥ −70 LUFS，且不低于学习区间 I 以下 20 LU（与 LRA 同一相对门）。
        // 有效时长、代表性、musicActive 都用它；淡出、段间静音、底噪不算学习时间。
        constexpr double effectiveRelativeGateLu = -20.0;

        // 不连续点之后丢掉起点落在「不连续点 + 64 样本」之前的子块：表的 8× 真峰值检测器窗长 64，拼接处的阶跃过冲只在这之内。
        constexpr int discontinuityGuardSamples = TruePeakDetector::tapsPerPhase;

        //==============================================================================
        // 置信度：各特征「观测充分」所需的有效时长（秒）。置信度 = min(观测充分度, 代表性)。
        // loudnessFullSeconds / lraFullSeconds / spectrumFullSeconds / onsetFullSeconds /
        // minimumSpectrumSeconds / minimumOnsetSeconds / minimumOnsetEvents：ProfileMath.h

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
        // bandGateDb：ProfileMath.h（段静音门，与 Balance 相同）
        constexpr double bandGateFadeDb = 6.0;     // 门上 6 dB 内权重线性淡入
        constexpr double contourKernel[] = { 0.0, 3.0, 2.0, 1.0 };   // 下标 = 与本段的距离（本段自己不参与）
        constexpr int contourRadius = 3;
        constexpr double frameAbsoluteGatePower = 1.0e-7;   // 帧总功率 −70 dBFS 绝对门
        constexpr double frameRelativeGateRatio = 0.01;     // 帧总功率低于学习区间均值 20 dB 的帧不计（淡出、间歇）

        // Recover 比 Tame 保守：只对持续缺失（缺失帧占比 50 % 起算、80 % 满额）、且在有效带宽内的中间段。
        // 有效带宽：两端各自往里数，逐段比里侧邻段低 3 dB 以上的连续段是滚降区（歌本身的高频滚降、低频下潜），不填；
        // 两端的 40 Hz 与 16 kHz 段本身没有外侧邻段，分不清「缺口」还是「滚降」，一律不给 Recover。
        // recoverPersistence* / rollOffStepDb / lowShareLastBand / highShareFirstBand /
        // minimumSlopeBands / noDataDb：ProfileMath.h
        // 低频 = 100 Hz 段及以下（段 0 ～ 4），高频 = 6.3 kHz 段及以上（段 22 ～ 26）。

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
        // lowRangeBinDb / lowRangeBins / lowRangeLowFraction / lowRangeHighFraction：ProfileMath.h

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
        // onsetGroupSeconds / maximumGroupLength：ProfileMath.h（384 kHz 以下都是整 0.33 ms；再高就缩短组长）
        constexpr double onsetFastSeconds       = 0.010;
        constexpr double onsetSlowSeconds       = 0.400;
        constexpr double onsetTriggerDb         = 6.0;
        constexpr double onsetRearmDb           = 3.0;
        constexpr double onsetRefractorySeconds = 0.050;
        constexpr double onsetWarmUpSeconds     = 0.250;
        constexpr double onsetTriggerFloorPower = 1.0e-6;      // −60 dBFS
        constexpr double onsetSlowFloorPower    = 1.0e-7;      // −70 dBFS
        constexpr double onsetEffectiveRelativeRatio = 0.01;   // −20 dB
        // onsetStrengthCapDb / strengthBinDb / strengthBins：ProfileMath.h

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
        // minimumOnsetAgreement：ProfileMath.h

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
        //     泄漏门（BUG-009，T-042）：本帧低频组的核心（段 0 ～ 4，40 ～ 100 Hz）门上功率 < lowLeakageRatio（−35 dB）× 段 6 ～ 8
        //     （160 ～ 250 Hz）的功率时，本帧低频组按「没有内容」算——两路的 ΣX、ΣP 都记 0，与整组都在段静音门下同一处理（平滑值按
        //     系数衰减、比值停在最后的真内容上，平滑功率落到门下就无效）。为什么：段 5（125 Hz）的格子伸到 152 Hz（48 k / 2048 点时
        //     bin 6 = 129 ～ 152 Hz 按 0.49 计入），Hann 主瓣再把 ~200 Hz 以内的内容漏进来；没有低音、只有 140 Hz 以上内容的段落里，
        //     低频组的读数全是这部分泄漏，相关度就是漏进来那些分音的左右关系。合成 House 的 breakdown（只有左右各自失谐 ±0.3 % 的
        //     pad，最低分音 174.6 / 196 Hz）：段 0 ～ 4 全在 −60 dBFS 门下、段 5 却在门上，慢路随 ~1 Hz 的拍频摆到 −0.95，反相提示亮了
        //     breakdown 的 89 %（全曲 14.4 %）。
        //     判据为什么看「核心对上方」而不看「低频对全频」：22 首门槛歌里真反相段（WTF、will we be okay 等，40 ～ 80 Hz 各段 ρ 都 < −0.2、
        //     3 s 平均 ρ −0.3 ～ −0.5、持续到 7 ～ 9 s）的低频组占全频也只有 −20 ～ −29 dB，与 breakdown 的泄漏（−25 ～ −31 dB）重叠，
        //     按占比设门要么修不掉、要么把真反相一起关掉；核心对上方：真反相帧 ≥ −34 dB（只有 l_kskdhjf 不到 1 % 的尾巴更低），
        //     breakdown 的泄漏 −42 ～ −45 dB。门限按 Balance 的 Hann 窗与段权重逐 Hz 算（44.1 ～ 192 kHz 各采样率）：单个满幅正弦要
        //     单独把门打开，频率只能落在 140.5 ～ 162 Hz（段 5 格子上沿之上不到一个 bin，2048 点分辨率的极限，这里本来就分不开），
        //     ≥ 162 Hz 的任何内容都打不开；−38 dB 时 48 k 系的 170 ～ 175 Hz 也打得开（174.6 Hz 正在第一旁瓣上，核心 / 上方 =
        //     −37.6 dB），所以取 −35 dB。上方取到段 8：250 Hz 一带的分音在段 6 ～ 7 里也只剩旁瓣，只看段 6 ～ 7 时核心 / 上方可到
        //     −20 dB、比门限松得多；算上段 8，200 ～ 300 Hz 的分音 ≤ −46 dB。≥ 300 Hz 的内容漏进核心约 −66 dB 以下，满幅也在
        //     −60 dBFS 段门下。核心与上方都是 0（例如只有段 5 一段有内容、上方什么都没有）时不算泄漏。只改「哪些帧算低频内容」，相关度的算法与有效规则不变；声音不受影响（ADR-015 起反相只提示）。
        //     高频组不需要这道门：组宽 2 ～ 16 kHz（约 600 个 bin），从下方漏进的只有段 17 下沿一个 bin 以内的内容，与组内真内容同性质；
        //     画像的立体声特征是学习区间里的功率域时间和，泄漏帧功率小、被真低音稀释，且低频组的画像特征自 ADR-015 起不进任何决策，不改。
        constexpr double subCutoffHz = 16.0;
        constexpr std::array<double, 4> subStageQ { 0.5097955791041592, 0.6013448869350453,    // 8 阶 Butterworth 四对极点的 Q
                                                    0.8999762231364156, 2.5629154477415055 };  // = 1 / (2·cos((2k − 1)·π/16))
        // subShareFloor / minimumSubSeconds / subFullSeconds / 立体声组段号 / stereoRatioFloor /
        // minimumStereoSeconds / stereoFullSeconds：ProfileMath.h
        // 低频组：段 0 ～ 5（40 ～ 125 Hz）；高频组：段 17 ～ 26（2 ～ 16 kHz）。
        constexpr double liveStereoSeconds = 0.3;       // 实时视图的平滑时间常数；重启后平滑满这么久才标有效
        // 低频相关度快路（ADR-012，设计规格-Clay-根治修订 3.1 / 3.4；审核 V1-01）：120 ms，重启后平滑满 120 ms 才标有效。
        // 阶段 2 的 60 ms 快路与慢路取小：40 ～ 125 Hz 在 2048 点 FFT 里只有 4 ～ 5 个 bin，60 ms 平滑的估计散布约 ±0.5，宽立体声
        // 低音被大面积判成反相（V1：底鼓 + 独立宽垫、长期 ρ 0.87，30 s 触发 6 次；真歌 kskdhjf 75 %、KINOSEI 57 % 的时间在保护）。
        // 120 ms 把散布压到约 ±0.35（∝ 1 / √τ）；整体反相时它约 83 ms 过零（τ·ln 2），慢路约 210 ms。
        constexpr double liveFastLowSeconds = 0.12;
        constexpr double liveBandGatePower = 1.0e-6;    // 实时视图的逐段静音门：本帧段功率 −60 dBFS（与 bandGateDb 同一电平）
        // 实时视图的泄漏门（BUG-009，见上「泄漏门」）：核心 = 低频组的段 0 ～ 4，上方 = 段 6 ～ 8；核心门上功率 < 上方功率 × 10^(−35/10)
        // → 本帧低频组只算泄漏、不计。
        constexpr int lowStereoCoreLastBand = 4;
        constexpr int lowLeakageFirstBand = 6;
        constexpr int lowLeakageLastBand = 8;
        constexpr double lowLeakageRatio = 3.1622776601683794e-4;   // −35 dB

        static_assert (lowStereoLastBand < highStereoFirstBand && highStereoLastBand == numBands - 1,
                       "修坯的低频组 / 高频组段号必须落在 27 段之内且不重叠");
        static_assert (lowStereoFirstBand <= lowStereoCoreLastBand && lowStereoCoreLastBand < lowStereoLastBand
                           && lowLeakageFirstBand == lowStereoLastBand + 1 && lowLeakageLastBand < highStereoFirstBand,
                       "泄漏门：核心是低频组去掉最上一段，上方从低频组的下一段起、不碰高频组");

        //==============================================================================
        // 五、修坯 Clay 的先试后用（ADR-012，设计规格-Clay-根治修订 3.2；T-043 守护混合损失，ADR-027）：候选处理、100 ms 块网格、交齐与
        //     P99 代价的口径全在 ClayTrials.h（T-048 阶段 1 从这一节原样抽出，逐位不变）。分析器这边只管三件事：
        //   - 什么时候跑：学习期间跑（不学习时不耗 CPU）；实时模式开着时一直跑（第「七」节）。每次开始学习、每次组网格重启都从零状态
        //     重新起跑（实时模式开着时也一样，所以学习期的代价与实时模式关着时逐位相同）；
        //   - 每个组算不算（noteGroup 的两种口径）：学习口径 = 过了重启预热、在学习区间内、在有效音频门上（与起音 / 真次声同一套边界）；
        //     实时口径 = 过了重启预热、在有效音频门上；
        //   - 交齐的块进哪里（trialBlockCompleted）：学习口径有效、且交齐时还在学习的块进学习累计（最多 ClayTrials::capacity 块）与
        //     House 段落账；实时口径有效的块进实时段落账（第「七」节）。实时模式关着时候选只在学习期间跑，「交齐时还在学习」恒成立，
        //     与原来「有效就记」逐位相同；开着时学习停下之后才交齐的块（原来根本交不齐）不进学习累计。
        //   - 代价（updateTrialProfile）= ClayTrials::costsOf（与 House 画像的 houseTrialProfile 同一份），置信度同其他特征：
        //     min(观测充分度 = 有效块时长 / 10 s, 代表性)。
        static_assert (ClayTrials::warmUpSeconds == onsetWarmUpSeconds, "候选起跑的预热与起音的重启预热同长（同一套边界）");
        static_assert (maximumGroupLength <= ClayTrials::maxSegmentSamples, "候选一次处理一个组里的一段");

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
        static_assert (SegmentLedger::trialSeries == ClayTrials::numSeries && SegmentLedger::trialTopCount == ClayTrials::topCount
                       && SegmentLedger::trialCapacity == ClayTrials::capacity && SegmentLedger::blendSeries == ClayTrials::numBlend,
                       "段落账的先试后用记账必须与 ClayTrials 同口径");
        static_assert (std::is_same_v<decltype (SegmentLedger::Segment::trialTop), ClayTrials::TopSeries>
                       && std::is_same_v<decltype (SegmentLedger::Segment::liveTrialTop), ClayTrials::TopSeries>
                       && std::is_same_v<decltype (SegmentLedger::Segment::blendIntended), ClayTrials::BlendSums>
                       && std::is_same_v<decltype (SegmentLedger::Segment::liveBlendOutput), ClayTrials::BlendSums>,
                       "段的先试后用累计与 ClayTrials 的累计同一种类型（ClayTrials::accumulate 直接往段里并）");
        static_assert (strengthBins <= 256, "起音强度的格号按 8 位存");
        static_assert (SegmentLedger::loudnessHistogramMinLufs == histogramMinLufs && SegmentLedger::loudnessBinsPerLu == histogramBinsPerLu,
                       "段落账的响度直方图与分析器 I / LRA 的直方图同格");

        //==============================================================================
        // 七、T-048 实时模式（MusicAnalyzer.h 文件头「T-048」一节；设计规格-实时GLAZE 第 3 节）。默认关；关着时下面一项都不跑。
        //   - 另一份段落账（liveLedger，与 House 档那一份同一个类、同一套常数）：所有曲风都分段，三路数据按第「六」节同一口径归段
        //     （同样按起点归属、同样等四路都越过段尾才定稿），定稿的段交给 SegmentSink。它不进任何画像：通用画像、House 档那一份段落账与
        //     House 画像、辅助量、短期视图与实时模式关着时逐位相同（House 那一份连 live* 四项也与关着时一样，只记学习期间的块）。
        //     为什么不复用 House 那一份：它的定稿时刻、事件、计数与账本内容编排器每块都读（锁定后防漏网、此刻是否主段落），实时一侧要的
        //     「不学习也等先试后用的块交齐再定稿」「通用档也记」「重新聆听不清」都会改动它们。
        //   - 三道相对门的参考与学习无关（T-048 阶段 1 修订；REPORT-C 2.4、2.5、7.1）。House 那一份与通用画像的三道相对门参考学习区间：
        //     帧门 = 学习区间的帧均值（读档后不学习，learnFrames = 0，门就没了）；子块有效门 = 学习中学习区间的 I、不学习时画像的 I；
        //     起音组有效门 = 学习区间的平均功率。学习在宿主块边界上停、帧又晚一块才交，实时段落账若也用它们，交来的段就随块长、随学不学习变。
        //     实时段落账改用自己的三份长期参考（门的形状与学习区间那一份相同，只换参考）：
        //       帧门         总功率 ≥ −70 dBFS，且不低于「实时帧均值」20 dB 以上；
        //       子块有效门   400 ms 响度 ≥ −70 LUFS，且不低于「实时 I」20 LU 以上（实时 I 同 BS.1770：绝对门之上的门块，−10 LU 相对门）；
        //       起音组有效门 慢包络 ≥ −70 dBFS，且不低于「实时组平均功率」20 dB 以上。
        //     口径：
        //       ① 每份参考只由这一路自己的数据按先后逐项更新（帧、门块、起音组各按自己的时间戳）：与宿主怎么切块无关，也不读学习状态；
        //       ② 先按参考判这一项的门，再把它并进参考（因果，同学习区间）；还没有参考时只用绝对门（同学习区间的冷启动）；
        //       ③ 并进参考的是绝对门之上、且窗口不早于实时段落账起点的项，不看相对门——学习区间的 I 与起音组也是这样；帧门原来只并过了
        //          相对门的帧，这里不跟：否则一首比参考轻 20 dB 以上的歌永远进不了参考、永远过不了门；
        //       ④ 按进来的音频遗忘：每并进一项，旧的全部乘 exp(−这一项的时长 ÷ liveGateMemorySeconds)。静音不推进遗忘（参考不会在
        //          歌间停顿里漂走）；换成一首轻得多的歌，参考按它的有效时长跟过去。时间常数取 180 s：参考代表「这首歌的主体」，不随
        //          一两个段落起落（32 小节的 breakdown 只把帧均值拉低约 1.4 dB，离 20 dB 的门很远），与实时画像参考域的遗忘同量级；
        //       ⑤ 参考随实时段落账一起清（打开实时模式、reset、prepare），不在不连续点清（跳播、回绕还是同一首歌）；
        //       ⑥ 持续偏轻就加快遗忘（Max 2026-10-03）：④ 的 180 s 让换成轻 20 dB 以上的素材（同一实例换成一首轻得多的歌、长时间 solo 一条
        //          很轻的轨）要 5～13 分钟才过门（按功率平均：轻 28 dB 时帧门约 1.85 τ、子块门约 4.25 τ），这期间实时画像认不出新素材。
        //          每一路各自净计「有声（绝对门之上、并进参考的项）却没过相对门」比「过了相对门」多出的时长：没过门的项加、过门的项减，
        //          夹在 0 与 liveGateQuietSeconds 之间，静音不计。到顶时没过门的项按 liveGateFastMemorySeconds 遗忘着并进，过门的项照旧按 ④。
        //          净计而不是「过门就清零」：轻下来的新素材里底鼓之类的瞬态会先过门，清零会让加快一拍一断、永远加不上去；多数项都过门之后
        //          净计自然回落、回到平时的遗忘。估算（之前响了 1～5 分钟、新素材轻 28 dB）：帧门约 40～45 s、子块门约 50～60 s 之后放开
        //          （子块门慢：实时 I 自带 −10 LU 相对门，旧的响素材的权重要降到新素材的
        //          约 1.5 % 以下，新素材的门块才进得了积分）；快遗忘取 10 s 时子块门要 75～90 s。测试把「一直放开」卡在 60 s 以内，
        //          并另跑「之前响 300 s」：三道门仍要在 60 s 内放开（拍板 5.2）。普通 breakdown 只比主体轻 10 LU 左右、
        //          过得了相对门，碰不到这条；每一路只看自己的数据，与宿主切块无关（同 ①）。
        //     House 门控那一份照旧用学习区间的参考（House 画像逐位不变）。实时帧门不再蕴含学习口径的帧门，所以一帧的频谱证据只算一次
        //     （computeEvidence），按各自的门分给各收方：学习区间、短期视图与 House 段照旧在学习口径的帧门之上才收，实时段在实时帧门之上才收。
        //   - 先试后用：实时模式开着时候选一直跑；实时口径有效的块进 live*，其中学习口径有效、交齐时还在学习的块同时进 trial*（第「五」节）。
        //     实时口径的组有效门用上面的实时参考。定稿要等先试后用的块交齐（与 House 档学习期间同一条），所以起点落在这一段里的有效块全都记进了
        //     它的 live*，与宿主分块无关。trial* 三项按 SegmentLedger.h 的约定只记学习期间：它们随学习的起止变，实时画像不读。
        //     开始学习时候选照旧从零状态重新起跑：重启那一刻还没交齐的块作废、之后 250 ms 的块不算（实时一侧每次开始学习有这个空档）。
        //   - 时间轴与 advanceTo（SegmentSink）：交给接收者的段与时刻都在「实时时间轴」上 = 分析器时间轴 + 一个偏移。分析器时间轴（IN 表的
        //     累计样本数）在表被 reset、分析器 reset / prepare 之后会从 0 重新数；那时偏移加大到「新时间轴的第一个样本 = 已报过的最晚时刻」，
        //     所以实时时间轴在分析器的一生里单调不减、首尾相接（段在交出去时才换算：段落账自己仍在分析器时间轴上）。processInput /
        //     processSpectrum 末尾各报一次 advanceTo (T)：T = 实时段落账此刻交齐的位置（定稿用的「四路都越过的时刻」，再与报过的取大）——
        //     end ≤ T 的段都已交出，之后交来的段 end > T。接收者按 T 安排自己的工作，结果只取决于段序列与 T，与宿主块长无关。
        //     就地定稿（不连续点、reset、prepare、关实时模式）之后报一次此刻（数据只送到这里，开着的段都截到这里交出去了）。
        //   - 断开：只在不连续点（跳播、回绕、宿主 reset、时间轴断开、子块长变了）。开着的段截到此刻（此刻之后的数据不会再归它）、
        //     就地定稿交出去，下一块重新锚定——交出去的段 [start, end) 首尾相接。上一次断开之后还没收过一块数据（例如读档后第一块之前的
        //     宿主 reset）时什么都不截，连续时间段号也不加：否则「读档后从头播」与「从头播」交来的段只差一个段号。重新聆听、clearHouseLedger、
        //     换曲风都不碰它（段不丢、不截）。reset / prepare / setLiveMode (false) 先同样就地定稿交出去再清。
        //   - 关实时模式时不丢最后一块的帧：链在下一块开头才把 Balance 的帧交给分析器（GlazeController::analyzeSpectrum），关的那一刻最后
        //     一块的帧还在 Balance 里。setLiveMode (false, &balance) 先把这些帧只交给实时段落账（帧流的核对与运行状态用一份副本，同一套函数），
        //     再就地定稿：通用画像、House 账与短期视图一个不碰——它们下一块开头照常收这些帧，编排器看到的与没开过实时模式逐位相同。
        //   - 宿主走带信息只按 SegmentLedger 现有的规则用（小节锚定、位置分格），不另加按走带触发的逻辑。
        constexpr double liveGateMemorySeconds = 180.0;   // 实时段落账三道相对门参考的遗忘时间常数（按进来的音频计，见上 ④）
        constexpr double liveGateQuietSeconds = 20.0;     // 有声却没过相对门比过了门净多出这么久，就加快遗忘（见上 ⑥；同实时画像换歌判定的 20 s）
        constexpr double liveGateFastMemorySeconds = 5.0;    // 加快之后的遗忘时间常数（见上 ⑥：轻 28 dB 的新素材约 1 分钟内过门）

        // 响度 / 直方图 / 立体声比值在 ProfileMath.h（与 LiveProfile 将要共用的那一份）。
        float toReading (double lufsOrDb) noexcept
        {
            // 数字静音的 −∞ 写成表的「没有读数」下限（有限数，存档与相减都安全）。
            return (float) juce::jmax ((double) LoudnessMeter::noReading, lufsOrDb);
        }

        //==============================================================================
        // 第「七」节：实时段落账三道相对门的参考（口径 ①～⑥ 见本文件开头第「七」节）。
        // 一路参考的遗忘（④⑥）：平时每并进一项，旧值乘 exp(−项长 ÷ liveGateMemorySeconds)；「没过相对门」比「过了」净多出 liveGateQuietSeconds
        // 之后，没过门的项改乘 exp(−项长 ÷ liveGateFastMemorySeconds)。项长随这一路定（帧 = hop、门块 = 子块长、组 = 组长）。
        struct LiveForgetting
        {
            double itemSeconds = 0.0, retain = 1.0, fastRetain = 1.0;   // 随项长定（setItemSeconds）
            double quietSeconds = 0.0;                                   // 没过门比过了门净多出的有声时长，夹在 [0, liveGateQuietSeconds]（随参考一起清）

            void setItemSeconds (double seconds) noexcept
            {
                itemSeconds = seconds;
                retain = std::exp (-seconds / liveGateMemorySeconds);
                fastRetain = std::exp (-seconds / liveGateFastMemorySeconds);
            }

            void clear() noexcept   { quietSeconds = 0.0; }

            // 一项要并进参考的项判完门之后调（passes = 过了相对门；还没有参考时算过）：更新净计，返回并进它时旧值的保留系数。
            double retainFor (bool passes) noexcept
            {
                if (passes)
                {
                    quietSeconds = juce::jmax (0.0, quietSeconds - itemSeconds);
                    return retain;
                }

                quietSeconds = juce::jmin (liveGateQuietSeconds, quietSeconds + itemSeconds);
                return quietSeconds >= liveGateQuietSeconds ? fastRetain : retain;
            }
        };

        // 按进来的项遗忘的长期均值（帧门：帧总功率；起音组有效门：组平均功率）。运行均值起步：第一项之后均值就是它自己。
        // weight ≥ 1、sum ≥ 绝对门 × weight（只并绝对门之上的项），不会成为次正规数。
        struct LiveMean
        {
            double sum = 0.0, weight = 0.0;

            bool valid() const noexcept   { return weight > 0.0; }
            double mean() const noexcept  { return sum / weight; }
            void clear() noexcept         { sum = weight = 0.0; }

            void add (double value, double retain) noexcept
            {
                sum = sum * retain + value;
                weight = weight * retain + 1.0;
            }
        };

        // 按进来的门块遗忘的 BS.1770 门控积分（子块有效门的「实时 I」）：同学习区间的 I（refreshIntegrated）——0.1 LU 一格的直方图，
        // 格里存门块能量与个数，相对门 = 全部门块的能量平均 − 10 LU（按格中心判在不在门上），I = 门上门块的能量平均——只是每并进一个门块，
        // 旧的全部乘 retain（只扫有数的格；权重落到 snapThreshold 以下的格归零：约 69 个时间常数的音频之后才发生，免得成为次正规数）。
        struct LiveIntegral
        {
            std::array<double, (size_t) histogramBins> energy {}, weight {};
            double totalEnergy = 0.0, totalWeight = 0.0;
            int lowBin = histogramBins, topBin = -1;   // 有数的格的范围（遗忘与求和只扫这一段）
            bool valid = false;
            double lufs = 0.0;

            void clear() noexcept
            {
                energy.fill (0.0);
                weight.fill (0.0);
                totalEnergy = totalWeight = 0.0;
                lowBin = histogramBins;
                topBin = -1;
                valid = false;
                lufs = 0.0;
            }

            // blockPower / blockLufs = 这个 400 ms 门块的 K 加权均方与响度（调用方已判过绝对门）。
            void add (double blockPower, double blockLufs, double retain) noexcept
            {
                for (int b = lowBin; b <= topBin; ++b)
                {
                    auto& w = weight[(size_t) b];
                    auto& e = energy[(size_t) b];
                    w *= retain;
                    e *= retain;

                    if (w < snapThreshold)
                        w = e = 0.0;
                }

                totalEnergy *= retain;
                totalWeight *= retain;

                const auto bin = histogramBinOf (blockLufs);
                energy[(size_t) bin] += blockPower;
                weight[(size_t) bin] += 1.0;
                totalEnergy += blockPower;
                totalWeight += 1.0;
                lowBin = juce::jmin (lowBin, bin);
                topBin = juce::jmax (topBin, bin);

                while (lowBin < topBin && weight[(size_t) lowBin] <= 0.0)
                    ++lowBin;

                const auto relativeGate = energyToLufs (totalEnergy / totalWeight) + integratedRelativeGateLu;
                double gatedEnergy = 0.0, gatedWeight = 0.0;

                for (int b = juce::jmax (lowBin, firstBinAtOrAbove (relativeGate)); b <= topBin; ++b)
                {
                    gatedEnergy += energy[(size_t) b];
                    gatedWeight += weight[(size_t) b];
                }

                valid = gatedWeight > 0.0;
                lufs = valid ? energyToLufs (gatedEnergy / gatedWeight) : 0.0;
            }
        };
    }

    //==============================================================================
    // 先试后用交齐的块由 ClayTrials 回调进来（BlockReceiver，第「五」节 trialBlockCompleted）。
    struct MusicAnalyzer::Impl final : private ClayTrials::BlockReceiver
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

        // 修坯 Clay 的真次声低通（样本流，第「四」节）：四级双二阶，系数按采样率算（转置直接 II 型，double；与先试后用的候选共用 ClayBiquad）。
        std::array<ClayBiquad::Coefficients, subStageQ.size()> subLowpass {};

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
        std::int64_t discardSubBlocksBefore = 0; // 起点早于它的子块丢弃（跨越不连续点）
        HostInfo host;

        // 帧流这一侧的核对与游标（processSpectrum 开头的 checkFrameTimeline；关实时模式时的补交用一份副本）。
        struct FrameClock
        {
            bool haveBalanceTime = false;
            std::int64_t lastBalanceSamples = 0;
            std::int64_t lastSpectrumInputSamples = 0;
            std::int64_t frameCursor = 0;
            std::int64_t discardFramesBefore = 0;    // 分析窗起点早于它的帧丢弃
        };

        FrameClock frameClock;

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
        // 帧流的运行状态（段电平的 1 s 平均、谱通量的参考与滑动均值）：每帧由 advanceFrameRun 推进，与谁收这一帧无关；
        // 关实时模式时的补交用一份副本、同一个函数推进（第「七」节）。
        struct FrameRun
        {
            std::array<double, (size_t) numBands> averagedPower {};
            std::int64_t framesSinceRestart = 0;
            bool havePreviousFrame = false;
            std::int64_t previousFrameEnd = 0;
            std::array<std::array<double, (size_t) numBands>, (size_t) fluxReferenceFrames> recentFramePower {};   // 前几帧的段功率（环形）
            int recentFrameWrite = 0;
            int recentFrameCount = 0;
            double fluxMean = 0.0;
            std::int64_t fluxFrames = 0;
            std::int64_t framesSinceFluxEvent = 0;

            // 时间平均与谱通量从下一帧重新开始（restartSpectrum 的这一半；环里的旧值不清，只按 recentFrameCount 读）。
            void restart() noexcept
            {
                averagedPower.fill (0.0);
                framesSinceRestart = 0;
                havePreviousFrame = false;
                recentFrameWrite = 0;
                recentFrameCount = 0;
                previousFrameEnd = 0;
                fluxMean = 0.0;
                fluxFrames = 0;
                framesSinceFluxEvent = 0;
            }
        };

        // 一帧推进运行状态之后、各收方要读的量（advanceFrameRun 填）。
        struct FrameStep
        {
            std::int64_t windowStart = 0;
            bool reachedPosition = false;     // 过了「跨越不连续点」那道检查：各份账的「帧流读到这里」可以推进
            bool restarted = false;           // 这一帧让帧流重新开始了（短期视图的平滑与证据跟着作废）
            std::array<double, (size_t) numBands> power {}, cross {};
            double total = 0.0;
            bool haveFlux = false, fluxEvent = false;
            double flux = 0.0;
        };

        // 一帧的频谱证据里与谁收无关的那一半（computeEvidence 填）：1 s 平均电平、逐段静音门、第一遍轮廓的 Huber 稳健权重、
        // 第二遍轮廓的残差。
        struct EvidenceFrame
        {
            std::array<double, (size_t) numBands> level {}, gate {}, weights {}, residual {};
            std::array<bool, (size_t) numBands> evaluated {};   // 第二遍拟合出了轮廓（residual 有效）
        };

        FrameRun frameRun;
        std::array<double, (size_t) numBands> livePositiveSeconds {};
        double liveTameEvidence = 0.0;

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
        std::array<ClayBiquad::State, subStageQ.size()> subState {};
        double groupSubEnergy = 0.0, groupMidEnergy = 0.0;   // 本组（还没完成）的 Σ 低通输出²、Σ M²
        double subEnergy = 0.0, midEnergy = 0.0;             // 学习区间内有效组的累计 E_sub、E_全

        //==============================================================================
        // 五、先试后用（样本流；学习期间跑，实时模式开着时一直跑）：候选与块记账在 ClayTrials（prepare 时分配，音频线程只清零），
        // 这里只留学习区间的累计——学习口径有效、交齐时还在学习的块：[0] 原始、[1 + c] 候选 c 各自最大的 topCount 个块峰值（降序），
        // 与块数一起足以求 P99（ClayTrials.h 第六节）；两个混合损失候选的两份能量之和（与 P99 同一批块、同一个容量上限）。
        ClayTrials trials;
        ClayTrials::TopSeries trialTop {};
        int trialCount = 0;                 // 记下的有效块数（封顶 ClayTrials::capacity）
        bool trialDirty = false;            // 记下了新块，画像的代价字段待重算
        ClayTrials::BlendSums trialBlendIntendedTotal {}, trialBlendOutputTotal {};

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
        // 六、House 档（第「六」节；通用档不读写下面任何一项）与 七、实时模式共用的「一份段落账的喂法」：三路数据各自读到哪里、
        // 起音组网格原点是否已知、正在进行的起音事件、各路找段的缓存。House 那一份与实时那一份各一个，口径相同。
        struct LedgerFeed
        {
            std::int64_t subBlockPosition = 0;    // 子块流读到的位置（最后一个子块的末尾）
            std::int64_t framePosition = 0;       // 帧流读到的位置（最后一帧分析窗起点 + hop）
            bool gridKnown = false;               // 上次重启以来已为这份账跑过样本（组网格原点已知）
            bool eventPending = false;            // 正在进行、要计入这份账的起音事件（触发时在有效门上）
            std::int64_t eventTime = 0;           // 它的触发组的起点
            int subHint = -1, frameHint = -1, onsetHint = -1, trialHint = -1;   // 各路找段的缓存

            // 段落账清空 / 断开：各路的位置从 at 重新数，进行中的事件作废，缓存作废（网格原点是否已知不在这里：随组网格重启）。
            void restart (std::int64_t at) noexcept
            {
                subBlockPosition = framePosition = at;
                eventPending = false;
                subHint = frameHint = onsetHint = trialHint = -1;
            }
        };

        Genre genre = Genre::general;
        bool houseActive = false;                 // isHouse (genre)
        HouseConstants houseConstants;
        SegmentLedger ledger;
        MusicProfile houseProfile;                // House 档的候选画像（getCandidateProfile 在 House 档返回它）
        AuxiliaryFeatures houseAux;
        bool houseDirty = false;                  // 段落账有新定稿的段，画像还没按它重算（不学习时留着，等 recomputeHouseProfile）
        std::uint32_t houseGeneration = 0;
        LedgerFeed houseFeed;
        std::int64_t onsetGridOrigin = 0;         // 起音组网格（也是先试后用的块网格）原点的分析器时间（House 档或实时模式开着时才更新）

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
            ClayTrials::TopSeries trialTop {};
            int trialCount = 0;
            ClayTrials::BlendSums blendIntended {}, blendOutput {};   // 合并进来的块的两份 K 加权能量（T-043）
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
        // 七、T-048 实时模式（第「七」节；关着时下面一项都不碰，实时段落账也不分配）
        bool liveMode = false;
        SegmentLedger liveLedger;                 // 实时段落账：打开实时模式与 prepare 时分配；定稿的段经 liveAxis 交给 liveSink
        LedgerFeed liveFeed;
        SegmentSink* liveSink = nullptr;          // 只存指针、不拥有（setSegmentSink；在处理之前设）
        bool liveEpochFed = false;                // 上一次断开 / 清空之后实时段落账收过一块数据（没收过的断开什么都不截，第「七」节「断开」）

        // 三道相对门的参考（第「七」节 ①～⑥）：与学习无关，随实时段落账一起清。
        LiveMean liveFrameReference;              // 帧门：绝对门之上的帧总功率
        LiveIntegral liveLoudnessReference;       // 子块有效门：绝对门之上的 400 ms 门块（实时 I）
        LiveMean liveOnsetReference;              // 起音组有效门：慢包络在绝对门之上的组的平均功率
        // 三路各自的遗忘（④⑥）：项长随帧流的 hop / 表的子块长 / 组长定（变了重设，连续偏轻的时长不清），连续偏轻的时长随参考一起清。
        LiveForgetting liveFrameForgetting, liveSubBlockForgetting, liveGroupForgetting;

        // 第「七」节「时间轴与 advanceTo」：把实时段落账交来的段换到单调的实时时间轴上再交给接收者，并报 advanceTo。
        struct LiveAxis final : SegmentSink
        {
            SegmentSink* target = nullptr;        // 真正的接收者（setSegmentSink）
            std::int64_t offset = 0;              // 实时时间 = 分析器时间 + offset（只在分析器时间轴回退时加大）
            std::int64_t reported = 0;            // 报过的最晚时刻（实时时间）：end 不晚于它的段都已交出
            SegmentLedger::Segment shifted {};    // 换算后的段（offset ≠ 0 时用；定长，不分配）

            void segmentFinalized (const SegmentLedger::Segment& segment) noexcept override
            {
                if (target == nullptr)
                    return;

                if (offset == 0)
                {
                    target->segmentFinalized (segment);
                    return;
                }

                shifted = segment;
                shifted.start += offset;
                shifted.end += offset;
                target->segmentFinalized (shifted);
            }

            // advanceTo：end ≤ analyzerTime（分析器时间）的段都已交出。报的时刻与报过的取大，所以单调不减；没有接收者时只记下来。
            void report (std::int64_t analyzerTime) noexcept
            {
                reported = juce::jmax (reported, analyzerTime + offset);

                if (target != nullptr)
                    target->advanceTo (reported);
            }

            // 分析器时间轴从 axisStart 起重新数（表被 reset；分析器 reset / prepare 之后第一块）：新时间轴的第一个样本不早于报过的最晚时刻。
            void rebase (std::int64_t axisStart) noexcept
            {
                if (axisStart + offset < reported)
                    offset = reported - axisStart;
            }
        };

        LiveAxis liveAxis;                        // 分析器的一生里一直用这一份（关 / 开实时模式、reset、prepare 都不清）

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

            // 第「七」节：实时参考每并进一组 / 一个门块（子块长随表，变了在 processInput 里重算）旧值的保留系数。
            liveGroupForgetting.setItemSeconds (groupSeconds);
            liveSubBlockForgetting.setItemSeconds ((double) subBlockLength / sampleRate);

            // 修坯 Clay 的真次声低通（截止频率封顶 0.45 fs 只防病态采样率；正常采样率下恒为 16 Hz）。
            const auto subFrequency = juce::jmin (subCutoffHz, 0.45 * sampleRate);

            for (size_t stage = 0; stage < subLowpass.size(); ++stage)
                subLowpass[stage] = ClayBiquad::lowpass (sampleRate, subFrequency, subStageQ[stage]);

            hop = 0;
            frameWindow = 0;
        }

        // prepare（消息线程，也在构造时；configure 之后）：先试后用的候选（ClayTrials：块网格与系数随采样率与组长，收单候选的线性相位
        // 低通、M / S 延迟环与两份守护随采样率定长）与段落账。之后音频线程上只清零，不再分配。
        void allocate()
        {
            trials.prepare (sampleRate, groupLength);

            // 阶段 2：House 门控那一份段落账退役，不再分配（约 1.8 MB）。分段只剩实时段落账。

            // T-048 实时段落账（第「七」节）：账本（512 段 × 约 4 KB ≈ 2 MB）只在实时模式开着时分配；关着时只有 SegmentLedger 自身的
            // 定长成员（8 个开着的段、门控直方图，约 50 KB，在 Impl 里）。
            if (liveMode)
                prepareLiveLedger();
        }

        void prepareLiveLedger()
        {
            liveLedger.setConstants (houseConstants);
            liveLedger.prepare (sampleRate, subBlockLength);
            connectLiveSink();
        }

        // 实时段落账定稿的段先经 liveAxis 换到实时时间轴再交给接收者；没有接收者时段落账不交。
        void connectLiveSink() noexcept
        {
            liveAxis.target = liveSink;
            liveLedger.setSink (liveSink != nullptr ? &liveAxis : nullptr);
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
            liveFrameForgetting.setItemSeconds (hopSeconds);
        }

        //==============================================================================
        void resetAll() noexcept
        {
            haveTime = false;
            now = 0;
            lastBlockSamples = 0;
            subBlockCursor = 0;
            frameClock = FrameClock {};
            discardSubBlocksBefore = 0;
            host = HostInfo {};
            learning = false;
            learnStart = 0;

            clearWindows();
            restartSpectrum();
            restartOnsets();
            clearLearning();
            clearHouse (0);

            // T-048：实时段落账回到空（开着的段已由调用方先就地定稿交出去：MusicAnalyzer::reset / prepare）。
            if (liveMode)
                clearLive (0);
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

            // 先试后用：记下的块清空；新的学习区间从零状态重新起跑（下一个样本前；实时模式开着、候选一直在跑时也重新起跑，
            // 所以学习区间里的候选状态与块网格和实时模式关着时逐位相同）。
            for (auto& top : trialTop)
                top.fill (0.0);

            trialCount = 0;
            trialBlendIntendedTotal.fill (0.0);
            trialBlendOutputTotal.fill (0.0);
            trialDirty = false;
            trials.requestRestart();

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
            frameRun.restart();
            restartLiveView();
        }

        // restartSpectrum 的另一半：短期视图的频谱项与它们的平滑状态（不进任何累计，只给保护层与界面）。
        void restartLiveView() noexcept
        {
            livePositiveSeconds.fill (0.0);
            liveTameEvidence = 0.0;
            invalidate (view.liveTameEvidenceDb);
            invalidate (view.liveSpectralSlopeDbPerOct);
            view.liveSlopeWeights.fill (0.0f);

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
            subState.fill (ClayBiquad::State {});
            groupSubEnergy = 0.0;
            groupMidEnergy = 0.0;

            // 先试后用的块网格随组网格一起从这里重新数；候选在下一个要跑的样本前从零状态重新起跑（清零在音频线程上做，
            // 候选不跑的时候不做）。已记下的有效块不动（同其他累计）。
            trials.requestRestart();

            // House 档（第「六」节）与实时段落账（第「七」节）：进行中的事件作废（与上面的事件状态一起），组网格的原点等下一个样本再定。
            houseFeed.eventPending = false;
            houseFeed.gridKnown = false;
            liveFeed.eventPending = false;
            liveFeed.gridKnown = false;
        }

        // 在时间戳 at 处断开：丢弃跨越它的全部窗口（子块、帧、包络）。已完成的累计（直方图、证据、事件数、有效时长）不动。
        void beginDiscontinuity (std::int64_t at) noexcept
        {
            discardSubBlocksBefore = at + (at > 0 ? discontinuityGuardSamples : 0);
            frameClock.discardFramesBefore = at;
            clearWindows();
            restartSpectrum();
            restartOnsets();

            // House 档：跨越不连续点的数据上面都丢了，段落账里没定稿的段已经交齐，就地定稿；下一块重新锚定。
            if (houseActive)
                houseEndEpoch (at);

            // 实时段落账同理（第「七」节）：开着的段截到此刻、就地定稿交出去。
            if (liveMode)
                liveEndEpoch (at);
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
                liveLedger.setSubBlockLength (subBlockLength);   // 实时段落账同理（没分配时也只存一个数）
                liveSubBlockForgetting.setItemSeconds ((double) subBlockLength / sampleRate);
                beginDiscontinuity (blockStart);
            }

            // 时间轴核对：本块之后表的累计样本数应 = 上一块之后 + 本块样本数。对不上就在本块起点断开：
            // 比上一块之后还少 = 表被 reset 过（游标归零、学习区间的起点挪到新时间轴上）；其余（漏调了几块、
            // 表这块没处理）只断开窗口，游标照旧（环形缓冲自己会报「丢过」）。
            // 第「七」节：分析器时间轴从新起点重新数时（构造 / reset / prepare 之后第一块、表被 reset），实时时间轴接在报过的最晚时刻之后。
            if (! haveTime)
            {
                haveTime = true;
                liveAxis.rebase (blockStart);

                if (meterNow != (std::int64_t) numSamples)
                    beginDiscontinuity (juce::jmax ((std::int64_t) 0, blockStart));   // 表比分析器早开始：之前的子块不属于这次分析
            }
            else if (meterNow != now + (std::int64_t) numSamples)
            {
                const auto meterWasReset = meterNow < now;

                if (meterWasReset)
                {
                    subBlockCursor = 0;
                    learnStart = juce::jmax ((std::int64_t) 0, blockStart);
                }

                beginDiscontinuity (juce::jmax ((std::int64_t) 0, blockStart));   // 实时段落账：开着的段截到旧的此刻交出去、报过了

                if (meterWasReset)
                    liveAxis.rebase (blockStart);
            }

            now = meterNow;
            lastBlockSamples = numSamples;

            // House 档：先按宿主拍点把这一块盖进段里（锚点与整数段边界，第「六」节），三路数据再各自找段。实时段落账同一做法（第「七」节）。
            if (houseActive)
                ledger.beginBlock (blockStart, numSamples, host);

            if (liveMode && numSamples > 0)
            {
                liveLedger.beginBlock (blockStart, numSamples, host);
                liveEpochFed = true;
            }

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

            if (liveMode)
                liveAdvance();
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

            // House 档与实时段落账：子块流读到这里了（丢掉的子块同样算读过：之后不会再有起点早于它的子块）。
            if (houseActive)
                houseFeed.subBlockPosition = juce::jmax (houseFeed.subBlockPosition, subBlock.endSample);

            if (liveMode)
                liveFeed.subBlockPosition = juce::jmax (liveFeed.subBlockPosition, subBlock.endSample);

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
                noteSubBlock (ledger, houseFeed, subBlock, start, effective, gatingPower, gatingLufs, haveShortTerm, shortTermPower, shortTermLufs);

            // 实时段落账（第「七」节）：同一口径归段，只是有效门的参考换成实时段落账自己的长期 I（与学习无关）——先按它判这个子块，
            // 再把这个门块并进参考（窗口不早于实时段落账的起点、在绝对门之上，同学习区间 I 的收法）。
            if (liveMode)
            {
                const auto liveEffective = gatingLufs >= absoluteGateLufs
                                           && (! liveLoudnessReference.valid || gatingLufs >= liveLoudnessReference.lufs + effectiveRelativeGateLu);
                noteSubBlock (liveLedger, liveFeed, subBlock, start, liveEffective, gatingPower, gatingLufs, haveShortTerm, shortTermPower,
                              shortTermLufs);

                if (windowCount >= gatingSubBlocks && subBlock.endSample - gatingSubBlocks * length >= liveLedger.getAcceptFrom()
                    && gatingLufs >= absoluteGateLufs)
                    liveLoudnessReference.add (gatingPower, gatingLufs, liveSubBlockForgetting.retainFor (liveEffective));
            }

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

            // 先试后用在学习期间跑、实时模式开着时一直跑（本块内不变：开始 / 停止学习、开关实时模式只发生在块之间）；
            // 单声道时 L、R 取同一个声道。
            const auto trialRunning = trialsRun() && channelsToUse > 0;
            const auto trialStereo = channelsToUse >= 2;

            // 按组切段：一段 = 到本组结束（或本块结束）为止。段内逐样本做起音功率与真次声；候选在跑时再把候选跑一遍这一段；组满再结组。
            // 每个样本仍是「起音 / 真次声 → 候选 → （组末）结组」的先后，与逐样本处理逐位相同，所以照样与块长无关。
            for (int segmentStart = 0; segmentStart < numSamples;)
            {
                const auto segmentLength = juce::jmin (numSamples - segmentStart, groupLength - groupFill);
                const auto segmentPosition = groupsSinceRestart * (std::int64_t) groupLength + groupFill;   // 从重启起算的样本序号

                // House 档与实时段落账：组网格（也是先试后用的块网格）原点的分析器时间 = 这一段首样本的时间 − 它在网格上的序号
                // （两次重启之间不变，两份账算出的是同一个数）。
                if (houseActive || liveMode)
                {
                    onsetGridOrigin = blockStart + segmentStart - segmentPosition;

                    if (houseActive)
                        houseFeed.gridKnown = true;

                    if (liveMode)
                        liveFeed.gridKnown = true;
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
                        low = ClayBiquad::run (low, subLowpass[stage], subState[stage]);

                    groupSubEnergy += low * low;
                    groupMidEnergy += mid * mid;
                }

                if (trialRunning)
                    trials.process (channels[0] + segmentStart, channels[trialStereo ? 1 : 0] + segmentStart, trialStereo,
                                    segmentLength, segmentPosition, *this);

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
                        trials.snapStates();
                }
            }
        }

        // 先试后用的候选此刻跑不跑（第「五」节）：学习期间，或实时模式开着。
        // 阶段 2：没有学习期。先试后用只在实时模式里跑，结果只进实时段落账的 live*。
        bool trialsRun() const noexcept   { return liveMode; }

        // 修坯 Clay 的样本流工具（第「四」节；双二阶的设计与运行在 ClayBiquad，ClayTrials.h）。
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
                noteTrialGroup (false, false);   // 重启预热里的组：先试后用的块同样不算（两种口径都不算）
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
                noteGroupInto (ledger, houseFeed, groupEnd - (std::int64_t) groupLength);

            // 实时段落账（第「七」节）：同一口径归段，有效门的参考换成实时段落账自己的长期组平均功率（与学习无关）。
            const auto liveEffective = liveMode && liveOnsetEffective (meanPower, groupEnd - (std::int64_t) groupLength);

            if (liveEffective)
                noteGroupInto (liveLedger, liveFeed, groupEnd - (std::int64_t) groupLength);

            // 先试后用：同一套边界决定这一组算不算（块里每个组都算，块才算）。学习口径还要在学习区间内、按学习区间的门；
            // 实时口径不看学习区间、按实时段落账的门（实时模式关着时不用，恒为否）。
            noteTrialGroup (inside && effective, liveEffective);

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

                        // House 档：事件归触发的那一组所在的段（在有效门上才计；事件结束前那一段不定稿）。实时段落账同理。
                        if (houseActive)
                        {
                            houseFeed.eventPending = effective;
                            houseFeed.eventTime = groupEnd - (std::int64_t) groupLength;
                        }

                        if (liveMode)
                        {
                            liveFeed.eventPending = liveEffective;
                            liveFeed.eventTime = groupEnd - (std::int64_t) groupLength;
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
            if (houseFeed.eventPending)
                noteEventInto (ledger, houseFeed);

            if (liveFeed.eventPending)
                noteEventInto (liveLedger, liveFeed);

            if (! (eventCounts && learning))
                return;

            const auto strengthDb = juce::jlimit (0.0, onsetStrengthCapDb, 10.0 * std::log10 (eventPeakRatio));
            const auto bin = juce::jlimit (0, strengthBins - 1, (int) std::floor (strengthDb / strengthBinDb));
            ++strengthCount[(size_t) bin];
            ++onsetEvents;
            onsetDirty = true;
        }

        //==============================================================================
        // 五、先试后用（样本流；第「五」节，候选与块记账在 ClayTrials）
        //==============================================================================
        // 原始一侧（processGroup 每组调一次）：本组按两种口径算不算；候选没在跑时不调（ClayTrials 自己在等着重新起跑时也不记）。
        void noteTrialGroup (bool countsLearning, bool countsLive) noexcept
        {
            if (trialsRun())
                trials.noteGroup (groupsSinceRestart, countsLearning, countsLive, *this);
        }

        // ClayTrials 每交齐一块调一次（按块号先后；第「五」节「交齐的块进哪里」）。
        void trialBlockCompleted (const ClayTrials::Block& block) noexcept override
        {
            // 阶段 2：学习累计与 House 账都退役。实时口径有效的块只进实时段落账的 live*（trial* / blend* 不再另记）。
            if (liveMode && block.liveValid)
                noteLedgerTrial (liveLedger, liveFeed, block);
        }

        //==============================================================================
        // 二、帧流
        //==============================================================================
        // 帧流这一轮的入口核对（processSpectrum 开头）。只读写 clock：真跑用 frameClock，关实时模式时的补交用它的副本（第「七」节）。
        struct FrameIntake
        {
            bool breakStream = false;             // 帧流要在 breakAt 断开
            std::int64_t breakAt = 0;
            std::int64_t offset = 0;              // 帧的时间戳换到表的时间轴要加的数
        };

        FrameIntake checkFrameTimeline (FrameClock& clock, const BalanceModule& balance) const noexcept
        {
            FrameIntake intake;
            const auto balanceNow = balance.getProcessedSamples();
            const auto inputAdvance = now - clock.lastSpectrumInputSamples;
            // 本轮频谱对应两次交帧之间的整个输入区间，不是最后一个统计切片。
            // 输入时间轴回退时 processInput 已确定新时间轴上的断点，不能再用旧时间戳或末片起点覆盖它。
            intake.breakAt = inputAdvance < 0 ? clock.discardFramesBefore
                                              : juce::jmax (clock.lastSpectrumInputSamples, clock.discardFramesBefore);

            // Balance 的累计样本数与表对不上：Balance 单独被 reset 过（链的 restartWithFadeIn 只清模块不清表）或漏调：
            // 帧流在本块起点断开；Balance 被 reset 时游标归零。帧的时间戳按「表 − Balance」的差换算到表的时间轴。
            // 输入可被统计层分片，而频谱每宿主块只读一次；比较两次读帧间的输入总推进量，不是最后一片长度。
            if (! clock.haveBalanceTime)
            {
                clock.haveBalanceTime = true;
                intake.breakStream = balanceNow != now;
            }
            else if (balanceNow < clock.lastBalanceSamples || inputAdvance < 0
                     || balanceNow != clock.lastBalanceSamples + inputAdvance)
            {
                if (balanceNow < clock.lastBalanceSamples)
                    clock.frameCursor = 0;

                intake.breakStream = true;
            }

            clock.lastBalanceSamples = balanceNow;
            clock.lastSpectrumInputSamples = now;
            intake.offset = now - balanceNow;
            return intake;
        }

        void processSpectrum (const BalanceModule& balance) noexcept
        {
            const auto intake = checkFrameTimeline (frameClock, balance);

            if (intake.breakStream)
                frameDiscontinuity (intake.breakAt);

            const auto hopNow = balance.getAnalysisHopSamples();
            const auto windowNow = balance.getAnalysisWindowSamples();

            if (hopNow <= 0 || windowNow <= 0)
            {
                // Balance 还没 prepare。实时段落账照样在 processSpectrum 末尾报一次交齐的位置（第「七」节「时间轴与 advanceTo」；
                // 帧流没有帧，交齐的位置与 processInput 末尾的相同）。
                if (liveMode)
                    liveAdvance();

                return;
            }

            if (hopNow != hop || windowNow != frameWindow)
            {
                updateFrameConstants (hopNow, windowNow);
                frameDiscontinuity (intake.breakAt);
            }

            for (int guard = 0; guard <= BalanceModule::frameRingCapacity / (int) frameScratch.size() + 1; ++guard)
            {
                const auto got = balance.readFramesSince (frameClock.frameCursor, frameScratch.data(), (int) frameScratch.size());
                const auto count = std::abs (got);

                if (got < 0)
                    restartSpectrum();

                for (int i = 0; i < count; ++i)
                    consumeFrame (frameScratch[(size_t) i], frameScratch[(size_t) i].endSample + intake.offset);

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

            if (liveMode)
                liveAdvance();
        }

        void frameDiscontinuity (std::int64_t at) noexcept
        {
            frameClock.discardFramesBefore = at;
            restartSpectrum();

            // House 档与实时段落账：分析窗早于 at 的帧不会再来了，帧流就算读到了 at。
            if (houseActive)
                houseFeed.framePosition = juce::jmax (houseFeed.framePosition, at);

            if (liveMode)
                liveFeed.framePosition = juce::jmax (liveFeed.framePosition, at);
        }

        // 一帧推进帧流的运行状态（与谁收这一帧无关的那一半：真跑推进 frameRun，关实时模式时的补交推进它的副本，第「七」节）。
        // 返回 false = 这一帧不用：分析窗跨越不连续点、或数值坏了（两种都让帧流重新开始）。各项的先后与算式与改前的 consumeFrame 相同；
        // 收方要读的量写进 step。
        bool advanceFrameRun (FrameRun& run, std::int64_t discardBefore, const BalanceModule::FrameSnapshot& frame,
                              std::int64_t end, FrameStep& step) const noexcept
        {
            step.windowStart = end - (std::int64_t) frameWindow;
            step.reachedPosition = false;
            step.restarted = false;

            if (step.windowStart < discardBefore)
            {
                run.restart();   // 分析窗跨越不连续点
                step.restarted = true;
                return false;
            }

            if (run.havePreviousFrame && end - run.previousFrameEnd != (std::int64_t) hop)
            {
                run.restart();   // 丢过帧：时间平均与谱通量都从这一帧重新开始
                step.restarted = true;
            }

            step.reachedPosition = true;
            auto& power = step.power;
            auto& cross = step.cross;
            double total = 0.0;

            for (size_t b = 0; b < (size_t) numBands; ++b)
            {
                power[b] = juce::jmax (0.0, (double) frame.bandPower[b]);
                total += power[b];
            }

            if (! std::isfinite (total))
            {
                run.restart();
                step.restarted = true;
                return false;
            }

            step.total = total;

            // 修坯 Clay：同一帧、同一段边界的左右交叉功率（FrameSnapshot::bandCross）。物理上 |X| ≤ P（逐 bin 有
            // |Re(L·R*)| ≤ (|L|² + |R|²) / 2，非负权重求和后照样成立），按它再钳一次：Balance 已钳过，这里只防读者一侧的舍入，
            // 也保证 P_M、P_S 都不为负。非有限值按 0（Balance 遇到非有限输入时本段功率也已记 0，钳到 [−0, 0] 同样是 0）。
            for (size_t b = 0; b < (size_t) numBands; ++b)
            {
                const auto measuredCross = (double) frame.bandCross[b];
                cross[b] = std::isfinite (measuredCross) ? juce::jlimit (-power[b], power[b], measuredCross) : 0.0;
            }

            // 各段功率域一阶平均（运行均值起步）。
            ++run.framesSinceRestart;
            const auto n = (double) run.framesSinceRestart;
            const auto retain = juce::jmin (spectrumRetain, (n - 1.0) / n);

            for (size_t b = 0; b < (size_t) numBands; ++b)
            {
                run.averagedPower[b] = power[b] + (run.averagedPower[b] - power[b]) * retain;
                snapToZero (run.averagedPower[b]);
            }

            // 辅助：250 Hz 以上段的正向谱通量（与上一帧相邻时才有；静音帧也算）。
            step.haveFlux = false;
            step.fluxEvent = false;
            step.flux = 0.0;

            if (run.havePreviousFrame)
            {
                double rise = 0.0, referenceTotal = 0.0;

                for (size_t b = (size_t) fluxFirstBand; b < (size_t) numBands; ++b)
                {
                    auto reference = 0.0;

                    for (int k = 0; k < run.recentFrameCount; ++k)
                        reference = juce::jmax (reference, run.recentFramePower[(size_t) k][b]);

                    rise += juce::jmax (0.0, power[b] - reference);
                    referenceTotal += reference;
                }

                const auto floorTotal = fluxFloorPowerPerBand * (double) (numBands - fluxFirstBand);
                const auto flux = 10.0 * std::log10 (1.0 + rise / juce::jmax (referenceTotal, floorTotal));

                step.haveFlux = true;
                step.flux = flux;
                ++run.fluxFrames;

                const auto warmedUp = run.fluxFrames > fluxWarmUpFrames;
                const auto threshold = juce::jmax (fluxMinimumDb, fluxThresholdRatio * run.fluxMean);
                step.fluxEvent = warmedUp && run.framesSinceFluxEvent >= fluxRefractoryFrames && flux > threshold;
                run.framesSinceFluxEvent = step.fluxEvent ? 0 : juce::jmin (run.framesSinceFluxEvent + 1, (std::int64_t) 1 << 30);

                // 滑动均值（指数加权、运行均值起步）。突起本身按门限截断后再进均值，免得几次大突起把门限抬走。
                const auto x = warmedUp ? juce::jmin (flux, threshold) : flux;
                const auto m = (double) run.fluxFrames;
                const auto r = juce::jmin (fluxMeanRetain, (m - 1.0) / m);
                run.fluxMean = x + (run.fluxMean - x) * r;
                snapToZero (run.fluxMean);
            }

            run.recentFramePower[(size_t) run.recentFrameWrite] = power;
            run.recentFrameWrite = (run.recentFrameWrite + 1) % fluxReferenceFrames;
            run.recentFrameCount = juce::jmin (run.recentFrameCount + 1, fluxReferenceFrames);
            run.previousFrameEnd = end;
            run.havePreviousFrame = true;
            return true;
        }

        void consumeFrame (const BalanceModule::FrameSnapshot& frame, std::int64_t end) noexcept
        {
            FrameStep step;
            const auto kept = advanceFrameRun (frameRun, frameClock.discardFramesBefore, frame, end, step);

            if (step.restarted)
                restartLiveView();   // 帧流重新开始：短期视图的频谱项同样从这一帧重新平滑（restartSpectrum 的另一半）

            // House 档与实时段落账：帧流读到这里了（之后的帧分析窗起点都不早于它）。
            if (step.reachedPosition)
            {
                if (houseActive)
                    houseFeed.framePosition = juce::jmax (houseFeed.framePosition, step.windowStart + (std::int64_t) hop);

                if (liveMode)
                    liveFeed.framePosition = juce::jmax (liveFeed.framePosition, step.windowStart + (std::int64_t) hop);
            }

            if (! kept)
                return;

            const auto& power = step.power;
            const auto& cross = step.cross;
            const auto total = step.total;
            const auto windowStart = step.windowStart;

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
                houseSegment = ledger.openSegmentAt (windowStart, houseFeed.frameHint);
                houseMusic = total >= frameAbsoluteGatePower
                          && (learnFrames == 0 || total >= frameRelativeGateRatio * learnTotalPowerSum / (double) learnFrames);
            }

            // 实时段落账（第「七」节）：自己的帧门与参考（与学习无关）；这一帧所在的段收通量计数，过了门的帧收长期段功率。
            auto liveMusic = false;
            auto* const liveSegment = liveMode ? takeLiveFrame (step, liveMusic) : nullptr;

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

            // 突起密度按学习区间内的全部帧计时（含静音帧），与包络事件同一段区间比较。
            if (inside && step.haveFlux)
            {
                ++learnFluxFrames;

                if (step.fluxEvent)
                    ++learnFluxEvents;

                spectrumDirty = true;
            }

            if (houseSegment != nullptr && step.haveFlux)   // House 档：同上，按段（含静音帧）
            {
                ++houseSegment->fluxFrames;

                if (step.fluxEvent)
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

                if (step.haveFlux)
                {
                    learnFluxSum += step.flux;
                    ++learnMusicFluxFrames;
                }

                spectrumDirty = true;
            }

            if (houseMusic && houseSegment != nullptr)   // House 档：同上，按段
                noteMusicFrame (*houseSegment, power, step.haveFlux, step.flux);

            // 频谱证据（长期画像按原来的学习边界累计；短期突出证据与学习状态无关，实时播放和离线渲染走同一路径）：一帧只算一次
            // （computeEvidence，不增加 FFT），按各自的门分给各收方。学习区间、短期视图与 House 段照旧在学习口径的帧门之上、1 s 平均
            // 已稳定时才收（与改前逐位相同）；实时段在实时帧门之上、已稳定时收（第「七」节）。
            const auto settled = frameRun.framesSinceRestart >= settleFrames;
            const auto viewEvidence = music && settled;
            auto* const houseEvidence = viewEvidence && houseMusic ? houseSegment : nullptr;
            auto* const liveEvidence = liveMusic && settled ? liveSegment : nullptr;

            if (viewEvidence || liveEvidence != nullptr)
            {
                EvidenceFrame evidence;
                computeEvidence (frameRun, evidence);

                // 修坯 Clay 的立体声画像（1.3）与频谱证据：学习区间里同一批帧、同一逐段静音门权重。
                if (viewEvidence && inside)
                {
                    accumulateStereo (power, cross, evidence.gate);
                    addEvidence (evidence, tameSum, recoverSum, deficitWeight, validWeight);
                }

                for (auto* segment : { houseEvidence, liveEvidence })   // House 档与实时段落账：同一份残差、同一权重，加进段里
                    if (segment != nullptr)
                        noteSegmentEvidence (*segment, power, cross, evidence);

                if (viewEvidence)
                    updateEvidenceView (evidence, power);
            }

            if (! viewEvidence)
            {
                livePositiveSeconds.fill (0.0);
                updateLiveTame (0.0);
                invalidate (view.liveSpectralSlopeDbPerOct);
            }
        }

        // 实时段落账收一帧（第「七」节 ①～④）：先按「实时帧均值」判帧门，再把这一帧并进参考（绝对门之上、分析窗不早于实时段落账的起点）；
        // 这一帧所在的段收通量计数（含静音帧，同 House 段），过了门的帧收长期段功率（noteMusicFrame）。真跑（consumeFrame）与关实时模式
        // 时的补交（deliverPendingFramesToLive）共用。返回这一帧所在的段（没有就是 nullptr）；music = 过了实时帧门。
        SegmentLedger::Segment* takeLiveFrame (const FrameStep& step, bool& music) noexcept
        {
            auto* const segment = liveLedger.openSegmentAt (step.windowStart, liveFeed.frameHint);
            const auto audible = step.total >= frameAbsoluteGatePower;
            music = audible && (! liveFrameReference.valid() || step.total >= frameRelativeGateRatio * liveFrameReference.mean());

            if (audible && step.windowStart >= liveLedger.getAcceptFrom())
                liveFrameReference.add (step.total, liveFrameForgetting.retainFor (music));

            if (segment != nullptr)
            {
                if (step.haveFlux)
                {
                    ++segment->fluxFrames;

                    if (step.fluxEvent)
                        ++segment->fluxEvents;
                }

                if (music)
                    noteMusicFrame (*segment, step.power, step.haveFlux, step.flux);
            }

            return segment;
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

            if (frameRun.framesSinceRestart >= settleFrames)
            {
                setFeature (view.liveTameEvidenceDb, liveTameEvidence, (double) frameRun.framesSinceRestart * hop / sampleRate);
                view.liveTameEvidenceDb.confidence = 1.0f;
            }
        }

        // 一帧的频谱证据里与谁收无关的那一半（第「二」节；consumeFrame 每帧至多算一次，关实时模式时的补交按副本算）：
        //   - 第一遍：邻段只按静音门加权；
        //   - Huber 降权：第一遍残差超出容忍区间的段，作为邻段时的权重按 容忍 / |r| 缩小；
        //   - 第二遍：按稳健权重重新拟合，残差 = 电平 − 轮廓（拟合不出的段不评估）。
        static void computeEvidence (const FrameRun& run, EvidenceFrame& e) noexcept
        {
            std::array<double, (size_t) numBands> firstResidual {};
            std::array<bool, (size_t) numBands> firstEvaluated {};

            for (size_t b = 0; b < (size_t) numBands; ++b)
            {
                e.level[b] = 10.0 * std::log10 (juce::jmax (1.0e-30, run.averagedPower[b]));
                e.gate[b] = juce::jlimit (0.0, 1.0, (e.level[b] - bandGateDb) / bandGateFadeDb);
            }

            for (int b = 0; b < numBands; ++b)
            {
                double contour = 0.0;
                const auto i = (size_t) b;
                firstEvaluated[i] = e.gate[i] > 0.0 && contourAt (b, e.level, e.gate, contour);
                firstResidual[i] = firstEvaluated[i] ? e.level[i] - contour : 0.0;
            }

            for (size_t b = 0; b < (size_t) numBands; ++b)
            {
                const auto magnitude = std::abs (firstResidual[b]);
                e.weights[b] = e.gate[b] * (magnitude > toleranceDb ? toleranceDb / magnitude : 1.0);
            }

            for (int b = 0; b < numBands; ++b)
            {
                double contour = 0.0;
                const auto i = (size_t) b;
                e.evaluated[i] = firstEvaluated[i] && contourAt (b, e.level, e.weights, contour);
                e.residual[i] = e.evaluated[i] ? e.level[i] - contour : 0.0;
            }
        }

        // 一帧的证据并进一份累计（学习区间，或 House / 实时的一段）：超出 ±2 dB 容忍区间的正 / 负偏离按本段静音门权重分别累计，
        // 不互相抵消；缺失帧的权重与评估过的权重另记（Recover 的「持续缺失」系数用）。
        static void addEvidence (const EvidenceFrame& e, std::array<double, (size_t) numBands>& tame,
                                 std::array<double, (size_t) numBands>& recover, std::array<double, (size_t) numBands>& deficit,
                                 std::array<double, (size_t) numBands>& valid) noexcept
        {
            for (size_t i = 0; i < (size_t) numBands; ++i)
            {
                if (! e.evaluated[i])
                    continue;

                const auto residual = e.residual[i];
                const auto positive = juce::jmax (0.0, residual - toleranceDb);
                tame[i] += e.gate[i] * positive;
                recover[i] += e.gate[i] * juce::jmax (0.0, -residual - toleranceDb);

                if (residual < -toleranceDb)
                    deficit[i] += e.gate[i];

                valid[i] += e.gate[i];
            }
        }

        // House 段 / 实时段收一帧的立体声与频谱证据（同学习区间的 accumulateStereo + addEvidence，同一份残差、同一权重）。
        static void noteSegmentEvidence (SegmentLedger::Segment& segment, const std::array<double, (size_t) numBands>& currentPower,
                                         const std::array<double, (size_t) numBands>& currentCross, const EvidenceFrame& e) noexcept
        {
            accumulateSegmentStereo (segment, currentPower, currentCross, e.gate);
            addEvidence (e, segment.tameSum, segment.recoverSum, segment.deficitWeight, segment.validWeight);
        }

        // 短期视图的频谱项（只在学习口径的帧门之上、1 s 平均已稳定的帧更新，同改前）：门上宽带内容的斜率、持续突出的 Tame 证据。
        void updateEvidenceView (const EvidenceFrame& e, const std::array<double, (size_t) numBands>& currentPower) noexcept
        {
            const auto& averaged = frameRun.averagedPower;

            // 用同一份电平与稳健权重估计宽带斜率。至少 16 段、跨 4 oct，且当下仍有能量；
            // 独奏窄音、已结束的镲片和静音尾巴不能被当作整首歌的明暗。只做 27 段回归，无新 FFT。
            double sw = 0.0, sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
            int slopeBands = 0, firstSlopeBand = numBands, lastSlopeBand = 0;
            for (int b = 0; b < numBands; ++b)
            {
                const auto i = (size_t) b;
                view.liveSlopeWeights[i] = 0.0f;
                if (e.gate[i] < 0.5 || currentPower[i] < averaged[i] * liveTamePresentRatio)
                    continue;
                const auto w = e.weights[i];
                view.liveSlopeWeights[i] = (float) w;
                const auto x = (double) b / 3.0;
                sw += w; sx += w * x; sy += w * e.level[i]; sxx += w * x * x; sxy += w * x * e.level[i];
                ++slopeBands;
                firstSlopeBand = juce::jmin (firstSlopeBand, b);
                lastSlopeBand = b;
            }
            const auto denominator = sw * sxx - sx * sx;
            if (slopeBands >= 16 && lastSlopeBand - firstSlopeBand >= 12 && sw >= 12.0 && denominator > 1.0e-9)
            {
                setFeature (view.liveSpectralSlopeDbPerOct, (sw * sxy - sx * sy) / denominator,
                            (double) frameRun.framesSinceRestart * hop / sampleRate);
                view.liveSpectralSlopeDbPerOct.confidence = (float) juce::jlimit (0.0, 1.0, sw / 20.0);
            }
            else
                invalidate (view.liveSpectralSlopeDbPerOct);

            // 持续突出（正偏离持续满 liveTamePersistenceSeconds、此刻仍有能量）的均方根 → 短期 Tame 证据。
            double liveSumSquares = 0.0, liveWeights = 0.0;
            const auto hopSeconds = (double) hop / sampleRate;
            for (size_t i = 0; i < (size_t) numBands; ++i)
            {
                if (! e.evaluated[i])
                {
                    livePositiveSeconds[i] = 0.0;
                    continue;
                }

                const auto positive = juce::jmax (0.0, e.residual[i] - toleranceDb);

                if (positive > 0.0 && currentPower[i] >= averaged[i] * liveTamePresentRatio)
                    livePositiveSeconds[i] = juce::jmin (liveTamePersistenceSeconds, livePositiveSeconds[i] + hopSeconds);
                else
                    livePositiveSeconds[i] = 0.0;

                if (livePositiveSeconds[i] >= liveTamePersistenceSeconds)
                    liveSumSquares += e.gate[i] * positive * positive;
                liveWeights += e.gate[i];
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
            double lowCrossNow = 0.0, lowPowerNow = 0.0, lowCorePowerNow = 0.0, highMidNow = 0.0, highSideNow = 0.0;

            for (int b = lowStereoFirstBand; b <= lowStereoLastBand; ++b)
            {
                const auto i = (size_t) b;

                if (power[i] >= liveBandGatePower)
                {
                    lowCrossNow += cross[i];
                    lowPowerNow += power[i];

                    if (b <= lowStereoCoreLastBand)
                        lowCorePowerNow += power[i];
                }
            }

            // 泄漏门（BUG-009）：核心门上功率低于上方段 6 ～ 8 的 −35 dB → 低频组只剩上方内容漏进来的部分（段 5 的格子与 Hann 主瓣），
            // 本帧按「没有低频内容」算，同整组在门下。核心与上方都是 0 时不算泄漏。
            double leakageSourcePower = 0.0;

            for (int b = lowLeakageFirstBand; b <= lowLeakageLastBand; ++b)
                leakageSourcePower += power[(size_t) b];

            if (lowCorePowerNow < lowLeakageRatio * leakageSourcePower)
                lowCrossNow = lowPowerNow = 0.0;

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
                    // bandStereoConfidence。ADR-015 起自动收单恒为中性，决策器不再用逐段相关度选分频点；可信度 0 的段仍然不能当成「很宽」。
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

        // 先试后用（第「五」节）：代价字段只由记下的有效块按纯函数算出（ClayTrials::costsOf，与 House 画像同一份）。每记下一块重算一次
        // （至多每 100 ms 音频一次，O(候选数)）。置信度在 finalizeConfidences 里按 min(观测充分度, 代表性) 给。
        void updateTrialProfile() noexcept
        {
            trialDirty = false;
            applyTrialCosts (ClayTrials::costsOf (trialTop, trialBlendIntendedTotal, trialBlendOutputTotal, trialCount,
                                                  trials.getBlockLength(), sampleRate),
                             profile, aux, measuredTrial);
        }

        // 一份代价写进画像的同名字段与辅助量的基准 P99（通用画像与 House 画像共用）。
        static void applyTrialCosts (const ClayTrials::Costs& costs, MusicProfile& target, AuxiliaryFeatures& targetAux, float& measured) noexcept
        {
            targetAux.trialPeakP99Db = costs.originalP99Db;
            target.lowCutPeakCostDb = costs.lowCutPeakCostDb;
            target.monoPeakCostDb = costs.monoPeakCostDb;
            target.widthPeakCostDb = costs.widthPeakCostDb;
            target.lowCutBlendLossDb = costs.blendLossDb;
            measured = costs.measured;
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
            for (auto& loss : profile.lowCutBlendLossDb)
                finalize (loss, measuredTrial);
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
            houseFeed.restart (acceptFrom);

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

            houseFeed.restart (at);
        }

        // 一份段落账（House 那一份或实时那一份）此刻四路数据都已越过的时刻：段尾不晚于它的段，数据已经交齐（第「六」节「定稿」）。
        // trialsCount = 先试后用的块要不要等（House 那一份：学习中；实时那一份：一直要）。
        std::int64_t completeBefore (const LedgerFeed& feed, bool trialsCount) const noexcept
        {
            // 子块：最后读到的子块末尾；表的响度计量关着（两个子块长都没有新子块）时按此刻 − 两个子块，免得段永远定不了稿。
            auto position = juce::jmax (feed.subBlockPosition, now - 2 * (std::int64_t) subBlockLength);

            // 帧：Balance 还没 prepare（hop 未知）时不等它。
            if (hop > 0)
                position = juce::jmin (position, feed.framePosition);

            // 起音：正在进行的事件归它触发的那一段；否则当前组的起点。
            if (feed.eventPending)
                position = juce::jmin (position, feed.eventTime);
            else if (feed.gridKnown)
                position = juce::jmin (position, onsetGridOrigin + groupsSinceRestart * (std::int64_t) groupLength);

            // 先试后用（候选在跑）：第 k 块的最后一笔在网格位置 (k + 1)·块长 + 最长延迟交齐，还没交齐的最早一块的起点。
            if (trialsCount && ! trials.isRestartPending() && feed.gridKnown)
                position = juce::jmin (position, onsetGridOrigin
                                                     + trials.pendingBlockStart (groupsSinceRestart * (std::int64_t) groupLength + groupFill));

            return position;
        }

        // 每次 processInput / processSpectrum 末尾：定稿交齐的段；学习中有新定稿就重算画像（不学习时留着，等 recomputeHouseProfile）。
        void houseAdvance() noexcept
        {
            // T-038 评审 F3：只在账本真变了时置脏（新存进一段或覆盖了旧段）；没存进的段（静音、账本满、重放不如旧值）不触发 O(段数) 的重算。
            const auto before = ledger.getStoredRevision();
            ledger.finalizeBefore (completeBefore (houseFeed, learning));

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

        // ---- 归段（三路数据各自调；口径与通用画像的同一处累计相同。target / feed = House 那一份或实时那一份段落账）----

        // 子块（consumeSubBlock）：子块数、有效子块的 K 加权能量、真峰值；门块功率与 3 s 短期响度随产生它们的子块，窗口最早的样本
        // 早于段落账的起点就不收（同学习区间）。
        void noteSubBlock (SegmentLedger& target, LedgerFeed& feed, const LoudnessMeter::SubBlock& subBlock, std::int64_t start,
                           bool effective, double gatingPower, double gatingLufs, bool haveShortTerm, double shortTermPower,
                           double shortTermLufs) noexcept
        {
            auto* segment = target.openSegmentAt (start, feed.subHint);

            if (segment == nullptr)
                return;

            const auto length = (std::int64_t) subBlockLength;
            const auto acceptFrom = target.getAcceptFrom();
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
        void noteMusicFrame (SegmentLedger::Segment& segment, const std::array<double, (size_t) numBands>& power,
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

        // 立体声（noteSegmentEvidence 里，与频谱证据同一批帧、同一逐段静音门权重；同 accumulateStereo）。
        static void accumulateSegmentStereo (SegmentLedger::Segment& segment, const std::array<double, (size_t) numBands>& currentPower,
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
        void noteGroupInto (SegmentLedger& target, LedgerFeed& feed, std::int64_t groupStart) noexcept
        {
            auto* segment = target.openSegmentAt (groupStart, feed.onsetHint);

            if (segment == nullptr)
                return;

            segment->onsetEffectiveSamples += groupLength;
            segment->subEnergy += groupSubEnergy;
            segment->midEnergy += groupMidEnergy;
        }

        // 起音事件结束（finishEvent）：计入触发的那一组所在的段；强度的格同 finishEvent。
        void noteEventInto (SegmentLedger& target, LedgerFeed& feed) noexcept
        {
            feed.eventPending = false;
            auto* segment = target.openSegmentAt (feed.eventTime, feed.onsetHint);

            if (segment == nullptr || segment->strengthCount >= SegmentLedger::maxEvents)
                return;

            const auto strengthDb = juce::jlimit (0.0, onsetStrengthCapDb, 10.0 * std::log10 (eventPeakRatio));
            const auto bin = juce::jlimit (0, strengthBins - 1, (int) std::floor (strengthDb / strengthBinDb));
            segment->strengthBin[(size_t) segment->strengthCount++] = (std::uint8_t) bin;
            ++segment->onsetEvents;
        }

        // 先试后用交齐的块：按块起点归进实时段落账，只记 live*（阶段 2 把 trial* / blend* / live* 并成这一组）。
        void noteLedgerTrial (SegmentLedger& target, LedgerFeed& feed, const ClayTrials::Block& block) noexcept
        {
            const auto time = onsetGridOrigin + block.index * (std::int64_t) trials.getBlockLength();
            auto* segment = target.openSegmentAt (time, feed.trialHint);

            if (segment == nullptr)
                return;

            ClayTrials::accumulate (segment->liveTrialTop, segment->liveBlendIntended, segment->liveBlendOutput,
                                    segment->liveTrialBlocks, block);
            ++segment->liveTrialBlocks;
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
            s.blendIntended.fill (0.0);
            s.blendOutput.fill (0.0);
        }

        // 先试后用的块：按格序整段整段地取（满 ClayTrials::capacity 块为止），每一路合并各段最大的 8 个（第「六」节）。
        void mergeHouseTrial (bool fullOnly) noexcept
        {
            auto& s = houseSums;

            for (auto& top : s.trialTop)
                top.fill (0.0);

            s.trialCount = 0;
            s.blendIntended.fill (0.0);
            s.blendOutput.fill (0.0);
            auto inserted = 0;   // 已插入的个数（insertTop 的 filled 口径；每一路相同）

            for (int i = 0; i < ledger.getNumStored(); ++i)
            {
                const auto& segment = ledger.getStored (i);

                if ((fullOnly && ! segment.full) || segment.trialBlocks <= 0)
                    continue;

                if (s.trialCount + segment.trialBlocks > ClayTrials::capacity)
                    break;

                const auto values = juce::jmin (segment.trialBlocks, ClayTrials::topCount);

                for (int k = 0; k < values; ++k)
                {
                    for (size_t series = 0; series < s.trialTop.size(); ++series)
                        ClayTrials::insertTop (s.trialTop[series], inserted, segment.trialTop[series][(size_t) k]);

                    ++inserted;
                }

                for (size_t c = 0; c < (size_t) ClayTrials::numBlend; ++c)   // 混合损失与峰值代价同一批块（同通用画像：同一批有效块）
                {
                    s.blendIntended[c] += segment.blendIntended[c];
                    s.blendOutput[c] += segment.blendOutput[c];
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

            if ((double) s.trialCount * (double) trials.getBlockLength() / sampleRate < ClayTrials::minimumSeconds)
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

        // 同 updateTrialProfile（同一份 ClayTrials::costsOf；块数 = 合并进来的块数，满编段，不够 10 s 时全部段）。
        void houseTrialProfile() noexcept
        {
            const auto& s = houseSums;
            applyTrialCosts (ClayTrials::costsOf (s.trialTop, s.blendIntended, s.blendOutput, s.trialCount, trials.getBlockLength(), sampleRate),
                             houseProfile, houseAux, houseMeasured.trial);
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
            for (auto& loss : p.lowCutBlendLossDb)
                finalize (loss, m.trial);
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

        //==============================================================================
        // 七、T-048 实时模式（口径见本文件开头第「七」节）。只在 liveMode 时调。
        //==============================================================================
        // 实时段落账回到空（不分配；开着的段由调用方先 liveEndEpoch 交出去）。各路的位置从 acceptFrom 重新数；三道门的参考一起清
        // （第「七」节 ⑤）。实时时间轴（liveAxis）不清：它在分析器的一生里单调不减。
        void clearLive (std::int64_t acceptFrom) noexcept
        {
            liveLedger.reset (acceptFrom);
            liveFeed.restart (acceptFrom);
            liveEpochFed = false;
            liveFrameReference.clear();
            liveLoudnessReference.clear();
            liveOnsetReference.clear();
            liveFrameForgetting.clear();
            liveSubBlockForgetting.clear();
            liveGroupForgetting.clear();
        }

        // 在 at 处断开：开着的段截到此刻（now：数据只送到这里，此刻之后的块归新段），就地定稿交给接收者，下一块重新锚定——
        // 交出去的 [start, end) 首尾相接（SegmentLedger::endEpochAt；House 那一份照旧用不截段尾的 endEpoch）。之后报一次此刻：
        // 开着的段都截到这里交出去了。
        // 上一次断开 / 清空之后还没收过一块数据、新起点也不早于账本现在的起点时什么都不做（没有开着的段，连续时间段号不加，第「七」节
        // 「断开」）：账本的其余状态与就地定稿之后相同（还没锚定），起点不挪也一样——跨越 at 的窗口在分析器这边已经全丢了
        // （调用方都先 clearWindows），不会有早于 at 的数据进来。
        void liveEndEpoch (std::int64_t at) noexcept
        {
            if (liveEpochFed || at < liveLedger.getAcceptFrom())
            {
                liveLedger.endEpochAt (now, at);
                liveEpochFed = false;
            }

            liveFeed.restart (at);
            liveAxis.report (now);
        }

        // 每次 processInput / processSpectrum 末尾：定稿交齐的段（一直等先试后用的块交齐：候选在实时模式下一直跑），
        // 再把交齐的位置报给接收者（advanceTo：end 不晚于它的段此刻都已交出）。
        void liveAdvance() noexcept
        {
            const auto complete = completeBefore (liveFeed, true);
            liveLedger.finalizeBefore (complete);
            liveAxis.report (complete);
        }

        // 起音组的实时有效门（第「七」节 ①～④；processGroup 在重启预热之后每组调一次）：慢包络在绝对门之上、且不低于实时组平均功率
        // 20 dB 以上（同学习区间的门，只换参考）。先判门，再把这一组并进参考（慢包络在绝对门之上、组起点不早于实时段落账的起点）。
        bool liveOnsetEffective (double meanPower, std::int64_t groupStart) noexcept
        {
            const auto audible = slow >= onsetSlowFloorPower;
            const auto passes = audible && (! liveOnsetReference.valid()
                                            || slow >= onsetEffectiveRelativeRatio * liveOnsetReference.mean());

            if (audible && groupStart >= liveLedger.getAcceptFrom())
                liveOnsetReference.add (meanPower, liveGroupForgetting.retainFor (passes));

            return passes;
        }

        // 关实时模式时不丢最后一块的帧（第「七」节）：balance 已发布、分析器还没读的帧只交给实时段落账。帧流的核对与运行状态用副本、
        // 同一套函数（checkFrameTimeline / advanceFrameRun / takeLiveFrame / computeEvidence），通用画像、House 账与短期视图一个不碰——
        // 它们下一块开头照常读到这些帧（游标也是副本）。消息线程、不与处理并发（setLiveMode）。
        // Balance 的 hop / 窗长与分析器记的不同 = 它在上一次交帧之后重新 prepare 过：环里只有 prepare 之后还没处理过一块的空状态，不交。
        void deliverPendingFramesToLive (const BalanceModule& balance) noexcept
        {
            auto clock = frameClock;
            auto run = frameRun;
            const auto intake = checkFrameTimeline (clock, balance);

            if (intake.breakStream)
            {
                clock.discardFramesBefore = intake.breakAt;
                run.restart();
                liveFeed.framePosition = juce::jmax (liveFeed.framePosition, intake.breakAt);
            }

            if (hop <= 0 || balance.getAnalysisHopSamples() != hop || balance.getAnalysisWindowSamples() != frameWindow)
                return;

            for (int guard = 0; guard <= BalanceModule::frameRingCapacity / (int) frameScratch.size() + 1; ++guard)
            {
                const auto got = balance.readFramesSince (clock.frameCursor, frameScratch.data(), (int) frameScratch.size());
                const auto count = std::abs (got);

                if (got < 0)
                    run.restart();

                for (int i = 0; i < count; ++i)
                {
                    const auto& frame = frameScratch[(size_t) i];
                    FrameStep step;
                    const auto kept = advanceFrameRun (run, clock.discardFramesBefore, frame, frame.endSample + intake.offset, step);

                    if (step.reachedPosition)
                        liveFeed.framePosition = juce::jmax (liveFeed.framePosition, step.windowStart + (std::int64_t) hop);

                    if (! kept)
                        continue;

                    auto music = false;
                    auto* const segment = takeLiveFrame (step, music);

                    if (segment != nullptr && music && run.framesSinceRestart >= settleFrames)
                    {
                        EvidenceFrame evidence;
                        computeEvidence (run, evidence);
                        noteSegmentEvidence (*segment, step.power, step.cross, evidence);
                    }
                }

                if (count < (int) frameScratch.size())
                    break;
            }
        }
    };

    //==============================================================================
    MusicAnalyzer::MusicAnalyzer() : impl (std::make_unique<Impl>()) {}
    MusicAnalyzer::~MusicAnalyzer() = default;

    void MusicAnalyzer::prepare (const juce::dsp::ProcessSpec& spec)
    {
        // T-048：实时段落账里开着的段先就地定稿交出去（按旧采样率折算；重新分配会丢掉它们）。
        if (impl->liveMode)
            impl->liveEndEpoch (impl->now);

        impl->configure (spec.sampleRate, (int) spec.numChannels);
        impl->allocate();   // 先试后用随采样率定长的部分（收单候选低通、M / S 延迟环）与段落账：只在这里分配
        impl->resetAll();
    }

    void MusicAnalyzer::reset()
    {
        // T-048：实时段落账里开着的段先就地定稿交出去，再清。
        if (impl->liveMode)
            impl->liveEndEpoch (impl->now);

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
        // 阶段 2：聆听 / 候选画像退役。保留空函数，编排器（代理 A）删掉调用之前这里什么都不做，
        // 也不再让先试后用重新起跑（阶段 1 遗留 6 的 250 ms 空档随之消失）。
    }

    void MusicAnalyzer::stopLearning() noexcept
    {
    }

    bool MusicAnalyzer::isLearning() const noexcept
    {
        return false;
    }

    // 候选画像不再存在。返回的是一份保持默认（各特征 valid = false）的画像，给还没改完的调用方。
    const MusicProfile& MusicAnalyzer::getCandidateProfile() const noexcept     { return impl->profile; }
    const ShortTermView& MusicAnalyzer::getShortTermView() const noexcept       { return impl->view; }
    const AuxiliaryFeatures& MusicAnalyzer::getAuxiliaryFeatures() const noexcept { return impl->aux; }

    //==============================================================================
    // T-038 House 档
    void MusicAnalyzer::setGenre (Genre genre) noexcept
    {
        // 阶段 2：曲风只存下来给调用方读。不再另挂 House 门控账，也不因换档重算画像。
        impl->genre = genre;
        impl->houseActive = false;
    }

    Genre MusicAnalyzer::getGenre() const noexcept   { return impl->genre; }

    void MusicAnalyzer::setHouseConstants (const HouseConstants& constants) noexcept
    {
        impl->houseConstants = constants;
        impl->liveLedger.setConstants (constants);   // 实时段落账的分段常数（门控常数它不再用来重算全账）
    }

    void MusicAnalyzer::recomputeHouseProfile() noexcept
    {
        // 阶段 2：House 画像重算退役。
    }

    void MusicAnalyzer::clearHouseLedger() noexcept
    {
        // 阶段 2：没有 House 门控账。实时段落账不在这里清（宿主 reset / 重新聆听都不断开它）。
    }

    const SegmentLedger& MusicAnalyzer::getSegmentLedger() const noexcept   { return impl->liveLedger; }
    std::uint32_t MusicAnalyzer::getHouseProfileGeneration() const noexcept  { return 0; }

    //==============================================================================
    // T-048 实时模式（文件头第「七」节）
    void MusicAnalyzer::setLiveMode (bool enabled, const BalanceModule* pendingFrames)
    {
        auto& s = *impl;

        if (enabled == s.liveMode)
            return;

        if (! enabled)
        {
            // 链在下一块开头才交帧：先把 pendingFrames 已发布、还没读的帧只交给实时段落账（第「七」节；别的收方下一块照常收）。
            // 再把开着的段就地定稿交给接收者、报到此刻。不在学习时候选从此不跑（下次开始学习、或再打开实时模式时从零状态重新起跑）。
            if (pendingFrames != nullptr)
                s.deliverPendingFramesToLive (*pendingFrames);

            s.liveEndEpoch (s.now);
            s.liveMode = false;
            return;
        }

        s.liveMode = true;
        s.prepareLiveLedger();      // 分配（消息线程），连同常数、子块长与接收者
        s.clearLive (s.now);        // 此刻之前的窗口不收
        s.liveFeed.gridKnown = false;

        // 候选原来没在跑（不在学习：状态是旧的）就从零状态起跑；在跑（学习中）就接着跑，学习累计与块网格不受影响。
        if (! s.learning)
            s.trials.requestRestart();
    }

    bool MusicAnalyzer::isLiveMode() const noexcept   { return impl->liveMode; }

    void MusicAnalyzer::setSegmentSink (SegmentSink* sink) noexcept
    {
        impl->liveSink = sink;
        impl->connectLiveSink();
    }
}

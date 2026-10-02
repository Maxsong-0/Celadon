/*
  ==============================================================================

    MusicAnalyzer.h
    GLAZE 分析器（设计规格-GLAZE-分析器）：在信号链入口测整首歌的特征，产出画像与短期视图。

      - 只读音频，不改变任何处理结果；
      - 入口响度与真峰值复用链里的 IN 表（同一测点不重复计算），频谱复用 Balance 每帧原始段功率；
      - 全部状态固定容量，prepare 里分配，音频线程不分配、不加锁；
      - 时间一律按新增有效音频样本计。

    学习由编排器控制：startLearning 清空候选累计并开始累计；stopLearning 停止累计（保留结果）。
    不学习时短期视图照常更新（保护层与界面要用）。

    ---- 实现口径（v0.6 A1；详细理由见 MusicAnalyzer.cpp 各节注释）----

    三路数据各自按自己的时间戳独立累计，结果与宿主块长无关（同一段音频、任意分块，画像逐位相同）：
      - 子块流（IN 表的 100 ms 子块）：M / S / I / LRA / 真峰值 / PLR / PSR、有效时长、代表性；
      - 帧流（Balance 每帧原始段功率，约 10 ms 一帧）：长期段电平、频谱正负证据、斜率、低 / 高频占比、辅助谱通量；
      - 样本流（链入口原始样本）：起音包络（功率域、按约 0.33 ms 一组的固定网格）。
    时间轴 = IN 表的累计样本数；Balance 的帧按「本块两者的样本数之差」换算到同一时间轴，所以 Balance 单独被 reset
    （链的 restartWithFadeIn 只清模块、不清表）也对得上。每块必须先 processInput、再 processSpectrum。
    分析器自己核对两边的累计样本数：对不上（对方被 reset、或漏调了几块）就在本块起点当作一个不连续点处理。

    学习区间从 startLearning 那一刻（上一块末尾的时间戳）起按样本精确划分：窗口最早的样本早于这一刻的数据不计入；
    stopLearning 按到达立即冻结（「停止后固定」）。

    「有效音频」（有效时长、代表性、短期视图的 musicActive）：400 ms 响度（不足 4 个连续子块时用已有的）
    ≥ −70 LUFS，且不低于学习区间的 I 20 LU 以上（与 LRA 同一相对门；还没有 I 时只用绝对门）。

    置信度 = min(本特征的观测充分度, 代表性)：
      - 观测充分度 = 有效观测时长 / 所需时长（I、真峰值、PLR、频谱、起音 10 s；LRA 60 s——规格「前 60 s 未稳定」），封顶 1；
      - 代表性 = 不重复的有效素材 / 15 s，封顶 1。宿主给了播放位置（且在播放）时按 100 ms 格记录听过的位置，
        同一段反复循环不增加；拿不到位置时按有效时长计，但封顶 0.75（低于出厂满置信度 0.8：保守降级，不阻止学习）。
      - 起音两项再乘辅助谱通量的一致性系数（包络事件远多于 250 Hz 以上的通量突起时降到最低 0.5，见 cpp「起音」节）。
    「不知道」一律 valid = false（数值不当 0）：例如没有任何起音事件时密度 = 0（有效），强度无效。

    频谱证据的单位是 dB：tameEvidenceDb / recoverEvidenceDb = 该段在有效帧上超出 ±2 dB 容忍区间的正 / 负偏离的
    时间平均（都记为正数，分开累计、不互相抵消）；Recover 另乘「持续缺失」系数（缺失时间占比 50 % 起、80 % 满），
    且只给有效带宽内、两侧都有内容的中间段（两端的 40 Hz / 16 kHz 段、逐段陡降超过 3 dB 的滚降区一律为 0）。

    ---- v0.1.7.0 修坯 Clay（设计规格-Clay-分析决策与验收 1.2 ～ 1.4、设计规格-Clay-根治修订 3.1 ～ 3.2；实现口径见
         MusicAnalyzer.cpp「四」「五」节）----

    画像格式 2 的立体声 / 超低频字段与实时立体声视图，同样只在入口测点、只读，不新增 FFT：
      - subShareDb（样本流）：真次声占比——M = (L + R) / 2 过 8 阶 Butterworth 16 Hz 低通，E_sub / E_全（ADR-012：原 30 Hz 4 阶
        在底鼓基音 45 Hz 处只衰减 14 dB，测到的主要是底鼓漏入）。与起音包络同一套边界（同一个组网格、学习区间按组样本精确、
        有效音频门用起音那一份），所以同样与块长无关；M 恒为 0 时不知道。
      - 立体声画像（帧流）：Balance 帧的 bandPower / bandCross，与频谱证据同一批帧、同一逐段 −60 dBFS 静音门权重做功率域
        时间和，组特征按组内功率和；有效 / 有效时长 / 置信度同现有特征口径（至少 1 s、10 s 观测充分），逐段可信度同 bandConfidence。
        逐段没有统计时数值 0、可信度 0（0 dB / ρ = 0 看上去像「很宽」，读的人先看 bandStereoConfidence）。
      - 先试后用的峰值代价（样本流，只在学习区间内跑）：把低切 / 收单 ×4 / 加宽的候选处理跑在原始输入上，比较 100 ms 块峰值
        P99 的变化（lowCutPeakCostDb、monoPeakCostDb[4]、widthPeakCostDb）；块网格与起音同一套边界，有效块满 10 s 才有效。
        不学习时候选不跑（不耗 CPU）；收单候选的线性相位低通与 M / S 延迟环在 prepare 时按采样率分配。
      - 实时视图（帧流，不进画像）：liveLowCorrelation = 300 ms 慢路，liveLowCorrelationFast = 120 ms 快路（同门、同运行均值起步、
        同重启规则），liveHigh* = 300 ms；本帧门下的段不计；不连续点 / 丢帧时清空，重新平滑满各自的时间常数才有效；组内平滑功率
        落到段静音门下即无效。帧门限下（静音）两路低频相关度标无效（平滑状态保留，回到门上接着平滑——ClayGuard 见到无效就冻结，
        与这里的帧门同一口径，审核 V1-03），高频两项冻结在最后的值。泄漏门（BUG-009）：本帧低频组的核心（40 ～ 100 Hz）门上功率
        低于 160 ～ 250 Hz 的 −35 dB 时，低频组的读数只是上方内容漏进 125 Hz 段的部分，本帧低频组不计（同整组在门下）——没有低音、
        只有 pad 的段落里两路低频相关度因此无效（ClayGuard 冻结反相一路），不再跟着漏进来的分音摆。
        分析器只看入口，宿主旁通时入口照常，所以「宿主旁通时冻结」由 ClayGuard 按编排器给的时长 0 做（ClayGuard.h），不在这里。

  ==============================================================================
*/

#pragma once

#include "BalanceModule.h"
#include "GlazeTypes.h"
#include "LoudnessMeter.h"

#include <juce_dsp/juce_dsp.h>

namespace celadon::glaze
{
    // 短期视图（每块更新；给保护层、校准器、界面）。
    struct ShortTermView
    {
        Feature momentaryLufs;      // M：400 ms
        Feature shortTermLufs;      // S：3 s
        Feature psr3sDb;            // 最近 3 s 真峰值最大值 − S
        Feature liveTameEvidenceDb; // 持续突出频段的短期均方根证据；复用 Balance 帧，停止学习后仍更新，不进入保存画像。
        Feature liveSpectralSlopeDbPerOct; // 门上宽带内容的短期斜率；窄带/静音无效，不进入保存画像。
        std::array<float, (size_t) numBands> liveSlopeWeights {}; // 基准画像须用相同段/权重回归，避免统计口径造成假漂移。
        // ---- v0.1.7.0 修坯 Clay 的实时立体声视图（设计规格-Clay-分析决策与验收 1.4；不进画像，ClayGuard 读）----
        // 由 Balance 帧的 bandPower / bandCross 做 300 ms 功率域一阶平滑；静音门下的段不计；静音 / 宿主旁通时冻结。
        Feature liveLowCorrelation;       // 40 ～ 125 Hz：Σ X̃ / Σ P̃（−1 ～ 1），300 ms 慢路（ADR-012：不再与快路取小）；
                                          // 门上无内容或本帧在帧门限下（静音）时 valid = false（与 ClayGuard 的冻结对齐）；
                                          // 只有上方内容漏进来（核心 40 ～ 100 Hz 低于 160 ～ 250 Hz 的 −35 dB）的帧不计（BUG-009）
        Feature liveLowCorrelationFast;   // 同上，120 ms 快路（ClayGuard 只在慢路已 < clayGuardPhaseFastGate 时用它加速）
        float liveHighMidPower = 0.0f;    // 2 ～ 16 kHz 的 Σ P̃_M（线性）
        float liveHighSidePower = 0.0f;   // 2 ～ 16 kHz 的 Σ P̃_S（线性）
        bool liveHighValid = false;       // 上面两项有效（门上有内容；本帧在帧门限下（静音）时为 false、两项功率值保留）
        bool musicActive = false;   // 本块在有效信号门限之上
    };

    // 辅助量（v0.6 A1 追加；画像里没有对应字段，给编排器 / 诊断 / 测试读）。只在学习区间内累计，停止后固定。
    struct AuxiliaryFeatures
    {
        Feature highBandFluxDb;                 // 250 Hz 以上段每帧正向谱通量的均值（dB / 帧；帧间隔 = Balance 的 hop，约 10 ms）
        Feature highBandFluxOnsetsPerSecond;    // 谱通量突起（高于 2 s 滑动均值 3 倍、至少 3 dB、50 ms 不应期）的密度
        float onsetAgreement = 1.0f;            // 起音置信度的一致性系数 0.5 ～ 1（见 MusicAnalyzer.cpp「起音」节）
        Feature lowBandRangeDb;                 // 低频（100 Hz 段及以下）逐帧电平的 P90 − P10（dB）：分析器第 4 节「低频随时间的变化」，
                                                // 持续低音小（几 dB）、间歇的重底鼓大（十几 dB 以上）；与 lowShare 一起给 Glue 的风险判断
        Feature trialPeakP99Db;                 // v0.1.7.0 先试后用（ADR-012）：学习区间有效块的原始块峰值 max(|L|, |R|) 的 P99（dBFS），
                                                // 即画像里各项峰值代价的基准；与代价同时有效（有效块满 10 s）。给诊断 / 测试核对，不进画像
    };

    class MusicAnalyzer
    {
    public:
        MusicAnalyzer();
        ~MusicAnalyzer();

        void prepare (const juce::dsp::ProcessSpec& spec);
        void reset();                                   // 全部清空（含候选累计）
        void handleDiscontinuity (Discontinuity kind);  // 丢弃跨越不连续点的窗口；不清已完成的累计

        // 链是原地处理，入口样本在模块处理后就没了，所以每块分两步（CeladonChain::process 里按此顺序调）：
        //   1. processInput：模块处理之前。input = 链入口原始输入；inputMeter = 已处理过这一块的 IN 表
        //      （读它新完成的 100 ms 子块能量与本块真峰值，算 M / S / I / LRA / PLR / PSR）；起音包络在这里逐样本跑。
        //   2. processSpectrum：模块处理之后。balance = 已处理过这一块的 Balance，读它这一块新产生的全部原始帧
        //      （每帧段功率，入口测点），累计频谱证据、斜率、占比与起音辅助的谱通量。
        void processInput (const juce::AudioBuffer<float>& input, const LoudnessMeter& inputMeter) noexcept;
        void processSpectrum (const BalanceModule& balance) noexcept;

        // 宿主时间线（编排器每块转交）：用来识别同一段反复循环，重复覆盖不增加代表性置信度。
        void setHostInfo (const HostInfo& info) noexcept;

        void startLearning() noexcept;
        void stopLearning() noexcept;
        bool isLearning() const noexcept;

        // 当前候选画像（学习中不断更新；停止后固定）。
        const MusicProfile& getCandidateProfile() const noexcept;
        const ShortTermView& getShortTermView() const noexcept;

        // v0.6 A1 追加：辅助量（学习区间内的高频谱通量统计）。
        const AuxiliaryFeatures& getAuxiliaryFeatures() const noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MusicAnalyzer)
    };
}

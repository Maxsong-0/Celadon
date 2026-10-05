/*
  ==============================================================================

    GlazeController.h
    T-048 阶段 2 编排器。Max 2026-10-05 对草图第 8 节 Q1–Q12 答「可以」，下面按那些默认做，不再改口径。

    产品路径：不再聆听、不再锁定、不再校准、不再重新聆听。链里只有一份 LiveProfile。
    大约每 1 秒，已发布的画像进一次 makePlan。旋钮、风格、曲风、交付在本块记下，和这一秒的格子
    合在同一次 makePlan 里。实际增量按同一套速率靠过去，增益再加 G_trim 和各守门里更负的那个。

    调用顺序是链定死的：beginBlock 在分析之前返回，本块要用的参数只能是上一块 endBlock 已经靠过去的结果。
    endBlock 在分析之后最多做一次 makePlan，再把 applied 靠向新目标，下一块才听得见。这是一块的延迟。

    Q4：音频线程只记下新的 MainSectionModel，不调用 LiveProfile::setConstants（那一次要同步重算整窗，
    放不进 48 kHz / 32 的块）。已发布的画像继续用旧模型。要换模型，得到下一次 prepare（音频已停）。
    折进 advanceTo 需要改 LiveProfile 的契约，不在本文件的清单里。

    正在播的这一段的 m：现在读的是 MusicAnalyzer::getSegmentLedger()。通用档这份账本不记账，读数是 none，
    m 按 1（第 6 条不误报「本段较轻」）。分析器把实时账本接到这个读口之前，棘轮在通用档不会卡住增益。

    1 秒格子是编排器自己的已处理样本计数，格子长度 = llround(采样率)。宿主 reset 不清这个计数。
    分析器的实时时间轴没有公开读口。

    GLAZE ≤ 0：这一块的增量、G_trim、守门退让都归零，交给模块的参数除 Clay 三态外逐位等于手动值。
    守门的统计窗留着。Clay 照旧 resolveClayParams。

    宿主 reset、跳播、循环回绕：不调用 LiveProfile::reset，不清 LIVE 累计，不清 GlazeGuard。
    prepare 若已经准备过：先把内存里的 LIVE 做成 XML，冷启动，再读回来。这样只调 prepare 的调用方也不会丢 LIVE。

    旧工程（格式 1，或只有 GLAZE_PROFILE / PLAN / CALIBRATION / EXECUTION、没有能用的 LIVE）丢掉内容，
    本次打开把 legacyStateDiscarded 置上。这个标志不进 LIVE。格式高于 2：冷启动，不置这个标志。
    GLAZE_CONSTANTS 读到就忽略。makePlan 用本二进制的出厂常数（测试可以用 setConstantsForTesting 换一份）。

    百分数旋钮没有单独的速率。摘录里的 0.2 / 1.0 是 dB/s，这里按参数自己的单位套用同一组数字。

  ==============================================================================
*/

#pragma once

#include "ChainParams.h"
#include "GlazeTypes.h"
#include "LiveProfile.h"

#include <juce_dsp/juce_dsp.h>

#include <memory>

namespace celadon::glaze
{
    class GlazeController
    {
    public:
        GlazeController();
        ~GlazeController();

        // 消息线程。采样率变了也保留 LIVE：先快照，冷启动，再读回。
        void prepare (const juce::dsp::ProcessSpec& spec);

        void setHostInfo (const HostInfo& info) noexcept;
        void hostReset() noexcept;
        int getGuardSamplesUntilBoundary() const noexcept;
        void handleDiscontinuity (Discontinuity kind) noexcept;

        // 本块交给四个模块的参数。还没 prepare 时：只记下曲风对应的主段模型，Clay 按三态合成，其余等于手动值。
        const ChainParams& beginBlock (const ChainParams& manual, bool hostBypassed) noexcept;

        void analyzeInput (const juce::AudioBuffer<float>& input, const LoudnessMeter& inputMeter) noexcept;
        void analyzeSpectrum (const BalanceModule& balance) noexcept;

        // 限幅之后、Listen 之前。出口主段电平在这里累计（endBlock 没有缓冲区）。
        void noteOutput (const juce::AudioBuffer<float>& output) noexcept;

        void endBlock (const BalanceModule& balance, const LoudnessMeter& inputMeter, const LoudnessMeter& outputMeter,
                       const ModuleStats& stats, int chainLatencySamples, bool spectrumDeferred = false) noexcept;

        // 消息线程。音频停着。还没 prepare 时先收着，prepare 末尾再读。
        std::unique_ptr<juce::XmlElement> createStateXml() const;
        void restoreStateXml (const juce::XmlElement* xml);

        Status getStatus() const noexcept;
        const LiveProfile& getLiveProfile() const noexcept;

        // 测试换一份常数。产品路径不调，makePlan 因此用出厂常数。
        void setConstantsForTesting (const Constants& constants);

    private:
        struct Impl;
        std::unique_ptr<Impl> impl;
    };

    // 频率对数、比例与 dB 线性。Clay 仍用它；方案本身不再做 3 秒过渡。
    ClayPlan blendClayPlans (const ClayPlan& from, const ClayPlan& to, float t, float lowCutFloorHz) noexcept;
}

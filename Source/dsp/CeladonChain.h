/*
  ==============================================================================

    CeladonChain.h
    整条信号链（设计规格-总体，顺序固定；v0.1.7.0 起见 ADR-009）：输入 → Balance → Clay → Color → Limit → 输出。

    把「按顺序调模块、延迟相加、宿主旁通、播放中途重置」从 PluginProcessor 里抽出来，
    只依赖 juce_dsp，可无头编译测试；PluginProcessor 只负责读参数、输入 NaN 保护与宿主集成。

    v0.6（GLAZE，ADR-008；设计规格-GLAZE-界面与实现第 3 节）：两块表（IN / OUT）与 GLAZE 编排器挪进链里——
    入口分析与出口监测都在链内，整条 GLAZE 行为才能无头测试。每块的顺序：
      setHostInfo → setParams（编排器 beginBlock 算实际参数，再交给四个模块）→
      process：编排器接上一块的 Balance 帧 → IN 表 → 编排器入口分析 → Balance → Clay → Color → Limit →
               输出非有限值清零并重启淡入 → OUT 表 → 编排器 endBlock → Listen 监听补偿。
    Listen 在链末端按实测响度只衰减，不抬高较轻的母带；OUT 表始终显示监听补偿前的母带输出。
    复用 100 ms K 加权能量，按延迟占子块的比例近似配对；不是样本精确的对齐测量。

    v0.1.7.0（修坯 Clay，设计规格-Clay 第 3 节、ADR-009）：Clay 插在 Balance 之后、Color 之前——
      - 画像不随 GLAZE 变：Balance 的分析帧（GLAZE 的频谱 / 立体声证据）与编排器入口分析都取链入口原始信号，Clay 在它们之后，
        分析器看不到 Clay 自己的结果（放最前面会形成「按自己的输出决策」的闭环，画像随 GLAZE 变，违反 ADR-008）；
      - 清理赶在所有非线性 / 动态级之前：Glue 检测、Heat、Clip、限幅器都吃到切掉超低频之后的干净低频；
      - 与放最前面几乎等效：Balance 最低一段中心 40 Hz（Q ≈ 4.3），基本不碰 30 Hz 以下。
    Clay 的实际参数由编排器 beginBlock 按「自动 / 关 / 手动」三态合成（见 ChainParams.h 文件头；「手动」在 GLAZE 0 也生效），
    链只负责交给模块：宿主旁通时与其他模块一样强制旁通，Guard 时隙切片、NaN 恢复、restartWithFadeIn、reset 都包含它。
    Clay 有固定延迟 D + G（D = 线性相位收单，ADR-012；G = 峰值守护前瞻，ADR-014；设计规格-Clay-根治修订 第 2、6 节）：
    44.1 / 48 / 96 / 192 kHz 为 1013 / 1102 / 2206 / 4414 样本（其中 D 为 528 / 574 / 1150 / 2302），与参数、旁通无关。
    总延迟仍是各模块之和：48 kHz / Lookahead 2 ms 为 282 + 1102 = 1384 样本（约 28.8 ms），96 kHz 为 378 + 2206 = 2584，
    192 kHz 为 4984（T-016 逐级基准实测）；Listen 的配对、校准器的对齐、restartWithFadeIn 的静音段都按 getLatencySamples() 自动跟上。
    GLAZE 0 且抽屉默认（三态都「自动」）时 Clay 三项实际值都是 0、模块是逐位的 D + G 样本纯延迟：整链输出 = 加 Clay 之前的链在
    Clay 的位置插一段 D + G 样本纯延迟（Color / Limit 也是纯延迟时就是旧输出平移 D + G 个样本，DAW 延迟补偿后对齐不变）。

    v0.1.8.0 试听差值（界面 LIMIT 卡片的耳机）：限幅器顺带写出「本级削掉的部分」（按推响前的电平，见 LimiterModule.h 文件头），
    链在所有测点（OUT 表、编排器 endBlock、Listen 的测量）之后才把送出去的信号换成它，20 ms 交叉淡化——表、响度、GLAZE 的学习
    与校准、等响的估计都照常按母带算，只有送给宿主的监听信号换了。差值不乘 Listen 的监听增益（它本来就在推响前的电平）。
    听原声、宿主旁通、LIMIT 旁通时不试听（淡回正常输出）。差值要整段存下来：宿主块超过 prepare 的块大小时（少见），只在试听期间
    按差值缓冲的长度切片。不试听时（默认）与原来逐位相同。

  ==============================================================================
*/

#pragma once

#include <juce_dsp/juce_dsp.h>

#include "BalanceModule.h"
#include "ChainParams.h"
#include "ClayModule.h"
#include "ColorModule.h"
#include "GlazeController.h"
#include "LimiterModule.h"
#include "LoudnessMeter.h"
#include "ReferenceSpectrum.h"
#include "ReleaseSafety.h"

#include <array>
#include <atomic>
#include <memory>

namespace celadon
{
    class CeladonChain
    {
    public:
        // 用户参数（v0.6 拆到 ChainParams.h，GLAZE 各单元也要用；这里保留原名）。balance / color / limit 是用户在
        // 各模块旋钮上的值；setParams 交给 GLAZE 编排器算出实际值再交给模块，GLAZE ≤ 0 时实际值逐位等于用户值（ADR-008）。
        // v0.1.7.0：claySettings 是「高级」抽屉里的三态设置与手动值；传进来的 clay（实际参数）不读——由编排器按三态合成。
        using Params = ChainParams;

        // 滤波器余振（秒）：插件报给宿主的尾巴 = 链延迟 + 本项；离线渲染工具冲尾巴也用它（审核 V3-02：两处曾各写一份、不一致）。
        // 0.35 s 的推导：Clay 10 Hz 4 阶低切衰 60 dB 约 0.29 s，留余量（A7，v0.1.7.0）。
        static constexpr double filterRingOutSeconds = 0.35;

        CeladonChain() = default;

        // 四个模块、两块表、编排器按同一份 spec 整体重建。编排器保留已提交的画像与方案（采样率变了校准标「待验证」）。
        void prepare (const juce::dsp::ProcessSpec& spec);

        // 清空全部状态，立即出声（同规格 prepareToPlay 时用）。两块表不清（宿主重启不该把整体响度清掉，与 v0.5 相同）；
        // 编排器按「宿主 reset」清运行状态、保留已提交版本。
        void reset();

        // 播放中途的重置（宿主 Reset、NaN 恢复）：清空全部模块，先静音「整条链的延迟」再 10 ms 淡入；编排器同上。
        void restartWithFadeIn();

        // 宿主时间线（播放位置、是否在播、循环、离线渲染）。每块在 setParams 之前调（处理器从 AudioPlayHead 读）；
        // 不调就沿用上一次的（默认 = 不知道位置、当作在播、实时）。
        void setHostInfo (const glaze::HostInfo& info) noexcept;

        // 音频线程每块调一次。编排器按 GLAZE 算出实际参数，再交给各模块——各模块自带的 20 ms 平滑 / 开关交叉淡化照常生效，
        // 拧 GLAZE 不会有拉链噪声或咔哒。hostBypassed = 宿主旁通：强制旁通全部模块（含 Clay；各模块自带延迟补偿与 20 ms
        // 交叉淡化，Clay 旁通时干路同样延迟 D），同时让编排器暂停校准；Listen 只影响测点后的监听增益，校准照常。
        //
        // compareOriginal = 界面的 A / B 对比正在听 B（原声，v0.1.8.0）：输出与宿主旁通完全一样（延迟补偿过的干信号、
        // 20 ms 交叉淡化、校准暂停、Listen 的测量冻结），区别只在 Listen 打开时的监听增益——宿主旁通不补偿；
        // 听原声时若原声比处理结果响，就把原声衰减到处理结果的响度（同样只衰减、不抬高），A / B 两边等响。
        // 处理结果更响时原声原样播放：A 那边已经由 Listen 衰减到原声的响度。默认 false 时与 v0.1.7 逐位相同。
        //
        // auditionDifference = 界面 LIMIT 卡片的「试听差值」（v0.1.8.0，见文件头）：送出去的信号换成限幅器削掉的部分，
        // 测点与处理都不变；听原声、宿主旁通或 LIMIT 旁通时不生效。默认 false 时逐位不变。
        void setParams (const Params& newParams, bool hostBypassed, bool compareOriginal = false, bool auditionDifference = false);

        // 原地处理。GLAZE 活跃时按 Guard 实际 10 ms 时隙边界切片，让模块统计不跨窗口；不添加音频缓冲或延迟。
        // 返回 true = 输出出现非有限值（内部溢出）：故障切片已清零、整链重启淡入，交给处理器计数。
        bool process (juce::AudioBuffer<float>& io);

        // 整条链的延迟（样本）= 各模块之和（Balance 为 0；Clay 为固定的 D + G，prepare 后不变）；setParams 之后立即反映 Lookahead 的变化。
        int getLatencySamples() const noexcept;

        // 上一次 setParams 交给四个模块的实际参数（宿主旁通强制之前；音频线程读——处理器每块把其中 7 项写进原子量给界面；
        // Clay 的实际值界面走 getGlazeStatus() 的 clay* 字段，不走这里）。
        const Params& getEffectiveParams() const noexcept   { return effective; }

        // ---- 表（界面线程调用，无锁）----
        float getAndResetLimitGainReductionDb() noexcept;
        float getAndResetGlueGainReductionDb() noexcept;

        // IN / OUT 真峰值：自上次读取以来的最大值（dBTP），读完清零；OUT 在 Listen 监听补偿之前。
        float getAndResetInputTruePeakDb() noexcept          { return inputMeter.getAndResetTruePeakDb(); }
        float getAndResetOutputTruePeakDb() noexcept         { return outputMeter.getAndResetTruePeakDb(); }

        // OUT 的短期 / 整体响度（界面右侧读数）：Listen 监听补偿之前的母带输出。
        float getOutputShortTermLufs() const noexcept        { return outputMeter.getShortTermLufs(); }
        float getOutputIntegratedLufs() const noexcept       { return outputMeter.getIntegratedLufs(); }

        // 界面「Reset」：两块表的整体响度与短期窗口在音频线程的下一块开头清（子块网格不断，GLAZE 的分析与校准不受影响）。
        void requestMeterReset() noexcept
        {
            inputMeter.requestReset();
            outputMeter.requestReset();
        }

        // Balance 各段当前修正量（dB），界面曲线用；无锁。
        void getBalanceBandGainsDb (BalanceModule::BandGains& out) const noexcept;

        // Listen 测到的「处理结果 − 原声」响度差（dB，正 = 处理后更响；±24 封顶），界面的等响按钮与 A / B 状态显示用；
        // 还没测出第一份估计（prepare 之后要先攒满 1 s 窗口）时 hasListenDifference() 为 false。Listen 关着也照常测（与监听增益同一份估计）；
        // 宿主旁通、听原声时冻结在最后一份。任何线程可读（两个原子量）。
        bool hasListenDifference() const noexcept           { return listenDifferenceValid.load (std::memory_order_relaxed); }
        float getListenDifferenceDb() const noexcept         { return listenDifferenceDb.load (std::memory_order_relaxed); }

        // ---- GLAZE（界面线程调用，无锁；存取在消息线程）----
        glaze::Status getGlazeStatus() const noexcept                    { return controller.getStatus(); }
        void requestGlazeRelearn() noexcept                              { controller.requestRelearn(); }
        std::unique_ptr<juce::XmlElement> createGlazeStateXml() const    { return controller.createStateXml(); }
        void restoreGlazeStateXml (const juce::XmlElement* xml)          { controller.restoreStateXml (xml); }

        void processReference (const juce::AudioBuffer<float>& input, bool connected) noexcept { reference.process (input, connected); }
        void requestReferenceLearn() noexcept { reference.requestLearn(); }
        void cancelReferenceLearn() noexcept { reference.cancelLearn(); }
        void clearReference() noexcept { reference.clear(); }
        std::unique_ptr<juce::XmlElement> createReferenceStateXml() const { return reference.createStateXml(); }
        void restoreReferenceStateXml (const juce::XmlElement* xml) { reference.restoreStateXml (xml); }
        void setReleasePreviewEnabled (bool enabled) noexcept { previewRequested.store (enabled, std::memory_order_relaxed); }
        bool isReleasePreviewEnabled() const noexcept { return previewRequested.load (std::memory_order_relaxed); }
        MasteringStatus getMasteringStatus() const noexcept;

        // ---- 只给测试用 ----
        glaze::GlazeController& getGlazeController() noexcept              { return controller; }
        const glaze::GlazeController& getGlazeController() const noexcept  { return controller; }
        const LoudnessMeter& getInputMeter() const noexcept                { return inputMeter; }
        const LoudnessMeter& getOutputMeter() const noexcept               { return outputMeter; }
        int getLastProcessSliceCountForTesting() const noexcept { return lastProcessSliceCount; }
        // 只给测试 / 离线工具：Clay 模块（读峰值守护的诊断统计，ADR-014）。
        ClayModule& getClayForTesting() noexcept { return clay; }

    private:
        bool processSlice (juce::AudioBuffer<float>& io);
        void resetListenMeasurement (bool resetGain);
        void processListen (juce::AudioBuffer<float>& io);
        void applyListenGain (juce::AudioBuffer<float>& io, int start, int length);
        void updateListenTarget();

        // 试听差值：淡入中、试听中、淡出中都算「在用」——这时限幅器要写差值、宿主大块要切片。
        bool differenceInvolved() const noexcept   { return differenceMix.isSmoothing() || differenceMix.getCurrentValue() > 0.0f; }
        void applyDifferenceAudition (juce::AudioBuffer<float>& io, bool outputCleared);

        // 按信号顺序声明（v0.1.7.0 起 Balance → Clay → Color → Limit）。
        BalanceModule balance;
        ClayModule clay;
        ColorModule color;
        LimiterModule limiter;

        LoudnessMeter inputMeter, outputMeter;
        glaze::GlazeController controller;
        ReferenceSpectrum reference;
        ReleaseSafety releaseSafety;
        std::atomic<bool> previewRequested { false };
        std::atomic<float> publishedPreviewGain { 0.0f }, publishedCodecMargin { 0.0f };
        std::atomic<int> publishedDither { 0 };
        bool offline = false, releaseAllowed = true, previewActive = false;


        Params effective;
        Params manual;
        int lastProcessSliceCount = 0;

        double listenSampleRate = 0.0;
        // hostIsBypassed = 此刻走干路（宿主旁通，或 v0.1.8.0 起听原声）：Listen 的测量冻结、不补偿处理结果。
        bool listenRequested = false, hostIsBypassed = false, listenHasEstimate = false;
        bool listenComparing = false;   // 听原声（A / B 的 B）且宿主没有旁通：Listen 的监听增益改用 listenOriginalDb
        int listenLatency = 0;
        std::int64_t listenInputCursor = 0, listenOutputCursor = 0, listenIgnoreThrough = 0;
        std::array<LoudnessMeter::SubBlock, LoudnessMeter::subBlockRingCapacity> listenInputHistory {};
        // 固定读缓冲重用，避免极小宿主块每次都清零两份 128 子块数组。
        std::array<LoudnessMeter::SubBlock, LoudnessMeter::subBlockRingCapacity> listenInputRead {}, listenOutputRead {};
        static constexpr int listenWindowBlocks = 10; // 1 s，静音子块也纳入窗口
        std::array<double, listenWindowBlocks> listenWindowIn {}, listenWindowOut {};
        int listenWindowWrite = 0, listenWindowCount = 0;
        double listenInputEnergy = 0.0, listenOutputEnergy = 0.0;
        float listenEstimateDb = 0.0f;
        float listenOriginalDb = 0.0f;   // 听原声时给原声的衰减（≤ 0 dB）：原声比处理结果响多少就衰减多少
        std::atomic<float> listenDifferenceDb { 0.0f };      // 界面读：处理结果 − 原声（dB）
        std::atomic<bool> listenDifferenceValid { false };
        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> listenGain;

        // 试听差值（v0.1.8.0）：限幅器写出的差值（prepare 时按块大小分配）与 0 → 1 的 20 ms 交叉淡化。
        juce::AudioBuffer<float> differenceBuffer;
        juce::SmoothedValue<float> differenceMix;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CeladonChain)
    };
}

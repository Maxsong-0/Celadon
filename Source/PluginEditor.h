/*
  ==============================================================================

    PluginEditor.h
    v0.1.8.0 界面改版（V4 设计稿「融合版」）：1200 × 820 的浅色画布，整体按 70 % ～ 200 % 等比缩放。

      上层：左 IN 表 · 正中频谱仪 · 右 OUT / GR 表；
      中层：GLAZE 主旋钮在正中；T-107 重构包 A（效果图，T-105 线 ③ 第 6.2 节）起两侧是两块侧板——左「此刻在推什么」（四个模块的
            自动加量 + 状态句）、右「交付」（原来频谱仪右上角的响度小窗搬到这里放大，加最近 60 秒的短期响度与 GLAZE 自动增益），
            粒子流收进侧板与旋钮之间的通道；GLAZE 标题行左右原来的「曲风」「交付」两个选择（T-038）删了（Max 2026-11-13；参数本身 T-108 删），
            参数还在、面板上不再有；
            下面一排 操作提示 / 监听退出提示 · 风格 | A / B · 等响 · 颜色图例；
      下层：按处理顺序的四张卡片 BALANCE → CLAY → COLOR → LIMIT。

    画布（Canvas）是一个固定 1200 × 820 的子组件，编辑器按窗口大小给它一个缩放变换——所有文字与矢量在任何比例下都清晰，
    位图（背景、瓷面、流线）按当次绘制的物理像素比生成。窗口大小随工程存一份（uiZoom），另记为新窗口的默认大小（UiSettings）。

    GLAZE 的数据源由处理器在 createEditor 里注入：状态快照、本块交给模块的实际参数。
    「重新聆听」动作的注入还在，阶段 2 起是空操作（没有这个按钮）。两种旧工程提示（旧聆听结果已丢弃、曲风与交付已取消）由计时器每帧
    从处理器取当前值（状态快照里的 legacySessionDiscarded、注入的 genreDeliveryResetSource）。

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>

#include "PluginProcessor.h"
#include "ui/CeladonLookAndFeel.h"
#include "ui/Controls.h"
#include "ui/GlazeKnob.h"
#include "ui/GlazeStatusText.h"
#include "ui/HelpText.h"
#include "ui/LevelMeters.h"
#include "ui/MiddleLayer.h"
#include "ui/ModuleCards.h"
#include "ui/ParticleField.h"
#include "ui/Popovers.h"
#include "ui/SpectrumAnalyser.h"
#include "ui/SpectrumView.h"
#include "ui/TopBar.h"
#include "ui/UiSettings.h"

#include <functional>
#include <vector>

//==============================================================================
class CeladonAudioProcessorEditor  : public juce::AudioProcessorEditor,
                                     private juce::Timer,
                                     private juce::ValueTree::Listener,
                                     private juce::AsyncUpdater,
                                     private juce::ChangeListener
{
public:
    explicit CeladonAudioProcessorEditor (CeladonAudioProcessor&);
    ~CeladonAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    // ---- GLAZE 的三个数据源（处理器在 createEditor 里注入；都在消息线程调、无锁）----
    using GlazeStatusSource = std::function<celadon::glaze::Status()>;
    using GlazeRelearnAction = std::function<void()>;
    using GlazeEffectiveParamsSource = std::function<celadon::ChainParams()>;

    void setGlazeStatusSource (GlazeStatusSource source);
    // 阶段 2 没有重新聆听。处理器 createEditor 仍会注入：留着空实现，编排器删掉那一行之前这里什么都不做。
    void setGlazeRelearnAction (GlazeRelearnAction action);
    void setGlazeEffectiveParamsSource (GlazeEffectiveParamsSource source);

    // 两种旧工程提示，左下角各占一行；都是 false 就不显示。不写入 LIVE、不进工程。
    //   legacyGlazeDiscarded（T-048 Q1）：读到的是旧聆听结果（v1，或只有 GLAZE_PROFILE / PLAN / CALIBRATION / EXECUTION），已丢弃，
    //     提示「旧的聆听结果已丢弃，声音会变」——取状态快照的 legacySessionDiscarded；
    //   genreDeliveryReset（T-108，ADR-039 Q2）：读到的旧工程存着非默认的曲风 / 交付（T-038，已删），提示「曲风与交付已取消，
    //     已改回通用 + 标准，声音会变」——取这个数据源。
    // 两个值都是处理器记下的「最近一次读档」的结果（原子量），计时器每帧取一次：读档在哪个线程、编辑器先开后开都一样，
    // 读到干净的工程下一帧就清（T-108 独立复查 F12、第 2 轮 R2-F1：改前只会置上不会清，宿主不在消息线程读档时曲风提示不亮）。
    using GenreDeliveryResetSource = std::function<bool()>;
    void setGenreDeliveryResetSource (GenreDeliveryResetSource source);

    // 截图工具用：直接摆出两种提示；摆了以后计时器不再跟处理器的值（截图会话用完即弃，不需要恢复）。不进工程。
    void setLegacyNoticesPreview (bool legacyGlazeDiscarded, bool genreDeliveryReset);

    // 截图工具用：直接给状态行一份读数（c、m、冷导出、守护原因）。不进工程。clear 之后回到编排器快照。
    void setStatusLinePreview (const celadon::ui::glazestatus::Reading& reading);
    void clearStatusLinePreview();

    static constexpr int baseWidth = 1200;
    static constexpr int baseHeight = 820;

    // ---- 界面动作（按钮会调它们；截图工具也直接调）----
    // A / B 的 B（听原声）与 LIMIT 的试听差值互斥：打开一个就关掉另一个；LIMIT 旁通时不能试听差值。
    void setListeningToOriginal (bool listenToOriginal);
    void setAuditioningDifference (bool shouldAudition);
    void setReleasePreviewEnabled (bool enabled);
    void showSettings (bool shouldShow);
    void showHelp (bool shouldShow);
    void showAdvanced (bool shouldShow);
    void setZoom (double zoom);

    // 截图工具用：频谱仪往前推 seconds 秒、立刻刷新一帧读数，并把粒子动画往前推 seconds 秒（离屏时没有垂直同步）。
    void advanceForSnapshot (double seconds);

    // 截图工具的基准用：上一次生成静态背景花的毫秒数（打开编辑器、缩放停下时在消息线程同步生成）。
    double getStaticLayerRenderMs() const noexcept;

private:
    class Canvas;

    void timerCallback() override;
    void setLegacyNotices (bool legacyGlazeDiscarded, bool genreDeliveryReset);   // 有变化就重画一帧
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override;
    void valueTreeRedirected (juce::ValueTree&) override;
    void handleAsyncUpdate() override;

    void applyLanguage();
    void applyReduceMotion();
    void updateDismissLayer();
    void restartOutputSpectrum();
    void spectrumFrame (double timestampSeconds);
    void advanceSpectrum (double elapsedSeconds, double nowMs);
    void applyStoredZoom();
    double readStoredZoom() const;
    double fitZoomToScreen (double zoom) const;
    const juce::Displays::Display* currentDisplay() const;
    void storeZoom (double zoom);
    void resetMeters();

    CeladonAudioProcessor& processor;
    juce::SharedResourcePointer<celadon::ui::UiSettings> settings;
    celadon::ui::CeladonLookAndFeel lookAndFeel;

    GlazeStatusSource glazeStatusSource;
    GlazeEffectiveParamsSource glazeEffectiveParamsSource;
    GenreDeliveryResetSource genreDeliveryResetSource;
    bool legacyGlazeDiscarded = false;
    bool genreDeliveryReset = false;
    bool legacyNoticesPreviewOn = false;
    bool statusLinePreviewOn = false;
    celadon::ui::glazestatus::Reading statusLinePreview;

    std::unique_ptr<Canvas> canvas;

    celadon::ui::SpectrumAnalyser analyser;
    std::vector<float> pullBuffer;

    // 频谱仪按屏幕刷新推（T-036）：垂直同步回调挂在频谱仪组件上（60 / 120 帧，封顶 120）；垂直同步停了 / 没有（离屏、宿主或
    // 系统不给）时 30 Hz 定时器接着推；最小化时只收样本不计算。析构时先于画布释放（它要求组件在它的整个生命周期里有效）。
    juce::VBlankAttachment spectrumVBlank;
    double lastSpectrumAdvanceMs = 0.0;            // 最近一次推频谱的时刻（墙钟，毫秒）
    double lastVBlankMs = -1.0e9;                  // 最近一次垂直同步回调的时刻（墙钟，毫秒）
    double lastVBlankSeconds = 0.0;                // ……它带来的帧时间戳（秒）
    bool lastAdvanceWasVBlank = false;             // 上一次推频谱的是垂直同步（下一帧的时长按两次时间戳之差算）
    bool snapshotDriven = false;                   // 截图工具调过 advanceForSnapshot：频谱仪只由它按给定时长推，定时器的看门狗不再按墙钟补推（T-072）

    std::unique_ptr<juce::MouseListener> cornerListener;   // 右下角缩放把手的按下 / 松开（显示大小提示）

    double lastTimerMs = 0.0;
    float truePeakMaxDb = -120.0f;                 // 复位以来 OUT 的最大真峰值（右侧板「交付」）
    double historyPendingSeconds = 0.0;            // T-107 右侧板的历史：攒够 0.1 s 记一点（只在放音乐时攒）
    double lastSoundMs = -1.0e9;                   // 最近一次输出有声音的时刻（粒子据此判断「在播放」）
    double lastOverMs = -1.0e9;                    // 最近一次 OUT 真峰值超过 Ceiling 的时刻
    double nonFiniteWarningUntilMs = 0.0;
    int lastNonFiniteBlocks = 0;

    // 状态胶囊里 GLAZE 状态行的几段：View 已量化到显示精度，View 与语言都没变就沿用上次拼好的（T-032 R9-12）。
    celadon::ui::glazestatus::View glazeSegmentsView;
    celadon::Language glazeSegmentsLanguage = celadon::Language::chinese;
    juce::StringArray glazeSegments;
    bool glazeSegmentsValid = false;

    // GLAZE 状态的悬停气泡：只在语言变了时重设。
    celadon::Language glazeTooltipLanguage = celadon::Language::chinese;
    bool glazeTooltipValid = false;

    bool writingZoom = false;
    bool fittingZoomToScreen = false;              // applyStoredZoom 为放进屏幕缩小窗口时：resized 不写回
    const juce::Identifier uiZoomId { celadon::uiid::uiZoomProperty };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CeladonAudioProcessorEditor)
};

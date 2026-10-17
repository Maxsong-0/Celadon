/*
  ==============================================================================

    SnapshotMain.cpp
    celadon_ui_snapshot：在 Linux 上把整个插件（处理器 + v0.1.8.0 的编辑器）跑起来，喂一段合成的音乐，
    再把编辑器按几种状态离屏渲染成 PNG，对照 V4 设计稿检查布局、配色、中英文与缩放（见 README.md）。

    用法：celadon_ui_snapshot --out <目录> [--scale 2] [--only 名字,名字] [--frames N] [--deterministic]
      每种状态一张图：zh-default、en-default、zh-original（A / B 听原声 + 等响）、zh-audition（Gain +8 dB 时 LIMIT 试听差值）、
      zh-truepeak（限幅旁通 → 真峰值超限）、zh-settings、en-help、zh-help、zh-advanced、en-advanced（「高级」面板）、
      zh-glaze90、zh-glaze0、zh-zoom70、zh-zoom150、zh-reduced、zh-input（T-051：频谱仪 IN 虚线开着）；
      T-038（曲风 Deep House、交付 俱乐部）：zh / en-house-layout（两个选择在 GLAZE 标题行左右）、-house-hint（反相提示）。
      T-048 状态行：-status-follow / -guard / -export（跟随、守护退让、导出冷启动，只截胶囊）、-legacy-notice（旧工程提示，左下角）。
      --only check 跑交互检查（末尾 ⑫～⑲ 是 T-032 的回归检查，另有 T-036 频谱仪子组件缓存类型一条：分层瓷面与原算法逐像素 ≤ 2 LSB、主旋钮 / 文字小部件 / 粒子缓存图
      命中、30 帧节流、底排与延迟读数的触发、「高级」面板关着时的状态、状态胶囊的缓存与呼吸帧不排版文字），⑲ 续与 ⑳ 是 T-041 的：
      小数像素比（0.7 ～ 2.5）下胶囊呼吸与对齐缓存的局部作废只重画作废区、结果与整块重画逐字节相同；其后两条是 T-051 的：
      频谱仪 IN 虚线自己切段与 JUCE createDashedStroke 逐位相同、频谱仪增量重画（×1 / ×2 矩形列表、×1 包围盒）与整帧重画逐字节相同
      （评审 B1 的 S8 场景）；末尾六条是 T-058 的：S9（峰值线关 → 停播 → 打开 → 切 B 的增量重画 vs 整帧；峰值线关、只剩峰值保持在落时
      0 次重建）、播放（评审 B1 的第一个场景）、S10a / b / c（打开峰值线、换斜率、悬停那一刻画的是此刻的数据）。增量重画几个场景见 dirtyScenarios；
      --only bench 跑绘制性能基准（T-032）：瓷面参照与分层的生成耗时、静态背景生成耗时，GLAZE 45 % 播放中 1 / 2 / 4 倍下
      编辑器定时器一帧、整帧重画与只重画粒子带的毫秒（墙钟与本线程 CPU）、各部件累计 paint 与缓存读写、状态胶囊呼吸帧，
      拖动 GLAZE 时消息线程的 CPU 占比，听原声（B）时的粒子带重画；--frames N 改每段帧数。软件渲染口径，Mac 需 Max 实测；
      --only spectrum-bench 跑频谱仪专项基准（T-036）：30 / 60 / 120 帧 × 1 / 2 倍，音乐按帧率在放，每帧推一帧、只重画频谱仪那块，
      打印每帧与每秒的 CPU；--seconds N 改每种组合跑几秒（默认 3）。只用改前改后都有的接口，能在改前的树上量基线；
      --only spectrum-peak-bench 量峰值线（PEAK）关着时频谱仪多做的重建 / 重画（T-058）：S8 与「停止」（正弦 4 s → 数字静音 10 s，
      斜率 0 / 4.5）× PEAK 关 / 开 × 60 / 120 帧 × 1 / 2 倍，每帧推一帧、只按脏矩形重画频谱仪，按秒打印标脏帧、曲线帧、脏区像素与
      advance / 重画的墙钟与 CPU（见 runSpectrumPeakBench）；同样只用改前改后都有的接口；
      --only digest 打印逐位摘要（瓷面参照实现 renderGlaze 的 200 种组合、静态背景的 4 种像素比）：改动前后各跑一次逐行比，
      应当逐位不变的项一字不差。
      --deterministic：出图时播放不跑消息循环（编辑器 / 胶囊 / 旋钮的定时器不按墙钟插进来），两次运行逐像素相同（电平表除外），
      用于改前改后逐像素对比；听原声那张图的瓷面因此还没换成去色版（换图靠主旋钮的定时器）。T-072 起编辑器定时器里的看门狗
      （垂直同步 100 ms 没来就按墙钟补推频谱仪）在第一次 advanceForSnapshot 之后整个关掉：单帧耗时 ≥ 100 ms（慢机、Debug、机器被占满）
      也不会让频谱仪按墙钟多走一步、出图随机器快慢变（--only check 里有一条守它，见 probeTimerPush）。
      T-100（ADR-037）：旋钮的读数带弹簧与彗尾。--only check 在滚轮那条之后多一组外圈手感检查（⑤b），⑬ 按读数走弹簧改写；
      每次存图前所有旋钮先停稳（save 里调 finishMotion），存下的是静止时的样子。

  ==============================================================================
*/

#include <JuceHeader.h>

#include "../../Source/ParameterIds.h"
#include "../../Source/PluginEditor.h"
#include "../../Source/PluginProcessor.h"
#include "../../Source/ui/HelpText.h"
#include "../../Source/ui/ModuleCards.h"
#include "../../Source/ui/Porcelain.h"
#include "../../Source/ui/UiSettings.h"

#if JUCE_MAC
 #include <CoreFoundation/CoreFoundation.h>
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <functional>
#include <iostream>
#include <optional>
#include <vector>

// PluginProcessor.cpp 里定义（插件外壳平时由 juce_audio_plugin_client 声明，截图工具不编它）。
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter();

namespace
{
    void dispatchEvents (int milliseconds)
    {
       #if JUCE_MAC
        CFRunLoopRunInMode (kCFRunLoopDefaultMode, (double) milliseconds / 1000.0, true);
       #else
        juce::MessageManager::getInstance()->runDispatchLoopUntil (milliseconds);
       #endif
    }

    constexpr double sampleRate = 48000.0;
    constexpr int blockSize = 512;

    // --deterministic：播放时不跑消息循环。编辑器自己的 30 Hz 定时器、状态胶囊的呼吸、主旋钮的定时器都按墙钟触发，
    // 跑消息循环时它们落在哪一帧每次不同（频谱取样口被拉的时刻、粒子跟的电平、圆点的相位都跟着变）；不跑时每帧只走
    // advanceForSnapshot，两次运行逐像素相同（只剩电平表按墙钟算的下落），改前改后的截图才能逐像素比。
    bool deterministic = false;

    // 一段 120 BPM 的合成音乐：底鼓、军鼓、踩镲、贝斯、和弦，立体声。每次调用往后写 numSamples 个样本。
    class Music
    {
    public:
        void render (juce::AudioBuffer<float>& buffer, int numSamples)
        {
            auto* left = buffer.getWritePointer (0);
            auto* right = buffer.getWritePointer (1);

            for (int i = 0; i < numSamples; ++i, ++position)
            {
                const auto t = (double) position / sampleRate;
                const auto beat = t * 2.0;                    // 120 BPM
                const auto beatPhase = beat - std::floor (beat);
                const auto eighthPhase = beat * 2.0 - std::floor (beat * 2.0);
                const auto bar = (int) std::floor (beat / 4.0);
                const auto beatInBar = (int) std::floor (beat) % 4;

                // 底鼓：每拍，150 → 45 Hz 下扫
                const auto kickTime = beatPhase / 2.0;
                const auto kickFrequency = 45.0 + 105.0 * std::exp (-kickTime * 30.0);
                kickPhase += kickFrequency / sampleRate;
                const auto kick = 0.62 * std::sin (juce::MathConstants<double>::twoPi * kickPhase) * std::exp (-kickTime * 9.0);

                // 军鼓：第 2、4 拍，噪声 + 190 Hz
                const auto snareOn = beatInBar == 1 || beatInBar == 3;
                const auto snareEnvelope = snareOn ? std::exp (-beatPhase / 2.0 * 22.0) : 0.0;
                const auto snare = snareEnvelope * (0.18 * noise() + 0.12 * std::sin (juce::MathConstants<double>::twoPi * 190.0 * t));

                // 踩镲：每个八分音符，高通后的噪声
                const auto hatRaw = noise();
                const auto hat = (hatRaw - lastHat) * 0.5 * std::exp (-eighthPhase / 4.0 * 60.0) * 0.16;
                lastHat = hatRaw;

                // 贝斯：A1 / F1 / C2 / G1 轮换，锯齿（6 个谐波）
                static const double roots[] = { 55.0, 43.65, 65.41, 49.0 };
                const auto root = roots[bar % 4];
                double bass = 0.0;

                for (int k = 1; k <= 6; ++k)
                    bass += std::sin (juce::MathConstants<double>::twoPi * root * k * t) / (double) k;

                bass *= 0.16 * (0.55 + 0.45 * std::exp (-eighthPhase * 3.0));

                // 和弦：根音上的小三和弦 + 八度，左右声道稍微错开
                double chordLeft = 0.0, chordRight = 0.0;

                for (const auto ratio : { 4.0, 4.757, 5.993, 8.0 })
                    for (int k = 1; k <= 4; ++k)
                    {
                        const auto f = root * ratio * k;
                        const auto amplitude = 0.028 / (double) (k * k);
                        chordLeft  += amplitude * std::sin (juce::MathConstants<double>::twoPi * f * t);
                        chordRight += amplitude * std::sin (juce::MathConstants<double>::twoPi * f * 1.002 * t + 0.7);
                    }

                // 整体再压 7 dB：真峰值约 −4 dBTP，像一段还没做母带的混音。
                constexpr double trim = 0.447;
                const auto centre = kick + snare + bass;
                left[i]  = (float) (trim * (centre + chordLeft + hat * 0.8));
                right[i] = (float) (trim * (centre + chordRight + hat * 1.2));
            }
        }

    private:
        double noise() noexcept
        {
            seed = seed * 1664525u + 1013904223u;
            return (double) (seed >> 8) / 8388608.0 - 1.0;
        }

        std::int64_t position = 0;
        double kickPhase = 0.0, lastHat = 0.0;
        std::uint32_t seed = 12345u;
    };

    struct Session
    {
        std::unique_ptr<CeladonAudioProcessor> processor;
        std::unique_ptr<CeladonAudioProcessorEditor> editor;
        Music music;
        juce::AudioBuffer<float> buffer { 2, blockSize };
        juce::MidiBuffer midi;
        juce::Rectangle<int> crop;   // T-038：状态的 after 动作可以指定只存这一块（编辑器坐标）；空 = 照 State::crop

        void setParameter (const char* id, float value)
        {
            if (auto* parameter = processor->getParameters().getParameter (id))
                parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
        }

        // 跑 seconds 秒的音频；每 1600 个样本（约 33 ms）让界面刷新一帧——与真实播放时界面看到的节奏一样。
        void play (double seconds)
        {
            const auto total = (int) (seconds * sampleRate);
            int sinceFrame = 0;

            for (int done = 0; done < total; done += blockSize)
            {
                music.render (buffer, blockSize);
                processor->processBlock (buffer, midi);
                sinceFrame += blockSize;

                if (sinceFrame >= 1600 && editor != nullptr)
                {
                    sinceFrame = 0;
                    editor->advanceForSnapshot (1600.0 / sampleRate);

                    if (! deterministic)
                        dispatchEvents (1);
                }
            }
        }
    };

    //==============================================================================
    // 交互检查（--check）：直接给真实的控件发鼠标事件，核对参数与界面的反应。
    juce::Component* findById (juce::Component& root, const juce::String& id)
    {
        if (root.getComponentID() == id)
            return &root;

        for (auto* child : root.getChildren())
            if (auto* found = findById (*child, id))
                return found;

        return nullptr;
    }

    juce::MouseEvent mouseEvent (juce::Component& target, juce::Point<float> position, juce::ModifierKeys mods,
                                 juce::Point<float> downPosition, int clicks = 1, bool dragged = false)
    {
        const auto now = juce::Time::getCurrentTime();
        return { juce::Desktop::getInstance().getMainMouseSource(), position, mods, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                 &target, &target, now, downPosition, now, clicks, dragged };
    }

    const auto leftButton = juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier);

    void click (juce::Component& target, juce::Point<float> at, juce::ModifierKeys extra = {})
    {
        const auto mods = juce::ModifierKeys (leftButton.getRawFlags() | extra.getRawFlags());
        target.mouseDown (mouseEvent (target, at, mods, at));
        target.mouseUp (mouseEvent (target, at, extra, at));
    }

    void drag (juce::Component& target, juce::Point<float> from, juce::Point<float> to, juce::ModifierKeys extra = {}, bool release = true)
    {
        const auto mods = juce::ModifierKeys (leftButton.getRawFlags() | extra.getRawFlags());
        target.mouseEnter (mouseEvent (target, from, extra, from));
        target.mouseDown (mouseEvent (target, from, mods, from));

        for (int step = 1; step <= 10; ++step)
            target.mouseDrag (mouseEvent (target, from + (to - from) * ((float) step / 10.0f), mods, from, 1, true));

        if (release)
            target.mouseUp (mouseEvent (target, to, extra, from, 1, true));
    }

    //==============================================================================
    // 检查与基准共用：挂在组件上的 paint 计时（JUCE 9 的 ComponentPaintDiagnostics）。cacheReads = 这次从组件缓存图读
    //（setBufferedToImage / AlignedComponentCache），cacheWrites = 这次缓存作废、重画进缓存图。
    struct PaintProbe  : public juce::ComponentListener
    {
        juce::String name;
        double totalMs = 0.0;
        int paints = 0, cacheReads = 0, cacheWrites = 0;

        void componentPainted (juce::Component&, const juce::ComponentPaintDiagnostics& diagnostics) override
        {
            totalMs += diagnostics.totalPaintDuration.get<std::chrono::duration<double, std::milli>>();
            ++paints;
            cacheReads += diagnostics.readFromCache ? 1 : 0;
            cacheWrites += diagnostics.wroteToCache ? 1 : 0;
        }

        void reset()
        {
            totalMs = 0.0;
            paints = 0;
            cacheReads = 0;
            cacheWrites = 0;
        }
    };

    void collectDescendants (juce::Component& root, std::vector<juce::Component*>& out)
    {
        for (auto* child : root.getChildren())
        {
            out.push_back (child);
            collectDescendants (*child, out);
        }
    }

    template <typename ComponentType>
    ComponentType* findFirst (juce::Component& root)
    {
        std::vector<juce::Component*> all;
        collectDescendants (root, all);

        for (auto* component : all)
            if (auto* found = dynamic_cast<ComponentType*> (component))
                return found;

        return nullptr;
    }

    // 画一帧到离屏图：clipLogical 给了就只标脏那一块（编辑器逻辑坐标）。返回毫秒。
    double paintFrame (juce::Component& editor, juce::Image& target, float scale, std::optional<juce::Rectangle<int>> clipLogical)
    {
        juce::Graphics g (target);

        if (! juce::approximatelyEqual (scale, 1.0f))
            g.addTransform (juce::AffineTransform::scale (scale));

        if (clipLogical.has_value())
            g.reduceClipRegion (*clipLogical);

        const auto started = juce::Time::getMillisecondCounterHiRes();
        editor.paintEntireComponent (g, true);
        return juce::Time::getMillisecondCounterHiRes() - started;
    }

    // 同上，裁剪区是一组矩形（窗口系统的脏矩形列表，编辑器逻辑坐标；T-051 的增量重画检查用）。
    void paintRegion (juce::Component& editor, juce::Image& target, float scale, const juce::RectangleList<int>& clipLogical)
    {
        juce::Graphics g (target);

        if (! juce::approximatelyEqual (scale, 1.0f))
            g.addTransform (juce::AffineTransform::scale (scale));

        if (g.reduceClipRegion (clipLogical))
            editor.paintEntireComponent (g, true);
    }

    // 一个频谱仪的三个图例开关（OUT / IN / PEAK，SpectrumView 构造时按这个顺序加的 ClickableComponent 子组件）；找不到返回空。
    std::vector<celadon::ui::ClickableComponent*> legendToggles (celadon::ui::SpectrumView& view)
    {
        std::vector<celadon::ui::ClickableComponent*> toggles;

        for (auto* child : view.getChildren())
            if (auto* toggle = dynamic_cast<celadon::ui::ClickableComponent*> (child))
                toggles.push_back (toggle);

        return toggles.size() == 3 ? toggles : std::vector<celadon::ui::ClickableComponent*> {};
    }

    // 编辑器里频谱仪的三个图例开关；找不到返回空。
    std::vector<celadon::ui::ClickableComponent*> spectrumToggles (juce::Component& editor)
    {
        if (auto* view = findFirst<celadon::ui::SpectrumView> (editor))
            return legendToggles (*view);

        return {};
    }

    juce::String ms (double value)
    {
        return juce::String (value, 3);
    }

    std::unique_ptr<Session> makeSession (float glazePercent)
    {
        auto session = std::make_unique<Session>();
        session->processor.reset (dynamic_cast<CeladonAudioProcessor*> (createPluginFilter()));
        session->processor->setPlayConfigDetails (2, 2, sampleRate, blockSize);
        session->processor->prepareToPlay (sampleRate, blockSize);
        session->setParameter (celadon::param::glazeAmount, glazePercent);
        session->editor.reset (dynamic_cast<CeladonAudioProcessorEditor*> (session->processor->createEditor()));
        return session;
    }

    int runChecks (Session& session, const juce::File& outDir, float scale);
    int runCacheChecks (Session& session, const std::function<void (bool, const juce::String&)>& check);
    void runSpectrumChecks (const std::function<void (bool, const juce::String&)>& check);
    void runClayGuardChecks (const std::function<void (bool, const juce::String&)>& check);
    void runBench (int frames);
    void runSpectrumBench (int seconds);
    void runSpectrumPeakBench();
    void runDigest();

    // 用户设置文件跨进程保留（README「用法」末段）：上一次出图留下的值（zh-zoom70 / zh-zoom150 把默认大小写成 0.7 / 1.5，
    // zh-reduced 留下减少动效）会被下一次运行的新编辑器读进来。检查、基准、摘要开跑前一律先放回这一份基线（T-039）。
    void resetUiSettings (celadon::ui::UiSettings& settings)
    {
        settings.setLanguage (celadon::Language::chinese);
        settings.setReduceMotion (false);
        settings.setDefaultZoom (1.0);
    }

    // T-100：旋钮的读数带弹簧，这里不跑消息循环、弹簧停在半路；存图之前一律停稳（读数落到参数值、彗尾清零），
    // 存下的就是真机静止时的样子，与没有动画时逐像素相同。
    void settleDials (juce::Component& root)
    {
        std::vector<juce::Component*> all { &root };
        collectDescendants (root, all);

        for (auto* component : all)
            if (auto* dial = dynamic_cast<celadon::ui::DialControl*> (component))
                dial->finishMotion();
    }

    bool save (juce::Component& component, float scale, const juce::File& file, juce::Rectangle<int> area = {})
    {
        settleDials (component);

        // area 为空 = 整个组件；否则只存这一块（T-038 的状态行 / 气泡截图只要局部，图小）。
        const auto image = component.createComponentSnapshot (area.isEmpty() ? component.getLocalBounds() : area, true, scale);
        file.deleteFile();
        juce::FileOutputStream stream (file);

        if (! stream.openedOk())
            return false;

        juce::PNGImageFormat png;
        return png.writeImageToStream (image, stream);
    }

    //==============================================================================
    // T-038 曲风与交付 / House 档的截图（README「T-038」一节）。House 档的 house* 字段由编排器（阶段 2 的 B 线）填，截图时把处理器的
    // 状态快照改一改再交给编辑器（setGlazeStatusSource 是公开接口）：界面只按字段渲染，这里验的是渲染，不是编排器。
    using StatusEdit = std::function<void (celadon::glaze::Status&)>;

    void injectStatus (Session& s, StatusEdit edit)
    {
        auto* processor = s.processor.get();
        s.editor->setGlazeStatusSource ([processor, edit] { auto status = processor->getGlazeStatus(); edit (status); return status; });
        s.editor->advanceForSnapshot (0.05);
    }

    // T-048：状态行读数直接交给编辑器（c / m / 冷导出 / 守护还没全部进 Status 时，注入快照画不出这几条）。
    void previewLine (Session& s, celadon::ui::glazestatus::Reading reading)
    {
        s.editor->setStatusLinePreview (reading);
        s.editor->advanceForSnapshot (0.05);
    }

    celadon::ui::glazestatus::Reading followReading (bool house)
    {
        celadon::ui::glazestatus::Reading reading;
        reading.confidence = 1.0f;
        reading.currentMainness = 1.0f;
        reading.effectiveTargetLufs = -9.6f;
        reading.resolvedStyle = celadon::glaze::Style::warm;
        reading.houseActive = house;
        return reading;
    }

    celadon::ui::glazestatus::Reading guardReading()
    {
        auto reading = followReading (false);
        reading.guardEngaged = true;
        reading.guardCause = celadon::ui::glazestatus::GuardCause::budget;
        reading.protectionDb = -1.5f;
        return reading;
    }

    celadon::ui::glazestatus::Reading exportReading()
    {
        auto reading = followReading (false);
        reading.coldExport = true;
        reading.confidence = 0.2f;
        return reading;
    }

    // House 反相提示亮着（单声道低频损失 lossDb）。
    StatusEdit housePhaseHint (float lossDb)
    {
        return [lossDb] (celadon::glaze::Status& status)
        {
            status.houseActive = true;
            status.houseLockPending = false;
            status.houseSegmentMode = celadon::glaze::SegmentMode::bars;
            status.houseHeardSegments = 64;
            status.houseFullSegments = 16;
            status.housePhaseHint = true;
            status.housePhaseMonoLossDb = lossDb;
        };
    }

    // 曲风 Deep House、交付 俱乐部（建编辑器之前设：两个选择显示选中项）。
    void houseParameters (Session& s)
    {
        s.setParameter (celadon::param::glazeGenre, 1.0f);
        s.setParameter (celadon::param::glazeDelivery, 2.0f);
    }

    // T-074 修坯守护回收中：手动低切 25 Hz、手动收单 120 Hz 在做，峰值守护把它们的改动量大半收回（保留比 kept，0 ～ 1）。
    // 界面只按字段渲染（低切 / 收单两盏灯琥珀、气泡第 2 行末尾写还剩多少），这里验的是渲染，不是守护（守护与编排器由 ClayTests / GlazeControllerTests 验）。
    StatusEdit clayPeakGuard (float kept)
    {
        return [kept] (celadon::glaze::Status& status)
        {
            status.clayLowCutHz = 25.0f;
            status.clayMonoHz = 120.0f;
            status.clayMonoAmount = 1.0f;
            status.clayPeakGuardActive = true;
            status.clayPeakGuardKept = kept;
        };
    }

    // 把 CLAY 灯的气泡（修坯 · 立体声像与超低频清理：第 2 行是此刻的实际值，守护回收中时末尾多一段）手动弹在 CLAY 卡片的右边缘旁（压在 COLOR
    // 卡片上，不盖住 CLAY 的灯与数值；锚点在编辑器底部时气泡会翻到上方、盖住行里的字，英文版字多就是这样，所以不用卡片内的锚点）；
    // 文字取 celadon::help::clayIndicatorTooltip（与界面同一个函数、同一份读数），截图存卡片与气泡。
    void showClayIndicatorTooltip (Session& s, celadon::Language language, float kept)
    {
        using namespace celadon::ui;
        auto* window = findFirst<juce::TooltipWindow> (*s.editor);
        auto* card = findFirst<ClayCard> (*s.editor);

        if (window == nullptr || card == nullptr || window->getParentComponent() == nullptr)
        {
            std::cout << "FAILED to find the CLAY card or the tooltip window" << std::endl;
            return;
        }

        auto status = s.processor->getGlazeStatus();
        clayPeakGuard (kept) (status);
        const auto text = celadon::help::clayIndicatorTooltip (celadon::help::clayReadoutFor (status, {}), language);
        const auto anchor = window->getParentComponent()->localPointToGlobal (card->getBounds().getTopRight() + juce::Point<int> (-16, 60));
        window->displayTip (anchor, text);
        s.crop = window->getBounds().getUnion (card->getBounds()).expanded (16).getIntersection (s.editor->getLocalBounds());
    }

    // 暂定期间的悬停气泡：取 GLAZE 标题此刻的气泡文字（编辑器定时器按 House 状态换好的那一版），在标题左下方手动弹出；
    // 截图只存标题与气泡那一块（编辑器 1 倍时画布坐标 = 编辑器坐标）。
    void showGlazeTooltip (Session& s)
    {
        auto* title = findById (*s.editor, "glaze_title");
        auto* window = findFirst<juce::TooltipWindow> (*s.editor);
        auto* client = dynamic_cast<juce::SettableTooltipClient*> (title);

        if (title == nullptr || window == nullptr || client == nullptr || window->getParentComponent() == nullptr)
        {
            std::cout << "FAILED to find the GLAZE title tooltip" << std::endl;
            return;
        }

        const auto anchor = window->getParentComponent()->localPointToGlobal (juce::Point<int> (440, 326));
        window->displayTip (anchor, client->getTooltip());
        s.crop = window->getBounds().getUnion (title->getBounds()).expanded (16).getIntersection (s.editor->getLocalBounds());
    }
}

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI gui;

    juce::File outDir = juce::File::getCurrentWorkingDirectory();
    float scale = 1.0f;
    int benchFrames = 120;
    int benchSeconds = 3;
    juce::StringArray only;

    for (int i = 1; i < argc; ++i)
    {
        const juce::String arg (argv[i]);

        if (arg == "--out" && i + 1 < argc)         outDir = juce::File::getCurrentWorkingDirectory().getChildFile (argv[++i]);
        else if (arg == "--scale" && i + 1 < argc)  scale = (float) juce::String (argv[++i]).getDoubleValue();
        else if (arg == "--only" && i + 1 < argc)   only.addTokens (argv[++i], ",", {});
        else if (arg == "--frames" && i + 1 < argc) benchFrames = juce::jmax (1, juce::String (argv[++i]).getIntValue());
        else if (arg == "--seconds" && i + 1 < argc) benchSeconds = juce::jlimit (1, 60, juce::String (argv[++i]).getIntValue());
        else if (arg == "--deterministic")          deterministic = true;
    }

    outDir.createDirectory();
    juce::SharedResourcePointer<celadon::ui::UiSettings> settings;

    if (only.contains ("bench"))
    {
        resetUiSettings (*settings);
        runBench (benchFrames);
        return 0;
    }

    if (only.contains ("spectrum-bench"))
    {
        settings->setLanguage (celadon::Language::chinese);
        settings->setReduceMotion (false);
        settings->setDefaultZoom (1.0);
        runSpectrumBench (benchSeconds);
        return 0;
    }

    if (only.contains ("spectrum-peak-bench"))
    {
        resetUiSettings (*settings);
        runSpectrumPeakBench();
        return 0;
    }

    if (only.contains ("digest"))
    {
        resetUiSettings (*settings);
        runDigest();
        return 0;
    }

    if (only.contains ("check"))
    {
        resetUiSettings (*settings);   // 原先漏了默认大小：留下 0.7 / 1.5 时编辑器按那个大小打开，T-032 的缓存检查随之失败（T-039）

        Session session;
        session.processor.reset (dynamic_cast<CeladonAudioProcessor*> (createPluginFilter()));
        session.processor->setPlayConfigDetails (2, 2, sampleRate, blockSize);
        session.processor->prepareToPlay (sampleRate, blockSize);
        session.setParameter (celadon::param::glazeAmount, 45.0f);
        session.editor.reset (dynamic_cast<CeladonAudioProcessorEditor*> (session.processor->createEditor()));
        session.play (3.0);

        const auto failures = runChecks (session, outDir, scale);
        session.editor->setReleasePreviewEnabled (true);
        session.editor.reset();
        const auto closedSafely = ! session.processor->isReleasePreviewEnabled();
        std::cout << (closedSafely ? "PASS" : "FAIL") << " editor close clears release preview" << std::endl;
        return failures == 0 && closedSafely ? 0 : 1;
    }

    struct State
    {
        const char* name;
        celadon::Language language;
        std::function<void (Session&)> before;   // 建编辑器之前（参数、设置）
        std::function<void (Session&)> after;    // 跑完音频之后（界面动作）
        juce::Rectangle<int> crop {};            // 只存这一块（编辑器坐标，1 倍）；空 = 整张
    };

    const auto none = [] (Session&) {};

    // T-038 局部截图的范围：顶栏状态胶囊（居中 x = 600、最宽 560）；版面那几张只要上半（顶栏、频谱、中层到底排，y < 560），
    // 卡片与位置无关、图小一半。气泡那两张的范围由 showGlazeTooltip 按气泡实际大小定。
    const juce::Rectangle<int> pillCrop { 300, 0, 600, 52 };
    const juce::Rectangle<int> upperCrop { 0, 0, 1200, 560 };
    const juce::Rectangle<int> cardsCrop { 0, 540, 1200, 280 };   // T-074：下半部分四张模块卡片（CLAY 的灯色在这里）

    const State states[] =
    {
        { "zh-default",  celadon::Language::chinese, none, none },
        { "en-default",  celadon::Language::english, none, none },
        { "zh-original", celadon::Language::chinese,
          [] (Session& s) { s.setParameter (celadon::param::limitListen, 1.0f); },
          [] (Session& s) { s.editor->setListeningToOriginal (true); s.play (1.5); } },
        { "zh-audition", celadon::Language::chinese,
          [] (Session& s) { s.setParameter (celadon::param::limitGain, 8.0f); },   // 推 +8 dB：限幅器结实地在压，差值听得见也看得见
          [] (Session& s) { s.editor->setAuditioningDifference (true); s.play (1.5); } },
        { "zh-truepeak", celadon::Language::chinese,
          [] (Session& s) { s.setParameter (celadon::param::limitBypass, 1.0f); s.setParameter (celadon::param::colorOutput, 6.0f); }, none },
        { "zh-settings", celadon::Language::chinese, none, [] (Session& s) { s.editor->showSettings (true); } },
        { "en-help",     celadon::Language::english, none, [] (Session& s) { s.editor->showHelp (true); } },
        { "zh-help",     celadon::Language::chinese, none, [] (Session& s) { s.editor->showHelp (true); } },   // T-100：旋钮那条变长，中文也要看
        { "zh-advanced", celadon::Language::chinese, none, [] (Session& s) { s.editor->showAdvanced (true); } },
        { "en-advanced", celadon::Language::english, none, [] (Session& s) { s.editor->showAdvanced (true); } },
        { "zh-target", celadon::Language::chinese, none, [] (Session& s)
          {
              s.editor->showAdvanced (true);
              if (auto* tabs = dynamic_cast<celadon::ui::Segmented*> (findById (*s.editor, "adv_tabs")))
                  tabs->setSelected (1, true);
              s.processor->requestReferenceLearn();
              s.play (0.1);
          } },
        { "en-release", celadon::Language::english, none, [] (Session& s)
          {
              s.editor->showAdvanced (true);
              if (auto* tabs = dynamic_cast<celadon::ui::Segmented*> (findById (*s.editor, "adv_tabs")))
                  tabs->setSelected (2, true);
              s.editor->setReleasePreviewEnabled (true);
              s.play (1.0);
          } },
        { "zh-preview", celadon::Language::chinese, none, [] (Session& s)
          { s.editor->setReleasePreviewEnabled (true); s.play (1.0); } },
        { "zh-glaze90",  celadon::Language::chinese, [] (Session& s) { s.setParameter (celadon::param::glazeAmount, 90.0f); }, none },
        { "zh-glaze0",   celadon::Language::chinese, [] (Session& s) { s.setParameter (celadon::param::glazeAmount, 0.0f); }, none },
        { "zh-zoom70",   celadon::Language::chinese, none, [] (Session& s) { s.editor->setZoom (0.7); } },
        { "zh-zoom150",  celadon::Language::chinese, none, [] (Session& s) { s.editor->setZoom (1.5); } },
        { "zh-reduced",  celadon::Language::chinese, [] (Session&) {}, none },
        // T-051：频谱仪 IN 虚线开着（默认关；打开后 IN 那路到下一次计算才有曲线，再放 1.5 s）。--deterministic 下改前改后逐字节比虚线。
        { "zh-input",    celadon::Language::chinese, none, [] (Session& s)
          {
              const auto toggles = spectrumToggles (*s.editor);

              if (toggles.empty())
                  std::cout << "FAILED to find the spectrum legend toggles" << std::endl;
              else
                  click (*toggles[1], toggles[1]->getLocalBounds().getCentre().toFloat());

              s.play (1.5);
          } },

        // ---- T-074 修坯守护回收中：手动低切 / 收单的灯琥珀、气泡写还剩多少（状态字段注入，见 clayPeakGuard）----
        { "zh-guard-recovering", celadon::Language::chinese, none, [] (Session& s) { injectStatus (s, clayPeakGuard (0.42f)); }, cardsCrop },
        { "en-guard-recovering", celadon::Language::english, none, [] (Session& s) { injectStatus (s, clayPeakGuard (0.42f)); }, cardsCrop },
        { "zh-guard-tooltip", celadon::Language::chinese, none,
          [] (Session& s) { injectStatus (s, clayPeakGuard (0.42f)); showClayIndicatorTooltip (s, celadon::Language::chinese, 0.42f); } },
        { "en-guard-tooltip", celadon::Language::english, none,
          [] (Session& s) { injectStatus (s, clayPeakGuard (0.42f)); showClayIndicatorTooltip (s, celadon::Language::english, 0.42f); } },

        // ---- T-038 曲风与交付 / House 档（曲风 Deep House、交付 俱乐部；House 的状态字段注入，见 injectStatus）----
        // 版面：两个选择在 GLAZE 标题行左右（Max 2026-10-01 选定），整张。
        { "zh-house-layout", celadon::Language::chinese, houseParameters, [] (Session& s) { previewLine (s, followReading (true)); }, upperCrop },
        { "en-house-layout", celadon::Language::english, houseParameters, [] (Session& s) { previewLine (s, followReading (true)); }, upperCrop },
        // T-048 七条里截图工具单独摆出来的三句（起步是默认图；反相提示仍是 -house-hint）。只截顶栏胶囊。
        { "zh-status-follow", celadon::Language::chinese, none, [] (Session& s) { previewLine (s, followReading (false)); }, pillCrop },
        { "en-status-follow", celadon::Language::english, none, [] (Session& s) { previewLine (s, followReading (false)); }, pillCrop },
        { "zh-status-guard", celadon::Language::chinese, none, [] (Session& s) { previewLine (s, guardReading()); }, pillCrop },
        { "en-status-guard", celadon::Language::english, none, [] (Session& s) { previewLine (s, guardReading()); }, pillCrop },
        { "zh-status-export", celadon::Language::chinese, none, [] (Session& s) { previewLine (s, exportReading()); }, pillCrop },
        { "en-status-export", celadon::Language::english, none, [] (Session& s) { previewLine (s, exportReading()); }, pillCrop },
        // 旧工程提示一次：左下角琥珀色第二行，不进状态行。
        { "zh-legacy-notice", celadon::Language::chinese, none, [] (Session& s) { s.editor->noteDiscardedLegacyGlaze(); }, juce::Rectangle<int> (0, 470, 520, 90) },
        { "en-legacy-notice", celadon::Language::english, none, [] (Session& s) { s.editor->noteDiscardedLegacyGlaze(); }, juce::Rectangle<int> (0, 470, 560, 90) },
        // House 反相提示：状态胶囊（琥珀）+ CLAY 卡片两行提示，整张。
        { "zh-house-hint", celadon::Language::chinese, houseParameters, [] (Session& s) { injectStatus (s, housePhaseHint (-1.62f)); } },
        { "en-house-hint", celadon::Language::english, houseParameters, [] (Session& s) { injectStatus (s, housePhaseHint (-1.62f)); } },
    };

    for (const auto& state : states)
    {
        if (! only.isEmpty() && ! only.contains (state.name))
            continue;

        settings->setLanguage (state.language);
        settings->setReduceMotion (juce::String (state.name) == "zh-reduced");
        settings->setDefaultZoom (1.0);

        Session session;
        session.processor.reset (dynamic_cast<CeladonAudioProcessor*> (createPluginFilter()));
        session.processor->setPlayConfigDetails (2, 2, sampleRate, blockSize);
        session.processor->prepareToPlay (sampleRate, blockSize);
        session.setParameter (celadon::param::glazeAmount, 45.0f);
        state.before (session);

        session.editor.reset (dynamic_cast<CeladonAudioProcessorEditor*> (session.processor->createEditor()));

        // 先让 GLAZE 听完（约 30 s 音乐），再跑 2 s 让表与频谱稳定。
        session.play (30.0);
        state.after (session);
        session.editor->advanceForSnapshot (0.05);

        const auto file = outDir.getChildFile (juce::String (state.name) + ".png");
        const auto ok = save (*session.editor, scale, file, session.crop.isEmpty() ? state.crop : session.crop);
        std::cout << (ok ? "wrote " : "FAILED ") << file.getFullPathName() << " (" << session.editor->getWidth() << " x "
                  << session.editor->getHeight() << ")" << std::endl;

        session.editor.reset();
        session.processor->releaseResources();
    }

    return 0;
}

namespace
{
    int runChecks (Session& session, const juce::File& outDir, float scale)
    {
        auto& editor = *session.editor;
        auto& state = session.processor->getParameters();
        int failures = 0;

        const auto value = [&] (const char* id) { return state.getRawParameterValue (id)->load(); };
        const auto check = [&] (bool ok, const juce::String& what)
        {
            std::cout << (ok ? "PASS " : "FAIL ") << what << std::endl;
            failures += ok ? 0 : 1;
        };
        const auto find = [&] (const juce::String& id) -> juce::Component&
        {
            auto* component = findById (editor, id);

            if (component == nullptr)
            {
                std::cout << "FAIL missing component " << id << std::endl;
                std::exit (2);
            }

            return *component;
        };

        // 前提：编辑器按 1 倍打开（1200 × 820）。后面的坐标都按 1 倍——⑬～⑮ 的粒子带裁剪框 (0, 300, 1200, 260) 直接用编辑器坐标，
        // ⑲ 要的「只作废圆点那一块」也只在物理像素比为整数时成立。默认大小不是 1、或虚拟屏幕放不下 1 倍（编辑器自动缩到屏幕里，
        // 1280 × 1024 下 2 倍会变成 1.033 倍）时，缓存检查会报出与真实原因无关的失败（T-039：0.7 倍时裁剪框落在 GLAZE 标题下方、
        // 「从缓存读 0 次」；小数像素比下圆点作废后重画的裁剪框是整颗胶囊、「排版了 3 次字」），这里先拦下、不往下跑。
        check (editor.getWidth() == CeladonAudioProcessorEditor::baseWidth && editor.getHeight() == CeladonAudioProcessorEditor::baseHeight,
               "checks run at zoom 1 (editor " + juce::String (editor.getWidth()) + " x " + juce::String (editor.getHeight())
                   + "; needs 1200 x 820 and a virtual screen of at least 1240 x 900)");

        if (failures > 0)
            return failures;

        auto& knob = find (celadon::param::glazeAmount);
        auto& bubble = find ("value_bubble");
        auto& corner = find ("corner_note");
        const juce::Point<float> centre (112.0f, 112.0f);

        // ① 上下拖 GLAZE：250 px 走满量程，往上 50 px = +20 %；拖动中出气泡、左下角换成操作提示。
        drag (knob, centre, centre.translated (0.0f, -50.0f), {}, false);
        check (std::abs (value (celadon::param::glazeAmount) - 65.0f) < 0.3f, "drag GLAZE +50 px -> 65 % (" + juce::String (value (celadon::param::glazeAmount), 2) + ")");
        check (bubble.isVisible(), "value bubble visible while dragging");
        check (static_cast<celadon::ui::CornerNote&> (corner).isShowingHint(), "corner note shows the interaction hint while dragging");
        save (editor, scale, outDir.getChildFile ("check-drag.png"));
        knob.mouseUp (mouseEvent (knob, centre.translated (0.0f, -50.0f), {}, centre, 1, true));
        check (! bubble.isVisible(), "value bubble hidden after release");

        // ② ⌘ / Ctrl 精调：同样 50 px 只走 1 / 8。
        drag (knob, centre, centre.translated (0.0f, -50.0f), juce::ModifierKeys (juce::ModifierKeys::commandModifier));
        check (std::abs (value (celadon::param::glazeAmount) - 67.5f) < 0.3f, "fine drag +50 px -> 67.5 % (" + juce::String (value (celadon::param::glazeAmount), 2) + ")");

        // ③ 双击回默认 35 %。
        knob.mouseDown (mouseEvent (knob, centre, leftButton, centre));
        knob.mouseUp (mouseEvent (knob, centre, {}, centre));
        knob.mouseDown (mouseEvent (knob, centre, leftButton, centre, 2));
        knob.mouseDoubleClick (mouseEvent (knob, centre, leftButton, centre, 2));
        knob.mouseUp (mouseEvent (knob, centre, {}, centre, 2));
        check (std::abs (value (celadon::param::glazeAmount) - 35.0f) < 0.05f, "double-click -> default 35 %");

        // ④ Alt 单击直接输入 60，回车确认。
        click (knob, centre, juce::ModifierKeys (juce::ModifierKeys::altModifier));
        auto& entry = static_cast<juce::TextEditor&> (find ("value_entry"));
        check (entry.isVisible() && entry.getText() == "35", "Alt-click opens the value entry with the current value (" + entry.getText() + ")");
        // Windows：输入框开着时按键抬起（松开 Alt 的 WM_SYSKEYUP）由画布吃掉，不转给宿主去激活菜单栏；按下照常往上传。
        auto* entryParent = entry.getParentComponent();
        check (entryParent != nullptr && entryParent->keyStateChanged (false) && ! entryParent->keyStateChanged (true),
               "key-ups are consumed while the value entry is open");
        entry.setText ("60");
        entry.onReturnKey();
        check (std::abs (value (celadon::param::glazeAmount) - 60.0f) < 0.05f && ! entry.isVisible(), "typed value 60 + Return is applied");
        check (entryParent != nullptr && ! entryParent->keyStateChanged (false), "key-ups pass through again once the entry is closed");

        // 中文输入法的全角数字与句号（「５５。５」）照样认。
        click (knob, centre, juce::ModifierKeys (juce::ModifierKeys::altModifier));
        entry.setText (juce::String (juce::CharPointer_UTF8 ("\xef\xbc\x95\xef\xbc\x95\xe3\x80\x82\xef\xbc\x95")));
        entry.onReturnKey();
        check (std::abs (value (celadon::param::glazeAmount) - 55.5f) < 0.05f, "full-width IME digits (55.5) are applied (" + juce::String (value (celadon::param::glazeAmount), 2) + ")");
        click (knob, centre, juce::ModifierKeys (juce::ModifierKeys::altModifier));
        entry.setText ("60");
        entry.onReturnKey();

        // ⑤ 滚轮：往上一格变大一点，⌘ 时更少。
        {
            juce::MouseWheelDetails wheel { 0.0f, 0.47f, false, false, false };
            const auto before = value (celadon::param::glazeAmount);
            knob.mouseWheelMove (mouseEvent (knob, centre, {}, centre), wheel);
            const auto coarse = value (celadon::param::glazeAmount) - before;
            knob.mouseWheelMove (mouseEvent (knob, centre, juce::ModifierKeys (juce::ModifierKeys::commandModifier), centre), wheel);
            const auto fine = value (celadon::param::glazeAmount) - before - coarse;
            check (coarse > 0.5f && coarse < 3.0f && fine > 0.0f && fine < coarse, "wheel steps (coarse " + juce::String (coarse, 2) + ", fine " + juce::String (fine, 2) + ")");
        }

        // ⑤b T-100（ADR-037）：旋钮外圈照 CometDial 的手感——点外圈跳过去、沿外圈拖、快甩多滑一段、拖进底部缺口锁在来的那一端、
        // ⌘ 相对精调且松开 ⌘ 不跳、外圈双击回默认、小旋钮同样；读数走弹簧、停稳时精确落在参数值上、彗尾熄灭、定时器停；
        // 减少动效时直接落下。鼠标事件带确定的时间（甩动按事件时间算速度），动画用 advanceMotion 按 1 / 60 s 推，不靠墙钟。
        {
            using celadon::ui::DialControl;
            namespace knobGeometry = celadon::ui::knob;

            auto& glazeDial = static_cast<DialControl&> (knob);
            const auto glazeBefore = value (celadon::param::glazeAmount);
            const auto start = juce::Time::getCurrentTime();
            double clockMs = 0.0;

            const auto event = [&] (juce::Component& target, juce::Point<float> position, juce::ModifierKeys mods, juce::Point<float> down, bool dragged)
            {
                const auto time = start + juce::RelativeTime::milliseconds ((juce::int64) clockMs);
                return juce::MouseEvent { juce::Desktop::getInstance().getMainMouseSource(), position, mods, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                          &target, &target, time, down, time, 1, dragged };
            };
            const auto ringPoint = [] (juce::Point<float> dialCentre, float radius, float proportion)
            {
                return knobGeometry::pointAt (dialCentre, radius, knobGeometry::angleFor (proportion));
            };
            const auto settle = [] (DialControl& dial)
            {
                for (int i = 0; i < 600 && dial.isMotionRunning(); ++i)
                    dial.advanceMotion (1.0 / 60.0);
            };
            const auto settledCleanly = [] (const DialControl& dial)
            {
                return ! dial.isMotionRunning() && dial.isReadingSettled() && juce::exactlyEqual (dial.getCometStrength(), 0.0f);
            };
            const auto tap = [&] (DialControl& dial, juce::Point<float> point)
            {
                dial.mouseDown (event (dial, point, leftButton, point, false));
                dial.mouseUp (event (dial, point, {}, point, false));
                clockMs += 500.0;
            };
            // 沿外圈从 from 拖到 to：steps 步、每步 stepMs，最后一步之后停 pauseMs 再松手。
            const auto ringDrag = [&] (DialControl& dial, juce::Point<float> dialCentre, float radius, float from, float to, int steps,
                                       double stepMs, double pauseMs)
            {
                const auto down = ringPoint (dialCentre, radius, from);
                dial.mouseDown (event (dial, down, leftButton, down, false));

                for (int i = 1; i <= steps; ++i)
                {
                    clockMs += stepMs;
                    dial.mouseDrag (event (dial, ringPoint (dialCentre, radius, from + (to - from) * (float) i / (float) steps), leftButton, down, true));
                }

                clockMs += pauseMs;
                dial.mouseUp (event (dial, ringPoint (dialCentre, radius, to), {}, down, true));
                clockMs += 1000.0;
            };

            const juce::Point<float> glazeCentre (112.0f, 112.0f);
            constexpr float onRing = 100.0f;
            const auto glazeAt = [&] { return value (celadon::param::glazeAmount); };
            const auto percent = [] (float v) { return juce::String (v, 2) + " %"; };

            check (glazeDial.isInRing (ringPoint (glazeCentre, onRing, 0.5f)) && ! glazeDial.isInRing (glazeCentre)
                       && ! glazeDial.isInRing (glazeCentre.translated (0.0f, -50.0f)) && ! glazeDial.isInRing (glazeCentre.translated (0.0f, -111.0f)),
                   "T-100 GLAZE: the ring is 56..110 px from the centre; inside stays up / down drag");

            // ① 点外圈：参数直接到所点的角度，读数从原处起弹簧（还在路上），停稳后精确落在参数值上、彗尾熄灭。
            glazeDial.finishMotion();
            {
                const auto point = ringPoint (glazeCentre, onRing, 0.8f);
                glazeDial.mouseDown (event (glazeDial, point, leftButton, point, false));
                glazeDial.mouseUp (event (glazeDial, point, {}, point, false));
                check (std::abs (glazeAt() - 80.0f) < 0.3f, "T-100 click on the ring jumps to that angle (80 %, got " + percent (glazeAt()) + ")");
                check (glazeDial.isMotionRunning() && ! glazeDial.isReadingSettled(), "T-100 the reading springs there instead of jumping");
                settle (glazeDial);
                check (settledCleanly (glazeDial), "T-100 the spring settles exactly on the value; comet out, timer stopped");
                clockMs += 1000.0;
            }

            // ② 沿外圈拖、停 120 ms 再松手：值 = 指针所在的角度，不多滑。
            ringDrag (glazeDial, glazeCentre, onRing, 0.3f, 0.6f, 15, 16.0, 120.0);
            check (std::abs (glazeAt() - 60.0f) < 0.3f, "T-100 dragging round the ring follows the angle; a pause before release does not glide (60 %, got "
                                                            + percent (glazeAt()) + ")");
            settle (glazeDial);

            // ③ 快甩：0.3 → 0.5，每 10 ms 一步，松手前只停 5 ms。头一步离按下点不到 10 px 不算拖，最后 4 个点的速度 2 量程 / 秒，
            //    落点 0.5 + 2 × 0.099 ≈ 0.698；读数带着松手速度起弹簧，停稳后落在落点上。
            {
                ringDrag (glazeDial, glazeCentre, onRing, 0.3f, 0.5f, 10, 10.0, 5.0);
                check (std::abs (glazeAt() - 69.8f) < 0.5f, "T-100 a quick flick keeps gliding past the release point (released at 50 %, landed "
                                                                + percent (glazeAt()) + ", expected 69.8 %)");
                const auto running = glazeDial.isMotionRunning();
                settle (glazeDial);
                check (running && settledCleanly (glazeDial), "T-100 after a flick the reading springs to the landing and settles there");
            }

            // ④ 从 0.9 拖过终点进底部缺口，一直拖到偏左（离起点更近）的地方：锁在最大，不绕回 0。
            {
                const auto down = ringPoint (glazeCentre, onRing, 0.9f);
                glazeDial.mouseDown (event (glazeDial, down, leftButton, down, false));

                for (const auto angle : { knobGeometry::angleFor (0.95f), knobGeometry::endAngle, 0.9f * juce::MathConstants<float>::pi,
                                          1.08f * juce::MathConstants<float>::pi })
                {
                    clockMs += 16.0;
                    glazeDial.mouseDrag (event (glazeDial, knobGeometry::pointAt (glazeCentre, onRing, angle), leftButton, down, true));
                }

                clockMs += 120.0;
                glazeDial.mouseUp (event (glazeDial, knobGeometry::pointAt (glazeCentre, onRing, 1.08f * juce::MathConstants<float>::pi), {}, down, true));
                clockMs += 1000.0;
                check (glazeAt() > 99.95f, "T-100 dragging past the end into the bottom gap stays at the maximum (got " + percent (glazeAt()) + ")");
                settle (glazeDial);
            }

            // ⑤ ⌘ 在外圈按下：不跳；相对转动、慢 8 倍（转过 0.4 圈的角度只走 +5 %）；拖到一半松开 ⌘：不跳到指针所在的角度，按原速接着转。
            {
                session.setParameter (celadon::param::glazeAmount, 40.0f);
                glazeDial.finishMotion();
                const auto fineMods = juce::ModifierKeys (leftButton.getRawFlags() | juce::ModifierKeys::commandModifier);
                const auto down = ringPoint (glazeCentre, onRing, 0.2f);
                glazeDial.mouseDown (event (glazeDial, down, fineMods, down, false));
                check (std::abs (glazeAt() - 40.0f) < 0.05f, "T-100 a command-press on the ring does not jump (got " + percent (glazeAt()) + ")");

                for (int i = 1; i <= 20; ++i)
                {
                    clockMs += 16.0;
                    glazeDial.mouseDrag (event (glazeDial, ringPoint (glazeCentre, onRing, 0.2f + 0.4f * (float) i / 20.0f), fineMods, down, true));
                }

                const auto afterFine = glazeAt();
                check (std::abs (afterFine - 45.0f) < 0.3f, "T-100 command on the ring turns relative and 8x slower (0.4 of the track -> 45 %, got "
                                                                + percent (afterFine) + ")");

                for (int i = 1; i <= 5; ++i)
                {
                    clockMs += 16.0;
                    glazeDial.mouseDrag (event (glazeDial, ringPoint (glazeCentre, onRing, 0.6f + 0.1f * (float) i / 5.0f), leftButton, down, true));
                }

                check (std::abs (glazeAt() - afterFine - 10.0f) < 0.3f, "T-100 letting go of command mid-drag does not jump (+10 % from "
                                                                            + percent (afterFine) + " to " + percent (glazeAt()) + "; pointer at 70 %)");
                clockMs += 120.0;
                glazeDial.mouseUp (event (glazeDial, ringPoint (glazeCentre, onRing, 0.7f), {}, down, true));
                clockMs += 1000.0;
                settle (glazeDial);
            }

            // ⑥ 外圈双击回默认 35 %（第一下点按先跳到所点角度）。
            {
                const auto point = ringPoint (glazeCentre, onRing, 0.7f);
                tap (glazeDial, point);
                glazeDial.mouseDown (event (glazeDial, point, leftButton, point, false));
                glazeDial.mouseDoubleClick (event (glazeDial, point, leftButton, point, false));
                glazeDial.mouseUp (event (glazeDial, point, {}, point, false));
                check (std::abs (glazeAt() - 35.0f) < 0.05f, "T-100 double-click on the ring -> default 35 % (got " + percent (glazeAt()) + ")");
                settle (glazeDial);
            }

            // ⑦ 小旋钮（COLOR 粘合，0 ～ 100）：外圈点按、外圈拖同样生效；帽子上照旧上下拖（180 px 满量程）。
            {
                auto& glue = static_cast<DialControl&> (find (celadon::param::colorGlue));
                const auto glueBefore = value (celadon::param::colorGlue);
                const juce::Point<float> c (36.0f, 48.0f);
                constexpr float r = 23.0f;
                const auto glueAt = [&] { return value (celadon::param::colorGlue); };

                check (glue.isInRing (ringPoint (c, r, 0.5f)) && ! glue.isInRing (c) && ! glue.isInRing (c.translated (0.0f, 16.0f))
                           && ! glue.isInRing ({ 36.0f, 89.0f }),
                       "T-100 small knob: the ring is 17..31 px from the centre; the cap and the value row stay up / down drag");

                tap (glue, ringPoint (c, r, 0.75f));
                check (std::abs (glueAt() - 75.0f) < 0.3f, "T-100 small knob: click on the ring jumps there (75, got " + juce::String (glueAt(), 2) + ")");
                ringDrag (glue, c, r, 0.75f, 0.25f, 12, 16.0, 120.0);
                check (std::abs (glueAt() - 25.0f) < 0.3f, "T-100 small knob: dragging round the ring follows the angle (25, got " + juce::String (glueAt(), 2) + ")");
                drag (glue, c, c.translated (0.0f, -45.0f));
                check (std::abs (glueAt() - 50.0f) < 0.3f, "T-100 small knob: the cap still drags up / down, 45 px = +25 (got " + juce::String (glueAt(), 2) + ")");
                settle (glue);
                check (settledCleanly (glue), "T-100 small knob settles exactly on its value");

                // 外圈拖动的门槛按轨道半径换算（小旋钮 4 px）：沿外圈挪约 5.4 px（0.5 → 0.55）就开始跟着走，不再等转过约 25°。
                {
                    const auto down = ringPoint (c, r, 0.5f);
                    glue.mouseDown (event (glue, down, leftButton, down, false));
                    clockMs += 16.0;
                    glue.mouseDrag (event (glue, ringPoint (c, r, 0.55f), leftButton, down, true));
                    const auto followed = glueAt();
                    clockMs += 120.0;
                    glue.mouseUp (event (glue, ringPoint (c, r, 0.55f), {}, down, true));
                    clockMs += 1000.0;
                    check (std::abs (followed - 55.0f) < 0.3f, "T-100 small knob: a 5 px move along the ring already counts as a drag (55, got "
                                                                   + juce::String (followed, 2) + ")");
                    settle (glue);
                }
                session.setParameter (celadon::param::colorGlue, glueBefore);
                glue.finishMotion();
            }

            // ⑧ 减少动效：读数直接落下，没有彗尾，也不起定时器。
            {
                const auto reducedBefore = glazeDial.getReduceMotion();
                glazeDial.setReduceMotion (true);
                tap (glazeDial, ringPoint (glazeCentre, onRing, 0.25f));
                check (std::abs (glazeAt() - 25.0f) < 0.3f && settledCleanly (glazeDial),
                       "T-100 reduce motion: the reading lands at once, no comet, no timer");

                // 同 ③ 的快甩：减少动效时不多滑（与改前一样没有惯性），停在松手处 50 %。
                ringDrag (glazeDial, glazeCentre, onRing, 0.3f, 0.5f, 10, 10.0, 5.0);
                check (std::abs (glazeAt() - 50.0f) < 0.3f && settledCleanly (glazeDial),
                       "T-100 reduce motion: a quick flick does not glide (released at 50 %, got " + percent (glazeAt()) + ")");
                glazeDial.setReduceMotion (reducedBefore);
            }

            // ⑨ 中间拖到一半按住不动：读数追上手、彗尾熄了，动画就停（按钮还按着也停，不再每帧重画）；松手后照常停稳。
            {
                const auto down = glazeCentre;
                glazeDial.mouseDown (event (glazeDial, down, leftButton, down, false));

                for (int i = 1; i <= 6; ++i)
                {
                    clockMs += 16.0;
                    glazeDial.mouseDrag (event (glazeDial, down.translated (0.0f, -5.0f * (float) i), leftButton, down, true));
                    glazeDial.advanceMotion (1.0 / 60.0);
                }

                int frames = 0;

                for (; frames < 600 && glazeDial.isMotionRunning(); ++frames)
                    glazeDial.advanceMotion (1.0 / 60.0);

                check (frames < 600 && ! glazeDial.isMotionRunning() && juce::exactlyEqual (glazeDial.getCometStrength(), 0.0f),
                       "T-100 holding still mid-drag: the reading catches up, the comet fades and the timer stops with the button still down (after "
                           + juce::String (frames) + " frames)");

                clockMs += 200.0;
                glazeDial.mouseUp (event (glazeDial, down.translated (0.0f, -30.0f), {}, down, true));
                clockMs += 1000.0;
                settle (glazeDial);
                check (settledCleanly (glazeDial), "T-100 releasing after holding still settles exactly on the value");
            }

            // ⑩ 弹簧正在跑时滚一下滚轮：读数直接跳过去，这一跳不算成速度——下一帧彗尾不猛闪，只按原来的速度慢慢熄。
            {
                session.setParameter (celadon::param::glazeAmount, 60.0f);
                glazeDial.finishMotion();
                tap (glazeDial, ringPoint (glazeCentre, onRing, 0.3f));   // 弹簧从 60 % 往 30 % 走
                glazeDial.advanceMotion (1.0 / 60.0);
                glazeDial.advanceMotion (1.0 / 60.0);
                const auto before = glazeDial.getCometStrength();

                const juce::MouseWheelDetails wheelDown { 0.0f, -0.47f, false, false, false };
                glazeDial.mouseWheelMove (event (glazeDial, glazeCentre, {}, glazeCentre, false), wheelDown);
                glazeDial.advanceMotion (1.0 / 60.0);
                const auto after = glazeDial.getCometStrength();
                check (before > 0.0f && after < before, "T-100 a wheel step while the spring runs does not flash the comet (strength "
                                                            + juce::String (before, 3) + " -> " + juce::String (after, 3) + ")");
                settle (glazeDial);
                check (settledCleanly (glazeDial), "T-100 after a wheel step the dial settles exactly on the value");
            }

            session.setParameter (celadon::param::glazeAmount, glazeBefore);
            glazeDial.finishMotion();
        }

        // ⑥ 行滑条：点在轨道正中 → 50 %。
        {
            auto& mix = find (celadon::param::colorMix);
            const auto trackLeft = (float) (celadon::ui::ParamSlider::nameWidth + celadon::ui::ParamSlider::gap);
            const auto trackRight = (float) mix.getWidth() - (float) (celadon::ui::ParamSlider::valueWidth + celadon::ui::ParamSlider::gap);
            click (mix, { 0.5f * (trackLeft + trackRight), 9.0f });
            check (std::abs (value (celadon::param::colorMix) - 50.0f) < 1.0f, "slider click at the middle -> Mix 50 % (" + juce::String (value (celadon::param::colorMix), 1) + ")");
        }

        // ⑦ 分段选择：风格点第 3 个（冲击）；CLAY 低切切到「手动」后数值可以拖。
        {
            auto& style = find (celadon::param::glazeStyle);
            click (style, { (float) style.getWidth() * 0.625f, 14.0f });
            check (juce::roundToInt (value (celadon::param::glazeStyle)) == 2, "style segment 3 -> Punchy");

            auto& lowCutMode = find (celadon::param::clayLowCutMode);
            click (lowCutMode, { (float) lowCutMode.getWidth() * 0.85f, 12.0f });
            check (juce::roundToInt (value (celadon::param::clayLowCutMode)) == 2, "Clay low cut -> Manual");

            auto& lowCutValue = find (celadon::param::clayLowCutHz);
            const auto before = value (celadon::param::clayLowCutHz);
            drag (lowCutValue, { 10.0f, 7.0f }, { 10.0f, -29.0f });
            check (value (celadon::param::clayLowCutHz) > before + 3.0f, "manual low cut value drags (" + juce::String (before, 1) + " -> "
                                                                         + juce::String (value (celadon::param::clayLowCutHz), 1) + " Hz)");
        }

        // ⑦ 续（T-038）：曲风 / 交付两个选择——点最右一格选中最后一项（Melodic / 俱乐部），点左边的灰字说明格什么都不改；
        //    两个都在 GLAZE 标题行两侧、不与主旋钮和左下说明重叠。查完放回通用 / 标准，后面的检查不受影响。
        {
            auto& genre = find (celadon::param::glazeGenre);
            auto& delivery = find (celadon::param::glazeDelivery);
            const auto genreY = (float) genre.getHeight() * 0.5f, deliveryY = (float) delivery.getHeight() * 0.5f;

            click (genre, { (float) genre.getWidth() - 12.0f, genreY });
            check (juce::roundToInt (value (celadon::param::glazeGenre)) == 3, "genre: last segment -> Melodic-Progressive");
            click (genre, { 12.0f, genreY });
            check (juce::roundToInt (value (celadon::param::glazeGenre)) == 3, "genre: the caption cell is not a choice");
            click (delivery, { (float) delivery.getWidth() - 12.0f, deliveryY });
            check (juce::roundToInt (value (celadon::param::glazeDelivery)) == 2, "delivery: last segment -> Club");

            const auto knobArea = juce::Rectangle<int> (500, 322, 200, 200);
            const auto cornerArea = find ("corner_note").getBounds();
            check (! genre.getBounds().intersects (knobArea) && ! delivery.getBounds().intersects (knobArea)
                       && ! genre.getBounds().intersects (cornerArea) && genre.getRight() <= delivery.getX(),
                   "genre / delivery sit either side of the GLAZE heading, clear of the knob (" + genre.getBounds().toString() + ", " + delivery.getBounds().toString() + ")");

            session.setParameter (celadon::param::glazeGenre, 0.0f);
            session.setParameter (celadon::param::glazeDelivery, 0.0f);
            check (static_cast<celadon::ui::Segmented&> (genre).getSelected() == 0 && static_cast<celadon::ui::Segmented&> (delivery).getSelected() == 0,
                   "host changes show up on both selectors");
        }

        // ⑧ 电源键旁通、AUTO 小胶囊、等响、A / B。
        {
            click (find (celadon::param::colorBypass), { 11.0f, 11.0f });
            check (value (celadon::param::colorBypass) > 0.5f, "COLOR power button bypasses");
            click (find (celadon::param::colorBypass), { 11.0f, 11.0f });

            click (find (celadon::param::limitReleaseAuto), { 10.0f, 9.0f });
            check (value (celadon::param::limitReleaseAuto) < 0.5f, "AUTO chip turns auto release off");

            click (find ("match"), { 20.0f, 14.0f });
            check (value (celadon::param::limitListen) > 0.5f, "Match turns loudness-matched listening on");

            auto& ab = find ("ab");
            click (ab, { (float) ab.getWidth() - 20.0f, 14.0f });
            check (session.processor->isComparingOriginal(), "A / B -> B listens to the original");
            session.play (1.0);
            save (editor, scale, outDir.getChildFile ("check-original.png"));
            click (ab, { 20.0f, 14.0f });
            check (! session.processor->isComparingOriginal(), "A / B -> back to A");
        }

        // ⑨ 试听差值：点 LIMIT 的耳机 → 处理器在试听；点 A / B 的 B → 自动退出；在 B 时点耳机 → 回到 A 并试听；
        //    旁通 LIMIT → 退出、耳机禁用；LIMIT 开回来耳机可用。
        {
            auto& headphones = find (celadon::uiid::limitAudition);
            click (headphones, { 11.0f, 11.0f });
            check (session.processor->isAuditioningDifference(), "LIMIT headphones -> listening to the difference");
            session.play (1.0);
            save (editor, scale, outDir.getChildFile ("check-audition.png"));

            auto& ab = find ("ab");
            click (ab, { (float) ab.getWidth() - 20.0f, 14.0f });
            check (session.processor->isComparingOriginal() && ! session.processor->isAuditioningDifference(),
                   "B turns the difference audition off");

            click (headphones, { 11.0f, 11.0f });
            check (session.processor->isAuditioningDifference() && ! session.processor->isComparingOriginal(),
                   "headphones while in B -> back to A and auditioning");

            auto& limitPower = find (celadon::param::limitBypass);
            click (limitPower, { 11.0f, 11.0f });
            editor.advanceForSnapshot (0.05);
            check (value (celadon::param::limitBypass) > 0.5f && ! session.processor->isAuditioningDifference() && ! headphones.isEnabled(),
                   "bypassing LIMIT ends the audition and disables the headphones");
            click (headphones, { 11.0f, 11.0f });
            check (! session.processor->isAuditioningDifference(), "disabled headphones cannot start an audition");
            click (limitPower, { 11.0f, 11.0f });
            check (headphones.isEnabled() && value (celadon::param::limitBypass) < 0.5f, "headphones are enabled again with LIMIT on");
        }

        // ⑩ 高级：点顶栏「高级」打开面板，点 ✕ 关上。
        {
            auto& advancedButton = find (celadon::uiid::advanced);
            auto& panel = find ("advanced_panel");
            click (advancedButton, { 20.0f, 14.0f });
            check (panel.isVisible(), "Advanced opens its panel");
            save (editor, scale, outDir.getChildFile ("check-advanced.png"));

            const auto margin = (float) celadon::ui::PopoverCard::shadowMargin;
            click (panel, { (float) panel.getWidth() - margin - 16.0f - 12.0f, margin + 24.0f });
            check (! panel.isVisible(), "the close button hides the Advanced panel");
        }

        // Advanced controls are exercised through the real widgets and Processor.
        {
            editor.showAdvanced (true);
            auto& tabs = find ("adv_tabs");
            click (tabs, { (float) tabs.getWidth() * 0.5f, 14.0f });
            click (find ("adv_learn"), { 30.0f, 14.0f });
            session.play (0.1);
            check (session.processor->getMasteringStatus().referenceState == celadon::ReferenceState::waitingForSidechain,
                   "reference learning waits for an actual sidechain");
            click (find ("adv_cancel"), { 30.0f, 14.0f });
            session.play (0.1);
            check (session.processor->getMasteringStatus().referenceState == celadon::ReferenceState::idle,
                   "reference learning can be cancelled");
            click (tabs, { (float) tabs.getWidth() * 0.84f, 14.0f });
            click (find ("adv_preview"), { 30.0f, 14.0f });
            check (session.processor->isReleasePreviewEnabled(), "release preview widget enables monitoring");
            editor.setListeningToOriginal (true);
            check (! session.processor->isReleasePreviewEnabled() && session.processor->isComparingOriginal(),
                   "original audition disables release preview");
            editor.setReleasePreviewEnabled (true);
            check (! session.processor->isComparingOriginal() && session.processor->isReleasePreviewEnabled(),
                   "release preview restores processed audio");
            editor.setAuditioningDifference (true);
            check (! session.processor->isReleasePreviewEnabled() && session.processor->isAuditioningDifference(),
                   "difference audition disables release preview");
            editor.setReleasePreviewEnabled (true);
            check (! session.processor->isAuditioningDifference(), "preview disables difference audition");
            session.processor->setNonRealtime (true);
            session.play (0.1);
            check (! session.processor->isReleasePreviewEnabled(), "offline export disables release preview");
            session.processor->setNonRealtime (false);
            editor.setReleasePreviewEnabled (false);
            editor.showAdvanced (false);
        }

        // Visible peak readouts retain their reset target after the panel height change.
        for (const auto* id : { "input_meter", "output_meter" })
        {
            auto& meter = static_cast<celadon::ui::LevelMeterPanel&> (find (id));
            const auto original = meter.onResetClicked;
            bool clicked = false;
            meter.onResetClicked = [&] { clicked = true; };
            click (meter, { 20.0f, (float) meter.getHeight() - 20.0f });
            meter.onResetClicked = original;
            check (clicked, juce::String (id) + " visible readout resets peak hold");
        }

        // Run the no-reduction difference regression through the actual Apple Release library.
        for (const auto rate : { 44100.0, 48000.0, 96000.0 })
            for (const auto lookahead : { 1.0f, 2.0f, 10.0f })
            {
                celadon::LimiterModule limiter;
                celadon::LimiterModule::Params params;
                params.gainDb = 0.0f;
                params.ceilingDb = -1.0f;
                params.lookaheadMs = lookahead;
                params.clip = 0.0f;
                limiter.setParams (params);
                limiter.prepare ({ rate, 512, 2 });
                juce::AudioBuffer<float> input (2, 512), difference (2, 512);
                float worst = 0.0f;
                std::uint32_t noise = 42;
                const auto total = (int) (rate * 0.6);
                for (int start = 0; start < total; start += 512)
                {
                    params.gainDb = start < total / 3 ? 0.0f : start < total * 2 / 3 ? 6.0f : 2.0f;
                    limiter.setParams (params);
                    const auto count = juce::jmin (512, total - start);
                    input.setSize (2, count, false, false, true);
                    difference.setSize (2, count, false, false, true);
                    for (int sample = 0; sample < count; ++sample)
                    {
                        noise = noise * 1664525u + 1013904223u;
                        auto level = (float) ((double) (noise >> 8) / 8388608.0 - 1.0) * 0.05f;
                        for (const auto frequency : { 60.0, 1000.0, 9000.0 })
                            level += (float) (0.03 * std::sin (juce::MathConstants<double>::twoPi * frequency * (start + sample) / rate));
                        input.setSample (0, sample, level);
                        input.setSample (1, sample, -level);
                    }
                    limiter.process (input, &difference);
                    for (int channel = 0; channel < 2; ++channel)
                        for (int sample = 0; sample < count; ++sample)
                            worst = juce::jmax (worst, std::abs (difference.getSample (channel, sample)));
                }
                check (worst == 0.0f && limiter.getAndResetGainReductionDb() == 0.0f,
                       "Release difference stays exactly zero at " + juce::String (rate) + " Hz / " + juce::String (lookahead) + " ms");
            }

        // State migration is exercised on an already-used Processor, as in preset hot-swap.
        {
            auto legacy = session.processor->getParameters().copyState();
            for (int i = legacy.getNumChildren(); --i >= 0;)
                if (legacy.getChild (i).getProperty ("id").toString().startsWith ("master_"))
                    legacy.removeChild (i, nullptr);
            session.setParameter ("master_harsh_mode", 2.0f);
            session.setParameter ("master_target_mode", 4.0f);
            session.setParameter ("master_codec_margin_db", 1.0f);
            session.setParameter ("master_dither", 1.0f);
            juce::MemoryBlock legacyBytes;
            juce::AudioProcessor::copyXmlToBinary (*legacy.createXml(), legacyBytes);
            session.processor->setStateInformation (legacyBytes.getData(), (int) legacyBytes.getSize());
            check (value ("master_harsh_mode") == 1.0f && value ("master_target_mode") == 0.0f
                   && value ("master_codec_margin_db") == 0.0f && value ("master_dither") == 0.0f,
                   "loading an old session neutralises all new processing in a used instance");
        }

        // The actual host bus layout: main and reference share a buffer but never mix.
        {
            CeladonAudioProcessor referenceProcessor, controlProcessor;
            auto layout = referenceProcessor.getBusesLayout();
            layout.inputBuses.set (1, juce::AudioChannelSet::stereo());
            check (referenceProcessor.setBusesLayout (layout), "Processor accepts stereo reference sidechain");
            for (auto* processor : { &referenceProcessor, &controlProcessor })
            {
                auto* amount = processor->getParameters().getParameter (celadon::param::glazeAmount);
                amount->setValueNotifyingHost (amount->convertTo0to1 (0.0f));
                processor->setRateAndBufferSizeDetails (sampleRate, blockSize);
                processor->prepareToPlay (sampleRate, blockSize);
            }
            referenceProcessor.requestReferenceLearn();
            juce::AudioBuffer<float> main (2, blockSize), withSidechain (4, blockSize);
            juce::MidiBuffer midi;
            Music mainMusic;
            juce::AudioBuffer<float> referenceAudio (2, blockSize);
            bool identical = true;
            for (int block = 0; block < 1100; ++block)
            {
                mainMusic.render (main, blockSize);
                for (int i = 0; i < blockSize; ++i)
                {
                    const auto t = (double) (block * blockSize + i) / sampleRate;
                    referenceAudio.setSample (0, i, (float) (0.3 * std::sin (juce::MathConstants<double>::twoPi * 3700.0 * t)));
                    referenceAudio.setSample (1, i, (float) (0.2 * std::cos (juce::MathConstants<double>::twoPi * 620.0 * t)));
                }
                if (block == 0)
                    check (std::memcmp (main.getReadPointer (0), referenceAudio.getReadPointer (0),
                                       (size_t) blockSize * sizeof (float)) != 0,
                           "reference routing test uses audio distinct from the main input");
                for (int channel = 0; channel < 2; ++channel)
                {
                    withSidechain.copyFrom (channel, 0, main, channel, 0, blockSize);
                    withSidechain.copyFrom (channel + 2, 0, referenceAudio, channel, 0, blockSize);
                }
                referenceProcessor.processBlock (withSidechain, midi);
                controlProcessor.processBlock (main, midi);
                for (int channel = 0; channel < 2; ++channel)
                    identical = identical && std::memcmp (main.getReadPointer (channel), withSidechain.getReadPointer (channel),
                                                         (size_t) blockSize * sizeof (float)) == 0;
            }
            check (identical, "connected reference audio never leaks into main output at GLAZE 0");
            const auto learned = referenceProcessor.getMasteringStatus();
            check (learned.hasReference && learned.referenceState == celadon::ReferenceState::ready,
                   "Processor learns a real sidechain through the host bus");
            juce::MemoryBlock saved;
            referenceProcessor.getStateInformation (saved);
            controlProcessor.setStateInformation (saved.getData(), (int) saved.getSize());
            check (controlProcessor.getMasteringStatus().hasReference, "Processor state restores learned reference");
            controlProcessor.prepareToPlay (sampleRate, blockSize);
            check (controlProcessor.getMasteringStatus().hasReference, "same-spec reprepare preserves learned reference");
        }

        // ⑪ 语言：点 EN，用户设置变英文，界面在下一次消息循环里跟上。
        {
            auto& language = find ("language");
            click (language, { (float) language.getWidth() - 12.0f, 13.0f });
            dispatchEvents (50);
            editor.advanceForSnapshot (0.05);
            juce::SharedResourcePointer<celadon::ui::UiSettings> settings;
            check (settings->getLanguage() == celadon::Language::english, "language selector -> English");
            check (static_cast<celadon::ui::CornerNote&> (corner).getFirstLine() == "Drives 7 parameters",
                   "corner note follows the language (" + static_cast<celadon::ui::CornerNote&> (corner).getFirstLine() + ")");
            save (editor, scale, outDir.getChildFile ("check-english.png"));
            settings->setLanguage (celadon::Language::chinese);
        }

        failures += runCacheChecks (session, check);
        runSpectrumChecks (check);   // T-051：失败由 check 计入
        runClayGuardChecks (check);   // T-074：失败由 check 计入

        std::cout << (failures == 0 ? "all interaction checks passed" : juce::String (failures) + " interaction checks FAILED") << std::endl;
        return failures;
    }

    //==============================================================================
    // T-074 修坯守护回收指示：CLAY 卡片在真编辑器里画出来的灯与数。
    // 界面只按状态字段渲染（Status → clayReadoutFor → ClayCard::setReadout → paint）；HelpTextTests 验前两步的函数，这里验最后一步：
    //   ① 标志开着，低切、收单两行的灯与数值是琥珀色，加宽行（不经峰值守护）不变；
    //   ② 标志变了会重画——setReadout 靠 ClayReadout::operator== 判「没变就不画」，漏比较守护字段时低切 / 收单的值没变、灯就不会跟着变；
    //   ③ 标志关掉后灯回到原样。
    // 同一个编辑器、同一份「低切 25 Hz、收单 120 Hz 在做」的状态，守护标志 关 → 开 → 关 各把 CLAY 卡片画成图（1 倍），数灯那一列（x 10 ～ 26，圆点在
    // 14 ～ 21）里灯色（palette::warning，各通道容差 8）的像素，与数值那一列（x 29 ～ 125）里偏橙（红减蓝 ≥ 60）的像素——数值字是小号抗锯齿字，
    // 整个字色（palette::warningText）的像素寥寥无几，正常时的字色（紫 / 青 / 灰）红都小于蓝，所以用「偏橙」判；行带是 y = 46 + 47 i 起 42 px
    //（ClayCard::rowBounds）。开着：低切行、收单行的灯各 ≥ 15 个（7 px 的实心圆约 38 个，抗锯齿后留一大半）、数值字各 ≥ 15 个，加宽行两样都是 0；
    // 关着（开之前、开之后）：三行两样都是 0。
    void runClayGuardChecks (const std::function<void (bool, const juce::String&)>& check)
    {
        using namespace celadon::ui;

        auto session = makeSession (0.0f);
        auto& editor = *session->editor;
        auto* card = findFirst<ClayCard> (editor);

        if (card == nullptr || editor.getWidth() != CeladonAudioProcessorEditor::baseWidth)
        {
            check (false, "T-074 CLAY card: the card was not found, or the editor does not open at zoom 1 (needs to be 1200 px wide)");
            return;
        }

        struct Counts
        {
            int lamp[3] {}, text[3] {};   // 每行（低切 / 收单 / 加宽）的灯色像素数、字色像素数
        };

        const auto isClose = [] (juce::Colour pixel, juce::Colour wanted)
        {
            return std::abs ((int) pixel.getRed() - (int) wanted.getRed()) <= 8
                && std::abs ((int) pixel.getGreen() - (int) wanted.getGreen()) <= 8
                && std::abs ((int) pixel.getBlue() - (int) wanted.getBlue()) <= 8;
        };

        const auto isOrangeish = [] (juce::Colour pixel)
        {
            return (int) pixel.getRed() - (int) pixel.getBlue() >= 60;
        };

        const auto area = editor.getLocalArea (card, card->getLocalBounds());

        const auto draw = [&] (bool guardActive)
        {
            injectStatus (*session, [guardActive] (celadon::glaze::Status& status)
            {
                clayPeakGuard (0.42f) (status);
                status.clayPeakGuardActive = guardActive;
            });

            const auto image = editor.createComponentSnapshot (area, true, 1.0f);
            Counts counts;

            for (int row = 0; row < 3; ++row)
            {
                for (int y = 46 + 47 * row; y < 46 + 47 * row + 42; ++y)
                {
                    for (int x = 10; x < 26; ++x)
                        counts.lamp[row] += isClose (image.getPixelAt (x, y), palette::warning) ? 1 : 0;

                    for (int x = 29; x < 125; ++x)
                        counts.text[row] += isOrangeish (image.getPixelAt (x, y)) ? 1 : 0;
                }
            }

            return counts;
        };

        const auto describe = [] (const Counts& c)
        {
            return "lamps " + juce::String (c.lamp[0]) + " / " + juce::String (c.lamp[1]) + " / " + juce::String (c.lamp[2])
                 + ", value text " + juce::String (c.text[0]) + " / " + juce::String (c.text[1]) + " / " + juce::String (c.text[2]);
        };

        const auto allZero = [] (const Counts& c)
        {
            return c.lamp[0] + c.lamp[1] + c.lamp[2] + c.text[0] + c.text[1] + c.text[2] == 0;
        };

        const auto before = draw (false);
        const auto during = draw (true);
        const auto after = draw (false);
        const auto lit = during.lamp[0] >= 15 && during.lamp[1] >= 15 && during.text[0] >= 15 && during.text[1] >= 15
                      && during.lamp[2] == 0 && during.text[2] == 0;

        check (allZero (before) && lit && allZero (after),
               "T-074 CLAY card: with the peak guard pulling back, the low cut and mono lamps and values turn amber and the width row does not; "
               "they go back when it stops (amber pixels per row, low cut / mono / width: guard off " + describe (before) + "; on " + describe (during)
                   + "; off again " + describe (after) + ")");
    }

    //==============================================================================
    // T-032 界面性能改动的回归检查：各缓存命中、分层瓷面与原算法逐像素对照、30 帧节流、底排与延迟读数的触发条件。
    // 返回 0（失败已由 check 计入 runChecks 的总数）。
    struct ImageDiff
    {
        int worst = 0;
        long long pixels = 0, offBy1 = 0, offBy2 = 0, offByMore = 0;
        juce::Point<int> worstAt { -1, -1 };             // 第一个差到 worst 的像素与两边的值（预乘 ARGB）
        juce::uint32 worstA = 0, worstB = 0;

        juce::String describeWorst() const
        {
            return worstAt.x < 0 ? juce::String ("-")
                                 : "(" + juce::String (worstAt.x) + ", " + juce::String (worstAt.y) + ") #"
                                       + juce::String::toHexString ((juce::int64) worstA).paddedLeft ('0', 8) + " vs #"
                                       + juce::String::toHexString ((juce::int64) worstB).paddedLeft ('0', 8);
        }
    };

    // 两张同尺寸 ARGB 图逐像素、逐通道（预乘值）比。尺寸不同按 255 记。
    ImageDiff compareImages (const juce::Image& a, const juce::Image& b)
    {
        ImageDiff diff;

        if (a.getWidth() != b.getWidth() || a.getHeight() != b.getHeight())
        {
            diff.worst = 255;
            return diff;
        }

        const juce::Image::BitmapData da (a, juce::Image::BitmapData::readOnly), db (b, juce::Image::BitmapData::readOnly);

        for (int y = 0; y < da.height; ++y)
            for (int x = 0; x < da.width; ++x)
            {
                const auto& p = *reinterpret_cast<const juce::PixelARGB*> (da.getPixelPointer (x, y));
                const auto& q = *reinterpret_cast<const juce::PixelARGB*> (db.getPixelPointer (x, y));
                const auto d = juce::jmax (std::abs ((int) p.getAlpha() - (int) q.getAlpha()), std::abs ((int) p.getRed() - (int) q.getRed()),
                                           std::abs ((int) p.getGreen() - (int) q.getGreen()), std::abs ((int) p.getBlue() - (int) q.getBlue()));
                ++diff.pixels;

                if (d > diff.worst)
                {
                    diff.worst = d;
                    diff.worstAt = { x, y };
                    diff.worstA = p.getNativeARGB();
                    diff.worstB = q.getNativeARGB();
                }

                diff.offBy1 += d == 1 ? 1 : 0;
                diff.offBy2 += d == 2 ? 1 : 0;
                diff.offByMore += d > 2 ? 1 : 0;
            }

        return diff;
    }

    // ⑳ 用的合成部件（T-041）：挂对齐缓存（AlignedComponentCache），记下最近一次 paint 拿到的裁剪包围盒（用户坐标）。
    // 内容有底、圆角边、一个会变的圆、字；再挂一个子组件——同频谱仪的斜率控件：子组件自己重画时，父组件的缓存只作废子组件那一块。
    struct AlignedCacheProbe  : public juce::Component
    {
        struct Child  : public juce::Component
        {
            void paint (juce::Graphics& g) override
            {
                g.setColour (juce::Colour (0xff2a7f86));
                g.fillRoundedRectangle (getLocalBounds().toFloat().reduced (0.5f), 3.0f);
            }
        };

        explicit AlignedCacheProbe (bool opaque)
        {
            setOpaque (opaque);
            addAndMakeVisible (child);
            celadon::ui::setBufferedToAlignedImage (*this);
        }

        void resized() override   { child.setBounds (150, 6, 38, 20); }

        void paint (juce::Graphics& g) override
        {
            lastClip = g.getClipBounds();
            ++paints;

            const auto bounds = getLocalBounds().toFloat().reduced (0.5f);

            if (isOpaque())
                g.fillAll (juce::Colour (0xffe6eeec));

            g.setColour (juce::Colour (0xfff4f7f6));
            g.fillRoundedRectangle (bounds, bounds.getHeight() * 0.5f);
            g.setColour (juce::Colour (0xff9fb8b4));
            g.drawRoundedRectangle (bounds, bounds.getHeight() * 0.5f, 1.0f);
            g.setColour (juce::Colour (0xff3e8f8a).withAlpha (0.35f + 0.65f * shade));
            g.fillEllipse (dot);
            g.setColour (juce::Colour (0xff1d3b3a));
            g.setFont (celadon::ui::uiFont (12.0f));
            g.drawText (juce::String::fromUTF8 ("Aligned \xe5\xb7\xb2\xe9\x94\x81\xe5\xae\x9a"), juce::Rectangle<float> (64.0f, 0.0f, 80.0f, (float) getHeight()),
                        juce::Justification::centredLeft, false);
        }

        Child child;
        juce::Rectangle<float> dot { 41.3f, 9.6f, 13.0f, 13.0f };
        float shade = 0.0f;
        juce::Rectangle<int> lastClip;
        int paints = 0;
    };

    int runCacheChecks (Session& session, const std::function<void (bool, const juce::String&)>& check)
    {
        using namespace celadon::ui;
        auto& editor = *session.editor;
        juce::SharedResourcePointer<UiSettings> settings;

        const auto find = [&editor] (const juce::String& id) -> juce::Component&
        {
            auto* component = findById (editor, id);

            if (component == nullptr)
            {
                std::cout << "FAIL missing component " << id << std::endl;
                std::exit (2);
            }

            return *component;
        };

        // 回到中文、让语言切换与各定时器（瓷面交叉淡化、按像素比重生成）先跑完。
        settings->setLanguage (celadon::Language::chinese);
        dispatchEvents (400);
        editor.advanceForSnapshot (0.05);

        juce::Image target (juce::Image::RGB, editor.getWidth(), editor.getHeight(), false);
        const juce::Rectangle<int> band (0, ParticleField::top, editor.getWidth(), ParticleField::bandHeight);
        paintFrame (editor, target, 1.0f, std::nullopt);

        // ⑫ R9-02：分层渲染与原算法（renderGlaze）逐像素相差 ≤ 2 LSB——各缩放下的像素比（含主旋钮 200 / 224 的换算）× GLAZE × 去色。
        {
            porcelain::GlazeRenderer renderer;
            ImageDiff total;
            juce::String worstCase = "-";

            for (const auto pixelScale : { 0.625f, 0.892857f, 1.0f, 1.339286f, 1.5f, 1.785714f, 2.0f, 2.5f, 3.571429f, 4.0f })
                for (const auto t : { 0.0f, 0.03f, 0.2f, 0.35f, 0.45f, 0.5f, 0.65f, 0.8f, 0.9f, 1.0f })
                    for (const auto desaturated : { false, true })
                    {
                        const auto diff = compareImages (porcelain::renderGlaze (t, pixelScale, desaturated), renderer.render (t, pixelScale, desaturated));

                        if (diff.worst > total.worst)
                            worstCase = "scale " + juce::String (pixelScale) + " t " + juce::String (t) + (desaturated ? " desaturated" : "")
                                          + " px " + diff.describeWorst() + " reference vs layered";

                        total.worst = juce::jmax (total.worst, diff.worst);
                        total.pixels += diff.pixels;
                        total.offBy1 += diff.offBy1;
                        total.offBy2 += diff.offBy2;
                        total.offByMore += diff.offByMore;
                    }

            check (total.worst <= 2, "T-032 layered porcelain within 2 LSB of renderGlaze (worst " + juce::String (total.worst) + " at " + worstCase
                                         + "; of " + juce::String (total.pixels) + " px: off by 1 " + juce::String (total.offBy1) + ", by 2 "
                                         + juce::String (total.offBy2) + ", by more " + juce::String (total.offByMore) + ")");
            check (renderer.getLayerBuilds() == 10, "T-032 porcelain layers rebuilt only when the image size changes (" + juce::String (renderer.getLayerBuilds()) + " builds for 10 scales)");
        }

        // ⑬ R9-01 / 07 / 14：整颗主旋钮的缓存图——GLAZE 不动时重画粒子带只贴图；值、听原声一变就重新合成一次。
        //    T-100：值变了读数走弹簧——减少动效时直接落下、合成一次；正常动效时读数在路上每帧合成一次，停稳（精确落在参数值上、
        //    彗尾熄、定时器停）之后又只贴图。弹簧按 1 / 60 s 一帧手动推（advanceMotion），不跑消息循环。
        {
            auto& glazeKnob = static_cast<GlazeKnob&> (find (celadon::param::glazeAmount));
            const auto reducedBefore = glazeKnob.getReduceMotion();
            glazeKnob.finishMotion();
            paintFrame (editor, target, 1.0f, band);
            const auto before = glazeKnob.getPaintStats();

            for (int i = 0; i < 5; ++i)
                paintFrame (editor, target, 1.0f, band);

            const auto steady = glazeKnob.getPaintStats();
            check (steady.paints == before.paints + 5 && steady.faceRenders == before.faceRenders,
                   "T-032 GLAZE knob: 5 band repaints only blit its cached face (" + juce::String (steady.paints - before.paints) + " paints, "
                       + juce::String (steady.faceRenders - before.faceRenders) + " face renders)");

            auto* glaze = session.processor->getParameters().getParameter (celadon::param::glazeAmount);
            const auto bump = [glaze] { glaze->setValueNotifyingHost (glaze->convertTo0to1 (glaze->convertFrom0to1 (glaze->getValue()) + 1.0f)); };

            glazeKnob.setReduceMotion (true);
            bump();
            paintFrame (editor, target, 1.0f, band);
            paintFrame (editor, target, 1.0f, band);
            const auto moved = glazeKnob.getPaintStats();
            check (moved.faceRenders == steady.faceRenders + 1, "T-032 GLAZE knob (reduce motion): a value change re-composes the face once ("
                                                                    + juce::String (moved.faceRenders - steady.faceRenders) + ")");

            glazeKnob.setReduceMotion (false);
            bump();
            int frames = 0;

            while (glazeKnob.isMotionRunning() && frames < 600)
            {
                glazeKnob.advanceMotion (1.0 / 60.0);
                paintFrame (editor, target, 1.0f, band);
                ++frames;
            }

            const auto sprung = glazeKnob.getPaintStats();
            const auto springRenders = sprung.faceRenders - moved.faceRenders;
            check (frames > 1 && frames < 600 && springRenders > 1 && springRenders <= frames + 1 && glazeKnob.isReadingSettled()
                       && juce::exactlyEqual (glazeKnob.getCometStrength(), 0.0f),
                   "T-100 GLAZE knob: the reading springs to a new value, re-composing per moving frame, then settles exactly ("
                       + juce::String (springRenders) + " face renders over " + juce::String (frames) + " frames)");

            for (int i = 0; i < 5; ++i)
                paintFrame (editor, target, 1.0f, band);

            const auto settled = glazeKnob.getPaintStats();
            check (settled.paints == sprung.paints + 5 && settled.faceRenders == sprung.faceRenders,
                   "T-100 GLAZE knob: once the spring settles, 5 band repaints only blit its cached face ("
                       + juce::String (settled.faceRenders - sprung.faceRenders) + " face renders)");

            glazeKnob.setComparing (true);
            paintFrame (editor, target, 1.0f, band);
            paintFrame (editor, target, 1.0f, band);
            const auto comparing = glazeKnob.getPaintStats();
            check (comparing.faceRenders == settled.faceRenders + 1, "T-032 GLAZE knob: listening to the original re-composes the face once ("
                                                                         + juce::String (comparing.faceRenders - settled.faceRenders) + ")");
            glazeKnob.setComparing (session.processor->isComparingOriginal());
            dispatchEvents (400);

            // 瓷面在动画停下后精确落在读数上：改 0.1 %（比例差 0.001，在动时的容差 0.004 以内），弹簧停稳后让瓷面的定时器跑一会儿，
            // 瓷面按的比例应当与读数逐位相同（不然静止画面会随动画途中哪一帧生成过瓷面而变）。往裂纹条数不变的方向改：条数一变
            // 瓷面本来就会重新生成，测不到容差这条路（反向验证时改 0.3 % 正好跨过一条裂纹，撤掉修复也照样过）。
            {
                const auto p0 = juce::jlimit (0.0f, 1.0f, glazeKnob.getReadingProportion());
                const auto porcelainBefore = glazeKnob.getPaintStats();
                const auto nudge = porcelain::visibleCrackCount (p0 + 0.001f) == porcelain::visibleCrackCount (p0) ? 0.1f : -0.1f;
                glaze->setValueNotifyingHost (glaze->convertTo0to1 (glaze->convertFrom0to1 (glaze->getValue()) + nudge));

                for (int i = 0; i < 600 && glazeKnob.isMotionRunning(); ++i)
                    glazeKnob.advanceMotion (1.0 / 60.0);

                dispatchEvents (400);
                const auto exact = glazeKnob.getPaintStats();
                const auto reading = juce::jlimit (0.0f, 1.0f, glazeKnob.getReadingProportion());
                check (juce::exactlyEqual (porcelainBefore.porcelainProportion, p0) && ! juce::exactlyEqual (reading, p0)
                           && porcelain::visibleCrackCount (reading) == porcelain::visibleCrackCount (p0) && ! glazeKnob.isMotionRunning()
                           && juce::exactlyEqual (exact.porcelainProportion, reading),
                       "T-100 GLAZE knob: once the motion stops the porcelain is regenerated exactly at the reading, even within the 0.004 tolerance "
                       "(porcelain " + juce::String (porcelainBefore.porcelainProportion, 5) + " -> " + juce::String (exact.porcelainProportion, 5)
                           + ", reading " + juce::String (p0, 5) + " -> " + juce::String (reading, 5) + ", cracks "
                           + juce::String (porcelain::visibleCrackCount (reading)) + ")");
            }

            glazeKnob.setReduceMotion (reducedBefore);
            dispatchEvents (400);
        }

        // ⑭ R9-01：粒子带里的 10 个文字小部件（T-038 起多了曲风、交付两个选择）都挂着整数对齐的缓存；重画粒子带时从缓存读、不再重画进缓存。
        {
            int cached = 0;
            juce::StringArray missing;

            for (const auto* id : { "glaze_title", "glaze_style", "ab", "match", "monitoring", "corner_note", "legend",
                                    "glaze_genre", "glaze_delivery" })
            {
                if (dynamic_cast<AlignedComponentCache*> (find (id).getCachedComponentImage()) != nullptr)
                    ++cached;
                else
                    missing.add (id);
            }

            check (cached == 9, "T-032 the 9 text widgets in the particle band have aligned image caches" + (missing.isEmpty() ? juce::String() : " (missing " + missing.joinIntoString (", ") + ")"));

            PaintProbe probe;
            auto& title = find ("glaze_title");
            title.addComponentListener (&probe);
            paintFrame (editor, target, 1.0f, band);
            probe.reset();

            for (int i = 0; i < 5; ++i)
                paintFrame (editor, target, 1.0f, band);

            title.removeComponentListener (&probe);
            check (probe.paints == 5 && probe.cacheReads == 5 && probe.cacheWrites == 0,
                   "T-032 band repaints read the GLAZE title from its cache (" + juce::String (probe.cacheReads) + " reads, "
                       + juce::String (probe.cacheWrites) + " re-renders of 5)");
        }

        // ⑮ R9-05 / M-3：粒子的柔光与流线缓存图——像素比不变时不重新生成。
        {
            auto* field = findFirst<ParticleField> (editor);
            const auto before = field != nullptr ? field->getCacheStats() : ParticleField::CacheStats {};

            for (int i = 0; i < 5; ++i)
            {
                editor.advanceForSnapshot (1.0 / 30.0);
                paintFrame (editor, target, 1.0f, band);
            }

            const auto after = field != nullptr ? field->getCacheStats() : ParticleField::CacheStats { -1, -1 };
            check (field != nullptr && after.lineRenders == before.lineRenders && after.glowRenders == before.glowRenders && before.lineRenders > 0,
                   "T-032 particle glow and streamlines stay cached across frames (" + juce::String (after.lineRenders) + " / "
                       + juce::String (after.glowRenders) + " renders so far)");
        }

        // ⑯ R9-06：30 帧 / s 节流在各种刷新率与时间戳抖动下都按整数个 VBlank 画（60 Hz 每 2 个、90 Hz 每 3 个、120 Hz 每 4 个、
        //     144 Hz 每 5 个）；对照：只按「≥ 1/30 s」严格比，60.001 Hz 屏就掉到每 3 个 VBlank 一帧。
        {
            struct Case { double hz, jitterMs; int expected; };
            juce::Random random (0x7032);
            juce::StringArray results;
            bool allOk = true;

            for (const auto& c : { Case { 60.0, 0.0, 2 }, Case { 60.001, 0.0, 2 }, Case { 59.94, 0.0, 2 }, Case { 60.0, 1.5, 2 },
                                   Case { 90.0, 0.0, 3 }, Case { 120.0, 0.0, 4 }, Case { 120.0, 1.0, 4 }, Case { 144.0, 0.0, 5 } })
            {
                const auto period = 1.0 / c.hz;
                double lastVBlank = -1.0, lastFrame = -1.0, lastStrictFrame = -1.0;
                int lastFrameIndex = -1, lastStrictIndex = -1, minGap = 1000, maxGap = 0, strictMaxGap = 0;

                for (int k = 0; k < 900; ++k)
                {
                    // 时间戳从 1 s 起：真实的 VBlank 时间戳恒为正（开机以来的秒数），第 0 帧带抖动也不能变成负数去撞「< 0 = 还没有」。
                    const auto t = 1.0 + (double) k * period + (random.nextDouble() * 2.0 - 1.0) * c.jitterMs * 1.0e-3;
                    const auto dt = lastVBlank < 0.0 ? 0.0 : t - lastVBlank;
                    lastVBlank = t;

                    if (lastFrame < 0.0 || ParticleField::isFrameDue (t - lastFrame, dt))
                    {
                        if (lastFrameIndex >= 0)
                        {
                            minGap = juce::jmin (minGap, k - lastFrameIndex);
                            maxGap = juce::jmax (maxGap, k - lastFrameIndex);
                        }

                        lastFrame = t;
                        lastFrameIndex = k;
                    }

                    if (lastStrictFrame < 0.0 || t - lastStrictFrame >= 1.0 / ParticleField::maxFrameRate)
                    {
                        if (lastStrictIndex >= 0)
                            strictMaxGap = juce::jmax (strictMaxGap, k - lastStrictIndex);

                        lastStrictFrame = t;
                        lastStrictIndex = k;
                    }
                }

                const auto ok = minGap == c.expected && maxGap == c.expected;
                allOk = allOk && ok;
                results.add (juce::String (c.hz, 3) + " Hz" + (c.jitterMs > 0.0 ? " +/-" + juce::String (c.jitterMs, 1) + " ms" : juce::String())
                             + ": every " + juce::String (minGap) + (maxGap != minGap ? "-" + juce::String (maxGap) : juce::String())
                             + " (strict >= 1/30 s: up to " + juce::String (strictMaxGap) + ")");
            }

            check (allOk, "T-032 particle frame pacing locks to whole VBlanks at 30 fps [" + results.joinIntoString ("; ") + "]");
        }

        // ⑰ R9-12：底排不再每帧重排——等响的文字变了（开 ↔ 关）才按新宽度重排；语言切换也重排。
        {
            auto& match = static_cast<PillButton&> (find ("match"));
            auto& ab = static_cast<Segmented&> (find ("ab"));
            const auto laidOut = [&] { return match.getWidth() == match.getNaturalWidth() && ab.getWidth() == ab.getNaturalWidth(); };

            const auto before = match.getText();
            click (match, { 20.0f, 14.0f });
            editor.advanceForSnapshot (0.05);
            check (match.getText() != before && laidOut(), "T-032 bottom row re-laid out after the Match text changed (" + match.getText() + ")");
            click (match, { 20.0f, 14.0f });
            editor.advanceForSnapshot (0.05);

            settings->setLanguage (celadon::Language::english);
            dispatchEvents (50);
            editor.advanceForSnapshot (0.05);
            auto& topBar = static_cast<TopBar&> (*find ("language").getParentComponent());
            check (laidOut() && match.getText().startsWith ("Match"),
                   "T-032 bottom row re-laid out for English (" + match.getText() + ")");
            check (topBar.getLatencyText().startsWith (tr (Text::latency, celadon::Language::english) + " "),
                   "T-032 latency label follows the language at once (" + topBar.getLatencyText() + ")");

            settings->setLanguage (celadon::Language::chinese);
            dispatchEvents (50);
            editor.advanceForSnapshot (0.05);
            check (laidOut() && topBar.getLatencyText().startsWith (tr (Text::latency, celadon::Language::chinese) + " "),
                   "T-032 bottom row and latency label back to Chinese");
        }

        // T-036（评审 R-01）：频谱仪的 5 个子组件（三个图例开关、斜率、响度小窗）挂整数对齐的缓存——频谱一秒重画 60 / 120 次时
        //     它们从缓存贴；不能是 JUCE 自带的 setBufferedToImage（小数像素比下每帧双线性重采样、字发糊）。
        {
            auto* view = findFirst<SpectrumView> (editor);
            int aligned = 0, children = 0;

            if (view != nullptr)
                for (auto* child : view->getChildren())
                {
                    ++children;
                    aligned += dynamic_cast<AlignedComponentCache*> (child->getCachedComponentImage()) != nullptr ? 1 : 0;
                }

            check (view != nullptr && children == 5 && aligned == 5,
                   "T-036 the spectrum's legend toggles, slope control and loudness HUD have aligned image caches ("
                       + juce::String (aligned) + " of " + juce::String (children) + ")");
        }

        // ⑱ R9-12：「高级」面板关着时只记下状态，打开时一次刷新到最新（预听开着 → 按钮写「停止预听」）。
        {
            editor.showAdvanced (false);
            editor.setReleasePreviewEnabled (true);
            editor.advanceForSnapshot (0.05);
            auto& preview = static_cast<PillButton&> (find ("adv_preview"));
            const auto hiddenText = preview.getText();
            editor.showAdvanced (true);
            const auto shownText = preview.getText();
            check (session.processor->isReleasePreviewEnabled() && shownText == tr (Text::previewStop, celadon::Language::chinese),
                   "T-032 Advanced panel refreshes the stored status when it opens (" + hiddenText + " -> " + shownText + ")");
            editor.showAdvanced (false);
            editor.setReleasePreviewEnabled (false);
            editor.advanceForSnapshot (0.05);
        }

        // ⑲ R9-13：状态胶囊整颗缓存（整数对齐）。内容变了（整颗作废）时每段文字照画进缓存；别处引起的重画（macOS 的脏矩形包围盒
        //     把胶囊圈进去）只从缓存贴、不排版；呼吸只作废圆点那一小块（圆点 7 px + 3 px 光晕取整，T-041 起不再多留 1 px），缓存只重画那一块，
        //     胶囊与顶栏一段文字都不排版（跳过只按脏区判断，不会漏画）。
        {
            auto* pill = findFirst<StatusPill> (editor);
            auto* topBar = findFirst<TopBar> (editor);
            const auto area = pill != nullptr ? pill->getPulseArea() : juce::Rectangle<int>();
            check (pill != nullptr && topBar != nullptr && ! area.isEmpty() && area.getWidth() <= 16 && area.getHeight() <= 16
                       && pill->getLocalBounds().contains (area)
                       && dynamic_cast<AlignedComponentCache*> (pill->getCachedComponentImage()) != nullptr,
                   "T-032 status pill has an aligned image cache and pulses by invalidating only its dot (" + area.toString() + " of "
                       + (pill != nullptr ? pill->getLocalBounds().toString() : juce::String ("-")) + ")");

            if (pill != nullptr && topBar != nullptr)
            {
                PaintProbe probe;
                pill->addComponentListener (&probe);
                const auto whole = editor.getLocalArea (pill, pill->getLocalBounds());

                pill->repaint();   // 内容变了：整颗作废
                const auto renderedBefore = pill->getTextDraws();
                paintFrame (editor, target, 1.0f, whole);
                const auto renderedText = pill->getTextDraws() - renderedBefore;
                const auto renderedWrites = probe.cacheWrites;

                probe.reset();     // 别处引起的重画：缓存没作废
                const auto blitBefore = pill->getTextDraws();
                paintFrame (editor, target, 1.0f, whole);
                const auto blitText = pill->getTextDraws() - blitBefore;
                const auto blitPaints = probe.paints, blitReads = probe.cacheReads, blitWrites = probe.cacheWrites;

                probe.reset();     // 呼吸一帧：只作废圆点那一块
                const auto pulseBefore = pill->getTextDraws(), barBefore = topBar->getTextDraws();
                pill->repaint (area);
                paintFrame (editor, target, 1.0f, editor.getLocalArea (pill, area));
                const auto pillPulse = pill->getTextDraws() - pulseBefore, barPulse = topBar->getTextDraws() - barBefore;
                const auto pulseWrites = probe.cacheWrites;
                pill->removeComponentListener (&probe);

                check (renderedText > 0 && renderedWrites == 1,
                       "T-032 a content change re-renders the whole pill into its cache (" + juce::String (renderedText) + " text item(s))");
                check (blitPaints == 1 && blitReads == 1 && blitWrites == 0 && blitText == 0,
                       "T-032 repaints from elsewhere only blit the cached pill (" + juce::String (blitReads) + " read, "
                           + juce::String (blitWrites) + " re-render, " + juce::String (blitText) + " text)");
                check (pillPulse == 0 && barPulse == 0 && pulseWrites == 1,
                       "T-032 a pulse frame re-renders only the dot into the cache and lays out no text (pill " + juce::String (pillPulse)
                           + ", top bar " + juce::String (barPulse) + ")");
            }

            // ⑲ 续（T-041，BUG-008）：小数物理像素比下也只重画圆点那一块。1.03293 是 T-039 实测的那个（xvfb 1280 × 1024 放不下
            //     默认大小 2，编辑器被缩进屏幕）；改前缓存「排除有效区」时各有效矩形往里收、接缝贯穿整颗胶囊，每段字都重排。
            //     每个像素比：整颗重画 → 呼吸若干帧（圆点确实变了）只作废圆点 → 贴图结果须与同一帧整颗重画逐字节相同。
            //     1 倍及以上一段字都不排版；低于 1 倍时一个设备像素比逻辑像素大，圆点区向外取整后可能与第一段字的 2 px 字形余量
            //     落进同一列设备像素——那一段照画才不会漏墨迹，只许这一段。
            if (pill != nullptr && topBar != nullptr)
            {
                const auto original = pill->getContent();
                auto pulsing = original;
                pulsing.warning = false;
                pulsing.pulse = true;
                pill->setContent (pulsing);

                const auto whole = editor.getLocalArea (pill, pill->getLocalBounds());
                juce::StringArray results;
                bool allPassed = pill->isPulsing();

                for (const auto scale : { 1.0f, 1.03293f, 1.25f, 1.5f, 1.75f, 2.5f, 0.7f, 0.8f, 0.9f })
                {
                    juce::Image frame (juce::Image::ARGB, (int) std::ceil ((float) editor.getWidth() * scale),
                                       (int) std::ceil ((float) editor.getHeight() * scale), true);
                    const auto device = (whole.toFloat() * scale).getSmallestIntegerContainer().getIntersection (frame.getBounds());
                    const auto pulseArea = pill->getPulseArea();

                    pill->repaint();   // 整颗作废：按这一像素比重建缓存
                    paintFrame (editor, frame, scale, whole);
                    const auto before = frame.getClippedImage (device).createCopy();

                    // 呼吸一帧只作废圆点：每批推 9 帧（八分之一周期）再画一次。起始相位不定（检查前消息循环里定时器走过几帧），
                    // 任一固定步长总有些相位前后亮度对称相等、圆点看不出变化——没变就再推一批，最多 4 批。
                    // 贴图都从清空的画布画起：ARGB 画布上反复贴图会把半透明像素一层层叠上去，比的就不只是缓存了。
                    PaintProbe probe;
                    pill->addComponentListener (&probe);
                    int pillPulse = 0, barPulse = 0, batches = 0;
                    bool changed = false;
                    juce::Image partial;

                    while (! changed && batches < 4)
                    {
                        const auto pillBefore = pill->getTextDraws(), barBefore = topBar->getTextDraws();

                        for (int i = 0; i < 9; ++i)
                            pill->advancePulse();

                        paintFrame (editor, frame, scale, editor.getLocalArea (pill, pulseArea));
                        pillPulse = juce::jmax (pillPulse, pill->getTextDraws() - pillBefore);
                        barPulse = juce::jmax (barPulse, topBar->getTextDraws() - barBefore);
                        ++batches;

                        frame.clear (frame.getBounds());
                        paintFrame (editor, frame, scale, whole);   // 只贴图：缓存里是局部重画过的胶囊
                        partial = frame.getClippedImage (device).createCopy();
                        changed = compareImages (before, partial).worst > 0;
                    }

                    const auto pulseWrites = probe.cacheWrites;
                    pill->removeComponentListener (&probe);

                    pill->repaint();                                // 对照：同一帧整颗重画
                    frame.clear (frame.getBounds());
                    paintFrame (editor, frame, scale, whole);
                    const auto full = frame.getClippedImage (device).createCopy();

                    const auto diff = compareImages (partial, full);
                    const auto passed = pulseWrites == batches && pillPulse <= (scale >= 1.0f ? 0 : 1) && barPulse == 0 && diff.worst == 0 && changed;
                    allPassed = allPassed && passed;
                    results.add (juce::String (scale, 5) + ": pill " + juce::String (pillPulse) + ", bar " + juce::String (barPulse)
                                 + ", writes " + juce::String (pulseWrites) + " of " + juce::String (batches) + ", vs full " + juce::String (diff.worst)
                                 + (changed ? "" : ", dot unchanged") + (passed ? "" : " FAIL"));
                }

                pill->setContent (original);
                pill->repaint();

                check (allPassed, "T-041 at fractional pixel ratios a pulse frame re-renders only the dot, lays out no text at >= 1x "
                                  "(at most the first segment below 1x) and matches a full re-render (" + results.joinIntoString ("; ") + ")");
            }
        }

        // ⑳ T-041（BUG-008）：对齐缓存（AlignedComponentCache，胶囊、粒子带 8 个文字小部件、频谱仪 5 个子组件共用）局部作废时，
        //     各像素比下只重画作废区（裁剪包围盒不超出作废区向外 2 px——设备像素向外取整、再按包围盒换回用户坐标各进一位），
        //     重画结果与整块重画逐字节相同。两种局部作废：自己 repaint (某一块)（胶囊呼吸）、子组件自己重画（频谱仪斜率控件的分段）；
        //     透明与不透明两种缓存图。
        {
            juce::StringArray failures;

            for (const auto opaque : { false, true })
            {
                juce::Component host;
                host.setBounds (0, 0, 240, 60);
                AlignedCacheProbe probe (opaque);
                host.addAndMakeVisible (probe);
                probe.setBounds (7, 9, 200, 32);

                const auto dotArea = probe.dot.getSmallestIntegerContainer();
                const auto childArea = probe.child.getBounds();

                for (const auto scale : { 1.0f, 1.03293f, 1.25f, 1.5f, 1.75f, 2.0f, 2.5f, 0.7f, 0.8f, 0.9f })
                {
                    juce::Image frame (juce::Image::ARGB, (int) std::ceil (240.0f * scale), (int) std::ceil (60.0f * scale), true);
                    const auto tag = juce::String (opaque ? "opaque " : "transparent ") + juce::String (scale, 5);

                    probe.repaint();   // 整块作废：按这一像素比重建缓存
                    paintFrame (host, frame, scale, std::nullopt);

                    const auto partialRepaint = [&] (const juce::String& kind, juce::Rectangle<int> invalidated, const std::function<void()>& invalidate)
                    {
                        const auto paintsBefore = probe.paints;
                        invalidate();
                        paintFrame (host, frame, scale, invalidated + probe.getPosition());
                        const auto clip = probe.lastClip;
                        const auto repainted = probe.paints - paintsBefore;

                        frame.clear (frame.getBounds());                  // 从清空的画布贴：透明画布上反复贴会层层叠加
                        paintFrame (host, frame, scale, std::nullopt);   // 只贴图
                        const auto partial = frame.createCopy();
                        probe.repaint();                                  // 对照：整块重画
                        frame.clear (frame.getBounds());
                        paintFrame (host, frame, scale, std::nullopt);
                        const auto diff = compareImages (partial, frame);

                        if (repainted != 1 || ! invalidated.expanded (2).contains (clip) || diff.worst != 0)
                            failures.add (tag + " " + kind + ": " + juce::String (repainted) + " paint(s), clip " + clip.toString()
                                          + " for " + invalidated.toString() + ", vs full " + juce::String (diff.worst));
                    };

                    partialRepaint ("self", dotArea, [&] { probe.shade = 1.0f - probe.shade; probe.repaint (dotArea); });
                    partialRepaint ("child", childArea, [&] { probe.child.repaint(); });
                }
            }

            check (failures.isEmpty(), "T-041 aligned cache re-renders only the invalidated area at ratios 0.7 - 2.5 and matches a full re-render ("
                                           + (failures.isEmpty() ? juce::String ("20 cases x 2") : failures.joinIntoString ("; ")) + ")");
        }

        return 0;
    }

    //==============================================================================
    // 频谱仪检查（README「--only check」）：
    //   IN 虚线（T-051）：自己切段的 createDashedStrokeReusing 与 JUCE 的 createDashedStroke 逐字节相同（评审 R-07 的改法不改像素）；
    //   增量重画 vs 整帧（评审 R-08 建议，守 R-02 那一类「脏区漏标、旧画面留在屏上」）：T-051 的 S8（守 ①），T-058 的 S9（守打开 PEAK 时的重建；
    //   R-02 ② 在 T-058 上成了第二道保险，单独撤它 S9 照样全过，要验 ② 只能在 main 上撤，见 T-058 证据的 M 变体（T-073 起 ② 已去掉，
    //   probeRegionWithoutPeak 守它的去向）；另一行守「PEAK 关、只剩峰值
    //   在落时不重建」）与播放（评审 B1 的第一个场景）——场景表 dirtyScenarios；
    //   打开 PEAK、换斜率、悬停那一刻画的是此刻的数据（T-058 的 S10，runPeakFreshness）。
    //   T-070（夜间评审补覆盖）：S9 斜率 0 与 4.5 各一份（零重建数真的重建，悬停也比）、S9b（PEAK 开 → 关）、S11（IN 开着：换斜率、IN 关 / 开）、
    //   播放加悬停；S12 与「没有历史的全新视图」逐字节对照（停播后、稳态 IN 换斜率）+ 曲线 / 读数的绝对探针（OUT 填充、平均线、峰值线、IN 虚线、
    //   悬停读数）——前面这些都是视图对它自己，两边一起错就全过，见 SpectrumTimeline 一带的说明。

    // 两条路径逐元素、逐位相同：元素类型、坐标的位模式（−0 与 +0 也分开）、绕数规则。
    bool samePathBits (const juce::Path& a, const juce::Path& b)
    {
        if (a.isUsingNonZeroWinding() != b.isUsingNonZeroWinding())
            return false;

        juce::Path::Iterator i (a), j (b);

        for (;;)
        {
            const auto more = i.next();

            if (more != j.next())
                return false;

            if (! more)
                return true;

            const float p[] = { i.x1, i.y1, i.x2, i.y2, i.x3, i.y3 };
            const float q[] = { j.x1, j.y1, j.x2, j.y2, j.x3, j.y3 };

            if (i.elementType != j.elementType || std::memcmp (p, q, sizeof (p)) != 0)
                return false;
        }
    }

    // 离屏编辑器没有对等体，repaint 到顶层就没人接：挂在编辑器上、只记账的缓存图——invalidate 时记下区域（编辑器坐标）、
    // 返回 true（照常往上传）。编辑器是顶层，paintEntireComponent 不经过缓存图；这个 paint 只在它被当成子组件画时才用到。
    struct DirtyRecorder  : public juce::CachedComponentImage
    {
        explicit DirtyRecorder (juce::Component& ownerToUse) : owner (ownerToUse) {}

        void paint (juce::Graphics& g) override                       { owner.paintEntireComponent (g, false); }
        bool invalidateAll() override                                 { dirty.add (owner.getLocalBounds()); return true; }
        bool invalidate (const juce::Rectangle<int>& area) override   { dirty.add (area); return true; }
        void releaseResources() override                              {}

        juce::Component& owner;
        juce::RectangleList<int> dirty;
    };

    // 一种窗口系统口径的「屏幕」：一直留着、每帧只重画脏区的图，以及它与整帧重画对不上的统计。
    struct DirtyScreen
    {
        DirtyScreen (const juce::String& nameToUse, bool useBoundingBox) : name (nameToUse), boundingBox (useBoundingBox) {}

        juce::String name;
        bool boundingBox = false;      // false = 只重画各脏矩形（Windows Direct2D、Linux）；true = 重画脏区的包围盒（macOS）
        juce::Image image;
        int ghostFrames = 0, worst = 0;
        juce::String firstGhost;
    };

    // 两张同尺寸、同格式的图在 area（设备像素）里比：返回不同的像素数；worst = 最大的通道差，at = 第一个不同的像素。
    // 先按行逐字节比（相同的行不拆像素），对不上的行再逐像素量差多少；通道差不超过 tolerance 的像素不算不同（默认零）。
    int countDifferences (const juce::Image& a, const juce::Image& b, juce::Rectangle<int> area, int& worst, juce::Point<int>& at, int tolerance = 0)
    {
        const juce::Image::BitmapData da (a, juce::Image::BitmapData::readOnly), db (b, juce::Image::BitmapData::readOnly);
        area = area.getIntersection ({ da.width, da.height });
        int different = 0;

        for (int y = area.getY(); y < area.getBottom(); ++y)
        {
            if (std::memcmp (da.getPixelPointer (area.getX(), y), db.getPixelPointer (area.getX(), y),
                             (size_t) area.getWidth() * (size_t) da.pixelStride) == 0)
                continue;

            for (int x = area.getX(); x < area.getRight(); ++x)
            {
                const auto p = da.getPixelColour (x, y), q = db.getPixelColour (x, y);
                const auto d = juce::jmax (std::abs ((int) p.getRed() - (int) q.getRed()), std::abs ((int) p.getGreen() - (int) q.getGreen()),
                                           std::abs ((int) p.getBlue() - (int) q.getBlue()), std::abs ((int) p.getAlpha() - (int) q.getAlpha()));

                if (d <= tolerance)
                    continue;

                if (different++ == 0)
                    at = { x, y };

                worst = juce::jmax (worst, d);
            }
        }

        return different;
    }

    // 稳态正弦（两声道相同，dBFS 是峰值电平）：相位跨块、跨帧连续。频谱仪的检查与基准用（T-051 的 S8 起：6 kHz −12 dBFS）。
    struct SineSource
    {
        SineSource (double frequency, double dBFS)
            : amplitude (juce::Decibels::decibelsToGain (dBFS)),
              increment (juce::MathConstants<double>::twoPi * frequency / sampleRate) {}

        void operator() (juce::AudioBuffer<float>& chunk)
        {
            for (int i = 0; i < chunk.getNumSamples(); ++i)
            {
                const auto sample = (float) (amplitude * std::sin (phase));
                phase = std::fmod (phase + increment, juce::MathConstants<double>::twoPi);
                chunk.setSample (0, i, sample);
                chunk.setSample (1, i, sample);
            }
        }

        double amplitude, increment, phase = 0.0;
    };

    // 频谱仪按帧推进的测试台（T-058 从 T-051 的 S8 检查里抽出来：增量重画检查（DirtyRun）与 --only spectrum-peak-bench 共用，
    // 之后的频谱仪检查照这个走）。一个会话（GLAZE 百分比），找好频谱仪、三个图例开关与斜率选择；调用方先设好开关 / 斜率，
    // attach() 把编辑器设成可见、挂上 DirtyRecorder。之后每帧照真实窗口的顺序：feed() 送 1 / fps 秒音频（processBlock，
    // 每块 ≤ 512 个样本）→ advance() = advanceForSnapshot（取样口 → 计算器 → setSpectrum，外加编辑器其余读数）→ takeDirty()
    // 取走这一帧记下的脏区。离屏没有窗口系统：按哪种口径重画、画进哪张「屏幕」图由调用方定（paintRegion）。
    struct SpectrumRig
    {
        enum Toggle { outputToggle = 0, inputToggle = 1, peakToggle = 2 };   // spectrumToggles 的顺序
        using AudioSource = std::function<void (juce::AudioBuffer<float>&)>;  // 往一块里写音频（块长 ≤ 512）

        explicit SpectrumRig (float glazePercent)
            : session (makeSession (glazePercent)), editor (*session->editor),
              view (findFirst<celadon::ui::SpectrumView> (editor)), toggles (spectrumToggles (editor)),
              slope (view != nullptr ? findFirst<celadon::ui::Segmented> (*view) : nullptr)   // 斜率选择是频谱仪里唯一的 Segmented
        {
            if (view != nullptr)
                area = editor.getLocalArea (view, view->getLocalBounds());
        }

        ~SpectrumRig()
        {
            if (recorder != nullptr)
                editor.setCachedComponentImage (nullptr);   // 编辑器接管着 recorder，这里删掉

            session->editor.reset();
            session->processor->releaseResources();
        }

        bool isValid() const noexcept   { return view != nullptr && ! toggles.empty() && slope != nullptr; }

        // 点一下图例开关（只有按下 / 抬起，不进不出：开关没有悬停的样子）。
        void clickToggle (Toggle which)
        {
            auto& toggle = *toggles[(size_t) which];
            click (toggle, toggle.getLocalBounds().getCentre().toFloat());
        }

        // 编辑器要「可见」：Component::internalRepaintUnchecked 对不可见的组件直接返回，repaint 传不到它的缓存图（真实窗口里
        // 宿主把编辑器设成可见；截图工具平时不设，离屏画不受影响）。没有对等体，设可见不会开窗口。
        void attach()
        {
            editor.setVisible (true);
            recorder = new DirtyRecorder (editor);
            editor.setCachedComponentImage (recorder);
        }

        // 这一帧该送的样本数：1 / fps 秒，除不尽的余数累到下一帧（48 kHz 下 60 / 120 帧正好 800 / 400 个）。
        int samplesForFrame (int framesPerSecond)
        {
            owedSamples += sampleRate / (double) framesPerSecond;
            const auto due = (int) owedSamples;
            owedSamples -= (double) due;
            return due;
        }

        void feed (int numSamples, const AudioSource& source)
        {
            for (int done = 0; done < numSamples;)
            {
                const auto n = juce::jmin (blockSize, numSamples - done);
                juce::AudioBuffer<float> chunk (session->buffer.getArrayOfWritePointers(), 2, n);
                source (chunk);
                session->processor->processBlock (chunk, session->midi);
                done += n;
            }
        }

        void advance (double seconds)   { editor.advanceForSnapshot (seconds); }

        // 取走到此刻为止记下的脏区（编辑器逻辑坐标；包括编辑器别处的，调用方按需 clipTo (area)）。
        juce::RectangleList<int> takeDirty()
        {
            auto region = recorder->dirty;
            recorder->dirty.clear();
            return region;
        }

        // 画对照图（整帧重画）不是真实窗口里的那一次绘制：它引起的标脏不算；之前记下的（画屏幕图时又标脏的）留给下一帧。
        void paintUnrecorded (const std::function<void()>& paint)
        {
            const auto kept = recorder->dirty;
            paint();
            recorder->dirty = kept;
        }

        // 窗口后备图：不透明的 RGB，编辑器大小 × 像素比；内容由调用方画（第一次通常整帧，见 paintRegion）。
        juce::Image newBackingImage (float scale) const
        {
            return { juce::Image::RGB, (int) std::ceil ((float) editor.getWidth() * scale), (int) std::ceil ((float) editor.getHeight() * scale), true };
        }

        juce::Rectangle<int> deviceArea (float scale) const   { return (area.toFloat() * scale).getSmallestIntegerContainer(); }

        std::unique_ptr<Session> session;
        CeladonAudioProcessorEditor& editor;
        celadon::ui::SpectrumView* view;
        std::vector<celadon::ui::ClickableComponent*> toggles;   // OUT / IN / PEAK；找不到为空
        celadon::ui::Segmented* slope;                           // 0 / 3 / 4.5 dB/oct = 下标 0 / 1 / 2
        juce::Rectangle<int> area;                               // 频谱仪在编辑器里的区域（逻辑坐标）
        DirtyRecorder* recorder = nullptr;                       // attach() 之后才有（编辑器持有）
        double owedSamples = 0.0;
    };

    // 增量重画 vs 整帧的推进器（T-051 的 S8 起；T-058 把逐帧比较抽出来，S8、S9 与「播放」共用）：一个测试台（调用方已设好开关、
    // 斜率并 attach）、一种像素比、几张不同口径的「屏幕」，外加同一时刻整帧重画频谱仪那一块的对照图。建的时候各屏整帧画一次
    // （窗口第一次显示）。每帧 = 1 / 60 s 音频 + advanceForSnapshot；暖机的帧不画、不比，省掉逐帧整帧对照的时间。
    struct DirtyRun
    {
        static constexpr int framesPerSecond = 60;

        DirtyRun (SpectrumRig& rigToUse, float scaleToUse, std::vector<DirtyScreen> screensToUse)
            : rig (rigToUse), scale (scaleToUse), screens (std::move (screensToUse)), reference (rig.newBackingImage (scale)),
              deviceArea (rig.deviceArea (scale)), bottomCentre (rig.area.getCentreX(), rig.area.getBottom() - 1)
        {
            for (auto& screen : screens)
                screen.image = rig.newBackingImage (scale);

            sync();
        }

        // 各屏整帧重画（建的时候；暖机之后、开始比之前也要来一次）。画的时候又标脏的不算。
        void sync()
        {
            for (auto& screen : screens)
                paintRegion (rig.editor, screen.image, scale, juce::RectangleList<int> (rig.editor.getLocalBounds()));

            rig.takeDirty();
        }

        // 暖机：推 count 帧，不画、不比，记下的脏区丢掉（曲线、峰值保持先走到要比的那一刻）。之后先 sync()。
        void warmUp (int count, const SpectrumRig::AudioSource& source)
        {
            for (int i = 0; i < count; ++i)
            {
                rig.feed (rig.samplesForFrame (framesPerSecond), source);
                rig.advance (1.0 / framesPerSecond);
                rig.takeDirty();
            }
        }

        // 一帧：送 1 / 60 s 音频 → 编辑器推一帧（频谱仪 setSpectrum、编辑器其余读数）→ 重画、比（repaintAndCompare）。
        // 返回这一帧记下的脏区（编辑器逻辑坐标，没裁剪）。
        juce::RectangleList<int> frame (const juce::String& stage, int index, const SpectrumRig::AudioSource& source)
        {
            rig.feed (rig.samplesForFrame (framesPerSecond), source);
            rig.advance (1.0 / framesPerSecond);
            return repaintAndCompare (stage, index);
        }

        // 连推 count 帧、每帧都比；返回其中的曲线帧数（isCurveFrame）。这一次调用里数到的第一个曲线帧记在 firstCurveFrame（每次调用先清空；失败时好定位）。
        int frames (const juce::String& stage, int count, const SpectrumRig::AudioSource& source)
        {
            int curveFrames = 0;
            firstCurveFrame.clear();

            for (int i = 0; i < count; ++i)
            {
                const auto region = frame (stage, i, source);

                if (! isCurveFrame (region))
                    continue;

                if (curveFrames++ == 0)
                    firstCurveFrame = stage + " frame " + juce::String (i) + ", dirty " + region.getBounds().toString();
            }

            return curveFrames;
        }

        // 跑 body，返回这期间频谱仪重建路径的次数（SpectrumView::getCurveBuildCount 的差；T-070）。
        int countBuilds (const std::function<void()>& body)
        {
            const auto before = rig.view->getCurveBuildCount();
            body();
            return (int) (rig.view->getCurveBuildCount() - before);
        }

        // 只重画、不推：窗口系统先送来别的事件（点击、换斜率）引起的重画，显示器的帧回调还没到（两者谁先到不固定）。
        juce::RectangleList<int> repaintOnly (const juce::String& stage)   { return repaintAndCompare (stage, 0); }

        // 曲线帧：脏区够到频谱仪底边正中——setSpectrum 判变了、重建路径后 repaintCurves 的区域总是从最高的曲线顶到底边、横跨整块，
        // 读数框与响度小窗的重画够不到；整块重画也算（与 --only spectrum-peak-bench 同一判据）。
        bool isCurveFrame (const juce::RectangleList<int>& region) const   { return region.containsPoint (bottomCentre); }

        SpectrumRig& rig;
        const float scale;
        std::vector<DirtyScreen> screens;
        juce::Image reference;
        const juce::Rectangle<int> deviceArea;
        const juce::Point<int> bottomCentre;
        int framesCompared = 0;
        int counted = 0;   // 场景自己数的帧（S8：静止最后 1 s 里频谱仪被标脏的帧；S9：PEAK 关、只剩峰值保持在落时的曲线帧）
        int builds = 0;    // 场景自己数的路径重建次数（countBuilds；S9：PEAK 关、只剩峰值在落的 30 帧里应为 0）
        juce::String firstCurveFrame;   // 最近一次 frames() 数到的第一个曲线帧：阶段、帧序号、脏区外框
        int tolerance = 0;   // 逐像素比时允许的通道差（零 = 逐字节相同；只有悬停场景放 1 级，见 hoverTolerance）

    private:
        // 各屏按口径重画记下的脏区（只画频谱仪那一块，别处两边都照画、不比）→ 整帧重画频谱仪那一块作对照 → 逐字节比。
        juce::RectangleList<int> repaintAndCompare (const juce::String& stage, int index)
        {
            const auto region = rig.takeDirty();

            for (auto& screen : screens)
            {
                auto clip = screen.boundingBox && ! region.isEmpty() ? juce::RectangleList<int> (region.getBounds()) : region;
                clip.clipTo (rig.area);
                paintRegion (rig.editor, screen.image, scale, clip);   // 画的时候又标脏的留在 recorder 里：真实窗口里留到下一帧画
            }

            rig.paintUnrecorded ([this] { paintRegion (rig.editor, reference, scale, juce::RectangleList<int> (rig.area)); });
            ++framesCompared;

            for (auto& screen : screens)
            {
                int worst = 0;
                juce::Point<int> at;
                const auto different = countDifferences (screen.image, reference, deviceArea, worst, at, tolerance);

                if (different == 0)
                    continue;

                if (screen.ghostFrames++ == 0)
                    screen.firstGhost = stage + " frame " + juce::String (index) + ": " + juce::String (different) + " px, first at device ("
                                      + juce::String (at.x) + ", " + juce::String (at.y) + ")";

                screen.worst = juce::jmax (screen.worst, worst);
            }

            return region;
        }
    };

    // 一个增量重画场景跑完（×1 与 ×2 合起来）的结果。
    struct DirtyOutcome
    {
        std::vector<DirtyScreen> screens;   // 各口径的残影统计：×1 矩形列表、×1 包围盒（、×2 矩形列表）
        int framesCompared = 0;             // 各像素比合计
        std::vector<int> counted;           // DirtyRun::counted，每种像素比一个（×1、×2）
        std::vector<int> builds;            // DirtyRun::builds，同上
        juce::StringArray firstCurve;       // DirtyRun::firstCurveFrame，同上
        juce::String error;

        // 「0 ghosts over N frames」，或各屏的残影：「x1 rect list 6 frame(s), worst 164 (first: …)」。
        juce::String ghostSummary() const
        {
            juce::StringArray ghosts;

            for (const auto& screen : screens)
                if (screen.ghostFrames > 0)
                    ghosts.add (screen.name + " " + juce::String (screen.ghostFrames) + " frame(s), worst " + juce::String (screen.worst)
                                + " (first: " + screen.firstGhost + ")");

            return (ghosts.isEmpty() ? juce::String ("0 ghosts") : ghosts.joinIntoString ("; ")) + " over " + juce::String (framesCompared) + " frames";
        }

        bool noGhosts() const
        {
            for (const auto& screen : screens)
                if (screen.ghostFrames > 0)
                    return false;

            return error.isEmpty();
        }

        // 「×1 / ×2」两次数到的帧
        juce::String countedText() const
        {
            juce::StringArray parts;

            for (const auto value : counted)
                parts.add (juce::String (value));

            return parts.joinIntoString (" / ");
        }

        bool countedAllZero() const
        {
            for (const auto value : counted)
                if (value != 0)
                    return false;

            return error.isEmpty();
        }

        // 「×1 / ×2」两次数到的路径重建次数
        juce::String buildsText() const
        {
            juce::StringArray parts;

            for (const auto value : builds)
                parts.add (juce::String (value));

            return parts.joinIntoString (" / ");
        }

        bool buildsAllZero() const
        {
            for (const auto value : builds)
                if (value != 0)
                    return false;

            return error.isEmpty();
        }

        // 数到曲线帧时，第一个曲线帧在哪（失败信息用；没有就是空串）。
        juce::String firstCurveText() const
        {
            for (const auto& where : firstCurve)
                if (where.isNotEmpty())
                    return " (first: " + where + ")";

            return {};
        }
    };

    using CheckFunction = std::function<void (bool, const juce::String&)>;

    // 增量重画检查的一个场景（场景表见 dirtyScenarios）。
    struct DirtyScenario
    {
        std::function<void (SpectrumRig&)> setup;                                 // 挂记账缓存图之前：开关、斜率
        std::function<void (SpectrumRig&, DirtyRun&)> script;                     // 之后一帧帧走
        bool atTwiceScale;                                                        // 也跑 ×2（矩形列表）
        std::function<void (const DirtyOutcome&, const CheckFunction&)> report;   // 拼检查行
    };

    // 跑一个场景：×1（矩形列表、包围盒两张屏共用同一串帧）一遍，atTwiceScale 时再起一个会话按 ×2（矩形列表）跑一遍。
    DirtyOutcome runDirtyScenario (const DirtyScenario& scenario)
    {
        DirtyOutcome outcome;

        const auto runAt = [&] (float scale, std::vector<DirtyScreen> screens)
        {
            SpectrumRig rig (0.0f);   // GLAZE 0：链近乎直通

            if (! rig.isValid())
            {
                outcome.error = "spectrum view, its three legend toggles or the slope control not found";
                outcome.counted.push_back (0);
                outcome.builds.push_back (0);
                outcome.firstCurve.add ({});
                return;
            }

            scenario.setup (rig);
            rig.attach();

            DirtyRun run (rig, scale, std::move (screens));   // 先于测试台析构
            scenario.script (rig, run);

            outcome.framesCompared += run.framesCompared;
            outcome.counted.push_back (run.counted);
            outcome.builds.push_back (run.builds);
            outcome.firstCurve.add (run.firstCurveFrame);

            for (auto& screen : run.screens)
                outcome.screens.push_back (std::move (screen));
        };

        runAt (1.0f, { { "x1 rect list", false }, { "x1 bounding box", true } });

        if (scenario.atTwiceScale)
            runAt (2.0f, { { "x2 rect list", false } });

        return outcome;
    }

    // 悬停场景逐像素比时允许的通道差：悬停之后光标列的增量重画与整帧重画在面板最上、最下一行（光标列的左边缘，clip 边界上）各有一个像素差 1 级
    // （8 位里的 1，肉眼不可见）——只出现在悬停之后、只在 clip 边界与面板边框相交的像素，是 JUCE 软件渲染器裁剪边界的取整，不是残影；
    // 真正的残影（旧光标、旧曲线没擦掉）差的是几十级以上（S9 的 worst 是 106、S8 的 164），所以悬停场景放 1 级的容差，其余场景一律零容差。
    constexpr int hoverTolerance = 1;

    // 鼠标移到某个频率上（只有移动，不进不出：频谱仪的悬停只看 mouseMove）。
    void hoverAt (celadon::ui::SpectrumView& view, float frequency)
    {
        const juce::Point<float> at (celadon::ui::SpectrumView::xOf (frequency), 120.0f);
        view.mouseMove (mouseEvent (view, at, {}, at));
    }

    // ---- S9（T-058；T-070 起斜率 0 与 4.5 各一份，4.5 是出厂默认；守打开 PEAK 时的重建（R-02 ② 在 T-058 上成了第二道保险，单独撤它 S9 照样全过，
    // 要验 ② 只能在 main 上撤，见 T-058 证据的 M 变体；T-073 起 ② 已去掉）；外加「PEAK 关、只剩峰值在落时不重建」）----
    // 峰值线关、斜率 slopeIndex：6 kHz −12 dBFS 正弦 2 s → 数字静音（暖机，不画）。停播后 OUT 约 0.3 s 落到底边以下、平均线约 1 s 内也落到
    // 底边以下，峰值保持 1.5 s 后按 12 dB/s 落——5.5 ～ 6.0 s 这 30 帧只剩它在落：PEAK 关着时屏上什么都没变，曲线帧（重建 + 重画曲线区）
    // 应为 0（main 218789a 每帧一次）。T-070：「不重建」数真的重建（SpectrumView::getCurveBuildCount 的差，评审 F5：原来拿脏区当代理，
    // 每帧无条件重建、只在判变了时才重画的改法数不出来），脏区里的曲线帧一并数。6.0 s 时 6 kHz 的保持值约 −42 dB（斜率 0 时 y ≈ 114，在响度
    // 小窗 y ≤ 100 之下），远高于底边上 OUT / 平均线的区域：这时点开 PEAK → 只重画一帧（点击的重画先到）→ 中间不推、紧接着切 B（峰值线清到
    // 底线）→ 静音 30 帧，每帧都比。点开之后只要推过一帧（下一次判变了的区域含峰值线）、或在稳态正弦里点开（峰值线贴着 OUT 峰顶、被它的区域
    // 盖住），撤掉 ② 也漏不出来。T-070：停播后鼠标悬停在 6 kHz，光标列与读数框的增量重画也与整帧比（评审 F6）。
    DirtyScenario peakHoldScenario (int slopeIndex, const juce::String& slopeText)
    {
        const SpectrumRig::AudioSource silence = [] (juce::AudioBuffer<float>& chunk) { chunk.clear(); };

        return { [slopeIndex] (SpectrumRig& rig)
                 {
                     rig.clickToggle (SpectrumRig::peakToggle);
                     rig.slope->setSelected (slopeIndex, true);
                 },
                 [silence] (SpectrumRig& rig, DirtyRun& run)
                 {
                     SineSource sine (6000.0, -12.0);
                     run.warmUp (2 * DirtyRun::framesPerSecond, std::ref (sine));
                     run.warmUp (210, silence);   // 2.0 ～ 5.5 s
                     run.sync();
                     run.tolerance = hoverTolerance;
                     hoverAt (*rig.view, 6000.0f);
                     run.repaintOnly ("hover repaint");
                     juce::String window;   // 计数窗口里的第一个曲线帧（后面切 B 的帧不算）
                     run.builds = run.countBuilds ([&]
                                                  {
                                                      run.counted = run.frames ("silence, peak off", 30, silence);   // 5.5 ～ 6.0 s
                                                      window = run.firstCurveFrame;
                                                  });

                     rig.clickToggle (SpectrumRig::peakToggle);
                     run.repaintOnly ("peak on repaint");
                     rig.editor.setListeningToOriginal (true);
                     run.frames ("B", 30, silence);
                     run.firstCurveFrame = window;
                 },
                 true,
                 [slopeText] (const DirtyOutcome& outcome, const CheckFunction& check)
                 {
                     check (outcome.noGhosts(),
                            "T-058 spectrum incremental repaint == full repaint (S9: GLAZE 0, peak off, slope " + slopeText
                                + ", 6 kHz -12 dBFS sine 2 s, silence 4 s, hover at 6 kHz, peak on, then B; " + outcome.ghostSummary() + ")");
                     check (outcome.countedAllZero() && outcome.buildsAllZero(),
                            "T-058 spectrum with the peak line off rebuilds no curves while only the peak hold falls (S9, slope " + slopeText
                                + ", silence 5.5 - 6 s: "
                                + (outcome.error.isNotEmpty() ? outcome.error
                                                              : outcome.countedText() + " of 30 frames reach the curve area, " + outcome.buildsText()
                                                                    + " path rebuilds, needs 0" + outcome.firstCurveText())
                                + ")");
                 } };
    }

    // ---- S9b（T-070，评审 F1）：已显示过的视图上 PEAK 开 → 关 ----
    // 关掉不重建：峰值线靠 setPeakShown(false) 里的整面重画擦掉。T-058 之前峰值保持往下落会每帧判变了、顺手把它擦掉，现在这条 repaint 是唯一的
    // 手段，撤掉它其他检查都看不出（它们都在第一帧之前把 PEAK 关好，或只测 关 → 开）。PEAK 开着（默认）、斜率 slopeIndex，6 kHz −12 dBFS
    // 正弦 2 s → 数字静音到 5.5 s（暖机，不画）；这时峰值保持线在 y ≈ 114 附近往下落：先比 30 帧（PEAK 开）→ 点关 → 只重画一帧（点击先到）
    // → 再比 30 帧静音（PEAK 关）。
    DirtyScenario peakOffScenario (int slopeIndex, const juce::String& slopeText)
    {
        const SpectrumRig::AudioSource silence = [] (juce::AudioBuffer<float>& chunk) { chunk.clear(); };

        return { [slopeIndex] (SpectrumRig& rig) { rig.slope->setSelected (slopeIndex, true); },
                 [silence] (SpectrumRig& rig, DirtyRun& run)
                 {
                     SineSource sine (6000.0, -12.0);
                     run.warmUp (2 * DirtyRun::framesPerSecond, std::ref (sine));
                     run.warmUp (210, silence);   // 2.0 ～ 5.5 s
                     run.sync();
                     run.frames ("silence, peak on", 30, silence);
                     rig.clickToggle (SpectrumRig::peakToggle);
                     run.repaintOnly ("peak off repaint");
                     run.frames ("silence, peak off", 30, silence);
                 },
                 true,
                 [slopeText] (const DirtyOutcome& outcome, const CheckFunction& check)
                 {
                     check (outcome.noGhosts(),
                            "T-070 spectrum incremental repaint == full repaint (S9b: GLAZE 0, slope " + slopeText
                                + ", 6 kHz -12 dBFS sine 2 s, silence to 5.5 s, 30 frames, peak off, 30 frames; " + outcome.ghostSummary() + ")");
                 } };
    }

    // ---- S11（T-070，评审 F4）：IN 虚线开着 ----
    // T-058 动过 IN 的两处（路径一重建就作废展开好的虚线；比 IN 要先看计算器此刻有没有 IN 的曲线），之前没有任何检查进过 IN。PEAK 关、IN 开、
    // 斜率 0，合成音乐：暖机 1 s，比 1 s → 斜率换 3 → IN 关 → IN 开，每一步先只重画一帧（点击的重画先到）再比 30 帧。与整帧重画对照；
    // 「重建时没作废展开好的虚线」这类两边一起错的，由 S12（与全新视图对照）管。
    DirtyScenario inputScenario()
    {
        return { [] (SpectrumRig& rig)
                 {
                     rig.clickToggle (SpectrumRig::peakToggle);
                     rig.clickToggle (SpectrumRig::inputToggle);
                     rig.slope->setSelected (0, true);
                 },
                 [] (SpectrumRig& rig, DirtyRun& run)
                 {
                     auto& music = rig.session->music;
                     const SpectrumRig::AudioSource source = [&music] (juce::AudioBuffer<float>& chunk) { music.render (chunk, chunk.getNumSamples()); };
                     run.warmUp (DirtyRun::framesPerSecond, source);
                     run.sync();
                     run.frames ("music, IN on", DirtyRun::framesPerSecond, source);
                     rig.slope->setSelected (1, true);
                     run.repaintOnly ("slope 3 repaint");
                     run.frames ("music, IN on, slope 3", 30, source);
                     rig.clickToggle (SpectrumRig::inputToggle);
                     run.repaintOnly ("IN off repaint");
                     run.frames ("music, IN off", 30, source);
                     rig.clickToggle (SpectrumRig::inputToggle);
                     run.repaintOnly ("IN on repaint");
                     run.frames ("music, IN on again", 30, source);
                 },
                 false,
                 [] (const DirtyOutcome& outcome, const CheckFunction& check)
                 {
                     check (outcome.noGhosts(),
                            "T-070 spectrum incremental repaint == full repaint (S11: GLAZE 0, music, peak off, IN on, slope 0 -> 3, IN off, IN on; "
                                + outcome.ghostSummary() + ")");
                 } };
    }

    // 增量重画检查的场景表（每个场景各起会话；×1 两种口径 + ×2 矩形列表 = 评审 B1 的三种口径）。
    std::vector<DirtyScenario> dirtyScenarios()
    {
        const SpectrumRig::AudioSource silence = [] (juce::AudioBuffer<float>& chunk) { chunk.clear(); };

        return {
            // ---- S8（T-051；评审 B1 的 S8，守 R-02 ①：paint 里重建路径时把画出的区域并进旧区域）----
            // 峰值线关（开着时峰值保持把脏区一直撑到峰顶，盖住要找的漏标）、斜率 0，6 kHz −12 dBFS 稳态正弦 12 s（最后 1 s 频谱仪应
            // 0 帧被标脏：曲线已静止）→ 斜率换 4.5（6 kHz 抬高约 11.6 dB；先只重画一帧，路径在 paint 里重建，见下）→ 切到 B（OUT 清零
            // 重来，最高的曲线一下子没了）→ 再跑 1 s。每帧都与整帧重画比（不只比切 B 的那一帧）。
            { [] (SpectrumRig& rig)
              {
                  rig.clickToggle (SpectrumRig::peakToggle);
                  rig.slope->setSelected (0, true);
              },
              [] (SpectrumRig& rig, DirtyRun& run)
              {
                  SineSource sine (6000.0, -12.0);
                  const SpectrumRig::AudioSource source = std::ref (sine);
                  constexpr int steadyFrames = 12 * DirtyRun::framesPerSecond;

                  for (int i = 0; i < steadyFrames; ++i)
                      if (run.frame ("steady sine", i, source).intersects (rig.area) && i >= steadyFrames - DirtyRun::framesPerSecond)
                          ++run.counted;

                  // 斜率 0 → 4.5：点击引起的整面重画先到、显示器的帧回调后到，路径在 paint 里重建——这一帧只重画、不推。评审复现时
                  // （577ded0）setSpectrum 按未加斜率的 dB 判变化，曲线静止时换斜率后下一帧也不重画，等价于这里；T-036 修订（867b454，
                  // 评审 R-07）改按加斜率后的纵坐标判之后，PEAK 关着也照比的峰值保持数据（压在底边下、还在往下落）换到 4.5 时露出来，
                  // 先推一帧的话 setSpectrum 当帧就判变了、旧区域跟上了，照原样走 S8 抓不到 R-02 那一类漏标（T-051）。T-058 起 PEAK
                  // 关着不比峰值线、重建都先换成此刻的值，这一帧的 setSpectrum 不再判变（临时副本里加打印核过），先推一帧也抓得到 ①；
                  // 照留「只重画」：与 T-051 逐字相同，也正是点击先到的那种顺序。
                  rig.slope->setSelected (2, true);
                  run.repaintOnly ("slope 4.5 repaint");

                  rig.editor.setListeningToOriginal (true);

                  for (int i = 0; i < DirtyRun::framesPerSecond; ++i)
                      run.frame ("B", i, source);
              },
              true,
              [] (const DirtyOutcome& outcome, const CheckFunction& check)
              {
                  check (outcome.noGhosts() && outcome.countedAllZero(),
                         "T-051 spectrum incremental repaint == full repaint (S8: GLAZE 0, peak off, 6 kHz -12 dBFS sine 12 s, slope 0 -> 4.5, then B; "
                             + (outcome.error.isNotEmpty() ? outcome.error
                                                           : outcome.ghostSummary() + "; spectrum marked dirty in " + outcome.countedText()
                                                                 + " of the last 60 steady frames, needs 0")
                             + ")");
              } },

            peakHoldScenario (0, "0"),
            peakHoldScenario (2, "4.5"),
            peakOffScenario (0, "0"),
            peakOffScenario (2, "4.5"),
            inputScenario(),

            // ---- 播放（T-058；评审 B1 的第一个场景）：最常见的路径，守 curveRegion 的描边外扩余量与各条线都进区域（峰值线常高出 OUT）----
            // 峰值线开、斜率默认 4.5，合成音乐暖机 1 s，再比 2 s。只跑 ×1：音乐每帧整块在变，逐帧整帧对照在 ×2 下最贵；撤掉余量或峰值线
            // 在 ×1 矩形列表下就失败（红绿验证）。repaintCurves 并旧区域它守不住：峰值线区域的顶总在最近的 OUT 之上、每帧只落约 0.6 px，
            // 新区域本身就盖住了——那一条由 S8 / S9 守（切 B 时曲线一下子清到底）。T-070：鼠标悬停在 3 kHz（光标圆点跟着曲线走，
            // repaintCurves 补光标那一列；评审 F6）。
            { [] (SpectrumRig&) {},
              [] (SpectrumRig& rig, DirtyRun& run)
              {
                  auto& music = rig.session->music;
                  const SpectrumRig::AudioSource source = [&music] (juce::AudioBuffer<float>& chunk) { music.render (chunk, chunk.getNumSamples()); };
                  run.warmUp (DirtyRun::framesPerSecond, source);
                  run.sync();
                  run.tolerance = hoverTolerance;
                  hoverAt (*rig.view, 3000.0f);
                  run.repaintOnly ("hover repaint");
                  run.frames ("music", 2 * DirtyRun::framesPerSecond, source);
              },
              false,
              [] (const DirtyOutcome& outcome, const CheckFunction& check)
              {
                  check (outcome.noGhosts(),
                         "T-058 spectrum incremental repaint == full repaint (B1 playback: GLAZE 0, music, peak on, slope 4.5, hover at 3 kHz; "
                             + outcome.ghostSummary() + ")");
              } },
        };
    }

    // 打开 PEAK、换斜率、悬停那一刻画的是此刻的数据（T-058 的 S10）。残影检查拿同一个视图的整帧重画作对照：路径是旧的时两边一样旧，
    // 抓不到。这里要一个一直「新」的参照：不建编辑器，一个计算器（48 kHz）+ 两个不进窗口的频谱仪——test 的 PEAK 先关、到时点开；
    // ref 的一直开着（峰值保持在落时每帧判变了、按此刻的值重建）。每帧：送 1 / 60 s 音频 → update → 两边 setSpectrum（两边 IN 都关，
    // 告诉计算器的是同一个值）。6 kHz −12 dBFS 正弦 2 s → 数字静音 4 s，暖机不画：停播后 OUT 约 0.3 s 钳到底边、斜率 0 时平均线一直在
    // 底边以下，两边画出来是同一条底线；峰值保持 1.5 s 后往下落，6.0 s 时 6 kHz 约 −42 dB。到 6.0 s：
    //   a 点开 test 的 PEAK；b 两边先同时把斜率换成 4.5，再点开 test 的 PEAK——都是不推、两边各整帧画一张 ×1 图逐字节比；
    //   c 不点 PEAK，两边鼠标都移到 6 kHz，只比读数框那一片（图例开关左边、上沿：test 的 PEAK 还关着，图例与峰值线两边本来就不同）。
    // 之后再推 30 帧，每帧都比同一片。
    enum class FreshnessCase { peakOn, slopeAndPeakOn, hoverReadout };

    struct FreshnessResult
    {
        int switchDifferent = 0, laterFramesDifferent = 0, worst = 0;   // 那一刻不同的像素数；之后 30 帧里有不同的帧数
        int firstLaterFrame = -1;                                       // 之后 30 帧里第一个有不同的帧（从 0 数；没有 = -1）
        juce::Point<int> firstAt { -1, -1 };
        juce::String error;
    };

    FreshnessResult runPeakFreshness (FreshnessCase which)
    {
        using namespace celadon::ui;
        FreshnessResult result;
        SpectrumAnalyser analyser;
        SpectrumView test, ref;
        const auto testToggles = legendToggles (test);
        auto* testSlope = findFirst<Segmented> (test);   // 斜率选择是频谱仪里唯一的 Segmented
        auto* refSlope = findFirst<Segmented> (ref);

        if (testToggles.empty() || testSlope == nullptr || refSlope == nullptr)
        {
            result.error = "legend toggles or the slope control not found";
            return result;
        }

        auto& testPeak = *testToggles[(size_t) SpectrumRig::peakToggle];
        const auto clickPeak = [&testPeak] { click (testPeak, testPeak.getLocalBounds().getCentre().toFloat()); };   // 不进不出：没有悬停的样子

        clickPeak();   // test 的 PEAK 关
        testSlope->setSelected (0, true);
        refSlope->setSelected (0, true);

        constexpr int framesPerSecond = 60;
        constexpr int samplesPerFrame = (int) sampleRate / framesPerSecond;
        juce::AudioBuffer<float> chunk (2, samplesPerFrame);
        SineSource sine (6000.0, -12.0);

        const auto advance = [&] (bool sound)
        {
            if (sound)
                sine (chunk);
            else
                chunk.clear();

            analyser.push (SpectrumAnalyser::Stream::output, chunk.getReadPointer (0), samplesPerFrame);
            analyser.update (1.0 / framesPerSecond);
            test.setSpectrum (analyser, 1.0 / framesPerSecond);
            ref.setSpectrum (analyser, 1.0 / framesPerSecond);
        };

        // 比哪一片：a / b 整个视图；c 读数框那一片（图例开关左边、开关底下再留 6 px）。
        const auto area = which == FreshnessCase::hoverReadout
                              ? juce::Rectangle<int> (0, 0, testToggles.front()->getX(), testToggles.front()->getBottom() + 6)
                              : test.getLocalBounds();

        // 两边各整帧画一张（不透明的 RGB，同一底色），返回 area 里不同的像素数。
        const auto compare = [&] (int& worst, juce::Point<int>& at)
        {
            const auto snapshot = [] (SpectrumView& view)
            {
                juce::Image image (juce::Image::RGB, view.getWidth(), view.getHeight(), true);
                juce::Graphics g (image);
                view.paintEntireComponent (g, true);
                return image;
            };

            const auto a = snapshot (test), b = snapshot (ref);
            return countDifferences (a, b, area, worst, at);
        };

        for (int i = 0; i < 2 * framesPerSecond; ++i)
            advance (true);

        for (int i = 0; i < 4 * framesPerSecond; ++i)
            advance (false);

        if (which == FreshnessCase::hoverReadout)
        {
            // 鼠标移到 6 kHz（只有移动，不进不出）：读数立即换成光标处的值——test 画出来的那组还停在停播后约 0.3 s，ref 的是此刻的。
            const juce::Point<float> at (SpectrumView::xOf (6000.0f), 120.0f);
            test.mouseMove (mouseEvent (test, at, {}, at));
            ref.mouseMove (mouseEvent (ref, at, {}, at));
        }
        else
        {
            if (which == FreshnessCase::slopeAndPeakOn)
            {
                testSlope->setSelected (2, true);
                refSlope->setSelected (2, true);
            }

            clickPeak();   // test 的 PEAK 开：点击的重画先到，这一帧不推
        }

        result.switchDifferent = compare (result.worst, result.firstAt);

        for (int i = 0; i < 30; ++i)
        {
            advance (false);
            int worst = 0;
            juce::Point<int> at;

            if (compare (worst, at) == 0)
                continue;

            if (result.laterFramesDifferent++ == 0)
            {
                result.firstLaterFrame = i;

                if (result.switchDifferent == 0)
                    result.firstAt = at;
            }

            result.worst = juce::jmax (result.worst, worst);
        }

        return result;
    }

    //==============================================================================
    // S12（T-070，评审 F2、F3、F4、F6、F7）：与「没有历史的视图」对照 + 曲线真的画出来了的绝对探针。
    // 之前的检查都是「视图对它自己」：增量重画对整帧重画、开着的视图对一个一直开着 PEAK 的视图——两边跑同一份代码，数据通路上丢了一条曲线
    // （拷贝漏了、建路径漏了）、「看没看变化」少比一条线、换斜率后虚线没作废，两边一起错、全过。这里要两种不靠历史的参照：
    //   ① 每个时刻现起一个全新的视图（同开关、同斜率、同悬停），只 setSpectrum 一次、整帧画一张——它画的就是计算器此刻的数据；长期运行的
    //      视图在曲线都静止的时刻（停播 4 s 之后；稳态正弦 10 s 之后换斜率）必须与它逐字节相同；
    //   ② 绝对探针：曲线应该在的那个点（位置由计算器的值与 SpectrumView::xOf / yOf 算出）附近，必须有像素与「从没喂过数据的空视图」不同；
    //      悬停的读数必须等于计算器此刻 OUT 的值（加斜率）。
    // 不建编辑器：一个计算器（48 kHz）+ 长期运行的视图，每帧送 1 / 60 s 音频 → update → setSpectrum，与 S10 同一套。

    // 斜率选择三档的值（dB / 倍频程，绕 1 kHz 转），下标与 Segmented 一致。
    constexpr float spectrumSlopes[] = { 0.0f, 3.0f, 4.5f };

    // 视图的开关与斜率（频谱仪默认：OUT 开、IN 关、PEAK 开、斜率 4.5）。找不到控件返回 false。
    bool configureSpectrumView (celadon::ui::SpectrumView& view, bool peakOn, bool inputOn, int slopeIndex)
    {
        const auto toggles = legendToggles (view);
        auto* slopeControl = findFirst<celadon::ui::Segmented> (view);

        if (toggles.empty() || slopeControl == nullptr)
            return false;

        const auto press = [&toggles] (SpectrumRig::Toggle which)
        {
            auto& toggle = *toggles[(size_t) which];
            click (toggle, toggle.getLocalBounds().getCentre().toFloat());   // 不进不出：没有悬停的样子
        };

        if (! peakOn)
            press (SpectrumRig::peakToggle);

        if (inputOn)
            press (SpectrumRig::inputToggle);

        slopeControl->setSelected (slopeIndex, true);
        return true;
    }

    juce::Image snapshotOf (celadon::ui::SpectrumView& view)
    {
        juce::Image image (juce::Image::RGB, view.getWidth(), view.getHeight(), true);
        juce::Graphics g (image);
        view.paintEntireComponent (g, true);
        return image;
    }

    // 一条时间线：一个计算器 + 一个长期运行的视图。step() = 一帧（1 / 60 s）：6 kHz −12 dBFS 正弦、白噪声（宽带：平均线要宽带内容才会高出面板底边）
    // 或数字静音 → update → setSpectrum，OUT 与 IN 送同一份。freshView() / emptyView() 起对照用的视图。
    struct SpectrumTimeline
    {
        static constexpr int framesPerSecond = 60;
        static constexpr int samplesPerFrame = (int) sampleRate / framesPerSecond;

        enum class Sound { silence, sine, noise };

        // 构造时先整帧画一次：视图第一次 paint 时（curvePathsValid 为假）会用「此刻」的值建路径——不先画，第一次比较那一刻才建，
        // 之前「看没看变化」少比的曲线从没更新过也不露馅。先画一次（此刻全是底线），之后的路径只能靠 setSpectrum 里的判变来更新，
        // 与真实窗口里从头一帧帧画一样。
        SpectrumTimeline (bool peakOn, bool inputOn, int slopeIndex)
            : peak (peakOn), input (inputOn), slope (slopeIndex), valid (configureSpectrumView (view, peakOn, inputOn, slopeIndex))
        {
            if (valid)
                snapshotOf (view);
        }

        void step (Sound sound)
        {
            if (sound == Sound::sine)
            {
                sine (chunk);
            }
            else if (sound == Sound::noise)
            {
                for (int i = 0; i < samplesPerFrame; ++i)
                {
                    const auto sample = 0.1f * (random.nextFloat() * 2.0f - 1.0f);   // 约 −25 dBFS RMS
                    chunk.setSample (0, i, sample);
                    chunk.setSample (1, i, sample);
                }
            }
            else
            {
                chunk.clear();
            }

            analyser.push (celadon::ui::SpectrumAnalyser::Stream::output, chunk.getReadPointer (0), samplesPerFrame);
            analyser.push (celadon::ui::SpectrumAnalyser::Stream::input, chunk.getReadPointer (0), samplesPerFrame);
            analyser.update (1.0 / framesPerSecond);
            view.setSpectrum (analyser, 1.0 / framesPerSecond);
        }

        void steps (int count, Sound sound)
        {
            for (int i = 0; i < count; ++i)
                step (sound);
        }

        void changeSlope (int slopeIndex)
        {
            slope = slopeIndex;
            findFirst<celadon::ui::Segmented> (view)->setSelected (slopeIndex, true);   // 点击引起的重画先到：不推帧
        }

        void hover (float frequency)
        {
            hoverFrequency = frequency;
            hoverAt (view, frequency);
        }

        // 此刻现起一个全新视图：画的是计算器此刻的数据。读数框的文字约 0.27 s 才刷新一次，而选斜率（configureSpectrumView）会把这个计时清零，
        // 所以数据不动、连着 setSpectrum 20 次（1 / 3 s）让它把读数刷新到此刻——长期运行的视图在曲线静止的时刻读数也早已是此刻的。
        std::unique_ptr<celadon::ui::SpectrumView> freshView()
        {
            auto fresh = std::make_unique<celadon::ui::SpectrumView>();
            configureSpectrumView (*fresh, peak, input, slope);

            for (int i = 0; i < 20; ++i)
                fresh->setSpectrum (analyser, 1.0 / framesPerSecond);

            if (hoverFrequency.has_value())
                hoverAt (*fresh, *hoverFrequency);

            return fresh;
        }

        // 同开关、同斜率、从没喂过数据的视图：绝对探针的底。
        std::unique_ptr<celadon::ui::SpectrumView> emptyView() const
        {
            auto empty = std::make_unique<celadon::ui::SpectrumView>();
            configureSpectrumView (*empty, peak, input, slope);
            return empty;
        }

        bool peak, input;
        int slope;
        std::optional<float> hoverFrequency;
        celadon::ui::SpectrumAnalyser analyser;
        celadon::ui::SpectrumView view;
        bool valid;
        SineSource sine { 6000.0, -12.0 };
        juce::Random random { 20261004 };
        juce::AudioBuffer<float> chunk { 2, samplesPerFrame };
    };

    // 长期运行的视图 == 全新视图：在若干时刻整个面板逐字节比（零容差），记下不同的。
    struct FreshEquality
    {
        int moments = 0, differing = 0;
        juce::String firstDifference, error;

        // tolerance：通道差不超过它的像素不算不同（默认零）。曲线停在「差不到 0.1 px」的滞后里时（视图只在累计漂移超过 0.1 px 才重建），实线的 AA
        // 会差十几级——S12c 放 24 级；少比一条线、判变化时不加斜率，曲线停在半空，差的是几十到两百多级。
        // belowReadout：只比读数框（y < 40）以下：宽带噪声每帧都不同，读数框的「最响的峰」约 0.27 s 才刷新一次，与全新视图的读数合法地不同。
        void compare (SpectrumTimeline& timeline, const juce::String& moment, int tolerance = 0, bool belowReadout = false)
        {
            ++moments;
            const auto fresh = timeline.freshView();
            int worst = 0;
            juce::Point<int> at;
            const auto bounds = timeline.view.getLocalBounds();
            const auto area = belowReadout ? bounds.withTrimmedTop (40) : bounds;
            const auto different = countDifferences (snapshotOf (timeline.view), snapshotOf (*fresh), area, worst, at, tolerance);

            if (different > 0 && differing++ == 0)
                firstDifference = moment + ": " + juce::String (different) + " px differ, worst " + juce::String (worst) + ", first at ("
                                + juce::String (at.x) + ", " + juce::String (at.y) + ")";
        }
    };

    // S12a：6 kHz −12 dBFS 正弦 2 s → 数字静音。停播后 2 s、3 s、4 s（曲线都静止；PEAK 开着时只有峰值保持在落，视图每帧重建）各比一次，
    // 再把斜率换成 4.5（点击的重画先到、下一帧还没来：路径在 paint 里重建）立刻比，之后每帧比，共 10 帧；最后鼠标悬停在 6 kHz 再比一次。
    FreshEquality runFreshAfterStop (bool peakOn)
    {
        FreshEquality result;
        SpectrumTimeline timeline (peakOn, false, 0);

        if (! timeline.valid)
        {
            result.error = "legend toggles or the slope control not found";
            return result;
        }

        const auto second = SpectrumTimeline::framesPerSecond;
        using Sound = SpectrumTimeline::Sound;
        timeline.steps (2 * second, Sound::sine);
        timeline.steps (2 * second, Sound::silence);
        result.compare (timeline, "4 s, slope 0");
        timeline.steps (second, Sound::silence);
        result.compare (timeline, "5 s, slope 0");
        timeline.steps (second, Sound::silence);
        result.compare (timeline, "6 s, slope 0");
        timeline.changeSlope (2);
        result.compare (timeline, "6 s, slope 0 -> 4.5 before the next frame");

        for (int i = 1; i <= 10; ++i)
        {
            timeline.step (Sound::silence);
            result.compare (timeline, "6 s + " + juce::String (i) + " frame(s), slope 4.5");
        }

        timeline.hover (6000.0f);
        result.compare (timeline, "6 s + 10 frames, slope 4.5, hover at 6 kHz");
        return result;
    }

    // S12b：IN 开着（评审 F4）：6 kHz −12 dBFS 稳态正弦 10 s（曲线静止）→ 斜率 0 → 3 → 4.5，每次立刻比、再推几帧比。
    // 曲线静止时换斜率：虚线靠「重建时作废展开好的那份」才会按新斜率画，视图对它自己的整帧重画抓不到这个（两边一样旧）。
    // 稳态那一刻本身不比：视图只在累计漂移超过 0.1 px 时才重建，画出来的与此刻的差不到 0.1 px（合法）；虚线的相位沿路径累计，
    // 这点差到尖峰处会放大成半个像素的相位差（实测 134 个像素差 40 级以上），与全新视图逐字节比不得。换斜率时视图用此刻的值整组重建，
    // 与全新视图逐位相同——要比的就是这些时刻。
    FreshEquality runFreshSteadyInput (bool peakOn)
    {
        FreshEquality result;
        SpectrumTimeline timeline (peakOn, true, 0);

        if (! timeline.valid)
        {
            result.error = "legend toggles or the slope control not found";
            return result;
        }

        timeline.steps (10 * SpectrumTimeline::framesPerSecond, SpectrumTimeline::Sound::sine);

        for (const auto slopeIndex : { 1, 2 })
        {
            timeline.changeSlope (slopeIndex);
            result.compare (timeline, "slope -> " + juce::String (spectrumSlopes[slopeIndex]) + " before the next frame");

            for (int i = 1; i <= 5; ++i)
            {
                timeline.step (SpectrumTimeline::Sound::sine);
                result.compare (timeline, "slope " + juce::String (spectrumSlopes[slopeIndex]) + " + " + juce::String (i) + " frame(s)");
            }
        }

        return result;
    }

    // S12c（评审 F3、F8）：白噪声 2 s → 数字静音，斜率 4.5，PEAK 关、IN 关。纯音的 OUT 停播后一两百毫秒就落到底边以下、平均线沿频率平滑后一直在面板外，
    // 「看没看变化」少比 OUT 或平均线、不加斜率比，纯音全看不出来（停播后视图从没重建过，画的恰好也是底线）；宽带噪声 OUT 与平均线都在面板里，
    // 斜率 4.5 把高频的曲线抬高，落到底边附近时还看得见。停播前后若干时刻与全新视图比（噪声每帧都不同，视图每帧重建；之后落得慢，滞后不到 0.1 px，
    // 容差 24 级，理由见 FreshEquality::compare）。
    FreshEquality runFreshDecay()
    {
        constexpr int tolerance = 24;
        FreshEquality result;
        SpectrumTimeline timeline (false, false, 2);

        if (! timeline.valid)
        {
            result.error = "legend toggles or the slope control not found";
            return result;
        }

        using Sound = SpectrumTimeline::Sound;
        const auto second = SpectrumTimeline::framesPerSecond;
        timeline.steps (second, Sound::noise);
        result.compare (timeline, "noise 1 s", tolerance, true);
        timeline.steps (second, Sound::noise);
        result.compare (timeline, "noise 2 s", tolerance, true);

        double stopped = 0.0;   // 停播后的秒数

        for (const auto until : { 0.2, 0.5, 1.0, 2.0, 3.0, 5.0 })
        {
            timeline.steps ((int) std::lround ((until - stopped) * second), Sound::silence);
            stopped = until;
            result.compare (timeline, juce::String (until, 1) + " s after the noise stopped", tolerance, true);
        }

        return result;
    }

    // S12d（评审 F3）：稳态正弦、PEAK 关、IN 关：OUT 在 6 kHz 有一根尖峰、平均线在面板外。「看没看变化」少比 OUT 时视图从没重建过、画的是底线，
    // 与全新视图的尖峰差几百个像素；S12a / S12c 的比较点都在 OUT 已经落到底边（或每帧都因别的曲线在变而重建）之后，看不出。
    // 容差与「只比读数框以下」同 S12c（实线的 0.1 px 滞后只差十几级）。
    FreshEquality runFreshSteady (int slopeIndex)
    {
        constexpr int tolerance = 24;
        FreshEquality result;
        SpectrumTimeline timeline (false, false, slopeIndex);

        if (! timeline.valid)
        {
            result.error = "legend toggles or the slope control not found";
            return result;
        }

        const auto second = SpectrumTimeline::framesPerSecond;
        double elapsed = 0.0;

        for (const auto until : { 0.5, 1.0, 2.0, 4.0 })
        {
            timeline.steps ((int) std::lround ((until - elapsed) * second), SpectrumTimeline::Sound::sine);
            elapsed = until;
            result.compare (timeline, juce::String (until, 1) + " s into the steady sine", tolerance, true);
        }

        return result;
    }

    struct ProbeResult
    {
        bool ok = false;
        juce::String detail;
    };

    // 一条曲线上最响的点：下标、dB、在面板里的位置（斜率 0：纵坐标不加斜率）。
    struct CurvePoint
    {
        int index = 0;
        float db = 0.0f;
        juce::Point<int> at;
    };

    CurvePoint loudestPoint (const std::vector<float>& values)
    {
        CurvePoint point;
        point.index = (int) (std::max_element (values.begin(), values.end()) - values.begin());
        point.db = values[(size_t) point.index];
        point.at = { (int) std::lround (celadon::ui::SpectrumView::xOf (celadon::ui::SpectrumAnalyser::frequencyAt (point.index))),
                     (int) std::lround (celadon::ui::SpectrumView::yOf (point.db)) };
        return point;
    }

    // 视图与同开关的空视图（从没喂过数据）在 box 里不同的像素数。
    int differingFromEmpty (SpectrumTimeline& timeline, juce::Rectangle<int> box)
    {
        const auto empty = timeline.emptyView();
        int worst = 0;
        juce::Point<int> at;
        return countDifferences (snapshotOf (timeline.view), snapshotOf (*empty), box.getIntersection (timeline.view.getLocalBounds()), worst, at);
    }

    // OUT 的填充：1.5 s 稳态正弦，PEAK 关、IN 关、斜率 0；最响那个点正下方 11 × 50 个像素里至少 100 个与空视图不同（只有 OUT 画填充；
    // 填充带竖纹与按响度的渐变，实测约 190 / 550，OUT 整条不画时只剩平均线的零星几个像素）。
    ProbeResult probeOutputFill()
    {
        using Stream = celadon::ui::SpectrumAnalyser::Stream;
        SpectrumTimeline timeline (false, false, 0);

        if (! timeline.valid)
            return { false, "legend toggles or the slope control not found" };

        timeline.steps (90, SpectrumTimeline::Sound::sine);
        const auto top = loudestPoint (timeline.analyser.getDisplayDb (Stream::output));
        const auto differing = differingFromEmpty (timeline, { top.at.x - 5, top.at.y + 10, 11, 50 });
        return { differing >= 100, juce::String (differing) + " of 550 px under the OUT peak (x " + juce::String (top.at.x) + ", y "
                                       + juce::String (top.at.y) + ") differ from an empty view, needs >= 100" };
    }

    // 平均线：白噪声 2 s → 静音（纯音的平均线沿频率平滑后只剩 −107 dB 的一个小包，在面板底边 −84 dB 之下，要宽带内容才看得见），找一个 OUT 已经落到
    // 面板底边以下、平均线还在面板里（高出底边 15 dB 以上）的时刻；平均线最响那个点的 13 × 7 个像素里至少 5 个与空视图不同（PEAK 关、没有填充，
    // 只有平均线能画在那里）。找不到这样的时刻 = 探针空转，也算失败。
    ProbeResult probeAverage()
    {
        using Stream = celadon::ui::SpectrumAnalyser::Stream;
        SpectrumTimeline timeline (false, false, 0);

        if (! timeline.valid)
            return { false, "legend toggles or the slope control not found" };

        timeline.steps (120, SpectrumTimeline::Sound::noise);

        for (int i = 0; i < 240; ++i)
        {
            timeline.step (SpectrumTimeline::Sound::silence);
            const auto& output = timeline.analyser.getDisplayDb (Stream::output);
            const auto& average = timeline.analyser.getAverageDb();
            const auto outputMax = *std::max_element (output.begin(), output.end());
            const auto averageMax = *std::max_element (average.begin(), average.end());

            if (outputMax > celadon::ui::SpectrumView::bottomDb - 3.0f || averageMax < celadon::ui::SpectrumView::bottomDb + 15.0f)
                continue;

            const auto top = loudestPoint (average);
            const auto differing = differingFromEmpty (timeline, { top.at.x - 6, top.at.y - 3, 13, 7 });
            return { differing >= 5, juce::String (differing) + " of 91 px at the average line's peak (x " + juce::String (top.at.x) + ", y "
                                           + juce::String (top.at.y) + ", " + juce::String (averageMax, 1) + " dB, " + juce::String (i + 1)
                                           + " frames after the sound stopped, OUT already below the panel) differ from an empty view, needs >= 5" };
        }

        return { false, "no moment found where the OUT curve is below the panel while the average line is still inside it" };
    }

    // 峰值线：6 kHz 正弦 2 s → 静音 2 s（PEAK 开）；OUT 与平均线都在面板底边以下、峰值线还在面板里时，峰值线最响那个点的 13 × 7 个
    // 像素里至少 5 个与空视图不同。
    ProbeResult probePeak()
    {
        using Stream = celadon::ui::SpectrumAnalyser::Stream;
        SpectrumTimeline timeline (true, false, 0);

        if (! timeline.valid)
            return { false, "legend toggles or the slope control not found" };

        timeline.steps (120, SpectrumTimeline::Sound::sine);
        timeline.steps (120, SpectrumTimeline::Sound::silence);
        const auto& output = timeline.analyser.getDisplayDb (Stream::output);
        const auto& average = timeline.analyser.getAverageDb();
        const auto& peak = timeline.analyser.getPeakDb();
        const auto peakMax = *std::max_element (peak.begin(), peak.end());

        if (*std::max_element (output.begin(), output.end()) > celadon::ui::SpectrumView::bottomDb - 3.0f
            || *std::max_element (average.begin(), average.end()) > peakMax - 10.0f || peakMax < celadon::ui::SpectrumView::bottomDb + 15.0f)
            return { false, "the OUT curve and the average line are not both well below the peak hold line any more (probe is vacuous)" };

        const auto top = loudestPoint (peak);
        const auto differing = differingFromEmpty (timeline, { top.at.x - 6, top.at.y - 3, 13, 7 });
        return { differing >= 5, juce::String (differing) + " of 91 px at the peak hold line's peak (x " + juce::String (top.at.x) + ", y "
                                       + juce::String (top.at.y) + ", " + juce::String (peakMax, 1) + " dB) differ from an empty view, needs >= 5" };
    }

    // IN 虚线：两条同样的时间线（PEAK 关、斜率 0、1.5 s 稳态正弦），一个 IN 开、一个关；IN 最响那个点的 13 × 7 个像素里至少 3 个不同
    // （OUT 与 IN 送的是同一份音频，OUT 线两边相同，差别只能是 IN 的虚线）。
    ProbeResult probeInput()
    {
        using Stream = celadon::ui::SpectrumAnalyser::Stream;
        SpectrumTimeline withInput (false, true, 0), withoutInput (false, false, 0);

        if (! withInput.valid || ! withoutInput.valid)
            return { false, "legend toggles or the slope control not found" };

        withInput.steps (90, SpectrumTimeline::Sound::sine);
        withoutInput.steps (90, SpectrumTimeline::Sound::sine);
        const auto top = loudestPoint (withInput.analyser.getDisplayDb (Stream::input));
        int worst = 0;
        juce::Point<int> at;
        const auto differing = countDifferences (snapshotOf (withInput.view), snapshotOf (withoutInput.view),
                                                 juce::Rectangle<int> (top.at.x - 6, top.at.y - 3, 13, 7).getIntersection (withInput.view.getLocalBounds()),
                                                 worst, at);
        return { differing >= 3, juce::String (differing) + " of 91 px at the IN curve's peak (x " + juce::String (top.at.x) + ", y "
                                     + juce::String (top.at.y) + ") differ from the same view with IN off, needs >= 3" };
    }

    // 两点之间按对数频率插值（与 SpectrumView::curveDbAt 同一算式，探针独立再算一份）。
    float interpolatedDb (const std::vector<float>& values, float frequency)
    {
        const auto position = juce::jlimit (0.0f, 1.0f, celadon::ui::SpectrumAnalyser::positionOf (frequency))
                            * (float) (celadon::ui::SpectrumAnalyser::numPoints - 1);
        const auto below = juce::jlimit (0, celadon::ui::SpectrumAnalyser::numPoints - 1, (int) std::floor (position));
        const auto above = juce::jmin (celadon::ui::SpectrumAnalyser::numPoints - 1, below + 1);
        return values[(size_t) below] + (values[(size_t) above] - values[(size_t) below]) * (position - (float) below);
    }

    // 悬停的读数：6 kHz 正弦 2 s → 停播 0.15 s（OUT 在落、平均线与峰值线还高：三条线在 6 kHz 各是各的值，读错来源会差好几 dB），
    // 鼠标移到 6 kHz；读数框第一个数必须是计算器此刻 OUT 在 6 kHz 的值（加斜率），误差在两位小数的舍入以内。
    ProbeResult probeHoverReadout (int slopeIndex)
    {
        using Stream = celadon::ui::SpectrumAnalyser::Stream;
        SpectrumTimeline timeline (false, false, slopeIndex);

        if (! timeline.valid)
            return { false, "legend toggles or the slope control not found" };

        timeline.steps (120, SpectrumTimeline::Sound::sine);
        timeline.steps (9, SpectrumTimeline::Sound::silence);
        timeline.hover (6000.0f);

        const auto tilt = spectrumSlopes[slopeIndex] * std::log2 (6000.0f / 1000.0f);
        const auto expected = interpolatedDb (timeline.analyser.getDisplayDb (Stream::output), 6000.0f) + tilt;
        const auto fromAverage = interpolatedDb (timeline.analyser.getAverageDb(), 6000.0f) + tilt;
        const auto fromPeak = interpolatedDb (timeline.analyser.getPeakDb(), 6000.0f) + tilt;
        const auto text = timeline.view.getReadoutText();
        const auto shown = text.upToFirstOccurrenceOf ("dB", false, false).replace (juce::String::charToString ((juce::juce_wchar) 0x2212), "-").getFloatValue();
        const auto detail = "readout \"" + text + "\" vs OUT " + juce::String (expected, 2) + " dB (the average line would read " + juce::String (fromAverage, 2)
                          + ", the peak hold " + juce::String (fromPeak, 2) + ")";

        if (std::abs (expected - fromAverage) < 0.5f || std::abs (expected - fromPeak) < 0.5f)
            return { false, "probe is vacuous: OUT, average and peak read nearly the same at that moment (" + detail + ")" };

        return { std::abs (shown - expected) <= 0.011f, detail };
    }

    // ---- T-072（T-070 评审 F9）：截图工具驱动的编辑器，定时器的看门狗不按墙钟补推频谱仪 ----
    // 编辑器平时由垂直同步推频谱仪，垂直同步 100 ms 没来就由 30 Hz 定时器接着推；离屏没有垂直同步，截图工具用 advanceForSnapshot 自己按给定时长推。
    // 改前只靠「把最近一次垂直同步记成现在」让看门狗以为垂直同步还活着：最后一次 advanceForSnapshot 之后过 100 ms，或那一帧自己耗时 ≥ 100 ms
    // （慢机、Debug、机器被占满），看门狗就按墙钟再推一次，OUT / 峰值保持按墙钟落，出图随机器快慢变。这里：6 kHz −12 dBFS 正弦 0.5 s（每帧 1 / 60 s
    // 音频 + advanceForSnapshot），最后一帧之后让消息循环跑 300 ms（定时器约触发 9 次，每次离最近一帧都 ≥ 100 ms）：频谱仪的路径重建次数不得再增加
    // ——离屏没有对等体、没有垂直同步回调，能推频谱仪的只剩看门狗。改前（main 43d788b 的编辑器）这 300 ms 里看门狗按墙钟推、OUT 往下落，实测重建 2 次。
    ProbeResult probeTimerPush()
    {
        SpectrumRig rig (0.0f);

        if (! rig.isValid())
            return { false, "spectrum view, its legend toggles or the slope control not found" };

        rig.attach();   // 编辑器设成可见（挂记账缓存图）：定时器照常跑
        SineSource sine (6000.0, -12.0);

        for (int i = 0; i < 30; ++i)
        {
            rig.feed (rig.samplesForFrame (60), std::ref (sine));
            rig.advance (1.0 / 60.0);
        }

        const auto before = rig.view->getCurveBuildCount();
        dispatchEvents (300);
        const auto rebuilds = rig.view->getCurveBuildCount() - before;
        return { rebuilds == 0, juce::String ((int) rebuilds) + " path rebuilds in 300 ms of message loop after the last advanceForSnapshot, needs 0" };
    }

    // ---- T-073（T-070 评审 SV-2）：PEAK 关着时看不见的峰值线不进脏区 ----
    // curveRegion（增量重画按它标脏）原来不论 PEAK 开关都算峰值线（R-02 ②）：峰值保持常比 OUT 顶高几 dB，PEAK 关着放音乐时脏区白白多 11 ～ 12 %。
    // 现在只在 PEAK 开着时算。两个时间轴喂同一段声音（6 kHz −12 dBFS 正弦 1 s → 静音 0.2 s：OUT 与平均线已落下去，峰值保持 1.5 s 内还在
    // 顶上），PEAK 开的区域顶必须比 PEAK 关的高出 20 px 以上。改回「不论开关都算」→ 两个区域一样高（差 0）；改成「永远不算」→ 开着时也不含
    // 峰值线、同样差 0。两个方向都抓。
    ProbeResult probeRegionWithoutPeak()
    {
        SpectrumTimeline withPeak (true, false, 0), withoutPeak (false, false, 0);

        if (! withPeak.valid || ! withoutPeak.valid)
            return { false, "legend toggles or the slope control not found" };

        for (auto* timeline : { &withPeak, &withoutPeak })
        {
            timeline->steps (60, SpectrumTimeline::Sound::sine);
            timeline->steps (12, SpectrumTimeline::Sound::silence);
        }

        const auto topWith = withPeak.view.getCurveRegion().getY();
        const auto topWithout = withoutPeak.view.getCurveRegion().getY();
        const auto gap = topWithout - topWith;
        return { gap >= 20.0f, "curve region top y = " + juce::String (topWithout, 1) + " with the peak line off, " + juce::String (topWith, 1)
                                   + " with it on: the peak hold sits " + juce::String (gap, 1) + " px above the other curves, needs >= 20 px" };
    }

    void runSpectrumChecks (const std::function<void (bool, const juce::String&)>& check)
    {
        using namespace celadon::ui;

        // IN 虚线：随机折线（频谱仪那样每逻辑像素一个点、x 从 0 到 1032）——每点乱跳、缓变、贴顶 / 贴底平直、顶底交替、带重复点，
        //     同一组 centre / dest 反复用（第二次起不再分配）；与 JUCE createDashedStroke 的结果逐元素逐位比。
        {
            const float dashes[] = { 3.0f, 2.0f };
            const juce::PathStrokeType stroke (1.1f);
            juce::Random random (20261004);
            juce::Path line, centre, reused, fresh;
            int cases = 0, mismatches = 0;

            const auto test = [&] (const std::function<float (int)>& yAt, bool repeatPoints)
            {
                line.clear();

                for (int i = 0; i < SpectrumAnalyser::numPoints; ++i)
                {
                    const juce::Point<float> point ((float) i / (float) (SpectrumAnalyser::numPoints - 1) * (float) SpectrumView::width, yAt (i));

                    if (i == 0)
                        line.startNewSubPath (point);
                    else
                        line.lineTo (point);

                    if (repeatPoints && i % 7 == 3)
                        line.lineTo (point);
                }

                createDashedStrokeReusing (stroke, line, dashes, 2, centre, reused);
                fresh.clear();
                stroke.createDashedStroke (fresh, line, dashes, 2);
                ++cases;
                mismatches += samePathBits (reused, fresh) ? 0 : 1;
            };

            const auto h = (float) SpectrumView::height;

            for (int n = 0; n < 40; ++n)
                test ([&] (int) { return random.nextFloat() * h; }, n % 4 == 0);

            for (int n = 0; n < 40; ++n)
            {
                const auto rate = 0.002f + 0.1f * random.nextFloat(), offset = random.nextFloat() * 6.0f, depth = random.nextFloat() * 0.5f * h;
                test ([=] (int i) { return 0.5f * h + depth * std::sin ((float) i * rate + offset); }, n % 4 == 1);
            }

            test ([] (int) { return 0.0f; }, false);
            test ([h] (int) { return h; }, false);
            test ([h] (int i) { return (i % 2 == 0) ? 0.0f : h; }, false);
            test ([h] (int i) { return (i / 3) % 2 == 0 ? 0.25f * h : 0.75f * h; }, true);

            check (mismatches == 0, "T-051 IN dashes: createDashedStrokeReusing matches juce::PathStrokeType::createDashedStroke bit for bit ("
                                        + juce::String (cases - mismatches) + " / " + juce::String (cases) + " polylines)");
        }

        // 增量重画 vs 整帧（场景表见 dirtyScenarios）：S8（T-051）、S9 与播放（T-058）。
        for (const auto& scenario : dirtyScenarios())
            scenario.report (runDirtyScenario (scenario), check);

        // 打开 PEAK、换斜率、悬停那一刻画的是此刻的数据（T-058 的 S10，见 runPeakFreshness）：a 只点开 PEAK（守打开时的重建与 PEAK 关时
        // 每帧拷峰值），b 同时把斜率 0 换成 4.5（守任何重建都先换成此刻的值：曲线静止时换斜率，旧 OUT 不被抬出来；换斜率的读数），
        // c 鼠标移到 6 kHz（守悬停的读数用此刻的值）。
        for (const auto which : { FreshnessCase::peakOn, FreshnessCase::slopeAndPeakOn, FreshnessCase::hoverReadout })
        {
            const auto result = runPeakFreshness (which);
            juce::String detail;

            if (result.error.isNotEmpty())
                detail = result.error;
            else
                detail = (result.switchDifferent == 0 ? juce::String ("identical") : juce::String (result.switchDifferent) + " px differ")
                       + " at that moment, " + juce::String (result.laterFramesDifferent) + " of 30 later frames differ"
                       + (result.laterFramesDifferent > 0 ? ", first at frame " + juce::String (result.firstLaterFrame) : juce::String())
                       + (result.worst > 0 ? ", worst " + juce::String (result.worst) + ", first at (" + juce::String (result.firstAt.x) + ", "
                                                 + juce::String (result.firstAt.y) + ")"
                                           : juce::String());

            const auto setting = " at 6 s after 6 kHz -12 dBFS sine 2 s + silence 4 s, vs a view with the peak line always on: " + detail + ")";
            check (result.error.isEmpty() && result.switchDifferent == 0 && result.laterFramesDifferent == 0,
                   which == FreshnessCase::peakOn
                       ? "T-058 spectrum: switching the peak line on draws this moment's data (S10a: slope 0, peak on" + setting
                       : which == FreshnessCase::slopeAndPeakOn
                             ? "T-058 spectrum: changing the slope while the curves are still draws this moment's data (S10b: slope 0 -> 4.5 and peak on" + setting
                             : "T-058 spectrum: the hover readout shows this moment's value (S10c: peak off, slope 0, mouse to 6 kHz, readout area only" + setting);
        }

        // S12（T-070）：长期运行的视图 == 没有历史的全新视图（停播后、稳态 IN 换斜率；PEAK 关 / 开各一遍）。
        const auto reportFresh = [&check] (const FreshEquality& result, const juce::String& what)
        {
            check (result.error.isEmpty() && result.differing == 0,
                   "T-070 spectrum: a long-running view draws what a view built from scratch draws (" + what + ": "
                       + (result.error.isNotEmpty() ? result.error
                                                    : juce::String (result.moments - result.differing) + " / " + juce::String (result.moments)
                                                          + " moments identical" + (result.differing > 0 ? "; first: " + result.firstDifference : juce::String()))
                       + ")");
        };

        reportFresh (runFreshAfterStop (false), "S12a: 6 kHz -12 dBFS sine 2 s, silence, peak off, slope 0 -> 4.5, hover");
        reportFresh (runFreshAfterStop (true), "S12a: 6 kHz -12 dBFS sine 2 s, silence, peak on, slope 0 -> 4.5, hover");
        reportFresh (runFreshSteadyInput (false), "S12b: IN on, peak off, 6 kHz -12 dBFS sine 10 s steady, slope 0 -> 3 -> 4.5");
        reportFresh (runFreshSteadyInput (true), "S12b: IN on, peak on, 6 kHz -12 dBFS sine 10 s steady, slope 0 -> 3 -> 4.5");
        reportFresh (runFreshDecay(), "S12c: white noise 2 s, silence, peak off, slope 4.5, tolerance 24 levels");
        reportFresh (runFreshSteady (0), "S12d: 6 kHz -12 dBFS sine steady, peak off, slope 0, tolerance 24 levels");
        reportFresh (runFreshSteady (2), "S12d: 6 kHz -12 dBFS sine steady, peak off, slope 4.5, tolerance 24 levels");

        // 绝对探针（T-070）：曲线 / 读数真的画在计算器给的位置，不靠任何历史。
        const auto reportProbe = [&check] (const ProbeResult& result, const juce::String& what)
        {
            check (result.ok, "T-070 spectrum draws the data: " + what + " (" + result.detail + ")");
        };

        reportProbe (probeOutputFill(), "S12 probe OUT fill");
        reportProbe (probeAverage(), "S12 probe average line");
        reportProbe (probePeak(), "S12 probe peak hold line");
        reportProbe (probeInput(), "S12 probe IN dashes");
        reportProbe (probeHoverReadout (0), "S12 probe hover readout, slope 0");
        reportProbe (probeHoverReadout (2), "S12 probe hover readout, slope 4.5");

        // 截图模式下定时器的看门狗不补推（T-072，T-070 评审 F9，见 probeTimerPush）。
        const auto timerPush = probeTimerPush();
        check (timerPush.ok, "T-072 spectrum: an editor driven by advanceForSnapshot is not advanced by its own timer (6 kHz -12 dBFS sine 0.5 s, then 300 ms of message loop: "
                                 + timerPush.detail + ")");

        // PEAK 关着时看不见的峰值线不进脏区、开着时进（T-073，T-070 评审 SV-2，见 probeRegionWithoutPeak）。
        const auto regionWithoutPeak = probeRegionWithoutPeak();
        check (regionWithoutPeak.ok, "T-073 spectrum: the dirty curve region leaves out the peak line while PEAK is off and includes it while on (6 kHz -12 dBFS sine 1 s, silence 0.2 s: "
                                         + regionWithoutPeak.detail + ")");
    }
}

//==============================================================================
// 性能基准（--only bench，T-032 R9-01 / 02 / 05 / 06 / 07 / 08 / 12 / 13 / 14 的改前改后对照）。
// 离屏没有窗口系统的脏矩形，这里按两种口径各量一遍：
//   full = 整个编辑器重画（宿主首帧、缩放后的那一帧）；
//   band = 只把粒子带（画布 y 300 ～ 560 的整条）标脏——macOS 的 CoreGraphics 只拿脏矩形的包围盒，粒子每帧 repaint
//          在 Mac 上就是这一条整带，带里的主旋钮、标题、按钮、图例都要跟着重画。
// 每项给墙钟与本线程 CPU 时间两个数（机器与别的任务共用时墙钟会被抢占拉长，CPU 时间不受影响）。另外给主旋钮、粒子、
// 中层文字部件与顶栏状态胶囊各挂一个 ComponentListener（JUCE 9 的 ComponentPaintDiagnostics，墙钟），累计各自的 paint 时长
// 与「从缓存读」的次数。数字是软件渲染器的（Linux / Windows 非 Direct2D 的路径），Mac 的绝对值需 Max 用活动监视器实测。
namespace
{
    // 本线程的 CPU 时间（毫秒）；没有对应接口的平台退回墙钟。
    double threadCpuMs() noexcept
    {
       #if JUCE_WINDOWS
        return juce::Time::getMillisecondCounterHiRes();
       #else
        timespec now {};
        clock_gettime (CLOCK_THREAD_CPUTIME_ID, &now);
        return (double) now.tv_sec * 1000.0 + (double) now.tv_nsec * 1.0e-6;
       #endif
    }

    struct Clock
    {
        double wall = 0.0, cpu = 0.0;

        static Clock now() noexcept                        { return { juce::Time::getMillisecondCounterHiRes(), threadCpuMs() }; }
        Clock elapsed() const noexcept                     { const auto n = now(); return { n.wall - wall, n.cpu - cpu }; }
        Clock& operator+= (const Clock& other) noexcept    { wall += other.wall; cpu += other.cpu; return *this; }
        Clock per (int count) const noexcept               { return { wall / juce::jmax (1, count), cpu / juce::jmax (1, count) }; }
        juce::String text() const                          { return ms (wall) + " ms (cpu " + ms (cpu) + ")"; }
    };

    // 画一帧（同 paintFrame），返回墙钟与 CPU 时间。
    Clock paintTimed (juce::Component& editor, juce::Image& target, float scale, std::optional<juce::Rectangle<int>> clipLogical)
    {
        juce::Graphics g (target);

        if (! juce::approximatelyEqual (scale, 1.0f))
            g.addTransform (juce::AffineTransform::scale (scale));

        if (clipLogical.has_value())
            g.reduceClipRegion (*clipLogical);

        const auto started = Clock::now();
        editor.paintEntireComponent (g, true);
        return started.elapsed();
    }

    juce::Image frameFor (juce::Component& editor, float scale)
    {
        return { juce::Image::RGB, juce::roundToInt ((float) editor.getWidth() * scale), juce::roundToInt ((float) editor.getHeight() * scale), false };
    }

    void runBench (int frames)
    {
        std::cout << "==== bench: frames=" << frames << " (software renderer; wall and thread-CPU ms; Mac absolute numbers need Max)" << std::endl;

        // ---- 1. 瓷面生成：拖动 GLAZE 时每次重新生成的开销，各取 5 次最短 ----
        //   renderGlaze = 原算法（逐像素全量，T-032 前主旋钮每次都走它）；layered = GlazeRenderer（R9-02 分层，界面现在用它）。
        celadon::ui::porcelain::GlazeRenderer renderer;

        const auto timeGlaze = [&renderer] (float t, float pixelScale, bool desaturated, bool layered)
        {
            Clock best { 1.0e9, 1.0e9 };

            for (int repeat = 0; repeat < 5; ++repeat)
            {
                const auto started = Clock::now();
                const auto image = layered ? renderer.render (t, pixelScale, desaturated) : celadon::ui::porcelain::renderGlaze (t, pixelScale, desaturated);
                const auto took = started.elapsed();
                juce::ignoreUnused (image);
                best = { juce::jmin (best.wall, took.wall), juce::jmin (best.cpu, took.cpu) };
            }

            return best;
        };

        for (const auto pixelScale : { 1.0f, 2.0f, 4.0f })
        {
            // 分层的第一次（这个像素比下建底层逐像素量、两张覆盖层与裂纹轮廓）单独量一次。
            const auto firstStarted = Clock::now();
            juce::ignoreUnused (renderer.render (0.45f, pixelScale, false));
            std::cout << "porcelain layered first render (builds layers)  scale=" << pixelScale << "  " << firstStarted.elapsed().text() << std::endl;

            for (const auto t : { 0.2f, 0.45f, 0.9f })
                std::cout << "porcelain scale=" << pixelScale << " t=" << t << "  renderGlaze " << timeGlaze (t, pixelScale, false, false).text()
                          << "   layered " << timeGlaze (t, pixelScale, false, true).text() << std::endl;
        }

        std::cout << "porcelain scale=2 t=0.45 desaturated  renderGlaze " << timeGlaze (0.45f, 2.0f, true, false).text()
                  << "   layered " << timeGlaze (0.45f, 2.0f, true, true).text() << std::endl;

        // ---- 2. 打开编辑器与缩放（R9-08）：静态背景在消息线程同步生成。----
        //   打开：新建编辑器后的第一帧 − 第二帧（子组件全藏起来量，只剩静态背景；改前改后同一口径）；
        //   缩放：同一个编辑器 1 → 2 倍，旧图先拉伸、像素比稳定 150 ms 后画布定时器重新生成——量那段消息循环的 CPU 时间，
        //         减去同样长的空转（改后面板阴影的模糊图沿用，不再重做）；改后另有 renderStaticLayer 自己记的耗时。
        const auto hideChildren = [] (juce::Component& editor)
        {
            for (auto* child : editor.getChildren())
            {
                if (child->getNumChildComponents() == 0)
                    child->setVisible (false);

                for (auto* grandChild : child->getChildren())
                    grandChild->setVisible (false);
            }
        };

        for (const auto pixelScale : { 1.0f, 2.0f })
        {
            auto session = makeSession (45.0f);
            auto& editor = *session->editor;
            hideChildren (editor);

            auto target = frameFor (editor, pixelScale);
            const auto first = paintTimed (editor, target, pixelScale, std::nullopt);
            const auto second = paintTimed (editor, target, pixelScale, std::nullopt);
            std::cout << "static layer, editor opened at scale=" << pixelScale << ": first frame " << first.text() << ", second " << second.text()
                      << " -> generating it " << Clock { first.wall - second.wall, first.cpu - second.cpu }.text()
                      << "; renderStaticLayer's own timer " << ms (editor.getStaticLayerRenderMs()) << " ms" << std::endl;

            if (pixelScale < 2.0f)
            {
                dispatchEvents (400);
                auto zoomed = frameFor (editor, 2.0f);
                paintTimed (editor, zoomed, 2.0f, std::nullopt);   // 像素比变了：先拉伸旧图，150 ms 后重生成
                const auto busyStarted = Clock::now();
                dispatchEvents (600);
                const auto busy = busyStarted.elapsed();
                const auto idleStarted = Clock::now();
                dispatchEvents (600);
                const auto idle = idleStarted.elapsed();
                std::cout << "static layer, zoom 1 -> 2 in the same editor: message thread cpu " << ms (busy.cpu) << " ms vs idle " << ms (idle.cpu)
                          << " ms -> regenerating it ~" << ms (busy.cpu - idle.cpu) << " ms cpu; renderStaticLayer's own timer "
                          << ms (editor.getStaticLayerRenderMs()) << " ms" << std::endl;
            }

            session->editor.reset();
            session->processor->releaseResources();
        }

        // 打开编辑器的第一帧（全部组件：静态背景、第一次瓷面、各缓存图）与第二帧。
        for (const auto pixelScale : { 1.0f, 2.0f })
        {
            auto session = makeSession (45.0f);
            session->play (0.5);
            auto& editor = *session->editor;
            auto target = frameFor (editor, pixelScale);
            const auto first = paintTimed (editor, target, pixelScale, std::nullopt);
            const auto second = paintTimed (editor, target, pixelScale, std::nullopt);
            std::cout << "editor opened at scale=" << pixelScale << ": first full frame " << first.text() << ", second " << second.text() << std::endl;
            session->editor.reset();
            session->processor->releaseResources();
        }

        // ---- 3. 每帧口径：GLAZE 45 %、音乐在放（粒子在动）----
        auto session = makeSession (45.0f);
        session->play (3.0);

        auto& editor = *session->editor;
        std::vector<juce::Component*> all;
        collectDescendants (editor, all);

        std::vector<std::unique_ptr<PaintProbe>> probes;
        std::vector<juce::Component*> probed;

        const auto probe = [&] (juce::Component* component, const juce::String& name)
        {
            auto p = std::make_unique<PaintProbe>();
            p->name = name;
            component->addComponentListener (p.get());
            probes.push_back (std::move (p));
            probed.push_back (component);
        };

        for (auto* component : all)
        {
            if (dynamic_cast<celadon::ui::ParticleField*> (component) != nullptr)        probe (component, "ParticleField");
            else if (dynamic_cast<celadon::ui::GlazeKnob*> (component) != nullptr)       probe (component, "GlazeKnob");
            else if (dynamic_cast<celadon::ui::GlazeTitle*> (component) != nullptr)      probe (component, "GlazeTitle");
            else if (dynamic_cast<celadon::ui::CornerNote*> (component) != nullptr)      probe (component, "CornerNote");
            else if (dynamic_cast<celadon::ui::ColourLegend*> (component) != nullptr)    probe (component, "ColourLegend");
            else if (dynamic_cast<celadon::ui::StatusPill*> (component) != nullptr)      probe (component, "StatusPill");
            else if (component->getComponentID() == "match"
                     || component->getComponentID() == celadon::param::glazeStyle || component->getComponentID() == "ab"
                     || component->getComponentID() == "monitoring"
                     || component->getComponentID() == celadon::param::glazeGenre || component->getComponentID() == celadon::param::glazeDelivery)
                probe (component, component->getComponentID());
        }

        auto* knob = findFirst<celadon::ui::GlazeKnob> (editor);
        auto* field = findFirst<celadon::ui::ParticleField> (editor);
        celadon::ui::GlazeKnob::PaintStats knobBefore;
        celadon::ui::ParticleField::CacheStats fieldBefore;

        const auto startPhase = [&]
        {
            for (auto& p : probes)
                p->reset();

            knobBefore = knob->getPaintStats();
            fieldBefore = field->getCacheStats();
        };

        const auto report = [&] (const juce::String& phase, int count)
        {
            for (auto& p : probes)
            {
                std::cout << "  " << p->name.paddedRight (' ', 14) << " paints " << juce::String (p->paints).paddedLeft (' ', 5)
                          << "  total " << ms (p->totalMs).paddedLeft (' ', 9) << " ms  avg " << ms (p->paints > 0 ? p->totalMs / p->paints : 0.0)
                          << " ms  cacheReads " << p->cacheReads << "  cacheWrites " << p->cacheWrites << std::endl;
                p->reset();
            }

            const auto knobNow = knob->getPaintStats();
            const auto fieldNow = field->getCacheStats();
            std::cout << "  [" << phase << ", " << count << " frames] GlazeKnob face renders " << (knobNow.faceRenders - knobBefore.faceRenders)
                      << " of " << (knobNow.paints - knobBefore.paints) << " paints; porcelain renders " << (knobNow.porcelainRenders - knobBefore.porcelainRenders)
                      << " (layer builds " << (knobNow.porcelainLayerBuilds - knobBefore.porcelainLayerBuilds) << ", last " << ms (knobNow.lastPorcelainMs) << " ms)"
                      << "; ParticleField line / glow renders " << (fieldNow.lineRenders - fieldBefore.lineRenders) << " / "
                      << (fieldNow.glowRenders - fieldBefore.glowRenders) << std::endl;
        };

        const juce::Rectangle<int> band (0, celadon::ui::ParticleField::top, editor.getWidth(), celadon::ui::ParticleField::bandHeight);

        for (const auto pixelScale : { 1.0f, 2.0f, 4.0f })
        {
            const auto count = pixelScale > 2.0f ? juce::jmax (10, frames / 4) : frames;
            auto target = frameFor (editor, pixelScale);
            paintTimed (editor, target, pixelScale, std::nullopt);   // 暖机：这一比例的缓存先生成
            dispatchEvents (600);                                   // 让缩放后 150 ms 的静态底图 / 瓷面重生成先做完
            paintTimed (editor, target, pixelScale, std::nullopt);
            dispatchEvents (100);
            paintTimed (editor, target, pixelScale, std::nullopt);

            startPhase();
            Clock timer, full, bandOnly;

            for (int frame = 0; frame < count; ++frame)
            {
                const auto started = Clock::now();
                editor.advanceForSnapshot (1.0 / 30.0);   // 编辑器 30 Hz 定时器的一帧（R9-12）+ 粒子往前走一帧
                timer += started.elapsed();
                dispatchEvents (1);
                full += paintTimed (editor, target, pixelScale, std::nullopt);
            }

            std::cout << "---- steady GLAZE 45 %, playing, scale " << pixelScale << ": " << count << " frames" << std::endl;
            std::cout << "  editor timer callback avg " << timer.per (count).text() << std::endl;
            std::cout << "  full-frame paint      avg " << full.per (count).text() << std::endl;
            report ("full frames, scale " + juce::String (pixelScale), count);

            for (int frame = 0; frame < count; ++frame)
            {
                editor.advanceForSnapshot (1.0 / 30.0);
                dispatchEvents (1);
                bandOnly += paintTimed (editor, target, pixelScale, band);
            }

            std::cout << "  band-only paint       avg " << bandOnly.per (count).text() << "  (particle band dirty: macOS bounding-box case)" << std::endl;
            report ("band frames, scale " + juce::String (pixelScale), count);

            // 状态胶囊呼吸的一帧：改后只作废圆点那一块（缓存只重画那一块，顶栏、胶囊的 paint 都跳过与它不相交的文字）；
            // 改前呼吸每帧 repaint() 整个胶囊、整颗重画。另量两种整颗胶囊的重画：缓存没作废（macOS 的包围盒把胶囊圈进去，
            // 改后只贴图）与内容变了（整颗作废）。
            if (auto* pill = findFirst<celadon::ui::StatusPill> (editor))
            {
                const auto pulseArea = pill->getPulseArea();
                const auto pulse = editor.getLocalArea (pill, pulseArea);
                const auto whole = editor.getLocalArea (pill, pill->getLocalBounds());
                Clock pulseTotal, wholeTotal, changedTotal;

                for (int frame = 0; frame < count; ++frame)
                {
                    pill->repaint (pulseArea);
                    pulseTotal += paintTimed (editor, target, pixelScale, pulse);
                    wholeTotal += paintTimed (editor, target, pixelScale, whole);
                    pill->repaint();
                    changedTotal += paintTimed (editor, target, pixelScale, whole);
                }

                std::cout << "  status pill breathing frame (dirty " << pulse.toString() << ") avg " << pulseTotal.per (count).text()
                          << "; whole pill " << whole.toString() << " avg " << wholeTotal.per (count).text()
                          << "; whole pill after a content change avg " << changedTotal.per (count).text() << std::endl;

                for (auto& p : probes)
                    p->reset();
            }
        }

        // ---- 4. 拖动 GLAZE：35 % → 65 %，每帧走 33 ms 消息循环让旋钮的定时器按节流重生成瓷面 ----
        for (const auto pixelScale : { 2.0f, 4.0f })
        {
            auto target = frameFor (editor, pixelScale);
            paintTimed (editor, target, pixelScale, std::nullopt);
            dispatchEvents (600);
            paintTimed (editor, target, pixelScale, std::nullopt);

            constexpr int dragFrames = 60;
            Clock bandTotal;
            startPhase();
            const auto started = Clock::now();

            for (int frame = 0; frame < dragFrames; ++frame)
            {
                session->setParameter (celadon::param::glazeAmount, 35.0f + 30.0f * (float) frame / (float) (dragFrames - 1));
                editor.advanceForSnapshot (1.0 / 30.0);
                dispatchEvents (33);
                bandTotal += paintTimed (editor, target, pixelScale, band);
            }

            const auto wall = started.elapsed();
            std::cout << "---- drag GLAZE 35 -> 65 %, scale " << pixelScale << ": " << dragFrames << " frames" << std::endl;
            std::cout << "  band-only paint avg " << bandTotal.per (dragFrames).text() << std::endl;
            std::cout << "  message thread busy " << ms (wall.cpu) << " ms cpu over " << ms (wall.wall) << " ms wall ("
                      << ms (100.0 * wall.cpu / juce::jmax (1.0, wall.wall)) << " %; porcelain re-renders + paints + timer)" << std::endl;
            report ("drag, scale " + juce::String (pixelScale), dragFrames);

            session->setParameter (celadon::param::glazeAmount, 45.0f);
            dispatchEvents (300);
        }

        // ---- 5. 听原声（B）：主旋钮改前每次 paint 开透明图层，改后缓存图乘 0.55 贴 ----
        {
            const auto pixelScale = 2.0f;
            auto target = frameFor (editor, pixelScale);
            editor.setListeningToOriginal (true);
            session->play (1.0);
            paintTimed (editor, target, pixelScale, std::nullopt);
            dispatchEvents (600);
            paintTimed (editor, target, pixelScale, std::nullopt);

            startPhase();
            Clock bandTotal;

            for (int frame = 0; frame < frames; ++frame)
            {
                editor.advanceForSnapshot (1.0 / 30.0);
                dispatchEvents (1);
                bandTotal += paintTimed (editor, target, pixelScale, band);
            }

            std::cout << "---- comparing (B), scale 2: " << frames << " frames" << std::endl;
            std::cout << "  band-only paint avg " << bandTotal.per (frames).text() << std::endl;
            report ("comparing, scale 2", frames);
            editor.setListeningToOriginal (false);
        }

        for (size_t i = 0; i < probes.size(); ++i)
            probed[i]->removeComponentListener (probes[i].get());

        session->editor.reset();
        session->processor->releaseResources();
        std::cout << "==== bench done" << std::endl;
    }

    //==============================================================================
    // 频谱仪专项基准（--only spectrum-bench，T-036）：音乐按帧率在放，每帧先处理 1 / fps 秒的音频（每块 ≤ 512 个样本），
    // 再走 advanceForSnapshot (1 / fps)（取样口 → 计算器 → 视图，外加编辑器其余读数），然后只把频谱仪那一块标脏重画。
    // 只用改前改后都有的接口：同一份基准能在改前的树上量基线。每帧重画整块频谱仪是上限——真机上只重画曲线新旧包围盒，
    // 安静时更小。「每秒 CPU」= 帧率 × 每帧 CPU；advance 里含编辑器其余读数（真机上它们留在 30 Hz 定时器里），只作参考，
    // 频谱仪自己的开销看 paint 一项。软件渲染器口径，Mac 的绝对值要 Max 用活动监视器实测。
    void runSpectrumBench (int seconds)
    {
        std::cout << "==== spectrum bench: " << seconds << " s per case (software renderer; Mac absolute numbers need Max)" << std::endl;

        for (const auto fps : { 30, 60, 120 })
        {
            for (const auto pixelScale : { 1.0f, 2.0f })
            {
                auto session = makeSession (45.0f);
                session->play (3.0);
                auto& editor = *session->editor;
                auto* view = findFirst<celadon::ui::SpectrumView> (editor);

                if (view == nullptr)
                {
                    std::cout << "no SpectrumView in the editor" << std::endl;
                    return;
                }

                const auto area = editor.getLocalArea (view, view->getLocalBounds());
                auto target = frameFor (editor, pixelScale);
                paintTimed (editor, target, pixelScale, std::nullopt);   // 暖机：这一比例的缓存先生成
                dispatchEvents (600);
                paintTimed (editor, target, pixelScale, std::nullopt);

                const auto frames = fps * seconds;
                const auto samplesPerFrame = sampleRate / (double) fps;
                double owed = 0.0;
                Clock advance, paint;

                for (int frame = 0; frame < frames; ++frame)
                {
                    owed += samplesPerFrame;
                    auto due = (int) owed;
                    owed -= (double) due;

                    while (due > 0)
                    {
                        const auto n = juce::jmin (due, blockSize);
                        juce::AudioBuffer<float> chunk (session->buffer.getArrayOfWritePointers(), 2, n);
                        session->music.render (chunk, n);
                        session->processor->processBlock (chunk, session->midi);
                        due -= n;
                    }

                    const auto started = Clock::now();
                    editor.advanceForSnapshot (1.0 / (double) fps);
                    advance += started.elapsed();
                    paint += paintTimed (editor, target, pixelScale, area);
                }

                const auto advanceAvg = advance.per (frames), paintAvg = paint.per (frames);
                std::cout << "---- " << fps << " fps, scale " << pixelScale << ", " << frames << " frames: advance avg " << advanceAvg.text()
                          << "; spectrum paint avg " << paintAvg.text() << "; per second cpu: paint " << ms ((double) fps * paintAvg.cpu)
                          << " ms, advance " << ms ((double) fps * advanceAvg.cpu) << " ms" << std::endl;

                session->editor.reset();
                session->processor->releaseResources();
            }
        }

        std::cout << "==== spectrum bench done" << std::endl;
    }

    //==============================================================================
    // PEAK（峰值线）关时频谱仪多做的重建 / 重画（--only spectrum-peak-bench，T-058）：每帧照真实窗口走（SpectrumRig）——
    // 送 1 / fps 秒音频 → advanceForSnapshot (1 / fps) → 只按记下的脏矩形（矩形列表口径：Windows Direct2D、Linux）把频谱仪那一块
    // 画进一直留着的「屏幕」图。只用改前改后都有的接口：同一份基准能在改前的树上量基线、改后再量一遍逐行对照。
    //   S8：GLAZE 0、斜率 0，6 kHz −12 dBFS 正弦 12 s → 斜率换 4.5（先只重画一帧）→ 切 B → 1 s（同增量重画检查的 S8）；
    //   停止：同样的正弦 4 s → 数字静音 10 s，斜率 0 与 4.5 各一遍——峰值保持 1.5 s 后按 12 dB/s 落过整块面板（6 kHz 的峰从约
    //         −12 dB 落到底边 −84 dB 要 6 s，斜率 4.5 时从顶边落、再多约 1 s），之后全静（每帧的「什么都没变」作对照）。
    // 各 × PEAK 关（被测）/ 开（对照）× 60 / 120 帧 × 1 / 2 倍。按秒打印：频谱仪被标脏的帧、其中「曲线帧」（脏区够到面板底边——
    // setSpectrum 判变了、重建路径后 repaintCurves 的区域总是从最高的曲线顶到底边、横跨整块，读数框 / 响度小窗的重画够不到；
    // 整块重画也算在内）、脏区设备像素、advance 与增量重画的墙钟与本线程 CPU（advance 曲线帧与其余帧分开平均：差值约是一次
    // 比较之外的重建 + 标脏）。软件渲染器口径，绑单核跑更稳；Mac 的绝对值要 Max 实测。
    struct PeakBenchStats
    {
        int frames = 0, dirtyFrames = 0, curveFrames = 0;
        juce::int64 devicePixels = 0;
        Clock advance, advanceOnCurveFrames, paint, paintOnCurveFrames;

        void addFrame (Clock advanced, Clock painted, const juce::RectangleList<int>& clip, float scale, bool curves)
        {
            ++frames;
            advance += advanced;
            paint += painted;

            if (clip.isEmpty())
                return;

            ++dirtyFrames;

            for (const auto& r : clip)
            {
                const auto device = (r.toFloat() * scale).getSmallestIntegerContainer();
                devicePixels += (juce::int64) device.getWidth() * (juce::int64) device.getHeight();
            }

            if (curves)
            {
                ++curveFrames;
                advanceOnCurveFrames += advanced;
                paintOnCurveFrames += painted;
            }
        }

        PeakBenchStats& operator+= (const PeakBenchStats& other)
        {
            frames += other.frames;
            dirtyFrames += other.dirtyFrames;
            curveFrames += other.curveFrames;
            devicePixels += other.devicePixels;
            advance += other.advance;
            advanceOnCurveFrames += other.advanceOnCurveFrames;
            paint += other.paint;
            paintOnCurveFrames += other.paintOnCurveFrames;
            return *this;
        }
    };

    juce::String perFrame (double totalMs, int count)
    {
        return count > 0 ? ms (totalMs / count) : juce::String ("-");
    }

    // 一行：帧数 | 设备像素 | advance 墙钟 / CPU 合计（曲线帧与其余帧的每帧 CPU）| 重画墙钟 / CPU 合计（曲线帧与其余脏帧的每帧 CPU）| CPU 合计。
    void printPeakBenchLine (const juce::String& label, const PeakBenchStats& s, int labelWidth = 16)
    {
        const auto otherFrames = s.frames - s.curveFrames, otherDirtyFrames = s.dirtyFrames - s.curveFrames;
        std::cout << "  " << label.paddedRight (' ', labelWidth)
                  << " | fr " << juce::String (s.frames).paddedLeft (' ', 4) << " dirty " << juce::String (s.dirtyFrames).paddedLeft (' ', 4)
                  << " curves " << juce::String (s.curveFrames).paddedLeft (' ', 4)
                  << " | dev px " << juce::String (s.devicePixels).paddedLeft (' ', 9)
                  << " | advance wall " << ms (s.advance.wall).paddedLeft (' ', 8) << " cpu " << ms (s.advance.cpu).paddedLeft (' ', 8)
                  << " (cpu/fr curve " << perFrame (s.advanceOnCurveFrames.cpu, s.curveFrames)
                  << " other " << perFrame (s.advance.cpu - s.advanceOnCurveFrames.cpu, otherFrames) << ")"
                  << " | paint wall " << ms (s.paint.wall).paddedLeft (' ', 8) << " cpu " << ms (s.paint.cpu).paddedLeft (' ', 8)
                  << " (cpu/fr curve " << perFrame (s.paintOnCurveFrames.cpu, s.curveFrames)
                  << " other " << perFrame (s.paint.cpu - s.paintOnCurveFrames.cpu, otherDirtyFrames) << ")"
                  << " | cpu " << ms (s.advance.cpu + s.paint.cpu).paddedLeft (' ', 8) << std::endl;
    }

    void runSpectrumPeakBench()
    {
        enum class Scenario { s8, stopSlope0, stopSlope45 };

        struct Row
        {
            juce::String name;
            PeakBenchStats total;
        };

        std::vector<Row> rows;
        std::cout << "==== spectrum peak-off bench (T-058): GLAZE 0, 6 kHz -12 dBFS sine; each frame = 1 / fps s of audio, advanceForSnapshot, "
                     "rect-list repaint of the dirty part of the spectrum (software renderer)" << std::endl
                  << "     fr = frames; dirty = frames with the spectrum marked dirty; curves = of those, frames whose dirty region reaches the "
                     "spectrum's bottom edge (setSpectrum rebuilt the curves, or a whole-spectrum repaint); dev px = device pixels repainted;" << std::endl
                  << "     advance = advanceForSnapshot (analyser update + setSpectrum + the editor's other readouts); paint = the incremental "
                     "repaint; ms, wall and thread CPU; cpu/fr = per frame on curve frames vs the others" << std::endl;

        // 一种组合跑一遍：新会话（每遍从头开始，峰值保持、平均线不带上一遍的）。
        const auto runCase = [&rows] (Scenario scenario, bool peakOn, int fps, float scale)
        {
            const auto title = juce::String (scenario == Scenario::s8 ? "S8" : scenario == Scenario::stopSlope0 ? "stop, slope 0" : "stop, slope 4.5")
                             + " | PEAK " + (peakOn ? "on " : "off") + " | " + juce::String (fps) + " fps | x" + juce::String (scale, 0);
            SpectrumRig rig (0.0f);

            if (! rig.isValid())
            {
                std::cout << "spectrum view, its three legend toggles or the slope control not found" << std::endl;
                return;
            }

            // 峰值线默认开：关 = 点一下 PEAK 图例。S8 与「停止，斜率 0」先把斜率从默认的 4.5 换成 0。
            if (! peakOn)
                rig.clickToggle (SpectrumRig::peakToggle);

            rig.slope->setSelected (scenario == Scenario::stopSlope45 ? 2 : 0, true);
            rig.attach();

            auto& editor = rig.editor;
            auto screen = rig.newBackingImage (scale);
            paintRegion (editor, screen, scale, juce::RectangleList<int> (editor.getLocalBounds()));   // 窗口第一次显示：整帧
            rig.takeDirty();

            // 曲线帧的判据：频谱仪底边正中那个像素在脏区里（repaintCurves 的区域横跨整块、一直到底边；读数框与响度小窗在上沿）。
            const juce::Point<int> bottomCentre (rig.area.getCentreX(), rig.area.getBottom() - 1);
            SineSource sine (6000.0, -12.0);
            const SpectrumRig::AudioSource silence = [] (juce::AudioBuffer<float>& chunk) { chunk.clear(); };
            PeakBenchStats segment, total;

            const auto frame = [&] (bool advance, const SpectrumRig::AudioSource& source)
            {
                Clock advanced, painted;

                if (advance)
                {
                    rig.feed (rig.samplesForFrame (fps), source);
                    const auto started = Clock::now();
                    rig.advance (1.0 / (double) fps);
                    advanced = started.elapsed();
                }

                auto clip = rig.takeDirty();
                clip.clipTo (rig.area);

                if (! clip.isEmpty())
                {
                    const auto started = Clock::now();
                    paintRegion (editor, screen, scale, clip);   // 画的时候又标脏的留在 recorder 里：下一帧画
                    painted = started.elapsed();
                }

                segment.addFrame (advanced, painted, clip, scale, clip.containsPoint (bottomCentre));
            };

            const auto endSegment = [&] (const juce::String& label)
            {
                printPeakBenchLine (label, segment);
                total += segment;
                const auto finished = segment;
                segment = {};
                return finished;
            };

            // 放 seconds 秒，每秒一段、打印一行；返回逐秒的统计。
            const auto play = [&] (const juce::String& label, int seconds, const SpectrumRig::AudioSource& source)
            {
                std::vector<PeakBenchStats> perSecond;

                for (int second = 0; second < seconds; ++second)
                {
                    for (int i = 0; i < fps; ++i)
                        frame (true, source);

                    perSecond.push_back (endSegment (label + " " + juce::String (second) + "-" + juce::String (second + 1) + " s"));
                }

                return perSecond;
            };

            std::cout << "---- " << title << std::endl;

            if (scenario == Scenario::s8)
            {
                const auto steady = play ("steady", 12, std::ref (sine));

                // 稳态的中段 1 ～ 11 s 合起来再打一行（前 1 s、末 1 s 就是上面第一行、最后一行；这一行不进合计）。
                PeakBenchStats middle;

                for (size_t second = 1; second + 1 < steady.size(); ++second)
                    middle += steady[second];

                printPeakBenchLine ("~ steady 1-11 s", middle);

                rig.slope->setSelected (2, true);   // 斜率 0 → 4.5：点击引起的整面重画先到，这一帧只重画、不推（同 S8 检查）
                frame (false, std::ref (sine));
                endSegment ("slope 0 -> 4.5");

                editor.setListeningToOriginal (true);
                play ("B", 1, std::ref (sine));
            }
            else
            {
                play ("sine", 4, std::ref (sine));
                play ("silence", 10, silence);
            }

            printPeakBenchLine ("= total", total);
            rows.push_back ({ title, total });
        };

        for (const auto scenario : { Scenario::s8, Scenario::stopSlope0, Scenario::stopSlope45 })
            for (const auto fps : { 60, 120 })
                for (const auto scale : { 1.0f, 2.0f })
                    for (const auto peakOn : { false, true })
                        runCase (scenario, peakOn, fps, scale);

        std::cout << "==== summary (whole case per row)" << std::endl;

        for (const auto& row : rows)
            printPeakBenchLine (row.name, row.total, 36);

        std::cout << "==== spectrum peak-off bench done" << std::endl;
    }

    //==============================================================================
    // 逐位摘要（--only digest，T-032）：改动前后各跑一次、逐行比。FNV-1a 64 位喂的是图的原始字节（预乘 ARGB / RGB，
    // 不经 PNG 的反预乘），只用来判「两次是否逐位相同」。
    juce::uint64 pixelDigest (const juce::Image& image)
    {
        juce::uint64 hash = 14695981039346656037ull;
        const juce::Image::BitmapData data (image, juce::Image::BitmapData::readOnly);

        for (int y = 0; y < data.height; ++y)
            for (int x = 0; x < data.width; ++x)
            {
                const auto* pixel = data.getPixelPointer (x, y);

                for (int k = 0; k < data.pixelStride; ++k)
                    hash = (hash ^ (juce::uint64) pixel[k]) * 1099511628211ull;
            }

        return hash;
    }

    juce::String hex (juce::uint64 value)
    {
        return juce::String::toHexString ((juce::int64) value).paddedLeft ('0', 16);
    }

    void runDigest()
    {
        // ---- 瓷面参照实现（renderGlaze）：各缩放下主旋钮的像素比（含 200 / 224 的换算）× GLAZE × 去色 ----
        juce::uint64 combined = 14695981039346656037ull;

        for (const auto pixelScale : { 0.625f, 0.892857f, 1.0f, 1.339286f, 1.5f, 1.785714f, 2.0f, 2.5f, 3.571429f, 4.0f })
            for (const auto t : { 0.0f, 0.03f, 0.2f, 0.35f, 0.45f, 0.5f, 0.65f, 0.8f, 0.9f, 1.0f })
                for (const auto desaturated : { false, true })
                {
                    const auto image = celadon::ui::porcelain::renderGlaze (t, pixelScale, desaturated);
                    const auto digest = pixelDigest (image);
                    combined = (combined ^ digest) * 1099511628211ull;
                    std::cout << "digest porcelain scale=" << juce::String (pixelScale, 6) << " t=" << juce::String (t, 2)
                              << " desaturated=" << (desaturated ? 1 : 0) << " " << image.getWidth() << "x" << image.getHeight()
                              << " " << hex (digest) << std::endl;
                }

        std::cout << "digest porcelain all " << hex (combined) << std::endl;

        // ---- 静态背景：子组件全部藏起来，整张编辑器只剩画布贴的静态背景（与一条分隔线）----
        auto session = makeSession (45.0f);
        auto& editor = *session->editor;

        const auto hideChildren = [&editor]
        {
            for (auto* child : editor.getChildren())
            {
                if (child->getNumChildComponents() == 0)
                    child->setVisible (false);

                for (auto* grandChild : child->getChildren())
                    grandChild->setVisible (false);
            }
        };

        for (const auto scale : { 1.0f, 2.0f, 0.7f, 1.5f })
        {
            juce::Image target (juce::Image::RGB, juce::roundToInt ((float) editor.getWidth() * scale),
                                juce::roundToInt ((float) editor.getHeight() * scale), false);
            hideChildren();
            paintFrame (editor, target, scale, std::nullopt);   // 这一像素比的静态背景：第一次同步生成，之后缩放停稳 150 ms 再生成
            dispatchEvents (400);
            hideChildren();
            paintFrame (editor, target, scale, std::nullopt);
            std::cout << "digest static scale=" << juce::String (scale, 2) << " " << target.getWidth() << "x" << target.getHeight()
                      << " " << hex (pixelDigest (target)) << std::endl;
        }

        session->editor.reset();
        session->processor->releaseResources();
        std::cout << "==== digest done" << std::endl;
    }
}

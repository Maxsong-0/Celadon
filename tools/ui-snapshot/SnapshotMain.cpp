/*
  ==============================================================================

    SnapshotMain.cpp
    celadon_ui_snapshot：在 Linux 上把整个插件（处理器 + v0.1.8.0 的编辑器）跑起来，喂一段合成的音乐，
    再把编辑器按几种状态离屏渲染成 PNG，对照 V4 设计稿检查布局、配色、中英文与缩放（见 README.md）。

    用法：celadon_ui_snapshot --out <目录> [--scale 2] [--only 名字,名字] [--frames N] [--deterministic]
      每种状态一张图：zh-default、en-default、zh-original（A / B 听原声 + 等响）、zh-audition（Gain +8 dB 时 LIMIT 试听差值）、
      zh-truepeak（限幅旁通 → 真峰值超限）、zh-settings、en-help、zh-advanced、en-advanced（「高级」面板）、
      zh-glaze90、zh-glaze0、zh-zoom70、zh-zoom150、zh-reduced、zh-input（T-051：频谱仪 IN 虚线开着）；
      T-038（曲风 Deep House、交付 俱乐部，House 状态字段注入）：zh / en-house-layout（两个选择在 GLAZE 标题行左右；Max 2026-10-01 选定，备选图已去掉）、
      -house-status-bars / -main / -seconds（状态行三种文字，只截胶囊）、-house-tip（暂定期间的气泡）、-house-hint（反相提示）。
      --only check 跑交互检查（末尾 ⑫～⑲ 是 T-032 的回归检查，另有 T-036 频谱仪子组件缓存类型一条：分层瓷面与原算法逐像素 ≤ 2 LSB、主旋钮 / 文字小部件 / 粒子缓存图
      命中、30 帧节流、底排与延迟读数的触发、「高级」面板关着时的状态、状态胶囊的缓存与呼吸帧不排版文字），⑲ 续与 ⑳ 是 T-041 的：
      小数像素比（0.7 ～ 2.5）下胶囊呼吸与对齐缓存的局部作废只重画作废区、结果与整块重画逐字节相同；末尾两条是 T-051 的：
      频谱仪 IN 虚线自己切段与 JUCE createDashedStroke 逐位相同、频谱仪增量重画（×1 / ×2 矩形列表、×1 包围盒）与整帧重画逐字节相同
      （评审 B1 的 S8 场景，见 runSpectrumDirtyScenario）；
      --only bench 跑绘制性能基准（T-032）：瓷面参照与分层的生成耗时、静态背景生成耗时，GLAZE 45 % 播放中 1 / 2 / 4 倍下
      编辑器定时器一帧、整帧重画与只重画粒子带的毫秒（墙钟与本线程 CPU）、各部件累计 paint 与缓存读写、状态胶囊呼吸帧，
      拖动 GLAZE 时消息线程的 CPU 占比，听原声（B）时的粒子带重画；--frames N 改每段帧数。软件渲染口径，Mac 需 Max 实测；
      --only spectrum-bench 跑频谱仪专项基准（T-036）：30 / 60 / 120 帧 × 1 / 2 倍，音乐按帧率在放，每帧推一帧、只重画频谱仪那块，
      打印每帧与每秒的 CPU；--seconds N 改每种组合跑几秒（默认 3）。只用改前改后都有的接口，能在改前的树上量基线；
      --only digest 打印逐位摘要（瓷面参照实现 renderGlaze 的 200 种组合、静态背景的 4 种像素比）：改动前后各跑一次逐行比，
      应当逐位不变的项一字不差。
      --deterministic：出图时播放不跑消息循环（编辑器 / 胶囊 / 旋钮的定时器不按墙钟插进来），两次运行逐像素相同（电平表除外），
      用于改前改后逐像素对比；听原声那张图的瓷面因此还没换成去色版（换图靠主旋钮的定时器）。

  ==============================================================================
*/

#include <JuceHeader.h>

#include "../../Source/ParameterIds.h"
#include "../../Source/PluginEditor.h"
#include "../../Source/PluginProcessor.h"
#include "../../Source/ui/Porcelain.h"
#include "../../Source/ui/UiSettings.h"

#if JUCE_MAC
 #include <CoreFoundation/CoreFoundation.h>
#endif

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

    // 频谱仪的三个图例开关（OUT / IN / PEAK，SpectrumView 构造时按这个顺序加的 ClickableComponent 子组件）；找不到返回空。
    std::vector<celadon::ui::ClickableComponent*> spectrumToggles (juce::Component& editor)
    {
        std::vector<celadon::ui::ClickableComponent*> toggles;

        if (auto* view = findFirst<celadon::ui::SpectrumView> (editor))
            for (auto* child : view->getChildren())
                if (auto* toggle = dynamic_cast<celadon::ui::ClickableComponent*> (child))
                    toggles.push_back (toggle);

        return toggles.size() == 3 ? toggles : std::vector<celadon::ui::ClickableComponent*> {};
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
    void runBench (int frames);
    void runSpectrumBench (int seconds);
    void runDigest();

    // 用户设置文件跨进程保留（README「用法」末段）：上一次出图留下的值（zh-zoom70 / zh-zoom150 把默认大小写成 0.7 / 1.5，
    // zh-reduced 留下减少动效）会被下一次运行的新编辑器读进来。检查、基准、摘要开跑前一律先放回这一份基线（T-039）。
    void resetUiSettings (celadon::ui::UiSettings& settings)
    {
        settings.setLanguage (celadon::Language::chinese);
        settings.setReduceMotion (false);
        settings.setDefaultZoom (1.0);
    }

    bool save (juce::Component& component, float scale, const juce::File& file, juce::Rectangle<int> area = {})
    {
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

    // 锁定条件还没满足：按小节（宿主给了拍点）。
    StatusEdit housePendingBars (int heard, int full)
    {
        return [heard, full] (celadon::glaze::Status& status)
        {
            status.houseActive = true;
            status.houseLockPending = true;
            status.houseSegmentMode = celadon::glaze::SegmentMode::bars;
            status.houseHeardSegments = heard;
            status.houseTargetSegments = 64;
            status.houseFullSegments = full;
            status.houseRequiredFullSegments = 8;
            status.houseMainSectionHeard = full >= 8;
        };
    }

    // 同上，宿主没给拍点：按秒。
    StatusEdit housePendingSeconds (float heard)
    {
        return [heard] (celadon::glaze::Status& status)
        {
            status.houseActive = true;
            status.houseLockPending = true;
            status.houseSegmentMode = celadon::glaze::SegmentMode::seconds;
            status.houseHeardSeconds = heard;
            status.houseTargetSeconds = 124.0f;
        };
    }

    // 已锁定、House 反相提示亮着（单声道低频损失 lossDb）。
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

        // ---- T-038 曲风与交付 / House 档（曲风 Deep House、交付 俱乐部；House 的状态字段注入，见 injectStatus）----
        // 版面：两个选择在主旋钮左右（代码里的默认位置）与备选位置（挪到 GLAZE 标题行），整张。
        { "zh-house-layout", celadon::Language::chinese, houseParameters, [] (Session& s) { injectStatus (s, housePendingBars (23, 0)); }, upperCrop },
        { "en-house-layout", celadon::Language::english, houseParameters, [] (Session& s) { injectStatus (s, housePendingBars (23, 0)); }, upperCrop },
        // 状态行三种文字（设计规格-曲风-House学习方式 第 7 节），只截顶栏胶囊。
        { "zh-house-status-bars", celadon::Language::chinese, houseParameters, [] (Session& s) { injectStatus (s, housePendingBars (23, 0)); }, pillCrop },
        { "en-house-status-bars", celadon::Language::english, houseParameters, [] (Session& s) { injectStatus (s, housePendingBars (23, 0)); }, pillCrop },
        { "zh-house-status-main", celadon::Language::chinese, houseParameters, [] (Session& s) { injectStatus (s, housePendingBars (41, 8)); }, pillCrop },
        { "en-house-status-main", celadon::Language::english, houseParameters, [] (Session& s) { injectStatus (s, housePendingBars (41, 8)); }, pillCrop },
        { "zh-house-status-seconds", celadon::Language::chinese, houseParameters, [] (Session& s) { injectStatus (s, housePendingSeconds (52.0f)); }, pillCrop },
        { "en-house-status-seconds", celadon::Language::english, houseParameters, [] (Session& s) { injectStatus (s, housePendingSeconds (52.0f)); }, pillCrop },
        // 暂定期间的悬停气泡（第 3 行「从 drop 开始播放能学得更快」），截标题与气泡一块。
        { "zh-house-tip", celadon::Language::chinese, houseParameters,
          [] (Session& s) { injectStatus (s, housePendingBars (23, 0)); showGlazeTooltip (s); } },
        { "en-house-tip", celadon::Language::english, houseParameters,
          [] (Session& s) { injectStatus (s, housePendingBars (23, 0)); showGlazeTooltip (s); } },
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
        //    两个都在主旋钮两侧、不与旋钮和左下说明重叠。查完放回通用 / 标准，后面的检查不受影响。
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
                   "genre / delivery sit either side of the knob (" + genre.getBounds().toString() + ", " + delivery.getBounds().toString() + ")");

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

        std::cout << (failures == 0 ? "all interaction checks passed" : juce::String (failures) + " interaction checks FAILED") << std::endl;
        return failures;
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
        {
            auto& glazeKnob = static_cast<GlazeKnob&> (find (celadon::param::glazeAmount));
            paintFrame (editor, target, 1.0f, band);
            const auto before = glazeKnob.getPaintStats();

            for (int i = 0; i < 5; ++i)
                paintFrame (editor, target, 1.0f, band);

            const auto steady = glazeKnob.getPaintStats();
            check (steady.paints == before.paints + 5 && steady.faceRenders == before.faceRenders,
                   "T-032 GLAZE knob: 5 band repaints only blit its cached face (" + juce::String (steady.paints - before.paints) + " paints, "
                       + juce::String (steady.faceRenders - before.faceRenders) + " face renders)");

            auto* glaze = session.processor->getParameters().getParameter (celadon::param::glazeAmount);
            glaze->setValueNotifyingHost (glaze->convertTo0to1 (glaze->convertFrom0to1 (glaze->getValue()) + 1.0f));
            paintFrame (editor, target, 1.0f, band);
            paintFrame (editor, target, 1.0f, band);
            const auto moved = glazeKnob.getPaintStats();
            check (moved.faceRenders == steady.faceRenders + 1, "T-032 GLAZE knob: a value change re-composes the face once ("
                                                                    + juce::String (moved.faceRenders - steady.faceRenders) + ")");

            glazeKnob.setComparing (true);
            paintFrame (editor, target, 1.0f, band);
            paintFrame (editor, target, 1.0f, band);
            const auto comparing = glazeKnob.getPaintStats();
            check (comparing.faceRenders == moved.faceRenders + 1, "T-032 GLAZE knob: listening to the original re-composes the face once ("
                                                                       + juce::String (comparing.faceRenders - moved.faceRenders) + ")");
            glazeKnob.setComparing (session.processor->isComparingOriginal());
            dispatchEvents (400);
        }

        // ⑭ R9-01：粒子带里的 10 个文字小部件（T-038 起多了曲风、交付两个选择）都挂着整数对齐的缓存；重画粒子带时从缓存读、不再重画进缓存。
        {
            int cached = 0;
            juce::StringArray missing;

            for (const auto* id : { "glaze_title", "glaze_style", "relearn", "ab", "match", "monitoring", "corner_note", "legend",
                                    "glaze_genre", "glaze_delivery" })
            {
                if (dynamic_cast<AlignedComponentCache*> (find (id).getCachedComponentImage()) != nullptr)
                    ++cached;
                else
                    missing.add (id);
            }

            check (cached == 10, "T-032 the 10 text widgets in the particle band have aligned image caches" + (missing.isEmpty() ? juce::String() : " (missing " + missing.joinIntoString (", ") + ")"));

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
            auto& relearn = static_cast<PillButton&> (find ("relearn"));
            auto& ab = static_cast<Segmented&> (find ("ab"));
            const auto laidOut = [&] { return match.getWidth() == match.getNaturalWidth() && relearn.getWidth() == relearn.getNaturalWidth()
                                              && ab.getWidth() == ab.getNaturalWidth(); };

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
            check (laidOut() && relearn.getText() == tr (Text::relearn, celadon::Language::english),
                   "T-032 bottom row re-laid out for English (" + relearn.getText() + ")");
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
    // T-051 的两条频谱仪检查（README「--only check」）：
    //   IN 虚线：自己切段的 createDashedStrokeReusing 与 JUCE 的 createDashedStroke 逐字节相同（评审 R-07 的改法不改像素）；
    //   增量重画 vs 整帧（评审 R-08 建议、B1 的 S8 场景，守 R-02 那一类「脏区漏标、旧画面留在屏上」）。

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
    // 先按行逐字节比（相同的行不拆像素），对不上的行再逐像素量差多少。
    int countDifferences (const juce::Image& a, const juce::Image& b, juce::Rectangle<int> area, int& worst, juce::Point<int>& at)
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

                if (d == 0)
                    continue;

                if (different++ == 0)
                    at = { x, y };

                worst = juce::jmax (worst, d);
            }
        }

        return different;
    }

    // S8 跑一遍（一种像素比，几种口径共用同一串帧）。场景（评审 B1 的 S8）：GLAZE 0（链近乎直通）、峰值线关、斜率 0，
    // 6 kHz −12 dBFS 稳态正弦 12 s（最后 1 s 频谱仪应 0 帧被标脏：曲线已静止）→ 斜率换 4.5（6 kHz 抬高约 11.6 dB；先只重画
    // 一帧，路径在 paint 里重建，见下）→ 切到 B（OUT 清零重来，最高的曲线一下子没了）→ 再跑 1 s。每帧 = 1 / 60 s 音频 +
    // advanceForSnapshot，每帧都与整帧重画比（不只比切 B 的那一帧）。返回带统计的各屏；steadyDirtyFrames = 静止那 1 s 里
    // 频谱仪被标脏的帧数。
    std::vector<DirtyScreen> runSpectrumDirtyScenario (float scale, std::vector<DirtyScreen> screens, int& framesCompared,
                                                       int& steadyDirtyFrames, juce::String& error)
    {
        using namespace celadon::ui;
        auto session = makeSession (0.0f);
        auto& editor = *session->editor;
        auto* view = findFirst<SpectrumView> (editor);
        const auto toggles = spectrumToggles (editor);
        auto* slope = view != nullptr ? findFirst<Segmented> (*view) : nullptr;   // 斜率选择是频谱仪里唯一的 Segmented

        if (view == nullptr || toggles.empty() || slope == nullptr)
        {
            error = "spectrum view, its three legend toggles or the slope control not found";
            return screens;
        }

        // 峰值线关（开着时峰值保持把脏区一直撑到峰顶，盖住要找的漏标）、斜率 0。
        click (*toggles[2], toggles[2]->getLocalBounds().getCentre().toFloat());
        slope->setSelected (0, true);

        // 编辑器要「可见」：Component::internalRepaintUnchecked 对不可见的组件直接返回，repaint 传不到它的缓存图（真实窗口里
        // 宿主把编辑器设成可见；截图工具平时不设，离屏画不受影响）。没有对等体，设可见不会开窗口。
        editor.setVisible (true);
        auto* recorder = new DirtyRecorder (editor);
        editor.setCachedComponentImage (recorder);   // 编辑器接管，下面 setCachedComponentImage (nullptr) 时删掉

        const auto area = editor.getLocalArea (view, view->getLocalBounds());          // 频谱仪在编辑器里的区域（逻辑坐标）
        const auto deviceArea = (area.toFloat() * scale).getSmallestIntegerContainer();
        const auto width = (int) std::ceil ((float) editor.getWidth() * scale), height = (int) std::ceil ((float) editor.getHeight() * scale);
        juce::Image reference (juce::Image::RGB, width, height, true);   // 窗口后备图是不透明的 RGB

        for (auto& screen : screens)
        {
            screen.image = juce::Image (juce::Image::RGB, width, height, true);
            paintRegion (editor, screen.image, scale, juce::RectangleList<int> (editor.getLocalBounds()));
        }

        recorder->dirty.clear();

        constexpr int framesPerSecond = 60;
        constexpr int samplesPerFrame = (int) sampleRate / framesPerSecond;
        const auto amplitude = juce::Decibels::decibelsToGain (-12.0);
        const auto increment = juce::MathConstants<double>::twoPi * 6000.0 / sampleRate;
        double phase = 0.0;

        // 一帧：送 1 / 60 s 正弦 → 编辑器推一帧（频谱仪 setSpectrum、编辑器其余读数）→ 各屏按口径重画记下的脏区（只画频谱仪那一块，
        // 别处两边都照画、不比）→ 整帧重画频谱仪那一块作对照 → 逐字节比。advance = false：只重画、不推（窗口系统先送来别的
        // 事件引起的重画，显示器的帧回调还没到）。返回这一帧频谱仪有没有被标脏。
        const auto frame = [&] (const juce::String& stage, int index, bool advance)
        {
            for (int done = 0; advance && done < samplesPerFrame;)
            {
                const auto n = juce::jmin (blockSize, samplesPerFrame - done);
                juce::AudioBuffer<float> chunk (session->buffer.getArrayOfWritePointers(), 2, n);

                for (int i = 0; i < n; ++i)
                {
                    const auto sample = (float) (amplitude * std::sin (phase));
                    phase = std::fmod (phase + increment, juce::MathConstants<double>::twoPi);
                    chunk.setSample (0, i, sample);
                    chunk.setSample (1, i, sample);
                }

                session->processor->processBlock (chunk, session->midi);
                done += n;
            }

            if (advance)
                editor.advanceForSnapshot (1.0 / framesPerSecond);

            const auto region = recorder->dirty;
            recorder->dirty.clear();

            for (auto& screen : screens)
            {
                auto clip = screen.boundingBox && ! region.isEmpty() ? juce::RectangleList<int> (region.getBounds()) : region;
                clip.clipTo (area);
                paintRegion (editor, screen.image, scale, clip);
            }

            const auto markedWhilePainting = recorder->dirty;   // 画的时候又标脏的：真实窗口里留到下一帧画
            paintRegion (editor, reference, scale, juce::RectangleList<int> (area));
            recorder->dirty = markedWhilePainting;               // 整帧对照不是真实的那一次绘制，它引起的标脏不算
            ++framesCompared;

            for (auto& screen : screens)
            {
                int worst = 0;
                juce::Point<int> at;
                const auto different = countDifferences (screen.image, reference, deviceArea, worst, at);

                if (different == 0)
                    continue;

                if (screen.ghostFrames++ == 0)
                    screen.firstGhost = stage + " frame " + juce::String (index) + ": " + juce::String (different) + " px, first at device ("
                                      + juce::String (at.x) + ", " + juce::String (at.y) + ")";

                screen.worst = juce::jmax (screen.worst, worst);
            }

            return region.intersects (area);
        };

        constexpr int steadyFrames = 12 * framesPerSecond;

        for (int i = 0; i < steadyFrames; ++i)
            if (frame ("steady sine", i, true) && i >= steadyFrames - framesPerSecond)
                ++steadyDirtyFrames;

        // 斜率 0 → 4.5：点击引起的整面重画先到、显示器的帧回调后到（两者谁先到不固定），路径在 paint 里重建——这一帧只重画、不推。
        // 评审复现时（577ded0）setSpectrum 按未加斜率的 dB 判变化，曲线静止时换斜率后下一帧也不重画，等价于这里；T-036 修订
        // （867b454，评审 R-07）改按加斜率后的纵坐标判之后，换到 4.5 时压在底边下、还在往下落的峰值保持数据露出来，先推一帧的话
        // 旧区域当帧就跟上了，照原样走 S8 抓不到 R-02 那一类漏标。
        slope->setSelected (2, true);
        frame ("slope 4.5 repaint", 0, false);

        editor.setListeningToOriginal (true);

        for (int i = 0; i < framesPerSecond; ++i)
            frame ("B", i, true);

        editor.setCachedComponentImage (nullptr);
        session->editor.reset();
        session->processor->releaseResources();
        return screens;
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

        // 增量重画 vs 整帧：S8 跑两遍——×1 两种口径（矩形列表、包围盒）、×2 矩形列表（评审 B1 的三种口径）。
        {
            juce::String error;
            int frames = 0, steady1 = 0, steady2 = 0;
            auto screens = runSpectrumDirtyScenario (1.0f, { { "x1 rect list", false }, { "x1 bounding box", true } }, frames, steady1, error);

            for (auto& screen : runSpectrumDirtyScenario (2.0f, { { "x2 rect list", false } }, frames, steady2, error))
                screens.push_back (std::move (screen));

            juce::StringArray ghosts;

            for (const auto& screen : screens)
                if (screen.ghostFrames > 0)
                    ghosts.add (screen.name + " " + juce::String (screen.ghostFrames) + " frame(s), worst " + juce::String (screen.worst)
                                + " (first: " + screen.firstGhost + ")");

            check (error.isEmpty() && steady1 == 0 && steady2 == 0 && ghosts.isEmpty(),
                   "T-051 spectrum incremental repaint == full repaint (S8: GLAZE 0, peak off, 6 kHz -12 dBFS sine 12 s, slope 0 -> 4.5, then B; "
                       + (error.isNotEmpty() ? error
                                             : (ghosts.isEmpty() ? "0 ghosts" : ghosts.joinIntoString ("; ")) + " over " + juce::String (frames)
                                                   + " frames; spectrum marked dirty in " + juce::String (steady1) + " / " + juce::String (steady2)
                                                   + " of the last 60 steady frames, needs 0")
                       + ")");
        }
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
            else if (component->getComponentID() == "relearn" || component->getComponentID() == "match"
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

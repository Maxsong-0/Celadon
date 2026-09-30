/*
  ==============================================================================

    SnapshotMain.cpp
    celadon_ui_snapshot：在 Linux 上把整个插件（处理器 + v0.1.8.0 的编辑器）跑起来，喂一段合成的音乐，
    再把编辑器按几种状态离屏渲染成 PNG，对照 V4 设计稿检查布局、配色、中英文与缩放（见 README.md）。

    用法：celadon_ui_snapshot --out <目录> [--scale 2] [--only 名字,名字] [--frames N]
      每种状态一张图：zh-default、en-default、zh-original（A / B 听原声 + 等响）、zh-audition（Gain +8 dB 时 LIMIT 试听差值）、
      zh-truepeak（限幅旁通 → 真峰值超限）、zh-settings、en-help、zh-advanced、en-advanced（「高级」面板）、
      zh-glaze90、zh-glaze0、zh-zoom70、zh-zoom150、zh-reduced。
      --only check 跑交互检查（末尾 ⑫～⑲ 是 T-032 的回归检查：分层瓷面与原算法逐像素 ≤ 2 LSB、主旋钮 / 文字小部件 / 粒子缓存图
      命中、30 帧节流、底排与延迟读数的触发、「高级」面板关着时的状态、状态胶囊呼吸帧不排版文字）；--only bench 跑绘制性能基准（T-032：每帧 paint 毫秒、
      各部件累计时长与缓存读写、瓷面参照与分层的生成耗时、静态背景耗时；--frames N 改每段帧数。软件渲染口径，Mac 需 Max 实测）；
      --only digest 打印逐位摘要（瓷面参照实现 renderGlaze 的 200 种组合、静态背景的 4 种像素比）：改动前后各跑一次逐行比，
      应当逐位不变的项一字不差。

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
    void runBench (int frames);
    void runDigest();

    bool save (juce::Component& component, float scale, const juce::File& file)
    {
        const auto image = component.createComponentSnapshot (component.getLocalBounds(), true, scale);
        file.deleteFile();
        juce::FileOutputStream stream (file);

        if (! stream.openedOk())
            return false;

        juce::PNGImageFormat png;
        return png.writeImageToStream (image, stream);
    }
}

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI gui;

    juce::File outDir = juce::File::getCurrentWorkingDirectory();
    float scale = 1.0f;
    int benchFrames = 120;
    juce::StringArray only;

    for (int i = 1; i < argc; ++i)
    {
        const juce::String arg (argv[i]);

        if (arg == "--out" && i + 1 < argc)         outDir = juce::File::getCurrentWorkingDirectory().getChildFile (argv[++i]);
        else if (arg == "--scale" && i + 1 < argc)  scale = (float) juce::String (argv[++i]).getDoubleValue();
        else if (arg == "--only" && i + 1 < argc)   only.addTokens (argv[++i], ",", {});
        else if (arg == "--frames" && i + 1 < argc) benchFrames = juce::jmax (1, juce::String (argv[++i]).getIntValue());
    }

    outDir.createDirectory();
    juce::SharedResourcePointer<celadon::ui::UiSettings> settings;

    if (only.contains ("bench"))
    {
        settings->setLanguage (celadon::Language::chinese);
        settings->setReduceMotion (false);
        settings->setDefaultZoom (1.0);
        runBench (benchFrames);
        return 0;
    }

    if (only.contains ("digest"))
    {
        settings->setLanguage (celadon::Language::chinese);
        settings->setReduceMotion (false);
        settings->setDefaultZoom (1.0);
        runDigest();
        return 0;
    }

    if (only.contains ("check"))
    {
        settings->setLanguage (celadon::Language::chinese);
        settings->setReduceMotion (false);

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
    };

    const auto none = [] (Session&) {};

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
        const auto ok = save (*session.editor, scale, file);
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

        // ⑭ R9-01：粒子带里的 8 个文字小部件都挂着整数对齐的缓存；重画粒子带时从缓存读、不再重画进缓存。
        {
            int cached = 0;
            juce::StringArray missing;

            for (const auto* id : { "glaze_title", "glaze_style", "relearn", "ab", "match", "monitoring", "corner_note", "legend" })
            {
                if (dynamic_cast<AlignedComponentCache*> (find (id).getCachedComponentImage()) != nullptr)
                    ++cached;
                else
                    missing.add (id);
            }

            check (cached == 8, "T-032 the 8 text widgets in the particle band have aligned image caches" + (missing.isEmpty() ? juce::String() : " (missing " + missing.joinIntoString (", ") + ")"));

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

        // ⑲ R9-13：状态胶囊呼吸时只重画圆点那一小块（圆点 7 px + 3 px 光晕 + 1 px 抗锯齿），这一帧胶囊与顶栏一段文字都不排版；
        //     整个胶囊重画时每段文字照画（跳过只按脏区判断，不会漏画）。
        {
            auto* pill = findFirst<StatusPill> (editor);
            auto* topBar = findFirst<TopBar> (editor);
            const auto area = pill != nullptr ? pill->getPulseArea() : juce::Rectangle<int>();
            check (pill != nullptr && topBar != nullptr && ! area.isEmpty() && area.getWidth() <= 16 && area.getHeight() <= 16
                       && pill->getLocalBounds().contains (area),
                   "T-032 status pill pulses by repainting only its dot (" + area.toString() + " of " + (pill != nullptr ? pill->getLocalBounds().toString() : juce::String ("-")) + ")");

            if (pill != nullptr && topBar != nullptr)
            {
                const auto pillBefore = pill->getTextDraws(), barBefore = topBar->getTextDraws();
                paintFrame (editor, target, 1.0f, editor.getLocalArea (pill, area));
                const auto pillPulse = pill->getTextDraws() - pillBefore, barPulse = topBar->getTextDraws() - barBefore;
                paintFrame (editor, target, 1.0f, editor.getLocalArea (pill, pill->getLocalBounds()));
                const auto pillWhole = pill->getTextDraws() - pillBefore - pillPulse;

                check (pillPulse == 0 && barPulse == 0 && pillWhole > 0,
                       "T-032 a pulse frame lays out no text (pill " + juce::String (pillPulse) + ", top bar " + juce::String (barPulse)
                           + "); a whole-pill repaint draws " + juce::String (pillWhole) + " text item(s)");
            }
        }

        return 0;
    }
}

//==============================================================================
// 性能基准（--only bench，T-032 R9-01 / 02 / 05 / 07 / 08 / 12 / 13 / 14 的改前改后对照）。
// 离屏没有窗口系统的脏矩形，这里按两种口径各量一遍：
//   full = 整个编辑器重画（宿主首帧、缩放后的那一帧）；
//   band = 只把粒子带（画布 y 300 ～ 560 的整条）标脏——macOS 的 CoreGraphics 只拿脏矩形的包围盒，粒子每帧 repaint
//          在 Mac 上就是这一条整带，带里的主旋钮、标题、按钮、图例都要跟着重画。
// 另外给主旋钮、粒子、中层文字部件与顶栏状态胶囊各挂一个 ComponentListener（JUCE 9 的 ComponentPaintDiagnostics），
// 累计各自的 paint 时长与「从缓存读」（setBufferedToImage）的次数。数字是软件渲染器的，Mac 的绝对值需 Max 用活动监视器实测。
namespace
{
    void runBench (int frames)
    {
        std::cout << "==== bench: frames=" << frames << " (software renderer; Mac absolute numbers need Max)" << std::endl;

        // ---- 瓷面渲染：拖动 GLAZE 时每次生成的开销，按像素比与 GLAZE 各量三次取最短 ----
        //   renderGlaze = 原算法（逐像素全量，T-032 前主旋钮每次都走它）；layered = GlazeRenderer（R9-02 分层，界面现在用它）。
        celadon::ui::porcelain::GlazeRenderer renderer;

        const auto timeGlaze = [&renderer] (float t, float scale, bool desaturated, bool layered)
        {
            double best = 1.0e9;

            for (int repeat = 0; repeat < 3; ++repeat)
            {
                const auto started = juce::Time::getMillisecondCounterHiRes();
                const auto image = layered ? renderer.render (t, scale, desaturated) : celadon::ui::porcelain::renderGlaze (t, scale, desaturated);
                best = juce::jmin (best, juce::Time::getMillisecondCounterHiRes() - started);
                juce::ignoreUnused (image);
            }

            return best;
        };

        for (const auto scale : { 1.0f, 2.0f, 4.0f })
        {
            // 分层的第一次（这个像素比下建两张基底图、覆盖层与裂纹轮廓）单独量一次。
            const auto firstStarted = juce::Time::getMillisecondCounterHiRes();
            juce::ignoreUnused (renderer.render (0.45f, scale, false));
            const auto first = juce::Time::getMillisecondCounterHiRes() - firstStarted;
            std::cout << "porcelain layered first render (builds layers)  scale=" << scale << "  " << ms (first) << " ms" << std::endl;

            for (const auto t : { 0.2f, 0.45f, 0.9f })
                std::cout << "porcelain renderGlaze  scale=" << scale << " t=" << t << "  " << ms (timeGlaze (t, scale, false, false))
                          << " ms   layered " << ms (timeGlaze (t, scale, false, true)) << " ms" << std::endl;
        }

        std::cout << "porcelain renderGlaze  scale=2 t=0.45 desaturated  " << ms (timeGlaze (0.45f, 2.0f, true, false))
                  << " ms   layered " << ms (timeGlaze (0.45f, 2.0f, true, true)) << " ms" << std::endl;

        // ---- 静态底图 + 首帧缓存：新建编辑器后的第一帧（含 renderStaticLayer、首次瓷面、流线 / 柔光缓存）与第二帧 ----
        for (const auto scale : { 1.0f, 2.0f })
        {
            auto session = makeSession (45.0f);
            session->play (0.2);

            auto& editor = *session->editor;
            juce::Image target (juce::Image::RGB, juce::roundToInt ((float) editor.getWidth() * scale),
                                juce::roundToInt ((float) editor.getHeight() * scale), false);
            const auto first = paintFrame (editor, target, scale, std::nullopt);
            const auto second = paintFrame (editor, target, scale, std::nullopt);
            std::cout << "first frame  scale=" << scale << "  " << ms (first) << " ms (static layer + first caches); second frame "
                      << ms (second) << " ms; static layer alone " << ms (editor.getStaticLayerRenderMs()) << " ms" << std::endl;
            session->editor.reset();
            session->processor->releaseResources();
        }

        // ---- 每帧口径：GLAZE 45 %、音乐在放（粒子在动、状态胶囊在呼吸）----
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
                     || component->getComponentID() == "monitoring")
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

        const auto report = [&] (const juce::String& phase, int count, double fullTotal, double bandTotal)
        {
            std::cout << "---- " << phase << ": " << count << " frames" << std::endl;

            if (fullTotal >= 0.0)
                std::cout << "  full-frame paint  avg " << ms (fullTotal / count) << " ms" << std::endl;

            if (bandTotal >= 0.0)
                std::cout << "  band-only paint   avg " << ms (bandTotal / count) << " ms  (particle band dirty: macOS bounding-box case)" << std::endl;

            for (auto& p : probes)
            {
                std::cout << "  " << p->name.paddedRight (' ', 14) << " paints " << juce::String (p->paints).paddedLeft (' ', 5)
                          << "  total " << ms (p->totalMs).paddedLeft (' ', 9) << " ms  avg " << ms (p->paints > 0 ? p->totalMs / p->paints : 0.0)
                          << " ms  cacheReads " << p->cacheReads << "  cacheWrites " << p->cacheWrites << std::endl;
                p->reset();
            }

            const auto knobNow = knob->getPaintStats();
            const auto fieldNow = field->getCacheStats();
            std::cout << "  GlazeKnob face renders " << (knobNow.faceRenders - knobBefore.faceRenders) << " of " << (knobNow.paints - knobBefore.paints)
                      << " paints; porcelain renders " << (knobNow.porcelainRenders - knobBefore.porcelainRenders) << " (layer builds "
                      << (knobNow.porcelainLayerBuilds - knobBefore.porcelainLayerBuilds) << ", last " << ms (knobNow.lastPorcelainMs) << " ms)"
                      << "; ParticleField line / glow renders " << (fieldNow.lineRenders - fieldBefore.lineRenders) << " / "
                      << (fieldNow.glowRenders - fieldBefore.glowRenders) << std::endl;
        };

        const juce::Rectangle<int> band (0, celadon::ui::ParticleField::top, editor.getWidth(), celadon::ui::ParticleField::bandHeight);

        for (const auto scale : { 1.0f, 2.0f })
        {
            juce::Image target (juce::Image::RGB, juce::roundToInt ((float) editor.getWidth() * scale),
                                juce::roundToInt ((float) editor.getHeight() * scale), false);
            paintFrame (editor, target, scale, std::nullopt);   // 暖机：这一比例的缓存先生成
            dispatchEvents (200);                                // 让缩放后 150 ms 的静态底图 / 瓷面重生成先做完
            paintFrame (editor, target, scale, std::nullopt);

            startPhase();
            double fullTotal = 0.0, bandTotal = 0.0;

            for (int frame = 0; frame < frames; ++frame)
            {
                editor.advanceForSnapshot (1.0 / 30.0);
                dispatchEvents (1);
                fullTotal += paintFrame (editor, target, scale, std::nullopt);
                bandTotal += paintFrame (editor, target, scale, band);
            }

            report ("steady GLAZE 45 %, playing, scale " + juce::String (scale), frames, fullTotal, bandTotal);

            // 状态胶囊呼吸的一帧：脏区只有胶囊里的圆点（顶栏、胶囊的 paint 都跳过与它不相交的文字）。
            if (auto* pill = findFirst<celadon::ui::StatusPill> (editor))
            {
                const auto pulse = editor.getLocalArea (pill, pill->getPulseArea());
                const auto whole = editor.getLocalArea (pill, pill->getLocalBounds());
                double pulseTotal = 0.0, wholeTotal = 0.0;

                for (int frame = 0; frame < frames; ++frame)
                {
                    pulseTotal += paintFrame (editor, target, scale, pulse);
                    wholeTotal += paintFrame (editor, target, scale, whole);
                }

                std::cout << "  status pill pulse frame (dot only " << pulse.toString() << ")  avg " << ms (pulseTotal / frames)
                          << " ms; whole pill avg " << ms (wholeTotal / frames) << " ms" << std::endl;

                for (auto& p : probes)
                    p->reset();
            }
        }

        // ---- 拖动 GLAZE：35 % → 65 %，每帧走 33 ms 消息循环让旋钮的定时器按节流重生成瓷面 ----
        {
            const auto scale = 2.0f;
            juce::Image target (juce::Image::RGB, juce::roundToInt ((float) editor.getWidth() * scale),
                                juce::roundToInt ((float) editor.getHeight() * scale), false);
            constexpr int dragFrames = 90;
            double bandTotal = 0.0;
            startPhase();
            const auto started = juce::Time::getMillisecondCounterHiRes();

            for (int frame = 0; frame < dragFrames; ++frame)
            {
                session->setParameter (celadon::param::glazeAmount, 35.0f + 30.0f * (float) frame / (float) (dragFrames - 1));
                editor.advanceForSnapshot (1.0 / 30.0);
                dispatchEvents (33);
                bandTotal += paintFrame (editor, target, scale, band);
            }

            const auto wall = juce::Time::getMillisecondCounterHiRes() - started;
            report ("drag GLAZE 35 -> 65 %, scale 2", dragFrames, -1.0, bandTotal);
            std::cout << "  wall time " << ms (wall) << " ms for " << dragFrames << " frames of 33 ms (" << ms (wall - 33.0 * dragFrames)
                      << " ms beyond the message-loop waits: porcelain re-renders + paints)" << std::endl;
        }

        // ---- 听原声（B）：主旋钮走透明图层 / 缓存乘 0.55 的那条路 ----
        {
            const auto scale = 2.0f;
            juce::Image target (juce::Image::RGB, juce::roundToInt ((float) editor.getWidth() * scale),
                                juce::roundToInt ((float) editor.getHeight() * scale), false);
            editor.setListeningToOriginal (true);
            session->play (1.0);
            paintFrame (editor, target, scale, std::nullopt);
            dispatchEvents (300);
            paintFrame (editor, target, scale, std::nullopt);

            startPhase();
            double bandTotal = 0.0;

            for (int frame = 0; frame < frames; ++frame)
            {
                editor.advanceForSnapshot (1.0 / 30.0);
                dispatchEvents (1);
                bandTotal += paintFrame (editor, target, scale, band);
            }

            report ("comparing (B), scale 2", frames, -1.0, bandTotal);
            editor.setListeningToOriginal (false);
        }

        for (size_t i = 0; i < probes.size(); ++i)
            probed[i]->removeComponentListener (probes[i].get());

        session->editor.reset();
        session->processor->releaseResources();
        std::cout << "==== bench done" << std::endl;
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

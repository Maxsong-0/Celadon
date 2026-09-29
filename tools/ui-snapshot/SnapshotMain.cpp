/*
  ==============================================================================

    SnapshotMain.cpp
    celadon_ui_snapshot：在 Linux 上把整个插件（处理器 + v0.1.8.0 的编辑器）跑起来，喂一段合成的音乐，
    再把编辑器按几种状态离屏渲染成 PNG，对照 V4 设计稿检查布局、配色、中英文与缩放（见 README.md）。

    用法：celadon_ui_snapshot --out <目录> [--scale 2] [--only 名字,名字]
      每种状态一张图：zh-default、en-default、zh-original（A / B 听原声 + 等响）、zh-audition（Gain +8 dB 时 LIMIT 试听差值）、
      zh-truepeak（限幅旁通 → 真峰值超限）、zh-settings、en-help、zh-advanced、en-advanced（「高级」面板）、
      zh-glaze90、zh-glaze0、zh-zoom70、zh-zoom150、zh-reduced。

  ==============================================================================
*/

#include <JuceHeader.h>

#include "../../Source/ParameterIds.h"
#include "../../Source/PluginEditor.h"
#include "../../Source/PluginProcessor.h"
#include "../../Source/ui/UiSettings.h"

#if JUCE_MAC
 #include <CoreFoundation/CoreFoundation.h>
#endif

#include <cmath>
#include <iostream>

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

    int runChecks (Session& session, const juce::File& outDir, float scale);

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
    juce::StringArray only;

    for (int i = 1; i < argc; ++i)
    {
        const juce::String arg (argv[i]);

        if (arg == "--out" && i + 1 < argc)        outDir = juce::File::getCurrentWorkingDirectory().getChildFile (argv[++i]);
        else if (arg == "--scale" && i + 1 < argc) scale = (float) juce::String (argv[++i]).getDoubleValue();
        else if (arg == "--only" && i + 1 < argc)  only.addTokens (argv[++i], ",", {});
    }

    outDir.createDirectory();
    juce::SharedResourcePointer<celadon::ui::UiSettings> settings;

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
                        auto value = (float) ((double) (noise >> 8) / 8388608.0 - 1.0) * 0.05f;
                        for (const auto frequency : { 60.0, 1000.0, 9000.0 })
                            value += (float) (0.03 * std::sin (juce::MathConstants<double>::twoPi * frequency * (start + sample) / rate));
                        input.setSample (0, sample, value);
                        input.setSample (1, sample, -value);
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

        std::cout << (failures == 0 ? "all interaction checks passed" : juce::String (failures) + " interaction checks FAILED") << std::endl;
        return failures;
    }
}

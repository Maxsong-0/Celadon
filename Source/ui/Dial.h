/*
  ==============================================================================

    Dial.h
    旋钮（T-100，ADR-037）：公共部分 DialControl 与卡片小旋钮 ParamKnob；主旋钮 GlazeKnob 在自己的文件里，也从 DialControl 派生。
    外圈照 React Bits 的 CometDial 的手感（绕圈拖、点外圈跳过去、快甩有惯性），中间照旧上下拖；画出来的位置（读数）带弹簧，
    转动时指示点身后拖一条彗尾。子类只管自己的几何与画法。计算部分（常数、公式）在 DialMotion.h。

    分区（按下点离圆心的距离 d）：
      - 外圈（deadRadius ≤ d ≤ grabRadius）：按下即跳到所按角度——按住 ⌘ / Ctrl 按下不跳，改相对转动；移动超过约 7°
        （原版 R 80 上 10 px，按轨道半径换算，最少 4 px）起算拖，值跟着角度走（拖进底部缺口锁在来的那一端；按下就在缺口里
        锁在近的一端），读数按 60 ms 追手；松手前 80 ms 内还在动就按速度多滑一段，再用弹簧停稳。
        拖动中按下 ⌘ / Ctrl：从那一刻起改相对转动、慢 8 倍，到松手都保持相对（松开 ⌘ 不跳）。
      - 其余（中间、小旋钮的名字与数值行）：上下拖（左右也算），与 v0.1.8.0 起相同（满量程像素由子类给）。
    双击回默认、Alt 单击输入、滚轮、数值气泡、手势都在 ParameterControl：拖一次 = 一个宿主手势，甩出去的落点也在手势里设好。

    读数怎么动：
      - 拖动（两种拖法）：追手；松手后从当前位置起弹簧（外圈带松手速度与按速度插值的回弹）；
      - 点外圈、宿主自动化、预设、撤销、输入数值、双击回默认：从当前位置起弹簧（回弹 0.2）；
      - 滚轮：直接落到新值（原版键盘微调的做法：不出彗尾；动画正在跑时也不把这一跳算成速度）；
      - 「减少动效」：一律直接落下，没有彗尾；快甩也不多滑（与改前一样没有惯性）。
    动画由 60 Hz 定时器推（不用垂直同步：截图工具无界面跑检查时收不到），停稳（读数到位——拖动中就是追上了手——且彗尾速度
    落到阈值下）就停，按住不动也停，再动时重新起。
    静止时读数精确等于参数值，画出来与没有动画时逐像素相同；截图工具存静态图之前调 finishMotion。

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>

#include "Controls.h"
#include "DialMotion.h"

namespace celadon::ui
{
    class DialControl  : public ParameterControl
    {
    public:
        DialControl (juce::RangedAudioParameter& parameter, InteractionHost& host);
        ~DialControl() override;

        void setReduceMotion (bool shouldReduceMotion);
        bool getReduceMotion() const noexcept           { return reduceMotion; }

        // 读数直接落到参数值、彗尾清零、停掉动画（静态出图之前；检查也用）。
        void finishMotion();

        // 不等定时器，按 seconds 推进一帧动画（截图工具的检查用：按确定的步长推，不靠墙钟）。
        void advanceMotion (double seconds);

        // 读数：比例（弹簧过冲时可能略出 0 ～ 1）与参数单位（钳到范围；停稳时就是参数值本身，文字与改前逐字相同）。
        float getReadingProportion() const noexcept     { return reading.getPosition(); }
        float getReadingValue() const;
        bool isReadingSettled() const noexcept;

        float getCometStrength() const noexcept         { return comet.getStrength(); }
        int getCometDirection() const noexcept          { return comet.getDirection(); }
        bool isMotionRunning() const noexcept           { return ticker.isTimerRunning(); }

        // 按下点在不在外圈（截图工具的检查也用它找点）。
        bool isInRing (juce::Point<float> position) const;

        void mouseMove (const juce::MouseEvent&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDrag (const juce::MouseEvent&) override;
        void mouseUp (const juce::MouseEvent&) override;
        void mouseDoubleClick (const juce::MouseEvent&) override;
        void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    protected:
        struct Geometry
        {
            juce::Point<float> centre;       // 圆心（本地坐标）
            float trackRadius = 0.0f;        // 轨道半径（外圈拖动门槛按它换算）
            float deadRadius = 0.0f;         // 离圆心小于它算中间
            float grabRadius = 0.0f;         // 大于它也算中间（小旋钮的名字、数值行）
            float fullDragPixels = 0.0f;     // 中间上下拖：拖满量程的像素
        };

        virtual Geometry getDialGeometry() const = 0;

        // 读数或彗尾变了（每帧动画、参数变了）：默认重画。
        virtual void dialMotionChanged()                { repaint(); }

        void parameterValueChanged() override;

        // 彗尾：沿半径 radius 的轨道画在读数身后。thickness = 轨道宽，extraWidth = 满强度时头部多出的线宽；
        // 颜色与数值弧同一函数（bipolar、saturation 同 drawValueArc）。
        void drawComet (juce::Graphics& g, juce::Point<float> centre, float radius, float thickness, float extraWidth,
                        bool bipolar, float saturation = 1.0f) const;

        // 光标：外圈是手，其余由子类定（主旋钮上下箭头；小旋钮照旧默认箭头）。
        juce::MouseCursor::StandardCursorType centreCursor = juce::MouseCursor::NormalCursor;

    private:
        enum class Grab { none, ring, centre };

        bool isFollowing() const noexcept               { return grab == Grab::centre || (grab == Grab::ring && ringMoved); }
        void startMotion();
        void tick();
        void settleFromRelease (float bounce, float velocity);
        float pointerAngle (juce::Point<float> position) const;
        float ringProportion (juce::Point<float> position);

        dial::Reading reading;
        dial::Comet comet;
        dial::FlickTracker flick;
        juce::TimedCallback ticker { [this] { tick(); } };
        double lastTickMs = 0.0;
        bool reduceMotion = false;
        bool jumpNextChange = false;       // 滚轮：这次参数变了读数直接落下

        Grab grab = Grab::none;
        bool ringMoved = false;            // 外圈：按下后移动过 dial::dragThreshold（之前算点按）
        bool ringRelative = false;         // 外圈：改成相对转动（⌘ / Ctrl 按过）
        dial::Side side = dial::Side::none;
        juce::Point<float> downPosition, lastDragPosition;
        float lastPointerAngle = 0.0f;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DialControl)
    };

    //==============================================================================
    // 模块卡片里的小旋钮（72 × 100）：名字（上）+ 54 px 旋钮 + 数值（下，GLAZE 的自动加量紫色接在后面）。
    // 外圈 = 轨道两侧各 8 px 以内、帽子以外（帽子半径 17，比 0.55 × 23 大）；帽子、名字行、数值行照旧上下拖。
    class ParamKnob  : public DialControl
    {
    public:
        struct Options
        {
            bool bipolar = false;        // 从正中往两边长（Tone、Brighten、Output……）
            int autoDecimals = 0;        // 自动加量显示几位小数（整数量 0，dB 量 1）
        };

        ParamKnob (juce::RangedAudioParameter& parameter, InteractionHost& host, Options options);

        void setLabel (const juce::String& newName);

        // GLAZE 叠加后的实际值（参数单位）；空 = 不显示（GLAZE 0）。只在看得出变化时重画。
        void setEffectiveValue (std::optional<float> effective);

        void paint (juce::Graphics&) override;

        static constexpr int width = 72;
        static constexpr int height = 100;

    protected:
        juce::Point<float> getBubbleAnchor() const override;
        juce::Rectangle<int> getTextEntryArea() const override;
        Geometry getDialGeometry() const override;

    private:
        juce::String autoAmountText() const;

        Options options;
        juce::String name;
        std::optional<float> effectiveValue;
        float effectiveBaseValue = 0.0f;          // 上次 setEffectiveValue 时的用户值（加量文字只由它与 effectiveValue 决定）
        juce::String shownAutoText;               // 上次重画时的自动加量文字与实际值比例（-1 = 不显示）
        float shownEffectiveProportion = -1.0f;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ParamKnob)
    };
}

/*
  ==============================================================================

    ParameterRanges.h
    浮点参数的取值范围与刻度。只依赖 juce_core：参数表（Parameters.cpp）与无头测试共用同一份，测试按真范围做往返。

    宿主读工程时 APVTS 对每个参数做两趟归一化往返（JUCE 9.0.2：setDenormalisedValue → AudioParameterFloat::setValue 一趟，
    ParameterAdapter::parameterValueChanged 里 denormalise (getValue()) 又一趟；处理器经 getRawParameterValue 读到第二趟的结果）。
    每趟都是 RangedAudioParameter 的换算：值 → 吸附 → 0 ～ 1 → 值 → 吸附。
      - 线性刻度都带步进，往返后吸附回同一个格点，逐位不变；
      - 对数刻度（logRange）连续取值、不吸附，powf / logf 重算一遍会漂几个 ulp；反复存读会慢慢游走，最多约 1.2e-5（相对，
        2026-09-29 在 GCC / glibc 上对区间内每个 float 穷举实测，约 100 趟后停在不动点）。
    所以对数刻度的值不能按取整网格做精确比较：GLAZE 校准参考条件把它们移出精确哈希、按相对容差比（BUG-004，ADR-018；
    GlazeController.h setManualConditions）。新加对数刻度参数时同样要这样处理。

  ==============================================================================
*/

#pragma once

#include <juce_core/juce_core.h>

#include <cmath>

namespace celadon::param::ranges
{
    // 真对数刻度：旋钮正中间是两端的几何中心（Limit Release 10 ～ 1000 ms 中间 100 ms；
    // Clay 低切 10 ～ 60 Hz 中间约 24.5 Hz、收单 40 ～ 300 Hz 中间约 110 Hz）。频率与时间都按倍数感知，线性刻度会把可用段挤在一头。
    // 不给吸附函数、步长 0：连续取值（APVTS 按 NormalisableRange 只做钳制）。
    inline juce::NormalisableRange<float> logRange (float start, float end)
    {
        return { start, end,
                 [] (float s, float e, float normalised) { return s * std::pow (e / s, normalised); },
                 [] (float s, float e, float value)      { return std::log (value / s) / std::log (e / s); } };
    }

    // ---- Limit（设计规格-Limit「参数」表）----
    inline juce::NormalisableRange<float> limitGain()      { return { 0.0f, 24.0f, 0.01f }; }
    inline juce::NormalisableRange<float> limitCeiling()   { return { -3.0f, 0.0f, 0.1f }; }
    inline juce::NormalisableRange<float> limitRelease()   { return logRange (10.0f, 1000.0f); }
    // 步进 0.5 ms：每换一档都要重置限幅器并重新上报延迟，连续值拖动会频繁重置。
    inline juce::NormalisableRange<float> limitLookahead() { return { 1.0f, 10.0f, 0.5f }; }
    inline juce::NormalisableRange<float> limitClip()      { return { 0.0f, 100.0f, 0.1f }; }

    // ---- Color（设计规格-Color「参数」表）----
    inline juce::NormalisableRange<float> colorTone()      { return { -3.0f, 3.0f, 0.01f }; }
    inline juce::NormalisableRange<float> colorGlue()      { return { 0.0f, 100.0f, 0.1f }; }
    inline juce::NormalisableRange<float> colorHeat()      { return { 0.0f, 100.0f, 0.1f }; }
    inline juce::NormalisableRange<float> colorMix()       { return { 0.0f, 100.0f, 0.1f }; }
    inline juce::NormalisableRange<float> colorOutput()    { return { -6.0f, 6.0f, 0.01f }; }

    // ---- Balance（设计规格-Balance「参数」表）----
    inline juce::NormalisableRange<float> balanceRecover()  { return { 0.0f, 100.0f, 0.1f }; }
    inline juce::NormalisableRange<float> balanceTame()     { return { 0.0f, 100.0f, 0.1f }; }
    inline juce::NormalisableRange<float> balanceBrighten() { return { -100.0f, 100.0f, 0.1f }; }

    // ---- GLAZE 总旋钮 ----
    inline juce::NormalisableRange<float> glazeAmount()    { return { 0.0f, 100.0f, 0.1f }; }

    // ---- Clay 修坯（设计规格-Clay 第 6 节）：手动值的范围必须与 ClayModule 的钳位常数一致（处理器 Debug 版构造时逐项核对）----
    inline juce::NormalisableRange<float> clayLowCut()     { return logRange (10.0f, 60.0f); }    // 低切（L/R 4 阶 Butterworth 高通）
    inline juce::NormalisableRange<float> clayMono()       { return logRange (40.0f, 300.0f); }   // 低频收单（只对 S 的 2 阶高通）的分频点
    // 加宽（只对 S 的 2 kHz 高架）：线性、0.01 dB 步进；负值 = 收窄，只能手动。
    inline juce::NormalisableRange<float> clayWidth()      { return { -3.0f, 3.0f, 0.01f }; }
}

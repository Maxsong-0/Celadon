/*
  ==============================================================================

    ReleaseSafety.h
    发行端试听衰减与最终 PCM 抖动量化。无延迟、无堆分配、无锁。
    调用方先取母带表的读数，再调用本模块；离线渲染 / 宿主旁通 / 关闭窗口时
    由调用方关闭试听。所有方法在音频线程串行使用，UI 读数由调用方发布。

  ==============================================================================
*/

#pragma once

#include <juce_dsp/juce_dsp.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace celadon
{
    class ReleaseSafety
    {
    public:
        void prepare (double sampleRate);
        void reset() noexcept;

        // 试听只衰减；非有限 targetLufs 关闭试听。ditherBits 仅接受 0 / 16 / 24，
        // 其他值按 0（关）处理。reset 保留这些参数，增益与随机序列回到初始状态。
        void setParams (bool previewEnabled, float targetLufs, int ditherBits) noexcept;

        // 支持单声道 / 双声道；其他布局原样返回。读数未知（非有限或 <= -90 LUFS）
        // 时增益以 50 ms 回到 1。TPDF 在舍入前加入，PCM 输出范围为 [-1, 1 - LSB]。
        // 生效的处理路径把非有限音频输入当作 0；关闭且增益归一后的旁通完全不碰缓冲。
        void process (juce::AudioBuffer<float>& io, float integratedLufs) noexcept;

        // Audio-thread handover from another monitor path; avoids summing two independently moving gains.
        void setCurrentPreviewGain (double gain) noexcept;
        double getCurrentPreviewGain() const noexcept { return previewGain.getCurrentValue(); }
        float getPreviewGainDb() const noexcept { return previewGainDb; }
        int getDitherBits() const noexcept      { return ditherBits; }

    private:
        static double nextUniform (std::uint32_t& state) noexcept;

        static constexpr std::array<std::uint32_t, 2> initialRandomStates
        {
            0x6d2b79f5u, 0xa511e9b3u
        };

        std::array<std::uint32_t, 2> randomStates = initialRandomStates;
        juce::SmoothedValue<double> previewGain { 1.0 };
        bool previewEnabled = false;
        float targetLufs = -14.0f;
        int ditherBits = 0;
        float previewGainDb = 0.0f;
    };
}

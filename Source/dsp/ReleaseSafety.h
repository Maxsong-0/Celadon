/*
  ==============================================================================

    ReleaseSafety.h
    发行端试听衰减与最终 PCM 抖动量化。无延迟、无堆分配、无锁。
    调用方先取母带表的读数，再调用本模块；离线渲染 / 宿主旁通 / 关闭窗口时
    由调用方关闭试听。所有方法在音频线程串行使用，UI 读数由调用方发布。

    T-032（2026-09-29，R8-04 / 核验漏项）：
      - 读数暂时未知（界面 Reset 清表后的 400 ms、非有限）而此前已有过有效读数时，预听增益保持上一次的目标不动，
        等下一份有效读数再跟——预听只衰减、永不放大，保持衰减任何时候都不比瞬间回 1（+N dB 爆响 0.4 s）危险；
        从未有过读数、显式 reset()、关掉预听（setParams 传 false）、离线（调用方 reset）仍立即回到 1（50 ms 平滑）。
      - reset() 拆成两半：resetDitherSequence() 只把 TPDF 随机序列回到固定种子（整链 reset / 重启淡入时用，预听增益保留，
        免得同规格 prepareToPlay 后音频回来时斜坡只走了一半）；reset() = 两半都做（prepare、离线、干路、差值、交接给 Listen）。

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

        // 增益立即回到 1、忘掉上一次有效读数、随机序列回到固定种子（参数保留）。
        void reset() noexcept;

        // 只把 TPDF 随机序列回到固定种子：预听增益与上一次有效读数都保留（整链 reset / restartWithFadeIn 用）。
        void resetDitherSequence() noexcept;

        // 试听只衰减；非有限 targetLufs 关闭试听。ditherBits 仅接受 0 / 16 / 24，
        // 其他值按 0（关）处理。关掉试听同时忘掉上一次有效读数（再打开时从头判断）。
        void setParams (bool previewEnabled, float targetLufs, int ditherBits) noexcept;

        // 支持单声道 / 双声道；其他布局原样返回。读数未知（非有限或 <= -90 LUFS）时：此前有过有效读数就保持上一次的目标，
        // 从未有过就以 50 ms 回到 1。TPDF 在舍入前加入，PCM 输出范围为 [-1, 1 - LSB]。
        // 生效的处理路径把非有限音频输入当作 0；关闭且增益归一后的旁通完全不碰缓冲。
        void process (juce::AudioBuffer<float>& io, float integratedLufs) noexcept;

        // Audio-thread handover from another monitor path; avoids summing two independently moving gains.
        void setCurrentPreviewGain (double gain) noexcept;
        double getCurrentPreviewGain() const noexcept { return previewGain.getCurrentValue(); }
        float getPreviewGainDb() const noexcept { return previewGainDb; }
        int getDitherBits() const noexcept      { return ditherBits; }

        // 只给测试：此刻是否还记着一份有效读数算出的目标（读数暂时未知时据此保持）。
        bool holdsLastValidTargetForTesting() const noexcept { return hasValidTarget; }

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
        double lastValidTargetGain = 1.0;   // 最近一次由有效读数算出的目标增益
        bool hasValidTarget = false;        // 自上次 reset / 关预听以来有没有过有效读数
    };
}

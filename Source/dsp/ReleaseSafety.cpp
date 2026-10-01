/*
  ==============================================================================

    ReleaseSafety.cpp

  ==============================================================================
*/

#include "ReleaseSafety.h"

#include <cmath>

namespace celadon
{
    void ReleaseSafety::prepare (double sampleRate)
    {
        // 防止非法采样率使平滑器的样本计数溢出；正常音频采样率保持原值。
        const auto validSampleRate = std::isfinite (sampleRate)
                                  && sampleRate >= 1.0 && sampleRate <= 1000000.0;
        previewGain.reset (validSampleRate ? sampleRate : 48000.0, 0.05);
        reset();
    }

    void ReleaseSafety::reset() noexcept
    {
        previewGain.setCurrentAndTargetValue (1.0);
        previewGainDb = 0.0f;
        lastValidTargetGain = 1.0;
        hasValidTarget = false;
        resetDitherSequence();
    }

    void ReleaseSafety::resetDitherSequence() noexcept
    {
        randomStates = initialRandomStates;
    }

    void ReleaseSafety::setParams (bool enabled, float target, int bits) noexcept
    {
        previewEnabled = enabled && std::isfinite (target);
        targetLufs = std::isfinite (target) ? target : -14.0f;
        ditherBits = bits == 16 || bits == 24 ? bits : 0;

        // 关掉预听就忘掉上一次的读数：下次打开时从「从未有读数 → 1」重新判断，不沿用上一段预听的衰减。
        if (! previewEnabled)
        {
            hasValidTarget = false;
            lastValidTargetGain = 1.0;
        }
    }

    void ReleaseSafety::setCurrentPreviewGain (double gain) noexcept
    {
        const auto safe = std::isfinite (gain) ? juce::jlimit (0.0, 1.0, gain) : 1.0;
        previewGain.setCurrentAndTargetValue (safe);
        previewGainDb = (float) (20.0 * std::log10 (juce::jmax (1.0e-12, safe)));
    }

    double ReleaseSafety::nextUniform (std::uint32_t& state) noexcept
    {
        // xorshift32：每个声道持有独立的非零状态，不使用共享随机数源。
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return static_cast<double> (state >> 8) * (1.0 / 16777216.0);
    }

    void ReleaseSafety::process (juce::AudioBuffer<float>& io, float integratedLufs) noexcept
    {
        const auto numChannels = io.getNumChannels();
        const auto numSamples = io.getNumSamples();
        if (numChannels < 1 || numChannels > 2 || numSamples <= 0)
            return;

        double targetGain = 1.0;
        if (previewEnabled)
        {
            if (! std::isfinite (integratedLufs))
            {
                // 坏读数（非有限）：回到 1，并忘掉上一次的有效读数——之后再来「暂时没有读数」也不会跳回旧的衰减。
                lastValidTargetGain = 1.0;
                hasValidTarget = false;
            }
            else if (integratedLufs > -90.0f)
            {
                // 用 double 相减，极端但有限的输入也不会溢出。禁止正增益；下限只防坏读数。
                const auto attenuationDb = juce::jlimit (-144.0, 0.0,
                    static_cast<double> (targetLufs) - static_cast<double> (integratedLufs));
                targetGain = std::pow (10.0, attenuationDb / 20.0);
                lastValidTargetGain = targetGain;
                hasValidTarget = true;
            }
            else if (hasValidTarget)
            {
                // 暂时没有读数（≤ −90 LUFS：界面 Reset 清表后的前 400 ms）而此前有过有效读数：保持上一次的目标，
                // 不先回 1 再落回来（T-032 R8-04：改前监听 +N dB 爆响约 0.4 s）。预听只衰减、永不放大，保持不比回 1 危险。
                targetGain = lastValidTargetGain;
            }
        }
        previewGain.setTargetValue (targetGain);
        // JUCE 会忽略极近的目标值；归一时仍明确落到 1，保证最终逐位旁通。
        if (targetGain == 1.0 && ! previewGain.isSmoothing())
            previewGain.setCurrentAndTargetValue (1.0);

        // 精确旁通在取得写指针之前返回，保留所有位（也包括 -0 和非有限输入）。
        if (! previewEnabled && ditherBits == 0
            && ! previewGain.isSmoothing() && previewGain.getCurrentValue() == 1.0)
        {
            previewGainDb = 0.0f;
            return;
        }

        auto* const* channels = io.getArrayOfWritePointers();
        const auto quantise = ditherBits != 0;
        const double scale = ditherBits == 16 ? 32768.0 : 8388608.0;
        const double lsb = 1.0 / scale;

        for (int sample = 0; sample < numSamples; ++sample)
        {
            // 每帧只推进一次，左右声道使用完全相同的试听增益。
            const auto gain = juce::jlimit (0.0, 1.0, previewGain.getNextValue());
            if (! previewEnabled && ! quantise && gain == 1.0)
                continue;

            for (int channel = 0; channel < numChannels; ++channel)
            {
                const auto input = channels[channel][sample];
                double output = std::isfinite (input) ? static_cast<double> (input) * gain : 0.0;

                if (quantise)
                {
                    // 两个 U[0,1) 的差产生 ±1 LSB TPDF；先在 double 中相加再显式舍入，
                    // 避免 24 位时先转 float 丢掉抖动，也避免总是向下截断产生的 DC 偏差。
                    auto& state = randomStates[static_cast<std::size_t> (channel)];
                    const auto first = nextUniform (state);
                    const auto second = nextUniform (state);
                    const auto dither = first - second;
                    const auto rounded = std::round (output * scale + dither);
                    output = juce::jlimit (-scale, scale - 1.0, rounded) * lsb;
                }

                // 未量化时增益不超过 1，有限 float 输入不会变成无穷；量化时已钳到 PCM 范围。
                channels[channel][sample] = static_cast<float> (output);
            }
        }

        if (targetGain == 1.0 && ! previewGain.isSmoothing())
            previewGain.setCurrentAndTargetValue (1.0);

        const auto currentGain = juce::jlimit (1.0e-12, 1.0, previewGain.getCurrentValue());
        previewGainDb = static_cast<float> (20.0 * std::log10 (currentGain));
    }
}

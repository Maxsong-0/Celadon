/*
  ==============================================================================

    CrackleTexture.h
    程序生成的开片纹（青瓷釉面的冰裂纹）。固定随机种子 → 每次打开都一样。

    生成开销不在每帧：调用方（背景、GLAZE 旋钮）把图片缓存起来，只在尺寸变化时重生成。

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>

namespace celadon::ui
{
    // 在 area 内生成一组折线裂纹。density = 裂纹条数（背景约 40，旋钮面约 10）。
    juce::Path createCracklePath (juce::Rectangle<float> area, juce::uint32 seed, int density);

    // 渲染成透明底图片：logical 尺寸 width × height，按 pixelScale（显示缩放 × 界面缩放）放大像素，
    // 线色 palette::crackle、不透明度 palette::crackleOpacity、线宽 0.7 逻辑像素。
    juce::Image renderCrackleImage (int width, int height, float pixelScale, juce::uint32 seed, int density);
}

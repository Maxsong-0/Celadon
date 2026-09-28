/*
  ==============================================================================

    Parameters.h
    参数表（APVTS 用）。参数 ID 常量在 ParameterIds.h。

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>

#include "ParameterIds.h"

namespace celadon::param
{
    // 生成交给 AudioProcessorValueTreeState 的完整参数表。
    juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
}

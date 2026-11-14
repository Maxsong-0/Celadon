/*
  ==============================================================================

    LegacyState.h
    读旧工程时对状态树做的迁移（只动 XML，不碰处理器和界面）。

    T-108（ADR-039 Q2、Q3）：T-038 的曲风 / 交付两个参数已从参数表删掉，旧工程的状态树里还留着这两个 PARAM 节点。
    节点留在树里会被 APVTS 原样存回去（下次读档又提示一次），所以读档时摘掉；其中有不是默认值的（下标 0 = 通用 / 标准），
    这个工程的声音会变（House 档的音色、主段模型与限幅跟拍，流媒体的 −2 dBTP、俱乐部的目标响度都没了），界面要提示一次。

    单独放一个头文件，是为了让无头测试（celadon_tests）直接测这条判定：PluginProcessor.cpp 只进插件和截图工具，CI 跑不到它
   （T-108 独立复查 F7）。只依赖 juce_core。

  ==============================================================================
*/

#pragma once

#include <juce_core/juce_core.h>

#include "ParameterIds.h"

#include <cmath>

namespace celadon::legacystate
{
    // 从读进来的状态树（APVTS 根元素）里摘掉曲风 / 交付两个 PARAM 节点（重复的也都摘），返回其中有没有不是默认值的。
    // 旧值是 APVTS 存的选项下标（float 文本，"0" / "1" / "2"）。判定照旧版读回下标的口径：AudioParameterChoice 先把存的值钳到
    // [0, 选项数 − 1]（convertTo0to1），getIndex 再按 juce::roundToInt 取整——所以负数（含 "-inf"）读回仍是默认、旧版声音本来没变，
    // 不提示；超出范围的大数（含 "inf"）读回是最后一档，提示。这里只问「是不是下标 0」，钳到 [0, 1] 结果相同，也免得极大的数取整溢出
    // （T-108 独立复查第 2 轮 R2-F5）。"nan" 读回是哪一档说不准：读不懂的旧档宁可提示一次，也不悄悄变声。
    // 没有 value 属性的节点按 0（默认）读，与 APVTS 缺值时的回退一致。
    inline bool removeGenreDeliveryNodes (juce::XmlElement& stateXml)
    {
        auto nonDefault = false;

        for (auto* node = stateXml.getFirstChildElement(); node != nullptr;)
        {
            auto* next = node->getNextElement();
            const auto id = node->getStringAttribute ("id");

            if (node->hasTagName ("PARAM") && (id == param::legacy::glazeGenre || id == param::legacy::glazeDelivery))
            {
                const auto value = node->getDoubleAttribute ("value", 0.0);
                const auto readsAsDefault = ! std::isnan (value) && juce::roundToInt (juce::jlimit (0.0, 1.0, value)) == 0;
                nonDefault = nonDefault || ! readsAsDefault;
                stateXml.removeChildElement (node, true);
            }

            node = next;
        }

        return nonDefault;
    }
}

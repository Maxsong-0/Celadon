/*
  ==============================================================================

    ChainParams.h
    整条链的用户参数（原 CeladonChain::Params，v0.6 拆出独立头文件）：GLAZE 各单元与链都要用它，
    放在链的头文件里会与「链持有 GLAZE 编排器」形成循环包含。CeladonChain 里以 using Params = ChainParams 保留原名。

    v0.1.7.0 修坯 Clay（设计规格-Clay、ADR-009）：
      - claySettings = 用户在「高级」抽屉里的三态设置与手动值（处理器从 APVTS 读）；
      - clay = 交给 ClayModule 的实际参数。作为 GlazeController::beginBlock 的输入（manual）时忽略，
        由编排器用 glaze::resolveClayParams(claySettings, 自动值, glazePercent) 算出后写进返回的实际参数（GLAZE 为 0 也要算：
        「手动」态在 GLAZE 0 时同样生效）。

  ==============================================================================
*/

#pragma once

#include "BalanceModule.h"
#include "ClayModule.h"
#include "ColorModule.h"
#include "LimiterModule.h"
#include "MasteringFeatures.h"

namespace celadon
{
    // 三态（ADR-009）：顺序 = 参数 clay_*_mode 的选项顺序（AudioParameterChoice 下标：自动 / 关 / 手动）。
    enum class ClayMode : int { automatic = 0, off = 1, manual = 2 };

    struct ClaySettings
    {
        ClayMode lowCutMode = ClayMode::automatic;
        ClayMode monoMode   = ClayMode::automatic;
        ClayMode widthMode  = ClayMode::automatic;
        float lowCutHz = 25.0f;    // 手动值（clay_lowcut_hz，10 ～ 60）
        float monoHz   = 120.0f;   // 手动值（clay_mono_hz，40 ～ 300）
        float widthDb  = 0.0f;     // 手动值（clay_width_db，−3 ～ +3）
        bool  bypass   = false;    // clay_bypass
    };

    struct ChainParams
    {
        BalanceModule::Params balance;
        ColorModule::Params color;
        LimiterModule::Params limit;

        float glazePercent = 0.0f;   // GLAZE 0 ～ 100 %（v0.6 起由 GlazeController 按音乐状态解释，ADR-008；v0.4 的 GlazeMacro 偏移表已退役）
        int glazeStyle = 0;          // glaze::Style 的下标 0 ～ 3（自动 / 温润 / 冲击 / 通透；参数 glaze_style）

        MasteringSettings mastering;
        ClaySettings claySettings;   // v0.1.7.0：用户三态设置
        ClayModule::Params clay;     // v0.1.7.0：交给 ClayModule 的实际参数（见文件头说明）
    };
}

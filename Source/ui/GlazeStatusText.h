/*
  ==============================================================================

    GlazeStatusText.h
    GLAZE 状态行「此刻只显示最要紧的一个」的判定与文字（设计规格-GLAZE-界面与实现 第 1 节状态表）。

    纯函数，只依赖 juce_core 与 dsp/GlazeTypes.h：插件与无头测试都能编（与 HelpText 一样）。
    编辑器定时器每 30 ms 调一次 classify，得到一个量化到显示精度的 View；View 没变就不重拼字符串、不重画。

    优先级（从上到下取第一个成立的；实现与测试都按这张表）：

      GLAZE > 0 时
        1. 受手动设置限制                           琥珀   手动值本身已超约束、自动增量已撤回——根因在手动设置，最该先看见
        2. 方案受限 · 已退让 1.5 dB                 琥珀   保护层退到底仍超约束
        3. 聆听中 9 / 15 s                          梅子青  首次聆听或「重新聆听」的候选版本在建（进行中的事）
        3b. 与聆听时的音乐差很多 · 建议重新聆听      琥珀   已提交画像与此刻的音乐严重不符（如只听到安静前奏），编排器已按
                                                           这一段压低增益（v0.6 验收 F-01；设计规格-GLAZE-实现修订 第 3 节）
        4. 未学习 · 播放后自动聆听（导出将用保守方案） 灰青   ；已按保守方案离线导出过则写「未学习，导出使用保守方案」
        5. 校准暂停（旁通）                          灰青   宿主旁通且此刻本该校准（生成方案后的过渡、校准中、待校准）
        6. 校准中                                   梅子青  方案过渡（约 3 s）与校准都算
        7. 已锁定 · 待校准（条件已变，稳定后自动校准） 灰青
        8. 已锁定 · 自动→温润 · 目标 −9.6 LUFS       灰青   目标写 effectiveTarget；读到旧算法存的方案时风格换成「沿用旧版方案」
           （时间线与保存第 5 节「照用保存的增量…并在界面提示一次」：拧 GLAZE / 换风格 / 重学之后按新算法重算，提示随之消失）

      GLAZE = 0 时只显示学习状态（规格：GLAZE 为 0 时不显示标记，状态行只显示学习状态）
        聆听中 n / N s（梅子青）→ 未学习 · 播放后自动聆听（灰青，不写导出说明：GLAZE 0 导出就是手动处理结果）
        → 已锁定 · 自动→温润（灰青，不写目标：GLAZE 0 不追目标）

    T-038 House 档（设计规格-曲风-House学习方式 第 7、9 节；Status 的 house* 字段由编排器填，这里只按字段渲染。houseActive = false
    时这些字段一律不看，与通用档逐字相同）：
        3'. House · 已听 23 / 64 小节 · 还没听到主段落      梅子青  锁定条件还没满足（houseLockPending）且已开始聆听：取代第 3 行；
            House · 已听 41 / 64 小节 · 主段落已听 8 小节          宿主没给拍点时「House · 已听 52 / 124 秒（宿主没给拍点）」。
                                                                   GLAZE 0 也显示（它是学习状态）
        2'. 俱乐部单声道会丢低频（约 1.6 dB），请回混音修    琥珀   House 反相提示（housePhaseHint）：压过第 1、2、3b 行以外的一切
                                                                   （手动设置、方案受限、画像失配更要紧）；GLAZE 0 不显示（同其他琥珀行）
        暂定期间悬停气泡的第 3 行写「从 drop 开始播放能学得更快」：houseLearningTipApplies 判定，文字在 HelpText houseListeningTooltip。

    原则：要用户动手的（琥珀）＞ 正在进行的（梅子青）＞ 稳态说明（灰青）；琥珀之内先写根因（手动设置）。
    「校准暂停」只在此刻本该校准时才压过别的状态：聆听不受 Listen 影响（分析的是入口），已锁定且校准有效时没有东西被暂停。

  ==============================================================================
*/

#pragma once

#include <juce_core/juce_core.h>

#include "../dsp/GlazeTypes.h"
#include "Localisation.h"

namespace celadon::ui::glazestatus
{
    enum class Kind : int
    {
        unlearned = 0,          // 未学习 · 播放后自动聆听（导出将用保守方案）
        unlearnedExported,      // 未学习，导出使用保守方案（设计规格-GLAZE-时间线与保存 第 7 节）
        listening,              // 聆听中 9 / 15 s
        calibrating,            // 校准中
        calibrationPaused,      // 校准暂停（旁通）/ 校准暂停（未播放）
        awaitingCalibration,    // 已锁定 · 待校准（条件已变，稳定后自动校准）
        locked,                 // 已锁定 · 自动→温润 · 目标 −9.6 LUFS
        planLimited,            // 方案受限 · 已退让 1.5 dB
        limitedByManual,        // 受手动设置限制
        profileMismatch,        // 与聆听时的音乐差很多 · 建议重新聆听（追加在末尾：不改已有枚举值）
        houseListening,         // T-038：House · 已听 23 / 64 小节 · 还没听到主段落（追加在末尾，同上）
        housePhaseHint          // T-038：俱乐部单声道会丢低频（约 1.6 dB），请回混音修
    };

    // 颜色类别：灰青 / 梅子青 / 琥珀（界面按 palette::textDim / accent / reduction 画）。
    enum class Tone : int { dim = 0, accent, amber };

    // 状态行此刻要显示的内容：只含决定文字与颜色的量，已量化到显示精度（整秒、0.1 dB / 0.1 LU），
    // 与这一种情形无关的字段一律清成默认值——两次相等就说明画面上一个像素都不会变。
    struct View
    {
        Kind kind = Kind::unlearned;
        bool glazeActive = false;                               // GLAZE > 0：「未学习」写不写导出说明、「已锁定」写不写目标
        int listenedSeconds = 0;                                // 聆听中：已听整秒数（向下取整，不超过 requiredSeconds）
        int requiredSeconds = 0;                                // 聆听中：这一轮要听满的秒数
        glaze::Style chosenStyle = glaze::Style::automatic;     // 已锁定：参数 glaze_style 选的风格
        glaze::Style resolvedStyle = glaze::Style::automatic;   // 已锁定且选「自动」：此刻偏向哪种
        bool hasTarget = false;                                 // 已锁定：effectiveTarget 可显示（有限值）
        int targetTenthsLufs = 0;                               // 已锁定：effectiveTarget × 10，四舍五入
        int yieldedTenthsDb = 0;                                // 方案受限：|g_保护| × 10，四舍五入，0 ～ 30
        bool pausedByTransport = false;                         // 校准暂停：原因是宿主停止播放（聆听与校准都要有音乐）
        bool olderPlan = false;                                 // 已锁定：照用旧算法保存的方案（GLAZE > 0 时才提示）

        // ---- T-038 House 档 ----
        bool houseBySeconds = false;                            // House 聆听：宿主没给拍点，按秒计（否则按小节）
        int houseHeard = 0;                                     // House 聆听：已听小节（或整秒，向下取整），不超过 houseTarget
        int houseTarget = 0;                                    // House 聆听：锁定所需的小节（或秒）；0 = 编排器没给，只写已听
        int houseFullBars = 0;                                  // House 聆听（按小节）：主段落已听的小节；0 = 还没听到
        int houseLossTenthsDb = 0;                              // House 反相提示：单声道低频损失 × 10（正数；0 = 读不到，不写数）
        Kind underlyingKind = Kind::unlearned;                  // House 反相提示：被它压住的那一行（界面按它画 GLAZE 阶段标签）

        bool operator== (const View& other) const noexcept;
        bool operator!= (const View& other) const noexcept   { return ! operator== (other); }
    };

    // status = 编排器的状态快照；glazePercent = GLAZE 参数值（0 ～ 100，NaN / 负数按 0）；
    // chosenStyle = 参数 glaze_style；listenOn 为调用接口兼容保留，试听补偿不再暂停校准。
    View classify (const glaze::Status& status, float glazePercent, glaze::Style chosenStyle, bool listenOn) noexcept;

    // 文字（数字保留一位小数，负号用 U+2212「−」，与规格的写法一致）。不带语言参数 = 中文（v0.1.8.0 起另有英文）。
    // 各段之间一律用「 · 」连接：界面顶部的状态胶囊按它拆成几段、分别上色（见 segmentsFor）。
    juce::String textFor (const View& view);
    juce::String textFor (const View& view, Language language);

    // 状态胶囊的几段（textFor 按「 · 」拆开）：已锁定一类是「阶段 · 风格 · 目标」，其余多为一两段。
    juce::StringArray segmentsFor (const View& view, Language language);

    Tone toneFor (Kind kind) noexcept;

    // 风格名：自动 / 温润 / 冲击 / 通透（与风格胶囊上的字相同）；英文 Auto / Warm / Punchy / Clean。
    juce::String styleName (glaze::Style style);
    juce::String styleName (glaze::Style style, Language language);

    // 参数 glaze_style 的原始值（选项下标的 float）→ 风格：取整方式同 AudioParameterChoice::getIndex，钳到 0 ～ 3，非有限值按「自动」。
    glaze::Style styleFromParameterValue (float rawValue) noexcept;

    // T-038：House 档锁定条件还没满足（含还没开始播放）时，GLAZE 状态的悬停气泡换成 House 版（第 3 行「从 drop 开始播放能学得更快」）。
    // 与状态行此刻显示哪一行无关：琥珀行压住了聆听进度时，提示照样有用。
    bool houseLearningTipApplies (const glaze::Status& status) noexcept;
}

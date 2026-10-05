/*
  ==============================================================================

    Localisation.h
    界面语言（v0.1.8.0 界面改版：中文 / English 全部支持）。

      - Language：全局只有两种，编辑器顶栏「中 / EN」切换，存在用户设置里（UiSettings），所有实例共用；
      - tr (Text, Language)：界面上所有固定文字（标签、按钮、图例、设置面板……）都从这张表取，界面里不写字面量；
      - 提示气泡的三行文案在 HelpText（按参数 ID 查）、GLAZE 状态行在 GlazeStatusText，两者也都带 Language 参数。

    只依赖 juce_core：插件与无头测试都能编（LocalisationTests 检查每个键两种语言都有、不重复、是合法 UTF-8）。

  ==============================================================================
*/

#pragma once

#include <juce_core/juce_core.h>

#include <initializer_list>
#include <optional>

namespace celadon
{
    // 源码里的中文字面量是 UTF-8 字节。juce::String (const char*) 只接受 ASCII
    // （遇到中文会在 Debug 版断言、显示乱码），所以中文一律经过这个函数转换。
    // （v0.1.8.0 从 HelpText.h 挪到这里：语言表与提示文案都要用，HelpText.h 包含本文件。）
    inline juce::String utf8 (const char* text)
    {
        return juce::String (juce::CharPointer_UTF8 (text));
    }

    enum class Language : int { chinese = 0, english = 1 };

    // 存进用户设置的代号：「zh」「en」。认不出的代号按中文（插件最初只有中文）。
    const char* languageCode (Language language) noexcept;
    Language languageFromCode (juce::StringRef code) noexcept;

    // 没有存过语言时的默认值：系统语言是中文（zh-*）就用中文，否则英文。
    Language defaultLanguageForSystem();
}

namespace celadon::ui
{
    // 界面固定文字的键。顺序无意义；加键时同时在 Localisation.cpp 的表里加一行（测试核对数量与覆盖）。
    enum class Text : int
    {
        // ---- 顶栏 ----
        brandSubtitle, latency, settings, help, close, version, advanced,

        // ---- 电平表 ----
        meterIn, meterOut, meterGr,

        // ---- 频谱仪 ----
        showOutput, showInput, showPeak, slope, cents, markLowCut, markMono,

        // ---- 响度 ----
        loudness, truePeakShort, shortTermShort, peakToLoudness, normalisation, streamingReference, resetMeters,

        // ---- GLAZE 中层 ----
        glazeSubtitle, processed, original, match, matchWithDifference, matchOff, listeningOriginal,
        matchedBy, notMatched, differenceUnknown, drivesParameters, glazeManual, cornerFollowing, legacyDiscarded,
        hintDrag, hintClick, fine,
        legendManual, legendAuto, legendAttention,
        chipManual, chipManualLimit, chipPhase, chipExport, chipLimited, chipStarting, chipHolding, chipFollowing,
        chipAwaiting,

        // ---- 模块卡片 ----
        describeClay, describeBalance, describeColor, describeLimit, bypassModule, bypassedModule,
        clayLowCut, clayMono, clayWidth, modeAuto, modeOff, modeManual, stripStereo, clayNotActive, clayBypassed,
        recover, tame, brighten, speed, speedSlow, speedMid, speedFast, bands27,
        tone, glue, heat, mix, output, glueReduction,
        gain, ceiling, clip, release, lookahead, limitReduction, autoChip, auditioningDifference, auditionDifferenceNote,

        // ---- 高级面板 ----
        comingSoon, subprojectLoudNotHarsh, subprojectTargetSpectrum, subprojectReleaseSafety,
        advancedSubtitle, harshNote, harshStrength, transientPreserve, harshReduction, harshAutoNote, harshOffNote, harshManualNote,
        targetNote, targetWarm, targetNeutral, targetBright, targetReference, targetStrength, targetCurveTitle,
        referenceLearn, referenceCancel, referenceClear, referenceIdle, referenceWaiting, referenceLearning, referenceReady, referenceRouting,
        codecMargin, codecNote, ditherLabel, ditherNote, previewLevel, previewStart, previewStop, previewNote, previewAttenuation,
        releasePreview, returnToMaster, advancedFooter, lowBand, midBand, highBand,

        // ---- 设置面板 ----
        windowSize, screenSize, tooBig, sizeNote, reduceMotion, reduceMotionNote, language,

        // ---- 帮助面板 ----
        helpTitle, helpDrag, helpFine, helpReset, helpType, helpWheel, helpColours, helpAb, helpAudition,

        // ---- 警告 ----
        warnTruePeak, warnNonFinite, debugBuild,

        // ---- 曲风与交付（T-038，GLAZE 标题行左右的两个选择；设计规格-曲风与交付 第 5 节）----
        genreCaption, genreGeneral, genreDeep, genreTech, genreMelodic,
        deliveryCaption, deliveryStandard, deliveryStreaming, deliveryClub,

        numTexts
    };

    // 取文字。键不存在（不该发生）时返回空串。
    juce::String tr (Text key, Language language);

    // 带占位符的文字：把 {0}、{1}……依次换成 arguments。
    juce::String trFormat (Text key, Language language, std::initializer_list<juce::String> arguments);

    // 直接输入数值（Alt 单击）时用户敲的文字规整成半角：中文输入法在中文标点 / 全角模式下「.」出「。」「．」、数字出「１２」、
    // 负号出「－」、逗号出「，」，不换回来的话「3。5」会被当成「35」（Gain 直接顶到上限）。全角数字、全角 / 中文句号、全角逗号、
    // 各种减号与全角加号换成 ASCII，其余原样（单位、空格由解析时再去掉）。Windows 与 macOS 的中文输入法都会这样，界面与测试共用。
    juce::String normaliseTypedNumber (const juce::String& text);

    // 直接输入的数值：先 normaliseTypedNumber，再只看数字、正负号、小数点与逗号（单位、空格都不管）。
    //   - 没有数字、正负号不在最前、两个小数点：说不清是什么数，返回空（不改参数）；
    //   - 逗号：德、法、俄等地区拿它当小数点（「1,5」= 1.5、「-0,3」= −0.3），英文里是千位分隔（「1,000」= 1000，Release 能到
    //     1000 ms）。第一段是不以 0 开头的一到三位数、每个逗号后面正好三位数字时当千位分隔；否则只有一个逗号时当小数点；
    //     其余（包括逗号与小数点混用的「1.000,5」「1,000.5」）返回空。
    std::optional<double> parseTypedNumber (const juce::String& text);

    // 只给测试：表里每一行的两种语言原文（UTF-8 字节）。
    struct TextEntry
    {
        Text key;
        const char* chinese;
        const char* english;
    };

    const TextEntry* textEntries (int& count) noexcept;
}

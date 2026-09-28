/*
  ==============================================================================

    HelpText.cpp
    全部控件的提示文案。随实际处理行为更新，测试 HelpTextTests 检查覆盖与格式。

    只依赖 juce_core（与 dsp/GlazeTypes.h 的纯数据类型）：这个文件同时进插件与无头测试（celadon_tests）编译。
    v0.1.7.0 起文件末尾还有修坯三灯指示的判定与第 2 行实时值（ClayReadout，见头文件说明）；ADR-012 起还有第 3 行的代价说明。

  ==============================================================================
*/

#include "HelpText.h"

#include "../ParameterIds.h"

#include <cmath>
#include <cstdlib>

namespace celadon::help
{
    namespace
    {
        // 文案表：id 用 ParameterIds.h 里的常量，避免手写字符串与参数表脱节。
        // 顺序照规格四张表：LIMIT → COLOR → BALANCE（v0.2 灰显占位）→ 总览区；v0.1.7.0 起末尾追加修坯（高级抽屉的 7 个参数、
        // 箭头上的三灯指示、顶栏「高级」胶囊，设计规格-Clay 第 5、6 节）。
        // IN 表与 OUT 表共用规格里「IN / OUT 表」同一行文字。
        constexpr Entry table[] =
        {
            // ---- LIMIT ----
            { param::limitGain,
              "Gain · 推响度",
              "往限幅器里加增益：越大越响，GR 表压得越深。",
              "从 0 慢慢加，GR 偶尔跳 2～3 dB 就够；推到 −8 LUFS 一般要 +6～+10 dB。" },

            { param::limitCeiling,
              "Ceiling · 输出上限",
              "限幅器按真峰值检测控制输出上限（dBTP）；不同测量器会有微小读数差异。",
              "上流媒体用 −1.0；想更保险用 −1.5。" },

            { param::limitRelease,
              "Release · 释放时间",
              "压下去之后多快回来：短更响但易失真，长更平滑但易泵。Auto 打开时不起作用。",
              "保持 Auto；手动时从 100 ms 起。" },

            { param::limitLookahead,
              "Lookahead · 提前量",
              "提前多久看到峰值：越长越透明，延迟也越大。",
              "2 ms；鼓很密的歌可试 3～5 ms。" },

            { param::limitClip,
              "Clip · 削波",
              "在限幅器前先把尖峰轻轻削圆，同样响度下限幅器压得更少。",
              "0～30 很安全；50 以上听得出颗粒感。" },

            { param::limitReleaseAuto,
              "Auto · 自动释放",
              "快慢双段释放，并参考持续低音的周期；兼顾鼓点恢复与低频平滑。",
              "大多数时候保持打开。" },

            { param::limitListen,
              "Listen · 等响度试听",
              "测量整条处理链的响度提升并衰减试听，减少音量差对判断的影响；输出表显示试听衰减前的母带。",
              "播放数秒后比较；只衰减不加大音量，自动处理与校准照常进行。导出前关闭。" },

            { param::limitBypass,
              "Bypass · 旁通 Limit",
              "只关掉限幅器；延迟不变，切换没有咔哒。",
              "对比限幅前后时用。" },

            // ---- COLOR ----
            { param::colorTone,
              "Tone · 明暗",
              "跷跷板式均衡：往右高频提、低频减（更亮），往左相反（更暖）。",
              "±1 dB 以内就很明显。" },

            { param::colorGlue,
              "Glue · 粘合",
              "总线压缩：把各轨捏在一起、动态更紧；侧链低切减少低频触发的抽吸。",
              "20～40；右上 GR 小表跳 1～3 dB 最自然。" },

            { param::colorHeat,
              "Heat · 温度",
              "保留低频主体的并联电子管染色；两侧采用更轻染色，新增高频谐波过量时自动收敛。",
              "20～50；超过 70 染色明显，可用 Listen 等响试听比较。" },

            { param::colorMix,
              "Mix · 干湿",
              "处理后与原声混合；低于 100% 就是并行处理，保留原来的冲击力。",
              "50～80% 有「厚而不闷」的效果。" },

            { param::colorOutput,
              "Output · 输出微调",
              "调整 Color 送进 Limit 的音量。",
              "让 Color 开关前后音量一致，再判断好坏。" },

            { param::colorBypass,
              "Bypass · 旁通 Color",
              "只关掉 Color；延迟不变，切换没有咔哒。",
              "对比染色前后时用。" },

            // ---- BALANCE（v0.3）----
            { param::balanceRecover,
              "Recover · 找回",
              "把被其他声音遮住的频段提起来，细节和空气感出来。",
              "从 20～40 起；超过 60 可能把底噪也提起来。" },

            { param::balanceTame,
              "Tame · 驯服",
              "把突出刺耳或发闷的频段压下去，箱音和刺耳感减少。",
              "GLAZE 的自动部分保护起音与有明确谐波支持的音色；手动从 30～50 起。" },

            { param::balanceBrighten,
              "Brighten · 倾向",
              "再加一道倾斜：往右高频提、低频压（偏亮），往左相反。",
              "独立生效：整体发闷时往右 20～40，偏刺耳时往左；不必先打开 Recover 或 Tame。" },

            { param::balanceSpeed,
              "Speed · 反应速度",
              "Balance 跟随音乐变化的快慢：Slow 稳、Mid 均衡、Fast 灵敏但可能听出起伏。",
              "大多数歌用 Mid；节奏密的电子乐可试 Fast。" },

            { param::balanceBypass,
              "Bypass · 旁通 Balance",
              "只关掉 Balance；它没有延迟，切换没有咔哒。",
              "对比修正前后时用。" },

            { uiid::balanceCurve,
              "修正曲线",
              "27 个频段此刻被提起（向上、梅子青）或压下（向下、琥珀）了多少 dB。",
              "大部分贴着中线、只在少数频段起伏最自然。" },

            // ---- 总览区 ----
            { param::glazeAmount,
              "Glaze · 施釉（总旋钮）",
              "按歌曲自动推响度、修正持续共振、调整明暗并加入温和染色；达到动态保护上限后停止加压。",
              "新实例默认 35% 和自动风格，播放时持续聆听；按需要增加力度，0% 回到手动。" },

            { param::glazeStyle,
              "风格 · 母带取向",
              "自动按歌选；温润：厚、圆、不刺；冲击：鼓点有力、更响；通透：清楚、少压缩、留动态。",
              "拿不准就用自动，状态行会写出它此刻偏向哪种。" },

            { uiid::glazeRelearn,
              "重新聆听",
              "再听 15～30 秒重新定方案；听的时候旧方案照常生效，听完约 3 秒平滑换过去。",
              "换歌或改了混音后点；聆听中再点一次取消。" },

            { uiid::glazeStatus,
              "GLAZE 状态",
              "显示聆听、校准、目标响度与受限状态；频谱持续跟随，自动染色随段落密度和明亮度缓慢收敛。",
              "受约束表示已自动保护动态；想保留更多起伏就调低 GLAZE。换歌后可重新聆听，更新整首歌的取向。" },

            { uiid::meterIn,
              "输入 / 输出真峰值",
              "8 倍插值测出的真实峰值（dBTP），比采样点峰值更准；下面数字是峰值保持。",
              "点数字清零。OUT 不应超过 Ceiling。" },

            { uiid::meterOut,
              "输入 / 输出真峰值",
              "8 倍插值测出的真实峰值（dBTP），比采样点峰值更准；下面数字是峰值保持。",
              "点数字清零。OUT 不应超过 Ceiling。" },

            { uiid::meterGr,
              "Limit 压缩量",
              "限幅器此刻压了多少 dB。",
              "长时间超过 6 dB 说明 Gain 推太多了。" },

            { uiid::integrated,
              "整体响度（LUFS）",
              "从复位开始累计的平均响度，流媒体按它做音量归一。",
              "流行 −9～−7；想保留动态 −14～−11。" },

            { uiid::shortTerm,
              "短期响度",
              "最近 3 秒的响度，跟着段落变化。",
              "副歌和主歌差 2～4 LU 比较自然。" },

            { uiid::resetMeters,
              "Reset · 重新计量",
              "清零整体响度和所有峰值保持。",
              "换一首歌或从头播放前点一下。" },

            { uiid::glueGr,
              "Glue 压缩量",
              "Color 的 Glue 此刻压了多少 dB。",
              "1～3 dB 最自然。" },

            // ---- 修坯 CLAY（v0.1.7.0，设计规格-Clay 第 5、6 节；ADR-009）----
            // 抽屉里每行 = 名字 + 三态胶囊（*_mode）+ 小旋钮（*_hz / *_db）：胶囊悬停出 *_mode 的三态说明，名字与旋钮出频率 / 增益
            // 本身的作用。三态语义（ADR-009 第 4 条）：自动 = 决策器 + 实时保护，GLAZE 为 0 时不处理；关 = 永远不处理；
            // 手动 = 严格按旋钮值，GLAZE 为 0 也生效、自动与保护都不改它。「自动」时旋钮可拖但不生效，小圆点是此刻的实际值。
            { param::clayLowCutMode,
              "低切 · 自动 / 关 / 手动",
              "自动：只在这首歌真有听不见的次声（16 Hz 以下）、且试过不会抬高峰值才切，播放中若一直抬峰值会自动减量；关：永远不切；手动：按旋钮频率切，GLAZE 为 0 也生效。",
              "一般保持自动；想留住刻意的超低频效果时选「关」。" },

            { param::clayLowCutHz,
              "低切 · 清理超低频",
              "4 阶高通切掉这个频率以下的次声与直流；「自动」时旋钮不生效，小圆点是此刻的实际值。",
              "16～25 Hz 足够；它会转动低频相位，峰值守护会在峰值处自动让一点，不让限幅器多压。" },

            { param::clayMonoMode,
              "收单 · 自动 / 关 / 手动",
              "自动：不收单，只在低频反相（单声道播放会丢低音）时亮琥珀灯提醒；关：不收单、不提醒；手动：旋钮频率以下全部收单。",
              "收单只让立体声的低音更集中，改变不了单声道播放；想要更紧的低音时用手动，从 100～150 Hz 起。" },

            { param::clayMonoHz,
              "收单 · 低频收成单声道",
              "这个频率以下的侧声道收成单声道（线性相位，不转动相位）：低频更紧，单声道回放完全不变；「自动」时旋钮不生效，小圆点是此刻的实际值。",
              "80～150 Hz 常用；超过 200 Hz 会把吉他、键盘的声像一起收窄。" },

            { param::clayWidthMode,
              "加宽 · 自动 / 关 / 手动",
              "自动：只在高频偏窄、且试过不会抬高峰值时适度加宽（最多 +1.5 dB），并盯住相关度防止发虚；关：不加宽；手动：按旋钮值，负值收窄。任何状态下峰值处都会自动让一点。",
              "本来就宽的歌自动不会动；想更宽先试手动 +0.5～+1 dB。" },

            { param::clayWidthDb,
              "加宽 · 高频侧声道",
              "抬高约 2 kHz 以上的侧声道，声像更开阔，单声道回放不变；负值收窄；「自动」时旋钮不生效，小圆点是此刻的实际值。",
              "±1 dB 以内就很明显；超过 +2 dB 容易发虚、相位感变重。" },

            { param::clayBypass,
              "Bypass · 旁通修坯",
              "只关掉修坯（低切、收单、加宽）；延迟不变，切换没有咔哒。",
              "对比修坯前后时用。" },

            // 三灯指示：气泡的第 2 行在界面上换成此刻的实际值（clayIndicatorTooltip），这里的第 2 行是静态说明（无实时数据时的底稿）；
            // 第 3 行照常显示，所以灯色的含义写在第 3 行。
            { uiid::clayIndicator,
              "修坯 · 立体声像与超低频清理",
              "三盏灯自上而下：低切（清超低频）、收单（低频收成单声道）、加宽（窄歌的高频适度加宽）。修坯从不让峰值变高：会抬峰值的瞬间自动让开。",
              "灯自上而下：低切 / 收单 / 加宽；亮 = 在处理；收单灯琥珀 = 低频反相提醒（建议回混音检查），加宽灯琥珀 = 宽度保护介入。想手动覆盖，点顶栏「高级」。" },

            { uiid::advanced,
              "高级 · 手动覆盖",
              "打开下方抽屉：修坯的低切、收单、加宽各自可选自动、关或手动；另外三列即将推出。",
              "默认全自动就好；自动判断不合适时（例如不想要自动加宽）再关掉或改手动。" },
        };

        //==============================================================================
        // 修坯读数的显示精度与上限（上限只防荒唐值撑爆整数与文字：正常低切 10～60 Hz、收单 40～300 Hz、加宽 ±3 dB，远在其下）。
        constexpr int maxShownHz          = 9999;
        constexpr int maxShownPercent     = 100;
        constexpr int maxShownTenthsDb    = 999;    // ±99.9 dB

        // 正的有限值 → 按 unitsPerValue 取整的显示单位（至少 1：实际值非 0，灯亮着，文字不写 0）；0、负数、非有限值 → 0。
        // 先钳再取整：荒唐的大数不会在 roundToInt 里溢出。
        int positiveUnits (float value, float unitsPerValue, int maxUnits) noexcept
        {
            if (! std::isfinite (value) || value <= 0.0f)
                return 0;

            const auto scaled = juce::jmin (static_cast<double> (value) * static_cast<double> (unitsPerValue),
                                            static_cast<double> (maxUnits));
            return juce::jlimit (1, maxUnits, juce::roundToInt (scaled));
        }

        // 带符号的 0.1 精度（加宽 dB）：非 0 的有限值至少 ±1（同上）；0 与非有限值 → 0。
        int signedTenths (float value) noexcept
        {
            if (! std::isfinite (value) || juce::exactlyEqual (value, 0.0f))
                return 0;

            const auto limit  = static_cast<double> (maxShownTenthsDb) / 10.0;
            const auto tenths = juce::roundToInt (juce::jlimit (-limit, limit, static_cast<double> (value)) * 10.0);

            if (tenths != 0)
                return tenths;

            return value > 0.0f ? 1 : -1;
        }

        // 非负的 0.1 精度整数 → 「0.8」「1.5」。
        juce::String formatTenths (int tenths)
        {
            const auto magnitude = std::abs (tenths);
            return juce::String (magnitude / 10) + "." + juce::String (magnitude % 10);
        }
    }

    //==============================================================================
    const juce::Array<Entry>& allEntries()
    {
        // 函数内 static：首次调用时从上面的常量表复制一份，之后只读，多线程首次调用也安全
        // （C++11 起局部静态初始化由编译器加锁）。界面线程只在悬停时来查，不在音频线程用。
        static const juce::Array<Entry> entries (table, juce::numElementsInArray (table));
        return entries;
    }

    const Entry* find (juce::StringRef id)
    {
        // 条目只有几十条、悬停时才查一次，线性查找足够，不值得再建索引。
        for (const auto& entry : allEntries())
            if (id == juce::StringRef (entry.id))
                return &entry;

        return nullptr;
    }

    juce::String tooltipFor (juce::StringRef id)
    {
        const auto* entry = find (id);

        if (entry == nullptr)
            return {};

        // 三行固定用 '\n' 连接；中文字面量必须经 utf8() 转成正确的 Unicode，
        // 否则 juce::String (const char*) 在 Debug 版会断言、Release 版显示乱码。
        return utf8 (entry->title) + "\n" + utf8 (entry->effect) + "\n" + utf8 (entry->advice);
    }

    //==============================================================================
    // 修坯三灯指示（v0.1.7.0，设计规格-Clay 第 5 节）
    //==============================================================================
    bool ClayReadout::operator== (const ClayReadout& other) const noexcept
    {
        return lowCutHz == other.lowCutHz
            && monoHz == other.monoHz
            && monoPercent == other.monoPercent
            && widthTenthsDb == other.widthTenthsDb
            && phaseGuard == other.phaseGuard
            && widthGuard == other.widthGuard
            && needsRelearn == other.needsRelearn
            && bypassed == other.bypassed
            && lowCutCostLimited == other.lowCutCostLimited
            && monoCostLimited == other.monoCostLimited
            && widthCostLimited == other.widthCostLimited;
    }

    ClayReadout clayReadoutFor (const glaze::Status& status, const ClayUserState& user) noexcept
    {
        ClayReadout readout;

        // 旧工程提示与旁通无关（说的是画像），但只在有自动态的项时才有意义：三项都选了「关」或「手动」，重新聆听也不改变声音。
        // 正在重新聆听时已提交的仍是旧画像（clayNeedsRelearn 仍为真），但「点重新聆听」的提示已经没有意义（A6 报告备注）。
        readout.needsRelearn = status.clayNeedsRelearn && ! status.relearnInProgress && (user.lowCutAuto || user.monoAuto || user.widthAuto);

        // 旁通：模块在往干信号淡化，交给它的实际值不代表在处理——全清零（三灯全暗、第 2 行写「修坯已旁通」）。
        // 清零后读数不随被旁通的实际值变动，气泡文字也不会白白重拼。
        if (user.bypassed)
        {
            readout.bypassed = true;
            return readout;
        }

        readout.lowCutHz      = positiveUnits (status.clayLowCutHz, 1.0f, maxShownHz);
        readout.monoPercent   = positiveUnits (status.clayMonoAmount, 100.0f, maxShownPercent);
        readout.monoHz        = readout.monoPercent > 0 ? positiveUnits (status.clayMonoHz, 1.0f, maxShownHz) : 0;
        readout.widthTenthsDb = signedTenths (status.clayWidthDb);

        // 反相一路只是提示（ADR-015：不改收单）；只在收单选「自动」时报（选了「关」或「手动」= 用户自己接管了这一项）。
        // 宽度保护只改「自动」态的项（ADR-009 第 4 条）；选了「关」或「手动」时，即使快照里保护单元仍在跟踪，也不算介入。
        readout.phaseGuard = user.monoAuto && status.clayPhaseGuardActive;
        readout.widthGuard = user.widthAuto && status.clayWidthGuardActive;

        // 代价说明（ADR-012）：同样只算「自动」态的项（编排器已按三态筛过，这里按界面此刻读到的三态再筛一遍，两边口径一致）。
        // 宽度保护正在介入时不写加宽的代价：此刻的实际值是保护定的，第 2 行的「宽度保护中」已经说明了它。收单自动态不做（ADR-015），
        // 决策器不会再置收单的代价位；这里保留判断只为读旧快照时的口径一致。
        const auto mask = status.clayCostLimitedMask;
        readout.lowCutCostLimited = user.lowCutAuto && (mask & glaze::clayCostLimitedLowCut) != 0;
        readout.monoCostLimited   = user.monoAuto && (mask & glaze::clayCostLimitedMono) != 0 && ! readout.phaseGuard;
        readout.widthCostLimited  = user.widthAuto && (mask & glaze::clayCostLimitedWidth) != 0 && ! readout.widthGuard;
        return readout;
    }

    ClayLamp clayLampFor (const ClayReadout& readout, ClayStage stage) noexcept
    {
        switch (stage)
        {
            case ClayStage::lowCut:
                return readout.lowCutHz > 0 ? ClayLamp::on : ClayLamp::off;

            case ClayStage::mono:
                if (readout.phaseGuard)
                    return ClayLamp::guard;

                return readout.monoPercent > 0 ? ClayLamp::on : ClayLamp::off;

            case ClayStage::width:
                if (readout.widthGuard)
                    return ClayLamp::guard;

                return readout.widthTenthsDb != 0 ? ClayLamp::on : ClayLamp::off;
        }

        return ClayLamp::off;
    }

    juce::String clayLiveLine (const ClayReadout& readout)
    {
        if (readout.bypassed)
            return utf8 ("修坯已旁通");

        // 三级按处理顺序（= 三盏灯自上而下）各写一段，再接保护状态；段间「 · 」（规格示例的写法）。
        juce::StringArray parts;

        parts.add (readout.lowCutHz > 0 ? utf8 ("低切 ") + juce::String (readout.lowCutHz) + " Hz"
                                        : utf8 ("未低切"));

        if (readout.monoPercent <= 0)
            parts.add (utf8 ("未收单"));
        else if (readout.monoHz > 0)
            parts.add (juce::String (readout.monoHz) + utf8 (" Hz 以下收单 ") + juce::String (readout.monoPercent) + "%");
        else
            parts.add (utf8 ("收单 ") + juce::String (readout.monoPercent) + "%");   // 分频点不可显示（不该发生）：只写比例

        if (readout.widthTenthsDb > 0)
            parts.add (utf8 ("高频加宽 +") + formatTenths (readout.widthTenthsDb) + " dB");
        else if (readout.widthTenthsDb < 0)
            parts.add (utf8 ("高频收窄 ") + formatTenths (readout.widthTenthsDb) + " dB");   // 只有手动给得出负值
        else
            parts.add (utf8 ("未加宽"));

        if (readout.phaseGuard)
            parts.add (utf8 ("低频反相：单声道播放会丢低音"));

        if (readout.widthGuard)
            parts.add (utf8 ("宽度保护中"));

        return parts.joinIntoString (utf8 (" · "));
    }

    juce::String clayRelearnHint()
    {
        return utf8 ("点「重新聆听」后启用自动修坯");
    }

    juce::String clayCostLine (const ClayReadout& readout)
    {
        // 按处理顺序（= 三盏灯自上而下）。「跳过」= 该项此刻实际值为 0（量化读数：灯暗），否则是「减量」（灯亮着，只是少做了）。
        struct Item
        {
            bool limited;
            bool skipped;
            const char* name;
        };

        const Item items[] =
        {
            { readout.lowCutCostLimited, readout.lowCutHz <= 0,      "低切" },
            { readout.monoCostLimited,   readout.monoPercent <= 0,   "收单" },
            { readout.widthCostLimited,  readout.widthTenthsDb == 0, "加宽" },
        };

        juce::StringArray limitedNames, skippedNames, reducedNames;

        for (const auto& item : items)
        {
            if (! item.limited)
                continue;

            limitedNames.add (utf8 (item.name));
            (item.skipped ? skippedNames : reducedNames).add (utf8 (item.name));
        }

        if (limitedNames.isEmpty())
            return {};

        const auto separator = utf8 ("、");
        const auto line = utf8 ("本曲试过：") + limitedNames.joinIntoString (separator) + utf8 ("会抬高峰值，已自动");

        if (reducedNames.isEmpty())
            return line + utf8 ("跳过");

        if (skippedNames.isEmpty())
            return line + utf8 ("减量");

        // 两种都有：分别点名（「已自动跳过低切，减量收单」）。
        return line + utf8 ("跳过") + skippedNames.joinIntoString (separator)
                    + utf8 ("，减量") + reducedNames.joinIntoString (separator);
    }

    juce::String clayIndicatorTooltip (const ClayReadout& readout)
    {
        const auto* entry = find (uiid::clayIndicator);
        jassert (entry != nullptr);   // 文案表里必须有 ui_clay_indicator（HelpTextTests 也查）

        const auto title = entry != nullptr ? utf8 (entry->title) : juce::String();

        // 第 3 行的优先级（设计规格-Clay-根治修订 3.5）：旧工程的「点重新聆听」> 本曲的代价说明 > 文案表的建议。
        // 旧工程时没有画像特征，也就不会有代价说明；两者同时出现只可能在「重新聆听」提交之前的一瞬，先让人去点。
        juce::String advice;

        if (readout.needsRelearn)
        {
            advice = clayRelearnHint() + utf8 ("。");
        }
        else
        {
            const auto costLine = clayCostLine (readout);
            advice = costLine.isNotEmpty() ? costLine + utf8 ("。")
                                           : (entry != nullptr ? utf8 (entry->advice) : juce::String());
        }

        return title + "\n" + clayLiveLine (readout) + "\n" + advice;
    }
}

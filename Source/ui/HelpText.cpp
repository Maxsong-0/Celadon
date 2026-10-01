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
        // 顺序照规格四张表：LIMIT → COLOR → BALANCE（v0.2 灰显占位）→ 总览区；v0.1.7.0 起末尾追加修坯（7 个参数与三灯指示，
        // 设计规格-Clay 第 5、6 节；v0.1.8.0 起都在 CLAY 卡片上，顶栏「高级」改为打开「高级」面板）。
        // v0.1.8.0 另加 LIMIT 卡片的耳机「试听差值」与顶栏「高级」两个界面元素。
        // IN 表与 OUT 表共用规格里「IN / OUT 表」同一行文字。每条的后三行是英文（v0.1.8.0）。
        constexpr Entry table[] =
        {
            // ---- LIMIT ----
            { param::limitGain,
              "Gain · 推响度",
              "往限幅器里加增益：越大越响，GR 表压得越深。",
              "从 0 慢慢加，GR 偶尔跳 2～3 dB 就够；推到 −8 LUFS 一般要 +6～+10 dB。",
              "Gain · push loudness",
              "Adds gain into the limiter: the more you add, the louder it gets and the deeper the GR meter goes.",
              "Raise it slowly from 0; occasional 2–3 dB of GR is plenty. Reaching −8 LUFS usually takes +6 to +10 dB." },

            { param::limitCeiling,
              "Ceiling · 输出上限",
              "限幅器按真峰值检测控制输出上限（dBTP）；不同测量器会有微小读数差异。",
              "上流媒体用 −1.0；想更保险用 −1.5。",
              "Ceiling · output limit",
              "The limiter keeps the output below this true-peak level (dBTP); other meters may read slightly differently.",
              "Use −1.0 for streaming; −1.5 to be extra safe." },

            { param::limitRelease,
              "Release · 释放时间",
              "压下去之后多快回来：短更响但易失真，长更平滑但易泵。Auto 打开时不起作用。",
              "保持 Auto；手动时从 100 ms 起。",
              "Release · recovery time",
              "How fast the gain comes back after limiting: short is louder but can distort, long is smoother but can pump. Ignored while Auto is on.",
              "Keep Auto; when setting it manually, start at 100 ms." },

            { param::limitLookahead,
              "Lookahead · 提前量",
              "提前多久看到峰值：越长越透明，延迟也越大。",
              "2 ms；鼓很密的歌可试 3～5 ms。",
              "Lookahead · anticipation",
              "How early the limiter sees peaks coming: longer is more transparent but adds latency.",
              "2 ms; songs with dense drums can try 3–5 ms." },

            { param::limitClip,
              "Clip · 削波",
              "在限幅器前先把尖峰轻轻削圆，同样响度下限幅器压得更少。",
              "0～30 很安全；50 以上听得出颗粒感。",
              "Clip · soft clipping",
              "Gently rounds off peaks before the limiter, so it has less to do at the same loudness.",
              "0–30 is safe; above 50 it starts to sound grainy." },

            { param::limitReleaseAuto,
              "Auto · 自动释放",
              "快慢双段释放，并参考持续低音的周期；兼顾鼓点恢复与低频平滑。",
              "大多数时候保持打开。",
              "Auto · automatic release",
              "Two-stage fast / slow release that also follows the period of sustained bass: drums recover quickly, lows stay smooth.",
              "Leave it on most of the time." },

            { param::limitListen,
              "等响 · 等响度试听",
              "测量整条处理链的响度差，把较响的一边衰减到另一边的响度（听处理后时衰减处理结果，听原声时衰减原声），A / B 对比不受音量差影响；输出表显示试听衰减前的母带。",
              "播放数秒后再比较；只衰减不加大音量，自动处理与校准照常进行。导出前关闭。",
              "Match · loudness-matched listening",
              "Measures the loudness difference across the whole chain and turns the louder side down to the other (the processed signal in A, the original in B), so A / B is not biased by level; the meters show the master before this attenuation.",
              "Compare after a few seconds of playback; it only attenuates, never boosts, and automatic processing keeps running. Turn it off before exporting." },

            { param::limitBypass,
              "Bypass · 旁通 Limit",
              "只关掉限幅器；延迟不变，切换没有咔哒。",
              "对比限幅前后时用。",
              "Bypass · Limit",
              "Turns off only the limiter; latency stays the same and switching is click-free.",
              "Use it to compare before and after limiting." },

            { uiid::limitAudition,
              "试听差值 · 只听限幅削掉的部分",
              "送出去的信号换成 LIMIT 削掉的那部分（增益、削波、限幅一起算，按推响前的电平，不会比原声响）；表、响度与 GLAZE 照常按母带算。",
              "用来听有没有削到不该削的（鼓点变扁、低音发闷）；听原声、旁通 LIMIT 或关掉窗口时自动退出。",
              "Listen to difference · hear only what the limiter removes",
              "Replaces the output with what LIMIT takes away (gain, clip and limiting together, at the level before the gain push, never louder than the original); meters, loudness and GLAZE keep measuring the master.",
              "Use it to hear whether anything important is being shaved off (flattened drums, dull bass); it turns off when you switch to the original, bypass LIMIT or close the window." },

            // ---- COLOR ----
            { param::colorTone,
              "Tone · 明暗",
              "跷跷板式均衡：往右高频提、低频减（更亮），往左相反（更暖）。",
              "±1 dB 以内就很明显。",
              "Tone · brightness tilt",
              "A seesaw EQ: to the right it lifts the highs and lowers the lows (brighter), to the left the opposite (warmer).",
              "Within ±1 dB is already clearly audible." },

            { param::colorGlue,
              "Glue · 粘合",
              "总线压缩：把各轨捏在一起、动态更紧；侧链低切减少低频触发的抽吸。",
              "20～40；GR 小表跳 1～3 dB 最自然。",
              "Glue · bus compression",
              "A bus compressor that binds the tracks together and tightens the dynamics; a sidechain low cut reduces bass-triggered pumping.",
              "20–40; 1–3 dB on the Glue GR meter sounds most natural." },

            { param::colorHeat,
              "Heat · 温度",
              "保留低频主体的并联电子管染色；两侧采用更轻染色，新增高频谐波过量时自动收敛。",
              "20～50；超过 70 染色明显，可用「等响」对比。",
              "Heat · warmth",
              "Parallel tube-style colouring that keeps the low-end body; the sides get a lighter touch, and it backs off when new high harmonics get excessive.",
              "20–50; above 70 the colouring is obvious — compare with Match on." },

            { param::colorMix,
              "Mix · 干湿",
              "处理后与原声混合；低于 100% 就是并行处理，保留原来的冲击力。",
              "50～80% 有「厚而不闷」的效果。",
              "Mix · dry / wet",
              "Blends the processed and the original signal; below 100% it becomes parallel processing that keeps the original punch.",
              "50–80% gives a thick but open result." },

            { param::colorOutput,
              "Output · 输出微调",
              "调整 Color 送进 Limit 的音量。",
              "让 Color 开关前后音量一致，再判断好坏。",
              "Output · trim",
              "Adjusts the level Color sends into Limit.",
              "Make Color on and off equally loud before judging it." },

            { param::colorBypass,
              "Bypass · 旁通 Color",
              "只关掉 Color；延迟不变，切换没有咔哒。",
              "对比染色前后时用。",
              "Bypass · Color",
              "Turns off only Color; latency stays the same and switching is click-free.",
              "Use it to compare with and without colouring." },

            // ---- BALANCE（v0.3）----
            { param::balanceRecover,
              "Recover · 找回",
              "把被其他声音遮住的频段提起来，细节和空气感出来。",
              "从 20～40 起；超过 60 可能把底噪也提起来。",
              "Recover · bring back",
              "Lifts bands that are masked by other sounds, bringing out detail and air.",
              "Start at 20–40; above 60 it may lift the noise floor too." },

            { param::balanceTame,
              "Tame · 驯服",
              "把突出刺耳或发闷的频段压下去，箱音和刺耳感减少。",
              "GLAZE 的自动部分保护起音与有明确谐波支持的音色；手动从 30～50 起。",
              "Tame · calm down",
              "Pulls down bands that stick out as harsh or boxy.",
              "GLAZE's automatic part protects attacks and clearly harmonic tones; manually, start at 30–50." },

            { param::balanceBrighten,
              "Brighten · 倾向",
              "再加一道倾斜：往右高频提、低频压（偏亮），往左相反。",
              "独立生效：整体发闷时往右 20～40，偏刺耳时往左；不必先打开 Recover 或 Tame。",
              "Brighten · tilt",
              "An extra tilt: to the right it lifts the highs and lowers the lows (brighter), to the left the opposite.",
              "Works on its own: 20–40 to the right when dull, to the left when harsh; Recover and Tame are not needed." },

            { param::balanceSpeed,
              "Speed · 反应速度",
              "Balance 跟随音乐变化的快慢：Slow 稳、Mid 均衡、Fast 灵敏但可能听出起伏。",
              "大多数歌用 Mid；节奏密的电子乐可试 Fast。",
              "Speed · reaction",
              "How quickly Balance follows the music: Slow is steady, Mid balanced, Fast responsive but its movement may be audible.",
              "Mid for most songs; try Fast for dense electronic music." },

            { param::balanceBypass,
              "Bypass · 旁通 Balance",
              "只关掉 Balance；它没有延迟，切换没有咔哒。",
              "对比修正前后时用。",
              "Bypass · Balance",
              "Turns off only Balance; it has no latency and switches click-free.",
              "Use it to compare before and after correction." },

            { uiid::balanceCurve,
              "修正曲线",
              "27 个频段此刻被提起（向上）或压下（向下）了多少 dB。",
              "大部分贴着中线、只在少数频段起伏最自然。",
              "Correction curve",
              "How many dB each of the 27 bands is being lifted (up) or pulled down (down) right now.",
              "Mostly close to the centre line, with only a few bands moving, is most natural." },

            // ---- 总览区 ----
            { param::glazeAmount,
              "Glaze · 施釉（总旋钮）",
              "按歌曲自动推响度、修正持续共振、调整明暗并加入温和染色；达到动态保护上限后停止加压。",
              "新实例默认 35% 和自动风格，播放时持续聆听；按需要增加力度，0% 回到手动。",
              "Glaze · the master knob",
              "Adapts to the song: pushes loudness, corrects sustained resonances, adjusts brightness and adds gentle colour; it stops pushing at the dynamics protection limit.",
              "New instances start at 35% with Auto style and keep listening during playback; raise it as needed, 0% is fully manual." },

            { param::glazeStyle,
              "风格 · 母带取向",
              "自动按歌选；温润：厚、圆、不刺；冲击：鼓点有力、更响；通透：清楚、少压缩、留动态。",
              "拿不准就用自动，顶部状态会写出它此刻偏向哪种。",
              "Style · mastering direction",
              "Auto picks per song; Warm: thick, round, never harsh; Punchy: strong drums, louder; Clean: clear, less compression, more dynamics.",
              "When unsure, use Auto; the status at the top shows which way it is leaning." },

            { uiid::glazeRelearn,
              "重新聆听",
              "再听 15～30 秒重新定方案；听的时候旧方案照常生效，听完约 3 秒平滑换过去。",
              "换歌或改了混音后点；聆听中再点一次取消。",
              "Relearn",
              "Listens for another 15–30 s to build a new plan; the old plan keeps working meanwhile, then fades over in about 3 s.",
              "Use it after switching songs or changing the mix; click again while listening to cancel." },

            { uiid::glazeStatus,
              "GLAZE 状态",
              "显示聆听、校准、目标响度与受限状态；频谱持续跟随，自动染色随段落密度和明亮度缓慢收敛。",
              "受约束表示已自动保护动态；想保留更多起伏就调低 GLAZE。换歌后可重新聆听，更新整首歌的取向。",
              "GLAZE status",
              "Shows listening, calibration, target loudness and limits; the spectrum keeps following, and the automatic colouring settles slowly with the density and brightness of each section.",
              "“Limited” means the dynamics are being protected; lower GLAZE to keep more movement. Relearn after switching songs to update the plan." },

            { uiid::advanced,
              "高级 · 母带微调",
              "展开推响不刺、目标频谱与发行设置；修坯的低切、收单、加宽仍在下方 CLAY 卡片上。",
              "日常使用 GLAZE 即可；需要更具体的调整或准备最终导出时再展开。",
              "Advanced · mastering detail",
              "Opens harshness control, target spectrum and release settings; low cut, mono and width remain on the CLAY card.",
              "Use GLAZE day to day; open this panel for a specific adjustment or the final export." },

            // ---- 高级面板：参数与界面动作 ----
            { param::masterHarshMode,
              "推响不刺 · 处理方式",
              "通过 BALANCE 抑制刺耳；自动随 GLAZE 缩放，手动用固定力度，关或旁通 BALANCE 时无效。",
              "先用自动；只想比较这项的影响时切到关。",
              "Harshness · mode",
              "Controls harshness through BALANCE: Auto scales with GLAZE, Manual uses a fixed amount. Inactive when Off or BALANCE is bypassed.",
              "Start with Auto; choose Off to compare this stage alone." },
            { param::masterHarshAmount,
              "抑制力度 · 减轻刺耳频段",
              "决定高频刺耳抑制的程度；自动模式还会乘以 GLAZE 的力度，关时无效。",
              "从 50% 起；过量会损失空气感，用等响对比判断。",
              "Amount · ease harsh bands",
              "Sets the depth of high-frequency harshness control; Auto also scales it with GLAZE. Inactive when Off.",
              "Start at 50%; too much can lose air, so compare at matched loudness." },
            { param::masterTransient,
              "瞬态保护 · 保留起音",
              "在检测到起音时减轻刺耳抑制，保留鼓点与拨弦；自动和手动模式都有效。",
              "鼓点发软时提高；高频起音本身刺耳时适当降低。",
              "Transients · preserve attacks",
              "Relaxes harshness reduction on detected attacks to preserve drums and plucks; active in both Auto and Manual.",
              "Raise it if drums soften; lower it if the attacks themselves sound harsh." },
            { param::masterTargetMode,
              "目标频谱 · 整体音色取向",
              "自动随 GLAZE 工作；温暖、均衡、明亮与参考是独立手动目标；BALANCE 旁通时均无效。",
              "没有参考时先用预设；参考目标需先从 DAW 外部侧链学习。",
              "Target spectrum · tonal direction",
              "Auto follows GLAZE; Warm, Neutral, Bright and a learned Reference are independent manual targets. All are inactive if BALANCE is bypassed.",
              "Use a preset first; learn the Reference target from the DAW's external sidechain." },
            { param::masterTargetAmount,
              "匹配力度 · 靠近频谱目标",
              "控制 27 个频带向目标靠近的幅度；图中显示实际目标修正，关闭目标时本项无效。",
              "从较小力度起，保留原曲的配器与音色差异，不必让每个频带完全一致。",
              "Amount · move toward the target",
              "Sets how strongly 27 bands move toward the target; the graph shows the current target correction. Inactive with Target Off.",
              "Start gently and keep the mix's character; every band need not match exactly." },
            { param::masterCodecMarginDb,
              "编码余量 · 降低最终上限",
              "启用 LIMIT 时把输出上限再降低 0～1.5 dB；旁通 LIMIT 时无效，不模拟编码器或检测编码后峰值。",
              "按交付要求预留余量，并在实际编码后用独立工具复测真峰值。",
              "Codec margin · lower the final ceiling",
              "Lowers the ceiling by another 0–1.5 dB when LIMIT is enabled; inactive with LIMIT bypassed. No codec simulation or encoded-peak detection.",
              "Leave the headroom your delivery requires, then remeasure true peaks after the actual encoding." },
            { param::masterDither,
              "抖动 · 最终整数位深",
              "为最终 16 或 24 位整数导出加入抖动；默认关闭，浮点中间文件不需要。",
              "只在处理链末级开启一次；若 DAW 或后续工具负责抖动，这里保持关闭。",
              "Dither · final integer bit depth",
              "Adds dither for the final 16- or 24-bit integer export; Off by default and unnecessary for floating-point intermediates.",
              "Dither once at the end of the chain; leave this Off if the DAW or a later tool handles it." },
            { param::masterPreviewLufs,
              "预听响度 · 归一化监听目标",
              "选择 −24～−8 LUFS 的监听目标；按预听前的 OUT 整体响度只作衰减，不把小音量母带放大。",
              "默认 −14 LUFS；这是通用监听对比，不代表某个平台的精确播放规则。",
              "Preview LUFS · listening target",
              "Chooses a −24 to −8 LUFS listening target using the pre-preview OUT integrated loudness; attenuation only, never boosting a quiet master.",
              "Default −14 LUFS; this is a general listening comparison, not a platform's exact playback policy." },
            { uiid::masterTabs,
              "高级页面 · 逐项微调",
              "三页分别控制推响不刺、目标频谱和发行设置；切换页面不会改变参数。",
              "日常仍可用 GLAZE 一键完成，按需要再展开。",
              "Advanced pages · focused controls",
              "The three pages cover harshness, target spectrum and release settings; changing pages never changes parameters.",
              "Keep using GLAZE for everyday work and open the details only when needed." },
            { uiid::masterPreview,
              "归一化预听 · 只改变监听",
              "按选定 LUFS 衰减监听，不改变母带表头与处理方案；关闭插件窗口时自动退出。",
              "与原声或差值试听互斥；离线导出自动关闭，实时导出前请退出预听。",
              "Normalisation preview · monitor only",
              "Attenuates monitoring to the selected LUFS without changing master meters or the processing plan; stops when the window closes.",
              "Mutually exclusive with original and difference listening; disabled for offline export. Stop preview before real-time export." },
            { uiid::masterLearn,
              "学习参考 · 从外部侧链采集",
              "播放接到 DAW 外部侧链的参考曲，累计 10 秒有效非静音声音后建立目标；参考曲不会混入输出。",
              "接好侧链再点；未收到声音会继续等待，不会假装完成。",
              "Learn reference · capture the external sidechain",
              "Builds a target from 10 seconds of usable, non-silent reference audio on the DAW's external sidechain; never mixes it into the output.",
              "Connect and play the sidechain first; it waits for audio instead of claiming completion." },
            { uiid::masterCancel,
              "取消学习 · 保留旧参考",
              "停止当前等待或学习，丢弃本次未完成的采集，保留之前已经完成的参考。",
              "参考曲放错或需要重新准备侧链时使用。",
              "Cancel learning · keep the previous reference",
              "Stops waiting or learning, discards the incomplete capture and keeps any previously completed reference.",
              "Use if the wrong reference plays or the sidechain needs adjusting." },
            { uiid::masterClear,
              "清除参考 · 移除已学目标",
              "移除已保存的参考频谱；参考模式在没有参考时不施加目标修正。",
              "切换到另一首参考曲时可清除后重新学习；预设目标不受影响。",
              "Clear reference · remove the learned target",
              "Removes the saved reference spectrum; Reference mode applies no target correction without a reference.",
              "Clear and relearn when changing references; preset targets are unaffected." },
            { uiid::masterCurve,
              "目标修正 · 当前 27 频带",
              "从左到右为低频到高频；向上提升、向下衰减，刻度为 ±6 dB；这不是参考曲的原始频谱。",
              "先听整体音色，再看曲线；没有目标或没有有效输入时曲线可以是平的。",
              "Target correction · 27 current bands",
              "Low to high frequencies run left to right; up is boost, down is cut, on a ±6 dB scale. This is not the reference's raw spectrum.",
              "Judge the overall sound first; a flat curve is normal without an active target or usable input." },

            { uiid::meterIn,
              "输入 / 输出真峰值",
              "8 倍插值测出的真实峰值（dBTP），比采样点峰值更准；下面数字是峰值保持。",
              "点数字清零。OUT 不应超过 Ceiling。",
              "Input / output true peak",
              "True peak (dBTP) measured with 8× interpolation, more accurate than sample peaks; the number below is the peak hold.",
              "Click the number to clear it. OUT should not exceed the Ceiling." },

            { uiid::meterOut,
              "输入 / 输出真峰值",
              "8 倍插值测出的真实峰值（dBTP），比采样点峰值更准；下面数字是峰值保持。",
              "点数字清零。OUT 不应超过 Ceiling。",
              "Input / output true peak",
              "True peak (dBTP) measured with 8× interpolation, more accurate than sample peaks; the number below is the peak hold.",
              "Click the number to clear it. OUT should not exceed the Ceiling." },

            { uiid::meterGr,
              "Limit 压缩量",
              "限幅器此刻压了多少 dB。",
              "长时间超过 6 dB 说明 Gain 推太多了。",
              "Limit gain reduction",
              "How many dB the limiter is reducing right now.",
              "Staying above 6 dB for long means Gain is pushed too far." },

            { uiid::integrated,
              "整体响度（LUFS）",
              "从复位开始累计的平均响度，流媒体按它做音量归一。",
              "流行 −9～−7；想保留动态 −14～−11。",
              "Integrated loudness (LUFS)",
              "Average loudness since the last reset; streaming services normalise playback by it.",
              "Pop −9 to −7; to keep dynamics, −14 to −11." },

            { uiid::shortTerm,
              "短期响度",
              "最近 3 秒的响度，跟着段落变化。",
              "副歌和主歌差 2～4 LU 比较自然。",
              "Short-term loudness",
              "Loudness of the last 3 seconds, following the sections of the song.",
              "2–4 LU between verse and chorus sounds natural." },

            { uiid::resetMeters,
              "Reset · 重新计量",
              "清零整体响度和所有峰值保持。",
              "换一首歌或从头播放前点一下。",
              "Reset · measure again",
              "Clears the integrated loudness and every peak hold.",
              "Click it before a new song or before playing from the top." },

            { uiid::glueGr,
              "Glue 压缩量",
              "Color 的 Glue 此刻压了多少 dB。",
              "1～3 dB 最自然。",
              "Glue gain reduction",
              "How many dB Color's Glue is compressing right now.",
              "1–3 dB is most natural." },

            // ---- 修坯 CLAY（v0.1.7.0，设计规格-Clay 第 5、6 节；ADR-009）----
            // v0.1.8.0 起在 CLAY 卡片上：每行 = 灯 + 名字 + 数值 + 三态（*_mode）。数值在「手动」时可拖动（*_hz / *_db 的手动值），
            // 「自动」时显示此刻的实际值、手动值不生效。三态语义（ADR-009 第 4 条）：自动 = 决策器 + 实时保护，GLAZE 为 0 时不处理；
            // 关 = 永远不处理；手动 = 严格按手动值，GLAZE 为 0 也生效、自动与保护都不改它。
            { param::clayLowCutMode,
              "低切 · 自动 / 关 / 手动",
              "自动：只在这首歌真有听不见的次声（16 Hz 以下）、且试过不会抬高峰值才切，播放中若一直抬峰值会自动减量；关：永远不切；手动：按手动频率切，GLAZE 为 0 也生效。",
              "一般保持自动；想留住刻意的超低频效果时选「关」。",
              "Low cut · Auto / Off / Manual",
              "Auto: cuts only when the song really has inaudible infrasound (below 16 Hz) and a trial shows it won't raise peaks, and backs off during playback if it keeps raising them; Off: never cuts; Manual: cuts at your frequency, even at GLAZE 0.",
              "Usually keep Auto; choose Off to keep deliberate sub-bass effects." },

            { param::clayLowCutHz,
              "低切 · 清理超低频",
              "4 阶高通切掉这个频率以下的次声与直流；「自动」时手动值不生效，这里显示的是此刻的实际值。",
              "16～25 Hz 足够；它会转动低频相位，抬高峰值的瞬间峰值守护会把低切让回去，不让限幅器多压。",
              "Low cut · clean up sub-bass",
              "A 4th-order high-pass removes infrasound and DC below this frequency; in Auto your value is not used and the display shows the actual value.",
              "16–25 Hz is enough; it rotates the low-end phase, and whenever that would raise a peak the peak guard yields the low cut so the limiter does not work harder." },

            { param::clayMonoMode,
              "收单 · 自动 / 关 / 手动",
              "自动：不收单，只在低频反相（单声道播放会丢低音）时亮琥珀灯提醒；关：不收单、不提醒；手动：手动频率以下全部收单。",
              "收单只让立体声的低音更集中，改变不了单声道播放；想要更紧的低音时用手动，从 100～150 Hz 起。",
              "Mono · Auto / Off / Manual",
              "Auto: no mono-ing, only an amber warning when the lows are out of phase (bass lost on mono playback); Off: no mono-ing, no warning; Manual: everything below your frequency becomes mono.",
              "Mono-ing only focuses stereo bass and cannot change mono playback; for tighter lows use Manual, starting at 100–150 Hz." },

            { param::clayMonoHz,
              "收单 · 低频收成单声道",
              "这个频率以下的侧声道收成单声道（线性相位，不转动相位）：低频更紧，单声道回放完全不变；「自动」时手动值不生效，这里显示的是此刻的实际值。",
              "80～150 Hz 常用；超过 200 Hz 会把吉他、键盘的声像一起收窄。",
              "Mono · lows to mono",
              "The side signal below this frequency becomes mono (linear phase, no phase rotation): tighter lows, mono playback completely unchanged; in Auto your value is not used and the display shows the actual value.",
              "80–150 Hz is common; above 200 Hz guitars and keys get narrower too." },

            { param::clayWidthMode,
              "加宽 · 自动 / 关 / 手动",
              "自动：只在高频偏窄、且试过不会抬高峰值时适度加宽（最多 +1.5 dB），并盯住相关度防止发虚；关：不加宽；手动：按手动值，负值收窄。加宽不经峰值守护：设多少就是多少。",
              "本来就宽的歌自动不会动；想更宽先试手动 +0.5～+1 dB。",
              "Width · Auto / Off / Manual",
              "Auto: widens a narrow top end moderately (up to +1.5 dB) only when a trial shows it won't raise peaks, watching the correlation so it never sounds hollow; Off: no widening; Manual: your value, negative narrows. Width is not run through the peak guard: what you set is what you get.",
              "Songs that are already wide are left alone; to go wider, try Manual at +0.5 to +1 dB." },

            { param::clayWidthDb,
              "加宽 · 高频侧声道",
              "抬高约 2 kHz 以上的侧声道，声像更开阔，单声道回放不变；负值收窄；「自动」时手动值不生效，这里显示的是此刻的实际值。",
              "±1 dB 以内就很明显；超过 +2 dB 容易发虚、相位感变重。",
              "Width · high-frequency side",
              "Raises the side signal above about 2 kHz for a wider image, mono playback unchanged; negative narrows; in Auto your value is not used and the display shows the actual value.",
              "Within ±1 dB is clearly audible; above +2 dB it easily sounds hollow and phasey." },

            { param::clayBypass,
              "Bypass · 旁通修坯",
              "只关掉修坯（低切、收单、加宽）；延迟不变，切换没有咔哒。",
              "对比修坯前后时用。",
              "Bypass · Clay",
              "Turns off only Clay (low cut, mono, width); latency stays the same and switching is click-free.",
              "Use it to compare with and without Clay." },

            // 三灯指示（v0.1.8.0 起是 CLAY 卡片每行的灯）：气泡第 2 行在界面上换成此刻的实际值（clayIndicatorTooltip），这里的第 2 行
            // 是静态说明（无实时数据时的底稿）；第 3 行照常显示，所以灯色的含义写在第 3 行。
            { uiid::clayIndicator,
              "修坯 · 立体声像与超低频清理",
              "三盏灯自上而下：低切（清超低频）、收单（低频收成单声道）、加宽（窄歌的高频适度加宽）。低切与收单从不让峰值变高：会抬峰值的瞬间自动让开；加宽不经守护，自动加宽由先试后用把关。",
              "亮 = 在处理（紫色 = 自动，青色 = 手动）；收单灯琥珀 = 低频反相提醒（建议回混音检查），加宽灯琥珀 = 宽度保护介入。想手动覆盖，把该行切到「手动」。",
              "Clay · stereo image and sub-bass cleanup",
              "Three lamps from the top: low cut (clears sub-bass), mono (lows to mono), width (moderate widening of narrow highs). Low cut and mono never make peaks higher: they step aside at moments that would; width is not guarded, and automatic width is kept in check by the trial.",
              "Lit = processing (violet = auto, cyan = manual); amber mono = out-of-phase lows warning (check the mix), amber width = width guard engaged. To override, switch a row to Manual." },
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
        return tooltipFor (id, Language::chinese);
    }

    juce::String tooltipFor (juce::StringRef id, Language language)
    {
        const auto* entry = find (id);

        if (entry == nullptr)
            return {};

        // 三行固定用 '\n' 连接；中文字面量必须经 utf8() 转成正确的 Unicode，
        // 否则 juce::String (const char*) 在 Debug 版会断言、Release 版显示乱码。英文里也有「·」「–」「×」，同样经 utf8()。
        if (language == Language::english)
            return utf8 (entry->titleEn) + "\n" + utf8 (entry->effectEn) + "\n" + utf8 (entry->adviceEn);

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

    juce::String clayLiveLine (const ClayReadout& readout, Language language)
    {
        const auto english = language == Language::english;

        if (readout.bypassed)
            return english ? juce::String ("Clay bypassed") : utf8 ("修坯已旁通");

        // 三级按处理顺序（= 三盏灯自上而下）各写一段，再接保护状态；段间「 · 」（规格示例的写法）。
        juce::StringArray parts;

        if (english)
        {
            parts.add (readout.lowCutHz > 0 ? "Low cut " + juce::String (readout.lowCutHz) + " Hz" : juce::String ("No low cut"));

            if (readout.monoPercent <= 0)
                parts.add ("No mono");
            else if (readout.monoHz > 0)
                parts.add ("Mono " + juce::String (readout.monoPercent) + "% below " + juce::String (readout.monoHz) + " Hz");
            else
                parts.add ("Mono " + juce::String (readout.monoPercent) + "%");

            if (readout.widthTenthsDb > 0)
                parts.add ("High width +" + formatTenths (readout.widthTenthsDb) + " dB");
            else if (readout.widthTenthsDb < 0)
                parts.add ("High width " + utf8 ("\xe2\x88\x92") + formatTenths (readout.widthTenthsDb) + " dB");
            else
                parts.add ("No widening");

            if (readout.phaseGuard)
                parts.add ("Lows out of phase: bass is lost on mono playback");

            if (readout.widthGuard)
                parts.add ("Width guard engaged");

            return parts.joinIntoString (utf8 (" \xc2\xb7 "));
        }

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

    juce::String clayRelearnHint (Language language)
    {
        if (language == Language::english)
            return utf8 ("Click \xe2\x80\x9cRelearn\xe2\x80\x9d to enable automatic Clay");

        return utf8 ("点「重新聆听」后启用自动修坯");
    }

    juce::String clayCostLine (const ClayReadout& readout, Language language)
    {
        const auto english = language == Language::english;

        // 按处理顺序（= 三盏灯自上而下）。「跳过」= 该项此刻实际值为 0（量化读数：灯暗），否则是「减量」（灯亮着，只是少做了）。
        struct Item
        {
            bool limited;
            bool skipped;
            const char* name;
            const char* nameEn;
        };

        const Item items[] =
        {
            { readout.lowCutCostLimited, readout.lowCutHz <= 0,      "低切", "low cut" },
            { readout.monoCostLimited,   readout.monoPercent <= 0,   "收单", "mono" },
            { readout.widthCostLimited,  readout.widthTenthsDb == 0, "加宽", "width" },
        };

        juce::StringArray limitedNames, skippedNames, reducedNames;

        for (const auto& item : items)
        {
            if (! item.limited)
                continue;

            const auto name = utf8 (english ? item.nameEn : item.name);
            limitedNames.add (name);
            (item.skipped ? skippedNames : reducedNames).add (name);
        }

        if (limitedNames.isEmpty())
            return {};

        if (english)
        {
            // 「Tried on this song: low cut, mono raise peaks — skipped low cut, reduced mono」
            const auto line = "Tried on this song: " + limitedNames.joinIntoString (", ") + " would raise peaks, ";

            if (reducedNames.isEmpty())
                return line + "skipped automatically";

            if (skippedNames.isEmpty())
                return line + "reduced automatically";

            return line + "skipped " + skippedNames.joinIntoString (", ") + ", reduced " + reducedNames.joinIntoString (", ");
        }

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

    juce::String clayIndicatorTooltip (const ClayReadout& readout, Language language)
    {
        const auto english = language == Language::english;
        const auto* entry = find (uiid::clayIndicator);
        jassert (entry != nullptr);   // 文案表里必须有 ui_clay_indicator（HelpTextTests 也查）

        const auto title = entry != nullptr ? utf8 (english ? entry->titleEn : entry->title) : juce::String();
        const auto fullStop = english ? juce::String (".") : utf8 ("。");

        // 第 3 行的优先级（设计规格-Clay-根治修订 3.5）：旧工程的「点重新聆听」> 本曲的代价说明 > 文案表的建议。
        // 旧工程时没有画像特征，也就不会有代价说明；两者同时出现只可能在「重新聆听」提交之前的一瞬，先让人去点。
        juce::String advice;

        if (readout.needsRelearn)
        {
            advice = clayRelearnHint (language) + fullStop;
        }
        else
        {
            const auto costLine = clayCostLine (readout, language);
            advice = costLine.isNotEmpty() ? costLine + fullStop
                                           : (entry != nullptr ? utf8 (english ? entry->adviceEn : entry->advice) : juce::String());
        }

        return title + "\n" + clayLiveLine (readout, language) + "\n" + advice;
    }

    juce::String clayStageValue (const ClayReadout& readout, ClayStage stage)
    {
        if (readout.bypassed)
            return {};

        switch (stage)
        {
            case ClayStage::lowCut:
                return readout.lowCutHz > 0 ? juce::String (readout.lowCutHz) + " Hz" : juce::String();

            case ClayStage::mono:
                if (readout.monoPercent <= 0 || readout.monoHz <= 0)
                    return {};

                return readout.monoPercent >= 100 ? juce::String (readout.monoHz) + " Hz"
                                                  : juce::String (readout.monoHz) + utf8 (" Hz \xc2\xb7 ") + juce::String (readout.monoPercent) + "%";

            case ClayStage::width:
                if (readout.widthTenthsDb == 0)
                    return {};

                return (readout.widthTenthsDb > 0 ? juce::String ("+") : utf8 ("\xe2\x88\x92")) + formatTenths (readout.widthTenthsDb) + " dB";
        }

        return {};
    }
}

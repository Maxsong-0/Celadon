#!/usr/bin/env python3
"""T-071：T-060 拍板档第 6 条与「范围外顺带发现」——v0.1.8.0 界面改版遗留的过期叫法与几处与代码不符的注释，**只改注释**。
每条改动先断言原文在目标文件里恰好出现一次；用法：在仓库根执行 python3 docs/validation/2026-10-04-T071-clay-comment-cleanup-2/apply_edits.py
叫法约定：Clay 三项的默认设置（三态都「自动」）原先叫「抽屉默认」（v0.1.7.0 时设置在「高级」抽屉里），v0.1.8.0 起设置在 CLAY 卡片上，统一叫「三态默认」；
引号里引用设计规格原文的（「GLAZE 0 且抽屉默认」等）不动；字符串字面量（--help、测试消息）不在本任务范围。"""
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[3]
EDITS = []


def sub(path, old, new):
    EDITS.append((path, old, new))


# ---- 「抽屉」叫法 → CLAY 卡片 / 三态默认
sub("Source/Parameters.cpp", "；抽屉里的三态胶囊显示中文「自动 / 关 / 手动」", "；CLAY 卡片上的三态胶囊显示中文「自动 / 关 / 手动」")
sub("Source/Parameters.cpp", "// 全部在「高级」抽屉里、都可自动化，追加在参数表末尾", "// 全部在 CLAY 卡片上（v0.1.7.0 时在「高级」抽屉里）、都可自动化，追加在参数表末尾")
sub("Source/PluginProcessor.h", "修坯指示灯与抽屉的小圆点读它", "CLAY 卡片每行的灯与数值读它")
sub("Source/PluginProcessor.cpp", "这里只读用户在「高级」抽屉里的设置：三个三态按下标转枚举", "这里只读用户在 CLAY 卡片上的设置：三个三态按下标转枚举")
sub("Source/PluginProcessor.cpp", "不沿用读之前抽屉里的「手动 / 关」", "不沿用读之前 CLAY 卡片上的「手动 / 关」")
sub("Source/dsp/ClayGuard.cpp", "验收第 1 条：GLAZE 0 且抽屉默认 / 三项都关 / 旧工程未重新聆听）", "验收第 1 条：GLAZE 0 且三态默认 / 三项都关 / 旧工程未重新聆听）")
sub("Source/dsp/ClayGuard.cpp", "这里钳是为了让 Status、执行快照与抽屉的实际值小圆点看到的就是模块真正用的值）", "这里钳是为了让 Status、执行快照与 CLAY 卡片每行的灯和数值看到的就是模块真正用的值）")
sub("Source/dsp/ClayModule.h", "      GLAZE 0 且抽屉默认 → Clay 是 D + G 样本纯延迟：整链 = 旧链在 Clay 的位置插一段 D + G 样本纯延迟", "      GLAZE 0 且三态默认 → Clay 是 D + G 样本纯延迟：整链 = 旧链在 Clay 的位置插一段 D + G 样本纯延迟")
sub("Source/dsp/CeladonChain.h", "GLAZE 0 且抽屉默认（三态都「自动」）时 Clay 三项实际值都是 0", "GLAZE 0 且三态默认（都「自动」）时 Clay 三项实际值都是 0")
sub("Source/dsp/CeladonChain.h", "claySettings 是「高级」抽屉里的三态设置与手动值", "claySettings 是 CLAY 卡片上的三态设置与手动值")
sub("Source/dsp/ChainParams.h", "claySettings = 用户在「高级」抽屉里的三态设置与手动值", "claySettings = 用户在 CLAY 卡片上的三态设置与手动值")
sub("Source/dsp/GlazeTypes.h", "ClayIndicator 与抽屉的实际值小圆点读这些", "ClayIndicator 与 CLAY 卡片每行的灯和数值读这些")
sub("Source/dsp/GlazeController.cpp", "修坯 Clay（ClayIndicator 与抽屉的实际值小圆点）", "修坯 Clay（ClayIndicator 与 CLAY 卡片每行的灯和数值）")
sub("tests/ClayGuardTests.cpp", "最要紧的两条单独再写一遍：抽屉默认 + GLAZE 0 → 全中性", "最要紧的两条单独再写一遍：三态默认 + GLAZE 0 → 全中性")
sub("tests/ClayGlazeTests.cpp", "      1. GLAZE 0：抽屉默认（三态都「自动」）、三项全「关」", "      1. GLAZE 0：三态默认（都「自动」）、三项全「关」")
sub("tests/ClayGlazeTests.cpp", "「GLAZE 0 且抽屉默认 → 整链与改动前逐位相同」", "「GLAZE 0 且三态默认 → 整链与改动前逐位相同」")
sub("tests/ClayGlazeTests.cpp", "a. 抽屉默认（三态都「自动」）；b. 三项全「关」", "a. 三态默认（都「自动」）；b. 三项全「关」")
sub("tests/ClayGlazeTests.cpp", "a. 抽屉默认、GLAZE 60 读入", "a. 三态默认、GLAZE 60 读入")
sub("tests/ClayGlazeTests.cpp", "默认抽屉不改参考条件 → 打开后声音不变", "三态默认不改参考条件 → 打开后声音不变")
sub("tests/ClayGlazeTests.cpp", "（两条链都没有依赖手动参数哈希的已存状态），抽屉默认 vs 全关，锁步 4 s：", "（两条链都没有依赖手动参数哈希的已存状态），三态默认 vs 全关，锁步 4 s：")
sub("tests/ClayGlazeTests.cpp", "// ---- a. 抽屉默认读旧存档 ----", "// ---- a. 三态默认读旧存档 ----")
sub("tests/ClayGlazeTests.cpp", "// ---- c. 逐位对照：抽屉默认 vs 全关（不带校准与执行快照的旧存档）----", "// ---- c. 逐位对照：三态默认 vs 全关（不带校准与执行快照的旧存档）----")
sub("tests/ClayGlazeTests.cpp", "低切「手动 30 Hz」vs 抽屉默认，各 4 s，跳过前 1 s：", "低切「手动 30 Hz」vs 三态默认，各 4 s，跳过前 1 s：")
sub("tests/ClayGlazeTests.cpp", "加宽「手动 +3 dB」vs 抽屉默认，各 4 s，跳过前 1 s：", "加宽「手动 +3 dB」vs 三态默认，各 4 s，跳过前 1 s：")
sub("tests/ClayGlazeTests.cpp", "//     GLAZE 60、抽屉默认，从第一块起数", "//     GLAZE 60、三态默认，从第一块起数")
sub("tests/ClayGlazeTests.cpp", "GLAZE 60、抽屉默认，只在 drop 段上循环学完并锁定", "GLAZE 60、三态默认，只在 drop 段上循环学完并锁定")
sub("tests/ChainTests.cpp", "// 三项都「手动」的抽屉设置（Clay Bypass 关）。", "// 三项都「手动」的设置（Clay Bypass 关）。")
sub("tests/ChainTests.cpp", "逐位相同（memcmp，连 ±0）：抽屉默认（三态都「自动」）；", "逐位相同（memcmp，连 ±0）：三态默认（都「自动」）；")
sub("tests/ChainTests.cpp", "抽屉默认与三项「手动」全开两种设置下，链的延迟都等于", "三态默认与三项「手动」全开两种设置下，链的延迟都等于")
sub("tests/ChainTests.cpp", "// 0 = 抽屉默认；1 = 三项「手动」全开", "// 0 = 三态默认；1 = 三项「手动」全开")
sub("tests/HelpTextTests.cpp", "设计规格-Clay 第 5 节（抽屉每行的名字 低切 / 收单 / 加宽", "设计规格-Clay 第 5 节（CLAY 卡片每行的名字 低切 / 收单 / 加宽")
sub("tests/HelpTextTests.cpp", "三项实际值都为 0（GLAZE 0 且抽屉默认、旧工程）", "三项实际值都为 0（GLAZE 0 且三态默认、旧工程）")
sub("tests/GlazePlannerTests.cpp", "决策器不看抽屉设置）。所以：① 手动值、抽屉设置、约束反馈 maxGainDb 都不改变 Clay", "决策器不看三态设置）。所以：① 手动值、三态设置、约束反馈 maxGainDb 都不改变 Clay")
sub("tools/render/RenderMain.cpp", "// 修坯的对照：自动 = 抽屉默认（三项都「自动」）", "// 修坯的对照：自动 = 三态默认（三项都「自动」）")
sub("tools/render/RenderMain.cpp", "// 「全关」= 抽屉里三项都选「关」", "// 「全关」= 三项都选「关」")
sub("tools/render/README.md", "**修坯「自动」**= 高级抽屉的默认（", "**修坯「自动」**= 三态默认（")

# ---- 与代码不符的注释
sub("Source/ui/HelpText.cpp", "各写一段，再接保护状态；段间「 · 」（规格示例的写法）。", "各写一段，再接低频反相提示与宽度保护两段（亮着才写）；段间「 · 」（规格示例的写法）。")
sub("Source/ui/ModuleCards.h", "每帧调：编排器快照里的 Clay 实际值与保护状态（已按三态与旁通算好的读数）。", "每帧调：编排器快照里的 Clay 实际值与低频反相提示、宽度保护的状态（已按三态与旁通算好的读数）。")
sub("Source/ui/GlazeStatusText.h", "    // 颜色类别：灰青 / 梅子青 / 琥珀（界面按 palette::textDim / accent / reduction 画）。\n",
    "    // 颜色类别：灰青（稳态说明）/ 梅子青（进行中）/ 琥珀（要用户动手）。编辑器按类别用：琥珀 → 状态胶囊的警告样式（PluginEditor：toneFor (kind) == amber）；\n"
    "    // 梅子青那三种（聆听中 / 校准中 / House 聆听中）→ 圆点呼吸（pill.pulse，按 Kind 判）；灰青 = 胶囊的常规样式。\n")
sub("tests/GlazeControllerTests.cpp", "        // Clay 6. 执行快照：保存 liveClay", "        // Clay 7. 执行快照：保存 liveClay")
sub("Source/ui/HelpText.h", "    // ADR-012 的代价说明（设计规格-Clay-根治修订 3.5）：「本曲试过：{低切 / 收单 / 加宽}会抬高峰值，已自动{减量 / 跳过}」，\n",
    "    // ADR-012 的代价说明（设计规格-Clay-根治修订 3.5）：「本曲试过：{低切 / 收单 / 加宽}会抬高峰值，已自动{减量 / 跳过}」（含低切时写\n"
    "    // 「会抬高峰值或削弱低音」：T-043 / ADR-027，守护的混合损失把低切封顶），\n")
sub("Source/ui/HelpText.h", "    // 否则实际值非 0 → 梅子青；否则暗。旁通时 readout 里全是 0 → 全暗。\n",
    "    // 否则实际值非 0 → 亮（「自动」紫圈、「手动」青色实心，画法见 ModuleCards.cpp）；否则暗。旁通时 readout 里全是 0 → 全暗。\n")
sub("tests/HelpTextTests.cpp", "实际值非 0 → 梅子青（on）", "实际值非 0 → 亮（on，「自动」紫圈、「手动」青色实心）")
sub("tests/HelpTextTests.cpp", "实际值仍非 0 → 亮（梅子青），不是琥珀", "实际值仍非 0 → 亮（「手动」是青色实心），不是琥珀")
sub("Source/dsp/ClayGuard.cpp",
    "门 0 = 快路只在慢路已低于 0 时参与，最短持续 0 = 条件一成立就攻击——比出厂更敏感、但不失控，",
    "门 0 = 快路只在慢路已低于 0 时参与（比出厂 0.1 更不爱触发），最短持续 0 = 条件一成立就攻击（比出厂 0.15 s 更爱触发）——合起来不失控，")
sub("tests/ClayGlazeTests.cpp", "        // 五个代价里最大的（dB；都无效时 −∞）。", "        // 画像里各个代价里最大的（dB；都无效时 −∞）。")
sub("tests/ClayGlazeTests.cpp", "画像的五个代价都有效且 ≤ clayCostFullDb（0.2 dB）", "画像的各个代价都有效且 ≤ clayCostFullDb（0.2 dB）")


def main():
    by_file = {}
    for path, old, new in EDITS:
        by_file.setdefault(path, []).append((old, new))
    failures, updated = [], {}
    for path, edits in by_file.items():
        text = (ROOT / path).read_text(encoding="utf-8")
        for old, new in edits:
            n = text.count(old)
            if n != 1:
                failures.append(f"{path}: 原文出现 {n} 次（应为 1）：{old[:70]!r}")
                continue
            text = text.replace(old, new)
        updated[path] = text
    if failures:
        print("\n".join(failures))
        sys.exit(1)
    # 「Clay 5」那一段注释说的是 testClayGuardThroughController，却压在 testClayTransitionBlend 的末尾、隔着「Clay 6」「Clay 6b」两段：挪到它自己的函数上面
    gct = "tests/GlazeControllerTests.cpp"
    gtext = updated[gct]
    block_start = "        // Clay 5. 实时反相提示与宽度保护经编排器生效（分析器看真实素材 → ClayGuard → 三态合成）：\n"
    block_end = "收单不动、灯只报自动态」与宽度保护「会介入、会退回」，精确时序由 ClayGuard / ClayGlaze 测试核对。\n"
    a = gtext.index(block_start)
    b = gtext.index(block_end, a) + len(block_end)
    assert gtext.count(block_start) == 1 and gtext.count(block_end) == 1 and b - a < 2000
    block = gtext[a:b]
    assert block.count("\n") == 8, block.count("\n")
    gtext = gtext[:a] + gtext[b:]
    anchor = "        void testClayGuardThroughController()\n"
    assert gtext.count(anchor) == 1
    gtext = gtext.replace(anchor, block + anchor)
    updated[gct] = gtext
    for path, text in updated.items():
        (ROOT / path).write_text(text, encoding="utf-8")
    print(f"套用 {len(EDITS)} 条改动，涉及 {len(updated)} 个文件")


main()

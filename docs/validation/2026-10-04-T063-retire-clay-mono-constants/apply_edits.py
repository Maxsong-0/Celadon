#!/usr/bin/env python3
"""T-063：退役 T-057 留下的 7 个自动收单常数（clayMonoBandSideRatioDb、clayMonoBandCorrelation、clayMonoMinHz、clayMonoMaxHz、
clayMonoDefaultHz、clayMonoLossLowDb、clayMonoLossHighDb）连标定快照字段（Max 2026-10-04 19:4x 选 B「现在退役」；方案见 vault
04-协调/2026-10-04-T-057-收单遗留常数退役待Max拍板）：删 7 个成员与 7 行字段表，文件头登记「不再写、残留读时忽略、名字不得复用」，
快照 version 不升（并入未发布的 9）、constantsFormatVersion 不升；测试只动 GlazePlannerTests.cpp（含新增「旧存档带着这 7 个属性读时忽略」一组）。
每条先断言 old / 起止标记恰好出现一次（对不上就停，一个文件都不写）。用法（仓库根）：python3 <本文件> [--dry-run]"""
import pathlib, sys

dry = "--dry-run" in sys.argv
H = pathlib.Path("Source/dsp/GlazeConstants.h")
C = pathlib.Path("Source/dsp/GlazeConstants.cpp")
T = pathlib.Path("tests/GlazePlannerTests.cpp")
texts = {p: p.read_text(encoding="utf-8") for p in (H, C, T)}
log = []

def sub(path, name, old, new):
    t = texts[path]
    n = t.count(old)
    log.append((n == 1, f"{path.name}: {name}（匹配 {n} 处）"))
    if n == 1:
        texts[path] = t.replace(old, new, 1)

def sub_range(path, name, first, last, new):
    """把「以 first 开头的那一行」到「之后第一行以 last 开头的行」整段（含两端）换成 new（多行字符串，可为空 = 删除）。"""
    lines = texts[path].split("\n")
    starts = [i for i, l in enumerate(lines) if l.lstrip().startswith(first)]
    ok = len(starts) == 1
    if ok:
        i = starts[0]
        ends = [j for j in range(i, len(lines)) if lines[j].lstrip().startswith(last)]
        ok = len(ends) >= 1
    log.append((ok, f"{path.name}: {name}（起点 {len(starts)} 处）" + (f"，{ends[0] - i + 1} 行" if ok else "")))
    if ok:
        lines[i:ends[0] + 1] = new.split("\n") if new else []
        texts[path] = "\n".join(lines)

# ===================== GlazeConstants.h =====================
sub_range(H, "7 个成员连说明", "// 自动收单的 7 个常数——逐段选点阈值", "float clayMonoDefaultHz = 80.0f;",
"""        // 已退役的 7 个自动收单常数（T-063，2026-10-04）：clayMonoBandSideRatioDb、clayMonoBandCorrelation、clayMonoMinHz、clayMonoMaxHz、
        // clayMonoDefaultHz、clayMonoLossLowDb、clayMonoLossHighDb。ADR-015（0.1.7.1）起自动态不收单，决策器不再读它们；现在连成员带
        // 快照字段一起删了：新存档不再写这 7 个属性，旧存档里残留的读时忽略（读取只认字段表里的名字）。名字不得复用——旧存档里带着它们，
        // 复用会把旧值读进新字段（GlazePlannerTests 的「旧存档带着 7 个自动收单属性」用例守着）。现行自动方案收单比例恒为 0、分频点恒为
        // ClayPlan{} 的 80 Hz。更早（ADR-012）删掉的 clayMonoSideRatioLowDb / HighDb、clayMonoCorrelationLow / High 同理：收单需要度改由收益
        // 定后，宽低音（ρ ≈ 0.8）单声道回放只少约 0.46 dB 低音，不值得收。""")

sub_range(H, "代价常数一段的说明与收单损失两端", "// ---- ADR-012 修坯根治（设计规格-Clay-根治修订 第 3 节）", "float clayMonoLossHighDb = -3.0f;",
"""        // ---- ADR-012 修坯根治（设计规格-Clay-根治修订 第 3 节）：默认值就是出厂值；【设计】，待真歌标定 ----
        // 代价 = 画像里「候选处理跑在原始输入上」量到的 100 ms 块峰值 P99 变化（dB，GlazeTypes.h MusicProfile 的 *PeakCostDb）；
        // 代价系数 f = ramp(代价, clayCostZeroDb, clayCostFullDb)（反向斜坡），乘在低切、加宽的需要度之后（ADR-015 起收单自动态不做，
        // 收单代价照测不用）。快照读写同其他 clay*：缺字段按 0 读——代价两端 0 / 0 → 判断不了 → 三项中性。（ADR-012 的收单损失两端
        // clayMonoLossLowDb / HighDb 已随 ADR-015 的退役一起删，见上；决策器算 monoLossDb 的代码已在 T-018 删除。ClayGuard 的 House 反相提示
        // （ClayGuard.cpp houseMonoLossDb）用的是同一个公式，但亮起的门限只读 house.phaseHintCorrelation，与这两项无关。）
        float clayCostFullDb = 0.2f;                  // 先试后用：代价 ≤ 本项 → 照做（系数 1）
        float clayCostZeroDb = 0.6f;                  // 代价 ≥ 本项 → 不做（系数 0）；两者之间线性""")

# ===================== GlazeConstants.cpp =====================
sub(C, "文件头：Clay 全局常数清单",
    "不读的自动收单 7 项：逐段选点阈值两项、分频点钳制范围与默认值三项、收单需要度斜坡两项）默认值就是出厂值，只写在 GlazeConstants.h；",
    "不读的自动收单 7 项已于 T-063 退役、不在此列）默认值就是出厂值，只写在 GlazeConstants.h；")
sub_range(C, "文件头：ADR-015 段", "ADR-015（0.1.7.1）起自动不收单：自动收单的 7 个常数（", "存档内容不变（唯一还按它们出方案的是 v0.1.7.0",
"""    ADR-015（0.1.7.1）起自动不收单，自动收单的 7 个常数（clayMonoBandSideRatioDb、clayMonoBandCorrelation、clayMonoMinHz / MaxHz /
    DefaultHz、clayMonoLossLowDb / HighDb）决策器不读；T-063（2026-10-04，Max 选「现在退役」）把它们连快照字段一起退役，同上：新快照
    不再写、旧快照里残留的读时忽略，名字不得复用（复用会把旧值读进新字段）。快照 version 不升（9 尚未发布）、格式号不升：删字段不改
    能读的存档。后果：v0.1.7.0 降级打开新存档时这 7 个读成 0，自动收单保持中性（方向就是 ADR-015）；这 7 个属性存在但损坏的存档以前
    整份拒收，现在忽略（只出现在手改或损坏的存档上，同 ADR-012）。""")
# （上一条的 3 行范围里，第 3 行末尾本来就带「退役与否待 Max 拍板（T-057）。」，已一并换掉。）
sub(C, "版本 9 的注释补一句",
    "T-038 的曲风 / House / 交付字段（可缺省）同在这一版。算法版本不升（只改常数，决策方式不变）。",
    "T-038 的曲风 / House / 交付字段（可缺省）同在这一版。算法版本不升（只改常数，决策方式不变）。\n"
    "            // T-063（2026-10-04）退役 7 个自动收单常数（不再写进快照、读时忽略）也并入这一版：删字段不改能读的存档，9 尚未发布，不另升。")
sub_range(C, "字段表：自动收单 5 项连注释", "// 自动收单的逐段选点阈值两项、分频点钳制范围与默认值三项", 'v.optionalField ("clayMonoDefaultHz"', "")
sub(C, "ADR-012 段文字", "决策器按「判断不了」让三项中性、不标代价位；收单损失两端（clayMonoLossLowDb / HighDb）ADR-015 起决策器不读，照写照读\n"
    "            // 只为旧快照字段兼容（v0.1.7.0 时缺字段读成 0 / 0 → 不需要收单）。ClayGuard 的快路门与最短持续秒数读成 0 时怎样解释归\n",
    "决策器按「判断不了」让三项中性、不标代价位。（ADR-012 的收单损失两端 clayMonoLossLowDb / HighDb 已随 ADR-015 的退役一起删，\n"
    "            // T-063，见文件头。）ClayGuard 的快路门与最短持续秒数读成 0 时怎样解释归\n")
sub(C, "字段表：收单损失两端", '            v.optionalField ("clayMonoLossLowDb", c.clayMonoLossLowDb, 0.0f);\n            v.optionalField ("clayMonoLossHighDb", c.clayMonoLossHighDb, 0.0f);\n', "")

# ===================== GlazePlannerTests.cpp =====================
sub(T, "文件头说明",
    "收单恒为中性、不置收单代价位（testClayAutoMonoAlwaysNeutral 等）；自动收单的 7 个常数决策器已不读，只剩 testClayConstants 的\n"
    "    出厂值与快照读写核对，另有 testClayNeutralCases（「逐段统计全没有」）与 testClayMonotonicAndBounded（分频点范围）两处拿它们当\n"
    "    期望值（前者只是与 ClayPlan{}.monoHz 恰好相等，后者在走不到的分支里；退役时一起改，见 T-057）。",
    "收单恒为中性、不置收单代价位（testClayAutoMonoAlwaysNeutral 等）；自动收单的 7 个常数决策器已不读，T-063（2026-10-04）连快照字段\n"
    "    一起退役：testClayConstants 删了它们的出厂值与字段表项、加了「旧存档带着这 7 个属性读时忽略、再存不写」；testClayNeutralCases、\n"
    "    testClayMonotonicAndBounded 里原来拿它们当期望值的地方改成 ClayPlan{} 的默认分频点 / 收单恒为 0。")
sub(T, "Clay 一节注释",
    "        //   收单：ADR-015 起自动态恒为中性。原先的需要度 ramp(10·log10((1 + ρ) / 2), −0.5, −3)、逐段「S/M > −15 dB 或 ρ < 0.3」、\n"
    "        //   分频点 60 ～ 160 Hz / 默认 80 Hz——这 7 个自动收单常数决策器已不读，只剩 testClayConstants 的出厂值与快照读写核对，另有\n"
    "        //   testClayNeutralCases、testClayMonotonicAndBounded 两处拿它们当期望值（前者只是与 ClayPlan{}.monoHz 恰好相等，后者在\n"
    "        //   走不到的分支里；退役时一起改，见 T-057）。",
    "        //   收单：ADR-015 起自动态恒为中性。原先的需要度 ramp(10·log10((1 + ρ) / 2), −0.5, −3)、逐段「S/M > −15 dB 或 ρ < 0.3」、\n"
    "        //   分频点 60 ～ 160 Hz / 默认 80 Hz——这 7 个自动收单常数已于 T-063 连快照字段一起退役，不在出厂值与字段表里了。")
sub(T, "逐段统计全没有（noBands）",
    "            // 收单照样是中性（比例 0、分频点 ClayPlan{} 默认值）。下面把期望的分频点写成 clayMonoDefaultHz，只是它与 ClayPlan{}.monoHz\n"
    "            // 恰好都是 80 Hz（退役时一起改，见 T-057）。\n",
    "            // 收单照样是中性（比例 0、分频点 ClayPlan{} 默认值）。\n")
sub(T, "noBands 断言",
    "expect (sameBits (noBandPlan.monoAmount, reference.monoAmount) && sameBits (noBandPlan.monoHz, factoryConstants().clayMonoDefaultHz),\n"
    "                    utf8 (\"没有逐段统计：照样收单，分频点取默认值\"));",
    "expect (sameBits (noBandPlan.monoAmount, reference.monoAmount) && sameBits (noBandPlan.monoHz, ClayPlan {}.monoHz),\n"
    "                    utf8 (\"没有逐段统计：收单照样中性，分频点取 ClayPlan{} 的默认值\"));")
sub(T, "单调与上界：分频点那一支",
"""                    // 分频点：ADR-015 起自动不收单，收单恒为 0、分频点恒为默认值（中性）；下面「收单开着」那一支（[60, 160] Hz、
                    // 整轮逐位不变）是 v0.1.7.0 的口径，走不到（退役时一起改，见 T-057）。
                    if (clay.monoAmount > 0.0f)
                    {
                        if (crossover == 0.0f)
                            crossover = clay.monoHz;

                        ok = ok && sameBits (clay.monoHz, crossover)
                                && clay.monoHz >= constants.clayMonoMinHz && clay.monoHz <= constants.clayMonoMaxHz;
                    }
                    else
                    {
                        ok = ok && sameBits (clay.monoHz, ClayPlan {}.monoHz);
                    }
""",
"""                    // 收单与分频点：ADR-015 起自动不收单，收单恒为 0、分频点恒为 ClayPlan{} 的默认值（中性）。
                    // （原先「收单开着」那一支核对分频点落在 [clayMonoMinHz, clayMonoMaxHz] 且整轮逐位不变——v0.1.7.0 的口径，走不到，
                    // T-063 随常数一起删。）
                    ok = ok && clay.monoAmount == 0.0f && sameBits (clay.monoHz, ClayPlan {}.monoHz);
""")
sub(T, "单调与上界：crossover 局部变量",
    "                auto crossover = 0.0f;   // 这一轮第一次开收单时的分频点（之后必须逐位不变）\n", "")
sub(T, "出厂值：7 行",
    "            expectEquals (factory.clayMonoBandSideRatioDb, -15.0f);\n"
    "            expectEquals (factory.clayMonoBandCorrelation, 0.3f);\n"
    "            expectEquals (factory.clayMonoMinHz, 60.0f);\n"
    "            expectEquals (factory.clayMonoMaxHz, 160.0f);\n"
    "            expectEquals (factory.clayMonoDefaultHz, 80.0f);\n", "")
sub(T, "出厂值：收单损失两端",
    "            expectEquals (factory.clayMonoLossLowDb, -0.5f);\n"
    "            expectEquals (factory.clayMonoLossHighDb, -3.0f);\n", "")
sub(T, "出厂值注释：收单需要度",
    "代价系数 ramp(代价, 0.6, 0.2)、收单需要度 ramp(损失, −0.5, −3)（ADR-015 起决策器不读，\n"
    "            // 这里只核对出厂值）、反相快路门 0.1（ADR-014 由 0.3 改）、最短持续 0.15 s。",
    "代价系数 ramp(代价, 0.6, 0.2)（ADR-012 的收单需要度斜坡 ramp(损失, −0.5, −3) 两端\n"
    "            // 随 ADR-015 的退役一起删了，T-063）、反相快路门 0.1（ADR-014 由 0.3 改）、最短持续 0.15 s。")
sub(T, "字段表注释计数",
    "逐项核对「全部 33 个 clay* 字段都进了快照、都能按位读回、缺了都按 0 读」。\n"
    "            // ADR-012 删掉了 clayMonoSideRatioLowDb / HighDb、clayMonoCorrelationLow / High（4 个），加了代价系数两端、收单损失两端、\n"
    "            // 反相快路门（float 5 个）与最短持续秒数（double 1 个）：30 − 4 + 6 = 32；T-043 加混合损失封顶门槛（float 1 个）= 33。",
    "逐项核对「全部 26 个 clay* 字段都进了快照、都能按位读回、缺了都按 0 读」。\n"
    "            // ADR-012 删掉了 clayMonoSideRatioLowDb / HighDb、clayMonoCorrelationLow / High（4 个），加了代价系数两端、收单损失两端、\n"
    "            // 反相快路门（float 5 个）与最短持续秒数（double 1 个）：30 − 4 + 6 = 32；T-043 加混合损失封顶门槛（float 1 个）= 33；\n"
    "            // T-063 退役 7 个自动收单常数（ADR-015 起决策器不读）：33 − 7 = 26。")
sub(T, "floatFields",
    "                &Constants::clayLowCutMinK, &Constants::clayMonoBandSideRatioDb,\n"
    "                &Constants::clayMonoBandCorrelation, &Constants::clayMonoMinHz, &Constants::clayMonoMaxHz, &Constants::clayMonoDefaultHz,\n"
    "                &Constants::clayWidthNarrowStartDb, &Constants::clayWidthNarrowFullDb, &Constants::clayWidthMaxDb,\n"
    "                &Constants::clayMinCorrelationAfterWidth, &Constants::clayGuardPhaseCorrelation,\n"
    "                &Constants::clayCostFullDb, &Constants::clayCostZeroDb, &Constants::clayMonoLossLowDb, &Constants::clayMonoLossHighDb,\n"
    "                &Constants::clayGuardPhaseFastGate, &Constants::clayBlendLossLimitDb",
    "                &Constants::clayLowCutMinK,\n"
    "                &Constants::clayWidthNarrowStartDb, &Constants::clayWidthNarrowFullDb, &Constants::clayWidthMaxDb,\n"
    "                &Constants::clayMinCorrelationAfterWidth, &Constants::clayGuardPhaseCorrelation,\n"
    "                &Constants::clayCostFullDb, &Constants::clayCostZeroDb,\n"
    "                &Constants::clayGuardPhaseFastGate, &Constants::clayBlendLossLimitDb")
sub(T, "clayFieldCount", "constexpr int clayFieldCount = 21 + 6 + 3 * 2;", "constexpr int clayFieldCount = 14 + 6 + 3 * 2;")
sub(T, "特殊位模式：NaN 换字段", "odd.clayMonoBandCorrelation = std::numeric_limits<float>::quiet_NaN();", "odd.clayWidthNarrowFullDb = std::numeric_limits<float>::quiet_NaN();")
sub(T, "特殊位模式：第二个 NaN 换字段", "odd.clayMonoLossHighDb = std::numeric_limits<float>::quiet_NaN();", "odd.clayWidthMaxDb = std::numeric_limits<float>::quiet_NaN();")
sub(T, "差 1 ulp 换字段", "oneUlp.clayMonoMaxHz = std::nextafter (oneUlp.clayMonoMaxHz, 1000.0f);", "oneUlp.clayWidthMaxDb = std::nextafter (oneUlp.clayWidthMaxDb, 1000.0f);")
sub(T, "差 1 ulp 注释", "（float、double、风格字段各一例，另加 ADR-012 的两个新字段）", "（float、double、风格字段各一例，另加 ADR-012 的新字段）")

sub(T, "删掉的常数 + 旧存档带着 7 个属性",
"""            // ADR-012 删掉的四个常数不再写；开发期快照里残留的这几个属性读时忽略（与没有它们的快照读出来逐位相同）。
            const char* const removedFields[] = { "clayMonoSideRatioLowDb", "clayMonoSideRatioHighDb", "clayMonoCorrelationLow",
                                                  "clayMonoCorrelationHigh" };
""",
"""            // ADR-012 删掉的四个常数、ADR-015 起决策器不读而 T-063 一起退役的 7 个自动收单常数都不再写；旧存档 / 开发期快照里残留的这些
            // 属性读时忽略（与没有它们的快照读出来逐位相同）。名字不得复用：残留属性的值（−20 的位模式，与 11 个出厂值都不同）读进来
            // 若影响了任何字段，下面的逐位比较就败。
            const char* const removedFields[] = { "clayMonoSideRatioLowDb", "clayMonoSideRatioHighDb", "clayMonoCorrelationLow",
                                                  "clayMonoCorrelationHigh",
                                                  "clayMonoBandSideRatioDb", "clayMonoBandCorrelation", "clayMonoMinHz", "clayMonoMaxHz",
                                                  "clayMonoDefaultHz", "clayMonoLossLowDb", "clayMonoLossHighDb" };
""")
sub(T, "残留属性之后加真实出厂位模式一组",
"""            expect (fromXml (*strayXml, stray) && bitwiseEqual (stray, factory), utf8 ("残留的旧常数属性读时忽略"));
""",
"""            expect (fromXml (*strayXml, stray) && bitwiseEqual (stray, factory), utf8 ("残留的旧常数属性读时忽略"));

            // T-063：T-063 之前存的工程，标定快照里带着这 7 个属性、值是它们的真实出厂位模式（−15 / 0.3 / 60 / 160 / 80 / −0.5 / −3 的
            // IEEE 单精度十六进制）。新版读它们：忽略，与没有它们的快照逐位相同；再存不写它们；属性存在但损坏（不是 8 位十六进制）
            // 也忽略（以前整份拒收，T-063 起接受，同 ADR-012；只出现在手改或损坏的存档上）。
            {
                const char* const retiredAutoMono[][2] = { { "clayMonoBandSideRatioDb", "c1700000" }, { "clayMonoBandCorrelation", "3e99999a" },
                                                           { "clayMonoMinHz", "42700000" },           { "clayMonoMaxHz", "43200000" },
                                                           { "clayMonoDefaultHz", "42a00000" },       { "clayMonoLossLowDb", "bf000000" },
                                                           { "clayMonoLossHighDb", "c0400000" } };

                auto oldArchive = toXml (factory);

                for (const auto& attribute : retiredAutoMono)
                    oldArchive->setAttribute (attribute[0], attribute[1]);

                Constants fromOld;
                expect (fromXml (*oldArchive, fromOld) && bitwiseEqual (fromOld, factory),
                        utf8 ("T-063 之前存的快照带着 7 个自动收单属性（真实出厂位模式）：读时忽略，与出厂快照逐位相同"));

                const auto resaved = toXml (fromOld);

                for (const auto& attribute : retiredAutoMono)
                    expect (! resaved->hasAttribute (attribute[0]), utf8 ("再存不写：") + attribute[0]);

                // 与出厂快照的 XML 逐属性相同（读旧存档再存 = 没有那 7 个属性的快照）。
                auto sameAttributes = resaved->getNumAttributes() == factoryXml->getNumAttributes();

                for (int i = 0; sameAttributes && i < factoryXml->getNumAttributes(); ++i)
                    sameAttributes = factoryXml->getAttributeName (i) == resaved->getAttributeName (i)
                                  && factoryXml->getAttributeValue (i) == resaved->getAttributeValue (i);

                expect (sameAttributes, utf8 ("读旧存档再存 = 出厂快照的 XML，一个属性都不多不少"));

                // 损坏的残留属性也忽略（不是整份拒收）。
                auto corrupt = toXml (factory);

                for (const auto& attribute : retiredAutoMono)
                    corrupt->setAttribute (attribute[0], "zz");

                Constants fromCorrupt;
                expect (fromXml (*corrupt, fromCorrupt) && bitwiseEqual (fromCorrupt, factory), utf8 ("损坏的残留属性也忽略（以前整份拒收）"));
            }
""")
sub(T, "ADR-012 开发期快照：去 2 个收单损失字段",
"""            // v0.1.7.0 开发期的快照：有 clay* 却没有 ADR-012 的 6 个字段 → 那 6 个按 0 读（代价两端 0 / 0 判断不了；收单损失两端 ADR-015
            // 起决策器不读）→ 三项都中性、不置代价位（不会按旧口径去低切），其余 clay* 照读。
            auto devXml = toXml (factory);
            const char* const adr012Fields[] = { "clayCostFullDb", "clayCostZeroDb", "clayMonoLossLowDb", "clayMonoLossHighDb",
                                                 "clayGuardPhaseFastGate", "clayGuardPhaseMinSeconds" };
""",
"""            // v0.1.7.0 开发期的快照：有 clay* 却没有 ADR-012 的 4 个字段（收单损失两端 ADR-015 起不读、T-063 起不写，不在其列）→ 那 4 个按 0
            // 读（代价两端 0 / 0 判断不了）→ 三项都中性、不置代价位（不会按旧口径去低切），其余 clay* 照读。
            auto devXml = toXml (factory);
            const char* const adr012Fields[] = { "clayCostFullDb", "clayCostZeroDb", "clayGuardPhaseFastGate", "clayGuardPhaseMinSeconds" };
""")
sub(T, "ADR-012 开发期快照：断言",
"""            expect (dev.clayCostFullDb == 0.0f && dev.clayCostZeroDb == 0.0f && dev.clayMonoLossLowDb == 0.0f && dev.clayMonoLossHighDb == 0.0f
                        && dev.clayGuardPhaseFastGate == 0.0f && dev.clayGuardPhaseMinSeconds == 0.0,""",
"""            expect (dev.clayCostFullDb == 0.0f && dev.clayCostZeroDb == 0.0f
                        && dev.clayGuardPhaseFastGate == 0.0f && dev.clayGuardPhaseMinSeconds == 0.0,""")
sub(T, "ADR-012 断言文字", 'utf8 ("缺的 ADR-012 字段按 0 读"));', 'utf8 ("缺的 ADR-012 字段按 0 读"));')

bad = [m for ok, m in log if not ok]
for ok, m in log:
    print(("OK  " if ok else "*** ") + m)
if bad:
    sys.exit(f"{len(bad)} 条对不上，一个文件都没写")
if not dry:
    for p, t in texts.items():
        p.write_text(t, encoding="utf-8")
    print(f"已写入 {len(texts)} 个文件")

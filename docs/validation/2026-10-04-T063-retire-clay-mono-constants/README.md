# T-063 · 退役 7 个自动收单常数连标定快照字段（2026-10-04，DGX Spark 本机）

Max 2026-10-04 19:4x 在 T-057 拍板档里选 **B「现在退役」**（拍板档 `04-协调/2026-10-04-T-057-收单遗留常数退役待Max拍板.md` 第 2、3 条）。
**声音与方案逐位不变**：这 7 个常数在 ADR-015 之后运行期没有任何读者（决策器不读、保护单元不读、渲染工具不读），只剩快照字段表和测试在引用。

## 改了什么

| 位置 | 改动 |
|---|---|
| `Source/dsp/GlazeConstants.h` | 删 `clayMonoBandSideRatioDb` / `clayMonoBandCorrelation` / `clayMonoMinHz` / `clayMonoMaxHz` / `clayMonoDefaultHz` / `clayMonoLossLowDb` / `clayMonoLossHighDb` 7 个成员；13 行说明换成「已退役的 7 个自动收单常数」登记块（名字、不再写、旧存档残留读时忽略、**名字不得复用**）；ADR-012 代价注释改写（保留 `clayCostFullDb=0.2`、`clayCostZeroDb=0.6`） |
| `Source/dsp/GlazeConstants.cpp` | 删 7 行 `optionalField`；文件头与 version 9 的注释写明「T-063 并入这一版」；`Constants::version` 不升（9 尚未发布）、`constantsFormatVersion` 不升（仍为 1） |
| `tests/GlazePlannerTests.cpp` | 删 7 个出厂值断言与字段表项（clay\* 字段 33 → 26）；拿它们当期望值的地方改成 `ClayPlan{}` 默认分频点 / 收单恒为 0；`removedFields` 并入这 7 个名字；**新增一组**「T-063 之前存的快照带着 7 个自动收单属性」 |

改动脚本 `apply_edits.py`（逐条断言原文恰好出现一次才套用）；完整 diff 见提交 `974796f`。

## 兼容性（拍板档第 2、3 条，Max 选 B 即接受）

- **旧工程用新版打开**：残留的 7 个属性读时忽略，读出来与没有它们的快照逐位相同（新用例用这 7 个属性的真实出厂位模式验）。
- **v0.1.7.0 降级打开新存档**：这 7 个读成 0，自动收单保持中性——方向就是 ADR-015。
- **这 7 个属性存在但损坏的存档**：以前整份拒收，现在忽略（只出现在手改或损坏的存档上，同 ADR-012）。

## 验证（Spark 本机，`celadon_tests` 0 警告）

1. **全套测试（改前 main `879288d` / 改后）**：148349 过 / 5 败 → **148359 过 / 5 败**。
   - 多出的 10 条逐项对得上：新增那一组 10 条（旧存档读出与出厂逐位相同 1 + 再存不写 7 + 再存 XML 与出厂逐属性相同 1 + 损坏残留属性也忽略 1）＋ `removedFields` 并入 7 个名字（每个名字一条「不再写进快照」）＋7，再减去删掉的 7 个出厂值断言 −7，净 +10；
   - 两边 `Completed tests in …` 各 615 行，名单逐行相同；
   - 5 条失败是同一批 Spark 本机（aarch64）已知基线：ColorModule L=R ×4、BalancePerformance 速度比 1 条（比值每次跑略有不同：1.72 / 1.75，限 1.40）。**没有新失败。**
2. **渲染对照**（`render_compare.sh`，结果 `render-compare.log`）：改前 / 改后两版 `celadon_render` 渲染 Jamendo 9 首（通用组 `--glaze 0,35,70 --clay auto,off`）+ 3 首 deep 曲目（House 组：俱乐部交付、4/4、清单 BPM、GLAZE 35、修坯自动），`metrics.csv` **12 份逐字节相同、0 份不同**（78 个 render 行，每行的 `audio_hash` 都在里面）。
3. **反向验证（变异测试 R1，`mutation_run.py`，失败清单 `mutation-R1-failures.txt`）**：在独立源码树里套上 T-063 的三个文件，再把 7 个退役名字**全部**重新塞进字段表（借道现有成员 `clayLowCutMinK` 读写，等于「新存档又写了它们」「旧存档的值被读进别的字段」「名字被复用」三件事同时发生）→ `--only GlazePlanner` **93 条里 20 条败**，覆盖：7 条「删掉的常数不再写进快照」、7 条「再存不写」、残留忽略、真实出厂位模式忽略、XML 逐属性相同、损坏的残留属性忽略、字段数 26 ≠ 33。新用例不是摆设。

## 没做 / 不改

- 快照 `version` 与 `constantsFormatVersion`：不升（见上），要升须另拍板。
- 测试里的标识符旧名不改（不是用户可见文字，只增加噪音）。
- x86 / MSVC 全套、22 首门检：本机到 107 / 109 的 SSH 没有私钥，做不了（见 TASKS 末尾阻塞清单）；本次不涉及这两条链路的新代码，仅删字段。

# T-066 · 官网说明页收单 / 修坯措辞补强 + 插件收单频率气泡末句 + 文案表死文案（2026-10-04，DGX Spark 本机）

T-061 范围外发现挂出的两张后台卡片，Max 2026-10-04 19:4x 同意做。ADR-015 起「自动」不收单、手动的低切 / 收单仍受峰值守护；官网说明页与插件气泡里几处停在旧说法。**只改文字 / 注释 / 文案表**，判定逻辑与声音不变。

## 改了什么（17 处官网句子 + 插件 / 测试，完整改动见 `change.diff`）

| 位置 | 改法 |
|---|---|
| `docs/{zh,en}/glaze.md` :14、:51 | 补「收单在自动下从不收，只提醒」；:51 把「CLAY 三项」拆成「低切与加宽」与「收单」 |
| `docs/{zh,en}/clay.md` 三态表 | 「自动」行补收单例外；「手动」行写明峰值守护照常（手动的低切 / 收单在会抬峰值的瞬间让开一点，手动值本身不变；加宽不经守护）；`en/clay.md` 小图「slope」→「hatching」 |
| `algorithms/{zh,en}/clay.md` 三态语义 | 同上两处 |
| `docs/{zh,en}/{interface,meters}.md` | 「画出低切与收单频率」→「正在生效的低切频率与（手动）收单频率」。核实：`PluginEditor.cpp:1300` 只在 `monoPercent > 0` 时把收单频率交给频谱仪，自动态不收单所以不画 |
| 收单频率一行（`docs/{zh,en}/clay.md` 与 `HelpText.cpp` 悬停气泡，同源） | 末句「自动时手动值不生效，这里显示的是此刻的实际值」读起来像自动也有一个收单频率 → 「「自动」时手动值不生效（自动不收单，这里不显示收单频率）」。保留规格第 5 节要求三个手动值参数都有的「「自动」时手动值不生效」（`HelpTextTests` 既有断言），只换掉误导的后半句。核实：`clayStageValue` 在 `monoPercent <= 0` 时不返回数值 |
| `HelpText.cpp` 灯条目注释 | 第 2 行底稿界面上从不显示，只作官网说明页首句的同源底稿 |
| `Localisation.{h,cpp}` | 删掉无调用处的 `Text::advancedClay` / `advancedClayNote`（全仓 grep 只剩定义；`static_assert` 与 `LocalisationTests` 按 `numTexts` 自适应） |
| `HelpTextTests.cpp` | 加一条「收单频率」阳性对照断言（中英各一） |

## 验证

- **站点**：改前（main `40d619a`）/ 改后各自 `pnpm install --frozen-lockfile` + `pnpm run build`（`check-i18n` + `tsc --noEmit` + `vite build`）都过；产物 JS 逐句对比（`dist_sentence_diff.py`，输出 `dist-diff.txt`）：只在改前 **17** 行、只在改后 **17** 行，一一对应上面的 17 处改动，别的句子一个字没变。
- **插件测试**（`celadon_tests`，0 警告）：`--only HelpText,HelpTextClay,HelpTextHouse,Localisation` 改后 **5265 过 / 0 败**（`targeted-tests-after.txt`）。
- **阳性对照**：把改后的树里 `HelpText.cpp` 换回 main 的旧文案、测试保持新的（`targeted-tests-old-control.txt`）：**恰好新加的两条败**（`mono frequency: Auto shows no frequency` 与它的英文版），其余 5263 过；放回新文案全过。

## 没做 / 待定

- `site/src/content/changelog.ts` 254-255「方案切换」是历史条目，不改历史。
- 页脚「与插件悬停提示一致」的名实：官网说明页与插件气泡并不逐字相同，是否改成「说的是同一件事」留给 Max。
- 官网部署 112：见 TASKS（Spark 上没有 112 的 SSH 私钥；`deploy.sh` 还要求 0.1.8.8 的安装包目录；线上站点已迁到 celadonaudio.com，仓库尚未跟进——不先对齐就部署会把线上域名改回去）。

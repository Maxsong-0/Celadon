# T-042 · BUG-009 低频相关度泄漏门：验证证据（2026-10-01）

档案：`项目/Celadon/06-BUG/BUG-009-低频组只有上方泄漏时反相提示误亮.md`（根因、门限推导、同类评估都在那里）、日志 `02-日志/2026-10-01-2.md` T-042 节。
改动：`Source/dsp/MusicAnalyzer.cpp`（`updateLiveStereo` 的泄漏门：核心段 0 ～ 4 门上功率 < 段 6 ～ 8 的 −35 dB → 本帧低频组不计）。

## 文件

| 文件 | 内容 |
|---|---|
| `livestereo_emu.py` | numpy 逐帧复算 Balance 帧（Hann、2048 点、hop 512、27 段 bin 权重）与 `updateLiveStereo` 的实时视图；与 C++ 追踪对齐链延迟 28.8 ms 后有效性 99.2 % 一致、相关度中位差 0。门限的分布分析与「单个正弦能否打开门」的扫频都用它的 `band_weights` / `frames` |
| `high_check.py` | Tech breakdown 高频组按和弦拆（宽度保护那一路的评估） |
| `t042-render.sh` | 109 上的渲染回路（照 `docs/validation/2026-09-27-T015-calibrating/calib-loop.sh`）：`build <标签>` 同步本工作树编 `celadon_render` 并按标签留二进制，`run <标签> <远端输入> <本地输出> [参数]` 每首一个进程。ROOT 写死为本工作树 |
| `synth-{before,after}-*-metrics.csv` | 合成 House / Tech（`make_synthetic_house.py`，T-038 分支）GLAZE 35 修坯自动：反相提示 14.39 / 14.72 % → 0.00 / 0.00 %，音频哈希相同，宽度保护 14.90 % 不变 |
| `gate22-{before,after}-all.csv` | 22 首门槛歌（109 `songs-9a355b99857c`）GLAZE 0 / 35 / 70 × 修坯自动 / 全关 + 同决策重放，198 行，改前（main `88199cb`）/ 改后 |
| `compare22.py` / `compare22.md` | 改前改后对比：198 行音频哈希全同；反相提示时长 22 首 × GLAZE 35 / 70 全同；慢路反相块占比三首微动（泄漏块变无效、分母变小） |
| `gate22-{before,after}-check.txt` | `tools/render/gate_check.py` 输出：判定相同，只差那三首豁免说明里的占比；唯一失败 woqu g70（修坯让响度降 0.199 LU、入口峰值只降 0.099 dB）改前就有，与本修复无关 |

## 测试

- 107 `celadon_tests` 全套 134612 过 / 0 败（改动的编译单元重编 0 警告）。
- 新用例在 main 版分析器上跑：MusicAnalyzerClay 2 条、ClayGlaze 3 条失败（安静 pad 418 / 1125 块有效、+24 dB pad 1110 / 1125、整链 breakdown 灯亮 2001 / 3862 块）——测试抓得住这个 bug。

# T-026 · BUG-004 重开工程后 GLAZE 校准丢失 —— 验证记录（2026-09-29）

档案：vault `项目/Celadon/06-BUG/BUG-004-对数刻度参数往返漂移导致重开后校准丢失.md`、`03-记忆/ADR-018-对数刻度参数按容差比不进精确哈希.md`、日志 `02-日志/2026-09-29.md`。
所有编译与测试都在 107（GCC 15，Release）上跑，本机 Mac 没编译、没跑测试。

## 1. 往返漂移实测（决定容差与邻域）

`rt_drift.cpp`：照抄 `ParameterRanges.h` 的 `logRange` 与 JUCE `NormalisableRange` 的钳位，对区间内每个 float 做 to → from 往返 k 次，
记最大漂移。109，`g++ -O2 -ffp-contract=off`。把 `k <= 8` 改成 200 即第二组数据。

| 参数 | 两趟（一次保存重开）最大漂移 | 反复往返的最大漂移（到不动点） |
|---|---|---|
| Limit Release 10～1000 ms | 12 ulp | 17 ulp（相对 1.14e-6） |
| Clay 低切 10～60 Hz | 4 ulp | 9 ulp（相对 8.0e-7） |
| Clay 收单 40～300 Hz | 6 ulp | 124 ulp（相对 1.18e-5，约 100 趟后不再动） |

→ 新口径容差取相对 1e-4；旧存档升格邻域取 ±16 ulp。

## 2. 全套

- 改前（`be87262`）：`suite-baseline-be87262.log`，**131126 过 / 0 败**，0 警告。
- 改后：`suite-after.log`，**131193 过 / 0 败**，0 警告（同时编 `celadon_render`，0 警告）。所测源码散列见 `source-sha256.txt`（与工作树 104 个文件逐个一致）。

## 3. 反向验证

- `reverse-fix-disabled.log`：服务器副本里把 `referenceMatches` 改成只比旧口径哈希、`upgradeLegacyConditions` 直接返回 0（= 修前的比较）→
  GlazeCalibration 27b 失败 13 项、T-026 用例 1 失败 2 项（往返 54608 次不一致、升格 15000 次失败）、用例 2 失败 9 项（重开后校准不适用、执行快照被丢、开了新一轮校准）。
- `reverse-rolling-stability.log`：把稳定计时改回逐块比 → T-026 用例 3（慢速扫描不算稳定）失败。

## 4. 其他检查

- `Parameters.cpp`（不在测试目标里）与改过的 DSP 文件、`PluginProcessor.cpp`：107 上 GCC 与 clang 21 `-fsyntax-only`，插件宏与警告选项同 `scripts/check-syntax.sh`，另加 `-Wshorten-64-to-32`：0 错 0 警告。
- 开销（`hashbench.cpp`，107，clang -O3）：旧 conditionsHash 约 104 ns / 次，新（两份哈希同一趟）约 120 ns / 次；参考条件每块算 2～3 次 → 192k / 32 约 +0.03 个百分点。

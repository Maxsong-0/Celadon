# T-123 界面 V5「一釉」· 验证记录

计划 `docs/handoff/2026-11-19-T123-plan.md`（原件在 vault `04-协调/`），规格 `docs/handoff/2026-11-19-T123-spec-ui-v5*.md`。
这里按里程碑记「跑了什么命令、得到什么」。Mac 上不编不测，全部经 testbox 交 108（x86_64）；最后另跑 DGX aarch64 与 Windows。

## 怎么跑

打包目录里放三样：`Celadon/`（被测源码，可以是带着未提交改动的工作树）、`JUCE/`（`LICENSE.md` + `modules/`）、本目录的 `run.sh`。

```bash
testbox --remote testbox@max-spark submit --arch x86_64 --recipe full --cmd './run.sh tests base' --memory 16G --cpu 20 --label '…'
```

- `./run.sh tests <base|v3> [--only 类名]`：编 `celadon_tests` 并运行；`v3` 加 `-march=x86-64-v3`。
- `./run.sh processor`：编 `celadon_processor_tests` 并运行。
- `./run.sh ui <清单>`：编截图工具，清单一行一次调用（`lists/` 里是用过的清单）；`@sha <目录>` 印每张图的 sha256，`@b64 <目录>` 把图打包进日志。
- 开发中的定向检查加 `BUILD_ROOT=/cache/celadon-t123`，构建目录跨任务保留、只编改过的。全量验证不加，每次全新编。
- 打包目录从工作树同步时按内容比较、不照搬修改时间：增量构建按修改时间判要不要重编，把文件换回旧内容时照搬时间会漏编（2026-11-19 踩过一次）。

## M0 基线（提交 `8090ad3`，界面代码与 main `6b37415` 相同）

2026-11-19 在 108 上跑，testbox 任务号在括号里。

| 项 | 结果 |
|---|---|
| `celadon_tests` 基线档（`20261119-092449-5834`） | 136,195 过 / 0 败，0 条编译警告 |
| `celadon_tests` `-march=x86-64-v3`（`20261119-093256-5f8a`） | 136,195 过 / 0 败 |
| `celadon_processor_tests`（`20261119-093257-cbda`） | 208 过 / 0 败；编译警告 1 条，改动前就有（`ProcessorStateDigestTests.cpp:136` 的 `-Wshadow`） |
| 截图工具 `--only check`（`20261119-093325-bda4`） | 退出码 0，190 条 PASS、0 条 FAIL，末行 `all interaction checks passed`；全文 `m0/check.txt` |
| `--only digest --deterministic`（同上） | 205 行，全文 `m0/digest.txt`；`digest porcelain all 2502f71e25cd7e7a`，静态背景四种像素比见文件末四行 |
| 全套出图 `--deterministic` 两遍（同上） | 各 43 张；两遍逐字节相同的 40 张，不同的 3 张：`zh-audition`、`zh-glaze90`、`zh-input`。哈希在 `m0/shots-run-a.sha256`、`m0/shots-run-b.sha256` |
| `--only bench` 两遍（`20261119-094039-94a6`） | 整帧重画平均：1 倍 6.97 / 7.04 ms，2 倍 12.70 / 12.99 ms，4 倍 153.3 / 155.7 ms；编辑器计时器一帧 0.053 / 0.052 ms（1 倍）。全文 `m0/bench-1.txt`、`m0/bench-2.txt` |
| `--only spectrum-bench --seconds 4` 两遍（同上） | 频谱一帧重画平均：1 倍 1.48 ～ 1.50 ms，2 倍 3.57 ～ 3.62 ms；推一帧 0.24 ms。全文 `m0/spectrum-bench-1.txt`、`m0/spectrum-bench-2.txt` |

说明：

- 出图那三张两遍不同是改动前就有的（工具文件头写着「两次运行逐像素相同，电平表除外」）。M1 ～ M5 判「V4 的画面没变」用：digest 逐行相同、`--only check` 全过、那 40 张的哈希不变。
- 基准跑的时候 108 上还有别的项目的任务在跑（两遍之间差 1 ～ 2 %）。C6 的结论不拿这组数直接比：收尾时把 M0 的树和做完的树放进同一个任务里交替跑。
- DGX aarch64 的已知失败 5 条（ColorModule 4 条、BalancePerformance 1 条）是固定的，口径见 `docs/BUILDING.md`「测试机」一节；T-123 的 aarch64 在 M9 跑。

## M1 展开状态（`Source/ui/OpenState.h` 与两个工程属性）

界面代码没动（编辑器还没读这两个属性），所以 digest 与出图不重跑。

| 项 | 结果 |
|---|---|
| 先红：`OpenState.h` 用占位实现（`20261119-093551-90e6`） | `--only OpenState` 62 条失败 |
| 先红（第二次，`fromStored` 改成吃属性文字之后）：把 `fromStored` 改成原样返回 | `--only OpenState` 38 条失败 |
| 后绿：`./run.sh tests base --only OpenState` | 2,158 过 / 0 败 |
| `celadon_processor_tests`（`20261119-094056-36ad`） | 264 过 / 0 败（= 基线 208 + 新增 56）；编译警告仍是基线那 1 条 |
| `celadon_tests` 基线档全量（`20261119-094057-3faa`） | 138,353 过 / 0 败（= 基线 136,195 + 新增 2,158），0 条编译警告 |

处理器层新增的四条（`tests/processor/ProcessorStateTests.cpp` 的 `testUiOpenStateProperties`）证的是：

- 旧工程没有这两个属性，读出来是全部收起。
- 带上这两个属性存出的 XML，根上只多两个属性；摘掉它们之后和不带时一字不差，GLAZE 节点也一字不差——参数与声音状态不受影响。
- 存 → 读回来是同一页；再读一份旧工程，属性消失、回到全部收起。
- 14 组非法存值（超范围、负数、小数、`nan`、`inf`、字母、空串、页号超出该模块页数……）都读成全部收起，参数照常读入。

`-march=x86-64-v3` 档这一步没重跑：新增的是不含浮点的纯整数逻辑，留到 M9 统一跑。

# T-096f · 含实时 GLAZE 的新 CPU 基线：方法、口径与复现 · 2026-10-11

> 状态：**工具代码改完·未验证**。DGX 构建 / 自检 / 预演 / 首跑由主会话在独占时段做；基线数字填在第 8 节骨架里。
> 依据：计划 `04-协调/2026-10-10-T-096-第1步工具任务计划.md` §13 Q2 / Q10 / Q12 / Q16；侦察档 (f)。插件源码 `Source/**` 一字未改。

## 1. 做什么、不做什么

- **双口径**（Q10）：
  - DGX（aarch64、GCC 13、无 LTO）做「同机不退步」基线：只比同一台机器上**基线二进制与当前二进制**的差，绑 CPU 5（Cortex-X925），交替 ≥ 2 轮，编译观察日志为空才算。
  - Mac（M2 Pro、Xcode -O3 -flto）的绝对数由 Max 本人跑（第 6 节）。预算只在 Mac 读数上判。
- **两层都测**：
  - `processor` = 真 `CeladonAudioProcessor::processBlock`（参数读取、NaN 扫描、频谱仪接头、LookaheadHold、整链）。
  - `chain` = 直接调 `CeladonChain`：`setHostInfo + setParams` 记一段，`process` 记一段。不再手抄链逻辑。
- **本轮不做**：
  - `CeladonChain` 分级计时探针：排 T-095 之后，另立子项 f2。所以 Balance / Clay / Color / Limit 各级拆分本轮没有。
  - `bench.mm` modules 模式只加「已漂移、待退役」说明，不删。
  - 不进 CI；不改 `Source/**`；不动 `tests/BalancePerformanceTests.cpp`；`scripts/perf-local.sh` 只加注释。

## 2. 文件

| 文件 | 作用 |
|---|---|
| `tools/perf/bench_portable.cpp` | 可移植基准 `celadon_bench_portable`（只用 JUCE 与 `Source/**`），带 `--selftest` / `--list` |
| 根 `CMakeLists.txt` 末尾「可移植 CPU 基准（T-096f）开始 / 结束」一段 | EXCLUDE_FROM_ALL 目标。能编界面库：处理器层 + 链层，链 `celadon_juce_gui`；否则（或 `-DCELADON_BENCH_PORTABLE_CHAIN_ONLY=ON`）：只有链层，只链 `celadon_juce` |
| `scripts/perf-dgx.sh` | DGX 入口：记录机器状态、编译观察进程、`taskset` 绑核、基线 / 当前交替、作废判定，带 `--dry-run` |
| `tools/perf/compare_perf.py` | 只用标准库：两份（组）TSV 每轮各自相减、各轮一致性、阈值标记，带 `--selftest` |
| `tools/perf/bench.mm` | rendercheck 改阶段 2 口径；WARM 行改报阶段 2 字段；modules 模式加漂移说明（只在 Mac 编） |

## 3. 口径

所有百分比都等于「耗时 ÷ 块时长 × 100」。多遍归约照 `bench.mm` runGrid：`avg` / `worst` / `p99.9` 取各遍里最好的一遍，`maxAllPasses` 取所有块的最大值。

| 列 | 定义 | 判 / 报 |
|---|---|---|
| `structWorst` | 按「块在 Balance 分析 hop 里的相位」分组，各组取中位数再取最大。口径照 `bench.mm:105-139`；hop 直接问 `BalanceModule` | **主判** |
| `avg` | 每块墙钟均值（最好的一遍） | 不退步判（阈值待定） |
| `structWorst64k` | 按 65536 样本相位分组（滑动和精确重算网格），同 `bench.mm` | 报 |
| `structWorst_1s` | 按 1 s 决策网格对齐分组（`GlazeController.cpp:1253`，`llround(采样率)`，从 prepare 起算） | 只报 |
| `structWorst_5ms` | 按实时画像 5 ms 分片网格对齐分组（`LiveProfile.h:231`） | 只报 |
| `thread_cpu_worst` / `thread_cpu_maxAllPasses` | 逐块本线程 CPU 时间（`CLOCK_THREAD_CPUTIME_ID`），归约同 `worst` / `maxAllPasses` | 只报 |
| `invol_switches` / `vol_switches` | 计时各遍里本线程的非自愿 / 自愿切换总数（`getrusage(RUSAGE_THREAD)`，只 Linux 有） | 只报 |
| `p99.9`、`maxAllPasses`、`worst` | 同 `bench.mm` | 只报 |
| `setparams_*` / `process_*` | 链层两段各自的均值与 structWorst | 只报 |
| `latency`、`prepare_ms`、`nonfinite_blocks`、`confidence_warm` / `confidence_end` / `mainness_end`、`chk` | 状态与核对 | — |

**控制层网格分组怎么算**：组号 = `((pos + bs) mod G) / bs`。第 0 组恰好是含网格点的块，也就是事件在这一块里触发。G 不是块长整数倍时（44.1 kHz 的 1 s）也只落第 0 组；`--selftest` 用暴力枚举核对这一点。

**尖峰归因**：`maxAllPasses`（墙钟）远大于 `thread_cpu_maxAllPasses`，说明那一块大部分时间不在 CPU 上（被抢占或停顿），不是插件真超时。最慢那块的 CPU 时间不会超过全部块的 CPU 最大值。这时再看 `invol_switches`。

**「分析器 + 控制层」估计行**（`row_kind = estimate`、`basis = <场景>-minus-<孪生>`）：
- 算法：同层、同采样率、同块长下，GLAZE > 0 那行减 GLAZE 0 孪生那行（两行只差 GLAZE）。
- **这是估计，不是分级实测**：
  - 分析器（MusicAnalyzer、实时画像、出口电平跟踪）在 GLAZE 0 也在跑，所以差里**不含**它。
  - 差里**含**决策 / 守护 / 方案执行、GLAZE > 0 按 10 ms 时隙切片的开销，还有模块因实际参数变了带来的增减（例如 Heat 开起来以后 Color 的过采样）。
- 只对耗时类做差；`maxAllPasses`、计数与状态写空值，`na_reason = estimate_not_differenced`。
- 精确拆分要等 f2 探针。

**判与报的边界**：
- 192k / 32 只报不判；估计行只报不判（`compare_perf.py` 顶部 `REPORT_ONLY_CONFIGS`、`GATE_ESTIMATE_ROWS`）。
- 预算（48k/32 最慢块 ≤ 15 %、96k/64 ≤ 20 %、48k/512 均值 ≤ 6 %）维持原值，只在 Mac 读数上判。

**`na_reason` 代码**（Q12：算不出写空值并注明原因，绝不填 0）：

| 代码 | 含义 |
|---|---|
| `segments_chain_only` | 两段拆分只在链层有，处理器层这几列为空 |
| `thread_cpu_unavailable` | 读不到线程 CPU 时间 |
| `rusage_thread_unavailable` | 系统没有 `RUSAGE_THREAD`（如 macOS） |
| `estimate_not_differenced` | 估计行不对 `maxAllPasses` / 计数 / 状态做差 |

## 4. 场景、素材、宿主时间线

| 场景 | GLAZE | 其余 | GLAZE 0 孪生 |
|---|---|---|---|
| `default` | 出厂（35） | 出厂：通用、标准、Gain 0 | `default-g0` |
| `manual` | 0 | Gain 9（同 `bench.mm`） | — |
| `auto100` | 100 | 出厂 | `default-g0` |
| `house-club-35` / `house-club-100` | 35 / 100 | Deep House + 俱乐部 | `house-club-g0` |
| `general-standard-35` / `-100` | 35 / 100 | 钉死通用 + 标准 | `general-standard-g0` |

- `default` 跟 `ParameterDefaults.h` 走。`general-standard-*` 把参数钉死：今天两者按值相同，出厂默认以后改了它也不动。
- 点名 GLAZE > 0 的场景时，自动在它后面补测孪生。

**素材**（`makeMaterial`，纯函数、固定种子）：
- 124 BPM，32 小节 ≈ 61.9 s 一圈，循环喂。
- 结构：前奏（开镲第 3 小节起，底鼓第 5 小节起）/ drop（全编制）/ breakdown（无鼓无贝斯，铺底更响，后 4 小节噪声上扬）/ drop，各 8 小节。
- drop 比前奏、breakdown 响 ≥ 3 dB（自检核对）。峰值归一到 −1 dBFS。

**宿主时间线**：两层看到同一份宿主信息——在播、位置 = 从 prepare 起的样本数、124 BPM、4/4、小节起点。House 档按小节记账要拍点。注意 `bench.mm` grid 不给播放头，这是两者的一处差别。

**默认长度**：空跑 20 s（< 20 s 会在 stderr 提示）；每遍 = 素材一整圈（`--pass 0`）；3 遍。各遍内容相同，取「最好的一遍」才有意义。

## 5. DGX 复现（主会话在独占时段跑）

先按 vault「踩坑-本机基准与他人编译撞车」办：在 TASKS ② 登记「DGX 性能基准独占」，给在跑的 Celadon 会话发消息，跑完释放。

```bash
SPARK=~/.cache/celadon-spark/bin/spark-test.sh
SRC=~/.cache/celadon-spark/src
OUTDIR=~/.cache/celadon-t096f-perf

# 1. 源码目录：当前 = 本分支；基线 = 冻结提交 3a7a367（Q5），再补上基准源码与本 CMake 段（3a7a367 里还没有这个目标）
#    （git archive 在有仓库的一侧做，例如 Mac：git archive 3a7a367 | ssh max-spark 'mkdir -p …/t096f-base && tar -x -C …/t096f-base'）
cp $SRC/t096f/tools/perf/bench_portable.cpp $SRC/t096f-base/tools/perf/
grep -q '可移植 CPU 基准（T-096f）开始' $SRC/t096f-base/CMakeLists.txt || \
  sed -n '/^# ---- 可移植 CPU 基准（T-096f）开始/,/^# ---- 可移植 CPU 基准（T-096f）结束/p' $SRC/t096f/CMakeLists.txt >> $SRC/t096f-base/CMakeLists.txt

# 2. 编（期望 0 警告）
$SPARK $SRC/t096f-base build celadon_bench_portable 2>&1 | tee /tmp/t096f-build-base.log
$SPARK $SRC/t096f      build celadon_bench_portable 2>&1 | tee /tmp/t096f-build-cur.log
grep -c 'warning:' /tmp/t096f-build-base.log /tmp/t096f-build-cur.log

# 3. 自检与预演
$SPARK $SRC/t096f run bash -c '"$BUILD/celadon_bench_portable" --selftest'
python3 $SRC/t096f/tools/perf/compare_perf.py --selftest
BASE_BIN="$($SPARK $SRC/t096f-base dir)/celadon_bench_portable"
CUR_BIN="$($SPARK $SRC/t096f dir)/celadon_bench_portable"
bash $SRC/t096f/scripts/perf-dgx.sh --dry-run --base "$BASE_BIN" --cur "$CUR_BIN" --out $OUTDIR -- --scenario default --sr 48000 --bs 512

# 4. 真跑（全机锁里；脚本自己起编译观察进程）
$SPARK $SRC/t096f run bash $SRC/t096f/scripts/perf-dgx.sh --base "$BASE_BIN" --out $OUTDIR --rounds 2 -- \
  --scenario default,manual,house-club-35 --sr 48000,96000,192000      # 日常子集；全量去掉 -- 之后的参数
python3 $SRC/t096f/tools/perf/compare_perf.py $OUTDIR/run-<UTC 时刻> > $OUTDIR/compare.md
```

- **判干净**：
  - `compile-activity.log` 必须为空，观察进程心跳 ≥ 1。否则整个目录改名 `contaminated-<时刻>/`，退出码 3，要重跑（Q12）。
  - 一分钟负载上限 `LOAD1_MAX` 现在为空（只记录），起点值等首跑后由主会话填。
  - 调速器不是 `performance` 只提示。
- **输出**：`run-<时刻>/` 下有 `machine.txt`（调速器、频率、负载、二进制 sha256、参数）、`runs.tsv`（每次的起止与负载）、`compile-activity.log`、`base-N.tsv` / `cur-N.tsv` 与日志、`VERDICT.txt`。归档时整目录拷进本目录。
- **耗时**：
  - 每个配置喂 `空跑 + 遍数 × 每遍` 秒音频，默认约 206 s；墙钟 ≈ 这个数 × 该配置的 CPU 占比。
  - 全量是 10 个场景 × 4 个采样率 × 3 个块长 × 2 层 = 240 个配置。
  - 以首跑日志里 `PROGRESS` 行的 `wall_s` 为准，再定日常子集。
- **只编链层**：在 CMake 配置时加 `-DCELADON_BENCH_PORTABLE_CHAIN_ONLY=ON`。
- **处理器层的显示依赖**：处理器层不建编辑器、不连 X，按理不需要 DISPLAY。万一启动报显示错误，就套 `xvfb-run -a`，Xvfb 不绑核。

## 6. Mac 侧（Max 本人跑）

```bash
scripts/perf-local.sh XcodeRelease grid all 44100,48000,96000,192000 32,64,512 20 10 5   # 含实时 GLAZE 的绝对数；预算在这份上判
scripts/perf-local.sh XcodeRelease rendercheck                                          # 复核 bench.mm 阶段 2 口径的改动
```

跑之前同样登记「本机性能基准」占用；跑的时候本机不能有编译（`perf-local.sh` 开跑前会等编译结束，跑的过程中不再查）。

## 7. 474 % 出处勘误

- **原记法**：
  - T-093 `REPORT.md:384` 记为出自 `modules-after-T020.log`。
  - `J2-feasibility.md:50 / 241 / 273` 记为 vault `TASKS.md:189`。
- **实际原始读数**在 `docs/validation/2026-09-26-intelligent/perf-bass.log:9`：
  `GRID,release,manual,96000,32,avg%=1.951,worst%=9.200,p99.9%=5.588,…,maxAllPasses%=474.263,structWorst%=4.562,…,latency=378,…,blocks=12000`
- **怎么读这一行**：
  - 474 % 是 `maxAllPasses`：所有遍里单次最慢的一块，会混进调度抢占。
  - 同一行的结构性最坏 `structWorst` 只有 4.562 %，`worst`（各遍最坏取最好的一遍）9.200 %。
  - `latency = 378` 说明是 v0.6 代码（Clay 之前；现行 96 k 为 2584）。空跑 2 s，每遍 4 s。
  - 按文件名是 bassgrid（持续低频侧声道素材）跑法；GRID 行本身分不出 grid 和 bassgrid。
- **结论**：这是单块尖峰，不是插件的结构性开销；读数被抢占污染是推断，未证实。要归因，在现行 main 上用新基准重跑 96k/32，对比 `maxAllPasses` 与 `thread_cpu_maxAllPasses`，再看 `invol_switches`（第 3 节）。

## 8. 报告骨架（主会话填）

1. **结论**：同机不退步与否（主判 structWorst）；Mac 预算与否（Max 的读数）；超预算项的归因：哪层、哪类事件、抢占还是真超时。
2. **机器与提交**：DGX `machine.txt` 摘要（核 5 / CPU part / 调速器 / 负载）；基线 `3a7a367` 与当前提交号、两个二进制 sha256。
3. **跑法**：命令原文；轮次顺序与时刻（`runs.tsv`）；`compile-activity.log` 为空的证明；作废重跑的轮次（`contaminated-*/`）。
4. **结果表**：processor / chain 各一张，列 48k/32、96k/32、192k/32（只报）、48k/512、96k/64；GLAZE 0 行、GLAZE > 0 行、估计行。
5. **只报项**：structWorst_1s / _5ms、thread_cpu、invol_switches、p99.9。
6. **474 % 勘误**（第 7 节）与现行 main 上 96k/32 的重跑归因。
7. **「现行」一行**（Q5）：守护族与 T-095 合入后，在现行 main 上补量一行并登记提交号。
8. **待 Max 拍板**：增量阈值起点；负载上限；日常子集。

## 9. 已知局限

- `structWorst_5ms` 第 0 组的中位数只在画像「一遍」覆盖了多数网格点时才显出分片开销；分片稀疏时要和 `p99.9`、`thread_cpu_*` 一起看。`structWorst_1s` 不受此限：每秒都决策。
- 处理器层带着 JUCE 定时器线程：处理器 30 Hz 定时器、不跑消息循环，约 300 ms 重发一次消息。它和基准同绑在核 5 上，会计进切换次数。
- 每块两次 `clock_gettime(CLOCK_THREAD_CPUTIME_ID)`（系统调用）夹在墙钟窗口外，对缓存有轻微扰动。两版二进制一样，同机对比公平。
- DGX 是 GCC 13、无 LTO，Mac 是 clang -O3 -flto，两边绝对数不可比。
- 界面库构建里，链层的 DSP 带 `-include JucePluginDefines.h`、链 `celadon_juce_gui`。要纯 `celadon_juce` 的链层，开 `CELADON_BENCH_PORTABLE_CHAIN_ONLY`。
- 合成素材不是真歌；真歌负载（DGX 22 首 / 45 首语料）本轮没接。

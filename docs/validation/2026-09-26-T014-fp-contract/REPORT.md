# T-014 取证：GCC 默认融合乘加对「各路径逐位相同」的影响（2026-09-26）

服务器 109（github-2，GCC 15.2.0 / clang 21.1.8，CPU 有 AVX2 + FMA），Release -O3。仓库临时副本（scratchpad）经
`scripts/test-linux.sh` 同步到独立远端目录，与 T-012 的构建目录无关；远端临时目录已删。本机没有编译、没有跑测试。

## 1. celadon_tests，只跑 TruePeakDetector, Saturator, ColorModule（HalfBand 用例在 ColorModule 里）

| 构建（CMAKE_CXX_FLAGS / CMAKE_C_FLAGS） | 结果 | 日志 |
|---|---|---|
| 基线（空，SSE2） | 81884 过 / 0 败，0 警告 | `tests-base.log` |
| `-march=x86-64-v3` | **81526 过 / 358 败**，0 警告 | `tests-v3.log` |
| `-march=x86-64-v3 -ffp-contract=off` | 81884 过 / 0 败，0 警告 | `tests-v3-nocontract.log` |

358 项失败的分布：

| 用例 | 失败数 | 说明 |
|---|---|---|
| TruePeakDetector：processBlock / processBlockMax 与 processSample 逐位（SSE2） | 292 | IEEE / FTZ / DAZ / FTZ+DAZ 四种模式全失败，非冲零问题 |
| TruePeakDetector：混合调用 | 6 | 同上 |
| ColorModule：HalfBand 输出与块长无关 | 12 | 块长 1 / 17 / 333 与整块不同 |
| ColorModule：HalfBand 预热复原 | 7 | |
| ColorModule：fastTanh 精度 / 界 | 2 | `fastTanh(±Inf)` 不再恰为 ±1 |
| ColorModule：空闲跳过、块长无关（自动化）、Heat 预算等 | 39 | HalfBand / Saturator 的下游 |

## 2. 对照程序（`dump.cpp`；JUCE 静态库固定用基线编译，差异只来自被测三个文件）

融合乘加指令数（vfmadd / vfnmadd 等，按函数统计见 `run-109.log`）与输出逐位比较（相对 gcc 基线）：

| 配置 | HalfBand | Saturator | TruePeak | 与基线逐位 |
|---|---|---|---|---|
| gcc 基线 | 0 | 0 | 0 | — |
| gcc `-march=x86-64-v3` | 24（Up 4、Down 16，不一致） | 91（fastTanh 核 18） | 8（核 7，参照 0） | **全部不同**（HalfBand 上采样 361207 / 761856 个样本不同；fastTanh 取样 590616 / 1048588，最多 7 ulp；TruePeak 核 11978 / 48000，参照 0） |
| gcc v3 + `-ffp-contract=off` | 0 | 0 | 0 | 全部相同 |
| gcc v3 + `-ffp-contract=on` | 1（designFilter） | 8（set* / updateTube） | 2（构造函数） | 全部相同（只合并同一表达式，核不受影响） |
| gcc v3 + 三个文件加 `#pragma GCC optimize ("fp-contract=off")` | 0 | 0 | 1（构造函数，漏网） | 全部相同，但 pragma 没覆盖头文件里先声明的成员函数 |
| gcc 强制标量回退 | 0 | 0 | 0 | 全部相同（基线下「三条路径逐位相同」成立） |
| gcc 强制标量回退 + v3 | 218 | 36 | 8 | 全部不同；**与 gcc v3 的 SSE2 路径也不同**（HalfBand 上采样 175449 个） |
| clang `-march=x86-64-v3`（默认 on） | 5（designFilter） | 8（set*） | 1（构造函数） | 全部相同 |
| clang v3 + `-ffp-contract=fast` | 49 | 91 | 78 | 全部不同（TruePeak 核与参照两边都合并，本构建内仍一致） |

fastTanh 在 FMA 下：`fastTanh(+Inf) = fastTanh(7.9053111) = 0.999999762（0x3f7ffffc）`，穷举 [0, 8] 最大值 0.99999994；
钳位常数 7.9053… 是 Eigen「不用 FMA」那组，FMA 求值顺序下 p/q 到不了 1。奇对称仍成立、|y| ≤ 1 仍成立。

TruePeak 在 GCC v3 下为什么只合并一边：参照实现 `processSample` 被 SLP 向量化成 8 条 256 位 `vmulps` + 64 条按序的标量 `vaddss`，
乘和加分开了，合并不起来；SSE2 核的 `_mm_add_ps (acc, _mm_mul_ps (…))` 是 128 位乘直接喂加，被合并成 `vfmadd`。
是否合并取决于向量化与调优启发式（`-mtune=generic`，`--param avoid-fma-max-bits=256`），同一份源码不同循环编出来不一样。

## 3. 机理与其他编译器

- GCC 15.2 对 C++ 默认 `-ffp-contract=fast`，`-std=c++17` 与 `gnu++17` 都是（`-Q --help=common`）。
- GCC 的 `xmmintrin.h`：`_mm_add_ps` / `_mm_mul_ps` 就是向量类型的 `+` / `*`，内联后与普通表达式无异，可跨函数、跨语句合并。
- `-march=x86-64-v3` 定义 `__FMA__`、`__FP_FAST_FMAF`；**加了 `-ffp-contract=off` 这两个宏照样定义**（`run2-109.log`），编译期守卫不能只看它们。
- clang aarch64 小样本（`arm-snippet.cpp`，`--target=aarch64-linux-gnu`）：默认 on 时 NEON 封装写法与两条语句都是 fmul + fadd，
  只有同一表达式 `sum + tap * x` 合成 fmadd；`fast` 全部合并；`off` 全部不合并。
- Mac 正式构建（`Builds/MacOSX/build/Release/libCeladon.a`，2026-09-25 23:53，Apple clang 21，arm64，LTO 位码，服务器上 llvm-dis-21 读）：
  `llvm.fmuladd` 只出现在 `HalfBandOversampler::designFilter`、`Saturator::setDrive / setAsymmetry / setDriveAndAsymmetry / updateTube`、
  `TruePeakDetector::TruePeakDetector`（都是同一表达式里的设参 / 系数代码）；核里没有。TruePeak 的 87 条 `llvm.fma` 是 NEON 核与
  `std::fma` 有意写的。
- aarch64 上的 GCC：服务器没有交叉编译器，**未实测**。GCC 的 `arm_neon.h` 里 `vaddq_f32` / `vmulq_f32` 也是向量类型的 `+` / `*`，
  且 aarch64 基线就有 FMA，按 x86 的结果推断默认同样会合并。
- MSVC：没有环境，未实测。VS 2022 起 `/fp:precise` 默认不合并；TruePeak 另有 `#pragma fp_contract (off)`。

## 4. 全套范围与补丁预跑（Max 拍板 ADR-013 之后，2026-09-26 22:10～22:40，109）

底本：T-012 的 `Celadon-releases/source-baseline-2026-09-26-phase4-before/`（三个目标文件与当时仓库逐字节相同）。它的 CMakeLists
已登记了当时还不存在的 `Source/dsp/LinearPhaseLowpass.cpp`、`tests/LegacyFixtureTests.cpp`，临时副本里只删这两个文件名（括号保留）。
补丁 `fix.patch` 打在同一底本上（`patch -p2`）。

| 构建 | 结果 | 日志 |
|---|---|---|
| 原始源码 · `-march=x86-64-v3` · 全套 | 127694 过 / 385 败 | `tests-scope-v3-full.log` |
| 补丁 · 基线选项 · 全套 | 128063 过 / 16 败，0 警告 | `tests-fix-base-full.log` |
| 补丁 · `-march=x86-64-v3` · 全套 | 128063 过 / 16 败，0 警告；**失败行与上一行逐行相同** | `tests-fix-v3-full.log` |

- 16 项（GlazePlanner 15、ClayGuard 1，差值很大，如期望 14.1421 实得 20）在基线选项下也失败：是底本自身（T-012 阶段 4 进行中）的状态，
  补丁在 SSE2 基线上不改变任何运算，与合并无关。已告知 T-012。
- 所以**合并造成的是 385 − 16 = 369 项、4 个类**：TruePeakDetector 298、ColorModule 60、LimiterModule 10（Clip 空闲跳过 8、块长无关 2）、
  GlazeChain 1（参照与整链逐位）。打补丁后 v3 下这 369 项全部消失。
- 编译命令核对：补丁后 `TruePeak.cpp` 带 `-ffp-contract=off -DCELADON_FP_CONTRACT_OFF=1`。
- 守卫绊线（`run3-fix-109.log`）：三个文件 × 6 种选项共 18 项全对——g++ v3 不带宏报错；g++ v3 带选项与宏通过；g++ 基线通过；
  `-ffast-math`（g++ / clang++）报错；clang++ v3 通过。
- 预跑之后 `fix.patch` 只改过 CMakeLists 的注释措辞（适用范围写成全工程），代码与选项未变。

## 5. 最终验收（2026-09-27，补丁已进真仓库，107）

T-012 于 2026-09-27 00:31（PDT）把 8 个文件释放给 T-014；改前备份 `Celadon-releases/source-baseline-2026-09-27-T-014-before/`，
按 `fix.patch` 逐处 Edit，7 个源文件与预跑补丁逐字节相同，`CMakeLists.txt` 只多出那 9 行。源码其余部分是 T-012 当时的现状。
`scripts/test-linux.sh bash -c "$(cat run4-final.sh)"`（107 = githubci，GCC 15.2，Release -O3）：

| 项 | 结果 | 日志 |
|---|---|---|
| 基线全套（共享构建目录，缓存里 `CMAKE_*_FLAGS` 为空） | **129304 过 / 0 败，0 警告**；`TruePeak.cpp` 带 `-ffp-contract=off -DCELADON_FP_CONTRACT_OFF=1` | `final-107-tests-base.log`、`final-107-build-base.log` |
| `-march=x86-64-v3` 全套（独立目录，跑完已删） | **129304 过 / 0 败，0 警告** | `final-107-tests-v3.log`、`final-107-build-v3.log` |
| 守卫绊线（三个文件 × 6 种选项） | **对 18 / 错 0** | `run4-final-107.log` |

结论：BUG-002 已修复——GCC 开 FMA 的构建与基线逐项同样全过；守卫对 GCC 没关合并、`-ffast-math` 报错，对正常构建不误伤。
Mac 端只多了注释与对 clang 为假的 `#if`（`.jucer` / Xcode 工程里没有 fast-math 设置，已核），插件二进制不变。

## 文件

`run.sh` / `run2.sh` / `run3-scope.sh` / `run3-fix.sh` / `run4-final.sh`（远端脚本）、`dump.cpp`、`compare.py`、`arm-snippet.cpp`、`fix.patch`、
`run-109.log`、`run2-109.log`、`run3-scope-109.log`、`run3-fix-109.log`、`run4-final-107.log`、`tests-*.log`、`final-107-*.log`。
远端临时目录（109 与 107）都已删。

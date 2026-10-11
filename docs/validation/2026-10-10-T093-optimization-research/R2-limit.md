# R2 · Limit 模块算法：透明限幅与削波的前沿与改进（T-093）

**一句话结论**：检测和前瞻的对齐已经很扎实（T-032 E2 / V1 都核过），再去抠检测精度收益很小（0.05～0.4 dB）。真正值得做的有三件：一是缺一把尺子，量「每 dB 衰减换来多少响度、带出多少失真和泵感」（R2-01）；二是编码后的过冲（T-085 实测 1.2～7.4 dB）比内部重建误差大一个数量级（R2-02）；三是 House 里由底鼓 / 低音触发的宽带衰减会把踩镲一起压下去（R2-03）。另外，T-047 第 3c 步的「残差 ADAA」有个没人指出的半样本错位：2× 下削波级的峰值会多漏 0.35～1.25 dB（R2-04）。

**前 5 条（按价值）**：
1. R2-01 限幅效率尺：ΔL_eff ÷ GR、快调制失真 D_fast、慢调制泵深 P_slow、Clip 残差，渲染工具加列（工具 · M · P1）
2. R2-02 编码过冲后续：Clip 份额 × 编码 A/B、SRC / 带限 / PNS 代理指标、按 PSR 的编码余量（工具 + 控制层 · S～M · P1）
3. R2-03 先测「低频引起的衰减占比」，过门槛再做「低频预限」原型，就此重新评估「多段限幅」这条非目标（Limit · S → L · P1 / P2）
4. R2-04 3c 残差 ADAA 的干路也要做 2 点平均（或改 4×），否则削波级峰值控制会失效（Limit Clip 级 · S · P2）
5. R2-05 限幅跟拍改看自己测到的过载节拍（IOI），宿主 BPM 只作先验和校验（Limit / 链 · M · P2）

> 2026-10-10 · 代号 R2 · 只读调研。本机没编译、没跑 celadon_tests、没跑渲染工具；只在 scratchpad 里跑了 4 个 numpy 玩具仿真（S1～S4，见第 3 节），并用 T-085 已入库的 CSV 算了一次相关。证据标记：【一手】厂商文档 / 标准 / 论文 / 源码；【推断】本代理推理或仿真；【统计】二次统计。

## 1. 提案（按价值排序）

### R2-01 · 限幅效率尺：用「每 dB 衰减换多少响度」和「快 / 慢调制」两组数比较各种限幅策略（工具 · M · P1）

- **问题 / 现状**：现在比较限幅策略，只有 `BlockStats` 的 GR 均值 / 最大值和 Clip 活动占比可看（`Source/dsp/LimiterModule.h:114-121`），再加上 Max 试听（BRIEF §5「好听没有客观验收」）。GR 均值分不出「短而深」（鼓头，基本听不出）和「长而浅」（泵）。`clipActiveFraction` 只数 |x| > T 的样本，不数 0.7T～T 的拐点区（`Saturator.h:101`、`LimiterModule.h:118-119`），可 S2 显示拐点区正是软削波残差的主体：拐点 0.7 时被整形的样本占 1.70 %，超过 T 的不到十分之一。本报告的 R2-03 / 05 / 06 / 07 / 08 都要一把「等响下谁更干净」的尺子才能裁决。
- **提案**：`celadon_render` 加 `--limit-metrics`。LimiterModule 加一个只给测试用的钩子（写法照 `setClipIdleSkipEnabledForTesting`），交出每样本的限幅增益 g[n]，以及 Clip 级 1× 的输入 / 输出。离线算 6 个量，进指标表：
  1. **ΔL_eff** = L_I(y) − [L_I(x) + (C_eff − TP(x))]：比「纯增益归一到同一 Ceiling」多换来的响度（LU）。x 是加 Gain 前的 Limit 输入。
  2. **GR 均值 / p95**（沿用现有口径）。
  3. **η = ΔL_eff ÷ GR 均值**（LU / dB），也就是「每 dB 衰减换多少响度」。
  4. **D_fast** = 10·log10(Σ(x_d·(g − g_LP20))² ÷ Σy²)：增益里 20 Hz 以上的调制（粗糙度 / 失真区）作用到信号上的能量。g_LP20 是对 g 做 20 Hz 零相位低通（离线双向滤波）。
  5. **P_slow**：20·log10 g 经 0.5～8 Hz 带通后的 RMS（dB），即落在起伏度区的泵深。
  6. **D_clip** = 10·log10(Σ(y_clip − x_clip)² ÷ Σy²)，外加它在 8 kHz 以上的份额。

  比较口径：对每个变体都用二分法调 Limit Gain，让输出 LUFS-I 相差在 ±0.05 LU 以内，再比 D_fast + D_clip、P_slow。可选：对等响后的成对音频再跑 GstPEAQ（advanced）ODG 或 ViSQOL v3 audio，作第三方交叉核对。
- **依据**：Pro-L 2 把限幅拆成「瞬态级（前瞻）」与「释放级（响应平均动态）」两级，联动也分两路设【一手 FabFilter】。Limitless 写的是「Dual-stage dynamic handling separates transients and dynamics」【一手 DMG】。Ozone 把立体声独立度拆成 Transient / Sustain【一手 iZotope】。快 / 慢两分是业界共识，指标按同一条线分。调制感知的分界：起伏度峰在约 4 Hz，粗糙度峰在约 70 Hz，交界 15～20 Hz【二手，Fastl & Zwicker 的摘要，见证据索引】。GstPEAQ（Holters & Zölzer，DAFx-15）和 ViSQOL v3（Chinen 等，QoMEX 2020）都是开源实现【一手】。S1 显示，换窗形或改前瞻长度会让 1 kHz / 3 kHz 以外的边带能量差 3～17 dB；这类差别听不听得出，要先有尺子才能讨论【推断】。
- **对硬约束 / ADR**：只加测试钩子（插件自己从不调用），输出、状态、统计都逐位不变；GLAZE 0、延迟、CPU 不受影响。渲染工具只新增选项，默认关。
- **代价与风险**：M（C++ 钩子 + 渲染列约 200 行，Python 分析约 200 行）。风险：指标和听感怎么对应，要 Max 试听来标定。这把尺子只用来排序和抓回归，不当验收门槛。
- **怎么验**（在 DGX 上跑）：自检：纯增益（g 恒定）→ D_fast = −∞、P_slow = 0；正弦加已知调幅（10 Hz / 80 Hz，±1 dB）→ P_slow / D_fast 与解析值误差 < 0.1 dB；Clip = 0 → D_clip = −∞。然后对 45 首 Jamendo House 加 22 首门槛曲出一张基线表，入库。
- **对 Max 口味**：不改声音。它给「全自动」调参和 Max 试听一条共同的数轴，可以减少 A/B 的轮数。

### R2-02 · 编码过冲后续：先查清 Clip 有没有加重过冲，再找便宜的代理指标，最后才谈程序相关的编码余量（工具 + 控制层 · S～M · P1）

- **问题 / 现状**：T-085 的结果：39 个成品的 PCM 真峰值全部 ≤ −2.29 dBTP，但编码后最坏过冲 MP3 320k 是 +1.15 dB，AAC 128k（PNS 开）是 +7.38 dB；关掉 PNS 后 AAC 仍有 +2.40 / +4.37 dB（`docs/validation/2026-10-08-T085-codec-tp/REPORT.md`）。现有手段：编码余量最多 1.5 dB，有效 Ceiling 不低于 −4.5 dBTP（`CeladonChain.cpp:266-275`、`LimiterModule.cpp:111-112`）。本代理用 T-085 已入库的 CSV 重算了一次（每种编码各 27 个「真歌 × 目标」配对，不独立）：过冲与限幅 GR 均值的相关 r = +0.43～+0.62，与 PSR 的 r = −0.54～−0.76，与峰值因数的 r = −0.45～−0.71。**越密的母带过冲越大**（第 3 节 S5）。但渲染 CSV 里没有 Clip 量这一列，「Clip 份额是否加重编码过冲」无从回答。
- **提案**：
  - **(a) A/B 实验**：同一批歌，比三组：风格「通透」（λ = 0.8 / 0.84 ≈ 0.95，Clip 拿 5 %）、风格「冲击」（λ ≈ 0.59，Clip 拿 41 %，`GlazePlanner.cpp:894`、`GlazeConstants.cpp` 三种风格的 share）、手动 Clip 0。三组用 R2-01 的二分法等响后，用现有 `codec_tp_check.py` 测编码过冲；渲染 CSV 补上 clip 参数、`clipDepthMaxDb` 和 D_clip 三列。判据：冲击组减通透组的过冲中位数，在 AAC / Opus 上若 > 0.5 dB，流媒体档就该给 Clip 份额封顶（规划器的位置交 R6）。
  - **(b) 代理指标**（加进 `codec_tp_check.py`）：① SRC 过冲：48k → 44.1k（soxr 最高质量）后的 TP 减 PCM TP；② 带限过冲：16 kHz / 19 kHz 线性相位低通后的 TP 增量；③ PNS 代理：STFT 里 5 kHz 以上的 bin 保留幅度、随机化相位后的 TP 增量。每个代理都和实测编码过冲算 Spearman 相关，r ≥ 0.8 的才收进 `celadon_render` 当便宜的交付检查（以后可能做成插件里的风险提示，本提案不含）。
  - **(c) 程序相关的编码余量**（流媒体档）：余量 = clamp(a + b·(PSR_ref − PSR_实时), 0, M)，PSR 用 LiveProfile 已有的读数。但 T-085 推导出的需要量（关 PNS 的 AAC / Opus 96k 要 −3.6～−6 dBTP 才能编码后 ≤ −1）超出了现有范围，所以先拍板口径再做（见「待用户拍板」2）。控制层的位置交 R5 / R6。
- **依据**：T-085 实测与 CSV【一手，库内】；相关系数【统计，本代理据 CSV 算，n = 27，不独立，只作方向】。Apple Digital Masters：macOS 自带 AURoundTripAAC 与 afclip，建议编码前后都查削波【一手 Apple】。Nielsen & Lund（AES 109，2000；AES 第 23 届会议，2003）：0 dBFS+ 与 SRC / 数据压缩环节的过载【一手，论文】。Spotify：响于 −14 的母带建议真峰值低于 −2【一手，经 T-085 引用】。
- **对硬约束 / ADR**：(a)(b) 只动工具。(c) 只改流媒体档、只在 GLAZE > 0 时生效，GLAZE 0 交给模块的值不变；流媒体档的旧工程峰值会更低，需要新 ADR。
- **代价与风险**：(a) S，(b) S～M，(c) M，且要先拍板。风险：T-085 只测了 ffmpeg 原生 AAC、libopus、LAME，代理指标未必能推广到 Apple AAC / FDK。Apple AAC 只能在 Mac 上编（`afconvert` 或 ffmpeg 的 `aac_at`），见「待用户拍板」5。
- **怎么验**：(a) 9 首真歌 + 4 首压力曲 × 3 组 × 6 种编码，报告过冲中位数和最坏值；(b) 每种编码 39 个条件上的代理 Spearman r；(c) 留出的歌上，编码后 ≤ −1 dBTP 的通过率。
- **对 Max 口味**：流媒体版是 Max 的两种交付之一；俱乐部版不动；不引入预设。

### R2-03 · 先测「低频引起的衰减占比」，过门槛再做「低频预限」原型：重新评估「多段限幅」非目标（Limit · 阶段 1 S / 阶段 2 L · P1 / P2）

- **问题 / 现状**：Glue 侧链有 100 Hz 高通（`ColorModule.h:10`），Heat 只处理 150 Hz 以上（`ColorModule.h:16`），所以底鼓 / 低音的峰值基本原样落到 Clip 和限幅器上。限幅器对全频带只用一个增益（`LimiterModule.cpp:885-924`），每个底鼓都会连踩镲 / pad 一起压下去。现有的缓解只覆盖一部分：低频周期释放下限只认「持续、周期稳定」的低音（`:943-998`；生效门在 `:1012-1014`：连续 3 个周期一致、低频能量 ≥ 35 %、持续 120 ms），底鼓（音高下滑、衰减快）被明确排除在外（`LimiterModule.h:24-25`）；House「一拍 ÷ 3」只缩短慢段释放。S3 玩具仿真：只有底鼓过载 6 dB 时，底鼓后 0～60 ms 的踩镲比 150～300 ms 低 **5.3～5.4 dB**（释放 30 ms 和 80 ms 几乎一样）。这是按拍速（约 2 Hz，正落在起伏度区）发生的高频泵。改成「只压 150 Hz 以下」后泵深降到 0.32 dB，代价是近似响度低约 0.5 dB、底鼓「肉 / 头」比例变了。总体规格的非目标写的是「多段限幅（v0.3 试听后再评估）」（`01-框架/设计规格-总体.md:18`），现在已是 0.1.8.10，本提案就是这次「再评估」的依据和办法。
- **提案**：
  - **阶段 1（测量，S）**：在 R2-01 的工具里，每个过载峰（required < 1 的峰值样本 n*）算低频占比 s = |LF(n*)| ÷ |x(n*)|，LF 用观察器同款 150 Hz 低通。由此得到「低频引起的衰减占比」= Σ GR_dB·[s > 0.6] ÷ Σ GR_dB，和高频带（> 5 kHz）的 P_slow。在 45 首 House 和 22 首门槛曲上跑。判据：占比中位数 ≥ 50 % 且高频 P_slow ≥ 1 dB 才进阶段 2；否则维持非目标，结案。
  - **阶段 2（原型，L，按「原型例外」放独立分支、不进 main）**：在现有宽带核心之前加一级低频预限：y = x + (a − 1)·LP(x)。LP 用 120 Hz 左右的 2 / 4 阶 IIR，零延迟；a 恰好等于 1 时 y 逐位等于 x。a 按样本求：峰值超限且低频占比高时，在峰值样本上解 |HF + a·LF| ≤ c'，a 下限 −6 dB；然后走同一套滑动最小 / 释放 / 攻击整形，低频释放 ≥ 2 个周期（60～120 ms）。宽带级照旧做最后的真峰值保证，所以「≤ Ceiling + 0.01」的门槛不变。低频级要自己的前瞻（1 ms，48 kHz 下 +48 样本），因为宽带检测器必须看到降过低频的信号。
- **依据**：Ozone IRC IV「limits frequency bands that contribute most to these peaks … reduces intermodulation」，用「dozens of psychoacoustically spaced bands」【一手 iZotope】。Elevate 26 个 Mel 频段 + Adaptive Gain / Speed，削波器在链尾【二手，SOS 评测】。Limitless 最多 6 段线性相位分频，官网称「Bit-transparent」，外加双级与真峰值抑制【一手 DMG】。S3【推断，玩具：理想分频、样本峰检测，只说明机理和量级，不代表真歌占比】。
- **对硬约束 / ADR**：常开的话，GLAZE 0 下只要在限幅就会改输出（先例 0.1.7.2）→ 要拍板「只在 GLAZE > 0 时生效」还是「新增参数（只许末尾追加）」。延迟 +1 ms（目标 ≤ 10 ms，T-088 阻塞中）。CPU：一对双二阶 + 第二个真峰值检测器（三个检测器 NEON 合计 0.12 %，加一个约 +0.04 %）。块长无关：IIR 加逐样本逻辑，结果确定。ADR-004（检测域）在宽带级照旧。限幅损失 / 平均 GR 等规划器常数（`GlazeConstants.h:320-324`）要重拟合。
- **代价与风险**：阶段 1 是 S，阶段 2 是 L。主要风险是底鼓变薄、低频动态被「摆弄」，可能出现 Max 对自动收单那样「不自然、有点假」的反应，所以必须盲听。另外要防它和 Balance / Clay 低切互相打架。
- **怎么验**：阶段 1 按上面的判据。阶段 2：真峰值网格、块长 {1, 7, 32, 64, 480, 512, 4096} 逐位一致、不限幅时 null；R2-01 等响对照要求高频 P_slow 降 ≥ 2 dB、D_fast 不变差、ΔL_eff 损失 ≤ 0.3 LU；最后 Max 对 5 首 House 盲听 A/B。
- **对 Max 口味**：全自动，House 俱乐部版受益最大；不碰收单和加宽。「假」是头号风险，结论必须以盲听为准。

### R2-04 · T-047 第 3c 步「残差 ADAA」要补的一点：干路也得做 2 点平均（或改 4×），否则削波级的峰值会漏出来（Limit Clip 级 · S · P2）

- **问题 / 现状**：T-047 REPORT-B §7c 建议把 Clip 写成 `out = x_延迟 + down(ADAA(r(up(x))))`（r = f − x），目的是避开 ADAA 在线性区那个 2 点平均的高频损失（2× 下 20 kHz 处 −2.0 dB）。可一阶 ADAA 的输出相当于在半个（过采样域）样本之后求 f：线性区 r ≡ 0，结果精确；但在削波区，残差和没有延迟的干路错开了半个样本，两者相加不再等于 f(x)，峰值就超出阈值。S4（理想 FFT 上下采样，过阈值 6 dB，阈值 T）在下采样后的超出量如下：
  | 曲线 / 倍率 | 现行纯过采样 | 全量 ADAA | 残差 ADAA（干路不平均） |
  |---|---|---|---|
  | 软拐点 0.7，2×，1 kHz / 3.1 kHz | +0.00 / +0.13 dB | +0.00 / +0.10 | **+0.35 / +1.25** |
  | 软拐点 0.7，4× | +0.00 / +0.13 | +0.00 / +0.12 | +0.16 / +0.71 |
  | 硬削波，2× | +0.08 / +0.32 | +0.05 / +0.24 | **+0.51 / +1.40** |
  | 软拐点，1×（T-047 说的 192 kHz「不过采样 + ADAA」） | +0.00 / +0.28 | +0.00 / +0.13 | +0.74 / **+2.27** |

  错位不影响混叠：残差 ADAA 和全量 ADAA 的混叠一样（2× 软拐点 3.1 kHz：−79.5 对现行 −64.2 dB），坏掉的只是峰值控制。漏出来的部分会被限幅器悄悄接走：GR 变多，GLAZE 按现行映射实测拟合的 Clip / 限幅常数（`GlazeConstants.h:314-326`）随之失准。
- **提案**：3c 立项时三选一：(i) 干路也做同样的 2 点平均（等于全量 ADAA，峰值控制恢复），湿路后加一段固定的短线性相位 FIR，补回 2 点平均的高频损失；(ii) 残差 ADAA 改在 4× 上做（错位减半，但仍漏 0.16～0.71 dB）；(iii) 干路的半样本点从再升一级 2× 插值里取。另外，3c 的验收除了混叠，必须加一项「Clip 级输出真峰值 − 阈值」。还有一点 3c 没写：一阶 ADAA 只有 1 个样本的记忆，保得住「空闲跳过 + 预滚逐位等价」；多相 IIR 半带（3c 的选项 b）保不住。这一点支持「(i) + 短 FIR 半带」的组合。
- **依据**：Parker / Zavalishin / Le Bivic（DAFx-16）、Bilbao / Esqueda / Parker / Välimäki（IEEE SPL 2017）提出并完善了一阶 / 二阶 ADAA【一手】；Vicanek 的笔记写明一阶 ADAA 有半样本延迟（T-047 [S13]）【一手】；S4【推断，仿真】。
- **对硬约束 / ADR**：现在什么都不改，只在 T-088 第 3 步实施时生效；3c 本身改输出、要拍板，这一点 T-047 已写。
- **代价与风险**：S（一条设计说明 + 3c 验收多一个用例）。
- **怎么验**：3c 实现时，Clip 100，输入 1 k / 3.1 k / 9 kHz 正弦与粉噪，过阈值 6～12 dB，量 Clip 级输出的 8× 真峰值减阈值：新实现 ≤ 现行 4× FIR + 0.05 dB，混叠不劣于现行。
- **对 Max 口味**：听不出来；它保住的是规格里「Clip 30～50 时同响度比纯限幅更干净」这个目标。

### R2-05 · 限幅跟拍改看自己测到的过载节拍（IOI），宿主 BPM 只作先验和校验（Limit + 链 · M · P2）

- **问题 / 现状**：`beatReleaseCapFor`（`CeladonChain.cpp:51-61`）只在「House 档 + 自动释放 + 宿主速度有效 + 正在播放 + BPM 100～140」时给出上限。规格自己写了风险：「工程速度可能不等于音频速度，例如在默认 120 BPM 的空工程里母带成品」，以及「不做音频节拍跟踪（第一期）」（`01-框架/设计规格-曲风-House学习方式.md` 第 8 节）。常见的母带做法正是把成品拖进新工程，这时宿主 BPM 往往不对；离线渲染还得手动给 `--bpm`。慢段释放的自适应映射也几乎用不到：满 12 dB 才取上限（`LimiterModule.cpp:46`），而风格预算把平均 GR 封在 1.8～3 dB，实际只在 100～275 ms 之间动（规格第 8 节：平均 GR 1.5 dB 时约 190 ms）。
- **提案**：LimiterModule 自己测过载起点的周期。起点 = required 在高于 0.93 至少 150 ms 之后跌破 0.93；最近 32 个起点存进定长环。在 250～750 ms 内按 5 ms 一格做 IOI 直方图，含 ×2 / ÷2 折叠；置信度 = 落在众数 ±3 % 以内的 IOI 占比。最近 8 s 置信度 ≥ 0.6 时，上限 = IOI ÷ 3（沿用 `beatReleaseDivisor`）。宿主 BPM 与 IOI 相差不到 3 % 时当先验，相差大时忽略，状态行写成「跟拍：按音频 126 BPM（宿主 120）」。宿主停着、离线渲染没给 `--bpm` 时也照样能用。曲风门控不变，仍由链决定交不交开关。顺带：用 R2-01 的 P_slow 重标 `deepReductionDb`。
- **依据**：规格第 8 节自己写明的风险【一手，库内】；Pro-L 2 释放级「respond to the average dynamics」【一手】；用 IOI 聚类求速度是节拍跟踪的经典起点（Dixon，JNMR 30(1)，2001）【一手，凭记忆，未核全文】。
- **对硬约束 / ADR**：跟拍上限本来就不看 GLAZE（`03-记忆/踩坑-官网说明页的绝对说法要对着代码核对.md` 已记为 GLAZE 0 的例外之一），本提案不扩大这个例外：只在现在会给上限的那些情形里，换一个更可靠的速度来源。按样本计时、块长无关；`reset()` 清空。延迟为 0，CPU 可忽略。宿主速度正确、音频周期一致时，输出与现在逐位相同。
- **代价与风险**：M。风险：误认周期（例如过载由踩镲触发）、倍半速认错。有界：最短仍受 `slowReleaseMinMs` = 100 ms 限制。
- **怎么验**：合成四四拍 118 / 124 / 128 / 174 BPM，宿主 BPM 给错或不给 → 上限 = IOI ÷ 3 ±3 %；无周期素材 → 不封顶，与现状逐位相同；45 首歌给与不给 `--bpm` 的上限一致率；R2-01 的 P_slow 对照。
- **对 Max 口味**：全自动，不用 Max 每次去设工程速度；House 优先。

### R2-06 · 短过载判定改成连续映射，并把「短 / 长过载各占多少衰减」交给 Clip / 限幅分工（Limit + 规划器 · M · P2）

- **问题 / 现状**：短过载的判定是二值的（`LimiterModule.cpp:54-56, 1057-1079`）：短于 70 ms 就有 35 ms 的「慢段不低于快段」，70 ms 以上就没有。规格修订记录里，12 ms 和 280 ms 两种过载之后，尾部差 6.6 dB。时长按 `held` 数，而滑动最小值会把每次过载拉长 L + 63 个样本，所以分界随前瞻漂移：2 ms 前瞻多 3.3 ms，10 ms 前瞻多 11.3 ms。Clip / 限幅分工 λ 是风格常数（`GlazePlanner.cpp:894`：暖 ≈ 0.86、冲击 ≈ 0.59、通透 ≈ 0.95），与内容无关。规划器分不出超限是「鼓头尖刺」（削掉会被时间掩蔽，代价低）还是「持续的密集段」（削波失真一直在）。
- **提案**：(a) 限幅器里改成连续权重 w(d) = smoothstep(d; 40 ms, 120 ms)，d 按 `required`（保持之前）来数，不再受前瞻影响；「慢段不低于快段」的窗长 = (1 − w)·35 ms。(b) `BlockStats` 新增两个只读字段：按过载时长分类的 GR·样本合计（短 / 长），音频零改动。(c) 交给 R6 / R5：规划器用 30 s 尺度的短过载占比 ρ，在风格带内移动 λ：λ_eff = clamp(λ_风格 + k·(ρ_ref − ρ), λ_风格 ± 0.15)。短过载多就多给 Clip，持续过载多就多给限幅器。
- **依据**：Pro-L 2 的瞬态级 / 释放级分开设联动【一手】；Limitless「separates transients and dynamics」【一手】；Ozone Transient / Sustain 分开设独立度【一手】；Giannoulis / Massberg / Reiss 2013（JAES 61(10)）用瞬态检测区分「瞬态 / 准稳态」并给不同时间常数，专利 EP3588776B1 引用它并给出连续映射 τ_A = τ_A,max ÷ a(n)、τ_R = a(n)·τ_R,min【一手，经专利原文转述；论文正文未读】。
- **对硬约束 / ADR**：(a) 只要自动释放在限幅就改输出，GLAZE 0 也一样（先例 0.1.7.2）→ 待拍板。(b) 不改。(c) 只在 GLAZE > 0 时生效，GlazeGuard 的 Clip 预算不变，规划器常数要重拟合。
- **代价与风险**：M。λ 自适应会和 GlazeGuard 的 Clip 活动预算相互作用，要防来回振荡（ρ 用 30 s 时间常数，λ 每秒最多变 0.02）。
- **怎么验**：单元测试：10～200 ms 的过载突发 → w 连续，没有台阶；前瞻 1 ms 和 10 ms 分类相同。R2-01 等响对照（45 首）：D_clip + D_fast 与现状相比。
- **对 Max 口味**：正合「根据音乐整体状态全自动」，大旋钮不是把所有旋钮一起调大。

### R2-07 · 瞬态级部分联动：先测联动的代价，够大再做（Limit · S → M · P3）

- **问题 / 现状**：联动是两声道峰值取最大、100 % 联动（`LimiterModule.cpp:885-891`）。完全居中的内容 r_L = r_R，联动不起作用，所以底鼓 / 低音不受影响。真正付代价的是不对称的峰值（偏置的打击乐、宽 synth），但它在 House 里到底亏掉多少响度，目前不知道。
- **提案**：阶段 1（R2-01 里测）：联动代价 = 负荷较轻的声道多吃的 GR（GR_link − GR_own），按 GR 加权；再估算「只对短过载（R2-06 的 w > 0.5）不联动」能换回多少 ΔL。判据：中位数 ≥ 0.2 LU 才进阶段 2。阶段 2：每声道 held_ch′ = held_ch^(1−λT)·held_link^(λT)（在 dB 域插值；held_link ≤ held_ch，所以每个声道的真峰值约束照样成立），短过载 λT = 0.5，释放包络 100 % 联动。声像漂移有界：只在短于 70 ms 的事件里，ΔGR_LR ≤ (1 − λT)·(GR_link − GR_own)。
- **依据**：FabFilter：瞬态联动「often works well to choose less than 100%」，「removing a short peak in only one channel is often nearly inaudible」，释放联动从 100 % 起步【一手】；Ozone：Transient / Sustain 独立度能更响，但「can result in a narrow stereo image」【一手】。
- **对硬约束 / ADR**：阶段 1 不改；阶段 2 在限幅不对称内容时改输出，GLAZE 0 是否生效待拍板；纯居中内容逐位不变。CPU 很小。
- **代价与风险**：S → M。风险：踩镲、拍手的声像瞬间偏移。
- **怎么验**：阶段 1 按判据。阶段 2：真峰值网格；纯居中输入与现状逐位相同；ΔGR_LR 的 p99 ≤ 1 dB；Max A/B。
- **对 Max 口味**：不加宽（Max：母带不拉宽）。只改瞬间的左右平衡，所以排在后面。

### R2-08 · 前瞻长度 × 攻击窗形的 A/B 方案：不是白捡的好处，先量再定（Limit · S · P3）

- **问题 / 现状**：T-047「一档」建议默认前瞻 2 → 1 ms；攻击窗是两级滑动平均（`LimiterModule.cpp:1029-1038`）。S1（单个瞬态过载，统计增益调制谱里偏离载波一定频率以上的能量，单位是「占调制总能量的 dB」）：两级窗下，前瞻 2 ms → 1 ms，1 kHz 外的边带能量从 −60.2 升到 −43.0 dB（+17），3 kHz 外从 −81.9 升到 −68.9 dB（+13）。换三级 / 升余弦窗：2 ms 时 3 kHz 外再降 12～17 dB（−94.2 / −99.4），1 kHz 外降 3～5 dB；**1 ms 时 3 kHz 外降 10 dB，但 1 kHz 外反升 4 dB**（−43.0 → −38.7）。是取舍，不是白捡。
- **提案**：先不改默认。用 R2-01 跑一个网格：前瞻 {1, 1.5, 2, 3, 5} ms（现成参数）× 窗形 {两级（现行）、三级、升余弦}（窗形由测试钩子选），等响后报 D_fast、ΔL_eff 和瞬态保留量（起音前 5 ms 能量）。三级窗的长度满足 L₁ + L₂ + L₃ − 2 = L，到达时刻不变。只有在选定的前瞻下 D_fast 降 ≥ 1 dB 且 ΔL_eff 不亏，才采用三级窗。T-088 若选 1 ms，就重跑一遍。
- **依据**：S1【推断，仿真】；Signalsmith：box 滤波串联更平滑、平滑长度不超过保持窗就不会过冲【一手，技术文章】；Sanfilippo（IFC-22）：串联指数平滑器做低失真限幅【一手，论文 + 开源实现】；Pro-L 2：前瞻短保瞬态、更响但有失真风险，短于 0.1 ms 近似硬削波【一手】。
- **对硬约束 / ADR**：换窗后只要在限幅就改输出（GLAZE 0 也一样）；保持窗 L + 63 与 E2 的对齐结论不变；延迟不变。T-032 R10-09（固定按最大前瞻上报）不在本提案范围。
- **代价与风险**：S（只测量）。
- **怎么验**：网格结果表；若采用，跑现有的真峰值 / null / 块长 / 分块无关用例。
- **对 Max 口味**：透明正是「Invisible Limiter 式」的核心。

### R2-09 · 末端口径：给硬钳位加命中计数；Listen 加一个长窗档（末端 · S · P3）

- **问题 / 现状**：硬钳位（`LimiterModule.cpp:916-924`）在设计上不会触发，理由有三：检测器第 0 相就是原样本（`TruePeak.h:10-12`），采样点峰值永远不会低估；增益在整个保持窗内 ≤ 需求（T-032 E2）；检测用的是 Ceiling × 0.9605，所以样本 ≤ Ceiling − 0.35 dB。唯一的缝是 Ceiling 斜坡时钳位用的 Ceiling 早 32 个样本（< 0.1 dB，T-032 R1-K）。T-085 的 39 个成品里有 31 个（9 首真歌的全部 27 个 + 4 个压力曲）采样峰恰为 −2.35 dBFS（流媒体封顶 −2 再减 0.35，`masters.csv`），印证了这一点。所以可闻性约等于 0，但实际使用中没有任何测量能证明它。Listen 用 1 s 窗的 K 加权能量比，再按每 100 ms 0.3 平滑（τ 约 0.28 s，`CeladonChain.cpp:254, 681-682`），补偿会在 1～2 s 内跟着段落响度走。等响 A/B 因此听不出 breakdown 对 drop 的对比被压平了，而这恰恰是 ContrastGuard 守的那件事。
- **提案**：(a) `BlockStats` 加只读的 `hardClipSamples`（|delayed × gain| > ceilingAtOutput 的样本数），渲染门槛加一条「全部语料 hardClipSamples == 0」。(b) Listen 加长窗档：匹配增益取「开 Listen 以来」的门控整体能量比（或 10 s 窗），稳定后冻结，A/B 里能听出宏观动态的差别。界面与控制层的取舍交 R5。
- **依据**：代码推理【推断】；BS.1770 的门控整体响度【一手，标准】。
- **对硬约束 / ADR**：(a) 不改输出；(b) 只动监听路径，导出时 Listen 本来就要关。
- **代价与风险**：S。
- **怎么验**：(a) 单元测试：Ceiling 在 20 ms 内从 −3 拉到 0、输入很响 → 计数为 0；加进渲染门槛。(b) 用 `make_synthetic_house.py` 的段落，人为把对比压掉 2 LU → 长窗 Listen 下各段 A/B 的响度差 = 对比变化 ±0.2 LU。
- **对 Max 口味**：Max 判断靠等响 A/B，让它能听出「对比没了」，是在帮他判断。

## 2. 角度要求 8 项逐项评估

| 项 | 结论 | 去向 |
|---|---|---|
| 1 释放 / 攻击自适应 | 结构（快 30 ms / 慢 100～800 ms / 短过载松开 / 低频周期下限 / 跟拍）方向对，与 Pro-L 2「瞬态级 + 释放级」同构。缺口：短过载判定是二值且随前瞻漂移；慢段映射实际只用到 100～275 ms；跟拍信宿主 BPM；底鼓引起的衰减没有低频专门处理 | R2-06、R2-05、R2-03。dB 域释放：不建议（T-032 已记为设计选择，无增益证据） |
| 2 立体声联动 | 居中内容联动无影响；收益取决于不对称峰值多不多，先测 | R2-07。**M/S 限幅不建议**：真峰值约束在 L/R 上，M/S 限幅还得再接一级 L/R；只压 M 时 S 相对变大，每个底鼓都会瞬间变宽，违背 Max「母带不拉宽」 |
| 3 削波 / 限幅分工 | λ 是风格常数，不看内容；ADAA 见 R2-04。**SoftClip 拐点不建议改**：S2 在同等真峰值降幅下，硬拐点只响 0.03～0.09 dB；总残差小（−41.9 对 −38.1 dB），但 4 dB 降幅时高频残差略多（−30.5 对 −31.5 dB）；改了还要重拟合 GLAZE 常数，不值。另外 S2 显示：2 dB 降幅时下采样会让峰值重新长回 0.42 dB（软）/ 0.70 dB（硬），与 V1 驳回 R1-06 的结论一致（Clip 100 时限幅器必然兜底） | R2-06、R2-04 |
| 4 低频 | 低频周期下限覆盖不了底鼓；重新评估多段的依据和办法见 R2-03 | R2-03 |
| 5 真峰值与重建 | 检测误差 0.05～0.4 dB，比编码过冲（1.2～7.4 dB）小一个数量级，优先级应让给 R2-02。抛物线顶点修正（R1-04，存疑）只能修带限素材上 ≤ 0.07 dB 的低读，修不了近奈奎斯特那部分；8×/128 抽头要 +32 样本延迟。另想过一种「按近奈奎斯特能量自适应的余量」：带限素材上最多能拿回约 0.27 dB 响度，但所有限幅成品与 GLAZE 常数都要跟着变，本轮不提 | R2-02（含 SRC / 带限代理） |
| 6 前瞻与延迟 | 1 ms 前瞻会让 1 kHz 外的边带能量 +17 dB；窗形有取舍；R10-09 不碰 | R2-08 |
| 7 效率指标 | ΔL_eff、η = ΔL_eff ÷ GR、D_fast、P_slow、D_clip | R2-01 |
| 8 Listen / 预听 / 硬钳位 | 硬钳位在设计上不触发，缺测量；Listen 会抹平宏观对比；预听问题见范围外 | R2-09 |

**建议 Max 重新考虑**：无。没有提案触及已否决项（自动收单、ARA、两遍渲染、硬件建模、预设）；R2-03 针对的是规格原文就写了「再评估」的非目标。

## 3. 仿真与统计（scratchpad，玩具模型，只作量级与机理证据）

- **S1 攻击窗形**（`sim1_window.py`）：照 LimiterModule 的控制路（滑动最小 L + 63 → 瞬时下降、30 ms 线性域回升 → 多级滑动平均），统计单个 / 20 ms 过载的增益调制谱里偏离 200 / 1k / 3k / 8k Hz 以上的能量占比。单个瞬态，L = 96：box1 / box2 / box3 / 升余弦在 >1k 是 −50.6 / −60.2 / −63.3 / −65.6 dB，在 >3k 是 −64.4 / −81.9 / −94.2 / −99.4 dB；L = 48：>1k 是 −45.0 / −43.0 / −38.7 / −40.3，>3k 是 −58.3 / −68.9 / −78.7 / −80.9。占比与衰减深度无关（线性）。
- **S2 SoftClip 拐点**（`sim2_knee.py`）：8 s 合成 House（底鼓 + 低音 + 踩镲 + pad），理想 4× 上下采样，二分求出让 8× 真峰值降 2 / 4 dB 的阈值。降 2 dB 时：拐点 0.5 / 0.7 / 0.85 / 0.95 / 1.0 的阈值为 −1.93 / −2.42 / −2.62 / −2.69 / −2.70 dB，残差为 −34.5 / −38.1 / −40.7 / −41.7 / −41.9 dB，> 8 kHz 残差为 −39.9 / −41.6 / −43.3 / −44.0 / −44.1 dB，同真峰值下的近似响度为 +1.89 / +1.96 / +1.98 / +1.99 / +1.99 dB。降 4 dB 时残差都约 −25 dB，> 8 kHz 残差 −32.2 → −30.5 dB，响度 +3.53 → +3.73 dB。
- **S3 底鼓触发的高频泵**（`sim3_lf.py`）：只有底鼓过载 6 dB、踩镲恒定。宽带 30 / 80 ms 释放：踩镲泵深 5.36 / 5.30 dB，近似响度 −13.71 / −14.08；低频预限 + 宽带兜底：泵深 0.32 dB，近似响度 −14.27～−14.38，兜底级最大 GR 1.51 dB。该脚本「200 Hz～4 kHz 底鼓失真」一列混进了频谱比例的变化，不采用。
- **S4 残差 ADAA 错位**（`sim4_adaa.py`）：见 R2-04 的表；硬 / 软拐点 × 1 / 2 / 4× × 纯过采样 / 全量 ADAA / 残差 ADAA / 残差 ADAA + 干路平均（与全量逐点相同，验证了线性关系）。
- **S5 T-085 相关**（`corr.py`，读 `codec_roundtrips.csv` 与 `render_target_*.csv`，9 首真歌 × 3 目标）：r(过冲, GR 均值)：AAC 128k / 256k = 0.62 / 0.49，Opus 96k / 160k = 0.44 / 0.53，MP3 128k / 320k = 0.56 / 0.43。r(过冲, PSR)：−0.64 / −0.54、−0.66 / −0.72、−0.76 / −0.71。r(过冲, GR 最大值) ≈ 0。

## 待用户拍板

1. **R2-03**：要不要正式重新评估「多段限幅（v0.3 后再评估）」：先只做阶段 1 测量；阶段 2 原型放不放行（独立分支、不进 main）；若将来进产品，是只在 GLAZE > 0 时生效，还是新增参数。
2. **「流媒体安全」的完成标准**：是维持「PCM 真峰值 ≤ −2 dBTP」（现状），还是「编码后 ≤ −1 dBTP」（对哪些编码器 / 码率）？如果是后者，是否放开有效 Ceiling 的 −4.5 dBTP 下限和编码余量的 1.5 dB 上限（T-085 推导需要 −3.6～−6 dBTP）？（R2-02(c)）
3. 会改 GLAZE 0 下限幅输出的提案（R2-06(a)、R2-07 阶段 2、R2-08 换窗）：是接受「旧工程限幅时声音会变」（先例 0.1.7.2），还是只在 GLAZE > 0 时生效？
4. **R2-05**：跟拍能否用音频自测的速度覆盖宿主 BPM？扩到非 House 曲风由谁来定？
5. **R2-02(b)**：Apple AAC 只能在 Mac 上编。能否在 Mac 上只跑 `afconvert` 编 / 解码（不编译、不跑测试和渲染工具），还是另找 Mac 测试机？
6. **R2-04**：如果 T-088 第 3c 步立项，是否把「Clip 级输出真峰值泄漏 ≤ 现状 + 0.05 dB」写进完成标准？
7. **R2-09(b)**：Listen 加长窗档属于界面 / 监听口径的改动，由 Max 决定。

## 范围外发现（只报不改）

1. **Clip 活动占比的口径**：`clipActiveFraction` 只数 |x| > T（`LimiterModule.h:118-119`，文件头有写明）。但软拐点从 0.7T 就开始整形，S2 里被整形的样本比超过 T 的多 10～20 倍，残差主体在拐点区。所以 GlazeGuard 的 Clip 活动预算（`GlazeConstants.h:55`）管的不是「被非线性处理的量」。口径交 R5 / R6 评估，不算 bug。
2. **T-085 渲染 CSV 缺 Clip 量**：`render_target_*.csv` 有 `limit_gr_*`，没有 Clip 参数 / 深度，无法做 Clip 与编码过冲的归因（R2-02(a) 会补上）。
3. **归一化预听的偏差**：预听按 OUT 表的整体响度（`CeladonChain.cpp:508`）只衰减。只循环 drop 时，整体响度 ≈ drop 响度，比平台对整首测的值高，预听偏保守（比实际轻）；从头播到第一个 drop 时，整体响度还停留在前奏，预听偏响。实时没法预知整首，建议界面上提示「预听需完整播放一遍才准」。交 R5。
4. **短过载分界随前瞻漂移**：已写进 R2-06，这里只作记录。

## 证据索引

**项目内（文件:行号）**
- `Source/dsp/LimiterModule.h:20-29`（余量、低频释放、攻击整形、短过载说明）、`:114-121`（BlockStats）
- `Source/dsp/LimiterModule.cpp:41-56`（释放常数）、`:61`（0.35 dB）、`:111-112`（Ceiling −4.5 钳位）、`:389-391`（Clip 阈值映射）、`:885-924`（联动、需求、保持、包络、攻击、硬钳位）、`:943-1027`（低频周期释放）、`:1029-1038`（两级窗）、`:1040-1109`（释放包络、短过载、跟拍上限）
- `Source/dsp/Saturator.h:78-88, :101`（SoftClip，0.7 拐点）；`Source/dsp/TruePeak.h:10-12`（第 0 相 = 原样本）；`Source/dsp/TruePeak.cpp:112, :139-156`（Kaiser β 9，截止在奈奎斯特）
- `Source/dsp/ColorModule.h:10, :16`（Glue 侧链 100 Hz 高通、Heat 150 Hz 高通）
- `Source/dsp/GlazePlanner.cpp:795-826, :894`（λ 分工、Clip 阈值）；`Source/dsp/GlazeConstants.cpp`（warm / punchy / clean 的 shareClip / shareLimit）；`Source/dsp/GlazeConstants.h:55-56, :116, :314-326`
- `Source/dsp/CeladonChain.cpp:51-61, :266-275, :301-302, :508, :681-682`；`Source/dsp/CeladonChain.h:254`
- vault `01-框架/设计规格-Limit.md`（三节修订）、`01-框架/设计规格-总体.md:18`（多段非目标）、`01-框架/设计规格-曲风-House学习方式.md` 第 8 节、`03-记忆/ADR-004-…`、`03-记忆/踩坑-官网说明页的绝对说法要对着代码核对.md:13`
- `docs/validation/2026-09-29-T032-deep-research/R1-limit.md`、`V1-limit.md`（E1 / E2、R1-04～06 判决）；`docs/validation/2026-10-02-T047-realtime-research/REPORT-B.md` §6–7（前瞻、残差 ADAA）；`docs/validation/2026-10-08-T085-codec-tp/REPORT.md`、`RESULTS.md`、`codec_roundtrips.csv`、`render_target_-1{0,2,4}.csv`

**外部**
- 【一手】FabFilter Pro-L 2 Help · Advanced settings（风格、前瞻、瞬态 / 释放联动）：https://www.fabfilter.com/help/pro-l/using/advancedsettings
- 【一手】iZotope Ozone 9 Maximizer（IRC IV 按贡献最大的频段限幅、Stereo Independence）：https://downloads.izotope.com/docs/ozone9/en/maximizer/index.html ；Ozone 8：https://downloads.izotope.com/docs/ozone8/maximizer/index.html
- 【一手，产品页】A.O.M. Invisible Limiter（「Automatic control of attack and release time with look-ahead」；算法未公开）：https://www.kvraudio.com/product/invisible-limiter-by-a-o-m ；G2：https://www.kvraudio.com/product/invisible-limiter-g2-by-a-o-m
- 【二手，评测】Sound On Sound · Newfangled Audio Elevate：https://www.soundonsound.com/reviews/newfangled-audio-elevate
- 【一手】DMG Audio Limitless（6 段线性相位、双级、真峰值）：https://dmgaudio.com/limitless
- 【一手，产品页】Voxengo Elephant（只公开了线性相位过采样做真峰值）：https://www.voxengo.com/product/elephant/
- 【一手】Giannoulis, Massberg, Reiss, "Parameter Automation in a Dynamic Range Compressor", JAES 61(10):716–726, 2013：https://aes2.org/publications/elibrary-page/?id=16965 ；连续映射见 EP3588776B1（已撤销）：https://patents.google.com/patent/EP3588776B1
- 【一手，技术文章】Signalsmith Audio, Designing a straightforward limiter (2022)：https://signalsmith-audio.co.uk/writing/2022/limiter/
- 【一手】Sanfilippo, "Envelope following via cascaded exponential smoothers for low-distortion peak limiting and maximisation", IFC-22；实现：https://github.com/dariosanfilippo/LimiterClass
- 【一手】Parker, Zavalishin, Le Bivic, DAFx-16：https://dafx.de/paper-archive/details/vem_XXF5qBbfiWOH2RVVAA ；Bilbao, Esqueda, Parker, Välimäki, "Antiderivative Antialiasing for Memoryless Nonlinearities", IEEE SPL 24(7), 2017（卷期凭记忆）；Esqueda, Bilbao, Välimäki, "Aliasing Reduction in Clipped Signals", IEEE TSP 64(20), 2016：https://www.pure.ed.ac.uk/ws/files/26997332/07499828.pdf ；Vicanek：https://www.vicanek.de/articles/AADistortion.pdf ；CCRMA ADAA 笔记：https://ccrma.stanford.edu/~jatin/Notebooks/adaa.html
- 【一手】Apple Digital Masters（AURoundTripAAC、afclip）：https://www.apple.com/itunes/mastered-for-itunes/
- 【一手】Nielsen & Lund, "0dBFS+ Levels in Digital Mastering", AES 109, 2000：https://service-tcgroup.tcelectronic.com/media/Level_paper_AES109.pdf ；"Overload in Signal Conversion", AES 23rd Conf., 2003：https://cdn.mediavalet.com/aunsw/musictribe/2_NUqQYNUUWwJY6ZM2wn8w/RQNWR1o9v0S7uLbTbCNagg/Original/nielsen_lund_2003_overload.pdf
- 【一手】ITU-R BS.1770-5：https://www.itu.int/rec/R-REC-BS.1770-5-202311-I/en ；Spotify 响度说明：https://support.spotify.com/us/artists/article/loudness-normalization/
- 【二手】起伏度 / 粗糙度峰值与交界（Fastl & Zwicker 的摘要）：https://acoustics.salford.ac.uk/?p=716
- 【一手】GstPEAQ（DAFx-15）：https://dafx.de/paper-archive/details/cHfHWwcZJB8k3KwAtdoigw ；ViSQOL v3：https://arxiv.org/abs/2004.09584
- 【一手，凭记忆】Dixon, "Automatic Extraction of Tempo and Beat from Expressive Performances", JNMR 30(1), 2001

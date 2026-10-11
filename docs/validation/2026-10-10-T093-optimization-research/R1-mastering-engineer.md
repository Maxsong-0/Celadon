# R1 · 母带师视角：这个一旋钮母带插件该达到什么效果、差在哪

> **一句话结论**：Celadon 在频谱平衡、透明真峰值限幅、声像稳定、低切清理上已过职业母带链的及格线；对 Max 的 House 两版交付，短板集中在三处——**没有量「冲击与泵感」的尺子**（守护族里管微动态的 PSR 守门读的是入口，见范围外发现 1）、**低频与高频共用一条限幅包络**（底鼓每一下都在压踩镲和人声）、**交付端只管 PCM 峰值，不管编码后峰值和翻译监听**。建议先补尺子和交付检查（S 级），再拿尺子决定要不要动限幅结构与 M/S 均衡（M～L 级）。
>
> **前 5 条**：R1-01 母带指标集（冲击 / 泵感 / 失真 / 声像）进渲染工具与门槛（S～M · P0）｜R1-02 交付检查单 + 俱乐部版编码余量（S · P1）｜R1-03 翻译监听：单声道 / 侧声道 / 小音箱 + 低频相关度常显（S · P1）｜R1-04 出口 LRA / PSR / 落差读数（S · P1）｜R1-05 平台归一化后的 A/B（S～M · P2）。
>
> 代号 R1 · 2026-10-10 · 只调研、不改码、不改默认值 · 行号以本工作树（main `3a7a367`）为准 · 标注：【一手】厂商文档 / 标准 / 论文 / 源码，【推断】本代理推理，【统计】二次统计 · Mac 上没有编译、没有跑测试，所有「怎么验」交主会话在 DGX / 107 / 109 跑。

## 0. 读法

- 第 1 节回答 Max 的原话「如果你是一个混音师你想要这个插件达到一个什么样的效果」：15 条可感知、可测的目标，每条给测量代理与现状。
- 第 2 节把一条职业母带链逐级对照 Celadon；第 3 节列 T-086 之外职业用户还要的工作流能力；第 4 节排序；第 5 节是前 10 条提案（每条七个固定字段 + 链位置 + 验收）。
- 我只写「要什么、差多少、怎么验」；限幅结构、Balance 算法、Heat、控制层位置、工序位置分别交 R2 / R3 / R4 / R5 / R6，第 7 节一句话交接。
- 不重提：自动收单（ADR-015）、ARA、两遍渲染、硬件建模、预设；T-032 待拍板项、T-083～T-088、T-047 延迟三步、T-086 可解释性与 A/B、T-087 冻结、T-088 延迟。可以依赖它们的地方写明「依赖」。

## 1. 如果我是母带师，我要它做到……

判定：「有且够」= 现在的机制足以达到；「有但弱」= 有机制但没量或结构上管不住；「没有」= 没人管。阈值都是**起点**，要用第 5 节 R1-01 的数据和 Max 试听标定。

| # | 我要它做到 | 测量代理（指标 + 口径） | 现状（谁在管 · 证据） | 判定 |
|---|---|---|---|---|
| G1 | 俱乐部 PA / 手机 / 耳机 / 单声道都翻译 | ① 出口 27 段相对谱（主段）对曲风目标斜率 −2.25 dB/oct 的残差 RMS ≤ 1 dB；② 单声道低频损失 10·log10((1+ρ)/2)，ρ 为 40～125 Hz 相关度，主段 ≥ −1 dB；③ 手机模拟（M 过 4 阶 200 Hz 高通）主段响度 − 全频主段响度，出口相对入口 ≥ −0.5 LU | ① Balance + Tone 参考斜率（设计规格-曲风与交付 §3.1）；② 只提示不改（ADR-015；`ModuleCards.h:11-12`）；③ 没人管 | 有但弱 |
| G2 | 底鼓与 bass 分得开、sub 有定义 | 低频峰值因数 LCF：M 过 4 阶 120 Hz 低通，3 s 窗 20·log10(峰 / RMS)，主段中位；出口 − 入口 ≥ −1.5 dB | Balance 低 5 段（40～100 Hz）的 Tame 只管频谱突出、不管动态；Glue 侧链 100 Hz 高通（`ColorModule.h:10`）；Clip 与限幅都是宽带 | 有但弱 |
| G3 | drop 的冲击不被磨平 | ① 出口 PSR₃ₛ 主段 P10 ≥ 交付下限 8 / 7 / 6 dB（标准 / 流媒体 / 俱乐部，`PsrGuard.cpp:7-16` 的数）；② 起音峰值因数损失（起音处 10 ms 峰值 − 其后 100 ms RMS，出口 − 入口中位）≥ −2 dB；③ breakdown → drop 落差损失 ≤ 1.2 / 1.5 LU | ③ 落差守门（实现与规格不符，范围外发现 2）；① PSR 守门读的是入口（范围外发现 1）；② 没量 | 有但弱 |
| G4 | 2～5 kHz 推响后不刺 | 推响不刺同口径的超出量（本段 60 ms 快电平 − 邻段均值 − 1.5 dB），出口主段 P95 ≤ 1 dB 且不高于入口；侧声道同口径另算 | 推响不刺 + 自动 Tame（`BalanceModule.h:13-16`；设计规格-高级母带控制 §4）；两声道同一组增益（`BalanceModule.h:23`），侧声道单独刺时只能连中间一起压 | 有但弱 |
| G5 | 200～500 Hz 不糊 | 200～500 Hz 5 段相对曲风目标的正偏差，主段 P75 ≤ +1.5 dB | Tame 局部峰 + 目标频谱（高级母带控制 §5）；同样没有 M/S | 有且够 |
| G6 | 声像稳定、M/S 平衡不随段落漂 | 逐段 S/M（2～16 kHz、40～125 Hz）出口 − 入口的绝对值 ≤ 0.5 dB，段间标准差 ≤ 0.3 dB | Balance / Glue / 限幅全部左右联动（`BalanceModule.h:23`、`ColorModule.h:10`、`LimiterModule.cpp:885-887`）；House 自动加宽 0（曲风与交付 §3.1）；Heat 侧声道残差 ×0.75（`ColorModule.h:22-23`） | 有且够（缺验证读数） |
| G7 | 同一张 EP 各曲音色、响度一致 | 各曲主段 LUFS 极差 ≤ 1 LU；出口谱斜率极差 ≤ 0.3 dB/oct；主段 PSR 中位极差 ≤ 1.5 dB | 同曲风、同交付的目标天然趋同（交付对准响段，曲风与交付 §4.2）；没有跨曲检查 | 有但弱 |
| G8 | 达到交付响度时没有泵 | 高频调制深度 HMD：2～8 kHz 10 ms 包络的出口 / 入口比（dB，延迟对齐），减 1 s 滑动中位去趋势后主段 P95 − P5；有拍点时另报拍频分量 | 低频周期给快释放下限，但孤立衰减的鼓击不触发（`LimiterModule.h:24-25`）；House 跟拍「一拍 ÷ 3」（`CeladonChain.h:207-208`）；限幅 GR 预算 P95 4～5.5 dB、峰 6～9 dB（`GlazeConstants.cpp:78-126`）；单一增益包络（`LimiterModule.cpp:885-897`） | 有但弱 |
| G9 | 没有可闻失真与互调 | ① 限幅差值能量 / 出口能量（K 加权，主段，dB）；② SMPTE 互调（60 Hz + 7 kHz，4:1）过整链在交付工作点的边带和 | 试听差值能听不能量（`CeladonChain.h:32-36`）；Heat 支路 150 Hz 高通减低频互调（`ColorModule.h:16`；`docs/colour-and-render-revision.md:7-9`） | 有但弱 |
| G10 | 动态（PSR / LRA）落在曲风区间 | 出口主段 PSR 中位：House 流媒体 ≥ 8、俱乐部 ≥ 6.5（T-047 REPORT-C 的区间）；出口 LRA ≥ 入口 LRA − 1 LU（EBU Tech 3342） | 决策器 LRA 修正（曲风与交付 §4.1）；预算环管 GR；界面没有出口 LRA / PSR | 有但弱 |
| G11 | 真峰值与编码后都安全 | PCM 8× TP ≤ Ceiling + 0.01 dB；MP3 320 解码后 ≤ −0.1 dBFS（俱乐部）；AAC 256 解码后 ≤ −1 dBTP（流媒体） | PCM 有且够（ADR-004 + 0.35 dB 重建余量，`LimiterModule.h:19-21`）；编码后没人管（高级母带控制 §6 第 80 行；T-085 最坏过冲 MP3 320 +1.15、AAC 256 PNS 关 +2.40 dB） | 有但弱 |
| G12 | 等响 A/B 下只变好、不变坏 | Listen 等响下的盲听偏好（`tools/render/README.md:556-583` 的 ABX 流程）；R1-01 指标全部「不变坏」 | Listen 只衰减等响（`CeladonChain.h:10-16`）、听原声（`CeladonChain.h:112-115`）；T-086 加快照 | 有且够（缺指标） |
| G13 | 两版交付音色相同、只差响度与峰值 | 两版出口 27 段相对谱差（主段）≤ 0.5 dB | 同一份画像，差在 Gain / Glue / Heat / Clip（曲风与交付 §4.3）；打印可复现依赖 T-087（阻塞） | 有且够 |
| G14 | 低切干净、不伤底鼓 | 次声 < 16 Hz 占比 ≤ −30 dB；40～60 Hz 能量变化 ≥ −0.2 dB | Clay 低切 House 25 Hz + 峰值守护 + 混合损失 + 压力反馈（ADR-014 / 027 / 028；`ClayModule.h:10-20`） | 有且够 |
| G15 | 平台归一化后不吃亏 | −14 LUFS 归一化后的回放 TP、PSR（T-085 模型） | 归一化预听（高级母带控制 §6），但不能和原声在同一电平比（三种临时监听互斥，同规格 §1 第 17 行） | 有但弱 |

小结：15 条里 5 条「有且够」，10 条「有但弱」，没有一条完全空白——Celadon 的结构是对的，缺的主要是**尺子**（G2、G3、G8、G9、G10、G11 都没有出口侧的测量）和**两处结构**（限幅单包络、Balance 不分 M/S）。

## 2. 职业母带链 vs Celadon 逐级对照

| 级 | 职业链的典型做法（出处） | Celadon（证据） | 判定 |
|---|---|---|---|
| 1 增益分级 / 输入体检 | 浮点内部、看输入峰值 / 响度 / 相位 | IN 真峰值表；画像含 I、LRA、PLR、低频相关度、次声占比（`GlazeTypes.h:110-145`） | 有且够（没有 DC 读数，影响小） |
| 2 修正 EQ（静态 + 动态 + 共振 + M/S） | Pro-Q / Pro-MB 逐段 M/S；soothe 有 L/R 与 M/S 两种模式（soothe 手册）；Gullfoss 立体声处理兼容 M/S 通道（Gullfoss 手册） | Balance 27 段 1/3 倍频程动态修正 + 推响不刺 + 目标频谱，两声道同一组增益（`BalanceModule.h:8-24`） | 有但弱：无 M/S（R1-06）；1/3 倍频程抓不到窄共振（交 R3） |
| 3 低频管理 | 低切、低频收窄、低频动态（Ozone Low End Focus：20～300 Hz、Punchy / Smooth、Stereo 与 M/S 模式）、低频峰值因数表（iZotope TBC） | 低切 有且够（`ClayModule.h:10`）；收单只手动 + 反相提示（ADR-015，按 Max 决定不算缺口）；低频动态 / 峰值因数没有 | 有但弱（R1-01、R1-08） |
| 4 压缩 / 密度 | 宽带总线压缩，必要时多段 | Glue 宽带 2:1、软拐 6 dB、攻击 20 ms，低 / 高分频施压差 ≤ 3.2 dB（`ColorModule.h:10-15`） | 有且够（细节交 R4） |
| 5 饱和 / 色彩 | 并联谐波、按频段 | Heat 8× 并联残差、M/S、150 Hz 以下不生成谐波（`ColorModule.h:16-24`） | 有且够（低频不生成谐波是有意取舍，见 R1-10） |
| 6 声像 | 宽度、低频收窄、相关度检查 | Clay 侧声道 2 kHz 高架（House 自动 0）、手动收单、反相提示（`ClayModule.h:15`） | 有且够（按 Max 口味）；缺相关度读数（R1-03） |
| 7 削波 + 限幅 | 削波器 + 真峰值限幅；多段包络（Ozone 12 IRC 5 四段；A.O.M. G3 软限幅用于链中多级限幅） | Clip 4× 宽带软削波 + 8× 真峰值、左右联动、单一增益包络（`LimiterModule.h:9-12`、`LimiterModule.cpp:885-897`） | 有但弱：低频和高频共用一条包络（R1-07） |
| 8 计量 | I / S / M、LRA、TP、PLR / PSR、相关度（Pro-L 2、smart:limit、Metric AB） | I、S、TP 最大、PLR、流媒体归一量、频谱、两条 GR（`SpectrumView.h:84-101`、`LevelMeters.h:5-9`） | 有但弱：缺 LRA、PSR、相关度（R1-03、R1-04） |
| 9 编码预览 / 编码后测峰 | Apple 的 afclip 与 AURoundTripAAC（Apple Digital Masters 技术简报） | 插件内没有；离线 `tools/render/codec_tp_check.py`（T-085） | 没有（R1-02；不建议插件内复刻编码器） |
| 10 抖动 / 位深 / SRC | 最后一步抖动，SRC 在抖动之前 | TPDF 16 / 24 bit、无噪声整形（高级母带控制 §6 第 81 行） | 有且够；SRC 顺序没有提示（R1-02） |
| 11 交付 QC | 每版一份检查单 | 没有 | 没有（R1-02） |
| 12 参考对比 | 等响切到参考曲 | 侧链学 27 段相对频谱（`ReferenceSpectrum.h:24-35`），不能听参考 | 有但弱（R1-09） |

## 3. 一旋钮全自动工具里，职业用户仍然想要的工作流

T-086 已认领：GLAZE 时间线、各模块预算条、A/B 快照、等响改为不进工程的监听状态（TASKS T-086 行）。下表只列它没覆盖的。

| 能力 | 职业用户为什么要 | Celadon 现状 | 本报告 |
|---|---|---|---|
| 单声道 / 侧声道 / 小音箱监听 + 相关度 | 俱乐部系统常有单声道段、手机外放基本听不到 sub；ADR-015 说母带改不了单声道回放，只能回混音修，那就得在母带阶段听到 | 没有（只在反相提示触发时显示损失） | R1-03 |
| 出口 LRA / PSR / 落差数字 | 判断「压扁没有」的行业通用读数 | 没有 | R1-04 |
| 平台归一化后的 A/B | 判断推响在 −14 归一化后是否反而吃亏（第 ③ 项要的听感证据） | 预听与原声互斥 | R1-05 |
| 等响听参考曲 | 去掉「响的那边占便宜」的偏见 | 参考只学频谱 | R1-09 |
| 交付检查 / 导出安全提示 | 编码后削波、SRC 顺序、忘了退出临时监听就导出 | 没有 | R1-02 |
| 分段响度（前奏 / drop / breakdown） | 看 drop 被推到哪、落差剩多少 | 渲染工具有逐段 CSV（`tools/render/README.md:286-315`）；插件里没有数字 | 并入 R1-04（曲线归 T-086 时间线） |
| 撤销 | 改错能回去 | 参数撤销由宿主负责；GLAZE 自动态的回退由 T-086 快照覆盖 | 不另提 |
| 「它做了什么」 | 信任 | T-086 | 不另提 |
| 打印可复现 | 两版交付可重导 | T-087（阻塞） | 依赖，不另提 |

## 4. 排序依据（对 Max House 两版交付的听感收益 × 代价）

| 编号 | 链位置 | 听感收益 | 代价 | 优先级 | 依赖 |
|---|---|---|---|---|---|
| R1-01 | 工具 | 间接但根本：没有它，G2 / G3 / G8 / G9 全凭耳朵猜 | S～M | P0 | — |
| R1-02 | 末端 + 工具 | 防止俱乐部版 MP3 解码后削波、SRC 顺序错 | S | P1 | — |
| R1-03 | 末端监听 | 能听到单声道 / 手机上的问题 | S | P1 | — |
| R1-04 | 末端计量 | 一眼看出压扁没有 | S | P1 | — |
| R1-05 | 末端监听 | 第 ③ 项（流媒体 −10 去留）的听感证据 | S～M | P2 | — |
| R1-06 | Balance | 宽声场踩镲 / 合成器不刺，同时不把人声压暗 | M～L | P2 | R1-01 |
| R1-07 | Limit | 推响不泵，收益最大但最不确定 | L | P2 | R1-01 先量 |
| R1-08 | 控制层 | drop 底鼓不被磨平 | M | P2 | R1-01 |
| R1-09 | 末端监听 | 参考对比去偏见 | M | P2 | — |
| R1-10 | Color | 流媒体版手机上听得到底鼓 | M～L | P3 | R1-01 |

## 5. 前 10 条提案

### R1-01 母带指标集：冲击 / 泵感 / 失真 / 声像进渲染工具与门槛

链位置：工具（`tools/render`）· 类型：验证方法 · 代价 S～M · P0

- **问题 / 现状**：渲染工具的指标表只有 LUFS、真峰值、PLR、限幅 GR 均值 / 最大、低频相关度、次声占比（`tools/render/README.md:131-146`），逐段表只有段响度、低频占比、GR（同文件 `:308-315`）；BRIEF §5 写明「好听没有客观验收」。GR 预算是泵感的代理，但 GR 不等于可闻泵感：同样 4 dB GR，高频跟不跟着底鼓起伏取决于高频是不是骑在同一条包络上。PSR 守门名义上管微动态，实际读入口（范围外发现 1），所以「冲击有没有被磨平」现在没有任何数字。
- **提案**：只改工具（`RenderMain.cpp` 多写几列 + 一个 Python 后处理），每首 × 每组加下列量；主段 = `--segments-csv` 段响度 ≥ 本曲段响度 P95 − 3 LU 的段：
  1. `out_psr_p10_db` / `out_psr_med_db`：每 100 ms 取尾随 3 s 窗的 TP8 − S（同 T-085 口径），主段 P10 / 中位；入口同算，另列差值；
  2. `out_lra_lu`：EBU Tech 3342（S、相对门 −20 LU、P10～P95）；
  3. `low_crest_delta_db`：G2 的 LCF，出口 − 入口；
  4. `hf_mod_depth_db`：G8 的 HMD；有 `--bpm` 时另报拍频处的幅度；
  5. `onset_crest_loss_db`：入口用 27 段谱通量检出起音，G3② 的量；
  6. `limiter_diff_ratio_db`：G9① 的量；差值用 `auditionDifference` 路径在同决策重放上再跑一遍得到（ADR-016 机制已在）；
  7. `ms_delta_hi_db` / `ms_delta_lo_db`：G6 的逐段 S/M 差的中位与标准差；
  8. `phone_rel_delta_lu`：G1③ 的量。
  另加一个合成阳性对照：SMPTE 60 Hz + 7 kHz（4:1）过整链，报边带和（G9②）。`gate_check.py` 先只打印、不判定，等第二步标定后再按 Max 认可的阈值判。
- **依据**：sonible smart:limit 用 3 s PSR 的中位数作动态量，理由是它比 PLR 更贴近短时动态（sonible 帮助中心 4413934150546）【一手】；PSR 的提出见 Ian Shepherd 在 AES 143 的报告（同页引述）【一手转引】；iZotope TBC 帮助：低频 Crest Factor 表偏右表示低频压过头、偏左表示过于动态【一手】，本提案的 120 Hz / 3 s 口径是【推断】（TBC 未公开算法）；iZotope 的 Lukin 说明 IRC 1～4 单包络时重低音会连带压高频（Inside Ozone 12）【一手】；EBU Tech 3342 的 LRA 定义【一手】；差值监听当失真检查（Pro-L 2 Audition Limiting、soothe delta）【一手】。
- **对硬约束与 ADR**：不碰插件、不碰默认值；工具只多读，插件输出逐位不变；差值走同决策重放的第二遍。无 ADR 冲突。
- **代价与风险**：S～M（C++ 指标约 300 行、Python 约 100 行；离线耗时每首多约 30 %）。风险：这些量没有经过感知标定，阈值拍脑袋会把好成品判坏——所以先报告后判定。
- **怎么验**：① 阳性对照（DGX，`make_synthetic_house.py` 的合成曲）：手动 Release 10 ms 对 300 ms → `hf_mod_depth_db` 应随释放变短变大；Clip 0 对 100 → `onset_crest_loss_db` 应变化；GLAZE 0 → 除 `limiter_diff_ratio_db` 外各差值 ≈ 0。② 22 首门槛曲 + 45 首 Jamendo × GLAZE {0, 35, 70, 100} × 交付 {流媒体, 俱乐部} × 曲风 {Deep, Tech, Melodic} 出分布。③ Max 等响盲听：按 `hf_mod_depth_db`、`low_crest_delta_db` 各挑最高 / 最低 5 对（README 盲听流程），指标排序与耳朵排序的 Spearman ≥ 0.6 才把该量升格为门槛。
- **对 Max 口味**：不改声音；给「全自动提高音量、但不泵、不磨冲击」一把尺子，后面 R2 / R4 的算法提案都能拿它说话。

### R1-02 交付检查单 + 俱乐部版编码余量

链位置：末端（有效 Ceiling）+ 工具 · 类型：工作流能力 / 末端 · 代价 S · P1

- **问题 / 现状**：俱乐部档有效 Ceiling = 用户 Ceiling − 编码余量，默认 −1.0 − 0（设计规格-曲风与交付 §4.1 第 64 行；`GlazeConstants.h:178-180` 没有俱乐部峰值上限）；插件不测编码后峰值（高级母带控制 §6 第 80 行）。T-085（9 首、源为 MP3、ffmpeg 编码器）：MP3 320 最坏过冲 +1.15 dB，AAC 256（PNS 关）+2.40 dB（`docs/validation/2026-10-08-T085-codec-tp/REPORT.md` 编码表与 AAC 补测表）。Beatport 收 WAV / AIFF 16 / 24 bit 44.1 kHz，也收 MP3 320（LabelGrid 分销商说明）——俱乐部版会有一部分以 MP3 320 进 DJ 的 U 盘，按 T-085 最坏值，−1.0 dBTP 的成品解码后可到约 +0.15 dBFS。另外：工程 48 kHz、交付 44.1 kHz 时宿主在 Celadon 之后做 SRC，Celadon 的 16 bit 抖动加在 SRC 之前，SRC 后的再量化不再被抖动覆盖【推断】。
- **提案**：
  - (a) 工具 `tools/render/delivery_check.py`（复用 `codec_tp_check.py` 的计量与编码代码）：输入 Max 导出的两版成品（多首 = 一张 EP），每首一行：整曲 I、主段 S、LRA、出口 PSR P10、8× TP、MP3 320 / AAC 256 / Opus 160 解码后 TP、采样率与位深、DC、首尾静音、单声道低频损失、**疑似带着临时监听导出**（全曲 S/M < −60 dB = 单声道监听；整曲 I 比交付目标低 3 LU 以上且 TP 远低于 Ceiling = 预听衰减）；EP 汇总行给主段 LUFS、谱斜率、PSR 中位的极差（G7）。
  - (b) 默认值（交 Max）：俱乐部档有效 Ceiling 取 min(用户 Ceiling − 编码余量, −1.5 dBTP)，与流媒体 −2 dBTP 走同一条路径；或保持 −1.0、只靠 (a) 提示。
  - (c) 用法说明：交付采样率不等于工程采样率时，Celadon 的抖动关、由宿主在导出最后一步抖动。
- **依据**：Apple Digital Masters 技术简报：提供 afconvert / afclip / AURoundTripAAC，明确提醒 PCM 上没超的电平编码后仍可能削波、建议至少留 1 dB 余量，并提到俱乐部母带要考虑舞池的噪声底【一手】；T-085 实测【一手-本仓库】；Nielsen & Lund, *0dBFS+ Levels in Digital Mastering*, AES 109th Convention, Paper 5251（2000）【一手-论文】；Spotify 响度说明（母带响于 −14 LUFS 时 TP 低于 −2）【一手】；LabelGrid 的 Beatport 格式要求【一手-分销商】。
- **对硬约束与 ADR**：(a)(c) 不碰插件。(b) 改出厂值：按「流媒体 −2 dBTP 写在链里、读 factoryConstants、改了旧工程跟着变」的先例（曲风与交付 §3.2 第 48 行），旧工程的俱乐部档会变轻 ≤ 0.5 dB，与「旧工程读入不改声音」冲突，要 Max 选；交付上限在 GLAZE 0 时是否生效要 R2 / J2 核（决定 GLAZE 0 逐位是否受影响）。延迟、CPU 无影响。
- **代价与风险**：(a) S；(b) S 代码 + 一次 22 首重放。风险：−1.5 让俱乐部版限幅多做 0.5 dB，GR 预算更紧、预算环更早介入。
- **怎么验**：Max 的 22 首无损门槛曲（45 首 Jamendo 是 MP3 源，只作参考）× 俱乐部 Ceiling {−1.0, −1.5} 渲染 → `delivery_check.py`：MP3 320 解码后 > 0 dBFS 与 > −0.1 dBFS 的首数、最大值；R1-01 的 `hf_mod_depth_db`、`out_psr_p10_db` 看 −1.5 的代价；Max 等响 A/B 俱乐部 −1.0 对 −1.5（只差 0.5 dB 多压）3 首。AAC 用哪个编码器作准见「待用户拍板」2。
- **对 Max 口味**：不改音色；两版交付各有一份能拿给厂牌 / 分销的检查单。

### R1-03 翻译监听：单声道 / 侧声道 / 小音箱 + 低频相关度常显

链位置：末端监听（OUT 表、GLAZE 测点之后，与试听差值同位）· 类型：工作流能力 · 代价 S · P1

- **问题 / 现状**：只有三种临时监听（原声、试听差值、归一化预听；高级母带控制 §1 第 17 行），源码里没有单声道 / 侧声道监听；单声道低频损失只在 House 反相提示触发时显示（`ModuleCards.h:11-12`，`GlazeController.cpp:897`）。ADR-015 的原理是单声道回放听的是 M、母带改不了它，只能回混音修——那 Max 得在母带阶段**听到**它。
- **提案**：加三个临时监听：单声道 (L+R)/2 送两边；侧声道 (L−R)/2；小音箱 = 单声道 → 4 阶 Butterworth 200 Hz 高通 → 2 阶 8 kHz 低通（固定近似，不模拟具体设备【推断】）。CLAY 卡片常显「低频相关度 ρ / 单声道低频损失 x.x dB」，复用 `liveLowCorrelation` 与提示用的同一公式（ADR-015）。
- **依据**：ADPTR Metric AB 提供 Mono / Sides / L / R 监听与按频段的相关度显示（Plugin Alliance 产品页、MusicTech 评测）【一手 + 评测】；Audified MixChecker 用多台同类设备的平均响应模拟消费设备（SOS 新闻）【一手-评测】；TBC 帮助的频段独奏【一手】；ADR-015【一手-本库】。
- **对硬约束与 ADR**：只改送宿主的监听，表与 GLAZE 照常按母带算（同现有三种）；不存工程、关编辑器退出、离线渲染强制关（同预听）；默认关时逐位不变；0 延迟、CPU 可忽略。需要把规格「三种互斥」改成「N 种互斥」。
- **代价与风险**：S。风险：实时导出时忘了退出——R1-02 的「疑似带着临时监听导出」兜底，主窗口照现有做法常显退出提示。
- **怎么验**：DGX 单元测试：关着时与改前逐位相同；单声道模式输出 = (L+R)/2 逐位；离线强制关；UI 快照（中英）。Max：22 首里 `slow_inverted_pct` > 0 的几首用单声道听，核对琥珀提示与耳朵是否一致。
- **对 Max 口味**：不改声音、不重开自动收单（ADR-015），只给「请回混音修」一个能听的依据。

### R1-04 出口读数补全：LRA、出口 PSR、段落落差

链位置：末端计量 · 类型：工作流能力 · 代价 S · P1

- **问题 / 现状**：响度小窗有整体、短期、真峰值最大、峰响比、流媒体归一量（`SpectrumView.h:84-101`；设计规格-总体第 124 行），没有 LRA、PSR、段落落差；落差守门内部已在算输入 / 输出的 (P90 − P25)（`ContrastGuard.cpp:63-82`），不显示。母带师判断密度看 PSR 和 LRA：smart:limit 以 PSR 为动态量并显示 LRA，Pro-L 2 也显示 LRA。
- **提案**：小窗加三项（悬停展开，不挤主视图）：出口 PSR₃ₛ（按交付下限 8 / 7 / 6 着色）与复位以来的主段 P10；出口 LRA（Tech 3342，前 60 s 标「未稳定」，同 T-038 引的 Tech 3341 规定）；落差「原 x.x → 现 y.y LU」。全部用 OUT 表已有的 100 ms 子块（带真峰值）算。
- **依据**：EBU Tech 3342；sonible 帮助（PSR 中位）；FabFilter Pro-L 2 帮助（LRA、目标电平）；T-047 REPORT-C 的 PSR 区间【一手 + 本库】。
- **对硬约束与 ADR**：只读表，不改音频；音频线程只多几次加法（LRA 直方图可以放消息线程）；和 T-086 不重叠（它画曲线，这里给数字）。
- **代价与风险**：S。风险：一旋钮界面数字太多——放悬停层。
- **怎么验**：DGX 单元测试：同一文件对照 pyloudnorm 的 LRA 与 T-085 的 PSR 脚本，误差 ≤ 0.1；UI 快照；Max 看 3 首 drop 时的 PSR 和「听着压扁」是否对得上。
- **对 Max 口味**：只给信息。

### R1-05 平台归一化后的 A/B

链位置：末端监听 · 类型：工作流能力 · 代价 S～M · P2

- **问题 / 现状**：三种临时监听互斥（高级母带控制 §1 第 17 行），开着归一化预听就不能和原声比；Listen 把处理结果拉到原声的响度（`CeladonChain.h:112-115`），不是平台的回放电平。于是「推到 −7 / −10 之后，到了 Spotify −14 是不是反而更弱」听不到。Apple 简报的 Sound Check 一段就提醒，响母带被拉低后可能听起来更弱；T-085 模型里流媒体 −10 档在 −14 归一化下中位被拉低 3.3 dB。这正是第 ③ 项（流媒体 −10 LUFS 去留，BRIEF §4 T-089 注）要的听感证据。
- **提案**：预听开着时允许切原声：A = 母带 × 10^(min(0, 目标 − OUT 整体)/20)，B = 原声 × 10^(min(0, 目标 − IN 整体)/20)，两边各自只衰减到同一目标；IN 整体响度还没读数时 B 退回 Listen 等响并提示。
- **依据**：Apple Digital Masters（Sound Check 段）、Spotify 响度说明、Pro-L 2 Unity Gain（等响比较的理由）、T-085 归一化模型【一手】。
- **对硬约束与 ADR**：只改监听；规格 §1「互斥」改成「原声 + 预听可叠加」；离线强制关同预听；默认关时逐位不变。
- **代价与风险**：S～M。风险：听原声期间 OUT 整体响度暂停积分的口径（设计规格-总体第 134 行）要和 IN 表一起核。
- **怎么验**：单元测试：两路已知 LUFS 的正弦，A / B 都落在目标 ±0.1 LU；Max 用 3 首 × 流媒体档，在 −14 下 A/B。
- **对 Max 口味**：直接服务两版交付的取舍。

### R1-06 Balance 的侧声道「只许多压」

链位置：Balance · 类型：新功能（算法交 R3）· 代价 M～L · P2

- **问题 / 现状**：Balance 两声道同一组增益（`BalanceModule.h:23`）；逐段 S/M 与相关度已经测出来了（`BalanceModule.h:134-139` 的 bandCross；`GlazeTypes.h:131-139`），却只给 Clay 加宽和反相提示用。House 常见宽声场的踩镲、shaker、混响尾和立体声合成器让侧声道 3～8 kHz 刺或 200～500 Hz 糊，而中间（人声、底鼓 click）不刺——联动 EQ 只能连中间一起压（变暗）或不压。
- **提案**：Tame 与推响不刺在侧声道上可以比中间多压：G_S[b] = G_M[b] + min(0, x[b])，x[b] 按侧声道相对自身参考轮廓的超出量算（与中间同口径）；硬约束 G_S ≤ G_M（侧声道永远不比中间多提 = 永远不加宽）；Recover、Brighten、目标频谱仍联动。只有某段 x ≠ 0 时走 M/S 滤波（M、S 各 27 个峰值滤波器，总数与现在 L、R 各 27 个相同），全部 x = 0 时走现在的 L/R 路径；路径切换的交叉淡化、同频不同增益的峰值滤波器之间的相位差交 R3。
- **依据**：soothe 手册：复杂立体声素材推荐 M/S 模式、链接 100 % 起步，M/S 下降低链接通常让声像变宽【一手】——所以必须加「只许多压」的约束；Ozone Low End Focus 与 FabFilter Pro-Q / Pro-MB 都有逐段 M/S【一手】；Gullfoss 立体声处理兼容 M/S 通道（Gullfoss 手册）【一手】；House 侧声道刺耳有多普遍没有公开统计【推断，先用 45 首 Jamendo 的 bandSideToMidDb 分布核实】。
- **对硬约束与 ADR**：自动份额为 0（含 GLAZE 0）时 x = 0 → 逐位不变；旧工程读入补「关」（同 ADR-020 推响不刺的做法）；0 延迟；CPU 只多 M/S 编解码每样本 4 次加乘、每 hop 27 段一次比较，滤波器数量不变；分析按 hop，块长逐位不受影响；仍在 Balance 内，ADR-001 不变。
- **代价与风险**：M～L。风险：M、S 增益不同带来左右相位差，可能听着「发虚」；侧声道压多了声像变窄（Max 能接受收窄，但要听）。
- **怎么验**：先用 45 首 Jamendo 统计 2～8 kHz S/M 高于 P75 的曲目；22 首 + 这些曲目 GLAZE 35 / 70，看 R1-01 的 `ms_delta_hi_db`（要求 ≤ 0）与侧声道超出量；门槛：任一段 S/M 增量 ≤ +0.1 dB；Max 等响盲听 5～10 首（只开关此项）。
- **对 Max 口味**：「母带不拉宽」——只收窄刺耳的侧声道。

### R1-07 低频不拖累高频：拿 HMD 决定要不要上分频限幅

链位置：Limit（结构与位置交 R2 / R6）· 类型：优化算法 · 代价 L · P2

- **问题 / 现状**：限幅器左右联动、单一增益包络（`LimiterModule.cpp:885-897`），Clip 宽带（`LimiterModule.h:9-12`）。House 的峰值几乎都来自底鼓 + sub，ADR-029 放宽后限幅 GR 预算 P95 4～5.5 dB、峰值 6～9 dB（`GlazeConstants.cpp:78-126`），也就是每一下底鼓都把踩镲、人声一起压下去几个 dB。现有缓解只针对持续低音（低频周期下限，孤立衰减的鼓击不触发，`LimiterModule.h:24-25`）和释放时长（House 一拍 ÷ 3）。ADR-001 当年把多段限幅定为「暂不做，v0.3 试听后再评估」，设计规格-总体的「非目标」也写的是试听后再评估。
- **提案**（我给目标与验收，结构交 R2）：先用 R1-01 的 `hf_mod_depth_db` 在俱乐部 −7 档量 22 + 45 首；若超过 Max 试听定的阈值的曲目占比显著，再评估「低频（约 100～150 Hz 以下）单独削波或单独包络、高频沿用现在的包络、最后宽带真峰值兜底」。验收目标：同交付响度、同 TP，`hf_mod_depth_db` 降 ≥ 1.5 dB，`low_crest_delta_db` 不更差，`limiter_diff_ratio_db` 不升。
- **依据**：iZotope Lukin 解释 IRC 5 为什么做成多段：旧算法里 "heavy bass notes inevitably affected higher-frequency sounds"，IRC 5 在四个频段用不同增益包络减泵（Inside Ozone 12）【一手】；Ozone 8 手册：IRC IV 只限制对峰值贡献最大的频段以减少互调【一手】；A.O.M. Invisible Limiter G3 的 Soft Limiter 可放在链中做多级限幅（厂商新闻稿）【一手】；`LimiterModule.h:24` 自己已写明要「减少低音调制高频」【一手-本库】。
- **对硬约束与 ADR**：新增分频：线性相位分频加延迟，与 ADR-030 / T-088 的 ≤ 10 ms 目标冲突，宜用最小相位互补结构（交 R2）；CPU：每段一套检测，若每段都做 8× 真峰值，检测成本翻倍以上；默认关、GLAZE 0 逐位；需修订 ADR-001。
- **代价与风险**：L。风险：分频限幅让低频与高频的比例随底鼓变（频谱泵），低频冲击可能变软。
- **怎么验**：先只做 R1-01 的测量；方案阶段用合成底鼓 + 踩镲 + SMPTE 互调信号作阳性对照；22 + 45 首俱乐部 / 流媒体；Max 盲听 HMD 最高的 5 首。
- **对 Max 口味**：「提高音量」而不泵；多段限幅不在否决清单。

### R1-08 低频冲击守门

链位置：控制层（守护族，位置交 R5）· 类型：优化控制层 · 代价 M · P2

- **问题 / 现状**：预算环看 GR，落差守门看段落落差，PSR 守门（读入口）看宽带；没有一项专门保护低频冲击。Glue 侧链 100 Hz 高通（`ColorModule.h:10`）只让 Glue 不被底鼓基音推动，Clip 和限幅仍直接削平底鼓峰。TBC 把低频峰值因数当作「低频压过头」的读数。
- **提案**：新守门：出口对入口的低频峰值因数（R1-01 第 3 项口径，3 s 窗）损失超过阈值（起点 3 dB，待标定）并持续 3 s → 先按 0.5 dB/s 收 Clip 的自动份额，再收 Limit 自动增益；回程 0.15 dB/s；只作用于自动加上的部分（同设计规格-实时GLAZE-主段与增益 §4 第 4 条）；状态行「保冲击」。若 Max 不想再加守门，就只保留 R1-01 的测量。
- **依据**：TBC 帮助（低频 Crest Factor）；Ozone Low End Focus：Contrast 为正时拉大低频高低电平差、带出瞬态（Ozone 9 帮助）【一手】；smart:limit 以动态为目标、响度为结果（T-047 REPORT-A 第 4 条引）【一手】。
- **对硬约束与 ADR**：GLAZE 0 不动作；守门状态按 10 ms 时隙推进（与现有守门同形状，块长逐位）；`LIVE` 存档加一节（格式升号）；CPU：M 一路 4 阶低通 + 峰值 / 均方累加，< 0.1 %。
- **代价与风险**：M。风险：与俱乐部 −7 目标冲突时谁让（待拍板 7）；与预算环重复退让。
- **怎么验**：合成 House（kick + sub）GLAZE 100 俱乐部档：守门触发且 `low_crest_delta_db` 回到阈值内；22 + 45 首统计触发率与响度代价；Max 盲听触发最多的 5 首（守门开关，等响）。
- **对 Max 口味**：「全自动提高音量」但不把 drop 的底鼓磨平。

### R1-09 参考曲等响监听

链位置：末端监听 · 类型：工作流能力 · 代价 M · P2

- **问题 / 现状**：侧链 Reference 只用来学 27 段相对频谱（`ReferenceSpectrum.h:24-35`；高级母带控制 §5）；插件里不能听参考，宿主里独奏切换不等响，响的那边占便宜。
- **提案**：临时监听「听参考」：输出 = 侧链参考 × g；g 用 Listen 同一套 1 s K 加权能量窗（`CeladonChain.h:254-257`），把参考与母带中较响的一方衰减到另一方（只衰减）；与其他临时监听互斥；不存工程。
- **依据**：Metric AB 的参考响度匹配（Plugin Alliance 产品页）【一手】；Ozone Master Assistant 可按上传的参考曲匹配音色、宽度、动态与响度（iZotope 教程）【一手】；Katz, *An Integrated Approach to Metering, Monitoring, and Leveling Practices*, JAES 48(9), 2000（校准监听与等响比较）【一手-论文】。
- **对硬约束与 ADR**：只改监听；参考路径保持「只读侧链」（`ReferenceSpectrum.h` 文件头）；0 延迟；CPU 可忽略。
- **代价与风险**：M（处理器总线、界面、等响）。风险：各宿主侧链路由差异（参考学习已经碰过同类问题）。
- **怎么验**：单元测试等响误差 ≤ 0.2 LU；Max 在 FL 与 Live 各走一遍。
- **对 Max 口味**：工作流，不改声音。

### R1-10 流媒体版的手机翻译：低频虚拟谐波实验

链位置：Color（并联，算法与位置交 R4 / R6）· 类型：加工序 · 代价 M～L · P3

- **问题 / 现状**：Heat 支路先 150 Hz 高通再生成谐波（`ColorModule.h:16`），是为了不让低频推动高频互调（`docs/adaptive-mastering-review.md:53`、`docs/colour-and-render-revision.md:7-9`）——代价是 40～90 Hz 的底鼓 / sub 基音在手机、笔记本扬声器上没有可听的替身（专利背景段给的小扬声器典型截止约 150～300 Hz）。
- **提案**：只在交付 = 流媒体、默认关的实验项：M 路 ≤ 100 Hz 带通 → 谐波生成（偶次为主）→ 100～300 Hz 带通 → 小比例并联（GLAZE 控制量）；谐波都在 300 Hz 以下，1× 采样率即可，没有混叠问题。用 R1-01 的 `phone_rel_delta_lu` 量。
- **依据**：Larsen & Aarts, *Reproducing Low-Pitched Signals through Small Loudspeakers*, JAES 50(3), 2002（利用缺失基音的虚拟低音）【一手-论文】；Aalto 学位论文综述：3 寸扬声器预处理后低音感接近 5 寸【一手-论文】；USPTO 12593194 背景段的小扬声器截止频率【一手-专利，不是测量】；Ozone 12 Bass Control 宣称让低频在不同回放系统上翻译（未公开算法）【一手-营销】。
- **对硬约束与 ADR**：新工序 → 修订 ADR-001；默认关、GLAZE 0 逐位；0 延迟（IIR）；CPU 约 6 个双二阶 + 一次整形 / 样本，< 0.5 %【推断】；俱乐部档不开（PA 有真 sub，多出的 100～300 Hz 只会糊）。
- **代价与风险**：M（DSP）+ L（试听）。风险最高：Max 对「不自然、有点假」很敏感（ADR-015 的起因），虚拟低音最容易被听成假。
- **怎么验**：Max 用手机实听流媒体版开 / 关（同响度）3 首；监听音箱上 100～300 Hz 不糊（G5 指标不更差）。
- **对 Max 口味**：放最后；只有 Max 先在手机上听出「底鼓没了」时才值得做。

## 6. 未入前十的候选（一行一条）

- **EP 锚点**：参考学习扩展到「主段响度 + PSR」（Ozone 的参考匹配含响度与动态）——交付档已让同一张 EP 趋同，先用 R1-02 的 EP 汇总看极差，超了再做。
- **逐段目标隧道**（TBC 式按段容差带替代统一 1 dB 死区）——Balance 算法，交 R3。
- **噪声整形抖动**——−7～−10 LUFS 的 House 里 16 bit TPDF 的噪声远低于可闻，收益近零。
- **Low End Focus 式低频冲击 / 持续整形工序**——只有 R1-08 证明 Celadon 磨平了低频之后才考虑，否则是加料不是修复。
- **插件内编码预览**——Mac 上 Apple 自带的 AURoundTripAAC 能插在 Celadon 后面实时比较原声与编码版（Apple 简报）【一手】，插件内复刻编码器不值得。

## 7. 跨角度交接（只指出，不展开）

- R2：R1-07 的分频限幅结构与检测成本；R1-02(b) 俱乐部上限的实现路径、交付上限在 GLAZE 0 是否生效。
- R3：R1-06 的 M/S 路径切换与相位；逐段目标隧道。
- R4：R1-10 虚拟谐波与 Heat 的分工。
- R5：R1-01 指标的感知标定方法；R1-08 守门放在哪；范围外发现 1、2。
- R6：R1-07 / R1-10 在链上的位置与延迟账。
- 建议 Max 重新考虑：无。本角度没有足以推翻已否决项的新证据；多段限幅本来就不在否决清单里（ADR-001「试听后再评估」）。

## 待用户拍板

1. **俱乐部版有效 Ceiling**（R1-02b）：是否因下载店 MP3 320 转码收紧到 −1.5 dBTP（或俱乐部档默认编码余量 0.5 dB），还是保持 −1.0 只靠交付检查提示；若收紧，是只对新实例生效，还是像流媒体 −2 dBTP 那样改出厂值、旧工程跟着变（与「旧工程读入不改声音」冲突）。
2. **交付检查以哪个 AAC 编码器为准**：Apple Music 实际用 Apple AAC（macOS 自带 afconvert / afclip），DGX 上只有 ffmpeg 原生 AAC（T-085 已注明与 Apple 不同）。在 Max 的 Mac 上跑 afclip 检查导出文件，算不算「Mac 不跑测试」的例外（它是交付检查，不是编译或项目测试）。
3. **临时监听的组合规则**（R1-03 / R1-05 / R1-09）：规格「三种临时监听互斥」是否放宽为「原声 + 预听可叠加」，新增的单声道 / 侧声道 / 小音箱 / 听参考与现有三种怎样互斥。
4. **新指标进门槛的口径**（R1-01）：先只报告不判定，还是给初始阈值；阈值以什么为准（Max 盲听与指标排序的一致性门槛 Spearman ≥ 0.6 是我提的起点）。这属于完成标准，由 Max 定。
5. **M/S Balance 进不进「GLAZE 自动」**（R1-06）：侧声道只压不提会让声像略收窄，是否符合「母带不拉宽」的本意。
6. **多段限幅是否按 R1-01 的测量结果重新评估**（R1-07）；新增分频属于 Limit 模块还是另一级（模块边界），由 Max 定。
7. **低频冲击守门与交付响度冲突时谁优先**（R1-08）：俱乐部 −7 目标对「底鼓不被磨平」。
8. **虚拟低音值不值得做实验**（R1-10）：新工序要修订 ADR-001。
9. **范围外发现 1、2 由谁、何时处理**（出错时谁负责）。

## 范围外发现（只报不改）

1. **PSR 守门读的是入口 PSR，疑似偏离规格**。`GlazeController.cpp:1287-1288` 把 `analyzer.getShortTermView().psr3sDb` 交给 `PsrGuard`；这个读数来自 `MusicAnalyzer::processInput`——模块处理之前、链入口原始输入的 IN 表子块（`MusicAnalyzer.h:161`，`MusicAnalyzer.cpp:1248-1254`）。规格写的是「3 s PSR 实测 < F − 0.5 持续 3 s → 0.5 dB/s 退」（设计规格-实时GLAZE-主段与增益.md:83），T-047 REPORT-C 的原意是约束**出口**（响段短期响度上限 = 真峰值上限 − PSR 下限）。读入口时，Limit 增益退让不会改变读数，守门无法收敛：输入本来就密（PSR 低于下限 − 0.5）时会一路退到守门下限（默认 −3 dB，`PsrGuard.h:50`）或把自动加上的部分退完（`GlazeController.cpp:864-865`）；输入松时永远不触发，Celadon 自己压扁了也看不见。另：规格说「通用与 House 现有四项默认关」，代码里 PSR 守门在任何曲风都参与（`GlazeController.cpp:861-862`），下限只按交付分（`PsrGuard.cpp:7-16`）。修法候选（不在本任务做）：改读 OUT 表子块（`LoudnessMeter` 子块已带真峰值），并按规格加曲风开关。需要 R5 / J2 复核。
2. **落差守门的实现与规格不符**。规格：输入 / 输出各一份 S 直方图，100 ms 一笔、按有效音频遗忘 τ 180 s；两份都满 60 s 且输入 P90 − P25 ≥ 3 LU 才判定；超门持续 10 s 才退 0.1 dB/s；低于门槛 0.3 LU 持续 30 s 才以 0.05 dB/s 回；4/4 型开、响度型先关、被动型关（设计规格-实时GLAZE-主段与增益.md:80-82、:118）。代码：128 笔环形缓冲（约 12.8 s，`ContrastGuard.h:42`），满 8 笔（0.8 s）就判定（`ContrastGuard.cpp:65`），没有 60 s / 3 LU 前提，没有 10 s 持续条件，回程与退让同速 0.1 dB/s（`ContrastGuard.cpp:112-127`），任何主段模型都开（`GlazeController.cpp:1286`）。12.8 s 窗装不下 16～32 小节的 breakdown（124 BPM 约 31～62 s），量到的更像「转场附近的局部落差」，不是规格要保的段落对比。没找到说明这是有意简化的档案（T-048 阶段 2 的 `INTERFACE-SKETCH.md:168` 仍写「直方图」）。需要 R5 / J2 复核。
3. **规格陈旧**：设计规格-曲风与交付 §4.3（第 78～80 行）与 §5（第 94 行）仍写「重新聆听 / 再校准两轮 / `GlazeCalibrator::begin`」，ADR-030 已取消聆听、锁定与校准器；设计规格-高级母带控制 §3（第 42 行）的「校准参考条件 / Mast 哈希」同理；设计规格-总体第 88 行「界面仍只显示 OUT 的 LUFS 与 IN 的真峰值」已过时（现在 IN / OUT 都显示真峰值，`LevelMeters.h:5-9`）。

## 证据索引

**本仓库源码（工作树 `hopeful-volhard-c634dc`，main `3a7a367`）**

- `Source/dsp/BalanceModule.h:8-24`（分析与执行，`:23` 两声道同一组增益）、`:134-139`（bandCross）
- `Source/dsp/ClayModule.h:10-20`（低切 / 收单 / 加宽 / 守护）
- `Source/dsp/ColorModule.h:9-24`（Tone / Glue 侧链 100 Hz / Heat 150 Hz 高通、Side ×0.75）
- `Source/dsp/LimiterModule.h:9-12`（Clip + 限幅）、`:19-21`（0.35 dB 重建余量）、`:24-25`（低频周期下限，孤立鼓击不触发）
- `Source/dsp/LimiterModule.cpp:885-897`（左右联动取最大、单一包络）
- `Source/dsp/GlazeConstants.h:174-180`（交付常数）；`Source/dsp/GlazeConstants.cpp:78-126`（限幅 GR 预算）
- `Source/dsp/CeladonChain.h:10-16`（Listen）、`:32-36`（试听差值）、`:112-119`（听原声 / 差值）、`:207-208`（跟拍释放）、`:254-257`（Listen 1 s 窗）
- `Source/dsp/GlazeTypes.h:110-145`（画像特征）、`:429`（单声道低频损失）
- `Source/dsp/PsrGuard.cpp:7-16, 49-61`；`Source/dsp/ContrastGuard.h:42-45`；`Source/dsp/ContrastGuard.cpp:63-82, 112-127`
- `Source/dsp/GlazeController.cpp:861-862, 897, 1283-1288`；`Source/dsp/MusicAnalyzer.h:161`；`Source/dsp/MusicAnalyzer.cpp:1248-1254`
- `Source/dsp/ReferenceSpectrum.h:24-35`；`Source/dsp/ReleaseSafety.h`（抖动 / 预听）
- `Source/ui/SpectrumView.h:84-101`（响度小窗）；`Source/ui/LevelMeters.h:5-9`；`Source/ui/ModuleCards.h:11-12`
- `tools/render/README.md:131-146`（指标列）、`:286-315`（逐段统计）、`:556-583`（盲听）；`tools/render/codec_tp_check.py`
- `docs/validation/2026-10-08-T085-codec-tp/REPORT.md`（编码过冲表、AAC PNS 补测表、归一化模型）
- `docs/validation/2026-10-02-T047-realtime-research/REPORT-C.md`（PSR 区间、T_main = C − F）、`REPORT-A.md` 第 4 条（smart:limit 以动态为目标）
- `docs/validation/2026-10-01-T038-genre-mastering-research/REPORT.md`（曲风与交付依据）
- `docs/adaptive-mastering-review.md:53`；`docs/colour-and-render-revision.md:7-9`
- `docs/validation/2026-10-05-T048-phase2/INTERFACE-SKETCH.md:168`

**vault 档案（`项目/Celadon/`）**

- `01-框架/设计规格-总体.md`：第 18 行（非目标）、第 88 行、第 124 行、第 134 行
- `01-框架/设计规格-高级母带控制.md`：§1 第 17 行（三种临时监听互斥）、§3 第 42 行、§4、§5、§6 第 75～82 行
- `01-框架/设计规格-曲风与交付.md`：§3.1、§3.2 第 48 行、§4.1 第 58～68 行、§4.2、§4.3 第 78～80 行、§5 第 94 行
- `01-框架/设计规格-实时GLAZE-主段与增益.md`：第 70 行、第 79～83 行、第 118 行
- `03-记忆/ADR-001`（多段限幅「v0.3 试听后再评估」）、`ADR-004`、`ADR-014`、`ADR-015`、`ADR-016`、`ADR-020`、`ADR-027`、`ADR-028`、`ADR-029`、`ADR-030`
- `04-协调/TASKS.md` T-086 行（可解释性与 A/B 的范围）

**外部一手**

- iZotope，Inside Ozone 12（Lukin 谈 IRC 5 多段、四段增益包络；Unlimiter）：https://www.izotope.com/en/learn/inside-ozone-12 【一手】
- iZotope，Why upgrade to Ozone 12（Bass Control、IRC 5 宣称）：https://www.izotope.com/community/blog/ozone-12 【一手-营销】
- iZotope，How to use Master Assistant in Ozone 12（参考曲匹配音色 / 宽度 / 动态 / 响度；分析最响段）：https://www.izotope.com/en/learn/how-to-use-master-assistant-in-ozone.html 【一手】
- iZotope Tonal Balance Control 帮助（Crest Factor、目标范围、四段宽视图）：https://downloads.izotope.com/docs/tonal-balance-control/meters-and-target-curves/index.html 【一手】
- iZotope Ozone 8 Maximizer 帮助（IRC IV）：https://downloads.izotope.com/docs/ozone8/maximizer/index.html 【一手】
- iZotope Low End Focus（20～300 Hz、Punchy / Smooth、M/S）：https://www.izotope.com/en/products/ozone/features/low-end-focus 【一手】；https://www.izotope.com/en/learn/mastering-bass-with-low-end-focus 【一手】
- oeksound soothe 手册（L/R 与 M/S、链接、delta）：https://oeksound.com/manuals/soothe3 【一手】
- Soundtheory Gullfoss 手册（立体声兼容 M/S、延迟约 20 ms）：https://www.soundtheory.com/static/Gullfoss%20Operation%20Manual.pdf 【一手】
- FabFilter Pro-L 2 帮助（BS.1770-4、LRA、目标电平、Unity Gain、Audition Limiting）：https://www.fabfilter.com/help/pro-l/using/loudnessmetering 【一手】
- sonible 帮助（smart:limit 以 3 s PSR 中位为动态量）：https://help.sonible.com/hc/en-us/articles/4413934150546 【一手】
- A.O.M. Invisible Limiter G3（Soft Limiter 多级限幅、算法改进）：https://rekkerd.org/?p=222246 【一手-厂商新闻转载】
- Plugin Alliance / ADPTR Metric AB（Mono / Sides 监听、相关度、参考响度匹配）：https://www.plugin-alliance.com/en/products/adptr_metricab.html 【一手】；MusicTech 评测 https://www.musictech.net/reviews/plug-ins/adptr-audio-plugin-alliance-metricab/ 【评测】
- Audified MixChecker（设备平均响应模拟）：https://www.soundonsound.com/news/mixchecker-ultra-audified 【评测】
- Apple Digital Masters 技术简报（afconvert / afclip / AURoundTripAAC、至少 1 dB 余量、Sound Check、俱乐部母带）：https://www.apple.com/itunes/docs/apple-digital-masters.pdf 【一手】
- Spotify 响度归一化：https://support.spotify.com/us/artists/article/loudness-normalization/ 【一手】
- LabelGrid，Beatport 要求（WAV / AIFF 16 / 24 bit、44.1 kHz、MP3 320）：https://help.labelgrid.com/en/distribution/beatport-requirements/ 【一手-分销商】
- EBU Tech 3342（LRA）：https://tech.ebu.ch/docs/tech/tech3342.pdf 【一手】；ITU-R BS.1770-5：https://www.itu.int/rec/R-REC-BS.1770-5-202311-I/en 【一手】
- Nielsen & Lund，0dBFS+ Levels in Digital Mastering，AES 109th Convention，Paper 5251，2000 【一手-论文】
- Katz，An Integrated Approach to Metering, Monitoring, and Leveling Practices，JAES 48(9)，2000 【一手-论文】
- Larsen & Aarts，Reproducing Low-Pitched Signals through Small Loudspeakers，JAES 50(3)，2002 【一手-论文】
- Aalto 学位论文（虚拟低音综述）：https://aaltodoc.aalto.fi/items/28f25581-3beb-49e4-8418-506dcb5b3446 【一手-论文】
- USPTO 12593194（虚拟低音专利背景段的小扬声器截止）：https://image-ppubs.uspto.gov/dirsearch-public/print/downloadPdf/12593194 【一手-专利】
- 公开访谈：按任务书检索了 Alex Psaroudakis、Matt Colton 的公开访谈，没找到可核查的具体技术表述，本报告不引用（避免把二手转述当事实）。

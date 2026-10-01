# T-037 官网中英双语校对报告

> 校对代理（Opus 5.5 · max，只读）产出；它写不了文件，由主进程原样落盘。处理结果见文末「处理记录」。

- 日期：2026-10-01（10:39 PDT）
- 范围：`git diff c1a6918..HEAD -- site/`，分支 `claude/site-i18n`
- 方法：中英逐页对照通读。数字用脚本比对过两边，差异只来自「1/3 倍频程」「第 3 行」「0 分配 0 锁」这类序数和固定说法，没有错抄。有疑点的行为和数值去 `Source/` 核对过。只读。

## 各类条数

| 类别 | 条数 | 其中存疑 |
|---|---|---|
| 错译 / 事实错 | 12 | 5 |
| 漏译 | 2 | 0 |
| 术语不统一 | 8 | 0 |
| 英文不自然 | 12 | 0 |
| 风格 | 6 | 0 |
| i18n 代码问题 | 3 | 1 |

## 1. 结论
英文整体忠实、通顺，术语基本照术语表，没提任何商业插件，数字没有错抄。要改的主要是四处：A1（首页「逐位契约」）、A3（导出清单的 "Leave"）、A2（状态机图的「参考条件」），加上颜色名 "celadon" 和插件帮助面板的 "Cyan" 对不上（C1）。其余都是轻微的措辞和统一问题。

## 2. 问题清单

### 2.1 错译 / 事实错

**A1 · `en/home.json:217`（zh 同一处）· 事实错**
- 现在：With GLAZE at 0, or modules at zero or bypassed, the output equals the input exactly; …
- 建议（英）：With GLAZE at 0, the chain is bit-identical to the same chain without GLAZE; with a module at zero or bypassed, it passes the input through unchanged — at any block size, SIMD or scalar, idle-skipping or always running, identical bit for bit.
- 建议（中）：GLAZE 为 0 时与没有 GLAZE 的参照链逐位相同；模块归零或旁通时输出与输入零差；任意块长……逐位相同。
- 依据：`algorithms/zh/overview.md:20`、`algorithms/zh/glaze.md:68`。英文用 "or" 把三种情况并列，错得比中文更明显。

**A2 · `figures.json:120`、`:122` · 错译**
- 现在：a reference change voids calibration
- 建议：any change in the reference conditions voids calibration（窄版写 "Changed conditions void calibration"）
- 依据：参考条件指手动参数、采样率、算法版本和高级设置（`algorithms/zh/glaze.md:46-48`），不是参考曲。

**A3 · `docs/en/listening.md:33`、`docs/en/quickstart.md:26` · 歧义，可能读反**
- 现在：Leave Normalisation preview and Listen to difference / leave Listen to difference
- 建议：Exit（或 turn off）
- 理由：两处都在导出前的清单里；实时导出时预听开着会衰减导出结果（`docs/en/advanced.md:47`）。

**A4 · `algorithms/en/clay.md:27` · 意思偏了**
- 现在：if a peak keeps rising during playback
- 建议：if it keeps raising the peaks during playback
- 依据：中文是「播放中若一直抬峰值」，主语是修坯本身；插件原文 `HelpText.cpp:408` 是 "backs off during playback if it keeps raising them"。

**A5 · 事实（中英都有）：CLAY「自动」不收单，加宽也不经峰值守护**
- `docs/en/quickstart.md:33`：现在 "CLAY's three controls default to Auto: they only act when this song really has a problem…"；建议 "Low cut and Width act only when this song really needs it and a trial run shows they won't raise the peaks; Mono on Auto never folds the lows — it only warns when they're out of phase."
- `en/home.json:70`：现在 "…fold the lows to mono… each move is tried on this song first, and backs off by itself if it would raise the peaks"；建议 "…fold the lows to mono (by hand)… automatic moves are tried on this song first, and Low cut and Mono step aside whenever they would raise a peak."
- 依据：`HelpText.cpp:421-424`、`:466`；`docs/zh/clay.md:27`。这两处中文不在这次的同步清单里。

**A6 · 存疑 · `docs/en/install.md:43`（zh 同一行）**
- 现在：44.1 / 48 kHz 合一行，1384 samples
- 问题：修坯和前瞻的延迟随采样率变化（`LinearPhaseLowpass.cpp:31-39`，G = round(0.008fs) + round(0.003fs)）。按源码推算，44.1 kHz 约 528 + 485 + 64 + 60 + 88 + 62 = 1287 样本 ≈ 29.2 ms。48、96、192 kHz 三档按同一套公式验过，都对。
- 存疑原因：64、60、62 三项是按文档写的「固定样本数」算的，没有逐一去源码确认。
- 建议：拆成两行，44.1 kHz 那行先实测再填。

**A7 · 存疑 · `en/home.json:269`（zh 同一处）**
- 现在：标签 "Audio-thread budget"，配的数值是 1.3 %
- 问题：1.3 % 是实测占用；「预算」在网站别处指 15 % 的门槛（`verification.md:3`、`tests/BalancePerformanceTests.cpp:83`）。
- 建议："Audio-thread load" /「音频线程占用」。

**A8 · 存疑 · `docs/en/faq.md:48`、`algorithms/en/clay.md:18`（中文原文同样）**
- 现在：Low cut removes energy below 30 Hz
- 问题：低切范围 10～60 Hz（`ParameterRanges.h`），默认 25 Hz，自动只切 16 Hz 以下，「30 Hz」和哪个数都对不上。
- 建议："10–60 Hz; on Auto, only infrasound below 16 Hz"。

**A9 · 存疑 · Glue「低、中、高分开施压」的标题和要点（中英都有）**
- 位置：`en/home.json:80`、`:83`、`:248`，`docs/en/color.md:18`，`algorithms/en/color.md:7`
- 问题：0.1.8.6 起中频直接用联动增益（`ColorModule.cpp:1282-1283`），只有低、高两段有自己的增益。算法页正文已改，标题和首页要点没改。
- 建议："Glue gives the lows and highs their own gain"。
- 存疑原因：「分开施压」也可以宽松地理解成三段各自乘增益。

**A10 · `en/home.json:41`（中文「十几秒」）**
- 现在：for a dozen seconds or so
- 问题：全站其他地方都写聆听 15～30 秒。
- 建议：for 15–30 seconds。

**A11 · `docs/zh/interface.md:31`（只有中文）**
- 现在：COLOR 卡片副题写「染色」
- 问题：插件里是「音色」（`Localisation.cpp:121`）；英文 "Tone" 是对的。

**A12 · 存疑 · `algorithms/{zh,en}/glaze.md:9`**
- 现在：「算法值为底，旋钮是微调」
- 问题：首页和 `docs/glaze.md:12` 都说「手动值是底，GLAZE 往上加」，这里正好反过来。英文是忠实翻译，问题在中文原文。
- 存疑原因：可能是设计稿原话，有意保留。

### 2.2 漏译
- **B1** `en/common.json:33`：漏了「不商用」。建议 "non-commercial and not for sale"。
- **B2** `share.json:5`：漏了「自用」。建议 "A personal mastering plugin"。
- 说明：`docs/en/balance.md:8` 的 Tame 少译「箱音和刺耳感减少」，与插件英文悬停提示一致，是有意的，不算漏译。

### 2.3 术语不统一
- **C1 · 颜色名**（`docs/en/interface.md:1`、`glaze.md:12`、`clay.md:13`、`controls.md:17`）："celadon = your settings" 应改为 "cyan"。依据：插件帮助面板 "Cyan = your setting"（`Localisation.cpp:236`），修坯灯 "cyan = manual"（`HelpText.cpp:467`）；`controls.md:17` 句首大写的 "Celadon =" 还会被读成产品名。
- **C2 · 「标定快照」**（`docs/en/glaze.md:56`、`algorithms/en/glaze.md:18`、`:44`）：译成 "calibration snapshot / calibration version"，与「校准」同词。建议统一成 "constants snapshot / constants version 8"。
- **C3 · GLAZE 的「保护」**（`figures.json:11-12`）："calibrate → guard" 建议改成 "protect"；guard 在全站是修坯「峰值守护」的词。
- **C4 · 引用的插件文字要逐字一致**（`docs/en/controls.md:32`、`docs/en/clay.md:13`、`algorithms/en/clay.md:23`）：实时值行插件原文 "Low cut 22 Hz · No mono · High width +0.8 dB"（`HelpText.cpp:642`、`:649`）；代价说明插件原文 "Tried on this song: low cut would raise peaks, reduced automatically"（`HelpText.cpp:736`），网站多了一个 "the"。
- **C5 · 同一张卡片两种语言页面的英文不一致**（`en/home.json:195/200/205` 对 `zh/home.json:196/206`）：Level-matched / Loudness-matched、Reference learning / Reference from sidechain 各用了两种。建议统一，并用插件写法 "Listen to difference"。
- **C6 · 修坯的灯**：网站写 light，插件写 lamp（`HelpText.cpp:466`）。可选。
- **C7 · 延迟图**（`figures.json:127`）：英文 "Clip 60"、中文「过采样 60」各说一半。建议两边都写 Clip 过采样 60。
- **C8 · `changelog.ts:298`**："Tame ceiling" 和 LIMIT 的 Ceiling 撞名，建议 "Tame's maximum cut"。

### 2.4 英文不自然
- **D1** 逗号列表里的 "Loud, not harsh"（`docs/en/quickstart.md:25`、`:32`，`faq.md:16`，`install.md:51`，`advanced.md:51` 两处，`glaze.md:50`，`changelog.ts:47`、`:118`）：名字自带逗号，列表里读起来断错。正文用 "harshness control" 或加引号；标题、页签、表格里的控件名不动。
- **D2** "leaves them all"（`interface.md:23`、`listening.md:1`）→ "exits all of them"。
- **D3** `controls.md:17`："invalid audio blocks blocked" → "invalid (NaN / Inf) audio blocks caught"。
- **D4** `download.json:84`、`:86`、`:95-101`："This is a phone / This is Linux" 直译 → "You're on a phone" / "Looks like…"（按浏览器信息猜的，looks like 更准）。
- **D5** "only grows with GLAZE"（`home.json:128`、`docs/en/glaze.md:9`）→ "only ever rises as GLAZE goes up"。
- **D6** `algorithms/en/glaze.md:44`：句子太绕 → "only if, within its limit, it can shave at least 0.1 dB off this song's peaks"。
- **D7** `home.json:225`：宾语前置生硬 → "The interface keeps reminding you about anything you should switch off before export."
- **D8** `home.json:100`：图注 "width" 与参数 Width 撞名 → "band thickness = side (S)"。
- **D9** `docs.json:13`：分组名 "Judging" 意思不清 → "Judging the result" 或 "Monitoring and checks"。
- **D10** `home.json:293`："Arch" → "Chip" 或 "Architecture"。
- **D11** `figures.json:114`："Matches loudness gap" → "Aligns in/out loudness"。
- **D12** 「许可」译成 permission（`algorithms/en/balance.md:29-34`、`docs/en/balance.md:17`、`docs.json:80`）：可选改 gate / allowance。

### 2.5 风格
- **E1** 英文里 28 处 "35 %" 带空格，插件英文与英文习惯写 "35%"。
- **E2** `changelog.ts:315`："Sep 24 – 26, 2026" → "Sep 24–26, 2026"。
- **E3** `algorithms/en/glaze.md:1` 引语可更顺（可选）。
- **E4**（`common.json:4`、`share.json:5`、`index.html:9`/`:44`）："VST3 / AU for macOS and Windows" 易被读成 Windows 也有 AU → "VST3 / AU on macOS, VST3 on Windows"。中文同样。
- **E5** `home.json:165`：一句两个 how（可选）。
- **E6** `changelog.json:4`："firing by firing" 是窑烧意象，术语表说能不用就不用（可选，Max 定）。

## 3. 看过、没发现问题的部分
- 界面短句：`common` 的 nav、pkg、downloadButton、notFound；`download` 中 D4 以外；`changelog` 的 kinds、lead；`docs` 的 sections、layout、toc、search；`figures` 的 chain、limiter、truepeak、balance、glue、heat、clay；`share.json` 的 zh。
- 说明页：`meters.md`、`limit.md`，以及 color、advanced、install、balance 除上面条目以外。
- 算法页：`overview.md`、`limit.md`、`verification.md`，以及 balance、color 除上面条目以外。
- 0.1.8.6 同步改动抽查源码，中英都与源码一致：推响不刺三段 60 ms 快电平（`BalanceModule.cpp:1790-1795`）；系数每 1/8 hop（`:46`、`:286`）；van Herk（`tests/LimiterTests.cpp:710`）；预算 15 %（`tests/BalancePerformanceTests.cpp:83`）；自动 Heat 门槛 0.1 dB、底量 12（`GlazeConstants.h:236`、`.cpp:79/102/125`）；Glue 中频联动增益（`ColorModule.cpp:1282`）；修坯守护只管低切和收单（`HelpText.cpp:466`）。
- 更新日志：除 C8、D1、E2 外，中英一一对应，数字一致。
- 组件：`Nav.tsx` 语言开关、`LangHint.tsx`、`index.html` 头部无问题。
- 商业插件：全文检索常见厂商与产品名，无。

## 4. i18n 代码问题
- **F1 · `Showcase.tsx:26`**：`useState(useLocale())` 只在首次挂载取一次语言。`/` 与 `/zh` 渲染同一棵 `<Home />`（`App.tsx:79-80`，无 key），用导航开关切语言时首页不重挂，「真界面」截图与其语言开关停在 English。修法：给 Home 或 Showcase 加 `key={locale}`，或用 effect 同步。
- **F2 · `locale.tsx:36`**：存过中文偏好的人从英文链接进来，`LocaleSync` 把 hash 原样带到中文页；文档标题锚点两种语言不同，锚点找不到。首页 `#process` 等两种语言相同，不受影响。修法：`/docs`、`/algorithms` 下跳转不带 hash。
- **F3 · 存疑 · `Home.tsx:33`**：`ScrollTrigger.refresh()` 只在字体加载完时触发；中文切回英文而英文字体早已加载时不会刷新，「四道工序」钉住滚动的触发位置可能偏。未运行验证。建议语言变化后也刷新一次。
- 查过没问题的：`localizePath` / `stripLocale`（`/zhuyin` 不误判）、canonical 与 hreflang、`gen-zh-html.mjs`、复数键、日期与千分位、`Trans` 标签映射。

## 处理记录（主进程，2026-10-01）

事实类先回源码核过再改；中文与插件不符的一并改（Max「中英都按现在的插件写」）。

| 条目 | 处理 | 说明 |
|---|---|---|
| A1 | 已改（中英） | 「GLAZE 0 = 与参照链逐位相同；模块归零 / 旁通 = 输入零差」分开写 |
| A2 | 已改 | 宽版 "changed reference conditions void calibration"（"any change in…" 太长，框里只剩 6 px）；窄版 "Changed conditions void calibration" |
| A3 | 已改 | listening / quickstart 的 Leave → Exit |
| A4 | 已改 | 依 `HelpText.cpp:408` |
| A5 | 已改（中英） | 首页 CLAY 导语、快速上手第 33 行：自动不收单只提醒；低切与收单会抬峰值就让开（依 `HelpText.cpp:421-424、462-467`） |
| A6 | 已改（中英） | 核源码：收单 D = 2(round(0.24·fs/40) − 1)、守护 G = round(0.008fs) + round(0.003fs)、前瞻 round(2 ms·fs)，44.1 kHz = 528 + 485 + 64 + 60 + 88 + 62 = **1287 样本 ≈ 29.2 ms**；48 kHz 与 `tests/ChainTests.cpp` 的 1384 一致。表拆成两行 |
| A7 | 已改（中英） | 「音频线程占用」/ "Audio-thread load" |
| A8 | 已改（中英） | 核 `ParameterRanges.h`（10～60 Hz）、`ParameterDefaults.h`（25 Hz）、帮助文本（自动只切 16 Hz 以下）：faq 与算法 CLAY 两处改写 |
| A9 | 已改（中英） | 首页要点与卡片标题、说明 COLOR、算法 COLOR 标题改「低、高两段分开施压，中频直接用联动增益」；示意图中文标签「三段分开施压」→「分频施压」。更新日志 0.1.7.2 那条是当时的事实，不改 |
| A10 | 已改（中英） | 「15～30 秒」 |
| A11 | 已改 | 界面页表格 COLOR 副题「音色」（同表其余三行都是插件卡片副题） |
| A12 | **不改** | 原话出自 `设计规格-GLAZE.md` 决定表，后半句公式 actual = clamp(manual + auto) 已讲清，与首页说法同一公式 |
| B1 / B2 | 已改 | "non-commercial and not for sale"；share.json 与 index.html 改 "A personal mastering plugin" |
| C1 | 已改 | interface / glaze / clay / controls 的 celadon → cyan（依 `Localisation.cpp:236`） |
| C2 | 已改 | constants snapshot / constants version 8 |
| C3 | 已改 | calibrate → protect（两套排版量过不出框） |
| C4 | 已改 | 实时值行与代价说明逐字照 `HelpText.cpp:640-652、732-744` |
| C5 | 已改 | 两种语言统一 "Level-matched A / B"、"Listen to difference"、"Reference learning" |
| C6 | 已改 | CLAY 的 light → lamp（clay ×4、controls、interface） |
| C7 | 已改（中英） | 「Clip 过采样 60」/ "Clip oversampling 60"；延迟图横排读数改正文字体、横条左移，英文那行右端 687 / 720 |
| C8 | 已改 | "Tame's maximum cut" |
| D1 | 已改 | 逗号 / and 列表里的控件名加弯引号 “Loud, not harsh”（快速上手 ×2、FAQ、安装、高级 ×2、GLAZE、BALANCE、算法总览、更新日志 ×3）；斜杠分隔的页签名、标题、表格控件名不动 |
| D2–D11 | 已改 | 按建议；D4 用 "Looks like…"；D9 "Judging the result"；D10 "Architecture" |
| D12 | **不改** | 插件英文里没有「许可」的对应词；"permission" 在上下文里意思清楚 |
| E1 | 已改 | 英文资源 98 处 "N %" → "N%"（数字墙的单位是单独小字标签，不动） |
| E2 / E4 | 已改（E4 中英） | "Sep 24–26, 2026"；"VST3 / AU on macOS, VST3 on Windows"（common / share / index.html 同步） |
| E3 / E5 | 不改 | 可选的风格项 |
| E6 | 留给 Max | 更新日志标题 "firing by firing"（窑烧意象）要不要换 |
| F1 / F3 | 已改 | 根因：`/` 与 `/zh` 同一位置的页面组件被复用。改为 `<Routes key={locale}>`：切语言整页重挂，截图语言、首屏拆字 / 拆词、钉住滚动的测量都按新语言重来。107 预览实测：中 ↔ 英切换后截图开关跟着变、标题重新按词拆，控制台 0 错误 |
| F2 | 已改 | `LocaleSync` 跳中文时 /docs、/algorithms 下不带锚点；实测 `/docs/clay#peak-guard` → `/zh/docs/clay`，`/#listen` → `/zh#listen` |

验证：107 `pnpm run build` 通过（check-i18n、tsc、vite build、sitemap、/zh 首屏 HTML）；改过文字的 3 张示意图中英、横竖两套逐张量过，无出框、无压线。

# 官网英文术语表（T-037）

以插件英文界面为准：`Source/ui/Localisation.cpp`（中英对照表）、`Source/ui/GlazeStatusText.cpp`、`Source/ui/HelpText.cpp` 的英文段。
插件英文是**英式拼写**（colour、normalisation、behaviour），官网跟着用。不提任何别家插件（ADR-023）。

## 模块与总旋钮（原样，全大写）

| 中文 | 英文 | 说明 |
|---|---|---|
| GLAZE（施釉、总旋钮） | GLAZE | 「智能母带」= Smart mastering；「主段」= main section；「跟着主段慢慢调整」= Follows the main section（插件已没有「重新聆听 / Relearn」） |
| BALANCE（平衡） | BALANCE | 卡片副题「动态均衡」= Dynamic EQ |
| CLAY（修坯） | CLAY | 卡片副题「声像」= Stereo；「修坯」作动作讲时 = stereo clean-up / shaping |
| COLOR（染色） | COLOR | 模块名保留美式 COLOR（插件就这么写）；正文里的「音色」= tone / colour |
| LIMIT（限幅） | LIMIT | 卡片副题「限幅」= Limiter |

## 参数（照插件）

回补 Recover · 抑制 Tame · 提亮 Brighten · 速度 Speed（慢 / 中 / 快 = Slow / Mid / Fast）· 27 段 27 bands
低切 Low cut · 收单 Mono · 加宽 Width · 自动 / 关 / 手动 = Auto / Off / Manual
音调 Tone · 粘合 Glue · 热度 Heat · 干湿 Mix · 输出 Output
增益 Gain · 上限 Ceiling · 削波 Clip · 释放 Release · 预读 Lookahead
推响不刺 Loud, not harsh（正文里叫 harshness control）· 抑制力度 Amount · 瞬态保护 Transients
目标频谱 Target spectrum（温暖 / 均衡 / 明亮 / 参考 = Warm / Neutral / Bright / Reference）· 匹配力度 Amount · 学习参考 Learn reference
发行安全 Release safety · 编码余量 Margin（正文 codec margin）· 最终位深抖动 Final bit-depth dither · 归一化预听 Normalisation preview · 预听响度 preview loudness

## 风格、状态、表

风格：自动 Auto · 温润 Warm · 冲击 Punchy · 通透 Clean
曲风 Genre：通用 General · Deep / Tech / Melodic（选择器上只写这三个简称；正文提到选项时照写简称，悬停气泡与专门介绍子类时写全名 Deep House / Tech House / Melodic-Progressive；三者合称 House genres）
交付 Delivery：标准 Standard · 流媒体 Streaming · 俱乐部 Club
House：小节 bar · 主段落 main section（drop = drop，breakdown = breakdown，不译）· 逐小节的记录 = tally · 状态行前缀「House ·」· 反相提示 = out-of-phase hint（插件里的原句是 Club mono loses low end / Fix it in the mix）· 门控 / 门 gating / the gates
状态（GLAZE 标题旁的阶段标签）：全手动 Manual · 受手动 Manual limit · 反相 Phase · 导出 Export · 受限 Limited · 起步 Starting · 保持 Holding · 跟随 Following；卡片旁通 已旁通 Bypassed
状态行原句（照 GlazeStatusText.cpp 的英文）：受限 · 预算 / 交付峰值 / 保对比 / PSR · 已退 x dB = Limited · Budget / Delivery peak / Contrast / PSR · backed off x dB · 受手动设置限制 = Limited by manual settings（标签 受手动 = Manual limit）· 起步 Starting · 保持 Holding · 跟随 Following；House 门控：门控 gating · 门 gate（正文 the gates）
监听：处理后 Processed · 原声 Original · 等响 Match（level-matched）· 试听差值 Listen to difference · 旁通 Bypass
表：输入 / 输出 / 衰减 = IN / OUT / GR · 响度 Loudness · 真峰值 True peak（dBTP）· 短期 Short-term · 峰响比 PLR · 重新计量 Reset meters · 频谱仪 spectrum analyser

## 写法约定

- 单位与数字：LUFS、dBTP、dB、kHz、ms 前空一格；千分位用逗号（134,523）；日期 `Oct 1, 2026`。
- 芯片：Apple silicon（小写 s，苹果官方写法）/ Intel；系统 macOS 12 / Windows 10 / 11（64-bit）。
- 瓷器意象（开窑、施釉、修坯、开片、釉色）意译：kiln / glaze / trim / crackle，能不用就不用，不硬译。
- 语气：短句、主动语态、第二人称（you）、具体数字；不夸张，不用 “revolutionary” 一类词。

# T-085 编码后真峰值与流媒体目标对比

2026-10-08；基线 `5a2142572df76ef30bcd66786d68011cba1ffd7e`（0.1.8.9，阶段 2 连续 GLAZE）。**本次未改任何默认值**，未改 DSP、插件行为或现有测试；仅新增离线测量脚本、渲染工具显式覆盖选项与报告。本地分支 `chatgpt/t085-codec-tp`；未打包、部署、push 或开 PR。

核心结论：当前 PCM 封顶有效，但本机编码回测不支持编码后安全保证；只把流媒体基准从 −10 改成 −12 的效果很小。后续建议优先验证“有效响度上限 + 曲风 PSR 下限”，并把编码后 TP 作为交付检查。

## 语料与范围

| 语料 | 数量 / 选择 | 说明 |
|---|---|---|
| 真实歌曲 | T-038 Jamendo 清单每类按清单顺序前 3 首，共 9 首完整歌曲 | Deep / Tech / Melodic 各 3；见 [corpus.csv](corpus.csv)，原 MP3 的下载 URL、曲名、艺人 ID 与 SHA-256 均有记录 |
| 合成压力 | 4 首，每首 40 s，48 kHz 双声道，随机种子 85 | 宽带 tanh 热混音、密集高通踩镲、反相 Side 55 Hz 低音、20 Hz～22 kHz 双向对数扫频；短淡入淡出；热混音最多衰减到 −8 LUFS，其余最多衰减到 −18 LUFS；峰值预先归一至 0.97 |
| 原 22 首门槛 / 完整 45 首集 | 本次未跑 | 本工作区及可读验证目录未找到音频实体；采用仓库记录的公开下载方案，不把 9 首子集称为完整回归 |

真实歌曲全长、不裁 drop，不做预先响度归一；原下载 MP3 解码为 48 kHz 双声道浮点 WAV。**源已是有损音频**，本次测的是 Celadon 输出后的再次编码；不能直接外推到无损原始母带。音频仅在 `artifacts/t085/`，不随 git 分发。公开曲目许可及非商业研究范围沿用 [T-038 说明](../2026-10-01-T038-reference/README.md)。[input_manifest.csv](input_manifest.csv) 保存实际帧数、采样率与输入 WAV 哈希。

## 设置与方法

| 项目 | 本次设置 / 口径 |
|---|---|
| 渲染 | Release `celadon_render`；GLAZE 100、style auto、clay auto、genre general、delivery streaming、block 512；所有曲目用同一通用档，隔离交付目标差异 |
| 原交付值 | streaming 基准 −10 LUFS、范围 −12～−8、有效 Ceiling ≤ −2 dBTP；club 基准 −7、范围 −9～−5；标准档沿用风格目标；club 不套 streaming 的 −2 TP 封顶，仍取用户 Ceiling 和编码余量；来源 `Source/dsp/GlazeConstants.h` |
| Ceiling / 余量 | 用户 Ceiling −1 dBTP、codec margin 0 dB；流媒体封顶为 −2；LIMIT 内部重建余量 0.35 dB 保留；本次未覆盖 Ceiling、余量或任何预算 |
| −12 / −14 对比 | 显式 `--streaming-target-lufs`，基准及上下界同移；−12 为 −14～−10，−14 为 −16～−12。保留风格偏移、LRA 修正、预算与守护，**基准目标不等于整首 LUFS-I** |
| 输出 | 原工具聆听第一遍 → 新链读档 → 第二遍成品；补齐延迟与余振；`--no-align --no-replay`；编码前不再等响或限幅。连续 GLAZE 在第二遍仍跟随，不声称冻结打印 |
| 编解码 | ffmpeg 7.1.5-0+deb13u1；`aac`（AAC-LC）256/128 k，`libopus` 160/96 k（audio、VBR on），`libmp3lame` 320/128 k；每种编码独立从同一浮点成品开始，再解码为 `pcm_f32le`，防止整数转换掩盖超限 |
| 真峰值 | BS.1770 Annex 2 的插值过采样方式，独立于插件检测器；4× 和 8× 线性相位 FIR（每相 64 taps、Kaiser β=12、unity DC），分块两边补 64 原采样点；全声道取绝对最大。使用自选长滤波器，未声称采用 ITU 示例系数或通过计量认证 |
| LUFS-I | pyloudnorm 0.2.0 的 DeMan K 加权；400 ms、75% 重叠、−70 LUFS 绝对门及 −10 LU 相对门；整段含实际解码边界/尾帧，不裁编码延迟或 padding |
| PSR / PLR / 峰值因数 | PSR：每 100 ms 同步尾随 3 s 窗 TP8 − 无门控 K 加权 S，S > −70，逐曲取中位；PLR：全曲 TP8 − I；峰值因数：原采样点峰值 − 双声道整体 RMS；跨曲表再取逐曲指标中位，单位 dB |
| 过冲 | 编码后 TP − 编码前 TP，同倍率配对；4×/8× 各自报告，不把 sample peak 当 TP |
| 超限次数 | 原采样点或插值点 `abs(x) > 1.0` 的**声道采样数**；插值统计排除原采样位置，4×/8× 分别计数。是浮点超满刻度风险指标，不是失真事件数，不能跨倍率直接比较 |
| 归一化 | 按题设 Spotify / YouTube −14、Apple −16 建立逐曲纯增益模型；gain = min(0, 平台目标 − 解码 LUFS-I)，回放 TP = 解码 TP8 + gain。只衰减；不是上传平台后的实测，不模拟专辑、音量选项、上推或平台限幅 |

[BS.1770-5](https://www.itu.int/rec/R-REC-BS.1770-5-202311-I/en) 是计量依据。[Spotify 官方说明](https://support.spotify.com/us/artists/article/loudness-normalization/) 确认 Normal 为 −14，响于 −14 时建议 TP 低于 −2；不同设备和档位有差异。[Apple 官方](https://support.apple.com/en-us/109331) 说明 Sound Check 调整曲间响度，但该页没有给出固定 −16 数值；Apple −16 与 YouTube −14 在此仅为题设模型。

## 数值结果

**未编码的 39 个成品全部低于 −2 dBTP（独立 8× 最坏 −2.29），原采样点超限为 0；编码后共有 590 个原采样点、4250 个 8× 样本间点超满刻度。** MP3 320k 在全部 39 个条件中满足编码后 ≤−1；AAC、Opus 及 MP3 128k 均有失败。

### 各编码最坏过冲（13 个案例 × 3 个目标，4×/8× 分别取最大）

| 编码器 | kbps | 4× 过冲 dB | 8× 过冲 dB | 8× 最坏曲 / 目标 | 最高编码 TP8 dBTP | ≤−1 通过数 | 采样超限数 | 8× 样本间超限数 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| aac | 256 | 6.54 | 6.57 | melodic_647576 / -10 | 4.23 | 21/39 | 36 | 258 |
| aac | 128 | 7.35 | 7.38 | melodic_647576 / -10 | 5.03 | 10/39 | 453 | 3296 |
| libopus | 160 | 2.22 | 2.22 | melodic_241941 / -12 | -0.13 | 28/39 | 0 | 0 |
| libopus | 96 | 4.56 | 4.56 | melodic_241941 / -14 | 2.21 | 13/39 | 83 | 589 |
| libmp3lame | 320 | 1.15 | 1.15 | melodic_241941 / -10 | -1.20 | 39/39 | 0 | 0 |
| libmp3lame | 128 | 3.11 | 3.15 | melodic_241941 / -10 | 0.80 | 22/39 | 18 | 107 |

最坏过冲均来自真歌；压力案例中最坏为 AAC 128k +4.43 dB（密集踩镲，−12 组）。4×/8× 编码后 TP 的最大差为 0.22 dB，不能把两种倍率读数视为完全相同。以上是本机 ffmpeg native AAC / libopus / LAME 的实测，尤其 native AAC 的稀疏高峰值**不是其他 AAC 编码器或实际平台的统一结论**；三次独立 ffmpeg EBU 复核也确认了 +1.8、+3.4、+5.0 dB 峰值。未作盲听，浮点超限不是已经发生播放器硬削波的证明。

### 真歌流媒体目标对比（9 首；各指标先逐曲测，再跨曲取中位）

| 目标 | LUFS-I | PSR 中位 | PLR 中位 | 峰值因数中位 | 最坏编码 TP | 采样超限数 | 8× 样本间超限数 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| -10 | -10.69 | 8.04 | 8.35 | 9.12 | 5.03 | 211 | 1429 |
| -12 | -10.88 | 8.39 | 8.53 | 9.53 | 4.30 | 198 | 1497 |
| -14 | -12.01 | 9.10 | 9.66 | 10.36 | 3.03 | 178 | 1300 |

### 平台归一化模型（9 首 × 6 编码，衰减与回放 TP 均取中位；另列最大 TP）

| 目标 | Spotify / YouTube 衰减 dB | 回放 TP 中位 / 最大 | Apple 衰减 dB | 回放 TP 中位 / 最大 |
| --- | --- | --- | --- | --- |
| -10 | 3.29 | -4.04 / 0.32 | 5.29 | -6.04 / -1.68 |
| -12 | 3.11 | -3.56 / 1.20 | 5.11 | -5.56 / -0.80 |
| -14 | 1.98 | -3.13 / -0.88 | 3.98 | -5.13 / -2.88 |

−10 与 −12 组在 −14 归一化模型中仍各有正峰值，归一化不能替代编码回测；−14 组模型最坏 −0.88 dBTP，虽未超满刻度，仍未达到研究方案的 −1 dBTP 目标。详表见 [RESULTS.md](RESULTS.md)；逐曲逐编码/码率的 LUFS、4×/8× TP、过冲和全部超限次数见 [codec_roundtrips.csv](codec_roundtrips.csv)，成品动态指标见 [masters.csv](masters.csv)。插件原始规划/预算读数见 [−10](render_target_-10.csv)、[−12](render_target_-12.csv)、[−14](render_target_-14.csv)；审计汇总见 [data_audit.json](data_audit.json)。


## 建议

**推荐的默认值：暂保留流媒体有效 Ceiling −2.0 dBTP / Codec Margin 0 dB 作为起点，不放宽至 −1 / −1.5，也不把它称为编码安全保证。** 本次没有重渲新的 Ceiling 网格；不建议根据 9 首已是 MP3 的歌曲及单一 native AAC 编码器，把全局默认直接改成极低的固定值。应先以每曲、每编码的回测门槛约束交付，再扩大无损语料和 AAC 编码器复核。

若要一个**覆盖本次全部六种条件的编码安全档候选**，保守推导为 **PCM TP ≤−9.0 dBTP**；各编码的下一轮验证起点如下。该候选超出当前插件范围，不是可直接设置或已验证通过的插件默认。建议须区分固定默认起点与编码后实测通过：−1 / −1.5 / −2 dBTP 都不能在本语料上直接成为编码安全保证。余量推导按 `PCM TP ≤ −1.0 − 最坏过冲 − 0.2`，只作为下一轮重渲/重编码验证起点；量化编码器不是完全线性系统，不能把推导值称为已验证的新 Ceiling。

| 编码器 | kbps | 最坏过冲（4/8× 最大） | 推导 PCM TP 上限 | 向下取 0.5 dB 起点 |
| --- | --- | --- | --- | --- |
| aac | 256 | 6.57 | -7.77 | -8.00 |
| aac | 128 | 7.38 | -8.58 | -9.00 |
| libopus | 160 | 2.22 | -3.42 | -3.50 |
| libopus | 96 | 4.56 | -5.76 | -6.00 |
| libmp3lame | 320 | 1.15 | -2.35 | -2.50 |
| libmp3lame | 128 | 3.15 | -4.35 | -4.50 |


**表中上限指独立测得的 PCM TP，不是 UI Ceiling。** 不够余量的曲目可在 Celadon 成品之后先做离线交付增益回退，再逐个编码/解码重测；直到最高 TP≤−1 dBTP，不能只用一次线性推算宣称通过。

参数范围也约束建议：用户 Ceiling 为 −3～0（`Source/ParameterRanges.h`），Codec Margin 为 0～1.5 dB，链上扣余量后封在 −4.5～0（`Source/dsp/CeladonChain.cpp`）；内部 0.35 dB 是 PCM 重建余量，不能当成 AAC/Opus/MP3 编码保证。若推导所需 PCM Ceiling 超出这些范围，应先用离线交付增益回退并重新回测，而非在本任务修改 DSP 或默认值。

### −10 LUFS 的选择

| 方案 | 本批数据 | 建议 |
| --- | --- | --- |
| 保持 −10 | I 中位 −10.69，PSR 8.04；−14 模型衰减 3.29 dB | 不作为“归一化后更响”的理由；可保留为可选热交付 |
| 只降到 −12 | I 只降低 0.18 LU，PSR 增 0.35 dB；AAC 最坏峰值仍很高 | 不足以解决问题，不建议作为唯一修正 |
| 上限 + 按曲风 PSR 下限 | −14 组较 −10：I 低 1.32 LU，PSR +1.06、PLR +1.31、峰值因数 +1.24 dB | 推荐后续方向；先验证有效上限 −14 与 PSR 中位 ≥9 dB 的试验起点 |

交付基准会被曲风/风格偏移、LRA 修正、预算和守护共同影响；−14 组实测中位仍是 −12.01，而非 −14。建议让交付上限约束**最终有效目标**，曲风偏移只在上限内选择，再用 PSR 约束可推响量；不能把“只改基准数字”当成已实现响度上限。

PSR 下限还需按曲风、主段和无损原始素材另做标定：本次全部通过 general 档渲染，每类仅 3 首，没有盲听，不能直接产出 House 子类的正式 PSR 默认值。

## 验证与复现

- `celadon_render --selftest` 全部通过，见 [render_selftest.log](render_selftest.log)。未修改现有测试，未跑约 438 条已知陈旧红灯的完整测试集。
- 独立正弦计量阳性对照通过，见 [meter_selfcheck.py](meter_selfcheck.py) 与 [meter_selfcheck.json](meter_selfcheck.json)：1 kHz 双声道 −20 dBFS 正弦的 I 与 ffmpeg EBU 表一致；12 kHz、相位 π/4 的正弦验证“原采样点未超限、样本间超限”，4×/8× 与解析峰值偏差 < 0.01 dB；11 s 长度跨越插值分块边界。分相 FFT 与原 `resample_poly` 的随机信号最大样本差 < 5×10⁻¹⁵，计量滤波器未改变。
- AAC 128k 的高峰值另以 ffmpeg `ebur128=peak=true` 复核，见 [ffmpeg_worst_aac_probe.log](ffmpeg_worst_aac_probe.log)：第一轮发现的 deep_155923 编码后 TP 为 +1.8 dB，与独立 FIR 的 +1.777 dBTP 相符。
- MARS（melodic_647576）的 AAC 128k 也经独立 ffmpeg 复核为 +5.0 dB，与脚本 +5.029 dBTP 相符，见 [ffmpeg_aac647576_probe.log](ffmpeg_aac647576_probe.log)。native AAC 的默认 coder 为 twoloop，见 [aac_encoder_options.log](aac_encoder_options.log)。
- 另一首 deep_388822 的 AAC 128k 复核为 +3.4 dB，与本表约 +3.41 dBTP 一致，见 [ffmpeg_aac388822_probe.log](ffmpeg_aac388822_probe.log)。
- 完整检查点续测验证通过：234 个已完成条件全部跳过，两个测量 CSV 字节保持一致，见 [resume_selfcheck.json](resume_selfcheck.json)。
- 用 `git show 5a21425:tools/render/RenderMain.cpp` 在同一工具链另编基线工具，对压力踩镲做默认渲染：改动前后 WAV SHA-256 完全一致；显式 −10 覆盖与不覆盖的未等响 WAV 也完全一致；−14 请求目标精确降低 4 LU、Ceiling 相同。该案例整首 I 只下降 0.10 LU，说明预算/守护限制了输出。见 [render_cli_check.py](render_cli_check.py)、[render_cli_check.json](render_cli_check.json)。

依赖和构建产物均放本工作区。机器无 C++ 编译器，sudo 不可用；从 Debian HTTPS 下载对应 g++-14 / libstdc++-14-dev 包，解包到 `.t085/toolchain/`，复用机器已有 gcc-14 和 binutils；JUCE 9.0.2 在 `.t085/JUCE/`，未修改共享仓库或系统目录。只构建 `celadon_render` 目标。常规已有依赖环境的复现命令：

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCELADON_JUCE_DIR=/path/to/JUCE
cmake --build build --target celadon_render -j 2
python -m pip install -r docs/validation/2026-10-08-T085-codec-tp/requirements.txt
python tools/render/codec_tp_check.py --render build/celadon_render \
  --input artifacts/t085/input --work artifacts/t085/run \
  --out docs/validation/2026-10-08-T085-codec-tp --jamendo-per-class 3 --synthetic
python docs/validation/2026-10-08-T085-codec-tp/meter_selfcheck.py
python docs/validation/2026-10-08-T085-codec-tp/summarize.py
```

本次为节省等待，三组渲染/测量独立执行；临时分组证据在 `artifacts/t085/evidence_-12/-14`，用 `merge_runs.py --target12 ... --target14 ...` 合并。`environment.json` 保留三组命令；常规复现直接使用上面的单次全网格命令即可，无需合并。

首次下载使用 curl；已有相同 WAV 时可省 `--jamendo-per-class`。`environment.json` 保存 ffmpeg 完整版本、Python 包版本、编码选项、渲染命令、二进制哈希。链接库包版本：libopus 1.5.2-2、libmp3lame 3.100-6+b3。音频和编码缓存保留本地；CSV 为本次事实记录，不依赖将来 Jamendo 下载内容保持不变。

提交时环境将 worktree 的 Git index 目录挂为只读，普通 `git add` 无法创建 `index.lock`。本地提交使用工作区内临时索引 `.t085/commit-index`，只更新本任务分支；原索引未写入，退出只读环境后可用 `git reset --mixed HEAD` 同步索引。该命令不改变已提交文件内容。

**本次未改任何默认值。** 所有建议只用于下一轮交付校准与产品决策。

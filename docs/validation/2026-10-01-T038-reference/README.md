# T-038 标定用参考曲（2026-10-01）

Max 2026-10-01 要求标定不用他自己的曲子、改用网上的，并选了「下 Jamendo 的 45 首」。这里只存清单，**音频不进 git**。

- **来源**：MTG-Jamendo 数据集（https://github.com/MTG/mtg-jamendo-dataset）的流派标签表 `data/autotagging_genre.tsv`；音频按曲目号从 Jamendo 的公开下载地址 `https://prod-1.storage.jamendo.com/download/track/<号>/mp32/` 逐首取。
- **许可**：Creative Commons（每首的具体许可见数据集的 audio_licenses 说明）；数据集条款限非商业研究用途。Celadon 自用、不商用，只做分析，不再分发。
- **挑选规则**（确定性，按曲目号排序）：时长 150～600 s；每位艺人只取 1 首；三类互不重复。
  - Deep House：标签含 deephouse，且不含 techno / progressive / trance；
  - Tech House：数据集没有 techhouse 标签，用同时含 house 与 techno、且不含 deephouse / progressive / trance 的代替；
  - Melodic / Progressive：同时含 house 与 progressive，且不含 deephouse / techno。
- **结果**：每类 15 首，第一轮全部可用；Deep 平均 5.6 分钟、Tech 4.7 分钟、Melodic 5.5 分钟；共 342 MB。清单 `manifest.tsv`（类别、曲目号、艺人号、时长、标签、字节数、文件名）。
- **存放**：测试服务器 109 的 `~/.cache/celadon-t038-ref/{deep,tech,melodic}/`（maxsong 用户）。T-038 第 5 步标定完成后删除。
- **局限**：独立音乐人作品，母带水平参差，不是榜单商业成品；只当「子类之间明暗、低频差异」的近似参考。

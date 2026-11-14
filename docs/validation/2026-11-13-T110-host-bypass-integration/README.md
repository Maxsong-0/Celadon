# T-110 宿主旁通时整体响度暂停积分（2026-11-13）

来源：T-105 插件体检 2-12（`docs/validation/2026-11-12-T105-plugin-review/2-features.md`），Max 2026-11-12 拍板（vault
`04-协调/2026-11-12-T-105-插件体检待Max拍板.md` 第 6 节第 11 项）：宿主旁通时 OUT 表的整体响度暂停积分，同听原声。

## 改了什么

- `CeladonChain::setParams`：`outputMeter.setIntegrationPaused (dryPath)`——出口不是母带（走干路：宿主旁通，或实时听原声）的块一律不进
  整体响度。原来的 `static constexpr bool pauseIntegrationOnHostBypass = false;` 开关与 `if constexpr` 分支删掉：政策已定，不再留开关。
- 离线导出：宿主旁通是宿主的决定，不是临时监听，离线时照样走干路、照样暂停；听原声在离线时不生效（T-095），
  所以「离线 + 只开听原声」照常积分母带，「离线 + 宿主旁通（不管听原声开没开）」暂停。
- 短期响度、真峰值、子块环、Listen 的配对照常（与听原声同一个开关，见 `LoudnessMeter.h` 头注）。音频不变，只有在线预听归一化
  在宿主旁通之后读到的整体响度不再混进干信号（预听只在实时生效）。
- 测试：`CeladonChainGuards::testHostBypassIntegrationPolicy` 改成断言冻结，三个场景（实时、离线、离线 + 界面开着听原声）；
  `GlazeChain::testGlazeZeroEqualsReferenceChain` 的参照 OUT 表要在宿主旁通的块同样暂停（`tests/GlazeTests.cpp` 一行，主会话补）。
- `ChainDigest`：`monitor-toggles` 的整体响度摘要有意改变。

## 验收

与 T-095 合在一份清单里：`docs/validation/2026-10-11-T095-offline-monitoring/DGX.md`（定向、`93af3e0` 上反证、改坏变体 `host-bypass`、
ChainDigest 对比）。还没跑。

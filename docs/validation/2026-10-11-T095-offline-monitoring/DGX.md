# T-095 + T-110 待跑验证（2026-11-13 按新基点重写；改码代理不编译、不跑测试）

沿革：T-095 原在 `origin/claude/t095-offline-bypass-listen`（`3a7a367` 起 4 个提交，最后 `3b1e0fc`），10-11 写的清单以 `3a7a367` / `5a21425` 为基线、
裸 ssh 到 DGX 跑 `spark-test.sh`。2026-11-13 T-114 把链路与测试改动搬到 `27e33d0`（main `93af3e0` + T-109）之上（分支 `t114/l1-t095-t110`），
同一分支接着做 T-110（宿主旁通时 OUT 表的整体响度暂停积分，Max 2026-11-12 定）。本清单对新基点成立，取代 10-11 那一版。
Max 2026-10-11 15:09「全按推荐」的两条照旧：放行自写用例四项修正；选 A，接受实时 B 不经 reset 直接转离线时模块的 20 ms 淡化。

## 0. 约定

- 跑在哪：测试先交 108（testbox 的 `x86-1`，DGX 上 `testbox submit --arch x86_64 …`），108 不在线或自检没过再交 DGX
  （`~/.cache/celadon-spark/bin/spark-test.sh <源码目录> test …`）。不裸 ssh 上 108 / DGX 跑测试，Mac 上不跑。
- 打包：主会话的 `mkpkg.sh <仓库> <提交> <包名>`（`git archive` + JUCE 9.0.2 modules + `run-ci.sh`），提交时
  `--memory 16G --cpu 24 --cmd 'JOBS=24 ./run-ci.sh base [celadon_tests 参数]'`。下面「包 X 跑 Y」都指这一套。
- 被测树 = 集成分支上合入本线（`t114/l1-t095-t110`）**并且补上 `tests/GlazeTests.cpp` 那一行**之后的提交（记作 `<集成>`）：
  `testGlazeZeroEqualsReferenceChain` 逐位对照「链的 OUT 表 == 一块普通的表」，T-110 之后宿主旁通的块两边口径要一起改——
  在 `referenceOut.process (r);` 之前加 `referenceOut.setIntegrationPaused (hostBypassed);`。那个文件不在本线写域，由主会话补；
  不补时 `GlazeChain` 的「OUT integrated」在 mode 2 宿主旁通的块上失败（预期，见 T-032 IMPL-A4-chain.md 第 135～136 行的同一试验）。
- 对照基线 = `93af3e0`（T-095 / T-110 之前的 main）。开工基线：108 两项（base、x86-64-v3）各 134978 过 / 0 败。
- 改坏变体与反证都在一次性的 detached worktree 里做成本地提交（不推送、不建远端分支），打包跑完即删 worktree。

## 1. 定向用例

包 `<集成>` 跑 `--only CeladonChainMonitoring,CeladonChainGuards,GlazeChain,ChainDigest,AdvancedMasteringChain,CeladonChain`。必须 0 败。

- `CeladonChainMonitoring` 里 T-095 的两个函数：
  - `testOfflineBypassesTemporaryMonitoring`：三种监听各一条「an offline export bypasses temporary monitoring: …」、
    「an explicit realtime host report renders bit-exactly like no report」、「realtime -> offline -> realtime with Match on …」，共 5 个场景；
  - `testOfflineMonitoringCoverage`：14 个场景——原声积分 / 估计 1、原声与差值抖动 2、显式实时 2、三种监听各两种离线重启 6、
    差值转换 1、原声经 reset 转换 1、原声不经 reset 转换 1。阳性对照确认监听、抖动、重启后的音频确实在工作。
- `CeladonChainGuards` 的 `testHostBypassIntegrationPolicy`（T-110 改写）：3 个场景「host bypass freezes the OUT integrated loudness at the
  master's value (T-110): realtime / offline export / offline export while the UI listens to the original」，每个都断言旁通 6 s 后
  整体读数与旁通前相差 < 0.01 LU、短期读数是干信号、回母带后接着母带的值、OUT 表样本计数不断。
- `GlazeChain`（补上那一行之后）：`testGlazeZeroEqualsReferenceChain` 0 败。
- `ChainDigest`：只判「输出有限」与阳性对照，0 败；摘要对比见第 4 节。

## 2. main 上的反证（用例抓得住缺陷）

做一个 `93af3e0` 的 detached worktree，只把 `<集成>` 的 `tests/ChainTests.cpp` 拷进去（不碰产品码；新用例只用公开接口，在 `93af3e0` 上能编），
本地提交后打包，跑 `--only CeladonChainMonitoring,CeladonChainGuards`。编译失败不算反证成立。预期：

- T-095：「an offline export bypasses temporary monitoring: Match on / listening to the original / listening to the difference」的
  「offline: the monitored chain renders the master bit-exactly」、「the offline span equals the master from its first sample」、
  「offline original integrates the master bit-exactly」、「no difference crossfade tail in the offline span」等离线断言失败；
  显式实时与默认状态逐位相同那几条（回归对照）允许仍通过。
- T-110：「realtime」「offline export」两个「host bypass freezes …」场景的「host bypass must not pull the integrated reading」与
  「back on the master …」失败（`93af3e0` 宿主旁通时照旧积分，整体读数被 −6 dB 的干信号拉低约 2 LU）；「positive control」
  「short-term shows the dry signal」通过。第三个场景「offline export while the UI listens to the original」在 `93af3e0` 上**通过**：
  旧码听原声连离线时也暂停积分，把宿主旁通的影响盖住了，这个场景只在 T-095 之后才分得出 T-110（由下面的 `host-bypass` 变体验）。
  （2026-11-13 实跑更正：本条原写「三个场景都失败」。）

## 3. 改坏变体（每项一个 detached worktree，从 `<集成>` 起，只改 `Source/dsp/CeladonChain.cpp`）

用下面的脚本做替换（原文必须恰好出现一次，否则停下），本地提交、打包，跑 `--only CeladonChainMonitoring,CeladonChainGuards,GlazeChain`。
不能只看非零退出码，逐条在日志里找期望失败的断言；编译失败不算改坏成功。

```bash
# 在 <worktree> 里执行；$1 = 变体名
python3 - "$1" <<'PY'
import pathlib, sys
variants = {
    'step': ('listenGain.setTargetValue (juce::Decibels::decibelsToGain (db));',
             'listenGain.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (db));'),
    'difference': ('        if (offline)\n'
                   '            differenceMix.setCurrentAndTargetValue (0.0f);   // 离线：差值的交叉淡化也立即落回母带，不留斜坡尾巴（T-095）\n',
                   ''),
    'integration': ('outputMeter.setIntegrationPaused (dryPath);',
                    'outputMeter.setIntegrationPaused (hostBypassed || compareOriginal);'),
    'host-bypass': ('outputMeter.setIntegrationPaused (dryPath);',
                    'outputMeter.setIntegrationPaused (monitorOriginal);'),
}
old, new = variants[sys.argv[1]]
cpp = pathlib.Path('Source/dsp/CeladonChain.cpp')
text = cpp.read_text()
assert text.count(old) == 1, (sys.argv[1], text.count(old))
cpp.write_text(text.replace(old, new, 1))
PY
```

- `step`（T-095：回实时后等响要走 20 ms 斜坡）：「returning to realtime ramps the monitor gain」必须失败（切换前 50 ms 的窗口捕获那一对样本）。
  别的 Listen / 预听斜坡用例跟着失败是预期。
- `difference`（T-095：离线时差值交叉淡化立即落回母带）：「no difference crossfade tail in the offline span」必须失败。
- `integration`（T-095：离线时听原声不暂停积分）：「offline original integrates the master bit-exactly」必须失败；
  它的「positive control: the master has integrated audio」必须通过；T-110 的三个「host bypass freezes …」场景必须仍然全过
  （第三个场景宿主旁通与听原声同时开，`hostBypassed` 照样让它暂停）；`GlazeChain` 0 败。
- `host-bypass`（T-110：宿主旁通时暂停积分）：三个「host bypass freezes …」场景的「host bypass must not pull the integrated reading」
  必须失败；`GlazeChain` 的「OUT integrated」必须失败（参照表已按宿主旁通暂停）；T-095 的「offline original integrates …」必须仍然通过。

## 4. 全套 + ChainDigest 对比

改坏与反证做完后，`<集成>` 与 `93af3e0` 各打一个包：

- 包 `<集成>` 跑 `base` 与 `x86-64-v3` 两项全套：0 败，过数比开工基线 134978 多（新增断言：T-095 两个函数、T-110 三个场景），
  不得有相对 `93af3e0` 新增的失败。
- 两个包各跑一次 `--only ChainDigest`（同一台机器、同一套选项），各自日志里 `grep 'digest '`，两份 `diff -u`。预期恰好两处不同：
  - `preview-listen-exit` 的 **audio**（T-095：3.2 ～ 3.6 s 离线段等响不生效、监听增益立即归一，回实时后 20 ms 斜坡回到衰减）；
  - `monitor-toggles` 的 **integrated**（T-110：1.0 ～ 1.5 s 宿主旁通期间 OUT 表暂停积分）。
  两者的其余摘要（readings，及另一项）与其余五个场景的三项摘要全部逐字相同。日志里必须真的有七个场景的 `digest` 行，缺了就补跑，
  不拿没生成的摘要判通过。`baselineDigests` 的数值不动（它是 T-032 改前值，只作提示）；`LegacyFixtureTests.cpp` 头注已写明这两处有意改变。

## 5. 不在本清单

界面文字（`Source/ui/**`）与官网四页（`site/**`）的离线旁通说明、T-110 的文字归 L3；官网构建由那条线的清单验。

上述命令都还没跑；不得把这里的预期写成「通过」。跑完把源码 SHA、testbox 任务号、失败断言原文与摘要 diff 记进 T-114 的验证报告。

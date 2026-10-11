# T-095 待跑验证（本机未编译、未测试）

Max 2026-10-11 15:09「全按推荐」：放行自写用例四项修正；选 A，接受实时 B 不经 reset 直接转离线的模块 20 ms 淡化。新增覆盖全部在 `testOfflineMonitoringCoverage`，其它既有测试函数不变；LegacyFixture 的 baselineDigests 数值不动。

下面命令在 DGX 执行，源码先以本地归档/rsync 同步，不需推送。`src/t095-offline-listen` 必须为本次本地提交完整源码；`src/t095-main` 必须为选定 main 的完整源码，并记录其 SHA。交接旧基线为 `3a7a367`，本次工作区 main 为 `5a2142572df76ef30bcd66786d68011cba1ffd7e`；比较必须用同一 main 快照，不混用旧日志。各副本使用新的目录，命令拒绝覆盖已有副本。

```bash
ssh -o HostKeyAlias=max-spark maxsong@192.168.109.100
export T095_ROOT="$HOME/.cache/celadon-spark"
export T095_SRC="$T095_ROOT/src/t095-offline-listen"
export T095_MAIN="$T095_ROOT/src/t095-main"
export T095_LOG="$T095_ROOT/logs/t095/2026-10-11"
export T095_SPARK="$T095_ROOT/bin/spark-test.sh"
mkdir -p "$T095_LOG"
set -o pipefail
```

## 定向用例

```bash
"$T095_SPARK" "$T095_SRC" build celadon_tests 2>&1 | tee "$T095_LOG/target-build.log"
"$T095_SPARK" "$T095_SRC" test --only CeladonChainMonitoring 2>&1 | tee "$T095_LOG/target.log"
```

必须全部通过。新函数有 14 个 beginTest 场景：原声积分/估计 1、原声与差值抖动 2、显式实时 2、三种监听各两种离线重启 6、差值转换 1、原声经 reset 转换 1、原声不经 reset 转换 1（合计 14）。阳性对照确认监听、抖动、重启后的音频确实在工作。

## main 代码上的反证

在 main 副本只覆盖本分支 ChainTests（包含修正和新增用例），不覆盖产品码；此副本不作为全套 main 基线。

```bash
python3 - <<'PY'
import os, pathlib, shutil
root = pathlib.Path(os.environ['T095_ROOT'])
dest = root / 'src/t095-before-20261011'
shutil.copytree(os.environ['T095_MAIN'], dest, ignore=shutil.ignore_patterns('.git', 'build', 'node_modules', 'dist'))
shutil.copy2(pathlib.Path(os.environ['T095_SRC']) / 'tests/ChainTests.cpp', dest / 'tests/ChainTests.cpp')
PY
"$T095_SPARK" "$T095_ROOT/src/t095-before-20261011" build celadon_tests 2>&1 | tee "$T095_LOG/main-counter-build.log"
"$T095_SPARK" "$T095_ROOT/src/t095-before-20261011" test --only CeladonChainMonitoring 2>&1 | tee "$T095_LOG/main-counter.log"
```

预期离线旁通相关断言失败；编译失败不算反证成立。实时等价等回归对照允许仍通过。

## 三项改坏代码验证

每项独立副本、只改 CeladonChain.cpp；正式源码不动。以下 Python 仅在 DGX 执行：

```bash
python3 - <<'PY'
import os, pathlib, shutil, subprocess
root = pathlib.Path(os.environ['T095_ROOT'])
src = pathlib.Path(os.environ['T095_SRC'])
logs = pathlib.Path(os.environ['T095_LOG'])
variants = {
    'step': ('listenGain.setTargetValue (juce::Decibels::decibelsToGain (db));',
             'listenGain.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (db));'),
    'difference': ('        if (offline)\n            differenceMix.setCurrentAndTargetValue (0.0f);', ''),
    'integration': ('outputMeter.setIntegrationPaused (monitorOriginal ||',
                    'outputMeter.setIntegrationPaused (compareOriginal ||'),
}
for name, (old, new) in variants.items():
    dest = root / ('src/t095-mut-' + name + '-20261011')
    shutil.copytree(src, dest, ignore=shutil.ignore_patterns('.git', 'build', 'node_modules', 'dist'))
    cpp = dest / 'Source/dsp/CeladonChain.cpp'
    text = cpp.read_text()
    assert text.count(old) == 1, (name, text.count(old))
    cpp.write_text(text.replace(old, new, 1))
    with (logs / ('mut-' + name + '-build.log')).open('w') as log:
        result = subprocess.run([os.environ['T095_SPARK'], str(dest), 'build', 'celadon_tests'], stdout=log, stderr=subprocess.STDOUT)
    if result.returncode:
        raise SystemExit('build failed: ' + name + ' (not a successful mutation test)')
    with (logs / ('mut-' + name + '.log')).open('w') as log:
        result = subprocess.run([os.environ['T095_SPARK'], str(dest), 'test', '--only', 'CeladonChainMonitoring'], stdout=log, stderr=subprocess.STDOUT)
    print(name, 'test exit:', result.returncode, 'inspect the expected assertion in its log')
PY
```

逐条查日志，不能仅凭非零退出码判定成功：

- `step`：`returning to realtime ramps the monitor gain` 必须失败（切换前 50 ms 的窗口捕获那一对样本）。
- `difference`：`no difference crossfade tail in the offline span` 必须失败。
- `integration`：`offline original integrates the master bit-exactly` 必须失败；参照表阳性断言必须通过。

## 最后全套 + ChainDigest 对比

先完成上面定向与反证，再各跑一次全套，使用未被改坏且没有覆盖新 ChainTests 的 main 树。

```bash
"$T095_SPARK" "$T095_MAIN" build celadon_tests 2>&1 | tee "$T095_LOG/main-full-build.log"
"$T095_SPARK" "$T095_MAIN" test 2>&1 | tee "$T095_LOG/main-full.log"
"$T095_SPARK" "$T095_SRC" test 2>&1 | tee "$T095_LOG/branch-full.log"
grep 'digest ' "$T095_LOG/main-full.log" > "$T095_LOG/main-digests.txt"
grep 'digest ' "$T095_LOG/branch-full.log" > "$T095_LOG/branch-digests.txt"
diff -u "$T095_LOG/main-digests.txt" "$T095_LOG/branch-digests.txt"
```

预期没有相对同次 main 的新增失败；旧交接的 134603 过 / 5 败仅作历史参考。ChainDigest 只有 `preview-listen-exit` 不同，baseline 提示文字与预期音频摘要改变一起核对。全套日志必须实际包含完整 ChainDigest 场景；若缺失，补跑 `test --only ChainDigest` 后再比摘要，不用未生成的摘要判定通过。

官网四页的补充验收也只在 DGX：

```bash
"$T095_SPARK" "$T095_SRC" run bash -c 'cd site && pnpm build' 2>&1 | tee "$T095_LOG/site-build.log"
```

保存源码 SHA、构建日志、失败断言和摘要 diff。上述命令清单尚未执行；不得把待跑预期写为通过。

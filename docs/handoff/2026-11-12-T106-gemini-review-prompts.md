# T-106 给 Gemini 的复查提示词（2026-11-12）

> 用途：Max 自己拿去给 Gemini 跑一轮只读复查。结果只当第二意见：拿回来先由主会话逐条核对「文件:行号」，再定改不改。
> 材料：一份文本材料包 `t106-review-pack-v2.txt`（主会话生成、发给 Max；不进仓库）。里面没有密钥；代码注释里有「Max」这个名字和日期。
> 第一份 `t106-review-pack.txt`（20:3x 发的，对应提交 `7dc1526`）比 v2 少一处修正和几条测试，差别见文末「两份材料包的差别」。
> 分支 `claude/t106-gtrim-error-basis`，对照提交 `3a16132`。计划、决定见 vault `04-协调/2026-11-12-T-106-G_trim换误差口径计划.md`、`03-记忆/ADR-038-G_trim换误差口径.md`。

## 怎么用

1. 新开一个 Gemini 对话，上传 `t106-review-pack-v2.txt`，发「提示词 1」。
2. 它答完后，在同一个对话里依次发「提示词 2」「提示词 3」。
3. 「提示词 4」放最后发：里面有 muse、Codex 两家已经提过的问题和处理，先发会让它先入为主。
4. 把四段回答原样贴回主会话（或存成文件）。

提示词写的是英文，要求它用简体中文回答。

## 材料包里有什么

| 部分 | 内容 |
|---|---|
| PART 1 | 产品代码的差异（`Source/dsp/OutputLevelTracker.h/.cpp`、`Source/dsp/GlazeController.h/.cpp`、`CMakeLists.txt`，对照 `3a16132`）|
| PART 2 | 这四个源文件改后的全文，带行号 |
| PART 3 | 没改、但要对着看的代码（带行号）：链怎么按宿主块、按片调编排器（`CeladonChain.cpp`）、限幅器的增益平滑（`LimiterModule.cpp`）、段落账的三种读数、主段度函数的声明、方案净增益的定义、存档读写器的约定、改前的 `OutputLevelTracker.h/.cpp` 全文（0.1.8.10 就是这样读写存档的）|
| PART 4 | 测试：4.1 新文件 `tests/OutputLevelTrackerTests.cpp` 全文；4.2 环路测试用的夹具；4.3 `tests/GlazeControllerTests.cpp` 里随行为改写的现有用例的差异；4.4 文件末尾新加的 `GlazeControllerTrimLoop` 全文；4.5 `tests/GlazeTests.cpp` 的差异 |
| PART 5 | 意图说明：ADR-038 全文、规格第 4 节第 3 条和它的修订注 |

材料包由 `make_review_pack.py` 从工作树和 vault 现读现拼（脚本在会话草稿目录，不进仓库）；代码再改就重新生成，别拿旧包去审。v2 对应的四个源文件 sha256 前 12 位：`GlazeController.cpp b053c7402a6d`、`GlazeController.h 37776d696d76`、`OutputLevelTracker.cpp 445bffe75049`、`OutputLevelTracker.h 20f010bff79f`（第一份的前三个是 `568315410ce5`、`bfa31493ee52`、`250140f55ddd`，最后一个没变）。材料包第 2 行写着它对应的提交号。

## 提示词 1：独立找问题

```text
You are doing a read-only code review of one change ("T-106") in an audio mastering plugin (C++20, JUCE). The attached text file (the T-106 review pack) holds everything; its first lines list the parts. Code comments and the design notes are in Chinese; identifiers are English. Please answer in Simplified Chinese and keep identifiers and file paths as they are.

Review what T-106 introduced or changed (PART 1 is the diff against base commit 3a16132; PART 2 has the full new files with line numbers; PART 3 is unchanged context). Read the code first and form your own view; read PART 5 (design notes) last and treat it as claims to check, not as facts.

Background. "G_trim" is a slow output-side trim added to the limiter's input gain: Limit Gain = manual + automatic increment + G_trim + guard relief. It integrates an error at no more than 0.05 dB/s and is clamped to +/-3 dB. Before T-106 the error was "plan target loudness - output main-section level", where the output level is an energy average over up to 32 stored segments (at most 8 s each). Each segment had been recorded at whatever gain was in force then, so the integrator could not see its own action: it ran to +3 dB and later slid to -3 dB.

What T-106 does (OutputLevelTracker = the sensor, GlazeController::Impl = the loop):
1. Error = planned net gain - measured net gain. Measured net gain = output main level - input main level over the same segments with the same weights (OutputLevelTracker::evidenceAt). The tracker measures K-weighted input energy itself (processInput) and books it with the output samples of the same slice.
2. Each segment also stores its output energy referred back to 0 dB Limit Gain. The reading is that level plus a reference gain = manual + the automatic Limit Gain increment the current host block executed + the live G_trim, excluding guard relief (trimReferenceGainDb), so a step of G_trim shows up in the reading immediately.
3. A segment counts fully if its recording gain is within 1 dB of the reference, not at all beyond 3 dB, linearly in between; with less than 3 s of weighted evidence G_trim does not integrate. Segments whose input was not paired sample for sample, or that have no output energy, are not evidence.
4. "Plan in place" gate: a host block whose applied automatic increments had not reached the target plan is not evidence and G_trim does not integrate on it. The flag, that plan's net gain and the executed Limit Gain increment are captured once per host block in beginBlock (noteExecutedPlan) and reused by every slice's endBlock.
5. Closed segments are re-judged every block against the live reference (rejudge); only segments that still count as main are read.
6. Integrate only while the tracker's open segment is main; anti-windup (usableTrimRange).
7. State: the OUTPUT_LEVEL XML node gains 7 attributes; the 8 old ones are written unchanged.

Invariants. A violation of any of these is a finding:
I1. With the GLAZE parameter at 0 the audio output is bit-identical to the base commit.
I2. Nothing on the audio thread allocates, locks, or does unbounded work.
I3. G_trim moves at most 0.05 dB/s, stays within +/-3 dB, and does not move while any guard has relief, on a block where a relief changed, or within 1 s of a parameter change (these freeze rules are unchanged from base).
I4. State compatibility: a node written by the new code is still readable by the previous release (it reads the 8 old attributes by name; "closed" is 32 x 24 bytes). The new code reading a node without the 7 new attributes accepts it, clears the segment history and restarts G_trim from 0. A node that has the new attributes but any missing or invalid value is rejected as a whole and leaves the object untouched.
I5. No NaN or infinity can reach gTrim or the gain handed to the limiter.
I6. A host block may be processed as several slices (CeladonChain::process): beginBlock runs once per host block, endBlock once per slice, and every slice executes the settings computed in beginBlock.

Look for: logic errors; sign or unit mistakes (dB versus power, energy versus mean square); off-by-one errors in the segment ring and in the 8 s roll-over inside a slice; mistakes in the weighting math (division by zero, log of zero); stale state after restore, prepare, host reset, bypass, or while the host is stopped; slice-versus-block inconsistencies; holes in the validation of restored state; numerical edge cases (zero energy, non-finite samples, huge sample counts, a different sample rate between save and restore); control-loop problems (any path where G_trim acts but its sensor cannot see it, wind-up, a limit cycle with the guards or with the plan slewing); and comments that contradict the code.

For every finding give: severity (blocker / should-fix / nit); file:line using the line numbers printed in PART 2 or PART 4; the concrete sequence of inputs or state that goes wrong; why the existing gates do not prevent it; the smallest fix; and whether it was introduced by T-106 or already existed in the base (use PART 1 and the base files in PART 3 to tell). Do not report style or naming, and do not propose new features. If you are unsure, say what you would need to check. Finish with a list of the areas you covered and found clean, one line each. You cannot build or run anything; say so wherever a claim would need a run to confirm.
```

## 提示词 2：让它自己核实一遍

```text
Now try to refute your own findings. For each one, re-open the cited lines and trace one concrete call sequence with numbers through the real order of operations: beginBlock (applied increments -> effective settings, applyLimitExtras, noteExecutedPlan) -> analyzeInput (OutputLevelTracker::processInput) -> the modules process the slice -> endBlock (noteMainness, rejudge, processOutput, replan if due, the three guards, slewApplied, freeze checks, the integration step). Use block size 480 and block size 4800 at 48 kHz where slicing matters.

Answer with a table: finding, verdict (CONFIRMED / RETRACTED / UNCERTAIN), and one sentence of evidence with file:line. For UNCERTAIN say exactly which run or measurement would settle it.

Then list the three risks you could not rule out by reading alone, and for each the smallest automated test that would settle it (what to feed, what to assert). Answer in Simplified Chinese.
```

## 提示词 3：测试和验收判据

```text
Now review the tests in PART 4. Answer in Simplified Chinese.

(a) Coverage of the new conditions. List every condition T-106 added to the production code (each gate, each validation check, each weighting rule). For each one say which test would fail if the condition were removed, inverted, or moved by one step; name the test and the assertion. List the conditions for which you find no such test.

(b) Existing tests that were modified because the behaviour changed. For each of these say whether the modification weakens what the test used to prove, and whether a stricter criterion that cannot be gamed exists:
- GlazeControllerGuardFamily 1a and 1b: the warm-up before the checked stretch was lengthened (2 s -> 200 s, 6 s -> 80 s) because G_trim now waits for the plan to be in place.
- GlazeControllerGuardFamily "legacy LIVE": reading an old-format node now asserts that G_trim restarts from 0 and the output history is empty.
- GlazeControllerGuardScenarios PSR closed loop: "bit-identical is not a coincidence" is now shown with a control run that has the PSR guard switched off.
- GlazeControllerGuardScenarios contrast scenario (two rounds, 12 sections of breakdown / drop). The original acceptance, decided by the product owner, was "the LAST breakdown -> drop pair loses no more than 1.5 LU". It was replaced by "the pairs that start after the guard's first judgement lose no more than 1.5 LU ON AVERAGE (signed mean, at least 3 pairs)", and the last pair is only logged. Measured per-pair losses for the pairs after the first judgement, same machine: before the change, round 1: 2.20 / 1.10 / 1.65 / 1.35, round 2: 2.12 / 2.24 / 1.53 / 0.81; after the change, round 1: 2.22 / 0.94 / 0.67 / 1.58, round 2: 3.77 / -0.11 / -0.45 / -0.07. So under the original criterion round 1 now fails (1.58 > 1.5). The author's argument: the guard acts on its own lagging histogram reading, its relief swings, and where the song ends within that swing is phase; before the change G_trim was pinned at the +3 dB clamp and the guard stayed deep, which is why the last pair happened to be low. Is a signed mean defensible as an acceptance criterion here? Does it remove the original acceptance? What would you assert instead, and would that criterion have passed before the change and pass after it, given the numbers above?
- GlazeChain "loudness rises with GLAZE": expected applied gains changed (2.5 -> 2.0 dB at 10 s, 10 -> 8 dB at 40 s) and G_trim is asserted to be 0 during the ramp.

(c) Design trade-offs that were flagged to the product owner. Give your view on each, staying within the fixed constraints (0.05 dB/s, +/-3 dB, the freeze rules):
- Because G_trim no longer integrates while the automatic increments are still ramping to the plan, clips shorter than a minute come out 0.5-1.4 LU quieter than in the previous release (songs over a minute change by less than 0.12 LU on average). Is waiting the right call? Is there a way to keep the loop honest without this cost?
- The "in place" tolerances (1 unit for the 0-100 controls, 0.1 dB for Tone, the gain increment inside its 0.1 / 0.3 dB hysteresis dead band) and the evidence weighting (full within 1 dB, none beyond 3 dB, at least 3 s).
- Known approximations left as they are: the recorded Limit Gain is the limiter's target value (the limiter smooths it over 20 ms and has look-ahead latency); input and output energies are not aligned by the chain latency; inside the gain dead band the remaining difference to the plan (up to 0.3 dB) is slowly made up by G_trim.
```

## 提示词 4：对照前两家的结论（放最后发）

```text
Two other reviewers (call them R1 and R2) reviewed an earlier state of this change. Below is what they raised and how each item was handled; the pack you have already contains the handled state. For each item check the handling against the code: is the fix correct and complete, is anything half-done, and do you disagree with any item that was not changed? Answer in Simplified Chinese with a table: item, your verdict (handled correctly / incomplete / disagree), evidence with file:line. Then add anything R1 and R2 both missed that you now see.

R1-1 (medium) planInPlace() was evaluated in endBlock against live state, but every slice of a host block executes the settings from beginBlock; after a replan inside the block the error used the new target. -> Fixed: beginBlock captures executedPlanInPlace and executedPlanGainDb; endBlock uses them for the evidence gate and for the error. New tests: GlazeControllerTrimLoop 12 and 13.
R1-2 (low) The recorded Limit Gain is the limiter's smoothing target, not the gain actually applied during a ramp. -> No code change. Comment corrected; kept as a documented approximation (about 0.002 dB on in-place blocks, at most about 0.02 dB for a 6 dB manual jump inside an 8 s segment).
R1-3 (low) openInputSamples / openPairedSamples were parsed with juce::String::getLargeIntValue(), which reads garbage as 0. -> Fixed: strict decimal parsing (parseCount) for all three sample counts in the new-format path, with an upper bound of 8 s x 768 kHz.
R1-4 (medium) The contrast scenario's criterion was changed from "last pair <= 1.5 LU" to a signed mean, which removes the original acceptance. -> Not changed; escalated to the product owner (see prompt 3).
R1-5 (low) The weighting test could not tell whether both energy sums used a wrong common weight. -> Fixed: the two segments now have different input levels and the test checks the weighted input level against a hand formula.
R2-A1 (medium) Segments with no usable gain information (zero or non-finite energy) were given weight 1. -> Fixed: such segments are not evidence and add no evidence seconds.
R2-A2 (medium) rejudge() does not re-judge the open segment. -> By design: the open segment is judged on every block with the current reference through noteMainness; comments now say so; a test documents it.
R2-A3 (low) Slices containing non-finite samples still counted as paired. -> Fixed: an input slice with a non-finite sample is not booked; a non-finite output sample is not counted as paired.
R2-B1 (medium) The new-format restore did not validate the Segment contents (energy, samples, mainness). -> Fixed: validSegment for the open segment and all 32 closed ones. The legacy path is unchanged (its history is discarded anyway).
R2-B2 (medium) Three decimal sample counts were parsed leniently. -> Fixed (same as R1-3).
R2-B3 (low) No static_assert on the layout of Segment / Extra. -> Added (size, offsets, trivially copyable).
R2-C1 (medium) Same as R1-1. -> Fixed.
R2-C2 (low) mainNow does not guarantee that this slice was booked. -> No code change: the comment now states what is checked (the slice's output was handed to the tracker and its open segment is main). Only a zero-channel buffer could differ, and an empty slice has dt = 0.
R2-D1..D3 (tests) The re-judge test did not cover the open segment; the broken-state tests only corrupted Extra; no test for zero-energy segments or non-finite samples. -> Tests added.
R2-D4 (low) Loop test 2 asserts status().currentMainness instead of the tracker's own flag; loop test 9's "would integrate" predicate was incomplete. -> Test 9's predicate completed. Test 2 left as it is on purpose: tests 1-5 must compile and run unmodified on the base commit, which has none of the new probe fields.
R2-E (comments) Four comments contradicted the code. -> Corrected.
S-1 (low; found by the author after R1 and R2) trimReferenceGainDb() referred the reading to the live applied Limit Gain increment. After a replan inside a host block that increment is slewed by the remaining slices, while the block's audio was recorded with the increment from beginBlock. -> Fixed: beginBlock also captures executedIncrementGainDb and the reference uses it (before the first block after a restore it falls back to the live value). New test: GlazeControllerTrimLoop 14; loop test 6 now also checks that a restore drops these captured values.
S-2 (nit; found by the author's mutation run) parseCount computed a digit as (int64) (c - '0'). juce_wchar is unsigned on aarch64 Linux and on Windows, so there a character below '0' produced a value of about 4.29e9 that the upper-bound check happened to reject: the lower-bound check `c < '0'` was dead code on those platforms (moving it by one changed nothing, so no test could guard it), while on platforms with a signed wchar_t it was the only thing rejecting such characters. -> Now (int64) c - (int64) '0'. The unmutated parser behaves the same as before on every platform; the existing malformed-count cases ("4800/" among them) now guard the lower bound on both architectures.
```

## 两份材料包的差别

| | 第一份 `t106-review-pack.txt` | `t106-review-pack-v2.txt` |
|---|---|---|
| 对应的代码 | 提交 `7dc1526` | `7dc1526` 之后的那一个提交 |
| 读数折算用的自动增量 | 此刻的（块内可能已被速率限制推过）| 这一个宿主块执行的（上面的 S-1）|
| 环路测试 | 13 条 | 14 条；第 6 条末尾多一段 |
| 跟踪器测试 | 8 条 | 8 条；坏十进制多 3 种写法 |
| 样本数解析里算数字的那一行 | `(std::int64_t) (c - '0')` | 两边先转成有符号 64 位再减（上面的 S-2；没改错的代码行为不变）|

已经拿第一份跑了也可以接着用：提示词 1 ～ 3 原样发（其中「What T-106 does」第 2、4 条说的自动增量，第一份里还是此刻的值），提示词 4 发之前删掉 S-1、S-2 两行。Gemini 要是在第一份上指出 S-1 这件事，说明它读得细，这一处 v2 里已经改了。

## 拿回结果之后

主会话逐条做四件事：核对它说的「文件:行号」；分成「要改 / 小毛病 / 改动前就有 / 不采纳（写明理由）」；该改的改完补两侧测试并重跑受影响的验证；把处理结果写进 `docs/validation/2026-11-12-T106-gtrim-error-basis/REPORT.md` 第 7 节。

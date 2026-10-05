Before porcelain is glazed, the clay is trimmed. CLAY sits after BALANCE and before COLOR: the clean-up happens ahead of every nonlinear or dynamic stage (Glue detection, Heat, Clip and the limiter all receive clean lows), while the profile still only looks at the raw signal at the chain's input and doesn't change with GLAZE.

## Signal processing

M = (L + R) / 2, S = (L − R) / 2; output L' = M' + S', R' = M' − S'.

![Three stages: low cut on L/R; Mono and Width on S only; the peak guard watches only Low cut and Mono, and Width's increment is added back after the guard](figure:clay)

| Stage | Applies to | Filter | Parameter (actual value) |
|---|---|---|---|
| ① Low cut | L, R | 4th-order Butterworth high-pass (two RBJ stages, Q = 0.5412 / 1.3066) | 0 = off, otherwise 10–60 Hz (log scale) |
| ② Low-frequency Mono | S only | A linear-phase low-pass takes the “low S” and subtracts it: S' = (1 − a)·S + a·HP(S), where HP is built from the linear-phase low-pass, so no phase rotation | Crossover 40–300 Hz (log scale); a ∈ [0, 1] |
| ③ Width | S' only | RBJ high shelf, f0 = 2 kHz, slope 1 | −3 to +3 dB (Auto only uses positive values ≤ +1.5; negative = narrowing, Manual only) |

- Precision: coefficients and state in double; coefficients are recalculated every 64 samples from the smoothed parameters. Frequency parameters are smoothed over 20 ms in the log domain; a and widthDb linearly over 20 ms.
- Shadow running: while not bypassed, all three stages keep running and the output selects / crossfades (20 ms) by actual value, so when a stage goes from 0 to non-zero its filter state is already settled — no low-frequency jump transient.
- Bit-exact pass-through: when all three actual values are 0 (and no fade is in progress), output = input.
- Mono compatibility comes for free: ② and ③ only touch S, so L + R = 2M is completely unchanged; ① only removes sub-bass below its 10–60 Hz setting.
- Mono uses linear phase (latency D = 2(L_max − 1)), and crossover changes spread their pre-warm across blocks (the spike dropped from +11 to +0.125 percentage points).

## Try before applying

Auto isn't “decide, then do”: **it's tried on this song first, and reduced or vetoed if it would raise the peaks or thin the low end.** The decision-maker proposes candidate values from the profile; a short render estimates their peak cost; anything too costly is reduced in proportion, and skipped if it reaches 0. Low cut is also tried for blend loss (how much low end cancels when the guard pulls it back — see the next section): its frequency is capped so the predicted loss stays within 0.09 dB, and it's skipped if even the lowest frequency goes over. The interface's third line reads “Tried on this song: low cut would raise peaks or thin the low end, reduced automatically”.

## Peak guard

**Low cut and Mono never make the peaks higher.** The guard covers only ① Low cut and ② Mono: within an 8 ms lookahead it compares the correlated peaks after and before these two stages, and steps aside as needed at moments that would raise a peak (taking a fast path while the g path sits at all-ones). The guard also applies to manual Low cut / Mono (the manual value isn't rewritten; it just yields a little at the peaks).

Pressure feedback extends “try before applying” to the whole song: when either of the two checks below holds for 1 s, the orchestrator steps automatic CLAY down one level (1 → 0.5 → 0.25 → 0), then waits out a 4 s cooldown and starts counting again from scratch at the new level. Manual values aren't affected.

- **Peak cost**: the same yardstick as the trial run. Over the real music, it logs the peaks of Low cut / Mono's output (without the guard) and of the dry signal every 100 ms, takes the level of each side's loudest 1% (P99), and the cost is how many dB higher the processed side sits. A cost of 0.6 dB or more counts once 10 s have been logged; 0.2–0.6 dB needs 51.2 s — with little logged, one loud drum passage could push it over the line.
- **Blend loss**: when the guard pulls Low cut back towards the dry signal, the two partly cancel where their phases differ, so the low end gets lighter. Measured as K-weighted loudness, it counts once the actual output is more than 0.15 dB quieter than it would be without the guard pulling back.

③ Width doesn't go through the guard (since 0.1.8.6): the shelf is used only to compute an increment δ = HS(S') − S', which is added back onto the guard's output (L + δ, R − δ), so Width takes effect at its nominal value — previously the guard took most of the widening back (+3 dB left only about 0.3 dB on wide pink noise). Automatic Width is still kept in check by the trial run (at most +1.5 dB); a large manual widening can raise the peaks slightly (at most about +1 dB measured at +3 dB on wide material), which the LIMIT stage after it keeps in check. At Width 0 it's bit-identical to the old version.

After being rewritten to work in 64-sample segments, the output is bit-identical and the stage is 1.3–2.0× faster (x86).

## Three rules for Auto

| Control | What Auto does |
|---|---|
| Low cut | Cuts only when the song really has inaudible infrasound (below 16 Hz) and a trial shows it won't raise peaks or thin the low end |
| Mono | **No mono-ing** (removed after listening tests on 2026-09-27); it only lights an amber warning when the lows are out of phase — mono-ing only reduces stereo bass and leaves mono playback unchanged, and it sounded unnatural |
| Width | Widens a narrow top end moderately (at most +1.5 dB) only when a trial shows it won't raise peaks, watching the correlation so it never sounds hollow |

Three-mode semantics: Auto = decision-maker + real-time protection (Mono is the exception: on Auto it never folds, it only warns), doing nothing at GLAZE 0; Off = never processes; Manual = follows your value exactly, works even at GLAZE 0, and neither automation nor protection rewrites it (the peak guard still applies: manual Low cut and Mono step aside at moments that would raise peaks, without changing your value).

## Thresholds (how we prove it doesn't make things worse)

A “same-decision replay” comparison on 22 real songs: the render tool logs, block by block, the parameters the Auto chain actually hands each module, and a second chain at GLAZE 0 replays exactly those parameters but with CLAY switched off — the two differ only by CLAY, and the peaks and loudness at the input are compared. Full run on 2026-09-28: 0 failures, all 44 positive controls ok; for one song at GLAZE 70, the input peak of the Auto chain and of the replay (CLAY fully off) differed by +0.000 dB.

## Latency

Clay = D + G: linear-phase Mono D = 2(L_max − 1) + peak-guard lookahead G = round(0.008 fs) + round(0.003 fs). At 48 kHz that's 574 + 528 = 1102; the whole chain at 48 / 96 / 192 kHz is 1384 / 2584 / 4984.

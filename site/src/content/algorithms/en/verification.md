## How it's measured

**Percentages = a single audio thread's processing time ÷ the playback time of that block of audio** — not total CPU as in Activity Monitor, and not any DAW's meter. The budget limit (the regression threshold in the automated tests) is 15% of that thread (relaxed from 10% as of 0.1.8.6: on a 4-core cloud test machine, unchanged code already read 11.4%; real hardware measures far below it, see the table below). Only Release builds may be installed (a Debug -O1 build maxing out and dropping out is exactly how that lesson was learned).

## Whole-plugin measurements (Apple M2 Pro, on mains power, Release -O3 / LTO, stereo, interface closed)

GLAZE 100; each setting first processes 45 s of material, then measures 3 passes of 8 s each; the mean and p99.9 take the minimum across passes, and the last column is the slowest block across all passes (including scheduling jitter):

| Sample rate | Buffer | Mean budget use | p99.9 | Slowest block, all passes |
|---|---|---|---|---|
| 48 kHz | 32 | 1.276% | 3.175% | 13.231% |
| 48 kHz | 64 | 1.259% | 2.484% | 10.131% |
| 48 kHz | 512 | 1.171% | 1.691% | 1.993% |
| 96 kHz | 32 | 2.557% | 7.350% | 23.625% |
| 96 kHz | 64 | 2.522% | 5.513% | 12.506% |
| 96 kHz | 512 | 2.330% | 3.548% | 6.841% |
| 192 kHz | 32 | 5.094% | 18.525% | 51.150% |
| 192 kHz | 64 | 5.031% | 12.812% | 32.962% |
| 192 kHz | 512 | 4.652% | 7.759% | 9.516% |

At 48 kHz / 64 that's about 16.8 µs per block on average. With the VST3 actually loaded in an independent test host: 48 kHz / 64 and 512 average 1.091% and 0.993%; 96 kHz 2.172% and 1.979%. These readings date from the v0.1.6.2 one-knob revision; the later 0.1.7.2 algorithm revision averaged 3.3 / 6.4 / 13.0% at 48 / 96 / 192 kHz on an x86 virtual machine, the same trend as the published 1.3 / 2.5 / 5% on the M2 Pro. The Mac matrix for the 0.1.8.5 advanced mastering controls: the worst p99.9 across all groups was 49% of the block budget, with the maximum neither removed nor attributed (background services were running at the time).

## What each stage has optimised

- Block-wise vectorised true-peak detection (NEON / SSE2): Limit at the 48 kHz / 64 default went from 2.80% to 0.22%, and the three true-peak detectors together from 1.77% to 0.12%.
- Clip and Heat share a custom block-wise half-band oversampler (within < −130 dBFS of JUCE's); idle-skipping + pre-roll, bit-identical.
- CLAY's peak guard rewritten in 64-sample segments with a fast path when idle: the stage is 1.3–2.0× faster (x86) with bit-identical output. Crossover changes spread their pre-warm: the spike went from +11 to +0.125 percentage points.
- Reference-learning FFT sliced across multiple callbacks: p99.9 on the learning path at 192k / 32 went from 168 µs to 66 µs.
- Calibration reference-condition cache: saves 0.1–0.2 percentage points per block at 192k / 32.

## Automated tests

The full `juce::UnitTest` suite, run natively on a Linux server (GCC 15) and with real MSVC 19.44 + Wine for Windows: before the 0.1.8.7 release, **134,597 assertions passed / 0 failures / 0 warnings** (identical on both; Linux also ran an extra pass with `-march=x86-64-v3`, all passing). Coverage: per-module spec acceptance, block-size independence, Lookahead changes, true peak, null, zero allocations, stereo, silence, automation smoothing, learning and save / restore, old-state compatibility, GLAZE 0 bit-exactness, CLAY thresholds, reference learning, preview and dither, hover-text coverage, and valid UTF-8 in both languages.

The interface is rendered off-screen from the real components (Linux Xvfb / Wine) to check Chinese and English, 70% scaling and the three Advanced tabs, then mouse events are sent to the controls to verify parameters and state: all 58 interaction checks pass on Windows.

## Build requirements behind the bit-exact contract

Multiplies and adds are rounded separately; the compiler may not fuse them into FMA, or the guards in HalfBand / Saturator / TruePeak hit `#error` on FMA-capable targets:

| Compiler | Requirement |
|---|---|
| Apple clang (Xcode) | Default `-ffp-contract=on` (fuses only within a single expression); no extra flags |
| GCC (server) | `-ffp-contract=off` + `CELADON_FP_CONTRACT_OFF=1` (the default `fast` once caused 369 failures) |
| MSVC | `/fp:precise`; no `/fp:contract` or `/fp:fast`; eight DSP files also hard-code `#pragma fp_contract (off)` |
| clang-cl | `/clang:-ffp-contract=off` |
| Any | No `-ffast-math` / `-Ofast` |

The x64 SSE2 fast paths are only enabled when multiply–adds aren't fused. The true-peak max of the two channels is hard-coded to a single instruction (x86 `maxss` / `maxps`), and all four FTZ / DAZ combinations are bit-identical.

Builds on different platforms naturally differ in the last few bits (arm64's TruePeak / Balance use fused multiply–add while x86 doesn't; the Mac FFT is vDSP; Windows' UCRT switches transcendental implementations by CPU), so the bit-exact contract only holds between identical builds on the same machine; cross-platform comparisons use dB / LU thresholds.

## Not verified

- Sidechain routing, automation, real-time export and level-matched listening on songs in real DAWs; true peak after an encoder (AAC / MP3); performance grids on real Windows machines and Intel Macs; older computers and many concurrent instances.
- Synthetic-signal tests can't replace listening acceptance.

This section publishes Celadon's algorithms: structure, formulas and constants. No source code, and no verdicts on how it sounds — synthetic signals can only prove direction, limits and that old safety problems haven't come back; whether it sounds good is for level-matched listening on real songs.

## Signal chain

![Input → BALANCE → CLAY → COLOR → LIMIT → output; meters at the input and output; the GLAZE orchestrator reads input features and module statistics and sets each module's actual parameters](figure:chain)

The order is fixed: **fix spectral problems first, then add density and harmonics, and handle peaks last.** The other way round, the stages fight each other — whatever the limiter has already squashed, an EQ can't lift back.

| Module | What it does | Latency (samples) |
|---|---|---|
| BALANCE | 27-band dynamic spectral balance (including Loud, not harsh and Target spectrum) | 0 |
| CLAY | Low cut / linear-phase Mono / Width + peak guard | D + G (48 kHz: 574 + 528) |
| COLOR | Tone → split-band Glue → residual Heat → Mix → Output | 64 |
| LIMIT | Gain → Clip (4×) → lookahead true-peak limiting (8× detection / 1× application) → hard clamp | 60 + Lookahead + 62 |

At 48 kHz with 2 ms Lookahead, total latency is 1384 samples ≈ 28.8 ms.

## Design principles

1. **Transparency**: with any module's parameters at zero or the module bypassed, the output nulls against the input (< −120 dBFS). With GLAZE held at 0, the whole chain is bit-identical to a reference chain without GLAZE.
2. **Latency-compensated bypass**: a bypassed module still keeps its latency (the dry signal runs through an equal-length delay line), so switching never misaligns or clicks (20 ms fade).
3. **The peak it limits is the peak it measures**: the limiter and both meters use the same 8× true-peak detector.
4. **Zero allocations, zero locks on the audio thread**: all buffers are allocated in `prepare`; the tests replace the global `operator new` to count.
5. **Bit-exact contract**: SIMD vs scalar, any block size (17 to 2048), idle-skipping vs always running, pre-warmed vs cold start — bit for bit identical. That's why the compiler may not fuse multiply–adds (GCC `-ffp-contract=off`, MSVC `/fp:precise`, no `-ffast-math`), with compile-time guards in the source.
6. **Parameter IDs never change once released**: host projects store IDs; parameters are only ever added, never removed.

## Who analyses, who executes

- **Every block (host buffer)**: read parameters → the GLAZE orchestrator produces the actual parameters → each module's `setParams` → `process`. Parameters are smoothed over 20 ms to avoid zipper noise.
- **Every hop (≈ 10 ms)**: BALANCE's FFT analysis, GLAZE's feature updates, CLAY's protection statistics (10 ms slots, so a block's average is never smeared across different moments).
- **Every 64 samples**: CLAY recalculates its filter coefficients; BALANCE does so every 1/8 hop (also 64 samples at 48 kHz); with linear interpolation in between.

## How true peak is measured

![The real waveform between samples can rise above every sample value](figure:truepeak)

8-phase × 64-tap polyphase interpolation (32 samples of latency); the block-wise vectorised versions (NEON / SSE2) are bit-identical to a per-sample reference, and all four x86 FTZ / DAZ combinations are tested. The plugin's promise: output true peak ≤ Ceiling + 0.01 dB (as measured by this meter). A 16× reference detector reads 0.1–0.66 dB higher on broadband material such as white noise, bursts and hot mixes, so an extra 0.35 dB of reconstruction headroom is reserved wherever limiting gain is needed — the worst case under an independent 16× remeasurement is Ceiling − 0.0461 dB.

A lookahead true-peak limiter with an optional Clip stage in front:

```text
× Gain → [Clip soft clipping, 4× oversampled] → lookahead limiting (8× interpolated true-peak detection, gain applied at 1×) → Ceiling hard-clamp safety → output
```

The original design — “do everything in the 4× oversampled domain, then downsample” — structurally couldn't pass the null acceptance test, so it became **detection in the oversampled domain, gain at 1×**. When it isn't limiting, gain is exactly 1.0 and the output = the delayed input, bit for bit.

## Processing (per block)

![|x| and true peak; required gain → lookahead sliding minimum → dual envelopes → two-stage moving average](figure:limiter)

1. **Gain**: multiply by the smoothed linear gain (1× domain).
2. **Clip stage**: has its own 4× oversampling (equiripple half-band FIR, integer latency of 60), applies Saturator's `SoftClip` curve in the 4× domain, then downsamples. The clipping threshold falls linearly in dB from “no clipping” (+12 dBTP) to the Ceiling as Clip goes up; at Clip = 100 the threshold = Ceiling. At Clip = 0 it fades out over 20 ms to an equal-length delayed dry path; once the fade has finished it skips the oversampling, and when reopened it pre-rolls the last 256 input samples, bit-identical to having run all along.
3. **True-peak detection**: 8-phase × 64-tap interpolation (32 samples of latency), the larger of left and right (stereo-linked), giving `peak[n]`.
4. **Required gain** `r[n] = min(1, ceiling / peak[n])`. An extra 0.35 dB of reconstruction headroom is reserved wherever limiting gain is needed (one multiply per sample).
5. **Lookahead sliding minimum**: a sliding minimum of `r` over length = Lookahead + 63 (the two-buffer van Herk algorithm, O(1) per sample; since 0.1.8.6 it replaces the earlier monotonic queue, with bit-identical output), giving `rMin[n]`. The extra 63 is a hold segment so that the 64 samples the interpolator sees around a peak all get the same gain.
6. **Release envelopes** (instant downward tracking, exponential recovery upward):
   - Manual: single stage, time constant = Release (10–1000 ms, log scale).
   - Auto (dual stage): the **fast stage** follows `rMin` directly with a 30 ms release; the **slow stage** follows `rMin` smoothed over 50 ms, with an adaptive 100–800 ms release — the deeper the average reduction over the last 500 ms (capped at 12 dB), the slower the release; with a House genre, the slow stage also has an upper limit of one beat ÷ 3, see [GLAZE · decisions and calibration](/algorithms/glaze#genre-and-delivery). `env = min(fast, slow)`.
7. **Attack shaping**: originally a rectangular moving average of length = Lookahead over `env`; since 0.1.7.2 it's a **two-stage moving average** (a triangular attack) — the attack still completes within the lookahead, the gain still arrives exactly at the peak, the corners are smooth, and constant gain still returns precisely to 1.
8. **Apply** (1× domain): the audio goes through a `Lookahead + 62`-sample delay line and is multiplied by the gain. The release envelopes are computed in double, so gain returns exactly to 1.0 after limiting.
9. **Hard-clamp safety**: `clamp(±ceiling)` (with the Ceiling aligned to the audio); in theory it never triggers, it's only there so that no sample ever exceeds the limit.
10. Meter update: this block's minimum gain in dB (GR); output true peak is measured by the output meter with the same detector.

## Short overloads don't hold the slow envelope (0.1.7.2)

When a continuous overload lasts less than about 70 ms, the slow envelope isn't allowed below the fast one for half a window after it ends, so recovery after a drum hit follows the fast release; sustained overloads are still held by the slow envelope. Verification: the tail 45 ms after a 12 ms overload sits 6.6 dB higher than the tail after a 280 ms sustained overload (−18.3 dB vs −24.9 dB).

## Period-aware release for sustained lows (v0.1.6.2)

The added detector works only on the 1× control path and watches the lows of left and right separately (keeping the evidence of pure-Side bass). Once continuous period consistency and energy stability hold, the fast release moves from 30 ms toward about four low-frequency periods, capped at 140 ms; manual Release is unaffected. Verification: normalised intermodulation for 40 / 55 / 80 Hz + 6 kHz improves by about 9.17 / 6.93 / 4.01 dB, with RMS differences of −0.058 / −0.019 / −0.002 dB; after sending the full latency tail, old and new both read −1.3500 dBTP.

## Codec margin (0.1.8.5)

Limiter Ceiling = clamp(Ceiling − margin, −4.5, 0) dBTP, with a margin of 0–1.5 dB; with Delivery set to Streaming it is then capped at −2 dBTP, whatever the GLAZE amount (see [GLAZE · decisions and calibration](/algorithms/glaze#genre-and-delivery)). With LIMIT bypassed the margin readout is 0. No post-encode AAC / MP3 peak measurement is done.

## Latency and Lookahead debouncing

`Clip oversampling 60 + Lookahead in samples + 62` (218 samples at 48 kHz with 2 ms). The new lookahead is handed to the limiter — and the latency reported — only once the parameter has been still for 0.3 s; the reported latency always equals the actual latency. At the moment the lookahead changes, the limiter restarts: silence for one latency length + a 10 ms fade-in. One drag of Lookahead now triggers 1 host restart instead of 34–37.

## Match (Listen)

It isn't “divide the Gain back out”: it uses the K-weighted energy already measured at the input and output to compensate the overall loudness increase from Balance, Color, Gain and limiting — a 1-second continuous energy window, with silence included in the accumulation; it holds off updating when transients at the edges make up too much of the window. It only attenuates; the output meter and the calibrator read the master before attenuation. On steady material the loudness difference between the final monitored audio and the input is 0.000–0.002 LU; after a sudden 6 dB change in Color Output, the 1-second difference at the end of the third second is −0.0241 LU.

## Acceptance

1. A 1 kHz sine at +12 dB, a 100 Hz square wave, a unit impulse, a 20 ms burst and inter-sample peak sequences → output true peak ≤ Ceiling + 0.01 dB.
2. Gain 0, Clip 0 and input peaks < Ceiling → difference < −120 dBFS (null).
3. Measured impulse latency = reported latency; no NaN / Inf for any input.
4. An independent 16× reference (128-tap double-precision Kaiser sinc, 48 cases): worst case Ceiling − 0.0461 dB, passing the Ceiling + 0.01 dB threshold.
5. Zero mismatches between the block-wise vectorised true-peak code and the per-sample reference (arm64 -O1 / -O3, x86 -O2 / -O3; all four x86 FTZ / DAZ combinations).

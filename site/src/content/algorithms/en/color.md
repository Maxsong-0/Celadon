A fixed chain with three amount knobs: Tone (tilt EQ) → Glue (bus compression) → Heat (saturation, 8× oversampled) → Mix (dry / wet) → Output. Fixed latency of 64 samples (Heat's dry-path delay).

## Tone

A high shelf at 8 kHz with gain +Tone and a low shelf at 100 Hz with gain −Tone, Q 0.707. One knob, one seesaw.

## Glue: low, mid and high pressed separately

![The linked sidechain envelope drives the low and high bands (each with its own attack and offset); the mid band uses the linked gain directly](figure:glue)

- Sidechain: the input through a 100 Hz high-pass (so the bass doesn't trigger pumping), with left and right linked by taking the larger peak.
- 2:1 ratio, 6 dB soft knee, 20 ms attack, automatic release (50–500 ms, adapting to the average reduction over the last 500 ms).
- The threshold is mapped from Glue: 0 → 0 dBFS (no compression), 100 → −24 dBFS. Automatic make-up: `makeup = (0 − threshold) × (1 − 1/ratio) × 0.5`.
- **Split-band pressure (0.1.7.2)**: the meter and statistics still use the original linked envelope, so the reduction reading on a steady square wave is unchanged. If the low band (below about 180 Hz, 36 ms attack) or the high band (above about 5 kHz, 8 ms attack) is lighter on its own, its gain can sit up to 3.2 dB above the linked gain, or go 0.6 dB deeper; the mid band uses the linked gain directly (since 0.1.8.6 the mid band is no longer detected separately — a separate mid detector compressed deeper than the linked one on transients, which was an artefact). When both the low and high differences are 0, it falls back to the original single-gain multiply. The detectors reuse the sidechain's single 100 Hz high-pass instead of filtering a second time.
- With Glue fully off, the low / high split stops running and only the sidechain high-pass remains; when it's turned back on, the split state starts from 0, as does the reduction envelope, faded in by the 20 ms on/off fade. At Glue 0 it's switched off completely.

Verification: at Glue 100, 90 Hz changes by +0.68 dB while a simultaneous 2.5 kHz tone changes by −2.59 dB (+0.66 / −2.92 dB before 0.1.8.6, when the mid band was detected separately); the linked reduction on a 6 kHz square wave still lands on the original static curve.

## Heat: residual colouring that keeps the low-end body

![The dry path is kept intact; the colouring branch goes 150 Hz high-pass → M/S tube stage at 8× → the residual is added back](figure:heat)

- The original passes through a fixed 64-sample delay fully intact. The colouring branch first applies a second-order 150 Hz high-pass, then adds the **residual** produced by the oversampled nonlinearity (wet − dry) back to the original, reducing the intermodulation the bass would otherwise cause in the mids and highs.
- An 8× half-band FIR oversampling chain (custom block-wise half-band filters, sharing coefficients and latency with the Clip stage), with FIR history restored when it wakes from idle; Heat 0 stays a latency-matched pass-through.
- Drive 1 → 2, asymmetry 0 → 0.04, wet path 0 → 25 % (progressive over the whole range; previously Heat 10 already meant fully wet and full setting meant drive 10). A 5 Hz first-order high-pass blocks DC.
- **Image protection**: the colouring branch is encoded as M/S. M keeps gentle asymmetric harmonics, while S uses a lighter, strictly odd-symmetric curve, before decoding and adding back. Pure Side material leaves less than −120 dBFS in Mid; swapping left and right, dual mono and mono are bit-identical.
- **High-frequency excess control**: a 4 kHz first-order complementary split analyses the added M/S residual and limits excessive colouring by the ratio of its high-frequency energy to the original's — 30 ms energy smoothing, 15 ms to settle, 250 ms to recover, with the residual's high-frequency gain going no lower than 0.35. This is a colouring budget, not a promise about output THD.
- Automatic loudness compensation: in `prepare`, a −18 dBFS sine is used to precompute a compensation factor for every Heat value into a lookup table; turning it up doesn't just make things louder. GLAZE's Heat conversion table (45 points: 5 input true-peak levels × 9 Heat amounts) is calibrated against the same DSP.

Heat before and after the change (48 kHz, 1 kHz at −0.5 dBFS, THD from the 2nd to the 12th harmonic):

| Heat | Old THD | New THD | Old fundamental change | New fundamental change |
|---|---|---|---|---|
| 10 | 15.336 % | 0.144 % | −4.276 dB | −0.043 dB |
| 50 | 33.884 % | 1.033 % | −11.380 dB | −0.330 dB |
| 100 | 39.756 % | 2.688 % | −14.789 dB | −0.935 dB |

Intermodulation sidebands for 60 Hz + 7 kHz: old +2.464 dBc → new −22.379 dBc. With the frozen old model matched on the 6 kHz harmonic ratio and 60 Hz bass added: intermodulation fell from 3.5425 % to 0.1778 % (≈ 25.99 dB).

## Mix and Output

The dry path goes through an equal-length delay, then `out = dry × (1 − mix) + wet × mix`; Output ±6 dB.

## The automatic share follows the song's sections

The automatic Heat / Glue shares slowly pull back with section density (Heat by up to 50 %, Glue by up to 35 %; reclaiming at most 10 % per second and recovering 4 %, with 2.5 % hysteresis); the manual part and the long-term loudness target are never rewritten by this path; it freezes when evidence is lacking, during silence or while bypassed.

## Acceptance

- 1 kHz at −6 dBFS with Heat 100: output true peak no more than input + 3 dB; aliasing from the `Tube` curve after the 8× domain < −80 dBFS.
- Glue 0 and Heat 0 each null; with Mix at 0, output = the delayed input.
- 40 / 60 / 100 Hz at 44.1 / 48 / 96 kHz, −12 / −0.5 dBFS, Heat 30 / 100: low-frequency fundamental change within ±0.25 dB, low-frequency distortion < 1 %.
- 333-sample blocks, a 20 ms fade-out, reopening immediately after a short off, reopening after a long off: bit-identical to running with idle-skipping disabled.

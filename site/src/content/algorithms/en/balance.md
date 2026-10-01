BALANCE's core job: watch the spectrum in real time, pull down what sticks out and lift what's buried. The yardstick is **this song's own smoothed contour** — the song's overall tilt and character stay intact, and only local peaks and dips move. Zero latency; the analysis trailing the audio by 10–40 ms is reaction lag, not reported latency.

## Analysis (once per hop)

1. 2048-point FFT (at 48 kHz and below; 4096 at 96 kHz to keep a window of about 43 ms), Hann window, hop 512 (≈ 10 ms). Left and right are averaged into one spectrum: the same gains go to both channels, so the stereo image is unaffected.
2. Grouped into **27 third-octave bands** centred from 40 Hz to 16 kHz (40, 50, 63, 80 … 12.5 k, 16 k); each band's energy is summed and converted to dB to give `L[b]`.
3. **Fast tracking** `Lfast[b]`: first-order smoothing with attack / release set by Speed — Slow 200 / 800 ms, Mid 80 / 400 ms, Fast 30 / 150 ms.
4. **Reference contour** `R[b]`: `Lfast` smoothed across frequency (±3 bands weighted [1,2,3,4,3,2,1]/16, about one octave), then slowly smoothed over time (Slow 4 s / Mid 2 s / Fast 1 s), and finally the Brighten tilt line is added (linear in log frequency, ±3 dB from 100 Hz to 10 kHz).
5. **Deviation** `D[b] = Lfast[b] − R[b]` (dB).

![Fast-tracked level, reference contour and the ±1 dB dead zone](figure:balance)

## Decision (every hop)

- ±1 dB dead zone: small deviations are left alone to avoid jitter.
- `D > 1`: `G[b] = −min((D − 1) × Tame/100, 6 × Tame/100)`.
- `D < −1`: `G[b] = +min((−D − 1) × Recover/100, 6 × Recover/100)`.
- Silence gate: bands with `L[b] < −60 dBFS` have `G[b] → 0`, so the noise floor isn't lifted.
- Rate limit: `G[b]` changes by at most 1 dB per hop, to prevent self-oscillation.
- Extra Tame force at medium-high settings: base strength t, with an extra t² applied only to current local spectral peaks; the dynamic cut is capped at 8t dB; the display range is ±8 dB.
- Brighten is independent of Recover / Tame: 3 dB per decade at full setting.

## Execution

27 peaking filters (Q ≈ 4.3, third-octave bandwidth) with gains `G[b]`, identical on both channels; coefficients are recalculated every 1/8 hop (64 samples at 48 kHz, 128 at 96 kHz) with linear interpolation in between. CPU: one 2048-point FFT + 54 biquads per 512 samples.

## The automatic (GLAZE) share is kept apart from the manual share

The manual amount follows the curve above. The automatically added part needs its own permission:

- **Automatic Recover**: only interior bands with a sustained gap that has energy on both sides may be lifted; it fades in after 150 ms and reaches full permission at 500 ms. End bands, monotonic natural roll-offs and anything under the silence gate are not filled in.
- **Automatic Tame**: needs evidence that the band keeps sticking out locally; per-band permission fades in over 180–450 ms; after an attack is detected it's held off for 60 ms; the automatically added cut is at most 2.5 dB per band.
- **Harmonic-support protection**: uses the existing FFT to recognise reliable 1:2:3 spectral-line relationships (a complete set present in at least one channel), entering over 60 ms and leaving over 300 ms, and takes back up to 75 % of the automatic Tame permission. It doesn't recognise voices or instruments.
- **One-knob baseline** (a full-strength request, then scaled by GLAZE, confidence, style limits and module budget): automatic Recover 8, automatic Tame 24.

## Upward masking transfer (0.1.7.2)

Active only when Recover and Tame are both above 0. A dominant band that Tame has already pulled down hands part of its cut, by upward spread, to **a higher neighbour that still has content and sits within 16 dB**. The two end bands and anything above 6.3 kHz don't receive this new lift; the automatic Recover permission is left as it is — an automatic share without evidence of a gap never lifts a band out of nowhere.

The lift is smoothed along hops over about 180 ms, then A-weighting brings the participating bands' net gain back near 0, capped at ±0.45 dB. When the spectrum is already flat, with no cut deeper than 0.8 dB, the target is bit-identical to before.

Verification: with a dominant tone at 508 Hz and a neighbour at 640 Hz, and Recover / Tame both on, the neighbour is pulled down less than with Tame alone and ends up above the dominant tone; Tame alone never lifts it on its own. The original thresholds for pink-noise convergence, level steps and sweeps all still hold.

## Loud, not harsh (0.1.8.5, inside BALANCE)

- It only looks at the 7 active bands centred from 2 to 8 kHz (a 1.9–8.2 kHz window); frequency weight 0.5 at 2 kHz and 1 from 2.5 to 8 kHz.
- Excess = max(0, this band − ½ × (the band 3 below + the band 2 above) − 1.5 dB), with all three bands using the 60 ms fast level: they share the same dynamics, so overall level changes (sudden jumps in volume, drum hits) cancel out and aren't mistaken for harshness. Broadband support = the smaller of the two neighbours' gate weights: isolated pure tones, or silent neighbours, are left alone.
- Target cut = min(3, 0.65 × excess) × h × support × frequency weight × (1 − 0.6 × harmonic protection) × attack permission, where h = Amount × GLAZE (Auto) or Amount (Manual).
- Attacks: when this frame's band power exceeds the previous frame's (updated every hop) by 1.7× (about +2.3 dB), a 55 ms window opens; inside it the attack permission = 1 − p (p = Transients), and any cut already applied is immediately multiplied by 1 − p too; once the window ends, that multiplier glides back to 1 with 20 ms first-order smoothing instead of snapping back.
- The envelope runs per hop: 25 ms attack, 200 ms release. At most an extra 3 dB cut per band, and together with Tame it's still capped at −8 dB.

## Target spectrum (0.1.8.5, inside BALANCE)

- Shape: presets = slope × (band number − 14) / 3 dB, pivoting at 1 kHz; Warm −1.5, Neutral −0.75, Bright 0 dB / octave. Reference = the learned relative power of the 27 bands; bands at ≤ −65 dB don't take part.
- Only active bands numbered 3–25 (80 Hz – 12.5 kHz) are used; the measurement uses each band's 60 ms fast power.
- The gate-weighted mean is subtracted from both measurement and target: only the shape is compared, the reference's overall loudness is never copied; it acts only when the participating bands' gate weights sum to ≥ 6.
- Past a 1 dB dead zone, the error is × 0.35, capped at ±2.5 dB per band, then × t × gate weight; first-order smoothing over 2 s, returning to zero within 0.15 s once t is back at 0.

Reference learning: reads only the “Reference” sidechain; collects 10 s of usable audio; frames of 2^k points (k from 13 up, 2^k ≥ sample rate / 6, at most 17); a frame whose stereo mean square is below −80 dBFS, or with almost no energy in the bands, is discarded entirely; the result = each third-octave band's power ÷ total stereo power, as 10 log10. The computation is sliced across many callbacks (16 fixed-size work units per input sample), so small blocks never stall and the analysis tail adds no latency to the main output.

## Acceptance (automated tests)

1. Recover = Tame = 0 → nulls against bypass (< −120 dBFS).
2. Pink noise with Recover / Tame at full → every |G| converges below 0.5 dB.
3. Sine sweep → no band shows periodic oscillation in its gain (sign flips between adjacent hops < 3 per second).
4. 1 kHz sine + pink noise, Tame 100 → the 1 kHz band ≤ −3 dB, neighbouring bands |G| < 1 dB.
5. A sustained 1 kHz peak in pink noise comes down by 2.503 dB while a simultaneous short 4 kHz attack changes by −0.009 dB (attack protection).
6. Brighten at 30 measured on pink noise: −1.28 dB at 100 Hz, +0.94 dB at 10 kHz; at full setting the two ends differ by 7.42 dB.

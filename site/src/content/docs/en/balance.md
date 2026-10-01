BALANCE is dynamic spectral balance: 27 third-octave bands watch the spectrum in real time, pulling down what sticks out and lifting what's buried. The reference isn't someone else's curve — it's **this song's own smoothed contour**, so the song's overall tilt and character stay intact and only local peaks and dips move. Zero latency.

## Controls

| Control | What it does | Where to start |
|---|---|---|
| **Recover** | Lifts bands that are masked by other sounds, bringing out detail and air. | Start at 20–40; above 60 it may lift the noise floor too. |
| **Tame** | Pulls down bands that stick out as harsh or boxy. | GLAZE's automatic part protects attacks and clearly harmonic tones; manually, start at 30–50. |
| **Brighten** | An extra tilt: to the right it lifts the highs and lowers the lows (brighter), to the left the opposite. | Works on its own: 20–40 to the right when dull, to the left when harsh; Recover and Tame are not needed. |
| **Speed** | How quickly Balance follows the music: Slow is steady, Mid balanced, Fast responsive but its movement may be audible. | Mid for most songs; try Fast for dense electronic music. |
| **Power button** | Turns off only Balance; it has no latency and switches click-free. | Use it to compare before and after correction. |

## Correction curve

The 27 small bars on the card show how many dB each band is being lifted (up) or pulled down (down) right now. Mostly close to the centre line, with only a few bands moving, is most natural. The display range is ±8 dB.

The violet part is GLAZE's automatic share: automatic Recover only lifts sustained gaps that have energy on both sides, fading in after 150 ms and reaching full permission at 500 ms; automatic Tame needs evidence that a band keeps sticking out locally, holds off for 60 ms when an attack is detected, and adds at most 2.5 dB per band; peaks with reliable 1:2:3 harmonic support are pulled down less.

## Two Advanced options run here too

“Loud, not harsh” (harshness control at 2–8 kHz) and Target spectrum (Warm / Neutral / Bright / Reference) reuse BALANCE's 27-band analysis and add bounded corrections on the same set of filters, with no extra latency. **When BALANCE is bypassed, they don't work either.** See [The Advanced panel](/docs/advanced).

## A few numbers

- Analysis: 2048-point FFT (4096 at 96 kHz), hop 512 (≈ 10 ms), averaged across left and right; the same gains go to both channels, so the stereo image is unaffected.
- ±1 dB dead zone; at most 1 dB of change per hop; bands below the −60 dBFS silence gate are never lifted.
- 27 peaking filters with Q ≈ 4.3; coefficients are recalculated every 1/8 hop (64 samples at 48 kHz) and interpolated in between, so there's no zipper noise.

Algorithm details: [BALANCE · dynamic spectral balance](/algorithms/balance).

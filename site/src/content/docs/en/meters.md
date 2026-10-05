## Vertical meters

| Reading | What it shows | What to aim for |
|---|---|---|
| **IN / OUT true peak** | True peak (dBTP) measured with 8× interpolation, more accurate than sample peaks; the number below is the peak hold. | Click the number to clear it. OUT should not exceed the Ceiling. |
| **GR · Limit gain reduction** | How many dB the limiter is reducing right now. | Staying above 6 dB for long means Gain is pushed too far. |
| **GLUE GR** | How many dB Color's Glue is compressing right now. | 1–3 dB is most natural. |

## Loudness panel

| Reading | What it shows | What to aim for |
|---|---|---|
| **Integrated loudness (LUFS)** | Average loudness since the last reset; streaming services normalise playback by it. | Pop −9 to −7; to keep dynamics, −14 to −11. |
| **Short-term loudness S** | Loudness of the last 3 seconds, following the sections of the song. | 2–4 LU between verse and chorus sounds natural. |
| **True-peak maximum TP** | The highest true peak since the last reset. | Should stay below the Ceiling. |
| **Peak-to-loudness ratio PLR** | The difference between the true-peak maximum and integrated loudness — how much dynamics are left. | The smaller it is, the fuller it sounds. |
| **Norm · streaming −14** | How much a platform normalising to −14 LUFS would turn your master up or down. | A strongly negative number means you've pushed it far louder than the platform will play it. |
| **Reset meters** | Clears the integrated loudness and every peak hold. | Click it before a new song or before playing from the top. |

The meters read the master **before** any listening attenuation: Match and normalisation preview don't change them.

## Spectrum analyser

- 20 Hz – 20 kHz on a log axis, −84 to −3 dB.
- OUT fill (the original, while you're listening to it), coloured by loudness: the louder a region, the brighter it gets (grey-cyan → bright mint); white line = average; thin dark cyan line = peak hold; dashed line = IN; each can be turned on or off.
- Display slope 0 / 3 / 4.5 dB/oct, pivoting at 1 kHz, 4.5 by default — pink noise is flat at 3 dB/oct, and most masters look most comfortable at around 4.5.
- While CLAY is working, it draws the low-cut frequency and, when Mono is on Manual, the mono frequency; hovering shows “dB | Hz | note + cents”.
- It taps the signal at the end of the chain. The spectrum follows your screen's refresh rate (60 frames a second on a 60 Hz display, 120 on a 120 Hz one, at most 120); attacks land in about one frame and the fall-off takes about 0.1 s, with an 8192-point window (low-frequency resolution unchanged). The readout — at the mouse, or the loudest peak when the mouse is off the panel — updates about every 0.27 s. The meters and loudness readings still refresh at 30 Hz.

## Correction curve (BALANCE card)

How many dB each of the 27 bands is being lifted (up) or pulled down (down) right now, over a ±8 dB range. Mostly close to the centre line, with only a few bands moving, is most natural. Violet = GLAZE's automatic share.

## Current target correction (Advanced → Target spectrum)

Low to high runs left to right; up is boost, down is cut, on a ±6 dB scale. This is not the reference's raw spectrum. The curve is flat when there's no target or no usable input.

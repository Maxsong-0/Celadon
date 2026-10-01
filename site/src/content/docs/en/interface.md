The interface is a fixed 1200 × 820 canvas that scales with the window (70 %–200 %). Light background, a single celadon hue; **celadon = your settings, violet = GLAZE's automatic increment right now, orange = needs attention** (true peak over the ceiling, plan limited, NaN… shown in only one place at a time).

## Top bar

- Left: the brand and “MASTERING”.
- Centre: the **status pill**, which shows only the single most important thing, in the priority order needs attention > listening to the original / listening to the difference > the GLAZE status line. For example, “B · Listening to original · level-matched 4.8 dB”.
- Right: latency readout · `中 / EN` · Settings · Help · Advanced.

## Meters and spectrum (top half)

- **IN / OUT meters**: true peak measured with 8× interpolation (dBTP); the number below is the peak hold — click it to reset. OUT shouldn't exceed the Ceiling; if it does, it turns orange.
- **GR meter**: how many dB the limiter is pulling down right now. If it stays above 6 dB for long stretches, Gain is pushed too far.
- **Spectrum analyser**: 20 Hz – 20 kHz on a log axis, −84 to −3 dB. The OUT fill (the original, while you're listening to it), white line = average, thin dark celadon line = peak hold, dashed line = IN; each can be turned on or off. Display slope 0 / 3 / 4.5 dB/oct (pivoting at 1 kHz, 4.5 by default). While CLAY is working, it draws the low-cut and mono frequencies. Hovering reads out “dB | Hz | note + cents”.
- **Loudness panel** (top right): integrated loudness, short-term (S), true-peak maximum (TP), peak-to-loudness ratio (PLR), the streaming normalisation amount (against −14) and the target loudness marker.

## Middle row

- The GLAZE heading and status label: Locked / Listening / Calibrating / Not learned / Paused / Limited / Off.
- The main knob: the crackle in the porcelain grows denser and the glaze deeper as GLAZE goes up; the value arc grows and darkens; the thin violet arc is the automatic increment right now.
- Particles on both sides: streams flow into the knob on the left and out on the right (sound enters GLAZE from the left and leaves on the right); Reduce motion turns them off.
- A row of buttons: style (Auto / Warm / Punchy / Clean) · Relearn | A / B · Match.
- Two lines of description at bottom left (replaced by control hints while you hover or drag); a colour legend at bottom right.
- Exit hints for temporary monitoring: listening to the original, listening to the difference and normalisation preview are mutually exclusive; while one is on, a clickable hint to leave it stays here. Closing the window leaves them all.

## The four cards (bottom half, in real processing order)

| No. | Card | In one line | Main controls |
|---|---|---|---|
| 01 | **BALANCE** Dynamic EQ | Bring back what's buried, pull down what sticks out | 27-band correction bars · Recover / Tame / Brighten · Speed |
| 02 | **CLAY** Stereo | Clean-up: Low cut · Mono · Width | Three rows = light + name + current value + Auto / Off / Manual; a 10 Hz – 1 kHz mini graph |
| 03 | **COLOR** Tone | A little denser, a little warmer | Tone / Glue / Heat · Mix · Output · Glue GR meter |
| 04 | **LIMIT** Limiter | Loud, without breaking up | Gain / Ceiling / Clip · Release (AUTO) · Lookahead · Limit GR meter · the headphones for Listen to difference |

Each card header shows the number, name, a one-line description and a power button (bypass; the card fades while bypassed). Bypass is latency-compensated: the latency stays the same and switching never clicks.

## Panels

- **Settings**: window size (five common sizes + a slider + a preview in proportion to your screen), Reduce motion (turns off particles, ripples and crack fade-ins) and language.
- **Help**: control hints.
- **Advanced**: three tabs — Loud, not harsh / Target spectrum / Release safety; see [The Advanced panel](/docs/advanced).

## Hover help

Appears after a 600 ms hover, in three lines: name · what it does / what you'll hear · where to start, in both Chinese and English. The tables on this site's card pages use exactly this text.

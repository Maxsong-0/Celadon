The interface is a fixed 1200 × 820 canvas that scales with the window (70%–200%). Light background, a single cyan hue; **cyan = your settings, violet = GLAZE's automatic increment right now, orange = needs attention** (true peak over the ceiling, guard relief “Limited · …”, NaN… shown in only one place at a time).

## Top bar

- Left: the brand and “MASTERING”.
- Centre: the **status pill**, which shows only the single most important thing, in the priority order needs attention > listening to the original / listening to the difference / normalisation preview > the GLAZE status line. For example, “Listening to original · level-matched 4.8 dB”.
- Right: Settings · Help · Advanced. The language switch and the latency readout are both in Settings.

## Meters and spectrum (top half)

- **IN / OUT meters**: true peak measured with 8× interpolation (dBTP); the number below is the peak hold — click it to reset. OUT shouldn't exceed the ceiling in effect (your Ceiling minus any codec margin); if it does, it turns orange.
- **GR meter**: how many dB the limiter is pulling down right now. If it stays above 6 dB for long stretches, Gain is pushed too far.
- **Spectrum analyser**: 20 Hz – 20 kHz on a log axis, −84 to −3 dB. The OUT fill (the original, while you're listening to it; coloured by loudness), white line = average, thin dark cyan line = peak hold, dashed line = IN; each can be turned on or off. Display slope 0 / 3 / 4.5 dB/oct (pivoting at 1 kHz, 4.5 by default). While CLAY is working, it draws the low-cut frequency and, when Mono is on Manual, the mono frequency. Hovering reads out “dB | Hz | note + cents”.

## Middle row

- The GLAZE heading and status label: Manual / Manual limit / Export / Limited / Warming up / Holding / Following. On either side of the heading row sit the titles of the two side panels.
- **Left side panel, “GLAZE now”**: one row per module, in processing order — the number and name, a violet output bar (how hard GLAZE is pushing that module right now) and what GLAZE is adding to each control right now (for example “Recover +20 · Tame +8”). CLAY isn't one of the parameters GLAZE drives, so it has no bar; instead it shows how many of its three controls are on Auto, and the low cut / mono / width in effect right now. A bypassed module's row fades. Below the divider is the full text of the status pill's line (never cut short); the last line reads “Confidence · whether this is the main section · Protection”, and is empty at GLAZE 0.
- **Right side panel, “Delivery”**: the title shows the target loudness (“—” at GLAZE 0). Integrated loudness in large type, with short-term, true-peak maximum and peak-to-loudness ratio beside it; below that, a −24 to −6 LUFS loudness bar with the target as a triangle above it, −14 as a dashed line below it, and the streaming normalisation amount underneath; then the last 60 seconds of short-term loudness (with a dashed target line) and the automatic increment GLAZE adds to LIMIT's Gain (violet). The small icon at top right is Reset meters. It only records while audio plays, and stands still when playback stops.
- The main knob: the crackle in the porcelain grows denser and the glaze deeper as GLAZE goes up; the value arc grows and darkens; the thin violet arc is the automatic increment right now; as it turns, a cyan comet tail trails behind the pointer. See [Controls and shortcuts](/docs/controls) for how to turn it.
- Particles: only in the channels between the side panels and the knob; streams flow into the knob on the left and out on the right (sound enters GLAZE from the left and leaves on the right); Reduce motion turns them off.
- A row of buttons: style (Auto / Warm / Punchy / Clean) | Master / Original · Match.
- The bottom row's left side is normally empty: it shows control hints while you hover or drag, and when you open an old project it shows “Old warm-up result discarded, so the sound will change” or “Genre/delivery reset: General + Standard, sound changes” in orange (one line each when both apply), for that session only. A colour legend sits at the right.
- Exit hints for temporary monitoring also appear at the bottom left: listening to the original, listening to the difference and normalisation preview are mutually exclusive; while one is on, a clickable hint to leave it stays there. Closing the window exits all of them.

## The four cards (bottom half, in real processing order)

| No. | Card | In one line | Main controls |
|---|---|---|---|
| 01 | **BALANCE** Dynamic EQ | Bring back what's buried, pull down what sticks out | 27-band correction curve (over a faint output spectrum) · Recover / Tame / Brighten · Speed |
| 02 | **CLAY** Stereo | Clean-up: Low cut · Mono · Width | Three rows = lamp + name + current value + Auto / Off / Manual; a 10 Hz – 1 kHz mini graph |
| 03 | **COLOR** Colour | A little denser, a little warmer | Tone / Glue / Heat · Mix · Output · Glue GR meter |
| 04 | **LIMIT** Limiter | Loud, without breaking up | Gain / Ceiling / Clip · Release (AUTO) · Lookahead · Limit GR meter · the headphones for Listen to difference |

Each card header shows the number, name, a one-line description and a power button (bypass; the card fades while bypassed). Bypass is latency-compensated: the latency stays the same and switching never clicks.

## Panels

- **Settings**: window size (five common sizes + a slider + a preview in proportion to your screen), Reduce motion (turns off particles, ripples and crack fade-ins, and the knobs' comet tail and spring; the dot in the top bar's status pill doesn't pulse either) and language.
- **Help**: control hints.
- **Advanced**: three tabs — Loud, not harsh / Target spectrum / Release safety; see [The Advanced panel](/docs/advanced).

## Hover help

Appears after a 600 ms hover, in three lines: name · what it does / what you'll hear · where to start, in both Chinese and English. The tables on this site's card pages use exactly this text.

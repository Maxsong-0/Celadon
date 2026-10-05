Before porcelain is glazed, the clay body is trimmed — cleaned up and evened out so the glaze (GLAZE) sits well. CLAY comes after BALANCE and before COLOR: it cuts inaudible infrasonics and DC, folds the lows to mono when you ask for it, and widens narrow mixes a little. **Low cut and Mono never make the peaks higher**: at moments that would, they step aside; Width doesn't go through the peak guard; automatic Width is kept in check by the trial run and, while playing, by a width guard that watches correlation.

## Three rows · three modes

The card has three rows, top to bottom: Low cut, Mono, Width. Each row = a lamp + the name + the current value + Auto / Off / Manual.

| Mode | Meaning |
|---|---|
| Auto | Decision-maker + real-time protection: acts only when this song really has a problem and a trial run shows it won't raise the peaks (Mono is the exception: on Auto it never folds the lows, it only lights an amber warning when they are out of phase); does nothing at GLAZE 0 |
| Off | Never processes |
| Manual | Follows your value exactly, even at GLAZE 0; neither automation nor protection rewrites it (the peak guard still applies: manual Low cut and Mono are pulled back at moments that would raise peaks while your value stays as set — on dense material with wide, uncorrelated lows a lot of the effect can be pulled back — when it is clearly being pulled back, the Low cut / Mono lamps turn amber and the tooltip says how much is left; Width does not go through the peak guard). In Manual the value can be dragged |

Lamps: lit = processing (violet = Auto, cyan = Manual); amber Mono lamp = out-of-phase lows warning (check the mix); amber Width lamp = width guard engaged; amber Low cut / Mono lamp = the peak guard is clearly pulling your setting back (Manual and Auto alike). Hover a lamp and the second line of the tooltip shows the live value, while the third line gives cost notes such as “Tried on this song: low cut would raise peaks or thin the low end, reduced automatically”.

## Controls

| Control | What it does | Where to start |
|---|---|---|
| **Low cut · Auto / Off / Manual** | Auto: cuts only when the song really has inaudible infrasound (below 16 Hz) and a trial shows it won't raise peaks or thin the low end, and backs off during playback if it keeps doing either; Off: never cuts; Manual: cuts at your frequency, even at GLAZE 0. | Usually keep Auto; choose Off to keep deliberate sub-bass effects. |
| **Low cut frequency** 10–60 Hz | A 4th-order high-pass removes infrasound and DC below this frequency; in Auto your value is not used and the display shows the actual value. | 16–25 Hz is enough; it rotates the low-end phase, and whenever that would raise a peak the peak guard yields the low cut so the limiter does not work harder; yielding too often thins the low end, so Auto lowers the frequency or skips the cut. |
| **Mono · Auto / Off / Manual** | Auto: no mono-ing, only an amber warning when the lows are out of phase (bass lost on mono playback); Off: no mono-ing, no warning; Manual: everything below your frequency becomes mono. | Mono-ing only focuses stereo bass and cannot change mono playback; for tighter lows use Manual, starting at 100–150 Hz. |
| **Mono frequency** 40–300 Hz | The side signal below this frequency becomes mono (linear phase, no phase rotation): tighter lows, mono playback completely unchanged; in Auto your value is not used (Auto never folds the lows to mono, so no mono frequency is shown here). | 80–150 Hz is common; above 200 Hz guitars and keys get narrower too. |
| **Width · Auto / Off / Manual** | Auto: widens a narrow top end moderately (up to +1.5 dB) only when a trial shows it won't raise peaks, watching the correlation so it never sounds hollow; Off: no widening; Manual: your value, negative narrows. Width is not run through the peak guard: what you set is what you get. | Songs that are already wide are left alone; to go wider, try Manual at +0.5 to +1 dB. |
| **Width amount** −3 to +3 dB | Raises the side signal above about 2 kHz for a wider image, mono playback unchanged; negative narrows; in Auto your value is not used and the display shows the actual value. | Within ±1 dB is clearly audible; above +2 dB it easily sounds hollow and phasey. |
| **Power button** | Turns off only Clay (low cut, mono, width); latency stays the same and switching is click-free. | Use it to compare with and without Clay. |

> Auto **never** applies Mono automatically (removed after listening tests on 2026-09-27: mono-ing only reduces stereo bass and leaves mono playback unchanged, and it sounded unnatural). Out-of-phase lows only trigger a warning; to apply Mono, switch to Manual.

> **With Genre set to Deep / Tech / Melodic (a House genre)**: automatic Low cut aims at 25 Hz and Auto never widens; everything else stays as above. There is also a more sensitive out-of-phase hint (it has to persist for 4 bars of main section), and while it is lit the mini graph slot shows two amber lines instead. See [Genre and Delivery](/docs/glaze#genre-and-delivery).

## Mini graph

Below the three rows is a 10 Hz – 1 kHz mini graph: the hatching below the low cut, the mono region, and the stereo region above it.

## Old projects

Old projects have no CLAY profile: the three lamps stay dark and the third line says to click Relearn to enable automatic CLAY. If all three rows are set to Off or Manual, no relearn is needed.

Algorithm details: [CLAY · stereo clean-up](/algorithms/clay).

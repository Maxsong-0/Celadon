COLOR is what makes the master denser and louder the moment it's inserted: a Tone tilt, Glue bus compression and Heat tube-style colouring. Saturation rounds off peaks and compression tightens the dynamics, so the LIMIT stage after it can push louder without breaking up. Fixed latency of 64 samples.

## Controls

| Control | What it does | Where to start |
|---|---|---|
| **Tone** −3 to +3 dB | A seesaw EQ: to the right it lifts the highs and lowers the lows (brighter), to the left the opposite (warmer). | Within ±1 dB is already clearly audible. |
| **Glue** 0–100 | A bus compressor that binds the tracks together and tightens the dynamics; a sidechain low cut reduces bass-triggered pumping. | 20–40; 1–3 dB on the Glue GR meter sounds most natural. |
| **Heat** 0–100 | Parallel tube-style colouring that keeps the low-end body; the sides get a lighter touch, and it backs off when new high harmonics get excessive. | 20–50; above 70 the colouring is obvious — compare with Match on. |
| **Mix** 0–100 % | Blends the processed and the original signal; below 100 % it becomes parallel processing that keeps the original punch. | 50–80 % gives a thick but open result. |
| **Output** −6 to +6 dB | Adjusts the level Color sends into Limit. | Make Color on and off equally loud before judging it. |
| **GLUE GR** | How many dB Color's Glue is compressing right now. | 1–3 dB is most natural. |
| **Power button** | Turns off only Color; latency stays the same and switching is click-free. | Use it to compare with and without colouring. |

## What each stage is

- **Tone**: a high shelf at 8 kHz of +Tone and a low shelf at 100 Hz of −Tone — one knob, one seesaw.
- **Glue**: feed-forward bus compression with a 100 Hz high-passed sidechain, max-linked left and right, 2:1, a 6 dB soft knee, 20 ms attack and automatic release (50–500 ms). Low (below ≈ 180 Hz), mid and high (above ≈ 5 kHz) are pressed separately — if the low or high band is lighter on its own, its gain can sit up to 3.2 dB above the linked gain, so the bass no longer ducks when the vocal comes in. The meter still reads the linked envelope. At Glue 0 it's switched off completely.
- **Heat**: the original passes through a 64-sample delay fully intact; the colouring branch is high-passed at 150 Hz first, and only the **residual** produced by the 8× oversampled tube nonlinearity is added back, so the bass doesn't drive intermodulation in the mids and highs. It's encoded as M/S: M is gently asymmetric, S lighter and odd-symmetric, so the image stays put. Loudness is compensated automatically, so turning it up doesn't just make things louder. At Heat 0 it's a latency-matched pass-through.

## With GLAZE

GLAZE drives Tone, Glue and Heat (in violet). The automatic Heat / Glue shares slowly pull back with section density (Heat by up to 50 %, Glue by up to 35 %) and then recover; the manual part is unaffected.

Algorithm details: [COLOR · tone, glue and heat](/algorithms/color).

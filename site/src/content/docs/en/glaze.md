GLAZE doesn't turn every knob up at once. It **listens to the song first and decides how to process it and how much loudness to push based on its loudness, dynamics, spectrum and attacks**; the knob only sets how far to push toward this song's ideal master.

## What the knob means

| Value | Meaning |
|---|---|
| 0 | Nothing moves: the manual values on the four cards are all there is, and the output is bit-identical to having no GLAZE at all |
| 35 (default) | Gentle: new instances start here |
| Higher | Louder, more correction, more colour; every automatic increment only grows with GLAZE and never exceeds each module's limit |
| Limited | The dynamics-protection limit has been reached; turning it further won't get louder. Lower it to keep more movement |

**Actual value = clamp(manual value + automatic increment).** The manual knobs are the floor and GLAZE adds on top; it never automatically cancels out your changes. On screen, celadon is your value and violet is the automatic increment.

GLAZE drives 7 parameters: BALANCE's Recover / Tame, COLOR's Tone / Glue / Heat, and LIMIT's Gain / Clip. CLAY's three controls and the automatic options under Advanced follow it too (see below).

## What it listens for

Once playback starts it listens automatically for 15–30 seconds: loudness (M / S / I / LRA), peak-to-loudness ratio, positive and negative spectral evidence, attack density… and builds a **profile** (with a confidence value; one per song, saved with the project). Profile + style + GLAZE + manual values → a **plan**. The plan is first applied gently at low confidence, then the **calibrator** matches the input-to-output loudness difference and finishes once it hits the target; after locking, only bounded protective adjustments around that baseline remain.

Status labels:

| Label | Meaning |
|---|---|
| Not learned | Nothing has played yet; listening starts automatically on playback |
| Listening | Gathering evidence (the corner shows “Listening x / 30 s”); the previous plan (or a gentle one) stays in effect |
| Calibrating | The plan is set and the measured loudness is being brought to the target |
| Locked | Done; only bounded protection from here on |
| Limited | A constraint keeps being exceeded, so the protection layer slowly backs off (by up to −3 dB); shown in orange |
| Paused | The host has stopped playback |

> The first profile captures the character of the passage it heard. Very quiet or very loud sections are still handled by calibration and protective back-off; for a different song, click Relearn to update the whole-song profile.

## Relearn

Listens for another 15–30 seconds to build a new plan; the old plan keeps working meanwhile, then fades over in about 3 seconds. Use it after switching songs or changing the mix; click again while it's listening to cancel. It never relearns on its own — only when you click it.

## Styles

| Style | Direction |
|---|---|
| Auto (default) | Blends the three styles per song; the status at the top shows which way it's leaning |
| Warm | Thick, round, never harsh |
| Punchy | Strong drums, louder |
| Clean | Clear, less compression, more dynamics |

The style also sets the shape of the automatic Target spectrum under Advanced: Warm → Warm, Clean → Bright, Auto / Punchy → Neutral.

## How it relates to Advanced and CLAY

- With Loud, not harsh and Target spectrum on Auto: strength = the amount you set × GLAZE / 100. At GLAZE 0 they're zero.
- CLAY's three controls on Auto are decided by the plan and do nothing at GLAZE 0; on Manual they work even at GLAZE 0; Off never processes.
- Whenever an Advanced setting changes (or a manual parameter or the sample rate does), the calibration's reference conditions change and calibration realigns.

## Saving and reopening

The profile, plan, calibration, calibration snapshot and execution snapshot are all saved with the project. Reopening doesn't relearn, and re-rendering from the same starting point is bit-for-bit reproducible. Offline export and real-time playback learn and calibrate on the same sample timeline, so switching to offline neither stops learning nor undoes calibration.

For the algorithm details: [GLAZE · decisions and calibration](/algorithms/glaze).

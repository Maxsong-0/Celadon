Advanced in the top bar opens a 680 × 440 panel with three tabs: Loud, not harsh / Target spectrum / Release safety. They're all host parameters — automatable, undoable and saved with the project; switching tabs never changes a parameter. **GLAZE remains the everyday control; open these only when you need them.**

CLAY's Low cut, Mono and Width aren't here; they're on the CLAY card in the main view.

## Loud, not harsh

Eases harsh bands at 2–8 kHz while keeping drums and transients. It runs inside BALANCE, does nothing while BALANCE is bypassed, and adds no latency.

| Control | What it does | Where to start |
|---|---|---|
| **Mode** Auto / Off / Manual | Controls harshness through BALANCE: Auto scales with GLAZE, Manual uses a fixed amount. Inactive when Off or BALANCE is bypassed. | Start with Auto; choose Off to compare this stage alone. |
| **Amount** 0–100% | Sets the depth of high-frequency harshness control; Auto also scales it with GLAZE. Inactive when Off. | Start at 50%; too much can lose air, so compare at matched loudness. |
| **Transients** 0–100% | Relaxes harshness reduction on detected attacks to preserve drums and plucks; active in both Auto and Manual. | Raise it if drums soften; lower it if the attacks themselves sound harsh. |

The panel shows “Current reduction x dB”. At full amount it cuts at most an extra 3 dB per band, and together with Tame it's still capped at −8 dB. It isn't a de-esser and doesn't recognise voices or instruments.

## Target spectrum

Gently moves the **shape** of the 27 bands toward a target: it doesn't copy the reference track's overall loudness, and doesn't aim for a frequency-by-frequency match. It runs inside BALANCE and does nothing while BALANCE is bypassed.

| Control | What it does | Where to start |
|---|---|---|
| **Mode** Off / Auto / Warm / Neutral / Bright / Reference | Auto follows GLAZE; Warm, Neutral, Bright and a learned Reference are independent manual targets. | Use a preset first; learn the Reference target from the DAW's external sidechain. |
| **Amount** 0–100% | Sets how strongly the 27 bands move toward the target; the graph shows the current target correction. Inactive with Target off. | Start gently and keep the mix's character; every band need not match exactly. |
| **Current target correction · 27 bands** | Low to high frequencies run left to right; up is boost, down is cut, on a ±6 dB scale. This is not the reference's raw spectrum. | Judge the overall sound first; a flat curve is normal without an active target or usable input. |

Preset shapes: Warm −1.5, Neutral −0.75, Bright 0 dB / octave (pivoting at 1 kHz). Auto picks the shape from the GLAZE style: Warm → Warm, Clean → Bright, Auto / Punchy → Neutral. With a House genre it follows the genre instead: Deep −1.5 (the same as Warm), Tech −0.75 (the same as Neutral), Melodic −1.25 (between the two); see [Genre and Delivery](/docs/glaze#genre-and-delivery).

### Learn reference

1. Route the reference track on its own to Celadon's external sidechain input, “Reference” (sidechain routing differs between hosts).
2. Play the reference and click **Learn reference**. It reads only the sidechain; with no audio there it keeps waiting rather than borrowing the main input, and the reference is never mixed into the master output.
3. It finishes after **10 seconds of usable, non-silent** audio (the final frame has a short analysis tail). The panel shows “Learning reference · x s of usable audio collected” → “Reference ready · select Reference to use its spectrum”.
4. Switch the mode to Reference and adjust Amount.

**Cancel learning** stops the current capture and keeps any previously completed reference. **Clear reference** removes the learned target; Reference mode applies no correction without one, and the presets are unaffected. The learned 27 bands are saved with the project.

## Release safety

| Control | What it does | Where to start |
|---|---|---|
| **Preview loudness** −24 to −8 LUFS | Chooses the listening target, using the pre-preview OUT integrated loudness; attenuation only, never boosting a quiet master. | Default −14 LUFS; this is a general listening comparison, not a platform's exact playback policy. |
| **Start / Stop normalisation preview** | Attenuates monitoring to the selected LUFS without changing the master meters or the processing plan; stops when the plugin window closes. | Mutually exclusive with original and difference listening; disabled for offline export. Stop the preview before a real-time export. |
| **Codec margin** 0–1.5 dB | Lowers the output ceiling by another 0–1.5 dB when LIMIT is enabled; inactive with LIMIT bypassed. No codec simulation or encoded-peak detection. | Leave the headroom your delivery requires, then remeasure true peaks after the actual encoding with an independent tool. |
| **Final bit-depth dither** Off / 16 bit / 24 bit | Adds TPDF dither for the final 16- or 24-bit integer export; off by default and unnecessary for floating-point intermediates. | Dither once at the end of the chain; leave this off if the DAW or a later tool handles it. |

> ⚠ Codec margin does **not** run an AAC / MP3 encode–decode or detect encoded true peaks, so it can't guarantee that every encoder stays under the peak. A real-time recording or export can't be told apart from normal playback, so stop the preview before you start.

## Old projects and defaults

When you load an old project that doesn't contain these parameters, “Loud, not harsh”, Target spectrum, codec margin and dither are all off. New instances default to “Loud, not harsh” on Auto (Amount 50, Transients 50) and Target spectrum on Auto (Amount 35): at GLAZE 35% that's at most about 0.53 dB of harshness reduction per band and about ±0.31 dB of target correction; at GLAZE 0 both are zero. For why an old project's GLAZE sound changes, see [An old project sounds different when I open it?](/docs/faq#an-old-project-sounds-different-when-i-open-it).

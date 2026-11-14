The hardest part of mastering isn't the processing — it's the judgement. Celadon treats hearing the difference clearly as a feature. The three kinds of temporary monitoring (listening to the original, listening to the difference and normalisation preview) are mutually exclusive; while one is on, the main window keeps showing a dark, clickable hint to leave it, and closing the editor exits all of them. **For an offline export they and Match are all bypassed automatically**: whatever monitoring is on, the rendered file is the master itself.

## Master / Original

Master / Original in the middle row (called “A Processed / B Original” before 0.1.8.11). Original runs through the same dry path as host bypass: latency-compensated, a 20 ms crossfade, Match measurement frozen. When you're listening to the master (the default), the output is bit-identical to having no such feature at all.

The status pill reads “Listening to original · level-matched 4.8 dB” or “not matched (x dB apart)”, with a dark dot: listening to the original is a monitoring mode, not a problem, so it isn't orange.

## Match

| | |
|---|---|
| What it does | Measures the loudness difference across the whole chain and turns the louder side down to the other (the master while you listen to Master, the original while you listen to Original), so the comparison is not biased by level; the meters show the master before this attenuation. |
| Where to start | Compare after a few seconds of playback; it only attenuates, never boosts, and automatic processing keeps running. Bypassed automatically for offline export; turn it off before a real-time export. |

It's the parameter `limit_listen` and is saved with the project. **It is bypassed automatically for an offline export**: if you forget to turn it off, the rendered master is still not attenuated. The plugin can't tell a **real-time export** (recording while playing) from normal playback, so turn it off by hand before one. It measures with a 1-second continuous energy window, K-weighted, and holds off updating when transients at the edges make up too much of the window; it's a slowly following loudness-matching aid, and can't guarantee an exact match for a short while after switching or a sudden change in the arrangement.

## Listen to difference

The headphones in the LIMIT card header. They replace the output with what LIMIT takes away (gain, clip and limiting together, at the level before the gain push, never louder than the original); meters, loudness and GLAZE keep measuring the master. It turns off when you switch to the original, bypass LIMIT or close the window, can't be turned on while LIMIT is bypassed, and has no effect on an offline export.

## Normalisation preview

Under Advanced → Release safety. It attenuates monitoring to the selected LUFS (−24 to −8, default −14), computing the attenuation from the OUT integrated loudness **before** the preview: attenuation only, never boosting, and it doesn't change the master meters, the processing plan or the GLAZE target. While it's on, the status pill reads “Normalisation preview · Target −14.0 LUFS · monitor attenuation 3.2 dB”.

- It turns off automatically for offline export; **stop it manually before a real-time export** — the plugin can't tell a real-time export from normal playback.
- It hands the current gain over to Match rather than stacking with it.
- It also turns off for host bypass, listening to the original and listening to the difference.

## Before you export

1. Before a **real-time export** (recording while playing): turn Match off and exit Normalisation preview, Original and Listen to difference (click the hint in the main window if one is showing) — the plugin can't tell a real-time export from normal playback.
2. An **offline export** needs none of that: all four are bypassed automatically and the rendered file is always the master itself.
3. Check that the OUT true-peak maximum ≤ Ceiling; set a codec margin if you need one.
4. If you're responsible for the final bit-depth reduction, turn dither on; otherwise leave it off.
5. Play through the main section once before an offline export: if it hasn't heard enough of the main section yet, the status line reads “First drop may export quiet: play it once, then export”. The main section is the loudest parts; see [How it follows the main section](/docs/glaze#how-it-follows-the-main-section).
6. If you're exporting two versions of one song (say, with a different Ceiling or GLAZE), play from the drop after each change until the “Target … LUFS” reading settles, then export.

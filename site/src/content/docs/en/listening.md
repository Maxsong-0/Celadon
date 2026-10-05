The hardest part of mastering isn't the processing — it's the judgement. Celadon treats hearing the difference clearly as a feature. The three kinds of temporary monitoring (listening to the original, listening to the difference and normalisation preview) are mutually exclusive; while one is on, the main window keeps showing a clickable hint to leave it, and closing the editor exits all of them.

## A / B

Processed / Original in the middle row. B = listening to the original, through the same dry path as host bypass: latency-compensated, a 20 ms crossfade, calibration paused, Match measurement frozen. When you're listening to A (the default), the output is bit-identical to having no such feature at all.

The status pill reads “B · Listening to original · level-matched 4.8 dB” or “not matched (x dB apart)”.

## Match

| | |
|---|---|
| What it does | Measures the loudness difference across the whole chain and turns the louder side down to the other (the processed signal in A, the original in B), so A / B is not biased by level; the meters show the master before this attenuation. |
| Where to start | Compare after a few seconds of playback; it only attenuates, never boosts, and automatic processing keeps running. Turn it off before exporting. |

It's the parameter `limit_listen` and is saved with the project — so **turn it off before you export**. It measures with a 1-second continuous energy window, K-weighted, and holds off updating when transients at the edges make up too much of the window; it's a slowly following loudness-matching aid, and can't guarantee an exact match for a short while after switching or a sudden change in the arrangement.

## Listen to difference

The headphones in the LIMIT card header. They replace the output with what LIMIT takes away (gain, clip and limiting together, at the level before the gain push, never louder than the original); meters, loudness and GLAZE keep measuring the master. It turns off when you switch to the original, bypass LIMIT or close the window, and can't be turned on while LIMIT is bypassed.

## Normalisation preview

Under Advanced → Release safety. It attenuates monitoring to the selected LUFS (−24 to −8, default −14), computing the attenuation from the OUT integrated loudness **before** the preview: attenuation only, never boosting, and it doesn't change the master meters, the processing plan or the GLAZE target.

- It turns off automatically for offline export; **stop it manually before a real-time export** — the plugin can't tell a real-time export from normal playback.
- It hands the current gain over to Match rather than stacking with it.
- It also turns off for host bypass, listening to the original and listening to the difference.

## Before you export

1. Turn Match off.
2. Exit Normalisation preview and Listen to difference (click the hint in the main window if one is showing).
3. Check that the OUT true-peak maximum ≤ Ceiling; set a codec margin if you need one.
4. If you're responsible for the final bit-depth reduction, turn dither on; otherwise leave it off.
5. With a House genre, wait for Locked before exporting (exporting while it is still listening switches the plan partway through the master; the status line warns you); see [How House genres listen](/docs/glaze#how-house-genres-listen).
6. If you're exporting one song as both a Streaming and a Club version, play from the drop after each Delivery switch until the status is back to Locked, then export; see [Switching, and exporting two masters](/docs/glaze#switching-and-exporting-two-masters).

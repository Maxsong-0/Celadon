## I switched songs — what should I do?

Nothing to click. GLAZE follows whatever is playing, the old song's statistics fade out as the new one plays, and the plan moves over gradually; until it has heard enough of the new song's main section, the status line reads Starting. Click Reset meters to clear the integrated loudness and the peak holds.

## It stays on Starting?

- Starting means it hasn't heard enough of the main section yet, so the plan stays cautious. A long intro, or looping a short stretch of intro, keeps it there longer; play through from the drop (or the chorus) once and it relaxes. A House genre recognises the main section the four-on-the-floor way; see [How House genres follow](/docs/glaze#how-house-genres-follow).
- Material that's too short or mostly silent can't gather enough of the main section and stays on Starting. If you still run into it, please send the project and the song to Max.

## An old project sounds different when I open it?

From this version GLAZE no longer listens through a passage and then settles on a plan; it follows the music all the time. The listening result, plan and calibration saved in an old project are discarded on load and GLAZE starts following from scratch, so the sound changes; the bottom-left corner says so once: “The old listening result was discarded, so the sound will change”. Play through the main section once and it settles. New projects save the statistics gathered so far and carry on from them when reopened.

## Why is everything under Advanced off in my old projects?

Compatibility: “Loud, not harsh”, Target spectrum, codec margin and dither, added in 0.1.8.5, all load as off in old projects; only new instances default to Auto. Turn them on by hand if you want them. (For why an old project's GLAZE sound changes, see the question above.)

## My host doesn't see the new version?

The host has the old binary cached. Save your project, then restart the host or rescan the plug-in folder. On macOS you may also need to clear the AU cache.

## Turning GLAZE up doesn't make it any louder?

Look at the “Target … LUFS” on the status line first: dynamics protection comes before loudness, so once each module's budget is used up GLAZE stops pushing and the target comes down; the status usually still reads “Following · … · Target … LUFS”. Two other cases: “Limited by manual settings” (label Manual limit) means your manual values have already used up a module's budget, so that module's automatic increment is withdrawn; “Limited · cause · backed off x dB” (label Limited) means a guard is pulling the automatic gain back during playback (cause: Budget / Delivery peak / Contrast / PSR; Budget and Delivery peak back off at most 3 dB, Contrast at most all of the automatic gain) and it eases back once the condition clears. Either accept it, lower GLAZE to keep more movement, or add Gain / Clip by hand — manual values are the floor, but they're still subject to each module's constraints.

## OUT true peak occasionally reads 0.0x dB above the Ceiling?

The plugin promises ≤ Ceiling + 0.01 dB as measured by its own 8× meter, plus 0.35 dB of reconstruction headroom. Other meters (a 16× reference, after resampling or lossy encoding) can read slightly higher. Set a codec margin before delivery, and remeasure with an independent tool after the actual encoding.

## The export is louder / quieter than what I heard?

Check whether Match is still on (it's saved with the project) and whether Normalisation preview is off. Offline exports turn the preview off automatically; real-time exports don't.

## How do I get both a Streaming and a Club version of one song?

Set Delivery to Streaming, then to Club. After each switch, play from the drop until the “Target … LUFS” reading settles, then export; that gives you two masters. The steps and the reasons are in [Switching, and exporting two masters](/docs/glaze#switching-and-exporting-two-masters).

## Windows warns about an unknown publisher?

The installer isn't code-signed. In SmartScreen, click More info → Run anyway.

## Intel Mac / Rosetta?

Install the Intel package; the AU passes auval on both chips. Hosts opened through Rosetta on Apple silicon also need the Intel package. Don't rebuild it yourself with a higher instruction set (Rosetta has no AVX and will crash outright). On older Intel laptops, 192 kHz with a small buffer risks dropouts.

## The interface says “Debug build”?

That's a debug build: it uses far more CPU than Release and is only for debugging. Normal distribution is Release only.

## Will CLAY ruin the stereo image or mono compatibility?

Mono and Width only touch the side channel S, so mono playback (L + R) is completely unchanged; Low cut removes only sub-bass below its 10–60 Hz setting (Auto cuts only when there's infrasound below 16 Hz). Auto never applies Mono; it only lights an amber warning when the lows are out of phase.

## How do I check CPU use?

There's no CPU meter next to Latency at the top right. For reference: on an M2 Pro with GLAZE 100 and the interface closed, about 1.3% of the audio-thread budget at 48 kHz. With the interface open there's roughly an extra quarter of a core for drawing; Reduce motion saves a little.

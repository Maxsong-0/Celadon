Day to day, Celadon takes three steps: insert it on the master bus, press play, turn GLAZE. Every other knob is the floor; GLAZE adds automatic increments on top of your settings, based on this song.

## Three steps

1. **Insert it last on the master (stereo) bus.** If you also use dither or loudness-meter plug-ins, put Celadon before them. Your host compensates the latency automatically (about 28.8 ms at 48 kHz).
2. **Play the song.** New instances default to GLAZE 35% and the Auto style. GLAZE starts following as soon as playback starts: the status at the top first reads “Starting · main section not heard yet (staying conservative)”, then “Following · Auto→… · Target … LUFS” once it has heard enough of the main section, and “Holding · this section is quieter” in lighter sections. It keeps measuring the song's loudness, dynamics, spectrum and attacks, and the plan adjusts gradually as it goes.
3. **Turn GLAZE.** The knob sets how far to push toward this song's ideal master: 0 means nothing moves (fully manual, apart from two Genre and Delivery effects that don't depend on GLAZE; see [Genre and Delivery](/docs/glaze#genre-and-delivery)); higher means louder, more correction and more colour. Dynamics protection comes before loudness: once each module's budget is used up it stops pushing, and the “Target … LUFS” on the status line comes down.

> There's no listening phase to wait for: it processes from the first beat, stays cautious until it has heard enough of the main section and relaxes as it hears more; the spectral balance itself follows the music in real time all along.

## Is it better?

- **A / B**: Processed / Original in the middle row. B runs through the same dry path as host bypass — latency-aligned with a 20 ms crossfade, so switching never clicks.
- **Match**: with it on, both sides are aligned by measured loudness — the louder side is turned down to match, never boosted. That way you compare whether the tone and dynamics improved, not whether it got louder.
- **Listen to difference**: the headphones on the LIMIT card play only what the limiter removes. Flattened drums or muddy bass are obvious straight away.

Give it a few seconds of playback before comparing; Match needs time to measure the loudness difference.

## Everyday actions

| To | Do this |
|---|---|
| Move on to another song | Nothing to click: GLAZE adjusts to the new song gradually as the old song's statistics fade out; click Reset meters to clear integrated loudness and peak holds |
| Master a House track, or export one song as both a Streaming and a Club version | Pick Genre (left) and Delivery (right) on either side of the GLAZE heading. House genres recognise the main section the four-on-the-floor way, staying cautious through the intro and relaxing once the drop arrives. After each Delivery switch, play from the drop until the “Target … LUFS” reading settles before you export. See [Genre and Delivery](/docs/glaze#genre-and-delivery) |
| Work fully by hand | Turn GLAZE to 0 and set Genre and Delivery back to General and Standard; the knobs on the four cards are then all there is |
| Stop one part from working automatically | Switch that row on the CLAY card to Off or Manual; “Loud, not harsh” and Target spectrum under Advanced each have their own Off |
| Get ready to export | An offline export bypasses Match, Normalisation preview, Original and Listen to difference automatically; before a real-time export, turn them off by hand. See [Before you export](/docs/listening#before-you-export) |
| Make the interface bigger or smaller | Pick a size under Settings in the top bar, or drag the bottom-right corner; 70%–200%, saved with the project |

## Defaults

- GLAZE 35%, Auto style, General genre, Standard delivery; Ceiling −1.0 dBTP; Lookahead 2 ms; Auto release on.
- “Loud, not harsh” and Target spectrum under Advanced default to Auto in new instances (linked to GLAZE, zero at GLAZE 0); old projects load with them off. An old project's GLAZE listening result is discarded on load, though, so its sound still changes; see [Saving and reopening](/docs/glaze#saving-and-reopening).
- CLAY's three controls default to Auto: Low cut and Width act only when this song really needs it and a trial run shows they won't raise the peaks; Mono on Auto never folds the lows — it only warns when they're out of phase.

Next: [Install and compatibility](/docs/install). Curious what it's computing? See [Open algorithms](/algorithms).

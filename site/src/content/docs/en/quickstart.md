Day to day, Celadon takes three steps: insert it on the master bus, press play, turn GLAZE. Every other knob is the floor; GLAZE adds automatic increments on top of your settings, based on this song.

## Three steps

1. **Insert it last on the master (stereo) bus.** If you also use dither or loudness-meter plug-ins, put Celadon before them. Your host compensates the latency automatically (about 28.8 ms at 48 kHz).
2. **Play the song.** New instances default to GLAZE 35 % and the Auto style. Once playback starts, the status at the top changes from Not learned to Listening, then after 15–30 seconds goes through Calibrating to Locked — by then it has measured the song's loudness, dynamics, spectrum and attacks and settled on a plan.
3. **Turn GLAZE.** The knob sets how far to push toward this song's ideal master: 0 means nothing moves (fully manual); higher means louder, more correction and more colour. Once it reaches the dynamics-protection limit it stops pushing, and the status reads Limited.

> While it listens, the previous plan stays in effect, so there's never a stretch of unprocessed audio. The first 15–30 seconds build a profile of the whole song; the spectral balance itself follows the music in real time all along.

## Is it better?

- **A / B**: Processed / Original in the middle row. B runs through the same dry path as host bypass — latency-aligned with a 20 ms crossfade, so switching never clicks.
- **Match**: with it on, both sides are aligned by measured loudness — the louder side is turned down to match, never boosted. That way you compare whether the tone and dynamics improved, not whether it got louder.
- **Listen to difference**: the headphones on the LIMIT card play only what the limiter removes. Flattened drums or muddy bass are obvious straight away.

Give it a few seconds of playback before comparing; Match needs time to measure the loudness difference.

## Everyday actions

| To | Do this |
|---|---|
| Move on to another song | Click Relearn (it listens for another 15–30 s while the old plan keeps working, then crossfades over in about 3 s); click Reset meters to clear integrated loudness and peak holds |
| Work fully by hand | Turn GLAZE to 0; the knobs on the four cards are then all there is |
| Stop one part from working automatically | Switch that row on the CLAY card to Off or Manual; Loud, not harsh and Target spectrum under Advanced each have their own Off |
| Get ready to export | Turn off Match and Normalisation preview, and leave Listen to difference; stop the preview before a real-time export in particular (offline exports turn it off automatically) |
| Make the interface bigger or smaller | Pick a size under Settings in the top bar, or drag the bottom-right corner; 70 %–200 %, saved with the project |

## Defaults

- GLAZE 35 %, Auto style; Ceiling −1.0 dBTP; Lookahead 2 ms; Auto release on.
- Loud, not harsh and Target spectrum under Advanced default to Auto in new instances (linked to GLAZE, zero at GLAZE 0); old projects load with them off, so they sound exactly as before.
- CLAY's three controls default to Auto: they only act when this song really has a problem, and only after a trial run shows they won't raise the peaks.

Next: [Install and compatibility](/docs/install). Curious what it's computing? See [Open algorithms](/algorithms).

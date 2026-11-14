GLAZE doesn't turn every knob up at once. It **listens to the song as it plays and keeps deciding how to process it and how much loudness to push, based on its loudness, dynamics, spectrum and attacks**; the knob only sets how far to push toward this song's ideal master.

## What the knob means

| Value | Meaning |
|---|---|
| 0 | Nothing moves: the manual values on the four cards are all there is, and the output is bit-identical to having no GLAZE at all |
| 35 (default) | Gentle: new instances start here |
| Higher | Louder, more correction, more colour; every automatic increment only ever rises as GLAZE goes up and never exceeds each module's limit |
| No louder when turned further | Dynamics protection comes before loudness: once each module's budget is used up GLAZE stops pushing, and the “Target … LUFS” on the status line comes down. Lower it to keep more movement |

**Actual value = clamp(manual value + automatic increment).** The manual knobs are the floor and GLAZE adds on top; it never automatically cancels out your changes. On screen, cyan is your value and violet is the automatic increment.

GLAZE drives 7 parameters: BALANCE's Recover / Tame, COLOR's Tone / Glue / Heat, and LIMIT's Gain / Clip. CLAY's three controls (Mono never folds the lows on Auto, it only warns) and the automatic options under Advanced follow it too (see below).

## What it listens for

GLAZE listens all the time while the song plays: loudness (M / S / I / LRA), peak-to-loudness ratio, positive and negative spectral evidence, attack density… weighted towards the **main section** (the loudest, fullest parts) to build a **profile** (saved with the project). Profile + style + GLAZE + manual values → a **plan**, recomputed every second from the latest profile; the automatic increments rise slowly and fall faster as they move toward the new plan, and in lighter sections such as an intro or breakdown the gain can only fall, never rise. Until it has heard enough of the main section the plan stays cautious, and it relaxes as it hears more. A very slow trim at the output brings the measured loudness to the target, and a set of guards takes gain back when a constraint is exceeded. It never waits through a passage before settling on a plan, and nothing locks. For how it recognises the main section and what happens in intros and breakdowns, see [How it follows the main section](#how-it-follows-the-main-section).

The stage label beside the GLAZE heading matches the status line at the top; only one shows at a time, the first of these that applies:

| Label | Meaning |
|---|---|
| Manual limit | Orange; the status line reads “Limited by manual settings”: your manual settings alone already exceed a module's budget (or LIMIT is bypassed), so the matching automatic increment is withdrawn. Not shown at GLAZE 0 |
| Export | Orange; the status line reads “First drop may export quiet: play it once, then export”. An offline export started before enough of the main section had been heard |
| Limited | Orange; the status line reads “Limited · cause · backed off x dB”: during playback a guard is pulling GLAZE's automatic gain back, or has reached its floor and the problem persists (at most 3 dB). The cause is one of Budget (glue / limiting / clipping over the style's budget) or Limiter (the limiter working too hard). Once the condition clears it eases back and the status returns to Following. Not shown at GLAZE 0 |
| Warming up | The status line reads “Warming up · main section not heard yet (staying conservative)”. Not enough of the main section heard yet (just started, or still in the intro): the plan stays cautious and relaxes as it hears more |
| Holding | The status line reads “Holding · this section is quieter”. A lighter section is playing: gain can only fall, never rise, until the main section returns |
| Following | The status line reads “Following · Auto→Warm · Target −9.6 LUFS”. The usual state: following the profile built so far |
| Manual | The status line reads “GLAZE 0 · manual settings only”. GLAZE is at 0; none of the lines above appear |

> The profile builds up over the whole song: it stays cautious through the intro, relaxes once the main section arrives, and lighter sections never pull the gain up. Moving on to another song needs no button; the old song's statistics fade out as the new one plays.

## Styles

| Style | Direction |
|---|---|
| Auto (default) | Blends the three styles per song; the status at the top shows which way it's leaning |
| Warm | Thick, round, never harsh |
| Punchy | Strong drums, louder |
| Clean | Clear, less compression, more dynamics |

The style also sets the shape of the automatic Target spectrum under Advanced: Warm → Warm, Clean → Bright, Auto / Punchy → Neutral.

## How it follows the main section

GLAZE doesn't listen to a stretch first and then decide; it keeps a tally part by part and weights each part by how much it sounds like the main section: the loudest parts count heavily, lighter ones such as intros and breakdowns lightly. So:

- Just after you press play, while the intro is still running, the status line reads “Warming up · main section not heard yet (staying conservative)” and the plan stays cautious; it relaxes once the main section arrives and enough of it has been heard.
- In lighter parts such as an intro or breakdown it reads “Holding · this section is quieter”: the gain can only fall, never rise, so light parts aren't pushed up.
- The target loudness (the “Target … LUFS” on the status line) is the loudness of the main section, not the song's integrated loudness — the intro and breakdown pull that one down, and it only shows in the integrated reading after a full play-through (see [Meters and spectrum analyser](/docs/meters)).
- Jumping around is fine: when the host gives a tempo it tallies by bar, and replaying a bar only overwrites it rather than counting it twice; without a tempo it counts in seconds instead.
- To make it relax sooner, play through from the drop (or the chorus) once.

## Genre and Delivery (removed)

From 0.1.8.8 to 0.1.8.10 two choices sat on either side of the GLAZE heading: Genre (General / Deep / Tech / Melodic) and Delivery (Standard / Streaming / Club). Both were removed in 0.1.8.11, and every song is now processed the way General + Standard always was: the main section is recognised by loudness, the tonal target and target loudness follow the style, and the true-peak ceiling is your Ceiling (minus any codec margin).

- **Opening an old project saved with a House genre, Streaming or Club**: it loads as General + Standard, so it sounds different, and the bottom-left corner says so once: “Genre/delivery removed, now General + Standard; sound changes”. Old projects saved with General + Standard don't show this notice.
- **A version for streaming platforms**: Streaming used to cap the ceiling at −2 dBTP; now pull the Ceiling down yourself (to −2 dBTP, say), or add a codec margin under Advanced, then remeasure true peaks with an independent tool after encoding.
- **A louder version** (what Club used to do): turn GLAZE up, or switch the style to Punchy.

## How it relates to Advanced and CLAY

- With “Loud, not harsh” and Target spectrum on Auto: strength = the amount you set × GLAZE / 100. At GLAZE 0 they're zero.
- CLAY's Low cut and Width on Auto are decided by the plan and do nothing at GLAZE 0; Mono on Auto never folds the lows, it only lights an orange warning when they are out of phase (at GLAZE 0 even the warning stays off); on Manual they work even at GLAZE 0; Off never processes.
- Whenever an Advanced setting changes (or a manual parameter or the style does), GLAZE recomputes the target straight away and moves toward it by rising slowly and falling faster.

## Saving and reopening

What GLAZE has gathered (the bar tally, the profile, the automatic increments being applied, the output trim, each guard's relief and CLAY's automatic values) is saved with the project, and after reopening it carries on from where it was saved instead of starting over. Offline export and real-time playback run through the same processing path and the statistics keep updating during an export, so two exports of the same project in a row aren't guaranteed to be bit-identical. Projects saved before 0.1.8.9 (when GLAZE started following while you play): the old warm-up result, plan and calibration are discarded on load, so GLAZE starts following from scratch and the sound will change; the bottom-left corner says so once when the project opens: “The old warm-up result was discarded, so the sound will change”.

For the algorithm details: [GLAZE · decisions and following](/algorithms/glaze).

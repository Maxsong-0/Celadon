GLAZE doesn't turn every knob up at once. It **listens to the song first and decides how to process it and how much loudness to push based on its loudness, dynamics, spectrum and attacks**; the knob only sets how far to push toward this song's ideal master.

## What the knob means

| Value | Meaning |
|---|---|
| 0 | Nothing moves: the manual values on the four cards are all there is, and the output is bit-identical to having no GLAZE at all (with two exceptions, neither of which depends on GLAZE: with Delivery set to Streaming, LIMIT's ceiling is still capped at −2 dBTP; with a House genre (Deep / Tech / Melodic) and Release on Auto, LIMIT's slow release is still tightened to the host tempo. To work fully by hand, also set Genre and Delivery back to General and Standard; see [Genre and Delivery](#genre-and-delivery)) |
| 35 (default) | Gentle: new instances start here |
| Higher | Louder, more correction, more colour; every automatic increment only ever rises as GLAZE goes up and never exceeds each module's limit |
| No louder when turned further | Dynamics protection comes before loudness: once each module's budget is used up GLAZE stops pushing, and the “Target … LUFS” on the status line comes down. Lower it to keep more movement |

**Actual value = clamp(manual value + automatic increment).** The manual knobs are the floor and GLAZE adds on top; it never automatically cancels out your changes. On screen, cyan is your value and violet is the automatic increment.

GLAZE drives 7 parameters: BALANCE's Recover / Tame, COLOR's Tone / Glue / Heat, and LIMIT's Gain / Clip. CLAY's three controls (Mono never folds the lows on Auto, it only warns) and the automatic options under Advanced follow it too (see below).

## What it listens for

Once playback starts it listens automatically for 15–30 seconds: loudness (M / S / I / LRA), peak-to-loudness ratio, positive and negative spectral evidence, attack density… and builds a **profile** (with a confidence value; one per song, saved with the project). Profile + style + Genre + Delivery + GLAZE + manual values → a **plan**. The plan is first applied gently at low confidence, then the **calibrator** matches the input-to-output loudness difference and finishes once it hits the target; after locking, only bounded protective adjustments around that baseline remain. With a House genre (Deep / Tech / Melodic) it counts bars and locks only after hearing the main section (the drop and other full-arrangement parts), which usually takes around two minutes, and after locking it can still update the profile when a clearly louder run of bars turns up; see [How House genres listen](#how-house-genres-listen).

Status labels:

| Label | Meaning |
|---|---|
| Not learned | Nothing has played yet; listening starts automatically on playback |
| Listening | Gathering evidence (the corner shows “Listening x / 30 s”); the previous plan (or a gentle one) stays in effect |
| Calibrating | The plan is set and the measured loudness is being brought to the target |
| Locked | Done; only bounded protection from here on |
| Limited | Orange, in two cases: the status line reads “Limited by manual settings” — your manual settings alone already exceed a module's budget (or LIMIT is bypassed), so the matching automatic increment is withdrawn; or it reads “Plan limited · backed off x dB” — during playback a constraint keeps being exceeded and the protection layer, backing off slowly, has reached its floor (−3 dB) and it is still exceeded. Neither shows at GLAZE 0 |
| Paused | The host has stopped playback |

> The first profile captures the character of the passage it heard. Very quiet or very loud sections are still handled by calibration and protective back-off; for a different song, click Relearn to update the whole-song profile.

## Relearn

Listens for another 15–30 seconds to build a new plan; the old plan keeps working meanwhile, then fades over in about 3 seconds. Use it after switching songs or changing the mix; click again while it's listening to cancel. It never relearns on its own — only when you click it. The exceptions are with a House genre only: when the current profile wasn't learned with a House genre (for example it was learned in General), GLAZE starts a House-style listening round by itself, and after locking, a run of clearly louder bars is folded into the profile automatically. See [Genre and Delivery](#genre-and-delivery). With a House genre, Relearn also listens bar by bar: it clears the bars heard so far and starts counting again from zero, and it has to collect 64 bars and hear the main section again before the plan changes (usually around two minutes); the old plan keeps working meanwhile.

## Styles

| Style | Direction |
|---|---|
| Auto (default) | Blends the three styles per song; the status at the top shows which way it's leaning |
| Warm | Thick, round, never harsh |
| Punchy | Strong drums, louder |
| Clean | Clear, less compression, more dynamics |

The style also sets the shape of the automatic Target spectrum under Advanced: Warm → Warm, Clean → Bright, Auto / Punchy → Neutral. With a House genre the shape follows the genre instead; see the next section.

## Genre and Delivery

From 0.1.8.8, two choices sit on either side of the GLAZE heading: **Genre on the left, Delivery on the right**. They do different jobs from the Style above, and any combination works:

| Choice | The question it answers | Options |
|---|---|---|
| **Genre** | What kind of music is this? Sets the tonal target, and how GLAZE listens to the song | General (default) / Deep / Tech / Melodic |
| **Delivery** | Where is this version going? Sets the target loudness and the true-peak ceiling | Standard (default) / Streaming / Club |
| **Style** | Where should the loudness be spent? Sets how much goes to limiting, glue and colour | Auto (default) / Warm / Punchy / Clean |

The default is General + Standard: neither choice does anything, and GLAZE works just as it would without them. Old projects have no such settings; they load as General + Standard, and their saved plan and calibration stay in effect. Genre and Delivery are saved with the project but can't be automated in the host — they are decisions about how this song gets mastered, not moves that change over time.

### Genre

| Genre | When to pick it |
|---|---|
| **General** (default) | Not House, or you're not sure: works as before |
| **Deep** | Deep House |
| **Tech** | Tech House |
| **Melodic** | Melodic-Progressive (Melodic House and Progressive House both fit) |

The selector shows only Deep / Tech / Melodic; the tooltip gives the full names Deep House / Tech House / Melodic-Progressive. Pick the House type you wrote, or the closest one; use General for anything else.

With Deep, Tech or Melodic (called “House genres” below), six things change:

- **Tonal target**: automatic Tone (on COLOR) aims at a House reference and has less room to brighten than in General. With Target spectrum under Advanced on Auto, the target shape follows the genre instead of the Style: Deep leans warm, Tech is neutral and Melodic sits between the two. Every other setting is shared by the three sub-genres, so this target shape is the only difference between them — with Target spectrum Off (old projects load with it Off) or on a fixed shape, Deep, Tech and Melodic behave identically.
- **Low cut**: CLAY's automatic Low cut aims at 25 Hz (General uses 16–20 Hz, depending on the style). Whether it cuts at all is still decided by whether the song has infrasound, and it has to pass a trial first: if the cut would raise peaks or thin the low end, it is reduced or skipped.
- **Width**: CLAY's Auto never widens, so the master isn't pulled wider. Manual Width still works.
- **Listening**: bar by bar, using only the main section (the drop and other full-arrangement parts), and it locks only after hearing the main section; the details and the status line are at the end of this section, under [How House genres listen](#how-house-genres-listen).
- **Limiter release**: LIMIT's Auto release is tightened to the host tempo: the slow stage of the release is kept to no more than a third of a beat (about 160 ms at 124 BPM). This applies only when Release is on Auto, the host is playing and its tempo is between 100 and 140 BPM, wherever the GLAZE knob is (including 0); a manual Release is unaffected.
- **Out-of-phase hint**: when the lows in the main section are out of phase and mono playback would lose bass, you're told with a more sensitive test than in General (a mono loss from about 1 dB counts, but it has to persist for 4 bars of main section, or 8 seconds when the host gives no tempo). The GLAZE status line in the status pill at the top switches to this warning in amber, “Club mono loses low end (about x dB), fix it in the mix”, and takes the place of the usual status while it is lit (you won't see the House progress, Calibrating or “Locked · … · Target … LUFS”; the stage label is still drawn, and a target marker in the loudness panel means it is locked), and the CLAY card's mini graph is replaced by two lines, “Club mono loses low end (about x dB)” and “Fix it in the mix”. It only tells you — it doesn't touch the audio — and it stays off at GLAZE 0. Applying Mono can't help mono playback; narrow the lows or align their phase in the mix.

### Delivery

Delivery decides how loud this version should be and how much true-peak margin it keeps:

| Delivery | Target loudness | True-peak ceiling | When to pick it |
|---|---|---|---|
| **Standard** (default) | Set by the style (the rule is unchanged) | Your Ceiling (minus any codec margin) | Everyday use, or when you haven't decided where it's going |
| **Streaming** | About −10 LUFS | At or below −2 dBTP; if your Ceiling (minus any codec margin) is lower, that wins | The version you upload to a streaming service |
| **Club** | About −7 LUFS | Your Ceiling (minus any codec margin); never raised automatically | The version for DJs and download stores |

- **The loudness in the table is roughly what GLAZE aims for with the knob at 100.** The knob still decides how far to push toward it, and the default 35% pushes only part of the way; to get closer, turn GLAZE up. The “Target … LUFS” on the status line is the number it is actually chasing right now. For a mix that is already close to or above the table's loudness, see the third point below.
- **Style still counts in Streaming and Club**: the target moves up or down around the setting's loudness with the style (Auto blends by weight), and the gaps between the three styles are the same as in Standard. Songs with a wide loudness range get a slightly lower target, exactly as in Standard.
- **GLAZE only pushes up; it never turns down a mix that is already loud enough.** If the mix is already louder than the target, the master isn't pulled back to it. All three settings keep a “minimum push”: when a mix is already close to or above the table's loudness, GLAZE still pushes a little more, so with the knob at 100 the target can end up louder than the table's number. Streaming and Club also have a hard upper limit (about −8 and −5 LUFS) that even this minimum push won't take them past, and a mix that is already louder than that limit isn't pushed at all; Standard has no such hard limit.
- **The target is a direction, not a guarantee.** Dynamics protection comes before loudness: once each module's budget is used up GLAZE stops pushing and the target comes down, so the “Target” on the status line reads lower than it would without that limit.
- **Streaming's −2 dBTP is the effective ceiling handed to LIMIT**, wherever the GLAZE knob is (including 0). It leaves extra true-peak margin for lossy transcoding, so with Streaming selected there's no need to pull the Ceiling down yourself. The other two settings leave the ceiling alone.
- **With a House genre, the target loudness is that of the main section (the drop)**, not the integrated loudness of the whole song. The intro and breakdown pull the latter down, and you only see it in OUT's integrated reading after playing the song through (see [Meters and spectrum analyser](/docs/meters)). With General it still refers to the stretch heard after playback starts.

### Switching, and exporting two masters

- **Switching doesn't need Relearn.** Changing Delivery or Genre recomputes the plan on the profile it has already learned: the new plan fades in over about 3 seconds, then calibrates again (the status line reads Calibrating). Two versions that differ only in Delivery share one profile and make the same Tone, BALANCE and stereo clean-up decisions (switching Genre does change the tonal target, Low cut and Width); they differ in how much gain is pushed and how much true-peak margin is left, and the Glue, Heat and Clip amounts are shared out again for that gain and ceiling.
- **With a House genre, calibration only starts inside the main section** (when the committed profile was learned with House gating): if you switch while the song is playing an intro or breakdown, then after the 3-second transition the status line reads “Locked · awaiting calibration (conditions changed)”, and calibration starts when the drop plays; a round already under way just pauses outside the main section and the status line stays on Calibrating; if you switch while stopped, the status line first reads “Calibration paused (stopped)”.
- **Going from General to a House genre**: the plan is recomputed for the new genre straight away. If the current profile wasn't learned with a House genre (it was learned in General, or comes from an old project), GLAZE also starts a House-style listening round by itself: the old profile stays in use until the new one is ready, and bars are counted from when that round starts (if you switch while stopped, from the next time you play). A profile already learned with a House genre is kept, so switching between House genres, or going from a House genre to General and back, doesn't relearn.
- **To get two masters of one song**:
  1. Pick the Genre, play the song through once and wait for Locked (a House genre needs to hear the main section; watch the status line, and see [How House genres listen](#how-house-genres-listen) at the end of this section). Turn GLAZE to the amount you want.
  2. Set Delivery to Streaming, play from the drop and keep playing until the status is back to “Locked · … · Target … LUFS” (not Calibrating, Calibration paused or awaiting calibration), then export this version. If it reads Plan limited along the way, keep playing: GLAZE lowers its plan a step by itself and then calibrates again. With a House genre's out-of-phase hint lit, the status line is taken over by the hint; look for the target marker in the loudness panel instead, which appears once it is locked.
  3. Set Delivery to Club, play from the drop again until the status is back to Locked, then export the second version.

If you export offline straight after switching, the start of the export may not be calibrated yet — hence playing until the status is back to “Locked · … · Target … LUFS” first. With a House genre the status must also already read Locked before you export, otherwise the plan changes partway through the master (the status line warns you). Before exporting, turn off Match and Normalisation preview as usual; see [Monitoring](/docs/listening#before-you-export).

### How House genres listen

In General, GLAZE decides its plan after listening for 15–30 seconds from the start of playback. A House intro often has 16–32 bars of drums only, with no bass or pads, so listening to the start alone would take the intro for the whole song. With a House genre, GLAZE listens like this instead:

1. **It keeps a tally bar by bar.** It follows the position and time signature the host gives it and notes one bar at a time. Replaying a bar only overwrites it rather than counting it twice, so jumping around the song gradually fills in all of it. When the host gives no tempo, it falls back to one entry every 2 seconds.
2. **It plans from the main section only.** A bar counts as main section when it is both loud enough and full enough in the low end (the drop and similar full-arrangement parts). The gates are relative: until the drop arrives, the loudest, fullest part so far — often the intro — counts as main section; once the drop is in, the gates rise with it and most intro and breakdown bars stop counting as main section (an intro with kick and bass that is only a little lighter than the drop can stay inside). They still count as bars heard. The profile, loudness and spectrum are all taken from the main section alone.
3. **It locks only after listening enough.** Three conditions must all be met before it calibrates and locks: at least 64 different bars heard (about 2 minutes at 124 BPM; 124 seconds when counting in seconds), at least 8 bars of main section (one phrase; 16 seconds when counting in seconds), and the same confidence conditions as in General.
4. **On a first listen, the plan is provisional until it locks.** It is a more cautious plan built from the main section heard so far, and it relaxes as more is heard; there is no calibration in the meantime. The plan is recomputed whenever something new turns up — normally fading in over about 3 seconds, but over about 0.5 seconds when a bar arrives that is at least 3 LU louder than the loud parts heard so far and full in the low end (usually the drop), so the drop isn't held back by a plan built from the intro. During Relearn, or the round started by switching from General, there is no provisional plan: the old plan keeps working instead.
5. **After it locks, it keeps watching.** If a run of 4 bars turns up later that is at least 3 LU louder than the profile and has a low-frequency share of at least 60% of the profile's (usually a late drop), GLAZE folds them into the profile, switches plan over about 0.5 seconds and calibrates again. Each profile can be updated this way at most twice, and only towards louder material.

> With material shorter than 64 different bars (a short loop or a clip), a House genre can't meet the lock conditions: the status line keeps showing progress, and on a first listen the plan stays in its cautious, uncalibrated provisional state. Choose General for that kind of material.

While it listens, the GLAZE status line in the status pill at the top reports progress (the tooltips of the GLAZE heading and of the pill switch to a House version too):

| Status line | What it means |
|---|---|
| House · 0 / 64 bars · main section not heard yet | Just started; no bar counts as main section yet |
| House · 41 / 64 bars · 8 bars of main section | A phrase of main section is in, but the bar count hasn't reached 64, so it hasn't locked yet |
| House · 52 / 124 s (no tempo from host) | The host gives no tempo, so it counts in seconds: 124 are needed |

The gates are relative: after a bar or two the intro already counts as main section, so “x bars of main section” appears; once the drop arrives the figure can first shrink and then climb again, so it doesn't tell you whether the drop has arrived.

Two more notes may follow at the end of the line: “segment memory full” (it keeps at most 512 bars, about 16 minutes) and “exported before learning finished: play it through once, then export again”. With GLAZE above 0, the second line at bottom left says “Start playback at the drop to learn faster” at the same time (at GLAZE 0 it reads “GLAZE at 0: fully manual”). Once the conditions are met it calibrates (the status line reads Calibrating) and then shows “Locked · … · Target … LUFS”; after the lock, calibration rounds only start inside the main section, so while the song is in an intro or breakdown and no round has started, the status line reads “Locked · awaiting calibration (conditions changed)” until the drop plays.

## How it relates to Advanced and CLAY

- With “Loud, not harsh” and Target spectrum on Auto: strength = the amount you set × GLAZE / 100. At GLAZE 0 they're zero.
- CLAY's Low cut and Width on Auto are decided by the plan and do nothing at GLAZE 0; Mono on Auto never folds the lows, it only lights an amber warning when they are out of phase (at GLAZE 0 even the warning stays off); on Manual they work even at GLAZE 0; Off never processes.
- Whenever an Advanced setting changes (or a manual parameter, the sample rate, the style, the genre or the delivery does), the calibration's reference conditions change and calibration realigns.

## Saving and reopening

The profile, plan, calibration, constants snapshot and execution snapshot are all saved with the project. Reopening doesn't relearn, and re-rendering from the same starting point is bit-for-bit reproducible. Offline export and real-time playback learn and calibrate on the same sample timeline, so switching to offline neither stops learning nor undoes calibration. Genre and Delivery are saved with the project too; with a House genre the bar tally is not — after reopening, the locked profile stays in effect and bars are counted again from zero (the watch after locking only starts to work once 64 bars have been heard again).

For the algorithm details: [GLAZE · decisions and calibration](/algorithms/glaze).

# GLAZE · decisions and following

“The big knob can't just turn every knob up — it has to get the result and raise the volume fully automatically, by algorithm, from the overall state of the music.” — GLAZE listens to the song as it plays and keeps deciding how to process it and how much loudness to push, based on its loudness, dynamics, spectrum and attacks; the knob only sets how far to push toward this song's ideal master.

## Five key choices

| Question | Decision |
|---|---|
| How it follows the music | It listens all the time: a live, main-section-weighted profile feeds the planner once a second, and the automatic increments move toward each new target under a rate limit and a ratchet, rising slowly and falling faster; there is no listening phase and nothing locks |
| What the knob means | Strength: how far to push toward this song's ideal master; 0 = nothing moves (equal to the manual result) |
| Manual knobs | The algorithm's value is the floor and the knobs fine-tune it: actual value = clamp(manual value + automatic increment); it never automatically cancels out your changes |
| When the profile updates | All the time, and it's saved with the project (`LIVE`); there's no Relearn, and it doesn't guess song changes from the host transport — old statistics fade out through soft forgetting |
| Style | Pick one of four: Auto (default) / Warm / Punchy / Clean; Auto blends the weights of the three styles per song |

## Seven units

| Unit | Responsibility |
|---|---|
| **MusicAnalyzer + LiveProfile** | Input features (M / S / I / LRA, PLR / PSR, positive and negative spectral evidence, attacks, confidence) go into one live bar-by-bar tally (the same for every genre); LiveProfile weights it by main-section likelihood and publishes the profile and c on schedule |
| **GlazePlanner** | A pure function: published profile + style / genre / delivery + GLAZE + manual values + factory constants → target (7 automatic increments, Clay automatic values, target loudness, style weights) |
| **GlazeConstants** | Style preferences, the genre profiles and House constants, the three deliveries, module constraints, the Heat conversion table; no longer saved as a snapshot with the project |
| **Rate limit and ratchet** | Moves the applied increments toward the target: gain rises slowly and falls faster; while m < 0.5 the net gain can only fall |
| **G_trim + OutputLevelTracker** | A very slow trim from the difference between the output's main-section level and the target, frozen while the budget loop is backing off. Replaces the calibrator |
| **Guards** GlazeGuard / ContrastGuard / PsrGuard | The budget loop, the contrast guard and the PSR guard each give a relief that never adds level; the smallest is added to Limit Gain. The cause and the amount on the status line come from the same guard that is currently acting |
| **GlazeController** | Continuous scheduling: recomputes or applies every block and writes the status snapshot; the project stores only `LIVE` |

![Continuous scheduling: the live profile feeds the planner every 1 s → rate limit and ratchet → output trim G_trim and guard relief](figure:glaze)

## What happens every block

1. If the knob, style, genre or delivery has changed since the last block, the target is recomputed straight away from the profile currently published. At GLAZE ≤ 0 every target increment is 0, and the rate limiter, G_trim and guard relief drop to 0 in that block, so the parameters handed to the modules equal the manual values bit for bit.
2. Analysis: the input goes into the live tally; finished entries go into LiveProfile, which publishes a profile on schedule.
3. When analyser time crosses the 1 s grid, the target is recomputed from the newly published profile (once per block at most).
4. The rate limiter moves the applied increments toward the target, rising slowly and falling faster; while the current main-section likelihood m is below 0.5, the net gain can only fall.
5. G_trim applies a very slow trim from the difference between the output's main-section level and the target.
6. The three guards each give a relief; the smallest is added to Limit Gain. True peak is still guaranteed by the limiter and the Ceiling.
7. The status snapshot is written (the seven status lines read only this).

There is no listening, plan transition, calibration or lock. From a cold start c = 0 and the planner uses its cautious low-confidence plan; c rises with the quality of the main section heard.

## The planner's targets and constraints

- requestedTarget: the loudness that Delivery, the style and GLAZE want; effectiveTarget: the loudness currently allowed after module constraints — the output trim G_trim and the “Target” on the status line both use this one.
- **One-knob baseline** (a full-strength request, then scaled by GLAZE, confidence, style limits and module budget): a minimum loudness-increase request of 3 dB (on all three Delivery settings, kept even for songs already at the preferred loudness), automatic Recover 8, automatic Tame 24, automatic Heat 12.
- Material that's already loud enough still keeps that minimum push (GLAZE only pushes up, it never turns down); for Streaming and Club the target has a hard upper limit (−8 / −5 LUFS), and the requested gain is 0 only when the input is already at or above that limit; when a limit is exceeded, **the target is lowered rather than forcing harder compression**.
- Automatic Tone can brighten (positive) or soften (negative): it's compared with the profile at equal weight, with a 0.35 dB/oct dead zone, an effective bandwidth, 0.15 dB/s following at full strength, and a style limit of ±0.75 dB × GLAZE (0.4 dB towards brighter with a House genre; see “Genre and Delivery” below). The style and loudness target aren't re-chosen because of short-term brightness.
- Auto style is no longer pushed wholesale toward Clean by a 60 s LRA confidence that hasn't filled up yet; LRA still constrains the dynamics features on its own.
- The parallel Heat's share of peak reduction is an optional contribution: when it falls short, it doesn't lock up a limiter that still has headroom.
- **Automatic Heat only takes on peaks it can genuinely shave (algorithm / constants version 8, 0.1.8.6)**: Heat is added for peak reduction only if, within its limit, it can shave at least 0.1 dB off this song's peaks; otherwise only the style baseline applies (12 for all three styles out of the box); full-strength automatic Heat drops from the old per-style 70 / 50 / 20 to about 12. An old project's constants snapshot is discarded on load, and the plan always uses this version's factory constants.

## Genre and Delivery

From 0.1.8.8, GLAZE has two independent choices, and one principle behind them: **Genre changes only the tonal targets and the way the main section is recognised (plus, with a House genre, the beat-following limiter release and the out-of-phase hint) and carries no absolute loudness of its own; loudness belongs to Delivery; the dynamics budget (how much limiting, glue and clipping is allowed) still belongs to Style.** The reason is that the research found House's three sub-genres can't be told apart by their dynamics — they differ in spectrum — so Genre leaves the dynamics budget alone. Neither choice changes the signal chain (the same modules in the same order): they add constants and conditions to GLAZE's analyser, planner and controller, plus three things handed straight to the chain — the shape of Target spectrum on Auto, Streaming's −2 dBTP ceiling and, with a House genre, a beat-based cap on LIMIT's Auto release; the last two apply wherever the GLAZE knob is. General + Standard changes nothing.

### Genre settings

After the style weights have been blended, the three House sub-genres override the four items below (General overrides none of them, and only the automatic part is overridden — your manual values are left alone):

| Item | General (by style) | Deep / Tech / Melodic |
|---|---|---|
| Reference slope for automatic Tone (total power inside BALANCE's bands, pink noise = 0) | −3.5 / −2.5 / −2.5 dB / octave (Warm / Punchy / Clean) | −2.25, the same for all three |
| Limit on automatic Tone (both directions scale with GLAZE) | 0.75 dB both ways | 0.4 dB towards brighter, still 0.75 dB towards darker |
| Target frequency of automatic Low cut | 16 / 18 / 20 Hz (Warm / Punchy / Clean) | 25 Hz |
| Style factor for automatic Width | 0.4 / 0.6 / 1.0 | 0 (no automatic widening) |

Shape slope of Target spectrum on Auto (dB / octave, zero at 1 kHz): General takes the shape from the style (Warm → Warm −1.5, Clean → Bright 0, Auto and Punchy → Neutral −0.75); House takes Deep −1.5, Tech −0.75, Melodic −1.25.

The −2.25 reference slope was calibrated on 45 independently produced House masters (15 per sub-genre): the median spectral slopes of the three sub-genres came out at −2.21, −2.19 and −2.24, with no measurable difference in brightness, so all three share one value, and the only thing that separates the sub-genres is the shape of the target spectrum (which only acts with Target spectrum on Auto). The brightening limit is cut to 0.4 dB because sound propped up by the very top end tends to turn artificial once a DJ pitches the track.

### The main section with a House genre

Every genre shares one live tally: one entry = one bar, placed by the host's position and time signature, falling back to one entry every 2 seconds when there's no tempo; replaying a bar overwrites the old value rather than counting it twice; at most 512 entries are kept. Genre only changes how the main section is recognised: House genres use a four-on-the-floor model, General a loudness model. The profile is weighted by main-section likelihood, so intros and breakdowns count for little; while the current part has m < 0.5, the ratchet lets the net gain fall but never rise, so light parts aren't pushed up. There are no lock conditions, provisional plan or watch after locking; the status line puts “House ·” in front of Following.

Keeping every intro bar out is still impossible: a DJ intro with kick and bass that is only 1–3 LU lighter than the drop can score close to the drop.

### Limiter release following the beat

In General the Auto release's slow stage runs between 100 and 800 ms, interpolated from the average gain reduction of the last 500 ms. With an average gain reduction of 1.5 dB, for example, the slow stage is about 190 ms and the gain takes about 0.56 s to recover — longer than the 0.48 s of one beat at 124 BPM, so the next kick arrives before the gain is back. With a House genre, the slow release becomes min(the adaptive value, max(100 ms, one beat ÷ 3)), with one beat = 60000 ÷ BPM (a quarter note): about 160 ms at 124 BPM.

It applies only when all of these hold: the genre is a House genre; Limit is on Auto release; the host tempo is valid and between 100 and 140 BPM; the host is playing. If any one fails, it behaves as in General. A manual Release and the fast release for short overloads (under 70 ms) are unaffected, and the GLAZE amount doesn't matter: choosing a House genre is choosing this limiter behaviour. The project tempo may differ from the audio's tempo (for example when mastering a finished track in an empty project at the default 120 BPM): the limit is only tightened to one beat ÷ 3, so a wrong tempo has limited consequences, and the plug-in doesn't track beats in the audio.

### The out-of-phase hint

CLAY's general out-of-phase lamp lights when the slow-path low-frequency correlation drops below −0.2. A House genre adds a more sensitive hint: when the slow-path low-frequency correlation in the main section stays below 0.6 for 4 bars (8 seconds when there's no tempo), the GLAZE status line and the CLAY card say how much low end mono playback would lose. A correlation of 0.6 corresponds to a mono bass loss of roughly 1 dB (with equal power left and right, mono power ∝ (1 + ρ) ÷ 2). Once the correlation rises back above 0.6, the hint stays for another 2 seconds (it goes out at once when the main section ends), and the loss figure in dB is computed by the formula when it lights, then smoothed over 1 second. It only tells you: it doesn't process the audio and doesn't apply Mono automatically — Mono only changes the stereo image and leaves mono playback as it is, so it can't rescue it, and the fix belongs in the mix. It stays off at GLAZE 0.

### Delivery

What follows gives the full-strength target at GLAZE 100; how far it actually pushes is further multiplied by GLAZE and by a confidence factor (0.35 + 0.65 × confidence, so 1 at full confidence), and the target is lowered if a module budget runs out. For Streaming and Club, target loudness = the setting's own loudness + the style offset − the loudness-range (LRA) correction, clamped into the setting's allowed range; Standard uses the blended style preference minus the LRA correction, clamped to −14 to −7 LUFS, with no style offset added (the preference already depends on the style):

| Delivery | Setting loudness | Allowed range | Effective Ceiling handed to the limiter |
|---|---|---|---|
| Standard | The blended style preference (−8.5 / −9 / −10 LUFS) | −14 to −7 LUFS | Your Ceiling − codec margin |
| Streaming | −10 LUFS | −12 to −8 LUFS | min(your Ceiling − codec margin, −2 dBTP) |
| Club | −7 LUFS | −9 to −5 LUFS | Your Ceiling − codec margin, never raised automatically |

- **Style offset** (Streaming and Club only) = the blended style preference − Punchy's preference (−9): Warm +0.5, Punchy 0, Clean −1, with Auto blending by weight. The **LRA correction** is unchanged (for each LU of loudness range above 6 LU, the target drops by 1/3 dB) and is subtracted before the clamp.
- **All three settings have a “minimum push” (3 dB at full strength)**: material that is already close to or above the setting's loudness still gets a gentle extra push. **For Streaming and Club, the top of the allowed range is a hard limit**: even this minimum push won't take the target past it (−8 LUFS for Streaming, −5 for Club), and material that is already louder than the limit isn't pushed at all; Standard has no such hard limit.
- **Streaming's −2 dBTP leaves margin for lossy transcoding**: the effective Ceiling handed to LIMIT is capped by it from the start, wherever the GLAZE knob is. For Club, the Ceiling is entirely yours and never raised automatically — there is no reliable basis for saying that club playback should use a higher ceiling than you set.
- **The target aims at the loud part**: the planner works out gain from “target − profile loudness”. In General the profile loudness is that of the main section recognised by the loudness model (the loudest parts); with a House genre it is that of the main section recognised by the four-on-the-floor model (full-arrangement parts such as the drop), so Club's −7 LUFS refers to the drop, not to the whole-song integral — which the intro and breakdown pull down, and which only appears, as a result, in OUT's integrated loudness after the song has been played through.

### Switching

When Delivery or Genre changes, the planner recomputes the target at the start of that block from the profile currently published, and the applied side follows the same rate limits, rising slowly and falling faster, with no separate transition and no calibration round. If you switch Genre during playback, the way the main section is recognised changes only from the next time playback starts. Two masters that differ only in Delivery share one profile and make the same Tone, BALANCE and CLAY decisions (switching Genre does change the tonal target, Low cut and Width); they differ in the target loudness and true-peak ceiling, and in the amount of Glue, Heat and Clip used to reach them (all three are shared out from the estimated excess, true peak + gain − Ceiling). A known limitation: an offline export started straight after switching may begin before the new target is reached; an offline export started before enough of the main section has been heard (c < 0.5) makes the status line read “Main-section stats are still cold at export, so the first drop may come out quiet: play through once, then export”.

## Saving and offline export

GLAZE saves a single `GLAZE_STATE` (format 2) holding `LIVE`: the live profile window (`LIVE_PROFILE`), the rate limiter's current increments, G_trim, the Auto style weights, the three guards, the output-level statistics and CLAY's automatic values, wrapped in zlib + base64 and lossless. From the first block after loading, these control values equal what was saved, bit for bit; filters and any windows not stored in `LIVE` start from zero. An old project's `GLAZE_PROFILE` / `PLAN` / `CALIBRATION` / `EXECUTION` / `GLAZE_CONSTANTS` nodes are discarded on load and treated as a cold start, and the interface says so once. A host reset, stopping or seeking doesn't clear `LIVE`. Offline and real time share one processing path; every export keeps updating the statistics, so repeated exports aren't guaranteed to be bit-identical.

## Guard statistics slots

The guard approximates a p95 over 10 ms slot means. The chain is sliced at the protection layer's slot boundaries so module statistics never span slots; a whole host block still uses one set of execution parameters, and new control values apply from the next host block.

The protection layer's timing follows music time only: while the host is stopped, during silence and during host bypass, its statistics and observation periods are frozen, so it never absorbs feedback by mistake. After every target recomputation (once a second, or on a change of style, genre or delivery), the limits of the protection layer and the upstream monitor update with it. For items where your manual settings alone already exceed the style budget (Heat drive, Glue mean / p95, Clip depth / activity), the protection layer and upstream monitor no longer apply a constraint — the automatic part of that item is already 0, so checking it against the style limit would only cause pointless back-off; the other items are still checked against the style limits (all three since 0.1.8.6).

## Acceptance summary

1. With GLAZE held at 0 from initialisation, Genre on General and Delivery on Standard: the whole chain's output is bit-identical to the reference chain (90 scenarios).
2. The planner is a pure function: identical input gives bit-identical output; monotonic in GLAZE; never exceeds any module's limit; all increments are 0 at GLAZE 0.
3. Material that's already loud enough keeps the minimum push (a Standard request is never below it); for Streaming and Club the requested gain is 0 when the input is at or above the limit; when a limit is exceeded, the target is lowered.
4. Final output true peak ≤ Ceiling + 0.01 dB (as measured by the 8× meter).
5. Save round trip: from the first block after loading, the control values (applied increments, G_trim, the three guards, Clay automatic values, the published profile) equal what was saved, bit for bit; repeated exports aren't guaranteed to be bit-identical.
6. Performance: with GLAZE on, the whole plugin at 48k / 512 costs ≤ 0.15% more, with the worst block ≤ 0.5 percentage points more; zero allocations and zero locks on the audio thread.
7. Synthetic music with only GLAZE = 50: a cumulative loudness increase of 6.47 LU; GLAZE 0 / 25 / 50 / 75 / 100 → −23.97 / −20.61 / −17.24 / −13.96 / −10.64 LUFS (one piece of material, not a guarantee for any song).

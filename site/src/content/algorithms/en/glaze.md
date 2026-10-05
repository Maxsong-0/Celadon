“The big knob can't just turn every knob up — it has to get the result and raise the volume fully automatically, by algorithm, from the overall state of the music.” — GLAZE listens to the song first and decides how to process it and how much loudness to push based on its loudness, dynamics, spectrum and attacks; the knob only sets how far to push toward this song's ideal master.

## Five key choices

| Question | Decision |
|---|---|
| How it follows the music | Listen to a stretch first to establish a whole-song baseline (profile → plan), then make only bounded protective adjustments around that baseline |
| What the knob means | Strength: how far to push toward this song's ideal master; 0 = nothing moves (equal to the manual result) |
| Manual knobs | The algorithm's value is the floor and the knobs fine-tune it: actual value = clamp(manual value + automatic increment); it never automatically cancels out your changes |
| When the baseline is built | Learned automatically + saved with the project + a Relearn button; it never relearns on its own (the exceptions, with a House genre only: a listening round starts by itself when the existing profile wasn't learned with House gating, and after locking the profile can update itself at most twice for louder material) |
| Style | Pick one of four: Auto (default) / Warm / Punchy / Clean; Auto blends the weights of the three styles per song |

## Seven units

| Unit | Responsibility |
|---|---|
| **MusicAnalyzer** | Input features: M / S / I / LRA, PLR / PSR, positive and negative spectral evidence (reusing BALANCE's 27-band frames), attack activity, confidence; with a House genre, also a bar-by-bar tally with gating → profile |
| **GlazePlanner** | A pure function: profile + style / genre / delivery + GLAZE + manual values + constants snapshot → plan (7 automatic increments, Clay automatic values, target loudness, style weights) |
| **GlazeConstants** | Every constant and conversion table, versioned (style preferences, the genre profiles and House constants, the three deliveries, module constraints, the Heat conversion table) |
| **GlazeCalibrator** | Has an end condition: matches the input-to-output loudness difference with direction-aware anti-saturation; not a continuous closed loop |
| **GlazeGuard** | Backs off slowly when a constraint keeps being exceeded, g_guard ∈ [−3, 0] dB; only takes back automatic gain |
| **GlazeProfile** | Saving / loading the committed version (profile + plan + calibration + snapshot) |
| **GlazeController** | The state machine, candidate / committed versions, seeking / reset / offline; produces the actual parameters every block |

![State machine: Not learned → Listening → Calibrating → Locked ⇄ Limited; Relearn goes through a candidate version](figure:glaze)

## Timeline

1. **Not learned**: listening starts automatically on playback. Until then, a gentle low-confidence plan is used.
2. **Listening**: 15–30 s, ending once enough confidence has been gathered (`minListenSeconds 15 / maxListenSeconds 30`). The previous plan (or the gentle one) stays in effect. House genres follow different rules; see “Genre and Delivery” below.
3. **Plan transition**: the new plan crossfades in over about 3 s.
4. **Calibrating**: the calibrator brings the measured loudness difference to effectiveTarget; with unchanged reference conditions it catches up at 1 dB/s. If a calibration round is abandoned (GLAZE briefly off, LIMIT bypassed, a style switch and so on), a new round starts once the conditions are back as they were, so it never stays uncalibrated (0.1.8.6).
5. **Locked**: from here on there are only two paths of change — the protection layer's slow back-off, and section following (bounded pull-back of the automatic Heat / Glue shares). With a House genre there is a third: the watch after the lock can fold a louder run of bars into the profile (see “Genre and Delivery” below).
6. **Relearn**: builds a candidate version and crossfades it into the committed one when finished.

## The planner's targets and constraints

- requestedTarget: the loudness that Delivery, the style and GLAZE want; effectiveTarget: the loudness currently allowed after module constraints — calibration only uses this one.
- **One-knob baseline** (a full-strength request, then scaled by GLAZE, confidence, style limits and module budget): a minimum loudness-increase request of 3 dB (kept even for songs already at the preferred loudness), automatic Recover 8, automatic Tame 24, automatic Heat 12.
- Material that's already loud enough gets a requested gain of 0; when a limit is exceeded, **the target is lowered rather than forcing harder compression**.
- Automatic Tone can brighten (positive) or soften (negative): it's compared with the profile at equal weight, with a 0.35 dB/oct dead zone, an effective bandwidth, 0.15 dB/s following at full strength, and a style limit of ±0.75 dB × GLAZE (0.4 dB towards brighter with a House genre; see “Genre and Delivery” below). The style and loudness target aren't re-chosen because of short-term brightness.
- Auto style is no longer pushed wholesale toward Clean by a 60 s LRA confidence that hasn't filled up yet; LRA still constrains the dynamics features on its own.
- The parallel Heat's share of peak reduction is an optional contribution: when it falls short, it doesn't lock up a limiter that still has headroom.
- **Automatic Heat only takes on peaks it can genuinely shave (algorithm / constants version 8, 0.1.8.6)**: Heat is added for peak reduction only if, within its limit, it can shave at least 0.1 dB off this song's peaks; otherwise only the style baseline applies (12 for all three styles out of the box); full-strength automatic Heat drops from the old per-style 70 / 50 / 20 to about 12. Old projects store version-7 snapshots, which take the old branch by snapshot version and stay bit-identical; the new rule applies after Relearn.

## Genre and Delivery

From 0.1.8.8, GLAZE has two independent choices, and one principle behind them: **Genre changes only the tonal targets and the way of learning (plus, with a House genre, the beat-following limiter release and the out-of-phase hint) and carries no absolute loudness of its own; loudness belongs to Delivery; the dynamics budget (how much limiting, glue and clipping is allowed) still belongs to Style.** The reason is that the research found House's three sub-genres can't be told apart by their dynamics — they differ in spectrum — so Genre leaves the dynamics budget alone. Neither choice changes the signal chain (the same modules in the same order): they add constants and conditions to GLAZE's analyser, planner and controller, plus three things handed straight to the chain — the shape of Target spectrum on Auto, Streaming's −2 dBTP ceiling and, with a House genre, a beat-based cap on LIMIT's Auto release; the last two apply wherever the GLAZE knob is. General + Standard changes nothing.

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

### Listening with a House genre

In General, GLAZE listens for only 15–30 seconds from the start of playback. At 124 BPM in 4/4 a bar lasts 1.935 s, so that covers only about 7.8–15.5 bars, while a House intro often has 16–32 bars of drums only — GLAZE would set loudness and low end from an intro with no bass and no pads. With a House genre it works like this instead:

- **A tally kept bar by bar**: one entry = one bar, placed by the host's position and time signature; when no tempo can be had (no position, not playing, an invalid tempo, or a bar length outside 0.5–6.4 s) it falls back to one entry every 2 seconds. Entries are stored by position: replaying a bar overwrites the old value instead of counting twice, so jumping around the song gradually fills in all of it. At most 512 entries are kept (about 16.5 minutes at 124 BPM); once full, new ones are not added. At a constant tempo the result doesn't depend on the host's buffer size (any block size gives the same profile); a tempo change allows some deviation.
- **Gating, so only the main section reaches the profile**: a bar counts as main section only if it passes two gates. Loudness gate: the bar's loudness ≥ the reference loudness − 3 LU, where the reference loudness is the 95th percentile of the loudness of all bars heard (weighted by duration). Low-end gate: the bar's low-frequency share ≥ the low-end reference × 0.6, where the low-end reference is the 95th percentile of the low-frequency share of the bars that passed the loudness gate. The loudness, peak-to-loudness ratio, true peak, spectrum, low / high shares, attacks, stereo image, infrasound, the trial-run costs for CLAY and the loudness range (LRA) in the profile are all taken from the main section only; the effective duration and representativeness still use every bar heard. Both gates are relative: until the drop arrives, the loudest, fullest part so far (often the intro) counts as main section; when the drop arrives, the reference loudness and the low-end reference rise together, so most intro and breakdown bars stop counting as main section on their own and nothing has to be cleared and relearned.
- **The lock conditions**: at least 64 different bars heard, at least 8 bars of main section (one phrase), plus the same confidence conditions as General; counting in seconds, that is at least 124 s and at least 16 s. All three must hold before it calibrates and locks, and this path has no listening timeout.
- **The provisional plan**: on a first listen, until it locks, the plan is built from the main section so far. The profile's representativeness is capped at bars heard ÷ 64, and confidence takes the least certain item and counts as full at 0.8, so confidence can be at most bars heard ÷ 64 ÷ 0.8 and the discount ends at about 51 bars, which keeps the intro stage very cautious; there is no calibration in the meantime. It is recomputed each time the gating changes: normally over about 3 seconds, but when a new bar turns up that is at least 3 LU above the reference loudness heard so far and passes the low-end gate (usually the drop arriving), over about 0.5 seconds, with any transition already under way compressed to finish first. During Relearn, or the round started by switching from General, there is no provisional plan: the old plan stays in effect.
- **Keeping watch after the lock**: the tally continues. If 4 bars in a row each satisfy “bar loudness ≥ profile loudness + 3 LU and low-frequency share ≥ the profile's × 0.6”, those bars are folded into the gated profile, the plan switches over about 0.5 seconds, and it calibrates twice more. Each profile can be updated this way at most twice, only towards louder material, never towards lighter. The existing profile-mismatch protection (limiting the gain when the actual loudness is 10 LU or more above the profile for 1 second) stays as the last line of defence.
- **Relearn** clears the tally and starts over. **Reopening a project** doesn't restore the tally: it starts again from empty, the locked profile stays in effect, and the watch after locking only starts to work once 64 bars have been heard again.

Taking the intro out bar by bar can't be done perfectly: many DJ intros already have kick and bass, only 1–3 LU lighter than the drop, and a relative gate can't tell them apart. So three safeguards work together — the relative gate, at least 64 bars heard, and the watch after the lock — and acceptance looks at how representative the profile is (the gap between the gated profile's loudness and the drop's loudness), not at how many intro bars are blocked. A known bias: for a track whose drop comes very late and is very short, the profile at the time of locking can come out too light.

### Limiter release following the beat

In General the Auto release's slow stage runs between 100 and 800 ms, interpolated from the average gain reduction of the last 500 ms. With an average gain reduction of 1.5 dB, for example, the slow stage is about 190 ms and the gain takes about 0.56 s to recover — longer than the 0.48 s of one beat at 124 BPM, so the next kick arrives before the gain is back. With a House genre, the slow release becomes min(the adaptive value, max(100 ms, one beat ÷ 3)), with one beat = 60000 ÷ BPM (a quarter note): about 160 ms at 124 BPM.

It applies only when all of these hold: the genre is a House genre; Limit is on Auto release; the host tempo is valid and between 100 and 140 BPM; the host is playing. If any one fails, it behaves as in General. A manual Release and the fast release for short overloads (under 70 ms) are unaffected, and the GLAZE amount doesn't matter: choosing a House genre is choosing this limiter behaviour. The project tempo may differ from the audio's tempo (for example when mastering a finished track in an empty project at the default 120 BPM): the limit is only tightened to one beat ÷ 3, so a wrong tempo has limited consequences, and the plug-in doesn't track beats in the audio.

### The out-of-phase hint

CLAY's general out-of-phase lamp lights when the slow-path low-frequency correlation drops below −0.2. A House genre adds a more sensitive hint: when the slow-path low-frequency correlation in the main section stays below 0.6 for 4 bars (8 seconds when there's no tempo), the GLAZE status line and the CLAY card say how much low end mono playback would lose. A correlation of 0.6 corresponds to a mono bass loss of roughly 1 dB (with equal power left and right, mono power ∝ (1 + ρ) ÷ 2). Once the correlation rises back above 0.6, the hint stays for another 2 seconds (it goes out at once when the main section ends), and the loss figure in dB is computed by the formula when it lights, then smoothed over 1 second. It only tells you: it doesn't process the audio and doesn't apply Mono automatically — Mono only changes the stereo image and leaves mono playback as it is, so it can't rescue it, and the fix belongs in the mix. It stays off at GLAZE 0.

### Delivery

What follows gives the full-strength target at GLAZE 100; how far it actually pushes is further multiplied by GLAZE and by a confidence factor (0.35 + 0.65 × confidence, so 1 at full confidence), and the target is lowered if a module budget runs out. For Streaming and Club, target loudness = the setting's own loudness + the style offset − the loudness-range (LRA) correction, clamped into the setting's allowed range; Standard works as before — the blended style preference minus the LRA correction, clamped to −14 to −7 LUFS, with no style offset added (the preference already depends on the style):

| Delivery | Setting loudness | Allowed range | Effective Ceiling handed to the limiter |
|---|---|---|---|
| Standard | The blended style preference (−8.5 / −9 / −10 LUFS) | −14 to −7 LUFS | Your Ceiling − codec margin |
| Streaming | −10 LUFS | −12 to −8 LUFS | min(your Ceiling − codec margin, −2 dBTP) |
| Club | −7 LUFS | −9 to −5 LUFS | Your Ceiling − codec margin, never raised automatically |

- **Style offset** (Streaming and Club only) = the blended style preference − Punchy's preference (−9): Warm +0.5, Punchy 0, Clean −1, with Auto blending by weight. The **LRA correction** is unchanged (for each LU of loudness range above 6 LU, the target drops by 1/3 dB) and is subtracted before the clamp.
- **All three settings have a “minimum push” (3 dB at full strength)**: material that is already close to or above the setting's loudness still gets a gentle extra push. **For Streaming and Club, the top of the allowed range is a hard limit**: even this minimum push won't take the target past it (−8 LUFS for Streaming, −5 for Club), and material that is already louder than the limit isn't pushed at all; Standard has no such hard limit.
- **Streaming's −2 dBTP leaves margin for lossy transcoding**: the effective Ceiling handed to LIMIT is capped by it from the start, wherever the GLAZE knob is. For Club, the Ceiling is entirely yours and never raised automatically — there is no reliable basis for saying that club playback should use a higher ceiling than you set.
- **The target aims at the loud part**: the planner works out gain from “target − profile loudness”. In General the profile loudness is that of the stretch listened to after playback starts, as before; with a House genre it is that of the main section, so Club's −7 LUFS refers to the drop, not to the whole-song integral — which the intro and breakdown pull down, and which only appears, as a result, in OUT's integrated loudness after the song has been played through.

### Switching

Changing Delivery or Genre doesn't trigger a relearn: the plan is recomputed for the new target on the profile that has already locked, fades in over 3 seconds, and then calibrates twice more. Calibration starts from zero — the target has changed and so has the amount of limiting, and the old offset no longer describes the model error at the new working point — and the status line reads Calibrating as usual. With a House genre, and a profile learned with House gating, a calibration round only starts while the current bar counts as main section (the same width of 3 LU and factor of 0.6 as the gating, but measured against the committed profile rather than the tally's reference: bar loudness ≥ profile loudness − 3 LU and low-frequency share ≥ 0.6 × the profile's): elsewhere the status line reads “Locked · awaiting calibration” until the drop plays, and a round already under way stops collecting there while the status line stays on Calibrating. Two masters that differ only in Delivery share one profile and make the same Tone, BALANCE and CLAY decisions (switching Genre does change the tonal target, Low cut and Width); they differ in the target loudness and true-peak ceiling, and in the amount of Glue, Heat and Clip used to reach them (all three are shared out from the estimated excess, true peak + gain − Ceiling). A known limitation: if you export offline straight after switching, the start of the export may not be calibrated yet, so keep playing until the status line is back to “Locked · … · Target … LUFS” (not “Locked · awaiting calibration”; with a House genre that means playing through a drop) before exporting. Switching can start a listening round by itself in two cases: if a round of listening is already under way when you switch from General to a House genre, it starts over the House way (the bar count goes back to zero); and when nothing is being listened to, with a House genre whose existing profile wasn't learned with House gating (for example it was learned in General), GLAZE starts a House-style listening round and the old profile stays in effect until the new one is ready. A profile already learned with a House genre (including one learned in 2-second entries when the host gave no tempo) is kept, so going to General and back doesn't relearn.

## Calibration reference conditions

Any change invalidates it: manual parameters, style / genre / delivery, sample rate, algorithm and constants version, Advanced settings (the 6 Loud, not harsh / Target spectrum parameters, the “Mast” segment).

**Log-scale parameters are compared within a tolerance instead of an exact hash**: Limit Release and Clay's low cut / mono drift by a few ulps on a host round trip (one save-and-reopen = two normalisation round trips, at most 12 ulps; repeated round trips drift up to a relative 1.18e-5 and then settle at a fixed point), and the old `llround(x × 1e4)` rounding hash flipped at the boundaries, losing the calibration when a project was reopened. Now discrete values are hashed exactly and the three log-scale values are compared to a relative 1e-4; an old save that's read back is promoted on its first block within a ±16-ulp neighbourhood; the stability timer for “conditions unchanged for 1 s” compares against the conditions at the start of the stable period (tolerance comparison isn't transitive).

The manual-parameter part of the reference conditions is cached per input: a hit costs 6.6 ns, a recompute about 110 ns; at 192 kHz / 32 that saves about 0.1–0.2 percentage points per block.

## Execution snapshot and offline export

Alongside the committed profile, a separate `EXECUTION` snapshot is saved: the current actual plan, reference conditions, the calibration offset being applied, the protective back-off, and the running Tone / Tame increments. A candidate profile isn't a committed profile, and a calibration in progress isn't a valid calibration. The snapshot is invalidated if its parameters, sample rate or algorithm conditions don't match.

Real time and offline learn, calibrate and apply safety feedback on the same sample timeline; switching to offline neither stops learning nor withdraws a calibration offset that's in effect. A host reset recovers through the same entry point. What's guaranteed is reproducibility between real time and offline **for the same snapshot, the same input starting point and the same block timeline**: from the start of a 55-second identical playback, offline and real time match sample for sample; rendering three snapshots for 60 seconds with 64-sample real-time / 1024-sample offline blocks, the integrated loudness differs by ≤ 0.001181 LU.

## Guard statistics slots

The guard approximates a p95 over 10 ms slot means. The chain is sliced at the protection layer's slot boundaries so module statistics never span slots; a whole host block still uses one set of execution parameters, and new control values apply from the next host block.

The protection layer's timing follows music time only: while the host is stopped, during silence and during host bypass, its statistics and observation periods are frozen, so it never absorbs feedback by mistake. After every plan recomputation (a style switch, a commit, a relearn), the limits of the protection layer and the upstream monitor update with it. For items where your manual settings alone already exceed the style budget (Heat drive, Glue mean / p95, Clip depth / activity), the protection layer and upstream monitor no longer apply a constraint — the automatic part of that item is already 0, so checking it against the style limit would only cause pointless back-off; the other items are still checked against the style limits (all three since 0.1.8.6).

## Acceptance summary

1. With GLAZE held at 0 from initialisation, Genre on General and Delivery on Standard: the whole chain's output is bit-identical to the reference chain (90 scenarios).
2. The planner is a pure function: identical input gives bit-identical output; monotonic in GLAZE; never exceeds any module's limit; all increments are 0 at GLAZE 0.
3. Material that's already loud enough gets a requested gain of 0; when a limit is exceeded, the target is lowered.
4. Final output true peak ≤ Ceiling + 0.01 dB (as measured by the 8× meter).
5. Reopening a project: no relearning, and re-rendering from the same starting point is bit-for-bit reproducible.
6. Performance: with GLAZE on, the whole plugin at 48k / 512 costs ≤ 0.15% more, with the worst block ≤ 0.5 percentage points more; zero allocations and zero locks on the audio thread.
7. Synthetic music with only GLAZE = 50: a cumulative loudness increase of 6.47 LU; GLAZE 0 / 25 / 50 / 75 / 100 → −23.97 / −20.61 / −17.24 / −13.96 / −10.64 LUFS (one piece of material, not a guarantee for any song).

# GLAZE · decisions and following

“The big knob can't just turn every knob up — it has to get the result and raise the volume fully automatically, by algorithm, from the overall state of the music.” — GLAZE listens to the song as it plays and keeps deciding how to process it and how much loudness to push, based on its loudness, dynamics, spectrum and attacks; the knob only sets how far to push toward this song's ideal master.

## Five key choices

| Question | Decision |
|---|---|
| How it follows the music | It listens all the time: a live, main-section-weighted profile feeds the planner once a second, and the automatic increments move toward each new target under a rate limit and a ratchet, rising slowly and falling faster; it never waits through a passage before settling on a plan, and nothing locks |
| What the knob means | Strength: how far to push toward this song's ideal master; 0 = nothing moves (equal to the manual result) |
| Manual knobs | The algorithm's value is the floor and the knobs fine-tune it: actual value = clamp(manual value + automatic increment); it never automatically cancels out your changes |
| When the profile updates | All the time, and it's saved with the project (`LIVE`); there's no Relearn, and it doesn't guess song changes from the host transport — old statistics fade out through soft forgetting |
| Style | Pick one of four: Auto (default) / Warm / Punchy / Clean; Auto blends the weights of the three styles per song |

## Seven units

| Unit | Responsibility |
|---|---|
| **MusicAnalyzer + LiveProfile** | Input features (M / S / I / LRA, PLR / PSR, positive and negative spectral evidence, attacks, confidence) go into one live bar-by-bar tally; LiveProfile weights it by main-section likelihood and publishes the profile and c on schedule |
| **GlazePlanner** | A pure function: published profile + style + GLAZE + manual values + factory constants → target (7 automatic increments, Clay automatic values, target loudness, style weights) |
| **GlazeConstants** | Style preferences, module constraints, the Heat conversion table; no longer saved as a snapshot with the project |
| **Rate limit and ratchet** | Moves the applied increments toward the target: gain rises slowly and falls faster; while m < 0.5 the net gain can only fall |
| **G_trim + OutputLevelTracker** | A very slow output-side trim (at most 0.05 dB per second, within ±3 dB): it compares how much the plan means to change the level with how much the output actually differs from the input, both over the same main-section segments. It waits while the plan isn't in place yet, outside the main section and until it has 3 s of evidence, and it is frozen while the guard holds a relief or for 1 s after a parameter change. Replaces the calibrator |
| **Guard** GlazeGuard | The budget loop: when glue / limiting / clipping goes over the style's budget, or the limiter works too hard, it gives a relief that never adds level (at most 3 dB), added to Limit Gain, and eases it back to 0 once the condition clears. The cause (Budget / Limiter) and the amount on the status line come from it. From 0.1.8.8 to 0.1.8.10 there were also a contrast guard and a PSR guard (used only with House genres); they went with Genre in 0.1.8.11 |
| **GlazeController** | Continuous scheduling: recomputes or applies every block and writes the status snapshot; the project stores only `LIVE` |

![Continuous scheduling: the live profile feeds the planner every 1 s → rate limit and ratchet → output trim G_trim and guard relief](figure:glaze)

## What happens every block

1. If the knob or style has changed since the last block, the target is recomputed straight away from the profile currently published. At GLAZE ≤ 0 every target increment is 0, and the rate limiter, G_trim and guard relief drop to 0 in that block, so the parameters handed to the modules equal the manual values bit for bit.
2. Analysis: the input goes into the live tally; finished entries go into LiveProfile, which publishes a profile on schedule.
3. When analyser time crosses the 1 s grid, the target is recomputed from the newly published profile (once per block at most).
4. The rate limiter moves the applied increments toward the target, rising slowly and falling faster; while the current main-section likelihood m is below 0.5, the net gain can only fall.
5. G_trim applies a very slow trim; its error is the plan's net gain minus the measured net gain (output minus input loudness over the same main-section segments). The history of about the last 128 segments (one bar each when the host gives a tempo, about four minutes at 124 BPM) is referred to the current Limit Gain from the gain each segment was recorded at, and judged again against the current profile to see which segments still count as main sections; segments recorded more than 1 dB away from the current gain count for less, and more than 3 dB away not at all. Blocks in which the automatic increments are still moving toward the plan are neither evidence nor integrated.
6. The guard's relief is added to Limit Gain. True peak is still guaranteed by the limiter and the Ceiling.
7. The status snapshot is written (the six status lines read only this).

There is no listen-then-settle phase, plan transition, calibration or lock. From a cold start c = 0 and the planner uses its cautious low-confidence plan (shown as Warming up in the interface); c rises with the quality of the main section heard.

## The planner's targets and constraints

- requestedTarget: the loudness that the style and GLAZE want (the blended style preference, −8.5 / −9 / −10 LUFS, minus 1/3 dB for each LU of loudness range above 6 LU, clamped to −14 to −7 LUFS); effectiveTarget: the loudness currently allowed after module constraints (the input loudness measured by the profile + the plan's net gain), which the “Target” on the status line uses. The output trim G_trim uses only the plan's net gain (step 5 above): the profile's input loudness and the output statistics don't cover the same segments, so comparing against the target loudness directly would chase their difference as if it were an error.
- **One-knob baseline** (a full-strength request, then scaled by GLAZE, confidence, style limits and module budget): a minimum loudness-increase request of 3 dB (kept even for songs already at the preferred loudness), automatic Recover 8, automatic Tame 24, automatic Heat 12.
- Material that's already loud enough still keeps that minimum push (GLAZE only pushes up, it never turns down), with no hard upper limit. When a limit is exceeded, **the target is lowered rather than forcing harder compression**.
- **The target aims at the loud part**: the planner works out gain from “target − profile loudness”, and the profile loudness is that of the main section recognised by the loudness model (the loudest parts), not the whole-song integral — which the intro and breakdown pull down, and which only appears, as a result, in OUT's integrated loudness after the song has been played through.
- Automatic Tone can brighten (positive) or soften (negative): it's compared with the profile at equal weight, with a 0.35 dB/oct dead zone, an effective bandwidth, 0.15 dB/s following at full strength, and a style limit of ±0.75 dB × GLAZE. The style and loudness target aren't re-chosen because of short-term brightness.
- Auto style is no longer pushed wholesale toward Clean by a 60 s LRA confidence that hasn't filled up yet; LRA still constrains the dynamics features on its own.
- The parallel Heat's share of peak reduction is an optional contribution: when it falls short, it doesn't lock up a limiter that still has headroom.
- **Automatic Heat only takes on peaks it can genuinely shave (algorithm / constants version 8, 0.1.8.6)**: Heat is added for peak reduction only if, within its limit, it can shave at least 0.1 dB off this song's peaks; otherwise only the style baseline applies (12 for all three styles out of the box); full-strength automatic Heat drops from the old per-style 70 / 50 / 20 to about 12. An old project's constants snapshot is discarded on load, and the plan always uses this version's factory constants.

## The main section

The live tally: one entry = one bar, placed by the host's position and time signature, falling back to one entry every 2 seconds when there's no tempo; replaying a bar overwrites the old value rather than counting it twice; at most 512 entries are kept. The main section is recognised by a loudness model (a soft gate on each part's loudness relative to the reference). The profile is weighted by main-section likelihood, so intros and breakdowns count for little; while the current part has m < 0.5, the ratchet lets the net gain fall but never rise, so light parts aren't pushed up. There are no lock conditions, provisional plan or watch after locking.

## Genre and Delivery (removed in 0.1.8.11)

From 0.1.8.8 to 0.1.8.10 there were two extra choices: Genre (General / Deep / Tech / Melodic; the three House genres changed the tonal target, Low cut, Width and the main-section model, and added a beat-following limiter release, an out-of-phase hint and the contrast guard) and Delivery (Standard / Streaming / Club; changed the target loudness, and Streaming capped the limiter ceiling at −2 dBTP). 0.1.8.11 removed both, with their constants and guards, leaving only the path General + Standard always took; that path sounds the same as before. An old project saved with any other setting loads as General + Standard, the interface says so once, and the sound changes.

## Saving and offline export

GLAZE saves a single `GLAZE_STATE` (format 2) holding `LIVE`: the live profile window (`LIVE_PROFILE`), the rate limiter's current increments, G_trim, the Auto style weights, the guard, the output-level statistics and CLAY's automatic values, wrapped in zlib + base64 and lossless. From the first block after loading, these control values equal what was saved, bit for bit; filters and any windows not stored in `LIVE` start from zero. An old project's `GLAZE_PROFILE` / `PLAN` / `CALIBRATION` / `EXECUTION` / `GLAZE_CONSTANTS` nodes are discarded on load and treated as a cold start, and the interface says so once; the contrast and PSR guard state saved by 0.1.8.10 is dropped on load, and the rest of `LIVE` loads as usual. A host reset, stopping or seeking doesn't clear `LIVE`. Offline and real time share one processing path; every export keeps updating the statistics, so repeated exports aren't guaranteed to be bit-identical.

## Guard statistics slots

The guard approximates a p95 over 10 ms slot means. The chain is sliced at the protection layer's slot boundaries and 100 ms sub-block boundaries (whichever comes first) so module statistics never span slots; a whole host block still uses one set of execution parameters, and new control values apply from the next host block.

The protection layer's timing follows music time only: while the host is stopped, during silence and during host bypass, its statistics and observation periods are frozen, so it never absorbs feedback by mistake. After every target recomputation (once a second, or on a change of style), the limits of the protection layer and the upstream monitor update with it. For items where your manual settings alone already exceed the style budget (Heat drive, Glue mean / p95, Clip depth / activity), the protection layer and upstream monitor no longer apply a constraint — the automatic part of that item is already 0, so checking it against the style limit would only cause pointless back-off; the other items are still checked against the style limits (all three since 0.1.8.6).

## Acceptance summary

1. With GLAZE held at 0 from initialisation: the whole chain's output is bit-identical to the reference chain (90 scenarios).
2. The planner is a pure function: identical input gives bit-identical output; monotonic in GLAZE; never exceeds any module's limit; all increments are 0 at GLAZE 0.
3. Material that's already loud enough keeps the minimum push (the request is never below it); when a limit is exceeded, the target is lowered.
4. Final output true peak ≤ Ceiling + 0.01 dB (as measured by the 8× meter).
5. Save round trip: from the first block after loading, the control values (applied increments, G_trim, the guard, Clay automatic values, the published profile) equal what was saved, bit for bit; repeated exports aren't guaranteed to be bit-identical.
6. Performance: with GLAZE on, the whole plugin at 48k / 512 costs ≤ 0.15% more, with the worst block ≤ 0.5 percentage points more; zero allocations and zero locks on the audio thread.
7. Synthetic music with only GLAZE = 50: a cumulative loudness increase of 6.47 LU; GLAZE 0 / 25 / 50 / 75 / 100 → −23.97 / −20.61 / −17.24 / −13.96 / −10.64 LUFS (one piece of material, not a guarantee for any song).

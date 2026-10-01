“The big knob can't just turn every knob up — it has to get the result and raise the volume fully automatically, by algorithm, from the overall state of the music.” — GLAZE listens to the song first and decides how to process it and how much loudness to push based on its loudness, dynamics, spectrum and attacks; the knob only sets how far to push toward this song's ideal master.

## Five key choices

| Question | Decision |
|---|---|
| How it follows the music | Listen to a stretch first to establish a whole-song baseline (profile → plan), then make only bounded protective adjustments around that baseline |
| What the knob means | Strength: how far to push toward this song's ideal master; 0 = nothing moves (equal to the manual result) |
| Manual knobs | The algorithm's value is the floor and the knobs fine-tune it: actual value = clamp(manual value + automatic increment); it never automatically cancels out your changes |
| When the baseline is built | Learned automatically + saved with the project + a Relearn button; it never relearns on its own |
| Style | Pick one of four: Auto (default) / Warm / Punchy / Clean; Auto blends the weights of the three styles per song |

## Seven units

| Unit | Responsibility |
|---|---|
| **MusicAnalyzer** | Input features: M / S / I / LRA, PLR / PSR, positive and negative spectral evidence (reusing BALANCE's 27-band frames), attack activity, confidence → profile |
| **GlazePlanner** | A pure function: profile + style + GLAZE + manual values + constants snapshot → plan (7 automatic increments, Clay automatic values, target loudness, style weights) |
| **GlazeConstants** | Every constant and conversion table, versioned (style preferences, module constraints, the Heat conversion table) |
| **GlazeCalibrator** | Has an end condition: matches the input-to-output loudness difference with direction-aware anti-saturation; not a continuous closed loop |
| **GlazeGuard** | Backs off slowly when a constraint keeps being exceeded, g_guard ∈ [−3, 0] dB; only takes back automatic gain |
| **GlazeProfile** | Saving / loading the committed version (profile + plan + calibration + snapshot) |
| **GlazeController** | The state machine, candidate / committed versions, seeking / reset / offline; produces the actual parameters every block |

![State machine: Not learned → Listening → Calibrating → Locked ⇄ Limited; Relearn goes through a candidate version](figure:glaze)

## Timeline

1. **Not learned**: listening starts automatically on playback. Until then, a gentle low-confidence plan is used.
2. **Listening**: 15–30 s, ending once enough confidence has been gathered (`minListenSeconds 15 / maxListenSeconds 30`). The previous plan (or the gentle one) stays in effect.
3. **Plan transition**: the new plan crossfades in over about 3 s.
4. **Calibrating**: the calibrator brings the measured loudness difference to effectiveTarget; with unchanged reference conditions it catches up at 1 dB/s. If a calibration round is abandoned (GLAZE briefly off, LIMIT bypassed, a style switch and so on), a new round starts once the conditions are back as they were, so it never stays uncalibrated (0.1.8.6).
5. **Locked**: from here on there are only two paths of change — the protection layer's slow back-off, and section following (bounded pull-back of the automatic Heat / Glue shares).
6. **Relearn**: builds a candidate version and crossfades it into the committed one when finished.

## The planner's targets and constraints

- requestedTarget: the loudness the style and GLAZE want; effectiveTarget: the loudness currently allowed after module constraints — calibration only uses this one.
- **One-knob baseline** (a full-strength request, then scaled by GLAZE, confidence, style limits and module budget): a minimum loudness-increase request of 3 dB (kept even for songs already at the preferred loudness), automatic Recover 8, automatic Tame 24, automatic Heat 12.
- Material that's already loud enough gets a requested gain of 0; when a limit is exceeded, **the target is lowered rather than forcing harder compression**.
- Automatic Tone can brighten (positive) or soften (negative): it's compared with the profile at equal weight, with a 0.35 dB/oct dead zone, an effective bandwidth, 0.15 dB/s following at full strength, and a style limit of ±0.75 dB × GLAZE. The style and loudness target aren't re-chosen because of short-term brightness.
- Auto style is no longer pushed wholesale toward Clean by a 60 s LRA confidence that hasn't filled up yet; LRA still constrains the dynamics features on its own.
- The parallel Heat's share of peak reduction is an optional contribution: when it falls short, it doesn't lock up a limiter that still has headroom.
- **Automatic Heat only takes on peaks it can genuinely shave (algorithm / constants version 8, 0.1.8.6)**: Heat is added for peak reduction only if, within its limit, it can shave at least 0.1 dB off this song's peaks; otherwise only the style baseline applies (12 for all three styles out of the box); full-strength automatic Heat drops from the old per-style 70 / 50 / 20 to about 12. Old projects store version-7 snapshots, which take the old branch by snapshot version and stay bit-identical; the new rule applies after Relearn.

## Calibration reference conditions

Any change invalidates it: manual parameters, sample rate, algorithm and constants version, Advanced settings (the 6 Loud, not harsh / Target spectrum parameters, the “Mast” segment).

**Log-scale parameters are compared within a tolerance instead of an exact hash**: Limit Release and Clay's low cut / mono drift by a few ulps on a host round trip (one save-and-reopen = two normalisation round trips, at most 12 ulps; repeated round trips drift up to a relative 1.18e-5 and then settle at a fixed point), and the old `llround(x × 1e4)` rounding hash flipped at the boundaries, losing the calibration when a project was reopened. Now discrete values are hashed exactly and the three log-scale values are compared to a relative 1e-4; an old save that's read back is promoted on its first block within a ±16-ulp neighbourhood; the stability timer for “conditions unchanged for 1 s” compares against the conditions at the start of the stable period (tolerance comparison isn't transitive).

The manual-parameter part of the reference conditions is cached per input: a hit costs 6.6 ns, a recompute about 110 ns; at 192 kHz / 32 that saves about 0.1–0.2 percentage points per block.

## Execution snapshot and offline export

Alongside the committed profile, a separate `EXECUTION` snapshot is saved: the current actual plan, reference conditions, the calibration offset being applied, the protective back-off, and the running Tone / Tame increments. A candidate profile isn't a committed profile, and a calibration in progress isn't a valid calibration. The snapshot is invalidated if its parameters, sample rate or algorithm conditions don't match.

Real time and offline learn, calibrate and apply safety feedback on the same sample timeline; switching to offline neither stops learning nor withdraws a calibration offset that's in effect. A host reset recovers through the same entry point. What's guaranteed is reproducibility between real time and offline **for the same snapshot, the same input starting point and the same block timeline**: from the start of a 55-second identical playback, offline and real time match sample for sample; rendering three snapshots for 60 seconds with 64-sample real-time / 1024-sample offline blocks, the integrated loudness differs by ≤ 0.001181 LU.

## Guard statistics slots

The guard approximates a p95 over 10 ms slot means. The chain is sliced at the protection layer's slot boundaries so module statistics never span slots; a whole host block still uses one set of execution parameters, and new control values apply from the next host block.

The protection layer's timing follows music time only: while the host is stopped, during silence and during host bypass, its statistics and observation periods are frozen, so it never absorbs feedback by mistake. After every plan recomputation (a style switch, a commit, a relearn), the limits of the protection layer and the upstream monitor update with it. For items where your manual settings alone already exceed the style budget (Heat drive, Glue mean / p95, Clip depth / activity), the protection layer and upstream monitor no longer apply a constraint — the automatic part of that item is already 0, so checking it against the style limit would only cause pointless back-off; the other items are still checked against the style limits (all three since 0.1.8.6).

## Acceptance summary

1. With GLAZE held at 0 from initialisation: the whole chain's output is bit-identical to the reference chain (90 scenarios).
2. The planner is a pure function: identical input gives bit-identical output; monotonic in GLAZE; never exceeds any module's limit; all increments are 0 at GLAZE 0.
3. Material that's already loud enough gets a requested gain of 0; when a limit is exceeded, the target is lowered.
4. Final output true peak ≤ Ceiling + 0.01 dB (as measured by the 8× meter).
5. Reopening a project: no relearning, and re-rendering from the same starting point is bit-for-bit reproducible.
6. Performance: with GLAZE on, the whole plugin at 48k / 512 costs ≤ 0.15% more, with the worst block ≤ 0.5 percentage points more; zero allocations and zero locks on the audio thread.
7. Synthetic music with only GLAZE = 50: a cumulative loudness increase of 6.47 LU; GLAZE 0 / 25 / 50 / 75 / 100 → −23.97 / −20.61 / −17.24 / −13.96 / −10.64 LUFS (one piece of material, not a guarantee for any song).

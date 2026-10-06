GLAZE doesn't turn every knob up at once. It **listens to the song as it plays and keeps deciding how to process it and how much loudness to push, based on its loudness, dynamics, spectrum and attacks**; the knob only sets how far to push toward this song's ideal master.

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

GLAZE listens all the time while the song plays: loudness (M / S / I / LRA), peak-to-loudness ratio, positive and negative spectral evidence, attack density… weighted towards the **main section** (the loudest, fullest parts) to build a **profile** (saved with the project). Profile + style + Genre + Delivery + GLAZE + manual values → a **plan**, recomputed every second from the latest profile; the automatic increments rise slowly and fall faster as they move toward the new plan, and in lighter sections such as an intro or breakdown the gain can only fall, never rise. Until it has heard enough of the main section the plan stays cautious, and it relaxes as it hears more. A very slow trim at the output brings the measured loudness to the target, and a set of guards takes gain back when a constraint is exceeded. There is no listening phase and nothing locks. With a House genre (Deep / Tech / Melodic) the main section is recognised the four-on-the-floor way; see [How House genres follow](#how-house-genres-follow).

The stage label beside the GLAZE heading matches the status line at the top; only one shows at a time, the first of these that applies:

| Label | Meaning |
|---|---|
| Manual limit | Orange; the status line reads “Limited by manual settings”: your manual settings alone already exceed a module's budget (or LIMIT is bypassed), so the matching automatic increment is withdrawn. Not shown at GLAZE 0 |
| Phase | Amber. The status line reads “Club mono loses low end (about x dB), fix it in the mix”. House genres only; see [Genre](#genre) |
| Export | Amber. The status line reads “Main-section stats are still cold at export, so the first drop may come out quiet: play through once, then export”. An offline export started before enough of the main section had been heard |
| Limited | Orange; the status line reads “Limited · cause · backed off x dB”: during playback a guard is pulling GLAZE's automatic gain back, or has reached its floor (−3 dB) and the problem persists. The cause is one of Budget (glue / limiting / clipping over the style's budget), Delivery peak (the limiter working too hard), Contrast (the master's loudness movement has narrowed too much compared with the mix) or PSR (peak-to-loudness ratio below the delivery's floor). Once the condition clears it eases back and the status returns to Following. Not shown at GLAZE 0 |
| Starting | The status line reads “Starting · main section not heard yet (staying conservative)”. Not enough of the main section heard yet (just started, or still in the intro): the plan stays cautious and relaxes as it hears more |
| Holding | The status line reads “Holding · this section is quieter”. A lighter section is playing: gain can only fall, never rise, until the main section returns |
| Following | The status line reads “Following · Auto→Warm · Target −9.6 LUFS”. The usual state: following the profile built so far; with a House genre it starts with “House ·” |
| Manual | The status line reads “GLAZE 0 · manual settings only”. GLAZE is at 0; none of the lines above appear |

> The profile builds up over the whole song: it stays cautious through the intro, relaxes once the main section arrives, and lighter sections never pull the gain up. Moving on to another song needs no button; the old song's statistics fade out as the new one plays.

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
| **Genre** | What kind of music is this? Sets the tonal target, and how GLAZE recognises the main section | General (default) / Deep / Tech / Melodic |
| **Delivery** | Where is this version going? Sets the target loudness and the true-peak ceiling | Standard (default) / Streaming / Club |
| **Style** | Where should the loudness be spent? Sets how much goes to limiting, glue and colour | Auto (default) / Warm / Punchy / Clean |

The default is General + Standard: the main section is recognised by loudness, the tonal target and the loudness are both set by style, and your Ceiling is kept. Old projects have no such settings and load as General + Standard; the listening result, plan and calibration saved in an old project are discarded on load, so GLAZE starts following from scratch and the sound will change. Genre and Delivery are saved with the project but can't be automated in the host — they are decisions about how this song gets mastered, not moves that change over time.

### Genre

| Genre | When to pick it |
|---|---|
| **General** (default) | Not House, or you're not sure: recognises the main section by loudness and sets the tonal target by style |
| **Deep** | Deep House |
| **Tech** | Tech House |
| **Melodic** | Melodic-Progressive (Melodic House and Progressive House both fit) |

The selector shows only Deep / Tech / Melodic; the tooltip gives the full names Deep House / Tech House / Melodic-Progressive. Pick the House type you wrote, or the closest one; use General for anything else.

With Deep, Tech or Melodic (called “House genres” below), six things change:

- **Tonal target**: automatic Tone (on COLOR) aims at a House reference and has less room to brighten than in General. With Target spectrum under Advanced on Auto, the target shape follows the genre instead of the Style: Deep leans warm, Tech is neutral and Melodic sits between the two. Every other setting is shared by the three sub-genres, so this target shape is the only difference between them — with Target spectrum Off (old projects load with it Off) or on a fixed shape, Deep, Tech and Melodic behave identically.
- **Low cut**: CLAY's automatic Low cut aims at 25 Hz (General uses 16–20 Hz, depending on the style). Whether it cuts at all is still decided by whether the song has infrasound, and it has to pass a trial first: if the cut would raise peaks or thin the low end, it is reduced or skipped.
- **Width**: CLAY's Auto never widens, so the master isn't pulled wider. Manual Width still works.
- **Main section**: recognised the four-on-the-floor way (full-arrangement parts with both kick and bass, such as the drop), and the profile and target loudness are taken mainly from those parts; the status line puts “House ·” in front of Following. Details are at the end of this section, under [How House genres follow](#how-house-genres-follow).
- **Limiter release**: LIMIT's Auto release is tightened to the host tempo: the slow stage of the release is kept to no more than a third of a beat (about 160 ms at 124 BPM). This applies only when Release is on Auto, the host is playing and its tempo is between 100 and 140 BPM, wherever the GLAZE knob is (including 0); a manual Release is unaffected.
- **Out-of-phase hint**: when the lows in the main section are out of phase and mono playback would lose bass, you're told with a more sensitive test than in General (a mono loss from about 1 dB counts, but it has to persist for 4 bars of main section, or 8 seconds when the host gives no tempo). The GLAZE status line in the status pill at the top switches to this warning in amber, “Club mono loses low end (about x dB), fix it in the mix”, and while it's lit it takes over the status line and the stage label beside the GLAZE heading reads Phase (only “Limited by manual settings” ranks above it); and the CLAY card's mini graph is replaced by two lines, “Club mono loses low end (about x dB)” and “Fix it in the mix”. It only tells you — it doesn't touch the audio — and it stays off at GLAZE 0. Applying Mono can't help mono playback; narrow the lows or align their phase in the mix.

### Delivery

Delivery decides how loud this version should be and how much true-peak margin it keeps:

| Delivery | Target loudness | True-peak ceiling | When to pick it |
|---|---|---|---|
| **Standard** (default) | Set by the style | Your Ceiling (minus any codec margin) | Everyday use, or when you haven't decided where it's going |
| **Streaming** | About −10 LUFS | At or below −2 dBTP; if your Ceiling (minus any codec margin) is lower, that wins | The version you upload to a streaming service |
| **Club** | About −7 LUFS | Your Ceiling (minus any codec margin); never raised automatically | The version for DJs and download stores |

- **The loudness in the table is roughly what GLAZE aims for with the knob at 100.** The knob still decides how far to push toward it, and the default 35% pushes only part of the way; to get closer, turn GLAZE up. The “Target … LUFS” on the status line is the number it is actually chasing right now. For a mix that is already close to or above the table's loudness, see the third point below.
- **Style still counts in Streaming and Club**: the target moves up or down around the setting's loudness with the style (Auto blends by weight), and the gaps between the three styles are the same as in Standard. Songs with a wide loudness range get a slightly lower target, exactly as in Standard.
- **GLAZE only pushes up; it never turns down a mix that is already loud enough.** If the mix is already louder than the target, the master isn't pulled back to it. All three settings keep a “minimum push”: when a mix is already close to or above the table's loudness, GLAZE still pushes a little more, so with the knob at 100 the target can end up louder than the table's number. Streaming and Club also have a hard upper limit (about −8 and −5 LUFS) that even this minimum push won't take them past, and a mix that is already louder than that limit isn't pushed at all; Standard has no such hard limit.
- **The target is a direction, not a guarantee.** Dynamics protection comes before loudness: once each module's budget is used up GLAZE stops pushing and the target comes down, so the “Target” on the status line reads lower than it would without that limit.
- **Streaming's −2 dBTP is the effective ceiling handed to LIMIT**, wherever the GLAZE knob is (including 0). It leaves extra true-peak margin for lossy transcoding, so with Streaming selected there's no need to pull the Ceiling down yourself. The other two settings leave the ceiling alone.
- **With a House genre, the target loudness is that of the main section (the drop)**, not the integrated loudness of the whole song. The intro and breakdown pull the latter down, and you only see it in OUT's integrated reading after playing the song through (see [Meters and spectrum analyser](/docs/meters)). With General it refers to the loudness of the main section recognised by loudness (the loudest parts).

### Switching, and exporting two masters

- **Switching needs nothing from you.** After a change of Delivery or Genre, GLAZE recomputes the target on the current profile straight away and moves toward it by rising slowly and falling faster. After a Genre switch the target is recomputed for the new genre and eases toward it. If you switch Genre during playback, the way the main section is recognised changes only from the next time playback starts. Two versions that differ only in Delivery share one profile and make the same Tone, BALANCE and stereo clean-up decisions (switching Genre does change the tonal target, Low cut and Width); they differ in how much gain is pushed and how much true-peak margin is left, and the Glue, Heat and Clip amounts are shared out again for that gain and ceiling.
- **Going from General to a House genre**: the plan is recomputed for the new genre straight away; the statistics gathered so far are kept, and no separate listening round starts. If you switch Genre during playback, the way the main section is recognised changes only from the next time playback starts.
- **To get two masters of one song**:
  1. Pick the Genre and play the song through once so the status line reaches “Following · … · Target … LUFS”. Turn GLAZE to the amount you want.
  2. Set Delivery to Streaming, play from the drop and keep playing until the “Target … LUFS” reading settles and the status line is back on Following, then export this version. If it reads Limited along the way, keep playing: the guard pulls the automatic gain back by itself (at most 3 dB) and eases back once the condition clears. With a House genre's out-of-phase hint lit, the status line is taken over by the hint; wait for the stage label to read Following.
  3. Set Delivery to Club, play from the drop again until the “Target … LUFS” reading settles and the status line is back on Following, then export the second version.

If you export offline straight after switching, the gain may still be moving toward the new target, so play until the “Target … LUFS” reading settles first. If you export offline before enough of the main section has been heard, the status line reads “Main-section stats are still cold at export, so the first drop may come out quiet: play through once, then export”. Every export keeps updating the statistics, so two exports of the same project in a row aren't guaranteed to be bit-identical. Before exporting, turn off Match and Normalisation preview; see [Monitoring](/docs/listening#before-you-export).

### How House genres follow

A House intro often has 16–32 bars of drums only, with no bass or pads. GLAZE doesn't listen to a stretch first and then decide; it keeps a tally bar by bar and weights each part by how much it sounds like the main section: full-arrangement parts with both kick and bass (such as the drop) count heavily, intros and breakdowns lightly. So:

- Just after you press play, while the intro is still running, the status line reads “Starting · main section not heard yet (staying conservative)” and the plan stays cautious; it relaxes once the drop arrives and enough of the main section has been heard.
- In lighter parts such as an intro or breakdown it reads “Holding · this section is quieter”: the gain can only fall, never rise, so light parts aren't pushed up.
- The usual state reads “House · Following · Auto→Warm · Target −9.6 LUFS”.
- Jumping around is fine: replaying a bar only overwrites it rather than counting it twice. When the host gives no tempo, it counts in seconds instead.
- To make it relax sooner, play through from the drop once.

### How House genres listen

See [How House genres follow](#how-house-genres-follow).

## How it relates to Advanced and CLAY

- With “Loud, not harsh” and Target spectrum on Auto: strength = the amount you set × GLAZE / 100. At GLAZE 0 they're zero.
- CLAY's Low cut and Width on Auto are decided by the plan and do nothing at GLAZE 0; Mono on Auto never folds the lows, it only lights an amber warning when they are out of phase (at GLAZE 0 even the warning stays off); on Manual they work even at GLAZE 0; Off never processes.
- Whenever an Advanced setting changes (or a manual parameter, the style, the genre or the delivery does), GLAZE recomputes the target straight away and moves toward it by rising slowly and falling faster.

## Saving and reopening

What GLAZE has gathered (the bar tally, the profile, the automatic increments being applied, the output trim, each guard's relief and CLAY's automatic values) is saved with the project, and after reopening it carries on from where it was saved instead of starting over. Offline export and real-time playback run through the same processing path and the statistics keep updating during an export, so two exports of the same project in a row aren't guaranteed to be bit-identical. Genre and Delivery are saved with the project too. Projects saved by earlier versions: the old listening result, plan and calibration are discarded on load, so GLAZE starts following from scratch and the sound will change; the bottom-left corner says so once when the project opens: “The old listening result was discarded, so the sound will change”.

For the algorithm details: [GLAZE · decisions and following](/algorithms/glaze).

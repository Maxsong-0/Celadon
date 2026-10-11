LIMIT is a transparent lookahead true-peak limiter: 8× interpolation sees the peaks between samples, and gain is applied at 1×. It never exceeds the true-peak ceiling, never clicks, doesn't pump and keeps the image steady. An optional Clip stage of soft clipping comes first.

## Controls

| Control | What it does | Where to start |
|---|---|---|
| **Gain** 0 to +24 dB | Adds gain into the limiter: the more you add, the louder it gets and the deeper the GR meter goes. | Raise it slowly from 0; occasional 2–3 dB of GR is plenty. Reaching −8 LUFS usually takes +6 to +10 dB. |
| **Ceiling** −3 to 0 dBTP | The limiter keeps the output below this true-peak level (dBTP); other meters may read slightly differently. | With Delivery on Streaming, the ceiling handed to the limiter is kept at or below −2 dBTP, so there's no need to lower it; on Standard or Club it follows your setting: −1.0 is common, −1.5 to be extra safe. |
| **Clip** 0–100 | Gently rounds off peaks before the limiter, so it has less to do at the same loudness. | 0–30 is safe; above 50 it starts to sound grainy. |
| **Release** 10–1000 ms | How fast the gain comes back after limiting: short is louder but can distort, long is smoother but can pump. Ignored while Auto is on. | Keep Auto; when setting it manually, start at 100 ms. |
| **AUTO release** | Two-stage fast / slow release that also follows the period of sustained bass: drums recover quickly, lows stay smooth. With a House genre, the slow stage is also kept to no more than a third of a beat at the host tempo; see [Genre and Delivery](/docs/glaze#genre-and-delivery). | Leave it on most of the time. |
| **Lookahead** 1–10 ms | How early the limiter sees peaks coming: longer is more transparent but adds latency. | 2 ms; songs with dense drums can try 3–5 ms. |
| **LIMIT GR** | How many dB the limiter is reducing right now. Turns orange while the guards are backing off (“Limited · …” on the status line). | Staying above 6 dB for long means Gain is pushed too far. |
| **Power button** | Turns off only the limiter; latency stays the same and switching is click-free. | Use it to compare before and after limiting. |

## Headphones · Listen to difference

The headphones to the left of the card's power button: they replace the output with what LIMIT takes away (gain, clip and limiting together, at the level before the gain push, never louder than the original); meters, loudness and GLAZE keep measuring the master.

Use it to hear whether anything important is being shaved off (flattened drums, dull bass). It turns off when you switch to the original, bypass LIMIT or close the window, and can't be turned on while LIMIT is bypassed. It has no effect on an offline export, which renders the master; exit it before a real-time export.

## A few promises

- Output true peak ≤ Ceiling + 0.01 dB (as measured by the plugin's own 8× true-peak meter), with an extra 0.35 dB of reconstruction headroom reserved where limiting gain is needed; under a 16× reference detector, the worst case on broadband material still stays below the Ceiling.
- When it isn't limiting, gain is exactly 1.0; after limiting it returns exactly to 1.0.
- Gain 0, Clip 0 and input peaks below the Ceiling → the output equals the input exactly.
- Codec margin (Advanced → Release safety) lowers the Ceiling handed to the limiter by a further 0–1.5 dB.
- With Delivery set to Streaming, the Ceiling handed to the limiter is also capped at −2 dBTP (if your Ceiling minus any codec margin is lower, that wins), wherever the GLAZE knob is. With Streaming selected you don't need to pull the Ceiling down yourself. See [Genre and Delivery](/docs/glaze#genre-and-delivery).

> ⚠ If you deliver with the Ceiling at 0 dBTP, leave some margin: other meters, resampling and lossy encoding can all show a true peak slightly higher than what's read here.

Algorithm details: [LIMIT · true-peak limiting](/algorithms/limit).

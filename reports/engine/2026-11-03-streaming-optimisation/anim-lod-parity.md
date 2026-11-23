# NPC day parity: A vs B

- A: `/tmp/anim-lod/day-off` — tier `auto`, seed 73106, isolate True, 1440 min every 5, wall 1164 s, end `done`
- B: `/tmp/anim-lod/day-on3` — tier `auto`, seed 73106, isolate True, 1440 min every 5, wall 971 s, end `done`
- NPC-samples compared: 304028 (1052 NPCs, up to t=1440 min); only in A: 0, only in B: 0
- A ends at t=1440 of 1440 requested minutes, `done`
- B ends at t=1440 of 1440 requested minutes, `done`

| Metric | Value |
|---|---|
| same ZS state | 100.00% |
| same routine (TA) entry | 100.00% |
| same HP | 100.00% |
| same inventory | 100.00% |
| position error p50 / p95 / max, m | 0.00 / 0.00 / 0.0 |
| ZS starts matched (±60 min) | 100.00% of 11568 |
| all raw ZS starts matched (including final post-sample tick) | 100.00% of 11574 |
| mutual-nearest raw ZS starts matched | 100.00% |
| mutual-nearest raw ZS start offset p50 / p95 / max, game s | 0.000 / 0.000 / 0.000 |
| matched ZS start offset p50 / p95 / max, game min | 0.00 / 0.00 / 0.0 |
| deaths A / B; matched pairs | 8 / 8; 8 |
| NPCs missing in one run at the end | 0 |
| alive differs at the end (missing counts) | 0 |
| inventory differs at the end (missing counts) | 0 |
| HP differs at the end (missing counts) | 0 |
| NPCs with any ZS difference or >5 m apart | 0 of 1052 |
| first ZS difference, game min | — |

## Tolerances

| Check | Value | Limit | |
|---|---|---|---|
| both runs finished the journal (end_reason done) | yes | required | pass |
| both runs reach their requested minutes and end at the same time | yes | required | pass |
| NPC-samples present in only one run | 0 | ≤ 0 | pass |
| NPCs present at the end of one run and missing in the other | 0 | ≤ 0 | pass |
| same routine (TA) entry, share of NPC-samples | 1.0000 | ≥ 0.9950 | pass |
| same ZS state, share of NPC-samples | 1.0000 | ≥ 0.9900 | pass |
| position error p95 when stationary in both runs, m | 0.0000 | ≤ 1.0000 | pass |
| ZS starts matched within the window, share | 1.0000 | ≥ 0.9900 | pass |
| all raw ZS starts matched within the window, share | 1.0000 | ≥ 0.9900 | pass |
| mutual-nearest raw ZS start offset p95, game seconds | 0.0000 | ≤ 8.0000 | pass |
| NPCs alive in one run and dead (or missing) in the other at the end | 0 | ≤ 0 | pass |
| NPCs whose HP differs (or who are missing) at the end | 0 | ≤ 0 | pass |
| death events matched, share | 1.0000 | ≥ 1.0000 | pass |
| NPCs whose inventory differs (or who are missing) at the end | 0 | ≤ 3 | pass |
| every inventory difference has attested script provenance | yes | required | pass |

## By tier in B

| Tier | Samples | Same ZS | Same TA | Position p50 / p95 / max, m |
|---|---|---|---|---|
| full | 78473 | 100.00% | 100.00% | 0.00 / 0.00 / 0.0 |
| simplified | 225555 | 100.00% | 100.00% | 0.00 / 0.00 / 0.0 |

## By movement

| Samples | Count | Position p50 / p95 / max, m |
|---|---|---|
| moving_either | 4182 | 0.00 / 0.00 / 0.0 |
| stationary_both | 299846 | 0.00 / 0.00 / 0.0 |

## By game hour since the start

| Hour | Same ZS | Same TA | Position p95, m | Max, m |
|---|---|---|---|---|
| 0 | 100.00% | 100.00% | 0.00 | 0.0 |
| 1 | 100.00% | 100.00% | 0.00 | 0.0 |
| 2 | 100.00% | 100.00% | 0.00 | 0.0 |
| 3 | 100.00% | 100.00% | 0.00 | 0.0 |
| 4 | 100.00% | 100.00% | 0.00 | 0.0 |
| 5 | 100.00% | 100.00% | 0.00 | 0.0 |
| 6 | 100.00% | 100.00% | 0.00 | 0.0 |
| 7 | 100.00% | 100.00% | 0.00 | 0.0 |
| 8 | 100.00% | 100.00% | 0.00 | 0.0 |
| 9 | 100.00% | 100.00% | 0.00 | 0.0 |
| 10 | 100.00% | 100.00% | 0.00 | 0.0 |
| 11 | 100.00% | 100.00% | 0.00 | 0.0 |
| 12 | 100.00% | 100.00% | 0.00 | 0.0 |
| 13 | 100.00% | 100.00% | 0.00 | 0.0 |
| 14 | 100.00% | 100.00% | 0.00 | 0.0 |
| 15 | 100.00% | 100.00% | 0.00 | 0.0 |
| 16 | 100.00% | 100.00% | 0.00 | 0.0 |
| 17 | 100.00% | 100.00% | 0.00 | 0.0 |
| 18 | 100.00% | 100.00% | 0.00 | 0.0 |
| 19 | 100.00% | 100.00% | 0.00 | 0.0 |
| 20 | 100.00% | 100.00% | 0.00 | 0.0 |
| 21 | 100.00% | 100.00% | 0.00 | 0.0 |
| 22 | 100.00% | 100.00% | 0.00 | 0.0 |
| 23 | 100.00% | 100.00% | 0.00 | 0.0 |
| 24 | 100.00% | 100.00% | 0.00 | 0.0 |

## NPCs that diverge most

| NPC | Max distance, m | ZS differs, samples | First ZS difference, min |
|---|---|---|---|
| `PAL_227_SCHIFFSWACHE#0` | 0.00 | 0 of 289 | — |
| `SLD_803_CIPHER#0` | 0.00 | 0 of 289 | — |

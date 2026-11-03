# NPC day parity: window vs headless

- window: `day-window-full` — tier `full`, seed 73106, isolate True, 1440 min every 5, wall 2589 s, end `done`
- headless: `day-headless-full` — tier `full`, seed 73106, isolate True, 1440 min every 5, wall 1782 s, end `done`
- NPC-samples compared: 304028 (1052 NPCs, up to t=1440 min); only in window: 0, only in headless: 0
- window ends at t=1440 of 1440 requested minutes, `done`
- headless ends at t=1440 of 1440 requested minutes, `done`

| Metric | Value |
|---|---|
| same ZS state | 100.00% |
| same routine (TA) entry | 100.00% |
| same HP | 100.00% |
| same inventory | 100.00% |
| position error p50 / p95 / max, m | 0.00 / 0.00 / 0.0 |
| ZS starts matched (±60 min) | 100.00% of 10712 |
| matched ZS start offset p50 / p95 / max, game min | 0.00 / 0.00 / 0.0 |
| deaths window / headless; matched pairs | 6 / 6; 6 |
| NPCs missing in one run at the end | 0 |
| alive differs at the end (missing counts) | 0 |
| inventory differs at the end (missing counts) | 0 |
| NPCs with any ZS difference or >5 m apart | 0 of 1052 |
| first ZS difference, game min | — |

## Tolerances

| Check | Value | Limit | |
|---|---|---|---|
| both runs finished the journal (end_reason done) | yes | required | pass |
| both runs reach their requested minutes and end at the same time | yes | required | pass |
| NPC-samples present in only one run | 0 | ≤ 0 | pass |
| NPCs present at the end of one run and missing in the other | 0 | ≤ 0 | pass |
| same ZS state, share of NPC-samples | 1.0000 | ≥ 1.0000 | pass |
| same routine (TA) entry, share of NPC-samples | 1.0000 | ≥ 1.0000 | pass |
| same HP, share of NPC-samples | 1.0000 | ≥ 1.0000 | pass |
| same inventory, share of NPC-samples | 1.0000 | ≥ 1.0000 | pass |
| position error p50, m | 0.0000 | ≤ 0.0000 | pass |
| position error p95, m | 0.0000 | ≤ 0.0000 | pass |
| position error max, m | 0.0000 | ≤ 0.0000 | pass |
| ZS starts matched within the window, share | 1.0000 | ≥ 1.0000 | pass |
| matched ZS start offset p95, game min | 0.0000 | ≤ 0.0000 | pass |
| NPCs alive in one run and dead (or missing) in the other at the end | 0 | ≤ 0 | pass |
| NPCs whose inventory differs (or who are missing) at the end | 0 | ≤ 0 | pass |

## By tier in headless

| Tier | Samples | Same ZS | Same TA | Position p50 / p95 / max, m |
|---|---|---|---|---|
| full | 304028 | 100.00% | 100.00% | 0.00 / 0.00 / 0.0 |

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
| none | 0 | 0 | — |

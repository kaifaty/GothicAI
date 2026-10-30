# Цена контента квартала

A: `lightfix-1`; B: `harbour-rigging-16`. Ryzen, Vulkan, drawable 1728×1117.

Commit: `64de2036a32f2d3a16e3fbe0f9a650dd71b1b366`; dirty files: 20. Порядок каждого места: A B B A, одна сборка и одна заявка `gq quiet`.

Вердикт p95: «в шуме» при модуле Δ ≤ 2%; «в пороге» при Δ ≤ +5% вне шума; «сверх порога» иначе.

| Место | p50 A, мс | p50 B, мс | Δ p50 | Разброс p50 A / B, мс | p95 A, мс | p95 B, мс | Δ p95 | Разброс p95 A / B, мс | Вердикт p95 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| harbour | 9.600 | 9.552 | -0.50% | 0.073 / 0.016 | 10.366 | 10.315 | -0.49% | 0.045 / 0.004 | в шуме |

| Место | Треугольники/кадр A | Треугольники/кадр B | Draw calls/кадр A | Draw calls/кадр B |
|---|---:|---:|---:|---:|
| harbour | 1265444 | 1601389 | 83.1 | 83.1 |

harbour: +336 тыс. треугольников (+26.5%) и -0.0 вызова отрисовки на кадр; время кадра — в шуме (Δ p95 -0.49%).

Frame intervals are differences between consecutive submitted attempt begin_ns timestamps in the same contiguous perf-PLACE/view-N segment. Intervening FenceBusy attempts count toward elapsed time and zone costs, never as rendered frames. Partial time before the first and after the last submitted attempt of each segment is omitted. FPS is interval count / measured wall time; percentiles use linear interpolation. appcadence.csv is retained as a clock diagnostic, not used for these statistics.

Absolute p95 delta <= calibrated noise band in percent. Repeat max-min spread is descriptive only. Percentiles pool frame intervals from both repeats.

Треугольники: Vulkan clipping-stage input primitives, все треугольные проходы (включая indirect/mesh, тени, глубину, UI и post). Draw calls: команды графической отрисовки API, включая indirect с нулевой работой. Средние счётчиков используют только кадры с `geometry_done=1`; доля завершения — в JSON. Запросы читаются после штатного fence без дополнительного ожидания.

CSV и журналы: `/mnt/shared/gothic-ai/perf-content-worktrees/runs/perf-content-20261030T150539Z-24686`.

SHA-256 наборов: `{"harbour-rigging-16": "98e76e3e41b51e2303f548d2198557793d2089f1a197e571f7130119991e8139", "lightfix-1": "c457ebf4cfbb7b924db18b201698f1ad6c45898d3eb86aad80f67cb9e1f1cbbb"}`.

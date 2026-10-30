# Цена контента квартала

A: `lightfix-1`; B: `harbour-rigging-26`. Ryzen, Vulkan, drawable 1728×1117.

Commit: `74043d4e24a88252603b08cc35233a0164647083`; dirty files: 8. Порядок каждого места: A B B A, одна сборка и одна заявка `gq quiet`.

Вердикт p95: «в шуме» при модуле Δ ≤ 2%; «в пороге» при Δ ≤ +5% вне шума; «сверх порога» иначе.

| Место | p50 A, мс | p50 B, мс | Δ p50 | Разброс p50 A / B, мс | p95 A, мс | p95 B, мс | Δ p95 | Разброс p95 A / B, мс | Вердикт p95 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| harbour | 9.649 | 9.634 | -0.15% | 0.030 / 0.072 | 10.435 | 10.532 | +0.92% | 0.020 / 0.268 | в шуме |

| Место | Треугольники/кадр A | Треугольники/кадр B | Draw calls/кадр A | Draw calls/кадр B |
|---|---:|---:|---:|---:|
| harbour | 1266702 | 1565655 | 83.1 | 83.1 |

harbour: +299 тыс. треугольников (+23.6%) и +0.0 вызова отрисовки на кадр; время кадра — в шуме (Δ p95 +0.92%).

Frame intervals are differences between consecutive submitted attempt begin_ns timestamps in the same contiguous perf-PLACE/view-N or streaming-flythrough/PASS segment. Intervening FenceBusy attempts count toward elapsed time and zone costs, never as rendered frames. Partial time before the first and after the last submitted attempt of each segment is omitted. FPS is interval count / measured wall time; percentiles use linear interpolation. appcadence.csv is retained as a clock diagnostic, not used for these statistics.

Absolute p95 delta <= calibrated noise band in percent. Repeat max-min spread is descriptive only. Percentiles pool frame intervals from both repeats.

Треугольники: Vulkan clipping-stage input primitives, все треугольные проходы (включая indirect/mesh, тени, глубину, UI и post). Draw calls: команды графической отрисовки API, включая indirect с нулевой работой. Средние счётчиков используют только кадры с `geometry_done=1`; доля завершения — в JSON. Запросы читаются после штатного fence без дополнительного ожидания.

CSV и журналы: `/mnt/shared/gothic-ai/perf-content-worktrees/runs/perf-content-20261030T180821Z-17689`.

SHA-256 наборов: `{"harbour-rigging-26": "bd2d9f667060ecbe974a7a41806087b7129f5c845b4c0bb51b0e3d36937767e9", "lightfix-1": "c457ebf4cfbb7b924db18b201698f1ad6c45898d3eb86aad80f67cb9e1f1cbbb"}`.

# Цена контента квартала

A: `lightfix-2`; B: `harbour-volume-2`. Ryzen, Vulkan, drawable 1728×1117.

Commit: `92f527047d8898dda7d62e89ab6db635ff92a948`; dirty files: 0. Порядок каждого места: A B B A, одна сборка и одна заявка `gq quiet`.

Вердикт p95: «в шуме» при модуле Δ ≤ 2%; «в пороге» при Δ ≤ +5% вне шума; «сверх порога» иначе.

| Место | p50 A, мс | p50 B, мс | Δ p50 | Разброс p50 A / B, мс | p95 A, мс | p95 B, мс | Δ p95 | Разброс p95 A / B, мс | Вердикт p95 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| harbour | 9.594 | 9.590 | -0.04% | 0.086 / 0.127 | 10.414 | 10.527 | +1.08% | 0.206 / 0.364 | в шуме |

| Место | Треугольники/кадр A | Треугольники/кадр B | Draw calls/кадр A | Draw calls/кадр B |
|---|---:|---:|---:|---:|
| harbour | 1301256 | 1260482 | 83.1 | 83.1 |

harbour: -41 тыс. треугольников (-3.1%) и -0.0 вызова отрисовки на кадр; время кадра — в шуме (Δ p95 +1.08%).

Frame intervals are differences between consecutive submitted attempt begin_ns timestamps in the same contiguous perf-PLACE/view-N or streaming-flythrough/PASS segment. Intervening FenceBusy attempts count toward elapsed time and zone costs, never as rendered frames. Partial time before the first and after the last submitted attempt of each segment is omitted. FPS is interval count / measured wall time; percentiles use linear interpolation. appcadence.csv is retained as a clock diagnostic, not used for these statistics.

Absolute p95 delta <= calibrated noise band in percent. Repeat max-min spread is descriptive only. Percentiles pool frame intervals from both repeats.

Треугольники: Vulkan clipping-stage input primitives, все треугольные проходы (включая indirect/mesh, тени, глубину, UI и post). Draw calls: команды графической отрисовки API, включая indirect с нулевой работой. Средние счётчиков используют только кадры с `geometry_done=1`; доля завершения — в JSON. Запросы читаются после штатного fence без дополнительного ожидания.

CSV и журналы: `/mnt/shared/gothic-ai/perf-content-worktrees/runs/perf-content-20261030T174526Z-89841`.

SHA-256 наборов: `{"harbour-volume-2": "d6ccf9048d3c30a9437a4754dcdc4be1a6fdefa3116720d5d6e34a187dd2c1c9", "lightfix-2": "03a5a838edc00b14b246ba90d1bf491e046e7b85d9f1bba5ead093f36d13c14c"}`.

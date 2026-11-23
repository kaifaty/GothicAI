# Цена контента квартала

A: `harbour-volume-4`; B: `cityroofs-test-3`. Ryzen, Vulkan, drawable 1728×1117.

Commit: `6bbce67f79359f640bb28a58286243036589ebb1`; dirty files: 12. Порядок каждого места: A B B A, одна сборка и одна заявка `gq quiet`.

Вердикт p95: «в шуме» при модуле Δ ≤ 2%; «в пороге» при Δ ≤ +5% вне шума; «сверх порога» иначе.

| Место | p50 A, мс | p50 B, мс | Δ p50 | Разброс p50 A / B, мс | p95 A, мс | p95 B, мс | Δ p95 | Разброс p95 A / B, мс | Вердикт p95 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| harbour | 11.615 | 11.594 | -0.19% | 0.156 / 0.149 | 12.977 | 12.720 | -1.99% | 0.306 / 0.274 | в шуме |
| market | 11.856 | 11.833 | -0.20% | 0.002 / 0.019 | 13.099 | 13.069 | -0.22% | 0.149 / 0.080 | в шуме |

| Место | Треугольники/кадр A | Треугольники/кадр B | Draw calls/кадр A | Draw calls/кадр B |
|---|---:|---:|---:|---:|
| harbour | 1021380 | 1082402 | 73.1 | 73.1 |
| market | 957166 | 1001870 | 71.3 | 71.3 |

harbour: +61 тыс. треугольников (+6.0%) и +0.0 вызова отрисовки на кадр; время кадра — в шуме (Δ p95 -1.99%).

market: +45 тыс. треугольников (+4.7%) и -0.0 вызова отрисовки на кадр; время кадра — в шуме (Δ p95 -0.22%).

Frame intervals are differences between consecutive submitted attempt begin_ns timestamps in the same contiguous perf-PLACE/view-N or streaming-flythrough/PASS segment. Intervening FenceBusy attempts count toward elapsed time and zone costs, never as rendered frames. Partial time before the first and after the last submitted attempt of each segment is omitted. FPS is interval count / measured wall time; percentiles use linear interpolation. appcadence.csv is retained as a clock diagnostic, not used for these statistics.

Absolute p95 delta <= calibrated noise band in percent. Repeat max-min spread is descriptive only. Percentiles pool frame intervals from both repeats.

Треугольники: Vulkan clipping-stage input primitives, все треугольные проходы (включая indirect/mesh, тени, глубину, UI и post). Draw calls: команды графической отрисовки API, включая indirect с нулевой работой. Средние счётчиков используют только кадры с `geometry_done=1`; доля завершения — в JSON. Запросы читаются после штатного fence без дополнительного ожидания.

CSV и журналы: `/mnt/shared/gothic-ai/perf-content-worktrees/runs/perf-content-20261102T080802Z-82039`.

SHA-256 наборов: `{"cityroofs-test-3": "62510b954612091b6d7f358ccffd1bf5c9279511dbbfc1cbe8a9f1adee8059f7", "harbour-volume-4": "12c8085672b139e573e28584507fce61f725902cf3283a3e463f1ab778594e4f"}`.

# Цена контента квартала

A: `life-base-1`; B: `harbour-life-4`. Ryzen, Vulkan, drawable 1728×1117.

Commit: `b66e6c752e49adafe4cb8c8dbde6ba23cfda38fc`; dirty files: 7. Порядок каждого места: A B B A, одна сборка и одна заявка `gq quiet`.

Вердикт p95: «в шуме» при модуле Δ ≤ 2%; «в пороге» при Δ ≤ +5% вне шума; «сверх порога» иначе.

| Место | p50 A, мс | p50 B, мс | Δ p50 | Разброс p50 A / B, мс | p95 A, мс | p95 B, мс | Δ p95 | Разброс p95 A / B, мс | Вердикт p95 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| harbour | 9.602 | 9.626 | +0.24% | 0.025 / 0.060 | 10.454 | 10.421 | -0.32% | 0.032 / 0.077 | в шуме |

| Место | Треугольники/кадр A | Треугольники/кадр B | Draw calls/кадр A | Draw calls/кадр B |
|---|---:|---:|---:|---:|
| harbour | 1568065 | 1594991 | 83.1 | 83.1 |

harbour: +27 тыс. треугольников (+1.7%) и +0.0 вызова отрисовки на кадр; время кадра — в шуме (Δ p95 -0.32%).

Frame intervals are differences between consecutive submitted attempt begin_ns timestamps in the same contiguous perf-PLACE/view-N or streaming-flythrough/PASS segment. Intervening FenceBusy attempts count toward elapsed time and zone costs, never as rendered frames. Partial time before the first and after the last submitted attempt of each segment is omitted. FPS is interval count / measured wall time; percentiles use linear interpolation. appcadence.csv is retained as a clock diagnostic, not used for these statistics.

Absolute p95 delta <= calibrated noise band in percent. Repeat max-min spread is descriptive only. Percentiles pool frame intervals from both repeats.

Треугольники: Vulkan clipping-stage input primitives, все треугольные проходы (включая indirect/mesh, тени, глубину, UI и post). Draw calls: команды графической отрисовки API, включая indirect с нулевой работой. Средние счётчиков используют только кадры с `geometry_done=1`; доля завершения — в JSON. Запросы читаются после штатного fence без дополнительного ожидания.

CSV и журналы: `/mnt/shared/gothic-ai/perf-content-worktrees/runs/perf-content-20261030T191908Z-21933`.

SHA-256 наборов: `{"harbour-life-4": "de6c0fd692bad524e0d6b4c87d9913121ec8d09e6ac8c17853d04aa69fb73479", "life-base-1": "142449da8a590206279d4282e385e9ad6925018456edb9be126cb255f6085133"}`.

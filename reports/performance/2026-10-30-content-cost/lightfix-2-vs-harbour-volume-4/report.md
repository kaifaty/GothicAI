# Цена контента квартала

A: `lightfix-2`; B: `harbour-volume-4`. Ryzen, Vulkan, drawable 1728×1117.

Commit: `92f527047d8898dda7d62e89ab6db635ff92a948`; dirty files: 0. Порядок каждого места: A B B A, одна сборка и одна заявка `gq quiet`.

Вердикт p95: «в шуме» при модуле Δ ≤ 2%; «в пороге» при Δ ≤ +5% вне шума; «сверх порога» иначе.

| Место | p50 A, мс | p50 B, мс | Δ p50 | Разброс p50 A / B, мс | p95 A, мс | p95 B, мс | Δ p95 | Разброс p95 A / B, мс | Вердикт p95 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| harbour | 9.625 | 9.624 | -0.02% | 0.071 / 0.019 | 10.496 | 10.554 | +0.54% | 0.125 / 0.118 | в шуме |

| Место | Треугольники/кадр A | Треугольники/кадр B | Draw calls/кадр A | Draw calls/кадр B |
|---|---:|---:|---:|---:|
| harbour | 1301212 | 1250499 | 83.1 | 83.1 |

harbour: -51 тыс. треугольников (-3.9%) и -0.0 вызова отрисовки на кадр; время кадра — в шуме (Δ p95 +0.54%).

Frame intervals are differences between consecutive submitted attempt begin_ns timestamps in the same contiguous perf-PLACE/view-N or streaming-flythrough/PASS segment. Intervening FenceBusy attempts count toward elapsed time and zone costs, never as rendered frames. Partial time before the first and after the last submitted attempt of each segment is omitted. FPS is interval count / measured wall time; percentiles use linear interpolation. appcadence.csv is retained as a clock diagnostic, not used for these statistics.

Absolute p95 delta <= calibrated noise band in percent. Repeat max-min spread is descriptive only. Percentiles pool frame intervals from both repeats.

Треугольники: Vulkan clipping-stage input primitives, все треугольные проходы (включая indirect/mesh, тени, глубину, UI и post). Draw calls: команды графической отрисовки API, включая indirect с нулевой работой. Средние счётчиков используют только кадры с `geometry_done=1`; доля завершения — в JSON. Запросы читаются после штатного fence без дополнительного ожидания.

CSV и журналы: `/mnt/shared/gothic-ai/perf-content-worktrees/runs/perf-content-20261030T180213Z-10298`.

SHA-256 наборов: `{"harbour-volume-4": "12c8085672b139e573e28584507fce61f725902cf3283a3e463f1ab778594e4f", "lightfix-2": "03a5a838edc00b14b246ba90d1bf491e046e7b85d9f1bba5ead093f36d13c14c"}`.

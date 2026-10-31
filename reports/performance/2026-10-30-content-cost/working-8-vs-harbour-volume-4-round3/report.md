# Цена контента квартала

A: `working-8`; B: `harbour-volume-4`. Ryzen, Vulkan, drawable 1728×1117.

Commit: `0d0410d2901d7a66e0e99f2e81d72dac7321cfa5`; dirty files: 14. Порядок каждого места: A B B A, одна сборка и одна заявка `gq quiet`.

Вердикт p95: «в шуме» при модуле Δ ≤ 2%; «в пороге» при Δ ≤ +5% вне шума; «сверх порога» иначе.

| Место | p50 A, мс | p50 B, мс | Δ p50 | Разброс p50 A / B, мс | p95 A, мс | p95 B, мс | Δ p95 | Разброс p95 A / B, мс | Вердикт p95 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| harbour | 9.582 | 9.576 | -0.07% | 0.008 / 0.005 | 10.450 | 10.367 | -0.80% | 0.086 / 0.083 | в шуме |

| Место | Треугольники/кадр A | Треугольники/кадр B | Draw calls/кадр A | Draw calls/кадр B |
|---|---:|---:|---:|---:|
| harbour | 1265936 | 1334253 | 83.1 | 83.1 |

harbour: +68 тыс. треугольников (+5.4%) и -0.0 вызова отрисовки на кадр; время кадра — в шуме (Δ p95 -0.80%).

Frame intervals are differences between consecutive submitted attempt begin_ns timestamps in the same contiguous perf-PLACE/view-N segment. Intervening FenceBusy attempts count toward elapsed time and zone costs, never as rendered frames. Partial time before the first and after the last submitted attempt of each segment is omitted. FPS is interval count / measured wall time; percentiles use linear interpolation. appcadence.csv is retained as a clock diagnostic, not used for these statistics.

Absolute p95 delta <= calibrated noise band in percent. Repeat max-min spread is descriptive only. Percentiles pool frame intervals from both repeats.

Треугольники: Vulkan clipping-stage input primitives, все треугольные проходы (включая indirect/mesh, тени, глубину, UI и post). Draw calls: команды графической отрисовки API, включая indirect с нулевой работой. Средние счётчиков используют только кадры с `geometry_done=1`; доля завершения — в JSON. Запросы читаются после штатного fence без дополнительного ожидания.

CSV и журналы: `/mnt/shared/gothic-ai/perf-content-worktrees/runs/perf-content-20261030T080327Z-71919`.

SHA-256 наборов: `{"harbour-volume-4": "b65c819f876fa0474050dde2e042f5692e7a4b5a9dfb7efcb8e191424ac68179", "working-8": "3497b8d4062dc76cd7588dafbf793724ad8ef452d3aea47c2bed8d2c54642b80"}`.

# Цена контента квартала

A: `lightfix-1`; B: `harbour-rigging-7`. Ryzen, Vulkan, drawable 1728×1117.

Commit: `f447b7746fb55c1f345ca307e4f0161861788897`; dirty files: 29. Порядок каждого места: A B B A, одна сборка и одна заявка `gq quiet`.

Вердикт p95: «в шуме» при модуле Δ ≤ 2%; «в пороге» при Δ ≤ +5% вне шума; «сверх порога» иначе.

| Место | p50 A, мс | p50 B, мс | Δ p50 | Разброс p50 A / B, мс | p95 A, мс | p95 B, мс | Δ p95 | Разброс p95 A / B, мс | Вердикт p95 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| harbour | 9.589 | 9.591 | +0.03% | 0.049 / 0.076 | 10.386 | 10.490 | +1.00% | 0.094 / 0.233 | в шуме |

| Место | Треугольники/кадр A | Треугольники/кадр B | Draw calls/кадр A | Draw calls/кадр B |
|---|---:|---:|---:|---:|
| harbour | 1265059 | 1568973 | 83.1 | 83.1 |

harbour: +304 тыс. треугольников (+24.0%) и +0.0 вызова отрисовки на кадр; время кадра — в шуме (Δ p95 +1.00%).

Frame intervals are differences between consecutive submitted attempt begin_ns timestamps in the same contiguous perf-PLACE/view-N segment. Intervening FenceBusy attempts count toward elapsed time and zone costs, never as rendered frames. Partial time before the first and after the last submitted attempt of each segment is omitted. FPS is interval count / measured wall time; percentiles use linear interpolation. appcadence.csv is retained as a clock diagnostic, not used for these statistics.

Absolute p95 delta <= calibrated noise band in percent. Repeat max-min spread is descriptive only. Percentiles pool frame intervals from both repeats.

Треугольники: Vulkan clipping-stage input primitives, все треугольные проходы (включая indirect/mesh, тени, глубину, UI и post). Draw calls: команды графической отрисовки API, включая indirect с нулевой работой. Средние счётчиков используют только кадры с `geometry_done=1`; доля завершения — в JSON. Запросы читаются после штатного fence без дополнительного ожидания.

CSV и журналы: `/mnt/shared/gothic-ai/perf-content-worktrees/runs/perf-content-20261030T113733Z-24868`.

SHA-256 наборов: `{"harbour-rigging-7": "493de7e284af201b4efe24b44a7d56c56526292535b3cd878f8e9d088dc2bf72", "lightfix-1": "c457ebf4cfbb7b924db18b201698f1ad6c45898d3eb86aad80f67cb9e1f1cbbb"}`.

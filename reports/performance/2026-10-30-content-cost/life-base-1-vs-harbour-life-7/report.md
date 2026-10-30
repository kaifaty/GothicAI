# Цена контента квартала

A: `life-base-1`; B: `harbour-life-7`. Ryzen, Vulkan, drawable 1728×1117.

Commit: `cc90be8abc749b17b6e11a732ddaf89fdcb44c7e`; dirty files: 13. Порядок каждого места: A B B A, одна сборка и одна заявка `gq quiet`.

Вердикт p95: «в шуме» при модуле Δ ≤ 2%; «в пороге» при Δ ≤ +5% вне шума; «сверх порога» иначе.

| Место | p50 A, мс | p50 B, мс | Δ p50 | Разброс p50 A / B, мс | p95 A, мс | p95 B, мс | Δ p95 | Разброс p95 A / B, мс | Вердикт p95 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| harbour | 9.638 | 9.631 | -0.07% | 0.061 / 0.021 | 10.489 | 10.479 | -0.09% | 0.015 / 0.004 | в шуме |

| Место | Треугольники/кадр A | Треугольники/кадр B | Draw calls/кадр A | Draw calls/кадр B |
|---|---:|---:|---:|---:|
| harbour | 1566781 | 1585401 | 83.1 | 83.1 |

harbour: +19 тыс. треугольников (+1.2%) и -0.0 вызова отрисовки на кадр; время кадра — в шуме (Δ p95 -0.09%).

Frame intervals are differences between consecutive submitted attempt begin_ns timestamps in the same contiguous perf-PLACE/view-N or streaming-flythrough/PASS segment. Intervening FenceBusy attempts count toward elapsed time and zone costs, never as rendered frames. Partial time before the first and after the last submitted attempt of each segment is omitted. FPS is interval count / measured wall time; percentiles use linear interpolation. appcadence.csv is retained as a clock diagnostic, not used for these statistics.

Absolute p95 delta <= calibrated noise band in percent. Repeat max-min spread is descriptive only. Percentiles pool frame intervals from both repeats.

Треугольники: Vulkan clipping-stage input primitives, все треугольные проходы (включая indirect/mesh, тени, глубину, UI и post). Draw calls: команды графической отрисовки API, включая indirect с нулевой работой. Средние счётчиков используют только кадры с `geometry_done=1`; доля завершения — в JSON. Запросы читаются после штатного fence без дополнительного ожидания.

CSV и журналы: `/mnt/shared/gothic-ai/perf-content-worktrees/runs/perf-content-20261030T195815Z-48413`.

SHA-256 наборов: `{"harbour-life-7": "df854263ac8d9b61f2136beeddd8fbd5a67b90986043dedb7e6c265a0d13317f", "life-base-1": "142449da8a590206279d4282e385e9ad6925018456edb9be126cb255f6085133"}`.

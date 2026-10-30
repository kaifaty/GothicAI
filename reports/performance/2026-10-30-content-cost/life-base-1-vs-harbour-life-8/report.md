# Цена контента квартала

A: `life-base-1`; B: `harbour-life-8`. Ryzen, Vulkan, drawable 1728×1117.

Commit: `4355cff61988d78b05d044f6ad1f97b326c1937a`; dirty files: 4. Порядок каждого места: A B B A, одна сборка и одна заявка `gq quiet`.

Вердикт p95: «в шуме» при модуле Δ ≤ 2%; «в пороге» при Δ ≤ +5% вне шума; «сверх порога» иначе.

| Место | p50 A, мс | p50 B, мс | Δ p50 | Разброс p50 A / B, мс | p95 A, мс | p95 B, мс | Δ p95 | Разброс p95 A / B, мс | Вердикт p95 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| harbour | 9.560 | 9.559 | -0.02% | 0.041 / 0.075 | 10.405 | 10.375 | -0.29% | 0.037 / 0.140 | в шуме |

| Место | Треугольники/кадр A | Треугольники/кадр B | Draw calls/кадр A | Draw calls/кадр B |
|---|---:|---:|---:|---:|
| harbour | 1566709 | 1587514 | 83.1 | 83.1 |

harbour: +21 тыс. треугольников (+1.3%) и -0.0 вызова отрисовки на кадр; время кадра — в шуме (Δ p95 -0.29%).

Frame intervals are differences between consecutive submitted attempt begin_ns timestamps in the same contiguous perf-PLACE/view-N or streaming-flythrough/PASS segment. Intervening FenceBusy attempts count toward elapsed time and zone costs, never as rendered frames. Partial time before the first and after the last submitted attempt of each segment is omitted. FPS is interval count / measured wall time; percentiles use linear interpolation. appcadence.csv is retained as a clock diagnostic, not used for these statistics.

Absolute p95 delta <= calibrated noise band in percent. Repeat max-min spread is descriptive only. Percentiles pool frame intervals from both repeats.

Треугольники: Vulkan clipping-stage input primitives, все треугольные проходы (включая indirect/mesh, тени, глубину, UI и post). Draw calls: команды графической отрисовки API, включая indirect с нулевой работой. Средние счётчиков используют только кадры с `geometry_done=1`; доля завершения — в JSON. Запросы читаются после штатного fence без дополнительного ожидания.

CSV и журналы: `/mnt/shared/gothic-ai/perf-content-worktrees/runs/perf-content-20261030T201727Z-65262`.

SHA-256 наборов: `{"harbour-life-8": "ca9e2022276d9ae12590480309aa6a7484b751e31c5dc162894582d72e17e6e0", "life-base-1": "142449da8a590206279d4282e385e9ad6925018456edb9be126cb255f6085133"}`.

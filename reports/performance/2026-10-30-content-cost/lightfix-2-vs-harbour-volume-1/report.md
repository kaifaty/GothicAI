# Цена контента квартала

A: `lightfix-2`; B: `harbour-volume-1`. Ryzen, Vulkan, drawable 1728×1117.

Commit: `92f527047d8898dda7d62e89ab6db635ff92a948`; dirty files: 0. Порядок каждого места: A B B A, одна сборка и одна заявка `gq quiet`.

Вердикт p95: «в шуме» при модуле Δ ≤ 2%; «в пороге» при Δ ≤ +5% вне шума; «сверх порога» иначе.

| Место | p50 A, мс | p50 B, мс | Δ p50 | Разброс p50 A / B, мс | p95 A, мс | p95 B, мс | Δ p95 | Разброс p95 A / B, мс | Вердикт p95 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| harbour | 9.561 | 9.590 | +0.31% | 0.025 / 0.054 | 10.314 | 10.556 | +2.35% | 0.068 / 0.352 | в пороге |

| Место | Треугольники/кадр A | Треугольники/кадр B | Draw calls/кадр A | Draw calls/кадр B |
|---|---:|---:|---:|---:|
| harbour | 1301650 | 1306885 | 83.1 | 83.1 |

harbour: +5 тыс. треугольников (+0.4%) и +0.0 вызова отрисовки на кадр; время кадра — в пороге (Δ p95 +2.35%).

Frame intervals are differences between consecutive submitted attempt begin_ns timestamps in the same contiguous perf-PLACE/view-N or streaming-flythrough/PASS segment. Intervening FenceBusy attempts count toward elapsed time and zone costs, never as rendered frames. Partial time before the first and after the last submitted attempt of each segment is omitted. FPS is interval count / measured wall time; percentiles use linear interpolation. appcadence.csv is retained as a clock diagnostic, not used for these statistics.

Absolute p95 delta <= calibrated noise band in percent. Repeat max-min spread is descriptive only. Percentiles pool frame intervals from both repeats.

Треугольники: Vulkan clipping-stage input primitives, все треугольные проходы (включая indirect/mesh, тени, глубину, UI и post). Draw calls: команды графической отрисовки API, включая indirect с нулевой работой. Средние счётчиков используют только кадры с `geometry_done=1`; доля завершения — в JSON. Запросы читаются после штатного fence без дополнительного ожидания.

CSV и журналы: `/mnt/shared/gothic-ai/perf-content-worktrees/runs/perf-content-20261030T175301Z-98283`.

SHA-256 наборов: `{"harbour-volume-1": "d0ab53542f2fa75076e76e69b09386c0bab79670cc54fe8fa8c192d40ee61358", "lightfix-2": "03a5a838edc00b14b246ba90d1bf491e046e7b85d9f1bba5ead093f36d13c14c"}`.

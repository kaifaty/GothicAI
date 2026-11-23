# Цена контента квартала

A: `harbour-life-8`; B: `harbour-motion-2`. Ryzen, Vulkan, drawable 1728×1117.

Commit: `d98dae1b198983a9ababfce9c0ef11e3c04b7a66`; dirty files: 13. Порядок каждого места: A B B A, одна сборка и одна заявка `gq quiet`.

Вердикт p95: «в шуме» при модуле Δ ≤ 2%; «в пороге» при Δ ≤ +5% вне шума; «сверх порога» иначе.

| Место | p50 A, мс | p50 B, мс | Δ p50 | Разброс p50 A / B, мс | p95 A, мс | p95 B, мс | Δ p95 | Разброс p95 A / B, мс | Вердикт p95 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| harbour | 9.982 | 9.979 | -0.04% | 0.118 / 0.063 | 11.274 | 11.009 | -2.35% | 0.613 / 0.084 | в пороге |
| harbour-night | 9.719 | 9.821 | +1.06% | 0.098 / 0.118 | 10.684 | 10.786 | +0.96% | 0.118 / 0.024 | в шуме |

| Место | Треугольники/кадр A | Треугольники/кадр B | Draw calls/кадр A | Draw calls/кадр B |
|---|---:|---:|---:|---:|
| harbour | 1297237 | 1293365 | 73.1 | 73.8 |
| harbour-night | 465260 | 466666 | 50.0 | 51.0 |

harbour: -4 тыс. треугольников (-0.3%) и +0.7 вызова отрисовки на кадр; время кадра — в пороге (Δ p95 -2.35%).

harbour-night: +1 тыс. треугольников (+0.3%) и +1.0 вызова отрисовки на кадр; время кадра — в шуме (Δ p95 +0.96%).

Frame intervals are differences between consecutive submitted attempt begin_ns timestamps in the same contiguous perf-PLACE/view-N or streaming-flythrough/PASS segment. Intervening FenceBusy attempts count toward elapsed time and zone costs, never as rendered frames. Partial time before the first and after the last submitted attempt of each segment is omitted. FPS is interval count / measured wall time; percentiles use linear interpolation. appcadence.csv is retained as a clock diagnostic, not used for these statistics.

Absolute p95 delta <= calibrated noise band in percent. Repeat max-min spread is descriptive only. Percentiles pool frame intervals from both repeats.

Треугольники: Vulkan clipping-stage input primitives, все треугольные проходы (включая indirect/mesh, тени, глубину, UI и post). Draw calls: команды графической отрисовки API, включая indirect с нулевой работой. Средние счётчиков используют только кадры с `geometry_done=1`; доля завершения — в JSON. Запросы читаются после штатного fence без дополнительного ожидания.

CSV и журналы: `/mnt/shared/gothic-ai/perf-content-worktrees/runs/perf-content-20261031T071709Z-78004`.

SHA-256 наборов: `{"harbour-life-8": "ca9e2022276d9ae12590480309aa6a7484b751e31c5dc162894582d72e17e6e0", "harbour-motion-2": "d17abd86b9edae880abc5bada9876a9321351e728ee356e32af3543da4b977a2"}`.

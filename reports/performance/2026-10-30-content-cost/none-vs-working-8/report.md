# Цена контента квартала

A: `none`; B: `working-8`. Ryzen, Vulkan, drawable 1728×1117.

Commit: `bc10b0e8b77dbcb9cdfc117e4a382f9baaa5f460`; dirty files: 12. Порядок каждого места: A B B A, одна сборка и одна заявка `gq quiet`.

Вердикт p95: «в шуме» при модуле Δ ≤ 2%; «в пороге» при Δ ≤ +5% вне шума; «сверх порога» иначе.

| Место | p50 A, мс | p50 B, мс | Δ p50 | Разброс p50 A / B, мс | p95 A, мс | p95 B, мс | Δ p95 | Разброс p95 A / B, мс | Вердикт p95 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| harbour | 9.634 | 9.685 | +0.53% | 0.074 / 0.067 | 10.572 | 10.459 | -1.07% | 0.301 / 0.093 | в шуме |
| market | 10.012 | 9.988 | -0.25% | 0.068 / 0.032 | 10.877 | 10.857 | -0.18% | 0.066 / 0.182 | в шуме |

| Место | Треугольники/кадр A | Треугольники/кадр B | Draw calls/кадр A | Draw calls/кадр B |
|---|---:|---:|---:|---:|
| harbour | 1123291 | 1265446 | 82.1 | 83.1 |
| market | 1057692 | 1163902 | 80.3 | 81.3 |

harbour: +142 тыс. треугольников (+12.7%) и +1.0 вызова отрисовки на кадр; время кадра — в шуме (Δ p95 -1.07%).

market: +106 тыс. треугольников (+10.0%) и +1.0 вызова отрисовки на кадр; время кадра — в шуме (Δ p95 -0.18%).

Frame intervals are differences between consecutive submitted attempt begin_ns timestamps in the same contiguous perf-PLACE/view-N segment. Intervening FenceBusy attempts count toward elapsed time and zone costs, never as rendered frames. Partial time before the first and after the last submitted attempt of each segment is omitted. FPS is interval count / measured wall time; percentiles use linear interpolation. appcadence.csv is retained as a clock diagnostic, not used for these statistics.

Absolute p95 delta <= calibrated noise band in percent. Repeat max-min spread is descriptive only. Percentiles pool frame intervals from both repeats.

Треугольники: Vulkan clipping-stage input primitives, все треугольные проходы (включая indirect/mesh, тени, глубину, UI и post). Draw calls: команды графической отрисовки API, включая indirect с нулевой работой. Средние счётчиков используют только кадры с `geometry_done=1`; доля завершения — в JSON. Запросы читаются после штатного fence без дополнительного ожидания.

CSV и журналы: `/mnt/shared/gothic-ai/perf-content-claude-c1-content-perf/runs/perf-content-20261030T005706Z-58719`.

SHA-256 наборов: `{"working-8": "3497b8d4062dc76cd7588dafbf793724ad8ef452d3aea47c2bed8d2c54642b80"}`.

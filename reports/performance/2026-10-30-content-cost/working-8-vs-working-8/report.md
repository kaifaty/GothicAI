# Цена контента квартала

A: `working-8`; B: `working-8`. Ryzen, Vulkan, drawable 1728×1117.

Commit: `ac23f28b0cefb957423870cefa4431d89166b3d3`; dirty files: 3. Порядок каждого места: A B B A, одна сборка и одна заявка `gq quiet`.

Вердикт p95: «в шуме» при модуле Δ ≤ 2%; «в пороге» при Δ ≤ +5% вне шума; «сверх порога» иначе.

| Место | p50 A, мс | p50 B, мс | Δ p50 | Разброс p50 A / B, мс | p95 A, мс | p95 B, мс | Δ p95 | Разброс p95 A / B, мс | Вердикт p95 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| harbour | 9.639 | 9.676 | +0.38% | 0.038 / 0.130 | 10.432 | 10.490 | +0.55% | 0.027 / 0.076 | в шуме |
| market | 10.009 | 9.923 | -0.87% | 0.061 / 0.002 | 10.854 | 10.697 | -1.45% | 0.011 / 0.138 | в шуме |

| Место | Треугольники/кадр A | Треугольники/кадр B | Draw calls/кадр A | Draw calls/кадр B |
|---|---:|---:|---:|---:|
| harbour | 1264998 | 1265108 | 83.1 | 83.1 |
| market | 1163388 | 1163189 | 81.3 | 81.3 |



Frame intervals are differences between consecutive submitted attempt begin_ns timestamps in the same contiguous perf-PLACE/view-N segment. Intervening FenceBusy attempts count toward elapsed time and zone costs, never as rendered frames. Partial time before the first and after the last submitted attempt of each segment is omitted. FPS is interval count / measured wall time; percentiles use linear interpolation. appcadence.csv is retained as a clock diagnostic, not used for these statistics.

Absolute p95 delta <= calibrated noise band in percent. Repeat max-min spread is descriptive only. Percentiles pool frame intervals from both repeats.

Треугольники: Vulkan clipping-stage input primitives, все треугольные проходы (включая indirect/mesh, тени, глубину, UI и post). Draw calls: команды графической отрисовки API, включая indirect с нулевой работой. Средние счётчиков используют только кадры с `geometry_done=1`; доля завершения — в JSON. Запросы читаются после штатного fence без дополнительного ожидания.

CSV и журналы: `/mnt/shared/gothic-ai/perf-content-claude-c1-content-perf/runs/perf-content-20261030T011911Z-78983`.

SHA-256 наборов: `{"working-8": "3497b8d4062dc76cd7588dafbf793724ad8ef452d3aea47c2bed8d2c54642b80"}`.

# Цена контента квартала

A: `lightfix-1`; B: `harbour-rigging-15`. Ryzen, Vulkan, drawable 1728×1117.

Commit: `64de2036a32f2d3a16e3fbe0f9a650dd71b1b366`; dirty files: 20. Порядок каждого места: A B B A, одна сборка и одна заявка `gq quiet`.

Вердикт p95: «в шуме» при модуле Δ ≤ 2%; «в пороге» при Δ ≤ +5% вне шума; «сверх порога» иначе.

| Место | p50 A, мс | p50 B, мс | Δ p50 | Разброс p50 A / B, мс | p95 A, мс | p95 B, мс | Δ p95 | Разброс p95 A / B, мс | Вердикт p95 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| harbour | 9.555 | 9.827 | +2.85% | 0.042 / 0.491 | 10.419 | 11.133 | +6.85% | 0.037 / 1.004 | сверх порога |

| Место | Треугольники/кадр A | Треугольники/кадр B | Draw calls/кадр A | Draw calls/кадр B |
|---|---:|---:|---:|---:|
| harbour | 1263589 | 1598890 | 83.1 | 83.1 |

harbour: +335 тыс. треугольников (+26.5%) и -0.0 вызова отрисовки на кадр; время кадра — сверх порога (Δ p95 +6.85%).

Frame intervals are differences between consecutive submitted attempt begin_ns timestamps in the same contiguous perf-PLACE/view-N segment. Intervening FenceBusy attempts count toward elapsed time and zone costs, never as rendered frames. Partial time before the first and after the last submitted attempt of each segment is omitted. FPS is interval count / measured wall time; percentiles use linear interpolation. appcadence.csv is retained as a clock diagnostic, not used for these statistics.

Absolute p95 delta <= calibrated noise band in percent. Repeat max-min spread is descriptive only. Percentiles pool frame intervals from both repeats.

Треугольники: Vulkan clipping-stage input primitives, все треугольные проходы (включая indirect/mesh, тени, глубину, UI и post). Draw calls: команды графической отрисовки API, включая indirect с нулевой работой. Средние счётчиков используют только кадры с `geometry_done=1`; доля завершения — в JSON. Запросы читаются после штатного fence без дополнительного ожидания.

CSV и журналы: `/mnt/shared/gothic-ai/perf-content-worktrees/runs/perf-content-20261030T145706Z-15877`.

SHA-256 наборов: `{"harbour-rigging-15": "61cbc548bfd6da4f745608b64b9b19e017d8997d97736d755e74347b3cf67c1e", "lightfix-1": "c457ebf4cfbb7b924db18b201698f1ad6c45898d3eb86aad80f67cb9e1f1cbbb"}`.

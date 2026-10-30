# Цена контента квартала

A: `lightfix-1`; B: `harbour-rigging-5`. Ryzen, Vulkan, drawable 1728×1117.

Commit: `5dda102e8cf1084179d514618fe2b505d5068dfc`; dirty files: 33. Порядок каждого места: A B B A, одна сборка и одна заявка `gq quiet`.

Вердикт p95: «в шуме» при модуле Δ ≤ 2%; «в пороге» при Δ ≤ +5% вне шума; «сверх порога» иначе.

| Место | p50 A, мс | p50 B, мс | Δ p50 | Разброс p50 A / B, мс | p95 A, мс | p95 B, мс | Δ p95 | Разброс p95 A / B, мс | Вердикт p95 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| harbour | 9.633 | 9.644 | +0.12% | 0.116 / 0.137 | 10.531 | 10.820 | +2.75% | 0.240 / 0.776 | в пороге |

| Место | Треугольники/кадр A | Треугольники/кадр B | Draw calls/кадр A | Draw calls/кадр B |
|---|---:|---:|---:|---:|
| harbour | 1265534 | 1593395 | 83.1 | 83.1 |

harbour: +328 тыс. треугольников (+25.9%) и +0.0 вызова отрисовки на кадр; время кадра — в пороге (Δ p95 +2.75%).

Frame intervals are differences between consecutive submitted attempt begin_ns timestamps in the same contiguous perf-PLACE/view-N segment. Intervening FenceBusy attempts count toward elapsed time and zone costs, never as rendered frames. Partial time before the first and after the last submitted attempt of each segment is omitted. FPS is interval count / measured wall time; percentiles use linear interpolation. appcadence.csv is retained as a clock diagnostic, not used for these statistics.

Absolute p95 delta <= calibrated noise band in percent. Repeat max-min spread is descriptive only. Percentiles pool frame intervals from both repeats.

Треугольники: Vulkan clipping-stage input primitives, все треугольные проходы (включая indirect/mesh, тени, глубину, UI и post). Draw calls: команды графической отрисовки API, включая indirect с нулевой работой. Средние счётчиков используют только кадры с `geometry_done=1`; доля завершения — в JSON. Запросы читаются после штатного fence без дополнительного ожидания.

CSV и журналы: `/mnt/shared/gothic-ai/perf-content-worktrees/runs/perf-content-20261030T102901Z-32597`.

SHA-256 наборов: `{"harbour-rigging-5": "645bbd710ccadc1539990e29c866bee9afeaff08cc66ba18991dfaf1b866edc3", "lightfix-1": "c457ebf4cfbb7b924db18b201698f1ad6c45898d3eb86aad80f67cb9e1f1cbbb"}`.

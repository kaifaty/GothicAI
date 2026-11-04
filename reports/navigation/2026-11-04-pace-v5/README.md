# Pace-input v5: обучение от HD и сравнение при одинаковом темпе

2026-11-04. Обучено 20 эпох; паритет финальных весов пройден. Сравнение — 2048/2048 эпизодов, гейт — 9/9. Живой набор — 54/54 записей двух моделей на 11 местах.

## Исходники и база

Основа — main `9a72c18c388c72d7c30ea6cad21da2a09d2dab46` с входом темпа из родительской ветки `23adda065` (PR #843); дерево `OpenGothic/`: `5bea2c6f9bc183b766594ae5d07196ff8d2ed564`. Из одной сборки получены runner, контракт, игра и content-convert.

- База: `032e902aee6fef6ccc54fd01fdc4f018e0a2cb80ed470cdae2e79d5ee150b14b`; формат: `aece012711c9320c2e633d166168f2d705c346deef72d365496601e1a82966ca`.
- Runner всех десяти частей сравнения: SHA-256 `27b96fe527b3631b6217c5f7885c07fef37b378d962004833f01bd520442001a`.
- Игра всех свежих живых записей: SHA-256 `1b29241c788045c020ee6fe7ea3425379aa3772e7282a924b4f854f758b555cf`.

Отпечаток базы совпал во всех 2048 метриках эпизодов, подытогах сравнения и 54 живых summary. Общая NAS-база использовалась для чтения; ссылки и кэши находились внутри своей задачи.

## Обучение

Исходный HD: `/mnt/shared/gothic-ai/archive/nav-dagger3-20261030/hd-run/`. Рецепт сверён с [отчётом HD](../2026-10-22-dagger3/README.md), его history и training_config. Корпус: `/mnt/shared/gothic-ai/archive/nav-demos-v4-20261029/`, исходный train split: 602 эпизода / 405626 решений, вход HD. В каждой эпохе 405626 решений учителя и 0 DAgger; за 20 эпох — 8112520 решений.

`nav_pace_convert.py --source HD --output pace-from-hd --steps 256` дал нулевую ошибку логитов и вероятностей четырёх оставшихся голов при обоих темпах. Затем `nav_bc_train.py train --pace-input --init pace-from-hd/last.pt`: AdamW, lr 0.0003, weight decay 0.0001, batch 8, TBPTT segment 64, seed 17, threads 6, CUDA. Focal gamma 2; окно maneuver ×5; auxiliary stuck/next_near/detour = 0.1/0.1/0.5. Выбран фиксированный итог 20-й эпохи.

Цепочка заявок: `train-1 → train-5 → train-10 → train-15 → train-20`; продолжения читают checkpoint с состоянием оптимизатора. Каждая — gq ≤2 ч под `scripts/nav_job.py`.

| Эпоха | Train loss | Время эпохи, с |
|---:|---:|---:|
| 1 | 0.202941918543 | 549.785 |
| 2 | 0.199697117991 | 426.671 |
| 3 | 0.197709852172 | 276.910 |
| 4 | 0.194463190031 | 354.179 |
| 5 | 0.191675186074 | 269.532 |
| 6 | 0.189662433884 | 445.193 |
| 7 | 0.187517637130 | 319.660 |
| 8 | 0.185093150695 | 379.711 |
| 9 | 0.182951200954 | 397.894 |
| 10 | 0.179971326126 | 440.444 |
| 11 | 0.178494279916 | 363.311 |
| 12 | 0.176080391022 | 304.154 |
| 13 | 0.175273211840 | 298.918 |
| 14 | 0.173405681573 | 276.482 |
| 15 | 0.171364214962 | 274.044 |
| 16 | 0.168507082954 | 280.790 |
| 17 | 0.169435100827 | 287.860 |
| 18 | 0.166205976781 | 292.257 |
| 19 | 0.165026210871 | 277.617 |
| 20 | 0.164586443706 | 284.293 |

Сумма времени эпох: 6799.705 с. Параметры и веса классов: `/mnt/shared/gothic-ai/nav-pace-v5/run/training_config.json`. Полная история: `/mnt/shared/gothic-ai/nav-pace-v5/run/history.json`.

| Файл на NAS | SHA-256 |
|---|---|
| `/mnt/shared/gothic-ai/nav-pace-v5/run/policy.navbc` (v5, 2465510 байт) | `b70d3185c320af0e3c519adc9fd471d5f5df4abf3844bde7cbb39d96ccd351dc` |
| `/mnt/shared/gothic-ai/nav-pace-v5/run/last.pt` (9223460 байт) | `ae104e2e4ab322242e33af8059bc4362ab32943331c22a9536246fb3b06176cd` |
| Исходный `HD/policy.navbc` | `304ea6c6574e3836139da72b5a299a28217a263fac3648d792b458327d251a6b` |

## Паритет финальных весов

1000 записанных шагов из HD-корпуса, NAVDEMO v2, веса v5, с переносом hidden. Контракт C++ собран вместе с runner.

| Сравнение | Max feature error | Max logit error | Max hidden error |
|---|---:|---:|---:|
| Torch / NumPy | — | 0.000009059906005859375 | 0.000006906688213348389 |
| C++ / NumPy | 0 | 0.0000181198 | 0.000012584 |

Контракт памяти: 9 конфигураций, 6 случаев неверных metadata, 4 неверных override. Результаты: `/mnt/shared/gothic-ai/nav-pace-v5/parity/{python,cpp,memory}.json`.

## Сравнение

Статический набор — те же 461 `(family,id,action_seed)`, что в [forced-pace](../2026-11-01-forced-pace/README.md): approach 101, object 179, point 60, detour 121. Follow — 102 маршрута из validation HD, всегда run. Seed 18102027. Все четыре статические конфигурации и оба follow исполнены заново на указанном runner и базе.

Сценарии: `/mnt/shared/gothic-ai/nav-demos-v5/manifest.json`; detour: `/mnt/shared/gothic-ai/archive/nav-bc4-20261030/input/detour-val.jsonl`. Переиспользованы два исходных setup-rejection. CV, gaze, sampling, β=0, 16 worker; `--force-pace` задан явно обеим моделям. Run — 240000 мс; walk — `ceil(240000 × 3.064158265) = 735398` мс. Исходные ID, сценарии и seed сохранены. В run walk=0 решений, в walk walk=100%; терминальный сброс не входит в доли.

Медиана игрового времени включает все эпизоды. Turn_only: turn ≠ 0, forward = strafe = 0. Idle: forward = strafe = turn = jump = climb = 0; walk не исключает idle. Доли действий — отношения сумм решений семейства.

| Модель / темп | Семейство | n | arrived | Доля, % | Медиана, с | turn_only, % | idle, % |
|---|---|---:|---:|---:|---:|---:|---:|
| v5 / run | approach | 101 | 96 | 95.049505 | 27.867000 | 13.976113 | 14.831705 |
| v5 / run | object | 179 | 174 | 97.206704 | 11.900000 | 15.099086 | 5.394593 |
| v5 / run | point | 60 | 51 | 85.000000 | 26.116500 | 17.320172 | 9.112608 |
| v5 / run | detour | 121 | 105 | 86.776860 | 14.300000 | 13.706967 | 9.270492 |
| v5 / walk | approach | 101 | 101 | 100.000000 | 42.133000 | 14.990573 | 4.737055 |
| v5 / walk | object | 179 | 164 | 91.620112 | 45.633000 | 21.466880 | 2.652108 |
| v5 / walk | point | 60 | 52 | 86.666667 | 85.033500 | 13.982937 | 16.167520 |
| v5 / walk | detour | 121 | 95 | 78.512397 | 54.967000 | 15.104508 | 9.394591 |
| HD / run | approach | 101 | 97 | 96.039604 | 21.733000 | 15.823533 | 6.950745 |
| HD / run | object | 179 | 173 | 96.648045 | 9.367000 | 14.081646 | 3.129960 |
| HD / run | point | 60 | 45 | 75.000000 | 31.133500 | 11.705093 | 10.164058 |
| HD / run | detour | 121 | 102 | 84.297521 | 14.133000 | 10.197122 | 8.769959 |
| HD / walk | approach | 101 | 100 | 99.009901 | 44.800000 | 9.393215 | 3.427337 |
| HD / walk | object | 179 | 164 | 91.620112 | 34.900000 | 7.292126 | 4.349689 |
| HD / walk | point | 60 | 50 | 83.333333 | 98.883000 | 9.178541 | 4.100401 |
| HD / walk | detour | 121 | 91 | 75.206612 | 49.133000 | 9.411043 | 7.351634 |
| v5 / run | follow | 102 | 2 | 1.960784 | 240.000000 | 14.089639 | 15.926716 |
| HD / run | follow | 102 | 1 | 0.980392 | 240.000000 | 11.550960 | 19.897972 |

## Парные разности v5 − HD

Arrived — разность долей; turn_only и idle — разность отношений сумм решений. Парный SE рассчитан delta method по маршрутам. Для разности медиан — 5000 парных bootstrap-выборок маршрутов, seed 18102027. На маршрут приходится один action seed. Число SE = Δ/SE; при Δ=SE=0 записан 0. п.п. — процентные пункты.

| Темп | Семейство | Метрика | Δ | SE | Число SE | Маршрутов |
|---|---|---|---:|---:|---:|---:|
| run | approach | arrived, п.п. | -0.990099 | 2.222767 | -0.445435 | 101 |
| run | approach | медиана времени, с | 6.134000 | 5.242293 | 1.170099 | 101 |
| run | approach | turn_only, п.п. | -1.847420 | 1.738030 | -1.062939 | 101 |
| run | approach | idle, п.п. | 7.880960 | 6.234928 | 1.264002 | 101 |
| run | object | arrived, п.п. | 0.558659 | 1.680157 | 0.332504 | 179 |
| run | object | медиана времени, с | 2.533000 | 0.778382 | 3.254187 | 179 |
| run | object | turn_only, п.п. | 1.017441 | 2.600066 | 0.391313 | 179 |
| run | object | idle, п.п. | 2.264633 | 2.707774 | 0.836345 | 179 |
| run | point | arrived, п.п. | 10.000000 | 5.674803 | 1.762176 | 60 |
| run | point | медиана времени, с | -5.017000 | 6.288664 | -0.797785 | 60 |
| run | point | turn_only, п.п. | 5.615079 | 2.620217 | 2.142983 | 60 |
| run | point | idle, п.п. | -1.051450 | 6.593375 | -0.159471 | 60 |
| run | detour | arrived, п.п. | 2.479339 | 2.479339 | 1.000000 | 121 |
| run | detour | медиана времени, с | 0.167000 | 1.029864 | 0.162157 | 121 |
| run | detour | turn_only, п.п. | 3.509845 | 2.003636 | 1.751738 | 121 |
| run | detour | idle, п.п. | 0.500533 | 6.031159 | 0.082991 | 121 |
| walk | approach | arrived, п.п. | 0.990099 | 0.990099 | 1.000000 | 101 |
| walk | approach | медиана времени, с | -2.667000 | 4.688688 | -0.568816 | 101 |
| walk | approach | turn_only, п.п. | 5.597358 | 1.205012 | 4.645062 | 101 |
| walk | approach | idle, п.п. | 1.309718 | 0.971814 | 1.347704 | 101 |
| walk | object | arrived, п.п. | 0.000000 | 2.096175 | 0.000000 | 179 |
| walk | object | медиана времени, с | 10.733000 | 2.488949 | 4.312263 | 179 |
| walk | object | turn_only, п.п. | 14.174754 | 3.210336 | 4.415349 | 179 |
| walk | object | idle, п.п. | -1.697581 | 1.097457 | -1.546831 | 179 |
| walk | point | arrived, п.п. | 3.333333 | 2.336962 | 1.426353 | 60 |
| walk | point | медиана времени, с | -13.849500 | 13.048417 | -1.061393 | 60 |
| walk | point | turn_only, п.п. | 4.804397 | 2.701097 | 1.778684 | 60 |
| walk | point | idle, п.п. | 12.067119 | 6.926620 | 1.742137 | 60 |
| walk | detour | arrived, п.п. | 3.305785 | 2.858915 | 1.156308 | 121 |
| walk | detour | медиана времени, с | 5.834000 | 5.420809 | 1.076223 | 121 |
| walk | detour | turn_only, п.п. | 5.693466 | 1.643180 | 3.464906 | 121 |
| walk | detour | idle, п.п. | 2.042957 | 5.790036 | 0.352840 | 121 |
| run | follow | arrived, п.п. | 0.980392 | 1.703684 | 0.575454 | 102 |
| run | follow | медиана времени, с | 0.000000 | 0.000000 | 0.000000 | 102 |
| run | follow | turn_only, п.п. | 2.538678 | 0.615254 | 4.126231 | 102 |
| run | follow | idle, п.п. | -3.971256 | 1.117160 | -3.554778 | 102 |

Гейт пройден по 9/9 критериям: для arrived во всех четырёх семействах при run и walk, а также в follow/run, Δ ≥ −2 SE. Полные результаты: `/mnt/shared/gothic-ai/nav-pace-v5/comparison.json`; исходные метрики — `results/{v5-run,v5-walk,hd-run,hd-walk,v5-follow-run,hd-follow-run}/metrics.json` в той же папке.

## Живой набор pace-v5

Команда: `scripts/nav-live.sh --round pace-v5 --weights /mnt/shared/gothic-ai/nav-pace-v5/run/policy.navbc`. Заморожены 11 мест / 27 seed на модель: три routine-return имеют по одному seed, остальные восемь мест — по три. Три routine-return идут walk, остальные места, включая follow, — run; обеим моделям задан `--force-pace` места. Новые записи: v5 27/27, HD 27/27, учитель 11/11. Путь × рассчитан относительно свежей записи учителя на этой же игре и базе.

Замороженный набор: `/mnt/shared/gothic-ai/nav-live/baseline/extension/943431e2590c-a2964efaf4f4/suite.json`, SHA-256 `a8c97d5b5c25e7122699a745447aff5899a127f8470b515efd7f36c51ca2b6e4`. №4 и лестница №9 исключены заморозкой; follow сохраняет пометку «эталон не квалифицирован».

В ячейках: приезды/n, медиана времени, медиана пути / пути учителя, суммы контактов и падений. Полоса follow — время на расстоянии 2–5 м / весь горизонт, включая исходный разрыв и паузы цели. Видео — медианный и худший seed по прежнему порядку baseline: приезд/время; для follow — полоса/дистанция/стояние.

| Место / темп | v5 | HD |
|---|---|---|
| 01-maleth-fence / run | 3/3; 7.000 с; путь ×1.043421; контактов 3; падений 0; [медианный seed 18102028](/mnt/shared/gothic-ai/nav-pace-v5/live/pace-v5/01-maleth-fence/seed-18102028/source.mp4); [худший 18102029](/mnt/shared/gothic-ai/nav-pace-v5/live/pace-v5/01-maleth-fence/seed-18102029/source.mp4) | 3/3; 9.367 с; путь ×1.182060; контактов 4; падений 0; [медианный seed 18102029](/mnt/shared/gothic-ai/nav-pace-v5/live/HD/01-maleth-fence/seed-18102029/source.mp4); [худший 18102028](/mnt/shared/gothic-ai/nav-pace-v5/live/HD/01-maleth-fence/seed-18102028/source.mp4) |
| 02-routine-return-forest-tree / walk | 1/1; 27.400 с; путь ×1.026965; контактов 2; падений 0; [медианный seed 18102027](/mnt/shared/gothic-ai/nav-pace-v5/live/pace-v5/02-routine-return-forest-tree/seed-18102027/source.mp4) | 1/1; 21.633 с; путь ×0.958937; контактов 1; падений 0; [медианный seed 18102027](/mnt/shared/gothic-ai/nav-pace-v5/live/HD/02-routine-return-forest-tree/seed-18102027/source.mp4) |
| 02-routine-return-farm-edge / walk | 1/1; 95.733 с; путь ×0.982481; контактов 5; падений 0; [медианный seed 18102028](/mnt/shared/gothic-ai/nav-pace-v5/live/pace-v5/02-routine-return-farm-edge/seed-18102028/source.mp4) | 1/1; 102.567 с; путь ×1.086554; контактов 3; падений 0; [медианный seed 18102028](/mnt/shared/gothic-ai/nav-pace-v5/live/HD/02-routine-return-farm-edge/seed-18102028/source.mp4) |
| 02-routine-return-forest-pine / walk | 1/1; 67.667 с; путь ×1.848552; контактов 0; падений 0; [медианный seed 18102029](/mnt/shared/gothic-ai/nav-pace-v5/live/pace-v5/02-routine-return-forest-pine/seed-18102029/source.mp4) | 1/1; 76.733 с; путь ×2.211133; контактов 1; падений 0; [медианный seed 18102029](/mnt/shared/gothic-ai/nav-pace-v5/live/HD/02-routine-return-forest-pine/seed-18102029/source.mp4) |
| 03-follow-hero / run | 0/3; 180.000 с; путь ×1.237192; контактов 105; падений 0; полоса 40.796296%; [медианный seed 18102027](/mnt/shared/gothic-ai/nav-pace-v5/live/pace-v5/03-follow-hero/seed-18102027/source.mp4); [худший 18102029](/mnt/shared/gothic-ai/nav-pace-v5/live/pace-v5/03-follow-hero/seed-18102029/source.mp4) | 0/3; 180.000 с; путь ×1.068723; контактов 82; падений 0; полоса 54.722222%; [медианный seed 18102028](/mnt/shared/gothic-ai/nav-pace-v5/live/HD/03-follow-hero/seed-18102028/source.mp4); [худший 18102027](/mnt/shared/gothic-ai/nav-pace-v5/live/HD/03-follow-hero/seed-18102027/source.mp4) |
| 05-wall-pocket / run | 1/3; 90.000 с; путь ×1.739448; контактов 77; падений 0; [медианный seed 18102028](/mnt/shared/gothic-ai/nav-pace-v5/live/pace-v5/05-wall-pocket/seed-18102028/source.mp4); [худший 18102029](/mnt/shared/gothic-ai/nav-pace-v5/live/pace-v5/05-wall-pocket/seed-18102029/source.mp4) | 2/3; 26.900 с; путь ×1.229336; контактов 62; падений 0; [медианный seed 18102027](/mnt/shared/gothic-ai/nav-pace-v5/live/HD/05-wall-pocket/seed-18102027/source.mp4); [худший 18102028](/mnt/shared/gothic-ai/nav-pace-v5/live/HD/05-wall-pocket/seed-18102028/source.mp4) |
| 06-harbour-water / run | 3/3; 23.800 с; путь ×1.713487; контактов 8; падений 0; [медианный seed 18102027](/mnt/shared/gothic-ai/nav-pace-v5/live/pace-v5/06-harbour-water/seed-18102027/source.mp4); [худший 18102029](/mnt/shared/gothic-ai/nav-pace-v5/live/pace-v5/06-harbour-water/seed-18102029/source.mp4) | 3/3; 28.200 с; путь ×1.823067; контактов 17; падений 0; [медианный seed 18102029](/mnt/shared/gothic-ai/nav-pace-v5/live/HD/06-harbour-water/seed-18102029/source.mp4); [худший 18102028](/mnt/shared/gothic-ai/nav-pace-v5/live/HD/06-harbour-water/seed-18102028/source.mp4) |
| 07-cliff-trail / run | 3/3; 2.767 с; путь ×0.804664; контактов 1; падений 0; [медианный seed 18102027](/mnt/shared/gothic-ai/nav-pace-v5/live/pace-v5/07-cliff-trail/seed-18102027/source.mp4); [худший 18102028](/mnt/shared/gothic-ai/nav-pace-v5/live/pace-v5/07-cliff-trail/seed-18102028/source.mp4) | 3/3; 6.167 с; путь ×1.935130; контактов 0; падений 0; [медианный seed 18102028](/mnt/shared/gothic-ai/nav-pace-v5/live/HD/07-cliff-trail/seed-18102028/source.mp4); [худший 18102029](/mnt/shared/gothic-ai/nav-pace-v5/live/HD/07-cliff-trail/seed-18102029/source.mp4) |
| 08-market-crowd / run | 3/3; 32.267 с; путь ×1.060946; контактов 4; падений 0; [медианный seed 18102029](/mnt/shared/gothic-ai/nav-pace-v5/live/pace-v5/08-market-crowd/seed-18102029/source.mp4); [худший 18102027](/mnt/shared/gothic-ai/nav-pace-v5/live/pace-v5/08-market-crowd/seed-18102027/source.mp4) | 3/3; 31.600 с; путь ×1.039339; контактов 5; падений 0; [медианный seed 18102027](/mnt/shared/gothic-ai/nav-pace-v5/live/HD/08-market-crowd/seed-18102027/source.mp4); [худший 18102028](/mnt/shared/gothic-ai/nav-pace-v5/live/HD/08-market-crowd/seed-18102028/source.mp4) |
| 09-door-stairs-door / run | 3/3; 1.633 с; путь ×0.999281; контактов 0; падений 0; [медианный seed 18102029](/mnt/shared/gothic-ai/nav-pace-v5/live/pace-v5/09-door-stairs-door/seed-18102029/source.mp4); [худший 18102028](/mnt/shared/gothic-ai/nav-pace-v5/live/pace-v5/09-door-stairs-door/seed-18102028/source.mp4) | 3/3; 2.700 с; путь ×1.112681; контактов 0; падений 0; [медианный seed 18102027](/mnt/shared/gothic-ai/nav-pace-v5/live/HD/09-door-stairs-door/seed-18102027/source.mp4); [худший 18102029](/mnt/shared/gothic-ai/nav-pace-v5/live/HD/09-door-stairs-door/seed-18102029/source.mp4) |
| 10-local-stranger / run | 3/3; 18.667 с; путь ×1.191642; контактов 2; падений 0; [медианный seed 18102028](/mnt/shared/gothic-ai/nav-pace-v5/live/pace-v5/10-local-stranger/seed-18102028/source.mp4); [худший 18102029](/mnt/shared/gothic-ai/nav-pace-v5/live/pace-v5/10-local-stranger/seed-18102029/source.mp4) | 3/3; 15.067 с; путь ×1.036244; контактов 0; падений 0; [медианный seed 18102028](/mnt/shared/gothic-ai/nav-pace-v5/live/HD/10-local-stranger/seed-18102028/source.mp4); [худший 18102029](/mnt/shared/gothic-ai/nav-pace-v5/live/HD/10-local-stranger/seed-18102029/source.mp4) |

FFprobe подтвердил 65/65 видео (54 моделей и 11 учительских). Summary и все 54 видео моделей: `/mnt/shared/gothic-ai/nav-pace-v5/live/`. Итог — `live-summary.json`; requested и applied pace совпали во всех 54 summary.

## Время и проверки

Оценка после первых заявок HD и первой эпохи: 11.396 ч; после 20 эпох: 10.241 ч; после полного v5/run: 9.999 ч; после HD/walk: 9.610 ч. В оценку живой части входили 65 свежих записей. Ожидание очереди в бюджет вычислений не включено.

Первый живой запуск `8a9c22743c21`: 67.816 с, exit 1, 0/54 записей моделей. При обработке первой записи учителя изолированный Python (`-I`) не нашёл соседний `nav_live_render`; флаг убран из строки запуска. Повтор продолжил эту же свежую запись учителя и записал весь набор.

| Этап | gq | Занято, с | Exit |
|---|---|---:|---:|
| train-1 | `b07bdb24844f` | 0.000 | 143 |
| train-1 | `cfc865695467` | 29.794 | 143 |
| train-1 | `0350b95b5ef4` | 2424.147 | 0 |
| compare-hd-run-main | `72cf080c31aa` | 1553.098 | 0 |
| compare-hd-run-detour | `1adcd8cf6317` | 721.060 | 0 |
| compare-hd-run-follow | `73fb4341c622` | 2133.946 | 0 |
| train-5 | `7d4a799c7189` | 1339.507 | 0 |
| train-10 | `9b9706e918e2` | 1991.634 | 0 |
| train-15 | `212b7437d320` | 1527.666 | 0 |
| train-20 | `f8bff9d8b75b` | 1430.451 | 0 |
| parity | `98c6dda8a917` | 19.315 | 0 |
| compare-v5-run-main | `93ecfd40e928` | 1406.122 | 0 |
| compare-v5-run-detour | `f1d640bfa00b` | 642.294 | 0 |
| compare-v5-run-follow | `cfb70719aa7f` | 2076.726 | 0 |
| compare-hd-walk-main | `cac8bc5ab7f3` | 3795.222 | 0 |
| compare-hd-walk-detour | `be8b2a0604cd` | 1830.246 | 0 |
| compare-v5-walk-main | `9afe48b92829` | 3304.230 | 0 |
| compare-v5-walk-detour | `a8280ad4883a` | 1545.371 | 0 |
| live-v5 | `8a9c22743c21` | 67.816 | 1 |
| live-v5 | `8bffa13ed9a7` | 4404.382 | 0 |

Суммарная занятость Ryzen: **32243.026 с (8.956396 ч) из 43200 с (12 ч)**, включая отменённую подготовку 29.794 с; ещё одна заявка отменена до допуска, 0 с. Все выполненные заявки — под `scripts/nav_job.py`, каждая gq ≤2 ч.

Собраны четыре целевых бинарника; `gothic-navpolicy-contract --live-format`: records=2, ray_bytes=55, record_bytes=125874. `python3 scripts/check.py run --base 23adda065`: 31 passed за 10.75 с. Bash-синтаксис, Python-синтаксис встроенных блоков и `git diff --check` пройдены.

Своя папка `/home/kaifaty/gothic-ai/nav-pace-v5/` удалена командой `bash scripts/nav-pace-remote.sh clean-v5`, вместе со сборками, venv и кэшами. Проверено отсутствие папки; после удаления свободно 124243562496 байт (115.711 ГиБ). Финальные веса, история и записи сохранены на NAS.

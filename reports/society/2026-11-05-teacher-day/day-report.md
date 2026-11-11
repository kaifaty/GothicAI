# Отчёт суток: ограды

| Проверка | Значение | Итог |
|---|---|---|
| teacher-1: critical food, median h | 0.0 | да |
| teacher-1: critical food, max h | 8.048 | **нет** |
| teacher-1: critical sleep, median h | 0.0 | да |
| teacher-1: critical sleep, max h | 8.188 | **нет** |
| teacher-1: critical toilet, median h | 0.0 | да |
| teacher-1: critical toilet, max h | 8.707 | **нет** |
| teacher-1: critical social, median h | 0.0 | да |
| teacher-1: critical social, max h | 3.148 | да |
| teacher-1: critical work, median h | 0.0 | да |
| teacher-1: critical work, max h | 0.244 | да |
| teacher-1: critical faith, median h | 0.0 | да |
| teacher-1: critical faith, max h | 0.0 | да |
| teacher-1: critical leisure, median h | 0.0 | да |
| teacher-1: critical leisure, max h | 2.792 | да |
| teacher-1: out of mask | 0 | да |
| teacher-1: over capacity | 0 | да |
| teacher-1: protected marks same | 1.0 | да |
| teacher-1: anchor time by routine | 0.95142 | да |
| teacher-1: changes an hour, p95 | 2 | да |
| teacher-1: a day, s | 0.0938 | да |
| teacher-7: critical food, median h | 0.0 | да |
| teacher-7: critical food, max h | 22.441 | **нет** |
| teacher-7: critical sleep, median h | 0.0 | да |
| teacher-7: critical sleep, max h | 24.0 | **нет** |
| teacher-7: critical toilet, median h | 0.001 | да |
| teacher-7: critical toilet, max h | 16.355 | **нет** |
| teacher-7: critical social, median h | 0.0 | да |
| teacher-7: critical social, max h | 21.075 | **нет** |
| teacher-7: critical work, median h | 0.0 | да |
| teacher-7: critical work, max h | 9.044 | **нет** |
| teacher-7: critical faith, median h | 0.0 | да |
| teacher-7: critical faith, max h | 24.0 | **нет** |
| teacher-7: critical leisure, median h | 0.0 | да |
| teacher-7: critical leisure, max h | 20.035 | **нет** |
| teacher-7: out of mask | 0 | да |
| teacher-7: over capacity | 0 | да |
| teacher-7: protected marks same | 1.0 | да |
| teacher-7: anchor time by routine | 0.8257 | **нет** |
| teacher-7: changes an hour, p95 | 3 | да |
| teacher-7: a day, s | 0.1142 | да |
| random-7: out of mask | 0 | да |
| random-7: over capacity | 0 | да |
| sensitivity: food grows | 0.243 | да |
| sensitivity: sleep grows | 0.83 | да |
| sensitivity: toilet grows | 0.142 | да |
| sensitivity: social grows | 0.201 | да |
| sensitivity: work grows | 0.165 | да |
| sensitivity: faith grows | 3.287 | да |
| sensitivity: leisure grows | 0.559 | да |
| traits: talk moves chosen social time its way | 0.886 | да |
| traits: same-guild pairs differ the same way | 0.895 | да |

## Места и концы занятий

Держатели места — пользователи распорядка (`place_held`) и выбранные занятия с бронью. Выселен — выбранное занятие уступило место пришедшему пользователю распорядка.

| Прогон | взято распорядком | больше всего держателей | сверх вместимости | выселено в пути / на месте | конец: уставка / выше «низко» / наибольшее время / другой выбор / якорь | не удалось: занято / якорь |
|---|---|---|---|---|---|---|
| teacher-1 | 1306 | 4 | 0 | 9 / 50 | 1500 / 343 / 323 / 158 / 51 | 24 / 2 |
| teacher-7 | 8759 | 4 | 0 | 81 / 428 | 11399 / 3983 / 3457 / 3269 / 914 | 134 / 41 |
| random-7 | 4332 | 4 | 0 | 468 / 454 | 19128 / 4129 / 5800 / 11292 / 429 | 119 / 152 |

## Часы в «критично» в сутки у решающих людей (человеко-сутки: медиана, p90, максимум)

| Прогон | food | sleep | toilet | social | work | faith | leisure |
|---|---|---|---|---|---|---|---|
| routine-1 | 9.81 / 11.806 / 12.167 | 4.021 / 7.724 / 8.322 | 16.788 / 17.552 / 17.724 | 0.0 / 4.834 / 9.359 | 11.416 / 14.113 / 14.598 | 0.0 / 0.0 / 0.0 | 0.0 / 3.376 / 5.173 |
| routine-7 | 24.0 / 24.0 / 24.0 | 24.0 / 24.0 / 24.0 | 24.0 / 24.0 / 24.0 | 0.0 / 24.0 / 24.0 | 12.396 / 24.0 / 24.0 | 0.0 / 24.0 / 24.0 | 4.699 / 24.0 / 24.0 |
| teacher-1 | 0.0 / 0.52 / 8.048 | 0.0 / 0.34 / 8.188 | 0.0 / 0.318 / 8.707 | 0.0 / 0.0 / 3.148 | 0.0 / 0.0 / 0.244 | 0.0 / 0.0 / 0.0 | 0.0 / 0.0 / 2.792 |
| teacher-7 | 0.0 / 1.438 / 22.441 | 0.0 / 4.135 / 24.0 | 0.001 / 0.659 / 16.355 | 0.0 / 1.115 / 21.075 | 0.0 / 0.0 / 9.044 | 0.0 / 0.045 / 24.0 | 0.0 / 0.741 / 20.035 |
| random-7 | 2.114 / 13.464 / 24.0 | 5.033 / 19.626 / 24.0 | 11.562 / 24.0 / 24.0 | 0.0 / 10.518 / 24.0 | 0.0 / 1.991 / 23.881 | 0.0 / 3.08 / 24.0 | 0.0 / 7.225 / 24.0 |
| стражи, посты держат (7 суток, 64 чел.) | 24.0 / 24.0 / 24.0 | 24.0 / 24.0 / 24.0 | 24.0 / 24.0 / 24.0 | 24.0 / 24.0 / 24.0 | 0.0 / 0.0 / 0.072 | 1.233 / 24.0 / 24.0 | 24.0 / 24.0 / 24.0 |
| стражи, посты отпускают (7 суток, 64 чел.) | 1.161 / 5.234 / 12.974 | 1.437 / 9.222 / 21.324 | 0.224 / 0.76 / 4.538 | 0.659 / 5.104 / 12.205 | 0.0 / 0.0 / 6.139 | 0.0 / 14.831 / 24.0 | 0.466 / 3.919 / 11.661 |

## Якорное время по распорядку (порог ≥ 95% по всем решающим)

| Прогон | все | < 2 свободных ч | ≥ 2 свободных ч |
|---|---|---|---|
| teacher-1 | 95.1% | 91.0% (44 чел.) | 98.1% |
| teacher-7 | 82.6% | 70.3% (44 чел.) | 91.5% |
| random-7 | 61.0% | 49.4% (44 чел.) | 69.4% |
| teacher-7, посты отпускают (для решения пользователя) | 69.3% | 55.9% (88 чел.) | 88.7% |

## Нечем утолить: 49 нужд без способа в пределах пути от точек распорядка

Пробелы контента: такая нужда не убывает и в ограды не входит.

| NPC | гильдия | нужда | ближайший способ | место | пути, мин |
|---|---|---|---|---|---|
| BAU_4300_ADDON_CAVALORN#0 | 0 | social | campfire | FP_CAMPFIRE_NW_FARM1_MILL_01 | 85.9 |
| BAU_901_BAUER#0 | 8 | faith | shrine | NW_BIGMILL_MALAKSVERSTECK_02 | 68.2 |
| BAU_902_GUNNAR#0 | 8 | faith | shrine | NW_BIGMILL_MALAKSVERSTECK_02 | 60.4 |
| BAU_903_BODO#0 | 8 | faith | shrine | NW_BIGMILL_MALAKSVERSTECK_02 | 60.9 |
| BAU_904_BAUER#0 | 8 | faith | shrine | NW_BIGMILL_MALAKSVERSTECK_02 | 60.8 |
| BAU_907_WASILI#0 | 8 | faith | shrine | NW_BIGMILL_MALAKSVERSTECK_02 | 76.2 |
| BAU_908_HODGES#0 | 8 | faith | shrine | NW_BIGMILL_MALAKSVERSTECK_02 | 65.2 |
| BAU_910_MARIA#0 | 8 | faith | shrine | NW_BIGMILL_MALAKSVERSTECK_02 | 66.8 |
| BAU_914_BAEUERIN#0 | 8 | faith | shrine | NW_BIGMILL_MALAKSVERSTECK_02 | 60.2 |
| BAU_916_BAUER#0 | 8 | faith | shrine | NW_BIGMILL_MALAKSVERSTECK_02 | 67.6 |
| BAU_917_BAUER#0 | 8 | faith | shrine | NW_BIGMILL_MALAKSVERSTECK_02 | 67.6 |
| BAU_918_BAUER#0 | 8 | faith | shrine | NW_BIGMILL_MALAKSVERSTECK_02 | 67.8 |
| BAU_919_BAUER#0 | 8 | faith | shrine | NW_BIGMILL_MALAKSVERSTECK_02 | 67.8 |
| BAU_920_BAUER#0 | 8 | faith | shrine | NW_BIGMILL_MALAKSVERSTECK_02 | 66.5 |
| BAU_921_BAUER#0 | 8 | faith | shrine | NW_BIGMILL_MALAKSVERSTECK_02 | 60.4 |
| BAU_982_GRIMBALD#0 | 12 | food | pan | NW_TROLLAREA_RUINS_28 | 136.6 |
| BAU_982_GRIMBALD#0 | 12 | leisure | bench | NW_TAVERNE_TROLLAREA_MONSTER_03_01 | 265.3 |
| BAU_984_NICLAS#0 | 12 | social | smalltalk | FP_SMALLTALK_NW_CITY_MERCHANT_01 | 78.0 |
| BAU_984_NICLAS#0 | 12 | faith | shrine | FP_PRAY_VATRAS | 99.3 |
| BDT_1002_BANDIT_L#1 | 9 | work | throne | NW_CITY_MERCHANT_HUT_02_IN | 60.7 |
| BDT_1009_BANDIT_L#0 | 9 | work | field | FP_PICK_NW_FARM2_FIELD_09 | 67.6 |
| BDT_1010_BANDIT_L#0 | 9 | work | field | FP_PICK_NW_FARM2_FIELD_09 | 66.5 |
| BDT_1011_BANDIT_M#0 | 9 | work | field | FP_PICK_NW_FARM2_FIELD_09 | 67.6 |
| BDT_1013_BANDIT_L#0 | 9 | social | campfire | FP_CAMPFIRE_NW_FARM1_MILL_01 | 76.2 |
| BDT_1014_BANDIT_L#0 | 9 | food | stove | NW_XARDAS_TOWER_IN1_32 | 77.0 |
| BDT_1014_BANDIT_L#0 | 9 | social | campfire | FP_CAMPFIRE_NW_FARM1_MILL_01 | 93.6 |
| BDT_1014_BANDIT_L#0 | 9 | work | stove | NW_XARDAS_TOWER_IN1_32 | 77.0 |
| BDT_1015_BANDIT_L#0 | 9 | food | stove | NW_XARDAS_TOWER_IN1_32 | 77.0 |
| BDT_1015_BANDIT_L#0 | 9 | social | campfire | FP_CAMPFIRE_NW_FARM1_MILL_01 | 93.6 |
| BDT_1015_BANDIT_L#0 | 9 | work | stove | NW_XARDAS_TOWER_IN1_32 | 77.0 |
| KDW_1400_ADDON_SATURAS_NW#0 | 14 | leisure | bench | NW_TAVERNE_TROLLAREA_MONSTER_03_01 | 129.3 |
| KDW_1401_ADDON_CRONOS_NW#0 | 14 | faith | shrine | NW_TROLLAREA_PATH_37 | 68.1 |
| KDW_1401_ADDON_CRONOS_NW#0 | 14 | leisure | bench | NW_TAVERNE_TROLLAREA_MONSTER_03_01 | 148.7 |
| KDW_1402_ADDON_NEFARIUS_NW#0 | 14 | faith | shrine | NW_TROLLAREA_PATH_37 | 71.1 |
| KDW_1402_ADDON_NEFARIUS_NW#0 | 14 | leisure | bench | NW_TAVERNE_TROLLAREA_MONSTER_03_01 | 151.7 |
| KDW_1403_ADDON_MYXIR_NW#0 | 14 | faith | shrine | NW_TROLLAREA_PATH_37 | 70.4 |
| KDW_1403_ADDON_MYXIR_NW#0 | 14 | leisure | bench | NW_TAVERNE_TROLLAREA_MONSTER_03_01 | 150.9 |
| KDW_1404_ADDON_RIORDIAN_NW#0 | 14 | leisure | bench | NW_TAVERNE_TROLLAREA_MONSTER_03_01 | 136.5 |
| KDW_1405_ADDON_MERDARION_NW#0 | 14 | faith | shrine | NW_TROLLAREA_PATH_37 | 68.5 |
| KDW_1405_ADDON_MERDARION_NW#0 | 14 | leisure | bench | NW_TAVERNE_TROLLAREA_MONSTER_03_01 | 149.1 |
| NONE_100_XARDAS#0 | 0 | social | campfire | FP_CAMPFIRE_NW_FARM1_MILL_01 | 82.5 |
| PAL_222_SCHIFFSWACHE#0 | 1 | faith | shrine | FP_PRAY_VATRAS | 63.6 |
| PAL_223_SCHIFFSWACHE#0 | 1 | faith | shrine | FP_PRAY_VATRAS | 66.5 |
| PAL_224_SCHIFFSWACHE#0 | 1 | faith | shrine | FP_PRAY_VATRAS | 63.2 |
| PAL_227_SCHIFFSWACHE#0 | 1 | faith | shrine | FP_PRAY_VATRAS | 60.8 |
| PC_PSIONIC#0 | 0 | food | stove | NW_XARDAS_TOWER_IN1_32 | 103.2 |
| PC_PSIONIC#0 | 0 | work | stove | NW_XARDAS_TOWER_IN1_32 | 103.2 |
| PC_PSIONIC#0 | 0 | faith | shrine | NW_XARDAS_TOWER_IN1_21 | 113.3 |
| VLK_4301_ADDON_FARIM#0 | 3 | faith | shrine | FP_PRAY_VATRAS | 65.6 |

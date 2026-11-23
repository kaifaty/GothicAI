# Отчёт суток: ограды

| Проверка | Значение | Итог |
|---|---|---|
| teacher-1: critical food, median h | 0.0 | да |
| teacher-1: critical food, max h | 4.818 | **нет** |
| teacher-1: critical sleep, median h | 0.0 | да |
| teacher-1: critical sleep, max h | 8.148 | **нет** |
| teacher-1: critical toilet, median h | 0.0 | да |
| teacher-1: critical toilet, max h | 8.707 | **нет** |
| teacher-1: critical social, median h | 0.0 | да |
| teacher-1: critical social, max h | 2.822 | да |
| teacher-1: critical work, median h | 0.0 | да |
| teacher-1: critical work, max h | 0.0 | да |
| teacher-1: critical faith, median h | 0.0 | да |
| teacher-1: critical faith, max h | 0.0 | да |
| teacher-1: critical leisure, median h | 0.0 | да |
| teacher-1: critical leisure, max h | 1.508 | да |
| teacher-1: out of mask | 0 | да |
| teacher-1: over capacity | 0 | да |
| teacher-1: protected marks same | 1.0 | да |
| teacher-1: anchor time by routine | 0.9523 | да |
| teacher-1: changes an hour, p95 | 2 | да |
| teacher-1: a day, s | 0.1303 | да |
| teacher-7: critical food, median h | 0.0 | да |
| teacher-7: critical food, max h | 22.23 | **нет** |
| teacher-7: critical sleep, median h | 0.0 | да |
| teacher-7: critical sleep, max h | 24.0 | **нет** |
| teacher-7: critical toilet, median h | 0.001 | да |
| teacher-7: critical toilet, max h | 16.355 | **нет** |
| teacher-7: critical social, median h | 0.0 | да |
| teacher-7: critical social, max h | 20.067 | **нет** |
| teacher-7: critical work, median h | 0.0 | да |
| teacher-7: critical work, max h | 0.49 | да |
| teacher-7: critical faith, median h | 0.0 | да |
| teacher-7: critical faith, max h | 24.0 | **нет** |
| teacher-7: critical leisure, median h | 0.0 | да |
| teacher-7: critical leisure, max h | 17.712 | **нет** |
| teacher-7: out of mask | 0 | да |
| teacher-7: over capacity | 0 | да |
| teacher-7: protected marks same | 1.0 | да |
| teacher-7: anchor time by routine | 0.82813 | **нет** |
| teacher-7: changes an hour, p95 | 3 | да |
| teacher-7: a day, s | 0.1976 | да |
| random-7: out of mask | 0 | да |
| random-7: over capacity | 0 | да |
| sensitivity: food grows | 0.264 | да |
| sensitivity: sleep grows | 0.872 | да |
| sensitivity: toilet grows | 0.181 | да |
| sensitivity: social grows | 0.231 | да |
| sensitivity: work grows | 0.164 | да |
| sensitivity: faith grows | 3.523 | да |
| sensitivity: leisure grows | 0.566 | да |
| traits: talk moves chosen social time its way | 0.862 | да |
| traits: same-guild pairs differ the same way | 0.908 | да |

## Места и концы занятий

Держатели места — пользователи распорядка (`place_held`) и выбранные занятия с бронью. Выселен — выбранное занятие уступило место пришедшему пользователю распорядка. Повторное решение — нужда в «низко» или «критично» не наполняется тем, что NPC делает, `recheck_min` после прошлого решения.

| Прогон | взято распорядком | больше всего держателей | сверх вместимости | выселено в пути / на месте | конец: уставка / выше «низко» / наибольшее время / другой выбор / якорь | не удалось: занято / якорь | повторных решений |
|---|---|---|---|---|---|---|---|
| teacher-1 | 1329 | 4 | 0 | 5 / 58 | 1355 / 327 / 298 / 202 / 64 | 25 / 5 | 119 |
| teacher-7 | 8985 | 4 | 0 | 95 / 433 | 10715 / 3857 / 3252 / 3826 / 1014 | 122 / 51 | 1923 |
| random-7 | 4592 | 4 | 0 | 535 / 507 | 18922 / 4112 / 5097 / 21762 / 456 | 79 / 167 | 11957 |

## Часы в «критично» в сутки у решающих людей (человеко-сутки: медиана, p90, максимум)

| Прогон | food | sleep | toilet | social | work | faith | leisure |
|---|---|---|---|---|---|---|---|
| routine-1 | 9.81 / 11.806 / 12.167 | 4.021 / 7.724 / 8.322 | 16.788 / 17.552 / 17.724 | 0.0 / 4.834 / 9.359 | 11.416 / 14.113 / 14.598 | 0.0 / 0.0 / 0.0 | 0.0 / 3.376 / 5.173 |
| routine-7 | 24.0 / 24.0 / 24.0 | 24.0 / 24.0 / 24.0 | 24.0 / 24.0 / 24.0 | 0.0 / 24.0 / 24.0 | 12.396 / 24.0 / 24.0 | 0.0 / 24.0 / 24.0 | 4.699 / 24.0 / 24.0 |
| teacher-1 | 0.0 / 0.588 / 4.818 | 0.0 / 0.104 / 8.148 | 0.0 / 0.355 / 8.707 | 0.0 / 0.0 / 2.822 | 0.0 / 0.0 / 0.0 | 0.0 / 0.0 / 0.0 | 0.0 / 0.0 / 1.508 |
| teacher-7 | 0.0 / 1.491 / 22.23 | 0.0 / 3.394 / 24.0 | 0.001 / 0.711 / 16.355 | 0.0 / 1.152 / 20.067 | 0.0 / 0.0 / 0.49 | 0.0 / 0.0 / 24.0 | 0.0 / 0.898 / 17.712 |
| random-7 | 1.647 / 12.693 / 24.0 | 12.069 / 23.582 / 24.0 | 9.231 / 18.679 / 24.0 | 0.0 / 9.865 / 24.0 | 0.0 / 7.205 / 24.0 | 0.0 / 3.304 / 24.0 | 0.0 / 9.0 / 24.0 |
| стражи, посты держат (7 суток, 64 чел.) | 24.0 / 24.0 / 24.0 | 24.0 / 24.0 / 24.0 | 24.0 / 24.0 / 24.0 | 24.0 / 24.0 / 24.0 | 0.0 / 0.0 / 0.072 | 1.27 / 24.0 / 24.0 | 24.0 / 24.0 / 24.0 |
| стражи, посты отпускают (7 суток, 64 чел.) | 1.105 / 3.934 / 9.638 | 1.72 / 9.279 / 22.675 | 0.255 / 0.893 / 5.5 | 0.797 / 5.177 / 13.991 | 0.0 / 0.0 / 10.037 | 0.0 / 15.038 / 24.0 | 0.518 / 3.761 / 10.825 |

## Якорное время по распорядку (порог ≥ 95% по всем решающим)

| Прогон | все | < 2 свободных ч | ≥ 2 свободных ч |
|---|---|---|---|
| teacher-1 | 95.2% | 91.2% (44 чел.) | 98.2% |
| teacher-7 | 82.8% | 69.8% (44 чел.) | 92.2% |
| random-7 | 53.4% | 42.0% (44 чел.) | 61.6% |
| teacher-7, посты отпускают (для решения пользователя) | 69.6% | 55.5% (88 чел.) | 90.0% |

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

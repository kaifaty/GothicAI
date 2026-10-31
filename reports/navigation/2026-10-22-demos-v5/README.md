# Демонстрации навигации v5

Записано 939 train-эпизодов; годных 700; решений 906800. NAVDEMO v3, память 4 (поход 300 с / 300 м, familiar), gaze, CV. Учитель v3b; переходы размечены учителем v2. Знакомая подсказка учителю выключена. Ученик не обучался.

| Семейство | Эпизоды | Годные | Решения | Назад | Вбок | jump | climb |
|---|---:|---:|---:|---:|---:|---:|---:|
| point | 400 | 310 | 381313 | 3.76% | 14.38% | 0 | 0 |
| object | 123 | 101 | 70143 | 2.47% | 6.06% | 0 | 0 |
| approach | 81 | 62 | 68101 | 6.07% | 31.40% | 0 | 0 |
| follow_below_gate | 119 | 61 | 281497 | 4.18% | 11.26% | 0 | 0 |
| jump | 168 | 125 | 65785 | 0.00% | 0.00% | 453 | 125 |
| water | 48 | 41 | 39961 | 1.16% | 16.03% | 0 | 0 |

Местные: 396/939 (42.17%); чужаки: 543. Признак получен из knownroads и записан в каждый эпизод. Объём корпуса: 18277406376 байт (18.277 ГБ), бюджет 30 ГБ.

## Водные сценарии

Train-сценариев 48, годных 41. Входов в воду 0, глубоких входов 0; Вход — начало непрерывного интервала, включая мокрый первый кадр: вода глубже 10 см; глубокая вода — порог груди соответствующей гильдии с допуском 1 см. Счётчики рассчитаны по нативным физическим кадрам, включая начало и конец эпизода.

| Сценарий | Начало → цель | Годен | Входы в воду | Глубокие входы | Кадры глубокой воды | Плавание, кадры |
|---|---|---:|---:|---:|---:|---:|
| water-067b504c89c80daf | NW_CITY_HABOUR_HUT_05_BED_01 → NW_CITY_HABOUR_SHIP_01 | 1 | 0 | 0 | 0 | 0 |
| water-0709851d1a1d436a | NW_CITY_HABOUR_KASERN_01 → NW_CITY_HABOUR_SHIP_01 | 1 | 0 | 0 | 0 | 0 |
| water-09aeb98b2e4aa50d | NW_CITY_HABOUR_06_C → NW_CITY_HABOUR_POOR_AREA_PATH_01_01 | 1 | 0 | 0 | 0 | 0 |
| water-1a36c45baea3bb9d | NW_CITY_HABOUR_11 → NW_CITY_HABOUR_POOR_AREA_01_B | 1 | 0 | 0 | 0 | 0 |
| water-1d56c80e521976ed | NW_CITY_HABOUR_KASERN_02 → NW_CITY_HABOUR_SHIP_01 | 1 | 0 | 0 | 0 | 0 |
| water-27bb9dfc0bc5504d | NW_CITY_HABOUR_HUT_07_BED_01 → NW_CITY_HABOUR_SHIP_01 | 0 | 0 | 0 | 0 | 0 |
| water-2adfeb2cf2eb084e | NW_CITY_HABOUR_POOR_AREA_01_B → NW_CITY_HABOUR_11 | 1 | 0 | 0 | 0 | 0 |
| water-2ba76fb66c1a5f61 | NW_CITY_HABOUR_JACK → NW_CITY_HABOUR_12 | 1 | 0 | 0 | 0 | 0 |
| water-32eba5523f8573cc | NW_CITY_HABOUR_06_B → NW_CITY_HABOUR_11 | 1 | 0 | 0 | 0 | 0 |
| water-38a788f79ca4b988 | NW_CITY_HABOUR_11 → NW_CITY_HABOUR_JACK | 1 | 0 | 0 | 0 | 0 |
| water-396f9f357bcfc962 | NW_CITY_HABOUR_REPAIR_03 → NW_CITY_HABOUR_SHIP_01 | 1 | 0 | 0 | 0 | 0 |
| water-3af58c8806e8dac6 | NW_CITY_HABOUR_PUFF_07 → NW_CITY_HABOUR_SHIP_01 | 1 | 0 | 0 | 0 | 0 |
| water-3d014ad53bcd9b92 | NW_CITY_HABOUR_HUT_03_IN_01 → NW_CITY_HABOUR_SHIP_01 | 1 | 0 | 0 | 0 | 0 |
| water-5096f505c97ea0b0 | NW_CITY_HABOUR_PUFF_IN_06 → NW_CITY_HABOUR_12 | 0 | 0 | 0 | 0 | 0 |
| water-51f276ad970d277a | NW_CITY_HABOUR_10 → NW_CITY_HABOUR_SHIP_01 | 1 | 0 | 0 | 0 | 0 |
| water-566c6972dfae0dbe | NW_CITY_HABOUR_PUFF_VANJA → NW_CITY_HABOUR_SHIP_01 | 0 | 0 | 0 | 0 | 0 |
| water-569b981cf24fc3e6 | NW_CITY_HABOUR_HUT_03_IN_08 → NW_CITY_HABOUR_SHIP_01 | 1 | 0 | 0 | 0 | 0 |
| water-5707544a3f0ff68c | NW_CITY_HABOUR_REPAIR_04 → NW_CITY_HABOUR_SHIP_01 | 1 | 0 | 0 | 0 | 0 |
| water-5d4d11eb78084720 | NW_CITY_HABOUR_HUT_01_FRONT → NW_CITY_HABOUR_SHIP_01 | 1 | 0 | 0 | 0 | 0 |
| water-6821cd891cd42021 | NW_CITY_HABOUR_PUFF_IN_02 → NW_CITY_HABOUR_12 | 0 | 0 | 0 | 0 | 0 |
| water-685fdb96bfba26da | NW_CITY_HABOUR_PUFF_IN_03 → NW_CITY_HABOUR_SHIP_01 | 0 | 0 | 0 | 0 | 0 |
| water-6a140c90d97778b9 | NW_CITY_HABOUR_PUFF_IN_04 → NW_CITY_HABOUR_SHIP_01 | 1 | 0 | 0 | 0 | 0 |
| water-6a514cf2d157aca7 | NW_CITY_HABOUR_02 → NW_CITY_HABOUR_12 | 1 | 0 | 0 | 0 | 0 |
| water-6be4c673001beec9 | NW_CITY_HABOUR_HUT_04_COOK → NW_CITY_HABOUR_SHIP_01 | 1 | 0 | 0 | 0 | 0 |
| water-75a404ce4f16a025 | NW_CITY_HABOUR_HUT_06_BED_01 → NW_CITY_HABOUR_SHIP_01 | 1 | 0 | 0 | 0 | 0 |
| water-77d8dc282a0029b8 | NW_CITY_HABOUR_06 → NW_CITY_HABOUR_12 | 1 | 0 | 0 | 0 | 0 |
| water-7dabd37f097fe1f9 | NW_CITY_HABOUR_12 → NW_CITY_HABOUR_JACK | 1 | 0 | 0 | 0 | 0 |
| water-8ae64f65db99d055 | NW_CITY_HABOUR_06_C → NW_CITY_HABOUR_11 | 1 | 0 | 0 | 0 | 0 |
| water-958889d3eddf4adb | NW_CITY_HABOUR_HUT_08_BED_01 → NW_CITY_HABOUR_SHIP_01 | 1 | 0 | 0 | 0 | 0 |
| water-9f4bfeadca8f157f | NW_CITY_HABOUR_08 → NW_CITY_HABOUR_SHIP_01 | 1 | 0 | 0 | 0 | 0 |
| water-a10a9412614c73b1 | NW_CITY_HABOUR_09 → NW_CITY_HABOUR_SHIP_01 | 1 | 0 | 0 | 0 | 0 |
| water-a9f7265295549a68 | NW_CITY_HABOUR_PUFF_IN_07 → NW_CITY_HABOUR_12 | 0 | 0 | 0 | 0 | 0 |
| water-ae56d821e7a1317d | NW_CITY_HABOUR_KASERN_04 → NW_CITY_HABOUR_06_C | 1 | 0 | 0 | 0 | 0 |
| water-af6ebbf712580a7e | NW_CITY_HABOUR_HUT_03_IN → NW_CITY_HABOUR_SHIP_01 | 1 | 0 | 0 | 0 | 0 |
| water-b890bac6316aecdd | NW_CITY_HABOUR_POOR_AREA_PATH_20 → NW_CITY_HABOUR_SHIP_01 | 1 | 0 | 0 | 0 | 0 |
| water-bc9f0b2a996972f9 | NW_CITY_HABOUR_12 → NW_CITY_HABOUR_POOR_AREA_PATH_04 | 1 | 0 | 0 | 0 | 0 |
| water-c55bb1e7c8dc1b3a | NW_CITY_HABOUR_HUT_07 → NW_CITY_HABOUR_SHIP_01 | 1 | 0 | 0 | 0 | 0 |
| water-c716a7a86e27fabb | NW_CITY_HABOUR_POOR_AREA_PATH_19 → NW_CITY_HABOUR_SHIP_01 | 1 | 0 | 0 | 0 | 0 |
| water-c8642f2afa3db0b9 | NW_CITY_HABOUR_SHIP_01 → NW_CITY_HABOUR_HUT_05_BED_02 | 0 | 0 | 0 | 0 | 0 |
| water-d3ad11ea8390d047 | NW_CITY_HABOUR_HUT_03_IN_04 → NW_CITY_HABOUR_SHIP_01 | 1 | 0 | 0 | 0 | 0 |
| water-db616e25a66b4ef0 | NW_CITY_HABOUR_POOR_AREA_PATH_04 → NW_CITY_HABOUR_12 | 1 | 0 | 0 | 0 | 0 |
| water-e1f71d2347f5eb09 | NW_CITY_HABOUR_WERFT_IN_01 → NW_CITY_HABOUR_SHIP_01 | 1 | 0 | 0 | 0 | 0 |
| water-e37bc24b2b6f98ae | NW_CITY_HABOUR_HUT_03_IN_02 → NW_CITY_HABOUR_SHIP_01 | 1 | 0 | 0 | 0 | 0 |
| water-ea88304369a0a85e | NW_CITY_HABOUR_STOMPER_01 → NW_CITY_HABOUR_SHIP_01 | 1 | 0 | 0 | 0 | 0 |
| water-f426423a66ba12ff | NW_CITY_HABOUR_HUT_05_BED_02 → NW_CITY_HABOUR_SHIP_01 | 1 | 0 | 0 | 0 | 0 |
| water-f4596fdaf8c3fe76 | NW_CITY_HABOUR_HUT_07_IN → NW_CITY_HABOUR_SHIP_01 | 1 | 0 | 0 | 0 | 0 |
| water-f6723620a549cf4c | NW_CITY_HABOUR_JACK → NW_CITY_HABOUR_SHIP_01 | 1 | 0 | 0 | 0 | 0 |
| water-f9ab9957a4d94af4 | NW_CITY_HABOUR_06_B → NW_CITY_HABOUR_12 | 1 | 0 | 0 | 0 | 0 |

Все попытки: входов в воду 4, глубоких входов 55, эпизодов с глубокой водой или плаванием 4; цель 0 глубоких не выполнена. В годном train: глубоких входов 0, кадров глубокой воды 0, плавания 0. Негодные попытки сохранены для диагностики и не входят в train.

## Сравнение с v4

Парные общие train-сценарии совпадают по семейству, id, началу и цели и успешны у обоих учителей. Длина — сумма отрезков физической траектории в XZ; Detour — нативная длина исходного пути. Для движущихся целей отношение не определено; они исключены.

| Семейство | Общие успешные | Средняя длина v4, м | Средняя длина v5, м | Длина / Detour v4 | Длина / Detour v5 |
|---|---:|---:|---:|---:|---:|
| point | 232 | 60.348 | 54.808 | 1.6219 | 1.4298 |
| object | 92 | 36.249 | 36.548 | 1.0987 | 1.1017 |
| jump | 142 | 5.578 | 5.596 | 1.3291 | 1.3275 |

## Проверка и манифест

Все 939 эпизодов прошли `nav_demos.py check` (`summarize(validate=True)`). SHA-256 каждого samples.bin.gz совпал с итоговой сводкой и метаданными; набор файлов на хранилище точно совпал со сводкой. Годный train не содержит глубокой воды или плавания.

[manifest.json](manifest.json) содержит компактные записи: id, семейство, split, годность, число решений, SHA-256 samples и путь относительно корня корпуса. Общий формат и корень указаны один раз; подробные сценарии, knownroads, метрики и validation/test v4 и detour сохранены в полном манифесте. Загрузчик `nav_bc_data.load_training_manifest` принимает компактный файл и явный корень корпуса, читает `episode.json` только годного train. Полный манифест при загрузке train не нужен. Подготовка признаков NAVDEMO v3 и обучение не запускались.

Полный манифест: `/mnt/shared/gothic-ai/nav-demos-v5/manifest.json`. SHA-256: `55ef9e12dc7f8586ac37e3e73fae300d64533bc13103579970fbb6a33df8f2f5`.

## Разбор отказа финализации 23 октября

`point-6eec512509e66c24`: `NW_FARM1_PATH_02` → `FP_PICK_NW_FARM1_FIELD_03_01`, исходная дорожная пара №1430. Она входит в замороженный train-пул v4 (`recording-input/point.jsonl`) и уже была записана в v4.

| Источник | Прямая пара | Обратная пара | Вывод |
|---|---:|---:|---|
| v4 point validation/test | 0 | 0 | Отложенной дорожной пары нет |
| holdout-cv | 0 | 0 | Совпадений нет |
| ppo-validation | 0 | 0 | Совпадений нет |
| detour-val/test | 0 | 0 | Совпадений нет |
| v4 moving test | 2 | 0 | Другие задачи approach/follow, Lobart |

Два moving-test ID: `approach-fa9c410186d8a35f` (SCAVENGER) и `follow-0c9eb3ee424715ca` (BAU_954_MALETH). Они имеют свои типы цели, маршруты и target, не записаны в v5. Финализатор терял семейство задачи и складывал все концы маршрутов в глобальный запрет. Quick-пробы, исторические witness и архив памяти 3 к этому отказу не относятся.

Сохранена политика v4 `scope=family_specific`, `globally_disjoint_spatial_regions=false`: дороги охватывают все шесть районов; moving отделён по району, object — по place_id, jump — по site/transition. Среди 939 актуальных попыток есть 37 дорожных совпадений концов с moving validation/test и три объектных совпадения между алиасами CITY_UPTOWN_JUDGE (train) и CITY_RICHTER (validation). Это исходные ограничения split v4, а не обещание географически новых мест между всеми семействами. Принадлежность группам train проверена для каждого объекта, moving и перехода. Точные held-out ID, обе ориентации point validation/test, holdout/PPO и detour остаются запрещены. Эпизоды не исключались и не перезаписывались.

## Негодные попытки с глубокой водой

| Эпизод | Учитель | Место | Глубокие входы | Глубокие кадры | Кадры плавания |
|---|---:|---|---:|---:|---:|
| follow-6878a3abb71206e9 | v3b | harbour | 1 | 6120 | 6119 |
| jump-0188 | v2 | shipwreck | 1 | 7192 | 7192 |
| jump-0189 | v2 | shipwreck | 1 | 7125 | 7124 |
| jump-0190 | v2 | shipwreck | 52 | 3625 | 3578 |

Все четыре попытки уже имели `training_eligible=false`; пригодность не менялась при финализации. Три shipwreck-перехода начинаются в воде глубиной 124,6 см и неудачно заканчиваются плаванием. Follow в гавани падает в глубокую воду около 36 с. У jump-0190 порог глубины пересекается 52 раза; это переходы состояния по кадрам, а не 52 разных водоёма. Все 48 специально отобранных водных сценариев сухие; все 700 годных эпизодов также без глубокой воды и плавания. Нулевая цель среди всех попыток не достигнута; в train и водном наборе достигнута.

## Хранение и воспроизводимость

Актуальных записей 939. Ещё три старые записи teacher v2 / память 3 сохранены вне корпуса в `/mnt/shared/gothic-ai/nav-demos-v5/superseded-v2-memory3/jump/train/`; вместе с ними физический счётчик даёт 942. В компактном манифесте 939 актуальных попыток, загрузчик допускает только 700 годных train-эпизодов. Сжатые samples занимают 15126816477 байт (15,127 ГБ); полный объём записей с трассами приведён выше.

Всё хранилище v5 вместе со служебными файлами и архивом занимает 18,531 ГБ из 30 ГБ; после очистки локально свободно 116,7 GiB (минимум 20 GiB).

Остатки постановки, discovery, worker-сценарии и журналы перенесены в `diagnostics/` через `rsync --no-links`, каждый файл сверён по SHA-256 до удаления своей локальной копии. Корпус v4 использовался только для чтения. Linux build удалён после полной проверки; собственный Mac build удалён после ранее прошедших сборок Gothic2Notr и gothic-body-runner. Код C++ в этой финализации не менялся.

Проверки финализации: 25 целевых pytest; NAVDEMO check и SHA-256 всех 939 записей / 906800 решений; совпадение набора путей и хешей с итоговой сводкой. Ранее прошли 206 целевых тестов записи и переходов, 38 проверок контракта, обе Mac-сборки и два нативных jump-прогона на памяти 4. Новых записей и обучения в финализации не было.

После ревью размер полного манифеста вынесен из Git: компактный файл занимает 223532 байта. Суммарный предел компактного файла и отчёта — 1500000 байт, финализатор проверяет его до публикации. Прошли 55 целевых тестов; чтение настоящего корпуса через компактный манифест выбрало ровно 700 годных эпизодов / 405599 решений. Записи не менялись, новые эпизоды не запускались.

# Живой набор навигации

Набор от 30 октября 2026. Проверены **54/54** записей baseline после `b0e833601016`:
все имеют видео, трассу, `topdown.mp4`, `timeline.png` и `summary.json`.
HD — **20/24** приездов, it35 — **19/24**, BC4 — **6/6**. Записи завершены и повторно не снимаются.
После перепроверки `c7eed385b279` квалифицированы **10/13** постановок:
дверь REICH02 — **5,233 с / 53 решения**, улица верхнего квартала —
**42,700 с / 427 решений**. Лестница сохраняет отказ. По новому решению пользователя следование
включено с неквалифицированным эталоном; для №4 требование скалы снято.

Заявка `b0e833601016` завершилась: **exit 0**, фаза **880 с**, **54/54** записей,
из них переиспользованы **36**, добавлены **18**. Собственные Ryzen-сборки и кэши
удалены (`local_cleanup.status: complete`). Для новой фазы временно сохранены
точные копии своих бинарников и принятого навмеша; они удаляются после завершения.
Новая единая фаза включает следование — HD, it35 и BC4 по три seed,
без повторной квалификации учителя. В №4 выбирается validation-пара с видимой
целью и статическим препятствием, исключая стену дома/забор; после прохода
учителя без падений и с кромкой ≥1 м выполняются ещё девять записей трёх моделей.
Итого — **72** записи при найденном №4, **63** при его отказе. Готовые файлы
предыдущих фаз повторно не снимаются.

## Набор и источники

Состав десяти сценариев фиксирован. Старт, цель, NPC, время и seed заданы в
[`scenarios.json`](../../../scripts/nav-live/scenarios.json). Места заморожены (`places_frozen: true`): новые раунды сохраняют координаты,
цели, NPC и время суток; вода, личные точки распорядка и час рынка больше
не выбираются заново. Следование имеет явное разрешение `teacher_qualification_required:false` и
пометку «эталон не квалифицирован». Лестница остаётся исключённой; новый №4
фиксируется только после квалификации. `bc4_compare:true` добавляет BC4 для №3/4.
Изменение входов запрещает продолжение старого завершённого раунда.

| № | Сценарий | Место и источник | Что ещё проверить |
|---|---|---|---|
| 1 | Малет у забора | `(14926,1739,-15914)` → `FP_ROAM_FARM1_SHEEP_09`; [контроль 28,867 с](../2026-10-16-stuck/README.md); Detour 20,85 м | v3b прошёл за 26,433 с, 265 решений; запись проверена |
| 2 | Возврат с дороги | Три позиции из [PR #574](https://github.com/kaifaty/GothicAI/pull/574), `reports/physics/2026-10-30-routine-return/entry-on-trace.jsonl` коммита `971d9cd7a`: городской страж у дерева, Лобарт у края поля, Эрол у сосны; отклонения от waynet 11,65 / 12,01 / 16,13 м | Проход v3b к ближайшей точке личного распорядка; Вино заменён из-за заблокированной капсулы, координаты Лобарта скопированы без сдвига |
| 3 | Спутник за героем | Новый маршрут: рынок → узкие улицы → таверна → рынок | Эталон не квалифицирован: 28,246%, 14,955 м, 90 с стояния; три модели ×3 seed включены без квалификации |
| 4 | Видимая цель за статическим препятствием | Пара из тех же 79 видимых `detour-val`; прямую перекрывает статический объект или рельеф, исключая стены домов/заборы | Учитель success, 0 падений, кромка ≥1 м; затем HD/it35/BC4 ×3 |
| 5 | Тупик у стены | Новая координата `(1500,-186,1200)` у верфи: Detour 52,83 м, отношение 2,1667, snap 0,171 см; первый участок уходит от цели | Сухая опора, физическое подтверждение стены, v3b |
| 6 | Берег гавани | Новая пара на геометрии водных проб v5: `NW_CITY_WAY_TO_SHIP_08` → `NW_CITY_WAY_TO_SHIP_FISCHER_06` | v3b прошёл за 28,433 с, 0 глубоких заходов и падений |
| 7 | Тропа над обрывом | Safety-fixture `cliff-bridge-201-back200`, мост, link `1230208767`; [источник](../2026-10-22-teacher-v3b/README.md); верхний участок коридора до спуска, Y цели уточнён по физическому полу с 10614,061 до 10587,980 см, XZ сохранены | Кромка рядом с тропой, безопасный проход v3b |
| 8 | Рынок с распорядками | Новый переход через `NW_CITY_MERCHANT_PATH_20`, 06:59; максимум 6 движущихся NPC из 24 измеренных часов | Проход v3b; обычные NPC не удаляются и не замораживаются |
| 9 | Дверь и лестница раздельно | Дверь: тот же REICH02, `NW_CITY_UPTOWN_PATH_08 → NW_CITY_REICH02_01`, первый этаж, Y≈900 см. Лестница: прежние координаты и кадр ниже | Дверь прошла за 5,233 с. В 45 validation-переходах нет подтверждённого внутреннего подъёма ≥2 м; лестница не квалифицирована |
| 10 | Местный и чужак | Новый путь Фернандо по улице верхнего квартала: `NW_CITY_UPTOWN_PATH_08 → NW_CITY_UPTOWN_PATH_12`, прямая 40,670 м, граф 51,15 м, диапазон высот 15,71 см | Учитель прошёл за 42,700 с; новые записи HD/it35 и BC4 местный/чужак |

В `water-scenarios.jsonl` v5 все 98 кандидатов имеют split `train`: отдельного
водного validation в этом файле нет. Для №6 выбрана **новая** пара в гавани;
обе ориентации прежних train-пар исключены. Замороженные test-пары читаются
только как метаданные запрета; test-поездок и записей нет. Ветку routine-return
и её данные этот набор не изменяет.

Seed: **18102027, 18102028, 18102029**. В №2 каждому seed соответствует одна из
трёх позиций; остальные сценарии повторяют одно место на всех трёх seed.
Предыдущая часть содержит по **24** поездки HD/it35 и **6** BC4 для №10 —
всего **54**. Новые №3/4 добавляют каждой модели по три seed на сценарий.
При проходе №4: HD и it35 по **30**, BC4 — **12**, всего **72**. Дверь и лестница —
два отдельных варианта №9; исходные 12 постановок стали 13, прошли десять.

## Команда и хранение

```bash
scripts/nav-live.sh \
  --weights /mnt/shared/gothic-ai/archive/nav-dagger3-20261030/hd-run/policy.navbc,/mnt/shared/gothic-ai/archive/nav-ppo/run/iter-000035/update/policy.navbc \
  --round baseline
```

Команда подаёт фазу в `gq` и возвращает ID без ожидания и опроса очереди.
Для текущих №3/4 к той же команде добавляется `--stage extend`: одна заявка,
жёсткий предел 3300 с и проверка остатка ≥2800 с до игровых эпизодов.
Первый вызов — экономная Linux-сборка и подготовка геометрии/часа рынка.
Следующая фаза `qualify` проверяет физические признаки мест и записывает только
v3b — один эпизод на постановку, всего 12 на 10 сценариев. Готовый Малет
используется повторно. Отказ места с причиной не прерывает проверку остальных;
вода и рынок при неудаче остаются «не квалифицирован», без подмены типа сценария.
Результаты и выбранные места сохраняются на NAS в `qualified-suite.json`,
а таблица учителя с числами и ссылками — в README раунда. По решению пользователя
baseline сразу выполняется на **8** квалифицированных постановках, без ожидания
остальных четырёх. Первый baseline был разделён на три заявки; каждое дополнение подаётся одной.
`--stage extend` записывает №3 и квалифицирует/записывает №4 в одной заявке
без пересборки движка. Временные копии своих прежних бинарников и навмеша
сохраняют точные SHA и позволяют продолжить после уборки предыдущей фазы.
Каждая содержит неизменяемый снимок набора и копию бинарника по SHA-256;
поздняя перепроверка их не меняет. Перед первой
частью выполняется smoke; остальные используют тот же прошедший результат.
Отказ smoke останавливает baseline, без автоматических повторов той же ошибки.
Новые прошедшие места добавляются отдельным продолжением, готовые записи
повторно не снимаются. Исключённые места и причины остаются в таблице.
Каждая заявка ограничена 59 минутами, суммарное исполнение раунда —
10 800 с без времени ожидания очереди. Все веса читаются, выборка из softmax,
β=0. Память определяется весами; настройки политики, учителя, памяти и глаза
не меняются.

Отдельная проверка первой записи учителя, без других эпизодов:

```bash
scripts/nav-live.sh \
  --weights /mnt/shared/gothic-ai/archive/nav-dagger3-20261030/hd-run/policy.navbc,/mnt/shared/gothic-ai/archive/nav-ppo/run/iter-000035/update/policy.navbc \
  --round baseline --stage teacher
```

Эта заявка ограничена 10 минутами вместе со сборкой. Она берёт только принятое
место №1 из `baseline/qualified-suite.json`, записывает v3b и выпускает его
`source.mp4`, трассу, `topdown.mp4`, `timeline.png`, `summary.json`.

Результаты: `/mnt/shared/gothic-ai/nav-live/baseline/`. У каждого прогона:
`source.mp4` — боковая следящая камера bench, `live-decisions.jsonl` — решения
100 мс, `live-observations.bin.gz` — реальные uint8 near/far и попадания лучей
каждые 400 мс, `live-format.json` — масштабы и исходный Detour. Видео и трасса
пишутся одним процессом в одном прогоне.

Обработчик выпускает `topdown.mp4` ×2 с легендой классов, лучами, целью,
морковкой и путями ученика/v3b/Detour, `timeline.png`, `summary.json`.
Порог стояния/кружения — скорость <20 см/с; кружение дополнительно требует
ненулевой команды поворота. Упоры — начала контактов CV, stuck считается
отдельно по отсутствию продвижения. Глубокие входы и падения считаются по физическим
кадрам; расстояния до кромки/глубокой воды — пассивные пробы 16 направлений
радиусом 5 м, с шагом 10/25 см. Значение с `censored=true` означает ≥5 м,
а не точное расстояние. Safety показывает `stairBlockedByPolicy`, peer clipping
записывается отдельно. Источник каждой метрики и пороги есть в `summary.json`.

Завершённые данные не перезаписываются. Неполный собственный прогон можно
повторить; завершённую запись можно дообработать без поездки. Весам, сценарию
и бинарнику сопоставляются SHA-256. Предел раунда — 5 000 000 000 байт,
локальный резерв — 20 ГиБ. После полной записи квалифицированной части
baseline сам удаляет собственные build/venv/кэши. Маркер завершения перечисляет
исключения и отдельно показывает, что полный набор не квалифицирован.
Явный `--stage clean` использует те же проверки. NAS, исходные веса и соседние
задачи не затрагиваются.

Все сценарии имеют `training_eligible:false`; `nav_demos_v4_collect.directory_for`
запрещает их ID, составные ID вариантов и обе ориентации пар/звеньев маршрута.
`navdemos live on` требует `record off`: NAVDEMO для обучения не создаётся.

Графический bench разрешает `navdemos` только при `OPENGOTHIC_NAV_LIVE=1`;
launcher выставляет его и для записи, и для контроля без записи. Постановки
`live-guide player` и `live-target player` действуют одинаково в обоих режимах.
`live-probes NPC FILE` — отдельные пассивные запросы перед поездками: опора,
капсула, препятствие, видимость, глубокая вода и кромка; эти данные не передаются
политике или учителю.

Во всех десяти постановках старт героя выбирается одинаково: до 80
детерминированных точек внутри наземных полигонов Detour в кольце 2–6 м вокруг
NPC, с высотой самого полигона. Связная наземная область NPC имеет приоритет;
точки отсортированы по расстоянию. Физический луч идёт от высоты навмеша +2 м
вниз на 5 м; затем проверяются уклон, сухость и капсула на найденном полу.
Это действует и без записи. Для №3 выбранная точка — старт маршрутного героя;
остальные узлы маршрута сохраняются. Для №1 начальный путь героя к следу NPC
идёт по наземному Detour, без прыжковых связей. При отсутствии места сценарий
выдаёт `setup_rejected:true` с причиной `live_setup_no_safe_observer_position`,
без исключения игры и без рендера пустого эпизода. Проверенные кандидаты,
лучи и выбранный пол сохраняются в `live-guide-start.json` на NAS.

## Baseline

Числитель — приезды, знаменатель — требуемые seed. Для неполной записи число
готовых файлов указано отдельно. Медианный и худший seed ранжируются по приезду,
затем по времени и номеру seed; №2 сохраняет название каждого старта.

| № | Ориентир v3b | HD | it35 (прошлая модель) | BC4 | BC4 местный | BC4 чужак |
|---|---|---|---|---|---|---|
| 1 | 1/1, 26,433 с | 3/3; [медиана 18102029 / 01-maleth-fence: 24.633 с](/mnt/shared/gothic-ai/nav-live/baseline/HD/01-maleth-fence/seed-18102029/topdown.mp4), [видео](/mnt/shared/gothic-ai/nav-live/baseline/HD/01-maleth-fence/seed-18102029/source.mp4), [timeline](/mnt/shared/gothic-ai/nav-live/baseline/HD/01-maleth-fence/seed-18102029/timeline.png), [JSON](/mnt/shared/gothic-ai/nav-live/baseline/HD/01-maleth-fence/seed-18102029/summary.json); [худший 18102028 / 01-maleth-fence: 31.700 с](/mnt/shared/gothic-ai/nav-live/baseline/HD/01-maleth-fence/seed-18102028/topdown.mp4), [видео](/mnt/shared/gothic-ai/nav-live/baseline/HD/01-maleth-fence/seed-18102028/source.mp4), [timeline](/mnt/shared/gothic-ai/nav-live/baseline/HD/01-maleth-fence/seed-18102028/timeline.png), [JSON](/mnt/shared/gothic-ai/nav-live/baseline/HD/01-maleth-fence/seed-18102028/summary.json) | 3/3; [медиана 18102027 / 01-maleth-fence: 27.100 с](/mnt/shared/gothic-ai/nav-live/baseline/it35/01-maleth-fence/seed-18102027/topdown.mp4), [видео](/mnt/shared/gothic-ai/nav-live/baseline/it35/01-maleth-fence/seed-18102027/source.mp4), [timeline](/mnt/shared/gothic-ai/nav-live/baseline/it35/01-maleth-fence/seed-18102027/timeline.png), [JSON](/mnt/shared/gothic-ai/nav-live/baseline/it35/01-maleth-fence/seed-18102027/summary.json); [худший 18102028 / 01-maleth-fence: 27.100 с](/mnt/shared/gothic-ai/nav-live/baseline/it35/01-maleth-fence/seed-18102028/topdown.mp4), [видео](/mnt/shared/gothic-ai/nav-live/baseline/it35/01-maleth-fence/seed-18102028/source.mp4), [timeline](/mnt/shared/gothic-ai/nav-live/baseline/it35/01-maleth-fence/seed-18102028/timeline.png), [JSON](/mnt/shared/gothic-ai/nav-live/baseline/it35/01-maleth-fence/seed-18102028/summary.json) | — | — | — |
| 2 | 3/3: 17,633 / 98,033 / 19,067 с | 3/3; [медиана 18102029 / 02-routine-return-forest-pine: 14.200 с](/mnt/shared/gothic-ai/nav-live/baseline/HD/02-routine-return-forest-pine/seed-18102029/topdown.mp4), [видео](/mnt/shared/gothic-ai/nav-live/baseline/HD/02-routine-return-forest-pine/seed-18102029/source.mp4), [timeline](/mnt/shared/gothic-ai/nav-live/baseline/HD/02-routine-return-forest-pine/seed-18102029/timeline.png), [JSON](/mnt/shared/gothic-ai/nav-live/baseline/HD/02-routine-return-forest-pine/seed-18102029/summary.json); [худший 18102028 / 02-routine-return-farm-edge: 39.733 с](/mnt/shared/gothic-ai/nav-live/baseline/HD/02-routine-return-farm-edge/seed-18102028/topdown.mp4), [видео](/mnt/shared/gothic-ai/nav-live/baseline/HD/02-routine-return-farm-edge/seed-18102028/source.mp4), [timeline](/mnt/shared/gothic-ai/nav-live/baseline/HD/02-routine-return-farm-edge/seed-18102028/timeline.png), [JSON](/mnt/shared/gothic-ai/nav-live/baseline/HD/02-routine-return-farm-edge/seed-18102028/summary.json) | 3/3; [медиана 18102027 / 02-routine-return-forest-tree: 24.367 с](/mnt/shared/gothic-ai/nav-live/baseline/it35/02-routine-return-forest-tree/seed-18102027/topdown.mp4), [видео](/mnt/shared/gothic-ai/nav-live/baseline/it35/02-routine-return-forest-tree/seed-18102027/source.mp4), [timeline](/mnt/shared/gothic-ai/nav-live/baseline/it35/02-routine-return-forest-tree/seed-18102027/timeline.png), [JSON](/mnt/shared/gothic-ai/nav-live/baseline/it35/02-routine-return-forest-tree/seed-18102027/summary.json); [худший 18102028 / 02-routine-return-farm-edge: 88.833 с](/mnt/shared/gothic-ai/nav-live/baseline/it35/02-routine-return-farm-edge/seed-18102028/topdown.mp4), [видео](/mnt/shared/gothic-ai/nav-live/baseline/it35/02-routine-return-farm-edge/seed-18102028/source.mp4), [timeline](/mnt/shared/gothic-ai/nav-live/baseline/it35/02-routine-return-farm-edge/seed-18102028/timeline.png), [JSON](/mnt/shared/gothic-ai/nav-live/baseline/it35/02-routine-return-farm-edge/seed-18102028/summary.json) | — | — | — |
| 3 | эталон не квалифицирован: 28,246%, 14,955 м, 90 с стояния | 0/3 записано; новая заявка | 0/3 записано; новая заявка | 0/3 записано; новая заявка | — | — |
| 4 | новая квалификация статического препятствия | после прохода учителя | после прохода учителя | после прохода учителя | — | — |
| 5 | 1/1, 51,967 с | 2/3; [медиана 18102029 / 05-wall-pocket: 29.367 с](/mnt/shared/gothic-ai/nav-live/baseline/HD/05-wall-pocket/seed-18102029/topdown.mp4), [видео](/mnt/shared/gothic-ai/nav-live/baseline/HD/05-wall-pocket/seed-18102029/source.mp4), [timeline](/mnt/shared/gothic-ai/nav-live/baseline/HD/05-wall-pocket/seed-18102029/timeline.png), [JSON](/mnt/shared/gothic-ai/nav-live/baseline/HD/05-wall-pocket/seed-18102029/summary.json); [худший 18102028 / 05-wall-pocket: 90.000 с](/mnt/shared/gothic-ai/nav-live/baseline/HD/05-wall-pocket/seed-18102028/topdown.mp4), [видео](/mnt/shared/gothic-ai/nav-live/baseline/HD/05-wall-pocket/seed-18102028/source.mp4), [timeline](/mnt/shared/gothic-ai/nav-live/baseline/HD/05-wall-pocket/seed-18102028/timeline.png), [JSON](/mnt/shared/gothic-ai/nav-live/baseline/HD/05-wall-pocket/seed-18102028/summary.json) | 0/3; [медиана 18102028 / 05-wall-pocket: 90.000 с](/mnt/shared/gothic-ai/nav-live/baseline/it35/05-wall-pocket/seed-18102028/topdown.mp4), [видео](/mnt/shared/gothic-ai/nav-live/baseline/it35/05-wall-pocket/seed-18102028/source.mp4), [timeline](/mnt/shared/gothic-ai/nav-live/baseline/it35/05-wall-pocket/seed-18102028/timeline.png), [JSON](/mnt/shared/gothic-ai/nav-live/baseline/it35/05-wall-pocket/seed-18102028/summary.json); [худший 18102029 / 05-wall-pocket: 90.000 с](/mnt/shared/gothic-ai/nav-live/baseline/it35/05-wall-pocket/seed-18102029/topdown.mp4), [видео](/mnt/shared/gothic-ai/nav-live/baseline/it35/05-wall-pocket/seed-18102029/source.mp4), [timeline](/mnt/shared/gothic-ai/nav-live/baseline/it35/05-wall-pocket/seed-18102029/timeline.png), [JSON](/mnt/shared/gothic-ai/nav-live/baseline/it35/05-wall-pocket/seed-18102029/summary.json) | — | — | — |
| 6 | 1/1, 28,433 с | 3/3; [медиана 18102028 / 06-harbour-water: 46.533 с](/mnt/shared/gothic-ai/nav-live/baseline/HD/06-harbour-water/seed-18102028/topdown.mp4), [видео](/mnt/shared/gothic-ai/nav-live/baseline/HD/06-harbour-water/seed-18102028/source.mp4), [timeline](/mnt/shared/gothic-ai/nav-live/baseline/HD/06-harbour-water/seed-18102028/timeline.png), [JSON](/mnt/shared/gothic-ai/nav-live/baseline/HD/06-harbour-water/seed-18102028/summary.json); [худший 18102027 / 06-harbour-water: 46.667 с](/mnt/shared/gothic-ai/nav-live/baseline/HD/06-harbour-water/seed-18102027/topdown.mp4), [видео](/mnt/shared/gothic-ai/nav-live/baseline/HD/06-harbour-water/seed-18102027/source.mp4), [timeline](/mnt/shared/gothic-ai/nav-live/baseline/HD/06-harbour-water/seed-18102027/timeline.png), [JSON](/mnt/shared/gothic-ai/nav-live/baseline/HD/06-harbour-water/seed-18102027/summary.json) | 3/3; [медиана 18102028 / 06-harbour-water: 29.467 с](/mnt/shared/gothic-ai/nav-live/baseline/it35/06-harbour-water/seed-18102028/topdown.mp4), [видео](/mnt/shared/gothic-ai/nav-live/baseline/it35/06-harbour-water/seed-18102028/source.mp4), [timeline](/mnt/shared/gothic-ai/nav-live/baseline/it35/06-harbour-water/seed-18102028/timeline.png), [JSON](/mnt/shared/gothic-ai/nav-live/baseline/it35/06-harbour-water/seed-18102028/summary.json); [худший 18102029 / 06-harbour-water: 29.800 с](/mnt/shared/gothic-ai/nav-live/baseline/it35/06-harbour-water/seed-18102029/topdown.mp4), [видео](/mnt/shared/gothic-ai/nav-live/baseline/it35/06-harbour-water/seed-18102029/source.mp4), [timeline](/mnt/shared/gothic-ai/nav-live/baseline/it35/06-harbour-water/seed-18102029/timeline.png), [JSON](/mnt/shared/gothic-ai/nav-live/baseline/it35/06-harbour-water/seed-18102029/summary.json) | — | — | — |
| 7 | 1/1, 8,133 с | 1/3; [медиана 18102027 / 07-cliff-trail: 60.000 с](/mnt/shared/gothic-ai/nav-live/baseline/HD/07-cliff-trail/seed-18102027/topdown.mp4), [видео](/mnt/shared/gothic-ai/nav-live/baseline/HD/07-cliff-trail/seed-18102027/source.mp4), [timeline](/mnt/shared/gothic-ai/nav-live/baseline/HD/07-cliff-trail/seed-18102027/timeline.png), [JSON](/mnt/shared/gothic-ai/nav-live/baseline/HD/07-cliff-trail/seed-18102027/summary.json); [худший 18102029 / 07-cliff-trail: 60.000 с](/mnt/shared/gothic-ai/nav-live/baseline/HD/07-cliff-trail/seed-18102029/topdown.mp4), [видео](/mnt/shared/gothic-ai/nav-live/baseline/HD/07-cliff-trail/seed-18102029/source.mp4), [timeline](/mnt/shared/gothic-ai/nav-live/baseline/HD/07-cliff-trail/seed-18102029/timeline.png), [JSON](/mnt/shared/gothic-ai/nav-live/baseline/HD/07-cliff-trail/seed-18102029/summary.json) | 1/3; [медиана 18102027 / 07-cliff-trail: 60.000 с](/mnt/shared/gothic-ai/nav-live/baseline/it35/07-cliff-trail/seed-18102027/topdown.mp4), [видео](/mnt/shared/gothic-ai/nav-live/baseline/it35/07-cliff-trail/seed-18102027/source.mp4), [timeline](/mnt/shared/gothic-ai/nav-live/baseline/it35/07-cliff-trail/seed-18102027/timeline.png), [JSON](/mnt/shared/gothic-ai/nav-live/baseline/it35/07-cliff-trail/seed-18102027/summary.json); [худший 18102029 / 07-cliff-trail: 60.000 с](/mnt/shared/gothic-ai/nav-live/baseline/it35/07-cliff-trail/seed-18102029/topdown.mp4), [видео](/mnt/shared/gothic-ai/nav-live/baseline/it35/07-cliff-trail/seed-18102029/source.mp4), [timeline](/mnt/shared/gothic-ai/nav-live/baseline/it35/07-cliff-trail/seed-18102029/timeline.png), [JSON](/mnt/shared/gothic-ai/nav-live/baseline/it35/07-cliff-trail/seed-18102029/summary.json) | — | — | — |
| 8 | 1/1, 84,200 с | 2/3; [медиана 18102029 / 08-market-crowd: 87.733 с](/mnt/shared/gothic-ai/nav-live/baseline/HD/08-market-crowd/seed-18102029/topdown.mp4), [видео](/mnt/shared/gothic-ai/nav-live/baseline/HD/08-market-crowd/seed-18102029/source.mp4), [timeline](/mnt/shared/gothic-ai/nav-live/baseline/HD/08-market-crowd/seed-18102029/timeline.png), [JSON](/mnt/shared/gothic-ai/nav-live/baseline/HD/08-market-crowd/seed-18102029/summary.json); [худший 18102028 / 08-market-crowd: 90.000 с](/mnt/shared/gothic-ai/nav-live/baseline/HD/08-market-crowd/seed-18102028/topdown.mp4), [видео](/mnt/shared/gothic-ai/nav-live/baseline/HD/08-market-crowd/seed-18102028/source.mp4), [timeline](/mnt/shared/gothic-ai/nav-live/baseline/HD/08-market-crowd/seed-18102028/timeline.png), [JSON](/mnt/shared/gothic-ai/nav-live/baseline/HD/08-market-crowd/seed-18102028/summary.json) | 3/3; [медиана 18102027 / 08-market-crowd: 85.067 с](/mnt/shared/gothic-ai/nav-live/baseline/it35/08-market-crowd/seed-18102027/topdown.mp4), [видео](/mnt/shared/gothic-ai/nav-live/baseline/it35/08-market-crowd/seed-18102027/source.mp4), [timeline](/mnt/shared/gothic-ai/nav-live/baseline/it35/08-market-crowd/seed-18102027/timeline.png), [JSON](/mnt/shared/gothic-ai/nav-live/baseline/it35/08-market-crowd/seed-18102027/summary.json); [худший 18102029 / 08-market-crowd: 85.333 с](/mnt/shared/gothic-ai/nav-live/baseline/it35/08-market-crowd/seed-18102029/topdown.mp4), [видео](/mnt/shared/gothic-ai/nav-live/baseline/it35/08-market-crowd/seed-18102029/source.mp4), [timeline](/mnt/shared/gothic-ai/nav-live/baseline/it35/08-market-crowd/seed-18102029/timeline.png), [JSON](/mnt/shared/gothic-ai/nav-live/baseline/it35/08-market-crowd/seed-18102029/summary.json) | — | — | — |
| 9 | дверь 1/1, 5,233 с; лестница не квалифицирована | 3/3; [медиана 18102028: 2.567 с](/mnt/shared/gothic-ai/nav-live/baseline/HD/09-door-stairs-door/seed-18102028/topdown.mp4), [видео](/mnt/shared/gothic-ai/nav-live/baseline/HD/09-door-stairs-door/seed-18102028/source.mp4), [timeline](/mnt/shared/gothic-ai/nav-live/baseline/HD/09-door-stairs-door/seed-18102028/timeline.png), [JSON](/mnt/shared/gothic-ai/nav-live/baseline/HD/09-door-stairs-door/seed-18102028/summary.json); [худший 18102029: 2.567 с](/mnt/shared/gothic-ai/nav-live/baseline/HD/09-door-stairs-door/seed-18102029/topdown.mp4), [видео](/mnt/shared/gothic-ai/nav-live/baseline/HD/09-door-stairs-door/seed-18102029/source.mp4), [timeline](/mnt/shared/gothic-ai/nav-live/baseline/HD/09-door-stairs-door/seed-18102029/timeline.png), [JSON](/mnt/shared/gothic-ai/nav-live/baseline/HD/09-door-stairs-door/seed-18102029/summary.json) | 3/3; [медиана 18102029: 34.433 с](/mnt/shared/gothic-ai/nav-live/baseline/it35/09-door-stairs-door/seed-18102029/topdown.mp4), [видео](/mnt/shared/gothic-ai/nav-live/baseline/it35/09-door-stairs-door/seed-18102029/source.mp4), [timeline](/mnt/shared/gothic-ai/nav-live/baseline/it35/09-door-stairs-door/seed-18102029/timeline.png), [JSON](/mnt/shared/gothic-ai/nav-live/baseline/it35/09-door-stairs-door/seed-18102029/summary.json); [худший 18102028: 43.133 с](/mnt/shared/gothic-ai/nav-live/baseline/it35/09-door-stairs-door/seed-18102028/topdown.mp4), [видео](/mnt/shared/gothic-ai/nav-live/baseline/it35/09-door-stairs-door/seed-18102028/source.mp4), [timeline](/mnt/shared/gothic-ai/nav-live/baseline/it35/09-door-stairs-door/seed-18102028/timeline.png), [JSON](/mnt/shared/gothic-ai/nav-live/baseline/it35/09-door-stairs-door/seed-18102028/summary.json) | — | — | — |
| 10 | улица 1/1, 42,700 с | 3/3; [медиана 18102029: 16.567 с](/mnt/shared/gothic-ai/nav-live/baseline/HD/10-local-stranger/seed-18102029/topdown.mp4), [видео](/mnt/shared/gothic-ai/nav-live/baseline/HD/10-local-stranger/seed-18102029/source.mp4), [timeline](/mnt/shared/gothic-ai/nav-live/baseline/HD/10-local-stranger/seed-18102029/timeline.png), [JSON](/mnt/shared/gothic-ai/nav-live/baseline/HD/10-local-stranger/seed-18102029/summary.json); [худший 18102028: 19.167 с](/mnt/shared/gothic-ai/nav-live/baseline/HD/10-local-stranger/seed-18102028/topdown.mp4), [видео](/mnt/shared/gothic-ai/nav-live/baseline/HD/10-local-stranger/seed-18102028/source.mp4), [timeline](/mnt/shared/gothic-ai/nav-live/baseline/HD/10-local-stranger/seed-18102028/timeline.png), [JSON](/mnt/shared/gothic-ai/nav-live/baseline/HD/10-local-stranger/seed-18102028/summary.json) | 3/3; [медиана 18102027: 42.733 с](/mnt/shared/gothic-ai/nav-live/baseline/it35/10-local-stranger/seed-18102027/topdown.mp4), [видео](/mnt/shared/gothic-ai/nav-live/baseline/it35/10-local-stranger/seed-18102027/source.mp4), [timeline](/mnt/shared/gothic-ai/nav-live/baseline/it35/10-local-stranger/seed-18102027/timeline.png), [JSON](/mnt/shared/gothic-ai/nav-live/baseline/it35/10-local-stranger/seed-18102027/summary.json); [худший 18102028: 43.600 с](/mnt/shared/gothic-ai/nav-live/baseline/it35/10-local-stranger/seed-18102028/topdown.mp4), [видео](/mnt/shared/gothic-ai/nav-live/baseline/it35/10-local-stranger/seed-18102028/source.mp4), [timeline](/mnt/shared/gothic-ai/nav-live/baseline/it35/10-local-stranger/seed-18102028/timeline.png), [JSON](/mnt/shared/gothic-ai/nav-live/baseline/it35/10-local-stranger/seed-18102028/summary.json) | — | 3/3; [медиана 18102028: 50.200 с](/mnt/shared/gothic-ai/nav-live/baseline/bc4/10-local-stranger-local/seed-18102028/topdown.mp4), [видео](/mnt/shared/gothic-ai/nav-live/baseline/bc4/10-local-stranger-local/seed-18102028/source.mp4), [timeline](/mnt/shared/gothic-ai/nav-live/baseline/bc4/10-local-stranger-local/seed-18102028/timeline.png), [JSON](/mnt/shared/gothic-ai/nav-live/baseline/bc4/10-local-stranger-local/seed-18102028/summary.json); [худший 18102027: 59.400 с](/mnt/shared/gothic-ai/nav-live/baseline/bc4/10-local-stranger-local/seed-18102027/topdown.mp4), [видео](/mnt/shared/gothic-ai/nav-live/baseline/bc4/10-local-stranger-local/seed-18102027/source.mp4), [timeline](/mnt/shared/gothic-ai/nav-live/baseline/bc4/10-local-stranger-local/seed-18102027/timeline.png), [JSON](/mnt/shared/gothic-ai/nav-live/baseline/bc4/10-local-stranger-local/seed-18102027/summary.json) | 3/3; [медиана 18102028: 35.133 с](/mnt/shared/gothic-ai/nav-live/baseline/bc4/10-local-stranger-stranger/seed-18102028/topdown.mp4), [видео](/mnt/shared/gothic-ai/nav-live/baseline/bc4/10-local-stranger-stranger/seed-18102028/source.mp4), [timeline](/mnt/shared/gothic-ai/nav-live/baseline/bc4/10-local-stranger-stranger/seed-18102028/timeline.png), [JSON](/mnt/shared/gothic-ai/nav-live/baseline/bc4/10-local-stranger-stranger/seed-18102028/summary.json); [худший 18102027: 57.467 с](/mnt/shared/gothic-ai/nav-live/baseline/bc4/10-local-stranger-stranger/seed-18102027/topdown.mp4), [видео](/mnt/shared/gothic-ai/nav-live/baseline/bc4/10-local-stranger-stranger/seed-18102027/source.mp4), [timeline](/mnt/shared/gothic-ai/nav-live/baseline/bc4/10-local-stranger-stranger/seed-18102027/timeline.png), [JSON](/mnt/shared/gothic-ai/nav-live/baseline/bc4/10-local-stranger-stranger/seed-18102027/summary.json) |

Готово **54/54** записей предыдущей части. В этих 54: глубокие заходы — **0**; падения HD — **2**,
it35 — **0**. Полная таблица с ориентиром, всеми ссылками и новыми записями
обновляется на NAS: `/mnt/shared/gothic-ai/nav-live/baseline/README.md`.

Повторяемость: **одинаковый seed дал одинаковые действия; запись вкл/выкл — одинаковые действия**.
Малет, HD, seed 18102027: три прогона по **9,133 с / 92 решения**; SHA-256
`actions.txt` во всех трёх — `458694a0ad190875aeb016516db0f2bd27bda2380083b3160174987e33d37b60`.

Заявки первой части: `ee9c7f35a9fa` — exit 0 / 876 с, `368f42fc1773` —
exit 0 / 1597 с, `1b7445ad4b57` — exit 0 / 1474 с; перепроверка
`c7eed385b279` — exit 0 / 1224 с. Время gq включает ожидание блокировки
своей предыдущей фазы. Непосредственные фазы — **875 / 721 / 753 / 471 с**;
вместе с `b0e833601016` по `runtime.json` учтено **6241/10 800 с**, остаток
**4559 с**. Размер раунда перед новой фазой — **1 304 636 603 байта**
из 5 000 000 000. Оценка новой фазы — около **2800 с**, жёсткий предел —
**3300 с** и остаток общего бюджета; максимальная сумма — **9541/10 800 с**.
Эпизод учителя follow180 с занимал **151,855 с** с загрузкой и отрисовкой;
максимум среди прежних учеников с горизонтом90 с — **93,649 с**. В оценку
входят девять follow, до трёх попыток учителя90 с и девять обходов90 с.

## Перепроверка после квалификации

Заявка `f5d9e97ee028`: **exit 0, 737 с**, 8 приездов / 12 постановок,
четыре отказа. Один приехавший имеет падение. Учтённое время по `runtime.json` —
**2116/10 800 с**, остаток **8684 с**. Заявка `1f348d3ce53e` проверила пять размещений ниже (четыре ситуации, дом
имел две цели), сохранив семь остальных записей. После неё квалифицированы 8/12;
учтено **2541/10 800 с**, остаток **8259 с**. Размер NAS перед новыми заявками —
**380 461 206 байт** из 5 000 000 000; на Ryzen свободно около 98 ГБ.

| Постановка | Факты старого прогона | Изменение постановки |
|---|---|---|
| 02 farm-edge | На 90 с `(12892,825; 1646,482; −15750,302)`, до цели по прямой **7,333 м**. На 89,9 с скорость 71,956 см/с, вперёд, без stuck/контакта; последние 5 с — около 3,55 м смещения | Предел **180 с**, тот же старт и ближайшая личная точка Лобарта |
| 03 follow-hero | `target_down` на 56,367 с, 53,267 с стояния учителя; 526 решений `start_not_found`. В `events.jsonl` есть только roads/rec/output, сообщения атаки/смерти отсутствуют; записанный HP героя — 40 до остановки. Точная причина смерти старой трассой не установлена | `immortal player on`. Код moving переводит героя в `AiNormal`, тогда `isPlayer()` становится false и `cheat god` не защищает `changeAttribute`. Флаг IMMORTAL от этого режима не зависит. Проверяются четыре звена прежнего маршрута; после эпизода печатается HP героя |
| 04 visible-detour | Фактическое место — **PASS → NW_PASS_08**, NPC Ксардас. Без опоры 20 кадров на 5,700–6,333 с, Y 4104,385→3649,766 см: **4,546 м вниз**, без native jump/climb. Это реальный срыв; эталон исключён | Новый старт в 25% первого ребра записанного Detour: `(32878,188; 4997,164; −33336,293)`, сдвиг **2,838 м**. Пол перепроверяется и Y проецируется на физическую поверхность; прежняя цель и обход сохраняются. Приезд с любым падением не квалифицируется |
| 09 door-stairs | Дверной проём пройден. Максимум Y **1105,188 см** на 16,700 с — первая площадка лестницы; затем возврат на нижний этаж. На 60 с `(10911,639; 900,028; −2488,788)`, цель Y=1303,144 см | Тот же дом, старт и цель; один повтор. При повторном отказе — «не квалифицирован», место не заменяется |
| 10 local-stranger | Максимум Y **1144,984 см** на 14,700 с — выше первой площадки, ниже второго этажа. На 60 с `(10729,408; 900,031; −2502,468)`, цель Y=1300,045 см | Тот же дом и путь; один повтор. Повторный отказ сохраняется как ограничение v3b на этой постановке |

Кадры взяты из тех же `topdown.mp4`: время видео 30 с соответствует времени
прогона 60 с (×2). Полные видео и трассы остаются на NAS.

![№9: нижний этаж на 60 с](house09-topdown-60s.png)

![№10: нижний этаж на 60 с](house10-topdown-60s.png)

Команда выборочной перепроверки:

```bash
scripts/nav-live.sh \
  --weights /mnt/shared/gothic-ai/archive/nav-dagger3-20261030/hd-run/policy.navbc,/mnt/shared/gothic-ai/archive/nav-ppo/run/iter-000035/update/policy.navbc \
  --round baseline --stage requalify
```

Она ограничена 59 минутами, не запускает учеников и сохраняет прежние неудачные
записи. В текущей перепроверке меняются только №3, №4, дверь №9 и улица №10.
Восемь прошедших постановок используют прежний закреплённый бинарник и прежние
ориентиры; новый бинарник нужен только для шага героя и новых мест.

Для №3 неизменённый порог — **80%** времени после первого сближения в полосе
**2–5 м**. Новый учитель: **50 833/179 967 мс = 28,245734%**; `summary.json`
по частоте решений — **28,277778%**. Средняя дистанция до героя с весами
по времени — **14,954769 м**, медиана отсчётов — **9,706487 м**.

| Метрика скорости | Герой | Ведомая |
|---|---:|---:|
| Средняя за весь прогон, м/с | 0,955311 | 0,932754 |
| Медиана, м/с | 0,999999 | 0,195633 |
| Максимум, м/с | 1,000111 | 3,683058 |
| Средняя при движении ≥0,2 м/с, м/с | 0,998000 | 1,862589 |

Герой шёл **100 см/с**, без бега, с прежними остановками и разворотами;
горизонт **180 с**. Ведомая стояла **90 с**, кружилась **10,7 с**;
`start_not_found` — **755/1800** решений. Падения и глубокие заходы — **0**.
Средняя дистанция получена из `target-trajectory.txt` (левый отсчёт, вес —
длительность интервала); скорость героя — из смещения цели, ведомой —
из `live-decisions.jsonl`. По решению пользователя на том же месте запускаются HD, it35 и BC4 ×3 seed.
В сравнении используются доля в полосе, средняя дистанция и время стояния;
эталон помечен неквалифицированным. Медианный/худший seed для следования
ранжируются по доле в полосе, затем по дистанции и стоянию.

Для №4 новая поездка учителя **не запускалась**: из **121** validation-пары
**93** имеют поддерживаемый Detour, у **79** цель видна; положительных
ROCK/FELS/CLIFF-свидетельств — **0**, кандидатов на безопасный проход — **0**.
Из 121 физического луча 66 дали попадания, в 56 из этих попаданий имя сектора
пустое; оставшиеся 10 содержат имена стен. Это отсутствие подтверждения типа,
а не доказательство отсутствия камней. Неоднозначный STONE не принимался.
Числа и пустой список `teacher_attempts` сохранены в
`baseline/qualification/recheck-1c9a1fde8d40-a2964efaf4f4/rock-attempts.json`
на NAS. После снятия требования скалы №4 проверяется заново по статическому
препятствию; старые ROCK/FELS/CLIFF-пробы и записи остаются неизменными.

Для лестницы проверочные 45 переходов относятся к TROLLAREA_PORTAL/PORTALTEMPEL,
а не лестницам внутри дома. Подтверждённого подъёма внутри здания ≥200 см нет.
Прежний REICH02 сохраняется как отрицательный результат: старт
`(11253,981; 900,005; −3067,747)`, цель
`(10678,160; 1303,144; −2519,752)`, два timeout по 90 с; кадр выше.

## Проверки

- Новая единая фаза: целевые Python-проверки с фильтром `live_` —
  **7 passed**, 140 deselected, **0,98 с**. Fixture проверяет отказ до запуска
  при бюджете <2800 с, восстановление своих точных бинарников/навмеша,
  включение follow без повторного учителя, запрет стены/забора, разрешение
  статической стойки, сохранение готовых ссылок и один вызов baseline.
  Проверены средняя дистанция по физической трассе и обновление только
  `summary.json`; видео и трасса не перезаписываются.
- `b0e833601016`: **exit 0**, фаза **880 с**, **54/54** полных записей,
  `reused_runs:36`, `newly_completed_runs:18`, уборка завершена. Все 54 заново
  проверены по фактическим входам и комплекту файлов. Приезды: HD **20/24**,
  it35 **19/24**, BC4 **6/6**; глубокие заходы **0**, падения HD **2**.
- Дополнение: целевые `test_nav_drive.py` / `test_nav_demos_v4_collect.py`
  с фильтром `live_` — **6 passed**, 140 deselected, **0,84 с**. Существующий
  fixture проверяет сохранение 36 готовых и добавление 18, включая шесть BC4,
  правильный ориентир местного/чужака, завершение квалифицированной части,
  уборку и её продолжение после обрыва, повтор команды после уборки,
  неизменность мест/часа рынка и отдельную дверь без метаданных распорядка.
- Первая часть baseline: **36/36** записей прошли проверку файлов, завершённости
  видео/трассы и идентичности входов. Дополнение содержит только **18** недостающих
  записей. Mac-сборка этого worktree удалена: **434 229 248 байт**, также удалены
  собственные Python/pytest-кэши. Ryzen-уборка выполняется внутри завершающей заявки
  только при готовности **54/54** и совпадении набора, весов и smoke.
- Текущие изменения: целевые `test_nav_drive.py` / `test_nav_demos_v4_collect.py`
  с фильтром `live_` — **6 passed**, 140 deselected, 0,61 с. Проверяются запись
  и отрисовка fixture, запрет train, закрепление старого бинарника для восьми
  постановок, отдельная дверь/лестница, ROCK вместо неоднозначного STONE,
  порог кромки 100 см, уличный путь и продолжение без повторной записи.
  Mac: изменённый модуль, контракт и `Gothic2Notr` собраны, exit 0;
  `--live-format` — 2 записи по 125 874 байта, метаданные шага 100 см/с.
  В добавленных C++-строках **0** найденных аналогов прежнего ведущего `if`
  с последующим незащищённым оператором. Shell/Python syntax и diff-check — exit 0.
- До правки размещения, целевые Python-файлы `test_nav_drive.py` и `test_nav_demos_v4_collect.py`:
  **143 passed**, 0 failed. Fixture проверяет формат, отрисовку, ×2, метрики
  и отказ на обрезанной трассе; отдельно проверены продолжение обрезанного JSONL
  без повтора готовой группы и отказ при заблокированной капсуле. Второй файл —
  запрет попадания live в train.
- Mac: `Gothic2Notr` и `gothic-navpolicy-contract` собраны Apple clang, exit 0;
  `--live-format`: 2 записи, 55 байт на луч, 125 874 байта на запись, exit 0.
- После общего поиска размещения: Mac-сборка `Gothic2Notr`,
  `gothic-navpolicy-contract`, `gothic-navmesh-contract` — exit 0.
  `gothic-navmesh-contract --nearby-ground` — exit 0, 80 точек на приподнятом
  большом полигоне, 44 при исходной позе выше пола; проверены кольцо 2–6 м,
  высота поверхности, приоритет связной области и повторяемость.
  Целевые `test_nav_drive.py -k live_`: **3 passed**, 19 deselected;
  включён отказ сцены с сохранением диагностики, без рендера и повторного запуска.
- Read-only preflight: 12 постановок / 30 seed на модель, 0 отсутствующих
  waypoint и 0 совпадений с 917 защищёнными test-парами. Прогонов test нет.
- Исходная геометрия подготовлена; физические признаки приняты для 9/12
  постановок. Проход v3b, живой smoke, повтор seed и запись on/off,
  полный baseline пока не проверены.

- Квалификация `84ba32ddb6e1`: **exit 1, 344 с**, ожидание 7390 с, пик памяти
  6,3 ГБ. Приняты физические признаки 9/12 постановок; для Вино в поле,
  скалы и верхней тропы подходящее место ещё не найдено. Первая запись v3b
  `teacher/01-maleth-fence-5b5550836751` оборвалась на размещении героя:
  `nav live: player guide start is not supported`, до первого решения.
  В этом логе чисел опоры нет. Следующая попытка записала их в
  `live-guide-start.json`.
  В исходном `game.log` команда `seed 18102027` напечатала `seed 18102028`:
  общий bench разбирал число как float. Только в режиме `OPENGOTHIC_NAV_LIVE=1`
  seed теперь разбирается целым; seed выборки действий уже разбирался целым.
  Учтённое машинное время двух фаз — **761/10 800 с** (по `runtime.json`).

- Отдельный эпизод `85a6a84fd603`: **exit 1, 213 с**, сама игра — 29 с,
  решений — 0. В `teacher/01-maleth-fence-6a3952ba5d82/live-guide-start.json`:
  запрошена точка `(15225,929; 1739; −15920,520)`, `floor_found:false`,
  `capsule_clear:false`; проекция навмеша — 94,511 см, из них 67,643 см по XZ
  и +66,006 см по Y. Восстановление нормали не помогло при отсутствии попадания
  в пол. Фиксированное смещение удалено, применяется общий поиск выше.
  Команда seed в этой попытке сохранила точное значение **18102027**.
  Учтённое машинное время трёх фаз — **973/10 800 с** (по `runtime.json`).
  Следующая проверка ограничена одним эпизодом и 10 минутами; при её неудаче
  дальнейшие прогоны на этом месте останавливаются.

- Заявка `481148fbba42` остановилась на GCC-сборке через **178 с**:
  `navdemos.cpp:296`, `-Werror=misleading-indentation`. Игра и проверка
  размещения не запускались; это не дополнительная попытка размещения.
  Условный вывод запятой и безусловное `first=false` разнесены по строкам.
  Поиск по добавленным C++-строкам всего PR после правки: **0** аналогичных
  ведущих `if` со следующим незащищённым оператором в той же строке.
  Учтённое машинное время четырёх фаз — **1151/10 800 с** (`runtime.json`).

- Отдельный эпизод `5595870ad9c7`: **exit 0, 230 с**. Учитель Малета дошёл
  за **26,433 с**, **265 решений**, путь **22,243 м**, падений **0**, глубоких
  заходов **0**. Получены 67 кадров входов/лучей и все выходы. Боковое видео —
  640×360, 397 кадров, 26,466667 с; topdown — 960×720, 265 кадров, 13,25 с.
  Папка на NAS: `baseline/teacher/01-maleth-fence-59619ac71331/`;
  `source.mp4`, `topdown.mp4`, `timeline.png`, `summary.json` проверены.
  Первый из 80 кандидатов стартового пола принят: навмеш Y=1738,508 см,
  луч с Y=1938,508 см нашёл физический пол Y=1722,971 см, капсула свободна.
  Учтённое машинное время — **1380/10 800 с**, остаток **9420 с**;
  размер папки раунда — **46 МиБ** (`du`), предел — 5 000 000 000 байт.
  Новый целевой прогон тестов: **6 passed**, 140 deselected; проверены
  последовательность «только учитель → smoke → baseline», сохранение отказов,
  полный ряд 10 сценариев в таблице, ожидание рынка после телепорта и запрет train.

- Подготовка `fc42c24fed0a`: **exit 0, 418 с**, 37 пар Detour, 180 водных
  кандидатов и 24 часа рынка. Полные данные — `baseline/preparation/` на NAS.
  Работник гавани: `disconnected`; водных пересечений среди исходных 180 пар — 0.
  Для следующей фазы расширена выборка на точки `NW_CITY_WAY_TO_SHIP_*`.
  Пик рынка — 06:59, 6 движущихся NPC в радиусе 30 м при смещении ≥50 см за 10 с
  после 4 с подготовки; это максимум среди 24 окон этого census.

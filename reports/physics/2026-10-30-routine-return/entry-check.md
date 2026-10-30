# Вход в сеть распорядка по земле: ON против OFF

**Итог: needs_decision — остаётся Хильда (`farm_stable`).** ON: **8 PASS / 4 FAIL**, все **12 основных + 4/4 повтора**, `complete=true`, **exit 1, 659 с**. OFF: **7 PASS / 5 FAIL**, все **12 основных + 5/5 повторов**, `complete=true`, **exit 1, 633 с**. Все **33/33 подготовки валидны**, SIGSEGV нет.

`harbour_wall`, `tower_room` и `below_cliff` в ON получили PASS. Для трёх остальных исходных закрытых подходов (`city_garden`, `city_wall`, `harbour_shore`) выполнена согласованная альтернатива: **0 прошедших testWalk во всём радиусе 30 м и исчерпание трёх перевыборов**, в основных и повторных случаях. OFF воспроизвёл FAIL всех четырёх исходных закрытых подходов. Все случаи, которые проходили в предыдущем ON, продолжают проходить.

Хильда сохраняет FAIL в обоих режимах и повторах. В ON: **0/9** прямых наземных подходов после подъёма, **0/12** при исчерпании, все отказы `slope`. Условия пункта готовности «Хильда PASS» не выполнены; другой механизм движения не добавлялся. Принятые измерения до исправления сохранены в [исходном отчёте](README.md) и в истории предыдущего [сравнения](https://github.com/kaifaty/GothicAI/blob/8af2828db53280a559111ad997a45e5f861c7182/reports/physics/2026-10-30-routine-return/entry-check.md). Текущие ON/OFF выполнены на одном коде движка/стенда `0de79501637`, с одной базой `3436f2bee419…`, через `gq`, `RYZEN_CLEAN_IDLE_HOURS=0`.

## Как работает сейчас

`routine_entry_check=1` выбирает первый из 8 ближайших waypoint в 30 м, который проходит **`Npc::testWalk`**. Шаг XZ не больше `stepSz` родной капсулы; луч вниз ищет опору, проверяются подъём, спуск, нормаль, вода и пересечение капсулы с ландшафтом/статикой. NPC и рэгдоллы не препятствия для этой проверки. Проверка не перемещает NPC и не меняет его контроллер, физические тела или кэши. Выполняется при построении пути, а не каждый кадр.

Нативные пределы взяты из MoveAlgo: подъём `stepHeight()`, спуск **`0.75 × falldownHeight()`** — предел обычного NPC при [ходьбе](../../../OpenGothic/common/game/movealgo.cpp#L531), уклон `acos(slideAngle())`, глубокая вода — `waterDepthChest()`. Фактические значения каждого NPC записаны в `routine_return_up`. Отказ содержит `step_up`, `drop`, `slope`, `blocked`, `no_ground` или `water` и индекс первого неудачного шага. Во всех 33 наблюдениях пределы одинаковы: **подъём 60 см, спуск 375 см, уклон 50°, глубокая вода 140 см**. Проверка — [`NpcItem::testWalk`](../../../OpenGothic/common/physics/dynamicworld.cpp#L2802).

Если за 8 с активного подхода к входу NPC не сместился на 1 м, вход заносится в его временный список плохих точек и путь строится снова. Не больше 3 перевыборов; следующий такой застой даёт `exhausted=true` и прежний выбор. При 0 прошедших проверку сохраняется прежний fallback: в этом случае события могут повторять один старый вход, это не три разных доступных входа. Метки сбрасываются при входе в сеть, смене конкретной TA (в том числе с тем же waypoint), изменении распорядков и загрузке NPC. OFF не выполняет перевыбор.

`routine_entry_check=0` сохраняет прежний алгоритм выбора; диагностические `testWalk` стенда не назначают NPC путь. Marvin: `toggle routine_entry_check`; запуск: `ROUTINE_ENTRY_CHECK=1/0`. Combat/freepoint-пути, скорости/бег распорядка, `ragdoll_quest_guard`, P30, обучаемая навигация, navmesh и `docs/physics.md` не менялись.

Стенд сохраняет 12 NPC и позиции предыдущего раунда: NEWWORLD, полдень, настоящий KO, `standup` после родного `settled()` (ожидание до 20 с), штатный ИИ после подъёма. Перед следующей подготовкой NPC восстанавливается исходным живым архивом. Герой заморожен в единой `NW_PATH_TO_MONASTER_MONSTER22` дальше 30 м. Состав, районы и типы мест — в [исходной таблице](README.md#основные-12-случаев); текущие дистанции ниже.

Лимит фиксируется после подъёма: **max(90 с, 1,5 × маршрут / 0,8 м/с)**, маршрут включает подход к выбранному входу и граф до TA. Скачки >1 м при подъёме — FAIL; MOB-выравнивания и возвраты защиты пишутся отдельно. FAIL «ниже мира» — только низ физической query-капсулы ниже найденной опоры >20 см или ниже нижней границы мира. Берётся физическая позиция, а не корень анимации; луч опоры начинается у верха капсулы +1 см, чтобы не захватывать потолок над ней. Корень и стопы остаются диагностикой.

## Основные 12 случаев

Все длины в метрах, время в секундах симуляции после подъёма. `d₀` — расстояние от подготовленного старта до сети (одинаково ON/OFF). `d↑ / L` — расстояние до ближайшего waypoint после подъёма / расчётная длина маршрута. Время — **лимит / приход**; `>` означает тайм-аут.

| Случай | d₀ | ON d↑ / L | ON лимит / приход | ON | OFF d↑ / L | OFF лимит / приход | OFF |
|---|---:|---:|---:|---|---:|---:|---|
| `city_garden` | 11.18 | 11.61 / 36.03 | 90.00 / >90.00 | [FAIL](entry-on-trace.jsonl#L37) | 11.61 / 36.03 | 90.00 / >90.00 | [FAIL](entry-off-trace.jsonl#L28) |
| `city_wall` | 11.28 | 11.62 / 101.63 | 190.56 / >190.56 | [FAIL](entry-on-trace.jsonl#L80) | 11.62 / 101.63 | 190.56 / >190.56 | [FAIL](entry-off-trace.jsonl#L62) |
| `harbour_shore` | 12.01 | 11.49 / 11.49 | 90.00 / >90.00 | [FAIL](entry-on-trace.jsonl#L116) | 11.49 / 11.49 | 90.00 / >90.00 | [FAIL](entry-off-trace.jsonl#L89) |
| `harbour_wall` | 11.72 | 11.72 / 34.20 | 90.00 / 30.27 | [PASS](entry-on-trace.jsonl#L142) | 11.72 / 34.20 | 90.00 / 30.27 | [PASS](entry-off-trace.jsonl#L115) |
| `tower_room` | 11.88 | 12.08 / 29.88 | 90.00 / 73.80 | [PASS](entry-on-trace.jsonl#L165) | 12.08 / 85.11 | 159.57 / 61.67 | [PASS](entry-off-trace.jsonl#L137) |
| `farm_field` | 11.22 | 11.34 / 47.63 | 90.00 / 40.13 | [PASS](entry-on-trace.jsonl#L183) | 11.34 / 43.80 | 90.00 / 37.30 | [PASS](entry-off-trace.jsonl#L154) |
| `farm_stable` | 19.14 | 19.00 / 76.28 | 143.03 / >143.03 | [FAIL](entry-on-trace.jsonl#L221) | 19.00 / 76.28 | 143.03 / >143.03 | [FAIL](entry-off-trace.jsonl#L183) |
| `forest_tree` | 11.65 | 11.76 / 39.25 | 90.00 / 28.73 | [PASS](entry-on-trace.jsonl#L237) | 11.76 / 34.09 | 90.00 / 24.67 | [PASS](entry-off-trace.jsonl#L199) |
| `forest_pine` | 16.13 | 11.55 / 80.15 | 150.28 / 76.60 | [PASS](entry-on-trace.jsonl#L258) | 11.55 / 80.01 | 150.01 / 75.77 | [PASS](entry-off-trace.jsonl#L220) |
| `fenced_field` | 12.11 | 11.22 / 104.37 | 195.69 / 125.20 | [PASS](entry-on-trace.jsonl#L284) | 11.22 / 104.37 | 195.69 / 125.20 | [PASS](entry-off-trace.jsonl#L246) |
| `below_cliff` | 12.90 | 12.33 / 298.64 | 559.95 / 350.90 | [PASS](entry-on-trace.jsonl#L334) | 12.33 / 77.69 | 145.66 / >145.66 | [FAIL](entry-off-trace.jsonl#L284) |
| `farm_edge` | 12.01 | 11.66 / 106.36 | 199.42 / 132.77 | [PASS](entry-on-trace.jsonl#L361) | 11.66 / 103.70 | 194.43 / 126.87 | [PASS](entry-off-trace.jsonl#L310) |

`below_cliff`: ON выбрал `NW_FOREST_PATH_04_14_MONSTER` на расстоянии **27,77 м** от центра NPC. Прямой подход проходит testWalk, но графовый маршрут до TA длинный: **298,64 м**, лимит **559,95 с**, приход **350,90 с**. OFF выбрал прежний `NW_FARM2_OUT_02` и не дошёл за свой лимит **145,66 с**. Это [разные входы](entry-on-trace.jsonl#L292) при том же NPC и старте.

В башне первый проверенный вход `NW_XARDAS_TOWER_IN1_30` всё же дал застой; [перевыбор](entry-on-trace.jsonl#L156) исключил его и обычный ИИ дошёл до TA за **73,80 с**. Проверка прямого подхода не оптимизирует длину графа: сохраняется требование выбирать первый прошедший кандидат по расстоянию.

### Штатные повторы FAIL

PASS основных случаев не повторяется. Получены все 4 повтора ON и все 5 OFF; для них также приведены L и лимит.

| Случай | ON повтор: L; лимит / приход; результат | OFF повтор: L; лимит / приход; результат |
|---|---|---|
| `city_garden` | 35.99 м; 90.00 / >90.00 с; [FAIL](entry-on-trace.jsonl#L396) | 35.99 м; 90.00 / >90.00 с; [FAIL](entry-off-trace.jsonl#L336) |
| `city_wall` | 101.62 м; 190.53 / >190.53 с; [FAIL](entry-on-trace.jsonl#L440) | 101.62 м; 190.53 / >190.53 с; [FAIL](entry-off-trace.jsonl#L371) |
| `harbour_shore` | 11.43 м; 90.00 / >90.00 с; [FAIL](entry-on-trace.jsonl#L475) | 11.43 м; 90.00 / >90.00 с; [FAIL](entry-off-trace.jsonl#L395) |
| `farm_stable` | 76.28 м; 143.03 / >143.03 с; [FAIL](entry-on-trace.jsonl#L514) | 76.28 м; 143.03 / >143.03 с; [FAIL](entry-off-trace.jsonl#L425) |
| `below_cliff` | — | 77.90 м; 146.07 / >146.07 с; [FAIL](entry-off-trace.jsonl#L466) |

## Кандидаты и перевыборы

Полный радиус — разовая пассивная диагностика после подъёма и при исчерпании. Production остаётся ограничен 8 ближайшими точками. Числа — прошедшие `testWalk` / все настоящие waypoint в 30 м; частоты — число кандидатов с каждой первой причиной отказа. Это не число контактов или проваленных шагов.

| Случай | ON прошло / всего | ON причины отказов | OFF прошло / всего | OFF причины отказов | ON перевыборы |
|---|---:|---|---:|---|---:|
| `city_garden` | 0/29 | slope=23, blocked=6 | 0/29 | slope=23, blocked=6 | [3](entry-on-trace.jsonl#L14) |
| `city_wall` | 0/35 | slope=20, blocked=15 | 0/35 | slope=20, blocked=15 | [3](entry-on-trace.jsonl#L52) |
| `harbour_shore` | 0/8 | blocked=8 | 0/8 | blocked=8 | [3](entry-on-trace.jsonl#L96) |
| `harbour_wall` | 0/13 | step_up=4, slope=1, blocked=8 | 0/13 | step_up=4, slope=1, blocked=8 | 0 |
| `tower_room` | 2/53 | step_up=13, slope=25, blocked=13 | 2/53 | step_up=13, slope=25, blocked=13 | [1](entry-on-trace.jsonl#L156) |
| `farm_field` | 3/21 | step_up=2, slope=1, blocked=15 | 3/21 | step_up=2, slope=1, blocked=15 | 0 |
| `farm_stable` | 0/9 | slope=9 | 0/9 | slope=9 | [3](entry-on-trace.jsonl#L198) |
| `forest_tree` | 12/15 | slope=2, blocked=1 | 12/15 | slope=2, blocked=1 | 0 |
| `forest_pine` | 5/10 | blocked=5 | 5/10 | blocked=5 | 0 |
| `fenced_field` | 1/7 | step_up=1, slope=2, blocked=3 | 1/7 | step_up=1, slope=2, blocked=3 | 0 |
| `below_cliff` | 1/7 | slope=6 | 1/7 | slope=6 | 0 |
| `farm_edge` | 8/13 | blocked=5 | 8/13 | blocked=5 | 0 |

Для оставшихся FAIL ON — отдельное подтверждение основных и повторных случаев. Радиус при исчерпании измеряется из текущей позиции и может содержать другое число точек. Ссылка «исчерпание» ведёт к полному списку причин и индексов шагов.

| Случай / попытка | После подъёма | При исчерпании | Частоты при исчерпании | События перевыбора |
|---|---:|---:|---|---|
| `city_garden` / 1 | 0/29 | 0/29 [исчерпание](entry-on-trace.jsonl#L26) | slope=2, blocked=27 | [1 @ 17.40 с](entry-on-trace.jsonl#L14), [2 @ 25.40 с](entry-on-trace.jsonl#L19), [3 @ 33.40 с](entry-on-trace.jsonl#L22), [3 exhausted @ 41.40 с](entry-on-trace.jsonl#L25) |
| `city_garden` / 2 | 0/29 | 0/29 [исчерпание](entry-on-trace.jsonl#L390) | slope=2, blocked=27 | [1 @ 27.70 с](entry-on-trace.jsonl#L378), [2 @ 35.70 с](entry-on-trace.jsonl#L381), [3 @ 43.70 с](entry-on-trace.jsonl#L384), [3 exhausted @ 51.70 с](entry-on-trace.jsonl#L389) |
| `city_wall` / 1 | 0/35 | 0/40 [исчерпание](entry-on-trace.jsonl#L61) | slope=24, blocked=16 | [1 @ 11.60 с](entry-on-trace.jsonl#L52), [2 @ 19.60 с](entry-on-trace.jsonl#L54), [3 @ 27.60 с](entry-on-trace.jsonl#L57), [3 exhausted @ 35.60 с](entry-on-trace.jsonl#L60) |
| `city_wall` / 2 | 0/35 | 0/39 [исчерпание](entry-on-trace.jsonl#L421) | slope=24, blocked=15 | [1 @ 11.60 с](entry-on-trace.jsonl#L412), [2 @ 19.60 с](entry-on-trace.jsonl#L414), [3 @ 27.60 с](entry-on-trace.jsonl#L417), [3 exhausted @ 35.60 с](entry-on-trace.jsonl#L420) |
| `harbour_shore` / 1 | 0/8 | 0/9 [исчерпание](entry-on-trace.jsonl#L111) | blocked=9 | [1 @ 27.50 с](entry-on-trace.jsonl#L96), [2 @ 35.50 с](entry-on-trace.jsonl#L100), [3 @ 43.50 с](entry-on-trace.jsonl#L103), [3 exhausted @ 71.10 с](entry-on-trace.jsonl#L110) |
| `harbour_shore` / 2 | 0/9 | 0/9 [исчерпание](entry-on-trace.jsonl#L467) | blocked=9 | [1 @ 23.50 с](entry-on-trace.jsonl#L456), [2 @ 36.37 с](entry-on-trace.jsonl#L460), [3 @ 44.37 с](entry-on-trace.jsonl#L463), [3 exhausted @ 52.37 с](entry-on-trace.jsonl#L466) |
| `farm_stable` / 1 | 0/9 | 0/12 [исчерпание](entry-on-trace.jsonl#L207) | slope=12 | [1 @ 12.50 с](entry-on-trace.jsonl#L198), [2 @ 20.50 с](entry-on-trace.jsonl#L201), [3 @ 28.50 с](entry-on-trace.jsonl#L203), [3 exhausted @ 36.50 с](entry-on-trace.jsonl#L206) |
| `farm_stable` / 2 | 0/9 | 0/12 [исчерпание](entry-on-trace.jsonl#L500) | slope=12 | [1 @ 12.60 с](entry-on-trace.jsonl#L491), [2 @ 20.60 с](entry-on-trace.jsonl#L494), [3 @ 28.60 с](entry-on-trace.jsonl#L496), [3 exhausted @ 36.60 с](entry-on-trace.jsonl#L499) |

## Длина ходьбы и диагностика

В «путь / прямая» исключены возвраты защиты и MOB-скачки; прямая — от подъёма до TA. В диагностике каждого режима: кадры корня ниже опоры / нарушения физической капсулы / обе стопы ниже опоры / MOB-скачки. Корень и стопы на PASS не влияют.

| Случай | ON путь / прямая, м | OFF путь / прямая, м | ON диагностика | OFF диагностика |
|---|---:|---:|---:|---:|
| `city_garden` | 35.91 / 25.97 | 35.91 / 25.97 | 0 / 0 / 0 / 0 | 0 / 0 / 0 / 0 |
| `city_wall` | 3.60 / 69.33 | 3.60 / 69.33 | 0 / 0 / 0 / 0 | 0 / 0 / 0 / 0 |
| `harbour_shore` | 34.56 / 11.49 | 34.56 / 11.49 | 0 / 0 / 0 / 0 | 0 / 0 / 0 / 0 |
| `harbour_wall` | 36.84 / 32.97 | 36.84 / 32.97 | 20 / 0 / 15 / 1 | 20 / 0 / 15 / 1 |
| `tower_room` | 90.31 / 12.37 | 89.32 / 12.37 | 0 / 0 / 0 / 0 | 2 / 0 / 3 / 0 |
| `farm_field` | 49.45 / 25.09 | 45.69 / 25.09 | 0 / 0 / 0 / 0 | 0 / 0 / 0 / 0 |
| `farm_stable` | 4.74 / 57.40 | 4.74 / 57.40 | 0 / 0 / 0 / 0 | 0 / 0 / 0 / 0 |
| `forest_tree` | 39.81 / 29.73 | 34.50 / 29.73 | 0 / 0 / 0 / 0 | 0 / 0 / 0 / 0 |
| `forest_pine` | 84.46 / 24.57 | 83.65 / 24.57 | 0 / 0 / 0 / 0 | 0 / 0 / 0 / 0 |
| `fenced_field` | 106.42 / 66.90 | 106.42 / 66.90 | 0 / 0 / 0 / 0 | 0 / 0 / 0 / 0 |
| `below_cliff` | 303.79 / 58.77 | 37.81 / 58.77 | 1 / 0 / 0 / 0 | 0 / 0 / 129 / 0 |
| `farm_edge` | 109.99 / 59.34 | 106.45 / 59.34 | 0 / 0 / 0 / 0 | 0 / 0 / 0 / 0 |

После подъёма в обоих режимах **0 нарушений физической капсулы/нижней границы мира** и **0 незащищённых не-MOB скачков >1 м**. В каждом режиме по **1 возврату защиты**, **0 clamp**, **1 MOB-выравниванию** у работника гавани. Минимальная дистанция до героя: **85,08 м ON / 76,09 м OFF**.

У пилы [корень ниже опоры на 60,35 см](entry-on-trace.jsonl#L133), но низ физической капсулы **выше неё на 17,24 см** — это PASS по согласованному критерию. В OFF на ступени башни [корень ниже на 26,32 см](entry-off-trace.jsonl#L129), капсула **выше на 51,27 см**; этот случай также PASS.

Интервалы без смещения на 1 м за 10 с — только метрика. Время относительно подъёма; отсчёт сбрасывается при смещении ≥1 м (не при уменьшении расстояния до TA).

| Режим | Случай / попытка | Интервалы, с |
|---|---|---|
| ON | `city_garden` / 1 | 9.37–20.20; 25.30–46.37; 56.30–66.37; 78.97–90.00 |
| ON | `city_garden` / 2 | 19.67–44.73; 49.67–90.00 |
| ON | `city_wall` / 1 | 3.57–190.57 |
| ON | `city_wall` / 2 | 3.57–190.57 |
| ON | `harbour_shore` / 1 | 19.47–31.20; 31.20–47.20; 63.03–79.10 |
| ON | `harbour_shore` / 2 | 28.23–62.83; 76.77–90.00 |
| ON | `farm_stable` / 1 | 4.47–143.07 |
| ON | `farm_stable` / 2 | 4.57–143.03 |
| OFF | `city_garden` / 1 | 9.37–20.20; 25.30–46.37; 56.30–66.37; 78.97–90.00 |
| OFF | `city_garden` / 2 | 19.17–75.27; 75.27–86.27 |
| OFF | `city_wall` / 1 | 3.57–190.57 |
| OFF | `city_wall` / 2 | 3.57–190.57 |
| OFF | `harbour_shore` / 1 | 19.47–31.20; 31.20–47.20; 63.03–79.10 |
| OFF | `harbour_shore` / 2 | 15.30–90.00 |
| OFF | `farm_stable` / 1 | 4.47–143.07 |
| OFF | `farm_stable` / 2 | 4.50–143.03 |
| OFF | `below_cliff` / 1 | 23.67–35.23; 43.37–57.63; 57.63–74.67; 100.93–113.87; 119.90–145.67 |
| OFF | `below_cliff` / 2 | 32.30–44.77; 44.77–55.63; 55.63–69.90; 85.70–96.00; 98.70–121.53; 127.17–137.93 |

## Где остановились FAIL

Ниже указаны наблюдаемые причины, без вывода об отсутствии любого непрямого пути. Наличие графового маршрута при этом не доказывает доступность первого сегмента вне сети.

| Режим | Случай / попытка | Где остановился, оставалось до TA | Подтверждение |
|---|---|---|---|
| ON | `city_garden` / 1 | (182.17, 10.05, -23.12) м, 21.03 м; go_to=`NW_CITY_CITYHALL_08`, `ZS_RAKE_FP`, `S_WALK T_WALKTURNR`; очередь 0, ожидания ИИ/анимации/вывода 0/0/0 мс. | 0/29 после подъёма: slope=23, blocked=6; [трасса](entry-on-trace.jsonl#L8); [конечный кадр](entry-on-trace.jsonl#L36) |
| ON | `city_garden` / 2 | (182.03, 9.88, -24.02) м, 21.46 м; go_to=`NW_CITY_CITYHALL_08`, `ZS_RAKE_FP`, `T_WALK_2_WALKL`; очередь 0, ожидания ИИ/анимации/вывода 0/0/0 мс. | 0/29 после подъёма: slope=23, blocked=6; [трасса](entry-on-trace.jsonl#L371); [конечный кадр](entry-on-trace.jsonl#L395) |
| ON | `city_wall` / 1 | (174.24, 10.36, -44.62) м, 68.83 м; go_to=`NW_CITY_CITYHALL_09`, `ZS_SIT_BENCH`, `S_WALKL`; очередь 0, ожидания ИИ/анимации/вывода 0/0/0 мс. | 0/35 после подъёма: slope=20, blocked=15; [трасса](entry-on-trace.jsonl#L46); [конечный кадр](entry-on-trace.jsonl#L79) |
| ON | `city_wall` / 2 | (174.35, 10.34, -44.68) м, 68.93 м; go_to=`NW_CITY_CITYHALL_09`, `ZS_SIT_BENCH`, `S_WALKL`; очередь 0, ожидания ИИ/анимации/вывода 0/0/0 мс. | 0/35 после подъёма: slope=20, blocked=15; [трасса](entry-on-trace.jsonl#L406); [конечный кадр](entry-on-trace.jsonl#L439) |
| ON | `harbour_shore` / 1 | (-103.44, -6.97, -62.31) м, 5.11 м; go_to=`NW_CITY_WAY_TO_SHIP_FISCHER_06`, `ZS_STAND_GUARDING`, `S_WALKL`; очередь 0, ожидания ИИ/анимации/вывода 0/0/0 мс. | 0/8 после подъёма: blocked=8; [трасса](entry-on-trace.jsonl#L89); [конечный кадр](entry-on-trace.jsonl#L115) |
| ON | `harbour_shore` / 2 | (-103.42, -6.95, -62.38) м, 5.15 м; go_to=`NW_CITY_WAY_TO_SHIP_FISCHER_06`, `ZS_STAND_GUARDING`, `S_WALKL`; очередь 0, ожидания ИИ/анимации/вывода 0/0/0 мс. | 0/9 после подъёма: blocked=9; [трасса](entry-on-trace.jsonl#L450); [конечный кадр](entry-on-trace.jsonl#L474) |
| ON | `farm_stable` / 1 | (146.10, 25.32, -188.93) м, 55.48 м; go_to=`NW_FARM1_FIELD_09`, `ZS_COOK_STOVE`, `S_WALKL`; очередь 0, ожидания ИИ/анимации/вывода 0/0/0 мс. | 0/9 после подъёма: slope=9; [трасса](entry-on-trace.jsonl#L192); [конечный кадр](entry-on-trace.jsonl#L220) |
| ON | `farm_stable` / 2 | (146.20, 25.32, -188.87) м, 55.41 м; go_to=`NW_FARM1_FIELD_09`, `ZS_COOK_STOVE`, `S_WALKL`; очередь 0, ожидания ИИ/анимации/вывода 0/0/0 мс. | 0/9 после подъёма: slope=9; [трасса](entry-on-trace.jsonl#L485); [конечный кадр](entry-on-trace.jsonl#L513) |
| OFF | `city_garden` / 1 | (182.17, 10.05, -23.12) м, 21.03 м; go_to=`NW_CITY_CITYHALL_08`, `ZS_RAKE_FP`, `S_WALK T_WALKTURNR`; очередь 0, ожидания ИИ/анимации/вывода 0/0/0 мс. | 0/29 после подъёма: slope=23, blocked=6; [трасса](entry-off-trace.jsonl#L8); [конечный кадр](entry-off-trace.jsonl#L27) |
| OFF | `city_garden` / 2 | (181.94, 9.83, -24.16) м, 21.47 м; go_to=`NW_CITY_CITYHALL_08`, `ZS_RAKE_FP`, `T_WALK_2_WALKL`; очередь 0, ожидания ИИ/анимации/вывода 0/0/0 мс. | 0/29 после подъёма: slope=23, blocked=6; [трасса](entry-off-trace.jsonl#L320); [конечный кадр](entry-off-trace.jsonl#L335) |
| OFF | `city_wall` / 1 | (174.24, 10.36, -44.62) м, 68.83 м; go_to=`NW_CITY_CITYHALL_09`, `ZS_SIT_BENCH`, `S_WALKL`; очередь 0, ожидания ИИ/анимации/вывода 0/0/0 мс. | 0/35 после подъёма: slope=20, blocked=15; [трасса](entry-off-trace.jsonl#L37); [конечный кадр](entry-off-trace.jsonl#L61) |
| OFF | `city_wall` / 2 | (174.35, 10.34, -44.68) м, 68.93 м; go_to=`NW_CITY_CITYHALL_09`, `ZS_SIT_BENCH`, `S_WALKL`; очередь 0, ожидания ИИ/анимации/вывода 0/0/0 мс. | 0/35 после подъёма: slope=20, blocked=15; [трасса](entry-off-trace.jsonl#L346); [конечный кадр](entry-off-trace.jsonl#L370) |
| OFF | `harbour_shore` / 1 | (-103.44, -6.97, -62.31) м, 5.11 м; go_to=`NW_CITY_WAY_TO_SHIP_FISCHER_06`, `ZS_STAND_GUARDING`, `S_WALKL`; очередь 0, ожидания ИИ/анимации/вывода 0/0/0 мс. | 0/8 после подъёма: blocked=8; [трасса](entry-off-trace.jsonl#L71); [конечный кадр](entry-off-trace.jsonl#L88) |
| OFF | `harbour_shore` / 2 | (-103.72, -7.03, -62.34) м, 5.33 м; go_to=`NW_CITY_WAY_TO_SHIP_FISCHER_06`, `ZS_STAND_GUARDING`, `S_WALK T_WALKTURNR`; очередь 0, ожидания ИИ/анимации/вывода 0/0/0 мс. | 0/9 после подъёма: blocked=9; [трасса](entry-off-trace.jsonl#L381); [конечный кадр](entry-off-trace.jsonl#L394) |
| OFF | `farm_stable` / 1 | (146.10, 25.32, -188.93) м, 55.48 м; go_to=`NW_FARM1_FIELD_09`, `ZS_COOK_STOVE`, `S_WALKL`; очередь 0, ожидания ИИ/анимации/вывода 0/0/0 мс. | 0/9 после подъёма: slope=9; [трасса](entry-off-trace.jsonl#L163); [конечный кадр](entry-off-trace.jsonl#L182) |
| OFF | `farm_stable` / 2 | (146.14, 25.32, -188.90) м, 55.44 м; go_to=`NW_FARM1_FIELD_09`, `ZS_COOK_STOVE`, `S_WALKL`; очередь 0, ожидания ИИ/анимации/вывода 0/0/0 мс. | 0/9 после подъёма: slope=9; [трасса](entry-off-trace.jsonl#L405); [конечный кадр](entry-off-trace.jsonl#L424) |
| OFF | `below_cliff` / 1 | (301.32, 25.17, 120.93) м, 60.42 м; go_to=`NW_FARM2_OUT_02`, `ZS_SMALLTALK`, `S_WALKL`; очередь 0, ожидания ИИ/анимации/вывода 0/0/0 мс. | 1/7 после подъёма: slope=6; [трасса](entry-off-trace.jsonl#L255); [конечный кадр](entry-off-trace.jsonl#L283) |
| OFF | `below_cliff` / 2 | (301.41, 25.17, 120.96) м, 60.42 м; go_to=`NW_FARM2_OUT_02`, `ZS_SMALLTALK`, `S_WALKL`; очередь 0, ожидания ИИ/анимации/вывода 0/0/0 мс. | 1/6 после подъёма: slope=5; [трасса](entry-off-trace.jsonl#L435); [конечный кадр](entry-off-trace.jsonl#L465) |

Хильда в ON прошла **4,74 / 4,58 м** (основной / повтор), затем почти весь остаток **143 с** стоит у `(146,10; 25,32; −188,93)` м. Графовый путь существует: **9 узлов, 76,28 м** до `NW_FARM1_INHOUSE_02`. Отказ прямого подхода к `NW_FARM1_FIELD_09` после подъёма — [`slope`, шаг 43](entry-on-trace.jsonl#L192); после исчерпания все 12 кандидатов отвергаются уже на [шаге 0](entry-on-trace.jsonl#L207). Она в `ZS_COOK_STOVE` / `S_WALKL`, очередь и все ожидания нулевые. Это ограничение прямого подхода на данном участке опоры, а не доказанный разрыв графа или очередь ИИ. При 0 подходящих кандидатов ограниченному перевыбору не из чего выбрать новый безопасный вход.

## Причина SIGSEGV и нативные проверки

Держателем устаревшего rider оказался **[список `DynamicWorld::NpcBodyList::frozen`](../../../OpenGothic/common/physics/dynamicworld.cpp#L348)**, не регистрация опоры в Jolt. При удалении записи `del(..., preserveOrder=true)` сдвигал массив, но не исправлял `NpcBody::frozen` следующих NPC. `onMove` затем обращался по старому индексу; тело могло одновременно остаться в frozen и попасть в активный список. После его удаления оставалась висячая запись, которую цикл `DynamicWorld::tick` превращал в `Npc*` для `carryCharacterVirtual`.

Исправление обновляет индексы сразу после удаления. Архив fixture больше не обходит проблему: он восстанавливается прежним путём. Тот же lifecycle вызывается игровой сменой визуала [`GameScript::mdl_setvisual`](../../../OpenGothic/common/game/gamescript.cpp#L1969) → [`Npc::setVisual`](../../../OpenGothic/common/world/objects/npc.cpp#L2878) → [`setPhysic`](../../../OpenGothic/common/world/objects/npc.cpp#L3067) и восстановлением визуала из архива [`Npc::load`](../../../OpenGothic/common/world/objects/npc.cpp#L520) → [`MdlVisual::load`](../../../OpenGothic/common/graphics/mdlvisual.cpp#L137). Полная загрузка WorldObjects сначала создаёт новые NPC; именно падение полной загрузки сохранения здесь отдельно не воспроизводилось.

В существующий `gothic-physics-contract` [добавлена](../../../OpenGothic/tests/physics_contract.cpp#L2424) замена первого из трёх замороженных ghost NPC, движение последнего, создание CharacterVirtual, проверка единственной записи, удаление и следующий tick. В одной сборочной копии через `gq` выполнено нативное сравнение:

```text
Original del:  Frozen NPC replacement: moved body records 2 (expected 1)
              build exit 0; contract exit 1; 30.458 s
Fixed del:     Frozen NPC replacement: moved body records 1 (expected 1)
              Physics contracts passed
              build exit 0; contract exit 0; 33.047 s
```

После проверки сборочная копия восстановлена побайтно к исправленному исходнику. Core/ASan не потребовались: контракт воспроизвёл повреждение списка до обращения к освобождённой памяти. OFF завершил все 5 штатных повторов без SIGSEGV, включая повтор `city_wall`, в начале которого происходили оба прежних падения. [Его результат](entry-off-trace.jsonl#L371) и [полная сводка](entry-off-trace.jsonl#L468) сохранены.

[Контракт `testWalk`](../../../OpenGothic/tests/physics_contract.cpp#L1851) использует реальную нативную капсулу без загруженного мира: ступенчатый бугор из подъёмов по 45 см при пределе 50 см проходит `testWalk`, но не `testMove`; стена отвергается как `blocked`; уклон 60° при пределе 45° отвергается как `slope`. Позиция и число физических тел сохраняются. Новые тестовые файлы не добавлялись.

## Запуски и оставшееся решение

| Запуск | Результат |
|---|---|
| `20261030T192804Z-36179`, подготовочный ON, код `6dac14cfe` | 12 + 6 повторов, complete, exit 1, 698 с. Не включён в итоговое ON/OFF: луч наблюдателя начинался на 100 см выше верха капсулы и захватывал перекрытие. У пилы [ошибочная опора +45,07 см при полу −163,11 см](preparation-trace.jsonl#L33). Луч исправлен в наблюдателе (`bb3e9236c`); после уточнения сброса меток при смене TA оба режима выполнены на общем `0de79501637`. |
| **`20261030T194334Z-86042`, итоговый ON** | **12/12 + 4/4**, **8 PASS / 4 FAIL** основных, complete, **exit 1, 659 с**. |
| **`20261030T195711Z-87359`, итоговый OFF** | **12/12 + 5/5**, **7 PASS / 5 FAIL** основных, complete, **exit 1, 633 с**, без SIGSEGV. |

Оба итоговых режима — один бинарник **`ff9b331a8c04…`** и исходник **`0de79501637`**. Команда ON:

```sh
BENCH_HOST=ryzen RYZEN_CLEAN_IDLE_HOURS=0 RYZEN_LOCK_WAIT=3600 GOTHIC_AI=0 \
OPENGOTHIC_BASE_FINGERPRINT=3436f2bee419cf7655388e6e24cd82878747ca749abb01f6dcb27ede9ac99264 \
OPENGOTHIC_BASE_CACHE=/home/kaifaty/gothic-ai/e2e/claude-routine-return/base-cache \
OPENGOTHIC_BASE_STORE=/mnt/shared/gothic-ai/routine-return/base-store \
ROUTINE_ENTRY_CHECK=1 scripts/bench.sh routine-return "$PWD/.bench/routine-walk-on-final"
```

OFF: `ROUTINE_ENTRY_CHECK=0`, выход `.bench/routine-walk-off`, обычное ожидание блокировки 900 с; фактически 817 с. Ожидание окна: ON 16 с, OFF 149 с. Эти ожидания не входят во время прихода NPC и приведённые 659/633 с самого стенда. Автоочистка выключена, чужие процессы/папки и общие настройки Ryzen не менялись.

Проверка последнего исходника, тот же отпечаток и каталоги базы через `REMOTE_ENV`:

```sh
RYZEN_CLEAN_IDLE_HOURS=0 ~/.codex/checks/gothic/engine-targets.sh . \
  gothic-physics-contract gothic-ragdoll-contract gothic-ragdoll-save-contract
```

**3/3 контракта passed, helper exit 0, сборка Mac exit 0.** `bash -n scripts/bench.sh scripts/ryzen.sh` — exit 0. Полные журналы — `.bench/routine-walk-final-contracts.log`, `.bench/routine-walk-lifecycle-{red,green}.log`; полные игровые события — `.bench/routine-walk-on-final/` и `.bench/routine-walk-off/`. В git выбраны **516 ON + 468 OFF** исходных событий из **84833 / 81427**; исходные кадры наблюдения — **59485 / 57129**. Принятые старые трассы и отчёты доступны в истории, а исходный `README.md` сохранён. Другие сценарии и полная регрессия не запускались.

**Остаётся решение для Хильды.** Её обязательный PASS не получен при валидной подготовке и исчерпанном перевыборе: все прямые подходы в 30 м отвергнуты по уклону. Маршрут по графу есть, но к его первому узлу не найден допустимый прямой подход. Следующее предлагаемое исправление — **локальный непрямой выход через промежуточные опоры, проверяемые тем же testWalk, без navmesh**. Это отдельный механизм; в данном раунде он не добавлен. Для сада/стены/берега предусмотренная альтернатива готовности выполнена, хотя сами случаи остаются FAIL.

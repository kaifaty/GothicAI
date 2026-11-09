# Боевой контракт слоя управления телом NPC

Дата: 13 октября 2026 года. Контракт этапа 1 [плана GRU](npc-body-control-plan.md). Этот документ фиксирует механику reference OpenGothic и словарь первой melee policy. Headless runner реализован на этапе 2: [runtime](npc-body-headless-runner.md), [проверка baseline и ресурсы](../reports/npc-body/2026-10-13-headless/headless-report.md). Этап 3 добавил [числовые наблюдения](npc-body-observations.md) с информационной availability mask; [серверная проверка](../reports/npc-body/2026-10-13-observations/body-observe.md). Реализация внешних команд, authoritative masks и ownership описана в [боевом API этапа 4](npc-body-api.md); описания native ниже относятся к reference пути с выключенным внешним управлением.

Первая задача — дуэль двух людей с одинаковым одноручным оружием, фиксированными характеристиками и штатным `FightAlgo`. Модель выбирает боевой примитив; движение, анимационные окна, столкновения и урон остаются в настоящем движке. Численные результаты и конфигурация reference baseline сохраняются отдельно от контракта. Ни задержки модели, ни окна конкретной анимации нельзя вывести из этой таблицы без прогона с выбранными assets.

## 1. Как сейчас возникает боевое действие

Путь native AI:

```text
Daedalus AI_Attack
  → FightAlgo::fetchInstructions
  → выбор FightAiMove из таблицы тактики через RNG
  → nextFromQueue: разворачивание инструкции в очередь MV_*
  → Npc::implAttack: gates, поворот, проверки, вызов исполнителя
  → Pose: начало/продолжение/отказ в анимации
  → consumeAction: сдвиг очереди, в том числе без начала действия
  → последующие тики анимации и OPTIMAL_FRAME
  → commitDamage: новая проверка дальности и угла
  → takeDamage: блок/отскок либо расчёт урона
```

Источники: [`AI_Attack` в `Npc::nextAiAction`](../OpenGothic/common/world/objects/npc_aiqueue.cpp), [`FightAlgo`](../OpenGothic/common/game/fightalgo.cpp), [`Npc::implAttack`](../OpenGothic/common/world/objects/npc.cpp) и [`commitDamage`](../OpenGothic/common/world/objects/npc_damage.cpp), [`Pose`](../OpenGothic/common/graphics/mesh/pose.cpp).

Здесь две очереди: script `aiQueue` и внутренняя боевая `FightAlgo::tr`. Перед исполнением `implAttack` уступает непустой `aiQueue`. Повторный внешний запрос через [`NpcController::action`](../OpenGothic/ai_bridge/npccontroller.cpp) способен отменить длинное действие и очистить script queue. Поэтому этот endpoint не подходит для потока решений 20 Гц. Будущий provider должен заменять источник следующего боевого решения в движке, сохраняя один общий исполнитель.

`nextFromQueue` не является чистым getter: когда очередь пуста, он материализует новую инструкцию, иногда с RNG. Его нельзя повторно вызывать ради logging, mask или наблюдения. `consumeAction` сдвигает очередь из шести элементов. `MV_MAX=6` в enum означает её вместимость, а не число вариантов действия; значение совпадает с `MV_ATTACKR`.

## 2. Полный словарь native и границы MVP

Публикуемые policy IDs будут отдельным версионированным словарём, а не ordinal значениями C++ enum. Полный существующий enum приведён, чтобы recorder не потерял редкие или неподдержанные значения. Источник: [`fightalgo.h`](../OpenGothic/common/game/fightalgo.h).

| Значение | Native primitive | Реальное исполнение сейчас | Решение для MVP |
|---|---|---|---|
| 0 | `MV_NULL` | Отсутствие инструкции; ветка остановки ходьбы есть, но обычно отсекается `hasInstructions()` | Записывать; не учить как сознательное `wait` |
| 1 | `MV_MOVEG` | Объявлен; нет producer/ветки исполнения в `implAttack` | Исключить |
| 2 | `MV_MOVE` | Подход к текущей цели, в том числе через waypoint path при преграде | `approach` |
| 3 | `MV_JUMPBACK` | `Anim::MoveBack`, короткая анимация отскока | `jump_back` |
| 4 | `MV_ATTACK` | Фронтальная атака, возможна из бега | `attack_front` |
| 5 | `MV_ATTACKL` | Боковая атака слева; для кулаков сводится к фронтальной | `attack_left`, только выбранный sword mode |
| 6 | `MV_ATTACKR` | Боковая атака справа; для кулаков сводится к фронтальной | `attack_right`, только выбранный sword mode |
| 7 | `MV_STRAFEL` | `Anim::MoveL` | `strafe_left` |
| 8 | `MV_STRAFER` | `Anim::MoveR` | `strafe_right` |
| 9 | `MV_BLOCK` | Fist block или sword `AttackBlock` | `block`, MVP только sword mode |
| 10 | `MV_WAIT` | Ожидание 200 игровых мс либо отскок при тесной дистанции | Native intent сохранить; policy `wait_200` после общего resolver |
| 11 | `MV_WAITLONG` | Ожидание 300 игровых мс | `wait_300` |
| 12 | `MV_TURN2HIT` | Поворот к цели до завершения поворота | `turn_to_target` |
| 13 | `MV_TURN` | Исполняется общей веткой подхода; при тесной дистанции становится отскоком | Native intent сохранить; не считать чистым поворотом |
| 14 | `MV_TURNG` | Объявлен; нет producer/ветки исполнения | Исключить |
| 15 | `MV_STRAFE_E` | Завершение бокового шага через `Anim::Idle` | Внутренняя часть strafe; не отдельный выбор policy |

`continue` означает «не назначать новый примитив, исполнять текущий». Это служебный вариант policy, а не `MV_NULL`. MVP vocabulary: `continue`, `approach`, `turn_to_target`, `strafe_left`, `strafe_right`, `jump_back`, `attack_front`, `attack_left`, `attack_right`, `block`, `wait_200`, `wait_300`. Этап 4 публикует этот словарь как IDs 0–11 через общий resolver и body API.

Полный native intent vocabulary берётся из [`zenkit::FightAiMove`](../OpenGothic/lib/ZenKit/include/zenkit/addon/daedalus.hh). Producer [`FightAlgo::nextFromQueue`](../OpenGothic/common/game/fightalgo.cpp) разворачивает его так:

| Script intent | Native queue |
|---|---|
| `NOP` | Нет инструкции |
| `RUN` | `MV_MOVE` |
| `RUN_BACK` | `MV_NULL`; `TODO` в исходнике, исполнителя бега назад нет |
| `JUMP_BACK` | `MV_JUMPBACK` |
| `TURN` | `MV_TURN` |
| `STRAFE` | RNG выбирает `MV_STRAFEL` или `MV_STRAFER`, затем `MV_STRAFE_E` |
| `ATTACK` | `MV_ATTACK` |
| `ATTACK_SIDE` | `MV_ATTACKL`, `MV_ATTACKR` |
| `ATTACK_FRONT` | RNG выбирает `MV_ATTACKL` или `MV_ATTACKR`, затем `MV_ATTACK` |
| `ATTACK_TRIPLE` | RNG: front/right/left либо left/right/front |
| `ATTACK_WHIRL` | left/right/left/right |
| `ATTACK_MASTER` | left/right/front/front/front/front |
| `TURN_TO_HIT` | `MV_TURN2HIT` |
| `PARRY` | `MV_BLOCK` |
| `STAND_UP` | Не добавляет примитив; это не реализованное действие вставания |
| `WAIT`, `WAIT_EXT` | `MV_WAIT` |
| `WAIT_LONGER` | `MV_WAITLONG` |

`RUN_BACK` и `STAND_UP` исключены из обучаемого action space. Отскок `MoveBack` не заменяет полноценное бегство `AI_Flee`: у него другой путь исполнения и waypoint logic.

## 3. Намерение, эффективный примитив и жизненный цикл

Для траектории нужны отдельные стадии. `intent` — выбранная script инструкция или команда provider; `queued_primitive` — `MV_*` после materialization; `effective_primitive` — операция, которую исполнитель действительно пытается выполнить в этом тике. Выбранный удар может давать `turn_to_target` несколько тиков подряд до первой попытки удара. Выбранное ожидание может стать `jump_back`.

| Primitive | Условия/преобразования native | Когда очередь сдвигается | Что считать завершением в будущем API |
|---|---|---|---|
| Attack | При угле больше 5° сначала поворот без consume. Side attack вне W-range пропускается, кроме случая `bodyState()==BS_RUN`. Melee ray между collision centers с преградой пропускает удар. Нужен drawn fist/1H/2H mode | `doAttack=true`; также swim/dive, wall, неверный weapon mode и side вне range способны consume без удара | Завершение выбранного attack segment/его lock, отмена damage reaction либо смена owner. Hit — отдельное событие |
| Block | Нужен focus ±30°. `blockSword` вызывает `doAttack(AttackBlock, BS_PARADE)`. Нет активного оружия или невозможна анимация → остаётся в очереди | Успех block-функции; вне focus/неподходящий mode → consume без блока | Конец segment/защитного окна фиксировать отдельно; принятие block не означает отражённый удар |
| Strafe | Попытка `MoveL/MoveR`; поворот анимации сбрасывается в 0. Отсутствие анимации обходит soft-lock | При успехе задаётся FAI wait на полное время анимации; при отсутствующей anim тоже consume | Завершение движения и внутреннего `MV_STRAFE_E` либо отмена |
| `MV_STRAFE_E` | `Idle`; `setAnim=false` возвращает false из `implAttack` | После успеха Idle | Внутренняя финализация strafe |
| Jump back | Также результат `MV_WAIT/MV_TURN` при closeup+focus. В воде и при `BS_PARADE` пропускается; вне focus пропускается и ускоряет script loop | После успешного `MoveBack`, либо перечисленных skip | Конец lock/segment либо отмена; защита активна только по `Pose::isJumpBack` и углу |
| Move/turn | Вне G-range при преграде может передать управление pathfinding. В W-range останавливает ходьбу и поворачивается; иначе делает шаг к цели | Когда W-range достигнут, изменилось попадание в G-range или body state, и focus ±5°; script loop вызывается в том же тике | Достижение условия resolver, потеря цели либо отмена. Время заранее неизвестно |
| Turn-to-hit | Поворачивается к цели | Когда `implTurnTo=false` | Достигнута ориентация либо отмена |
| Wait | `MV_WAIT` без promotion: 200 мс; long: 300 мс; остановка ходьбы | Сразу после назначения wait | Когда снят wait gate; consume происходит раньше |

Источник всей таблицы: [`Npc::implAttack`](../OpenGothic/common/world/objects/npc.cpp). Точная длительность сегментов определяется MDS/MAN assets, overlays, combo и speed, а не константой «длительность удара».

В текущем native пути отсутствуют RPC стадии `accepted/started/completed`. При инструментировании их нельзя выдумывать по `consumeAction`. Будущий внешний API должен различать:

- `accepted`: арбитр принял команду конкретного поколения; действие ещё может не начаться.
- `started`: новая анимация/движение действительно начались; `continued` отдельно отмечает продолжение существующего сегмента/combo.
- `rejected`: до принятия не выполнены owner/generation/TTL/mask/target условия.
- `cancelled`: ранее принятую/начатую команду остановил reset, смена владельца, цель, deadline или hit reaction.
- `completed`: конкретный execution segment завершён по engine условию. Consume очереди, истечение FAI wait, конец защитного окна и конец animation — разные события.

`Pose::startAnim=true` может означать уже текущую sequence с тем же combination: она не перезапускается. `continueCombo` также может продолжить прежнюю sequence, изменить combo/start time или запустить новую. Поэтому точка истинного `started` требует сравнения слоя `(sequence, combination, start_time, combo)` либо явного engine execution ID. Смена `animNames` сама по себе недостаточна. Источники: [`Pose::startAnim` и `continueCombo`](../OpenGothic/common/graphics/mesh/pose.cpp).

## 4. Окна боя и interruptibility

В assets времена окон переводятся из кадров в мс. `Sequence::totalTime = numFrames × 1000 / fpsRate`. На слое effective time равен `start + (t_game − start) × speed`; сравнения окон учитывают speed. `Pose::animationTotalTime` и `atkTotalTime` возвращают полную длительность, а не remaining time. Источники: [`animation.cpp`](../OpenGothic/common/graphics/mesh/animation.cpp), [`pose.cpp`](../OpenGothic/common/graphics/mesh/pose.cpp).

| Предикат/окно | Точное значение в коде | Ограничение интерпретации |
|---|---|---|
| `isAttackAnim` | В sequence есть хотя бы один `OPTIMAL_FRAME` | Не означает, что урон активен именно сейчас |
| `isPrehit` | Хотя бы один `OPTIMAL_FRAME` находится впереди текущего animation frame | В многоударной sequence это не только первый замах; после первого удара может остаться true |
| Combo `defWindow` | Для combo index: `begin < elapsed <= end` | Не идентичен общему defense window |
| `isDefWindow` | Любая пара `begin <= elapsed < end` | Сам по себе не проверяет `BS_PARADE` |
| `isDefence` | Слой `BS_PARADE` и `isDefWindow` | Реальный melee block ещё требует focus ±30° и допустимого атакующего |
| `isJumpBack` | `BS_PARADE` с пустым `defWindow` либо с активным `defWindow` | Защита от incoming melee ещё требует facing ±90° |
| `isDefParWindow` | Ровно две отметки `defParFrame`, `begin <= elapsed < end` | Не подменять этим предикатом `isDefence`, используемый для урона |
| `isFinished` | `elapsed > defHitEnd[combo]` либо `elapsed > totalTime` | Pose учитывает ещё transition/loop/next sequence |

Обычный `Pose::startAnim` не заменяет слой, если текущая sequence не finished и `canInterrupt=false`. Transition с соответствующими `defWindow/defHitEnd` допускает interrupt после начала окна; без этих данных — только completion или force. Для других sequence interrupt запрещён внутри combo window. Специальный `continueCombo` разрешает hit→block через `Force`, а hit→hit — по combo window. Это animation capability, а не полное правило выбора native action.

У native NPC раньше срабатывают gates в [`Npc::tick` и `implAttack`](../OpenGothic/common/world/objects/npc.cpp):

- `waitTime >= t`, `aniWaitTime >= t`, `outWaitTime > t` удерживают tick в `WaitMove`.
- Нет цели, player actor, разговор, down цель или непустая script queue не дают обычной попытки боевого примитива.
- Melee `BS_HIT` делает поворот/движение и возвращает управление до `nextFromQueue`.
- `BS_LIE` пытается Idle; stumble/fall/air продолжают movement; FAI wait удерживает решение до `faiWaitTime < t`.
- Успешная melee attack назначает `faiWaitTime = t + atkTotalTime + 1`; атака из бега дополнительно назначает `aniWaitTime`. Block не назначает такой wait в своей ветке.
- Полученный damage вызывает `FightAlgo::onTakeHit`, очищает боевую очередь и выставляет FAI wait на текущий `t`; возможный stumble/interrupt зависит от body flags и script override. У отражённого блока эта ветка damage calculation не вызывается.

Следовательно, `BS_FLAG_INTERRUPTABLE`, `canInterrupt(sequence)` и `can_decide` нельзя сливать в один boolean. Native AI не выбирает мгновенный блок во время обычного `BS_HIT` только потому, что Pose умеет force transition. Для refactor сначала сохраняем gates, порядок queue и RNG. Изменение боевой реакции после baseline требует отдельного сравнения.

## 5. Фазы тика и время наблюдений

В текущей оконной игре порядок такой:

1. `MainWindow::tick`: pause/load проверка, ограничение wall dt; bench заменяет его фиксированным шагом.
2. `Gothic::tick`: `onTick(dt)` — bridge commands и roster/views работают **до** game tick.
3. `GameSession::tick`: scaleTime/timeMul, увеличивается `ticks`, script tick, world tick.
4. `WorldObjects::tick`: actor ticks последовательно по `handle().id`. Внутри `Npc::tick` сначала animation events/layers, затем waits и `implAttack`.
5. Player focus/input/move после game tick; затем `updateAnimation`, camera.
6. `TestBench::tick`: scenario commands и captures после animation update.

Источники: [`MainWindow::tick/render`](../OpenGothic/common/mainwindow.cpp), [`Gothic::tick`](../OpenGothic/common/gothic.cpp), [`GameSession::tick`](../OpenGothic/common/game/gamesession.cpp), [`WorldObjects::tick`](../OpenGothic/common/world/worldobjects.cpp), [`AiBridge::onTick`](../OpenGothic/ai_bridge/aibridge.cpp).

**Reference trace фиксирует фазу каждой записи.** Snapshot `npc_pre_tick_animation_tags` на входе actor tick уже имеет новый `t_game`, но events/layer transitions данного actor за этот тик ещё не обработаны. Часть других actors уже обновлена, часть ещё нет. Это не однородный post-world snapshot. Native выбор/попытка внутри `implAttack` — отдельная decision boundary; generic snapshot нельзя приклеить к нему только по одинаковому timestamp.

Для этапа 3 проектируем общий снимок `post_simulation_post_animation` после завершения world/player/animation updates. Для шага k запись action связывается с `observation_seq`, состоянием до решения и последующим transition; события внутри шага сохраняют свой phase/substep. В headless режиме нужно сохранить ту же механику обработки animation events, movement и pose update; просто убрать вызов render недостаточно.

Существующий bench использует 30 Гц: шаги 33/33/34 мс. `OPENGOTHIC_BENCH_REALTIME` дополнительно привязывает к wall clock; без него fixed simulated dt не гарантирует заданного коэффициента ускорения. При 30 Гц обнаруженные края boolean окон квантованы шагом; «первый/последний true» не дают точную длительность assets. Окно, полностью лежащее между снимками, можно пропустить. Для точных интервалов нужны metadata окон или event crossings по каждому sim step. Источник: [`TestBench::frameDt`](../OpenGothic/common/testbench.cpp).

## 6. Числовое наблюдение: поля, единицы, доступность

Это целевая схема этапа 3. «Есть в движке» не означает «уже отправляется внешней policy». Runtime/queue/animation details в baseline — privileged telemetry для анализа. Состояние противника без видимости или точные будущие hit-frame времена по умолчанию не входят в actor input; доступность оговаривается отдельно для фиксированной арены.

| Поле | Единица/смысл | Источник и статус |
|---|---|---|
| `t_game_ms`, `dt_game_ms`, `phase`, `sim_step` | Игровые мс; dt после применённого scaling; phase как выше | `GameSession::ticks`, `World::tickCount`; phase/step — trace либо будущий runner |
| `entity_id`, `target_id`, `script_symbol` | Runtime instance отдельно от типа | Для baseline допустим trace-only lifetime ID; symbol существующего bridge не уникален для копий NPC |
| `position_world_cm[3]`, `relative_target_cm[3]` | x/z горизонталь, y высота; relative = target − self | `Npc::position`; координаты мира в см |
| `yaw_rad`, `target_yaw_error_rad` | Радианы; yaw 0 направлен по +x, π/2 по +z; error wrap в [−π, π] | `rotationRad`; future relative error использует collision-center direction как `FightAlgo::angleTest` |
| `fight_distance_cm` | Длина трёхмерного `fightDistanceTo` между transformed skeleton roots | `Npc::fightDistanceTo`; не подменять обычным `qDistTo`/горизонтальной дистанцией |
| `attack_range_cm`, `g_range_cm`, `closeup_range_cm` | Порог attack, порог G, `0.75 × baseDistance` | `FightAlgo::prefferedAttackDistance/prefferedGDistance/baseDistance`; зависят от guild, weapon length, script values |
| `in_w_range`, `in_g_range`, `in_closeup`, `focus_30`, `focus_5` | Engine bool с точным тем же расчётом | `FightAlgo`; privileged для baseline, input доступность решить явно |
| `hp`, `hp_max`, `hp_fraction` | Script integer points; fraction = hp/hp_max при hp_max>0 | `Npc::attribute`; opponent exact HP privileged, видимая оценка отдельна |
| `weapon_state`, `weapon_symbol`, `fight_tactic`, `skill`, `protection` | Enum / script points / instance | `WeaponState`, inventory, handle; фиксировать в episode manifest, не скрытые константы recorder |
| `body_state_raw`, `body_state_base`, `body_flags` | Raw uint32; base = raw & BS_MAX; flags отдельно | `Npc::bodyState`; `bodyStateMasked()` сохраняет flags, это не «чистый» base ID |
| `is_dead`, `is_unconscious`, `is_in_air`, `is_swim`, `is_down` | Boolean, down объединяет terminal incapacitation | `Npc`; own state input, opponent по договорённой видимости |
| `anim_names`, `attack_anim`, `prehit`, `defence_active`, `jumpback_active` | Текущая sequence/engine predicates | `Npc`/`Pose`; достаточно для baseline edge tracing, недостаточно для execution ID |
| `layers[].name/start_game_ms/speed/elapsed_ms/combo` | Start в game ms; elapsed в effective animation ms | `Pose`; private internals требуют read-only probe. Privileged baseline; полного RPC сейчас нет |
| `fai_wait_remaining_ms`, `ani_wait_remaining_ms`, `ai_wait_remaining_ms`, `out_wait_remaining_ms` | max(deadline − t, 0), плюс gate boolean для границ `>=`/`>` | Private Npc deadlines; срез remaining=0 ещё может быть gated в точном t |
| `intent`, `queued_primitive`, `effective_primitive`, `last_result` | Версионированные categories и execution link | Trace/API по разделу 3; не равнозначны друг другу |
| `can_decide`, `action_mask` | Допустимость нового решения и каждого policy action | **Проектируется**, общего pure resolver сейчас нет |
| `target_seen`, `line_of_sight`, `local_obstacles` | Engine perception и локальная геометрия | `canSeeNpc`, ray queries; задачи этапов 3/11. Не выполнять дорогой новый ray для каждого debug field |
| `linear_velocity_cm_s` | Разность одинаковых phase snapshots / dt ×1000 | Производное поле; сейчас `animMoveSpeed` возвращает animation displacement за dt, не готовую мировую скорость |

Источники единиц и enum: [`npcview.cpp`](../OpenGothic/ai_bridge/npcview.cpp) явно использует cm; [`Npc::rotationRad/fightDistanceTo/bodyState`](../OpenGothic/common/world/objects/npc.cpp); [`constants.h`](../OpenGothic/common/game/constants.h); [`FightAlgo` range/angle methods](../OpenGothic/common/game/fightalgo.cpp).

Существующий [`npc_roster`](../OpenGothic/ai_bridge/npcobserver.cpp) выходит раз в 500 мс, [`npc_views`](../OpenGothic/ai_bridge/npcview.cpp) — раз в 1000 мс. Они агрегируют body/weapon и не являются боевым наблюдением. В частности view body обычно сводит бой к `stand`; он не заменяет `BS_HIT/BS_PARADE` и animation windows.

## 7. Action masks и RPC: предлагаемая спецификация

Общий resolver/mask не реализован на этапе 1. Его задача — одинаково проверить native/scripted/neural решения без side effects: без RNG, `setAnim`, queue mutation и изменения `loopNextTime`. При старте нужно повторить проверку; маска не гарантирует попадание или урон.

Для melee MVP `can_decide=false` при отсутствии owner lease, pause/load, terminal self/target, stale observation, script-body ownership, неподдержанном weapon mode и активных gates. `continue` поддерживает безопасное исполнение текущего segment. Per-action checks используют orientation/range, доступность anim, jump/swim/parade и фактическую interruptibility. Решение, что делать с атаками, требующими сначала turn/approach, принимается в одном общем resolver: запретить сейчас либо вернуть explicit effective primitive. Только neural путь не должен получить скрытое другое поведение.

Проектируемые request/result IDs: `session_epoch`, `world_epoch`, `control_generation`, `entity_id`, `goal_version`, `observation_seq`, `decision_id`, `command_id`. Reconnect/load/reset/owner change инвалидируют прежнее управление, дубликат command не повторяет start, дубликат observation не обновляет GRU hidden state второй раз. Trace `seq`/actor lifetime ID этапа 1 **не** являются этими runtime guarantees.

`ticks` сохраняется в save game, начинается заново в новой session и масштабируется `timeMul`. Это не monotonic network clock. В дальнейшем TTL считается по локальному возрасту engine snapshot и игровому времени; watchdog — через локальный `steady_clock`. Между Mac и сервером не сравниваем абсолютные monotonic timestamps. В lockstep simulated time стоит, пока actor отвечает; wall timeout считается infrastructure failure. Искусственная задержка реакции задаётся явными simulation steps и записывается в manifest. Источник: [`GameSession::load/save/tick`](../OpenGothic/common/game/gamesession.cpp).

## 8. Reference trace, измерения и готовность этапа 1

Reference native run делается на `dev-ubuntu` без LLM и моделей; Mac не выполняет серию симуляций. Оконный Linux baseline под виртуальным display, если применяется до этапа 2, остаётся оконным движком с renderer. Его успех не доказывает наличие CPU-only headless runner или отсутствие renderer VRAM.

Минимальный trace разделяет native choice, effective attempt, consume, animation start/continue, phase edges, incoming hit attempt, defensive result и damage. Каждая запись содержит phase, game time, step, actor instance и target instance. Наблюдение и action result связываются явно; соединение только по timestamp неоднозначно из-за нескольких actors/попыток в одном тике. Сам logging не меняет queue, RNG, perception, deadlines или native исполнение.

Reference manifest содержит engine Git revision и dirty patch hash, сборку/компилятор, world/assets hashes, сценарий и его hash, fixed dt/timeMul, RNG seed либо честный признак отсутствия seed control, конфигурацию участников, CPU/RAM/render backend и расположение сырого trace. Анализ отдельно сообщает:

- исход дуэли, HP/полученный и нанесённый damage, длительность, таймаут/потерю цели;
- интервалы **новых** native решений и повторных execution attempts;
- наблюдаемые prehit/defence/jumpback окна с указанием разрешения 30 Гц;
- accepted function return, реальный segment start/continue и consume без start как разные счётчики;
- simulation wall speed и ресурсную нагрузку, без заявлений о neural latency;
- что осталось не покрыто: другие оружие/skills/overlays, несколько активных NPC, точные asset windows, RPC и accelerated headless parity.

Принятые начальные budgets до обучения:

| Метрика | Критерий | Что подтверждает этап 1 |
|---|---|---|
| Период наблюдений | 20 Гц, 50 игровых мс; дополнительная запись на переходах | Измеряет native окна; достаточность для реакции ещё требует API/realtime проверки |
| Warm model step | p95 ≤ 5 wall мс на сервере | Модель отсутствует, бюджет ещё не измерен |
| Local collector: готовый snapshot → отправка команды | p95 ≤ 20 wall мс, включая IPC | Collector/API отсутствуют |
| Engine snapshot → arbitration result | p95 ≤ 100 wall мс в realtime, включая транспорт и тики | Начало/конец animation lock измеряются отдельно; это не neural latency |
| Работа наблюдений для четырёх NPC | p95 ≤ 2 мс simulation thread, падение headless substeps/s ≤ 5% | Четыре наблюдаемых NPC и headless будут измерены на этапах 2–3 |
| Влияние на ASR/TTS/Strands | Деградация выбранной p95 метрики ≤ 10% при совместной фиксированной нагрузке | Native baseline не посылает запросов моделям; статусы юнитов не заменяют latency test |

Период 50 игровых мс не равен wall задержке ответа policy. Полный бюджет 100 мс согласован с этапом 8 плана; новый кадр или animation lock может отдельно отложить фактический старт. Budgets меняются только по измерениям с объяснением, до выбора checkpoint.

**Этап 1 завершён:** [reference отчёт](../reports/npc-body/2026-10-13-melee-reference/body-baseline.md), [полные численные данные](../reports/npc-body/2026-10-13-melee-reference/body-baseline.json), [провенанс и ограничения](../reports/npc-body/2026-10-13-melee-reference/run-notes.md). Seeds 17/29/43 завершились смертью одного из двух NPC, без timeout; terminal HP 0/300, 300/0, 300/0. Проверены lifetime IDs, setup, native выбор/consume, melee damage и шесть guard полей при p95/max snapshot interval 66 мс.

Из trace: полные observed `prehit` интервалы 66–1134 мс, `defence` 466–467 мс, `def_window` 33–767 мс; `def_par_window` ни разу не активен. Это объединённые диапазоны для конкретных assets/skills, не универсальные attack durations. Metadata блока `T_1HPARADE_0*`: 25 fps, 600 мс total, window `[0,480]`; `S_1HATTACK`: 4560 мс total и несколько hit/combo windows. Минимальный observed guard 33 мс подтверждает, что периодические 20 Гц сами по себе не покрывают каждое короткое окно: нужны события/edges и повторная проверка executor. `isJumpBack` здесь также true при блоке; `MV_JUMPBACK` в этой серии не consume, так что отдельный отскок ещё не проверен.

Animation hooks включают draw/idle/hit reaction и несут текущий/последний queue choice ID; их counts/delays не равны числу решений policy или command start latency. Все повторные hooks записаны. Точная remaining recovery/interruptibility и будущие masks пока заданы по коду/metadata, а не доказаны принудительными попытками каждого примитива.

Этапы 2–4 добавят headless parity, частые наблюдения, общий resolver и внешний боевой API. Этот trace используется для проверки сохранения native поведения, а не как готовый BC датасет. Модель, четыре управляемых NPC, network latency и accelerated simulation throughput ещё не измерены.

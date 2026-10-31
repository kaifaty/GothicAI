# Headless арена и runner эпизодов

Этап 5 [плана GRU](npc-body-control-plan.md). Арена использует настоящий CPU движок и [боевой API этапа 4](npc-body-api.md). Симуляции выполняются на dev-ubuntu; Mac запускает SSH orchestration и лёгкие тесты parser/adapter.

**Готов, 13 октября 2026 года:** полная серия `stage5-v2` проверила 100 dirty restorations, микроэпизоды и mirrored native/scripted сравнения. [Результаты, throughput и границы](../reports/npc-body/2026-10-13-arena/validation-notes.md), машинный отчёт (3,9 МБ, в репозитории не хранится: dev-ubuntu `~/gothic-ai/body-arena/reports/stage5-v2-final/body-arena.json`).

## Как работает сейчас

`native_duel` поддерживает `arena.weapon:"1h"` (также по умолчанию) и `"2h"`. Одноручный профиль и ответы без поля сохранены; двуручный использует `ITMW_2H_BAU_AXE`, `TALENT_2H=60`, master-анимации и `W2H` (`hnpc->weapon=4`), остальные характеристики прежние. Snapshot пишет действительное оружие; для `2h` дополнительно `twohand`/`twohand_talent`. Неизвестное оружие или `2h` вне `native_duel` отклоняются при reset. Проверка — `body_setup_probe.weapon_contract` на реальном runner.

[`body_sim_bench.py`](../scripts/body_sim_bench.py) принимает `--weapon 1h|2h`. `--checkpoint` разрешён только с `1h`; `--policy-only` запускает одну P30-серию без повторения native. Вариант `vanilla` выключает `melee_blade_shadow`, `melee_physics_hits`, `melee_posture`, сохраняя окружение, fork-prefix/reset, trace и watch варианта `posture-on`. Это контроль этих трёх флагов на текущем движке. При включённых журналах строки клинка/равновесия сохраняются по эпизодам до ротации общего файла. Журнал клинка содержит `arena_seed`, слои анимации — `is_attack`, событие урона — состояние нокдауна до удара и анимацию атакующего; механика не меняется.

[`melee_balance_report.py`](../scripts/melee_balance_report.py) читает папки с `bench.json`, пишет `balance.json` и `balance.md`, выводит таблицы в stdout. Серия — вариант × native/P30 × оружие; недоступные метрики имеют `null` и причину. Полная матрица запускается одной командой `scripts/melee-balance-remote.sh`: синхронизация этого checkout, Linux-сборка игры/runner, отдельная заявка `gq quiet` на серию и отчёт. По умолчанию 64 дуэли, seed 100000, 60000 мс; `--episodes`, `--seed`, `--timeout-ms`, `--series`, `--run-name` меняют прогон. Данные — только `~/gothic-ai/melee-balance/` и `/mnt/shared/gothic-ai/melee-balance/`, база через их собственные кэш/хранилище. [Короткая проверка и команды полной матрицы](../reports/melee-physics/2026-10-29-balance/README.md).

## Reset и начало эпизода

`reset` загружает неизменный контрольный save и исполняет [body-arena.bench](../scripts/bench/body-arena.bench) до marker арены. Native `BodyArena` создаёт двух смертных бойцов: primary `BDT_1003_BANDIT_M` и opponent `PAL_255_RITTER`. У обоих 300 HP, strength/dexterity 60, one-hand chance 60%, master one-hand animations, sword `ItMw_1h_Common_01`, leather armour и edge/blunt protection 30. Статы устанавливаются после equipment callbacks.

Setup задаёт seed до создания NPC и повторно перед боем, отдельный animation RNG, позиции на настоящем ground, направления, взаимные цели, уже извлечённое оружие и пустые очереди/waits. `side=1` меняет физические позиции ролей местами; профили primary/opponent сохраняются. Snapshot фиксирует оборудование, статы, poses/control, queues, цели и epochs. До `arena_start` симуляция участников не продвигается.

Для scripted микроэпизодов provider приобретает оба lease; для candidate duel — только primary, opponent остаётся native. После acquire нужен свежий числовой снимок. `arena_start` с native источником запускает штатный `ZS_ATTACK`. После старта fixtures и перенастройка наблюдений запрещены; движения, атаки, блоки и урон проходят через штатный исполнитель.

| Команда | Поведение |
|---|---|
| `reset` с `arena` | Новый world из save, setup, canonical initial state и свежие наблюдения |
| `arena_start` | Начать эпизод с `provider=native`, `scripted` или `native_with_scripted_opponent` |
| `arena_step` | Продвинуть 1–2000 substeps в lockstep, остановиться на terminal/infrastructure или подготовленной границе реакции |
| `arena_status` | Snapshot эпизода без продвижения времени |
| `arena_reaction_activate` | Включить защитника на подготовленной границе `block` или `reset_stress` с `reaction_hold:true` |
| Обычный body API | Lease, heartbeat, primitive, lifecycle events и numeric observations |

`arena` содержит `scenario`, `seed`, `side`, `distance_cm`, `timeout_ms` и необязательный `weapon`. Native bounds: seed uint32, side 0/1, distance 80–2000 см, timeout 33–60000 игровых мс. Runner поддерживает `mode=lockstep` и `realtime` при fixed 30 Hz simulation и time multiplier 1. В realtime после reset мир также ждёт успешного `arena_start`; wall задержка до старта не создаёт накопленных тиков. `arena_step` разрешён только в lockstep. Наблюдения идут на 20 Hz; terminal получает дополнительный настоящий post-simulation sample, если последний periodic sample старее terminal game time.

`native_with_scripted_opponent` предназначен для микроэпизодов: primary должен быть без lease, opponent — с lease. Primary запускает обычный `ZS_ATTACK`, а действия opponent проходят через боевой API. Для `native_duel` сохраняются native оба участника либо scripted primary с native opponent.

## Отдельная проверка реакции на начавшуюся атаку

Опциональный `reset.arena.reaction_hold:true` разрешён только для `scenario=block` или `reset_stress`, без load fixture, при `reset.mode=lockstep`. Отсутствующий параметр сохраняет обычный запуск. Проверка реакции отдельно от обычного `block`: во всех вариантах Native/GRU/MLP защитник впервые получает управление после настоящего windup противника.

Для сбора траекторий используется `reset_stress`: успешное отражение удара не останавливает мир, поэтому collector может дождаться фактического `completed` блока (`action_id=9`). Collector ограничивает такой макроэпизод timeout до 2000 игровых мс и заканчивает сбор после настоящего завершения команды. Этот источник обучения не получает combat-quality credit. В `block` сохраняется прежний terminal по настоящему blocked hit; анимация блока не продлевается искусственно после terminal.

Driver приобретает обе обычные lease и вызывает `arena_start` с `provider=scripted`. Фаза `held` не допускает `body_action`, `body_goal`, `body_release` или `body_disconnect` для primary; heartbeat, чтение и явные reset/close доступны. Для opponent разрешена настоящая `AttackFront` (`action_id=6`) через общий исполнитель. Подготовка продвигается через `arena_step`; её предел — 1000 игровых мс либо timeout эпизода, если он меньше.

Runner переходит в `armed` на первом подходящем **естественном periodic** снимке после обновления мира и анимаций. Обе lease должны сохраняться, primary — быть без команды, в доступном состоянии и видеть opponent в native weapon range. У opponent должны совпасть настоящий `started` для `action_id=6`, command/execution IDs и текущие `attack_anim` / `prehit`. У обоих остаётся 300 HP; до границы не должно быть hit attempt, damage или block. Принудительный event-only снимок не становится входом модели. Если подготовка повреждена или не успела, результат — infrastructure.

В `armed` симуляция останавливается: следующий `arena_step` возвращает `advanced_substeps=0`. Активация требует:

| Поле `arena_reaction_activate` | Значение |
|---|---|
| `session_epoch`, `world_epoch` | Текущие uint64 эпохи наблюдений |
| `primary_entity_id`, `opponent_entity_id` | Реальные ID соответствующих участников |
| `primary_control_generation`, `opponent_control_generation` | Текущие uint64 поколения обеих lease |
| `observation_seq` | Sequence подготовленного periodic снимка primary |
| `issued_game_ms` | Его game time, равный текущей подготовленной границе |
| `provider` | `native` или `scripted`; по умолчанию `scripted` |
| `mode` | `lockstep` или `realtime`; по умолчанию `lockstep` |

Команда повторно проверяет actual ownership, доступность primary, видимую цель, windup, HP и свежесть periodic источника. Для `native` общий арбитр явно освобождает idle primary с `release_reason=reaction_activation`, после чего ставится обычный `ZS_ATTACK`. Для `scripted` lease primary сохраняется. Opponent продолжает уже начатую атаку. Переход в realtime обновляет mode и heartbeat сохранённых lease и назначает scheduler deadline от текущего wall clock; накопленных тиков подготовки нет. Игровые часы не перематываются, новые маски, FOV или синтетические attack flags не вводятся.

Успешный ответ — обычная status-оболочка runner с актуальным `arena.reaction`, а не `body_control_result`. Неверная или повторная активация возвращает обычный RPC error и не меняет scheduler. `arena.reaction` содержит `enabled`, `phase` (`held`, `armed`, `active`), `held_game_ms`, `preparation_max_game_ms`, `game_ms`, `sim_step`, `observation_seq`, `periodic`, `opponent_command_id`, `opponent_execution_id`, `opponent_started_game_ms`, `opponent_started_sim_step`, `opponent_started_event_seq`, `activation_game_ms`, `activation_sim_step`, `activation_provider`. `game_ms` / `sim_step` / `observation_seq` здесь фиксируют подготовленный источник; поля activation фиксируют переход на той же физической границе.

Native журнал сохраняет `reaction_held`, `reaction_armed`, `reaction_active` и настоящие control events. Явная native restoration при активации относится к подготовке этого сценария; её можно отделять от fallback только по совпавшему proof активации. Исход и окно защиты устанавливаются по реальным `hit_attempt`, `defence_result`, HP и игровым timestamps. Подготовленный windup сам по себе не означает успешный block.

## Сценарии и исходы

| Сценарий | Native условие |
|---|---|
| `distance` | Войти в заданную дистанцию и удержать её 200 игровых мс |
| `attack` | Нанести положительный melee damage; HP target действительно уменьшился |
| `block` | Native `defence_result blocked:true` при настоящем ударе, HP primary не уменьшились |
| `native_duel` | Один участник down/dead либо истёк лимит времени |
| `reset_stress` | Живыми действиями изменить HP и позицию, оставить owned active wait перед reset |

`terminated` означает native task success или death/down. `truncated` означает time limit либо `control_lost`: живой scripted участник дуэли потерял цель из вида, и арбитр начал возврат native. Такой эпизод заканчивается по настоящему ownership event после simulation step и не считается победой candidate; продолжение native не используется для улучшения его результата. Watchdog/disconnect provider, неожиданные причины takeover, прерванный running эпизод и load/setup/transport failure относятся к infrastructure. Конечные актёры и game time сохраняются до cleanup/reset. После terminal новые acquire/action/goal запрещены.

[EpisodeEnv](../scripts/body_arena_env.py) предоставляет reset/step/action и очищает observation cache, processed sequences, leases, command IDs и recurrent token перед новым reset, включая неудачный reset. Token включает process/world/runner generation; GRU hidden state появится на этапе 7. Provider получает только свежие видимые numeric features и masks. Полные arena snapshots используются для проверки стенда.

## Проверка и throughput

[body-arena-v1.json](../evals/npc-body/body-arena-v1.json) фиксирует критерии до полной оценки. Suite выполняет 100 dirty reset циклов в одном процессе и дополнительный canonical reset после сотой изменённой дуэли. Перед reset NPC наносит настоящий удар, делает jump back и начинает wait. Отскок сохраняет цель в поле зрения; strafe на короткой дистанции может штатно завершиться возвратом к native из-за устойчивой потери видимости.

Повторяются seed/setup и сравниваются семантические initial состояния; runtime IDs и epochs меняются. Проверка не обещает побитовую идентичность всей игры. Есть микроэпизоды, paired native/scripted дуэли для seeds 17/29/43 и обеих сторон, отдельная throughput серия. Legacy native-off trace сверяется с архивом этапа 3.

Независимый reporter читает IPC с wall timestamps, native arena/control/mechanics journals и observations. Он проверяет обе стороны damage/block counters, реальные HP/positions, completion/pending execution, final sample, mirrored setups и смену generations. Driver pass flag не устанавливает готовность.

Policy transition — уникальная принятая non-continue команда со свежим допустимым наблюдением, фактическим native start и связанным command/episode outcome. Locked observation ticks, heartbeats и status queries не считаются решениями. Отдельно измеряются substeps/s, provider-loop transitions/s, end-to-end transitions/s с reset, episodes/s и reset p50/p95. Прогнозы 32768/100000/1000000 решений относятся к сбору среды; model inference, optimizer и запись датасета не измеряются.

Начальный feasibility budget — 32768 решений за 6 часов, то есть минимум 1,517 transitions/s end-to-end. Готовность среды и достаточность скорости для малого PPO отражаются отдельно. При недостаточной скорости нужна оптимизация серверного runner перед большим сбором; перенос симуляций на Mac не применяется.

## Серверный запуск

```sh
scripts/body-arena-remote.sh preflight
scripts/body-arena-remote.sh sync
scripts/body-arena-remote.sh build
scripts/body-arena-remote.sh run stage5-validation
scripts/body-arena-remote.sh collect stage5-validation
```

Runs/provenance и Python слой находятся в `~/gothic-ai/body-arena/`. Для этого worktree переиспользуется private incremental engine cache `~/gothic-ai/body-api/source` и `build`; предыдущие runs сохраняют отдельные source archives и бинарники. Новый run получает собственные неизменные binary/source copies.

Transient user unit использует CPUQuota 200%, MemoryMax 12G, TasksMax 128, Nice 10 и RuntimeMax 2400 с. Shared simulation lock допускает один серверный engine процесс. ASR/TTS и продовые контейнеры не изменяются. На Mac возвращаются компактные manifests/reports; большие журналы, assets, saves и бинарники остаются на dev-ubuntu. Smoke проверяет запуск и основные сценарии, но не заменяет полный gate.

## План и журнал

- 2026-10-29: добавлены двуручный профиль дуэли, полностью выключенный контроль трёх флагов, отчёт метрик и запускатель матрицы. Короткая проверка описана в [отчёте](../reports/melee-physics/2026-10-29-balance/README.md); полная матрица и выводы о балансе выполняются отдельно.

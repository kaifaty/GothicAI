# Native headless runner, этап 2

Статус: **этап 2 готов**, 13 октября 2026 года. Полная серверная серия `stage2-v3` прошла; [отчёт с критериями, ресурсами и provenance](../reports/npc-body/2026-10-13-headless/headless-report.md), [компактный JSON](../reports/npc-body/2026-10-13-headless/headless-report.json). Этот документ описывает runtime и протокол. Контекст — [план](npc-body-control-plan.md), [боевой контракт](npc-body-combat-contract.md) и [сохранённый reference этапа 1](../reports/npc-body/2026-10-13-melee-reference/run-notes.md).

## Исполнение настоящего движка

Linux target `gothic-body-runner` запускает `Gothic`, `GameSession`, Daedalus VM, настоящий мир и native NPC AI. Entry point — [bodyrunner.cpp](../OpenGothic/game/bodyrunner.cpp). Он загружает контрольный save через `Serialize` и синхронно создаёт `GameSession`. Физика, waypoint graph, очереди AI, perceptions, урон, CPU pose и animation tags остаются в движке.

Headless entry point создаёт `Resources` без устройства. Он не создаёт `Tempest::Device`, `MainWindow`, `Window`, `Application`, swapchain, renderer или `SoundDevice`. Случайный доступ к `Resources::device()` без устройства выбрасывает ошибку; такая ошибка считается сбоем реализации, а не разрешением включить renderer.

CPU adapters сохраняют скелеты, последовательности анимаций, pose, root motion, mesh geometry для collision/hit zones и weapon attachments. Текстуры, GPU buffers и presentation исключены. `World` создаёт collision world/BSP/WayMatrix без `WorldView`; CPU mesh handles сохраняют geometry и transforms без GPU allocation.

Звуковое устройство отсутствует, но игровые пути звука не обрываются: сохраняются диапазон listener, выбор native звукового варианта, длительность и lifecycle silent effect, а также script sound perceptions. Выбор варианта использует тот же `SoundFx::selectSample()` и native RNG, что обычная игра. Headless и rendered benchmark duration/lifecycle идут по игровому времени; rendered сохраняет реальное воспроизведение, обычная игра вне benchmark сохраняет прежний OpenAL lifecycle; совпадение боевой механики проверяется trace, а не наличием звука.

`GameScript::setSeed()` задаёт script/fight RNG. Кроме него native код использует C `rand()` для некоторых звуковых вариантов и анимаций защиты. Reset пересоздаёт `PlayerControl` и очищает input/focus; оси движения имеют нулевую инициализацию. Физический handle NPC освобождает принадлежащие ему body/capsule после unregister, поэтому повторная загрузка не сохраняет эти allocations. Reset восстанавливает C RNG в стартовое состояние `srand(1)`; scenario отдельно задаёт seed каждого `bodyduel`. Исторический reference нельзя объявлять совпавшим только по seed или одинаковому outcome.

## Воспроизводимость вариантов анимации

Исходный `std::rand` общий для звуков и вариантов блока. Завершение OpenAL по wall clock сдвигает поток RNG относительно ускоренной симуляции. A2/A3 имеют одинаковые боевые окна, но разный root motion; поэтому seed `GameScript` сам по себе не фиксирует траекторию и попадания.

Проверка явно разделяет два режима, включаемых только переменной окружения в benchmark:

- `OPENGOTHIC_BODY_ANIMATION_RNG=isolated`: два независимых xorshift32 потока, заданных seed/стороной дуэли. Full rendered/headless сравнение использует эти же потоки. Native solve по-прежнему расходует штатный `std::rand` один раз; изменяется только выбор варианта 1H/2H блока.
- `OPENGOTHIC_BODY_ANIMATION_RNG=replay` и `OPENGOTHIC_BODY_ANIMATION_FIXTURE`: выбираются реальные варианты блока из сохранённого trace по точным seed/роли/игровому времени. Fixture содержит только имя clip и вариант; она не задаёт AI intent, очередь, позицию, HP, урон или outcome. Каждая запись должна быть использована ровно один раз после реального accepted/pose_changed блока; ошибки и остатки отвергают прогон.

При отсутствии переменных сохраняется прежний выбор `std::rand`. Историческое сравнение обычного/isolated запуска сохраняет диагностическое расхождение. Отдельный строгий branch replay проверяет механику при записанных случайных ветках, а full same-source сравнение проверяет удаление renderer на новых воспроизводимых ветках. Ни один режим не расширяет числовые допуски и не подменяет native боевой исполнитель.

## Порядок CPU substep

[SimulationStep](../OpenGothic/common/game/simulationstep.h) используется и обычным `MainWindow`, и runner. Сохранён прежний порядок фаз:

В обычной игре `dialogs.tick()` и `inventory.tick()` остаются перед общим world/player шагом; их UI callbacks runner не создаёт.

1. `Gothic::tick(dt)` вызывает внешние `onTick`, затем `GameSession::tick(dt)`: время, VM, `World::tick`, NPC/physics/perceptions.
2. Внутри `Npc::tick` telemetry snapshot снимается **перед** `tickAnimationTags()`. Tags обрабатывают текущую анимацию и могут вызвать `commitDamage`; затем идут native AI и движение/root motion. Это происходит до обновления pose текущего substep.
3. `PlayerControl::tickFocus()`; в обычной игре здесь остаются document/mouse callbacks; затем `PlayerControl::tickMove(dt)`.
4. `Gothic::updateAnimation(dt)` обновляет CPU poses, attachments и необходимые animation passes.
5. Камера: UI callback обычной игры либо `SimulationStep::observerCamera(dt)` в runner.
6. `TestBench::tick(dt, player)` выполняет сценарные команды и проверяет terminal. В обычной игре после этого идут capture/render; в runner их нет.
7. Как окно после кадра, runner вызывает `World::updateLandscape(camera)`: резидентность кусков следует за героем и камерой (уровни NPC в `auto`), видов без `WorldView` нет. С `--game-parity` затем отдаёт миру позицию камеры для радиуса эффектов.

Один вызов `GameSession::tick()` не заменяет этот цикл: без animation update последующие tags и боевые окна перестают соответствовать native игре. Bench-команды исполняются после шага, поэтому созданные ими бойцы начинают обычный tick на следующем substep.

Первое `finishStep(0, player)` после загрузки повторяет rendered load-finalize phase и инициализирует bench без продвижения игрового времени. После старта bench `TestBench::frameDt()` даёт фиксированные 30 Hz substeps, чередуя 33/34 мс. `timeMul` установлен в 1; ускорение lockstep достигается отсутствием ожидания wall clock.

## Бенчи без окна: `--world` и `--game-parity`

`--world <ZEN>` вместо `--save` начинает новую игру в этом мире, как `-w` в игре; после загрузки runner, как `Gothic::finishLoading`, зовёт `Gothic::onWorldLoaded`. `--game-parity` считает мир так же, как окно: создаёт `AiBridge`, как `game/main.cpp` (без сервера мост всё равно считает важные разговоры вокруг героя при `-nointro` сказанными, перехватывает восприятие зверей и запускает диалоги как программы графа; `GOTHIC_AI=0` выключает); после каждого шага отдаёт `World::setHeadlessViewer` позицию камеры, и `World::isInPfxRange` даёт те же 40 м, что рендер (события анимаций с эффектами и foot IK — только рядом с камерой, а не у всех NPC мира); ставит `OPENGOTHIC_BODY_TRACE=0`, если вызывающий не задал своё. С ним час `npc-day-parity` без окна побайтно равен оконному прогону той же сборки и окружения и идёт 66 с вместо 98 ([замер](../reports/compute/2026-11-02-headless-bench/README.md)). Без флага runner ведёт себя как раньше: сбор данных тел и навигации не меняется.

```sh
gothic-body-runner -g "$GOTHIC_DIR" -devmode -nointro --world NEWWORLD.ZEN --game-parity \
  --scenario scripts/bench/npc-day-parity.bench --output /absolute/out --mode lockstep
```

## Выгрузка мира: `--society-export`

`--world <ZEN> --society-export <путь>` загружает новую игру в этом мире и сразу после стартовых скриптов (день 0, 08:00) пишет мир стенда общества. Шагов мира, сценария и `AiBridge` при этом нет. Потом раннер выходит. Формат, запуск и контракт описаны в [`society-sim.md`](society-sim.md#формат-v2).

## Игрок-наблюдатель и AI LOD

Используется тот же save и [melee-сценарий](../scripts/bench/body-melee.bench), что в reference. Он ставит бессмертного игрока-наблюдателя примерно в 12 м от двух смертных native участников. Наблюдатель не атакует; fighter setup, оружие, броня, stats, guilds и `ZS_ATTACK` задаются существующим `bodyduel`.

Наличие игрока сохраняет native AI LOD: `WorldObjects::tick` считает расстояние от player position и назначает `AiNormal` в радиусе 30 м, `AiFar` до 60 м, далее `AiFar2`. Headless не принудительно включает AI всем NPC и не удаляет perceptions. CPU observer camera сохраняет player-follow, camera bone, mode и listener для спокойной арены без UI. Обычные UI/dialog/cutscene camera paths здесь не эмулируются.

## Локальный JSONL env API

`--stdio` включает построчный протокол через stdin/stdout. Каждая команда — JSON object с `cmd`; необязательный `id` возвращается без изменения. Ответы имеют `schema_version: 1`, `ok` и status fields. Engine/library logs отправляются в stderr, чтобы stdout оставался JSONL.

CLI требует `--save` и `--scenario`; `--output` по умолчанию `.body-runner`, `--mode` по умолчанию `lockstep`. `-g <assets>` выбирает игровые данные Gothic II NotR. Без `--stdio` runner сам загружает save, исполняет сценарий до завершения, записывает summary и выходит.

| Команда | Семантика |
| --- | --- |
| `reset` | Уничтожает старый `PlayerControl` и session, сбрасывает scenario-owned state, загружает выбранный при старте save, создаёт новый `PlayerControl` и `generation`. Допускает `scenario`, `output_dir`, `mode` и `seed`. |
| `advance` | Требует `substeps`, целое от 1 до 100000. Останавливается на лимите, завершении сценария либо новом native choice, если `until_decision: true`. |
| `status` | Возвращает counters/state без изменения lockstep simulated time. |
| `close` | Сохраняет summary, удаляет session и завершает процесс. |

`seed` в `reset` — uint32 override для строк `bodyduel` выбранного сценария. При его отсутствии сценарий сохраняет собственные seeds; в melee-v1 это 17, 29 и 43. Save остаётся тем, который указан в CLI; reset не принимает путь к другому save. Для каждого независимого запуска следует использовать новый каталог output: trace files открываются для записи.

Пример последовательных запросов; каждый JSON находится на отдельной строке:

```json
{"id":1,"cmd":"reset","mode":"lockstep","seed":17,"output_dir":"/home/user/gothic-ai/body-headless/runs/example/episode-001"}
{"id":2,"cmd":"advance","substeps":60}
{"id":3,"cmd":"status"}
{"id":4,"cmd":"advance","substeps":120,"until_decision":true}
{"id":5,"cmd":"close"}
```

В `advance` response дополнительно присутствуют `steps_advanced` и `stopped_on`: `budget`, `native_decision` либо `completed`. `until_decision` отслеживает изменение счётчика **native** `native_choice` и возвращает управление после закончившегося CPU substep. Это не observation/action boundary будущего body API: GRU, action masks, learner control и `step(actions, substeps)` ещё не подключены.

Status содержит `generation`, `loaded`, `done`, `game_ms`, `sim_step`, `simulated_ms`, `native_choices`, `mode`, `output_dir`, wall/reset/lag counters и `graphics_device_created`, `window_created`, `audio_device_created`. `game_ms` включает время исходного save; `simulated_ms` относится к текущему reset. `done` означает завершение bench, а не обязательно смерть участника: outcome каждого `bodyduel` записывается в trace отдельно.

Ошибка команды возвращается как `ok: false` с `error` и исходным `id`, например:

```json
{"schema_version":1,"ok":false,"id":7,"error":"substeps must be an integer in [1,100000]"}
```

Фактический error response также содержит доступные status fields. Fatal startup/load error завершает процесс с exit code 2. Закрытие stdin завершает runner с сохранением summary; оно не подменяет недостающий episode terminal. Проверка completeness отклоняет незавершённый trace.

### Lockstep и realtime

В `lockstep` процесс ожидает команду, мир стоит. `advance` исполняет до заданного бюджета без wall-clock sleeps; задержка клиента между запросами не расходует боевые окна. Simulated delay, если он понадобится, следует задавать отдельными substeps.

В `realtime` после reset мир продвигается по wall clock даже без ожидающего `advance`. `advance` задаёт условие будущего ответа и не приостанавливает симуляцию; во время ожидающего advance разрешены `status` и `close`. Второй advance/reset до ответа отклоняется. Если CPU отстаёт, runner записывает `max_realtime_lag_ms` и продолжает фиксированные substeps, не увеличивая `dt` или `timeMul`.

Wall watchdog принадлежит внешнему suite/systemd job. Его срабатывание означает infrastructure failure. Игровой timeout отдельного `bodyduel` остаётся truncation эпизода; эти причины не смешиваются.

IPC читает stdin без блокировки и обрабатывает строки с завершающим newline из собственного буфера. Неполная строка не останавливает realtime; несколько команд в одном пакете получают отдельные ответы. Максимальный размер команды — 1 MiB. EOF с незавершённой строкой возвращает infrastructure error. Отдельный [IPC probe](../scripts/body_headless_ipc_probe.py) проверяет fragmented/pipelined input, freeze, until_decision, generation и clean close.

## Запуск только на dev-ubuntu

Из корня репозитория на Mac запускается только orchestration:

```sh
scripts/body-headless-remote.sh preflight
scripts/body-headless-remote.sh sync
scripts/body-headless-remote.sh build
scripts/body-headless-remote.sh run stage2-melee-001
scripts/body-headless-remote.sh collect stage2-melee-001
```

Команда `all <fresh-run-name>` объединяет эти действия. `GOTHIC_BODY_SSH` может переопределить SSH alias; по умолчанию используется `dev-ubuntu`. Код launcher — [body-headless-remote.sh](../scripts/body-headless-remote.sh), suite — [body_headless_suite.py](../scripts/body_headless_suite.py).

Рабочий каталог — `~/gothic-ai/body-headless/`: `source/`, `build/`, `provenance/`, `runs/<name>/`. Используются сохранённые assets в `~/gothic-ai/body-baseline/assets/`, приватный save и immutable binary reference `melee-reference-stage1-v4`. Assets не копируются в Git и не изменяются. Stage 1 user-local toolchain переиспользуется; сборка `gothic-body-runner` и обычного `Gothic2Notr` идёт с `--parallel 2` и `nice 10`.

При установленном `gq` launcher использует очередь: сборки — `quick`, весь suite и replay — `quiet`, поскольку они измеряют скорость и включают realtime-проверки. Заявки сохраняют `CPUQuota=200%` и `MemoryMax=12G`; полный suite резервирует окно для rendered verification в Xvfb, headless runner и replay окна не требуют. API, observation и arena suites также используют `quiet` из-за realtime-проверок и замеров скорости, сбор trajectories — `batch`. Без `gq` сохраняются прежние transient user units, проверки чужих game/runner процессов и `body-simulation.lock`. Системные пакеты, Docker, пользовательские продовые контейнеры и ASR/TTS units не меняются.

Для runner задаются пустые `DISPLAY`/`WAYLAND_DISPLAY` и заведомо несуществующие `VK_DRIVER_FILES`/`VK_ICD_FILENAMES`. Он должен работать без графического backend. Suite проверяет status attestation и `/proc/<pid>/fd`: отсутствие `/dev/nvidia*`, `/dev/dri/*`, `/dev/snd/*`, X11/Wayland sockets. Эти проверки дополняют архитектурное отсутствие device/window/audio constructors.

Rendered verification исполняется **отдельно**, только через CPU lavapipe и Xvfb на dev-ubuntu. Это контроль того же исходного кода и trace; Xvfb не является способом запуска headless runner. Rendered и headless процессы не работают одновременно. Pytest и симуляционные прогоны также выполняются последовательно.

## Доказательства parity и ресурсы

[headless-parity-v1.json](../evals/npc-body/headless-parity-v1.json) задаёт допуски до оценки: время события до 34 игровых мс, позиция до 2 см, yaw до 0,5°, scalar animation metadata до 10⁻⁶. Setup/roles, порядок native choices/очередей/consume/gates, names/flags анимаций, guards, hit/defence/damage/HP и outcome сравниваются точно. Сходные агрегаты или одинаковая победа не заменяют ordered trace.

Есть два профиля [парсера parity](../scripts/body_headless_parity.py):

- `archived_reference` сравнивает наблюдаемые события и resulting movement с записью этапа 1. В ней нет per-tick root/collision internals; одного этого сравнения недостаточно для полного gate этапа 2.
- `full` сравнивает rendered/headless trace из одних исходников с `physics_step`: CPU root displacement, фактическое движение и native collision flags.

`physics_step` имеет phase `npc_post_movement` и покрывает каждый наблюдаемый NPC tick. Реальный вызов `MoveAlgo::tick` отмечен `movealgo_tick:true`; native ветка без него — `movealgo_tick:false`, `skip_reason:npc_tick_without_movealgo`, `collision_scope:none` и нулевое применённое root/requested движение. Такое измерение не вызывает физику дополнительно. Фактическая разница позиции и yaw записывается в обоих случаях. `root_dx/dy/dz` — read-only CPU pose displacement в world coordinates до движения; `requested_dx/dy/dz` — первый displacement через `MoveAlgo::tryMove`; `moved_dx/dy/dz` — итоговая разница позиции. `grounded` означает native `movement_state == Run`. Collision counts/flags относятся только к `MoveAlgo::tryMove`; прямые ground corrections/mobsi calls не объявляются дополнительными probes. `Npc::tryMove` скрывает внутренний MoveCode за bool, поэтому telemetry не придумывает этот enum.

Suite сохраняет manifests/hashes, stdout IPC, stderr, события, runner summary и `/proc` resource samples. Проверки включают lockstep с дополнительной wall задержкой, realtime, повторный reset, состояние ASR/TTS units и shared GPU usage. Метрики substeps/s и native decisions/s — параметры runtime, а не скорость learner transitions или latency сервиса модели.

100 коротких последовательных reset-прогонов проверяют новый generation, одинаковое начальное **экспонированное** actor/animation state и стабильность warmed RSS, descriptors/threads. Это не доказательство равенства всей памяти, будущих leases, policy hidden state или reward buffers. Missing evidence и первый ordered mismatch остаются ошибками gate; допуски после неудачного прогона не расширяются.

`collect` переносит evidence в ignored `data/npc-body/headless/<name>/`, исключая runtime save, binaries и source archive. Компактный итоговый отчёт публикуется в `reports/npc-body/` отдельно после проверки.

Финальная серия `stage2-v3`: все три full rendered/headless сравнения и обе проверки независимости от расписания совпали. Архивный branch replay совпал с исходным baseline, использовал 17/17 записанных вариантов ровно по одному разу. 100 resets прошли с p95 910,14 мс и warmed RSS +0,43 MiB; FD/thread span 0. Sensory и IPC probes прошли. Lockstep показал 64,61 substeps/s, sim/wall 2,14 и peak RSS 562,60 MiB при CPUQuota 200%; realtime — sim/wall 1,00 и максимальное отставание 46 мс. Эти числа относятся к одному runner и trace-enabled native melee, а не к обучению модели.

Отчёт воспроизводится из collected evidence без симуляции:

```sh
python3 scripts/body_headless_report.py data/npc-body/headless/stage2-v3 \
  --output-dir reports/npc-body/2026-10-13-headless
```

## Текущие границы

Этап 3 добавляет пассивные `body_watch` и `body_observations`: [числовая схема, epochs, очередь и проверка](npc-body-observations.md). Они исполняются в той же CPU симуляции и не предоставляют внешнее управление боевым действием. Для этой серии используется [отдельный launcher](../scripts/body-observe-remote.sh).

Этап 6 добавляет `body_trajectory_watch` (boolean `enabled`, ограниченный `queue_capacity`) и `body_trajectory_events` (`max` 1–256). Эти команды включают и читают отдельную пассивную очередь наблюдений, native решений и исполнения. Запись включается до `arena_start`; выключение позволяет забрать конечные события. Status содержит `body_trajectory` counters. [Recorder, схема датасета и split](npc-body-trajectories.md) описаны отдельно.

Основной parity gate относится к native human melee-v1. CPU morph rendering, все виды PFX/spell interactions, полноценный dialogue UI, другие camera paths, archery и навигационные задачи этим набором не подтверждаются. Некоторые adapters сохраняют необходимые CPU callbacks/geometry, но это не равнозначно parity всех игровых систем.

ASR/TTS latency и latency `body-service` не измеряются этим suite. Проверяются присутствие прежних сервисов и shared host resource evidence; причинное влияние на latency требует отдельного замера. Этап 4 реализовал [боевой API, ownership и masks](npc-body-api.md), этап 5 — [headless арену и runner эпизодов](npc-body-arena.md). Контракт parity этапа 2 сохраняется; GRU, inference, обучающие trajectories, BC/PPO и parallel arena pool остаются следующими этапами плана.

# Числовые наблюдения тела NPC

**Этап 3 готов**, 13 октября 2026 года: полная серия `stage3-v1` на `dev-ubuntu` прошла все критерии. Пассивный поток настоящего движка использует общий `BodyObservations` builder в headless runner и AI bridge. Подписка не меняет native источник действий, очередь, RNG, движение или боевые анимации. [Отчёт](../reports/npc-body/2026-10-13-observations/body-observe.md), [JSON](../reports/npc-body/2026-10-13-observations/body-observe.json), [проверки](../reports/npc-body/2026-10-13-observations/validation-notes.md); критерии зафиксированы в [body-observe-v1.json](../evals/npc-body/body-observe-v1.json).

## Фаза, идентификаторы и время

Снимок `post_simulation_post_animation` снимается в общей `SimulationStep::finishStep` после тика всего мира, player control и animation/camera update, перед изменениями benchmark. Все NPC и цели в одном шаге имеют эту фазу. Native события возникают внутри шага; запись несёт их исходные `source_game_ms` и `source_sim_step`.

- `session_epoch` — положительный opaque ID процесса, создаётся один раз без RNG calls, сохраняется при reset и меняется в новом процессе. Идентичность scoped session и manifest прогона; это поле не является feature или часами для deadline.
- `world_epoch` и `generation` меняются при reset/load и перенастройке подписки без владельцев тела; они инвалидируют runtime ID, историю и отложенные samples. С v45 обычный мост может менять список `body_watch` без смены эпохи, если все NPC под lease или с `acquire_pending` остаются в нём и частота не меняется. Headless runner сохраняет прежнее поведение.
- В пассивном режиме `control_generation=0`, поскольку подписка не захватывает тело. [API этапа 4](npc-body-api.md) публикует generation действующего lease и отдельную authoritative mask.
- `entity_id` — ID экземпляра в generation. Два NPC с одинаковым числовым `script_symbol` имеют разные ID; script symbol не является ключом recurrent state.
- `observation_seq` — одна возрастающая последовательность всех акторов в generation. Между строками одного NPC бывают значения других NPC. Рост или сокращение списка обычного моста под lease или с `acquire_pending` не сбрасывает счётчик, identity и историю оставшихся NPC. Новые NPC получают строки с ближайшего тика, удалённые свободные NPC перестают их получать. Повторный sample не является новым обновлением будущего GRU hidden state.
- `t_game_ms`, `dt_game_ms` и `sim_step` относятся к игре. `dt_game_ms` — разность предыдущего снимка того же NPC; начальный dt равен нулю.

При 20 Гц используются 50 игровых мс. Fixed engine step 33/33/34 мс квантует периодические снимки; события и изменения состояния добавляют строки между ними. `periodic` и `event_only` разделяют эти причины. `state_changed` объясняет sample на изменении body/pose/видимости даже при пустом списке событий. Периодическая частота считается отдельно, без улучшения статистики дополнительными events.

При сокращении списка мост удаляет из своей очереди накопленные строки исключённых NPC и учитывает их в `dropped_total`; после подтверждения подписки они больше не отправляются. Накопленные строки оставшихся NPC сохраняются.

## Схема v1

Пакет `kind=body_observations`, `schema_version=1` содержит общую фазу/epochs, `sample_period_ms`, `queue_capacity`, `dropped_total`, `dropped_since_drain`, `event_dropped_total` и `observations`. Каждая строка имеет `kind=body_observation` и собственные epoch/seq/time/ID.

| Объект | Поля | Единицы и доступность |
|---|---|---|
| `self` | position, linear velocity, yaw, HP/max/fraction | Мир x/z горизонталь, y высота; см, см/с, рад, script HP points. Velocity — конечная разность одинаковой фазы, не animation displacement |
| `self` | weapon state/symbol/length, skill/talent/valid, fight tactic | Native значения; длина в см, skill в script points, talent 0/1H/2H/bow/crossbow |
| `self` | raw/base/flags body state, dead/down/air/swim, attack/prehit/defence/jumpback/windows/interruptible | Native enum/boolean. `body_state_base=raw & 31`; remaining=0 не обязательно означает снятый wait gate |
| `self.gates` | fai/ani/ai/output wait remaining и boolean gates | Игровые мс и точные native boundary predicates |
| `target` | present/entity ID/visible/current_valid | Visibility проверяет настоящий NPC sight и world collision. ID обозначает известную native цель, а не её актуальную позицию |
| `target` | position/relative position, velocity/relative velocity, yaw error, native fight distances/ranges, focus/range booleans, observed pose/HP | Текущие значения доступны при видимости. За преградой **все** эти поля равны `null`; точные видимые HP — допущение фиксированной арены первой версии |
| `target` | last-known position/velocity, last seen time, age | Последний видимый sample и его возраст. Скрытые перемещения/урон не обновляют эти поля; первый derivative после reappearance не считается достоверной текущей скоростью |
| `local_obstacles` | ray range, 16 rays: angle/distance/hit | Горизонтальные rays из collision center, равномерно вокруг yaw. Рад/см; no-hit distance равна range, sentinel Bullet не сериализуется |
| `goal` | kind/version/target ID/move hint | Только native combat/idle intent первой версии; полноценная LLM задача и её version относятся к API этапа 4 |
| `last_result` | valid/code/source time/source step | Последний собственный native результат; execution/command IDs этапа 4 ещё отсутствуют |
| `events` | event/source entity IDs, source time/step, perspective, code | Собственные события и события видимой цели. Невидимая цель не раскрывает текущие события через этот канал |

`velocity_valid=false` означает `linear_velocity_cm_s=null`. Начальный sample, нулевой dt и `motion_discontinuity=true` инвалидируют собственную скорость. Последний признак используется для явно помеченных fixture teleports; скачок положения не превращается в физическую скорость.

В словаре событий: 1/2 — animation accepted/rejected, 3–6 — hit applied/out-of-range/out-of-focus/no-target, 7/8 — block/evade, 9/10 — damage/loss absent, 11/12 — dead/unconscious. Source timestamp берётся в native hook, а не из времени Python polling.

В пассивном режиме `can_decide=false`, `control_allowed=false`, `action_mask_valid=false`. `native_decision_eligible` отражает только read-only native gates. `availability_mask[12]` — консервативная доступность native примитивов с `mask_kind=conservative_native_availability`; continue остаётся доступным. Этот массив не является разрешением внешней команды. [Общий resolver и арбитр этапа 4](npc-body-api.md) добавляют отдельную `action_mask`, повторную проверку при запуске и актуальные control/goal версии для занятого тела.

## Headless API и ограниченные буферы

После `reset` включается watch:

```json
{"cmd":"body_watch","enabled":true,"rate_hz":20,"max_actors":4,"queue_capacity":256,"ray_range_cm":300}
```

Runner выбирает участников текущего bench (`bodyduel` или `bodyobserve`); лимит — четыре NPC. Поддерживаются 10 или 20 Гц. `body_observations` с `max=256` сливает целиком настроенную bounded очередь, не продвигая время lockstep:

```json
{"cmd":"body_observations","max":256}
{"cmd":"body_watch","enabled":false}
```

Строковая очередь ограничена 1–256 entries; overflow удаляет старую строку и увеличивает счётчики. Pending events ограничены 32 на actor; потери имеют отдельный счётчик. Runtime registry ограничен. `status.body_observation` содержит размеры, emitted/dropped/invalid counts, missed period buckets и измеренные затраты. Смена generation очищает историю; между поколениями sequence не соединяется.

`observations.jsonl` — полный engine journal тех же serialized rows для проверки; `observation-batches.jsonl` — фактически полученные пакеты. Журнал помогает проверить истину, но не подменяет доказательство wire delivery при overflow. Long journals остаются на `dev-ubuntu`; в Git сохраняются компактные отчёты, scripts и eval configuration.

Измерительный headless driver читает bulk пакеты раз в 60 substeps (около двух игровых секунд), чтобы оценивать throughput без busy polling. Частота 20 Гц относится к созданию отдельных снимков, их native wall timestamps и данным внутри пакета. Этот режим доставки не является realtime задержкой будущей policy. Коллектор управления может запрашивать `body_observations` между короткими advances или во время pending realtime advance; обычный bridge отправляет готовые строки на следующем `onTick`. Бюджет observation→action будет проверяться с executor и моделью на последующих этапах.

## Проверка на dev-ubuntu

Запуск из корня репозитория запускает только SSH orchestration на Mac:

```sh
scripts/body-observe-remote.sh preflight
scripts/body-observe-remote.sh sync
scripts/body-observe-remote.sh build
scripts/body-observe-remote.sh run stage3-validation
scripts/body-observe-remote.sh collect stage3-validation
```

Launcher переиспользует mutable `~/gothic-ai/body-headless/source` и `build`; архивы прежних runs остаются неизменными. Каждый запуск получает собственные source/binary hashes, copied executable и transient user unit с CPUQuota 200%, MemoryMax 12G и wall watchdog 2400 с. Docker, системные пакеты и прежние сервисы не меняются. Одновременно работает один runner. Final evidence: `~/gothic-ai/body-headless/runs/stage3-v1`; long journals остаются там, `collect` переносит компактные результаты.

Серверная серия включает десять минут realtime для четырёх native бойцов, три пары on/off performance запусков, реальную overflow проверку, native melee parity и четыре privacy cases: visible, occluded, hidden movement/HP change, reappearance. Python driver управляет настоящим runner; симуляции и моделей на Mac нет.

`body_observe_report.py` независимо читает raw evidence. Проверяются finite JSON без дублированных ключей, epochs/seq/drops, единицы, HP fraction, native body flags, производная скорости, относительные координаты, 16 rays, nullable hidden target и отдельный privileged oracle. Неполный stream, недоказанный gate или smoke не получают готовность этапа.

Oracle снимает истинное состояние в той же post-simulation фазе и соединяется с наблюдением по game time, sim step и runtime actor/target ID. Координаты команды fixture предыдущего кадра не подставляются как истина следующего кадра: native root motion между ними законен. Отдельные setup записи доказывают реальные скрытые изменения положения и HP.

Активность четырёх бойцов проверяется независимо от enum `AiNormal`: в каждом из десяти полных 60-секундных игровых интервалов у каждого NPC должны быть native choice, принятая native попытка анимации и hit attempt. За весь эпизод каждый должен действительно потерять HP. Явные `observation_actor_mapping` связывают ID trace и observation даже для одинаковых script типов; большой event journal читается последовательно, в отчёте остаются counts и SHA-256.

Cadence измеряется в игровых и wall мс. Wall измерения используют `wall_sample_us` одного `steady_clock` процесса, связанные с sim step; часы разных машин не сравниваются. Cost складывает `sample_step` и `packet_transport` одного шага, включая native hooks, признаки/visibility/rays, builder/journal serialization, watch list, сборку/сериализацию/отправку IPC. Gate p95 ≤ 2 мс считается только для четырёх наблюдаемых NPC. Запись самого timing journal исключена из cost instrumentation; paired throughput включает и её.

Native on/off parity использует strict comparator этапа 2 для seeds 17/29/43. Это подтверждает пассивность подписки в human 1H melee, без заявления о spells, всех типах NPC, сетевой latency или обученном policy. ASR/TTS состояние и GPU snapshot не являются проверкой задержек этих сервисов.

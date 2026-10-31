# Боевой API и арбитр тела NPC

Этап 4 [плана GRU](npc-body-control-plan.md). Общий CPU `BodyController` используется headless runner и обычным AI bridge. С v54 сервер по умолчанию запрашивает `policy:"engine"`: примитивы выбирает P30 в движке; внешний provider остаётся запасным режимом. Настоящие NPC, физика, анимационные окна и расчёт урона исполняют их.

Этап прошёл полный серверный прогон `stage4-v5`: [результаты и границы проверки](../reports/npc-body/2026-10-13-body-api/validation-notes.md), [37 сценариев](../reports/npc-body/2026-10-13-body-api/body-api.md), [машинный отчёт](../reports/npc-body/2026-10-13-body-api/body-api.json).

## Владение и границы первой версии

Для NPC существует один body lease. Без lease источник — native AI. Acquire требует `body_control=true`, наблюдаемого участника сценария, живую видимую текущую цель и безопасную границу. Боевые действия первой версии требуют drawn 1H/2H sword. Bridge дополнительно запрещает acquire у attached NPC, во время разговора и soft action.

Эта версия управляет melee-примитивами относительно **существующей native цели**. `body_goal` повышает версию этой цели; выбор другого NPC и стратегические задачи пока не входят в API. Native смена цели завершает lease через безопасную передачу управления.

Под lease native routines/perceptions и обычные body queues не выбирают следующий примитив. `npc_act`, attach/action и разговорные команды, меняющие тело, отклоняются. Сюжетные/native изменения идут через отложенный takeover: они инвалидируют команду, ждут завершения защищённого сегмента и применяются после возврата native. Текущая атака не теряет animation/wait gates из-за очистки очереди.

Повторная настройка `body_watch` в обычном мосту с v45 разрешена при действующем lease или ожидающей заявке `acquire_pending`, если новый список содержит всех таких NPC и сохраняет текущую частоту. Лимит остаётся 4 NPC, частота — 10 или 20 Гц; смена частоты, исключение занятого NPC или пустой список при владельцах отклоняются. У всех оставшихся NPC сохраняются lease, `session_epoch`, `world_epoch`, `entity_id`, `control_generation`, ожидающая заявка и возрастающий `observation_seq`; команды по прежнему lease продолжают приниматься. Новые NPC получают строки с ближайшего тика, удалённые свободные NPC больше их не получают. Без lease и заявок сохраняется прежнее поведение: перенастройка полностью сбрасывает наблюдения и управление, пустой список выключает подписку. Headless runner сохраняет прежнее правило перенастройки.

World load/reset завершает эпизод, очищает управление и меняет IDs. Reconnect сначала завершает старое владение на безопасной границе и только затем меняет observation epoch.

## Общий wire API

Headless использует JSONL поле `cmd`; bridge — такое же значение в `type` и свой transport request ID. Оба адаптера вызывают один parser. Runner сохраняет точный общий encoder в `body_reply`, а вокруг добавляет свой status envelope.

| Операция | Назначение |
|---|---|
| `body_acquire` | Получить lease: actor, provider, текущая цель, `body_control`, mode, timeout; с v54 необязательное `policy:"engine"` |
| `body_goal` | Повысить `goal_version` для текущей native melee цели |
| `body_action` | Передать выбранный примитив с полным контекстом наблюдения |
| `body_heartbeat` | Обновить wall watchdog без изменения действия |
| `body_release` | Запросить возврат native на безопасной границе |
| `body_disconnect` | Потеря конкретного lease/provider; требуется полная identity |
| `body_control_status` | Прочитать owner, pending execution, masks и допустимость решения |
| `body_events` | Забрать ограниченную очередь событий с явным счётчиком потерь |

Lease identity: `session_epoch`, `world_epoch`, `entity_id`, `control_generation`, `provider`. Команда дополнительно несёт `goal_version`, `target_entity_id`, `observation_seq`, `issued_game_ms`, `command_id`, `action_id` и `ttl_ms`. Runtime entity ID обозначает экземпляр NPC, а не script symbol.

`command_id` положителен и возрастает внутри lease. Последние 64 receipts кэшируются: точный повтор возвращает прежний ответ с `duplicate=true`, изменённый повтор отклоняется, слишком старый ID не исполняется. Pending command slot один; во время исполнения новое действие отклоняется, а `continue` сохраняет тот же execution ID.

Snapshot должен быть последним для этого NPC, с точным game timestamp; в realtime обычного моста — последним отправленным провайдеру, ещё не отправленные строки его не устаревают. После acquire и смены goal нужен новый sample; прежнее наблюдение нельзя присвоить новому управлению. TTL 1–1000 мс проверяется при принятии и снова перед фактическим началом. В realtime учитываются game age и wall age исходного engine snapshot. В lockstep TTL определяется game time, а wall потеря provider отдельно завершает аварийный эпизод.

Режим lease привязан к runner mode; в обычной игре разрешён только realtime. Timeout provider — 100–5000 wall мс, измеряется локальным `steady_clock`. Часы Mac и сервера не сравниваются.

### Политика в движке (v54)

`body_acquire` с необязательным `policy:"engine"` получает обычный lease, а выбор
действий выполняет P30 в движке. Без этого поля внешние `body_action` работают как
прежде. Сервер-владелец по-прежнему поддерживает `body_heartbeat`; watchdog lease
не меняется. При отсутствии загруженной `.bodynet` acquire отклоняется с
`body_net_unavailable`. Движок загружает сеть один раз при старте из
`OPENGOTHIC_BODY_NET`, по умолчанию — из
`~/.local/share/gothic-ai/body/nets/p30.bodynet`; `hello.body_net` сообщает
`loaded`, `checkpoint_sha256` и `error`. Ошибка загрузки не останавливает игру.

Один `BodyNet` общий для всех NPC, `BodyPolicyRuntime` и память GRU — свои у lease.
Адаптер `common/game/bodyenginepolicy.*` получает новые periodic JSON-строки
`body_observations` после захвата. Identity, sequence и гейты прежние;
memory-only строки, повтор `observation_too_old` и сброс после `hidden_reset`
соответствуют `BodyFight._row`. Десятый последовательный временный отказ
завершает бой; остальные отказы политики завершают его сразу.
Решение проходит тот же validator и `FightAlgo::resolve`, что внешний
`body_action`, с `ttl_ms:100` от времени строки. Обычные наблюдения продолжают идти.

При отказе NPC возвращается родному ИИ на безопасной границе: событие ownership
имеет `reason:"native_restored"` и `release_reason:"policy_fallback:<reason>"`.
Раз в секунду wall-времени движок отправляет отдельное сообщение `body_policy`
с identity lease, `decisions` (применённые решения), `fallbacks` по причинам,
`steps` (число измеренных шагов) и `p50_us` / `p99_us`
за интервал; при отсутствии шагов процентиля равны 0. Сразу при конце по отказу
приходит сводка `final:true`: runtime fallback имеет `reason:"policy_fallback:<reason>"`,
ошибка политики — `policy_fallback:policy_error`, окончательный отказ применения —
`policy_fallback:body_action_rejected`.
Обычная сводка содержит `final:false`, `reason:""`.
При запросе release последний интервал отправляется с `final:true` и причиной
release в `reason`, чтобы не терять счётчики короткого боя.
Необязательные интервальные словари `selected_actions` и `accepted_actions`
содержат счётчики выбранных сетью и принятых движком действий. Ключи в порядке
`actions` файла `.bodynet`: `continue`, `approach`, `turn_to_target`, `strafe_left`,
`strafe_right`, `jump_back`, `attack_front`, `attack_left`, `attack_right`, `block`,
`wait_200`, `wait_300`. Выбор считается до применения, поэтому включает отказы;
сумма принятых равна `decisions`. Необязательное целое `hidden_reset` считает
сбросы памяти, включая создание памяти нового lease и `observation_too_old`.
После сводки интервальные счётчики обнуляются. Отсутствующие поля означают
`{}`, `{}` и `0`; старые серверы игнорируют дополнение, версия остаётся v54.
[Полная схема](protocol.md#политика-боя-в-движке-v54).
Серверный `BodyPilot` по умолчанию использует `--body-policy engine`
(`GOTHIC_BODY_POLICY`). Он создаётся без `--body-url`, но начинает бой только при
`hello.body_net.loaded:true`. Отсутствие поля равнозначно незагруженной сети:
заданный `--body-url` / `GOTHIC_BODY_URL` включает запасной сервис, без адреса
остаётся native AI. Причина из hello логируется один раз на подключение.
Явный `--body-policy service` требует адрес; `--no-neural-body` отключает оба пути.
В engine сервер поддерживает watch, lease, heartbeat, goal и release, но не создаёт
`PolicyClient`, не запускает сторож сервиса и не отправляет `body_action`.
`body_policy` попадает только в бой с совпадающими `entity_id` и
`control_generation`; счётчики интервалов, включая выбранные/принятые действия
и сбросы памяти (`policy_memory_resets`), суммируются в `FightSummary`.
Поскольку отдельные времена шагов не передаются, `engine_max_interval_p50_us` / `engine_p99_us`
сохраняют максимумы интервальных процентилей, а не процентиль за весь бой.
`policy_fallbacks` сохраняет все причины из сводок; `fallbacks` не включает
повторяемые отказы действий (`command_busy`, `stale_observation` и остальные
`TRANSIENT_ACTION_REJECTIONS`). Завершение по `policy_fallback:*` всегда учитывается
как минимум как один отказ политики, даже без финальной сводки.
В service те же счётчики действий собираются по ответам политики и `body_action`.
Лог конца сохраняет формат `neural-body end … decisions=… policy=engine|service`
без новых полей для действий.
Лаунчер передаёт `llm.body_policy` серверу, а необязательный путь `llm.body_net`
игре как `OPENGOTHIC_BODY_NET`.

### Ожидание безопасной границы (v41)

`body_acquire` принимает необязательный `await_boundary_ms`: целое число от 0 до 2000.
По умолчанию 0 сохраняет немедленный захват или отказ, в том числе в арене. Пилот живой игры
передаёт 1500 мс, ограничивая ожидание оставшейся частью общего окна захвата в 5 секунд.

Если мешает только безопасная граница, а цель, родной `ZS_ATTACK`, происхождение боевой
очереди и переход к врагу проходят проверки, движок принимает одну отложенную заявку
на актёра. Очередь, переход, анимации и native AI до выдачи lease продолжают работать.
Повтор не продлевает срок заявки; чужая заявка на того же актёра не вытесняет владельца заявки.

Receipt `accepted: true`, `reason: "acquire_pending"` означает только принятую заявку:
владелец ещё `native`, `control_generation` равен 0. Положительный `acquire_request_id`
связывает receipt, исполнение и отмену; клиент может задать его заранее, чтобы отменить
заявку даже при потере receipt. ID проверяется вместе с provider, actor и process/world epochs.

На первой безопасной границе внутри NPC tick, до следующего выбора родного боя,
`BodyController` повторно проверяет условия, очищает только доказанную боевую очередь и
переход и выдаёт lease. Событие `phase: "ownership"`, `reason: "acquired"` несёт ID заявки,
полную identity lease и `observation_seq` на момент захвата. Только после этого клиент
считает тело своим; для первого действия требуется новое наблюдение выданной generation.
При немедленном успешном захвате с `await_boundary_ms > 0` receipt также содержит
`acquire_request_id` и `observation_seq`. В строке `body_control_status` ожидающая заявка
обозначается `acquire_pending: true`, её ID и native-владельцем; у выданного lease остаётся ID
заявки. Отсутствующие добавочные поля имеют значения `false` и 0.

Отмена приходит событием `phase: "ownership"`, `reason: "acquire_cancelled"` с ID заявки
и `acquire_cancel_reason`. Истечение wall-окна, смена цели, падение NPC, разговор,
сюжетная очередь, release, disconnect и загрузка не оставляют заявку на будущий захват.
Причины включают `expired`, `target_changed`, `actor_down`, `talking`, `native_combat_ended`,
`native_queue_busy`, `native_overlay_busy`, `goto_busy`, `released`, `disconnect`, `destroyed`,
`loading` и `pause`; остальные проверки возвращают свою причину отказа acquire.
Пауза отменяет заявку. При загрузке отмена отправляется в прежнем world epoch перед
событием загрузки и сохраняется в журнале; сервер завершает ожидания старого мира.

Для отмены до выдачи lease `body_release` или `body_disconnect` передаёт те же
provider/actor/epochs, `acquire_request_id` и `control_generation: 0`. Если заявка уже
исполнилась, такой запрос освобождает только lease этой заявки; он не затрагивает
другое владение. Смена `body_watch` допускается в обычном мосту и до завершения заявки
или освобождения lease: все NPC с `acquire_pending` или lease должны остаться в списке,
а частота — прежней. Оставшиеся заявки сохраняют ID, срок и условия захвата; смена списка
не отменяет и не продлевает ожидание. Чтобы убрать такого NPC из подписки, сначала требуется
подтверждённое завершение заявки и освобождение lease.

Пилот повторяет истёкшую заявку в оставшемся окне захвата, не учащая попытки.
Сводка содержит число отправленных и исполненных заявок, p50/max времени
«отправка заявки → подтверждённый захват». Это локальное wall-время сервера;
оно включает доставку подтверждения и не смешивает часы разных машин.

## Примитивы, masks и события

| ID | Действие |
|---|---|
| 0 | `continue` |
| 1 | `approach` |
| 2 | `turn_to_target` |
| 3, 4 | `strafe_left`, `strafe_right` |
| 5 | `jump_back` |
| 6, 7, 8 | `attack_front`, `attack_left`, `attack_right` |
| 9 | `block` |
| 10, 11 | `wait_200`, `wait_300` |

Pure `FightAlgo` resolver получает числовое/boolean состояние и не вызывает RNG, не двигает очередь и не запускает анимацию. Native normalization и policy masks используют общие правила; исполнитель — один `Npc::implAttack`. Masks учитывают sword/target, gates, доступность анимаций, угол, дальность и collision ray. Полный native consume/skip порядок сохраняется при выключенном внешнем управлении.

`wait_200` сохраняет native преобразование в jump back при тесной дистанции и focus. Поэтому intent, effective primitive и физическое исполнение записываются отдельно. Mask не обещает попадание: range/focus/collision проверяются снова, включая native damage commit.

В observations schema 1 добавлены authoritative `action_mask`, `action_mask_valid`, `control_generation`, `goal.version`, `control_allowed` и `can_decide`. Пассивная availability mask сохраняется отдельно. Без lease или при lock новое решение недоступно; observations продолжают поступать для будущей recurrent memory.

События связывают command и execution: `accepted`, `started`, `continued`, `completed`, `rejected`, `cancelled`. Start определяется реальной сменой pose/segment, движения, поворота или native wait deadline; consume сам по себе не является start. Completion следует engine условию движения/ожидания или завершению конкретного animation segment. Ранняя замена сегмента — cancellation.

Видимость обязательна для нового решения и старта команды. Краткая потеря видимости из-за направления взгляда в strafe-анимации не прерывает уже начатый защищённый сегмент. После его завершения NPC возвращается в Idle; если цель остаётся скрытой после обновления позы, управление возвращается native AI. Скрытые признаки цели и новые действия при этом недоступны. Смерть, удаление или смена самой цели инвалидируют команду независимо от видимости.

События имеют process/world/control epochs, goal/actor/target/command/execution IDs, возрастающий `event_seq`, game/wall timestamps, effective primitive и причину. Очередь ограничена 512 событиями; pending native mutations — ограниченным буфером. Старые epoch события сохраняются в durable journal, но не входят в новый IPC packet после reset.

## Потеря provider

В realtime watchdog или disconnect отменяет административную команду и запрещает следующий внешний выбор. Непрерываемая физическая часть заканчивается штатно; на следующей безопасной границе возвращается native. Журнал фиксирует обнаружение потери, оставшийся lock и фактическую передачу ownership.

В lockstep timeout/disconnect ставит `infrastructure_failure`; `advance` заканчивается с соответствующим `stopped_on`. Этот эпизод не продолжается как обычная native дуэль. Watchdog работает также при неподвижном game time и pause. `advance` во время pause отклоняется; resume не ускоряет симуляцию для компенсации wall паузы.

## Проверка изменения подписки

`gothic-body-engine-test` не требует игровых ассетов: на фикстуре
`scripts/body_net.py fixture` проверяет последовательность решений относительно `BodyPolicyRuntime`,
валидацию примитивов, причину fallback и предел последовательных временных отказов.
`gothic-body-watch-test` дополнительно проверяет ответ `body_acquire` без сети,
применение решений к настоящим NPC, watchdog владельца и возврат native.
Без uv и Python он собирается с прежними проверками тела, без сетевой фикстуры.

`gothic-body-watch-test` рядом с resolver-тестом запускает два настоящих NPC без окна и звука: рост подписки под lease, прежние identity и команды, захват нового NPC, отказы при исключении владельца и смене частоты, сокращение списка и сохранение ожидающей заявки до выдачи lease.

При `AiFar`/`AiFar2` и текущей дистанции героя от 30 м release и realtime watchdog возвращают native не позднее следующего тика, очищая боевую позу и её waits; физические реакции сохраняются для native, причина остаётся в `release_reason` события `native_restored`. Стенд проверяет удаление героя на 90 м как наблюдателя и как настоящей цели, а также его возвращение рядом до пересчёта дальнего режима. Его C++ вызов `statusJson(&actor,true)` добавляет к `body_control_status` поля `process_policy` (0 — игрок, 1 — обычный, 2 — дальний от 30 м, 3 — от 60 м), `segment_remaining_ms`, `fai_gate`, `ani_gate`, `ai_gate`, `output_gate`, `interruptible`; обычный wire status сохраняет прежнюю схему. Учебные актёры могут сохранять полное обновление вдали.

Сборка: `cmake --build OpenGothic/build --target gothic-body-watch-test`. Запуск из корня репозитория: `OpenGothic/build/opengothic/gothic-body-watch-test -g "$GOTHIC_DIR" -g2 -devmode --scenario "$PWD/scripts/bench/body-api.bench"`. Нужны игровые assets; сохранение не требуется. На Ryzen сборка и запуск идут через `scripts/gq`.

## Запуск на dev-ubuntu

Отдельный mutable runtime: `~/gothic-ai/body-api/source`, `build`, `runs`. Assets/save и baseline этапов 1–3 используются из архивов. Mac выполняет только SSH orchestration.

```sh
scripts/body-api-remote.sh preflight
scripts/body-api-remote.sh sync
scripts/body-api-remote.sh build
scripts/body-api-remote.sh run stage4-validation
scripts/body-api-remote.sh collect stage4-validation
```

Сборка включает CPU resolver/arbiter test executable. Серверный сценарий исполняется transient user unit с CPUQuota 200%, MemoryMax 12G, TasksMax 128 и wall watchdog 2400 с. Общий simulation lock допускает один engine процесс. Продовые контейнеры, Docker и ASR/TTS не изменяются.

`body_fixture` и `body_self_test` — test-only headless endpoints. Fixture устанавливает реальные позиции/weapon/pose/HP/locks и проверяет legacy/native пути; движения, атаки и урон остаются в engine. Геометрический сценарий ищет настоящую преграду через world rays, а не подставляет boolean столкновения.

Полная проверка и независимый report используют [body-api-v1.json](../evals/npc-body/body-api-v1.json): все 12 примитивов, fault injection, masks/purity, ownership, realtime native fallback, lockstep infrastructure failure и strict native-off traces для seeds 17/29/43. Smoke не считается готовностью этапа. Большие журналы и бинарники остаются на сервере; в репозитории сохраняются компактные результаты и код повторного запуска.

# Реестр фич

Всё, что мы строим, включено по умолчанию. Выключатель нужен для сравнений «до и после» и для отладки. Одна строка — одна фича: как её выключить, где она живёт, что её проверяет, где о ней прочитать. Как части связаны между собой — в карте проекта [`README.md`](README.md).

PR, который добавляет фичу или выключатель, добавляет строку сюда.

**Как читать столбцы:**
- **Как выключить.** `--no-X` — флаг `gothic-ai serve` (`server/src/gothic_ai/cli.py`). `GOTHIC_AI_*=0` и `OPENGOTHIC_*=0` — переменные окружения игры. `[GAME] x=0` — ключ `Gothic.ini`; у физических ключей есть и команда Marvin `toggle x`. «нет» — выключателя нет, фича работает всегда.
- **Модуль.** Серверные модули лежат в `server/src/gothic_ai/`, движковые пути даны от `OpenGothic/`.
- **Проверка.** e2e — сценарий `SCENARIO=<имя>` из шапки `scripts/e2e.sh`. Бенч — `scripts/bench/<имя>.bench`, запуск `scripts/bench.sh <имя>`. «живой» — сценарий проверяет только с настоящей моделью (`LLM=chatgpt`) и тратит квоту.

## Сервер: разговор и голос

| Фича | Как выключить | Модуль | Проверка | Документ |
|---|---|---|---|---|
| Голос героя: push-to-talk, текст на лету | `--no-voice` | `voice.py`, `asr-service/` | e2e `m3`, `m5e` | [protocol.md](protocol.md#голос-push-to-talk) |
| Голос NPC: TTS, потоковая озвучка | `--no-tts` (остаются субтитры) | `tts.py`, `tts-service/` | e2e `tts`; бенч `tts-record` | [protocol.md](protocol.md#голоса-npc-tts) |
| Запросы к Responses по своему WebSocket на NPC (кэш промпта на подписке) | `--no-websocket` | `llm/openai_responses.py` | — | — |
| Память NPC по веткам сохранений | нет | `memory.py` | e2e `m3` | [protocol.md](protocol.md#сессии-и-память) |
| Разговор без окна: адресата выбирает сервер | нет | `talk.py`, `turn.py` | e2e `m5e` | [npc-agent.md](npc-agent.md#живой-разговор-m5e) |
| Быстрые звуки, пока модель думает (`say.filler`) | нет | `dialogue.py` | — | [protocol.md](protocol.md#сервер--игра) |
| Настроение реплики: лицо и голос | нет | `dialogue.py`; мост — `ai_bridge/aibridge.cpp` (`moodFace`) | бенчи `mood-ai`, `faces`, `face-fade` | [npc-agent.md](npc-agent.md#лицо) |
| Реплики в бою от быстрой модели (Gemma) | `--no-quick`; без `--quick-url` и `--decider-url` её нет | `dialogue.py`, `server/deploy/gemma/` | e2e `m5` с `QUICK_URL` | [npc-agent.md](npc-agent.md#реплики-в-бою-быстрая-модель) |
| Трейсы ходов и их повтор | `--no-trace` | `trace.py`, `replay.py` | e2e `m5` с `QUICK_URL` (читает трейс) | [npc-agent.md](npc-agent.md#трасса-и-воспроизведение) |
| Приказы спутнику без модели | нет | `commands.py`, `orders.py` | e2e `orders`, `companion` | [npc-agent.md](npc-agent.md#приказы-спутникам) |
| Успокоить нападающего словами (`calm`) | нет | `dialogue.py`, `orders.py` | e2e `calm` | [npc-agent.md](npc-agent.md#успокоить-словами) |
| Общая беседа: по слову за раз | нет | `group.py` | e2e `group` | [npc-tools.md](npc-tools.md#сделано-б2-одно-слово-за-раз) |
| Кто вклинится в беседу по характеру | `--no-interject`; без планировщика его нет | `interject.py` | e2e `interject` (живой) | [npc-tools.md](npc-tools.md#сделано-б3-кто-вклинится-по-характеру) |
| Карточки характера | нет | `characters.py`, `gamedata/characters.json` | e2e `haggle` (живой); `cards` их пишет | [npc-tools.md](npc-tools.md#карточки-характера) |

## Сервер: быстрый контур и тело

| Фича | Как выключить | Модуль | Проверка | Документ |
|---|---|---|---|---|
| Планировщик NPC вне разговора (Strands Decider) | `--no-brain`; без `--decider-url` его нет | `brain/`, `decider-service/` | e2e `m4` (что нужно от моста), `m5`, `day`, `initiative` с `DECIDER_URL` | [npc-agent.md](npc-agent.md), [npc-scheduler.md](npc-scheduler.md) |
| Угрозы свободным NPC — сначала поведенческой модели | `--no-hold-threats` | `brain/`; `cli.py` шлёт `threat_control` | e2e `m5c` | [protocol.md](protocol.md#планировщик-npc-v5) |
| Бой и бегство по решению модели | `--no-apply-threats` (решения только в трейсе) | `brain/reflex.py` | e2e `m5c`, `rescue`, `rescue_self` | [npc-agent.md](npc-agent.md#свободные-npc-m5c) |
| Следующий час NPC из нового меню | `--no-apply-day` (меню в тени, работает старый `plan`) | `brain/planner.py`, `brain/day.py`, `daylife.py` | e2e `day` | [npc-tools.md](npc-tools.md#сделано-быт) |
| NPC сам подходит и заговаривает | нет; без планировщика её нет | `initiative.py`, `concerns.py` | e2e `initiative` | [npc-tools.md](npc-tools.md#сделано-инициатива) |
| Реакция на физический шум: посмотреть, пойти проверить | нет; без планировщика её нет | `brain/situation.py`, `brain/apply.py` | — | [protocol.md](protocol.md#физический-шум-v36) |
| Боевое тело P30 | `--no-neural-body`; без `--body-url` его нет | `bodypilot.py`, `body-service/`; движок — `common/game/bodycontroller.*` | e2e `neural`, `neural-companion`; бенчи `body-api`, `body-arena`, `body-observe` | [npc-body-control-plan.md](npc-body-control-plan.md), [npc-body-api.md](npc-body-api.md) |
| До четырёх независимых боёв P30 | `--neural-body-max 1` ограничивает одним боем; допустимо 1–4, по умолчанию 4; несколько боёв только при протоколе ≥45 из `hello` | `bodypilot.py`, `cli.py`; отдельное TCP-соединение и lease у каждого боя; старый/неизвестный протокол — `watch_busy` только в логе и счётчике, без изменения подписки, отказ движка ≥45 — `watch_refused` | `test_bodypilot.py`; e2e `neural-group` проверяет три одновременных lease, решения, release, снятие подписки и задержку сервиса | [npc-body-control-plan.md](npc-body-control-plan.md) |
| Переходы P30 между разговором, боем и загрузкой | вместе с пилотом; сценарии требуют `GOTHIC_BODY_URL` | `bodypilot.py`, `scenarios/companions.py`, `scenarios/neural_knockout.py` (отрезок `neural-talk`), `scenarios/neural_load.py` | e2e `neural-talk`: сначала NPC не из `GIL_BDT` нападает после слов с `AR_NONE`, герой переживает нокаут и встаёт, lease снят и NPC в распорядке; затем враждебный бандит: разговор, нападение, бегство, распорядок, повторная встреча (нападение или разговор), новый бой с чистой памятью; после `return_to_second_fight` ≥20 решений или 20 с, новое бегство и `fight_end_clean`: `target_lost`/`npc_unavailable`, release ≤2 с, через 2 с нет lease, `body` и подписки; один удар героя после 15 с без урона цели, число и сумма `stall_broken` как диагностика P30 (ожидание `hero_down` до 30 с остаётся только для отдельного нокаута); звери убраны, мечник отведён; `neural-load`: загрузка при трёх lease, снятие подписки и новый бой | [npc-body-control-plan.md](npc-body-control-plan.md) |
| Изменение подписки тела без потери текущих боёв | вместе с пилотом | `game.py`, `bodypilot.py`; все NPC с ожидающим захватом остаются в списке, при прежней generation кэш свободных актёров очищается, lease и ожидающие захваты сохраняются; неудачное сжатие повторяется и не блокирует retarget | `test_body_control.py`, `test_bodypilot.py` | [npc-body-control-plan.md](npc-body-control-plan.md) |
| Память P30 после подвисаний игры | `gothic-body serve-net --no-allow-stale-memory-only` возвращает проверку возраста строк памяти | `bodypilot.py`, `body-service/src/gothic_body/runtime.py`, `serving_net.py` | `test_bodypilot.py`, `test_runtime.py`, `test_serving_net.py`; e2e `neural`, `neural-companion` | [npc-body-control-plan.md](npc-body-control-plan.md) |
| Волки слышат тон героя | `--no-beasts` | `beasts.py`; мост — `ai_bridge/beasts*` | e2e `wolf`; бенч `wolf` | [beasts.md](beasts.md) |

## Сервер: восприятие и знание

| Фича | Как выключить | Модуль | Проверка | Документ |
|---|---|---|---|---|
| Восприятие NPC, имена только знакомым | нет | `perception.py`; мост — `ai_bridge/npcview*` | e2e `see` | [npc-tools.md](npc-tools.md#сделано-м1-вижу-сейчас) |
| Память увиденного, улики | нет | `sightings.py` | e2e `seen`, `remembers`, `trace`, `broken` | [npc-tools.md](npc-tools.md#сделано-м2-видел-недавно) |
| Слышимость голоса героя в обход скал | нет | `talk.py`; мост — `npc_roster.heard` | e2e `hearing` | [npc-agent.md](npc-agent.md#слух-и-защита-издалека) |
| Предметы на земле: NPC видит и поднимает | нет | `npc_tools.py`; мост | e2e `ground` | [protocol.md](protocol.md#предметы-на-земле-v22) |
| Книга мест, ответ «где» | нет; без книги — только вейпоинты | `places.py`, `whereabouts.py`, `gamedata/places.json` | e2e `cave` (`places` только выгружает точки) | [npc-tools.md](npc-tools.md#сделано-м3-карта-мест) |
| Кто что знает о мире по своим диалогам | нет | `lore.py`, `gamedata/lore.json` | e2e `knows` | [npc-tools.md](npc-tools.md#сделано-м4-знаю) |
| Знание мира из скриптов по главе | `--no-knowledge` | `knowledge.py` | e2e `m6` | [npc-agent.md](npc-agent.md#знания-о-мире-m6) |

## Сервер: правда, дела, ложь

| Фича | Как выключить | Модуль | Проверка | Документ |
|---|---|---|---|---|
| Хроника ветки сохранения | нет | `chronicle.py` | все сценарии лжи и сюжета | [story-graph.md](story-graph.md) |
| Ложь героя о квестах и её раскрытие | нет | `quests.py`, `trust.py` | e2e `lie` | [story-graph.md](story-graph.md) |
| Проверка слов через свидетеля | нет | `witnesses.py` | e2e `witness` | [npc-tools.md](npc-tools.md#сделано-свидетель) |
| Прикрытие героя свидетелем: сделка, страх, раскрытие | `--no-cover` или `--no-crimes` | `deals.py`, `witnesses.py`, `crimes.py` | e2e `cover`; pytest `test_cover.py`, `test_deals.py` | [npc-wave3.md](npc-wave3.md#как-работает-сейчас) |
| Слухи о NPC и знание о раскрытом прикрытии: обстановка и пометки свидетеля | новые прикрытия — `--no-cover` или `--no-crimes`; старые факты читаются | `social.py`, `people_knowledge.py`, `witnesses.py`, `npc_tools.py` | e2e `cover_subject`; pytest `test_people_knowledge.py`, `test_social.py`, тесты читателей слухов | [npc-wave3.md](npc-wave3.md#как-работает-сейчас) |
| Слухи по знакомым | нет | `social.py` (`Rumors`) | e2e `rumor` | [story-graph.md](story-graph.md) |
| Пересказ слуха вслух рядом с героем | нет | `gossip.py` | e2e `gossip`; бенч `gossip` | [npc-tools.md](npc-tools.md#сделано-двое-при-герое) |
| Преступления, свидетели, закон общины | `--no-crimes` | `crimes.py`, `crime_law*.py`; мост — `hero_deed` | e2e `crimes`, `crime_talk`, `crime_law` | [crime.md](crime.md) |
| Реакция свидетеля на месте: правила роли и характера, родные SVM, зов стражи, память ванили | `--no-crimes` | `crime_react.py`, `crime_memory.py`; мост — `threat_control.deeds`, v44 | e2e `crime_react`, pytest `test_crime_react.py`, `test_crime_memory.py` | [crime.md](crime.md) |
| Встреча со знающим NPC: возмещение пострадавшему, изгнание из дома, страж и известный убийца, закрытые ворота для вора | `--no-crimes` | `crime_meet.py`, `crime_memory.py`, `amends.py`, `initiative.py`, `crime_law_graph.py`; мост — удержание входа, v46 | `crime_meet`: 37/37, живой Amends 24/24; pytest `test_crime_meet.py`, `test_crime_memory.py`, `test_crime_law_graph.py`, `test_initiative.py` | [crime.md](crime.md) |
| Сюжетные ответы на преступления: квест вернуть украденное, свидетель заговаривает, жертва требует возмещения | `--no-crimes`; квесты — ещё и `--no-showrunner` | `crimes.py`, `occasions.py`, `showrunner.py`, `chains.py`, `amends.py` | e2e `crime_director` (с `LLM=chatgpt` — живой шоураннер) | [crime.md](crime.md) |
| Гнев Ксардаса: ответ некроманта | `--no-wrath` | `wrath.py`; мост — `ai_bridge/necromancy*` | e2e `wrath`; бенч `necro` | [xardas-wrath.md](xardas-wrath.md) |

## Сервер: сделки и поручения

| Фича | Как выключить | Модуль | Проверка | Документ |
|---|---|---|---|---|
| Сделки: окно, торг, долги | нет | `deals.py` | e2e `deal`, `debt`, `haggle` (живой); бенч `deal` | [npc-tools.md](npc-tools.md#сделки) |
| Опыт, обучение, платные задания, тайник | нет | `contracts.py`, `gamedata/stash.json` | e2e `contract` | [contracts.md](contracts.md) |
| Сундуки NPC | `--no-chests` | `chests.py`; мост — `ai_bridge/npcchests*` | e2e `chests`, `chests_lobart` | [npc-tools.md](npc-tools.md#сундуки) |
| Проводник: отвести героя | `--no-lead` | `guide.py` | e2e `lead` | [npc-tools.md](npc-tools.md#сделано-проводник) |
| Встреча с героем в назначенный час | `--no-meet`; без проводника её нет | `meetings.py` | e2e `meet`, `meet_missed` | [npc-tools.md](npc-tools.md#сделано-встречи) |
| Свои поручения NPC: рассказать, отнести | `--no-errands`; без проводника их нет | `errands.py` | e2e `errand`, `errand_fork`, `errand_far`, `errand_give` | [npc-tools.md](npc-tools.md#сделано-поручения) |

## Сервер: сюжет

| Фича | Как выключить | Модуль | Проверка | Документ |
|---|---|---|---|---|
| Граф сюжета и правки режиссёра | граф не грузится, только если выключены все четыре: `--no-director`, `--no-knowledge`, `--no-questlog`, `--no-crimes`; свой файл — `--story-graph PATH` | `story.py`, `validator.py`, `gamedata/storygraph.py` | e2e `graph`, `programs` | [story-graph.md](story-graph.md) |
| Бандиты у башни Ксардаса без скриптовой засады | `--no-free-bandits` | `story.py` (`freed`) | e2e `companion` (проверка `freed`) | [npc-agent.md](npc-agent.md#приказы-спутникам) |
| Ремонт квестов после смерти | `--no-director` | `director.py` | e2e `repair` | [story-graph.md](story-graph.md) |
| Квесты скриптов в разговоре и ложь о них | `--no-director` | `vanilla.py` | e2e `vgive`, `vlie`, `vcheck` | [story-graph.md](story-graph.md) |
| Расплата за ложь: сториллеты и ванильные диалоги | `--no-director` | `amends.py`, `consequences.py` | e2e `amends`, `amends_deal`, `vlie` | [story-graph.md](story-graph.md) |
| Персонажи режиссёра | `--no-director` | `cast.py`, `casting.py` | e2e `own`, `cast`, `person`, `character` | [director.md](director.md) |
| Квесты режиссёра как объект, автор и цепочки | `--no-director` | `questbook.py`, `authoring.py`, `chains.py` | e2e `authored`, `stages`, `own_repair`, `chain`, `requires` | [director.md](director.md) |
| Триггеры режиссёра | нет | `triggers.py`; мост | e2e `trigger` | [director.md](director.md) |
| Ход квестов скриптов | `--no-questlog` | `questlog.py` | e2e `questlog` | [director.md](director.md) |
| Поводы режиссёра | `--no-occasions`; без книги мест их нет | `occasions.py` | e2e `occasions` | [director.md](director.md) |
| Шоураннер: новые сюжетные линии | `--no-showrunner`; в сценариях выключен сам | `showrunner.py` | e2e `showrunner`, `vanilla_line`; `showrunner_live`, `vanilla_line_live` (живые) | [director.md](director.md) |

## Движок: мост

| Фича | Как выключить | Модуль | Проверка | Документ |
|---|---|---|---|---|
| Мост к ИИ-серверу | `GOTHIC_AI=0` | `ai_bridge/aibridge.cpp` | все e2e | [protocol.md](protocol.md) |
| Изменение списка `body_watch` под lease или с заявкой на захват (v45) | нет; без владельцев остаётся полный сброс | `ai_bridge/aibridge.cpp`, `common/game/bodyobservations.*`, `common/game/bodycontroller.*` | `gothic-body-watch-test`; e2e `neural` | [npc-body-api.md](npc-body-api.md), [npc-body-observations.md](npc-body-observations.md) |
| Диалоговые функции скриптов как программы без VM | `GOTHIC_AI_STORYEXEC=0` | `ai_bridge/dialoggraph.cpp` | e2e `graph`, `programs` | [story-graph.md](story-graph.md) |

## Движок: физика Jolt

Сам Jolt выключателя не имеет: Bullet убран. `OPENGOTHIC_PHYSICS_THREADS` задаёт число потоков, это не выключатель.

| Фича | Как выключить | Модуль | Проверка | Документ |
|---|---|---|---|---|
| Рэгдоллы при смерти и оглушении | `[GAME] ragdoll=0` | `common/physics/ragdoll.cpp` | бенчи `ragdoll`, `ragdoll-disabled`, `ragdoll20-*` | [physics.md](physics.md) |
| Возврат живых NPC после падения со скалы или в воду | `[GAME] ragdoll_quest_guard=0` | `common/world/objects/npc.cpp` | бенчи `immortal-return`, `spell-quest-return` | [physics.md](physics.md) |
| Кинематические тела живых NPC в 20 м от героя | `[GAME] living_bodies=0` | `common/world/objects/npc.cpp` | бенчи `living-bodies`, `living-radius`, `living-seat` | [physics.md](physics.md) |
| Удар стрелы в кость | `[GAME] arrow_physics=0` | `common/physics/arrowtrace.h` | бенч `arrow-reaction` | [physics.md](physics.md) |
| Стрелы застревают в теле и в мире | `[GAME] arrow_sticking=0` | `common/physics/arrowtrace.h` | бенч `arrow-sticking` | [physics.md](physics.md) |
| Физика заклинаний: кулак ветра, смерч, огненный шар | `[GAME] spell_physics=0` | `common/world/bullet.cpp`, `common/world/world.cpp` | бенчи `spell-physics`, `spell-targets` | [physics.md](physics.md) |
| Подвижные vob и их сохранение | `[GAME] movable_vobs=0` | `common/world/movablevobs.*` | бенчи `movable-vobs*`, `movable-candle`, `movable-census` | [physics.md](physics.md) |
| Физический шум: звук и восприятие NPC | `[GAME] physics_noise=0` | `common/physics/physicsnoise.h` | бенчи `physics-noise`, `physics-noise-on`, `physics-noise-off` | [physics.md](physics.md) |
| Физические следы в восприятии NPC | нет | `ai_bridge/npcview*` | e2e `trace`; бенчи `physics-traces`, `trace-e2e` | [physics.md](physics.md) |
| Ткань мира в радиусе 40 м: флаги, знамёна и паруса | `[GAME] cloth=0`; Marvin `toggle cloth` | `common/physics/cloth.*`, `clothgeometry.*`, `worldcloth.*`, `common/graphics/clothvisual.*`; `data/cloth-vobs.csv`, `cloth-world.csv` | `check-cloth`; бенчи `cloth*` | [physics.md](physics.md#этап-6-разрушение-вода-ткань) |
| Разрушение вещей и оград, обломки как улики | `[GAME] destruction=0`; без кэша резки — пустой `OPENGOTHIC_FRACTURE` | `common/physics/fracture.cpp` | бенчи `destruction*`, `broken-e2e`; e2e `broken` | [physics.md](physics.md) |
| Вода: плавучесть вещей и тел | `[GAME] buoyancy=0` | `common/physics/dynamicworld.cpp` | бенчи `water*`, `water-bodies*` | [physics.md](physics.md) |
| CharacterVirtual героя | `[GAME] character_virtual=0` | `common/game/movealgo.cpp`, `common/physics/collisionworld.cpp` | бенчи `character-virtual*` | [physics.md](physics.md) |
| CharacterVirtual NPC-людей в 20 м от героя | `[GAME] npc_character_virtual=0` | `common/game/movealgo.cpp`, `common/physics/collisionworld.cpp` | бенчи `npc-character-virtual*` | [physics.md](physics.md) |
| CharacterVirtual наземных монстров в 20 м от героя (по умолчанию **0**) | `[GAME] monster_character_virtual=0`; Marvin `toggle monster_character_virtual` | `common/game/movealgo.cpp`, `common/physics/collisionworld.cpp` | `check-physics`; бенч `monster-character-virtual`; e2e `wolf` с ON | [Приёмка шага 4, часть 1](../reports/physics/2026-10-18-monster-character-virtual/validation-notes.md) |

## Движок: анимация

Все — в `common/graphics/mesh/`.

| Фича | Как выключить | Модуль | Проверка | Документ |
|---|---|---|---|---|
| Инерциальные переходы | `OPENGOTHIC_ANIBLEND=0` | `animblend.*` | бенч `anim` | — |
| Foot IK, у четвероногих тоже | `OPENGOTHIC_FOOTIK=0` | `footik.*` | бенчи `ik-ab`, `ik-moves`, `ik-stairs`, `ik-sweep`, `quad-wolf`, `quad-zoo` | — |
| Моргание | `OPENGOTHIC_BLINK=0` | `eyeblink.*` | бенчи `blink`, `listen` | — |
| Жесты рук при речи | `OPENGOTHIC_GESTURES=0` | `talkgestures.*` | бенч `gestures` | — |
| Губы по голосу | `OPENGOTHIC_LIPSYNC=0` | `lipsync.*` | бенчи `lips`, `lips-dialog` | — |
| Взгляд | `OPENGOTHIC_GAZE=0` | `gaze.*` | бенчи `gaze`, `gaze-ambient` | — |
| Наклон на бегу | `OPENGOTHIC_LEAN=0` | `lean.*` | бенч `lean` | — |
| Вздрагивание от ударов | `OPENGOTHIC_FLINCH=0` | `flinch.*` | бенч `flinch` | — |
| Стрелок поворачивает корпус к цели | `OPENGOTHIC_AIM=0` | `aim.*` | бенч `aim` | — |
| Тетива натягивается рукой | `OPENGOTHIC_BOWDRAW=0` | `bowdraw.*` | бенч `bow-draw` | [manual-aim.md](manual-aim.md) |
| Шаги с поднятым луком | `OPENGOTHIC_AIMWALK=0` | `aimwalk.*` | бенч `aim-walk` | [manual-aim.md](manual-aim.md) |
| Зоны попадания по костям | нет | `hitzones.*` | бенч `zones` | — |

## Движок: рендер

Что включилось, пишет строка `render:` в логе игры.

| Фича | Как выключить | Модуль | Проверка | Документ |
|---|---|---|---|---|
| Свои PBR-материалы мира и статичных vob: нормаль/ORM, GGX солнца и светильников, отражение неба без GI и с GI Probes; AO только на рассеянное небо; albedo без загрузки оригинала, с переписью подмены | убрать каталог контента или `OPENGOTHIC_CONTENT=` | `common/resources.cpp`, `graphics/material.*`, `drawbuckets.*`, `renderer.*`, `shader/materials/`, `shader/lighting/` | синтетическая DDS-фикстура `scripts/content/test-material.py`, `rf.bench` Vulkan/Metal | [Своя игра](own-content.md#как-работает-сейчас) |
| Ray query на Mac: тени точечных светильников, свет не проходит сквозь стены (на Linux и Windows — с апстрима) | `-rt 0` | `common/commandline.h` | замер рендера | [отчёт](../reports/render-features/2026-10-18/README.md) |
| GI-пробы на дискретной видеокарте с трассировкой | `-gi 0` | `common/gothic.cpp` | замер рендера | [отчёт](../reports/render-features/2026-10-18/README.md) |
| Сглаживание CMAA2 (только при `vidResIndex=0`) | `-aa 0` | `common/commandline.h` | замер рендера | [отчёт](../reports/render-features/2026-10-18/README.md) |

## Движок: стрельба

| Фича | Как выключить | Модуль | Проверка | Документ |
|---|---|---|---|---|
| Ручной прицел героя | `OPENGOTHIC_MANUALAIM=0` или `[GAME] manualAim=0` | `common/game/archery.cpp`, `aimcamera.h` | бенчи `manual-aim`, `aim-cant`, `aim-corner`, `aim-over-edge`, `aim-stance`, `aim-steep-wall`, `crossbow`, `edge-shot`, `bow-pole`, `hero-balance`, `archery-smoke` | [manual-aim.md](manual-aim.md) |
| Ручной прицел лучников NPC | `OPENGOTHIC_NPCAIM=0` или `[GAME] npcManualAim=0` | `common/game/archerai.*` | бенчи `npc-mech`, `npc-fight`, `npc-lead`; `npc-today` — кости, для сравнения | [manual-aim.md](manual-aim.md) |

## Выключено по умолчанию

Это не фичи игры, а инструменты и опции. Включаются явно:
- `gothic-ai serve --scenario <имя>`, `--journal`, `--voice-files`;
- HD-текстуры: каталог в `OPENGOTHIC_TEXTURES`, его передаёт лаунчер, когда текстуры сделаны;
- Перепись оригинальных ресурсов: `OPENGOTHIC_CENSUS=<путь к json>`, запись при выходе и Marvin `census`; `common/contentcensus.*`, наблюдатель VFS, `scripts/content-census.py`; проверки — `smoke` на Ryzen и `scripts/test_content_census.py`, [own-content.md](own-content.md);
- `GOTHIC_AI_STORYCHECK=1` — сверка условий скриптов с поднятыми; в `-devmode` включена сама;
- `OPENGOTHIC_ANIM_SKIP_FAR=1`, `OPENGOTHIC_FOOTIK_WATCHED_ONLY=1` — экономия на дальних NPC для прогонов тела;
- `OPENGOTHIC_BENCH*`, `OPENGOTHIC_*_LOG`, `OPENGOTHIC_*_TRACE` — стенд, журналы и трассы.
- Обложки шортсов шаблона B: поле `cover` плана сравнения, `gothic-shorts cover`, `upload --thumbnail` и `thumbnail`; модули `shorts/src/gothic_shorts/{cover,compare,youtube,cli}.py`, проверки `shorts/tests/test_{cut,cover,youtube}.py`, [команды и шаблон](../shorts/README.md#covers). Без поля или команды обложки нет.
- Субтитры сравнения по времени строк в плане: `gothic-shorts srt <workspace> --plan FILE` (по умолчанию `plan.json`); `gothic_shorts.{compare,cli}`, проверки `shorts/tests/test_srt.py`, [команда](../shorts/README.md#uploading-to-youtube). Explainer без compare-плана использует `words.ru.json`.

| Фича стенда | Как включить / выключить | Модуль | Проверка | Документ |
|---|---|---|---|---|
| Общий кэш Linux-бинарников движка и `storygraph` по хэшу исходников | включён в `scripts/ryzen.sh`; `clean-cache` удаляет записи старше двух часов; `RYZEN_WORKTREE` задаёт имя папки worktree | `scripts/ryzen.sh` | `test_ryzen`, `test_ryzen_lifetime`, `test_ryzen_lock`; e2e `stages` в двух worktree | [ryzen.md](ryzen.md), [compute.md](compute.md) |
| Офлайн-перепись площадей, масштаба UV и плиточности текстур четырёх миров со статичными vob | `scripts/content-textures.sh`; без запуска скрипта не работает | `scripts/content/world-textures.cpp` | `scripts/content-textures.sh --self-test`, офлайн-прогон четырёх миров со сверкой площади ≤ 0,1 % | [own-content.md](own-content.md), [CSV и отчёт](../reports/content/2026-10-world-textures/README.md) |
| Популяция навигации: N NPC, независимые состояния, выборка softmax и награды Detour без перезагрузки мира | `navdemos population … sample\|argmax … RECORD`; `navdemos physics cv\|native`, в `nav_rl.py` CV по умолчанию; `RECORD=0` выключает записи; `hero near\|keep`, `kill-slot SLOT AFTER_MS` — проверки стенда | `common/navdemos.*`, минимальные вставки в `worldobjects.cpp`, `npc.cpp`, `MoveAlgo::useCharacterVirtual`; `scripts/nav_rl.py` | `test_nav_rl.py`, `nav_rl.py check/equivalence/population/death-smoke` | [navigation.md](navigation.md) |

Остальная обучаемая навигация ([navigation.md](navigation.md)) — пока исследование, в игру не включена.

## Пробелы покрытия

Эти фичи включены, но ни один сценарий e2e и ни один бенч их не проверяет. pytest здесь не в счёт.

- Запросы к Responses по WebSocket (`--no-websocket`).
- Быстрые звуки, пока модель думает (`say.filler`).
- Реакция планировщика на физический шум (`npc_event noise` → `npc_act investigate`). Движковую часть проверяют бенчи `physics-noise*`, серверную — никто.

Проверяются не в каждом прогоне:
- **только с моделями на dev-ubuntu:** реплики в бою от Gemma, трейс хода (`m5` с `QUICK_URL`), планировщик в `day` и `initiative` (с `DECIDER_URL`);
- **только живыми прогонами:** кто вклинится в беседу (`interject`), карточки характера (`haggle`), шоураннер на настоящей модели (`showrunner_live`, `vanilla_line_live`).

## Инструменты разработки

| Фича | Как выключить | Модуль | Проверка | Документ |
|---|---|---|---|---|
| Три игры e2e на Ryzen: слоты, отдельные порты и окна | sharing требует `--display --display-shared`; старый `--display` и `--display-exclusive` занимают всё окно; ёмкость задаёт профиль `gq` | `scripts/gq.py`, `scripts/ryzen.sh`, `scripts/e2e.sh` | `scripts/tests/test_gq.py`, `test_ryzen.py`; e2e `orders`, `fight_reactions_bandits`, `calm` ([отчёт](../reports/compute/2026-10-19-ryzen-three-windows/README.md)) | [compute.md](compute.md#очередь-gq), [ryzen.md](ryzen.md) |
| Начальная позиция X11-окна | без `TEMPEST_WINDOW_POS=x,y` — прежнее размещение | `lib/Tempest/Engine/system/api/x11api.cpp`, `scripts/ryzen.sh` | e2e трёх окон на Ryzen, `test_ryzen.py` | [ryzen.md](ryzen.md) |
| Средний FPS представленных кадров в логе раз в 10 с | `OPENGOTHIC_FPS_LOG` выключен по умолчанию; нужна сборка с `OPENGOTHIC_ENABLE_APP_CADENCE=ON` | `common/appcadence.cpp` | одиночные и тройные e2e на Ryzen | [ryzen.md](ryzen.md) |
| Сравнение генераторов PBR-материалов по тексту и реестр собственных ресурсов | только явный запуск `scripts/content/bakeoff.py` / `run-materials.sh`; `enable-flux` после принятия соглашения пользователем; статус `draft` | `scripts/content/`, `data/content/registry.jsonl` | `registry.py check` (50), `test_registry.py` (55), `test_flux.py`, `test_run_materials.py`, `test_capture_materials.py`; 70 инференций dev-ubuntu; 10 снимков `rf.bench` на Ryzen через `gq` | [own-content.md](own-content.md#как-работает-сейчас) |

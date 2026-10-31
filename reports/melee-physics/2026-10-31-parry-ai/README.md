# Native ИИ: парад под замах и удар по оглушённому

2026-10-31. Шаг 3 симметрии боёвки NRFTW, `[GAME] melee_parry_ai=1` по умолчанию, только при `melee_parry=1`. Люди с 1H, 2H и кулаками; герой и P30 не затронуты.

`MV_BLOCK` предсказывает ближайший будущий `OPTIMAL_FRAME` текущего сегмента атаки цели с учётом скорости слоя и возврата после остановки кадра. Старт — контакт − окно/2 − lead + нормальная ошибка, σ = 150 − 1,1 × талант мс. Ошибка использует RNG FAI; при смене слоя или `sOrigin` выбирается заново. NPC держит ход, стойку и поворот до старта, не дольше 1000 мс. У оглушённого — подход, поворот, удар с прежними воротами выносливости и `faiWait`.

## Mac-смоук ON/OFF

Сценарий [`melee-parry-ai.bench`](../../../scripts/bench/melee-parry-ai.bench): seed 73013, новый `NEWWORLD.ZEN`, герой 1H с `cheat god` и `still player on`, бессмертный `BDT_1013_BANDIT_L`, начальная дистанция 160 см, native `fight BDT_1013_BANDIT_L player`, 50 запросов атаки через 1400 мс. Штатный талант бандита — 10: окно 133 мс, σ 139 мс, aim 66,5 мс. Lead — 0. Мост отключён (`GOTHIC_AI=0`), ИИ-сервисы не используются.

Для каждого прогона: `BENCH_HOST=local WORLD=NEWWORLD.ZEN NO_BUILD=1 OPENGOTHIC_BODY_TRACE=1 MELEE_PARRY=1`, готовая база выбрана через `OPENGOTHIC_BASE_FINGERPRINT`; затем:

```sh
MELEE_PARRY_AI=1 scripts/bench.sh melee-parry-ai "$PWD/.bench/melee-parry-ai-on"
MELEE_PARRY_AI=0 scripts/bench.sh melee-parry-ai "$PWD/.bench/melee-parry-ai-off"
```

Оба запуска завершились кодом 0 и `done` в `bench.log`; сценарий занял 74 433 мс игрового времени. Сырые журналы — `.bench/melee-parry-ai-{on,off}/events.jsonl` в этом worktree.

| Счётчик | ON | OFF |
|---|---:|---:|
| Событий всего | 13 507 | 10 826 |
| `hit_attempt` героя | 11 | 3 |
| `hit_attempt` бандита | 31 | 33 |
| `parade_timed` бандита | 1 | 0 |
| `parade_skipped` бандита | 2 (`no_swing`) | 0 |
| Парирования бандита | 1 | 1 |
| Поздняя защита бандита | 0 | 0 |
| Медиана `parade_ms` всех отбитых контактов бандита | 100 мс | 100 мс |
| `stunned_target`: move / turn / attack | 0 / 1 / 1 | 0 / 0 / 0 |
| Новых фаз во всём журнале | 5 | 0 |

ON: `parade_timed` предсказал контакт через 100 мс, ошибка −166,529663 мс дала `wait_ms=0`. На 4601 мс бандит парировал, герой получил `stun_start(reason=parried)`; на 5134 мс бандит принял удар по нему (`stunned_target(action=attack)`), за 467 мс до конца оглушения на 5601 мс. Медиана 100 мс внутри окна 133 мс. OFF сохраняет обычное парирование, новых фаз нет.

Это смоук механики: по одному отбитому контакту, выводов о балансе и доле парирований нет. Отложенный старт, смена замаха и таймаут отдельно в живом бою не измерялись. Матрица дуэлей на Ryzen не запускалась.

Подготовка: в первом варианте сценария герой нажимал `action` только один раз; после первого парирования штатный `PlayerControl::clearInput` оставлял следующие запросы без action. Получилось только 2 `hit_attempt` героя; при этом уже были парирование на 100 мс и удар по оглушённому. Сценарий исправлен: отпускать и снова нажимать action перед каждым замахом. Это настройка ввода стенда, движок не менялся. До запуска игры также остановлена одна подготовка базы (код 143), чтобы отключить мост и выбрать уже готовую базу.

## Проверки

- Запрошенный `MAC_BUILD=0 ~/.codex/checks/gothic/engine-targets.sh . gothic-meleeparry-contract gothic-meleestamina-contract gothic-physics-contract`: своя заявка `gq` снята до запуска тестов, позиция 8 → 5, оценка ожидания до 13 200 с; выход 143. По правилу репозитория те же три контракта перенесены на Mac.
- `cmake --build OpenGothic/build --target gothic-meleeparry-contract gothic-meleestamina-contract gothic-physics-contract` и запуск трёх бинарников из `OpenGothic/build/opengothic/`: все 3 прошли с кодом 0 с первого запуска. Контракт парирования проверяет σ 150/84/40, середину окна при нулевой ошибке, прошлый старт → сейчас, error/lead и подход/поворот/удар у оглушённого.
- `cmake --build OpenGothic/build --target check-bodypolicy`: код 0, 1280 шагов, 4 эпизода, 0 расхождений; max logits error 9,313225746e−9, max hidden error 5,960464478e−8.
- `scripts/build-engine.sh`: код 0, первая полная сборка Mac прошла; финальный запуск после контрактов — `ninja: no work to do`.
- `bash -n` для трёх изменённых shell-скриптов, AST-разбор `body_sim_bench.py`, `git diff --check`: код 0.

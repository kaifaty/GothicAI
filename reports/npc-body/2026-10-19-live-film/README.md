# Живые бои P30 на Mac — 19 октября 2026

Записаны три дубля для глав «Живая игра» и «Честно о слабостях» (биты 38 и 41). Все файлы съёмки лежат вне Git: `~/.local/share/gothic-ai/explain/combat-ai/footage/`.

Каждый `shot.mp4`: **1920×1080, 30 FPS, 750 кадров, 25,000 с**. Каждый `shot.wav`: **48 кГц, stereo PCM16, 1 200 000 отсчётов на канал, 25,000 с**. Музыка и фоновые звуки выключены: `SOUND=1 MUSIC=0 WORLD_SOUNDS=0`.

Герою включены **бессмертие (`immortal player on`) и 100 000 HP**, каждому бандиту заданы 1 800 HP. Одноручные шпаги — штатные из `BDT_1002_BANDIT_L`. Сцена — поле `NW_BIGFARM_VORPOSTEN1_01_RIGHTFIELD_02`, полдень. В одиночном и групповом дублях герой наносит настоящие удары через ввод игрока (7 и 8 попаданий в журнале); в пассивном — 0.

Работали обычные `GameServer` и `BodyPilot`, P30 — существующий сервис Ryzen из `[llm].body_url`. Сервис и веса не менялись. `LLM=ASR=TTS=fake`; клиенты языковых и речевых моделей не создавались. Перед каждым запуском выполнен `pgrep -fl Gothic2Notr`; точное имя процесса проверялось отдельно, чужие игры дожидались завершения.

## Видео и данные

| Дубль | Видео | Звук | Данные |
|---|---|---|---|
| `live-takeover` | [shot.mp4](/Users/kaifat/.local/share/gothic-ai/explain/combat-ai/footage/live-takeover/shot.mp4) | [shot.wav](/Users/kaifat/.local/share/gothic-ai/explain/combat-ai/footage/live-takeover/shot.wav) | [`actions.json`](/Users/kaifat/.local/share/gothic-ai/explain/combat-ai/footage/live-takeover/actions.json) |
| `live-group` | [shot.mp4](/Users/kaifat/.local/share/gothic-ai/explain/combat-ai/footage/live-group/shot.mp4) | [shot.wav](/Users/kaifat/.local/share/gothic-ai/explain/combat-ai/footage/live-group/shot.wav) | [`actions-DIR_NPC_LIVE_BANDIT_1.json`](/Users/kaifat/.local/share/gothic-ai/explain/combat-ai/footage/live-group/actions-DIR_NPC_LIVE_BANDIT_1.json); [`actions-DIR_NPC_LIVE_BANDIT_2.json`](/Users/kaifat/.local/share/gothic-ai/explain/combat-ai/footage/live-group/actions-DIR_NPC_LIVE_BANDIT_2.json); [`actions-DIR_NPC_LIVE_BANDIT_3.json`](/Users/kaifat/.local/share/gothic-ai/explain/combat-ai/footage/live-group/actions-DIR_NPC_LIVE_BANDIT_3.json) |
| `live-passive` | [shot.mp4](/Users/kaifat/.local/share/gothic-ai/explain/combat-ai/footage/live-passive/shot.mp4) | [shot.wav](/Users/kaifat/.local/share/gothic-ai/explain/combat-ai/footage/live-passive/shot.wav) | [`actions.json`](/Users/kaifat/.local/share/gothic-ai/explain/combat-ai/footage/live-passive/actions.json) |

## Решения и синхронизация

| Дубль / NPC | Решений в `actions` | Принято за весь сеанс | `neural-body end decisions=` | `fallback` |
|---|---:|---:|---:|---:|
| takeover / 1 | 63 | 65 | 65 | 0 |
| group / 1 | 67 | 67 | 67 | 0 |
| group / 2 | 47 | 48 | 48 | 0 |
| group / 3 | 37 | 38 | 38 | 0 |
| passive / 1 | 78 | 79 | 79 | 0 |

Полные журналы — `events.jsonl`, `metrics.csv`, `server.log` рядом с каждым дублем. Пять lease получены; захвата без lease при правильной постановке не было. Незавершённый первый вариант групповой подготовки сохранён отдельно в `~/.local/share/gothic-ai/explain/combat-ai/data/live-group-preparation-race/`: ранний захват отменяла следующая команда постановки `fight` (`native_script_queue`). Пилот теперь запускается после подготовки и события `rec`.

`actions` содержит принятые решения P30 (`accepted`), а не обещание начала анимации. Кадр берётся непосредственно из события движка; `t = frame / 30`. Контроллерные события привязаны к полной lease из строки `neural-body start`. До захвата допускается штатная перенумерация сущностей при настройке наблюдения; после захвата идентичность проверяется строго. Все пять файлов имеют по 750 точек головы и HP; головы находятся в кадре во всех 750 кадрах каждого трека.

Пять решений после последнего видеокадра не включены в `actions`, но учтены при полной сверке: 2 у takeover, по 1 у group/2, group/3 и passive. Привязка решений вне записи к последнему кадру не подделывается.

В passive первое решение — `approach` на 0,367 с, затем `wait_200`. Самая длинная серия — **17 подряд**, с **16,633 до 21,533 с**. HP бандита остаются 1 800, герой не атакует.

## Звук

Проверены PCM-значения, тишины вместо дорожки нет. `nonzero` ниже считает значения обоих каналов.

| Дубль | Peak PCM16 | Ненулевых значений | RMS, dBFS |
|---|---:|---:|---:|
| takeover | 12 917 | 1 487 362 | −29,21 |
| group | 26 832 | 2 012 543 | −26,94 |
| passive | 12 054 | 719 859 | −42,95 |

## Проверка трека на изображении

Точки нанесены на голову из соответствующего трека; в группе жёлтая — NPC 1, голубая — NPC 2, пурпурная — NPC 3. PNG лежат вне Git. Проверены все девять изображений.

| Дубль | Кадр 150, 5 с | Кадр 375, 12,5 с | Кадр 600, 20 с |
|---|---|---|---|
| `live-takeover` | [track-0150.png](/Users/kaifat/.local/share/gothic-ai/explain/combat-ai/footage/live-takeover/track-0150.png) | [track-0375.png](/Users/kaifat/.local/share/gothic-ai/explain/combat-ai/footage/live-takeover/track-0375.png) | [track-0600.png](/Users/kaifat/.local/share/gothic-ai/explain/combat-ai/footage/live-takeover/track-0600.png) |
| `live-group` | [track-0150.png](/Users/kaifat/.local/share/gothic-ai/explain/combat-ai/footage/live-group/track-0150.png) | [track-0375.png](/Users/kaifat/.local/share/gothic-ai/explain/combat-ai/footage/live-group/track-0375.png) | [track-0600.png](/Users/kaifat/.local/share/gothic-ai/explain/combat-ai/footage/live-group/track-0600.png) |
| `live-passive` | [track-0150.png](/Users/kaifat/.local/share/gothic-ai/explain/combat-ai/footage/live-passive/track-0150.png) | [track-0375.png](/Users/kaifat/.local/share/gothic-ai/explain/combat-ai/footage/live-passive/track-0375.png) | [track-0600.png](/Users/kaifat/.local/share/gothic-ai/explain/combat-ai/footage/live-passive/track-0600.png) |

## Проверки

- `cmake --build OpenGothic/build --target Gothic2Notr -j 6` — успешно.
- `cd body-service && uv run pytest -q ../scripts/tests/test_body_replay_film.py ../scripts/tests/test_body_live_film.py ../scripts/tests/test_body_live_capture.py ../scripts/tests/test_body_live_bench_contract.py` — **80 passed**.
- Все пять экспортов `body_live_film.py labels` прошли пороги: active ≥10; passive ≥5 подряд `wait_200`.
- `ffprobe`, проверка WAV и девяти PNG — успешно; три файла по 25 с, без молчащих WAV.

Инструменты и способ повторной съёмки описаны в [bench README](../../../scripts/bench/README.md#live-p30-capture-on-mac). Готовые `footage/p30-*`, `footage/old/` и `shorts/explain/` не менялись.

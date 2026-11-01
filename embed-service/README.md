# gothic-embed

## Как работает сейчас

HTTP-сервис кодирует запросы и старые реплики NPC для поиска в памяти. По умолчанию используется
`ai-forever/FRIDA`: 1536 компонент, `bfloat16`, `cuda`, около 1,9 ГБ видеопамяти.
Модель выбрана по [замеру recall](../reports/npc-memory/2026-11-01-recall-eval/README.md):
47 из 50 пересказов против 22 из 50 у поиска по словам.

Модель загружается и прогревается одним запросом в рабочем потоке. До конца прогрева HTTP отвечает
503. Все запросы кодирования выполняются последовательно в одном потоке под замком; HTTP-цикл
остаётся свободным. Префиксы добавляет сервис по таблице из
[`recall_eval.py`](../scripts/recall_eval.py): FRIDA и USER2 — `search_query: ` / `search_document: `,
e5 — `query: ` / `passage: `, e5-instruct — инструкция поиска реплик для запроса и пустой префикс
для документа. Для модели вне таблицы с подсказкой `query` используются `encode_query` /
`encode_document`; иначе — обычный `encode` без префикса.

Сервис не хранит реплики или векторы. Клиент памяти реализуется отдельно. Проверки используют
поддельный кодировщик без torch, загрузки весов и GPU. Сервис развёрнут на dev-ubuntu 1 ноября
2026 года. По замеру при развёртывании занятая видеопамять выросла с 13,4 до 15,3 ГБ; запрос
с Mac через Tailscale занимает 25 мс по p50.

## Контракт

`GET /health` после прогрева:

```json
{"model": "ai-forever/FRIDA", "dim": 1536, "dtype": "bfloat16", "device": "cuda"}
```

До готовности: `503 {"error": "loading"}`.

`POST /embed`, тело:

```json
{"texts": ["Что ты говорил о барьере?", "Барьер пал."], "kind": "query"}
```

`kind` — `query` или `document`; клиент передаёт текст без префиксов. Ответ:

```json
{"model": "ai-forever/FRIDA", "dim": 1536, "vectors": "<base64>"}
```

`vectors` — base64 от массива float32 little-endian в порядке строк: `len(texts) × dim`.
Каждая строка L2-нормирована в float32. `model` и `dim` соответствуют загруженной модели.

Невалидный JSON, тело не объект, пустой `texts` или не список строк, более 256 текстов,
текст длиннее 4000 символов и неверный `kind` дают `400 {"error": "<причина>"}`.
Границы 256 и 4000 включены. Пока модель грузится, корректный `/embed` также получает 503.

## Настройки и запуск

| Переменная | По умолчанию | Назначение |
|---|---|---|
| `EMBED_MODEL` | `ai-forever/FRIDA` | Имя модели sentence-transformers |
| `EMBED_DTYPE` | `bfloat16` | `bfloat16`, `float32` или `float16` |
| `EMBED_DEVICE` | `cuda` | Устройство модели |
| `EMBED_HOST` | `127.0.0.1` | Адрес HTTP |
| `EMBED_PORT` | `47837` | Порт HTTP |

Для FRIDA `float16` запрещён: запуск завершается с понятной ошибкой до загрузки модели.
В замере косинус с fp32 составил 0,958 для fp16 и 0,998 для bf16.

На GPU-сервере:

```bash
uv sync --locked --extra gpu --no-dev
uv run --no-sync gothic-embed serve --host 127.0.0.1 --port 47837
```

Аргументы `--host` и `--port` переопределяют переменные среды. На Mac большие модели не запускаются.
Основные зависимости — `aiohttp` и `numpy`; `sentence-transformers` и torch входят только в extra `gpu`.

## Развёртывание и логи

Из корня репозитория, после ревью, с `GOTHIC_EMBED_URL` в `.env`:

```bash
scripts/deploy-gpu.sh embed
```

Скрипт копирует пакет в `~/gothic-ai/embed/`, создаёт `.venv` через `~/.local/bin/uv` с extra `gpu`,
устанавливает и перезапускает пользовательский юнит `gothic-embed`.
`GOTHIC_EMBED_URL` преобразуется в `EMBED_HOST` / `EMBED_PORT` в `~/gothic-ai/gothic-ai.env`.
Этот файл читает systemd. Модель, dtype и устройство передаются из `.env` через
`GPU_EMBED_MODEL`, `GPU_EMBED_DTYPE` и `GPU_EMBED_DEVICE`: скрипт убирает префикс `GPU_`.
Юнит привязан к ядрам 2–11 и перезапускается при ошибке. Первый запуск скачивает веса.

Команды на GPU-сервере:

```bash
systemctl --user status gothic-embed
journalctl --user -u gothic-embed -f
systemctl --user disable --now gothic-embed
```

Готовность отмечает строка `listening on H:P model=… dim=… dtype=…` после загрузки и прогрева.
Последняя команда останавливает сервис и отключает автозапуск.

## Проверка без модели

```bash
uv run --locked pytest -q
uv run ruff check .
```

## План и журнал

- 2026-11-01: подготовлены HTTP-контракт, последовательное кодирование, тесты без GPU и
  пользовательский юнит с интеграцией в `deploy-gpu.sh`; FRIDA развёрнута на dev-ubuntu.
  Видеопамять: 13,4 → 15,3 ГБ, около 1,9 ГБ у FRIDA; запрос с Mac через Tailscale — 25 мс по p50.

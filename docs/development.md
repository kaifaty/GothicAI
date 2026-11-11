# Сборка и запуск из исходников

Документ для разработчиков: как собрать игру и сервисы, запустить их и проверить. Что проект умеет — в [`README.md`](../README.md), как части связаны между собой — в [карте проекта](README.md). Пути в командах и таблицах даны от корня репозитория.

## Что где лежит

| Путь | Что |
|---|---|
| `docs/README.md` | Карта проекта: части и машины, путь реплики, модули сервера, подсистемы движка, сервисы, тесты, документы |
| `docs/MVP.md` | Первая спецификация MVP (исторический) «один живой NPC»: решения, объём работ, вехи |
| `server/` | ИИ-сервер (Python): соединение с игрой, диалоги, персоны, память, голосовой ввод, адаптер LLM, сценарии |
| `asr-service/` | Сервис распознавания речи со сменными провайдерами: Voxtral Realtime на GPU-сервере (audio.cpp), подписка ChatGPT, OpenAI-совместимые API. `deploy/voxtral/` — развёртывание Voxtral на GPU-сервере |
| `tts-service/` | Сервис синтеза речи для голосов NPC со сменными провайдерами: Inworld TTS-2 (по умолчанию), OpenAI-совместимые API по ключу и VoxCPM2 на GPU-сервере (audio.cpp, выключен с 2026-10-27). `deploy/voxcpm/` — развёртывание на GPU-сервере |
| `libs/chatgpt-auth/` | Вход через ChatGPT (OAuth PKCE) и общее хранилище токенов для сервера и ASR-сервиса |
| `launcher/core/` | Ядро лаунчера `gothic-launcher`: поднимает сервисы и игру одной командой, хранит настройки и ключи |
| `launcher/app/` | Окно лаунчера (Tauri, Svelte): мастер, запуск, журнал, настройки, значок в строке меню |
| `docs/protocol.md` | Протокол игра ↔ сервер |
| `docs/launcher.md` | План лаунчера: запуск игры и сервисов одной кнопкой, настройки и ключи в окне |
| `scripts/e2e.sh` | Сквозная проверка: сервер + новая игра, `SCENARIO=<имя>`. Сценарии по областям — в `docs/README.md`, описание каждого — в шапке скрипта |
| `scripts/make-dev-app.sh` | Обёртка отладочной сборки в `OpenGothic Dev.app` |
| `scripts/build-engine.sh` | Сборка игры: `OpenGothic/build/opengothic/Gothic2Notr` |
| `scripts/bench.sh` | Тестовый стенд механик: сценарий в игре → видео с отладочным наложением, метрики и отчёт с подозрительными кадрами, см. `scripts/bench/README.md` |
| `site/` | Сайт проекта на GitHub Pages (https://kaifaty.github.io/GothicAI/): что уже работает и дневник разработки на английском и русском, RSS; публикуется из `main` (`.github/workflows/pages.yml`), см. `site/README.md` |
| `reports/` | Итоговые исследования |
| `research_notes/` | Заметки исследований с источниками |
| `OpenGothic/` | Движок: OpenGothic (от upstream `Try/OpenGothic`) с ИИ-мостом `ai_bridge/` и IK стоп. Сторонние библиотеки лежат в `lib/` обычными файлами, без подмодулей (`lib/VENDORED.md`) |

Инструмент роликов живёт в приватном репозитории `kaifaty/gothic-shorts`.

## Сборка и запуск на macOS

Игра: всё нужное для сборки, включая библиотеки движка, лежит в репозитории.

```bash
scripts/build-engine.sh
```

Изменения из upstream OpenGothic при необходимости переносятся вручную: движок сильно отличается от оригинала.

Локальные настройки: адреса GPU-сервера и ключи API. ASR- и TTS-сервисы читают ближайший `.env` выше текущей папки, то есть `.env` в корне репозитория (он в `.gitignore`). Переменные окружения важнее файла.

```bash
cp .env.example .env
```

Модели на GPU-сервере (Strands Decider, Gemma; по выбору Voxtral для ASR и VoxCPM2 для TTS) ставятся из репозитория по ssh (`GOTHIC_GPU_SSH` в `.env`). Настройки сервера, `~/gothic-ai/gothic-ai.env`, скрипт собирает из того же `.env`: модель слушает адрес и порт из `GOTHIC_VOXTRAL_URL` / `GOTHIC_VOXCPM_URL`, а строки `GPU_<ИМЯ>=…` попадают туда как `<ИМЯ>=…`. Ключи API на сервер не уходят.

```bash
scripts/deploy-gpu.sh show            # что уйдёт на сервер
scripts/deploy-gpu.sh env             # обновить настройки; изменившиеся сервисы перезапустятся
scripts/deploy-gpu.sh voxcpm          # установить или обновить модель (или voxtral)
```

Чёткие текстуры персонажей (по желанию). У оригинальных текстур лицо 256×128, тело и броня в основном 512×512, на крупных планах они мыльные. Скрипт достаёт из архивов игры текстуры людей и монстров со всеми вариантами лиц и кожи. Потом увеличивает их в 4 раза моделью Real-ESRGAN на GPU-сервере, ужимает до 1024 по большей стороне, сохраняя цвета оригинала, и сжимает в DXT с мип-уровнями, как сжаты текстуры самой игры. В городе это добавляет около 0,1 ГБ памяти, а без сжатия добавило бы гигабайт. Результат ложится в `~/.local/share/gothic-ai/hd-textures/textures`. Лаунчер, стенд и съёмка шортсов передают эту папку игре через `OPENGOTHIC_TEXTURES`, и игра грузит эти текстуры вместо своих. Текстуры сделаны из файлов игры, поэтому в git они не попадают. На сервере нужен `realesrgan-ncnn-vulkan` из релиза Real-ESRGAN, подробности в начале скрипта. Весь прогон занимает около 3 минут.

```bash
CREATURES=1 scripts/hd-textures.sh
```

Вход в ChatGPT. Один раз: откроется браузер, токены сохранятся в `~/.config/gothic-ai/chatgpt.json`.

```bash
cd server && uv run gothic-ai login chatgpt
```

Проверить модель и задержку:

```bash
cd server && uv run gothic-ai llm-test "Кто ты?"
```

### Запуск одной командой

Лаунчер поднимает ASR, TTS и ИИ-сервер на свободных портах и ждёт их готовности, потом запускает игру. Сервис, который упал во время игры, лаунчер перезапускает. Когда игра закрывается, всё останавливается. План и устройство описаны в [docs/launcher.md](launcher.md).

Настройки и ключи лаунчера лежат в `~/.config/gothic-ai/settings.toml` и `secrets.toml` (права 600). При первом запуске туда переносятся значения из `.env`, потом лаунчер `.env` не читает. Логи каждого запуска пишутся в `~/Library/Logs/GothicAI/<время>/`. Игра работает в `~/.local/share/gothic-ai/game/`: там её сохранения, `Gothic.ini` и `log.txt`.

Поставить команду `gothic-launcher` (правки кода подхватываются без переустановки):

```bash
uv tool install --editable launcher/core
```

Один раз указать папку игры (для первого импорта — установленную Gothic II с аддоном и русской локализацией):

```bash
gothic-launcher config set game.dir "/Volumes/DevSSD/Dev/games/Gothic II"
```

После импорта `game.dir` может быть каталогом только с настройками `system/*.ini`: лаунчер выбирает готовую базу через непустой `OPENGOTHIC_BASE` или указатель этого канонического каталога в `OPENGOTHIC_BASE_CACHE`. Проверки показывают отпечаток базы и определяют русский язык по её библиотеке OU, не читая `Data` и `OU.BIN` установки. Без готовой базы сохраняются проверки установки; первый запуск соберёт базу. Проверить без запуска игры и запросов к провайдерам: `gothic-launcher check [каталог]`.

На Ryzen `scripts/ryzen.sh setup` готовит `~/gothic-ai/game-settings/` только с `system/Gothic.ini` и `system/SystemPack.ini`, переносит готовую базу по отпечатку и сохраняет отпечатки рядом. `game.dir` может указывать на этот каталог; e2e и бенчи передают отпечатки игре и не требуют копии установки. Старый `~/gothic-ai/game` setup не меняет. Если готовой части видео нет, setup предупреждает, что ролики будут пропущены.

Играть, продолжив последнее сохранение (`--new` — сразу новая игра, `-v` — вывод всех процессов в терминал):

```bash
gothic-launcher up --continue
```

Посмотреть и поменять настройки. Ключ без значения лаунчер спросит, чтобы он не остался в истории оболочки:

```bash
gothic-launcher config show
```

```bash
gothic-launcher config set secrets.inworld_api_key
```

Только сервисы, на их обычных портах, а игру запускать вручную:

```bash
gothic-launcher up --no-game
```

### Окно лаунчера

Приложение `Gothic AI` на Tauri: мастер первой настройки, кнопка «Играть», журнал реплик и настройки. Пока идёт игра, окно уходит в строку меню. Ядро и сервисы оно берёт из этого репозитория, поэтому нужны `uv` и `bun`.

Запустить в режиме разработки:

```bash
cd launcher/app && bun install && bun tauri dev
```

Собрать, подписать и поставить в `/Applications`. Подпись берётся из `GOTHIC_SIGNING_IDENTITY` в `.env` (см. `.env.example`): с постоянной подписью macOS спрашивает доступ к микрофону один раз, а не после каждой сборки.

```bash
scripts/build-launcher.sh
```

Экраны можно править в обычном браузере, без Tauri: `bun run dev` в `launcher/app` и http://localhost:47850. Вместо ядра там отвечает заглушка (`src/lib/mock.ts`), `?scenario=fresh` открывает мастер, `?scenario=failed` — ошибку запуска.

ASR-сервис (для голоса) сам моделей не запускает. По умолчанию он распознаёт потоком через Inworld STT (ключ `INWORLD_API_KEY`) с подсказками имён, а если тот недоступен — через подписку ChatGPT (Realtime API, `gpt-realtime-whisper`). Провайдеры меняются флагами `--provider` и `--fallback` или переменными `GOTHIC_ASR_PROVIDER` и `GOTHIC_ASR_FALLBACK` (`inworld`, `voxtral[:url=…]`, `chatgpt`, `openai:base_url=…,model=…,api_key_env=…`). Voxtral Mini 4B Realtime (адрес в `GOTHIC_VOXTRAL_URL`) остаётся по выбору; он работает на GPU-сервере под audio.cpp (Q4_K, ~3,5 ГБ видеопамяти); развёртывание описано в `asr-service/deploy/voxtral/install.sh`. Своего параметра языка у модели нет, поэтому наш патч к audio.cpp (`audiocpp-language.patch`) запрещает для `ru` все токены с нерусскими буквами.

```bash
cd asr-service && uv run gothic-asr serve
```

Сравнить провайдеры на своих записях (16-битный WAV). `--realtime` подаёт звук в темпе речи, как при push-to-talk:

```bash
cd asr-service && uv run gothic-asr transcribe tests/audio/*.wav --provider voxtral --fallback none --realtime
```

TTS-сервис (голоса NPC) сам моделей не запускает. По умолчанию голоса даёт Inworld TTS-2 (облако, ключ `INWORLD_API_KEY`), запасного провайдера нет: если Inworld недоступен, NPC говорят только субтитрами (как и при остановленном TTS-сервисе). Запасной включается флагом `--fallback`, например `--fallback voxcpm` для VoxCPM2 на GPU-сервере (адрес в `GOTHIC_VOXCPM_URL`); по решению пользователя от 2026-10-27 VoxCPM2 остановлен, голоса — только Inworld.

Упавший провайдер пропускается, пока фоновая проверка не увидит, что он снова работает. Проверки идут через 5 с, затем интервал удваивается до 2 минут. Если провайдер ломается посреди реплики, реплика дальше идёт без звука: переключиться на другой голос на полуслове нельзя. Следующая реплика уже озвучивается запасным, если он задан.

Сюжетные NPC в игре говорят клонами своих оригинальных голосов. Голос остальных NPC создаётся один раз по текстовому описанию из персоны. У разных провайдеров голоса разные.

Сервер и TTS (`--game-dir DIR`) получают реплики, программы и оригинальную речь из готовой базы: непустой `OPENGOTHIC_BASE`, иначе указатель канонического `DIR` в `OPENGOTHIC_BASE_CACHE` (по умолчанию `~/.cache/gothic-ai/base`). `DIR` может не содержать `Data` и `_work`. Сервисы базу не импортируют; если её нет, один раз запустите игру с установкой или задайте `OPENGOTHIC_BASE`. `storygraph` должен быть собран; путь меняется через `STORYGRAPH_TOOL`. Кэши графа и голосовых образцов привязаны к отпечатку базы, а не к mtime установки.

Синтезированные голоса на 7–10 дБ тише оригинальной озвучки игры. Поэтому сервис выравнивает каждую реплику до −16 LUFS (медиана русской озвучки Gothic II), а пики выше −1 дБ срезает лимитер. Реплики идут потоком, поэтому усиление берётся по прошлым репликам того же голоса; первая реплика нового голоса получает усиление по обычному уровню TTS (−26 LUFS). В игре выровненная реплика звучит так же громко, как оригинальная (Ксардас в одном кадре: −18,0 LUFS оригинал, −17,6…−18,6 TTS). Уровень меняется флагом `--loudness` (`--loudness none` — как отдал провайдер).

```bash
cd tts-service && uv run gothic-tts serve
```

Провайдер меняется флагами `--provider` и `--fallback` или переменными `GOTHIC_TTS_PROVIDER` и `GOTHIC_TTS_FALLBACK`. Ключи API и адрес VoxCPM2 сервис берёт из `.env` в корне репозитория (см. `.env.example`).

- `inworld` — Inworld Realtime TTS-2 по ключу `INWORLD_API_KEY`. Голос NPC один раз создаётся по описанию из персоны (Voice Design), публикуется в библиотеку голосов Inworld с тегом `gothic-ai` и запоминается в `~/.local/share/gothic-ai/inworld-voices.json`. Готовый голос из библиотеки задаётся опцией `voice` (например, `inworld:voice=Dmitry`); описание из персоны тогда управляет подачей. Более быстрая модель без управления подачей — `inworld:model=inworld-tts-2-flash`.

Только один провайдер, без запасного:

```bash
cd tts-service && uv run gothic-tts serve --provider openai --fallback none
```

Послушать и замерить (`--out` сохраняет последнюю реплику в WAV):

```bash
cd tts-service && uv run gothic-tts say "Драконы уже здесь." --voice-id xardas --voice "An old man with a deep calm voice" --out /tmp/x.wav
```

ИИ-сервер. Ждёт игру на `ws://127.0.0.1:47817` и принимает команды из консоли: `attach`, `goto`, `come`, `ani`, `talk <текст>`, `memory`, `routine`, … Бэкенд LLM выбирается флагом `--llm chatgpt|openai|local|off`, модель — `--model`. Память NPC хранится в `~/.local/share/gothic-ai/memory.sqlite3` по веткам сохранений; голос пишется с микрофона Mac (`--no-voice` выключает). NPC говорят вслух через TTS-сервис (`--no-tts` оставляет только субтитры).

```bash
cd server && uv run gothic-ai serve
```

Боевая сеть P30 по умолчанию выключена до полного переобучения (решение пользователя 2026-11-03):
бои ведёт native `FightAlgo`. Для опытов запустите `gothic-ai serve --neural-body`: тогда сеть
считается в движке (`--body-policy engine`, по умолчанию), до четырёх боёв сразу (`--neural-body-max`).
Веса движок берёт из `OPENGOTHIC_BODY_NET`, по умолчанию `~/.local/share/gothic-ai/body/nets/p30.bodynet`;
без файла и запасного `--body-url` бой остаётся родному ИИ. `--neural-body --body-policy service`
с адресом выбирает сервис для опытов; `--no-neural-body` явно выключает пилот. Подробности — [карта проекта](README.md#два-контура-npc) и
[план тела](npc-body-control-plan.md#как-работает-сейчас).

Каждый ход NPC и каждая реакция его тела пишутся в трассу `~/.local/share/gothic-ai/traces/<дата>.jsonl` (`--no-trace` выключает). Устройство агента NPC — [docs/npc-agent.md](npc-agent.md). Прогнать последний записанный ход заново, например на другой модели или с сегодняшней персоной:

```bash
cd server && uv run gothic-ai replay --persona-now
```

Проверить микрофон и распознавание (macOS спросит доступ к микрофону для терминала):

```bash
cd server && uv run gothic-ai voice-test
```

Игра в режиме разработчика. Окна чата нет: **удерживайте T где угодно, чтобы сказать фразу голосом**, а короткое нажатие T открывает строку ввода (Enter отправляет, Esc закрывает). Кому сказано, решает сервер: имя во фразе, NPC, на которого смотрит герой, текущий собеседник или ближайший NPC перед героем. Консоль Marvin открывается по F2, там доступны `ai status` и `ai attach <npc>`:

```bash
OpenGothic/build/opengothic/Gothic2Notr -g "/Volumes/DevSSD/Dev/games/Gothic II" -devmode
```

Тесты (в каждом проекте свои):

```bash
cd server && uv run pytest
```

```bash
cd asr-service && uv run pytest
```

```bash
cd tts-service && uv run pytest
```

```bash
cd libs/chatgpt-auth && uv run pytest
```

```bash
cd launcher/core && uv run pytest
```

Сквозная проверка. Запускает свои сервер и игру на отдельных портах; сохранения и память остаются во временной папке:

```bash
SCENARIO=m3 ASR=voxtral scripts/e2e.sh
```

## Windows

Готовый архив для Windows 10/11 x64 собирает GitHub Actions ([`release.yml`](../.github/workflows/release.yml), job `windows`): движок этого форка с ИИ-мостом и граф сюжета компилируются MSVC, тесты ядра и входа ChatGPT идут на Windows. Запускается только вручную, перед выпуском; на PR сборка Windows не идёт. Выпуск описан в разделе [«Выпуск»](#выпуск). Игроку компилятор не нужен; инструкция — [`START_HERE.ru.md`](../START_HERE.ru.md). Сборка из исходников — `scripts/build-engine.ps1` (Visual Studio 2022 C++, Vulkan SDK, Git), затем `Setup-GothicAI.cmd`. Только при первой настройке `setup` выбирает профиль без своих серверов: ChatGPT — мозг NPC, Inworld — распознавание с запасным ChatGPT и оригинальные голоса; повторный запуск сохраняет выбор игрока. Игра, графика и микрофон на настоящем ПК ещё не проверены.

## Linux: готовый архив

Готовый архив для Linux x64 собирает тот же [`release.yml`](../.github/workflows/release.yml), job `linux` на `ubuntu-22.04`. Цель — glibc 2.35 и новее: Ubuntu 22.04+, Debian 12+, Fedora 36+, SteamOS 3. Игроку нужны только glibc, загрузчик Vulkan драйвера, X11 и звук системы; инструкция — [`START_HERE.linux.ru.md`](../START_HERE.linux.ru.md). Повседневная сборка на Ryzen и Mac (`scripts/build-engine.sh`) от этого не меняется.

- **Компилятор — `g++-13` из PPA `ubuntu-toolchain-r/test` на Ubuntu 22.04** (`GCC_VERSION` в workflow; шаг подключает PPA, когда в архиве Ubuntu нужного GCC нет). Свой GCC 22.04 (12) не подошёл: при `-O2` он даёт ложное `-Werror=restrict` в `std::string` (`clothvisual.cpp`), libstdc++ 13 это исправила. Сборка идёт на самой 22.04, а libstdc++ и libgcc линкуются статически, поэтому планка glibc остаётся 2.35. Прогон с GCC 12 заодно нашёл в `testbench.cpp` копии в циклах по `range-loop-construct` и настоящее чтение освобождённой памяти: тернарный оператор над двумя `initializer_list`. Оба места поправлены.
- **Статический libstdc++ и libgcc** задаёт [`scripts/linux/build.sh`](../scripts/linux/build.sh) флагами CMake, а не `CMakeLists.txt` движка: `-static-libstdc++ -static-libgcc` и `-Wl,--exclude-libs,libstdc++.a:libgcc.a:libgcc_eh.a`. Последний флаг нужен из-за `-rdynamic` движка (имена в трассах падений): без него копия libstdc++ попала бы в таблицу экспорта, и драйвер Vulkan, загруженный в процесс, мог бы связаться с ней вместо своей libstdc++.
- **`Release` с `-O2`, а не `-O3` CMake:** `-O3` GCC даёт ложные `-Werror=stringop-overflow` в движке, так же как на Ryzen (`scripts/ryzen-build.sh`).
- **Vulkan — Linux SDK LunarG той же версии 1.4.357.0, что Windows:** пакеты 22.04 (1.3.204) старше заголовков, которые нужны Tempest (`VK_EXT_descriptor_heap`, `VK_KHR_maintenance5`). Архив сверяется с SHA-256, закреплённым в workflow, и с опубликованным LunarG (`sdk.lunarg.com/sdk/sha/…`). Из SDK берутся только заголовки, `libvulkan.so` для линковки и `glslangValidator`; в архив игры Vulkan не кладётся.
- **Звук:** OpenAL Soft собирается с бэкендами ALSA, PulseAudio и PipeWire, как на Ryzen (`libasound2-dev`, `libpulse-dev`, `libpipewire-0.3-dev`); библиотеки он открывает сам при запуске, поэтому в `NEEDED` их нет.
- **Проверки job:** `bash -n` и `shellcheck` скриптов Linux, импорт сервисов, `content-convert --self-test`, отказ `content-convert resolve` на пустой папке, код 2 у `gothic-launcher setup --check` без «не собран» и «не найден uv», JSON с кодом 1 у `gothic-launcher check` пустой папки. Проверки идут с одноразовыми `HOME`, `XDG_*` и `OPENGOTHIC_BASE_CACHE` в `$RUNNER_TEMP`. Модульные тесты Python на Linux гоняет `tests.yml`.
- **Упаковка — [`scripts/linux/package.sh`](../scripts/linux/package.sh)**, аналог `scripts/windows/package.ps1`. Белый список в раскладке репозитория: `Setup-GothicAI.sh`, `Start-GothicAI.sh`, `scripts/linux/common.sh`, инструкция, `LICENSE`; исходники и lock-файлы `launcher/core`, `server`, `asr-service`, `tts-service`, `libs/chatgpt-auth` без тестов; все файлы `OpenGothic/build/opengothic/` (сейчас `Gothic2Notr` и `content-convert`), `storygraph`; `tools/uv/uv` с лицензиями; `licenses/` с `SOURCE.txt` и заметкой о рантайме GCC. Итог — `GothicAI-linux-x64-r<run>-<sha7>.tar.gz` (tar сохраняет права на запуск), манифест с SHA-256 каждого файла и для каждого ELF — архитектура, `NEEDED` и наибольшая `GLIBC_`, и `SHA256SUMS`. Скрипт отказывает, если попал запрещённый файл (список `package.ps1` плюс `.o` и `.a`), ELF не x86-64, в `NEEDED` есть библиотека вне списка системных (части glibc, `ld-linux-x86-64.so.2`, `libvulkan.so.1`, `libX11.so.6`, `libXcursor.so.1`, `libasound.so.2`), нужна `GLIBC_` новее 2.35 или есть `GLIBCXX_`/`CXXABI_`. Официальной сборке `uv` дополнительно разрешён `libgcc_s.so.1`: Rust связывает его динамически, а ставит его каждая система с glibc (glibc сама загружает его для `pthread_cancel`). Тест на синтетическом дереве с подменённым `readelf` — `scripts/tests/test_linux_package.py`.
- **Локально тот же архив** собирают на Ubuntu 22.04 с теми же пакетами: `CC=gcc-13 CXX=g++-13 VULKAN_SDK=<SDK>/x86_64 LIBRARY_PATH=<SDK>/x86_64/lib/VulkanLoader/lib PATH=<SDK>/x86_64/bin:$PATH scripts/linux/build.sh`, затем `scripts/linux/package.sh --uv-tar … --uv-licenses … --out-dir … --name …`. Сборку на более новой системе `package.sh`, скорее всего, отклонит по версии glibc.

## Выпуск

Выпуск делает один ручной workflow [`release.yml`](../.github/workflows/release.yml) (`name: Release`): `gh workflow run release.yml -f windows=true -f linux=true -f release=draft`. Входы `windows` и `linux` выбирают платформы, `release` — что сделать с архивами:

- `none` (по умолчанию) — только сборка и проверки, архивы остаются на раннерах;
- `draft` — черновик prerelease: мейнтейнер проверяет архивы и публикует его сам;
- `prerelease` — сразу опубликованный prerelease.

Job `windows` и `linux` идут параллельно с правами `contents: read` и при `release != none` передают архив, манифест и `SHA256SUMS` артефактом workflow на один день. Job `release` — единственный с `contents: write` — ждёт обе, идёт, если ни одна не упала и хотя бы одна прошла (невыбранная платформа не мешает), сверяет переданные файлы с их `SHA256SUMS`, делает общий `SHA256SUMS` по всем архивам и манифестам и создаёт один релиз: тег `r<run_number>.<run_attempt>-<sha7>`, заголовок `Gothic AI r<run_number>, preliminary (<платформы>)`.

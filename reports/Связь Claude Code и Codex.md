# Очередь и хуки заменяют клики по Codex

На штатной установке (ChatGPT.app с codex-cli 0.162 на macOS) **ни один плагин, MCP-сервер, мост или оркестратор не может вмешаться в идущий ход треда Codex desktop или прервать его**. Приложение держит тред в собственном app-server-процессе, подключённом по приватному stdio, без сокета и без порта. С сентября 2026 Codex ещё и запрещает второму процессу писать в тред, который кто-то держит. При этом три вещи доступны штатно, и для супервизии их хватает. Первая: **читать прогресс** из сохранённой истории треда, то есть rollout-файлов в `~/.codex/sessions` или ответа `thread/read`. Это не живой поток, но через `tail -F` задержка небольшая. Вторая: **ставить follow-up** официальной командой `codex queue --thread <id> --message …`. Codex desktop забирает такое сообщение, как только тред простаивает, то есть между ходами, а не посреди хода. Третья: **получать уведомление о конце хода** через `notify` или `Stop`-хук Codex. Хук пишет либо в файл, за которым следит `Monitor` в сессии Claude, либо прямо в inbox-сокет сессии Claude Code: межсессионные сообщения (cross-session messaging) общедоступны начиная с v2.1.224. Настоящие `turn/steer` и `turn/interrupt` извне появляются, только если перезапустить ChatGPT.app на общем демоне app-server через недокументированную переменную `CODEX_APP_SERVER_WS_URL`. Этот обход описан в community-гисте, он рабочий, но неофициальный. Всё остальное запускает собственные сессии Codex: официальный `openai/codex-plugin-cc`, community MCP-серверы, AgentBridge, Conductor, claude-squad, адаптеры ACP и A2A. Это полезно, чтобы делегировать новые задачи, но не чтобы управлять уже открытыми тредами. Старый путь `codex mcp-server` удалён в 0.154.0. Рекомендация: начать со связки «`codex queue` + `notify` → `Monitor`», а общий демон пробовать отдельно, на тестовом треде, когда действительно понадобится вмешиваться посреди хода.

## Десктоп держит тред в приватном stdio-процессе, поэтому steer извне закрыт

Сам протокол умеет всё, что нужно супервизору. `codex app-server` работает по JSON-RPC 2.0 (вторая версия протокола) и предоставляет такие методы:

- `thread/start`, `thread/resume`, `thread/fork`, `thread/read`, `thread/list`, `thread/loaded/list`;
- `thread/inject_items`: добавляет элементы в загруженный тред, не запуская хода;
- `turn/start`;
- **`turn/steer`**: дописывает ввод в активный ход и требует совпадения `expectedTurnId`;
- **`turn/interrupt`**: прерывает ход, и тот завершается со статусом `"interrupted"`;
- поток уведомлений `turn/started`, `turn/completed`, `item/agentMessage/delta`, `thread/status/changed` ([learn.chatgpt.com: Codex App Server](https://learn.chatgpt.com/docs/app-server)).

`turn/steer` появился ещё в rust-v0.99.0 в феврале 2026 ([rust-v0.99.0](https://github.com/openai/codex/releases/tag/rust-v0.99.0)). В бинарнике 0.162.0-alpha.17.2, который вшит в ChatGPT.app, есть и семейство `thread/queue/list|add|update|delete|reorder|start` с уведомлением `thread/queue/changed`, которого нет в публичной документации ([строки бинарника](file:///Applications/ChatGPT.app/Contents/Resources/codex-cli/CodexCLI.app/Contents/MacOS/codex)). Дело не в нехватке методов. Любой из них действует только внутри того app-server-процесса, который держит тред.

На вашем Mac этот процесс недосягаем. Локальная проверка 15 октября показала, что десктоп запускает app-server дочерним процессом `codex -c features.code_mode_host=true app-server --analytics-default-enabled -c plugins.codex-app-tools@openai-bundled… -c plugins.code-review@openai-bundled…` **без `--listen`**, то есть на транспорте по умолчанию `stdio://`. Каталога `~/.codex/app-server-control/` с сокетом общего демона нет вовсе ([локальная проверка](file:///Users/kaifat/.codex/)). Issue #52705 описывает ровно этот случай: `turn/steer` доступен только на app-server, который владеет тредом, а у Desktop это приватный stdio-ребёнок, к которому внешний процесс подключиться не может ([openai/codex#52705](https://github.com/openai/codex/issues/52705)). Просьбы о поддерживаемом пути открыты, и ни на одну не ответили мейнтейнеры:

- внешний `turn/steer` в ходы, которыми владеет Desktop на macOS ([#51625](https://github.com/openai/codex/issues/51625));
- режим `codex queue --steer` ([#48928](https://github.com/openai/codex/issues/48928));
- документация о том, как найти активный тред Desktop и подключиться к нему ([#25914](https://github.com/openai/codex/issues/25914)), открыта с 2 июня;
- подключение Desktop к внешнему app-server ([#40134](https://github.com/openai/codex/issues/40134)).

В #25914 пользователи уже перепробовали очевидное: `thread/loaded/list` возвращал ноль тредов, `turn/interrupt` отвечал «thread not found», а `turn/steer` на треде, открытом заново через `thread/resume`, отвечал «no active turn to steer».

Второй барьер — блокировка писателя. Начиная с PR #44138 (9 сентября 2026) Codex держит межпроцессную блокировку «один писатель на тред» ([PR #44138](https://github.com/openai/codex/pull/44138)). Поэтому `codex exec resume <id>` на треде, который загружен в Desktop, падает с ошибкой **`thread … already has an active writer (code -32600)`**. Desktop держит блокировку, даже когда тред простаивает ([chenhg5/cc-connect#1909](https://github.com/chenhg5/cc-connect/issues/1909)). Тред остаётся загруженным ещё 30 минут после ухода последнего подписчика ([App Server docs](https://learn.chatgpt.com/docs/app-server)). TUI в такой ситуации показывает тред только для чтения: клавиша `r` повторяет попытку, `f` делает редактируемую копию ([PR #43253](https://github.com/openai/codex/pull/43253), [PR #47185](https://github.com/openai/codex/pull/47185)). Даже если внешнему клиенту удаётся дописать ход в тред, который виден в Desktop, открытое окно треда не обновляется ([#21743](https://github.com/openai/codex/issues/21743)).

Два «очевидных» пути тоже отпадают. Remote Control действительно позволяет телефону ставить в очередь и направлять ходы в тредах на хосте: в iOS 1.2026.146 появился выбор Queue или Steer, в 1.2026.237 очередь синхронизируется с хостом ([changelog](https://learn.chatgpt.com/docs/changelog)). Но это работает через relay OpenAI с QR-сопряжением «одно устройство — один хост» ([Remote connections](https://learn.chatgpt.com/docs/remote-connections)), и локального API там нет. Локальной программе пришлось бы выдавать себя за сопряжённое устройство. **`codex mcp-server` больше не существует.** Его объявили устаревшим 24 августа и удалили в rust-v0.154.0 (#42993) ([rust-v0.154.0](https://github.com/openai/codex/releases/tag/rust-v0.154.0), [changelog](https://learn.chatgpt.com/docs/changelog)). В 0.162 строка `codex mcp-server` разбирается как обычный промпт. Значит, не работает любой гайд с `claude mcp add codex -- codex mcp-server`, и зависимые интеграции уже сломались ([KjellKod/quest#176](https://github.com/KjellKod/quest/issues/176), [claude-delegator#14](https://github.com/jarrodwatts/claude-delegator/issues/14)).

## `codex queue` доходит до живого треда, но только между ходами

Единственный официальный инструмент, который без перенастройки доставляет текст в тред Codex desktop, — `codex queue`. Он появился в 0.149.0 20 августа ([rust-v0.149.0](https://github.com/openai/codex/releases/tag/rust-v0.149.0)). Команда отправляет сообщение через `thread/queue/add` и находит тред по UUID или по точному имени среди интерактивных, exec- и прочих источников, а неоднозначное имя отвергает ([PR #39092](https://github.com/openai/codex/pull/39092)). Сообщение из другого процесса доходит благодаря PR #39034: очереди хранятся в SQLite, Codex следит за изменениями базы, будит простаивающие треды с внешними сообщениями и подхватывает очередь при загрузке треда ([PR #39034](https://github.com/openai/codex/pull/39034)). На вашем Mac `~/.codex/queue_1.sqlite` существует, и в него писали утром 15 октября ([локальная проверка](file:///Users/kaifat/.codex/)).

Семантика у команды именно follow-up, а не steer, и в этом её главное ограничение. Два независимых отчёта (Windows с CLI 0.160.1 и macOS с CLI 0.160.1, вшитым в приложение) описывают одно и то же. **Desktop разбирает очередь только когда тред простаивает.** Во время активного хода сообщение ждёт. Если человек нажмёт в интерфейсе **Steer**, сообщение вливается в текущий ход. Под активным `/goal` элемент очереди висит, пока Steer не нажмут вручную ([#52705](https://github.com/openai/codex/issues/52705), [#51625](https://github.com/openai/codex/issues/51625)). Ещё одна ловушка: **`codex queue` возвращает 0, как только элемент сохранён**, и отправитель не отличит «сохранено» от «взято в работу» ([#52705](https://github.com/openai/codex/issues/52705)). Для выгруженного треда сообщение лежит в очереди, пока тред снова не откроют. Мейнтейнеры сознательно сохраняют такой контракт и закрыли #44491 как «not planned» ([#44491](https://github.com/openai/codex/issues/44491)). Настройка `followUpQueueMode = "queue"` в строке 379 вашего `config.toml` здесь ни при чём. Она влияет только на то, что вы набираете в поле ввода самого приложения ([#48928](https://github.com/openai/codex/issues/48928)). По умолчанию в Desktop она равна `steer` ([app.asar](file:///Applications/ChatGPT.app/Contents/Resources/app.asar)).

Для долгих тредов вывод такой: внешняя поправка приходит не раньше конца текущего хода. Отсюда гибридный приём, который сокращает computer use до одного клика. Claude ставит сообщение командой `codex queue`, а если ждать нельзя, кликает **Steer** на элементе очереди в окне ChatGPT.app. Набирать текст в окне ему уже не нужно. Это вывод из описанного в #51625 поведения, на вашей версии он не проверялся.

Читать прогресс можно всегда, но только из истории на диске. Codex хранит треды в `~/.codex/sessions` ([TS SDK README](https://github.com/openai/codex/blob/main/sdk/typescript/README.md)). `thread/read`, запущенный из любого отдельного app-server, возвращает сохранённые данные, не открывая тред заново и не трогая блокировку ([App Server docs](https://learn.chatgpt.com/docs/app-server)). Claude проще всего держать `tail -F` на rollout-файле активного треда под инструментом `Monitor`: тот запускает команду в фоне и отдаёт Claude каждую строку вывода ([Claude Code: tools reference](https://code.claude.com/docs/en/tools-reference)). Оговорка: формат rollout внутренний, и примерно с 0.153 в Codex есть компрессор rollout-файлов ([rust-v0.153.0](https://github.com/openai/codex/releases/tag/rust-v0.153.0)). Так что слежение за файлом может сломаться при обновлении.

## Плагины, MCP-серверы и оркестраторы запускают свой Codex, а не подключаются к вашему

Ниже сводка всех найденных способов в направлении Claude → Codex с оценкой того, что каждый может сделать с тредом, живущим в Codex desktop при штатной конфигурации.

| Способ | Механизм | Тред Codex desktop: читать / в очередь / steer, interrupt | Свои треды | Состояние на октябрь 2026 |
|---|---|---|---|---|
| Computer use | клики и ввод в окне ChatGPT.app | да / да / да | — | работает, медленно и хрупко |
| `codex queue --thread … --message …` | `thread/queue/add` → `queue_1.sqlite` | нет / **да, когда тред простаивает** / нет | да | stable с 0.149.0 |
| rollout-файлы, `thread/read` | история на диске | **да, не вживую** / нет / нет | да | формат внутренний |
| `codex exec resume <id>`, TS SDK `resumeThread` | новый процесс `exec` | нет / нет (`active writer`) / нет | да | только для отпущенных тредов |
| Общий демон + `CODEX_APP_SERVER_WS_URL` | Desktop и CLI на одном app-server | да, вживую / да / **да** | да | неофициальный обход |
| [openai/codex-plugin-cc](https://github.com/openai/codex-plugin-cc) | своя дочерняя `codex app-server` через брокер | нет / нет / нет | свои «Codex Companion Task» | v1.0.6, около 34.2k★, без релизов с июля |
| Python SDK `openai-codex` | JSON-RPC к своему app-server | нет / нет / нет | `steer()`, `interrupt()`, `ExternalMessage` | официальный |
| tuannvm/codex-mcp-server, PAL `clink`, abetoots/codex-app-mcp, mkXultra/ai-cli-mcp | обёртки над `codex exec` или app-server | нет / нет / нет | новые сессии, частично продолжение | на 0.16x не проверены; PAL заброшен с декабря 2025 |
| alanwilhelm/ctrl | WebSocket на сокете демона | только треды демона | `list`, `read`, `spawn`, `send` | только Linux, 0.146.0, 0★ |
| AgentBridge (raysonmeng) | Channels + прокси app-server | нет (свой TUI) | `turn/start` | 391★, по умолчанию `--yolo` |
| C-C-chat-bridge | внутренние интерфейсы Desktop | заявлено | — | только Windows, 0★ |
| Conductor, claude-squad, ntm, agent-deck, vibe-kanban | запуск агентов в worktree или tmux | нет | да | чужие треды не подхватывают |
| ACP (`codex-acp`) и A2A (a2abridge, a2a-adapter) | клиент запускает агента сам | нет | да | молодые адаптеры |
| tmux `send-keys` | ввод в TUI | только TUI | да | без подтверждения доставки |

Официальный плагин заслуживает отдельного разбора, потому что его рекомендует сама OpenAI. Когда `codex mcp-server` объявили устаревшим, OpenAI предложила пользоваться из Claude Code именно этим плагином ([Codex Releases на X](https://x.com/CodexReleases/status/2091929016194289782)). Установка: `/plugin marketplace add openai/codex-plugin-cc`, `/plugin install codex@openai-codex`, `/codex:setup` ([The Decoder](https://the-decoder.com/openai-launches-a-codex-plugin-that-runs-inside-anthropics-claude-code/)). Команды:

- `/codex:review` и `/codex:adversarial-review` делают ревью;
- `/codex:rescue --background` передаёт задачу субагенту;
- `/codex:status`, `/codex:result` и `/codex:cancel` управляют фоновыми задачами;
- `/codex:transfer` превращает текущую сессию Claude в тред Codex, который потом можно открыть в приложении ([README](https://github.com/openai/codex-plugin-cc)).

По исходникам видно, что плагин запускает **собственный** `codex app-server` через stdio ([app-server.mjs](https://github.com/openai/codex-plugin-cc/blob/main/plugins/codex/scripts/lib/app-server.mjs)). `--resume` берёт только последний тред с источником `appServer` и именем, начинающимся на «Codex Companion Task»; произвольный id указать нельзя ([codex.mjs](https://github.com/openai/codex-plugin-cc/blob/main/plugins/codex/scripts/lib/codex.mjs)). У плагина есть и эксплуатационные проблемы, важные для вашей конфигурации с множеством десктопных сессий Claude. Все app-server разделяют один `~/.codex`, хотя он не рассчитан на нескольких писателей сразу ([#382](https://github.com/openai/codex-plugin-cc/issues/382)). Долгоживущие брокеры сжигают одноразовый refresh-токен ChatGPT (`refresh_token_reused`) и разлогинивают всех клиентов `~/.codex` ([#789](https://github.com/openai/codex-plugin-cc/issues/789)). Брокер не отписывается от тредов ([#706](https://github.com/openai/codex-plugin-cc/issues/706)) и не завершается вместе с сессией ([#108](https://github.com/openai/codex-plugin-cc/issues/108)). В одном отчёте **14 брокеров и 14 app-server жили больше трёх суток**, потому что десктопные сессии почти не закрываются ([StoryOS#1093](https://github.com/FrankQDWang/StoryOS/issues/1093)). Режим review gate, хук, который ревьюит каждый ответ Claude, по предупреждению самого README умеет зациклиться и быстро съесть лимиты ([README](https://github.com/openai/codex-plugin-cc)).

У остальных инструментов та же картина. Python SDK действительно умеет `TurnHandle.steer()` и `interrupt()`, а `ExternalMessage` вливается в активный ход. Но это клиент к app-server, который SDK запустил сам ([Python API reference](https://github.com/openai/codex/blob/main/sdk/python/docs/api-reference.md), [client.py](https://github.com/openai/codex/blob/main/sdk/python/src/openai_codex/client.py)). TypeScript SDK — обёртка над `codex exec --experimental-json`, и у него нет steer ([exec.ts](https://github.com/openai/codex/blob/main/sdk/typescript/src/exec.ts)). PAL `clink` запускает Codex с `--dangerously-bypass-approvals-and-sandbox` ([clink docs](https://github.com/BeehiveInnovations/pal-mcp-server/blob/main/docs/tools/clink.md)). abetoots/codex-app-mcp возвращает инструменты `codex` и `codex-reply(threadId)` поверх app-server, но проверен только на 0.154.0 и не имеет звёзд ([codex-app-mcp](https://github.com/abetoots/codex-app-mcp)). AgentBridge позиционируется как «живой» двусторонний мост, но сторона Codex у него — запущенный им же TUI с `--remote`, а по умолчанию он пропускает разрешения ([raysonmeng/agent-bridge](https://github.com/raysonmeng/agent-bridge)). Единственный проект, заявляющий доступ к чатам Codex Desktop, — C-C-chat-bridge. Он опирается на нераскрытые внутренние интерфейсы и работает только на Windows ([C-C-chat-bridge](https://github.com/Alex123666tw/C-C-chat-bridge)). У самого Codex desktop есть инструменты связи между тредами (`send_message_to_thread`, `create_thread`, `list_threads` в пространстве `codex_app`), но они доступны только агентам внутри Codex и не видны снаружи ([app.asar](file:///Applications/ChatGPT.app/Contents/Resources/app.asar)).

Эти инструменты хороши для другой задачи: когда Claude сам ставит Codex новую работу. В этом случае подходят `/codex:rescue --background` с `/codex:status` и `/codex:result` или фоновый `codex exec --json` с последующим `codex exec resume <id>`. Цикл ревью плана через `exec resume` обычно сходится за 2–3 раунда ([Aseem Shrey](https://aseemshrey.com/blog/claude-codex-iterative-plan-review/)). Такие треды появляются в боковой панели Desktop, по описанию в #40134, только после перезапуска приложения ([#40134](https://github.com/openai/codex/issues/40134)). Для автоматизации, которой не нужно быть видимой в приложении, отдельный `CODEX_HOME` со своей копией `auth.json` убирает и конфликт писателей, и гонку за токен ([cc-connect#1909](https://github.com/chenhg5/cc-connect/issues/1909)). Длинный путь `CODEX_HOME` на macOS может упереться в лимит длины пути Unix-сокета ([multica#4041](https://github.com/multica-ai/multica/issues/4041)).

## Обратный канал готов: хук Codex будит сессию Claude через файл или inbox-сокет

В направлении Codex → Claude картина зеркальная, и она лучше. Межсессионные сообщения Claude Code (cross-session messaging) общедоступны начиная с v2.1.224 и включены по умолчанию. Если получатель занят, он читает сообщение между вызовами инструментов. Если простаивает, **Claude Code начинает с этим сообщением новый ход**. Подтвердить запрос разрешения или поменять конфигурацию через такое сообщение нельзя ([Cross-session messaging](https://code.claude.com/docs/en/cross-session-messaging)). Каждая сессия открывает свой inbox-сокет. Его путь и токен сессии экспортируются в хуки и Bash как `CLAUDE_CODE_MESSAGING_SOCKET` и `CLAUDE_CODE_MESSAGING_TOKEN`, и документация прямо называет сценарий «скрипт или хук пишет в сессию» ([Cross-session messaging](https://code.claude.com/docs/en/cross-session-messaging), [env vars](https://code.claude.com/docs/en/env-vars)).

Локальная проверка на Claude Code 2.1.296 показала, что десктопные сессии тоже открывают сокет `/tmp/cc-socks/<pid>.sock`. Записи `~/.claude/sessions/<pid>.json` содержат `name`, `cwd`, `status` и `messagingSocketPath`, так что сторонний процесс может найти супервизора по имени ([локальная проверка, Cross-session messaging](https://code.claude.com/docs/en/cross-session-messaging)). Сам формат сообщений не документирован. Подсказка в бинарнике показывает построчный JSON: сначала строка авторизации `{"type":"auth","token":…}`, затем `{"type":"user","message":{"role":"user","content":"…"}}`. Это внутренний интерфейс, который может меняться от версии к версии.

Для вас критичен один параметр приёма. **Десктопная сессия не умеет показывать диалог подтверждения**. Задержанные сообщения истекают через 5 минут, а получатель в режиме bypass по умолчанию задерживает сообщения от посторонних процессов. Поэтому в проекте супервизора нужен явный `"crossSessionInbound": "accept"` ([Cross-session messaging](https://code.claude.com/docs/en/cross-session-messaging)).

Со стороны Codex есть два спусковых крючка. Устаревший, но работающий `notify` вызывается только на `agent-turn-complete`. Он получает одним аргументом JSON с полями `type`, `thread-id`, `turn-id`, `cwd`, `input-messages` и `last-assistant-message` ([Codex advanced config](https://learn.chatgpt.com/docs/config-file/config-advanced)). Ключ должен стоять на верхнем уровне, до первого заголовка таблицы `[...]` ([Daniel Vaughan](https://codex.danielvaughan.com/2026/04/13/codex-cli-notification-pipeline-osc9-hooks-alerts/)). В 2026 году в payload добавили поле `client`, где лежит `clientInfo.name` клиента app-server. Это намекает, что `notify` срабатывает и для ходов десктопа, но источник — зеркало репозитория, а не официальный сайт ([зеркало docs/config.md](https://forge.lthn.ai/core/core-agent-ide/src/commit/1afbbc11c3e2452cd61a4b102eb96b60e45f2de0/docs/config.md)). Второй вариант — хуки. Они стабильны и включены по умолчанию. `Stop` получает `last_assistant_message` и `turn_id`, а пользовательские хуки нужно один раз одобрить в `/hooks` ([Codex hooks](https://learn.chatgpt.com/docs/hooks)). **Ни один первичный источник не подтверждает, что ChatGPT.app исполняет `notify` и `hooks.json` так же, как CLI.** Поэтому это первое, что нужно проверить.

Готовое решение — [vshuraeff/codex-claude-notify](https://github.com/vshuraeff/codex-claude-notify): `Stop`-хук или `notify` отправляет финальный ответ Codex в inbox-сокет. Но оно рассчитано на Codex, запущенный из Bash сессии Claude и унаследовавший её переменные окружения. Без них, как в случае с ChatGPT.app, хук ничего не отправляет. Нужна доработка: искать сокет супервизора в файле. Есть и вариант через MCP. В 2.1.296 команда `claude mcp serve` хотя и запускает каждый раз новый процесс Claude ([Claude Code: MCP](https://code.claude.com/docs/en/mcp)), но отдаёт среди своих 27 инструментов `ListAgents` и `SendMessage`. Локальный запрос `ListAgents` из такого процесса вернул 24 живые сессии, включая десктопные. Доставку не проверяли, чтобы не писать в ваши рабочие сессии. Если подключить `claude mcp serve` к Codex с `enabled_tools = ["ListAgents","SendMessage"]` ([Codex config reference](https://learn.chatgpt.com/docs/config-file/config-reference)) и добавить правило в AGENTS.md, Codex сможет сообщать о промежуточных вехах и задавать вопросы посреди хода, а не только в конце. Ответ Claude при этом обратно в Codex не попадёт.

| Способ | Доходит до живой десктопной сессии Claude | Что документировано | Главный риск |
|---|---|---|---|
| `notify` Codex → строка в файл → `Monitor` в Claude | да, Claude просыпается на каждой строке | обе стороны | работает ли `notify` в ChatGPT.app |
| `notify` или `Stop` → inbox-сокет супервизора | да, push | сокет и переменные документированы, формат сообщений нет | изменения формата, путь сокета меняется при перезапуске |
| `claude mcp serve` → `SendMessage` | да, судя по `ListAgents`; доставка не проверена | команда официальная, такое применение не описано | нет обратного пути, лишний процесс Claude |
| Channels | только CLI, запущенный с `--channels` или dev-флагом | research preview | Desktop не передаёт флаги |
| Mods (`$.clock.every` + `$.prompt.submit`) | в сессиях с модом | API документирован | опрос по таймеру, поддержка Desktop не описана |
| Codex-плагины, вызывающие Claude | нет, каждый раз новая сессия Claude | — | лишний расход, супервизор ничего не видит |
| MCP Agent Mail, hcom | нет push / только агенты, запущенные через hcom | — | Agent Mail занимает порт 8765 |

Channels, единственный официальный push в живую сессию через MCP, для десктопного супервизора не годятся. Свои каналы требуют `--dangerously-load-development-channels`, этот флаг игнорируется в `-p` и Agent SDK, а сессии Desktop запускаются без пользовательских флагов ([Channels](https://code.claude.com/docs/en/channels), [Channels reference](https://code.claude.com/docs/en/channels-reference)). Mods умеют опрашивать по таймеру и запускать ход через `$.prompt.submit` ([Mods API](https://code.claude.com/docs/en/plugins/mods/api)), но про Desktop документация молчит. Показателен опыт codex-claude-bridge: в v0.3.0 автор отказался от каналов и MCP в пользу общего файла `talkto.md`, за которым Claude следит через `Monitor` ([abhishekgahlot2/codex-claude-bridge](https://github.com/abhishekgahlot2/codex-claude-bridge)). Codex-плагины, которые зовут Claude, каждый раз запускают новый Claude через `claude -p`, `claude --bg` или Agent SDK. Это [yanchuk/claude-plugin-codex](https://github.com/yanchuk/claude-plugin-codex) (Alpha, 22★), [@leo000001/claude-code-mcp](https://github.com/xihuai18/claude-code-mcp) и архивный [@steipete/claude-code-mcp](https://github.com/steipete/claude-code-mcp). Они дают второе мнение, но не докладывают супервизору. MCP Agent Mail работает только по опросу через `fetch_inbox` и по умолчанию слушает порт 8765, который у вас занят дашбордом ([mcp_agent_mail](https://github.com/Dicklesworthstone/mcp_agent_mail)). hcom доставляет сообщения только агентам, запущенным через `hcom` ([aannoo/hcom](https://github.com/aannoo/hcom)).

## Общий демон открывает `turn/steer` и `turn/interrupt` ценой недокументированного запуска

Начиная с 0.131.0 Codex поставляет управляемый демон app-server на сокете `$CODEX_HOME/app-server-control/app-server-control.sock`. К нему подключаются CLI-клиенты: `codex --remote unix://`, `codex queue --remote`, `codex agents`, `codex app-server proxy` ([daemon README](https://github.com/openai/codex/blob/main/codex-rs/app-server-daemon/README.md), [PR #20718](https://github.com/openai/codex/pull/20718)). Начиная с 0.157.0 TUI поднимает демон автоматически ([PR #47179](https://github.com/openai/codex/pull/47179)), а в 0.162.0 на него переехал `codex remote-control` ([rust-v0.162.0](https://github.com/openai/codex/releases/tag/rust-v0.162.0)). Десктоп по умолчанию демоном не пользуется, но в его коде есть два переключателя ([app.asar](file:///Applications/ChatGPT.app/Contents/Resources/app.asar)). Первый, `CODEX_APP_SERVER_USE_LOCAL_DAEMON=1`, срабатывает только без `-c`-переопределений. Обычный запуск десктопа их передаёт, так что, по всей видимости, этот переключатель на штатной установке не включается (вывод, не проверено). Второй, `CODEX_APP_SERVER_WS_URL`, использован в community-гисте. Его проверили 6 октября на macOS 27.0.1 с ChatGPT desktop 26.930.61225 и повторно 13 октября на Linux с вшитым 0.162.0-alpha.17.2. После запуска демона и открытия приложения с этой переменной у десктопа пропадает собственный дочерний app-server, а `codex --remote unix:// resume <THREAD_ID>` подключает TUI к тому же чату ([ale-rinaldi gist](https://gist.github.com/ale-rinaldi/80f063b60177f1281e219ebd68587a4d)).

В такой конфигурации у Claude появляется именно тот канал, который нужен супервизору. Через `codex app-server proxy` (переходник stdio ↔ сокет) скрипт делает следующее:

1. `initialize` с `experimentalApi`;
2. `thread/resume` треда десктопа и становится вторым подписчиком;
3. получает живые уведомления `turn/started`, `item/agentMessage/delta` и `turn/completed`;
4. вмешивается в ход через `turn/steer` с актуальным `expectedTurnId` или прерывает его через `turn/interrupt` ([App Server docs](https://learn.chatgpt.com/docs/app-server)).

Подписку на `turn/completed` можно держать под `Monitor`, и тогда уведомления придут без всяких хуков. Это вывод из протокола. Автор гиста проверял только скриптовые запросы на чтение, скриптовый steer не тестировался.

Цена такого обхода высока. Это неофициальный путь, и OpenAI нигде не описывает, как направить десктоп на демон ([CLI reference](https://learn.chatgpt.com/docs/cli/reference)). Переменная обходит `-c`-переопределения, которые десктоп передаёт при запуске. Поэтому `codex_app`-инструменты, плагин code-review и `code_mode_host` нужно проверить заново и при необходимости прописать в `config.toml`. Автор гиста на Linux так и сделал. Переключение между интерфейсами не создаёт независимых параллельных ходов в одном чате. Демон сам обновился с 0.161.0 до 0.162.0 8 октября, после чего Remote оказался выключен ([gist](https://gist.github.com/ale-rinaldi/80f063b60177f1281e219ebd68587a4d)). README предупреждает ещё о двух вещах: `update` может прервать идущую работу, а все клиенты делят окружение того процесса, который запустил демон ([daemon README](https://github.com/openai/codex/blob/main/codex-rs/app-server-daemon/README.md)). Демон переживает закрытие окон и держит копию пакета размером около 374 МБ вместе с самообновлятором ([#48195](https://github.com/openai/codex/issues/48195)). Из наследования окружения следует практическое правило (вывод): **запускать демон из своего Terminal, а не из Bash сессии Claude**. Иначе он унаследует песочницу и переменные одной конкретной сессии Claude, включая её inbox-сокет.

Отдельный случай — треды, которые десктоп ведёт на SSH-хосте dev-ubuntu. Демон создавался прежде всего для Codex, запущенного по SSH ([PR #20718](https://github.com/openai/codex/pull/20718)), так что на самом хосте `codex queue` и `codex --remote unix://` ближе к поддерживаемому сценарию. Для вашего хоста это не проверено.

## Рабочая схема супервизора и порядок первых проверок

Рекомендуемая схема держится на трёх штатных каналах. Демон остаётся опцией. Claude **читает** rollout-файл треда по запросу или под `Monitor`. **Дописывает инструкции** через `codex queue`. **Узнаёт о конце хода** от `notify`, который пишет строку в журнал, а журнал под `Monitor` будит супервизора. Раз внешние поправки доходят только между ходами, саму работу стоит резать на ходы покороче, с контрольными точками в конце каждого. Для тредов, которые вы хотите направлять извне, лучше не включать `/goal`, иначе очередь будет ждать ручного Steer. Ниже порядок проверок. Первые четыре шага безопасны и обратимы.

**Шаг 1. Зафиксировать бинарник и найти тред.** Используйте CLI, вшитый в приложение, чтобы версия схемы очереди совпадала с той, что у десктопа: у Homebrew сейчас 0.162.1, а внутри приложения 0.162.0-alpha.17.2. Чтобы найти UUID треда, напишите в тестовый тред и посмотрите, какой rollout-файл изменился. UUID есть в имени файла, но проверьте это на своём диске.

```bash
CODEX=/Applications/ChatGPT.app/Contents/Resources/codex-cli/CodexCLI.app/Contents/MacOS/codex
"$CODEX" --version
find ~/.codex/sessions -type f -mmin -5
```

**Шаг 2. Проверить очередь на тестовом треде.** Сначала отправьте сообщение, когда тред простаивает: должен начаться новый ход. Потом во время длинного хода: сообщение должно дождаться его конца. Код 0 означает только то, что сообщение сохранено. Факт доставки смотрите в rollout-файле.

```bash
"$CODEX" queue --thread <UUID> --message "Проверка очереди: ответь одним словом ОК"
```

**Шаг 3. Проверить, вызывает ли десктоп `notify`.** Положите скрипт в `~/.codex/bin/turn-log.py` и добавьте ключ **первой строкой** `~/.codex/config.toml`, до любой таблицы, предварительно проверив, что `notify` там ещё нет. Пути указывайте абсолютные: приложения с графическим интерфейсом получают урезанный `PATH`. Затем полностью закройте ChatGPT.app (Cmd-Q) и откройте снова, чтобы его app-server перечитал конфиг. После любого хода в десктопе в журнале должна появиться строка, а поле `client` покажет, какой клиент её породил. Если строки нет, десктоп `notify` не исполняет, и уведомления придётся брать из `Monitor` на rollout-файле.

```toml
notify = ["/usr/bin/python3", "/Users/kaifat/.codex/bin/turn-log.py"]
```

```python
#!/usr/bin/env python3
import json, sys, time
from pathlib import Path
try:
    ev = json.loads(sys.argv[-1])
    rec = {"ts": time.strftime("%Y-%m-%d %H:%M:%S"), "type": ev.get("type"),
           "client": ev.get("client"), "thread": ev.get("thread-id"),
           "turn": ev.get("turn-id"), "cwd": ev.get("cwd"),
           "last": (ev.get("last-assistant-message") or "")[:2000]}
    with (Path.home() / ".codex" / "turn-events.jsonl").open("a") as f:
        f.write(json.dumps(rec, ensure_ascii=False) + "\n")
except Exception:
    pass
```

**Шаг 4. Подключить журнал к супервизору.** Попросите сессию-супервизор запустить `Monitor` с командой `tail -n 0 -F ~/.codex/turn-events.jsonl`. На каждое событие она должна прочитать хвост rollout-файла нужного треда и решить, нужен ли follow-up через `codex queue`. Каждое событие стоит супервизору одного хода, поэтому фильтруйте журнал по `thread`, если в десктопе идёт много тредов.

**Шаг 5 (по желанию). Push прямо в inbox-сокет, без `Monitor`.** Супервизор один раз записывает свой сокет и токен в файл. Делать это нужно при каждом перезапуске сессии, потому что путь к сокету меняется. В `.claude/settings.local.json` проекта супервизора нужно добавить `{"crossSessionInbound": "accept"}`. После этого в `turn-log.py` внутри блока `try` дописывается отправка. Формат строк не документирован, поэтому если сообщение не дошло, первым делом подозревайте его.

```bash
umask 077; printf '{"socket":"%s","token":"%s"}\n' "$CLAUDE_CODE_MESSAGING_SOCKET" "$CLAUDE_CODE_MESSAGING_TOKEN" > ~/.claude/codex-supervisor.json
```

```python
import socket
sup = json.loads((Path.home() / ".claude" / "codex-supervisor.json").read_text())
text = f"[Codex turn complete] thread={rec['thread']} cwd={rec['cwd']}\n{rec['last']}"
with socket.socket(socket.AF_UNIX) as s:
    s.connect(sup["socket"])
    s.sendall((json.dumps({"type": "auth", "token": sup["token"]}) + "\n" +
               json.dumps({"type": "user", "message": {"role": "user", "content": text}}) + "\n").encode())
    time.sleep(1)
```

**Шаг 6 (отдельно и только на тестовом треде). Общий демон.** Демон запускайте из своего Terminal. Схему параметров `turn/steer`, `turn/interrupt` и `thread/queue/*` для вашей версии берите из `generate-json-schema`, а не из памяти. Учтите особенность этой сборки: у подкоманд `daemon` и `proxy` справку показывает `-h`, а `--help` выводит корневую справку ([локальная проверка](file:///Applications/ChatGPT.app/Contents/Resources/codex-cli/CodexCLI.app/Contents/MacOS/codex)). Откат простой: закрыть приложение через Cmd-Q, выполнить `"$CODEX" app-server daemon stop` и открыть ChatGPT.app обычным способом.

```bash
"$CODEX" app-server daemon start && "$CODEX" app-server daemon version
# Cmd-Q в ChatGPT.app, затем:
open -a /Applications/ChatGPT.app --env "CODEX_APP_SERVER_WS_URL=ws+unix://localhost$HOME/.codex/app-server-control/app-server-control.sock:/rpc"
ps ax -o pid,command | grep '[a]pp-server'      # у десктопа не должно остаться своего stdio-ребёнка
"$CODEX" --remote unix:// resume <UUID>          # TUI на том же живом треде
"$CODEX" app-server generate-json-schema --out ~/codex-schema
```

### Риски, которые стоит держать в голове

Самый вероятный сбой — тихая потеря поправки. `codex queue` отвечает успехом сразу после сохранения сообщения, а под `/goal` или за долгим ходом оно может ждать часами. Поэтому супервизор должен сверять, что инструкция появилась в rollout-файле, а не доверять коду возврата ([#52705](https://github.com/openai/codex/issues/52705)). Второй по вероятности сбой — разлогин Codex. Каждый лишний app-server на общем `~/.codex` (брокеры плагина в нескольких десктопных сессиях Claude, `exec`-обёртки) повышает шанс гонки за одноразовый refresh-токен ([#789](https://github.com/openai/codex-plugin-cc/issues/789)). Официальный плагин стоит держать только там, где он действительно нужен, а фоновую автоматизацию увести в отдельный `CODEX_HOME`.

Третий риск — хрупкость недокументированного. Формат inbox-сокета, реестр `~/.claude/sessions/*.json`, `SendMessage` внутри `claude mcp serve` и `CODEX_APP_SERVER_WS_URL` держатся только на текущих версиях. ChatGPT.app и Claude Code обновляются часто, так что после каждого обновления нужен короткий повторный тест.

Наконец, безопасность. Текст, пришедший от Codex, — данные, а не команды пользователя. Межсессионное сообщение не может само подтвердить разрешение ([Cross-session messaging](https://code.claude.com/docs/en/cross-session-messaging)), но супервизор в режиме bypass способен выполнить то, о чём его попросил чужой вывод. Поэтому супервизору лучше работать с подтверждениями, а `crossSessionInbound: accept` ставить в настройки проекта, а не в пользовательские. Из той же логики не стоит брать мосты, которые по умолчанию отключают песочницу: AgentBridge с `--yolo`, PAL `clink` с `--dangerously-bypass-approvals-and-sandbox`, ctrl с `dangerFullAccess` ([ctrl](https://github.com/alanwilhelm/ctrl)).

## Заключение

Главный барьер не в протоколе, а в том, кто владеет тредом и как к этому владельцу подключиться. В бинарнике 0.162 уже есть `turn/steer`, `turn/interrupt`, `thread/inject_items` и полноценная очередь. В коде десктопа уже есть два переключателя на общий демон. TUI переехал на демон в 0.157, `remote-control` — в 0.162. Вероятно, рано или поздно на демон перейдёт и десктоп, и тогда сегодняшний неофициальный JSON-RPC-клиент станет штатным. Это прогноз, а не обещание OpenAI. Поэтому слой супервизора разумно строить уже сейчас на примитивах app-server: очередь сегодня, steer потом. Слой должен быть тонким, чтобы при обновлении меняли только транспорт.

Второй вывод касается формы работы, а не инструментов. Claude Code умеет принимать push в живую сессию, а Codex desktop принимает извне только очередь. Значит, правильная модель супервизии — «Codex сообщает о событиях, Claude правит на границах ходов». Долгий тред, разбитый на короткие ходы с контрольными точками, управляется через `codex queue` почти так же хорошо, как через steer. Монолитный ход под `/goal` извне остаётся неуправляемым при любом наборе плагинов, и для него computer use по-прежнему единственный способ, хотя теперь достаточно одного клика по Steer.

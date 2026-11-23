# Official OpenAI Codex mechanisms for external control of Codex sessions (state as of 2026-10-15)

Scope: what OpenAI itself ships for an external program (e.g. Claude Code) to read, message, queue into, steer or interrupt a Codex thread, especially a thread shown in the Codex desktop app (ChatGPT.app). Evidence = official docs (learn.chatgpt.com, which developers.openai.com/codex now 308-redirects to), openai/codex GitHub (release notes, PRs, READMEs, issues incl. maintainer replies), and read-only local inspection of the user's installs: ChatGPT.app bundling `codex-cli 0.162.0-alpha.17.2`, and Homebrew `codex-cli 0.162.1`. Nothing was started, stopped or sent.

Local snapshot (2026-10-15, user's Mac): the desktop app's app-server is a stdio child process: `codex -c features.code_mode_host=true app-server --analytics-default-enabled -c plugins.codex-app-tools@openai-bundled.mcp_servers.codex_app.enabled=true -c plugins.code-review@openai-bundled.mcp_servers.code-review.enabled=true`. There is no `--listen`, so it uses the default `stdio://`. `~/.codex/app-server-control/` does not exist, so no shared daemon is running. `~/.codex/app-server-daemon/` holds only `daemon.lock` and `app-server-updater.pid.lock`. `~/.codex/queue_1.sqlite` exists and is in active use (WAL modified 2026-10-15 10:26). `~/.codex/config.toml` contains `followUpQueueMode = "queue"` (line 379). — [local `ps`/`ls` inspection](file:///Users/kaifat/.codex/)

---

## Q1. The app-server protocol: which thread/turn methods exist, and where is it documented?

### Takeaway
`codex app-server` speaks a JSON-RPC 2.0 protocol (v2). It has full thread/turn control: `thread/start|resume|fork|read|list|loaded/list|unsubscribe|inject_items|queue/*`, `turn/start|steer|interrupt`, and a rich notification stream. Each method only acts on the app-server process that owns the thread, though. The canonical docs are learn.chatgpt.com/docs/app-server and the schema that `codex app-server generate-ts` / `generate-json-schema` produce for the exact installed version.

### Cited Findings
- **Where documented:** developers.openai.com/codex/app-server now 308-redirects to `https://learn.chatgpt.com/docs/app-server` ("Codex App Server"). The app-server powers rich clients such as the VS Code extension. The WebSocket transport is "experimental and unsupported". — [learn.chatgpt.com/docs/app-server](https://learn.chatgpt.com/docs/app-server)
- The `codex-rs/app-server/README.md` on `main` is now a set of feature notes (thread-list exclusions, shutdown errors, read state, attachments, and so on), not the full method reference. — [app-server README](https://github.com/openai/codex/blob/main/codex-rs/app-server/README.md)
- **Schema generation:** `codex app-server generate-ts --out ./schemas` and `codex app-server generate-json-schema --out ./schemas`. The output "matches the Codex version that generated it". Both are marked `[experimental]` in local `codex app-server -h`. There is also a hidden `generate-internal-json-schema`. — [learn.chatgpt.com/docs/app-server](https://learn.chatgpt.com/docs/app-server); [local CLI 0.162.0-alpha.17.2 help/strings](file:///Applications/ChatGPT.app/Contents/Resources/codex-cli/CodexCLI.app/Contents/MacOS/codex)
- **Handshake:** a client must send `initialize` first. Otherwise it gets `Not initialized`, and a second `initialize` gets `Already initialized`. Gated methods and fields need `initialize.capabilities.experimentalApi: true`; without it the error is "`<descriptor> requires experimentalApi capability`". `optOutNotificationMethods` filters notifications per connection. — [learn.chatgpt.com/docs/app-server](https://learn.chatgpt.com/docs/app-server)
- **Core thread methods (documented):**
  - `thread/start`: creates a thread, emits `thread/started` and auto-subscribes the calling connection.
  - `thread/resume`: reopens by id so later `turn/start` calls append to it.
  - `thread/fork`: copies history into a new id. `lastTurnId` is inclusive; an in-progress turn is rejected.
  - `thread/read`: returns stored metadata "without resuming or emitting `thread/started`".
  - `thread/list`: cursor-paginated; `sourceKinds` defaults to `cli` and `vscode`.
  - `thread/loaded/list`: returns the thread ids currently in memory.
  - `thread/inject_items`: "Appends raw Responses API items to a loaded thread without starting a turn."
  - `thread/unsubscribe`: returns `unsubscribed` | `notSubscribed` | `notLoaded`. After the last subscriber leaves, the thread unloads after a 30-minute grace period.
  - Also: `thread/archive|unarchive|delete|name/set|goal/set|get|clear|metadata/update|turns/list|items/list|compact/start|shellCommand|revert`. `thread/rollback` was removed.
  - — [learn.chatgpt.com/docs/app-server](https://learn.chatgpt.com/docs/app-server)
- **Turn methods (documented):**
  - `turn/start`: starts generation with `input` items.
  - `turn/steer`: "Appends input to the active turn". It requires `expectedTurnId` to match the active turn, fails if no turn is active, emits no `turn/started`, rejects `model`, `cwd`, `sandboxPolicy` and `outputSchema`, and returns `turnId`.
  - `turn/interrupt`: returns `{}`, and the turn ends with `turn/completed` status `"interrupted"`.
  - — [learn.chatgpt.com/docs/app-server](https://learn.chatgpt.com/docs/app-server)
  - Conflict: a later chunk of the same long page seemed to say there is no `turn/interrupt`. The first chunk and the shipped binary both contain `turn/interrupt`, so treat the method as present. — [local binary strings](file:///Applications/ChatGPT.app/Contents/Resources/codex-cli/CodexCLI.app/Contents/MacOS/codex)
- **Steer API history:** `turn/steer` was added in rust-v0.99.0 (2026-02-11, PR #10821). Steer mode became stable in the TUI in 0.98.0 (2026-02-05). An opt-in `instant_interrupt` "lets new input steer Codex during model responses or long-running code-mode calls" since 0.159.0 (2026-09-29). — [rust-v0.99.0](https://github.com/openai/codex/releases/tag/rust-v0.99.0); [rust-v0.98.0](https://github.com/openai/codex/releases/tag/rust-v0.98.0); [rust-v0.159.0](https://github.com/openai/codex/releases/tag/rust-v0.159.0)
- **`thread/unsubscribe`** was added via PR #10954 ("a `thread/unsubscribe` flow to unload live threads without archiving them"). — [openai/codex releases](https://github.com/openai/codex/releases)
- **Full request-method list compiled into 0.162.0-alpha.17.2 (binary strings):**
  - Thread lifecycle and data: `thread/start`, `thread/resume`, `thread/fork`, `thread/archive`, `thread/unarchive`, `thread/delete`, `thread/unsubscribe`, `thread/name/set`, `thread/goal/set|get|clear`, `thread/metadata/update`, `thread/attachment/add|list|remove`, `thread/attachmentOwner/list`, `thread/section/move`, `thread/compact/start`, `thread/shellCommand`, `thread/approveGuardianDeniedAction`, `thread/revert`, `thread/list`, `thread/loaded/list`, `thread/read`, `thread/turns/list`, `thread/items/list`, `thread/inject` (also `thread/inject_items`), `thread/search`, `thread/searchOccurrences`, `thread/settings/update`, `thread/memoryMode/set`, `thread/backgroundTerminals/clean|list|terminate`, `thread/prediction/request`.
  - Queue: **`thread/queue/list|add|update|delete|reorder|start`**.
  - Realtime: `thread/realtime/start|appendAudio|appendText|appendSpeech|stop|listVoices`, `thread/timeline/list`.
  - Turns and review: **`turn/start`, `turn/steer`, `turn/interrupt`**, `turn/settings/update`, `review/start`.
  - Remote control: `remoteControl/enable|disable|status/read|pairing/start|pairing/status|client/list|client/revoke`.
  - — [local binary strings](file:///Applications/ChatGPT.app/Contents/Resources/codex-cli/CodexCLI.app/Contents/MacOS/codex)
- **Notifications in the same binary:**
  - Thread: `thread/started`, `thread/status/changed`, `thread/archived|deleted|unarchived|closed|reverted`, `thread/name/updated`, `thread/goal/updated|cleared`, **`thread/queue/changed`**, `thread/settings/updated`, `thread/tokenUsage/updated`.
  - Turn: `turn/started`, `turn/completed`, `turn/diff/updated`, `turn/plan/updated`.
  - Items: `item/started`, `item/completed`, `item/agentMessage/delta`, `item/reasoning/*`, `item/commandExecution/outputDelta`.
  - Other: `serverRequest/resolved`, `remoteControl/status/changed`.
  - Server→client requests (approvals): `item/commandExecution/requestApproval`, `item/fileChange/requestApproval`, `item/tool/requestUserInput`, `item/permissions/requestApproval`, `item/tool/call`, `mcpServer/elicitation/request`.
  - Thread status values in the schema: `notLoaded`, `idle`, `systemError`, `active` with `activeFlags` such as `waitingOnApproval`, `waitingOnUserInput`, `paused`, `blocked`, `usageLimited`, `budgetLimited`.
  - — [local binary strings](file:///Applications/ChatGPT.app/Contents/Resources/codex-cli/CodexCLI.app/Contents/MacOS/codex)
- **Thread ownership errors (binary strings):** "thread … is already running", "cannot resume thread … with history while it is already running", "thread/resume overrides ignored for loaded thread", and the cross-process "**thread … already has an active writer**" / "failed to acquire thread writer lock". — [local binary strings](file:///Applications/ChatGPT.app/Contents/Resources/codex-cli/CodexCLI.app/Contents/MacOS/codex); see Q4.
- **Transports:** `--listen stdio://` (default, newline-delimited JSON), `unix://` (the default control socket) or `unix://PATH`, `ws://IP:PORT` (experimental), and `off`. WebSocket auth: `--ws-auth capability-token` (with `--ws-token-file` or `--ws-token-sha256`) or `--ws-auth signed-bearer-token` (with `--ws-shared-secret-file`, plus optional `--ws-issuer`, `--ws-audience`, `--ws-max-clock-skew-seconds`). Clients send `Authorization: Bearer`. Non-loopback WebSocket listeners allow unauthenticated connections by default during rollout. Health probes: `GET /readyz` and `/healthz`. — [learn.chatgpt.com/docs/app-server](https://learn.chatgpt.com/docs/app-server); [local `codex app-server -h`](file:///Applications/ChatGPT.app/Contents/Resources/codex-cli/CodexCLI.app/Contents/MacOS/codex)
- **Desktop-only client methods:** the desktop app's renderer bundle also uses `turn/addUserMessage`, `thread/startAeon` and `thread/stop`. Its client rewrites `thread/queue/add` into `turn/addUserMessage` with `mode: "queue"`. — [ChatGPT.app app.asar (read-only grep)](file:///Applications/ChatGPT.app/Contents/Resources/app.asar)

### Inferences
- The protocol fully covers "read, queue, steer, interrupt". The hard part is the connection: the client must reach the app-server process that has the thread loaded (see Q2).
- `turn/steer` needs the active `turnId`. An external steerer must subscribe (e.g. `thread/resume` on the owning server) or read `thread/read` / `thread/turns/list` to learn it. A stale id is rejected rather than silently queued.

### Gaps
- I did not run `generate-json-schema` (instructed read-only). The exact param shapes of `thread/queue/*` and `thread/inject_items` were not extracted, and the public docs page does not document `thread/queue/*` at all.
- Whether `turn/addUserMessage` / `thread/startAeon` are app-server methods or desktop-side extensions was not established.

---

## Q2. Shared local app-server daemon, control socket, proxy, `codex agents`, `codex queue`, `codex --remote`: and can the desktop app use the daemon?

### Takeaway
Since 0.131.0 (2026-05-18) Codex ships a managed app-server daemon on a Unix control socket, `$CODEX_HOME/app-server-control/app-server-control.sock`. CLI clients (`codex`, `codex agents`, `codex queue`, `codex --remote unix://`, `codex app-server proxy`) attach to it, and since 0.157.0 (2026-09-25) the TUI auto-starts it by default. The desktop app does **not** use it by default: it runs its own stdio app-server child. The desktop bundle contains two undocumented env-var switches that would make it use the daemon socket: `CODEX_APP_SERVER_USE_LOCAL_DAEMON=1` and `CODEX_APP_SERVER_WS_URL`. There is no UI setting for this.

Even without a shared daemon, `codex queue` does reach a desktop-shown thread through the shared SQLite queue (`~/.codex/queue_1.sqlite`). Delivery happens only when that thread is idle (a follow-up, not a steer). Live `turn/steer` / `turn/interrupt` from outside is impossible while the desktop owns the thread over private stdio; this is an open feature request (#51625, #48928).

### Cited Findings
**Daemon lifecycle (official):**
- `codex app-server daemon` subcommands (local `-h`, 0.162.0-alpha.17.2):
  - `bootstrap` ("Install durable local app-server management for SSH-driven use")
  - `start` ("Start the local app server daemon if it is not already running")
  - `restart`
  - `update` ("may interrupt running work")
  - `enable-remote-control`, `disable-remote-control` ("for future starts and a currently running managed daemon")
  - `stop`
  - `version` ("Print local CLI and running app-server versions as JSON")
  - Hidden: `pid-update-loop`.
  - — [local CLI help](file:///Applications/ChatGPT.app/Contents/Resources/codex-cli/CodexCLI.app/Contents/MacOS/codex)
- The daemon README (marked experimental, lifecycle contract may change):
  - It "backs the machine-readable `codex app-server` lifecycle commands used by remote clients such as the desktop and mobile apps. It is intended for Codex instances launched over SSH".
  - Every command prints exactly one JSON object.
  - `start` "is idempotent and returns after app-server is ready to answer the normal JSON-RPC initialize handshake on the Unix control socket".
  - `enable-/disable-remote-control` persist the setting and restart a running daemon.
  - Settings live in `CODEX_HOME/app-server-daemon/settings.json`, e.g. `{"remoteControlEnabled":false,"shutdownGraceSeconds":60,"updater":{"autoUpdateEnabled":false,"updateIntervalMinutes":120}}`.
  - State files: `settings.json`, `app-server.pid`, `app-server-updater.pid`, `daemon.lock`.
  - Packages live in `CODEX_HOME/packages/app-server-daemon/current/bin/codex`.
  - Updates are checked 5 min after start, then hourly.
  - "Shared clients use the environment inherited when the daemon started… per-client environment isolation is not provided."
  - If an implicitly discovered daemon can't initialize, the TUI falls back to an embedded server. Explicit `--remote` endpoints "remain authoritative".
  - Bootstrap on a new machine: `curl -fsSL https://chatgpt.com/codex/install.sh | sh` then `$HOME/.codex/packages/standalone/current/codex app-server daemon bootstrap --remote-control`.
  - — [app-server-daemon README](https://github.com/openai/codex/blob/main/codex-rs/app-server-daemon/README.md)
- **Origin:** PR #20718 "[daemon] Add app-server daemon lifecycle management" (merged 2026-05-08, shipped in rust-v0.131.0 on 2026-05-18). Its rationale: "Desktop and mobile Codex clients need a machine-readable way to bootstrap and manage `codex app-server` on remote machines reached over SSH". — [PR #20718](https://github.com/openai/codex/pull/20718); [rust-v0.131.0](https://github.com/openai/codex/releases/tag/rust-v0.131.0)
- **Timeline (release notes):**
  - 0.131.0 (2026-05-18): UDS support in `codex --remote` (#22414).
  - 0.149.0 (2026-08-20): "Added `codex queue` for sending messages to existing local or remote sessions" (#39092) and the `codex agents` dashboard (#39114).
  - 0.154.0 (2026-09-09): Windows daemon support.
  - 0.155.0 (2026-09-17): `codex app-server daemon update`; saved threads and active goals recover after daemon restart (#44314).
  - 0.156.0 (2026-09-22): `/daemon` menu (#45854) and `--no-daemon` (#46088).
  - 0.157.0 (2026-09-25): "Enable automatic daemon startup by default" (#47179). It promotes `daemon_auto_start` to stable and on by default "for eligible interactive launches".
  - 0.162.0 (2026-10-08): plain `codex remote-control` uses the managed daemon (#50803).
  - — [rust-v0.149.0](https://github.com/openai/codex/releases/tag/rust-v0.149.0); [rust-v0.156.0](https://github.com/openai/codex/releases/tag/rust-v0.156.0); [rust-v0.157.0](https://github.com/openai/codex/releases/tag/rust-v0.157.0); [PR #47179](https://github.com/openai/codex/pull/47179); [rust-v0.162.0](https://github.com/openai/codex/releases/tag/rust-v0.162.0)
- **Opt-out:** `codex features disable daemon_auto_start`, or `daemon_auto_start = false` in config. Issue #48195 (open, opened 2026-09-25) asks for it to be opt-in and reports these mechanics: the daemon runs as `codex app-server --managed-daemon`, persists after windows close, and keeps a ~374 MB package copy plus a `pid-update-loop` self-updater. No maintainer reply. — [issue #48195](https://github.com/openai/codex/issues/48195)
- **Socket path:** `$CODEX_HOME/app-server-control/app-server-control.sock`, with `app-server-startup.lock` and `loaded-threads.json` under `app-server-daemon`. The listener refuses unsafe parents ("socket parent must be owned by the user or root and prevent other users from replacing socket entries"). JSON-RPC runs as WebSocket frames over the UDS at path `/rpc`; the desktop code uses `websocketUrl: ws://localhost/rpc` with `createConnection` on the socket. — [local binary strings](file:///Applications/ChatGPT.app/Contents/Resources/codex-cli/CodexCLI.app/Contents/MacOS/codex); [ChatGPT.app app.asar](file:///Applications/ChatGPT.app/Contents/Resources/app.asar)
- **`codex app-server proxy [--sock <SOCKET_PATH>]`:** "Proxy stdio bytes to the running app-server control socket". In effect it turns the daemon into a stdio JSON-RPC peer for any client that can only spawn a process. — [local `codex app-server proxy -h`](file:///Applications/ChatGPT.app/Contents/Resources/codex-cli/CodexCLI.app/Contents/MacOS/codex)
- **`codex --remote <ADDR>`:** accepted forms are `ws://host:port`, `wss://host:port`, `unix://` (default socket) or `unix://PATH`, plus `--remote-auth-token-env <ENV_VAR>`. `--no-daemon` "can't be combined with `--remote` or `codex agents`". — [CLI reference](https://learn.chatgpt.com/docs/cli/reference); [local help](file:///Applications/ChatGPT.app/Contents/Resources/codex-cli/CodexCLI.app/Contents/MacOS/codex)
- **`codex agents`** (stable): "Open the agent command center on a shared local or remote server". It takes `--remote <URL>` and `--cd <DIR>` for new remote tasks. Binary error text: "The agents overview requires a shared server. Use codex --no-daemon to work without it." — [CLI reference](https://learn.chatgpt.com/docs/cli/reference); [local binary strings](file:///Applications/ChatGPT.app/Contents/Resources/codex-cli/CodexCLI.app/Contents/MacOS/codex)
- **`codex queue --thread <UUID|exact name> --message <TEXT>`** (stable):
  - It also takes `--remote` and `--remote-auth-token-env`. Image attachments are not supported, despite `-i` appearing in local help.
  - PR #39092: it submits "through the `thread/queue/add` app-server API". It resolves sessions "by UUID or exact name across interactive, exec, and custom sources", rejects ambiguous names, and supports "local and explicit remote app servers… local-daemon routing".
  - — [CLI reference](https://learn.chatgpt.com/docs/cli/reference); [PR #39092](https://github.com/openai/codex/pull/39092)
- **Cross-process delivery without a daemon:** PR #39034 "Dispatch queued messages written by other processes" (merged 2026-08-17): "Durable thread queues can be updated through another SQLite connection". Codex polls SQLite's data version, "wake[s] idle threads with pending external messages, discover[s] queued work when threads are loaded or resumed". — [PR #39034](https://github.com/openai/codex/pull/39034)
- **`codex queue` into a desktop-shown thread, observed:**
  - Issue #52705 (Windows Codex App 26.1002, CLI 0.160.1, opened 2026-10-09): `codex queue` maps to `thread/queue/add` and the item is stored in `~/.codex/queue_1.sqlite`. "The Desktop drains the queue (`thread/queue/start`) only while the thread is idle". It works in an ordinary thread, but under an active `/goal` the item waits for a manual **Steer**. "`codex queue` exits 0 once the item is stored, so the sender cannot tell 'stored' from 'taken'." `turn/steer` "exists, but only on the app-server that owns the thread. When the Desktop owns it, that server is its private stdio child, so no external process can reach it." — [issue #52705](https://github.com/openai/codex/issues/52705)
  - Issue #51625 (macOS, bundled CLI 0.160.1, open, 2026-10-07): an external `codex queue` message "remained queued behind the active turn". Manually choosing **Steer** in the desktop "incorporated it into that active turn". The app-owned server "was started over stdio without `--listen`" with no Unix or TCP listener. The issue asks for a supported external `turn/steer` into Desktop-owned turns; no maintainer response yet. — [issue #51625](https://github.com/openai/codex/issues/51625)
  - Issue #48928 (open, 2026-09-28) asks for `codex queue --steer` / a delivery mode. It notes the desktop setting `[desktop] followUpQueueMode = "steer"` "applies only to messages typed in the composer". — [issue #48928](https://github.com/openai/codex/issues/48928)
- **Unloaded threads:** `codex queue` "intentionally persists input without resuming an unloaded thread… We'd prefer to preserve that contract". The maintainer (etraut-openai, 2026-09-12) suggests wrapping `codex queue` followed by `codex resume`. Issue #44491 was closed as not planned. Its repro: the item sits in `thread/queue/list` while `thread/read` says `notLoaded`, and `thread/resume` then dispatches it. — [issue #44491](https://github.com/openai/codex/issues/44491)
- **Desktop composer setting:** `followUpQueueMode`, schema `queue | steer | interrupt`, desktop default `steer`, VS Code default `queue`, `agentAccess: read-write`, described as "How follow-up prompts behave while a turn is running". — [ChatGPT.app app.asar](file:///Applications/ChatGPT.app/Contents/Resources/app.asar)
- **Desktop daemon switch #1, `CODEX_APP_SERVER_USE_LOCAL_DAEMON`** (shipped desktop code, undocumented). The app-server connection uses WebSocket over `join(<codex home>, "app-server-control", "app-server-control.sock")` only if all of these hold:
  - the platform is not win32;
  - `getConfigOverrides()` returned no overrides (`!e?.length`);
  - the host kind is `local`;
  - `process.env.CODEX_APP_SERVER_USE_LOCAL_DAEMON === "1"`;
  - `CODEX_APP_SERVER_FORCE_CLI !== "1"`;
  - `CODEX_CLI_PATH` is empty;
  - `hostConfig.codex_cli_command == null`;
  - an internal resources check passes;
  - `codex app-server daemon version` returns a valid `appServerVersion` within 2.5 s.
  - Otherwise it uses `kind = stdio`.
  - — [ChatGPT.app app.asar](file:///Applications/ChatGPT.app/Contents/Resources/app.asar)
- **Desktop daemon switch #2, `CODEX_APP_SERVER_WS_URL`:** the code reads `process.env.CODEX_APP_SERVER_WS_URL ?? hostConfig.websocket_url`, which is ignored when `CODEX_APP_SERVER_FORCE_CLI=1`. — [ChatGPT.app app.asar](file:///Applications/ChatGPT.app/Contents/Resources/app.asar)
  - A community gist (2026-10-06, macOS 27.0.1, ChatGPT desktop 26.930.61225, CLI/daemon 0.160.1; Linux retest 2026-10-13 with bundled 0.162.0-alpha.17.2) makes ChatGPT Desktop share the daemon:
    - `codex app-server daemon start`
    - `open -a /Applications/ChatGPT.app --env "CODEX_APP_SERVER_WS_URL=ws+unix://localhost$HOME/.codex/app-server-control/app-server-control.sock:/rpc"`
    - verification via `ps` (the desktop no longer has its own `app-server` child; `codex app-server --listen unix:// --managed-daemon` serves it) and `codex --remote unix:// resume THREAD_ID`.
  - The gist's caveats: "experimental workaround, not an officially supported setting". The override "bypasses the app's launch-time CLI overrides", so app-only tools and plugins need separate testing; on Linux the author re-added them in config.toml. "Switching interfaces does not create independent simultaneous turns in one chat". The daemon auto-updated from 0.161.0 to 0.162.0 on 2026-10-08 and Remote was found disabled afterwards. — [gist ale-rinaldi](https://gist.github.com/ale-rinaldi/80f063b60177f1281e219ebd68587a4d)
- **No official doc or UI setting:** none of the official CLI reference, app-server docs or changelog says how to point the desktop app at the shared daemon. The CLI reference does not document `codex app-server daemon …` or `daemon_auto_start`. — [CLI reference](https://learn.chatgpt.com/docs/cli/reference); [changelog](https://learn.chatgpt.com/docs/changelog)
- **CLI help quirk (0.162.0-alpha.17.2):** `codex app-server daemon --help`, `codex app-server proxy --help` and `codex exec resume --help` print the root help, while `-h` prints the correct subcommand help. — [local CLI](file:///Applications/ChatGPT.app/Contents/Resources/codex-cli/CodexCLI.app/Contents/MacOS/codex)

### Inferences
- **What reaches a thread the desktop app is showing, on the user's current setup (desktop = private stdio app-server, no daemon):**
  - **Read:** yes, from disk or SQLite (rollouts in `~/.codex/sessions`, `thread/read`/`thread/list` from any separate app-server). It is not live.
  - **Queue a follow-up:** yes, `codex queue --thread <id> --message …`. It is written to `queue_1.sqlite` and picked up by the desktop's app-server when the thread is loaded and idle; it waits behind an active turn, and in `/goal` mode it can starve until a human presses Steer.
  - **Steer, interrupt or inject into the live turn:** no supported path.
  - **Live event stream:** no.
- **If the desktop is relaunched against the shared daemon** (`CODEX_APP_SERVER_WS_URL`, or possibly `CODEX_APP_SERVER_USE_LOCAL_DAEMON=1`), an external agent could connect to the same socket. It would use `codex app-server proxy` or a WebSocket-over-UDS client at `/rpc`, `initialize`, `thread/resume` the desktop's thread id (joining as a second subscriber), then call `turn/steer` with the active `expectedTurnId`, `turn/interrupt`, or `thread/inject_items`, and receive live notifications. This is plausible from the protocol but not officially supported or documented for the desktop, and I did not test it.
- The `USE_LOCAL_DAEMON` path requires no config overrides. The desktop's normal local launch passes `-c` overrides (seen in `ps`: `features.code_mode_host=true` and the two plugin enables), so that switch may never take effect on a stock install. This likely explains why the gist used `CODEX_APP_SERVER_WS_URL` instead. Unverified.
- The daemon is aimed first at SSH-hosted Codex (desktop/mobile managing remote machines). On an SSH host the desktop connects to (the user's desktop has an `ssh … dev-ubuntu-ts` child), threads may live in that host's daemon. A local agent on that host could then reach them via `codex --remote unix://` / `codex queue`. Not verified for the user's dev-ubuntu.

### Gaps
- Whether `codex queue` with no daemon running spins up an embedded app-server or writes SQLite directly. The outcome (a row in `queue_1.sqlite`) is documented by users, not by OpenAI.
- Whether the desktop UI reflects externally queued items before they run: `thread/queue/changed` exists, but I found no confirmation.
- No OpenAI statement on a supported desktop↔daemon mode or any roadmap; #51625 and #48928 have no maintainer replies.

---

## Q3. `codex remote-control` / ChatGPT mobile/web control: is there an API a local agent can use?

### Takeaway
Remote Control connects a host's app-server outbound to OpenAI's relay so paired ChatGPT mobile/desktop devices can drive it. Pairing is QR or a manual code, device-authenticated, one-to-one. No local API comes out of it. A local agent should talk to the app-server/daemon socket directly; Remote Control adds nothing locally beyond enabling the relay transport.

### Cited Findings
- `codex remote-control [start|stop|pair] [--json] [--no-daemon]` (experimental):
  - `start`: "Start the app-server daemon with remote control enabled".
  - `stop`: "Stop the app-server daemon".
  - `pair`: "Create and print a short-lived manual pairing code".
  - `--no-daemon`: "Run an embedded app-server in the foreground until Ctrl-C".
  - — [local `codex remote-control -h`](file:///Applications/ChatGPT.app/Contents/Resources/codex-cli/CodexCLI.app/Contents/MacOS/codex); [CLI reference](https://learn.chatgpt.com/docs/cli/reference)
- **History:**
  - 0.130.0 (2026-05-08): "Added `codex remote-control` as a simpler entrypoint for starting a headless, remotely controllable app-server".
  - 0.131.0: daemon-managed remote control and runtime enable/disable APIs.
  - 0.136.0: remote-control websockets use short-lived server tokens.
  - 0.137.0: pairing and controller-grant RPCs.
  - 0.143.0 (2026-07-08): `codex remote-control pair`.
  - 0.162.0: the bare command uses the managed daemon.
  - — [rust-v0.130.0](https://github.com/openai/codex/releases/tag/rust-v0.130.0); [rust-v0.137.0](https://github.com/openai/codex/releases/tag/rust-v0.137.0); [rust-v0.143.0](https://github.com/openai/codex/releases/tag/rust-v0.143.0); [PR #50803](https://github.com/openai/codex/pull/50803)
- **App-server RPCs:** `remoteControl/enable`, `remoteControl/disable`, `remoteControl/status/read`, `remoteControl/pairing/start`, `remoteControl/pairing/status`, `remoteControl/client/list`, `remoteControl/client/revoke`, plus the notification `remoteControl/status/changed`. The binary also references the backend path `wham/remote/control/environments` and an "enroll" flow. — [local binary strings](file:///Applications/ChatGPT.app/Contents/Resources/codex-cli/CodexCLI.app/Contents/MacOS/codex)
- **Official user flow:**
  - On the host: Settings → Connections → "Control this Mac or PC" → QR code, which pairs that phone with that host.
  - Mobile setup "can't be set up from the Codex CLI or IDE extension".
  - Workspace admins may need to enable Remote Control.
  - The docs warn against exposing app-server transports on shared or public networks.
  - Remote Control has been GA since Codex app 26.623 (2026-06-25), with "authenticated one-to-one QR pairing between each iOS or Android device and each host".
  - — [Remote connections](https://learn.chatgpt.com/docs/remote-connections); [changelog](https://learn.chatgpt.com/docs/changelog)
- **Phones can queue and steer:**
  - ChatGPT for iOS 1.2026.146 (2026-06-02) "Added a new settings screen for choosing Queue or Steer as the default follow-up behavior".
  - 1.2026.237 (2026-09-01) "Queued prompts now sync with the connected host".
  - — [changelog](https://learn.chatgpt.com/docs/changelog)
- The app-server README notes that native user-verification calls accept local stdio/in-process connections, while "WebSocket and remote-control peers must use their own device authenticator". So remote-control peers are treated as separate authenticated devices. — [app-server README](https://github.com/openai/codex/blob/main/codex-rs/app-server/README.md)

### Inferences
- A phone can steer desktop-hosted threads (Queue/Steer setting). That steering goes through OpenAI's relay into the host app-server with remote control enabled, via device-paired, account-bound credentials. It is not something a local program can use without impersonating a paired device. No supported local endpoint comes from it.

### Gaps
- Whether the desktop app's own stdio app-server carries the remote-control transport when the desktop is the host, or delegates to a daemon. #50803 implies the CLI path uses the daemon; the desktop path is not documented.

---

## Q4. `codex exec --json` and `codex exec resume <id> "prompt"` when the thread is open or running in the desktop app

### Takeaway
`codex exec` is the stable non-interactive path (`--json` = JSONL events). `exec resume <id|name> [PROMPT]` continues a thread in a new process. Since early September 2026 Codex enforces a cross-process single writer per thread. If the desktop app's app-server has the thread loaded, another process resuming it hits an "already has an active writer" conflict. The TUI then shows a read-only view; it does not inject a turn into the desktop. The desktop does not pick up turns run elsewhere live.

### Cited Findings
- `codex exec [OPTIONS] [PROMPT]`, subcommands `resume`, `fork` and `review`. Flags include `--json` (alias `--experimental-json`), `-o/--output-last-message <FILE>`, `--output-schema`, `--ephemeral`, `--skip-git-repo-check` and `--thread-source <SOURCE>`. `codex exec resume [SESSION_ID] [PROMPT]`: "Conversation/session id (UUID) or thread name… If `-` is used, read from stdin", with `--last` and `--all`. — [local `codex exec -h`, `codex exec resume -h`](file:///Applications/ChatGPT.app/Contents/Resources/codex-cli/CodexCLI.app/Contents/MacOS/codex); [CLI reference](https://learn.chatgpt.com/docs/cli/reference)
- `--json`: "Print newline-delimited JSON events instead of formatted text". `codex exec` is labeled stable. — [CLI reference](https://learn.chatgpt.com/docs/cli/reference)
- `exec resume` changes over time:
  - 0.79.0: global flags may follow `exec resume`.
  - 0.132.0 (2026-05-20): `--output-schema` for `exec resume`.
  - 0.147.0: prefers the state DB for `--last`.
  - 0.153.0: handles compressed rollouts.
  - — [rust-v0.132.0](https://github.com/openai/codex/releases/tag/rust-v0.132.0); [rust-v0.153.0](https://github.com/openai/codex/releases/tag/rust-v0.153.0)
- PR #43253 "Show read-only conversations when resume encounters an active writer" (merged 2026-09-06): "Resuming a conversation that is open in another app fails with an active-writer error." The TUI falls back to a read-only snapshot with `r` to retry. PR #47185 (2026-09-22, released in 0.157.0): "Conversations open in another app disable the composer". `f` forks into an editable copy "without taking the source lease". TUI text: "This conversation is open in another app… Close it there and press r to retry". — [PR #43253](https://github.com/openai/codex/pull/43253); [PR #47185](https://github.com/openai/codex/pull/47185); [local binary strings](file:///Applications/ChatGPT.app/Contents/Resources/codex-cli/CodexCLI.app/Contents/MacOS/codex)
- PR #44138 (2026-09-09): "Share cross-process writer locks between the rollout compressor and local thread store… reject competing writers before updating SQLite". Binary errors: "thread … already has an active writer", "failed to acquire thread writer lock". PR #44183 fixes writers left held after cancelled startup "blocking a subsequent resume". — [PR #44138](https://github.com/openai/codex/pull/44138); [PR #44183](https://github.com/openai/codex/pull/44183)
- A loaded thread stays loaded until 30 minutes after its last subscriber unsubscribes. — [learn.chatgpt.com/docs/app-server](https://learn.chatgpt.com/docs/app-server)

### Inferences
- `codex exec resume <desktop-thread-id> "…"` while the desktop has that thread loaded will most likely fail with the active-writer error. The thread may still be loaded after you navigate away from it (30-min grace). Once the desktop releases it, `exec resume` succeeds and appends turns to the same rollout. The desktop would then show them only after reloading or re-resuming the thread, not live. Before ~0.153 (September 2026) there was no lock, and two writers could interleave a rollout.
- `codex exec` (new thread) never conflicts. Its JSONL is the simplest reliable "Claude → Codex task" channel, but it targets a separate thread, not the desktop's.

### Gaps
- I did not test the exact `exec resume` error text or exit code, or whether `codex exec` attaches to a running shared daemon (where the resume would be a second subscriber instead of a lock conflict). No doc states exec's daemon eligibility.

---

## Q5. Does current Codex still ship `codex mcp-server` (tools `codex` / `codex-reply`)?

### Takeaway
No. It was deprecated 2026-08-24, and the command plus the standalone `codex-mcp-server` binary were removed in rust-v0.154.0 (2026-09-09, PR #42993). OpenAI's replacement is the app-server, which is JSON-RPC, not MCP. `codex mcp` now only manages external MCP servers that Codex consumes. In 0.162.x, `codex mcp-server` is parsed as a prompt.

### Cited Findings
- Changelog 2026-08-24: "The `codex mcp-server` command is now deprecated. Use the Codex app server instead." Changelog 2026-09-05: "The `codex mcp-server` command and standalone `codex-mcp-server` binary have been removed". — [changelog](https://learn.chatgpt.com/docs/changelog); [CLI reference](https://learn.chatgpt.com/docs/cli/reference)
- rust-v0.154.0 (2026-09-09): "The deprecated `codex mcp-server` entry point is no longer available. (#42993)" / "#42993 Remove the deprecated `codex mcp-server` command". — [rust-v0.154.0](https://github.com/openai/codex/releases/tag/rust-v0.154.0)
- **History:**
  - 0.44.0 (2025-10-03) split `codex mcp` into `codex mcp-server` and `codex app-server`.
  - 0.81.0 (2026-01-14): the `codex` tool returned `threadId` for use with `codex-reply`.
  - — [rust-v0.44.0](https://github.com/openai/codex/releases/tag/rust-v0.44.0); [rust-v0.81.0](https://github.com/openai/codex/releases/tag/rust-v0.81.0)
- **Locally:** neither `codex --help` in 0.162.0-alpha.17.2 nor 0.162.1 lists `mcp-server`, and `codex mcp-server --help` prints the root help. `codex mcp` offers `list|get|add|remove|login|logout`. — [local CLI](file:///Applications/ChatGPT.app/Contents/Resources/codex-cli/CodexCLI.app/Contents/MacOS/codex)
- **Downstream breakage:** Claude-side integrations broke, e.g. KjellKod/quest #176 ("Codex CLI 0.154.0 removed `codex mcp-server`") and jarrodwatts/claude-delegator #14. The third-party `abetoots/codex-app-mcp` re-exposes `codex`/`codex-reply` on top of `codex app-server`. — [quest #176](https://github.com/KjellKod/quest/issues/176); [claude-delegator #14](https://github.com/jarrodwatts/claude-delegator/issues/14); [codex-app-mcp](https://github.com/abetoots/codex-app-mcp)
- **Desktop-internal thread tools are not external MCP:** the desktop's bundled `codex_app` MCP namespace gives Codex agents inside the desktop `create_thread`, `send_message_to_thread` (annotated `destructiveHint`), `fork_thread`, `handoff_thread`, `automation_update` and `list_threads`. It is enabled through `-c plugins.codex-app-tools@openai-bundled.mcp_servers.codex_app.enabled=true` on the desktop's app-server. Issue #42973 reports these tools missing on headless SSH hosts. — [ChatGPT.app app.asar](file:///Applications/ChatGPT.app/Contents/Resources/app.asar); [issue #42973](https://github.com/openai/codex/issues/42973)

### Inferences
- "Renamed / behind a flag" does not apply; it is gone. For Claude Code → Codex over MCP, the options are a third-party wrapper around app-server, or shelling out to `codex exec --json` / `codex exec resume` / `codex queue`.

### Gaps
- None material.

---

## Q6. SDKs (TypeScript `@openai/codex-sdk`, Python `openai-codex`): can they attach to an existing thread by id and stream or steer it?

### Takeaway
- **TypeScript SDK:** wraps `codex exec --experimental-json`. `resumeThread(id)` starts a new `codex exec … resume <id>` process per run. You can stream (`runStreamed`) and abort, but not steer, and it cannot attach to a thread live in another process.
- **Python SDK:** a typed JSON-RPC client for `codex app-server` over stdio. It has `thread_resume(id)`, turn handles with `steer()` and `interrupt()`, and `ExternalMessage` that "joins an active regular turn". It still only controls threads in the app-server it launched, unless pointed at the shared daemon (unofficial, see inference).

### Cited Findings
- **TS SDK:** "The TypeScript SDK wraps the `codex` CLI from `@openai/codex`. It spawns the CLI and exchanges JSONL events over stdin/stdout." The API offers `codex.startThread()`, `thread.run()`, `thread.runStreamed()` (async generator of events) and `codex.resumeThread(savedThreadId)`. "Threads are persisted in `~/.codex/sessions`." — [TS SDK README](https://github.com/openai/codex/blob/main/sdk/typescript/README.md)
- `sdk/typescript/src/exec.ts` builds `["exec", "--experimental-json"]`, appends `"resume", threadId` for resumed threads, and passes an `AbortSignal` to the child process. — [exec.ts](https://github.com/openai/codex/blob/main/sdk/typescript/src/exec.ts)
- **Python SDK:** `pip install openai-codex`, `from openai_codex import Codex`. `Codex.thread_start()`, `thread_list(...)`, `thread_resume(thread_id, …)`, `thread_fork(...)`. `thread.run(...)` returns a `TurnResult`. "Use `turn(...)` when you need low-level turn control (`stream()`, `steer()`, `interrupt()`)", with `TurnHandle.steer(input) -> TurnSteerResponse` and `interrupt() -> TurnInterruptResponse` (sync and async). — [Python SDK README](https://github.com/openai/codex/blob/main/sdk/python/README.md); [Python API reference](https://github.com/openai/codex/blob/main/sdk/python/docs/api-reference.md)
- **Python `ExternalMessage(tool_name, content, namespace)`:** "supplies untrusted content from another agent, tool, or application… It starts a turn when the thread is idle or joins an active regular turn". Both handles can stream it, and it is stored as a `functionCallOutput` item with tool-level authority. It requires CLI ≥ 0.151.0. — [Python API reference](https://github.com/openai/codex/blob/main/sdk/python/docs/api-reference.md)
- **Python transport:** `client.py` says "Synchronous typed JSON-RPC client for `codex app-server` over stdio". `CodexConfig` takes `codex_bin`, `launch_args_override: tuple[str, ...]`, `cwd` and `env`. — [client.py](https://github.com/openai/codex/blob/main/sdk/python/src/openai_codex/client.py)
- **Python SDK history:** first introduced around PR #14245/#14435 with filesystem RPCs; renamed to `openai-codex` (#21905); versions aligned with stable CLI releases (#44067); "Start Python SDK turn subscriptions at their attachment point" (#44400); "Add untrusted external messages to the Python SDK" (#44086). — [openai/codex releases](https://github.com/openai/codex/releases)
- **TS SDK fix:** `resumeThread()` argument ordering was fixed so resuming with local images no longer starts an unintended new session (#10709). In 0.161.0 the TS SDK gained a `cyberAccessProgram` option. — [openai/codex releases](https://github.com/openai/codex/releases)

### Inferences
- Neither SDK officially attaches to a thread the desktop app owns. TS `resumeThread` on a desktop-loaded thread should hit the same active-writer conflict as `exec resume`.
- Python with `CodexConfig(launch_args_override=("codex","app-server","proxy"))` would plausibly make the SDK speak to the shared daemon socket instead of a private server. Combined with a desktop relaunched onto the daemon (Q2), that would be the closest thing to "SDK steers the desktop's live thread". It is untested and unsupported.

### Gaps
- No official doc describes either SDK connecting to `--remote`/`unix://` endpoints or to a running daemon.
- I did not verify whether the TS SDK has any `steer`/`interrupt` beyond AbortSignal (README shows none).

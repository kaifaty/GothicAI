# Agent messaging hubs, bridges and orchestrators for Claude Code <-> Codex (state as of October 2026)

Scope note: research done 2026-10-15 with web search + page fetches. Star counts and dates are what the fetched pages showed on that day. Many GitHub pages did not expose last-commit dates in the fetch; where a date is missing it is listed under Gaps rather than guessed.

## Key question 0 (cross-cutting): can anything reach an already-running session, and a Codex desktop-app thread in particular?

### Takeaway
Claude Code has an official push path into a *running* CLI session (Channels, research preview). Codex has an official programmatic surface (app-server JSON-RPC with `turn/start` / `turn/steer`) and `codex --remote` lets a TUI share an app-server, but as of mid-October 2026 there is **no documented way for an external client to attach to the live thread of the Codex desktop app**; the open issues asking for it have no maintainer answer. Every "live" Claude<->Codex bridge found works by launching Codex itself (TUI on a shared app-server or a child app-server), not by joining a desktop-app thread.

### Cited Findings
- Codex app-server is the interface Codex uses to power rich clients (e.g. the VS Code extension); transports: stdio (default), WebSocket `--listen ws://IP:PORT` (experimental, unsupported), Unix socket `--listen unix://PATH`; methods include `thread/start`, `thread/resume`, `thread/fork`, `turn/start`, `turn/steer` ("add input to the active turn"), `turn/interrupt`, `thread/loaded/list`; notifications `turn/started`, `turn/completed`, `item/agentMessage/delta`, `thread/status/changed` etc. — [Codex App Server docs](https://learn.chatgpt.com/docs/app-server) (redirected from developers.openai.com/codex/app-server)
- The TUI can attach to a running app-server: `codex app-server --listen ws://127.0.0.1:4500` then `codex --remote ws://127.0.0.1:4500`; WebSocket auth via `--ws-auth capability-token` or `signed-bearer-token`; app-server and WebSocket transport are "experimental and not supported for production workloads"; the docs do not say how the desktop app uses app-server — [Codex App Server docs](https://learn.chatgpt.com/docs/app-server)
- Issue #25914 "Document how app-server clients can discover and attach to the active Codex Desktop thread" (opened 2026-06-02, still open, latest comment 2026-10-13, no OpenAI maintainer reply): `thread/loaded/list` returned zero loaded threads from the client-visible surface; `turn/interrupt` on the candidate thread → "thread not found"; `turn/steer` on a resumed thread → "no active turn to steer"; Windows users got "already has an active writer"; commenters say the Desktop IPC thread-follower methods are not a public contract; only workarounds are resuming the persisted thread in a separate app-server (does not attach to the live UI turn) or creating a continuation from history — [openai/codex#25914](https://github.com/openai/codex/issues/25914)
- Issue #40134 "Allow Codex Desktop to connect to an externally managed App Server" (opened 2026-08-22, open, no maintainer reply): Desktop has no documented way to join an external app-server; a commenter's orchestrator drives its own app-server but its threads don't appear in the running Desktop sidebar; restarting Desktop is described as the only way externally created threads show up; another comment asks for the external-client contract on macOS Desktop app-server `0.162.0-alpha.17.2` — [openai/codex#40134](https://github.com/openai/codex/issues/40134)
- External client can append a turn to a Desktop-visible thread, but the already-open Desktop thread view does not live-refresh — [openai/codex#21743](https://github.com/openai/codex/issues/21743)
- Windows 0.147-alpha: automation via `codex exec resume` / TS SDK `resumeThread()` cannot run while Desktop is open if the thread was ever viewed (Desktop keeps the thread writer) — [openai/codex#37450](https://github.com/openai/codex/issues/37450)
- Related open requests: persistent thread-attachment RPCs [#24398](https://github.com/openai/codex/issues/24398); peer-client co-presence with the live TUI thread (RFC) [#21551](https://github.com/openai/codex/issues/21551); programmatic sidebar-visible chat creation from an external orchestrator [#24107](https://github.com/openai/codex/issues/24107); cross-thread messaging by thread ID [#22768](https://github.com/openai/codex/issues/22768); explicit cross-thread orchestration in Desktop [#14923](https://github.com/openai/codex/issues/14923)
- A third-party reading of the source says the TUI probes `$CODEX_HOME/app-server-control/app-server-control.sock` and, if present, runs the thread in that daemon with the TUI as a client (an undocumented local control path, not a Desktop attach mechanism) — [davebream/glosa#161](https://github.com/davebream/glosa/issues/161) (via search snippet)
- Inside Codex itself there are native cross-thread tools: a community skill uses `codex_tui.list_threads`, `codex_tui.read_thread`, `codex_tui.send_message_to_thread` ("send a follow-up prompt to an existing thread in the background"), `codex_tui.wait_threads`; the sender is approval-gated, prompts limited to 1,000 UTF-8 bytes; on Codex 0.159.3 a `--remote`-reconnected TUI lost the sender, on 0.160.0 all nine tools were exposed; designed for Codex-to-Codex only (1 star) — [Vuk97/codex-thread-coordination](https://github.com/Vuk97/codex-thread-coordination)
- Paul Solt (X): "Codex can have a thread manage other threads", Codex can create heartbeats, and `/goal` on work threads — [Paul Solt on X](https://x.com/PaulSolt/status/2071945658492170259)
- The official changelog's October 2026 section reportedly includes commit-style items such as preserving queued agent mail across session eviction and "remote agent message boards in multi-agent sessions" (seen only in a search summary, not verified by fetch) — [ChatGPT & Codex changelog](https://developers.openai.com/codex/changelog/)
- Claude Code side: a channel is an MCP server (local stdio subprocess) that pushes `notifications/claude/channel` events into a running session; declares `capabilities.experimental['claude/channel']`; events queue and, if Claude is busy, are delivered together on the next turn; no acknowledgment (dropped silently if not registered); custom channels need `claude --dangerously-load-development-channels server:<name>` and the flag is ignored in `-p` / Agent SDK mode; optional permission relay (`claude/channel/permission`) lets the remote side approve tool calls; Team/Enterprise orgs must enable channels — [Claude Code Channels reference](https://code.claude.com/docs/en/channels-reference)
- Channels require Claude Code v2.1.80+ and ship with Telegram, Discord, (iMessage) and the fakechat demo — [search summary of channels docs](https://code.claude.com/docs/en/channels-reference.md); [Channels reference](https://code.claude.com/docs/en/channels-reference)
- An Anthropic-side feature request for official Channels→Codex app-server bridging was closed as not planned — [anthropics/claude-code#36871](https://github.com/anthropics/claude-code/issues/36871) (per search summary)

### Inferences
- For "Claude supervises a Codex **desktop-app** thread" there is no clean, supported path today. Realistic options: (a) run Codex in a TUI against a shared app-server (`codex app-server --listen unix://...` + `codex --remote`) and have Claude drive the same server via JSON-RPC (`turn/start`, `turn/steer`) — this is what AgentBridge does; (b) drive the desktop app via UI automation/computer use (fragile, rejected by issue commenters); (c) treat the desktop thread as read-only history and continue it elsewhere.
- Channels is the best mechanism for the opposite direction (Codex → running Claude Code), but it is tied to launching the `claude` CLI with a development flag; whether it works for sessions in the Claude Code desktop app is not documented (see Gaps).
- Local observation (not web-sourced): the Claude Code desktop app in this environment exposes session-management tools (e.g. sending a message to another Claude session), so Claude→Claude steering inside the desktop app is easier than Claude→Codex.

### Gaps
- No official OpenAI statement on a desktop-app attach contract; no maintainer comment in #25914 or #40134 as of 2026-10-13.
- Not found: whether Claude Code Channels (`--dangerously-load-development-channels`) can be enabled for sessions started from the Claude Code desktop app.
- Not verified: whether `codex_tui.send_message_to_thread` reaches threads that the desktop app is showing (the skill only claims "recipients on the same app server").

## Key question 1: Agent messaging over MCP (MCP Agent Mail and similar inbox/mailbox servers) — how are agents notified?

### Takeaway
MCP Agent Mail is the most mature shared-inbox layer (identities, threads, file leases, web UI, Claude Code + Codex + Gemini wired by installer), but it is **pull-based**: agents learn of mail by calling `fetch_inbox` (or a cursor-based event feed), so an idle session is not woken. The Python original is now marked superseded by a Rust rewrite. Smaller Claude<->Codex MCP bridges either use request/response MCP tools (Claude calls Codex as a tool) or add push into Claude via Channels.

### Cited Findings
- MCP Agent Mail = "asynchronous email + directory + change-intent signaling for your agents", HTTP-only FastMCP server backed by Git (human-auditable) + SQLite; per-agent inbox/outbox on disk; contact policies (open / contacts-only / block-all) — [glama README mirror](https://glama.ai/mcp/servers/@Dicklesworthstone/mcp_agent_mail/blob/5fa08841bb5783e805d166bec4754a72b6dc1ac8/README.md); [GitHub](https://github.com/Dicklesworthstone/mcp_agent_mail)
- Python repo: banner says "Superseded by mcp_agent_mail_rust" and "no longer maintained" (while the status line still says "Under active development" — internal contradiction); ~2.2k stars, 230 forks, 874 commits; supports Claude Code, Codex, Gemini CLI, Factory Droid (+ configs for Cursor, Cline, Windsurf); installer `curl ... install.sh | bash -s -- --yes` installs uv/jq, Python 3.14 env, wires detected agents, starts server on **port 8765**, prints a masked bearer token; flags `--port`, `--token`; web UI at `/mail` with a "Human Overseer" composer; advisory file reservations (TTL, exclusive/shared) + optional pre-commit guard using `AGENT_NAME`; AGENTS.md/CLAUDE.md blurb helper; companion tools Beads (`br`/`bd`), Beads Viewer (`bv`), commercial "AgentMail Companion" iOS app — [Dicklesworthstone/mcp_agent_mail](https://github.com/Dicklesworthstone/mcp_agent_mail)
- Notification model: polling — `fetch_inbox` (filter to unread to cut polling cost), `resource://inbox/...`; no push/hook/wake mechanism described — [mcp_agent_mail](https://github.com/Dicklesworthstone/mcp_agent_mail)
- Rust rewrite: 199 stars, 40 forks, 5,305 commits, mentions releases up to v0.3.39, MIT+Rider license; agents pull mail via `fetch_inbox`, `resource://inbox/{Agent}`, `am robot inbox`; for monitors, `fetch_inbox_events` / `am inbox-events` gives a durable cursor-based feed that survives restarts; still no push or idle-agent wake-up described; supports Claude Code, Codex CLI, Gemini CLI, Oh My Pi, Copilot CLI; default `127.0.0.1:8765`, client URL `http://127.0.0.1:8765/mcp/`, Codex config in `~/.codex/config.toml`; 16-screen TUI + `/mail/` web UI; rewrite targets Git lock contention, SQLite pool exhaustion, cascading failures — [Dicklesworthstone/mcp_agent_mail_rust](https://github.com/Dicklesworthstone/mcp_agent_mail_rust)
- Third-party tracker lists ~1.9k stars for the Python repo — [beamforai](https://www.beamforai.com/tools/Dicklesworthstone/mcp_agent_mail) (slightly differs from 2.2k on GitHub; trackers lag)
- `claude relay` (gvorwaller): relay for Claude Code instances that can also track Codex sessions; registry in `~/claude-relay/sessions/registry.json`; tools `relay_send` / `relay_receive` — [glama: claude-relay](https://glama.ai/mcp/servers/gvorwaller/claude-relay)
- `agents-bridge-mcp` (naldomadeira): "Bidirectional MCP server bridge between Claude Code and Codex CLI" — each side can ask the other for help/review — [GitHub](https://github.com/naldomadeira/agents-bridge-mcp)
- Request/response style bridges and their gotchas: `tuannvm/codex-mcp-server` (own session store, 24h TTL, 100-session cap); npm `claude-codex-bridge` (symmetric MCP on both sides, `BRIDGE_DEPTH` blocks depth ≥2 → no Claude→Codex→Claude chains); `abhishekgahlot2/codex-claude-bridge` (Channels for push + web UI; push only when Codex initiates, Claude→Codex is still request/poll); `mkXultra/claude-code-mcp` "ai-cli-mcp" (one MCP server exposing Claude, Codex, Gemini CLIs, async background runs) — [Daniel Vaughan, 2026-03-27](https://codex.danielvaughan.com/2026/03/27/claude-code-codex-mcp-in-practice/)
- Native MCP servers: `codex mcp-server` exposes `codex` (new session) and `codex-reply` (continue by thread ID); thread ID now returned in `structuredContent.threadId` (earlier versions didn't return it — issues #3712, #4651, #5660, #8388, #8580); `claude mcp serve` exposes Claude Code as an MCP server; suggested Codex-side timeouts e.g. `tool_timeout_sec = 180–300` — [Daniel Vaughan](https://codex.danielvaughan.com/2026/03/27/claude-code-codex-mcp-in-practice/); [Codex MCP docs](https://developers.openai.com/codex/mcp)
- A proposal (not a finished tool) for a local MCP mailbox for Codex + Claude with "restart preserves unread messages" — [pzelant/VikingAI#44](https://github.com/pzelant/VikingAI/issues/44)

### Inferences
- Shared-inbox servers solve "where do messages live" but not "who wakes up": without a hook/loop, an agent only sees mail when it next calls `fetch_inbox`. Pairing an inbox with Claude Code Channels (push into Claude) or with Codex app-server `turn/start` (push into Codex) is what turns it into steering.
- Practical conflict for this user: Agent Mail's default port 8765 collides with the user's dashboard port (per the user's own project notes); it would need `--port`.
- `codex mcp-server` used from Claude Code creates *new* Codex sessions owned by the MCP server process — it is good for "Claude delegates a task to Codex", not for steering a Codex thread the user already has open.

### Gaps
- Last-commit dates for mcp_agent_mail (Python and Rust) were not visible in fetches.
- No evidence found of any inbox server with true push into an idle Codex session.

## Key question 2: Live two-way Claude Code <-> Codex bridges (Channels + Codex app-server)

### Takeaway
A small cluster of 2026 projects pairs Claude Code Channels (push into Claude) with Codex app-server or Codex MCP (push into Codex). AgentBridge (raysonmeng) is the most visible (391 stars, MIT, macOS/Linux) but it launches its own Codex TUI against a proxied app-server; none of them claims to attach to a Codex desktop-app thread.

### Cited Findings
- AgentBridge (posted 2026-03-20 in openai/codex Discussions #15374): "first bridge" plugging MCP notifications into the open-source Codex App Server, claims bidirectional mid-execution injection in one live session — [openai/codex discussion #15374](https://github.com/openai/codex/discussions/15374)
- AgentBridge architecture: `bridge.ts` (MCP client/channel started by Claude Code via plugin) + persistent `daemon.ts` owning a Codex app-server proxy; ports 4500 (app-server WS), 4501 (proxy the Codex TUI connects to), 4502 (control WS); Codex→Claude: daemon captures `agentMessage` and emits a channel notification; Claude→Codex: `reply` tool injected as `turn/start` (no `turn/steer` mentioned); fallback in-memory queue drained by `get_messages`; loop prevention via `source` field; busy-guard rejects replies during an active Codex turn (README elsewhere says either side can push mid-turn — contradictory); setup `npm install -g @raysonmeng/agentbridge`, `abg init`, `abg claude`, `abg codex` (TUI with `--remote`); runs with `--dangerously-skip-permissions` / `--yolo` by default (`--safe` to opt out); requires Bun; macOS + Linux; 391 stars, 64 forks, 193 commits, MIT; only `agentMessage` items forwarded; one Codex thread per pair; cross-network "rooms" preview in v0.1.31 — [raysonmeng/agent-bridge](https://github.com/raysonmeng/agent-bridge)
- claude-codex-bridge (eduardruzsa): Claude Channel + Codex MCP, local Unix socket in a 0700 dir with random token; can attach Codex to an existing Claude conversation and opens the other agent in a new terminal if not running; uses `codex queue` (tested with Codex 0.156.1); Codex asks approval before every bridge MCP call unless `default_tools_approval_mode = "approve"`; macOS needs Xcode CLT; Node 22.23.2+; desktop app not mentioned; 0 stars, 22 commits, MIT — [eduardruzsa/claude-codex-bridge](https://github.com/eduardruzsa/claude-codex-bridge)
- C-C-chat-bridge (Alex123666tw): "Local MCP bridge connecting Codex and Claude Code native cross-chat messaging interfaces"; relies on undisclosed "internal app interfaces" of Codex Desktop, can reach Codex chats on machines connected to Codex Desktop; **Windows-only** host, needs a running Claude Code session; docs in Traditional Chinese; 0 stars, 7 commits, MIT — [Alex123666tw/C-C-chat-bridge](https://github.com/Alex123666tw/C-C-chat-bridge)

### Inferences
- AgentBridge is the closest thing to a ready-made "Claude supervises a live Codex session" tool, but the Codex side is a TUI it starts (`abg codex`), so the user would supervise a TUI thread, not a ChatGPT.app/Codex.app thread. Its permission-skipping defaults are a notable risk.
- C-C-chat-bridge is the only project claiming access to Codex Desktop chats, via unofficial internals, and is Windows-only and essentially unadopted.

### Gaps
- Release/commit dates for AgentBridge, claude-codex-bridge and C-C-chat-bridge were not visible.
- No independent reports (HN/Reddit) evaluating these bridges were found in the time budget.

## Key question 3: Agent Client Protocol (ACP) adapters — can one ACP client drive both and pass messages?

### Takeaway
Both agents are reachable over ACP through adapters (Claude via Zed's `claude-agent-acp`, formerly `claude-code-acp`; Codex via `codex-acp`), and clients like Zed, JetBrains, Nimbalyst or the Vercel AI SDK provider can host both. But ACP is a client↔agent (editor↔agent) protocol: the client spawns the agent as a subprocess, so it cannot attach to an already-running Codex desktop thread, and no ACP-level agent-to-agent messaging was found — relaying would be the client's own logic.

### Cited Findings
- ACP agents list: "Claude Agent — via Zed's SDK adapter" (`zed-industries/claude-agent-acp`); "Codex CLI — via ACP's adapter" (`agentclientprotocol/codex-acp`); the page says nothing about multi-agent or agent-to-agent messaging — [agentclientprotocol.com agents](https://agentclientprotocol.com/get-started/agents)
- Package naming changed: older docs use `@zed-industries/claude-code-acp`, newer ones `@zed-industries/claude-agent-acp`; Codex also via `@zed-industries/codex-acp` or community `@normahq/codex-acp-bridge` — [ex_mcp ACP guide](https://hexdocs.pm/ex_mcp/acp_guide.html); [AI SDK ACP provider](https://ai-sdk.dev/providers/community-providers/acp); [sourcepulse: claude-agent-acp by agentclientprotocol](https://www.sourcepulse.org/projects/14647130)
- Conflicting attribution: Daniel Vaughan describes `codex-acp` as a community-maintained Rust bridge by cola-io (`cola-io/codex-acp`), with session modes read-only/auto/full-access, slash commands, embedded `acp_fs` MCP server; Zed and JetBrains co-developed ACP; JetBrains 2025.3+ lists Codex in its ACP Agent Registry — [Daniel Vaughan, 2026-05-01](https://codex.danielvaughan.com/2026/05/01/codex-cli-agent-interoperability-protocols-mcp-acp-a2a/); contradicted by the ACP site listing `agentclientprotocol/codex-acp` — [agentclientprotocol.com](https://agentclientprotocol.com/get-started/agents)
- Same article: ACP connects agents to editing environments; agent-to-agent coordination is attributed to A2A — [Daniel Vaughan](https://codex.danielvaughan.com/2026/05/01/codex-cli-agent-interoperability-protocols-mcp-acp-a2a/)
- Vercel AI SDK community provider bridges ACP agents (Claude Code, Gemini CLI, Codex CLI…) into the AI SDK — [AI SDK ACP provider](https://ai-sdk.dev/providers/community-providers/acp)
- `beyond5959/acp-adapter`: Go project bridging Codex, Claude Code and Pi to ACP; 4 stars, 0 forks — [gittrend](https://gittrend.io/repo/beyond5959/acp-adapter)
- Nimbalyst lists Claude Code and Codex as primary engines and "Codex-via-ACP" as alpha — [rywalker research: Nimbalyst](https://rywalker.com/research/nimbalyst) (via search summary)

### Inferences
- An ACP host could in principle run Claude and Codex side by side and forward one's output as the other's prompt, but that is custom orchestration; ACP itself offers nothing for steering a session started elsewhere (desktop app or another terminal).
- ACP adapters use the official SDKs/CLIs, so subscriptions/API keys behave as for headless use; the exact auth requirements per adapter were not verified.

### Gaps
- No ACP client was found that advertises Claude↔Codex message relay as a feature.
- Whether ACP `session/load` can resume a Codex thread created in the desktop app was not checked.

## Key question 4: Google A2A (Agent2Agent) bridges for Claude Code / Codex

### Takeaway
Native A2A in Codex was still an open issue/PR as of May 2026; the practical route is small third-party bridges that wrap each CLI as an A2A peer (a2abridge, a2acode, a2a-adapter). They are young, low-adoption, and spawn their own agent processes, so they add a protocol layer without solving the "reach a running desktop thread" problem.

### Cited Findings
- Codex native A2A: tracked in issue #11980 (work in `codex-rs/a2a` and `codex-rs/mcp-server`), still open as of May 2026; external orchestrators (Agents SDK, Kata, Bernstein) use Codex's MCP server instead — [Daniel Vaughan, 2026-05-01](https://codex.danielvaughan.com/2026/05/01/codex-cli-agent-interoperability-protocols-mcp-acp-a2a/); [openai/codex#11980](https://github.com/openai/codex/issues/11980)
- a2abridge (Go, MIT): wraps each local coding agent incl. Claude Code and Codex CLI as an A2A 1.0 peer; local discovery; JSON-RPC 2.0 + SSE — [pkg.go.dev a2abridge](https://pkg.go.dev/github.com/vbcherepanov/a2abridge)
- a2acode (Python, Apache 2.0, v0.6.x): ACP on the agent side, A2A on the caller side; Claude Code and any ACP agent (e.g. Codex) as backends — [PyPI a2acode](https://pypi.org/project/a2acode/)
- a2a-adapter (Go): Claude Code and Codex backends; for Codex runs a long-lived `codex app-server`, approvals via app-server server→client RPCs; streaming over A2A — [pkg.go.dev a2a-adapter](https://pkg.go.dev/github.com/vibe-agi/a2a-adapter)
- A2A spec repository — [a2aproject/A2A](https://github.com/a2aproject/A2A)

### Inferences
- For a single-user Mac setup, A2A adds overhead (agent cards, JSON-RPC server per agent) with little benefit over MCP or app-server directly; it matters more for cross-vendor/cross-machine fleets.

### Gaps
- No adoption numbers (stars, users) or independent evaluations found for a2abridge/a2acode/a2a-adapter.
- Current (October 2026) status of Codex issue #11980 not re-checked.

## Key question 5: Orchestrators/dashboards that run Claude Code and Codex side by side

### Takeaway
Most orchestrators (Conductor, vibe-kanban, claude-squad, Nimbalyst, agent-deck, ntm, Superset, Emdash) support Codex and let a *human* steer sessions they spawned (attach / follow-up message / tmux), usually in per-task git worktrees. None found lets them adopt a session started outside the tool (e.g. a Codex desktop thread), and cross-agent messaging is mostly absent — ntm (+ Agent Mail) and agent-deck's "conductor" session are the main exceptions. Several projects are abandoned or pivoted.

### Cited Findings
- **Conductor** (Melty Labs, YC S24, $22M Series A): macOS app (Apple Silicon), runs Claude Code, Codex, Cursor, OpenCode in parallel, one workspace/branch/terminal/diff per task; multi-model mode runs Claude and Codex on the same prompt to compare diffs; uses existing Claude Pro/Max or API key; free locally per one source — [Conductor docs](https://www.conductor.build/docs/); [rustman wiki, 2026-04-22](https://rustman.org/wiki/conductor-parallel-agents/); [amux comparison](https://amux.io/blog/best-multi-agent-orchestrators-2026/). Docs link a "Conductor API" (/docs/api) whose content wasn't retrieved.
- **Vibe Kanban** (BloopAI): README says "Vibe Kanban is sunsetting"; 28.4k stars; supports Claude Code, Codex, Gemini CLI, Copilot, Amp, Cursor, OpenCode, Droid, CCR, Qwen Code; review diffs, inline comments, send feedback to the agent; `npx vibe-kanban` — [BloopAI/vibe-kanban](https://github.com/BloopAI/vibe-kanban). Maintenance status conflicts across sources (community-maintained after Bloop's shutdown; paid cloud ended after a 2026-04-10 shutdown; "four months without a commit" vs a last public commit 2026-09-19) — [search summary of comparisons](https://www.augmentcode.com/tools/open-source-agent-orchestrators); [munderdiffl.in](https://munderdiffl.in/blog/claude-code-orchestration-tools-compared/)
- **claude-squad** (smtg-ai): 8.6k stars, AGPL-3.0; tmux + git worktrees; supports Claude Code, Codex (`cs -p "codex"`), Gemini, Aider (+ OpenCode, Amp per description); steering = attach to the session (`↵/o`) and reprompt, detach `ctrl-q`; no cross-agent messaging — [smtg-ai/claude-squad](https://github.com/smtg-ai/claude-squad); contradicted by a comparison table listing it as "Claude Code only" — [rustman wiki](https://rustman.org/wiki/conductor-parallel-agents/)
- **Crystal** (stravu): deprecated (Feb 2026) and replaced by **Nimbalyst**, which supports Claude Code and Codex as primary engines (OpenCode, Copilot, Codex-via-ACP alpha) and authenticates through the existing CLIs/subscriptions — [stravu/crystal](https://github.com/stravu/crystal); [rywalker: Nimbalyst](https://rywalker.com/research/nimbalyst); [developersdigest](https://www.developersdigest.tech/blog/nimbalyst-visual-workspace-codex-claude-code)
- **Sculptor** (Imbue): launched as a UI that runs Claude Code agents in separate Docker containers; Codex support only claimed by a secondary review (unverified); macOS and Linux — [Show HN: Sculptor](https://hn.svelte.dev/item/45427697); [imbue.com](https://imbue.com/)
- **Omnara** (YC S25): launched as "Run Claude Code and Codex from anywhere" (mobile/web/Watch command center to monitor and steer live sessions); a 2026-01-28 company post labels the remote-coding product historical and repositions it as an open-source managed-agent platform (REST API/TS SDK/CLI, human approval for risky tool calls) — [RuntimeWire](https://runtimewire.com/article/startup-spotlight-omnara-agent-control-plane); [Launch HN summary](https://mewayz.com/cs/blog/launch-hn-omnara-yc-s25-run-claude-code-and-codex-from-anywhere); [openalternative](https://openalternative.co/omnara)
- **agent-deck** (asheshgoplani): terminal session manager/TUI for Claude, Gemini, OpenCode, Codex, Aider; per-session worktrees and forking; a "conductor" session that monitors other sessions and can itself run on Codex; v1.12.0 published 2026-08-14 — [asheshgoplani/agent-deck](https://github.com/asheshgoplani/agent-deck); [vibecodinghub review](https://vibecodinghub.org/blog/agent-deck-review)
- Same-name projects: matacoder/agent-deck (web panel over Tailscale for Claude Code/Codex sessions in tmux, working/waiting/done status, subscription-limit tracking) — [GitHub](https://github.com/matacoder/agent-deck); Fini240/agent-deck (Android + web dashboard to monitor and steer Claude Code and Codex on a Mac, needs tmux + Tailscale) — [GitHub](https://github.com/Fini240/agent-deck); arisros/tmux-agent-deck (shows blocked/done/running, PR adds Codex tracking) — [GitHub](https://github.com/arisros/tmux-agent-deck); AliceLJY/agentdeck (web terminal tabs for Claude Code, Codex, Kimi, Antigravity) — [GitHub](https://github.com/AliceLJY/agentdeck)
- **ntm** (Named Tmux Manager, Dicklesworthstone): Go CLI to spawn/tile/coordinate Claude, Codex, Gemini in tmux panes (`ntm spawn myproject --cc=2 --cod=1 --gmi=1`), broadcast prompts, robot mode for scripted sends, Agent Mail coordination, safety policy/approvals; latest release v1.13.1 — [Dicklesworthstone/ntm](https://github.com/Dicklesworthstone/ntm); [releases](https://github.com/Dicklesworthstone/ntm/releases)
- Others in 2026 comparisons: Superset (any CLI agent incl. Codex, Apache-2.0, macOS/Linux/Windows), Emdash (YC W26, 22+ agents), Parallel Code (Claude Code + Codex + Gemini side by side), Composio Agent Orchestrator, ccswarm (Claude only) — [rustman wiki](https://rustman.org/wiki/conductor-parallel-agents/); atmux (Driver → Team Lead → Members via tmux send-keys; Claude Code, OpenCode, Kimi, Cursor CLI) — [geoyws/atmux](https://github.com/geoyws/atmux)
- OpenAI Symphony connects Linear tickets to Codex sessions; since v1.1.0 also supports the Kata CLI runtime for Claude Code and Gemini — [Daniel Vaughan, 2026-05-01](https://codex.danielvaughan.com/2026/05/01/codex-cli-agent-interoperability-protocols-mcp-acp-a2a/); [OpenAI: Symphony](https://openai.com/index/open-source-codex-orchestration-symphony/)
- A vendor comparison notes that session managers/worktree apps (Claude Squad, Conductor, Crystal) keep each agent's context isolated, rating Vibe Kanban higher on inter-agent coordination but still as a board, not shared memory — [search summary of comparisons](https://munderdiffl.in/blog/best-claude-code-multi-agent-tools/) (vendor-authored, weigh accordingly)

### Inferences
- These tools are "human relay" orchestrators: you watch both agents and copy/feedback between them. For Claude-supervises-Codex, ntm (robot mode + Agent Mail) and agent-deck's conductor are closest, but both require running Codex as a TUI inside tmux, not in the desktop app.
- Conductor is the most polished Mac GUI that runs both, but there is no evidence it exposes an API for one agent to message the other or that it can adopt desktop-app threads.

### Gaps
- Conductor's `/docs/api` content (could enable external control) not retrieved.
- Exact last-commit dates for vibe-kanban, claude-squad, Sculptor not obtained; vibe-kanban's maintenance status is contradictory across sources.
- No source confirmed Sculptor's Codex support from Imbue itself.

## Key question 6: Simple patterns — tmux send-keys, shared files/inbox + AGENTS.md/CLAUDE.md, git handoff

### Takeaway
The lowest-friction working patterns are (1) file/artifact handoff (plan file → result file), which Daniel Vaughan calls the most reliable, and (2) tmux `send-keys` into a Codex TUI plus `capture-pane` to read back — universal but fire-and-forget with fragile screen parsing. Neither reaches a Codex desktop-app thread (unless combined with UI automation).

### Cited Findings
- Handoff patterns: artifact handoff (filesystem as shared state, no session IDs — "most reliable"); thread-ID relay with `codex`/`codex-reply`; summarised context injection into a fresh Codex prompt — [Daniel Vaughan, 2026-03-27](https://codex.danielvaughan.com/2026/03/27/claude-code-codex-mcp-in-practice/)
- tmux: `send-keys` is fire-and-forget (no exit code/ack; keystrokes can land in the wrong place if the pane is busy); `Enter`/`Escape`/`Tab` are key names, use `-l` for literal text; verify with `capture-pane -p` before the next input; `tmux wait-for` works for shell jobs, not reliably for TUIs; detached sessions default to 80x24 and inherit the creator's env/PATH; pros: no SDK, survives disconnects, works over SSH; cons: no structured output, fragile parsing, verify every step; for typed results prefer `claude --print --output-format json` — [mager.co, 2026-08-23](https://www.mager.co/blog/2026-08-23-orchestrating-agents-with-tmux/)
- One tmux orchestration project confirms delivery via `capture-pane` and retries up to 3 times; tmux teams can't tell you a member is blocked without extra status detection — [search summary: tmux orchestration guides](https://crystl.dev/guides/tmux-for-ai-agents/); [primeline-ai/claude-tmux-orchestration](https://github.com/primeline-ai/claude-tmux-orchestration)
- Agent Mail ships an AGENTS.md/CLAUDE.md blurb (inserted by a helper) so agents know to register and check mail, plus a pre-commit guard for reserved files — [mcp_agent_mail](https://github.com/Dicklesworthstone/mcp_agent_mail)

### Inferences
- Pros/cons summary for the report writer:
  - Shared files + AGENTS.md/CLAUDE.md rules: zero setup, works with the desktop apps (both can read/write the repo), auditable in git; but no notification — the human (or a polling loop/heartbeat) must prompt the agent to look.
  - Git-based handoff (branches/commits/PR comments): durable and reviewable, natural for worktree orchestrators; coarse-grained and slow for chat-style steering.
  - tmux send-keys into Codex TUI: real-time steering of a live session from Claude's Bash tool, no extra servers; fragile (busy panes, paste/Enter handling, TUI redraws), no delivery receipt, only for terminal sessions.
  - Codex app-server via `codex --remote` + JSON-RPC: structured alternative to send-keys for a live TUI thread (`turn/start`, `turn/steer`, streamed `item/*` notifications); experimental API, version churn.

### Gaps
- No source specifically documents Codex TUI quirks for send-keys (multi-line paste, Enter vs submit binding) as of the current Codex version.
- No source found comparing these simple patterns head-to-head with measured reliability.

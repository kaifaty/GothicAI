# Claude Code → OpenAI Codex: tools to delegate to, message, supervise or read Codex sessions (state as of 2026-10-15)

Context for the report writer. Two facts shape everything below:

1. **`codex mcp-server` no longer exists.** It was deprecated in codex-cli 0.149.1 (2026-08-24) and removed soon after. Sources disagree on the exact version (0.153.0 or 0.154.0). Every "register Codex as an MCP server in Claude Code" guide written before September 2026 is broken on the user's codex-cli 0.162.1.
2. **OpenAI's own answer is the official plugin `openai/codex-plugin-cc`.** It wraps `codex app-server` (JSON-RPC), not MCP.

On "can it reach a thread that is live in the Codex desktop app", the short answer is no, with one experimental exception (see the last section).

Local checks were run on the user's Mac on 2026-10-15. They are marked "(local check)". They found codex-cli 0.162.1 at `/opt/homebrew/bin/codex`, the plugin source cloned at v1.0.6, and GitHub stats pulled through the `gh` API.

## Is there an official OpenAI plugin for Claude Code (or an Anthropic-marketplace plugin wrapping Codex)? What does it do and how does it talk to Codex?

### Takeaway
Yes. OpenAI ships `openai/codex-plugin-cc` (Apache-2.0, about 34.2k stars, v1.0.6 of 2026-07-08). It adds the commands `/codex:review`, `/codex:adversarial-review`, `/codex:rescue`, `/codex:transfer`, `/codex:status`, `/codex:result`, `/codex:cancel` and `/codex:setup`, plus an optional Stop-hook review gate. It talks to Codex through a per-workspace broker that spawns its own `codex app-server` child over stdio. It can resume only its own "Codex Companion Task" threads, never an arbitrary or desktop thread. Anthropic's official marketplace has no Codex plugin.

### Cited Findings
- **Release and origin.** OpenAI released the official open-source Codex plugin for Claude Code around 2026-03-30/31. Install it with `/plugin marketplace add openai/codex-plugin-cc`, then `/plugin install codex@openai-codex`, `/reload-plugins` and `/codex:setup` — [The Decoder](https://the-decoder.com/openai-launches-a-codex-plugin-that-runs-inside-anthropics-claude-code/); [DEV Community](https://dev.to/terminalblog/openai-just-shipped-an-official-codex-plugin-for-claude-code-heres-what-it-does-2577)
- **OpenAI's own recommendation.** When it deprecated `codex mcp-server`, OpenAI said: "To use Codex from Claude Code, use the Codex plugin for Claude Code" — [Codex Releases on X](https://x.com/CodexReleases/status/2091929016194289782)
- **Commands, from the README:**
  - `/codex:review` is a read-only review of uncommitted changes, or of a branch with `--base <ref>`. It takes `--wait` or `--background` and no custom focus text.
  - `/codex:adversarial-review` is a steerable review that challenges design choices. It accepts focus text and does not fix code.
  - `/codex:rescue` hands a task to the `codex:codex-rescue` subagent. It takes `--background`, `--wait`, `--resume`, `--fresh`, `--model` and `--effort`; "spark" maps to `gpt-5.3-codex-spark`.
  - `/codex:transfer` turns the current Claude Code session into a persistent Codex thread and prints `codex resume <session-id>`. The thread "can be continued in the App or TUI". The source transcript must be under `~/.claude/projects`.
  - `/codex:status`, `/codex:result` (includes the Codex session ID) and `/codex:cancel` manage background jobs, which have IDs like `task-abc123`.
  - [openai/codex-plugin-cc README](https://github.com/openai/codex-plugin-cc)
- **Review gate.** A Stop hook runs a Codex review of Claude's response and blocks the stop if it finds issues. The README warns it "can create a long-running Claude/Codex loop and may drain usage limits quickly" — [README](https://raw.githubusercontent.com/openai/codex-plugin-cc/main/README.md)
- **Runtime and billing.** The plugin brings no runtime of its own. It uses the global `codex` binary, its config and its auth (ChatGPT plan, including Free, or an API key) and needs Node.js 18.18+. Usage counts against Codex limits — [README](https://github.com/openai/codex-plugin-cc)
- **Transport (verified in source).** `scripts/lib/app-server.mjs` runs `spawn("codex", ["app-server"], { stdio: ["pipe","pipe","pipe"] })`. A broker (`scripts/app-server-broker.mjs`) multiplexes plugin jobs onto that one child and returns "Shared Codex broker is busy" when it is occupied. It does not connect to the daemon control socket or to the desktop app's app-server — [app-server.mjs](https://github.com/openai/codex-plugin-cc/blob/main/plugins/codex/scripts/lib/app-server.mjs), [app-server-broker.mjs](https://github.com/openai/codex-plugin-cc/blob/main/plugins/codex/scripts/app-server-broker.mjs) (local check of commit db52e28)
- **Defaults (verified in source).** `approvalPolicy: "never"`, `sandbox: "read-only"`, `ephemeral: true` for reviews. Rescue tasks run `workspace-write` only with `--write` and persist their thread — [codex.mjs](https://github.com/openai/codex-plugin-cc/blob/main/plugins/codex/scripts/lib/codex.mjs); [codex-companion.mjs](https://github.com/openai/codex-plugin-cc/blob/main/plugins/codex/scripts/codex-companion.mjs) (local check)
- **Resume scope (verified in source).** `--resume` maps to `--resume-last`. `findLatestTaskThread()` calls `thread/list` with `sourceKinds: ["appServer"]` and `searchTerm: "Codex Companion Task"`, then picks the newest thread whose name starts with that prefix. No option accepts an arbitrary thread or session ID — [codex.mjs](https://github.com/openai/codex-plugin-cc/blob/main/plugins/codex/scripts/lib/codex.mjs); [agents/codex-rescue.md](https://github.com/openai/codex-plugin-cc/blob/main/plugins/codex/agents/codex-rescue.md) (local check)
- **Maturity.**
  - About 34.2k stars, 2.4k forks, 283 open issues and 251 PRs — [GitHub page](https://github.com/openai/codex-plugin-cc)
  - Releases: v1.0.4 (2026-04-18), v1.0.5 (2026-06-23, adds `/codex:transfer`) and v1.0.6 (2026-07-08). The last commit is 2026-07-07 and the repo had not been pushed since 2026-07-08 as of 2026-10-15 — `gh api repos/openai/codex-plugin-cc/releases` (local check)
- **Anthropic's marketplace.** The official Anthropic marketplace `anthropics/claude-plugins-official` has no Codex, OpenAI or GPT entry. Its 804-file tree and `marketplace.json` contain no "codex" match — `gh api repos/anthropics/claude-plugins-official/git/trees/main` (local check); background on the marketplace: [Claude Code docs: discover plugins](https://code.claude.com/docs/en/discover-plugins)

### Catalogue at a glance (Claude Code → Codex)
Notes on the columns:
- **Mode**: N = starts a new Codex task, C = continues or steers an existing thread, R = only reads results.
- **Stars and last push** come from `gh api` on 2026-10-15 (local check) unless noted.

| Tool | Kind | Mechanism | Mode | Stars / last push | Install | Status on codex 0.162 |
|---|---|---|---|---|---|---|
| [openai/codex-plugin-cc](https://github.com/openai/codex-plugin-cc) | Official plugin: commands, subagent, Stop hook | Own `codex app-server` via stdio broker | N; C only for its own latest "Codex Companion Task" thread; R via `/codex:result` | 34.2k / 2026-07-08 (v1.0.6) | `/plugin marketplace add openai/codex-plugin-cc` | Works; known broker and auth bugs (below) |
| [skills-directory/skill-codex](https://github.com/skills-directory/skill-codex) | Skill, also packaged as a plugin | `codex exec ... --sandbox read-only` | N (resume is listed in the description but not shown in the README) | 1.46k / 2026-09-13 | `/plugin marketplace add skills-directory/skill-codex` | Likely works (exec-based) |
| [cathrynlavery/codex-skill](https://github.com/cathrynlavery/codex-skill) | Skill + hooks | Codex reviews every Claude plan through hooks, plus a manual `/codex` | N | 219 / 2026-09-20 | Repo | Not verified |
| [dsaad68/second-opinion](https://github.com/dsaad68/second-opinion) | Skill | Read-only Codex consult, side-by-side synthesis | N | 1 / 2026-09-27 | Repo | Not verified |
| Aseem Shrey `/codex-review` ([blog](https://aseemshrey.com/blog/claude-codex-iterative-plan-review/)) | Skill or command | `codex exec` then `codex exec resume <session-id>` loop until Codex approves | N + C (own session) | Blog | Copy from blog | Exec-based |
| [tuannvm/codex-mcp-server](https://github.com/tuannvm/codex-mcp-server) | MCP server (npm `codex-mcp-server`) | Wraps the Codex CLI; tools `codex`, `review`, `websearch`, `listSessions`, `ping`, `help` | N + C (sessionId) | 643 / 2026-05-25, no license | `claude mcp add codex-cli -- npx -y codex-mcp-server` | Not verified after mcp-server removal; README needs an API key |
| [BeehiveInnovations/pal-mcp-server](https://github.com/BeehiveInnovations/pal-mcp-server) (formerly zen-mcp-server), tool `clink` | Multi-model MCP | `codex exec --json --dangerously-bypass-approvals-and-sandbox` | N (+ PAL's own `continuation_id`) | 11.8k / **2025-12-15 (stale, about 10 months)** | MCP | Exec-based, unmaintained |
| [abetoots/codex-app-mcp](https://github.com/abetoots/codex-app-mcp) | MCP replacement for the removed server | Spawns `codex app-server` over stdio and re-exposes `codex` and `codex-reply(threadId)` | N + C (threadId) | 0 / 2026-10-14 | Build, then `claude mcp add --scope user codex -- node dist/index.js` | Tested with 0.154.0; brand new |
| [mkXultra/ai-cli-mcp](https://github.com/mkXultra/claude-code-mcp/) | MCP | Runs Claude, Codex and Gemini CLIs asynchronously in parallel | N | 27 / 2026-09-30 | MCP | Not verified |
| [alanwilhelm/ctrl](https://github.com/alanwilhelm/ctrl) | CLI + skill (for Hermes and Codex) | WebSocket JSON-RPC on `~/.codex/app-server-control/app-server-control.sock` | N + C + R (`list`, `read`, `spawn`, `send`) | 0 / 2026-08-04 | Python 3.11+ | Linux only, tested on 0.146.0 |
| Native CLI (no wrapper) | Shell from a Claude Bash tool, subagent or skill | `codex exec --json` / `codex exec resume <id>` / `codex queue --thread --message` / `codex --remote unix:// resume <id>` | N + C + R | — | Built in | Present in 0.162.1 (local check) |

## Community MCP servers wrapping Codex for Claude Code: which still work, and which support resuming a session ID and multi-turn conversations?

### Takeaway
The classic route was `claude mcp add codex -- codex mcp-server`, using its `codex` and `codex-reply` tools. It is dead on codex-cli 0.153/0.154 and later, including the user's 0.162.1.

What remains:
- **Exec-based wrappers** (tuannvm, PAL clink, ai-cli-mcp). They still shell out to `codex exec`, but most were not re-verified after the removal, and PAL has been unmaintained since 2025-12.
- **abetoots/codex-app-mcp**, a new zero-star shim that restores `codex` and `codex-reply(threadId)` on top of `codex app-server`.

Multi-turn support: tuannvm (`sessionId`) and codex-app-mcp (`threadId`) offer native multi-turn. PAL's `continuation_id` is PAL's own conversation memory.

### Cited Findings
- **Deprecation and removal.**
  - Codex CLI v0.149.1 (2026-08-24) deprecated `codex mcp-server`. App-server is the replacement, and the Claude Code plugin is the path for Claude Code users — [Daniel Vaughan, 2026-08-25](https://codex.danielvaughan.com/2026/08/25/codex-mcp-server-deprecated-app-server-migration-claude-code-plugin-v0149/); [Codex Releases on X](https://x.com/CodexReleases/status/2091929016194289782)
  - A later release lists as a breaking change: "The deprecated codex mcp-server entry point is no longer available" — [Codex Releases on X](https://x.com/CodexReleases/status/2097817398040551429)
  - The removal version is disputed: 0.153.0 (PR #42993) per [abetoots/codex-app-mcp](https://github.com/abetoots/codex-app-mcp), contradicted by a changelog guide that lists 0.154.0 (via search summary of [gradually.ai changelog](https://www.gradually.ai/en/changelogs/codex-cli/)).
  - On 0.162.1, `codex mcp-server --help` just prints the top-level "Codex CLI" help, so the subcommand is gone (local check).
- **App-server is not MCP.** It speaks its own JSON-RPC protocol and is not a drop-in replacement for MCP tool calls — search summary of the [OpenAI migration page](https://learn.chatgpt.com/docs/mcp-server) and [Daniel Vaughan](https://codex.danielvaughan.com/2026/08/25/codex-mcp-server-deprecated-app-server-migration-claude-code-plugin-v0149/)
- **Historical threadId bug.** Before the removal, `codex()` did not return the conversation ID that `codex-reply()` needed. This was tracked in issues #3712, #4651, #5660, #8388 and #8580 (Sep–Dec 2025). It was fixed by returning `structuredContent.threadId` (commit "add threadId to MCP server messages #9192"). The workaround was parsing `~/.codex/sessions/YYYY/MM/DD/rollout-*.jsonl` — [Daniel Vaughan, 2026-03-27](https://codex.danielvaughan.com/2026/03/27/claude-code-codex-mcp-in-practice/); [forge mirror of commit](https://forge.lthn.ai/core/core-agent-ide/commit/0c09dc3c033eadb76a64d61b368972e11eb90fbd)
- **tuannvm/codex-mcp-server.**
  - Tools are `codex` (with sessions), `review`, `websearch`, `listSessions`, `ping` and `help`. Passing `sessionId` creates a session or resumes context, and `threadId` is returned in `structuredContent`.
  - Requires Codex CLI v0.75.0+ and an API key in the README setup (`codex login --api-key`). Sessions are scoped to one server instance. There is no license file.
  - [GitHub](https://github.com/tuannvm/codex-mcp-server)
  - Earlier docs: it uses native `codex resume` on v0.50.0+, otherwise rebuilds context, and caps sessions at 100 with a 24 h expiry — [Daniel Vaughan](https://codex.danielvaughan.com/2026/03/27/claude-code-codex-mcp-in-practice/); [session docs](https://docs.tuannvm.com/codex-mcp-server/docs/session-management)
- **PAL MCP `clink`.**
  - The Codex preset runs `codex exec --json --dangerously-bypass-approvals-and-sandbox`. Roles are `default`, `planner` and `codereviewer`. `continuation_id` continues clink conversations. The docs warn that the flags bypass each CLI's safety prompts — [clink docs](https://github.com/BeehiveInnovations/pal-mcp-server/blob/main/docs/tools/clink.md)
  - Last push 2025-12-15 with 11.8k stars, so it is effectively unmaintained as of Oct 2026 (local check via `gh api`).
- **abetoots/codex-app-mcp.**
  - A thin stdio MCP server that spawns `codex app-server` and re-exposes `codex` (new thread) and `codex-reply` (`threadId` + `prompt`).
  - It auto-declines approval requests, rejects `profile`, and maps the `on-failure` approval policy to `on-request`. The author warns of protocol drift because app-server is "experimental" and recommends a smoke test after each `codex update`.
  - Tested with 0.154.0. 0 stars, last push 2026-10-14.
  - [GitHub](https://github.com/abetoots/codex-app-mcp)
- **Other bridges named in a March 2026 survey:**
  - `claude-codex-bridge` (npm): symmetric, with `BRIDGE_DEPTH` blocking chains of depth 2 or more.
  - `abhishekgahlot2/codex-claude-bridge`: Codex → Claude push via Claude Code Channels, with a web UI. This is the opposite direction.
  - `mkXultra/ai-cli-mcp`: runs CLIs in parallel.
  - The same survey recommends "artifact handoff" (Claude writes PLAN.md, Codex writes RESULT.md) for one-shot tasks.
  - [Daniel Vaughan, 2026-03-27](https://codex.danielvaughan.com/2026/03/27/claude-code-codex-mcp-in-practice/)
- **Statelessness of MCP delegation.** MCP passes only a string prompt, so Claude's session memory, open files and tool history are not carried over — [Daniel Vaughan](https://codex.danielvaughan.com/2026/03/27/claude-code-codex-mcp-in-practice/)

### Inferences
- **Do not use pre-September-2026 guides.** Any guide with `claude mcp add codex -- codex mcp-server`, or a `.mcp.json` that launches `codex mcp-server`, will fail on 0.162.1.
- **For an MCP-shaped multi-turn interface**, codex-app-mcp is the only one built on the current official integration point, but it has 0 stars. The official plugin plus native `codex exec resume` is the lower-risk path.
- **Exec-based servers should keep working mechanically**, but none of them was confirmed against 0.16x in the sources found.

### Gaps
- No source confirms that tuannvm/codex-mcp-server or PAL `clink` work on codex-cli 0.16x. Their GitHub pages do not say whether tuannvm uses `codex exec` or the removed `codex mcp-server` internally.
- `claude-codex-bridge` (npm) was not inspected; only the secondhand description exists.
- Whether PAL's `continuation_id` maps to native Codex `exec resume` is undocumented.

## Claude Code skills or subagents that drive `codex exec --json` / `codex exec resume` in the background and parse its JSONL

### Takeaway
No single canonical skill exists. Several community skills shell out to `codex exec`; the most popular is skill-codex with 1.46k stars. Of the skills found, only Aseem Shrey's review loop documents using `codex exec resume <session-id>` for multi-round dialogue.

The official plugin's `codex:codex-rescue` subagent with `--background` is the best-supported "background delegate + poll status + fetch result" flow. It uses app-server events rather than JSONL.

The native CLI in 0.162.1 provides every building block: `exec --json`, `--ephemeral`, `--output-schema`, `--ignore-user-config`, `exec resume [SESSION_ID|name] [PROMPT]` with `--last`/`--all`, and `codex queue --thread --message`.

### Cited Findings
- **skills-directory/skill-codex** runs `codex exec -m <model> --config model_reasoning_effort=... --sandbox read-only --full-auto --skip-git-repo-check "<prompt>" 2>/dev/null`. stderr, which carries the thinking tokens, is dropped to save context. Claude summarizes Codex's output and asks whether to continue. Install with `/plugin marketplace add skills-directory/skill-codex`, then `/plugin install skill-codex@skill-codex`. 1.5k stars, MIT — [GitHub](https://github.com/skills-directory/skill-codex)
- **Aseem Shrey's `/codex-review`** sends Claude's implementation plans to Codex and loops until Codex approves, using `codex exec resume <session-id>` to keep context. It "typically" converges in 2–3 rounds — [blog](https://aseemshrey.com/blog/claude-codex-iterative-plan-review/)
- **cathrynlavery/codex-skill** has hooks make Codex review every plan Claude creates before the user approves it, plus a manual `/codex` — [GitHub](https://github.com/cathrynlavery/codex-skill)
- **dsaad68/second-opinion** has Claude form its own opinion first, then ask Codex read-only, and present both side by side — [GitHub](https://github.com/dsaad68/second-opinion)
- **Sandbox design pitfall** (nirux PR). `-s read-only` sandboxes only shell commands, so MCP servers and connectors still run. The PR adds `--ignore-user-config --disable apps`, plus `--ephemeral` so second-opinion runs do not clutter `codex resume` history — [xikimay/nirux PR #90](https://github.com/xikimay/nirux/pull/90)
- **Native building blocks on 0.162.1** (local check):
  - `codex exec resume [OPTIONS] [SESSION_ID] [PROMPT]` takes a UUID or thread name, with `--last` and `--all`.
  - `codex exec` has `--json`, `--ephemeral`, `--ignore-user-config`, `--output-schema` and `--skip-git-repo-check`.
  - `codex queue --thread <UUID|name> --message <TEXT> [--remote <ADDR>]` is described as "Queue a message for an existing session".
  - `codex agents` is described as "Browse all agent sessions on the shared local app-server daemon".
  - `codex app-server {daemon,proxy,generate-ts,generate-json-schema}`.
  - v0.149 itself added the agents dashboard and `codex queue` — [search summary of v0.149 notes](https://github.com/openai/codex/releases/tag/rust-v0.149.0)

### Inferences
- **DIY background delegation.** Run `codex exec --json` in a background Bash task and capture `thread.started`/thread id from the first JSONL event. Follow up with `codex exec resume <id> "<prompt>"`. This gives multi-turn delegation without any third-party code.
- **Thread naming.** `exec resume` also accepts a thread name, which would let a skill use stable names instead of parsing IDs. This is an inference from the help text and was not tested.

### Gaps
- No skill was found that explicitly parses `codex exec --json` JSONL events, such as streaming progress into Claude. The JSONL event schema for 0.162 was not fetched.
- The skill-codex `SKILL.md` was not read, so whether it actually issues `exec resume` is unconfirmed.

## Real-world write-ups: Claude Code as orchestrator or reviewer of Codex — what works, and the pitfalls

### Takeaway
The dominant pattern is "Claude builds, Codex reviews": a second pass by a different model family, either a plan review loop or a post-implementation review. The usual tools are `/codex:review`/`/codex:adversarial-review` or a second-opinion skill, with heavier work handed over via `/codex:rescue --background`.

The documented pitfalls are mostly operational:
- leaked or duplicated app-server brokers;
- several app-servers sharing one `~/.codex`, which races and burns the single-use ChatGPT refresh token, logging every client out;
- the Desktop app holding thread writer locks;
- review-gate loops draining limits;
- `--dangerously-bypass` sandboxes in third-party wrappers.

First-hand Reddit or Hacker News threads did not surface in searches. The evidence is blogs and GitHub issues.

### Cited Findings
- **The "Claude builds, Codex reviews" habit.** One author runs Claude Code's finished work through Codex for a review-only second pass. They say they picked the habit up from Reddit and that Claude "tends to ship quickly" and lets edge cases through — [Pablo Stanley, Substack](https://pablostanley.substack.com/p/a-heated-rivalry-claude-code-openai)
- **Cross-provider framing.** The value is a different model catching what the writing model missed ("grade their own homework"). This source is vendor-adjacent — [MindStudio](https://www.mindstudio.ai/blog/openai-codex-plugin-claude-code-cross-provider-review)
- **Plan-review loop.** Claude and Codex iterate on a plan via `exec resume` until Codex approves, typically in 2–3 rounds — [Aseem Shrey](https://aseemshrey.com/blog/claude-codex-iterative-plan-review/)
- **Handoff patterns.** Artifact handoff (PLAN.md and RESULT.md), threadId relay, and summarized context injection. Timeouts in the example configs are `startup_timeout_sec` 20–30 and `tool_timeout_sec` 180–300 — [Daniel Vaughan](https://codex.danielvaughan.com/2026/03/27/claude-code-codex-mcp-in-practice/)
- **Plugin pitfalls, open issues in `openai/codex-plugin-cc`** (fix status not visible in the sources):
  - #382: one app-server per workspace, all sharing `~/.codex`, which "is not safe for concurrent multi-process writers" — [issue #382](https://github.com/openai/codex-plugin-cc/issues/382)
  - #789: long-lived brokers burn the single-use ChatGPT refresh token (`refresh_token_reused`) and log out every client of `~/.codex` — [issue #789](https://github.com/openai/codex-plugin-cc/issues/789)
  - #706: the broker never sends `thread/unsubscribe`, so finished threads and their MCP runtimes stay loaded until the 30-minute idle timeout — [issue #706](https://github.com/openai/codex-plugin-cc/issues/706)
  - #108: the broker is not cleaned up on session exit and has no idle timeout — [issue #108](https://github.com/openai/codex-plugin-cc/issues/108)
  - #718 and #782: Windows broker leaks — [issue #718](https://github.com/openai/codex-plugin-cc/issues/718), [issue #782](https://github.com/openai/codex-plugin-cc/issues/782)
- **Broker accumulation in long sessions.** On 2026-10-10, 14 brokers and 14 app-servers were alive, some for more than 3 days, because desktop sessions rarely close — [FrankQDWang/StoryOS issue #1093](https://github.com/FrankQDWang/StoryOS/issues/1093)
- **Review gate cost.** The review gate can loop and drain usage limits; enable it only while monitoring — [README](https://github.com/openai/codex-plugin-cc)
- **Auth and billing.** Plugin usage bills to the OpenAI account (ChatGPT plan limits or API), not the Anthropic subscription — [The Decoder](https://the-decoder.com/openai-launches-a-codex-plugin-that-runs-inside-anthropics-claude-code/); [README](https://github.com/openai/codex-plugin-cc)
- **Sandbox and safety.**
  - PAL clink runs Codex with `--dangerously-bypass-approvals-and-sandbox` — [clink docs](https://github.com/BeehiveInnovations/pal-mcp-server/blob/main/docs/tools/clink.md)
  - ctrl starts turns with `approvalPolicy: never` and `dangerFullAccess` — [ctrl](https://github.com/alanwilhelm/ctrl)
  - The official plugin defaults to read-only + never-approve and only writes with `--write` — [codex.mjs](https://github.com/openai/codex-plugin-cc/blob/main/plugins/codex/scripts/lib/codex.mjs) (local check)
- **Loop depth.** `claude-codex-bridge` blocks Claude→Codex→Claude chains at depth 2 or more — [Daniel Vaughan](https://codex.danielvaughan.com/2026/03/27/claude-code-codex-mcp-in-practice/)
- **Project config trust.** Project `.codex/config.toml` overrides load only for trusted projects — [README](https://github.com/openai/codex-plugin-cc)

### Inferences
- **Several Claude Code desktop sessions and the Codex desktop app share one `~/.codex`.** On this Mac that means several plugin brokers plus the Codex app. Expect the #382/#789 class of problems: random Codex logouts, and resume failures from writer locks.
- **Isolate `CODEX_HOME` for automation.** A separate `CODEX_HOME` with its own copy of `auth.json` avoids this for automation, at the cost of those threads not appearing in the Codex app sidebar.

### Gaps
- No Reddit (r/ClaudeAI, r/ChatGPTCoding) or Hacker News first-hand threads were retrieved; searches returned only blogs.
- Whether issues #382, #789 and #706 are fixed is unknown. The plugin has had no release since v1.0.6 (2026-07-08).
- No systematic comparison of Codex review quality was found.

## Can any of these reach a thread that is live in the Codex desktop app (ChatGPT.app)?

### Takeaway
Not with any packaged tool in its default setup.

- **The official plugin** only creates and resumes its own app-server threads in a separate `codex app-server` child. Its transferred threads can later be opened in the App, but that is the plugin → App direction only.
- **`codex exec resume <id>`** on a thread the Desktop app has loaded fails with "already has an active writer (code -32600)", because the Desktop's app-server holds the writer lock even when idle.
- **The only working route** is experimental and unofficial: make ChatGPT.app and the CLI share one managed app-server daemon by launching the app with `CODEX_APP_SERVER_WS_URL=ws+unix://…/app-server-control.sock:/rpc`. Clients attached to that daemon can then open, queue into or steer the same live thread. Those clients are `codex --remote unix:// resume <id>`, probably `codex queue --remote unix://`, `ctrl`, or a custom JSON-RPC `turn/steer` client.
- **On this Mac today** no daemon control socket exists, so the Desktop app runs its own private app-server.

### Cited Findings
- **Writer-lock conflict.** On macOS with codex-cli 0.153.4 bundled with the ChatGPT desktop app (opened 2026-09-26), `codex exec resume` failed with "thread/resume failed: thread … already has an active writer (code -32600)". The Desktop app-server held the lock even while idle. The reported workaround is a separate `CODEX_HOME` with its own `config.toml` and a copy of `auth.json`, which hides those conversations from the Desktop sidebar — [chenhg5/cc-connect issue #1909](https://github.com/chenhg5/cc-connect/issues/1909)
- **The official plugin cannot target an existing desktop thread (verified in source):**
  - it spawns its own `codex app-server` over stdio;
  - resume picks only `sourceKinds: ["appServer"]` threads named "Codex Companion Task…";
  - no flag accepts an arbitrary thread ID.
  - The README speaks only of threads that "can be continued in the App or TUI" via `codex resume`/`/codex:transfer` — [codex.mjs](https://github.com/openai/codex-plugin-cc/blob/main/plugins/codex/scripts/lib/codex.mjs); [README](https://github.com/openai/codex-plugin-cc)
- **Sharing one daemon between Desktop and CLI (experimental gist).**
  - Launch with `open -a /Applications/ChatGPT.app --env "CODEX_APP_SERVER_WS_URL=ws+unix://localhost$HOME/.codex/app-server-control/app-server-control.sock:/rpc"` after `codex app-server daemon start`. The app then uses the daemon instead of its own child app-server.
  - `codex --remote unix:// resume THREAD_ID` attaches the TUI to the same running chat.
  - Tested 2026-10-06 on macOS 27.0.1 with ChatGPT desktop 26.930.61225, CLI/daemon 0.160.1 and daemon 0.162.0.
  - Caveats: "an experimental workaround, not an officially supported desktop setting". Only read-only scripted RPC was tested; scripted steering was not. Daemon updates restart the backend for all clients.
  - [ale-rinaldi gist](https://gist.github.com/ale-rinaldi/80f063b60177f1281e219ebd68587a4d)
- **App-server API for supervision.**
  - `thread/start`, `thread/resume` (reopen by ID), `thread/list`, `thread/read` (stored metadata without resuming), `turn/start`, `turn/steer` (add input to an active turn) and `turn/interrupt`.
  - Transports are stdio (default), WebSocket (experimental) and Unix-socket WebSocket on the default control socket.
  - A thread stays loaded "until it has no subscribers and no thread activity for 30 minutes".
  - The docs do not say whether a thread loaded in a different app-server process can be resumed concurrently.
  - [Codex app-server docs](https://learn.chatgpt.com/docs/app-server)
- **Conflicting reports on the Desktop app and the default socket.** One write-up says the Desktop app uses its own child-process connection registry rather than the default daemon socket. Others say the socket is present whenever the daemon runs. Treat this as version-dependent — search summary of [zenn.dev Codex App Server study](https://zenn.dev/suwash/articles/codex-app-server_20260514) and [taskq issue #161](https://github.com/alexkirs/taskq/issues/161)
- **ctrl** can `list`, `read`, `spawn` and `send` (steer an active turn or start a new one) on threads of the daemon at `~/.codex/app-server-control/app-server-control.sock`. It is Linux-only, tested on 0.146.0, with 0 stars — [alanwilhelm/ctrl](https://github.com/alanwilhelm/ctrl)
- **macOS socket-path pitfall.** A long `CODEX_HOME` can push the control-socket path past the macOS `SUN_LEN` limit — [multica issue #4041](https://github.com/multica-ai/multica/issues/4041)
- **This Mac today** (local check, 2026-10-15): `~/.codex/app-server-control/` does not exist, so no daemon socket. codex-cli is 0.162.1 and has `codex queue --remote`, `codex agents` ("shared local app-server daemon") and `codex app-server daemon start|stop|version|…`.

### Inferences
- **Reading desktop threads** works without the shared daemon: rollout files and `thread/read`/`thread/list` from any app-server read stored threads. Writing a new turn into a thread the Desktop app has loaded will hit the writer lock.
- **The only clean "message a live desktop thread" path** is to run the Desktop app on the shared daemon (gist setup) and drive it with `codex queue --remote unix:// --thread <id> --message …`, or with `turn/steer` over the socket from a Claude Code skill or Bash. This is unofficial and fragile across ChatGPT.app updates.
- **`codex queue` behaviour is unclear.** Without the shared daemon, `codex queue` presumably goes to its own app-server and may hit the same writer lock. This was not tested.

### Gaps
- Not tested: `codex queue` against a thread open in ChatGPT.app, with or without the shared daemon.
- No official OpenAI statement was found on whether ChatGPT.app supports external clients attaching to its threads; the gist's env override is internal.
- The truncated tail of the app-server docs (warnings and concurrency) was not read.

// A stand-in core for working on the screens in a plain browser (`bun run dev`, no Tauri): answers the
// requests with made-up data and plays a session's events. `?scenario=fresh|failed` picks the start;
// `update`, `update-unbuilt`, `update-failed`, `update-manual`, `updated`, `failed-update` show the Ryzen
// install's update.
import { mockIPC } from "@tauri-apps/api/mocks";
import { emit } from "@tauri-apps/api/event";
import type { JournalEntry, ServiceEvent, Session, State, Update } from "./core";
import schema from "./schema.mock.json"; // `gothic-launcher` settings models, dumped; may lag behind them

const scenario = new URLSearchParams(location.search).get("scenario") ?? "ready";
const now = () => Date.now() / 1000;

const state: State = {
  configured: scenario !== "fresh",
  problems: scenario === "fresh" ? ["game folder is not set: gothic-launcher config set game.dir '/path/to/Gothic II'"] : [],
  settings: {
    game: { dir: scenario === "fresh" ? null : "/Games/Gothic II", dev_mode: false, chat_scale: null },
    llm: { backend: "chatgpt", model: null, base_url: null, reasoning_effort: "low" },
    voice: { enabled: true, provider: "inworld", fallback: "chatgpt", voxtral_url: "http://gpu-server:47830", mic_device: null },
    tts: { enabled: true, provider: "inworld", fallback: "none", voxcpm_url: "ws://gpu-server:47831", loudness: null },
    paths: { repo: null, uv: null, game_bin: null, game_home: null },
  },
  secrets: { openai_api_key: false, inworld_api_key: true },
  signed_in: true,
  last_save: scenario === "fresh" ? null : { slot: "1", time: now() - 3600 },
  session: null,
  journal: [],
  paths: { config: "/Users/me/.config/gothic-ai", logs: "/Users/me/Library/Logs/GothicAI", game_home: "/Users/me/.local/share/gothic-ai/game" },
};

const minutesAgo = (m: number) => new Date(Date.now() - m * 60_000).toISOString();
const update: Update | null = ["update", "update-unbuilt", "update-failed", "update-manual", "failed-update"].includes(scenario)
  ? {
      current: false,
      behind: scenario === "update-unbuilt" ? 0 : 24,
      built: scenario !== "update-unbuilt",
      latest_failed: scenario === "update-failed",
      head: "f5fb1cf5a2d4e6b8c0d2e4f6a8b0c2d4e6f8a0b2",
      target: "685c693e0b1d3f5a7c9e1b3d5f7a9c1e3b5d7f9a",
      changes:
        scenario === "update-unbuilt"
          ? []
          : [
              { pr: 723, title: "docs: решения по оставшимся дефектам e2e" },
              { pr: 720, title: "npc memory: замер поиска recall против BM25 и эмбеддингов" },
              { pr: 721, title: "Живая итоговая карта: случай fate_card в live_director" },
            ],
      report: { time: minutesAgo(9), result: "busy" },
      auto: scenario !== "update-manual",
      updating: null,
    }
  : scenario === "updated"
    ? {
        current: true, behind: 0, built: true, latest_failed: false, head: "685c693e0b1d", target: "685c693e0b1d",
        changes: [], report: { time: minutesAgo(0), result: "updated" }, auto: true, updating: null,
      }
    : null;
if (update?.latest_failed) Object.assign(update, { current: true, behind: 0, changes: [], report: { time: minutesAgo(4), result: "build_failed_rolled_back" } });

async function runUpdate(u: Update) {
  const lines = [
    "gq: waiting for admission (Gothic AI install/update, quick, 12 threads)",
    "+ git -C /home/kaifaty/gothic-ai/play reset --hard 685c693e0b1d",
    "+ cmake --build /home/kaifaty/gothic-ai/play/OpenGothic/build --target Gothic2Notr --parallel 12",
    "[412/1240] Building CXX object opengothic/CMakeFiles/Gothic2Notr.dir/game/world/objects/npc.cpp.o",
    "+ /home/kaifaty/.local/bin/uv sync --locked --project /home/kaifaty/gothic-ai/play/server",
    "+ /home/kaifaty/gothic-ai/play/scripts/build-launcher.sh --no-install",
  ];
  for (const detail of lines) {
    u.updating = { state: "running", detail, started: u.updating!.started, result: null, error: null };
    await emit("core", { event: "update", data: structuredClone(u) });
    await sleep(1400);
  }
  u.updating = { state: "finished", detail: "", started: u.updating!.started, result: "updated", error: null };
  await emit("core", { event: "update", data: structuredClone(u) });
}

state.update = update;

if (scenario.startsWith("failed")) {
  state.session = {
    state: "stopped",
    mode: "continue",
    save: "1",
    started: now() - 20,
    ended: now() - 18,
    log_dir: "/Users/me/Library/Logs/GothicAI/2026-10-11_18-21-04",
    services: {},
    failed: {
      proc: "asr",
      detail:
        "exited with code 2 before it was ready\n    --- 2026-10-11 18:21:04 uv run --project asr-service gothic-asr serve --provider voxtrall\n    error: unknown provider 'voxtrall', known: inworld, voxtral, chatgpt, openai, fake"
        + (scenario === "failed-update" ? Array.from({ length: 14 }, (_, i) => `\n    traceback line ${i + 1}`).join("") : ""),
    },
    code: 1,
  };
}

const providers = {
  brain: { enabled: true, providers: [{ id: "chatgpt", ok: true, title: "Подписка ChatGPT", detail: "Вход выполнен" }] },
  voice: {
    enabled: true,
    providers: [
      { id: "inworld", ok: true, title: "Inworld", detail: "Доступен" },
      { id: "chatgpt", ok: true, title: "Подписка ChatGPT", detail: "Вход выполнен" },
    ],
  },
  speech: {
    enabled: true,
    providers: [
      { id: "inworld", ok: true, title: "Inworld", detail: "Доступен" },
      { id: "voxcpm", ok: false, title: "VoxCPM2 на своём сервере", detail: "Не отвечает: ConnectError" },
    ],
  },
};

const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));
const service = (data: ServiceEvent) => emit("core", { event: "service", data });
const journal = (data: Omit<JournalEntry, "t">) => emit("core", { event: "journal", data: { t: now(), ...data } });

async function play(session: Session) {
  await emit("core", { event: "session", data: session });
  for (const proc of ["asr", "tts", "server"]) await service({ proc, kind: "starting", detail: "", data: null });
  await sleep(700);
  await service({ proc: "asr", kind: "ready", detail: "in 1.8s", data: { ok: true, providers: [{ name: "inworld" }, { name: "chatgpt" }] } });
  await service({ proc: "tts", kind: "ready", detail: "in 1.2s", data: { ok: true, providers: [{ name: "inworld", ok: true }, { name: "voxcpm", ok: true }] } });
  await service({ proc: "server", kind: "ready", detail: "in 1.1s", data: { ok: true, game: false } });
  await service({ proc: "game", kind: "starting", detail: "", data: null });
  await sleep(1500);
  await service({ proc: "server", kind: "status", detail: "", data: { ok: true, game: true } });
  await sleep(1200);
  await journal({ kind: "player", npc: "NONE_100_XARDAS", name: "Ксардас", text: "Привет, Ксардас.", via: "voice", asr: "inworld", latency_s: 0.49 });
  await sleep(900);
  await journal({ kind: "npc", npc: "NONE_100_XARDAS", name: "Ксардас", text: "Придержи язык.", llm: "chatgpt", first_token_s: 3.13, total_s: 5.06 });
  await sleep(900);
  await service({ proc: "tts", kind: "status", detail: "", data: { ok: true, providers: [{ name: "inworld", ok: false }, { name: "voxcpm", ok: true }] } });
}

export function installMock(): void {
  mockIPC(
    async (cmd, args) => {
      if (cmd === "restart_app") location.search = "?scenario=updated";
      if (cmd !== "core_call") return null; // window commands: nothing to do in a browser
      const { method, params } = args as { method: string; params: Record<string, unknown> };
      switch (method) {
        case "state":
          return structuredClone(state);
        case "schema":
          return schema;
        case "check.providers":
          await sleep(600);
          return providers;
        case "check.game":
          return state.settings.game.dir
            ? [
                { id: "archives", ok: true, title: "Архивы Gothic II", detail: "Anims.vdf, Meshes.vdf, Textures.vdf и ещё 18" },
                { id: "addon", ok: true, title: "Аддон «Ночь Ворона»", detail: "Anims_Addon.vdf, Speech_Addon.vdf и ещё 4" },
                { id: "russian", ok: true, title: "Русская локализация", detail: "Реплики NPC на русском" },
              ]
            : [{ id: "folder", ok: false, title: "Папка игры", detail: "Не выбрана" }];
        case "settings.set": {
          const [section, field] = String(params.key).split(".");
          if (section === "secrets") state.secrets[field] = Boolean(params.value);
          else (state.settings as Record<string, Record<string, unknown>>)[section][field] = params.value;
          if (params.key === "game.dir") {
            state.problems = [];
            state.configured = true;
          }
          return structuredClone(state);
        }
        case "mics":
          return [{ index: 0, name: "Микрофон MacBook Pro", default: true }];
        case "find.game":
          await sleep(400);
          return scenario === "fresh"
            ? [
                {
                  path: "/Users/me/Library/Application Support/Steam/steamapps/common/Gothic II",
                  ok: true,
                  checks: [{ id: "archives", ok: true, title: "Архивы Gothic II", detail: "Worlds.vdf" }],
                },
              ]
            : [];
        case "test.voice":
          for (let i = 0; i < 30; i++) {
            await emit("core", { event: "voice_test", data: { level: Math.abs(Math.sin(i / 3)) * 0.8 } });
            if (i === 15) await emit("core", { event: "voice_test", data: { partial: "Привет" } });
            await sleep(100);
          }
          return { text: "Привет, Ксардас.", provider: "inworld", audio_s: 4, latency_s: 0.49 };
        case "test.speech":
          await sleep(1800);
          return { text: "Я ждал тебя. Нам нужно поговорить.", provider: "inworld", first_chunk_s: 0.26, audio_s: 2.4, latency_s: 0.9 };
        case "test.llm":
          await sleep(1500);
          return { text: "Придержи язык.", llm: "chatgpt", model: "gpt-6.1-sol", first_token_s: 3.13, total_s: 5.06 };
        case "login.chatgpt":
          await sleep(1000);
          return { ok: true, signed_in: true };
        case "session.start": {
          const session: Session = {
            state: "running",
            mode: String(params.mode),
            save: "1",
            started: now(),
            log_dir: "/Users/me/Library/Logs/GothicAI/2026-10-11_18-06-34",
            services: {},
            failed: null,
            code: null,
          };
          state.session = session;
          void play(session);
          return session;
        }
        case "session.stop":
          if (state.session) Object.assign(state.session, { state: "stopped", code: 0, ended: now() });
          await emit("core", { event: "session", data: state.session });
          return state.session;
        case "update.start": {
          const u = state.update;
          if (!u?.auto) throw new Error("Автообновление не установлено: выполните scripts/ryzen-play.sh autoupdate install на Mac");
          await sleep(300);
          u.updating = { state: "running", detail: "Запускаю обновление", started: now(), result: null, error: null };
          void runUpdate(u);
          return structuredClone(u);
        }
        case "report.create":
          return { path: "/Users/me/Library/Logs/GothicAI/report.zip" };
      }
      throw new Error(`mock: no ${method}`);
    },
    { shouldMockEvents: true },
  );
}

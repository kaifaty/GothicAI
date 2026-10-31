// What both windows (the launcher and the menu bar panel) know: the core's state, the providers' checks
// and the journal in plain words. Events from the core keep it current.
import { providerTitle } from "./status";
import {
  call,
  onCore,
  showWindow,
  toMenuBar,
  type CoreMessage,
  type JournalEntry,
  type Providers,
  type ServiceEvent,
  type State,
} from "./core";

export type Tone = "plain" | "you" | "npc" | "warn" | "bad";
export type Line = { t: number; tone: Tone; text: string; meta?: string };

export const SERVICE_NAMES: Record<string, string> = {
  prepare: "Подготовка базы игры",
  asr: "Голос игрока",
  tts: "Озвучка NPC",
  server: "ИИ-сервер",
  game: "Игра",
  launcher: "Лаунчер",
  queue: "Очередь игр",
};

const ACTIONS: Record<string, string> = {
  give_item: "отдаёт предмет",
  follow_player: "идёт за вами",
  stop: "останавливается",
  come_closer: "подходит ближе",
  gesture: "делает жест",
  end_conversation: "заканчивает разговор",
};

const KEEP_LINES = 300;

/** 0.49 -> "0,49 с" */
export function seconds(value: number | undefined, digits = 2): string {
  return `${(value ?? 0).toFixed(digits).replace(".", ",")} с`;
}

export function clock(t: number): string {
  return new Date(t * 1000).toLocaleTimeString("ru-RU", { hour: "2-digit", minute: "2-digit", second: "2-digit" });
}

export function journalLine(e: JournalEntry): Line | null {
  switch (e.kind) {
    case "player":
      return {
        t: e.t,
        tone: "you",
        text: e.name ? `Вы → ${e.name}: «${e.text}»` : `Вы (никому): «${e.text}»`,
        meta: e.via === "voice" ? `голосом, ${providerTitle(e.asr ?? "?")}, ${seconds(e.latency_s)}` : undefined,
      };
    case "npc":
      return { t: e.t, tone: "npc", text: `${e.name}: «${e.text}»`, meta: `первое слово ${seconds(e.first_token_s, 1)}` };
    case "action":
      return { t: e.t, tone: "plain", text: `${e.name} ${ACTIONS[e.action ?? ""] ?? e.action}` };
    case "tts_off":
      return { t: e.t, tone: "warn", text: `Озвучка не ответила: ${e.seconds ?? 30} с только субтитры`, meta: e.detail };
    case "llm_error":
      return { t: e.t, tone: "bad", text: `${e.name} не смог ответить`, meta: e.detail };
    case "voice_error":
      return { t: e.t, tone: "warn", text: `Голос не распознан`, meta: e.detail };
    case "interrupt":
      return { t: e.t, tone: "plain", text: `Вы перебили: ${e.name}` };
  }
  return null;
}

function firstLine(text: string): string {
  return text.split("\n")[0].trim();
}

export function serviceLine(e: ServiceEvent, t = Date.now() / 1000): Line | null {
  const name = SERVICE_NAMES[e.proc] ?? e.proc;
  switch (e.kind) {
    case "waiting":
      return { t, tone: "plain", text: e.detail };
    case "starting":
      if (e.proc === "prepare") return { t, tone: "plain", text: name };
      return e.proc === "game" ? { t, tone: "plain", text: "Запускаю игру" } : null;
    case "ready":
      if (e.detail === "after restart") return { t, tone: "plain", text: `${name}: снова работает` };
      return { t, tone: "plain", text: `${name}: готов ${e.detail.replace(/^in (.+)s$/, "за $1 с").replace(".", ",")}` };
    case "exited":
      if (e.proc === "game") return { t, tone: "plain", text: "Игра закрыта" };
      return { t, tone: "bad", text: `${name}: упал, ${firstLine(e.detail).replace("code", "код")}` };
    case "restarting":
      return { t, tone: "warn", text: `${name}: перезапуск ${e.detail.replace(/^in (\d+)s$/, "через $1 с")}` };
    case "failed":
      return { t, tone: "bad", text: `${name}: не запустился`, meta: firstLine(e.detail) };
    case "status": {
      if (e.proc === "prepare") return { t, tone: "plain", text: e.detail };
      if (e.proc === "microphone") return { t, tone: "warn", text: e.detail };
      const h = e.data;
      if (!h?.ok) return { t, tone: "warn", text: `${name}: не отвечает` };
      if (h.game !== undefined) return { t, tone: "plain", text: h.game ? "Игра подключилась" : "Игра отключилась" };
      const down = (h.providers ?? []).filter((p) => p.ok === false).map((p) => providerTitle(p.name));
      return down.length
        ? { t, tone: "warn", text: `${name}: ${down.join(", ")} не отвечает, работает запасной` }
        : { t, tone: "plain", text: `${name}: все провайдеры снова доступны` };
    }
  }
  return null;
}

type Schema = { settings: JsonSchema; secrets: JsonSchema };
type JsonSchema = {
  properties: Record<string, { $ref?: string; title?: string; description?: string }>;
  $defs?: Record<string, JsonSchema>;
  title?: string;
};

class Launcher {
  state = $state<State | null>(null);
  schema = $state<Schema | null>(null);
  providers = $state<Providers | null>(null);
  checking = $state(false);
  lines = $state<Line[]>([]);
  coreDown = $state(false);
  /** The microphone check while it records: recent levels (0..1) and the text heard so far. */
  voiceTest = $state<{ levels: number[]; partial: string }>({ levels: [], partial: "" });
  error = $state<string | null>(null);

  constructor(private role: "window" | "panel") {}

  async init(): Promise<void> {
    await onCore((m) => this.onMessage(m));
    [this.schema] = await Promise.all([call<Schema>("schema"), this.refresh()]);
    this.lines = (this.state?.journal ?? []).map(journalLine).filter((l): l is Line => l !== null);
    void this.checkProviders();
  }

  async refresh(): Promise<void> {
    this.state = await call<State>("state");
  }

  async checkProviders(): Promise<void> {
    this.checking = true;
    try {
      this.providers = await call<Providers>("check.providers");
    } finally {
      this.checking = false;
    }
  }

  /** Saves one setting; the state comes back with it. */
  async set(key: string, value: unknown): Promise<void> {
    try {
      this.state = await call<State>("settings.set", { key, value });
      this.error = null;
    } catch (err) {
      this.error = String(err);
      throw err;
    }
  }

  /** A setting's current value: `game.dir`, `tts.provider`; for `secrets.*` whether the key is set. */
  value(key: string): unknown {
    const [section, field] = key.split(".");
    if (!this.state) return undefined;
    if (section === "secrets") return this.state.secrets[field];
    return (this.state.settings as Record<string, Record<string, unknown>>)[section]?.[field];
  }

  /** Title and description of a setting, from the core's settings models (settings.py). */
  info(key: string): { title?: string; description?: string } {
    const [section, field] = key.split(".");
    if (!this.schema) return {};
    if (section === "secrets") return this.schema.secrets.properties[field] ?? {};
    const ref = this.schema.settings.properties[section]?.$ref?.split("/").pop();
    return (ref && this.schema.settings.$defs?.[ref]?.properties[field]) || {};
  }

  private recheckTimer: ReturnType<typeof setTimeout> | undefined;

  /** Checks the providers again shortly (a setting changed; several may change in a row). */
  recheck(): void {
    clearTimeout(this.recheckTimer);
    this.recheckTimer = setTimeout(() => void this.checkProviders(), 400);
  }

  async start(mode: "menu" | "continue" | "new"): Promise<void> {
    this.lines = [];
    try {
      await call("session.start", { mode });
      this.error = null;
    } catch (err) {
      this.error = String(err);
    }
  }

  async stop(): Promise<void> {
    await call("session.stop");
  }

  private onMessage(m: CoreMessage): void {
    const s = this.state;
    switch (m.event) {
      case "session":
        if (s) s.session = m.data;
        if (m.data.state === "stopped") {
          void this.refresh();
          if (this.role === "window") void showWindow();
        }
        break;
      case "service": {
        const e = m.data;
        if (s?.session) {
          const svc = (s.session.services[e.proc] ??= { state: e.kind, detail: e.detail });
          svc.state = e.kind;
          svc.detail = e.detail;
          if (e.data) svc.health = e.data;
        }
        const line = serviceLine(e);
        if (line) this.push(line);
        // the game is on its way: the launcher steps aside to the menu bar
        if (this.role === "window" && e.proc === "game" && e.kind === "starting") void toMenuBar();
        break;
      }
      case "journal": {
        s?.journal.push(m.data);
        const line = journalLine(m.data);
        if (line) this.push(line);
        break;
      }
      case "voice_test":
        if (m.data.level !== undefined) this.voiceTest.levels = [...this.voiceTest.levels, m.data.level].slice(-24);
        if (m.data.partial !== undefined) this.voiceTest.partial = m.data.partial;
        break;
      case "core_exit":
        this.coreDown = true;
        break;
    }
  }

  private push(line: Line): void {
    this.lines.push(line);
    if (this.lines.length > KEEP_LINES) this.lines.splice(0, this.lines.length - KEEP_LINES);
  }
}

export const launcher = new Launcher(window.location.pathname.endsWith("tray.html") ? "panel" : "window");

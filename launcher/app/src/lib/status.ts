// The four status rows (game, brain, voice, speech): before the game from the providers' checks,
// during it from the services' own health answers.
import type { Check, Part, Providers, Session } from "./core";

export type Tone = "ok" | "warn" | "bad" | "idle";
export type Row = {
  key: string;
  icon: "candle" | "mic" | "speaker" | "shield";
  title: string;
  detail: string;
  chip: string;
  tone: Tone;
};

const PROVIDER_TITLES: Record<string, string> = {
  voxtral: "Voxtral",
  chatgpt: "ChatGPT",
  openai: "OpenAI",
  inworld: "Inworld",
  voxcpm: "VoxCPM2",
  fake: "заглушка",
};

export const providerTitle = (name: string) => PROVIDER_TITLES[name.split(":")[0]] ?? name;

/** "Не отвечает: ConnectError" -> "не отвечает: ConnectError": for text that continues a sentence. */
export const lowerFirst = (text: string) => text.charAt(0).toLowerCase() + text.slice(1);

function fromChecks(key: string, icon: Row["icon"], title: string, part: Part | undefined, checking: boolean): Row {
  if (!part) return { key, icon, title, detail: checking ? "проверяю…" : "не проверено", chip: "…", tone: "idle" };
  if (!part.enabled) return { key, icon, title, detail: "Выключено в настройках", chip: "выключено", tone: "idle" };
  const [primary, fallback] = part.providers as [Check, Check | undefined];
  const names = fallback ? `${primary.title}, запасной ${fallback.title}` : primary.title;
  if (primary.ok) return { key, icon, title, detail: names, chip: lowerFirst(primary.detail), tone: "ok" };
  if (fallback?.ok) {
    return { key, icon, title, detail: `${primary.title}: ${lowerFirst(primary.detail)}`, chip: "запасной", tone: "warn" };
  }
  return { key, icon, title, detail: `${primary.title}: ${lowerFirst(primary.detail)}`, chip: "недоступно", tone: "bad" };
}

export function beforeGame(p: Providers | null, checking: boolean, gameProblems: string[], gameDir: string | null): Row[] {
  const game: Row = gameProblems.length
    ? { key: "game", icon: "shield", title: "Игра", detail: gameProblems[0], chip: "не готова", tone: "bad" }
    : { key: "game", icon: "shield", title: "Игра", detail: gameDir ?? "", chip: "найдена", tone: "ok" };
  return [
    fromChecks("brain", "candle", "Мозг NPC", p?.brain, checking),
    fromChecks("voice", "mic", "Голос игрока", p?.voice, checking),
    fromChecks("speech", "speaker", "Озвучка NPC", p?.speech, checking),
    game,
  ];
}

function service(session: Session, name: string, icon: Row["icon"], title: string, off: boolean): Row {
  if (off) return { key: name, icon, title, detail: "Выключено в настройках", chip: "выключено", tone: "idle" };
  const svc = session.services[name];
  if (!svc || svc.state === "starting") return { key: name, icon, title, detail: "Запускается", chip: "…", tone: "idle" };
  if (svc.state === "failed" || svc.state === "exited")
    return { key: name, icon, title, detail: "Сервис упал", chip: "упал", tone: "bad" };
  if (svc.state === "restarting") return { key: name, icon, title, detail: "Перезапускается", chip: "перезапуск", tone: "warn" };
  const providers = svc.health?.providers ?? [];
  const up = providers.filter((p) => p.ok !== false);
  if (!svc.health?.ok) return { key: name, icon, title, detail: "Не отвечает", chip: "не отвечает", tone: "bad" };
  if (!up.length) return { key: name, icon, title, detail: "Все провайдеры недоступны", chip: "недоступно", tone: "bad" };
  const active = providerTitle(up[0].name);
  if (up[0] !== providers[0]) {
    return { key: name, icon, title, detail: `${active}, пока ${providerTitle(providers[0].name)} недоступен`, chip: "запасной", tone: "warn" };
  }
  const rest = providers.slice(1).map((p) => providerTitle(p.name));
  return { key: name, icon, title, detail: rest.length ? `${active}, запасной ${rest.join(", ")}` : active, chip: "работает", tone: "ok" };
}

export function duringGame(session: Session, p: Providers | null, voiceOff: boolean, speechOff: boolean): Row[] {
  if (session.voice_enabled !== undefined) voiceOff = !session.voice_enabled;
  if (session.tts_enabled !== undefined) speechOff = !session.tts_enabled;
  const server = session.services.server;
  const game = session.services.game;
  const connected = server?.health?.game === true;
  const gameRow: Row =
    game?.state === "exited"
      ? { key: "game", icon: "shield", title: "Игра", detail: "Закрыта", chip: "закрыта", tone: "idle" }
      : connected
        ? { key: "game", icon: "shield", title: "Игра", detail: "Подключена к ИИ-серверу", chip: "работает", tone: "ok" }
        : { key: "game", icon: "shield", title: "Игра", detail: "Загружается", chip: "…", tone: "idle" };
  return [
    gameRow,
    fromChecks("brain", "candle", "Мозг NPC", p?.brain, false),
    service(session, "asr", "mic", "Голос игрока", voiceOff),
    service(session, "tts", "speaker", "Озвучка NPC", speechOff),
  ];
}

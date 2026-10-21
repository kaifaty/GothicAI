// The launcher core (Python, `gothic-launcher control`) behind the Rust bridge: requests and events.
// Method names, params and shapes are documented in launcher/core/src/gothic_launcher/control.py.
import { invoke } from "@tauri-apps/api/core";
import { listen, type UnlistenFn } from "@tauri-apps/api/event";

export type Check = { id: string; ok: boolean; title: string; detail: string };
export type Part = { enabled: boolean; providers: Check[] };
export type Providers = { brain: Part; voice: Part; speech: Part };

export type Settings = {
  game: { dir: string | null; dev_mode: boolean; chat_scale: number | null };
  llm: { backend: string; model: string | null; base_url: string | null; reasoning_effort: string };
  voice: {
    enabled: boolean;
    provider: string;
    fallback: string;
    voxtral_url: string | null;
    mic_device: string | null;
  };
  tts: {
    enabled: boolean;
    provider: string;
    fallback: string;
    voxcpm_url: string | null;
    loudness: number | "none" | null;
  };
  paths: { repo: string | null; uv: string | null; game_bin: string | null; game_home: string | null };
};

export type Health = { ok: boolean; service?: string; game?: boolean; providers?: { name: string; ok?: boolean }[] };
export type ServiceState = { state: string; detail: string; health?: Health };

export type Session = {
  state: "waiting" | "running" | "stopped";
  mode: string;
  save: string | null;
  started: number;
  ended?: number;
  log_dir: string;
  services: Record<string, ServiceState>;
  voice_enabled?: boolean;
  tts_enabled?: boolean;
  failed: { proc: string; detail: string } | null;
  code: number | null;
};

export type JournalEntry = {
  t: number;
  kind: string;
  npc?: string;
  name?: string;
  text?: string;
  via?: string;
  asr?: string;
  latency_s?: number;
  llm?: string;
  first_token_s?: number;
  total_s?: number;
  action?: string;
  arguments?: Record<string, unknown>;
  seconds?: number;
  detail?: string;
};

export type Mic = { index: number; name: string; default: boolean };
export type FoundGame = { path: string; ok: boolean; checks: Check[] };
export type VoiceResult = { text: string; provider: string; audio_s: number; latency_s: number };
export type SpeechResult = { text: string; provider: string; first_chunk_s: number; audio_s: number; latency_s: number };

export type ServiceEvent = { proc: string; kind: string; detail: string; data: Health | null };

export type State = {
  configured: boolean;
  problems: string[];
  settings: Settings;
  secrets: Record<string, boolean>;
  signed_in: boolean;
  last_save: { slot: string; time: number } | null;
  session: Session | null;
  journal: JournalEntry[];
  paths: { config: string; logs: string; game_home: string };
};

export type CoreMessage =
  | { event: "ready"; data: Record<string, never> }
  | { event: "session"; data: Session }
  | { event: "service"; data: ServiceEvent }
  | { event: "journal"; data: JournalEntry }
  | { event: "voice_test"; data: { level?: number; partial?: string } }
  | { event: "core_exit"; data: Record<string, never> }
  | { event: "error"; data: { detail: string } };

export function call<T>(method: string, params: Record<string, unknown> = {}): Promise<T> {
  return invoke<T>("core_call", { method, params });
}

export function onCore(handler: (msg: CoreMessage) => void): Promise<UnlistenFn> {
  return listen<CoreMessage>("core", (e) => handler(e.payload));
}

/** The launcher window hides; the menu bar icon stays (while the game runs). */
export const toMenuBar = () => invoke("to_menu_bar");
export const showWindow = () => invoke("show_window");
/** Stops the game and the services and quits (after the player confirmed `quit_requested`). */
export const quitApp = () => invoke("quit_app");
/** Quitting while the game runs: the window asks first. */
export const onQuitRequested = (handler: () => void) => listen("quit_requested", handler);
export const reveal = (path: string) => invoke("reveal", { path });
export const openFolder = (path: string) => invoke("open_folder", { path });

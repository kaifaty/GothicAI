"""`gothic-launcher`: the AI services and the game with one command, and the settings they run with.

    gothic-launcher up                  services, then the game (its menu); everything stops with the game
    gothic-launcher up --continue       ... loading the newest save
    gothic-launcher up --no-game        only the services, on their usual ports, until Ctrl-C
    gothic-launcher login               sign in to ChatGPT (once)
    gothic-launcher config show | set KEY [VALUE] | unset KEY | import-env [FILE]
    gothic-launcher setup [--game-dir DIR] [--change-key]    first run: game folder, remote-only
                                        profile, Inworld key (hidden input), ChatGPT sign-in
    gothic-launcher setup --check       what is still missing before `up`, in plain Russian
    gothic-launcher check [DIR]         check the ready base or installation, without provider requests
    gothic-launcher prepare [DIR]       prepare the base and videos, without starting services or game
    gothic-launcher control             the launcher window's link to the core (see control.py)
    gothic-launcher play                Ryzen desktop session (also used by the terminal fallback)

The first run takes the values of the repository's `.env`.
"""

from __future__ import annotations

import argparse
import asyncio
import contextlib
import getpass
import json
import os
import signal
import subprocess
import sys
import time
from collections.abc import Callable
from pathlib import Path
from typing import Any

from . import checks
from .checks import CHATGPT_TOKENS
from .commands import (
    LaunchError,
    Ports,
    base_cache_env,
    content_converter,
    default_game_bin,
    find_layout,
    game,
    latest_save,
    load_config,
    new_log_dir,
    preparation,
    services,
    storygraph_tool,
    tools,
)
from .settings import Config, Secrets, Settings, SettingsError
from .state import AlreadyRunning, SessionFile
from .supervisor import Event, Supervisor

DEFAULT_PORTS = Ports(server=47817, asr=47819, tts=47820)  # what the services and the game use by default


def up(args: argparse.Namespace) -> int:
    session = SessionFile()
    for name, pgid in session.acquire().items():  # a second launcher stops here, before touching anything
        print(f"stopped {name} (group {pgid}), left running by a launcher that was killed")
    try:
        return _up(args, session)
    finally:
        session.release()


def _up(args: argparse.Namespace, session: SessionFile) -> int:
    config = _config()
    if args.smoke:
        _smoke_settings(config)
    if args.no_voice:
        config.settings.voice.enabled = False
    if sys.platform in ("linux", "win32"):
        _require_credentials(config)
    layout = find_layout(config)
    ports = DEFAULT_PORTS if args.no_game else Ports.free()
    log_dir = new_log_dir()
    save = args.save
    if args.continue_:
        save = latest_save(layout.game_home)
        if save is None:
            print(f"no saves in {layout.game_home} yet: starting with the menu")
    _warn_login(config)
    print(f"logs: {log_dir}")
    if sys.platform == "linux":
        print(f"server health: http://127.0.0.1:{ports.server}/health", flush=True)
    if args.no_game:
        print(f"the game connects to ws://127.0.0.1:{ports.server} (its default)")
    game_proc = None if args.no_game else game(config, layout, ports, save=save, new=args.new)
    sup = Supervisor(
        services(config, layout, ports, cwd=log_dir),
        game_proc,
        log_dir,
        print_event,
        session=session,
        echo=_echo if args.verbose else None,
        preparation=(
            preparation(layout.game_bin, layout.game_dir, log_dir, game_proc.env) if game_proc else None
        ),
    )
    return asyncio.run(_run(sup))


async def _run(sup: Supervisor) -> int:
    loop = asyncio.get_running_loop()
    interrupted: list[int] = []

    def stop(sig: int) -> None:
        interrupted.append(sig)
        sup.stop()

    restore = on_stop_signals(loop, stop)
    try:
        code = await sup.run()
    finally:
        restore()
    return 128 + interrupted[0] if interrupted else code


def on_stop_signals(loop: asyncio.AbstractEventLoop, callback: Callable[[int], None]) -> Callable[[], None]:
    """SIGINT, SIGTERM and SIGHUP call `callback(signum)` in the loop; returns what undoes it on Windows.

    Windows has no add_signal_handler: Ctrl-C and Ctrl-Break arrive through signal.signal (the proactor
    loop wakes up for them) and are handed over to the loop. Closing the console window ends the launcher
    instead, and the job objects end every process it started."""
    if sys.platform != "win32":
        for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
            loop.add_signal_handler(sig, callback, sig)
        return lambda: None
    stops = (signal.SIGINT, signal.SIGBREAK)
    previous = {sig: signal.getsignal(sig) for sig in stops}

    def handler(signum: int, _frame: object) -> None:
        loop.call_soon_threadsafe(callback, signum)

    for sig in stops:
        signal.signal(sig, handler)

    def restore() -> None:
        for sig, old in previous.items():
            signal.signal(sig, old)

    return restore


def play(args: argparse.Namespace) -> int:
    """The terminal fallback follows exactly the window's session.start path."""
    from .control import Control

    def write(msg: dict[str, Any]) -> None:
        if msg["event"] == "service":
            print_event(Event(**msg["data"]))
        elif msg["event"] == "journal":
            print_event(Event("server", "journal", data=msg["data"]))
        elif msg["event"] == "session" and msg["data"]["state"] == "running":
            print(f"logs: {msg['data']['log_dir']}", flush=True)
            if url := msg["data"].get("health_url"):
                print(f"server health: {url}", flush=True)

    return asyncio.run(_controlled_session(Control(write), "new" if args.smoke else "continue", args.smoke))


def prepare(args: argparse.Namespace) -> int:
    config = _config()
    if args.dir is not None:
        config.settings.game.dir = args.dir
    layout = find_layout(config)
    log_dir = new_log_dir()
    sup = Supervisor(
        [], None, log_dir, print_event,
        preparation=preparation(layout.game_bin, layout.game_dir, log_dir, layout.game_env),
    )
    return asyncio.run(_run(sup))


def session_worker(args: argparse.Namespace) -> int:
    """Only called by LinuxSession inside gq; stdout is the window's normal event stream."""
    from .control import Control
    from .linux_session import installation_lock

    def write(msg: dict[str, Any]) -> None:
        print(json.dumps(msg, ensure_ascii=False), flush=True)

    with installation_lock():
        control = Control(
            write,
            config_dir=args.config_dir,
            state_file=args.log_dir / "session.json",
            log_root=args.log_dir.parent,
            admitted=True,
            session_log_dir=args.log_dir,
            no_voice=args.no_voice,
        )
        return asyncio.run(_controlled_session(control, args.mode, args.smoke))


async def _controlled_session(control, mode: str, smoke: bool) -> int:
    loop = asyncio.get_running_loop()
    closing = asyncio.Event()
    restore = on_stop_signals(loop, lambda _sig: closing.set())
    try:
        await control.session_start(mode=mode, smoke=smoke)
        assert control.task is not None
        stopping = asyncio.create_task(closing.wait())
        try:
            await asyncio.wait({control.task, stopping}, return_when=asyncio.FIRST_COMPLETED)
        finally:
            stopping.cancel()
        await control.shutdown()
        return control.session["code"] or 0
    finally:
        await control.shutdown()
        restore()


def login(args: argparse.Namespace) -> int:
    repo, uv = tools(Config.load())
    argv = [str(uv), "run", "--project", str(repo / "server"), "gothic-ai", "login", "chatgpt"]
    return subprocess.call(argv)


# What the author plays with: every model remote, nothing on one's own servers, no local models. The brain
# and speech recognition on the ChatGPT plan, NPC voices on Inworld TTS-2 with the original voices cloned,
# no fallbacks and no quick model.
REMOTE_PROFILE: dict[str, Any] = {
    "llm.backend": "chatgpt",
    "llm.quick": False,
    "llm.decider_url": None,
    "llm.quick_url": None,
    "llm.body_url": None,
    "voice.enabled": True,
    "voice.provider": "chatgpt",
    "voice.fallback": "none",
    "tts.enabled": True,
    "tts.provider": "inworld",
    "tts.fallback": "none",
    "tts.original_voices": True,
}
NO_GAME_FOLDER = 3  # the Windows setup then shows a folder picker


def setup(args: argparse.Namespace) -> int:
    """First run, for a player: the game folder, the remote-only profile, the Inworld key (hidden input)
    and the ChatGPT sign-in. A value of one's own that the profile replaces is printed; other settings and
    a key already set stay. `--check` only says what is still missing before `up`."""
    config = _config()
    if args.check:
        return _preflight(config)
    folder = _choose_game(config, args.game_dir)
    if folder is None:
        return NO_GAME_FOLDER
    replaced = _apply({"game.dir": str(folder), **REMOTE_PROFILE}, config)
    config.save()
    print(f"Папка игры: {folder}")
    print("Мозг NPC и распознавание речи: подписка ChatGPT. Голоса NPC: Inworld, оригинальные голоса.")
    if replaced:
        print("Заменены ваши прежние значения:")
        for title, old in replaced:
            print(f"  {title}: было {_shown(old)}")
    if args.change_key or not config.secrets.inworld_api_key:
        _ask_inworld_key(config)
    if not CHATGPT_TOKENS.is_file():
        print("Сейчас откроется браузер: войдите в ChatGPT и вернитесь в это окно.")
        if login(args) != 0 or not CHATGPT_TOKENS.is_file():
            print(f"Вход в ChatGPT не завершён. Запустите {_setup_hint()} ещё раз.")
    return _preflight(config)


def _choose_game(config: Config, given: str | None) -> Path | None:
    """The given folder, else the one already set, else one found in the Steam libraries. None, with the
    reasons, when none fits."""
    if given:
        candidates, found = [Path(given).expanduser()], False
    else:
        current = [config.settings.game.dir] if config.settings.game.dir is not None else []
        candidates = list(dict.fromkeys([*current, *checks.find_games()]))
        found = bool(candidates)
    for folder in candidates:
        problems = [c for c in checks.game_folder(folder) if not c.ok]
        if not problems:
            return folder
        print(f"Папка {folder} не подходит:")
        for c in problems:
            print(f"  {c.title}: {c.detail}")
    if not found and not given:
        print("Gothic II с аддоном «Ночь Ворона» не найдена в библиотеках Steam.")
    print("Укажите папку Gothic II: в ней лежат папки Data и _work.")
    return None


def _apply(values: dict[str, Any], config: Config) -> list[tuple[str, Any]]:
    """Sets the values; returns (title, old value) of those that replace a value of one's own."""
    defaults = Config(Settings(), Secrets(), config.dir)
    replaced = []
    for key, value in values.items():
        old = config.get(key)
        config.set(key, value)
        if old != config.get(key) and old != defaults.get(key):
            section, _, field = key.partition(".")
            model = Settings.model_fields[section].annotation
            replaced.append((model.model_fields[field].title or key, old))  # type: ignore[union-attr]
    return replaced


def _shown(value: Any) -> str:
    if value is None:
        return "пусто"
    if isinstance(value, bool):
        return "да" if value else "нет"
    return str(value)


def _ask_inworld_key(config: Config) -> None:
    """Hidden input, never an argument: the key stays out of the shell history and the process list."""
    key = getpass.getpass("Вставьте ключ API Inworld (символы не видны) и нажмите Enter: ").strip()
    if not key:
        print("Ключ не введён: без него NPC будут молчать, только субтитры.")
        return
    config.set("secrets.inworld_api_key", key)
    config.save()
    speech = asyncio.run(checks.providers(config))["speech"]["providers"]
    for c in speech:
        print(f"{c['title']}: {c['detail']}")


def _preflight(config: Config) -> int:
    """What is still missing before `up`: one plain line each, with what to run; 0 when nothing is."""
    s, hint, missing = config.settings, _setup_hint(), []
    if s.game.dir is None or not all(c.ok for c in checks.game_folder(s.game.dir)):
        missing.append(f"Папка игры не выбрана или не подходит. Запустите {hint}.")
    try:
        repo, _ = tools(config)
    except LaunchError:
        missing.append(f"Не найден uv или папка проекта. Запустите {hint}.")
    else:
        game_bin = s.paths.game_bin or default_game_bin(repo)
        # `up` prepares the base with the converter beside the game, unless a base is given explicitly
        prepares = not os.environ.get("OPENGOTHIC_BASE")
        if not game_bin.is_file() or (prepares and not content_converter(game_bin).is_file()):
            missing.append(f"Игра ещё не собрана. Запустите {hint}.")
        if "STORYGRAPH_TOOL" not in os.environ and not storygraph_tool(repo).is_file():
            missing.append(f"Не собран граф сюжета, без него NPC не знают сюжета. Запустите {hint}.")
    providers = _providers_in_use(config)
    if "chatgpt" in providers and not CHATGPT_TOKENS.is_file():
        missing.append(f"Нет входа в ChatGPT. Запустите {hint}.")
    if "inworld" in providers and not config.secrets.inworld_api_key:
        missing.append(f"Не задан ключ Inworld. Запустите {hint}.")
    if "openai" in providers and not config.secrets.openai_api_key:
        missing.append("Не задан ключ OpenAI: gothic-launcher config set secrets.openai_api_key")
    for line in missing:
        print(line)
    if not missing:
        print("Всё готово к игре.")
    return 2 if missing else 0


def _setup_hint() -> str:
    return "Setup-GothicAI.cmd" if sys.platform == "win32" else "gothic-launcher setup"


def config_cmd(args: argparse.Namespace) -> int:
    # any first command starts the settings from `.env`, not only `up`
    config = Config.load() if args.action == "import-env" else _config()
    match args.action:
        case "show":
            print(f"# {config.settings_path}\n# {config.secrets_path}\n")
            print(config.show(), end="")
        case "set":
            value = args.value
            if value is None:
                if not config.is_secret(args.key):
                    raise SettingsError(f"{args.key}: value missing")
                value = getpass.getpass(f"{args.key}: ")  # keys stay out of the shell history
            if value.startswith("~"):
                value = os.path.expanduser(value)
            config.set(args.key, value)
            config.save()
            print(f"{args.key} = {'<set>' if config.is_secret(args.key) else config.get(args.key)}")
        case "unset":
            config.set(args.key, None)
            config.save()
            print(f"{args.key} = {config.get(args.key)} (default)")
        case "import-env":
            path = Path(args.file) if args.file else tools(config)[0] / ".env"
            _import_env(config, path)
            config.save()
    return 0


def print_event(e: Event) -> None:
    text = _describe(e)
    if text is not None:
        print(f"{time.strftime('%H:%M:%S')} {e.proc:<6} {text}", flush=True)


def _describe(e: Event) -> str | None:
    data = e.data or {}
    match e.kind:
        case "ready":
            return " ".join(filter(None, ["ready", e.detail, _providers(data)]))
        case "status":
            if e.proc in ("microphone", "prepare"):
                return e.detail
            if not data.get("ok"):
                return "not answering"
            if "game" in data:
                return "game connected" if data["game"] else "game disconnected"
            return _providers(data)
        case "exited":
            return f"exited with {e.detail}"
        case "restarting":
            return f"restarting {e.detail}"
        case "failed":
            return f"failed: {e.detail}"
        case "stopped":
            return f"stopped{': ' + e.detail if e.detail else ''}"
        case "starting":
            return "starting"
        case "waiting":
            return e.detail
        case "journal":
            return _journal_line(data)
    return None


def _journal_line(entry: dict[str, Any]) -> str | None:
    match entry.get("kind"):
        case "player":
            return f"you -> {entry.get('name') or 'nobody'}: {entry.get('text')}"
        case "interrupt":
            return f"you interrupted {entry.get('name')}"
        case "npc":
            return f"{entry.get('name')}: {entry.get('text')}"
        case "action":
            return f"{entry.get('name')} does {entry.get('action')} {entry.get('arguments') or ''}".rstrip()
        case "tts_off" | "llm_error" | "voice_error":
            return f"{entry['kind']}: {entry.get('detail')}"
    return None


def _providers(health: dict[str, Any]) -> str:
    """`voxtral, chatgpt`; a provider that dropped out is marked: `inworld (down), voxcpm`."""
    names = [p["name"] + ("" if p.get("ok", True) else " (down)") for p in health.get("providers", [])]
    return ", ".join(names)


def _echo(name: str, line: str) -> None:
    print(f"{name:<6}| {line}", flush=True)


def _config() -> Config:
    """On the first run (no settings.toml yet) the settings start from the repository's `.env`."""
    config, taken = load_config()
    if taken is not None:
        _report_import(taken.path, taken.imported, taken.skipped)
        print(f"settings saved to {config.settings_path}; change them with `gothic-launcher config set`")
    return config


def _import_env(config: Config, path: Path) -> None:
    _report_import(path, *config.import_env(path))


def _report_import(path: Path, imported: list[str], skipped: list[str]) -> None:
    print(f"from {path}: {', '.join(imported) or 'nothing'}")
    if skipped:
        print(f"  not launcher settings, left out: {', '.join(skipped)}")


def _warn_login(config: Config) -> None:
    s = config.settings
    uses_chatgpt = s.llm.backend == "chatgpt" or (
        s.voice.enabled and "chatgpt" in (s.voice.provider, s.voice.fallback)
    )
    if uses_chatgpt and not CHATGPT_TOKENS.exists():
        print("not signed in to ChatGPT: run `gothic-launcher login` first", file=sys.stderr)


def _providers_in_use(config: Config) -> set[str]:
    s = config.settings
    providers = {s.llm.backend}
    if s.voice.enabled:
        providers.update(p.partition(":")[0] for p in (s.voice.provider, s.voice.fallback))
    if s.tts.enabled:
        providers.update(p.partition(":")[0] for p in (s.tts.provider, s.tts.fallback))
    return providers


def _require_credentials(config: Config) -> None:
    """Fail before spawning Linux or Windows services, whose provider constructors need these credentials."""
    providers = _providers_in_use(config)
    missing = []
    if "chatgpt" in providers and not CHATGPT_TOKENS.is_file():
        missing.append("not signed in to ChatGPT: run `gothic-launcher login` on this computer")
    if "openai" in providers and not config.secrets.openai_api_key:
        missing.append("OpenAI API key is missing: run `gothic-launcher config set secrets.openai_api_key`")
    if "inworld" in providers and not config.secrets.inworld_api_key:
        missing.append("Inworld API key is missing: run `gothic-launcher config set secrets.inworld_api_key`")
    if missing:
        raise LaunchError("\n".join(missing))


def _smoke_settings(config: Config) -> None:
    """A single quota-free run; never write these overrides to settings.toml."""
    llm = config.settings.llm
    llm.backend, llm.model, llm.base_url = "fake", None, None
    llm.decider_url, llm.quick_url, llm.body_url, llm.quick = None, None, None, False
    llm.body_policy, llm.body_net = "service", None
    config.settings.voice.enabled = False
    config.settings.tts.enabled = False


def main() -> None:
    parser = argparse.ArgumentParser(prog="gothic-launcher")
    sub = parser.add_subparsers(dest="command", required=True)

    p = sub.add_parser("up", help="start the services and the game; everything stops when the game exits")
    which = p.add_mutually_exclusive_group()
    which.add_argument("--continue", dest="continue_", action="store_true", help="load the newest save")
    which.add_argument("--save", help="load this save slot")
    which.add_argument("--new", action="store_true", help="start a new game at once (no menu)")
    which.add_argument("--no-game", action="store_true", help="only the services, on their default ports")
    p.add_argument("-v", "--verbose", action="store_true", help="show the output of every process")
    p.add_argument("--no-voice", action="store_true", help="disable microphone input for this run")
    p.add_argument(
        "--smoke", action="store_true", help="test with fake LLM, without voice, TTS or remote models"
    )

    p = sub.add_parser("play", help="start a Ryzen desktop session (the window's play action)")
    p.add_argument("--smoke", action="store_true", help="fake LLM, no model requests or voice")

    p = sub.add_parser("_session", help=argparse.SUPPRESS)
    p.add_argument("--mode", choices=("menu", "continue", "new"), required=True)
    p.add_argument("--config-dir", type=Path, required=True)
    p.add_argument("--log-dir", type=Path, required=True)
    p.add_argument("--smoke", action="store_true")
    p.add_argument("--no-voice", action="store_true")

    sub.add_parser("login", help="sign in to ChatGPT")
    sub.add_parser("control", help="serve the launcher window: JSON lines on stdin and stdout")
    p = sub.add_parser("check", help="check the ready game base or installation without provider requests")
    p.add_argument("dir", nargs="?", type=Path, help="game settings directory (default: game.dir)")
    p = sub.add_parser("prepare", help="prepare the game base and videos without starting game or services")
    p.add_argument("dir", nargs="?", type=Path, help="game directory (default: game.dir)")

    p = sub.add_parser(
        "setup", help="first run: game folder, remote-only profile, Inworld key, ChatGPT sign-in"
    )
    p.add_argument("--game-dir", help="the Gothic II folder (default: the one set, or found in Steam)")
    p.add_argument("--change-key", action="store_true", help="enter the Inworld key again (hidden input)")
    p.add_argument("--check", action="store_true", help="only say what is still missing before `up`")

    p = sub.add_parser("config", help="show or change the settings")
    actions = p.add_subparsers(dest="action", required=True)
    actions.add_parser("show")
    a = actions.add_parser("set", help="a key without a value asks for it (API keys)")
    a.add_argument("key")
    a.add_argument("value", nargs="?")
    a = actions.add_parser("unset", help="back to the default")
    a.add_argument("key")
    a = actions.add_parser("import-env", help="take the values of a .env (default: the repository's)")
    a.add_argument("file", nargs="?")

    args = parser.parse_args()
    os.environ.update(base_cache_env())  # Windows: the base readers here and in every child agree
    interrupted = False
    try:
        match args.command:
            case "up":
                code = up(args)
            case "play":
                code = play(args)
            case "prepare":
                code = prepare(args)
            case "_session":
                code = session_worker(args)
            case "login":
                code = login(args)
            case "control":
                from . import control

                code = control.main()
            case "config":
                code = config_cmd(args)
            case "setup":
                code = setup(args)
            case "check":
                from .checks import game_folder

                items = game_folder(args.dir if args.dir is not None else _config().settings.game.dir)
                print(json.dumps([item.plain() for item in items], ensure_ascii=False))
                code = 0 if all(item.ok for item in items) else 1
            case _:
                code = 2
    except (SettingsError, LaunchError, AlreadyRunning) as err:
        print(f"error: {err}", file=sys.stderr)
        code = 2
    except KeyboardInterrupt:
        code = 130
        interrupted = True
    if args.command == "play" and code and not interrupted:
        print(f"Gothic AI exited with code {code}.", file=sys.stderr)
        if sys.stdin.isatty():
            with contextlib.suppress(EOFError):
                input("Press Enter to close this terminal.")
    sys.exit(code)

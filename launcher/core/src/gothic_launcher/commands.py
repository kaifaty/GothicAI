"""What the launcher starts: the AI services from the repository and the game, built from the settings.

Services run with `uv run --project <service>` from the repository, so code changes are picked up without
rebuilding anything. Their working directory is the session's log directory, where they find no `.env`,
and the variables the launcher manages (GOTHIC_*, API keys) are dropped from the inherited environment:
the settings are the only source of their configuration. API keys travel in the environment, never in
argv, so they stay out of logs and `ps`.
"""

from __future__ import annotations

import os
import re
import shutil
import socket
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path

from .settings import CONFIG_DIR, Config

GAME_HOME = Path.home() / ".local" / "share" / "gothic-ai" / "game"


def default_hd_textures() -> Path:
    if sys.platform == "linux":
        return Path.home() / "gothic-ai" / "hd-textures" / "textures"
    return Path.home() / ".local" / "share" / "gothic-ai" / "hd-textures" / "textures"


def default_log_root() -> Path:
    if sys.platform == "linux":
        return Path.home() / ".local" / "state" / "gothic-ai" / "logs"
    return Path.home() / "Library" / "Logs" / "GothicAI"


# sharper textures of the characters, made from the game's own by scripts/hd-textures.sh (never in git)
HD_TEXTURES = default_hd_textures()
FRACTURE_CACHE = Path.home() / ".local" / "share" / "gothic-ai" / "fracture"
LOG_ROOT = default_log_root()
KEEP_LOGS = 20
UV_CANDIDATES = ("~/.local/bin/uv", "/opt/homebrew/bin/uv", "/usr/local/bin/uv", "~/.cargo/bin/uv")
MANAGED_ENV = ("OPENAI_API_KEY", "INWORLD_API_KEY")
FOREIGN_ENV = ("VIRTUAL_ENV",)  # the launcher's own venv: `uv run` of a service would warn and ignore it
SAVE_RE = re.compile(r"save_slot_(\d+)\.sav")


class LaunchError(Exception):
    """Something needed to start is missing; the message says what to set or build."""


@dataclass(frozen=True)
class Proc:
    name: str
    argv: list[str]
    cwd: Path
    env: dict[str, str] = field(default_factory=dict)  # on top of the inherited environment, cleaned
    health: str | None = None  # answers when the process is ready; None: started is ready
    restart: bool = True  # restarted when it exits during the session (the game ends the session instead)
    stamp: bool = False  # prefix log lines with the time (the game's own lines have none)


@dataclass(frozen=True)
class Layout:
    repo: Path
    uv: Path
    game_bin: Path
    game_dir: Path
    game_home: Path
    hd_textures: Path | None = None  # the game loads them instead of its own (OPENGOTHIC_TEXTURES)
    fracture_cache: Path | None = None  # pre-cut breakable meshes (OPENGOTHIC_FRACTURE)


@dataclass(frozen=True)
class Ports:
    server: int
    asr: int
    tts: int

    @classmethod
    def free(cls) -> Ports:
        """Ports nobody listens on now (kept for the whole session, so a restarted service keeps its
        address and the game reconnects to the same url)."""
        socks = [socket.socket() for _ in range(3)]
        try:
            for s in socks:
                s.bind(("127.0.0.1", 0))
            return cls(*(s.getsockname()[1] for s in socks))
        finally:
            for s in socks:
                s.close()


@dataclass(frozen=True)
class EnvImport:
    path: Path
    imported: list[str]
    skipped: list[str]


def load_config(config_dir: Path = CONFIG_DIR) -> tuple[Config, EnvImport | None]:
    """The settings. On the first run (no settings.toml yet) they start from the repository's `.env` and
    are saved; the second value then says what came from it."""
    config = Config.load(config_dir)
    if config.settings_path.exists():
        return config, None
    taken = None
    repo = repo_dir(config)
    env = repo / ".env" if repo is not None else None
    if env is not None and env.is_file():
        taken = EnvImport(env, *config.import_env(env))
    config.save()
    return config, taken


def new_log_dir(root: Path = LOG_ROOT) -> Path:
    """A folder for this session's logs (made by the supervisor); the oldest ones beyond KEEP_LOGS go."""
    root.mkdir(parents=True, exist_ok=True)
    old = sorted(p for p in root.iterdir() if p.is_dir())
    for p in old[: max(0, len(old) - KEEP_LOGS + 1)]:
        shutil.rmtree(p, ignore_errors=True)
    return root / time.strftime("%Y-%m-%d_%H-%M-%S")


def find_layout(config: Config) -> Layout:
    """Raises LaunchError listing everything that is missing."""
    paths = config.settings.paths
    problems: list[str] = []
    try:
        repo, uv = tools(config)
    except LaunchError as err:
        problems.append(str(err))
        repo, uv = repo_dir(config) or Path(), Path("uv")
    game_bin = paths.game_bin or default_game_bin(repo)
    if not os.access(game_bin, os.X_OK):
        problems.append(f"the game is not built: no {game_bin} (see README, «Сборка и запуск»)")
    game_dir = config.settings.game.dir
    if game_dir is None:
        problems.append("game folder is not set: gothic-launcher config set game.dir '/path/to/Gothic II'")
        game_dir = Path()
    elif not _has_child(game_dir, "data"):
        problems.append(f"{game_dir} has no Data folder: is it Gothic II?")
    if problems:
        raise LaunchError("\n".join(problems))
    hd = HD_TEXTURES if HD_TEXTURES.is_dir() else None
    fracture = FRACTURE_CACHE if FRACTURE_CACHE.is_dir() else None
    return Layout(repo, uv, game_bin, game_dir, paths.game_home or GAME_HOME, hd, fracture)


def texture_env(layout: Layout) -> dict[str, str]:
    """The game's environment for the sharper textures, when they have been made."""
    return {"OPENGOTHIC_TEXTURES": str(layout.hd_textures)} if layout.hd_textures is not None else {}


def fracture_env(layout: Layout) -> dict[str, str]:
    """An explicit override, including empty, takes precedence over the generated cache."""
    if "OPENGOTHIC_FRACTURE" in os.environ:
        return {"OPENGOTHIC_FRACTURE": os.environ["OPENGOTHIC_FRACTURE"]}
    return {"OPENGOTHIC_FRACTURE": str(layout.fracture_cache)} if layout.fracture_cache is not None else {}


def tools(config: Config) -> tuple[Path, Path]:
    """The repository and uv: enough for `uv run` of a service's own commands (login, tests)."""
    repo, uv = repo_dir(config), uv_path(config)
    problems = []
    if repo is None or not (repo / "server" / "pyproject.toml").is_file():
        problems.append(f"no GothicAI repository at {repo}: gothic-launcher config set paths.repo <path>")
    if uv is None:
        problems.append("uv not found: install it, or gothic-launcher config set paths.uv <path>")
    if problems:
        raise LaunchError("\n".join(problems))
    assert repo is not None and uv is not None
    return repo, uv


def repo_dir(config: Config) -> Path | None:
    """Set in the settings, or the checkout this launcher runs from."""
    if config.settings.paths.repo:
        return config.settings.paths.repo
    for d in Path(__file__).resolve().parents:
        if (d / "server" / "pyproject.toml").is_file() and (d / "launcher").is_dir():
            return d
    return None


def uv_path(config: Config) -> Path | None:
    """Set in the settings, on PATH, or where installers put it (apps started from the Dock get a short
    PATH without Homebrew and ~/.local/bin)."""
    if config.settings.paths.uv:
        return config.settings.paths.uv
    if found := shutil.which("uv"):
        return Path(found)
    for c in UV_CANDIDATES:
        p = Path(c).expanduser()
        if os.access(p, os.X_OK):
            return p
    return None


def default_game_bin(repo: Path) -> Path:
    """The build's Gothic2Notr; from inside `OpenGothic Dev.app` (scripts/make-dev-app.sh) when that bundle
    exists, so the game has its own name and icon in the Dock."""
    build = repo / "OpenGothic" / "build" / "opengothic"
    bundled = build / "OpenGothic Dev.app" / "Contents" / "MacOS" / "Gothic2Notr"
    return bundled if sys.platform == "darwin" and os.access(bundled, os.X_OK) else build / "Gothic2Notr"


def services(config: Config, layout: Layout, ports: Ports, cwd: Path) -> list[Proc]:
    """In start order: ASR and TTS (when on), then the AI server that uses them."""
    s, keys = config.settings, config.secrets
    procs: list[Proc] = []
    server_args = [
        "--port", str(ports.server), "--no-console", "--journal",
        "--game-dir", str(layout.game_dir),
        "--llm", s.llm.backend, "--reasoning-effort", s.llm.reasoning_effort,
    ]  # fmt: skip
    server_args += _opt("--model", s.llm.model) + _opt("--base-url", s.llm.base_url)
    server_args += _opt("--decider-url", s.llm.decider_url) + _opt("--quick-url", s.llm.quick_url)
    server_args += _opt("--body-url", s.llm.body_url)  # the neural body pilot is on when the address is set
    server_args += [] if s.llm.quick else ["--no-quick"]  # on by default, next to the decider
    openai_key = {"OPENAI_API_KEY": keys.openai_api_key} if keys.openai_api_key else {}

    if s.voice.enabled:
        env = dict(openai_key)
        if s.voice.voxtral_url:
            env["GOTHIC_VOXTRAL_URL"] = s.voice.voxtral_url
        argv = ["--port", str(ports.asr), "--provider", s.voice.provider, "--fallback", s.voice.fallback]
        procs.append(_service(layout, "asr", "asr-service", "gothic-asr", argv, env, ports.asr, cwd))
        server_args += ["--asr-url", f"ws://127.0.0.1:{ports.asr}"] + _opt("--mic-device", s.voice.mic_device)
    else:
        server_args.append("--no-voice")

    if s.tts.enabled:
        env = dict(openai_key)
        if s.tts.voxcpm_url:
            env["GOTHIC_VOXCPM_URL"] = s.tts.voxcpm_url
        if keys.inworld_api_key:
            env["INWORLD_API_KEY"] = keys.inworld_api_key
        argv = ["--port", str(ports.tts), "--provider", s.tts.provider, "--fallback", s.tts.fallback]
        if s.tts.loudness is not None:
            level = s.tts.loudness if isinstance(s.tts.loudness, str) else f"{s.tts.loudness:g}"
            argv.append(f"--loudness={level}")  # one argument: a negative level is not an option
        procs.append(_service(layout, "tts", "tts-service", "gothic-tts", argv, env, ports.tts, cwd))
        server_args += ["--tts-url", f"ws://127.0.0.1:{ports.tts}"]
    else:
        server_args.append("--no-tts")

    procs.append(
        _service(layout, "server", "server", "gothic-ai", server_args, openai_key, ports.server, cwd)
    )
    return procs


def game(config: Config, layout: Layout, ports: Ports, save: str | None = None, new: bool = False) -> Proc:
    """`save`: slot to load (`save_slot_<save>.sav` in the game home); `new`: start a new game at once."""
    s = config.settings.game
    argv = [str(layout.game_bin), "-g", str(layout.game_dir)]
    if save is not None:
        argv += ["-save", save]
    elif new:
        argv.append("-nomenu")
    if s.dev_mode:
        argv.append("-devmode")
    env = {"GOTHIC_AI_URL": f"ws://127.0.0.1:{ports.server}", **texture_env(layout), **fracture_env(layout)}
    if s.chat_scale is not None:
        env["GOTHIC_AI_CHAT_SCALE"] = f"{s.chat_scale:g}"
    return Proc("game", argv, layout.game_home, env, restart=False, stamp=True)


def latest_save(game_home: Path) -> str | None:
    """Slot of the newest save the game wrote (quick save is slot 0)."""
    saves = [p for p in game_home.glob("save_slot_*.sav") if SAVE_RE.fullmatch(p.name)]
    if not saves:
        return None
    newest = max(saves, key=lambda p: p.stat().st_mtime)
    return SAVE_RE.fullmatch(newest.name).group(1)  # type: ignore[union-attr]


def child_env(extra: dict[str, str]) -> dict[str, str]:
    env = {
        k: v
        for k, v in os.environ.items()
        if not k.startswith("GOTHIC_") and k not in MANAGED_ENV and k not in FOREIGN_ENV
    }
    env["PYTHONUNBUFFERED"] = "1"  # service output reaches the log as it happens
    env.update(extra)
    return env


def _service(
    layout: Layout,
    name: str,
    project: str,
    script: str,
    args: list[str],
    env: dict[str, str],
    port: int,
    cwd: Path,
) -> Proc:
    argv = [str(layout.uv), "run", "--project", str(layout.repo / project), script, "serve", *args]
    return Proc(name, argv, cwd, env, health=f"http://127.0.0.1:{port}/health")


def _opt(flag: str, value: str | None) -> list[str]:
    return [flag, value] if value else []


def _has_child(folder: Path, name: str) -> bool:
    """Gothic's folders differ in case between installs (Data, DATA)."""
    try:
        return any(p.name.lower() == name and p.is_dir() for p in folder.iterdir())
    except OSError:
        return False

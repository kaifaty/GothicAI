"""`gothic-launcher control`: the launcher window's link to the core, JSON lines over stdin and stdout.

    window -> core  {"id": 7, "method": "session.start", "params": {"mode": "continue"}}
    core -> window  {"id": 7, "result": {...}}   or   {"id": 7, "error": "what went wrong"}
    core -> window  {"event": "service", "data": {...}}   pushed as it happens, without an id

Only these lines go to stdout, the core's own log goes to stderr. Requests run concurrently: a sign-in
waiting for the browser does not hold up the others. When stdin closes (the window quit or crashed) the
core stops the session, so nothing outlives the launcher.

Methods (params in brackets):
    state                              settings, keys set or not, last save, session, journal and update
    schema                             JSON Schema of the settings and keys, for the forms
    settings.set [key, value]          value null: back to the default; returns the state
    settings.import_env [path?]        the repository's .env by default
    check.game [dir?]                  the game folder, default: the one in the settings
    check.providers                    can each chosen provider be reached (no quota spent)
    find.game                          Gothic II folders found on this Mac, each with its checks
    mics                               input devices
    login.chatgpt                      the browser sign-in, answers when it is done
    test.llm [text?]                   one reply from the npc brain, with its latency (spends quota)
    test.voice [seconds?]              record the microphone and recognize it through a temporary ASR
                                       service; voice_test events carry {"level"} and {"partial"}
    test.speech                        one line in Xardas' voice through a temporary TTS service, aloud
    session.start [mode, smoke?]       "menu" (default), "continue" or "new"; smoke: quota-free run
    session.stop
    update.start                       release the install lock, update via systemd; restart when finished
    report.create                      a zip of the latest session's logs, keys removed

Events: service (supervisor events: starting, ready, status, exited, restarting, stopped, failed),
journal (lines and actions, see the server's journal.py), session ({"state": "running"|"stopped"}),
voice_test (the microphone test: levels and partial text while it records),
update (personal Ryzen installation status, with updating running/finished until the app restarts).
state.update is null outside the personal Linux install; otherwise it includes the target, build,
pending changes, auto-update report and availability, and the current update's progress.
"""

from __future__ import annotations

import asyncio
import contextlib
import json
import logging
import os
import re
import signal
import sys
import time
import zipfile
from collections import deque
from collections.abc import Awaitable, Callable
from dataclasses import asdict
from datetime import datetime
from pathlib import Path
from typing import Any, BinaryIO

from . import checks
from .commands import (
    GAME_HOME,
    LOG_ROOT,
    LaunchError,
    Layout,
    Ports,
    child_env,
    find_layout,
    game,
    latest_save,
    load_config,
    new_log_dir,
    preparation,
    services,
    tools,
)
from .settings import CONFIG_DIR, Config, Secrets, Settings
from .state import STATE_DIR, AlreadyRunning, SessionFile
from .supervisor import Event, NotReady, Supervisor, temporary

if sys.platform != "win32":
    from .linux_session import LinuxSession, installation_lock
else:  # the Ryzen desktop session needs fcntl; Windows runs the Supervisor directly
    LinuxSession = installation_lock = None

log = logging.getLogger(__name__)

JOURNAL_KEEP = 300  # entries kept for a window opened in the middle of a session
TEST_LINE = "Привет, Ксардас."
UPDATE_CHECK_S = 60
UPDATE_POLL_S = 2
UPDATE_STATUS_TIMEOUT_S = 10
UPDATE_UNIT = "gothic-play-update.service"

Write = Callable[[dict[str, Any]], None]


class RequestError(Exception):
    """An answer for the window: the message is shown to the player as is."""


class Control:
    def __init__(
        self,
        write: Write,
        *,
        config_dir: Path = CONFIG_DIR,
        state_file: Path = STATE_DIR / "launcher.json",
        log_root: Path = LOG_ROOT,
        admitted: bool = False,
        session_log_dir: Path | None = None,
        no_voice: bool = False,
        install_dir: Path | None = None,
        install_lock: BinaryIO | None = None,
    ) -> None:
        self.write = write
        self.config_dir = config_dir
        self.state_file = state_file
        self.log_root = log_root
        self.admitted = admitted  # internal worker, already inside its gq unit
        self.session_log_dir = session_log_dir
        self.no_voice = no_voice
        self.supervisor: Supervisor | LinuxSession | None = None
        self.task: asyncio.Task[None] | None = None
        self.session: dict[str, Any] | None = None  # the running (or last) session, as the window sees it
        self.journal: deque[dict[str, Any]] = deque(maxlen=JOURNAL_KEEP)
        self._testing = asyncio.Lock()  # one microphone or voice test at a time
        self._helpers = 0  # helper programs may run concurrently, but keep updates out
        # Tests can supply a play tree; production detection uses the core's own resolved location.
        play = install_dir or Path.home() / "gothic-ai/play"
        self.install_dir = (
            play.resolve() if sys.platform == "linux" and not admitted
            and (install_dir is not None or Path(__file__).resolve().is_relative_to(play.resolve())) else None
        )
        self.install_lock = install_lock
        self._update_service = (
            (play.parent.parent if install_dir is not None else Path.home())
            / ".config/systemd/user" / UPDATE_UNIT
        )
        self.update: dict[str, Any] | None = None
        self.updating: dict[str, Any] | None = None
        self.update_task: asyncio.Task[None] | None = None
        self._update_checked = False
        self._status_lock = asyncio.Lock()  # no status subprocess may outlive release of the install lock
        self._state_layout: tuple[list[str], Path] | None = None
        self.methods: dict[str, Callable[..., Awaitable[Any]]] = {
            "state": self.state,
            "schema": self.schema,
            "settings.set": self.settings_set,
            "settings.import_env": self.settings_import_env,
            "check.game": self.check_game,
            "check.providers": self.check_providers,
            "find.game": self.find_game,
            "mics": self.mics,
            "test.voice": self.test_voice,
            "test.speech": self.test_speech,
            "login.chatgpt": self.login_chatgpt,
            "test.llm": self.test_llm,
            "session.start": self.session_start,
            "session.stop": self.session_stop,
            "update.start": self.update_start,
            "report.create": self.report_create,
        }

    async def handle(self, msg: dict[str, Any]) -> None:
        req = msg.get("id")
        method = self.methods.get(str(msg.get("method")))
        try:
            if method is None:
                raise RequestError(f"unknown method {msg.get('method')!r}")
            result = await method(**(msg.get("params") or {}))
        except (RequestError, LaunchError, AlreadyRunning, ValueError, TypeError) as err:
            self.write({"id": req, "error": str(err)})
        except Exception as err:  # the window gets an answer for every request, whatever happened
            log.exception("%s failed", msg.get("method"))
            self.write({"id": req, "error": f"{type(err).__name__}: {err}"})
        else:
            self.write({"id": req, "result": result})

    async def shutdown(self) -> None:
        if self.supervisor is not None:
            self.supervisor.stop()
        if self.task is not None:
            await asyncio.gather(self.task, return_exceptions=True)

    # --- settings ------------------------------------------------------------------------------

    def _config(self) -> Config:
        return load_config(self.config_dir)[0]

    async def state(self) -> dict[str, Any]:
        if not self._update_checked:
            await self.refresh_update()
        config = self._config()
        if self.updating is not None:
            assert self._state_layout is not None
            problems, game_home = self._state_layout
        else:
            try:
                layout = find_layout(config)
                problems: list[str] = []
                game_home = layout.game_home
            except LaunchError as err:
                problems = str(err).splitlines()
                game_home = config.settings.paths.game_home or GAME_HOME
            self._state_layout = problems, game_home
        save = latest_save(game_home)
        save_file = game_home / f"save_slot_{save}.sav" if save is not None else None
        return {
            "configured": config.settings.game.dir is not None and not problems,
            "problems": problems,
            "settings": config.settings.model_dump(mode="json"),
            "secrets": {name: bool(value) for name, value in config.secrets.model_dump().items()},
            "signed_in": checks.CHATGPT_TOKENS.is_file(),
            "last_save": {"slot": save, "time": save_file.stat().st_mtime} if save_file else None,
            "session": self.session,
            "journal": list(self.journal),
            "update": self.update,
            "paths": {
                "config": str(config.dir),
                "logs": str(self.log_root),
                "game_home": str(game_home),
            },
        }

    async def schema(self) -> dict[str, Any]:
        return {"settings": Settings.model_json_schema(), "secrets": Secrets.model_json_schema()}

    async def settings_set(self, key: str, value: Any = None) -> dict[str, Any]:
        config = self._config()
        if config.is_secret(key) and value == "":
            value = None  # an emptied key field removes the key
        config.set(key, value)
        config.save()
        return await self.state()

    async def settings_import_env(self, path: str | None = None) -> dict[str, Any]:
        config = self._config()
        env = Path(path) if path else tools(config)[0] / ".env"
        imported, skipped = config.import_env(env)
        config.save()
        return {"imported": imported, "skipped": skipped}

    # --- checks --------------------------------------------------------------------------------

    async def check_game(self, dir: str | None = None) -> list[dict[str, Any]]:  # noqa: A002 (the param name)
        folder = Path(dir) if dir else self._config().settings.game.dir
        return [c.plain() for c in await asyncio.to_thread(checks.game_folder, folder)]

    async def check_providers(self) -> dict[str, Any]:
        return await checks.providers(self._config())

    async def find_game(self) -> list[dict[str, Any]]:
        found = await asyncio.to_thread(checks.find_games)
        out = []
        for folder in found:
            items = await asyncio.to_thread(checks.game_folder, folder)
            out.append(
                {"path": str(folder), "ok": all(c.ok for c in items), "checks": [c.plain() for c in items]}
            )
        return sorted(out, key=lambda f: not f["ok"])

    async def mics(self) -> list[dict[str, Any]]:
        self._not_updating()
        out: list[str] = []
        async with self._helper():
            await _run(
                "mics", self._server_argv(self._tool_layout(self._config()), "mics"), stdout=out.append,
            )
        result = json.loads(out[-1]) if out else {"error": "нет ответа"}
        if isinstance(result, dict):
            raise RequestError(result.get("error", "микрофоны не найдены"))
        return result

    async def test_voice(self, seconds: float = 4.0) -> dict[str, Any]:
        self._not_updating()
        config = self._config()
        if not config.settings.voice.enabled:
            raise RequestError("Голосовой ввод выключен в настройках")
        layout, ports = self._tool_layout(config), Ports.free()
        argv = self._server_argv(layout, "voice-test", "--json", "--seconds", f"{seconds:g}")
        argv += ["--asr-url", f"ws://127.0.0.1:{ports.asr}"]
        if config.settings.voice.mic_device:
            argv += ["--mic-device", config.settings.voice.mic_device]
        result: dict[str, Any] = {}

        def line(text: str) -> None:
            with contextlib.suppress(ValueError):
                msg = json.loads(text)
                if "level" in msg or "partial" in msg:
                    self.write({"event": "voice_test", "data": msg})
                else:
                    result.update(msg)

        async with self._service_for_test(config, layout, ports, "asr"):
            await _run("voice-test", argv, stdout=line)
        if "error" in result or "text" not in result:
            raise RequestError(result.get("error", "Проверка не удалась: voice-test ничего не ответил"))
        return result

    async def test_speech(self) -> dict[str, Any]:
        self._not_updating()
        config = self._config()
        if not config.settings.tts.enabled:
            raise RequestError("Озвучка выключена в настройках")
        layout, ports = self._tool_layout(config), Ports.free()
        argv = self._server_argv(layout, "speech-test", "--json", "--tts-url", f"ws://127.0.0.1:{ports.tts}")
        if config.settings.game.dir:
            argv += ["--game-dir", str(config.settings.game.dir)]
        out: list[str] = []
        async with self._service_for_test(config, layout, ports, "tts"):
            await _run("speech-test", argv, stdout=out.append)
        try:
            result = json.loads(out[-1])
        except (IndexError, ValueError):
            raise RequestError("Проверка не удалась: speech-test ничего не ответил") from None
        if "error" in result:
            raise RequestError(result["error"])
        return result

    def _tool_layout(self, config: Config) -> Layout:
        """The repository and uv are enough for the checks: the game need not be built yet."""
        repo, uv = tools(config)
        game_dir = config.settings.game.dir or Path()
        return Layout(repo, uv, Path(), game_dir, config.settings.paths.game_home or GAME_HOME)

    @staticmethod
    def _server_argv(layout: Layout, *args: str) -> list[str]:
        return [str(layout.uv), "run", "--project", str(layout.repo / "server"), "gothic-ai", *args]

    @contextlib.asynccontextmanager
    async def _service_for_test(self, config: Config, layout: Layout, ports: Ports, name: str):  # noqa: ANN202
        """The service `name` as the session would run it, on its own, while a check needs it."""
        self._not_updating()
        if self.task is not None and not self.task.done():
            raise RequestError("Сначала остановите игру: проверки запускают свои сервисы")
        if self._testing.locked():
            raise RequestError("Уже идёт другая проверка")
        async with self._testing, self._helper():
            proc = next(
                p for p in services(config, layout, ports, cwd=self.log_root / "checks") if p.name == name
            )
            lock = SessionFile(self.state_file)
            lock.acquire()  # AlreadyRunning: `gothic-launcher up` runs in a terminal
            try:
                async with temporary(proc, self.log_root / "checks" / f"{name}.log", lock):
                    yield
            except NotReady as err:
                raise RequestError(f"Сервис для проверки не запустился: {err}") from None
            finally:
                lock.release()

    async def login_chatgpt(self) -> dict[str, Any]:
        self._not_updating()
        repo, uv = tools(self._config())
        argv = [str(uv), "run", "--project", str(repo / "server"), "gothic-ai", "login", "chatgpt"]
        async with self._helper():
            code = await _run("login", argv)
        return {"ok": code == 0, "signed_in": checks.CHATGPT_TOKENS.is_file()}

    async def test_llm(self, text: str = TEST_LINE) -> dict[str, Any]:
        self._not_updating()
        config = self._config()
        repo, uv = tools(config)
        llm = config.settings.llm
        argv = [str(uv), "run", "--project", str(repo / "server"), "gothic-ai", "llm-test", text, "--json"]
        argv += ["--llm", llm.backend, "--reasoning-effort", llm.reasoning_effort]
        if llm.model:
            argv += ["--model", llm.model]
        if llm.base_url:
            argv += ["--base-url", llm.base_url]
        if config.settings.game.dir:
            argv += ["--game-dir", str(config.settings.game.dir)]
        env = {"OPENAI_API_KEY": config.secrets.openai_api_key} if config.secrets.openai_api_key else {}
        out: list[str] = []
        async with self._helper():
            await _run("llm-test", argv, env=env, stdout=out.append)
        try:
            result = json.loads(out[-1])
        except (IndexError, ValueError):
            raise RequestError("Проверка не удалась: llm-test ничего не ответил") from None
        if "error" in result:
            raise RequestError(result["error"])
        return result

    # --- the session ---------------------------------------------------------------------------

    async def session_start(self, mode: str = "menu", smoke: bool = False) -> dict[str, Any]:
        self._not_updating()
        if self.task is not None and not self.task.done():
            raise RequestError("Игра уже запущена")
        if mode not in ("menu", "continue", "new"):
            raise RequestError(f"unknown mode {mode!r}")
        lock = SessionFile(self.state_file)
        left = lock.acquire()  # AlreadyRunning: a `gothic-launcher up` in a terminal holds it
        try:
            config = self._config()
            if smoke:
                from .cli import _smoke_settings

                _smoke_settings(config)
            if self.no_voice:
                config.settings.voice.enabled = False
            layout = find_layout(config)
        except Exception:
            lock.release()
            raise
        save = latest_save(layout.game_home) if mode == "continue" else None
        ports = Ports.free()
        log_dir = self.session_log_dir or new_log_dir(self.log_root)
        self.journal.clear()
        self.session = {
            "state": "running",
            "mode": mode,
            "save": save,
            "started": time.time(),
            "log_dir": str(log_dir),
            "services": {},
            "failed": None,
            "code": None,
        }
        try:
            if sys.platform == "linux" and not self.admitted:
                queued = LinuxSession(
                    layout,
                    self.config_dir,
                    log_dir,
                    mode,
                    smoke,
                    self._on_linux_message,
                    self._on_event,
                    lock,
                )
                try:
                    from .cli import _require_credentials

                    if not queued.microphone:
                        config.settings.voice.enabled = False
                    _require_credentials(config)
                except Exception:
                    queued.lock.close()
                    raise
                self.supervisor = queued
                self.session["state"] = "waiting"
                self.session["services"]["queue"] = {"state": "waiting", "detail": "Ожидаем допуска gq"}
            else:
                game_proc = game(config, layout, ports, save=save, new=mode == "new")
                self.supervisor = Supervisor(
                    services(config, layout, ports, cwd=log_dir),
                    game_proc,
                    log_dir,
                    self._on_event,
                    session=lock,
                    preparation=preparation(layout.game_bin, layout.game_dir, log_dir, game_proc.env),
                )
                if sys.platform == "linux":
                    self.session["voice_enabled"] = config.settings.voice.enabled
                    self.session["tts_enabled"] = config.settings.tts.enabled
                    self.session["health_url"] = f"http://127.0.0.1:{ports.server}/health"
        except Exception:
            lock.release()
            self.session = None
            raise
        for name, pgid in left.items():
            self._on_event(Event(name, "stopped", f"left over by a launcher that was killed (group {pgid})"))
        self.task = asyncio.create_task(self._run(self.supervisor, lock))
        self.write({"event": "session", "data": self.session})
        return self.session

    async def session_stop(self) -> dict[str, Any] | None:
        await self.shutdown()
        return self.session

    def _on_linux_message(self, msg: dict[str, Any]) -> None:
        if msg["event"] == "session":
            queued = (self.session or {}).get("services", {}).get("queue")
            self.session = msg["data"]
            if queued is not None:
                self.session["services"]["queue"] = queued
            self.write({"event": "session", "data": self.session})
        elif msg["event"] == "service":
            self._on_event(Event(**msg["data"]))
        elif msg["event"] == "journal":
            self.journal.append(msg["data"])
            self.write(msg)

    async def _run(self, sup: Supervisor | LinuxSession, lock: SessionFile) -> None:
        try:
            code = await sup.run()
        except Exception as err:
            log.exception("session failed")
            code = 1
            assert self.session is not None
            self.session["failed"] = {"proc": "launcher", "detail": f"{type(err).__name__}: {err}"}
        finally:
            lock.release()
        assert self.session is not None
        self.session.update(state="stopped", code=code, ended=time.time())
        self.write({"event": "session", "data": self.session})

    def _on_event(self, e: Event) -> None:
        if e.kind == "journal":
            assert e.data is not None
            self.journal.append(e.data)
            self.write({"event": "journal", "data": e.data})
            return
        if self.session is not None:
            svc = self.session["services"].setdefault(e.proc, {})
            svc.update(state=e.kind, detail=e.detail)
            if e.data:
                svc["health"] = e.data
            if e.kind == "failed" and (e.proc != "queue" or self.session["failed"] is None):
                self.session["failed"] = {"proc": e.proc, "detail": e.detail}
        self.write({"event": "service", "data": asdict(e)})

    # --- personal Linux installation -----------------------------------------------------------

    def _not_updating(self) -> None:
        if self.updating is not None:
            raise RequestError("Идёт обновление установки")

    @contextlib.asynccontextmanager
    async def _helper(self):  # noqa: ANN202
        self._not_updating()
        self._helpers += 1
        try:
            yield
        finally:
            self._helpers -= 1

    def _auto_installed(self) -> bool:
        return self._update_service.is_file()

    async def refresh_update(self) -> None:
        async with self._status_lock:
            self._update_checked = True
            if self.install_dir is None or self.updating is not None:
                return
            status = None
            try:
                raw = json.loads(await _output(
                    sys.executable, str(self.install_dir / "scripts/ryzen-play.sh"), "__status",
                    limit_s=UPDATE_STATUS_TIMEOUT_S,
                ))
                status = {
                    "current": raw["current"], "behind": raw["behind"],
                    "built": raw["built"] == raw["head"], "latest_failed": raw["latest_failed"],
                    "head": raw["head"], "target": raw["target"], "changes": raw["changes"],
                    "report": raw["report"], "auto": self._auto_installed(), "updating": None,
                }
            except (OSError, ValueError, KeyError, TypeError, TimeoutError, RuntimeError) as err:
                log.warning("Cannot read installation update status: %s: %s", type(err).__name__, err)
            if status != self.update:
                self.update = status
                self.write({"event": "update", "data": status})

    async def poll_updates(self) -> None:
        while True:
            await self.refresh_update()
            await asyncio.sleep(UPDATE_CHECK_S)

    def _update_progress(self, **values: Any) -> None:
        assert self.updating is not None and self.update is not None
        self.updating = {**self.updating, **values}
        self.update = {**self.update, "updating": self.updating}
        self.write({"event": "update", "data": self.update})

    async def update_start(self) -> dict[str, Any]:
        if self.install_dir is None:
            raise RequestError("Обновление из окна есть только у личной установки Ryzen")
        if not self._auto_installed():
            raise RequestError(
                "Автообновление не установлено: выполните scripts/ryzen-play.sh autoupdate install на Mac"
            )
        if not self._update_checked:
            await self.refresh_update()
        async with self._status_lock:
            self._not_updating()
            if self.session is not None and self.session["state"] in ("waiting", "running"):
                raise RequestError("Сначала остановите игру")
            if self._testing.locked() or self._helpers:
                raise RequestError("Сначала завершите проверку")
            if self.update is None:
                raise RequestError("Не удалось проверить обновление установки")
            await self.state()  # find_layout may run ryzen_game.py; cache it before releasing the lock
            self.updating = {
                "state": "running", "detail": "Запускаю обновление", "started": time.time(),
                "result": None, "error": None,
            }
            self._update_progress()
            if self.install_lock is not None:
                self.install_lock.close()
            released = time.time()
            self.update_task = asyncio.create_task(self._run_update(released))
            return self.update

    def _update_report(self) -> tuple[dict[str, Any] | None, float]:
        assert self.install_dir is not None
        path = self.install_dir.parent / "play-update.json"
        try:
            with path.open() as stream:
                report = json.load(stream)
                modified = os.fstat(stream.fileno()).st_mtime
            return report, modified
        except (OSError, ValueError):
            return None, 0

    async def _run_update(self, released: float) -> None:
        assert self.updating is not None
        started = self.updating["started"]
        show = ("systemctl", "--user", "show", "-p", "ActiveState", "--value", UPDATE_UNIT)

        async def active_state() -> str:
            failures = 0
            while True:
                try:
                    active = (await _output(*show)).strip()
                    if not active:
                        raise RuntimeError("systemctl returned no ActiveState")
                    return active
                except (OSError, TimeoutError, RuntimeError) as err:
                    failures += 1
                    log.warning("Cannot read update service state (%s/5): %s", failures, err)
                    if failures == 5:
                        raise RuntimeError(
                            "Cannot read update service state after 5 consecutive polls: "
                            f"{str(err) or type(err).__name__}"
                        ) from err
                await asyncio.sleep(UPDATE_POLL_S)

        try:
            for attempt in range(2):
                # A timer's busy run may finish between start --no-block and the first poll.
                seen_active = await active_state() not in ("inactive", "failed")
                report_after = started if attempt == 0 else time.time()
                await _output("systemctl", "--user", "start", "--no-block", UPDATE_UNIT)
                while True:
                    await asyncio.sleep(UPDATE_POLL_S)
                    active = await active_state()
                    try:
                        journal = await _output(
                            "journalctl", "--user", "-u", UPDATE_UNIT, "--since", f"@{started}",
                            "-n", "1", "-o", "cat", "--no-pager",
                        )
                    except (OSError, TimeoutError, RuntimeError) as err:
                        log.warning("Cannot read update service journal: %s", err)
                    else:
                        detail = next(
                            (line.strip() for line in reversed(journal.splitlines()) if line.strip()), "",
                        )
                        if detail and detail != self.updating["detail"]:
                            self._update_progress(detail=detail)
                    report, modified = self._update_report()
                    if active in ("inactive", "failed"):
                        if seen_active or modified >= report_after:
                            break
                    else:
                        seen_active = True
                report_time = (report or {}).get("time", "")
                if (
                    attempt == 0 and report is not None and report.get("result") == "busy"
                    and datetime.fromisoformat(report_time).timestamp() < released
                ):
                    continue
                break
            assert self.update is not None
            self.update = {**self.update, "report": report}
            self._update_progress(
                state="finished", result=report.get("result") if report else None,
                error=report.get("error") if report else "Обновление завершилось без отчёта",
            )
        except (OSError, ValueError, TypeError, TimeoutError, RuntimeError) as err:
            log.warning("Installation update failed: %s", err)
            self._update_progress(state="finished", error=str(err) or type(err).__name__)

    # --- reports -------------------------------------------------------------------------------

    async def report_create(self) -> dict[str, Any]:
        """The newest session's logs and the settings in one zip, every key replaced by <hidden>."""
        config = self._config()
        root = self.log_root
        sessions = sorted(p for p in root.iterdir() if p.is_dir()) if root.is_dir() else []
        if not sessions:
            raise RequestError("Логов ещё нет: лаунчер ни разу не запускал игру")
        latest = sessions[-1]
        secrets = [v for v in config.secrets.model_dump().values() if v]
        with contextlib.suppress(OSError, ValueError):
            tokens = json.loads(checks.CHATGPT_TOKENS.read_text())
            if isinstance(tokens, dict):
                secrets.extend(
                    value for key in ("access_token", "refresh_token")
                    if isinstance(value := tokens.get(key), str) and value
                )
        target = root / f"report-{latest.name}.zip"

        def build() -> None:
            with zipfile.ZipFile(target, "w", zipfile.ZIP_DEFLATED) as z:
                for f in sorted(latest.iterdir()):
                    if f.is_file():
                        z.writestr(f"{latest.name}/{f.name}", _redact(f.read_text(errors="replace"), secrets))
                z.writestr("settings.toml", _redact(config.show(), secrets))

        await asyncio.to_thread(build)
        return {"path": str(target)}


def _redact(text: str, secrets: list[str]) -> str:
    text = re.sub(r"\bBearer[ \t]+[A-Za-z0-9._~+/=-]+", "Bearer <hidden>", text, flags=re.IGNORECASE)
    for s in secrets:
        text = text.replace(s, "<hidden>")
    return text


async def _run(
    name: str, argv: list[str], env: dict[str, str] | None = None, stdout: Callable[[str], None] | None = None
) -> int:
    """A helper command (sign-in, llm-test): its stdout lines to `stdout`, everything else to our log."""
    proc = await asyncio.create_subprocess_exec(
        *argv,
        env=child_env(env or {}),
        stdin=asyncio.subprocess.DEVNULL,
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.PIPE,
    )

    async def pump(stream: asyncio.StreamReader | None, sink: Callable[[str], None]) -> None:
        assert stream is not None
        while line := await stream.readline():
            sink(line.decode(errors="replace").rstrip("\n"))

    def to_log(line: str) -> None:
        log.info("%s: %s", name, line)

    await asyncio.gather(pump(proc.stdout, stdout or to_log), pump(proc.stderr, to_log))
    return await proc.wait()


async def _output(*argv: str, limit_s: float = 10) -> str:
    """Read a short command result; never leave a timed-out status process using the clone."""
    proc = await asyncio.create_subprocess_exec(
        *argv, stdin=asyncio.subprocess.DEVNULL, stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.PIPE, close_fds=True, start_new_session=True,
    )
    try:
        async with asyncio.timeout(limit_s):
            stdout, stderr = await proc.communicate()
    except (TimeoutError, asyncio.CancelledError):
        with contextlib.suppress(ProcessLookupError):
            os.killpg(proc.pid, signal.SIGKILL)
        await proc.communicate()
        raise
    if proc.returncode:
        raise RuntimeError(stderr.decode(errors="replace").strip() or f"{argv[0]} exited {proc.returncode}")
    return stdout.decode(errors="replace")


async def serve(install_lock: BinaryIO | None = None) -> int:
    """Reads requests from stdin until it closes."""
    loop = asyncio.get_running_loop()
    reader = asyncio.StreamReader(limit=1 << 20)
    await loop.connect_read_pipe(lambda: asyncio.StreamReaderProtocol(reader), sys.stdin)

    def write(msg: dict[str, Any]) -> None:
        sys.stdout.write(json.dumps(msg, ensure_ascii=False) + "\n")
        sys.stdout.flush()

    from .cli import on_stop_signals

    control = Control(write, install_lock=install_lock)
    closing = asyncio.Event()
    on_stop_signals(loop, lambda _sig: closing.set())
    requests: set[asyncio.Task[None]] = set()

    async def read() -> None:
        while line := await reader.readline():
            try:
                msg = json.loads(line)
                if not isinstance(msg, dict):
                    raise ValueError("not an object")
            except ValueError as err:
                write({"event": "error", "data": {"detail": f"malformed request: {err}"}})
                continue
            task = asyncio.create_task(control.handle(msg))
            requests.add(task)
            task.add_done_callback(requests.discard)
        closing.set()  # stdin closed: the window is gone

    reading = asyncio.create_task(read())
    updates = asyncio.create_task(control.poll_updates())
    write({"event": "ready", "data": {}})
    await closing.wait()
    reading.cancel()
    updates.cancel()
    if control.update_task is not None:
        control.update_task.cancel()
    for task in requests:
        task.cancel()
    with contextlib.suppress(asyncio.CancelledError):
        await asyncio.gather(
            reading, updates, *requests, *([control.update_task] if control.update_task else []),
            return_exceptions=True,
        )
    await control.shutdown()
    return 0


def main() -> int:
    logging.basicConfig(
        level=logging.INFO,
        stream=sys.stderr,
        format="%(asctime)s %(levelname).1s %(name)s: %(message)s",
        datefmt="%H:%M:%S",
    )
    with installation_lock() if sys.platform == "linux" else contextlib.nullcontext() as lock:
        return asyncio.run(serve(lock))

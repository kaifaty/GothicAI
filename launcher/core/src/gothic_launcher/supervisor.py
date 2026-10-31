"""Runs one session: the services, then the game once they are ready, until the game exits.

- Services start together, and the game waits until each answers `GET /health`.
- A service that exits during the session is restarted after 1, 2, 4… s (the game reconnects by itself).
  The game exiting ends the session.
- Every process runs in its own process group (`uv run` starts the service as its child), and stopping
  signals the whole group: SIGTERM, SIGKILL after a grace period.
- Output goes to `<name>.log` in the session's log directory, a restart appends to the same file.
- While the session runs, the health answers are polled, and a change (a TTS provider dropped out, the game
  connected) is reported as a status event.
- A `JOURNAL {json}` line in a process's output (the AI server with --journal: player and npc lines,
  actions) becomes a journal event with that entry.
"""

from __future__ import annotations

import asyncio
import contextlib
import json
import os
import shlex
import signal
import time
from collections import deque
from collections.abc import AsyncIterator, Callable
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, BinaryIO, Literal

import httpx

from .commands import Proc, child_env
from .state import SessionFile

READY_TIMEOUT_S = 180.0  # the first `uv run` of a service may install its dependencies
STOP_GRACE_S = 5.0
RESTART_DELAYS_S = (1, 2, 4, 8, 16, 30)
STABLE_S = 60.0  # a service that ran this long restarts after the shortest delay again
STATUS_EVERY_S = 3.0
TAIL_LINES = 40

EventKind = Literal[
    "starting", "ready", "status", "exited", "restarting", "stopped", "failed", "journal", "waiting"
]
JOURNAL_PREFIX = "JOURNAL "


@dataclass(frozen=True)
class Event:
    proc: str
    kind: EventKind
    detail: str = ""
    data: dict[str, Any] | None = None  # the health answer for ready/status, the entry for journal


class NotReady(Exception):
    pass


@dataclass
class _Running:
    proc: Proc
    process: asyncio.subprocess.Process
    log: BinaryIO
    started: float
    reader: asyncio.Task[None] | None = None
    tail: deque[str] = field(default_factory=lambda: deque(maxlen=TAIL_LINES))


class Supervisor:
    def __init__(
        self,
        services: list[Proc],
        game: Proc | None,
        log_dir: Path,
        on_event: Callable[[Event], None],
        *,
        session: SessionFile | None = None,
        echo: Callable[[str, str], None] | None = None,
        preparation: Proc | None = None,
    ) -> None:
        self.services = services
        self.game = game
        self.preparation = preparation
        self.log_dir = log_dir
        self.on_event = on_event
        self.session = session or SessionFile()
        self.echo = echo  # (process name, line): output as it comes, besides the log
        self.running: dict[str, _Running] = {}
        self._stop = asyncio.Event()
        self._status: dict[str, dict[str, Any] | None] = {}

    def stop(self) -> None:
        """Ends the session: the game too, if it runs."""
        self._stop.set()

    def tail(self, name: str) -> list[str]:
        r = self.running.get(name)
        return list(r.tail) if r else []

    async def run(self) -> int:
        """The game's exit code; 1 when a service could not start; 0 when stopped (or without a game)."""
        if not self.session.held:  # the CLI takes it before anything else; AlreadyRunning goes to the caller
            for name, pgid in self.session.acquire().items():
                left = f"left over by a launcher that was killed (group {pgid})"
                self.on_event(Event(name, "stopped", left))
        self.log_dir.mkdir(parents=True, exist_ok=True)
        tasks: list[asyncio.Task[Any]] = []
        try:
            if self._stop.is_set():
                return 0
            if self.preparation is not None:
                code = await self._prepare()
                if code or self._stop.is_set():
                    return code
            if not await self._start_services():
                return 0 if self._stop.is_set() else 1
            tasks = [asyncio.create_task(self._keep(p)) for p in self.services]
            tasks.append(asyncio.create_task(self._watch_status()))
            if self.game is None:
                if self.services:
                    await self._stop.wait()
                return 0
            if self._stop.is_set():
                return 0
            game = await self._spawn(self.game)
            ended = asyncio.create_task(_exit_code(game.process))
            stopping = asyncio.create_task(self._stop.wait())
            await asyncio.wait({ended, stopping}, return_when=asyncio.FIRST_COMPLETED)
            stopping.cancel()
            if not ended.done():
                ended.cancel()
                return 0
            code = ended.result()
            self.on_event(Event("game", "exited", f"code {code}"))
            return code
        finally:
            self._stop.set()
            for t in tasks:
                t.cancel()
            await asyncio.gather(*tasks, return_exceptions=True)
            await self._stop_all()
            self.session.release()

    async def _prepare(self) -> int:
        assert self.preparation is not None
        r = await self._spawn(self.preparation)
        ended = asyncio.create_task(_exit_code(r.process))
        stopping = asyncio.create_task(self._stop.wait())
        try:
            await asyncio.wait({ended, stopping}, return_when=asyncio.FIRST_COMPLETED)
            if self._stop.is_set():
                return 0  # run's finally stops the converter's process group before returning
            code = ended.result()
            await self._finish(r, signal.SIGTERM)
            if code:
                self.on_event(Event("prepare", "failed", f"exited with code {code}\n{_indent(list(r.tail))}"))
            else:
                self.on_event(Event("prepare", "ready", f"in {time.monotonic() - r.started:.1f}s"))
            return code
        finally:
            ended.cancel()
            stopping.cancel()
            await asyncio.gather(ended, stopping, return_exceptions=True)

    async def _start_services(self) -> bool:
        """False as soon as one service fails to come up (its last output in the event), or on stop()."""
        for p in self.services:
            await self._spawn(p)
        pending = {asyncio.create_task(self._wait_ready(p)): p for p in self.services}
        stopping = asyncio.create_task(self._stop.wait())
        try:
            while pending:
                done, _ = await asyncio.wait({*pending, stopping}, return_when=asyncio.FIRST_COMPLETED)
                if stopping in done:
                    return False
                for task in done:
                    p = pending.pop(task)
                    if (err := task.exception()) is not None:
                        await self._drain(self.running[p.name])
                        self.on_event(Event(p.name, "failed", f"{err}\n{_indent(self.tail(p.name))}"))
                        return False
                    self._status[p.name] = task.result()
                    took = time.monotonic() - self.running[p.name].started
                    self.on_event(Event(p.name, "ready", f"in {took:.1f}s", task.result()))
            return True
        finally:
            for task in (*pending, stopping):
                task.cancel()
            await asyncio.gather(*pending, stopping, return_exceptions=True)

    async def _keep(self, proc: Proc) -> None:
        """Restarts the service whenever it exits, until the session stops."""
        failures = 0
        while not self._stop.is_set():
            r = self.running[proc.name]
            code = await _exit_code(r.process)
            if self._stop.is_set():
                return
            await self._finish(r, signal.SIGKILL)  # whatever is left of its group would hold the port
            failures = 0 if time.monotonic() - r.started >= STABLE_S else failures + 1
            delay = RESTART_DELAYS_S[min(max(failures - 1, 0), len(RESTART_DELAYS_S) - 1)]
            self.on_event(Event(proc.name, "exited", f"code {code}\n{_indent(list(r.tail)[-10:])}"))
            self.on_event(Event(proc.name, "restarting", f"in {delay}s"))
            with contextlib.suppress(TimeoutError):
                await asyncio.wait_for(self._stop.wait(), delay)
                return
            await self._spawn(proc)
            try:
                health = await self._wait_ready(proc)
            except NotReady as err:
                self.on_event(Event(proc.name, "failed", str(err)))
                await self._finish(self.running[proc.name], signal.SIGKILL)
                continue
            self._status[proc.name] = health
            self.on_event(Event(proc.name, "ready", "after restart", health))

    async def _spawn(self, proc: Proc) -> _Running:
        log = (self.log_dir / f"{proc.name}.log").open("ab")
        log.write(f"--- {time.strftime('%Y-%m-%d %H:%M:%S')} {shlex.join(proc.argv)}\n".encode())
        log.flush()
        self.on_event(Event(proc.name, "starting"))
        proc.cwd.mkdir(parents=True, exist_ok=True)
        process = await asyncio.create_subprocess_exec(
            *proc.argv,
            cwd=proc.cwd,
            env=child_env(proc.env),
            stdin=asyncio.subprocess.DEVNULL,
            stdout=asyncio.subprocess.DEVNULL if proc.name == "prepare" else asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE if proc.name == "prepare" else asyncio.subprocess.STDOUT,
            start_new_session=True,  # own process group, and Ctrl-C in the terminal reaches only us
            limit=1 << 22,
        )
        r = _Running(proc, process, log, time.monotonic())
        r.reader = asyncio.create_task(self._read(r))
        self.running[proc.name] = r
        self.session.add(proc.name, process.pid)
        return r

    async def _read(self, r: _Running) -> None:
        stream = r.process.stderr if r.proc.name == "prepare" else r.process.stdout
        assert stream is not None
        while (line := await stream.readline()) and not r.log.closed:
            if r.proc.stamp:
                line = time.strftime("%H:%M:%S ").encode() + line
            r.log.write(line)
            r.log.flush()
            text = line.decode(errors="replace").rstrip("\n")
            r.tail.append(text)
            if self.echo is not None:
                self.echo(r.proc.name, text)
            if r.proc.name == "prepare":
                self.on_event(Event("prepare", "status", text))
                continue
            if text.startswith(JOURNAL_PREFIX):
                with contextlib.suppress(ValueError):
                    entry = json.loads(text.removeprefix(JOURNAL_PREFIX))
                    if isinstance(entry, dict):
                        self.on_event(Event(r.proc.name, "journal", data=entry))

    async def _wait_ready(self, proc: Proc) -> dict[str, Any]:
        if proc.health is None:
            return {}
        return await wait_ready(self.running[proc.name].process, proc.health)

    async def _watch_status(self) -> None:
        async with httpx.AsyncClient(timeout=2.0, trust_env=False) as http:
            while True:
                await asyncio.sleep(STATUS_EVERY_S)
                for p in self.services:
                    r = self.running.get(p.name)
                    if p.health is None or r is None or r.process.returncode is not None:
                        continue  # a restart reports by itself
                    try:
                        resp = await http.get(p.health)
                        health = resp.json() if resp.status_code == 200 else {"ok": False}
                    except (httpx.HTTPError, ValueError):
                        health = {"ok": False}
                    if health != self._status.get(p.name):
                        self._status[p.name] = health
                        self.on_event(Event(p.name, "status", json.dumps(health, ensure_ascii=False), health))

    async def _stop_all(self) -> None:
        """The game first, then the services in reverse start order (the server before ASR and TTS)."""
        order = [
            n for n in ("game", *(p.name for p in reversed(self.services)), "prepare") if n in self.running
        ]
        for name in order:
            r = self.running[name]
            if r.log.closed:
                continue
            if r.process.returncode is None:
                self.on_event(Event(name, "stopped"))
            await self._finish(r, signal.SIGTERM)

    async def _finish(self, r: _Running, sig: signal.Signals) -> None:
        """Stops the process group (SIGKILL if `sig` is not enough) and closes the log."""
        pgid = r.process.pid
        _killpg(pgid, sig)
        if sig != signal.SIGKILL:
            try:
                await asyncio.wait_for(_exit_code(r.process), STOP_GRACE_S)
            except TimeoutError:
                pass
        deadline = time.monotonic() + STOP_GRACE_S
        while _group_alive(pgid) and time.monotonic() < deadline:  # noqa: ASYNC110 (no event for this)
            await asyncio.sleep(0.05)  # members other than the leader, e.g. the service under `uv run`
        _killpg(pgid, signal.SIGKILL)
        await _exit_code(r.process)
        await self._drain(r)
        r.log.close()
        if self.running.get(r.proc.name) is r:
            self.session.remove(r.proc.name)

    @staticmethod
    async def _drain(r: _Running) -> None:
        """Lets the reader take the output still in the pipe."""
        if r.reader is not None and not r.reader.done():
            with contextlib.suppress(TimeoutError, asyncio.CancelledError):
                await asyncio.wait_for(asyncio.shield(r.reader), 1.0)


async def wait_ready(process: asyncio.subprocess.Process, health: str) -> dict[str, Any]:
    """The service's health answer once it gives one; NotReady if it exits first or takes too long."""
    deadline = time.monotonic() + READY_TIMEOUT_S
    exited = asyncio.ensure_future(_exit_code(process))
    try:
        async with httpx.AsyncClient(timeout=2.0, trust_env=False) as http:
            while True:
                if exited.done():
                    raise NotReady(f"exited with code {exited.result()} before it was ready")
                with contextlib.suppress(httpx.HTTPError, ValueError):
                    resp = await http.get(health)
                    if resp.status_code == 200:
                        return resp.json()
                if time.monotonic() > deadline:
                    raise NotReady(f"not ready after {READY_TIMEOUT_S:.0f}s")
                await asyncio.wait({exited}, timeout=0.2)
    finally:
        exited.cancel()


@contextlib.asynccontextmanager
async def temporary(proc: Proc, log: Path, session: SessionFile) -> AsyncIterator[asyncio.subprocess.Process]:
    """One service outside a game session, for a check (the setup's microphone and voice tests): started,
    ready (NotReady carries the end of its output), stopped with its group afterwards. Its group goes
    into the session file, so a killed launcher's next start stops it."""
    log.parent.mkdir(parents=True, exist_ok=True)
    proc.cwd.mkdir(parents=True, exist_ok=True)
    with log.open("wb") as out:
        process = await asyncio.create_subprocess_exec(
            *proc.argv,
            cwd=proc.cwd,
            env=child_env(proc.env),
            stdin=asyncio.subprocess.DEVNULL,
            stdout=out,
            stderr=asyncio.subprocess.STDOUT,
            start_new_session=True,
        )
        session.add(proc.name, process.pid)
        try:
            if proc.health is not None:
                try:
                    await wait_ready(process, proc.health)
                except NotReady as err:
                    out.flush()
                    text = await asyncio.to_thread(log.read_text, errors="replace")
                    tail = text.splitlines()[-TAIL_LINES:]
                    raise NotReady(f"{err}\n{_indent(tail)}") from None
            yield process
        finally:
            _killpg(process.pid, signal.SIGTERM)
            with contextlib.suppress(TimeoutError):
                await asyncio.wait_for(_exit_code(process), STOP_GRACE_S)
            _killpg(process.pid, signal.SIGKILL)
            await _exit_code(process)
            session.remove(proc.name)


async def _exit_code(process: asyncio.subprocess.Process) -> int:
    """`process.wait()` also waits for its pipes to close, and a child left in the group (the service under
    a killed `uv run`) keeps them open. The exit code is known as soon as the process exits."""
    while process.returncode is None:  # noqa: ASYNC110 (asyncio offers no event for the exit alone)
        await asyncio.sleep(0.05)
    return process.returncode


def _killpg(pgid: int, sig: signal.Signals) -> None:
    with contextlib.suppress(ProcessLookupError, PermissionError):
        os.killpg(pgid, sig)


def _group_alive(pgid: int) -> bool:
    try:
        os.killpg(pgid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


def _indent(lines: list[str]) -> str:
    return "\n".join(f"    {line}" for line in lines)

#!/usr/bin/env python3.14
"""File-backed resource queue. Each submitted request has a finite waiter, no daemon.

The only long-lived processes are pending submit waiters and running systemd units.
All systemd calls happen outside queue.lock; starting reserves resources atomically.
"""
from __future__ import annotations

import argparse
import contextlib
import fcntl
import importlib.util
import json
import math
import os
from pathlib import Path
import re
import select
import shutil
import signal
import socket
import statistics
import subprocess
import sys
import threading
import time
import uuid

FORMAT_VERSION = 1
GIB = 1024**3
PROFILES = {
    "ryzen": {
        "live_cpus": "0,1,16,17",
        "cpus": "2-15,18-31",
        "threads": 28,
        "mem": 20 * GIB,
        "display": 3,
        "gpu_mem": 10 * GIB,
        "quiet": True,
        "min_disk": 20 * GIB,
    },
}
TERMINAL = {"done", "cancelled", "timeout", "failed"}
ACTIVE = {"starting", "running"}
SCRIPT = str(Path(__file__).resolve())
POLL = 0.25
QUEUED_POLL = 1
RECONCILE_INTERVAL = 5
RETENTION = 24 * 3600


class QueueError(Exception):
    pass


def archive_module():
    """Load the shared archiver from a checkout or its installed library path."""
    if "nas_archive" not in sys.modules:
        path = Path(SCRIPT).with_name("nas_archive.py")
        if not path.is_file():
            path = Path(SCRIPT).parent.parent / "lib" / "nas_archive.py"
        spec = importlib.util.spec_from_file_location("nas_archive", path)
        module = importlib.util.module_from_spec(spec)
        sys.modules[spec.name] = module
        try:
            spec.loader.exec_module(module)
        except BaseException:
            sys.modules.pop(spec.name, None)
            raise
    return sys.modules["nas_archive"]


@contextlib.contextmanager
def archive_stop_timeout():
    """Extend only ExecStopPost's transfer, keeping the command's stop grace intact."""
    address = os.environ.get("NOTIFY_SOCKET")
    if not address:
        yield
        return
    if address.startswith("@"):
        address = "\0" + address[1:]
    stopped = threading.Event()

    def heartbeat():
        with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as channel:
            channel.settimeout(1)
            while not stopped.is_set():
                try:
                    channel.sendto(b"EXTEND_TIMEOUT_USEC=60000000", address)
                except OSError:
                    return
                stopped.wait(20)

    worker = threading.Thread(target=heartbeat, name="gq-archive-timeout", daemon=True)
    worker.start()
    try:
        yield
    finally:
        stopped.set()
        worker.join()


def duration(value):
    match = re.fullmatch(r"(\d+(?:\.\d+)?)([smh]?)", value)
    if not match:
        raise argparse.ArgumentTypeError("время: секунды либо 30s, 15m, 2h")
    number = float(match[1]) * {"": 1, "s": 1, "m": 60, "h": 3600}[match[2]]
    if not math.isfinite(number) or number <= 0:
        raise argparse.ArgumentTypeError("время должно быть положительным")
    return number


def memory(value):
    match = re.fullmatch(r"(\d+(?:\.\d+)?)([KMGT]?)(?:I?B)?", value.upper())
    if not match:
        raise argparse.ArgumentTypeError("память: число байт либо 512M, 8G")
    return int(float(match[1]) * 1024 ** (" KMGT".index(match[2]) if match[2] else 0))


def cpu_quota(value):
    match = re.fullmatch(r"(\d+(?:\.\d+)?)(?:%)?", str(value))
    if match:
        number = float(match[1])
        if math.isfinite(number) and number > 0:
            return f"{match[1]}%"
    raise argparse.ArgumentTypeError("квота CPU должна быть положительным числом: 200% или 200")


def process_token(pid):
    try:
        # comm may contain spaces and parentheses; fields after the final ')' are stable.
        fields = Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()
        if fields[0] == "Z":
            return None
        return fields[19]
    except (OSError, IndexError):
        return None


def alive(waiter):
    return waiter is None or process_token(waiter["pid"]) == waiter["start"]


def same_attempt(job, snapshot):
    return (job.get("attempt"), job.get("reserved")) == (snapshot.get("attempt"), snapshot.get("reserved"))


def exclusive_display(job):
    # Sharing is opt-in: older clients cannot isolate their ports by slot.
    return job.get("display", False) and (
        job.get("display_exclusive", False) or not job.get("display_shared", False))


def systemctl(*args):
    return subprocess.run(["systemctl", "--user", *args], capture_output=True, text=True)


def exec_property(args):
    # systemd's Exec* syntax, not shell syntax. Disable specifier/environment expansion.
    return " ".join('"' + str(s).replace("\\", "\\\\").replace('"', '\\"')
                    .replace("%", "%%").replace("$", "$$") + '"' for s in args)


def unit_cgroup(job_id, membership_path=None):
    """The stop callback is still inside the unit, before systemd removes its cgroup."""
    try:
        memberships = Path(membership_path or "/proc/self/cgroup").read_text().splitlines()
    except OSError:
        return None
    for membership in memberships:
        fields = membership.split(":", 2)
        if (len(fields) == 3 and fields[0:2] == ["0", ""]
                and fields[2].endswith(f"/gq-{job_id}.service")):
            return fields[2]
    return None


def cgroup_memory_stats(job_id, group=None, cgroup_root=None):
    group = unit_cgroup(job_id) or group
    result = {"oom_kill": None, "memory_peak": None}
    if not group:
        return result
    path = Path(cgroup_root or "/sys/fs/cgroup") / group.lstrip("/")
    try:
        events = dict(line.split() for line in (path / "memory.events").read_text().splitlines())
        result["oom_kill"] = int(events["oom_kill"])
    except (OSError, KeyError, ValueError):
        pass
    try:
        result["memory_peak"] = int((path / "memory.peak").read_text())
    except (OSError, ValueError):
        pass
    return result


class Queue:
    def __init__(self, home=None, profile=None, legacy_lock=None, proc_locks=None):
        self.home = Path(home or os.environ.get("GQ_HOME", "~/gothic-ai/queue")).expanduser().resolve()
        self.legacy_lock = Path(legacy_lock or os.environ.get(
            "GQ_LEGACY_LOCK", "~/gothic-ai/body-simulation.lock")).expanduser().resolve()
        self.proc_locks = Path(proc_locks or "/proc/locks")
        key = profile or os.environ.get("GQ_PROFILE")
        if not key:
            profile_path = Path.home() / "gothic-ai/bin/gq.profile"
            try:
                key = profile_path.read_text().strip()
            except FileNotFoundError:
                raise QueueError("профиль не задан: GQ_PROFILE=ryzen или scripts/gq install ryzen "
                                 f"(создаёт {profile_path})") from None
        if isinstance(key, dict):
            self.profile = key
            self.profile_name = "ryzen"
        else:
            if key not in PROFILES:
                raise QueueError(f"неизвестный профиль {key!r}; задайте GQ_PROFILE=ryzen")
            self.profile = PROFILES[key]
            self.profile_name = key
        self.home.mkdir(parents=True, exist_ok=True, mode=0o700)
        (self.home / "logs").mkdir(exist_ok=True, mode=0o700)
        self.legacy_lock.parent.mkdir(parents=True, exist_ok=True)
        self.legacy_lock.touch(exist_ok=True)
        with self.transaction():
            pass

    def now(self):
        return time.time()

    def disk_free(self):
        return shutil.disk_usage(self.home).free

    @contextlib.contextmanager
    def transaction(self):
        with (self.home / "queue.lock").open("a+") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            path = self.home / "state.json"
            state = json.loads(path.read_text()) if path.exists() else {"version": FORMAT_VERSION, "jobs": []}
            if state.get("version") != FORMAT_VERSION:
                raise QueueError(f"несовместимая версия формата {state.get('version')}; "
                                 f"этот gq поддерживает {FORMAT_VERSION}. Установите совместимый gq; "
                                 "не удаляйте состояние работающей очереди")
            original = json.dumps(state, ensure_ascii=False)
            yield state
            data = json.dumps(state, ensure_ascii=False)
            if data != original or not path.exists():
                tmp = self.home / "state.json.tmp"
                with tmp.open("w", encoding="utf-8") as output:
                    os.chmod(tmp, 0o600)
                    output.write(data + "\n")
                    output.flush()
                    os.fsync(output.fileno())
                os.replace(tmp, path)

    def event(self, job, event, **extra):
        # Called only within a transaction: journal lines from different processes never interleave.
        record = {k: job.get(k) for k in ("id", "owner", "class", "name", "threads", "mem",
                                        "gpu_mem", "display", "display_shared", "display_exclusive", "display_slot",
                                        "cpu_quota", "interactive", "max", "submitted", "start")}
        record.update(event=event, time=self.now(), **extra)
        with (self.home / "journal.jsonl").open("a", encoding="utf-8") as output:
            os.chmod(output.name, 0o600)
            output.write(json.dumps(record, ensure_ascii=False) + "\n")
            output.flush()
            os.fsync(output.fileno())

    def jobs(self):
        with self.transaction() as state:
            return [dict(j) for j in state["jobs"]]

    def get(self, job_id):
        for job in self.jobs():
            if job["id"] == job_id:
                return job
        archived = None
        for record in self.history():
            if record.get("event") == "finished" and record.get("id") == job_id:
                archived = dict(record, status=record["reason"], end=record["time"])
        if archived is not None:
            return archived
        raise QueueError(f"нет заявки {job_id}")

    def history(self):
        path = self.home / "journal.jsonl"
        if path.exists():
            with path.open(encoding="utf-8") as source:
                for line in source:
                    try:
                        yield json.loads(line)
                    except json.JSONDecodeError:
                        continue

    def claim_reconcile(self):
        with self.transaction() as state:
            now = self.now()
            previous = state.get("last_reconcile")
            if previous is not None and 0 <= now - previous < RECONCILE_INTERVAL:
                return False
            state["last_reconcile"] = now
            state["jobs"] = [job for job in state["jobs"] if not (
                job["status"] in TERMINAL and job.get("end", now) < now - RETENTION)]
        return True

    def validate(self, spec):
        p = self.profile
        if spec["class"] not in {"quick", "batch", "quiet"}:
            raise QueueError("неизвестный класс")
        if spec["class"] == "quiet" and not p["quiet"]:
            raise QueueError("quiet недоступен: иди на Ryzen")
        for key in ("threads", "mem", "max"):
            if not math.isfinite(spec[key]) or spec[key] <= 0:
                raise QueueError(f"{key} должен быть положительным")
        if spec["threads"] > p["threads"] or spec["mem"] > p["mem"]:
            raise QueueError("ресурсы заявки превышают ёмкость хоста")
        if not 0 <= spec["gpu_mem"] <= p["gpu_mem"]:
            raise QueueError("видеопамять заявки превышает ёмкость хоста")
        if spec["class"] == "batch" and spec["max"] > 7200:
            raise QueueError("--max для batch не больше 2h")
        if spec.get("interactive"):
            if self.profile_name != "ryzen" or spec["class"] != "quick" or not spec["display_exclusive"]:
                raise QueueError("--interactive требует quick --display-exclusive на Ryzen")
            if spec["max"] > 6 * 3600:
                raise QueueError("--max для --interactive не больше 6h")
        if not spec["command"]:
            raise QueueError("после -- нужна команда")
        if spec.get("archive"):
            archiver = archive_module()
            try:
                spec["archive"] = [str(archiver.validate_source(path, require_absolute=True))
                                   for path in spec["archive"]]
            except archiver.ArchiveError as error:
                raise QueueError(str(error)) from None

    def submit(self, spec, waiter=None):
        spec = dict(spec)
        spec.setdefault("gpu_mem", 0)
        spec.setdefault("display", False)
        spec.setdefault("display_shared", False)
        spec.setdefault("display_exclusive", False)
        if spec["display_exclusive"]:
            spec["display"] = True
        if spec.get("cpu_quota") is not None:
            try:
                spec["cpu_quota"] = cpu_quota(spec["cpu_quota"])
            except argparse.ArgumentTypeError as error:
                raise QueueError(str(error)) from None
        if "owner" not in spec:
            spec["owner"] = owner()
        self.validate(spec)
        job = dict(spec, id=uuid.uuid4().hex[:12], status="queued", submitted=self.now(),
                   start=None, waiter=waiter, cwd=str(Path.cwd()), env=dict(os.environ))
        with self.transaction() as state:
            state["jobs"].append(job)
            self.event(job, "submitted")
        return job["id"]

    def terminal(self, job, reason, code):
        if job["status"] in TERMINAL:
            return
        end = self.now()
        job.update(status=reason, reason=reason, code=code, end=end)
        self.event(job, "finished", reason=reason, code=code,
                   oom_kill=job.get("oom_kill"), memory_peak=job.get("memory_peak"),
                   wait=(job["start"] or end) - job["submitted"],
                   duration=end - job["start"] if job["start"] else 0)

    def cancel(self, job_id, owner, force=False, reason="cancelled", *, expected=None, queued_only=False):
        stop = False
        found = False
        cancelled = False
        with self.transaction() as state:
            for job in state["jobs"]:
                if job["id"] != job_id:
                    continue
                found = True
                if job["owner"] != owner and not force:
                    raise QueueError("чужая заявка: нужен --force")
                if expected is not None and not same_attempt(job, expected):
                    continue
                if queued_only and job["status"] != "queued":
                    continue
                if job["status"] in ACTIVE:
                    job["cancel_reason"] = reason
                    stop = True
                    cancelled = True
                elif job["status"] == "queued":
                    self.terminal(job, reason, 143)
                    cancelled = True
        if not found:
            if queued_only:
                return False
            raise QueueError(f"нет заявки {job_id}")
        if stop:
            result = systemctl("stop", "--no-block", f"gq-{job_id}.service")
            if result.returncode:
                detail = result.stderr.strip() or result.stdout.strip()
                raise QueueError(f"systemctl stop gq-{job_id}: код {result.returncode}: {detail}")
        return cancelled

    def retry_cancel(self, job_id, owner, *, expected=None):
        try:
            return self.cancel(job_id, owner, expected=expected)
        except QueueError as error:
            # The cancellation flag is durable; another reconcile retries the stop.
            print(f"gq: {error}", file=sys.stderr)
            return False

    def finish(self, job_id, result, exit_code="exited", exit_status="0", *, expected=None):
        job = next((item for item in self.jobs() if item["id"] == job_id), None)
        if job is None or not job.get("archive"):
            return self._finish(job_id, result, exit_code, exit_status, expected=expected)
        # ExecStopPost and recovery may observe the same completion concurrently.
        # Serialize the slow copy separately; never hold queue.lock while copying.
        with (self.home / "logs" / f"{job_id}.archive.lock").open("a") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            job = self.get(job_id)
            if (job["status"] in ACTIVE and job.get("start") is not None
                    and not job.get("retry") and job.get("cancel_reason") != "cancelled"
                    and (expected is None or (job["status"] == expected["status"]
                                             and same_attempt(job, expected)))):
                if unit_cgroup(job_id):
                    with archive_stop_timeout():
                        self.archive(job)
                else:
                    error = "skipped after recovery; local data kept"
                    for directory in job["archive"]:
                        line = f"gq: warning: archive {directory}: {error} (0 bytes)"
                        self.append_log(job_id, line + "\n")
                        with self.transaction() as state:
                            current = next(item for item in state["jobs"] if item["id"] == job_id)
                            self.event(current, "archive", directory=directory, outcome="skipped",
                                       bytes=0, error=error)
            self._finish(job_id, result, exit_code, exit_status, expected=expected)

    def archive(self, job):
        def cancelled():
            return self.get(job["id"]).get("cancel_reason") == "cancelled"

        for directory in job["archive"]:
            if cancelled():
                break
            size = 0
            error = None
            try:
                with (self.home / "logs" / f"{job['id']}.log").open("a") as output:
                    with contextlib.redirect_stdout(output):
                        size = archive_module().move(directory, cancelled=cancelled)
            except Exception as exc:
                size = getattr(exc, "bytes", 0)
                error = " ".join(str(exc).splitlines())
            outcome = "error" if error is not None else "moved"
            line = (f"gq: warning: archive {directory}: {error} ({size} bytes)" if error is not None
                    else f"gq: archived {directory} ({size} bytes)")
            self.append_log(job["id"], line + "\n")
            with self.transaction() as state:
                current = next(item for item in state["jobs"] if item["id"] == job["id"])
                self.event(current, "archive", directory=directory, outcome=outcome,
                           bytes=size, error=error)

    def _finish(self, job_id, result, exit_code="exited", exit_status="0", *, expected=None):
        with self.transaction() as state:
            for job in state["jobs"]:
                if job["id"] != job_id or job["status"] not in ACTIVE:
                    continue
                if expected is not None and (job["status"] != expected["status"] or not same_attempt(job, expected)):
                    continue
                if job.get("retry") and not job.get("cancel_reason"):
                    job.update(status="queued", start=None, retry_pending=True)
                    job.pop("retry", None)
                    job.pop("display_slot", None)
                    self.event(job, "deferred", reason="legacy lock")
                    continue
                try:
                    code = int(exit_status)
                except ValueError:
                    code = 128 + getattr(signal, "SIG" + exit_status, 1)
                if exit_code != "exited" and code < 128:
                    code += 128
                reason = job.get("cancel_reason") or ("timeout" if result == "timeout" else
                         "done" if result == "success" and code == 0 else "failed")
                if reason == "timeout":
                    code = 124
                elif reason == "cancelled":
                    code = 143
                job.update(cgroup_memory_stats(job_id, job.get("cgroup")))
                self.terminal(job, reason, code)

    def started(self, job_id):
        with self.transaction() as state:
            for job in state["jobs"]:
                if job["id"] == job_id and job["status"] == "starting" and not job.get("cancel_reason"):
                    job.update(status="running", start=self.now())
                    group = unit_cgroup(job_id)
                    if group:
                        job["cgroup"] = group
                    invocation = os.environ.get("INVOCATION_ID", "")
                    if re.fullmatch(r"[0-9a-f]{32}", invocation):
                        job["invocation_id"] = invocation
                    self.event(job, "started")
                    return True
        return False

    def reconcile(self):
        if not self.claim_reconcile():
            return
        snapshot = self.jobs()
        for job in snapshot:
            if job.get("retry_pending") and job["status"] == "queued":
                previous = systemctl("show", f"gq-{job['id']}.service", "--property=LoadState")
                if "LoadState=not-found" in previous.stdout.splitlines():
                    with self.transaction() as state:
                        for current in state["jobs"]:
                            if (current["id"] == job["id"] and current["status"] == "queued"
                                    and same_attempt(current, job)):
                                current.pop("retry_pending", None)
            if (job["status"] not in TERMINAL and not job.get("cancel_reason")
                    and not alive(job.get("waiter"))):
                self.retry_cancel(job["id"], job["owner"], expected=job)
            if job["status"] not in ACTIVE:
                continue
            # ExecStopPost normally finalizes. Recover after reboot or failed/missing callback.
            result = systemctl("show", f"gq-{job['id']}.service", "--property=LoadState,ActiveState,Result,ExecMainCode,ExecMainStatus")
            props = dict(line.split("=", 1) for line in result.stdout.splitlines() if "=" in line)
            missing = props.get("LoadState") == "not-found"
            if result.returncode and not missing:
                continue  # A transient bus error must not free a live job's resources.
            # One tick can reserve several jobs before launching them in sequence.
            # A live launcher still owns every unstarted reservation, regardless of age.
            if missing and job["status"] == "starting" and job.get("launcher") and alive(job["launcher"]):
                continue
            if missing and self.now() - job.get("reserved", self.now()) > 5:
                self.finish(job["id"], "failed", "exited", "1", expected=job)
                continue
            active = props.get("ActiveState", "")
            if job.get("cancel_reason") and active in {"active", "activating", "deactivating", "reloading"}:
                self.retry_cancel(job["id"], job["owner"], expected=job)
            if active in {"inactive", "failed"} and self.now() - job.get("reserved", self.now()) > 5:
                self.finish(job["id"], props.get("Result", "failed"),
                            "exited" if props.get("ExecMainCode") == "1" else "killed",
                            props.get("ExecMainStatus", "1"), expected=job)

    def legacy_waiting(self):
        stat = self.legacy_lock.stat()
        try:
            lines = self.proc_locks.read_text().splitlines()
        except OSError as error:
            raise QueueError(f"нельзя прочитать {self.proc_locks}: {error}") from error
        for line in lines:
            parts = line.split()
            if "->" not in parts or "FLOCK" not in parts or "WRITE" not in parts:
                continue
            for part in parts:
                if part.count(":") != 2:
                    continue
                try:
                    major, minor, inode = part.split(":")
                    if (int(major, 16), int(minor, 16), int(inode)) == (
                            os.major(stat.st_dev), os.minor(stat.st_dev), stat.st_ino):
                        return True
                except ValueError:
                    continue
        return False

    @contextlib.contextmanager
    def legacy_probe(self, quiet=False):
        with self.legacy_lock.open("a+") as lock:
            try:
                fcntl.flock(lock, (fcntl.LOCK_EX if quiet else fcntl.LOCK_SH) | fcntl.LOCK_NB)
            except BlockingIOError:
                yield False
            else:
                yield True

    def tick(self):
        self.reconcile()
        free = self.disk_free()
        legacy_waiting = self.legacy_waiting()
        launch = []
        with self.transaction() as state:
            jobs = state["jobs"]
            active = [j for j in jobs if j["status"] in ACTIVE]
            queued = sorted((j for j in jobs if j["status"] == "queued"),
                            key=lambda j: not j.get("interactive", False))
            quiet_waiting = legacy_waiting or any(j["class"] == "quiet" for j in queued)
            exclusive_waiting = next((index for index, j in enumerate(queued)
                                      if exclusive_display(j)), None)
            interactive_waiting = any(j.get("interactive") for j in queued)
            if free < self.profile["min_disk"]:
                return
            batch_blocked = False
            skipped_owners = set()
            for index, job in enumerate(queued):
                interactive = job.get("interactive", False)
                # A person at the desktop waits only for windows already in use.
                # Background quiet/training jobs keep running, without admitting
                # another window while the player's exclusive request drains it.
                if not interactive and (any(j["class"] == "quiet" for j in active)
                                        or (interactive_waiting and job["display"])):
                    continue
                if job.get("retry_pending"):
                    if job["class"] == "batch":
                        batch_blocked = True
                    continue
                kind = job["class"]
                if kind == "batch":
                    if job["owner"] in skipped_owners:
                        continue
                    other_waits = any(j["status"] == "queued" and j["class"] == "batch"
                                      and j["owner"] != job["owner"] for j in queued)
                    same_active = any(j["class"] == "batch" and j["owner"] == job["owner"] for j in active)
                    if quiet_waiting or batch_blocked:
                        batch_blocked = True
                        continue
                    if other_waits and same_active:
                        # Preserve this owner's FIFO after yielding to another session.
                        skipped_owners.add(job["owner"])
                        continue
                    if sum(j["threads"] for j in active if j["class"] == "batch") + job["threads"] > self.profile["threads"]:
                        batch_blocked = True
                        continue
                if kind == "quiet" and (active or legacy_waiting):
                    continue
                if kind == "quick" and quiet_waiting and not interactive:
                    last_batch = max(((j["start"] or j.get("reserved", self.now())) + j["max"]
                                      for j in active if j["class"] == "batch"), default=0)
                    if self.now() + job["max"] >= last_batch:
                        continue
                if not interactive and any(
                        sum(j[resource] for j in active) + job[resource] > self.profile[resource]
                        for resource in ("mem", "gpu_mem")):
                    if kind == "batch":
                        batch_blocked = True
                    continue
                slot = None
                if job["display"]:
                    # Drain games ahead of the oldest exclusive, then admit no
                    # later games. Earlier batches retain FIFO; quiet retains its
                    # class priority, otherwise the two admission gates can cycle.
                    if (not interactive and exclusive_waiting is not None and index > exclusive_waiting
                            and kind != "quiet"):
                        if kind == "batch":
                            batch_blocked = True
                        continue
                    capacity = self.profile.get("display", 1)
                    occupied = set()
                    for current in active:
                        if current["display"]:
                            # Persisted jobs without display_shared are legacy
                            # clients and reserve every slot, even if they have one.
                            occupied.update(range(capacity) if exclusive_display(current)
                                            else [current.get("display_slot", 0)])
                    available = set(range(capacity)) - occupied
                    if not available or (exclusive_display(job) and occupied):
                        if kind == "batch":
                            batch_blocked = True
                        continue
                    slot = min(available)
                with (contextlib.nullcontext(True) if interactive else self.legacy_probe(kind == "quiet")) as available:
                    if not available:
                        break
                job.update(status="starting", reserved=self.now(), attempt=uuid.uuid4().hex,
                           launcher={"pid": os.getpid(), "start": process_token(os.getpid())})
                if slot is not None:
                    job["display_slot"] = slot
                active.append(job)
                launch.append(dict(job))
                if kind == "quiet":
                    break
        for job in launch:
            self.launch(job)

    def launch(self, job):
        # Recheck cancellation after releasing the reservation transaction.
        current = self.get(job["id"])
        if current["status"] != "starting" or not same_attempt(current, job):
            return
        if current.get("cancel_reason"):
            self.finish(job["id"], "cancelled", "exited", "143", expected=current)
            return
        env = {**job["env"], "GQ_HOME": str(self.home), "GQ_LEGACY_LOCK": str(self.legacy_lock),
               "GQ_PROFILE": self.profile_name}
        env.pop("GQ_DISPLAY_SLOT", None)
        if job["display"]:
            env["GQ_DISPLAY_SLOT"] = str(job["display_slot"])
        internal = [sys.executable, SCRIPT]
        post = exec_property(["/usr/bin/env", *(f"{k}={env[k]}" for k in (
            "GQ_HOME", "GQ_LEGACY_LOCK", "GQ_PROFILE")), *internal, "_finish", job["id"]])
        props = {
            "CPUAffinity": self.profile["cpus"].replace(",", " "),
            "CPUWeight": 1000 if job.get("interactive") else 10 if job["class"] == "batch" else 100,
            "Nice": 19 if job["class"] == "batch" else 0,
            "MemoryMax": job["mem"],
            "RuntimeMaxSec": f"{job['max'] * (1 if job.get('interactive') else 1.25):g}s",
            "TimeoutStopSec": 60, "KillMode": "control-group", "ExecStopPost": post,
            "CollectMode": "inactive-or-failed",
            "WorkingDirectory": job["cwd"],
            "StandardOutput": "append:" + str(self.home / "logs" / f"{job['id']}.log"),
            "StandardError": "inherit",
        }
        if job.get("archive"):
            # ExecStopPost extends its own timeout while verifying the transfer.
            props["NotifyAccess"] = "exec"
        if job.get("cpu_quota") is not None:
            props["CPUQuota"] = job["cpu_quota"]
        args = ["systemd-run", "--user", "--quiet", "--unit", "gq-" + job["id"]]
        args += [f"--property={k}={v}" for k, v in props.items()]
        args += [f"--setenv={k}={v}" for k, v in env.items() if k not in {
            "SERVICE_RESULT", "EXIT_CODE", "EXIT_STATUS", "INVOCATION_ID", "NOTIFY_SOCKET"}]
        args += ["--", *internal, "_execute", job["id"]]
        try:
            result = subprocess.run(args, capture_output=True, text=True, timeout=15)
            if result.returncode:
                self.append_log(job["id"], result.stderr)
                self.finish(job["id"], "failed", "exited", str(result.returncode), expected=job)
            elif self.get(job["id"]).get("cancel_reason"):
                self.retry_cancel(job["id"], job["owner"], expected=job)
        except OSError as error:
            self.append_log(job["id"], str(error) + "\n")
            self.finish(job["id"], "failed", "exited", "127", expected=job)
        except subprocess.TimeoutExpired as error:
            # A timed-out D-Bus request may have started the unit. Stop before releasing resources.
            self.append_log(job["id"], str(error) + "\n")
            self.retry_cancel(job["id"], job["owner"], expected=job)
        finally:
            # Only reservations still waiting for their launch call need a live launcher.
            # Once the call returns (including timeout), reconcile can recover a missing
            # unit without waiting for this run/pump process itself to exit.
            with self.transaction() as state:
                for current in state["jobs"]:
                    if current["id"] == job["id"] and same_attempt(current, job):
                        current.pop("launcher", None)

    def append_log(self, job_id, text):
        with (self.home / "logs" / f"{job_id}.log").open("a") as output:
            output.write(text)

    def estimate(self, job_id):
        jobs = self.jobs()
        target = self.get(job_id)
        ahead = 0
        for job in jobs:
            if job["id"] == job_id:
                break
            if job["status"] in ACTIVE:
                ahead += job["max"]
            elif job["status"] == "queued":
                ahead += job["max"]
        history = []
        for record in self.history():
            if (record.get("event") == "finished" and record.get("reason") == "done"
                    and record.get("name") == target["name"]):
                history.append(record["duration"])
        typical = statistics.median(history) if history else None
        return {"upper": ahead + (typical or 0), "typical": typical}



def unit_has_children(job_id, membership_path=None, cgroup_root=None):
    """Keep the in-unit lock holder alive until orphaned command children exit too."""
    membership_path = Path(membership_path or "/proc/self/cgroup")
    cgroup_root = Path(cgroup_root or "/sys/fs/cgroup")
    try:
        memberships = membership_path.read_text().splitlines()
    except OSError:
        return False  # Direct helper execution in the portable fake-systemd tests.
    for membership in memberships:
        fields = membership.split(":", 2)
        if len(fields) != 3 or not fields[2].endswith(f"/gq-{job_id}.service"):
            continue
        try:
            pids = (cgroup_root / fields[2].lstrip("/") / "cgroup.procs").read_text().split()
        except OSError:
            return True  # Fail closed: RuntimeMaxSec will still stop the unit.
        return any(int(pid) != os.getpid() for pid in pids)
    return False


def command_gate(fd, command):
    """Stay inside the unit without executing user code until admission is committed."""
    try:
        token = os.read(fd, 1)
    finally:
        os.close(fd)
    if token != b"1":
        return 143
    try:
        os.execvpe(command[0], command, os.environ)
    except OSError as error:
        print(error, file=sys.stderr, flush=True)
        return 127


def execute(queue, job_id):
    job = queue.get(job_id)
    deadline = None
    if job.get("interactive"):
        # An older, still-running pump may have launched this newly submitted
        # request with ordinary quick properties. The helper loads current code
        # from disk: fix only its own unit before allowing the command to start.
        result = systemctl("set-property", "--runtime", f"gq-{job_id}.service", "CPUWeight=1000")
        if result.returncode:
            detail = result.stderr.strip() or result.stdout.strip()
            raise QueueError(f"не удалось задать приоритет игры: {detail}")
        # RuntimeMaxSec cannot be changed on a running service. Keep the six-hour
        # ceiling here too, including when an old pump supplied max * 1.25.
        deadline = time.monotonic() + job["max"]
    # A legacy EX can win the race between reservation and unit start. Never wait here:
    # retry admission instead, so RuntimeMaxSec measures work, not lock waiting.
    with (contextlib.nullcontext(True) if job.get("interactive")
          else queue.legacy_probe(job["class"] == "quiet")) as acquired:
        if not acquired:
            with queue.transaction() as state:
                for current in state["jobs"]:
                    if current["id"] == job_id:
                        current["retry"] = True
            return 0
        if not queue.started(job_id):
            return 143
        # Keep the lock holder alive during shutdown. Latch a stop before the child
        # exists and forward later stops, including the gap inside Popen itself.
        child = None
        interrupted = 0
        timed_out = False
        read_fd = write_fd = None

        def stop(signum, _frame):
            nonlocal interrupted
            interrupted = signum
            if child is not None and child.returncode is None:
                with contextlib.suppress(ProcessLookupError):
                    os.kill(child.pid, signum)

        def monitor():
            nonlocal timed_out
            if deadline is not None and time.monotonic() >= deadline:
                if not timed_out:
                    queue.cancel(job_id, job["owner"], reason="timeout")
                    timed_out = True
            elif not alive(job.get("waiter")):
                queue.retry_cancel(job_id, job["owner"])

        previous = {sig: signal.signal(sig, stop) for sig in (signal.SIGTERM, signal.SIGINT)}
        try:
            read_fd, write_fd = os.pipe()
            try:
                child = subprocess.Popen(
                    [sys.executable, SCRIPT, "_command", str(read_fd), "--", *job["command"]],
                    stdin=subprocess.DEVNULL, pass_fds=(read_fd,))
            except OSError as error:
                print(error, file=sys.stderr, flush=True)
                return 128 + interrupted if interrupted else 127
            os.close(read_fd)
            read_fd = None
            # cancel() uses this same lock. If cancellation won, the child sees EOF
            # instead of permission to exec. If admission won, a child already exists
            # to receive systemd's cgroup signal or the helper's forwarded signal.
            with queue.transaction() as state:
                for current in state["jobs"]:
                    if (current["id"] == job_id and current["status"] == "running"
                            and same_attempt(current, job)):
                        if not alive(current.get("waiter")):
                            # EOF can end the child before either monitor loop runs.
                            # Persist the observed cause before releasing the gate.
                            if not current.get("cancel_reason"):
                                current["cancel_reason"] = "cancelled"
                        elif not current.get("cancel_reason") and not interrupted:
                            with contextlib.suppress(BrokenPipeError):
                                os.write(write_fd, b"1")
                        break
            os.close(write_fd)
            write_fd = None
            while child.poll() is None:
                monitor()
                time.sleep(POLL)
            while unit_has_children(job_id):
                monitor()
                time.sleep(POLL)
            if interrupted:
                return 128 + interrupted
            return child.returncode if child.returncode >= 0 else 128 - child.returncode
        finally:
            for fd in (read_fd, write_fd):
                if fd is not None:
                    os.close(fd)
            for sig, handler in previous.items():
                signal.signal(sig, handler)


def pump(queue, job_id):
    # One finite waiter per submit, detached from SSH. It exits as soon as its job ends.
    os.nice(19)
    while queue.get(job_id)["status"] not in TERMINAL:
        queue.tick()
        time.sleep(QUEUED_POLL if queue.get(job_id)["status"] == "queued" else POLL)
    return 0


def spawn_pump(queue, job_id):
    env = {**os.environ, "GQ_HOME": str(queue.home), "GQ_PROFILE": queue.profile_name,
           "GQ_LEGACY_LOCK": str(queue.legacy_lock)}
    subprocess.Popen([sys.executable, SCRIPT, "_pump", job_id], env=env, start_new_session=True,
                     stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def submit_job(queue, spec):
    """Own a detached request until its ID has reached the caller.

    A signal may arrive after submit() persists the job but before it returns its
    ID. Record that signal without unwinding the transaction; once submit()
    returns we can cancel the exact request. Later signals can interrupt pump
    startup or stdout immediately because the ID is already known.
    """
    class Interrupted(Exception):
        pass

    job_id = None
    interrupted = 0
    delivered = False
    closing = False

    def stop(signum, _frame):
        nonlocal interrupted
        interrupted = interrupted or signum
        if job_id is not None and not closing:
            raise Interrupted

    previous = {sig: signal.signal(sig, stop)
                for sig in (signal.SIGHUP, signal.SIGTERM, signal.SIGINT)}
    try:
        job_id = queue.submit(spec)
        if interrupted:
            raise Interrupted
        spawn_pump(queue, job_id)
        print(job_id, flush=True)
        delivered = True
        return 0
    except Interrupted:
        return 128 + interrupted
    finally:
        closing = True
        try:
            if job_id is not None and (not delivered or interrupted):
                queue.retry_cancel(job_id, spec["owner"])
        finally:
            for sig, handler in previous.items():
                signal.signal(sig, handler)


def stream_log(queue, job_id, offset):
    path = queue.home / "logs" / f"{job_id}.log"
    if path.exists():
        with path.open("rb") as log:
            log.seek(offset)
            chunk = log.read()
            if chunk:
                sys.stdout.buffer.write(chunk)
                sys.stdout.buffer.flush()
            return log.tell()
    return offset


def wait_job(queue, job_id, run=False, wait_max=None, follow=True):
    os.nice(19)
    started = queue.now()
    next_report = 0
    offset = 0
    disconnected = False

    def interrupted(signum, frame):
        nonlocal disconnected
        disconnected = True

    old = {}
    if run:
        for signum in (signal.SIGHUP, signal.SIGTERM, signal.SIGINT):
            old[signum] = signal.signal(signum, interrupted)
    try:
        while True:
            if run and not disconnected:
                try:
                    if select.select([sys.stdin], [], [], 0)[0]:
                        disconnected = not bool(os.read(sys.stdin.fileno(), 4096))
                except (OSError, ValueError):
                    disconnected = True
            if disconnected:
                queue.retry_cancel(job_id, queue.get(job_id)["owner"])
            queue.tick()
            jobs = queue.jobs()
            job = next((j for j in jobs if j["id"] == job_id), None)
            if job is None:
                job = queue.get(job_id)  # Completed requests may already be archived.
            if follow:
                offset = stream_log(queue, job_id, offset)
            if job["status"] in TERMINAL:
                return job["code"]
            if wait_max is not None and job["status"] == "queued" and queue.now() - started >= wait_max:
                if queue.cancel(job_id, job["owner"], queued_only=True):
                    print("Ryzen занят", file=sys.stderr)
                    return 75
                continue  # Admission/completion won the race: return its actual result.
            if job["status"] == "queued" and queue.now() >= next_report:
                if job.get("interactive"):
                    windows = [f"{j['id']} ({j['owner']}, {j['name']})"
                               for j in jobs if j["status"] in ACTIVE and j["display"]]
                    detail = "ждём игры: " + ", ".join(windows) if windows else "ожидаем допуска"
                    print(f"gq {job_id}: {detail}", file=sys.stderr, flush=True)
                else:
                    waiting = [j["id"] for j in jobs if j["status"] == "queued"]
                    estimate = queue.estimate(job_id)
                    typical = f", медиана имени {estimate['typical']:.0f}s" if estimate["typical"] is not None else ""
                    print(f"gq {job_id}: место {waiting.index(job_id) + 1}, старт ≤ {estimate['upper']:.0f}s{typical}",
                          file=sys.stderr, flush=True)
                next_report = queue.now() + 60
            time.sleep(QUEUED_POLL if job["status"] == "queued" else POLL)
    finally:
        for signum, handler in old.items():
            signal.signal(signum, handler)


def owner():
    if "GQ_OWNER" in os.environ:
        return os.environ["GQ_OWNER"]
    for path in (Path.cwd(), Path(SCRIPT).parent):
        ancestors = (path.resolve(), *path.resolve().parents)
        # Codex worktrees use .../worktrees/<session>/<repo>, even in copied trees
        # without Git metadata. Subdirectories retain the session's identity.
        for root in ancestors:
            if root.parent.parent.name == "worktrees":
                return root.parent.name
        for root in ancestors:
            marker = root / ".git"
            if marker.exists():
                if marker.is_file():
                    gitdir = marker.read_text().strip().removeprefix("gitdir: ")
                    metadata = (root / gitdir).resolve()
                    if metadata.parent.name == "worktrees":
                        return metadata.name
                return root.name
    raise QueueError("не удалось определить worktree; задайте GQ_OWNER")


def status(queue):
    print("ID           ВЛАДЕЛЕЦ         КЛАСС  СОСТОЯНИЕ  РЕСУРСЫ                         СТАРТ    ПОТОЛОК ОЦЕНКА")
    for job in queue.jobs():
        if job["status"] in TERMINAL:
            continue
        display = "все" if exclusive_display(job) else str(int(job["display"]))
        slot = job.get("display_slot", 0 if job["display"] and job["status"] in ACTIVE else "—")
        resources = (f"{job['threads']}t {job['mem']/GIB:g}G gpu={job['gpu_mem']/GIB:g}G "
                     f"окно={display} слот={slot}")
        start = time.strftime("%H:%M:%S", time.localtime(job["start"])) if job["start"] else "—"
        estimate = queue.estimate(job["id"])
        typical = f" median={estimate['typical']:.0f}s" if estimate["typical"] is not None else ""
        print(f"{job['id']} {job['owner'][:16]:16} {job['class']:6} {job['status']:10} "
              f"{resources:31} {start:8} {job['max']:g}s ≤{estimate['upper']:.0f}s{typical}")
    free = queue.disk_free()
    print(f"Диск: свободно {free/GIB:.1f}G; минимум {queue.profile['min_disk']/GIB:g}G")
    if queue.legacy_waiting():
        print("Старый LOCK_EX ждёт: допуск batch остановлен, машина осушается")
    if free < queue.profile["min_disk"]:
        print("Мало места: новые заявки остановлены. Крупные папки:")
        result = subprocess.run(["du", "-h", "--max-depth=1", str(Path.home() / "gothic-ai")],
                                capture_output=True, text=True)
        print(result.stdout, end="")


def parser():
    p = argparse.ArgumentParser(description=__doc__)
    sub = p.add_subparsers(dest="action", required=True)
    for name in ("run", "submit"):
        r = sub.add_parser(name)
        r.add_argument("--class", dest="class_", choices=["quick", "batch", "quiet"], required=True)
        r.add_argument("--threads", type=int, required=True)
        r.add_argument("--mem", type=memory, required=True)
        r.add_argument("--gpu-mem", type=memory, default=0)
        r.add_argument("--display", action="store_true")
        r.add_argument("--display-shared", action="store_true",
                       help="делить слоты окна с другими совместимыми заявками --display")
        r.add_argument("--display-exclusive", action="store_true")
        r.add_argument("--interactive", action="store_true",
                       help="игра пользователя: quick --display-exclusive, ждёт только занятые окна")
        r.add_argument("--cpu-quota", type=cpu_quota)
        r.add_argument("--max", type=duration, required=True)
        r.add_argument("--name", required=True)
        r.add_argument("--archive", action="append", default=[], metavar="DIR",
                       help="move results to NAS with verification after the command finishes")
        if name == "run":
            r.add_argument("--wait-max", type=float)
        r.add_argument("command", nargs=argparse.REMAINDER)
    sub.add_parser("status")
    r = sub.add_parser("_command")
    r.add_argument("fd", type=int)
    r.add_argument("command", nargs=argparse.REMAINDER)
    for name in ("wait", "logs", "cancel", "_execute", "_finish", "_pump"):
        r = sub.add_parser(name)
        r.add_argument("id")
        if name == "logs":
            r.add_argument("-f", "--follow", action="store_true")
        if name == "cancel":
            r.add_argument("--force", action="store_true")
    return p


def main(argv=None):
    args = parser().parse_args(argv)
    try:
        if args.action == "_command":
            command = args.command[1:] if args.command[:1] == ["--"] else args.command
            return command_gate(args.fd, command)
        queue = Queue()
        if args.action == "_execute":
            return execute(queue, args.id)
        if args.action == "_finish":
            queue.finish(args.id, os.environ.get("SERVICE_RESULT", "failed"),
                         os.environ.get("EXIT_CODE", "exited"), os.environ.get("EXIT_STATUS", "1"))
            return 0
        if args.action == "_pump":
            return pump(queue, args.id)
        if args.action != "cancel":
            queue.tick()
        if args.action in {"submit", "run"}:
            if args.action == "run" and args.wait_max is not None and (not math.isfinite(args.wait_max) or args.wait_max < 0):
                raise QueueError("--wait-max должен быть неотрицательным")
            command = args.command[1:] if args.command[:1] == ["--"] else args.command
            waiter = {"pid": os.getpid(), "start": process_token(os.getpid())} if args.action == "run" else None
            request = {
                "class": args.class_, "threads": args.threads, "mem": args.mem, "gpu_mem": args.gpu_mem,
                "display": args.display, "display_shared": args.display_shared,
                "display_exclusive": args.display_exclusive,
                "interactive": args.interactive,
                "cpu_quota": args.cpu_quota, "max": args.max,
                "name": args.name, "owner": owner(), "command": command}
            if args.archive:
                request["archive"] = args.archive
            if args.action == "submit":
                return submit_job(queue, request)
            job_id = queue.submit(request, waiter)
            return wait_job(queue, job_id, run=True, wait_max=args.wait_max)
        if args.action == "status":
            status(queue)
        elif args.action == "cancel":
            queue.cancel(args.id, owner(), force=args.force)
            queue.tick()
        elif args.action == "wait":
            queue.get(args.id)
            return wait_job(queue, args.id)
        elif args.action == "logs":
            if Path(args.id).name != args.id:
                raise QueueError("неверный id заявки")
            if not (queue.home / "logs" / f"{args.id}.log").exists():
                queue.get(args.id)
            if args.follow:
                os.nice(19)
            offset = 0
            while True:
                offset = stream_log(queue, args.id, offset)
                if not args.follow:
                    break
                try:
                    job = queue.get(args.id)
                except QueueError:
                    break  # A standalone archived log has no running producer.
                if job["status"] in TERMINAL:
                    # Completion may have happened after the read above. Once terminal,
                    # the command's output is final, so drain the remaining bytes.
                    stream_log(queue, args.id, offset)
                    break
                queue.tick()
                time.sleep(QUEUED_POLL if job["status"] == "queued" else POLL)
        return 0
    except (QueueError, OSError, ValueError) as error:
        print(f"gq: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())

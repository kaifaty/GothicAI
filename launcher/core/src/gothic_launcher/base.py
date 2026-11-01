"""Read the ready base selected by the game, without consulting its installation."""

from __future__ import annotations

import hashlib
import json
import os
import re
from pathlib import Path, PurePosixPath

try:
    import fcntl
except ImportError:  # Windows
    fcntl = None  # type: ignore[assignment]

_SHA256 = re.compile(r"[0-9a-f]{64}")


def _digest(value: object) -> bool:
    return isinstance(value, str) and _SHA256.fullmatch(value) is not None


def _share(lease) -> None:
    """A shared lock on the whole lease file, held until it is closed. Windows: LockFileEx over the same
    range as the engine's lease (common/basestore.cpp), so its cleanup skips a base in use here."""
    if fcntl is not None:
        fcntl.flock(lease, fcntl.LOCK_SH)
        return
    import ctypes
    import msvcrt

    class Overlapped(ctypes.Structure):  # OVERLAPPED
        _fields_ = [
            ("Internal", ctypes.c_void_p),
            ("InternalHigh", ctypes.c_void_p),
            ("Offset", ctypes.c_uint32),
            ("OffsetHigh", ctypes.c_uint32),
            ("hEvent", ctypes.c_void_p),
        ]

    lock = ctypes.WinDLL("kernel32", use_last_error=True).LockFileEx  # type: ignore[attr-defined]
    lock.restype = ctypes.c_int
    lock.argtypes = (ctypes.c_void_p, *(ctypes.c_uint32,) * 4, ctypes.POINTER(Overlapped))
    handle = msvcrt.get_osfhandle(lease.fileno())  # type: ignore[attr-defined]
    if not lock(handle, 0, 0, 0xFFFFFFFF, 0xFFFFFFFF, ctypes.byref(Overlapped())):
        raise ctypes.WinError(ctypes.get_last_error())  # type: ignore[attr-defined]


def _checked(raw: bytes, entry: dict, name: str) -> bytes:
    if type(entry.get("size")) is not int or entry["size"] < 0 or not _digest(entry.get("sha256")):
        raise ValueError(f"invalid file binding: {name}")
    if len(raw) != entry["size"]:
        raise ValueError(f"base file size differs: {name}")
    if hashlib.sha256(raw).hexdigest() != entry["sha256"]:
        raise ValueError(f"base file SHA-256 differs: {name}")
    return raw


class Base:
    def __init__(self, path: Path, cache: Path, expected: str | None = None) -> None:
        self.path = path.resolve()
        self._lease = None
        if self.path.parent == cache and _digest(self.path.name):
            self._lease = (cache / f"{self.path.name}.use").open("a+b")
            _share(self._lease)
        try:
            metadata = json.loads((self.path / "base.json").read_bytes())
            if (
                metadata.get("format") != "gothic-base"
                or metadata.get("version") != 2
                or not _digest(metadata.get("fingerprint"))
            ):
                raise ValueError("invalid base.json")
            self.fingerprint = metadata["fingerprint"]
            if expected is not None and self.fingerprint != expected:
                raise ValueError("pointer base fingerprint differs")
            binding = metadata["manifest"]
            if not isinstance(binding, dict) or binding.get("path") != "base-files.json":
                raise ValueError("invalid base manifest binding")
            manifest = json.loads(
                _checked((self.path / "base-files.json").read_bytes(), binding, "base-files.json")
            )
            if (
                manifest.get("format") != "gothic-base-files"
                or manifest.get("version") != 1
                or not isinstance(manifest.get("files"), list)
            ):
                raise ValueError("invalid base file manifest")
            self._files = {}
            for entry in manifest["files"]:
                name = entry["path"]
                relative = PurePosixPath(name)
                if (
                    not isinstance(name, str)
                    or not name
                    or relative.is_absolute()
                    or ".." in relative.parts
                    or str(relative) != name
                    or name in ("base.json", "base-files.json")
                    or name in self._files
                    or type(entry.get("size")) is not int
                    or entry["size"] < 0
                    or not _digest(entry.get("sha256"))
                ):
                    raise ValueError(f"invalid base manifest entry: {name}")
                self._files[name] = entry
        except Exception:
            self.close()
            raise

    def read(self, relative: str) -> bytes:
        try:
            entry = self._files[relative]
            path = self.path / relative
            if not path.resolve().is_relative_to(self.path):
                raise ValueError(f"base file leaves its directory: {relative}")
            return _checked(path.read_bytes(), entry, relative)
        except (OSError, ValueError, KeyError) as error:
            raise RuntimeError(f"base {self.path}: {error}") from error

    def json(self, relative: str):
        try:
            return json.loads(self.read(relative))
        except ValueError as error:
            raise RuntimeError(f"base {self.path}/{relative}: invalid JSON: {error}") from error

    def close(self) -> None:
        if self._lease is not None:
            self._lease.close()
            self._lease = None

    def __enter__(self) -> Base:
        return self

    def __exit__(self, *_args) -> None:
        self.close()


def resolve(game_dir: Path) -> Base:
    """Select an explicit base or this canonical game directory's P18a pointer."""
    game = Path(game_dir).expanduser().resolve()
    try:
        cache_name = os.environ.get("OPENGOTHIC_BASE_CACHE", str(Path.home() / ".cache/gothic-ai/base"))
        if not cache_name:
            raise ValueError("base cache path is empty")
        cache = Path(cache_name).expanduser().resolve()
        explicit = os.environ.get("OPENGOTHIC_BASE")
        if explicit:
            return Base(Path(explicit).expanduser(), cache)
        pointer = cache / "current" / f"{hashlib.sha256(str(game).encode()).hexdigest()}.json"
        current = json.loads(pointer.read_bytes())
        part = current["base"]
        if (
            current.get("version") != 1
            or current.get("gothic") != str(game)
            or not isinstance(part, dict)
            or not isinstance(part.get("path"), str)
            or not Path(part["path"]).is_absolute()
            or not _digest(part.get("fingerprint"))
        ):
            raise ValueError("invalid or foreign current base pointer")
        return Base(Path(part["path"]), cache, part["fingerprint"])
    except (OSError, ValueError, KeyError, TypeError, AttributeError) as error:
        raise RuntimeError(
            f"no ready base for {game}; run the game once with the installation "
            f"or set OPENGOTHIC_BASE: {error}"
        ) from error

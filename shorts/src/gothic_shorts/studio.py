"""A voice-over studio in the browser: the script as a teleprompter, the short's draft video playing the part
each line is said over, a take per press of the space bar, heard back with the picture, the best one chosen.

The short's folder (an explainer's workspace, outside the repository):

    script.ru.txt                one line of the voice per row, as it is read
    plan.json | plan.storyboard.json   where each line falls in the short (its words' times)
    render/preview.mp4           the draft the user voices over (optional)
    voice/raw/03-2.wav           take 2 of line 3: 48 kHz mono 24-bit, as the microphone gave it
    voice/raw/03-2.json          its length, peak, where the speech starts and ends, where the video was
    voice/takes.json             the chosen take of each line
    voice/room.wav               10 s of the room's silence

Nothing is deleted: a take thrown away goes to voice/trash/. Only ffmpeg runs here.
"""

from __future__ import annotations

import json
import mimetypes
import re
import shutil
import subprocess
import tempfile
import threading
import time
import webbrowser
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, unquote, urlparse

ROOT = Path.home() / ".local/share/gothic-ai/explain"
APP = Path(__file__).with_name("studio")
RATE = 48000
PREVIEW = "render/preview.mp4"


class StudioError(RuntimeError):
    pass


def workspace(arg: str) -> Path:
    """A short's folder: a path, or a name under ~/.local/share/gothic-ai/explain."""
    path = Path(arg).expanduser()
    if not path.is_dir():
        path = ROOT / arg
    if not path.is_dir():
        raise StudioError(f"no short {arg} (a folder, or a name in {ROOT})")
    return path


def script_lines(ws: Path, lang: str) -> list[str]:
    path = ws / f"script.{lang}.txt"
    if not path.is_file():
        raise StudioError(f"no {path.name} in {ws}: the voice's text, a line per row")
    return [row.strip() for row in path.read_text().splitlines() if row.strip()]


def planned(ws: Path, count: int) -> list[tuple[float, float] | None]:
    """Where each line falls in the short by its plan: (start, end) of its words, None without a plan."""
    for name in ("plan.json", "plan.storyboard.json"):
        path = ws / name
        if path.is_file():
            lines = json.loads(path.read_text()).get("lines", [])
            spans = [(ln[0]["start"], ln[-1]["end"]) if ln else None for ln in lines]
            return (spans + [None] * count)[:count]
    return [None] * count


def _ffmpeg(args: list[str]) -> str:
    done = subprocess.run(["ffmpeg", "-hide_banner", "-nostats", *args], capture_output=True, text=True)
    if done.returncode != 0:
        raise StudioError(f"ffmpeg failed: {done.stderr.strip()[-800:]}")
    return done.stderr


def measure(wav: Path) -> dict:
    """Length, peak (dBFS) and where the speech starts and ends (silence under -45 dB for 0.25 s)."""
    log = _ffmpeg(["-i", str(wav), "-af", "volumedetect,silencedetect=n=-45dB:d=0.25", "-f", "null", "-"])
    dur = _duration(wav)
    peak = re.search(r"max_volume: (-?[\d.]+|-inf) dB", log)
    starts = [max(0.0, float(x)) for x in re.findall(r"silence_start: (-?[\d.]+)", log)]
    ends = [float(x) for x in re.findall(r"silence_end: ([\d.]+)", log)]
    # silence from the very start: the speech begins where it ends; silence that runs to the end (no end
    # printed, or ending at the end): the speech stops where it begins
    speech_from = ends[0] if starts and starts[0] <= 0.05 and ends else 0.0
    to_end = starts and (len(ends) < len(starts) or ends[-1] >= dur - 0.05)
    speech_to = starts[-1] if to_end else dur
    if speech_to <= speech_from:
        speech_from, speech_to = 0.0, dur  # all silence (or too quiet to tell): the whole take
    return {
        "dur": round(dur, 3),
        "peak": None if peak is None or peak.group(1) == "-inf" else float(peak.group(1)),
        "speech": [round(speech_from, 3), round(speech_to, 3)],
    }


def _duration(wav: Path) -> float:
    out = subprocess.run(
        ["ffprobe", "-v", "error", "-show_entries", "format=duration", "-of", "csv=p=0", str(wav)],
        capture_output=True, text=True,
    )  # fmt: skip
    try:
        return float(out.stdout.strip())
    except ValueError as err:
        raise StudioError(f"can not read the length of {wav}") from err


class Studio:
    def __init__(self, ws: Path, lang: str = "ru"):
        self.ws, self.lang = ws, lang
        self.raw, self.trash = ws / "voice/raw", ws / "voice/trash"
        self.lock = threading.Lock()

    # -- state -------------------------------------------------------------------------------------------

    def _chosen(self) -> dict[str, int]:
        path = self.ws / "voice/takes.json"
        return json.loads(path.read_text()) if path.is_file() else {}

    def _choose(self, n: int, k: int | None) -> None:
        chosen = self._chosen()
        if k is None:
            chosen.pop(f"{n:02d}", None)
        else:
            chosen[f"{n:02d}"] = k
        path = self.ws / "voice/takes.json"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(dict(sorted(chosen.items())), indent=1) + "\n")

    def takes(self, n: int) -> list[dict]:
        out = []
        for wav in sorted(self.raw.glob(f"{n:02d}-*.wav"), key=lambda p: int(p.stem.split("-")[1])):
            info = wav.with_suffix(".json")
            meta = json.loads(info.read_text()) if info.is_file() else measure(wav)
            out.append({"k": int(wav.stem.split("-")[1]), "url": f"/media/voice/raw/{wav.name}", **meta})
        return out

    def session(self) -> dict:
        texts = script_lines(self.ws, self.lang)
        spans = planned(self.ws, len(texts))
        chosen = self._chosen()
        lines = []
        for i, (text, span) in enumerate(zip(texts, spans, strict=True), start=1):
            takes = self.takes(i)
            pick = chosen.get(f"{i:02d}")
            lines.append({"n": i, "text": text, "plan": span, "takes": takes, "chosen": pick})
        video = self.ws / PREVIEW
        return {
            "name": self.ws.name,
            "lang": self.lang,
            "lines": lines,
            "video": f"/media/{PREVIEW}?v={int(video.stat().st_mtime)}" if video.is_file() else None,
            "room": (self.ws / "voice/room.wav").is_file(),
        }

    # -- changes -----------------------------------------------------------------------------------------

    def _convert(self, body: bytes, dest: Path) -> None:
        """The browser's WAV (float or 16-bit, its own rate) as 48 kHz mono 24-bit."""
        dest.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.NamedTemporaryFile(suffix=".wav") as tmp:
            tmp.write(body)
            tmp.flush()
            _ffmpeg(["-y", "-i", tmp.name, "-ac", "1", "-ar", str(RATE), "-c:a", "pcm_s24le", str(dest)])

    def add_take(self, n: int, body: bytes, video_from: float | None) -> dict:
        with self.lock:
            ks = [t["k"] for t in self.takes(n)] + [
                int(p.stem.split("-")[1]) for p in self.trash.glob(f"{n:02d}-*.wav")
            ]
            k = max(ks, default=0) + 1
            wav = self.raw / f"{n:02d}-{k}.wav"
            self._convert(body, wav)
            meta = {**measure(wav), "video_from": video_from, "at": time.strftime("%Y-%m-%dT%H:%M:%S")}
            wav.with_suffix(".json").write_text(json.dumps(meta, indent=1) + "\n")
            self._choose(n, k)  # the newest take is the one until another is chosen
            return {"k": k, "url": f"/media/voice/raw/{wav.name}", **meta}

    def choose(self, n: int, k: int) -> None:
        with self.lock:
            if not (self.raw / f"{n:02d}-{k}.wav").is_file():
                raise StudioError(f"line {n} has no take {k}")
            self._choose(n, k)

    def discard(self, n: int, k: int) -> None:
        """The take goes to voice/trash/ (kept, not deleted); the newest left becomes the chosen one."""
        with self.lock:
            self.trash.mkdir(parents=True, exist_ok=True)
            for path in (self.raw / f"{n:02d}-{k}.wav", self.raw / f"{n:02d}-{k}.json"):
                if path.is_file():
                    shutil.move(path, self.trash / path.name)
            if self._chosen().get(f"{n:02d}") == k:
                left = self.takes(n)
                self._choose(n, left[-1]["k"] if left else None)

    def restore(self, n: int, k: int) -> None:
        """Bring a discarded take back to voice/raw/ and choose it, without replacing any files."""
        with self.lock:
            wav = self.trash / f"{n:02d}-{k}.wav"
            if not wav.is_file():
                raise StudioError(f"line {n} has no take {k} in the trash")
            paths = (wav, wav.with_suffix(".json"))
            if any((self.raw / path.name).exists() for path in paths):
                raise StudioError(f"line {n} take {k} already exists in voice/raw")
            self.raw.mkdir(parents=True, exist_ok=True)
            for path in paths:
                if path.is_file():
                    shutil.move(path, self.raw / path.name)
            self._choose(n, k)

    def room(self, body: bytes) -> dict:
        with self.lock:
            wav = self.ws / "voice/room.wav"
            self._convert(body, wav)
            return measure(wav)


class Handler(BaseHTTPRequestHandler):
    studio: Studio

    def log_message(self, fmt: str, *args) -> None:  # quiet: the terminal shows takes, not requests
        pass

    def _send(self, code: int, body: bytes, ctype: str, extra: dict | None = None) -> None:
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def _json(self, data: object, code: int = 200) -> None:
        self._send(code, json.dumps(data, ensure_ascii=False).encode(), "application/json; charset=utf-8")

    def _file(self, path: Path) -> None:
        """A file with Range support: the video seeks, the takes play from any point."""
        size = path.stat().st_size
        ctype = mimetypes.guess_type(path.name)[0] or "application/octet-stream"
        rng = re.match(r"bytes=(\d*)-(\d*)$", self.headers.get("Range", ""))
        if rng and (rng.group(1) or rng.group(2)):
            if rng.group(1):
                start, end = int(rng.group(1)), int(rng.group(2) or size - 1)
            else:
                start, end = max(0, size - int(rng.group(2))), size - 1
            end = min(end, size - 1)
            if start > end:
                self._send(416, b"", ctype, {"Content-Range": f"bytes */{size}"})
                return
            with path.open("rb") as f:
                f.seek(start)
                body = f.read(end - start + 1)
            ranges = {"Content-Range": f"bytes {start}-{end}/{size}", "Accept-Ranges": "bytes"}
            self._send(206, body, ctype, ranges)
            return
        self._send(200, path.read_bytes(), ctype, {"Accept-Ranges": "bytes"})

    def _inside(self, base: Path, rel: str) -> Path | None:
        path = (base / unquote(rel)).resolve()
        return path if path.is_file() and path.is_relative_to(base.resolve()) else None

    def do_HEAD(self) -> None:  # noqa: N802
        self.do_GET()

    def do_GET(self) -> None:  # noqa: N802
        url = urlparse(self.path)
        try:
            if url.path == "/":
                self._file(APP / "index.html")
            elif url.path.startswith("/app/") and (path := self._inside(APP, url.path[5:])):
                self._file(path)
            elif url.path == "/api/session":
                self._json(self.studio.session())
            elif url.path.startswith("/media/") and (path := self._inside(self.studio.ws, url.path[7:])):
                self._file(path)
            else:
                self._send(404, b"not found", "text/plain")
        except (StudioError, OSError) as err:
            self._json({"error": str(err)}, 500)

    def do_POST(self) -> None:  # noqa: N802
        url = urlparse(self.path)
        q = {k: v[0] for k, v in parse_qs(url.query).items()}
        body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        try:
            if url.path == "/api/take":
                video_from = float(q["from"]) if q.get("from") not in (None, "") else None
                take = self.studio.add_take(int(q["line"]), body, video_from)
                n = int(q["line"])
                print(f"line {n:2d} take {take['k']}: {take['dur']:.1f} s, peak {take['peak']} dB")
                self._json(take)
            elif url.path == "/api/choose":
                self.studio.choose(int(q["line"]), int(q["take"]))
                self._json({"ok": True})
            elif url.path == "/api/discard":
                self.studio.discard(int(q["line"]), int(q["take"]))
                self._json({"ok": True})
            elif url.path == "/api/restore":
                self.studio.restore(int(q["line"]), int(q["take"]))
                self._json({"ok": True})
            elif url.path == "/api/room":
                self._json(self.studio.room(body))
            else:
                self._send(404, b"not found", "text/plain")
        except (StudioError, KeyError, ValueError, OSError) as err:
            self._json({"error": str(err)}, HTTPStatus.BAD_REQUEST)


def serve(ws: Path, lang: str = "ru", port: int = 47871, open_browser: bool = True) -> None:
    if shutil.which("ffmpeg") is None:
        raise StudioError("ffmpeg is not installed (brew install ffmpeg)")
    studio = Studio(ws, lang)
    studio.session()  # fails here, not in the browser, if the script is missing
    handler = type("StudioHandler", (Handler,), {"studio": studio})
    server = ThreadingHTTPServer(("127.0.0.1", port), handler)
    url = f"http://127.0.0.1:{port}/"
    print(f"voice studio for {ws}: {url}  (Ctrl+C stops it)")
    if not (ws / PREVIEW).is_file():
        print(f"no {PREVIEW}: the studio works without the picture")
    if open_browser:
        webbrowser.open(url)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()

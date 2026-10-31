import json
import re
import shutil
import subprocess
import threading
from http.client import HTTPConnection
from http.server import ThreadingHTTPServer
from pathlib import Path

import pytest

from gothic_shorts.studio import APP, Handler, Studio, StudioError, planned, script_lines

needs_ffmpeg = pytest.mark.skipif(shutil.which("ffmpeg") is None, reason="ffmpeg is not installed")


def _short(tmp_path, plan=True):
    (tmp_path / "script.ru.txt").write_text("Первая фраза.\n\nВторая фраза, длиннее.\n")
    if plan:
        lines = [[{"text": "Первая", "start": 0.3, "end": 0.8}, {"text": "фраза.", "start": 0.8, "end": 1.4}]]
        (tmp_path / "plan.storyboard.json").write_text(json.dumps({"lines": lines}))
    return tmp_path


def _tone(path, silence=1.0, tone=1.5, tail=0.5):
    """A take: silence, a tone at -12 dBFS, silence (a voice stands in as a tone)."""
    total = silence + tone + tail
    expr = f"if(between(t,{silence},{silence + tone}),0.25*sin(2*PI*220*t),0)"
    src = f"aevalsrc='{expr}':s=44100:d={total}"  # quoted: the commas are the expression's, not the graph's
    subprocess.run(["ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i", src, "-c:a", "pcm_f32le", str(path)],
                   check=True)  # fmt: skip
    return path.read_bytes()


def _take(studio, n, k):
    """Premeasured files: discard and restore do not need ffmpeg or real audio."""
    studio.raw.mkdir(parents=True, exist_ok=True)
    wav = studio.raw / f"{n:02d}-{k}.wav"
    wav.write_bytes(b"recorded take")
    wav.with_suffix(".json").write_text(json.dumps({"dur": 1.0, "peak": -12, "speech": [0, 1]}))
    studio.choose(n, k)
    return wav


def test_the_script_skips_blank_rows_and_the_plan_places_the_lines_it_has(tmp_path):
    ws = _short(tmp_path)
    assert script_lines(ws, "ru") == ["Первая фраза.", "Вторая фраза, длиннее."]
    assert planned(ws, 2) == [(0.3, 1.4), None]
    with pytest.raises(StudioError):
        script_lines(ws, "en")


def test_a_short_without_a_plan_has_no_places(tmp_path):
    ws = _short(tmp_path, plan=False)
    assert planned(ws, 2) == [None, None]


@needs_ffmpeg
def test_a_take_is_kept_as_48k_24bit_measured_and_chosen(tmp_path):
    studio = Studio(_short(tmp_path))
    take = studio.add_take(1, _tone(tmp_path / "in.wav"), video_from=0.0)
    wav = tmp_path / "voice/raw/01-1.wav"
    probe = subprocess.run(["ffprobe", "-v", "error", "-show_entries",
                            "stream=sample_rate,channels,codec_name", "-of", "csv=p=0", str(wav)],
                           capture_output=True, text=True).stdout  # fmt: skip
    assert probe.strip() == "pcm_s24le,48000,1"
    assert take["k"] == 1 and take["video_from"] == 0.0
    assert take["peak"] == pytest.approx(-12, abs=0.5)
    assert take["speech"][0] == pytest.approx(1.0, abs=0.05)
    assert take["speech"][1] == pytest.approx(2.5, abs=0.05)
    assert studio.session()["lines"][0]["chosen"] == 1


@needs_ffmpeg
def test_a_discarded_take_goes_to_the_trash_and_its_number_is_not_reused(tmp_path):
    studio = Studio(_short(tmp_path))
    body = _tone(tmp_path / "in.wav")
    studio.add_take(2, body, None)
    studio.add_take(2, body, None)
    studio.choose(2, 1)
    studio.discard(2, 2)
    assert (tmp_path / "voice/trash/02-2.wav").is_file()
    assert [t["k"] for t in studio.takes(2)] == [1]
    assert studio.add_take(2, body, None)["k"] == 3  # 2 is in the trash: never two files of one name
    studio.discard(2, 3)  # the chosen one goes: the newest left is chosen
    assert studio.session()["lines"][1]["chosen"] == 1
    with pytest.raises(StudioError):
        studio.choose(2, 2)


def test_a_discarded_take_is_restored_with_its_metadata_and_chosen(tmp_path):
    studio = Studio(_short(tmp_path))
    _take(studio, 1, 1)
    wav = _take(studio, 1, 2)
    audio, metadata = wav.read_bytes(), wav.with_suffix(".json").read_bytes()
    studio.discard(1, 2)
    assert studio.session()["lines"][0]["chosen"] == 1
    assert not wav.exists()
    assert not wav.with_suffix(".json").exists()

    studio.restore(1, 2)

    assert wav.read_bytes() == audio
    assert wav.with_suffix(".json").read_bytes() == metadata
    assert not (studio.trash / wav.name).exists()
    assert not (studio.trash / wav.with_suffix(".json").name).exists()
    assert studio.session()["lines"][0]["chosen"] == 2
    with pytest.raises(StudioError):
        studio.restore(1, 2)
    with pytest.raises(StudioError):
        studio.restore(2, 99)


def test_restore_accepts_a_take_without_metadata(tmp_path):
    studio = Studio(_short(tmp_path))
    studio.trash.mkdir(parents=True)
    wav = studio.trash / "02-1.wav"
    wav.write_bytes(b"recorded take without metadata")

    studio.restore(2, 1)

    assert (studio.raw / wav.name).read_bytes() == b"recorded take without metadata"
    assert not wav.exists()
    assert not (studio.raw / "02-1.json").exists()
    assert studio._chosen() == {"02": 1}


@pytest.mark.parametrize("collision", [".wav", ".json"])
def test_restore_does_not_replace_existing_files(tmp_path, collision):
    studio = Studio(_short(tmp_path))
    wav = _take(studio, 1, 1)
    studio.discard(1, 1)
    dest = wav.with_suffix(collision)
    dest.write_bytes(b"existing file")

    with pytest.raises(StudioError):
        studio.restore(1, 1)

    assert dest.read_bytes() == b"existing file"
    assert (studio.trash / wav.name).is_file()
    assert (studio.trash / wav.with_suffix(".json").name).is_file()
    assert studio._chosen() == {}


def test_discard_and_restore_over_http(tmp_path):
    studio = Studio(_short(tmp_path))
    _take(studio, 2, 1)
    wav = _take(studio, 2, 2)
    handler = type("StudioHandler", (Handler,), {"studio": studio})
    server = ThreadingHTTPServer(("127.0.0.1", 0), handler)
    thread = threading.Thread(target=server.serve_forever, kwargs={"poll_interval": 0.01}, daemon=True)
    thread.start()

    def request(method, path):
        connection = HTTPConnection(*server.server_address, timeout=5)
        try:
            connection.request(method, path)
            response = connection.getresponse()
            return response.status, json.loads(response.read())
        finally:
            connection.close()

    try:
        assert request("POST", "/api/discard?line=2&take=2") == (200, {"ok": True})
        assert not wav.exists()
        assert not wav.with_suffix(".json").exists()
        assert (studio.trash / wav.name).is_file()
        assert (studio.trash / wav.with_suffix(".json").name).is_file()
        status, session = request("GET", "/api/session")
        assert status == 200
        assert session["lines"][1]["chosen"] == 1

        assert request("POST", "/api/restore?line=2&take=2") == (200, {"ok": True})
        assert wav.is_file()
        assert wav.with_suffix(".json").is_file()
        assert not (studio.trash / wav.name).exists()
        assert not (studio.trash / wav.with_suffix(".json").name).exists()
        status, session = request("GET", "/api/session")
        assert status == 200
        assert session["lines"][1]["chosen"] == 2

        status, error = request("POST", "/api/restore?line=2&take=2")
        assert status == 400
        assert error["error"]
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=5)
    assert not thread.is_alive()


def test_the_studio_does_not_use_native_browser_dialogs():
    assert re.search(r"\b(?:confirm|alert|prompt)\s*\(", (APP / "app.js").read_text()) is None


@pytest.mark.skipif(shutil.which("node") is None, reason="node is not installed")
def test_the_studio_javascript_has_valid_syntax():
    done = subprocess.run(["node", "--check", str(APP / "app.js")], capture_output=True, text=True)
    assert done.returncode == 0, done.stderr


@pytest.mark.skipif(shutil.which("node") is None, reason="node is not installed")
def test_the_studio_handles_take_actions_and_late_responses():
    done = subprocess.run(
        ["node", str(Path(__file__).with_name("studio_race.js")), str(APP / "app.js")],
        capture_output=True,
        text=True,
        timeout=10,
    )
    assert done.returncode == 0, done.stdout + done.stderr

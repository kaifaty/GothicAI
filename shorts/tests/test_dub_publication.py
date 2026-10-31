"""Publication ownership, interruption and source replacement use synthetic media only."""

import json
import re
import shutil
import subprocess
from concurrent.futures import ThreadPoolExecutor
from threading import Event
from types import SimpleNamespace

import pytest
from test_dub import (
    CONFIG,
    _assert_publication,
    _assert_unchanged,
    _packet_hashes,
    _stale_outputs,
    _tts_sound,
)
from test_explain import _plan
from test_explain import synthetic_stems as synthetic_stems

from gothic_shorts import dub, explain, levels


@pytest.fixture(scope="module")
def russian_versions(tmp_path_factory, synthetic_stems):
    directory = tmp_path_factory.mktemp("dub-source-versions")
    original, replacement = directory / "old.mp4", directory / "new.mp4"
    subprocess.run(
        ["ffmpeg", "-v", "error", "-i", str(synthetic_stems / "picture.mp4"),
         "-i", str(synthetic_stems / "game.wav"), "-map", "0:v:0", "-map", "1:a:0",
         "-c:v", "copy", "-c:a", "aac", "-t", "14.2", str(original)],
        check=True, capture_output=True,
    )
    subprocess.run(
        ["ffmpeg", "-v", "error", "-f", "lavfi", "-i", "color=c=red:s=16x16:r=30:d=14.2",
         "-f", "lavfi", "-i", "sine=frequency=880:sample_rate=48000:duration=14.2",
         "-c:v", "mpeg4", "-c:a", "aac", "-ac", "2", "-t", "14.2", str(replacement)],
        check=True, capture_output=True,
    )
    return original, replacement


@pytest.fixture
def explainer_publication(tmp_path, monkeypatch, synthetic_stems, russian_versions):
    """Real dub orchestration/mux with prepared stems and a prechecked AAC master."""
    ws = tmp_path / "explainer"
    directory = ws / "render"
    directory.mkdir(parents=True)
    plan = _plan(lang="ru")
    (ws / "plan.json").write_text(json.dumps(plan))
    (ws / "words.ru.json").write_text(json.dumps({"lines": [{"words": plan["lines"][0]}]}))
    (ws / "script.en.txt").write_text("First and last\n")
    russian = directory / "short-ru.mp4"
    shutil.copyfile(russian_versions[0], russian)
    report = {"ok": True, "rows": [{"id": "voice_mastered", "measured_lufs": -14.0}]}
    russian.with_suffix(".levels.json").write_text(json.dumps(report))

    def render(workspace, rendered_plan, target, *, stems):
        assert workspace == ws and rendered_plan == plan
        assert stems == ("game", "music")
        for stem in stems:
            shutil.copyfile(synthetic_stems / f"{stem}.wav", target / f"{stem}.wav")

    def publish(joined, music, voice, speech, total, length, *, out, **kwargs):
        # Mastering is covered in test_dub; these tests keep real AAC and stream copies.
        subprocess.run(
            ["ffmpeg", "-v", "error", "-y", "-i", str(joined), "-i", str(voice[0][0]),
             "-map", "0:v:0", "-map", "1:a:0", "-c:v", "copy", "-c:a", "aac",
             "-t", str(length), str(out)], check=True, capture_output=True,
        )
        out.with_suffix(".levels.json").write_text(json.dumps(report))
        return report.copy()

    monkeypatch.setattr(explain, "render", render)
    monkeypatch.setattr(levels, "publish", publish)
    monkeypatch.setattr(dub, "clone", lambda *args: [synthetic_stems / "voice.wav"])
    monkeypatch.setattr(
        dub, "_english_stem", lambda *args, **kwargs: (synthetic_stems / "voice.wav", [(4, 8)]),
    )
    return SimpleNamespace(ws=ws, directory=directory, russian=russian,
                           replacement=russian_versions[1], stems=synthetic_stems)


def _outputs(directory):
    return [directory / f"dub.en.{suffix}" for suffix in ("mp4", "wav", "levels.json")]


def _assert_dual_mono(path):
    measured = subprocess.run(
        ["ffmpeg", "-nostdin", "-hide_banner", "-i", str(path), "-map", "0:a:0",
         "-af", "pan=mono|c0=c0-c1,volumedetect", "-f", "null", "-"],
        check=True, capture_output=True, text=True,
    ).stderr
    difference = float(re.search(r"max_volume: ([\d.+-]+) dB", measured)[1])
    assert difference <= -90, f"{path.name}: max L-R = {difference} dB"


def test_explainer_english_aac_and_wav_are_dual_mono(
    tmp_path, monkeypatch, synthetic_stems, russian_versions,
):
    ws = tmp_path / "explainer"
    directory = ws / "render"
    directory.mkdir(parents=True)
    plan = _plan(lang="ru")
    (ws / "plan.json").write_text(json.dumps(plan))
    (ws / "words.ru.json").write_text(json.dumps({"lines": [{"words": plan["lines"][0]}]}))
    (ws / "script.en.txt").write_text("First and last\n")
    russian = directory / "short-ru.mp4"
    shutil.copyfile(russian_versions[0], russian)
    russian.with_suffix(".levels.json").write_text(json.dumps({
        "ok": True, "rows": [{"id": "voice_mastered", "measured_lufs": -14}],
    }))

    def render(workspace, rendered_plan, target, *, stems):
        assert workspace == ws and rendered_plan == plan
        assert stems == ("game", "music")
        for stem in stems:
            shutil.copyfile(synthetic_stems / f"{stem}.wav", target / f"{stem}.wav")

    monkeypatch.setattr(explain, "render", render)
    monkeypatch.setattr(dub, "clone", lambda *args: [
        _tts_sound(ws / "voice/en/raw/01.wav", seconds=2),
    ])
    done = dub.dub_explainer(CONFIG, ws)

    report = _assert_publication(done, russian)
    assert next(row for row in report["rows"] if row["id"] == "dual_mono")["ok"] is True
    _assert_dual_mono(done.preview)
    _assert_dual_mono(done.wav)


@pytest.mark.parametrize("invalid", [
    "report-missing", "words-missing", "plan-missing", "script-missing",
    "script-count", "language", "length",
])
def test_explainer_input_refusals_preserve_the_previous_publication(
    explainer_publication, monkeypatch, invalid,
):
    case = explainer_publication
    lang = "de" if invalid == "language" else "en"
    before = _stale_outputs(case.directory)
    if lang != "en":
        before.update(_stale_outputs(case.directory, lang))
    if invalid == "report-missing":
        case.russian.with_suffix(".levels.json").unlink()
    elif invalid == "words-missing":
        (case.ws / "words.ru.json").unlink()
    elif invalid == "plan-missing":
        (case.ws / "plan.json").unlink()
    elif invalid == "script-missing":
        (case.ws / "script.en.txt").unlink()
    elif invalid == "script-count":
        (case.ws / "script.en.txt").write_text("First\nLast\n")
    elif invalid == "length":
        plan = json.loads((case.ws / "plan.json").read_text())
        plan["brand"] = False
        (case.ws / "plan.json").write_text(json.dumps(plan))

    def unexpected_work(*args, **kwargs):
        raise AssertionError("invalid inputs must be rejected before rendering or requesting voices")

    monkeypatch.setattr(explain, "render", unexpected_work)
    monkeypatch.setattr(dub, "clone", unexpected_work)
    with pytest.raises(dub.DubError):
        dub.dub_explainer(CONFIG, case.ws, lang)
    _assert_unchanged(before)


def test_explainer_clears_previous_outputs_only_when_assembly_starts(explainer_publication, monkeypatch):
    case = explainer_publication
    before = _stale_outputs(case.directory)

    def render(*args, **kwargs):
        assert all(not path.exists() for path in before)
        raise dub.DubError("render failed after validation")

    monkeypatch.setattr(explain, "render", render)
    with pytest.raises(dub.DubError, match="render failed after validation"):
        dub.dub_explainer(CONFIG, case.ws)
    assert all(not path.exists() for path in before)


def test_parallel_dub_rejects_before_touching_the_owners_finished_outputs(
    explainer_publication, monkeypatch,
):
    case = explainer_publication
    finished, release = Event(), Event()
    real_ffmpeg = dub._ffmpeg
    cloned = []

    def clone(*args):
        cloned.append(True)
        return [case.stems / "voice.wav"]

    def ffmpeg(args, what):
        result = real_ffmpeg(args, what)
        if what == "the checked AAC as WAV":
            finished.set()
            assert release.wait(timeout=10), "test did not release the output owner"
        return result

    monkeypatch.setattr(dub, "clone", clone)
    monkeypatch.setattr(dub, "_ffmpeg", ffmpeg)
    with ThreadPoolExecutor(max_workers=1) as pool:
        owner = pool.submit(dub.dub_explainer, CONFIG, case.ws)
        try:
            assert finished.wait(timeout=20), "owner did not finish the synthetic AAC/WAV"
            before = [path.read_bytes() for path in _outputs(case.directory)]
            with pytest.raises(dub.DubError, match="уже собирается"):
                dub.dub_explainer(CONFIG, case.ws)
            assert [path.read_bytes() for path in _outputs(case.directory)] == before
            assert len(cloned) == 1
        finally:
            release.set()
        done = owner.result(timeout=10)
    assert done.preview.is_file() and done.wav.is_file()
    assert json.loads(done.preview.with_suffix(".levels.json").read_text())["ok"] is True


@pytest.mark.parametrize("stage", ["the bilingual preview", "the checked AAC as WAV"])
def test_keyboard_interrupt_invalidates_a_master_and_removes_final_media(
    explainer_publication, monkeypatch, stage,
):
    case = explainer_publication
    real_ffmpeg = dub._ffmpeg

    def ffmpeg(args, what):
        result = real_ffmpeg(args, what)
        if what == stage:
            raise KeyboardInterrupt
        return result

    monkeypatch.setattr(dub, "_ffmpeg", ffmpeg)
    with pytest.raises(KeyboardInterrupt):
        dub.dub_explainer(CONFIG, case.ws)
    video, wav, report_path = _outputs(case.directory)
    assert not video.exists() and not wav.exists()
    report = json.loads(report_path.read_text())
    assert report["ok"] is False and report["error"] == "KeyboardInterrupt"
    # The ownership lock is released even for BaseException; retry can publish.
    monkeypatch.setattr(dub, "_ffmpeg", real_ffmpeg)
    done = dub.dub_explainer(CONFIG, case.ws)
    assert done.preview.is_file() and done.wav.is_file()


def test_both_dub_entrypoints_share_interruption_cleanup(tmp_path):
    # Both public entrypoints use this context, including the take's longer voice phase.
    out = tmp_path / "dub.en.mp4"
    with pytest.raises(KeyboardInterrupt), dub._publication(out) as session:
        session.start()
        out.write_bytes(b"mastered mp4")
        out.with_suffix(".wav").write_bytes(b"partial PCM")
        out.with_suffix(".levels.json").write_text('{"ok": true}')
        raise KeyboardInterrupt
    assert not out.exists() and not out.with_suffix(".wav").exists()
    assert json.loads(out.with_suffix(".levels.json").read_text())["ok"] is False


def test_replaced_russian_source_keeps_picture_and_russian_audio_from_one_version(
    explainer_publication, monkeypatch,
):
    case = explainer_publication
    video_hashes = _packet_hashes(case.russian, "v:0")
    audio_hashes = _packet_hashes(case.russian, "a:0")
    assert video_hashes != _packet_hashes(case.replacement, "v:0")
    assert audio_hashes != _packet_hashes(case.replacement, "a:0")
    assert dub._probe(case.russian) == pytest.approx(dub._probe(case.replacement), abs=0.001)

    def clone(*args):
        # Voices come after _join_picture: simulate cut/explain atomically replacing its source.
        replacement = case.russian.with_name("replacement.mp4")
        shutil.copyfile(case.replacement, replacement)
        replacement.replace(case.russian)
        return [case.stems / "voice.wav"]

    monkeypatch.setattr(dub, "clone", clone)
    done = dub.dub_explainer(CONFIG, case.ws)
    assert _packet_hashes(done.preview, "v:0") == video_hashes
    assert _packet_hashes(done.preview, "a:1") == audio_hashes
    assert _packet_hashes(done.preview, "a:1") != _packet_hashes(case.russian, "a:0")
    assert json.loads(done.preview.with_suffix(".levels.json").read_text())["ok"] is True

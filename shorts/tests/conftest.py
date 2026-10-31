"""Shared fixtures."""

import shutil
import subprocess

import pytest

from gothic_shorts import delivery

_FFMPEG_WAV_TESTS = {
    ("test_compare.py", "test_comparison_dub_publishes_the_russian_effects_ending_and_picture"),
    ("test_dub.py", "test_take_dub_uses_the_russian_effects_bed_and_picture_with_english_speaker_gains"),
    ("test_dub.py", "test_levels_rejection_removes_both_dub_outputs_and_keeps_the_failed_report"),
    ("test_dub.py", "test_explainer_dub_renders_only_game_and_music_from_its_russian_plan"),
    ("test_dub_long_lines.py", "test_cli_reports_the_long_final_line_after_real_levels_publication"),
    ("test_dub_publication.py", "test_explainer_english_aac_and_wav_are_dual_mono"),
}


def pytest_collection_modifyitems(items):
    affected = [item for item in items if (item.path.name, item.originalname) in _FFMPEG_WAV_TESTS]
    ffmpeg = shutil.which("ffmpeg")
    if not affected or ffmpeg is None:
        return
    probe = subprocess.run([ffmpeg, "-version"], check=True, capture_output=True, text=True)
    version = probe.stdout.split()[2]
    if int(version.split(".", 1)[0]) >= 7:
        return
    marker = pytest.mark.skip(
        reason="FFmpeg 6.1 truncates WAV during mixing; shorts render with FFmpeg 8–9",
    )
    for item in affected:
        item.add_marker(marker)


@pytest.fixture(autouse=True)
def delivery_passes(request, monkeypatch):
    """Publication runs the delivery check (gothic_shorts.delivery), which has its own tests in test_delivery.py.
    Elsewhere the synthetic 16x16 videos would fail it, so it passes, recording what it saw."""
    seen = []
    if request.module.__name__.endswith("test_delivery"):
        return seen
    monkeypatch.setattr(delivery, "check_zones", lambda ws, plan, report=None: seen.append(("zones", plan["lang"]))
                        or report or delivery.Report())
    monkeypatch.setattr(delivery, "check_file", lambda video, report=None: seen.append(("file", video.name))
                        or report or delivery.Report())
    return seen

"""Delivery check: YouTube's file specification and the Shorts interface's zones, on synthetic ffmpeg files."""

import shutil
import subprocess

import pytest

from gothic_shorts import delivery

pytestmark = pytest.mark.skipif(shutil.which("ffmpeg") is None, reason="ffmpeg is required")


def _ffmpeg(*args):
    subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", *map(str, args)], check=True, capture_output=True)


def _short(path, *video_args):
    _ffmpeg("-f", "lavfi", "-i", "testsrc2=s=1080x1920:r=30:d=0.4", "-f", "lavfi",
            "-i", "sine=frequency=440:sample_rate=48000:duration=0.4", "-ac", "2",
            "-c:v", "libx264", "-preset", "veryfast", "-profile:v", "high", *video_args, "-c:a", "aac", path)
    return path



def test_a_bt709_limited_file_passes_the_colour_checks(tmp_path):
    video = _short(tmp_path / "good.mp4", "-vf", "scale=out_color_matrix=bt709:out_range=tv,format=yuv420p,"
                   "setparams=colorspace=bt709:color_primaries=bt709:color_trc=bt709:range=tv", "-movflags", "+faststart")
    report = delivery.check_file(video)
    lines = "\n".join(report.lines)
    assert "ok   pixels yuv420p" in lines
    assert "ok   range tv" in lines
    assert "ok   matrix/primaries/transfer bt709/bt709/bt709" in lines
    assert "ok   coded luma outside" in lines
    assert "ok   size 1080x1920" in lines
    assert "ok   audio 48000 Hz, 2 channels" in lines
    assert "ok   index before the data" in lines
    # a 0.4 s sine is not mastered: loudness is the one failure
    assert len(report.failures) == 1 and report.failures[0].startswith("loudness")


def test_remotion_4_default_full_range_bt601_fails(tmp_path):
    video = _short(tmp_path / "bad.mp4", "-vf", "scale=out_range=pc,format=yuvj420p",
                   "-colorspace", "bt470bg", "-color_range", "pc")
    report = delivery.check_file(video)
    failures = "\n".join(report.failures)
    assert "pixels yuvj420p" in failures
    assert "range pc" in failures
    assert "matrix/primaries/transfer bt470bg/unknown/unknown" in failures
    assert "index before the data" in "\n".join(report.notes)


def test_atoms_read_the_top_level_boxes_in_order(tmp_path):
    video = _short(tmp_path / "fast.mp4", "-movflags", "+faststart")
    names = delivery.atoms(video)
    assert names[0] == "ftyp" and names.index("moov") < names.index("mdat")


def test_theme_safe_reads_the_composition_margins():
    assert delivery.theme_safe("SAFE") == {"top": 150, "bottom": 380, "right": 150, "left": 60}
    assert set(delivery.theme_safe("WIDE_SAFE")) == {"top", "bottom", "left", "right"}


def test_depth_counts_from_the_safe_edge_into_the_zone():
    rects = delivery.zones(1080, 1920, {"top": 150, "bottom": 380, "right": 150, "left": 60})
    assert delivery.depth_into("right", rects["right"], (940, 500, 952, 540)) == 22
    assert delivery.depth_into("bottom", rects["bottom"], (100, 1540, 600, 1576)) == 36
    assert delivery.depth_into("top", rects["top"], (100, 120, 600, 150)) == 30
    assert set(rects) == {"top", "bottom", "right"}  # nothing covers the left margin


def test_check_zones_reports_a_held_thing_and_lists_one_passing_through(tmp_path):
    # a quarter-size mask: a label held 1 s in the right zone (full frame x 940-980), and one 3 frames in the bottom
    mask = tmp_path / "mask.mp4"
    _ffmpeg("-f", "lavfi", "-i", "color=c=black:s=270x480:r=30:d=2",
            "-vf", "drawbox=x=235:y=100:w=10:h=8:color=white:t=fill:enable='between(t,0.5,1.49)',"
                   "drawbox=x=40:y=400:w=60:h=10:color=white:t=fill:enable='between(t,1.6,1.69)'",
            "-c:v", "libx264", "-crf", "1", "-pix_fmt", "yuv420p", mask)
    plan = {"lang": "ru", "beats": [{"id": "one", "dur": 1.0}, {"id": "two", "dur": 1.0}]}
    report = delivery.check_zones(tmp_path, plan, mask=mask)
    assert len(report.failures) == 1
    held = report.failures[0]
    assert "right   0.50-  1.50 s" in held and "one" in held and "50 px in" in held  # its far edge, 980, past 930
    passing = [line for line in report.lines if line.startswith("pass")]
    assert len(passing) == 1 and "bottom" in passing[0] and "two" in passing[0]


def test_a_faint_grid_and_a_dark_card_ground_are_not_drawing(tmp_path):
    mask = tmp_path / "mask.mp4"
    _ffmpeg("-f", "lavfi", "-i", "color=c=0x14110D:s=270x480:r=30:d=0.5",
            "-vf", "drawbox=x=0:y=440:w=270:h=2:color=0x323232:t=fill",
            "-c:v", "libx264", "-crf", "1", "-pix_fmt", "yuv420p", mask)
    report = delivery.check_zones(tmp_path, {"lang": "ru", "beats": [{"id": "card", "dur": 0.5}]}, mask=mask)
    assert report.ok and not [line for line in report.lines if line.startswith("pass")]


def test_a_wide_video_has_no_zones_to_check(tmp_path):
    report = delivery.check_zones(tmp_path, {"lang": "ru", "format": "wide", "beats": []})
    assert report.ok and report.lines[0].startswith("skip safe zones")

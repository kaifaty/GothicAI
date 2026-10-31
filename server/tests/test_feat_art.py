import asyncio
import stat
from pathlib import Path

import numpy as np
import pytest
from PIL import Image, ImageDraw

from gothic_ai import feat_art
from gothic_ai.feat_art import ACCENT, INK, PARCH, Artist, normalise, process, prompt

PAPER = (214, 196, 160)  # a generator's parchment, a little off the house tone


def woodcut(path: Path, box=(300, 200, 700, 520), accent=True, size=1254) -> Path:
    """A raw picture like the generator's: an ink block off-centre on parchment with a red detail."""
    img = Image.new("RGB", (size, size), PAPER)
    d = ImageDraw.Draw(img)
    d.rectangle(box, fill=(30, 26, 22))
    d.rectangle((box[0] + 40, box[1] + 40, box[2] - 40, box[1] + 80), fill=PAPER)  # a wide cut
    if accent:
        d.ellipse((box[0] + 60, box[3] - 120, box[0] + 120, box[3] - 60), fill=(160, 40, 30))
    img.save(path)
    return path


def test_prompt_puts_the_emblem_between_style_and_common_blocks():
    text = prompt("a broken key on a stone")
    style, subject, common, note = text.split("\n\n")
    assert style == feat_art.STYLE["style"] and common == feat_art.STYLE["common"]
    assert subject == "Subject: a broken key on a stone" and note == feat_art.STYLE["anchor_note"]


def test_normalise_centres_the_subject_and_keeps_the_palette(tmp_path):
    raw = woodcut(tmp_path / "raw.png")
    pic, stats = normalise(Image.open(raw))
    assert pic.size == (feat_art.WORK, feat_art.WORK)
    rgb = np.asarray(pic, float)
    dark = (rgb @ [0.299, 0.587, 0.114]) < 120
    rows, cols = np.flatnonzero(dark.any(1)), np.flatnonzero(dark.any(0))
    assert abs((rows[0] + rows[-1]) / 2 - feat_art.WORK / 2) < 0.02 * feat_art.WORK
    assert abs((cols[0] + cols[-1]) / 2 - feat_art.WORK / 2) < 0.02 * feat_art.WORK
    longer = max(rows[-1] - rows[0], cols[-1] - cols[0]) / feat_art.WORK
    assert longer == pytest.approx(feat_art.FILL, abs=0.02)
    corner = rgb[:40, :40].reshape(-1, 3).mean(0)
    assert np.abs(corner - PARCH).max() < 12  # the paper is the house parchment, grain aside
    centre = rgb[700:760, 300:340].reshape(-1, 3).mean(0)  # inside the ink block, below the cut
    assert np.abs(centre - INK).max() < 12
    red = rgb[(rgb[..., 0] > rgb[..., 1] + 40)].mean(0)
    share = (red - INK) / (ACCENT - INK)  # the accent keeps its carved darks: a point on the ink-accent line
    assert 0.3 < share.mean() <= 1.05 and np.ptp(share) < 0.2
    assert 0.06 < stats["ink"] < 0.6 and 0 < stats["accent"] < 0.08


def test_process_writes_engraving_and_icon(tmp_path):
    stats = process(woodcut(tmp_path / "raw.png"), tmp_path / "out")
    assert stats["ok"]
    assert Image.open(tmp_path / "out/engraving.jpg").size == (512, 512)
    icon = np.asarray(Image.open(tmp_path / "out/icon.png").convert("RGB"), float)
    assert icon.shape == (128, 128, 3)
    assert np.abs(icon[1, 64] - INK).max() < 20  # the outer ink rule of the frame


def test_process_flags_a_picture_out_of_bounds(tmp_path):
    raw = tmp_path / "blot.png"
    Image.new("RGB", (600, 600), (25, 20, 18)).save(raw)
    assert not process(raw, tmp_path / "out")["ok"]  # all ink
    blank = tmp_path / "blank.png"
    Image.new("RGB", (600, 600), PAPER).save(blank)
    with pytest.raises(ValueError, match="empty picture"):
        process(blank, tmp_path / "out")


def fake_codex(tmp_path: Path, body: str) -> str:
    """A stand-in for `codex exec`: a script whose behaviour depends on how often it was called."""
    script = tmp_path / "codex"
    script.write_text(
        "#!/bin/sh\n"
        f"calls={tmp_path}/calls\n"
        'echo x >> "$calls"\n'
        'n=$(wc -l < "$calls" | tr -d " ")\n'
        f"cat > {tmp_path}/prompt-$n.txt\n"
        f'echo "$@" > {tmp_path}/args-$n.txt\n'
        f"{body}\n"
    )
    script.chmod(script.stat().st_mode | stat.S_IEXEC)
    return str(script)


def calls(tmp_path: Path) -> int:
    path = tmp_path / "calls"
    return len(path.read_text().splitlines()) if path.exists() else 0


async def test_draw_stores_the_picture_and_its_prompt(tmp_path):
    raw = woodcut(tmp_path / "gen.png")
    artist = Artist(tmp_path / "feats", codex=fake_codex(tmp_path, f'echo "Saved to {raw}"'))
    picture = await artist.draw("abc123", "a coiled snake under a cart")
    assert picture is not None and picture.dir == tmp_path / "feats/abc123"
    assert {p.name for p in picture.dir.iterdir()} == {"raw.png", "engraving.jpg", "icon.png", "prompt.txt"}
    assert picture.style == feat_art.STYLE_VERSION and len(picture.hash) == 16
    assert (picture.dir / "prompt.txt").read_text() == prompt("a coiled snake under a cart")
    sent = (tmp_path / "prompt-1.txt").read_text()
    assert sent.startswith(feat_art.WRAP) and sent.endswith(prompt("a coiled snake under a cart"))
    args = (tmp_path / "args-1.txt").read_text().split()
    assert args[:4] == ["exec", "--skip-git-repo-check", "-s", "read-only"]
    assert [args[i + 1] for i, a in enumerate(args) if a == "-i"] == [
        str(feat_art.KIT / a) for a in feat_art.STYLE["anchors"]
    ]
    assert calls(tmp_path) == 1


async def test_draw_retries_once_then_gives_up(tmp_path):
    raw = woodcut(tmp_path / "gen.png")
    body = f'[ "$n" -ge 2 ] && echo "{raw}" || exit 1'
    artist = Artist(tmp_path / "feats", codex=fake_codex(tmp_path, body))
    assert await artist.draw("retry", "a cold campfire") is not None
    assert calls(tmp_path) == 2

    (tmp_path / "calls").unlink()
    failing = Artist(tmp_path / "feats", codex=fake_codex(tmp_path, "exit 1"))
    assert await failing.draw("fail", "a cold campfire") is None
    assert calls(tmp_path) == 2
    assert not (tmp_path / "feats/fail").exists()


async def test_draw_rejects_a_picture_out_of_bounds(tmp_path):
    blot = tmp_path / "blot.png"
    Image.new("RGB", (600, 600), (25, 20, 18)).save(blot)
    artist = Artist(tmp_path / "feats", codex=fake_codex(tmp_path, f'echo "{blot}"'))
    assert await artist.draw("blot", "a black bag") is None
    assert calls(tmp_path) == 2


async def test_draw_times_out_and_kills_the_call(tmp_path):
    artist = Artist(tmp_path / "feats", codex=fake_codex(tmp_path, "exec sleep 30"), timeout_s=0.5)
    start = asyncio.get_running_loop().time()
    assert await artist.draw("slow", "a rope") is None
    assert asyncio.get_running_loop().time() - start < 5
    assert calls(tmp_path) == 2


async def test_draw_with_a_missing_codex_binary_gives_up(tmp_path):
    assert await Artist(tmp_path / "feats", codex=str(tmp_path / "no-codex")).draw("gone", "a rope") is None


async def test_draw_without_codex_keeps_the_category_emblem(tmp_path, monkeypatch):
    monkeypatch.delenv("GOTHIC_AI_CODEX", raising=False)
    monkeypatch.setattr(feat_art.shutil, "which", lambda name: None)
    assert await Artist(tmp_path / "feats").draw("none", "a rope") is None


async def test_draws_go_one_at_a_time(tmp_path):
    raw = woodcut(tmp_path / "gen.png")
    body = (
        f'[ -e {tmp_path}/busy ] && echo overlap >> {tmp_path}/overlaps\n'
        f'touch {tmp_path}/busy; sleep 0.3; rm {tmp_path}/busy; echo "{raw}"'
    )
    artist = Artist(tmp_path / "feats", codex=fake_codex(tmp_path, body))
    results = await asyncio.gather(*(artist.draw(f"f{i}", "a rope") for i in range(3)))
    assert all(results) and not (tmp_path / "overlaps").exists()

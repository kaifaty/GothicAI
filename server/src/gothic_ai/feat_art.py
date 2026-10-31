"""Feat pictures in the approved style (docs/achievements.md#изображение).

The chronicler's `emblem` goes into a fixed prompt (the style block from assets/feats/style.json plus two
style anchors); headless `codex exec` makes one image_gen call in an empty repository and replies with the
path. Code then removes the generator's spread: the subject is cropped to its content and re-centred with
one margin, colours are pulled to the fixed palette (ink, parchment, one accent), the parchment gets the
same grain everywhere, and the icon gets the common frame.

    uv run python -m gothic_ai.feat_art gen OUT.png "EMBLEM"     # raw picture
    uv run python -m gothic_ai.feat_art process RAW.png OUT_DIR  # engraving.jpg (512) and icon.png (128)
    uv run python -m gothic_ai.feat_art draw ID "EMBLEM"         # both, into the feats folder
"""

from __future__ import annotations

import asyncio
import hashlib
import json
import logging
import os
import re
import shutil
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

log = logging.getLogger(__name__)

KIT = Path(__file__).resolve().parent / "assets/feats"
STYLE = json.loads((KIT / "style.json").read_text())
STYLE_VERSION: int = STYLE["version"]
INK, PARCH, ACCENT = (np.array(STYLE["palette"][k], float) for k in ("ink", "parchment", "accent"))
DATA = Path(os.environ.get("GOTHIC_AI_DATA", Path.home() / ".local" / "share" / "gothic-ai"))
DEFAULT_ROOT = DATA / "feats"

WORK = 1024  # working size of the processed picture
FILL = 0.76  # the subject's longer side as a share of the square
INK_LO = 70.0  # luminance ramp: below is solid ink
PAPER_KNEE = 0.8  # share of the picture's own paper tone above which it is bare parchment (no stains)
FRAME = (0.034, 0.020, 0.010)  # icon frame at 512: outer ink rule, parchment gap, inner rule (share of side)
INK_RANGE = (0.06, 0.60)  # share of the area in ink: less is an empty picture, more is a black blot
ACCENT_MAX = 0.08  # the red accent stays on one detail

WRAP = (
    "Use your image generation tool exactly once with the prompt below and the attached images. Do not "
    "view, judge or edit the result. Reply with only the absolute path of the generated file.\n\nPrompt:\n"
)


def prompt(emblem: str) -> str:
    return f"{STYLE['style']}\n\nSubject: {emblem}\n\n{STYLE['common']}\n\n{STYLE['anchor_note']}"


def _accent_weight(rgb: np.ndarray) -> np.ndarray:
    """How strongly each pixel belongs to the red accent: a saturated red hue, not the warm parchment."""
    r, g, b = rgb[..., 0], rgb[..., 1], rgb[..., 2]
    mx, mn = rgb.max(2), rgb.min(2)
    sat = (mx - mn) / np.maximum(mx, 1)
    red = (r >= g) & (r >= b) & ((g - b) < 0.45 * (r - b + 1e-6))  # hue below ~25°, parchment sits at ~38°
    return np.clip((sat - 0.42) / 0.18, 0, 1) * red * np.clip((mx - 45) / 40, 0, 1)


def _grain(size: int) -> np.ndarray:
    """The same paper grain for every picture: soft blotches and a fine tooth, seeded."""
    rng = np.random.default_rng(1)
    blot = Image.fromarray((rng.random((24, 24)) * 255).astype(np.uint8)).resize((size, size), Image.BICUBIC)
    blot = np.asarray(blot.filter(ImageFilter.GaussianBlur(size / 40)), float) / 255 - 0.5
    tooth = rng.normal(0, 1, (size, size))
    return 1 + 0.05 * blot + 0.018 * tooth


def normalise(src: Image.Image) -> tuple[Image.Image, dict]:
    """The picture in the house palette at WORK size, subject centred at FILL; stats of the source."""
    rgb = np.asarray(src.convert("RGB"), float)
    lum = rgb @ [0.299, 0.587, 0.114]
    acc = _accent_weight(rgb)
    ink_hi = max(PAPER_KNEE * np.percentile(lum, 90), INK_LO + 1)  # the generator's parchment varies in tone
    t = np.clip((lum - INK_LO) / (ink_hi - INK_LO), 0, 1)[..., None]
    base = INK + t * (PARCH - INK)
    shade = np.clip(lum / 120, 0, 1)[..., None]  # the accent keeps its own carved darks
    out = base + acc[..., None] * (INK + shade * (ACCENT - INK) - base)

    mask = (lum < 120) | (acc > 0.5)
    h, w = mask.shape
    rows = np.flatnonzero(mask.sum(1) > w * 0.004)
    cols = np.flatnonzero(mask.sum(0) > h * 0.004)
    if not len(rows) or not len(cols):
        raise ValueError("empty picture")
    y0, y1, x0, x1 = rows[0], rows[-1], cols[0], cols[-1]
    side = max(int(max(y1 - y0, x1 - x0) / FILL), 1)
    cy, cx = (y0 + y1) // 2, (x0 + x1) // 2
    canvas = Image.new("RGB", (side, side), tuple(PARCH.astype(int)))
    canvas.paste(Image.fromarray(out.clip(0, 255).astype(np.uint8)), (side // 2 - cx, side // 2 - cy))
    pic = np.asarray(canvas.resize((WORK, WORK), Image.LANCZOS), float) * _grain(WORK)[..., None]

    stats = {
        "ink": round(float(mask.mean()), 3),
        "accent": round(float((acc > 0.5).mean()), 4),
        "fill": round(float(max(y1 - y0, x1 - x0) / max(h, w)), 3),
    }
    return Image.fromarray(pic.clip(0, 255).astype(np.uint8)), stats


def icon(pic: Image.Image, size: int = 128) -> Image.Image:
    """The picture inside the common frame: a heavy ink rule and a thin one, like a broadside block."""
    s = 512
    outer, gap, inner = (round(f * s) for f in FRAME)
    frame = Image.new("RGB", (s, s), tuple(INK.astype(int)))
    d = ImageDraw.Draw(frame)
    d.rectangle((outer, outer, s - 1 - outer, s - 1 - outer), fill=tuple(PARCH.astype(int)))
    a = outer + gap
    d.rectangle((a, a, s - 1 - a, s - 1 - a), fill=tuple(INK.astype(int)))
    b = a + inner
    # the subject sits a little larger in the icon than in the engraving: the frame already gives the margin
    crop = round(WORK * (1 - FILL) * 0.28)
    frame.paste(
        pic.crop((crop, crop, WORK - crop, WORK - crop)).resize((s - 2 * b, s - 2 * b), Image.LANCZOS), (b, b)
    )
    return frame.resize((size, size), Image.LANCZOS)


def process(raw: Path, out_dir: Path) -> dict:
    """engraving.jpg (512) and icon.png (128) in out_dir; stats with `ok` for the ink and accent bounds."""
    with Image.open(raw) as src:
        pic, stats = normalise(src)
    out_dir.mkdir(parents=True, exist_ok=True)
    pic.resize((512, 512), Image.LANCZOS).save(out_dir / "engraving.jpg", quality=90)
    icon(pic).save(out_dir / "icon.png", optimize=True)
    stats["ok"] = INK_RANGE[0] <= stats["ink"] <= INK_RANGE[1] and stats["accent"] <= ACCENT_MAX
    return stats


@dataclass(frozen=True)
class Picture:
    dir: Path
    engraving: Path
    icon: Path
    hash: str  # sha256 of the engraving, first 16 hex digits
    style: int


class Artist:
    """Draws feat pictures one at a time: codex exec image_gen, then processing; None when it fails.

    Files go to root/<feat id>/: raw.png (the generator's output, kept to reprocess when the style
    changes), engraving.jpg, icon.png and prompt.txt. They are not tied to a save branch.
    """

    def __init__(
        self, root: Path = DEFAULT_ROOT, *,
        codex: str | None = None, timeout_s: float = 180, attempts: int = 2,
    ) -> None:
        self.root = root
        self.codex = codex or os.environ.get("GOTHIC_AI_CODEX") or shutil.which("codex")
        self.timeout_s, self.attempts = timeout_s, attempts
        self._lock = asyncio.Lock()

    async def draw(self, feat_id: str, emblem: str) -> Picture | None:
        if not self.codex:
            log.warning("feat art: codex not found, feat %s keeps its category emblem", feat_id)
            return None
        out = self.root / feat_id
        async with self._lock:
            for attempt in range(1, self.attempts + 1):
                with tempfile.TemporaryDirectory(prefix="feat-art-") as tmp:
                    try:
                        raw = await self._generate(Path(tmp), emblem)
                    except OSError as error:  # git or codex cannot be started
                        log.warning("feat art: %s attempt %d: %s", feat_id, attempt, error)
                        continue
                    if raw is None:
                        log.warning("feat art: %s attempt %d: no picture", feat_id, attempt)
                        continue
                    try:
                        picture, stats = await asyncio.to_thread(_store, raw, out, emblem)
                    except (OSError, ValueError) as error:
                        log.warning("feat art: %s attempt %d: %s", feat_id, attempt, error)
                        continue
                if picture is None:
                    log.warning("feat art: %s attempt %d: out of bounds %s", feat_id, attempt, stats)
                    continue
                log.info("feat art: %s drawn %s", feat_id, stats)
                return picture
        return None

    async def _generate(self, tmp: Path, emblem: str) -> Path | None:
        """One image_gen call in an empty repository with a read-only sandbox; the picture's path."""
        git = ["git", "-C", str(tmp)]
        for cmd in ([*git, "init", "-q"], [*git, "commit", "-q", "--allow-empty", "-m", "init"]):
            proc = await asyncio.create_subprocess_exec(
                *cmd, stdout=asyncio.subprocess.DEVNULL, stderr=asyncio.subprocess.DEVNULL
            )
            if await proc.wait():
                return None
        images = [arg for a in STYLE["anchors"] for arg in ("-i", str(KIT / a))]
        proc = await asyncio.create_subprocess_exec(
            str(self.codex), "exec", "--skip-git-repo-check", "-s", "read-only", *images, "-",
            cwd=tmp, stdin=asyncio.subprocess.PIPE, stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.DEVNULL,
        )
        try:
            stdout, _ = await asyncio.wait_for(
                proc.communicate((WRAP + prompt(emblem)).encode()), self.timeout_s
            )
        except TimeoutError:
            return None
        finally:
            if proc.returncode is None:  # timed out or cancelled: the call must not outlive the feat
                proc.kill()
                await proc.wait()
        paths = re.findall(r"(/\S+\.png)", stdout.decode(errors="replace"))
        return Path(paths[-1]) if proc.returncode == 0 and paths else None


def _store(raw: Path, out: Path, emblem: str) -> tuple[Picture | None, dict]:
    """Keep the raw picture, process it in place; a Picture when it is within the bounds."""
    out.mkdir(parents=True, exist_ok=True)
    shutil.copy(raw, out / "raw.png")
    stats = process(out / "raw.png", out)
    if not stats["ok"]:
        return None, stats
    (out / "prompt.txt").write_text(prompt(emblem))
    digest = hashlib.sha256((out / "engraving.jpg").read_bytes()).hexdigest()[:16]
    return Picture(out, out / "engraving.jpg", out / "icon.png", digest, STYLE_VERSION), stats


def main(argv: list[str]) -> int:
    logging.basicConfig(level=logging.INFO, format="%(message)s")
    match argv:
        case ["gen", out, emblem]:
            async def gen() -> Path | None:
                with tempfile.TemporaryDirectory() as tmp:
                    raw = await Artist()._generate(Path(tmp), emblem)
                    if raw:
                        Path(out).parent.mkdir(parents=True, exist_ok=True)
                        shutil.copy(raw, out)
                        Path(out).with_suffix(".txt").write_text(prompt(emblem))
                    return raw
            return 0 if asyncio.run(gen()) else 1
        case ["process", raw, out_dir]:
            print(json.dumps(process(Path(raw), Path(out_dir))))
            return 0
        case ["draw", feat_id, emblem]:
            picture = asyncio.run(Artist().draw(feat_id, emblem))
            print(picture.dir if picture else "failed")
            return 0 if picture else 1
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

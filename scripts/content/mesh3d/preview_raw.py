"""Render every raw mesh triangle on CPU, with unlit vertex color and Z up."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import subprocess
import sys
import textwrap

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.collections import PolyCollection
import numpy as np
from PIL import Image, ImageDraw, ImageFont, ImageOps


def render(source: Path, output: Path):
    with np.load(source, allow_pickle=False) as raw:
        vertices = raw["vertices"]
        faces = raw["faces"]
        color = raw["color"]

    # The same orthographic camera is used for both upstream Z-up meshes.
    camera = np.array([1.5, -2.0, 1.2], dtype=np.float32)
    camera /= np.linalg.norm(camera)
    right = np.cross([0.0, 0.0, 1.0], camera)
    right /= np.linalg.norm(right)
    up = np.cross(camera, right)
    centered = vertices - (vertices.min(axis=0) + vertices.max(axis=0)) / 2
    projected = centered @ np.stack([right, up], axis=1)
    # Painter's order preserves overlapping front surfaces without a GPU.
    order = np.argsort((centered @ camera)[faces].mean(axis=1), kind="stable")
    face_color = np.clip(color[faces].mean(axis=1), 0, 1)[order]
    face_color = np.where(face_color <= 0.0031308, 12.92 * face_color,
                          1.055 * face_color ** (1 / 2.4) - 0.055)
    collection = PolyCollection(projected[faces[order]], facecolors=face_color,
                                edgecolors="none", antialiaseds=False)
    fig, ax = plt.subplots(figsize=(7, 7), dpi=160, facecolor="#e8e5df")
    ax.set_facecolor("#e8e5df")
    ax.add_collection(collection)
    bounds = np.max(np.abs(projected), axis=0)
    radius = float(bounds.max()) * 1.08
    ax.set(xlim=(-radius, radius), ylim=(-radius, radius), aspect="equal")
    ax.axis("off")
    fig.subplots_adjust(left=0, right=1, bottom=0, top=1)
    output.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(output, dpi=160, facecolor=fig.get_facecolor())
    plt.close(fig)
    print(f"{output}: {len(vertices)} vertices, {len(faces)} triangles; CPU, all faces")


def compose_sheet(item: str, batch_root: Path, sources_dir: Path, output: Path,
                  render_errors: dict[Path, str]):
    """Compare each concept with both seeds from each model on a light background."""
    width, height, label_height = 480, 480, 112
    background = "#e8e5df"
    sheet = Image.new("RGB", (5 * width, 2 * (height + label_height)), background)
    draw = ImageDraw.Draw(sheet)
    font = ImageFont.load_default(size=18)
    journal_path = batch_root / "batch.jsonl"
    journal = {}
    if journal_path.exists():
        for line in journal_path.read_text().splitlines():
            record = json.loads(line)
            if record.get("event", "").startswith("variant_"):
                journal[(record["input"], record["model"], record["seed"])] = record
    for version in (1, 2):
        concept = f"{item}-v{version}"
        top = (version - 1) * (height + label_height)
        columns = [(sources_dir / f"{concept}.png", f"{concept} / concept", "")]
        for model in ("trellis", "trellis2"):
            for seed in (1, 2):
                variant = batch_root / concept / f"{model}-s{seed}"
                meta_path = variant / "meta.json"
                record = journal.get((f"{concept}.png", model, seed), {})
                meta = json.loads(meta_path.read_text()) if meta_path.exists() else record
                seconds = meta.get("inference_seconds", meta.get("seconds"))
                seconds_label = f"{seconds:.1f} s" if isinstance(seconds, (int, float)) else "time unknown"
                faces = meta.get("mesh", {}).get("faces")
                faces_label = f"{faces:,} triangles" if isinstance(faces, int) else "triangles unknown"
                name = "TRELLIS v1" if model == "trellis" else "TRELLIS.2 512"
                label = f"{name} / seed {seed}\n{seconds_label} / {faces_label}"
                attempts = meta.get("attempts", [])
                last = attempts[-1] if attempts else {}
                reason = (render_errors.get(variant) or last.get("error") or meta.get("error")
                          or meta.get("reason") or record.get("reason")
                          or last.get("outcome") or meta.get("outcome")
                          or "not generated; no metadata")
                if reason == "running":
                    reason = "interrupted during generation"
                columns.append((variant / "preview.png", label, str(reason)))
        for column, (source, label, reason) in enumerate(columns):
            left = column * width
            if source.exists():
                with Image.open(source) as image:
                    thumb = ImageOps.contain(image.convert("RGBA"), (width - 24, height - 24))
                sheet.paste(thumb, (left + (width - thumb.width) // 2,
                                   top + (height - thumb.height) // 2), thumb)
            else:
                draw.multiline_text((left + 20, top + 100),
                    textwrap.fill(reason or "concept source missing", width=42,
                                  max_lines=13, placeholder="..."),
                    fill="#703b35", font=font, spacing=6)
            draw.multiline_text((left + 12, top + height + 8), label,
                                fill="#282623", font=font, spacing=5)
    output.parent.mkdir(parents=True, exist_ok=True)
    sheet.save(output, format="JPEG", quality=92)
    print(output)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, nargs="?")
    parser.add_argument("output", type=Path, nargs="?")
    parser.add_argument("--batch-root", type=Path)
    parser.add_argument("--sources-dir", type=Path)
    parser.add_argument("--sheets-dir", type=Path)
    args = parser.parse_args()
    if args.batch_root is not None:
        if args.sources_dir is None or args.sheets_dir is None or args.source or args.output:
            parser.error("--batch-root requires --sources-dir and --sheets-dir, without positional paths")
        render_errors = {}
        items = ("barrel", "bench", "chest", "crate", "table")
        for item in items:
            for version in (1, 2):
                for model in ("trellis", "trellis2"):
                    for seed in (1, 2):
                        variant = args.batch_root / f"{item}-v{version}" / f"{model}-s{seed}"
                        raw, preview = variant / "raw.npz", variant / "preview.png"
                        if raw.exists() and not preview.exists():
                            # Matplotlib retains a Path per triangle; release its entire
                            # process heap before loading the next multi-million-face mesh.
                            result = subprocess.run([sys.executable, str(Path(__file__).resolve()),
                                str(raw), str(preview)], capture_output=True, text=True)
                            print(result.stdout, end="")
                            if result.returncode:
                                render_errors[variant] = f"preview failed (exit {result.returncode})"
                                print(result.stderr, file=sys.stderr, end="")
        for item in items:
            compose_sheet(item, args.batch_root, args.sources_dir,
                          args.sheets_dir / f"{item}-batch.jpg", render_errors)
        if render_errors:
            raise SystemExit(1)
    elif args.source is not None and args.output is not None and not (args.sources_dir or args.sheets_dir):
        render(args.source, args.output)
    else:
        parser.error("provide source and output, or --batch-root, --sources-dir and --sheets-dir")

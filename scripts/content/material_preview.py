#!/usr/bin/env python3
"""Build reduced material bakeoff sheets using Pillow and a CPU numpy renderer.

    uv run --no-project --with numpy --with pillow python \
        scripts/content/material_preview.py --registry data/content/registry.jsonl \
        --content-root ~/.local/share/gothic-ai/content/bakeoff --output /tmp/sheets

Add ``--set city-1`` to render every include texture from the matching staged
set as one materials.jpg sheet (albedo 2x2, normal and obliquely lit preview).
Architectural modules in city-2 are shown once in their physical proportions,
undoing the square storage stretch for albedo, normal and lit previews.

Albedo is sRGB. Normal PNGs encode tangent XYZ in [0, 1], with positive Y up;
scalar maps are linear. The preview uses one fixed camera, directional light,
exposure and Lambert + isotropic GGX BRDF for every variant. It is an inspection
render, not a prediction of the game's renderer. No model or GPU is loaded.
"""

from __future__ import annotations

import argparse
import json
import math
import re
import textwrap
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont

from alpha_material import ALPHA_TEST_THRESHOLD


PIPELINES = ("flux-marigold", "flux-filter", "sdxl-marigold", "sdxl-filter", "stablematerials")
ROWS = ("Albedo 2 x 2", "Normal (Y+)", "Roughness", "Lambert + GGX")
REQUIRED_MAPS = ("albedo", "normal", "roughness", "metallic", "ao")
CHECKER_COLOURS = ((64, 68, 74), (104, 110, 118))


def normalize(value: np.ndarray) -> np.ndarray:
    return value / np.maximum(np.linalg.norm(value, axis=-1, keepdims=True), 1e-8)


def srgb_to_linear(value: np.ndarray) -> np.ndarray:
    return np.where(value <= 0.04045, value / 12.92, ((value + 0.055) / 1.055) ** 2.4)


def linear_to_srgb(value: np.ndarray) -> np.ndarray:
    value = np.clip(value, 0, 1)
    return np.where(value <= 0.0031308, value * 12.92, 1.055 * value ** (1 / 2.4) - 0.055)


def rgb_image(value: np.ndarray) -> Image.Image:
    return Image.fromarray(np.rint(np.clip(value, 0, 1) * 255).astype(np.uint8))


def checkerboard(size: tuple[int, int], cell: int = 12) -> Image.Image:
    """An unlit screen-space backing makes discarded albedo pixels visible."""
    width, height = size
    y, x = np.mgrid[:height, :width]
    colours = np.asarray(CHECKER_COLOURS, dtype=np.uint8)
    return Image.fromarray(colours[(x // cell + y // cell) % 2])


def alpha_preview(image: Image.Image) -> Image.Image:
    """Show the same binary cutoff as the game, without blending hidden RGB."""
    if image.mode != "RGBA":
        return image
    rgba = np.asarray(image)
    result = np.asarray(checkerboard(image.size)).copy()
    visible = rgba[..., 3] / 255 >= ALPHA_TEST_THRESHOLD
    result[visible] = rgba[..., :3][visible]
    return Image.fromarray(result)


def tiled_panel(image: Image.Image, size: int) -> Image.Image:
    half = size // 2
    tiled = Image.new(image.mode, (size, size))
    tile = image.resize((half, half), Image.Resampling.LANCZOS)
    for offset in ((0, 0), (half, 0), (0, half), (half, half)):
        tiled.paste(tile, offset)
    return alpha_preview(tiled)


def sample_periodic(image: np.ndarray, u: np.ndarray, v: np.ndarray, wrap_axes: str = "uv") -> np.ndarray:
    """Bilinear sampling; bounded module axes clamp instead of wrapping.

    PNG row zero is the top (+Y). ``wrap_axes`` specifies the axes whose image
    edges form a seamless repeat; a panel uses neither axis.
    """
    h, w = image.shape[:2]
    x = (u % 1 if "u" in wrap_axes else np.clip(u, 0, 1)) * w - 0.5
    y = ((1 - v) % 1 if "v" in wrap_axes else np.clip(1 - v, 0, 1)) * h - 0.5
    ix, iy = np.floor(x).astype(np.int32), np.floor(y).astype(np.int32)
    tx, ty = (x - ix)[..., None], (y - iy)[..., None]
    x0, x1 = (ix % w, (ix + 1) % w) if "u" in wrap_axes else (ix.clip(0, w - 1), (ix + 1).clip(0, w - 1))
    y0, y1 = (iy % h, (iy + 1) % h) if "v" in wrap_axes else (iy.clip(0, h - 1), (iy + 1).clip(0, h - 1))
    a, b = image[y0, x0], image[y0, x1]
    c, d = image[y1, x0], image[y1, x1]
    return (a * (1 - tx) + b * tx) * (1 - ty) + (c * (1 - tx) + d * tx) * ty


def plane_preview(maps: dict[str, np.ndarray], size: int, *, aspect: float = 1.,
                  repeats: tuple[float, float] = (2., 2.), wrap_axes: str = "uv") -> Image.Image:
    """Perspective plane, lit from above and the left.

    All BRDF operations are in linear colour. AO affects ambient illumination
    only; roughness is perceptual (microfacet alpha = roughness squared).
    The physical width/height is ``aspect``; the longest side stays fixed.
    Defaults retain the square bakeoff plane with two repeats on each axis.
    """
    if not math.isfinite(aspect) or aspect <= 0:
        raise ValueError("preview physical aspect must be finite and positive")
    camera = np.array((2.35, -3.2, 3.3), dtype=np.float32)
    forward = normalize(-camera)
    right = normalize(np.cross(forward, np.array((0, 0, 1), dtype=np.float32)))
    up = normalize(np.cross(right, forward))
    y, x = np.mgrid[:size, :size].astype(np.float32)
    spread = math.tan(math.radians(34) * 0.5)
    sx = ((x + 0.5) / size * 2 - 1) * spread
    sy = (1 - (y + 0.5) / size * 2) * spread
    rays = normalize(forward + sx[..., None] * right + sy[..., None] * up)
    position = camera - camera[2] / rays[..., 2, None] * rays
    half_width, half_height = min(aspect, 1.), min(1 / aspect, 1.)
    on_plane = (np.abs(position[..., 0]) <= half_width) & (np.abs(position[..., 1]) <= half_height)
    u = (position[..., 0] / half_width + 1) * 0.5 * repeats[0]
    v = (position[..., 1] / half_height + 1) * 0.5 * repeats[1]
    sample = lambda name: sample_periodic(maps[name], u, v, wrap_axes)
    sampled_albedo = sample("albedo")
    albedo = srgb_to_linear(sampled_albedo[..., :3])
    normal = normalize(sample("normal") * 2 - 1)
    roughness = np.clip(sample("roughness"), 0.045, 1)
    metallic = np.clip(sample("metallic"), 0, 1)
    ao = np.clip(sample("ao"), 0, 1)
    light = normalize(np.array((-0.8, -0.45, 0.85), dtype=np.float32))
    view = normalize(camera - position)
    halfway = normalize(view + light)
    dot = lambda a, b: np.maximum(np.sum(a * b, axis=-1, keepdims=True), 0)
    nl, nv = dot(normal, light), dot(normal, view)
    nh, vh = dot(normal, halfway), dot(view, halfway)
    alpha2 = roughness**4
    distribution = alpha2 / (math.pi * (nh * nh * (alpha2 - 1) + 1) ** 2 + 1e-8)

    def smith_g1(nd: np.ndarray) -> np.ndarray:
        return 2 * nd / np.maximum(nd + np.sqrt(alpha2 + (1 - alpha2) * nd**2), 1e-8)

    geometry = smith_g1(nl) * smith_g1(nv)
    f0 = 0.04 * (1 - metallic) + albedo * metallic
    fresnel = f0 + (1 - f0) * (1 - vh) ** 5
    specular = distribution * geometry * fresnel / np.maximum(4 * nl * nv, 1e-6)
    diffuse = (1 - fresnel) * (1 - metallic) * albedo / math.pi
    direct = (diffuse + specular) * nl * 2.8
    ambient = (albedo * (1 - metallic) * 0.20 + f0 * 0.025) * ao
    colour = direct + ambient
    # Fixed highlight roll-off; identical exposure for every material.
    colour = colour / (1 + colour)
    result = linear_to_srgb(colour)
    if sampled_albedo.shape[-1] == 4:
        discarded = on_plane & (sampled_albedo[..., 3] < ALPHA_TEST_THRESHOLD)
        backing = np.asarray(checkerboard((size, size)), dtype=np.float32) / 255
        result[discarded] = backing[discarded]
    result[~on_plane] = (0.125, 0.135, 0.15)
    return rgb_image(result)


def contained_path(root: Path, relative: str) -> Path:
    path = (root / relative).resolve()
    if not path.is_relative_to(root.resolve()):
        raise ValueError(f"content path escapes root: {relative}")
    return path


def read_maps(record: dict, root: Path) -> tuple[dict[str, np.ndarray] | None, str | None]:
    files = record.get("files", {})
    reason = record.get("parameters", {}).get("skip_reason") or record.get("skip_reason")
    missing = [name for name in REQUIRED_MAPS if name not in files or not contained_path(root, files[name]).is_file()]
    if missing:
        if reason:
            return None, str(reason)
        raise ValueError(f"{record['id']}: missing maps {', '.join(missing)} without skip_reason")
    maps = {}
    for name in REQUIRED_MAPS:
        mode = "RGB" if name in ("albedo", "normal") else "L"
        with Image.open(contained_path(root, files[name])) as source:
            if name == "albedo" and ("A" in source.getbands() or "transparency" in source.info):
                mode = "RGBA"
            maps[name] = np.asarray(source.convert(mode), dtype=np.float32) / 255
        if mode == "L":
            maps[name] = maps[name][..., None]
    dimensions = {image.shape[:2] for image in maps.values()}
    if len(dimensions) != 1:
        raise ValueError(f"{record['id']}: map dimensions differ: {sorted(dimensions)}")
    return maps, None


def font(size: int) -> ImageFont.ImageFont:
    # Pillow's bundled default font is portable; prefer a scalable system font.
    for name in ("DejaVuSans.ttf", "/System/Library/Fonts/Supplemental/Arial.ttf"):
        try:
            return ImageFont.truetype(name, size)
        except OSError:
            pass
    return ImageFont.load_default(size=size)


def fitted_lines(draw: ImageDraw.ImageDraw, text: str, face: ImageFont.ImageFont, width: int) -> list[str]:
    lines, line = [], ""
    for word in text.split():
        trial = (line + " " + word).strip()
        if line and draw.textbbox((0, 0), trial, font=face)[2] > width:
            lines.append(line)
            line = word
        else:
            line = trial
    if line:
        lines.append(line)
    return lines


def skip_panel(reason: str, size: int) -> Image.Image:
    panel = Image.new("RGB", (size, size), (44, 40, 43))
    draw = ImageDraw.Draw(panel)
    draw.text((14, 12), "SKIPPED", font=font(17), fill=(245, 178, 156))
    lines = fitted_lines(draw, reason, font(14), size - 28)
    for row, line in enumerate(lines[: (size - 54) // 18]):
        draw.text((14, 43 + row * 18), line, font=font(14), fill=(225, 219, 219))
    return panel


def module_panel(image: Image.Image, size: int, aspect: float) -> Image.Image:
    """Undo square UV storage into one physical rectangle within a fixed cell."""
    if not math.isfinite(aspect) or aspect <= 0:
        raise ValueError("preview physical aspect must be finite and positive")
    width, height = max(1, round(size * min(aspect, 1.))), max(1, round(size * min(1 / aspect, 1.)))
    panel = Image.new("RGB", (size, size), (32, 34, 38))
    panel.paste(alpha_preview(image.resize((width, height), Image.Resampling.LANCZOS)),
                ((size - width) // 2, (size - height) // 2))
    return panel


def make_sheet(material: str, records: dict[str, dict], root: Path, output: Path, size: int) -> None:
    left, margin, gap, top, bottom = 124, 18, 12, 90, 48
    width = left + len(PIPELINES) * (size + gap) + margin
    height = top + len(ROWS) * (size + gap) + bottom
    sheet = Image.new("RGB", (width, height), (24, 28, 34))
    draw = ImageDraw.Draw(sheet)
    draw.text((margin, 14), material.replace("-", " "), font=font(25), fill=(242, 243, 245))
    draw.text((margin, 47), "Same scale, camera, light and exposure for every variant", font=font(14), fill=(169, 181, 197))
    for row, label in enumerate(ROWS):
        for index, line in enumerate(textwrap.wrap(label, 13)):
            draw.text((margin, top + row * (size + gap) + 14 + index * 20), line, font=font(15), fill=(207, 215, 226))
    for col, pipeline in enumerate(PIPELINES):
        record = records[pipeline]
        maps, reason = read_maps(record, root)
        x = left + col * (size + gap)
        draw.text((x, top - 25), pipeline, font=font(16), fill=(232, 236, 242))
        if maps is None:
            panels = [skip_panel(reason or "Generation skipped", size) for _ in ROWS]
        else:
            albedo = rgb_image(maps["albedo"])
            # Each quarter is a full tile. No seam guides are painted over it.
            tiled = tiled_panel(albedo, size)
            panels = [tiled, rgb_image(maps["normal"]).resize((size, size), Image.Resampling.LANCZOS),
                      rgb_image(np.repeat(maps["roughness"], 3, axis=2)).resize((size, size), Image.Resampling.LANCZOS),
                      plane_preview(maps, size)]
        for row, panel in enumerate(panels):
            sheet.paste(panel, (x, top + row * (size + gap)))
    footer = "sRGB albedo; linear maps; normal Y+; 2 repeats on preview plane; no painted seam guides"
    draw.text((margin, height - 29), footer, font=font(13), fill=(169, 181, 197))
    sheet.save(output / f"{material}.jpg", quality=88, optimize=True, subsampling=0)


def make_set_sheet(set_name: str, records: list[dict], root: Path, output: Path, size: int) -> Path:
    """Each material gets the same three views, light and exposure."""
    margin, gap, header, top, footer = 18, 10, 70, 82, 36
    columns = min(3, len(records))
    if columns == 0:
        raise ValueError("cannot render an empty material set")
    cell_width, cell_height = 3 * size + 2 * gap, size + header
    rows = math.ceil(len(records) / columns)
    width = 2 * margin + columns * cell_width + (columns - 1) * margin
    height = top + rows * cell_height + (rows - 1) * margin + footer
    sheet = Image.new("RGB", (width, height), (24, 28, 34))
    draw = ImageDraw.Draw(sheet)
    draw.text((margin, 14), f"Khorinis / {set_name} / {len(records)} materials", font=font(25), fill=(242, 243, 245))
    draw.text((margin, 47), f"{set_generator_label(records)} / same camera, oblique light and exposure", font=font(14), fill=(169, 181, 197))
    for index, record in enumerate(records):
        maps, reason = read_maps(record, root)
        if maps is None:
            raise ValueError(f"{record['id']}: selected material is skipped: {reason}")
        x = margin + (index % columns) * (cell_width + margin)
        y = top + (index // columns) * (cell_height + margin)
        texture = record["replaces"][0]
        parameters = record["parameters"]
        material_class = parameters["material_class"]
        decision = parameters.get("decision")
        is_module = decision in ("module", "panel", "strip")
        has_alpha = maps["albedo"].shape[-1] == 4
        draw.text((x, y), texture, font=font(14), fill=(240, 242, 246))
        caption = f"{decision} / {material_class}" if decision else material_class
        draw.text((x, y + 21), caption, font=font(14), fill=(176, 195, 214))
        if has_alpha:
            draw.text((x, y + 38), f"alpha cutoff {ALPHA_TEST_THRESHOLD:g}; albedo only in game",
                      font=font(11), fill=(176, 195, 214))
        if is_module:
            aspect = float(parameters["source_aspect"])
            axes = parameters.get("tiling_axes", "uv" if parameters.get("layout") == "tiled" else "")
            panels = (module_panel(rgb_image(maps["albedo"]), size, aspect),
                      module_panel(rgb_image(maps["normal"]), size, aspect),
                      plane_preview(maps, size, aspect=aspect, repeats=(1., 1.), wrap_axes=axes))
            albedo_label = "Albedo / 1 module"
        else:
            tiled = tiled_panel(rgb_image(maps["albedo"]), size)
            panels = (tiled, rgb_image(maps["normal"]).resize((size, size), Image.Resampling.LANCZOS),
                      plane_preview(maps, size))
            albedo_label = "Albedo 2 x 2"
        for panel_index, (label, panel) in enumerate(zip((albedo_label, "Normal (Y+)", "Oblique light / GGX"), panels)):
            px = x + panel_index * (size + gap)
            draw.text((px, y + (54 if has_alpha else 45)), label, font=font(12), fill=(169, 181, 197))
            sheet.paste(panel, (px, y + header))
    repeat_note = "physical module proportions; homogeneous surfaces repeat 2 x 2" if any(
        record["parameters"].get("decision") in ("module", "panel", "strip") for record in records
    ) else "2 repeats on preview plane"
    draw.text((margin, height - 25), f"sRGB albedo; linear maps; normal Y+; {repeat_note}; no original game pixels", font=font(13), fill=(169, 181, 197))
    output.mkdir(parents=True, exist_ok=True)
    destination = output / "materials.jpg"
    sheet.save(destination, quality=88, optimize=True, subsampling=0)
    return destination


def set_generator_label(records: list[dict]) -> str:
    """Describe the generators actually selected for this sheet."""
    names = {"imagegen-filter": "image_gen", "flux-filter": "FLUX.1-schnell"}
    generators = sorted({names.get(record["pipeline"], record["pipeline"]) for record in records})
    return " + ".join(generators) + " + generated height filter"


def read_registry(path: Path) -> dict[str, dict[str, dict]]:
    groups: dict[str, dict[str, dict]] = {}
    for number, line in enumerate(path.read_text().splitlines(), 1):
        if not line.strip():
            continue
        record = json.loads(line)
        if record.get("kind") != "material":
            continue
        if record.get("parameters", {}).get("set"):
            continue
        material = record.get("parameters", {}).get("material")
        if material is None:
            continue
        if not re.fullmatch(r"[a-z0-9][a-z0-9-]*", material):
            raise ValueError(f"line {number}: material must be a safe lowercase slug")
        pipeline = record.get("pipeline")
        if pipeline not in PIPELINES:
            raise ValueError(f"line {number}: unexpected bakeoff pipeline {pipeline!r}")
        variants = groups.setdefault(material, {})
        if pipeline in variants:
            raise ValueError(f"line {number}: duplicate {material}/{pipeline}")
        variants[pipeline] = record
    if not groups:
        raise ValueError("registry has no bakeoff materials with parameters.material")
    for material, variants in groups.items():
        missing = set(PIPELINES) - variants.keys()
        if missing:
            raise ValueError(f"{material}: missing pipeline records: {', '.join(sorted(missing))}")
    return groups


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--registry", type=Path, required=True)
    parser.add_argument("--content-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--set", dest="set_name", help="render one named material set into materials.jpg")
    parser.add_argument("--mapping", type=Path, default=Path(__file__).resolve().parents[2] / "data/content/city-materials.csv")
    parser.add_argument("--cell-size", type=int, default=256, help="even panel size in pixels (128..384)")
    args = parser.parse_args()
    if args.cell_size < 128 or args.cell_size > 384 or args.cell_size % 2:
        parser.error("--cell-size must be an even integer from 128 to 384")
    root = args.content_root.expanduser().resolve()
    try:
        if args.set_name:
            from pack_materials import select_set_records
            records = [json.loads(line) for line in args.registry.read_text().splitlines() if line.strip()]
            selected = select_set_records(records, args.set_name, args.mapping)
            destination = make_set_sheet(args.set_name, selected, root, args.output, args.cell_size)
            print(f"Wrote {len(selected)} material previews to {destination}")
            return
        groups = read_registry(args.registry)
        args.output.mkdir(parents=True, exist_ok=True)
        for material, records in groups.items():
            make_sheet(material, records, root, args.output, args.cell_size)
        print(f"Wrote {len(groups)} reduced JPG sheets to {args.output}")
    except (ValueError, OSError) as error:
        parser.error(str(error))


if __name__ == "__main__":
    main()

"""The launcher's icons from the channel's logo, the red wax seal with our G.

Sources here: seal-raster.png — the channel avatar's seal on parchment (brand kit `logo/letter/seal.png`, 1200 px,
cropped around the seal, centre 600,600, 940 px across, so the seal fills 84 % of the tile, then 824 px), and
seal-small.svg — the kit's flat seal for small sizes. Brand rule: 48 px and up the raster seal, 32 px and less the
flat one. The tile keeps the macOS grid of the old icon: 824 px with radius 184 on a 1024 canvas.

    uv run --no-project --with pillow --with resvg-py==0.5.0 python launcher/app/icons-src/build.py

Writes into ../src-tauri/icons: icon.icns (iconutil, macOS), icon.ico, icon.png, 32x32.png, 128x128.png,
128x128@2x.png, tray.png (macOS menu bar template: black seal, the G cut through) and tray-linux.png (AppIndicator).
"""
import io
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

import resvg_py
from PIL import Image, ImageDraw

SRC = Path(__file__).parent
OUT = SRC.parent / "src-tauri/icons"
BODY, RADIUS, CANVAS = 824, 184, 1024
OFF = (CANVAS - BODY) // 2

small_svg = (SRC / "seal-small.svg").read_text()
RIM, G = re.findall(r' d="([^"]+)"', small_svg)


def svg_png(svg, size):
    return Image.open(io.BytesIO(bytes(resvg_py.svg_to_bytes(svg_string=svg, width=size, height=size)))).convert("RGBA")


def tile_mask(size):
    """The rounded tile, antialiased by drawing it 4× larger."""
    k = 4 * size / CANVAS
    m = Image.new("L", (4 * size, 4 * size), 0)
    ImageDraw.Draw(m).rounded_rectangle((OFF * k, OFF * k, (OFF + BODY) * k - 1, (OFF + BODY) * k - 1),
                                        radius=RADIUS * k, fill=255)
    return m.resize((size, size), Image.LANCZOS)


raster = Image.new("RGBA", (CANVAS, CANVAS), (0, 0, 0, 0))
raster.paste(Image.open(SRC / "seal-raster.png").convert("RGBA"), (OFF, OFF))
raster.putalpha(tile_mask(CANVAS))


def big(size):
    return raster.resize((size, size), Image.LANCZOS)


def small(size):
    """The parchment of the big tile under the flat seal, drawn at the pixel size so its edges stay crisp."""
    s = BODY * 0.92 / 256
    seal = (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {CANVAS} {CANVAS}">'
            f'<g transform="translate({CANVAS / 2 - 128 * s} {CANVAS / 2 - 128 * s}) scale({s})">'
            f'<path fill="#9C2121" d="{RIM}"/><path fill="#F1E3C6" fill-rule="evenodd" d="{G}"/></g></svg>')
    im = big(size)
    seal = svg_png(seal, size)
    seal.putalpha(Image.composite(seal.getchannel("A"), Image.new("L", (size, size), 0), im.getchannel("A")))
    return Image.alpha_composite(im, seal)


def icon(size):
    return small(size) if size <= 32 else big(size)


def tray():
    """22 pt at 2×: black where the wax is, clear in the G; macOS tints a template image itself."""
    s = 32 / 256
    svg = (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 44 44"><defs><mask id="m">'
           f'<g transform="translate(6 6) scale({s})"><path fill="#fff" d="{RIM}"/>'
           f'<path fill="#000" fill-rule="evenodd" d="{G}"/></g></mask></defs>'
           f'<rect width="44" height="44" fill="#000" mask="url(#m)"/></svg>')
    return svg_png(svg, 44)


OUT.mkdir(exist_ok=True)
icon(32).save(OUT / "32x32.png", optimize=True)
icon(128).save(OUT / "128x128.png", optimize=True)
icon(256).save(OUT / "128x128@2x.png", optimize=True)
icon(512).save(OUT / "icon.png", optimize=True)
tray().save(OUT / "tray.png", optimize=True)
svg_png(small_svg, 64).save(OUT / "tray-linux.png", optimize=True)

sizes = [16, 24, 32, 48, 64, 128, 256]
icon(256).save(OUT / "icon.ico", sizes=[(s, s) for s in sizes], append_images=[icon(s) for s in sizes[:-1]])

with tempfile.TemporaryDirectory() as tmp:
    iconset = Path(tmp) / "icon.iconset"
    iconset.mkdir()
    for pt in (16, 32, 128, 256, 512):
        icon(pt).save(iconset / f"icon_{pt}x{pt}.png")
        icon(2 * pt).save(iconset / f"icon_{pt}x{pt}@2x.png")
    if shutil.which("iconutil"):
        subprocess.run(["iconutil", "-c", "icns", iconset, "-o", OUT / "icon.icns"], check=True)
    else:
        print("iconutil not found (macOS only): icon.icns left as it was")

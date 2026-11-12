"""Top-down map of a world region from the converted base: top surface by material class with hill shading,
waynet, chunk grid, objects. Usage: pilot_map.py REPO OUT.png [--rect X0,Z0,X1,Z1] [--mark X0,Z0,X1,Z1]
[--cell M] [--names REGEX] [--objects]"""
import argparse
import json
import re
import sys
from pathlib import Path

import numpy as np

ap = argparse.ArgumentParser()
ap.add_argument('repo')
ap.add_argument('out')
ap.add_argument('--world', default='NEWWORLD')
ap.add_argument('--rect')
ap.add_argument('--mark', action='append', default=[])
ap.add_argument('--cell', type=float, default=2.0)
ap.add_argument('--names')
ap.add_argument('--objects', action='store_true')
ap.add_argument('--title', default='')
a = ap.parse_args()

sys.path.insert(0, str(Path(a.repo) / 'scripts/content/world'))
import world_census as census  # noqa: E402
import world_classes as wc  # noqa: E402

import matplotlib  # noqa: E402
matplotlib.use('Agg')
import matplotlib.pyplot as plt  # noqa: E402
from matplotlib.patches import Rectangle  # noqa: E402

base = census.resolve_base(None)
region = census.parse_rect(a.rect) if a.rect else None
mesh = census.load_world(base, a.world, None, region)
cls = census.material_classes(mesh.materials)[mesh.material]
P = mesh.positions
cell = a.cell
cl, h, tid = census.rasterize(P, cell)
# top hit per cell
gx0, gz0 = cl[:, 0].min(), cl[:, 1].min()
W, H = cl[:, 0].max() - gx0 + 1, cl[:, 1].max() - gz0 + 1
key = (cl[:, 0] - gx0) * H + (cl[:, 1] - gz0)
o = np.lexsort((h, key))
key, h, tid = key[o], h[o], tid[o]
last = np.r_[key[1:] != key[:-1], True]
top_h = np.full(W * H, np.nan)
top_c = np.full(W * H, '', dtype=object)
top_h[key[last]] = h[last]
top_c[key[last]] = cls[tid[last]]
# ground height under shells: the highest non-vegetation hit, for the shading
solid = cls[tid] != 'vegetation'
ks, hs = key[solid], h[solid]
lasts = np.r_[ks[1:] != ks[:-1], True]
gh = np.full(W * H, np.nan)
gh[ks[lasts]] = hs[lasts]
gh = np.where(np.isnan(gh), top_h, gh)

top_h, gh = top_h.reshape(W, H), gh.reshape(W, H)
top_c = top_c.reshape(W, H)
colors = {'ground': (0.47, 0.56, 0.30), 'rock': (0.55, 0.52, 0.48), 'water': (0.25, 0.42, 0.62),
          'man-made': (0.78, 0.66, 0.50), 'vegetation': (0.16, 0.33, 0.17), 'other': (0.8, 0.2, 0.8),
          '': (0.08, 0.09, 0.11)}
img = np.zeros((W, H, 3))
for name, c in colors.items():
    img[top_c == name] = c
g = np.nan_to_num(gh, nan=np.nanmin(gh))
dx, dz = np.gradient(g, cell)
shade = np.clip(0.75 + 0.9 * (-dx * 0.7 + dz * 0.7) / np.sqrt(1 + dx * dx + dz * dz), 0.35, 1.25)
img = np.clip(img * shade[:, :, None], 0, 1)

# image axes: Gothic x to the right (cell i spans Gothic x -(i+1)cell..-i cell), Gothic z up
img = img[::-1].transpose(1, 0, 2)          # rows = z, cols = Gothic x ascending
x_min, x_max = -(gx0 + W) * cell, -gx0 * cell
z_min, z_max = gz0 * cell, (gz0 + H) * cell
fig_w = 16
fig, ax = plt.subplots(figsize=(fig_w, fig_w * (z_max - z_min) / (x_max - x_min) + 0.6), dpi=140)
ax.imshow(img, extent=(x_min, x_max, z_min, z_max), origin='lower', interpolation='nearest')

net = json.loads((base / 'worlds' / a.world / 'waynet.json').read_text())['waynet']
pts = np.array([p['position'] for p in net['points']]) / 100.0
names = [p['name'] for p in net['points']]
for i, j in net['edges']:
    ax.plot([pts[i, 0], pts[j, 0]], [pts[i, 2], pts[j, 2]], color=(1, 0.93, 0.55), lw=0.7 if region is None else 1.4,
            alpha=0.9)
if a.names:
    rx = re.compile(a.names)
    for n, p in zip(names, pts):
        if rx.search(n) and x_min <= p[0] <= x_max and z_min <= p[2] <= z_max:
            ax.text(p[0], p[2], n, fontsize=5.5, color='white', ha='center', va='bottom',
                    bbox=dict(facecolor='black', alpha=0.45, pad=0.6, lw=0))
if a.objects:
    vobs, _ = census.load_vobs(base, a.world, None)
    style = {'trees': ('^', (0.75, 1.0, 0.55), 22), 'bushes': ('o', (0.55, 0.85, 0.45), 8),
             'rocks': ('s', (0.95, 0.95, 0.95), 8)}
    for kind, (marker, color, size) in style.items():
        q = np.array([v.position for v in vobs if v.visual and wc.object_class(v.visual) == kind]) / 100.0
        if len(q):
            ax.scatter(q[:, 0], q[:, 2], s=size, marker=marker, color=color, edgecolors='black', linewidths=0.3,
                       label=kind, zorder=5)
    ax.legend(loc='lower left', fontsize=8, framealpha=0.8)

cx0, cx1 = int(np.floor(x_min / 64)), int(np.ceil(x_max / 64))
cz0, cz1 = int(np.floor(z_min / 64)), int(np.ceil(z_max / 64))
for c in range(cx0, cx1 + 1):
    ax.axvline(c * 64, color='white', lw=0.25, alpha=0.35)
for c in range(cz0, cz1 + 1):
    ax.axhline(c * 64, color='white', lw=0.25, alpha=0.35)
step = 1 if region is not None else 4
ax.set_xticks([(c + 0.5) * 64 for c in range(cx0, cx1, step)], [str(c) for c in range(cx0, cx1, step)], fontsize=7)
ax.set_yticks([(c + 0.5) * 64 for c in range(cz0, cz1, step)], [str(c) for c in range(cz0, cz1, step)], fontsize=7)
ax.set_xlabel('chunk x (Gothic x, 64 m per chunk)')
ax.set_ylabel('chunk z')
for k, m in enumerate(a.mark):
    x0, z0, x1, z1 = (int(v) for v in m.split(','))
    ax.add_patch(Rectangle((x0 * 64, z0 * 64), (x1 - x0 + 1) * 64, (z1 - z0 + 1) * 64, fill=False,
                           edgecolor=('red', 'orange', 'cyan')[k % 3], lw=2.2, zorder=6))
ax.set_xlim(x_min, x_max)
ax.set_ylim(z_min, z_max)
if a.title:
    ax.set_title(a.title, fontsize=11)
fig.tight_layout()
fig.savefig(a.out)
print(a.out, img.shape, 'x', x_min, x_max, 'z', z_min, z_max)

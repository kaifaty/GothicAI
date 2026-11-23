#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = ["pillow>=11,<13", "numpy>=1.26"]
# ///
"""A/B of the lamp census: where a variant B takes lamp light away from A (a leak A had) or adds it.

rank.py A_DIR B_DIR OUT_DIR [--top N]       two runs with the same shot names
rank.py --pairs RUN OUT_DIR [--top N]       one run of census.py: <view>-1.png (vsm sides 1) against <view>-2.png
Blocks of 8x8 pixels: a block 'darker' when its mean luma in B is lower than in A by more than 6/255 and by
more than 25 % of A; 'brighter' the other way. Ranks shots by darker blocks, writes ranking.json and
sheets A | B | the difference (red: B darker, green: B brighter, x4).
"""
import json, sys, pathlib
import numpy as np
from PIL import Image

args = [x for x in sys.argv[1:] if not x.startswith('--')]
top = int(sys.argv[sys.argv.index('--top') + 1]) if '--top' in sys.argv else 40
if '--top' in sys.argv:
    args.remove(str(top))
if '--pairs' in sys.argv:
    # one run with <shot>-1.png (vsm sides 1) and <shot>-2.png (vsm sides 2)
    run, out = map(pathlib.Path, args[:2])
    pairs = [(pa, pa.with_name(pa.name[:-6] + '-2.png')) for pa in sorted(run.rglob('*-1.png'))]
else:
    a_dir, b_dir, out = map(pathlib.Path, args[:3])
    pairs = [(pa, b_dir / pa.relative_to(a_dir)) for pa in sorted(a_dir.rglob('*.png'))]
out.mkdir(parents=True, exist_ok=True)


def luma(p):
    im = np.asarray(Image.open(p).convert('RGB'), dtype=np.float32)
    return im, im @ np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)


def blocks(l, k=8):
    h, w = (l.shape[0] // k) * k, (l.shape[1] // k) * k
    return l[:h, :w].reshape(h // k, k, w // k, k).mean(axis=(1, 3))


rows = []
for pa, pb in pairs:
    if not pb.exists():
        continue
    ia, la = luma(pa)
    ib, lb = luma(pb)
    if la.shape != lb.shape:
        continue
    ba, bb = blocks(la), blocks(lb)
    d = ba - bb
    darker = (d > 6) & (d > 0.25 * ba)
    brighter = (-d > 6) & (-d > 0.25 * bb)
    rows.append(dict(shot=pa.stem.removesuffix('-1'), darker=int(darker.sum()), brighter=int(brighter.sum()),
                     blocks=int(d.size), lost=float(np.clip(d, 0, None).sum()), path_a=str(pa), path_b=str(pb)))

rows.sort(key=lambda r: (-r['darker'], -r['lost']))
json.dump(rows, open(out / 'ranking.json', 'w'), indent=1)
print(f'{len(rows)} shots; with darker blocks: {sum(1 for r in rows if r["darker"])}; '
      f'with brighter blocks: {sum(1 for r in rows if r["brighter"])}')
for r in rows[:top]:
    print(f'{r["shot"]:14s} darker {r["darker"]:5d} brighter {r["brighter"]:4d} lost {r["lost"]:9.0f}')
    ia, la = luma(r['path_a'])
    ib, lb = luma(r['path_b'])
    dd = la - lb
    vis = np.zeros_like(ia)
    vis[..., 0] = np.clip(dd * 4, 0, 255)
    vis[..., 1] = np.clip(-dd * 4, 0, 255)
    sheet = np.concatenate([ia, ib, vis], axis=1).astype(np.uint8)
    Image.fromarray(sheet).save(out / f'{r["shot"]}.jpg', quality=88)

"""Check a tree's GLB against the wind layout of COLOR_0 and the rules of the cards. Reads only the file.

    python tree_check.py TREE.glb [--json OUT.json]

The numbers it prints are the acceptance of the layout (tree_glb.py, engine-plan.md task 1):

- the colour is four normalised bytes;
- the trunk (the wood that reaches lowest) has R = G = B = 0 and A = 255;
- every other piece of wood is a branch: B = 0, one G of its own, R is 0 at the trunk and grows along it;
- every card has the G of a branch (0 on the trunk and round the leader), one vertex with B = 0, the stem, which
  lies on that branch's axis, B = 255 at the free edge and B between the two on the way, not one step, and the
  R of the branch at the stem; past 255 branches the phases repeat, and the card's branch is the nearest one of
  its phase;
- A grows from the trunk to the crown's outline;
- every triangle of a card has a twin wound the other way over the same vertices;
- no closed piece of wood is inside out.

Exits 1 when a rule fails.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import tree_glb  # noqa: E402


def components(index: np.ndarray, count: int) -> np.ndarray:
    """A label per vertex: the smallest vertex number of its connected piece."""
    label = np.arange(count)
    while True:
        low = label[index].min(axis=1)
        nxt = label.copy()
        for k in range(3):
            np.minimum.at(nxt, index[:, k], low)
        nxt = nxt[nxt]
        if np.array_equal(nxt, label):
            return label
        label = nxt


def weld(position: np.ndarray, index: np.ndarray, step: float = 1e-4):
    _, first, inverse = np.unique(np.round(position / step).astype(np.int64), axis=0, return_index=True,
                                  return_inverse=True)
    return inverse.reshape(-1)[index], inverse.reshape(-1), len(first)


def closed_parts(position: np.ndarray, index: np.ndarray) -> tuple[int, int]:
    """Closed pieces and how many of them are wound inward (volume_check.py's rule, without Blender)."""
    welded, remap, count = weld(position, index)
    label = components(welded, count)[welded[:, 0]]
    points = np.zeros((count, 3))
    points[remap] = position
    closed = inside_out = 0
    for part in np.unique(label):
        tris = welded[label == part]
        edges = np.concatenate([tris[:, (0, 1)], tris[:, (1, 2)], tris[:, (2, 0)]])
        if len(np.unique(edges, axis=0)) != len(edges):
            continue
        if not np.array_equal(np.unique(edges, axis=0), np.unique(edges[:, ::-1], axis=0)):
            continue
        closed += 1
        a, b, c = (points[tris[:, k]] for k in range(3))
        inside_out += float(np.einsum('ij,ij->i', a, np.cross(b, c)).sum()) < -1e-9
    return closed, int(inside_out)


def corr(a, b) -> float:
    a, b = np.asarray(a, float), np.asarray(b, float)
    if a.std() < 1e-9 or b.std() < 1e-9:
        return 0.
    return float(np.corrcoef(a, b)[0, 1])


def axis(position: np.ndarray, weight: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """A branch's axis from its rings: R is one per ring and grows along the branch."""
    values = np.unique(weight)
    return np.array([position[weight == v].mean(axis=0) for v in values]), values.astype(np.float64)


def on_axis(point: np.ndarray, line: np.ndarray, weight: np.ndarray) -> tuple[float, float]:
    """Distance from the point to the axis and the axis's R there."""
    a, b = line[:-1], line[1:]
    t = np.clip(np.einsum('ij,ij->i', point - a, b - a) / np.maximum(np.einsum('ij,ij->i', b - a, b - a), 1e-12), 0, 1)
    distance = np.linalg.norm(a + t[:, None] * (b - a) - point, axis=1)
    k = int(np.argmin(distance))
    return float(distance[k]), float(weight[k] + t[k] * (weight[k + 1] - weight[k]))


def check(path: Path) -> dict:
    prims = tree_glb.read(path)
    wood = [p for p in prims if p['material'].get('alphaMode') != 'MASK']
    cards = [p for p in prims if p['material'].get('alphaMode') == 'MASK']
    fails: list[str] = []
    out: dict = dict(file=Path(path).name, materials=[p['name'] for p in prims],
                     triangles={p['name']: int(len(p['index'])) for p in prims},
                     triangles_total=int(sum(len(p['index']) for p in prims)))

    def rule(ok: bool, text: str) -> None:
        if not ok:
            fails.append(text)

    rule(len(wood) == 1 and len(cards) == 1, f'expected one wood and one cutout material, got {out["materials"]}')
    for p in prims:
        rule(p['colour_format'] == (tree_glb.UBYTE, 'VEC4', True), f"{p['name']}: COLOR_0 is {p['colour_format']}")
    if fails:
        return out | dict(fails=fails)
    wood, cards = wood[0], cards[0]
    out['colour_format'] = 'VEC4 UNSIGNED_BYTE normalized'

    # wood: the trunk and the branches
    position, colour = wood['position'].astype(np.float64), wood['colour'].astype(np.int64)
    welded, remap, count = weld(position, wood['index'])
    label = components(welded, count)[remap]
    trunk = label[np.argmin(position[:, 2])]
    on_trunk = label == trunk
    out['trunk'] = dict(vertices=int(on_trunk.sum()), rgb_max=int(colour[on_trunk, :3].max()),
                        a_min=int(colour[on_trunk, 3].min()),
                        height_m=round(float(position[on_trunk, 2].max()), 3))
    rule(out['trunk']['rgb_max'] == 0, f"trunk: R, G or B up to {out['trunk']['rgb_max']}, must be 0")
    rule(out['trunk']['a_min'] == 255, f"trunk: A down to {out['trunk']['a_min']}, must be 255")
    branches, growth, tips = {}, [], []
    mixed = flutter = rootless = 0
    for part in np.unique(label[~on_trunk]):
        mine = label == part
        g = np.unique(colour[mine, 1])
        mixed += len(g) != 1 or g[0] == 0
        flutter += int(colour[mine, 2].max() != 0)
        r = colour[mine, 0]
        rootless += int(r.min() != 0)
        base = position[mine][r == r.min()].mean(axis=0)
        growth.append(corr(r, np.linalg.norm(position[mine] - base, axis=1)))
        tips.append(int(r.max()))
        branches.setdefault(int(g[0]), []).append(axis(position[mine], r))
    out['branches'] = dict(count=len(growth), phases=len(branches), mixed_phase=int(mixed), with_b=int(flutter),
                           r_not_0_at_trunk=int(rootless), r_growth_corr_min=round(min(growth), 3),
                           r_growth_corr_mean=round(float(np.mean(growth)), 3),
                           tips_at_255=int(sum(t == 255 for t in tips)), tips_below=sorted(t for t in tips if t < 255))
    rule(mixed == 0, f'{mixed} branches without one phase G of their own')
    rule(flutter == 0, f'{flutter} branches with B above 0')
    rule(rootless == 0, f'{rootless} branches whose R does not start at 0')
    rule(min(growth) >= .9, f'R does not grow along a branch: correlation {min(growth):.3f}')
    rule(len(branches) == min(len(growth), 255), 'two branches share a phase while phases are free')
    closed, inside_out = closed_parts(position, wood['index'])
    out['closed_parts'], out['inside_out_parts'] = closed, inside_out
    rule(inside_out == 0, f'{inside_out} closed parts are inside out')

    # cards
    position, colour, index = cards['position'].astype(np.float64), cards['colour'].astype(np.int64), cards['index']
    order = np.sort(index, axis=1)
    _, inverse, copies = np.unique(order, axis=0, return_inverse=True, return_counts=True)
    parity = np.array([(tuple(t) in ((a, b, c), (b, c, a), (c, a, b))) for t, (a, b, c) in zip(index.tolist(), order.tolist())])
    even = np.bincount(inverse.reshape(-1), weights=parity, minlength=len(copies))
    twins = int(((copies == 2) & (even == 1)).sum())
    out['cards'] = dict(triangles=int(len(index)), pairs=twins, unpaired=int(len(index) - 2 * twins))
    rule(len(index) == 2 * twins, f"{len(index) - 2 * twins} card triangles without a twin wound the other way")
    label = components(index, len(position))
    wrong_phase = stemless = edgeless = stepped = 0
    r_error, b_growth, spans, off_axis = [], [], [], []
    for part in np.unique(label):
        mine = np.flatnonzero(label == part)
        g = np.unique(colour[mine, 1])
        b = colour[mine, 2]
        stem = mine[b == 0]
        stemless += len(stem) != 1
        edgeless += int(b.max() != 255)
        stepped += int(not ((b > 0) & (b < 255)).any())
        if len(g) != 1 or (g[0] != 0 and int(g[0]) not in branches):
            wrong_phase += 1
            continue
        if len(stem) != 1:
            continue
        b_growth.append(corr(b, np.linalg.norm(position[mine] - position[stem[0]], axis=1)))
        spans.append(int(np.ptp(colour[mine, 0])))
        if g[0] == 0:
            r_error.append(int(colour[stem[0], 0]))
            continue
        distance, weight = min(on_axis(position[stem[0]], *line) for line in branches[int(g[0])])
        off_axis.append(distance)
        r_error.append(abs(colour[stem[0], 0] - weight))
    count = len(np.unique(label))
    out['cards'] |= dict(count=count, wrong_phase=int(wrong_phase), without_one_stem=int(stemless),
                         without_free_edge_255=int(edgeless), without_b_between=int(stepped),
                         stem_off_branch_axis_max_m=round(float(np.max(off_axis)), 4),
                         b_growth_corr_mean=round(float(np.mean(b_growth)), 3),
                         b_growth_corr_min=round(float(np.min(b_growth)), 3),
                         r_vs_branch_mean=round(float(np.mean(r_error)), 2), r_vs_branch_max=round(float(np.max(r_error)), 2),
                         r_span_in_card_max=int(np.max(spans)))
    rule(wrong_phase == 0, f'{wrong_phase} cards with a phase G of no branch')
    rule(stemless == 0, f'{stemless} cards without exactly one stem vertex (B = 0)')
    rule(edgeless == 0, f'{edgeless} cards whose free edge does not reach B = 255')
    rule(stepped == 0, f'{stepped} cards whose B is a step: no vertex between 0 and 255')
    rule(np.max(off_axis) <= .03, f'the stem of a card is {np.max(off_axis):.3f} m off the axis of its branch')
    rule(float(np.min(b_growth)) > 0, f'B does not grow from the stem: correlation {np.min(b_growth):.3f}')
    rule(np.max(r_error) <= 4, f'R of a card is {np.max(r_error):.1f} of 255 off its branch at the stem')

    # openness
    r, z = np.hypot(position[:, 0], position[:, 1]), position[:, 2]
    edges = np.arange(z.min(), z.max() + 1., 1.)
    outline = np.array([r[(z >= a - .5) & (z < a + 1.5)].max(initial=.3) for a in edges])
    share = r / np.interp(z, edges + .5, outline)
    inner, outer = colour[share < .35, 3], colour[share > .85, 3]
    out['openness'] = dict(corr_with_radius_share=round(corr(colour[:, 3], share), 3),
                           a_mean_inner=round(float(inner.mean()), 1) if len(inner) else None,
                           a_mean_outer=round(float(outer.mean()), 1), a_min=int(colour[:, 3].min()),
                           a_max=int(colour[:, 3].max()))
    rule(out['openness']['corr_with_radius_share'] >= .8, 'A does not grow from the trunk to the outline')
    rule(len(inner) == 0 or inner.mean() < 40, f'A in the depth of the crown is {inner.mean():.0f}, expected near 0')
    rule(outer.mean() > 215, f'A at the outline is {outer.mean():.0f}, expected near 255')

    everything = np.concatenate([p['position'] for p in prims])
    out['bbox_min_m'] = [round(float(v), 3) for v in everything.min(axis=0)]
    out['bbox_max_m'] = [round(float(v), 3) for v in everything.max(axis=0)]
    out['fails'] = fails
    return out


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('glb', type=Path, nargs='+')
    parser.add_argument('--json', type=Path)
    args = parser.parse_args()
    results = [check(path) for path in args.glb]
    text = json.dumps(results if len(results) > 1 else results[0], indent=1, ensure_ascii=False)
    print(text)
    if args.json:
        args.json.write_text(text + '\n')
    return 1 if any(r['fails'] for r in results) else 0


if __name__ == '__main__':
    raise SystemExit(main())

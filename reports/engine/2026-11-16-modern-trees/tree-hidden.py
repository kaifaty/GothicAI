#!/usr/bin/env python3
"""Replay [INTERNAL] treeOcclusionProbe captures from perf-trees.bench. Requires NumPy."""
import argparse
from collections import Counter, defaultdict
from itertools import product
import json
from pathlib import Path
import sys

import numpy as np


CORNERS = np.array(list(product((0, 1), repeat=3)))
SIZE_LIMITS = (1, 2, 4, 8, 16, 32, 64)
TERRAIN_ID = 0xffffffff
CLUSTER_TAG = 0x80000000
LEGACY, PRECISE, WIDE, ELIGIBLE, DRAWN = 1, 2, 4, 8, 16


def pack_depth(z):
    return (np.asarray(z, dtype=np.float32) - np.float32(.95)) * np.float32(20)


def ceil_pow2(n):
    return 1 << (int(n) - 1).bit_length()


def pyramid_size(screen, limit):
    size = np.array([ceil_pow2(n) for n in screen])
    while size.max() > limit:
        size = np.maximum(size // 2, 1)
    return size


class Depth:
    def __init__(self, pixels, base):
        self.pixels = pixels
        self.screen = np.array(pixels.shape[::-1])
        pot_width, pot_height = map(ceil_pow2, self.screen)
        self.tile = np.array([pot_width, pot_height]) // base
        tx, ty = self.tile
        w, h = base
        padded = np.zeros((pot_height, pot_width), dtype=np.float32)
        padded[:pixels.shape[0], :pixels.shape[1]] = pixels
        self.mips = [self.encode(padded.reshape(h, ty, w, tx).max(axis=(1, 3)))]
        while self.mips[-1].size > 1:
            mip = self.mips[-1]
            h, w = mip.shape
            dh, dw = max(h // 2, 1), max(w // 2, 1)
            self.mips.append(mip.reshape(dh, h // dh, dw, w // dw).max(axis=(1, 3)))

    @staticmethod
    def encode(z):
        padded = np.clip(pack_depth(z) + np.float32(1 / 32768), 0, 1)
        return np.uint16(np.rint(padded * np.float32(65535)))

    def load_gpu(self, filename):
        actual = np.fromfile(filename, dtype="<u2")
        if actual.size != sum(mip.size for mip in self.mips):
            raise ValueError(f"Unexpected HiZ data size in {filename}")
        mismatch = []
        offset = 0
        for i, mip in enumerate(self.mips):
            gpu = actual[offset:offset + mip.size].reshape(mip.shape)
            delta = np.abs(gpu.astype(np.int32) - mip)
            mismatch.append([int(np.count_nonzero(delta)), int(delta.max())])
            self.mips[i] = gpu
            offset += mip.size
        return mismatch

    def rectangle(self, rect):
        return np.trunc(rect * self.screen.astype(np.float32)).astype(int)

    def crop(self, image, rect):
        (x0, y0), (x1, y1) = np.clip(self.rectangle(rect), 0, self.screen - 1)
        return image[y0:y1 + 1, x0:x1 + 1]

    def test(self, rect, depth, method="stock"):
        if depth <= 0:
            return True, -1
        if method in ("pixels", "unpacked_pixels"):
            z = self.crop(self.pixels, rect).max()
            if method == "unpacked_pixels":
                return bool(depth <= z), 0
            return bool(pack_depth(depth) <= np.float32(self.encode(z) / 65535)), 0
        pixels = self.rectangle(rect)
        if method == "precise":
            pixels = np.clip(pixels, 0, self.screen - 1)
        tiles = np.trunc(pixels / self.tile).astype(int)
        lo, hi = np.clip(tiles, 0, np.array(self.mips[0].shape[::-1]) - 1)
        span = int((hi - lo).max())
        lod = (span - 1).bit_length() if span else 0
        if method == "tiles":
            lod = 0
        if method in ("tight_mip", "precise"):
            lod = 0
            while ((hi >> lod) - (lo >> lod)).max() > 1:
                lod += 1
        x0, y0 = lo >> lod
        x1, y1 = hi >> lod
        if method == "stock" and span:
            x1, y1 = x0 + 1, y0 + 1
        z = self.mips[lod][y0:y1 + 1, x0:x1 + 1].max()
        packed = pack_depth(depth)
        if method == "precise":
            packed = min(packed, 1.)
        return bool(packed <= np.float32(z / 65535)), lod


def project(box, model, vp, near):
    xyz = box[CORNERS, np.arange(3)]
    world = np.c_[xyz, np.ones(8, dtype=np.float32)] @ model
    clip = np.c_[world, np.ones(8, dtype=np.float32)] @ vp
    if clip[:, 3].min() < near:
        return np.array([[0., 0.], [1., 1.]], dtype=np.float32), 0.
    ndc = clip[:, :3] / clip[:, 3:]
    lo = np.minimum(ndc[:, :2].min(axis=0), 1.)
    hi = np.maximum(ndc[:, :2].max(axis=0), -1.)
    return np.array([lo, hi]) * np.float32(.5) + np.float32(.5), min(ndc[:, 2].min(), 1.)


def in_frustum(sphere, planes):
    return sphere[3] > 0 and (planes @ np.r_[sphere[:3], np.float32(1)] > -sphere[3]).all()


def bounds(points):
    return np.array([points.min(axis=0), points.max(axis=0)])


def grow(box, margin):
    return box + np.array([[-margin], [margin]], dtype=np.float32)


def card_tiers(points, triangles, full_y):
    # Each card has its own connected vertices, including at coarse levels.
    parent = np.arange(len(points))

    def root(i):
        while parent[i] != i:
            parent[i] = parent[parent[i]]
            i = parent[i]
        return i

    for a, b, c in triangles:
        group = root(a)
        parent[root(b)] = group
        parent[root(c)] = group
    roots = np.array([root(i) for i in range(len(points))])
    total = np.bincount(roots, weights=points[:, 1], minlength=len(points))
    count = np.maximum(np.bincount(roots, minlength=len(points)), 1)
    center = (total / count)[roots[triangles[:, 0]]]
    lo, hi = full_y
    return np.clip((3 * (center - lo) / (hi - lo)).astype(int), 0, 2)


def read_geometry(repo, content):
    sys.path.insert(0, str(repo / "scripts/content/mesh3d/blender"))
    import tree_glb

    meshes = content / "meshes"
    shared, geometry = {}, {}
    for name, entry in json.loads((meshes / "meshes.json").read_text()).items():
        if not name.startswith("OWN_TREE_FIR_A_"):
            continue
        levels = [entry["glb"], *(level["glb"] for level in entry["levels"])]
        all_vertices = []
        for level, filename in enumerate(levels):
            for part in tree_glb.read(meshes / filename):
                crown = part["material"].get("alphaMode") == "MASK"
                # Builder Z-up meters -> engine Y-up centimeters, reflecting glTF X.
                points = part["position"][:, (0, 2, 1)] * np.array([-100, 100, -100], dtype=np.float32)
                tris = part["index"]
                box = bounds(points[tris.flatten()])
                all_vertices.append(points)
                if crown and level == 0:
                    full_y = box[:, 1]
                labels = card_tiers(points, tris, full_y) if crown else np.zeros(len(tris), dtype=int)
                parts = [(bounds(points[t.flatten()]), len(t)) for i in range(3) if len(t := tris[labels == i])]
                geometry[name, level, crown] = box, len(tris), parts
        shared[name] = bounds(np.concatenate(all_vertices))
    return shared, geometry


def replay(path, shared, geometry):
    def binary(suffix, dtype):
        return np.fromfile(path.with_suffix(suffix), dtype=dtype).reshape(screen[::-1])

    lines = path.with_suffix(".txt").read_text().splitlines()
    if len(lines[0].split()) > 5:
        return replay_precise(path, lines, shared, geometry)
    w, h, hw, hh, near = map(float, lines[0].split())
    screen, base = (int(w), int(h)), (int(hw), int(hh))
    vp = np.array(lines[1].split(), dtype=np.float32).reshape(4, 4)
    planes = np.array([line.split() for line in lines[2:8]], dtype=np.float32)
    pre, main = binary(".pre.f32", "<f4"), binary(".main.f32", "<f4")
    last_file = path.with_suffix(".previous.f32")
    last = binary(".previous.f32", "<f4") if last_file.is_file() else main
    last_matrix = path.with_suffix(".previous.txt")
    last_vp = np.loadtxt(last_matrix, dtype=np.float32).reshape(4, 4) if last_matrix.is_file() else vp
    ids = binary(".id.u32", "<u4")
    owner, count = np.unique(ids, return_counts=True)
    pixels = dict(zip(map(int, owner), map(int, count), strict=True))
    terrain = (ids == TERRAIN_ID) & (main == pre) & (pre < 1)
    rows = [line.split() for line in lines[8:] if line]
    spruce_surface = np.isin(ids, [int(row[0]) + 1 for row in rows])
    cpu, stock = Depth(pre, base), Depth(pre, base)
    mip_diff = stock.load_gpu(path.with_suffix(".hiz.u16"))
    larger = {str(n): Depth(pre, pyramid_size(screen, n)) for n in (128, 256)}
    previous, final = Depth(last, base), Depth(main, base)
    forest, frustum, submitted = set(), set(), set()
    visible, mip_count, counts = Counter(), Counter(), Counter()
    crown_causes, terrain_shares, filled_holes, sizes, margins = {}, {}, {}, {}, []
    kept, tris, parts_kept, visible_drops = defaultdict(set), Counter(), Counter(), Counter()
    tree_tris, tree_meshlets = Counter(), Counter()
    split = (("split", stock, "stock"), ("split128", larger["128"], "stock"), ("split_exact", stock, "pixels"))
    mesh_names = {}
    variant_labels = set()
    for fields in rows:
        instance, level, meshlets, canopy = map(int, fields[:4])
        sphere = np.array(fields[4:8], dtype=np.float32)
        model = np.array(fields[8:24], dtype=np.float32).reshape(4, 4)[:, :3]
        tree = tuple(model[3])
        lone = tree[0] == 48500 and tree[2] == 500
        if not lone:
            forest.add(tree)
            visible[tree] += pixels.get(instance + 1, 0)
        if not in_frustum(sphere, planes):
            continue
        if not lone:
            frustum.add(tree)
            counts["frustum_clusters"] += 1
        key = tuple(map(float, fields[24:30]))
        box = np.array(key, dtype=np.float32).reshape(2, 3)
        margin, faded = map(float, fields[30:32])
        rect, z = project(grow(box, margin), model, vp, near)
        passed, mip = stock.test(rect, z)
        if not meshlets:
            continue
        counts["cpu_gpu_decision_mismatches"] += passed != cpu.test(rect, z)[0]
        if passed and canopy:
            counts["all_canopy_clusters"] += 1
            counts["all_canopy_meshlets"] += meshlets
        if lone:
            continue
        if key not in mesh_names:
            error, name = min((float(np.max(np.abs(box - v))), name) for name, v in shared.items())
            if error > .02:
                raise ValueError(f"No spruce with bounds {key}. Closest {name}, {error:g} cm off")
            mesh_names[key] = name
        tight, triangles, pieces = geometry[mesh_names[key], level, bool(canopy)]
        counts["parts_before_culling"] += len(pieces)
        rest_rect, rest_z = project(box, model, vp, near)
        size = float((np.diff(np.clip(rest_rect, 0, 1), axis=0) * screen).max())
        if passed:
            submitted.add(tree)
            counts["hiz_meshlets"] += meshlets
            counts["hiz_canopy_clusters" if canopy else "hiz_bark_clusters"] += 1
            counts["triangles"] += triangles
            tree_tris[tree] += triangles
            tree_meshlets[tree] += meshlets
            mip_count[mip] += 1
            margins.append([margin, faded])
            sizes[tree] = size

        results = {
            "exact_neighbors": stock.test(rect, z, "neighbors")[0],
            "tight_mip": stock.test(rect, z, "tight_mip")[0],
            "exact_tiles": stock.test(rect, z, "tiles")[0],
            "raw_pixels": stock.test(rect, z, "pixels")[0],
            "raw_pixels_no_quantization": stock.test(rect, z, "unpacked_pixels")[0],
            "zero_wind": stock.test(rest_rect, rest_z)[0],
        }
        for label, depth in larger.items():
            results[label] = depth.test(rect, z)[0]
            results[label + "_tight_mip"] = depth.test(rect, z, "tight_mip")[0]
        for label, bbox, bound in (("tight_box", tight, margin), ("tight_box_fade", tight, faded),
                                   ("faded_wind", box, faded)):
            xy, zz = project(grow(bbox, bound), model, vp, near)
            results[label] = stock.test(xy, zz)[0]
            if label == "tight_box":
                results["tight_raw"] = stock.test(xy, zz, "pixels")[0]
        for label, depth, matrix in (("previous", previous, last_vp), ("final", final, vp)):
            xy, zz = project(grow(box, margin), model, matrix, near)
            results[label] = passed and depth.test(xy, zz)[0]
            results[label + "_raw"] = passed and depth.test(xy, zz, "pixels")[0]
        results.update((f"size_{n}px", passed and size >= n) for n in SIZE_LIMITS)
        variant_labels.update(results)
        for label, keep in results.items():
            if keep:
                kept[label].add(tree)
                tris[label] += triangles
                parts_kept[label] += 1
            elif pixels.get(instance + 1, 0):
                visible_drops[label] += 1
        passing_parts = set()
        for piece, faces in pieces:
            piece = grow(piece, margin)
            xyz = piece[CORNERS, np.arange(3)]
            mid = piece.mean(axis=0)
            center = np.r_[mid, np.float32(1)] @ model
            radius = np.linalg.norm((xyz - mid) @ model[:3], axis=1).max()
            xy, zz = project(piece, model, vp, near)
            if in_frustum(np.r_[center, radius], planes):
                for label, depth, method in split:
                    if depth.test(xy, zz, method)[0]:
                        passing_parts.add(label)
                        kept[label].add(tree)
                        tris[label] += faces
                        parts_kept[label] += 1
        if pixels.get(instance + 1, 0):
            for label, _, _ in split:
                if label not in passing_parts:
                    visible_drops[label] += 1
        if passed and canopy:
            causes = ("exact_neighbors", "tight_mip", "exact_tiles", "raw_pixels", "raw_pixels_no_quantization")
            cropped = stock.crop(pre, rect)
            residual = "near_plane" if z <= 0 else (
                "clear_depth_inside_box" if (cropped == 1).any() else "deeper_geometry_inside_box")
            crown_causes[tree] = next((k for k in causes if not results[k]), residual)
            terrain_shares[tree] = float(np.mean(stock.crop(terrain, rect) & (cropped < z)))
            filled = (cropped >= z) & (stock.crop(main, rect) < z)
            filled_holes[tree] = (int(filled.sum()), int((filled & stock.crop(spruce_surface, rect)).sum()))

    seen = {tree for tree, area in visible.items() if area}
    hidden = submitted - seen
    ground = [terrain_shares[tree] for tree in hidden if tree in terrain_shares]
    # View distance in meters, with the camera's fixed 1 km far plane.
    terrain_depth = pre[terrain].astype(np.float64)
    terrain_distances = near * 1000 / (100000 + (near - 100000) * terrain_depth)
    data = dict(
        screen=screen, hiz=base, tile_pixels=list(map(int, stock.tile)), forest=len(forest), frustum=len(frustum),
        hiz_pass=len(submitted), visible=len(seen), zero_pixels=len(hidden), visible_outside_hiz=len(seen - submitted),
        hidden_triangles=sum(tree_tris[t] for t in hidden), hidden_meshlets=sum(tree_meshlets[t] for t in hidden),
        counts=dict(counts), mips=dict(mip_count), mip_gpu_cpu_diff=mip_diff,
        size_px_min_median_max=np.quantile(list(sizes.values()), (0, .5, 1)).round(3).tolist() if sizes else None,
        wind_cm_full_fade_min_max=np.quantile(margins, (0, 1), axis=0).round(2).tolist() if margins else None,
        hidden_crown_reasons=dict(Counter(crown_causes[t] for t in hidden if t in crown_causes)),
        terrain_pixels=int((ids == TERRAIN_ID).sum()), terrain_pixels_matching_prepass=int(terrain.sum()),
        hidden_crown_terrain_any=sum(g > 0 for g in ground), hidden_crown_terrain_mean=float(np.mean(ground)) if ground else None,
        hidden_crown_samples_blocked_only_in_main=sum(filled_holes[t][0] for t in hidden if t in filled_holes),
        hidden_crown_samples_blocked_only_in_main_by_spruce=sum(filled_holes[t][1] for t in hidden if t in filled_holes),
        terrain_prepass_max_view_m=float(terrain_distances.max()) if terrain_distances.size else None,
        terrain_prepass_pixels_past_300m=int((terrain_distances >= 300).sum()),
    )
    data["variants"] = {}
    for label in sorted(variant_labels | {label for label, _, _ in split}):
        removed = submitted - kept[label]
        data["variants"][label] = dict(
            trees_removed=len(removed), trees_reintroduced=len(kept[label] - submitted),
            visible_trees_removed=len(removed & seen),
            visible_pixels_in_removed_trees=sum(visible[t] for t in removed), visible_clusters_removed=visible_drops[label],
            triangles_removed=counts["triangles"] - tris[label], clusters_or_parts_submitted=parts_kept[label],
        )
    return data


def replay_precise(path, lines, shared, geometry):
    w, h, hw, hh, near, mode = lines[0].split()
    screen, base = (int(w), int(h)), (int(hw), int(hh))
    vp = np.array(lines[1].split(), dtype=np.float32).reshape(4, 4)
    planes = np.array([line.split() for line in lines[2:8]], dtype=np.float32)
    pre = np.fromfile(path.with_suffix(".pre.f32"), dtype="<f4").reshape(screen[::-1])
    depth = Depth(pre, base)
    mip_diff = depth.load_gpu(path.with_suffix(".hiz.u16"))
    ids = np.fromfile(path.with_suffix(".id.u32"), dtype="<u4").reshape(screen[::-1])
    owner, count = np.unique(ids[ids >= CLUSTER_TAG] ^ np.uint32(CLUSTER_TAG), return_counts=True)
    decisions = np.fromfile(path.with_suffix(".decisions.u32"), dtype="<u4").reshape(-1, 3)
    flags, meshlets, triangles = decisions.T
    if owner.size and owner.max() >= len(flags):
        raise ValueError(f"A G-buffer owner has no visibility record in {path}")
    legacy, precise, wide = ((flags & bit) != 0 for bit in (LEGACY, PRECISE, WIDE))
    removed = legacy & ~precise
    violating = removed[owner] if mode == "verify" else np.zeros(len(owner), dtype=bool)
    drawn = (flags & DRAWN) != 0
    pixels = dict(zip(map(int, owner), map(int, count), strict=True))

    visible, tree_tris, tree_meshlets = Counter(), Counter(), Counter()
    canopy = Counter()
    submitted = set()
    counts = Counter()
    mesh_names = {}
    for line in lines[8:]:
        fields = line.split()
        _, level, meshlet_count, crown = map(int, fields[:4])
        cluster = int(fields[32])
        key = tuple(map(float, fields[24:30]))
        model = np.array(fields[8:24], dtype=np.float32).reshape(4, 4)[:, :3]
        tree = tuple(model[3])
        lone = tree[0] == 48500 and tree[2] == 500

        sphere = np.array(fields[4:8], dtype=np.float32)
        cpu_eligible = meshlet_count > 0 and in_frustum(sphere, planes)
        counts["cpu_frustum_mismatches"] += cpu_eligible != bool(flags[cluster] & ELIGIBLE)
        if cpu_eligible:
            box = np.array(fields[33:39], dtype=np.float32).reshape(2, 3)
            rect, z = project(box, model, vp, float(near))
            cpu_precise = depth.test(rect, z, "precise")[0]
        else:
            cpu_precise = False
        counts["cpu_precise_passed"] += cpu_precise
        counts["gpu_precise_passed"] += precise[cluster]
        counts["cpu_gpu_decision_mismatches"] += cpu_precise != precise[cluster]
        counts["trees_clusters_reintroduced_from_wide"] += precise[cluster] and not wide[cluster]
        counts["trees_clusters_reintroduced_from_legacy"] += precise[cluster] and not legacy[cluster]
        if drawn[cluster] and crown:
            canopy["all_canopy_clusters"] += 1
            canopy["all_canopy_meshlets"] += meshlet_count

        if lone:
            continue
        visible[tree] += pixels.get(cluster, 0)
        if drawn[cluster]:
            if key not in mesh_names:
                box = np.array(key, dtype=np.float32).reshape(2, 3)
                error, name = min((float(np.max(np.abs(box - v))), name) for name, v in shared.items())
                if error > .02:
                    raise ValueError(f"No spruce with bounds {key}. Closest {name}, {error:g} cm off")
                mesh_names[key] = name
            _, triangle_count, _ = geometry[mesh_names[key], level, bool(crown)]
            submitted.add(tree)
            tree_tris[tree] += triangle_count
            tree_meshlets[tree] += meshlet_count
        if removed[cluster]:
            counts["forest_clusters_removed"] += 1
            counts["forest_meshlets_removed"] += meshlet_count
            counts["forest_triangles_removed"] += triangles[cluster]

    seen = {tree for tree, area in visible.items() if area}
    hidden = submitted - seen
    return dict(
        mode=mode, screen=screen, hiz=base, mip_gpu_cpu_diff=mip_diff,
        hiz_pass=len(submitted), visible_trees=len(seen), zero_pixels=len(hidden),
        hidden_triangles=sum(tree_tris[t] for t in hidden), hidden_meshlets=sum(tree_meshlets[t] for t in hidden),
        main_clusters=int(drawn.sum()), main_meshlets=int(meshlets[drawn].sum()),
        all_canopy_clusters=canopy["all_canopy_clusters"], all_canopy_meshlets=canopy["all_canopy_meshlets"],
        clusters_removed=int(removed.sum()), meshlets_removed=int(meshlets[removed].sum()),
        triangles_removed=int(triangles[removed].sum()), clusters_reintroduced_from_legacy=int((precise & ~legacy).sum()),
        clusters_reintroduced_from_wide=int((precise & ~wide).sum()),
        violation_clusters=int(violating.sum()), violation_pixels=int(count[violating].sum()),
        unexpected_owner_clusters=int((~drawn[owner]).sum()), **{k: int(v) for k, v in counts.items()},
    )


def history_owner_samples(ids, main, previous, vp, previous_vp):
    """Approximate the missing previous owner IDs using depth agreement with the diagnostic frame."""
    h, w = ids.shape
    y, x = np.mgrid[2:h:4, 2:w:4]
    z = previous[y, x]
    clip = np.stack((2 * (x + .5) / w - 1, 2 * (y + .5) / h - 1, z, np.ones(x.shape)), axis=-1)
    current = clip @ (np.linalg.inv(previous_vp.astype(float)) @ vp.astype(float))
    x_ndc, y_ndc, depth = (current[..., :3] / current[..., 3:]).transpose(2, 0, 1)
    x = (x_ndc * .5 + .5) * w
    y = (y_ndc * .5 + .5) * h
    inside = (current[..., 3] > 0) & (x >= 0) & (x < w) & (y >= 0) & (y < h) & (z < 1)
    x, y, depth = x[inside].astype(int), y[inside].astype(int), depth[inside]
    agree = np.abs(main[y, x] - depth) <= 2e-5
    owners, counts = np.unique(ids[y[agree], x[agree]], return_counts=True)
    return {int(owner) for owner, count in zip(owners, counts, strict=True) if count >= 16}


class HistoryDepth(Depth):
    @staticmethod
    def encode(z):
        return z

    def sample(self, rect, base_skip, width):
        if width == 0:
            texels = self.crop(self.pixels, rect)
        else:
            lo, hi = np.clip(self.rectangle(rect), 0, self.screen - 1) // self.tile
            mip = base_skip
            while ((hi >> mip) - (lo >> mip)).max() >= width:
                mip += 1
            x0, y0 = lo >> mip
            x1, y1 = hi >> mip
            texels = self.mips[mip][y0:y1 + 1, x0:x1 + 1]
        return texels.max(), texels.size


def replay_history(path, shared, geometry):
    lines = path.with_suffix(".txt").read_text().splitlines()
    w, h, hw, hh, near = map(float, lines[0].split()[:5])
    screen = np.array([int(w), int(h)])
    precise = len(lines[0].split()) == 6
    vp = np.array(lines[1].split(), dtype=np.float32).reshape(4, 4)
    last_vp = np.loadtxt(path.with_suffix(".previous.txt"), dtype=np.float32).reshape(4, 4)
    planes = np.array([line.split() for line in lines[2:8]], dtype=np.float32)

    def image(suffix, dtype="<f4"):
        return np.fromfile(path.with_suffix(suffix), dtype=dtype).reshape(screen[::-1])

    pre, main, ids = image(".pre.f32"), image(".main.f32"), image(".id.u32", "<u4")
    seeds = history_owner_samples(ids, main, image(".previous.f32"), vp, last_vp)
    owners, counts = np.unique(ids, return_counts=True)
    pixels = dict(zip(map(int, owners), map(int, counts), strict=True))
    if precise:
        decisions = np.fromfile(path.with_suffix(".decisions.u32"), dtype="<u4").reshape(-1, 3)
        seeds = {o for o in seeds if o & CLUSTER_TAG and o != TERRAIN_ID and decisions[o ^ CLUSTER_TAG, 0] & PRECISE}
    else:
        stock = Depth(pre, np.array([int(hw), int(hh)]))
        stock.load_gpu(path.with_suffix(".hiz.u16"))

    trees, visible = [], set()
    for line in lines[8:]:
        fields = line.split()
        instance, level, meshlets, crown = map(int, fields[:4])
        model = np.array(fields[8:24], dtype=np.float32).reshape(4, 4)[:, :3]
        root = tuple(model[3])
        lone = root[0] == 48500 and root[2] == 500
        owner = (int(fields[32]) | CLUSTER_TAG) if precise else instance + 1
        if pixels.get(owner, 0):
            visible.add(root)
        key = np.array(fields[24:30], dtype=np.float32).reshape(2, 3)
        if precise:
            if not decisions[owner ^ CLUSTER_TAG, 0] & PRECISE:
                continue
            box = np.array(fields[33:39], dtype=np.float32).reshape(2, 3)
        else:
            sphere = np.array(fields[4:8], dtype=np.float32)
            if not meshlets or not in_frustum(sphere, planes):
                continue
            margin, faded = map(float, fields[30:32])
            wide = grow(key, margin)
            if not stock.test(*project(wide, model, vp, near), "stock")[0]:
                continue
        error, mesh = min((float(np.max(np.abs(key - value))), name) for name, value in shared.items())
        if error > .02:
            raise ValueError(f"No spruce with bounds {key}. Closest {mesh}, {error:g} cm off")
        tight, triangles, _ = geometry[mesh, level, bool(crown)]
        if not precise:
            box = np.clip(grow(tight, faded), wide[0], wide[1])
            if not stock.test(*project(box, model, vp, near), "precise")[0]:
                continue
        rect, z = project(box, model, vp, near)
        rect[0] -= 1 / screen
        rect[1] += 1 / screen
        trees.append((root, owner, triangles, rect, z - 8 * np.finfo(np.float32).eps, lone))

    tree_ids = {owner for _, owner, *_ in trees}
    if not precise:
        seeds &= tree_ids | {TERRAIN_ID}
    seed_depth = np.where(np.isin(ids, list(seeds)), np.minimum(pre, main), pre)
    forest = [tree for tree in trees if not tree[-1]]
    hidden = {root for root, *_ in forest} - visible
    seed_triangles = sum(tris for _, o, tris, *_ in trees if o in seeds)
    other_seed_meshlets = sum(int(decisions[o ^ CLUSTER_TAG, 1]) for o in seeds - tree_ids) if precise else None
    result = dict(
        zero_pixel_trees=len(hidden), zero_pixel_triangles=sum(tris for root, _, tris, *_ in forest if root in hidden),
        seed_clusters=len(seeds), seed_fir_triangles=seed_triangles, other_seed_meshlets=other_seed_meshlets,
    )
    for name, depth, excluded in (("phase_one", seed_depth, seeds), ("full_depth_limit", main, set())):
        pyramid = HistoryDepth(depth, pyramid_size(screen, 512))
        variants = [(f"{512 >> mip}x{width}", mip, width) for mip in range(3, -1, -1) for width in (2, 4, 8)]
        counts = {}
        for variant, mip, width in (*variants, ("pixels", 0, 0)):
            kept, saved, lost, samples, tests = set(), 0, 0, 0, 0
            for root, owner, triangles, rect, z, _ in forest:
                if owner in excluded or z <= 0:
                    kept.add(root)
                    continue
                furthest, reads = pyramid.sample(rect, mip, width)
                samples += reads
                tests += 1
                if min(z, 1) <= furthest:
                    kept.add(root)
                else:
                    lost += pixels.get(owner, 0)
                    if root in hidden:
                        saved += triangles
            counts[variant] = dict(
                trees_removed=len(hidden - kept), hidden_triangles_removed=saved, lost_pixels=lost,
                mean_reads=round(samples / tests, 2) if tests else 0,
            )
        result[name] = counts
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("content", type=Path)
    parser.add_argument("capture", nargs="+", type=Path)
    parser.add_argument("--repo", default=Path.cwd(), type=Path)
    parser.add_argument("--history", action="store_true", help="Replay the step 3b two-phase estimate")
    args = parser.parse_args()
    meshes = read_geometry(args.repo, args.content)
    if args.history:
        result = {str(p): replay_history(p, *meshes) for p in args.capture}
    else:
        result = {p.stem: replay(p, *meshes) for p in args.capture}
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()

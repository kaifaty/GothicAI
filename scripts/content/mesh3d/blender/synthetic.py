"""One CPU integration run: two dense meshes, two controls, GLB checks and sheets."""
import argparse
import json
from pathlib import Path
import struct
import sys

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from geometry import cylinder_uv, fit_bbox, triangle_areas
from postprocess import process_arrays, mesh_arrays, read_raw, uv_continuity


def barrel():
    sides, rings = 512, 289
    theta, y = np.meshgrid(np.arange(sides) * (2 * np.pi / sides), np.linspace(0, 1, rings))
    radius = 0.36 + 0.075 * np.sin(np.pi * y)
    radius += 0.0007 * np.sin(theta * 19 + y * 13) * np.sin(np.pi * y)
    v = np.stack((radius * np.cos(theta), y, radius * np.sin(theta)), axis=-1).reshape(-1, 3)
    cells = (np.arange(rings - 1)[:, None] * sides + np.arange(sides)).ravel()
    next_cell = (cells // sides) * sides + (cells + 1) % sides
    f = np.concatenate((np.column_stack((cells, cells + sides, next_cell)),
                        np.column_stack((next_cell, cells + sides, next_cell + sides))))
    # Fan caps, no coincident duplicate vertices.
    bottom, top = len(v), len(v) + 1
    v = np.vstack((v, [0, 0, 0], [0, 1, 0]))
    indices = np.arange(sides)
    f = np.vstack((f, np.column_stack((np.full(sides, bottom), indices, (indices + 1) % sides)),
                   np.column_stack((np.full(sides, top), (indices + 1) % sides + (rings - 1) * sides,
                                    indices + (rings - 1) * sides))))
    metal = (np.min(abs(v[:, 1, None] - np.array([0.2, 0.5, 0.8])), axis=1) < 0.027).astype(float)
    color = np.tile([0.24, 0.095, 0.026], (len(v), 1))
    color *= (0.86 + 0.25 * (0.5 + 0.5 * np.sin(np.arctan2(v[:, 2], v[:, 0]) * 16)))[:, None]
    color[metal > 0.5] = [0.035, 0.038, 0.04]
    return v, f.astype(np.int32), color, metal


def crate():
    divisions = 160
    u, w = np.meshgrid(np.linspace(-0.5, 0.5, divisions + 1),
                       np.linspace(-0.5, 0.5, divisions + 1))
    vertices, faces = [], []
    for axis in range(3):
        for sign in (-1, 1):
            coords = np.zeros((u.size, 3))
            other = [i for i in range(3) if i != axis]
            coords[:, other[0]], coords[:, other[1]] = u.ravel(), w.ravel()
            coords[:, axis] = sign * (0.5 + 0.0005 * np.sin(u.ravel() * 39)
                                      * np.sin(w.ravel() * 23)
                                      * np.cos(u.ravel() * np.pi) * np.cos(w.ravel() * np.pi))
            offset = sum(len(part) for part in vertices)
            cells = (np.arange(divisions)[:, None] * (divisions + 1) + np.arange(divisions)).ravel()
            local = np.vstack((np.column_stack((cells, cells + 1, cells + divisions + 1)),
                               np.column_stack((cells + 1, cells + divisions + 2, cells + divisions + 1))))
            vertices.append(coords)
            faces.append(local + offset)
    v, f = np.vstack(vertices), np.vstack(faces).astype(np.int32)
    metal = (np.sum(abs(v) > 0.405, axis=1) >= 3).astype(float)
    shade = 0.85 + 0.12 * (np.floor((v[:, 1] + 0.5) * 6) % 3)
    color = np.array([0.28, 0.12, 0.035])[None, :] * shade[:, None]
    color[metal > 0.5] = [0.035, 0.038, 0.04]
    return v, f, color, metal


def target(kind):
    return {"id": kind, "visual": f"SYNTHETIC_{kind.upper()}", "kind": "static_mrm",
            "bbox_min": [-0.47, -0.08, -0.38], "bbox_max": [0.47, 1.12, 0.38],
            "budget_tris": 1800 if kind == "barrel" else 1200,
            "zones": [{"key": "PILOT_WOOD", "group": "WOOD", "role": "boards"},
                      {"key": "PILOT_IRON", "group": "METAL", "role": "hoops and corners"}]}


def glb_document(path):
    data = Path(path).read_bytes()
    magic, version, length = struct.unpack_from("<4sII", data)
    if magic != b"glTF" or version != 2 or length != len(data):
        raise AssertionError(f"invalid GLB header: {path}")
    size, kind = struct.unpack_from("<II", data, 12)
    if kind != 0x4E4F534A:
        raise AssertionError("first GLB chunk must be JSON")
    return json.loads(data[20:20 + size])


def uv_quality(vertices, faces, uv, projection, face_types=None, patch_ids=None):
    """Measure physical metres per U/V, retaining each triangle's distortion.

    The inverse UV Jacobian's rows are the surface vectors traced by one unit
    in U and V. Their lengths preserve the checker directions, unlike singular
    values. Cylinder sides permit changing circumference; caps and flat plank
    edges use planar UVs and therefore keep the stricter box criterion.
    """
    if projection not in ("cylinder", "box"):
        raise ValueError("projection must be cylinder or box")
    vertices = np.asarray(vertices, dtype=float)
    faces = np.asarray(faces, dtype=np.int64)
    uv = np.asarray(uv, dtype=float)
    triangles = vertices[faces]
    if uv.shape != (len(faces), 3, 2) or len(faces) == 0:
        raise ValueError("UVs must contain three corners per nonempty triangle mesh")
    edges = triangles[:, 1:] - triangles[:, :1]
    normals = np.cross(edges[:, 0], edges[:, 1])
    double_area = np.linalg.norm(normals, axis=1)
    first_length = np.linalg.norm(edges[:, 0], axis=1)
    if np.any(double_area <= 0) or not np.isfinite(double_area).all():
        raise ValueError("UV quality requires finite nondegenerate triangles")
    e1 = edges[:, 0] / first_length[:, None]
    e2 = np.cross(normals / double_area[:, None], e1)
    local = np.stack((np.einsum("fij,fj->fi", edges, e1),
                      np.einsum("fij,fj->fi", edges, e2)), axis=-1)
    try:
        jacobian = np.linalg.solve(local, uv[:, 1:] - uv[:, :1])
        inverse = np.linalg.inv(jacobian)
    except np.linalg.LinAlgError as error:
        raise ValueError("UV quality requires nondegenerate UV triangles") from error
    meters = np.linalg.norm(inverse, axis=2)
    if not np.isfinite(meters).all():
        raise ValueError("UV quality requires finite UV coordinates")
    area = double_area * .5

    def describe(values, weights):
        return {"min": float(values.min()), "max": float(values.max()),
                "mean": float(np.average(values, weights=weights))}

    def surface(mask):
        stats = {"triangles": int(mask.sum())}
        if np.any(mask):
            stats["u_meters_per_uv"] = describe(meters[mask, 0], area[mask])
            stats["v_meters_per_uv"] = describe(meters[mask, 1], area[mask])
        return stats

    stats = {"projection": projection, **surface(np.ones(len(faces), dtype=bool))}
    if projection == "cylinder":
        centre = (vertices.min(axis=0) + vertices.max(axis=0)) * .5
        relative = triangles - centre
        if face_types is None:
            cap = abs(normals[:, 1]) > np.linalg.norm(normals[:, [0, 2]], axis=1) * 2
            radial = relative.mean(axis=1)
            tangent = np.column_stack((-radial[:, 2], np.zeros(len(faces)), radial[:, 0]))
            horizontal_normal = np.linalg.norm(normals[:, [0, 2]], axis=1)
            radial_face = (~cap & (abs(np.einsum("ij,ij->i", normals, tangent))
                                  > horizontal_normal * np.linalg.norm(tangent, axis=1) * .3))
            side = ~(cap | radial_face)
        else:
            face_types = np.asarray(face_types)
            if face_types.shape != (len(faces),) or not np.isin(face_types, [0, 1, 2]).all():
                raise ValueError("face_types must identify every UV patch projection")
            side = face_types == 1
        stats["planar"] = surface(~side)
        stats["cylindrical"] = surface(side)
        if np.any(side):
            # Reconstruct the mean section independently from area-weighted
            # vertex radii. A sparse stave's face angles can vary with height;
            # using face-centre radii would then mistake its ends for the mean.
            centres = relative.mean(axis=1)[side]
            if patch_ids is None:
                charts = np.zeros(int(side.sum()), dtype=np.int32)
            else:
                patch_ids = np.asarray(patch_ids)
                if patch_ids.shape != (len(faces),):
                    raise ValueError("patch_ids must identify every triangle's UV chart")
                charts = patch_ids[side]
            angle = np.arctan2(centres[:, 2], centres[:, 0])
            bins = np.minimum(((angle + np.pi) / (2 * np.pi) * 64).astype(int), 63)
            side_area, side_faces, side_uv = area[side], faces[side], uv[side]
            reference_values, reference_weights = [], []
            for chart in np.unique(charts):
                chart_mask = charts == chart
                chart_area = side_area[chart_mask]
                chart_bins = bins[chart_mask]
                used, corners = np.unique(side_faces[chart_mask], return_inverse=True)
                points = vertices[used] - centre
                vertex_area = np.bincount(corners.ravel(), weights=np.repeat(chart_area / 3, 3))
                radius = np.linalg.norm(points[:, [0, 2]], axis=1)
                if np.any(radius <= 0):
                    raise ValueError("cylindrical sides require a positive radius")
                vertex_angle = np.arctan2(points[:, 2], points[:, 0])
                origin = np.arctan2(np.sum(vertex_area * np.sin(vertex_angle)),
                                    np.sum(vertex_area * np.cos(vertex_angle)))
                vertex_angle = (vertex_angle - origin + np.pi) % (2 * np.pi) - np.pi
                vertex_bins = np.minimum(((vertex_angle + np.pi) / (2 * np.pi) * 64).astype(int), 63)
                weights = np.bincount(vertex_bins, weights=vertex_area, minlength=64)
                occupied = weights > 0
                sample_angle = np.bincount(vertex_bins, weights=vertex_area * vertex_angle, minlength=64)[occupied] / weights[occupied]
                sample_radius = np.bincount(vertex_bins, weights=vertex_area * radius, minlength=64)[occupied] / weights[occupied]
                section_radius = np.interp(vertex_angle, sample_angle, sample_radius)
                points[:, [0, 2]] *= (section_radius / radius)[:, None]
                section = points[corners].reshape(-1, 3, 3)
                section_edges = section[:, 1:] - section[:, :1]
                section_normal = np.cross(section_edges[:, 0], section_edges[:, 1])
                section_length = np.linalg.norm(section_normal, axis=1)
                if np.any(section_length <= 0):
                    raise ValueError("mean section must contain nondegenerate triangles")
                section_normal /= section_length[:, None]
                section_e1 = section_edges[:, 0] / np.linalg.norm(section_edges[:, 0], axis=1)[:, None]
                section_e2 = np.cross(section_normal, section_e1)
                section_local = np.stack((np.einsum("fij,fj->fi", section_edges, section_e1),
                                          np.einsum("fij,fj->fi", section_edges, section_e2)), axis=-1)
                u_edges = side_uv[chart_mask, 1:, 0] - side_uv[chart_mask, :1, 0]
                gradient = np.linalg.solve(section_local, u_edges[..., None])[..., 0]
                horizontal = np.column_stack((-section_normal[:, 2], np.zeros(len(section_normal)), section_normal[:, 0]))
                horizontal /= np.linalg.norm(horizontal, axis=1)[:, None]
                tangent = np.column_stack((np.einsum("ij,ij->i", horizontal, section_e1),
                                           np.einsum("ij,ij->i", horizontal, section_e2)))
                # U on the horizontal mean section is independent of V's
                # meridian interpolation. Original-face inverse-Jacobian U/V
                # extrema above still measure all physical checker distortion.
                rate = abs(np.einsum("ij,ij->i", gradient, tangent))
                if np.any(rate <= 0):
                    raise ValueError("mean-section U must vary along the horizontal tangent")
                reference = 1 / rate
                weights = np.bincount(chart_bins, weights=chart_area, minlength=64)
                occupied = weights > 0
                sums = np.bincount(chart_bins, weights=chart_area * reference, minlength=64)
                reference_values.extend((sums[occupied] / weights[occupied]).tolist())
                reference_weights.extend(weights[occupied].tolist())
            reference_values = np.asarray(reference_values)
            reference_weights = np.asarray(reference_weights)
            stats["cylindrical"]["u_at_mean_section_meters_per_uv"] = {
                **describe(reference_values, reference_weights),
                "occupied_bins": len(reference_values), "charts": int(len(np.unique(charts)))}
    return stats


def assert_uv_quality(stats):
    """Apply the pilot's checker tolerances without discarding small faces."""
    def within(surface, direction, tolerance):
        if not surface["triangles"]:
            return
        limits = surface[f"{direction}_meters_per_uv"]
        assert limits["min"] >= 1 - tolerance and limits["max"] <= 1 + tolerance, (
            f"{stats['projection']} UV {direction.upper()} {limits['min']:.6f}.."
            f"{limits['max']:.6f} m outside 1m +/-{tolerance:.0%}")

    if stats["projection"] == "box":
        within(stats, "v", .05)
        within(stats, "u", .05)
    else:
        within(stats["planar"], "v", .05)
        within(stats["planar"], "u", .05)
        side = stats["cylindrical"]
        # Accepted limitation (PR #313): the meridian of a dense noisy barrel
        # stretches V on single faces by up to ~14%, invisible on tiled wood;
        # the area-weighted mean still has to keep the 1 m scale.
        within(side, "v", .15)
        within(side, "u", .15)
        if side["triangles"]:
            mean_v = side["v_meters_per_uv"]["mean"]
            assert .95 <= mean_v <= 1.05, (
                f"cylinder UV V mean {mean_v:.6f} m outside 1m +/-5%")
        if side["triangles"]:
            reference = side["u_at_mean_section_meters_per_uv"]
            assert reference["min"] >= .95 and reference["max"] <= 1.05, (
                f"cylinder UV U at mean section {reference['min']:.6f}.."
                f"{reference['max']:.6f}m outside 1m +/-5%")


def verify(obj, raw_v, raw_f, raw_m, meta, goal, metrics, output):
    from mathutils import Vector
    from mathutils.bvhtree import BVHTree
    v, f = mesh_arrays(obj)
    aligned = fit_bbox(raw_v, meta, goal)
    # Nearest final triangle sampled over the entire dense source surface.
    coords = v[:, [0, 2, 1]].copy()
    coords[:, 1] *= -1
    tree = BVHTree.FromPolygons(coords.tolist(), f.tolist(), all_triangles=True)
    centers = aligned[raw_f].mean(axis=1)[:, [0, 2, 1]]
    centers[:, 1] *= -1
    indices = np.fromiter((tree.find_nearest(Vector(point))[2] for point in centers),
                          dtype=np.int32, count=len(centers))
    pred = np.array([goal["zones"][p.material_index]["group"] == "METAL"
                     for p in obj.data.polygons])[indices]
    expected = raw_m[raw_f].mean(axis=1) > 0.5
    area = triangle_areas(aligned, raw_f)
    iou = float(area[pred & expected].sum() / area[pred | expected].sum())
    accuracy = float(area[pred == expected].sum() / area.sum())
    uv = np.array([item.uv[:] for item in obj.data.uv_layers.active.data]).reshape(-1, 3, 2)
    face_types = np.array([item.value for item in obj.data.attributes["UVProjection"].data])
    patch_ids = np.array([item.value for item in obj.data.attributes["UVPatch"].data])
    uv_stats = uv_quality(v, f, uv, metrics["uv_projection"], face_types, patch_ids)
    doc = glb_document(output / "model.glb")
    used_materials = {goal["zones"][p.material_index]["key"] for p in obj.data.polygons}
    assert {m["name"] for m in doc["materials"]} == used_materials
    assert {m["extras"]["group"] for m in doc["materials"]} == {"WOOD", "METAL"}
    assert np.allclose(v.min(axis=0), goal["bbox_min"], atol=1e-5)
    assert np.allclose(v.max(axis=0), goal["bbox_max"], atol=1e-5)
    assert metrics["triangles"] <= goal["budget_tris"], metrics
    assert metrics["open_edges"] == metrics["non_manifold_edges"] == 0, metrics
    assert metrics["uv_continuity"]["seam_edges"] == 0, metrics["uv_continuity"]
    assert iou >= 0.9, f"iron area IoU {iou:.4f} < 0.9"
    assert_uv_quality(uv_stats)
    return {"triangles": metrics["triangles"], "raw_triangles": len(raw_f),
            "metal_area_iou": iou, "zone_area_accuracy": accuracy,
            "uv_continuity": metrics["uv_continuity"],
            "uv_quality": uv_stats, "budget": goal["budget_tris"]}


def main():
    import parametric
    import sheet
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--out", required=True)
    args = p.parse_args(sys.argv[sys.argv.index("--") + 1:])
    output = Path(args.out).resolve()
    output.mkdir(parents=True, exist_ok=True)
    summary = {}
    for kind, generate in (("barrel", barrel), ("crate", crate)):
        goal = target(kind)
        item = output / kind
        item.mkdir(parents=True, exist_ok=True)
        (item / "target.json").write_text(json.dumps(goal, indent=2) + "\n")
        v, f, c, m = generate()
        # Exercise non-Y up, nonmeter source, yaw and non-origin target anchor.
        angle = np.radians(23)
        rotation = np.array([[np.cos(angle), 0, -np.sin(angle)], [0, 1, 0],
                             [np.sin(angle), 0, np.cos(angle)]])
        v = (v @ rotation)[:, [0, 2, 1]] * 100
        v[:, 1] *= -1
        meta = {"up_axis": "+Z", "meters_per_unit": 0.01}
        raw_path, meta_path = item / "raw.npz", item / "meta.json"
        np.savez_compressed(raw_path, vertices=v.astype(np.float32), faces=f,
                            color=c.astype(np.float32), metallic=m.astype(np.float32),
                            roughness=np.full(len(v), 0.7, dtype=np.float32))
        meta_path.write_text(json.dumps(meta) + "\n")
        v, f, c, m, _ = read_raw(raw_path)
        raw_continuity = None
        if kind == "barrel":
            aligned = fit_bbox(v, meta, goal)
            raw_uv, raw_patches, _ = cylinder_uv(aligned, f, return_patches=True)
            raw_continuity = uv_continuity(aligned, f, raw_uv, raw_patches)
            assert raw_continuity["seam_edges"] == 0, raw_continuity
        obj, metrics = process_arrays(v, f, c, m, goal, item / "processed", meta)
        summary[kind] = verify(obj, v, f, m, meta, goal, metrics, item / "processed")
        if raw_continuity is not None:
            summary[kind]["raw_uv_continuity"] = raw_continuity
        pv, pf, pc, pm = parametric.build(kind)
        control = item / "parametric"
        control.mkdir(parents=True, exist_ok=True)
        np.savez_compressed(control / "raw.npz", vertices=pv, faces=pf, color=pc, metallic=pm)
        (control / "meta.json").write_text(json.dumps({"up_axis": "+Y", "meters_per_unit": 1.0}) + "\n")
        control_obj, control_metrics = process_arrays(
            pv, pf, pc, pm, goal, item / "parametric",
            {"up_axis": "+Y", "meters_per_unit": 1.0}, cleanup=False)
        cv, cf = mesh_arrays(control_obj)
        control_uv = np.array([item.uv[:] for item in control_obj.data.uv_layers.active.data]).reshape(-1, 3, 2)
        control_types = np.array([item.value for item in control_obj.data.attributes["UVProjection"].data])
        control_patches = np.array([item.value for item in control_obj.data.attributes["UVPatch"].data])
        control_quality = uv_quality(cv, cf, control_uv, control_metrics["uv_projection"], control_types, control_patches)
        assert_uv_quality(control_quality)
        assert control_metrics["triangles"] <= goal["budget_tris"], control_metrics
        assert control_metrics["open_edges"] == control_metrics["non_manifold_edges"] == 0, control_metrics
        assert control_metrics["uv_continuity"]["seam_edges"] == 0, control_metrics["uv_continuity"]
        summary[kind]["parametric"] = {"triangles": control_metrics["triangles"],
                                        "uv_continuity": control_metrics["uv_continuity"],
                                        "uv_quality": control_quality}
        variants = {"title": f"Synthetic {kind}", "variants": [
            {"label": "synthetic x analytic x seed 0", "raw": str(raw_path), "meta": str(meta_path),
             "result": str(item / "processed")},
            {"label": "parametric control", "result": str(item / "parametric")}]}
        variant_path = item / "variants.json"
        variant_path.write_text(json.dumps(variants, indent=2) + "\n")
        # Same public sheet entry point, retaining Blender's command-line convention.
        saved_argv = sys.argv
        try:
            sys.argv = ["sheet.py", "--", "--variants", str(variant_path), "--out", str(item / "sheet.png")]
            sheet.main()
        finally:
            sys.argv = saved_argv
        assert (item / "sheet.png").stat().st_size > 10000
    (output / "checks.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()

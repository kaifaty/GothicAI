"""Clean a TRELLIS.2 raw mesh into a single closed skin and a game-budget mesh, on dev-ubuntu's GPU.

    ~/gothic-ai/mesh3d/.venv-trellis2/bin/python cumesh_clean.py RAW.npz OUT_DIR TARGET_VERTICES [--resolution 512]

The raw marching skin of TRELLIS.2 is two-sided and torn (the armchair: 104k edges of more than two faces, 32k open
ones), so Blender's decimation stalled and a voxel remesh kept only outlines (city-furniture pilot, 2026-11-02).
This repeats the mesh half of TRELLIS.2's own o_voxel.postprocess.to_glb with CuMesh (MIT): fill holes, a
narrow-band dual-contouring remesh, then simplification and cleanup. Its texture half needs nvdiffrast (NVIDIA
non-commercial licence, reports/content/2026-10-3d-generators), so the texture is baked in Blender instead.
Writes OUT_DIR/clean.npz (the remeshed skin, the bake source) and OUT_DIR/low.npz (vertices, faces).
"""
import argparse
from pathlib import Path

import numpy as np
import torch
import cumesh


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("raw", type=Path)
    parser.add_argument("out", type=Path)
    parser.add_argument("target", type=int)
    parser.add_argument("--resolution", type=int, default=512)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    data = np.load(args.raw)
    vertices = torch.tensor(data["vertices"], dtype=torch.float32, device="cuda")
    faces = torch.tensor(data["faces"], dtype=torch.int32, device="cuda")
    mesh = cumesh.CuMesh()
    mesh.init(vertices, faces)
    mesh.fill_holes(max_hole_perimeter=3e-2)
    vertices, faces = mesh.read()
    bvh = cumesh.cuBVH(vertices, faces)
    band = 1
    mesh.init(*cumesh.remeshing.remesh_narrow_band_dc(
        vertices, faces, center=torch.zeros(3, device="cuda"),
        scale=(args.resolution + 3 * band) / args.resolution, resolution=args.resolution, band=band,
        project_back=0.9, verbose=True, bvh=bvh))
    # The remesh of Pixal3D's multi-view armchair left non-manifold edges, and orienting across them read out of
    # bounds (CUDA error 700, 2026-11-02): repair first.
    mesh.remove_duplicate_faces()
    mesh.repair_non_manifold_edges()
    mesh.unify_face_orientations()
    v, f = mesh.read()
    np.savez(args.out / "clean.npz", vertices=v.cpu().numpy(), faces=f.cpu().numpy())
    print(f"clean: {mesh.num_vertices} vertices, {mesh.num_faces} faces")
    mesh.simplify(args.target, verbose=False)
    mesh.remove_duplicate_faces()
    mesh.repair_non_manifold_edges()
    mesh.remove_small_connected_components(1e-5)
    mesh.fill_holes(max_hole_perimeter=3e-2)
    mesh.unify_face_orientations()
    v, f = mesh.read()
    np.savez(args.out / "low.npz", vertices=v.cpu().numpy(), faces=f.cpu().numpy())
    print(f"low: {mesh.num_vertices} vertices, {mesh.num_faces} faces")


if __name__ == "__main__":
    main()

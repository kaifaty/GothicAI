"""Pixal3D raw mesh arrays on dev-ubuntu, in .venv-pixal3d; no rasterizer, GLB or texture baking.

    python generate_pixal3d.py --check
    python generate_pixal3d.py --image RGBA.png --fov 0.69 --output DIR [--seed 1]   # one view
    python generate_pixal3d.py --views DIR --output DIR [--seed 1]                    # posed views, transforms.json

Pixal3D (TencentARC, MIT) is TRELLIS.2 with image features projected to the voxels they come from, and a
multi-view mode for views with known cameras (inference_mv.py: Blender/NeRF transforms, frame 0 the front view with
the camera at (0, -d, 0)). The city-furniture pilot (2026-11-02) tries it on renders of the original model.
Like generate_trellis.py: local weights only, no RMBG-2.0 (CC BY-NC), no nvdiffrast, no MoGe (the camera is
known), and NAF, the feature upsampler, loads from its pinned checkout. Writes OUTPUT/raw.npz in TRELLIS.2's
frame (+Z up, -0.5..0.5, linear vertex colour) and OUTPUT/meta.json.
"""
from __future__ import annotations

import argparse
import gc
import hashlib
import importlib
import importlib.util
import json
import math
import os
from pathlib import Path
import sys
import time
import types

import numpy as np

REVISION = {"repo": "TencentARC/Pixal3D", "revision": "b0cb2e1b794cab9aa0ac38a95d794a4d9337437f",
            "source_revision": "f7cf38429b0bd264f1995f0f8743a88b1c728b94", "license": "MIT"}
# The stage configs of upstream inference.py; each stage has its own projected DINOv3 extractor.
STAGES = {
    "ss": {"image_size": 512, "grid_resolution": 16},
    "shape_512": {"image_size": 512, "grid_resolution": 32, "use_naf_upsample": True, "naf_target_size": 512},
    "shape_1024": {"image_size": 1024, "grid_resolution": 64, "use_naf_upsample": True, "naf_target_size": 512},
    "tex_1024": {"image_size": 1024, "grid_resolution": 64, "use_naf_upsample": True, "naf_target_size": 1024},
}


def setup(root: Path):
    """Import Pixal3D under the same restrictions as TRELLIS (adapter.py) and return its pipeline module."""
    os.environ.update(HF_HUB_DISABLE_IMPLICIT_TOKEN="1", HF_HUB_OFFLINE="1", TRANSFORMERS_OFFLINE="1",
                      ATTN_BACKEND="xformers", SPARSE_ATTN_BACKEND="xformers", FLEX_GEMM_USE_AUTOTUNE_CACHE="0",
                      PYTORCH_CUDA_ALLOC_CONF="expandable_segments:True", TORCH_HOME=str(root / "cache/torch"))
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from adapter import _NoRestrictedImports, _package
    sys.meta_path.insert(0, _NoRestrictedImports())
    repo = root / "repos/Pixal3D"
    sys.path.insert(0, str(repo))
    # o_voxel's package __init__ imports its nvdiffrast postprocess; only its submodules are used.
    voxel = importlib.util.find_spec("o_voxel")
    _package("o_voxel", Path(next(iter(voxel.submodule_search_locations))))
    # pixal3d/__init__ imports the renderers package; load the parts the pipeline needs without it.
    _package("pixal3d", repo / "pixal3d")
    pipelines = importlib.import_module("pixal3d.pipelines")
    pipelines.rembg = types.SimpleNamespace()  # an ordinary object, never the BiRefNet/RMBG module
    return importlib.import_module("pixal3d.pipelines.pixal3d_mv_image_to_3d")


def load_pipeline(root: Path, multiview: bool):
    import torch
    module = setup(root)
    proj = importlib.import_module("pixal3d.trainers.flow_matching.mixins.image_conditioned_proj")
    base = importlib.import_module("pixal3d.pipelines.base").Pipeline
    samplers = importlib.import_module("pixal3d.pipelines.samplers")
    cls = module.Pixal3DMVImageTo3DPipeline if multiview else module.Pixal3DImageTo3DPipeline
    extractor = proj.DinoV3ProjMultiViewFeatureExtractor if multiview else proj.DinoV3ProjFeatureExtractor
    naf_repo, naf_weights = root / "repos/NAF", root / "models/naf/naf_release.pth"
    shared = {}

    def load_naf(self):  # upstream: torch.hub.load("valeoai/NAF", ...) from GitHub at run time
        if self.naf_model is None:
            if "naf" not in shared:
                sys.path.insert(0, str(naf_repo))
                model = torch.hub.load(str(naf_repo), "naf", source="local", pretrained=False, device="cpu")
                model.load_state_dict(torch.load(naf_weights, map_location="cpu", weights_only=True))
                shared["naf"] = model.eval().requires_grad_(False)
            self.naf_model = shared["naf"]

    extractor._load_naf = load_naf
    # Each stage builds its own extractor around the same frozen DINOv3 and NAF; one copy of each serves all four
    # (3.6 GB of RAM less: run-mesh3d.sh holds generation to 20 GiB).
    extractors = {}
    for stage, config in STAGES.items():
        config = dict(config, model_name=str(root / "models/dinov3"))
        if multiview:
            config["multiview_fusion"] = "average"
        model = extractor(**config).eval()
        if "dinov3" in shared:
            model.model = shared["dinov3"]
        shared.setdefault("dinov3", model.model)
        gc.collect()
        if model.use_naf_upsample:
            model._load_naf()
        extractors[stage] = model
    weights = root / "models/pixal3d"
    config_file = "pipeline_mv.json" if multiview else "pipeline.json"
    pipeline = base.from_pretrained.__func__(cls, str(weights), config_file)  # upstream's loader minus RMBG
    args = pipeline._pretrained_args
    if "rembg_model" in args:
        raise ValueError("The local Pixal3D config must omit rembg_model")
    for name in ("sparse_structure", "shape_slat", "tex_slat"):
        spec = args[f"{name}_sampler"]
        setattr(pipeline, f"{name}_sampler", getattr(samplers, spec["name"])(**spec["args"]))
        setattr(pipeline, f"{name}_sampler_params", spec["params"])
    pipeline.shape_slat_normalization = args["shape_slat_normalization"]
    pipeline.tex_slat_normalization = args["tex_slat_normalization"]
    pipeline.rembg_model = None
    pipeline.pbr_attr_layout = {"base_color": slice(0, 3), "metallic": slice(3, 4),
                                "roughness": slice(4, 5), "alpha": slice(5, 6)}
    for stage, model in extractors.items():
        setattr(pipeline, f"image_cond_model_{stage}", model)
    # Low-VRAM mode: one flow model and one extractor on the GPU at a time (upstream: ~10-12 GB instead of ~18).
    pipeline._device = torch.device("cuda")
    pipeline.low_vram = True
    # Low-VRAM mode moves each flow model back to RAM after its stage, but one run uses each once: keeping all four
    # 1.3B models there took the first multi-view run past run-mesh3d.sh's 20 GiB at the decoders (2026-11-02).
    for name, model in pipeline.models.items():
        if "flow" in name:
            model.cpu = types.MethodType(lambda self, name=name: drop(self, name), model)
    trim_memory("loaded")
    from adapter import assert_clean_imports
    assert_clean_imports()
    return pipeline


def trim_memory(stage: str) -> None:
    """Hand freed heap back to the system (the fp32 checkpoints pass through RAM) and log what is left."""
    import ctypes
    gc.collect()
    ctypes.CDLL("libc.so.6").malloc_trim(0)
    rss = next(line for line in Path("/proc/self/status").read_text().splitlines() if line.startswith("VmRSS"))
    print(f"[memory] {stage}: {int(rss.split()[1]) / 2**20:.1f} GiB resident", flush=True)


def drop(model, name: str):
    model.to("meta")
    trim_memory(f"{name} done")
    return model


def single_camera(fov: float, mesh_scale: float = 1.0, resolution: int = 512) -> dict:
    """upstream inference.py distance_from_fov for a manual fov: the camera that fits the unit cube in frame."""
    import torch
    rotation = torch.tensor([[1.0, 0.0, 0.0], [0.0, 0.0, -1.0], [0.0, 1.0, 0.0]])
    point = (torch.tensor([-1.0, 0.0, 0.0]) @ rotation.T) / mesh_scale / 2
    focal = 16.0 / math.tan(fov / 2) * resolution / 32.0
    x = 0 - resolution / 2.0
    distance = focal * float(point[0]) / x - float(point[1])
    return {"camera_angle_x": fov, "distance": distance, "mesh_scale": mesh_scale}


def load_views(root: Path, views: Path) -> dict:
    """The bundle run_mv wants, read by upstream inference_mv.py's own loader (RGBA views only)."""
    spec = importlib.util.spec_from_file_location("pixal3d_inference_mv", root / "repos/Pixal3D/inference_mv.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    bundle = module.load_views(str(views), rembg=None)
    module.check_main_view(bundle)
    return bundle


def save(mesh, res: int, output: Path) -> dict:
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from generate_trellis import sample_voxel_attributes, save_raw, srgb_to_linear

    def numpy(tensor):
        return tensor.detach().float().cpu().numpy() if hasattr(tensor, "detach") else np.asarray(tensor)
    vertices, faces = numpy(mesh.vertices), numpy(mesh.faces).astype(np.int64)
    origin = numpy(getattr(mesh, "origin", np.full(3, -0.5)))
    voxel_size = float(getattr(mesh, "voxel_size", 1.0 / res))
    attrs = sample_voxel_attributes(vertices, numpy(mesh.coords), numpy(mesh.attrs), origin, voxel_size)
    return save_raw(output / "raw.npz", vertices, faces, srgb_to_linear(attrs[:, :3]),
                    metallic=np.clip(attrs[:, 3], 0, 1), roughness=np.clip(attrs[:, 4], 0, 1))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", type=Path, default=Path.home() / "gothic-ai/mesh3d")
    parser.add_argument("--check", action="store_true", help="import and load on the CPU only, no generation")
    source = parser.add_mutually_exclusive_group()
    source.add_argument("--image", type=Path)
    source.add_argument("--views", type=Path)
    parser.add_argument("--fov", type=float, default=0.3490658503988659, help="horizontal fov of --image, radians")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--resolution", type=int, default=1024, choices=(1024, 1536))
    args = parser.parse_args()
    root = args.root.expanduser().resolve()
    if args.check:
        module = setup(root)
        importlib.import_module("pixal3d.trainers.flow_matching.mixins.image_conditioned_proj")
        assert module.Pixal3DImageTo3DPipeline and module.Pixal3DMVImageTo3DPipeline
        from adapter import assert_clean_imports
        print("imports ok", assert_clean_imports(), flush=True)
        return 0
    if not args.output or not (args.image or args.views):
        parser.error("--output and one of --image/--views are required")
    import torch
    from PIL import Image
    args.output.mkdir(parents=True, exist_ok=True)
    started = time.time()
    pipeline = load_pipeline(root, multiview=bool(args.views))
    torch.manual_seed(args.seed)
    common = dict(seed=args.seed, return_latent=True, pipeline_type=f"{args.resolution}_cascade", max_num_tokens=49152)
    if args.views:
        meshes, (_, _, res) = pipeline.run_mv(load_views(root, args.views), **common)
        source_hash = hashlib.sha256((args.views / "transforms.json").read_bytes()).hexdigest()
    else:
        image = pipeline.preprocess_image(Image.open(args.image))
        meshes, (_, _, res) = pipeline.run(image, camera_params=single_camera(args.fov), preprocess_image=False, **common)
        source_hash = hashlib.sha256(args.image.read_bytes()).hexdigest()
    save(meshes[0], res, args.output)
    meta = {"model": "pixal3d", "models": [REVISION], "mode": "multiview" if args.views else "single",
            "input": str(args.views or args.image), "input_sha256": source_hash, "seed": args.seed,
            "fov": None if args.views else args.fov, "pipeline_type": common["pipeline_type"], "grid": res,
            "axes": {"up": "+Z", "front": "-Y (the camera of frame 0)"}, "scale": {"canonical_bounds": [-0.5, 0.5]},
            "color_space": "linear RGB", "seconds": round(time.time() - started, 1),
            "peak_vram_gb": round(torch.cuda.max_memory_allocated() / 2**30, 2), "torch_version": torch.__version__}
    (args.output / "meta.json").write_text(json.dumps(meta, indent=2) + "\n")
    print(json.dumps({k: meta[k] for k in ("mode", "grid", "seconds", "peak_vram_gb")}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

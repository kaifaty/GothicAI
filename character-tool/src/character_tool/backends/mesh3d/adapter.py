"""Mesh-only, local-weight TRELLIS imports; upstream checkouts stay unchanged."""
from __future__ import annotations

import ast
import importlib
import importlib.abc
import importlib.metadata
import importlib.util
import json
import os
from pathlib import Path
import runpy
import sys
import types


FORBIDDEN = (
    "nvdiffrast", "nvdiffrec", "diff_gaussian_rasterization", "diffoctreerast",
    "mip_splatting", "rembg", "rmbg", "birefnet",
)


def _forbidden(name: str) -> bool:
    normalized = name.lower().replace("-", "_")
    return any(part.startswith(prefix) for part in normalized.split(".") for prefix in FORBIDDEN)


class _NoRestrictedImports(importlib.abc.MetaPathFinder):
    def find_spec(self, fullname, path=None, target=None):
        if _forbidden(fullname):
            raise ImportError(f"Restricted mesh3d dependency: {fullname}")
        return None


def assert_clean_imports() -> dict:
    """Check installed distributions as well as code actually imported."""
    installed = sorted(d.metadata["Name"] for d in importlib.metadata.distributions())
    forbidden_packages = [name for name in installed if _forbidden(name)]
    forbidden_modules = [name for name in sys.modules if _forbidden(name)]
    if forbidden_packages or forbidden_modules:
        raise RuntimeError(f"Restricted packages={forbidden_packages}; modules={forbidden_modules}")
    return {"installed_distributions": len(installed), "forbidden_packages": [], "forbidden_modules": []}


def _package(name: str, path: Path):
    """Skip eager package exports for unused renderers and representations."""
    module = types.ModuleType(name)
    module.__path__ = [str(path)]
    module.__package__ = name
    module.__spec__ = importlib.util.spec_from_loader(name, loader=None, is_package=True)
    module.__spec__.submodule_search_locations = [str(path)]
    sys.modules[name] = module
    return module


def _load_flexicubes(source: Path):
    # Upstream uses Kaolin only for this shape assertion. Loading its entire
    # CUDA package is unnecessary; keep the same check without modifying files.
    name = "trellis.representations.mesh.flexicubes.flexicubes"
    filename = source / "representations/mesh/flexicubes/flexicubes.py"
    _package("trellis.representations.mesh", source / "representations/mesh")
    _package("trellis.representations.mesh.flexicubes", filename.parent)
    tree = ast.parse(filename.read_text(), filename=str(filename))
    imports = [node for node in tree.body if isinstance(node, ast.ImportFrom)
               and node.module == "kaolin.utils.testing"]
    if len(imports) != 1 or [a.name for a in imports[0].names] != ["check_tensor"]:
        raise RuntimeError("Pinned FlexiCubes Kaolin import changed")
    tree.body.remove(imports[0])
    module = types.ModuleType(name)
    module.__file__ = str(filename)
    module.__package__ = name.rsplit(".", 1)[0]

    def check_tensor(tensor, shape, throw=False):
        import torch
        valid = isinstance(tensor, torch.Tensor) and tensor.ndim == len(shape)
        valid = valid and all(n is None or actual == n for actual, n in zip(tensor.shape, shape))
        if throw and not valid:
            raise ValueError(f"Expected tensor shape {shape}")
        return valid

    module.check_tensor = check_tensor
    sys.modules[name] = module
    exec(compile(tree, str(filename), "exec"), module.__dict__)
    mesh = sys.modules["trellis.representations.mesh"]
    cube2mesh = importlib.import_module("trellis.representations.mesh.cube2mesh")
    mesh.MeshExtractResult = cube2mesh.MeshExtractResult
    mesh.SparseFeatures2Mesh = cube2mesh.SparseFeatures2Mesh
    return mesh


def pipeline_class(root: Path, model: str):
    """Import required decoder classes without loading weights or running inference."""
    os.environ["HF_HUB_DISABLE_IMPLICIT_TOKEN"] = "1"
    os.environ["HF_HUB_OFFLINE"] = "1"
    os.environ["TRANSFORMERS_OFFLINE"] = "1"
    os.environ["ATTN_BACKEND"] = "xformers"
    os.environ["SPARSE_ATTN_BACKEND"] = "xformers"
    os.environ["FLEX_GEMM_USE_AUTOTUNE_CACHE"] = "0"
    os.environ["TORCH_HOME"] = str(root / "cache/torch")
    sys.meta_path.insert(0, _NoRestrictedImports())
    package = "trellis2" if model == "trellis2" else "trellis"
    repo = root / "repos" / ("TRELLIS.2" if model == "trellis2" else "TRELLIS")
    source = repo / package
    _package(package, source)
    pipelines = (importlib.import_module("trellis2.pipelines") if model == "trellis2"
                 else _package("trellis.pipelines", source / "pipelines"))
    if model == "trellis2":
        # Its package __init__ eagerly imports unrelated nvdiffrast postprocess.
        voxel_spec = importlib.util.find_spec("o_voxel")
        if voxel_spec is None or not voxel_spec.submodule_search_locations:
            raise ImportError("o_voxel extension is not installed")
        _package("o_voxel", Path(next(iter(voxel_spec.submodule_search_locations))))
        # The image pipeline imports this package attribute even for RGBA input.
        # It is deliberately an ordinary object, never a rembg sys.modules entry.
        pipelines.rembg = types.SimpleNamespace()
        cls = importlib.import_module(f"{package}.pipelines.trellis2_image_to_3d").Trellis2ImageTo3DPipeline
    else:
        representations = _package("trellis.representations", source / "representations")
        mesh = _load_flexicubes(source)
        representations.MeshExtractResult = mesh.MeshExtractResult
        # __init__ also exports Gaussian/RF decoders; only mesh is needed here.
        vae = _package("trellis.models.structured_latent_vae", source / "models/structured_latent_vae")
        decoder = importlib.import_module("trellis.models.structured_latent_vae.decoder_mesh")
        vae.SLatMeshDecoder = decoder.SLatMeshDecoder
        vae.ElasticSLatMeshDecoder = decoder.ElasticSLatMeshDecoder
        module_name = "trellis.pipelines.trellis_image_to_3d"
        filename = source / "pipelines/trellis_image_to_3d.py"
        tree = ast.parse(filename.read_text(), filename=str(filename))
        # Remove exactly the unused unconditional import, only in memory.
        imports = [node for node in tree.body if isinstance(node, ast.Import)
                   and [alias.name for alias in node.names] == ["rembg"]]
        if len(imports) != 1:
            raise RuntimeError("Pinned TRELLIS rembg import changed")
        tree.body.remove(imports[0])
        module = types.ModuleType(module_name)
        module.__file__ = str(filename)
        module.__package__ = "trellis.pipelines"
        sys.modules[module_name] = module
        exec(compile(tree, str(filename), "exec"), module.__dict__)
        cls = module.TrellisImageTo3DPipeline
    # Exercise lazy model imports too, without constructing a CUDA mesh extractor.
    model_lib = importlib.import_module(f"{package}.models")
    weights = root / "models" / ("trellis2" if model == "trellis2" else "trellis-v1")
    config = json.loads((weights / "pipeline.json").read_text())["args"]
    for checkpoint in config["models"].values():
        checkpoint = Path(checkpoint)
        if not checkpoint.is_absolute():
            checkpoint = weights / checkpoint
        model_config = json.loads(checkpoint.with_suffix(".json").read_text())
        getattr(model_lib, model_config["name"])
    if model == "trellis":
        sys.path.insert(0, str(root / "repos/dinov2"))
        hub = runpy.run_path(str(root / "repos/dinov2/hubconf.py"))
        if not callable(hub[config["image_cond_model"]]):
            raise RuntimeError("Pinned DINOv2 torch-hub entry is unavailable")
    assert_clean_imports()
    return cls


def load_pipeline(root: Path, model: str, pipeline_type: str | None = None):
    """Load only local checkpoints; missing weights fail instead of downloading."""
    import torch
    cls = pipeline_class(root, model)
    package = "trellis2" if model == "trellis2" else "trellis"
    weights = root / "models" / ("trellis2" if model == "trellis2" else "trellis-v1")
    config = json.loads((weights / "pipeline.json").read_text())["args"]
    model_lib = importlib.import_module(f"{package}.models")
    models = {}
    for name, checkpoint in config["models"].items():
        if model == "trellis2":
            if pipeline_type == "512" and name.endswith("_1024"):
                continue
            if pipeline_type == "1024_cascade" and name == "tex_slat_flow_model_512":
                continue
        if model == "trellis" and name in ("slat_decoder_gs", "slat_decoder_rf"):
            raise ValueError("Local TRELLIS pipeline must contain only the mesh decoder")
        checkpoint = Path(checkpoint)
        if not checkpoint.is_absolute():
            checkpoint = weights / checkpoint
        for suffix in (".json", ".safetensors"):
            if not checkpoint.with_suffix(suffix).is_file():
                raise FileNotFoundError(checkpoint.with_suffix(suffix))
        models[name] = model_lib.from_pretrained(str(checkpoint))
        models[name].eval()
    pipeline = cls()
    pipeline.models = models
    samplers = importlib.import_module(f"{package}.pipelines.samplers")
    names = ("sparse_structure", "shape_slat", "tex_slat") if model == "trellis2" else ("sparse_structure", "slat")
    for name in names:
        spec = config[f"{name}_sampler"]
        setattr(pipeline, f"{name}_sampler", getattr(samplers, spec["name"])(**spec["args"]))
        setattr(pipeline, f"{name}_sampler_params", spec["params"])
    if model == "trellis2":
        if "rembg_model" in config:
            raise ValueError("Local TRELLIS.2 pipeline must omit rembg_model")
        features = importlib.import_module("trellis2.modules.image_feature_extractor")
        spec = config["image_cond_model"]
        image_args = dict(spec["args"], model_name=str(root / "models/dinov3"))
        pipeline.image_cond_model = getattr(features, spec["name"])(**image_args)
        pipeline.shape_slat_normalization = config["shape_slat_normalization"]
        pipeline.tex_slat_normalization = config["tex_slat_normalization"]
        pipeline.rembg_model = None
        pipeline.low_vram = True
        pipeline.default_pipeline_type = "1024_cascade"
        pipeline.pbr_attr_layout = {"base_color": slice(0, 3), "metallic": slice(3, 4),
                                   "roughness": slice(4, 5), "alpha": slice(5, 6)}
        pipeline._device = "cpu"
    else:
        from torchvision import transforms
        dinov2 = torch.hub.load(str(root / "repos/dinov2"), config["image_cond_model"],
                               source="local", pretrained=False)
        checkpoint = root / "models/dinov2/dinov2_vitl14_reg4_pretrain.pth"
        dinov2.load_state_dict(torch.load(checkpoint, map_location="cpu", weights_only=True))
        dinov2.eval()
        pipeline.models["image_cond_model"] = dinov2
        pipeline.image_cond_model_transform = transforms.Compose([
            transforms.Normalize(mean=[0.485, 0.456, 0.406], std=[0.229, 0.224, 0.225])])
        pipeline.slat_normalization = config["slat_normalization"]
        pipeline.rembg_session = None
    assert_clean_imports()
    return pipeline

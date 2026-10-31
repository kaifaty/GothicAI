"""Text-only adapter for the pinned, native StableMaterials 512 px pipeline.

The caller downloads the snapshot on dev-ubuntu. Loading is deliberately local:
neither weights nor Python code can be fetched by this module. The upstream
pipeline's ``tileable=True`` rolled convolutions and padded VAE decode are kept.
"""

from __future__ import annotations

import hashlib
import importlib.util
from pathlib import Path
import sys
import threading
import types

import numpy as np


HF_MODEL = "gvecchio/StableMaterials"
HF_REVISION = "4d452731535bd5c74261d6645623a573326f6f36"
PIPELINE_SHA256 = "0e0a5cd721d4abadd2ceb2e46fc3ee876b93e720fb63caf8bb74784ede5a55be"
GUIDANCE_SCALE = 10.0
_GENERATION_LOCK = threading.Lock()


def _pipeline_class(model_path: Path):
    """Import exactly the custom code reviewed at HF_REVISION."""
    source = model_path / "pipeline.py"
    if hashlib.sha256(source.read_bytes()).hexdigest() != PIPELINE_SHA256:
        raise ValueError(
            f"StableMaterials pipeline.py differs from pinned revision {HF_REVISION}"
        )
    name = f"_gothic_stablematerials_{HF_REVISION}"
    if name not in sys.modules:
        spec = importlib.util.spec_from_file_location(name, source)
        if spec is None or spec.loader is None:
            raise ImportError(f"Cannot load StableMaterials pipeline from {source}")
        module = importlib.util.module_from_spec(spec)
        sys.modules[name] = module
        try:
            spec.loader.exec_module(module)
        except BaseException:
            del sys.modules[name]
            raise
    return sys.modules[name].StableMaterialsPipeline


def load_pipeline(model_path: str):
    """Load the pinned snapshot on CUDA, without image conditioning components.

    Requires torch with matching torchvision, diffusers 0.35.2, transformers
    4.x, accelerate, safetensors, Pillow and numpy. The VAE stays float32 (its
    config requests ``force_upcast``); the UNet and text encoder use float16.
    """
    import torch
    from diffusers import AutoencoderKL, DDIMScheduler, UNet2DConditionModel
    from transformers import CLIPTextModelWithProjection, CLIPTokenizerFast

    if not torch.cuda.is_available():
        raise RuntimeError("StableMaterials generation requires CUDA on dev-ubuntu")
    path = Path(model_path).expanduser().resolve(strict=True)
    pipeline_class = _pipeline_class(path)
    local = {"local_files_only": True, "token": False}
    vae = AutoencoderKL.from_pretrained(
        path, subfolder="vae", torch_dtype=torch.float32, use_safetensors=True, **local
    )
    # The upstream 0.27-era pipeline forwards UNet latents directly to its VAE.
    # Cast at that boundary so force_upcast also works with modern diffusers.
    original_decode = vae.decode

    def decode(self, z, *args, **kwargs):
        return original_decode(z.to(dtype=self.dtype), *args, **kwargs)

    vae.decode = types.MethodType(decode, vae)
    pipe = pipeline_class(
        vae=vae,
        unet=UNet2DConditionModel.from_pretrained(
            path, subfolder="unet", torch_dtype=torch.float16, use_safetensors=True, **local
        ),
        scheduler=DDIMScheduler.from_pretrained(path, subfolder="scheduler", **local),
        text_encoder=CLIPTextModelWithProjection.from_pretrained(
            path, subfolder="text_encoder", torch_dtype=torch.float16, use_safetensors=True, **local
        ),
        tokenizer=CLIPTokenizerFast.from_pretrained(path, subfolder="tokenizer", **local),
        # Both prompts below must be strings. The custom pipeline therefore
        # never needs its optional image-conditioning encoder or processor.
        vision_encoder=None,
        processor=None,
    )
    return pipe.to("cuda")


def _array(value, *, channels: int) -> np.ndarray:
    """Convert native CHW torch maps to finite float32 HWC/HW maps."""
    array = value.detach().float().cpu().numpy()
    if channels == 1:
        array = array[0]
    else:
        array = np.moveaxis(array, 0, -1)
    if not np.isfinite(array).all():
        raise RuntimeError("StableMaterials produced a non-finite map")
    return np.ascontiguousarray(np.clip(array, 0.0, 1.0), dtype=np.float32)


def generate(pipe, prompt: str, negative: str, seed: int, steps: int = 50):
    """Return native albedo/normal/height/roughness/metallic maps, all in [0, 1].

    Normal is the upstream unit XYZ normal encoded as RGB in [0, 1]. Heights
    retain the model's native relative scale. No input pixels, upscaling, seam
    retouching or additional model is used.
    """
    import torch

    if not isinstance(prompt, str) or not isinstance(negative, str):
        raise TypeError("StableMaterials accepts only text prompt and negative strings")
    if not prompt.strip() or steps < 1:
        raise ValueError("A non-empty prompt and positive inference steps are required")
    limit = pipe.text_encoder.config.max_position_embeddings
    for label, text in (("prompt", prompt), ("negative", negative)):
        count = len(pipe.tokenizer(text)["input_ids"])
        if count > limit:
            raise ValueError(f"StableMaterials {label} has {count} tokens; CLIP limit is {limit}")
    device = torch.device(pipe._execution_device)
    if device.type != "cuda":
        raise RuntimeError("StableMaterials generation requires CUDA on dev-ubuntu")
    device_index = device.index if device.index is not None else torch.cuda.current_device()
    # Upstream feature rolling draws from the global CPU RNG rather than the
    # generator argument. Seed and restore both RNGs to make its tiles repeatable.
    # Upstream rolled_conv also patches a global method; serialize and restore
    # it explicitly if decoding/denoising raises (e.g. an out-of-memory error).
    with _GENERATION_LOCK, torch.random.fork_rng(devices=[device_index]):
        torch.manual_seed(int(seed))
        generator = torch.Generator(device=device).manual_seed(int(seed))
        original_conv_forward = torch.nn.Conv2d.forward
        try:
            material = pipe(
                prompt=prompt,
                negative_prompt=negative,
                generator=generator,
                num_inference_steps=int(steps),
                num_images_per_prompt=1,
                height=512,
                width=512,
                tileable=True,
                patched=False,
                guidance_scale=GUIDANCE_SCALE,
                eta=0.0,
                output_type="pt",
            ).images[0]
        finally:
            torch.nn.Conv2d.forward = original_conv_forward
    return {
        "albedo": _array(material.basecolor, channels=3),
        "normal": _array(material.normal, channels=3),
        "height": _array(material.height, channels=1),
        "roughness": _array(material.roughness, channels=1),
        "metallic": _array(material.metallic, channels=1),
    }

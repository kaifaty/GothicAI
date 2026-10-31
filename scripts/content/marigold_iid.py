#!/usr/bin/env python3
"""Remove estimated lighting from our PNGs on dev-ubuntu, never on a Mac.

Install the pinned dependencies from requirements.txt into a dedicated venv.
Download prs-eth/marigold-iid-lighting-v1-1 at MODEL_REVISION, including fp16
weights, then pass that local snapshot as --model-path. No originals, prompts,
registry writes, service changes or downloads are performed by this script.
The model predicts linear RGB: output albedo PNGs are encoded as sRGB.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import platform
import sys
import time


MODEL_ID = "prs-eth/marigold-iid-lighting-v1-1"
MODEL_REVISION = "08c3930bb641abf786ba44ce92547507ebefbc16"
MODEL_LICENSE = "CreativeML Open RAIL++-M"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("images", nargs="+", type=Path, help="Our generated PNGs only")
    parser.add_argument("--model-path", required=True, type=Path, help="Pinned local HF snapshot")
    parser.add_argument("--output-dir", required=True, type=Path, help="Outside content staging")
    parser.add_argument("--steps", type=int, default=4)
    parser.add_argument("--ensemble", type=int, default=1)
    parser.add_argument("--resolution", type=int, default=768, help="Processing long edge")
    parser.add_argument("--seed", type=int, default=20261020)
    args = parser.parse_args(argv)
    if platform.system() != "Linux":
        parser.error("GPU models run on dev-ubuntu, never on the Mac")
    if args.steps < 1 or args.ensemble < 1 or args.resolution < 8 or args.resolution % 8:
        parser.error("steps/ensemble must be positive; resolution must be a positive multiple of 8")
    if args.model_path.resolve().name != MODEL_REVISION:
        parser.error(f"Expected pinned model snapshot {MODEL_REVISION}")
    names = [path.stem for path in args.images]
    if len(set(names)) != len(names):
        parser.error("Input PNG names must be unique (copy them to descriptive names first)")
    if any(path.suffix.lower() != ".png" or not path.is_file() for path in args.images):
        parser.error("Every input must be an existing PNG")
    outputs = [args.output_dir / f"{name}-albedo.png" for name in names]
    if any(path.exists() for path in outputs):
        parser.error("An output already exists; use a fresh output directory")
    if any(output.resolve() == source.resolve() for output in outputs for source in args.images):
        parser.error("Output would overwrite an input")

    import numpy as np
    from PIL import Image
    import torch
    import diffusers

    if not torch.cuda.is_available():
        parser.error("A CUDA GPU on dev-ubuntu is required")
    free_bytes, total_bytes = torch.cuda.mem_get_info()
    if free_bytes < 8 * 1024**3:
        parser.error(f"Only {free_bytes / 1024**2:.0f} MiB GPU memory free; need 8192 MiB")
    started = time.monotonic()
    pipe = diffusers.MarigoldIntrinsicsPipeline.from_pretrained(
        str(args.model_path), variant="fp16", torch_dtype=torch.float16,
        local_files_only=True,
    ).to("cuda")
    pipe.vae.enable_slicing()
    pipe.set_progress_bar_config(disable=True)
    target_names = list(pipe.target_properties["target_names"])
    if "albedo" not in target_names or pipe.target_properties["albedo"]["prediction_space"] != "linear":
        raise ValueError("Expected lighting IID model with linear RGB albedo")
    args.output_dir.mkdir(parents=True, exist_ok=True)
    print(json.dumps({
        "model": MODEL_ID, "revision": MODEL_REVISION, "license": MODEL_LICENSE,
        "torch": torch.__version__, "diffusers": diffusers.__version__,
        "gpu": torch.cuda.get_device_name(), "free_before_mib": round(free_bytes / 1024**2, 1),
        "total_mib": round(total_bytes / 1024**2, 1),
        "load_seconds": round(time.monotonic() - started, 3),
        "steps": args.steps, "ensemble_size": args.ensemble,
        "processing_resolution": args.resolution, "match_input_resolution": True,
        "scheduler": type(pipe.scheduler).__name__, "seed": args.seed,
    }), flush=True)
    for source, output in zip(args.images, outputs):
        image_started = time.monotonic()
        torch.cuda.reset_peak_memory_stats()
        with Image.open(source) as image:
            rgb = image.convert("RGB")
            alpha = image.getchannel("A") if "A" in image.getbands() else None
        with torch.inference_mode():
            prediction = pipe(
                rgb, num_inference_steps=args.steps, ensemble_size=args.ensemble,
                processing_resolution=args.resolution, match_input_resolution=True,
                generator=torch.Generator(device="cuda").manual_seed(args.seed),
                output_type="np",
            ).prediction
        albedo = np.clip(prediction[target_names.index("albedo")], 0, 1)
        if not np.isfinite(albedo).all():
            raise ValueError(f"Non-finite model output for {source.name}")
        # The lighting model returns linear RGB; storing it directly as PNG
        # would cause consumers to decode gamma twice and make it too dark.
        encoded = np.where(
            albedo <= 0.0031308, 12.92 * albedo,
            1.055 * np.power(albedo, 1 / 2.4) - 0.055,
        )
        result = Image.fromarray(np.rint(np.clip(encoded, 0, 1) * 255).astype(np.uint8))
        if alpha is not None:
            result.putalpha(alpha)
        result.save(output)
        torch.cuda.synchronize()
        print(json.dumps({
            "input": str(source), "output": str(output), "size": list(rgb.size),
            "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
            "output_sha256": hashlib.sha256(output.read_bytes()).hexdigest(),
            "seconds": round(time.monotonic() - image_started, 3),
            "peak_allocated_mib": round(torch.cuda.max_memory_allocated() / 1024**2, 1),
            "peak_reserved_mib": round(torch.cuda.max_memory_reserved() / 1024**2, 1),
            "linear_mean_rgb": [float(value) for value in albedo.mean(axis=(0, 1))],
        }), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())

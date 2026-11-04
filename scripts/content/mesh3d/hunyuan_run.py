"""Shapes for the Hunyuan pilot (reports/content/2026-11-ai-3d-pipeline/hunyuan-pilot.md), on dev-ubuntu.

cd ~/gothic-ai/parts && hy-venv/bin/python hunyuan_run.py --model 21 --images DIR --out OUT
                        hy-venv/bin/python hunyuan_run.py --model omni --images DIR --control point|bbox --cond DIR --out OUT

21: Hunyuan3D-2.1's shape model on each RGBA picture of DIR. omni: Hunyuan3D-Omni with the same picture and DIR/<name>.ply
(the original's surface points, already turned into the frame 2.1 builds that picture in, parts_pilot-style) or
DIR/<name>.json {"bbox": [x, y, z]} (its extents, the largest 1). Writes OUT/<name>.glb and OUT/stats.json (seconds,
peak GiB). Each part of a model is on the GPU only while it runs (staged()). The pictures carry their alpha, so no background remover is loaded (BRIA's RMBG is not to be used).
Both models ship a package named hy3dshape, so each run is its own process.
"""
import argparse
import json
import os
import sys
import time
from pathlib import Path

import numpy as np
import torch
import trimesh

ROOT = Path.home() / 'gothic-ai/parts'

ap = argparse.ArgumentParser()
ap.add_argument('--model', choices=['21', 'omni'], required=True)
ap.add_argument('--images', type=Path, required=True)
ap.add_argument('--out', type=Path, required=True)
ap.add_argument('--control', choices=['point', 'bbox'])
ap.add_argument('--cond', type=Path)
ap.add_argument('--seed', type=int, default=1234)
a = ap.parse_args()
a.out.mkdir(parents=True, exist_ok=True)

if a.model == '21':
    sys.path.insert(0, str(ROOT / 'Hunyuan3D-2.1/hy3dshape'))
    from hy3dshape.pipelines import Hunyuan3DDiTFlowMatchingPipeline
    from hy3dshape.postprocessors import FloaterRemover, DegenerateFaceRemover
    pipe = Hunyuan3DDiTFlowMatchingPipeline.from_pretrained('tencent/Hunyuan3D-2.1')
else:
    sys.path.insert(0, str(ROOT / 'Hunyuan3D-Omni'))
    from hy3dshape.pipelines import Hunyuan3DOmniSiTFlowMatchingPipeline
    from hy3dshape.postprocessors import FloaterRemover, DegenerateFaceRemover
    pipe = Hunyuan3DOmniSiTFlowMatchingPipeline.from_pretrained('tencent/Hunyuan3D-Omni', fast_decode=False)



def staged(pipe):
    """8 GB of the 3090 is free beside the services and the 3.3B DiT alone takes 6.6 GB in fp16: the image encoder
    is on the GPU only while it encodes, the VAE only while it turns the latents into a mesh (the DiT waits on the CPU
    then). The pipeline's own enable_model_cpu_offload expects diffusers components it does not have. 2.1 decodes in
    _export, Omni calls vae.decode inside __call__; its encoder is cond_encoder."""
    encoder = getattr(pipe, 'conditioner', None) or pipe.cond_encoder

    def swap(module_in, module_out):
        module_out.to('cpu')
        torch.cuda.empty_cache()
        module_in.to('cuda')

    encode = pipe.encode_cond

    def encode_cond(*args, **kwargs):
        swap(encoder, pipe.vae)
        try:
            return encode(*args, **kwargs)
        finally:
            encoder.to('cpu')
            torch.cuda.empty_cache()

    def wrap_decode(owner, name):
        decode = getattr(owner, name)

        def staged_decode(*args, **kwargs):
            swap(pipe.vae, pipe.model)
            try:
                return decode(*args, **kwargs)
            finally:
                swap(pipe.model, pipe.vae)
        setattr(owner, name, staged_decode)

    encoder.to('cpu')
    pipe.vae.to('cpu')
    torch.cuda.empty_cache()
    pipe.encode_cond = encode_cond
    wrap_decode(pipe, '_export') if hasattr(pipe, '_export') else wrap_decode(pipe.vae, 'decode')


staged(pipe)
stats = {}
for image in sorted(a.images.glob('*.png')):
    name = image.stem
    kwargs = {}
    if a.model == 'omni':
        if a.control == 'point':
            points = trimesh.load(a.cond / f'{name}.ply').vertices
            kwargs['point'] = torch.FloatTensor(np.asarray(points)).unsqueeze(0).to(pipe.device).to(pipe.dtype)
        else:
            box = json.loads((a.cond / f'{name}.json').read_text())['bbox']
            kwargs['bbox'] = torch.FloatTensor(box).unsqueeze(0).unsqueeze(0).to(pipe.device).to(pipe.dtype)
    torch.cuda.reset_peak_memory_stats()
    start = time.time()
    result = pipe(image=str(image), num_inference_steps=50, octree_resolution=512, mc_level=0, guidance_scale=4.5 if a.model == 'omni' else 5.0,
                  generator=torch.Generator('cuda').manual_seed(a.seed), **kwargs)
    mesh = result['shapes'][0][0] if a.model == 'omni' else result[0]
    mesh = DegenerateFaceRemover()(FloaterRemover()(mesh))
    mesh.export(a.out / f'{name}.glb')
    stats[name] = {'seconds': round(time.time() - start, 1), 'peak_gib': round(torch.cuda.max_memory_allocated() / 2**30, 2),
                   'faces': int(len(mesh.faces))}
    print('DONE', name, stats[name], flush=True)
(a.out / 'stats.json').write_text(json.dumps(stats, indent=1))

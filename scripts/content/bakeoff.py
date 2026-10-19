#!/usr/bin/env python3
"""Text-only material bakeoff. GPU stages must run on dev-ubuntu, never on Mac.

prepare resolves public HF metadata without accepting gates. download fetches only
needed pinned components. GPU stages are serialized by run-materials.sh, which
restores any temporarily stopped services. All binary output is outside git.
"""
from __future__ import annotations
import argparse
import gc
import json
import os
from pathlib import Path
import subprocess
import time
import urllib.request

MODELS = {
    'flux': ('black-forest-labs/FLUX.1-schnell', 'Apache-2.0'),
    'sdxl': ('stabilityai/stable-diffusion-xl-base-1.0', 'CreativeML OpenRAIL++-M'),
    'stablematerials': ('gvecchio/StableMaterials', 'openrail'),
    'normals': ('prs-eth/marigold-normals-v1-1', 'CreativeML OpenRAIL++-M'),
    'appearance': ('prs-eth/marigold-iid-appearance-v1-1', 'CreativeML OpenRAIL++-M'),
}
MATERIALS = [
    ('cobblestone', 'Irregular rounded grey cobblestone pavement, small stones, dirty recessed joints', .83),
    ('stone-wall', 'City wall masonry, staggered rough grey limestone blocks, recessed lime mortar', .85),
    ('plaster', 'Weathered pale ochre lime plaster for a timber framed house, fine cracks and stains', .91),
    ('floorboards', 'Wide parallel oak floorboards, worn grain, narrow dark gaps, old nail holes', .76),
    ('timber', 'Rough hewn parallel timber beams and logs, dark aged wood grain, deep checks', .86),
    ('thatch', 'Dense overlapping golden brown thatched roof straw, irregular coarse dry fibres', .94),
    ('roof-tiles', 'Rows of overlapping curved terracotta roof tiles, chipped edges, faded red clay', .84),
    ('mud', 'Compacted brown earth and dried mud, shallow footprints, small pebbles and damp patches', .90),
    ('forest-floor', 'Forest floor, dark leaf litter and soil, scattered moss and short sparse grass', .94),
    ('rock', 'Rough grey rock face, fractured layered stone, mineral grain and traces of lichen', .89),
]
STYLE = ', medieval, worn and dirty, slightly muted colors, diffuse light without highlights or shadows, strictly top-down orthographic view, seamless tile, no text or logos'
NEGATIVE = 'text, logos, watermark, perspective, horizon, cast shadows, specular highlights, objects, frame'
PIPELINES = ['flux-marigold', 'flux-filter', 'sdxl-marigold', 'sdxl-filter', 'stablematerials']
MAPS = ('albedo', 'normal', 'roughness', 'metallic', 'height', 'ao')
FLUX_REVISION = '741f7c3ce8b383c54771c7003378a50191e9efe9'
FLUX_TILING = 'circular VAE Conv2d and asymmetric downsample padding; packed latent feature rolling by half a tile on alternating denoising steps'
FLUX_QUANTIZATION = 'bitsandbytes NF4 transformer and T5 with double quantization and bfloat16 compute; two-phase prompt encoding releases T5/CLIP before transformer load; model CPU offload'


def read_registry(path):
    return [json.loads(line) for line in path.read_text().splitlines() if line.strip()]


def write_registry(path, records):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix('.tmp')
    temporary.write_text(''.join(json.dumps(r, ensure_ascii=False, sort_keys=True, allow_nan=False) + '\n' for r in records))
    temporary.replace(path)


def prepare(path):
    metadata = {}
    for key, (model, license_name) in MODELS.items():
        with urllib.request.urlopen('https://huggingface.co/api/models/' + model, timeout=30) as response:
            info = json.load(response)
        metadata[key] = {'id': model, 'revision': info['sha'], 'license': license_name, 'gated': info.get('gated', False)}
    records = []
    for index, (material, description, roughness) in enumerate(MATERIALS):
        for pipeline in PIPELINES:
            base = pipeline.split('-')[0]
            keys = [base] + (['normals', 'appearance'] if pipeline.endswith('marigold') else [])
            models = [{k: v for k, v in metadata[key].items() if k != 'gated'} for key in keys]
            gated = [metadata[key]['id'] for key in keys if metadata[key]['gated']]
            record = {
                'id': f'bakeoff-{material}-{pipeline}', 'kind': 'material', 'replaces': [],
                'pipeline': pipeline, 'models': models, 'prompt': description + STYLE,
                'negative': NEGATIVE, 'seed': 31000 + index, 'status': 'draft', 'files': {},
                'parameters': {'material': material, 'resolution': 512 if base == 'stablematerials' else 1024,
                    'steps': 50 if base == 'stablematerials' else (4 if base == 'flux' else 30),
                    'guidance_scale': 10.0 if base == 'stablematerials' else (0.0 if base == 'flux' else 7.0),
                    'roughness_base': roughness, 'tileable': True,
                    'tiling': 'native feature rolling and padded decode' if base == 'stablematerials' else 'circular Conv2d padding in UNet and VAE, including asymmetric downsample padding',
                    'height_filter': {'sigma_small_px_at_1024': 2, 'sigma_large_px_at_1024': 32, 'normal_strength': .025},
                    'normal_convention': 'OpenGL: X right, Y up, Z toward viewer; DDS pack flips Y for engine texture-V basis',
                    'input': 'text only; map estimators see only the generated source image'},
                'generation': {'outcome': 'pending'},
            }
            if pipeline.endswith('marigold'):
                record['parameters']['marigold'] = {'steps': 4, 'ensemble_size': 1, 'processing_resolution': 1024, 'match_input_resolution': True, 'seed': 41000 + index,
                    'tiling': 'circular UNet/VAE convolution and asymmetric downsample padding; generated 1024 source input',
                    'height_ao': 'non-neural luminance bandpass from generated source, shared with filter variant'}
            if base == 'flux':
                record['parameters']['quantization'] = 'int8 transformer and T5 (planned; gated model skipped)'
            if gated:
                record['skip_reason'] = 'HF gated agreement required; not accepted: ' + ', '.join(gated)
                record['generation'] = {'outcome': 'skipped'}
            records.append(record)
    write_registry(path, records)
    print(f'{len(records)} recipes; {sum(bool(r.get("skip_reason")) for r in records)} skipped')


def enable_flux(records):
    """Enable only the recipes whose HF agreement the user accepted separately.

    This command neither reads credentials nor accepts an agreement. It is
    idempotent, so resuming a completed FLUX run does not lose its measurements.
    """
    count = 0
    for record in records:
        if not record['pipeline'].startswith('flux-'):
            continue
        model = next(m for m in record['models'] if m['id'] == MODELS['flux'][0])
        reset = model['revision'] != FLUX_REVISION or record['generation']['outcome'] == 'skipped'
        model.update(revision=FLUX_REVISION, license='Apache-2.0')
        record.pop('skip_reason', None)
        record['parameters'].update(resolution=1024, steps=4, guidance_scale=0.0,
            max_sequence_length=256, quantization=FLUX_QUANTIZATION, torch_dtype='bfloat16',
            tiling=FLUX_TILING, negative_prompt_used=False)
        if reset:
            record['generation'] = {'outcome': 'pending'}
            record['files'] = {}
        count += 1
    if count != 20:
        raise ValueError(f'Expected 20 FLUX recipes, found {count}')


def flux_token(root):
    """Read the private credential only at an explicitly authenticated HF call."""
    token = (root/'hf/token').read_text().strip()
    if not token or any(character.isspace() for character in token):
        raise ValueError('FLUX credential file is empty or malformed')
    return token


def content_bytes(root):
    # HF snapshots symlink to blobs: count each file once, not once per link.
    seen = set()
    total = 0
    for path in root.rglob('*'):
        if path.is_file():
            stat = path.stat()
            key = (stat.st_dev, stat.st_ino)
            if key not in seen:
                seen.add(key)
                total += stat.st_size
    return total


def download(records, root):
    from huggingface_hub import snapshot_download
    models = {m['id']: m for r in records if not r.get('skip_reason') for m in r['models']}
    for model in models.values():
        if content_bytes(root) > 60_000_000_000:
            raise RuntimeError('60 GB pre-download safety reserve reached (80 GB task limit)')
        stable = model['id'] == MODELS['stablematerials'][0]
        flux = model['id'] == MODELS['flux'][0]
        allowed = ['model_index.json', 'README.md', 'LICENSE*', 'pipeline.py', 'scheduler/*', 'tokenizer/*', 'tokenizer_2/*',
                   'text_encoder/*.json', 'text_encoder_2/*.json', 'unet/config.json', 'vae/config.json']
        for component in ['text_encoder', 'unet', 'vae'] + ([] if stable else ['text_encoder_2']):
            allowed.append(component + ('/*.safetensors' if stable else '/*fp16.safetensors'))
        if flux:
            # The root checkpoint duplicates the diffusers transformer weights.
            # Only native component shards and their index files are needed.
            allowed = ['model_index.json', 'README.md', 'LICENSE*', 'scheduler/*', 'tokenizer/*', 'tokenizer_2/*',
                'text_encoder/config.json', 'text_encoder/model.safetensors',
                'text_encoder_2/config.json', 'text_encoder_2/model*.safetensors', 'text_encoder_2/model.safetensors.index.json',
                'transformer/config.json', 'transformer/diffusion_pytorch_model*.safetensors', 'transformer/diffusion_pytorch_model.safetensors.index.json',
                'vae/config.json', 'vae/diffusion_pytorch_model.safetensors']
        snapshot = snapshot_download(model['id'], revision=model['revision'], allow_patterns=allowed,
            cache_dir=root/'hf/hub', token=flux_token(root) if flux else False, max_workers=4)
        print(model['id'], snapshot, flush=True)


def model_path(records, root, key):
    model = next(m for r in records for m in r['models'] if m['id'] == MODELS[key][0])
    return root / 'hf/hub' / ('models--' + model['id'].replace('/', '--')) / 'snapshots' / model['revision']


def circular(module):
    import torch
    import types
    from diffusers.models.downsampling import Downsample2D
    count = 0
    for layer in module.modules():
        if isinstance(layer, torch.nn.Conv2d):
            layer.padding_mode = 'circular'
            count += 1
        if isinstance(layer, Downsample2D) and layer.use_conv and layer.padding == 0:
            original = layer.forward
            def forward(self, hidden_states, *args, _original=original, **kwargs):
                hidden_states = torch.nn.functional.pad(hidden_states, (0, 1, 0, 1), mode='circular')
                self.padding = 1  # Skip the original explicit zero-pad branch.
                try:
                    return _original(hidden_states, *args, **kwargs)
                finally:
                    self.padding = 0
            layer.forward = types.MethodType(forward, layer)
    return count


def periodic_flux(transformer):
    """Move the tile boundary into the interior on alternate diffusion steps.

    FLUX has no spatial convolutions to patch. Roll its packed latent grid,
    leave positional coordinates fixed, and undo the roll on its prediction.
    The solver therefore keeps one coordinate system while the denoiser sees
    both toroidal origins. No generated pixels are retouched or blended.
    """
    import torch
    import types
    original = transformer.forward
    transformer._tile_step = 0

    def forward(self, hidden_states, *args, **kwargs):
        batch, tokens, channels = hidden_states.shape
        side = int(tokens ** .5)
        if side * side != tokens:
            raise ValueError('FLUX bakeoff expects a square packed latent grid')
        shift = side // 2 if self._tile_step % 2 else 0
        self._tile_step += 1
        if shift:
            hidden_states = torch.roll(hidden_states.reshape(batch, side, side, channels),
                (shift, shift), (1, 2)).reshape(batch, tokens, channels)
        result = original(hidden_states, *args, **kwargs)
        if shift:
            prediction = result[0] if isinstance(result, tuple) else result.sample
            prediction = torch.roll(prediction.reshape(batch, side, side, channels),
                (-shift, -shift), (1, 2)).reshape(batch, tokens, channels)
            if isinstance(result, tuple):
                result = (prediction, *result[1:])
            else:
                result.sample = prediction
        return result

    transformer.forward = types.MethodType(forward, transformer)


def load_flux(records, root, pending):
    """Keep quantized T5 and transformer in disjoint GPU lifetimes.

    NF4 allows model CPU offload with our pinned bitsandbytes version. Encoding
    the text first also avoids keeping both models and their activations in GPU
    memory at once on the shared 24 GB card.
    """
    import torch
    from diffusers import BitsAndBytesConfig, FluxPipeline, FluxTransformer2DModel
    from transformers import BitsAndBytesConfig as TextBitsAndBytesConfig, T5EncoderModel
    path = str(model_path(records, root, 'flux'))
    common = dict(torch_dtype=torch.bfloat16, use_safetensors=True, local_files_only=True, low_cpu_mem_usage=True)
    quantization = dict(load_in_4bit=True, bnb_4bit_quant_type='nf4',
        bnb_4bit_use_double_quant=True, bnb_4bit_compute_dtype=torch.bfloat16)
    text_encoder = T5EncoderModel.from_pretrained(path, subfolder='text_encoder_2',
        quantization_config=TextBitsAndBytesConfig(**quantization), device_map={'': 'cuda:0'},
        token=flux_token(root), **common)
    encoder = FluxPipeline.from_pretrained(path, transformer=None, vae=None, text_encoder_2=text_encoder,
        token=flux_token(root), **common)
    encoder.enable_model_cpu_offload()
    embeddings = {}
    measurements = {}
    for record in pending:
        material = record['parameters']['material']
        torch.cuda.empty_cache()
        sample = gpu_state(root, 'flux-encode', material)
        torch.cuda.reset_peak_memory_stats()
        started = time.monotonic()
        with torch.inference_mode():
            prompt, pooled, ids = encoder.encode_prompt(prompt=record['prompt'], device=torch.device('cuda:0'),
                max_sequence_length=record['parameters']['max_sequence_length'])
            embeddings[material] = (prompt.detach().cpu(), pooled.detach().cpu())
        measurements[material] = {'prompt_encoding_seconds': round(time.monotonic()-started, 3),
            'prompt_encoding_peak_allocated_mib': round(torch.cuda.max_memory_allocated()/1024**2, 1),
            'prompt_encoding_peak_reserved_mib': round(torch.cuda.max_memory_reserved()/1024**2, 1),
            'prompt_encoding_gpu_before': sample}
        del prompt, pooled, ids
        print(material, 'flux-encode', json.dumps(measurements[material]), flush=True)
    # encode_prompt is called directly, so perform the cleanup normally done
    # at the end of FluxPipeline.__call__ before loading the transformer.
    encoder.maybe_free_model_hooks()
    del encoder, text_encoder
    gc.collect()
    torch.cuda.empty_cache()
    gpu_state(root, 'flux', 'transformer-load', 10000)
    transformer = FluxTransformer2DModel.from_pretrained(path, subfolder='transformer',
        quantization_config=BitsAndBytesConfig(**quantization), device_map={'': 'cuda:0'},
        token=flux_token(root), **common)
    pipe = FluxPipeline.from_pretrained(path, transformer=transformer, text_encoder=None, text_encoder_2=None,
        token=flux_token(root), **common)
    circular(pipe.vae)
    periodic_flux(pipe.transformer)
    pipe.enable_model_cpu_offload()
    return pipe, embeddings, measurements


def gpu_state(root, stage, material, reserve_mib=0):
    result = subprocess.run(['nvidia-smi', '--query-gpu=memory.free,memory.used', '--format=csv,noheader,nounits'], check=True, capture_output=True, text=True)
    free, used = map(int, result.stdout.strip().split(','))
    sample = {'utc': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()), 'stage': stage, 'material': material, 'free_mib': free, 'used_mib': used}
    with (root/'gpu.jsonl').open('a') as output:
        output.write(json.dumps(sample) + '\n')
    if free < reserve_mib:
        raise RuntimeError(f'GPU free {free} MiB < required {reserve_mib} MiB')
    return sample


def save_maps(root, record, maps):
    import numpy as np
    from PIL import Image
    directory = root / 'out/bakeoff' / record['id']
    directory.mkdir(parents=True, exist_ok=True)
    for key in MAPS:
        value = np.asarray(maps[key])
        if not np.isfinite(value).all():
            raise ValueError(f'{key} contains nonfinite values')
        if value.ndim == 3 and value.shape[-1] == 1:
            value = value[..., 0]
        Image.fromarray(np.rint(np.clip(value, 0, 1) * 255).astype('uint8')).save(directory / (key + '.png'))
    record['files'] = {key: f'bakeoff/{record["id"]}/{key}.png' for key in MAPS}
    record['generation']['outcome'] = 'complete'


def filtered_maps(image, record):
    import numpy as np
    from scipy.ndimage import gaussian_filter
    rgb = np.asarray(image.convert('RGB'), dtype=np.float32) / 255
    luminance = rgb @ np.array([.2126, .7152, .0722], dtype=np.float32)
    size = rgb.shape[0]
    small = gaussian_filter(luminance, 2 * size / 1024, mode='wrap')
    large = gaussian_filter(luminance, 32 * size / 1024, mode='wrap')
    band = small - large
    scale = max(float(np.percentile(np.abs(band), 98)), .02)
    height = np.clip(.5 + band / (2 * scale), 0, 1)
    dx = (np.roll(height, -1, axis=1) - np.roll(height, 1, axis=1)) * size / 2
    dy = (np.roll(height, -1, axis=0) - np.roll(height, 1, axis=0)) * size / 2
    # Texture V points down; OpenGL-style tangent normal Y points up.
    normal = np.stack((-.025 * dx, .025 * dy, np.ones_like(dx)), axis=-1)
    normal /= np.linalg.norm(normal, axis=-1, keepdims=True)
    ao = np.clip(1 - .8 * np.maximum(gaussian_filter(height, 8 * size / 1024, mode='wrap') - height, 0), .35, 1)
    roughness = np.clip(record['parameters']['roughness_base'] + .14 * (large - luminance), .35, 1)
    return dict(albedo=rgb, normal=normal * .5 + .5, height=height, roughness=roughness, metallic=np.zeros_like(height), ao=ao)


def stage_record(records, material, stage, base='sdxl'):
    pipeline = stage + '-filter' if stage in ('sdxl', 'flux') else ('stablematerials' if stage == 'stablematerials' else base + '-marigold')
    return next(r for r in records if r['pipeline'] == pipeline and r['parameters']['material'] == material)


def source_path(root, material, base='sdxl'):
    return root/'out/bakeoff/_sources'/base/f'{material}.png'


def run_stage(records, registry, root, stage, base='sdxl'):
    if __import__('platform').system() != 'Linux':
        raise RuntimeError('Model execution is allowed only on dev-ubuntu Linux')
    import numpy as np
    import torch
    from PIL import Image
    torch.set_num_threads(4)
    torch.backends.cuda.matmul.allow_tf32 = True
    pending = [stage_record(records, material, stage, base) for material, _, _ in MATERIALS]
    pending = [record for record in pending if not record.get('skip_reason') and stage not in record['generation']
        and not (stage in ('sdxl', 'flux', 'stablematerials') and record['generation']['outcome'] == 'complete')]
    if not pending:
        print(f'{stage}: no pending materials', flush=True)
        return
    gpu_state(root, stage, 'load', 10000 if stage == 'flux' else 8500)
    before_load = time.monotonic()
    prompt_embeddings, encoding_stats = {}, {}
    if stage == 'flux':
        pipe, prompt_embeddings, encoding_stats = load_flux(records, root, pending)
    elif stage == 'sdxl':
        from diffusers import StableDiffusionXLPipeline
        pipe = StableDiffusionXLPipeline.from_pretrained(str(model_path(records, root, 'sdxl')), torch_dtype=torch.float16, variant='fp16', use_safetensors=True, local_files_only=True, add_watermarker=False, token=False)
        circular(pipe.unet); circular(pipe.vae)
        pipe.enable_model_cpu_offload()
    elif stage == 'stablematerials':
        from stable_materials_adapter import load_pipeline
        pipe = load_pipeline(str(model_path(records, root, stage)))
    else:
        from diffusers import MarigoldNormalsPipeline, MarigoldIntrinsicsPipeline
        cls = MarigoldNormalsPipeline if stage == 'normals' else MarigoldIntrinsicsPipeline
        pipe = cls.from_pretrained(str(model_path(records, root, stage)), torch_dtype=torch.float16, variant='fp16', use_safetensors=True, local_files_only=True, token=False)
        circular(pipe.unet); circular(pipe.vae)
        pipe.enable_model_cpu_offload()
    load_seconds = time.monotonic() - before_load - sum(stats['prompt_encoding_seconds'] for stats in encoding_stats.values())
    for record in pending:
        material = record['parameters']['material']
        source = source_path(root, material, stage if stage in ('sdxl', 'flux') else base)
        torch.cuda.empty_cache()
        sample = gpu_state(root, stage, material)
        torch.cuda.reset_peak_memory_stats()
        started = time.monotonic()
        if stage in ('sdxl', 'flux'):
            arguments = dict(height=1024, width=1024, guidance_scale=record['parameters']['guidance_scale'],
                num_inference_steps=record['parameters']['steps'], generator=torch.Generator('cpu').manual_seed(record['seed']))
            if stage == 'flux':
                prompt, pooled = prompt_embeddings[material]
                arguments.update(prompt_embeds=prompt.to('cuda:0', dtype=pipe.transformer.dtype),
                    pooled_prompt_embeds=pooled.to('cuda:0', dtype=pipe.transformer.dtype),
                    max_sequence_length=record['parameters']['max_sequence_length'])
                pipe.transformer._tile_step = 0
            else:
                arguments.update(prompt=record['prompt'], negative_prompt=record['negative'])
            image = pipe(**arguments).images[0]
            del arguments
            source.parent.mkdir(parents=True, exist_ok=True)
            image.save(source)
            save_maps(root, record, filtered_maps(image, record))
        elif stage == 'stablematerials':
            from stable_materials_adapter import generate
            maps = generate(pipe, record['prompt'], record['negative'], record['seed'], steps=record['parameters']['steps'])
            from scipy.ndimage import gaussian_filter
            h = maps['height']
            maps['ao'] = np.clip(1 - .8 * np.maximum(gaussian_filter(h, 4, mode='wrap') - h, 0), .35, 1)
            save_maps(root, record, maps)
        else:
            image = Image.open(source).convert('RGB')
            result = pipe(image, num_inference_steps=4, ensemble_size=1, processing_resolution=1024,
                match_input_resolution=True, generator=torch.Generator('cpu').manual_seed(record['parameters']['marigold']['seed']))
            prediction = result.prediction
            crop = (slice(None), slice(None))
            directory = root/'out/bakeoff'/record['id']
            directory.mkdir(parents=True, exist_ok=True)
            if stage == 'normals':
                n = prediction[0][crop]
                # Marigold +X right,+Y up,+Z toward viewer (OpenGL convention).
                maps = filtered_maps(image, record)
                maps['normal'] = n * .5 + .5
                save_maps(root, record, maps)
                record['generation']['outcome'] = 'pending'
                record['files'] = {}
            else:
                # prediction shape [N*target,H,W,C], target 0=albedo,
                # target 1 material channels roughness, metallicity, unused.
                maps = {k: np.asarray(Image.open(directory/(k+'.png')), dtype=np.float32)/255 for k in MAPS}
                maps['albedo'] = prediction[0][crop]
                maps['roughness'] = prediction[1][crop][...,0]
                maps['metallic'] = prediction[1][crop][...,1]
                save_maps(root, record, maps)
        record['generation'][stage] = {'seconds': round(time.monotonic()-started,3), 'load_seconds': round(load_seconds,3),
            'peak_allocated_mib': round(torch.cuda.max_memory_allocated()/1024**2,1), 'peak_reserved_mib': round(torch.cuda.max_memory_reserved()/1024**2,1), 'gpu_before': sample,
            'host': 'dev-ubuntu', 'utc': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())}
        if stage == 'flux':
            record['generation'][stage].update(encoding_stats[material])
        print(material, stage, json.dumps(record['generation'][stage]), flush=True)
        write_registry(registry, records)
    del pipe
    gc.collect()
    torch.cuda.empty_cache()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=['prepare','enable-flux','download','flux','sdxl','normals','appearance','stablematerials'])
    parser.add_argument('--registry', type=Path, default=Path('data/content/registry.jsonl'))
    parser.add_argument('--root', type=Path, default=Path.home()/'gothic-ai/materials')
    parser.add_argument('--base', choices=['sdxl', 'flux'], default='sdxl', help='Generated source for Marigold stages (default: sdxl)')
    args = parser.parse_args()
    if args.command == 'prepare':
        prepare(args.registry)
        return
    args.root = args.root.resolve()
    args.root.mkdir(parents=True, exist_ok=True)
    os.environ['HF_HOME'] = str(args.root/'hf')
    os.environ['HF_HUB_DISABLE_IMPLICIT_TOKEN'] = '1'
    records = read_registry(args.registry)
    if args.command == 'enable-flux':
        enable_flux(records)
        write_registry(args.registry, records)
        print('20 FLUX recipes enabled; other recipes unchanged')
    elif args.command == 'download':
        download(records, args.root)
    else:
        run_stage(records, args.registry, args.root, args.command, args.base)

if __name__ == '__main__':
    main()

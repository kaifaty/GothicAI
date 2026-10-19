"""FLUX auth, resume and source routing without loading a model or GPU."""
from copy import deepcopy
from contextlib import nullcontext
import fnmatch
import importlib.util
import json
from pathlib import Path
import sys
from types import SimpleNamespace
import weakref

import pytest


SPEC = importlib.util.spec_from_file_location('material_bakeoff', Path(__file__).with_name('bakeoff.py'))
bakeoff = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(bakeoff)


def recipes():
    entries = bakeoff.read_registry(Path(__file__).resolve().parents[2]/'data/content/registry.jsonl')
    for entry in entries:
        if entry['pipeline'].startswith('flux-'):
            entry['generation'] = {'outcome': 'skipped'}
            entry['files'] = {}
            entry['skip_reason'] = 'HF agreement was not previously accepted'
            entry['models'][0]['revision'] = '0' * 40
    return entries


def test_enable_flux_preserves_other_materials_and_resume(tmp_path):
    records = recipes()
    original = deepcopy(records)
    bakeoff.enable_flux(records)
    changed = [record for record, old in zip(records, original) if record != old]
    assert len(changed) == 20
    assert all(record['pipeline'].startswith('flux-') for record in changed)
    assert all(record['generation'] == {'outcome': 'pending'} and record['files'] == {} for record in changed)
    assert all(record['models'][0]['revision'] == bakeoff.FLUX_REVISION for record in changed)
    assert all(record['models'][0]['license'] == 'Apache-2.0' for record in changed)
    # Refresh an earlier pending recipe after a precision change without
    # resetting seeds, unrelated results, or completed measurements.
    changed[1]['parameters']['quantization'] = 'earlier precision'
    bakeoff.enable_flux(records)
    assert changed[1]['parameters']['quantization'] == bakeoff.FLUX_QUANTIZATION
    assert all(record['seed'] == old['seed'] for record, old in zip(records, original))
    assert all('skip_reason' not in record for record in changed)
    complete = changed[0]
    complete['generation'] = {'outcome': 'complete', 'flux': {'seconds': 12.5}}
    complete['files'] = {'albedo': 'existing.png'}
    snapshot = deepcopy(records)
    bakeoff.enable_flux(records)
    assert records == snapshot
    output = tmp_path/'registry.jsonl'
    bakeoff.write_registry(output, records)
    assert bakeoff.read_registry(output) == records


def test_download_sends_credential_only_to_flux_and_does_not_print_it(tmp_path, monkeypatch, capsys):
    records = recipes()
    bakeoff.enable_flux(records)
    secret = 'test-private-credential-not-for-output'
    (tmp_path/'hf').mkdir()
    (tmp_path/'hf/token').write_text(secret+'\n')
    calls = []

    def snapshot_download(model, **kwargs):
        calls.append((model, kwargs))
        return tmp_path/'snapshot'

    monkeypatch.setitem(sys.modules, 'huggingface_hub', SimpleNamespace(snapshot_download=snapshot_download))
    bakeoff.download(records, tmp_path)
    assert len(calls) == 5
    for model, kwargs in calls:
        assert kwargs['token'] == (secret if model == bakeoff.MODELS['flux'][0] else False)
    flux = next(kwargs for model, kwargs in calls if model == bakeoff.MODELS['flux'][0])
    assert flux['revision'] == bakeoff.FLUX_REVISION
    for filename in ('transformer/diffusion_pytorch_model-00001-of-00003.safetensors',
                     'transformer/diffusion_pytorch_model.safetensors.index.json',
                     'text_encoder_2/model-00002-of-00002.safetensors',
                     'text_encoder_2/model.safetensors.index.json'):
        assert any(fnmatch.fnmatch(filename, pattern) for pattern in flux['allow_patterns'])
    assert not any(fnmatch.fnmatch('flux1-schnell.safetensors', pattern) for pattern in flux['allow_patterns'])
    assert secret not in capsys.readouterr().out
    assert secret not in json.dumps(records)
    (tmp_path/'hf/token').unlink()
    # Public model downloads must not try to read a missing FLUX credential.
    bakeoff.download([record for record in records if not record['pipeline'].startswith('flux-')], tmp_path)


@pytest.mark.parametrize('stage', ['normals', 'appearance'])
def test_marigold_uses_the_selected_generated_source(stage, tmp_path):
    records = recipes()
    bakeoff.enable_flux(records)
    sdxl = bakeoff.stage_record(records, 'cobblestone', stage)
    flux = bakeoff.stage_record(records, 'cobblestone', stage, 'flux')
    assert sdxl['pipeline'] == 'sdxl-marigold'
    assert flux['pipeline'] == 'flux-marigold'
    assert bakeoff.source_path(tmp_path, 'cobblestone') == tmp_path/'out/bakeoff/_sources/sdxl/cobblestone.png'
    assert bakeoff.source_path(tmp_path, 'cobblestone', 'flux') == tmp_path/'out/bakeoff/_sources/flux/cobblestone.png'
    assert bakeoff.stage_record(records, 'cobblestone', 'flux')['pipeline'] == 'flux-filter'


def test_cache_size_does_not_double_count_snapshot_links(tmp_path):
    blob = tmp_path/'blob'
    blob.write_bytes(b'weights')
    (tmp_path/'snapshot').symlink_to(blob)
    assert bakeoff.content_bytes(tmp_path) == len(b'weights')


def test_flux_load_releases_t5_before_transformer_and_routes_private_auth(tmp_path, monkeypatch, capsys):
    records = recipes()
    bakeoff.enable_flux(records)
    pending = [record for record in records if record['pipeline'] == 'flux-filter'][:2]
    secret = 'private-test-only-flux-load'
    (tmp_path/'hf').mkdir()
    (tmp_path/'hf/token').write_text(secret)
    calls, gpu_checks, t5_refs, encoder_offloaded = [], [], [], []

    class Embedding:
        device = 'cuda:0'

        def detach(self):
            return self

        def cpu(self):
            output = Embedding()
            output.device = 'cpu'
            return output

    class T5:
        @classmethod
        def from_pretrained(cls, path, **kwargs):
            calls.append(('t5', kwargs))
            model = cls()
            t5_refs.append(weakref.ref(model))
            return model

    class Transformer:
        @classmethod
        def from_pretrained(cls, path, **kwargs):
            assert t5_refs[0]() is None, 'T5 must be freed before allocating transformer'
            assert encoder_offloaded == [True]
            calls.append(('transformer', kwargs))
            return cls()

    class Pipeline:
        @classmethod
        def from_pretrained(cls, path, **kwargs):
            calls.append(('pipeline', kwargs.copy() if kwargs.get('transformer') else {
                key: value for key, value in kwargs.items() if key != 'text_encoder_2'}))
            output = cls()
            if kwargs.get('transformer') is None:
                assert kwargs['vae'] is None
                output.t5 = kwargs['text_encoder_2']
            else:
                assert kwargs['text_encoder'] is None and kwargs['text_encoder_2'] is None
                output.transformer = kwargs['transformer']
                output.vae = object()
            return output

        def enable_model_cpu_offload(self):
            pass

        def maybe_free_model_hooks(self):
            encoder_offloaded.append(True)

        def encode_prompt(self, **kwargs):
            assert self.t5 is t5_refs[0]()
            assert kwargs['device'] == 'cuda:0'
            assert kwargs['max_sequence_length'] == 256
            return Embedding(), Embedding(), Embedding()

    cuda = SimpleNamespace(empty_cache=lambda: None, reset_peak_memory_stats=lambda: None,
        max_memory_allocated=lambda: 123, max_memory_reserved=lambda: 456)
    monkeypatch.setitem(sys.modules, 'torch', SimpleNamespace(bfloat16='bfloat16', device=str,
        inference_mode=nullcontext, cuda=cuda))
    monkeypatch.setitem(sys.modules, 'diffusers', SimpleNamespace(BitsAndBytesConfig=lambda **kwargs: kwargs,
        FluxPipeline=Pipeline, FluxTransformer2DModel=Transformer))
    monkeypatch.setitem(sys.modules, 'transformers', SimpleNamespace(BitsAndBytesConfig=lambda **kwargs: kwargs,
        T5EncoderModel=T5))
    monkeypatch.setattr(bakeoff, 'gpu_state', lambda root, stage, material, *args:
        gpu_checks.append((stage, material)) or {'stage': stage, 'material': material})
    monkeypatch.setattr(bakeoff, 'circular', lambda module: None)
    monkeypatch.setattr(bakeoff, 'periodic_flux', lambda module: None)
    pipe, embeddings, measurements = bakeoff.load_flux(records, tmp_path, pending)
    assert pipe.transformer is not None
    assert t5_refs[0]() is None
    assert len(embeddings) == len(measurements) == 2
    assert all(prompt.device == pooled.device == 'cpu' for prompt, pooled in embeddings.values())
    assert [stage for stage, _ in gpu_checks] == ['flux-encode', 'flux-encode', 'flux']
    assert len(calls) == 4
    assert all(kwargs['token'] == secret and kwargs['local_files_only'] for _, kwargs in calls)
    assert all(kwargs['quantization_config'] == {'load_in_4bit': True,
        'bnb_4bit_quant_type': 'nf4', 'bnb_4bit_use_double_quant': True,
        'bnb_4bit_compute_dtype': 'bfloat16'}
        for kind, kwargs in calls if kind in ('t5', 'transformer'))
    assert secret not in capsys.readouterr().out
    assert secret not in json.dumps(records)


def test_periodic_flux_moves_seam_and_restores_prediction_coordinates(monkeypatch):
    class Tensor:
        def __init__(self, values, shape):
            self.values, self.shape = list(values), shape

        def reshape(self, *shape):
            return Tensor(self.values, shape)

    def roll(tensor, shifts, dimensions):
        assert dimensions == (1, 2)
        batch, height, width, channels = tensor.shape
        values = [tensor.values[((b*height+(y-shifts[0]) % height)*width+(x-shifts[1]) % width)*channels+c]
            for b in range(batch) for y in range(height) for x in range(width) for c in range(channels)]
        return Tensor(values, tensor.shape)

    seen = []

    class Transformer:
        def forward(self, hidden_states, **kwargs):
            seen.append(hidden_states.values)
            return (Tensor([2*value for value in hidden_states.values], hidden_states.shape),)

    monkeypatch.setitem(sys.modules, 'torch', SimpleNamespace(roll=roll))
    transformer = Transformer()
    bakeoff.periodic_flux(transformer)
    original = Tensor(range(16), (1, 16, 1))
    for _ in range(4):
        assert transformer.forward(original)[0].values == [2*value for value in original.values]
    assert seen[0] == seen[2] == original.values
    assert seen[1] == seen[3] == [10, 11, 8, 9, 14, 15, 12, 13, 2, 3, 0, 1, 6, 7, 4, 5]

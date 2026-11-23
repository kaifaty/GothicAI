"""Check the Mark plan through the legacy scripts/content/npc/build_head.py import and
incremental builds from character-tool/src/character_tool/head/build_head.py."""
import hashlib
import importlib
import json
import os
import sys
from pathlib import Path

import pytest
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import build_head  # noqa: E402


def plan(tmp_path, monkeypatch, builder=build_head, **extra):
    monkeypatch.setattr(builder, 'base_cache', lambda: str(tmp_path/'base'))
    cdir = tmp_path/'concept'
    cdir.mkdir(exist_ok=True)
    for name in ('sheet.png', 'bald.png'):
        Image.new('RGB', (64, 32), (90, 90, 90)).save(cdir/name)
    cfg = {'name': 'test', 'sheet': 'sheet.png', 'bald': 'bald.png', **extra}
    (cdir/'npc.json').write_text(json.dumps(cfg))
    return {s.name: s for s in builder.steps(str(cdir), cfg, str(tmp_path/'work'), str(tmp_path/'content'))}


def plan_with_current_env(tmp_path, monkeypatch, **extra):
    # The CLI reads the external roots at import time in each new process.
    with monkeypatch.context() as fresh:
        fresh.delitem(sys.modules, 'head_paths')
        fresh.delitem(sys.modules, 'build_head')
        fresh.syspath_prepend(build_head.HEAD)
        builder = importlib.import_module('build_head')
        return builder, plan(tmp_path, fresh, builder, **extra)


@pytest.mark.parametrize('cached_bald', [True, False], ids=['mixed', 'explicit'])
def test_mesh_cache_follows_data_root_in_same_work(tmp_path, monkeypatch, cached_bald):
    work = tmp_path/'work'
    concept = tmp_path/'concept'
    concept.mkdir()
    mesh = {'full': 'full.npz'}
    if not cached_bald:
        mesh['bald'] = 'bald.npz'
    for name in mesh.values():
        (concept/name).touch()
    views = b'{}'
    for kind in ('full', 'bald'):
        directory = work/'views'/kind
        directory.mkdir(parents=True)
        (directory/'transforms.json').write_bytes(views)
    key = 'pixal3d-seed1-' + hashlib.sha256(views).hexdigest()[:16]

    for data, rebuild in ((tmp_path/'data-a', True), (tmp_path/'data-b', cached_bald)):
        cached = data/'mpfb/work/build/_mesh'/key/'raw.npz'
        cached.parent.mkdir(parents=True)
        cached.touch()
        monkeypatch.setenv('GOTHIC_AI_DATA', str(data))
        builder, steps = plan_with_current_env(tmp_path, monkeypatch, mesh=mesh)
        assert builder.run(steps['mesh'], work) is rebuild
        assert (work/'mesh/full/raw.npz').resolve() == concept/'full.npz'
        assert (work/'mesh/bald/raw.npz').resolve() == (cached if cached_bald else concept/'bald.npz')
        (cached.parent/'meta.json').write_text('{}')
        assert not builder.run(steps['mesh'], work)


@pytest.mark.parametrize('recipe', [{}, {'params': 'params.json'}], ids=['auto', 'preset'])
def test_mpfb_profile_fingerprints_in_same_work(tmp_path, monkeypatch, recipe):
    monkeypatch.setenv('GOTHIC_AI_DATA', str(tmp_path/'data'))
    monkeypatch.setenv('TEXPACK', str(tmp_path/'texpack'))
    (tmp_path/'work/views/points').mkdir(parents=True)

    def fingerprints(profile):
        monkeypatch.setenv('BLENDER_USER_RESOURCES', str(profile))
        builder, steps = plan_with_current_env(tmp_path, monkeypatch, **recipe)
        keys = {}
        for name in ('base', 'reg', 'cut', 'export', 'round'):
            for path in map(Path, steps[name].inputs):
                if path.is_relative_to(tmp_path) and not path.exists():
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.touch()
            keys[name] = builder.stamp(steps[name])
        return keys

    first, second = tmp_path/'profile-a', tmp_path/'profile-b'
    first.mkdir()
    second.mkdir()
    before = fingerprints(first)
    (first/'unrelated').touch()
    assert fingerprints(first) == before
    after = fingerprints(second)
    assert {name for name in after if after[name] != before[name]} == {'base', 'reg', 'cut', 'export'}


def test_without_the_key_the_plan_is_as_before(tmp_path, monkeypatch):
    steps = plan(tmp_path, monkeypatch)
    assert 'shapes' not in steps
    export = steps['export']
    assert len(export.cmds) == 1                               # the export alone, no copy of the MPFB head
    assert export.outputs == [str(tmp_path/'content'/'characters'/'hero'/'head.glb')]


def test_mark_adds_the_step_after_the_export(tmp_path, monkeypatch):
    before = plan(tmp_path, monkeypatch)
    steps = plan(tmp_path, monkeypatch, shapes='mark')
    order = list(before)
    order.insert(order.index('export')+1, 'shapes')
    assert list(steps) == order
    assert steps['export'].cmds[0] == before['export'].cmds[0]
    kept = str(tmp_path/'work'/'export'/'head-mpfb.glb')
    assert kept in steps['export'].outputs
    cmd = steps['shapes'].cmds[0]
    assert cmd[1].endswith('shapes/head_shapes.py') and cmd[2] == kept and '--face' in cmd


def test_an_unknown_set_is_refused(tmp_path, monkeypatch):
    with pytest.raises(SystemExit):
        plan(tmp_path, monkeypatch, shapes='ict')


def test_standalone_character_tool_wheel(tmp_path, monkeypatch):
    """Run character-tool/src/character_tool/cli.py and character-tool/src/character_tool/face/face_track.py
    through the installed console entry point."""
    import base64
    import shutil
    import subprocess

    manifest = Path(__file__).resolve().parents[3] / 'character-tool/pyproject.toml'
    source = shutil.copytree(manifest.parent, tmp_path / 'character-tool', symlinks=True,
                             ignore=shutil.ignore_patterns('build', 'dist', '*.egg-info', '.venv', '__pycache__'))
    monkeypatch.chdir(tmp_path)
    monkeypatch.delenv('PYTHONPATH', raising=False)
    subprocess.run(['uv', 'build', '--wheel', source], check=True)
    wheel, = (source / 'dist').glob('*.whl')
    venv = tmp_path / 'venv'
    subprocess.run(['uv', 'venv', '--python', sys.executable, venv], check=True)
    subprocess.run(['uv', 'pip', 'install', '--python', venv, '--no-deps', wheel], check=True)
    shutil.rmtree(source)

    cli = venv / ('Scripts/character-tool.exe' if os.name == 'nt' else 'bin/character-tool')
    (tmp_path / 'weights.csv').write_text('0,1' + ',0' * 51 + '\n')
    subprocess.run([cli, 'face', 'json', 'weights.csv', 'out.face.json'], check=True)
    track = json.loads((tmp_path / 'out.face.json').read_text())
    assert track['fps'] == 30
    assert track['shapes'][0] == 'eyeBlinkLeft'
    assert base64.b64decode(track['data']) == b'\xff' + bytes(51)

    package, = venv.rglob('site-packages/character_tool')
    for name in (
        'head/template_points.json', 'head/mpfb/test-head.face.json',
        'body/tripo-body-v1.landmarks.json', 'anim/moves-strikes.json', 'anim/run_kimodo.sh',
        'face/native/CMakeLists.txt', 'face/native/a2f-weights.cpp',
        'backends/mesh3d/run-mesh3d.sh', 'backends/texpack/texpack.cpp', 'backends/texpack/BUILD.md',
    ):
        assert (package / name).is_file(), name

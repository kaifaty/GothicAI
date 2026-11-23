import importlib.util
import json
from pathlib import Path

import pytest

SCRIPT = Path(__file__).resolve().parent / 'fire/torch_fire_content.py'
spec = importlib.util.spec_from_file_location('torch_fire_content', SCRIPT)
torch = importlib.util.module_from_spec(spec)
spec.loader.exec_module(torch)

FILES = ['particles.d/wall-torch-fire.json', 'worlds/NEWWORLD/vobs.d/wall-torch-fire.json']


def texture(path: Path, text: str) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)
    return path


def test_part_holds_the_flame_and_leaves_the_harbours_smoke(tmp_path, monkeypatch):
    monkeypatch.setattr(torch, 'STAGING', tmp_path / 'content-staging')
    atlas = texture(tmp_path / 'atlas.dds', 'flame')
    assert torch.main(['--set', 'torch-fire-1', '--atlas', str(atlas)]) == 0
    part = tmp_path / 'content-staging/torch-fire-1'
    assert json.loads((part / 'materials/materials.json').read_text()) == {
        'OWN_TORCH_FLAME.TGA': {'albedo': 'own-torch-flame/albedo.dds'}}
    assert (part / 'materials/own-torch-flame/albedo.dds').read_text() == 'flame'
    assert not (part / 'materials/own-hut-smoke').exists()
    for name in FILES:
        assert json.loads((part / name).read_text()) == json.loads((torch.PILOT / name).read_text())
    # the part's layer edits the torch's lamp and its soot names the harbour's texture
    layer = json.loads((part / FILES[1]).read_text())
    assert [entry['id'] for entry in layer['light']] == [20160]
    effects = json.loads((part / FILES[0]).read_text())['effects']
    assert any(effect.get('set', {}).get('visName_S') == 'OWN_HUT_SMOKE.TGA' for effect in effects)
    with pytest.raises(SystemExit, match='exists'):
        torch.main(['--set', 'torch-fire-1', '--atlas', str(atlas)])


def test_folder_gets_both_textures_and_the_installed_content_is_refused(tmp_path, monkeypatch):
    monkeypatch.setattr(torch, 'INSTALLED', tmp_path / 'content')
    atlas = texture(tmp_path / 'atlas.dds', 'flame')
    smoke = texture(tmp_path / 'smoke.dds', 'smoke')
    look = tmp_path / 'look'
    texture(look / 'materials/materials.json', json.dumps({'KEPT.TGA': {'albedo': 'kept/albedo.dds'}}))
    assert torch.main([str(look), '--atlas', str(atlas), '--smoke', str(smoke)]) == 0
    assert json.loads((look / 'materials/materials.json').read_text()) == {
        'KEPT.TGA': {'albedo': 'kept/albedo.dds'},
        'OWN_TORCH_FLAME.TGA': {'albedo': 'own-torch-flame/albedo.dds'},
        'OWN_HUT_SMOKE.TGA': {'albedo': 'own-hut-smoke/albedo.dds'}}
    assert (look / 'materials/own-hut-smoke/albedo.dds').read_text() == 'smoke'
    with pytest.raises(SystemExit, match='installed content'):
        torch.main([str(tmp_path / 'content'), '--atlas', str(atlas), '--smoke', str(smoke)])
    with pytest.raises(SystemExit, match='one of the two'):
        torch.main(['--atlas', str(atlas)])

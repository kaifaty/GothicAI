"""The step "shapes" of scripts/content/npc/build_head.py (reports/content/2026-11-hero-head/face-shapes.md, "Формы
Mark в игре"): without the key "shapes" in npc.json the plan and the export are as before, with "shapes": "mark" the
step follows the export and the export keeps the MPFB head."""
import json
import os
import sys

import pytest
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import build_head  # noqa: E402


def plan(tmp_path, monkeypatch, **extra):
    monkeypatch.setattr(build_head, 'base_cache', lambda: str(tmp_path/'base'))
    cdir = tmp_path/'concept'
    cdir.mkdir(exist_ok=True)
    for name in ('sheet.png', 'bald.png'):
        Image.new('RGB', (64, 32), (90, 90, 90)).save(cdir/name)
    cfg = {'name': 'test', 'sheet': 'sheet.png', 'bald': 'bald.png', **extra}
    (cdir/'npc.json').write_text(json.dumps(cfg))
    return {s.name: s for s in build_head.steps(str(cdir), cfg, str(tmp_path/'work'), str(tmp_path/'content'))}


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

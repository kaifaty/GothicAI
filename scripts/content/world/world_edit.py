#!/usr/bin/env python3
"""Edit chunks of the converted world mesh: take a base chunk, apply a list of edits in Blender, check, publish.

    python3 scripts/content/world/world_edit.py take NEWWORLD/1_-1 [--gui]
    python3 scripts/content/world/world_edit.py build <id> [--set <set> --chunk <WORLD>/<x>_<z>] [--from <id>] \\
        [--edit <script> ...] [--materials <materials.json> ...] [--dry-run] [--gui]
    python3 scripts/content/world/world_edit.py check <base.glb> <edited.glb> [--allowed allowed.json] \\
        [--materials <materials.json> ...]
    python3 scripts/content/world/world_edit.py compare <a.glb> <b.glb> [--full]

An edit is a Blender script in git with one function, edit(ctx) (world_bpy.Context): harbour_stairs.py,
roof_holes.py. A chunk's record in data/content/world-edits.jsonl holds its list of edits; build applies the whole
list, in order, to the base chunk itself, so edits of several steps in one chunk add up instead of overwriting
each other. build <id> of an existing record rebuilds it, --edit appends to its list, --from starts the list from
another record's (the same chunk in another set).

build: copies the base chunk to a work folder outside git (~/.local/share/gothic-ai/world-edit/<id>/), runs
Blender (world_edit_blender.py) with the original as an underlay, writes the result back in the converter's form
with the untouched source triangles byte for byte as in the base (chunk_canon.py), checks it (chunk_glb.check:
border vertices, open edges, vertex light, source triangles, materials, triangles and area per material) and exits non-zero with the reasons when a check
fails. A passing chunk goes to <staging>/<set>/worlds/<WORLD>/chunks/<x>_<z>.glb and its record to the registry:
the base chunk and its SHA-256, the edits with the SHA-256 of their scripts and data, the output's SHA-256. A
chunk equal to the one already published (same triangles, UV, normals, colours, materials) is left as it is: the
exact boolean of roof_holes.py orders the cut roof's vertices differently from run to run.

The base: --base, else OPENGOTHIC_BASE, else the newest base in ~/.cache/gothic-ai/base/current/. The base and its
cache are only read. Materials a base world does not have must be keys of the set's materials/materials.json or
of a --materials file under the staging root.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import chunk_canon  # noqa: E402
import chunk_glb  # noqa: E402

REPO = Path(__file__).resolve().parents[3]
REGISTRY = REPO / 'data/content/world-edits.jsonl'
STAGING = Path.home() / '.local/share/gothic-ai/content-staging'
WORK = Path.home() / '.local/share/gothic-ai/world-edit'
BASE_CACHE = Path.home() / '.cache/gothic-ai/base'
BLENDER_SCRIPT = Path(__file__).resolve().parent / 'world_edit_blender.py'
TOOL = 'scripts/content/world/world_edit.py'
BLENDER_VERSION = '5.2'
CHUNK = re.compile(r'[A-Z0-9_]+/-?[0-9]+_-?[0-9]+')


class EditError(RuntimeError):
    pass


def sha256(path: Path) -> str:
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def resolve_base(given: str | None) -> Path:
    if given:
        return Path(given).expanduser()
    if os.environ.get('OPENGOTHIC_BASE'):
        return Path(os.environ['OPENGOTHIC_BASE']).expanduser()
    pointers = sorted((BASE_CACHE / 'current').glob('*.json'), key=lambda p: p.stat().st_mtime, reverse=True)
    for pointer in pointers:
        path = Path(json.loads(pointer.read_text()).get('base', {}).get('path', ''))
        if (path / 'worlds').is_dir():
            return path
    raise EditError('no base: pass --base or set OPENGOTHIC_BASE')


def split_chunk(chunk: str) -> tuple[str, str]:
    if not CHUNK.fullmatch(chunk):
        raise EditError(f'chunk {chunk!r}: expected <WORLD>/<x>_<z>')
    world, name = chunk.split('/')
    return world, name


def base_chunk(base: Path, chunk: str) -> Path:
    world, name = split_chunk(chunk)
    path = base / 'worlds' / world / 'chunks' / f'{name}.glb'
    if not path.is_file():
        raise EditError(f'the base has no chunk {chunk}: {path}')
    return path


def repo_path(path: str | Path) -> str:
    """A path in git, as the registry writes it."""
    p = Path(path)
    p = (p if p.is_absolute() else Path.cwd() / p).resolve()
    if not p.is_relative_to(REPO):
        p = (REPO / path).resolve()
    if not p.is_relative_to(REPO) or not p.is_file():
        raise EditError(f'{path}: an edit must be a file in the repository')
    return p.relative_to(REPO).as_posix()


def staging_path(path: str | Path, staging: Path) -> str:
    p = Path(path).expanduser().resolve()
    if not p.is_relative_to(staging.resolve()) or not p.is_file():
        raise EditError(f'{path}: a materials.json must be a file under the staging root {staging}')
    return p.relative_to(staging.resolve()).as_posix()


def read_registry(path: Path) -> list[dict]:
    if not path.exists():
        return []
    return [json.loads(line) for line in path.read_text().splitlines() if line.strip()]


def write_registry(path: Path, entries: list[dict]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(''.join(json.dumps(e, sort_keys=True, ensure_ascii=False) + '\n' for e in entries))


def record_edits(record: dict) -> list[str]:
    if 'edits' in record:
        return [e['script'] for e in record['edits']]
    return [record['tool']['script']]      # a record of a one-off script before world_edit.py


def blender_binary() -> str:
    for candidate in (os.environ.get('BLENDER'), '/Applications/Blender.app/Contents/MacOS/Blender',
                      shutil.which('blender'), shutil.which('Blender')):
        if candidate and Path(candidate).exists():
            return candidate
    raise EditError('no Blender: set BLENDER')


def run_blender(job: dict, work: Path, gui: bool = False) -> None:
    job_path = work / 'job.json'
    job_path.write_text(json.dumps(job, indent=1) + '\n')
    cmd = [blender_binary()] + ([] if gui else ['-b']) + ['--factory-startup', '--python', str(BLENDER_SCRIPT),
                                                          '--', str(job_path)]
    log = work / 'blender.log'
    with log.open('w') as out:
        result = subprocess.run(cmd, stdout=out, stderr=subprocess.STDOUT)
    text = log.read_text()
    if result.returncode != 0 or 'Traceback' in text or 'SystemExit' in text or \
            (not gui and not Path(job['out']).exists()):
        tail = '\n'.join(text.splitlines()[-25:])
        raise EditError(f'Blender failed (log {log}):\n{tail}')


def describe_edits(scripts: list[str], allowed: dict, meta: dict) -> list[dict]:
    """The registry's list of edits: script, its SHA-256, PARAMETERS, data files with SHA-256, declared edges."""
    declared: dict[str, set] = {}
    for e in allowed['edits']:
        declared.setdefault(e['edit'], set()).update(chunk_glb.edge_keys_from_floats(e['edges']))
    out = []
    for script in scripts:
        row = {'script': script, 'sha256': sha256(REPO / script)}
        meta_row = meta.get(script, {})
        if meta_row.get('parameters'):
            row['parameters'] = meta_row['parameters']
        if meta_row.get('inputs'):
            row['inputs'] = [{'path': p, 'sha256': sha256(REPO / p)} for p in meta_row['inputs']]
        row['open_edges_declared'] = len(declared.get(script, ()))
        out.append(row)
    return out


def git_head() -> str:
    return subprocess.run(['git', '-C', str(REPO), 'rev-parse', 'HEAD'], capture_output=True, text=True,
                          check=True).stdout.strip()


def allowed_edges(allowed: dict) -> set[tuple]:
    out: set[tuple] = set()
    for e in allowed['edits']:
        out |= chunk_glb.edge_keys_from_floats(e['edges'])
    return out


def cmd_take(a) -> int:
    base = resolve_base(a.base)
    src = base_chunk(base, a.chunk)
    world, name = split_chunk(a.chunk)
    work = Path(a.work).expanduser() if a.work else WORK / f'{world}_{name}'
    work.mkdir(parents=True, exist_ok=True)
    dst = work / 'base.glb'
    shutil.copyfile(src, dst)
    print(f'{a.chunk}: {dst} (sha256 {sha256(dst)})')
    if a.gui:
        run_blender({'base': str(dst), 'out': str(work / 'out.glb'), 'allowed': str(work / 'allowed.json'),
                     'meta': str(work / 'meta.json'), 'world': world, 'edits': [], 'gui': True, 'repo': str(REPO)},
                    work, gui=True)
    return 0


def cmd_build(a) -> int:
    registry_path = Path(a.registry)
    staging = Path(a.staging).expanduser()
    entries = read_registry(registry_path)
    by_id = {e['id']: e for e in entries}
    record = by_id.get(a.id)
    if record is not None:
        chunk = record['replaces'][0]
        published = Path(record['files']['glb']['path'])
        set_name = published.parts[0]
        if (a.set and a.set != set_name) or (a.chunk and a.chunk != chunk):
            raise EditError(f'{a.id} is {set_name} {chunk}: a record keeps its set and chunk')
        edits = record_edits(record)
    else:
        if not (a.set and a.chunk):
            raise EditError(f'{a.id} is not in {registry_path}: a new record needs --set and --chunk')
        chunk, set_name, edits = a.chunk, a.set, []
    if a.from_id:
        source = by_id.get(a.from_id)
        if source is None:
            raise EditError(f'--from {a.from_id}: no such record')
        if source['replaces'][0] != chunk:
            raise EditError(f'--from {a.from_id} edits {source["replaces"][0]}, not {chunk}')
        edits = record_edits(source)
    edits = edits + [repo_path(e) for e in a.edit or []]
    if not edits:
        raise EditError('no edits: pass --edit')
    if len(set(edits)) != len(edits):
        raise EditError(f'an edit appears twice in {", ".join(edits)}')
    for e in edits:
        if not (REPO / e).is_file():
            raise EditError(f'edit {e} of {a.id} is not in the repository')
    world, name = split_chunk(chunk)

    base = resolve_base(a.base)
    src = base_chunk(base, chunk)
    work = Path(a.work).expanduser() if a.work else WORK / a.id
    work.mkdir(parents=True, exist_ok=True)
    base_copy = work / 'base.glb'
    shutil.copyfile(src, base_copy)
    base_sha = sha256(base_copy)
    if record is not None and record['inputs'][0]['sha256'] != base_sha:
        print(f'note: the base chunk changed since the record ({record["inputs"][0]["sha256"][:12]} -> {base_sha[:12]})')

    out = work / 'out.glb'
    out.unlink(missing_ok=True)
    job = {'base': str(base_copy), 'out': str(out), 'allowed': str(work / 'allowed.json'),
           'meta': str(work / 'meta.json'), 'world': world, 'gui': bool(a.gui), 'repo': str(REPO),
           'chunk': name, 'base_chunks': str(src.parent),
           'edits': [str(REPO / e) for e in edits], 'names': {str(REPO / e): e for e in edits}}
    print(f'{a.id}: {chunk} of {base.name}, edits {", ".join(edits)}')
    run_blender(job, work, gui=a.gui)
    if a.gui:
        return 0
    canon = chunk_canon.canonical(base_copy, out, out)
    print(f'source triangles written back from the base: {canon["kept"]} of {canon["source"]}; new or changed: '
          f'{canon["changed"]}')

    allowed = json.loads((work / 'allowed.json').read_text())
    materials = list(a.materials or [])
    if record is not None and not materials:
        materials = [str(staging / p) for p in record.get('materials_from', [])]
    own = staging / set_name / 'materials' / 'materials.json'
    indexes = []
    for index in ([own] if own.exists() else []) + [Path(m).expanduser() for m in materials]:
        if index.resolve() not in [i.resolve() for i in indexes]:
            indexes.append(index)
    report = chunk_glb.check(src, out, allowed_open=allowed_edges(allowed), material_indexes=indexes,
                             world_json=src.parent.parent / 'world.json')
    (work / 'report.json').write_text(json.dumps({'problems': report.problems, **report.summary}, indent=1) + '\n')
    print(chunk_glb.format_report(report))
    if not report.ok:
        print(f'{a.id}: the check failed, nothing published (work folder {work})', file=sys.stderr)
        return 1
    if a.dry_run:
        print(f'{a.id}: checked, not published (--dry-run): {out}')
        return 0

    target = staging / set_name / 'worlds' / world / 'chunks' / f'{name}.glb'
    target.parent.mkdir(parents=True, exist_ok=True)
    if target.exists() and not chunk_glb.same_geometry(target, out, full=True):
        print(f'{target}: the same chunk as published, left as it is')
    else:
        shutil.copyfile(out, target)
        print(f'{target}: published')

    meta = json.loads((work / 'meta.json').read_text())
    entry = dict(record or {})
    entry.update({
        'id': a.id, 'kind': 'worldchunk', 'replaces': [chunk], 'pipeline': 'blender-world-edit', 'models': [],
        'prompt': '', 'negative': '', 'seed': None, 'status': 'draft',
        'inputs': [{'input': f'converted world mesh {chunk}', 'sha256': base_sha}],
        'tool': {'script': TOOL, 'commit': git_head()},
        'edits': describe_edits(edits, allowed, meta),
        'parameters': {'blender': BLENDER_VERSION},
        'check': report.summary,
        'files': {'glb': {'path': target.relative_to(staging).as_posix(), 'sha256': sha256(target)}},
        'utc': datetime.now(timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ'),
    })
    # every materials.json the check read, the set's own one too
    if indexes:
        entry['materials_from'] = [staging_path(m, staging) for m in indexes]
    else:
        entry.pop('materials_from', None)
    if a.notes:
        entry['notes'] = a.notes
    sys.path.insert(0, str(REPO / 'scripts/content'))
    import registry
    registry.validate_entry(entry, content_root=staging)
    if record is None:
        entries.append(entry)
    else:
        entries = [entry if e['id'] == a.id else e for e in entries]
    write_registry(registry_path, entries)
    print(f'{a.id}: recorded in {registry_path}')
    return 0


def cmd_check(a) -> int:
    allowed = allowed_edges(json.loads(Path(a.allowed).read_text())) if a.allowed else set()
    report = chunk_glb.check(a.base, a.edited, allowed_open=allowed,
                             material_indexes=[Path(m) for m in a.materials or []])
    print(chunk_glb.format_report(report))
    return 0 if report.ok else 1


def cmd_compare(a) -> int:
    diff = chunk_glb.same_geometry(a.a, a.b, full=a.full)
    print('\n'.join(diff) if diff else 'the same triangles, UV and materials' + (', normals, colours' if a.full else ''))
    return 1 if diff else 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='command', required=True)
    t = sub.add_parser('take', help='copy a base chunk to a work folder')
    t.add_argument('chunk')
    t.add_argument('--base')
    t.add_argument('--work')
    t.add_argument('--gui', action='store_true', help='open it in Blender with the underlay')
    b = sub.add_parser('build', help='apply the edits of a record, check, publish')
    b.add_argument('id')
    b.add_argument('--set')
    b.add_argument('--chunk')
    b.add_argument('--from', dest='from_id')
    b.add_argument('--edit', action='append')
    b.add_argument('--materials', action='append')
    b.add_argument('--notes')
    b.add_argument('--base')
    b.add_argument('--staging', default=str(STAGING))
    b.add_argument('--registry', default=str(REGISTRY))
    b.add_argument('--work')
    b.add_argument('--dry-run', action='store_true', help='build and check in the work folder only')
    b.add_argument('--gui', action='store_true', help='open the edited chunk in Blender with the underlay')
    c = sub.add_parser('check', help='check an edited chunk against its base chunk')
    c.add_argument('base')
    c.add_argument('edited')
    c.add_argument('--allowed')
    c.add_argument('--materials', action='append')
    m = sub.add_parser('compare', help='are two chunks the same geometry')
    m.add_argument('a')
    m.add_argument('b')
    m.add_argument('--full', action='store_true')
    a = ap.parse_args(argv)
    try:
        return {'take': cmd_take, 'build': cmd_build, 'check': cmd_check, 'compare': cmd_compare}[a.command](a)
    except EditError as error:
        print(f'world_edit: {error}', file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main())

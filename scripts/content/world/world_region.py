"""world_edit.py build-region: edit the chunks of a rectangle of the converted world mesh as one surface.

    python3 scripts/content/world/world_edit.py build-region <id> --set <set> --region <WORLD>/<x0>,<z0>,<x1>,<z1> \\
        [--edit <script> ...] [--materials <materials.json> ...] [--dry-run] [--gui]

The rectangle is inclusive chunk coordinates; its cells without a base chunk are skipped. Blender
(world_region_bpy.py) joins the chunks into one mesh, runs the edits on it with the joined original as an
underlay, bakes the light of new vertices from the base around them over the whole region and puts every face back
into its chunk (the face attribute region_chunk, which children of a face inherit). Then each chunk is written back
in the converter's form (chunk_canon.py) and checked against its own base chunk (chunk_glb.check): border vertices
only towards the chunks outside the region, vertex light, source triangles, materials. The open edges are judged
over the union of the region's chunks against the union of their base chunks, welded by position: an inner border
edge split on both sides passes, split on one side only it is a T-junction and fails. An open edge of the base
(the free side of a tree crown, of duckweed) that the edits moved or split along new vertices passes, unless it is
the region's outer border. One failing chunk fails the build, and nothing is published.

Two things Blender loses are put back: the triangles it cannot hold (a corner twice, or the same corners as another
triangle: the weld makes the two sides of a double-sided pair one face, 7 264 triangles on the pilot), where the
edits took them (world_region_dropped.py, chunk_dropped.py), and the material records as the
converter wrote them (base_materials: Blender writes [9.99999975e-05, 0] as [9.99999975e-05, 0.0]). So an edit that
changes nothing gives every chunk equal to its base.

A passing chunk the edits changed goes to <staging>/<set>/worlds/<WORLD>/chunks/<x>_<z>.glb (left as it is when it
is the same as the published one); a chunk equal to its base is not published. The region's record in the registry
(kind worldregion) holds the region, the set, the edits and per chunk the base chunk's SHA-256, the output with its
SHA-256 and the check. A chunk of a set belongs to one record: a region over a chunk another record of the set
holds fails, and so does build on a chunk of a region. build-region <id> of an existing record rebuilds it, --edit
appends to its list; a rebuild that changes nothing leaves the record as it is.
"""
from __future__ import annotations

from datetime import datetime, timezone
import json
from pathlib import Path
import re
import shutil
import struct
import sys
import time

import chunk_canon
import chunk_dropped
import chunk_glb
import world_edit
from edit_error import EditError

KIND = 'worldregion'
REGION = re.compile(r'([A-Z0-9_]+)/(-?[0-9]+),(-?[0-9]+),(-?[0-9]+),(-?[0-9]+)')


def parse_region(text: str) -> tuple[str, tuple[int, int, int, int]]:
    m = REGION.fullmatch(text or '')
    if not m:
        raise EditError(f'region {text!r}: expected <WORLD>/<x0>,<z0>,<x1>,<z1>')
    x0, z0, x1, z1 = (int(v) for v in m.groups()[1:])
    if x0 > x1 or z0 > z1:
        raise EditError(f'region {text}: expected x0 <= x1 and z0 <= z1')
    return m.group(1), (x0, z0, x1, z1)


def region_chunks(chunks_dir: Path, rect) -> list[str]:
    x0, z0, x1, z1 = rect
    return [f'{x}_{z}' for x in range(x0, x1 + 1) for z in range(z0, z1 + 1) if (chunks_dir / f'{x}_{z}.glb').is_file()]


def outside_ids(chunks_dir: Path, names: list[str]) -> dict[str, set[int]]:
    """Per chunk of the region: the _VERTEX ids of its neighbour chunks outside the region (its outer border)."""
    inside, cache, out = set(names), {}, {}
    for name in names:
        x, z = chunk_glb.chunk_coords(name)
        ids: set[int] = set()
        for dx in (-1, 0, 1):
            for dz in (-1, 0, 1):
                other = f'{x + dx}_{z + dz}'
                path = chunks_dir / f'{other}.glb'
                if other in inside or not path.exists():
                    continue
                if other not in cache:
                    _, vid = chunk_glb.read_chunk(path).corners()
                    cache[other] = {int(v) for v in set(vid.tolist()) if v >= 0}
                ids |= cache[other]
        out[name] = ids
    return out


def record_set(entry: dict) -> str | None:
    if entry.get('kind') == KIND:
        return entry.get('set')
    path = entry.get('files', {}).get('glb', {})
    return Path(path['path']).parts[0] if isinstance(path, dict) and path.get('path') else None


def held_chunks(entry: dict) -> set[str]:
    if entry.get('kind') == KIND:
        return {c['chunk'] for c in entry.get('chunks', [])}
    return set(entry.get('replaces', []))


def check_owner(entries: list[dict], set_name: str, chunks: list[str], own_id: str) -> None:
    """A chunk of a set belongs to one record: fail naming the other record that holds one of these chunks."""
    for e in entries:
        if e['id'] == own_id or record_set(e) != set_name:
            continue
        both = sorted(held_chunks(e) & set(chunks))
        if both:
            raise EditError(f'{", ".join(both)} of {set_name} belong{"s" if len(both) == 1 else ""} to the record '
                            f'{e["id"]} ({e.get("kind")}): a chunk of a set belongs to one record')


def union(paths) -> chunk_glb.Chunk:
    out = chunk_glb.Chunk(Path('region'))
    for p in paths:
        out.primitives += chunk_glb.read_chunk(p).primitives
    return out


def _ids_at(chunk: chunk_glb.Chunk) -> dict[tuple, set[int]]:
    """Exact position -> the _VERTEX ids of the corners there (-1 for a new vertex)."""
    pos, vid = chunk.corners()
    out: dict[tuple, set[int]] = {}
    for k, v in zip(chunk_glb._key_tuples(chunk_glb.position_keys(pos)), vid):
        out.setdefault(k, set()).add(int(v))
    return out


def _followed(new: set[tuple], base_open: set[tuple], base: chunk_glb.Chunk, edited: chunk_glb.Chunk,
              outer: set[int]) -> set[tuple]:
    """The new open edges that are an open edge of the base the edits moved or split: between the same two base
    vertices (by _VERTEX), directly or along a chain of new vertices. Not the region's outer border (both ends shared
    with a chunk outside): it is open only because the region ends there."""
    base_ids, ids = _ids_at(base), _ids_at(edited)
    pairs = {frozenset((x, y)) for a, b in base_open for x in base_ids.get(a, ()) for y in base_ids.get(b, ())
             if x >= 0 and y >= 0 and not (x in outer and y in outer)}
    old = {k for k, v in ids.items() if any(i >= 0 for i in v)}
    links: dict[tuple, list[tuple]] = {}
    for e in new:
        for k in e:
            links.setdefault(k, []).append(e)
    followed: set[tuple] = set()
    for start in (k for k in links if k in old):
        for first in links[start]:
            chain, at, edge = [first], start, first
            while True:
                at = edge[1] if edge[0] == at else edge[0]
                if at in old or len(links[at]) != 2:
                    break
                edge = links[at][0] if links[at][1] == edge else links[at][1]
                chain.append(edge)
            if at in old and any(frozenset((x, y)) in pairs for x in ids[start] for y in ids[at]):
                followed.update(chain)
    return followed


def union_open_edges(bases, editeds, allowed: set[tuple] = frozenset(),
                     outer: set[int] = frozenset()) -> tuple[dict, list[str]]:
    """The open-edge rule over a region: an edge of the edited chunks together that has one triangle (welded by exact
    position) is new if the base chunks together do not have it open, no edit declared it and it is not an open edge
    of the base the edits moved or split (_followed). outer: the _VERTEX ids shared with chunks outside the region."""
    base, edited = union(bases), union(editeds)
    base_open, out_open = chunk_glb.open_edges(base), chunk_glb.open_edges(edited)
    new = out_open - base_open - set(allowed)
    followed = _followed(new, base_open, base, edited, set(outer))
    new = sorted(new - followed)
    problems = [f'new open edge of the region {chunk_glb._floats(a)} - {chunk_glb._floats(b)}' for a, b in new[:20]]
    if len(new) > 20:
        problems.append(f'... {len(new) - 20} more new open edges of the region')
    return {'base': len(base_open), 'edited': len(out_open), 'declared': len(allowed), 'followed': len(followed),
            'new': len(new)}, problems


def base_materials(out: Path, records: dict[str, dict]) -> None:
    """Give out's materials the base's records (name and extras.gothic as the converter wrote them): Blender's custom
    properties turn [9.99999975e-05, 0] into [9.99999975e-05, 0.0], so a chunk the edits left was not its base."""
    doc, binary = chunk_glb.read_glb(out)
    materials = [records.get(m['name'], m) for m in doc['materials']]
    if json.dumps(materials) == json.dumps(doc['materials']):     # 0 == 0.0 in Python, not in the file
        return
    doc['materials'] = materials
    text = json.dumps(doc).encode()
    text += b' ' * (-len(text) % 4)
    binary += b'\0' * (-len(binary) % 4)
    head = struct.pack('<4sII', b'glTF', 2, 12 + 8 + len(text) + 8 + len(binary))
    out.write_bytes(head + struct.pack('<II', len(text), 0x4E4F534A) + text +
                    struct.pack('<II', len(binary), 0x004E4942) + binary)


def material_indexes(a, staging: Path, set_name: str, record: dict | None) -> list[Path]:
    materials = list(a.materials or [])
    if record is not None and not materials:
        materials = [str(staging / p) for p in record.get('materials_from', [])]
    own = staging / set_name / 'materials' / 'materials.json'
    indexes = []
    for index in ([own] if own.exists() else []) + [Path(m).expanduser() for m in materials]:
        if index.resolve() not in [i.resolve() for i in indexes]:
            indexes.append(index)
    return indexes


def cmd_build_region(a) -> int:
    registry_path = Path(a.registry)
    staging = Path(a.staging).expanduser()
    entries = world_edit.read_registry(registry_path)
    record = next((e for e in entries if e['id'] == a.id), None)
    if record is not None:
        if record.get('kind') != KIND:
            raise EditError(f'{a.id} is a record of one chunk: use build')
        region, set_name = record['region'], record['set']
        if (a.set and a.set != set_name) or (a.region and a.region != region):
            raise EditError(f'{a.id} is {set_name} {region}: a record keeps its set and region')
        edits = world_edit.record_edits(record)
    else:
        if not (a.set and a.region):
            raise EditError(f'{a.id} is not in {registry_path}: a new record needs --set and --region')
        region, set_name, edits = a.region, a.set, []
    world, rect = parse_region(region)
    edits = edits + [world_edit.repo_path(e) for e in a.edit or []]
    if not edits:
        raise EditError('no edits: pass --edit')
    if len(set(edits)) != len(edits):
        raise EditError(f'an edit appears twice in {", ".join(edits)}')
    for e in edits:
        if not (world_edit.REPO / e).is_file():
            raise EditError(f'edit {e} of {a.id} is not in the repository')

    base = world_edit.resolve_base(a.base)
    chunks_dir = base / 'worlds' / world / 'chunks'
    names = region_chunks(chunks_dir, rect)
    if not names:
        raise EditError(f'the base has no chunk in {region}')
    check_owner(entries, set_name, [f'{world}/{n}' for n in names], a.id)
    work = Path(a.work).expanduser() if a.work else world_edit.WORK / a.id
    for folder in ('base', 'out'):
        (work / folder).mkdir(parents=True, exist_ok=True)
    srcs = [chunks_dir / f'{n}.glb' for n in names]
    copies = [work / 'base' / f'{n}.glb' for n in names]
    outs = [work / 'out' / f'{n}.glb' for n in names]
    for src, copy, out in zip(srcs, copies, outs):
        shutil.copyfile(src, copy)
        out.unlink(missing_ok=True)
    base_sha = {n: world_edit.sha256(c) for n, c in zip(names, copies)}
    if record is not None:
        old = {c['chunk']: c['base_sha256'] for c in record['chunks']}
        changed = [n for n in names if old.get(f'{world}/{n}') != base_sha[n]]
        if changed:
            print(f'note: the base chunks {", ".join(changed)} changed since the record')
    outside = outside_ids(chunks_dir, names)

    result_path, dropped_path = work / 'region.json', work / 'dropped.json'
    result_path.unlink(missing_ok=True)
    job = {'region': region, 'chunks': names, 'bases': [str(c) for c in copies], 'outs': [str(o) for o in outs],
           'outside': sorted(set().union(*outside.values())), 'out': str(result_path), 'dropped': str(dropped_path),
           'allowed': str(work / 'allowed.json'), 'meta': str(work / 'meta.json'), 'world': world,
           'gui': bool(a.gui), 'repo': str(world_edit.REPO), 'base_chunks': str(chunks_dir),
           'edits': [str(world_edit.REPO / e) for e in edits], 'names': {str(world_edit.REPO / e): e for e in edits}}
    print(f'{a.id}: {region} ({len(names)} chunks) of {base.name}, edits {", ".join(edits)}')
    start = time.perf_counter()
    world_edit.run_blender(job, work, gui=a.gui)
    if a.gui:
        return 0
    result = json.loads(result_path.read_text())
    print(f'Blender: {time.perf_counter() - start:.1f} s, {result["seconds"]} s in the script, peak memory '
          f'{result["peak_rss_mb"]} MB; {result["welded"]} vertices welded ({result["feature_seams"]} feature seams '
          f'closed), {result["border_vertices"]} on the outer border, {result["dropped"]} triangles Blender cannot hold')
    if result['problems']:
        print('\n'.join(f'FAIL: {p}' for p in result['problems']))
        print(f'{a.id}: the edits failed, nothing published (work folder {work})', file=sys.stderr)
        return 1

    allowed = json.loads((work / 'allowed.json').read_text())
    declared = world_edit.allowed_edges(allowed)
    indexes = material_indexes(a, staging, set_name, record)
    lost_triangles = json.loads(dropped_path.read_text())
    records = {}
    for src in srcs:
        for m in chunk_glb.read_glb(src)[0]['materials']:
            records.setdefault(m['name'], {'name': m['name'], 'extras': m.get('extras', {})})
    reports, ok, lost, total = {}, True, [], {}
    print('per chunk the open edges are counted; the region judges them over all its chunks (below)')
    canons = [chunk_canon.canonical(src, out, out) for src, out in zip(srcs, outs)]
    fresh = chunk_dropped.new_corners(outs)
    for name, src, out, canon in zip(names, srcs, outs, canons):
        back, problems = chunk_dropped.write(src, out, [e for e in lost_triangles if e['chunk'] == name], fresh)
        lost += [f'{world}/{name}: {p}' for p in problems]
        total = {k: total.get(k, 0) + v for k, v in back.items()}
        base_materials(out, records)
        report = chunk_glb.check(src, out, allowed_open=declared, material_indexes=indexes,
                                 world_json=chunks_dir.parent / 'world.json', shared=outside[name],
                                 judge_open_edges=False)
        reports[name] = report
        ok &= report.ok
        print(f'{world}/{name}: source triangles from the base {canon["kept"]} of {canon["source"]}, new or changed '
              f'{canon["changed"]}; Blender cannot hold {back["unchanged"]} as they were, {back["moved"]} moved as '
              f'{back["triangles"]} triangles; ' + chunk_glb.format_report(report))
    edges, problems = union_open_edges(srcs, outs, declared, set().union(*outside.values()))
    problems = lost + problems
    print(f'triangles Blender cannot hold: {total.get("unchanged", 0)} written back as they were, '
          f'{total.get("moved", 0)} moved with their corners as {total.get("triangles", 0)} triangles')
    print(f'region open edges: base {edges["base"]}, edited {edges["edited"]}, declared {edges["declared"]}, '
          f'base open edges moved or split {edges["followed"]}, new {edges["new"]}')
    for p in problems:
        print(f'FAIL: {p}')
    (work / 'report.json').write_text(json.dumps({
        'problems': [p for r in reports.values() for p in r.problems] + problems, 'open_edges': edges,
        'chunks': {n: r.summary for n, r in reports.items()}}, indent=1) + '\n')
    if not ok or problems:
        print(f'{a.id}: the check failed, nothing published (work folder {work})', file=sys.stderr)
        return 1
    changed = [n for n, src, out in zip(names, srcs, outs) if chunk_glb.same_geometry(src, out, full=True)]
    print(f'changed chunks: {", ".join(changed) or "none"}; the same as the base: {len(names) - len(changed)}')
    if a.dry_run:
        print(f'{a.id}: checked, not published (--dry-run): {work / "out"}')
        return 0

    published = {c['chunk']: c['output']['path'] for c in (record or {}).get('chunks', []) if c.get('output')}
    rows, stale = [], []      # chunks the edits no longer change: removed once the record no longer lists them
    for name, out in zip(names, outs):
        chunk = f'{world}/{name}'
        target = staging / set_name / 'worlds' / world / 'chunks' / f'{name}.glb'
        row = {'chunk': chunk, 'base_sha256': base_sha[name], 'check': reports[name].summary}
        if name in changed:
            target.parent.mkdir(parents=True, exist_ok=True)
            if target.exists() and not chunk_glb.same_geometry(target, out, full=True):
                print(f'{target}: the same chunk as published, left as it is')
            else:
                shutil.copyfile(out, target)
                print(f'{target}: published')
            row['output'] = {'path': target.relative_to(staging).as_posix(), 'sha256': world_edit.sha256(target)}
        elif chunk in published and (staging / published[chunk]).exists():
            stale.append(staging / published[chunk])
        rows.append(row)

    meta = json.loads((work / 'meta.json').read_text())
    entry = dict(record or {})
    entry.update({
        'id': a.id, 'kind': KIND, 'set': set_name, 'region': region,
        'replaces': [r['chunk'] for r in rows if 'output' in r], 'pipeline': 'blender-world-edit', 'models': [],
        'prompt': '', 'negative': '', 'seed': None, 'status': 'draft',
        'tool': {'script': world_edit.TOOL, 'commit': world_edit.git_head()},
        'edits': world_edit.describe_edits(edits, allowed, meta),
        'parameters': {'blender': world_edit.BLENDER_VERSION},
        'chunks': rows, 'check': {'open_edges': edges},
        'files': {'glb': [r['output'] for r in rows if 'output' in r]},
        'utc': datetime.now(timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ'),
    })
    if indexes:
        entry['materials_from'] = [world_edit.staging_path(m, staging) for m in indexes]
    else:
        entry.pop('materials_from', None)
    if a.notes:
        entry['notes'] = a.notes
    sys.path.insert(0, str(world_edit.REPO / 'scripts/content'))
    import registry
    registry.validate_entry(entry, content_root=staging)
    if record is not None and {**entry, 'utc': None} == {**record, 'utc': None}:
        print(f'{a.id}: the same as its record, the registry is left as it is')
        return 0
    entries = [entry if e['id'] == a.id else e for e in entries] if record is not None else entries + [entry]
    world_edit.write_registry(registry_path, entries)
    print(f'{a.id}: recorded in {registry_path}')
    for path in stale:
        path.unlink()
        print(f'{path}: the edits no longer change it, removed')
    return 0

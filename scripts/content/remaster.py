#!/usr/bin/env python3
"""Remaster pipeline bookkeeping: one asset folder, its sources, reviews and decisions.

The process is described in docs/remaster-pipeline.md; Claude follows it with the
`remaster` skill. This tool keeps the artifacts honest: immutable sources with
SHA-256 and C2PA provenance, a backup check, reviewer packets in both orders with
the key kept outside the packet, merged verdicts, the user's decisions verbatim and
a gate check before each stage.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
MODEL_MAPS = {'normal', 'roughness', 'ao'}   # maps a model bake may bring with its albedo
TEMPLATES = ROOT / '.claude/skills/remaster/templates'
DEFAULTS = ROOT / 'docs/remaster-defaults.md'
MAX_EDGE = 2576
MAX_TOKENS = 4784
PATCH = 28
STAGES = ('intent', 'concept', 'textures', 'build', 'ingame', 'done')
REVIEW_STAGES = ('concept', 'textures', 'ingame', 'calibration')
GATE_VERDICTS = ('approved', 'rejected', 'changes')
CRITERIA = ('silhouette', 'main_feature', 'crookedness_wear', 'tone_light', 'material', 'coherence')
ATTEMPT = re.compile(r'(?P<stem>.+)-attempt-(?P<n>\d+)\Z')


def data_root() -> Path:
    return Path(os.environ.get('GOTHIC_AI_DATA', Path.home() / '.local/share/gothic-ai'))


def assets_root() -> Path:
    return Path(os.environ.get('REMASTER_ASSETS', ROOT / 'reports/content/remaster'))


def sources_root() -> Path:
    return data_root() / 'content-sources'


def work_root(asset: str) -> Path:
    return data_root() / 'remaster' / asset


def asset_dir(asset: str) -> Path:
    if not re.fullmatch(r'[a-z0-9]+(?:-[a-z0-9]+)*', asset):
        raise ValueError(f'asset id must be a lowercase slug: {asset!r}')
    return assets_root() / asset


def today() -> str:
    return datetime.now(timezone.utc).strftime('%Y-%m-%d')


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b''):
            digest.update(chunk)
    return digest.hexdigest()


def load_manifest(asset: str) -> dict:
    path = asset_dir(asset) / 'asset.json'
    if not path.is_file():
        raise ValueError(f'{asset}: no asset.json; run `remaster.py new {asset}` first')
    return json.loads(path.read_text())


def save_manifest(asset: str, manifest: dict) -> None:
    path = asset_dir(asset) / 'asset.json'
    tmp = path.with_suffix('.json.tmp')
    tmp.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + '\n')
    tmp.replace(path)


# --- provenance ---------------------------------------------------------------------------

def _cbor_text(blob: bytes, key: bytes, start: int) -> str | None:
    """Text value that follows a short CBOR text key, e.g. b'name' -> 'ChatGPT'."""
    marker = bytes([0x60 + len(key)]) + key
    i = blob.find(marker, start)
    if i < 0 or i - start > 400:
        return None
    j = i + len(marker)
    if j >= len(blob) or not 0x60 <= blob[j] <= 0x77:
        return None
    n = blob[j] - 0x60
    try:
        return blob[j + 1:j + 1 + n].decode('utf-8')
    except UnicodeDecodeError:
        return None


def c2pa_agent(path: Path) -> dict | None:
    """softwareAgent name/version from the PNG's C2PA (caBX) chunk, if any."""
    data = path.read_bytes()
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        return None
    i = 8
    while i + 8 <= len(data):
        length = int.from_bytes(data[i:i + 4], 'big')
        kind = data[i + 4:i + 8]
        if kind == b'caBX':
            blob = data[i + 8:i + 8 + length]
            at = blob.find(b'softwareAgent')
            if at < 0:
                return {'agent': None, 'version': None}
            return {'agent': _cbor_text(blob, b'name', at), 'version': _cbor_text(blob, b'version', at)}
        if kind == b'IEND':
            break
        i += 12 + length
    return None


def model_label(c2pa: dict | None) -> str:
    if not c2pa:
        return 'unknown (no C2PA)'
    return ' '.join(part for part in (c2pa.get('agent'), c2pa.get('version')) if part) or 'unknown'


# --- new / register / import ----------------------------------------------------------------

def fill(text: str, **values: str) -> str:
    for key, value in values.items():
        text = text.replace('{' + key + '}', value)
    return text


def cmd_new(args) -> int:
    folder = asset_dir(args.asset)
    if folder.exists():
        raise ValueError(f'{folder} already exists')
    folder.mkdir(parents=True)
    (folder / 'reviews').mkdir()
    values = dict(asset=args.asset, title=args.title, captures='—', measurement='—')
    for name in ('intent.md', 'concept.md', 'decisions.md'):
        (folder / name).write_text(fill((TEMPLATES / name).read_text(), **values))
    manifest = dict(asset=args.asset, title=args.title, kind=args.kind, created=today(),
        goal='the same Gothic, better made', namespaces=[args.asset], sources=[], selected={},
        reviews=[], gates=[], build=None, backup=None)
    save_manifest(args.asset, manifest)
    print(folder)
    return 0


def _source_entry(png: Path, meta: dict, namespace: str) -> dict:
    match = ATTEMPT.fullmatch(png.stem)
    if match is None:
        raise ValueError(f'{png.name}: expected <stem>-attempt-<n>.png')
    texture = meta.get('texture')
    role = meta.get('role') or ('texture' if texture else 'concept')
    c2pa = c2pa_agent(png)
    return dict(file=f'{namespace}/{png.name}', role=role, texture=texture, attempt=int(match['n']),
        sha256=sha256(png), c2pa=c2pa, model=model_label(c2pa), utc=meta.get('utc'),
        thread=meta.get('thread'), parent=meta.get('parent') or meta.get('reference_image'))


def cmd_register(args) -> int:
    manifest = load_manifest(args.asset)
    folder = sources_root() / args.namespace
    if not folder.is_dir():
        raise ValueError(f'{folder}: no such source namespace')
    known = {s['file'] for s in manifest['sources']}
    added = 0
    for png in sorted(folder.glob('*.png')):
        meta_path = png.with_suffix('.json')
        if not meta_path.is_file():
            raise ValueError(f'{png}: missing JSON metadata')
        entry = _source_entry(png, json.loads(meta_path.read_text()), args.namespace)
        if entry['file'] not in known:
            manifest['sources'].append(entry)
            added += 1
    if args.namespace not in manifest['namespaces']:
        manifest['namespaces'].append(args.namespace)
    save_manifest(args.asset, manifest)
    print(f'{args.asset}: registered {added} sources from {args.namespace}')
    return 0


def cmd_import(args) -> int:
    manifest = load_manifest(args.asset)
    namespace = args.namespace or args.asset
    folder = sources_root() / namespace
    folder.mkdir(parents=True, exist_ok=True)
    source = Path(args.png)
    raw = source.read_bytes()
    if raw[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError(f'{source}: expected a PNG from image_gen')
    # A shared material keeps one concept image per texture: concept-<TEXTURE>-attempt-<n>.png.
    stem = (Path(args.texture).stem if args.role == 'texture'
            else f'concept-{Path(args.texture).stem}' if args.texture else args.role)
    if args.role == 'texture' and not args.texture:
        raise ValueError('--texture is required for --role texture')
    numbers = [int(m['n']) for p in folder.glob(f'{stem}-attempt-*.png') if (m := ATTEMPT.fullmatch(p.stem))]
    attempt = max(numbers, default=0) + 1
    target = folder / f'{stem}-attempt-{attempt}.png'
    prompt = Path(args.prompt_file).read_text().strip()
    start = datetime.fromisoformat(args.start.replace('Z', '+00:00'))
    end = datetime.fromisoformat(args.end.replace('Z', '+00:00'))
    maps = dict(item.split('=', 1) for item in args.map or [])
    if maps and not args.tool:
        raise ValueError('--map needs --tool: only our own model bakes carry their own maps')
    unknown = set(maps) - MODEL_MAPS
    if unknown:
        raise ValueError(f'--map: unknown map {sorted(unknown)}, expected {sorted(MODEL_MAPS)}')
    target.write_bytes(raw)
    c2pa = c2pa_agent(target)
    copies = []
    for name, path in sorted(maps.items()):
        copy = target.with_name(f'{target.stem}.{name}.png')
        copy.write_bytes(Path(path).read_bytes())
        copies.append(copy)
    meta = dict(texture=args.texture, role=args.role, attempt=attempt,
        prompt=prompt, utc=args.start, utc_end=args.end, seconds=max(0, round((end - start).total_seconds())),
        generator='image_gen in a fresh Codex thread (ChatGPT plan); judged by Claude', thread=args.thread,
        parent=args.parent, reason=args.reason, c2pa=c2pa, source_sha256=sha256(target), tool=args.tool)
    if copies:
        meta['maps'] = {name: dict(file=f'{namespace}/{copy.name}', sha256=sha256(copy))
                        for name, copy in zip(sorted(maps), copies)}
    target.with_suffix('.json').write_text(json.dumps(meta, ensure_ascii=False, indent=2) + '\n')
    for path in (target, target.with_suffix('.json'), *copies):
        path.chmod(0o444)
    entry = _source_entry(target, meta, namespace)
    if args.tool:
        # Assembled by our code from image_gen sources (the parents); it has no C2PA of its own.
        entry.update(tool=args.tool, model=f'derived: {args.tool}')
    if copies:
        entry['maps'] = meta['maps']
    manifest['sources'].append(entry)
    if namespace not in manifest['namespaces']:
        manifest['namespaces'].append(namespace)
    models = {s['model'] for s in manifest['sources'] if not s.get('tool')}
    save_manifest(args.asset, manifest)
    print(f'{target} ({entry["model"]})')
    if len(models) > 1:
        print(f'warning: {args.asset} now mixes image models: {sorted(models)}; '
              'stop and decide before continuing (docs/remaster-pipeline.md)', file=sys.stderr)
    return 0


def cmd_select(args) -> int:
    manifest = load_manifest(args.asset)
    files = {s['file'] for s in manifest['sources']}
    if args.file not in files:
        raise ValueError(f'{args.file}: not a registered source of {args.asset}')
    manifest['selected'][args.key] = dict(file=args.file, reason=args.reason, date=today())
    save_manifest(args.asset, manifest)
    return 0


# --- backup -------------------------------------------------------------------------------

def compare_digests(local: dict[str, str], remote_text: str) -> tuple[int, list[str]]:
    remote = {}
    for line in remote_text.splitlines():
        parts = line.split(None, 1)
        if len(parts) == 2:
            remote[parts[1].strip().lstrip('*')] = parts[0]
    bad = sorted(name for name, digest in local.items() if remote.get(name) != digest)
    return len(local) - len(bad), bad


def cmd_backup(args) -> int:
    manifest = load_manifest(args.asset)
    matched_total, bad_total = 0, []
    for namespace in manifest['namespaces']:
        local_dir = sources_root() / namespace
        remote_dir = f'gothic-ai/content-backup/sources/{namespace}'
        subprocess.run(['ssh', args.host, f'mkdir -p {remote_dir}'], check=True)
        subprocess.run(['rsync', '-a', f'{local_dir}/', f'{args.host}:{remote_dir}/'], check=True)
        local = {p.name: sha256(p) for p in sorted(local_dir.iterdir()) if p.is_file()}
        remote = subprocess.run(['ssh', args.host, f'cd {remote_dir} && sha256sum *'],
            check=True, capture_output=True, text=True).stdout
        matched, bad = compare_digests(local, remote)
        matched_total += matched
        bad_total += [f'{namespace}/{name}' for name in bad]
    manifest['backup'] = dict(date=today(), host=args.host, matched=matched_total, mismatched=bad_total)
    save_manifest(args.asset, manifest)
    print(f'{args.asset}: backup {matched_total} files match, {len(bad_total)} mismatched')
    return 1 if bad_total else 0


# --- reviewer packets ---------------------------------------------------------------------

def fit_for_review(path: Path, target: Path) -> tuple[int, int]:
    """Copy an image within Claude's vision budget: long edge and patch tokens."""
    from PIL import Image
    with Image.open(path) as image:
        image = image.convert('RGB')
        w, h = image.size
        scale = min(1.0, MAX_EDGE / max(w, h))
        while math.ceil(w * scale / PATCH) * math.ceil(h * scale / PATCH) > MAX_TOKENS:
            scale *= 0.98
        size = (max(1, round(w * scale)), max(1, round(h * scale)))
        if size != image.size:
            image = image.resize(size, Image.Resampling.LANCZOS)
        image.save(target)
        return image.size


def section(markdown: str, title: str) -> str:
    match = re.search(rf'^## {re.escape(title)}\s*\n(.*?)(?=^## |\Z)', markdown, re.S | re.M)
    return match.group(1).strip() if match else ''


def preserve_items(intent: str) -> list[str]:
    body = section(intent, 'Список сохранения')
    return [line for line in body.splitlines() if re.match(r'\s*\d+[a-z]?\.\s+\S', line)]


def property_items(intent: str) -> list[str]:
    body = section(intent, 'Свойства нашего кадра')
    return [line for line in body.splitlines() if re.match(r'\s*P\d+\.\s+\S', line) and '…' not in line]


def render_measurements(path: str | None) -> str:
    if not path:
        return 'Нет.'
    rows = json.loads(Path(path).read_text())
    return '\n'.join(f"- {r['name']}: {r['value']}" + (f" (ограничитель {r['limit']})" if r.get('limit') else '')
        + (f" — {r['note']}" if r.get('note') else '') for r in rows) or 'Нет.'


def cmd_pair(args) -> int:
    folder = asset_dir(args.asset)
    manifest = load_manifest(args.asset)
    problems = missing_for(args.asset, 'intent')
    if problems:
        raise ValueError('; '.join(problems))
    intent = (folder / 'intent.md').read_text()
    items = preserve_items(intent)
    review = work_root(args.asset) / 'reviews' / args.id
    if review.exists():
        raise ValueError(f'{review} exists; choose a new review id')
    blind = args.mode == 'blind'
    images = [Path(args.a), Path(args.b)]
    orders = [(0, 1), (1, 0)] if blind else [(0, 1)]
    key = {'stage': args.stage, 'mode': args.mode, 'a': str(images[0]), 'b': str(images[1]),
           'ours': args.ours, 'orders': {}, 'private_a': bool(args.private_a)}
    defaults = DEFAULTS.read_text().split('\n## ', 1)[-1]
    defaults = '## ' + defaults if not defaults.startswith('## ') else defaults
    defaults = re.sub(r'^## ', '### ', defaults, flags=re.M)
    for number, (first, second) in enumerate(orders, 1):
        packet = review / f'order-{number}'
        packet.mkdir(parents=True)
        sizes = []
        for slot, index in enumerate((first, second), 1):
            sizes.append(fit_for_review(images[index], packet / f'image-{slot}.png'))
        if blind:
            mode_text = 'слепая пара: два кадра из игры с одной точки, одной камерой и при одном свете; какой из них новый, не сообщается'
            described = '\n'.join(f'- `image-{i}.png`: кадр из игры, {w}×{h}' for i, (w, h) in enumerate(sizes, 1))
        else:
            mode_text = 'с эталоном: изображение 1 — оригинал (эталон), изображение 2 — кандидат'
            described = (f'- `image-1.png`: эталон, {args.a_label}, {sizes[0][0]}×{sizes[0][1]}\n'
                         f'- `image-2.png`: кандидат, {sizes[1][0]}×{sizes[1][1]}')
        text = fill((TEMPLATES / 'packet.md').read_text(), packet=f'{args.asset}/{args.id}/order-{number}',
            stage=args.stage, mode_text=mode_text, images=described, subject=args.subject,
            preserve='\n'.join(items), properties='\n'.join(property_items(intent)) or 'Нет.',
            deviations=section(intent, 'Отступления') or 'Нет.',
            defaults=defaults.strip(), measurements=render_measurements(args.measurements))
        (packet / 'packet.md').write_text(text)
        ours_slot = None
        if args.ours:
            ours_index = 0 if args.ours == 'a' else 1
            ours_slot = 1 if first == ours_index else 2
        key['orders'][f'order-{number}'] = {'image_1': str(images[first]), 'image_2': str(images[second]),
                                            'ours': ours_slot}
    (review.parent / f'{args.id}.key.json').write_text(json.dumps(key, ensure_ascii=False, indent=2) + '\n')
    manifest['reviews'].append(dict(id=args.id, stage=args.stage, mode=args.mode, verdict='pending', date=today()))
    save_manifest(args.asset, manifest)
    for number in range(1, len(orders) + 1):
        print(review / f'order-{number}')
    return 0


def _relabel(value, ours: int | None):
    if ours is None or value not in (1, 2, '1', '2'):
        return value
    return 'ours' if int(value) == ours else 'original'


def order_verdict(verdict: dict, ours: int | None, mode: str) -> tuple[str, list[str]]:
    """The final rule, applied once we know which image is ours.

    Preserve items compare the pair, so any `no` is a deviation from the original.
    Properties, blockers and notable rejected defaults count only on our image;
    findings of kind `context` (outside the asset) never count.
    """
    reasons = [f"preserve {p['id']}: {p.get('evidence', '')}" for p in verdict.get('preserve', [])
               if p.get('answer') == 'no']
    slot = ours if ours is not None else (2 if mode == 'reference' else None)
    if slot is not None:
        key = f'image_{slot}'
        reasons += [f"property {p['id']}: {p.get('evidence', '')}" for p in verdict.get('properties', [])
                    if p.get(key) == 'no']
        reasons += [f"{f.get('severity')} {f.get('kind')}: {f.get('text', '')}" for f in verdict.get('findings', [])
                    if str(f.get('image')) == str(slot) and f.get('kind') != 'context'
                    and (f.get('severity') == 'blocker'
                         or (f.get('kind') == 'default' and f.get('severity') in ('blocker', 'notable')))]
    return ('REJECT' if reasons else 'PASS'), reasons


def merge_verdicts(verdicts: dict[str, dict], key: dict) -> dict:
    """Combine orders: a verdict counts only when every order agrees."""
    per_order = {}
    for name, verdict in verdicts.items():
        ours = key['orders'][name]['ours']
        criteria = {}
        for criterion, raw in verdict.get('criteria', {}).items():
            # Reviewers sometimes append a reason ("1 — boards are straighter on 2"); the token decides.
            value = re.split(r'[\s—–-]', str(raw).strip(), maxsplit=1)[0]
            if key['mode'] == 'blind' and ours is not None and value in ('1', '2'):
                criteria[criterion] = 'better' if value == str(ours) else 'worse'
            else:
                criteria[criterion] = value
        findings = [dict(f, image=_relabel(f.get('image'), ours)) for f in verdict.get('findings', [])]
        preserve = {p['id']: dict(p, differs=_relabel(p.get('differs'), ours)) for p in verdict.get('preserve', [])}
        properties = {}
        for p in verdict.get('properties', []):
            if ours is not None:
                properties[p['id']] = dict(evidence=p.get('evidence'), ours=p.get(f'image_{ours}'),
                                           original=p.get(f'image_{3 - ours}'))
            else:
                properties[p['id']] = p
        result, reasons = order_verdict(verdict, ours, key['mode'])
        per_order[name] = dict(verdict=result, reviewer_verdict=verdict.get('verdict'), reasons=reasons,
                               criteria=criteria, findings=findings, preserve=preserve, properties=properties)
    answers = {}
    for name, data in per_order.items():
        for pid, item in data['preserve'].items():
            answers.setdefault(pid, {})[name] = item.get('answer')
    disputed_items = sorted(pid for pid, by_order in answers.items() if len(set(by_order.values())) > 1)
    results = {d['verdict'] for d in per_order.values()}
    final = results.pop() if len(results) == 1 else 'DISPUTED'
    criteria = {}
    for criterion in CRITERIA:
        values = {d['criteria'].get(criterion) for d in per_order.values()}
        criteria[criterion] = values.pop() if len(values) == 1 else 'disputed'
    return dict(verdict=final, orders={n: d['verdict'] for n, d in per_order.items()},
        disputed_preserve=disputed_items, criteria=criteria, per_order=per_order)


def cmd_merge(args) -> int:
    manifest = load_manifest(args.asset)
    review = work_root(args.asset) / 'reviews' / args.id
    key = json.loads((review.parent / f'{args.id}.key.json').read_text())
    verdicts = {}
    for name in key['orders']:
        path = review / name / 'verdict.json'
        if not path.is_file():
            raise ValueError(f'{path}: reviewer has not written a verdict')
        verdicts[name] = json.loads(path.read_text())
    merged = merge_verdicts(verdicts, key)
    merged.update(id=args.id, stage=key['stage'], mode=key['mode'], date=today())
    target = asset_dir(args.asset) / 'reviews' / args.id
    target.mkdir(parents=True, exist_ok=True)
    for name in key['orders']:
        shutil.copyfile(review / name / 'packet.md', target / f'packet-{name}.md')
        shutil.copyfile(review / name / 'verdict.json', target / f'verdict-{name}.json')
    (target / 'merged.json').write_text(json.dumps(merged, ensure_ascii=False, indent=2) + '\n')
    _pair_preview(key, target / 'pair.jpg')
    for entry in manifest['reviews']:
        if entry['id'] == args.id:
            entry.update(verdict=merged['verdict'], path=f'reviews/{args.id}/merged.json')
    save_manifest(args.asset, manifest)
    print(f"{args.asset}/{args.id}: {merged['verdict']} {merged['orders']}"
          + (f" disputed {merged['disputed_preserve']}" if merged['disputed_preserve'] else ''))
    return 0


def _pair_preview(key: dict, target: Path, height: int = 432) -> None:
    """Small side-by-side for the user and git: original/reference left, ours/candidate right.
    A private reference (original texture pixels) stays out of git: only the candidate is shown."""
    from PIL import Image
    first, second = Path(key['a']), Path(key['b'])
    if key.get('ours') == 'a':
        first, second = second, first
    tiles = []
    for path in (second,) if key.get('private_a') else (first, second):
        with Image.open(path) as image:
            image = image.convert('RGB')
            tiles.append(image.resize((round(image.width * height / image.height), height), Image.Resampling.LANCZOS))
    sheet = Image.new('RGB', (sum(t.width for t in tiles) + 8, height), (24, 24, 24))
    x = 0
    for tile in tiles:
        sheet.paste(tile, (x, 0))
        x += tile.width + 8
    for quality in range(88, 40, -6):
        sheet.save(target, quality=quality, optimize=True)
        if target.stat().st_size <= 400_000:
            break


# --- decisions and gates ------------------------------------------------------------------

def cmd_decide(args) -> int:
    manifest = load_manifest(args.asset)
    entry = dict(gate=args.gate, verdict=args.verdict, by=args.by, date=args.date or today(), quote=args.quote,
                 note=args.note)
    if args.gate == 'enable':                # the set that goes into the working content: later builds don't move it
        entry['set'] = args.set or (manifest.get('build') or {}).get('set')
    manifest['gates'].append(entry)
    save_manifest(args.asset, manifest)
    line = f"\n**{entry['date']} — {args.gate}: {args.verdict}** ({args.by}). «{args.quote}»"
    if args.note:
        line += f' {args.note}'
    with (asset_dir(args.asset) / 'decisions.md').open('a') as stream:
        stream.write(line + '\n')
    print(f'{args.asset}: update its «Статус» in {QUEUE_DOC.relative_to(ROOT)} to: {queue_status(manifest)}')
    return 0


def _gate_ok(manifest: dict, gate: str) -> bool:
    latest = [g for g in manifest['gates'] if g['gate'] == gate]
    return bool(latest) and latest[-1]['verdict'] == 'approved'


def _review_ok(manifest: dict, stage: str) -> bool:
    latest = [r for r in manifest['reviews'] if r['stage'] == stage and r['verdict'] != 'pending']
    return bool(latest) and latest[-1]['verdict'] in ('PASS', 'DISPUTED')


def missing_for(asset: str, stage: str) -> list[str]:
    folder = asset_dir(asset)
    manifest = load_manifest(asset)
    missing = []
    intent = (folder / 'intent.md').read_text() if (folder / 'intent.md').is_file() else ''
    if len(preserve_items(intent)) < 3 or '…' in section(intent, 'Список сохранения'):
        missing.append('intent.md: filled preserve list (at least three items, no template placeholders)')
    if not section(intent, 'Отступления'):
        missing.append('intent.md: «Отступления» section')
    if stage == 'intent':
        return missing
    if not (folder / 'concept.md').is_file():
        missing.append('concept.md')
    if stage == 'concept':
        return missing
    concepts = [key for key in manifest['selected'] if key == 'concept' or key.startswith('concept:')]
    if not concepts:
        missing.append('selected concept (remaster.py select --key concept, or concept:<TEXTURE> per texture)')
    if not _review_ok(manifest, 'concept'):
        missing.append('reviewer Р1 on the concept (PASS or DISPUTED)')
    if not _gate_ok(manifest, 'concept'):
        missing.append('user approval of the concept (decide --gate concept --verdict approved)')
    if stage == 'textures':
        return missing
    spec = folder / 'build.json'
    if not spec.is_file():
        missing.append('build.json')
    else:
        for texture in json.loads(spec.read_text())['textures']:
            if texture not in manifest['selected']:
                missing.append(f'selected source for {texture}')
    if not _review_ok(manifest, 'textures'):
        missing.append('reviewer Р2 on the texture sheet (PASS or DISPUTED)')
    if stage == 'build':
        return missing
    if not manifest.get('build'):
        missing.append('built staging set (building_set.py build)')
    if stage == 'ingame':
        return missing
    if not _review_ok(manifest, 'ingame'):
        missing.append('reviewer Р3 on in-game pairs (PASS or DISPUTED)')
    if not _gate_ok(manifest, 'ingame'):
        missing.append('user verdict in game (decide --gate ingame --verdict approved)')
    if not (manifest.get('backup') or {}).get('matched'):
        missing.append('verified backup of sources (remaster.py backup)')
    return missing


def cmd_check(args) -> int:
    missing = missing_for(args.asset, args.stage)
    if missing:
        print(f'{args.asset}: not ready for {args.stage}:')
        for item in missing:
            print(f'  - {item}')
        return 1
    print(f'{args.asset}: ready for {args.stage}')
    return 0


def cmd_status(args) -> int:
    manifest = load_manifest(args.asset)
    print(f"{manifest['asset']}: {manifest['title']} ({manifest['kind']})")
    print(f"  sources: {len(manifest['sources'])}, models: {sorted({s['model'] for s in manifest['sources']})}")
    for key, value in manifest['selected'].items():
        print(f"  selected {key}: {value['file']}")
    for review in manifest['reviews']:
        print(f"  review {review['id']} [{review['stage']}]: {review['verdict']}")
    for gate in manifest['gates']:
        print(f"  gate {gate['gate']}: {gate['verdict']} {gate['date']}")
    if manifest.get('build'):
        print(f"  build: {manifest['build'].get('set')} from {manifest['build'].get('base')}")
    if manifest.get('backup'):
        print(f"  backup: {manifest['backup']['matched']} match, {len(manifest['backup']['mismatched'])} mismatched")
    for stage in STAGES[1:]:
        if missing_for(args.asset, stage):
            print(f'  next stage: {stage}')
            break
    else:
        print('  done')
    return 0


# --- the queue -----------------------------------------------------------------------------

GATES = ('preserve', 'concept', 'textures', 'ingame', 'enable')
GATE_PASSED = {'preserve': 'список сохранения принят', 'concept': 'концепт принят', 'textures': 'текстуры приняты',
               'ingame': 'принят в игре', 'enable': 'включён'}
GATE_NAMES = {'preserve': 'список сохранения', 'concept': 'концепт', 'textures': 'текстуры', 'ingame': 'игра',
              'enable': 'включение'}
QUEUE_DOC = ROOT / 'docs/remaster-pipeline.md'


def queue_status(manifest: dict) -> str:
    """The user's gates in one phrase: the furthest approved one, then every later gate sent back."""
    latest = {gate['gate']: gate['verdict'] for gate in manifest['gates']}
    approved = [gate for gate in GATES if latest.get(gate) == 'approved']
    if not approved:
        status, after = 'начат', 0
    else:
        status, after = GATE_PASSED[approved[-1]], GATES.index(approved[-1]) + 1
        if approved[-1] == 'enable':
            enabled = [gate for gate in manifest['gates'] if gate['gate'] == 'enable'][-1]
            name = enabled.get('set') or (manifest.get('build') or {}).get('set')
            if name:
                status += f", набор {name}"
    for gate in GATES[after:]:
        if latest.get(gate) in ('changes', 'rejected'):
            status += f"; {GATE_NAMES[gate]} — {'правки' if latest[gate] == 'changes' else 'отклонено'}"
    return status


def queue_rows(doc: Path) -> dict[str, str]:
    """Asset slug → the «Статус» cell of its row in the queue table."""
    rows = {}
    match = re.search(r'^### Очередь переделки\s*\n(.*?)(?=^#{2,3} |\Z)', doc.read_text(), re.S | re.M)
    for line in (match.group(1) if match else '').splitlines():
        cells = [cell.strip() for cell in line.strip().strip('|').split('|')]
        match = re.fullmatch(r'`([a-z0-9-]+)`', cells[1]) if len(cells) > 2 else None
        if match:
            rows[match.group(1)] = cells[-1]
    return rows


def cmd_queue(args) -> int:
    assets = sorted(path.parent.name for path in assets_root().glob('*/asset.json'))
    statuses = {asset: queue_status(load_manifest(asset)) for asset in assets}
    if not args.check:
        for asset in assets:
            print(f'{asset}\t{statuses[asset]}')
        return 0
    rows = queue_rows(Path(args.check))
    stale = [f'{asset}: «{rows.get(asset, "нет строки")}», должно быть «{statuses[asset]}»'
             for asset in assets if rows.get(asset) != statuses[asset]]
    stale += [f'{asset}: нет папки ассета, статус должен быть «не начат»'
              for asset in rows if asset not in statuses and rows[asset] != 'не начат']
    for line in stale:
        print(line)
    return 1 if stale else 0


def cmd_sheet(args) -> int:
    """Unlabelled grid of reference shots, e.g. several original views as one reference image."""
    from PIL import Image
    images = [Image.open(path).convert('RGB') for path in args.images]
    width = args.width // args.cols
    tiles = [im.resize((width, round(im.height * width / im.width)), Image.Resampling.LANCZOS) for im in images]
    rows = math.ceil(len(tiles) / args.cols)
    row_heights = [max(t.height for t in tiles[r * args.cols:(r + 1) * args.cols]) for r in range(rows)]
    sheet = Image.new('RGB', (width * args.cols, sum(row_heights)), (24, 24, 24))
    y = 0
    for r in range(rows):
        for c, tile in enumerate(tiles[r * args.cols:(r + 1) * args.cols]):
            sheet.paste(tile, (c * width, y))
        y += row_heights[r]
    sheet.save(args.out)
    print(args.out)
    return 0


def bench_lines(asset: str, cameras: list[dict]) -> list[str]:
    """Scenario lines for named eye -> target cameras (world cm), as harbour_hut.cameras writes them:
    the hero stands above both points, `cam world` puts the eye relative to it, `cam look` the target."""
    lines = [f'# {asset}: original or a staging set, default game FOV, noon, NPC and effects hidden.',
             f'# Generated by remaster.py bench from reports/content/remaster/{asset}/cameras.json; world cm.',
             'cheat god', 'clear 1000000', 'set time 12 0', 'overlay off',
             'view hide animated morph pfx', 'cam lift off', '']
    names = set()
    for camera in cameras:
        name, eye, target = camera['name'], camera['eye'], camera['target']
        if not re.fullmatch(r'[a-z0-9]+(-[a-z0-9]+)*', name) or name in names:
            raise ValueError(f'camera name {name!r}: lowercase slug, unique')
        names.add(name)
        hero = [float(target[0]), max(float(eye[1]), float(target[1])) + 500, float(target[2])]
        delta = [float(eye[i]) - hero[i] for i in range(3)]
        distance = math.hypot(delta[0], delta[2])
        if distance < 50:
            raise ValueError(f'{name}: eye straight above or below the target')
        yaw = math.degrees(math.atan2(delta[2], delta[0]))
        lines += [f'# {name}: {camera.get("note", "")}'.rstrip(),
                  f'cam look {float(target[1]) - hero[1]:.3f}',
                  'goto pos ' + ' '.join(f'{v:.3f}' for v in hero),
                  f'cam world {distance:.3f} {delta[1]:.3f} {yaw:.6f}',
                  'wait 34', 'cam hold', 'wait 6000', 'set time 12 0', f'shot {name}', 'wait 300', '']
    return lines


def cmd_bench(args) -> int:
    cameras = json.loads((asset_dir(args.asset) / 'cameras.json').read_text())['cameras']
    target = ROOT / 'scripts/content' / f'{args.asset}.bench'
    target.write_text('\n'.join(bench_lines(args.asset, cameras)))
    print(target)
    return 0


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    p = sub.add_parser('new'); p.add_argument('asset'); p.add_argument('--title', required=True)
    p.add_argument('--kind', default='building', choices=('building', 'props', 'district', 'material'))
    p.set_defaults(func=cmd_new)
    p = sub.add_parser('register'); p.add_argument('asset'); p.add_argument('--namespace', required=True)
    p.set_defaults(func=cmd_register)
    p = sub.add_parser('import'); p.add_argument('asset'); p.add_argument('png')
    p.add_argument('--role', required=True, choices=('concept', 'texture', 'reference'))
    p.add_argument('--texture'); p.add_argument('--prompt-file', required=True)
    p.add_argument('--start', required=True); p.add_argument('--end', required=True)
    p.add_argument('--thread', required=True); p.add_argument('--parent'); p.add_argument('--reason')
    p.add_argument('--namespace')
    p.add_argument('--tool', help='our script that assembled this PNG from the --parent image_gen sources')
    p.add_argument('--map', action='append', metavar='NAME=PNG',
                   help='a map baked from our model with this albedo: normal, roughness, ao (with --tool)')
    p.set_defaults(func=cmd_import)
    p = sub.add_parser('select'); p.add_argument('asset'); p.add_argument('--key', required=True)
    p.add_argument('--file', required=True); p.add_argument('--reason', required=True)
    p.set_defaults(func=cmd_select)
    p = sub.add_parser('backup'); p.add_argument('asset'); p.add_argument('--host', default='dev-ubuntu')
    p.set_defaults(func=cmd_backup)
    p = sub.add_parser('pair'); p.add_argument('asset'); p.add_argument('--id', required=True)
    p.add_argument('--stage', required=True, choices=REVIEW_STAGES)
    p.add_argument('--mode', required=True, choices=('blind', 'reference'))
    p.add_argument('--a', required=True, help='original / reference image')
    p.add_argument('--b', required=True, help='ours / candidate image')
    p.add_argument('--ours', choices=('a', 'b'), default='b')
    p.add_argument('--subject', required=True); p.add_argument('--measurements')
    p.add_argument('--a-label', default='оригинал в игре', help='what the reference image shows (reference mode)')
    p.add_argument('--private-a', action='store_true',
                   help='the reference holds original texture pixels: keep it out of pair.jpg in git')
    p.set_defaults(func=cmd_pair)
    p = sub.add_parser('merge'); p.add_argument('asset'); p.add_argument('--id', required=True)
    p.set_defaults(func=cmd_merge)
    p = sub.add_parser('decide'); p.add_argument('asset')
    p.add_argument('--gate', required=True, choices=('preserve', 'concept', 'textures', 'ingame', 'enable'))
    p.add_argument('--verdict', required=True, choices=GATE_VERDICTS)
    p.add_argument('--quote', required=True); p.add_argument('--note'); p.add_argument('--date')
    p.add_argument('--by', default='user')
    p.add_argument('--set', help='enable: the set enabled (default: the last built set)')
    p.set_defaults(func=cmd_decide)
    p = sub.add_parser('check'); p.add_argument('asset'); p.add_argument('--stage', required=True, choices=STAGES)
    p.set_defaults(func=cmd_check)
    p = sub.add_parser('status'); p.add_argument('asset'); p.set_defaults(func=cmd_status)
    p = sub.add_parser('queue', help='status of every asset; --check DOC compares the queue table with the gates')
    p.add_argument('--check', metavar='DOC'); p.set_defaults(func=cmd_queue)
    p = sub.add_parser('bench'); p.add_argument('asset'); p.set_defaults(func=cmd_bench)
    p = sub.add_parser('sheet'); p.add_argument('out'); p.add_argument('images', nargs='+')
    p.add_argument('--cols', type=int, default=2); p.add_argument('--width', type=int, default=2048)
    p.set_defaults(func=cmd_sheet)
    args = parser.parse_args(argv)
    try:
        return args.func(args)
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
        print(f'remaster: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())

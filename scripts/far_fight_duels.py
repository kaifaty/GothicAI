#!/usr/bin/env python3
"""Plan, run and summarize native nonlethal NPC calibration duels."""
import argparse
from collections import defaultdict
import json
import math
import os
from pathlib import Path
import shlex
import statistics
import subprocess
import sys
from datetime import datetime

ROOT = Path(__file__).resolve().parents[1]
PAIRS = ROOT / 'data/npc/far-fight-pairs.json'
PRELUDE = ('set time 12 0\ngoto NW_BIGFARM_VORPOSTEN1_01_RIGHTFIELD_02\n'
           'turn 210\nwait 500\nwatch player\nclear 3000\nimmortal player on\nrelease all\n')
SIDE_FIELDS = {'instance', 'level', 'hp_before', 'hp_after', 'hp_max', 'str', 'dex',
               'hitchance', 'talent_skills', 'talent_values', 'protection', 'damage',
               'damagetype', 'weapon', 'two_handed', 'weapon_damage', 'weapon_damagetype',
               'immortal_before', 'hits', 'damage_dealt', 'hp_one', 'unconscious',
               'weapon_dropped', 'guild', 'fight_tactic'}


def load_pairs(path=PAIRS):
    return json.loads(Path(path).read_text())['pairs']


def plan(out, pairs, seeds=20, first_seed=17, timeout_ms=180000, max_seconds=7200):
    """Budget every timeout in real time, plus setup and between-duel overhead."""
    if seeds < 1 or timeout_ms < 1 or not 60 < max_seconds <= 7200:
        raise ValueError('positive seeds/timeout and a batch limit in (60, 7200] seconds required')
    capacity = int((max_seconds - 60) // (timeout_ms / 1000 + 1))
    if capacity < 1:
        raise ValueError('one duel does not fit the batch time limit')
    out = Path(out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    commands = []
    for pair in pairs:
        a, b = pair['npcs']
        for attacker, defender in ((a, b), (b, a)):
            for seed in range(first_seed, first_seed + seeds):
                commands.append(f'npcduel {attacker} {defender} {seed} {timeout_ms}')
    paths = []
    for old in out.glob('duels-*.bench'):
        old.unlink()
    for i in range(0, len(commands), capacity):
        path = out / f'duels-{len(paths):03d}.bench'
        path.write_text(PRELUDE + '\n'.join(commands[i:i+capacity]) + '\n')
        paths.append(path)
    return paths


def read_records(folders):
    rows = []
    for folder in folders:
        path = Path(folder) / 'events.jsonl'
        for line in path.read_text().splitlines():
            row = json.loads(line)
            if row.get('kind') == 'npc_duel_error':
                raise ValueError(f'{path}: {row["reason"]}')
            if row.get('kind') == 'npc_duel':
                validate(row)
                rows.append(row)
    if not rows:
        raise ValueError('no npc_duel records')
    return rows


def validate(row):
    for field in ('seed', 'duration_ms', 'attacker', 'defender'):
        if field not in row:
            raise ValueError(f'missing {field}')
    if row['duration_ms'] <= 0:
        raise ValueError('duration_ms must be positive')
    for role in ('attacker', 'defender'):
        side = row[role]
        missing = SIDE_FIELDS - side.keys()
        if missing:
            raise ValueError(f'{role}: missing {sorted(missing)}')
        for field in ('protection', 'damage', 'weapon_damage'):
            if len(side[field]) != 8:
                raise ValueError(f'{role}: {field} needs 8 entries')
        if set(side['hitchance']) != {'1h', '2h'} or len(side['talent_skills']) != 22 or len(side['talent_values']) != 22:
            raise ValueError(f'{role}: incomplete talents')
        if not 0 <= side['guild'] < 16:
            raise ValueError(f'{role}: not a human guild')
    if row.get('timeout') is True:
        return
    sides = {row[r]['instance']: row[r] for r in ('attacker', 'defender')}
    if {row.get('winner'), row.get('loser')} != set(sides) or len(sides) != 2:
        raise ValueError('invalid winner/loser')
    loser = sides[row['loser']]
    if loser['hp_after'] <= 0:
        return  # the native nonlethal fight still killed (body impact): keep it, report counts deaths
    if loser['hp_after'] != 1 or not loser['hp_one'] or not loser['unconscious']:
        raise ValueError('resolved duel did not end at 1 HP in ZS_UNCONSCIOUS')


def _killed(row):
    return bool(row.get('loser')) and next(
        row[r] for r in ('attacker', 'defender') if row[r]['instance'] == row['loser'])['hp_after'] <= 0


def summarize(rows, pairs):
    lookup = {frozenset(p['npcs']): p for p in pairs}
    groups, talents = defaultdict(list), defaultdict(lambda: [0, 0.0, 0])
    for row in rows:
        a, b = row['attacker']['instance'], row['defender']['instance']
        pair = lookup[frozenset((a, b))]
        groups[(pair['id'], a, b)].append(row)
        for role in ('attacker', 'defender'):
            side = row[role]
            talent = 2 if side['two_handed'] else 1
            tier = side['talent_skills'][talent] if side['weapon'] else 0
            bucket = talents[('2h' if side['two_handed'] else '1h' if side['weapon'] else 'fist', tier)]
            bucket[0] += side['hits']
            bucket[1] += row['duration_ms'] / 1000
            bucket[2] += 1
    results = []
    for (pair_id, a, b), samples in sorted(groups.items()):
        losses = []
        for row in samples:
            if row.get('timeout'):
                continue
            winner = next(row[r] for r in ('attacker', 'defender') if row[r]['instance'] == row['winner'])
            losses.append(winner['hp_before'] - winner['hp_after'])
        results.append(dict(pair=pair_id, holdout=lookup[frozenset((a, b))]['holdout'], attacker=a, defender=b,
                            n=len(samples), win_rate=sum(r.get('winner') == a for r in samples)/len(samples),
                            timeouts=sum(r.get('timeout', False) for r in samples),
                            deaths=sum(_killed(r) for r in samples),
                            median_winner_hp_loss=statistics.median(losses) if losses else None,
                            median_duration_ms=statistics.median(r['duration_ms'] for r in samples)))
    return dict(records=len(rows), pairs=results, hits_by_talent=[
        dict(weapon=weapon, tier=tier, n=n, hits=hits, seconds=seconds, hits_per_second=hits/seconds)
        for (weapon, tier), (hits, seconds, n) in sorted(talents.items())])


def report(folders, out, pairs):
    result = summarize(read_records(folders), pairs)
    lines = [f'NPC duels: {result["records"]} records', '',
             '| Pair | Holdout | Attacker → defender | n | Attacker wins / all | Timeouts | Deaths | Winner HP loss, median | Duration, median s |',
             '|---|---|---|---:|---:|---:|---:|---:|---:|']
    for row in result['pairs']:
        loss = row['median_winner_hp_loss']
        lines.append(f'| {row["pair"]} | {row["holdout"]} | {row["attacker"]} → {row["defender"]} | '
                     f'{row["n"]} | {row["win_rate"]:.3f} | {row["timeouts"]} | {row["deaths"]} | '
                     f'{loss if loss is not None else "n/a"} | {row["median_duration_ms"]/1000:.3f} |')
    lines += ['', '| Weapon | Talent tier | n | HP-loss hits | Duel seconds | Hits/s |', '|---|---:|---:|---:|---:|---:|']
    for row in result['hits_by_talent']:
        lines.append(f'| {row["weapon"]} | {row["tier"]} | {row["n"]} | {row["hits"]} | '
                     f'{row["seconds"]:.3f} | {row["hits_per_second"]:.4f} |')
    out = Path(out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    (out / 'far-fight.json').write_text(json.dumps(result, indent=2) + '\n')
    table = '\n'.join(lines) + '\n'
    (out / 'far-fight.md').write_text(table)
    print(table, end='')
    return result


def run_benches(paths, out, *, smoke=False):
    """bench.sh owns build, gq admission and output collection; never nest queue jobs."""
    inspect = ("import json; from pathlib import Path; h=Path.home(); "
               "key=json.loads((h/'gothic-ai/game-base.json').read_text())['base']; "
               "base=next(p for p in (h/'.cache/gothic-ai/base'/key, Path('/mnt/shared/gothic-ai/base')/key) "
               "if (p/'base.json').is_file()); print(json.dumps(dict(home=str(h),base=str(base))))")
    prepared = json.loads(subprocess.check_output(
        ['ssh', '-o', 'ConnectTimeout=10', os.environ.get('RYZEN_HOST', 'ryzen'),
         shlex.join(['python3', '-c', inspect])], text=True))
    remote_home = prepared['home']
    out = Path(out).resolve()
    folders = []
    for path in paths:
        count = sum(line.startswith('npcduel ') for line in path.read_text().splitlines())
        timeout_ms = max(int(line.split()[4]) for line in path.read_text().splitlines() if line.startswith('npcduel '))
        seconds = math.ceil(60 + count * (timeout_ms/1000 + 1))
        if seconds > 7200:
            raise ValueError(f'{path}: exceeds the 2h batch budget')
        dest = out / path.stem
        env = {**os.environ, 'BENCH_HOST': 'ryzen', 'RYZEN_BENCH_CLASS': 'quick' if smoke else 'batch',
               'RYZEN_MAX': f'{max(1800, seconds)}s', 'TIMEOUT': str(seconds),
               'OPENGOTHIC_TEXTURES': '', 'OPENGOTHIC_FRACTURE': '', 'OPENGOTHIC_CONVEX_PARTS': '',
               'OPENGOTHIC_BASE': prepared['base'],
               'OPENGOTHIC_BASE_CACHE': f'{remote_home}/gothic-ai/far-fight/base-cache',
               'OPENGOTHIC_BASE_STORE': f'{remote_home}/gothic-ai/far-fight/base-store',
               'RYZEN_BENCH_ARCHIVE': f'{remote_home}/gothic-ai/far-fight/{out.name}/{path.stem}'}
        subprocess.run(['bash', str(ROOT/'scripts/bench.sh'), str(path), str(dest)], env=env, check=True)
        if 'gq: warning: archive ' in (dest/'ryzen.log').read_text():
            raise ValueError(f'{dest}: gq could not archive the result')
        records = read_records([dest])
        expected = {tuple(line.split()[1:4]) for line in path.read_text().splitlines() if line.startswith('npcduel ')}
        actual = {(r['attacker']['instance'], r['defender']['instance'], str(r['seed'])) for r in records}
        if len(records) != count or actual != expected:
            raise ValueError(f'{dest}: expected {count} unique duels, got {len(records)} records/{len(actual)} unique')
        folders.append(dest)
    return folders


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    p = sub.add_parser('plan')
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--seeds', type=int, default=20)
    p.add_argument('--first-seed', type=int, default=17)
    p.add_argument('--timeout-ms', type=int, default=180000)
    p.add_argument('--max-seconds', type=int, default=7200)
    p = sub.add_parser('run')
    p.add_argument('plan', type=Path)
    p.add_argument('--out', type=Path, required=True)
    p = sub.add_parser('smoke')
    p.add_argument('--out', type=Path, default=ROOT/'.bench'/f'far-fight-smoke-{datetime.now():%Y%m%d-%H%M%S}')
    p = sub.add_parser('report')
    p.add_argument('folders', type=Path, nargs='+')
    p.add_argument('--out', type=Path)
    args = parser.parse_args(argv)
    pairs = load_pairs()
    if args.command == 'plan':
        paths = plan(args.out, pairs, args.seeds, args.first_seed, args.timeout_ms, args.max_seconds)
        print('\n'.join(map(str, paths)))
        print('Run: ' + shlex.join([sys.executable, str(Path(__file__).resolve()), 'run', str(args.out.resolve()), '--out',
                                   str(args.out.resolve().with_name(args.out.name+'-run'))]))
    elif args.command == 'report':
        report(args.folders, args.out or args.folders[0], pairs)
    else:
        if args.command == 'smoke':
            selected = [p for p in pairs if p.get('smoke')]
            if len(selected) != 2:
                raise ValueError('smoke needs exactly two pairs')
            paths = plan(args.out/'plan', selected, seeds=2)
        else:
            paths = sorted(args.plan.resolve().glob('duels-*.bench'))
            if not paths:
                raise ValueError('plan has no duel benches')
        folders = run_benches(paths, args.out, smoke=args.command == 'smoke')
        result = report(folders, args.out, pairs)
        if args.command == 'smoke' and result['records'] != 8:
            raise ValueError(f'smoke expected 8 records, got {result["records"]}')
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        print(f'far-fight: {error}', file=sys.stderr)
        sys.exit(1)

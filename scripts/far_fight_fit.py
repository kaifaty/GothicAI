#!/usr/bin/env python3
"""Measure distant-fight v3 parts on training pairs; --accept checks holdouts once.

Run with the server environment::

    cd server && uv run python ../scripts/far_fight_fit.py FOLDERS... --out RESULT.json

No outcome fitting or game runs: clocks, hit probabilities, damage multipliers and
response delays come directly from training counters/logs. No server file is written.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import statistics
import sys
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'server/src'))
sys.path.insert(0, str(ROOT / 'scripts'))
from far_fight_duels import load_pairs, validate, weapon_talent  # noqa: E402

from gothic_ai.npc_far_fight import (  # noqa: E402
    TIME_LIMIT_S,
    Coefficients,
    _duel,
    combatant_from_values,
    hit_damage,
)

DRAWS = 400
PROBABILITY_FLOOR = 1 / 800
SEED_NAMESPACE = 'far-fight-fit-v1'
MIN_SWINGS = 30
WEAPONS = ('fist', '1h', '2h')
ROLES = ('attacker', 'defender')
UNCALIBRATED = Coefficients(.4, .5, 1.)


def story_values(side, weapons):
    """Reconstruct the npcduel pre-draw story_eval values, then use the server builder.

    npcduel snapshots freshly constructed NPCs before aiDrawWeapon: there is no readied
    weapon contribution. Equipped weapon stats are exchanged by combatant_from_values,
    rather than added a second time here. Legacy fist chance comes from GOTHIC.DAT.
    """
    weapon = weapons['weapons'][side['weapon']] if side['weapon'] else None
    if weapon is not None and (weapon['damagetype'] != side['weapon_damagetype']
                               or weapon['damage'] != side['weapon_damage']):
        raise ValueError('Weapon snapshot differs from GOTHIC.DAT')
    total = side.get('weapon_damage_total', weapon['damage_total'] if weapon else 0)
    if weapon is not None and total != weapon['damage_total']:
        raise ValueError('Weapon total differs from GOTHIC.DAT')
    fist = side['hitchance'].get('fist')
    if fist is None:
        fist = weapons.get('unarmed_hitchance', {}).get(side['instance'], 0) if weapon else (
            weapons['unarmed_hitchance'][side['instance']])
    return [side['level'], side['hp_before'], side['hp_max'], side['str'],
            fist, side['hitchance']['1h'], side['hitchance']['2h'],
            *side['protection'], *side['damage'], side['damagetype'],
            side['talent_skills'][1], side['talent_skills'][2], int(bool(weapon)),
            weapon['flags'] if weapon else 0, side['weapon_damagetype'], total,
            *side['weapon_damage'], 0, 0, *([0] * 8)]


def combatant(side, weapons):
    return combatant_from_values(side['instance'], story_values(side, weapons))


def outcome(row):
    """A body-impact death is a win by the other participant, including at timeout."""
    dead = [i for i, role in enumerate(('attacker', 'defender')) if row[role]['hp_after'] <= 0]
    if dead:
        if len(dead) != 1:
            raise ValueError('Both participants died')
        return 1 - dead[0]
    if row.get('timeout'):
        return 2
    return 0 if row['winner'] == row['attacker']['instance'] else 1


def split_rows(rows, pairs, *, holdout=False):
    lookup = {frozenset(pair['npcs']): pair for pair in pairs}
    included, excluded = [], []
    for row in rows:
        pair = lookup[frozenset(row[role]['instance'] for role in ROLES)]
        if pair['holdout'] != holdout:
            continue
        # Exclude the whole role-cell, including its armed opponent's measurements.
        (included if row['attacker']['weapon'] else excluded).append(row)
    def key(row):
        return (lookup[frozenset(row[role]['instance'] for role in ROLES)]['id'],
                row['attacker']['instance'], row['seed'])
    return sorted(included, key=key), sorted(excluded, key=key)


def verify_base_damage(rows, weapons):
    matches = 0
    for row in rows:
        validate(row)
        if 'hit_log' not in row:
            raise ValueError('v3 calibration requires npcduel v2 hit logs and swing counters')
        actors = {row[role]['instance']: combatant(row[role], weapons) for role in ROLES}
        for hit in row['hit_log']:
            actual = hit_damage(actors[hit['attacker']], actors[hit['victim']],
                                hit['critical'], UNCALIBRATED)
            if actual != hit['base_damage']:
                raise ValueError(f'Base damage mismatch: seed={row["seed"]}, '
                                 f'{hit["attacker"]} -> {hit["victim"]}, '
                                 f'critical={hit["critical"]}, '
                                 f'recorded={hit["base_damage"]}, calculated={actual}')
            matches += 1
    return dict(hits=matches, matches=matches)


def cells_from_rows(rows, pairs, weapons):
    lookup = {frozenset(pair['npcs']): pair for pair in pairs}
    groups = defaultdict(list)
    for row in rows:
        pair = lookup[frozenset(row[role]['instance'] for role in ROLES)]
        groups[(pair['id'], row['attacker']['instance'], row['defender']['instance'])].append(row)
    cells = []
    for key, samples in sorted(groups.items()):
        actors = tuple(combatant(samples[0][role], weapons) for role in ROLES)
        hp_max = tuple(samples[0][role]['hp_max'] for role in ROLES)
        seeds = set()
        counts = [0, 0, 0]
        losses = []
        for row in samples:
            if row['seed'] in seeds:
                raise ValueError(f'Duplicate duel seed in {key}')
            seeds.add(row['seed'])
            if (tuple(combatant(row[role], weapons) for role in ROLES) != actors
                    or tuple(row[role]['hp_max'] for role in ROLES) != hp_max):
                raise ValueError(f'Combatants changed within cell {key}')
            result = outcome(row)
            counts[result] += 1
            if result < 2:
                side = row[ROLES[result]]
                losses.append((side['hp_before'] - side['hp_after']) / side['hp_max'])
        cells.append(dict(pair=key[0], attacker=key[1], defender=key[2], actors=actors,
                          hp_max=hp_max, n=len(samples), counts=counts, losses=losses,
                          median_duration_s=statistics.median(row['duration_ms']/1000 for row in samples),
                          deaths=sum(any(row[role]['hp_after'] <= 0 for role in ROLES)
                                     for row in samples)))
    return cells


def measure_parts(rows):
    """Fixed pooling: <30 swings drops opponent tier, then own tier. Never use outcomes."""
    samples = []
    multipliers = {weapon: [] for weapon in WEAPONS}
    delays = []
    negative_delays = 0
    for row in rows:
        sides = {row[role]['instance']: row[role] for role in ROLES}
        for role in ROLES:
            side = row[role]
            opponent = row['defender' if role == 'attacker' else 'attacker']
            weapon, tier = weapon_talent(side)
            samples.append(dict(weapon=weapon, tier=tier, opponent=weapon_talent(opponent)[1],
                                swings=side['swings'], hits=side['hits'],
                                seconds=row['duration_ms']/1000))
        for hit in row['hit_log']:
            weapon = weapon_talent(sides[hit['attacker']])[0]
            multipliers[weapon].append(hit['melee_scale'] * hit['stun_bonus'] * hit['backstab_mult'])
        received = [hit['time_ms'] for hit in row['hit_log']
                    if hit['victim'] == row['defender']['instance']]
        if received and row['defender_first_swing_ms'] is not None:
            delay = (row['defender_first_swing_ms'] - min(received))/1000
            negative_delays += delay < 0
            delays.append(max(0., delay))

    def totals(group):
        return dict(swings=sum(s['swings'] for s in group), hits=sum(s['hits'] for s in group),
                    seconds=sum(s['seconds'] for s in group), observations=len(group))

    swing_rate, p_hit = {}, {}
    swing_cells, hit_cells = [], []
    for weapon in WEAPONS:
        pooled = [s for s in samples if s['weapon'] == weapon]
        if sum(s['swings'] for s in pooled) < MIN_SWINGS:
            raise ValueError(f'Fewer than {MIN_SWINGS} training swings for weapon {weapon}')
        swing_rate[weapon], p_hit[weapon] = {}, {}
        for tier in range(3):
            own = [s for s in pooled if s['tier'] == tier]
            raw = totals(own)
            rate_group = own if raw['swings'] >= MIN_SWINGS else pooled
            rate_totals = totals(rate_group)
            rate = rate_totals['swings']/rate_totals['seconds']
            swing_rate[weapon][str(tier)] = rate
            swing_cells.append(dict(weapon=weapon, tier=tier, raw=raw, used=rate_totals,
                                    scope='own_tier' if rate_group is own else 'weapon', rate=rate))
            p_hit[weapon][str(tier)] = {}
            for opponent in range(3):
                exact = [s for s in own if s['opponent'] == opponent]
                raw_hit = totals(exact)
                group, scope = exact, 'own_and_opponent_tier'
                if raw_hit['swings'] < MIN_SWINGS:
                    group, scope = own, 'own_tier'
                if sum(s['swings'] for s in group) < MIN_SWINGS:
                    group, scope = pooled, 'weapon'
                used = totals(group)
                probability = used['hits']/used['swings']
                p_hit[weapon][str(tier)][str(opponent)] = probability
                hit_cells.append(dict(weapon=weapon, tier=tier, opponent_tier=opponent,
                                      raw=raw_hit, used=used, scope=scope, probability=probability))
    calibration = dict(version=3, source=('npcduel v2; training pairs only; '
                                         f'{len(rows)} duels; unarmed attackers excluded'),
                       swing_rate=swing_rate, p_hit=p_hit,
                       damage_multiplier={w: sorted(values) for w, values in multipliers.items()},
                       response_delay=sorted(delays))
    Coefficients.from_mapping(calibration)
    return dict(calibration=calibration, swing_cells=swing_cells, hit_cells=hit_cells,
                response_records=len(delays), response_unmeasured=len(rows)-len(delays),
                negative_response_delays=negative_delays)


def log_likelihood(counts, probabilities):
    return sum(count * math.log(max(PROBABILITY_FLOOR, probability))
               for count, probability in zip(counts, probabilities, strict=True))


def trial_seeds(cell):
    seed = hashlib.sha256((':'.join((SEED_NAMESPACE, cell['pair'], cell['attacker'],
                                    cell['defender']))).encode()).digest()
    return [hashlib.sha256(seed + trial.to_bytes(4, 'big')).digest() for trial in range(DRAWS)]


class Evaluator:
    """Use the server's exact seeded simulator for both v3 and the uncalibrated baseline."""
    def __init__(self, cells):
        self.cells = cells

    def simulate(self, index, coefficients):
        cell = self.cells[index]
        draws = [_duel(*cell['actors'], coefficients, seed) for seed in trial_seeds(cell)]
        winners = [2 if d[0] is None else 0 if d[0] == cell['attacker'] else 1 for d in draws]
        return dict(probabilities=[winners.count(role)/DRAWS for role in range(3)],
                    winners=winners, losses=[d[1] for d in draws], durations=[d[2] for d in draws])


def binomial_interval(n, probability):
    """Equal-tail central 95% predictive interval for an observed count (no scipy)."""
    distribution = [math.comb(n, k) * probability ** k * (1 - probability) ** (n - k)
                    for k in range(n + 1)]
    def quantile(q):
        cumulative = 0.
        for k, mass in enumerate(distribution):
            cumulative += mass
            if cumulative >= q - 1e-14:
                return k
        return n
    return [quantile(.025), quantile(.975)]


def cell_result(cell, evaluator, index, coefficients):
    draws = evaluator.simulate(index, coefficients)
    winner_losses = [draws['losses'][i]/cell['hp_max'][role]
                     for i, role in enumerate(draws['winners']) if role < 2]
    return dict(pair=cell['pair'], attacker=cell['attacker'], defender=cell['defender'], n=cell['n'],
                counts=cell['counts'], probabilities=draws['probabilities'], deaths=cell['deaths'],
                observed_median_winner_loss=statistics.median(cell['losses']) if cell['losses'] else None,
                model_median_winner_loss=statistics.median(winner_losses) if winner_losses else None,
                observed_median_duration_s=cell['median_duration_s'],
                model_median_duration_s=statistics.median(draws['durations']),
                log_likelihood=log_likelihood(cell['counts'], draws['probabilities']))


def accept(cells, training, coefficients, pairs, excluded):
    expected = {(pair['id'], a, b) for pair in pairs if pair['holdout']
                for a, b in (pair['npcs'], pair['npcs'][::-1])}
    excluded_keys = {(frozenset(row[role]['instance'] for role in ROLES), row['attacker']['instance'])
                     for row in excluded}
    expected = {key for key in expected if (frozenset(key[1:]), key[1]) not in excluded_keys}
    actual = {(cell['pair'], cell['attacker'], cell['defender']) for cell in cells}
    if actual != expected or any(cell['n'] != 10 for cell in cells):
        raise ValueError(f'Holdout cells incomplete: expected {len(expected)} cells of 10, got {len(actual)}')
    evaluator = Evaluator(cells)
    total = sum(cell['n'] for cell in training)
    constant = [sum(cell['counts'][result] for cell in training) / total for result in range(3)]
    results = []
    for index, cell in enumerate(cells):
        result = cell_result(cell, evaluator, index, coefficients)
        result['attacker_win_interval'] = binomial_interval(cell['n'], result['probabilities'][0])
        low, high = result['attacker_win_interval']
        result['count_check'] = low <= cell['counts'][0] <= high
        result['uncalibrated_log_likelihood'] = log_likelihood(
            cell['counts'], evaluator.simulate(index, UNCALIBRATED)['probabilities'])
        result['constant_log_likelihood'] = log_likelihood(cell['counts'], constant)
        results.append(result)
    n = sum(cell['n'] for cell in cells)
    scores = {name: sum(cell[name] for cell in results) for name in (
        'log_likelihood', 'uncalibrated_log_likelihood', 'constant_log_likelihood')}
    better = scores['log_likelihood'] > max(scores['uncalibrated_log_likelihood'],
                                           scores['constant_log_likelihood'])
    counts_pass = all(cell['count_check'] for cell in results)
    return dict(passed=better and counts_pass, cells=results, records=n,
                constant_probabilities=constant, better_than_baselines=better,
                all_attacker_counts_in_interval=counts_pass, **scores,
                mean_log_likelihood={name: score/n for name, score in scores.items()},
                likelihood_per_duel={name: math.exp(score/n) for name, score in scores.items()})


def run(folders, weapons, pairs, *, acceptance=False):
    rows = []
    for folder in sorted(map(Path, folders)):
        for line in (folder / 'events.jsonl').read_text().splitlines():
            row = json.loads(line)
            if row.get('kind') == 'npc_duel_error':
                raise ValueError(row['reason'])
            if row.get('kind') == 'npc_duel':
                rows.append(row)
    train_rows, excluded_train = split_rows(rows, pairs)
    if not train_rows:
        raise ValueError('No training duels')
    # Verify even excluded training hits; exclusion applies only to estimation/acceptance.
    verification = verify_base_damage(train_rows + excluded_train, weapons)
    training = cells_from_rows(train_rows, pairs, weapons)
    result = measure_parts(train_rows)
    coefficients = Coefficients.from_mapping(result['calibration'])
    result.update(training_records=len(train_rows), training_pairs=sorted({c['pair'] for c in training}),
                  excluded_training_duels=len(excluded_train), base_damage_verification=verification,
                  settings=dict(draws=DRAWS, probability_floor=PROBABILITY_FLOOR,
                                time_limit_s=TIME_LIMIT_S, seed_namespace=SEED_NAMESPACE,
                                minimum_swings=MIN_SWINGS,
                                pooling=('exact <30 swings -> ignore opponent tier; '
                                         'still <30 -> ignore own tier')),
                  folders=[Path(folder).name for folder in sorted(map(Path, folders))])
    evaluator = Evaluator(training)
    result['training_cells'] = [cell_result(cell, evaluator, i, coefficients)
                                for i, cell in enumerate(training)]
    result['acceptance'] = None
    if acceptance:
        holdout_rows, excluded_holdout = split_rows(rows, pairs, holdout=True)
        verified = verify_base_damage(holdout_rows + excluded_holdout, weapons)
        result['base_damage_verification'] = {key: verification[key]+verified[key] for key in verification}
        result['excluded_holdout_duels'] = len(excluded_holdout)
        result['acceptance'] = accept(cells_from_rows(holdout_rows, pairs, weapons), training,
                                      coefficients, pairs, excluded_holdout)
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('folders', type=Path, nargs='+')
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--weapons', type=Path, default=ROOT / 'data/npc/far-fight-weapons.json')
    parser.add_argument('--pairs', type=Path, default=ROOT / 'data/npc/far-fight-pairs.json')
    parser.add_argument('--accept', action='store_true')
    args = parser.parse_args(argv)
    result = run(args.folders, json.loads(args.weapons.read_text()), load_pairs(args.pairs),
                 acceptance=args.accept)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(result, ensure_ascii=False, indent=2, allow_nan=False) + '\n')
    print(json.dumps({key: result[key] for key in ('training_records', 'excluded_training_duels',
                                                 'base_damage_verification')}))
    if result['acceptance'] is not None:
        print(json.dumps({'acceptance': result['acceptance']['passed'],
                          'records': result['acceptance']['records'],
                          'mean_log_likelihood': result['acceptance']['mean_log_likelihood']}))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())

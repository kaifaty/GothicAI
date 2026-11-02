#!/usr/bin/env python3
"""Calibrate distant NPC duels on training pairs, then optionally accept on holdouts.

Run with the server environment (numpy is an existing server dependency)::

    cd server && uv run python ../scripts/far_fight_fit.py FOLDERS... --out RESULT.json

Add --accept only after training/selection to evaluate the held-out pairs. No game is
started and no calibration file is modified. All objective values are log likelihoods
of individual duel outcomes, with probabilities floored at 1/800 without renormalizing.
"""
from __future__ import annotations

import argparse
from collections import defaultdict
from dataclasses import asdict, replace
from functools import lru_cache
import hashlib
import itertools
import json
import math
from pathlib import Path
import random
import statistics
import sys

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'server/src'))
sys.path.insert(0, str(ROOT / 'scripts'))
from far_fight_duels import load_pairs, summarize, validate  # noqa: E402
from gothic_ai.npc_far_fight import (  # noqa: E402
    TIME_LIMIT_S, Coefficients, combatant_from_values, hit_damage,
)

DRAWS = 400
PROBABILITY_FLOOR = 1 / 800
SEED_NAMESPACE = 'far-fight-fit-v1'
PARAMETERS = ('rate_base', 'rate_per_tier', 'damage_scale', 'parry_per_tier', 'starter_rate')
GRID = ((.05, .2, .8), (0., .5, 2.), (.25, 1., 2.), (0., .25, .5), (.5, 1., 2.))
BOUNDS = ((.01, 1.6), (0., 4.), (.11, 4.), (0., 1.), (.125, 8.))
STEPS = (.1, .5, .25, .125, .5)
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


def cells_from_rows(rows, pairs, weapons, *, holdout=False):
    lookup = {frozenset(pair['npcs']): pair for pair in pairs}
    groups = defaultdict(list)
    for row in rows:
        pair = lookup[frozenset(row[role]['instance'] for role in ('attacker', 'defender'))]
        # Holdout contents never reach validation, adaptation or fitting in train-only mode.
        if pair['holdout'] != holdout:
            continue
        validate(row)
        groups[(pair['id'], row['attacker']['instance'], row['defender']['instance'])].append(row)
    cells = []
    for key, samples in sorted(groups.items()):
        actors = tuple(combatant(samples[0][role], weapons) for role in ('attacker', 'defender'))
        hp_max = tuple(samples[0][role]['hp_max'] for role in ('attacker', 'defender'))
        seeds = set()
        counts = [0, 0, 0]
        losses = [[], []]
        for row in samples:
            if row['seed'] in seeds:
                raise ValueError(f'Duplicate duel seed in {key}')
            seeds.add(row['seed'])
            if (tuple(combatant(row[role], weapons) for role in ('attacker', 'defender')) != actors
                    or tuple(row[role]['hp_max'] for role in ('attacker', 'defender')) != hp_max):
                raise ValueError(f'Combatants changed within cell {key}')
            result = outcome(row)
            counts[result] += 1
            if result < 2:
                side = row[('attacker', 'defender')[result]]
                losses[result].append((side['hp_before'] - side['hp_after']) / side['hp_max'])
        cells.append(dict(pair=key[0], attacker=key[1], defender=key[2], actors=actors,
                          hp_max=hp_max, n=len(samples), counts=counts, losses=losses,
                          deaths=sum(any(row[role]['hp_after'] <= 0 for role in ('attacker', 'defender'))
                                     for row in samples)))
    return cells


def log_likelihood(counts, probabilities):
    return sum(count * math.log(max(PROBABILITY_FLOOR, probability))
               for count, probability in zip(counts, probabilities, strict=True))


def trial_seeds(cell):
    seed = hashlib.sha256((':'.join((SEED_NAMESPACE, cell['pair'], cell['attacker'],
                                    cell['defender']))).encode()).digest()
    return [hashlib.sha256(seed + trial.to_bytes(4, 'big')).digest() for trial in range(DRAWS)]


class Evaluator:
    """Replay the scalar model's exact RNG stream in 400 parallel trials.

    Base rate scales the entire timeline, so a cached completed path can be censored
    at TIME_LIMIT_S for every base. Critical draws and exponentials are consumed in
    chronological event order, exactly as _duel (including skipped zero-rate draws).
    """
    def __init__(self, cells):
        self.cells = cells
        self._tapes = {}
        self.evaluations = 0

    def tape(self, index, positive):
        key = index, positive
        if key not in self._tapes:
            cell = self.cells[index]
            # Every hit removes >=1 HP. This bound includes the final knockout hit.
            length = sum(actor.hp for actor in cell['actors'])
            initial = np.full((2, DRAWS), math.inf)
            critical = np.empty((length, DRAWS), dtype=np.int16)
            exponential = np.empty((length, DRAWS))
            for trial, seed in enumerate(trial_seeds(cell)):
                rng = random.Random(int.from_bytes(seed, 'big'))
                for actor in range(2):
                    if positive[actor]:
                        initial[actor, trial] = rng.expovariate(1)
                for event in range(length):
                    critical[event, trial] = rng.randrange(100)
                    exponential[event, trial] = rng.expovariate(1)
            self._tapes[key] = initial, critical, exponential
        return self._tapes[key]

    @lru_cache(maxsize=8192)
    def path(self, index, tier, scale, parry, starter):
        actors = self.cells[index]['actors']
        rates = [(1 + tier * actor.tier) * max(0., 1 - parry * actors[1 - i].tier)
                 * (starter if i == 0 else 1) for i, actor in enumerate(actors)]
        initial, critical, exponential = self.tape(index, tuple(rate > 0 for rate in rates))
        clocks = initial.copy()
        for actor, rate in enumerate(rates):
            if rate:
                clocks[actor] /= rate
        hp = np.array([[actor.hp] * DRAWS for actor in actors], dtype=np.int64)
        winner = np.full(DRAWS, 2, dtype=np.int8)
        duration = np.full(DRAWS, math.inf)
        loss = np.zeros(DRAWS, dtype=np.int64)
        active = np.ones(DRAWS, dtype=bool) if any(rates) else np.zeros(DRAWS, dtype=bool)
        coefficients = Coefficients(1., tier, scale, parry, starter)
        damages = [[hit_damage(actors[i], actors[1 - i], crit, coefficients) for crit in (False, True)]
                   for i in range(2)]
        for event in range(critical.shape[0]):
            if not active.any():
                break
            chosen = np.where(clocks[0] < clocks[1], 0, 1)
            for attacker in range(2):
                trials = np.flatnonzero(active & (chosen == attacker))
                if not len(trials):
                    continue
                defender = 1 - attacker
                damage = np.where(critical[event, trials] < actors[attacker].hitchance,
                                  damages[attacker][1], damages[attacker][0])
                hp[defender, trials] = np.maximum(1, hp[defender, trials] - damage)
                ended = trials[hp[defender, trials] == 1]
                winner[ended] = attacker
                duration[ended] = clocks[attacker, ended]
                loss[ended] = actors[attacker].hp - hp[attacker, ended]
                active[ended] = False
                continuing = trials[hp[defender, trials] != 1]
                clocks[attacker, continuing] += exponential[event, continuing] / rates[attacker]
        if active.any():
            raise AssertionError('HP bound exhausted before knockout')
        return winner, duration, loss

    def simulate(self, index, coefficients):
        winner, duration, loss = self.path(index, coefficients.rate_per_tier, coefficients.damage_scale,
                                           coefficients.parry_per_tier, coefficients.starter_rate)
        winner = np.where(duration / coefficients.rate_base <= TIME_LIMIT_S, winner, 2)
        probabilities = [int(np.count_nonzero(winner == result)) / DRAWS for result in range(3)]
        return probabilities, winner, loss

    @lru_cache(maxsize=16384)
    def scores(self, coefficients):
        self.evaluations += 1
        return tuple(log_likelihood(cell['counts'], self.simulate(index, coefficients)[0])
                     for index, cell in enumerate(self.cells))


def candidate_grid(variant):
    axes = list(GRID[:3]) + [GRID[3] if variant in ('B', 'C') else (0.,),
                            GRID[4] if variant == 'C' else (1.,)]
    return [Coefficients(*values, variant) for values in itertools.product(*axes)]


def fit(evaluator, indices, variant):
    def score(coefficients):
        scores = evaluator.scores(coefficients)
        return sum(scores[index] for index in indices)

    def ranking(coefficients):
        return (-score(coefficients), *(getattr(coefficients, name) for name in PARAMETERS))

    best = min(candidate_grid(variant), key=ranking)
    dimensions = 3 if variant == 'A' else 4 if variant == 'B' else 5
    for refinement in range(3):
        while True:
            improved = False
            for dimension in range(dimensions):
                name = PARAMETERS[dimension]
                step = STEPS[dimension] / 2 ** refinement
                low, high = BOUNDS[dimension]
                choices = [best]
                for direction in (-1, 1):
                    value = round(getattr(best, name) + direction * step, 8)
                    if low <= value <= high:
                        choices.append(replace(best, **{name: value}))
                candidate = min(choices, key=ranking)
                # Resolve improving candidates lexicographically, but never walk a
                # likelihood plateau. Repeat complete sweeps to a coordinate optimum.
                if score(candidate) > score(best):
                    best = candidate
                    improved = True
            if not improved:
                break
    return best, score(best)


def calibrate(cells):
    if not cells:
        raise ValueError('No training cells')
    evaluator = Evaluator(cells)
    pairs = sorted({cell['pair'] for cell in cells})
    if len(pairs) < 2:
        raise ValueError('Cross-validation requires at least two training pairs')
    variants = []
    n = sum(cell['n'] for cell in cells)
    for variant in ('A', 'B', 'C'):
        folds = []
        for pair in pairs:
            training = [i for i, cell in enumerate(cells) if cell['pair'] != pair]
            testing = [i for i, cell in enumerate(cells) if cell['pair'] == pair]
            coefficients, training_ll = fit(evaluator, training, variant)
            scores = evaluator.scores(coefficients)
            folds.append(dict(pair=pair, training_pairs=sorted(set(pairs) - {pair}),
                              coefficients=asdict(coefficients), train_log_likelihood=training_ll,
                              log_likelihood=sum(scores[i] for i in testing),
                              n=sum(cells[i]['n'] for i in testing)))
            print(f'far-fight-fit: {variant}, held out {pair}, '
                  f'log likelihood {folds[-1]["log_likelihood"]:.6f}', file=sys.stderr, flush=True)
        final, training_ll = fit(evaluator, list(range(len(cells))), variant)
        cv_ll = sum(fold['log_likelihood'] for fold in folds)
        variants.append(dict(variant=variant, coefficients=asdict(final), folds=folds,
                             train_log_likelihood=training_ll, cv_log_likelihood=cv_ll,
                             cv_likelihood_per_duel=math.exp(cv_ll / n)))
    best = max(value['cv_log_likelihood'] for value in variants)
    selected = next(value for value in variants
                    if (best - value['cv_log_likelihood']) / n < math.log(1.01))
    coefficients = Coefficients(**selected['coefficients'])
    return dict(selected=selected['variant'], coefficients=asdict(coefficients), variants=variants,
                training_records=n, training_pairs=pairs, evaluations=evaluator.evaluations), evaluator


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
    probabilities, winner, loss = evaluator.simulate(index, coefficients)
    medians = [float(np.median(loss[winner == role] / cell['hp_max'][role]))
               if np.any(winner == role) else None for role in range(2)]
    observed = [statistics.median(values) if values else None for values in cell['losses']]
    pooled_observed = [value for values in cell['losses'] for value in values]
    pooled_model = [float(loss[trial] / cell['hp_max'][role])
                    for trial, role in enumerate(winner) if role < 2]
    return dict(pair=cell['pair'], attacker=cell['attacker'], defender=cell['defender'], n=cell['n'],
                counts=cell['counts'], probabilities=probabilities, deaths=cell['deaths'],
                observed_winner_loss=observed, model_winner_loss=medians,
                observed_median_winner_loss=statistics.median(pooled_observed) if pooled_observed else None,
                model_median_winner_loss=statistics.median(pooled_model) if pooled_model else None,
                log_likelihood=log_likelihood(cell['counts'], probabilities))


def accept(cells, training, coefficients, pairs):
    expected = {(pair['id'], a, b) for pair in pairs if pair['holdout']
                for a, b in (pair['npcs'], pair['npcs'][::-1])}
    actual = {(cell['pair'], cell['attacker'], cell['defender']) for cell in cells}
    if actual != expected:
        raise ValueError(f'Holdout cells incomplete: expected {len(expected)}, got {len(actual)}')
    evaluator = Evaluator(cells)
    total = sum(cell['n'] for cell in training)
    constant = [sum(cell['counts'][result] for cell in training) / total for result in range(3)]
    results = []
    for index, cell in enumerate(cells):
        result = cell_result(cell, evaluator, index, coefficients)
        result['intervals'] = {name: binomial_interval(cell['n'], result['probabilities'][outcome_id])
                               for name, outcome_id in (('attacker_wins', 0), ('timeouts', 2))}
        result['count_checks'] = {name: interval[0] <= cell['counts'][outcome_id] <= interval[1]
                                  for (name, outcome_id), interval in zip(
                                      (('attacker_wins', 0), ('timeouts', 2)),
                                      result['intervals'].values(), strict=True)}
        result['winner_counts'] = [len(values) for values in cell['losses']]
        result['hp_check'] = None if sum(result['winner_counts']) < 3 else (
            result['model_median_winner_loss'] is not None
            and abs(result['model_median_winner_loss'] - result['observed_median_winner_loss']) <= .15)
        result['uncalibrated_log_likelihood'] = log_likelihood(
            cell['counts'], evaluator.simulate(index, UNCALIBRATED)[0])
        result['constant_log_likelihood'] = log_likelihood(cell['counts'], constant)
        result['passed'] = all(result['count_checks'].values()) and result['hp_check'] is not False
        results.append(result)
    n = sum(cell['n'] for cell in cells)
    scores = {name: sum(cell[name] for cell in results) for name in (
        'log_likelihood', 'uncalibrated_log_likelihood', 'constant_log_likelihood')}
    better = scores['log_likelihood'] > max(scores['uncalibrated_log_likelihood'],
                                           scores['constant_log_likelihood'])
    return dict(passed=better and all(cell['passed'] for cell in results), cells=results,
                records=n, constant_probabilities=constant, better_than_baselines=better,
                **scores, likelihood_per_duel={name: math.exp(score / n) for name, score in scores.items()})


def run(folders, weapons, pairs, *, acceptance=False):
    rows = []
    for folder in sorted(map(Path, folders)):
        for line in (folder / 'events.jsonl').read_text().splitlines():
            row = json.loads(line)
            if row.get('kind') == 'npc_duel_error':
                raise ValueError(row['reason'])
            if row.get('kind') == 'npc_duel':
                rows.append(row)
    training = cells_from_rows(rows, pairs, weapons)
    result, evaluator = calibrate(training)
    coefficients = Coefficients(**result['coefficients'])
    result['settings'] = dict(draws=DRAWS, probability_floor=PROBABILITY_FLOOR,
                              time_limit_s=TIME_LIMIT_S, seed_namespace=SEED_NAMESPACE,
                              grid=dict(zip(PARAMETERS, GRID, strict=True)),
                              bounds=dict(zip(PARAMETERS, BOUNDS, strict=True)),
                              refinement_steps=STEPS, refinement_levels=3,
                              refinement_strategy=('repeat coordinate sweeps at each step size until no strict '
                                                   'log-likelihood improvement; lexicographic candidate ties, '
                                                   'no plateau moves'),
                              selection_threshold='simplest within 1% of best CV likelihood per duel')
    result['training_cells'] = [cell_result(cell, evaluator, i, coefficients)
                                for i, cell in enumerate(training)]
    train_rows = [row for row in rows if frozenset(row[role]['instance'] for role in ('attacker', 'defender'))
                  in {frozenset(pair['npcs']) for pair in pairs if not pair['holdout']}]
    result['training_summary'] = summarize(train_rows, pairs)
    result['folders'] = [Path(folder).name for folder in sorted(map(Path, folders))]
    result['acceptance'] = (accept(cells_from_rows(rows, pairs, weapons, holdout=True), training,
                                   coefficients, pairs) if acceptance else None)
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
    print(json.dumps({key: result[key] for key in ('selected', 'coefficients', 'training_records')}))
    if result['acceptance'] is not None:
        print(json.dumps({'acceptance': result['acceptance']['passed'],
                          'records': result['acceptance']['records']}))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())

"""Final navigation comparisons require the complete regional test coverage."""
import importlib.util
from pathlib import Path

import pytest


SPEC = importlib.util.spec_from_file_location("nav_ppo_report", Path(__file__).resolve().parents[1] / "nav_ppo_report.py")
report = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(report)


def measured_pairs():
    return {(f"{region}-S{index}", f"{region}-G{index}"): dict(region=region)
            for region in report.REGIONS for index in range(10)}


def test_final_comparison_accepts_sixty_pairs_with_ten_per_region():
    baseline = measured_pairs()
    final = {key: dict(episode) for key, episode in baseline.items()}
    assert report.matched_episodes(baseline, final) == sorted(baseline, key=lambda key: (baseline[key]["region"], *key))


def test_final_comparison_rejects_identically_incomplete_fifty_nine_pairs():
    baseline = measured_pairs()
    baseline.pop(next(iter(baseline)))
    final = {key: dict(episode) for key, episode in baseline.items()}
    with pytest.raises(ValueError, match="exactly 60 pairs"):
        report.matched_episodes(baseline, final)


def test_final_comparison_rejects_sixty_pairs_with_wrong_regional_distribution():
    baseline = measured_pairs()
    baseline[("harbour-S0", "harbour-G0")]["region"] = "market"
    final = {key: dict(episode) for key, episode in baseline.items()}
    assert len(baseline) == 60
    with pytest.raises(ValueError, match="ten in each"):
        report.matched_episodes(baseline, final)


def outcome_pairs(success=True, mode='sample', seed=18102027):
    return {key: dict(episode, start=key[0], goal=key[1], success=success,
                      action_mode=mode, sampling_base_seed=seed)
            for key, episode in measured_pairs().items()}


def test_selection_includes_recovery_regression_and_persistent_failure_with_six_regions():
    baseline, final = outcome_pairs(), outcome_pairs()
    # Recoveries in every region must not crowd out the less flattering cases.
    for region in report.REGIONS:
        baseline[(f'{region}-S0', f'{region}-G0')]['success'] = False
    regression = ('market-S1', 'market-G1')
    persistent = ('harbour-S1', 'harbour-G1')
    final[regression]['success'] = False
    baseline[persistent]['success'] = final[persistent]['success'] = False
    selected = report.select_comparisons(baseline, final)
    assert len(selected) == len(set(selected)) == 6
    assert regression in selected and persistent in selected
    assert {report.comparison_category(baseline[key], final[key]) for key in selected} >= {
        'recovered', 'regression', 'persistent_failure'}
    assert {baseline[key]['region'] for key in selected} == set(report.REGIONS)
    assert selected == report.select_comparisons(dict(reversed(list(baseline.items()))), final)


def test_sample_context_requires_the_same_recorded_seed_and_action_mode():
    baseline, final = outcome_pairs(), outcome_pairs()
    assert report.comparison_context(baseline, final) == dict(action_mode='sample', seed=18102027)
    key = next(iter(final))
    final[key]['sampling_base_seed'] += 1
    with pytest.raises(ValueError, match='same recorded base seed'):
        report.comparison_context(baseline, final)
    final[key]['sampling_base_seed'] -= 1
    final[key]['action_mode'] = 'argmax'
    with pytest.raises(ValueError, match='same recorded action mode'):
        report.comparison_context(baseline, final)


def test_load_drive_reads_actual_sample_mode_and_unoffsets_original_index(tmp_path):
    import json
    directory = tmp_path / 'route'
    directory.mkdir()
    (directory / 'trajectory.txt').touch()
    (directory / 'episode.json').write_text(json.dumps(dict(action_mode='sample', mixing_seed=18102034, index=7)))
    (tmp_path / 'metrics.json').write_text(json.dumps(dict(episodes=[dict(start='A', goal='B', region='market',
        episode=str(directory), index=7, mixing_seed=18102034)])))
    episode = report.load_drive(tmp_path)[('A', 'B')]
    assert episode['action_mode'] == 'sample'
    assert episode['sampling_base_seed'] == 18102027


def test_curve_events_include_retune_28_and_manual_stop_from_durable_state(tmp_path):
    import json
    state = dict(iteration=44, stop_reason='manual_stop_after_validation_decline', parameter_changes=[
        dict(after_iteration=27, effective_from_iteration=28, decisions=8_643_602,
             current=dict(actor_lr=1e-4, batch_windows=32), previous=dict(actor_lr=3e-5))])
    (tmp_path / 'state.json').write_text(json.dumps(state))
    events = report.curve_events(tmp_path, [dict(iteration=44, stop_reason=None)])
    assert events['parameter_changes'][0]['iteration'] == 28
    assert events['stops'] == [dict(iteration=44, reason='manual_stop_after_validation_decline')]


def test_pair_plots_include_six_individual_files_and_share_coordinates_and_labels(tmp_path, monkeypatch):
    import numpy as np
    pytest.importorskip('matplotlib')
    baseline, final = outcome_pairs(), outcome_pairs()
    for episode in [*baseline.values(), *final.values()]:
        episode.update(target_position=[1000, 0, 2000], elapsed_s=12, observed_reason='no_motion')
    selected = report.select_comparisons(baseline, final)
    monkeypatch.setattr(report, 'trajectory', lambda _: np.array([[0, 0, 0, 0, 22], [1000, 800, 0, 900, 22]]))
    saved = []
    def save(fig, destination):
        saved.append((destination, [axis.get_title() for axis in fig.axes]))
        for left, right in zip(fig.axes[::2], fig.axes[1::2]):
            assert left.get_xlim() == right.get_xlim()
            assert left.get_ylim() == right.get_ylim()
            assert 'sample' in left.get_title() and 'seed 18102027' in left.get_title()
            assert 'iter35' in right.get_title()
        assert any('Illustrative' in text.get_text() for text in fig.texts)
        return [str(destination.with_suffix('.svg')), str(destination.with_suffix('.png'))]
    monkeypatch.setattr(report, 'save_figure', save)
    paths = report.plot_comparison(baseline, final, selected, tmp_path, final_label='iter35')
    assert len(paths) == 14
    assert len([path for path, _ in saved if path.name.startswith('comparison-')]) == 6
    assert saved[-1][0].name == 'comparison'


def test_plots_only_preserves_existing_readme_and_metrics(tmp_path, monkeypatch):
    output = tmp_path / 'out'
    output.mkdir()
    (output / 'README.md').write_text('complete human report\n')
    (output / 'final_metrics.json').write_text('existing aggregate\n')
    baseline, final = outcome_pairs(), outcome_pairs()
    monkeypatch.setattr(report, 'load_curves', lambda _: [dict(iteration=35)])
    monkeypatch.setattr(report, 'load_drive', lambda path: baseline if path.name == 'baseline' else final)
    monkeypatch.setattr(report, 'plot_curves', lambda *args: [])
    monkeypatch.setattr(report, 'plot_comparison', lambda *args: [])
    monkeypatch.setattr(report, 'final_metrics', lambda *args: pytest.fail('plots-only recomputed aggregate metrics'))
    monkeypatch.setattr('sys.argv', ['nav_ppo_report.py', '--run', str(tmp_path / 'run'),
        '--baseline', str(tmp_path / 'baseline'), '--final', str(tmp_path / 'final'),
        '--output', str(output), '--plots-only', '--final-label', 'iter35', '--action-mode', 'sample', '--seed', '18102027'])
    report.main()
    assert (output / 'README.md').read_text() == 'complete human report\n'
    assert (output / 'final_metrics.json').read_text() == 'existing aggregate\n'
    assert report.read_json(output / 'comparison_pairs.json')['purpose'] == 'illustrative_pairs_not_aggregate'


def test_paired_bootstrap_keeps_three_seed_repeats_in_one_route_cluster():
    import numpy as np
    seeds = report.BATCH_SEEDS
    baseline = {seed: outcome_pairs(False, seed=seed) for seed in seeds}
    final = {seed: outcome_pairs(False, seed=seed) for seed in seeds}
    # One route improves in only one seed: its cluster effect is 1/3 of a route.
    final[seeds[0]][('market-S0', 'market-G0')]['success'] = True
    result = report.paired_route_bootstrap(baseline, final, resamples=2000, rng_seed=17)
    assert result['mean_net_arrivals_per_60'] == pytest.approx(1 / 3)
    rng = np.random.default_rng(17)
    oracle = np.zeros(2000)
    for region in report.REGIONS:
        draw = rng.integers(0, 10, (2000, 10))
        if region == 'market':
            oracle += (draw == 0).sum(axis=1) / 3
    np.testing.assert_allclose(result['ci95_net_arrivals_per_60'], np.quantile(oracle, [.025, .975]))
    assert result['route_clusters'] == 60 and result['seeds_per_cluster'] == 3
    assert result['units'] == 'routes_per_60'
    assert result == report.paired_route_bootstrap(baseline, final, resamples=2000, rng_seed=17)


def test_batch_summary_keeps_each_seed_pools_180_and_reports_paired_changes(tmp_path, monkeypatch):
    def load(directory):
        policy, mode, seed = directory.parts[-3:]
        values = outcome_pairs(mode=mode, seed=int(seed))
        if policy == 'iter35':
            values[('market-S0', 'market-G0')]['success'] = False
        return values
    calls = []
    def summary(episodes):
        calls.append(len(episodes))
        return dict(n=len(episodes), arrived=sum(episode['success'] for episode in episodes))
    monkeypatch.setattr(report, 'load_drive', load)
    monkeypatch.setattr(report, 'measured_summary', summary)
    result = report.summarize_batch(tmp_path)
    assert result['measurements'] == 16
    for split in ('validation', 'test'):
        values = result['splits'][split]
        for policy, arrived in (('round2', 180), ('iter35', 177)):
            sample = values['policies'][policy]['sample']
            assert sample['pooled']['all'] == dict(n=180, arrived=arrived)
            assert set(sample['per_seed']) == set(map(str, report.BATCH_SEEDS))
            assert all(region['n'] == 30 for region in sample['pooled']['by_region'].values())
        assert all(value['regressions'] == 1 and value['net'] == -1
                   for value in values['sample_pairwise_by_seed'].values())
        assert values['sample_paired_route_bootstrap']['mean_net_arrivals_per_60'] == -1
    assert 180 in calls and 30 in calls and 60 in calls and 10 in calls


def test_batch_cli_can_write_summary_without_plot_inputs(tmp_path, monkeypatch):
    expected = dict(measurements=16, marker='batch-only')
    monkeypatch.setattr(report, 'summarize_batch', lambda _: expected)
    monkeypatch.setattr(report, 'load_curves', lambda _: pytest.fail('batch-only loaded training curves'))
    monkeypatch.setattr('sys.argv', ['nav_ppo_report.py', '--batch-root', str(tmp_path / 'final'), '--output', str(tmp_path / 'out')])
    report.main()
    assert report.read_json(tmp_path / 'out/batch_metrics.json') == expected


def test_curve_plot_marks_retune_and_stop_and_displays_kl_coefficient(tmp_path, monkeypatch):
    import json
    import numpy as np
    pytest.importorskip('matplotlib')
    rows = []
    for iteration, coefficient in ((1, .1), (27, .083), (28, .083), (35, .02)):
        rows.append(dict(iteration=iteration, decisions=iteration * 320000, critic_only=iteration == 1,
            validation=None, stop_reason=None, kl_coefficient=coefficient,
            training=dict(mean_reward=.1, kl_reference=.001, turn_in_place_fraction=.3, blocked_fraction=.1,
                          entropy_by_head={head: .5 for head in report.HEADS},
                          reward_components={'progress_cm': .1}, reward_component_abs_fraction={'progress_cm': 1})))
    (tmp_path / 'state.json').write_text(json.dumps(dict(iteration=35, stop_reason='manual_stop', parameter_changes=[
        dict(effective_from_iteration=28, current=dict(actor_lr=1e-4, batch_windows=32))])))
    (tmp_path / 'best').mkdir()
    (tmp_path / 'best/selection.json').write_text(json.dumps(dict(best_iteration=35)))
    def save(fig, destination):
        assert len(fig.axes) == 9
        for axis in fig.axes[:8]:
            assert any(len(line.get_xdata()) == 2 and np.all(np.asarray(line.get_xdata()) == 28)
                       for line in axis.lines)
        assert fig.axes[-1].get_yscale() == 'log'
        title = '\n'.join(text.get_text() for text in fig.texts)
        assert 'retune from 28' in title and 'Stop at iteration 35: manual_stop' in title
        return [str(destination.with_suffix('.svg')), str(destination.with_suffix('.png'))]
    monkeypatch.setattr(report, 'save_figure', save)
    assert len(report.plot_curves(tmp_path, rows, tmp_path)) == 2

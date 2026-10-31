"""Physical detail matching catches density and orientation mistakes."""
import pytest

from material_scale import match_detail_scale, panel_canvas, target_detail_plan


def row(**changes):
    return dict(decision='include', **{'class': 'roof_shingle'}, uv_m_per_unit_median='8.62',
                target_period_u_uv='.25', target_period_v_uv='.125', target_direction='U', **changes)


def test_tile_counts_and_scale_preserve_both_physical_periods():
    target = row()
    plan = target_detail_plan(target, 2.)
    assert plan['scale'] == pytest.approx(4.)
    for axis in ('u', 'v'):
        generated_period = 1 / plan[f'details_{axis}_per_tile']
        assert generated_period * 8.62 / plan['scale'] == pytest.approx(plan[f'detail_{axis}_metres'])


def test_roof_integer_counts_adjust_class_scale_and_meet_both_axes():
    target = row()
    target.update(target_period_u_uv=str(1/3), target_period_v_uv='.1')
    plan = target_detail_plan(target, 2.)
    assert plan['details_u_per_tile'] >= 1 and plan['details_v_per_tile'] >= 1
    assert plan['details_u_per_tile'] == int(plan['details_u_per_tile'])
    for axis in ('u', 'v'):
        assert abs(plan[f'planned_detail_{axis}_metres'] / plan[f'detail_{axis}_metres'] - 1) <= .15


def test_equal_paving_periods_do_not_gain_an_avoidable_aspect_distortion():
    target = row()
    target.update(uv_m_per_unit_median='6', target_period_u_uv='.0625', target_period_v_uv='.0625')
    plan = target_detail_plan(target, 2.)
    assert plan['scale'] == pytest.approx(3.2)
    assert plan['details_u_per_tile'] == plan['details_v_per_tile'] == 5


def test_existing_density_can_be_fixed_by_one_scale():
    result = match_detail_scale(row(), dict(period_u_uv=.125, period_v_uv=.0625, direction='U'))
    assert result['scale'] == pytest.approx(.5)
    assert result['status'] == 'within_15_percent'


def test_incompatible_density_or_wrong_direction_is_rejected():
    measured = dict(period_u_uv=.125, period_v_uv=.125, direction='U')
    assert match_detail_scale(row(), measured)['status'] == 'incompatible_axes'
    measured.update(period_v_uv=.0625, direction='V')
    assert match_detail_scale(row(), measured)['status'] == 'direction_mismatch'


def test_module_cannot_change_scale_to_hide_wrong_detail_count():
    target = row()
    target['decision'] = 'module'
    assert target_detail_plan(target)['scale'] == 1
    result = match_detail_scale(target, dict(period_u_uv=.125, period_v_uv=.0625, direction='U'))
    assert result['scale'] == 1
    assert result['status'] == 'regenerate_details'


def test_missing_and_nonrepeating_are_not_claimed_to_pass():
    assert match_detail_scale(row(), dict(period_u_uv=0., period_v_uv=.125, direction='U'))['status'] == 'missing_period'
    target = row()
    target.update(target_period_u_uv='0', target_period_v_uv='0', target_direction='none')
    assert match_detail_scale(target, dict(period_u_uv=0., period_v_uv=0., direction='none'))['status'] == 'no_period'


def test_roof_panel_canvas_never_stretches_source_art_by_125_percent():
    plan = panel_canvas(14.48, 6.42)
    assert (plan['width'], plan['height'], plan['square_modules']) == (1536, 681, 3)
    assert plan['source_to_physical_stretch'] < 1.001

"""District names of waypoints and per-district area shares."""

import texture_districts as td


def test_named_spots_win_over_generic_prefixes():
    assert td.district('NW_CITY_HABOUR_KASERN_CENTRE_04') == 'barracks'
    assert td.district('NW_CITY_HABOUR_POOR_AREA_PATH_17') == 'harbour'
    assert td.district('NW_CITY_MERCHANT_TEMPLE_IN') == 'temple'
    assert td.district('NW_CITY_MERCHANT_PATH_38') == 'lower'
    assert td.district('NW_CITY_HAGEN') == 'upper'
    assert td.district('NW_CITY_PIRATESCAMP_03') == 'outside'


def test_unknown_city_name_defers_and_outside_names_are_outside():
    assert td.district('NW_CITY_BEER_02') is None
    assert td.district('NW_BIGFARM_HOUSE_02') == 'outside'


def test_unknown_waypoint_defers_to_nearest_known_one():
    locate = td.Locator([
        {'name': 'NW_CITY_BEER_02', 'position': [0, 0, 0]},
        {'name': 'NW_CITY_UPTOWN_PATH_01', 'position': [500, 0, 0]},
        {'name': 'NW_CITY_HABOUR_02', 'position': [-900, 0, 0]},
    ])
    assert locate([10, 0, 0]) == 'upper'
    assert locate([-800, 0, 0]) == 'harbour'


def test_shares_sum_area_per_district():
    locate = td.Locator([
        {'name': 'NW_CITY_HABOUR_02', 'position': [0, 0, 0]},
        {'name': 'MARKT', 'position': [1000, 0, 0]},
    ])
    table = td.shares([('ROOF.TGA', [10, 0, 0], 3.0), ('ROOF.TGA', [990, 0, 0], 1.0)], locate)
    assert table['ROOF.TGA']['harbour'] == 3.0
    assert table['ROOF.TGA']['lower'] == 1.0
    assert sum(table['ROOF.TGA'].values()) == 4.0


def test_centre_area_of_a_right_triangle_in_centimetres():
    centre, area = td.centre_area([[0, 0, 0], [300, 0, 0], [0, 0, 200]])
    assert centre == [100.0, 0.0, 200 / 3]
    assert area == 3.0

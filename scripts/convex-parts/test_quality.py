"""Cupboard void gates on small synthetic shelf units, in centimetres."""
import numpy as np

from quality import void_checks


def box(x0, x1, y0, y1, z0, z1):
    corners = np.array([(x, y, z) for x in (x0, x1) for y in (y0, y1) for z in (z0, z1)], dtype=float)
    faces = [(0, 1, 3), (0, 3, 2), (4, 6, 7), (4, 7, 5), (0, 4, 5), (0, 5, 1),
             (2, 3, 7), (2, 7, 6), (0, 2, 6), (0, 6, 4), (1, 5, 7), (1, 7, 3)]
    return corners, faces


def unit(boards):
    """Horizontal boards between two side walls and a back wall; the front (-z) stays open."""
    pieces = [box(-50, 50, y0, y1, -30, 30) for y0, y1 in boards]
    top = max(y1 for _, y1 in boards)
    pieces += [box(-50, -45, 0, top, -30, 30), box(45, 50, 0, top, -30, 30), box(-50, 50, 0, top, 25, 30)]
    vertices, triangles = [], []
    for corners, faces in pieces:
        triangles += [[len(vertices) * 8 + i for i in face] for face in faces]
        vertices.append(corners)
    return np.concatenate(vertices), np.asarray(triangles), [corners for corners, _ in pieces]


def test_an_opening_the_sphere_cannot_enter_in_the_source_counts_as_closed():
    # Two 6 cm slots: a 10 cm sphere fits neither, so filling them is no failure.
    vertices, triangles, _ = unit([(0, 5), (11, 16), (22, 27)])
    checks = void_checks(vertices, triangles, [vertices], "cupboard")
    assert [check["label"] for check in checks] == ["closed-cupboard"]
    assert checks[0]["passed"]
    openings = checks[0]["source_openings_below_radius"]
    assert [round(opening["gap_cm"], 6) for opening in openings] == [6, 6]
    assert all(opening["source_clearance_cm"] < 10 for opening in openings)


def test_a_real_open_shelf_still_fails_when_the_parts_fill_it():
    vertices, triangles, boxes = unit([(0, 5), (45, 50)])
    filled = void_checks(vertices, triangles, [vertices], "cupboard")
    assert [check["label"] for check in filled] == ["open-shelf-1"]
    assert filled[0]["source_clearance_cm"] >= 10 and not filled[0]["passed"]
    kept = void_checks(vertices, triangles, boxes, "cupboard")
    assert kept[0]["label"] == "open-shelf-1" and kept[0]["passed"]

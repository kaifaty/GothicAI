import importlib.util
from pathlib import Path

SPEC = importlib.util.spec_from_file_location("strips", Path(__file__).parents[1] / "explain/scripts/strips.py")
strips = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(strips)


def plan(*beats):
    return {"beats": list(beats)}


def notes(*beats):
    return [row.split(" | ")[2] for row in strips.language(plan(*beats))]


def test_defaults_and_durations_from_the_set_pass():
    assert notes(
        {"id": "a", "dur": 4, "texts": [{"at": 0}, {"at": 0.08}], "tags": [{"at": 1}],
         "paths": [{"at": 2, "dur": 0.6}], "fans": [{"at": 3}]},
        {"id": "b", "dur": 4, "enter": {"type": "slide"}, "texts": [{"at": 0.5}],
         "zoom": {"at": 1, "dur": 0.6, "scale": 1.2, "x": 0, "y": 0}},
        {"id": "c", "dur": 4, "zoom": {"at": 0, "dur": 3.5, "scale": 1.05, "x": 0, "y": 0, "ease": "linear"}},
        {"id": "d", "dur": 2, "enter": {"type": "flash"}, "tags": [{"at": 0}],
         "zoom": {"at": 0, "dur": 0.01, "scale": 1.3, "x": 0, "y": 0}},
    ) == []


def test_durations_off_the_set_are_named():
    rows = strips.language(plan(
        {"id": "a", "dur": 4, "enter": {"type": "fade", "dur": 0.45}, "paths": [{"at": 1, "dur": 0.9}],
         "strokes": [{"at": 2, "dur": 0.4}], "zoom": {"at": 3, "dur": 1.0, "scale": 1.2, "x": 0, "y": 0}},
        {"id": "b", "dur": 4, "zoom": {"at": 0, "dur": 1.5, "scale": 1.05, "x": 0, "y": 0, "ease": "linear"},
         "carry": {"from": [0, 0], "to": [1, 1], "text": "x", "dur": 0.9}},
    ))
    text = "\n".join(rows)
    assert "| a | 0.00 | enter-fade | 0.45 s | not from the set" in text
    assert "| path0 | 0.9 s | longer than slow 0.6" in text
    assert "| stroke0 | 0.4 s | not from the set" in text
    assert "| a | 0.00 | zoom | 1 s | an eased zoom takes slow 0.6" in text
    assert "| b | 4.00 | zoom | 1.5 s of 4 | a linear zoom is a slow push" in text
    assert "| carry | 0.9 s | a carry takes 0.35-0.6 s" in text


def test_two_entrances_at_once_and_entrances_under_a_transition():
    assert notes({"id": "a", "dur": 3, "texts": [{"at": 1}], "tags": [{"at": 1.05}]}) == ["text0 + tag0"]
    assert notes({"id": "a", "dur": 3, "enter": {"type": "wipe"}, "tags": [{"at": 0.3}, {"at": 0.5}]}) == ["tag0"]
    assert notes({"id": "a", "dur": 3, "enter": {"type": "fade", "dur": 0.2}, "texts": [{}]}) == ["text0"]


def picture(value, caption=None):
    w, h = strips.SMALL
    rows = [bytes([value]) * w for _ in range(h)]
    if caption is not None:
        top, bottom = strips.CAPTION_BAND["tall"]
        for r in range(h):
            if top * h <= r < bottom * h:
                rows[r] = bytes([caption]) * w
    return b"".join(rows)


def test_stills_ignore_the_caption_band_and_short_holds():
    fps = 10
    # 4 s still while the captions change every frame, then 1 s of motion, then a 2 s hold
    frames = [picture(50, caption=i % 2 * 200) for i in range(40)]
    frames += [picture(60 + 10 * i) for i in range(10)]
    frames += [picture(160)] * 20
    assert strips.stills(frames, fps, strips.CAPTION_BAND["tall"]) == [(0, 39)]
    assert strips.stills(frames, fps, strips.CAPTION_BAND["tall"], least=1.5) == [(0, 39), (50, 69)]


def test_slow_footage_is_not_still():
    w, h = strips.SMALL
    # a hero idling: a few pixels change a little every frame, about 0.06 of 255 on average
    base = bytearray(picture(80))
    frames = []
    for i in range(60):
        f = bytearray(base)
        for k in range(0, w * h, 17):
            f[k] = 80 + (i % 2)
        frames.append(bytes(f))
    assert strips.stills(frames, 30, strips.CAPTION_BAND["tall"], least=1) == []


def test_hold_push_follows_timing_ts():
    assert strips.hold_push({"kind": "card", "dur": 2}) == (0.0, 1.03)
    assert strips.hold_push({"kind": "card", "dur": 10}) == (0.0, 1.05)
    assert strips.hold_push({"kind": "footage", "dur": 5, "freeze": 1}) == (1, 1.04)
    assert strips.hold_push({"kind": "footage", "dur": 8}) is None
    assert strips.hold_push({"kind": "card", "dur": 6, "push": 0}) is None
    assert strips.hold_push({"kind": "still", "dur": 6, "zoom": {"at": 0}}) is None
    assert notes({"id": "a", "dur": 4, "push": 0.01}) == ["push"]


def test_a_hold_names_its_beats_and_their_push():
    p = plan({"id": "foot", "kind": "footage", "dur": 4}, {"id": "card", "kind": "card", "dur": 6},
             {"id": "quiet", "kind": "card", "dur": 6, "push": 0})
    assert strips.hold_beats(p, 5, 9) == ("card", "yes")
    assert strips.hold_beats(p, 3, 6) == ("foot, card", "part")
    assert strips.hold_beats(p, 11, 15) == ("quiet", "no")

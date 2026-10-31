import json
import sys
from types import SimpleNamespace

import pytest
from test_timeline import EVENTS

from gothic_shorts import cli, compare, cut, srt
from gothic_shorts.shot import Shot
from gothic_shorts.studio import StudioError


def test_timestamps():
    assert srt.timestamp(0) == "00:00:00,000"
    assert srt.timestamp(3725.4567) == "01:02:05,457"


def test_cues_numbered_in_time_order():
    text = srt.render([srt.Cue(2.0, 3.0, "b"), srt.Cue(0.5, 1.25, " a ")])
    assert text == "1\n00:00:00,500 --> 00:00:01,250\na\n\n2\n00:00:02,000 --> 00:00:03,000\nb\n"


def test_explainer_lines_over_their_russian_voice():
    words = {"lines": [
        {"words": [{"text": "В", "start": 1.0, "end": 1.2}, {"text": "Готике", "start": 1.2, "end": 4.0}]},
        {"words": [{"text": "Да", "start": 5.0, "end": 6.0}]},
    ]}  # fmt: skip
    cues = srt.explainer(words, ["In Gothic | you don't aim", "Yes"])
    assert [c.text for c in cues] == ["In Gothic", "you don't aim", "Yes"]
    # each chunk as long as its share of the letters: 9 and 13 of 22
    assert (cues[0].start, cues[0].end, cues[1].end) == (1.0, pytest.approx(1 + 3 * 9 / 22, abs=1e-3), 4.0)
    assert (cues[2].start, cues[2].end) == (5.0, 6.0)
    with pytest.raises(ValueError):
        srt.explainer(words, ["one line"])


@pytest.fixture
def compare_plan():
    return {"beats": [
        {"segment": "first", "start": 1.2, "end": 5.7, "lines": [
            {"at": 1.2, "file": "voice/01.wav", "words": [["Вода", 0.15, 1.2]]},
        ]},
        {"segment": "second", "start": 0.1, "end": 5.3, "lines": [
            {"at": 0.2, "file": "voice/02.wav", "words": [["Ящик", 0.72, 1.2]]},
        ]},
    ]}  # fmt: skip


def _studio_words():
    return {"lines": [
        {"n": 1, "file": "voice/01.wav", "start": 0,
         "words": [{"text": "Вода", "start": 0.15, "end": 1.2}]},
        {"n": 2, "file": "voice/02.wav", "start": 1.55,
         "words": [{"text": "Ящик", "start": 2.27, "end": 2.75}]},
    ]}  # fmt: skip


@pytest.mark.parametrize("plan_name", ["plan.json", "comparison.json"])
@pytest.mark.parametrize("words_source", ["studio", "missing", "invalid"])
def test_comparison_workspace_subtitles_follow_the_cut(
    tmp_path, monkeypatch, compare_plan, plan_name, words_source,
):
    (tmp_path / plan_name).write_text(json.dumps(compare_plan))
    (tmp_path / "script.en.txt").write_text("Water\nCrate\n")
    if words_source == "studio":
        (tmp_path / "words.ru.json").write_text(json.dumps(_studio_words()))
    elif words_source == "invalid":
        (tmp_path / "words.ru.json").write_text("not JSON: comparison subtitles must not read this")
    argv = ["gothic-shorts", "srt", str(tmp_path)]
    if plan_name != "plan.json":
        argv += ["--plan", plan_name]
    monkeypatch.setattr(sys, "argv", argv)

    with pytest.raises(SystemExit) as done:
        cli.main()

    assert done.value.code == 0
    assert (tmp_path / "subs.en.srt").read_text() == (
        "1\n00:00:00,150 --> 00:00:01,200\nWater\n\n"
        "2\n00:00:05,320 --> 00:00:05,800\nCrate\n"
    )


@pytest.mark.parametrize("plan_text", [None, '{"scenes": []}', "not JSON"])
def test_explainer_workspace_subtitles_keep_the_studio_timeline(tmp_path, monkeypatch, plan_text):
    if plan_text is not None:
        (tmp_path / "plan.json").write_text(plan_text)
    (tmp_path / "words.ru.json").write_text(json.dumps(_studio_words()))
    (tmp_path / "script.en.txt").write_text("Water\nCrate\n")
    monkeypatch.setattr(sys, "argv", ["gothic-shorts", "srt", str(tmp_path)])

    with pytest.raises(SystemExit) as done:
        cli.main()

    assert done.value.code == 0
    assert (tmp_path / "subs.en.srt").read_text() == (
        "1\n00:00:00,150 --> 00:00:01,200\nWater\n\n"
        "2\n00:00:02,270 --> 00:00:02,750\nCrate\n"
    )


def test_placed_lines_include_unvoiced_beats_in_the_offset(compare_plan):
    compare_plan["beats"].insert(1, {"segment": "silent", "start": 10.2, "end": 11.43456})
    original = json.dumps(compare_plan)

    assert compare.placed_lines(compare_plan) == [
        {"n": 1, "file": "voice/01.wav", "start": 0,
         "words": [{"text": "Вода", "start": 0.15, "end": 1.2}]},
        {"n": 2, "file": "voice/02.wav", "start": 5.835,
         "words": [{"text": "Ящик", "start": 6.555, "end": 7.035}]},
    ]
    assert json.dumps(compare_plan) == original


@pytest.mark.parametrize("translated", ["", "Water\n", "Water\nCrate\nAnother\n"])
def test_comparison_subtitles_reject_a_different_number_of_translated_lines(
    tmp_path, compare_plan, translated,
):
    (tmp_path / "plan.json").write_text(json.dumps(compare_plan))
    (tmp_path / "script.en.txt").write_text(translated)
    args = SimpleNamespace(short=str(tmp_path), take=None, lang="en", plan="plan.json")

    with pytest.raises(StudioError) as error:
        cli.cmd_srt(args)

    assert "script.en.txt" in str(error.value)
    assert "2" in str(error.value)
    assert not (tmp_path / "subs.en.srt").exists()


def test_comparison_subtitles_reject_a_plan_without_voice(tmp_path, compare_plan):
    for beat in compare_plan["beats"]:
        beat.pop("lines")
    (tmp_path / "plan.json").write_text(json.dumps(compare_plan))
    (tmp_path / "script.en.txt").write_text("Water\nCrate\n")
    args = SimpleNamespace(short=str(tmp_path), take=None, lang="en", plan="plan.json")

    with pytest.raises(StudioError) as error:
        cli.cmd_srt(args)

    assert "words.ru.json" not in str(error.value)
    assert not (tmp_path / "subs.en.srt").exists()


def test_a_take_as_english_captions_on_the_cut(tmp_path):
    (tmp_path / "events.jsonl").write_text("\n".join(json.dumps(e) for e in EVENTS))
    shot = Shot(name="t", title="T", script="", hook="Я соврал")
    (tmp_path / "subs.en.json").write_text(json.dumps({
        "Я соврал": "I lied", "Ксардас": "Xardas", "Меня прислал Ли.": "Lee sent me.",
        "Ли? Не смеши меня.": "Lee? Don't make me laugh.", "Уходи.": "Go away.",
    }))  # fmt: skip
    text = cut.subtitles(shot, tmp_path, "en").read_text()
    cues = text.split("\n\n")
    assert cues[0] == "1\n00:00:00,000 --> 00:00:02,800\n[I lied]"
    assert "\nLee sent me." in cues[1]  # the hero's line has no name
    # the answer at 6.0 s of the recording is at 3.45 s of the cut (the wait was cut down)
    assert "00:00:03,450 -->" in text and "Xardas: Lee? Don't make me laugh." in text
    assert "[Xardas]" in text  # the caption, burnt in in Russian
    with pytest.raises(cut.CutError):
        (tmp_path / "subs.en.json").write_text(json.dumps({"Я соврал": "I lied"}))
        cut.subtitles(shot, tmp_path, "en")

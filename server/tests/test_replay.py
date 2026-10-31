"""Replay recorded dialogue and quick first words without a game or live model."""

import json
from copy import deepcopy
from importlib import import_module
from pathlib import Path

import pytest

from gothic_ai import cli
from gothic_ai.dialogue import QUICK_HINT, REACT_HINT, SOUND_SAID, DialogueManager, svm_hint
from gothic_ai.llm import FakeBackend, FakeStep, LLMError, Message, TextDelta, ToolCall, ToolResult
from gothic_ai.persona import PersonaProvider
from gothic_ai.replay import replay, replay_first_words, replay_svm_lines
from gothic_ai.trace import TraceWriter, find_turn, item_json
from tests.fake_game import FakeGame, connected
from tests.test_dialogue import XARDAS, _until

NPC = "BDT_1013_BANDIT_L"
PHRASE = "Твоё оружие меня не пугает."
SITUATION = "Ты один против двоих.\nСилы: он опасен."


def _turn(*, events=(), situation=SITUATION, npc=NPC, kind="player", llm=None, turn="one"):
    return {
        "type": "turn",
        "id": turn,
        "npc": npc,
        "instructions": "persona",
        "trigger": {"kind": kind, "text": PHRASE},
        "events": list(events),
        "steps": [
            {
                "llm": llm,
                "added": [
                    {"role": "developer", "content": "An earlier note from history."},
                    {"role": "user", "content": "Раньше ты был смелее."},
                    {"role": "assistant", "content": "Ничего ты не знаешь."},
                    {"role": "developer", "content": situation},
                    {"role": "user", "content": PHRASE},
                ],
            }
        ],
    }


@pytest.mark.parametrize("events", [(), ({"kind": "first_words", "text": "Хм..."},)])
async def test_old_trace_reconstructs_history_and_removes_spoken_and_fight_tails(events):
    quick = FakeBackend("[afraid] Хм, двести золотых? Больше мне сказать нечего.", delay=0)
    turn = _turn(events=events, situation=SITUATION + SOUND_SAID.format("Ого!") + "\n" + QUICK_HINT)
    original = deepcopy(turn)

    result = await replay_first_words(quick, turn, "Recorded persona.")

    assert turn == original
    assert result["situation"] == SITUATION and result["hint"] == REACT_HINT
    assert result["runs"][0]["words"] == "Хм, двести золотых?"
    assert result["runs"][0]["outcome"] == "said"
    assert not result["runs"][0]["pure_sound"]
    assert quick.calls[0] == (
        "Recorded persona.",
        [
            Message("developer", "An earlier note from history."),
            Message("user", "Раньше ты был смелее."),
            Message("assistant", "Ничего ты не знаешь."),
            Message("developer", SITUATION + "\n" + REACT_HINT),
            Message("user", PHRASE),
        ],
        [],
    )
    assert quick.temperatures == [0.6]


async def test_new_trace_uses_exact_quick_situation_and_replacement_hint_and_temperature():
    brief = "Твоё намерение: отношение к герою — испуганное.\nСилы: он опасен."
    turn = _turn(events=[{"kind": "first_words", "text": "—", "situation": brief, "hint": "Old hint."}])
    quick = FakeBackend("Ты знаешь слишком много!", delay=0)

    result = await replay_first_words(quick, turn, "Persona.", hint="Custom hint.", temperature=0.35)

    assert quick.calls[0][1][-2] == Message("developer", brief + "\nCustom hint.")
    assert quick.temperatures == [0.35]
    assert result["situation"] == brief
    assert result["hint"] == "Custom hint." and result["temperature"] == 0.35


@pytest.mark.parametrize(
    ("reply", "previous", "words", "outcome", "pure_sound"),
    [
        (
            "Раз два три четыре пять шесть семь восемь!", "",
            "Раз два три четыре пять шесть семь восемь!", "said", False,
        ),
        (
            "Раз два три четыре пять шесть семь восемь девять!", "",
            "Раз два три четыре пять шесть семь восемь девять!", "too_long", False,
        ),
        ("[neutral] —", "", "", "nothing", False),
        ("[angry] Убирайся!", "Убирайся.", "Убирайся!", "same", False),
        ("Хммм...", "Ага!", "Хммм...", "said", True),
    ],
)
async def test_replayed_outcomes_match_game_without_a_pure_sound_cooldown(
    reply, previous, words, outcome, pure_sound
):
    result = await replay_first_words(FakeBackend(reply, delay=0), _turn(), "Persona.", previous=[previous])

    run = result["runs"][0]
    assert (run["words"], run["outcome"], run["pure_sound"]) == (words, outcome, pure_sound)
    assert run["elapsed_ms"] >= 0


async def test_late_reaction_is_reported(monkeypatch):
    monkeypatch.setattr(import_module("gothic_ai.replay"), "REACTION_S", 0.001)
    quick = FakeBackend("Ты знаешь слишком много!", first=0.05, delay=0)

    result = await replay_first_words(quick, _turn(), "Persona.")

    assert result["runs"][0]["outcome"] == "late"
    assert result["runs"][0]["words"] == ""


async def test_model_error_is_reported_with_its_partial_text():
    class FailingBackend(FakeBackend):
        async def stream(self, **kwargs):
            yield TextDelta("Гм")
            raise LLMError("connection lost")

    result = await replay_first_words(FailingBackend(), _turn(), "Persona.")

    assert result["runs"][0]["outcome"] == "error"
    assert result["runs"][0]["text"] == "Гм"
    assert result["runs"][0]["error"] == "connection lost"


@pytest.mark.parametrize("history", [False, True])
@pytest.mark.parametrize("use_event", [False, True])
async def test_svm_replay_uses_the_recorded_situation_and_optional_history(history, use_event):
    turn = _turn(
        events=[{"kind": "first_words", "situation": SITUATION}] if use_event else [],
        situation="Ignored developer situation." if use_event else SITUATION + SOUND_SAID.format("Хм!")
        + "\n" + QUICK_HINT,
    )
    quick = FakeBackend("[angry] (кричит) Помогите, на меня напали! Дальше не произношу.", delay=0)
    native = {"svm": "$HELP", "text": "(в испуге) Помогите!", "target": "PC_HERO"}

    results = await replay_svm_lines(
        quick, turn, "Recorded persona.", [native], names={"PC_HERO": "герой"}, history=history,
    )

    instructions, request, tools = quick.calls[0]
    expected_history = [
        Message("developer", "An earlier note from history."),
        Message("user", "Раньше ты был смелее."),
        Message("assistant", "Ничего ты не знаешь."),
    ] if history else []
    assert request == [*expected_history, Message(
        "developer", SITUATION + "\n" + svm_hint("$HELP", "Помогите!", target_name="герой"),
    )]
    assert instructions == "Recorded persona." and tools == []
    assert quick.temperatures == [0.7]
    assert results[0]["runs"][0]["words"] == "Помогите, на меня напали!"
    assert results[0]["runs"][0]["outcome"] == "said"
    assert results[0]["runs"][0]["first_phrase_ms"] >= 0


@pytest.mark.parametrize("history", [False, True])
def test_svm_cli_uses_last_turn_and_target_names_and_writes_results(monkeypatch, tmp_path, capsys, history):
    trace = tmp_path / "traces.jsonl"
    svm_file = tmp_path / "svm.jsonl"
    hint = tmp_path / "hint.txt"
    out = tmp_path / "results.jsonl"
    hint.write_text("[{event}: «{text}»; {to}]", encoding="utf-8")
    turn = _turn(turn="last", situation="Last situation.")
    target = "BDT_1014_BANDIT_L"
    turn["snapshot"] = {"nearby_npcs": [{"id": target, "name": "Браго"}]}
    rows = [
        {"type": "instructions", "hash": "persona", "text": "Recorded persona."},
        _turn(turn="old"), _turn(npc="ANOTHER_NPC"), turn,
    ]
    trace.write_text("\n".join(json.dumps(r, ensure_ascii=False) for r in rows), encoding="utf-8")
    natives = [
        {"svm": "$ENEMYKILLED", "text": "Ты заслужил это, подонок!", "target": target},
        {"svm": "$HELP", "text": "Помогите!", "target": ""},
    ]
    svm_file.write_text("\n".join(json.dumps(r, ensure_ascii=False) for r in natives), encoding="utf-8")
    quick = FakeBackend(script=[
        FakeStep("(потягиваюсь) Вон отсюда!"), FakeStep("(дьявольский смех) Ну вот!"),
        FakeStep("(зевает)"), FakeStep("(зевает)"),
    ], delay=0)
    monkeypatch.setattr(cli, "make_quick", lambda url: quick)
    monkeypatch.setattr("sys.argv", [
        "gothic-ai", "replay-svm-lines", str(trace), "--svm-file", str(svm_file),
        "--quick-url", "http://quick:9000/v1", "--npc", NPC.lower(), "--runs", "2",
        *(["--history"] if history else []), "--hint-file", str(hint), "--out", str(out),
    ])

    with pytest.raises(SystemExit) as exit:
        cli.main()

    assert exit.value.code == 0 and len(quick.calls) == 4
    assert all(c[0] == "Recorded persona." and len(c[1]) == (4 if history else 1) for c in quick.calls)
    assert all(c[1][-1].content.startswith("Last situation.\n") for c in quick.calls)
    assert " — Браго" in quick.calls[0][1][-1].content
    assert "обращаясь к Браго" in quick.calls[0][1][-1].content
    assert "вслух" in quick.calls[2][1][-1].content
    printed = capsys.readouterr().out.splitlines()
    assert len(printed) == 2 and printed[0].startswith(f"{NPC} | $ENEMYKILLED | ")
    assert "[said," in printed[0]
    results = [json.loads(row) for row in out.read_text(encoding="utf-8").splitlines()]
    assert [r["turn"] for r in results] == ["last", "last"]
    assert [r["svm"] for r in results] == ["$ENEMYKILLED", "$HELP"]
    assert all(r["history"] is history and len(r["runs"]) == 2 for r in results)
    assert [r["words"] for r in results[0]["runs"]] == ["Вон отсюда!", "Ну вот!"]
    assert all(r["words"] == "" and r["outcome"] == "nothing" for r in results[1]["runs"])


@pytest.mark.parametrize(
    ("flags", "quick_env", "expected_url"),
    [
        ([], "http://quick:9000/v1", "http://quick:9000/v1"),
        ([], None, "http://gpu:47834/v1"),
        (["--quick-url", "http://override:9000/v1"], "http://quick:9000/v1", "http://override:9000/v1"),
    ],
)
def test_cli_filters_turns_runs_independently_and_prints_and_writes_results(
    monkeypatch, tmp_path, capsys, flags, quick_env, expected_url
):
    trace = tmp_path / "traces.jsonl"
    out = tmp_path / "results.jsonl"
    hint = tmp_path / "hint.txt"
    hint.write_text("Alternative hint.\n", encoding="utf-8")
    rows = [
        {"type": "instructions", "hash": "persona", "text": "Recorded persona."},
        _turn(turn="one"),
        _turn(turn="two"),
        _turn(turn="three"),
        _turn(llm="quick", turn="fight"),
        _turn(kind="incident", turn="incident"),
        _turn(npc="ANOTHER_NPC", turn="another"),
        {"type": "turn", "id": "empty", "trigger": {"kind": "player"}, "steps": []},
    ]
    rows[1]["trigger"]["text"] = "А" * 100
    trace.write_text("\n".join(json.dumps(r, ensure_ascii=False) for r in rows), encoding="utf-8")
    quick = FakeBackend(
        script=[
            FakeStep("Хм..."), FakeStep("Ничего себе!"),
            FakeStep("Ха!"), FakeStep("Ничего себе!"),
            FakeStep("Ха!"), FakeStep("Ты шутишь!"),
        ],
        delay=0,
    )
    urls = []

    def make_quick(url):
        urls.append(url)
        return quick

    monkeypatch.setattr(cli, "make_quick", make_quick)
    monkeypatch.setattr(Path, "home", lambda: tmp_path)
    settings = tmp_path / ".config/gothic-ai/settings.toml"
    settings.parent.mkdir(parents=True)
    settings.write_text("[llm]\nquick_url = 'http://settings:9000/v1'\n", encoding="utf-8")
    monkeypatch.setenv("GOTHIC_DECIDER_URL", "ws://gpu:47832")
    if quick_env:
        monkeypatch.setenv("GOTHIC_QUICK_LLM_URL", quick_env)
    else:
        monkeypatch.delenv("GOTHIC_QUICK_LLM_URL", raising=False)
    monkeypatch.setattr("sys.argv", [
        "gothic-ai", "replay-first-words", str(trace), "--hint-file", str(hint), "--runs", "2",
        "--temperature", "0.7", "--npc", NPC.lower(), "--out", str(out), *flags,
    ])

    with pytest.raises(SystemExit) as exit:
        cli.main()

    assert exit.value.code == 0
    assert urls == [expected_url]
    assert len(quick.calls) == 6 and quick.temperatures == [0.7] * 6
    assert all(c[0] == "Recorded persona." for c in quick.calls)
    assert all(c[1][-2].content.endswith("\nAlternative hint.") for c in quick.calls)
    lines = capsys.readouterr().out.splitlines()
    assert len(lines) == 3
    assert lines[0].split(" | ")[1] == "А" * 80
    assert "pure_sound" in lines[0] and "same" in lines[1]
    results = [json.loads(line) for line in out.read_text(encoding="utf-8").splitlines()]
    assert [r["turn"] for r in results] == ["one", "two", "three"]
    assert [r["outcome"] for r in results[1]["runs"]] == ["said", "same"]
    assert [r["outcome"] for r in results[2]["runs"]] == ["same", "said"]


def test_cli_resets_every_npcs_repetitions_when_the_save_branch_changes(monkeypatch, tmp_path, capsys):
    trace = tmp_path / "traces.jsonl"
    out = tmp_path / "results.jsonl"
    other = "NONE_100_XARDAS"
    turns = [
        {**_turn(npc=npc), "branch": branch}
        for branch, npc in [
            ("first", other), ("first", NPC), ("first", other),
            ("second", NPC), ("second", other), ("first", other),
        ]
    ]
    rows = [{"type": "instructions", "hash": "persona", "text": "Recorded persona."}, *turns]
    trace.write_text("\n".join(json.dumps(r, ensure_ascii=False) for r in rows), encoding="utf-8")
    quick = FakeBackend("Ты серьёзно?", delay=0)
    monkeypatch.setattr(cli, "make_quick", lambda url: quick)
    monkeypatch.setattr("sys.argv", [
        "gothic-ai", "replay-first-words", str(trace), "--quick-url", "http://quick:9000/v1",
        "--runs", "2", "--out", str(out),
    ])

    with pytest.raises(SystemExit) as exit:
        cli.main()

    assert exit.value.code == 0
    results = [json.loads(line) for line in out.read_text(encoding="utf-8").splitlines()]
    assert [[run["outcome"] for run in result["runs"]] for result in results] == [
        ["said", "said"], ["said", "said"], ["same", "same"],
        ["said", "said"], ["said", "said"], ["said", "said"],
    ]
    assert len(capsys.readouterr().out.splitlines()) == 6


@pytest.mark.parametrize(
    ("setting", "expected_url"),
    [
        ("quick_url = 'http://configured:9000/v1'", "http://configured:9000/v1"),
        ("decider_url = 'ws://configured:47832'", "http://configured:47834/v1"),
    ],
)
def test_cli_reads_quick_address_from_launcher_settings(monkeypatch, tmp_path, setting, expected_url):
    monkeypatch.delenv("GOTHIC_QUICK_LLM_URL", raising=False)
    monkeypatch.delenv("GOTHIC_DECIDER_URL", raising=False)
    monkeypatch.setattr(Path, "home", lambda: tmp_path)
    settings = tmp_path / ".config/gothic-ai/settings.toml"
    settings.parent.mkdir(parents=True)
    settings.write_text("[llm]\n" + setting + "\n", encoding="utf-8")
    trace = tmp_path / "traces.jsonl"
    trace.write_text(
        json.dumps({"type": "instructions", "hash": "persona", "text": "Persona."}) + "\n"
        + json.dumps(_turn()),
        encoding="utf-8",
    )
    quick = FakeBackend("Ты шутишь!", delay=0)
    urls = []

    def make_quick(url):
        urls.append(url)
        return quick

    monkeypatch.setattr(cli, "make_quick", make_quick)
    monkeypatch.setattr("sys.argv", ["gothic-ai", "replay-first-words", str(trace)])
    with pytest.raises(SystemExit) as exit:
        cli.main()
    assert exit.value.code == 0
    assert urls == [expected_url] and len(quick.calls) == 1


@pytest.mark.parametrize("flags", [[], ["--runs", "0"]])
def test_cli_reports_missing_quick_address_or_invalid_run_count(monkeypatch, tmp_path, capsys, flags):
    monkeypatch.delenv("GOTHIC_QUICK_LLM_URL", raising=False)
    monkeypatch.delenv("GOTHIC_DECIDER_URL", raising=False)
    monkeypatch.setattr(Path, "home", lambda: tmp_path)
    monkeypatch.setattr("sys.argv", ["gothic-ai", "replay-first-words", "missing.jsonl", *flags])
    with pytest.raises(SystemExit) as exit:
        cli.main()
    assert exit.value.code == 2
    assert ("--runs" if flags else "--quick-url") in capsys.readouterr().err


CALL = ToolCall("take_item", {"item": "ITPL_HEALTH_HERB_01"}, "recorded-call")
REJECTION = "Отклонено: на земле нет ITPL_HEALTH_HERB_01; ты видишь там: ничего."


def _record(text="[angry] Полегче, приятель.", *, call=CALL, output=REJECTION, status="rejected"):
    return {
        "id": "recorded-turn",
        "npc": "BDT_1013_BANDIT_L",
        "trigger": {"kind": "player", "text": "Привет, чмо!"},
        "tools": [call.name],
        "steps": [
            {
                "added": [item_json(Message("user", "Привет, чмо!"))],
                "text": text,
                "calls": [
                    {
                        "name": call.name,
                        "arguments": call.arguments,
                        "call_id": call.call_id,
                        "result": output,
                        "outcome": status,
                    }
                ],
            },
            {
                "added": [
                    item_json(Message("assistant", text)),
                    item_json(call),
                    item_json(ToolResult(call.call_id, output)),
                ],
                "text": "",
            },
        ],
    }


@pytest.mark.parametrize("text", ["[angry] Полегче, приятель.", ""])
async def test_replay_adds_spoken_words_to_a_recorded_rejection(text):
    llm = FakeBackend(script=[FakeStep(text, [CALL]), FakeStep("")], delay=0)

    result = await replay(llm, _record(text), "NPC instructions")

    assert len(llm.calls) == 2
    (output,) = [i.output for i in llm.calls[1][1] if isinstance(i, ToolResult)]
    expected = REJECTION
    if text:
        expected += (
            " Ты уже сказал вслух: «Полегче, приятель.». Если отказ этого не меняет — ничего не говори. "
            "Если меняет — поправься одной короткой фразой, не повторяя сказанного."
        )
    assert output == expected
    assert result["replay"][0]["calls"][0]["output"] == expected


@pytest.mark.parametrize("after_step", [0, 1])
async def test_first_words_do_not_finish_a_wordless_accepted_action(after_step):
    call = ToolCall("give_item", {"item": "ITSC_FIREBALL", "count": 1}, "give-call")
    record = _record("", call=call, output="Принято: передашь свиток.", status="accepted")
    record["events"] = [{"kind": "first_words", "text": "Хм..."}]
    script = [FakeStep("", [call]), FakeStep("Держи, это поможет.")]
    llm = FakeBackend(script=script[after_step:], delay=0)

    result = await replay(llm, record, "NPC instructions", after_step=after_step)

    assert len(llm.calls) == 2 - after_step
    assert result["replay"][-1]["text"] == "Держи, это поможет."
    assert result["said_before"] == "Хм..."


@pytest.mark.parametrize("after_step", [0, 1])
@pytest.mark.parametrize(
    "events",
    [
        [{"kind": "first_words", "text": "Ишь ты!"}],
        [
            *[
                {"kind": "first_words", "outcome": outcome, "text": f"Unsaid {outcome}."}
                for outcome in ("too_long", "same", "sound", "nothing", "late", "error")
            ],
            {
                "kind": "first_words",
                "outcome": "said",
                "text": "[angry] Ишь ты! И это всё?",
                "words": "Ишь ты!",
            },
        ],
    ],
)
async def test_replay_quotes_only_first_words_that_were_said_once(events, after_step):
    record = _record("Ишь ты! Держи свиток.")
    record["events"] = events
    script = [FakeStep("Ишь ты! Держи свиток.", [CALL]), FakeStep("")]
    llm = FakeBackend(script=script[after_step:], delay=0)

    result = await replay(llm, record, "NPC instructions", after_step=after_step)

    (output,) = [i.output for i in llm.calls[-1][1] if isinstance(i, ToolResult)]
    assert output == (
        REJECTION + " Ты уже сказал вслух: «Ишь ты! Держи свиток.». "
        "Если отказ этого не меняет — ничего не говори. "
        "Если меняет — поправься одной короткой фразой, не повторяя сказанного."
    )
    assert result["said_before"] == ("Ишь ты! Держи свиток." if after_step else "Ишь ты!")


async def test_after_step_does_not_quote_any_discarded_first_words():
    record = _record("")
    record["events"] = [
        {
            "kind": "first_words",
            "outcome": outcome,
            "text": f"[angry] Raw {outcome}. Another sentence.",
            "words": f"Parsed {outcome}.",
        }
        for outcome in ("too_long", "sound", "same", "late", "nothing", "error")
    ]
    llm = FakeBackend("", delay=0)

    result = await replay(llm, record, "NPC instructions", after_step=1)

    assert result["said_before"] == ""
    assert result["initial_tool_results"] == [REJECTION]
    (output,) = [i.output for i in llm.calls[0][1] if isinstance(i, ToolResult)]
    assert output == REJECTION


@pytest.mark.parametrize("after_step", [0, 2])
@pytest.mark.parametrize(
    "first_reply",
    ["Ишь ты!", "functions.give_quest({...})"],
    ids=["repeated-first-words", "discarded-tool-text"],
)
async def test_replay_quotes_the_dialogue_lines_instead_of_raw_step_text(
    tmp_path, first_reply, after_step
):
    recall = ToolCall("recall", {"query": "scroll"}, "recall-call")
    give = ToolCall("give_item", {"item": "ITSC_FIREBALL", "count": 1}, "give-call")
    script = [
        FakeStep(first_reply, [recall]),
        FakeStep("Ишь ты! Держи свиток.", [give]),
        FakeStep(""),
    ]
    fake = FakeGame()
    llm = FakeBackend(script=script, chunk=3, delay=0)
    writer = TraceWriter(tmp_path)
    async with connected(fake) as link:
        dialogue = DialogueManager(
            link,
            llm,
            PersonaProvider(None),
            quick=FakeBackend("Ишь ты!", delay=0),
            trace=writer,
        )
        await link.attach(XARDAS)
        dialogue.open(XARDAS, "Ксардас")
        await dialogue.say_as_player(XARDAS, "Дай мне свиток.")
        await _until(lambda: fake.statuses[-1:] == ["idle"])

    assert fake.said == ["Ишь ты!", "Держи свиток."]
    assert len(llm.calls) == 3
    original = next(
        i for i in llm.calls[2][1] if isinstance(i, ToolResult) and i.call_id == give.call_id
    )
    assert original.output.startswith("Отклонено: У тебя нет предмета ITSC_FIREBALL")
    assert original.output.endswith(
        " Ты уже сказал вслух: «Ишь ты! Держи свиток.». "
        "Если отказ этого не меняет — ничего не говори. "
        "Если меняет — поправься одной короткой фразой, не повторяя сказанного."
    )
    assert original.output.count("Ишь ты!") == 1
    record, instructions = find_turn(writer.path(), None)
    assert instructions
    assert record["steps"][0]["text"] == first_reply
    assert record["steps"][1]["text"] == "Ишь ты! Держи свиток."
    assert record["steps"][1]["calls"][0]["outcome"] == "rejected"
    other = FakeBackend(script=script[after_step:], chunk=3, delay=0)

    result = await replay(other, record, instructions, after_step=after_step)

    assert len(other.calls) == 3 - after_step
    replayed = next(
        i for i in other.calls[-1][1] if isinstance(i, ToolResult) and i.call_id == give.call_id
    )
    assert replayed.output == original.output
    assert result["said_before"] == (" ".join(fake.said) if after_step else "Ишь ты!")
    assert not result["diverged"]


async def test_replay_can_continue_the_recorded_rejection_without_saying_the_first_step_again():
    record = _record()
    # New traces already contain the suffix in added; the call's result remains the raw rejection.
    record["steps"][1]["added"][-1]["output"] += " old suffix must be replaced"
    llm = FakeBackend(script=[FakeStep("")], delay=0)

    result = await replay(llm, record, "NPC instructions", after_step=1)

    assert len(llm.calls) == 1
    assert result["after_step"] == 1
    assert result["said_before"] == "Полегче, приятель."
    (output,) = [i.output for i in llm.calls[0][1] if isinstance(i, ToolResult)]
    assert output.startswith(REJECTION + " Ты уже сказал вслух: «Полегче, приятель.».")
    assert output.count("Ты уже сказал вслух") == 1
    assert "old suffix" not in output
    assert result["initial_tool_results"] == [output]


async def test_continuing_a_recorded_step_keeps_the_turn_step_cap():
    llm = FakeBackend(script=[FakeStep("", [CALL]), FakeStep("", [CALL])], delay=0)

    result = await replay(llm, _record(), "NPC instructions", after_step=1)

    assert len(result["replay"]) == 2
    assert llm.tool_choices == [None, "none"]
    assert result["replay"][-1]["calls"] == []


async def test_replay_offers_check_claim_after_the_recorded_quest_rejection():
    accept = ToolCall("accept_quest", {"quest": "MIS_ADDON_CAVALORN_KILLBRAGO"}, "accept-call")
    check = ToolCall("check_claim", accept.arguments, "check-call")
    record = _record(
        "[neutral] Получай обещанную награду.",
        call=accept,
        output="Отклонено: он уже лгал тебе. Можешь сходить и посмотреть сам (check_claim).",
    )
    record["tools"].append(check.name)
    record["steps"][1]["calls"] = [
        {
            "name": check.name,
            "arguments": check.arguments,
            "call_id": check.call_id,
            "result": "Принято: после реплики пойдёшь смотреть сам (Браго).",
            "outcome": "accepted",
        }
    ]
    llm = FakeBackend(
        script=[FakeStep("Только сперва вернусь в пещеру и проверю.", [check])], delay=0
    )

    result = await replay(llm, record, "NPC instructions", after_step=1)

    assert len(llm.calls) == 1
    assert {t.name for t in llm.calls[0][2]} == {accept.name, check.name}
    assert result["replay"][0]["calls"][0]["outcome"] == "accepted"
    assert not result["diverged"]


async def test_replay_does_not_add_the_rejection_rule_to_recall():
    call = ToolCall("recall", {"query": "bandits"}, "recall-call")
    llm = FakeBackend(script=[FakeStep("Погоди.", [call]), FakeStep("Вспомнил.")], delay=0)

    result = await replay(
        llm, _record(call=call, output="Ты помнишь бандитов.", status="read"), "NPC instructions"
    )

    assert len(llm.calls) == 2
    assert result["replay"][0]["calls"][0]["output"] == "Ты помнишь бандитов."


async def test_continuing_a_silent_step_keeps_gemmas_words_and_does_not_offer_a_correction():
    record = _record("These words were not said.")
    record["steps"][0]["silent"] = True
    record["steps"][1]["silent"] = True
    record["steps"].insert(
        0,
        {
            "added": [item_json(Message("developer", "Quick situation"))],
            "text": "[angry] Ни шагу!",
            "llm": "gemma",
        },
    )
    record["events"] = [{"kind": "first_words", "text": "Эй!"}]
    llm = FakeBackend(script=[FakeStep("These words stay silent too.")], delay=0)

    result = await replay(llm, record, "NPC instructions", after_step=2)

    assert result["said_before"] == "Эй! Ни шагу!"
    assert result["initial_tool_results"] == [REJECTION]
    assert result["replay"][0]["silent"]
    assert not any(getattr(i, "content", None) == "Quick situation" for i in llm.calls[0][1])
    assert llm.tool_choices == [None]


@pytest.mark.parametrize("after_step", [-1, 2])
async def test_replay_rejects_a_missing_continuation_step(after_step):
    llm = FakeBackend(delay=0)

    with pytest.raises(ValueError, match="after_step must be between 0 and 1"):
        await replay(llm, _record(), "NPC instructions", after_step=after_step)

    assert not llm.calls

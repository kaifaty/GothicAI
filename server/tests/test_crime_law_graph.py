import copy
import json
import os
import shutil
import subprocess
from collections import Counter
from functools import reduce
from pathlib import Path

import pytest

from gothic_ai.crime_law_graph import (
    AMBIENT,
    LAWFUL,
    NEWS_CRIME,
    absolution_program,
    ambient_ops,
    ambient_program,
    city_entry_nodes,
    city_entry_program,
    counter_values,
    gate_nodes,
    init_ops,
    law_var,
    project_program,
    projection_values,
    validate_ops,
)
from gothic_ai.gamedata.storygraph import Node, StoryGraph, load, walk
from gothic_ai.validator import Patches, Rules, Story, WorldState, names_in, validate

ANDRE = "MIL_311_ANDRE"
PETZMASTER = "DIA_ANDRE_PETZMASTER"
GREATEST = ["fn", "B_GETGREATESTPETZCRIME", ["inst", "SELF"]]


@pytest.fixture
def graph():
    nodes = {
        PETZMASTER: Node(
            PETZMASTER,
            "dialog",
            ANDRE,
            "",
            [{"do": "line", "id": "DIA_Andre_Petzmaster_05_01"}],
            set(),
            condition=[">", GREATEST, ["var", "ANDRE_LASTPETZCRIME"]],
            permanent=True,
            important=True,
        ),
        AMBIENT: Node(
            AMBIENT,
            "dialog",
            "$INSTANCE_HELP",
            "",
            [],
            set(),
            condition=["!=", ["fn", "B_GETPLAYERCRIME", ["inst", "SELF"]], ["int", 0]],
        ),
    }
    return StoryGraph(nodes=nodes, functions={}, instances={}, vars={}, quests={})


def test_additive_lock_keeps_native_condition_and_effects(graph):
    before = copy.deepcopy(graph)
    ops = init_ops(graph)
    assert graph == before
    assert ops[0]["vars"][NEWS_CRIME] == 0
    assert ops[1:] == [{"op": "condition", "node": PETZMASTER, "expr": ["var", law_var("city")]}]
    patched = Story(graph, WorldState(), Patches.of(ops))
    assert patched.graph.nodes[PETZMASTER].condition == before.nodes[PETZMASTER].condition
    assert patched.effects_of(PETZMASTER) == before.nodes[PETZMASTER].effects
    assert not patched.can_offer(PETZMASTER)
    assert validate_ops(ops, Story(graph, WorldState())) == []


def test_counters_count_deeds_not_stacks_and_project_zeros_for_other_kinds():
    crimes = [
        {"community": "city", "kind": "theft", "count": 23},
        {"community": "city", "kind": "murder"},
        {"community": "farm", "kind": "attack"},
    ]
    assert counter_values("city", crimes) == {
        "PETZCOUNTER_CITY_SHEEPKILLER": 0,
        "PETZCOUNTER_CITY_ATTACK": 0,
        "PETZCOUNTER_CITY_THEFT": 1,
        "PETZCOUNTER_CITY_MURDER": 1,
    }
    assert projection_values("city", crimes)["DIR_CRIME_PETZ_CITY_THEFT"] == 1
    assert counter_values("bl", crimes) == {}
    assert counter_values(None, crimes) == {}


def test_every_native_condition_and_information_runs_after_counter_projection():
    own = [
        {"do": "if", "cond": GREATEST, "then": [{"do": "return", "value": ["int", 1]}]},
        {"do": "return", "value": ["int", 0]},
    ]
    original = copy.deepcopy(own)
    projected = project_program("city", own)
    assert projected[1:] == own == original
    assert projected[0]["cond"] == ["var", law_var("city")]
    assert projected[0]["then"] == [
        {
            "do": "set",
            "target": ["var", f"PETZCOUNTER_CITY_{suffix}"],
            "value": ["var", f"DIR_CRIME_PETZ_CITY_{suffix}"],
        }
        for suffix in ("SHEEPKILLER", "ATTACK", "THEFT", "MURDER")
    ]
    projected[1]["then"].clear()
    assert own == original


@pytest.mark.parametrize("location, communities", [(2, ("city",)), (6, LAWFUL)])
def test_native_absolution_closes_projections_at_call_before_return(location, communities):
    native_call = {"do": "call", "call": ["fn", "B_GRANTABSOLUTION", ["int", location]]}
    own = [{"do": "if", "cond": ["int", 1], "then": [native_call, {"do": "return"}]}]
    original = copy.deepcopy(own)
    made = absolution_program(own)
    steps = made[0]["then"]
    assert steps[0] == native_call
    assert steps[-1] == {"do": "return"}
    written = {step["target"][1]: step["value"] for step in steps[1:-1]}
    assert written == {
        name: ["int", 0]
        for community in communities
        for name in [law_var(community), *projection_values(community, [])]
    }
    assert own == original


def test_absolution_rejects_a_program_without_the_native_call():
    with pytest.raises(ValueError, match="does not call"):
        absolution_program([{"do": "return"}])


def test_news_uses_actual_speaker_known_crime_and_keeps_native_voice(graph):
    player_crime = ["fn", "B_GETPLAYERCRIME", ["inst", "SELF"]]
    native_voice = {
        "do": "call",
        "call": ["fn", "B_SAY", ["inst", "SELF"], ["inst", "OTHER"], ["str", "$THEFT_CRIME"]],
    }
    own = [
        {"do": "if", "cond": ["==", player_crime, ["int", 3]], "then": [native_voice]},
        {"do": "return"},
    ]
    levels = {"VLK_400_MATTEO": 3, ANDRE: 2}
    original = copy.deepcopy(own)
    made = ambient_program(own, levels)
    body = made[1 + len(levels) :]
    assert body[0]["cond"] == ["==", ["var", NEWS_CRIME], ["int", 3]]
    assert body[0]["then"] == own[0]["then"] == [native_voice]
    assert body[1] == own[1]
    assert own == original
    assert player_crime in list(walk(own[0]["cond"]))
    assert made[0] == {"do": "set", "target": ["var", NEWS_CRIME], "value": ["int", 0]}
    assert made[1]["cond"] == ["==", ["call", "HLP_GETINSTANCEID", ["inst", "SELF"]], ["sym", ANDRE]]
    assert ambient_program(own, {}) == [made[0], *body]
    ops = ambient_ops(graph, own, levels)
    assert {op["part"] for op in ops} == {"condition", "information"}
    assert all(op["node"] == AMBIENT for op in ops)

    # Execute the emitted ordinary steps across speakers using the same scratch state. In
    # particular, a stranger immediately after Matteo must neither know nor utter his theft.
    variables = {NEWS_CRIME: 99}
    symbols = {ANDRE: 311, "VLK_400_MATTEO": 400}
    spoken = []

    def value(expr, speaker):
        match expr:
            case ["int", integer]:
                return integer
            case ["var", name]:
                return variables[name]
            case ["sym", name]:
                return symbols[name]
            case ["call", "HLP_GETINSTANCEID", ["inst", "SELF"]]:
                return speaker
            case ["==", left, right]:
                return int(value(left, speaker) == value(right, speaker))
            case ["!=", left, right]:
                return int(value(left, speaker) != value(right, speaker))
            case _:
                raise AssertionError(f"unsupported program expression: {expr}")

    def execute(steps, speaker):
        for step in steps:
            match step["do"]:
                case "set":
                    variables[step["target"][1]] = value(step["value"], speaker)
                case "if":
                    if value(step["cond"], speaker):
                        execute(step["then"], speaker)
                case "call":
                    assert step == native_voice
                    spoken.append(speaker)
                case "return":
                    return value(step["value"], speaker) if "value" in step else None
                case _:
                    raise AssertionError(f"unsupported program step: {step}")

    condition = next(op["steps"] for op in ops if op["part"] == "condition")
    for speaker, expected in ((400, 3), (999, 0), (311, 2), (999, 0), (400, 3)):
        assert execute(condition, speaker) == int(bool(expected))
        assert variables[NEWS_CRIME] == expected
        execute(made, speaker)
        assert variables[NEWS_CRIME] == expected
    assert spoken == [400, 400]


def test_scoped_validator_does_not_authorize_other_plot_dialogs(graph):
    assert validate_ops(
        [{"op": "program", "node": "DIA_HAGEN_HELLO", "steps": []}], Story(graph, WorldState())
    ) == ["unexpected community law program: DIA_HAGEN_HELLO"]
    assert validate_ops(
        [{"op": "condition", "node": PETZMASTER, "expr": ["int", 1]}], Story(graph, WorldState())
    ) == [f"unexpected community law condition: {PETZMASTER}"]


DATA_ROOT = Path(os.environ.get("GOTHIC_AI_DATA", Path.home() / ".local/share/gothic-ai"))
REAL_GRAPH = Path(os.environ.get("GOTHIC_STORY_GRAPH", DATA_ROOT / "story.json"))


@pytest.fixture(scope="module")
def real_graph():
    if not REAL_GRAPH.exists():
        pytest.skip("needs actual storygraph export (GOTHIC_STORY_GRAPH)")
    return load(REAL_GRAPH)


def test_all_24_real_graph_locks_preserve_native_conditions_and_pass_validator(real_graph):
    native = {node.id: copy.deepcopy(node.condition) for node in real_graph.nodes.values()}
    nodes = gate_nodes(real_graph)
    assert len(nodes) == 24
    assert Counter(nodes.values()) == {"city": 17, "farm": 2, "monastery": 3, "oldcamp": 2}
    assert nodes["DIA_PARLAN_DONTDISTURB"] == "monastery"
    ops = init_ops(real_graph)
    story = Story(real_graph, WorldState())
    assert validate_ops(ops, story) == []
    patched = story.then(ops)
    assert all(real_graph.nodes[node].condition == native[node] for node in nodes)
    assert all(
        patched.patches.conditions.get(node, []) == (
            [] if node in city_entry_nodes(real_graph) else [["var", law_var(group)]]
        )
        for node, group in nodes.items()
    )
    # Only the trusted crime service receives the law-dialog exception for Hagen, Lee and Garond.
    assert validate(ops, story, Rules.of(real_graph))


def test_real_native_fine_and_forgiveness_paths_are_retained(real_graph):
    for keeper in ("ANDRE", "HAGEN", "LEE", "PARLAN", "GAROND"):
        parent = real_graph.nodes[f"DIA_{keeper}_PETZMASTER"]
        debts = [e for e in parent.effects if e.get("do") == "set" and e.get("var") == f"{keeper}_SCHULDEN"]
        # One theft is B_GetTotalPetzCounter(self)*50; murder adds 500, sheep alone is flat 100.
        fifty = ["*", ["fn", "B_GETTOTALPETZCOUNTER", ["inst", "SELF"]], ["int", 50]]
        values = [e["value"] for e in debts]
        assert fifty in values
        assert ["+", fifty, ["int", 500]] in values
        assert ["int", 100] in values
        assert ["int", 1000] in values
        pay = real_graph.nodes[f"DIA_{keeper}_PETZMASTER_PAYNOW"]
        assert any(e.get("fn") == "B_GRANTABSOLUTION" for e in pay.effects)
    welcome = real_graph.nodes["DIA_PARLAN_WELCOME"]
    assert any(
        e.get("fn") == "B_GRANTABSOLUTION" and e.get("args") == [["int", 6, "LOC_ALL"]]
        for e in welcome.effects
    )
    thorben = [
        node.id
        for node in real_graph.nodes.values()
        if any(e.get("fn") == "B_THORBEN_DELETEPETZCRIMEGRITTA" for e in node.effects)
    ]
    assert len(thorben) == 3
    assert any(
        e.get("fn") == "B_MEMORIZEPLAYERCRIME" for e in real_graph.nodes["DIA_HILDA_PFANNETOOLATE"].effects
    )


class GateVM:
    """Execute emitted steps and real lifted gate conditions with deterministic engine answers."""

    def __init__(self, npc="MIL_310_STADTWACHE"):
        self.npc = npc
        self.number = npc.split("_", 2)[1]
        self.variables = {
            law_var("city"): 0, **projection_values("city", []), "MIS_ADDON_LESTER_PICKFORCONSTANTINO": 1
        }
        self.fields = {(npc, "WP"): "post", ("PC_HERO", "GUILD"): 0}
        self.items = {"ITMI_GOLD": 100, "ITWR_PASSIERSCHEIN": 1, "ITPL_MANA_HERB_01": 10}
        self.armor = "ITAR_BAU_L"
        self.lines = []

    def value(self, expr):
        tag, *args = expr
        if tag in {"int", "str", "sym"}:
            return args[0]
        if tag == "inst":
            return self.npc if args[0] == "SELF" else "PC_HERO" if args[0] in {"OTHER", "HERO"} else args[0]
        if tag == "var":
            return self.variables.get(args[0], 0)
        if tag == "field":
            return self.fields.get((self.value(args[0]), args[1], *args[2:3]), 0)
        if tag == "cases":
            pairs = zip(args[::2], args[1::2], strict=True)
            return next((self.value(v) for c, v in pairs if self.value(c)), 0)
        if tag == "!":
            return int(not self.value(args[0]))
        if tag in {"&&", "||"}:
            return int((all if tag == "&&" else any)(self.value(a) for a in args))
        operators = {
            "==": lambda a, b: int(a == b), "!=": lambda a, b: int(a != b),
            ">": lambda a, b: int(a > b), "<": lambda a, b: int(a < b),
            ">=": lambda a, b: int(a >= b), "<=": lambda a, b: int(a <= b),
            "+": lambda a, b: a + b, "-": lambda a, b: a - b,
        }
        if tag in operators:
            return reduce(operators[tag], (self.value(a) for a in args))
        assert tag in {"call", "fn"}, expr
        name, *params = args
        if name == "B_GETGREATESTPETZCRIME":
            levels = {"SHEEPKILLER": 1, "ATTACK": 2, "THEFT": 3, "MURDER": 4}
            return max((level for kind, level in levels.items()
                        if self.variables.get(f"PETZCOUNTER_CITY_{kind}", 0)), default=0)
        if name == "B_GETCURRENTABSOLUTIONLEVEL":
            return self.variables.get("ABSOLUTIONLEVEL_CITY", 0)
        if name == "NPC_GETDISTTOWP":
            return 1200
        if name == "NPC_GETNEARESTWP":
            return "post"
        if name == "NPC_GETTRUEGUILD":
            return self.fields.get((self.value(params[0]), "GUILD"), 0)
        if name == "HLP_STRCMP" or name == "HLP_ISITEM":
            return int(self.value(params[0]) == self.value(params[1]))
        if name == "NPC_REFUSETALK":
            return 0
        if name == "NPC_ISINSTATE" or name == "NPC_KNOWSINFO":
            return 1
        if name == "NPC_HASITEMS":
            return self.items.get(self.value(params[1]), 0)
        if name == "NPC_GETEQUIPPEDARMOR":
            return self.armor
        if name == "NPC_HASEQUIPPEDARMOR":
            return int(bool(self.armor))
        if name == "B_GIVEINVITEMS":
            return int(self.items.get(self.value(params[2]), 0) >= self.value(params[3]))
        raise AssertionError(f"unsupported gate expression: {expr}")

    def execute(self, steps):
        class Returned(Exception):
            pass

        result = None

        def run(body):
            nonlocal result
            for step in body:
                match step["do"]:
                    case "set":
                        target = step["target"]
                        if target[0] == "var":
                            self.variables[target[1]] = self.value(step["value"])
                        else:
                            assert target[0] == "field"
                            key = (self.value(target[1]), target[2], *target[3:4])
                            self.fields[key] = self.value(step["value"])
                    case "if":
                        run(step.get("then" if self.value(step["cond"]) else "else", []))
                    case "call":
                        if step["call"][1] == "AI_OUTPUT":
                            self.lines.append(self.value(step["call"][-1]))
                    case "return":
                        result = self.value(step["value"]) if "value" in step else None
                        raise Returned
                    case _:
                        raise AssertionError(step)

        try:
            run(steps)
        except Returned:
            pass
        return result


def lifted_gate_steps(node, part):
    """Run the exported native effects, preserving their guards and native line identifiers."""
    steps = []
    for effect in node.condition_effects if part == "condition" else node.effects:
        match effect["do"]:
            case "set_field":
                step = {
                    "do": "set", "target": ["field", effect["of"], effect["field"], effect["index"][1]],
                    "value": effect["value"],
                }
            case "set":
                step = {"do": "set", "target": ["var", effect["var"]], "value": effect["value"]}
            case "line":
                step = {"do": "call", "call": ["call", "AI_OUTPUT", ["str", effect["id"]]]}
            case _:
                continue  # journal/XP/animation do not affect whether passage is granted
        steps.append({"do": "if", "cond": effect["when"], "then": [step]} if "when" in effect else step)
    if part == "condition":
        steps.append({"do": "return", "value": node.condition})
    return steps


@pytest.mark.parametrize("npc", ["MIL_310_STADTWACHE", "MIL_333_STADTWACHE"])
def test_real_gate_warns_known_thief_and_allows_previous_visitor_after_payment(real_graph, npc):
    vm = GateVM(npc)
    first = real_graph.nodes[f"DIA_{npc}_FIRSTWARN"]
    condition = city_entry_program(first.id, "condition", lifted_gate_steps(first, "condition"))
    info = city_entry_program(first.id, "information", lifted_gate_steps(first, "information"))
    vm.variables[f"MIL_{vm.number}_SCHONMALREINGELASSEN"] = 1
    vm.variables[f"MIL_{vm.number}_PERSONAL_ABSOLUTIONLEVEL"] = 99  # previous native bribe
    vm.fields[(npc, "AIVAR", 14)] = 1
    vm.variables.update(projection_values("city", [{"kind": "theft"}]))
    vm.variables[law_var("city")] = 1
    assert vm.execute(condition) == 1
    assert vm.fields[(npc, "AIVAR", 14)] == 0
    vm.execute(info)
    theft_line = next(
        e["id"] for e in first.effects if e.get("text") == "Мы не потерпим здесь грязных воров!"
    )
    assert theft_line in vm.lines

    # Native GrantAbsolution clears these projections at its call site, before runtime polling.
    vm.variables.update(projection_values("city", []))
    vm.variables[law_var("city")] = 0
    vm.variables["ABSOLUTIONLEVEL_CITY"] = 1
    vm.fields[(npc, "AIVAR", 12)] = 0
    assert vm.execute(condition) == 0
    assert vm.fields[(npc, "AIVAR", 14)] == 1
    permanent = real_graph.nodes[f"DIA_{npc}_PERM"]
    permit = city_entry_program(permanent.id, "condition", lifted_gate_steps(permanent, "condition"))
    assert vm.execute(permit) == 1


@pytest.mark.parametrize("npc", ["MIL_305_TORWACHE", "PAL_205_TORWACHE"])
def test_upper_gate_keeps_native_first_warning_without_law_and_blocks_known_thief(real_graph, npc):
    first = real_graph.nodes[f"DIA_{npc}_FIRSTWARN"]
    admission = real_graph.nodes[f"DIA_{npc}_PASSASMIL"]
    assert first.id in city_entry_nodes(real_graph)
    assert not any(op.get("node") == first.id for op in init_ops(real_graph))
    first_condition = city_entry_program(first.id, "condition", lifted_gate_steps(first, "condition"))
    admission_condition = city_entry_program(
        admission.id, "condition", lifted_gate_steps(admission, "condition")
    )
    vm = GateVM(npc)
    vm.fields[("PC_HERO", "GUILD")] = 2  # GIL_MIL: a native admission, without a known crime
    vm.variables["PETZCOUNTER_CITY_THEFT"] = 50  # an unreported native perception must not bar it
    assert vm.execute(first_condition) == 1  # the guard still stops a newcomer at his post
    assert vm.variables["PETZCOUNTER_CITY_THEFT"] == 0
    assert vm.execute(admission_condition) == 1

    # A known unpaid theft revokes an old pass and leaves the native refusal warning available.
    vm.variables[f"{npc[:7]}_SCHONMALREINGELASSEN"] = 1
    vm.fields[(npc, "AIVAR", 14)] = 1
    vm.variables.update(projection_values("city", [{"kind": "theft"}]))
    vm.variables[law_var("city")] = 1
    assert vm.execute(first_condition) == 1
    assert vm.fields[(npc, "AIVAR", 14)] == 0
    assert vm.execute(admission_condition) == 0
    vm.execute(city_entry_program(first.id, "information", lifted_gate_steps(first, "information")))
    theft_line = next(e["id"] for e in first.effects if "воров" in e.get("text", ""))
    assert theft_line in vm.lines


@pytest.mark.parametrize("suffix", ["BRIBE", "PASSIERSCHEIN", "ZUMSCHMIED", "CONSTANTINO", "PERM"])
def test_real_entry_alternatives_cannot_bypass_known_theft_but_keep_native_admission(real_graph, suffix):
    nodes = [real_graph.nodes[n] for n in city_entry_nodes(real_graph) if n.endswith("_" + suffix)]
    assert nodes
    for node in nodes:
        vm = GateVM(node.npc)
        if suffix == "PERM":
            vm.fields[(node.npc, "AIVAR", 14)] = 1
            vm.variables[f"{node.npc[:7]}_SCHONMALREINGELASSEN"] = 1
        condition = city_entry_program(node.id, "condition", lifted_gate_steps(node, "condition"))
        information = city_entry_program(node.id, "information", lifted_gate_steps(node, "information"))
        assert vm.execute(condition) == 1
        vm.variables.update(projection_values("city", [{"kind": "theft"}]))
        vm.variables[law_var("city")] = 1
        assert vm.execute(condition) == 0
        before_fields = dict(vm.fields)
        vm.execute(information)
        assert vm.lines == []
        assert vm.fields == before_fields
        vm.variables.update(projection_values("city", []))
        vm.variables[law_var("city")] = 0
        assert vm.execute(condition) == 1
        vm.execute(information)
        assert vm.lines
        assert vm.fields[(node.npc, "AIVAR", 14)] == 1


@pytest.mark.parametrize(
    "kind,blocked", [("sheep", False), ("attack", True), ("theft", True), ("murder", True)]
)
def test_gate_law_projects_zero_when_unknown_and_blocks_only_native_severity(kind, blocked):
    vm = GateVM()
    # Native witness pollution cannot block a gate before our report arrives.
    vm.variables["PETZCOUNTER_CITY_THEFT"] = 50
    steps = city_entry_program(
        "DIA_MIL_310_STADTWACHE_BRIBE", "condition", [{"do": "return", "value": ["int", 1]}]
    )
    assert vm.execute(steps) == 1
    assert vm.variables["PETZCOUNTER_CITY_THEFT"] == 0
    vm.variables.update(projection_values("city", [{"kind": kind}]))
    vm.variables[law_var("city")] = 1
    assert vm.execute(steps) == int(not blocked)


def test_real_gate_programs_pass_the_scoped_validator(real_graph):
    ops = [
        {"op": "program", "node": node, "part": part,
         "steps": city_entry_program(node, part, [
             {"do": "return", **({"value": ["int", 1]} if part == "condition" else {})}
         ])}
        for node in sorted(city_entry_nodes(real_graph))
        for part in ("condition", "information")
    ]
    state = WorldState(symbols={name: "var" for name in names_in(ops)})
    story = Story(real_graph, state).then(init_ops(real_graph))
    assert validate_ops(ops, story) == []


@pytest.fixture(scope="module")
def native_program_reader(tmp_path_factory):
    """Use the bridge's exact JSON program reader, including native return-type checks."""
    compiler = shutil.which("c++")
    if compiler is None:
        pytest.skip("needs a C++ compiler for the native bridge program reader")
    repo = Path(__file__).resolve().parents[2]
    bridge = repo / "OpenGothic" / "ai_bridge"
    directory = tmp_path_factory.mktemp("crime-gate-reader")
    source = directory / "reader.cpp"
    source.write_text(
        '#include "exprjson.h"\n'
        '#include <iostream>\n'
        '#include <iterator>\n'
        'int main() {\n'
        '  std::string input((std::istreambuf_iterator<char>(std::cin)), {});\n'
        '  rapidjson::Document document; document.Parse(input.c_str());\n'
        '  if(document.HasParseError() || !document.IsArray()) return 2;\n'
        '  bool failed = false;\n'
        '  for(auto& item : document.GetArray()) {\n'
        '    auto program = storygraph::readProgram(item["steps"], item["returns"].GetBool());\n'
        '    if(!program.error.empty()) { std::cerr << program.error << "\\n"; failed = true; }\n'
        '  }\n'
        '  return failed ? 1 : 0;\n'
        '}\n', encoding="utf-8",
    )
    executable = directory / "reader"
    subprocess.run(
        [compiler, "-std=c++20", "-O0", "-I", str(bridge / "storygraph"), "-I", str(bridge),
         "-isystem", str(bridge.parent / "lib" / "rapidjson" / "include"), str(source),
         str(bridge / "storygraph" / "exprjson.cpp"), str(bridge / "storygraph" / "expr.cpp"),
         str(bridge / "textcodec.cpp"), "-o", str(executable)],
        check=True, capture_output=True, text=True, timeout=60,
    )
    return executable


def test_native_reader_accepts_gate_programs_and_rejects_value_in_void_information(native_program_reader):
    programs = [
        {"returns": part == "condition", "steps": city_entry_program(
            f"DIA_MIL_{number}_STADTWACHE_{suffix}", part,
            [{"do": "return", **({"value": ["int", 1]} if part == "condition" else {})}],
        )}
        for number in ("310", "333")
        for suffix in ("FIRSTWARN", "BRIBE", "PASSIERSCHEIN", "ZUMSCHMIED", "CONSTANTINO", "PERM")
        for part in ("condition", "information")
    ]
    accepted = subprocess.run(
        [str(native_program_reader)], input=json.dumps(programs), text=True,
        capture_output=True, timeout=10,
    )
    assert accepted.returncode == 0, accepted.stderr
    bad = [{"returns": False, "steps": [{"do": "return", "value": ["int", 0]}]}]
    rejected = subprocess.run(
        [str(native_program_reader)], input=json.dumps(bad), text=True,
        capture_output=True, timeout=10,
    )
    assert rejected.returncode == 1
    assert "the function returns nothing" in rejected.stderr

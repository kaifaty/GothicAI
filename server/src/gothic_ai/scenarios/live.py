"""Recorded cases and reports for manual, repeated live-model e2e runs."""

from __future__ import annotations

import argparse
import json
import os
import shlex
import time
from collections import Counter
from contextlib import contextmanager
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any, Literal

from ..trace import TraceWriter

Outcome = Literal["success", "failure", "no_decision", "observed"]


@dataclass(frozen=True)
class CaseSpec:
    name: str
    expected: str
    estimated_requests: int


CASE_SPECS = {
    "live_deals": (
        CaseSpec("money_unasked", "Xardas offers no gold during 20 seconds without a request", 1),
        CaseSpec("money_asked", "Xardas proposes an accepted gold deal within his purse", 2),
        CaseSpec("parley_terms", "After an unprovoked attack is rejected, Brago offers terms", 3),
        CaseSpec("bounty_leverage", "Brago sells his secret instead of revealing it for free", 2),
        CaseSpec("cheat_keeps", "Dishonest Lobart answers without paying the broken promise", 1),
        CaseSpec("lead_offer", "Maleth proposes a deal to lead the hero to the mill", 1),
        CaseSpec("meet_agree", "Observe a meeting offer, another time, or refusal", 1),
        CaseSpec("amends_forgive", "Observe Andre's forgiveness offer and price, or refusal", 2),
        CaseSpec("crime_demand", "The Amends model writes an accepted demand without fallback", 3),
    ),
    "live_lies": (
        CaseSpec("seen_alibi", "Lobart catches the city alibi using his own farm sighting", 2),
        CaseSpec("trace_age", "Lobart catches the age lie using the fresh arrow trace", 2),
        CaseSpec("trace_control", "A stranger's arrow and denial produce no accusation", 1),
        CaseSpec("witness_alibi", "Lobart asks a witness and catches the alibi from testimony", 4),
        CaseSpec("vcheck_claim", "Lobart checks the field and catches the false predator claim", 3),
        CaseSpec("crime_testimony", "Lobart catches the theft denial from Hilda's testimony", 4),
        CaseSpec("frame_belief", "Observe blame of Malak, refusal, or a witness question", 2),
        CaseSpec("cover_offer", "Observe Hilda's cover offer, refusal, or counterprice", 2),
    ),
    "haggle": (CaseSpec("greedy_price", "Canthar offers less than Lutero for the same silver cup", 6),),
    "surrender_live": (
        CaseSpec("A_lobart", "Observe whether wounded Lobart surrenders, flees, or keeps fighting", 2),
        CaseSpec("A_bandit", "Observe whether a wounded bandit surrenders, flees, or keeps fighting", 2),
        CaseSpec("B_lobart", "Observe whether Lobart accepts the hero surrender and what ransom he takes", 3),
        CaseSpec(
            "B_bandit", "Observe whether the bandit accepts the hero surrender and what ransom he takes", 3
        ),
        CaseSpec("B_guard", "Observe whether the guard accepts surrender; acceptance must not humiliate", 3),
        CaseSpec("C_bandit", "Observe whether the bandit accepts the requested humiliating surrender", 3),
        CaseSpec(
            "C_brago", "Brago accepts the humiliating surrender, humiliates the hero and stops fighting", 3
        ),
    ),
    "showrunner_live": tuple(
        CaseSpec(name, "The occasion produces a canon-approved line built and played to completion", 8)
        for name in ("arrived", "deed", "chapter")
    ),
    "vanilla_line_live": tuple(
        CaseSpec(
            name, "The vanilla occasion produces a canon-approved line built and played to completion", 8
        )
        for name in ("running", "success")
    ),
    "interject": tuple(
        CaseSpec(name, "At most three voices and no overlapping speech; interveners are observed", 4)
        for name in ("greeting", "stranger", "brago")
    ),
    "chests_lobart": (
        CaseSpec("fetch_clothes", "Lobart walks to his chest for the clothes and the hero receives them", 4),
    ),
}


def selected_cases(suite: str, only: str | None = None) -> tuple[CaseSpec, ...]:
    if suite not in CASE_SPECS:
        raise ValueError(f"unknown live suite {suite!r}; choose {', '.join(CASE_SPECS)}")
    specs = CASE_SPECS[suite]
    if not only:
        return specs
    names = {name.strip() for name in only.split(",") if name.strip()}
    unknown = names - {spec.name for spec in specs}
    if unknown or not names:
        raise ValueError(f"unknown CASES for {suite}: {', '.join(sorted(unknown)) or only}")
    return tuple(spec for spec in specs if spec.name in names)


def _offsets(directory: Path) -> dict[Path, int]:
    return {path: path.stat().st_size for path in directory.glob("*.jsonl")}


@dataclass
class LiveCase:
    spec: CaseSpec
    trace: TraceWriter
    started_at: float = field(default_factory=time.time)
    ended_at: float | None = None
    outcome: Outcome | None = None
    evidence: dict[str, Any] = field(default_factory=dict)
    setup_error: str | None = None
    _start: dict[Path, int] = field(default_factory=dict)
    _end: dict[Path, int] | None = None

    def finish(self, outcome: Outcome, *, evidence: dict[str, Any] | None = None) -> None:
        if outcome not in {"success", "failure", "no_decision", "observed"}:
            raise ValueError(f"invalid live outcome {outcome!r}")
        self.outcome = outcome
        if evidence is not None:
            self.evidence.update(evidence)

    def _turns(self) -> list[tuple[Path, dict[str, Any]]]:
        turns = []
        for path, end in (self._end if self._end is not None else _offsets(self.trace.directory)).items():
            start = self._start.get(path, 0)
            with path.open("rb") as stream:
                stream.seek(start)
                lines = stream.read(max(0, end - start)).decode("utf-8").splitlines()
            for line in lines:
                record = json.loads(line)
                if record.get("type") == "turn":
                    turns.append((path, record))
        return turns

    @property
    def records(self) -> list[dict[str, Any]]:
        return [record for _, record in self._turns()]

    def turns(self, npc: str, text: str | None = None) -> list[dict[str, Any]]:
        return [
            record for record in self.records
            if record.get("npc") == npc and record.get("status") in {"completed", "step_cap"}
            and (text is None or text in record.get("trigger", {}).get("text", ""))
        ]

    def calls(self, npc: str, name: str | None = None) -> list[dict[str, Any]]:
        return [
            call for record in self.records if record.get("npc") == npc
            for step in record.get("steps", []) for call in step.get("calls", [])
            if name is None or call.get("name") == name
        ]

    @property
    def requests(self) -> int:
        return sum(
            len(record.get("steps", []))
            + sum(event.get("kind") == "retry" for event in record.get("events", []))
            for record in self.records
        )

    def json(self) -> dict[str, Any]:
        turns = self._turns()
        transcripts: dict[str, list[str]] = {}
        for path, record in turns:
            transcripts.setdefault(str(path.resolve()), []).append(record.get("id", ""))
        return {
            **asdict(self.spec),
            "outcome": self.outcome,
            "evidence": self.evidence,
            "setup_error": self.setup_error,
            "started_at": self.started_at,
            "ended_at": self.ended_at,
            "requests": self.requests,
            "transcripts": [{"path": path, "turns": ids} for path, ids in transcripts.items()],
            "trace_directory": str(self.trace.directory.resolve()),
        }


class LiveSuite:
    def __init__(self, name: str, trace: TraceWriter | None) -> None:
        if trace is None:
            raise ValueError(f"live suite {name} requires a TraceWriter directory")
        self.name, self.trace = name, trace
        self.specs = selected_cases(name, os.environ.get("CASES"))
        self.selected = tuple(spec.name for spec in self.specs)
        self.cases: list[LiveCase] = []
        self.started_at = time.time()
        self._write()

    @contextmanager
    def case(self, name: str):
        spec = next((spec for spec in self.specs if spec.name == name), None)
        if spec is None:
            raise ValueError(f"case {name!r} is not selected for {self.name}")
        if any(case.spec.name == name for case in self.cases):
            raise ValueError(f"case {name!r} was already played")
        case = LiveCase(spec, self.trace, _start=_offsets(self.trace.directory))
        self.cases.append(case)
        try:
            yield case
            if case.outcome is None:
                raise RuntimeError("case ended without an outcome")
        except Exception as error:
            case.setup_error = f"{type(error).__name__}: {error}"
            case.outcome = None
        finally:
            case.ended_at = time.time()
            case._end = _offsets(self.trace.directory)
            self._write()

    def _write(self) -> None:
        path = self.trace.directory.parent / "live.json"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(
            json.dumps(
                {
                    "suite": self.name,
                    "started_at": self.started_at,
                    "ended_at": time.time(),
                    "cases": [case.json() for case in self.cases],
                },
                ensure_ascii=False,
                indent=2,
            )
            + "\n",
            encoding="utf-8",
        )

    def setup_error(self, error: Exception | str) -> None:
        for spec in self.specs:
            if not any(case.spec.name == spec.name for case in self.cases):
                case = LiveCase(spec, self.trace, _start=_offsets(self.trace.directory))
                case.setup_error = str(error)
                case.ended_at = time.time()
                case._end = case._start.copy()
                self.cases.append(case)
        self._write()

    def results(self) -> dict[str, str]:
        self.setup_error("case was not played")
        result = {
            case.spec.name: f"failed ({case.setup_error})" if case.setup_error else f"done ({case.outcome})"
            for case in self.cases
        }
        return result


def summarize(out: Path, suite: str, runs: int, codes: list[int], only: str | None = None) -> dict[str, Any]:
    specs = selected_cases(suite, only)
    rows: dict[str, list[dict[str, Any]]] = {spec.name: [] for spec in specs}
    attempts = []
    for index in range(1, runs + 1):
        run = out / f"run-{index}"
        path = run / "live.json"
        code = codes[index - 1]
        attempt: dict[str, Any] = {"run": index, "exit_code": code, "live": str(path), "cases": []}
        if path.exists():
            data = json.loads(path.read_text())
            if data["suite"] != suite:
                raise ValueError(f"wrong suite in {path}: {data['suite']}")
            attempt["cases"] = data["cases"]
            for case in data["cases"]:
                if case["name"] in rows:
                    rows[case["name"]].append({**case, "run": index})
        else:
            attempt["setup_error"] = "e2e did not write live.json"
        attempts.append(attempt)
    cases = []
    for spec in specs:
        records = rows[spec.name]
        counts = Counter(record.get("outcome") for record in records)
        played = sum(counts[outcome] for outcome in ("success", "failure", "no_decision", "observed"))
        observed = counts["observed"]
        requests = sum(record["requests"] for record in records)
        decisions = Counter(
            str(record["evidence"]["decision"])
            for record in records
            if record.get("outcome") is not None and "decision" in record.get("evidence", {})
        )
        transcripts = [
            {**transcript, "run": record["run"]}
            for record in records
            for transcript in record.get("transcripts", [])
        ]
        cases.append(
            {
                **asdict(spec),
                "successes": counts["success"],
                "attempts": played,
                "success_rate": counts["success"] / played if played and not observed else None,
                "failures": counts["failure"],
                "no_decision": counts["no_decision"],
                "observed": observed,
                "setup_errors": runs - played,
                "requests": requests,
                "mean_requests": requests / len(records) if records else 0,
                "decisions": {
                    decision: {"count": count, "share": count / decisions.total()}
                    for decision, count in decisions.items()
                },
                "transcripts": transcripts,
            }
        )
    return {
        "suite": suite,
        "runs": runs,
        "command": (
            f"SUITE={shlex.quote(suite)} RUNS={runs} "
            + (f"CASES={shlex.quote(only)} " if only else "")
            + f"LLM={shlex.quote(os.environ.get('LLM', 'chatgpt'))} scripts/e2e-live.sh"
        ),
        "cases": cases,
        "attempts": attempts,
        "requests": sum(case["requests"] for case in cases),
        "setup_errors": sum(case["setup_errors"] for case in cases),
    }


def write_report(out: Path, report: dict[str, Any]) -> None:
    (out / "report.json").write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    lines = [
        f"# Live e2e: {report['suite']}",
        "",
        f"Runs: {report['runs']}. Requests: {report['requests']}.",
        "",
        "| Case | Expected | Successes / attempts | Rate | Failures | No decision "
        "| Observed | Requests / mean |",
        "| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |",
    ]
    for case in report["cases"]:
        rate = f"{case['success_rate']:.1%}" if case["success_rate"] is not None else "observation"
        lines.append(
            f"| {case['name']} | {case['expected']} | {case['successes']}/{case['attempts']} | {rate} | "
            f"{case['failures']} | {case['no_decision']} | {case['observed']} | "
            f"{case['requests']} / {case['mean_requests']:.2f} |"
        )
    lines += ["", "```sh", report["command"], "```"]
    for case in report["cases"]:
        lines += ["", f"## {case['name']}", ""]
        if case["setup_errors"]:
            lines.append(f"Setup errors or unplayed cases: {case['setup_errors']}.")
        for decision, distribution in case["decisions"].items():
            lines.append(f"- {decision}: {distribution['count']} ({distribution['share']:.1%} of decisions)")
        for transcript in case["transcripts"]:
            lines.append(
                f"- Run {transcript['run']}: [{Path(transcript['path']).name}]({transcript['path']}) "
                f"(turns: {', '.join(transcript['turns'])})"
            )
    lines += ["", "## Runs", ""]
    for attempt in report["attempts"]:
        lines.append(
            f"- Run {attempt['run']}: exit {attempt['exit_code']}; "
            f"[live.json]({attempt['live']}); "
            f"[server.log]({Path(attempt['live']).parent / 'server.log'})"
        )
        if error := attempt.get("setup_error"):
            lines.append(f"  {error}")
        for case in attempt["cases"]:
            if case.get("setup_error"):
                lines.append(f"  {case['name']}: {case['setup_error']}")
    (out / "report.md").write_text("\n".join(lines) + "\n", encoding="utf-8")


def estimate(suite: str, runs: int, history: Path, only: str | None = None) -> float:
    samples: dict[str, tuple[int, float]] = {}
    for path in history.glob("**/report.json"):
        report = json.loads(path.read_text())
        if report.get("suite") != suite:
            continue
        for case in report["cases"]:
            if case["attempts"]:
                prior = samples.get(case["name"], (0, 0))
                samples[case["name"]] = (prior[0] + case["attempts"], prior[1] + case["requests"])
    per_run = sum(
        samples[spec.name][1] / samples[spec.name][0] if spec.name in samples else spec.estimated_requests
        for spec in selected_cases(suite, only)
    )
    return runs * per_run


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("estimate", "report"))
    args = parser.parse_args()
    suite, runs = os.environ["SUITE"], int(os.environ["RUNS"])
    only = os.environ.get("CASES")
    if args.action == "estimate":
        value = estimate(suite, runs, Path(os.environ["LIVE_HISTORY"]), only)
        count = len(selected_cases(suite, only))
        print(
            f"Estimated model requests: {count} cases × {runs} runs × {value / runs / count:.2f} "
            f"requests/case = {value:.1f} (past reports where available, otherwise declared estimates)"
        )
    else:
        out = Path(os.environ["OUT"])
        codes = [int(code) for code in os.environ["E2E_EXIT_CODES"].split()]
        report = summarize(out, suite, runs, codes, only)
        write_report(out, report)
        print(f"Report: {out / 'report.md'}")
        if report["setup_errors"] or any(codes):
            raise SystemExit(1)

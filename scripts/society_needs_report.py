#!/usr/bin/env python3
"""Where the routine does not close the needs: the society stand's people by their routine only, with needs.

    python3 scripts/society_needs_report.py --world WORLD --config CONFIG --tool GOTHIC_SOCIETY_DAY --out DIR
        [--work WORK] [--seed S] [--days 1,7]

Runs `gothic-society-day --config` on the export WORLD for each length in --days (a day and a week by default) and
reads its journal v3 (docs/society-sim.md, «Журнал и отметки»): the need_threshold events give every person's time
low and critical exactly, activity_started what it was doing meanwhile. For each need it counts the people who went
low and critical at least once, the hours in critical (median over all people, median over those who went critical,
the most), who is worst by guild and by the state they spend the day in, and which states their critical hours
fall in. Only people (`person` in the export) have needs.

Writes DIR/needs.json (every number, both runs) and DIR/needs.md (the tables); the conclusions go into DIR/README.md
by hand. WORK keeps the runs (a temporary folder by default).
"""

from __future__ import annotations

import argparse
import json
import statistics
import struct
import subprocess
import sys
import tempfile
from collections import Counter, defaultdict
from pathlib import Path

JOURNAL_HEADER = struct.Struct("<8sIIQQqIII")
JOURNAL_RECORD = struct.Struct("<IIBBHIII")
ACTIVITY_STARTED = 4
NEED_THRESHOLD = 6
DOWN, UP = 0, 1
LOW, CRITICAL = 1, 2
DAY = 24 * 3600
# the export's guild numbers (the scripts' GIL_*), as characters.py names them, and the orcs
GUILDS = {
    0: "без гильдии",
    1: "паладины",
    2: "ополчение",
    3: "горожане",
    4: "маги Огня",
    5: "послушники",
    6: "охотники на драконов",
    7: "наёмники",
    8: "крестьяне",
    9: "бандиты",
    10: "заключённые",
    11: "ищущие",
    12: "поселенцы",
    13: "пираты",
    14: "маги Воды",
    59: "орки",
}
# a person's day without its sleep: the state of the day is the one it spends the most of its waking time in
SLEEP_STATES = {"ZS_GOTOBED", "ZS_SLEEP", "ZS_SLEEP_GROUND", "ZS_MM_RTN_SLEEP"}
WORST = 5
TOP_STATES = 12


def read_journal(path: Path) -> dict:
    data = path.read_bytes()
    magic, version, size, world_hash, seed, start, npcs, points, states = JOURNAL_HEADER.unpack_from(data)
    if magic != b"GSOCJRNL" or version != 3 or size != JOURNAL_RECORD.size:
        raise SystemExit(f"{path}: not a society journal v3 (needs): version {version}")
    at = JOURNAL_HEADER.size
    names = []
    for _ in range(states):
        (n,) = struct.unpack_from("<H", data, at)
        names.append(data[at + 2 : at + 2 + n].decode())
        at += 2 + n
    config_hash, needs = struct.unpack_from("<QI", data, at)
    at += 12
    need_ids = []
    for _ in range(needs):
        (n,) = struct.unpack_from("<H", data, at)
        need_ids.append(data[at + 2 : at + 2 + n].decode())
        at += 2 + n
    records = list(JOURNAL_RECORD.iter_unpack(data[at:]))
    return {
        "world_hash": f"{world_hash:016x}",
        "config_hash": f"{config_hash:016x}",
        "seed": seed,
        "start": start,
        "npcs": npcs,
        "states": names,
        "needs": need_ids,
        "records": records,
    }


def run_tool(tool: Path, world: Path, config: Path, out: Path, days: int, seed: int) -> None:
    out.mkdir(parents=True, exist_ok=True)
    cmd = [str(tool), "--world", str(world), "--config", str(config), "--out", str(out)]
    cmd += ["--days", str(days), "--seed", str(seed)]
    done = subprocess.run(cmd, capture_output=True, text=True)
    (out / "day.log").write_text(done.stdout + done.stderr)
    if done.returncode != 0:
        raise SystemExit(f"{tool} exited with {done.returncode}:\n{done.stdout}{done.stderr}")


def median(values: list[float]) -> float:
    return statistics.median(values) if values else 0.0


def analyse(journal: dict, world: dict, config: dict, days: int) -> dict:
    """Per person and need: went low / critical, seconds low or worse and critical, and by state."""
    needs = journal["needs"]
    states = journal["states"]
    start = journal["start"]
    end = start + days * DAY
    npcs = world["npcs"]
    people = [i for i, npc in enumerate(npcs) if npc["person"]]
    person = set(people)
    # a sweep over each person's events: between two of its events it does one state and each need is in one zone
    current = {i: None for i in people}
    since = {i: start for i in people}
    zone = {i: [0] * len(needs) for i in people}
    went = {i: [[False, False] for _ in needs] for i in people}
    low_s = {i: [0] * len(needs) for i in people}
    crit_s = {i: [0] * len(needs) for i in people}
    state_s = {i: Counter() for i in people}
    crit_by_state = [Counter() for _ in needs]

    def spend(i: int, t: int) -> None:
        dt = t - since[i]
        if dt <= 0:
            return
        s = current[i]
        state_s[i][s] += dt
        for k, z in enumerate(zone[i]):
            if z >= LOW:
                low_s[i][k] += dt
            if z >= CRITICAL:
                crit_s[i][k] += dt
                crit_by_state[k][s] += dt
        since[i] = t

    for t, npc, kind, reason, _entry, a, b, _c in journal["records"]:
        if npc not in person:
            continue
        if kind == ACTIVITY_STARTED:
            spend(npc, t)
            current[npc] = states[a]
        elif kind == NEED_THRESHOLD:
            spend(npc, t)
            if reason == DOWN:
                zone[npc][a] = b
                went[npc][a][b - 1] = True
            else:
                zone[npc][a] = b - 1
    for i in people:
        spend(i, end)

    def day_state(i: int) -> str:
        awake = [(s, v) for s, v in state_s[i].items() if s not in SLEEP_STATES and s is not None]
        if not awake:
            return "только сон" if state_s[i] else "-"
        return max(awake, key=lambda x: (x[1], x[0]))[0]

    main_state = {i: day_state(i) for i in people}
    hours = 3600.0
    # which states of the config fill each need, and who has one in its day (routine, follow-on, start_aistate)
    fill_states = [set() for _ in needs]
    for row in config["states"]:
        for need_id in row["fill_hours"]:
            fill_states[needs.index(need_id)].add(row["state"])
    has_fill = [set() for _ in needs]
    for i in people:
        npc = npcs[i]
        own = {e["state"] for e in npc["routine"]}
        if "ZS_GOTOBED" in own:
            own.add("ZS_SLEEP")
        if npc.get("start_aistate"):
            own.add(npc["start_aistate"])
        for window in npc.get("mm_schedule") or []:
            own.add(window[0])
        for k in range(len(needs)):
            if own & fill_states[k]:
                has_fill[k].add(i)

    by_need = []
    for k, need_id in enumerate(needs):
        crit = [crit_s[i][k] / hours for i in people]
        went_crit = [crit_s[i][k] / hours for i in people if went[i][k][1]]
        worst = sorted(people, key=lambda i: (-crit_s[i][k], npcs[i]["key"]))[:WORST]
        total = sum(crit_by_state[k].values())
        top = crit_by_state[k].most_common(5)
        by_need.append(
            {
                "need": need_id,
                "name": config_need(config, need_id)["name"],
                "people": len(people),
                "routine_fills": len(has_fill[k]),
                "fill_states": sorted(fill_states[k]),
                "went_low": sum(went[i][k][0] for i in people),
                "went_critical": sum(went[i][k][1] for i in people),
                "low_hours_median": round(median([low_s[i][k] / hours for i in people]), 2),
                "critical_hours_median": round(median(crit), 2),
                "critical_hours_median_of_critical": round(median(went_crit), 2),
                "critical_hours_max": round(max(crit) if crit else 0.0, 2),
                "critical_person_hours": round(sum(crit), 1),
                "critical_by_state": [
                    {"state": s or "-", "hours": round(v / hours, 1), "share": round(v / total, 3) if total else 0.0}
                    for s, v in top
                ],
                "worst": [
                    {
                        "key": npcs[i]["key"],
                        "name": npcs[i]["name"],
                        "guild": GUILDS.get(npcs[i]["guild"], str(npcs[i]["guild"])),
                        "day_state": main_state[i],
                        "critical_hours": round(crit_s[i][k] / hours, 1),
                    }
                    for i in worst
                ],
            }
        )

    def group(key) -> list[dict]:
        members = defaultdict(list)
        for i in people:
            members[key(i)].append(i)
        rows = []
        for name, group_people in members.items():
            rows.append(
                {
                    "group": name,
                    "people": len(group_people),
                    "critical_share": {
                        needs[k]: round(sum(went[i][k][1] for i in group_people) / len(group_people), 3)
                        for k in range(len(needs))
                    },
                    "critical_hours_per_day": {
                        needs[k]: round(
                            statistics.mean(crit_s[i][k] / hours for i in group_people) / days, 2
                        )
                        for k in range(len(needs))
                    },
                }
            )
        rows.sort(key=lambda r: (-r["people"], r["group"]))
        return rows

    return {
        "days": days,
        "people": len(people),
        "needs": by_need,
        "by_guild": group(lambda i: GUILDS.get(npcs[i]["guild"], str(npcs[i]["guild"]))),
        "by_day_state": group(lambda i: main_state[i]),
    }


def config_need(config: dict, need_id: str) -> dict:
    return next(n for n in config["needs"] if n["id"] == need_id)


def fmt(x: float) -> str:
    return f"{x:.1f}".replace(".", ",")


def pct(x: float) -> str:
    return f"{x * 100:.0f}%"


def markdown(report: dict, config: dict) -> str:
    out = ["# Нужды под распорядком: таблицы", ""]
    out.append(
        f"Выгрузка `{report['world_hash']}`, настройка `{report['config_hash']}`, зерно {report['seed']}. "
        f"Люди — `person` выгрузки. Таблицы пишет `scripts/society_needs_report.py`, выводы — в README."
    )
    names = {n["id"]: n["name"] for n in config["needs"]}
    for run in report["runs"]:
        days = run["days"]
        title = "Сутки" if days == 1 else f"{days} суток"
        people = run["people"]
        out += ["", f"## {title}, {people} человек", ""]
        out.append(
            "| Нужда | В распорядке есть чем утолить | Дошли до «низко» | Дошли до «критично» "
            "| Часов в «критично»: медиана всех | медиана дошедших | максимум |"
        )
        out.append("|---|---|---|---|---|---|---|")
        for n in run["needs"]:
            out.append(
                f"| {n['name']} | {n['routine_fills']} | {n['went_low']} | {n['went_critical']} "
                f"| {fmt(n['critical_hours_median'])} | {fmt(n['critical_hours_median_of_critical'])} "
                f"| {fmt(n['critical_hours_max'])} |"
            )
        out += ["", "В каких состояниях проходят часы в «критично» (доля человеко-часов):", ""]
        for n in run["needs"]:
            if not n["critical_by_state"]:
                continue
            parts = ", ".join(f"`{s['state']}` {pct(s['share'])}" for s in n["critical_by_state"])
            out.append(f"- {n['name']}: {parts}")
        out += ["", "Хуже всех по каждой нужде (часов в «критично» за прогон):", ""]
        for n in run["needs"]:
            worst = [w for w in n["worst"] if w["critical_hours"] > 0]
            if not worst:
                continue
            parts = "; ".join(
                f"{w['name']} (`{w['key']}`, {w['guild']}, `{w['day_state']}`) {fmt(w['critical_hours'])}"
                for w in worst
            )
            out.append(f"- {n['name']}: {parts}")
        for title2, key, limit in (
            ("По гильдиям", "by_guild", None),
            ("По занятию дня (состояние, где человек проводит больше всего времени без сна)", "by_day_state", TOP_STATES),
        ):
            rows = run[key][:limit] if limit else run[key]
            ids = [n["need"] for n in run["needs"]]
            out += ["", f"{title2}: часов в «критично» в сутки на человека, в скобках — доля дошедших до него.", ""]
            out.append("| | Людей | " + " | ".join(names[i] for i in ids) + " |")
            out.append("|---|---" + "|---" * len(ids) + "|")
            for r in rows:
                cells = " | ".join(
                    f"{fmt(r['critical_hours_per_day'][i])} ({pct(r['critical_share'][i])})" for i in ids
                )
                out.append(f"| {r['group']} | {r['people']} | {cells} |")
    return "\n".join(out) + "\n"


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--world", required=True, type=Path, help="society-world export")
    ap.add_argument("--config", required=True, type=Path, help="config file with needs (data/society/config.json)")
    ap.add_argument("--tool", required=True, type=Path, help="gothic-society-day")
    ap.add_argument("--out", required=True, type=Path, help="report folder: needs.json and needs.md")
    ap.add_argument("--work", type=Path, help="folder for the runs (a temporary one by default)")
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--days", default="1,7", help="lengths of the runs in days, comma-separated")
    args = ap.parse_args(argv)
    world = json.loads(args.world.read_text())
    config = json.loads(args.config.read_text())
    lengths = [int(x) for x in args.days.split(",")]
    with tempfile.TemporaryDirectory() as tmp:
        work = args.work or Path(tmp)
        runs = []
        journal = None
        for days in lengths:
            out = work / f"days-{days}"
            run_tool(args.tool, args.world, args.config, out, days, args.seed)
            journal = read_journal(out / "society-day.journal")
            runs.append(analyse(journal, world, config, days))
    report = {
        "world_hash": journal["world_hash"],
        "config_hash": journal["config_hash"],
        "seed": args.seed,
        "needs": [
            {k: n[k] for k in ("id", "name", "empty_hours", "low", "critical")} for n in config["needs"]
        ],
        "runs": runs,
    }
    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / "needs.json").write_text(json.dumps(report, ensure_ascii=False, indent=1) + "\n")
    (args.out / "needs.md").write_text(markdown(report, config))
    for run in runs:
        print(f"{run['days']} days, {run['people']} people:")
        for n in run["needs"]:
            print(
                f"  {n['need']}: fills in routine {n['routine_fills']}, low {n['went_low']}, critical "
                f"{n['went_critical']}, critical hours median {n['critical_hours_median']} "
                f"(of critical {n['critical_hours_median_of_critical']}), max {n['critical_hours_max']}"
            )
    return 0


if __name__ == "__main__":
    sys.exit(main())

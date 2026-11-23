#!/usr/bin/env python3
"""The society stand's day report with its guard rails (docs/society-sim.md, «Журнал, отчёт, повтор»).

    python3 scripts/society_day_report.py --world WORLD --config CONFIG --tool GOTHIC_SOCIETY_DAY
        --out DIR [--work WORK] [--seed S] [--check]

Runs `gothic-society-day --config` on the export WORLD: the routine policy and the teacher for a day and for
7 days, the random policy for 7 days, the teacher's day once more for each need with that need at 0 for everyone at
the start, and the teacher's 7 days without the characters' cards. Reads the journals v3, v5 and v6 and the marks and
measures, for the people who decide (a person with a routine, not protected, of a guild that decides, not on a post
all day while posts do not release for needs):

- hours in critical a day by need: median and most over person-days (thresholds ≤ 1 h and ≤ 4 h);
- choices out of the mask, checked anew from the export and the config: the owner (the mobsi's, otherwise the
  room's; outdoors and places are public, own rooms only own or the guild's), the guilds of the kind, the anchor
  entry without a critical need the option fills, a chooser that does not decide, the whole walk over the travel
  limit — the rest of a walk under way and the walk from its end to the place, from the export (threshold 0);
- holders of a place over its kind's capacity: the routine's users (place_held, place_released) and the chosen
  activities with their reservations (threshold 0);
- the protected NPCs' marks against the routine run (threshold 100%);
- the share of anchor time spent by the routine (threshold ≥ 95%);
- changes of activity per NPC in a game hour, p95 over NPC-hours (threshold ≤ 3);
- sensitivity: with one need at 0 at the start the first day's time in states that fill it grows, for every need;
- characters: without the cards the talkative spend less time on chosen social activities and the quiet more;
  same-guild pairs with different talk differ the same way;
- speed: the tool's simulation time a day (threshold ≤ 0.5 s).

Writes DIR/day-report.json (every number) and DIR/day-report.md (the tables against the thresholds); the
conclusions go into DIR/README.md by hand. WORK keeps the runs (a temporary folder by default). --check exits 1 when
a threshold fails.
"""

from __future__ import annotations

import argparse
import heapq
import json
import math
import re
import statistics
import struct
import subprocess
import sys
import tempfile
from collections import Counter, defaultdict
from pathlib import Path

JOURNAL_HEADER = struct.Struct("<8sIIQQqIII")
FLOAT32 = struct.Struct("<f")
NEAR_POINT_CM = 500  # Society::NearPointCm: a place this near is done where the NPC stands
JOURNAL_RECORD = struct.Struct("<IIBBHIII")
ROUTINE_ENTRY, TRAVEL_STARTED, ARRIVED, ACTIVITY_STARTED, TELEPORTED, NEED_THRESHOLD = 1, 2, 3, 4, 5, 6
DECISION_REQUESTED, ACTION_CHOSEN, ACTION_FAILED, ACTIVITY_FINISHED = 7, 8, 9, 10
PLACE_HELD, PLACE_RELEASED = 11, 12
DOWN = 0
LOW, CRITICAL = 1, 2
CHOSEN, FOLLOW_ON, START_STATE = 3, 2, 1
TAKEN, INTERRUPTED, EVICTED = 1, 2, 3
RECHECK = 128  # a cause bit of decision_requested (v6): a need in want not filled by what the NPC does
FINISHED = ["setpoint", "max time", "switched", "anchor", "evicted", "above low"]
NO_KIND = 0xFFFF
NO_OBJECT = 0xFFFFFFFF
DAY = 24 * 3600
HOUR = 3600
SOCIAL = "social"

THRESHOLDS = {
    "critical_median_h": 1.0,
    "critical_max_h": 4.0,
    "out_of_mask": 0,
    "over_capacity": 0,
    "protected_same": 1.0,
    "anchor_by_routine": 0.95,
    "changes_p95": 3,
    "day_seconds": 0.5,
}


# --- Reading -------------------------------------------------------------------------------------------------


def read_journal(path: Path) -> dict:
    data = path.read_bytes()
    magic, version, size, world_hash, seed, start, npcs, _points, states = JOURNAL_HEADER.unpack_from(data)
    # v4 (decisions before the places counted the routine's users) is not read: its holders are not all there. v6 is
    # v5 with the recheck cause of decision_requested.
    if magic != b"GSOCJRNL" or version not in (3, 5, 6) or size != JOURNAL_RECORD.size:
        raise SystemExit(f"{path}: not a society journal v3, v5 or v6: version {version}")
    at = JOURNAL_HEADER.size

    def names(count: int) -> list[str]:
        nonlocal at
        out = []
        for _ in range(count):
            (n,) = struct.unpack_from("<H", data, at)
            out.append(data[at + 2 : at + 2 + n].decode())
            at += 2 + n
        return out

    state_names = names(states)
    config_hash, needs = struct.unpack_from("<QI", data, at)
    at += 12
    need_ids = names(needs)
    policy, menu = "routine", 0
    if version >= 5:
        (n,) = struct.unpack_from("<H", data, at)
        policy = data[at + 2 : at + 2 + n].decode()
        at += 2 + n
        (menu,) = struct.unpack_from("<I", data, at)
        at += 4
    return {
        "version": version,
        "world_hash": f"{world_hash:016x}",
        "config_hash": f"{config_hash:016x}",
        "seed": seed,
        "start": start,
        "npcs": npcs,
        "states": state_names,
        "needs": need_ids,
        "policy": policy,
        "menu": menu,
        "records": list(JOURNAL_RECORD.iter_unpack(data[at:])),
    }


def read_unmet(path: Path, rules: "Rules", needs: list[str]) -> list[dict]:
    """The needs not in use (society-needs-unmet.csv of gothic-society-day): nothing fills them within the travel
    limit of the NPC's routine; the nearest way and its walk."""
    keys = {n["key"]: i for i, n in enumerate(rules.npcs)}
    out = []
    lines = path.read_text().splitlines()[1:] if path.exists() else []
    for line in lines:
        key, need, kind, place, minutes = line.split(",")
        out.append({
            "npc": keys[key],
            "key": key,
            "guild": rules.npcs[keys[key]]["guild"],
            "need": need,
            "k": needs.index(need),
            "kind": None if kind == "-" else kind,
            "place": None if place == "-" else place,
            "travel_min": None if minutes == "-" else float(minutes),
        })
    return out


def run_tool(tool: Path, world: Path, config: Path, out: Path, days: int, seed: int, policy: str) -> dict:
    out.mkdir(parents=True, exist_ok=True)
    cmd = [str(tool), "--world", str(world), "--config", str(config), "--out", str(out)]
    cmd += ["--days", str(days), "--seed", str(seed), "--policy", policy]
    done = subprocess.run(cmd, check=False, capture_output=True, text=True)
    (out / "day.log").write_text(done.stdout + done.stderr)
    if done.returncode != 0:
        raise SystemExit(f"{tool} exited with {done.returncode}:\n{done.stdout}{done.stderr}")
    ms = [float(m.group(1)) for m in re.finditer(r"^day \d+: \d+ events, ([0-9.]+) ms$", done.stdout, re.M)]
    decisions = re.search(r"policy \w+: (\d+) decisions, (\d+) rows", done.stdout)
    return {
        "dir": out,
        "day_ms": ms,
        "decisions": int(decisions.group(1)) if decisions else 0,
        "rows": int(decisions.group(2)) if decisions else 0,
    }


def llround(x: float) -> int:
    """C's llround for x >= 0: halves away from zero (Python's round() goes to even)."""
    q = math.floor(x)
    return int(q) + (1 if x - q >= 0.5 else 0)


class Walks:
    """Game seconds of walking to an option as the core's menu counts them (Paths::length, travelSeconds), from the
    export alone: the shortest waynet path in integer cm, a freepoint through its near_waypoint and the straight
    segment to it (float positions, as the core reads them), length / own walk speed × clock rounded to the second;
    none to the point it stands at or within NearPointCm. Dijkstra from each waynet point once, bounded by the
    longest walk any NPC makes in the travel limit: what lies beyond is over the limit for everyone."""

    def __init__(self, world: dict, limit: int):
        pts = world["waypoints"]
        self.pos = [tuple(FLOAT32.unpack(FLOAT32.pack(v))[0] for v in p["pos"]) for p in pts]
        self.graph = [p["near_waypoint"] if p["freepoint"] else p["id"] for p in pts]
        self.graph_cm = [llround(math.sqrt(self.dist2(i, self.graph[i]))) if p["freepoint"] else 0
                         for i, p in enumerate(pts)]
        self.edges: dict[int, list] = defaultdict(list)
        for a, b, cm in world["edges"]:
            self.edges[a].append((b, cm))
        self.clock = world["clock"]["game_seconds_per_real_second"]
        median = world["movement"]["walk_cm_per_s"]
        self.walk = [n["walk_cm_per_s"] if n.get("walk_cm_per_s", 0) > 0 else median for n in world["npcs"]]
        self.bound = (limit + 1) * max(self.walk) / self.clock + 1000
        self.from_point: dict[int, dict] = {}

    def dist2(self, a: int, b: int) -> float:
        p, q = self.pos[a], self.pos[b]
        dx, dy, dz = p[0] - q[0], p[1] - q[1], p[2] - q[2]
        return (dx * dx + dy * dy) + dz * dz

    def seconds(self, npc: int, origin: int, point: int) -> float:
        if point == origin:
            return 0
        g = self.graph[origin]
        if g not in self.from_point:
            dist = {g: 0}
            heap = [(0, g)]
            while heap:
                d, u = heapq.heappop(heap)
                if d > dist[u]:
                    continue
                for v, cm in self.edges[u]:
                    nd = d + cm
                    if nd <= self.bound and nd < dist.get(v, nd + 1):
                        dist[v] = nd
                        heapq.heappush(heap, (nd, v))
            self.from_point[g] = dist
        d = self.from_point[g].get(self.graph[point])
        if d is None:
            return math.inf
        cm = self.graph_cm[origin] + d + self.graph_cm[point]
        if cm <= NEAR_POINT_CM and self.dist2(point, origin) <= NEAR_POINT_CM * NEAR_POINT_CM:
            return 0
        return llround(cm / self.walk[npc] * self.clock) if cm > 0 else 0


class Rules:
    """What the export and the config say, read anew: who decides, owners, kinds, states."""

    def __init__(self, world: dict, config: dict, config_path: Path):
        self.world = world
        self.config = config
        npcs = world["npcs"]
        self.npcs = npcs
        self.needs = [n["id"] for n in config["needs"]]
        self.states = {s["state"]: s for s in config["states"]}
        self.kinds = config["object_kinds"]
        dec = config["decisions"]
        self.dec = dec
        self.limit = round(dec["travel_limit_min"] * 60)
        self.walks = Walks(world, self.limit)
        protected = []
        if dec.get("protected"):
            path = Path(dec["protected"])
            path = path if path.is_absolute() else config_path.parent / path
            protected = [e["id"] for e in json.loads(path.read_text())]
        ids = {n["id"]: i for i, n in enumerate(npcs)}
        self.protected = {ids[p] for p in protected if p in ids}
        posts = dec["post_states"]
        self.release = dec["posts_release_for_needs"]

        def post(state: str) -> bool:
            return any(state.startswith(p[:-1]) if p.endswith("*") else state == p for p in posts)

        self.post = post
        no_needs = set(dec["no_needs_guilds"])
        self.decider = set()
        self.on_post_all_day = set()
        for i, n in enumerate(npcs):
            if not (n["person"] and n["routine"] and i not in self.protected and n["guild"] not in no_needs):
                continue
            if not self.release and all(post(r["state"]) for r in n["routine"]):
                self.on_post_all_day.add(i)
                continue
            self.decider.add(i)
        # Free hours a day by the routine (minutes not in an anchor entry), as Npc::currentRoutine fills the day.
        self.free_h = {}
        for i in self.decider | self.on_post_all_day:
            day = [None] * 1440
            for r in npcs[i]["routine"]:
                m = r["start"]
                while True:
                    if day[m] is None:
                        day[m] = r["state"]
                    m = (m + 1) % 1440
                    if m == r["end"]:
                        break
            self.free_h[i] = sum(1 for st in day if st and not self.anchor(st)) / 60
        rooms = {r["name"]: r for r in world.get("rooms", [])}
        names = dec["guild_names"]

        def room_owner(room: str | None) -> tuple[str | None, int | None]:
            if not room:
                return None, None
            r = rooms[room]
            return r["owner_npc"], r["owner_guild"]

        self.object_owner = []
        for o in world["objects"]:
            if o["owner_npc"] or o["owner_guild"]:
                guild = o["owner_guild"]
                self.object_owner.append((o["owner_npc"], names.get(guild, -2) if guild else None))
            else:
                self.object_owner.append(room_owner(o["room"]))
        self.point_owner = [room_owner(p["room"]) for p in world["waypoints"]]

    def access_ok(self, npc: int, owner: tuple[str | None, int | None]) -> bool:
        owner_npc, owner_guild = owner
        if owner_npc is None and owner_guild is None:
            return True
        if owner_npc == self.npcs[npc]["id"]:
            return True
        return owner_guild is not None and owner_guild >= 0 and owner_guild == self.npcs[npc]["guild"]

    def fills(self, state: str, need: str) -> bool:
        return self.states.get(state, {}).get("fill_hours", {}).get(need, 0) > 0

    def anchor(self, state: str) -> bool:
        return self.states.get(state, {}).get("kind") == "anchor"


# --- Measures --------------------------------------------------------------------------------------------------


def by_npc(records: list) -> dict[int, list]:
    out: dict[int, list] = defaultdict(list)
    for r in records:
        out[r[1]].append(r)
    return out


def critical_hours(journal: dict, people: set[int], days: int, unused: set | None = None) -> dict:
    """Seconds in critical by person, day and need, from need_threshold; the needs not in use (`unused`: (npc, need
    index)) are not counted."""
    unused = unused or set()
    needs = journal["needs"]
    start = journal["start"]
    zone = {i: [0] * len(needs) for i in people}
    since = {i: [start] * len(needs) for i in people}
    out = {i: [[0.0] * len(needs) for _ in range(days)] for i in people}

    def spend(i: int, k: int, t0: int, t1: int) -> None:
        while t0 < t1:
            day = (t0 - start) // DAY
            if day >= days:
                return
            end = min(t1, start + (day + 1) * DAY)
            out[i][day][k] += end - t0
            t0 = end

    for t, npc, kind, reason, _e, a, b, _c in journal["records"]:
        if kind != NEED_THRESHOLD or npc not in people:
            continue
        before = zone[npc][a]
        after = before + 1 if reason == DOWN else before - 1
        if before == CRITICAL:
            spend(npc, a, since[npc][a], t)
        zone[npc][a] = after
        since[npc][a] = t
    end = start + days * DAY
    for i in people:
        for k in range(len(needs)):
            if zone[i][k] == CRITICAL:
                spend(i, k, since[i][k], end)
    per_need = {}
    for k, need in enumerate(needs):
        using = [i for i in people if (i, k) not in unused]
        values = sorted(out[i][d][k] / HOUR for i in using for d in range(days))
        worst = max(using, key=lambda i: max(out[i][d][k] for d in range(days))) if using else None
        per_need[need] = {
            "median_h": round(statistics.median(values), 3) if values else 0.0,
            "p90_h": round(values[int(len(values) * 0.9)] if values else 0.0, 3),
            "max_h": round(values[-1] if values else 0.0, 3),
            "person_days_over_4h": sum(1 for v in values if v > 4),
            "person_days": len(values),
            "worst": worst,
        }
    return per_need


def check_choices(journal: dict, rules: Rules) -> dict:
    """Every choice of a policy against the masks' rules read anew; the holders of each place — the routine's users
    (place_held, place_released) and chosen activities with their reservations — against its capacity; the whole walk
    to a chosen place (the rest of a walk under way included) against the travel limit."""
    states = journal["states"]
    needs = journal["needs"]
    world = rules.world
    zone = defaultdict(lambda: [0] * len(needs))
    entry_state: dict[int, str] = {}
    held: dict[tuple, Counter] = defaultdict(Counter)  # place -> NPC -> ways it holds it (chosen, routine)
    chosen_of: dict[int, set] = defaultdict(set)  # NPC -> its chosen places (old and new while switching)
    walking: dict[int, tuple] = {}  # NPC -> (arrival, end) of its walk under way
    standing: dict[int, int] = {}  # NPC -> the point it stands at (or its walk under way began at)
    peak: dict[tuple, int] = {}
    over = []
    bad = Counter()
    examples = []
    chosen = Counter()
    failed = Counter()
    finished = Counter()
    routine_holds = 0
    rechecks = 0
    records = journal["records"]
    def target_of(b: int, c: int) -> tuple:
        return ("object", c) if c != NO_OBJECT else ("point", b)

    def add(npc: int, target: tuple, cap: int, t: int) -> None:
        held[target][npc] += 1
        n = sum(1 for v in held[target].values() if v > 0)
        peak[target] = max(peak.get(target, 0), n)
        if n > cap:
            over.append({"t": t, "target": target, "holders": n, "capacity": cap, "npc": rules.npcs[npc]["key"]})

    def remove(npc: int, target: tuple) -> None:
        if held[target][npc] > 0:
            held[target][npc] -= 1

    def whole_walk(npc: int, t: int, point: int) -> float:
        """Game seconds from a choice at `t` until the NPC stands at the chosen place, as the menu counts it: a walk
        under way goes on to its end, then the walk from there to the place (from the export, Walks)."""
        if npc in walking and walking[npc][0] > t:
            arrive, end = walking[npc]
            return arrive - t + rules.walks.seconds(npc, end, point)
        return rules.walks.seconds(npc, standing[npc], point)

    for idx, (t, npc, kind, reason, _e, a, b, c) in enumerate(records):
        if kind == NEED_THRESHOLD:
            zone[npc][a] += 1 if reason == DOWN else -1
        elif kind == ROUTINE_ENTRY:
            entry_state[npc] = states[a]
        elif kind == TRAVEL_STARTED:
            walking[npc] = (c, b)
        elif kind == ARRIVED:
            walking.pop(npc, None)
            standing[npc] = a
        elif kind == TELEPORTED:
            walking.pop(npc, None)
            standing[npc] = b
        elif kind == DECISION_REQUESTED:
            rechecks += 1 if reason & RECHECK else 0
        elif kind == PLACE_HELD:
            routine_holds += 1
            add(npc, target_of(b, c), rules.kinds[a]["capacity"], t)
        elif kind == PLACE_RELEASED:
            remove(npc, target_of(b, c))
        elif kind == ACTION_CHOSEN:
            if a == NO_KIND:
                chosen["routine"] += 1
                continue
            k = rules.kinds[a]
            chosen[k["kind"]] += 1
            target = target_of(b, c)
            # The NPC's next records of this second: a failure at once (taken), or the walk to it.
            after = [r for r in records[idx + 1 : idx + 12] if r[1] == npc and r[0] == t]
            taken = bool(after) and after[0][2] == ACTION_FAILED and after[0][3] == TAKEN
            problems = []
            if npc not in rules.decider:
                problems.append("not a decider")
            if reason == 0:
                problems.append("the core chose a non-routine option itself")
            owner = rules.object_owner[c] if c != NO_OBJECT else rules.point_owner[b]
            at = k.get("at")
            if at in ("outdoors", "places"):
                pass
            elif at == "own_rooms":
                if owner == (None, None) or not rules.access_ok(npc, owner):
                    problems.append("not its own or its guild's room")
            elif not rules.access_ok(npc, owner):
                problems.append("someone else's")
            if k.get("guilds") and rules.npcs[npc]["guild"] not in k["guilds"]:
                problems.append("guild may not")
            state = entry_state.get(npc, "")
            if rules.post(state) and not rules.release:
                problems.append("on a post")
            elif rules.anchor(state):
                crit = [needs[n] for n, z in enumerate(zone[npc]) if z >= CRITICAL]
                if not any(rules.fills(k["state"], need) for need in crit):
                    problems.append("anchor entry without a critical need it fills")
            if c != NO_OBJECT and world["objects"][c]["waypoint"] != b:
                problems.append("not at its object's point")
            if not taken and target not in chosen_of[npc] and whole_walk(npc, t, b) > rules.limit:
                problems.append("walk over the travel limit")
            if problems:
                for p in problems:
                    bad[p] += 1
                if len(examples) < 20:
                    examples.append({"t": t, "npc": rules.npcs[npc]["key"], "kind": k["kind"], "problems": problems})
            if taken or target in chosen_of[npc]:
                continue
            chosen_of[npc].add(target)
            add(npc, target, k["capacity"], t)
        elif kind == ACTION_FAILED:
            failed[reason] += 1
            if reason in (INTERRUPTED, EVICTED) and target_of(b, c) in chosen_of[npc]:
                chosen_of[npc].discard(target_of(b, c))
                remove(npc, target_of(b, c))
        elif kind == ACTIVITY_FINISHED:
            finished[reason] += 1
            if target_of(b, c) in chosen_of[npc]:
                chosen_of[npc].discard(target_of(b, c))
                remove(npc, target_of(b, c))
    return {
        "choices": dict(chosen),
        "routine_holds": routine_holds,
        "rechecks": rechecks,
        "out_of_mask": sum(bad.values()),
        "out_of_mask_by_rule": dict(bad),
        "out_of_mask_examples": examples,
        "over_capacity": len(over),
        "over_capacity_examples": over[:20],
        "most_holders": max(peak.values()) if peak else 0,
        "failed": {{TAKEN: "taken", INTERRUPTED: "interrupted", EVICTED: "evicted"}[r]: n for r, n in failed.items()},
        "finished": {FINISHED[r]: n for r, n in finished.items()},
    }


def anchor_time(journal: dict, rules: Rules, people: set[int], days: int) -> dict:
    """Anchor seconds (the entry in force is anchor) and of them by the routine (no chosen activity); apart for the
    people whose routine leaves them under 2 free hours a day."""
    out = anchor_share(journal, rules, people, days)
    little = {i for i in people if rules.free_h.get(i, 24) < 2}
    out["people_under_2_free_h"] = len(little)
    out["by_routine_under_2_free_h"] = anchor_share(journal, rules, little, days)["by_routine"]
    out["by_routine_2_free_h_or_more"] = anchor_share(journal, rules, people - little, days)["by_routine"]
    return out


def anchor_share(journal: dict, rules: Rules, people: set[int], days: int) -> dict:
    states = journal["states"]
    start = journal["start"]
    end = start + days * DAY
    total = by_routine = posts = posts_routine = 0
    per = by_npc(journal["records"])
    for i in people:
        entry = None
        chosen = False
        t0 = start
        for t, _n, kind, reason, _e, a, _b, _c in per.get(i, []) + [(end, i, 0, 0, 0, 0, 0, 0)]:
            if t > t0 and entry is not None and rules.anchor(entry):
                dt = t - t0
                total += dt
                by_routine += 0 if chosen else dt
                if rules.post(entry):
                    posts += dt
                    posts_routine += 0 if chosen else dt
            t0 = max(t0, t)
            if kind == ROUTINE_ENTRY:
                entry = states[a]
            elif kind == ACTIVITY_STARTED:
                chosen = reason == CHOSEN
    return {
        "anchor_h": round(total / HOUR, 1),
        "by_routine": round(by_routine / total, 5) if total else 1.0,
        "posts_h": round(posts / HOUR, 1),
        "without_posts_by_routine": round((by_routine - posts_routine) / (total - posts), 5) if total > posts else 1.0,
    }


def changes(journal: dict, people: set[int], days: int) -> dict:
    """Activity changes (activity_started but the follow-on into bed) per NPC and game hour."""
    start = journal["start"]
    hours = days * 24
    cells = Counter()
    for t, npc, kind, reason, _e, _a, _b, _c in journal["records"]:
        if kind == ACTIVITY_STARTED and npc in people and reason != FOLLOW_ON and t > start:
            h = (t - start) // HOUR
            if h < hours:
                cells[(npc, h)] += 1
    values = sorted(cells.get((i, h), 0) for i in people for h in range(hours))
    if not values:
        return {"p95": 0, "max": 0, "mean": 0.0}
    return {
        "p95": values[int(len(values) * 0.95)],
        "p99": values[int(len(values) * 0.99)],
        "max": values[-1],
        "mean": round(sum(values) / len(values), 3),
    }


def time_on(journal: dict, rules: Rules, people: set[int], until: int, need: str) -> float:
    """Hours the people spend standing in states that fill `need` until `until` seconds from the start."""
    states = journal["states"]
    start = journal["start"]
    end = start + until
    total = 0
    per = by_npc(journal["records"])
    for i in people:
        state, standing, t0 = None, True, start
        for t, _n, kind, _r, _e, a, _b, _c in per.get(i, []) + [(end, i, 0, 0, 0, 0, 0, 0)]:
            t = min(t, end)
            if t > t0 and state is not None and standing and rules.fills(state, need):
                total += t - t0
            t0 = max(t0, t)
            if kind == ACTIVITY_STARTED:
                state = states[a]
            elif kind == TRAVEL_STARTED:
                standing = False
            elif kind in (ARRIVED, TELEPORTED):
                standing = True
            if t >= end:
                break
    return total / HOUR


def chosen_time(journal: dict, rules: Rules, need: str) -> dict[int, float]:
    """Hours each NPC spends standing in chosen activities whose state fills `need`."""
    states = journal["states"]
    out: Counter = Counter()
    per = by_npc(journal["records"])
    end = journal["start"] + 7 * DAY
    for i, records in per.items():
        state, chosen, standing, t0 = None, False, True, journal["start"]
        for t, _n, kind, reason, _e, a, _b, _c in records + [(end, i, 0, 0, 0, 0, 0, 0)]:
            if t > t0 and chosen and standing and state and rules.fills(state, need):
                out[i] += t - t0
            t0 = max(t0, t)
            if kind == ACTIVITY_STARTED:
                state, chosen = states[a], reason == CHOSEN
            elif kind == TRAVEL_STARTED:
                standing = False
            elif kind in (ARRIVED, TELEPORTED):
                standing = True
    return {i: v / HOUR for i, v in out.items()}


def protected_marks(routine_marks: Path, teacher_marks: Path, rules: Rules, needs: int) -> dict:
    """The protected NPCs' mark rows of both runs without the decisions' columns."""
    keys = {rules.npcs[i]["key"] for i in rules.protected}

    def rows(path: Path, cut: int) -> list[str]:
        out = []
        with path.open() as f:
            next(f)
            for line in f:
                parts = line.rstrip("\n").split(",")
                if parts[1] in keys:
                    out.append(",".join(parts[: len(parts) - cut]))
        return out

    a = rows(routine_marks, 0)
    b = rows(teacher_marks, 2)
    same = sum(1 for x, y in zip(a, b) if x == y)
    return {
        "rows": len(a),
        "same": same,
        "share": round(same / len(a), 6) if a else 1.0,
        "rows_match": len(a) == len(b),
        "npcs": len(keys),
    }


def traits(teacher: dict, plain: dict, rules: Rules, cards: dict) -> dict:
    """The talkative choose more social time with their cards than without; same-guild pairs differ the same way."""
    with_cards = chosen_time(teacher, rules, SOCIAL)
    without = chosen_time(plain, rules, SOCIAL)
    agree = total = 0
    rows = []
    for i in sorted(rules.decider):
        card = cards.get(rules.npcs[i]["id"])
        if not card or card.get("talk", 3) == 3:
            continue
        talk = card["talk"]
        delta = with_cards.get(i, 0.0) - without.get(i, 0.0)
        if delta == 0:
            continue
        total += 1
        agree += 1 if (delta > 0) == (talk > 3) else 0
        rows.append((rules.npcs[i]["key"], talk, round(with_cards.get(i, 0.0), 2), round(without.get(i, 0.0), 2)))
    # Same-guild pairs of deciders with cards that differ in talk by 2 or more.
    by_guild = defaultdict(list)
    for i in rules.decider:
        card = cards.get(rules.npcs[i]["id"])
        if card:
            by_guild[rules.npcs[i]["guild"]].append(i)
    pairs = pair_agree = 0
    for members in by_guild.values():
        for x in range(len(members)):
            for y in range(x + 1, len(members)):
                a, b = members[x], members[y]
                ta, tb = cards[rules.npcs[a]["id"]]["talk"], cards[rules.npcs[b]["id"]]["talk"]
                if abs(ta - tb) < 2:
                    continue
                # Each one's change by its card, so the routine's share cancels.
                da = with_cards.get(a, 0.0) - without.get(a, 0.0)
                db = with_cards.get(b, 0.0) - without.get(b, 0.0)
                if da == db:
                    continue
                pairs += 1
                pair_agree += 1 if (da > db) == (ta > tb) else 0
    return {
        "people_with_talk_off_middle": total,
        "agree": agree,
        "agree_share": round(agree / total, 3) if total else 0.0,
        "pairs": pairs,
        "pairs_agree_share": round(pair_agree / pairs, 3) if pairs else 0.0,
        "examples": rows[:12],
    }


# --- The report ------------------------------------------------------------------------------------------------


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--world", type=Path, required=True)
    ap.add_argument("--config", type=Path, required=True)
    ap.add_argument("--tool", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--work", type=Path)
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()

    world = json.loads(args.world.read_text())
    config_path = args.config.resolve()
    config = json.loads(config_path.read_text())
    rules = Rules(world, config, config_path)
    cards_path = config["traits"].get("cards")
    cards = {}
    if cards_path:
        p = Path(cards_path)
        cards = json.loads((p if p.is_absolute() else config_path.parent / p).read_text())["cards"]
    people = rules.decider
    work = args.work or Path(tempfile.mkdtemp(prefix="society-day-report-"))
    work = work.resolve()

    def variant(name: str, change) -> Path:
        """The config with one change, its relative paths made absolute."""
        c = json.loads(config_path.read_text())
        for section, key in (("traits", "cards"), ("decisions", "protected"), ("decisions", "places")):
            if c[section].get(key):
                p = Path(c[section][key])
                c[section][key] = str(p if p.is_absolute() else (config_path.parent / p).resolve())
        change(c)
        path = work / f"config-{name}.json"
        path.write_text(json.dumps(c, ensure_ascii=False))
        return path

    def run(name: str, policy: str, days: int, config: Path = config_path) -> dict:
        r = run_tool(args.tool, args.world, config, work / name, days, args.seed, policy)
        r["journal"] = read_journal(work / name / "society-day.journal")
        return r

    print("runs in", work, file=sys.stderr)
    routine1, routine7 = run("routine-1", "routine", 1), run("routine-7", "routine", 7)
    teacher1, teacher7 = run("teacher-1", "teacher", 1), run("teacher-7", "teacher", 7)
    random7 = run("random-7", "random", 7)

    report: dict = {
        "world": args.world.name,
        "world_hash": teacher1["journal"]["world_hash"],
        "config_hash": teacher1["journal"]["config_hash"],
        "seed": args.seed,
        "thresholds": THRESHOLDS,
        "people": {
            "deciders": len(people),
            "on_post_all_day": len(rules.on_post_all_day),
            "protected": len(rules.protected),
        },
        "runs": {},
    }
    needs = teacher1["journal"]["needs"]
    unmet = read_unmet(teacher1["dir"] / "society-needs-unmet.csv", rules, needs)
    unused = {(u["npc"], u["k"]) for u in unmet}
    report["unmet"] = [{k: v for k, v in u.items() if k not in ("npc", "k")} for u in unmet]
    report["people"]["with_needs"] = sum(
        1 for n in rules.npcs if n["person"] and n["guild"] not in set(rules.dec["no_needs_guilds"])
    )
    report["people"]["deciders_with_a_need_not_in_use"] = len({u["npc"] for u in unmet if u["npc"] in people})
    for name, r, days in (
        ("routine-1", routine1, 1),
        ("routine-7", routine7, 7),
        ("teacher-1", teacher1, 1),
        ("teacher-7", teacher7, 7),
        ("random-7", random7, 7),
    ):
        j = r["journal"]
        entry = {
            "days": days,
            "policy": j["policy"],
            "events": len(j["records"]),
            "decisions": r["decisions"],
            "rows": r["rows"],
            "day_ms": r["day_ms"],
            "critical": critical_hours(j, people, days, unused),
            "critical_on_post_all_day": critical_hours(j, rules.on_post_all_day, days, unused),
            "anchor": anchor_time(j, rules, people, days),
            "changes": changes(j, people, days),
        }
        if j["version"] >= 5:
            entry["choices"] = check_choices(j, rules)
        report["runs"][name] = entry
        print(name, "done", file=sys.stderr)
    for name, routine, teacher in (("teacher-1", routine1, teacher1), ("teacher-7", routine7, teacher7)):
        report["runs"][name]["protected"] = protected_marks(
            routine["dir"] / "society-marks.csv", teacher["dir"] / "society-marks.csv", rules, len(needs)
        )

    # Sensitivity: each need at 0 for everyone at the start, the first day's time in states that fill it.
    sensitivity = {}
    base = {need: time_on(teacher1["journal"], rules, people, DAY, need) for need in needs}
    for k, need in enumerate(needs):

        def zero(c, k=k):
            c["needs"][k]["start"] = 0.0

        r = run(f"zero-{need}", "teacher", 1, variant(f"zero-{need}", zero))
        got = time_on(r["journal"], rules, people, DAY, need)
        sensitivity[need] = {
            "base_h": round(base[need], 1),
            "zero_h": round(got, 1),
            "change": round(got / base[need] - 1, 3) if base[need] else None,
            "grows": got > base[need],
        }
    report["sensitivity"] = sensitivity

    # For the user's decision: guards' posts release for needs (posts_release_for_needs: true), 7 days.
    def release(c):
        c["decisions"]["posts_release_for_needs"] = True

    release_config = variant("posts-release", release)
    released = run("teacher-7-posts-release", "teacher", 7, release_config)
    rules_released = Rules(world, json.loads(release_config.read_text()), release_config)
    guards = rules.on_post_all_day | {i for i in rules.decider if any(rules.post(r["state"]) for r in rules.npcs[i]["routine"])}
    report["posts_release"] = {
        "guards": len(guards),
        "critical_guards_routine_posts": critical_hours(teacher7["journal"], guards, 7, unused),
        "critical_guards_released": critical_hours(released["journal"], guards, 7, unused),
        "anchor_deciders_released": anchor_time(released["journal"], rules_released, rules_released.decider, 7),
    }

    # Characters: the same 7 days without the cards.
    def no_cards(c):
        c["traits"]["cards"] = None

    plain7 = run("teacher-7-no-cards", "teacher", 7, variant("no-cards", no_cards))
    report["traits"] = traits(teacher7["journal"], plain7["journal"], rules, cards)

    # Against the thresholds.
    checks = []

    def check(what: str, value, ok: bool) -> None:
        checks.append({"what": what, "value": value, "ok": ok})

    for name in ("teacher-1", "teacher-7"):
        r = report["runs"][name]
        for need, c in r["critical"].items():
            check(f"{name}: critical {need}, median h", c["median_h"], c["median_h"] <= THRESHOLDS["critical_median_h"])
            check(f"{name}: critical {need}, max h", c["max_h"], c["max_h"] <= THRESHOLDS["critical_max_h"])
        check(f"{name}: out of mask", r["choices"]["out_of_mask"], r["choices"]["out_of_mask"] == 0)
        check(f"{name}: over capacity", r["choices"]["over_capacity"], r["choices"]["over_capacity"] == 0)
        check(f"{name}: protected marks same", r["protected"]["share"], r["protected"]["share"] >= 1.0)
        check(f"{name}: anchor time by routine", r["anchor"]["by_routine"],
              r["anchor"]["by_routine"] >= THRESHOLDS["anchor_by_routine"])
        check(f"{name}: changes an hour, p95", r["changes"]["p95"], r["changes"]["p95"] <= THRESHOLDS["changes_p95"])
        slowest = max(r["day_ms"]) / 1000 if r["day_ms"] else 0
        check(f"{name}: a day, s", round(slowest, 4), slowest <= THRESHOLDS["day_seconds"])
    r = report["runs"]["random-7"]
    check("random-7: out of mask", r["choices"]["out_of_mask"], r["choices"]["out_of_mask"] == 0)
    check("random-7: over capacity", r["choices"]["over_capacity"], r["choices"]["over_capacity"] == 0)
    for need, s in sensitivity.items():
        check(f"sensitivity: {need} grows", s["change"], s["grows"])
    t = report["traits"]
    check("traits: talk moves chosen social time its way", t["agree_share"], t["agree_share"] >= 0.75)
    check("traits: same-guild pairs differ the same way", t["pairs_agree_share"], t["pairs_agree_share"] >= 0.75)
    report["checks"] = checks
    report["passed"] = all(c["ok"] for c in checks)

    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / "day-report.json").write_text(json.dumps(report, ensure_ascii=False, indent=1, default=str) + "\n")
    md = ["# Отчёт суток: ограды", "", "| Проверка | Значение | Итог |", "|---|---|---|"]
    md += [f"| {c['what']} | {c['value']} | {'да' if c['ok'] else '**нет**'} |" for c in checks]
    md += ["", "## Места и концы занятий", "",
           "Держатели места — пользователи распорядка (`place_held`) и выбранные занятия с бронью. Выселен — "
           "выбранное занятие уступило место пришедшему пользователю распорядка. Повторное решение — нужда в "
           "«низко» или «критично» не наполняется тем, что NPC делает, `recheck_min` после прошлого решения.", "",
           "| Прогон | взято распорядком | больше всего держателей | сверх вместимости | выселено в пути / на месте "
           "| конец: уставка / выше «низко» / наибольшее время / другой выбор / якорь | не удалось: занято / якорь "
           "| повторных решений |",
           "|---|---|---|---|---|---|---|---|"]
    for name, r in report["runs"].items():
        if "choices" not in r:
            continue
        ch = r["choices"]
        f, fl = ch["finished"], ch["failed"]
        md.append(f"| {name} | {ch['routine_holds']} | {ch['most_holders']} | {ch['over_capacity']} | "
                  f"{fl.get('evicted', 0)} / {f.get('evicted', 0)} | {f.get('setpoint', 0)} / {f.get('above low', 0)} / "
                  f"{f.get('max time', 0)} / {f.get('switched', 0)} / {f.get('anchor', 0)} | "
                  f"{fl.get('taken', 0)} / {fl.get('interrupted', 0)} | {ch.get('rechecks', 0)} |")
    md += ["", "## Часы в «критично» в сутки у решающих людей (человеко-сутки: медиана, p90, максимум)", ""]
    md += ["| Прогон | " + " | ".join(needs) + " |", "|---" * (len(needs) + 1) + "|"]
    for name, r in report["runs"].items():
        cells = [f"{c['median_h']} / {c['p90_h']} / {c['max_h']}" for c in r["critical"].values()]
        md.append(f"| {name} | " + " | ".join(cells) + " |")
    pr = report["posts_release"]
    for label, key in (("стражи, посты держат", "critical_guards_routine_posts"), ("стражи, посты отпускают", "critical_guards_released")):
        cells = [f"{c['median_h']} / {c['p90_h']} / {c['max_h']}" for c in pr[key].values()]
        md.append(f"| {label} (7 суток, {pr['guards']} чел.) | " + " | ".join(cells) + " |")
    md += ["", "## Якорное время по распорядку (порог ≥ 95% по всем решающим)", "",
           "| Прогон | все | < 2 свободных ч | ≥ 2 свободных ч |", "|---|---|---|---|"]
    rows = [(name, r["anchor"]) for name, r in report["runs"].items() if r["policy"] != "routine"]
    rows.append(("teacher-7, посты отпускают (для решения пользователя)", pr["anchor_deciders_released"]))
    for name, a in rows:
        md.append(f"| {name} | {a['by_routine']:.1%} | {a['by_routine_under_2_free_h']:.1%} "
                  f"({a['people_under_2_free_h']} чел.) | {a['by_routine_2_free_h_or_more']:.1%} |")
    md += ["", f"## Нечем утолить: {len(unmet)} нужд без способа в пределах пути от точек распорядка", "",
           "Пробелы контента: такая нужда не убывает и в ограды не входит.", "",
           "| NPC | гильдия | нужда | ближайший способ | место | пути, мин |", "|---|---|---|---|---|---|"]
    for u in unmet:
        md.append(f"| {u['key']} | {u['guild']} | {u['need']} | {u['kind'] or '—'} | {u['place'] or '—'} | "
                  f"{u['travel_min'] if u['travel_min'] is not None else '—'} |")
    (args.out / "day-report.md").write_text("\n".join(md) + "\n")
    print(f"{sum(c['ok'] for c in checks)} of {len(checks)} checks pass; {args.out}/day-report.json", file=sys.stderr)
    return 0 if report["passed"] or not args.check else 1


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""The candidate signal of a bump (step 2), read from the controller's move records of a bench run.

    python3 reports/physics/2026-11-15-run-bumps/bump_signal.py <bench out dir> [start cm/s, 120]

The run needs `npccharacter trace player` (was_bench.py has it). Per recorded take it prints every start of a loss of
forward speed and the contact that explains it: a neighbour (the ellipsoid clip), a wall, a movable body (dyn) or
nothing (?), the angle between the move and the contact normal, whether the tick climbed a stair step.

lost = |requested| - accepted along the request, per second. A start is a frame where `lost` rises over START after
PAUSE ms without one."""
import json, sys, math
START, PAUSE = float(sys.argv[2]) if len(sys.argv) > 2 else 120.0, 500
run = sys.argv[1]
segs = {}
for line in open(f"{run}/events.jsonl"):
    if '"npc_character_motion_trace"' in line:
        e = json.loads(line)
        if "npc_move" in e:
            segs.setdefault(e.get("seg") or "-", []).append(e)

def hnorm(v):
    l = math.hypot(v[0], v[2])
    return (v[0] / l, v[2] / l) if l > 1e-6 else None

tot = {}
for seg, es in segs.items():
    if seg == "-":
        continue
    last_start = -10**9
    starts = []
    prev_lost = 0.0
    hist = []
    for e in es:
        m = e["npc_move"]; dt = max(m["dt_ms"], 1) / 1000
        req = (m["requested_cm"][0], m["requested_cm"][2]); rl = math.hypot(*req)
        if rl < 1e-3:
            prev_lost = 0.0; hist.append(0); continue
        d = (req[0] / rl, req[1] / rl)
        acc = (m["accepted_cm"][0] - m["initial_cm"][0], m["accepted_cm"][2] - m["initial_cm"][2])
        lost = (rl - (acc[0] * d[0] + acc[1] * d[1])) / dt
        hist.append(lost)
        if lost > START and prev_lost <= START and e["t"] - last_start >= PAUSE:
            # what explains it: the neighbour's clip, else the non-floor contact most against the request
            col = m["request_collision"] if m["request_collision"]["npc"] != "none" else m["resolved_collision"]
            kind, h, what, cos = "?", None, "", 0.0
            if col["npc"] != "none":
                kind, what = "npc", col["npc"]
            else:
                lastk = [k for k in e if k.startswith("after")]
                for c in (e[lastk[-1]]["contacts"] if lastk else []):
                    sn = c["surface_normal"]
                    n = hnorm(sn)
                    if abs(sn[1]) >= 0.5 or not c["had_collision"] or not n:
                        continue
                    k = -(d[0] * n[0] + d[1] * n[1])
                    if k > cos:
                        b = c["body"]; cos = k
                        kind = "dyn" if b.get("motion_type") == 2 else "wall"
                        h = round(c["position_cm"][1] - e["root_cm"][1]); what = b.get("visual") or b.get("class")
            starts.append((e["t"], round(lost), kind, h, round(rl / dt), bool(e.get("stair_succeeded")), e["animations"], what, round(math.degrees(math.acos(min(cos, 1)))) if cos else None))
            last_start = e["t"]
        prev_lost = lost
    print(f"== {seg}: starts {len(starts)}")
    for s in starts:
        print(f"   t={s[0]} lost={s[1]} of {s[4]} {s[2]} h={s[3]} angle={s[8]} stair={int(s[5])} {s[6]} {s[7]}")

#!/usr/bin/env python3
"""Generate the «как сейчас» scenario of step 2 (run bumps): the hero runs into a house wall, a person and a table.

    python3 reports/physics/2026-11-15-run-bumps/was_bench.py <out.bench>
    WORLD=NEWWORLD.ZEN BENCH_HOST=ryzen scripts/bench.sh <out.bench> <abs out dir>
    python3 reports/physics/2026-11-15-run-bumps/bump_signal.py <abs out dir>

`npccharacter trace player` writes every move of the hero's controller to events.jsonl; bump_signal.py reads the
candidate signal of a bump from it. The wall is the long house wall of the harbour yard (z -3746, normal about -z).
"""
import sys
NPC = "SLD_815_SOELDNER"
YARD = "goto NW_CITY_HABOUR_POOR_AREA_PATH_08_A"   # the yard of the harbour quarter; the long house wall is at z -3746

def wall(take, start, yaw, mode="run", cam=90, slow=False, ms=2600):
    return f"""
release all
clear 3000
{YARD}
wait 400
clear 3000
goto pos {start[0]} -100 {start[1]}
wait 700
face {yaw}
walkmode {mode}
watch player
cam follow 430 110 {cam}
wait 1200
{"slowmo 3" if slow else "slowmo 1"}
rec {take}
wait 300
hold forward {ms}
wait {ms + 900}
pos
stop
slowmo 1
"""

def person(take, mode, dist, ms, slow=False):
    return f"""
release all
clear 3000
goto NW_CITY_MERCHANT_TRADE_04
wait 500
clear 3000
face wp NW_CITY_MERCHANT_PATH_09
arc {NPC} 1 {dist} 0
freeze {NPC}
walkmode {mode}
watch player
cam follow 430 110 270
wait 1200
{"slowmo 3" if slow else "slowmo 1"}
rec {take}
wait 300
hold forward {ms}
wait {ms + 900}
stop
slowmo 1
walkmode run
clear 3000
"""

text = """# Step 2, «как сейчас»: a run into a wall, a person, a table.
# @timeout 1500
seed 92202
cheat god
set time 12 0
movable on
overlay off
npccharacter trace player
"""
text += wall("wall_run", (3125, -4300), 90)
text += wall("wall_run_back", (3125, -4300), 90, cam=150)
text += wall("wall_run_slow", (3125, -4300), 90, slow=True)
text += wall("wall_walk", (3120, -4000), 90, mode="walk", ms=3000)
text += wall("wall_run_45", (2775, -4126), 45)
text += wall("wall_run_20", (2500, -3990), 20, ms=3200)
text += person("npc_run", "run", 700, 2600)
text += person("npc_run_slow", "run", 700, 2600, slow=True)
text += person("npc_walk", "walk", 350, 3200)
text += """
release all
character prop table
walkmode run
watch player
cam follow 380 120 120
wait 900
rec table_run
hold forward 3000
wait 3600
stop
character cleanup
"""
open(sys.argv[1], "w").write(text + "\nquit\n")
print(text.count("\nrec "), "takes")

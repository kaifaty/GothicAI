#!/usr/bin/env python3
"""Generate the acceptance scenario of step 2 (run bumps): the hero runs into a house wall, a person and a table.

    python3 reports/physics/2026-11-15-run-bumps/bump_bench.py <out.bench>
    WORLD=NEWWORLD.ZEN BUMP_REACTIONS=1 BENCH_HOST=ryzen scripts/bench.sh <out.bench> <abs out dir>   # and =0 for «было»
    python3 reports/physics/2026-11-15-run-bumps/bump_look.py <abs out dir>

The same takes as was_bench.py, filmed for the eye: the wall in the morning light from behind the left shoulder,
the person and the table from the side. Takes with `_slow` run three times slower.
"""
import sys
NPC = "SLD_815_SOELDNER"
YARD = "goto NW_CITY_HABOUR_POOR_AREA_PATH_08_A"   # the yard of the harbour quarter; the long house wall is at z -3746

def wall(take, start, yaw, mode="run", cam=240, slow=False, ms=2600):
    return f"""
release all
clear 3000
set time 9 0
{YARD}
wait 400
clear 3000
goto pos {start[0]} -100 {start[1]}
wait 700
face {yaw}
walkmode {mode}
watch player
cam follow 300 120 {cam}
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

def person(take, mode, dist, ms, slow=False, watch="player"):
    return f"""
release all
clear 3000
set time 12 0
goto NW_CITY_MERCHANT_TRADE_04
wait 500
clear 3000
face wp NW_CITY_MERCHANT_PATH_09
arc {NPC} 1 {dist} 0
freeze {NPC}
walkmode {mode}
watch {watch}
cam follow 330 120 270
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

def table(take, cam, slow=False):
    return f"""
release all
clear 3000
goto pos 8470 128 1080
wait 700
face 90
walkmode run
watch player
cam follow 330 120 {cam}
wait 900
{"slowmo 3" if slow else "slowmo 1"}
rec {take}
wait 300
hold forward 1800
wait 2700
pos
stop
slowmo 1
"""

text = """# Step 2, acceptance: a run into a wall, a person, a table.
# @timeout 1800
seed 92202
cheat god
set time 12 0
movable on
overlay off
npccharacter trace player
"""
text += wall("wall_run", (3125, -4300), 90)
text += wall("wall_run_slow", (3125, -4300), 90, slow=True)
text += wall("wall_walk", (3120, -4000), 90, mode="walk", ms=3000)
text += wall("wall_run_45a", (2950, -4130), 45)
text += wall("wall_run_45b", (3500, -4130), 135, cam=120)
text += wall("wall_run_20", (2500, -3990), 20, ms=3200)
text += person("npc_run", "run", 700, 2600)
text += person("npc_run_slow", "run", 700, 2600, slow=True)
text += person("npc_run_other", "run", 700, 2600, watch=NPC)
text += person("npc_walk", "walk", 350, 3200)
text += """
release all
character prop table
"""
text += table("table_run", 90)
text += table("table_run_slow", 90, slow=True)
text += table("table_run_c270", 270)
text += """
character cleanup
"""
open(sys.argv[1], "w").write(text + "\nquit\n")
print(text.count("\nrec "), "takes")

#!/usr/bin/env python3
"""Generate the acceptance scenario of step 3 (hands to the wall, the stand instead of the run in place).

    python3 reports/physics/2026-11-15-reach-wall/reach_bench.py <out.bench> [<take> ...]
    WORLD=NEWWORLD.ZEN WALL_REACH=1 BENCH_HOST=ryzen scripts/bench.sh <out.bench> <abs out dir>   # and =0 for «было»
    python3 reports/physics/2026-11-15-reach-wall/reach_look.py <abs out dir>

The wall and the table are the spots of step 2's scenario. The wall takes are filmed from behind the shoulder, `_side`
by a fixed camera along the wall, `_back` from straight behind, as the player sees the hero; `_slow` run three
times slower. In `wall_run_13` the hero meets the wall 13 degrees off and slides along it. In `wall_turn`
the hero, pressed into the wall, turns round with the key held and runs away; in `wall_sword` the right hand holds
a sword and only the left one goes to the wall.
"""
import sys
NPC = "SLD_815_SOELDNER"
YARD = "goto NW_CITY_HABOUR_POOR_AREA_PATH_08_A"   # the yard of the harbour quarter; the long house wall is at z -3746


def wall(take, start, yaw, mode="run", cam="follow 300 120 240", slow=False, ms=2600, before="", during=""):
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
{before}
watch player
cam {cam}
wait 1200
{"slowmo 3" if slow else "slowmo 1"}
rec {take}
wait 300
hold forward {ms}
{during if during else f"wait {ms + 900}"}
pos
stop
slowmo 1
"""


def person(take, slow=False):
    return f"""
release all
clear 3000
set time 12 0
goto NW_CITY_MERCHANT_TRADE_04
wait 500
clear 3000
face wp NW_CITY_MERCHANT_PATH_09
arc {NPC} 1 700 0
freeze {NPC}
walkmode run
watch player
cam follow 330 120 270
wait 1200
{"slowmo 3" if slow else "slowmo 1"}
rec {take}
wait 300
hold forward 2600
wait 3500
stop
slowmo 1
clear 3000
"""


def table(take, slow=False):
    # the stand is inside a tavern: a following camera runs into a post, so it is fixed. A take pushes the table,
    # it is put anew for every one
    return f"""
release all
character prop table
clear 3000
goto pos 8470 128 1080
wait 700
face 90
walkmode run
watch player
cam at 8770 260 1230 8470 215 1270
wait 900
{"slowmo 3" if slow else "slowmo 1"}
rec {take}
wait 300
hold forward 1800
wait 2700
pos
stop
slowmo 1
character cleanup
"""


# The wall stands 13 degrees off the world's axes: facing 76 the hero meets it head on and stops dead, facing 90 he
# meets it 13 degrees off and slides along it at 0.75 m/s with the key held.
HEAD_ON = ((3250, -4300), 76)
SIDE = "at 3581 24 -3895 3372 8 -3785"   # fixed, along the wall: the arms between the body and the wall
BACK = "follow 320 140 180"               # as the player sees the hero
HAND = "at 3275 33 -3807 3376 28 -3771"   # fixed, a metre from where the palms come to stand
text = """# Step 3, acceptance: the hands to the wall, the stand of a body pressed into it.
# @timeout 1800
seed 92203
cheat god
set time 12 0
movable on
overlay off
npccharacter trace player
"""
TAKES = {
    "wall_run": lambda t: wall(t, *HEAD_ON),
    "wall_run_side": lambda t: wall(t, *HEAD_ON, cam=SIDE),
    "wall_run_back": lambda t: wall(t, *HEAD_ON, cam=BACK),
    "wall_run_slow": lambda t: wall(t, *HEAD_ON, cam=SIDE, slow=True),
    "wall_run_back_slow": lambda t: wall(t, *HEAD_ON, cam=BACK, slow=True),
    "wall_run_13": lambda t: wall(t, (3250, -4300), 90, cam=BACK),
    "wall_walk": lambda t: wall(t, (3320, -3994), 76, mode="walk", ms=3000),
    "wall_run_45": lambda t: wall(t, (3600, -4186), 121, cam=BACK),
    "wall_run_hand": lambda t: wall(t, *HEAD_ON, cam=HAND),
    # along the wall, 70 cm off it: no hands
    "wall_along": lambda t: wall(t, (3067, -3773), 346, cam=BACK, ms=1800),
    # pressed, the key held: turn round and run away; let the key go
    "wall_turn": lambda t: wall(t, *HEAD_ON, ms=4200, during="wait 2600\nturn 180\nwait 2500"),
    "wall_release": lambda t: wall(t, *HEAD_ON, ms=2200, during="wait 4200"),
    "wall_sword": lambda t: wall(t, *HEAD_ON, before="equip ItMw_1h_Vlk_Sword\ntap weapon\nwait 1500",
                                 during="wait 3500\ntap weapon\nwait 1500"),
    "npc_run": person,
    "table_run": table,
    "table_run_slow": lambda t: table(t, slow=True),
}
# the takes named after the file, or all of them
for take in sys.argv[2:] or TAKES:
    text += TAKES[take](take)
open(sys.argv[1], "w").write(text + "\nquit\n")
print(text.count("\nrec "), "takes")

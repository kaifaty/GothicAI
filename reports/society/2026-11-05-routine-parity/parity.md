# Society core vs engine day: routine parity

- world: `world.json`
- core: `core`; engine: `ref/day-headless-full` (tier `full`, seed 73106, 1440 min every 5)
- people: 370 (missing in a run: 0), marks: 289; place radius 10 m

| Check | Value | Limit | |
|---|---|---|---|
| person-marks of the day missing in a run | 0 | <= 0 | pass |
| same ZS_* outside the windows | 99.76% | >= 95.00% | pass |
| within 10 m outside the windows, without self-moving states | 99.77% | >= 90.00% | pass |
| travel time error on transitions, median \|core − engine\|, game min | 0.37 | <= 2.00 | pass |

| Marks | Count | Same ZS_* | Within 10 m (count) |
|---|---|---|---|
| outside the windows | 104178 | 99.76% | 99.77% (94706) |
| inside the windows | 2752 | 92.33% | 89.43% (2752) |
| of them in open windows | 1248 | 89.34% | 85.42% |
| outside, self-moving states included | 104178 | 99.76% | 99.79% |
| self-moving `ZS_MM_RTN_ROAM`, outside | 5202 | 100.00% | 100.00% — not in the place check: roams between FP_ROAM points |
| self-moving `ZS_PICK_FP`, outside | 2015 | 100.00% | 100.00% — not in the place check: picks from free point to free point (AI_GotoNextFP) |
| self-moving `ZS_PRAY_INNOS_FP`, outside | 565 | 100.00% | 100.00% — not in the place check: goes between prayer free points (AI_GotoNextFP) |
| self-moving `ZS_RAKE_FP`, outside | 1500 | 99.93% | 100.00% — not in the place check: rakes from free point to free point (AI_GotoNextFP) |
| self-moving `ZS_SWEEP_FP`, outside | 190 | 100.00% | 100.00% — not in the place check: sweeps from free point to free point (AI_GotoNextFP) |

Travel time on transitions: 531 (the core never came within 10 m: 0); |core − engine| median 0.37 min, p90 2.17 min; signed median -0.25 min; engine's median travel 7.05 min.

## NPCs that differ most (outside the windows)

| NPC | Marks | ZS differs | Farther than 10 m (median, m) | Most common engine / core ZS |
|---|---|---|---|---|
| PC_PSIONIC#0 | 180 | 155 | 155 (19.9) | ZS_TALK / ZS_SIT_CAMPFIRE ×150; ZS_ATTACK / ZS_SIT_CAMPFIRE ×5 |
| VLK_487_BUERGERIN#0 | 288 | 0 | 30 (10.6) | — |
| PAL_228_SCHIFFSWACHE#0 | 131 | 2 | 24 (15.3) | - / ZS_STAND_GUARDING ×2 |
| MIL_314_MORTIS#0 | 284 | 8 | 0 (—) | - / ZS_SMITH_ANVIL ×3; - / ZS_SMITH_FIRE ×3; - / ZS_SMITH_COOL ×1 |
| VLK_461_CARL#0 | 284 | 7 | 0 (—) | - / ZS_SMITH_ANVIL ×4; - / ZS_SMITH_FIRE ×2; - / ZS_SMITH_COOL ×1 |
| BAU_900_ONAR#0 | 289 | 2 | 4 (16.3) | ZS_GOTOBED / ZS_SLEEP ×2 |
| PAL_227_SCHIFFSWACHE#0 | 117 | 5 | 0 (—) | - / ZS_STAND_GUARDING ×5 |
| VLK_412_HARAD#0 | 288 | 5 | 0 (—) | - / ZS_SMITH_ANVIL ×5 |
| PAL_207_GIRION#0 | 285 | 3 | 0 (—) | - / ZS_STAND_ARMSCROSSED ×2; ZS_GOTOBED / ZS_SLEEP ×1 |
| SLD_809_BENNET#0 | 286 | 3 | 0 (—) | - / ZS_SMITH_FIRE ×2; - / ZS_SMITH_ANVIL ×1 |
| SLD_823_KHALED#0 | 289 | 2 | 1 (15.7) | ZS_GOTOBED / ZS_SLEEP ×2 |
| BAU_983_DRAGOMIR#0 | 289 | 2 | 0 (—) | - / ZS_SIT_BENCH ×2 |
| MIL_312_WULFGAR#0 | 287 | 2 | 0 (—) | - / ZS_STAND_ARMSCROSSED ×2 |
| PAL_230_RITTER#0 | 289 | 2 | 0 (—) | - / ZS_SMALLTALK ×2 |
| VLK_4003_BUERGERIN#0 | 286 | 1 | 1 (11.9) | ZS_SLEEP / ZS_GOTOBED ×1 |
| VLK_402_RICHTER#0 | 278 | 1 | 1 (10.1) | ZS_SLEEP / ZS_GOTOBED ×1 |
| VLK_422_SALANDRIL#0 | 289 | 2 | 0 (—) | - / ZS_STAND_ARMSCROSSED ×1; - / ZS_SLEEP ×1 |
| VLK_466_GERNOD#0 | 287 | 2 | 0 (—) | - / ZS_SIT_BENCH ×1; ZS_GOTOBED / ZS_SLEEP ×1 |
| VLK_497_BUERGERIN#0 | 269 | 2 | 0 (—) | - / ZS_SMALLTALK ×1; - / ZS_STAND_EATING ×1 |
| BAU_902_GUNNAR#0 | 287 | 1 | 0 (—) | - / ZS_SIT_CHAIR ×1 |
| BAU_913_THEKLA#0 | 289 | 1 | 0 (—) | - / ZS_STAND_GUARDING ×1 |
| BAU_941_KATI#0 | 289 | 1 | 0 (—) | - / ZS_STAND_ARMSCROSSED ×1 |
| BDT_10301_ADDON_RANGERBANDIT_M#0 | 289 | 1 | 0 (—) | - / ZS_STAND_ARMSCROSSED ×1 |
| BDT_1045_BANDIT_L#0 | 289 | 1 | 0 (—) | - / ZS_SIT_BENCH ×1 |
| BDT_1047_BANDIT_L#0 | 289 | 1 | 0 (—) | - / ZS_SIT_CAMPFIRE ×1 |

## States (outside the windows)

| Entry state | Marks | ZS differs | Farther than 10 m |
|---|---|---|---|
| ZS_SIT_CAMPFIRE | 4763 | 157 | 155 of 4763 |
| ZS_STAND_GUARDING | 14277 | 12 | 24 of 14277 |
| ZS_COOK_STOVE | 1388 | 1 | 30 of 1388 |
| ZS_GOTOBED | 9991 | 17 | 5 of 9991 |
| ZS_STAND_ARMSCROSSED | 9021 | 9 | 0 of 9021 |
| ZS_GUARD_PASSAGE | 3179 | 9 | 0 of 3179 |
| ZS_SMITH_ANVIL | 342 | 9 | 0 of 342 |
| ZS_SMALLTALK | 13857 | 7 | 0 of 13857 |
| ZS_SMITH_FIRE | 227 | 7 | 0 of 227 |
| ZS_SMITH_COOL | 114 | 6 | 0 of 114 |
| ZS_SIT_THRONE | 3184 | 1 | 2 of 3184 |
| ZS_SIT_BENCH | 3896 | 3 | 0 of 3896 |
| ZS_STAND_DRINKING | 2576 | 2 | 0 of 2576 |
| ZS_REPAIR_HUT | 890 | 2 | 0 of 890 |
| ZS_STAND_EATING | 2318 | 1 | 0 of 2318 |
| ZS_SIT_CHAIR | 2867 | 1 | 0 of 2867 |
| ZS_RAKE_FP | 1500 | 1 | 0 of 0 |
| ZS_STAND_WP | 2016 | 1 | 0 of 2016 |
| ZS_POTION_ALCHEMY | 747 | 1 | 0 of 747 |
| ZS_STAND_SWEEPING | 963 | 1 | 0 of 963 |

## Engine / core ZS_* that differ most (outside the windows)

| Engine | Core | Marks |
|---|---|---|
| ZS_TALK | ZS_SIT_CAMPFIRE | 150 |
| - | ZS_STAND_GUARDING | 14 |
| - | ZS_SMITH_ANVIL | 13 |
| ZS_GOTOBED | ZS_SLEEP | 10 |
| - | ZS_STAND_ARMSCROSSED | 9 |
| - | ZS_GUARD_PASSAGE | 9 |
| - | ZS_SMITH_FIRE | 7 |
| ZS_SLEEP | ZS_GOTOBED | 5 |
| ZS_ATTACK | ZS_SIT_CAMPFIRE | 5 |
| - | ZS_SIT_BENCH | 4 |
| - | ZS_SMALLTALK | 4 |
| - | ZS_SIT_CHAIR | 3 |
| - | ZS_SIT_CAMPFIRE | 2 |
| - | ZS_SMITH_COOL | 2 |
| - | ZS_SLEEP | 2 |

Not people (monsters, without the hero and the dead or the moment of a state change in the engine): same ZS_* in 99.94% of 194625 marks, for information.

## Open windows (the engine never came within 10 m of the entry's point)

| NPC | Marks by entry state |
|---|---|
| VLK_475_BUERGER#0 | ZS_STAND_ARMSCROSSED 95, ZS_SMALLTALK 99, ZS_STAND_DRINKING 24 |
| PAL_226_SCHIFFSWACHE#0 | ZS_STAND_GUARDING 179 |
| PAL_227_SCHIFFSWACHE#0 | ZS_STAND_GUARDING 149 |
| VLK_482_BUERGERIN#0 | ZS_SMALLTALK 96, ZS_STOMP_HERB 48 |
| PAL_228_SCHIFFSWACHE#0 | ZS_STAND_GUARDING 139 |
| VLK_458_RUPERT#0 | ZS_STAND_SWEEPING 24, ZS_SIT_CHAIR 48, ZS_GOTOBED 60 |
| PC_PSIONIC#0 | ZS_SIT_CAMPFIRE 109 |
| VLK_430_BUERGERIN#0 | ZS_SMALLTALK 95 |
| SLD_816_FESTER#0 | ZS_PRACTICE_SWORD 13 |
| BAU_911_ELENA#0 | ZS_STAND_ARMSCROSSED 7 |
| SLD_819_SOELDNER#0 | ZS_STAND_DRINKING 4 |
| BAU_921_BAUER#0 | ZS_SMALLTALK 2 |
| MIL_314_MORTIS#0 | ZS_SMITH_FIRE 2 |
| SLD_804_ROD#0 | ZS_SMALLTALK 2 |
| VLK_461_CARL#0 | ZS_SMITH_FIRE 2 |
| BAU_901_BAUER#0 | ZS_STAND_EATING 1 |
| BAU_910_MARIA#0 | ZS_SIT_THRONE 1 |
| BAU_933_REGA#0 | ZS_PICK_FP 1 |
| BAU_934_BABERA#0 | ZS_PICK_FP 1 |
| BAU_935_BRONKO#0 | ZS_STAND_GUARDING 1 |

## Transitions with the largest travel error

| NPC | Start, min | State | Point | Engine, min | Core, min |
|---|---|---|---|---|---|
| VLK_4001_BUERGERIN#0 | 359.99 | ZS_SWEEP_FP | NW_CITY_UPTOWN_HUT_02_IN | 52.18 | 17.03 |
| SLD_816_FESTER#0 | 419.99 | ZS_PRACTICE_SWORD | NW_BIGFARM_ALLEE_04 | 22.93 | 7.8 |
| SLD_816_FESTER#0 | 540.0 | ZS_PRACTICE_SWORD | NW_BIGFARM_ALLEE_04 | 22.73 | 7.79 |
| SLD_816_FESTER#0 | 660.0 | ZS_PRACTICE_SWORD | NW_BIGFARM_ALLEE_04 | 22.5 | 7.79 |
| SLD_816_FESTER#0 | 1380.0 | ZS_PRACTICE_SWORD | NW_BIGFARM_ALLEE_04 | 21.91 | 7.79 |
| PAL_216_CEDRIC#0 | 660.0 | ZS_GOTOBED | NW_CITY_LEOMAR_BED_03 | 22.21 | 8.56 |
| MIL_330_MILIZ#0 | 780.99 | ZS_SMALLTALK | NW_CITY_KASERN_BARRACK02_02 | 16.41 | 2.81 |
| PAL_209_PALADIN#0 | 662.0 | ZS_SMALLTALK | NW_CITY_UPTOWN_HUT_03_01_B | 18.17 | 6.13 |
| SLD_816_FESTER#0 | 120.0 | ZS_PRACTICE_SWORD | NW_BIGFARM_ALLEE_02 | 19.71 | 7.79 |
| SLD_816_FESTER#0 | 240.0 | ZS_PRACTICE_SWORD | NW_BIGFARM_ALLEE_02 | 19.55 | 7.79 |
| MIL_331_MILIZ#0 | 781.99 | ZS_READ_BOOKSTAND | NW_CITY_KASERN_BARRACK02_04 | 15.95 | 4.32 |
| SLD_816_FESTER#0 | 359.99 | ZS_PRACTICE_SWORD | NW_BIGFARM_ALLEE_02 | 19.39 | 7.79 |
| SLD_816_FESTER#0 | 720.0 | ZS_SIT_CAMPFIRE | NW_BIGFARM_VORPOSTEN1_01 | 21.58 | 10.04 |
| SLD_816_FESTER#0 | 480.0 | ZS_PRACTICE_SWORD | NW_BIGFARM_ALLEE_02 | 19.21 | 7.79 |
| PAL_232_RITTER#0 | 900.0 | ZS_SMALLTALK | NW_CITY_PALCAMP_10 | 13.06 | 1.73 |

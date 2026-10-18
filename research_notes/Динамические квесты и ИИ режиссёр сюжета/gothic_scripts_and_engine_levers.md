# G2 NotR Daedalus quests, plot and events, and the runtime levers an external AI server can use in our OpenGothic fork (state as of 2026-10-11)

Method note. Local code was read in the worktree `yo-character-chat-display-77ba06` at commit `70f6761e`, and the newer branch `claude/openai-live-audio-research-8df685` (head `70a69987`) through `git show`. Local links are relative to this notes file. ZenKit was read on GitHub at the commit our submodule pins, `083cc5f5`; the local `OpenGothic/lib/ZenKit` directory is not checked out. The user's game folder (`/Volumes/shared/projects/Gothic II`) holds only compiled scripts: `_work/Data/Scripts/_compiled/GOTHIC.DAT` (7.55 MB, Russian) and `CONTENT/CUTSCENE/OU.BIN`. No `.d` sources are there. Symbol counts below come from `strings` on that DAT: unique upper-case names without a dot, so locals are excluded and the counts are approximate. Voiced-line counts come from our own `server/src/gothic_ai/gamedata/ou.py` parser. Daedalus source excerpts come from the community reconstruction [VaanaCZ/gothic-2-addon-scripts](https://github.com/VaanaCZ/gothic-2-addon-scripts), branch `2.7-EN`. A `2.7-RU` branch with the same symbol names also exists.

## 1. How G2 NotR Daedalus implements quests, dialogs, routines and events (with one side quest end to end)

### Takeaway
A G2 quest is not an object. It is a convention spread over five kinds of things:
- one global `MIS_*` int that holds `LOG_RUNNING/SUCCESS/FAILED/OBSOLETE`;
- a `TOPIC_*` string constant: the localized quest title, which doubles as the journal key;
- `C_INFO` dialog instances whose `condition` functions read those globals plus `Npc_KnowsInfo`, `Npc_HasItems` and `Kapitel`;
- `information` functions that write the globals, call `Log_*` and `B_LogEntry`, and pay out through `B_GivePlayerXP`;
- chapter-wide helpers such as `B_CheckLog` and `B_CloseTopic` that derive the journal status from the `MIS_*` values.

World changes go through `Wld_InsertNpc`, `B_StartOtherRoutine`/`Npc_ExchangeRoutine` and `Wld_SendTrigger`. They are fired from dialog code, from per-chapter world-entry functions and from `ZS_Dead`.

### Cited Findings
- **Scale of the content (Russian DAT, measured locally).**
  - 242 unique `MIS_*` globals, 250 `TOPIC_*` constants, about 4,034 dialogs (counted as `DIA_*_CONDITION` functions; there are 4,035 `DIA_*_INFO` functions);
  - 1,157 `RTN_*` routine functions, 313 `ZS_*` AI states, 67 `TA_*` routine helpers, 643 `B_*` helper functions, 285 `XP_*` constants, 128 `EVT_*` event symbols.
  - Source: `/Volumes/shared/projects/Gothic II/_work/Data/Scripts/_compiled/GOTHIC.DAT`, measured with `strings`.
- **Voiced lines.** `OU.BIN` holds 20,820 voiced lines: 17,934 `DIA_*`, 2,779 `SVM_*`, 5,530 spoken by the hero (voice 15). Source: the local `OU.BIN`, parsed with [ou.py](../../server/src/gothic_ai/gamedata/ou.py).
- **Source tree size.** The reconstructed `2.7-EN` tree has 1,766 files, 352 dialog files under `Content/Story/Dialoge/` and 760 NPC definition files under `Content/Story/NPC/`. Measured with `gh api .../git/trees/2.7-EN?recursive=1`. — [VaanaCZ/gothic-2-addon-scripts](https://github.com/VaanaCZ/gothic-2-addon-scripts)
- **The reconstruction is faithful.** The repo reconstructs scripts for eight official localizations, Russian among them (cp1251), "designed to compile identically to originals". It also has "Unified" branches with fixes. — [VaanaCZ README](https://github.com/VaanaCZ/gothic-2-addon-scripts)
- **Journal externals.** `Log_CreateTopic(string topic, int section)`, `Log_AddEntry(string topic, string entry)` and `Log_SetTopicStatus(string topic, int status)`. Sections are `LOG_MISSION`/`LOG_NOTE`; statuses are `LOG_RUNNING/SUCCESS/FAILED/OBSOLETE`. "The topic name serves as the unique identifier". — [GMC: Log externals](https://gothic-modding-community.github.io/gmc/zengin/scripts/externals/log/)
- **`C_INFO` fields.**
  - `npc`, `nr`, `description`, `condition`, `information`, `important`, `trade`, `permanent`.
  - The condition function returning TRUE makes the option visible.
  - Important dialogs are spoken by the NPC on his own, ordered by `nr`.
  - Non-permanent infos disappear after use.
  - Source: [GMC: C_INFO](https://gothic-modding-community.github.io/gmc/zengin/scripts/classes/c_info/)
- **Externals used in quest code.** `Npc_KnowsInfo(npc, infoinstance)`, `Npc_ExchangeRoutine(npc, routine)`, `Npc_PercEnable`, `Npc_SendPassivePerc`, `Npc_ChangeAttribute`, `Npc_IsDead`. — [GMC: NPC externals](https://gothic-modding-community.github.io/gmc/zengin/scripts/externals/npc/)
- **`B_LogEntry`** calls `Log_AddEntry`, then prints `PRINT_NewLogEntry` and plays `"LogEntry"`. — [B_LogEntry.d#L5-L11](https://github.com/VaanaCZ/gothic-2-addon-scripts/blob/2.7-EN/_work/Data/Scripts/Content/Story/B_Story/B_LogEntry.d#L5-L11)
- **`B_GivePlayerXP(add_xp)`** adds to `hero.exp`, handles level-up (HP and LP), and finally calls `B_CheckLog()`. — [B_GivePlayerXP.d#L5-L35](https://github.com/VaanaCZ/gothic-2-addon-scripts/blob/2.7-EN/_work/Data/Scripts/Content/Story/B_Story/B_GivePlayerXP.d#L5-L35)
- **`B_CheckLog`** only calls `B_CloseTopics_Mattes/Hoshi/Yoly`: per-scripter lists of topic/variable pairs. — [B_CheckLog.d#L5-L10](https://github.com/VaanaCZ/gothic-2-addon-scripts/blob/2.7-EN/_work/Data/Scripts/Content/Story/Log_Entries/B_CheckLog.d#L5-L10)
- **`B_CloseTopic(topic, MissionVar, CommonVar, EndChapter)`** sets the topic to SUCCESS, OBSOLETE or FAILED from the `MIS_*` value. It also sets RUNNING topics to OBSOLETE once `Kapitel >= EndChapter`. — [B_CloseTopic.d#L5-L25](https://github.com/VaanaCZ/gothic-2-addon-scripts/blob/2.7-EN/_work/Data/Scripts/Content/Story/Log_Entries/B_CloseTopic.d#L5-L25)
- **Side quest end to end: Lobart's turnips ("Harvest the Turnip Field").**
  1. **Title.** The title is a localized string constant: `const string TOPIC_Rueben = "Harvest the Turnip Field"`. — [LOG_Constants_Mattes.d#L35-L36](https://github.com/VaanaCZ/gothic-2-addon-scripts/blob/2.7-EN/_work/Data/Scripts/Content/Story/Log_Entries/LOG_Constants_Mattes.d#L35-L36)
  2. **Offer.** `DIA_Lobart_WorkNOW` ("I'm looking for work.") is visible once one of three other infos is known (`Npc_KnowsInfo`) and while `Kapitel < 3`. — [DIA_BAU_950_Lobart.d#L489-L505](https://github.com/VaanaCZ/gothic-2-addon-scripts/blob/2.7-EN/_work/Data/Scripts/Content/Story/Dialoge/DIA_BAU_950_Lobart.d#L489-L505)
  3. **Acceptance.** It offers two `Info_AddChoice` branches. Both run `Log_CreateTopic(TOPIC_Rueben, LOG_MISSION)`, `Log_SetTopicStatus(…, LOG_RUNNING)` and `B_LogEntry(…)`, then set `MIS_Lobart_Rueben = LOG_RUNNING`. — [DIA_BAU_950_Lobart.d#L540-L582](https://github.com/VaanaCZ/gothic-2-addon-scripts/blob/2.7-EN/_work/Data/Scripts/Content/Story/Dialoge/DIA_BAU_950_Lobart.d#L540-L582)
  4. **Turn-in condition.** The turn-in info `DIA_Lobart_RuebenRunning` is `permanent = TRUE`. Its condition is `MIS_Lobart_Rueben == LOG_RUNNING && Npc_HasItems(other, ItPl_Beet) >= 1 && Kapitel < 3`. — [DIA_BAU_950_Lobart.d#L587-L605](https://github.com/VaanaCZ/gothic-2-addon-scripts/blob/2.7-EN/_work/Data/Scripts/Content/Story/Dialoge/DIA_BAU_950_Lobart.d#L587-L605)
  5. **Turn-in.** It checks for at least 20 beets. If `Npc_IsDead(Hilda)` is false, it starts the follow-up quest `MIS_Lobart_RuebenToHilda`/`TOPIC_Ruebenbringen`. It then sets `MIS_Lobart_Rueben = LOG_SUCCESS`, calls `B_GivePlayerXP(XP_LobartHolRueben)` and offers the reward as `Info_AddChoice`: 5 gold or a cheaper outfit. — [DIA_BAU_950_Lobart.d#L607-L650](https://github.com/VaanaCZ/gothic-2-addon-scripts/blob/2.7-EN/_work/Data/Scripts/Content/Story/Dialoge/DIA_BAU_950_Lobart.d#L607-L650)
  6. **XP.** `XP_LobartHolRueben = 50`. — [XP_Constants.d#L132](https://github.com/VaanaCZ/gothic-2-addon-scripts/blob/2.7-EN/_work/Data/Scripts/Content/Story/XP_Constants.d#L132)
  7. **Journal status.** The dialog never sets the journal status to SUCCESS. `B_CloseTopic(TOPIC_Rueben, MIS_Lobart_Rueben, 0, 3)` in `B_CloseTopics_Mattes` does that on the next `B_CheckLog`, which runs on XP gain and on chapter change. The same call marks the quest OBSOLETE if it is still running at chapter 3. — [B_CloseTopics_Mattes.d#L27-L28](https://github.com/VaanaCZ/gothic-2-addon-scripts/blob/2.7-EN/_work/Data/Scripts/Content/Story/Log_Entries/B_CloseTopics_Mattes.d#L27-L28)
  8. **Cross-quest coupling.** The price of Lobart's clothes reads four other quests' results (`MIS_Lobart_Rueben`, `MIS_Hilda_PfanneKaufen`, `MIS_Vino_Wein`, `MIS_Maleth_Bandits == LOG_SUCCESS`). — [DIA_BAU_950_Lobart.d#L244-L283](https://github.com/VaanaCZ/gothic-2-addon-scripts/blob/2.7-EN/_work/Data/Scripts/Content/Story/Dialoge/DIA_BAU_950_Lobart.d#L244-L283)
- **Daily routines.**
  - An NPC instance sets `daily_routine = Rtn_Start_950`. The routine is a list of `TA_*` calls such as `TA_Stand_Guarding(07,30,23,00,"NW_FARM1_LOBART")` and `TA_Sleep(23,00,07,30,"NW_FARM1_INHOUSE_BED_01")`. — [BAU_950_Lobart.d#L37-L49](https://github.com/VaanaCZ/gothic-2-addon-scripts/blob/2.7-EN/_work/Data/Scripts/Content/Story/NPC/BAU_950_Lobart.d#L37-L49)
  - `B_StartOtherRoutine(npc, name)` wraps `Npc_ExchangeRoutine` + `AI_ContinueRoutine`. — [B_StartOtherRoutine.d](https://github.com/VaanaCZ/gothic-2-addon-scripts/blob/2.7-EN/_work/Data/Scripts/Content/Story/B_Story/B_StartOtherRoutine.d)
  - OpenGothic resolves `Npc_ExchangeRoutine(npc, "X")` to the script function `Rtn_X_<npc.id>`. — [gamescript.cpp#L2069-L2080](../../OpenGothic/common/game/gamescript.cpp)
  - `TA_Min` becomes `Npc::addRoutine(start, end, stateFn, waypoint)`. — [gamescript.cpp#L3278-L3282](../../OpenGothic/common/game/gamescript.cpp)
- **Per-chapter world population.**
  - `B_ENTER_NEWWORLD()` runs every `B_ENTER_NEWWORLD_Kapitel_N()` with `N <= Kapitel`, cumulatively. — [B_Enter_NewWorld.d#L926-L937](https://github.com/VaanaCZ/gothic-2-addon-scripts/blob/2.7-EN/_work/Data/Scripts/Content/Story/B_Story/B_Enter_NewWorld.d#L926-L937)
  - That file alone has 374 `Wld_InsertNpc`, 56 routine changes and 17 `B_KillNpc`/`B_RemoveNpc` calls (counted with grep).
  - `Startup.d` (4,801 lines) has 2,356 `Wld_InsertNpc` calls, and calls `B_ENTER_OLDWORLD/NEWWORLD/ADDONWORLD` from the world init functions (e.g. lines 2488, 4722, 4795). — [Startup.d](https://github.com/VaanaCZ/gothic-2-addon-scripts/blob/2.7-EN/_work/Data/Scripts/Content/Story/Startup.d)
- **Event scripts and triggers.**
  - `Content/Story/Events/` holds trigger callbacks: `EVT_*` functions, `Enter_OldWorld_FirstTime_Trigger`, `B_Event_Portal_Earthquake`, `EVT_OC_MainGate` and others. — [tree 2.7-EN](https://github.com/VaanaCZ/gothic-2-addon-scripts/tree/2.7-EN/_work/Data/Scripts/Content/Story/Events)
  - In OpenGothic a `TriggerScript` vob simply calls its Daedalus function on trigger. — [triggerscript.cpp#L13-L15](../../OpenGothic/common/world/triggers/triggerscript.cpp)
  - `Wld_SendTrigger(name)` posts a `TriggerEvent` to `World::triggerEvent`. — [gamescript.cpp#L1875-L1889](../../OpenGothic/common/game/gamescript.cpp)
- **Perception.** G2 perception intervals are 0.3 s for NPCs with important info and gate guards, 1 s otherwise. Sense range is 20 m; talk and drawn weapon 5 m; threat, theft and others' damage 10 m; fight sounds 30 m. — `docs/npc-scheduler.md` on branch `claude/openai-live-audio-research-8df685` (it quotes `Perception.d`, `AI_Constants.d`)
- **Kill-driven quest logic lives in the death state.**
  - `ZS_Dead` holds hard-coded per-NPC checks, e.g. `Hlp_GetInstanceID(self) == Hlp_GetInstanceID(Franco)` sets `MIS_HlpLogan/MIS_HlpEdgor = LOG_OBSOLETE`, plus checks on `MIS_Fester_KillBugs` and `MIS_KrokoJagd`. — [ZS_Dead.d#L30-L101](https://github.com/VaanaCZ/gothic-2-addon-scripts/blob/2.7-EN/_work/Data/Scripts/Content/AI/Human/ZS_Human/ZS_Dead.d#L30-L101)
  - `ZS_Dead_loop` calls `B_DragonKillCounter(self)`. — [ZS_Dead.d#L135-L140](https://github.com/VaanaCZ/gothic-2-addon-scripts/blob/2.7-EN/_work/Data/Scripts/Content/AI/Human/ZS_Human/ZS_Dead.d#L135-L140)

### Inferences
- **The vanilla journal is a derived view.** The `MIS_*` integers are the truth. Journal topics are created ad hoc in `information` functions, and their final status is recomputed by `B_CheckLog` → `B_CloseTopic` from the `MIS_*` values. So an external system that writes a vanilla `MIS_*` variable automatically gets a consistent journal on the next XP gain or chapter change.
- **Quest progress has no event channel.** Progress shows up only as side effects inside dialog `information` functions, `ZS_Dead` and trigger callbacks. A director that wants to know "quest X advanced" must poll the `MIS_*` globals or hook the `Log_*` externals and dialog execution in C++ (section 3).
- **Rewards and XP are plain function calls.** Any C++ caller can trigger the full native UX, including the "XP gained" text and level-up, through `B_GivePlayerXP(n)` and `B_LogEntry(topic, text)`.

### Gaps
- The reconstructed `2.7-RU` sources were not compiled and compared with the user's `GOTHIC.DAT`. It is not verified that they are byte-identical for this install (the folder looks like a Steam install: `EmptySteamDepot`, `installscript.vdf`).
- The C_INFO count (about 4,034) is a proxy (dialog condition functions). An exact count needs ZenKit enumeration of `C_INFO` instances (`GameScript::initDialogs` does that at runtime), which was not run.

## 2. How tightly the main plot is coupled to global state, and what is safe to leave alone vs. what conflicts with dynamic quests

### Takeaway
The main plot is a web of global ints (`Kapitel`, `MIS_OLDWORLD`, `MIS_ReadyforChapter3/4/6`, `MIS_KilledDragons`, `MIS_AllDragonsDead`, the Innos' Eye flags, the ship flags) and of `Npc_KnowsInfo` facts. They are set inside specific dialog infos and level-change scripts and read everywhere: almost every dialog of plot NPCs is gated on `Kapitel`.

Purely additive content leaves this web intact: new journal topics, XP, items, monsters or helper NPCs spawned from existing templates, temporary routines of non-plot NPCs. Writing vanilla variables, killing or removing plot NPCs, or changing `Kapitel` or guild attitudes directly conflicts with it.

### Cited Findings
- **`B_Kapitelwechsel(neues_Kapitel, level_zen)`.**
  - It sets `Kapitel = neues_Kapitel`.
  - It applies per-chapter side effects: in chapter 2 it clears the immortal flags of `Sekob` and `Bengar`; in chapters 4–6 it sets `PLAYER_TALENT_ALCHEMY[CHARGE_Innoseye] = TRUE`.
  - It calls `IntroduceChapter(...)`, then `B_CheckLog()`, then `B_ENTER_OLDWORLD()`/`B_ENTER_NEWWORLD()`, and finally resets `XP_Ambient` per chapter (`XP_AmbientKap1..6` = 50..300).
  - Sources: [B_Kapitelwechsel.d#L5-L117](https://github.com/VaanaCZ/gothic-2-addon-scripts/blob/2.7-EN/_work/Data/Scripts/Content/Story/B_Story/B_Kapitelwechsel.d#L5-L117); [XP_Constants.d#L111-L117](https://github.com/VaanaCZ/gothic-2-addon-scripts/blob/2.7-EN/_work/Data/Scripts/Content/Story/XP_Constants.d#L111-L117)
- **Chapter changes are fired from specific dialogs.**
  - Xardas's first info calls `B_Kapitelwechsel(1, NEWWORLD_ZEN)`, with the comment "must come here in any case, because of XP_AMBIENT", followed by `Npc_ExchangeRoutine(self,"START")`. — [DIA_NONE_100_Xardas.d#L32-L34](https://github.com/VaanaCZ/gothic-2-addon-scripts/blob/2.7-EN/_work/Data/Scripts/Content/Story/Dialoge/DIA_NONE_100_Xardas.d#L32-L34)
  - Lord Hagen sets `MIS_OLDWORLD = LOG_RUNNING`, calls `B_Kapitelwechsel(2, NEWWORLD_ZEN)`, switches `Fernando`'s routine and inserts `BDT_1020_Bandit_L` at `NW_TROLLAREA_PATH_47`. — [DIA_PAL_200_Hagen.d#L498-L512](https://github.com/VaanaCZ/gothic-2-addon-scripts/blob/2.7-EN/_work/Data/Scripts/Content/Story/Dialoge/DIA_PAL_200_Hagen.d#L498-L512)
  - Pyrokar sets `MIS_ReadyforChapter4 = TRUE`, with the comment "with this variable into the level change to the OW → chapter 4". — [DIA_KDF_500_Pyrokar.d#L1498](https://github.com/VaanaCZ/gothic-2-addon-scripts/blob/2.7-EN/_work/Data/Scripts/Content/Story/Dialoge/DIA_KDF_500_Pyrokar.d#L1498)
- **Pervasive chapter gating.**
  - Xardas's dialog file has at least 14 conditions with `Kapitel < 3` (lines 18–610). Hagen's, Garond's, Lee's and Pyrokar's files gate on `Kapitel == 2/3`, `Kapitel >= 3` and `MIS_OLDWORLD`. Garond's chapter-2 infos also call `B_StartOtherRoutine` on other NPCs.
  - Sources (grep of each file): [Xardas](https://github.com/VaanaCZ/gothic-2-addon-scripts/blob/2.7-EN/_work/Data/Scripts/Content/Story/Dialoge/DIA_NONE_100_Xardas.d), [Hagen](https://github.com/VaanaCZ/gothic-2-addon-scripts/blob/2.7-EN/_work/Data/Scripts/Content/Story/Dialoge/DIA_PAL_200_Hagen.d), [Garond](https://github.com/VaanaCZ/gothic-2-addon-scripts/blob/2.7-EN/_work/Data/Scripts/Content/Story/Dialoge/DIA_PAL_250_Garond.d), [Lee](https://github.com/VaanaCZ/gothic-2-addon-scripts/blob/2.7-EN/_work/Data/Scripts/Content/Story/Dialoge/DIA_SLD_800_Lee.d)
- **Plot globals in the user's Russian DAT (local `strings`).** `KAPITEL`, `MIS_OLDWORLD`, `MIS_READYFORCHAPTER3`, `MIS_READYFORCHAPTER4`, `MIS_READYFORCHAPTER6`, `ENTEROW_KAPITEL2`, `MIS_INNOSEYESTOLEN`, `MIS_SCKNOWSINNOSEYEISBROKEN`, `MIS_RITUALINNOSEYEREPAIR`, `MIS_KILLEDDRAGONS`, `MIS_ALLDRAGONSDEAD`, `MIS_SCVISITSHIP`, `MIS_SHIPISFREE`, `MIS_RICHTERSPERMISSIONFORSHIP`, `MIS_SCKNOWSWAYTOIRDORATH`, `MIS_XARDAS_GOTOVATRASINNOSEYE`, `MIS_PYROKAR_GOTOVATRASINNOSEYE`. Source: `/Volumes/shared/projects/Gothic II/_work/Data/Scripts/_compiled/GOTHIC.DAT`.
- **World entry rebuilds chapter state.** On every entry into a world, `B_ENTER_NEWWORLD` re-runs the chapter blocks. Those blocks insert, remove and re-route NPCs; for example chapter 1 runs `B_RemoveNpc` on the KDW mages and `Wld_InsertNpc` for Patrick, Monty and others. — [B_Enter_NewWorld.d#L7-L78](https://github.com/VaanaCZ/gothic-2-addon-scripts/blob/2.7-EN/_work/Data/Scripts/Content/Story/B_Story/B_Enter_NewWorld.d#L7-L78)
- **Chapter changes close journal topics.** `B_CloseTopic` sets still-running vanilla topics to OBSOLETE once their end chapter is reached; only topics listed in `B_CloseTopics_*` are touched. — [B_CloseTopic.d#L20-L24](https://github.com/VaanaCZ/gothic-2-addon-scripts/blob/2.7-EN/_work/Data/Scripts/Content/Story/Log_Entries/B_CloseTopic.d#L20-L24)
- **Specific NPC deaths change quest variables.** `ZS_Dead` changes quest variables when specific NPCs die (Franco, Ramon, Fortuno, DiegoOW and others), and the dragon-kill counter drives the plot. — [ZS_Dead.d](https://github.com/VaanaCZ/gothic-2-addon-scripts/blob/2.7-EN/_work/Data/Scripts/Content/AI/Human/ZS_Human/ZS_Dead.d)
- **Guild attitudes are global and persist.** Each save stores them: `gilAttitudes` is written in `saveQuests`. Scripts set them through `Wld_SetGuildAttitude`/`Wld_ExchangeGuildAttitudes`. — [gamescript.cpp#L536-L556](../../OpenGothic/common/game/gamescript.cpp); [gamescript.h#L250-L252](../../OpenGothic/common/game/gamescript.h)
- **The project already knows Xardas is fragile.** The MVP doc picks Xardas as the first test NPC "but he is important for the plot", so experiments run on separate saves and `detach` must restore the routine. — [docs/MVP.md#L17](../../docs/MVP.md)
- **Personas are chapter-1 only.** Hand-written personas carry "chapter 1 knowledge only". — [persona.py#L21](../../server/src/gothic_ai/persona.py)

### Inferences
- **Safe to add:**
  - new journal topics whose names do not collide with vanilla `TOPIC_*` strings (`B_CloseTopic` touches only listed topics);
  - `B_GivePlayerXP`;
  - giving items that already exist as instances;
  - spawning extra monsters or ambient NPCs from existing instances at existing waypoints;
  - short soft actions or temporary routines on non-plot NPCs;
  - per-NPC flags in `aivar[]`, provided the slot is not used by vanilla AI (unverified per slot).
- **Conflicting:**
  - writing vanilla `MIS_*` or `Kapitel`. Setting `Kapitel` directly skips the `B_Kapitelwechsel` side effects (immortality flags, world population, journal closure, XP scaling);
  - killing or removing NPCs that `ZS_Dead`, `Npc_IsDead(...)` checks or later chapters rely on (Hilda in the Lobart example, Diego, Franco);
  - re-routing NPCs that a later `B_ENTER_*_Kapitel_N` or plot dialog re-routes again. The script wins silently on the next world entry or chapter change;
  - `Wld_SetGuildAttitude`, and guild changes of the hero, which unlock or lock large dialog trees;
  - `Wld_SetTime` and teleports during scripted sequences.
- **A director needs a "plot safety catalog"** generated from the scripts: the list of plot NPC instances (immortal flags, NPCs referenced in `ZS_Dead`, in `Npc_IsDead` conditions and in `B_Enter_*`), the plot globals above, and the waypoints used by chapter scripts. The MVP's planned "M6: knowledge base from the original scripts through ZenKit, filtered by chapter and quest variables" is the natural place for it. — [docs/MVP.md#L315](../../docs/MVP.md)

### Gaps
- A full list of the chapter 3, 5 and 6 `B_Kapitelwechsel` call sites was not compiled. Only Xardas (1), Hagen (2) and Pyrokar's chapter-4 flag were confirmed. Chapter 3 and later go through `MIS_ReadyforChapterN` and level-change scripts, which were not traced line by line.
- How many `aivar` slots vanilla AI leaves free (`AIV_*` constants in `AI_Constants.d`) was not checked.

## 3. Runtime levers in OpenGothic/ZenKit for an external system: call functions, read/write globals and instance fields, C_INFO, journal, inserting NPCs/items, routines, triggers, XP, externals, and save persistence

### Takeaway
Almost everything is reachable from C++ in-process today, without changing scripts:
- `GameScript::getVm()` is public, so the bridge can call any Daedalus function (`B_GivePlayerXP`, `B_LogEntry`, `B_StartOtherRoutine`, any `DIA_*_Info`) and read or write any global (`Kapitel`, every `MIS_*`) or instance field (`INpc` attributes, `aivar`, name, guild).
- `World::addNpc`/`addItem` spawn existing instances at a waypoint or a position.
- `Npc::addRoutine`/`excRoutine` build or swap routines.
- `World::triggerEvent` fires ZEN triggers.
- `override_function` can wrap any script-to-script call.

What does not exist:
- runtime Daedalus compilation;
- new symbols: no new C_INFO instances, functions or item/NPC templates;
- new externals without a recompiled DAT.

Persistence is uneven:
- globals are saved by name and survive DAT changes;
- NPC instances, routines, AI states and known infos are saved by symbol index or address, which ties saves to the exact DAT;
- the journal and inserted NPCs/items persist automatically;
- our bridge already writes its own entry into the save.

### Cited Findings
- **The VM is public, and so are symbol lookups.** `inline auto& getVm() { return vm; }`, `findSymbol(std::string_view)`, `findSymbol(size_t)` and `findSymbolIndex`. The private section starts at line 176. — [gamescript.h#L78](../../OpenGothic/common/game/gamescript.h); [gamescript.h#L111-L114](../../OpenGothic/common/game/gamescript.h)
- **Calling functions.**
  - ZenKit `DaedalusVm::call_function<R>(name|symbol, args...)`. — [DaedalusVm.hh#L114-L124](https://github.com/GothicKit/ZenKit/blob/083cc5f5592726aed435f8e863a6e09e92748a4d/include/zenkit/DaedalusVm.hh#L114-L124)
  - OpenGothic already calls script functions from C++ this way: `G_CanNotUse`, `G_PickLock`, `B_RefreshAtInsert`, `initPerceptions`, dialog `information` functions. Before such calls it sets `self`/`other` with a `ScopeVar`. — [gamescript.cpp#L24](../../OpenGothic/common/game/gamescript.cpp); [gamescript.cpp#L956-L976](../../OpenGothic/common/game/gamescript.cpp); [gamescript.cpp#L1190-L1201](../../OpenGothic/common/game/gamescript.cpp)
- **Reading and writing globals.** `DaedalusSymbol::get_int/set_int/set_string(value, index, context)`. — [DaedalusScript.hh#L497-L541](https://github.com/GothicKit/ZenKit/blob/083cc5f5592726aed435f8e863a6e09e92748a4d/include/zenkit/DaedalusScript.hh#L497-L541)
  - OpenGothic's save loader restores globals with `findSymbol(name)` → `s->set_int(v, j)`, skipping members and constants. — [gamescript.cpp#L567-L658](../../OpenGothic/common/game/gamescript.cpp)
- **Instances.** `init_instance` runs the instance constructor; `allocate_instance` creates the instance without running it. — [DaedalusVm.hh#L176-L288](https://github.com/GothicKit/ZenKit/blob/083cc5f5592726aed435f8e863a6e09e92748a4d/include/zenkit/DaedalusVm.hh#L176-L288)
  - All `C_INFO` instances are created once at session start through `enumerate_instances_by_class_name("C_INFO", …)` into `dialogsInfo`. — [gamescript.cpp#L442-L449](../../OpenGothic/common/game/gamescript.cpp)
  - There is no API to add symbols: `DaedalusScript` exposes only `symbols()`/`find_symbol_by_*` and `enumerate_instances_by_class_name`. — [DaedalusScript.hh#L803-L890](https://github.com/GothicKit/ZenKit/blob/083cc5f5592726aed435f8e863a6e09e92748a4d/include/zenkit/DaedalusScript.hh#L803-L890)
- **Externals.**
  - `register_external(name, fn)` returns silently if the symbol is absent and throws "symbol is not external" if the symbol is a script function. New externals therefore must be declared in the compiled DAT. — [DaedalusVm.hh#L414-L419](https://github.com/GothicKit/ZenKit/blob/083cc5f5592726aed435f8e863a6e09e92748a4d/include/zenkit/DaedalusVm.hh#L414-L419)
  - OpenGothic binds 202 externals through `bindExternal`, among them `wld_insertnpc`, `wld_insertitem`, `wld_sendtrigger`, `npc_exchangeroutine`, `npc_knowsinfo`, `log_createtopic`, `log_settopicstatus`, `log_addentry` and `info_addchoice`. — [gamescript.cpp#L118-L325](../../OpenGothic/common/game/gamescript.cpp); [gamescript.h#L201-L205](../../OpenGothic/common/game/gamescript.h)
- **Overriding script functions.**
  - `override_function(name, callback)` redirects calls of a Daedalus function to C++. — [DaedalusVm.hh#L490-L563](https://github.com/GothicKit/ZenKit/blob/083cc5f5592726aed435f8e863a6e09e92748a4d/include/zenkit/DaedalusVm.hh#L490-L563)
  - The override is checked only at the `BL` opcode, i.e. on script-to-script calls. C++ `call_function` → `unsafe_call` jumps to the function body directly. — [DaedalusVm.cc#L106-L114](https://github.com/GothicKit/ZenKit/blob/083cc5f5592726aed435f8e863a6e09e92748a4d/src/DaedalusVm.cc#L106-L114); [DaedalusVm.cc#L233-L249](https://github.com/GothicKit/ZenKit/blob/083cc5f5592726aed435f8e863a6e09e92748a4d/src/DaedalusVm.cc#L233-L249)
  - OpenGothic already uses `override_function` heavily for Ikarus/LeGo emulation (`MEM_ReplaceFunc`, `ASMINT_*` and others). — [directmemory.cpp#L103-L141](https://github.com/Try/OpenGothic/blob/master/common/game/compatibility/directmemory.cpp#L103-L141)
- **Inserting NPCs and items.**
  - `World::addNpc(name|instance, waypoint)`, `addNpc(instance, Vec3)` and `addItem(instance, waypoint|Vec3)` are public. — [world.h#L154-L160](../../OpenGothic/common/world/world.h)
  - `Wld_InsertNpc` wraps `world().addNpc(...)` + `fixNpcPosition`; `Wld_InsertItem` wraps `world().addItem(...)`. — [gamescript.cpp#L2001-L2020](../../OpenGothic/common/game/gamescript.cpp)
- **Routines.** `Npc::addRoutine(gtime s, gtime e, uint32_t callback, wp)` and `Npc::excRoutine(callback)` are public. `excRoutine` clears the routines and invokes the `Rtn_*` function. — [npc.h#L320-L321](../../OpenGothic/common/world/objects/npc.h); [npc.cpp#L4347-L4368](../../OpenGothic/common/world/objects/npc.cpp)
- **Triggers.** `World::triggerEvent(TriggerEvent)` dispatches to vobs and sounds. Unknown targets are logged, with a hard-coded skip list for dragonisland. — [world.cpp#L500-L520](../../OpenGothic/common/world/world.cpp)
- **Known infos.** Known infos are a `std::set<pair<npcSymbolIndex, infoSymbolIndex>>`. `setNpcInfoKnown`/`doesNpcKnowInfo` are private. — [gamescript.h#L448-L449](../../OpenGothic/common/game/gamescript.h); [gamescript.h#L468](../../OpenGothic/common/game/gamescript.h); [gamescript.cpp#L3425-L3433](../../OpenGothic/common/game/gamescript.cpp)
- **Mod manifests.** `-game:<modfile.ini>` reads `[FILES] GAME=` (an alternative GOTHIC.DAT), `OUTPUTUNITS=` and `VDF=`, so a patched script DAT can ship without touching the original game files. — [gothic.cpp#L158-L175](../../OpenGothic/common/gothic.cpp); [Try/OpenGothic README](https://github.com/Try/OpenGothic)
- **What OpenGothic does not support.** "Content mods … that only rely on regular scripting and do not use memory hacking" are supported. Ikarus/LeGo are only partially emulated. Union plugins are "not possible". — [Try/OpenGothic README](https://github.com/Try/OpenGothic). The script-merge tool `zParserExtender` ("Merge or override new scripts for compiled .DAT files") is a Union plugin. — [Gratt-5r2/zParserExtender](https://github.com/Gratt-5r2/zParserExtender)
- **Ninja.** It injects scripts as an x86 DLL and targets the original Gothic executables only. — [szapp/Ninja](https://github.com/szapp/Ninja)
- **Compiler.** A cross-platform compiler exists: `dzieje-khorinis/DaedalusCompiler` (C#/.NET Core, ANTLR4) compiles `Gothic.src` + `.d` to `GOTHIC.DAT` plus optional `OU.csl`/`OU.bin`, and runs on Linux and macOS (Docker as well). Last updated 2026-03-30 per the GitHub API. — [dzieje-khorinis/DaedalusCompiler](https://github.com/dzieje-khorinis/DaedalusCompiler)
- **Save format: what is written.**
  - `GameSession::save` writes `header`, `preview.jpg`, `game/session` (ticks, world time), `game/camera`, `worlds/<name>/…`, `game/perc`, `game/quests` and `game/daedalus`. Then `Gothic::saveExtFn` lets the bridge add its own entries. — [gamesession.cpp#L180-L233](../../OpenGothic/common/game/gamesession.cpp)
  - Each world stores every NPC (`/npc/`), items, mobsi vob trees, pending trigger events and routines. — [worldobjects.cpp#L123-L150](../../OpenGothic/common/world/worldobjects.cpp)
- **Save format: globals by name.** `saveVar` writes every non-member, non-const INT, FLOAT and STRING global with its name. NPC and item instance globals are written as world ids. — [gamescript.cpp#L558-L565](../../OpenGothic/common/game/gamescript.cpp); [gamescript.cpp#L692-L720](../../OpenGothic/common/game/gamescript.cpp)
- **Save format: indices and addresses.**
  - An NPC is stored as `symbol_index()` plus its full `INpc` field set, including `name`, `guild`, `attribute[]`, `aivar[]`, `daily_routine` and `exp`. — [serialize.cpp#L311-L345](../../OpenGothic/common/game/serialize.cpp)
  - `ScriptFn` is stored as a raw `uint32`. — [serialize.cpp#L237-L250](../../OpenGothic/common/game/serialize.cpp)
  - NPC routines are stored as `(start, end, callback, point, fallbackName)`. — [npc.cpp#L343-L350](../../OpenGothic/common/world/objects/npc.cpp)
  - Known infos are stored as symbol-index pairs. — [gamescript.cpp#L536-L556](../../OpenGothic/common/game/gamescript.cpp)
- **Our bridge's own save entry.** The bridge writes `ai_bridge/session` (`{branch, seq}`) through `saveExtFn` and reads it back through `sessionStartFn`, on the loading thread under a mutex. — [aibridge.cpp#L234-L248](../../OpenGothic/ai_bridge/aibridge.cpp); [gothic.h#L185-L196](../../OpenGothic/common/gothic.h); [docs/protocol.md#L139-L145](../../docs/protocol.md)
- **The server already persists per-save state.** It stores state per save branch in an append-only SQLite journal: table `events(branch, seq, npc, kind, role, content, …)`, where `kind` is currently `message | summary`. A load branches off at the `seq` written into the save. — [memory.py#L1-L8](../../server/src/gothic_ai/memory.py); [memory.py#L30-L46](../../server/src/gothic_ai/memory.py)

### Inferences
- **Bridge-only levers** (no engine or script changes beyond the bridge itself):
  - read `Kapitel`/`MIS_*` (`findSymbol("KAPITEL")->get_int()`);
  - call `B_GivePlayerXP(n)`, `B_LogEntry(topic, text)`, `B_StartOtherRoutine(npc, "X")` and `B_KillNpc` with `ScopeVar`-style `self`/`other` set-up (the `ScopeVar` struct is file-local to gamescript.cpp, so the bridge would need its own equivalent);
  - spawn with `world.addNpc(script.findSymbolIndex("WOLF"), "NW_…")`;
  - create inventory items (`Npc::addItem`);
  - edit `INpc` fields (rename a spawned template NPC, set `guild` or `aivar`);
  - fire existing ZEN triggers by name.
  The `NpcController` already resolves instance symbols by name through `world.script().findSymbolIndex(...)`. — [npccontroller.cpp#L48-L94](../../OpenGothic/ai_bridge/npccontroller.cpp)
- **Small engine additions are needed for:**
  - creating journal topics and setting their status (`quests` and the `log_*` methods are private; `questLog()` is const). `B_LogEntry` covers only `Log_AddEntry`. Calling the `Log_*` external symbols through `call_function` is unverified: `unsafe_call` jumps to a symbol address, and externals are dispatched by the `BE` opcode;
  - marking infos as known;
  - event hooks (section 5).
  Each is a few lines, like the 3-line `onObservedPerception` hook on the newer branch.
- **Wrapping script functions without recompiling.** Because overrides fire only on script-to-script `BL` calls and `call_function` bypasses them, the bridge can wrap any script function: register `override_function("B_KAPITELWECHSEL", cb)`, emit `chapter_changed` in `cb`, then call the original body through `call_function`. This is a cheap "chapter change", "XP gained" or "journal entry" event source. It is untested and needs a spike, in particular the double `push_call` inside the override wrapper.
- **Persistence.**
  - Additive runtime effects persist in the vanilla save with no extra work: journal topics and entries (`game/quests`), inserted NPCs/items and their modified `INpc` fields, globals, routines built from existing `ZS_*` functions.
  - Server-side quest state (generated quest definitions, director memory) belongs in the existing branch/seq journal (a new `kind`, e.g. `quest`), so loading an older save rolls quest state back exactly like NPC memory.
  - Alternatively, a compact JSON blob could go into a second bridge save entry next to `ai_bridge/session`.
- **Save fragility of a patched DAT.** NPCs, routines (`ScriptFn`) and known infos are saved by symbol index or address, so any change to `GOTHIC.DAT` that shifts existing symbols breaks old saves. Symbols appended at the end of `Gothic.src` should keep the indices stable. Globals restore by name, so new globals are safe.

### Gaps
- Not verified: whether `DaedalusVm::call_function` on an external symbol (e.g. `LOG_CREATETOPIC`) works, and whether `override_function` + re-calling the original from C++ behaves correctly. Neither was tested.
- Whether item instance fields (`description`, `text[]`, `count[]`) are saved per item was not checked, so it is unknown whether a generated "quest item" text persists.

## 4. The quest journal UI in OpenGothic: can it show dynamic entries, and can we add externals?

### Takeaway
Yes. OpenGothic's journal is a plain C++ list of topics keyed by topic string: section Mission/Note, status Running/Success/Failed/Obsolete, plus entries. The vanilla G2 menu renders it, and it is saved in `game/quests`. Any topic created at runtime appears in the native UI and survives saves. The only change needed is a public mutator or bridge entry point, since `GameScript::quests` is private.

New Daedalus-callable externals can be bound only to external symbols that the DAT declares. The bridge can still expose any C++ capability over the WebSocket without involving Daedalus at all.

### Cited Findings
- **`QuestLog` data model.**
  - `enum Status {Running=1, Success=2, Failed=3, Obsolete=4}`, `enum Section {Mission=0, Note=1}`, and `struct Quest {name, section, status, vector<string> entry}`.
  - `add(name, s)` returns the existing quest if the name exists. `setStatus` creates the topic as Mission if missing, unless the new status is Obsolete. `addEntry` ignores unknown topics.
  - Sources: [questlog.h#L9-L48](../../OpenGothic/common/game/questlog.h); [questlog.cpp#L7-L53](../../OpenGothic/common/game/questlog.cpp)
- **Externals behind it.** `log_createtopic` accepts sections 0 and 1, `log_settopicstatus` accepts statuses 1–4, and `log_addentry` forwards to `quests.addEntry`. — [gamescript.cpp#L3284-L3299](../../OpenGothic/common/game/gamescript.cpp)
- **Menu rendering.** The menu's listbox items render `drawQuestList(...)` from `Gothic::inst().questLog()`. Categories map to status Running, Failed and Success, plus `Log` = notes. — [gamemenu.cpp#L483-L490](../../OpenGothic/common/ui/gamemenu.cpp); [gamemenu.h#L41-L46](../../OpenGothic/common/ui/gamemenu.h)
  - `isCompatible`: notes are shown under Log; missions are shown when the status matches the category. Obsolete (4) matches no category. — [gamemenu.cpp#L338-L342](../../OpenGothic/common/ui/gamemenu.cpp)
- **Access.** `Gothic::questLog()` returns `&game->script()->questLog()` (const). — [gothic.cpp#L293-L295](../../OpenGothic/common/gothic.cpp); [gamescript.h#L79](../../OpenGothic/common/game/gamescript.h)
- **Topic titles are localized literals.** For example `TOPIC_Rueben = "Harvest the Turnip Field"` in EN; in the Russian DAT it is a cp1251 string. — [LOG_Constants_Mattes.d#L35](https://github.com/VaanaCZ/gothic-2-addon-scripts/blob/2.7-EN/_work/Data/Scripts/Content/Story/Log_Entries/LOG_Constants_Mattes.d#L35)
- **Encoding.** The bridge declares `codepage: cp1251` in `hello` and converts with `TextCodec`. — [aibridge.cpp#L324-L328](../../OpenGothic/ai_bridge/aibridge.cpp)
- **Native UX on new entries.** `B_LogEntry` shows the "new journal entry" text and plays the sound. — [B_LogEntry.d#L5-L11](https://github.com/VaanaCZ/gothic-2-addon-scripts/blob/2.7-EN/_work/Data/Scripts/Content/Story/B_Story/B_LogEntry.d#L5-L11)
- **New externals need DAT declarations.** `register_external` needs the symbol to exist and to be declared external. — [DaedalusVm.hh#L414-L419](https://github.com/GothicKit/ZenKit/blob/083cc5f5592726aed435f8e863a6e09e92748a4d/include/zenkit/DaedalusVm.hh#L414-L419)

### Inferences
- **Proposed bridge message.** `quest_log{op: create|status|entry, topic, section?, status?, text?}`, implemented through a 5–10 line public method on `GameScript`, or by calling `B_LogEntry` for entries.
- **Topic naming.** Dynamic topics should use a reserved prefix or suffix to avoid collisions with vanilla titles (journal keys are exact strings, so a name clash merges the entries). Statuses must be set explicitly: no `B_CloseTopic` rule covers them, so vanilla chapter changes never auto-close them. That is good for independence, but the director must close or obsolete them itself.
- **Hiding stale quests.** Obsolete topics vanish from all lists, so OBSOLETE is a clean way to retire dynamic quests that became impossible.

### Gaps
- Not checked: how OpenGothic orders topics in the list (insertion order of the vector, presumably), or whether long Russian entries wrap well in the journal UI.

## 5. What the bridge and server expose today (protocol v4 on this branch, v5 on the newer branch), and what a story director needs

### Takeaway
Today's protocol drives the conversation and the body of a few NPCs: attach/detach, eight actions, chat, TTS, ASR, memory branches. Protocol v5 adds an awareness layer for nearby NPCs: roster, `npc_event` for player-caused perceptions, and soft `npc_act`. Nothing anywhere reads or writes quest state, `Kapitel` or the journal, and no events report kills, item pickups, locations or zones, native dialog choices, journal changes, chapter changes, guild changes or XP. A director needs those as roughly ten new event hooks plus four or five new commands.

### Cited Findings
- **Game → server (v4).** `hello`, `session`, `world_loaded`, `loading`, `saved`, `result`, `attached`, `detached`, `snapshot`, `action_result`, `perception` (attached NPCs only, throttled to 1/s per pair), `pong`, `chat_open`, `player_text`, `chat_close`, `line_started`, `ptt_*`, `follow_ended`. — [docs/protocol.md#L23-L46](../../docs/protocol.md)
- **Server → game (v4).** `attach`, `detach`, `action`, `snapshot`, `ping`, `say`, `say_audio`, `chat_text`, `say_end`, `chat_status`, `transcript`, `memory`, `debug` (save/load, devmode), `chat_end`. — [docs/protocol.md#L69-L86](../../docs/protocol.md)
- **Actions.** `go_to_waypoint`, `go_to_npc`, `turn_to`, `play_ani`, `wait`, `stop`, `follow`, `give_item` (from the NPC's own inventory), `debug_add_item` (devmode), `return_to_routine`. — [docs/protocol.md#L92-L109](../../docs/protocol.md); [npccontroller.cpp#L227-L371](../../OpenGothic/ai_bridge/npccontroller.cpp)
- **Snapshot fields.**
  - `npc{id, name, pos, rot, hp, hp_max, busy, action, following, waypoint, nearest_waypoint, inventory[≤40]}`;
  - `player{name, pos, distance, guild, hp, hp_max, armor, melee, ranged}`;
  - `world{name, day, hour, minute}`, `nearby_waypoints[≤6]`, `nearby_npcs[≤8]`;
  - sent only for attached NPCs.
  - Sources: [docs/protocol.md#L47-L52](../../docs/protocol.md); [npccontroller.cpp#L618-L720](../../OpenGothic/ai_bridge/npccontroller.cpp)
- **Server-side LLM tools.** `give_item`, `follow_player`, `stop`, `come_closer`, `gesture`, end conversation. — [npc_tools.py#L41-L71](../../server/src/gothic_ai/npc_tools.py)
- **v5 additions (newer branch).**
  - `npc_roster` (about every 0.5 s: humans within 60 m with `id`, `name`, `guild`, `distance`, `in_view`, `activity`, plus `hour`/`minute`);
  - `npc_event` with kinds `player_near`, `enter_room`, `item_given`, `weapon_drawn`, `theft`, `attack`, `magic`, `murder`, only for NPCs within 30 m, only for acts by the hero, throttled to 1/s per (NPC, kind);
  - `npc_idle`, `npc_act_result`, and soft actions through `npc_act` (`say_svm`, `turn_to`, `look_at`, `play_ani`, `go_to_npc`, `turn_away`).
  - "The server also understands `item_dropped`, `player_sits`, `player_sleeps`, but the bridge does not send them yet: the engine has no perceptions for them."
  - Source: `git show claude/openai-live-audio-research-8df685:docs/protocol.md` lines 147–195.
- **The v5 hook precedent.** The engine-side cost of v5 was a 3-line change: the `Gothic::onObservedPerception` signal plus one emit in `Npc::perceptionProcess`. The bridge observer is 321+60 lines (`ai_bridge/npcobserver.cpp/.h`) and the server planner 528 lines (`brain/planner.py`). Source: `git diff HEAD...claude/openai-live-audio-research-8df685 -- OpenGothic/common`; file sizes from `git show … | wc -l`.
- **No quest or chapter awareness anywhere.** Grep for `questlog|kapitel|MIS_|set_int|call_function` in `OpenGothic/ai_bridge` and `quest|kapitel|chapter|MIS_` in `server/src` finds only the persona comment "chapter 1 knowledge only" ([persona.py#L21](../../server/src/gothic_ai/persona.py)). The newer branch has no such references either.
- **Quest generation is out of MVP scope.** "Quest generation, creation of new NPCs and items, combat AI" is listed as out of scope. — [docs/MVP.md#L54](../../docs/MVP.md)
- **Engine points where director events can be hooked.**
  - `Npc::onNoHealth(bool death, …)`: death or unconsciousness, entering `ZS_Dead`. — [npc.cpp#L595-L635](../../OpenGothic/common/world/objects/npc.cpp)
  - `Npc::takeItem`. — [npc.cpp#L3400](../../OpenGothic/common/world/objects/npc.cpp)
  - `Npc::setTrueGuild`. — [npc.cpp#L1301](../../OpenGothic/common/world/objects/npc.cpp)
  - `Npc::startDialog` (native dialog start). — [npc.cpp#L4183](../../OpenGothic/common/world/objects/npc.cpp)
  - `GameScript::exec` (native dialog choice executed, with `setNpcInfoKnown`). — [gamescript.cpp#L956-L976](../../OpenGothic/common/game/gamescript.cpp)
  - The `log_*` externals. — [gamescript.cpp#L3284-L3299](../../OpenGothic/common/game/gamescript.cpp)
  - `World::triggerEvent`. — [world.cpp#L500](../../OpenGothic/common/world/world.cpp)
  - `Gothic::onTick` (polling globals). — [gothic.h#L186](../../OpenGothic/common/gothic.h)
  - `onWorldLoaded`/`onStartLoading`, already used for `world_loaded`/`loading`. — [aibridge.cpp#L363-L388](../../OpenGothic/ai_bridge/aibridge.cpp)
- **The server cannot start a conversation.** No server→game message opens the chat: `chat_open` comes only from the player pressing T, and the server can only close it with `chat_end`. — [docs/protocol.md#L39](../../docs/protocol.md); [aibridge.cpp#L493-L545](../../OpenGothic/ai_bridge/aibridge.cpp)

### Inferences
Director event and command gap list. "Have" means already emitted.

| Need | Status | Cheapest source |
|---|---|---|
| world/location changed | have (`world_loaded`) | — |
| time of day | partly (`snapshot.world`, `npc_roster.hour/minute`) | add `day` and a standalone `player_state` |
| player guild | only inside attached snapshots | `player_state` poll, or a hook in `Npc::setTrueGuild` |
| kills (any NPC or monster, killer, victim instance) | missing | 1-line signal in `Npc::onNoHealth` |
| item picked up / received / given / traded | missing (only `item_given` perception near NPCs) | `Npc::takeItem`, `addItem` overloads, or inventory diff |
| native dialog info chosen | missing | signal in `GameScript::exec`, with the info symbol name, e.g. `DIA_LOBART_RUEBENRUNNING` |
| journal topic created / status / entry | missing | signal in the `log_*` externals |
| chapter change | missing | poll `KAPITEL` on tick, or override `B_Kapitelwechsel` |
| `MIS_*` changes | missing | poll 242 ints every ~1 s and diff (cheap) |
| XP / level | missing | poll `hero.exp/level`, or wrap `B_GivePlayerXP` |
| zone or waypoint reached | missing | hero nearest-waypoint diff on tick; `World::triggerEvent` for scripted zones |

Commands a director needs:
- `script_get{names[]}`, `script_set{name, value}` (guarded by an allowlist);
- `script_call{fn, self?, other?, args[]}` (allowlist: `B_GivePlayerXP`, `B_LogEntry`, `B_StartOtherRoutine`…);
- `quest_log{…}`;
- `spawn_npc{instance, waypoint|pos, name?, guild?, aivars?}`, `spawn_item{instance, waypoint|pos}`, `give_player_item`;
- `set_routine{npc, entries[]|rtn}`;
- `send_trigger{name}`;
- `start_chat{npc}` (the NPC approaches and addresses the hero).

Effort: about 10–30 lines of engine hooks plus 300–500 lines of bridge code plus a protocol v6 doc, sized after the v5 precedent (a 3-line hook and about 380 bridge lines).

### Gaps
- Unknown whether `Npc::takeItem` also covers trading and scripted `B_GiveInvItems` transfers. Those may go through `Npc::addItem(…, Npc& from, …)` or `Inventory::transfer`, which were not traced.
- Killer attribution at `onNoHealth` time (last attacker or `currentOther`) was not verified.

## 6. Can dynamic dialog options go into the native dialog menu, or only into our separate AI chat?

### Takeaway
Not with scripts alone: `Info_AddChoice` can only point at existing script functions, and the set of `C_INFO` instances is fixed at session start. In C++ it is feasible and fairly contained. `GameScript::dialogChoices` builds the native menu from `dialogsInfo`, filtered by `npc`, known/permanent state and condition. A bridge-supplied list of synthetic choices per NPC could be appended there, and `exec`/`DialogMenu::onEntry` could route a chosen synthetic option to the bridge instead of `vm.call_function`. The NPC's reply can already use the existing generated-line path (`say` → `externalMessageFn`/`externalSoundFn`, which serve subtitles and TTS audio to the regular `AI_Output` pipeline). The alternative needs no engine UI work: offer, accept and complete dynamic quests inside our free-form AI chat through new LLM tools.

### Cited Findings
- **How the native menu is built.** `dialogChoices`:
  - sets `self`/`other`;
  - collects `dialogsInfo` entries with `info->npc == npc.symbol_index()`;
  - shows important infos first;
  - skips infos the hero already knows unless they are permanent;
  - evaluates the `condition` function;
  - emits `DlgChoice{title=description, scriptFn=information, handle=info*, isTrade, sort=nr}`.
  - Source: [gamescript.cpp#L871-L930](../../OpenGothic/common/game/gamescript.cpp)
- **How a choice executes.** `exec` dereferences `*dlg.handle`. If it was the main info, it marks the info known; otherwise it erases the sub-choice. Then it calls `vm.call_function(find_symbol_by_index(dlg.scriptFn))`. — [gamescript.cpp#L956-L976](../../OpenGothic/common/game/gamescript.cpp)
- **Sub-choices.** `updateDialog` returns the `Info_AddChoice` sub-choices of a handle and returns `{}` when `handle == nullptr`. — [gamescript.cpp#L932-L954](../../OpenGothic/common/game/gamescript.cpp)
- **`Info_AddChoice` binds to script functions.** It appends `{text, function}` to an existing `IInfo`; `function` must be a script function index. — [gamescript.cpp#L3370-L3385](../../OpenGothic/common/game/gamescript.cpp)
- **`DialogMenu`.**
  - `onStart` fills `choice` from `ot.dialogChoices(...)`, and auto-plays `choice[0]` when its title is empty, which is how important dialogs work. — [dialogmenu.cpp#L270-L285](../../OpenGothic/common/ui/dialogmenu.cpp)
  - `onEntry` pushes `scriptFn` into `except` and calls `Gothic::inst().dialogExec(...)` → `GameSession::dialogExec` → `GameScript::exec`. — [dialogmenu.cpp#L409-L419](../../OpenGothic/common/ui/dialogmenu.cpp); [gamesession.cpp#L450-L456](../../OpenGothic/common/game/gamesession.cpp)
- **Dialog data shape.** `DlgChoice{title, sort, scriptFn, isTrade, handle}`; `dialogsInfo` is a `std::vector<std::shared_ptr<zenkit::IInfo>>` filled once in `initDialogs`. — [gamescript.h#L50-L56](../../OpenGothic/common/game/gamescript.h); [gamescript.h#L469](../../OpenGothic/common/game/gamescript.h); [gamescript.cpp#L442-L449](../../OpenGothic/common/game/gamescript.cpp)
- **Runtime-line hooks already exist.** `Gothic::externalMessageFn` "lets `GameScript::messageByName/messageTime` serve subtitle text and duration for lines that are not in OU.BIN, so generated lines go through the regular `AI_Output` path" (commit `89579191`). `externalSoundFn` serves WAV bytes. — [gothic.h#L189-L192](../../OpenGothic/common/gothic.h); [aibridge.cpp#L228-L233](../../OpenGothic/ai_bridge/aibridge.cpp)
- **Our chat is separate from the native menu.** The player opens it with T while looking at an NPC; the game attaches the NPC; replies stream as `chat_text` and `say`. — [docs/protocol.md#L39-L41](../../docs/protocol.md); [docs/protocol.md#L78-L83](../../docs/protocol.md)

### Inferences
**Option N1: native menu injection, C++ only.**
1. Add `std::vector<SyntheticChoice>` per NPC symbol, filled by a new `dialog_offer{npc, options[{id, title, important?, permanent?}]}` message.
2. Append the options in `dialogChoices` with a sentinel `handle == nullptr` and `scriptFn` mapped to an id.
3. In `exec`, when `handle == nullptr`, emit `dialog_choice{npc, id}` to the server and skip `call_function`.
4. The server answers with `say` lines (voiced through TTS) and optional new offers or sub-options.

Estimate: about 100–250 lines across `gamescript.cpp`, `dialogmenu.cpp` and the bridge, 3–6 days. It touches upstream hot files (merge-conflict risk with Try/OpenGothic updates).

Caveats:
- `exec` currently dereferences `handle` unconditionally;
- `except` holds `scriptFn` values, so sentinel ids must not collide with symbol indices;
- important synthetic choices would auto-play (an empty title triggers `onEntry`).

**Option N2: script slot pool, needs a recompiled DAT** (section 7, option B).
1. Compile a patch with e.g. 30 placeholder `C_INFO` instances, `DIA_AI_SLOT_01..30`, whose condition and information call new externals `AI_SlotCond(i)`/`AI_SlotInfo(i)`.
2. At runtime C++ rewrites `info->npc` and `info->description` of the slot instances; they are mutable `IInfo` objects in `dialogsInfo`.

This keeps the engine UI untouched and lets slot infos call any vanilla `B_*` function. The cost is the DAT toolchain and the save-index discipline.

**Option N3: AI chat only.** Add LLM tools such as `offer_quest`, `update_quest`, `complete_quest` and `reward` beside `give_item`/`follow_player`. It needs no engine UI changes and fits the current architecture best. It does not reach players who use only the native menu.

### Gaps
- Not checked: how `DialogMenu` handles a choice whose `handle` is null in `startTrade`/`onDoneText`, and whether the native "important" auto-address path (perception `ASSESSTALK` → `ZS_Talk`, `Npc_CheckInfo`) can be triggered from C++ for synthetic important options.

## 7. What abandoning the vanilla scripts would cost, and the feasible integration options with effort estimates

### Takeaway
In G2 "the scripts" are not just quests. The same `GOTHIC.DAT` holds:
- all NPC AI: 313 `ZS_*` states and the perception handlers;
- fight-related script logic;
- 1,157 routines;
- trading inventories, teaching and learning, guild joining;
- world population: `Startup.d` and `B_Enter_*`, 2,700+ `Wld_InsertNpc` calls;
- about 4,034 dialogs bound to 20,820 professionally voiced Russian lines;
- the 242-variable quest web and the six-chapter plot.

Abandoning all of it means rebuilding a game. Even "abandon only the story layer" removes trading, teaching and guild progression, which live in the same dialog infos.

The feasible path is option A: keep vanilla and add an AI director layer on top, through bridge levers that already exist plus about ten small engine hooks, delivering quests through the AI chat and the native journal. Option B (a patched script DAT with AI slots and externals) can come later for deeper integration. Option C (replace everything) is not realistic.

### Cited Findings
- Content volume: see section 1. 242 `MIS_*`, 250 `TOPIC_*`, about 4,034 dialogs, 1,157 `RTN_*`, 313 `ZS_*`, 643 `B_*`, 20,820 OU lines (17,934 dialog, 2,779 SVM, 5,530 hero). Measured locally on `GOTHIC.DAT`/`OU.BIN`.
- **Teaching and trading live in dialog helpers.** Teaching is implemented as dialog helpers (`B_TeachAttributePoints`, `B_TeachFightTalentPercent`, `B_TeachMagicCircle`, `B_TeachPlayerTalent*`). Trader inventories are scripts too: `Story/B_GiveTradeInv/*.d`, 53 files. — [tree 2.7-EN Story/B_Story](https://github.com/VaanaCZ/gothic-2-addon-scripts/tree/2.7-EN/_work/Data/Scripts/Content/Story/B_Story); [tree 2.7-EN Story/B_GiveTradeInv](https://github.com/VaanaCZ/gothic-2-addon-scripts/tree/2.7-EN/_work/Data/Scripts/Content/Story/B_GiveTradeInv)
- **AI lives in the same DAT.** `Content/AI/AI_Intern/Perception.d`, `AI_Constants.d`, `Externals.d`, `Content/AI/Human/TA.d` and the `ZS_Human/*` states. — [tree 2.7-EN Content/AI](https://github.com/VaanaCZ/gothic-2-addon-scripts/tree/2.7-EN/_work/Data/Scripts/Content/AI)
- **Precedents for engine and bridge work in this repo:**
  - "Add engine hooks for external AI control" (commit `1ef4c8f4`: 27 lines in 5 files: `onTick`, external-control flag, console delegation);
  - "Add hook for runtime dialog lines" (`89579191`: 12 lines);
  - v5 observer: a 3-line engine hook, 381 bridge lines, 528-line planner;
  - the whole bridge today is `aibridge.cpp` 879 + `npccontroller.cpp` 724 lines.
  - Source: `git show --stat` and `wc -l` in this worktree.
- **Toolchain for a patched DAT:**
  - reconstructed Russian sources (`2.7-RU`) — [VaanaCZ](https://github.com/VaanaCZ/gothic-2-addon-scripts);
  - a cross-platform compiler (DaedalusCompiler, .NET) — [dzieje-khorinis/DaedalusCompiler](https://github.com/dzieje-khorinis/DaedalusCompiler);
  - OpenGothic `-game:<ini>` with `[FILES] GAME=`. — [gothic.cpp#L158-L175](../../OpenGothic/common/gothic.cpp)
- **Saves bind to symbol indices.** NPCs, routines and known infos are saved by symbol index or address. — [serialize.cpp#L237-L250](../../OpenGothic/common/game/serialize.cpp); [serialize.cpp#L311-L345](../../OpenGothic/common/game/serialize.cpp); [gamescript.cpp#L536-L556](../../OpenGothic/common/game/gamescript.cpp)
- **Per-save state that rolls back on load already exists** (`memory.py` branches/seq; `ai_bridge/session` save entry). — [memory.py#L1-L8](../../server/src/gothic_ai/memory.py); [aibridge.cpp#L234-L248](../../OpenGothic/ai_bridge/aibridge.cpp)

### Inferences
The estimates below are engineering judgement anchored on the precedents above, not measurements.

**Option A: keep vanilla, add an AI layer (recommended first).**

Sub-steps:
- **A1. Script-state access in the bridge.** `script_get`/`script_set` with an allowlist, `script_call` for allowlisted `B_*`, `quest_log`, `spawn_npc`/`spawn_item`, `set_routine`, `send_trigger`, plus a standalone `player_state` (guild, level, exp, `Kapitel`, inventory, nearest waypoint, day and time). Size: about 250–400 lines in the bridge, 5–20 lines in the engine (public `QuestLog` mutators or a `GameScript` wrapper). About 2–4 days.
- **A2. Director events.** `npc_died`, `item_taken`, `info_chosen`, `quest_log_changed`, `trigger_fired`, `chapter_changed`, `guild_changed`, `xp_gained`/`level_up`, `mis_changed` (diff poll), `waypoint_reached`. Size: about 10–30 engine lines (signals as in the v5 precedent), about 300 bridge lines, a protocol v6 doc. About 3–5 days.
- **A3. Server story director.**
  - quest model and state machine (offer → accepted → objectives → done/failed/obsolete), stored as new `kind`s in the branch journal so loads roll back;
  - a generator that only uses grounded entities (existing NPC and item instances, waypoints from the world, monsters by template) and checks a plot safety catalog (section 2);
  - objective trackers fed by the A2 events; rewards through `B_GivePlayerXP` and items;
  - journal through `quest_log`/`B_LogEntry`;
  - delivery through new chat tools (`offer_quest`, `complete_quest`).
  Size: about 1,000–2,000 lines of Python, comparable to two or three times `planner.py`, plus evals. About 2–4 weeks.
- **A4 (optional). Native dialog menu injection** (option N1 in section 6). About 3–6 days; merge risk with upstream.

What option A loses: nothing from vanilla.

Risks:
- generated quests that contradict vanilla state (mitigated by reading `MIS_*`/`Kapitel` into the prompt and by the safety catalog);
- spawned NPCs that later chapter scripts do not know about (harmless unless they block waypoints);
- events triggered while vanilla dialogs or cutscenes run (gate on `isTalk`/dialog state; the `npc_roster.activity` "dialog" flag already exists in v5).

**Option B: replace or extend parts through a patched script DAT.**

Steps:
1. Verify that the `2.7-RU` sources compile to the user's DAT byte for byte.
2. Append new `.d` files at the end of `Gothic.src`:
   - AI slot `C_INFO` pool;
   - new externals: `AI_SlotCond`, `AI_SlotInfo`, `AI_Event(string)`;
   - hooks inside `B_Kapitelwechsel`, `ZS_Dead`, `B_GivePlayerXP` calling `AI_Event`;
   - optionally neutralized or rewritten vanilla side quests.
3. Ship it as a `-game:` mod ini.

Cost: about 1–3 weeks including the .NET toolchain in the build, save-compatibility tests (append-only symbols) and a regression playthrough of the touched quests. Ongoing cost: every vanilla edit must be re-applied over upstream "Unified" fixes, and old saves break if any existing symbol shifts.

Gain: script-native conditions and effects (dynamic quests can use every `B_*` helper inside real dialog infos), a cleaner native-menu integration than N1, and the option to switch off specific vanilla side quests that the director replaces.

**Option C: replace all story scripts.**

It requires re-creating:
- about 4,034 dialogs and 242 quests, the six-chapter flow, about 2,700 world-population calls, trading and teaching;
- re-voicing about 18k dialog lines with TTS, or losing the original voice acting;
- keeping `Content/AI` anyway.

That is months of work for a far worse result, and it collides with the MVP principle "original dialogs and quests are not touched". — [docs/MVP.md#L15](../../docs/MVP.md)

**A hybrid that answers the user's question.** Do not abandon the vanilla scripts. Keep them as the "canon track" and have the AI director:
1. read their state (`Kapitel`, `MIS_*`, known infos) as world facts;
2. generate side content that only adds (new topics, NPCs from templates, items, XP);
3. optionally steer vanilla quests by writing allowlisted `MIS_*` values (e.g. mark one obsolete when the director's own quest supersedes it), letting `B_CheckLog` keep the journal consistent;
4. never touch `Kapitel` or plot NPCs.

A selective "replace parts" (option B) can come later, quest by quest, if the native-menu experience matters.

### Gaps
- No effort estimate here is backed by a spike. The riskiest unknowns are `override_function` wrapping, `call_function` on externals, how many free `aivar` slots there are, and whether the reconstructed RU sources compile byte-identically.
- How OpenGothic handles a save whose world contains NPCs of a symbol index that no longer exists was not checked: `readNpc` logs "Symbol not found" ([serialize.cpp#L325-L335](../../OpenGothic/common/game/serialize.cpp)). What happens after that log line (a crash, or a skipped NPC) is not known.
- No source was found that documents OpenGothic save compatibility with original-engine `.sav` files. OpenGothic uses its own entry-based format.

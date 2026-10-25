#!/usr/bin/env bash
# End-to-end check: starts the AI server with a scenario, then a new game in OpenGothic.
#   check_walk: new game, no SAVE, LLM=fake; Cavalorn walks from Xardas's stairs to Brago's
#       unseen body in the bandit cave using the native waynet, sees it and returns to the hero
#   follow_back: new game, no SAVE, LLM=fake; repeated set_intent follow_hero recalls Cavalorn
#       from go_to_npc towards Brago's body; one follow, cancelled trip and measured return to the hero
#   load_rewind: load a save at least 20 game seconds earlier; npc_views, story_world and npc_died
#       must resume within 2 s. New game, no SAVE, LLM=fake.
#   memprobe: engine RSS after each step (Linux /proc status, macOS RSS and phys_footprint), and after
#       each cycle Linux smaps groups or macOS `footprint` categories;
#       MEMPROBE_PHASES=wave,save,load,spawn,teleport, MEMPROBE_CYCLES=8; LLM=fake, no body URL
#   crime_meet: save 1, native home eviction and a brave victim's Amends approach/payment;
#       LLM=chatgpt runs only the short compensation scene with the real model
#   crime_ally: new game, no SAVE, LLM=fake; Brago attacks the following Cavalorn, the hero knocks him
#       down and finishes him; murder and crime_side remain in the chronicle and Cavalorn answers
#   bandit_parley: new game, LLM=fake; Brago's first fight is rejected, an ultimatum runs its course;
#       Y pays 100 gold for four hours of peace with the cave gang, including 1015; no attack for 20 s,
#       then drawing a weapon breaks the truce and Brago attacks
#   bandit_hold: new game, no SAVE, LLM=fake; ranger bandits and a scripted party member stay quiet
#       on renewable 5 s parley leases for 30 s, then releasing to scripts starts a native attack
#   combat_barks: new game, LLM=fake; first hit warms the fighter, next checks combat_hit and overlay voice;
#       QUICK_URL uses live Gemma and reports phrase/voice delays; TTS=fake|inworld
#   svm_lines: new game, no SAVE, LLM=fake; QUICK_URL supplies ten live Gemma SVM lines, their native
#       text and voice delays, then Cavalorn kills a beast and speaks its native script SVM.
#       An unavailable QUICK_URL falls back after three errors; TTS=fake|inworld.
#       GOTHIC_NO_SVM_LINES=1 keeps native speech only
#   bandit_bounty: new game, LLM=fake; Brago refuses a secret without leverage, sells Dexter's name
#       for 150 gold through Y, updates the vanilla leaflet quest and starts leading to CASTLEMINE_HUT
#   m0: takes Xardas over, walks him around and returns him to his routine
#   m1: Xardas answers a player question line by line (LLM backend: LLM=fake|chatgpt|local)
#   websocket: Lobart and Hilda speak through Responses against an in-process fake server: per-npc
#       WebSocket reuse, tool continuation, HTTP fallback and --no-websocket; SAVE=save_slot_1.sav; LLM=fake
#   m2: npc actions (give_item, follow/stop, gestures)
#   m3: memory survives save/load and follows the save; push-to-talk from a wav file (streamed at
#       speaking pace) through the ASR service (ASR=fake|inworld|voxtral|chatgpt); ASR=inworld is paid,
#       so it also needs PAID_ASR=1
#   m4: what the npc planner needs from the bridge: npc roster, soft actions (npc_act) and their end
#       (npc_idle); with DECIDER_URL the planner runs against that decision service too
#   m5: the turn loop (a rejected commitment corrected in the same turn, a real give after the line),
#       intents moving the body, and an incident: the body reacts before the words (behavioral model:
#       DECIDER_URL, or a fixed stand-in); with QUICK_URL the words are the quick model's line, at once
#   neural: a one-handed bandit fights the hero under the P30 body pilot through the full lease cycle;
#       requires GOTHIC_BODY_URL, a new game, >=10 decisions, no fallback, service p95 <=20 ms
#   neural-companion: the bandit attacks the hero's swordsman companion, >=6 decisions, no fallback,
#       a companion target in the fight summary, natural fight end and confirmed release
#   neural-group: three one-handed bandits fight the hero and his swordsman companion under P30 at once;
#       each gets >=10 decisions, no fallback, release <=2 s, service p95 <=20 ms; requires GOTHIC_BODY_URL
#   neural-talk: a non-bandit swordsman attacks after words (AR_NONE), knocks the hero out alive, releases
#       P30 and returns to his routine; the hero gets up. Then a hostile bandit gives a ten-second
#       ultimatum, attacks after its expiry, loses the
#       fleeing hero, returns to his routine, reattacks or talks again and starts a fresh fight. After
#       return_to_second_fight, >=20 decisions or 20 s trigger another hero flight; fight_end_clean checks
#       target_lost/npc_unavailable, release <=2 s and no lease/body/watch after 2 s; LLM=fake, GOTHIC_BODY_URL
#       one hero hit breaks 15 s without target damage; stall_broken reports count and seconds as P30
#       diagnostics. Only the separate knockout requires hero_down within 30 s after that hit;
#       wildlife is cleared and the swordsman parked away
#   neural-load: load an initial save while three bandits hold P30 leases; all fights end with loading,
#       release their leases and watch, then one bandit fights under P30 again; requires GOTHIC_BODY_URL
#   neural-soak: bandit waves in the ordinary new-game world with a swordsman companion; LLM=fake,
#       GOTHIC_BODY_URL, GOTHIC_SOAK_SECONDS=1800 by default; TIMEOUT=2400 for the full run.
#       GOTHIC_SOAK_CONTROL=1 uses native combat with --no-neural-body and needs no body URL.
#       Samples in neural_soak.jsonl; records 25 s of the first wave when normal-clock bench is available
#   rescue: the hero next to the shepherd Maleth, a wolf attacks him: the body reacts (a call for help
#       runs from the wolf), words follow on the move; the hero kills the wolf (monster_killed): the shepherd
#       reacts and speaks again (the live models with DECIDER_URL and LLM, or a fixed stand-in and scripted
#       words). rescue_self: the shepherd kills the wolf himself and says a word about it
#   wolf: wolves hear the hero (docs/beasts.md) on the road below Xardas's tower: a wolf growls at him, waits
#       for his words while he holds the talk key, is soothed, sits, lets him pass and be; hostility, «Стой!»,
#       walking right up to a calm one, a threat, a failed roll, and one talked down mid-bite (the tone is the
#       quick model's with QUICK_URL, the rules' else; the dice are the scenario's)
#   wolf_load: rewind a save by 30 game seconds, then young wolves (growling and biting), wargs,
#       ice wolves and black wolves hear the hero and calm down; tone as in wolf, fixed scenario dice
#   orders: immediate companion orders, then Cavalorn hears «Бей волка!» with no focus: he fights the
#       visible wolf before it attacks the hero; the wolf never hears the phrase or asks its tone model.
#       Scripted with LLM=fake; TIMEOUT=360
#   m5e: talking without a window: a typed line and the talk key (a wav through the ASR service) with the
#       hero's own focus, the addressee, the npc's body, the hero's doings, an interruption, a witness, the end
#       of a conversation when the hero walks away, a walking npc answering on the move
#   m6: npc's know the world from the scripts' dialogs as far as the story has got: Xardas's talk of chapter 1
#       but not chapter 3, a dialog played from the menu becomes one the hero has heard, chapter 3 opens its
#       talk; Lobart knows his field work and what farmers say; the knowledge is in the model's request and
#       recall gives a topic in full (model scripted with LLM=fake)
#   see: what npc's perceive (npc_view, v11): Xardas sees the hero in front of him and not through the tower's
#       walls; Lobart names whom he knows and tells the hero by his look, in militia armor a militiaman; the
#       view is in the model's request, `look` answers, the views of the nearest npc's are pushed each second;
#       a wolf killed in front of Lobart is a dead wolf to him; places by name at Xardas's tower, Lobart's
#       and Onar's farms and in the city
#   seen: what npc's saw (v12): Lobart keeps when, where and how he saw the hero on his farm; the hero goes to
#       the city (Harad sees him), an hour passes, he comes back and says he was in the city all morning:
#       Lobart catches the lie by what he saw (model scripted with LLM=fake); the memory follows the save;
#       story_state's `seen` (check_claim) is the engine's sight
#   trace: Lobart catches a lie about an arrow's age by its fresh trace; a stranger's arrow and denied authorship are the control
#   broken: Lobart returns to fresh fence debris and catches a lie about its age; another NPC's sword break is the control
#   witness: asking a witness (wave 2 step 3): at 8:00 someone Lobart knows sees the hero in the open, Lobart
#       does not; later the hero comes up to Lobart with an alibi («это не я топтал твою репу — я всё утро был
#       в городе»), Lobart calls that one over, it comes and tells what it saw, Lobart catches the lie «со слов»
#       (model scripted with LLM=fake; live, the hero bluffs «спроси у …» if nobody is asked); then someone far
#       is asked at their next meeting: the answer is a fact, no model call
#   places: not a check: dumps the world's waypoints and its people's routines into $LOG_DIR/world_points.json
#       for the book of places (gothic-ai places <dump>, server/src/gothic_ai/places.py)
#   tts: npc lines reach the game with audio from the TTS service (TTS=fake|voxcpm|inworld) and play aloud;
#       TTS=inworld is paid, so it also needs PAID_TTS=1
#       GOTHIC_TTS_RECORD=1 also records the reply and its bench events for check-voice-stem.sh
#   lie: the hero lies to a quest giver (Xardas: two sheep to kill) and is paid; a sheep he did not kill
#        turns up, Xardas finds out, attacks him through the scripts, and Jora, standing by, hears it as a
#        rumor and has it in mind when the hero speaks to her (story graph, stage 4)
#   death_load: human deaths before/after older-save load, player credit, and no repeated saved deaths; LLM=fake, no SAVE.
#   repair: Lobart gives the hero the turnip quest and dies; the director finds the quest without an ending,
#       its model proposes a repair (scripted with LLM=fake: first the dead Lobart, rejected by the validator,
#       then Hilda), and the hero ends the quest with Hilda for Lobart's reward (story graph, stage 5)
#   authored: the director makes Hilda a quest of its own (docs/director.md, stage 1; model scripted with
#       LLM=fake): a thing she lost lies near the farm; the hero takes the quest, finds the thing, saves and
#       loads, brings it and is paid; its variable DIR_Q1 lives in the save (protocol v29). On a save of
#       Lobart's farm: SAVE=save_slot_1.sav
#   own: script symbols of our own (docs/director.md, stage 2, layer 1): a story variable, an npc of the
#       director's made like a nameless farmer, another variable; two saves and loads keep them all, the npc
#       bound to its own symbol, the farmer and Hilda untouched (protocol v33: debug own_npc). On a save of
#       Lobart's farm: SAVE=save_slot_1.sav
#   cast: a character of the director's (docs/director.md, stage 2, layer 2): a peddler at Lobart's farm made
#       like a nameless townsman, with his own name, guild, clothes, things and day; a description with a plot
#       person as template is refused; saved and loaded he is the same, a save from before he came has none
#       (protocol v34: story_npc). On a save of Lobart's farm: SAVE=save_slot_1.sav
#   person: the peddler of `cast` as a person (docs/director.md, stage 2, layer 3): his card, past, knowledge
#       and acquaintances in front of his model when the hero speaks to him, a voice of his own; Hilda knows
#       him by name; the same after a save and a load. On a save of Lobart's farm: SAVE=save_slot_1.sav
#   character: the director brings in a character of its own (docs/director.md, stage 2, layer 4; models
#       scripted with LLM=fake): its model makes him up for the place of Lobart's day, the code builds and checks
#       him, and he gives the hero the quest of stage 1 (the checks of `authored`, him as the giver). On a
#       save of Lobart's farm: SAVE=save_slot_1.sav
#   trigger: the director's triggers in the bridge (docs/director.md, stage 3, layer 1): one for every event —
#       a thing put near the hero at once, his having it, a place he comes into again and again, a dialog of
#       the director's, the hour, the death of an npc of the director's he kills; a save and a load in the
#       middle keep the counts and fire nothing again (protocol v35: graph_patch trigger, trigger_fired). On a
#       save (a new game opens with Xardas's own dialog): SAVE=save_slot_1.sav
#   stages: a quest of the director's as an object (docs/director.md, stage 3, layer 2): Hilda's quest of three
#       stages written in the scenario — come to a place, get a thing put at another, bring it to her; the book
#       checks and builds it of vars, dialogs and triggers; a save and a load at stage 2 keep it. On a save:
#       SAVE=save_slot_1.sav
#   own_amend: Hilda's three-stage quest from `stages`; at the hero's "не нужно никуда идти", her scripted
#       model calls amend_quest to waive stage 1's destination without a visit. Stage 2 starts, terms survive
#       save/load, and the normal item pickup and turn-in finish with quest_outcome amended/full.
#       LLM=fake; SAVE=save_slot_1.sav
#   crimes: hero deeds and engine witnesses (docs/crime.md, layer 1), LLM=fake on Lobart's farm:
#       SAVE=save_slot_1.sav; TIMEOUT=420. Theft, chest, unowned item, attack, sheep, both pocket
#       outcomes, then save/load and a clean branch. Native script reactions remain enabled.
#   crime_talk: crime knowledge and speech (docs/crime.md, layer 2), on Lobart's farm/save_slot_1.sav.
#       LLM=fake: Hilda's theft rumor, testimony/catch_lie, accepted silence, chest suspicion/concern.
#       LLM=chatgpt: only the first scene; records Lobart's actual prompt, reply and catch_lie outcome.
#       TIMEOUT=600. Native theft perceptions stay enabled; Hilda's fight is reset after her sighting.
#   trace_crime: one reply catches lies by fresh fence debris and Hilda's theft testimony, then save/load.
#       LLM=fake; SAVE=save_slot_1.sav. Native theft perceptions stay enabled; checks mixed evidence,
#       both accepted catch_lie calls, crime knowledge and trust without treating debris as authorship proof.
#   cover: Hilda sees a real theft, sells cover through the deal key, and lies to Lobart. Her С row cannot
#       catch the hero; Vino's real sighting exposes her. Save/load keeps the cover and one exposed fact.
#       LLM=fake; SAVE=save_slot_1.sav; TIMEOUT=720. LLM=chatgpt runs only the one cover request to Hilda,
#       logs her exact reply and proposal/refusal, then exits. Fear is checked by the server unit tests.
#   cover_subject: the same native cover/exposure, then Lobart's cover rumor reaches an acquaintance
#       shared with Hilda (prefer Vatras). Its model reads the people block and both witness warnings;
#       hero trust is unchanged by the rumor. Save/load restores knowledge and the rumor once.
#       LLM=fake; SAVE=save_slot_1.sav; TIMEOUT=720.
#   cheat: Hilda sells silence after a native theft but tells Lobart; dishonest Lobart keeps a later
#       payment, Vino hears, then honesty 4 pays. Save/load preserves intentions and single outcomes.
#       LLM=fake; SAVE=save_slot_1.sav; TIMEOUT=720. LLM=chatgpt scripts only the later-payment setup,
#       records one ordinary real-model refusal response verbatim, then exits.
#   npc_will: character rules start theft, reclamation and accusations near the hero; save/load cooldown;
#       LLM=fake, SAVE=save_slot_1.sav. The scene never commands harm.
#   npc_far: natural farm routines and greedy cards start a real off-screen chest transfer; the owner
#       opens his chest and accuses off screen. Witness truth, save/load and cooldown are checked;
#       Lobart's GIL_OUT farm has no community law, so guards are covered by module tests.
#       LLM=fake; SAVE=save_slot_1.sav; TIMEOUT=720; the scene never commands harm.
#   npc_deeds: Vino steals from CHEST_LOBART in front of Hilda, assaults Maleth to a living knockout,
#       then steals a world item before brave Lobart, who defeats him and reclaims it. NPC knowledge,
#       its rumor and save/load are checked; guards are covered by module tests.
#       LLM=fake; SAVE=save_slot_1.sav; TIMEOUT=720; no live model.
#   npc_director: Vino steals Lobart's chest item before Hilda; the deed becomes Lobart's line to
#       find the thief, his ask_witness/about and blame finish its goal, then reward and save/load.
#       LLM=fake; SAVE=save_slot_1.sav; TIMEOUT=720. LLM=chatgpt asks only the ordinary showrunner
#       once on the same deed, records its line premise or none reason and canon, without building.
#   frame: unseen hero chest theft, Lobart believes Malak guilty, native reclaim combat finds no goods
#       and reveals the hero's lie; Hilda's honest testimony about Vino leads to true blame and return.
#       Save/load restores both cases without duplicates. LLM=fake; SAVE=save_slot_1.sav; TIMEOUT=900.
#       LLM=chatgpt stops after Lobart's one ordinary answer to «Это Малак взял», belief or refusal.
#   crime_director: deeds and story responses (docs/crime.md, layer 5), SAVE=save_slot_1.sav.
#       LLM=fake: unseen chest theft and its return quest, Hilda's initiative and paid silence,
#       Lobart's learned grievance and Amends payment. LLM=chatgpt: witnessed theft goes to the
#       real showrunner; records its line premise or none reason. TIMEOUT=900.
#   crime_law: community law (docs/crime.md, layer 3), LLM=fake; SAVE=save_slot_1.sav, TIMEOUT=420.
#       Matteo witnesses a native theft; Andre stays locked until the next day, charges 50 through
#       his native voiced dialog and accepts payment. Matteo still remembers. A second theft with
#       an accepted silence deal never reaches Andre. Native theft perceptions stay enabled.
#   gate: new game, LLM=fake: the city guard sells passage for 100 gold through Y; both gate guards
#       get AIV_PASSGATE and the hero crosses without SECONDWARN/ATTACK. Loading the outside save,
#       a believed let_pass claim grants passage and records its text, reference and game time.
#   gate_vouch: new game, LLM=fake: nearby Lothar vouches and the guard admits the hero; a loaded
#       branch admits a real pass without a native menu or lines. Another loaded branch believes
#       a false Harad claim; the next morning catches it and revokes both guards' passage.
#   crime_react: rule reactions at the deed (docs/crime.md, layer 4a), LLM=fake; SAVE=save_slot_1.sav.
#       Hilda's retained theft witness and own SVM shout; Lobart courage 2 demands restitution, then
#       a temporary scenario card with courage 3 fights through native AR_THEFT to a living knockout
#       and the cup's return. Finally deeds:false leaves theft to vanilla without a held perception.
#       Community guards are covered by unit tests. TIMEOUT=420; no behavioral model.
#   requires: a book quest depending on vanilla (docs/director.md, stage 5, layer 2), no model: Hilda's
#       offer stays absent while Lobart's turnip quest is open or running, opens after its success, and
#       the book's second stage waits for "Сковорода для Хильды" to become running. A save and a load
#       keep that stage and its unfired vanilla trigger; Hilda's native shopping dialog gives the pan
#       quest, advancing the book to its final talk and reward. On a save: SAVE=save_slot_1.sav; LLM=fake
#   own_repair: the director's own quest repaired (docs/director.md, stage 3, layer 3): Hilda gives the
#       three-stage quest of `stages`; after the hero reaches the farmyard she dies, the director passes her
#       turn-in to Lobart (scripted: first the dead Hilda is refused), keeping its id and required effects
#       and reducing the reward; a save and a load keep it, the hero brings the broom to Lobart, who takes it
#       and pays; the book follows the quest to success and Lobart knows he paid. On a save:
#       SAVE=save_slot_1.sav; LLM=fake
#   chain: Garet's linked quests (docs/director.md, stage 3, layer 4): its model proposes the cast and two
#       quests together (scripted: stolen merchandise held by a thief, then three bandits in a camp; the
#       first proposal's farmyard camp is refused). The book introduces the chain atomically; a general
#       player follows its goals, checks the next quest stays locked until turn-in, saves and loads after
#       the first bandit's death, finishes both quests, and checks Garet knows. LLM=chatgpt uses the same
#       player for the model's own chain. On a save: SAVE=save_slot_1.sav
#   occasions: the director's occasions (docs/director.md, stage 4, layer 1), no model: the hero settles
#       in a new inhabited place (one arrived), leaves and returns (none), saves and loads (the same
#       visited places, empty queue, no repeat); KAPITEL rises (chapter), the hero kills a living human
#       bandit (deed), and the clock passes a full game day (quiet). Uses the real 5 s sampling and 10 s
#       settling, normal story_eval for the nearest waypoint. On a save: SAVE=save_slot_1.sav; LLM=fake
#   questlog: vanilla quest progress (docs/director.md, stage 5, layer 1), no model: Lobart's actual
#       work offer and native answer start "Убрать поле репы" (open -> running, dialog node and npc,
#       vanilla occasion); story_effect finishes it and the real 30 s poll records running -> success
#       without a dialog node. A save and a load repeat no facts, including the next periodic sample.
#       On a save: SAVE=save_slot_1.sav; LLM=fake; TIMEOUT=300
#   showrunner: the director's decision (docs/director.md, stage 4, layer 2), LLM=fake only: a real
#       arrived at a new inhabited place produces a quest from an eligible giver; a plot giver is refused
#       first, the second proposal and the builder receive the premise. Another occasion that day is
#       skipped before the model; after a game day and a simulated 15 min, none is recorded. Save/load
#       keeps the lines and budget. On a save: SAVE=save_slot_1.sav; no ordinary background loop
#   showrunner_live: LLM=chatgpt only, stage 4 layer 3: one saved game, real arrived, human-kill deed and
#       chapter occasions consumed by Showrunner.run; every line is finished by play_book_chain before
#       the next occasion, with a game day and an injected 15 min between them. All three must have a
#       line/none outcome, at least one completed line. SAVE=save_slot_1.sav; TIMEOUT=1200
#   greg: LLM=fake, new game: Greg gives his city errand, accepts farmer clothes and pays 50 gold, a map
#       and two rum; reload the branch without the clothes and his acceptance records a lie with its truth
#   vanilla_line: LLM=fake only, stage 5 layer 3: take and finish Lobart's turnips through the scripts'
#       own dialogs. The vanilla success occasion produces a neighbor's line with follows and a success
#       requirement; its book offer is open, play_book_chain completes it, and save/load keeps the same
#       line and quest fields. SAVE=save_slot_1.sav; TIMEOUT=420
#   vanilla_line_live: LLM=chatgpt only: Showrunner.run consumes two real vanilla occasions, taking and
#       completing Lobart's turnips. Finish the first line, if any, before the second event, then advance
#       a game day and the budget's wall clock by 15 min. Both events need a line/none outcome, at least
#       one line, all quests completed; givers must be peaceful and outside their follows quest's cast.
#       Logs include each event, proposal outcome, built quests and passage. SAVE=save_slot_1.sav; TIMEOUT=1200
#   vgive: a quest of the scripts given and ended in a conversation (Cavalorn's bandits; model scripted with
#       LLM=fake): he gives it as his dialog HELFEN does, the bandits die, the hero says so and the scripts'
#       ending plays (the letter to Vatras, +100 exp); a new game (protocol v31: story_effect knows)
#   vspare: with broken trust, Cavalorn considers sparing the last bandit through amend_quest; after
#       loading, he credits all three witnessed deaths away from the bodies without check_claim.
#       New game, LLM=fake; LLM=chatgpt makes only Cavalorn live and logs his choices; TIMEOUT=900
#   vlie: a lie about a quest of the scripts: the hero tells Lobart the turnips are in the barn, Lobart's
#       model (scripted with LLM=fake) believes him and his own ending plays; the next morning he misses the
#       turnips, turns against the hero, and the people around hear it (story graph, stage 5)
#   vcheck: lies as a mechanic: the hero tells Lobart he killed the raiders on his field; Lobart goes to see
#       for himself, sees them alive and catches the lie; then he takes no lie on the hero's word; once the
#       raiders are really dead he goes to see again and pays (model scripted with LLM=fake; stage 5)
#   rumor: Lobart catches the hero lying; the clock goes to the next morning and the rumor goes round the
#       people that know each other (routines, shared quests) as far as the city; one there has it in mind
#       when the hero speaks to it (stage 5)
#   amends: the director's storylet: Lobart catches the hero lying, the rumor reaches Andre, who asks for a
#       fine and closes joining the militia until then (model scripted with LLM=fake); the hero hears him
#       out and pays, and the militia is open again (stage 5)
#   amends_deal: as amends, but the hero makes them in conversation: he asks to be forgiven, the npc offers a
#       deal that forgives the lie for the fine, and he accepts it (docs/npc-tools.md, wave 1 step 2)
#   debt: Lobart lends the hero 20 gold for 25 back by the end of the next day; two days on the debt is a
#       broken word (trust) and goes round as a rumor; then the hero pays it back, late (wave 1 step 2)
#   wrath: the necromancer's answer (docs/xardas-wrath.md): the hero insults Xardas line by line; the scales of
#       his patience tip (model scripted with LLM=fake: rude, an insult, his mother), he kills the hero with a
#       spell and raises him as a skeleton. LLM=chatgpt: whether the real model tips over (GOTHIC_WRATH_LINES:
#       other lines, "|" between them)
#   initiative: an npc comes up to the hero and speaks first (docs/npc-tools.md, wave 2 step 1). Hilda's
#       greeting waits for the hero (the game's Npc_CheckInfo), so she is left to the scripts. The hero owes
#       Lobart 25 gold since yesterday (a debt seeded into the chronicle): a broken word, on Lobart's mind. The
#       hero stands 12 m in front of him; Lobart walks up once by himself and speaks first about the debt (the
#       fact `initiative`), the hero's next words without a name go to him, Lobart offers a deal that settles
#       the debt, the hero accepts it and nothing is on Lobart's mind any more. Without DECIDER_URL a fixed
#       stand-in decides for Lobart only; with it the behavioral model decides (give a longer TIMEOUT).
#       Words scripted with LLM=fake. Run on a save: SAVE=save_slot_1.sav
#   gossip: two people near the hero talk about his lie, in their own voices (docs/npc-tools.md, wave 2
#       step 2). Lobart catches the hero lying, Hilda in the house does not hear it; an hour later she is put
#       5 m from Lobart, the hero 13 m off, and the pair is opened: the rumor passes as one fact (how: talked),
#       then Lobart turns to her and they say three lines in turn, each to the other, one voice after the
#       other (game.log); from a save the scene again: the hero's voice cuts both after the second line, he
#       asks Hilda and she has it in mind; far from the hero a pass is only the fact. Starts the TTS service
#       (TTS=fake). Words scripted with LLM=fake. Without DECIDER_URL (with it Lobart may first come up about
#       the lie himself); TIMEOUT=420
#   day: everyday life from concerns (docs/npc-tools.md, wave 2 step 6). Lobart catches the hero lying; Vino,
#       on his field, hears it from him far from the hero. With the hero by Malet, Vino's menu of his next hour
#       has a walk to Malet to tell the news (about an hour); it is chosen and Vino walks there: near the hero
#       the two say it in a scene of step 2 (how: talked) and the walk leaves a `day` fact the hero saw; from a
#       save, with the hero by Vino's field, the same walk ends far from him and Malet learns it as a fact
#       (how: visited), no model, no words. No sleep or dead option in any menu, few routine asks. Without
#       DECIDER_URL the scenario's own planner (a fixed stand-in); with it the server's planner, the choice
#       pinned and the live model's own answer logged. Starts the TTS service (TTS=fake). Words scripted with LLM=fake;
#       TIMEOUT=480
#   noise: a real physical impact near a free human at Lobart's farm: noise event, planner decision,
#       investigate receipt, walk within 2 m, idle within 45 s, return within 1.5 m of the routine post,
#       and heard-noise memory
#       with place and game time. Without DECIDER_URL a fixed stand-in chooses investigate for this NPC;
#       with it the server's planner logs the live choice and pins investigate. SAVE=save_slot_1.sav
#   cards: the model writes character cards (docs/npc-tools.md, wave 1 step 3) for every npc of the world with
#       lines of its own (GOTHIC_CARDS=SYM,...: those only) into server/src/gothic_ai/gamedata/characters.json;
#       those with a card are skipped; needs LLM=chatgpt and a long TIMEOUT for all of them
#   haggle: live only (LLM=chatgpt): the hero offers the same silver cup to Canthar (greedy by his card) and
#       Lutero (fairer) and asks for more: the greedy one should offer less (wave 1 step 3)
#   deal: the hero asks Lobart to sell him something; Lobart offers it for 50 gold (model scripted with
#       LLM=fake): Y waits without the gold; the hero declines (N), Lobart offers it for 40, then 30 after
#       haggling: the deal window shows the new version, Y waits right after the change, then the deal is
#       done and Lobart answers (docs/npc-tools.md, wave 1)
#   xardas_money: new game, no SAVE; LLM=fake: Xardas gives 30 gold, lends 150 for three days, refuses
#       another gift of 100; LLM=chatgpt: no gold offer for 20 s unasked, then the hero asks for 200 and
#       accepts Xardas's chosen terms; the result includes his words and the deal's terms
#   graph: the story graph decides which dialogs an npc offers (docs/story-graph.md, stage 3): switching a
#       dialog off, adding a condition and a dialog of the director's own, keeping that across save/load;
#       the game checks every script condition against its lifted form ("[story] check" in game.log)
#   ground: Cavalorn follows the hero, the hero throws his letter down: Cavalorn sees it lying there, picks it
#       up when told «бери» (take_item, model scripted with LLM=fake) and follows the hero again (protocol v22)
#   group: one word at a time (docs/npc-tools.md, step B2): «Пацаны, здорово!» at the bandits' cave while
#       Cavalorn is the current conversation partner: Brago answers first, his line is heard in the game,
#       the others' words wait their turn and are heard
#       after his, the second knows what he said, a fourth voice says nothing. Starts the TTS service
#       (TTS=fake); models scripted with LLM=fake
#   interject: who speaks up, by character (docs/npc-tools.md, step B3), live only (LLM=chatgpt and
#       DECIDER_URL): at the bandits' cave the hero says three phrases, to Brago, to his mate, to them all;
#       no more than three voices to each, none over another, someone speaks up by the behavioral model's
#       word; what each said is in the log, to be read. Starts the TTS service (TTS=fake)
#   remembers: seen lately (docs/npc-tools.md, step M2): Cavalorn goes into the bandits' cave first, sees them,
#       comes back out; «там в пещере бандиты, разберись» (model scripted with LLM=fake): it remembers them
#       though it sees them no more, walks into the cave first and fights Brago
#   fight_reactions_wolf/sight/bandits/flee: Cavalorn intercepts a hunting wolf before its first bite;
#       sees a bloodfly at 30 m; charges two bandits (one held listening), which shout and draw before
#       contact and strike within 3 s; a coward flees Cavalorn while the hero stays passive. Actual
#       strike/damage timestamps come from body_watch. Models scripted by default, DECIDER_URL optional;
#       starts TTS (TTS=fake), TIMEOUT=300 for bandits, 180 for the other cases
#   cave: the map of places (docs/npc-tools.md, step M3): Cavalorn goes with the hero to the steps below
#       Xardas's tower; the bandits' cave is among the places around him, a cave entered from the steps, and
#       `where{пещера}` finds it; «Кавалорн, зайди в пещеру.» with no place named takes him into it (model
#       scripted with LLM=fake; LLM=chatgpt: the model chooses the cave itself)
#   knows: what an npc knows of where people are (docs/npc-tools.md, step M4): Cavalorn by his fire knows the
#       bandits who robbed him sit in the cave below the steps, though the hero told him nothing: the fact is in
#       his turn's situation and in `where{пещера}`; LLM=chatgpt: his own answer to «что тут поблизости опасного?»
#   lead: the npc leads the hero (docs/npc-tools.md, wave 2 step 4): «Малет, отведи меня к мельнице» — a deal
#       with the way in its window, Y, Malet sets off; the hero put back where it started: it waits and goes on
#       once he comes up; at the mill it says so, a fact `led` (model scripted with LLM=fake); TIMEOUT=540
#   meet: a meeting (docs/npc-tools.md, wave 2 step 4): at 19:00 the hero and Malet agree to meet at dawn by
#       the field in front of the city gates (a deal, Y); the clock leaps past when Malet sets off, as a sleep:
#       he is there at once and waits; the hero comes, `meet_kept`, Malet speaks first (model scripted with
#       LLM=fake); TIMEOUT=600
#   meet_missed: the clock to a little before he sets off, Malet walks there himself and waits; the hero does
#       not come: the clock past the end, his word broken (`broken`, promise meet), Malet back to his routine,
#       and when the hero comes up the missed meeting is before him; TIMEOUT=700
#   errand: an errand (docs/npc-tools.md, wave 2 step 5, `plan`): Lobart catches the hero lying and, asked
#       «ты кому-нибудь расскажешь?», sets out to tell Andre tomorrow at 7:00; the night goes by (a leap), the
#       hero waits where Andre will be; Lobart walks there from the farm, finds Andre and tells him in front of
#       the hero: a rumor with `how: told`, `goal_done`, a scene of two voices. Starts the TTS service
#       (TTS=fake); model scripted with LLM=fake (LLM=chatgpt: Lobart decides himself whom he tells and
#       when, or not to); TIMEOUT=1500 (the walk is ~8 min of real time)
#   errand_fork: the same, but Andre is taken away before Lobart comes: another of the militia there hears it
#   errand_far: the same, the hero at the tower: nothing walks, the rumor comes as a fact when Lobart would be
#       there, his body stays at his routine, no model call
#   errand_give: Hilda takes Lobart 25 gold the hero asks her to (`npc_act give`): her −25, his +25, a fact
#       `gave`; Canthar's letter (a mission item, v39) is turned down with the reason
#   programs: the director's programs (docs/story-graph.md, «Исполнение без VM»): a dialog's program read
#       and given back, the director's in place of an information (a line of its own, gold, experience) and of
#       a condition, the scripts' one back, a bad one rejected, a dialog of the director with a program, its
#       choices (v24) through the debug commands and through the dialog window as the player has it, and a
#       program kept across save/load; the director's own lines in the npc's voice (v26). Starts the TTS
#       service (TTS=fake)
# A new game starts without the intro (`-nointro`): no videos, and the important dialogs of those around
# the hero count as told, so Xardas does not stop him for his first talk and its addon intro video (over a
# minute). GOTHIC_INTRO=1 brings both back. SAVE=<file.sav> loads that save instead (copied into LOG_DIR as
# slot 1). The game runs in LOG_DIR,
# so its saves and settings stay there; memories go to LOG_DIR/memory.sqlite3.
#
#   SCENARIO=m3 LLM=fake ASR=voxtral GOTHIC_DIR=/path/to/Gothic\ II scripts/e2e.sh
# GAME_EXTRA_ENV adds game-only environment assignments, one NAME=value per line.
# Example: GAME_EXTRA_ENV=$'LD_PRELOAD=/path/libjemalloc.so.2\nMALLOC_CONF=prof:true'
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GOTHIC_DIR="${GOTHIC_DIR:-/Volumes/DevSSD/Dev/games/Gothic II}"
GAME_BIN="${GAME_BIN:-$ROOT/OpenGothic/build/opengothic/Gothic2Notr}"
LOG_DIR="${LOG_DIR:-$(mktemp -d "${TMPDIR:-/tmp}/gothic-e2e.XXXXXX")}"
SCENARIO="${SCENARIO:-m0}"
BODY_ARGS=(--neural-body)
if [[ "${GOTHIC_SOAK_CONTROL:-0}" == 1 && "$SCENARIO" != neural-soak ]]; then
  echo 'GOTHIC_SOAK_CONTROL requires SCENARIO=neural-soak' >&2
  exit 2
fi
E2E_DEFAULT_TIMEOUT=240
if [[ "$SCENARIO" == memprobe ]]; then E2E_DEFAULT_TIMEOUT=1200; fi
TIMEOUT="${TIMEOUT:-$E2E_DEFAULT_TIMEOUT}"
if [[ "${GOTHIC_TTS_RECORD:-0}" == 1 && "$SCENARIO" != tts ]]; then
  echo 'GOTHIC_TTS_RECORD requires SCENARIO=tts' >&2
  exit 2
fi
if [[ "$SCENARIO" = neural || "$SCENARIO" = neural-companion || "$SCENARIO" = neural-group || "$SCENARIO" = neural-talk || "$SCENARIO" = neural-load || "$SCENARIO" = neural-soak ]]; then
  if [[ "$SCENARIO" == neural-soak && "${GOTHIC_SOAK_CONTROL:-0}" == 1 ]]; then
    BODY_ARGS=(--no-neural-body)
  else
    [ -n "${GOTHIC_BODY_URL:-}" ] || { echo "scenario $SCENARIO requires GOTHIC_BODY_URL=tcp://host:port" >&2; exit 2; }
  fi
  [ -z "${SAVE:-}" ] || { echo "scenario $SCENARIO requires a new game (unset SAVE)" >&2; exit 2; }
fi
LLM="${LLM:-fake}"
if [[ ( "$SCENARIO" == bandit_parley || "$SCENARIO" == bandit_bounty || "$SCENARIO" == bandit_hold || "$SCENARIO" == death_load ) && ( "$LLM" != fake || -n "${SAVE:-}" ) ]]; then
  echo "scenario $SCENARIO requires LLM=fake and a new game (unset SAVE)" >&2
  exit 2
fi
if [[ "$SCENARIO" == neural-soak && "$LLM" != fake ]]; then
  echo 'scenario neural-soak requires LLM=fake' >&2
  exit 2
fi
ASR="${ASR:-fake}"
# Inworld bills every recognized line, the same as its TTS below.
if [[ ( "$ASR" == inworld || "$ASR" == inworld:* ) && "${PAID_ASR:-0}" != 1 ]]; then
  echo "ASR=$ASR is paid Inworld; use ASR=fake or ASR=voxtral, or set PAID_ASR=1 when a step needs its recognition" >&2
  exit 2
fi
# own ports, so a server / ASR service running for a play session is left alone
PORT="${PORT:-47827}"
ASR_PORT="${ASR_PORT:-47829}"
TTS_PORT="${TTS_PORT:-47840}"
TTS="${TTS:-fake}"
# Inworld bills every line. Tests check our code, not its voices: fake or voxcpm does that for free.
if [[ ( "$TTS" == inworld || "$TTS" == inworld:* ) && "${PAID_TTS:-0}" != 1 ]]; then
  echo "TTS=$TTS is paid Inworld; use TTS=fake or TTS=voxcpm, or set PAID_TTS=1 when a step needs its voices" >&2
  exit 2
fi
# Inworld writes its voice cache through one fixed .tmp file. Keep that write
# inside this Ryzen run too; HOME still supplies credentials and read-only data.
if [[ "${GQ_PROFILE:-}" == ryzen && ( "$TTS" == inworld || "$TTS" == inworld:* ) &&
      ! "${TTS#*:}" =~ (^|,)[[:space:]]*cache[[:space:]]*= ]]; then
  if [[ -f "$HOME/.local/share/gothic-ai/inworld-voices.json" ]]; then
    cp "$HOME/.local/share/gothic-ai/inworld-voices.json" "$LOG_DIR/inworld-voices.json"
  fi
  if [[ "$TTS" == inworld || "$TTS" == inworld: ]]; then
    TTS="inworld:cache=$LOG_DIR/inworld-voices.json"
  else
    TTS+=",cache=$LOG_DIR/inworld-voices.json"
  fi
fi
VOICE_FILE="${VOICE_FILE:-$ROOT/asr-service/tests/audio/ru_1.wav}"
# the npc planner is off unless a decision service is given (DECIDER_URL=ws://<gpu-host>:47832)
BRAIN_ARGS=(--no-brain)
if [ -n "${DECIDER_URL:-}" ]; then
  BRAIN_ARGS=(--decider-url "$DECIDER_URL")
fi
# lines in a fight come from the quick model only when given (QUICK_URL=http://<gpu-host>:47834/v1; m5)
QUICK_ARGS=(--no-quick)
if [ -n "${QUICK_URL:-}" ]; then
  QUICK_ARGS=(--quick-url "$QUICK_URL")
fi
SVM_ARGS=()
if [[ "${GOTHIC_NO_SVM_LINES:-0}" == 1 ]]; then
  SVM_ARGS=(--no-svm-lines)
fi

ASR_PID=""
TTS_PID=""
VOICE_ARGS=(--no-voice)
TTS_ARGS=(--no-tts)
if [ "$SCENARIO" = "tts" ] || [ "$SCENARIO" = "svm_lines" ] || [ "$SCENARIO" = "combat_barks" ] || [ "$SCENARIO" = "gossip" ] || [ "$SCENARIO" = "day" ] || [ "$SCENARIO" = "group" ] || [ "$SCENARIO" = "interject" ] || [ "$SCENARIO" = "programs" ] || [ "${SCENARIO#errand}" != "$SCENARIO" ] || [ "${SCENARIO#fight_reactions_}" != "$SCENARIO" ]; then
  (cd "$ROOT/tts-service" && exec uv run gothic-tts serve --port "$TTS_PORT" --provider "$TTS" --fallback none) >"$LOG_DIR/tts.log" 2>&1 &
  TTS_PID=$!
  TTS_ARGS=(--tts-url "ws://127.0.0.1:$TTS_PORT")
fi
if [ "$SCENARIO" = "m3" ] || [ "$SCENARIO" = "m5e" ]; then
  (cd "$ROOT/asr-service" && exec uv run gothic-asr serve --port "$ASR_PORT" --provider "$ASR" --fallback none) >"$LOG_DIR/asr.log" 2>&1 &
  ASR_PID=$!
  VOICE_ARGS=(--asr-url "ws://127.0.0.1:$ASR_PORT" --voice-file "$VOICE_FILE")
fi

(cd "$ROOT/server" && exec uv run gothic-ai serve --port "$PORT" --scenario "$SCENARIO" --llm "$LLM" --exit-after --no-console \
  --memory-db "$LOG_DIR/memory.sqlite3" --trace-dir "$LOG_DIR/traces" "${VOICE_ARGS[@]}" "${TTS_ARGS[@]}" "${BRAIN_ARGS[@]}" "${QUICK_ARGS[@]}" ${SVM_ARGS[@]+"${SVM_ARGS[@]}"} "${BODY_ARGS[@]}") >"$LOG_DIR/server.log" 2>&1 &
SRV=$!
GAME=""

report_game_exit() {
  local game_rc signal_number signal_name
  [[ -n "$GAME" ]] || return 0
  wait "$GAME" && game_rc=0 || game_rc=$?
  if (( game_rc > 128 )) && signal_name="$(kill -l "$((game_rc - 128))" 2>/dev/null)"; then
    signal_number=$((game_rc - 128))
    echo "game exited: signal SIG$signal_name ($signal_number), status $game_rc ($1)"
  else
    echo "game exited: code $game_rc ($1)"
  fi
  if [[ ( "$1" == unexpected || "$1" == exited ) && "$game_rc" == 137 ]] &&
     ! grep -Fq "scenario $SCENARIO: checks started" "$LOG_DIR/server.log"; then
    echo "game killed before checks at $(date -u '+%Y-%m-%dT%H:%M:%SZ')"
  fi
  GAME=""
}

cleanup() {
  local attempt
  # Reap our exact child PID, including a game killed before the first poll.
  # `|| true` preserves the scenario status if a child has already finished.
  if [[ -n "$GAME" ]]; then
    if ! kill -0 "$GAME" 2>/dev/null; then
      # The server may have finished before the poll noticed the dead game.
      report_game_exit exited
    else
      kill "$GAME" 2>/dev/null || true
      # A signal during the fork/exec handoff can miss the game. Bound the wait
      # without touching another run's process or relying on its executable name.
      for attempt in $(seq 1 20); do
        kill -0 "$GAME" 2>/dev/null || break
        sleep 0.1
      done
      kill -KILL "$GAME" 2>/dev/null || true
      report_game_exit cleanup
    fi
  fi
  kill "$SRV" 2>/dev/null || true
  if [[ -n "$ASR_PID" ]]; then kill "$ASR_PID" 2>/dev/null || true; fi
  if [[ -n "$TTS_PID" ]]; then kill "$TTS_PID" 2>/dev/null || true; fi
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP

sleep 2
if ! kill -0 "$SRV" 2>/dev/null; then
  cat "$LOG_DIR/server.log"
  exit 1
fi

# Per-scenario defaults, as scenario:fps. Add only after repeated capped failures
# and an uncapped pass, with the measured reason beside the entry. Explicit
# E2E_MAX_FPS always wins, including 0 for an uncapped diagnostic run.
E2E_FPS_EXCEPTIONS=(
)
if [[ -z "${E2E_MAX_FPS+x}" ]]; then
  E2E_MAX_FPS=30
  # macOS Bash 3 treats an empty array as unset under nounset.
  for exception in ${E2E_FPS_EXCEPTIONS[@]+"${E2E_FPS_EXCEPTIONS[@]}"}; do
    if [[ "${exception%:*}" == "$SCENARIO" ]]; then
      E2E_MAX_FPS="${exception##*:}"
      break
    fi
  done
fi
[[ "$E2E_MAX_FPS" =~ ^[0-9]+$ ]] || { echo 'E2E_MAX_FPS must be a nonnegative integer' >&2; exit 2; }

# Merge run-local settings so ENGINE, character synchronization and TTS recording
# cannot erase each other. A literal 0 also overrides the base Gothic.ini cap.
python3 - "$LOG_DIR/Gothic.ini" "$E2E_MAX_FPS" <<'PY'
import configparser
import os
from pathlib import Path
import sys

path = Path(sys.argv[1])
ini = configparser.ConfigParser(interpolation=None, strict=False)
ini.optionxform = str
ini.read(path)

def put(section, key, value):
    section = next((name for name in ini.sections() if name.lower() == section.lower()), section)
    if not ini.has_section(section):
        ini.add_section(section)
    for name in list(ini[section]):
        if name.lower() == key.lower():
            ini.remove_option(section, name)
    ini.set(section, key, value)

put("ENGINE", "zMaxFps", str(int(sys.argv[2])))
for name in ("CHARACTER_VIRTUAL", "NPC_CHARACTER_VIRTUAL", "MONSTER_CHARACTER_VIRTUAL", "MELEE_BLADE_SHADOW"):
    value = os.environ.get(name, "")
    if value:
        if value not in ("0", "1"):
            print(f"{name} must be 0 or 1", file=sys.stderr)
            sys.exit(2)
        put("GAME", name.lower(), value)
if os.environ.get("GOTHIC_TTS_RECORD", "0") == "1":
    # Keep effects audible, with no music masking the controlled fake voice.
    put("INTERNAL", "vidResIndex", "0")
    put("SOUND", "soundVolume", "1")
    put("SOUND", "musicVolume", "0")
with path.open("w") as output:
    ini.write(output, space_around_delimiters=False)
PY
echo "e2e: [ENGINE] zMaxFps=$E2E_MAX_FPS (0 = uncapped); OPENGOTHIC_FPS_LOG=${OPENGOTHIC_FPS_LOG:-1}"
# FileUtil::nestedPath resolves game paths case-insensitively, on both hosts.
SYSTEMPACK_INI="$(find "$GOTHIC_DIR" -maxdepth 2 -iname SystemPack.ini -path '*[sS][yY][sS][tT][eE][mM]/*' -print -quit)"
if [[ -n "$SYSTEMPACK_INI" ]]; then
  # Original game INIs may contain CP1251 comments, including on UTF-8 hosts.
  SYSTEMPACK_FPS="$(LC_ALL=C awk '
    { sub(/\r$/, ""); line=tolower($0) }
    line ~ /^[[:space:]]*\[/ { parameters=(line ~ /^[[:space:]]*\[parameters\]/) }
    parameters && line ~ /^[[:space:]]*fps_limit[[:space:]]*=/ {
      sub(/^[^=]*=/, "", line); print int(line); exit
    }' "$SYSTEMPACK_INI")"
  if [[ "${SYSTEMPACK_FPS:-0}" -gt 0 ]]; then
    echo "WARNING: $SYSTEMPACK_INI [PARAMETERS] FPS_Limit=$SYSTEMPACK_FPS overrides e2e zMaxFps=$E2E_MAX_FPS" >&2
  fi
fi
START_ARGS=(-nomenu)
if [ -n "${SAVE:-}" ]; then
  cp "$SAVE" "$LOG_DIR/save_slot_1.sav"
  START_ARGS=(-save 1)
fi
if [ "${GOTHIC_INTRO:-0}" != "1" ]; then
  START_ARGS+=(-nointro)
fi
# Recheck Mac contention after server startup. On Ryzen, gq owns the display and
# permits server pytest alongside the game (older runners hold the machine lock).
if [ "$(uname -s)" = Darwin ]; then
  until ! LC_ALL=C pgrep -f '^[^ ]*python[0-9.]* [^ ]*bin/pytest|^[^ ]*Gothic2Notr -g' >/dev/null; do sleep 5; done
fi
GAME_ENV=("GOTHIC_AI_URL=ws://127.0.0.1:$PORT" "OPENGOTHIC_FPS_LOG=${OPENGOTHIC_FPS_LOG:-1}")
if [[ "$SCENARIO" == neural-soak ]]; then
  # Refuse a fixed-step recording: the soak must keep the ordinary game clock.
  SOAK_BUILD="$(dirname "$GAME_BIN")"
  SOAK_NORMAL_CLOCK=0
  # ryzen.sh publishes cache entries only from cadence-enabled builds. A fresh
  # worktree can run that runtime through game.sh without its own CMakeCache.
  if [[ "${GQ_PROFILE:-}" == ryzen &&
        "${RYZEN_GAME_BIN:-}" =~ ^"$HOME/gothic-ai/engine-cache/"[a-f0-9]{64}/Gothic2Notr$ &&
        -x "${RYZEN_GAME_BIN:-}" ]]; then
    SOAK_NORMAL_CLOCK=1
  fi
  # Ryzen publishes the runtime directory into its shared cache. Its CMake
  # flags remain in this task's build directory, outside that runtime copy.
  if [[ "${GQ_PROFILE:-}" == ryzen && -f "$ROOT/../build/CMakeCache.txt" ]] &&
     grep -qx 'OPENGOTHIC_ENABLE_APP_CADENCE:BOOL=ON' "$ROOT/../build/CMakeCache.txt"; then
    SOAK_NORMAL_CLOCK=1
  fi
  for ((i = 0; i < 4; i++)); do
    if [[ -f "$SOAK_BUILD/CMakeCache.txt" ]]; then
      if grep -qx 'OPENGOTHIC_ENABLE_APP_CADENCE:BOOL=ON' "$SOAK_BUILD/CMakeCache.txt"; then
        SOAK_NORMAL_CLOCK=1
      fi
      break
    fi
    SOAK_BUILD="$(dirname "$SOAK_BUILD")"
  done
  if [[ "$SOAK_NORMAL_CLOCK" == 1 ]]; then
    GAME_ENV+=("OPENGOTHIC_BENCH=$ROOT/scripts/bench/neural-soak-e2e.bench" "OPENGOTHIC_BENCH_OUT=$LOG_DIR"
      "OPENGOTHIC_BENCH_REALTIME=1" "OPENGOTHIC_BENCH_KEEP=1" "TEMPEST_BACKGROUND=1"
      "TEMPEST_WINDOW_SIZE=640x360" "OPENGOTHIC_BENCH_ENCODER=-c:v libx264 -preset ultrafast -crf 30")
  else
    echo 'neural-soak: video unavailable (normal-clock bench not compiled)'
  fi
fi
if [[ -n "${GAME_EXTRA_ENV:-}" ]]; then
  while IFS= read -r assignment; do
    [[ -n "$assignment" ]] || continue
    [[ "$assignment" =~ ^[a-zA-Z_][a-zA-Z0-9_]*= ]] || {
      echo 'GAME_EXTRA_ENV requires one NAME=value assignment per line' >&2
      exit 2
    }
    GAME_ENV+=("$assignment")
  done <<<"$GAME_EXTRA_ENV"
fi
if [ "$SCENARIO" = "trace" ] || [ "$SCENARIO" = "broken" ] || [ "$SCENARIO" = "trace_crime" ]; then
  GAME_ENV+=("OPENGOTHIC_BENCH=$ROOT/scripts/bench/$SCENARIO-e2e.bench" "OPENGOTHIC_BENCH_OUT=$LOG_DIR"
    "OPENGOTHIC_BENCH_REALTIME=1" "OPENGOTHIC_BENCH_KEEP=1" "TEMPEST_BACKGROUND=1")
fi
if { [ "$SCENARIO" = "broken" ] || [ "$SCENARIO" = "trace_crime" ]; } && [[ -z "${OPENGOTHIC_FRACTURE+set}" ]]; then
  GAME_ENV+=("OPENGOTHIC_FRACTURE=$HOME/.local/share/gothic-ai/fracture")
fi
if [[ "${GOTHIC_TTS_RECORD:-0}" == 1 ]]; then
  GAME_ENV+=("OPENGOTHIC_BENCH=$ROOT/scripts/bench/tts-record.bench" "OPENGOTHIC_BENCH_OUT=$LOG_DIR"
    "OPENGOTHIC_BENCH_REALTIME=1" "OPENGOTHIC_BENCH_KEEP=1" "TEMPEST_BACKGROUND=1"
    "TEMPEST_AUDIO_LOOPBACK=48000" "TEMPEST_WINDOW_SIZE=320x180"
    "OPENGOTHIC_BENCH_ENCODER=-c:v libx264 -preset ultrafast -crf 28")
fi
(cd "$LOG_DIR" && exec env "${GAME_ENV[@]}" "$GAME_BIN" -g "$GOTHIC_DIR" "${START_ARGS[@]}" -devmode) >"$LOG_DIR/game.log" 2>&1 &
GAME=$!
printf '%s\n' "$GAME" >"$LOG_DIR/game.pid"

for _ in $(seq 1 "$TIMEOUT"); do
  kill -0 "$SRV" 2>/dev/null || break
  if ! kill -0 "$GAME" 2>/dev/null; then
    report_game_exit unexpected
    echo "game exited unexpectedly, see $LOG_DIR/game.log"
    exit 1
  fi
  sleep 1
done

if kill -0 "$SRV" 2>/dev/null; then
  echo "timeout after ${TIMEOUT}s, logs in $LOG_DIR"
  exit 1
fi
wait "$SRV" && rc=0 || rc=$?

if [[ "$rc" == 0 && "${GOTHIC_TTS_RECORD:-0}" == 1 ]]; then
  # The ordinary tts scenario exits after the last line starts. Let the bench
  # close the WAV/video itself, after that voice has drained, before killing it.
  recorded=0
  for _ in $(seq 1 60); do
    if [[ -f "$LOG_DIR/events.jsonl" ]] && grep -q '"kind":"rec_stop"' "$LOG_DIR/events.jsonl"; then
      recorded=1
      break
    fi
    kill -0 "$GAME" 2>/dev/null || break
    sleep 1
  done
  if [[ "$recorded" == 0 ]]; then
    echo "tts recording did not close, see $LOG_DIR/game.log" >&2
    rc=1
  fi
fi

grep -E "scenario|attached|detached|replied|note|calls|memory|remembers|saved|voice|timing|roster|turn_to|say_svm|play_ani|look_at|unknown_npc|planner|decision|incident|reflex|rejected_give|give|intent_|story|director|ERROR" "$LOG_DIR/server.log" || true
grep -E "^\[(ai|story)\]" "$LOG_DIR/game.log" || true
echo "logs: $LOG_DIR"
exit "$rc"

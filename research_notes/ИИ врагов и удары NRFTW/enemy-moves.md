# Enemy use of dodge / barge / charged strike / riposte (and parry) - research notes

Date 2026-11-02. Purpose: inform NPC behaviour in the Gothic 2 remake (OpenGothic) that ports No Rest for the Wicked (NRFTW) combat.
Confidence tags: [V] = page fetched and read; [S] = only a search-engine snippet, page not read; [G] = my guess / design inference.
Hard limit up front: no source gives NRFTW enemy wind-up lengths in ms, enemy dodge cooldowns or probabilities. Those numbers are not public; everything numeric below is either a wiki figure, a patch note, or marked [G].

## 1. NRFTW: what enemies actually do

**Poise facts (wiki.gg, the only combat page on it)** [V] https://nrftw.wiki.gg/wiki/Poise
- Full poise bar = stun 2-3 s ("stagger" for enemies). Enemy poise armour: unarmoured about 10, brutes and knights about 30. Enemy poise damage per attack 5-30, mostly 10-20.
- Some attacks always knock down regardless of poise: giant kicks, **knight shield bashes**, specific boss attacks. Only a Greatshield block stops them.
- Shoulder barge (player) knocks down but does not count as a "stagger" for stagger enchantments. https://nrftw.wiki.gg/wiki/Poise
- Dodge classes: quickstep / roll / heavy roll + barge. Quickstep "nearly instantaneous", mostly i-frames, spammable but drains stamina; normal roll cannot be spammed; barge has short range, knocks down light enemies, unlikely to drop big enemies or bosses. [V] https://gamerguides.com/no-rest-for-the-wicked/guide/gameplay/basics/equip-load-and-dodging
- Players report heavy-load barge is "absolutely insane" in the open world and "does nothing in major boss fights"; barge deals no damage and an enemy's poise appeared to reset after a barge knockdown. [V] https://forum.norestforthewicked.com/t/heavy-shield-charge-dash-attack-should-do-damage/26356

**Enemy roster (Cerim Crucible guide, enemy move lists)** [V] https://gamerguides.com/no-rest-for-the-wicked/guide/endgame/cerim-crucible/all-cerim-crucible-enemies
- Shield Knight: **parries** attacks aimed at it ("leaving the player open"); **shield charge** "to knock the player over" (counter: let it close, roll past, hit once). Axe Knight also parries behind a shield.
- Charged/committed moves: Axe Knight "crouches to charge, leaps", second strike "can change direction"; Great Sword Knight "crouches, lowers sword, charges", then **pauses briefly** (backstab window); Pillory Witch lunge with upward swing "can knock the player down"; ground-orb moves read as "roll as soon as you see them raise their hammer".
- No enemy in that guide is listed as dodging, grabbing, or riposting. Grabs exist on bosses (Riven Twins: dodge the opposite way when the hand reaches out) [S] https://primagames.com/gaming/how-to-defeat-the-riven-twins-in-no-rest-for-the-wicked
- Darak's kick: about 1 s animation, knocks down; later patch made "all of Darak's attacks parryable" [V] https://forum.norestforthewicked.com/t/high-priority-issues-that-hurt-the-game-right-now/16491 and https://forum.norestforthewicked.com/t/patch-notes-the-breach-refined-patch-1/19212

**Enemy dodge: exists, mostly as a spacing tool**
- Player report: enemies "love to spam dodge dashbacks", which hurts low-reach weapons [V] https://forum.norestforthewicked.com/t/feedback-suggestions-post-breach-update-combat-builds-gear-and-enchantments/15691
- A Huntsman "dodged" three times in a row off a cliff (edge suicide bug) [V] https://forum.norestforthewicked.com/t/feedback-regarding-enemies-ai-fighting/8438
- Input-reading thread: "Enemies jump away when the player starts charging, even if the player cancels"; "enemies dodge the moment the attack button is pressed, even from out of reach, before the animation appears". Counter-view in the same thread: enemies react on the first animation frame, which "can feel like input reading". No dev replied. [V] https://forum.norestforthewicked.com/t/enemies-are-cheating-through-input-reading/6899
- Suggestion (implies they do not yet): "enemies could dodge arrows" [V] https://forum.norestforthewicked.com/t/suggestions-for-improving-the-combat-system/25936

**Telegraphs**
- Official wiki line: bosses have "well-telegraphed moves and fair windows"; read "wind-up times". [V] https://norestforthewicked.wiki.fextralife.com/Combat
- No unique cue for unparryable attacks; players ask for a reddish aura or sound; replies say "almost everything is parryable", unparryable = clean charges, grabs, long specials. [V] https://forum.norestforthewicked.com/t/consider-a-unique-visual-sound-queue-for-attacks-that-cannot-be-parried/17513
- Some cues are audio-only (crucible knight blue-fire explosion). [V] thread 6899 above.

**Complaints and designer fixes (NRFTW)**
- Tracking: enemies track "with robotic precision"; heavy spear throws "nearly unavoidable at close range"; asked to drop tracking at startup. [V] https://forum.norestforthewicked.com/t/high-priority-issues-that-hurt-the-game-right-now/16491 , https://forum.norestforthewicked.com/t/the-breach-feedback-so-far/15215
- Proposed rule: lock aim before the wind-up, give hammer smash/jump a "point of no return". [V] https://forum.norestforthewicked.com/t/enemies-attack-patterns/12699
- Aggression: "spamming abilities with little downtime"; knockdown + stomp "stun loop"; a fast kick then an overhead slam during the stun one-shots (sewer boss). [V] threads 15215, 15691.
- Crowds: group makes dodging "impossible, or leaves only dodging over and over"; mixed enemy types "do not attack in sync" (so they are not a coordinated crowd). [V] thread 25936.
- Patch response (Breach Refined): Risen Stalker crossbow "more delayed", Echo Knight tracking reduced, Echo Knight charge attack trigger condition fixed, more attacks made parryable, enemy-weapon parry sound added, Unspoken difficulty = more enemy poise. [V] https://forum.norestforthewicked.com/t/patch-notes-the-breach-refined-patch-1/19212
- Failed parry can cost "70-80% of your life", enemies can stunlock (staff user). [V] thread 15691.

**Enemy ripostes / executions on the hero in NRFTW:** none found in any source. Absence of evidence only. Staggered enemies take backstab; players say backstab on staggered enemies is hard to trigger (thread 15215).

## 2. Comparable games

**Elden Ring / Souls**
- Delayed attacks punish panic rolls ("that's what I was waiting for"); became a meme. [S] https://automaton-media.com/en/?p=10577
- "Input reading": dataminer Zullie's finding, relayed by MeinMMO: boss reacts when the heal animation begins, one of the first frames suffices; not raw input. [V] https://mein-mmo.de/en/elden-ring-one-of-the-most-unfair-mechanics-in-the-game-works-differently-than-you-thought,1103223 . Lesson: animation-keyed reaction still reads as cheating to players (same as NRFTW thread 6899).
- Hyper armour (super armour) is a timed frame window and **grabs bypass it entirely**. Wiki only covers player use; enemy use not documented. [V] https://eldenring.wiki.gg/wiki/Hyperarmor . Useful counter-rule for charged strikes: barge/grab beats super armour.
- Enemy poise 15-65 normal, 80-120 bosses [S] https://eldenring.wiki.gg/wiki/Poise . Bosses need several parries before a riposte opens [S] windowscentral guide. I did not find enemies riposting the player.

**Sekiro**
- Perilous attack: red kanji flashes over the player's head; types Thrust/Sweep/Grab/Lightning; each is unblockable with a dedicated answer (deflect+Mikiri, jump, step dodge). [V] https://sekiroshadowsdietwice.wiki.fextralife.com/Combat . Known complaint: all kanji look identical, you read the animation to pick the answer [S] steam discussion.
- Player posture break: "open to deadly attacks by enemies", no dedicated enemy deathblow animation on the player. [V] same wiki page. So the genre's flagship posture game does not paired-execute the player.

**Ghost of Tsushima**
- Red-glint = unblockable (dodge only), blue = parryable with unlocked technique; audio cue plays a split second before the visual. [S] https://redbull.com/ca-en/ghost-of-tsushima-boss-battle-tips-guide
- Sucker Punch (Zimmerman): player attack speed kept fast, enemy attacks **layered**, "we tune things so that Jin has barely enough time to deal with each enemy attack as it lands"; making enemies as fast as the player was unreadable, so first attack made predictable with follow-ups. [S] https://games.mxdwn.com/?p=69520 and https://www.androidcentral.com/ghost-tsushimas-combat-too-simple-hideo-kojima-tried-it

**For Honor**
- Guard break: community figure 400 ms static (300 ms after feint), 800 ms stun, 300 ms counter window; 2020 patch raised heavy-opener GB vulnerability to 433 ms to curb feint abuse. [S] https://forhonor.fandom.com/wiki/Guard_Break , https://forums.wynncraft.com/posts/3496795/ (snippet only; page 402 on fetch).
- Executions: PvP trophy, one guide lists kill time 2.8 s, total 6 s; some about 11.8 s; community says anything above 3 s is too long. Victim respawn +3 s. [S] https://forhonor.fandom.com/wiki/Executions . Whether AI executes the player: not found.
- Level-3 bots countering guard-break attempts with "well-timed attacks" reported as cheating (one post). [S] steam discussion 3726075043722800220.

**Kingdom Come: Deliverance**
- Complaint thread: after patches "enemies parry or master-strike nearly every melee attack", feints "don't work", a mace NPC master-struck four attacks in a row, groups of 3-4 flank. No dev reply. [V] https://forum.kingdomcomerpg.com/t/opponents-always-counter/69373
- "NPCs master strike 60% of the time regardless of level" is an unsourced player claim. [S] steam thread 4630357120384012058. Mod page says master strikes should require direction matching. Treat KCD as the cautionary tale: an unconditional counter rate makes the player's combo pointless.

**Witcher 3**: vanilla enemy dodge/counter behaviour is not documented; mods (Skilled Humans: counter after the second blocked hit, dodge mid-attack; Redux AHI: enemies speed up after the player dodge-attacks) show what players wanted added. [S] https://www.dsogaming.com/news/the-witcher-3-redux-v1-7-released-adds-adaptive-human-intelligence-to-make-human-enemies-smarter/

**Mordhau / Chivalry bots**: no bot reaction data found. The "block 70-90%, feint every ~2 s" numbers I first met belong to a **Mount & Blade: Warband** mod, not Mordhau. [S] https://www.nexusmods.com/mbwarband/mods/6033 . Mordhau wiki: kick during wind-up is the counter to a committed swing.

**The Last of Us Part II**: Naughty Dog lengthened and exaggerated enemy tells vs earlier games so melee became dodgeable; attacks start only if range, facing and line of motion allow; no numbers. [V] https://gamedeveloper.com/design/how-naughty-dog-defined-melee-attacks-and-behaviors-in-i-the-last-of-us-part-ii-i-

## 3. Reaction time and crowd control tools

- Human reaction to a clear single stimulus 200-300 ms; 15 frames at 60 fps (250 ms) is on the edge, 19 frames (316 ms) reactable in a test match; but a move that looks like another until late has a shorter usable window. [V] https://www.forums.dustloop.com/forums/topic/6399-reaction-time-analysis/ . Audio cue about 140-160 ms and in-game reaction 50-100 ms slower than lab [S] https://dashfight.com/news/new-reaction-game-tests-if-26-frames-is-too-fast-for-you-8740
- Attack tokens: enemy requests N tokens before an attack, all-or-nothing, cost per attack (1 for basic, 3 for a boss slam), released on attack end/abort/death. Pool size not stated. [V] https://lghts.itch.io/iron-reclamation/devlog/1092177/making-combat-feel-good-fair-readable-and-responsive-ai (a hobbyist write-up, not a studio talk). "Kung-fu circle" came from Kingdoms of Amalur [S] https://subscription.packtpub.com/book/game-development/9781783553570/4/ch04lvl1sec56/building-a-fighting-circle . The claim that Doom 2016 used per-attack-type token pools is a secondhand snippet; I could not confirm it in the GDC material (3 searches, no primary slides).

## 4. Design reading for the remake [G unless cited above]

1. **Dodge.** Trigger on the player's attack animation reaching its visible wind-up, plus a reaction delay of 250-400 ms [G, from the 200-300 ms figure]; never on the button press and never from out of reach (NRFTW thread 6899). Per-NPC chance (not 100%), per-NPC cooldown, max 2 in a row, none while the NPC is mid-attack, none during the player's already-active strike. Light-armour NPCs dash, heavy ones do not roll (mirror the player weight rule). Dashback is a spacing answer to short weapons, so give it to skirmishers only.
2. **Barge / shield bash.** Only heavy/shielded NPCs (knight-type). Visible forward-lean wind-up, rollable-through (NRFTW counter: roll past). Knockdown regardless of poise exists in NRFTW, so it needs the strictest budget: one barge in flight per group, long cooldown [G].
3. **Charged strike.** Lock aim before the wind-up or at a point of no return (thread 12699); super armour only for the wind-up and not against barge/grab (Elden Ring grabs bypass hyper armour); a pause after the swing (Great Sword Knight) as the punish window. Reduce tracking at startup as NRFTW did for the Echo Knight.
4. **Riposte on the hero.** No reference game paired-executes the player (Sekiro, Souls, NRFTW found none; For Honor executions are PvP and 3-6 s). The lost-control complaints are already strong in NRFTW (stun loop, kick + slam one-shot). If it is built: rare, one NPC at a time, only after a failed hero parry or a posture break caused by the NPC, never lethal from full health, control back within about 1.5 s [G]. A normal heavy hit instead of a paired animation is the safe fallback.
5. **Parry interaction.** Keep unparryable moves few and cue them (NRFTW players ask for it; GoT red/blue; Sekiro kanji). Parry stun on NPCs about 2-3 s matches the poise-bar stun. A parry-stunned NPC should not be able to dodge or barge.
6. **Crowds.** One special (barge, charged, riposte) per group at a time via a token; mixed types desynchronise by default (thread 25936). Sucker Punch layering: second attacker may wind up while the first lands, but only one unblockable in flight [G].

## 5. Gaps
- No NRFTW frame data or enemy AI parameters (dodge odds, cooldowns, wind-up ms); no developer statements on enemy AI found.
- No primary GDC slides for tokens (Doom, Assassin's Creed, Arkham); KCD dev statements absent; Mordhau/Chivalry bot internals absent.
- Several sources above are search snippets ([S]); re-fetch before quoting numbers in the design doc.

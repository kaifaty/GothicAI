# Physics-driven melee combat and characters in shipped games: case studies

Source note: about 22 searches and fetches. Primary developer sources were rare: the Epic Store Half Sword feature returned 403; Wolfire's GDC talk was found only through secondary summaries; there was no Bare Mettle interview. Many points below come from reviews, wikis and Steam forum posts. Claims marked **[prior knowledge, unverified here]** come from the researcher's background knowledge and were not confirmed by a source in this session. Use them as leads, not facts.

## Case-by-case breakdown (attack production, hit detection, reactions, block, controls, AI, reception)

### Takeaway
The shipped games fall along a spectrum:
- **Fully canned:** Gothic, Sekiro, For Honor. Animation-timed hits and the clearest readability.
- **Canned animation with physical values or traces:** Mordhau, Chivalry 2, Bannerlord, KCD.
- **Physical weapon carried by an animated or IK body:** Exanima, Hellish Quart, Overgrowth.
- **Fully motor-driven bodies:** Half Sword, Blade & Sorcery's NPCs, Boneworks, Toribash, Gang Beasts.

The further a game goes toward full physics, the more it depends on niche appeal, comedy or VR's 1:1 hands to stay playable.

### Cited findings

**Exanima (Bare Mettle, own engine)**
- The store copy calls it a "very deep, truly physics based combat system": real momentum, forces and collisions are always in play, so small differences in input change the outcome. The studio's tech, engine included, is its own, and Exanima is a "prelude" to their open-world RPG Sui Generis — [Exanima listing / rawg](https://rawg.io/games/exanima); [Steam store](https://store.steampowered.com/app/362490/)
- The developer says they work with historical martial-arts experts but "prioritise the gameplay and its very dynamic nature over realism" — [GOG forum, dev posts](https://gog.com/forum/general/in_development_exanima_1841d/page5)
- The developer rejected a classic stamina bar and tried fatigue that makes actions "gradually… slower and less energetic" — [Steam discussion](https://steamcommunity.com/app/362490/discussions/0/3168820451539042464)
- Player reports (2015, early build):
  - attacks and facing are both controlled with the mouse;
  - the camera does not rotate unless the player moves it;
  - swinging left-to-right instead of the default right-to-left needs mouse movements that "unavoidably mess with the direction you're facing";
  - "physics based so it's really hard to master."
  - Sources: [URW forum thread](https://www.tapatalk.com/groups/urwforum/exanima-t6557.html); [Steam discussion](https://steamcommunity.com/app/362490/discussions/0/224446614462282211)
- Earlier work: Hammerfight, the same studio's 2D mouse-swung physics weapon game, is the lineage — [nodal.gg Hammerfight](https://nodal.gg/game/hammerfight-41100)

**Half Sword (Half Sword Games, UE5)**
- The game director, Frank Togonidze, said they chose full physical simulation as the core after early active-ragdoll sword videos spread on Twitter and Reddit. It started as a student project — [Epic Games Store feature](https://store.epicgames.com/news/how-half-sword-makes-bloody-brutal-medieval-combat-feel-real) (403 on fetch; content from search snippet)
- A playtest update reworked "the physics system, which affects nearly every aspect of combat," and the developer warned of unpredictable effects — [PCGamesN](https://www.pcgamesn.com/half-sword/physics-update)
- Press describes limbs bending oddly during heavy swings. One reviewer played the demo for 10+ hours: hard and clumsy, but compelling — [Mein-MMO](https://mein-mmo.de/en/new-demo-on-steam-half-sword,1366763/)

**Blade & Sorcery (WarpFrog, VR, Unity)**
- Weapons damage and pierce enemies and surfaces according to momentum, and enemies are ragdolls — [Wikipedia](https://en.wikipedia.org/wiki/Blade_%26_Sorcery)
- "If I just barely tap an enemy with my sword it won't do anything." A hard thrust lodges the blade. A head stab kills, while a dagger in the shin "may just slow them down" — [UploadVR hands-on](https://uploadvr.com/hands-on-with-blade-sorcerys-brutally-realistic-vr-combat-system/)
- Inertia is simulated, so a weapon "drags behind" a hand that moves too fast for its weight, and the swing comes out weak and inaccurate. A shallow-angle strike is "slapping your target with a side of the blade" and does little damage — [Steam guide/discussion](https://steamcommunity.com/app/629730/discussions/1/1814296273133353033)
- Armour tiers have different penetration resistance — [Road to VR, U8](https://roadtovr.com/?p=95303)
- Imbue (fire and so on) lets a blade "sear through metal like a hot knife through butter" — [Road to VR, U8](https://roadtovr.com/?p=95303)
- Reviews call the enemies "stab-happy rag doll[s]" — [Road to VR EA review](https://www.roadtovr.com/?p=84339)
- Player complaints:
  - NPCs underestimate their reach and close in too far — [Steam discussion](https://steamcommunity.com/app/629730/discussions/0/3475107714835574158)
  - enemy bodies are "far too resistant" to the player's force, and cuts are hard to trigger — [Steam discussion](https://steamcommunity.com/app/629730/discussions/1/4407417241457273773)
- A popular mod adds continuous collision detection because fast swings phased through enemies — [Nexus "Improved Collisions"](https://www.nexusmods.com/bladeandsorcery/mods/12703)

**Boneworks / Bonelab (Stress Level Zero, VR)**
- Fully physics-based: the player has a full virtual body that answers both real input and obstructions in the world. Guns, objects and enemies are all driven by physics — [Wikipedia](https://en.wikipedia.org/wiki/Boneworks); [Road to VR reveal](https://roadtovr.com/boneworks-reveal-stress-level-zero-physics-based-interactions-embodiment/)
- Design rationale: one static set of physics rules instead of scripted interactions. The developers' example of what scripting causes is a shield bash clipping through an enemy — [Road to VR reveal](https://roadtovr.com/boneworks-reveal-stress-level-zero-physics-based-interactions-embodiment/)
- Melee examples: axes, hammers, grabbing enemies mid-attack, headbutting an enemy against a wall — [Road to VR gameplay](https://roadtovr.com/?p=87322)

**Overgrowth (Wolfire)**
- David Rosen's GDC 2014 talk "Animation Bootcamp: An Indie Approach to Procedural Animation" uses about 13 keyframes in total, blended with spring-like (not linear) interpolation, plus procedural physics — [Unity forum summary](https://discussions.unity.com/t/an-indie-approach-to-procedural-animation-gdc-video-talk/538228); [Polycount, GDC Vault link](https://polycount.com/discussion/comment/2093571); [jME summary](https://hub.jmonkeyengine.org/t/dynamic-procedural-character-animation-system/29460)
- Alpha updates added an active block, scabbards, multi-weapon animation and more accurate stab decals — [DSOGaming](https://www.dsogaming.com/?p=24687)

**Mordhau (Triternion)**
- Attack phases: windup, then release (the only phase that can hit or be blocked), then recovery. Feint (cancel, costs stamina) and morph (swing to stab, or back; about 7 stamina per one guide) are possible only in the windup — [Mordhau wiki](https://mordhau.fandom.com/wiki/Combat?oldid=3196); [Gamepressure](https://www.gamepressure.com/mordhau/combat/zac3ca)
- A chamber mirrors the enemy's attack: it blocks, and it also hits if the attacker feints — [Gamepressure](https://www.gamepressure.com/mordhau/combat/zac3ca)
- Drag and accel: turning the mouse toward or away from the target during release changes when the blade arrives. Over-dragged attacks lose a lot of damage — [Steam discussion](https://steamcommunity.com/app/629760/discussions/0/1651043958631171541)

**For Honor (Ubisoft Montreal)**
- "Art of Battle": no block button. Guard is three directions (top, left, right); matching the attacker's direction blocks, and attacking blocks the defender — [Ubisoft news 2015](https://www.ubisoft.com/en-us/game/for-honor/news-updates/3i9GE9e7XGWHqKQH2wUtZc/for-honor-the-art-of-battle); [fandom](https://forhonor.fandom.com/wiki/Gameplay)
- Ubisoft says Motion Matching (long mocap sequences chosen by pose, trajectory and desired events) removed the "disconnect between player input and what is seen on screen" — [TrueTrophies masterclass](https://www.truetrophies.com/n9030/for-honor-masterclass-video)

**Kingdom Come: Deliverance 1 and 2 (Warhorse, CryEngine)**
- KCD2 got iterative changes "to make it more accessible" and "even deeper" — [PC Gamer](https://www.pcgamer.com/games/rpg/kingdom-come-deliverance-2-is-twice-as-big-introduces-firearms-and-is-coming-this-year/)
- Players say KCD1's attack direction was "completely meaningless" and its fights largely driven by RNG and stats, apart from clinch and master strike. In KCD2 countering is much easier, master strikes are triggered by a green shield cue, and there are complaints of "90%" auto-block and of fights being "dumbed down" — [Steam KCD2 discussions](https://steamcommunity.com/app/1771300/discussions/0/601895917151348703); [another thread](https://steamcommunity.com/app/1771300/discussions/0/4363500448196093196)

**Mount & Blade II: Bannerlord (TaleWorlds)**
- The weapon physics blog:
  - the fencer is modelled as three "motors" (legs+hips, chest+shoulder, arm+wrist) that accelerate the weapon, with separate swing and thrust simulations;
  - thrust speed comes from mass; swing speed comes from mass and the weight distribution around the pivot (inertia);
  - a weapon that is too heavy makes contact before it reaches full speed and becomes "feeble";
  - weapons are crafted from parts, and each part's physical properties combine.
  - Source: [Dev blog pt.1 via GameBanshee](https://www.gamebanshee.com/k43ha)
- Damage comes from the attacker's and target's velocities, replacing the random damage ranges of earlier games ("fairer and easier for players to understand"). The inventory number assumes a hit a few cm below the tip; the real impact point is computed, so an "80 damage" axe hitting near the grip does much less — [PC Gamer](https://www.pcgamer.com/au/mount-blade-2-bannerlord-explains-how-damage-is-determined-in-combat); [Dev blog pt.2](https://gamebanshee.com/k4awc)
- Shields have HP, weapons do not, so any weapon blocks indefinitely, a frequent complaint — [Steam discussion](https://steamcommunity.com/app/261550/discussions/0/4327475236487689178)

**Sekiro (FromSoftware)**
- Posture replaces stamina. Bosses regain posture fast, so the player can't hit, back off and wait — [bit-tech review](https://bit-tech.net/reviews/sekiro-shadows-die-twice-review/1/)
- Miyazaki: lowering vitality slows posture recovery; the game is "less rooted in pure action and more in strategy" — [Red Bull interview](https://www.redbull.com/us-en/sekiro-shadows-die-twice-interview)
- Accounts of the deflect window conflict:
  - Yamamura (developer): the deflect is "very precise," stricter than earlier Souls parries, so enemy animations must be readable — [GamingBolt](https://gamingbolt.com/sekiro-shadows-die-twice-dev-talks-about-how-combat-differs-from-dark-souls-and-the-challenges-of-developing-it)
  - Super Jump Magazine: the window is "quite generous" and a missed deflect "seamlessly turns into a guard" — [Super Jump](https://www.superjumpmagazine.com/the-art-and-science-of-sekiros-combat/)

**Hellish Quart (Kubold)**
- Active ragdolls; "all damage your blade can do is calculated with realtime physics"; "the blades really block each other" — [Kubold blog](https://kubold.com/blog/2020/7/24/hellish-quart-a-game-about-historical-fencing)
- Four attack angles on four gamepad buttons, with automatic guard (autoguard) whenever the player isn't attacking. Local multiplayer only, Early Access — [Steam](https://store.steampowered.com/app/1000360/Hellish_Quart)

**Euphoria titles: GTA IV/V, RDR2, Star Wars: The Force Unleashed**
- GTA IV was the first commercial game with Euphoria. NaturalMotion stopped licensing Euphoria in 2017 — [Wikipedia Euphoria](https://en.wikipedia.org/wiki/Euphoria_(software)); [gta.wiki](https://gta.wiki/w/Euphoria)
- RDR2 (Phil Hooker, Rockstar): Euphoria "enhance[s] the physics-based reactions of both humans and animals." Examples: a rider rolls off a shot horse, is dragged in the stirrups or is pinned under it; lassoed NPCs reach for the rope or fire their guns — [Wccftech](https://wccftech.com/rockstar-euphoria-evolved-rdr2/amp/)
- GTA V's reactions are said to be stiffer and shorter than GTA IV's. This is a fan claim — [gta.wiki](https://gta.wiki/w/Euphoria)
- The Force Unleashed: Euphoria troopers scramble and grab ledges and railings "for dear life," which reviewers loved. One review says Euphoria "doesn't work quite as convincingly" as in GTA IV — [Empire](https://empireonline.com/gaming/reviews/star-wars-force-unleashed-review); [The HD Room](https://thehdroom.com/reviews/star-wars-the-force-unleashed-ps3-xbox-360-review-30332/)

**Gang Beasts / Human: Fall Flat (the "floaty" extreme)**
- Gang Beasts' fighters are "deliberately awkward to control"; the controls are still learnable with a "broader moveset than you might first think" — [PC Gamer review](https://pcgamer.com/gang-beasts-review)
- Human: Fall Flat maps each arm to a separate mouse button, so pushing a cart or climbing is hard on purpose. Some reviewers say the deliberate frustration wears thin — [CGMagazine](https://www.cgmagonline.com/reviews/fall-flat-review-pc-review/?cmpscreen); [PlayStation Country](https://www.playstationcountry.com/?p=6811)

**Toribash (Nabi Studios)**
- The body is controlled only through its joints; each joint can be relax, hold, contract or extend. Turn-based: set joints, simulate N frames, pause. Rigid-body sim with dismemberment. Points come from damage or from the opponent touching the ground with anything but hands or feet — [Giant Bomb](https://giantbomb.com/wiki/Games/Toribash); [404TechSupport](https://www.404techsupport.com/2011/01/10/toribash-a-free-ragdoll-fighting-game/amp/)

**Gothic 1/2 (Piranha Bytes): the baseline**
- From the game's scripts, per modders:
  - a normal melee hit does about 1/10 of (weapon damage + strength − armour); a critical hit skips the reduction — [kosgames, Archolos damage calc](https://kosgames.com/the-chronicles-of-myrtana-archolos-damage-calculations-14083/)
  - crit chance comes from weapon skill (player claim for G1: 0% untrained, ~5% at level 1, ~10% mastered) — [GOG forum](https://www.gog.com/forum/gothic_series/gothic_1_is_dexterity_useful_other_than_for_bow_reqs_and_ranged_tohit)
  - higher skill unlocks left-right-left combos; a blocked enemy is briefly open to a combo — [Steam G2 discussion](https://steamcommunity.com/app/39510/discussions/0/343787920135154915)

### Inferences

**Gothic, Sekiro, For Honor**
- **Gothic [prior knowledge, unverified here]:**
  - attacks are canned animations from the `.MDS` model scripts, with combo windows defined as frame ranges on the animation;
  - the hit lands when the attack animation's hit frame triggers and the target is inside the weapon range and angle, not through blade collision;
  - hit reactions are canned stumble/parade animations;
  - blocking is a held parade input against the attacker's direction of facing;
  - the AI uses scripted fight-AI tables (`FAI`) keyed on distance and enemy state.
- **Sekiro and For Honor [prior knowledge]:** both detect hits with per-animation active frames and capsule or volume traces, and use canned or lightly blended reactions. Readability comes from authored telegraphs, and Sekiro's deflect is a timed input window.

**Mordhau, Chivalry 2, KCD**
- **Mordhau [prior knowledge]:** the weapon is traced along its length between frames during release. The arc is canned, but the mouse moves the camera and so steers the trace, which is how drag and accel work. Hit reactions are flinch animations. Hit zones (head, torso, legs) times armour tiers drive damage; the AI exists only as practice bots.
- **Chivalry 2 [prior knowledge]:** the same trace family as Mordhau: directional LMB/RMB/keys, a parry with a riposte window, and weapon-to-weapon traces that produce clashes. No source in this session.
- **KCD1 [prior knowledge]:** a 5-direction attack star, a perfect-block window and master-strike counters; the AI is a scripted duel controller.

**Physical-weapon and full-physics games**
- **Exanima [prior knowledge]:** the character is a physically simulated body moved toward goal poses. The weapon swing follows mouse input plus momentum, so the player commits to a swing and steers it. Damage depends on the actual contact velocity and edge alignment against layered armour. Characters can trip and fall from collisions.
- **Half Sword [prior knowledge]:** the mouse drives hand and weapon targets more directly; damage comes from contact impulse and edge alignment; the AI is driven by the same motorised body.
- **Blade & Sorcery [prior knowledge]:** NPCs play animations while their body joints track them with limited strength (a "partial ragdoll"), and they go full ragdoll when stunned or killed.
- **Absolver; Jedi: Fallen Order / Survivor [prior knowledge]:** both are canned-animation games:
  - Absolver: four stances, a deck of moves per stance transition, parry/dodge/absorb defences.
  - Jedi: Sekiro-like parry and block meter; the lightsaber does not dismember humans.
  - Neither physics-driven; useful only as readability references.

### Gaps
- No primary developer write-up was found for the following:
  - Exanima's control or AI internals;
  - Half Sword's damage model (the Epic article was blocked);
  - Mordhau's or Chivalry 2's hit detection;
  - For Honor's hit detection;
  - Gothic's combo-window and hit-check implementation.
  - The source-code or `.MDS` details for Gothic can be checked directly in this repo or engine.
- Absolver and Chivalry 2 were not searched successfully; Jedi was not searched.
- No exact Bannerlord damage formula was found. Part 2 of the blog (gamebanshee.com/k4awc) was not fetched.

## Which design choices let realism coexist with playability, and which failures are reported?

### Takeaway
Playable realism usually keeps physics in the outcome (where the blade lands, its speed, the angle, armour) and authored structure in the input and timing:
- **Animation-based games:** windup/release/recovery phases, telegraphs, directional guards.
- **Physics-first games:** constraint on the weapon (momentum, inertia) instead of on the player.

The reported failures are consistent:
- floatiness and slapstick (Gang Beasts, Human: Fall Flat, Half Sword's bending limbs);
- weak or unregistered hits (B&S phasing, which needed a CCD mod; "too resistant" bodies);
- control conflicts (Exanima's facing versus swing);
- shallow strategies when the system is simplified (KCD2's "90% auto-block", any weapon blocking forever in Bannerlord);
- RNG feeling unfair (KCD1).

### Cited findings
- Bannerlord replaced random damage with velocity-based damage to be "fairer and easier for players to understand" — [GameBanshee dev blog](https://www.gamebanshee.com/k43ha)
- Mordhau limits feints and morphs to the windup and charges stamina for them, which keeps mind games readable — [Gamepressure](https://www.gamepressure.com/mordhau/combat/zac3ca)
- Sekiro's developer: a strict deflect means enemy animations must be readable — [GamingBolt](https://gamingbolt.com/sekiro-shadows-die-twice-dev-talks-about-how-combat-differs-from-dark-souls-and-the-challenges-of-developing-it). A missed deflect still becomes a guard, which softens failure — [Super Jump](https://www.superjumpmagazine.com/the-art-and-science-of-sekiros-combat/)
- Boneworks argues uniform physics rules avoid scripted clipping (the shield bash example) — [Road to VR](https://roadtovr.com/boneworks-reveal-stress-level-zero-physics-based-interactions-embodiment/)
- Fast swings phasing through targets in B&S needed continuous collision detection (a mod) — [Nexus](https://www.nexusmods.com/bladeandsorcery/mods/12703)
- Hellish Quart pairs a physical blade with four discrete attack buttons and an autoguard, cutting control complexity while blade collision stays real — [Steam](https://store.steampowered.com/app/1000360/Hellish_Quart); [Kubold blog](https://kubold.com/blog/2020/7/24/hellish-quart-a-game-about-historical-fencing)
- Exanima's developer puts gameplay above realism — [GOG forum](https://gog.com/forum/general/in_development_exanima_1841d/page5)
- A player complains that swing direction conflicts with facing in Exanima — [URW forum](https://www.tapatalk.com/groups/urwforum/exanima-t6557.html)
- KCD2 is criticised as "dumbed down" and auto-block-heavy, while others call it an improvement over KCD1's RNG — [Steam](https://steamcommunity.com/app/1771300/discussions/0/601895917151348703)
- Comedy is a deliberate choice in floppy games, and frustration wears thin for some reviewers — [PC Gamer](https://pcgamer.com/gang-beasts-review); [PlayStation Country](https://www.playstationcountry.com/?p=6811)

### Inferences
- **The hybrid pattern that works best in non-VR shipped titles:**
  - animated or targeted body plus a physical weapon (or physically valued traces);
  - discrete intent (direction or attack button) plus continuous modulation (mouse drag, momentum);
  - an authored defensive window;
  - physics-computed damage.
- **Full motor-driven bodies:** they stay niche (Half Sword, Toribash) or rely on VR (B&S, Boneworks), where the player's real hands supply the intelligence that would otherwise have to be authored.
- **Euphoria-style reactions:** they are praised as spectacle (RDR2, The Force Unleashed). The games that use them keep control on the attacker's side canned; physics is applied to the victim.

### Gaps
- No quantitative player-retention or learning-curve data, such as playtime-to-competence, was found for any title.

## How do these games keep player input responsive when the body is physical?

### Takeaway
There are three approaches:
1. Keep the controlled body kinematic or animated, and make only the weapon or victim physical (Mordhau, For Honor's motion matching, Euphoria titles).
2. Let the player steer the weapon's momentum, not its pose (Exanima, Bannerlord's motor model, B&S inertia lag).
3. Accept the lag as the game, either through VR's 1:1 hand tracking or by playing it as comedy.

### Cited findings
- For Honor's Motion Matching was built to remove the input-to-screen disconnect — [TrueTrophies](https://www.truetrophies.com/n9030/for-honor-masterclass-video)
- B&S weapons lag behind the hand by simulated inertia, so too-fast motion gives weak, inaccurate swings. Players learn to respect weapon weight — [Steam](https://steamcommunity.com/app/629730/discussions/1/1814296273133353033)
- Bannerlord's three-motor fencer accelerates the weapon; heavy weapons reach full speed late — [GameBanshee](https://www.gamebanshee.com/k43ha)
- Exanima: small differences in input change outcomes; the camera stays fixed unless the player moves it — [rawg](https://rawg.io/games/exanima); [URW forum](https://www.tapatalk.com/groups/urwforum/exanima-t6557.html)
- Toribash sidesteps real-time responsiveness entirely with turn-based joint orders — [Giant Bomb](https://giantbomb.com/wiki/Games/Toribash)
- Overgrowth uses spring-interpolated procedural poses from very few keyframes, so the body reacts instantly to input while staying physically plausible — [Unity forum](https://discussions.unity.com/t/an-indie-approach-to-procedural-animation-gdc-video-talk/538228)

### Inferences
- Responsiveness comes from not routing locomotion and facing through the physics. Root motion and facing stay kinematic or strongly assisted; only the limbs and weapon follow motor targets.
- Exanima's complaint is about the swing-steering axis colliding with the facing axis, not about latency.
- Overgrowth's spring-interpolated procedural pose sits halfway: it is not fully physical, but it reads as physical.

### Gaps
- No source gave motor gains, PD constants, or measured input latency for any of these games.

## How is AI made to fight with a physical body?

### Takeaway
Documented evidence is thin. Shipped approaches visible in sources:
- **Scripted duel controllers with canned moves:** Gothic, Sekiro, For Honor, KCD.
- **NPCs playing animations through partially strong ragdolls:** Blade & Sorcery, with visible failures in spacing and reach.
- **Behaviour-driven active ragdolls for reactions, not attacks:** Euphoria in RDR2 and The Force Unleashed; their NPCs grab ledges and reach for lassos.

### Cited findings
- B&S NPCs "underestimate their reach and get in closer than they should" — [Steam](https://steamcommunity.com/app/629730/discussions/0/3475107714835574158)
- Euphoria NPCs grab railings and ledges when pushed — [Empire](https://empireonline.com/gaming/reviews/star-wars-force-unleashed-review)
- RDR2 lassoed NPCs reach for the rope or fire their guns — [Wccftech](https://wccftech.com/rockstar-euphoria-evolved-rdr2/amp/)
- Exanima lists "complex and smart AI" only as marketing copy — [Steam store via search](https://store.steampowered.com/app/362490/)
- Mordhau's bots and KCD's duels use scripted controllers [prior knowledge].

### Inferences
- Physics-body AI in shipped games is generally the same animation/goal-pose controller that drives the player, plus a scripted decision layer (distance, opponent phase, guard) that picks targets.
- No shipped melee game found here uses learned (RL) control for its NPCs.
- The NaturalMotion approach (behaviours such as "balance", "grab", "protect head" on an active ragdoll) is the documented commercial precedent, but only for reactions.

### Gaps
- No Bare Mettle, Half Sword or WarpFrog write-up on AI architecture was found. Whether Exanima or Half Sword AI uses the same input layer as the player is unverified.

## What is the typical damage model?

### Takeaway
Physics-first games compute damage from contact momentum or velocity, the impact point along the blade, edge or point alignment, and the hit location against armour tier or penetration. Canned games use a stat formula with hit zones and armour.

### Cited findings
- B&S: momentum-based pierce and damage; a light tap does nothing; a shallow angle makes a "slap"; head kills while shin slows; armour tiers have penetration resistance — [Wikipedia](https://en.wikipedia.org/wiki/Blade_%26_Sorcery); [UploadVR](https://uploadvr.com/hands-on-with-blade-sorcerys-brutally-realistic-vr-combat-system/); [Road to VR](https://roadtovr.com/?p=95303)
- Bannerlord: damage comes from attacker and target velocities and the computed impact point (the inventory value assumes a few cm below the tip; near the grip does much less); swing inertia depends on mass distribution — [PC Gamer](https://www.pcgamer.com/au/mount-blade-2-bannerlord-explains-how-damage-is-determined-in-combat); [GameBanshee](https://www.gamebanshee.com/k43ha)
- Hellish Quart: all blade damage is computed from real-time physics — [Kubold](https://kubold.com/blog/2020/7/24/hellish-quart-a-game-about-historical-fencing)
- Mordhau: over-dragged attacks lose damage — [Steam](https://steamcommunity.com/app/629760/discussions/0/1651043958631171541)
- Gothic 2: a normal hit is about (weapon + STR − armour)/10, a crit is full; crit chance comes from weapon skill — [kosgames](https://kosgames.com/the-chronicles-of-myrtana-archolos-damage-calculations-14083/); [GOG forum](https://www.gog.com/forum/gothic_series/gothic_1_is_dexterity_useful_other_than_for_bow_reqs_and_ranged_tohit)
- Toribash: points from damage, plus ground contact by non-hand or foot parts — [Giant Bomb](https://giantbomb.com/wiki/Games/Toribash)

### Inferences
- A workable synthesis:
  - effective blow = relative contact speed at the impact point × effective mass (weapon + arm), scaled by edge or point alignment (cut versus slap), then reduced by armour type versus damage type (cut, thrust, blunt) and multiplied by hit zone;
  - add a minimum-impulse threshold so taps do nothing (B&S);
  - add a sweet-spot falloff along the blade (Bannerlord).
- In Bannerlord the velocity term is applied on top of a fixed animated arc, which shows physics-valued damage does not require a physical body.

### Gaps
- No published numeric formulas or coefficients were found for B&S, Half Sword, Exanima or Bannerlord (part 2 of the Bannerlord blog was not fetched).

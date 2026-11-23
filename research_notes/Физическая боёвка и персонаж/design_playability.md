# Making realistic, physics-driven melee combat playable and enjoyable (design guide notes)

Scope: what designers say players need from melee, how simulation breaks it, the known fixes, how to evaluate it, and how AI should fight. Sources are a mix of primary (designer articles, interviews) and secondary (reviews, forums). Each finding says which kind it is. Fetch notes: the Epic Games Store Half Sword interview and the TaleWorlds Bannerlord dev blog both returned HTTP 403, so they are cited only through search snippets.

## Q1. Which control schemes for directional attacks and blocks are proven, and which fit a third-person mouse-and-keyboard RPG like Gothic?

### Takeaway
Three families exist:
- **Mouse-direction gestures with snapped directions.** Examples are Mount & Blade, Chivalry and Mordhau, with 3 or 4 attack directions plus a thrust; Mordhau adds body-turn "accels" and "drags".
- **Stance-based guards.** For Honor holds the guard in one of three directions, with lock-on.
- **Free physical puppeteering.** In Half Sword and Exanima the mouse moves the weapon or hands.

The puppeteering family has a punishing learning curve. Warhorse, the studio behind Kingdom Come, rejected directional blocking because it was "simply not fun". For a third-person RPG, the evidence points to a few snapped directions with input assists, not free puppeteering.

### Cited Findings
- **Mordhau has three basic attack types:** horizontal strikes, vertical strikes and thrusts. Turning the body during a swing makes it land sooner (an "accel") or later (a "drag"). — [Way Too Many Games review](https://waytoomany.games/2019/05/30/review-mordhau/)
- **Players complain about drag and feint spam in Mordhau,** and the community disagrees on how drags even work. This shows that timing manipulation through body rotation is powerful and divisive. — [Steam discussion](https://steamcommunity.com/app/629760/discussions/0/3088885996481710788/?ctp=2) (anecdotal)
- **Mordhau is a spiritual successor to Chivalry,** made by veterans of that game, and its combat is loosely based on historical feints and redirection. — [TV Tropes](https://tvtropes.org/pmwiki/pmwiki.php/VideoGame/MORDHAU); [Wikipedia](https://en.wikipedia.org/wiki/Mordhau_(video_game))
- **For Honor (stance guard):**
  - Players block by facing a stance direction (left, right or top); there is no separate block button. — [For Honor wiki](https://forhonor.fandom.com/wiki/Gameplay)
  - A stance defends one direction and leaves the other two open, so most successful attacks are preceded by a quick change of stance. — [Stars and Stripes](https://www.stripes.com/lifestyle/games/for-honor-fighting-mechanics-bring-new-vigor-to-medieval-battlefield-1.454151)
  - Game director Campos-Oriola calls the Art of Battle the core innovation and says it is "easier to apprehend" than people expect. — [Gamesplanet interview](https://us.gamesplanet.com/community/for-honor-ubisoft-connect--3049-1/news_updates/139749-gamesplanet-interviews-the-for-honor-developer-team)
  - A closed-alpha review found the guard system and target lock "absolutely designed for one on one combat", and it struggled when facing several opponents. — [God is a Geek](https://godisageek.com/2016/09/for-honor-closed-alpha-impressions)
- **Mount & Blade II: Bannerlord:**
  - Bannerlord rewards matching a shield block to the attack direction. Blocking in the wrong direction breaks the shield faster instead of killing you outright. In Warband, shields blocked from any direction. — [PC Gamer hands-on](https://www.pcgamer.com/au/hands-on-with-mount-blade-2-bannerlords-thrilling-large-scale-combat)
  - TaleWorlds says it designs around combat as the core mechanic and had to weigh the effects before adding directional shield blocking. — [TaleWorlds Dev Blog 18/10/18](https://taleworlds.com/en/Games/Bannerlord/Blog/82) (snippet only)
  - Players report the mouse-direction input misreading their intent (one claims about 40% of the time) and prefer a "block by look direction" option. The game exposes a "Vertical Movement Scale for Direction Gestures" setting. — [Steam discussion](https://steamcommunity.com/app/261550/discussions/0/4509877893260401629) (anecdotal)
- **Kingdom Come: Deliverance (Warhorse, Viktor Bocan):**
  - The team considered directional blocking for a long time, then chose one-button blocking. Bocan: "at the end we came to the conclusion that it's simply not fun", because it would have made a combat simulator with a long learning curve. — [KCD forum, quoting a press article](https://forum.kingdomcomerpg.com/t/an-article-about-sword-fighting-in-video-games-with-new-info-about-kc-d/21746) (secondhand)
  - A modder argues that in vanilla KCD1 the attack direction barely matters except in combos, and that clinches and master strikes dominate. — [Nexus Mods](https://www.nexusmods.com/kingdomcomedeliverance/mods/1384) (opinion)
  - A player reports that KCD2 dropped the bottom-left and bottom-right attack directions. — [Steam](https://steamcommunity.com/app/1771300/discussions/0/597394861184787328) (anecdotal)
- **Half Sword (free physical puppeteering):**
  - The game uses freeform ragdoll control, driven mainly by directional mouse movement, with each hand controlled independently. — [Wikipedia](https://en.wikipedia.org/wiki/Half_Sword); [Steam discussion](https://steamcommunity.com/app/2397300/discussions/0/3949154954318457031)
  - The Epic feature describes a first fight that felt like "an hour just trying to figure out how to put my dukes up". — [Epic Games Store](https://store.epicgames.com/news/how-half-sword-makes-bloody-brutal-medieval-combat-feel-real) (snippet; the page returned 403)
  - A reviewer calls it "closer to puppeteering" than action gameplay. — [RAWG](https://rawg.io/games/half-sword)
  - The game entered early access on 30 January 2026. — [Wikipedia](https://en.wikipedia.org/wiki/Half_Sword)
- **Exanima (physics-driven mouse attacks):**
  - A 2015 forum post criticised that swinging left to right needs mouse movement that also turns your facing. — [GOG forum](https://gog.com/forum/general/in_development_exanima_1841d/page5) (old)
  - The developers say the game does not suit controllers and built dedicated control schemes for it. Version 0.8.4 reworked weapon mechanics, damage and combat movement. — [SteamDB patch notes](https://steamdb.info/patchnotes/9136508)

### Inferences
- **Fit for Gothic.** Gothic 2 uses a third-person camera, mouse and keyboard, an RPG audience and fights against several monsters. That points to:
  - attack direction from a short mouse flick (left, right, overhead, thrust), snapped to a few directions;
  - blocking that is held-to-block by default and optionally directional, like Bannerlord's shield. Pick the block direction from the incoming attack or from the look direction, as Bannerlord's option does, rather than requiring a matching gesture.
  
  Free puppeteering in the style of Half Sword or Exanima conflicts with an RPG's pacing and with fights against several opponents.
- **Mouse look and gesture conflict.** Mouse direction in third person collides with camera control, which is the Exanima facing problem. Read the gesture only while the attack button is held, or during a short window, and freeze or damp the camera while it is read.
- **Directional blocking needs a soft failure.** Bannerlord's "wrong direction wears the shield faster" is a gentler penalty than "wrong direction means a full hit". It keeps direction meaningful without making the game a reflex test.

### Gaps
- No primary Triternion (Mordhau) or Ubisoft (For Honor) design talk or text could be fetched. GDC Vault content was not reachable. The rationale behind accels and drags, and For Honor's choice of three guard directions, remains undocumented here.
- No Bare Mettle or Half Sword developer explanation of their control design could be fully read (the Epic page returned 403).

## Q2. How to blend authored combo timing (like Gothic 2 combos) with physical execution

### Takeaway
The proven pattern is **authored intent, physical execution**. The designer owns timing:
- anticipation, hit frames and recovery;
- cancel and buffer windows;
- the combo tree.

Physics only adds to that: hit reactions, deflection, foot placement and recovery from loss of balance. It is driven toward the authored pose by motors, not left to run free.

### Cited Findings
- **Eric Williams on cancels and buffers** (former God of War combat designer, "Combat Canceled", Game Developer, 2008). Source for this whole group: [Game Developer](https://www.gamedeveloper.com/pc/combat-canceled-i-god-of-war-i-action-game-design)
  - A cancel is "the ability to end one animation early by beginning another animation".
  - Partial cancels exist only in windows before or after the hit frames; complete cancels work at any frame.
  - Each cancel branch has frames where it turns on and off.
  - Worked example: a 60-frame swing with hit frames 15–20. The next combo input is buffered from frame 0 to 21 and fires at 21, or fires instantly anywhere from 21 to 60. Block cancels the whole move at any frame (0–60).
  - In good systems, a buffered command can be overridden by another input before its window opens.
  - Ninja Gaiden allows no cancels before the hit, so attacks are committed, fair and learnable, but hard. God of War allows pre-hit cancels on most basic attacks, which is more responsive for average players. Heavy specials allow only post-hit cancels, to prevent infinite lockdown loops and to teach risk versus reward.
  - Guard cancels before the hit enable feints, as in Soul Calibur.
  - Kratos had about 4,000 cancel branches in God of War II.
- **Buffer length.** Too short and the buffer does nothing; too long and the game feels sluggish. A 100 ms window is a typical example. Watch for "sticky" inputs that linger after release. — [Wayline: art of input buffering](https://www.wayline.io/blog/art-of-input-buffering) (secondary)
- **Buffer priority.** Direct inputs should take precedence over buffered ones. An animation cut short by a buffered input can look jarring. — [Wayline](https://www.wayline.io/blog/mastering-input-buffering-responsive-game-controls) (secondary)
- **Pose-driven ragdolls (CEDEC 2010 deck).** Joint motors or impulses drive a ragdoll toward the animation pose, so it still reacts to collisions. To recover, compare the body's orientation with the start frames of the get-up animations, drive it toward the closest one, then blend into the animation. — [CEDEC 2010 slides](https://2018.cedec.cesa.or.jp:443/2010/archive/files/C10_I0052/C10_I0052.pdf)
- **Unreal physical animation.** A per-bone physics blend weight goes from 1.0 (that bone and the bones below it fully physical) to 0.0 (keyframed). Hit reactions start by identifying the bone that was hit. — [Unreal docs](https://docs.unrealengine.com/4.26/AnimatingObjects/SkeletalMeshAnimation/PhysicallyDrivenAnimation)
- **Overgrowth (Wolfire, David Rosen, GDC 2014 "An Indie Approach to Procedural Animation").** It blends predetermined animations with physics, so characters can be knocked off balance mid-kick and recover. The talk uses procedural techniques with few keyframes. — [PC Gamer preview](https://www.pcgamer.com/overgrowth-preview/); [Game Developer coverage](https://gamedeveloper.com/design/video-an-indie-approach-to-procedural-animation)

### Inferences
- **Mapping for Gothic 2 combos.** Gothic combos are a timed second click inside a window of the current swing. Keep that window as an authored, data-driven cancel branch from post-hit frames to the end of the swing. Physics must not shift those frames.
- **Let physics modulate the outcome, not the timing.** Examples:
  - a blade hits armour or a block and is deflected, which aborts the remaining hit frames;
  - a heavy weapon's swing slows the attacker's turn rate;
  - a hit lands on a body part.
- **Contact during recovery.** When the weapon collides early, for example with a wall or a parry, play an authored recoil ("bounce off") clip that has its own cancel windows. Do not leave the arm in free physics. This keeps the combo system deterministic and readable.
- **Physics only as an override layer.** Drive physics with motors toward the authored pose, with per-bone blend weights. Use full ragdoll only for death, knockdown and heavy stagger. Return to animation through a matched get-up start pose.

### Gaps
- No primary source was found on Gothic 2's exact combo window frames. The project's own engine data is the authority.
- The content of the GDC 2019 talk "Evolving Combat in God of War for a New Perspective" (Mihir Sheth) was not reachable. Only its existence and topic are confirmed: combat built around a close third-person camera. — [GDC news](https://gdconf.com/news/see-how-god-wars-combat-evolved-match-its-new-camera-system-gdc)

## Q3. What feedback sells physical hits (sound, hit-stop, camera shake, particles, reaction animations)?

### Takeaway
Hit-stop is the central tool: both fighters freeze for the same time, scaled with damage and capped, plus a shrinking shake on the struck body. It is layered with matched sound, a little camera shake, particles and an authored flinch. Hit-stop must be cut back when the player fights several opponents.

### Cited Findings
- **Sakurai on hit-stop** (Famitsu column 490, fan translation that its publisher says may be inaccurate). Source for this whole group: [Source Gaming](https://sourcegaming.info/2015/11/11/thoughts-on-hitstop-sakurais-famitsu-column-vol-490-1/)
  - Hit-stop freezes attacker and victim for exactly the same time. More damage means longer hit-stop.
  - Each attack has its own modifier: Marth's sword tip gets more, the blade edge less, electric attacks more, projectiles less.
  - Freeze time is capped for very heavy hits.
  - During hit-stop the attacker also vibrates slightly, but hurtboxes stay in place.
  - Grounded characters shake side to side, airborne ones up and down. Vertical shake on the ground would clip feet into the floor.
  - Shake strength scales with camera distance, starts large and shrinks over the freeze.
  - The flinch switches to a pain animation over 4 frames.
  - Sakurai holds back longer hit-stop because, in free-for-alls, a frozen pair gives a third fighter a free opening.
- **Where hit-stop is rare.** Dynasty Warriors uses little hit-stop because you hit large groups at once, and constant freezes would slow the game. — [Nintenderos summary](https://www.nintenderos.com/2015/11/sakurai-nos-explica-que-es-la-mecanica-hitstop-usada-en-los-juegos-de-lucha/)
- **Boss stop.** Sakurai's later YouTube channel describes zooming and pausing on a final blow: "vital for really letting the player feel the impact". — [Nintendo Wire](https://nintendowire.com/news/2022/12/12/this-week-in-sakurai-12-5-12-11-fine-tuning-hit-stop-and-cheating-the-system/)
- **Study on impact feel (2022).** Without meticulous design of hit-stop, sound coherence and camera control, impact feel is unsatisfactory. The paper credits the former God of War combat designer (Daniels) with stressing hit-stop. — [Lin et al. 2022, U. Washington](https://faculty.washington.edu/zkwen/articles/lin22features.pdf)
- **"Juice it or lose it"** (Jonasson and Purho, Nordic Game 2012) demonstrated live how effects like screen shake bring a functional Breakout prototype to life. A counterpoint (Folmer Kelly) warns that juice can cost immersion. — [RPG Playground summary](https://rpgplayground.com/research-making-a-juicy-game/)
- **Hit-stop, screen shake, particles and sound work best together.** — [Bugnet: knockback that feels good](https://bugnet.io/blog/how-to-implement-knockback-that-feels-good) (secondary)

### Inferences
- **Damage-scaled hit-stop for a realistic medieval tone.**
  - Use short freezes of roughly 2–6 frames at 60 fps. Light hits get the shorter end, heavy two-handed hits and armour clangs the longer end. This range is an inference: no source gave milliseconds for 3D action.
  - Apply the freeze locally, to the attacker, the victim and their weapons, rather than pausing the whole world. That respects Sakurai's crowd problem when several bandits or wolves attack.
  - Skip or halve hit-stop on hits that do not change the fight, such as glancing blows.
- **Separate feedback by result** so players can read what happened without UI:
  - hit flesh: a dull thud, blood, flinch;
  - hit armour: metallic clang, sparks, shorter flinch;
  - blocked: a wood or metal block sound, weapon recoil, no flinch;
  - parried or deflected: a distinct "ting", a longer freeze, the attacker recoils.
- **Shake the victim's mesh, not the hitbox,** in the plane of the hit. Keep camera shake small and directional in third person; strong shake hurts the readability of the next tell.

### Gaps
- No primary God of War (Daniels or Sheth) or Sekiro sound-design material on sword-clash feedback was reachable. The GamingBolt Sekiro interview does not address clash feedback.

## Q4. Pitfalls reported by designers who tried "realistic" combat, and the remedies

### Takeaway
The reported pitfalls are:
- a steep learning curve and frustration (KCD, Half Sword, early Chivalry);
- direction input that misreads intent (Bannerlord);
- distance misjudgement;
- disagreement over what "realistic" even means;
- systems built for one-on-one that collapse against groups (For Honor).

The remedies are assistance (distance correction, a simple held block), soft failure states, posture or stagger instead of raw health, and readable tells tuned frame by frame.

### Cited Findings
- **Warhorse added distance correction** in KCD because players misjudge range in first person. If the player attacks from too far, "a character will just make one more step, it turned out to be neccesary". — [KCD forum quoting an interview](https://forum.kingdomcomerpg.com/t/an-article-about-sword-fighting-in-video-games-with-new-info-about-kc-d/21746) (secondhand)
- **Historical experts disagree** on how real combat looked. Bocan: "the other one will tell us that it's completely wrong". — same source
- **KCD2 difficulty.** Warhorse keeps difficulty high on purpose and won't simplify it, saying "to achieve something, you need to invest something". — [iXBT Games, 2026-01-27](https://ixbt.games/en/news/2026/01/27/zaluetes-na-sloznost-kingdom-come-deliverance-2-razrabotcik-otvecaet-prilozite-minimalnye-usiliia.html)
- **KCD2 master strikes.** Players say they are too easy to pull off. — [Steam KCD2](https://steamcommunity.com/app/1771300/discussions/0/507318268593183025) (anecdotal)
- **Chivalry and Chivalry 2 (Torn Banner, Alex Hayter):**
  - The original was "painfully hard to master". The sequel aims for easy to pick up, hard to master. — [Unreal Engine developer interview](https://www.unrealengine.com/en-US/developer-interviews/chivalry-2-is-designed-to-make-you-feel-like-you-re-in-a-medieval-war)
  - A held block helps new players get their bearings. — [Prima Games](https://primagames.com/featured/chivalry-2-interview-torn-banners-alex-hayter)
  - The combat was reworked to cut cheap exploits. — [PC Gamer](https://www.pcgamer.com/au/with-enormous-64-player-battles-chivalry-2-wants-to-be-the-new-standard-for-medieval-combat)
- **Sekiro (co-director Yamamura).** The deflect "is very precise", tighter than a parry. That requires enemy animations readable enough that players "see what's coming, and to learn these movements, and to react accordingly". The team must "tweak frame by frame the animations" rather than only changing parameters. — [GamingBolt](https://gamingbolt.com/sekiro-shadows-die-twice-dev-talks-about-how-combat-differs-from-dark-souls-and-the-challenges-of-developing-it)
- **Sekiro posture.**
  - Miyazaki calls posture the biggest change. — [Red Bull interview](https://www.redbull.com/us-en/sekiro-shadows-die-twice-interview)
  - Deflects build the most posture at no health cost, while blocking builds posture more slowly. — [Game Wisdom](https://game-wisdom.com/critical/breakdown-sekiros-combat-broken) (secondary)
  - Bosses regain posture quickly, so hit-and-retreat fails. — [Bit-Tech](https://bit-tech.net/reviews/sekiro-shadows-die-twice-review/1/)
- **God of War (2018) camera.** One critic says the over-the-shoulder camera hides enemies, that most of their deaths came from off-camera attacks, and that Kratos's body can hide enemy animations. — [Game Wisdom](https://game-wisdom.com/?p=22096) (single critic)
- **Physics-heavy games.** Half Sword is described as "puppeteering" with a long first-fight learning curve. — [RAWG](https://rawg.io/games/half-sword)

### Inferences
- **Common remedy set for physical melee in Gothic.**
  1. Distance correction or magnetism: one assisted step, plus a rotation snap toward the locked or nearest target.
  2. Clamp physics on impact and blocks: cap impulse, cap knockback distance, never let a hit put a character inside geometry.
  3. Stamina and posture so fights end through readable states (guard break, stagger, knockdown) rather than unclear ragdoll outcomes.
  4. Ragdoll only on death or heavy knockdown, with clear get-up timing. This avoids ragdoll comedy and floatiness.
  5. Damage from weapon type and hit zone, with velocity and edge alignment as bounded multipliers (for example 0.5–1.5×), never as the main source. Raw physics damage makes outcomes unpredictable.
  6. Optional slow motion only for finishers, the analogue of Sakurai's "boss stop".
- **Lock-on for one-on-one, soft targeting for groups.** For Honor's lesson is that lock-on plus a guard system built for duels breaks against groups. Use lock-on for duels, and soft targeting plus 360° awareness with a wider camera for groups.

### Gaps
- No primary postmortem on "ragdoll comedy" or clipping was found. These remain designer common knowledge, inferred here rather than cited.

## Q5. How AI opponents should behave so a physical fight stays fair and readable

### Takeaway
The proven practice comes from Vossen and Haggis on Game Developer:
- every attack has a tell, scaled with its strength;
- attack tokens or slots, so only one or two enemies attack at a time;
- enemies mostly attack every 2–3 seconds and never in quick succession;
- no attacks from off-screen, or reduced damage for them;
- a clear zone around the player;
- enemies don't attack while staggered.

### Cited Findings
- **Vossen and Haggis, "Enemy design and enemy AI for melee combat systems"** (2015). Source for this whole group: [Game Developer](https://www.gamedeveloper.com/design/enemy-design-and-enemy-ai-for-melee-combat-systems)
  - **Tells:**
    - Enemies always show a tell, and stronger attacks have more obvious tells.
    - DmC layers a preparation pose, a weapon glint, a sound and a brief pause.
    - Batman: Arkham City flashes an icon above the attacker.
    - Ninja Gaiden 2's only tell, the enemy moving toward you, is hard to spot.
  - **Attack slots:**
    - A slot system controls which enemy attacks and when.
    - Enemies attack about every 2–3 seconds on average, not at the same time or in quick succession.
    - Staggered enemies don't attack.
    - Aztez is the exception: it allows simultaneous attacks, but never two from the same category at once.
  - **Groups and camera:**
    - Enemies are split into a near group (the few current threats) and a far group (idling, taunting, repositioning).
    - DmC keeps a clear area around the player.
    - In Dead Island 2, hits from outside the field of view do less damage.
    - Enemies move into view before attacking. Killzone's designer called unseen one-hit kills unfair.
    - God of War 3 used camera positions that kept enemies from blocking the view.
  - **Tuning variables:** anticipation length, hit frames, cooldown, interruptibility and blockability.
  - **Teaching:** attack patterns are taught progressively.
  - **Roles:** Emphasizers, Enforcers, Smashers and Challengers. Tiers: fodder, mid-rank, elite and boss.
- **Readable deflect timing depends on frame-by-frame animation work** (Sekiro), not on parameters alone. — [GamingBolt](https://gamingbolt.com/sekiro-shadows-die-twice-dev-talks-about-how-combat-differs-from-dark-souls-and-the-challenges-of-developing-it)
- **AI cheating hurts trust.** One KCD1 player claims enemies master-strike you about 60% of the time regardless of level. This is an anecdotal perception, not verified, but it shows that the AI's skill must look earned. — [Steam KCD1](https://steamcommunity.com/app/379430/discussions/0/3446961485766901890)

### Inferences
- **For Gothic's physical fights:**
  - The AI should pick authored attacks, each with a tell of at least the human reaction time (about 200–300 ms of wind-up for normal attacks, longer for heavy ones). This is an inference from typical reaction time and is not cited.
  - Physics should only execute those attacks. The AI must not exploit physics outcomes the player cannot read.
  - Hold attack tokens per player, one or two at once, and make off-screen attackers wait or telegraph with sound.
  - AI parry and block chance must follow visible stats or skill, not secret rolls.
  - Give AI the same stamina and posture rules as the player, so the fight reads as symmetric.
- **Same constraints for the trained combat body.** For a learned controller (the project's combat GRU), apply the same rules as hard constraints outside the network: a minimum tell length, token gating and no attack while staggered.

### Gaps
- **Evaluation metrics.** No primary source with concrete melee playtest metrics was found: time-to-kill targets, win-rate bands, block or parry success rates. Recommended metrics are therefore inferences:
  - time-to-kill per enemy tier;
  - player win rate per encounter (with a target band per tier);
  - share of deaths from off-screen hits;
  - share of hits the player judged "unfair" or "unclear" in a post-fight survey;
  - input-to-response latency;
  - how often combo windows are hit versus missed;
  - how often ragdolls clip or behave absurdly.
  
  An AIIDE paper on game juice exists ([AAAI](https://ojs.aaai.org/index.php/AIIDE/article/download/18889/18654/22655)) but was not read.
- **Accessibility.** No primary source for melee-specific accessibility was read. Practical options (inference): toggle versus hold for block, a lock-on toggle, auto-direction blocking, adjustable buffer window, a hit-stop and shake intensity slider, and slow motion on parry as an assist.

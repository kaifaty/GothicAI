# Shipped games and proven non-neural techniques for need-driven NPC societies (as of 2026-11-04)

Labels used below. [DEV] = statement by a developer or a developer-written paper or chapter. [WIKI] = fan wiki or modding documentation, often reconstructed from code or observation. [WRITER] = a journalist's or blogger's own testing or opinion. [SEC] = secondary report of a developer statement, or a search-engine summary of a page that could not be opened. Where a page was rate-limited or unreadable, the fact comes from a search-result snippet and says so.

Method note: the Dwarf Fortress wiki returned HTTP 429 on every direct fetch, and the CK3 dev-diary forum thread, the Czech KCD slide deck, the Game AI Pro HTN chapter, Dave Mark's GDC slides and Richard Evans's Sims 3 slides could not be read. Gaps are listed per section. Several fetches were summarised by a small model, so exact numbers are only as reliable as that summary; the PDF sources (Forbus and Wright, Zubek, Orkin, Graham, Lewis, Game AI Pro ch. 4) were read as extracted text.

---

## 1. How are needs, desires, goals and personality represented?

### Takeaway
The proven pattern is a small array of decaying numeric needs per NPC (The Sims, RimWorld, Shadows of Doubt), with personality as multipliers on decay rate, on advertisement strength or on thresholds, and with a thin "mood" or "stress" aggregate on top (RimWorld, Dwarf Fortress, CK3). Real goal or desire layers are rare: the shipped systems either use schedules plus needs (Gothic, Oblivion, KCD, Shadows of Doubt) or location-assigned tasks (S.T.A.L.K.E.R. smart terrains). Several famous "needs" systems turned out to be much thinner than their marketing (Radiant AI, A-Life).

### Cited Findings

**The Sims (2000), the reference design**
- Needs in the original game: energy, comfort, hunger, hygiene, bladder, room, social, fun, mood. [DEV] — [Forbus and Wright, "Some notes on programming objects in The Sims" (2001)](https://www.qrg.northwestern.edu/papers/Files/Programming_Objects_in_The_Sims.pdf)
- Needs are plain numbers that change at rates set in a "Simulation Constants" table; a "free will" switch and the household's money are in "Simulation Globals". [DEV] — [Forbus and Wright](https://www.qrg.northwestern.edu/papers/Files/Programming_Objects_in_The_Sims.pdf)
- Personality is a set of trait axes with polar opposites (nice/grouchy, active/lazy, serious/playful). An advertisement has a Min and Max value per motive; if it varies by personality, the value is scaled linearly by the Sim's points on that axis. Examples: a toilet advertises Bladder from 0 to 70; the aquarium's Feed Fish gives 1 to 11 Fun depending on the Playful trait. [DEV] — [Forbus and Wright](https://www.qrg.northwestern.edu/papers/Files/Programming_Objects_in_The_Sims.pdf)
- Zubek (a Maxis employee who also worked on the Sims at Northwestern) describes the generic form: needs are an array of values in [0,100], "lower is worse and more urgent", they decay over time, and an action refills them. Needs for The Sims "borrowed heavily from Maslow's hierarchy". [DEV] — [Zubek, "Needs-based AI" (GPG8 draft)](http://robert.zubek.net/publications/Needs-based-AI-draft.pdf)
- Personality in Zubek's version is done three cheap ways: per-agent decay-rate multipliers (hunger decays slower, so the agent eats less often), per-agent per-need score multipliers, and attribute masks that filter which advertisements an agent may see (children cannot use the stove). [DEV] — [Zubek](http://robert.zubek.net/publications/Needs-based-AI-draft.pdf)
- The Sims 3: Richard Evans's GDC 2010 talk is "Modeling Individual Personalities in The Sims 3". Wikipedia's summary says action choice used a modified Boltzmann distribution whose "temperature" is low for a happy Sim and high for an unhappy one, with trait-based preference values. The talk's slides were not readable; this is a secondary summary. [SEC] — [GDC Vault listing](https://gdcvault.com/play/1012294/Modeling-Individual-Personalities-in-The); [Wikipedia, Utility system](https://en.wikipedia.org/wiki/Utility_system)
- A search summary of an academic paper on Sims personality says each Sim has up to five traits from a pool of 80. The paper itself was not opened, so treat as unconfirmed. [SEC] — [search result, GDC Vault / Evans cluster](https://gdcvault.com/play/1012294/Modeling-Individual-Personalities-in-The)
- The Sims 4 (low reliability): one aggregator page, itself an archived piece from an AI-generated persona site citing a PC Gamer piece and an EA forum Q&A it did not link, says the lead AI programmer (David "Rez" Graham) rewrote the AI shortly before alpha because earlier planning experiments confused players, and that Sims are "reactive" utility agents that do not plan or pursue long-term goals. Not independently verified. [SEC, low] — [tdp-frontend.pages.dev article, 2026-03-17](https://tdp-frontend.pages.dev/article/2026-03-17-how-a-sims-4-programmer-rewrote-the-entire-ai-from-scratch-mid-development-8fa1b164)

**Dwarf Fortress**
- Tarn Adams (GDC 2025 PC Gamer interview): dwarves are "creatures driven by simulated needs and desires". [DEV] — [PC Gamer](https://www.pcgamer.com/games/sim/dwarf-fortress-dwarves-are-more-human-than-human-creator-says-theyre-allowed-to-embody-bigger-emotions-theyre-allowed-to-make-bigger-mistakes-theyre-allowed-to-do-anything)
- Adams's own essay "Characterization and Emergent Narrative in Dwarf Fortress" (from *Procedural Storytelling in Game Design*): needs are derived from each character's personality facets and intellectual values; dwarves with unmet needs complain and suffer stress and distraction, and satisfied dwarves work more focused; personality is shown to the player as text, not numbers, because text is more shareable. The PDF could not be opened; these points come from search-result excerpts of it. [DEV, via snippets] — [Adams essay PDF (hosted copy)](https://wileywiggins.com//documents/dorf/Adams%20-%20Characterization%20and%20Emergent%20Narrative%20in%20Dwarf%20F.pdf)
- Wiki: facets are rated 1 to 100 (40d page); a value between 40 and 60 is not listed in a dwarf's profile; facets decide how a creature acts, "values" (beliefs) decide what it believes; beliefs and facets together decide which needs exist and how often they must be satisfied before the dwarf's focus drops; large gaps in a belief (one dwarf above 60, the other below 40) can create grudges. The wiki itself says many gameplay effects of facets are unknown. [WIKI, via search snippets] — [DF wiki, Personality](https://dwarffortresswiki.org/Personality); [DF2014 Personality facet](https://dwarffortresswiki.org/index.php/DF2014:Personality_facet)
- Stress: each emotion adds "stress points" to a running "stress score"; unhappy thoughts add, happy thoughts subtract; long-term memories re-trigger emotions when revisited. [WIKI, via snippet] — [DF wiki, Stress](https://dwarffortresswiki.org/Stress)

**RimWorld**
- Needs (wiki list): Food, Rest, Recreation, Beauty, Comfort, Outdoors/Indoors, Chemical, plus DLC needs (Learning, Play, Deathrest, Energy, Hemogen, Kill satiety). Food, rest and recreation fall by themselves and only rise while the pawn eats, sleeps or relaxes. Each need threshold adds or removes a mood "thought". [WIKI] — [RimWorld Wiki, Needs](https://rimworldwiki.com/wiki/Needs)
- Mood = base mood plus the sum of thought offsets. Default base 32; Peaceful and Community Builder 42, Adventure Story 37, Strive to Survive 32, Blood and Dust 27, Losing is Fun 22. Mental break thresholds for a normal colonist: extreme below 5%, major below 20%, minor below 35%. The mood bar moves toward its target at up to +12 per game hour rising and -8 per hour falling; while sleeping the bar stays put. The wiki marks this section a stub and says the tick timing is unclear. [WIKI] — [RimWorld Wiki, Mood](https://rimworldwiki.com/wiki/Mood)
- Recreation falls 2.5% per hour (slower below 30% and below 15%); penalties -5, -10, -20 mood for unfulfilled, deprived, starved. [WIKI, via snippet] — [RimWorld Wiki, Recreation](https://rimworldwiki.com/wiki/Recreation)
- Tynan Sylvester describes the storyteller as modelled on Left 4 Dead's AI Director, trying to "match a rising and falling pacing curve, and not to over-brutally murder struggling players". [DEV, via PC Gamer snippet] — [PC Gamer 2017 via PressReader](https://www.pressreader.com/usa/pc-gamer-us/20170523/283794263693348)

**Crusader Kings 3**
- Dev Diary 104 (AI): four economic archetypes for AI rulers (Warlike, Cautious, Builder, Unpredictable), assigned by personality traits (for example Warlike from Wrathful, Impatient, Sadistic, Ambitious; Cautious from Patient, Calm, Craven, Paranoid; Fickle and Lunatic are never Cautious). Stated goal: previously all AIs played alike and spent most gold on war; now personality drives spending. No numeric weights are published. [DEV] — [CK3 Dev Diary 104 via devtrackers.gg](https://devtrackers.gg/crusader-kings/p/059cf6b8-dev-diary-104-ai-ai-ai)
- Dev Diary 58: the Azure update tied the stress system more closely to personality traits (example: Shy no longer generates stress when launching a personal scheme). [DEV, via snippet] — [CK3 Dev Diary 58](https://forum.paradoxplaza.com/forum/threads/ck3-dev-diary-58-stre-ss-tching-the-traits.1472092)
- A modding forum lists the AI value keys ai_ambition, ai_greed, ai_honor, ai_rationality, ai_zeal (and boldness) as the scalar "personality" the scripted AI weights read; unconfirmed for current versions. [WIKI/forum, via snippet] — [Paradox forum, "how do I make AI more aggressive"](https://admin-forum.paradoxplaza.com/forum/threads/how-do-i-make-ai-more-aggressive.1256004)

**Shadows of Doubt**
- Developer: citizens have jobs, places to be, favourite places to eat and drink, and simulated needs such as caffeine. A later devblog says a "sims-like stat system dictates their minor actions" (eating, drinking, sleeping), replacing routines that were "almost completely pre-computed". [DEV] — [DevBlog 8](https://colepowered.com/?p=34142); [DevBlog 15](https://colepowered.com/?p=34835)

**Oblivion Radiant AI**
- Todd Howard (E3 2005): "We give them general goals, and they figure out on their own how to accomplish them." [DEV, as quoted] — [Paavo, "What was Radiant AI, anyway?"](https://blog.paavo.me/radiant-ai)
- Blogger's own testing: NPCs have no hunger or sleep need that changes their state; schedules include eating and sleeping, but those activities do not change the NPC. Radiant AI is not GOAP: it runs fixed templated "package" procedures (the writer cites a 2009 bit-tech interview in which a Fallout 3 developer described a planner only for combat). [WRITER] — [Paavo](https://blog.paavo.me/radiant-ai)
- Emil Pagliarulo: each NPC gets a scaling set of personality values that can change, with varying dispositions toward the player and each other; behaviour is driven by a goal or by a reaction to another agent. [DEV, as quoted] — [GamesBeat, "Dimming the Radiant AI in Oblivion"](https://gamesbeat.com/dimming-the-radiant-ai-in-oblivion/)

**S.T.A.L.K.E.R.**
- Dmitriy Iassenev (lead programmer) said food and sleep requirements were planned but were not implemented as a character-driven system; smart terrains simply assign tasks such as sleeping, "which is not the same thing". Characters' overarching goal is to uncover the Zone's mystery. Planned need-driven behaviour and "unarmed characters fight over ammo" scenarios were cut because characters received unlimited ammo. [DEV, as reported] — [Game Developer, "Interview: Inside the AI of S.T.A.L.K.E.R."](https://www.gamedeveloper.com/game-platforms/interview-inside-the-ai-of-i-s-t-a-l-k-e-r-i-)

### Inferences
- A per-NPC array of about 8 to 10 decaying scalars, with personality as per-need multipliers on decay and on advertised gain, is the only representation with several shipped, scaled examples (Sims, Shadows of Doubt, RimWorld). Everything beyond that (desires, long goals) has been built as authored content (quests, schedules) rather than as a learned or planned layer.
- "Mood" or "stress" aggregates (RimWorld, DF, CK3) are used for the player-facing story and for triggering rare dramatic breaks, not for routine action choice. In RimWorld and DF the routine choice uses a separate priority list or job system.
- S.T.A.L.K.E.R. and Oblivion are evidence that marketing claims of "NPCs with needs and goals" often ship as scheduled tasks with state-free eating and sleeping. A genuine need loop (state changes, consequences) is a differentiator.

### Gaps
- No primary DF numbers for the number of facets, need types or decay (wiki rate-limited; Adams's essay unreadable).
- Sims 3 trait pool and Boltzmann details were not verified against the talk.
- Per-need decay numbers for The Sims 1 (game minutes per point) were not found in any source read.

---

## 2. How does an NPC pick its next action (competing needs, interruption, personality)?

### Takeaway
Four shipped families exist. (1) Needs plus smart-object advertisement with utility scoring (The Sims, Sims Medieval, Zubek's generic design, Shadows of Doubt in spirit). (2) General utility AI with response curves (Dave Mark's Infinite Axis Utility System, used in Guild Wars 2: Heart of Thorns). (3) Planners (F.E.A.R. GOAP, Killzone/Transformers HTN), best for tactical goals, not for hundreds of routine agents. (4) Schedules plus interruptible state scripts (Gothic TA/ZS, Bethesda packages, KCD routines, RimWorld ThinkTree/work priorities). Commitment (inertia, cooldowns, "only decide when the queue is empty") is the standard fix for dithering.

### Cited Findings

**Smart objects and advertisement (The Sims)**
- Each object holds local data and a list of behaviours; each behaviour has a procedure, a "check" procedure that says whether it is currently possible, and advertisements describing which needs it satisfies. Autonomous Sims "choose what to do by selecting, from all of the possible behaviors in all of the objects, the behavior that maximizes their current happiness"; the behaviour then runs in the Sim's own thread. If the check tree returns false, the behaviour is neither on the pie menu nor available for autonomous selection. [DEV] — [Forbus and Wright](https://www.qrg.northwestern.edu/papers/Files/Programming_Objects_in_The_Sims.pdf)
- Advertisement strength has an "attenuation" with distance (none, low, moderate, high or custom). [DEV] — [Forbus and Wright](https://www.qrg.northwestern.edu/papers/Files/Programming_Objects_in_The_Sims.pdf)
- Runtime: simulation is evolved by running each object in turn on cooperative (non-preemptive) multitasking, with its own thread and local variables. [DEV] — [Forbus and Wright](https://www.qrg.northwestern.edu/papers/Files/Programming_Objects_in_The_Sims.pdf)
- Zubek's generic loop: pop actions from the queue; if empty, run action selection (examine objects, read their advertisements, score each against current needs, pick the best, push its action sequence); else fallback actions. Discovery is decoupled from choice, so an object controls what it advertises (a fridge advertises "clean me" after several uses and only "fix me" when broken). [DEV] — [Zubek](http://robert.zubek.net/publications/Needs-based-AI-draft.pdf)
- Zubek's three scoring functions: (a) trivial sum of future need values; (b) attenuated: score = sum of A(future value), for example A(x) = 10/x, so a thirst rise 10 to 30 scores 1/3 versus 1/9 for a hunger rise to 90; (c) attenuated need-delta: sum of A(current) - A(future), which gives a negative score to actions that worsen a need. Arbitration: winner-takes-all or weighted-random among the top n (for example top 3), "a good compromise". Distance attenuation (for example x/distance^2) causes "bird in hand" behaviour (always the worse thing nearby). He recommends piecewise-linear attenuation curves stored in a spreadsheet. [DEV] — [Zubek](http://robert.zubek.net/publications/Needs-based-AI-draft.pdf)
- Rewards should be granted by the action itself, not by the choice, so interrupted actions pay nothing; actions chain via an action queue, with lazy chaining so a microwave or a broken stove can change the chain. Interrupted chains keep state in the world (for example a "prepped food" object left on the counter). [DEV] — [Zubek](http://robert.zubek.net/publications/Needs-based-AI-draft.pdf)

**Utility details from Sims practitioners**
- The Sims uses piecewise-linear hand-tuned curves so a Sim ignores hunger until a threshold, then gets a bit hungry, then suddenly very hungry. [DEV] — [Graham, "An Introduction to Utility Theory", Game AI Pro](http://www.gameaipro.com/GameAIPro/GameAIPro_Chapter09_An_Introduction_to_Utility_Theory.pdf)
- Picking only the top score "can feel very robotic"; pure weighted random "every now and then" picks "something utterly stupid"; the compromise is weighted random among the top few (top 5, or within 10% of the best). [DEV] — [Graham](http://www.gameaipro.com/GameAIPro/GameAIPro_Chapter09_An_Introduction_to_Utility_Theory.pdf)
- Bucketing ("dual utility", credited to Dill): "On The Sims, motives are bucketed based on their utility value. The highest scoring motives are grouped into a bucket, and none of the motives below are considered", so a starving Sim never scores watching TV unless nothing can solve hunger. Example in the chapter: Hunger bucket scores 0.8, Fun bucket 0.4. [DEV] — [Graham](http://www.gameaipro.com/GameAIPro/GameAIPro_Chapter09_An_Introduction_to_Utility_Theory.pdf)
- Inertia: add weight to the current action, use cooldowns, or stall deciding; on The Sims Medieval "a Sim would only attempt to make a decision when their interaction queue was empty". [DEV] — [Graham](http://www.gameaipro.com/GameAIPro/GameAIPro_Chapter09_An_Introduction_to_Utility_Theory.pdf)

**Infinite Axis Utility System (IAUS), Guild Wars 2: Heart of Thorns**
- Per agent a set of decisions; each mapped to a decision score evaluator (DSE). Each consideration is a raw input normalised to [0,1], remapped by a response curve, and the considerations are multiplied to give the DSE score. Any consideration scoring 0 vetoes the decision and the system early-outs, so cheap considerations are ordered first (a line-of-sight raycast is ordered last because it is expensive). The best-scoring DSE per "think cycle" wins. [DEV] — [Lewis, "Choosing Effective Utility-Based Considerations", Game AI Pro 3](http://www.gameaipro.com/GameAIPro3/GameAIPro3_Chapter13_Choosing_Effective_Utility-Based_Considerations.pdf)
- Dithering fixes in the IAUS: runtime and cooldown considerations, a "commitment bonus" for the last-chosen decision, decision weights (total can be 3 or 4 instead of 1). Lewis warns these "shift where the scores will land" and do not eliminate oscillation. Most response curves come from a small preset palette; arbitrary curves should sit behind an "advanced mode". [DEV] — [Lewis](http://www.gameaipro.com/GameAIPro3/GameAIPro3_Chapter13_Choosing_Effective_Utility-Based_Considerations.pdf)
- Mark and Lewis presented the IAUS at GDC AI Summit 2015 ("Building a Better Centaur: AI at Massive Scale"); Mark introduced it in 2013 ("Architecture Tricks: Managing Behaviors in Time, Space, and Depth"). A vendor page claims designers can build AI packages "in as little as 7 minutes" (marketing). [SEC/vendor] — [Wikipedia, Utility system](https://en.wikipedia.org/wiki/Utility_system); [gameai.com IAUS](https://gameai.com/iaus.php)

**GOAP (F.E.A.R., 2005)**
- The character FSM has only three states (Goto, Animate, UseSmartObject); goals compete for activation and an A* search over action space plans the sequence. Differences from STRIPS: a cost per action (example OrderPizza 2.0 versus BakePie 8.0), no add/delete lists (world state is an array of four-byte values), procedural preconditions (for example an expensive pathfinding check), procedural effects. Different characters get the same Goal Set but different Action Sets (a rat has no attack action, so it falls back to Patrol). Replanning when a door is blocked makes the soldier kick the door. Motivation: only one AI programmer, and the previous goals were black boxes with embedded FSMs. [DEV] — [Orkin, "Three States and a Plan" GDC 2006](https://www.gamedevs.org/uploads/three-states-plan-ai-of-fear.pdf)
- Orkin notes the planner only reasons about one target and one weapon at a time (targeting systems choose the focus) to keep search small, and says HTN may suit squad planning better. [DEV] — [Orkin](https://www.gamedevs.org/uploads/three-states-plan-ai-of-fear.pdf)

**Comparison chapter (Game AI Pro 1, ch. 4)**
- Utility: suited to many possible actions with no single right answer and competing continuous inputs; "more art than science"; unpredictability is a plus for believability but needs overrides for scripted moments. Behaviours in a behaviour tree are stateless: a citizen fleeing a battle can loop between two behaviours; planners handle that more easily. GOAP is backward-chaining, designed from the world's mechanics, but can produce odd plans (the "Big Red Barrel"); HTN is a forward planner, the designer writes the task network, search size is controlled by culling methods and by partial plans; GOAP is "significantly easier to design than HTN". Used by: GOAP in F.E.A.R., Just Cause 2, Deus Ex: Human Revolution; HTN in Killzone 2 and Transformers: Fall of Cybertron. [DEV] — [Dawe et al., "Behavior Selection Algorithms: An Overview"](http://www.gameaipro.com/GameAIPro/GameAIPro_Chapter04_Behavior_Selection_Algorithms.pdf)

**Schedule plus interrupt systems**
- RimWorld social interaction choice: each interaction type has a base weight; deep talk 0.075 times compatibility (about 0 to 3); insults and slights weighted by the target's opinion (low opinion raises it a lot), abrasive trait times 2.3; compatibility is a hidden per-pair random value (normal, mean 0.3) plus an age-gap term. [WIKI] — [RimWorld Wiki, Social](https://rimworldwiki.com/wiki/Social)
- RimWorld work and sleep: the Work tab is a search order, leftmost category first, with optional manual numeric priorities on top; a pawn scheduled to sleep is woken if food falls below 30% or rest below 75%, or if food falls below 12.5% with an allowed food reachable (as summarised from the Menus page). Internals (ThinkTree XML, JobGivers) were not retrievable. [WIKI, via snippets] — [RimWorld Wiki, Menus](https://rimworldwiki.com/wiki/Menus); [Steam discussion on work priority order](https://steamcommunity.com/app/294100/discussions/0/2381701715727097818)
- Bannerlord lords: distance is the one factor common to all campaign actions; others vary by action (go to settlement to restock, recruit, ransom, rest; patrol; defend, favouring prosperous or own settlements; raid, weighing production, strength, clan relations and feuds; besiege; chase, avoiding faster parties). Lords follow the same rules as the player (restock, recruit, ransom). [DEV, via GameBanshee summary of the final 2019 dev blog] — [GameBanshee](https://www.gamebanshee.com/3wfc8)
- KCD (lead AI developer Tomas Plch, German interview): routines are not fixed; one NPC might fish in one playthrough and play backgammon in a tavern in another; an NPC knows three taverns and asks the tavern's AI whether a seat is free; a failed visit is stored as a negative experience so another tavern is chosen next time. Dev quote on design: scripts that are too rigid make behaviour repetitive, too much freedom frustrates players. [DEV, via translated interview] — [PC Games, KCD interview part 5](https://www.pcgames.de/Kingdom-Come-Deliverance-Spiel-16151/Specials/Kingdom-Come-Deliverance-Interview-Special-Teil-5-Kuenstlich-intelligenter-Ehebruch-und-denkende-Tueren-1156484/2/)
- KCD2 lead scripter Martin Ziegler: NPCs eat breakfast with others and leave for work; they alter routine after recent violence or theft in the region (stay home, carry weapons, call for patrols); they interrupt schedules for events "continuously in the entire world, whether you are there to see it or not". [DEV] — [Wccftech, quoting the Epic Games Store interview](https://wccftech.com/kingdom-come-deliverance-2-dev-talks-about-dynamic-npc-routines-and-emergent-storytelling-for-side-quests/amp/)
- Bethesda packages (Oblivion): the Paavo analysis counts about 7,200 packages in Oblivion (about 50% scheduled), about 6,000 in Skyrim (about 25%), about 3,500 in Starfield (about 6%); the "8x12" naming means 12 hours starting 8:00. Skyrim made package templates reusable in the editor. The writer's inference: Oblivion's best stories came from the generic "Find" package. [WRITER] — [Paavo](https://blog.paavo.me/radiant-ai)
- Watch Dogs: Legion: characters have 24-hour schedules repeating daily until something changes; scheduled activities can involve two or more characters converging on the same place and time; player actions (for example hospitalising someone) interrupt schedules and ripple to the social circle. [DEV] — [Game Developer, Watch Dogs Legion simulation](https://gamedeveloper.com/design/how-watch-dogs-legion-s-play-as-anyone-simulation-works)
- Shadows of Doubt: the average citizen makes about 4 to 10 journeys per day; daily activities are planned in advance and real-time deviations are limited ("never more than a handful of citizens requiring deviations"); later a goal-based system with a sims-like stat system for minor actions. [DEV] — [DevBlog 8](https://colepowered.com/?p=34142); [DevBlog 15](https://colepowered.com/?p=34835)

### Inferences
- The cheapest design that already satisfies "needs, many NPCs, personality, smart objects" is Zubek/Sims: advertisement from objects (beds, benches, fishing spots, FreePoints), attenuated need-delta score, piecewise-linear curves, bucket by most urgent need, weighted random among the top few, commit until the action ends. It matches Gothic's existing FreePoint/mobsi world model.
- Planners (GOAP/HTN) were shipped for small squads in combat with one target in focus; no source found shows them driving hundreds of routine social agents. Orkin and the Game AI Pro chapter both constrain the planner's scope.
- All sources agree on a commitment mechanism; the specific form differs (queue empty, cooldown, commitment bonus). Without it, dithering appears.

### Gaps
- Dave Mark's compensation factor formula for multiplying many considerations is not in any page read (Mark's GDC slides unreadable); only the "multiply and veto on zero" rule is confirmed (Lewis).
- RimWorld ThinkTree and JobGiver internals not retrieved; only player-side priority behaviour.
- Halo 2 behaviour tree (Isla) and Killzone HTN performance numbers were not retrieved.
- CK3 ai_will_do and scheme weight formulas not retrieved.
- No source found on how "everyone queues for one object" is handled in The Sims (occupancy, slots); Forbus mentions "routing slot" of a shower only.

---

## 3. Property, jobs, trade, relationships, knowledge, social obligations

### Takeaway
Shipped games model these very unevenly. Relationships are usually a per-pair opinion number with event deltas that decay (RimWorld, Watch Dogs: Legion). Knowledge is either implicit in the world or a limited witness-memory with decay (Shadows of Doubt, KCD tavern memory, Watch Dogs: Legion memory of player actions). Property and law are the weakest area: Dwarf Fortress's own creator says it lacked models of law, property, customs, status and economics; Radiant AI's "NPCs buy and steal" mostly did not ship.

### Cited Findings

**Relationships**
- RimWorld: opinion is the sum of beauty (+/-20 per level, cap +/-40), trait modifiers (for example annoying voice -25, body modder +8 per part up to +40), and timed event thoughts (chitchat +10, deep talk +15, insult -15, harmed -15 for 10 days, rescue +15 or +25, recruitment +20). Tiers: rival -100 to -20, acquaintance -20 to 20, friend 20 to 100. Social fight chance 4% per insult and 0.5% per slight, multiplied by malnutrition (x1.5 to x3), bloodlust (x4), intoxication and others. Family opinion: parents/children +30, siblings +20; lovers +35; spouses +30; a breakup gives -50 and an affair gives the spouse -70. [WIKI] — [RimWorld Wiki, Social](https://rimworldwiki.com/wiki/Social)
- Dwarf Fortress: relationships are affected by marked personality facets and belief gaps (grudges). [WIKI, via snippet] — [DF2014 Personality facet](https://dwarffortresswiki.org/index.php/DF2014:Personality_facet)
- Watch Dogs: Legion: opinions are tracked per individual and per organisation (DedSec); area-level opinion is not derived; helping someone makes relatives like you more; some characters hate DedSec so much they cannot be recruited and may trigger revenge missions. [DEV] — [Game Developer, Watch Dogs Legion simulation](https://gamedeveloper.com/design/how-watch-dogs-legion-s-play-as-anyone-simulation-works)
- Bannerlord: raid and chase choices weigh clan relations, favouring feuds. [DEV] — [GameBanshee](https://www.gamebanshee.com/3wfc8)

**Knowledge and memory**
- Shadows of Doubt: citizens' memories start "pretty much 100% accurate" and fuzz over time until lost; retention depends on familiarity, visual distinctiveness, unusual behaviour, witness alertness and memory quality (age); time accuracy degrades before the memory is lost; a global check loops through travelling citizens to find who sees whom. Citizens fearing suspicion may lie. [DEV] — [DevBlog 8](https://colepowered.com/?p=34142)
- KCD: a failed visit is stored as a negative experience for tavern choice. [DEV, via translated interview] — [PC Games interview](https://www.pcgames.de/Kingdom-Come-Deliverance-Spiel-16151/Specials/Kingdom-Come-Deliverance-Interview-Special-Teil-5-Kuenstlich-intelligenter-Ehebruch-und-denkende-Tueren-1156484/2/)
- Watch Dogs: Legion: a memory system records player actions and spreads them to friends and enemies, and feeds back into character generation to explain opinions. [DEV] — [Game Developer](https://gamedeveloper.com/design/how-watch-dogs-legion-s-play-as-anyone-simulation-works)
- Gothic has a "Spreading the News" page and a Perceptions page in the Inside-Gothic site (listed in its navigation, not read). [WIKI] — [Inside-Gothic index](https://ataulien.github.io/Inside-Gothic/)

**Jobs, homes, property, economy**
- Watch Dogs: Legion "Census": each spawned citizen has tags (gender, fashion, occupation, income, personality); generation builds outward, for example job then workplace then a home matched to income. [DEV] — [Game Developer](https://gamedeveloper.com/design/how-watch-dogs-legion-s-play-as-anyone-simulation-works)
- Shadows of Doubt: each citizen has a home, a job, fingerprints, a routine (game page and devblog). [DEV] — [DevBlog 8](https://colepowered.com/?p=34142)
- Sims 3 story progression (2009 SimGuru post): neighbours get jobs, promotions, demotions, firings; careers need a boss and coworkers; if a career needs filling the game first looks for a living Sim to promote, then may assign a neighbour as boss, otherwise a random Sim; Sims move between homes based on relationships; "townies" are created by careers. Primary goal is town stability (no empty world, enough children and single adults), secondary is the feeling of own lives. [DEV] — [SimGuruLyndsay on Story Progression, 2009](https://beyondsims.com/2009/07/simgurulyndsays-info-on-story-progression-in-ts3/)
- Dwarf Fortress: Tarn Adams said political intrigue is deferred because the game "lacks models of law, property, customs, status, and economics" and personal relationships and territories are not modelled well enough. (Article paraphrase of a PC Gamer interview, 2016; the game has since changed.) [DEV, paraphrased] — [Game Developer, 2016](https://gamedeveloper.com/design/-i-dwarf-fortress-i-figuring-out-how-to-simulate-the-universe-one-step-at-a-time)
- Oblivion: a blogger found that NPCs cannot buy anything (one package uses the GetGold condition, unrelated to buying) and cannot steal from the player; Pete Hines said NPC theft was scaled back so players would not assume items vanished by a bug. Skyrim's Acquire procedure has "Allow stealing/pickpocketing/killing" options that appear unused in vanilla. Crime reporting runs through factions; almost every non-hostile actor, even chickens, can report crimes. [WRITER with DEV quotes] — [Paavo](https://blog.paavo.me/radiant-ai)
- UO (Ultima Online) 1997: Koster says the closed resource loop (everything spawned from a fixed pool) caused the economy to run out when players hoarded wool shirts made to grind skills: the "central bank ran out of wool, and then couldn't spawn more sheep". [DEV] — [Raph Koster, "Did players destroy the UO ecology?"](https://www.raphkoster.com/?p=46439)
- Bannerlord lords follow player-like money and supply rules. [DEV] — [GameBanshee](https://www.gamebanshee.com/3wfc8)

**Gothic baseline of ownership**
- Gothic shares "waypoints" as rough markers, not owned spots; for item-based activities an NPC goes to the area and looks for a free item (for example an anvil), and if it is occupied may do something else. [WIKI] — [Inside-Gothic, Daily Routine](https://ataulien.github.io/Inside-Gothic/DailyRoutine)

### Inferences
- A practical set for a Gothic-like society: opinion-per-pair with decaying deltas (RimWorld numbers give a tuning starting point), a witness memory with per-item accuracy decay (Shadows of Doubt), home/work/owner bindings generated at world build (Census style), and law/property expressed through factions (Bethesda crime reporting via factions).
- The best-documented "knowledge" implementations are small: a per-NPC list of remembered events/experiences with decay, plus lookups by object. No source describes a large knowledge graph at scale in a shipped game.

### Gaps
- No primary source found for ownership representation in The Sims 1/2/3 (who owns an object, theft).
- No documentation found for DF's current (v50) economy, property and law models; the wiki was not accessible.
- Kenshi and Mount & Blade trade/job models: no reliable sources found in the searches (Kenshi sources were only marketing text, Steam threads and a fan mod).
- Gothic's own "Spreading the News" mechanism was not read.

---

## 4. Scale, LOD, per-tick cost and time skips

### Takeaway
Shipped societies are small or tiered: roughly 600 persistent NPCs (KCD1) to about 2,400 (KCD2), about 1,000 in Oblivion, "hundreds" active at once in Watch Dogs: Legion and Shadows of Doubt. Only a small near-player set runs full AI; the rest is a cheaper "offline" layer (S.T.A.L.K.E.R. calls it the LOD of online). Public per-tick CPU numbers are almost absent. Cost control comes from structure: score only objects in range, early-out on zero, plan the day in advance, throttle update rate by distance, and cap the population.

### Cited Findings

**Population numbers**
- KCD2: "nearly 2,400" NPCs on the map (roughly four times KCD1), about half concentrated in one city; new AI simulation LOD techniques were needed to keep frame times and memory low; the scripting interface was designed to be as unaware of LOD level as possible (GDC 2026 session by Petr Smrcek, Warhorse). [DEV, session description] — [GDC Vault](https://gdcvault.com/play/1035718/Supporting-Thousands-of-NPCs-in)
- KCD1 about 600 NPCs active at once with jobs, homes, daily cycle: from a search-tool summary of a 2018 Czech presentation by Smrcek; the PDF was not retrievable. [SEC, unverified] — [search result for Smrcek presentation](https://www.karlin.mff.cuni.cz/mffseminar/prezentace_smrcek.pdf)
- Oblivion: about 1,000 NPCs (Bethesda website and Game Informer, 2004, as cited by the blogger); four AI processing levels (low, low-middle, high-middle, high) with only "high" running in loaded cells, per the blogger's reading of OBSE and EngineBugFixes source. [WRITER] — [Paavo](https://blog.paavo.me/radiant-ai)
- Starfield: crowd NPCs are generated when the player enters; they have no homes, schedules, names or persistent data. Schedules "almost gone"; New Atlantis shops open 24/7. [WRITER] — [Paavo](https://blog.paavo.me/radiant-ai)
- Shadows of Doubt: "100s of little citizens"; the player will "likely never interact with 90% of them". The game needs "typically no more than 10-15 seconds" of calculation before each day depending on population; the developer's stated target is making "the performance cost of 95% of the citizens at any one time relatively insignificant". [DEV] — [DevBlog 8](https://colepowered.com/?p=34142); [DevBlog 15](https://colepowered.com/?p=34835)
- Watch Dogs: Legion: hundreds of citizens active at once; older unengaged characters are cycled out; three tiers (fully animated spawned, "fully uprezzed" contacts, incidental related characters); background citizens are deleted once the player is far away unless engaged. [DEV] — [Game Developer](https://gamedeveloper.com/design/how-watch-dogs-legion-s-play-as-anyone-simulation-works)
- Sims 3: the open neighbourhood runs everything at once, "too many Sims would reduce performance on many machines", so population is capped. [DEV] — [SimGuruLyndsay 2009](https://beyondsims.com/2009/07/simgurulyndsays-info-on-story-progression-in-ts3/)

**Offline or off-screen simulation**
- S.T.A.L.K.E.R. (Iassenev): offline behaviour "can be considered the LoD of the online"; offline characters skip animations, sounds, inventory management and path smoothing, still path over a global navigation graph; combat is resolved abstractly by turns with "a formula and the random factor"; characters near the player switch to online, radius "about 150 meters" but varying by level and designer-overridable; offline activity surfaces as generated news. [DEV, interview] — [Game Developer interview](https://www.gamedeveloper.com/game-platforms/interview-inside-the-ai-of-i-s-t-a-l-k-e-r-i-)
- An article by a game developer (not GSC) repeats about 150 m and says online events are recorded and applied to offline AI as game time passes; Iassenev's term is the "detailed graph". [SEC] — [Cicala, Game Developer blog](https://gamedeveloper.com/design/a-life-an-insight-into-ambitious-ai)
- S.T.A.L.K.E.R. 2 (2024) A-Life 2.0: GSC said in November 2024 that A-Life was meant to fuel regions with events and migrating groups but often spawned fights near the player instead, attributing it to an optimisation compromise (a larger simulation area needs much more memory; streaming and Xbox Series budgets). Patch 1.5 (25 June 2025, as reported) restored off-screen life. Community estimates of the visualisation radius are about 80 to 100 m, not official. [DEV and SEC] — [CloudsPress guide](https://www.cloudspress.com/stalker-2-a-life-2-0-explained-how-the-zones-npc-simulation-works/); [ITC.ua coverage](https://itc.ua/en/news/the-creators-of-s-t-a-l-k-e-r-2-explained-why-the-a-life-2-0-system-does-not-work-as-promised)
- Watch Dogs: Legion: off-screen characters are not loaded but "their paths are simulated at a high enough rate to place them accurately"; the metro is a fast-travel node. [DEV] — [Game Developer](https://gamedeveloper.com/design/how-watch-dogs-legion-s-play-as-anyone-simulation-works)
- Shadows of Doubt: AI update rate is "high only when nearest the player"; paths are calculated dynamically; routines are planned in advance and only a handful of citizens need real-time deviation. [DEV] — [DevBlog 15](https://colepowered.com/?p=34835); [DevBlog 8](https://colepowered.com/?p=34142)
- Dwarf Fortress: world "activities" continue after world generation (succession, marriages, children, jobs, deaths); off-site historical figures die of old age off-site, while figures on the fortress map only die of old age at the turn of the year; older versions stopped any activity after world generation. [WIKI, via snippet] — [DF wiki, World activities](https://dwarffortresswiki.org/index.php/World_activities)
- Sims 3 story progression is an off-screen layer for neighbours: job changes, moves, weight changes, meant to happen gradually. [DEV] — [SimGuruLyndsay 2009](https://beyondsims.com/2009/07/simgurulyndsays-info-on-story-progression-in-ts3/)
- UO (1996 to 1997): Koster says the AI ecology (creatures hunting and eating) was cut because "radial searches followed by pathfinding" were too expensive; the engineer who rewrote it "didn't really like or understand it". [DEV] — [Koster](https://www.raphkoster.com/?p=46439)

**Per-tick cost data that exists**
- Zubek: needs-based AI is "computationally very efficient; only a trivial amount of the CPU is required to pick what to do". No numbers. [DEV] — [Zubek](http://robert.zubek.net/publications/Needs-based-AI-draft.pdf)
- IAUS: ordering cheap considerations first and early-out on a zero score "is a large part of what made the Heart of Thorns AI sufficiently performant". [DEV] — [Lewis](http://www.gameaipro.com/GameAIPro3/GameAIPro3_Chapter13_Choosing_Effective_Utility-Based_Considerations.pdf)
- F.E.A.R. GOAP: expensive pathfinding is kept out of the planner via procedural preconditions; one target and one weapon at a time. No timings in the paper. [DEV] — [Orkin](https://www.gamedevs.org/uploads/three-states-plan-ai-of-fear.pdf)
- HTN cost is controlled by culling method branches and partial plans (for example only plan NavigateToEnemy when out of range). [DEV] — [Game AI Pro ch. 4](http://www.gameaipro.com/GameAIPro/GameAIPro_Chapter04_Behavior_Selection_Algorithms.pdf)
- CK2/CK3 modding: decisions have an `ai_check_interval` so rarely needed decisions are not evaluated every pulse; forum advice is to trim targets in the `potential` block and reduce check frequency. [DEV/WIKI, via snippet] — [CK2 Dev Diary 71, AI and optimization](https://admin-forum.paradoxplaza.com/forum/developer-diary/ck2-dev-diary-71-ai-and-optimization.1050551/page-2)
- Bannerlord: TaleWorlds said many campaign parties each constantly calculate distance to targets; a recap reported campaign-map fast-forward improving from 80 to 90 FPS on a GTX 1080 / i7-8700K with no party count given. [DEV, via snippet] — [TaleWorlds one-year recap](https://www.taleworlds.com/en/News/446)
- Shadows of Doubt's whole-city pre-day plan costing 10 to 15 seconds is the only concrete wall-clock number for a daily-plan approach. [DEV] — [DevBlog 8](https://colepowered.com/?p=34142)

**Time skips**
- No developer source read describes a general "catch up after a long skip" algorithm. The nearest facts are: S.T.A.L.K.E.R. records online events and applies them to offline AI as time passes [SEC]; Shadows of Doubt pre-plans each day [DEV]; DF's off-site figures age and die off-site [WIKI]; Gothic runs a routine from a time-keyed todo-list (below), so positions follow from the clock. KCD2 says NPCs act "whether you are there to see it or not" [DEV].

### Inferences
- If the target is hundreds to a few thousand NPCs, shipped precedent says: full simulation for tens near the player, a path-only or schedule-only layer for the rest, and a coarse statistical layer (Sims 3 story progression style) for slow social change. KCD2 (about 2,400) and Oblivion (about 1,000) are the closest scale references for a hand-authored RPG.
- Per-NPC need scoring is cheap enough (Zubek, IAUS) that the expensive parts are line of sight, pathfinding and perception; every cited developer moved those out of the decision step or throttled them.

### Gaps
- No published per-NPC microsecond or millisecond cost for The Sims, DF, RimWorld, Radiant AI, A-Life or KCD was found. Treat any such figure in later reports as unsourced.
- Dwarf Fortress FPS-versus-population statements by Tarn Adams: not found.
- Kenshi's off-screen handling: no reliable source (only marketing copy, Steam posts, a fan mod).
- Skyrim time-skip (wait/sleep) handling of NPC positions: not found.
- The KCD1 600 NPC figure and the KCD LOD level definitions: the detailed talk is members-only.

---

## 5. Failure modes: degenerate loops, emergent crime, "too random" and "too robotic"

### Takeaway
The documented failures fall into five groups: addiction loops from mis-tuned or false advertising (Joy Booth); emergent violence that broke quests and was dialled back (Radiant AI skooma); closed-loop economies collapsing (UO); stress cascades (DF tantrum spirals, RimWorld social fights); and LOD or optimisation compromises that breaks the illusion of a living world (STALKER 2). Player perception: selecting only the top score reads as robotic, pure weighted random reads as stupid, and unexplained AI behaviour is not read as intelligent.

### Cited Findings
- Joy Booth (The Sims): a transmogrified shower that gives a large Fun reward. "Joy Booths quickly become popular household items"; Sims use it so much they socialize less, relationships weaken and they lose sleep; the authors call it "SimHeroin", "something that the Sims find altogether too addicting". The paper's own design rule: objects that confer a benefit should also have a cost, preferably indirect. [DEV] — [Forbus and Wright](https://www.qrg.northwestern.edu/papers/Files/Programming_Objects_in_The_Sims.pdf)
- False advertising (reward promised but not granted) makes a hungry agent re-pick the same action again and again, producing "addictive" loops that are "very difficult to tune"; forcing an action is easier by pushing it directly on the queue. [DEV] — [Zubek](http://robert.zubek.net/publications/Needs-based-AI-draft.pdf)
- Zubek on global tuning: need set, decay rates and attenuation functions apply to all characters, so tuning is delicate once the game grows; split the world into partitions with local overrides. Needs-based AI is poor for triggered scripted sequences. Add thought bubbles so the player understands why the agent acts. [DEV] — [Zubek](http://robert.zubek.net/publications/Needs-based-AI-draft.pdf)
- Utility oscillation: two actions scoring about 0.5 each flip frame to frame ("frantic"); fixes are inertia weights, cooldowns and waiting for the queue to empty. [DEV] — [Graham](http://www.gameaipro.com/GameAIPro/GameAIPro_Chapter09_An_Introduction_to_Utility_Theory.pdf)
- Naive hunger curves: with a smooth quadratic, a nearly full Sim would still occasionally choose to eat; designers want "ignore until a threshold" so piecewise curves are used. [DEV] — [Graham](http://www.gameaipro.com/GameAIPro/GameAIPro_Chapter09_An_Introduction_to_Utility_Theory.pdf)
- Distance attenuation causes "bird in hand": the agent prefers a worse nearby action over a better distant one. [DEV] — [Zubek](http://robert.zubek.net/publications/Needs-based-AI-draft.pdf)
- Behaviour trees: stateless behaviours can loop ("citizen fleeing a battle then returning to it"); GOAP can find odd plans ("Big Red Barrel"). [DEV] — [Game AI Pro ch. 4](http://www.gameaipro.com/GameAIPro/GameAIPro_Chapter04_Behavior_Selection_Algorithms.pdf)
- Radiant AI: Pagliarulo said developers "have had to consciously tone down" NPC behaviours because "things can go haywire when NPCs have a mind of their own"; the famous case is Dark Brotherhood quest, where skooma-den NPCs out of skooma killed the merchant before the player arrived. The blogger tested the final game and argues the merchant case cannot happen in the shipped version (addicts' packages are Find and Eat, skooma is not food, the dealer's stock is in a vendor chest). Pete Hines said NPC theft was scaled back so players would not assume items vanished due to a bug. "Hungry guard" anecdote: the blogger calls it very likely made up. [DEV quotes, WRITER testing] — [GamesBeat](https://gamesbeat.com/dimming-the-radiant-ai-in-oblivion/); [Paavo](https://blog.paavo.me/radiant-ai)
- UO ecology: the AI was working in alpha (E3 spring 1996), removed in beta after a rewrite; Koster says it was cut for the cost of radial search plus pathfinding, and that the ecology's later collapse was a separate problem, a closed economy exhausted by players hoarding crafted shirts. He calls the popular story (hunting carnivores for pelts) senseless. [DEV] — [Koster](https://www.raphkoster.com/?p=46439)
- Dwarf Fortress: a tantrum can trigger tantrums in others ("tantrum chain", or "tantrum spiral" if it threatens the fortress); injuries and the destruction of a masterwork add stress; the wiki says the current mechanics are not well known. [WIKI, via snippets] — [DF wiki, Tantrum](https://dwarffortresswiki.org/Tantrum)
- RimWorld social fights: base chance 4% per insult, 0.5% per slight, multiplied by malnutrition, bloodlust (x4), intoxication up to x5, so a low-need, high-stress colony makes social fights more likely. [WIKI] — [RimWorld Wiki, Social](https://rimworldwiki.com/wiki/Social)
- S.T.A.L.K.E.R. 2 launch: A-Life spawned fights near the player instead of simulating regions; GSC called it an optimisation compromise; symptoms reported included close-range spawns and mid-air spawns. [DEV and SEC] — [CloudsPress](https://www.cloudspress.com/stalker-2-a-life-2-0-explained-how-the-zones-npc-simulation-works/)
- Watch Dogs: Legion: unconstrained generation produced "procedural soup" (characters looked random); pure stereotypes were the opposite risk; early commentary was too frequent so cooldowns were added; side missions interrupted main-story cliffhangers until a "player attention" request system was added. [DEV] — [Game Developer](https://gamedeveloper.com/design/how-watch-dogs-legion-s-play-as-anyone-simulation-works)
- Sims 3: with story progression off and aging on, neighbours gradually disappear and the world empties; with it on, overpopulation leads households to leave the world; the system's top goal is "town stability". [DEV] — [SimGuruLyndsay 2009](https://beyondsims.com/2009/07/simgurulyndsays-info-on-story-progression-in-ts3/)
- KCD AI lead (Plch): scripts too rigid make behaviour repetitive; too much freedom frustrates players; changing weapon damage can break the AI's balance. [DEV, translated] — [PC Games](https://www.pcgames.de/Kingdom-Come-Deliverance-Spiel-16151/Specials/Kingdom-Come-Deliverance-Interview-Special-Teil-5-Kuenstlich-intelligenter-Ehebruch-und-denkende-Tueren-1156484/2/)
- Gothic 1 Switch fix list includes NPCs no longer being interrupted and then instantly returning to their activity when the player is close (a proximity-related routine bug). [SEC, via snippet] — [THQ Nordic, Gothic 1 Switch fix list](https://thqnordic.com/article/gothic-1-switch-list-fixes)

### Inferences
- The most reproducible failure for an ad-based need system is the "popular object" (Joy Booth) and the false-reward loop. Guards: grant rewards in the action, not at choice; every beneficial advertisement has an indirect cost; cap repeated selection (runtime or cooldown considerations).
- Emergent crime was cut in practice by designers, because quest-critical NPCs and items were affected. A quest-protected flag or essential set is the shipped answer.
- Perception data points: robotic if argmax, stupid if pure random, "procedural soup" if generated attributes are unconstrained. Top-few weighted random, thought bubbles or reasons, and generation with constraints are the three documented remedies.

### Gaps
- "Everyone queuing for one object": no source found that documents it directly. Closest are the Joy Booth popularity and Zubek's distance attenuation. Gothic's occupied-freepoint fallback is the only documented handling.
- DF "drunk cats" story is only referenced in a Game Developer article as being in the full interview; text not found.
- No source for emergent bugs in Kenshi, Bannerlord or Shadows of Doubt beyond the devblog's "teething problems".

---

## 6. Baseline: how Gothic 1 and 2 run daily routines

### Takeaway
Gothic NPCs run a time-keyed todo-list of `TA_` entries; each entry starts a script state (`ZS_*`) with init, loop and end functions; any other state (damage, dialogue) interrupts it and the routine restarts when the NPC is idle. Waypoints are coarse markers; freepoints and mobsis are the claimable "smart objects". Gothic has no needs layer and no ownership beyond script flags, but the structure maps well onto an advertisement model.

### Cited Findings
- Routines are Daedalus functions (for example `Rtn_Start_1`) that run once when the NPC spawns or changes routine. Each `TA_` call adds an activity (time span, waypoint) to the NPC's todo-list, and every `TA_` function maps to the external `TA_Min`. [WIKI] — [Inside-Gothic, Daily Routine](https://ataulien.github.io/Inside-Gothic/DailyRoutine)
- When an activity's time arrives, a matching state such as `ZS_SMALLTALK` starts; states have optional `init` (once), `loop` (every frame while active and nothing else is pending; it can decide whether to continue) and `end` functions; at the next activity time the current state is cancelled gracefully. [WIKI] — [Inside-Gothic, Daily Routine](https://ataulien.github.io/Inside-Gothic/DailyRoutine)
- `ZS_SMALLTALK` example: sets slow-walk mode, walks to the waypoint, claims an unoccupied SMALLTALK freepoint and faces it. Waypoints are not owned by an NPC; shared waypoints make NPCs meet by chance daily. For item activities the NPC goes to the area and looks for a free item (an anvil); if occupied it may do something else. [WIKI] — [Inside-Gothic, Daily Routine](https://ataulien.github.io/Inside-Gothic/DailyRoutine)
- Interruption: assigning any other state (taking damage, dialogue starting `ZS_TALK`) interrupts the routine state; after it ends and the NPC has nothing to do, the routine starts again. Following the player and guiding the player are also implemented as routines. [WIKI] — [Inside-Gothic, Daily Routine](https://ataulien.github.io/Inside-Gothic/DailyRoutine)
- Monsters do not use this routine code; they use a separate monster scheduler. [WIKI, via snippet] — [Inside-Gothic, Monster AI](https://ataulien.github.io/Inside-Gothic/monster-ai)
- The page does not say how often routines are checked beyond the per-frame `loop`, how far-away NPCs are handled, or whether a restarted routine resumes mid-activity. [WIKI, absence] — [Inside-Gothic, Daily Routine](https://ataulien.github.io/Inside-Gothic/DailyRoutine)

### Inferences
- A need layer can sit as a new class of state: a need-driven `ZS_*` that reuses existing freepoints/mobsis as advertisements, and that the TA todo-list can schedule or preempt, using the interrupt-and-resume semantics Gothic already has.
- The A-Life lesson applies: location-assigned tasks (smart terrain) shipped; true food/sleep need loops were planned but not delivered in S.T.A.L.K.E.R.

### Gaps
- Far-NPC and world-load behaviour in the original Gothic engine: not documented in any page read.
- Gothic's perception and "spreading the news" mechanisms: pages exist but were not read.

---

## 7. Source quality and coverage map

### Takeaway
The strongest primary material found is the Forbus and Wright Sims notes, Zubek's Maxis-derived chapter, Graham and Lewis on utility AI, Orkin on F.E.A.R., Koster's own posts, Iassenev's interview, the Shadows of Doubt devblogs and the Watch Dogs: Legion developer interview. Dwarf Fortress, CK3 internals, Kenshi, Bannerlord scale and per-tick CPU numbers are the weakest.

### Cited Findings
- Game AI Pro chapters are freely hosted at gameaipro.com; the three read here were extracted as text (ch. 4, ch. 9, GAP3 ch. 13). — [Game AI Pro](http://www.gameaipro.com)
- Forbus and Wright notes are for Edith in the original Sims (not expansion packs), version of 31 May 2001. — [Forbus and Wright](https://www.qrg.northwestern.edu/papers/Files/Programming_Objects_in_The_Sims.pdf)
- Zubek notes that very few resources about the original Sims AI remain; only the Forbus and Wright course notes and a Sims 2 presentation by Jake Simpson (not retrieved) are freely downloadable. — [Zubek](http://robert.zubek.net/publications/Needs-based-AI-draft.pdf)
- Some fan wiki and dated claims conflict or are weak: the S.T.A.L.K.E.R. 150 m switch radius is repeated by developers' interview and a developer blog, and community forums for S2 give 80 to 100 m (not official); the CloudsPress S2 patch dates are a secondary report; the Sims 4 rewrite piece is from an AI-generated persona site. — [Cicala](https://gamedeveloper.com/design/a-life-an-insight-into-ambitious-ai); [CloudsPress](https://www.cloudspress.com/stalker-2-a-life-2-0-explained-how-the-zones-npc-simulation-works/)
- On UO, popular retellings (rabbits and wolves wiped out by player behaviour) conflict with Koster's account: the AI was cut for cost, and the economy collapse was a closed-loop resource issue. — [Koster](https://www.raphkoster.com/?p=46439)

### Inferences
- A report writer should present scale and cost numbers as ranges from developer statements (Section 4) and avoid inventing per-NPC timings.

### Gaps
- Not covered by any read source: Richard Evans's Versu "social practices" and Black and White desire model (the search returned only an unrelated historian), Jake Simpson's Sims 2 scripting talk, Dave Mark's own GDC slides, the Halo 2 behaviour tree talk, UO's spawn/ecology numbers, Kenshi, and Ultima VII/Majesty/other schedule systems.

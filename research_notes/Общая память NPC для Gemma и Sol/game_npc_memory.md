# LLM-driven game NPC memory: shipped games, middleware and game-agent research (state as of 2026-11-04)

Scope: what shipped games, middleware and game-agent research use for NPC memory (what is remembered, storage form, gossip, false beliefs/lies, problems, evaluation). Context: Gothic 2 remake, hybrid Gemma (fast, local) + Sol (slow, hosted) NPCs, "code owns truth, NPC owns beliefs", lies must work.

Source-quality flags used below: **[primary]** = vendor docs/paper fetched in full; **[marketing]** = vendor claim without independent check; **[secondary]** = press/commentary; **[search-summary]** = fact seen only in a web-search result summary, page itself not fetched or blocked (treat as lower confidence); **[abstract-only]** = only the abstract was read.

---

## 1. What do shipped products and middleware actually remember, and how is it stored?

### Takeaway
Across middleware (Convai, Inworld, SkyrimNet-style mods, Krafton's PUBG Ally) the dominant shipped pattern is: an LLM extracts short natural-language memory records (summaries/facts, often with an importance score) per character (often per player-character pair), stores them in a vector/keyword index, and injects a handful of top-ranked records into the prompt. Structured belief/knowledge graphs, per-NPC gossip, and lie tracking are NOT part of any shipped product I could document; the "knowledge" side is usually authored static text (backstory/knowledge bank) kept separate from the learned "memory" side.

### Cited Findings

**Convai (Mimir) - most detailed public technical description**
- Tiers: scene awareness (game context as text), short-term (last N turns verbatim; N not given), medium-term (a window of conversation summarized once N is exceeded), long-term (medium-term memories consolidated when they closely match an existing memory), plus "working memory" = the full assembled prompt incl. backstory, knowledge bank and memory layers. [primary, Convai blog] — [Convai: Long Term Memory - A Technical Overview](https://convai.com/blog/long-term-memory---a-technical-overview)
- Extraction: an LLM produces structured memory attributes as JSON; the two key fields are a conversation summary and an integer importance score 1-10. — same source.
- Retrieval: hybrid semantic embeddings + BM25 (tokenize, lowercase, stem); BM25 min-max normalized, weighted mean (starting point 0.5/0.5); Gaussian recency penalty (worked example max penalty 0.3); log10(importance) boost (importance 1 -> 0, 10 -> 1); queries use a window of recent chat, not only the last message; custom scoring replaces a separate re-ranker. No latency figures given. — same source.
- Conflict handling: a new medium-term memory is checked against highly similar existing memories; within a similarity threshold range they are consolidated into one, which Convai says "improves recall and resolves conflicting information" (threshold not given; this is a **[marketing]**-level claim with no evaluation). — same source.
- Forgetting: no deletion/expiry described; old memories only down-weighted by recency penalty. — same source.
- Cost motivation stated by Convai: a 50k-token history on gpt-4-turbo ≈ $0.503 per interaction, hence compression. — same source.
- Unity SDK docs: memory is per (end_user_id, character_id) pair; not visible to other users or other characters; on connect the stored memories are injected into context; during a session new info sits in a session buffer; at session end the transcript is processed into natural-language third-person `MemoryRecord` entries; semantically equivalent facts update the existing record ("update" vs "add"); retrieval happens only at connect time (character cannot query memory mid-conversation); extraction is selective so brief remarks may be dropped; LTM is off by default. [primary] — [Convai Unity SDK: How long-term memory works](https://docs.convai.com/api-docs/plugins-and-integrations/convai-unity-sdk/features/long-term-memory/how-long-term-memory-works)
- If the end-user ID changes (e.g. PlayerPrefs cleared) the player is treated as new. — same source.

**Inworld AI**
- Docs describe two parts: Flash Memory (immediate, fact-based info from the conversation) which feeds Long-Term Memory (condensed into summaries, e.g. "likes Italian and Japanese food"); enabled by an "Enable Long-Term Memory" toggle on the character; "Restart Conversation" in Studio flushes LTM. **[search-summary]** of [Inworld docs: Long-Term Memory](https://docs.inworld.ai/docs/tutorial-basics/long-term-memory/). The docs page itself now redirects to a login (fetch returned a 302 to `platform.inworld.ai/docs-auth/login-redirect`), so I could not read it directly.
- A 2023 Inworld blog says LTM was for Enterprise-plan users; that post is marked outdated. **[search-summary]** — [Inworld blog: introducing long term memory](https://inworld.ai/blog/introducing-long-term-memory) (the fetched page did not contain memory content).
- Inworld has moved from the Studio character product to "Runtime", a C++ graph-orchestration engine (Node.js, Unreal SDKs; Unity listed as coming/available depending on source) that orchestrates LLM, STT, TTS, memory/knowledge and tools; docs say Runtime carries over Studio capabilities incl. knowledge retrieval, safety checks, long-term memory. Customers named: Xbox co-development, Ubisoft NEO prototype. **[search-summary]** — [Inworld blog: new AI infrastructure](https://inworld.ai/blog/new-ai-infrastructure-scaling-games-media-characters), [Inworld Runtime character guide](https://docs.inworld.ai/guides/runtime-character) (login-gated). A third-party tutorial claiming a "50-token short-term window" and a REST memory endpoint was flagged by the search tool as unverifiable; ignore it.
- Xbox-Inworld: multi-year partnership announced Nov 2023 to build "AI game dialogue & narrative tools at scale"; no memory details. [secondary] — [Video Games Chronicle](https://www.videogameschronicle.com/news/xbox-partners-with-inworld-to-build-ai-game-dialogue-and-narrative-tools-at-scale)

**Ubisoft NEO NPC (GDC 2024) and Teammates (Nov 2025)**
- NEO NPC: Inworld supplies the LLM; Ubisoft's narrative team defines each NPC's background, knowledge base and conversational style; NPCs "remember what you've said" and this can be referenced later; a relationship bar tied to an emotion system and objective-dependent topics; layered safety (input detection, topic filtering, output filters); Ubisoft sets lore parameters so NPCs stay in the world's domain. Article states no known application in upcoming Ubisoft games. [secondary, Game Developer] — [Game Developer: How do Ubisoft's AI-driven NPCs handle dynamic player interactions](https://gamedeveloper.com/design/how-do-ubisoft-s-ai-driven-npcs-handle-dynamic-player-interactions-)
- Observed limits in the NEO demo: the infiltration scenario ended up "more structured like a traditional dialogue tree" with a best-outcome answer; the reporter's claims of superhuman powers/past allegiance drew "measured responses within the bounds set in the game world". — same source.
- Teammates (Nov 2025): first playable Ubisoft GenAI project, FPS prototype on Snowdrop, voice-controlled AI teammates, closed playtest with "a few hundred players" (Engadget). Sources conflict on stack (Gemini vs small on-device models) and on names of companions; memory claims are vague ("contextual memory to keep conversations coherent"). **[search-summary, secondary]** — [Engadget](https://www.engadget.com/gaming/ubisoft-made-a-prototype-game-with-voice-controlled-ai-teammates-200834163.html), [Hitmarker](https://hitmarker.net/news/ubisoft-reveals-teammates-a-generative-ai-experiment-featuring-voice-controlled-npcs-1582502)

**Krafton PUBG Ally (CPC) - NVIDIA ACE**
- Model: 2B Mistral-NeMo-Minitron SLM, quantized, on-device; NVIDIA ACE ASR; Krafton's own TTS; runs in VRAM left after PUBG, GPUs from 8 GB total. [primary, NVIDIA dev blog] — [NVIDIA: How KRAFTON built PUBG Ally](https://developer.nvidia.com/blog/how-krafton-built-pubg-ally-a-co-playable-character-powered-by-nvidia-ace/)
- Architecture: event-driven loop; System 2 = LLM (intent, coordination, speech), System 1 = behavior tree at game-tick rate (movement, aim, combat); observation tools return plain-text descriptions of engine data; prompts structured so stable instructions stay constant across turns for KV-cache reuse; cloud LLMs judged too slow for squad comms. — same source. (Direct analogue of a fast/slow split.)
- Memory: two timescales. Long-term across matches = a player profile (name, preferred weapons, favourite drop locations, personal details shared) and a history of prior matches (placements, notable moments). Short-term within a match = the player's recent speech and recent events. In playtests Ally remembered a name/preferences and brought them up unprompted later. — same source. **[marketing]** (vendor blog, no measurements of recall).
- Krafton's technical report (arXiv 2609.29837, 55 pages per Inven) names "context compaction" and "memory redaction" as safeguards without explaining them in the abstract page I could read; player feedback included "inconsistent memory"; no ablation of the two-system design. [abstract-page only] — [alphaXiv: PUBG Ally paper](https://www.alphaxiv.org/abs/2609.29837), [Inven Global](https://www.invenglobal.com/articles/26592/built-a-teammate-got-a-chat-partner-kraftons-first-scorecard-for-pubg-ally)
- Krafton reportedly showed long-term memory at CES 2026 (preferences, names). **[search-summary]** (gamedev.net returned 403) — [gamedev.net news](https://gamedev.net/news/4114/)

**inZOI (Krafton) "Smart Zoi"**
- Smart Zoi = experimental opt-in LLM-driven autonomy/inner thoughts, heavy hardware (one source: RTX 5070 Ti for best performance). A fan wiki lists planned event memory/diary/trauma and milestone memories (Q1 and Q3 2026) - treat as unconfirmed. **[search-summary, secondary]** — [inZOI memory-system wiki](https://thegameswiki.com/inzoi/wiki/memory-system), [PC Gamer](https://www.pcgamer.com/games/life-sim/inzoi-is-creating-a-newfangled-life-sim-npc-that-can-grow-and-develop-its-own-personality-with-nvidias-ai-tech)
- On the official forum Krafton staff said (Oct 3) a new LLM-based system, separate from Smart Zoi, "enables NPCs to form relationships and retain memories", already in development as shown at Gamescom; per a user, not certain to ship. A secondhand Cologne-meetup account: a Zoi talked with an AI "mother" who referenced character traits and used guilt. [primary for the staff quote, secondary for the demo] — [inZOI forum thread](https://forum.playinzoi.com/t/topic/4841)

**Mecha BREAK (Amazing Seasun) - first ACE-in-game**
- One mechanic NPC, Nemotron-4 4B Instruct on-device (~2 GB VRAM, ~300 ms vs "a few seconds" cloud), Whisper ASR, ElevenLabs cloud TTS, Audio2Face-3D; reporters call it a proof of concept for a single NPC. No persistent memory found in any source. **[search-summary, secondary]** — [Tom's Hardware](https://www.tomshardware.com/tech-industry/artificial-intelligence/nvidias-first-ai-npc-debuts-in-mecha-break-minitron-4b-model-only-requires-2gb-of-vram), [Neowin](https://www.neowin.net/amp/nvidias-ace-for-games-ai-chatbot-for-game-charaters-get-a-mecha-break-video-demo/)

**Where Winds Meet (Everstone / NetEase, 2025)**
- Free-text LLM chat with NPCs; commentary says memory is shallow and selectively persistent and main story is authored; backend rumoured to be DeepSeek-R1 - **unverified**, no official technical write-up found. **[secondary, unverified]** — [GeekChamp](https://geekchamp.com/?p=234583), [PC Gamer](https://www.pcgamer.com/games/rpg/wuxia-mmo-where-winds-meet-is-full-of-ai-chatbot-npcs-and-people-are-doing-all-the-standard-obscene-stuff-to-them-i-made-him-think-that-my-character-was-pregnant-with-his-child) (headline only; body not retrievable)

**Indie/Steam LLM-NPC games**
- Vaudeville (detective, LLM dialogue), Suck Up!, Whispers from the Star: one comparison table lists Suck Up! as session memory only; accounts of whether Whispers from the Star persists memory across calls conflict (a Reddit snapshot says resets, a listing says persists). The only source claiming "most have session-based memory" is a vendor page promoting its own persistent-memory product. **[search-summary, marketing-adjacent]** — [Wanderfolk comparison](https://wanderfolk-website.pages.dev/ai-npcs-in-games)

**Open mods (best public look at an NPC-memory pipeline)**
- SkyrimNet memory system: memory types EXPERIENCE/RELATIONSHIP/KNOWLEDGE/LOCATION/SKILL/TRAUMA/JOY, each with emotion tag, importance 0.0-1.0, topic tags (people, places, activities, items, emotions). Events grouped into segments (max 60-min gap, 10-480 min duration, 5-200 events), an LLM writes a first-person memory (summary + details). Embeddings all-MiniLM-L6-v2 (384-d) in SQLite with HNSW index. Per NPC cap 1000 memories (default), minimum importance 0.2 to store, 5 memories injected per prompt by default (configurable 1-10). Retrieval score weights: semantic 0.35, temporal proximity 0.20, actor involvement 0.20, emotional match 0.10, keyword 0.10, location 0.05. A separate Event Analyzer filters raw events for memory-worthiness. The page describes no decay mechanism. [primary, mod docs] — [SkyrimNet memory recall docs](https://goncalo22.github.io/SkyrimNet-GamePlugin/Memory%20System/memory-recall)
- Early Skyrim ChatGPT mods: model summarizes the conversation at the end and the summary is fed into later prompts; a fork (Pantella) uses ChromaDB retrieval chunks. **[search-summary, secondary]** — [PC Gamer](https://www.pcgamer.com/modder-wires-chatgpt-into-skyrim-vr-so-npcs-can-roleplay-and-remember-past-conversations), [Trusted Reviews](https://www.trustedreviews.com/news/skyrim-npcs-are-being-powered-by-chatgpt-4322856)

**Academic prototype for game NPC memory (not shipped)**
- "Fixed-Persona SLMs with Modular Memory" (arXiv 2511.10277): per NPC two ChromaDB stores (conversation memory updated every turn; world-knowledge store of structured facts/narrative hooks), top-k from each injected by cosine similarity; persona fixed by LoRA, memory modular so one SLM serves many NPCs. On RTX 2070 Super 8 GB: memory retrieval < 0.042 s even at 1,000 entries; memory swap 0.012-0.027 s; DB size 3.9-9.0 MB; Mistral-7B variant 93% factual consistency / 100% context retention (LLM-judge, 100 responses; 30 multi-turn tests), TinyLlama 55% / 63.3%, DistilGPT-2 9-16% / 6.7-10%. One persona, small tests, CLI only, no real-player study. [primary] — [arXiv 2511.10277](https://arxiv.org/html/2511.10277v1)

### Inferences
- The reusable shipped recipe is "episodic summaries + importance + hybrid retrieval + small K injected" (Convai, SkyrimNet, Fixed-Persona). It is per-NPC (SkyrimNet, Fixed-Persona) or per player-NPC pair (Convai). It matches what a shared Gemma/Sol memory would need minimally, but stores *what was said*, not *what the NPC believes is true* or *who told them*.
- Authored knowledge (backstory, knowledge bank, lore) is kept apart from learned memory in every product (Convai working-memory layers, NEO NPC, Fixed-Persona's two stores). That separation is the closest shipped analogue to "code owns truth".
- Krafton's System 1/System 2 split with plain-text observation tools and KV-cache-friendly stable prefixes is the closest shipped analogue to a Gemma-fast / Sol-slow hybrid; memory there is a coarse player profile plus match history, not a belief store.
- Retrieval-only-at-connect (Convai) and extraction-only-at-session-end mean a fact told in this conversation cannot be used by another character or by a parallel model until later; for lies that must have consequences within minutes, writes would need to be immediate and structured.

### Gaps
- Inworld's current memory documentation is login-gated; exact schema, size limits, retrieval method and whether memory is per-player are not verified from primary text.
- No primary technical description found for Where Winds Meet, Mecha BREAK memory, inZOI memory, Ubisoft Teammates memory, or the GDC 2024-2026 talks (talk slides/videos not retrieved; GDC Vault not searched directly).
- Replica Studios: I found only the May 2023 Smart NPC plugin announcement (Unreal, Matrix Awakens demo, OpenAI-style LLM plug-in); nothing on its memory or current company status. [search-summary] — [ITWire](https://itwire.com/business-it-news/business-software/replica-studios-releases-ai-generated-npcs-tech-demo-forecasting-a-new-era-of-epic-games-to-come)
- Microsoft/Xbox: only the partnership announcement; no memory design or results public in what I found.
- Full text of Krafton's PUBG Ally report (memory section) was not retrieved.

---

## 2. What do game-agent research systems use for memory (Project Sid, Voyager, Minecraft agents, Smallville as reference)?

### Takeaway
Game-agent research stores memory in three distinct forms: (a) natural-language episodic/observation streams with timescales (Project Sid's WM/STM/LTM, Smallville), (b) executable skill libraries (Voyager), (c) structured knowledge graphs plus experience pools (Optimus-1; JARVIS-1 multimodal memory). The social-simulation work reports that hallucination compounds across LLM calls and across agents, and that a central information bottleneck is used to keep behavior coherent.

### Cited Findings

**Project Sid (Altera, arXiv 2411.00114)**
- PIANO (Parallel Information Aggregation via Neural Orchestration): concurrent modules (memory, action awareness, social awareness, skill execution, talking, etc.) coordinated by a Cognitive Controller (CC) whose input passes through an information bottleneck; CC decisions are broadcast to the other modules (esp. talk) to keep speech and action consistent. [primary] — [arXiv 2411.00114 (HTML)](https://arxiv.org/html/2411.00114)
- Memory module "stores and retrieves conversations, actions, and observations across various timescales"; Figure 3 names working memory (WM), short-term (STM) and long-term memory (LTM); the paper text I could read does not describe their internals. — same source.
- Social awareness module: agents track other agents' sentiment, build profiles of others' goals and generate social goals every 5-10 s; without it, relationships in 50-agent societies became more neutral and roles failed to specialize. — same source.
- Hallucination: "Hallucinations compound over successive LLM calls"; example: a single claim of eating a bagel can lead to false beliefs affecting later behavior; in groups one agent's miscommunication can spread hallucinations to others. — same source.
- Scale: single-agent Minecraft 25 agents/30 min (avg 17 unique items); 49 agents/4 h saturating at about one third of items; societies 50 agents/4 h; cultural transmission 500 agents/9,000 s; runs above 1,000 agents exceeded server limits. Only GPT-4o achieved the reported progression. — same source.
- Culture/rumour: conversations converted to memes via LM calls; Pastafarianism seeded by 20 priests, tracked by keywords, converts still growing after two hours. The authors suggest gossip or opinion averaging may explain collective likeability judgments but did not test either. — same source.
- Limitations: no vision/spatial reasoning, no innate drives; because of pre-trained models "no de novo emergence" of institutions. — same source.

**Voyager (arXiv 2305.16291)** - skill library of executable code as long-term memory; 3.3x more unique items, 2.3x farther travel, up to 15.3x faster tech-tree milestones than prior SOTA; skills transfer to new worlds. [abstract-only] — [arXiv](https://arxiv.org/abs/2305.16291)

**JARVIS-1 (arXiv 2311.05997)** - multimodal memory supports planning with pre-trained knowledge plus actual game experience; 200+ Minecraft tasks; ~5x reliability of prior SOTA on ObtainDiamondPickaxe. [abstract-only] — [arXiv](https://arxiv.org/abs/2311.05997)

**Optimus-1 (NeurIPS 2024, arXiv 2408.03615)** - Hybrid Multimodal Memory = Hierarchical Directed Knowledge Graph (world knowledge used by a knowledge-guided planner) + Abstracted Multimodal Experience Pool (history summarized for in-context learning; consulted by a periodic Experience-Driven Reflector); "up to 6x" gain is from a secondary summary. [abstract-only] — [NeurIPS page](https://proceedings.neurips.cc/paper_files/paper/2024/hash/5949a8750a110ce1f0631b1776c500a2-Abstract.html)

**Generative Agents / Smallville (game-reference numbers only)**
- Without programmed intervention, knowledge of Isabella's party spread from 1 agent (4%) to 13 (52%) over two simulated days; knowledge of Sam's mayoral candidacy 4% to 32%; authors checked claims against the memory stream and report the candidacy claimants did not hallucinate it. A Stanford course deck says 12 (not 13) heard of the party; 5 of 12 invitees attended. I did not find an embellishment percentage. **[search-summary]** — [Generative Agents (arXiv 2304.03442)](https://arxiv.org/pdf/2304.03442) (PDF too large to fetch), [secondary review](https://edforson.substack.com/p/paper-review-generative-agents)

**Tool-less text-game agents (TextWorld/Jericho), Minecraft memory beyond the above, belief-graph/ToM architectures** - not retrieved in this session; see Gaps.

### Inferences
- Project Sid's CC bottleneck is a concrete pattern: one deliberate decision stream decides, and everything else (talk, action) is conditioned on that single broadcast decision. This is the mechanism it uses against inconsistency between "what the agent says" and "what it does"; in a Gemma+Sol hybrid, the equivalent would be that both models read/write the same decision/belief record rather than each keeping its own state.
- "Hallucinations compound across calls and across agents" is the same risk as an NPC repeating a fabricated "memory" to others. It is a reason to keep every shared fact tagged with its origin and to let only code-owned events create "ground truth".
- Voyager/Optimus-1 show memory of *procedures and world structure* (skills, graph) is a different object from memory of *social episodes*; NPC social memory systems reviewed here mostly implement only the latter.

### Gaps
- Internals of Project Sid's WM/STM/LTM and how its agents retrieve memories: the HTML text fetched covers only the first 100,000 of 128,204 characters; the appendix with implementation detail may be in the unread tail.
- Smallville paper's own limitation section (retrieval failures, embellishment) not read (PDF over the 10 MB fetch limit).
- Text-game agent memory (TextWorld/Jericho), JARVIS-1 and Optimus-1 implementation details beyond abstracts, belief-graph architectures, and "theory of mind" NPC architectures were not researched in depth.

---

## 3. How does knowledge/gossip spread between NPCs (classic and LLM)?

### Takeaway
The best-documented design is the Talk of the Town / Gossamer family: each character has a private memory database, knowledge is created by witnessing (possibly mutated) or confabulating, propagated by telling (with mutation), and decays with weights and a cull threshold; who tells whom is driven by familiarity/salience. LLM-agent versions (Smallville, Project Sid, rumour simulators) pass natural-language summaries and are known to spread quickly but with no provenance model. Dwarf Fortress uses event-anchored "rumors" that originate with witnesses and spread off-screen.

### Cited Findings

**Talk of the Town / Bad News (Ryan et al., AIIDE 2015), as summarized by Kreminski (Gossamer, IEEE CoG 2023)**
- Each character has perceptible attributes (appearance, home, most recent whereabouts); characters originate knowledge (by witnessing or confabulating) and propagate it (by telling). Every item is subject to mutation and decay; when two characters meet they choose what to talk about by mutual familiarity. Result: characters that "observe, tell, misremember, and lie", but mostly about attributes like hair colour and residence, not rich stories. [primary secondhand of Ryan] — [Kreminski: Toward Better Gossip Simulation (Gossamer)](https://mkremins.github.io/publications/Gossamer_CoG2023.pdf); abstract of the original at [AIIDE 2015 listing](https://aitopics.org/doc/conferences:EA0682D9) (abstract only; the full paper and the Game AI Pro 360 chapter were not retrievable).
- Versu: characters evaluate others by social role, communicate evaluation + reason, and may adopt another's evaluation; only ONE evaluation+justification stored per character-character-role triple. Anecdote: a developer rudely treated a butler, who gossiped, alienating characters he had not met. — same Gossamer paper.
- Kismet (Talk-of-the-Town-inspired): characters gossip about individual witnessed/heard actions; inference rules fire when a character knows several actions by one person; complex stories are not communicated. — same.
- City of Gangsters: per-character reputation toward every other character from specific actions; each action is known to its direct target and passed to the target's direct social connections; one action at a time; integer +/- score; unclear whether second-hand knowledge can be re-passed. — same.

**Gossamer's proposed model (open-source JavaScript, "preliminary", little evaluation yet)**
- Four phases: witness (decide who witnessed, how perception is mutated, whether to record, initial strength), reflection (assemble remembered actions into "microstories"), propagation (decide which stories to share, mutate on transmission, initial strength for receiver), decay (periodically lower each memory's weight, cull below a fixed threshold so characters do not accumulate unbounded memories). — same.
- Per-character memory database (DataScript) so queries implicitly ignore what the character does not know; salience = familiarity = count of references to the entity in the character's own memory / total references. — same.
- Phenomena the author plans to test (not yet shown): rumours spreading fast within clusters but slowly across the graph; being displaced by more shocking rumours; characters preceded by reputation; polarization from "dueling rumours". — same.

**Dwarf Fortress**
- Wiki: a rumor is "a certain piece of knowledge regarding a specific historical event"; originates with witnesses and spreads further from people who heard it; in adventure mode the player can only pass on rumors the character knows; spreading is inconsistent (sometimes about a week) and happens off-screen when a person who knows it is offloaded. [secondary, wiki; fetch returned 429, content from search result] — [DF Wiki: Rumor](https://www.dwarffortresswiki.org/index.php/Rumor)

**LLM-agent simulations of spread**
- Smallville: see section 2 numbers (4% to 52% / 32% in two game days, no provenance model; authors checked claims against memory logs). **[search-summary]**
- Project Sid: culture/meme and religion transmission among up to 500 agents (section 2); gossip not tested explicitly.
- Hu et al. (arXiv 2502.01450): each LLM agent keeps its own post history and rumour belief, updating each step; 100+ agents on four network topologies. [abstract-level via search-summary] — [arXiv](https://www.arxiv.org/abs/2502.01450)
- RumorSphere (arXiv 2509.02172): million-agent simulation mixing rule-based agents with dynamically chosen LLM "core" agents at conflict boundaries; finds tight local communities speed spread and that early, sustained corrections beat one-off interventions. [abstract-level via search-summary] — [arXiv](https://arxiv.org/abs/2509.02172v2)

**Provenance/false corroboration**
- Spivey (CogSci 2017) rumour-network simulations: in 100-node random networks, after 100 transmissions of one false rumour roughly 26 of 100 nodes had heard it from two or more apparent sources although all traced to one origin - "false corroboration" - independent of network density. [primary, cognitive-science simulation, not a game paper] — [eScholarship](https://escholarship.org/uc/item/0ph1j7h9)

### Inferences
- A gossip layer needs, at minimum, per-NPC private stores (so each NPC can only repeat what it knows), a witness/telling event that copies a *record with source*, and decay/cull. Gossamer's phases map directly onto an event bus in the game: code decides who witnessed an event; an NPC's belief record is created from that; telling copies it (optionally mutated by an LLM) to the listener's store.
- To avoid false corroboration (hearing the same lie from three NPCs who all got it from the player), each belief should carry an origin id/chain; trust computation should count distinct origins, not distinct tellers (follows from Spivey's result).
- LLM-based spread (Smallville) is cheap to implement but has no mechanism for provenance, partial belief or deliberate withholding; rules-based systems (Talk of the Town, DF) have those but only for narrow fact types. A hybrid (code-owned structured event records; LLM verbalizes and interprets) is not shipped anywhere I could find.

### Gaps
- No shipped LLM-NPC title with documented NPC-to-NPC gossip was found; all documented gossip is in research or classic simulation games.
- The actual Talk of the Town numbers (mutation probabilities, strength values, confabulation rules) are in the AIIDE 2015 paper and Game AI Pro 360 chapter, which I could not retrieve as text.
- Dwarf Fortress rumor mechanics beyond the wiki summary (e.g. distortion of rumors, settlement-level propagation) were not verified; Ultima Ratio Regum rumour system not researched.
- Oblivion/Skyrim-era Radiant gossip, Shadow of Mordor's Nemesis memory, and the Sims were not researched.

---

## 4. How are false beliefs and lies represented, and what is known about LLM deception, theory of mind and exploit/failure behaviour?

### Takeaway
Classic systems represent lies as mutated or confabulated knowledge items passed through a telling event; LLM-agent research measures deception and belief tracking in social-deduction games and finds models generate deception much more easily than they detect it. Player-facing evidence says deliberate in-fiction lies are accepted as part of the game while accidental hallucinations feel meaningless, and shipped LLM NPCs (Where Winds Meet) accept player-asserted events as fact.

### Cited Findings
- Talk of the Town: lies and misremembering are first-class mutations of knowledge items (see section 3); the original describes characters that can "observe, tell, misremember, and lie". [abstract + Gossamer summary]
- WOLF (Werewolf-based Observations for LLM Deception and Falsehoods, NeurIPS 2025 workshop): werewolves produced deceptive statements in 31% of turns; peer detection reached 71-73% precision with overall accuracy around 52%; extended interaction improved recall against liars without extra false accusations of truthful roles. **[search-summary]** — [arXiv 2512.09187](https://arxiv.org/html/2512.09187)
- Among Us sandbox (NeurIPS 2025): open-ended emergent deception across 18 LLMs; RL-trained models much better at producing deception than detecting it; linear probes on activations detected lies with AUROC > 95%. **[search-summary]** — [NeurIPS 2025 abstract](https://papers.nips.cc/paper_files/paper/2025/hash/105c4de1195135fae4974aa8c5e27bbf-Abstract-Conference.html)
- A 2026 preprint (arXiv 2608.24691) reportedly probes agents' role beliefs after each public utterance (MafiaScope) and finds opponent modelling emerging only with persistent memory in poker. **[search-summary only; not read]** — [arXiv 2608.24691](https://arxiv.org/pdf/2608.24691)
- CHI '24 "Lies, Deceit, and Hallucinations" (29 participants, game AlphaBetaCity with human-written lies and human-approved LLM hallucinations): players treated intentional falsehoods as part of the story/gameplay; unintentional errors "generally mismatched participants' mental models" and felt meaningless; designers should consider whether players see a falsehood as intentional, the audience, and players' desire for falsehoods to mean something. [abstract-only] — [Xiao et al.](https://www.robertxiao.ca/research/lies-deceit-and-hallucinations/)
- Where Winds Meet exploit: players type parenthetical narration like "(Suddenly, her two brothers appear)" and the LLM NPC treats it as a real event, eventually marking the quest complete; another trick (repeating the NPC's phrase until it gives up) pushes dialogue into a "success" state and releases rewards. The article says NPCs accept player-stated events as fact without checking world state; no developer statement was cited. [secondary] — [Outlook Respawn](https://respawn.outlookindia.com/gaming/gaming-news/where-winds-meet-ai-exploit-lets-players-skip-quests); a related story about convincing an NPC the player's character was pregnant with his child: [PC Gamer headline](https://www.pcgamer.com/games/rpg/wuxia-mmo-where-winds-meet-is-full-of-ai-chatbot-npcs-and-people-are-doing-all-the-standard-obscene-stuff-to-them-i-made-him-think-that-my-character-was-pregnant-with-his-child)
- Prompt injection can make LLM NPCs reveal hidden background secrets (Shiomi et al., arXiv 2508.19288, Aug 2025). [abstract-only] — [arXiv](https://arxiv.org/abs/2508.19288)
- RoleKE-Bench (arXiv 2409.11726): separates known-knowledge errors (character should know a fact but gets it wrong) from unknown-knowledge errors (character states something it should not know); even the latest LLMs "struggle to detect" both, particularly for familiar knowledge; a self-recollection/self-doubt method (S²RD) helps. [abstract-only] — [arXiv](https://arxiv.org/abs/2409.11726)
- NEO NPC demo: the reporter's false claims of superhuman powers/past allegiance got "measured responses within the bounds set in the game world" - the world-bounded persona resisted the lie rather than building on it. [secondary] — [Game Developer](https://gamedeveloper.com/design/how-do-ubisoft-s-ai-driven-npcs-handle-dynamic-player-interactions-)
- Smallville-style and Project Sid agents have no explicit lie/false-belief representation; Project Sid documents unintentional false beliefs from hallucination spreading (section 2).

### Inferences
- Two distinct failure classes need different handling: (1) the player's claim is accepted as world fact (Where Winds Meet) - an authority/trust problem, solvable only if the claim is stored as "player said X" with source and plausibility, not as "X happened"; (2) the NPC invents a memory/fact (hallucination) - a provenance problem. The CHI '24 result suggests (1) can be fun when it has narrative meaning and (2) is perceived as a bug.
- LLMs lie fluently but detect poorly (WOLF, Among Us), so "lies must work" is plausible with LLM NPCs as targets of lies, but "NPCs must catch contradictions" will need code-side checks rather than model judgement.
- The unknown-knowledge-error finding (RoleKE-Bench) supports giving each NPC only its own belief store in the prompt instead of omniscient lore plus an instruction to "not know" things.

### Gaps
- No shipped title with an explicit belief-with-confidence/source store for LLM NPCs was found.
- Theory-of-mind belief-graph architectures for NPCs (e.g. explicit "A believes that B believes") and belief-tracking evaluations beyond the Werewolf/Among Us snippets were not read in full.
- Details of the Where Winds Meet quest-completion mechanism (how the model state maps to quest flags) are not public in what I found.

---

## 5. Classic game-AI knowledge representation patterns still relevant

### Takeaway
The sourced patterns that I could verify are the social-simulation ones: per-character memory databases with decay and cull (Gossamer), knowledge items with mutation (Talk of the Town), one-evaluation-plus-justification per triple (Versu), per-pair reputations from specific actions (City of Gangsters), event-anchored rumors (Dwarf Fortress). F.E.A.R./Thief-style sensory memory with confidence and decay is well known but I could not retrieve a source in this session.

### Cited Findings
- All patterns above are documented in [Kreminski's Gossamer paper](https://mkremins.github.io/publications/Gossamer_CoG2023.pdf) and the [DF Wiki: Rumor](https://www.dwarffortresswiki.org/index.php/Rumor) (see section 3). In addition: Prom Week (Comme il Faut) is cited there as a social-simulation game; no further mechanism detail was retrieved.

### Inferences
- A knowledge item with fields {subject, predicate/event, source (witnessed/told-by/confabulated/player-claimed), strength/confidence, timestamp, mutation flag} plus decay-and-cull is the common core of Talk of the Town/Gossamer and directly maps to "NPC owns beliefs, code owns truth" (truth = event log; belief = per-NPC items with optional mismatch to truth).

### Gaps
- F.E.A.R. working memory (Orkin: facts with type, belief/confidence and update time), Thief/Halo target tracking with confidence and decay, Prom Week/CiF mechanics, Ultima Ratio Regum rumours: a search for Orkin's work returned only unrelated psychology papers; any statement about those systems here would come from background knowledge and is NOT verified, so I have not made any.

---

## 6. What problems do they hit: bloat, contradictions, hallucinated memories, cost, latency?

### Takeaway
Documented mitigations are caps and thresholds (SkyrimNet 1000 memories/actor, min importance 0.2, 5 injected), importance scoring plus consolidation (Convai), decay-and-cull (Gossamer), and context compaction (Krafton). Documented failures are hallucination compounding (Project Sid), "lack of NPC memory between interactions" and hallucinations in player reviews (Vaudeville), inconsistent memory in the PUBG Ally beta, and player-claim acceptance (Where Winds Meet). Latency strongly shapes player preference.

### Cited Findings
**Bloat / cost**
- Convai: compress history because a 50k-token history on gpt-4-turbo ≈ $0.503 per interaction. [Convai blog](https://convai.com/blog/long-term-memory---a-technical-overview)
- SkyrimNet caps and thresholds (above); an unnamed itch.io commenter says 3-sentence memory entries are common, longest ~377 characters (weak anecdote). **[search-summary]** — [itch.io post](https://itch.io/post/14067354)
- Gossamer decay phase: weights reduced and memories culled below a threshold "so characters do not accumulate larger and larger numbers of memories". [Gossamer](https://mkremins.github.io/publications/Gossamer_CoG2023.pdf)
- PUBG Ally cloud-vs-device cost per match (Krafton's measurements as reported by Inven): Claude Opus 4.8 $5.14-7.10, Claude Sonnet 4.6 $3.70-5.17, Gemma 4 31B $0.02-0.06, on-device $0 to the company; 4-bit local LM ≈ 1.6-2.1 GB; 1,046 adults, 38,956 sessions over 28 days. [secondary report of Krafton's paper] — [Inven Global](https://www.invenglobal.com/articles/26592/built-a-teammate-got-a-chat-partner-kraftons-first-scorecard-for-pubg-ally)

**Latency**
- Ally spoken exchange on RTX 4060-class: median ~1.62 s on-device vs 3.36 s cloud; LM step alone 0.80 s vs 2.54 s. [alphaXiv](https://www.alphaxiv.org/abs/2609.29837)
- In a blind test, first on-device model v1 was preferred over the cloud model 69.5% to 18.8% (154 participants), which Krafton attributed to response speed; with latency matched, the cloud model won 52.4% to 31.1%. [Inven Global](https://www.invenglobal.com/articles/26592/built-a-teammate-got-a-chat-partner-kraftons-first-scorecard-for-pubg-ally)
- Fixed-Persona SLM paper: retrieval < 0.042 s at 1,000 entries (negligible next to generation). [arXiv 2511.10277](https://arxiv.org/html/2511.10277v1)
- Mecha BREAK: local ~300 ms vs cloud "a few seconds" (Tom's Hardware, search-summary).

**Contradictions / consistency**
- Convai: semantically equivalent facts update the existing record (e.g. corrected name); medium-term memories consolidate with close matches (claimed to resolve conflicts) - no evaluation. [Convai Unity docs](https://docs.convai.com/api-docs/plugins-and-integrations/convai-unity-sdk/features/long-term-memory/how-long-term-memory-works)
- Versu stores only one evaluation+justification per triple (a deliberate limit). [Gossamer](https://mkremins.github.io/publications/Gossamer_CoG2023.pdf)
- Fixed-Persona: small models drift (DistilGPT-2 9-16% factual consistency) while a 7B model with the same memory scored 93%. [arXiv 2511.10277](https://arxiv.org/html/2511.10277v1)

**Hallucinated memories**
- Project Sid: hallucinations compound over calls and spread between agents. [arXiv 2411.00114](https://arxiv.org/html/2411.00114)
- PUBG Ally beta: players reported "inconsistent memory", delayed prompts, unfinished tasks; Ally sometimes replied "Okay" without carrying out the action and kept talking in late-game combat. [alphaXiv](https://www.alphaxiv.org/abs/2609.29837), [Inven Global](https://www.invenglobal.com/articles/26592/built-a-teammate-got-a-chat-partner-kraftons-first-scorecard-for-pubg-ally)
- Vaudeville: analysis of 132 Steam reviews (+30 replies) 1 Jul-20 Sep (2023) plus Discord found players relaying LLM hallucinations and lack of NPC memory between interactions. **[search-summary]** — [Cox & Ooi, CONVERSATIONS'23](https://samucox.github.io/files/CONVERSATIONS'23%20Paper.pdf)
- Memory distortion: a 2024 study of LLMs in a social-deduction game found memory distortion in 32 of 506 reasoning steps. **[search-summary]** — [arXiv 2408.09946](https://arxiv.org/pdf/2408.09946)

### Inferences
- Latency is not a secondary metric: in Krafton's data the preference between a smaller local model and a larger cloud model flipped when latency was equalized. For Gemma-first/Sol-later, the fast model's output is what the player judges first, so shared memory must be readable by Gemma without a Sol round trip.
- Every documented bloat control is one of: per-NPC cap, importance floor, small-K injection, decay/cull. None of the shipped systems consolidate by contradiction detection with measured accuracy.

### Gaps
- No published latency/cost numbers for Convai or Inworld memory retrieval; no numbers on how many tokens memory injection adds in shipped titles other than SkyrimNet's K=5 default.
- No public contradiction-rate measurements from any middleware vendor.

---

## 7. How is NPC memory quality evaluated, and what player-facing failure stories exist?

### Takeaway
Evaluation in the sources found is ad hoc: LLM-judge consistency and context-retention percentages on small test sets (Fixed-Persona), survey NPS and perception scores (PUBG Ally), qualitative analysis of Steam reviews (Vaudeville), and targeted benchmarks for knowledge errors and long-term chat memory (RoleKE-Bench, LongMemEval). No vendor publishes recall/contradiction metrics for NPC memory.

### Cited Findings
- Fixed-Persona SLMs: factual consistency (LLM judge, 100 responses/variant), context retention (30 multi-turn tests), world-knowledge retrieval (30 queries), fluency (LanguageTool), latency, VRAM; authors list small tests and a single persona as limits. [arXiv 2511.10277](https://arxiv.org/html/2511.10277v1)
- PUBG Ally beta: ~39k sessions recorded; net recommendation +25.1 points (17 languages, 141 countries), rising from ~+8 early to ~+36 in the final phase; gameplay ratings out of 5: combat 2.58, situational judgment 2.88, response speed 2.93, command execution 2.99; players saw Ally as 50.0% tool, 31.5% companion, 18.5% teammate; voluntary survey, no causal claim. [Inven Global](https://www.invenglobal.com/articles/26592/built-a-teammate-got-a-chat-partner-kraftons-first-scorecard-for-pubg-ally)
- LongMemEval (arXiv 2410.10813): 500 curated questions across five abilities (information extraction, multi-session reasoning, temporal reasoning, knowledge updates, abstention); commercial assistants and long-context LLMs show about a 30% accuracy drop across sustained interactions. [abstract-only] — [arXiv](https://arxiv.org/abs/2410.10813)
- RoleKE-Bench: detection of known/unknown knowledge errors in role-play (section 4).
- Gossamer references retellings-based evaluation ("Evaluating AI-based games through retellings", AIIDE 2019) as how player stories of emergent play are studied. [Gossamer](https://mkremins.github.io/publications/Gossamer_CoG2023.pdf)
- Player-facing failure stories: Where Winds Meet quest-skipping and "pregnant" NPC (section 4); Vaudeville hallucinations/memory loss (section 6); Ubisoft NEO NPC reporter found the conversation shifted to a dialogue-tree and that the NPC could not interpret speech while busy with another task [Game Developer](https://gamedeveloper.com/design/how-do-ubisoft-s-ai-driven-npcs-handle-dynamic-player-interactions-).
- A vendor blog (May 2026) claims players actively try to break NPCs with trick questions and false-memory claims; it is marketing for a platform. **[marketing, search-summary]** — [Respan](https://www.respan.ai/resources/npc-dialogue-consistency)

### Inferences
- Measurable memory tests that fit an NPC: (a) planted-fact recall after N turns/sessions, (b) knowledge-update tests (the NPC is told a corrected fact), (c) abstention (does the NPC admit not knowing something it was never told), (d) unknown-knowledge probes (RoleKE-style: does it state facts only code or another NPC knew), (e) contradiction rate across paraphrased questions. (a)-(d) correspond to LongMemEval abilities and RoleKE-Bench error types.

### Gaps
- Smallville's own interview-based evaluation protocol was not retrieved (PDF too large).
- No standard NPC-memory benchmark found; no vendor or studio reports contradiction rate.
- Reddit/Steam review-level failure stories for Suck Up!, Whispers from the Star, inZOI beta were not read directly.

---

## 8. Cross-cutting implications for a shared Gemma+Sol memory with gossip and lies (inference only)

### Takeaway
Shipped memory is mostly "summaries + retrieval", so the pillar "code owns truth, NPC owns beliefs" is not something to copy from products; the closest reusable designs are Talk of the Town/Gossamer (per-NPC knowledge items with source, mutation, decay) and Krafton's fast/slow split with plain-text observation tools.

### Cited Findings
- (All cited in the sections above; no new facts in this section.)

### Inferences
- One per-NPC store read by both models: Convai (connect-time injection) and SkyrimNet (per-NPC SQLite+HNSW, K=5) show that a retrieval layer independent of the generating model is normal; Fixed-Persona explicitly decouples memory from the model so one SLM serves many NPCs.
- Records should be typed: authored knowledge (static), episodic events (code-written), beliefs (NPC-owned: claim, source, strength, time, origin chain), relationship values. This mirrors Convai's separation of knowledge bank vs memory, SkyrimNet's memory types/importance, and Gossamer's strength/decay.
- Player statements should be stored as "player claimed X" with a source tag, never auto-promoted to world facts (Where Winds Meet exploit); gossip should copy beliefs with their origin so repeated hearing of one lie is not counted as corroboration (Spivey).
- Decay/cull and small-K retrieval are the proven bloat controls; Sol should write summaries/belief updates asynchronously, Gemma should read the same records synchronously (latency data from Krafton suggests the first spoken reply dominates perceived quality).

### Gaps
- Nothing sourced here is evidence that a hybrid belief store plus LLM verbalization works at scale; no shipped or published system matching the full design was found.

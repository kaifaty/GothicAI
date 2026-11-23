# How NPC memory and the Gemma–Sol model interaction work today in the Gothic AI repository (state of 2026-11-04, branch `main` at `41f145b3`)

Source convention: no web was used. Every claim cites a file in this repository as `path:line` (or `path:from-to`). Abbreviations: `S/` = `server/src/gothic_ai/`, `D/` = `docs/`, `RA/` = `reports/npc-agent/`, `RM/` = `reports/npc-memory/2026-11-01-recall-eval/`. Documents are in Russian; I translated. Items marked **[local measure]** were computed by me on 2026-11-04 from local, non-git files (`~/.local/share/gothic-ai/traces/*.jsonl`, `~/.local/share/gothic-ai/memory.sqlite3` on the Mac); they are small samples (55 player turns, 28 branches), not benchmarks.

The single most important fact for a designer: **the Gemma+Sol "hybrid", the route classifier and the Sol "mind" are offline pilots that live only in `reports/npc-agent/`; none of them is in `server/src`** (a grep of `server/src` for `needs_decision`, `think_short`, `[sol]`, `route_class`, `hybrid`, "sol mind" returns nothing; verified with `git diff --name-only <merge>^1 <merge>`: the merged PRs #760 (sol-thinks pilot), #763 (hybrid stage 1), #768 (route classifier), #775 (gated hybrid), #781 (repeats), #791 / #807 / #824 (Sol mind v1 / v2 / v3) touch only `reports/` and `docs/`; #776 changes only `server/src/gothic_ai/persona.py` (the rule that NPCs must not correct the hero's ASR-mangled names); #826 adds `fix_names` in `talk.py`; #726 / #720 are the recall eval (`scripts/recall_eval.py` only); #730 adds the `embed-service/` and #733 the semantic recall in `memory.py` / `dialogue.py` / `embeddings.py`. Journal: `D/npc-agent.md:115-152`). What runs in the game today is: Gemma says the first few words, Sol answers with tools, and in fights Gemma speaks while Sol only acts.

---

## 1. Which models run where; what each does; how routing/turn flow works (with timings)

### Takeaway
"Gemma" is Google's Gemma 4 12B (QAT Q4_0 GGUF) on llama.cpp on dev-ubuntu (RTX 3090), a no-tools, no-thinking "quick" model that speaks first words, fight lines, order acknowledgements and native-SVM rewrites; "Sol" is `gpt-6.1-sol` reached through the user's ChatGPT subscription over the Responses API (one WebSocket per NPC) and does every full conversational turn, all tool calls, and memory summarisation. In production routing is fixed by situation (fight or not), not by a classifier; the classifier-gated hybrid and Sol-written "thoughts" are validated offline only.

### Cited Findings

#### Gemma (the "quick" model)
- Model and quant: `gemma-4-12b-it-qat-q4_0.gguf` (Google's QAT Q4_0 GGUF, ~7 GB download, llama.cpp built from source with CUDA), served under the alias `gemma-4-12b` — `server/deploy/gemma/run.sh:15`, `server/deploy/gemma/install.sh:3,7,12-13`.
- Serving flags: `-ngl 99 -c 65536 -np 4 -fa on -ctk q8_0 -ctv q8_0 --no-cache-idle-slots --jinja --reasoning-budget 0`, thinking disabled — `server/deploy/gemma/run.sh:17-18`. Context is split into 4 slots of 16 384 tokens: three NPC slots (up to three nearby NPCs stay prompt-cached) plus a fourth slot used only by the surrender classifier; 16 384 per slot because a line's prompt is 2–5k tokens and a long talk grows past 6k (6144 overflowed on 2026-10-22) — `server/deploy/gemma/run.sh:5-11`, `D/npc-agent.md:552`.
- Host and port: dev-ubuntu (RTX 3090, 24 GB), HTTP port 47834, systemd user unit `gothic-gemma` — `server/deploy/gemma/gothic-gemma.service:2`, `S/llm/__init__.py:40`, `D/README.md:60-66` (machine diagram), `D/npc-agent.md:552`. Client address defaults to the decider's host on port 47834 — `S/llm/__init__.py:73-79`. CPU affinity cores 2–11 (decision of 2026-10-27) — `D/compute.md:102`.
- VRAM: ~8.3 GB with three slots (`D/npc-agent.md:552`, `server/deploy/gemma/run.sh:11`); 7.4 GB by a later `nvidia-smi` measure (`D/compute.md:147`).
- Client: `OpenAICompatBackend` over Chat Completions; `developer` notes are sent as `system` messages (`S/llm/openai_compat.py:196-203`); every quick request is pinned to an NPC's `id_slot` via a ContextVar (`S/llm/openai_compat.py:66-84`); `prewarm()` sends the prompt with `max_tokens=1` to fill the KV cache (`S/llm/openai_compat.py:86-95`). Answers capped at `QUICK_MAX_TOKENS = 120`, `QUICK_MODEL = "gemma-4-12b"` — `S/llm/__init__.py:38-39,82-84`.
- Gemma has **no tools** (`tools=[]` in its calls, `S/dialogue.py:1497-1499`; table row "не делает: не решает, что NPC делает, и не вызывает инструментов", `D/npc-agent.md:374`).
- What Gemma does today:
  - **First words to a hero phrase** (not in a fight): one sentence ≤ 8 words, temperature 0.6, must be ready within `REACTION_S = 1.2 s`, else skipped; the hint `REACT_HINT` says "react at once… do not start the answer, promise nothing; for a greeting write only «—»" — `S/dialogue.py:113-116,131-136,1850-1902`, `D/npc-agent.md:452-461`. The Sol answer then continues from them ("Ты уже сказал вслух: «…»", `S/dialogue.py:141`). The first words are said with `filler: true` (protocol v14) — `D/npc-agent.md:461`. They are not written to NPC memory by themselves: `_commit` stores the trigger and `state.text` (Sol's text) — `S/dialogue.py:2301-2310`; Gemma's first words only set `conv.first_words` for the repeat check — `S/dialogue.py:1881`. [inference from code reading]
  - **The whole line in a fight**; Sol only acts on it (`quick = self._fighting(conv)`, `S/dialogue.py:2421-2422,2494-2511`, `S/dialogue.py:2689-2718`, `D/npc-agent.md:516-532`). If Gemma does not start within 3 s (`QUICK_FIRST_S`, `S/dialogue.py:123`) or fails, Sol speaks (`D/npc-agent.md:532`).
  - **Order acknowledgement** by companions ("Есть!", "Стою.") — `S/dialogue.py:1904-1914`.
  - **Native SVM rewrites** (`svm_line`, ≤ 12 words, ready in 2.0 s, prompt = persona + `_brief` without history) — `D/npc-agent.md:9`, `S/dialogue.py:153-160`; **combat barks** (≤ 6 words, 1.2 s, persona + brief situation, no history) — `D/npc-agent.md:59-69`.
  - **Surrender classifier** (grammar-constrained "да"/"нет", `temperature=0`, `max_tokens=4`, separate slot, ~100 ms) — `S/dialogue.py:125-129,1434-1456`, `D/npc-agent.md:227-229` (live: 16/16 correct "yes", 0/15 false, median 103 ms).
  - Wolf tone judgement (a real request without NPC slot pinning) — `D/npc-agent.md:540`.
- Measured Gemma latency: first word ~0.3–0.4 s with prewarm (`D/npc-agent.md:373`), 0.80 s without vs 0.39 s with prewarm on fight turns (`D/npc-agent.md:548`); a cold read of a 3–4k-token prompt takes ~1.2 s (`D/npc-agent.md:542`). **[local measure]** Gemma step first-token median 0.30 s in traces of 2026-10-18/20 (n = 18).

#### Sol (the "conversational" model)
- Provider/model: backend `chatgpt` is the default, model `gpt-6.1-sol`, reasoning effort `low`; auth is "Sign in with ChatGPT" (plan token), no API key — `S/llm/__init__.py:32-35,98-105`, `S/cli.py:890-893`, `S/llm/siwc.py:1-19`.
- How called: OpenAI **Responses API** with `store=False`, `stream=True`, encrypted reasoning items carried between steps, **no sampling params** (plan tokens reject them), `prompt_cache_key` = NPC id — `S/llm/openai_responses.py:1-25,176-205`. Each NPC has its own **WebSocket** (opened ahead when the hero starts talking, `open_ahead`; lives up to an hour; continuation steps send only the delta with `previous_response_id`); over plain HTTP the gateway scatters requests across backends and cache hit is ~0 — `S/llm/openai_responses.py:15-25`, `D/npc-agent.md:421-434`. Switch off: `--no-websocket`.
- Turn loop: ≤ `MAX_STEPS = 3` model calls per turn; last step `tool_choice="none"`; a new step only after a `recall` result, a rejected call, an accepted amendment, or a commitment with no words — `S/turn.py:1-58`, `D/npc-agent.md:409-419`. Stall guards: no first byte in 15 s or idle 15 s → retry once if nothing was said (`S/dialogue.py:120`).
- Tools in the request (15 in a Cavalorn trace): `set_intent`, `give_item`, `take_item`, `recall`, `end_conversation`, `look`, `catch_lie`, `ask_witness`, `where`, `propose_deal`, `chest`, `plan`, `accept_quest`, `check_claim`, `give_quest` (**[local measure]** trace 2026-10-20; definitions `S/npc_tools.py:53-113`, others in `S/deals.py:100`, `S/vanilla.py:62-68`, `S/sightings.py:102`, `S/witnesses.py:93`, `S/whereabouts.py:33`, `S/errands.py:51`, `S/chests.py:47`, `S/perception.py:75`). All tools are always in the request so the prompt-cache prefix does not change; which are callable is set by `allowed_tools` — `D/npc-agent.md:421-423`.
- Sol also: summarises old memory (`S/dialogue.py:1132-1161`), drives the director/showrunner/gossip/amends/feats (own cache keys, rarely used, kept-open sockets — `D/npc-agent.md:433`), named the 388 places offline (`D/npc-agent.md:941-962`).
- Latency: first word 2.01 s median cold, 1.93 s "warm" (20 pairs), made of ~0.7 s gateway before the model + ~0.8 s model time-to-first-token + ~0.4 s delivery; the subscription "warm-up" (`generate:false`) does not reach the model (`num_engine_calls = 0`), so it was **not built** — `RA/2026-11-01-sol-prewarm/README.md:5,34-45,49-54`. **[local measure]** In 2026-10-18/20 traces (WebSocket): Sol step first token p50 1.9 s / p90 2.6 s, step total p50 3.0 s, 55 player turns total p50 3.6 s / p90 5.5 s, 1–3 steps per turn, ~46 output tokens per step, input tokens p50 8.6k with 53 % cached; in 2026-10-12/14 (HTTP) first token p50 3.3 s. Docs quote "3–5 s to first word" for Sol (`D/npc-agent.md:373`). A `recall` call costs a whole extra step, ~4 s (`D/npc-agent.md:884`).
- Quota: runs on the ChatGPT plan's allowance. Unprompted calls (incident words, summaries) come from token buckets, 6 per NPC per minute and 20 per minute overall (`S/cli.py:1064-1069`, `S/budget.py:1-83`); the player's own phrases are counted but never refused (`S/budget.py:76-83`). I found **no documented numeric plan quota** anywhere in the repo (see Gaps).

#### The third model in the loop
- The fast "behavioural" model is **Strands Decider 2B** (`decider-service`, 0.03–0.1 s): body reactions, day plans, "should I approach the hero" — `D/README.md:128-135`, `D/npc-agent.md:370-374`. Its state is English text built by `S/brain/situation.py`; `NpcContext.facts` ("What you remember") exists but the only fact fed is "you already spoke to the hero about it today" — `S/brain/situation.py:103-112,150-151`, `S/initiative.py:398-404`. So the Decider has no real memory either.

#### How routing works in production now
- Per hero phrase (talking, not fighting): `talk.hear` picks the addressee, applies the misheard-name fix (`fix_names`), parses companion orders (LLM-free), writes witnesses' notes — `S/talk.py:554-607`; then `DialogueManager._speak`: (a) brief situation + Gemma first words and the NPC's body gesture in parallel, (b) the full situation (all extensions' `prepare` up to 3 s each, `PREPARE_S`) built meanwhile, (c) up to 3 Sol steps on `[history, full situation, hero phrase]`, text streamed sentence by sentence to TTS/subtitles — `S/dialogue.py:2402-2594,2761-2802`.
- Fight: `_fighting(conv)` = quick model exists and (`in_fight` or `fights_hero` or an incident goal) — `S/dialogue.py:1165-1170`. Then Gemma speaks the line and Sol runs **in parallel** only to act (no words; `PARALLEL_ACT_HINT`; if Gemma is silent, Sol's buffer is voiced) — `S/dialogue.py:142-145,2720-2759,2494-2512`, `D/npc-agent.md:197-201`.
- Gemma's cache is kept warm for ≤ 3 NPCs (focus, recent partners within 5 min, fighters): `prewarm` is re-triggered after every memory write and every finished turn (`S/dialogue.py:1103-1109,1519-1533,1207-1261`, constants `S/dialogue.py:146-152`).

#### The offline hybrid, route classifier and gated hybrid (what "decides who answers" in the pilots)
- Stage 0 — "Sol thinks, Gemma speaks" (#760, 30 recorded turns of Cavalorn/Xardas): Sol writes a ~256-token "thought" before the hero speaks (11.1 s median, fits the window to the next hero phrase in 79 % of turns); Gemma says the whole reply from it. Blind ratings by the user: Sol 13/16/1 (good/ok/bad), Gemma-by-thought 12/16/2; first sentence 0.58 s vs ~2 s. Not shippable: Gemma has no tools and promised things nobody decided (4 of 30), Sol better on commitment turns — `RA/2026-11-02-sol-thinks/README.md:5-19,110-174`.
- Stage 1 — hybrid (#763, 123 turns): shorter Sol thought (~110 tokens, 6.3 s, ready in time in 111/115 = 96 %); Gemma either answers or begins with `[sol]` + one non-committal phrase and hands the turn to Sol. Gemma did not hold the route: 9 real commitments in 70 turns she kept, deals/quests handed off only 7/10, 19/53 hand-off phrases already agreed — `RA/2026-11-02-gemma-sol-hybrid/README.md:5-15,94-156`.
- Stage 1b — route classifier (#768, 447 hero phrases from 7 traces, 199 "needs a decision" / 248 not, labelled by Sol + recorded tool calls + manual review): Gemma yes/no on the hero phrase alone (grammar `да|нет`, one token, probability threshold chosen so recall ≥ 95 %): recall 95.5 %, 71.4 % of simple phrases left to Gemma, 0.09 s median. FRIDA embeddings + logistic regression at the same recall left only 40–44 % to Gemma. Rules (keywords): recall 59.8 %. Misses: pointing at an enemy in a fight ("Вот она"), "Ладно, пока", "полегче", one quest report — `RA/2026-11-02-route-classifier/README.md:5-15,96-118`; prompt `RA/2026-11-02-route-classifier/classify.md:1-7`.
- Stage 2 — gated hybrid (#775, same 123 turns): classifier hands decisions to Sol (65 of 68), Gemma's own `[sol]` is a second net (caught the other 3); Gemma answered 34 of 55 simple phrases (62 %, 28 % of all turns); after-the-hero-phrase time classifier + first sentence 0.47 s median (p90 0.56) vs recorded Sol first token 2.96 s / whole answer 5.1 s. One commitment slipped (`eecb4d62e7d3`: hero answers Cavalorn's own question inside a haggle; the classifier sees one phrase). Proposed rule "if the previous turn went to Sol and ended with a question, this turn goes to Sol too": 97.0 % recall on 447 phrases, but found on the same data and untested on fresh traces. A Claude blind rater put Gemma ≥ Sol in 9/34 turns but disagreed with the user's ears on the calibration sheet (10/30 vs 22/30), so the "sounds as good" gate is still the user's — `RA/2026-11-02-hybrid-gated/README.md:5-10,83-137,139-190`.
- Gemma without Sol's thought speaks in model clichés ("Ты смеешь так со мной разговаривать" said by 6 different characters; 18 clichés shared by ≥ 3 characters vs 2 for Sol); with the thought it is at Sol's level (1.9–3.6 % shared 3-grams vs 2.5 %) — `RA/2026-11-02-repeats/README.md:5-9`.
- Pilot Gemma prompt order (prefix prewarmed up to and including the thought, then only the hero phrase is read): `[persona instructions][history][developer: situation + "[Твои мысли…] {thought}" + speak rules][user: hero phrase]` — `RA/2026-11-02-sol-thinks/pilot.py:255-285`; speak rules `RA/2026-11-02-gemma-sol-hybrid/speak_hybrid.md:1-8`. Gemma's pilot prewarm took 0.89–1.09 s median, p90 1.8–2.9 s — `RA/2026-11-02-gemma-sol-hybrid/README.md:104`, `RA/2026-11-02-hybrid-gated/README.md:120`.

#### Production turn flow with timings (typical talking turn)
1. Hero key → ASR (Inworld STT) → `talk.hear` (addressee, `fix_names` ~6 ms, orders) — `D/README.md:116-124`, `S/talk.py:554-576`, `D/npc-agent.md:115`.
2. 0–1.2 s: Gemma first words (0.3–0.4 s warm) + gesture (≤ 0.25 s, `FIRST_REACTION_S`, `S/dialogue.py:110`).
3. ≈ 2 s: Sol first token (1.9–2.0 s median on WebSocket); typical turn done in 3–4 s; speech goes sentence by sentence to TTS (Inworld TTS-2) — `S/dialogue.py:2402-2594`, `D/README.md:127`.
4. Tool calls run after each step; `recall` adds ~4 s; ≤ 3 steps — `S/turn.py:22`, `D/npc-agent.md:884`.
5. Turn end: `_commit` writes the hero's phrase and Sol's text to memory (`S/dialogue.py:2301-2310`), `_remember` pushes the new head to the game and re-warms Gemma (`S/dialogue.py:1103-1109`); `_maybe_compact` summarises in the background when > 40 messages (`S/dialogue.py:1125-1161`).
6. Time before the next hero phrase: median 17 s, 10 % of turns ≤ 8.8 s (this is the window the offline "thought"/"mind" must fit into) — `RA/2026-11-02-sol-thinks/README.md:28`.

### Inferences
- Gemma and Sol are coupled today only through (a) the same persona instruction string, (b) the same `Conversation.history`, (c) the same `Intent`, (d) the "already said aloud" line. Gemma sees none of the extension blocks (knowledge, sightings, trust, deals…); Sol sees all of them. Any shared memory that must reach Gemma has to be put there explicitly (the pilots did it as the "thought"/"mind" developer message).
- The route classifier is the only validated way to keep Gemma from making commitments; its residual risk is context-dependent follow-ups (haggling), which the "previous-turn-to-Sol" rule addresses on the same data only.
- Gemma's cache economics (prewarm 1–2 s, re-triggered after every memory write) mean memory-derived content that changes per turn belongs after the history and before the hero phrase, as in the pilots.

### Gaps
- No live in-game run of the hybrid, the classifier or the mind exists; all numbers come from replaying 123 recorded turns of two NPCs (Cavalorn, Xardas) and 11 recorded conversations. `RA/2026-11-02-hybrid-gated/README.md:81`, `RA/2026-11-02-sol-mind/README.md:87`.
- User's ear verdict on "Gemma by thought sounds ≥ Sol" for the gated hybrid and on the mind ribbons/blind sheet is still pending (`RA/2026-11-02-sol-mind/README.md:137-140,370`, `D/npc-agent.md:117`).
- ChatGPT-plan numeric quota/limits: not documented in the repo.
- Whether pilots fed Gemma the brief (production) situation or Sol's full situation is mixed: `_quick_context` uses the recorded `first_words` event's situation when present, otherwise the Sol step's developer message (`S/replay.py:45-56`); I did not determine the proportion.

---

## 2. What NPC memory exists now: structures, storage, writers, retrieval; what the recall eval found

### Takeaway
All NPC memory is one append-only SQLite log (`memory.sqlite3`, table `events`, branched with save games) with kinds `message`, `summary`, `intent`, `seen`, `chest` (per NPC) and `fact`, `patch` (branch-wide, `npc='*'`); most "beliefs" are not stored but **projected at read time from the fact log** by about ten modules. A model sees memory as (a) a summary + up to 40 recent messages (including ~25 modules' `developer` remarks), (b) per-turn extension blocks (Sol only), and (c) `recall` (Sol only): FRIDA embeddings with a lexical fallback. Code decides what is true; no model writes memory directly except via tools, summaries and `set_intent`.

### Cited Findings

#### Storage layer (`S/memory.py`)
- File `memory.sqlite3` in `$GOTHIC_AI_DATA` or `~/.local/share/gothic-ai/`, SQLite WAL — `S/memory.py:33-35,126`.
- Schema: `branches(id, parent, parent_seq, created)`; `events(branch, seq, npc, kind, role, content, covers, game_time, created)` PK `(branch, seq)`, index `(branch, npc, seq)`; `vectors(hash, model, vec)` PK `(hash, model)`; partial index for feat facts — `S/memory.py:48-77`.
- Kinds: per NPC — `message` (role `user` = hero, `assistant` = NPC, `developer` = remarks), `summary` (with `covers` seq), `intent` (JSON), `seen` (sightings JSON), `chest` (JSON); branch-wide with `npc='*'` — `fact` (chronicle JSON), `patch` (story-graph change JSON) — `S/memory.py:6-12,37,59,164-193,342-419`.
- Branches: new game and every load start a new branch; `ensure_branch` copies the parent's events up to `parent_seq` into the child (full physical copy, "keeps reads to one branch") — `S/memory.py:1-12,139-156`, `D/protocol.md:419-426`.
- Reads: `load()` = latest summary + newest `LOAD_LIMIT = 100` messages after it + latest intent — `S/memory.py:36,174-190`.
- Compaction: when an NPC has > `MAX_HISTORY = 40` messages, Sol (the main `self.llm`, gated by the unprompted-call budget) folds all but the newest `KEEP_RECENT = 16` into a ≤ 1200-character second-person summary ("who the hero is, what he asked, what you promised and gave, how you feel, important events and when") — `S/dialogue.py:163-164,173-179,1125-1161`. Presented to models as a `developer` message "[Что ты помнишь о прежних встречах с героем: …]" at the start of history — `S/dialogue.py:578-586`.

#### What is written, by whom
| Record | Written by | When | Where |
|---|---|---|---|
| hero phrase (`user`) and NPC reply (`assistant`) | code, from the turn (text is Sol's; in fights Gemma's line) | turn end, even if the NPC said nothing | `S/dialogue.py:2301-2310` |
| `developer` remarks "[…]" | ~25 modules through `DialogueManager.remember/note/overheard/heard/told_all` (deals, mercy, talk, meetings, witnesses, guide, quests, orders, gossip, blame, amends, group, gates, errands, social rumors…) | on events: tool outcomes, rumors heard, overheard speech, "new meeting", "hero interrupted you" | `S/dialogue.py:1732-1790,2313-2316`; callers counted by grep (`deals.py` 9, `mercy.py` 7, `talk.py` 6, `meetings.py` 6, `witnesses.py` 5…) |
| Gemma's SVM rewrites and combat barks (only when the voice actually began) | code, as `developer` remark "[Ты сказал вслух: «…»]" | when the line starts | `S/dialogue.py:1021-1035`, `D/npc-agent.md:9,69` |
| `summary` | Sol, background | > 40 messages | `S/dialogue.py:1132-1161` |
| `intent` `{goal, target, attitude, agenda}` | Sol via `set_intent`; code on incidents/lie found out/orders (`change_intent`) | on change | `S/executor.py:53-89`, `S/dialogue.py:3076-3116,3143-3160`, `S/memory.py:171-172`, `D/npc-agent.md:386-407` |
| `seen` sightings (hero, known people, notable strangers/bodies/blood/traces; start, every 15 game-min, end) | code (`Sightings`) from the engine's per-second `npc_views` | continuous, near the hero only | `S/sightings.py:1-45,246-451`, `D/npc-agent.md:964-985` |
| `chest` records | code (`chests.py`) | when the NPC learns its chest contents | `S/memory.py:353-362` |
| `fact` (chronicle): `died`, `spawned`, `quest*`, `claim`, `belief`, `lie`, `found_out`, `caught`, `rumor`, `testimony`, `crime*`, `npc_crime*`, `blame/blamed`, `deal`-related, `goal*`, `initiative`, `feat*`, `triggered`, `surrender`, … (≈ 70 kinds across modules) | code only (`Chronicle.record`) | when something verifiably happens | `S/chronicle.py:1-70`; kinds grepped across `S/*.py` |
| `patch` (director's story-graph edits, `told` of dialogs) | code (`story.StoryPatches`) | on director action / dialog chosen | `S/memory.py:410-419`, `D/protocol.md:623` |
| `vectors` (float16 per SHA-256 of text × model) | `MemoryStore` in a background task | after each message is appended, and when a conversation is opened | `S/memory.py:68-73,200-276` |

#### Belief/knowledge modules built on the log (`server/src/gothic_ai/`; all are `TurnExtension`s that add a text block to **Sol's** situation each turn)
- `knowledge.py` — what the NPC knows of the world from the scripts' story graph (own dialogs, quests, its guild's ambient talk), filtered by the current chapter/quest variables into told / open / hidden; block "Что ты знаешь…" ~1.5–3k chars; lexical `recall` — `S/knowledge.py:1-24,287-352`, `D/npc-agent.md:817-860`.
- `sightings.py` — "Что ты видел сам, своими глазами (не с чужих слов)", lines В1…, deeds Д1…, `catch_lie` — `S/sightings.py:519-580,642-720`.
- `witnesses.py` — `ask_witness`, fact `testimony`, block "Что тебе рассказали о герое другие" with lines С1… — `S/witnesses.py:1-40,354-410,498`.
- `trust.py` — projection of the hero's honesty per NPC: lied / heard / kept / amended / broke / crimes — `S/trust.py:1-40,149-166`.
- `people_knowledge.py` — "who knows what about whom", from `SOURCES` (exposed cover, broken word, seen NPC deed, blame, fame) — `S/people_knowledge.py:1-30,340-409`.
- `concerns.py` — per-NPC worries about the hero derived only from facts (debt broken/due, lied, witness, meeting missed, crime witness) → agenda for the Decider and an occasion to initiate talk — `S/concerns.py:1-26`, `S/initiative.py:1-30`.
- `lore.py` / `whereabouts.py` / `places.py` — facts about places and people from the NPC's own dialogs, `where` tool — `S/lore.py:430-460`, `S/whereabouts.py:33-90`, `D/npc-agent.md:941-962`.
- `social.py` — who knows whom (routines, shared quests) and rumor propagation — see section 3.
- Others add blocks too (deals, contracts, parley, bounty, mercy, gates, chests, errands, meetings, guide, orders, wrath, blame, amends, authoring, questbook, vanilla, crimes, group): 28 classes with `situation()` — `grep "def situation(self, npc"`; wiring `S/session.py:335-660`.

#### How memory is retrieved into prompts
- **History (both models):** `Conversation.history` = optional summary developer message + newest entries (≤ 40 before compaction), roles preserved (`developer` remarks included) — `S/dialogue.py:578-586`. Gemma gets it in first words and fight lines; **not** in SVM rewrites or combat barks — `D/npc-agent.md:9,67`.
- **Situation block (Sol only):** `render_situation` + all extensions' `situation()` text, appended after the history as a `developer` message and **not** kept in history — `S/npc_tools.py:204-252`, `S/dialogue.py:2761-2802`. Gemma gets only `render_situation(..., brief=True)` (+ "Силы" from `parley`), i.e. place, hero line, who is seen, body state and the intent; no inventory/targets/knowledge/trust/deals — `S/dialogue.py:1467-1474`, `S/npc_tools.py:204-252`.
- **`recall{query}` tool (Sol only):** `DialogueManager._recall` searches messages older than those already in the prompt (`before = min(summary_seq, first_entry_seq-1)`), semantic first, lexical on any `EmbedError`; then asks each extension that has `recall` (knowledge, lore, sightings, witnesses) — `S/dialogue.py:3008-3039`, `S/knowledge.py:349`, `S/lore.py:453`, `S/sightings.py:722`, `S/witnesses.py:498`. Output "Ты вспоминаешь: [game time] Герой/Ты: …" — `S/dialogue.py:3021-3027`.
- **Semantic path:** `gothic-embed` service on dev-ubuntu (FRIDA = `ai-forever/FRIDA`, 823 M params, 1536-d, bf16, port 47837, ~1.9 GB VRAM, `POST /embed {texts, kind: query|document}`, prefixes added by the service, L2-normalised float32) — `embed-service/README.md:5-45`, `D/compute.md:7`. Client `EmbedClient` (query timeout 2 s, document batch 30 s, ≤ 256 texts/4000 chars, 60 s backoff after any failure) — `S/embeddings.py:19-111`. `semantic_search` = candidate window (≤ 2000 newest messages of this NPC on this branch, minus service notes) ranked by dot product of normalised vectors, top 6 returned in time order; if > 256 vectors are missing it queues a background backfill and falls back to lexical — `S/memory.py:38,278-317`.
- **Lexical fallback:** 4-letter stems, count of query stems found as substrings, top 6, topped up with the newest — `S/memory.py:45,319-340,471-473`.
- Service notes ("[Новая встреча с героем", "[Ты закончил разговор", "[Герой ушёл", "[Герой перебил тебя") are excluded from both searches and from embedding — `S/memory.py:39-44,475-476`.
- Flags: `--no-embeddings`, `--embed-url`/`GOTHIC_EMBED_URL`, default host = decider's, port 47837 — `S/cli.py:116,155,1076-1081`, `D/features.md:36`.

#### The recall eval (`RM/README.md`)
- Question: how well `MemoryStore.search` finds old NPC lines; do we need embeddings, which for Russian. Data: 959 unique lines (806 distinct texts, 5 NPCs) from the Mac DB, in two settings — "own" (all lines of one NPC across branches: Cavalorn 435, Xardas 170) and "mixed" (all NPCs' lines); 25 topics, 83 queries in four kinds (`same` 25, `form` 8, `para` 25, `para2` 25), hit = at least one matching line among the 6 returned; MRR also given — `RM/README.md:22-33`.
- Result: word-level queries are solved (24–25/25 for every method); **paraphrase without shared roots: current search 22/50 (own) and 12/50 (mixed); BM25+Snowball 23/50 and 14/50; FRIDA 47/50 and 45/50**; multilingual-e5-large 45/50 and 39/50; USER2-base 41/38; e5-base 41/33; Qwen3-Embedding-0.6B 41/35; bge-m3 and USER-bge-m3 36/29–31 (public ruMTEB leaders did badly on short colloquial ASR-noisy lines) — `RM/README.md:5-9,36-63`. Paired sign test: FRIDA vs e5-large 3:1 (own) and 8:2 (mixed, p = 0.11) — not proven vs e5-large; clearly better than e5-base and nearly vs USER2-base — `RM/README.md:65-73`.
- Why the old search fails: stem `день` matches every "[Новая встреча…, день 1, 08:21]" note (466 of 959 lines were such notes); `твои` outweighs `иссл` — `RM/README.md:20,55-57`.
- Deployment costs: FRIDA 12.8 ms/query on RTX 3090, 473 ms/query on 2 CPU cores, 325 ms/line on CPU; fp16 breaks it (cosine 0.958), bf16 ok (0.998); vectors 6 KB/line float32, 3 KB float16 (10k lines = 30–60 MB); alternatives e5-large 1.04 GB, USER2-base 0.28 GB — `RM/README.md:77-89`.
- Acceptance and live check (2026-11-01/02): with the real service via `MemoryStore.semantic_search` own 47/50, mixed 45/50, same 25/25, form 8/8; client-side add-on ≤ 50 ms target; 25 ms p50 Mac→service — `RM/README.md:113-117,133`, `D/npc-agent.md:161,165`, `embed-service/README.md:20-24`.
- Eval limitations stated by the author: questions and labels written by the author knowing how search works; **no real `recall` queries exist in any log**; **in the real game recall barely triggers** (each branch is a new game with 5–20 lines per NPC, never reaching the compaction threshold); knowledge `recall` (stem substring) and summary quality were not measured — `RM/README.md:96-103`.
- Recommended next steps in the report (partly done): service + float16 vector table by text hash (done), BM25 fallback (the lexical fallback kept as is), exclude service notes (done), and "stable facts about the hero (name, promises, gave, debts) are taken by code from the chronicle; the model keeps attitude and impressions" (item 5, **not done as such**; trust/people/concerns projections cover part of it) — `RM/README.md:105-118`.

### Inferences
- Memory is already a good substrate for sharing: one branch-aware log, extensible `kind`, vectors keyed by text hash (not copied on branch). What is missing for sharing is a **consumer-side view**: today the same facts are re-rendered by ~28 extensions for Sol and by none for Gemma.
- The branch copy (`INSERT … SELECT … WHERE seq <= parent_seq`) makes every load copy the whole prefix (including `seen` events, the largest volume class locally), so growth is O(loads × history). On the Mac DB it is still tiny (see section 6).
- Recall quality is measured on a synthetic long history; the production history is short, so most of the value of embeddings is still prospective.

### Gaps
- No telemetry on how often Sol actually calls `recall` in play and what queries look like (`RM/README.md:99`).
- Quality of the 1200-char summary, and of the lexical `knowledge.recall`, is unmeasured (`RM/README.md:101-103`).
- I did not enumerate every fact kind exhaustively; the ≈ 70 count is from a regex over `"fact": "…"` literals (it also misses dict-built kinds).

---

## 3. Truth vs belief: where beliefs live, how a hero's lie is recorded, how knowledge spreads

### Takeaway
Principle: "code owns truth, NPCs own beliefs; a hero's lie must work and have consequences" — implemented as a fact log (truth + what each NPC was told/believed/found out) plus per-NPC projections; a model never writes a fact directly, it chooses via tools (`accept_quest`, `check_claim`, `catch_lie`, `ask_witness`, `blame`, `propose_deal`) and code validates against the chronicle. Knowledge spreads by code-driven rumors over an acquaintance graph (hops, daily caps), by witnesses/overhearing remarks, by testimonies, and by crime reports to the law.

### Cited Findings
- Statement of the principle and module map: `D/README.md:139-180` ("Код владеет правдой, NPC — убеждениями. Ложь героя должна работать и иметь последствия"); `S/chronicle.py:1` ("code owns the truth, npc's own their beliefs"); `RA/2026-11-02-gemma-sol-hybrid/mind.md:37` (in the mind, "only opinions and beliefs; what was promised/given/concluded is recorded by tools and the chronicle, so the mind promises nothing itself").
- **Truth** = `Chronicle` facts: JSON events `kind='fact'` in the branch log, rolled back with the save — `S/chronicle.py:1-20,50-62`, `S/memory.py:364-373`. Each `record()` also sends `memory{branch, seq}` to the game so the save carries the new head — `S/chronicle.py:57-66`.
- **Belief layers (NPC-owned):**
  - per-NPC message log incl. remarks (what it was told/overheard) — `S/dialogue.py:1732-1790`;
  - `seen` (own eyes; "не с чужих слов") — `S/sightings.py:1-45`;
  - `intent.attitude`/`agenda` (the only affective state persisted per NPC) — `S/executor.py:53-89`;
  - projections from the fact log: `trust.Record` (lied/heard/kept/amended/broke/crimes) — `S/trust.py:1-66`; `people_knowledge` (about other people, direct vs "со слов: X") — `S/people_knowledge.py:340-361`; `concerns` — `S/concerns.py:1-26`; crime-side/knowledge (`crime_side`, `npc_crime_known/seen`) — `D/crime.md` lines on layers, `D/README.md:145-176`;
  - `blame.py` keeps the victim's belief about a culprit apart from the truth: the model picks `blame{who, crime}`, only code knows whether it was true — `S/blame.py:1-5`.
- **How the hero's lie is recorded**
  - Quest-bound claims ("it is done"): `claim` fact (with `truth`, `checked`) + `belief{npc, quest, claim: done, believed: true, source: hero}` + (if false) `lie{quest, npc, truth}`; the lie is not blocked — `S/vanilla.py:1080-1130`, `S/quests.py:239-247,272`. A lie is found out by rules over the chronicle (creatures still alive near the giver, `check_claim` using the engine's sight, a witness, a trace) → `found_out`/`caught` fact, giver turns hostile, tells people around → rumors — `S/quests.py:1-18,300-346`.
  - Free-form lies in talk (e.g. "I was in the city all morning") are **not parsed into facts**: they are just `user` messages in the NPC's log; persona rule: "what the hero says about his deeds is only his words; whether it is true you do not know; whether to believe is up to you as {name}" — `S/persona.py:64-66`. They become a recorded lie only when Sol calls `catch_lie{said, seen}` with a concrete own-sighting (В#), witness line (С#) or seen deed (Д#), and code checks both that the NPC saw it and that the hero said those words (`heard(conv, said)`) → `Lie(said=True)` → `caught` fact + rumor — `S/sightings.py:102,642-714`, `D/npc-agent.md:976-985`.
  - Consequences are produced by code: `trust` (an NPC that caught him takes his word no more, quests accepted only on what it sees), `amends` storylets (fine/reward back), `consequences` (vanilla dialogs of the deceived treat the quest as still running), `deals` (`forgive`, `silence`) — `S/trust.py:1-24`, `S/amends.py:1-30`, `S/consequences.py:1-25`, `S/deals.py:1-45`.
- **How knowledge spreads between NPCs**
  1. *Witnesses at the scene* — `crimes.py` takes real witnesses from the engine; each witness gets a remark in its own log and a sighting — `D/README.md:145-177`, `S/sightings.py:306-375`.
  2. *Overhearing* — `overheard`, `heard`, `told_all`, `overheard_talk` add remarks to nearby NPCs with no model call — `S/dialogue.py:1768-1797`; group.py lets bystanders hear NPC lines (`on_said`) — `S/dialogue.py:773`.
  3. *Rumors* — `social.Rumors` over an acquaintance graph (people meet by shared daily routines within 25 m at the same hours, or share a quest with ≤ 6 people; outlaws know only each other); a rumor passes on a day's meeting with p = 0.7, or to a dealing partner p = 0.5, ≤ 3 tellings/day while "fresh" (2 days), ≤ `HOPS` retellings (e.g. caught 2, found_out 3, murder 4, fame 3); chances are drawn from a hash of who/what/day so a run repeats; time jumps replay the meetings in order. Each retelling = a `rumor` fact + a remark in the listener's log ("X told you…"), written first; the live scene (gossip.py, two NPCs speak in their voices near the hero) is only decoration — `S/social.py:1-38,40-70,541-612`, `S/gossip.py:1-20`. Rumors about NPCs (not the hero) flow as facts without a scene; consequences for the hero read only rumors with subject `PLAYER` — `D/README.md:177`, `D/npc-wave3.md:11-13`.
  4. *Errands* — Sol's `plan` tool: go and tell someone / take a thing; near the hero it physically walks, far away it resolves as a fact — `S/errands.py:1-30`.
  5. *Asking a witness* — `ask_witness`: code, not the model, decides what the witness saw (from its own sightings, with cover/fear), result = `testimony` fact + line С# — `S/witnesses.py:1-40`.
  6. *Crimes* — `crimes.py` (deeds, witnesses), `crime_react` (on-site reaction by character rules; model gets only context), `crime_meet` (later meetings by personal knowledge and knowledge of the law), `crime_law` (community law: lawman/guard report immediately, other witnesses on a timer), `crime_memory` (projects known deeds into the vanilla NPC memory), `npc_crimes`/`npc_far` (NPC-on-NPC deeds), `crime_side` (witness side/excuses) — `D/crime.md:5-20`, `D/README.md:145-178`, `S/crime_memory.py:1-8`.
  7. *Fame* — public feats spread as `feat_fame` rumors and let NPCs recognise the hero — `D/README.md:178` (feats bullet), `S/people_knowledge.py:363-386`.
- **Deception by NPCs**: code decides at deal acceptance whether an NPC will keep a deferred promise (`npc_intent{keeps}` by honesty/greed/courage), the model is not told in advance; the window shows no lie — `D/npc-wave3.md:16-20`. Witness cover (`cover` deal, fear) changes only `told`, never `saw` in the testimony — `S/witnesses.py:25-30`.
- **Name hygiene before memory (2026-11-03, #826):** ASR mangles names ("Ковалорн", "Сардас"); `fix_names` rewrites them to the right ones *before* any model or memory sees the phrase (19 of 23 mangled names fixed on 447 live phrases, 0 wrong; 0.33 replacements/1000 words on original dialog text; ~6 ms); original text kept in the journal as `heard`; rule in persona `RULES` forbids correcting the hero's pronunciation (#776) — `S/talk.py:268-320,554-576`, `S/persona.py:67-69`, `RA/2026-11-03-hero-names/README.md:3-9,38-63`, `D/npc-agent.md:115,127`. Reason it matters for memory: Sol used to correct names in 5/123 turns and the mind v2 turned that into "hero kept confusing the name" 16 times — `RA/2026-11-02-sol-mind/README.md:235`.

### Inferences
- There are two distinct stores of "what the NPC believes": **log-resident, free text** (messages, remarks, summary, mind) that models read and write, and **structured, fact-derived projections** (trust, people, concerns, crime knowledge) that only code writes. Any shared memory for Gemma and Sol should keep that boundary: Gemma-visible content must come from the first kind or from a rendered, read-only projection of the second.
- Sol's free-form lie detection rests on a specific sighting; unspecific doubt can live only in intent attitude, the summary, or (in the pilot) the mind's "сомневаюсь".

### Gaps
- I did not trace every one of the ~70 fact kinds' readers; the table in section 2 is representative, not exhaustive.
- The effect of rumor content on Gemma-spoken lines is nil today (Gemma never sees the "trust"/"people" blocks), which no document discusses explicitly; this is my observation from `S/dialogue.py:1467-1474` vs `S/dialogue.py:2773-2793`.

---

## 4. The Sol "mind" pilot (#791, #807, #824): what the mind document is, versions, costs, problems

### Takeaway
The "mind" is a per-NPC state document that Sol updates between hero phrases (opinion of the hero, events between us, feelings with strength and cooling, wishes by horizon, beliefs with source, doubts, opinions of others, open threads, mood) plus a short "note to the voice" that Gemma reads first. Version history: v1 rewrote six fields (8.1 s, 314 tokens), v2 added richer carry-over between meetings but bloated (14.3 s, ~1027 tokens, up to 1439), v3 stores state in code and has Sol emit **patches** (7.1 s, 153 tokens, fits the window 91 %); remaining problems: stale items, 1.64× Sol calls, not implemented in the game, user's verdict pending.

### Cited Findings
- Origin and design: user decision 2026-11-02 "Gemma speaks simple cases, Sol is the character's mind"; written plan with what exists in code, how others do it (Inner Thoughts, Letta sleep-time agents, Generative Agents) and acceptance — `RA/2026-11-02-gemma-sol-hybrid/mind.md:1-86`. The plan's table of what already exists (intent, trust, claims/beliefs, deals, blame, summary, background turns) and "what is missing" (no regular reflection between turns; `_brief` for Gemma lacks trust/deals/knowledge — those extensions go to Sol only) — `RA/2026-11-02-gemma-sol-hybrid/mind.md:5-29`.
- **Mind turn** (one Sol call after each hero phrase/NPC reply, prompt `mind_turn*.md`): sees the character description, recorded conversation, situation, and its previous mind; writes in order: `[Голосу]` (≤ 50 words: Тон / Хочу / Помню / Нельзя / "Если он: … → …"), then the mind update, then optional tool calls (`set_intent`, `propose_deal`, `withdraw_deal`, `give_item`, `end_conversation`, `take_offense`) and `[Скажу]` — `RA/2026-11-02-sol-mind/README.md:21-26`, `RA/2026-11-02-sol-mind/mind_turn_v3_r2.md:1-57`. Gemma gets "thought" + "mind" as two developer blocks before the hero phrase (`speak_mind.md:1-11`) with the same hand-off rule `[sol]`.
- **v1** (11 conversations, 138 turns, 49 handed to Gemma): "Голосу" ready in 5.3 s median (p90 6.4 s) and in time in 122/130 turns (94 %); whole turn 8.1 s; 314 output tokens; fields changed per turn: opinion 30 %, belief 20 %, doubt 37 %, wish 20 %, threads 71 %, mood 30 %; judge found 0 false facts with full history; acts: round 1 made no tool calls (1 of 14 needed actions), round 2 ("you decide and do, the voice only speaks") 10 of 14 needed actions and 5 of 84 extra calls; on recorded history 9 of 10 calls closed "promised in words, no tool" gaps and 1 repeated a deal. **Cost: 227 Sol calls vs 138 (1.64×; 1.4–1.8× per conversation)** — `RA/2026-11-02-sol-mind/README.md:87-143,145-177`.
- **v2** ("richer mind, carried between meetings", #807): events with game time (≤ 12 shown), feelings with strength 1–3 that cool, wishes now/soon/long, beliefs with source (saw/told/heard), others, threads, mood; state carried across meetings along the save-branch chain from `memory.sqlite3` (`--carry`); Cavalorn on three later meetings remembered the theft, bandit help and letter, and anger — **but the mind is rewritten every turn and bloats: median 1027 / max 1439 output tokens, whole turn 14.3 s, ≤ 4300 chars at the end of a chain, only 62 % of turns fit the window**; "events" became a log of everything; 6 of 7 judge flags were "hero confused the name" learned from Sol's recorded name corrections; 13 stale items — `RA/2026-11-02-sol-mind/README.md:179-248`.
- **v3 "patches"** (#824): state held as JSON by code (`new_state`, sections events/feelings/wishes/beliefs/doubts/others/threads + opinion + mood), Sol outputs one line per change; numbered items (б1, ч2…) so edits reference IDs not text (Letta pitfall); code rules: feelings lose 1 strength per term (hour/day/week) by game time, events keep ≤ 12 by importance then recency, event importance 1–10 and ≤ 3 forgotten after a conversation, "now" wishes dropped at conversation end, caps (beliefs 10, doubts 5, others 8, threads 8, wishes 2 per term), duplicates skipped, unknown lines/numbers skipped and counted — `RA/2026-11-02-sol-mind/README.md:256-298`, `RA/2026-11-02-sol-mind/mind.py:56-131,206-216,304-314`, patch grammar `RA/2026-11-02-sol-mind/mind_turn_v3_r2.md:15-27`.
- **v3 numbers** (second round, v2 control at the same slow-backend hour): output tokens 153 median / 307 max (v2 788 / 1439), whole turn 7.1–7.2 s (v2 14.6 s same hour), whole turn fits the window 91 % (v2 62 %), voice note fits 95 %, mind ≤ 1708 chars at the end of a conversation (v2 ~4300), 281 patch lines with 0 not understood, 0 false facts, 0 "name confusion" minds; stale items by judge 18 (v2 14; v3 round 1 had 39); 16 acts of which 5 were repeats in closing turns — a stand gap (the closing turn's history had no tool calls of the last Sol reply) — `RA/2026-11-02-sol-mind/README.md:316-370`. Acceptance table: items 1–8 met or met under a caveat; item 9 (user reads the ribbons, "bad" ≤ 15 %) pending — `RA/2026-11-02-sol-mind/README.md:358-370`.
- Memory behaviour in v3: Cavalorn remembers theft, bandit help, the letter on three later meetings; anger set to "hour" cooled in two game hours while distrust stayed in opinion/event (importance 8); memory forgot 9 items after a conversation, cooled 2 feelings, evicted 3 doubts by cap — `RA/2026-11-02-sol-mind/README.md:351-357`.
- Planned production storage (not built): "each patch = an entry in the NPC log (`memory.py`), the mind = a fold of entries; it branches with saves like intent; Sol gets the whole mind, Gemma the note to the voice" — `RA/2026-11-02-sol-mind/README.md:194,377`, `RA/2026-11-02-gemma-sol-hybrid/mind.md:55`.
- Open design items listed by the author: the code must reject a `propose_deal` that repeats a deal already on the table and show the open deal in the mind's situation; closing turns must receive the last Sol reply's tool calls; names replaced before the models (done in #826); storage after acceptance — `RA/2026-11-02-sol-mind/README.md:372-378`.
- Decisions left to the user before building: the cost (an extra Sol call per hero phrase) and whether the NPC may speak on its own initiative (`[Скажу]`) — `RA/2026-11-02-gemma-sol-hybrid/mind.md:83-86`, `RA/2026-11-02-sol-mind/README.md:64-68`.

### Inferences
- v3 is the first mind format whose cost scales with the *change*, not the state size, which is the property a shared memory needs; the state document itself is already a candidate shared object (Sol writes patches, Gemma reads a rendering).
- The "Голосу" note — not the whole mind — is what must be ready before the hero speaks (95 % in time), and it is written first on purpose.
- Rough size of the pilot's Gemma-visible mind: ≤ ~1.7k characters + a ≤ 50-word note (≈ 0.6–0.8k tokens at Russian ~2.5 chars/token — my estimate, not measured).

### Gaps
- No in-game run of the mind; the pilot stand replays recorded conversations (Cavalorn, Xardas) with recorded history, so the effect on live Sol and on Gemma's talk in context is unknown.
- User's judgement of the mind ribbons and of Gemma-by-mind vs Gemma-by-note blind sheet (38 turns) is pending — links to artifact pages only (`RA/2026-11-02-sol-mind/README.md:139-140`).
- Per-NPC storage design, multi-NPC interaction (mind of "others" vs other NPCs' own minds), and the interplay with chronicle projections (trust etc.) are not designed.

---

## 5. What the prompts for Gemma and Sol contain today, sizes, and prompt caching

### Takeaway
Both models receive the same persona instruction string (~5.5k chars for Cavalorn) and the same history; Sol additionally gets 12–15 tool schemas and a ~5.5–7k-character situation block assembled from ~28 extensions (≈ 8–10k input tokens per Sol step, about half cached on the WebSocket), while Gemma gets only a brief situation (+ a hint) and relies on a per-NPC llama.cpp slot that is prewarmed with the persona + history prefix.

### Cited Findings
#### Persona instructions (identical for both)
- `render_instructions`: intro ("You play {name} in Gothic 2 NotR… with you speaks the main hero") + biography (hand-written for Xardas, else the character card) + director-person parts + `RULES` (3544 chars) + 24 evenly spread sample lines from the original dialog (30–220 chars each) — `S/persona.py:30-33,52-94,194-225`. Deterministic per NPC for prompt caching (`S/persona.py:3-4`). Cavalorn: 5456 chars **[local measure]** (trace 2026-10-18/20).
- `RULES` content: speak 1–3 sentences/≤ 40 words, Russian only, stay in role, what you know is in the situation and `recall`, no knowledge of the future, hero's statements about his deeds are only words, ASR mangles names so never correct them, remarks in brackets are not hero's words, begin with a mood tag, tool usage rules (say first, then call; `set_intent` only on change; deals via `propose_deal`) — `S/persona.py:52-94`.

#### Sol's request
- Layout: `instructions` (persona) → `tools` (all, ~12–15) → `input` = `[summary developer msg][≤ 40 history messages, incl. developer remarks][developer: situation][trigger]`; situation is per-turn and *not* stored in history — `S/dialogue.py:2466`, `S/npc_tools.py:204-212`.
- Situation of Cavalorn, 2026-10-20 trace **[local measure]** (5905 chars): headline "[Обстановка — видна только тебе…]", time/part of day, place, hero line (distance, guild, armour), inventory, "Видишь:", "На земле:", body state, intent + targets, then extension blocks: "Что ты знаешь (глава 1…)" (told dialogs; "Если герой спросит… есть что ответить" 1390 chars; open quests; "Подробности тем — через recall"), "Что ты знаешь о делах героя", "Что ты видел сам…" with В#/Д#, "Кого из знакомых можно расспросить (ask_witness)", "Что ты знаешь о местах и людях вокруг", "Что ты знаешь о честности героя", deal helper ("Дела героя для сделки"), chest/lead/errand offers, "Ты ждёшь от героя: «Бандиты Кавалорна»" (994 chars), "Ты можешь сам дать герою поручение…" (464 chars), body state. Largest turns reached 7067 chars.
- Sizes **[local measure]**: Sol step input tokens p50 5.4k (2026-10-12/14), 8.6k (2026-10-18/20), max 10.7k; cached share 23 % (HTTP era) → 53 % (WebSocket era); a typical turn adds ~0.9k new tokens, an incident turn up to ~3k (`D/npc-agent.md:424`). The prewarm probe replayed an 8.3k-input-token first step with 15 tools — `RA/2026-11-01-sol-prewarm/README.md:15`.
- Prompt cache: keyed by NPC id (`prompt_cache_key`); effective only over the per-NPC WebSocket (46 % of input tokens cached vs 0 % over HTTP in a 6-turn dialogue; in-game earlier 5 % of requests/2.9 % of tokens over HTTP); continuation steps send only the delta (`previous_response_id`: 4/4 cached, 91 % tokens); director/amends/gossip keys have too-short fixed parts (697/570/318 tokens < the 1024 threshold) so they get no cache, only a warm socket — `D/npc-agent.md:421-434`. The subscription "warm-up" is accepted but not usable: the real request re-reads the whole prompt (128 of 8703 tokens cached either way) — `RA/2026-11-01-sol-prewarm/README.md:41-47`. The tool list is always the same set so the prefix stays stable — `D/npc-agent.md:421-423`.

#### Gemma's requests
- First words: `[persona][history][developer: brief situation + REACT_HINT][hero phrase]`, temperature 0.6, first sentence only (stream cut at the first sentence) — `S/dialogue.py:1467-1474,1850-1867,2448-2449`. Brief situation = `render_situation(snap, intent, sight, brief=True)` (no inventory, no intent targets) + parley's "Силы:" line — `S/npc_tools.py:204-252`, `S/dialogue.py:1467-1474`.
- Fight line: `[persona][history][developer: brief situation + QUICK_HINT "Сейчас горячо…"][trigger]`, ≤ 120 tokens, cut at the first blank line (Gemma can append written-out tool calls) — `S/dialogue.py:124,2464-2466,2853-2857`, `D/npc-agent.md:528-531,550`.
- SVM and combat barks: persona + one developer message (brief situation + hint) with **no history** — `D/npc-agent.md:9,67`.
- Typical size: a fight/talk prompt is 2–5k tokens, 3–4k read in ~1.2 s cold; combat-bark prefix 1756/2005 tokens reused — `server/deploy/gemma/run.sh:10`, `D/npc-agent.md:542,67`.
- Cache: llama.cpp slot per NPC (`id_slot`), 3 slots + classifier slot, `--no-cache-idle-slots`; prewarm = send persona + history with `max_tokens=1` after a 0.1 s debounce (`PREWARM_DELAY_S`) whenever memory changes, only for ≤ 3 candidates; real requests cancel a running prewarm and never wait for it; slot eviction by least-recent use; map cleared on branch switch — `S/dialogue.py:146-152,1207-1299,1065-1094`, `D/npc-agent.md:534-544`. Without `--swa-full` the sliding-window cache cannot roll back beyond ~512 tokens, so prewarm only appends (works as long as the prefix is append-only) — `D/npc-agent.md:548`.
- Anything inserted into Gemma's *history* or persona changes the prefix and forces re-prefill; each new message calls `prewarm()` (`S/dialogue.py:1109`).

### Inferences
- The "brief vs full situation" split is the main existing seam: whatever shared memory exists, a brief rendering of it is what Gemma can afford; the full rendering is Sol's.
- Because Gemma's slot cache is append-friendly but not edit-friendly, a mutable mind block should sit last before the hero phrase (as in `pilot.py:269-285`) and be prewarmed when it changes (1–2 s p90 measured).

### Gaps
- No measured token count of the Gemma request in production by component; the only figures are the ranges above (2–5k) and one combat-bark measurement.
- I did not measure the persona-instruction token count (chars only).

---

## 6. Persistence: save/load, sessions, size

### Takeaway
Memory is durable on disk and rolled back exactly like the saves: every memory write moves a head `{branch, seq}` that the game writes into the save file; loading a save makes a new branch copying the parent's events up to the saved seq, so anything after the save is forgotten with the world. Size is small today (811 KB for 28 branches on the Mac) but each load copies the parent prefix.

### Cited Findings
- Protocol and flow: `session{branch, parent, parent_seq, reason}` from the game; `memory{branch, seq}` after every write; the save stores `{branch, seq}` under `ai_bridge/session`; loading starts a new branch with `parent` and `parent_seq`; saves without the record start a branch with no memory — `D/protocol.md:419-426`, `S/protocol.py:135-143,1677-1682`, `S/memory.py:1-12,139-156`.
- On branch switch the dialogue manager cancels turns, clears conversations, quick-slot maps and prewarm state, and re-reads memory per NPC on first use — `S/dialogue.py:1065-1094`. Chronicle, social/rumors, concerns re-derive from the log on `Session` (`S/chronicle.py:153-158`, `S/social.py:613-620`, `S/concerns.py:201`).
- Survives a process restart/new game session: SQLite file; embeddings table is keyed by text hash and not duplicated by branches — `S/memory.py:68-73`, `D/npc-agent.md:890`.
- Not branch-persisted in the pilot: the mind is only in the pilot stand's JSON; the production plan is to store patches as log entries (section 4).
- Not persisted: an unconfirmed deal offer ("living only in process", `RA/2026-11-02-gemma-sol-hybrid/mind.md:12`), the `catch_lie` duplicate-guard set (`_caught`, a load forgets it, mitigated by reading `caught` facts, `S/sightings.py:714-720`), the quick-model repeat history (`conv.first_words`, SVM phrase deque).
- Size **[local measure]** (Mac `~/.local/share/gothic-ai/memory.sqlite3`, last write 2026-11-02, before the `vectors` table existed): 811 008 bytes, 28 branches, 1117 `message` events (avg 79 chars), 365 `seen` (avg 268 chars, the largest class), 119 `fact` (avg 258), 111 `intent` (avg 95), 17 `chest`, 12 `summary` (avg 1108 chars), 6 `patch`. The recall-eval dump (959 distinct lines, 5 NPCs) is the same DB at an earlier time — `RM/README.md:24`. Estimates from the report: FRIDA vectors 3 KB/line float16, 10 000 lines = 30–60 MB — `RM/README.md:88`.
- Each branch physically copies its inherited events (`INSERT INTO events SELECT …`) — `S/memory.py:149-153`.

### Inferences
- With a mind stored as patches (3–6 lines per turn), a 100-hero-phrase playthrough per NPC adds on the order of 10^2–10^3 small events: negligible vs `seen`.
- Per-load O(prefix) copying is the only scaling concern; no document flags it.

### Gaps
- No measurement of DB growth over a long real playthrough (the author notes every real branch so far is a new game with 5–20 lines per NPC; `RM/README.md:100`).

---

## 7. Constraints stated in the docs that rule options in or out

### Takeaway
Hard constraints: all runtime models live on one machine (dev-ubuntu, RTX 3090) with the target of a single player PC with a ≥ 16 GB GPU; no big models on the Mac; Gemma must produce first sentences in < 1 s and the whole system must degrade gracefully when a model/service is down; new subsystems are on by default with `--no-X`; model behaviour must not be scripted in e2e.

### Cited Findings
- **Where models run:** big models are not run on the MacBook at all (ASR, LLM, TTS, embeddings, generators); small nets trained for the engine run in the engine; everything needing GPU lives on dev-ubuntu; all models used during play live on that one machine (decision 2026-10-27) with the aim "a player with a ≥ 16 GB GPU runs everything on one PC with one button, so a new model is not put on a second machine" — `AGENTS.md` "Где считаются модели", `D/compute.md:13`.
- **VRAM budget on dev-ubuntu (24 GB):** Gemma + Strands ≈ 13.4 GB; + FRIDA → 15.3 GB (1.9 GB); + Zencastr laughter 0.7 GB; VoxCPM2 and Voxtral are stopped; Gemma 7.4–8.3 GB, Strands 4.7 GB — `RM/README.md:89`, `D/compute.md:7-9,142-151`, `D/README.md:60-66`. The 16 GB-single-PC target is tighter: "Gemma and Strands already take ~14 GB, so FRIDA would have to run on CPU (0.5 s per query) or be swapped for e5-large (1 GB)/USER2-base (0.3 GB)" — `RM/README.md:89`, `embed-service/README.md` (env `EMBED_MODEL`, `EMBED_DEVICE`).
- **CPU:** dev-ubuntu is shared with CI (Forgejo builds with nice 0); services pinned to cores 2–11; no swap, a stray 10 GB process can OOM prod — `D/compute.md:102,151`.
- **Latency budgets in code/docs:** Gemma first words ≤ 1.2 s (else skipped), ≤ 8 words; SVM rewrite ≤ 2.0 s/12 words; combat bark ≤ 1.2 s/6 words; surrender probe 1.5–2 s; fight line starts within 3 s else Sol; per-model stall guards 15 s — `S/dialogue.py:113-123,153-156`, `D/npc-agent.md:9,67,227`. Pilot acceptance targets: classifier ≤ 0.3 s median (0.09 s measured), first sentence after hero phrase ≤ 0.8 s median (0.47 s), "thought" in time ≥ 90 % (96 %), mind whole turn fits ≥ 85 % (91 %), mind ≤ 7 s, recall add-on ≤ 50 ms — `RA/2026-11-02-route-classifier/README.md:63-67`, `RA/2026-11-02-hybrid-gated/README.md:52-61`, `RA/2026-11-02-sol-mind/README.md:306-314`, `RM/README.md:113-117`.
- **Degradation rules:** no Gemma → Sol speaks as before; no embedding service → lexical search; Gemma down 3× → SVM rewrite off, native SVM stays; no Sol → error status — `D/npc-agent.md:552`, `S/embeddings.py:1`, `D/npc-agent.md:8-10`.
- **Quota/cost:** Sol runs on the user's ChatGPT plan; "an extra Sol call per hero phrase = about twice the plan calls" is explicitly a user decision before building the mind — `RA/2026-11-02-gemma-sol-hybrid/mind.md:83-86`; unprompted-call buckets `S/budget.py`; "words spend quota, so they sound only when the hero is near" — `D/npc-tools.md:1190`; live e2e suites spend subscription quota and are manual — `D/e2e.md:467-480`.
- **Options already ruled out or untested:** Sol warm-up (not built); FRIDA must not run in fp16 (bf16 only); Qwen3-Embedding 4B/8B and GigaEmbeddings are too big for the VRAM budget, jina-embeddings-v3 has a non-commercial licence, EmbeddingGemma is gated (not tested); paid Inworld STT/TTS are not used in automated tests (`PAID_TTS=1`/`PAID_ASR=1` needed) — `RA/2026-11-01-sol-prewarm/README.md:5`, `RM/README.md:86,91-94`, `AGENTS.md` (section «Тесты»).
- **Process rules:** new subsystem = on by default, `--no-X` switch in `cli.py`, wired in `session.py`; memory-like state that must follow the save goes into the branch log; protocol changes bump the version in three places; e2e must not script models (`given`/`silent` declared; live suites verify behaviour); "new = research first", and after two rejected rounds of the same element stop and research instead of a third tuning round — `server/AGENTS.md:7-11`, `AGENTS.md` (research-first section).
- **Visual/quality gates:** the user's ear decides "sounds as good"; LLM raters are harsher than the user (Claude blind rater 25/3/2 good/ok/bad for Sol vs the user's 13/16/1 on the same sheet) so calibration against a user-rated sheet is needed — `RA/2026-11-02-hybrid-gated/README.md:156-168`.

### Inferences
- A design that adds a GPU model (e.g. a bigger memory embedder or reranker) conflicts with the 16 GB single-PC target; one that reuses Gemma (classification via grammar, as in the surrender and route classifiers) or FRIDA (already deployed) does not.
- A design that adds one more Sol call per hero turn is a user-level cost decision, not an engineering one; patch-style updates keep the per-call output at ~150 tokens.

### Gaps
- The VRAM table in `D/compute.md:142-151` is stale (still lists Voxtral and VoxCPM2 that were stopped); the current total is derived from other lines (13.4 + 1.9 + 0.7 ≈ 16 GB) and not re-measured in one place.
- The AGENTS.local.md memo (private) was not read; machine addresses and the 23.6 GB figure are outside this note.

---

## 8. Known gaps and open issues

### Takeaway
There are no open GitHub issues about NPC memory, recall, Gemma/Sol or beliefs (`gh issue list --state all` returned 17 issues; the only NPC-related one is the auto "hot file" debt for `npc.cpp`, #794; #795 flags `cli.py` size). The real gap list lives in the pilots' "what next" sections and the recall eval's "not measured".

### Cited Findings
- `gh issue list --state all --limit 200` (2026-11-04): 17 issues; open `долг`: #827 (hero-balance), #802, #801, #799, #797, #796, #795 (`server/src/gothic_ai/cli.py` hot file, score 756 558, 3821 lines), #794 (`npc.cpp`), #793; open without label #851, #571; none mentions memory/recall/Gemma/Sol/belief/gossip/embedding. Debt policy: `AGENTS.md` "Техдолг", `D/tech-debt.md`.
- Pilot-level open items:
  - gated hybrid: one commitment slipped; the "previous turn to Sol" rule needs validation on fresh traces; classifier prompt needs a "report of completed deed" case (checked only on new traces) — `RA/2026-11-02-hybrid-gated/README.md:178-190`, `RA/2026-11-02-route-classifier/README.md:140-143`;
  - hand-off phrases of Gemma ("Вижу её") sometimes awkward; Gemma-by-thought lacks concrete detail (letter, chest contents) because the harness gives it no memory (user's analysis: "a question of what our harness gives Gemma") — `RA/2026-11-02-hybrid-gated/README.md:107,170-176`;
  - Gemma marks `[sol]` on 128 of 360 simple phrases and mostly answers anyway — `RA/2026-11-02-repeats/README.md:9`;
  - mind: stale threads/doubts (18), repeated `propose_deal`, stand history gap, cost 1.64×, no storage, user verdict pending — section 4;
  - first-words/`recall` residual: `recall` costs a ~4 s step; the knowledge block gives topic starts to avoid it — `D/npc-agent.md:884`;
  - recall eval: no real queries, summary quality unmeasured, knowledge recall lexical — `RM/README.md:96-103`.
- Stated limits of knowledge: the model still knows Gothic 2 from training; rules and filtering only reduce leakage, "no guarantee, check on every new model" — `D/npc-agent.md:882`.
- Known live-test limits: live suites are manual, spend quota, and most NPC behaviour tests run with `off` models ("given") — `server/AGENTS.md:9`, `D/e2e.md:187`.

### Inferences
- The most design-relevant unstated gap is that Gemma has **no path to the structured knowledge** (trust, people knowledge, sightings, deals): the pilots solve this by routing such turns to Sol and giving Gemma only Sol's thought. A shared memory has to decide whether Gemma gets a compact rendering of those projections or keeps relying on the hand-off.
- The "name fix" and "no promises by Gemma" rules are preconditions for any memory that either model writes, otherwise ASR noise and unverified promises get memorised (observed in the v2 mind).

### Gaps
- No issue tracker entries to cite for open work; the trackable items are only in report prose.
- I did not check every `docs/npc-tools.md` / `docs/MVP.md` "open questions" list.

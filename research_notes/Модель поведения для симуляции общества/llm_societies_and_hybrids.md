# LLM-driven agent societies and hybrid designs (LLM sets goals, cheap components execute) — fact sheet as of 2026-11-04

Reading notes: "authors' claim" = number from the paper itself (self-reported); "independent" = measured or critiqued by someone else. Where I only saw a search-engine snippet or a secondary summary, I say so. Dollar figures use the pricing of the model and year named. Derived numbers (my arithmetic) are marked "derived" and live in the Inferences blocks.

---

## 1. What LLM agent societies achieved, what they cost per agent per simulated day, how they scaled, what failed

### Takeaway
Fully LLM-driven societies reach the following. About 25 agents at roughly $25-40 per agent-game-day (Smallville, 2023). About 1,000-10,000 agents only by cutting LLM calls to ~5 per agent per few rounds (AgentSociety) or by moving LLM use to archetypes, prototypes or caches (AgentTorch, APS, GenWorld). The headline "1,000 agents" (Project Sid) and "1M agents" (OASIS) are real systems, but the quantified experiments are much smaller or take days of GPU time. Reported failures are consistent: hallucination that propagates through memory, over-cooperative and homogeneous personalities, and speech/action mismatch.

### Cited Findings

**Generative Agents / Smallville (Stanford, 2023; 25 agents; GPT-3.5-turbo)**
- 25 agents, two game days, `gpt3.5-turbo`. The authors say the run cost "thousands of dollars in token credits" and took "multiple days to complete". No exact dollar figure. Simulation ran at 1 s real time = 1 min game time — [Park et al. 2023 (HTML)](https://arxiv.org/html/2304.03442v2)
- A third-party estimate: Lyfe Agents' appendix assumes $2,000 total for the 25-agent, 2-day run, which it converts to about $25 per agent per human-hour (16 game hours/day, 10x game speed) — [Lyfe Agents](https://arxiv.org/html/2310.02172v1)
- Token measurement by the Affordable Generative Agents authors: baseline 25.41M tokens (±0.96M) for the 25-agent town over 2 game days; 4.548M for a 3-agent town over 2 days — [AGA](https://arxiv.org/html/2402.02053v2)
- Emergence: Sam's candidacy spread from 1 agent (4%) to 8 (32%); Isabella's party from 1 (4%) to 13 (52%); network density 0.167 → 0.74. Of the 12 agents who heard about the party, 5 attended; 3 cited conflicts, 4 expressed interest but did not plan to come — [Park et al. 2023](https://arxiv.org/html/2304.03442v2)
- Failures reported by the authors: retrieval failures (agent says it "hasn't followed the election" despite hearing of it); embellishment (an agent invents "Sam will make an announcement tomorrow"; 1.3% of 453 awareness answers were hallucinated); norm errors (agents enter a single-occupancy bathroom while occupied; enter stores after the 5 pm closing); instruction-tuned agents too cooperative and formal ("Isabella rarely said no", her interests drifted) — [Park et al. 2023](https://arxiv.org/html/2304.03442v2)
- Retrieval score = recency + importance + relevance with all weights 1, recency decay 0.995 per game hour; reflection fires when summed importance of recent events exceeds 150 (about 2-3 reflections per day) — [Park et al. 2023](https://arxiv.org/html/2304.03442v2)

**Project Sid / PIANO (Altera, 2024; "10 to 1000+" agents; GPT-4o)**
- PIANO runs 10 distinct modules concurrently. Reflex-type modules use "small, fast non-LLM neural networks"; goal generation does "deliberate reasoning over graphs". A bottlenecked Cognitive Controller makes the decision and broadcasts it to other modules to keep speech and action coherent. Social goals are generated "every 5-10 seconds" — [Project Sid (HTML)](https://arxiv.org/html/2411.00114)
- Model: GPT-4o was required for single-agent results ("older base LMs" did not reach them). Role inference used GPT-4o. Claude 3.5 Sonnet snapshot used in a model comparison — [Project Sid](https://arxiv.org/html/2411.00114)
- Latency, per-module timing, LLM call counts and cost per agent are not reported in the paper text I read — [Project Sid](https://arxiv.org/html/2411.00114)
- Quantified runs in the paper: 25 agents (item benchmark, 30 min), 49 agents for 4 h (saturating at ~320 unique items, about one third of all Minecraft items), 50 agents for 4 h (social simulation), 29 agents for 1200 s (collective rules), 500 agents for 9000 s (cultural transmission, 20 Pastafarian priests). The text states that runs at 1000+ agents "exceeded the computational constraints of our Minecraft server environment" and shows "sporadic unresponsiveness" — [Project Sid](https://arxiv.org/html/2411.00114)
- Emergence: with the social module, roles (farmer, miner, engineer, guard, explorer, blacksmith) diversified and persisted; without it they did not specialise. Tax experiment: agents deposited about 20% of inventory under the initial rule and "9%" after the rate was lowered to 5-10%. Religion: converts rose and "did not saturate" after two hours. 25 agents with full PIANO reached an average of 17 unique items after 30 min — [Project Sid](https://arxiv.org/html/2411.00114)
- Failures: an agent claims to eat a bagel without eating, and the false claim propagates to later prompts; chat says "Sure thing!" while the function call says "explore"; weak spatial reasoning and building; no robust innate drives. "Personality collapse" is not discussed in the text I retrieved — [Project Sid](https://arxiv.org/html/2411.00114)
- The press describes "up to 1,000 agents"; the paper's abstract says 10 to 1000+. I did not find a quantified 1,000-agent experiment — [abstract](https://arxiv.org/abs/2411.00114)

**AgentSociety (Tsinghua FIB lab, 2025; 10k+ agents; DeepSeek-V3 API)**
- Over 10k agents and about 5 million interactions; "up to 10k agents, each averaging 500 interactions per day" (interactions are not all LLM calls) — [AgentSociety](https://arxiv.org/abs/2502.08691)
- Agent state: demographic profile and personality; six emotion intensities (sadness, joy, fear, disgust, anger, surprise, 0-10) plus keyword and thought sentence; Maslow-style needs hierarchy updated from behaviour, events and psychological state; attitudes 0-10; social relations (family, friend, colleague) with strength 0-100; stream memory with event flow and perception flow — [AgentSociety v2](https://arxiv.org/html/2502.08691v2)
- Mobility uses a gravity model; economy has taxes, a Taylor-rule interest rate and a statistics bureau — [AgentSociety v2](https://arxiv.org/html/2502.08691v2)
- Infrastructure: Ray actors, agent groups each in one process with asyncio; MQTT (EMQX) messaging; PostgreSQL batch writes; vLLM or Ollama for local inference. Message throughput: Redis 81,216 msg/s, MQTT 44,702 msg/s, RabbitMQ 23,668 msg/s; Kafka could not initialise 100,000 agents in 5 minutes — [AgentSociety v2](https://arxiv.org/html/2502.08691v2)
- Scalability table (five interaction rounds, DeepSeek-V3 API): 1,000 agents → ~4,800 LLM calls, ~413-430 input and ~76-79 output tokens per call, 41-82 s per round; 10,000 agents → ~54,000 LLM calls, ~430 input and ~76 output tokens per call, 5,681 s per round with 8 groups, 1,422 s with 16, 459 s with 32. LLM time dominated; cost not reported; "no request limit" on the API — [AgentSociety v2](https://arxiv.org/html/2502.08691v2)
- Environment only (no LLM): mean time per step of a 24-hour simulated day 0.0086 s at 10^3 agents, 0.0091 s at 10^4, 0.018 s at 10^5, 0.168 s at 10^6 — [AgentSociety v2](https://arxiv.org/html/2502.08691v2)
- Case studies (all authors' claims): gun-control polarisation — control 39% more polarised / 33% more moderate, homophilic group 52% more polarised, heterogeneous group 89% more moderate; UBI of $1,000/month raised consumption and lowered CES-D depression scores, directionally like Texas findings (no numeric match reported); hurricane Dorian with 1,000 agents — activity fell from 70-90% to ~30% at landfall, tracked SafeGraph trips with deviations at the peak — [AgentSociety v2](https://arxiv.org/html/2502.08691v2)
- Homogeneity, hallucination and mode collapse are not discussed in the sections I retrieved — [AgentSociety v2](https://arxiv.org/html/2502.08691v2)

**OASIS (CAMEL-AI, 2024; up to 1M agents; Llama-3-8B-Instruct on vLLM)**
- 21 action types (a third-party write-up says 23). One time step = 3 min of simulated time. Base LLM Llama3-8b-instruct; GPT-4o-mini used only as polarisation judge — [OASIS v4](https://arxiv.org/html/2411.11581v4)
- Cost of a single time step: 1M agents → 27 A100s, 18.0 h per step (48.5K new tweets, 97.1K comments); 100K agents → 5 A100s, 3.0 h; 10K agents → 2 A100s, 0.2 h. Herd-effect runs: 10K agents 15 min per step on 4 A100s, 1K 0.83 min, 100 0.33 min. Dollar cost and number of agents active per step not reported. The authors say simulating millions of agents "still requires several days" — [OASIS v4](https://arxiv.org/html/2411.11581v4)
- Activation: each agent has a 24-dimension hourly activity vector; in the misinformation run activation probability 0.1 for core users and 0.01 for regular users; herd-effect run 0.1 per step — [OASIS v4](https://arxiv.org/html/2411.11581v4)
- Results: information spreading reproduces 198 real Twitter15/16 cases with normalised RMSE ~30%; rumours reached more users than true information at 1M agents; explicit herd effect appeared only for dislikes and only at larger scales; opinion diversity and "helpfulness" rose from 196 to 10,196 to 100,196 agents; uncensored Llama-3-8B shifted more toward conservative views in group polarisation — [OASIS v4](https://arxiv.org/html/2411.11581v4)

**Stanford "Generative Agent Simulations of 1,000 People" (Park et al., 2024)**
- 1,052 real Americans, two-hour semi-structured interviews fed to an LLM. On held-out GSS items, agents reach 83% (interview), 82% (survey), 86% (both) and 74% (demographics only) of the participants' own two-week test-retest consistency in the current arXiv page; the v1 abstract and a secondary summary quote 85% for the interview-based agent. Agents reduce accuracy gaps across racial and ideological groups versus demographics-only — [arXiv page](https://arxiv.org/abs/2411.10109); [search summary of v1 numbers](https://bohrium.dp.tech/paper/arxiv/2411.10109). Discrepancy 85% vs 83% likely reflects the version updated in 2026; not resolved.
- This is a survey-answering replica of individuals, not a society simulation with interaction over time.

**Humanoid Agents (2023, EMNLP demo)**
- Adds basic needs, one of Ekman's seven emotions, and relationship closeness to Generative Agents. Reported F1 = 0.972 for predicting when activities increase fullness and 0.861 for energy effects — [Humanoid Agents summary](https://arxiv.org/abs/2310.05418) (figures via search summary)

**Lyfe Agents (2023)**
- Claims 0.5 USD per agent per human-hour (Sec. 4.3) against its own ~25 USD estimate for Smallville; GPT-3.5 only; abstract says 10-100x cheaper while the introduction appears to say ~30-100x (internally inconsistent). 3, 6 and 9-agent murder-mystery tests; 9 agents: police officer named the right suspect >60% of the time within 15 minutes — [Lyfe Agents](https://arxiv.org/html/2310.02172v1)

**2025-2026 large simulations**
- SocioVerse (Fudan, 2025): pool of 10 million real social-media users (X, Rednote), three simulations (election prediction, news feedback, national economic survey); tested Llama-3-70B, Qwen2.5-72B, DeepSeek-R1/V3, GPT-4o/4o-mini; models tend toward conservative attitudes — [SocioVerse](https://arxiv.org/abs/2504.10157) (via search summary)
- CityReal (2026; 3,000 agents in Tokyo, "tens of thousands" claimed; GPT-5.4-mini): 19.58M tokens per day per 1,000 agents (~19,580 per agent-day), $6.76 per 1,000 agents per day (~$0.007 per agent-day), one-time calibration about $248. LLM calls per agent-day not reported — [CityReal](https://arxiv.org/html/2608.16897)
- GenWorld (2026; 196,608 synthetic residents): online LLM simulation of 200,000 agents with 15-minute steps (T = 96) "would require 19.2 million online LLM calls" per simulated day; local 7B call estimated at 0.5-2 s — [GenWorld](https://arxiv.org/html/2606.27650)
- APS (2026; 10M agents, 8 rounds): full simulation = 80M LLM calls and 9.56 days; APS = 209.9K calls and 26.64 h — [APS](https://arxiv.org/html/2605.27419v1)
- OpenCity (Tsinghua/HKUST group): 10,000 agents' daily activities in about 1 hour on commodity hardware; arXiv abstract says 600x faster per agent, requests -70%, tokens -50%; a later citation in a search snippet gives 635x, -73.7% requests, -45.5% tokens — [OpenCity abstract](https://arxiv.org/abs/2410.21286); the 635x variant is from a search snippet, unverified
- AgentScope (2024): actor-based distributed platform for "very large-scale" simulation; a search snippet claims 1M agents in 12 minutes on 4 devices (85 minutes with a different prompt); the abstract page I fetched does not confirm those numbers — [AgentScope](https://arxiv.org/abs/2407.17789)
- ScaleSim (Jan 2026) is a memory-efficient LLM serving system for large multi-agent simulation; I only saw the title and abstract-level description — [ScaleSim](https://arxiv.org/abs/2601.21473)

**Homogeneity, persona collapse, mode collapse (independent critiques)**
- "The Chameleon's Limit" (arXiv 2604.24698): LLM responses to detailed personas cluster into classes while human responses spread out; collapse is "universal but task-dependent" across the LLMs tested (the abstract says eight; one search snippet says ten); models with the highest per-persona fidelity gave the most stereotyped populations; sampling more personas did not help and persona-targeted fine-tuning narrowed rather than diversified the population — [abstract](https://www.alphaxiv.org/abs/2604.24698)
- PIMMUR audit of 42 LLM social-simulation papers (2023-2025; 34 evaluable): 90.7% violate at least one of six principles (profile homogeneity, pseudo-agency of interaction, statelessness, goal injection, visibility leakage, circular validation); only three comply. In a reproduction, adding profile diversity changed the proportion of balanced states by about 14% — [PIMMUR](https://arxiv.org/html/2509.18052v2)
- AgentTorch authors admit archetype-level LLM queries cost diversity among individual agents — [AgentTorch](https://arxiv.org/html/2409.10568v3)

### Inferences
- Cost per agent-game-day (derived): Smallville ≈ $2,000 / (25 agents × 2 days) ≈ $40 per agent-day (GPT-3.5, 2023). AGA token counts: 25.41M / 50 agent-days ≈ 508K tokens per agent-day (baseline) and 10.86M / 50 ≈ 217K (AGA). 3-agent town: 758K → 236K per agent-day. CityReal (GPT-5.4-mini, 2026): ≈ 19.6K tokens and ≈ $0.007 per agent-day, i.e. 25-30x fewer tokens than Smallville per agent-day while being a much coarser agent.
- AgentSociety 10k: 54,000 calls × ~506 tokens ≈ 27M tokens per five-round block, ≈ 2.7K tokens per agent per five rounds (derived). The table caption says "five interaction rounds"; the extraction does not make clear whether "LLM calls" is per round or total, so treat ≈ 5.4 calls per agent per five rounds as a lower reading, ≈ 1.1 per agent per round otherwise.
- OASIS GPU cost (derived): 18 h × 27 A100 = 486 GPU-hours per 3-minute step at 1M agents; one simulated day = 480 steps ≈ 360 days of wall clock on that cluster if all steps cost the same. At 10K agents (0.2 h × 2 GPUs per step) a simulated day ≈ 96 h. Active-fraction effects (0.1 and 0.01 activation) mean the real figure depends on activation probability.
- Everything that reaches 10^4-10^7 agents does it by not calling the LLM per agent per step: ≤ ~5 calls per agent per block (AgentSociety), prototypes (APS), archetypes (AgentTorch), caches (AGA, GenWorld), or grouped prompts (OpenCity). No published system runs a per-agent per-step LLM at 10^4+ for a full simulated day within a day of wall clock without a large GPU cluster or an API with no rate limit.
- The agent state that societies actually use (needs, emotions, relation strengths 0-100, attitudes 0-10, inventories, economy) is mostly low-dimensional numeric state. AgentSociety, Humanoid Agents and CityReal keep it explicitly outside the LLM, which matches the proposed "graph of needs/goals/tasks/property/relationships" design.
- The best-documented failure classes for a game: (1) a false statement enters memory and spreads (Sid bagel, Smallville embellishment), (2) speech/action mismatch, (3) everyone agrees (Isabella, persona collapse), (4) location or world norms not in state (bathroom, shop hours). Park et al. themselves suggest fixing (4) by encoding norms in location state, not text.

### Gaps
- No cost per agent-day for Project Sid, OASIS (dollars) or AgentSociety (dollars or GPU-hours); none of the three report it in the text I read.
- No quantified 1,000-agent Project Sid experiment found; the paper says it exceeded server constraints.
- Exact LLM calls per agent per simulated day for AgentSociety, Sid and OASIS are not stated.
- Lyfe Agents' 10-100x claim is not independently measured; its Smallville baseline is its own estimate.
- Persona-collapse numeric metrics (Coverage, Uniformity, Complexity values) not seen; only the abstract.
- AgentScope's "1M agents in 12 minutes" and OpenCity's 635x are from search snippets and not confirmed on the primary page.
- I did not find any 2025-2026 society sim that measures personality drift over many simulated weeks.

---

## 2. Hybrid patterns: LLM for rare decisions, cheap model or rules for frequent ones

### Takeaway
Published hybrids split into five patterns: (a) fast reflex modules plus a slow LLM controller (PIANO, SwiftSage); (b) the LLM acts once, offline, to build a plan/skill graph and cheap executors run it (Plan4MC, Voyager skill library, SayCan skills); (c) cached or grouped LLM decisions reused across agents with the same context (AGA, AgentTorch, GenWorld, OpenCity, APS); (d) explicit non-LLM state (needs, finances, beliefs) with the LLM called at decision points (CityReal, Humanoid Agents, AgentSociety); (e) LLM as goal generator whose goals become rewards for a learned policy (ELLM). Cost cuts reported range from ~2x to ~380x in LLM calls, usually at some loss of diversity.

### Cited Findings

**(a) Fast reflex + slow LLM**
- SwiftSage: Swift = flan-t5-large (~770M parameters) fine-tuned on ~62k action-prediction examples from an oracle agent (2,157 training variations); Sage = GPT-4 for subgoal planning and grounding. Sage fires when Swift is stuck (5 consecutive zero-reward steps), makes an invalid prediction, a critical decision (e.g. "focus on"), or hits an unexpected exception; each activation makes two LLM calls and yields an action buffer of about 5 actions — [SwiftSage](https://arxiv.org/html/2305.17390)
- ScienceWorld score (GPT-4 baselines): SwiftSage 84.68, Reflexion 45.34, ReAct 36.43, SayCan 33.82; Swift alone 49.22. Tokens per action: SwiftSage 757, SayCan 1,856, ReAct 1,971, Reflexion 2,983. The paper does not report how often Sage was called — [SwiftSage](https://arxiv.org/html/2305.17390)
- Project Sid PIANO: concurrent modules, reflex modules as small non-LLM networks, slow goal reasoning, Cognitive Controller as the single broadcast bottleneck — [Project Sid](https://arxiv.org/html/2411.00114)
- Lyfe Agents: an option-action framework where one LLM call picks an option plus subgoal; option termination uses non-LLM checks (time triggers, repetition detection). Cost claim 0.5 USD per agent per human-hour. But ablating the option-action framework did not hurt measured performance, only made conversations shorter (23.8 s vs 70.3 s) and each step costlier. No smaller model is used for low-level actions; each talk step is still an LLM call — [Lyfe Agents](https://arxiv.org/html/2310.02172v1)
- inZOI "Smart Zoi" (Krafton + NVIDIA ACE): NVIDIA states a 0.5B-parameter Mistral NeMo Minitron small language model runs on-device; community wiki says ~1 GB VRAM, English only, opt-in experimental. Vendor claim, no evaluation numbers — [NVIDIA announcement](https://www.nvidia.com/en-gb/geforce/news/nvidia-ace-naraka-bladepoint-inzoi-launch-this-month)

**(b) LLM plans once, cheap skill executors run**
- SayCan: LLM proposes skills, per-skill value functions (affordances) score feasibility, choose the highest combined score. 101 tasks: 84% plan success, 74% execution success (PaLM-SayCan) — [SayCan project page](https://say-can.github.io/); [paper](https://arxiv.org/abs/2204.01691)
- Plan4MC: skills (Finding, Manipulation, Crafting) are trained with RL using intrinsic rewards; the LLM is used only before execution to produce the skill graph, and a search walks that graph at run time. 40 Minecraft tasks, many needing 10+ skills in sequence — [Plan4MC](https://arxiv.org/abs/2303.16563)
- Voyager: GPT-4 writes code programs as skills, stores each verified program in a library indexed by the embedding of its description; reports 3.3x more unique items, 2.3x longer distances, up to 15.3x faster tech-tree milestones than prior state of the art (the NeurIPS listing quoted 3.1x in a search snippet; the arXiv abstract says 3.3x) — [Voyager](https://arxiv.org/abs/2305.16291)

**(c) Caching, grouping, prototypes, archetypes**
- Affordable Generative Agents (AGA): plans the LLM decomposed and the agent executed successfully are stored with their execution conditions (the "Lifestyle Policy"). Reuse requires cosine similarity ≥ 0.97 on text-embedding-ada-002 plus an environment-condition check. This is a plan cache, not a trained model. Token use as % of Generative Agents baseline: 3-agent town 31.1% (1.417M vs 4.548M), 25-agent town 42.7% (10.86M vs 25.41M), VirtualHome day 3.4% (1,189 vs 34,327). Lifestyle Policy alone 40.2%, Social Memory alone 58.6%. Social Memory cut ~30-45 retrieved events (~2,000 tokens) to ~100 tokens per conversation. Reaction and reflection modules were left unchanged in the 25-agent run — [AGA](https://arxiv.org/html/2402.02053v2)
- AGA quality: GPT-4 human-likeness (5-pt) activity 3.21 vs 3.13, dialogue 4.01 vs 3.97; VirtualHome task success LLM-judged 85.0% vs 87.0%, human-judged 53.3% vs 42.6% — [AGA](https://arxiv.org/html/2402.02053v2)
- AgentTorch LLM archetypes (MIT Media Lab): 8.4M NYC agents (COVID, Aug 2020-Jul 2022); archetypes are unique combinations of characteristics (e.g. age × sex); each archetype queried M=3 times, mean of yes/no answers gives a probability, each agent then draws an action from a Bernoulli with that probability; budget fixed at 300 queries per step (about 400 queries total in validation); GPT-3.5 (GPT-4o in a sensitivity analysis). MSE: unemployment — archetype 24.59, heuristic 41.05, LLM-as-agent 56.98; infection — archetype 95.17, heuristic 2,914.73, LLM-as-agent 4,311.70. About 95% less runtime than LLM-as-agent. Limits: lower diversity, simple actions, sensitive to model and M — [AgentTorch](https://arxiv.org/html/2409.10568v3)
- GenWorld: persona/state/time/scenario are binned into a context key; the teacher (Gemma 3 27B) is queried K = 10-30 times per key offline; mean scores [0-10] become a sampling distribution; at run time decisions are sampled from the table and still pass the same validator/executor. Lookup ≈ 1 µs (Python micro-benchmark 1.85M queries/s on an i5-14600K). Unseen keys back off to a coarser key or default — [GenWorld](https://arxiv.org/html/2606.27650)
- APS: LLM queried only for adaptive "core prototypes", singleton tail agents and shadow-audit agents; the rest are interpolated from prototypes in their stratum. 10M agents, 8 rounds: 209.9K calls vs 80M (381.1x fewer), JSD 0.094 vs full-LLM reference, 26.64 h vs 9.56 days. At the same 15.5K-call budget on 10K agents: APS JSD 0.011 vs stratified sampling 0.088. LLM was glm-4-flash (10M run) — [APS](https://arxiv.org/html/2605.27419v1)
- OpenCity: IO-multiplexed request scheduler plus "group-and-distill" (cluster agents with similar static attributes, share prompts): requests -70%, tokens -50%, ~600x acceleration per agent, 10,000 agents' day in ~1 h — [OpenCity](https://arxiv.org/abs/2410.21286)

**(d) Explicit non-LLM state, LLM at decision points**
- CityReal: explicit models track financial pressure, beliefs and needs; a belief-aware gravity model chooses points of interest (no LLM); the orchestrator invokes the LLM at decision points such as start or end of an activity or when a need becomes urgent. Calibrated "textual adapters" are learned once (~$248) and reused across agents, days and counterfactuals, propagated to the rest of the population by nearest neighbour in persona-embedding space — [CityReal](https://arxiv.org/html/2608.16897)
- AgentSociety and Humanoid Agents keep needs/emotions as explicit state updated after activities (see section 4) — [AgentSociety v2](https://arxiv.org/html/2502.08691v2); [Humanoid Agents](https://arxiv.org/abs/2310.05418)
- Generative Agents re-plan only on a "react" decision: each step the LLM decides whether to continue the plan or react; if react, the plan is regenerated from that moment. Plan hierarchy: 5-8 coarse chunks per day → hour chunks → 5-15 minute steps — [Park et al. 2023](https://arxiv.org/html/2304.03442v2)

**(e) LLM-generated goals as reward**
- ELLM: LLM sees a textual description of the agent's state and suggests goals; the agent gets intrinsic reward for achieving them. Tested on Crafter and Housekeep; pretrained agents covered more common-sense behaviours and usually matched or beat baselines downstream — [ELLM](https://arxiv.org/abs/2302.06692)

**Triggering and routing**
- Beyond SwiftSage's four triggers and CityReal's decision-point orchestrator, I found no paper that measures a novelty or conflict detector in a society simulation. A search for event-triggered routing found only AGA-style caches and a vendor blog (Gravitee) listing event thresholds, deduplication and a cheap pre-filter model — [Gravitee blog](https://gravitee.io/corpus/gen-2309/algorithmic-trading/llm-cost-management-for-trading-agents.html) (vendor, not research)

### Inferences
- The proposed project design (one efficient non-text model drives needs/goals/tasks, LLMs change state from conversations) has these closest published analogues: Project Sid's reflex modules + controller; CityReal's explicit needs/finance/belief models with LLM at decision points; SwiftSage's small fine-tuned model with trigger-based LLM escalation; GenWorld's offline lookup. None combines a single learned non-text society model with LLM-written state edits; this is a gap the project would be filling, not copying.
- Reduction in LLM calls grows with how coarse the reuse key is: plan cache at 0.97 similarity ≈ 2.3-3.2x token cut (AGA); group-and-distill ≈ 2x tokens, 3.3x requests (OpenCity); archetypes ≈ 20x less runtime than LLM-as-agent (AgentTorch: 95% less) with ~300-400 queries for 8.4M agents, about 4 orders of magnitude fewer queries than agents; prototypes ≈ 380x (APS). Diversity loss grows with the same coarseness (AgentTorch limitation; APS tail-protected routing exists to counteract it).
- SwiftSage shows that a ~770M model trained on only ~62k examples can double the quality of SayCan-style prompting on its own benchmark (49.2 vs 33.8) at lower token cost, but the full hybrid (84.7) needs the LLM for stuck, invalid, critical and unexpected cases. Those four triggers map directly to: no-progress timer, invalid action (code validator), high-stakes event, unexpected world exception.
- For a game with a real-time budget, GenWorld's key-bucketed lookup (µs per decision, offline LLM sampling K = 10-30) and AGA's plan cache are the cheapest published runtime paths; both are unvalidated against fresh teacher decisions or limited to a plan-reuse test (see section 3).

### Gaps
- No published measurement of a novelty/conflict-triggered router in a many-agent society, including the fraction of decisions that reach the LLM.
- SwiftSage does not report how often Sage was called.
- Lyfe Agents' option-action ablation shows no benefit in performance; the cost benefit is only claimed.
- inZOI figures are vendor-reported and there is no published evaluation of Smart Zoi behaviour quality.
- Cost of the LLM call share in PIANO is not given.

---

## 3. Distillation of LLM decisions into small models

### Takeaway
Small models trained on LLM trajectories close most of the gap on narrow benchmarks (about 82-93% of teacher score in the best-documented cases) with 10^3-10^4 trajectories, but the evidence is for QA, math, text games and dialogue, not for society-scale behaviour. The two society-scale "distilled" systems (GenWorld lookup, AGA plan cache) are not trained models and have no quality validation against fresh teacher output. LLM judges inflate the quality of trained students.

### Cited Findings
- Agent Distillation (KAIST et al., 2025): teacher Qwen2.5-32B-Instruct; students Qwen2.5 0.5B/1.5B/3B/7B (also Llama-3.2-1B, Phi-4-mini). About 2,000 filtered trajectories (1,000 HotPotQA + 2,000 MATH questions, one correct trajectory per question). Eight-task average: teacher agent 46.00; 7B student 42.68 (≈ 93% of teacher); 3B 36.60; 1.5B 30.55; 0.5B 21.90. 3B agent ≈ 7B CoT distillation (33.54). Training with LoRA rank 64, 2 epochs on four A100 80GB. Self-consistent action generation samples N = 8 candidates per step (so inference cost grows 8x per step). Authors note small models hallucinate on new facts — [Agent Distillation](https://arxiv.org/html/2505.17612v2)
- Sub-goal Distillation (CoLLAs 2024 / PMLR 2025): a 770M student with a planning module (learns sub-goal sequences from an LLM-annotated oracle paths) and an execution module (elementary actions); +16.7% absolute on ScienceWorld over standard imitation learning; no LLM needed at inference, so LLM cost becomes a "fixed cost" — [PMLR page](https://proceedings.mlr.press/v274/hashemzadeh25a.html)
- Sotopia-π (ACL 2024): 462 GPT-4-generated social tasks (from 510 inspiration prompts), 10 interactions per task with 10 character pairs; behaviour cloning (BC) on GPT-4-vs-GPT-4 data, then self-reinforcement (SR) on the student's own top 20% interactions; QLoRA rank 8 on Mistral-7B; 4× A6000 80G per checkpoint. Goal completion on the hard subset (GPT-4 judged, 140 points): GPT-4 5.92, base 3.84, BC 5.25, BC+SR 5.34. Human-judged (28 points): GPT-4 5.25, base 2.89, BC 3.55, BC+SR 4.29. So GPT-4 as judge overstates BC+SR by 1.42 (5.71 vs 4.29) vs 0.64 for GPT-4 itself. MMLU 5-shot 49.21 base vs 48.57 BC+SR (SR alone 43.46). Toxic-word count in a small study: expert 0.3, base 3.6, SR 5.5, BC 7.5, BC+SR 0.9. Training cost not reported — [Sotopia-π](https://arxiv.org/html/2403.08715v3)
- SwiftSage's Swift module is trained on 62,083 examples from an oracle (rule-based) agent, not on LLM output; 500 steps, batch 128, lr 1e-4 — [SwiftSage](https://arxiv.org/html/2305.17390)
- LLM4Teach: an LLM teaches a small RL student, and the student continues training with environment feedback and eventually surpasses the teacher; MiniGrid and Habitat; abstract gives no numbers — [LLM4Teach](https://arxiv.org/abs/2311.13373)
- GenWorld "distilled policy": offline teacher queries (Gemma 3 27B, K = 10-30 per context key) compiled into lookup tables. The paper states it did not quantify fidelity against fresh teacher queries (no KL or agreement), did not ablate K, temperature or teacher, and that end-to-end speedup profiling is "ongoing work" — [GenWorld](https://arxiv.org/html/2606.27650)
- AGA: the learned "Lifestyle Policy" is a store of successful plans, no model is trained; benefits arise "mainly from batching" because policies must be built by running the system repeatedly — [AGA](https://arxiv.org/html/2402.02053v2)
- APS counts a "LS-Surrogate" baseline (8.0M labels) that scored JSD 0.264 vs APS 0.094 at 10M agents — [APS](https://arxiv.org/html/2605.27419v1)
- CityReal's one-time calibration cost of about $248 is reused across days, scenarios and population; this is a data point for refresh cost, not for refresh frequency — [CityReal](https://arxiv.org/html/2608.16897)
- Vendor claim (not independent): TensorZero reports fine-tuned small models beating GPT-4.1 at 5-20x lower cost and 2-4x faster on τ-bench and BabyAI-type tasks — [TensorZero](https://www.tensorzero.com/blog/distillation-programmatic-data-curation-smarter-llms-5-30x-cheaper-inference/)
- Another search result notes that distillation cost is dominated by teacher inference over the distillation set; student training is comparatively cheap — [Lenovo paper](https://arxiv.org/pdf/2602.20164) (via search summary)

### Inferences
- Data volume that has worked: ~2K trajectories (Agent Distillation, correct-only), ~1K interactions per round and ~460 tasks (Sotopia-π), ~62K oracle examples for a 770M fast module (SwiftSage). For a text-to-structured-action model the order of 10^3-10^5 labelled decisions is the evidence range; none of it is for an open-world NPC graph.
- Quality gap as ratio to teacher (derived): Agent Distillation 7B ≈ 93%; Sotopia-π BC+SR human-judged ≈ 82% (4.29/5.25) and 90% by GPT-4 judge (5.34/5.92); gaps grow with student size reduction (0.5B ≈ 48% of teacher on the average).
- Because judges inflate students (Sotopia-π +1.42) and human judgement flipped AGA's ranking (LLM judge 85% vs 87%; human judge 53.3% vs 42.6%), student acceptance should use human review or held-out ground truth, not only an LLM judge.
- Refresh frequency: no paper measures how fast a distilled society policy drifts from the teacher. GenWorld notes "unseen keys" fall back to coarser keys or defaults; the project would need its own drift test (fresh teacher queries on logged states).
- Since the teacher costs dominate (Lenovo), generating labels once offline per scenario bucket (GenWorld-style) is cheap relative to online LLM use: 96 calls per agent-day × 200K agents = 19.2M calls online vs K = 10-30 per key offline.

### Gaps
- No published study distilling LLM decisions into a small model for an NPC/society setting with believability measured. The closest are Sotopia-π (dialogue), Agent Distillation (QA/math) and GenWorld (unvalidated).
- No measured refresh interval or drift rate for distilled social/behaviour policies.
- Data volume for the GenWorld table is the number of context keys × K, not reported as a total.
- Sotopia-π training cost (GPU-hours) not reported.
- Centaur-style behavioural foundation models (fine-tuned on human choice data) were outside scope and not verified.

---

## 4. Interface between language and state: dialogue → structured state changes, validation, goals and tasks between agents

### Takeaway
Systems that work keep authoritative state as structured data (JSON tree, inventories, numeric needs, graph) and let the LLM either propose actions that a game master or validator turns into state changes, or classify what an activity did to a numeric state. Nobody I found tracks promises or tasks as first-class formal objects with an LLM; delegation/obligation is done by chat, goals-from-social-perception, or elections and taxes coded as game mechanics.

### Cited Findings
- Generative Agents: the sandbox server holds a JSON record per agent (location, current action text, object interacted with). When an agent acts on an object the LLM is prompted to determine the object's new state (coffee machine "off" → "brewing coffee"). The world is a tree with containment edges; each agent holds its own, possibly stale, subtree. Agents choose a destination by descending the tree one level at a time. Users can edit object state in natural language ("stove is burning") and agents react — [Park et al. 2023](https://arxiv.org/html/2304.03442v2)
- Park et al. attribute norm errors (bathroom, shop hours) to norms that are hard to convey in text and suggest encoding them into location state ("one-person bathroom") — [Park et al. 2023](https://arxiv.org/html/2304.03442v2)
- Concordia (DeepMind): agents state intended actions in natural language; a Game Master inspired by tabletop RPGs turns them into concrete outcomes, checks physical plausibility, and in digital settings handles API calls to tools; agents are built from components mediating LLM calls and associative memory retrieval — [Concordia](https://arxiv.org/abs/2312.03664)
- Project Sid: the Cognitive Controller's decision is broadcast to all modules; speech was found inconsistent with the function-call action ("Sure thing!" vs "explore"), motivating the broadcast design. Items, inventories and tax deposits are directly measured game quantities. Collective rules ran through prompts for amendment creation, feedback, voting, tallying and constitution change (GPT-4o / GPT-4o-mini), with 25 constituents, 3 influencers and 1 election manager — [Project Sid](https://arxiv.org/html/2411.00114)
- AgentSociety: agents act through the environment (OpenStreetMap roads, SafeGraph POIs, driving models, a bank/tax/statistics layer); relationship type and strength 0-100 live in the agent status; messaging via MQTT topics per agent; needs, emotions, attitudes updated from behaviours and events — [AgentSociety v2](https://arxiv.org/html/2502.08691v2)
- Humanoid Agents: basic needs (hunger, health, energy) and emotion drive the next activity; the system classified when activities increased fullness with F1 = 0.972 and energy with F1 = 0.861 (via search summary) — [Humanoid Agents](https://arxiv.org/abs/2310.05418)
- GenWorld: sampled decisions "pass through the same validator and executor as teacher outputs"; each person must have a complete 1,440-minute schedule with no gaps or overlaps; land-use compatibility checker reports zero violations — [GenWorld](https://arxiv.org/html/2606.27650)
- CityReal: LLM used for contextual reasoning, explicit models for financial pressure, beliefs, needs; gravity model for place choice — [CityReal](https://arxiv.org/html/2608.16897)
- Agentic-world-modeling survey (2026) proposes a commitment-consistency criterion (an agent that breaks a promise without explanation, renegotiation or sanction scores low) and says formal commitment tracking (Telang et al. 2021) has not been integrated into any LLM architecture; it names role drift and goal forgetting as problems — [survey, via search snippet](https://arxiv.org/pdf/2604.22748)
- Contract Net Protocol adaptations with LLMs (manager sends call for proposals, bidders offer, manager awards): secondary sources describe richer bids (plans, resource lists) and mid-task requirement updates; one aggregator summary of "SPEAR" reports 1.8x faster failure recovery and 15% fewer LLM invocations vs direct assignment, which I could not verify. "Agent Contracts" (Jan 2026) adds delegated resource budgets that respect parent constraints — [CNP overview (aggregator)](https://www.emergentmind.com/topics/contract-net-protocol-cnp); [Agent Contracts](https://arxiv.org/html/2601.08815v2)
- In Smallville, coordination of the party happened through chat only: 12 agents heard of it, 5 attended; no explicit obligation object existed — [Park et al. 2023](https://arxiv.org/html/2304.03442v2)
- Role inference in Sid was done post hoc by GPT-4o reading agent behaviour, then entropy of the role distribution was computed — [Project Sid](https://arxiv.org/html/2411.00114)
- Emergent social norms through dialogue (metanorms) shown in natural-language Norms Game agents (Horiguchi et al., 2024) and a common-pool-resource simulation with norm-based punishment (Gupta et al., 2025) — [Horiguchi et al.](https://arxiv.org/abs/2409.00993); [Gupta et al.](https://arxiv.org/abs/2510.14401) (via search snippets)

### Inferences
- Pattern that recurs and fits the proposed design: LLM output is parsed into a closed vocabulary of actions or state patches; code validates (schedule complete, location compatible, inventory exists) before commit; the same validator handles teacher and cached/distilled output (GenWorld). Park et al.'s own recommendation to move norms from prose into state is the same principle.
- Speech-vs-action drift (Sid) argues for generating the utterance and the state change from one structured call, or committing the state change first and letting speech describe it.
- A promise or task as a record (giver, taker, terms, deadline, status) plus a consistency check against later behaviour does not exist as published practice with LLM agents (survey: not integrated in any LLM architecture). The project's graph with tasks and relationships would be ahead of the literature, and the CNP vocabulary (call, bid, award, update, reject, report) is the closest transferable protocol.
- Where numbers can be checked by code (inventory, tax %, items, schedules), results are easier to evaluate than for free text; Sid and GenWorld show this.

### Gaps
- No paper found with a measured error rate for "LLM dialogue → structured state patch" (schema violations, rejected actions per 1,000 calls) in a society sim.
- Concordia's Game Master cost (extra LLM calls per step) not found.
- No source on how goals or tasks are passed between agents in AgentSociety or OASIS beyond message passing.
- CNP/SPEAR figures are from an aggregator; primary SPEAR paper not located.

---

## 5. Evaluation: how believability and social realism are measured

### Takeaway
Methods in use: human ranking of interview answers (TrueSkill), LLM-judge rating, outcome distributions versus real-world data (survey, mobility, spread of rumours), mechanism-level counts (roles entropy, items, tax rate), and diagnostics for diversity. The evidence shows LLM judges inflate scores for trained agents, and many studies skip diversity and validation controls.

### Cited Findings
- Generative Agents: TrueSkill believability (μ/σ) on interview answers by 100 Prolific evaluators (~30 min each, $15/h): full architecture 29.89/0.72; no reflection 26.88; no reflection or planning 25.64; crowdworker-authored baseline 22.95; no memory, planning, reflection 21.21. Kruskal-Wallis H(4) = 150.29, p < 0.001; Cohen's d (prior-work condition vs full) = 8.16. Questions: 5 categories × 5 = 25. The authors note the crowdworker baseline "did not represent the maximal human performance", evaluation covered a short timescale, and robustness (prompt/memory hacking) is unknown — [Park et al. 2023](https://arxiv.org/html/2304.03442v2)
- Project Sid metrics: entropy of role distributions, % of inventory deposited as tax (95% CI over 25 constituents), unique items (averaged over 5 repeats), sentiment 0-10 (SEM over 4 repeats), perceived vs true likeability (Pearson r and regression slope), meme counts per agent, religious keyword counts for direct and indirect conversion — [Project Sid](https://arxiv.org/html/2411.00114)
- AgentSociety: reproduces four social experiments and compares simulated outputs to real data (SafeGraph trips for the hurricane; directional UBI vs Texas findings); deviation at the hurricane peak — [AgentSociety v2](https://arxiv.org/html/2502.08691v2)
- OASIS: information spreading vs 198 real Twitter15/16 cases (normalised RMSE ~30%); polarisation judged by GPT-4o-mini; herd effect vs 116,932 real Reddit comments across seven topics — [OASIS v4](https://arxiv.org/html/2411.11581v4)
- AGA: GPT-4 human-likeness (5-pt), GPT-4 relation-closeness (0-10), and task success judged by LLM and by humans; results differ by judge (VirtualHome 87.0% vs 85.0% LLM-judged, 42.6% vs 53.3% human-judged) — [AGA](https://arxiv.org/html/2402.02053v2)
- CityReal: Jensen-Shannon divergence against a 2021 Japanese time-use survey 0.0016 (time use) and 0.0021 (transport mode); GPT-5 judge 1-5 for activity 4.56, dialogue 4.32, mobility 4.46, event reaction 4.41; pairwise win rate vs prior frameworks with a Gemini judge (modest margin over CitySim); XGBoost beat CityReal on 5-class well-being macro-F1 (0.45 vs 0.43) — [CityReal](https://arxiv.org/html/2608.16897)
- GenWorld: census fit by tract (gender MAE < 0.02, employment R² > 0.99), schedule feasibility and zero land-use violations; commuting KS = 0.359 (synthetic mean 10.81 km vs 7.45 km observed); diurnal activity correlation r > 0.86 — [GenWorld](https://arxiv.org/html/2606.27650)
- APS: Jensen-Shannon divergence vs the full-LLM reference run and exact match; authors state this tests computational fidelity to the LLM, "not whether LLMs model real human behavior" — [APS](https://arxiv.org/html/2605.27419v1)
- Sotopia-π: LLM evaluators overestimate trained social agents (GPT-4 vs human gap 1.42 for BC+SR) — [Sotopia-π](https://arxiv.org/html/2403.08715v3)
- SimulateBench (Xiao et al. 2023): two believability metrics, consistency and robustness; agents lose character details when profiles are long and react unpredictably to small profile changes — [SimulateBench](https://arxiv.org/abs/2312.17115) (via search summary)
- ICML 2026 paper on human-likeness proposes comparing open-ended behaviour against large-scale human data on rationality, consistency and diversity instead of a Turing-style test; best tested model still shows a significant gap from real user behaviour — [ICML 2026 poster](https://icml.cc/virtual/2026/poster/65092) (via search summary)
- PIMMUR (2025): six validity principles (profile diversity, real interaction not summaries, persistent memory, minimal control prompts, unawareness of hypothesis, realism checks against human data); 90.7% of 34 papers violate at least one; recommends paraphrase robustness checks and describing results as model-specific until validated — [PIMMUR](https://arxiv.org/html/2509.18052v2)
- Stanford 1,052-person study measures agents against participants' own two-week self-consistency (normalised accuracy), a ceiling for how well any agent can match an individual — [arXiv page](https://arxiv.org/abs/2411.10109)
- A crowdsourced fact-checking study found LLM agents rate differently from humans (more sensitive to truthfulness, human raters more moderate) — [arXiv 2504.19940](https://arxiv.org/pdf/2504.19940) (via search summary)

### Inferences
- A defensible acceptance suite for the Gothic society would combine: (1) code-checkable invariants (schedules feasible, no impossible state, conservation of items/money); (2) distribution-level comparisons against designed targets or the teacher's logged decisions (JSD, KS, diurnal correlation), as in GenWorld, CityReal and APS; (3) mechanism counts (role entropy, spread of a seeded rumour or request through the graph) as in Sid and Smallville; (4) a small human-rated sample, because LLM judges inflate (Sotopia-π, AGA); (5) diversity diagnostics (coverage, uniformity of personalities) because persona collapse is not fixed by more personas or persona fine-tuning.
- Information diffusion (Smallville Sam/party percentages; OASIS rumour reach) is the cheapest social-realism test that is both quantitative and readily applied to a game graph.
- Believability for a player is not the same as statistical realism; the sources measure the latter (distributions) or reader preference on text, not 3D gameplay. I found no measure of long-horizon (weeks) personality stability.

### Gaps
- No paper found that measures believability of NPCs in a shipped game; inZOI/PUBG Ally evidence is promotional only.
- No standard benchmark for long-horizon consistency of agents in a society (SimulateBench covers profiles, not society).
- Persona-collapse numeric metric values and Stanford 1,000-person sub-results (Big Five, economic games, experiment replications) not retrieved.
- Several 2026 items (Chameleon's Limit, ICML position and human-likeness papers, Agentic World Modeling survey) were read only through abstracts or search snippets; no peer-review status verified.

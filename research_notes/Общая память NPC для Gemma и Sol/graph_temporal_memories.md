# Graph-based and temporal-knowledge-graph memory systems for LLM agents (as of 2026-11-04), seen through the Gothic NPC use case

Scope: everything except AriGraph (covered by another researcher). Vendor claims are marked "vendor" or "self-reported"; independent or third-party checks are marked as such. Where a figure came from a search snippet and not from a page I opened, I say so. Several 2026 numbers conflict between sources; the conflicts are kept, not resolved.

Reading guide for the report writer: section 1 (Zep/Graphiti) and section 2 (Mem0) are the two systems with real code and real production use. Section 5 explains why almost no LoCoMo/LongMemEval number in this file can be compared with another. Section 6 collects the practical points (write cost, small models, contradictions, provenance). Section 7 lists what the evidence implies for a per-NPC memory shared by a small local model and a large hosted model; those are inferences, not findings.

---

## 1. Zep / Graphiti: how it stores, updates, forgets, retrieves; cost; maturity

### Takeaway
Graphiti is the only widely used open-source engine with true bi-temporal edges and LLM-driven edge invalidation, and it is incremental per episode. All LLM cost sits on the write path (several LLM calls per episode, no published count or latency); the read path is embeddings + BM25 + graph traversal with no LLM, which Zep's page puts at a ~155-162 ms p95 (vendor). Its weak points for us are: a published ingestion cost nobody has measured independently, strict structured-output requirements that small local models often fail, and benchmark numbers that swing from 58 to 95 depending on who ran them.

### Cited Findings

**Data model and update**
- Zep's core is Graphiti, a "temporally aware knowledge graph engine"; the paper compares against MemGPT on DMR (Zep 94.8% vs 93.4%) and reports LongMemEval gains — [Zep paper abstract](https://arxiv.org/abs/2501.13956)
- Input unit is an "episode" (message, text or JSON). For messages the LLM sees the current message plus the previous n=4 messages; the speaker is always extracted as an entity; a Reflexion-style reflection step is applied to cut hallucinated or missed entities — [Zep paper](https://arxiv.org/html/2501.13956v1)
- Entity resolution: candidates by cosine similarity over entity nodes and full-text search on names/summaries, then an LLM resolution prompt with the episode context; on a duplicate the LLM outputs an updated name and summary. Entity names are embedded (1024-dim, BGE-m3) — [Zep paper](https://arxiv.org/html/2501.13956v1)
- Fact (edge) extraction: LLM extracts facts between listed entities with an all-caps relation type (e.g. WORKS_FOR); edge dedup is limited to edges between the same entity pair; graph writes use predefined Cypher, not LLM-generated queries — [Zep paper](https://arxiv.org/html/2501.13956v1)
- Four timestamps on every edge: t_valid / t_invalid (event time, "chronological") and t'_created / t'_expired (transaction/ingestion time). Relative dates ("last Tuesday") are resolved against the message's reference time t_ref — [Zep paper](https://arxiv.org/html/2501.13956v1)
- Edge invalidation: an LLM compares a new edge with semantically related existing edges; if one contradicts it and overlaps in time, the old edge's t_invalid is set to the new edge's t_valid; on the transaction timeline new information wins. Old edges are marked, not deleted — [Zep paper](https://arxiv.org/html/2501.13956v1); implementation lives in the edge-resolution stage of `add_episode()` and `utils/maintenance/temporal_operations.py` (`invalid_at`, `expired_at`) per a third-party code read — [Graphiti technical report (third party)](https://lin-guanguo.github.io/llm-memory-research/graphiti.research/)
- Community nodes: built with label propagation (chosen over Leiden because it extends incrementally). A new entity joins the community held by the plurality of its neighbours and the community summary is updated; the paper says periodic full refreshes are still needed because incremental communities drift — [Zep paper](https://arxiv.org/html/2501.13956v1)
- Provenance: each episode is stored as a node; entities link to it through `MENTIONS` edges, so facts can be traced to their source and "what was known at a given time" can be queried — [Graphiti docs: adding episodes](https://help.getzep.com/graphiti/graphiti/adding-episodes). The Zep paper lists episode-to-edge provenance links as not evaluated — [Zep paper](https://arxiv.org/html/2501.13956v1)
- Episodes carry `source_description` (free text about where content came from), `reference_time`, and a type (text, message, json); `add_episode_bulk` exists for large imports — [Graphiti docs: adding episodes](https://help.getzep.com/graphiti/graphiti/adding-episodes). A search-result summary of Zep docs says the bulk path does NOT perform edge invalidation, while the docs page I fetched says the bulk path resolves invalidated edges; these two statements conflict and I did not resolve them — [docs page](https://help.getzep.com/graphiti/graphiti/adding-episodes) vs search summary of [Zep core-concepts page](https://help.getzep.com/graphiti/core-concepts/adding-episodes.md)
- Custom ontology: entity and edge types can be defined as Pydantic models ("prescribed") or left to emerge ("learned"); prompts can be overridden per client. `group_id` partitions data; the reference servers have no authentication across groups — [Graphiti README](https://github.com/getzep/graphiti), [PyPI graphiti-core](https://pypi.org/project/graphiti-core/)

**Forgetting**
- There is no deletion-by-decay. Facts are invalidated, never forgotten; the paper's own "forgetting" is invalidation on contradiction plus periodic community refresh — [Zep paper](https://arxiv.org/html/2501.13956v1). I found no TTL, salience decay or pruning mechanism in the sources I read (see Gaps).

**Retrieval**
- Three search functions: cosine similarity (facts for edges, entity names for nodes, community names), Okapi BM25 (Neo4j Lucene), and breadth-first search from seed nodes (e.g. recent episodes) — [Zep paper](https://arxiv.org/html/2501.13956v1)
- Rerankers: RRF, MMR, episode-mentions, node-distance, cross-encoder (best accuracy, highest cost). The paper does not say which reranker the experiments used — [Zep paper](https://arxiv.org/html/2501.13956v1)
- Default recipe `COMBINED_HYBRID_SEARCH_RRF`: BM25 + cosine over edges, nodes, communities with RRF; `DEFAULT_MIN_SCORE` 0.6, `MAX_SEARCH_DEPTH` 3, `DEFAULT_SEARCH_LIMIT` 10 — [Graphiti technical report (third party)](https://lin-guanguo.github.io/llm-memory-research/graphiti.research/)

**Models and cost (write path)**
- Paper: graph construction with gpt-4o-mini-2024-07-18; answering with gpt-4o-mini or gpt-4o-2024-11-20; embeddings and reranking with BGE-m3 — [Zep paper](https://arxiv.org/html/2501.13956v1)
- The paper lists five LLM prompt types in its appendix (entity extraction, entity resolution, fact extraction, fact resolution, temporal extraction) but does NOT state the number of LLM calls per episode — [Zep paper](https://arxiv.org/html/2501.13956v1). No source I found gives measured ingestion seconds or tokens per episode ([search for Graphiti ingestion timing](https://lin-guanguo.github.io/llm-memory-research/graphiti.research/) returned only qualitative statements).
- Ingestion concurrency: `SEMAPHORE_LIMIT` defaults to 10 concurrent operations (README) to avoid HTTP 429; "if Graphiti feels slow, raise it" — [Graphiti README](https://github.com/getzep/graphiti). A third-party performance page claims 6-10 concurrent LLM requests per slot and a default of 20; I could not confirm this against source — [mintlify mirror, as summarised by search](https://mintlify.com/getzep/graphiti/advanced/performance-tuning)
- Memory size as measured by a competitor: Mem0's paper says Zep's graph for one LoCoMo conversation exceeded 600k tokens (summary cached at every node, facts on edges) versus ~26k raw conversation tokens and ~7k (Mem0) / ~14k (Mem0g). Vendor (competitor) claim — [Mem0 paper](https://arxiv.org/html/2504.19413)
- Mem0's authors observed that querying Zep immediately after ingestion often failed and that re-running the same searches hours later gave noticeably better results, which they attribute to asynchronous background LLM processing in Zep's graph build. Competitor observation; Zep's blog does not address it in the extract I read — [Mem0 paper](https://arxiv.org/html/2504.19413)

**Read-path latency and accuracy claims**
- Paper (LongMemEval_S): Zep+gpt-4o 71.2% vs 60.2% full-context, 2.58 s vs 28.9 s end-to-end, 1.6k vs 115k context tokens; Zep+gpt-4o-mini 63.8% vs 55.4%, 3.20 s vs 31.3 s. The "18.5%" headline is a relative gain (~11.0 absolute points). Zep was tested over a residential link to AWS us-west-2, which adds latency the baselines lacked — [Zep paper](https://arxiv.org/html/2501.13956v1)
- Per question type in the paper (gpt-4o-mini, Zep vs full-context): temporal reasoning 54.1 vs 36.5; single-session-preference 53.3 vs 30.0; multi-session 47.4 vs 40.6; but knowledge-update 74.4 vs 76.9 and single-session-assistant 75.0 vs 81.8 (Zep worse). With gpt-4o: knowledge-update 83.3 vs 78.2; single-session-assistant 80.4 vs 94.6 (worse). The gpt-4o single-session-user row repeats the gpt-4o-mini numbers exactly, probably a transcription error — [Zep paper](https://arxiv.org/html/2501.13956v1)
- Authors state "less capable models did not fully understand Zep's temporal data" and that the evaluation used only part of Graphiti's search features — [Zep paper](https://arxiv.org/html/2501.13956v1)
- DMR (small, ~60 messages per conversation): Zep 98.2% (gpt-4o-mini) vs full-conversation 98.0%; the authors themselves call DMR weak — [Zep paper](https://arxiv.org/html/2501.13956v1)
- Zep's 2026 research page (vendor, `dateModified` 2026-09-29): LoCoMo 94.7% (1,459/1,540), p95 retrieval 155 ms, median context 5,760 tokens; LongMemEval 90.2% (451/500), p95 162 ms, 4,408 tokens. Reader and judge are both gpt-5.4 with medium reasoning; multi-scope retrieval = 20 edges, 10 nodes, 10 episodes, 5 thread summaries, 5 observations with cross-encoder reranking, run as five parallel searches on the client. A single "auto search" call scores 86.5% on LoCoMo with 2,680 tokens. The page does not mention the 2025 paper or Graphiti — [Zep research page](https://getzep.com/research)
- Latency numbers on the research page are retrieval only; they are not end-to-end answer latency (answer model is a reasoning model) — [Zep research page](https://getzep.com/research)

**Maturity**
- Apache-2.0, 32.5k GitHub stars, 3.3k forks (as fetched 2026-11-04); `graphiti-core` 0.31.2 released 2026-10-20, Python ≥3.10; still 0.x — [GitHub](https://github.com/getzep/graphiti), [PyPI](https://pypi.org/project/graphiti-core/)
- Backends: Neo4j 5.26, FalkorDB 1.1.2, Amazon Neptune (+OpenSearch Serverless), Kuzu 0.11.2 (deprecated, to be removed). LLM defaults to OpenAI; extras for Anthropic, Groq, Gemini; OpenAI-compatible endpoints (DeepSeek, Together, OpenRouter, Ollama, vLLM, llama.cpp, LM Studio) — [GitHub](https://github.com/getzep/graphiti)
- Zep Community Edition is deprecated; Zep points users to Zep Cloud or Graphiti. Zep (managed or BYOC) adds users, threads, ingestion, dashboard; Graphiti is bring-your-own-database and you build ingestion/tenancy yourself — [Zep CE deprecation notice](https://help.getzep.com/ce-deprecation-notice), [Graphiti README](https://github.com/getzep/graphiti)
- An MCP server exists; the README warns that reference servers have no authentication and that any caller can read/change/delete data in any `group_id` — [Graphiti README via PyPI](https://pypi.org/project/graphiti-core/)

### Inferences
- Because retrieval is embeddings + BM25 + graph without an LLM (unless a cross-encoder/LLM reranker is chosen), two different models (a small local one and a large hosted one) can both read the same Graphiti store cheaply; the write path is where model quality and latency matter.
- Graphiti has no built-in notion of "belief vs truth" or "who asserted it". The nearest features are the speaker-as-entity rule, `source_description`, `group_id` (one graph per NPC), custom edge types, and episode provenance via `MENTIONS`. Modelling "NPC A believes X because B told them" would have to be done in the ontology (e.g. a TOLD/CLAIMS edge from a speaker to a claim node), with `reference_time` set to game time.
- The paper's own table shows invalidation does not guarantee better knowledge-update accuracy (knowledge-update was slightly worse than full context with gpt-4o-mini), so invalidation quality is model-dependent.

### Gaps
- No measured per-episode ingestion latency, token count or LLM-call count for Graphiti from any source (first-party or independent).
- Forgetting beyond invalidation (decay, pruning, salience): not found.
- Whether the bulk path invalidates edges: sources conflict (see above).
- No independent same-harness re-run of Zep/Graphiti with the 2026 settings; the only "independent" re-measurements found are 58.44% (Mem0's re-run), 59.22% (MemOS paper), 75.14% (Zep's corrected 2025 number), 79.09% (MIRIX paper) — see sections 2 and 5.

---

## 2. Mem0 and Mem0g; the Mem0-vs-Zep dispute

### Takeaway
Mem0 (68.4k stars, Apache-2.0) is the most used agent-memory library. Its 2025 paper design (LLM extract, then LLM chooses ADD/UPDATE/DELETE/NOOP per fact; Mem0g adds a Neo4j graph with invalidation) was replaced in April 2026 by an ADD-only single-LLM-call algorithm with entity-linking, and graph memory was removed from the open-source SDK (Platform-only). Scores for the old and new versions come only from Mem0 itself and are inconsistent across Mem0's own pages; the Mem0-Zep LoCoMo dispute shows how one system moved between 58% and 84% by method alone.

### Cited Findings

**2025 design (paper, arXiv 2504.19413)**
- Extraction: an LLM sees the newest message pair plus an asynchronously refreshed conversation summary and the last m=10 messages and emits candidate facts. Update: for each candidate, top s=10 similar memories are retrieved and an LLM (tool call) chooses ADD, UPDATE, DELETE or NOOP. LLM: GPT-4o-mini — [Mem0 paper](https://arxiv.org/html/2504.19413)
- Mem0g: directed labelled graph in Neo4j; LLM entity extractor, LLM relationship (triplet) generator, embedding match to existing nodes above threshold t; conflict detector flags conflicting edges; an LLM update resolver marks obsolete relations invalid rather than deleting them; retrieval by entity-centric subgraph and semantic triplet matching — [Mem0 paper](https://arxiv.org/html/2504.19413)
- Headline LoCoMo overall J: Mem0 66.88%, Mem0g 68.44%, Zep 65.99% (as run by Mem0), A-Mem 48.38%, LangMem 58.10%, OpenAI memory 52.90%, best RAG 60.97%, full-context 72.90% — [Mem0 paper](https://arxiv.org/html/2504.19413). Mem0's abstract frames this as "26% relative" over OpenAI; the same table shows full-context beating every memory system on accuracy — [Mem0 paper abstract](https://arxiv.org/abs/2504.19413)
- Latency (p50/p95, seconds): search 0.148/0.200 (Mem0), 0.476/0.657 (Mem0g), 0.513/0.778 (Zep), 0.668/1.485 (A-Mem), 17.99/59.82 (LangMem); total response 0.708/1.440 (Mem0), 1.091/2.590 (Mem0g), 1.292/2.926 (Zep), vs 9.870/17.117 full-context. Context tokens per query: Mem0 1,764; Mem0g 3,616; Zep 3,911; full-context 26,031 — [Mem0 paper](https://arxiv.org/html/2504.19413)
- Per-category J (single-hop / multi-hop / open-domain / temporal): Mem0 67.13 / 51.15 / 72.93 / 55.51; Mem0g 65.71 / 47.19 / 75.71 / 58.13; Zep 61.70 / 41.35 / 76.60 / 49.31 — [Mem0 paper](https://arxiv.org/html/2504.19413)

**2026 redesign ("token-efficient memory algorithm", OSS v3 / PyPI 2.x)**
- Single-pass, ADD-only extraction: one LLM call, no UPDATE/DELETE (previous algorithm used two LLM calls). When facts change, "the new fact lives alongside the old one, and both survive". Agent-generated facts are stored with the same weight as user facts. Pipeline: retrieve top-10 related memories for dedup context, one LLM call, batch embed, MD5 hash dedup, batch insert, entity extraction (spaCy via `[nlp]` extra, no LLM) — [Mem0 blog](https://mem0.ai/blog/mem0-the-token-efficient-memory-algorithm), [Mem0 OSS v2-to-v3 migration guide](https://docs.mem0.ai/migration/oss-v2-to-v3)
- Retrieval: three signals fused (semantic vectors, BM25 keyword, entity matching); BM25 is a boost, not a recall expander; defaults changed to `top_k` 20, `threshold` 0.1, `rerank` False — [Mem0 OSS migration guide](https://docs.mem0.ai/migration/oss-v2-to-v3)
- Graph memory removed from the open-source SDK (about 4,000 lines: `enable_graph`, `graph_store`, Neo4j/Memgraph/Kuzu/AGE/Neptune drivers; `relations` no longer returned); it is now Platform-only and not replaced by an OSS equivalent — [Mem0 OSS migration guide](https://docs.mem0.ai/migration/oss-v2-to-v3)
- Vendor numbers (old to new): LoCoMo 71.4 to 92.5 (blog/README; launch version said 91.6), LongMemEval 67.8 to 94.4 (launch said 93.4), BEAM 1M 64.1, BEAM 10M 48.6; mean retrieval ~6,956 tokens (LoCoMo) and ~6,787 (LongMemEval); single-pass, top-200 budget; ±1 point judge noise; p50 retrieval latency ~0.88-1.09 s on the managed platform (README); README says scores include platform-only optimizations — [Mem0 blog](https://mem0.ai/blog/mem0-the-token-efficient-memory-algorithm), [Mem0 GitHub](https://github.com/mem0ai/mem0). The blog page's metadata is inconsistent (header date Apr 16, 2026 vs published date Nov 2, 2026) and it does not name the answer or judge model — [Mem0 blog](https://mem0.ai/blog/mem0-the-token-efficient-memory-algorithm)
- New-algorithm BEAM per-category (1M / 10M): contradiction resolution 35.7 / 32.5, event ordering 53.6 / 20.2, temporal reasoning 61.8 / 16.3, knowledge update 65.0 / 75.0, multi-session reasoning 65.2 / 26.1. Contradiction resolution is the weakest category at both scales (vendor-reported) — [Mem0 blog](https://mem0.ai/blog/mem0-the-token-efficient-memory-algorithm)
- Repo: 68.4k stars, 8.1k forks, Apache-2.0; default LLM gpt-5-mini and embedding text-embedding-3-small; self-hosted Docker server and pip library; a `memory-benchmarks` repo with LoCoMo/LongMemEval harness — [Mem0 GitHub](https://github.com/mem0ai/mem0), [memory-benchmarks](https://github.com/mem0ai/memory-benchmarks)
- Mem0 in MemoryAgentBench (older version) scored 18.0 on single-hop and 2.0 on multi-hop conflict resolution (FactConsolidation) — [MemoryAgentBench](https://arxiv.org/html/2507.05257v2)

**The Mem0-Zep dispute (chronology)**
- Mem0's paper reports Zep at 65.99% J on LoCoMo (below Mem0 66.88 and Mem0g 68.44) and Zep search p95 0.778 s — [Mem0 paper](https://arxiv.org/html/2504.19413)
- Zep's rebuttal ("Lies, Damn Lies, & Statistics"): Zep corrected scores 75.14% ± 0.17 (an earlier version of the post had a miscalculation), about 10% above Mem0g's 68.44%; p95 search 0.632 s for a correct implementation vs 0.778 s as Mem0 measured. Alleged Mem0 mistakes: the user graph was built with the user role assigned to both speakers; timestamps were appended to message text instead of passed through `created_at`; searches were run sequentially instead of concurrently. Both sides exclude category 5 (no ground truth). Full-context GPT-4o-mini scored 72.90%. The post's meta description says 24% where the body says about 10% — [Zep blog](https://www.getzep.com/blog/lies-damn-lies-statistics-is-mem0-really-sota-in-agent-memory/)
- Mem0's reply on Zep's repo (getzep/zep-papers issue titled "Revisiting Zep's 84% LoCoMo Claim: Corrected Evaluation & 58.44% Accuracy"): Zep's number had counted the adversarial category 5 in the numerator but not the denominator; Mem0 re-measured Zep at 58.44%. I did not open the issue; this is from secondary summaries — [secondary summary](https://braindetox.kr/en/posts/memory_benchmark_scores_reliability_2026.html), [search summary referencing issue #5](https://reddit.sentinel-team.org/posts/1s1jb94/snapshots/2026-03-27T22%3A19%3A49.793866Z)
- Zep scores on LoCoMo by who measured (secondary compilation): 58.44 (Mem0 re-run), 59.22 (MemOS paper), 65.99 (Mem0 paper), 75.14 (Zep 2025, 10-run average, corrected from ~84), 79.09 (MIRIX), 94.7 (Zep 2026, gpt-5.4 reader and judge) — [braindetox](https://braindetox.kr/en/posts/memory_benchmark_scores_reliability_2026.html)
- Mem0's own LoCoMo numbers: 66.88 (paper), 71.4 (2026 blog, "old algorithm"), 92.5 (2026, reported with gpt-5 answering/grading under a newly written grading prompt); other companies' runs of Mem0 range 57.24-64.57 — [braindetox](https://braindetox.kr/en/posts/memory_benchmark_scores_reliability_2026.html) (secondary; the gpt-5 detail is not on Mem0's own page)
- Nemori's independent-ish run (gpt-4.1-mini, LLM-judge, Mem0 and Zep through commercial APIs): Mem0 66.3, Zep 61.6, LangMem 73.4, full-context 80.6 — [Nemori paper](https://arxiv.org/html/2508.03341)
- A competitor blog (Evermind, which sells a rival product) reports an independent reimplementation measuring Mem0 at 29.3% on LoCoMo and 49.0% on LongMemEval; I could not verify the original and treat it as unreliable — [Evermind blog (competitor), via search](https://evermind.ai/blogs/top-ai-memory-systems-benchmarked-in-2026)

### Inferences
- Mem0's move to ADD-only is an admission that LLM-driven UPDATE/DELETE was lossy or expensive; for an NPC memory that must hold both a truth and a lie, ADD-only plus provenance in the fact text is structurally the simplest way to keep contradictory statements, but the vendor's own BEAM contradiction-resolution number (35.7/32.5) shows retrieval alone does not resolve them.
- Mem0's open-source graph route (Mem0g) is gone; anyone wanting a graph with Mem0 must use the hosted platform. For a self-hosted, game-embedded NPC memory, Graphiti is the remaining open-source temporal-graph option among the two famous names.
- The 26-point or 36-point swings between sources (section 5) make any "X beats Y on LoCoMo" statement unsafe unless both ran under one harness.

### Gaps
- Mem0's own current answer and judge models for the 92.5/94.4 numbers: not on the vendor page.
- No independent verification of the 92.5/94.4 numbers found (only a competitor's blog, flagged above).
- I did not open the getzep/zep-papers issue itself.
- Mem0 v3 write latency in absolute seconds: only "roughly in half" is stated.

---

## 3. HippoRAG / HippoRAG 2, GraphRAG, LightRAG, Cognee, KAG: incremental per-turn updates vs batch indexing

### Takeaway
These are document-RAG graph systems, not conversational memories. None has a bi-temporal model or contradiction invalidation. HippoRAG 2 and LightRAG can add passages incrementally in principle (additive graphs), but their evidence is for corpus-style QA; GraphRAG is batch-first with a delta `update` command; KAG is a domain-specific heavy framework. Of the group only Cognee positions itself as an agent memory, with self-published and unmatched benchmarks.

### Cited Findings

**HippoRAG 2 (arXiv 2502.14802)**
- Offline: LLM OpenIE builds triples from passages; synonym edges join phrases whose embedding similarity exceeds 0.8; passage nodes link to their phrases. Online: query matched to triples (NV-Embed-v2), an LLM filters the top-5 triples ("recognition memory"), up to 5 phrase nodes and all passage nodes seed Personalised PageRank (damping 0.5, passage weight 0.05) — [HippoRAG 2 paper](https://arxiv.org/html/2502.14802)
- So there is an LLM call on every read (the triple filter) in addition to the LLM OpenIE on every write — [HippoRAG 2 paper](https://arxiv.org/html/2502.14802)
- QA F1 with a Llama-3.3-70B reader, averaged over NQ, PopQA, MuSiQue, 2Wiki, HotpotQA, LV-Eval, NarrativeQA: HippoRAG 2 59.8, NV-Embed-v2 57.0, HippoRAG 53.1, GraphRAG 49.6, RAPTOR 48.8, LightRAG 6.6. HippoRAG 2's gain over a plain embedding retriever is modest (+2.8 F1) and concentrated in multi-hop sets (2Wiki 71.0 vs 61.5; MuSiQue 48.6 vs 45.7). With a GPT-4o-mini reader the averages are 58.1 vs 55.7. The authors ran the baselines — [HippoRAG 2 paper](https://arxiv.org/html/2502.14802)
- Cost on the MuSiQue corpus (11,656 passages): HippoRAG 2 9.2M indexing input tokens, 99.5 min indexing, 1.2 s per query, 9.9 GB GPU memory; GraphRAG 115.5M tokens, 277 min, 10.7 s per query; LightRAG 68.5M tokens, 235 min, 13.3 s per query — [HippoRAG 2 paper](https://arxiv.org/html/2502.14802). Another statement in the paper: about 1.1 s per passage with Llama-3.3-70B, and under $2 with the gpt-4o-mini batch API (24-hour completion), per a search result quoting the paper — [search snippet of paper](https://arxiv.org/pdf/2502.14802)
- Incremental updates: the paper simulates a growing corpus by adding NQ/MuSiQue segments and says the gain over NV-Embed-v2 stays consistent, but I found no described incremental-indexing procedure or exact numbers — [HippoRAG 2 paper](https://arxiv.org/html/2502.14802)
- No results with small LLMs are reported — [HippoRAG 2 paper](https://arxiv.org/html/2502.14802)
- In MemoryAgentBench, HippoRAG-v2 scores 54.0 (single-hop) and 5.0 (multi-hop) on conflict resolution, similar to BM25 (48.0 / 3.0) — [MemoryAgentBench](https://arxiv.org/html/2507.05257v2)

**GraphRAG (Microsoft)**
- Early versions required full re-indexing on new data; GraphRAG 1.0 added an `update` command that computes deltas and merges, but adding content can still change community structure so much of an index may need recomputation — [Microsoft Research blog](https://www.microsoft.com/en-us/research/blog/moving-to-graphrag-1-0-streamlining-ergonomics-for-developers-and-users/). A third-party review says `update` handles only new documents and removing or revising a document still needs a full re-index (secondary, unverified) — [search summary](https://jacar.es/en/microsofts-graphrag-in-enterprise-patterns-that-work/)
- LazyGraphRAG defers graph construction; secondary sources put its indexing cost at about 0.1% of full GraphRAG (not in the Microsoft post excerpt I saw) — [Microsoft Research blog](https://www.microsoft.com/en-us/research/blog/moving-to-graphrag-1-0-streamlining-ergonomics-for-developers-and-users/), [secondary cost note](https://medium.com/graph-praxis/the-graphrag-cost-cliff-how-33-000-became-33-in-eighteen-months-be1b0fbe37e4)

**LightRAG**
- New documents go through the same graph indexing and are merged into the existing graph by a union operation (incremental update algorithm); paper contrasts this with GraphRAG's community rebuild — [LightRAG paper (alphaXiv)](https://alphaxiv.org/abs/2410.05779v3). A November 2025 preprint says these frameworks support only incremental additions, so edits or deletions require a full rebuild, and proposes Jigsaw-LightRAG — [Jigsaw-LightRAG preprint](https://www.preprints.org/manuscript/202511.1058)
- Its F1 in HippoRAG 2's table is abnormally low (avg 6.6; NQ 16.6, PopQA 2.4), probably a harness or configuration mismatch from HippoRAG 2's authors rather than the algorithm's true level — [HippoRAG 2 paper](https://arxiv.org/html/2502.14802) (flagged as my reading)

**Cognee**
- Graph + vector memory pipeline (`cognify`); 14 retrieval modes, default GRAPH_COMPLETION. Self-published January 2026 benchmark on a 24-question HotPotQA subset (45 runs per system): Cognee tuned (GRAPH_COMPLETION_COT) 0.85 DeepEval correctness vs Mem0 0.54, with Cognee tuned and the others on defaults; Cognee itself says it is not an independent study — [Cognee benchmarks (vendor)](https://www.cognee.ai/knowledge-graph-memory-benchmarks)
- Cognee's earlier arXiv paper: untuned 0.476 DeepEval correctness on HotPotQA vs 0.815 optimised — [Cognee paper](https://arxiv.org/html/2505.24478v1)
- Cognee on BEAM (vendor claim via third-party ranking): 0.79 at 100K and 0.67 at 10M tokens; no independent verification found — [secondary ranking](https://evermind.ai/blogs/top-ai-memory-systems-benchmarked-in-2026)
- Cognee was included in MemoryAgentBench (with chunk size 4096 "for computational overhead and API cost"), but I did not retrieve its per-task numbers — [MemoryAgentBench](https://arxiv.org/html/2507.05257v2)

**KAG (Ant Group / OpenSPG)**
- Knowledge-graph + vector framework for professional-domain QA, using logical-form-guided hybrid reasoning; builder and solver released, KAG-Model planned; authors report relative F1 gains of 19.6% (2Wiki) and 33.5% (HotpotQA) over RAG baselines — [KAG paper](https://arxiv.org/html/2409.13731v2), [KAG repo](https://github.com/OpenSPG/KAG)

### Inferences
- Per-turn incremental write: Graphiti and Mem0 are built for it; LightRAG and (by design) HippoRAG 2 can append; GraphRAG's community machinery and KAG's schema-constrained build are batch-oriented. HippoRAG 2 also needs an LLM call at every read, which clashes with a fast local-model reply path.
- None of these has a bi-temporal edge, so "the NPC used to believe X, now believes Y" is not natively representable in HippoRAG/GraphRAG/LightRAG/KAG.

### Gaps
- HippoRAG (original, arXiv 2405.14831) paper not read; only its numbers inside HippoRAG 2's tables.
- Cognee's `cognify` incremental behaviour and per-write cost: not found in sources.
- KAG's incremental update behaviour and cost: not found.
- No source measured any of these on dialogue-style memory.

---

## 4. A-MEM and the 2025-2026 newcomers (MemOS, MIRIX, Memory-R1, MemoryOS, LightMem, Nemori, G-Memory, Hindsight, Engram, REAL, Mastra OM, Supermemory)

### Takeaway
2025-2026 brought three trends: (1) cheaper writes through batching and offline "sleep-time" consolidation (LightMem, Nemori); (2) RL- or distilled-small-model memory managers (Memory-R1, MemReader, MemOperator); (3) explicit belief or provenance models (Hindsight opinion network with confidence, Engram supersession chains, REAL confidence-aware validity intervals, MemOS MemCube provenance metadata). Nearly all numbers are self-reported on LoCoMo or LongMemEval under different judges; a no-graph, text-in-context "observational memory" reaches 94.87% on LongMemEval (Mastra) which questions how much graph structure benchmarks reward.

### Cited Findings

**A-MEM (arXiv 2502.12110, Zettelkasten-style notes)**
- Each note stores content, timestamp, LLM-generated keywords, tags, contextual description, embedding (all-MiniLM-L6-v2) and links. A new note is compared with existing notes by cosine similarity; the top-k go to an LLM that decides links; then an LLM decides whether each neighbour's context, keywords and tags should be updated ("memory evolution") — [A-MEM paper](https://arxiv.org/html/2502.12110)
- The paper does not state the number of LLM calls per write; the equations imply at least construction + link generation + evolution — [A-MEM paper](https://arxiv.org/html/2502.12110)
- Small local models were tried (Qwen2.5 1.5B/3B, Llama 3.2 1B/3B via Ollama with LiteLLM structured output): LoCoMo F1 on e.g. Llama 3.2 3B: multi-hop 17.44, temporal 26.38, open-domain 12.53, single-hop 28.14; avg ~1,126-1,376 tokens per query vs 16,910 for the LoCoMo baseline; reported cost <$0.0003 per operation and 5.4 s processing with GPT-4o-mini vs 1.1 s with Llama 3.2 1B on a single GPU. Authors note organisation quality depends on the LLM and report no error bars — [A-MEM paper](https://arxiv.org/html/2502.12110)
- A-Mem re-run by the Mem0 authors scored 48.38% J overall (their A-Mem* re-run lines are far below A-Mem's published F1) — [Mem0 paper](https://arxiv.org/html/2504.19413)

**Memory-R1 (arXiv 2508.19828)**
- Two RL-trained agents (PPO or GRPO): a Memory Manager choosing ADD/UPDATE/DELETE/NOOP and an Answer Agent that selects and reasons over memories. Trained on 152 QA pairs (split 152/81/1307) mostly on 4×H100; reward = exact-match correctness of a frozen answer agent; zero-shot to MSC and LongMemEval — [Memory-R1 paper](https://arxiv.org/html/2508.19828)
- LoCoMo overall F1/B1/J: Memory-R1-GRPO with LLaMA-3.1-8B 45.02/37.51/62.74 vs Mem0 30.41/22.22/45.68 and MemoryOS 35.04/27.99/48.20; with Qwen-2.5-7B 43.14/36.44/61.51 vs Mem0 30.61/23.55/53.30. Manager p50 latency about 2 s on LLaMA-3.1-8B; memory search median under 0.35 s — [Memory-R1 paper](https://arxiv.org/html/2508.19828)

**MemoryOS (arXiv 2506.06326)**
- Three tiers (short-term, mid-term, long-term personal memory); short-to-mid by dialogue-chain FIFO, mid-to-long by segmented-page strategy. Claims +49.11% F1 and +46.18% BLEU-1 average over baselines with GPT-4o-mini on LoCoMo (relative, vendor) — [MemoryOS abstract](https://arxiv.org/abs/2506.06326)

**MIRIX (arXiv 2507.07957)**
- Six memory types (Core, Episodic, Semantic, Procedural, Resource, Knowledge Vault) coordinated by a multi-agent framework; reports 85.4% LoCoMo and 35% higher accuracy than a RAG baseline on its ScreenshotVQA with 99.9% less storage (self-reported); MIRIX also appears in MemoryAgentBench — [MIRIX abstract](https://arxiv.org/abs/2507.07957), [MemoryAgentBench](https://arxiv.org/html/2507.05257v2). Another evaluation (MemOS's) lists MIRIX much lower, and braindetox lists 79.09 — [MemOS eval repo via search](https://huggingface.co/datasets/MemTensor/MemOS_eval_result/blob/main/README.md), [braindetox](https://braindetox.kr/en/posts/memory_benchmark_scores_reliability_2026.html)

**MemOS (arXiv 2507.03724)**
- "Memory operating system" with a MemCube unit that holds content plus metadata such as provenance and versioning; unifies plaintext, activation and parametric memory. MemTensor's eval repo reports 75.80 on LoCoMo for MemOS vs 52.75 for GPT-4o-mini (self-reported). A third-party Q1-2026 summary notes the backend uses Neo4j, Qdrant, Redis and MySQL and that parametric (LoRA) memory is a stub — [MemOS abstract](https://arxiv.org/abs/2507.03724), [MemOS eval via search](https://huggingface.co/datasets/MemTensor/MemOS_eval_result/blob/main/README.md), [2026 Q1 summary](https://lin-guanguo.github.io/llm-memory-research/memory.26Q1.summary/)

**LightMem (arXiv 2510.18866)**
- Atkinson-Shiffrin-style: sensory memory (light compression and topic grouping), topic-aware short-term consolidation, long-term memory with offline "sleep-time" update. With GPT and Qwen backbones: accuracy up to +7.7% (LongMemEval) / +29.3% (LoCoMo) over baselines; total tokens up to 38× / 20.9× fewer; API calls up to 30× / 55.5× fewer; online test-time tokens up to 106× / 117× fewer and calls 159× / 310× fewer — [LightMem abstract](https://arxiv.org/abs/2510.18866) (self-reported)

**Nemori (arXiv 2508.03341)**
- Buffers messages and cuts episodes at a 20-message window; each episode becomes a narrative; each new episode is compared with its 5 nearest episodes (merge or insert); "predict-calibrate" semantic distillation: the LLM predicts the episode from existing knowledge and only the prediction error is stored; each insight is classed new/merge/conflict. Estimated about 4 LLM calls per episode plus one per insight (my reading of the paper's pipeline; the paper gives no per-write count) — [Nemori paper](https://arxiv.org/html/2508.03341)
- LoCoMo LLM-judge overall (gpt-4.1-mini): Nemori 80.8, full-context 80.6, LangMem 73.4, Mem0 66.3, Zep 61.6; (gpt-4o-mini): Nemori 73.0, full-context 72.3, Mem0 61.3, Zep 58.5, LangMem 51.3. Mem0 and Zep ran through commercial APIs; judge gpt-4o-mini. Ingestion tokens (gpt-4o-mini): Nemori about 322.9K vs 526.5K (MemoryOS) to 1,693.4K (Mem0) (unit, per conversation or per dataset, is unclear in the extract); retrieval 2,745 tokens/question vs 23,653 full-context; end-to-end latency about 3.05 s vs Zep 3.26 s, Mem0 3.54 s, LangMem 22.1 s. Authors: LoCoMo rarely requires knowledge updates, so consolidation has little measured effect there — [Nemori paper](https://arxiv.org/html/2508.03341)

**G-Memory (arXiv 2506.07398)**
- Hierarchical memory (insight, query, interaction graphs) for multi-agent systems; up to +20.89% success rate on embodied action and +10.12% accuracy on knowledge QA across five benchmarks. It targets agent task trajectories, not conversation facts — [G-Memory abstract](https://arxiv.org/abs/2506.07398)

**Hindsight (arXiv 2512.12818, Vectorize; MIT licence; ACL 2026 system demo)**
- Four memory networks: world facts, experience (first-person), opinions (subjective beliefs with a confidence in [0,1]) and observations (entity summaries). Retain: an open LLM (GPT-OSS-20B or 120B) extracts 2-5 narrative facts per conversation (coreference, temporal normalisation, participant attribution, network classification); entity resolution by a weighted score of string similarity, co-occurrence and temporal proximity; each memory stores an occurrence interval and a mention time — [Hindsight paper](https://arxiv.org/html/2512.12818v1)
- Opinions: when new facts arrive an LLM labels related opinions reinforce / weaken / contradict / neutral; confidence moves by step α (contradiction 2α); text can be revised on strong contradiction. The paper describes no separate speaker-vs-belief attribution scheme beyond participant attribution — [Hindsight paper](https://arxiv.org/html/2512.12818v1)
- Recall: four parallel channels (vector, BM25, graph spreading activation, temporal), RRF (k=60), cross-encoder rerank, token-budget packing — [Hindsight paper](https://arxiv.org/html/2512.12818v1)
- Self-reported: LongMemEval 83.6% (OSS-20B), 89.0% (OSS-120B), 91.4% (Gemini-3) vs full-context OSS-20B 39.0% and GPT-4o 60.2%; LoCoMo 83.18 / 85.67 / 89.61. Judge is GPT-OSS-120B at temperature 0, chosen by the authors; LongMemEval baselines (Zep 71.2, Supermemory) were copied from Supermemory's report and LoCoMo baselines from Backboard's published results, none re-run; latency not reported; a token-budget section contains unfilled placeholders — [Hindsight paper](https://arxiv.org/html/2512.12818v1)

**Engram (arXiv 2606.09900, June 2026)**
- Open-source (github.com/ly-wang19/engram) bi-temporal graph memory: an asynchronous write path extracts atomic (subject, predicate, object) facts; contradictions are resolved without an LLM call per fact; facts are invalidated not deleted, "every fact keeps provenance and a supersession chain"; read path fuses dense, lexical, graph and recency/salience signals with an as-of (point-in-time) filter and returns provenance-tagged context. LongMemEval_S: 83.6% with ~9.6k retrieved tokens vs 73.2% full-context at ~79k tokens (official judge, 500 questions). Single independent author — [Engram abstract](https://arxiv.org/abs/2606.09900)

**REAL (arXiv 2606.10694, June 2026)**
- Temporal and confidence-aware directed property graph: facts have valid-time intervals and confidence scores; updates are non-destructive (parallel fact versions with validity intervals); retrieval uses hybrid beam search over query roots. Abstract claims +22.72% on average over flat-text, graph and memory baselines; benchmarks, models and call counts not stated on the abstract page; no code link — [REAL abstract](https://arxiv.org/abs/2606.10694)

**Others worth knowing**
- Mastra "Observational Memory": plain-text observations kept in context (no vector or graph store), Observer and Reflector agents triggered by token thresholds; 94.87% on LongMemEval with gpt-5-mini, the strongest non-oracle number on a 2026 Q1 survey page; Mastra's number is a simple average of six type scores, counting all 500 questions equally gives 93.6% — [2026 Q1 summary](https://lin-guanguo.github.io/llm-memory-research/memory.26Q1.summary/), [braindetox](https://braindetox.kr/en/posts/memory_benchmark_scores_reliability_2026.html)
- Supermemory: production 81.6% on LongMemEval (pgvector); its "ASMR" experimental pipeline reaches 98.6% as an oracle (any of 8 attempts correct) at 11-15 LLM calls per question — [2026 Q1 summary](https://lin-guanguo.github.io/llm-memory-research/memory.26Q1.summary/)
- Small extractors: MemReader-0.6B reports 79.56% on LoCoMo vs 78.70% for a gpt-4o-mini MemOS setup (self-reported); MemOperator-4B model card claims to beat GPT-4o-mini on a LoCoMo extraction evaluation — [MemReader](https://arxiv.org/pdf/2604.07877), [MemOperator-4B](https://huggingface-gw-kz.just-ai.com/MemTensor/MemOperator-4B) (both via search snippets, not opened)
- Surveys: "Graph-based Agent Memory: Taxonomy, Techniques, and Applications" (arXiv 2602.05665, Feb 2026) organises graph memory by knowledge/temporal/hypergraph/hierarchical/hybrid graphs and by extraction, storage, retrieval, evolution — [survey](https://www.arxiv.org/abs/2602.05665) (search summary; not read in full). A December 2025 survey "Memory in the Age of AI Agents" exists but I did not open it.
- Older temporal-belief precedent: RecallM keeps a global counter and timestamps each concept update so beliefs can be revised and stale ones implicitly forgotten (2023) — [RecallM](https://arxiv.org/pdf/2307.02738)
- Game NPC memory: one 2025 MDPI paper splits NPC memory into a short-term editable knowledge graph and a long-term vector database, based on Atkinson-Shiffrin; not on arXiv; not opened — [MDPI Mar 2025 (search result)](https://www.mdpi.com/2673-2688/6/5/93)

### Inferences
- Systems that decouple a cheap online path from offline consolidation (LightMem sleep-time, Engram async writes, Nemori window batching, Mem0 v3 single call) are the best match for "writes must not block dialogue".
- Hindsight's opinion network (text + confidence + reinforce/weaken/contradict) and Engram's supersession chain are the closest published designs to "each NPC owns its beliefs", but neither models "who told me" as a first-class field in the sources I read.
- Memory-R1 and the 0.6B-4B extractor papers suggest that a small model fine-tuned for memory operations can beat a prompted larger one, but needs training data; these are self-reported.

### Gaps
- MemOS, MemoryOS and G-Memory method details (call counts, cost) not read beyond abstracts.
- Nemori ingestion-token unit unclear.
- No independent re-run of Hindsight, Engram, REAL, LightMem, MIRIX.
- Memory-R1 comparison against Zep and LangMem not in the extracted table.
- Did not open the "Memory in the Age of AI Agents" survey (Dec 2025) or the MemReader/MemOperator papers.

---

## 5. Benchmarks: what they measure, headline numbers, known criticisms

### Takeaway
LoCoMo, LongMemEval and MemoryAgentBench each test something different; LoCoMo is small, flawed and fits in a modern context window, LongMemEval_S (~115k tokens) is easily saturated by recent systems, and the only benchmark that stresses contradiction resolution (MemoryAgentBench FactConsolidation, BEAM's contradiction category) shows every system failing. Scores for the same product ranged 58-95 on LoCoMo depending on judge and prompt.

### Cited Findings

**LoCoMo (ACL 2024, arXiv 2402.17753)**
- Very long-term conversations grounded in temporal event graphs: up to 35 sessions, about 300 turns and about 9K tokens per conversation per the abstract page; tasks are QA, event summarisation and multimodal dialogue generation; long-context LLMs and RAG help but lag humans — [LoCoMo abstract](https://arxiv.org/abs/2402.17753). Memory papers use conversations of about 16-26k tokens (Mem0 counts 26,031 for full-context) — [Mem0 paper](https://arxiv.org/html/2504.19413), [Zep blog](https://www.getzep.com/blog/lies-damn-lies-statistics-is-mem0-really-sota-in-agent-memory/) (the 9K vs 16-26K difference is unexplained in my sources)
- Public data has 1,986 questions (the paper describes 7,512); category 5 (adversarial, 446) is dropped by Mem0, Zep and EverMemOS, leaving 1,540 — [braindetox](https://braindetox.kr/en/posts/memory_benchmark_scores_reliability_2026.html)
- Audit (independent researchers, "Penfield Labs", dial481/locomo-audit): 99 of 1,540 answers (6.4%) have wrong golden answers, giving a theoretical ceiling of 93.57%; the LLM judge accepted 62.81% of intentionally wrong answers that were vague but on topic and 10.61% of specific-but-wrong answers; category sizes range 96 (open-domain) to 841, and about 56% of adjacent-category comparisons are statistically indistinguishable; the EverMemOS result (92.32%) is within 1.25 points of the ceiling and exceeds per-category ceilings; a third party reported 38.38% for the same system; a full-context GPT-4.1-mini with a CoT answer prompt scored 92.62% — [locomo-audit](https://github.com/dial481/locomo-audit)
- Note: the search-result title of the Penfield dev.to post reads "64% of the answer key is wrong"; the audit repo itself says 99 of 1,540 = 6.4%, which matches the arithmetic, so I use 6.4% and treat the title as a typo or a lost decimal point — [dev.to post (title only)](https://dev.to/penfieldlabs/we-audited-locomo-64-of-the-answer-key-is-wrong-and-the-judge-accepts-up-to-63-of-intentionally-33lg)
- Another audit counted 52 wrong and 56 ambiguous (7.0%); removing them raises scores by 2-3 points. Re-grading the same 1,540 gpt-4o-mini answers with a different prompt moved the score from 55.8% to 81.4% (26 points, no answer changed). With Mem0's 2025 prompt the same answers scored 74.0% (gpt-4o-mini judge), 70.3% (Gemini), 62.1% (Haiku). Under Mem0's 2025 prompt 76.4% of hedged answers passed; Mem0's 2026 prompt passed all of them and counts dates within 14 days as correct. Treating the ten conversations as clusters widens the 95% interval from ±2.3 to ±3.1 points, so differences under about 4.5 points are not reliably separable — [braindetox](https://braindetox.kr/en/posts/memory_benchmark_scores_reliability_2026.html) (secondary compilation; underlying audits not individually verified)
- Full-context GPT-4o-mini scores 72.90% J in Mem0's own table, above every memory system in that table; in Nemori's table full-context equals or beats every memory baseline — [Mem0 paper](https://arxiv.org/html/2504.19413), [Nemori paper](https://arxiv.org/html/2508.03341)

**LongMemEval (ICLR 2025, arXiv 2410.10813)**
- 500 manually curated questions in seven types testing five abilities (information extraction, multi-session reasoning, knowledge updates, temporal reasoning, abstention) inside user-assistant chat histories: LongMemEval_S about 115k tokens, LongMemEval_M about 500 sessions and 1.5M tokens; GPT-4o judge with >97% agreement with human experts — [LongMemEval paper](https://arxiv.org/html/2410.10813)
- Long-context LLMs drop 30-60% reading the full S history vs only evidence sessions; ChatGPT (GPT-4o) 0.577 vs 0.918 offline reading (37% drop); Coze 0.330 (64% drop). Design findings: index at round level, append extracted user facts to keys (+9.4% recall, +5.4% accuracy), time-aware query expansion (+6.8-11.3% temporal recall with a strong LLM) — [LongMemEval paper](https://arxiv.org/html/2410.10813)
- Headline per-system numbers (different judges, models, dates; all self-reported unless noted): Zep paper 71.2% (gpt-4o) — [Zep paper](https://arxiv.org/html/2501.13956v1); Zep 2026 90.2% (gpt-5.4) — [Zep research](https://getzep.com/research); Mem0 2026 94.4% (vendor) — [Mem0 blog](https://mem0.ai/blog/mem0-the-token-efficient-memory-algorithm); Hindsight 83.6-91.4% — [Hindsight paper](https://arxiv.org/html/2512.12818v1); Engram 83.6% (official judge, +10.4 over full-context) — [Engram](https://arxiv.org/abs/2606.09900); Mastra OM 94.87% (gpt-5-mini) — [2026 Q1 summary](https://lin-guanguo.github.io/llm-memory-research/memory.26Q1.summary/)
- Criticisms found: the S setting now fits many context windows; Mem0's own data show retrieval top-50 vs top-200 gives nearly equal overall (94.8 vs 94.4) but multi-session drops from 93.2 to 88.0; Mastra's average-of-types inflates by ~1.3 points — [braindetox](https://braindetox.kr/en/posts/memory_benchmark_scores_reliability_2026.html). LongMemEval-V2 (arXiv 2605.12493, May 2026) extends to web-agent procedural memory — [search result](https://signals.aktagon.com/articles/2026/07/zep-a-temporal-knowledge-graph-architecture-for-agent-memory/) (title seen only in a search summary).

**MemoryAgentBench (ICLR 2026, arXiv 2507.05257)**
- Four competencies: accurate retrieval, test-time learning, long-range understanding, conflict resolution (called "selective forgetting" in v1); new datasets EventQA and FactConsolidation (single- and multi-hop); long inputs are chunked and fed as sequential "memorise this" turns; agents tested include long-context models, BM25, embedding RAG, RAPTOR, GraphRAG, MemoRAG, HippoRAG-v2, Mem0, Cognee, Zep, Self-RAG, MemGPT, MIRIX — [MemoryAgentBench](https://arxiv.org/html/2507.05257v2)
- Findings: RAG beats the plain GPT-4o-mini backbone on retrieval, long-context models are best on test-time learning and long-range understanding, and all methods fail on multi-hop conflict resolution (at most 7%). FactConsolidation single-hop / multi-hop: GPT-4o 60.0 / 5.0, Claude-3.7-Sonnet 43.0 / 2.0, HippoRAG-v2 54.0 / 5.0, BM25 48.0 / 3.0, Mem0 18.0 / 2.0; o4-mini scored 80.0 on the 6K multi-hop version but 14.0 at 32K — [MemoryAgentBench](https://arxiv.org/html/2507.05257v2)
- Reviewers called the selective-forgetting task somewhat artificial and raised fairness concerns about architectures — [MemoryAgentBench review summary](https://iclr.cc/virtual/2026/poster/10010781) (via search result)

**BEAM** (1M/10M-token scale, 10 abilities including contradiction resolution) appears only through Mem0's and Cognee's own reporting here: Mem0 64.1 / 48.6, Cognee 0.79 / 0.67 (vendor) — [Mem0 blog](https://mem0.ai/blog/mem0-the-token-efficient-memory-algorithm), [secondary ranking](https://evermind.ai/blogs/top-ai-memory-systems-benchmarked-in-2026). A public "Agent Memory Benchmark" aggregating BEAM, LifeBench, LoCoMo, LongMemEval and PersonaMem lists Cognee at 80.3% on LoCoMo; I found no methodology page — [same ranking](https://evermind.ai/blogs/top-ai-memory-systems-benchmarked-in-2026)

**Comparability warning from a survey page (Q1 2026):** all numbers are self-reported, methodologies vary, and direct cross-system comparison requires caution; the only LoCoMo figure given for a "Gen 1" system is Mem0 at about 58% — [2026 Q1 summary](https://lin-guanguo.github.io/llm-memory-research/memory.26Q1.summary/)

### Inferences
- A 2025 pattern holds into 2026: memory systems' real, defensible win is cost and latency (1.6k-7k tokens instead of 26k-115k; 0.2-3 s instead of 10-30 s), not accuracy over a good full-context baseline, and a stronger judge/answer model adds more points than the memory design.
- None of the benchmarks I read tests "someone lied to the agent" or tracks which speaker asserted a fact; knowledge-update, temporal reasoning and conflict-resolution categories are the nearest proxies.
- Because of ±3-4.5 point noise and the ceiling at ~93.6%, LoCoMo scores above ~85% should not be read as ranking evidence.

### Gaps
- Underlying audits were read mainly through the locomo-audit page and a secondary compilation (braindetox); the original Penfield and CoreDotToday posts were not opened.
- MemoryAgentBench latency and GPU appendix (D.5, D.6) and Cognee/Zep/MIRIX per-task numbers not read.
- Per-category LongMemEval table for the Zep 2026 claim is on the vendor page only.
- BEAM paper itself not read.

---

## 6. Practical points: write amplification, latency, small models, contradiction handling, provenance

### Takeaway
Write cost is where these systems differ most: Mem0 v3 needs one LLM call plus embeddings; Graphiti needs several dependent LLM calls per episode with strict structured output; A-MEM at least three; Nemori about four per episode. Every system keeps the read path LLM-free (except HippoRAG 2's triple filter, Hindsight's optional reflect, and cross-encoder rerankers). Small open models are the weak link for schema-heavy extractors (Graphiti), partially workable for simple ones (A-MEM on 1-3B, Hindsight on 20B), and are the target of 2026 distillation work. Contradictions are either invalidated by an LLM (Graphiti, Mem0g, Zep), kept side by side (Mem0 v3, REAL, Engram), or confidence-adjusted (Hindsight); none shows reliable results on hard conflict benchmarks.

### Cited Findings

**Write amplification (LLM calls per message/episode)**
- Mem0 2025: 1 extraction call + 1 update-decision call per candidate fact (tool call, GPT-4o-mini); Mem0 v3: 1 call total — [Mem0 paper](https://arxiv.org/html/2504.19413), [Mem0 migration guide](https://docs.mem0.ai/migration/oss-v2-to-v3)
- Mem0g adds LLM entity extraction, relationship generation and an update resolver on top of the base pipeline — [Mem0 paper](https://arxiv.org/html/2504.19413)
- Graphiti: entity extraction (+reflection), entity resolution, fact extraction, fact resolution/contradiction check, temporal extraction, summaries; count not published — [Zep paper appendix list](https://arxiv.org/html/2501.13956v1)
- A-MEM: at least construct + link + evolve; 5.4 s per memory operation with GPT-4o-mini (self-reported) — [A-MEM paper](https://arxiv.org/html/2502.12110)
- Nemori ingestion: about 322.9K tokens vs 526.5K-1,693.4K for MemoryOS ... Mem0 (unit unclear), 3.05 s end-to-end query vs full-context 5.81 s — [Nemori paper](https://arxiv.org/html/2508.03341)
- LightMem: up to 30× (LongMemEval) and 55.5× (LoCoMo) fewer API calls than baselines through batching and offline consolidation — [LightMem](https://arxiv.org/abs/2510.18866)
- Memory size left behind: ~7k (Mem0), ~14k (Mem0g), >600k (Zep) tokens per LoCoMo conversation per Mem0's paper (competitor claim) — [Mem0 paper](https://arxiv.org/html/2504.19413)

**Latency**
- Read: Mem0 search p95 0.200 s, Mem0g 0.657 s, Zep 0.778 s as measured by Mem0 (0.632 s per Zep); Zep 2026 vendor p95 155-162 ms; HippoRAG 2 1.2 s per query; GraphRAG 10.7 s and LightRAG 13.3 s per query (HippoRAG 2 authors' measurement) — [Mem0 paper](https://arxiv.org/html/2504.19413), [Zep blog](https://www.getzep.com/blog/lies-damn-lies-statistics-is-mem0-really-sota-in-agent-memory/), [Zep research](https://getzep.com/research), [HippoRAG 2](https://arxiv.org/html/2502.14802)
- Write: no first-party ingestion latency for Graphiti or Mem0; A-Mem 1.1-5.4 s; Memory-R1 manager p50 ~2 s (LLaMA-3.1-8B); Mem0 v3 "roughly half" of the old extraction latency; HippoRAG 2 ~1.1 s per passage with Llama-3.3-70B — [A-MEM](https://arxiv.org/html/2502.12110), [Memory-R1](https://arxiv.org/html/2508.19828), [Mem0 migration guide](https://docs.mem0.ai/migration/oss-v2-to-v3), [HippoRAG 2 snippet](https://arxiv.org/pdf/2502.14802)
- Eventual consistency: Zep builds its graph with asynchronous background LLM calls, and Mem0's authors saw poor answers when querying right after ingestion — [Mem0 paper](https://arxiv.org/html/2504.19413)

**Can small open models (Gemma/Llama/Qwen ≤30B) do the extraction step?**
- Graphiti's README: works best with structured-output services; other services "may produce incorrect output schemas and ingestion failures, especially smaller models"; `OpenAIGenericClient` has a `json_object` fallback mode — [Graphiti README](https://github.com/getzep/graphiti), [PyPI](https://pypi.org/project/graphiti-core/)
- A third-party known-issues page says local endpoints (Ollama, localhost) use the generic client with basic JSON mode and still hit Pydantic validation errors (e.g. EdgeDuplicate), tracking Graphiti issues #912 and #1116 (I did not open these issues); a January 2026 guide testing 16 Ollama models found 15 passed a simple JSON test but warns Graphiti's real schemas are harder — [known issues (third party)](https://madeinoz67.github.io/madeinoz-knowledge-system/troubleshooting/known-issues.html), [model guide (third party)](https://madeinoz67.github.io/madeinoz-knowledge-system/reference/model-guide.html) (both via search summary)
- A-MEM ran on Qwen2.5 1.5B/3B and Llama 3.2 1B/3B with working but weak scores — [A-MEM paper](https://arxiv.org/html/2502.12110)
- Memory-R1's table: Mem0 with LLaMA-3.1-8B scores J 45.68 and with Qwen-2.5-7B 53.30, vs 66.88 in Mem0's own paper with GPT-4o-mini (different harness/judge, so not a clean comparison) — [Memory-R1](https://arxiv.org/html/2508.19828), [Mem0 paper](https://arxiv.org/html/2504.19413)
- Hindsight extraction with GPT-OSS-20B scores 83.6% LongMemEval / 83.18% LoCoMo (self-reported, own judge) — [Hindsight paper](https://arxiv.org/html/2512.12818v1)
- Mem0 LongMemEval with different extraction LLMs: GPT-5 91.0% vs GPT-OSS-120B 89.8% (Mem0's benchmarks repo, via search snippet) — [mem0 memory-benchmarks](https://github.com/mem0ai/memory-benchmarks)
- Zep paper: "less capable models" did not fully exploit temporal data on the read side — [Zep paper](https://arxiv.org/html/2501.13956v1)
- A Reddit-sourced field report: a 3B extractor needed Ollama's `format` structured output plus schema validation and deterministic repairs — [search summary](https://reddit.sentinel-team.org/posts/1tx5og0/snapshots/2026-06-05T21%3A16%3A39.776657Z) (anecdotal)
- I found no head-to-head of Gemma 3/4 (or Qwen/Llama 7-30B) on Graphiti or Mem0 extraction against a hosted model.

**Contradiction handling**
- Graphiti: LLM compares new edge with related existing edges, sets t_invalid on the old one — [Zep paper](https://arxiv.org/html/2501.13956v1)
- Mem0 2025: LLM chooses DELETE/UPDATE; Mem0g marks obsolete relations invalid; Mem0 v3: both facts kept — [Mem0 paper](https://arxiv.org/html/2504.19413), [Mem0 migration guide](https://docs.mem0.ai/migration/oss-v2-to-v3)
- Hindsight: opinion confidence reinforced/weakened/contradicted by an LLM label — [Hindsight paper](https://arxiv.org/html/2512.12818v1)
- Nemori: each insight classed new / merge / conflict — [Nemori paper](https://arxiv.org/html/2508.03341)
- Engram: contradictions resolved without an LLM call per fact, supersession chain — [Engram](https://arxiv.org/abs/2606.09900)
- Results: no system does well on multi-hop conflicts (MemoryAgentBench ≤7%); Mem0 v3's own BEAM contradiction-resolution category is its weakest (35.7 at 1M) — [MemoryAgentBench](https://arxiv.org/html/2507.05257v2), [Mem0 blog](https://mem0.ai/blog/mem0-the-token-efficient-memory-algorithm)

**Provenance / "who told me"**
- Graphiti: episode nodes + MENTIONS edges (traceable source), `source_description`, speaker always an entity — [Graphiti docs](https://help.getzep.com/graphiti/graphiti/adding-episodes), [Zep paper](https://arxiv.org/html/2501.13956v1)
- Mem0 v3: agent-generated facts stored with equal weight to user facts; no attribution field described — [Mem0 blog](https://mem0.ai/blog/mem0-the-token-efficient-memory-algorithm)
- Hindsight: participant attribution in extraction; separate opinion network with confidence — [Hindsight paper](https://arxiv.org/html/2512.12818v1)
- Engram: provenance-tagged context and supersession chain; MemOS: MemCube metadata includes provenance and versioning; REAL: confidence per fact — [Engram](https://arxiv.org/abs/2606.09900), [MemOS](https://arxiv.org/abs/2507.03724), [REAL](https://arxiv.org/abs/2606.10694)
- Survey framing: graph memory generalises flat buffers/vector DBs, with an "evolution" stage split between internal consolidation/pruning and external feedback — [Graph-based Agent Memory survey](https://www.arxiv.org/abs/2602.05665)

### Inferences
- If the write path can be asynchronous and run by the larger model (the hosted Sol), the small local model needs only to read (retrieval is LLM-free in Graphiti and Mem0 v3), which sidesteps the weakest point (small-model structured extraction). The conflicting evidence on Graphiti with local models suggests not asking Gemma to run Graphiti's multi-schema extraction.
- A belief-aware design can use the speaker-as-entity + episode provenance that Graphiti already has, or store "A said X at time t" as the atomic fact (Mem0 v3 style, ADD-only), then derive "truth" separately from the game-code state. Published systems do not give a ready "claim vs verified fact" distinction.
- Retrieval immediately after a write may miss the new information (Zep async build); a short raw-episode buffer in the prompt covers the gap.

### Gaps
- No reliability numbers (schema-failure rate, retry rate) for any ≤30B open model on Graphiti, Mem0 or A-MEM extraction.
- No head-to-head cost per message in dollars/tokens measured identically across Graphiti, Mem0 v3, Hindsight and LightMem.
- No source on how memory systems behave with deliberately false statements from other speakers.

---

## 7. What the evidence implies for a per-NPC memory shared by a small local model (Gemma) and a large hosted model (Sol)

### Takeaway
The best-supported architecture pattern across 2025-2026 sources is: asynchronous, LLM-driven writes producing time-stamped, provenance-tagged facts or edges; LLM-free hybrid retrieval (vector + BM25 + graph, RRF) for reads; invalidation or supersession rather than deletion. Graphiti is the only mature open-source implementation of the bi-temporal piece; Mem0 v3 is the simplest write path; Hindsight and Engram show how to attach confidence and supersession to beliefs. None provides "truth vs belief" out of the box.

### Cited Findings
(see sections 1-6; this section is deliberately inference-only)

### Inferences
- Per-NPC isolation maps to Graphiti `group_id` (or one store per NPC). Game time should be the `reference_time`; real time would make "yesterday" meaningless.
- Beliefs that may be false: store each claim as an episode with speaker, hearing time and (optionally) a game-code verification flag; keep truth in game code as the user specified; use invalidation only for what the NPC itself learned supersedes an earlier claim, not to "correct" a lie the NPC has no reason to doubt.
- Writes: queue per message or per scene-end batch (LightMem/Nemori style) to a worker that uses the hosted model; avoid per-turn dependent LLM chains on the dialogue path. Expect eventual consistency like Zep and cover it with a recent-turn buffer.
- Reads by Gemma: use the pre-computed retrieval (hybrid search) and a compact context (Zep: 1.6k tokens; Mem0: 1.8-7k; benchmarks show 2-7k-token contexts are the norm).
- Forgetting/decay is not provided by Graphiti or Mem0 v3; Engram's recency/salience signals and Hindsight's confidence updates are the only published hooks, so decay would be custom.

### Gaps
- No tests of any of these on multi-party, adversarial (lie-heavy), real-time-game conversation; all evidence is from user-assistant or two-person chat benchmarks.
- No measured Gemma-class results for write-path extraction in any memory system.

---

## Source-quality notes for the writer
- Vendor sources: Zep research page, Zep blog, Mem0 blog/README, Cognee benchmarks, Hindsight (Vectorize), Evermind blog (competitor), Supermemory. Their numbers are self-reported and judged under their own setups.
- Independent or semi-independent: locomo-audit (Penfield-affiliated audit, fetched), MemoryAgentBench (academic, ICLR 2026), LongMemEval (academic), Nemori's comparison (academic but ran Mem0/Zep through commercial APIs), HippoRAG 2 (academic, baselines run by authors).
- Secondary and search-snippet-only: braindetox compilation (reasonably specific but a blog), lin-guanguo.github.io (a personal research wiki), the Reddit snapshots, the madeinoz known-issues pages, MemReader/MemOperator, the Graphiti mintlify mirror.
- One fetched page (LongMemEval arXiv HTML) came back with a summarizer note that the page content contained a block of extraction/formatting instructions; I ignored it and used only the benchmark facts.

# Neural / parametric memory architectures for language models (state as of 2026-11-04)

Scope: designs where memory lives inside the network or in a small trained artifact attached to it (weights, latent pools, trained caches, generated adapters), not in text. Question behind it: could each NPC (Gemma-class 12B local model + large hosted model) keep its memory in neural form instead of / besides a structured graph memory?

Method note for the report writer: most facts below come from arXiv abstract/HTML pages and GitHub READMEs read through a fetch tool that summarises pages with a small model; numbers were copied from those summaries and should be spot-checked against the PDFs before being quoted as final. Where a number came only from a secondary blog it is marked "secondary". Tags: [authors] = result reported only by the method's own authors; [independent] = reproduced or tested by a different group. The rule of thumb: almost everything below is [authors] only; the few independent checks are listed explicitly.

---

## 1. Test-time-learning memories (Titans, ATLAS, Nested Learning / HOPE, TTT layers, TTT-E2E, Sleep) — what exists, at what scale, is anything released?

### Takeaway
The family trains the memory (a small MLP or fast weights) by gradient steps at inference time, but every flagship result is from models trained from scratch at 0.17B–3B parameters (HOPE up to 1.3B, TTT-E2E up to 3B). No official Titans / ATLAS / HOPE weights or code exist as of this research; only community re-implementations. Only TTT-E2E has official code. None can be bolted onto an existing Gemma-class checkpoint. The one independent re-implementation of Titans did not reproduce a reliable win.

### Cited Findings

**Titans (Google, arXiv Dec 2024, Behrouz, Zhong, Mirrokni)**
- Write path: a neural long-term memory (an MLP, depth L_M >= 1; paper argues >= 2 beats linear) is updated at test time by gradient descent on an associative loss ||M(k_t) - v_t||^2; "surprise" is the gradient; update is M_t = (1 - alpha_t) M_(t-1) + S_t with momentum S_t = eta_t S_(t-1) - theta_t grad(loss), alpha_t is a forgetting gate. Read path: query the memory MLP; three ways to combine with attention: MAC (memory as context, retrieved memory prepended), MAG (gate with sliding-window attention), MAL (memory as a layer) — [Titans HTML](https://arxiv.org/html/2501.00663)
- Tested sizes: 170M, 340M, 400M (15B tokens FineWeb-Edu) and 760M (30B tokens); "working on finalizing the results of larger models" — [Titans HTML](https://arxiv.org/html/2501.00663)
- Language modelling at 760M/30B (Wiki ppl / avg accuracy): Transformer++ 25.21 / 48.69; Mamba2 22.94 / 48.34; Gated DeltaNet 21.18 / 49.69; Titans MAC 19.93 / 52.51; MAG 18.61 / 52.50 — [Titans HTML](https://arxiv.org/html/2501.00663)
- RULER S-NIAH at 16K: S-NIAH-PK Titans MAC 98.4 vs Mamba2 5.4, DeltaNet 71.4, TTT 88.4; S-NIAH-W Titans MAC 95.2 vs 0.0 for Mamba2/DeltaNet/TTT; claims scaling to >2M context; BABILong claims beat GPT-4 and Llama3.1-70B (scores only in figures) — [Titans HTML](https://arxiv.org/html/2501.00663)
- Cost: throughput linear in context, "slightly slower than Mamba2 and Gated DeltaNet"; deeper memory trains slower; chunked parallel update with a loss of expressivity — [Titans HTML](https://arxiv.org/html/2501.00663)
- Code/weights: v1 said code "soon"; no official implementation found. Community: lucidrains/titans-pytorch (unofficial), PyPI `titans-memory` (30M-parameter MAC built from config, no trained weights), `open-titans` — [search results](https://pypi.org/project/titans-memory/)
- [independent] "Titans Revisited" (Sapienza, Oct 2025) re-implemented Titans and found it does not reliably beat baselines on masked LM, time-series forecasting and recommendation; attributes the gap mainly to input chunking, but finds the neural memory component consistently adds information beyond attention-only models. Caveat: reimplementation without the original code, different tasks, not a direct test of Google's numbers — [aitopics summary of arXiv 2510.09551](https://aitopics.org/doc/arxivorg:F59FBF5B)

**ATLAS (Google, May 2025)**
- Fixes three claimed Titans weaknesses (limited capacity, online update looks only at latest token, weak memory management): optimises memory over current AND past tokens; also proposes "DeepTransformers". Reports "+80% accuracy at 10M-token context on BABILong" over Titans (abstract wording; the baseline of the +80% is unclear) — [ATLAS abstract](https://arxiv.org/abs/2505.23735)
- Model sizes, weights and code: not found on the abstract page; no official release found in search — [search results](https://pypi.org/project/open-titans/)

**MIRAS (Google, April 2025, "It's All Connected")**: theory framework: memory architecture, attentional bias, retention gate (how forgetting/regularisation works), memory algorithm. Decoder coverage said code was expected "in the near future"; no official release found. PyPI `python-titans-miras` is third-party — [search coverage](https://the-decoder.com/google-outlines-miras-and-titans-a-possible-path-toward-continuously-learning-ai/)

**Nested Learning / HOPE (Google, NeurIPS 2025; arXiv 2512.24695)**
- Idea: a model is a set of nested optimisation problems ranked by update frequency; HOPE = self-modifying Titans variant + Continuum Memory System (CMS: memory as a spectrum of MLP blocks updating at different frequencies; attention = infinite frequency, MLP = zero in Transformers) — [Google blog](https://research.google/blog/introducing-nested-learning-a-new-ml-paradigm-for-continual-learning/), [paper HTML](https://arxiv.org/html/2512.24695)
- Sizes: 760M/30B tokens and 1.3B/100B tokens. Table 2: HOPE 760M Wiki ppl 18.68, avg acc 52.28 (Titans 20.08/51.68, Transformer++ 24.18/50.11, TTT 24.17/47.32); HOPE 1.3B Wiki ppl 14.39, avg acc 58.04 (Titans 15.60/56.82, Transformer++ 17.92/53.38, TTT 18.42/55.58) — [paper HTML](https://arxiv.org/html/2512.24695). A secondary summary gives different 1.3B numbers (15.11 / 57.23): trust the paper table — [secondary](https://arxiviq.substack.com/p/nested-learning-the-illusion-of-deep)
- NIAH: HOPE beats other attention-free models but a Transformer wins on MK-NIAH-1 (Transformer 79.4/83.0/61.4 at 4K/8K/16K vs HOPE 29.4/24.8/14.8). Authors state: small models incl. HOPE drop on BABILong without fine-tuning; catastrophic forgetting "not solved"; HOPE has higher memory use than some baselines — [paper HTML](https://arxiv.org/html/2512.24695)
- Continual-learning results (class-incremental CLINC/Banking/DBpedia, continual translation Manchu/Kalamang) are reported in figures only; text says HOPE-3 nearly recovers single-language performance while ICL drops sharply — [paper HTML](https://arxiv.org/html/2512.24695)
- Code/weights: none official. Community reproductions: kmccleary3301/nested_learning, retentionlabs/nested_learning (pilot ~3B tokens), HF `smallhours19/nested-learning-hope` (states it is an unofficial reduced-scale reproduction) — [search results](https://pypi.org/project/nested-learning/)

**TTT layers (Stanford/UCB, 2024)**: the RNN hidden state is itself a small model (TTT-Linear = linear, TTT-MLP = 2-layer MLP) updated by a self-supervised step on the sequence; 125M–1.3B; Transformer and TTT keep lowering perplexity with context, Mamba stops improving beyond ~16k; TTT-MLP has memory-I/O issues — [TTT abstract](https://arxiv.org/abs/2407.04620)

**TTT-E2E (Dec 2025, Tandon et al., arXiv 2512.23675) — the 2026-era successor with released code**
- Write path: a standard Transformer with 8K sliding-window attention; during inference it does next-token-prediction gradient steps on each 1K-token mini-batch of the context, updating only the MLPs of the last 1/4 of blocks (embeddings, norms, attention frozen); updated blocks get a second static MLP as "safe" storage; initialisation is meta-learned so that test-time updates minimise final loss — [TTT-E2E HTML](https://arxiv.org/html/2512.23675)
- Sizes 125M–3B; 3B results trained on 164B tokens; context scaling matches full attention while Mamba2 / Gated DeltaNet do not; prefill ~2.7x faster than full attention at 128K on H100, constant latency in context length — [TTT-E2E HTML](https://arxiv.org/html/2512.23675), [search result](https://arxiv.org/pdf/2512.23675)
- Failure modes: recall is poor — needle-in-a-haystack at 128K: 0.06 / 0.05 / 0.03 (S-NIAH-1/2/3) vs full attention 0.99 / 0.86 / 0.64; training 3.4x slower than full attention at 8K (gradients of gradients; FlashAttention backward not supported) but 1.2x faster at 128K; only up to 3B; decoding eval on 512 sequences only — [TTT-E2E HTML](https://arxiv.org/html/2512.23675)
- Code: https://github.com/test-time-training/e2e (reference training/eval code; no ready model for Gemma) — [search result](https://arxiv.org/pdf/2512.23675)

**Sleep paradigm (Google, "Language Models Need Sleep", arXiv 2606.03979, v1 2 Jun 2026, v2 10 Jul 2026; Google Research blog 27 Oct 2026)** — see also section 6 for consolidation details
- Built on HOPE/CMS with MLP blocks assumed sparse MoE; backbones used in experiments: Llama-3B, Llama3-8B (class-incremental), Llama-3.2-1B (ARC few-shot), Qwen3-1.7B and Qwen3-8B (math) — [paper HTML](https://arxiv.org/html/2606.03979)
- No release statement for code or weights in the blog or the paper text read — [Google blog](https://research.google/blog/the-sleep-framework-a-new-way-for-llms-to-retain-knowledge-in-continual-learning/)

### Inferences
- "Released and usable today" for this family = only research code (TTT-E2E) and community re-implementations; nothing that can be fine-tuned from or attached to a 12B Gemma. For the NPC project these are a watch-list, not a component.
- The only independent check (Titans Revisited) is lukewarm: memory helps, but headline wins are not robust to reimplementation choices.
- All the strong claims are about long-context LM/NIAH, not about writing discrete facts or episodes and recalling them reliably; TTT-E2E's own needle numbers show weak exact recall (0.03–0.06 at 128K).

### Gaps
- Exact BABILong scores for Titans/ATLAS are only in figures; ATLAS model sizes and tables not read (abstract page only).
- No evidence found of any 2026 pretrained Titans/ATLAS/HOPE checkpoint from Google; absence of evidence from searches, not proof.
- Sleep paper: parameter counts of the MoE/HOPE model, cost of a sleep phase (appendix B.5) and code release not found in the portion read (first ~100K characters of 170K).

---

## 2. Memory layers and key-value memories in weights (Memory Layers at Scale, sparse memory finetuning, Engram, MemoryLLM, M+, Larimar, LM2, Memory Decoder, Memorizing Transformers, KBLaM, Locas)

### Takeaway
Two sub-families. (a) Pretrained-in memory (Memory Layers, Engram, LM2, Memorizing Transformers): large capacity for facts, but they require training the base model from scratch and only Meta/DeepSeek/IBM-style code exists, no usable big checkpoints. (b) Attachable memories (MemoryLLM/M+, Larimar, Memory Decoder, KBLaM, Locas): trained once on top of an LLM, with fast write; they exist at 1B–8B, with weights released only for MemoryLLM/M+ and Memory Decoder.

### Cited Findings

**Memory Layers at Scale (Meta FAIR, Dec 2024, arXiv 2412.09764)**
- Design: replace some FFN layers with a sparse key-value lookup (product-key keys, top-k, one shared pool across ~3 memory layers, gated "Memory+" variant); adds parameters, almost no FLOPs; cost is memory bandwidth; custom kernel reaches ~3 TB/s on H100 vs < 400 GB/s PyTorch — [HTML](https://arxiv.org/html/2412.09764)
- Sizes: dense base 134M, 373M, 720M, 1.3B (1T tokens), and 8B (200B and 1T tokens); up to 64M keys, ~128B memory parameters with the 1.3B base — [HTML](https://arxiv.org/html/2412.09764)
- Recall: 1.3B base, 1M memory values: NaturalQuestions 13.68 vs dense 7.76, MoE 8.14, PEER 12.33; TriviaQA F1 42.89 vs 32.64 / 31.46 / 42.46. With 64M keys: NQ 20.78, TriviaQA 62.14 vs Llama 2 7B (2T tokens, ~10x FLOPs) 25.10 / 64.00. 8B at 1T tokens: MMLU 63.04 vs 59.68 dense, TriviaQA 68.15 vs 63.62 — [HTML](https://arxiv.org/html/2412.09764)
- Limits stated: not as fast as dense layers on current hardware; effects on learning/forgetting only proposed, not studied; short-form QA only; instabilities for small bases — [HTML](https://arxiv.org/html/2412.09764)
- Code: https://github.com/facebookresearch/memory — code only (Meta Lingua based, CC-BY-NC, one commit, no checkpoints, you must train yourself) — [GitHub](https://github.com/facebookresearch/memory)

**Sparse memory finetuning (Meta FAIR + UC Berkeley, Oct 2025, arXiv 2510.15103)**
- Write path: in a memory-layer model update only the few memory slots most specific to the new batch (ranked by TF-IDF of slot access counts vs a background of 1,000 DCLM batches); top t = 500 slots for facts, 10,000 for document QA; gradients stopped elsewhere; SGD. Read path unchanged (normal memory lookup) — [HTML](https://arxiv.org/html/2510.15103)
- Model: only a 1.3B model, one middle layer (12 of 22) replaced by a 1M-slot memory, k = 32, 4 heads — [HTML](https://arxiv.org/html/2510.15103)
- Forgetting at equal new-knowledge acquisition (drop in NaturalQuestions F1): full finetuning -89%, LoRA -71%, sparse memory finetuning -11% — [abstract/HTML](https://arxiv.org/html/2510.15103)
- Tasks: 1,000 TriviaQA questions rewritten as statements learned sequentially; document QA on 100 SimpleQA questions / 1,824 chunks. Limits stated: only factual QA, only 1.3B, SGD vs AdamW baselines not tuned, no reasoning/coding tests. Code release not confirmed in the page read — [HTML](https://arxiv.org/html/2510.15103)

**DeepSeek Engram (Jan 2026, "Conditional Memory via Scalable Lookup")**
- Hashed N-gram embedding lookup (O(1)) fused with hidden states; inserted deep in the network (example layers 2 and 15); lookups are deterministic so the table can sit in CPU RAM — [repo](https://github.com/deepseek-ai/Engram)
- Secondary numbers (ArXivIQ summary, not read in the paper): Engram-27B vs MoE-27B at equal params and FLOPs: MMLU +3.4, BBH +5.0, HumanEval +3.0, multi-query NIAH 84.2 -> 97.0; best split ~20–25% of sparse parameters to Engram; < 3% overhead with a 100B-parameter table in host RAM — [secondary](https://arxiviq.substack.com/p/conditional-memory-via-scalable-lookup)
- Release: repo is a demo script plus paper; Apache-2.0 code; "Engram models" under a separate model license; weights not found — [repo](https://github.com/deepseek-ai/Engram)
- Note: static pretrained knowledge lookup (not a per-entity, writable-at-runtime memory).

**MemoryLLM (ICML 2024) and M+ (Feb 2025, arXiv 2502.00592)**
- Latent memory pool inside the model: ~1B parameters worth of memory tokens (N memory tokens per layer) for Llama-2-7B-based MemoryLLM — [M+ HTML](https://arxiv.org/html/2502.00592)
- Write: inject a chunk; the last K = 256 memory tokens are combined with the chunk; the new K tokens replace K randomly dropped old ones (dropped tokens lost permanently in MemoryLLM). Read: cross-attention to the memory during generation — [M+ HTML](https://arxiv.org/html/2502.00592)
- M+ (Llama-3.1-8B): dropped tokens go into a per-layer long-term memory with an age; a co-trained retriever (2-layer MLP projections) pulls K0 = 2,560 tokens per layer back; 10,240 short-term + 2,560 retrieved = 12,800 per layer; separate LoRA weights for update (write) vs generation (read); three-stage training on 8 A100 — [M+ HTML](https://arxiv.org/html/2502.00592)
- Retention extended from < 20k to > 160k injected tokens at similar GPU memory (21,177 MB vs 19,239 MB for Llama-3.1-8B with 16k context; 32,574 MB for SnapKV) — [M+ HTML](https://arxiv.org/html/2502.00592)
- Quality cost: LongBench QA-F1 average M+ 32.78 vs Llama-3.1-8B-16k 35.81 (at 16k); trails on HotpotQA and MuSiQue; random dropping loses information; retrieval makes M+ slower; ~30% of ground-truth tokens retrieved vs ~3% random — [M+ HTML](https://arxiv.org/html/2502.00592)
- Released: HF `YuWangX/memoryllm-7b`, `memoryllm-8b`, `memoryllm-8b-chat`, `mplus-8b` (pretrained version only); MIT licence; needs flash-attention, bf16, tested on H100 80GB — [GitHub](https://github.com/wangyu-ustc/MemoryLLM)

**Larimar (IBM, ICML 2024, arXiv 2403.11901)**
- Architecture: BERT-large encoder + GPT-2-large (1.3B) or GPT-J-6B decoder; episodic memory = a 512 x 768 matrix; a learned projection turns memory readouts into a KV prefix conditioning every decoder layer; write is one-shot by solving a least-squares problem with pseudo-inverses (no gradient steps); selective forgetting by removing a write; optional scope detector routes queries to memory or plain decoder — [HTML](https://arxiv.org/html/2403.11901)
- Speed: ~1.1 s (GPT-2) / 1.7 s (GPT-J) per edit vs ROME 4.8 s / 13.9 s — [HTML](https://arxiv.org/html/2403.11901)
- Recall: CounterFact edit success 99.6, paraphrase 88.4 (93.6 with two rephrases written); ZsRE 94.5 / 70.4 (82.2 with rephrases). Sequential 200 facts x 5 rephrasings: retention 0.97 (1.3B) / 0.92 (6B) vs GRACE 0.93, MEND 0.27. Batch rewrite accuracy ~100% up to 512 edits (= memory size K), ~82% at 1,024 — [HTML](https://arxiv.org/html/2403.11901)
- Forgetting: forgetting one fact drives its recall to ~0% while keeping ~99% of the others (6B, N = K); rephrase-attack success 17.6% vs ROME 29%, MEMIT 49% — [HTML](https://arxiv.org/html/2403.11901)
- Limits: short facts only; trained on sentence completion; neighbourhood specificity drops to 13.7 without the scope detector (80.4 with). Code: github.com/IBM/larimar, Apache-2.0, no checkpoints published (README expects a self-trained `larimar-1.3b` checkpoint) — [HTML](https://arxiv.org/html/2403.11901), [GitHub](https://github.com/IBM/larimar)

**LM2 (Convergence AI, Feb 2025, arXiv 2502.06049)**
- Decoder-only Transformer + memory bank (2,048 slots x 2,048 dims) in every one of 16 blocks, cross-attention read, input/forget/output gates for write: M(t+1) = g_in tanh(E_mem) + g_forget M(t); ~1.2B backbone + ~0.5B memory — [HTML](https://arxiv.org/html/2502.06049)
- BABILong: +37.1% vs RMT and +86.3% vs Llama-3.2 (averages); MMLU 28.0 -> 29.4 (only ~1.4 points); RAG beats LM2 on relation tracking; paper's memory-bank shape description is internally inconsistent; code github.com/convergence-ai/lm2 — [HTML](https://arxiv.org/html/2502.06049)

**Memory Decoder (Aug 2025, arXiv 2508.09874)**
- A small decoder (124M / 345M / 774M / 0.5B) is pretrained to imitate a kNN-LM retriever's output distribution over a domain datastore (loss = 0.5 KL + 0.5 CE); at inference p = alpha p_Mem + (1 - alpha) p_base with a frozen base; works for any model sharing the tokenizer (Qwen2/2.5 0.5B–72B) — [HTML](https://arxiv.org/html/2508.09874)
- Recall (perplexity): Qwen2.5-0.5B biomedical 17.01 -> 3.74, finance 16.04 -> 3.87, law 9.86 -> 4.57; Qwen2.5-72B 5.90 -> 3.44 / 6.80 -> 3.23 / 4.84 -> 3.70 — [HTML](https://arxiv.org/html/2508.09874)
- Cost: inference 1.28x base latency vs 1.51x in-context RAG vs 2.17x kNN-LM; pretraining budget ~ a 7B model for one epoch on 8 x A800 per domain; downstream avg 69.79 vs DAPT 60.84 (base 67.45) — i.e., little forgetting. Weights and code released (GitHub LUMIA-Group/MemoryDecoder, HF `Clover-Hill/memorydecoder`) — [HTML](https://arxiv.org/html/2508.09874)
- Limit: it models a fixed domain corpus; no per-entity incremental write path (requires datastore + kNN pretraining). Perplexity, not dialogue recall.

**Memorizing Transformers (Google, ICLR 2022, arXiv 2203.08913)**: kNN lookup into a non-differentiable cache of past (key, value) pairs; improves LM on C4, arXiv, PG-19, GitHub, Isabelle; "performance steadily improves when we increase the size of memory up to 262K tokens"; model sizes and exact gains not retrieved — [abstract](https://arxiv.org/abs/2203.08913)

**KBLaM (Microsoft, Oct 2024, arXiv 2410.10450)**: facts (key-value triples) encoded by a sentence encoder + linear adapters into continuous KV pairs, injected with a "rectangular attention" mechanism; no retrieval module; cost linear in KB size; > 10K triples into an 8B model with 8K context on a single A100 80GB; KB can be updated without retraining the LLM; code and datasets at github.com/microsoft/KBLaM — [abstract](https://arxiv.org/abs/2410.10450)

**Locas (Tencent AI Lab, Feb 2026, arXiv 2602.05085)**
- Write: forward-pass the context chunk, pick the top-r (r = 64) most-activated GLU dimensions per layer, clone matching key/gate rows from the backbone FFN, zero-init the value matrix, then back-propagate the LM loss as context streams in (LR 4e-3); the side-FFN can be merged into the weights. Read: runs in parallel to the FFN, H_out = FFN(A) + tau * Locas(A); no retrieval — [HTML](https://arxiv.org/html/2602.05085)
- Models: Qwen3-0.6B/1.7B/4B (Base) only. LoCoMo F1 (Qwen3-4B-Base, single-hop / multi-hop): full attention 41.2 / 22.6; + TempLoRA 41.5 / 24.3; + Locas-GLU 47.6 / 28.1. PG-19 at 200K context: long-context attention still beats Locas-GLU (24.57 vs 25.00 on 0.6B; 18.58 vs 19.04 on 1.7B). Overhead 11.0M params and 1.8x time (1.7B) vs TempLoRA 73.4M and 4.7x. MMLU after memorising a book: 60.4 -> 60.2 (Locas-GLU) vs 59.8 (TempLoRA) on Qwen3-1.7B-Base — [HTML](https://arxiv.org/html/2602.05085)
- Limits: one forgetting measurement on one model; no code link found; the "0.02% parameters" in the abstract is not traceable to a table — [HTML](https://arxiv.org/html/2602.05085)

### Inferences
- Memory Layers / Engram show that facts are cheaper to store in lookup tables than in dense weights, but they need pretraining a new base; they do not offer a per-character plug-in.
- The two designs most aligned with "pluggable per-character memory" are Larimar-style (write by closed-form solve in ~1–2 s, delete by un-writing, 512-slot capacity per memory) and KBLaM-style (plug-in memory tokens, update without retraining) — but both are research code on GPT-2/GPT-J/Llama-class bases, not on Gemma, with no packaged checkpoints (Larimar) or only code (KBLaM).
- MemoryLLM/M+ is the only attachable latent-memory pool with downloadable 7–8B weights, but it is a Llama-based model, loses quality on short tasks vs the plain model, and retains mostly free-form context rather than structured facts.

### Gaps
- Memorizing Transformers sizes/perplexity gains not retrieved.
- Sparse memory finetuning: code release not verified; no independent replication found; no test above 1.3B.
- Engram: no primary-source numbers read (secondary only), weights unclear.
- No independent replication found for Memory Layers, Larimar, LM2, M+ or Locas.

---

## 3. Trained context artifacts (Cartridges, Cartridges at Scale, Knowledge Modules / deep context distillation, Parametric RAG, gist tokens, ICAE, xRAG)

### Takeaway
Cartridges are the strongest "trained cache per corpus" result (match in-context quality with ~38.6x less memory) but training takes ~30 min on 8 x H100 for an 8B model and they need a custom serving stack; independent follow-up found that separately trained cartridges can collapse when mixed. LoRA-style Knowledge Modules are cheaper to serve (LoRA rank 16) but are trained per document and have only been tested on 3B–8B. Gist / ICAE / xRAG compress a prompt at query time and are not persistent memories.

### Cited Findings

**Cartridges (Stanford Hazy Research, Jun 2025, arXiv 2506.06266)**
- Write: offline train a small KV cache (p trainable tokens; "prefix-tuning" parametrisation) per corpus with "self-study": generate synthetic conversations about the corpus with the model itself and train with a context-distillation objective (next-token training on the raw corpus does not match ICL) — [HTML](https://arxiv.org/html/2506.06266)
- Read: load the cache as the KV prefix at inference; composition by concatenating independently trained cartridges without retraining — [HTML](https://arxiv.org/html/2506.06266)
- Models: Llama-3B and Llama-8B (70B only as a memory example: ~84 GB KV cache for 128k tokens). Benchmarks LongHealth, MTOB, QASPER, GenConvo (FinanceBench-derived), MMLU for retention — [HTML](https://arxiv.org/html/2506.06266)
- Numbers: matches ICL with 38.6x less memory and up to 26.4x higher throughput (SGLang, 1 x H100); extends effective context from 128k to 484k tokens on MTOB (beat ICL by +11.0 chrF over the first 130k tokens); an MTOB cartridge ~0.6 GB; training an ICL-quality cartridge for Llama-8B took ~30 minutes on a single 8 x H100 node (unoptimised); self-study is "much more expensive than standard ICL prefill" — [abstract](https://arxiv.org/abs/2506.06266), [HTML](https://arxiv.org/html/2506.06266)
- Ablations: context distillation beat next-token prediction on MTOB (24.9 -> 33.5 chrF); prefix-tuning beat memory-matched LoRA (+4.5 chrF) and kept MMLU (54.7 -> 54.3) while LoRA dropped (54.7 -> 45.3); initialising from the first p corpus tokens was best (LongHealth 55.3% vs random vectors 29.9%) — [HTML](https://arxiv.org/html/2506.06266)
- Release: github.com/HazyResearch/cartridges (Apache-2.0, Qwen3-4B in the example, Tokasaurus recommended for synthesis/serving, SGLang "not tested extensively"); "upload trained cartridges to HuggingFace" TODO unchecked; Tokasaurus can take a list of cartridges per request; no multi-tenant isolation discussed — [GitHub](https://github.com/HazyResearch/cartridges)
- [independent-ish] "Cartridges at Scale" (arXiv 2606.04557, 3 Jun 2026, Hardalov, Iglesias, de Gispert; a search snippet attributes it to Amazon AGI, affiliation not shown on the page read): monolithic cartridges don't scale and naively mixing separately trained cartridges drops performance "to near chance" (composition collapse); fix = joint training with dynamic distractor mixing plus a budget manager rotating hundreds of per-document cartridges between GPU and storage; > 1M-token collections, +10–31 points over monolithic at the same budget, oracle cartridge within 2–6 points of full ICL, with retrieval for selection matches or beats RAG with 3–4x fewer prompt tokens — [abstract](https://arxiv.org/abs/2606.04557)
- NeurIPS 2026 listing "Towards continual in-context learning via training Cartridges at test time": self-study + compaction loop; matches/exceeds ICL on a synthetic retrieval task and two Continual Learning Bench tasks (abstract-level snippet only) — [search result](https://neurips.cc/virtual/2026/173773)

**Knowledge Modules / Deep Context Distillation (Caccia et al., Mar 2025, arXiv 2503.08727)**
- Write: a LoRA (rank 16, alpha 16) per document trained so that (base + KM, no document) matches (base with document in context) on output distribution (KL) and hidden states (L1); data = document chunks or synthetic summaries / QAs / Entigraph. Read: plug the LoRA in; delete = remove it. Routing across many KMs is left to future work — [HTML](https://arxiv.org/html/2503.08727)
- Models Phi-3 (3B) and Llama-3.1 8B; QuALITY, NarrativeQA. Closed-book NarrativeQA Rouge-L: base 12.5 -> best KM 27.3 (28.6 with concatenated samples). RAG + knowledge extractor 36.7 -> 40.8 with KM added. KM + extractor without retrieval beats RAG + extractor with one retrieved passage, using ~40 vs ~200 prompt tokens. MMLU subset 63.9% -> 62.9% with a random KM attached. ICL memory for a 60k-token doc ~30 GB on Llama-3.1 8B — [HTML](https://arxiv.org/html/2503.08727)
- Limits: per-document gradient training cost; degrades when combined with RAG on longer contexts; assumes the right document is known. No code link found — [HTML](https://arxiv.org/html/2503.08727)

**Parametric RAG (PRAG, Jan 2025, arXiv 2501.15915)**: writes documents into FFN parameters ("document parameterisation") offline; claims better effectiveness and efficiency than in-context RAG and gains when combined; no numbers or model names on the abstract page; says code/data/models released on GitHub — [abstract](https://arxiv.org/abs/2501.15915)

**Gist tokens (Mu et al., Apr 2023, arXiv 2304.08467)**: train an LM to compress a prompt into a few cacheable "gist" tokens by changing only attention masks; up to 26x compression, up to 40% FLOPs reduction, 4.2% wall-time speedup, "minimal loss in output quality"; LLaMA-7B and FLAN-T5-XXL — [abstract](https://arxiv.org/abs/2304.08467)

**ICAE (In-context Autoencoder, Jul 2023, arXiv 2307.06945)**: LLM compresses a long input into memory slots; Llama-based, ~1% extra parameters, 4x context compression; pretrained on autoencoding + LM, fine-tuned on instructions — [abstract](https://arxiv.org/abs/2307.06945)

**xRAG (May 2024, arXiv 2405.13792)**: retrieved document's dense embedding fed to the LLM as a modality through a small trained bridge (retriever and LM frozen); > 10% average gain across six knowledge-intensive tasks; matches uncompressed models on several datasets with 3.53x fewer FLOPs; backbones 7B to 8x7B MoE — [abstract](https://arxiv.org/abs/2405.13792)

### Inferences
- Cartridges and KMs are "per-corpus" artifacts. For an NPC the analogue is "one artifact per character built from that character's journal"; the evidence is all for static documents, not for a stream of new events with deletions.
- Prefix/KV artifacts need serving support (KV-prefix injection per request). The only stack named is Tokasaurus/SGLang; whether llama.cpp can load a trained KV prefix for a quantised GGUF was not verified (my inference: possible via saved KV state, but nothing cited).
- LoRA artifacts (KM, PRAG, D2L below) are the ones that map to an existing, supported mechanism in llama.cpp (see section 8).
- The composition-collapse finding is directly relevant: a character's memory made of several independently trained parts may not compose.

### Gaps
- Cartridges: no number for GPU-hours; appendix tables (DuoAttention etc.) were beyond the portion read; cartridge size needed for a 12B model not stated.
- KM/DCD: no released code found; no test on a 12B model.
- PRAG: no numbers read (abstract only).
- Gist/ICAE/xRAG are 2023–2024 query-time compression; I did not look for 2026 successors.

---

## 4. Generated adapters (Text-to-LoRA, Doc-to-LoRA, Drag-and-Drop LLMs, Generative Adapter) and how fast they produce an adapter

### Takeaway
Hypernetworks that emit a LoRA in one forward pass are real and released (Sakana T2L and D2L: code + checkpoints). D2L internalises a document in 0.2–0.55 s versus 40–465 s for gradient-based context distillation, but retains ~82% of in-context quality on QA and the hypernetwork must be meta-trained per base model (D2L: ~5 days on 8 x H200) — for Gemma-2-2B, Mistral-7B and Qwen3-4B, not for Gemma 4 12B.

### Cited Findings

**Text-to-LoRA (Sakana AI, Jun 2025, arXiv 2506.06105)**
- Hypernetwork (L 55.3M / M 34.3M / S 4.9M parameters) maps a task description embedding (+ layer and module embeddings) to LoRA weights in one forward pass; trained on 479 Super-NaturalInstructions tasks by reconstruction (distil existing LoRAs) or SFT; base models Mistral-7B-Instruct (main), Llama-3.1-8B-Instruct, Gemma-2-2B-Instruct — [HTML](https://arxiv.org/html/2506.06105)
- Results: reconstruction matches task-specific LoRAs on 9 benchmarks (avg ~73.3–73.5 vs oracle 73.3; base 55.8); zero-shot on unseen tasks T2L-L 73.9 on 8 benchmarks vs multi-task LoRA 71.9, base 60.0, but task-specific LoRAs 75.8; sensitive to task-description quality; reconstruction-trained T2L doesn't generalise to unseen tasks; FLOPs estimate ~0.86 TFLOPs per instance vs ~4.18 TFLOPs for 3-shot ICL; no wall-clock latency given — [HTML](https://arxiv.org/html/2506.06105)
- Release: github.com/SakanaAI/text-to-lora (Apache-2.0); checkpoints on HF `SakanaAI/text-to-lora` (`gemma_2b_t2l`, `llama_8b_t2l`); demo needs > 16 GB GPU; each SFT training run ~5 days on one H100 — [GitHub](https://github.com/SakanaAI/text-to-lora)
- Note: T2L generates a behaviour/skill adapter from a task description; it is not designed to store facts or episodes.

**Doc-to-LoRA (Sakana AI, 13 Feb 2026, arXiv 2602.15902)**
- Design: Perceiver-style hypernetwork (8 cross-attention blocks, ~309M trainable parameters) reads a document in 8K-token chunks and emits a rank-8 LoRA per chunk on every MLP down-projection; chunk LoRAs are concatenated along the rank dimension for longer inputs; approximates context distillation in one forward pass — [HTML](https://arxiv.org/html/2602.15902)
- Target LLMs: gemma-2-2b-it main; Mistral-7B-Instruct-v0.2 and Qwen3-4B-Instruct-2507 in an appendix. Meta-training: ~5 days on 8 x H200 for the main model (NIAH variant ~33 h on one H200); must be retrained for each new target LLM — [HTML](https://arxiv.org/html/2602.15902)
- Speed (2WikiMultihopQA): D2L 0.209 s (batched) / 0.551 s (iterative) vs oracle context distillation ~40 s, CD with 5 generated queries ~73 s, with 25 queries ~465 s. Peak update memory D2L 11.5 / 3.8 GB vs CD 59.9–79.4 GB with generated queries; inference memory < 100 MB for the internalised doc vs ~1 GB extra for in-context (and > 12 GB extra at 128K tokens, D2L < 50 MB) — [HTML](https://arxiv.org/html/2602.15902)
- Quality: SQuAD 82.5% of the in-context upper bound (D2L 0.866 vs oracle CD 0.988 on a 100-sample table); 2WikiMultihopQA normalised 0.857 vs CD oracle 0.901; near-perfect NIAH to ~40K tokens (trained on 32–256-token inputs); abstract says > 4x the target model's native window — [HTML](https://arxiv.org/html/2602.15902), [abstract](https://arxiv.org/abs/2602.15902)
- Failure modes (authors): remaining gap vs ICL; interference when the later query is unrelated (SQuAD 0.126 vs base 0.175 without the doc); LoRA only; expensive meta-training — [HTML](https://arxiv.org/html/2602.15902)
- Release: github.com/SakanaAI/doc-to-lora (MIT), pretrained checkpoints on HF under SakanaAI, API `internalize(doc)`, `generate()`, `reset()`; only a Gemma demo checkpoint is named — [GitHub](https://github.com/SakanaAI/doc-to-lora)

**Drag-and-Drop LLMs (DnD, Jun 2025, arXiv 2506.16406)**: text encoder turns a batch of unlabeled prompts into condition embeddings, a cascaded hyper-convolutional decoder emits the full LoRA; claims up to 12,000x lower overhead than full fine-tuning and up to 30% average gain over the strongest trained LoRAs on unseen common-sense / math / coding / multimodal tasks; base/generator sizes not given on the abstract page — [abstract](https://arxiv.org/abs/2506.16406)

**Generative Adapter (Chen et al., Nov 2024, arXiv 2411.05877)**
- A ~500M adapter generator on a frozen LM emits low-rank updates (attention output projections) from context in one forward pass; Mistral-7B-Instruct v0.2 and Llama2-7B-Chat; streaming updates accumulate chunks into one adapter — [HTML](https://arxiv.org/html/2411.05877)
- StreamingQA (Mistral) F1 falls from 51.5 at 512 tokens to 32.0 at 32K, and prompting beats the adapter at long contexts (39.3 at 32K). Personalisation on Multi-Session Chat: full-conversation prompting 66.0 F1 at 2.059 TFLOPS vs adapter 40.2 F1 at 0.505 TFLOPS (about 4x less compute and storage, ~26 F1 lower). MetaICL avg 44.9. Needed SVD normalisation to be stable; no code link found — [HTML](https://arxiv.org/html/2411.05877)

### Inferences
- The only generated-adapter system that targets "remember this text" and is downloadable is D2L; with latency < 1 s it could, in principle, turn a character's recent journal into an adapter between dialogue turns, but at 2B–7B and with a ~18% quality loss vs reading the text, and it would need a new 5-day meta-training run for a Gemma-4-12B target (not done by anyone found).
- The dialogue-memory test (Generative Adapter on MSC, 40.2 vs 66.0 F1) is the closest published number to NPC memory: the adapter lost ~40% of the F1 of simply keeping the conversation in the prompt.
- T2L/DnD produce skills/styles (persona-like), not episodic recall.

### Gaps
- DnD base-model and generator sizes, code and weights not verified.
- No wall-clock latency for T2L; D2L latency is on gemma-2-2b only.
- No independent replication found of T2L, D2L, DnD or Generative Adapter. T2L README itself notes non-deterministic vLLM LoRA application.

---

## 5. Self-editing models (SEAL) and knowledge incorporation versus RAG

### Takeaway
SEAL lets the model write its own fine-tuning data ("self-edits") and trains the writing policy with RL; it lifts no-context SQuAD QA from 32.7% to 47.0% for a 7B model, comparable to training on GPT-4.1-written data, but each outer RL round costs ~6 h on 2 x H100, sequential edits cause forgetting, and RAG was not compared.

### Cited Findings
- Setup: Qwen2.5-7B for knowledge incorporation (SQuAD passages without context; Qwen2.5-3B also tested for scaling), Llama-3.2-1B-Instruct for ARC few-shot; self-edit = generated "implications" of the passage used for LoRA finetuning; reward = accuracy on questions after the update; 2 rounds of ReSTEM — [SEAL HTML](https://arxiv.org/html/2506.10943)
- Single passage (LoRA): base 32.7%; train on passage 33.5%; passage + base-model synthetic 39.7%; passage + GPT-4.1 synthetic 46.3%; SEAL after RL 47.0% — [SEAL HTML](https://arxiv.org/html/2506.10943)
- Continued pretraining on 200 passages: base 32.7, passage 36.0, +synthetic 50.6, +GPT-4.1 59.4, SEAL 58.2; on 2,067 passages: 29.0 / 31.2 / 43.4 / 49.2 / SEAL 46.4 (SEAL trails GPT-4.1 data and is only slightly above base-model synthetic data) — [SEAL HTML](https://arxiv.org/html/2506.10943)
- ARC few-shot: SEAL 72.5% vs ICL 0% vs un-trained self-edits 20% vs oracle TTT 100% — [SEAL HTML](https://arxiv.org/html/2506.10943)
- Cost: ~30–45 s per self-edit evaluation (finetune + test); one ReSTEM round (750 inner loops) ~6 h on 2 x H100; a cheap proxy reward (GPT-4.1 rubric) took ~5 min and reached 45.6% — [SEAL HTML](https://arxiv.org/html/2506.10943)
- Failure modes (authors): sequential self-edits degrade earlier passages ("remains susceptible to forgetting"; retention not optimised), reward loop expensive, needs paired downstream tasks — [SEAL HTML](https://arxiv.org/html/2506.10943); co-author says fixes under study (gradient orthogonalisation, memory buffers) — [search coverage](https://venturebeat.com/ai/self-improving-language-models-are-becoming-reality-with-mits-updated-seal)
- Code: project page https://jyopari.github.io/posts/seal — [SEAL HTML](https://arxiv.org/html/2506.10943)
- [independent-ish] Google's Sleep paper reports SEAL at 46.7 (single passage) and 43.2 (CPT n=200) in its own table, vs SEAL's 47.0 and 58.2 — a different protocol (and extracted via a summariser, so this discrepancy needs checking against both PDFs) — [Sleep HTML](https://arxiv.org/html/2606.03979)
- Comparison with RAG: SEAL's paper has no RAG baseline in what I read. Independent baseline evidence on fine-tuning vs RAG for new facts: "RAG consistently outperformed fine-tuning, both for knowledge the model saw during training and entirely new knowledge" and models struggle to learn new facts by fine-tuning unless facts appear in many phrasings — [Ovadia et al.](https://arxiv.org/abs/2312.05934); fine-tuning on unfamiliar facts is learned much more slowly and, once learned, linearly increases hallucination — [Gekhman et al.](https://arxiv.org/abs/2405.05904)

### Inferences
- SEAL is an RL recipe to train a model to generate good synthetic training data; the practical analogue for NPCs is a fixed pipeline "write synthetic Q&A about the character's events, then LoRA-train" (as in Dennis et al., section 6), without the RL loop.
- Knowledge-in-weights from a few passages is inefficient unless augmented with many rewrites; SEAL's gain comes from the rewrites, not from the passage itself (base 32.7% vs 33.5% when training on the passage alone, per the SEAL table above).

### Gaps
- No direct SEAL-vs-RAG number found. No 12B+ tests. No independent replication found of SEAL (the Sleep paper compares against it, with different numbers).
- VentureBeat's claim that SEAL "scales with model size" rests on an appendix experiment; not verified.

---

## 6. Complementary-Learning-Systems framing: fast episodic store + slow consolidation into weights ("sleep")

### Takeaway
2026 produced several explicit sleep/consolidation systems. Measured results exist for: (a) nightly LoRA consolidation of conversation facts into a 7B (80.4% retention vs 36.8% for cascading text compaction, vs 90.1% full-context ceiling; but self-reported, single-domain); (b) Google's Sleep (best SQuAD knowledge incorporation 48.9 vs SEAL 46.7, trained-from-scratch HOPE-style stack, no code); (c) a hobby project MEMIT-then-LoRA with a hard capacity cliff and a measured "alignment tax". No source compares consolidation to RAG head-to-head.

### Cited Findings

**Dennis et al. (23 May 2026, arXiv 2605.24657) "Beyond Inference-Only Deployment: Comparing Weight-Based Consolidation Against Cascading Compaction"** [authors only]
- Pipeline: three extraction passes ("reflection") per conversation with Claude Sonnet 4, ~20 synthetic two-message conversations per fact ("synthesis"), then LoRA (rank 16, alpha 32, on q/k/v/o projections, LR 2e-4, 8 epochs) on Qwen2.5-7B-Instruct; each cycle retrains a fresh adapter on all facts so far — [HTML](https://arxiv.org/html/2605.24657)
- Data/scale: 10 software-dev conversations, 816–979 facts per conversation (9,198 total), 183,393 training examples, 1,146 test questions (184 semantic, 154 procedural, 808 episodic) — [HTML](https://arxiv.org/html/2605.24657)
- Result after three cycles: overall retention consolidated 80.4 +/- 1.3% vs compaction 36.8 +/- 3.0% vs no-context floor 11.8% vs full-context ceiling 90.1%. By type (consolidated vs compaction vs ceiling): semantic 94.3 / 62.0 / 91.1; procedural 74.6 / 36.3 / 93.0; episodic 78.2 / 31.5 / 89.3 — [HTML](https://arxiv.org/html/2605.24657)
- Forgetting: accuracy flat at 80–82% across cycles, but this is cumulative retraining on all facts, not sequential overwrite; compaction fell 90.1% -> 43.2% after cycle one — [HTML](https://arxiv.org/html/2605.24657)
- Cost: ~1.5 h per conversation on one A100-40GB, ~8.2 h on an RTX 5090; ~$5 per user-night in cloud (Claude synthesis + rented A100) — [HTML](https://arxiv.org/html/2605.24657)
- Limits: only software-dev conversations; extraction/synthesis uses an external LLM API; no RAG baseline (RAG only mentioned in the intro); no MMLU/HumanEval/safety measurements — [HTML](https://arxiv.org/html/2605.24657)

**Google Research, "Language Models Need Sleep: Learning to Self-Modify and Consolidate Memories" (Behrouz, Hashemi, Javanmard, Mirrokni; arXiv 2606.03979; OpenReview since Sep 2025; blog 27 Oct 2026)** [authors only]
- Wake/sleep alternation. Sleep stage 1 "memory consolidation / knowledge seeding": the faster block's knowledge is distilled into the next slower block, which gets a new low-rank expert (only the new parameters are trained, the rest frozen), using on-policy distillation + RL imitation ("learning to imitate"); then the fast block's temporary low-rank experts are reset ("synaptic pruning"). Stage 2 "dreaming": RL-guided synthetic data generation, gradient-based filtering, SFT on top-rated samples — [Google blog](https://research.google/blog/the-sleep-framework-a-new-way-for-llms-to-retain-knowledge-in-continual-learning/), [paper HTML](https://arxiv.org/html/2606.03979)
- Numbers: SQuAD knowledge incorporation 48.9% (single passage) and 46.2% (continued pretraining) vs SEAL 46.7 / 43.2 in its table; without dreaming 35.7 / 36.2; ARC few-shot 80% vs SEAL 72.5%, TTT 10%, ICL 0%; math (Qwen3-8B) AIME-24 79.2, AIME-25 69.0, HMMT-25 46.1; claims near-perfect BABILong to 10M tokens (numbers not in text read) and "near-zero performance drop" on sequential Manchu/Kalamang translation while Cartridges and SFT forget — [Google blog](https://research.google/blog/the-sleep-framework-a-new-way-for-llms-to-retain-knowledge-in-continual-learning/), [paper HTML](https://arxiv.org/html/2606.03979)
- No explicit forgetting metric reported; no cost numbers for the sleep phase in the portion read; no code release statement — [paper HTML](https://arxiv.org/html/2606.03979)

**"Do Language Models Need Sleep? Offline Recurrence for Improved Online Inference" (Lee, McLeish, Goldstein, Fanti; arXiv 2605.26099, 25 May 2026)**: the model periodically converts recent context into persistent FAST weights in its SSM blocks (learned local rule), clears the KV cache, and runs N offline recurrent passes; more sleep passes -> better results on synthetic tasks (cellular automata, multi-hop graph retrieval) and a math task; it changes fast weights, not slow weights — [abstract](https://arxiv.org/abs/2605.26099)

**sleeping-llm (open-source hobby project, github vbario/sleeping-llm, MIT)** [single-author, single runs]
- Wake: write facts into weights with MEMIT; sleep: repair degraded edits (null-space constraints), then LoRA-train and fade the MEMIT scale 1.0 -> 0.5 -> 0.1 -> 0.0 stage by stage; claims unbounded lifetime capacity (design claim, untested) — [GitHub](https://github.com/vbario/sleeping-llm)
- Capacity cliff (Llama-3.1-8B): recall 0.92 up to 13 unconstrained edits, 0.57 at 14; hard cap 50 active edits; 70B/16-layer config runs out of VRAM at ~30 facts per session on 2 x H100 — [GitHub](https://github.com/vbario/sleeping-llm)
- Reported results: Llama-3.2-3B 4-bit, 15 facts, recall 0.60 (MacBook Air M3 8GB, ~5 min sleep); Llama-3.1-8B 14 and 30 facts recall 1.00 after sleep, perplexity drift +0.5% / +3.2%; Llama-3.1-70B NF4 60 facts recall 1.00 — [GitHub](https://github.com/vbario/sleeping-llm)
- "Alignment tax": RLHF suppresses LoRA-trained knowledge during wake chat: recall 47% (3B), 37% (8B), 0% (70B) — the reason LoRA is applied only in sleep; only synthetic person-city facts tested; no RAG comparison — [GitHub](https://github.com/vbario/sleeping-llm)

**Parameters as Agentic Memory / REMO (Tang et al., ICML 2026 workshop "Agents in the Wild")**: rebuilds overlapping dialogue windows, rewrites them in four views (intrinsic, cross-turn, network-structuring, interaction-oriented), trains LoRA adapters by SFT; reports outperforming retrieval-only and learned-memory baselines on LoCoMo, especially temporal reasoning (no scores or model sizes on the page) — [ICML page](https://icml.cc/virtual/2026/67803)

**TMEM — "Scaling Self-Evolving Agents via Parametric Memory" (Alibaba Qwen-Character, arXiv 2606.04536, 3 Jun 2026)**: agent keeps explicit memory and writes distilled QA-style supervision into fast LoRA weights during one episode (rank 6 on gate/up/down of the last 4 layers, SVD-initialised frozen A, trained B, SGD lr 5e-4, 5 epochs, triggered when context exceeds L_max) — [HF paper page](https://huggingface.co/papers/2606.04536)
- Qwen3-4B / 8B: LoCoMo F1 25.72 / 26.75 vs best explicit baseline 25.66 (A-MEM) / 24.26 (MemAgent); LongMemEval-S F1 41.24 / 41.87 vs 36.45 / 31.66; per-episode cost on Qwen3-4B: LoCoMo 1.53 s and 21.3 GB (retrieval A-MEM 2.03 s / 13.4 GB, summary 0.94 s / 11.2 GB) — gains are small on LoCoMo-4B and the paper calls some results "directional rather than decisive"; no code link — [HF paper page](https://huggingface.co/papers/2606.04536)

**Survey framing**: "Memory in the Age of AI Agents" (Hu et al., arXiv 2512.13564, v2 Jan 2026) classes memory forms as token-level, parametric (internal weights vs external adapters/LoRA) and latent (KV cache, hidden states); notes parametric memory adds no inference overhead but is costly to update and risks catastrophic forgetting — [survey summary](https://arxiviq.substack.com/p/memory-in-the-age-of-ai-agents), [arXiv listing](https://arxiv.org/abs/2512.13564v2)

**Related edit-based evidence**: MEMIT scales direct weight edits to "thousands of associations" in GPT-J (6B) and GPT-NeoX (20B) — [MEMIT](https://arxiv.org/abs/2210.07229). Knowledge capacity law: ~2 bits of knowledge per parameter (also at int8) — [Allen-Zhu & Li](https://arxiv.org/abs/2404.05405). LoRA "learns less and forgets less" than full finetuning on code/math; full finetuning learns perturbations of rank 10–100x higher than typical LoRA — [Biderman et al.](https://arxiv.org/abs/2405.09673)

### Inferences
- The most NPC-relevant measured data are Dennis et al. (nightly LoRA from structured synthetic Q&A on a 7B: 78% episodic retention vs 89% with the whole text in context) and sleeping-llm (a capacity cliff of ~13–14 MEMIT edits per 8B model, and chat-time suppression of LoRA knowledge by RLHF). Together they say: consolidation works for tens to hundreds of facts per adapter if you generate many paraphrased Q&A per fact, takes minutes–hours of GPU, and loses ~10 points against having the text in context.
- Rough arithmetic (mine, not in the paper): Dennis et al.'s ~18,000 examples x 8 epochs took ~1.5 h on an A100-40GB, so ~30 new facts (~600 examples) would be of the order of minutes on a similar GPU if time scaled linearly; a 12B model on a shared RTX 3090 is untested by anyone found.
- Google's Sleep/HOPE is conceptually the closest to "episodic fast store + consolidation" but is a pretrain-from-scratch architecture with no release.
- None of the consolidation papers test deletion, contradiction updates, or "NPC forgets/lies" semantics; Larimar-style write/unwrite is the only designed-in forgetting with numbers.

### Gaps
- No head-to-head of weight consolidation vs RAG/graph memory with matched budgets found (Dennis et al. and sleeping-llm explicitly lack it).
- Sleep (Google): cost, parameter counts, code unverified. TMEM, PAM/REMO: code not found.
- Dennis et al. is a single-group preprint (10 conversations). arXiv 2606.29916 "EVAF: A Test-Retest Protocol for Selective Parametric Consolidation" appeared in search but was not read.

---

## 7. Use for agents and game characters — has anyone done it?

### Takeaway
No source found stores a game NPC's episodic memory in weights. The nearest published NPC work puts persona in a per-NPC LoRA and memory in an external vector store. Agent-memory work (PAM/REMO, TMEM, Dennis et al., Locas) is evaluated on LoCoMo / LongMemEval / dev conversations, at 4B–8B.

### Cited Findings
- Fixed-Persona SLMs with Modular Memory (arXiv 2511.10277, Nov 2025): each NPC's persona is LoRA-fine-tuned from ~10–20 handwritten pairs (+ ~564 synthetic for a merchant) on DistilGPT-2, TinyLlama-1.1B or Mistral-7B-Instruct; memory = two ChromaDB stores per NPC (conversation, world knowledge), swapped at runtime in 12–27 ms with retrieval < 42 ms; Mistral-7B full-precision model: factual consistency 93%, context retention 100%, 5.49 s latency, ~4.2 GB VRAM on an RTX 2070 Super; the 4-bit quantised version was far slower (34.58 s); memory is not trained into weights; small LLM-judge evaluation (30–100 items) — [HTML](https://arxiv.org/html/2511.10277v1)
- Character-LLM (Shao et al., arXiv 2310.10158): trains models as specific people (Beethoven, Cleopatra, Caesar) from profiles rewritten as experiences ("experience reconstruction") and checks memorisation by interviews; abstract gives no numbers — [abstract](https://arxiv.org/abs/2310.10158)
- A search summary of a student/thesis report found per-NPC fine-tuned models taking ~30 min each and ~13% character-consistency gain from LoRA tuning on a small model (secondary, thesis not opened) — [search result](https://nlp.fi.muni.cz/aiproject/ui/adam_dominik2024/project-report.pdf)
- Search for "LoRA used to store per-character episodic memory" found nothing — [search](https://arxiv.org/pdf/2511.10277)
- Agent-memory work on weights (all at 4B–8B): TMEM (above), PAM/REMO (above), Dennis et al. (above), Locas on LoCoMo (above)

### Inferences
- The idea "one base model + pluggable per-character memory parts trained in the background" is untested for games; the closest published evidence is per-user nightly LoRA (Dennis) and per-episode fast LoRA (TMEM).

### Gaps
- No evidence on multi-character serving cost (many adapters), cross-character leakage, or players' perception of weight-memory NPCs.

---

## 8. Cross-cutting: serving constraints, comparison table, what is directly usable on llama.cpp + one RTX 3090

### Takeaway
Only adapter-style memory (LoRA made by gradient training, or by a hypernetwork) maps onto the existing local stack, because llama.cpp can load several LoRA adapters on a quantised base and set their scales at runtime; everything else (TTT/Titans/Memory Layers/Engram/LM2/Larimar/KV cartridges) needs custom training or serving and has no Gemma-class checkpoint.

### Cited Findings
- llama.cpp server: adapters loaded with `--lora` / `--lora-scaled file:scale`, `--lora-init-without-apply` loads them inactive; `GET/POST /lora-adapters` lists and sets global scales (scale 0 disables); a request may carry its own `lora` array overriding the global setting; requests with different LoRA configurations are not batched together; an open issue questions per-request adapter independence under concurrency; endpoint names are version-dependent — [guides/search result](https://www.simplified.guide/llama-cpp/server-set-lora-adapter), [issue #10374](https://github.com/ggml-org/llama.cpp/issues/10374)
- Typical tested model sizes across the surveyed papers: Titans 0.17–0.76B; HOPE 0.76–1.3B; TTT-E2E up to 3B; Memory Layers 0.13–8B (+128B memory params); sparse memory finetuning 1.3B; MemoryLLM/M+ 7–8B; Larimar 1.3B / 6B; LM2 1.7B; Memory Decoder base models 0.5–72B (decoder 0.1–0.8B); Cartridges 3B / 8B; KM 3B / 8B; T2L 2B / 7B / 8B; D2L 2B (+7B, 4B); Generative Adapter 7B; SEAL 7B (and 3B, 1B); Dennis et al. 7B; sleeping-llm 3B / 8B / 70B; TMEM 4B / 8B; Locas 0.6–4B. A 12B-class target is above everything except Memory Decoder (and the MEMIT/70B hobby result) — see sections above.
- General risk evidence: fine-tuning on unfamiliar facts raises hallucination ([Gekhman](https://arxiv.org/abs/2405.05904)); RAG beat unsupervised fine-tuning for knowledge injection ([Ovadia](https://arxiv.org/abs/2312.05934)); RLHF-tuned chat models suppressed LoRA-trained facts during chat ([sleeping-llm](https://github.com/vbario/sleeping-llm)); D2L adapters hurt unrelated queries ([D2L](https://arxiv.org/html/2602.15902)); mixed cartridges can collapse ([CAS](https://arxiv.org/abs/2606.04557)).

### Inferences — fact sheet (compact)

| Design | Where memory lives | Write | Read | Largest tested | Released? | Evidence |
|---|---|---|---|---|---|---|
| Titans / ATLAS / HOPE | MLP "fast weights" inside layers | gradient step per chunk at inference | forward through memory | 0.76B (Titans), 1.3B (HOPE) | no official code/weights; community re-impls | authors; Titans Revisited mixed |
| TTT-E2E | last-quarter MLPs | NTP gradient steps on 1K mini-batches | normal forward | 3B | code only | authors |
| Memory Layers / Engram | big KV tables in FFN slot | pretraining (SMF: sparse slot update) | top-k lookup / hash | 8B + 128B mem; Engram-27B | code only (CC-BY-NC) / demo | authors (Engram secondary) |
| MemoryLLM / M+ | latent token pool per layer | inject chunk, random-drop or LTM | cross-attn (+retriever) | 8B | weights (MIT) | authors |
| Larimar | 512x768 episodic matrix | one-shot pseudo-inverse (~1–1.7 s) | KV prefix | 6B | code only | authors |
| Memory Decoder | small decoder mimicking kNN | domain pretrain | logit interpolation | decoder 0.5B for base up to 72B | code + weights | authors |
| KBLaM | KB KV tokens | encode triples | rectangular attention | 8B, 10K triples | code | authors |
| Locas | side-FFN | backprop on streamed context | parallel to FFN | 4B | none found | authors |
| Cartridges | trained KV prefix | self-study training (~30 min/8xH100, 8B) | KV prefix | 8B | code (Apache-2.0) | authors; CAS: composition collapse |
| Knowledge Modules | LoRA per doc | DCD training | plug in | 8B | none found | authors |
| T2L / D2L / DnD / GenAdapter | LoRA made by hypernetwork | 1 forward pass (D2L 0.2–0.55 s) | plug in | 7–8B (D2L main 2B) | T2L, D2L: code + HF weights | authors |
| SEAL | LoRA from self-written data | RL-trained self-edit + SFT | normal | 7B | code | authors |
| Sleep (Google) / nightly LoRA (Dennis) | slow blocks / LoRA | distillation / LoRA retrain | normal | n/a / 7B | none / none | authors |

- For a 12B Gemma on a 3090, the options with a plausible path are: (1) per-character LoRA trained offline from synthetic Q&A about that character's events (Dennis recipe; SEAL/KM/PAM-REMO are variants), loaded via llama.cpp LoRA; (2) a D2L-like hypernetwork — needs meta-training for the target base; (3) research-only: Larimar/KBLaM/Locas-like plug-in memories, which require porting to Gemma.
- Evidence supports using weight memory as a slow, lossy, "feels-known" layer for stable facts, with exact recall of discrete facts still better served by text/graph retrieval (RAG > fine-tuning in independent tests; Dennis et al.: consolidated 78–80% vs full context 89–90%).

### Gaps
- Gemma 4 (12B) was not covered by any paper found; behaviour of its instruction tuning with LoRA memory (alignment tax) unknown.
- No source measured training LoRA on a quantised 12B on a 24GB card for this kind of memory; llama.cpp support for loading a trained KV-prefix artifact (Cartridge) not verified.
- Independent replications are rare: the only independent checks found are Titans Revisited (Titans), Cartridges at Scale (Cartridges composition), Ovadia/Gekhman/Biderman (general fine-tuning behaviour), plus Google's Sleep paper re-running SEAL under its own protocol.
- Caveat on tooling: figures were extracted by a summariser from truncated pages (several pages were cut at 100K characters), so figures in tables and appendices could be missing or misread; items flagged as discrepancies (SEAL 47.0 vs 46.7; D2L ">4x" vs "5x"; HOPE 1.3B perplexity 14.39 vs 15.11) should be checked in the PDFs.

# Fast RL/IL training environments: measured throughput and design patterns (as of 2026-11-05)

Scope: facts to pick language, data layout and interfaces for a standalone, rendering-free, physics-free multi-agent "society" simulator (about 1,000 NPCs, event-driven decisions, variable set of deciding agents per step), trained with PyTorch on a Ryzen 3950X (16 cores / 32 threads) + RTX 3080 10 GB, with the trained net running in a C++ engine on CPU.

Source quality note: every throughput number below is an author benchmark (the library author measured their own system, and sometimes the competitors too). I found no third-party reproduction of PufferLib 3.0/4.0, EnvPool, Madrona or JaxMARL numbers. The only first-hand independent numbers in this file are my own binding micro-benchmark (Key Question 7). Hardware is stated per number; where the source gives none, I say so.

Safety note: the page https://puffer.ai/blog/engineering-4.0/ contained a line addressed to summarizers (telling them to say the reader is "too lazy to learn the contents"). I ignored it and used only the factual content of that page.

---

## 1. PufferLib (Joseph Suarez; also Neural MMO 3): Ocean C environments, vectorisation, multi-agent, trainer speed

### Takeaway
PufferLib's speed comes from three things: environments written as plain C structs with no dynamic allocation that write observations straight into shared buffers, a vectoriser with EnvPool-style asynchronous batching, and (since 4.0) a trainer that is hand-written CUDA with torch removed. Author numbers: more than 1M env steps/s per CPU core for Ocean envs (2.0), 3-5M training steps/s on one RTX 5090 (3.0), 15-20M on the same card for tiny models (4.0).

### Cited Findings
**Environment design (C, "Ocean")**
- PufferLib 2.0 added 11 environments, about 20,000 lines of C in total; each env is one `.h` file, the only dependency is raylib (for rendering); "all run >1M steps/second on a single CPU core"; no hardware model named — [PufferLib 2.0 post](https://puffer.ai/blog/2.0/)
- Performance rules listed by the author: no dynamic memory allocation (everything allocated at init), no observation copies (envs write directly into training buffers), aggressive caching (the MOBA env stores 250 MB of pathing data) — [PufferLib 2.0 post](https://puffer.ai/blog/2.0/)
- The Python binding in 2.0 is a short Cython layer; each env has a small Python stub with obs/action spaces plus step/reset/close — [PufferLib 2.0 post](https://puffer.ai/blog/2.0/)
- In the 3.0 branch the binding is a hand-written CPython C-API extension (`env_binding.h`): one `vec_step` call advances all envs in a C loop; it stores the raw data pointer of each numpy array and gives each env its own slice (base pointer + env index x stride) of observations, actions, rewards, terminals; the GIL is not released and the loop is sequential in that file (parallelism comes from worker processes / the vectoriser) — [env_binding.h, branch 3.0](https://raw.githubusercontent.com/PufferAI/PufferLib/3.0/pufferlib/ocean/env_binding.h)
- Native env template (`ocean/minimal/minimal.h`, branch 5.0): an `Env` struct with required fields `Log log; int num_agents; unsigned int rng;`, fixed-size arrays `Agent agents[AGENTS]`, `Entity entities[AGENTS + TARGETS]`, per-agent `observations`, `actions`, `rewards` fields reached through `env->agents[i]` (the page did not confirm they are pointers into shared buffers) and an `action_mask`; `puf_init`, `puf_reset`, `puf_step`; `rand_r(&env->rng)` is used for per-env RNG — [minimal.h](https://github.com/PufferAI/PufferLib/blob/5.0/ocean/minimal/minimal.h)
- The docs say: obs/actions/rewards/terminals are "allocated as big chunks of memory" across all env instances; rewards and terminals must be zeroed at the start of `puf_step` ("a single memset will do it for multiagent envs"); obs buffers are not zeroed automatically — [PufferLib docs](https://puffer.ai/docs.html)
- 3.0: "PufferEnv C API ... native PufferEnvs write observations directly into shared memory batches with zero redundant copy operations"; Ocean grew by 10 first-party envs to 22; core training code is under 1,000 lines — [PufferLib 3.0 post](https://puffer.ai/blog/3.0/)

**Neural MMO 3 (large multi-agent env in C)**
- "runs millions of steps per second per cpu core"; written in C ("loops and conditionals"), started in Cython and later ported; "All the agents and NPCs are just structs in contiguous memory. There are no dynamic memory allocations."; example map 512x512 with 1024 agents and 2048 enemies; observation is a 2D crop of nearby tiles plus a self-vector; baseline ~1M-parameter CNN+LSTM trained on about 107B steps "in less than 3 days on a single RTX 4090" (that is at least 0.41M steps/s end to end, my arithmetic: 107e9 / 259,200 s) — [Neural MMO 3.0 post](https://puffer.ai/blog/nmmo3/)
- The page does not say how dead/respawning agents or changing agent counts are handled (see Gaps) — [Neural MMO 3.0 post](https://puffer.ai/blog/nmmo3/)

**Vectorisation (1.0 paper, still the core design)**
- Emulation: flatten structured obs to a numpy structured array (analog of C structs), flatten actions to one multidiscrete; the model unflattens in the first line of forward; "emulation layer typically adds a few tens of microseconds"; negligible for envs slower than a few thousand steps/s per core — [PufferLib 1.0 paper, arXiv 2406.12905](https://arxiv.org/pdf/2406.12905)
- Variable agent counts: "If the environment has a variable number of agents, PufferLib will pad observations to maintain fixed size data buffers"; multi-agent obs/actions are returned in canonical sorted order — [arXiv 2406.12905](https://arxiv.org/pdf/2406.12905)
- A Python implementation of EnvPool: simulate M envs, retrieve only N << M observations; M = 2N gives approximate double buffering (CPU steps half the envs while the GPU computes actions for the other half); M >> 2N avoids waiting on the slowest env — [arXiv 2406.12905](https://arxiv.org/pdf/2406.12905)
- Shared memory for obs/rewards/terminals/actions; workers busy-wait on an unlocked shared flag array instead of pipes/queues; pipes (up to 10x faster than queues) only for non-empty `infos`, about once per episode; multiple envs per worker stacked in preallocated arrays without copies; four separately optimised code paths (sync, async, one-batch-per-worker, zero-copy contiguous); an autotune utility — [arXiv 2406.12905](https://arxiv.org/pdf/2406.12905)
- Gymnasium and SB3 multiprocessing "experience significant scaling degradation above 1000 synchronizations per second per core" — [arXiv 2406.12905](https://arxiv.org/pdf/2406.12905)
- 3.0 (and docs, branch 5.0) vectorisation: envs split into chunks, each chunk tied to a rollout worker on its own CUDA stream; "Within each buffer, environment execution is parallelized with OMP threading"; each buffer asynchronously queues data transfers with pinned memory — [PufferLib docs](https://puffer.ai/docs.html)
- 4.0 engineering post: C-native multithreading backend using OpenMP; 3.0 ran chunks round-robin, 4.0 uses asynchronous rollout workers (a contributor's prototype gave "Immediate +2M sps"); pinned memory for obs buffers "roughly halved" CPU-to-GPU overhead — [Engineering 4.0 post](https://puffer.ai/blog/engineering-4.0/)

**Measured throughput, author benchmarks (1.0 paper; desktop D = 24-core i9-14900K + RTX 4090, laptop L = 6-core i7-10750H + RTX 3070; policy not in the loop)**
- Single-core SPS with emulation: Neural MMO (pre-3.0 version) 2.4k, NetHack 29k, Cartpole 270k, Ocean "Squared" 240k, Atari Breakout 1.2k, Crafter 320 — [arXiv 2406.12905](https://arxiv.org/pdf/2406.12905)
- Vectorised D / L: Cartpole PufferLib 460k / 110k, with EnvPool-style pool 3M / 200k, Gymnasium 82k / 14k, SB3 100k / 11k. Ocean Squared: 490k / 110k, pool 4M / 190k, Gymnasium 116k / 12k, SB3 99k / 12k. Neural MMO: 13k / 4.5k, pool 19k / 7.2k. Crafter (long resets, high step variance): pool is about 6x faster — [arXiv 2406.12905](https://arxiv.org/pdf/2406.12905)
- Even in the worst case (no model in the loop) the pool gives "at least a 30-40%" gain — [arXiv 2406.12905](https://arxiv.org/pdf/2406.12905)

**Training throughput by version (author numbers)**
- 2.0: "300k-1.2M sps on a single GPU, depending on the environment and policy"; "At 1M sps, simulation alone takes about 15 minutes per billion steps" and async sampling hides it — [PufferLib 2.0 post](https://puffer.ai/blog/2.0/)
- 3.0: "up to 4M steps/second on a single RTX 5090" — [PufferLib 3.0 post](https://puffer.ai/blog/3.0/). A search-result summary of the 3.0 post (not verified on the page itself) says ~150k-parameter MLP-LSTM at 2-4M sps, and Neural MMO with 2-3M-parameter nets at 500k-1M sps — [search summary of puffer.ai/blog.html](https://puffer.ai/blog.html)
- 4.0: "15M steps per second" standard model, "over 20M" smaller models, single RTX 5090; torch backend replaced by "5,000 lines of deterministic CUDA-C" (torch kept as fallback); MinGRU replaces LSTM (parallel over time) — [PufferLib 4.0 post](https://puffer.ai/blog/4.0/)
- 4.0 breakdown: 3-5M sps (3.0) -> about 7M with a cudagraphed C++ version -> about 13M after fused kernels -> 15M after removing torch -> 20M topline measured on Breakout with the hyperparameters that solve it fastest; author says other libraries reach "30-40M sps" only with settings that take longer to solve; CPU model not given — [Engineering 4.0 post](https://puffer.ai/blog/engineering-4.0/)
- Pokemon Red port: 7,000 steps/s with Clean PuffeRL on a single desktop, 2-3x the original SB3 project — [arXiv 2406.12905](https://arxiv.org/pdf/2406.12905)

### Inferences
- The recipe is portable to a C++ simulator: plain structs, fixed capacities, no allocation in the hot loop, one batched call per step, observation written directly into a numpy-visible buffer. For a 1,000-NPC sim the "env" is one instance with many agents, not 1,000 envs, so parallelism comes from running many sim instances (one per thread/process) and from the async "retrieve N of M" pattern.
- PufferLib's numbers are for envs whose agent decision is a fixed-size tiny observation. Our observation (entity attention over perhaps tens of entities) is orders of magnitude bigger per decision, so expect lower decisions/s than the 1M+ figures; the measurement to take first is cost of building one observation.
- PufferLib 4.0 shows the cost curve once the env is cheap: for small models the learner is dominated by launch overhead and memory-bound elementwise kernels, not FLOPs. For a 10^5-10^6 parameter net with attention + GRU, large batches, CUDA graphs / `torch.compile`, and fused elementwise ops matter more than env speed beyond about 10^5 decisions/s.

### Gaps
- No independent reproduction of the 3.0/4.0 numbers; hardware for 2.0 numbers is not named on the pages (the RLC 2.0 paper PDF could not be fetched, "Socket is closed", twice; a search summary attributes ">1M steps/s per core for 12 envs, some 10M, training 300k-1.2M sps on a single RTX 4090" to it, which I could not verify directly — [RLC 2025 paper](https://rlj.cs.umass.edu/2025/papers/RLJ_RLC_2025_151.pdf)).
- How Ocean/NMMO3 handle dead or variable agents, masks, and event-driven (only some agents act) stepping natively is not documented in the pages I could read; `minimal.h` only shows `num_agents` and `action_mask` fields.
- No PufferLib numbers for entity-attention / pointer-network policies or GRU-128 on a 3080-class card.
- CPU model and thread count are not given for the 2.0-4.0 numbers.

---

## 2. EnvPool: C++ thread pool, async batching, numbers on 12-16-core CPUs

### Takeaway
EnvPool's C++ thread pool plus asynchronous `send/recv` batching is about 3.1x (12-core laptop) to 4.2x (32-core workstation) faster than Python subprocess vectorisation on Atari, and about 2x even with a single env. The gap is largest when per-env step times vary, because async never waits for the slowest env.

### Cited Findings
- Design: three C++ components, `ActionBufferQueue`, `ThreadPool`, `StateBufferQueue`; pybind11 exposes the Python interface; `send` only puts actions into the queue and returns; workers execute and write results into pre-allocated blocks of `batch_size` states; when a block is full it is packed into numpy arrays and `recv` returns it — [EnvPool paper](https://arxiv.org/pdf/2206.10558)
- `batch_size < num_envs` gives asynchronous mode (max throughput); `num_envs == batch_size` is synchronous; the number of threads in the pool is kept limited to minimise context-switch overhead — [EnvPool paper](https://arxiv.org/pdf/2206.10558)
- Author benchmark, Atari Pong, highest FPS (frames, i.e. includes frameskip): 12-core laptop: for-loop 4,893, subprocess 15,863, Sample-Factory 28,216, EnvPool sync 37,396, EnvPool async 49,439. 32-core workstation: for-loop 7,914, subprocess 47,699, Sample-Factory 138,847, EnvPool sync 133,824, async 200,428. 96-core TPU-VM: async 359,559, NUMA+async 373,169. 256-core DGX-A100: subprocess 71,943, async 891,286, NUMA+async 1,069,922 — [EnvPool README](https://github.com/sail-sg/envpool)
- MuJoCo Ant: laptop 12 cores: subprocess 36,586, async 105,126; workstation 32 cores: subprocess 105,432, async 582,446; DGX-A100 256 cores: subprocess 163,656, NUMA+async 3,134,287 — [EnvPool README](https://github.com/sail-sg/envpool)
- "Even if we use a single environment in EnvPool, we can get a free ~2x speedup"; sync mode is limited by the slowest env instance; Python subprocess has "an almost flat curve" with more workers — [EnvPool paper](https://arxiv.org/pdf/2206.10558)
- NUMA note: one EnvPool per NUMA node removes contention on the `ActionBufferQueue` (relevant only for big multi-socket machines) — [EnvPool paper](https://arxiv.org/pdf/2206.10558)
- End-to-end: CleanRL PPO with for-loop envs 200 min vs 73 min with EnvPool sync, same sample efficiency; "Environment Step Time is a significant bottleneck under Python-level parallelization" — [EnvPool paper](https://arxiv.org/pdf/2206.10558)
- EnvPool sync trails Sample-Factory on several setups; async mostly wins — [EnvPool README](https://github.com/sail-sg/envpool)
- Each env must be ported to C++ and registered through pybind11 with unit tests (adding an env is real work); Linux MuJoCo needs EGL, Procgen needs Qt5 at build time — [EnvPool paper](https://arxiv.org/pdf/2206.10558), [EnvPool README](https://github.com/sail-sg/envpool)
- PufferLib's author calls his vectoriser "the only existing Python implementation of EnvPool", i.e. the async retrieve-N-of-M idea works without C++ thread pools — [arXiv 2406.12905](https://arxiv.org/pdf/2406.12905)

### Inferences
- EnvPool's environments are millisecond-scale (Atari with frameskip, MuJoCo). For microsecond-scale envs the Python-side synchronisation (>1000 syncs/s/core hurts Gymnasium/SB3 per PufferLib) matters more than the thread pool, which is why PufferLib uses shared flags and multiple envs per worker.
- For a 16-core / 32-thread Ryzen: a C++ thread pool that owns N simulation instances and exposes `recv(batch)` is the closest fit; the throughput ratio over Python subprocess is likely in the 3-4x range for slow step times and larger for fast ones (extrapolation from the 12- and 32-core numbers above, not measured on a 3950X).

### Gaps
- No EnvPool benchmark on a Ryzen 3950X or on environments with microsecond step times; no multi-agent or variable-agent results (EnvPool targets single-agent Gym APIs).
- All numbers are from the EnvPool authors.

---

## 3. Madrona (Stanford, ECS GPU batch simulation): steps/s and when GPU beats CPU

### Takeaway
Madrona runs thousands of environments as one ECS on the GPU. On an RTX 4090 it does 1.9M hide-and-seek steps/s (11x a strong ECS-based 32-thread CPU baseline, 270x the open-source reference), but a careful C++ ECS on 32 CPU threads already reaches 1.2-4.1M steps/s on simple game-logic envs. The GPU wins hardest where each step has lots of data-parallel work (physics, ray casts, pairwise observations); for tiny, branchy logic the advantage shrinks to 5x, and the sim then costs about as much as the policy.

### Cited Findings
- Design: Entity-Component-System with centralised storage: one column-store table per archetype across all environments, with an implicit `EnvID` column; deleted rows get `EnvID = -1` and are reclaimed by a radix sort on `EnvID` (which also improves memory coherence); tables are exposed to PyTorch as tensors aliasing the same GPU memory (zero-copy); the whole step is a task graph compiled into one GPU megakernel — [Madrona paper (SIGGRAPH 2023)](https://madrona-engine.github.io/shacklett_siggraph23.pdf)
- Dynamic entity creation/destruction is supported by adding table rows; archetypes must be declared up front and "adding and removing components dynamically from entities is not currently supported" — [Madrona paper](https://madrona-engine.github.io/shacklett_siggraph23.pdf), [Madrona README](https://github.com/shacklettbp/madrona)
- Hardware for all numbers: Intel Core i9-13900K (32 threads) + NVIDIA RTX 4090. Peak env steps/s (inference excluded, results in contiguous GPU tensors): HideSeek (32K envs) GPU 1.9e6, ECS-CPU 1.6e5, naive one-env-per-CUDA-thread GPU 4.5e4, open-source reference on CPU 6.9e3; Overcooked (64K envs) 4.0e7 / 1.2e6 / 1.1e6 / 9.7e3; Hanabi (128K envs) 2.1e7 / 4.1e6 / 4.1e6 / 1.3e4; Cartpole (1024K envs) 3.4e9 / 2.8e7 / 1.4e8 / 1.4e5 — [Madrona paper, Table 1](https://madrona-engine.github.io/shacklett_siggraph23.pdf)
- Ratios: GPU vs strong CPU baseline 11x (HideSeek), 33x (Overcooked), 5x (Hanabi), about 120x (Cartpole, 1M envs); reference implementations are 23x-320x slower than the authors' own ECS-CPU baseline, even the C++ Hanabi — [Madrona paper](https://madrona-engine.github.io/shacklett_siggraph23.pdf)
- "ECS-CPU" is single-threaded C++ ECS per env on each of the T threads with a work queue, which the authors describe as an implementation of the synchronous mode of EnvPool — [Madrona paper](https://madrona-engine.github.io/shacklett_siggraph23.pdf)
- Batch size sensitivity: HideSeek keeps more than 75% of peak down to 8K envs; GPU overtakes ECS-CPU at about 500 envs (HideSeek) and 200 envs (Overcooked); simple Cartpole needs about 512K envs to saturate — [Madrona paper](https://madrona-engine.github.io/shacklett_siggraph23.pdf)
- End-to-end: in a preliminary training pipeline, 54% of runtime is env generation + simulation + observation; policy inference + optimisation roughly balance the rest; 115B steps (all seven emergent phases in OpenAI hide-and-seek) estimated at about 1.5 days on one RTX 4090 vs 40 days to the early milestone on the original simulator on the 13900K — [Madrona paper](https://madrona-engine.github.io/shacklett_siggraph23.pdf)
- Agents per env: per-agent throughput rises until about 10 agents, then falls because pairwise observations are quadratic; a synthetic "Micro" env with no pairwise terms scales linearly until the GPU saturates at about 250 agents/env — [Madrona paper](https://madrona-engine.github.io/shacklett_siggraph23.pdf)
- Large batches may reduce sample efficiency, diminishing the benefit of fast simulation (paper cites McCandlish et al.) — [Madrona paper](https://madrona-engine.github.io/shacklett_siggraph23.pdf)
- Requirements: Volta or newer NVIDIA GPU, Linux, CUDA 12.4/12.8+; a CPU backend runs the same code "with no code changes" but is described as for debugging/visualisation — [Madrona README](https://github.com/shacklettbp/madrona)

### Inferences
- Our workload (rule-heavy, branchy, graph-of-waypoints, pointer-chasing, variable entity counts, few ms of work per decision) looks like Hanabi/Overcooked, not hide-and-seek: little intra-step data parallelism, so the Madrona evidence supports at most about 5x from a GPU, at the price of dropping virtual dispatch, dynamic structures and C++ engine code reuse. A fast CPU simulator is the lower-risk choice; GPU simulation is an option only if we later need 10^3+ concurrent worlds and the logic can be restated as fixed-shape systems.
- Madrona's "ECS-CPU" result is the most relevant datum: a data-oriented C++ implementation of the game logic (SoA tables, one instance per thread) is 23x-320x faster than a conventional reference and 1.2-4.1M env steps/s on a 32-thread desktop for small logic.

### Gaps
- No Madrona numbers for graph/waypoint sims or variable-size entity sets; no statement of determinism/seeding behaviour found.
- No Ryzen 3950X / RTX 3080 numbers (the 4090 has far more SMs and memory bandwidth).

---

## 4. JAX environments (Craftax, JaxMARL, Gymnax, Brax): throughput and fit for rule-heavy, variable-size sims

### Takeaway
JAX envs reach 10^7 steps/s only with 10^4 parallel envs on a datacentre GPU, and end-to-end PPO throughput of about 0.3-0.4M steps/s on a 4090 for a complex game (Craftax). They require static shapes, pure functions and `vmap` semantics that evaluate both sides of every branch, and their own authors list variable agent counts as an unsolved difficulty, so they are a poor fit for a graph-based, event-driven NPC society.

### Cited Findings
- Craftax (RTX 4090 + i9-13900K 24 cores/32 threads, 32 GB), steps/s averaged over the whole RL training process (not random actions), doubling workers until out-of-memory: Craftax-Classic 405,618 (4096 envs), Craftax 266,961 (4096), Procgen 7,638 (1024 workers, SB3 PPO + SubprocVecEnv), NetHack 5,628 (64), Crafter 1,580 (1024), MineRL 133 (8). Claim: PPO with 1 billion interactions finishes in under an hour on one GPU for Craftax-Classic — [Craftax paper, Appendix B](https://arxiv.org/pdf/2402.16801)
- Branching: in JAX "branching cannot occur inside a function that is parallelised with the vmap operator"; both branches must be evaluated and the result selected, which is why resets are the most expensive place and the authors use "optimistic resets" — [Craftax paper, Appendix C](https://arxiv.org/pdf/2402.16801)
- Compile time: about 30 s to render the first frame and 20 s to the first action in the Craftax play mode — [Craftax README](https://github.com/MichaelTMatthews/Craftax)
- JaxMARL, random-action steps/s on one NVIDIA A100 + AMD EPYC 7763 64-core, "Original, 1 env / JAX 1 env / JAX 100 envs / JAX 10k envs": MPE Simple Spread 8.3e4 / 5.5e3 / 5.2e5 / 4.0e7; Hanabi 2.1e3 / 1.4e3 / 1.1e5 / 5.0e6; Overcooked 1.9e3 / 3.6e3 / 3.0e5 / 1.7e7; StarCraft (SMAX 2s3z) 8.3e1 / 5.4e2 / 4.5e4 / 2.7e6; SMAX 27m_vs_30m 2.7e1 / 1.5e2 / 1.1e4 / 1.9e5 — [JaxMARL paper, Table 3, Appendix E](https://arxiv.org/pdf/2311.10090)
- Single JAX env on the GPU is often slower than the original CPU env (MPE 5.5e3 vs 8.3e4; Hanabi 1.4e3 vs 2.1e3): the speed only appears with batched envs — [JaxMARL paper](https://arxiv.org/pdf/2311.10090)
- Stated limitation: "inherent challenges with end-to-end JAX implementations, such as the difficulty in efficiently handling environments with a variable number of agents or those with massive observation sizes" — [JaxMARL paper](https://arxiv.org/pdf/2311.10090)
- Implementation constraint: "JAX's JIT compilation requires arrays to have static shapes, the total number of agents in an environment cannot vary during an episode"; the maximum agent count is fixed at instantiation and agents that terminate early pass dummy actions; `step(key, state, actions)` is a pure function with explicit state and PRNG key — [JaxMARL paper, Appendix](https://arxiv.org/pdf/2311.10090)
- JaxMARL training headline: about 14x faster than existing approaches in wall clock, up to 12,500x when many training runs are vectorised (the speed-comparison runs behind figures 3 and 5(c) used one of two RTX 4090s plus a 20-core Xeon Silver 4316) — [JaxMARL paper](https://arxiv.org/pdf/2311.10090)
- Brax: whole training loop (env rollouts + SGD) compiled into one jitted function; "hundreds of millions of steps per second" on a TPUv3 8x8 vs about thousands of steps/s for single-threaded gym MuJoCo Ant — [Brax paper](https://arxiv.org/pdf/2106.13281)

### Inferences
- The JAX speed story is about vectorising very many small identical worlds on an accelerator. The simulator would need padded fixed shapes (1,000 NPCs x max entities x max inventory), a waypoint graph as a dense matrix or fixed-degree adjacency, and branch-free update rules; event queues with variable length have no natural static-shape form. It also cannot be shared with the C++ engine, so the "identical encoder" requirement would need a second implementation.
- On a 3080 (10 GB) the batch of 10^4 worlds x 1,000 NPCs x per-NPC state would be memory-constrained; the datasheet numbers above come from A100/4090-class cards.

### Gaps
- No JAX benchmark of event-driven or graph-structured society simulations; Gymnax numbers not collected (not needed once the static-shape limitation is known).
- No measurement of JAX env speed on an RTX 3080.

---

## 5. Sample Factory and other trainers: where the bottleneck moves once the env is fast

### Takeaway
Across systems the bottleneck follows the slowest stage: env stepping and Python IPC first (EnvPool, PufferLib 1.0), then the sampler/policy-forward and data transfer (Sample Factory), then for small models the learner's kernel-launch and memory-bandwidth overhead (PufferLib 4.0), and when the sim is on the GPU, simulation and learning are about equal (Madrona 54% sim). Double buffering is the standard fix for the sampler stage.

### Cited Findings
- Sample Factory (APPO): "The sampling subsystem most critically affects the throughput ... since it is often the bottleneck"; rollout workers are thin wrappers around envs with no policy copy; policy workers are stateless (2-4 saturate the rollout workers); one learner; trajectories in shared memory; double-buffered sampling splits envs on a rollout worker into two groups so the CPU never idles while actions are computed, optimal when k/2 > ceil(t_inf / t_env) — [Sample Factory paper](https://arxiv.org/pdf/2006.11751)
- Sample Factory Table 1, System #2 = 2x Xeon Gold 6154 (36 physical cores) + one RTX 2080 Ti; env frames/s with frameskip 4, "pure simulation" vs systems: Atari: 181,740 sim only; Sample Factory 135,893 (74.8%); rlpyt PPO 68,880; SeedRL 39,726; RLlib IMPALA 22,440; DeepMind IMPALA 9,961. VizDoom: sim 322,907; Sample Factory 146,551 (45.4%). DMLab: sim 49,679; Sample Factory 42,149 (84.8%) — [Sample Factory paper](https://arxiv.org/pdf/2006.11751)
- "Both IMPALA implementations fail to efficiently utilize the resources in a single-machine deployment and hit performance bottlenecks related to data serialization and transfer" — [Sample Factory paper](https://arxiv.org/pdf/2006.11751)
- Sample Factory reports up to about 154k FPS (VizDoom battle PBT 8 agents 154,081; 4 agents 153,602; 72 rollout workers, 2,304 envs, System #3 = 2x Xeon Gold 6154 + 4x RTX 2080 Ti) — [Sample Factory paper, Table A.2/A.3](https://arxiv.org/pdf/2006.11751)
- Sample Factory abstract claim: throughput above 10^5 env frames/s on a single node with one GPU — [Sample Factory paper abstract](https://arxiv.org/abs/2006.11751)
- Current Sample Factory repo: sync and async regimes, a serial single-process debug mode, Isaac Gym / MuJoCo / VizDoom / DMLab-30 / Megaverse / Atari integrations; no benchmark numbers on the README — [Sample Factory repo](https://github.com/alex-petrenko/sample-factory)
- Megaverse (same author): batched GPU simulation at "more than 1,000,000 actions per second on a single 8-GPU node"; the abstract does not break down the bottleneck — [Megaverse abstract](https://arxiv.org/abs/2107.08170)
- PufferLib 4.0 bottleneck history: first Python overhead and "thousands of tiny kernels" in a flat profile; then memory bandwidth ("small kernels clogged it"); fixed by cudagraphs, fused ops and removing torch (3-5M -> 20M sps on a 5090) — [Engineering 4.0 post](https://puffer.ai/blog/engineering-4.0/)
- PufferLib 1.0: GPU idle time can be driven toward 0 with EnvPool-style async while the model computes — [arXiv 2406.12905](https://arxiv.org/pdf/2406.12905)
- Madrona: 54% of end-to-end time in env generation/simulation/observation for hide-and-seek on a 4090, the remaining about 46% in inference + optimisation — [Madrona paper](https://madrona-engine.github.io/shacklett_siggraph23.pdf)
- Brax/JAX: keeping env and learner in one jitted program removes data marshalling and network latency between "env" and "learner" processes — [Brax paper](https://arxiv.org/pdf/2106.13281)

### Inferences
- With our setup the likely order is: (1) observation building in the C++ sim (entity-attention features per decision), (2) policy forward for action selection at rollout (batching decisions across worlds is mandatory; a decision-point batch of 1 would be hopeless on GPU), (3) PPO update with a recurrent net (GRU-128 sequences) on a 3080.
- Variable-length per-agent GRU state is the unusual part: with event-driven decisions each NPC's sequence has gaps in wall time but not in decision index, so BPTT sequences can be cut per NPC by decision index; the training-throughput literature above has no direct analogue.
- Double buffering (k/2 > ceil(t_inf / t_env)) applies unchanged if each worker thread owns several sim instances.

### Gaps
- No source measuring a PPO learner on attention + pointer + GRU nets of 10^5-10^6 parameters; PufferLib's numbers are for MLP-LSTM / MinGRU nets of 150k-2M parameters on a 4090/5090.
- No data on RTX 3080 (10 GB) specifically.

---

## 6. Design patterns with evidence: SoA/ECS, zero-copy, padding vs variable batches, event-driven agents, determinism, snapshots, trajectory logs, replay

### Takeaway
The evidence converges on: contiguous column/struct-of-arrays storage with fixed capacities; observations written into a pre-allocated buffer that numpy wraps without copying; fixed-size padded agent/entity sets with masks (JAX and PufferLib do this by necessity); "only agents that act this step" expressed by the observation dict (RLlib) or an async retrieve-N-of-M queue (EnvPool/PufferLib); and state that is a plain copyable value (JAX) or a replayable action history (OpenSpiel) for snapshot and replay.

### Cited Findings
**Layout and zero-copy**
- Madrona: SoA column tables per archetype across all envs, tensors alias the same memory as the column store ("without requiring copies") — [Madrona paper](https://madrona-engine.github.io/shacklett_siggraph23.pdf)
- Madrona: sorting by env id gives cache-friendly access; per-env tables would give incoherent access and internal fragmentation — [Madrona paper](https://madrona-engine.github.io/shacklett_siggraph23.pdf)
- Naive vectorisation needs "two or three copies"; PufferLib's shared-memory design needs zero or one — [arXiv 2406.12905](https://arxiv.org/pdf/2406.12905)
- NMMO3: all agents and NPCs are structs in contiguous memory, no dynamic allocation — [Neural MMO 3.0 post](https://puffer.ai/blog/nmmo3/)
- Numpy structured arrays are "an analog to C structs" giving a flat-bytes view and a dict-like view of the same memory — [arXiv 2406.12905](https://arxiv.org/pdf/2406.12905)

**Fixed-size padded vs variable batches**
- PufferLib pads variable agent counts to fixed buffers; JaxMARL fixes the max agent count and uses dummy actions for terminated agents; Madrona instead keeps entity tables variable-length with row reclamation — [arXiv 2406.12905](https://arxiv.org/pdf/2406.12905), [JaxMARL paper](https://arxiv.org/pdf/2311.10090), [Madrona paper](https://madrona-engine.github.io/shacklett_siggraph23.pdf)
- PufferLib's multi-agent return order is canonical sorted order (a guard against hard-to-diagnose bugs) — [arXiv 2406.12905](https://arxiv.org/pdf/2406.12905)

**Event-driven agents (only some agents act)**
- RLlib `MultiAgentEnv`: only agents whose observations appear in the returned dict act on the next `step()`; the action dict must match the previously returned observation keys; reward/termination dicts may contain any agent at any step; agents can enter/leave (`possible_agents` vs current `agents`) — [RLlib docs](https://docs.ray.io/en/latest/rllib/multi-agent-envs.html)
- EnvPool async: `recv` returns "whichever envs finish first", `send(action, env_id)` targets specific envs — an event-driven interface on the env axis — [EnvPool README](https://github.com/sail-sg/envpool)
- Madrona handles per-step entity creation/destruction at the table level — [Madrona paper](https://madrona-engine.github.io/shacklett_siggraph23.pdf)

**Determinism, seeding, snapshot**
- JAX envs: `step(key, state, action)` pure function; the PRNG key and the full state are explicit values, so a snapshot is a copy of the state pytree and determinism is given by the key — [JaxMARL paper](https://arxiv.org/pdf/2311.10090)
- Ocean template keeps a per-env `unsigned int rng` field inside the env struct (`rand_r(&env->rng)`) — per-env deterministic seeding by construction — [minimal.h](https://github.com/PufferAI/PufferLib/blob/5.0/ocean/minimal/minimal.h)
- PufferLib 4.0's trainer is described as "deterministic CUDA-C" — [PufferLib 4.0 post](https://puffer.ai/blog/4.0/)
- OpenSpiel (C++ core, pybind11 bindings): `state.serialize()` returns a string from which the state can be rebuilt (by default the action history); `game.deserialize_state(...)` rebuilds it — replay-by-history as the snapshot mechanism — [OpenSpiel serialize docs](https://openspiel.readthedocs.io/en/latest/api_reference/state_serialize.html)

**Trajectory logging for imitation**
- Minari (Farama) stores each episode as its own group with observations, actions, rewards, terminations, truncations; optional `info`; extra per-episode datasets such as simulator state (qpos, qvel); Tuple/Dict spaces as nested groups; formats HDF5 and Arrow — [Minari dataset standards](https://minari.farama.org/main/content/dataset_standards)
- KataGo: C++ self-play writes input features and targets; Python `shuffle.py` writes npz; `train.py` trains; Python feature code is "NOT used during training because all of it has been computed by the C++ already" — [KataGo python README](https://github.com/lightvector/KataGo/blob/master/python/README.md)
- Rules of ML #29: log the features used at serving time and feed them into training; YouTube home page reported quality gains and simpler code after switching — [Rules of Machine Learning](https://developers.google.com/machine-learning/guides/rules-of-ml)

### Inferences
- Event-driven stepping maps to: the sim advances its own event queue until at least B NPCs sit at a decision point; the "env step" returns the observation tensors of exactly those NPCs (padded to a fixed entity count with a mask and a per-NPC id array); the next call supplies their actions. This is EnvPool's `recv(batch_size)` with decision points instead of finished envs, and RLlib's observation-dict-defines-actors rule in array form.
- Fixed padded entity slots + masks (like JaxMARL/PufferLib) suit both the PyTorch batch and a C++ inference library; variable-length-per-NPC lists can be packed into the same arrays with a count field.
- For snapshot/restore and replay into the full engine, the cheapest mechanism with evidence is OpenSpiel-style: seed + decision history, plus periodic full-state snapshots of the plain-struct state (possible because the state is a flat struct, as in NMMO3/Ocean).
- Imitation data (teacher decisions) can be generated by the same C++ sim and encoder (KataGo pattern) and written as encoded tensors + action + mask + id, so Python never re-encodes.

### Gaps
- No source on Ocean/Madrona save-load of full env state or on bit-exact determinism across thread counts.
- No example found of replaying a lightweight-sim trajectory into a full engine (closest: OpenSpiel history replay; Minari storing simulator state per episode).

---

## 7. Bindings: ctypes/cffi vs pybind11 vs nanobind overheads; batching

### Takeaway
Per-call overhead is 45-270 ns for scalar calls and 150-800 ns when an array is passed (own measurement), so it is irrelevant at one call per batched step, but extremely costly if each NPC decision is a separate Python call. nanobind is the fastest of the C++ binding options and ctypes the slowest; the largest hidden cost is creating the pointer/array wrapper in Python, not the call itself.

### Cited Findings
**Published benchmarks**
- nanobind docs (AMD Ryzen 9 7950X, boost off and clocks pinned, Ubuntu 22.04.2, clang++ 15.0.7, Python 3.10.6, nanobind 1.2.0): compile time about 2.7-4.4x faster than pybind11 (1.6-4.4x vs Cython); size-optimised binaries 3-5x smaller than pybind11; runtime call overhead about 3x lower than pybind11 for simple functions and about 10x lower when classes are passed around; similar to Cython; lower than an equivalent call made inside CPython itself — [nanobind benchmarks](https://nanobind.readthedocs.io/en/latest/benchmark.html)
- A pybind11 GitHub issue (known to me only through a search-result summary, page not fetched) says pybind11 has higher call overhead than some alternatives (releasing the GIL by default is costly in micro-benchmarks) and that it matters little when the C++ function does substantial work — [pybind11 issue 2760](https://github.com/pybind/pybind11/issues/2760)
- PufferLib's choice: hand-written CPython C-API extension, one `vec_step` call per step for all envs, raw numpy data pointers captured once at init — [env_binding.h](https://raw.githubusercontent.com/PufferAI/PufferLib/3.0/pufferlib/ocean/env_binding.h)
- PufferLib 1.0 found that emulation (flatten/unflatten via structured arrays) costs tens of microseconds per step and the critical code was "Cythonized and tested to be faster than a half dozen implementations ... including efforts to write it in C and Rust" — [arXiv 2406.12905](https://arxiv.org/pdf/2406.12905)
- EnvPool uses pybind11 and one batched `send`/`recv` per interaction — [EnvPool paper](https://arxiv.org/pdf/2206.10558)

**Own micro-benchmark (independent, quick, not tuned)**: Apple M1 Pro, macOS, Python 3.14.7, clang -O3, cffi 2.1.1, pybind11 3.1.0, nanobind 3.1.0, numpy 2.5.4, median of 7 runs of 500k calls (200k for array rows). Sources: `binding_bench/` next to this file (`bench.py`, `core.c`, `pb.cpp`, `nb.cpp`). This is a laptop, not the Ryzen; use as ratios.
- Plain Python function call 51.7 ns; ctypes `int add1(int)` 267 ns; cffi ABI mode 161 ns; cffi API (compiled) mode 92 ns; pybind11 67 ns; nanobind 45 ns — own measurement, [bench.py](binding_bench/bench.py)
- Passing a numpy array to a 1-element function: nanobind `ndarray` 158 ns, pybind11 `array_t` 265 ns, ctypes with a cached integer pointer 329 ns; getting the pointer with `arr.ctypes.data` on every call costs about 0.75 us (ctypes 1,104 ns, cffi cast 1,079 ns with it) — own measurement, [bench.py](binding_bench/bench.py)
- One call that fills 1000 floats in C: nanobind 237 ns, pybind11 334 ns, ctypes (cached ptr) 406 ns, i.e. 0.24-0.41 ns per element: the 150-400 ns of call overhead is already amortised to a fraction of a nanosecond per element at 1000 elements — own measurement, [bench.py](binding_bench/bench.py)
- Creating numpy views: `np.frombuffer(ctypes array)` 265 ns, `np.ctypeslib.as_array(ptr, shape)` 2,058 ns, ordinary slice view 87 ns — so create numpy views once at init and reuse them — own measurement, [bench.py](binding_bench/bench.py)

### Inferences
- Binding choice barely matters if the interface is "one call per batched step" with buffers allocated once and numpy views created once (PufferLib's pattern). Choose by ergonomics: nanobind (ndarray support, small binaries, fast compile) or a thin C ABI + ctypes/cffi if we want the identical library to also be loaded by the engine and by tests with no C++ binding dependency.
- Costs to avoid: per-NPC Python calls (1,000 NPCs x 50 decisions x ~0.3 us is still only 15 ms/day, but Python-side per-agent logic is what costs, not the call), `np.ctypeslib.as_array` in the loop, and `arr.ctypes.data` per call.

### Gaps
- My numbers are from an Apple M1 Pro with Python 3.14 and have not been repeated on the Ryzen/Linux target; the nanobind docs give ratios without absolute timings in the text; the other published comparisons I found (a blog giving ctypes 8.2 us, cffi 2.1 us, pybind11 0.08 us per call; a textbook giving 1-5 us for ctypes/cffi) have no stated methodology and disagree with my measurements, so I did not use them.

---

## 8. Training-serving skew: one encoder for the Python trainer and the C++ runtime

### Takeaway
The practice that works in production systems is to implement the feature/observation encoder once, in the runtime language, and have training consume its output (KataGo, OpenSpiel) or call it through bindings; the Python reimplementation is demoted to debug/inference tooling. Google's Rules of ML say the same: reuse code between training and serving, and log serving-time features.

### Cited Findings
- Rules of ML #32: reuse code between training and serving; gather everything into one system-specific object and run one common method to convert it to the model's format ("This eliminates a source of training-serving skew"); using different languages for training and serving makes code sharing nearly impossible — [Rules of Machine Learning](https://developers.google.com/machine-learning/guides/rules-of-ml)
- #29: log the features used at serving time and train on them; #37: measure skew, the third kind (next-day vs live data) "probably indicates an engineering error" when the same example gives different results — [Rules of Machine Learning](https://developers.google.com/machine-learning/guides/rules-of-ml)
- KataGo: the C++ self-play engine implements the board and writes the input features and targets; Python only shuffles to npz and trains; the Python board/feature modules are "NOT used during training"; they exist for debugging and queries to the raw net — [KataGo python README](https://github.com/lightvector/KataGo/blob/master/python/README.md)
- OpenSpiel: the game logic and `observation_tensor()` / `observation_tensor_shape()` live in C++ and are exposed to Python through pybind11 (`open_spiel/python/pybind11/pyspiel.cc`), so Python trainers see exactly the C++ observation — [OpenSpiel observation tensor docs](https://openspiel.readthedocs.io/en/latest/api_reference/state_observation_tensor.html), [OpenSpiel API reference](https://openspiel.readthedocs.io/en/latest/api_reference.html)
- PufferLib: observations defined once as a C struct / numpy structured dtype; the model unflattens them in the first line of forward, so the layout is a single declared schema shared by env and model — [arXiv 2406.12905](https://arxiv.org/pdf/2406.12905)
- PufferLib browser demos: a ~500-line `puffernet.h` loads PyTorch models so trained agents run in C/browser (same idea as our inference library) — [PufferLib 2.0 post](https://puffer.ai/blog/2.0/)
- PufferLib's own Ocean "minimal" env ships a `minimal_net.h` next to `minimal.h` (inference net in C beside the env) — [ocean/minimal listing](https://github.com/PufferAI/PufferLib/blob/5.0/ocean/minimal)

### Inferences
- Pitfalls the sources imply: (a) two independent encoders (C++ for engine, Python for training) drift; the fix is a single C++ encoder compiled into both the engine and the Python extension, with a golden test that encodes a corpus of saved world states and compares tensors bit-for-bit; (b) float formatting, normalisation constants and entity ordering must be part of the shared schema (PufferLib sorts agents canonically for this reason); (c) padding/mask conventions must be identical in training batches and in the engine's single-decision calls (batch of 1); (d) the trained-weights export format and the C++ inference library should be tested against PyTorch outputs on the logged serving-time observations (Rules #29/#37).
- A design that matches the evidence: sim + encoder + (optional) rule-based teacher in one C++ library `libsociety`; Python extension exposes `recv()/send()` batched arrays and a `encode_state(snapshot)`; the game engine links the same library for the encoder and runs its own inference lib; training logs are (encoded obs, mask, id, teacher action) tensors produced by C++.

### Gaps
- No documented case found of an RL project with exactly "same encoder in C++ engine and Python trainer" and a published list of pitfalls beyond the general Rules of ML; the KataGo and OpenSpiel examples are by inference from READMEs/docs, not postmortems.
- Not checked: Leela Chess Zero's data pipeline, ONNX Runtime numerics differences between PyTorch and a custom inference library.

---

## 9. Synthesis for the Gothic society stand (targets and design choices, inferences only)

### Takeaway
The evidence favours a C++ data-oriented simulator (plain structs / SoA, fixed capacities, no allocation in the hot loop) that owns the observation encoder, driven from Python through one batched async call, run in many CPU threads on the 3950X, with the GPU used only for the policy. A GPU/JAX simulator is not justified by the available numbers for a branchy, graph-based, event-driven sim.

### Cited Findings
(See sections 1-8 for the sources of each element.)
- Fast CPU simulators exist at 1M+ steps/s per core when logic is plain C structs (PufferLib 2.0, NMMO3) and 1.2-4.1M steps/s on a 32-thread desktop for a data-oriented C++ ECS (Madrona ECS-CPU baseline).
- GPU simulation gives 5x (Hanabi), 11x (hide-and-seek), 33x (Overcooked) over that strong CPU baseline on an RTX 4090 at 32K-128K envs.
- End-to-end PPO throughput reported by authors: 0.3-1.2M sps (PufferLib 2.0, one GPU), 3-5M (3.0, RTX 5090), 15-20M (4.0 tiny models, RTX 5090), 0.27-0.41M (Craftax JAX, RTX 4090), 0.41M+ (NMMO3, RTX 4090).

### Inferences
- Targets: a rule-based teacher run (for imitation) needs only sim + encoder + teacher, no GPU; set the first target as "10^6 logged decisions per minute per core" before optimising anything else (a placeholder derived from the 1M sps class of numbers, not a measured requirement). For PPO with 10^8 decisions: at 10^5 decisions/s end to end that is about 17 minutes of wall clock, at 10^4/s about 2.8 hours (plain arithmetic); the policy update on a 3080 will probably be the limiting stage, so measure policy forward + backward throughput on realistic batch shapes before investing in sim speed.
- Language: C++ for sim and encoder (shared with the engine); Python only as a thin binding (nanobind or C ABI), PyTorch for training. JAX/Madrona are ruled out by static-shape/fixed-archetype limits plus the requirement to share code with the engine.
- Data layout: per-world SoA tables of NPCs (needs, location, activity, inventory slots), smart objects, waypoint graph (CSR), property and task tables with fixed capacities; observation buffers pre-allocated as `[batch, max_entities, features]` plus mask and ids; worlds are independent so thread-level parallelism is trivial.
- Interface: EnvPool/PufferLib-style async: `recv()` returns up to B pending decisions (from any world), `send()` takes actions by decision id; worker threads own several worlds each so inference latency is hidden (Sample Factory double buffering rule).
- Determinism/replay: per-world RNG state in the world struct, decision log (world id, decision index, action) for replay; full-state snapshot = memcpy of the world struct if it is kept pointer-free.
- Binding: one call per batched step; create numpy views once; avoid per-agent Python work.

### Gaps
- Evidence quality: all benchmark numbers are from the libraries' authors (PufferLib/Suarez, EnvPool/Sea AI Lab, Madrona/Shacklett et al., Sample Factory/Petrenko, Craftax/Matthews et al., JaxMARL/Rutherford et al., Brax/Google); third-party reproductions were not found. Competitor numbers in those papers (SB3, Gymnasium, RLlib, rlpyt, OpenAI hide-and-seek) were also measured by the authors themselves.
- Nothing measured on the actual target hardware (Ryzen 3950X + RTX 3080 10 GB); nothing on entity-attention + pointer + GRU policies or on agent-based society simulations of this kind.
- Could not retrieve: the PufferLib 2.0 RLC 2025 paper PDF (socket closed), PufferLib native multi-agent/variable-agent docs, Madrona determinism/snapshot features, Leela Chess Zero pipeline.

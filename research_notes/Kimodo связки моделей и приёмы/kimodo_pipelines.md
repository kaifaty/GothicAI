# Kimodo: the pipelines practitioners build around it (state as of 2026-11-01)

Scope: which models and tools people chain to NVIDIA Kimodo (video-to-motion, physics tracking, LLM planning, DCC/engine integrations, retargeting, post-processing), what they report, and the licence of each piece, for choosing a pipeline for Gothic 2 combat animations (human Biped, 25 fps, RTX 3090, own retarget + Blender finish). Findings come from primary sources (NVIDIA repos, docs, model cards, licence texts) and community GitHub repos. Reddit, X and Discord gave almost nothing searchable (see the last section).

Baseline facts about Kimodo itself, used throughout:
- Kimodo is a kinematic motion diffusion model trained on about 700 h of optical mocap. It is controlled by text, full-body keyframes, sparse joint/end-effector constraints, 2D waypoints and dense 2D paths, and a two-stage denoiser (root, then body) that is meant to reduce floating and foot skating — [project page](https://research.nvidia.com/labs/sil/projects/kimodo)
- Models: Kimodo-SOMA-RP-v1.2 and SOMA-RP-ArdyRep-v1.2 (released 2026-10-15), SOMA-RP-v1.1 / SOMA-SEED-v1.1 (2026-04-10), G1 and SMPL-X variants (2026-03-16). Inputs and outputs use the SOMA 77-joint skeleton since 2026-03-19. v1.2 fixes BONES-SEED test sequences that had leaked into v1.1 training — [Kimodo README](https://github.com/nv-tlabs/kimodo)
- 30 fps, at most 10 s (300 frames) per generation. Model-card limitations: foot skating, does not always follow the prompt, weak outside its strong categories, cannot produce cartoon or physically implausible motion, **unaware of scene objects** — [HF model card](https://huggingface.co/nvidia/Kimodo-SOMA-RP-v1)
- About 17 GB VRAM with everything on the GPU; `TEXT_ENCODER_DEVICE=cpu` brings it under 3 GB. Tested on RTX 3090/4090/A100 — [Kimodo README](https://github.com/nv-tlabs/kimodo). A practitioner measured the diffusion model's peak at ≈2.5 GB, the rest being the Llama-3-8B text encoder. With the encoder on the CPU the run needs at least 20 GB free RAM — [kimodo-practical KIMODO.md](https://github.com/htdt/kimodo-practical/blob/main/KIMODO.md)
- The repo had 3,967 GitHub stars on 2026-11-01 (gh search) — [nv-tlabs/kimodo](https://github.com/nv-tlabs/kimodo)

## 1. Video-to-motion as a source of reference or constraints (GEM / GEM-X / GENMO and others)

### Takeaway
NVIDIA's commercially usable video model is **GEM-X** (SOMA output, Apache-2.0 code, NVIDIA Open Model License weights, HF card dated 2026-10-30). The older **GEM-SMPL / GENMO** is non-commercial and so excluded. NVIDIA documents no "video → keyframes/root path → Kimodo" pipeline. The only community example of GEM-X → SOMA → Kimodo-style clip path is Effigy (MIT), and it pulls in Meta's SAM 3D Body under the custom SAM License. Real-video accuracy is modest (EMDB-2 W-MPJPE ≈267 mm). Video is therefore better used as sparse keyframes or a root path than as a final motion.

### Cited Findings
- **GEM-X:** monocular-video 3D human pose and motion with moving cameras. It outputs per-frame body parameters in SOMA format plus a world-space global trajectory. Inputs are RGB video, a per-frame person bbox and per-frame camera intrinsics (or estimated ones). Single person only — [HF nvidia/GEM-X](https://huggingface.co/nvidia/GEM-X)
- GEM-X licence: source Apache-2.0, weights **NVIDIA Open Model License**, "ready for commercial or non-commercial use". Training data is NVIDIA in-house mocap rendered to synthetic video plus real visual data "under commercial-compatible licenses" (≈1 M images, 300 h video) — [HF nvidia/GEM-X](https://huggingface.co/nvidia/GEM-X)
- GEM-X repo: the default source licence is Apache-2.0. Some hand-processing files and the source-camera restoration function carry LicenseRef-OpenMDW-1.1. Weights, geometry assets and dependencies keep their own terms (ATTRIBUTIONS.md) — [NVlabs/GEM-X](https://github.com/NVlabs/GEM-X)
- GEM-X components and dates: Body Pose, Body Motion, and Hand Pose added 2026-10-21. Hand weights (EgoHand-v1) are access-controlled in a separate repo. Rendering needs Linux x86-64. Body Motion needs 77-point 2D keypoints — [NVlabs/GEM-X](https://github.com/NVlabs/GEM-X); [HF card](https://huggingface.co/nvidia/GEM-X)
- GEM-X quality: the HF card reports only W-MPJPE 100.2 mm on 5,000 held-out *synthetic* clips — [HF card](https://huggingface.co/nvidia/GEM-X). The repo reports real-video EMDB-2 Body Motion W-MPJPE₁₀₀ 267.20 mm, WA-MPJPE₁₀₀ 112.76 mm, RTE 2.45%, jitter 34.21, foot sliding 4.22 mm. Single-frame 3DPW MPJPE is 62.2 mm — [NVlabs/GEM-X](https://github.com/NVlabs/GEM-X)
- GEM-X hardware: tested on A100 80 GB. VRAM and speed are not stated. The temporal motion net has 46.8 M parameters, excluding the pose extractor, hand model and detectors — [HF card](https://huggingface.co/nvidia/GEM-X)
- **GENMO / GEM-SMPL:** renamed to GEM, code released October 2025, GEM-SMPL multimodal demo March 2026. It takes video, 2D keypoints, text and audio conditioning — [NVlabs/GENMO](https://github.com/NVlabs/GENMO). Licence: **NVIDIA OneWay Noncommercial License**, "non-commercially means for research or evaluation purposes only" — [GENMO LICENSE](https://github.com/NVlabs/GENMO/blob/main/LICENSE)
- Kimodo's own docs list GEM-X only as a "related project" for human motion reconstruction from video, with no pipeline — [Kimodo README](https://github.com/nv-tlabs/kimodo); [project page](https://research.nvidia.com/labs/sil/projects/kimodo)
- The Kimodo tech report says scaling with noisier video-derived data "may compromise motion quality" (an open challenge) — [Kimodo tech report](https://arxiv.org/html/2603.15546)
- Community GEM-X → Kimodo path: Effigy (MIT, created 2026-10-14) vendors GEM-X unmodified plus Meta's SAM 3D Body (under the SAM License). Its `mocap` step turns "a video of a person … into a motion on the SOMA skeleton, then a clip for our rig through the Kimodo path". It deliberately downloads the GEM-X SOMA model and "never `gem_smpl.ckpt`" — [Effigy engine/gemx README](https://github.com/SifuInTheShell/Effigy/blob/main/engine/gemx/README.md)
- Community GEM-SMPL → Kimodo path: engineAI-studio-pro adds `kimodo/imports/gem.py`, "NVIDIA GEM-SMPL (GENMO) video → AMASS SMPL-X → Kimodo motion tensors". It also pulls GVHMR regressors and YOLOv8x weights — both non-commercial or otherwise problematic for us — [gem.py](https://github.com/robofighter/engineAI-studio-pro/blob/main/kimodo/kimodo/imports/gem.py)
- SAM License (covers SAM 3 / SAM 3D, which GEM-X depends on per Effigy): a worldwide, royalty-free licence to use, modify and distribute. The licence must be passed on, and it bars military/warfare, nuclear, espionage and weapons uses, ITAR work and sanctioned parties. Meta may change the terms. This is a secondary summary, not the licence text — [Roboflow SAM 3 licence page](https://roboflow.com/model-licenses/segment-anything-3)

### Inferences
- For our dodges, rolls, barge and sword strikes, the practical way to use video is GEM-X on a self-recorded clip, keeping only the **root path and 3–10 key poses** (impact frame, wind-up apex, shoulder-contact frame). Those become Kimodo full-body or end-effector constraints, and Kimodo generates the mocap-quality motion between them. With ~267 mm world-space error, whole video sequences would carry visible jitter and drift. Kimodo's own guidance favours sparse constraints (< 20 keyframes per type, see §5).
- GEM-X → SOMA keeps us on Kimodo's native skeleton, so there is no extra retarget between video output and constraints (both SOMA). GENMO/GEM-SMPL and GVHMR-based paths must stay out because of their licences.
- The SAM License "weapons" clause is about weapons development, not depicting swords in a game. Legal should still read the actual text, since GEM-X imports SAM 3D Body for image features (per Effigy).

### Gaps
- No VRAM or speed figures for GEM-X on an RTX 3090 (only A100 80 GB is stated). It is unknown whether it fits in 24 GB alongside its detectors.
- No published example of GEM-X keyframes fed into Kimodo with measured quality. Effigy's results are not documented in what I read.
- No independent evaluation of GEM-X on fast martial or weapon motion (motion blur, self-occlusion).
- The GEM-X ATTRIBUTIONS.md and the actual SAM License text were not read directly.

## 2. Physics-based tracking / cleanup after Kimodo (ProtoMotions, MaskedMimic, PHC, GEAR-SONIC)

### Takeaway
NVIDIA's official Kimodo → physics path is **ProtoMotions** (Apache-2.0 code, NVIDIA Open Model License checkpoints). It converts Kimodo NPZ/CSV and trains or runs a tracking policy in IsaacLab/IsaacGym. It is aimed at robots and RL training, not at game-animation cleanup. Neither NVIDIA nor the community publishes success rates, skate numbers or style-loss numbers for tracking Kimodo output. Research says physics tracking removes sliding, floating and penetration, but trackers struggle with highly dynamic motion. The converter drops hands and fingers, and no pretrained model supports partner interaction (our riposte victim).

### Cited Findings
- The Kimodo README and project page say the NPZ (SOMA) and CSV (G1) outputs are ProtoMotions-compatible for "training physics-based policies". The G1 path was shown with GEAR-SONIC tracking in simulation as a "proof of concept" — [Kimodo README](https://github.com/nv-tlabs/kimodo); [project page](https://research.nvidia.com/labs/sil/projects/kimodo)
- The Kimodo tech report shows G1 motions "tracked by a physics-based humanoid policy trained with ProtoMotions" and deployed on a real robot. It gives no success rates, motion counts or failure modes — [tech report](https://arxiv.org/html/2603.15546)
- ProtoMotions Kimodo preparation: the SOMA NPZ converter keeps **only the 23 actuated joints** (robot `soma23`) and drops hands, fingers, face and toe ends. It computes heuristic contact labels from height and velocity thresholds and resamples fps (Kimodo 30 fps, ARDY 25 fps). Motions are then packaged into a MotionLib and a tracking policy is trained (`train_agent.py`) or run (`inference_agent.py`). The page does not describe exporting simulated motion back out — [ProtoMotions Kimodo preparation](https://nvlabs.github.io/ProtoMotions/getting_started/kimodo_preparation.html)
- ProtoMotions contents: MaskedMimic (one policy that completes motion from partial goals), the GPC latent prior, Metatron (MaskedMimic-style G1 policy, released 2026-10-28), and a General Tracking Policy trained on BONES-SEED (~142K motions) that transfers zero-shot to G1. Simulators are Isaac Lab, Isaac Gym, Newton and MuJoCo. Genesis was removed on 2026-10-28. Licence: Apache-2.0 code, NVIDIA Open Model License for NVIDIA checkpoints. No statement on foot skate or penetration — [NVlabs/ProtoMotions](https://github.com/NVlabs/ProtoMotions)
- Shipped checkpoints include a "SOMA BONES-SEED tracker" (continuous-action SOMA tracking, IsaacLab fine-tune only), a SOMA FSQ tracker, a SOMA GPC prior, and G1 trackers and Metatron. The page does not say whether any checkpoint tracks unseen motions zero-shot, does not mention partner interaction, and says "no pretrained Metatron SOMA policy" is included — [ProtoMotions pretrained models](https://nvlabs.github.io/ProtoMotions/getting_started/pretrained_models.html)
- ProtoMotions compute: reference runs are AMASS training in about 12 h on 4 A100s and BONES training on 24 A100s. IsaacSim 5.0+ is needed for high-fidelity rendering — [NVlabs/ProtoMotions](https://github.com/NVlabs/ProtoMotions)
- BONES-SEED (the trackers' training data) is gated behind the "BONES-SEED License" — [HF bones-studio/seed](https://huggingface.co/datasets/bones-studio/seed). Bones describes SEED as a curated subset with expanded datasets "available for commercial licensing" — [Bones press release, 2026-03-17](https://www.roboticstomorrow.com/news/2026/03/17/bones-studio-to-release-bones-seed-the-first-multimodal-motion-dataset-purpose-built-for-humanoid-robotics/26276/). A search snippet of the Bones site said SEED is open source "for academic use and qualifying startups" (seen only via search, not fetched) — [bones.studio/datasets/seed](https://bones.studio/datasets/seed)
- GEAR-SONIC / GR00T-WholeBodyControl: dual licence, Apache-2.0 code and NVIDIA Open Model License weights. It is robot (G1) whole-body control, not a SOMA game character — [GR00T-WholeBodyControl LICENSE](https://github.com/NVlabs/GR00T-WholeBodyControl); [SONIC paper](https://arxiv.org/html/2511.07820v2)
- PHC code is BSD-3-Clause-Clear, but "See assets/licenses for license information for assets", and its standard models are SMPL/AMASS-based — [ZhengyiLuo/PHC](https://github.com/ZhengyiLuo/PHC). (AMASS-trained models are already excluded by the brief.)
- Evidence that physics tracking fixes artefacts: CLoSD (ICLR 2025) runs a diffusion planner through an RL tracking controller. It notes that simulation "can also correct for nonphysical artifacts in the input target motion, such as floating, sliding, and penetration" — [CLoSD](https://arxiv.org/html/2410.03441v1). PhysDiff projects each denoising step through physics imitation and reports more than 78% improvement in physical plausibility — [PhysDiff](https://arxiv.org/abs/2212.02500v1)
- Evidence on the cost and failure modes of tracking: noisy kinematic references often make physics-based tracking fail (Ju et al., IJCAI 2023, seen as a search snippet, not read in full) — [IJCAI 2023 paper](https://www.ijcai.org/proceedings/2023/0105.pdf). FARM argues masked-modeling trackers "struggle with highly dynamic motions due to limited exposure to fast transitions" — [FARM](https://arxiv.org/html/2508.19926v1). On a high-dynamic LAFAN1 subset, OmniTrack reports 84.81% success versus BeyondMimic 70.04%, ExBody2 58.93% and OmniH2O 48.32% — [OmniTrack](https://arxiv.org/html/2602.23832). MaskedMimic tracks AMASS at 99.2–99.4% success, but arm-only tracking is weaker (95.5%) — [MaskAdapt](https://arxiv.org/pdf/2603.29272)

### Inferences
- A physics pass could in principle remove our 2–7 cm/frame foot slide and give the shoulder barge real contact. Its cost is large for an animation pipeline:
  - an IsaacLab install;
  - fine-tuning the SOMA tracker;
  - writing our own export of simulated poses back to NPZ/BVH (not documented);
  - re-adding the hands and fingers the converter drops;
  - modelling the sword as a body and a second (victim) character, which no pretrained model supports.

  Fast dashes and rolls are exactly the "highly dynamic" class trackers fail on most.
- Physics tracking also softens the high-frequency, stylised parts of combat (snap and overshoot). No source quantifies this for Kimodo, so it stays a risk, not a measured fact.
- For a 25 fps game with a handful of hero moves, deterministic kinematic cleanup (Kimodo MotionCorrection, then foot-lock IK, then Blender) is far cheaper and better documented than a physics pass (see §5). A physics tracker is worth trying only for the barge, where contact force matters, and as an experiment on the Ryzen/dev-ubuntu machines.

### Gaps
- No published tracking success rate, skate or MPJPE figures for Kimodo motions in ProtoMotions, and no style-loss measurement.
- It is unclear whether the SOMA BONES-SEED tracker tracks new motions zero-shot or must be fine-tuned per set.
- The BONES-SEED licence text is gated. It is unclear whether it constrains commercial use of trackers trained on it, beyond the NVIDIA Open Model License on the weights.
- IsaacLab on an RTX 3090 for SOMA tracking: memory and time were not found.

## 3. LLM planning: decomposing actions, generating constraints, agentic tools on Kimodo

### Takeaway
Kimodo itself needs decomposition: one or two behaviours per prompt, at most 10 s, self-contained prompts per segment, and the second segment spends its start on the transition. Agentic wrappers exist and are Apache-2.0: kimodo-mcp (an MCP server for coding agents with generate → QC → re-roll → FBX) and Animatica's MMCP server. DCC tools expose "timed prompt beats" or blocks (Mocara, Animatica). Research on LLM keyframe planning (Re2MoGen, CoMA) exists, but none of it is built on Kimodo. NVIDIA used an LLM only to paraphrase training captions.

### Cited Findings
- Kimodo best practices:
  - start prompts with "A person…";
  - keep one or at most two behaviours per prompt and split longer actions into several prompts;
  - use medium detail;
  - in multi-prompt timelines each prompt must carry its own context ("Then the person stops" fails);
  - multi-prompt segments are generated separately, the next one conditioned on the previous one's last frames, so "the second prompt must spend part of its duration on that transition";
  - training covers locomotion, gestures, everyday activities, common object interactions, **videogame combat**, dancing and styles, and actions outside these categories give poor results (example: a baseball bat swing).

  [Kimodo Best Practices](https://research.nvidia.com/labs/sil/projects/kimodo/docs/key_concepts/limitations.html)
- The Kimodo tech report used Qwen3-32B only to paraphrase motion captions into an "A [subject]…" structure for training. There is no LLM at inference. Multi-prompt sequencing is done by overlap and keyframe constraints — [tech report](https://arxiv.org/html/2603.15546)
- **kimodo-mcp** (Apache-2.0, 2026-08-10, 1 star): an MCP server giving Claude Code or any MCP client 17 tools. `kimodo_generate`/`batch_generate` run as background jobs. `kimodo_make_constraints` authors 2D root paths and waypoints with headings without the demo UI. `kimodo_inspect_motion` checks foot skate during contact, ground penetration, jitter, root speed and loopability. `kimodo_export_fbx` drives headless Blender BVH → FBX for Unreal/Unity. It ships "23 tuned game-animation presets" including dodge roll and melee combo, and supports a low-VRAM offload path (6 GB cards) — [wifimetas/kimodo-mcp](https://github.com/wifimetas/kimodo-mcp)
- **motionmcp-kimodo** (Apache-2.0, alpha): a self-hosted MMCP (Motion Model Communication Protocol) server wrapping Kimodo SOMA models with root-path, effector and pose-keyframe constraints and glTF responses. Its reference client is Proscenium for Blender (GPL-3.0). First install takes 30+ minutes because the MotionCorrection C++ build is "a common install blocker" — [animatica-ai/motionmcp-kimodo](https://github.com/animatica-ai/motionmcp-kimodo); [proscenium-blender](https://github.com/animatica-ai/proscenium-blender)
- kimodo-practical (MIT) is "written to be followed end-to-end by an autonomous coding agent (or a human)". It includes `ANIMATION_AGENT.md`, a one-page guide on which control to use when: text, pose, end-effector, waypoints, path or combinations — [htdt/kimodo-practical](https://github.com/htdt/kimodo-practical)
- Mocara (UE 5.8, Apache-2.0, beta 0.3.0): "1–16 timed prompt beats · seed · guidance", one to four sequential candidates, and pose keys sent back through Kimodo as constraints — [Conalh/Mocara-Unreal](https://github.com/Conalh/Mocara-Unreal)
- cc8887/Kimodo-Pipeline drives Kimodo from a production dependency graph of moves with per-scope prompt rules (JSON). A `refine_mantle.py` "automatically adds sparse conditions over failed intervals" and re-delivers. Licence: **PolyForm Noncommercial 1.0.0** for its own code — [cc8887/Kimodo-Pipeline](https://github.com/cc8887/Kimodo-Pipeline)
- Research on LLM planning, not Kimodo-based:
  - Re2MoGen (April 2026) uses an LLM plus MCTS to plan root and key-joint keyframes; a pose model optimises full-body keyframes and a pretrained motion model completes the sequence — [Re2MoGen](https://arxiv.org/html/2604.17807v1);
  - CoMA splits text into temporal segments, each with base generation plus local edits — [CoMA](https://arxiv.org/pdf/2412.07320);
  - an LLM planner into atomic actions reports quality "highly sensitive to the underlying LLM's reasoning power" — [ICMPG](https://www.emergentmind.com/papers/2606.26981).

### Inferences
- For combat, "LLM planning" is mostly decomposition plus constraint authoring. An LLM (Claude) can turn a move spec into (a) one prompt per phase, (b) Kimodo constraint JSON — root waypoints with distance and timing for dashes, end-effector targets for the sword hand at wind-up, impact and follow-through, and full-body stance bookends — and (c) world-space acceptance gates. kimodo-mcp and kimodo-practical are ready templates for (b) and (c).
- Re-rolling with gates (best-of-N) gives more than clever prompting: the prior is conservative, and a multi-phase "combo" in one prompt is unreliable (see §6).

### Gaps
- No public report of an LLM-generated end-effector constraint set for weapon strikes with measured success.
- No agentic tool handles two-character (attacker plus victim) planning for Kimodo.

## 4. Integrations (Omniverse, Isaac, Unreal, Unity, Blender, Maya) and retargeting of SOMA output to game skeletons

### Takeaway
There is no official NVIDIA DCC or engine plugin. Kimodo ships only a CLI, Python API, interactive demo and NPZ/BVH/CSV export. Community bridges exist for Blender, Unreal, Unity, Houdini, MotionBuilder, Godot, ComfyUI and Roblox, with mixed licences (Apache-2.0, GPL-3.0, none). NVIDIA's SOMA Retargeter and GMR target robots, not game skeletons. Game-skeleton retargeting is done by community code. The best-documented game retarget is kimodo-practical's certified position-based retargeter, which documents SOMA rest-pose and wrist traps directly relevant to our free-rotating sword wrist.

### Cited Findings
- Kimodo export: NPZ (77 joints for SOMA), BVH for SOMA only (`--bvh`; rest pose defaults to the BONES-SEED convention, or `--bvh_standard_tpose`), G1 MuJoCo CSV, and SMPL-X AMASS NPZ. FBX is not mentioned. The model runs internally on `somaskel30` and expands to `somaskel77` on export — [Kimodo output formats](https://research.nvidia.com/labs/sil/projects/kimodo/docs/user_guide/output_formats.html)
- Issue "BVH export produces non-standard rest pose" (#15, closed 2026-03-31) — [kimodo#15](https://github.com/nv-tlabs/kimodo/issues/15). The kimodo-practical "rest-pose trap": in NPZ the T-pose is the identity for every joint, while the bundled `standard_t_pose_global_offsets_rots.p` is a different (BVH) convention. Baking it as the rest pose "shipped as visibly skewed fists and toes-up feet". The SOMA T-pose hand bends ~18° off the forearm axis while game rigs bind hands straight, and the T-pose toe droop (~14°) must not be "levelled" — [kimodo-practical KIMODO.md](https://github.com/htdt/kimodo-practical/blob/main/KIMODO.md)
- kimodo-practical findings on wrist and forearm:
  - aim-based limb transfer under large torso pitch made the forearm "spin in place", up to 45°/frame of twist; the fix rebuilds the forearm from the source forearm's full world delta;
  - raw mocap wrist channels read as a "broken" wrist on game hands;
  - the fix is a `handFollow` slerp, baked at **0.3** for prediction-only clips and **1.0** where hand constraints were authored;
  - measured, 0.3 discards 22.8° mean / 60.8° p95 of real wrist articulation.

  [kimodo-practical KIMODO.md](https://github.com/htdt/kimodo-practical/blob/main/KIMODO.md)
- kimodo-practical overall (MIT, 2026-07, "validated by building a complete two-player fighting game from generated motion"):
  - Stage 1 certifies each character rig with a probe battery and gates;
  - Stage 2 generates on the canonical skeleton with best-of-N and QA gates;
  - Stage 3 plays baked clips with root motion;
  - a deterministic analytic two-bone constraint IK lands authored hand and foot targets on the final rig (p95 gates 2 cm / 5°) and holds foot keys across predicted contact runs "so the IK never slides a planted foot".

  [htdt/kimodo-practical](https://github.com/htdt/kimodo-practical)
- NVIDIA SOMA Retargeter (Apache-2.0, beta): SOMA BVH → humanoid **robot** CSV (G1, H2, T1, AgiBot) via IK per frame, contact stabilisation and joint-limit clamping. It has docs on contact detection and foot-plant correction. It is not a game-skeleton tool — [NVIDIA/soma-retargeter](https://github.com/NVIDIA/soma-retargeter)
- GMR (MIT) retargets Kimodo-SMPLX AMASS NPZ to robots — [Kimodo README](https://github.com/nv-tlabs/kimodo); [YanjieZe/GMR](https://github.com/YanjieZe/GMR). Kimodo-SMPLX-RP uses the more restrictive **NVIDIA R&D Model License**, and kimodo-practical advises using the SOMA models — [Kimodo README](https://github.com/nv-tlabs/kimodo); [kimodo-practical KIMODO.md](https://github.com/htdt/kimodo-practical/blob/main/KIMODO.md)
- Blender:
  - Kimodo Blender Bridge (lewdineer): background Kimodo process, auto-match bone names, per-bone retarget mode, bake; no licence file in the repo; also sold on Superhive — [GitHub](https://github.com/lewdineer/Kimodo_Blender_Bridge); [Superhive](https://superhivemarket.com/products/kimodoblenderbridge);
  - blender-kimodo-motion (Xingxun7777, Blender 5.0+, FBX-SDK retarget, licence NOASSERTION) — [GitHub](https://github.com/Xingxun7777/blender-kimodo-motion);
  - its fork kimodo_motion (atticus-lv, GPL-3.0, macOS/Metal and in-Blender retarget) — [GitHub](https://github.com/atticus-lv/kimodo_motion);
  - kimodo_motion_cn (NLA actions, no FBX SDK needed) — [GitHub](https://github.com/aibalabala/kimodo_motion_cn);
  - Animatica Choreographer (GPL-3.0, Blender 5+, cloud by default, self-host possible; prompt blocks on the timeline, key poses, waypoints, hand/foot pins, variations, loop and in-place) — [animatica-blender-plugin](https://github.com/animatica-ai/animatica-blender-plugin);
  - DevBud "Ai Animation Forge" (paid, constraint-based retarget setup) — [Gumroad](https://devbud.gumroad.com/l/AnimationForge).
- Unreal:
  - Animotive Kimodo (Fab; cloud with a free tier or self-hosted Docker; output to Content Browser and Sequencer) — [Fab listing](https://www.fab.com/listings/c746ffa2-7d6c-4a85-b0d8-eec7090636f8); [docs](https://kimodo.animotive.com/docs);
  - Kimodo Importer (Fab, BVH → UE5 skeleton retarget, does not include Kimodo) — [Fab](https://www.fab.com/listings/c77fd26e-39fe-4355-b607-cce8c37de491);
  - Mocara (Apache-2.0, UE 5.8, Windows + WSL2): retargets onto the UE5 mannequin or MetaHuman with a `FootPlantingStrength` control, local AutoPose that bakes keyed corrections without another model call, and a **Two-Hand Grip** relationship constraint for "sword-, rifle-, or tool-" holds; other skeletons need a validated target profile — [Mocara-Unreal](https://github.com/Conalh/Mocara-Unreal);
  - a Kimodo Motion Studio thread on the UE forum — [forum](https://forums.unrealengine.com/t/kimodo-motion-studio-text-to-animation-plugin-for-unreal-engine/2713432).
- Unity: KimodoUnityBridge (Apache-2.0, 113 stars) — [GitHub](https://github.com/OneYoungMean/KimodoUnityBridge); Kimodo-Unity (Apache-2.0, bakes to AnimationClip on any Humanoid) — [GitHub](https://github.com/Amin-HP/Kimodo-Unity); a showcase issue in the Kimodo repo — [kimodo#47](https://github.com/nv-tlabs/kimodo/issues/47).
- Other bridges:
  - Houdini (no licence) — [chordee/kimodo-houdini-bridge](https://github.com/chordee/kimodo-houdini-bridge);
  - MotionBuilder with HumanIK characterisation (no licence) — [Asukacodes/kimodo-mobu-bridge](https://github.com/Asukacodes/kimodo-mobu-bridge);
  - Godot via kimodo.cpp (Apache-2.0) — [shiena/godot-kimodo](https://github.com/shiena/godot-kimodo);
  - ComfyUI (no licence) — [jtydhr88/ComfyUI-Kimodo](https://github.com/jtydhr88/ComfyUI-Kimodo);
  - Roblox R15 retarget — [Chrrxs/kimodo-r15-retarget](https://github.com/Chrrxs/kimodo-r15-retarget);
  - a C++/GGML port, kimodo.cpp (Apache-2.0, 946 stars) — [localai-org/kimodo.cpp](https://github.com/localai-org/kimodo.cpp).
- Omniverse/Isaac: no Omniverse plugin was found. Isaac appears only through ProtoMotions (IsaacLab/IsaacGym) — [NVlabs/ProtoMotions](https://github.com/NVlabs/ProtoMotions). An NVIDIA developer blog (2026-04-30) frames Kimodo as a research project for prototyping behaviours, creating variations and "fill[ing] gaps between authored or captured animation clips". It names no engine integration — [NVIDIA developer blog](https://developer.nvidia.com/blog/?p=116210)

### Inferences
- For a Biped (Gothic) skeleton, no off-the-shelf retarget exists. Our own script is the right call. kimodo-practical's three traps (rest-pose convention, the 18° hand bend, forearm roll under torso pitch) and its `handFollow` damping map directly onto our "sword wrist rotates freely" problem. Damp the predicted wrist, and drive the sword hand from authored end-effector rotations at key frames.
- GPL-3.0 add-ons (Animatica, atticus-lv) are fine as internal tools, since their output animations are not GPL code. Repos with no licence (Kimodo Blender Bridge, Houdini, ComfyUI) cannot legally be copied into our code.

### Gaps
- No integration exports straight to a Gothic/ZenGin Biped (MDS/MSB/MAN) — expected.
- Animotive's pricing, licence and capabilities beyond the Fab blurb were not visible (GitBook docs not fetched).
- No official Blender or Maya plugin from NVIDIA was found. A search summary claimed an NVIDIA blog promised them, but the blog post I fetched does not say so. Treat that claim as unverified.

## 5. Post-processing practice: foot contact and IK cleanup, smoothing, retiming, in-betweening

### Takeaway
Kimodo's built-in post-processing (the C++ `motion_correction` package, on by default, `--no-postprocess` to disable) uses the model's predicted foot contacts to lock feet and corrects root and rotations so constraints land exactly. Practitioners keep it on: kimodo-practical measured 0.003–0.03 m/s skate after correction and exact hits on keyframe and end-effector targets. Beyond that, practice is best-of-N sampling with hard world-space gates, contact-held IK on the target rig, retiming in-engine rather than forcing short durations, and in-betweening via Kimodo keyframes or NVIDIA MotionBricks/ARDY.

### Cited Findings
- Post-processing is on by default and can be disabled with `--no-postprocess` — [Kimodo README](https://github.com/nv-tlabs/kimodo). Best practices: "If foot contact and constraint accuracy matter most, enable post-processing". The model alone "can produce foot skating and won't exactly hit constraints". Post-processing "works poorly on the G1 robot skeleton" — [Best Practices](https://research.nvidia.com/labs/sil/projects/kimodo/docs/key_concepts/limitations.html)
- API: `post_process_motion(local_rot_mats, root_positions, foot_contacts, skeleton, constraint_lst, contact_threshold=0.5, root_margin=0.04)` returns corrected rotations, root and posed joints — [Post-Processing API](https://research.nvidia.com/labs/sil/projects/kimodo/docs/api_reference/post-processing.html)
- Source: it calls `motion_correction.motion_postprocess.correct_motion(...)` with corrected hips and rotations, the contacts, the target keyframes extracted from constraints, constraint masks, `contact_threshold`, `root_margin` and a working rig. It uses a larger above-ground offset for SOMA (0.02 vs 0.007) "since model tends to generate lower to the ground". The package is optional and built with cmake — [kimodo/postprocess.py](https://github.com/nv-tlabs/kimodo/blob/main/kimodo/postprocess.py)
- Tech report: "Simple foot locking and IK can clean up any undesirable foot skate", plus a short optimisation to hit constraints. Raw-model foot skate is 3.87 cm/s for the full model versus 2.21 cm/s for ground-truth mocap (one-stage variant 7.59), measured without post-processing — [tech report](https://arxiv.org/html/2603.15546)
- Measured by a practitioner: with MotionCorrection, full-body keyframes and end-effector targets land at 0.0000 m / 0.00° and root waypoints at exactly `root_margin`, with foot skate after correction 0.003–0.03 m/s. The four shorthand EE types, full-body and root2d get correction masks, but "a generic `end-effector` constraint would condition but NOT be corrected" — [kimodo-practical KIMODO.md](https://github.com/htdt/kimodo-practical/blob/main/KIMODO.md)
- Constraint limits: "Performance is best with fewer than 20 keyframes per constraint type" (dense 2D root paths exempt). Conflicting constraints cause artefacts or are ignored. CFG weights trade text adherence against constraint adherence — [Best Practices](https://research.nvidia.com/labs/sil/projects/kimodo/docs/key_concepts/limitations.html). kimodo-practical uses CFG `separated (2.0, 2.0)` for stance-bookended moves — [KIMODO.md](https://github.com/htdt/kimodo-practical/blob/main/KIMODO.md)
- Best-of-N with gates (kimodo-practical BAKE.md):
  - N=8 by default;
  - reject on foot skate > 0.12 m/s during labelled contact, jitter > 0.015 m/frame², stance mismatch > 0.22 m, wrong travel sign, or apex outside bounds;
  - "Gate what you actually care about, in world space": a jump picked by smoothness gates alone "never left the ground", because skate gates reward staying planted;
  - defining-property gates are hard rejects, not score terms.

  [kimodo-practical BAKE.md](https://github.com/htdt/kimodo-practical/blob/main/BAKE.md)
- Retiming and amplitude: "Expect amplitude undershoot. Generative priors are conservative" — say "at head height", gate the apex, reword stronger or raise duration. "Respect the prior's natural tempo. If the game needs a 12-frame jab, generate the natural ~1.5 s move and play it faster in-engine … don't fight the prior with a tiny `duration`" — [kimodo-practical BAKE.md](https://github.com/htdt/kimodo-practical/blob/main/BAKE.md)
- Frame data from generated strikes: startup/active come from the striking tip's speed profile, and contact is the frame of max tip extension inside the active window. Measured, visual impact falls **5–9 frames after `active[0]`**, and damage, hitstop and SFX sync to it — [kimodo-practical BAKE.md](https://github.com/htdt/kimodo-practical/blob/main/BAKE.md)
- Chaining: bookend every move with the same stance pose (full-body keyframe at frames 0 and T-1) so clips chain in-game — [kimodo-practical KIMODO.md](https://github.com/htdt/kimodo-practical/blob/main/KIMODO.md)
- **MotionBricks** (NVIDIA, SIGGRAPH 2026):
  - real-time in-betweening from 4 context poses to 1–4 target poses, with the model choosing the duration;
  - outputs 30 Hz, 0.6–1.9 s per generation, and chains;
  - SOMA checkpoint trained on Bones Rigplay 1;
  - needs an NVIDIA GPU with TensorRT, tested on an RTX 4090;
  - licences: Apache-2.0 code, checkpoints under the **NVIDIA Open Model Agreement** ("permits commercial use and redistribution of modified models"), sample motions under an evaluation-only licence;
  - the README suggests Kimodo or ARDY could generate the target poses.

  [NVlabs/motionbricks](https://github.com/NVlabs/motionbricks)
- **ARDY** (NVIDIA/ETH, SIGGRAPH 2026, released 2026-07-10): an autoregressive real-time successor with Kimodo-style constraints. SOMA models run at 30 fps (2026-10-15, including a KimodoRep variant), G1 models at 25 fps. Code Apache-2.0, weights NVIDIA Open Model Agreement. Post-processing is optional and off by default in its demo — [nv-tlabs/ardy](https://github.com/nv-tlabs/ardy); [ARDY paper](https://huggingface.co/papers/2607.08741)
- The NVIDIA Open Model Agreement (release 2026-04-02) says "NVIDIA does not claim ownership to any outputs" and "Works are commercially usable". It terminates on patent or copyright litigation — [NVIDIA Open Model Agreement](https://www.nvidia.com/en-us/agreements/enterprise-software/nvidia-open-model-agreement/)

### Inferences
- Our foot slide of 2–7 cm/frame at peaks (≈0.6–2.1 m/s at 30 fps) is one to two orders of magnitude above kimodo-practical's 0.003–0.03 m/s after MotionCorrection. Likely causes:
  - post-processing was off or the MotionCorrection package was missing;
  - contact labels were missed on fast moves (the threshold is 0.5 on predicted contacts);
  - our retarget re-introduced the slide.

  Measure skate on the SOMA output before and after retarget to separate these. Then hold foot targets across contact runs with IK on the Biped, as kimodo-practical does.
- "Dashes too slow" matches the documented conservative prior. Options:
  - force displacement and timing with Root2D waypoints or a dense path (exempt from the <20 limit);
  - gate root displacement per time as a hard reject;
  - retime the clip in-engine or in Blender (e.g. 1.3–1.5×) rather than shortening `duration`.

  Resample 30 → 25 fps as the last step.
- Weapon moves (no downswing, no thrust): give the sword hand explicit end-effector position plus rotation targets at wind-up, impact and follow-through, using the corrected shorthand EE types so MotionCorrection lands them exactly. Bookend with the stance pose. Gate the hand's world-space apex and impact height. Run best-of-N. Mocara's two-hand grip idea (a wrist-to-wrist relationship) fits two-handed swings.

### Gaps
- The MotionCorrection algorithm itself (IK? optimisation?) is not documented beyond the API. Its licence is presumably Apache-2.0 as part of the repo, but this was not verified.
- No published numbers on post-processing on very fast motion (rolls, dashes) or whether contact prediction survives them.
- MotionBricks/ARDY quality on combat in-betweens is not reported.

## 6. Community showcases and reports: what looks best and how; common complaints

### Takeaway
The most rigorous public game pipeline is kimodo-practical (a fighting game built from Kimodo, with measured gates). The most relevant research showcase is a Kingsoft-trainee project that **fine-tuned Kimodo-SOMA-RP-v1.1 on 748 hand-checked game-action clips** (attacks, knockdowns, airborne), improving them while showing forgetting unless SEED data is replayed. Common complaints: undershoot and conservatism, multi-phase actions in one prompt, rest-pose and BVH conventions, wrists, vertical root for jumps, Llama gating and VRAM. I found no weapon-specific community success. Reddit, X and Discord searches surfaced no substantive Kimodo threads.

### Cited Findings
- Fine-tuning showcase (Calix-L, 2026-10):
  - official post-training code and data are not released, so they rebuilt a method;
  - 7,951 auto-labelled clips gave unstable results, while 748 human-checked "Gold" clips taught attacks, knockdowns and airborne moves (RTE 0.5857→0.3160, BPE 0.4933→0.3247);
  - Gold-only fine-tuning caused catastrophic forgetting (SEED Content R@1 38.86%), and mixing in SEED replay restored 95.33%;
  - known limits: jump and hover vertical root unstable, and attack and jump lower body, contact and foot skate "still need improvement".

  [Calix-L/Kimodo-SOMA-RP-final-showcase](https://github.com/Calix-L/Kimodo-SOMA-RP-final-showcase)
- kimodo-practical lessons from the fighting game: one action per prompt ("never a combo"), plain physical verbs with target height and direction, amplitude undershoot, play faster in-engine, and gate apexes. A sweep gated on root dip "shipped as a crouch whose feet moved 2.5 cm" — [BAKE.md](https://github.com/htdt/kimodo-practical/blob/main/BAKE.md). It also reports that "the model won't reliably invent multi-phase choreography from one prompt" — [KIMODO.md](https://github.com/htdt/kimodo-practical/blob/main/KIMODO.md)
- Production pipelines on Kimodo: an ALS (Advanced Locomotion System) resource pipeline with a dependency graph, bounded repair, root/pelvis decomposition and Godot consumer checks (PolyForm Noncommercial) — [cc8887/Kimodo-Pipeline](https://github.com/cc8887/Kimodo-Pipeline). kimodo-mcp ships game presets (dodge roll, melee combo, hit reaction, death) — [wifimetas/kimodo-mcp](https://github.com/wifimetas/kimodo-mcp)
- Robotics showcases: Kimodo-Policy adapts a Kimodo backbone into a humanoid vision-language-action policy (HKUST-GZ, JD and others) — [kimodo-policy](https://github.com/kimodo-policy/kimodo-policy.github.io); a RoboUniversity G1 course covers Kimodo → MuJoCo/ProtoMotions/GMR/GEAR-SONIC — [RoboStore blog](https://robostore.com/blogs/news/robouniversitys-latest-course-g1-humanoid-motion-generation-with-kimodo)
- Tutorials: "AI Animation Running Now Locally for FREE (6gb vram)" by PixelArtistry (YouTube), referenced by kimodo-mcp — [YouTube](https://www.youtube.com/watch?v=nNmeuvew8LY). A low-VRAM fork and a perf patch exist ("2.5x faster, 2.5x less VRAM", Apache-2.0) — [suzuenhasa/kimodo-perf-patch](https://github.com/suzuenhasa/kimodo-perf-patch)
- GitHub issues as complaint signals:
  - why the gated old Llama-3-8B-Instruct rather than Qwen3 (#39, open, 11 comments) — [kimodo#39](https://github.com/nv-tlabs/kimodo/issues/39);
  - the SMPL-X model walks on tiptoe (#18) — [kimodo#18](https://github.com/nv-tlabs/kimodo/issues/18);
  - non-standard BVH rest pose (#15) — [kimodo#15](https://github.com/nv-tlabs/kimodo/issues/15);
  - benchmark skate depends on TF32 vs fp32, about 10% (#52) — [kimodo#52](https://github.com/nv-tlabs/kimodo/issues/52).

  Searches of the issues for "sword", "weapon", "punch", "attack", "speed" and "fast" returned nothing (gh search, 2026-11-01).
- Licence clarification for commercial games (#55, 2026-10-28): a developer asked whether baked, retargeted animations from Kimodo-SOMA-RP-v1.2 may ship in a sold game. The maintainer (davrempe) answered that all repo files are Apache-2.0 and that motions generated with the pretrained models "are subject to the model licenses linked in the table" — [kimodo#55](https://github.com/nv-tlabs/kimodo/issues/55)
- Model licences:
  - SOMA-RP weights are under the NVIDIA Open Model License (version 2025-10-24): "NVIDIA claims no ownership rights in outputs", "Models are commercially usable", attribution needed only when distributing the model, rights terminate on guardrail bypass or on litigation, Delaware law — [NVIDIA Open Model License](https://www.nvidia.com/en-us/agreements/enterprise-software/nvidia-open-model-license/);
  - the model card says "ready for commercial use" — [HF model card](https://huggingface.co/nvidia/Kimodo-SOMA-RP-v1).
- Text encoder licence: Meta Llama 3 Community License (2024-04-18) grants a worldwide royalty-free licence. Above 700 M MAU a separate licence is needed. "Built with Meta Llama 3" attribution applies when distributing Llama materials. Outputs may not be used to improve other LLMs. There is no geographic or EU restriction; EEA licensees contract with Meta Platforms Ireland — [Llama 3 LICENSE](https://huggingface.co/meta-llama/Meta-Llama-3-8B-Instruct/blob/main/LICENSE)

### Inferences
- What looks best publicly comes from three habits:
  - (a) keep within Kimodo's training distribution ("A person…", one action, combat or locomotion);
  - (b) pin what the prompt cannot express with sparse constraints and keep MotionCorrection on;
  - (c) best-of-N with hard world-space gates, then deterministic IK on the target rig.

  No one publicly shows clean weapon strikes from text alone. Both the Calix-L and kimodo-practical results point to constraints and/or fine-tuning on domain data as the fix for weapon and combat gaps.
- Licence picture for our use. Usable:
  - Kimodo code (Apache-2.0) and SOMA-RP weights (NVIDIA Open Model License; outputs not claimed by NVIDIA, commercial OK; the internal tool is not redistributed);
  - Llama 3 encoder (no EU restriction);
  - GEM-X (Apache-2.0 + NVIDIA Open Model License);
  - MotionBricks and ARDY (Apache-2.0 + NVIDIA Open Model Agreement);
  - ProtoMotions (Apache-2.0 + NVIDIA Open Model License; BONES-SEED training-data terms unclear).

  Avoid:
  - Kimodo-SMPLX-RP (R&D licence);
  - GENMO/GEM-SMPL (non-commercial);
  - GVHMR-based importers;
  - PolyForm-Noncommercial pipeline code;
  - copying from repos with no licence.

  Fine-tuning Kimodo on our own data is allowed (derivative models permitted by the NVIDIA Open Model License), but the training code is not public.

### Gaps
- Reddit (r/gamedev, r/blender, r/StableDiffusion…), X/Twitter and Discord: web search returned no substantive Kimodo threads or user reports. Platform search on those sites was not available. This is a coverage gap, not evidence of no discussion.
- No public side-by-side of Kimodo sword or weapon strikes, paired (attacker plus victim) interactions or rolls with measured quality.
- Bones' position on Rigplay-derived outputs in commercial games was not found beyond NVIDIA's licence. The maintainer reply in #55 defers to the model licence.

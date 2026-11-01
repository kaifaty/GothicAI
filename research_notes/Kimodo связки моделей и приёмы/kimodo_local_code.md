# Kimodo as installed on dev-ubuntu: what the code can do (read 2026-11-01)

Notation. Unless a path says otherwise it is under `dev-ubuntu:~/gothic-ai/kimodo/src/` (git checkout of nv-tlabs/kimodo, editable install), and `#Lx-y` gives line numbers in that checkout at HEAD `e1c219c`. "HF snapshot" means `dev-ubuntu:~/.cache/huggingface/hub/models--nvidia--Kimodo-SOMA-RP-v1.2/snapshots/0e3c89024d896064b6851201373f7834784a386b/`. "Probe" means our run folders `dev-ubuntu:~/gothic-ai/kimodo/probe1/<move>/`. Method: the source text was copied read-only (rsync) to the Mac and read there. On dev-ubuntu I ran only `kimodo_gen --help`, an attempt at `kimodo_demo --help`, package-version prints, and reads of the probe npz/json files. Nothing was generated and no GPU job ran.

## 1. Installed version, model variants and downloaded checkpoints

### Takeaway
The checkout is tag v1.2 plus one commit (`e1c219c`, 2026-10-15), and the CLI defaults to Kimodo-SOMA-RP-v1.2. That is the only Kimodo checkpoint downloaded. It is a 282M-parameter two-stage transformer diffusion model on the 30-joint SOMA skeleton at 30 fps, trained on 700 h of Bones Rigplay mocap. The registry knows 9 Kimodo variants and 1 TMR retrieval model. The interactive demo cannot run, because `viser` is not installed.

### Cited Findings
- HEAD is `e1c219c99b4b94ba48be1fb6a1cbc64ecee4c954`, "remove unused tests", 2026-10-15, by Davis Rempe. `git describe` gives `v1.2-1-ge1c219c`. The only tag is `v1.2`. The history has 54 commits. The remote is `https://github.com/nv-tlabs/kimodo.git` — [git log/tag/describe](dev-ubuntu:~/gothic-ai/kimodo/src/.git)
- The package metadata still says `version = "1.0.0"`, and the CLI entry points are `kimodo_gen`, `kimodo_demo`, `kimodo_textencoder` and `kimodo_convert` — [pyproject.toml](dev-ubuntu:~/gothic-ai/kimodo/src/pyproject.toml)
- CHANGELOG, newest entry [2026-10-15]: it adds Kimodo-SOMA-RP-v1.2 and Kimodo-SOMA-RP-ArdyRep-v1.2, trained on corrected splits after a BONES-SEED test leak in v1.1, and makes v1.2 the demo/CLI default. ArdyRep predicts "global root, raw hips as the root". Earlier entries: 2026-04-24 adds `TEXT_ENCODER_DEVICE=cpu` (<3 GB VRAM), adds `--save_example_dir`, fixes post-processing so that the smoothed root is no longer used as the target for full-body constraints, and drops `share_transition`. 2026-03-19 switches inputs/outputs to the 77-joint `somaskel77` (breaking change) — [CHANGELOG.md](dev-ubuntu:~/gothic-ai/kimodo/src/CHANGELOG.md)
- Registry repo IDs: Kimodo-SOMA-RP-v1, -v1.1, -v1.2, Kimodo-SOMA-RP-ArdyRep-v1.2, Kimodo-SMPLX-RP-v1, Kimodo-G1-RP-v1, Kimodo-SOMA-SEED-v1, -v1.1, Kimodo-G1-SEED-v1, and TMR-SOMA-RP-v1 — [kimodo/model/registry.py#L15-28](dev-ubuntu:~/gothic-ai/kimodo/src/kimodo/model/registry.py#L15)
- `DEFAULT_MODEL = "kimodo-soma-rp"`. The versionless alias resolves to the highest version of its family, so it points to v1.2. ArdyRep gets its own key `kimodo-soma-rp-ardyrep` — [registry.py#L72-76, #L133-139, #L159](dev-ubuntu:~/gothic-ai/kimodo/src/kimodo/model/registry.py#L133)
- Our probe's gen.log prints `Loaded model: Kimodo-SOMA-RP-v1.2 (kimodo-soma-rp)` — [probe1/barge/gen.log](dev-ubuntu:~/gothic-ai/kimodo/probe1/barge/gen.log)
- Contents of the HF cache: `models--nvidia--Kimodo-SOMA-RP-v1.2` is the only Kimodo or TMR model. The text-encoder repos are also there (`McGill-NLP/LLM2Vec-Meta-Llama-3-8B-Instruct-mntp`, `...-mntp-supervised`, `NousResearch/Meta-Llama-3-8B-Instruct`), along with local copies in `~/gothic-ai/kimodo/models/` (Meta-Llama-3-8B-Instruct, llm2vec-mntp, llm2vec-supervised) — [ls ~/.cache/huggingface/hub, ~/gothic-ai/kimodo/models](dev-ubuntu:~/.cache/huggingface/hub)
- Checkpoint config: `num_base_steps: 1000`, `cfg_type: separated`, `TwostageDenoiser`, `KimodoMotionRep` at `fps: 30`, `SOMASkeleton30`, `motion_mask_mode: concat`, `llm_shape: [1, 4096]`, `use_text_mask: false`, `latent_dim 1024`, `ff_size 2048`, `num_layers 16`, `num_heads 8`, `input_first_heading_angle: true`, `num_text_tokens_override: 50` — [HF snapshot config.yaml](dev-ubuntu:~/.cache/huggingface/hub/models--nvidia--Kimodo-SOMA-RP-v1.2/snapshots/0e3c89024d896064b6851201373f7834784a386b/config.yaml)
- The model card states a 282 M-parameter "Novel Two-Stage Transformer" diffusion model, "ready for commercial use", released 10/15/2026. The licence is the NVIDIA Open Model License. Training data: 700 h of Bones Rigplay mocap with text, split 90/10. Joint rotations are output as `num_frames x 30 x 3 x 3` — [HF snapshot README.md](dev-ubuntu:~/.cache/huggingface/hub/models--nvidia--Kimodo-SOMA-RP-v1.2/snapshots/0e3c89024d896064b6851201373f7834784a386b/README.md)
- Installed packages in `~/gothic-ai/kimodo/.venv`: kimodo 1.0.0 (imported from `src/kimodo`), torch 2.11.0+cu128, transformers 5.1.0, py-soma-x 0.3.4. `motion_correction` imports from `src/MotionCorrection/python/motion_correction/` (prebuilt `.so`, cpython-311). `viser` is not installed, and `kimodo_demo --help` fails with `ModuleNotFoundError: No module named 'viser'` — [venv check on dev-ubuntu](dev-ubuntu:~/gothic-ai/kimodo/.venv)
- The text encoder is LLM2Vec: base `McGill-NLP/LLM2Vec-Meta-Llama-3-8B-Instruct-mntp`, PEFT adapter `...-mntp-supervised`, bf16, `llm_dim 4096` — [kimodo/model/load_model.py#L22-33](dev-ubuntu:~/gothic-ai/kimodo/src/kimodo/model/load_model.py#L22)

### Inferences
- The demo's timeline editor, gizmo pose editing and "Save Constraints" are unavailable on dev-ubuntu until `viser` is installed (optional extra `demo` in pyproject). That install is a pip install, which these research rules did not allow.
- Kimodo-SOMA-RP-ArdyRep-v1.2 is the only other variant on the same skeleton and data that might help us. Its root constraints apply to the raw hips, not to a smoothed root, and its benchmark 2D-root error is lower (see section 6). It is not downloaded.
- The TMR-SOMA-RP-v1 text-motion embedding model could rank our 8 samples per move by how well each matches its prompt. It is not downloaded, and we have not tested it for this use.

### Gaps
- The local copy holds no tech report (only a link to the PDF on research.nvidia.com), so architecture details beyond the code and docs were not checked here.

## 2. Constraint types, constraints.json schema and loader

### Takeaway
The loader accepts exactly 7 `type` strings: `root2d`, `fullbody`, `left-hand`, `right-hand`, `left-foot`, `right-foot` and `end-effector`. Five quantities can be conditioned: smooth-root XZ, root height, heading, global joint positions and global joint rotations. A `fullbody` keyframe constrains only the positions of all 30 joints (plus root XZ, height and heading), never the rotations. Only the hand and foot end-effector constraints pin a global rotation, and only of the chain's base joint (wrist or ankle). The `end-effector` type can also take `"Hips"`, which gives a pelvis position and rotation constraint. Foot contacts are a model feature but have no constraint class. Velocities cannot be constrained.

### Cited Findings
- `TYPE_TO_CLASS` maps `root2d`→Root2DConstraintSet, `fullbody`→FullBodyConstraintSet, `left-hand`/`right-hand`/`left-foot`/`right-foot`→the four subclasses, and `end-effector`→EndEffectorConstraintSet. `load_constraints_lst` reads a JSON array and dispatches on `el["type"]` — [kimodo/constraints.py#L555-592](dev-ubuntu:~/gothic-ai/kimodo/src/kimodo/constraints.py#L555)
- The CLI loads constraints from `--constraints <file>`. When it is given `--input_folder`, it loads `<folder>/constraints.json` if that file exists — [kimodo/scripts/generate.py#L260-263, #L302-306](dev-ubuntu:~/gothic-ai/kimodo/src/kimodo/scripts/generate.py#L260)
- **root2d.** It needs `frame_indices` and `smooth_root_2d` `[T,2]` as `[x, z]`, with optional `global_root_heading` `[T,2]`. The lengths must match `frame_indices` (assert). If a 3-column array is passed, the code keeps columns `[0, 1]`, which are x and y rather than x and z, so always pass 2 columns. Its `update_constraints` writes only `smooth_root_2d` and, if present, `global_root_heading` — [constraints.py#L75-121, #L164-179](dev-ubuntu:~/gothic-ai/kimodo/src/kimodo/constraints.py#L75)
- **fullbody.** `from_dict` reads `local_joints_rot` `[T,J,3]` (axis-angle) and `root_positions` `[T,3]`, plus an optional `smooth_root_2d` `[T,2]`. It runs FK to get global positions and rotations. `smooth_root_2d` defaults to the root's xz. Constrained features: global positions of every joint (`create_pairs(frame, all joints)`), `smooth_root_2d`, `root_y_pos` and `global_root_heading`. The code comment says "global rotations are not used here" — [constraints.py#L182-259, #L303-325](dev-ubuntu:~/gothic-ai/kimodo/src/kimodo/constraints.py#L233)
- The docs agree: "the `local_joint_rot` will not explicitly be constrained, the constraint will be on the joint positions that results from FK" — [docs/source/user_guide/constraints.md#L63-70](dev-ubuntu:~/gothic-ai/kimodo/src/docs/source/user_guide/constraints.md#L63)
- **End-effectors** (`left-hand`, `right-hand`, `left-foot`, `right-foot`, `end-effector`). They use the same fields as fullbody. `end-effector` additionally needs `joint_names`. Position joints are the whole chain and the rotation joint is its first element. Every EE constraint also writes `smooth_root_2d`, `root_y_pos` and `global_root_heading`, taken from the full pose given — [constraints.py#L328-427, #L485-512](dev-ubuntu:~/gothic-ai/kimodo/src/kimodo/constraints.py#L387)
- `expand_joint_names` accepts only `["LeftFoot", "RightFoot", "LeftHand", "RightHand", "Hips"]`. It constrains the positions of the chain `*_joint_names` (Hips: the pelvis) and the rotation of the chain's first joint — [kimodo/skeleton/base.py#L135-173](dev-ubuntu:~/gothic-ai/kimodo/src/kimodo/skeleton/base.py#L135)
- Chains on somaskel30: right hand = `["RightHand", "RightHandMiddleEnd"]`, left hand = `["LeftHand", "LeftHandMiddleEnd"]`, right foot = `["RightFoot", "RightToeBase"]`, left foot = `["LeftFoot", "LeftToeBase"]`. Heading is computed from `hip_joint_names = ["RightLeg", "LeftLeg"]` — [kimodo/skeleton/definitions.py#L187-204](dev-ubuntu:~/gothic-ai/kimodo/src/kimodo/skeleton/definitions.py#L187)
- The docs' `end-effector` example names (`["left_hand"]`, `["right_foot", "left_foot"]`) do not match the code. The code accepts only the CamelCase base names above, and anything else fails in `base_ee.index` — [docs user_guide/constraints.md#L77-82](dev-ubuntu:~/gothic-ai/kimodo/src/docs/source/user_guide/constraints.md#L77) vs [skeleton/base.py#L147, #L170](dev-ubuntu:~/gothic-ai/kimodo/src/kimodo/skeleton/base.py#L147)
- Heading is `atan2(dz, -dx)` of `posed_joints[RightLeg] - posed_joints[LeftLeg]`, stored as `[cos θ, sin θ]`. θ = 0 means facing +Z — [kimodo/motion_rep/feature_utils.py#L111-125](dev-ubuntu:~/gothic-ai/kimodo/src/kimodo/motion_rep/feature_utils.py#L111); [constraints.py#L50-54](dev-ubuntu:~/gothic-ai/kimodo/src/kimodo/constraints.py#L50); [docs user_guide/constraints.md#L36, #L44](dev-ubuntu:~/gothic-ai/kimodo/src/docs/source/user_guide/constraints.md#L36)
- Joint count: `local_joints_rot` may hold 30 or 77 joints. 77→30 is converted for the SOMA30 model, and any other mismatch raises `ValueError` — [constraints.py#L17-35](dev-ubuntu:~/gothic-ai/kimodo/src/kimodo/constraints.py#L17)
- somaskel30 joint order (the index order of `local_joints_rot` for 30 joints): 0 Hips, 1 Spine1, 2 Spine2, 3 Chest, 4 Neck1, 5 Neck2, 6 Head, 7 Jaw, 8 LeftEye, 9 RightEye, 10 LeftShoulder, 11 LeftArm, 12 LeftForeArm, 13 LeftHand, 14 LeftHandThumbEnd, 15 LeftHandMiddleEnd, 16 RightShoulder, 17 RightArm, 18 RightForeArm, 19 RightHand, 20 RightHandThumbEnd, 21 RightHandMiddleEnd, 22 LeftLeg, 23 LeftShin, 24 LeftFoot, 25 LeftToeBase, 26 RightLeg, 27 RightShin, 28 RightFoot, 29 RightToeBase — [definitions.py#L206-237](dev-ubuntu:~/gothic-ai/kimodo/src/kimodo/skeleton/definitions.py#L206)
- Features the model can be conditioned on (the KimodoMotionRep layout): `smooth_root_pos[3]`, `global_root_heading[2]`, `local_joints_positions[J,3]`, `global_rot_data[J,6]`, `velocities[J,3]`, `foot_contacts[4]`. `create_conditions` fills only smooth root xz, root y, heading, `global_rot_data` (from `global_joints_rots`) and `local_joints_positions` (from `global_joints_positions`). Positions raise `ValueError` unless the smooth root is constrained on the same frames — [kimodo/motion_rep/reps/kimodo_motionrep.py#L51-66, #L267-327](dev-ubuntu:~/gothic-ai/kimodo/src/kimodo/motion_rep/reps/kimodo_motionrep.py#L298)
- "**Foot contacts**: toe/heel contact patterns. While the model is trained to support this, it is not currently implemented in the demo UI or Python API" — [docs/source/key_concepts/constraints.md#L30](dev-ubuntu:~/gothic-ai/kimodo/src/docs/source/key_concepts/constraints.md#L30)
- When several constraints target the same (frame, joint) feature, one row is kept, and which one is unspecified ("the `scatter_` winner, which torch leaves unspecified") — [kimodo/motion_rep/conditioning.py#L79-103](dev-ubuntu:~/gothic-ai/kimodo/src/kimodo/motion_rep/conditioning.py#L79)
- Coordinates are Y-up in metres. Frame 0's smooth root is at XZ (0,0). The Y of `root_positions` is the absolute hip height (about 0.9 m standing). A constraint at frame 0 is not required, and with none the model starts freely near the origin at `first_heading_angle` — [docs user_guide/constraints.md#L21-49](dev-ubuntu:~/gothic-ai/kimodo/src/docs/source/user_guide/constraints.md#L21)
- In the CLI path (multi-prompt), every constraint goes through `crop_move(start, end)`, which keeps only `frame_indices < end`. A keyframe at or past the clip length is dropped without a warning — [kimodo/model/kimodo_model.py#L175-178](dev-ubuntu:~/gothic-ai/kimodo/src/kimodo/model/kimodo_model.py#L175); [constraints.py#L261-270](dev-ubuntu:~/gothic-ai/kimodo/src/kimodo/constraints.py#L261)
- The CLI computes `num_frames = int(float(duration) * fps)`, which truncates — [generate.py#L258](dev-ubuntu:~/gothic-ai/kimodo/src/kimodo/scripts/generate.py#L258)
- Shipped SOMA examples of each type:
  - `03_full_body_keyframes`: fullbody at frames [79, 134], with `local_joints_rot` [2,30,3], `root_positions` [2,3] and `smooth_root_2d` [2,2].
  - `04_ee_constraint`: all four `right-foot`/`left-foot`/`left-hand`/`right-hand` at frames [28, 94].
  - `05_root_path`: a dense `root2d` over all 300 frames of a 10 s clip.
  - `06_root_waypoints`: `root2d` at [0, 90, 180].
  - `07_mixed_constraints`: one fullbody at 108 plus a dense 152-frame `root2d`.

  None of the shipped files uses `global_root_heading` or `end-effector` — [kimodo/assets/demo/examples/kimodo-soma-rp/*/constraints.json](dev-ubuntu:~/gothic-ai/kimodo/src/kimodo/assets/demo/examples/kimodo-soma-rp)
- Our probe uses fullbody at frame 0 and at the last frame (for example barge [0] and [44] with 30 joints; root at [0.0, 0.92568, 0.0]). `riposte_victim` uses `root2d` at [74]. The right-wrist axis-angle in the stance keyframe is [2.258823, -0.461264, -0.186013], and the root rotation is [-0.002866, 0.096004, -0.060915] — [probe1/*/constraints.json](dev-ubuntu:~/gothic-ai/kimodo/probe1/barge/constraints.json)

Minimal JSON per type, matching the loader. Every `local_joints_rot` entry is a list of 30 axis-angle triples in the somaskel30 order above (77 also works). The `[...]` placeholders stand for those triples.

```json
[
  {"type": "root2d", "frame_indices": [0, 10, 35],
   "smooth_root_2d": [[0.0, 0.0], [0.0, -1.6], [0.0, -2.0]],
   "global_root_heading": [[1.0, 0.0], [1.0, 0.0], [1.0, 0.0]]},

  {"type": "fullbody", "frame_indices": [0, 35],
   "local_joints_rot": [[[0.0, 0.0, 0.0], "... 30 triples ..."], ["... 30 triples ..."]],
   "root_positions": [[0.0, 0.926, 0.0], [0.0, 0.926, -2.0]],
   "smooth_root_2d": [[0.0, 0.0], [0.0, -2.0]]},

  {"type": "right-hand", "frame_indices": [14],
   "local_joints_rot": [["... 30 triples: the full pose whose FK puts the right wrist where wanted ..."]],
   "root_positions": [[0.0, 0.90, 0.4]]},

  {"type": "left-hand", "frame_indices": [14], "local_joints_rot": [["..."]], "root_positions": [[0.0, 0.90, 0.4]]},
  {"type": "left-foot", "frame_indices": [20], "local_joints_rot": [["..."]], "root_positions": [[0.0, 0.90, 0.8]]},
  {"type": "right-foot", "frame_indices": [20], "local_joints_rot": [["..."]], "root_positions": [[0.0, 0.90, 0.8]]},

  {"type": "end-effector", "joint_names": ["Hips"], "frame_indices": [18],
   "local_joints_rot": [["..."]], "root_positions": [[0.0, 0.80, 1.0]]}
]
```

What each type constrains:
- `root2d`: smooth root xz, plus heading if given.
- `fullbody`: positions of all 30 joints, smooth root xz, root y and heading.
- `right-hand`: positions of RightHand and RightHandMiddleEnd, global rotation of RightHand, smooth root xz, root y and heading.
- `end-effector` + `["Hips"]`: pelvis position and pelvis global rotation, smooth root xz, root y and heading.

A dense path is a `root2d` with `frame_indices` 0…T-1 and one `[x, z]` per frame. `meta.json`: single prompt `{"text": str, "duration": s}`, or multi-prompt `{"texts": [...], "durations": [...]}`, plus optional `num_samples`, `seed`, `diffusion_steps` and `cfg` — [kimodo/meta.py#L32-80](dev-ubuntu:~/gothic-ai/kimodo/src/kimodo/meta.py#L32); [generate.py#L255-273](dev-ubuntu:~/gothic-ai/kimodo/src/kimodo/scripts/generate.py#L255)

### Inferences
- The free-rotating right wrist follows from this design. A `fullbody` keyframe pins only positions. On somaskel30 the hand orientation at a keyframe is set by three points (RightHand, RightHandThumbEnd, RightHandMiddleEnd), and between keyframes nothing constrains it. A `right-hand` constraint is the only way to pin the global wrist rotation, and it works per frame. It fits sparse keyframes along the swing better than a dense track (see section 6, under 20 keyframes per type).
- An EE constraint is not local to the hand. It also fixes the pelvis height, the smooth-root XZ and the hip heading taken from the full pose we supply. A `right-hand` keyframe for a strike therefore has to carry a plausible whole-body pose (for example the impact pose from the original Gothic animation, retargeted), with the root where we want it at that frame. Otherwise the constraint drags the root.
- `end-effector` + `["Hips"]` is the only per-frame constraint on pelvis rotation (pitch and roll as well as yaw). It might help a shoulder barge (torso lean into the target) or a crouch. It is enforced only by the model, because post-processing ignores the generic `end-effector` name (section 3).
- Our generator writes `duration = frames/30` and puts the end keyframe at `frames-1`. Kimodo re-derives `int(duration*30)`, and for frame counts 123 and 245–247 that truncates by one frame (checked in Python: `int((f/30)*30) != f` only for those f in 1..300). The end keyframe would then be dropped without notice. The probe's counts (36, 45, 51, 66, 69, 75, 78) are not affected (gen.log frame counts match the keyframes).
- Foot-contact constraints (for example "left foot planted through the swing") would need a local patch: a constraint class plus a `foot_contacts` branch in `create_conditions`. The JSON loader has no such path.

### Gaps
- The docs give no example of `global_root_heading` or of `end-effector`, and I did not check numerically how well the model follows them. The benchmark has `waypoint_2dposrot` and `path_2dposrot` cases (section 6), so heading is a trained input.

## 3. Guidance, sampling, conditioning, post-processing, CLI and demo options

### Takeaway
Sampling is plain deterministic DDIM (cosine schedule, 1000 base steps, subsampled to `diffusion_steps`) with no gradient guidance. Constraints enter by overwriting the noisy input plus a concatenated mask, which is learned inpainting. CFG is "separated" by default: the text and constraint branches are computed without each other and summed with weights [2.0, 2.0]. A `meta.json` can only request separated CFG or no CFG, while the CLI flags can choose `regular` (joint text+constraint). Post-processing is on by default in the CLI. It hits fullbody keyframes exactly (positions, rotations and root) and uses IK for hand and foot pins and foot contacts. The generic `end-effector` type is ignored by post-processing.

### Cited Findings
- Diffusion uses a cosine beta schedule (`get_beta_schedule`). `space_timesteps` spreads `num_denoising_steps` evenly over the 1000 trained steps. `DDIMSampler` is "Deterministic DDIM sampler (eta = 0)" — [kimodo/model/diffusion.py#L12-26, #L50-58, #L113-133](dev-ubuntu:~/gothic-ai/kimodo/src/kimodo/model/diffusion.py#L113)
- Sampling starts from `torch.randn` noise, so the seed decides everything. `guide_masks` is accepted by `denoising_step`/`_generate` but never used, which means no test-time gradient guidance — [kimodo/model/kimodo_model.py#L75-121, #L562-634](dev-ubuntu:~/gothic-ai/kimodo/src/kimodo/model/kimodo_model.py#L608)
- Conditioning: in `concat` mode the input is `x*(1-mask) + observed*mask`, and the mask is concatenated to the features. Stage 1 predicts the global root, which is converted to a local root for stage 2, the body denoiser — [kimodo/model/twostage_denoiser.py#L98-152](dev-ubuntu:~/gothic-ai/kimodo/src/kimodo/model/twostage_denoiser.py#L98); docs: "Constraints … are used to overwrite the corresponding values in the noisy motion. Additionally, a mask … is concatentated" — [docs/source/key_concepts/model.md#L16-20](dev-ubuntu:~/gothic-ai/kimodo/src/docs/source/key_concepts/model.md#L16)
- Text conditioning: LLM2Vec returns one mean-pooled 4096-d vector per prompt (`encoded_text[:, None]`, `lengths = 1`) with internal batch size 1. It is zero-padded to 50 tokens, and with `use_text_mask: false` every slot is attended. The first heading angle enters as an extra prefix token — [kimodo/model/llm2vec/llm2vec_wrapper.py#L65-95](dev-ubuntu:~/gothic-ai/kimodo/src/kimodo/model/llm2vec/llm2vec_wrapper.py#L65); [kimodo/model/llm2vec/llm2vec.py#L77-80](dev-ubuntu:~/gothic-ai/kimodo/src/kimodo/model/llm2vec/llm2vec.py#L77); [kimodo/model/backbone.py#L159-208](dev-ubuntu:~/gothic-ai/kimodo/src/kimodo/model/backbone.py#L159); [HF snapshot config.yaml](dev-ubuntu:~/.cache/huggingface/hub/models--nvidia--Kimodo-SOMA-RP-v1.2/snapshots/0e3c89024d896064b6851201373f7834784a386b/config.yaml)
- Prompts are sanitized: whitespace is collapsed, the text is capitalized, stray leading/trailing symbols are trimmed and a final period is added — [kimodo/sanitize.py#L6-62](dev-ubuntu:~/gothic-ai/kimodo/src/kimodo/sanitize.py#L6). A positional CLI prompt is split at every "." into sequential segments, but `meta.json` `text` is kept as one prompt — [generate.py#L131-149](dev-ubuntu:~/gothic-ai/kimodo/src/kimodo/scripts/generate.py#L131); [meta.py#L52-62](dev-ubuntu:~/gothic-ai/kimodo/src/kimodo/meta.py#L52)
- CFG modes are `nocfg`, `regular` and `separated`:
  - `regular`: `uncond + w*(text_and_constraint - uncond)`.
  - `separated`: `uncond + w_text*(text_only - uncond) + w_constraint*(constraint_only - uncond)`, where the text branch gets the constraint mask zeroed and the constraint branch gets the text zeroed.

  Separated triples the batch through the denoiser, and regular doubles it — [kimodo/model/cfg.py#L10, #L59-131](dev-ubuntu:~/gothic-ai/kimodo/src/kimodo/model/cfg.py#L94); [docs/source/user_guide/configuration.md#L5-26](dev-ubuntu:~/gothic-ai/kimodo/src/docs/source/user_guide/configuration.md#L5)
- The model defaults are `cfg_type: separated` (checkpoint config) and `cfg_weight=[2.0, 2.0]` — [kimodo_model.py#L34, #L387](dev-ubuntu:~/gothic-ai/kimodo/src/kimodo/model/kimodo_model.py#L387)
- How the CLI resolves CFG: `--cfg_type`/`--cfg_weight` override `meta.json`. A meta `cfg` block always becomes `separated` with `[text_weight, constraint_weight]` (defaults 2.0/2.0), or `nocfg` when `enabled: false`. One weight alone implies `regular` and two imply `separated` — [generate.py#L177-236](dev-ubuntu:~/gothic-ai/kimodo/src/kimodo/scripts/generate.py#L177)
- The shipped G1 example `02_multi_text_ee_constraint` uses `text_weight 1.0, constraint_weight 2.0`. Every SOMA example uses 2.0/2.0 — [kimodo/assets/demo/examples/*/meta.json](dev-ubuntu:~/gothic-ai/kimodo/src/kimodo/assets/demo/examples)
- Denoising-steps advice from the docs: "Fewer steps (50-100): Faster inference, slightly lower quality. More steps (100-200): Higher quality, slower inference" — [configuration.md#L32-35](dev-ubuntu:~/gothic-ai/kimodo/src/docs/source/user_guide/configuration.md#L32)
- The CLI always calls the model with `multi_prompt=True`. Post-processing is on unless `--no-postprocess` is given, and always off for G1 — [generate.py#L90-94, #L325-338](dev-ubuntu:~/gothic-ai/kimodo/src/kimodo/scripts/generate.py#L325)
- Multi-prompt composition: segments are generated one after another. Each later segment gets `num_transition_frames` (default 5) leading frames constrained by a FullBody set and an EndEffector set (`["LeftHand","RightHand","LeftFoot","RightFoot"]`, so hand and foot rotations too) from the previous segment's last frames. Each segment is post-processed on its own. Without post-processing the overlap is linearly blended — [kimodo_model.py#L163-356](dev-ubuntu:~/gothic-ai/kimodo/src/kimodo/model/kimodo_model.py#L187); docs: "the transition between prompts actually happens at the start of the second motion" — [docs/source/key_concepts/limitations.md#L22](dev-ubuntu:~/gothic-ai/kimodo/src/docs/source/key_concepts/limitations.md#L22)
- Options available only in the Python API (not in the CLI): `first_heading_angle` (default 0, facing +Z), `root_margin` (default 0.04 m), per-sample constraint lists (`list[list]`, only in the non-multi-prompt path), and batches of different prompts with `multi_prompt=False` — [kimodo_model.py#L380-452, #L473-489](dev-ubuntu:~/gothic-ai/kimodo/src/kimodo/model/kimodo_model.py#L380); [kimodo/motion_rep/reps/base.py#L551-588](dev-ubuntu:~/gothic-ai/kimodo/src/kimodo/motion_rep/reps/base.py#L551)
- Post-processing (`post_process_motion`) builds per-frame masks only for constraint names `fullbody`, `left-foot`, `right-foot`, `left-hand`, `right-hand` and `root2d`. Generic `end-effector` gets no mask. SOMA uses `above_ground_offset 0.02`, `contact_threshold 0.5` and `root_margin 0.04` — [kimodo/postprocess.py#L186-252, #L276](dev-ubuntu:~/gothic-ai/kimodo/src/kimodo/postprocess.py#L218)
- `CorrectMotion` runs these steps in order:
  1. `CorrectHipsY`: matches fullbody frames and keeps the root height on contact frames.
  2. `CorrectHipsXZ`: pins fullbody frames and keeps the root within `root_margin` on root2d frames.
  3. `CorrectJointRotations`: warps every bone's rotation to the fullbody keyframes.
  4. `DoEffectorIK`: 2-bone IK to the hand/foot target position, then restores the target global rotation.
  5. `DoContactIK`: sticks feet to the floor in the model's predicted contact intervals.

  [MotionCorrection/src/cpp/AnimProcessing/Utility.cpp#L1103-1196](dev-ubuntu:~/gothic-ai/kimodo/src/MotionCorrection/src/cpp/AnimProcessing/Utility.cpp#L1103); [Utility.cpp#L497-600](dev-ubuntu:~/gothic-ai/kimodo/src/MotionCorrection/src/cpp/AnimProcessing/Utility.cpp#L497)
- The warps use an ADMM `TrajectoryCorrector` that hits pinned frames and preserves the original positions, velocities and accelerations (weights `pos 0.001`, `vel 1.0`, `acc 10.0`), with root velocity weighting so standing characters do not drift — [MotionCorrection/src/cpp/AnimProcessing/TrajectoryCorrector.cpp#L139-167](dev-ubuntu:~/gothic-ai/kimodo/src/MotionCorrection/src/cpp/AnimProcessing/TrajectoryCorrector.cpp#L139); [Utility.cpp#L18-20, #L1118-1136](dev-ubuntu:~/gothic-ai/kimodo/src/MotionCorrection/src/cpp/AnimProcessing/Utility.cpp#L18)
- Hand and foot pins are suppressed on fullbody frames (`(1 - fullPoseMask) * handMask`). Foot contacts come from the model's own heel/toe contact channels and are skipped where a foot constraint exists — [MotionCorrection/src/cpp/BindingsPython.cpp#L168-228](dev-ubuntu:~/gothic-ai/kimodo/src/MotionCorrection/src/cpp/BindingsPython.cpp#L168)
- Docs: post-processing makes motion "_exactly_ hit constraints … while minimizing changes in acceleration and velocity". Also: "The model by itself can generate foot skating and will not exactly hit constraints" — [key_concepts/constraints.md#L50-52](dev-ubuntu:~/gothic-ai/kimodo/src/docs/source/key_concepts/constraints.md#L50); [limitations.md#L23](dev-ubuntu:~/gothic-ai/kimodo/src/docs/source/key_concepts/limitations.md#L23)
- `kimodo_gen --help` on dev-ubuntu lists exactly these options: `prompt`, `--model`, `--duration`, `--num_samples`, `--diffusion_steps`, `--num_transition_frames`, `--constraints`, `--output`, `--save_example_dir`, `--bvh`, `--bvh_standard_tpose`, `--no-postprocess`, `--seed`, `--input_folder`, `--cfg_type {nocfg,regular,separated}` and `--cfg_weight [..]` — [kimodo_gen --help output](dev-ubuntu:~/gothic-ai/kimodo/.venv/bin/kimodo_gen); [generate.py#L20-128](dev-ubuntu:~/gothic-ai/kimodo/src/kimodo/scripts/generate.py#L20)
- The demo app (not runnable here) has:
  - a timeline with several prompt boxes (2–10 s each) and constraint tracks Full-Body, 2D Root (with "Densify"/"Make Smooth Path") and Left/Right Hand/Foot;
  - Ctrl-drag interval constraints, FK gizmo editing in local or global space, and "Snap to Constraint" / "Reset Constraint";
  - per-sample selection, post-processing with a Root Margin setting, and saving of constraints, motion and example folders.

  [kimodo/demo/config.py#L33-40, #L66-162](dev-ubuntu:~/gothic-ai/kimodo/src/kimodo/demo/config.py#L106); [docs/source/interactive_demo/constraints.md#L8-15](dev-ubuntu:~/gothic-ai/kimodo/src/docs/source/interactive_demo/constraints.md#L8); [docs/source/interactive_demo/generation.md#L7-13](dev-ubuntu:~/gothic-ai/kimodo/src/docs/source/interactive_demo/generation.md#L7)
- With a 2D Root track present, the demo takes `smooth_root_2d` for fullbody/EE keyframes from the dense path, and it regroups EE keyframes by type into `left-hand`/`right-hand`/... classes — [kimodo/demo/generation.py#L24-129](dev-ubuntu:~/gothic-ai/kimodo/src/kimodo/demo/generation.py#L24)
- Dense paths: "Kimodo is only trained on full-sequence paths". The demo adds waypoints at the first and last frame when it densifies — [interactive_demo/constraints.md#L14](dev-ubuntu:~/gothic-ai/kimodo/src/docs/source/interactive_demo/constraints.md#L14)
- The text encoder can run as a separate Gradio server (`TEXT_ENCODER_MODE=api|local|auto`, `TEXT_ENCODER_URL`, default `http://127.0.0.1:9550/`). Other variables: `TEXT_ENCODER_DEVICE`, `TEXT_ENCODERS_DIR`, `CHECKPOINT_DIR`, `LOCAL_CACHE`, `HUGGINGFACE_CACHE_DIR` — [load_model.py#L44-105, #L160-196](dev-ubuntu:~/gothic-ai/kimodo/src/kimodo/model/load_model.py#L83); [registry.py#L160](dev-ubuntu:~/gothic-ai/kimodo/src/kimodo/model/registry.py#L160); [llm2vec_wrapper.py#L27-44](dev-ubuntu:~/gothic-ai/kimodo/src/kimodo/model/llm2vec/llm2vec_wrapper.py#L27)

### Inferences
- With separated CFG at 2.0/2.0, the constraint branch never sees the text. With identical stance keyframes at both ends, that branch pushes toward "stand in stance", which may damp the action the text asks for. Post-processing already pins fullbody keyframes exactly, so lowering `constraint_weight` (for example 1.0) and raising `text_weight` (for example 2.5–3), or trying `--cfg_type regular`, should cost little endpoint accuracy. This should be tested, not assumed. `regular` can only be selected on the CLI, never in `meta.json`.
- There is no inpainting or editing mode separate from constraints. To re-generate part of an existing clip, the frames to keep would be given as fullbody keyframes, ideally sparse (section 6).
- The constraint types available to force the timing of a fast dash are:
  - `root2d` waypoints at intermediate frames, which post-processing holds within 4 cm;
  - a dense full-sequence `root2d` path with our own speed profile, the trained "path" mode;
  - an intermediate fullbody keyframe.

  The model has no velocity input.
- Multi-prompt chaining ("stance → wind-up and strike → recover") is supported, but the transition takes the first 5 frames of each later segment, and the docs require every prompt to make sense alone.

### Gaps
- The code gives no numbers on how CFG weights trade text against constraint accuracy. The configuration doc only says the weights are "helpful when there is a tradeoff".

## 4. NPZ output contents and SOMA joint sets (30 vs 77)

### Takeaway
The model works internally on somaskel30. Every output is expanded to somaskel77: hands get a fixed relaxed rest pose, and foot contacts go from 4 to 6 channels. Each sample's npz has `local_rot_mats`, `global_rot_mats` and `posed_joints` (77 joints), plus `root_positions`, `smooth_root_pos`, `global_root_heading`, `foot_contacts` (bool, 6 channels) and an undocumented `posed_joints_norootpos` (30 joints).

### Cited Findings
- Output dict keys: `local_rot_mats`, `global_rot_mats`, `posed_joints`, `posed_joints_norootpos`, `root_positions`, `smooth_root_pos`, `foot_contacts` (contact probability > 0.5) and `global_root_heading` — [kimodo/motion_rep/reps/kimodo_motionrep.py#L185-265](dev-ubuntu:~/gothic-ai/kimodo/src/kimodo/motion_rep/reps/kimodo_motionrep.py#L248)
- SOMA30→77 conversion: `to_SOMASkeleton77` fills the 77-joint rotations with `relaxed_hands_rest_pose` and overwrites the 30-joint subset, then FK is re-run. `foot_contacts` goes from `[L_heel, L_toe, R_heel, R_toe]` to `[L_heel, L_toe, L_toe_end, R_heel, R_toe, R_toe_end]` — [definitions.py#L246-283](dev-ubuntu:~/gothic-ai/kimodo/src/kimodo/skeleton/definitions.py#L264); [kimodo_model.py#L371-373, #L553-555](dev-ubuntu:~/gothic-ai/kimodo/src/kimodo/model/kimodo_model.py#L553)
- Actual probe file `probe1/barge/out/out_00.npz`: `local_rot_mats (45,77,3,3)`, `global_rot_mats (45,77,3,3)`, `posed_joints (45,77,3)`, `posed_joints_norootpos (45,30,3)`, `root_positions (45,3)`, `smooth_root_pos (45,3)`, `foot_contacts (45,6) bool`, `global_root_heading (45,2)`, all float32 except the bool — [probe1/barge/out/out_00.npz](dev-ubuntu:~/gothic-ai/kimodo/probe1/barge/out/out_00.npz)
- The docs list `foot_contacts` as `[T, 4]` and do not mention `posed_joints_norootpos`. That differs from the actual SOMA output. The NPZ rest pose is the standard SOMA T-pose, while BVH by default uses the BONES-SEED rest pose (`--bvh_standard_tpose` switches it). BVH is written in centimetres — [docs/source/user_guide/output_formats.md#L7-40](dev-ubuntu:~/gothic-ai/kimodo/src/docs/source/user_guide/output_formats.md#L7)
- With several samples, the CLI writes `<output>/<basename>_NN.npz` (our probe: `out/out_00.npz` … `out_07.npz`) — [generate.py#L353-361](dev-ubuntu:~/gothic-ai/kimodo/src/kimodo/scripts/generate.py#L353)
- somaskel77 has 77 joints, including full finger chains (Thumb1-3/End, Index1-4/End, … Pinky) and HeadEnd, Jaw, Eyes and ToeEnd. somaskel30 keeps only ThumbEnd and MiddleEnd per hand and drops HeadEnd and ToeEnd — [definitions.py#L14-164, #L182-237](dev-ubuntu:~/gothic-ai/kimodo/src/kimodo/skeleton/definitions.py#L86)
- "all training data for Kimodo is on a uniform skeleton proportion corresponding to one single set of identity parameters for the SOMA body model" — [docs/source/key_concepts/skeleton.md#L20](dev-ubuntu:~/gothic-ai/kimodo/src/docs/source/key_concepts/skeleton.md#L20)
- `kimodo_convert` converts between Kimodo NPZ, AMASS NPZ, SOMA BVH and G1 CSV. "Kimodo NPZ output is always resampled to 30 Hz" (`KIMODO_CONVERT_TARGET_FPS = 30.0`), and there is no 25 fps output option — [kimodo/scripts/motion_convert.py#L40-89](dev-ubuntu:~/gothic-ai/kimodo/src/kimodo/scripts/motion_convert.py#L40); [kimodo/exports/motion_io.py#L23](dev-ubuntu:~/gothic-ai/kimodo/src/kimodo/exports/motion_io.py#L23)

### Inferences
- Finger poses in the output are a fixed relaxed rest pose. The model does not generate a sword grip, so the grip has to be authored in Blender, as we already do.
- Resampling 30 fps to 25 fps stays on our side (Blender/retarget). Kimodo has no option for it.

### Gaps
- I found no documentation of `posed_joints_norootpos` beyond the code (it is the FK pose with the root at the origin).

## 5. Limits: duration, frames, batch, memory, fps

### Takeaway
The model runs at 30 fps, with a documented maximum of 10 s (300 frames) per prompt. The demo's prompt boxes allow 2–10 s, below the length of our 1.2–1.7 s dashes and rolls. The code sets no cap on samples. The batch through the denoiser is `num_samples × 3` under separated CFG. Fewer than 20 constrained frames per constraint type is recommended (dense root paths excepted).

### Cited Findings
- "Maximum duration is 10 sec (300 frames at 30 frames per second)", "Motions are at 30 frames per second" — [HF snapshot README.md](dev-ubuntu:~/.cache/huggingface/hub/models--nvidia--Kimodo-SOMA-RP-v1.2/snapshots/0e3c89024d896064b6851201373f7834784a386b/README.md); "Maximum generated motion duration is 10 sec per prompt" — [limitations.md#L18](dev-ubuntu:~/gothic-ai/kimodo/src/docs/source/key_concepts/limitations.md#L18)
- Demo: `MIN_DURATION = 2.0`, `MAX_DURATION = 10.0`, with the comment "default start duration is 6.0 sec, but model can handle up to 10 sec". It also sets `INIT_POSTPROCESSING = True` and `NB_TRANSITION_FRAMES = 5` — [kimodo/demo/config.py#L32-40](dev-ubuntu:~/gothic-ai/kimodo/src/kimodo/demo/config.py#L32)
- The CLI enforces neither a minimum nor a maximum duration. The positional encoding buffer has `max_len = 5000` positions, which includes the 50 text tokens and the prefix — [generate.py#L131-149](dev-ubuntu:~/gothic-ai/kimodo/src/kimodo/scripts/generate.py#L131); [backbone.py#L235-280](dev-ubuntu:~/gothic-ai/kimodo/src/kimodo/model/backbone.py#L238)
- "The number of constrained frames per constraint type should be less than 20 (excluding the root path constraint)" — [limitations.md#L14, #L19](dev-ubuntu:~/gothic-ai/kimodo/src/docs/source/key_concepts/limitations.md#L14)
- The batch is `num_samples`. Separated CFG concatenates three copies of the batch for each denoiser call — [cfg.py#L97-124](dev-ubuntu:~/gothic-ai/kimodo/src/kimodo/model/cfg.py#L97); [kimodo_model.py#L609-610](dev-ubuntu:~/gothic-ai/kimodo/src/kimodo/model/kimodo_model.py#L609)
- Speed observed in our probe: 100 DDIM steps at about 12.2 it/s, about 8 s for 8 samples × 45 frames on the RTX 3090 — [probe1/barge/gen.log](dev-ubuntu:~/gothic-ai/kimodo/probe1/barge/gen.log)
- `TEXT_ENCODER_DEVICE=cpu` "reduces VRAM usage to <3 GB with a fairly small speed hit" — [CHANGELOG.md 2026-04-24](dev-ubuntu:~/gothic-ai/kimodo/src/CHANGELOG.md). Our runner states that generation took under 3 GB of VRAM with the encoder on the CPU in bf16 (about 16 GB RAM, capped at 22 GB). That is our own note, not Kimodo's — [run_kimodo.sh](dev-ubuntu:~/gothic-ai/kimodo/run_kimodo.sh)
- The LLM2Vec encoder always uses internal batch size 1 for repeatability. Default precision is bf16, and `text_encoder_fp32` exists (the benchmark notes that v1.1 and TMR were trained with fp32 embeddings) — [llm2vec_wrapper.py#L71-82](dev-ubuntu:~/gothic-ai/kimodo/src/kimodo/model/llm2vec/llm2vec_wrapper.py#L71); [load_model.py#L68-80](dev-ubuntu:~/gothic-ai/kimodo/src/kimodo/model/load_model.py#L68); [docs/source/benchmark/results.md#L7](dev-ubuntu:~/gothic-ai/kimodo/src/docs/source/benchmark/results.md#L7)

### Inferences
- Our 1.2 s (36-frame) dashes and 1.7 s rolls are below the demo's 2 s minimum. The authors probably did not intend clips that short, and that may contribute to the slow dashes. A cheap test is to generate 2–3 s clips with a `root2d` waypoint or fullbody keyframe placed early, so the dash ends by frame about 10–15, then trim.
- The documented maximum of 10 s per prompt makes long chains a multi-prompt job.

### Gaps
- No peak VRAM for a given `num_samples` and duration is documented. The minimum length in the training data is not stated in the repo (the tech report was not read here).

## 6. Best practice from the repo's docs and examples

### Takeaway
The docs recommend these prompt habits:
- start with "A person…";
- describe one or two behaviours per prompt, at a medium level of detail (the BONES-SEED annotation style);
- make each prompt of a sequence self-contained.

For constraints they recommend sparse keyframes (fewer than 20 per type), no conflicts between constraints and text, and post-processing on. Training data includes "videogame combat". The benchmark shows which constraint layouts the model was evaluated on: full-body in-betweening (start + end), full-body at random frames, hands and/or feet pos+rot, root waypoints and paths with or without heading, and mixtures.

### Cited Findings
- Prompting advice: begin with "A person…", optionally stylised ("An old person…", "A drunk person…"). "Keep each prompt focused one or at most two behaviors". Use "a medium level of detail", since "very long prompts describing detailed motion of each body part will be too much for the model". Look at BONES-SEED prompts for the right granularity. In multi-prompt sequences each prompt must "have enough information on its own" — [limitations.md#L5-10](dev-ubuntu:~/gothic-ai/kimodo/src/docs/source/key_concepts/limitations.md#L5)
- Coverage of the training data: "locomotion, gestures, everyday activities, common object interactions, videogame combat, dancing, and various styles including tired, angry, … injured, stealthy, old". Prompts outside this coverage "will likely give bad results" (the docs' example is baseball) — [limitations.md#L9](dev-ubuntu:~/gothic-ai/kimodo/src/docs/source/key_concepts/limitations.md#L9); [docs/source/key_concepts/model.md#L24](dev-ubuntu:~/gothic-ai/kimodo/src/docs/source/key_concepts/model.md#L24)
- Constraint advice: avoid constraints that contradict the text or each other, and adjust the CFG weights for text-versus-constraint trade-offs. Use sparse temporal constraints, except dense 2D root paths. "When foot contact accuracy and hitting constraints is high priority, make sure to enable post-processing". "Conflicting constraints: can lead to artifacts or constraints that are ignored" — [limitations.md#L12-23](dev-ubuntu:~/gothic-ai/kimodo/src/docs/source/key_concepts/limitations.md#L12)
- Benchmark constraint layouts:
  - `fullbody/inbetweening` ("constraints at start and end only") and `fullbody/random`;
  - `end-effectors/{feet_posrot, hands_posrot, hands_feet_posrot}`;
  - `root/{path_2dpos, path_2dposrot, waypoint_2dpos, waypoint_2dposrot}`;
  - `mixture/{root_ee_hands_feet_posrot_fullbody, root_ee_hands_posrot, root_ee_hands_posrot_fullbody, root_path_fullbody}`.

  Cases exist both with text and without — [docs/source/benchmark/introduction.md#L97-130](dev-ubuntu:~/gothic-ai/kimodo/src/docs/source/benchmark/introduction.md#L97)
- Model-only constraint error for Kimodo-SOMA-RP-v1.2 on the benchmark (cm, with text, Content split): FB Pos 2.967, EE Pos 3.216, 2D Root 4.658, Pelvis@95% 7.76. The ArdyRep-v1.2 row reads FB 3.063, EE 3.434, 2D Root 2.904. The tables print constraint position errors in cm — [docs/source/benchmark/results.md#L142-146](dev-ubuntu:~/gothic-ai/kimodo/src/docs/source/benchmark/results.md#L142); [docs/source/benchmark/metrics.md#L49-50](dev-ubuntu:~/gothic-ai/kimodo/src/docs/source/benchmark/metrics.md#L49)
- The README of the v1.2 checkpoint says that compared with v1.1 it "reduces foot skating and matches constraint accuracy" while text-following (R-precision/FID) is "slightly lower" — [HF snapshot README.md](dev-ubuntu:~/.cache/huggingface/hub/models--nvidia--Kimodo-SOMA-RP-v1.2/snapshots/0e3c89024d896064b6851201373f7834784a386b/README.md)
- Example prompts in the shipped SOMA examples (single sentence, "A person …", one to two actions): "A person runs forward and then leaps over an obstacle in front of them.", "A person picks up an object in front of them with two hands and places it to the left side", "A person walking backward points to the right side with their right hand", "A zombie with their left arm extended forward walks with an uneven gait at a slow pace." Seeds are 42–49, steps 100, CFG 2.0/2.0 — [kimodo/assets/demo/examples/kimodo-soma-rp/*/meta.json](dev-ubuntu:~/gothic-ai/kimodo/src/kimodo/assets/demo/examples/kimodo-soma-rp)
- Tip from the docs: "the easiest way to get a valid constraints file is to create constraints in the interactive demo and to click on `Save Constraints`" — [docs user_guide/constraints.md#L6](dev-ubuntu:~/gothic-ai/kimodo/src/docs/source/user_guide/constraints.md#L6)

### Inferences
- Our probe prompts pack three or four actions into one prompt. The overhead strike, for example, has "raises … holds … swings it down hard … returns to the fighting stance", which exceeds the docs' "one or at most two behaviors". It is also reduced to a single 4096-d sentence vector (section 3). That may explain why the downswing and the thrust were lost. Options: shorter prompts per phase, multi-prompt chaining, or carrying the phase content in keyframes instead of text. For instance, use an intermediate fullbody keyframe at the impact frame plus `right-hand` keyframes at wind-up and impact, both trained layouts ("fullbody/random", "hands_posrot", "mixture").
- The mocap data contains no sword, so phrases like "thrusts a sword … into the stomach" depend on whatever weapon-like "videogame combat" clips exist. A shoulder barge has no contact target in Kimodo. Contact timing and pose would come from keyframes, not from text.
- Post-processing gives exact keyframe hits for fullbody, hand and foot, and root2d types. Constraint strength in CFG can therefore be traded for text adherence, as long as the keyframe poses themselves carry the weapon intent.

### Gaps
- The docs give no recommended CFG values beyond the defaults (2.0/2.0), and no guidance on prompts for weapon or combat motions specifically. The BONES-SEED annotation style itself was not checked here.

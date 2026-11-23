# NVIDIA Kimodo: official and first-hand sources (state as of 2026-11-01)

Sources read for this note: the GitHub repo `nv-tlabs/kimodo` at commit `e1c219c` (README, CHANGELOG, the whole `docs/source` tree, the code in `kimodo/constraints.py`, `kimodo/skeleton/*`, `kimodo/model/*`, `kimodo/postprocess.py`, the demo example `meta.json` files); every GitHub issue (#1–#56; the repo has no Discussions tab); the tech report PDF (arXiv 2603.15546, 21 pages); the Hugging Face model cards for SOMA-RP v1, v1.1, v1.2 and SEED v1.1; the BONES-SEED and SEED-Timeline-Annotations dataset cards; the Bones Studio datasets page; the NVIDIA Open Model License; the Llama 3 Community License. "Second-hand" marks anything that does not come from NVIDIA or Bones.

## 1. What Kimodo is: report, releases, variants, training data, caption style

### Takeaway
Kimodo is a 282M-parameter kinematic motion diffusion model from NVIDIA (two-stage transformer, DDIM sampling, LLM2Vec/Llama-3-8B text encoder). It is trained on Bones Rigplay 1: 700 h of Vicon optical mocap, body only (no fingers), with "videogame combat" and stunt performers among the content. The newest checkpoint is **Kimodo-SOMA-RP-v1.2 (2026-10-15)**, now the CLI and demo default. The model was trained on captions rewritten by an LLM into one fixed form, "A [subject] …". It was trained on single-character mocap: it knows no props, no scene objects and no second character.

### Cited Findings
- **Report:** "Kimodo: Scaling Controllable Human Motion Generation", Rempe, Petrovich et al., NVIDIA. arXiv:2603.15546, published 2026-03-16; the PDF is dated 2026-3-16. — [Tech report PDF](https://research.nvidia.com/labs/sil/projects/kimodo/assets/kimodo_tech_report.pdf); [arXiv](https://arxiv.org/abs/2603.15546)
- **Release timeline.** All dates come from the README "News" and the CHANGELOG — [README](https://github.com/nv-tlabs/kimodo); [CHANGELOG](https://github.com/nv-tlabs/kimodo/blob/main/CHANGELOG.md)
  - 2026-03-16: first open-source release with five variants (SOMA-RP-v1, G1-RP-v1, SOMA-SEED-v1, G1-SEED-v1, SMPLX-RP-v1).
  - 2026-03-19, breaking: inputs and outputs moved to the 77-joint SOMA skeleton (`somaskel77`).
  - 2026-03-31: `kimodo_convert`, plus loading of BVH/CSV/NPZ files in the demo.
  - 2026-04-10: v1.1 SOMA models and the benchmark.
  - 2026-04-13: BVH export with a standard T-pose (`--bvh_standard_tpose`).
  - 2026-04-24: better multi-prompt transitions; `TEXT_ENCODER_DEVICE=cpu`; a fix for a post-processing bug that "was incorrectly making the smoothed root the target for the root in full-body constraints".
  - 2026-07-10: ARDY, a real-time sibling model.
  - 2026-10-15: SOMA-RP-v1.2 and SOMA-RP-ArdyRep-v1.2.
- **v1.2 vs v1.1.** v1.2 is trained on corrected splits: some BONES-SEED test sequences had leaked into v1.1's training set. Compared to v1.1, v1.2 "reduces foot skating and matches constraint accuracy; its text-following scores (R-precision/FID) are slightly lower". v1.2 is "ready for commercial use". — [HF card Kimodo-SOMA-RP-v1.2](https://huggingface.co/nvidia/Kimodo-SOMA-RP-v1.2)
- **v1.1 vs v1.** A slightly larger training set, plus "further data cleaning performed to remove problematic motions with wrist and shoulder twist artifacts" and improved training stability. — [HF card Kimodo-SOMA-RP-v1.1](https://huggingface.co/nvidia/Kimodo-SOMA-RP-v1.1)
- **ArdyRep-v1.2** uses ARDY's motion representation ("raw hips as the root") and exists for comparison with ARDY. The plain SOMA-RP-v1.2 keeps the smoothed root. — [README](https://github.com/nv-tlabs/kimodo)
- **Recommended variants.** The authors recommend the RP models (700 h). The SEED models use 288 h of public data and "are less capable". SMPL-X "is **not** the recommended Kimodo model to use since generated motions may display particularly severe retargeting artifacts". — [README](https://github.com/nv-tlabs/kimodo); [Skeletons doc](https://research.nvidia.com/labs/sil/projects/kimodo/docs/key_concepts/skeleton.html)
- **Skeleton.** The model works internally on `somaskel30` (30 joints, "removes most finger and hand detail") and converts the output to `somaskel77` for export. All training data uses one uniform skeleton proportion, i.e. a single set of SOMA identity parameters. — [Skeletons doc](https://research.nvidia.com/labs/sil/projects/kimodo/docs/key_concepts/skeleton.html)
- **Hands.** Only `Hand`, `HandThumbEnd` and `HandMiddleEnd` are kept, and the two end joints are leaves with no rotation. The maintainer says they were kept "to enable more intuitive end-effector positional constraints". Fingers are not generated. — [Issue #2, maintainer 2026-03-18](https://github.com/nv-tlabs/kimodo/issues/2); [Issue #28, 2026-04-23](https://github.com/nv-tlabs/kimodo/issues/28)
- **Architecture.**
  - Explicit diffusion on pose features, with a two-stage transformer: a root denoiser, then a body denoiser. Each stage has 16 layers, 8 heads and latent size 1024; 282M parameters in total.
  - Text conditioning: a 4096-d LLM2Vec embedding, which beat CLIP and T5 in early experiments.
  - Also conditioned on a first-frame heading token and 49 extra zero tokens.
  - Training: 1000 diffusion steps, 30 fps, batch 2048 on 16 A100s. Clips are cropped to at most 10 s.
  - Source: [Tech report §4.2–4.3](https://research.nvidia.com/labs/sil/projects/kimodo/assets/kimodo_tech_report.pdf)
- **Training curriculum.**
  - Phase 1, 500k steps: text-to-motion only.
  - Phase 2, 500k steps: text plus constraints, sampled from fixed patterns: sparse full-body joint positions; random subsets of hand and foot positions and rotations; sparse 2D root position and heading; dense 2D root paths; sparse foot contacts.
  - Two patterns are mixed 25% of the time; 10% of samples have no constraints.
  - The maximum number of sparse keyframes ramps from 1 to 20, "biased towards fewer keyframes".
  - Text is dropped 10% of the time to enable CFG.
  - Source: [Tech report §4.3](https://research.nvidia.com/labs/sil/projects/kimodo/assets/kimodo_tech_report.pdf)
- **Training data (Bones Rigplay 1).**
  - 700 h from 170 subjects, roughly equal male and female, with "thousands of unique actions".
  - Coverage: "locomotion, gestures, everyday activities, common object interactions, videogame combat, dancing, athletics, and more".
  - Styles: tired, angry, happy, sad, scared, drunk, injured, stealthy, old, childlike.
  - Body-only motions, retargeted to a uniform 27-joint skeleton and from there to SOMA, G1 and SMPL-X.
  - Source: [Tech report §3](https://research.nvidia.com/labs/sil/projects/kimodo/assets/kimodo_tech_report.pdf)
- **Bones' own description of RP01.**
  - "700+ hours", "170+ physically diverse performers" including "stuntmen, soldiers, and dancers".
  - Vicon optical capture at 120 fps, "20+ styles".
  - Performance category: "dancing, sports, martial arts, stunts, combat".
  - The page does not name swords, weapons, hit reactions, rolls or two-person interactions.
  - Source: [Bones Studio datasets page](https://bones.studio/datasets)
- **Public BONES-SEED subset** (shows what part of the data looks like):
  - 142,220 motions (71,132 original + 71,088 mirrored), about 288 h, 522 performers, 120 fps.
  - Packages: Locomotion 74,488; Communication 21,493; Interactions 14,643; Dances 11,006; **Gaming 8,700**; Everyday 5,816; Sport 3,993; Other 2,081.
  - Categories include **Stunts 858**, Magic 160 and **Martial Arts 20**.
  - Source: [BONES-SEED card](https://huggingface.co/datasets/bones-studio/seed)
- **Caption style.**
  - Each Rigplay clip has an **overview** description of the whole motion and **fine-grained atomic-action sub-clip** descriptions.
  - Qwen3-32B paraphrases the text "into a consistent prompt structure (always starting with 'A [subject]...') with various levels of detail".
  - Motions are also augmented by stitching random pairs of clips, with Kimodo-generated transitions between them.
  - Source: [Tech report §3](https://research.nvidia.com/labs/sil/projects/kimodo/assets/kimodo_tech_report.pdf)
- **SEED-Timeline-Annotations (CC BY 4.0).**
  - 352,703 segments in total, 2.48 segments per motion on average.
  - 37,943 unique human-written segment descriptions; 314,760 more were propagated automatically to other takes with dynamic time warping.
  - Source: [SEED-Timeline-Annotations card](https://huggingface.co/datasets/nvidia/SEED-Timeline-Annotations)
- **Real SEED captions** from the dataset viewer search (first-hand data):
  - Dodges: "A person quickly dodges backward into a ready-for-fight stance, then straightens upright", with sub-clips "A person standing still suddenly ducks and leans diagonally while quickly dodging backward into a ready-for-fight stance." and "A person standing straight moves and bends towards the right diagonal while dodging an attack."
  - Rolls: "A person performs a quick roll to the right side while facing left." and "A person quickly rolls forward on their back and stands up in a controlled motion."
  - The query "sword" matched only 2 of 142k SEED motions. Both are a walking-sideways clip whose overview says "holding a sword", while the fine-grained caption says "holds a small, lightweight object with one hand".
  - Source: [SEED-Timeline-Annotations (viewer)](https://huggingface.co/datasets/nvidia/SEED-Timeline-Annotations)
- **Model card limitations:** "The model is not aware of objects in the scene around a character", it "cannot create cartoon motions or non-physically plausible motions", and "Each trained model currently outputs motion for a single character skeleton". — [HF card v1.2](https://huggingface.co/nvidia/Kimodo-SOMA-RP-v1.2)
- **Speed:** on an RTX 3090 one prompt takes 2–5 s depending on duration. Kimodo "is designed as an offline motion authoring model". — [Tech report §2.1](https://research.nvidia.com/labs/sil/projects/kimodo/assets/kimodo_tech_report.pdf)

### Inferences
- **Combat:** Rigplay contains game combat and stunt performers, so dodges, rolls, falls and fight stances are in the distribution. Our probe agrees: body moves worked. Weapons, however, appear in the captions only as a "held object", sometimes with generic wording. There is no prop channel at all, so "sword" mostly reads as "something held in the right hand", not as a strike trajectory.
- **Our riposte** ("…thrusts a sword … into the stomach of an enemy…") made the character double over. A plausible reading: the model has no second character, so the body-part semantics ("stomach", "pain") attach to the only body it has. This is our inference, not an author statement.
- **Weapon arcs in the data.** The public SEED subset has only 20 "Martial Arts" motions; its "Gaming" package (8,700) may hold more. Whether the proprietary Rigplay has two-handed or one-handed sword swings is not documented publicly (see Gaps).

### Gaps
- No official list of Rigplay's combat content: armed vs unarmed, sword or axe swings, thrusts, parries, hit reactions, two-person fights. Bones only says "combat" and "martial arts".
- I could not inspect the SEED "Gaming" package captions in bulk. The dataset viewer's search API kept failing with authentication errors for "slash", "stab", "punch", "block" and "attack", and the SEED BVH set is gated.

## 2. Constraint types, their exact semantics, how they combine, and CFG

### Takeaway
Kimodo conditions by **imputation**: constraint features overwrite the noisy motion, and a mask is concatenated to it. Five types exist, each with exact semantics in the code:
- **Full-body keyframes** pin **joint positions only, not joint rotations**.
- **End-effector keyframes** pin the position *and global rotation* of a hand, a foot or the hips. Only these five chains are available, not arbitrary joints.
- **Root-2D waypoints and paths** act on the *smoothed* root.
- **Foot contacts** are trained but not exposed.

CFG is "separated" by default with text 2.0 and constraint 2.0.

### Cited Findings
- **Imputation.** Constraints "are used to overwrite the corresponding values in the noisy motion. Additionally, a mask indicating which elements are constrained is concatentated to the input motion." — [Model overview doc](https://research.nvidia.com/labs/sil/projects/kimodo/docs/key_concepts/model.html)
- **Gradient guidance.** The authors tried gradient-based test-time guidance and dropped it: it "gave minimal improvement, substantially increased generation time, and was generally unstable and difficult to tune." — [Tech report §4.4](https://research.nvidia.com/labs/sil/projects/kimodo/assets/kimodo_tech_report.pdf)
- **Pose features at each frame:**
  - smoothed root position (x/z heavily smoothed, y kept);
  - root heading as cos/sin, computed from the left-to-right hip vector;
  - joint positions: x/z relative to the smoothed root, y global, **not** rotated into the heading frame;
  - global joint velocities;
  - **global** joint rotations in 6D form;
  - four foot-contact flags (left heel, left toe, right heel, right toe).

  Source: [Tech report §4.1](https://research.nvidia.com/labs/sil/projects/kimodo/assets/kimodo_tech_report.pdf); [Motion representation doc](https://research.nvidia.com/labs/sil/projects/kimodo/docs/key_concepts/motion_representation.html)
- **Constraint types as the docs define them:**
  - Sparse root-2D waypoints and dense root-2D paths act on the 2D components of the *smoothed* root.
  - A full-body keyframe constrains "the smoothed root position and all body joint positions at a specific frame".
  - An end-effector constraint "constrains the smoothed root position along with the specified end-effectors. For hands, this will constrain the wrist position and rotation along with the hand end position. For feet, it constraints the heel position and rotation along with the toe position. Kimodo is trained to support arbitrary subsets of end-effectors."
  - Foot contacts: "While the model is trained to support this, it is not currently implemented in the demo UI or Python API."

  Source: [Constraints concepts doc](https://research.nvidia.com/labs/sil/projects/kimodo/docs/key_concepts/constraints.html)
- **Full-body keyframes ignore rotations (code).** `FullBodyConstraintSet.update_constraints` writes global joint positions for all joints, `smooth_root_2d`, root y and global heading. It contains the comment "# global rotations are not used here". The JSON docs agree: "the `local_joint_rot` will not explicitly be constrained, the constraint will be on the joint positions that results from FK with the given joint rotations." — [constraints.py](https://github.com/nv-tlabs/kimodo/blob/main/kimodo/constraints.py); [Constraints JSON doc](https://research.nvidia.com/labs/sil/projects/kimodo/docs/user_guide/constraints.html)
- **End-effector joint names (code).** The allowed names are exactly `["LeftFoot", "RightFoot", "LeftHand", "RightHand", "Hips"]`.
  - On `somaskel30`, `RightHand` pins the positions of `RightHand` and `RightHandMiddleEnd` and the global rotation of `RightHand`.
  - `RightFoot` pins the positions of `RightFoot` and `RightToeBase` and the rotation of `RightFoot`.
  - `Hips` pins the pelvis.
  - An end-effector keyframe also writes the smoothed root 2D, root y and heading.
  - No public API pins the spine, chest, head, elbow or shoulder alone.

  Source: [skeleton/base.py `expand_joint_names`](https://github.com/nv-tlabs/kimodo/blob/main/kimodo/skeleton/base.py); [skeleton/definitions.py](https://github.com/nv-tlabs/kimodo/blob/main/kimodo/skeleton/definitions.py); [constraints.py](https://github.com/nv-tlabs/kimodo/blob/main/kimodo/constraints.py)
- **The authors pin hand rotation the same way themselves.** Multi-prompt transitions add a `FullBodyConstraintSet` *and* a separate `EndEffectorConstraintSet` on `["LeftHand","RightHand","LeftFoot","RightFoot"]` over the overlap frames, with the code comment "# separate end-effector constraint to capture hand/feet rotations". — [kimodo_model.py `_multiprompt`](https://github.com/nv-tlabs/kimodo/blob/main/kimodo/model/kimodo_model.py)
- **JSON types:** `root2d`, `fullbody`, `left-hand`, `right-hand`, `left-foot`, `right-foot`, `end-effector` (with `joint_names`).
  - `fullbody` takes `local_joints_rot` (axis-angle, radians) and `root_positions`, plus an optional `smooth_root_2d`. If `smooth_root_2d` is omitted, the XZ of `root_positions` is used.
  - `root2d` takes `smooth_root_2d` and an optional `global_root_heading` given as a [cos, sin] pair, not radians.

  Source: [Constraints JSON doc](https://research.nvidia.com/labs/sil/projects/kimodo/docs/user_guide/constraints.html)
- **Coordinate frame:**
  - Y-up, +Z forward, meters.
  - The smoothed root must be at XZ (0, 0) at frame 0.
  - `root_positions` Y is the absolute hip height (about 0.9 m standing, about 0.7 m crouching).
  - The initial heading is set with `first_heading_angle`, default 0, facing +Z.
  - A constraint at frame 0 is not required, but later constraints must be "reachable from that starting configuration given the text prompt and motion duration".

  Source: [Constraints JSON doc](https://research.nvidia.com/labs/sil/projects/kimodo/docs/user_guide/constraints.html); [Issue #5, maintainer 2026-04-10](https://github.com/nv-tlabs/kimodo/issues/5)
- **Sparse vs dense.** "Except for dense 2d root paths, Kimodo is mainly trained to handle sparse temporal constraints. Kimodo will perform best when the number of constraints per constraint type is less than 20 keyframes." — [Best practices doc](https://research.nvidia.com/labs/sil/projects/kimodo/docs/key_concepts/limitations.html)
- **Dense paths must span the clip.** The demo's "Make Smooth Path" turns waypoints into a dense path and auto-adds waypoints at the first and last frames "since Kimodo is only trained on full-sequence paths." — [Demo constraints doc](https://research.nvidia.com/labs/sil/projects/kimodo/docs/interactive_demo/constraints.html)
- **Combining constraints.** "Constraints can be mixed arbitrarily to achieve desired motions". In-betweening means "full-body constraints placed at the start and end of the generated sequence". — [Tech report §2.2](https://research.nvidia.com/labs/sil/projects/kimodo/assets/kimodo_tech_report.pdf). Docs: "Avoid using constraints that contradict the given text prompt or other types of constraints", and "Conflicting constraints: can lead to artifacts or constraints that are ignored". — [Best practices doc](https://research.nvidia.com/labs/sil/projects/kimodo/docs/key_concepts/limitations.html)
- **What happens between keyframes.** "the model pose is only constrained at the specific timestep given. There is no guarantee what happens before or after the constraint". Whether the pose is reached early or only at the keyframe "is completely up to the random seed and text prompt." — [Issue #21, maintainer 2026-04-07](https://github.com/nv-tlabs/kimodo/issues/21)
- **Heading in joint positions.** The joint positions are not forced to agree with the root heading, but "when the root heading is constrained during denoising, the joint positions reasonably follow the constraint as well." — [Issue #7, maintainer 2026-03-25](https://github.com/nv-tlabs/kimodo/issues/7)
- **Accuracy before post-processing** (30 fps SOMA model): mean 3.21 cm joint error for full-body keyframes; 3.63 cm and 6.88° for end-effectors; 3.63 cm root error. "Light post-processing can be applied … to ensure they *exactly* hit user constraints". — [Tech report §2.2](https://research.nvidia.com/labs/sil/projects/kimodo/assets/kimodo_tech_report.pdf)
- **Smoothed root vs pelvis.** The 95th percentile of the gap between a smoothed-root constraint and the projected pelvis was 9.7 cm for the full model, against 6.3 cm in ground truth (20 fps ablation models). A model built without the smoothed root followed a straight-line constraint with an unnatural "stealthy/sneaking style or old person style". — [Tech report §6.1–6.2, Table 1](https://research.nvidia.com/labs/sil/projects/kimodo/assets/kimodo_tech_report.pdf)
- **CFG formulas.**
  - `regular`: `out_uncond + w*(out_text_and_constraint − out_uncond)`.
  - `separated`: `out_uncond + w_text*(out_text − out_uncond) + w_constraint*(out_constraint − out_uncond)`. The text-only branch sees no constraints; the constraint-only branch sees no text.
  - Defaults are `[2.0, 2.0]` separated (`kimodo_model.py`, `demo/generation.py`). All eight SOMA demo examples use 2.0/2.0 with 100 steps.
  - The CLI doc gives `--cfg_weight 2.0 1.5` as an example and calls separated "the usual setting in this project".

  Source: [Generation parameters doc](https://research.nvidia.com/labs/sil/projects/kimodo/docs/user_guide/configuration.html); [CLI doc](https://research.nvidia.com/labs/sil/projects/kimodo/docs/user_guide/cli.html); [demo examples](https://github.com/nv-tlabs/kimodo/tree/main/kimodo/assets/demo/examples/kimodo-soma-rp)
- **Tuning CFG.** "If you are having trouble with a tradeoff between constraint and text accuracy, try adjusting the classifier-free guidance weights." — [Best practices doc](https://research.nvidia.com/labs/sil/projects/kimodo/docs/key_concepts/limitations.html). For a backflip that would not appear, the maintainer used 100 steps and text CFG 2.0 and advised users with lower values to go "back to defaults or even higher". — [Issue #29, maintainer 2026-04-23](https://github.com/nv-tlabs/kimodo/issues/29)
- **Second-hand: a Unity front end** offers "full-body poses with per-joint activation for partial constraints" and per-segment CFG. — [Issue #47](https://github.com/nv-tlabs/kimodo/issues/47) → [Kimodo-Unity](https://github.com/Amin-HP/Kimodo-Unity). The official API and docs offer no per-joint full-body masks: the only subsets are the end-effector chains above. A Unity mode of that kind would be custom masking outside the trained patterns.

### Inferences
- **Why our wrist spun freely:** our probe pins the stance with `fullbody` keyframes only, and those pin positions only. Hand twist (the roll around the forearm) is barely determined by the `Hand` and `HandMiddleEnd` positions, so the sword-hand roll was left free.
  - The fix the code suggests is to add a `right-hand` (and optionally `left-hand`) end-effector constraint on the same frames. It pins the `RightHand` **global rotation**, the same trick the authors use for multi-prompt transitions.
  - The rotation must be computed by FK on the SOMA skeleton from our retargeted stance.
- **Pose keys for weapon moves.** Hands are the only upper-body handles. For a strike, the levers are end-effector keyframes on `RightHand` (position plus rotation) at the wind-up, impact and recovery frames, plus `fullbody` keys for the torso at those frames. A Gothic attack animation retargeted to SOMA can supply them. Stay under about 20 keyframes per type.
- **Dash speed.** The root timing can be pushed with `root2d` waypoints at intermediate frames, e.g. 80% of the travel at about 0.5 s instead of only an end point. A dense path must cover the whole clip.
  - The constraint acts on the *smoothed* root, so the pelvis can still lag by about 10 cm (95th percentile).
  - Post-processing `root_margin` (0.04 m) pulls the root back when it drifts further than that.
- **Separated CFG.** The text branch never sees the constraints. Lowering `w_constraint` (e.g. 1.0–1.5) and raising `w_text` (2.5–3) should shift the trade-off towards the action and away from "the easiest motion between the two stance poses"; post-processing still snaps the constraints exactly. This is inference from the formula, not an author recommendation of specific numbers.

### Gaps
- No official guidance on CFG values above 2–3 or below 1, and no ablation of the CFG weights in the report.
- Foot-contact constraints exist in training but not in the API, so they cannot be used without modifying the code.

## 3. Recommended generation settings, limits and post-processing

### Takeaway
Defaults are DDIM 100 steps, separated CFG 2.0/2.0, post-processing on in the CLI and demo, 5 transition frames between prompts, and 30 fps. Hard limits: 10 s per prompt (300 frames), fewer than 20 keyframes per constraint type, and in the demo at least 2 s per prompt segment. The authors' recipe for a hard motion is: prompts that start with "A person…", one or two behaviors at medium detail, several seeds, rephrasing, and post-processing on.

### Cited Findings
- **Prompting:**
  - "begin each prompt with 'A person...'". The subject can be stylized ("An old person…", "A drunk person…").
  - "Keep each prompt focused one or at most two behaviors. For long sequences of action, split them into multiple prompts".
  - Use "a medium level of detail"; "very long prompts describing detailed motion of each body part will be too much for the model". Read the BONES-SEED prompts to calibrate granularity.
  - Out-of-domain actions (the example is baseball) "will likely give bad results".
  - In multi-prompt work each prompt must stand alone: "Then the person stops" lacks context.

  Source: [Best practices doc](https://research.nvidia.com/labs/sil/projects/kimodo/docs/key_concepts/limitations.html)
- **Prompt length vs content.** The report says the model "can handle potentially complex text descriptions" with sequential ("then") or simultaneous ("while") actions. A sequence the model struggles with as one prompt can be split into several. — [Tech report §2.1](https://research.nvidia.com/labs/sil/projects/kimodo/assets/kimodo_tech_report.pdf)
- **Wording on the model card.** Prompts should describe actions "in neutral, physical terms (e.g., 'A person walks slowly with shuffled steps')" rather than with demographic adjectives. — [HF card v1.2](https://huggingface.co/nvidia/Kimodo-SOMA-RP-v1.2)
- **Limits:** "Maximum generated motion duration is 10 sec per prompt"; the model card says 300 frames at 30 fps. The number of constrained frames per type should stay below 20. — [Best practices doc](https://research.nvidia.com/labs/sil/projects/kimodo/docs/key_concepts/limitations.html); [HF card v1.2](https://huggingface.co/nvidia/Kimodo-SOMA-RP-v1.2)
- **Prompt boxes in the demo:** 2–10 s. The code sets `min_duration … # 2 seconds minimum`. — [UI overview doc](https://research.nvidia.com/labs/sil/projects/kimodo/docs/interactive_demo/ui_overview.html); [demo/app.py](https://github.com/nv-tlabs/kimodo/blob/main/kimodo/demo/app.py)
- **Steps:** "Fewer steps (50-100): Faster inference, slightly lower quality"; "More steps (100-200): Higher quality, slower inference". — [Generation parameters doc](https://research.nvidia.com/labs/sil/projects/kimodo/docs/user_guide/configuration.html)
- **CLI defaults:** `--duration 5.0`, `--diffusion_steps 100`, `--num_samples 1`, `--num_transition_frames 5`, `--seed`. `--no-postprocess` turns off "foot skate cleanup and constraint optimization", so post-processing is on by default. The CLI default model is now Kimodo-SOMA-RP-v1.2. `--input_folder` reads `meta.json`; explicit CFG flags override its `cfg` block. — [CLI doc](https://research.nvidia.com/labs/sil/projects/kimodo/docs/user_guide/cli.html)
- **Post-processing:**
  - "Simple foot locking and IK can clean up any undesirable foot skate using the foot contact classification directly from the model output", plus "a short optimization on the output motion to ensure it *exactly* hits the kinematic constraints".
  - The docs describe it as a "lightweight optimization that smoothly adjusts joints while minimizing changes in acceleration and velocity".
  - It is implemented in C++ (`MotionCorrection`: `TrajectoryCorrector`, `InverseKinematics`). Parameters: `contact_threshold=0.5` and `root_margin=0.04` m ("if the skeleton root deviates more than this margin from a constraint, the post-processing will fix it").
  - The report's quantitative experiments do not use it.

  Source: [Tech report §4.4](https://research.nvidia.com/labs/sil/projects/kimodo/assets/kimodo_tech_report.pdf); [Constraints concepts doc](https://research.nvidia.com/labs/sil/projects/kimodo/docs/key_concepts/constraints.html); [postprocess.py](https://github.com/nv-tlabs/kimodo/blob/main/kimodo/postprocess.py); [Demo generation doc](https://research.nvidia.com/labs/sil/projects/kimodo/docs/interactive_demo/generation.html)
- **Without post-processing:** "The model by itself can generate foot skating and will not exactly hit constraints." Post-processing "currently does not work well for the G1 robot skeleton". — [Best practices doc](https://research.nvidia.com/labs/sil/projects/kimodo/docs/key_concepts/limitations.html)
- **Post-processing artifact:** IK can force tiptoe walking when the predicted feet are high above the floor. Fixes: try another seed, or turn post-processing off. — [Issue #18, maintainer 2026-04-01](https://github.com/nv-tlabs/kimodo/issues/18)
- **Seeds and samples:** "if you don't get the result you are hoping for with a prompt on the first try, it's best to generate multiple samples with different seeds and rephrase the prompt a few times. If it's still not able to do it, Kimodo was probably not trained on that type of motion." — [Issue #29, maintainer 2026-04-23](https://github.com/nv-tlabs/kimodo/issues/29)
- **Hardware:**
  - About 17 GB VRAM fully on GPU. `TEXT_ENCODER_DEVICE=cpu` brings this under 3 GB.
  - The text-encoder service `kimodo_textencoder` avoids reloading the encoder on every CLI call.
  - Tested mostly on RTX 3090, RTX 4090 and A100.

  Source: [Quick start doc](https://research.nvidia.com/labs/sil/projects/kimodo/docs/getting_started/quick_start.html)
- **Text encoder fixes (2026-04-10):** LLM2Vec now always runs with batch size 1, "to avoid unexpected behavior of different embeddings based on batch size"; an fp32 option was added. — [CHANGELOG](https://github.com/nv-tlabs/kimodo/blob/main/CHANGELOG.md)
- **Numerics:** with TF32 matmuls (the NGC container default), foot skate was about 10% higher than with strict fp32. The benchmark scripts now turn TF32 off. — [Issue #52](https://github.com/nv-tlabs/kimodo/issues/52); [CHANGELOG 2026-10-15](https://github.com/nv-tlabs/kimodo/blob/main/CHANGELOG.md)
- **Outputs:**
  - NPZ fields: `posed_joints`, `global_rot_mats`, `local_rot_mats`, `foot_contacts`, `smooth_root_pos`, `root_positions`, `global_root_heading`, on `somaskel77`, with the standard T-pose as rest pose.
  - BVH (SOMA only) defaults to the BONES-SEED rest pose, scaled to centimeters. `--bvh_standard_tpose` exports with the standard T-pose.

  Source: [Output formats doc](https://research.nvidia.com/labs/sil/projects/kimodo/docs/user_guide/output_formats.html)
- **Two ways to skin SOMA:** the SOMA rig or the SOMA layer. They give "very slightly different results", and the SOMA rig better reflects the training data. — [Skeletons doc](https://research.nvidia.com/labs/sil/projects/kimodo/docs/key_concepts/skeleton.html)

### Inferences
- **Our probe vs these practices.** Our prompts pack three or four actions into 1.2–2.6 s, e.g. "rushes forward and rams … then steps back into the fighting stance". That breaks "one or at most two behaviors" and sits below the demo's 2 s minimum per prompt.
  - Option: describe only the core action, with medium detail on the body's own motion.
  - Option: generate longer clips (3–5 s) with the key pose keyframed mid-clip, then trim; or chain prompts for wind-up → strike → recover.
  - Both are inference from the official guidance.
- **Steps and samples:** 100 steps is the default; 150–200 is the documented "higher quality" range. More samples per move, varying seeds and paraphrases, is the authors' own remedy.
- **Frame rate:** Kimodo is 30 fps only, so resampling to the game's 25 fps stays our job. The docs give no guidance on it.

### Gaps
- No documented recommended range for `num_transition_frames` beyond the default of 5.
- No documented effect of very short durations (under 2 s) on quality. The 2 s minimum comes from the demo UI only.

## 4. Editing features: in-betweening, timelines, transitions, looping

### Takeaway
Editing works through constraints: in-betweening by full-body keys at the clip's ends, re-posing keyframes in the demo with FK, multi-prompt timelines that generate each segment in turn (the transition happens at the start of the next segment), and loading existing motions (BVH/NPZ/CSV) into the demo to author constraints from them. There is no IK posing, no loop mode and no partial regeneration in the official demo.

### Cited Findings
- **In-betweening:** "Animators can use Kimodo to generate plausible transitions between existing mocap clips through in-betweening, with full-body constraints placed at the start and end of the generated sequence." — [Tech report §2.2](https://research.nvidia.com/labs/sil/projects/kimodo/assets/kimodo_tech_report.pdf)
- **Multi-prompt generation:**
  - Each prompt is generated in turn. The next one overlaps the previous one, with full-body keyframes for continuity, and the shared frames are blended afterwards.
  - The best-practices doc says the second motion "is conditioned on the last frames of the first, so the transition between prompts actually happens at the start of the second motion", which "may reduce the time available to realize the new prompt content".
  - CLI: sentences separated by periods, e.g. `--duration "5.0 4.0"`, and `--num_transition_frames`.

  Source: [Tech report §4.4](https://research.nvidia.com/labs/sil/projects/kimodo/assets/kimodo_tech_report.pdf); [Best practices doc](https://research.nvidia.com/labs/sil/projects/kimodo/docs/key_concepts/limitations.html); [CLI doc](https://research.nvidia.com/labs/sil/projects/kimodo/docs/user_guide/cli.html)
- **Constraints with multi-prompt:** one constraints JSON holds all constraints for the whole timeline. — [Issue #31, maintainer 2026-04-23](https://github.com/nv-tlabs/kimodo/issues/31)
- **Demo timeline:**
  - Click a track to add a constraint taken from the current pose at that frame.
  - Ctrl/Cmd+drag makes an interval constraint; drag moves a constraint.
  - "Enter Editing Mode" offers FK gizmos in local or global space; "Snap to Constraint" and "Reset Constraint" are also there.
  - Several samples can be generated; one must be picked before editing.
  - Examples can be saved and loaded with all their settings, including seed and CFG.

  Source: [UI overview doc](https://research.nvidia.com/labs/sil/projects/kimodo/docs/interactive_demo/ui_overview.html); [Demo constraints doc](https://research.nvidia.com/labs/sil/projects/kimodo/docs/interactive_demo/constraints.html); [Examples doc](https://research.nvidia.com/labs/sil/projects/kimodo/docs/interactive_demo/examples.html)
- **No IK posing:** only FK posing exists and "We do not currently plan to add an IK posing system". The demo "is far from a fully-featured animation tool"; artists are expected to put the model into Blender, Maya or Houdini themselves. — [Issue #20, maintainer 2026-04-02](https://github.com/nv-tlabs/kimodo/issues/20)
- **Importing motions:** the demo loads BVH, CSV and NPZ (since 2026-03-31). `kimodo_convert` converts between NPZ, AMASS NPZ, SOMA BVH and G1 CSV. `kimodo_gen --save_example_dir` writes outputs the demo can load (since 2026-04-24). — [CHANGELOG](https://github.com/nv-tlabs/kimodo/blob/main/CHANGELOG.md)
- **Second-hand: Blender scripts** that import and export Kimodo constraint JSON for SOMA in Blender were posted by a user. — [Issue #5, carlosedubarreto 2026-04-10](https://github.com/nv-tlabs/kimodo/issues/5)
- **Second-hand: the Unity front end** can "freeze" a segment: the boundary frames are fed back as constraints and only the other segments are regenerated. It also bakes Unity AnimationClips. — [Issue #47 / Kimodo-Unity](https://github.com/Amin-HP/Kimodo-Unity)
- **Related NVIDIA tools:** ARDY, a real-time model "with all the controllability of Kimodo" (2026-07-10), and MotionBricks, real-time "fast and robust motion in-betweening". — [README](https://github.com/nv-tlabs/kimodo)

### Inferences
- A practical way to get weapon moves is to import a Gothic attack animation (retargeted to SOMA BVH or NPZ) into the demo. From its key frames, take `fullbody` and `right-hand` keyframes and add them to a Kimodo generation; this is the in-betweening the report describes.

### Gaps
- No official looping or cycle feature was found. Loops would need start-equals-end keyframes plus our own blending.
- No official "regenerate only a range" feature exists; the Unity plugin does it its own way.

## 5. Known limitations and problems users report

### Takeaway
The authors state these limits: 10 s per prompt, sparse constraints only (except root paths), constraints that conflict get ignored, foot skate without post-processing, acrobatics weaker than other motions, no fingers, no objects, a single character, and realistic motion only. User issues add: rotation jitter in BVH in one setup, problems with the SMPL-X model, no vertical or stair control, no training or fine-tuning code, and the gated Llama-3 encoder.

### Cited Findings
- **Limits listed by the authors:**
  - motion length of 10 s per prompt;
  - fewer than 20 constrained frames per type;
  - "Overly long or complex prompts can blur motion intent";
  - "Conflicting constraints: can lead to artifacts or constraints that are ignored";
  - multi-prompt transitions eat into the second segment;
  - without post-processing, foot skate and constraints that are not hit exactly.

  Source: [Best practices doc](https://research.nvidia.com/labs/sil/projects/kimodo/docs/key_concepts/limitations.html)
- **Model card:** "The motion does not always follow the given text prompt, and the model does not know how to perform certain types of actions (e.g., the model is best at locomotion, gestures, combat, dancing, and everyday activities)". It also states: no cartoon or non-physical motion, no awareness of objects, one skeleton per model. — [HF card v1.2](https://huggingface.co/nvidia/Kimodo-SOMA-RP-v1.2)
- **Acrobatics:** "Kimodo doesn't excel at acrobatics to be honest, but it has been trained on basic acrobatic motions like flips and cartwheels." The backflip appeared with "A person executes a backflip", 5 s, 100 steps and text CFG 2.0. The user had lowered CFG to 1.2–1.4 on advice from elsewhere and saw no flip; the defaults fixed it. — [Issue #29, 2026-04-18 to 04-23](https://github.com/nv-tlabs/kimodo/issues/29)
- **Fingers:** "Kimodo only generates motion for body joints so it will not be able to mimic detailed finger motions." — [Issue #28](https://github.com/nv-tlabs/kimodo/issues/28)
- **Jitter in local rotations:**
  - In one setup (B200, CUDA 13, post-processing off) the rotation-driven BVH jittered while the position-driven viewer looked smooth.
  - The maintainer concluded "the generated motion itself has some jitters" and "the jittering goes away" when regenerated on his side; it was never reproduced.
  - Also from that thread: a user running a Qwen3-8B GGUF instead of the official encoder got broken skeletons (second-hand).

  Source: [Issue #2, 2026-03-18 to 03-28](https://github.com/nv-tlabs/kimodo/issues/2)
- **SMPL-X problems:** tiptoe walking from post-processing IK, and "the Kimodo-SMPLX model does tend to give lower-quality motions than Kimodo-SOMA, due to the retargeting process of the training data" ([Issue #18](https://github.com/nv-tlabs/kimodo/issues/18)). End-effector constraints reportedly do nothing on SMPL-X; the issue has been open since 2026-09-04 with no reply ([Issue #49](https://github.com/nv-tlabs/kimodo/issues/49)).
- **Vertical control:** how to control vertical lift precisely, e.g. 12 cm stairs, is open since 2026-04-29 with no answer. — [Issue #33](https://github.com/nv-tlabs/kimodo/issues/33)
- **Fine-tuning:** a request for training or fine-tuning code is open since 2026-04-13 with no maintainer answer. — [Issue #25](https://github.com/nv-tlabs/kimodo/issues/25)
- **A question on the 10% data scaling run** and effective epochs is open since 2026-10-29 with no answer. — [Issue #56](https://github.com/nv-tlabs/kimodo/issues/56)
- **Gated text encoder:** Kimodo requires access to the gated `meta-llama/Meta-Llama-3-8B-Instruct` ([Installation doc](https://research.nvidia.com/labs/sil/projects/kimodo/docs/getting_started/installation.html)). Users report rejected or unanswered access requests (#13, #16) and ask for Qwen3 instead (#39, open since 2026-06-16). — [Issue #16](https://github.com/nv-tlabs/kimodo/issues/16); [Issue #39](https://github.com/nv-tlabs/kimodo/issues/39)
- **BVH rest pose:** the default BVH rest pose is deliberately non-standard, to match BONES-SEED, and confused Houdini users. It was fixed with `--bvh_standard_tpose` and a T-pose BVH asset (PR #26, 2026-04-13). — [Issue #15](https://github.com/nv-tlabs/kimodo/issues/15); [Issue #22](https://github.com/nv-tlabs/kimodo/issues/22)
- **Other hardware issues:** RTX 50-series/Blackwell compatibility (open since 2026-10-02) and an unreachable Docker PyPI mirror (open since 2026-08-26). — [Issue #53](https://github.com/nv-tlabs/kimodo/issues/53); [Issue #48](https://github.com/nv-tlabs/kimodo/issues/48)
- **Scaling:** more data mostly improves constraint following and foot skate; a bigger model mostly improves text following and FID. With 10% of the data, end-effector rotation error rose from 5.36° to 10.03°. — [Tech report Table 2](https://research.nvidia.com/labs/sil/projects/kimodo/assets/kimodo_tech_report.pdf)

### Inferences
- Our failures match the stated limits. The model knows no weapon or object and no opponent. Hands carry only wrist rotation, and that only through an end-effector constraint. Our prompts packed several behaviors into short clips.
- No user report or official source covers sword or melee-weapon motion specifically, so our probe stands as first-hand evidence for that case.

### Gaps
- I did not check Hugging Face discussion threads on the model pages, so HF-side user reports are not covered here.
- No NVIDIA developer forum, GTC or SIGGRAPH talk on Kimodo turned up in my searches.

## 6. Licences: code, weights, text encoder, data, generated outputs

### Takeaway
The code, including all skeleton assets in the repo, is Apache-2.0. The SOMA-RP, SOMA-SEED and G1 weights are under the **NVIDIA Open Model License**: "commercially usable", "NVIDIA claims no ownership rights in outputs", no geographic restriction, and the model card names "Animations for game and media development" as a use case. Kimodo-SMPLX-RP-v1 is under a separate NVIDIA R&D licence. The Llama-3 text encoder brings the Meta Llama 3 Community License into the pipeline.

### Cited Findings
- **Code:** "This codebase is licensed under Apache-2.0. Note that model checkpoints and data are licensed separately". — [README](https://github.com/nv-tlabs/kimodo)
- **Skeleton assets and commercial game exports.** A user asked about the bundled SOMA skeleton assets and about shipping baked, retargeted animations in a sold game. The maintainer answered: "All files in this repo are licensed under Apache-2.0 … Generating motions with the pre-trained models are subject to the model licenses linked in the table in the readme". — [Issue #55, 2026-10-28/29](https://github.com/nv-tlabs/kimodo/issues/55)
- **Licence per checkpoint:** SOMA-RP v1, v1.1 and v1.2, ArdyRep-v1.2, SOMA-SEED, G1-RP and G1-SEED use the NVIDIA Open Model License. Kimodo-SMPLX-RP-v1 uses the "NVIDIA R&D Model" license. — [README model table](https://github.com/nv-tlabs/kimodo)
- **Model card v1.2:** "This model is ready for commercial use"; "Deployment Geography: Global"; use cases include "Animations for game and media development". — [HF card v1.2](https://huggingface.co/nvidia/Kimodo-SOMA-RP-v1.2)
- **NVIDIA Open Model License** (version of 2025-10-24):
  - "Models are commercially usable"; "NVIDIA claims no ownership rights in outputs"; "You are responsible for outputs and their subsequent uses".
  - Anyone redistributing the model or a derivative must include the licence and a Notice file reading "Licensed by NVIDIA Corporation under the NVIDIA Open Model License".
  - Rights end automatically if guardrails are bypassed, or on filing patent or copyright litigation over the model.
  - NVIDIA may update the terms.
  - US federal and Delaware law, with courts in Santa Clara County.
  - No geographic restriction found; export and sanctions laws apply.

  Source: [NVIDIA Open Model License](https://www.nvidia.com/en-us/agreements/enterprise-software/nvidia-open-model-license/)
- **Meta Llama 3 Community License** (release date 2024-04-18). If you distribute "the Llama Materials (or any derivative works thereof), or a product or service that uses any of them", you must provide the licence and "prominently display 'Built with Meta Llama 3'". It also has the 700M-MAU clause, and you "will not use the Llama Materials or any output or results of the Llama Materials to improve any other large language model". — [Llama 3 licence text, NousResearch mirror](https://huggingface.co/NousResearch/Meta-Llama-3-8B-Instruct/blob/main/LICENSE)
- **Datasets:** the timeline annotations are CC BY 4.0 ([card](https://huggingface.co/datasets/nvidia/SEED-Timeline-Annotations)). BONES-SEED has its own "bones-seed-license" and is gated ([card](https://huggingface.co/datasets/bones-studio/seed)); Bones describes SEED as open "for academic use and qualifying startups" and the full RP data as licensed commercially, "IP-clean" ([Bones datasets](https://bones.studio/datasets)). Rigplay itself is proprietary ([README](https://github.com/nv-tlabs/kimodo)).

### Inferences
- **What this means for us:**
  - Kimodo stays internal on our Linux box. We only bake animations retargeted to our own rig, and we ship neither weights nor code.
  - On that path the OML's redistribution duties (Notice file, licence copy) would not be triggered, and NVIDIA claims no rights in the outputs.
  - The Llama 3 "Built with Meta Llama 3" duty is tied to distributing the Llama Materials "or a product or service that uses any of them". A game that ships only baked animations arguably does not "use" Llama. This is a legal reading, not legal advice.
  - Use SOMA-RP models, not SMPLX-RP, because of the R&D licence.
- **EU:** neither licence restricts the EU. The OML's governing law is US/Delaware.

### Gaps
- I did not read the text of the NVIDIA R&D Model license (for SMPLX-RP) or the licence of the LLM2Vec adapter weights (McGill-NLP) used on top of Llama 3.
- EU AI Act obligations for a game that ships baked AI-generated animations are outside these sources.
- The full text of the BONES-SEED licence (bones.studio/info/seed-license) was not fetched. It matters only if we train or fine-tune on SEED or run the benchmark, not for inference with the RP models.

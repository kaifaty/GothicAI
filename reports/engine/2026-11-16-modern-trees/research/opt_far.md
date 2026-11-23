# The far tree and the forest as a whole (research, 2026-11-18)

Scope: what replaces a tree beyond about 50–100 m, how the swap stays invisible, and how a forest of 2–5 thousand
trees is drawn and culled. This note builds on `tree_tech.md` (sections C, E, F, G) and `engine_audit.md` (section 2
and the capability table). It does not repeat them. Where it needs them it says "see tree_tech C" and so on.

Marks: **[primary]** means a developer, engine vendor or paper author is the source. **[secondary]** means press, a
forum or someone else's write-up. **[inference]** means my own reasoning or arithmetic. **[unconfirmed]** means I
could not check it against a primary source.

---

## Takeaway

- **For our island, the far tree should be a hemi-octahedral impostor per species and growth form.** Use 12×12
  frames, which is Epic's Fortnite value, at 128–170 px per frame, in a 2048 atlas.
  - Bake two packed layers that feed our G-buffer directly. The first is albedo + coverage. The second is
    normal + depth. Add a small third layer for AO/openness and thickness.
  - Light it in the same deferred path as the mesh, never pre-lit.
  - Draw it into the shadow view with frames chosen by the sun direction.
  - That is the only far representation in the sources that keeps the silhouette right against the sky, and on an
    island with ridges and cliffs, much of the far forest is seen against the sky. Merged canopy shells and
    terrain-colour layers lose that silhouette.
- **The swap becomes invisible in four steps, not by the impostor alone.**
  1. Put a cheap 3D level between the mesh and the impostor: tree_tech C's LOD3, about 0.4–1.2 k triangles.
  2. Bake the impostor from that level, not from LOD0. Amplify gives this advice in so many words.
  3. Cross-fade with hashed dithering anchored to the instance, with distance hysteresis.
  4. Correct impostor coverage in the mips, so the far crown keeps its colour and density.

  An open implementation measured the last step alone: a hard 0.5 cutoff left far crowns 30 levels of 255 darker. With
  coverage kept, the luminance error dropped from 5.44 to 1.50.
- **Instance count is not our problem.** Shipped games cull 100 k to over 1 M instances on the GPU in a fraction of a
  millisecond. Our 2–5 k trees are small change. The cost is per-pixel and per-triangle work on the trees that pass,
  and in shadows. So the order is:
  1. a screen-size cull and LOD choice in `visibility_pass.comp` for every view;
  2. the impostor;
  3. a terrain-only occluder pass, if measurement shows trees hidden behind ridges still cost.
- **Measure first** what the existing forest costs at the distances where impostors would apply. That means a view
  across a valley with only trees beyond 100 m, split by pass (G-buffer, VSM, RT). It decides whether the impostor
  pays off more than the intermediate LOD.

---

## 1. Impostors

### 1.1 What the primary sources say now

**Epic Impostor Baker documentation (UE 5.x, current page)** [primary]
([Epic docs](https://dev.epicgames.com/documentation/en-us/unreal-engine/impostor-baker-plugin-in-unreal-engine)):

- **Modes.** Three modes:
  - full sphere;
  - upper hemisphere, which captures only views from above the terrain;
  - traditional billboards: nine views in a 3×3 grid on eight cards, 72 triangles.
- **Fortnite.** Fortnite Battle Royale uses upper-hemisphere impostors for all trees when Nanite is off, with
  **FramesXY = 12 (144 frames)** and a **2048** final texture.
- **Geometry.** An impostor card is 8 triangles and 9 vertices. It blends the **three nearest frames**; mobile takes
  a single frame.
- **Supersampling.** The scene capture resolution should be at least 2× each subframe's final size. The example is
  a 4096 atlas at 16×16 frames, which gives 256 px per frame.
- **Baked maps.** Baked as G-buffer layers: BaseColor, Normal, optional Emissive/Subsurface/etc., plus packed scalar
  masks (opacity mask, depth, roughness…). If alpha is unused, DXT1 replaces DXT5.
- **Parallax modes.** None; single-sample parallax, which is like bump offset; iterative parallax with
  "depth-derived weights", which picks the best neighbour-frame ray per pixel.
- **Cost.** The impostor pixel shader costs more than a billboard because of the three-frame blend. Billboards have
  worse masked overdraw. Overall the impostor renders faster.
- **Capture pitfall.** Without G-buffer capture, the material must switch to capture outputs. Otherwise parallax
  reads colour or lighting as depth and gives "severe artefacts".

**Brucks, GDC 2018, "Realistic Foliage Imposter and Forest Rendering in UE4"** [primary, abstract only]
([Game Developer preview](https://www.gamedeveloper.com/art/make-like-a-tree-and-leave-time-at-gdc-2018-for-this-talk-on-ue4-foliage-rendering)):
- The talk covers a "hybrid combination of techniques" for forests and "a higher quality control version that uses a
  more expensive ray marching approach".
- **Pixel depth offset** is used to match shadowing and geometry intersections.
- Slides and video were not retrievable. The shaderbits article body was again empty this session.

**Amplify Impostors manual (Unity)** [primary]
([Amplify wiki](https://wiki.amplify.pt/index.php?title=Unity_Products:Amplify_Impostors/Manual)):
- **Layout.** A spherical lat-long layout pops noticeably between frames. Octahedral blends three neighbours and does
  not pop, but ghosts at close range. Hemi-octahedral has the same frame count with about 2× angular precision, and
  is wrong from below.
- **Standard bake.** Four packed maps: Albedo|Alpha, Specular|Smoothness, ObjectNormal|**Depth**, Emission|**AO**.
  Up to eight maps in a custom bake.
- **Padding.** Each frame's edge pixels are dilated, with 32 px as the example, so the mips do not bleed.
- **Sizes.** Use 2k maps, or 1k on mobile. Do not use 4k or 8k: reduce the view count instead.
- **LOD use.** The impostor is meant to replace the *last* LOD, not near meshes. For the best transition, **bake the
  impostor from the previous LOD**.
- **Cross-fade.** Unity cross-fade and dither cross-fade are supported.
- **Shadows.** Frame blending does not match binary shadows, and light depth differs from camera depth. There are
  separate "Shadow Bias" and "Shadow View" controls.
- **Other artefacts.** Neighbouring frames "peek through": the fix is a tighter card or "Clip Neighbours Frames".
  SSAO shows precision issues on impostor depth. No skinned animation.

**Meep engine octahedral impostors (open source, README)** [primary for that engine]
([README](https://unpkg.com/@woosh/meep-engine@2.124.2/src/engine/graphics/impostors/octahedral/README.md)):
- **Deferred design.** "An impostor is a set of pre-rendered G-Buffers", reconstructed per pixel and then shaded.
  Albedo (with vertex colours), normal, metalness, roughness and AO.
- **Card and depth.** A 6-triangle card with ray marching in the pixel shader. Depth is written, so impostors work in
  shadow maps.
- **Lineage.** It builds on Max & Ohsaki 1995 and Todt et al. 2007.
- **Artefacts.** Too close, the hidden interior is missing. At extreme projection, pixels smear.
- **Bake modes.** The discussion around it describes a full/hemi choice: hemi lacks the bottom views but gives about
  2× resolution elsewhere.

### 1.2 What to bake for our G-buffer

Our deferred G-buffer and crown shading need these inputs. Their sources are `engine_audit.md` sections 4 and 7 and
tree_tech B.

| Layer | Content | Why |
|---|---|---|
| A (BC7) | albedo RGB + coverage A | coverage, not a 0/1 mask, is needed for the mip fix in §3.3 |
| B (BC7 or BC5+BC4) | normal (object or view-frame space) + depth from the frame plane | lighting in the same deferred path; depth for parallax, shadow depth and contact with the ground |
| C (BC4/BC5, optional) | AO and **openness**, thickness | our crown shading uses per-vertex openness. If it is not baked, the impostor lights like a solid ball. Thickness feeds the sun-through-leaves term |

- **No pre-lit colour.** Epic, Amplify and Meep all bake G-buffer layers, not lit colour [primary]. The older
  SpeedTree billboard (diffuse × AO) and DynDOLOD per-type brightness controls are the failure cases (tree_tech G).
- **Openness.** Bake openness as a Blender Cycles AOV from the same vertex attribute the mesh uses.
  **[inference]**: no source covers openness, because it is our own term. Wildlands' baked fake shadowing and
  translucency channels are the closest precedent (tree_tech C).
- **Normals.** Bake *crown-scale* normals: the normal the mesh's openness shading actually produces, not the raw
  per-card normals. At 128 px per frame a card normal aliases into noise under TAA. **[inference]**, supported
  by the shimmering reports in tree_tech G.

### 1.3 Atlas size and memory per species **[inference, arithmetic]**

- **On-screen size.** At 2560×1440 with a 60° vertical FOV, an 18 m tree is about **224 px tall at 100 m, 112 px at
  200 m, 56 px at 400 m**. Formula: px = 18 / (2·d·tan 30°) · 1440. A frame needs about the on-screen size of the
  tree at the switch.
  - If the impostor starts at 150–200 m, a 128–170 px frame suffices.
  - 12×12 × 170 px ≈ 2048, which is Epic's Fortnite size.
- **Memory.** A 2048² BC7 layer is 4 MiB, 5.3 MiB with mips.
  - Two layers ≈ **10.7 MiB per species/form**.
  - Adding a BC4 AO/openness layer adds about 2.7 MiB.
  - Ten forms ≈ 110–130 MiB. That is affordable on a 10 GB 3080; on the Mac it is worth a 1536 or 1024 atlas.
- **Earlier switch.** If the impostor starts nearer, at 100 m and 224 px, the frames need 256 px: 4096 at 16×16, or a
  texture array. That is the size Amplify warns against. This is a reason to keep a cheap 3D level until about
  150–200 m rather than switching at 50–100 m.

### 1.4 Frame blending, parallax and the view from above or below

- **Blending.** Three-frame blending is standard. Epic, Amplify and Meep all do it, and mobile is the exception
  [primary].
- **Parallax.**
  - Single-sample parallax by baked depth is the cheap default.
  - Iterative parallax costs more and helps mainly when the camera comes close, which an impostor at 150 m+ does not
    need.
- **Hemi or full.** Hemi-octahedral is wrong from below [primary, Amplify]. On our island the camera is below a tree
  when looking up at a ridge from a valley. The elevation is usually small, but trees on cliff tops can be seen
  20–40° from below.
  - **[inference]**: with hemi, clamp the view to the horizon frame. The crown looks right and the trunk base is
    hidden by the cliff edge.
  - Only if captures show the clamped trees look wrong, bake a full octahedron for the ridge forms.

### 1.5 Lighting in a deferred renderer

- **Same shader path.** Write the impostor into the same G-buffer as the mesh: albedo, normal, material id
  "foliage", openness and thickness, plus per-pixel depth or pixel depth offset. Sun, VSM, GI and fog then treat it
  like the mesh. This is the convergent design in [primary] Epic (PDO, G-buffer capture), Meep (G-buffer frames) and
  arycama (per-pixel depth; tree_tech C).
- **Normal space.** Store normals in object space, as Amplify does, and rotate them by the instance's yaw.
  Frame-local normals blended across three frames give seams. **[inference]**
- **Matching check.** Open project data shows what a mismatch looks like and how to measure it [secondary, open
  project with numbers] ([bevy_flora PR #19](https://github.com/Ladvien/bevy_flora/pull/19)).
  - Impostors at about 200 m were darker and more saturated than level 0: 30/255 darker on an oak, colour error 68.
  - The cause was the 0.5 cut on mip coverage.
  - With alpha-to-coverage, beyond 200 m the luminance error went from 5.44 to 1.50 and the colour error from 13.21
    to 2.84.
  - The cost was unchanged: 7.30 vs 7.31 ms for 100 k oaks.

  The method is the point for us: render the mesh and the impostor at the switch distance and compare mean luminance
  and colour per tree.

### 1.6 Shadows from impostors

- Amplify: blended frames and binary shadows disagree. There are separate shadow-bias and shadow-view controls
  [primary].
- Meep and arycama: write depth, then use the impostor in shadow maps [primary/secondary].
- **For our VSM [inference].** In the shadow view, choose the frames by the *sun* direction, not the camera, and write
  depth from the baked depth with a small push away from the light. One sun direction per frame means the selection
  is stable, and the noon sun is high, which is where the upper hemisphere has frames.
- **Alternative.** Use LOD3, about 1 k triangles, as the shadow caster at every distance. That costs more triangles,
  but it is the Horizon-style fixed shadow LOD of tree_tech E. With VSM's 512 k meshlet–page cap (`engine_audit`
  table), the impostor caster is the safer choice for hundreds of far trees.
- **Ray tracing.** Impostors cannot enter the BVH as cards. For RT far shadow and GI, either keep an opaque LOD3 proxy
  in the TLAS or accept no far trees in the ray scene. tree_tech E covers the RT options. Note that the shells give
  the far forest darkness today (`engine_audit` §9).

### 1.7 Wind on impostors

- Nanite stops skinning below **0.1 of screen size** by default (`r.Skinning.DefaultAnimationMinScreenSize`), and VSM
  performance "requires you to disable animation in the distance". Wind can still reach far trees [primary]
  ([Nanite Foliage docs](https://dev.epicgames.com/documentation/en-us/unreal-engine/nanite-foliage)).
- **[inference]**: on the impostor, apply only trunk sway. Shear the card vertices by the same per-instance phase and
  the same gust front as the mesh. Without that, the switch shows as the crown "freezing".
  - Open project data: an impostor at 260 m with wind shimmered 0.024–0.030 per frame, against 0.224 for the full
    mesh [secondary, bevy_flora].
- **Shadows.** Keep the shadow-view impostor still. Its motion would put shadow pages back into a redraw every frame
  (`engine_audit` §6).

### 1.8 Known artefacts and fixes

| Artefact | Source | Fix |
|---|---|---|
| Pop between frames | Amplify (spherical), yummers.dev at about 8×8 | octahedral three-frame blend; ≥12×12 frames |
| Neighbour frames "peek through" | Amplify | tight card per frame; clip to frame |
| Ghosting at close range | Amplify | do not use closer than the bake was meant for |
| Wrong from below | Amplify, Epic hemi mode | horizon clamp; full octahedron for ridge forms |
| Colour/density mismatch at the swap | bevy_flora numbers; DynDOLOD (tree_tech G) | G-buffer bake; coverage-aware mips; bake from the previous LOD |
| Shadow mismatch | Amplify | sun-selected frames, separate bias; or LOD3 caster |
| Shimmer under TAA | tree_tech G | crown-scale normals; coverage mips; per-instance anchored dither |
| SSAO precision on impostor depth | Amplify | lower SSAO weight on the impostor material, or rely on baked AO |

### 1.9 Screen size at which shipped games switch

- **No primary source gives one.** Epic's impostor page and Fortnite material give frame counts and resolution, but
  no distance.
- Fortnite's mobile path drew many single-frame impostors in HLODs (tree_tech C).
- RDR2's about 500 m is a mod's claim (tree_tech F).
- Ghost Recon Wildlands switches sooner in dense areas (tree_tech C).
- **[inference]**: from the frame-size arithmetic in 1.3, a switch at about 110–150 px tree height (150–200 m for an
  18 m spruce) matches 128–170 px frames. Smaller trees switch nearer in metres at the same pixel size.

---

## 2. Alternatives for the far forest

**Billboard clouds** [primary] (Décoret, Durand, Sillion, Dorsey, "Billboard clouds for extreme model
simplification", ACM TOG 22(3), 2003,
[Yale page](https://graphics.cs.yale.edu/publications/billboard-clouds-extreme-model-simplification)).
- **Method.** A set of textured planes fitted greedily to the model. Hybrid billboard clouds add polycount and
  fill-rate terms (Bromberg-Martin et al., SIGGRAPH 2004 poster).
- **For trees.** Simplygon recommends them with the trunk kept as triangles (tree_tech C).
- **Pros.** Real parallax with no view-dependent atlas, correct from any elevation including below, and plain lit
  geometry in every pass, including the BVH as an opaque-ish proxy.
- **Cons.** Alpha overdraw across many planes. Our existing LOD3 card rule is already a hand-made billboard cloud.
- **[inference]**: for our trees, LOD3 *is* the billboard-cloud level. No separate tool is needed.

**Cross/X planes and the old SpeedTree billboards** (tree_tech C). They are cheap, but they light flatly and read as
crosses from above. On an island where the camera often looks down from cliffs onto the forest, that view is common.
Do not use them as the last level.

**Merged forest HLOD meshes**
- **Precedents.**
  - UE World Partition HLOD builds one proxy mesh and material per cell [primary]
    ([Epic HLOD docs](https://dev.epicgames.com/documentation/unreal-engine/world-partition---hierarchical-level-of-detail-in-unreal-engine)).
  - OpenMW object paging and KCD2 "HLod trees" are other examples. Of KCD2 players say "the transition from render
    trees to HLod trees is too obvious" [secondary, Nexus/Steam via search summary].
- **Pros.** Cell-sized draws.
- **Cons.**
  - The merged result is a remesh, and Simplygon advises against remeshing vegetation except for very distant HLOD
    (tree_tech C).
  - It is rebuilt whenever a tree moves.
  - Against the sky it reads as a lumpy blob.
- **[inference]**: with 2–5 k trees, draw calls are not our problem. The GPU-driven pass already instances. HLOD
  meshes buy us nothing that impostors do not.

**Canopy shell / terrain-colour forest layers**
- **Bruneton & Neyret** [primary, abstract via search]
  ([CGF 31(2), 2012](https://diglib.eg.org/handle/10.1111/v31i2pp373-382)):
  - Near trees are z-fields: per-tree precomputed views on an upper-hemisphere-to-square map, which is in effect a
    hemi-octahedral impostor.
  - Far trees become "shader-maps": a view- and light-dependent shader *on the terrain* that integrates the subpixel
    effect of many trees, including shadows between trees and the sky.
  - The two blend without popping.
  - This is the published reference for a "forest as terrain shading" level and for keeping colour constant across
    it.
- **Ghost of Yōtei** (Bentley, PlayStation Blog, 2025-10-23) [primary]
  ([tech deep dive](https://blog.playstation.com/?p=411201)): for distant mountains, they baked "models and detailed
  terrain materials into textures". On the GPU, more than 1 M trees, rocks and bushes are culled to about 60 k items
  rendered into the G-buffer.
- **Fit for us [inference].** A terrain-colour layer works for a forest seen from above at a kilometre: mountainsides
  across a big map. Our island is 1.5 km across, so the farthest forest is at most about 2 km away.
  - Seen from a valley, it sits on a ridge *against the sky*, where a terrain layer has no silhouette.
  - The one place where it pays is the darkening of the forest floor and slopes under canopy beyond the shadow range.
    That is the "baked top-down canopy occlusion" of tree_tech E, and the job the old shells do today
    (`engine_audit` §9).
  - Recommendation: a terrain tint/occlusion term, yes; terrain-only far trees, no.

**Ridge "tree line" tricks.** No primary source found. **[inference]**: the cheap version is to never let the far LOD
of a ridge-line tree go below the impostor. Do not distance-cull trees whose top is near the skyline from common
viewpoints; mark them in placement.

**Voxels and points**
- **UE 5.7 Nanite Foliage** [primary]
  ([docs](https://dev.epicgames.com/documentation/en-us/unreal-engine/nanite-foliage)):
  - Voxel clusters of up to 128 bricks of 4×4×4 voxels, "near pixel-sized". The switch happens when it gives lower
    error than triangles.
  - Each voxel has a normal *distribution*, sampled stochastically per pixel, with its statistics stored in vertex
    colour alpha.
  - Voxel clusters are binned and rasterised front to back in depth buckets for early-Z.
  - Assemblies: up to 65 k part instances. The example tree has 41 M triangles, 12 parts, 2,160 part instances and
    850 bones.
  - Demo world: 500 k instances.
  - Experimental status: some settings "don't actually work as intended yet". Forum users report inconsistent voxel
    switch distances and bad RT shadows from voxelised foliage [secondary].
- **NVIDIA RTX Mega Geometry foliage with CD Projekt RED** (GDC 2026)
  ([NVIDIA blog, 2026-03-10](https://developer.nvidia.com/blog/nvidia-rtx-innovations-are-powering-the-next-era-of-game-development)):
  - Partitioned TLAS, path traced, "millions" of animated foliage elements [primary].
  - Press reports a voxel LOD, about 1 M trees and 60 M plants on 5×5 km, and 58 fps at 1440p DLSS Quality on an RTX
    4070 [secondary; not in NVIDIA's post].
- **Fit for us.** Not for us now. Both need a virtual-geometry or cluster-LOD pipeline we do not have. They confirm
  that the far crown is best treated as a stochastic volume. A cheap analogue is the per-pixel stochastic normal from
  a baked distribution: the crown-scale normal + dither of 1.2.

---

## 3. Transitions

### 3.1 Dithered cross-fade, not alpha blend

- **Godot visibility ranges** [primary]
  ([docs](https://docs.godotengine.org/en/stable/tutorials/3d/visibility_ranges.html)):
  - With fade off, begin/end margins act as **hysteresis**.
  - Fade "Self"/"Dependencies" uses alpha blending and forces transparent rendering, which costs.
  - Dithering via per-material "Object Dither" is faster and has no sorting glitches. Its pattern is noisy, but "may
    not be as noticeable" with TAA.
- **Unity LOD Group** [primary]
  ([docs](https://docs.unity.com/en-us/engine/current/manual/analysis/graphics-performance-profiling/lod/lod-group/lod-transitions-lod-group)):
  - The cross-fade factor comes from screen size, either over a fraction of the LOD's range (example: 40 %) or over
    a fixed time (`crossFadeAnimationDuration`).
  - A timed fade cuts the number of objects mid-fade, but "transitions are more noticeable when the camera and object
    are still".
  - SpeedTree assets morph vertices between 3D levels and fall back to cross-fade for the billboard.
- **[inference] for us.**
  - Fade by screen size over a band of about 10–15 % of the switch size. Both levels draw only inside the band.
  - Use our existing hashed alpha threshold (`main.frag:947`) with the fade value, and the noise anchored to the
    instance and object space (Wyman 2017, tree_tech G).
  - Use *complementary* dither patterns for the two levels: pattern p for A and 1−p for B. The pixel is then covered
    by exactly one of them, and the mid-fade transparency of tree_tech G is gone.
  - A fixed-time fade is worse for us: the camera often stands still in dialogue, and that is where a timed swap
    shows.

### 3.2 Hysteresis and per-instance state

- The strand LOD already has one-step hysteresis (`engine_audit` §2).
- For trees on the GPU **[inference]**: keep one byte of state per instance (current level, fade direction) in a
  buffer the culling shader updates. Otherwise, compute the level from distance with an asymmetric band: switch down
  at s, up at 1.15 s.
  - The stateless form is enough for our sizes and keeps all views consistent.
  - The shadow view should use a *fixed* level per VSM clipmap level, the Horizon rule (tree_tech E), not the camera's
    level.

### 3.3 Keeping density and colour constant across the swap

1. **Coverage in the mips.** Use Castaño or Yuksel alpha distribution for the card atlas (tree_tech G). Apply the
   same to the **impostor atlas**: its far mips hold "leaf-and-sky mix as fractional alpha", and a hard cut darkens
   and thins the tree. See the bevy_flora numbers in 1.5.
   - Cem Yuksel, "Alpha Distribution for Alpha Testing", I3D 2018
     ([PDF](https://cemyuksel.com/research/alphadistribution/alpha_distribution.pdf)) [primary, not opened this
     session].
2. **Area restoration in 3D levels.** Restore area at 60–80 %, never 100 % (TreeLODGen, tree_tech C).
3. **Bake the impostor from the previous level** [primary, Amplify]. The 3D→impostor swap then compares like with
   like. The LOD0→LOD3 differences are hidden by the slower 3D fades nearer the camera.
4. **Shading inputs.** Use the same openness, thickness and AO inputs in both levels (1.2), and the same material id,
   so sun-through-leaves and fog treat them alike.
5. **Measured acceptance [inference].** Render each species at the switch distance from 8 azimuths and 3 elevations,
   mesh vs impostor, and compare:
   - mean luminance and colour (target in the bevy_flora range: luminance error below about 2/255);
   - coverage ratio (target ±5 %);
   - silhouette IoU.

   Then check a clip of the camera walking through the switch band, per the project's rule that motion is accepted
   by clip.

### 3.4 Scale-to-zero and small instances

- **Practice.** SpeedTree shrinks leaf instances to zero between levels (tree_tech C). Nanite stops animation below a
  screen fraction (1.7).
- **Small instances [inference].** For shrubs and saplings under about 4–8 px on screen, scale the instance to zero
  across the last band instead of fading, then cull it. It gives no dither noise and no density jump, because the
  object is already sub-pixel.
- **Large trees.** Never scale 18 m trees to zero inside the island's view distance. At 2 km a spruce is still about
  11 px tall at 1440p.

---

## 4. Instancing and culling at forest scale

**Numbers from shipped games**

| Game / system | Number | Source |
|---|---|---|
| Horizon Zero Dawn placement | 500+ asset types, 100,000+ objects in scene, about 250 µs average GPU busy load for placement | Van Muijden, GDC 2017 ([Guerrilla](https://www.guerrilla-games.com/read/gpu-based-procedural-placement-in-horizon-zero-dawn)) [primary, via search summary of the slides] |
| Ghost of Yōtei | more than 1 M trees/rocks/bushes on distant mountains culled to about 60 k items; GPU compute occlusion culling and draw-record fill, CPU reads back | Bentley, 2025 ([PS Blog](https://blog.playstation.com/?p=411201)) [primary] |
| Assassin's Creed Unity | about 10× more instances than earlier AC; 20–40 % of triangles and 30–80 % of shadow triangles culled per cluster; geometry-pass gain only about 10 % | Haar & Aaltonen, SIGGRAPH 2015 ([slides](https://advances.realtimerendering.com/s2015/aaltonenhaar_siggraph2015_combined_final_footer_220dpi.pdf)) [secondary summaries; PDF not opened] |
| UE Nanite Foliage demo | 500 k tree instances; 100 k bones ≈ 0.1 ms | Epic docs [primary] |
| Witcher 4 / NVIDIA foliage demo | about 1 M trees, 60 M plants, 5×5 km | press [secondary] |

**What this means for us [inference]**
- **Culling cost.** Per-instance GPU culling of 5 k trees is a few microseconds. No cell hierarchy is needed for
  culling cost. A 64 m chunk grouping, which the vob streaming already has (`engine_audit` §10), is enough for
  streaming.
- **The missing pieces** are the size test and LOD selection in `visibility_pass.comp` (`engine_audit` §2, option 2),
  for every view including VSM.
- **Sub-tree clusters.** AC Unity's lesson is that cluster culling saves triangles but gives a small gain in the
  geometry pass. A tree is one cluster today (`engine_audit` table). Splitting crowns into a few clusters helps
  frustum and HiZ only for near trees. Far trees are below one cluster's worth of screen anyway, so LOD matters more.

**HiZ and alpha-tested crowns [inference; no primary source found]**
- **Occludees.** HiZ tests an instance's bounds against a max-depth pyramid. Crowns as *occludees* behave like any
  box.
- **Occluders.** As *occluders* they are weak: a porous crown leaves sky or far depth in most coarse HiZ texels, so
  max reduction gives the far value. Only a dense stand fills a tile.
- **What really hides trees here** is the terrain, ridges and cliffs, and the building walls. Our depth prepass
  already draws occluders only (`engine_audit` table), so the terrain is in the HiZ.
- **Forum reports.** UE foliage instances and HZB occlusion are anecdotal and old (UE 4.15)
  ([UE forum](https://forums.unrealengine.com/t/occlusion-culling-with-foliage-trees/76974)) [secondary].
- **Measure first** how many trees beyond 100 m pass HiZ while behind a ridge. On an island of valleys this is likely
  the largest cheap win.
- **Impostors as occluders.** Impostors writing depth can occlude the next frame, but a two-pass HiZ only feeds the
  next frame, so mistakes are bounded.

**Streaming.** With 2–5 k trees and about 10–15 forms, everything fits resident. Impostor atlases come to about
130 MiB for all forms (1.3). Per-ring detail (`engine_audit` §10) is replaced by the per-view LOD choice, so no tree
streaming beyond the existing vob chunks is needed.

---

## 5. Baking offline in Blender (no engine editor)

**Tool status** [secondary, search and listings]
- **No current, licence-clear Blender octahedral baker was found.**
  - Pandrodor/Blender-Impostor-Baker was last updated in 2022.
  - "Instant Impostors" (paid) is EEVEE-only and "cannot export impostors" to engines. Its 2024 volumetric mode is
    "far more intensive than what is usable for any game engine"
    ([80.lv, 2024-08-21](https://80.lv/articles/high-fidelity-volumetric-3d-impostors-for-blender)).
- **References for layout:** the Godot bakers (Base, Normal, ORM textures; 144 views in one Godot 4 tool) and the
  open three.js and Meep code (tree_tech C).
- **Conclusion:** write our own. It is a small script on top of `scripts/content/mesh3d/blender/` (`engine_audit`
  §12).

**Bake recipe [inference, from the primary layouts above]**
1. **Input** is LOD3 or LOD2 of the tree (see 3.3 point 3), at rest pose, wind off, pivot at the trunk base. The
   bounding sphere is computed from the crown.
2. **Directions.** For each frame (12×12, hemi-octahedral), the view direction comes from the octahedral decode of
   the frame centre. The camera is orthographic, sized to the bounding sphere and pointed at its centre.
3. **Passes.** One Cycles render per frame at 2× the frame size. Emission-only material overrides or AOVs give:
   - base colour;
   - object-space normal (with crown-scale smoothing, see 1.2);
   - depth from the frame plane, normalised to the sphere's diameter;
   - coverage (alpha);
   - AO (Cycles AO pass);
   - openness and thickness (shader AOV from the vertex attribute).

   No lighting enters any pass.
4. **Post.**
   - Downsample 2× with coverage-weighted colour.
   - Dilate colour, normal and depth into empty pixels by at least 8–32 px per frame, as Amplify does.
   - Pack the layers.
   - Build mips with coverage preserved; `alpha_mips.py` and `glb_cards.py` already have the algorithm
     (`engine_audit` table).
   - Write BC7/BC4.
5. **Card.** Per species, a tight convex card (6–8 triangles) or a per-frame clipped card.
6. **Checks before the engine.**
   - Re-project the impostor in a Blender viewer and diff it against a lit Cycles render of the mesh from 24
     directions (8 azimuths × 3 elevations): luminance, colour, coverage, silhouette IoU.
   - Check for frame bleeding at mip 3–4.
   - Check depth range clipping on wide crowns.
   - Look at a view straight down: the cliffs case.
7. **Cost.** 144 frames × 4–6 passes of a 1 k-triangle tree at 340 px is seconds to minutes per form in Cycles on
   the Mac. That is light work and does not conflict with the no-models-on-Mac rule.

---

## 6. Recommendation for an island of about 1.5 km with 2–5 k trees and several species

**Best look per unit of work, in order [inference, built on the sources above]:**

1. **Measure first (no new art).**
   - **Far-only cost.** A view across a valley with the new spruce placed only beyond 100 m, then only beyond 200 m.
     Split the cost by G-buffer, VSM and RT pass at 1440p on the 3080.
   - **HiZ pass rate.** The count of trees that pass HiZ while behind a ridge.

   These two numbers decide steps 3 and 5.
2. **GPU LOD selection and size cull** in `visibility_pass.comp`.
   - This covers every view, with a fixed level per VSM clipmap.
   - Add hysteresis and complementary hashed dither between levels.
   - Prerequisite for everything else. The engine has the strand precedent.
3. **The 3D chain down to LOD3** (tree_tech C).
   - LOD3 is the shadow caster and the last 3D level up to about 150–200 m, which is about 110–150 px tall.
   - The 1/8-triangle result (−75 % cost) suggests this step alone removes most of today's +14..27 ms.
4. **Hemi-octahedral impostor per form, baked from LOD3**: 12×12 frames, 2048 atlas, albedo+coverage, normal+depth,
   AO+openness, coverage-aware mips.
   - Draw it into the G-buffer with depth.
   - Draw it into VSM with sun-selected frames.
   - Give it trunk sway only.
   - Accept it by the numeric comparison in 3.3 point 5 and a walk-through clip.
5. **Terrain occluders.** Only if step 1 shows trees behind ridges costing more than about 1 ms: make sure terrain and
   cliffs are in the HiZ for the main and shadow views.
6. **Canopy occlusion/tint term on the terrain** for the forest floor beyond the shadow range. This replaces what the
   shells do today (`engine_audit` §9). Bruneton & Neyret are the reference for doing it consistently.
7. **Do not do now:** HLOD merged meshes, voxel/Nanite-style foliage, terrain-only far forests, cross-plane
   billboards.

**What to measure on each step:**
- milliseconds per pass at fixed views (`perf-content.sh` ABBA);
- tree triangles per view;
- the luminance and colour error at the swap distance;
- a TAA shimmer metric: frame-to-frame difference on a still camera at 200 m, which is the bevy_flora method.

---

## Gaps / not confirmed

- **Brucks first-hand material.**
  - The shaderbits 2018 article body and the GDC 2018 slides were not retrievable. His ray-march "high quality"
    variant and the hybrid scheme are known only from the abstract.
  - Fortnite's 12×12 / 2048 is confirmed by Epic's docs; switch distances are not.
- **Screen-size or distance switch points** for impostors in any shipped game: none found in a primary source. RDR2's
  about 500 m remains a mod claim.
- **Instance counts.** The Horizon "100,000+ objects / about 250 µs" comes from a search summary of the official
  slides. The PDF was too large to fetch. AC Unity's culling percentages are from secondary summaries of the SIGGRAPH
  2015 slides.
- **Bruneton & Neyret 2012.** The full text and performance numbers were not retrieved; the abstract came via search.
- **Yuksel 2018** alpha distribution: cited, not opened this session.
- **Witcher 4 / NVIDIA foliage.** The voxel LOD and the counts (1 M trees, 60 M plants, fps) are press reports. NVIDIA's
  own post says only "millions".
- **UE 5.7 Nanite voxels.** No primary description of opacity, VSM or RT handling. Forum reports of RT-shadow and
  switch-distance issues are anecdotal.
- **HiZ with porous crowns as occluders**: no primary source. Section 4's reasoning is inference.
- **Ridge tree-line tricks** and **scale-to-zero for small instances**: no primary source. Section 3.4 and section 2's
  ridge advice are inference.
- **The bevy_flora numbers** come from an open hobby project on Apple M4 Pro. Its method is useful; its magnitudes are
  not a shipped-game benchmark.
- **Licences of open bakers** are still unchecked against LICENSE files (tree_tech C). This note recommends our own
  Blender script, so it matters only for reading their code.
- **Ghost of Yōtei** says distant mountains bake "models … into textures". It does not say whether trees are among
  those models or how the near/far swap is hidden.

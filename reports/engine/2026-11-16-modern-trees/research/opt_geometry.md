# Cost of the near and mid tree in the main pass: overdraw, LOD, clusters, density (research, 2026-11-18)

Scope: why a forest of our spruce (26,759 triangles, alpha-tested two-sided shoot cards, vertex-colour wind, no LOD) costs
+14..27 ms at 1,024 trees on an RTX 3080 at 1440p, and what shipped games and engines do about it. This note builds on
`tree_tech.md` (sections A, C, F, G) and `engine_audit.md` (sections 1-3) in this folder and does not repeat them: the
SpeedTree "fewer, bigger leaves" LOD, Preserve Area, TreeLODGen percentages, impostor parameters, Horizon's frame shares,
hashed-alpha cost and the Apple discard rule are all there already.

Marks: **[primary]** = engine docs, talk or the developer's own post; **[secondary]** = press, forum or summary;
**[inference]** = my reasoning from our measurements and the engine audit, not a source.

---

## Takeaway

1. **Our cost is mostly geometry, and most of that geometry does not need to be drawn.** Our own scaling test says so:
   1/8 of the triangles removes about 75 % of the added cost. The fall is steeper than linear: 1/2 the triangles removes
   35 %, 1/8 removes 75 %. A cost that grows faster than the triangle count is what small triangles cause (2x2-quad
   waste), plus fixed costs per meshlet and per page. **[inference]** The engine adds to this. A tree is one cluster
   with all its meshlets (`engine_audit.md` §1). So a tree that is 5 % on screen, or hidden behind another tree, is
   drawn whole. The same tree is drawn whole again for **every** VSM page it touches (§6). The two-sided cards are
   stored twice. The HiZ occluder pass runs alpha-tested cards with discard.
2. **Shipped engines do not let alpha-tested cards reach the shader pass unculled and unsorted.** They use one of
   three answers:
   - a depth prepass with alpha test, then the colour pass with EQUAL depth and no discard. This is the classic route,
     measured by Pettineo in 2025;
   - a visibility buffer or deferred texturing, so each pixel is shaded once. Horizon Forbidden West did this for
     foliage in 2022;
   - opaque modelled leaves inside a virtual-geometry system (Fortnite 2023, Nanite Foliage 2025).

   The third route needs Nanite-class infrastructure: a software rasteriser and voxel far crowns. For a small team it
   is out of reach.
3. **For us the order is fixed by our own numbers:**
   1. cull and choose LOD per meshlet, and per VSM page;
   2. build a discrete LOD chain chosen on the GPU, with a dither cross-fade and a fixed coarser LOD for shadows;
   3. drop the duplicated twin cards (real two-sided raster);
   4. take alpha-tested trees out of the HiZ occluder pass, or turn that pass into a proper foliage prepass;
   5. then do fill work: tight cut-outs and thinning of the inner crown.

   Steps 1-3 are almost free visually. Their expected effect is most of the 75 % that a 1/8 triangle budget showed.
   Steps 4-5 are where the look can be harmed.

---

## 1. Overdraw and alpha test: where the cost comes from

### Findings

**2x2 quads and small triangles.**
- GPUs shade in 2x2 pixel quads. A triangle that covers one pixel still pays for four. Fatahalian et al. showed that
  for pixel-sized triangles, merging quad fragments would cut total shading work by **8x**. That is a measure of how
  much the normal pipeline wastes at that size
  ([Fatahalian et al., "Reducing shading on GPUs using quad-fragment merging", SIGGRAPH 2010](https://www.microsoft.com/en-us/research/?p=159994), abstract) **[primary]**.
- Nanite's answer is a compute-shader rasteriser for clusters of small triangles, with hardware raster for clusters of
  larger ones. The often-quoted gain is "~3x faster than hardware" for mostly pixel-sized triangles. I found that figure
  only in later papers and summaries citing the Karis 2021 talk, not in the deck itself
  ([Karis, Stubbe, Wihlidal, SIGGRAPH 2021 Advances](https://advances.realtimerendering.com/s2021/Karis_Nanite_SIGGRAPH_Advances_2021_final.pdf);
  [summary](https://vrarwiki.com/wiki/Nanite)) **[secondary for the 3x]**.
- Practitioner rule: small, thin and long triangles waste quad work far more than the raw triangle count does. Spend
  polygons on the silhouette, keep triangles large and even on screen, and judge them from the game camera
  ([Świerad, "Unreal Art Optimization", 2019](https://unrealartoptimization.github.io/book/pipelines/pixel)) **[secondary, practitioner]**.

**Masked pixels and early-Z.**
- Epic: "Masked materials are fairly expensive compared to Opaque ones. Masked-out pixels cost nearly as much as drawn
  pixels." It also says: "Geometry foliage has shown to be faster with Nanite than card approaches, both Nanite cards and
  non-Nanite cards" ([Epic, Working with Nanite-enabled content, read 2026](https://dev.epicgames.com/documentation/en-us/unreal-engine/working-with-naniteenabled-content)) **[primary]**.
- Pettineo measured this on an RX 7900 XT in 2025. A discard does not force late-Z by itself: the depth test can still
  run early. But the depth **write** waits until the shader has run, so later fragments in flight cannot be culled by
  it. Pixel-shader invocations in his scene:
  - opaque back-to-front: 648,000;
  - opaque front-to-back: 440,640;
  - front-to-back with a discard that never fires: about 582,000, that is **+32 %** just from having discard in the
    shader. A uniform branch around the discard does not help.

  His recommendation: draw non-alpha-tested opaques first, or do a depth prepass with discard and then the full pass
  with an **EQUAL** depth test
  ([Pettineo, "To Early-Z, or Not To Early-Z", 2025-04-13](https://therealmjp.github.io/posts/to-earlyz-or-not-to-earlyz)) **[primary; AMD only, no NVIDIA numbers]**.
- Unreal ships the same split as an option. With `r.EarlyZPass=2` and `r.EarlyZPassOnlyMaterialMasking=1` the mask is
  evaluated only in the prepass. The base pass then runs with depth-equal and without clip
  ([UE cvar wiki](https://indxzero.github.io/ue544cvarwiki/articles/r.earlyzpassonlymaterialmasking);
  [forum bug report showing the Equal path](https://forums.unrealengine.com/t/r-earlyzpassonlymaterialmasking-1-bcastdynamicshadowasmasked/2742765)) **[secondary]**.
  A related cvar says that writing depth while doing clip() disables early-Z in the base pass
  ([r.BasePassWriteDepthEvenWithFullPrepass](https://indxzero.github.io/ue544cvarwiki/articles/r.basepasswritedepthevenwithfullprepass)) **[secondary]**.

**The visibility-buffer route.**
- Horizon Forbidden West draws a visibility buffer as a prepass. Analysis and shading then run entirely in compute,
  "to accelerate its foliage and alpha tested geometry". The work overlaps G-buffer and cascade rendering on the
  async compute queue, "typically" with very little overhead. Compute also allowed a software variable-rate shading
  ([Guerrilla, McLaren, GDC 2022](https://www.guerrilla-games.com/read/adventures-with-deferred-texturing-in-horizon-forbidden-west);
  [GDC Vault](https://gdcvault.com/play/1028035/Adventures-with-Deferred-Texturing-in)) **[primary; no numbers on the page]**.
- Nixxes said deferred texturing was the one feature that did not port well to PC and replaced it with a traditional
  G-buffer ([interview summary](https://gaminglatest.com/threads/horizon-forbidden-west-pc-tech-interview-with-nixxes-and-guerrilla-games.118617)) **[secondary]**.
  For us this means a visibility buffer is a large rewrite with a known PC risk. It is not the first lever.

**Tight cut-outs vs more triangles.**
- Humus's particle-trimming tool (2009) fits an optimal enclosing polygon to the alpha. Remaining fill as a share of the
  original quad:

  | Shape | Fill remaining |
  |---|---|
  | aligned rectangle | 69 % |
  | 4 vertices | 60 % |
  | 6 vertices | 54 % |
  | 8 vertices | **52 %** |

  The gain flattens after about 6 vertices
  ([Persson/Humus, 2009-05-21](https://www.humus.name/index.php?ID=266)) **[primary, one texture]**.
- SpeedTree's Cutout Editor removes fully transparent triangles automatically and lets you paint triangles away
  ([SpeedTree Modeler manual](https://docs.unity.cn/speedtree-modeler/manual/cutout-editor.html)) **[primary]**.
- Vendor guidance for particles is the same: the mesh should follow the opaque/transparent edge
  ([Imagination PowerVR guide](https://docs.imgtec.com/performance-guides/graphics-recommendations/html/topics/optimal-particle-rendering-on-powervr.html)) **[primary]**.

**Alpha-to-coverage, hashed alpha.**
- Alpha-to-coverage needs MSAA samples. Our G-buffer has none (`engine_audit.md` §3), so it does not apply.
- Hashed alpha is already in our engine. What matters here: if a prepass is added, the colour pass must use the
  **same** hashed threshold per pixel and frame, or EQUAL fails on the edges. Our hash is deterministic (pixel, frame,
  texel cell), so this is possible. **[inference]**

**Where geometric needles beat cards.**
- Epic's answer applies only under Nanite: opaque modelled leaves at 300-500 k vertices per tree beat masked cards.
  This rests on a software rasteriser that keeps pixel-sized triangles cheap (`tree_tech.md` A).
- With hardware raster the break-even is set by triangle size. Once a needle triangle is below a few pixels, the quad
  waste (up to 4x per pixel and up to 8x across the pipeline, per Fatahalian) is worse than a masked card's wasted
  pixels. I found no published break-even number. **[inference]**
- For us this means modelled needles only at LOD0 and only close. Cards from LOD1 on.

### Inference for our engine

- Our G-buffer pass (+10..12 ms) does three costly things:
  - it shades with discard in the shader, which stalls depth writes;
  - it draws in random order (`engine_audit.md` §3: "front to back by chance");
  - it draws every meshlet of every tree whose sphere passes the test.

  Even 64 trees (1.7 M triangles) at +4..11 ms is far above what 1.7 M triangles cost an RTX 3080 when they are large
  and opaque. That points at quad waste and discard, not at vertex count.
- **A prepass only pays after LOD.** At 27 M triangles a prepass doubles the vertex and raster work. Once LOD has cut
  the triangles, a depth-only alpha-tested prepass plus an EQUAL colour pass without discard is the textbook fix. It is
  measured (+32 % shader invocations from discard alone) and shipped (Horizon Zero Dawn spent about 10 % of its frame
  on the prepass, `tree_tech.md` F).

---

## 2. Discrete LOD chains as shipped

### Findings (new beyond `tree_tech.md` C)

**Per-level numbers that are public.**
- Ubisoft vegetation artist Rudy Lamotte (2018):
  - the common tree is under about 10 k triangles; the example LOD0 is 9,995 triangles;
  - each next LOD is about half (about 5 k at LOD1);
  - at the last LOD, branches and leaves are enlarged so the leaf cloud keeps its volume;
  - the SpeedTree leaf "Keep" value of 0.0 strips all leaves, so raise it until the crown's volume is covered;
  - shadows were switched off on the LOD1/LOD2 materials for FPS;
  - prominent trees are kept symmetrical so their billboards repeat less visibly

  ([80.lv interview, 2018-06-21](https://80.lv/articles/creating-large-open-levels-with-vegetation)) **[secondary, practitioner; UE4 personal project, not a shipped game]**.
- CryEngine (Kingdom Come 1/2 run on it): merged-mesh vegetation is batched into **16 m cells** that "internally LOD
  themselves, removing items as the camera moves away" — density thinning by cell
  ([CRYENGINE 5 docs, Vegetation 02 Grass (Merged Meshes)](https://cryengine.com/docs/static/engines/cryengine-5/categories/23756816/pages/24285889)) **[primary, but for grass/merged meshes]**.
  Distance and LOD ratios are cvars (`e_MergedMeshesLodRatio`, `e_MergedMeshesViewDistRatio`)
  ([AWS re:Post, Lumberyard](https://www.repost.aws/questions/QUndEqT6iPSvSLRQOw38r9Ow/1-6-problem-with-vegetation-applied-via-terrain-layers)) **[secondary]**.
  I found no Warhorse source on KCD tree LODs.
- Ghost of Yōtei (Sucker Punch, 2025): distant mountains hold over one million trees, rocks and bushes, culled on the
  GPU to "about sixty thousand individual items" for the G-buffer. Distant mountains use baked models with materials
  baked into textures ([PlayStation Blog, Ghost of Yōtei tech deep dive](https://blog.playstation.com/?p=411201)) **[primary, no per-tree numbers]**.
  Ghost of Tsushima has no public tree-LOD talk; its GDC talk covers grass only
  ([GDC Vault](https://gdcvault.com/play/1027214/contactUs)).
- Assassin's Creed Shadows ships Ubisoft's own virtual geometry ("Micropolygon", GDC 2025). Its architect says
  alpha-tested geometry such as vegetation stays expensive in ray tracing and virtual geometry is not yet in the
  acceleration structures. Coverage implies vegetation is **not** on the micropolygon path
  ([80.lv, 2025](https://80.lv/articles/ubisoft-shows-off-micropolygon-rendering-in-anvil-engine);
  [Ubisoft news, 2026-04-14](https://news.ubisoft.com/en-us/article/6u0kflsdY20YlnZks7rUeG/inside-anvil-the-technology-powering-assassins-creed-shadows)) **[primary for the RT statement, secondary for "vegetation not on it"]**.
  Valhalla used SpeedTree 6 ([SpeedTree interview](https://store.speedtree.com/?p=831591)) **[primary]**; its GDC 2021
  talk is not public.
- STALKER 2 launched on UE 5.1 with non-Nanite vegetation that popped at distance; Nanite meshes were reportedly still
  alpha-tested. Patch 2.0 moved it to UE 5.5.4
  ([PCGH](https://www.pcgameshardware.de/Stalker-2-Spiel-34596/Specials/Release-Test-Review-Steam-Benchmarks-Day-0-Patch-1459736/4/), [TweakTown](https://www.tweaktown.com/rss/click/news/113170/?c=news-mf)) **[secondary]**.
  It is a cautionary case, not a recipe.
- Alan Wake 2 (Northlight): GPU-driven mesh-shader pipeline with meshlet-level occlusion culling "down to a single-pixel
  precision, using everything in a scene as an occluder". The vegetation is animated by a GPU skinning system with
  art-driven bones, not vertex offsets
  ([Remedy, "How Northlight makes Alan Wake 2 shine"](https://remedygames.com/article/how-northlight-makes-alan-wake-2-shine);
  [Jansson, Digital Dragons 2024](https://alanwake.info/2024/05/18th-may-2024-digital-dragons-2024-erik.html);
  [Kandar, GDC 2024, GPU skinning for vegetation](https://gdcvault.com/play/1034310/Large-Scale-GPU-Based-Skinning)) **[primary for the claims; the slides were not readable this session]**.
  This is the closest shipped match to our architecture: meshlets, mesh shaders, a forest.
- Avatar: Frontiers of Pandora (Snowdrop): the public material covers ray tracing (custom BVH) and procedural scatter,
  not foliage LOD ([GDC 2024, Koshlo](https://gdcvault.com/play/1034404/Ray-Tracing-in-Snowdrop-Scene)) **[gap]**.
- RDR2, Witcher 3, Far Cry 4/5: no per-level tree numbers found this session. Far Cry 4's GDC 2015 talk covers
  vegetation rendering, but I could not read its slides ([GDC Vault](https://www.gdcvault.com/play/1022235/Rendering-the-World-of-Far)) **[gap]**.

**Transitions.**
- Unity's LODGroup offers time-based cross-fade ("Animate Cross-fading") or a fade band given as a fraction of the LOD
  range. Its "SpeedTree" mode passes a blend factor (`unity_LODFade.x`) for vertex lerp between two mesh LODs. The last
  level just fades out. Cross-fade is done by screen-space dither or by transparency
  ([Unity manual, LOD Group](https://docs.unity3d.com/ru/current/Manual/class-LODGroup.html)) **[primary]**.
- Godot 4 visibility ranges: begin and end distances with margins (hysteresis). Fade "Self" or "Dependencies" uses
  alpha blending, which forces transparency and can break sorting. Object dither via Distance Fade is the faster
  alternative, "noisy" and best with two LODs. Distance is measured to the AABB centre. Godot's own advice for far LODs
  is to drop normal maps and SSS to cut pixel cost
  ([Godot docs, Visibility ranges](https://docs.godotengine.org/en/stable/tutorials/3d/visibility_ranges.html)) **[primary]**.
- Unreal "Dithered LOD Transition": during the band **both** LODs are drawn. Users report the foliage system can show
  up to four LODs at once and that the overdraw cost was visible on grass
  ([UE forum](https://forums.unrealengine.com/t/dithered-lod-transition-on-grass-drops-frame-rate/404938/3);
  [UE forum](https://forums.unrealengine.com/t/foliage-lod-draw-calls-problem-foliage-system-doesnt-kick-lods/330644)) **[secondary]**.
  Keep fade bands short.
- SpeedTree runtime: one float LOD state from 1 to −1. From 1 to 0 the 3D detail scales away (vertex lerp, leaves
  shrink and grow); from 0 to −1 the billboard cross-fades in
  ([Unity SpeedTree Runtime SDK, Level of detail](https://docs.unity3d.com/speedtree-runtime-sdk/manual/level-of-detail.html)) **[primary]**.
  This is a geomorph inside the 3D range and needs no dither.

**Per-pass LOD.**
- Horizon used a fixed LOD per shadow cascade (`tree_tech.md` G).
- Unreal has no clean per-LOD shadow proxy for foliage. Users ask for "let LOD N cast for all LODs", and billboard
  LODs often stop casting
  ([UE forum](https://forums.unrealengine.com/t/billboards-not-casting-shadows/2437159);
  [shadow imposter plugin thread](https://forums.unrealengine.com/t/free-ue5-1-plugin-for-nanite-foliage-add-foliage-shadow-imposters/680365/10)) **[secondary]**.
- Epic's VSM docs:
  - "Non-Nanite geometry is much more expensive to render into VSMs than Nanite geometry";
  - non-Nanite meshes need LODs, or they "become extremely expensive to render into small pages";
  - `r.Shadow.RadiusThreshold` culls small casters;
  - Contact Shadows can stand in for foliage shadows at range;
  - WPO "always invalidates cached pages every frame"; the fixes are `WPO Disable Distance` and "Rigid" cache
    invalidation behaviour

  ([Epic, Virtual Shadow Maps](https://dev.epicgames.com/documentation/en-us/unreal-engine/virtual-shadow-maps-in-unreal-engine)) **[primary]**.

### Inference for our engine

- Put the chain in `visibility_pass.comp` (option 2 in `engine_audit.md` §2). The projected size is already computed
  there, and the shadow views can be given a **coarser fixed level**: for example, at most LOD1 for the first VSM
  clipmap levels and LOD2-3 beyond. Do the same for the HiZ occluder pass.
- Use dither cross-fade in the main view only, through the existing hashed-alpha threshold. Hard switches with
  hysteresis in shadow passes are acceptable: the shadow switch happens at a different place than the visual switch
  and in a pass where the eye notices less.
- **[inference]** Our 26.7 k triangles is 2.5x Lamotte's "common tree" and inside Fortnite's pre-Nanite 10-20 k
  vertices. Halving per level gives about 13 k, 6.5 k, 3 k and 1.2 k. That matches the proposal in `tree_tech.md` C
  if the two-sided twins are removed first.

---

## 3. Cluster/meshlet approaches for foliage

### Findings

- **Nanite Foliage (UE 5.7, experimental)**
  ([Epic docs, read 2026](https://dev.epicgames.com/documentation/en-us/unreal-engine/nanite-foliage)) **[primary]**:
  - *Assemblies*: parts (branches, fronds) instanced inside a tree, up to 65 k part instances per assembly. Clusters are
    encoded once, and instances are transformed on demand during cluster culling, "at a small cost during culling".
  - *Skinning*: wind runs on bones, not WPO. Bounds then come from the skinning matrices, so everything stays on the
    fixed-function rasteriser. About 0.1 ms per 100 k bones. Skinning stops below 0.1 screen size. VSM **requires**
    animation to be disabled at distance; disabled instances render in bind pose.
  - *Voxels*: clusters become up to 128 bricks of 4x4x4 voxels when that has lower error than triangles. Voxel
    clusters are rasterised separately, front to back, with a stored normal distribution sampled stochastically per
    pixel.
  - *Masked*: the page says alpha masking "works poorly with Nanite because of overdraw and expensive mask functions"
    and that masked foliage stays as flat cards. Translation: the system's premise is that cards are dropped.
  - Field report: voxelised foliage produces bad ray-traced shadows in 5.7
    ([UE forum](https://forums.unrealengine.com/t/bad-raytraced-shadows-with-voxelised-nanite-foliage-in-5-7/2696233)) **[secondary]**.
- **Nanite and aggregates.** Epic: aggregate geometry (leaves, hair) "breaks" Nanite's LOD and occlusion culling.
  When hidden layers lie just under visible ones, Nanite "just draws all the layers". Enabling Nanite on card foliage is
  not always faster
  ([Epic, Working with Nanite-enabled content](https://dev.epicgames.com/documentation/en-us/unreal-engine/working-with-naniteenabled-content)) **[primary]**.
  On WPO: "Foliage using WPO is less problematic because the foliage is filled with holes and cannot really occlude
  itself" ([Epic, Nanite virtualized geometry](https://dev.epicgames.com/documentation/en-us/unreal-engine/nanite-virtualized-geometry-in-unreal-engine)) **[primary]**.
  - "Pixel Programmable Distance" (UE 5.3+) switches off masking and other pixel programs beyond a distance per
    component. ISM support landed in 5.7
    ([UE forum](https://forums.unrealengine.com/t/ism-and-nanitepixelprogrammabledistance/2642546)) **[secondary]**.
    It is the same idea as "opaque cards beyond X m".
- **meshoptimizer on cards** ([README](https://github.com/zeux/meshoptimizer/blob/master/README.md); [v1.0 notes, late 2025](https://meshoptimizer.org/v1.html)) **[primary]**:
  - `meshopt_simplify` keeps topology and seams. On a mesh of thousands of separate cards, each card is a tiny
    component with open borders, so it may stall far short of the target.
  - `meshopt_SimplifyPrune` "allows the simplifier to remove isolated components regardless of topological
    restrictions", so whole small cards can vanish. This is what thins a crown. It does not enlarge the survivors, so
    coverage drops unless that is compensated (Epic's Preserve Area exists for exactly this reason).
  - `simplifySloppy` merges disjoint but close parts and has "poor attribute quality". It breaks card UVs.
  - `SimplifyPermissive` collapses across attribute seams except protected vertices; it is experimental.
  - `clusterlod.h` is a Nanite-like cluster DAG builder, with no API stability guarantee.

  **[inference]** For cards, the better LOD tool is a card-level script: keep N % of cards, scale them up, re-pack
  (SpeedTree, TreeLODGen). Use meshoptimizer on bark and branches only.
- **Cone/back-face cluster culling.** Measured meshlet-level back-face rejection is far below the ideal 50 %: about
  14 % for 64-triangle meshlets and about 3 % for 256-triangle meshlets
  ([Kapoulkine, "Meshlet size tradeoffs", 2023-01-16](https://zeux.io/2023/01/16/meshlet-size-tradeoffs/)). Cone tests
  reject "only up to about 25 %" even on smooth dense meshes
  ([Kapoulkine, "Fine-grained backface culling", 2023-04-28](https://zeux.io/2023/04/28/triangle-backface-culling/)) **[primary]**.
  For two-sided cards the cone is useless by construction: a meshlet holds both windings, or the cards face every
  direction. **[inference]**
- **Cluster frustum and occlusion culling** is where the gain is. The Assassin's Creed Unity GPU-driven pipeline culled
  20-40 % of triangles (back-face plus cluster bounds) in the main view and more in shadows. I have these numbers only
  as quoted on a forum, with the note that the overall gain was small for that content
  ([Haar & Aaltonen, SIGGRAPH 2015](https://advances.realtimerendering.com/s2015/aaltonenhaar_siggraph2015_combined_final_footer_220dpi.pdf);
  [forum quote](https://forums.anandtech.com/threads/gpu-driven-rendering-pipelines.2442871)) **[secondary for the numbers]**.
  Wihlidal's GDC 2016 compute culling (cluster, then per-triangle back-face, small-primitive and frustum tests) found
  about 256 triangles per cluster a sweet spot on GCN
  ([slides](https://pt.slideshare.net/gwihlidal/optimizing-the-graphics-pipeline-with-compute-gdc-2016)) **[primary, summary]**.
- **Small-primitive culling.** A triangle that covers no pixel centre or sample can be dropped before raster (Wihlidal
  2016). It saves raster setup but not quad waste for triangles that do cover one pixel. Only a software rasteriser or
  LOD fixes that. **[inference]**

### Inference for our engine

- Our mesh shader already culls back faces per triangle. What is missing is **per-meshlet frustum and HiZ tests** for
  vob trees, and the same test **per VSM page**. Today one tree = one cluster = all meshlets, repeated for every page it
  touches (`engine_audit.md` §1, §6). An 18 m spruce near the camera covers many pages. Per-meshlet page tests are
  what Nanite and Alan Wake 2 do, and they cost no look at all.
- Do not build a software rasteriser. The same effect, keeping triangles above a few pixels, comes cheaper from LOD
  thresholds set by **triangle size on screen** rather than tree size.

---

## 4. Density tricks that keep the look

### Findings

- **Thinning with compensation** is standard: SpeedTree "Keep" plus size scale; Lamotte "raise Keep until the volume
  is covered"; Epic Preserve Area; TreeLODGen restoring 60-80 % of area (`tree_tech.md` C). Restoring the area **fully**
  is wrong at far levels: the crown turns into "a few solid cards".
- **Size culling of small parts.** SpeedTree shrinks small outer branches to their centre line with distance
  (`tree_tech.md` C). CryEngine merged meshes drop items per 16 m cell with distance (above). Unreal culls small shadow
  casters by `r.Shadow.RadiusThreshold`, and Nanite skinning stops below 0.1 screen size. All sources above, **[primary]**.
- **Inner crown.** No source found that fades the inner crown by distance. **[inference]** Inner shoots of a dense
  spruce are mostly hidden by outer shoots from any view outside the crown. They are the best candidates for the first
  "Keep" cut: tag them by depth from the crown hull in Blender and drop them from LOD1.
- **Trees behind trees.** Epic says foliage "cannot really occlude itself" (above). Remedy's Northlight occludes with
  everything, at meshlet precision (above). Two-phase HiZ (last frame's visible set first, then a re-test against the
  fresh HiZ) is the standard way to make the accumulated depth of real foliage occlude
  ([Haar & Aaltonen, 2015](https://advances.realtimerendering.com/s2015/aaltonenhaar_siggraph2015_combined_final_footer_220dpi.pdf);
  [Karis et al., 2021](https://advances.realtimerendering.com/s2021/Karis_Nanite_SIGGRAPH_Advances_2021_final.pdf)) **[primary]**.
  **[inference]** A single card is a poor occluder, but 3-4 crowns deep the depth buffer is nearly solid. A HiZ built
  from the depth actually rendered (not from a hand-picked occluder list) will reject far trees inside the forest. That
  is where our cost is worst (34 ms).
- **Wind LOD.** Cut leaf wind early (`tree_tech.md` C). Epic ties skinning cut-off and VSM caching to distance (above).

---

## 5. Ranked list for our forest

The order is gain divided by work, with visual risk last. Expected gains are scaled from **our** triangle-scaling test
(1/2 → −35 %, 1/4 → −60 %, 1/8 → −75 % of the added cost) where no source gives a number. **[inference]** unless stated.

| # | Measure | Expected gain | Work | Visual risk |
|---|---|---|---|---|
| 1 | **Per-meshlet frustum + HiZ culling for vob trees**, and per-meshlet tests against VSM pages (instead of all meshlets per touched page) | Most of the VSM +4.3..4.7 ms for near trees; a part of the G-buffer cost for trees cut by the frustum or partly hidden. AC Unity quote: 20-40 % of triangles culled [secondary]. | Engine: clusters per meshlet for vob meshes (the landscape already does this, `engine_audit.md` §1), with care for the 512 k meshlet-page cap. | None |
| 2 | **Real two-sided raster for alpha-tested foliage** (NoCull pipeline, normal flipped by `gl_FrontFacing`) instead of twin cards, if the 26,759 triangles include twins | Halves triangles in every pass; our test says about −35 % of the added cost | Small: pipeline flag plus content | Low: check the lighting of the back face and the translucency sign |
| 3 | **Discrete LOD chain chosen on the GPU** (4 levels, about ½ triangles per level, cards thinned with 60-90 % area restored; switch by triangle size on screen), dither cross-fade through the hashed-alpha threshold | At a forest average near 1/8 triangles: about −75 % of the added cost, roughly −10..20 ms at 1,024 trees | Medium: content script plus a LOD table in `visibility_pass.comp` | Medium: popping, crown thinning; the "solid cards" failure at far levels |
| 4 | **Coarser fixed LOD for shadow passes and the HiZ pass**; no leaf wind in shadows beyond a short range | A large share of the VSM cost that remains after #1; HiZ pass +1..4.4 ms falls with it | Small, once #3 exists | Low: shadow pop happens away from the eye; check self-shadowing inside the crown |
| 5 | **Alpha-tested trees out of the HiZ occluder pass** (trunks only, or an opaque crown-core proxy), then **two-phase HiZ** from last frame's depth | Removes the discard-heavy HiZ cost; adds tree-behind-tree rejection inside forests | Medium: two-phase HiZ changes the culling pipeline | Low: missed occlusion only costs time; one-frame disocclusion holes need the second phase |
| 6 | **Depth prepass for alpha-tested foliage + EQUAL colour pass with discard compiled out** | Pettineo: discard alone costs +32 % shader invocations; Horizon paid about 10 % of its frame for its prepass [primary]. For us, part of the remaining G-buffer cost after #1-#3 | Medium: second pipeline per material; the hashed threshold must match in both passes | Low if the thresholds match; otherwise edge flicker |
| 7 | **Tighter card outlines** (6-8-vertex hull per shoot cluster) | Humus: 8 vertices leave 52 % of a quad's fill [primary]. Gain depends on how much empty area our cards carry now (unmeasured) | Small: Blender script on the atlas | Low |
| 8 | **Inner-crown cut at LOD1 and beyond**; small-part shrink | Part of #3's budget at no visible loss | Small | Low to medium: crown looks hollow when lit from behind |
| 9 | **Cheaper far-LOD shading**: no normal map or translucency term at LOD2+ (Godot docs advice) | Pixel cost of mid trees; unmeasured | Small | Medium: a lighting shift at the switch (`tree_tech.md` G) |

Not on the list: a visibility buffer / deferred texturing (Horizon Forbidden West), and Nanite-style opaque leaves with
a software rasteriser and voxels. Both are the end state of the big studios and both are large rewrites. Revisit only
if #1-#6 leave the G-buffer pass as the cost centre. The GI +5 ms inside the forest is a separate thread. One note for
it: opacity micromaps run on any RTX GPU, but only Ada has the hardware engine (about 2x on alpha-tested traversal per
NVIDIA), so on our RTX 3080 a lower-LOD or opaque proxy in the BLAS is the likelier lever
([NVIDIA OMM SDK page](https://developer.nvidia.com/rtx/ray-tracing/opacity-micro-map/getting-started);
[Ada whitepaper](https://images.nvidia.com/aem-dam/Solutions/geforce/ada/nvidia-ada-gpu-architecture.pdf)) **[primary]**.

**Measurement order.** First turn on the existing overdraw debug state (now a compile-time `static bool`, §3) and take
one capture with pass timings for #1 and #2 alone. That splits the +27 ms into the part "drawn but not visible" and the
part "visible but expensive" before any LOD content is built.

---

## Gaps / not confirmed

- **Shipped per-level tree numbers.** No primary per-level triangle counts or screen-size thresholds for Horizon Zero
  Dawn/Forbidden West, Ghost of Tsushima/Yōtei, RDR2, AC Valhalla/Shadows, KCD 1/2, Witcher 3, Far Cry, Avatar or
  STALKER 2. The only per-level numbers found come from a practitioner interview (Lamotte 2018) and tools (TreeLODGen,
  SpeedTree).
- **Talks I could not read.** The Horizon Forbidden West deferred-texturing slides (PPTX; not downloaded under this
  task's rules), Far Cry 4 GDC 2015, Alan Wake 2 Digital Dragons 2024 and GDC 2024, AC Shadows "Micropolygon" GDC 2025.
  All may hold foliage numbers.
- **Nanite's "3x faster" software raster** for pixel-sized triangles: only from secondary citations of Karis 2021.
- **AC Unity's 20-40 % culled triangles**: only from a forum quote of the slides.
- **Pettineo's discard numbers are AMD RDNA3.** NVIDIA behaviour with discard and depth writes was not measured in any
  source I read.
- **The break-even between modelled needles and cards** under hardware raster: no published number. My threshold of
  "a few pixels per triangle" is inference.
- **Fortnite's tree and VSM posts** returned 403. Their details (WPO handling, the trees' millisecond costs) come from
  search summaries and `tree_tech.md`.
- **Nanite Foliage masked support.** The docs say masking works poorly and masked foliage stays as cards. A Chinese
  summary says masking is unsupported. I did not resolve which is exact.
- **Our own facts to check before acting.** Do the 26,759 triangles include twin cards? What fraction of each card's
  area is transparent? Is the 1/2-1/4-1/8 test made by removing cards (fill falls too) or by decimation (fill stays)?
  The answers change the weight of #2 and #7.

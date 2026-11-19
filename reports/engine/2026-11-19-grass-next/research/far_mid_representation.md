# Mid-distance and far grass: representations and LOD tricks beyond rank thinning with widening

Scope and method. About 20 web searches and fetches, 2026-11-19. Builds on `reports/engine/2026-11-17-grass-cost/research/geometry_lod_density.md` (Ghost of Tsushima 15/7 vertices, GPUOpen thinning curve, Jahrmann culling, hexaquo impostor plane, csarko notes); those are not repeated. Shell, volume and ray-marched far grass (`2026-11-18-grass-volume`) is not redone.

Trust levels used: **P** = primary (paper, talk slides or video, engine doc, developer's own blog); **V** = vendor or studio PR blog; **S** = third-party write-up or AI-generated talk summary; **F** = forum or anecdote. No primary slide deck was read: GDC Vault PDFs were not reached, and the Pixar paper URL redirects to a landing page. **LOOK** marks techniques that change the look.

Baseline facts the inferences use (from the brief, not from the web): eye height h ≈ 1.7 m. Blades are 1 triangle beyond 24 m. Density is 10/d with width × d/10. Blades sink at 48–64 m. The cost tracks blades plus vertices. Band shares: 24–48 m holds 32–42 % of blades, beyond 48 m holds 22–31 %.

## 1. Multi-blade primitives at distance (cards, strips, clumps; opaque vs alpha; shipped systems)

### Takeaway
No shipped system was found that swaps individual blades for opaque multi-blade "clump" meshes at 24–64 m. The documented pattern for grass is still individual blades, then fewer and wider blades, then a ground texture. Multi-element cards and clumps are documented for leaves and trees, and for indie grass. The modern AAA direction is the reverse of alpha cards: Witcher 4/UE 5.7 moved from cards to real geometry plus a voxel far field, and report alpha cards as 3–4× slower than geometry. For an engine whose far blade is already one triangle, a clump saves instance and vertex overhead, not triangles.

### Cited Findings
- Ghost of Tsushima: distant tiles swap the ground material for artist-authored textures so far fields do not look like "monotonous green expanses". Blade normals are tilted outward and blades get small view-space shifts to keep "density and fullness". The talk lists decoupling LOD from tile size, to extend the visible range, as future work. No distances or tile sizes are given. (S, AI summary of the GDC 2021 video) — [gist.ly summary](https://gist.ly/youtube-summarizer/procedural-grass-systems-in-ghost-of-tsushima-achieving-art-direction)
- Ghost of Tsushima: shadows from a full compute or vertex shadow pipeline for grass were judged too costly. Impostors approximated grass heights for shadowing, and screen-space shadows added the fine detail. (S) — [gist.ly summary](https://gist.ly/youtube-summarizer/procedural-grass-systems-in-ghost-of-tsushima-achieving-art-direction)
- Ghost of Tsushima: "twice as wide for half as many blades" at distance. (S, community thread; consistent with the earlier note) — [threadreaderapp](https://threadreaderapp.com/thread/1688939411575119872)
- Ghost of Yōtei (2025): Sucker Punch says the GPU compute renderer can now produce double the grass and renderable items. Grass deforms with wind and characters, and a "cut buffer" lets the player cut grass. Distant mountains bake models and detailed terrain materials into textures. Over one million distant trees, rocks and bushes are culled to about 60,000 for the G-buffer. GPU compute does occlusion culling, memory allocation and draw records, with results read back to the CPU to build command lists. No grass LOD distances or costs are given. (P/V, studio director of programming on the PlayStation Blog) — [PlayStation Blog deep dive](https://blog.playstation.com/?p=411201)
- Witcher 4 tech demo (UE 5.6/5.7, base PS5 at 60 fps) as reported from the Unreal Fest talk:
  - about 7 million grass instances, 1.7 million shrubs and 500,000+ trees over 26 km²;
  - Nanite pass budget about 4 ms excluding shadows;
  - alpha-tested cards were 3–4× slower than Nanite geometry;
  - custom LODs gave 20–30 % gains but popped;
  - the grass technique itself is not described.

  (S, AI-generated Vietnamese summary of the talk, low trust) — [readray/lilys summary](https://readray.ai/notes/vi/unreal-engine-5-20251128/witcher-4-large-animated-foliage-demo)
- Witcher 4: the team replaced auto grass with runtime GPU PCG for "small debris, trash, grass". Nanite foliage replaces "the card-based system used for multiple console generations". (F, forum quote of a Digital Foundry interview) — [search result pointing to vgchartz/reddit threads](https://gamrconnect.vgchartz.com/thread.php?id=249372)
- Leaves collapse into composite cards at mid range ("a clump of 30 leaves becomes 1 textured card"). The article puts naive masked foliage at 8–15× effective overdraw, and at 1–2× once prepass, sorting, culling and impostors are added. These are its own synthesis numbers, and they are about trees, not grass. (S) — [cinevva foliage overdraw](https://app.cinevva.com/blog/2026-05-11-foliage-overdraw)
- Forum practice: combine 10–12 blades into one small mesh instead of a card per blade, and simplify distant LODs because small-triangle cost adds up over millions of instances. (F) — [UE forum, foliage overdraw testing](https://forums.unrealengine.com/t/foliage-overdraw-testing/80905)
- Instance clusters of grass pieces with random rotation, scale and colour, rather than single blades. (F) — [three.js discourse](https://discourse.threejs.org/t/instanced-mesh-of-instanced-mesh/66433)

### Inferences (mine)
- **Shared-base "sawtooth" strip.** Today a far blade is 1 triangle with 3 vertices and its own instance. A clump of k blades drawn as k triangles over shared base vertices needs 2k+1 vertices instead of 3k, about 33 % fewer vertices. It also needs one instance record instead of k. Example: k = 4 gives 9 vertices instead of 12, with 4× fewer instances. Triangles do not drop. Because the cost tracks blades and vertices submitted, this is a direct lever with no change in triangle count.
  - **LOOK:** blades in a clump share a root line and look more regular unless tips get independent height, lean and colour from a per-blade hash.
  - Wind must stay per tip; it is evaluated per vertex already.
- **Opaque cards with an alpha cut-out** are the wrong direction here: they defeat early-Z, add overdraw, and cost more on Apple GPUs (hidden-surface removal is lost with `discard`; see the volume note). This agrees with Witcher 4's 3–4× figure.
- **Opaque shaped geometry** (a jagged polygon whose top edge is the clump silhouette) keeps early-Z. At 24–64 m one blade is about 1–2.5 px wide after widening, so a jagged top edge of a few triangles can stand in for 3–6 tips.
  - **LOOK:** tips become correlated in height and lean. This reads as "fenced" under wind unless each tip vertex is animated on its own.
- Clumping changes **where the cost sits**, not the band. The 24–48 m band (32–42 % of blades) is where a k = 3–4 clump would pay most.

### Gaps
- No shipped title documents a grass clump or multi-blade LOD tier with numbers. The Ghost of Tsushima and Yōtei far-tile sizes and switch distances were not found in any source read.
- The Witcher 4 numbers come from an AI-generated summary. The original talk video or slides were not read.

## 2. Density by projected area, grazing angle and occlusion instead of distance

### Takeaway
No shipped game documents a pixels-per-blade budget for grass. The theory exists in offline and academic aggregate simplification:
- Pixar's stochastic simplification (Cook et al. 2007) prunes by a view-dependent fraction u, scales survivors' area by 1/u, and corrects contrast so the image does not go flat.
- View- and visibility-dependent pruning of botanical scenes (Neubert et al. line of work) is the academic counterpart.

From eye height, simple coverage geometry says the current 1/d thinning leaves the 24–64 m bands over-covered. A grazing-aware rule could drop roughly a quarter of all blades. That is an estimate, not a measurement.

### Cited Findings
- Stochastic simplification of aggregate detail (Cook, Halstead, Planck, Ryu, SIGGRAPH 2007 / ACM TOG 26(3)):
  - elements are pruned in a random order;
  - survivors are scaled so total area is preserved (area × 1/u, where u is the fraction kept);
  - survivors' shading is altered so the image contrast does not change;
  - the paper's figure removes 90 % of a plant's leaves and compares area correction alone with area plus contrast correction.

  (P, but read through a search summary; the PDF URL redirects) — [Pixar library page](https://graphics.pixar.com/library/StochasticSimplification), [bibtex](https://researchr.org/publication/CookHPR07/bibtex)
- Botanical pruning work describes scaling the remaining geometry "such that the total area of rendered surfaces is equal to the original area". (P, search snippet only; the PDF was empty when fetched) — [Improved model- and view-dependent pruning of large botanical scenes](https://typeset.io/pdf/improved-model-and-view-dependent-pruning-of-large-botanical-34hw73zbkg.pdf)
- Jahrmann & Wimmer: orientation, distance and occlusion culling remove up to 75 % of blades "without noticeable difference" (already in the earlier note). A third-party Vulkan reimplementation with 4.19 M blades measured 178.32 ms with no culling, 123.23 ms with orientation culling and 75.89 ms with distance culling. The numbers depend strongly on the camera. (S, reimplementation) — [TU Wien page](https://www.cg.tuwien.ac.at/research/publications/2017/JAHRMANN-2017-RRTG/), [search summary of reimplementations](https://github.com/LanLou123/Real-Time-Grass-Rendering)
- Alan Wake 2 (Northlight): mesh shaders allow occlusion culling "down to a single-pixel precision, using everything in a scene as an occluder. Even meshlets are culled, not just meshes." (V, Remedy; quoted via press) — [Remedy: How Northlight makes Alan Wake 2 shine](https://remedygames.com/article/how-northlight-makes-alan-wake-2-shine), [REAC 2024 slides](https://enginearchitecture.org/downloads/REAC_2024_Remedy.pdf) (slides not read)
- UE 5.7 Nanite: clusters switch to voxels only when the voxel representation has lower simplification error than triangles. This is error-driven, i.e. projected-size-driven, not distance-driven. Animation (skinning) stops below `r.Skinning.DefaultAnimationMinScreenSize` = 0.1. (P, Epic doc) — [Nanite Foliage doc](https://dev.epicgames.com/documentation/en-us/unreal-engine/nanite-foliage)
- Gears Tactics VRS chose shading rates by object size and by how much fog hides an object. These are projected-importance metrics for shading, not geometry. (P/V, Microsoft DirectX blog) — [Gears Tactics VRS](https://devblogs.microsoft.com/directx/gears-tactics-vrs/)

### Inferences (mine; LOOK where marked)
- **Coverage model at grazing view.** A sight line at distance d descends at angle ≈ h/d. Over a canopy of height H it passes through the blade layer for a ground depth L ≈ H·d/h.
  - For H = 0.3–0.5 m and h = 1.7 m: L ≈ 4–7 m at 24 m and 8–14 m at 48 m.
  - With n blades/m² of effective width w, the fraction of sight lines blocked inside the canopy is ≈ 1 − exp(−n·w·L), assuming Poisson placement.
  - The current rule n ∝ 10/d with w ∝ d/10 keeps n·w constant. Because L grows ∝ d, coverage *rises* with distance.
  - Holding coverage constant instead needs n·w ∝ 1/d. With widening kept at d/10, that means density ∝ 1/d² beyond some anchor distance d0.
- **Rough saving if d0 = 24 m** (density × 24/d on top of today's rule, from the brief's band shares):
  - the 24–48 m band keeps about 0.67 of its blades on average, saving about 33 % of a 32–42 % share;
  - the 48–64 m band keeps about 0.43, saving about 57 % of a 22–31 % share;
  - total ≈ 23–31 % of all blades (my arithmetic).
- **LOOK:** coverage is not the only cue. Fewer, wider blades reduce colour variance and tip-silhouette frequency, so the field reads flatter. Cook et al. use contrast correction for exactly this: push each survivor's colour away from the local mean by about 1/√u. The look also depends on whether the near-horizon top silhouette stays jagged. Accept by a clip, not a still.
- **Slope awareness.** The same model gives L ≈ H·d/(h + d·tan s) for ground tilted toward the camera by slope s. Hillsides facing the viewer need *more* blades per m² than flat ground at the same distance, and fields seen exactly edge-on need fewer. A per-tile slope term is cheap because ground height is known per blade.
- **Per-blade screen-size test.** Beyond 24 m a widened blade is about 1–3 px wide, so sub-pixel slivers are unlikely. Height sinking already handles the far end. Screen size is therefore better used as the input to the density curve than as a cull.
- **Occlusion density.** Front blades hide everything within L behind them, so at 48 m the hidden depth is about 10 m. HiZ cell culling (option 4 in the volume note) only removes blades behind terrain. The coverage rule above removes blades hidden behind other grass, and needs no HiZ.

### Gaps
- No shipped title or talk gives a blades-per-pixel budget or a grazing-angle density formula for grass. The model above is unvalidated.
- The exact contrast-correction formula of Cook et al. was not read from the paper (the PDF link redirects). The 1/√u figure above is my recollection and must be checked against the paper.

## 3. Temporal and stochastic approaches

### Takeaway
Dithered or stochastic LOD transitions that rely on TAA are shipped practice (UE). Stochastic shading inside a far representation is shipped in UE 5.7: Nanite voxels pick one normal per pixel at random and leave TAA/TSR to resolve it. No shipped title was found that draws *alternating subsets* of opaque grass blades per frame, or caches far grass in impostor tiles updated over several frames. Variable-rate shading is shipped for foliage scenes, but it targets pixel-shading cost, not this vertex-bound pass.

### Cited Findings
- UE: dithered LOD transitions "only function" on platforms with temporal AA. Users report harsh dither contrast looking bad. A Metal (iOS) bug showed grass that swaps LOD going dark until the swap completes. (F) — [UE forum](https://forums.unrealengine.com/t/how-to-manage-foliage-aliasing-dithering-on-grass-trees-without-tsr/2511404), [UE-32289](https://issues.unrealengine.com/issue/UE-32289), [dithered LOD bug](https://answers.unrealengine.com/questions/424215/view.html?sort=votes)
- A foliage renderer write-up seeds its stochastic pattern from the bit-reversed low bits of the frame number when TAA is on, and from a time-free seed when TAA is off, because time-varying noise is visible on a still camera. It reports TAA cost about 10–15 % of framerate and "almost completely removed noise from foliage". (S) — [borealorienteering OIT and AA](https://borealorienteering-web.pages.dev/tech/oit_and_aa.html)
- Hashed alpha testing (Wyman & McGuire, I3D 2017) uses a stable per-object-space hash threshold so thin alpha geometry does not vanish at distance. Both fixed and random thresholds can hurt the temporal stability of fine detail. (P) — [NVIDIA paper](https://research.nvidia.com/sites/default/files/pubs/2017-02_Hashed-Alpha-Testing/Wyman2017Hashed.pdf)
- UE 5.7 Nanite voxels:
  - clusters of up to 128 bricks of 4×4×4 voxels;
  - drawn front to back in depth buckets;
  - a per-voxel normal distribution is stored, and **one normal is picked stochastically per pixel** for the G-buffer; a future release will shade with the distribution directly "to reduce noise";
  - the settings are experimental.

  (P, Epic doc) — [Nanite Foliage](https://dev.epicgames.com/documentation/en-us/unreal-engine/nanite-foliage)
- With Nanite foliage on in 5.7, TSR thin-geometry detection is reportedly enabled as well (`TSR.ThinGeometryDetection`, manual in 5.6.1). Voxelised meshes show holes when small on screen (5.7.4). (F) — [UE forum: Nanite fence flickering with TSR](https://forums.unrealengine.com/t/nanite-fence-flickering-at-distance-with-tsr/2720993), [voxelization holes](https://forums.unrealengine.com/t/nanite-voxelization-holes-in-the-mesh/2723578)
- Gears Tactics VRS (RTX 2080 Super, 4K):
  - frame time 23.3 ms with VRS off, 20.9 ms with "On" (−10.3 %), 18.9 ms with "Performance" (−18.9 %);
  - the base pass saved only 0.6 ms, in Performance mode;
  - dynamic and opacity-masked objects were excluded because they "lost key detail" and looked worse in motion;
  - artifacts appeared in "a sand environment with various foliage";
  - pushing rates further reached about 30 % but looked too degraded.

  (P/V) — [Microsoft DirectX blog](https://devblogs.microsoft.com/directx/gears-tactics-vrs/)
- Impostor caching amortises cost over frames. The refresh is driven by angular error between the reference and current view; Schaufler's criterion compares bounding-box angle error with the pixel angle. These are academic, from the 1990s and 2000s, and about objects, not animated grass. (P) — [Lawlor thesis](https://lawlor.cs.uaf.edu/~olawlor/academic/thesis/lawlor_prelim.pdf), [Schaufler 96](https://www.cs.princeton.edu/courses/archive/spr01/cs598b/papers/schaufler96.pdf), [Jeschke layered impostors](https://www.cg.tuwien.ac.at/research/vr/layeredimpostor/jeschke_gi2002.pdf)

### Inferences (mine; all LOOK)
- **Alternating opaque subsets with TAA integration** (draw half of the far blades on even frames, half on odd): in a deferred renderer each frame writes different depth and normals into the same pixels.
  - TAA history rejection by depth or colour clamp will see each pixel flip between blade and ground. The likely result is shimmer and partially rejected history, not a smooth blend. Ghosting is worst under wind, because motion vectors of the missing subset do not exist in that frame.
  - Expect at most about 2× savings on the affected band, with flicker risk at 1–3 px blade width.
  - It is cheaper and safer to apply the static coverage rule from section 2.
  - If tried at all, try it only on the sinking band (48–64 m), and accept it only by a clip with wind and a moving camera.
- **Stochastic LOD cross-fade** (per-blade random switch threshold jittered by a frame-index hash) applied to the *transition* only is low risk. It replaces the deterministic rank threshold inside a short ramp and spreads the pop over frames. Note that it is not free on opaque geometry: both versions are drawn inside the ramp.
- **Impostor tiles of far grass refreshed over several frames:** wind and lighting change every frame and the camera moves at walking speed, so a cached tile is stale by angle quickly. With rays, GI and TAA the tile would also have to be lit consistently with the live G-buffer. No shipped grass example was found, and the ground texture (`GrassFarTint`) already plays this role statically.
- **VRS or coarse shading for grass only:** the pass is vertex- and primitive-bound (cost tracks blades and vertices), and G-buffer shading of opaque blades is cheap, so VRS would cut little. Gears Tactics excluded masked and dynamic objects for artifacts. Not worth it here.

### Gaps
- No shipped title documents temporally alternating blade subsets, or checkerboard rendering of grass only.
- No source on far-grass impostor tiles with amortised updates in a game.

## 4. What recent titles and engines do (2023–2026)

### Takeaway
Public detail on grass LOD in 2023–2026 AAA titles is thin. Studios talk about GPU-driven culling, wind and budgets, not about far-grass representation. Only UE 5.7 Nanite Foliage documents a new far representation (voxels), and it targets trees and shrubs more than blades.

### Cited Findings
- **Ghost of Yōtei:** see section 1. Grass budget doubled, distant geometry baked to textures, GPU compute occlusion culling; no grass LOD numbers. (P/V) — [PlayStation Blog](https://blog.playstation.com/?p=411201); [80.lv retelling](https://80.lv/articles/sucker-punch-breaks-down-tech-that-brought-ghost-of-y-tei-s-world-to-life) (not read)
- **Assassin's Creed Shadows (Anvil):** "all the vegetation … is physically animated on the GPU based on a dynamic wind system driven by a fluid simulation". The GDC 2025 talk (Nicolas Lopez, "Rendering Assassin's Creed Shadows") covered a GPU-driven pipeline; no grass LOD details were found. (V) — [Ubisoft tech Q&A](https://www.ubisoft.com/en-us/game/assassins-creed/news/4XbPPtFyQEtIMWrA9xVDmZ/assassins-creed-shadows-tech-qa), [80.lv on GDC talk](https://80.lv/articles/gdc-2025-talk-rendering-assassin-s-creed-shadows)
- **Kingdom Come: Deliverance II (CryEngine):** no technical source found. Players complain that open fields look much worse than forests ("looks like a ps2 game"). (F) — [Steam discussion](https://steamcommunity.com/app/1771300/discussions/0/600770973001661243)
- **Avatar: Frontiers of Pandora (Snowdrop):** a "microdata" system crams thousands of assets into a frame, the scatter system was generalised for Snowdrop, and dynamic resolution is used. No grass LOD talk was found. (V) — [Massive: Crafting Pandora's landscape](https://www.massive.se/?p=18290), [GamesBeat interview](https://gamesbeat.com/how-massive-created-the-world-of-avatar-frontiers-of-pandora-interview/)
- **Horizon Forbidden West:** "loosely tiled deferred texturing" accelerates foliage and alpha-tested geometry (GDC talk by James McLaren). Patch 1.07 retuned vegetation for image quality in performance mode. (P/V) — [Guerrilla](https://www.guerrilla-games.com/read/adventures-with-deferred-texturing-in-horizon-forbidden-west), [FPS Review on patch](https://www.thefpsreview.com/2022/03/09/horizon-forbidden-wests-latest-patch-improves-graphics-quality-of-vegetation-in-performance-mode)
- **Alan Wake 2:** GPU skinning for vegetation (GDC 2024, "Large Scale GPU-Based Skinning for Vegetation"), mesh-shader GPU-driven pipeline, pixel-precise occlusion and meshlet culling. (V) — [Remedy GDC 2024 talks](https://www.remedygames.com/article/explore-remedys-gdc2024-talks-on-creating-alan-wake-2), [GDC Vault](https://gdcvault.com/play/1034310/Large-Scale-GPU-Based-Skinning)
- **Witcher 4 / UE 5.6–5.7:**
  - Nanite Assemblies hold up to 65k parts each; the largest tree asset went from about 3.5 GB to about 29 MB;
  - 500k tree instances;
  - wind runs through bones, not world-position offset (100,000 bones ≈ 0.1 ms GPU);
  - voxels for far clusters.

  The source for each point: Assemblies and voxels — (P) [Epic doc](https://dev.epicgames.com/documentation/en-us/unreal-engine/nanite-foliage). Grass via runtime GPU PCG — (F) see section 1. 7 M grass instances and 4 ms Nanite budget — (S) [readray summary](https://readray.ai/notes/vi/unreal-engine-5-20251128/witcher-4-large-animated-foliage-demo). Experimental and "explicitly unfinished" status in 5.7 — (S) [Digital Production on 5.7](https://digitalproduction.com/2025/11/12/unreal-engine-5-7-foliage-pcg-and-in-editor-ai/).
- **Hellblade II, Unity 6 demos:** not researched within the call budget (see Gaps).

### Inferences (mine)
- The 2023–2026 trend is GPU-driven culling (occlusion at pixel or meshlet granularity) plus animation that turns off with screen size, rather than new far-grass representations. For this engine, the closest equivalents are:
  - occlusion and coverage-based density (section 2);
  - stopping per-vertex wind work on far blades. **LOOK:** far blades go still. Use a cheap per-blade sway in place of per-vertex wind instead, which matters little for 1-triangle blades anyway.
- Nanite voxels are not a fit: they need the Nanite cluster hierarchy, and they shade stochastically, relying on TSR. They are evidence that the industry accepts per-pixel stochastic shading of far foliage resolved by temporal AA.

### Gaps
- No primary grass numbers for Ghost of Yōtei, AC Shadows, KCD2, Avatar, HFW, Alan Wake 2 or Hellblade II. Their talks (if any) were not read.
- Unity 6 demos (Time Ghost / Fantasy Kingdom) and Hellblade II were not searched for lack of budget.

## 5. Pitfalls

### Cited Findings
- Custom LODs gave 20–30 % gains but caused popping (Witcher 4). (S) — [readray summary](https://readray.ai/notes/vi/unreal-engine-5-20251128/witcher-4-large-animated-foliage-demo)
- Voxel or far representations show holes when small on screen. TSR needs thin-geometry detection for foliage. (F) — [UE forum holes](https://forums.unrealengine.com/t/nanite-voxelization-holes-in-the-mesh/2723578), [TSR fence flicker](https://forums.unrealengine.com/t/nanite-fence-flickering-at-distance-with-tsr/2720993)
- World-position-offset wind inflates cluster bounds and invalidates virtual shadow map pages. Animation must stop at distance for VSM. (P) — [Epic doc](https://dev.epicgames.com/documentation/en-us/unreal-engine/nanite-foliage)
- Coarse shading on dynamic or masked objects looked worse in motion (Gears Tactics). (P/V) — [DirectX blog](https://devblogs.microsoft.com/directx/gears-tactics-vrs/)
- Pruning without contrast correction makes aggregates look washed out (Cook et al. figure). (P via summary) — [Pixar library](https://graphics.pixar.com/library/StochasticSimplification)
- Time-varying noise seeds are visible on a still camera; tie them to the frame index only when TAA is on. (S) — [borealorienteering](https://borealorienteering-web.pages.dev/tech/oit_and_aa.html)
- Grass that swaps LOD went dark until the swap completed on Metal in UE. This was an engine bug, but it shows the colour and energy mismatch risk of dual-drawn transitions. (F) — [UE-32289](https://issues.unrealengine.com/issue/UE-32289)

### Inferences (mine)
- Any density change beyond 24 m shifts the energy match at the 48–64 m seam with `GrassFarTint`. Fewer, wider blades show less self-shadow and fewer ground gaps, so the band brightens or darkens. Re-derive the seam tint after a density change.
- A clump or sawtooth strip correlates neighbouring tips. Wind phase must stay per tip, or the field reads as moving fences.
- Accept every LOOK change here by a walking clip with gusts, not stills (project rule). Compare near-horizon tip silhouette and colour variance, not only coverage.

### Gaps
- No team post-mortem found on far-grass mistakes in 2023–2026 titles.

# Vegetation, ground cover and small scatter in modern open-world games (state as of 2026-11-12)

Scope: placement, grass, trees and bushes, small scatter, wind, lighting, reference numbers from real forests and meadows, free assets and tools, and pitfalls. About 20 searches and fetches. Several primary slide decks were too large to fetch or contain no text (HZD PDF over 10 MB; the GDC Vault pages hold only abstracts), so many numbers below come from secondary write-ups and are marked as such. Anything marked **[unverified]** comes from my own background knowledge or from a single secondary source, not from a primary source checked here.

## Placement: rule-based procedural placement

### Takeaway
Shipped AAA open worlds place vegetation by evaluating density fields built from world data (height, slope, material and splat masks, painted masks, distance fields from roads and water), then turning them into points deterministically. Horizon Zero Dawn does this on the GPU at runtime around the player for about 0.25 ms. Far Cry 5 does it offline in Houdini with ecological rules. Hand-authored content is protected by exclusion masks and footprints that feed into the same density graph.

### Cited findings
- Horizon Zero Dawn (Guerrilla, GDC 2017, Jaap van Muijden):
  - The system is density-based so it can run on the GPU and stay deterministic and locally stable.
  - Placement logic produces per-layer density maps from mipmapped world data at a scalable granularity. A generate step then turns them into points by dithering, with a two-sided threshold test on layered densities.
  - Scale: 500+ asset types, over 100,000 objects in the scene, and about 250 µs average GPU busy load while the player moves.
  - World data footprint is about 32 bits per m² (secondary summary of the slides).
  - Sources: [Guerrilla slides PDF, 2017](https://www.guerrilla-games.com/media/News/Files/GDC2017_VanMuijden_GPUBasedProceduralPlacementInHorizonZeroDawn.pdf) (too large to fetch; numbers via search-indexed extracts), [GDC Vault 2017](https://www.gdcvault.com/play/1024120/GPU-Based-Run-Time-Procedural), [80.lv summary](https://80.lv/articles/real-time-procedural-placement-in-horizon-zero-dawn/?amp=1). One extract was ambiguous between "250 µs" and "around 250 per frame". Check slide text before quoting.
- HZD artist control: the system generates environment, wildlife, sound, effects and gameplay elements around the player as they move. Artists paint tree lines, redirect roads and change terrain or biomes and edit the rules in a graph editor — [Guerrilla article, 2017](https://www.guerrilla-games.com/read/gpu-based-procedural-placement-in-horizon-zero-dawn).
- Far Cry 5 (Ubisoft, GDC 2018, Étienne Carrier):
  - Houdini plus Houdini Engine tools handle biomes, terrain texturing, freshwater networks, cliff rocks, power lines and a fog density map.
  - Vegetation follows ecological rules, with species competing for ground. Grass depends on humidity and is oriented along a wind vector map. Most of the vegetation was built procedurally.
  - The tools were designed so the world regenerates when terrain changes daily.
  - Sources: [GDC Vault 2018](https://gdcvault.com/play/1025557/Procedural-World-Generation-of-Far), [80.lv, 2018](https://80.lv/articles/houdini-procedural-world-generation-of-far-cry-5/), [PlayStation Blog, 2018](https://blog.playstation.com/2018/03/22/the-procedural-world-generation-of-far-cry-5/), [SideFX talk page](https://www.sidefx.com/learn/talks/procedural-world-generation-far-cry-5/).
- Far Cry 5 terrain is a separate GPU compute pipeline for LOD, culling, stitching and rendering the heightfield (Jeremy Moore, GDC 2018) — [GDC Vault](https://gdcvault.com/play/1025480/Terrain-Rendering-in-Far-Cry).
- UE 5.7 (Nov 2025) extended PCG (Procedural Content Generation framework) and added an experimental Procedural Vegetation Editor that makes Nanite-ready foliage in the editor — [Digital Production, 2025](https://digitalproduction.com/2025/11/12/unreal-engine-5-7-foliage-pcg-and-in-editor-ai/), [80.lv, 2025](https://80.lv/articles/unreal-engine-5-7-is-now-availabe).

### Inferences
- For a fixed, hand-made world like the remaster target, an **offline (bake-time) deterministic** density-graph pipeline fits better than HZD-style runtime generation. Its output is an instance list per world cell, so it is reproducible, diffable and reviewable in screenshots. It also keeps runtime cost to culling and drawing.
- Runtime GPU generation (HZD/Tsushima style) is still the right choice for the densest, cheapest layers (grass, small pebbles). Those would cost too much memory if stored per instance.
- The way to keep the original's paths, clearings and sight lines is to build them **as inputs**: a distance field from the original waypoint/path network and vob footprints, painted "clearing" and "sight-line" masks, and slope and material masks, all multiplied into every density layer. This is the HZD/FC5 pattern, where the rules consume painted masks and road splines.
- HZD's density-plus-dither method gives locally stable results. Editing a mask changes placement only nearby, which keeps review diffs small.

### Gaps
- HZD slide-level details were not read first-hand: tile sizes, generation radius, density-map resolution, number of layers, exact footprint/exclusion mechanism. Read the PDF locally to confirm them.
- I found no primary numbers on Ghost of Tsushima tree and bush placement (only grass), and no measured UE PCG runtime costs from Epic.
- Unity's placement tooling in 2026 was not researched.

## Grass and ground cover

### Takeaway
State of the art is GPU-generated individual blade geometry: Ghost of Tsushima, GDC 2021. Blades are curves with about 15 vertices near the camera and 7 farther out. Grass is built per tile by a compute pass, thinned with distance while blades get wider, and animated by a scrolled 2D wind noise. Alpha cards (2 triangles standing in for 10 to 20 blades) remain the cheap fallback. Exact Tsushima ms and density figures are not confirmed from primary material.

### Cited findings
- Tsushima grass (Eric Wohllaib, Sucker Punch, GDC 2021 Advanced Graphics Summit): individual blades are generated on the GPU, each with its own procedural look and animation. The talk covers "acres of grass" within memory and performance limits and making thousands of blades read as a natural field — [GDC Vault, 2021](https://gdcvault.com/play/1027214/Advanced-Graphics-Summit-Procedural-Grass).
- Secondary details, consistent across several reimplementations and blogs but not confirmed against the talk:
  - Two blade LODs of 15 and 7 vertices, blended by shifting vertices near the switch point to avoid popping.
  - When switching to larger far tiles, 3 of 4 blades are dropped.
  - Far blades are made about twice as wide for half as many.
  - Blades are cubic Bézier curves built per tile in compute.
  - A second noise gives clumping, so neighbouring blades share properties.
  - Wind is a scrolled 2D Perlin noise that both CPU and GPU can sample.
  - Short blades can be "folded" into two for double apparent density.
  - Sources: [tigerabrodi blog, 2026](https://tigerabrodi.blog/grass-in-ghost-of-tsushima), [StylizedStation thread](https://x.com/StylizedStation/status/1688939411575119872), [GodotGrass](https://github.com/2Retr0/GodotGrass), [Jair Santana, Godot emulation](https://jairsantana.com/posts/grass/).
- "About 83,000 blades on screen at about 2.5 ms" circulates for Tsushima. One source itself flags this as **[unverified]** — [hashnode summary](https://hashnode.com/posts/what-we-can-learn-from-grass-in-ghost-of-tsushima-renders/69e130b2ffbb787634eb9ef4). Do not plan around it.
- General figures from a 2026 write-up:
  - A blade costs about 6 to 10 triangles.
  - An alpha card covers 10 to 20 blades with 2 triangles.
  - Alpha cards were the standard for 15+ years.
  - Source: [tigerabrodi blog](https://tigerabrodi.blog/grass-in-ghost-of-tsushima).
- Tile culling trade-off: whole tiles are culled at once, so there is a balance between the number of draw calls and culling granularity — [GodotGrass README](https://github.com/2Retr0/GodotGrass).

### Inferences
- A practical plan for this engine:
  - Use compute-generated blade grass for meadows and farmland only, in a near ring of about 30 to 60 m **[unverified distance, my estimate]**.
  - Use cards or a terrain colour/normal "grass carpet" beyond that.
  - Use a deterministic hash per cell, so blades never need storing.
- Grass shadows should be contact-only (screen-space or short-range). Grass should not cast into the cascaded or virtual shadow map beyond a few metres. See the lighting section for the VSM cost.
- Player interaction: Tsushima-style bending can sample a small displacement texture around actors. Details are not sourced here.

### Gaps
- The primary Tsushima talk was not read, so tile size, blades per tile, draw distance, GPU ms and shadow handling are missing. The talk video is the source to transcribe.
- No sourced numbers for grass instances per m² in shipped games.

## Trees and bushes: leaves, overdraw, LOD, impostors, Nanite foliage

### Takeaway
Alpha-tested leaf cards cause 8 to 15× shading overdraw in dense forests. Modern stacks cut it to about 1 to 2×: depth prepass or visibility buffer, GPU cluster culling, composite or impostor LODs, and hashed or dithered alpha. UE 5.7's experimental Nanite Foliage moves to modelled leaves plus assemblies plus voxels at distance. Octahedral impostors are the common far-LOD.

### Cited findings
- 2026 overview of foliage overdraw (secondary, with a reference list):
  - Naive dense forest runs the leaf shader about 8 to 15× per pixel. The combined techniques bring this to about 1 to 2×, and total foliage cost falls 4 to 6×.
  - Meshes are split into 64- or 128-triangle clusters for culling.
  - Composite LOD merges a 30-leaf clump into one card.
  - Hashed alpha (Wyman and McGuire, I3D 2017) replaces the fixed 0.5 cutoff to fight shimmer.
  - Some engines shade foliage at 1/2 or 1/4 rate.
  - Masked Nanite foliage became "competitive" with opaque by UE 5.5.
  - Cited sources include Haar and Aaltonen, GPU-Driven Rendering Pipelines (AC Unity), SIGGRAPH 2015; Wihlidal, GDC 2016; Sanders, HZD vegetation, GDC 2018; Karis et al., Nanite, SIGGRAPH 2021.
  - Source: [Cinevva blog, 2026](https://app.cinevva.com/blog/2026-05-11-foliage-overdraw).
- AC Unity GPU-driven pipeline (SIGGRAPH 2015), as summarised there: 20 to 40% of triangles culled, 30 to 80% of shadow triangles culled, 10× more on-screen instances than the previous generation — [Cinevva, 2026](https://app.cinevva.com/blog/2026-05-11-foliage-overdraw) (secondary).
- Alpha-tested foliage defeats early-Z: the fragment shader must run to discard — [Cinevva, 2026](https://app.cinevva.com/blog/2026-05-11-foliage-overdraw).
- UE 5.7 Nanite Foliage (Nov 2025, **experimental**):
  - It combines Nanite Assemblies (instanced parts such as branches), Nanite Skinning (wind motion) and Nanite Voxels (distant trees become a voxel representation). Voxels are said to improve far shadows and lighting.
  - The Procedural Vegetation Editor is an experimental plug-in.
  - Sources: [Digital Production, 2025](https://digitalproduction.com/2025/11/12/unreal-engine-5-7-foliage-pcg-and-in-editor-ai/), [80.lv voxel foliage, 2025](https://80.lv/articles/get-a-glimpse-of-nanite-foliage-with-voxel-representation-in-ue5-7).
- Nanite Foliage caveats (community reports):
  - Voxelizing ordinary meshes gave no speedup; gains need assemblies — [Reddit thread, 2025](https://reddit.birdcat.cafe/r/UnrealEngine5/comments/1mcbgk1/has_anyone_tested_ue57_voxelized_nanite_foliage).
  - In 5.7.4, splitting voxel and non-voxel parts can leave holes — [UE forum, 2026](https://forums.unrealengine.com/t/nanite-voxelization-holes-in-the-mesh/2723578).
  - Assemblies suit branches better than single leaves — [UE forum](https://forums.unrealengine.com/t/new-nanite-voxel-foliage/2585978).
- Octahedral impostors (Ryan Brucks, shaderbits, 2018) are the common far-LOD. Typical bake parameters:
  - Unigine exposes 4 to 64 horizontal frames and 1 to 16 vertical frames.
  - With too few angles, impostors pop and blur. An impostor should replace the mesh only when the mesh covers no more pixels than the bake resolution.
  - Sources: [Unigine docs](https://developer.unigine.com/docs/future/editor2/tools/impostors_creator), [three.js forest demo, 2025](https://discourse.threejs.org/t/a-forest-of-octahedral-impostors/85735), [arycama impostor notes, 2024](https://arycama.github.io/2024/10/05/Imposters.html).
- The Witcher 3 used SpeedTree, with about 20 large tree and bush species and a handful to over a dozen models per species, plus smaller plants — [SpeedTree via search summary](https://store.speedtree.com/?p=2530), [80.lv REDengine 3 interview](https://80.lv/articles/the-witcher-3-interview-a-deeper-look-into-redengine-3/).

### Inferences
- With no Nanite equivalent in the engine, the realistic stack is:
  1. GPU-driven instance culling: frustum plus HiZ occlusion, compacted into indirect draws per LOD.
  2. A depth prepass for masked foliage, or alpha-test only in the prepass with an equal-depth shading pass.
  3. Hashed or dithered alpha with alpha-to-coverage-style mip fixes (Castaño 2010).
  4. Three or four mesh LODs, then an octahedral impostor.
- The Witcher 3 figure suggests about 20 hero species is enough for a whole AAA world. A Gothic island could do with about 8 to 12 tree and bush species × 3 to 5 variants **[inference]**.
- Triangle budgets commonly quoted **[unverified, background knowledge]**: hero tree LOD0 about 20k to 100k triangles with cards, LOD1 about 50%, LOD2 about 20%, impostor about 2 to 8 triangles. Confirm per engine with measurements on an RTX 3080 and an M1 Pro.

### Gaps
- No primary figures for trees per frame or foliage ms in a shipped title (Witcher 3, AC, HZD).
- Brucks' recommended frame counts and atlas sizes were not read first-hand.
- No Epic-official performance numbers for Nanite Foliage.

## Forest floor and small scatter (rocks, sticks, leaves, roots, debris)

### Takeaway
Density is held without draw-call or memory blowups by GPU instancing with indirect draws, cluster and HiZ culling, and procedural (non-stored) generation for the smallest layers. Tiny scatter (pebbles, single grass cards) does not benefit from Nanite-class geometry.

### Cited findings
- GPU-driven culling and indirect draws (AC Unity, SIGGRAPH 2015; Frostbite, GDC 2016) are the basis for large instance counts. A 10,000-tree forest renders "in milliseconds", and instance lists of a few hundred thousand are sorted on the GPU with radix sort — [Cinevva, 2026](https://app.cinevva.com/blog/2026-05-11-foliage-overdraw) (secondary, illustrative).
- Guidance from a UE 5.7 open-world blog: do not Nanite everything. A single-plane grass card and a tiny pebble do not benefit. Dense Nanite foliage scenes have a different VRAM profile than HISM (hierarchical instanced static meshes) — [StraySpark blog, 2025/26](https://www.strayspark.studio/blog/ue5-nanite-foliage-procedural-placement-performance) (blog, unverified).
- HZD placed its 500+ asset types, including small scatter, through the same density system — [Guerrilla, 2017](https://www.guerrilla-games.com/media/News/Files/GDC2017_VanMuijden_GPUBasedProceduralPlacementInHorizonZeroDawn.pdf).

### Inferences
- Treat leaves, needles and twigs mainly as **terrain material and decal layers** (a leaf-litter albedo/normal blend, projected decals at path edges), and add mesh scatter only near the camera. Small instances are cheap per draw but expensive in count and shadows. Disable shadow casting for anything below knee height **[inference, matches the VSM advice below]**.
- Roots, logs and large rocks are hand-reviewed "medium" scatter that alters silhouettes. Place them by rule but freeze and commit them, since they affect navigation and collision. Grass and pebbles stay procedural and collision-free.

### Gaps
- No sourced per-m² counts for forest-floor scatter in shipped games, and no mesh-decal cost numbers.

## Wind

### Takeaway
The standard is a global wind field (direction plus scrolled noise for gusts) sampled per instance with a phase offset. It is applied hierarchically: trunk sway, then branch, then leaf flutter (SpeedTree-style, GPU Gems 3), as vertex animation. Its main hidden cost in modern renderers is shadow-cache invalidation and wider culling bounds, not ALU.

### Cited findings
- Tsushima wind: a 2D Perlin noise texture scrolled along the wind direction does most of the grass animation. CPU and GPU sample the same field, so gameplay can read wind cheaply — [StylizedStation thread](https://x.com/StylizedStation/status/1688939450317807617?lang=en), [Jair Santana](https://jairsantana.com/posts/grass/). Tsushima also had a separate wind talk at GDC 2021, "Blowing from the West" — [GDC Vault](https://gdcvault.com/play/1027350/Blowing-from-the-West-Simulating).
- Far Cry 5 oriented grass assets on a wind vector map — [80.lv, 2018](https://80.lv/articles/houdini-procedural-world-generation-of-far-cry-5/).
- Animated vertex offsets (WPO) invalidate cached virtual shadow map pages. Epic's API exposes "Rigid" and "Static" invalidation behaviour to suppress WPO-driven invalidations — [Epic API doc](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Engine/EShadowCacheInvalidationBehavior).
- Community mitigations, with a version-dependent caveat:
  - Disable WPO in shadows for knee/waist-high plants.
  - Set a WPO disable distance.
  - Bound the maximum displacement so culling and invalidation bounds are correct.
  - Turning the VSM cache off cost one user about 10 to 15 fps.
  - Sources: [UE forum](https://forums.unrealengine.com/t/vsm-with-nanite-foliage-wpo/776406), [StraySpark VSM 5.7](https://www.strayspark.studio/blog/virtual-shadow-map-optimization-open-worlds-ue5-7), [UE forum 5.7.0 invalidation](https://forums.unrealengine.com/t/constant-invalidation-of-vsm-dynamic-shadows-in-5-7-0/2686534).
- A cost illustration of "3 ms forest vs 9 ms forest" depending on wind and culling choices is illustrative only — [Cinevva, 2026](https://app.cinevva.com/blog/2026-05-11-foliage-overdraw).

### Inferences
- The engine already has virtual shadow maps, so wind design must include a **distance cutoff** (static beyond N metres) and a **shadow-pass simplification** (trunk sway only, or none, in the shadow pass). Without them, a windy forest invalidates most VSM pages every frame.
- Keep one wind-field texture shared by grass, trees, cloth, particles and audio, so gusts read as one weather event.

### Gaps
- No sourced per-vertex ALU cost of hierarchical wind. I did not read SpeedTree's wind documentation or GPU Gems 3 directly.

## Lighting vegetation: TAA, shadows, ray tracing, AO

### Takeaway
Thin alpha-tested geometry is the hardest case for TAA (shimmer, ghosting), virtual shadow maps (invalidation and page thrash with wind) and ray tracing (any-hit shaders for alpha are slow). Remedies include hashed alpha, motion vectors that include wind, opaque or modelled leaves where affordable, and voxel or simplified proxies for distant lighting and shadows.

### Cited findings
- Hashed alpha testing (I3D 2017) reduces the alpha shimmer and dissolve that TAA otherwise fights. Bayer 4×4 or 8×8 dithering is the cheaper variant — [Cinevva, 2026](https://app.cinevva.com/blog/2026-05-11-foliage-overdraw).
- UE 5.7 Nanite Voxels improve far-distance shadows and lighting of trees — [80.lv, 2025](https://80.lv/articles/get-a-glimpse-of-nanite-foliage-with-voxel-representation-in-ue5-7).
- VSM and foliage: shadow-depth cost spikes with Nanite/VSM foliage, flicker and page thrash with WPO, and the cache-disable cost above — [UE forum 5.3](https://forums.unrealengine.com/t/shadowdepth-tanking-fps-with-nanite-vsm-foliage-ue-5-3/2072806), [bugnet VSM flicker](https://bugnet.io/blog/fix-unreal-virtual-shadow-maps-flicker-with-foliage).
- UE 5.6 regression report: WPO foliage stopped casting correct shadows; it worked in 5.5 — [UE forum, 2025](https://forums.unrealengine.com/t/unreal-engine-5-6-nanite-trees-painted-with-foliage-tool-dont-cast-shadows-when-wpo-is-enabled/2596402/3).

### Inferences
- Under the engine's optional ray-traced GI, trace foliage against **opaque proxies**: impostor cards or a coarse leaf-volume mesh with a transmission term, not alpha-tested leaves. Alpha any-hit is the usual RT foliage bottleneck **[background knowledge, unverified here]**.
- Motion vectors must include the wind displacement (current and previous frame), or TAA will smear leaves. The engine already has motion vectors and a reactive mask, so foliage should write both.
- Grass AO: blade-base darkening (a gradient along the blade) plus terrain-normal or bent-normal shading, so a field reads as one surface **[common practice, not sourced here]**.

### Gaps
- No primary numbers for foliage RT cost (any-hit vs opaque) or for bent-normal grass. I did not read the Frostbite or Lumen foliage sections.

## Real-world reference: temperate forest and meadow

### Takeaway
Closed late-successional broadleaf canopies such as beech let only about 1 to 3% (up to 6 to 9% in some studies) of full light reach the floor. That is why an old forest reads dark and has sparse undergrowth except in gaps. Old-growth beech has about 40 large (>60 cm DBH) trees per hectare, while regeneration can number tens of thousands per hectare. Meadow tiller density ranges from a few hundred to several thousand per m².

### Cited findings
- PAR (photosynthetically active light) transmittance to the floor under closed canopies (openness <10%) falls from about 15% under early-successional species to **1 to 3%** under late-successional species, including *Fagus sylvatica* (German study) — [BMC Ecology, 2020](https://bmcecol.biomedcentral.com/track/pdf/10.1186/s12898-020-00311-9).
- A Romanian beech stand had 9% under the canopy and 6 to 7% at ground level relative to open land — [MDPI Forests, 2019](https://www.mdpi.com/1999-4907/10/10/925/htm).
- Old-growth beech had about **40 stems/ha above 60 cm DBH**, 57% of canopy dominants. Natural regeneration can reach tens of thousands to over 100,000 individuals/ha; planted stands start with a few thousand — [MDPI Forests 16:557, 2025](https://mdpi-res.com/d_attachment/forests/forests-16-00557/article_deploy/forests-16-00557.pdf) (via search extract).
- Across 192 European temperate forest plots, canopy cover was the best predictor of understorey light indicators. In unmanaged beech reserves, small natural gaps control light to the understorey, and persistent gaps (>10 years) dominate understorey composition — [search extract, 2025/26 sources](https://pubblicazioni.unicam.it/retrieve/a6686e00-8406-467b-baee-2ddc4d35941a/De%20Benedictis%20et%20al%202026%20-%20Forest%20structure%20and%20understory%20funct%20div%20at%20multiple%20scales.pdf).
- Grass tiller density:
  - Intensive ryegrass pasture: 2,000 to 4,500 tillers/m², plus 1,500 to 1,700 of *Poa annua*.
  - Other swards: about 228 to 390/m².
  - Density falls as tillers get larger.
  - Sources: [UKnowledge IGC paper](https://uknowledge.uky.edu/igc/20/themeC/79), [HESS 2018 table](https://hess.copernicus.org/articles/22/2449/2018/hess-22-2449-2018-t03.pdf).

### Inferences
- A "dark old forest" comes from canopy closure (light 1 to 5% on the floor), not from undergrowth density. The floor should be mostly leaf litter, moss, ferns and seedlings, with dense undergrowth only in gaps and at edges. This is cheaper to render and keeps sight lines open.
- Visual blade density in games is far below real tiller counts. Thousands per m² are not needed; clumping and cards fake perceived density. **[Unverified]** practical game densities are tens to low hundreds of rendered blades per m² near the camera.

### Gaps
- No sourced total trees per hectare for a mature managed central-European stand. Background knowledge **[unverified]**: mature beech or spruce stands have roughly 200 to 500 stems/ha above 7 cm DBH; mixed oak-hornbeam is similar.
- No sourced undergrowth cover percentages, and no sourced species-silhouette guide. Typical central-European reading species, from background knowledge: beech, oak, spruce, fir, pine, birch, alder in swamps, hazel and elder shrubs.
- No measured light levels for forest edges or clearings.

## Free sources of vegetation assets and tools (2026)

### Takeaway
New in November 2026: Epic made the whole Quixel Megascans library (about 23,000 scans, including Megaplants) free on Fab. Licence questions remain, because Fab terms still apply and the project's earlier EULA concern keeps scans out of open repositories. Fully open options are Poly Haven (CC0), Blender's Sapling (GPL, bundled) and EZ-Tree (MIT, scriptable, LOD export).

### Cited findings
- Quixel Megascans:
  - Quixel announced on 5 Nov 2026 that the full Megascans library is free on Fab: about 23,000 3D scanned assets, with listings updated through 12 Nov.
  - Terms were updated so Megascans and Megaplants can be used for AI training.
  - Buyers since 1 Jan 2025 get refunds.
  - The article does not state engine restrictions or redistribution rights, and Quixel does not say how long it stays free.
  - Sources: [80.lv, 2026-11-06](https://80.lv/articles/entire-quixel-megascans-library-now-available-for-free/), [Creative Bloq, 2026](https://creativebloq.com/news/the-worlds-biggest-library-of-scanned-assets-for-3d-artists-is-now-free-and-it-seems-that-this-time-its-for-good).
  - Earlier history: free to all only until end of 2024, then paid on Fab from 2025 — [CG Channel, 2024](https://www.cgchannel.com/2024/10/epic-games-has-made-megascans-free-to-all-but-only-until-the-end-of-2024/), [UE forum, March 2025](https://forums.unrealengine.com/t/megascans-update-march-2025/2419561).
- Blender Sapling Tree Gen:
  - Bundled with Blender, GPL. It implements Weber and Penn's "Creation and Rendering of Realistic Trees".
  - Since Blender 4.2 it is an Extension.
  - It has not been updated in years (third-party opinion). An improved community fork exists.
  - Sources: [Blender manual](https://docs.blender.org/manual/en/4.1/addons/add_curve/sapling.html), [improved-sapling fork](https://www.github.com/abpy/improved-sapling-tree-generator).
- EZ-Tree (Dan Greenheck):
  - MIT-licensed, Three.js-based, 50+ parameters (trunk, branches, leaves, bark), seeded and scriptable through npm (`@dgreenheck/ez-tree`).
  - `generateLODs()` builds multiple LODs. A browser app exports GLB/PNG.
  - Leaf and bark texture licences should be checked separately.
  - Sources: [GitHub](https://github.com/dgreenheck/ez-tree), [CG Channel, 2025](https://www.cgchannel.com/2025/04/ez-tree-is-a-lightweight-free-open-source-tree-generator), [Digital Production, Jan 2026](https://digitalproduction.com/2026/01/13/free-browser-based-procedural-tree-generator-released/).

### Inferences
- Megascans' new free status makes scanned plants and ground scatter tempting. The project's earlier finding (scans kept out of git and open packs under the Fab EULA) should be re-checked against the November 2026 terms before any use. Megaplants are UE-oriented assets.
- For an open, scriptable pipeline:
  - EZ-Tree (seeded, headless in Node) or Sapling, scripted from Blender Python, for branch structure.
  - Leaf cards baked in Blender from CC0 leaf scans (Poly Haven / ambientCG).
  - Impostors baked by own code.

### Gaps
- Poly Haven and ambientCG plant model and texture counts were not checked. The quality of Megaplants vs Poly Haven was not compared.
- Not researched: botaniq licence terms, Blender tree-gen add-ons (the "tree-gen" add-on by friggog, MTree), Blender 4.x/5.x geometry-node tree systems, and The Grove's current price.

## Pitfalls reported by teams

### Takeaway
Recurring failures:
- overdraw from alpha cards;
- shimmer and TAA instability on thin geometry;
- LOD and impostor popping;
- shadow-cache invalidation from wind;
- memory growth from dense instance data or Nanite foliage;
- immature engine features (Nanite Foliage is experimental, with regressions between UE versions);
- loss of art direction and visibility when density is maxed.

### Cited findings
- Overdraw of 8 to 15× in naive forests; masked foliage defeats early-Z — [Cinevva, 2026](https://app.cinevva.com/blog/2026-05-11-foliage-overdraw).
- Impostor popping and blur when there are too few view angles — [three.js forest demo](https://discourse.threejs.org/t/a-forest-of-octahedral-impostors/85735).
- Grass LOD popping, handled in Tsushima by vertex morphing between the 15 and 7 vertex LODs (secondary) — [tigerabrodi](https://tigerabrodi.blog/grass-in-ghost-of-tsushima).
- VSM invalidation and flicker from WPO foliage, plus version regressions (5.6) — [UE forum](https://forums.unrealengine.com/t/wpo-causes-shadow-glitches-when-rendering-with-vsm-using-caching-default/2707254), [UE 5.6 report](https://forums.unrealengine.com/t/unreal-engine-5-6-nanite-trees-painted-with-foliage-tool-dont-cast-shadows-when-wpo-is-enabled/2596402/3).
- Nanite Foliage, MegaLights and the Procedural Vegetation Editor are explicitly unfinished in 5.7. Voxel splits make holes — [Digital Production, 2025](https://digitalproduction.com/2025/11/12/unreal-engine-5-7-foliage-pcg-and-in-editor-ai/), [UE forum](https://forums.unrealengine.com/t/nanite-voxelization-holes-in-the-mesh/2723578).
- Tsushima's talk framed the challenge as art direction: making thousands of blades look like a natural field "within memory and performance limits" — [GDC Vault, 2021](https://gdcvault.com/play/1027214/Advanced-Graphics-Summit-Procedural-Grass).

### Inferences
- For the remaster, the biggest non-technical risk is **mood and gameplay visibility**. Dense undergrowth hides enemies and paths and flattens the contrast between dark forest and open farmland. Clearings, path and sight-line masks, plus per-area density caps reviewed against original screenshots, should be gates in the pipeline.
- The M1 Pro preset should reduce grass ring radius and blade density and drop to impostors earlier. Turning shadows off for grass and small scatter is more effective than lowering tree LOD0 **[inference]**.

### Gaps
- I found no post-mortem that quantifies authoring time for vegetation in a remaster, and no measured popping or visibility metrics.

# Terrain materials, cliffs and mesh–terrain joins in open-world games (state as of November 2026)

Source-strength legend: **[F]** fetched and read this session; **[S]** search-result summary only, the primary source was not opened; **[M]** from prior knowledge of a well-known paper or talk, not re-fetched this session. Treat [M] numbers as "verify before relying on them".

The strongest primary source found is Stephane Etienne (High Moon / Activision), "Large Scale Terrain Rendering in Call of Duty", SIGGRAPH Advances 2023. It covers most of the questions below with shipped numbers, and I read its full speaker notes. URL: https://advances.realtimerendering.com/s2023/Etienne(ATVI)-Large%20Scale%20Terrain%20Rendering%20with%20notes%20(Advances%202023).pdf (cited below as **ATVI 2023**).

## Layer blending: splat maps, per-vertex weights, material IDs, height blending, layers per pixel, paths and roads

### Takeaway
Shipped engines moved from one alpha mask per layer (memory-heavy, unbounded shader cost) to **material-ID schemes with a bounded number of layers per pixel**: index map plus tint, about 4 samples, blended by height or "reveal" maps. Roads and paths are **spline decals baked into a virtual texture**, not terrain layers. Hand-authored transition textures work but cost memory, and newer systems generate transitions at runtime from two materials plus a threshold.

### Cited Findings
- Call of Duty first used one alpha mask per terrain layer, capped at 32 layers. That was "a huge amount of memory", and shader cost was unpredictable: nothing stopped all 32 layers shading one pixel. — [ATVI 2023](https://advances.realtimerendering.com/s2023/Etienne%28ATVI%29-Large%20Scale%20Terrain%20Rendering%20with%20notes%20%28Advances%202023%29.pdf) [F]
- Black Ops Cold War (Treyarch, GDC 2021 "Boots on the Ground") replaced the masks with **One Material Per Vertex (OMPV)**: an index map at 1 byte per texel (the most prominent layer), a BC1 colour-tint map, up to 256 layers, and bounded GPU cost. — ATVI 2023 [F]
- OMPV shading gathers the index and colour at the 4 surrounding vertices and weights them by distance. Duplicate layers are merged, so the common case samples one material once. Weights are multiplied by 2 and clamped, then blended through artist-authored **reveal maps** (height-like masks). The lowest index is the primary layer and is never blended. — ATVI 2023 [F]
- OMPV pitfalls:
  - A pixelated, grid-shaped look, fixed by distorting the UVs with a global 256×256 noise map (amplitude and frequency set per level).
  - Sharp transitions between materials. These were first fixed with hand-made **transition materials**, which "adds pressure to our texture streaming system and consumes precious memory".
  - The newer fix is **Multi-Layered Terrain Materials**: two materials plus a threshold, blended at runtime through the target's reveal map. It costs no new textures, but the shader is more expensive in transitions only. — ATVI 2023 [F]
- Roads in Call of Duty are **splined decals**: top-down orthographic projections that follow a Bézier curve. There are also rectangular "terrain decals" and "volume decals" that work on any geometry. All three are composited into the virtual texture after the layers. — ATVI 2023 [F]
- Witcher 3 (REDengine 3, GDC 2014, Gollent): artists paint two textures per point, a background (for example rock) and an overlay (for example grass or snow). The overlay's coverage follows **slope thresholds** with a sharpness ramp. Normals are combined through partial derivatives with damping, and colours and normals are interpolated by verticality. The author admits the team "was far from" all its texturing goals. — [GDC Vault](https://www.gdcvault.com/play/1020394/Landscape-Creation-and-Rendering-in), [Scribd slide copy](https://es.scribd.com/document/369764097/Gollent-Marcin-Landscape-Creation-And) [S]
- Frostbite (SIGGRAPH 2007, Andersson) used procedural shader splatting driven by height, slope and world normal in graph-based terrain shaders. — [Andersson 2007 course notes](https://www.advances.realtimerendering.com/s2007/Andersson-TerrainRendering(Siggraph07)-CourseNotes.pdf) [S]
- Ghost of Tsushima: per-tile textures hold height, terrain material and grass parameters (type, height), and the material map also drives grass type. — [GDC 2021 session listing](https://gdcvault.com/play/1027214/contactUs) and secondary summaries [S]
- Unreal Landscape (general knowledge): weightmaps are packed 4 layers per RGBA texture per component. Docs and the community advise keeping roughly 3–4 painted layers per component for cost, because every layer painted in a component is evaluated for the whole component. Unity's built-in terrain draws 4 layers per pass and adds a pass for each further 4. — [M], not re-verified this session
- Height-based blending (the reveal-map idea above) is the standard way to make transitions look natural: the material with the higher height-plus-weight wins, with a small blend depth. — ATVI 2023 [F] for the principle; the exact formula is common practice [M]

### Inferences
- The engine's current state (one material per triangle, hard polygon edges, baked transition textures) is close to OMPV's "sharp transitions plus hand-authored transition materials" stage. The direct upgrade is per-vertex or low-resolution **material index + weight data with height-blend masks**, capped at about 4 materials per pixel. Because the world is a hand-modelled mesh, per-vertex IDs and weights fit naturally: they can be painted in Blender as vertex colour or attributes.
- Paths should become spline or decal geometry blended with height, not more terrain layers or transition textures.

### Gaps
- No primary numbers found for per-layer shader cost in ms (for example "4 layers = X ms at 1440p"). The cost depends on the sample count: each layer costs albedo, normal and ORM (3 samples), or 6 with macro UVs and 12 with 4-tap hex tiling.
- The GDC 2021 "Boots on the Ground" talk itself was not read.

## Virtual texturing for terrain (UE RVT, Far Cry 4 AVT, megatexture, Decima)

### Takeaway
Virtual texturing turns an expensive many-layer terrain shader into a **cached, composited page**: pay once per page, then sample once per pixel. It also lets meshes, decals and roads read the same terrain colour, which is what makes mesh–terrain blending and grass tinting work. Its costs are page-composition time, cache churn on fast camera moves, extra memory for uncompressed scratch buffers, and blur when pages are missing. Megatexture (unique authored texture everywhere) lost because of texel density, disk size and authoring cost; procedural or adaptive VT won.

### Cited Findings
- **Far Cry 4 AVT** (Ka Chen, GDC 2015) — [GDC Vault](https://gdcvault.com/play/1021761/Adaptive-Virtual-Texture-Rendering-in), [GAME Watch report, 2015](https://game.watch.impress.co.jp/docs/news/20150308_691757.html) [F, secondary report]
  - Scale: a 10×10 km world reaches **10 texels/cm (1000 px/m)**, which the talk says is 20× classic VT.
  - Size: a classic 512K×512K virtual texture gives only 0.5 texels/cm over 10 km.
  - Layout: 64 m sectors, each with its own virtual image sized by camera distance (up to 64K×64K near the camera, down to 1K×1K far away).
  - Cache: the physical cache is about 9K×9K, in 264×264 pages with a 4 px border.
  - Filtering: 8× anisotropic, with software trilinear because hardware trilinear would cost about 25% more memory.
  - Content: grass and flower decals are baked into pages procedurally.
  - Pitfalls: cache thrashing in vehicles, high per-frame composition cost (fixed by spreading it over frames, low mips first), visible up- and down-scaling as sectors resize, and colour bleed between pages under anisotropic filtering.
- **Call of Duty AVT** (2023) — ATVI 2023 [F]
  - Feedback: the GPU writes a 240×136 feedback buffer, which the CPU reads back. It never requests more than 1/3 of the cache pages per frame.
  - Composition budget: 4 pages per frame on low-end mobile, up to 20 per frame on high-end PC. Pages unused for 3 frames can be recycled.
  - Composition order: layers in a compute shader, then splined decals, terrain decals and volume decals, written to 3 uncompressed scratch G-buffers (albedo, normal, metal/AO/gloss).
  - Compression: compute shaders compress pages to 2× BC3 or 3× BC1. The 3× BC1 layout saves a third of the memory, and the author "never" saw a quality difference. Compression is "where we see the greatest loss in visual quality".
  - Physical pages: 248 px usable plus a 4 px border, 2 mips.
  - Quality presets: physical cache from 13×13 to 42×42 pages, indirection 512² or 1024².
  - Offline baking: the top 6 mip levels are baked offline. This cuts GPU load and streaming of far terrain.
  - Pitfalls:
    - A 180° camera turn shows low-resolution VT for a few frames. Fixed by always requesting a medium page per sector.
    - Camera teleports. Fixed by pre-streaming at the target.
    - On slow storage the camera "outruns the VT" and terrain "looks like a PS1 game". Fixed by compositing immediately as low-res, then recompositing after 15, then 30 frames.
    - "Vista UVs" (distance-faded macro detail) conflict with VT, which has no camera. Mip 0 had to be mapped to about 5 ft of viewing distance, causing discontinuities between mips.
- **Unreal Runtime Virtual Texture**
  - The landscape material writes to an RVT. Non-landscape actors read the same RVT to blend with the landscape. RVT is recommended for landscape splines, mesh decals and object blending.
  - Normals should be stored in **world space** so that the primitives writing and reading the RVT blend well.
  - UE also has Streaming VT for authored textures.
  - Sources: [Epic VT docs](https://dev.epicgames.com/documentation/en-us/unreal-engine/virtual-texturing-in-unreal-engine), [RVT quick start](https://dev.epicgames.com/documentation/unreal-engine/runtimevirtual-texturing-quick-start-in-unreal-engine?lang=en-US) [S]
- id Tech 5 megatexture (Rage, 2011): one unique virtual texture, with known complaints about blurry texel density, very large install size and a long authoring and bake cycle. id Tech 6 and 7 abandoned it. — [M], no source opened this session
- Decima (Horizon Zero Dawn, GDC 2017): world data is a set of streamed, generated-and-paintable 2D maps at about **4 MB/km²**. Procedural placement runs on the GPU for 500+ asset types and 100,000+ objects at about **250 µs** average GPU load. Ecotopes drive asset types, distribution and colourisation. — [Guerrilla GDC 2017 slides](https://www.guerrilla-games.com/media/News/Files/GDC2017_VanMuijden_GPUBasedProceduralPlacementInHorizonZeroDawn.pdf) [S]. No terrain-texturing detail was found.

### Inferences
- For a 2002-scale world (Khorinis is a few km², not 100 km²), full AVT may be more than needed. A simpler **clipmap-style or fixed-grid RVT around the camera** (UE-style) would provide the main benefits: a cheap ground shader, a mesh-blend source and grass tint. Because the world is an arbitrary mesh, a top-down RVT covers only the ground that faces upward; walls, cliffs and caves need direct shading.
- The engine already streams by on-screen size under a budget, so VT page loads would join the same budget.

### Gaps
- Decima's terrain shading itself, and anything published about Decima VT, was not found.
- No primary measurement of UE RVT page-update cost in ms was found.

## Hiding repetition: macro variation, distance tiling, stochastic and hex tiling, bombing

### Takeaway
The shipped workhorses are cheap: **per-vertex or per-cell random rotation** (CoD "tile hiding"), **a second, larger-scale sample of the same texture** (macro or "vista" UVs) mixed in with distance, and a **low-frequency colour-tint map** (CoD's colour map). Mikkelsen's hex tiling is the practical stochastic method. It takes 3 taps per texture and works best on normal maps; on colour it loses some contrast.

### Cited Findings
- CoD "tile hiding": a random rotation per vertex from world position, 16 discrete rotations. It is "very effective". The cost is that duplicate layers can no longer be merged, so a pixel samples grass 4 times instead of once. "Still the visual quality improvements are well worth it." — ATVI 2023 [F]
- CoD "Vista UVs": a second, scaled-down UV set samples albedo and normal again, lerped by camera distance. This prevents far terrain collapsing to one flat colour, and works well on rocks. — ATVI 2023 [F]
- CoD colour map: a per-terrain BC1 tint, and also a "mesh tint" in which cliffs sample a very low-resolution VT so that repeated meshes look unique. — ATVI 2023 [F]
- Mikkelsen, "Practical Real-Time Hex-Tiling", JCGT 11(2), 2022:
  - It adapts Heitz–Neyret by-example noise and samples the source texture directly, so it drops into any shader, with no histogram transform.
  - It allows random rotation per hex.
  - Normal maps look "very similar" to the original. On colour there is "a trade-off in quality … acceptable".
  - Reference code: [github.com/mmikk/hextile-demo](https://github.com/mmikk/hextile-demo). Ports exist for Godot and UE.
  - Source: [JCGT paper](https://jcgt.org/published/0011/03/05/paper.pdf) [S]
- Heitz & Neyret, "High-Performance By-Example Noise using a Histogram-Preserving Blending Operator", HPG 2018: 3 taps on a triangle grid plus a Gaussianising histogram transform (a precomputed LUT and inverse LUT). Deliot & Heitz, "Procedural Stochastic Textures by Tiling and Blending", GPU Zen 2, 2019, is the simplified real-time version Unity shipped as a sample. Costs: 3 taps per texture, sampled with gradients (textureGrad). Artefacts: blurring or contrast loss on structured textures (bricks, cobbles), and seams if gradients are not passed. — [M]
- Classic texture bombing (GPU Gems, 2004) and per-tile random offset (Quilez "texture repetition" article): cheap, but break structured patterns. — [M]

### Inferences
- Gothic ground textures are mostly stochastic (grass, dirt, forest litter, mud), which is the case where hex or stochastic tiling works well. Cobbles and planks should not use it, or only for rotation with care.
- Order of cost-effectiveness: (1) a macro colour/tint map plus macro-scale normal (cheapest, biggest win at mid-distance), (2) random rotation per vertex or cell, (3) hex tiling on near-distance layers only, faded out with distance.

### Gaps
- No published per-pixel ms numbers for hex tiling in a shipped game. Expect about 3× the texture fetches of the layer it is applied to.

## Cliffs and steep slopes: triplanar or biplanar mapping, slope rules, rock meshes over terrain, seam hiding

### Takeaway
Cliffs in modern games are mostly **kitbashed rock meshes**, not stretched terrain. Seams are hidden by (a) **snapping vertices to the terrain** and (b) **blending the mesh's colour and normal toward the terrain VT near the contact**, using world-space normals and matching textures. Triplanar costs 3 samples per texture and biplanar 2. Slope-driven overlays (moss, grass and snow on top, rock on the sides) are the standard rule.

### Cited Findings
- CoD "skirts": mesh vertices morph to the terrain height within an artist-set range above and below. When shading, the mesh samples the terrain VT according to how close the pixel is to the terrain, so rocks "seamlessly blend". Grass clutter uses the same mechanism to snap to the ground. — ATVI 2023 [F]
- UE RVT mesh blending: the RVT stores world-space normals. The Megascans blog advises giving meshes a world-aligned texture set similar to the terrain's, and avoiding tangent-space normals at the join to prevent a lighting seam. Community practice is a height-based gradient (world height, then a height-lerp to the mesh material). — [Megascans blog](https://blog.megascans.se/?p=3293), [Epic forum](https://forums.unrealengine.com/t/megascans-materials-and-runtime-virtual-texture-rvt/774989/19), [Dowell 2025 blog](https://andrewdowell.blogspot.com/2025/02/how-to-blend-assets-with-your-landscape.html) [S]
- Mesh-to-mesh blending (rock on rock) is not solved by RVT. A forum user asked about it and got no definitive answer. — [Epic forum](https://forums.unrealengine.com/t/megascans-materials-and-runtime-virtual-texture-rvt/774989/19) [S]
- Biplanar mapping (Quilez): 2 fetches instead of 3 by choosing the 2 dominant axes, at the cost of some extra arithmetic. Derivatives must be computed manually. Small artefacts appear near (±1,±1,±1) normals. — [iquilezles.org/articles/biplanar](https://iquilezles.org/articles/biplanar) [S]
- Witcher 3: slope thresholds control the overlay over the background, with normals combined by verticality. — [Gollent GDC 2014](https://www.gdcvault.com/play/1020394/Landscape-Creation-and-Rendering-in) [S]
- Other seam methods: dithered or temporal (TAA) blending between mesh and terrain, pixel depth offset to soften intersections, and distance-field-based blending (UE "DistanceToNearestSurface" from global distance fields) to fade the mesh material near other surfaces. — [M]; well-known UE practice, not re-verified

### Inferences
- Gothic's world is one mesh, so "rock mesh over terrain" maps onto (1) existing rock surfaces in the world mesh, which need triplanar or biplanar shading where the authored UVs stretch, and (2) any new rock kitbash meshes, which need a blend toward the ground material within about 0.3–1 m of the contact. The engine lacks a terrain VT. A cheaper substitute is to let the rock shader sample the **same ground material set by world XZ** near its base, weighted by height above the ground mesh or by a baked vertex attribute.
- World-space or triplanar normals on both sides of the join avoid the tangent-space lighting seam.

### Gaps
- No primary shipped numbers for triplanar cost in ms. Analytically: 3× fetches per texture, so for 3 maps (albedo, normal, ORM) that is 9 fetches per layer.

## Small-scale relief: POM, tessellation and displacement, pixel depth offset

### Takeaway
Shipped terrain uses **tessellation or displacement sparingly and at low resolution** (CoD: 32×32 BC4 pages, no collision), and **parallax occlusion mapping** for specific surfaces (cobbles, ice, mud ruts). In 2026 UE5, Nanite tessellation of landscapes is available but still buggy. POM breaks at silhouettes and grazing angles and does not cast shadows unless extra work is done.

### Cited Findings
- CoD displacement:
  - Stored in VT as 32×32 BC4 pages with 1 mip and a 1 px border, because vertex density is far below pixel density.
  - Displacement does not affect collision, "which means they cannot be too big".
  - Enabled only on platforms with GPU tessellation.
  - For ice, a "cheap parallax mapping technique" is used.
  - Source: ATVI 2023 [F]
- UE5 Nanite tessellation (5.3+) applies runtime displacement maps to Nanite meshes and landscapes. Open bugs as of 2026:
  - UE-367789: displacement goes the wrong way with landscape Z scale, still present in 5.8.
  - A 5.8 regression versus 5.7.
  - Users report weaker distance simplification than precomputed LOD.
  - Sources: [Epic forum UE-367789](https://forums.unrealengine.com/t/nanite-tessellation-pushes-displacement-in-the-wrong-direction-on-landscapes-with-z-scale-ue-367789-still-in-5-8/2845653), [forum 5.8 regression](https://forums.unrealengine.com/t/nanite-displacement-bugged-in-exact-same-landscape-in-ue-5-8-but-not-in-ue-5-7/2739815), [density thread](https://forums.unrealengine.com/t/decrease-tesselation-triangle-density-based-on-distance-screensize-with-nanite/2003507) [S]
- POM: typical cost is 8–32 height taps per pixel. There are no silhouettes. Self-shadowing needs a second ray march. Pixel depth offset writes the displaced depth, so that contacts with other geometry and decals intersect correctly. POM is unstable under TAA at grazing angles unless the step count scales with angle. — [M]

### Inferences
- For Gothic on Metal (M1 Pro): hardware tessellation is limited on Metal (it uses a different tessellation model and MoltenVK support is partial) — [M], verify. A **pre-tessellated mesh with vertex displacement from a height page**, or POM with a distance fade (for example within 10–15 m), is the portable path.
- Use POM for cobbled roads and city paving, and possibly mud ruts. Forest floor and grass get more from normal, AO and scatter meshes (litter, stones, roots) than from POM.

### Gaps
- No shipped game numbers for POM step counts or ms found this session.

## Weather-driven world-space layers: wetness, puddles, moss, snow; decal types

### Takeaway
The reference for physically based wetness is Lagarde's 2012–2013 "Water drop" series (Remember Me). Porous surfaces darken albedo and lower roughness. Puddles form in low spots, found from a height map or vertex data. Rain ripples are a normal-map animation scaled by intensity. Moss and snow are slope- and world-up-driven overlays. Decals: projected deferred decals for dynamic or large ones, mesh decals for authored edges. CoD composites decals into VT pages, so their per-frame cost is zero after composition.

### Cited Findings
- Lagarde's series (Observe rainy world, Dynamic rain and its effects, Physically based wet surfaces), started 2012–2013 for Remember Me. Its findings:
  - Water collects in low spots first, which breaks up puddles.
  - Ripple count scales with rain intensity.
  - Wet porous materials darken, and wet surfaces become glossier.
  - Sources: [3DVF summary](https://3dvf.com/actualite-5676-sebastien-lagarde-simulation-pluie-en-jeu-video-html/), [fxguide wet environments](https://www.fxguide.com/?p=43362) [S]
- Tatarchuk, SIGGRAPH 2006, covers artist-directable rain with puddles and wet reflections. — [course chapter](https://advances.realtimerendering.com/s2006/Chapter3-Artist-Directable_Real-Time_Rain_Rendering_in_City_Environments.pdf) [S]
- CoD has 3 decal kinds (rectangle, spline, volume). Volume decals work on any geometry, including non-terrain. Decals are baked into VT pages in a fixed order. — ATVI 2023 [F]
- Wetness rules from Lagarde's posts — [M], verify against the posts:
  - albedo darkens by roughly squaring it or by a porosity factor;
  - roughness goes toward about 0.1 in standing water;
  - normal maps flatten under standing water.

### Inferences
- In a deferred renderer, wetness can be a **G-buffer modifier pass**: read a world wetness mask (height, puddle mask, rain occlusion from above via a shadow-map-like top-down depth) and modify albedo and roughness before lighting. One pass covers ground, rock and meshes uniformly.

### Gaps
- No primary numbers for the cost of shipped weather layers were found.

## Texel density targets, resolution per layer, memory

### Takeaway
The best published figure is Far Cry 4's **1000 px/m (10 texels/cm)** near the camera through AVT. CoD maps VT mip 0 to about 5 ft (1.5 m) viewing distance. Tiling layers are typically 2K (sometimes 4K) textures repeating every 2–4 m, which is about 512–1024 px/m (estimate). Memory is bounded by the VT cache or by the streaming budget, not by the world size.

### Cited Findings
- Far Cry 4: 10 texels/cm (1000 px/m) near the camera, 64K² per 64 m sector at the nearest level. A classic 512K² VT over 10 km gives only 0.5 texels/cm (50 px/m). — [GAME Watch 2015](https://game.watch.impress.co.jp/docs/news/20150308_691757.html), [GDC Vault abstract](https://gdcvault.com/play/1021761/Adaptive-Virtual-Texture-Rendering-in) [F/S]
- CoD VT mip 0 is meant for at most about 5 ft of viewing distance, with each further mip doubling the distance. The VT physical cache runs from 13×13 to 42×42 pages of 256 px. At ultra, 42×256 = 10,752² texels per texture. With 2× BC3 (1 B/texel each) that is about 2×115 MB ≈ 230 MB; 3× BC1 is about 175 MB. These totals are my own arithmetic from the published page counts. — ATVI 2023 [F] for the inputs
- CoD Virtual Height Map: a typical unified height map is 32 MB (artists wanted 128 MB+). The virtualised cache is about 1.3 MB, and a 68×68 page takes about 1 ms to decompress on PS4. — ATVI 2023 [F]
- Horizon Zero Dawn world data (height and placement maps) is about 4 MB/km². — [Guerrilla 2017](https://www.guerrilla-games.com/media/News/Files/GDC2017_VanMuijden_GPUBasedProceduralPlacementInHorizonZeroDawn.pdf) [S]
- Common industry guidance for 1st/3rd-person games (unverified, [M]): 512 px/m for environment assets at 1080p, and 1024 px/m for hero ground near the camera at 1440p and above.

### Inferences
- At 1440p, 60° vertical FOV, with a camera about 1.7 m up, ground 2 m away covers roughly 1 screen pixel per 1–2 mm. Ground needs about 500–1000 px/m to avoid magnification within the first few metres. A 2K layer tiling every 2 m gives 1024 px/m (estimate).
- Ten ground layers × (2K albedo BC7 + 2K normal BC5 + 2K ORM BC7) is about 10 × 3 × 5.3 MB ≈ 160 MB with mips. That fits the 10 GB card easily, and M1 Pro too with a lower mip bias.

### Gaps
- No verified current-gen (2024–2026) game publicly states its ground px/m apart from FC4. Typical tiling size per layer is industry lore, not sourced here.

## Lighting: specular aliasing, normal-map filtering, AO from height, contact shadows

### Takeaway
Ground with high-frequency normal maps sparkles under TAA unless roughness is widened per mip (Toksvig, or LEAN/CLEAN) or a screen-space geometric specular AA is applied. Height or reveal maps double as AO and cavity. Contact shadows (a short screen-space ray march) ground small props, and virtual shadow maps help with larger ones.

### Cited Findings
- Toksvig, "Mipmapping Normal Maps" (2005): roughness grows with the shortened average normal length per mip. LEAN (Olano & Baker 2010) and CLEAN store second moments. Kaplanyan et al., "Filtering Distributions of Normals for Shading Antialiasing" (HPG 2016), and the Tokuyoshi–Kaplanyan geometric specular AA (2019/2021) give a cheap screen-space roughness widening used in UE and Unity HDRP. — [M], not fetched this session
- CoD composites "metalness, occlusion & gloss" into the VT along with the layers, so AO from layer heights is baked per page. — ATVI 2023 [F]
- UE "contact shadows" are a per-light screen-space ray march, length set per light. — [M]

### Inferences
- The cheapest robust fix is to **bake Toksvig into the roughness mips** when building BC textures (offline, zero runtime cost), plus optional geometric specular AA from the normal derivatives in the G-buffer pass.

### Gaps
- No shipped ms numbers were gathered.

## Arbitrary-mesh worlds and voxel/SDF terrain (caves, overhangs)

### Takeaway
Heightfield-only tricks (top-down RVT, slope from the heightmap, top-down projected splines) do not cover walls, overhangs or caves. Arbitrary-mesh and voxel worlds use **triplanar or biplanar projection with material IDs or weights per vertex**, chosen by world normal and procedural rules. Public technical detail on Enshrouded, No Man's Sky and Deep Rock Galactic texturing is thin.

### Cited Findings
- No Man's Sky (GDC 2017, McKendrick): pipeline voxel generation → polygonisation → texturing → population → simulation. No texturing specifics were found in sources this session. — [GDC Vault](https://gdcvault.com/play/1024265/Continuous-World-Generation-in-No) [S]
- Enshrouded (Keen "Holistic Engine"): no first-party technical write-up of terrain texturing was found. A 2026 article claims a hybrid of voxel terrain and Substance-textured assets; it is low-authority, so unverified. — [foro3d 2026](https://foro3d.com/en/2026/march/technical-analysis-of-the-holistic-engine-from-enshrouded.html) [S]
- General voxel practice: material IDs in a vertex attribute (for example 8 bits in Zylann's Godot voxel module), procedural rules in the shader where gameplay does not depend on them, and triplanar sampling. — [voxel-tools docs](https://voxel-tools.readthedocs.io/en/latest/smooth_terrain/) [S]
- Deep Rock Galactic: nothing found. — gap
- CoD "volume decals" work on arbitrary geometry, unlike top-down terrain decals. — ATVI 2023 [F]

### Inferences
- The Gothic world mesh with authored UVs is effectively what voxel games approximate. Keep UV-mapped sampling where UVs are good (they hold authored detail), add **per-vertex material ID and weight with height blending**, and fall back to **biplanar world projection** on steep or cave triangles. A top-down VT can serve ground that faces upward only; wall and ceiling triangles shade directly.

### Gaps
- Primary sources for Enshrouded, No Man's Sky and Deep Rock Galactic texturing were not found.

## Reference: how real ground looks at transitions (for acceptance criteria)

### Takeaway
Little was sourced here. The following is observational guidance from general knowledge, unverified, to be checked against photos.

### Cited Findings
- None fetched this session. Lagarde's rainy-world observations (water in low spots first, wet porous materials darker) are the one sourced item. — [fxguide](https://www.fxguide.com/?p=43362) [S]

### Inferences (unsourced observations, use as candidate acceptance checks)
- Path edges are never a straight line: there is a 0.2–1 m ragged band, grass thins and is flattened toward the path, and dirt shows between grass tufts. The worn centre is lower and smoother, with stones and roots exposed. Ruts and puddles sit in the low centre.
- Grass to rock: grass and soil collect in cracks and at the base, the rock is darker and damper at ground contact, and moss sits on tops and on the shaded (north) side. There is no visible line where the rock enters the ground.
- Forest floor: litter (needles, leaves) gathers against trunks and roots and thins on paths. Colour varies at metre scale, not only at texture-tile scale.
- Candidate acceptance tests:
  1. No straight transition edge longer than about 1 m visible from 5 m.
  2. No visible tile repeat in a 30 m wide frame.
  3. No lighting seam at mesh–ground contact.
  4. No sparkle on ground in motion under TAA.
  5. Judged on a moving clip, not a still.

### Gaps
- No photographic reference study was gathered. A follow-up should collect CC0 photo sets of path edges, rock bases and forest floor.

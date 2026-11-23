# Real-time grass geometry, density and distances in shipped games and open implementations

Summary. The primary Ghost of Tsushima talk was not readable (GDC Vault is behind a login limit; no free slides or transcript found), so every Tsushima number below is secondary or [unverified]. Verified primary data came from two documents I read: the Horizon Zero Dawn GDC 2018 slides (Guerrilla, PDF read in full text) and the Jahrmann/Wimmer I3D 2017 paper (PDF read). Further numbers come from the GPUOpen mesh-shader article, the Step Skyrim guides and the GodotGrass README plus a Godot LOD write-up. Nothing was found for Red Dead 2, Kingdom Come, BotW/TotK, Elden Ring, Assassin's Creed, Outerra grass, Unity Book of the Dead or the 2026 Gothic remake grass. Cards vs blades is only partly answered, and cliff-edge dressing was not found at all.

Working picture from what is confirmed: shipped games use cheap low-triangle grass units (Horizon: 20-36 triangles for the nearest grass LOD, then 10-18, 10-18), with thinning by distance while widening the survivors (Jahrmann, GPUOpen, Tsushima-secondhand). Per-blade GPU generation (Tsushima, GPUOpen, Jahrmann) uses roughly 6-9 triangles per blade at the highest quality in the open implementations.

## 1. Ghost of Tsushima (Wohllaib, GDC 2021)

### Takeaway
The primary talk could not be read; the only confirmed facts are its abstract and that blades are generated on the GPU per tile. All numbers (15/7 vertices, 83,000 blades, 2.5 ms, tile doubling) come from secondary sources and are [unverified].

### Cited Findings
- Official abstract: individual blades generated on the GPU, each with procedural appearance and animation; covers acres of grass within memory and performance limits, rendering and animating blades, making thousands of blades look like a natural field. Slides and transcript are members-only (login limit reached) — [GDC Vault](https://gdcvault.com/play/1027033/Advanced-Graphics-Summit-Procedural-Grass)
- Qualitative AI summary of the talk video (no numbers given): compute shaders place blades per tile with frustum and distance culling; blades are cubic Bezier curves; high and low LOD with a smooth transition; outward-tilted normals add roundness without extra vertices; blade type/height come from surrounding textures (terrain height, material, grass type and height); clumps via Voronoi drive height and orientation; wind from 2D Perlin noise plus sine bobbing; specular normals blend toward a clump normal with distance; shadows use imposters to approximate grass height plus screen-space shadows for fine detail, full shadow pipelines judged too expensive; distant tiles get artist-authored textures replacing the base material to avoid a monotone green expanse — [gist.ly summary](https://gist.ly/youtube-summarizer/procedural-grass-systems-in-ghost-of-tsushima-achieving-art-direction) (AI summary, may omit figures)
- 15 vertices per blade near and 7 far, ~83,000 blades at 2.5 ms, distant tiles double in size at constant blade count (about 3 of 4 blades dropped per step): [unverified]; the compiling article itself flags them unverified — [csarko.sh](https://csarko.sh/research/grass-and-trail-realism); vertex counts also appear in a fan thread (a recreation, not the developers) — [Stylized Station thread](https://threadreaderapp.com/thread/1688939411575119872); 83,000 / 2.5 ms also in [Hashnode article](https://hashnode.com/posts/what-we-can-learn-from-grass-in-ghost-of-tsushima-renders/69e130b2ffbb787634eb9ef4) which cites no source for it.
- Fan Godot recreation describes a tile LOD driven by density (blades along tile boundaries) plus a mesh, attributing the idea to the talk — [GodotGrass README](https://github.com/2Retr0/GodotGrass)

### Inferences
- The pattern worth copying (even if exact numbers are unknown): fixed-size tiles, blade positions generated in compute, distance step = bigger tile with fewer, wider blades, so apparent density is preserved.

### Gaps
- Tile size, blades per tile, draw distance, memory, GPU ms, shadow details and parameter ranges: not confirmed. Open next: the talk video (GDC YouTube) transcript, or Sucker Punch slides if they exist.

## 2. Horizon Zero Dawn / Forbidden West

### Takeaway
Horizon grass is mesh clumps with very small triangle counts per LOD (primary slides), animated in the vertex shader from a global wind field. No density or distance figures for grass were found.

### Cited Findings
- Grass has three LODs: LOD1 20-36 triangles with the high shader, LOD2 10-18 triangles high shader, LOD3 10-18 triangles low shader — [Sanders GDC 2018 slides](https://media.gdcvault.com/gdc2018/presentations/gilbert_sanders_between_tech_and.pdf) (read p.23)
- Plants (non-grass) have four LODs: ~2000, ~800, ~140 triangles, then 8-triangle billboard; trees five LODs from ~10,000 to a 12-triangle billboard — same slides, pp.77-78
- Alpha testing uses signed-distance alpha textures with a custom mip chain; small assets and grass use a 256x128 alpha texture "to fit in cache", requiring good anti-aliasing — same slides, pp.36, 43
- Distance LOD of animation: displacement is a percentage of animation / of object height based on camera distance — same slides, p.24+
- Shadow casting: four sun cascades (compartment 1024^2; cascade 0 2048^2 to 8 m; cascade 1 2048^2 to 80 m; distant height-field 1536^2 over 1.5 km), separate shadow-caster meshes using LOD3 visual mesh alpha-tested/animated for S1, fewer triangles non-alpha-tested for S2 — same slides, pp.80-83
- Summary lessons: "LOD up, not down", shadow casters separate — same slides, p.85
- Global wind field pass about 150 microseconds (all vegetation) — [csarko.sh](https://csarko.sh/research/grass-and-trail-realism) (secondary, cites the same deck)
- Placement is entirely procedural on GPU (van Muijden GDC 2017) — [Guerrilla PDF](https://www.guerrilla-games.com/media/News/Files/GDC2017_VanMuijden_GPUBasedProceduralPlacementInHorizonZeroDawn.pdf) (too large to fetch; only abstract-level confirmed via search)

### Inferences
- A Horizon-like grass unit is a short card/blade clump of ~20-36 triangles near, ~10-18 further; this is the budget of a clump-mesh approach.

### Gaps
- Cards per clump, atlas layout, density per m2, distances, fade, cost: not in the slide text extracted (text extraction of images missed diagrams). Guerrilla placement deck not read. Forbidden West: nothing found.

## 3. Other shipped references

### Takeaway
Hard data is thin. Witcher 3 grass distribution cost under 1 ms (secondary); no grass figures found for RDR2, KCD, BotW/TotK, Elden Ring, AC, Book of the Dead; UE 5.7 Nanite Foliage is experimental and guides advise cards for grass.

### Cited Findings
- Witcher 3: grass auto-distributed by terrain material, about ten types per level, each with own culling grid and draw distance, whole distribution under 1 ms (from the GDC 2014 Gollent talk, as relayed) — [csarko.sh](https://csarko.sh/research/grass-and-trail-realism) citing [Gollent GDC 2014](https://media.gdcvault.com/GDC2014/Presentations/Gollent_Marcin_Landscape_Creation_and.pdf) (I did not read the deck myself)
- UE 5.7 Nanite Foliage is experimental; single-plane grass cards are too simple to repay Nanite overhead, hybrid with traditional cards advised; voxel plus WPO shows mid-range artifacts; skeletal animation recommended for voxel grass — [UE forum thread](https://forums.unrealengine.com/t/5-7nanite-foliage-voxel-staticmesh-wpo/2690512), [StraySpark guide](https://www.strayspark.studio/blog/ue5-nanite-foliage-procedural-placement-performance) (blog guidance, low confidence)
- Gothic remake (Alkimia): Unreal Engine 5.4.3 with some backports from 5.7 (PSO precaching); vegetation noted as dense; foliage density among the heaviest settings; no grass technique published in what I found — [PCGH](https://www.pcgameshardware.de/Gothic-Remake-Spiel-73829/Specials/Release-Review-Prozessor-Grafikkarten-Benchmarks-Linux-1540130/2/) (via search summary, article not fully read)
- BotW: forum claim that every blade is a mesh, unconfirmed — [UE forum](https://forums.unrealengine.com/t/botw-grass-style/437647)

### Inferences
- If the Gothic remake ships standard UE5 foliage, its grass is likely instanced meshes/cards from the engine's foliage tools [unverified].

### Gaps
- RDR2, KCD1/2 (CryEngine merged meshes), BotW/TotK, Elden Ring, Assassin's Creed, Book of the Dead, Outerra: no primary data found within the tool budget.

## 4. Skyrim modding practice

### Takeaway
The community tunes three things: density (inverse knob), fade distance, and a precomputed grass cache; beyond full-3D range a separate "grass LOD" (DynDOLOD) draws a cheap far layer. Card sizes were not found.

### Cited Findings
- Full 3D grass renders only within the loaded grid radius (uGridsToLoad); grass LOD extends outward to LOD4 (set by fBlockLevel0Distance); grass LOD can cost as much as 20 fps; DynDOLOD grass mode 1 recommended; grass LOD density value 35 "often gives good results" — [STEP Grass LOD Guide](https://stepmodifications.org/wiki/SkyrimSE:Skyrim_SE_Grass_LOD_Guide)
- Grass cache (No Grass In Objects) keeps grass out of rocks/stairs and removes per-cell CPU generation cost; pregeneration 25 min to 1 hour — same guide
- OverwriteMinGrassSize works inversely to density (67 in the guide's step); GlobalGrassScale 1.15 makes grass taller allowing lower density; OverwriteGrassDistance=6144, OverwriteGrassFadeRange=14128 (game units) — same guide
- fGrassStartFadeDistance default 7000; fGrassMaxStartFadeDistance default 7000; fGrassMinStartFadeDistance default 400 — [STEP SkyrimPrefs/Grass](https://stepmodifications.org/wiki/SkyrimSE:SkyrimPrefs_INI/Grass)
- Folkvangr load order and density mod (Grass FPS Booster) mentioned; iMinGrassSize 20 default per one guide, 30-40 mid-spec — [Nexus listing via search](https://www.nexusmods.com/skyrimspecialedition/mods/48296/)
- Unit conversion (Skyrim ~70 units per metre, so 7000 units about 100 m) is common community knowledge, not from these pages: [unverified]

### Inferences
- The Skyrim lesson: separate full-grass radius from a cheaper far layer, precompute placement (cache), and let height scale substitute for density.

### Gaps
- Card dimensions, cards per clump, polygon counts of Folkvangr/Cathedral: Nexus pages returned 403; not obtained.

## 5. Open implementations

### Takeaway
Numbers exist for GPUOpen mesh-shader grass, Jahrmann/Wimmer, and a Godot tutorial; all are research or demo scale, not shipped.

### Cited Findings
- GPUOpen mesh-shader grass: max 32 blades per patch (256-vertex limit), 8 vertices and 6 triangles per blade, 128-thread group; blade count falls from 32 toward 2 with distance; surviving blades widen by maxBladeCount/bladeCount to hold density; last blade fractional width; blade width 0.03, lean 0.3; no timings reported — [GPUOpen](https://gpuopen.com/learn/mesh_shaders/mesh_shaders-procedural_grass_rendering/)
- Jahrmann & Wimmer I3D 2017: blades are Bezier curves with tessellation; per-blade culling by orientation, view frustum, distance (distance levels evenly spaced; level k culls k-1 of n blades), occlusion; width correction forces blades under about 1-3 pixels to quads; nature scene 397,881 blades: 123/129 FPS with culling vs 78 without, 43,128 blades drawn vs 168,333; helicopter scene 900,000 blades: 56 FPS (17.86 ms) vs 35 FPS without culling, 165,135 vs 503,382 drawn, rendering 5.47 ms vs 9.15; test GPU GTX 780M. The authors' 2016 poster reports up to 75% blades culled — [paper PDF](https://www.cg.tuwien.ac.at/research/publications/2017/JAHRMANN-2017-RRTG/JAHRMANN-2017-RRTG-draft.pdf), [2016 poster](https://www.cg.tuwien.ac.at/research/publications/2016/JAHRMANN-2016-IGR/JAHRMANN-2016-IGR-poster.pdf)
- Godot tutorial: 5x5 m chunk, 10,000 high-LOD blades per chunk (about 400 per m2), 9 triangles per blade (about 90,000 triangles per chunk), low LOD 1 triangle per blade, impostor fades in at 5-10 m, high LOD fades out at 10-20 m, impostor effect just under 2 ms — [hexaquo.at part 4](https://hexaquo.at/pages/grass-rendering-series-part-4-level-of-detail-tricks-for-infinite-plains-of-grass-in-godot/) (density derived by me from its numbers)
- GodotGrass (MIT licence): tile LOD by density and mesh (hard-coded high and low blade), CPU-computed positions, shadows on by default "at a very high performance cost" — [GodotGrass](https://github.com/2Retr0/GodotGrass)
- Godot MultiMesh chunking: frustum culling works only if the whole MultiMesh is out of view so chunks of about 10-30 m2; distance culling per chunk gives blocky edge — [Stansfield](https://leonstansfield.github.io/Posts/rendering_dense_foliage_in_godot/rendering_dense_foliage_in_godot.html)

### Inferences
- First numbers for a blade-based ring: ~6-9 triangles per near blade, thinning to 2/32 of blades with widening, chunks of 5-30 m, a near ring to about 10-20 m plus an impostor layer.

### Gaps
- Acerola's series, Outerra posts, Unity/Unreal community projects: not read. Licences of Vulkan ports of Jahrmann not checked.

## 6. Blades vs cards vs clump meshes

### Takeaway
No source in this pass compares them head-to-head; only indirect evidence.

### Cited Findings
- Horizon uses small clump meshes (10-36 triangles) with alpha test — see section 2.
- Tsushima and GPUOpen use per-blade geometry — see sections 1, 5.
- Nanite guidance: grass cards too simple to repay Nanite — [StraySpark](https://www.strayspark.studio/blog/ue5-nanite-foliage-procedural-placement-performance)
- Jahrmann: blades tapering below a pixel alias; they switch to quads at distance — [paper](https://www.cg.tuwien.ac.at/research/publications/2017/JAHRMANN-2017-RRTG/JAHRMANN-2017-RRTG-draft.pdf)

### Gaps
- Team statements on why they moved from cards to blades: not found.

## 7. Far field

### Takeaway
Confirmed practices: widen and thin blades with distance, fade out the 3D ring to a lower layer, replace the ground material beyond the ring (Tsushima, secondary), and in Skyrim a separate grass LOD layer.

### Cited Findings
- Thin-and-widen: [GPUOpen](https://gpuopen.com/learn/mesh_shaders/mesh_shaders-procedural_grass_rendering/), [Jahrmann](https://www.cg.tuwien.ac.at/research/publications/2017/JAHRMANN-2017-RRTG/JAHRMANN-2017-RRTG-draft.pdf)
- Artist-authored textures on distant tiles (Tsushima, per summary) — [gist.ly](https://gist.ly/youtube-summarizer/procedural-grass-systems-in-ghost-of-tsushima-achieving-art-direction)
- Godot: impostor layer fades in at 5-10 m under a high-LOD fade-out at 10-20 m (a cross-fade overlap) — [hexaquo.at](https://hexaquo.at/pages/grass-rendering-series-part-4-level-of-detail-tricks-for-infinite-plains-of-grass-in-godot/)
- Skyrim grass LOD beyond the full-grass grid, tinted via DynDOLOD brightness multipliers (0.5, bottom darkened) — [STEP](https://stepmodifications.org/wiki/SkyrimSE:Skyrim_SE_Grass_LOD_Guide)

### Gaps
- Shell/fin carpets and larger sparse cards in shipped games: not found.

## 8. Edges (cliff or wall tops)

### Gaps
- No documented practice or numbers found in this pass.

## Gaps (consolidated)
- Ghost of Tsushima primary numbers (tile size, blades per tile, vertices per LOD, distance, memory, ms, shadows): unconfirmed; GDC Vault login limit; need talk video transcript.
- Horizon: cards per clump, atlas layout, density, distances, cost; Forbidden West: nothing. The van Muijden deck exceeded the fetch limit and was not read.
- RDR2, KCD1/2, BotW/TotK, Elden Ring, Assassin's Creed, Book of the Dead, Outerra, Acerola, Gothic remake grass: nothing hard found.
- Skyrim card sizes and polygon counts: Nexus 403.
- Cards vs blades rationale from teams; far-field shells/fins; cliff-edge dressing: not found.
- UE 5.7 Nanite Foliage density figures: none published in what I found.

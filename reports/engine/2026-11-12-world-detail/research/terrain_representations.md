# Terrain representations in shipped open-world games (state as of November 2026)

Research notes. About 20 tool calls were made. Several primary slide decks (Far Cry 5 Advances 2018, Horizon GDC 2017) were too large to fetch (over 10 MB), so their numbers come from search-result summaries of those decks and are marked as such. Searches did not reach GDC Vault (paywalled), Digital Foundry or datamining forums. Treat every figure marked "(unverified)" as unverified.

## 1. Heightfield terrains (UE5 Landscape, Unity, Decima, REDengine, Dunia, others)

### Takeaway
Every large AAA open world I could check uses a 2D heightfield at roughly 0.5-1 m per sample for the ground. It is LODed with clipmaps, a quadtree or CDLOD, often with GPU culling and tessellation. Cliffs, caves and overhangs are separate placed meshes (often procedurally placed) plus holes cut into the heightfield. Storage is cheap: a few MB per km².

### Cited findings
- **Horizon Zero Dawn (Decima, 2017):** terrain height data at **0.5 m resolution, 16-bit**, plus a 2 m variance layer. World data is a set of 2D maps streamed in sections, about **4 MB per km²**. Placement is GPU-based, density-driven, deterministic and locally stable, with 500+ asset types (one version of the deck says 100,000+ objects per scene). These figures come from a search summary of the GDC 2017 slides; the PDF itself was too large to fetch. — [Guerrilla GDC 2017 slides](https://www.guerrilla-games.com/media/News/Files/GDC2017_VanMuijden_GPUBasedProceduralPlacementInHorizonZeroDawn.pdf), [80.lv summary](https://80.lv/articles/real-time-procedural-placement-in-horizon-zero-dawn/)
- **Far Cry 5 (Dunia, 2018):** GDC 2018 "Terrain Rendering in Far Cry 5" (Jeremy Moore) describes a GPU compute pipeline for LOD, culling, stitching and rendering of the **height field** terrain. — [GDC Vault](https://gdcvault.com/play/1025480/Terrain-Rendering-in-Far-Cry). A companion talk (Etienne Carrier, GDC 2018) covers procedural biomes, terrain texturing, freshwater networks and **procedurally generated cliff rocks**, which are meshes placed on the heightfield. — [GDC Vault](https://gdcvault.com/play/1025557/Procedural-World-Generation-of-Far). A Reddit comment claims an 8 km world with a 16k heightmap, about 0.5 m per sample (**unverified**). — [r/GraphicsProgramming 2025](https://redlib.hackliberty.org/r/GraphicsProgramming/comments/1kjwxvv/terrain_rendering_questions/mrxaqme/?context=3). The 2018 Advances talk is the primary source for the rest, but I could not fetch it. — [SIGGRAPH Advances 2018 PDF](https://www.advances.realtimerendering.com/s2018/The%20Challenges%20of%20Rendering%20an%20Open%20World%20in%20Far%20Cry%205%20(With%20Notes).pdf)
- **The Witcher 3 (REDengine 3, GDC 2014, Marcin Gollent):** terrain is streamed as a **clipmap** split into tiles, with a texture array. The Novigrad example uses **46×46 tiles of 512×512** and **5 clipmap levels**. Hardware tessellation is driven by a vertical-error clipmap that is downsampled into a quadtree, so software culling runs before tessellation. Holes are handled by splitting the terrain draw call in two. These details come from an unofficial Scribd copy of the slides. — [GDC Vault](https://www.gdcvault.com/play/1020394/Landscape-Creation-and-Rendering-in), [Scribd copy](https://es.scribd.com/document/369764097/Gollent-Marcin-Landscape-Creation-And). If the samples are about 0.5 m, 46×512 ≈ 23.5k samples ≈ 11.8 km. That spacing is my inference and was not confirmed.
- CDPR confirmed tessellation for landscape and water — [DSOGaming interview, 2013](https://www.dsogaming.com/interviews/cd-projekt-red-talks-the-witcher-3-tech-tessellation-physx-dx11-2-windows-8-global-illumination/)
- **UE5 Landscape (Epic docs, current):**
  - Heights are **16-bit**. At the default Z scale of 100, the range is −256 m to +256 m, so the step is about 7.8 mm.
  - Components are square. A component has 1 or 2×2 sections, each up to 256×256 vertices. **Sections are the LOD unit**, with LODs stored in heightmap mips. Each section is a draw call.
  - Epic recommends at most **1024 components**. The largest recommended landscape is **8129×8129** vertices (127 quads per section, 2×2 sections, 32×32 components).
  - Source: [UE Landscape Technical Guide](https://dev.epicgames.com/documentation/en-us/unreal-engine/landscape-technical-guide-in-unreal-engine)
  - The default X/Y scale of 100 cm gives 1 m per quad. This is widely known, but the fetched page did not state it (**unverified on this fetch**).
- **Nanite Landscape (UE5):** the landscape is converted into Nanite static meshes, with one Nanite component per 64 landscape components and up to 64 mesh sections each. Cost is driven by mesh sections, not by landscape components. — [Epic forum thread, community-sourced](https://forums.unrealengine.com/t/nanite-landscapes-component-counts-and-performance-overhead/2836494)
- A general point: heightmaps cannot represent overhangs or caves, because there is one height per (x, y). Voxel data is "several orders of magnitude larger" than a heightmap. — [Bilkent hybrid terrain paper](https://repository.bilkent.edu.tr/bitstream/11693/26473/1/A%20hybrid%20representation%20for%20modeling%2c%20interactive%20editing%2c%20and%20real%20time%20visualization%20of%20terrains%20with%20volumetric%20features.pdf)
- A general point on mixing: Godot Voxel Tools docs say heightmaps are "faster and easier". If players won't edit terrain, they suggest modelling it in a 3D modeller and optimizing up front, and say heightmaps can be mixed with 3D models. — [Voxel Tools docs](https://voxel-tools.readthedocs.io/en/latest/quick_start/)
- LOD algorithms: these are standard references I did not re-fetch this session.
  - Geometry clipmaps (Losasso & Hoppe, SIGGRAPH 2004) — [project page](https://hhoppe.com/proj/geomclipmap/)
  - CDLOD (Strugar, 2010) — [code and paper](https://github.com/fstrugar/CDLOD)
  - Terrain3D for Godot says it uses a geometry clipmap "like The Witcher 3". — [Terrain3D docs](https://terrain3d.readthedocs.io/en/latest/docs/system_architecture.html)

### Inferences
- 0.5 m per sample at 16 bit ≈ 4M samples per km² ≈ 8 MB per km² raw, which matches Horizon's ~4 MB per km² (after compression or with fewer layers). A 2 km Gothic region at 0.5 m is a 4096² heightmap, about 32 MB raw: trivial.
- In all these games, cliffs and rock faces are meshes (hand-made, scanned or procedurally placed). Caves are separate interiors or meshes behind holes in the heightfield. Nothing I found shows a AAA heightfield game making caves in the terrain itself.

### Gaps
- No primary numbers found for RDR2 (RAGE), Ghost of Tsushima, Frostbite or Unity Terrain sample spacing. Search time was spent elsewhere.
- No primary triangles per m² near the camera for any game. The Far Cry 5 and Witcher 3 decks probably contain them but could not be fetched.
- Nanite Landscape memory and streaming numbers: no official Epic figures found.

## 2. Voxel / SDF terrains in shipped games

### Takeaway
Shipped voxel terrains split into two families:
- Survival or sandbox games, where editing is the core loop: Enshrouded, Roblox Smooth Terrain, No Man's Sky, Astroneer, 7 Days to Die, Teardown.
- Hand-authored-room plus CSG hybrids: Deep Rock Galactic.

They pay for editability with coarse voxels (about 0.5-1.2 m for terrain), storage and meshing machinery (sparse chunks, RLE, LOD seams), and weak sharp man-made features. None of them is a dense, hand-authored, AAA-detail RPG world.

### Cited findings
- **Roblox Smooth Terrain (shipped about 2015; storage post by Arseny Kapoulkine, 2017):**
  - **2 bytes per voxel** uncompressed (material plus an 8-bit occupancy).
  - Storage chunks are **32³ (64 KB)**. Network uses 16³ chunks for the first send and 4³ for updates; physics uses 8³; render chunks grow from 16³ to 256³.
  - Measured on a level with about 1 billion non-empty voxels: **0.07 B per voxel on disk (RLE)**, 0.04 with RLE+zstd, **2.97 B per voxel unpacked in memory**, 0.49 B per voxel row-packed.
  - LOD uses downsampled mips (16³, 8³, 4³).
  - Hermite and SDF models were **rejected**, so sharp edges depend on material-driven shaping. Small writes are slow because they can repack chunks.
  - The voxels "had to be pretty big" for low-end platforms. Roblox voxels are 4 studs (about 1.1-1.2 m), which is known but not stated in this post (**unverified here**).
  - Sources: [zeux.io 2017](https://zeux.io/2017/03/27/voxel-terrain-storage/); a SIGGRAPH 2020 talk on the same system is at [zeux.io/data/siggraph2020.pdf](https://zeux.io/data/siggraph2020.pdf) (not fetched).
- **Enshrouded (Keen Games, early access 2024, 1.0 in 2025-26):** built on a custom engine "primarily to realize our ambitious voxel world". Everything, "from the smallest rock to the largest castle", is voxel-based. The team came from voxel experience on Portal Knights.
  - Sources: [mein-mmo interview](https://mein-mmo.de/en/enshrouded-dcp-interview-antony-christoulakis,1475032), [Enshrouded AMA highlights](https://enshrouded.com/news/reddit-ama-highlights)
  - Behind-the-scenes video "Creating the world of Enshrouded" — [RPG Site](https://rpgsite.net/news/15193-enshrouded-creating-world-enshrouded-behind-scenes-video)
  - The voxel size (commonly quoted as 0.5 m) and world dimensions were **not confirmed** from a primary source.
  - Forum discussion says chunks drop in resolution with distance, down to about 1 voxel per chunk. Hand-built voxel "models" reportedly have little voxel LOD (**unverified, Reddit**). — [r/VoxelGameDev 2024](https://lr.in.psf.lt/r/VoxelGameDev/comments/1c453u3/isosurface_algorithm_used_by_keen_games_in)
- **No Man's Sky (GDC 2017, Innes McKendrick):** the pipeline runs from voxel-based generation through polygonization, texturing and population, generated continuously at runtime. — [GDC Vault](https://gdcvault.com/play/1024265/), [Game Developer video summary](https://gamedeveloper.com/programming/video-how-continuous-world-generation-works-in-i-no-man-s-sky-i-)
  - A blog by Nick Gildea describes chunked SDF plus octree plus dual contouring for *his own* NMS-inspired engine. It is **not** a confirmed NMS method. — [ngildea blog 2015](https://ngildea.blogspot.com/2015/12/improving-generation-performance.html)
- **Teardown (Tuxedo Labs, 2020-22):**
  - Thousands of volumes, from a few hundred to millions of voxels each. Shapes are stored as **dense uncompressed 3D grids**, so empty space costs memory.
  - Large levels hold about **half a billion voxels**. Quicksaves are RLE'd to 15-20%, about **80 MB**. Physics data is rebuilt at load time.
  - Gustafsson's newer engine uses sparse 8×8×8 chunks with a bitmap. Per 80.lv, it can use *more* memory per voxel and needs compression.
  - Voxel size is about 10 cm per MagicaVoxel convention (**unverified**).
  - Sources: [Voxagon quicksave post 2020](https://blog.voxagon.se/2020/11/18/teardown-quicksave.html), [80.lv new engine](https://80.lv/articles/teardown-creator-is-working-on-a-new-voxel-engine), [PCGH tech test](https://www.pcgameshardware.de/Teardown-Spiel-73677/Specials/PC-Game-Test-Benchmark-Voxel-Dynamic-Destruction-1394545/2/)
- **Deep Rock Galactic (Ghost Ship):**
  - The developer says each cave room is **hand-crafted**. The cave system is generated from clusters of shapes that tell the engine what volume to carve from solid rock, and noise is then applied to walls, floors and ceilings. Early versions tried a blockier Minecraft-like terrain. — [Ghost Ship dev post via devtrackers](https://devtrackers.gg/deep-rock-galactic/p/8cd5ca9c-below-decks-at-ghost-ship-cave-generation-in-deep-rock-galactic)
  - Community claims of marching tetrahedra at generation time, then CSG at runtime, are **unverified** and disputed in the thread. — [r/howdidtheycodeit 2024](https://redlib.hbubli.cc/r/howdidtheycodeit/comments/1e69166/how_did_they_code_the_pathfinding_implementation/ldv2sxo/?context=3)
- **Voxel Plugin (UE):**
  - Version 2 targets UE 5.6/5.7 and is "actively being developed and can be buggy". It has Nanite output and a Render Chunk Size setting against hitches on high-resolution worlds. — [Voxel Plugin docs](https://voxelpluginvitedocs.pages.dev/2.0p8%20-%20v2/design-docs/overview), [2.0p8 changelog](https://voxelpluginvitedocs.pages.dev/resources/change-log/2.0p8-changelog)
  - Version 1.2 docs: chunks are 32³ at LOD0, doubling per LOD (64³, 128³…). Keep world size small. **Vertex data uses a lot of memory**. Only edited data is stored. — [VP 1.2 world size and LOD](https://voxelpluginvitedocs.pages.dev/1.2/core-systems/voxelworld/world-size-and-level-of-details), [VP 1.2 performance](https://voxelpluginvitedocs.pages.dev/1.2/technical-notes/performance-and-profiling)
  - VP2 can use any mesh to shape the terrain (stamps). — [80.lv 2024](https://80.lv/articles/voxel-plugin-2-for-ue5-lets-you-use-any-mesh-to-shape-terrain)
- LOD seams are the classic voxel pain: a high-resolution chunk next to a low-resolution one leaves cracks, so extra stitching triangles are needed. — [voxel LOD article 2018](https://legalian.github.io/voxel/2018/03/12/voxel-level-of-detail.html)
- Standard algorithm references (not re-fetched this session): Transvoxel, Lengyel 2010, which fixes LOD seams for marching cubes — [transvoxel.org](https://transvoxel.org/); Dual Contouring, Ju et al. 2002, which keeps sharp features through Hermite data.

### Inferences
- Voxel sizes in shipped *editable* terrain are coarse: Roblox about 1.2 m, Enshrouded likely about 0.5 m (unverified), Valheim's 64 m zones are heightmaps. Gothic's hand-modelled cliffs, walls and city detail need centimetre-level sharp edges. At 0.5 m an SDF rounds off every wall corner, window, stair and thin rock fin. Dual contouring with Hermite data helps edges but adds storage and non-manifold or self-intersection problems. Roblox explicitly gave up on Hermite and SDF data.
- Texturing a voxel mesh needs triplanar mapping and loses authored UVs. A hand-modelled world with UV-mapped textures loses its look if converted wholesale.
- Memory: at 0.25 m, a 2×2 km region with 100 m of vertical relief is 8000×8000×400 ≈ 2.6×10¹⁰ cells. Only the near-surface band matters after sparse storage, but that still needs Roblox-grade RLE and paging machinery. A heightfield plus meshes is orders of magnitude cheaper.

### Gaps
- Primary voxel sizes for Enshrouded, Astroneer, 7 Days to Die and No Man's Sky are not confirmed.
- No post-mortem found on EverQuest Next/Landmark (Voxel Farm). The Procworld blog (Miguel Cepero) is the place to look, and it was not fetched.
- No Dreams (Media Molecule) SDF numbers fetched. Alex Evans' SIGGRAPH 2015 "Learning from failure" is the known source.
- No CPU and memory budgets per km² from any shipped voxel game.

## 3. Hybrids: static world plus local deformation or destruction

### Takeaway
The shipped destruction-heavy games either:
- deform a heightfield (Valheim, likely Helldivers 2), or
- break pre-authored building pieces with physics (The Finals, Battlefield, Red Faction).

I found no shipped open world that converts a static mesh into SDF where it is hit. That pattern exists only in a Valheim mod.

### Cited findings
- **Valheim (Iron Gate, Unity):**
  - Terrain is a heightmap per **64×64 m zone**.
  - Originally every pickaxe or hoe hit spawned a TerrainModifier object. A heightmap rebuild reset the zone and **re-applied every modifier in range**, which was the main source of lag. — [BetterTerrain mod README](https://github.com/74oshua/BetterTerrain/blob/master/README.md)
  - Patch 0.150.3 (2021) introduced a new terrain modification system to cut network instances and loading time. — [PC Gamer](https://www.pcgamer.com/valheim-patch-overhauls-terrain-buffs-hoe), [GamingOnLinux](https://www.gamingonlinux.com/2021/04/valheim-gets-a-needed-patch-new-terrain-modification-system-and-no-more-draugr-in-stones/page=1)
  - Edits are stored per zone (TerrainComp). The HearthBelow mod converts a zone's heightmap, including edits, into a voxel volume on the first carve. This is a third-party example of "heightmap until dug, then voxel". — [HearthBelow](https://thunderstore.io/c/valheim/p/Azumatt/HearthBelow/)
  - A performance mod notes that edits must invalidate the sector and its neighbours' caches to avoid seams. — [Valheim Performance Overhaul](https://thunderstore.io/c/valheim/p/Skarif/ValheimPerformanceOverhaul/v/6.3.0/)
- **Helldivers 2 (Arrowhead, Stingray-derived, 2024):** terrain deformation with lasting craters from explosions, on procedurally generated maps, confirmed by CEO Johan Pilestedt. A patch note mentions "Improved performance of terrain destruction". The implementation (heightmap or other) is **not documented** publicly. — [GamingBolt 2024](https://gamingbolt.com/helldivers-2-features-terrain-deformation-and-procedural-generation)
- **The Finals (Embark, UE, 2023):**
  - All destruction is simulated **on the server**, with no physics on clients ("a single truth for physics"). — [Wccftech](https://wccftech.com/the-finals-multiplayer-fps-shows-impressive-destruction-ahead-of-alpha-playtest/)
  - Reported to use two layers: structural integrity of map elements, and debris. — [KitGuru](https://www.kitguru.net/?p=748306)
  - GDC 2024 talk "Engineering Mayhem" (Måns Isaksson), not read because of the paywall. — [GDC Vault](https://gdcvault.com/play/1034280/Eng)
  - Whether buildings are pre-fractured is **unconfirmed**.
- Hybrid research: heightfield plus volumetric features (caves, arches, overhangs) has been proposed so that editing and rendering stay efficient. — [Bilkent paper](https://repository.bilkent.edu.tr/bitstream/11693/26473/1/A%20hybrid%20representation%20for%20modeling%2c%20interactive%20editing%2c%20and%20real%20time%20visualization%20of%20terrains%20with%20volumetric%20features.pdf). A forum describes a CryEngine-era split of heightmap ground plus voxel caves. — [gamedev.net](https://gamedev.net/forums/topic/608202-cryengine-terrain/)
- Teardown is the only fully voxel destruction world here, at small level scale (see section 2).

### Inferences
- For "craters plus breakable walls" in an RPG, the cheapest shipped-proven path is:
  - **heightfield deformation** for ground craters (Valheim and, likely, Helldivers);
  - **pre-cut or fractured mesh pieces** for walls and buildings (The Finals, Battlefield);
  - optionally a **local SDF patch** only where a hole has to go *through* rock (cave breach). That last part has no shipped example in my findings, so it is the project's own risk.

### Gaps
- Red Faction Guerrilla (GeoMod 2.0) and Battlefield (Frostbite Destruction) primary talks were not fetched in this session.
- Fortnite (voxel-like building grid) was not researched.
- No primary detail on how Helldivers 2 stores craters.

## 4. Worlds made mostly of hand-placed meshes (Souls, Gothic family, Kingdom Come, Gothic Remake)

### Takeaway
I found **no reliable technical source** on whether Elden Ring, Dark Souls, Elex, Kingdom Come or the UE5 Gothic 1 Remake use heightfields or pure meshes. Search results for Elden Ring were size estimates (about 79-80 km²) and spam pages with invented "technical breakdowns".

### Cited findings
- Elden Ring's area is estimated at about 79 km² by fan measurement. — [PC Gamer 2022](https://pcgamer.com/games/rpg/elden-ring-geographer-tests-rigorous-calculation-against-weed-fueled-horse-math-to-determine-the-exact-size-of-the-lands-between)
- The pages claiming Elden Ring LOD or SSD streaming details cite nothing and were excluded. — [ggrecon (size only)](https://www.ggrecon.com/guides/elden-ring-map-size-comparison)
- Godot Voxel Tools recommends modelling non-editable terrain in a 3D modeller and optimizing up front. This is the hand-modelled route, and it is valid when players don't edit terrain. — [Voxel Tools docs](https://voxel-tools.readthedocs.io/en/latest/quick_start/)

### Inferences
- Gothic II's existing world is already in this category: one static mesh with cliffs, caves and buildings together. Keeping it keeps every landmark and sight line exactly. Its missing pieces are an LOD scheme (per-chunk simplified meshes or a Nanite-like cluster hierarchy) and a split of the ground surface from structures. Representation is not the gap.

### Gaps
- FromSoftware, Warhorse, Piranha Bytes and Alkimia (Gothic Remake) terrain internals: no public sources found. The next step would be datamining wikis (Souls modding: MSB/FLVER and heightmap files) and UE5 Gothic Remake developer videos.

## 5. Expert recommendations for a small team (caves, overhangs, limited destruction)

### Takeaway
The sources that speak to this all point the same way:
- Heightmaps are faster and easier, so use them unless players must dig.
- Add caves and overhangs as meshes.
- Use voxels only where editing is the game, and expect costs in memory, LOD seams, texturing and sharp features.

### Cited findings
- "Heightmaps are faster and easier to work with". Model non-editable terrain in a modeller. Mix heightmaps with 3D models. — [Godot Voxel Tools docs](https://voxel-tools.readthedocs.io/en/latest/quick_start/)
- Roblox engine lead (2017): voxels had to be big for low-end hardware. Hermite and SDF were rejected for practical reasons. Writes are expensive. Large engineering effort went into RLE, row packing and chunk sizing across disk, network, physics and render. — [zeux.io](https://zeux.io/2017/03/27/voxel-terrain-storage/)
- Voxel Plugin's own docs advise keeping worlds small and warn that vertex data dominates memory. — [VP 1.2 docs](https://voxelpluginvitedocs.pages.dev/1.2/core-systems/voxelworld/world-size-and-level-of-details)
- Voxel LOD cracks need extra stitching geometry. — [legalian 2018](https://legalian.github.io/voxel/2018/03/12/voxel-level-of-detail.html)
- Deep Rock Galactic, the best-known cave game, still hand-crafts its rooms and uses generation only to assemble and carve them. — [Ghost Ship via devtrackers](https://devtrackers.gg/deep-rock-galactic/p/8cd5ca9c-below-decks-at-ghost-ship-cave-generation-in-deep-rock-galactic)
- A Game Developer article titled "Towards realtime deformable worlds: why tetrahedra rule, voxels drool" argues against voxels for deformable worlds. It was not fetched, so its content is unverified. — [Game Developer](https://gamedeveloper.com/design/towards-realtime-deformable-worlds-why-tetrahedra-rule-voxels-drool)

### Inferences (an honest challenge to "SDF as the source representation")
- **Against SDF as the source of truth for Gothic:**
  1. The original world is a hand-modelled mesh with sharp walls, buildings and stairs. Resampling it into an SDF at any affordable resolution (0.25-0.5 m) rounds or loses thin and sharp features, and so breaks "keep layout, landmarks and sight lines".
  2. Authored UVs and textures are lost and replaced by triplanar mapping.
  3. LOD seams, meshing cost and storage machinery (Roblox-level) are a large engineering investment for one developer.
  4. No shipped hand-authored RPG world of AAA density uses voxel terrain. Enshrouded is the closest and is built around building and digging, at a stylised look.
- **For a hybrid:** a heightfield (0.5 m, 16-bit, clipmap or CDLOD) for open ground fits the industry-standard path: Horizon, Far Cry 5, Witcher 3, UE Landscape. Existing hand-modelled cliffs, caves and buildings become mesh pieces with their own LODs or Nanite-style clusters. Craters deform the heightfield, as in Valheim and Helldivers. Walls are pre-cut pieces, as in The Finals. A local sparse SDF or voxel patch is created only around a hit on rock or cave meshes, and remeshed there. Every part except the last has shipped precedents, and the last is confined and optional.
- **Scriptable authoring:** Blender Python can export a heightfield from the existing mesh (ray-cast down at 0.5 m) and split off everything with overhangs or vertical faces as separate meshes. That keeps the original exactly.

### Gaps
- No GDC or SIGGRAPH talk found that directly weighs voxel terrain against heightfield plus meshes for a small team. Candidates to look for: Procworld/Voxel Farm posts, Media Molecule's "Learning from failure" (SIGGRAPH 2015), and Embark and Keen talks.
- No concrete triangles per m² or build-time numbers from any source in this session.

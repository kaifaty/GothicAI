# Engine and content-tool audit for a denser, more detailed world (read-only, 2026-11-12)

Worktree: `gothic-skeletal-animations-mpfb2-f32f5d` (branch from `main` at `668c1521a`). Paths are relative to the
repo root; `OG/` = `OpenGothic/`. Nothing was built or run. "UNVERIFIED" marks inference not confirmed in code.
Docs read: `docs/map/engine.md`, `docs/world-streaming.md` (Как работает сейчас + plan), `docs/graphics-plan.md`,
`docs/world-detail-plan.md`, `docs/remaster-pipeline.md#правка-геометрии-мира`, `docs/asset-migration.md#мир`,
`reports/performance/2026-11-10-content-stress/README.md`, `reports/engine/2026-11-03-streaming-optimisation/README.md`,
`reports/content/2026-10-world-meshes/` (census). Where docs and code disagree it is noted.

Key numbers (from docs/reports, consistent with code): NEWWORLD world mesh 473,502 triangles in 403 chunks of 64 m
(`docs/world-streaming.md` 4a log); 359 of 403 chunks are Far after warm-up; ~5133 draw buckets; static-mesh vobs in
NEWWORLD 11,546 instances (7,126 "nature"), ~2.11 M triangles over all instances (nature ~1.11 M) — computed from
`reports/content/2026-10-world-meshes/families.csv` + `newworld.csv`; harbour ~0.68 M triangles/frame on Ryzen
(stress report; the older 1.26 M figure counts all passes differently).

---

## 1. World mesh path, end to end

**What exists**

- Import (ZEN -> base): `content-convert world` (`scripts/convert/convert.cpp`) calls `writeWorldChunks`
  (`OG/common/worldchunks.cpp:96`). It is a lossless re-serialisation of the ZEN `asset::WorldMesh` after the BSP filter:
  - triangle -> chunk by centroid in X/Z, 64 m grid rooted at (0,0) (`worldchunks.cpp:108-118`, `chunkCoord`);
  - one glTF primitive per material per chunk, shared vertex table; vertex = (source vertex, source feature) pair
    (`worldchunks.cpp:121-166`);
  - attributes: `POSITION`, `NORMAL`, `TEXCOORD_0`, `COLOR_0` (Gothic baked vertex light, RGBA8), custom `_VERTEX`,
    `_FEATURE` as float32 source ids (`worldchunks.cpp:180-188`);
  - per-triangle `extras.gothic`: `order` (source triangle index, float), `flags` (2x uint16: portal/occluder/sector/
    outdoor/lod bits + sector index), `lightmap` (float) (`worldchunks.cpp:189-207`, `packFlags` :29-35);
  - full Gothic material record in material `extras.gothic` (`materialJson`), full material table in `world.json`;
  - unused vertices/features -> `unreferenced.bin` (`worldchunks.cpp:228-266`).
  - Hard limit: `exactIndexLimit = 1u<<24` — triangles, vertices, features of the WHOLE world must each be < 2^24
    because ids/order are stored as float (`worldchunks.cpp:25-26,104-105`; reader `worldchunksruntime.cpp:96-97,324,355`).
- Runtime read: `Resources::loadWorld` (`OG/common/resources_base.cpp:174`) -> `tryWorldData` -> `readWorldChunks`
  (`OG/common/worldbase.cpp:75`, `OG/common/worldchunksruntime.cpp:533`). It reads EVERY chunk file listed in
  `world.json` (override: same file name under `content/worlds/<W>/chunks/` replaces the base one, `:558-559`),
  restitches the source-order `asset::WorldMesh` (source triangles sorted by `order`, edited triangles with no `order`
  appended, `:567-583`), and fills a `ChunkTable` (per-triangle chunk ownership + chunk-local ordinals,
  `worldchunksruntime.h:25-44`). Requires a 64 m grid (`:540`).
- Consumers of the stitched whole mesh at load (`OG/common/world/world.cpp:141-280`):
  - physics: `DynamicWorld(worldMesh)` -> `PackedMesh(..., PK_Physic)` (whole world, `dynamicworld.cpp:221`);
  - cloth extraction `WorldCloth::extract` (`world.cpp:147`);
  - landscape view: if streaming, all landscape materials are decoded first (`world.cpp:172-182`), then
    `WorldView::startLandscapeStreaming(std::move(worldMesh))` (`world.cpp:274`), which keeps only per-chunk id lists
    and re-reads each chunk GLB on demand via `ChunkMeshSource::read` (`worldchunksruntime.h:55-68`,
    `worldview.cpp:401-406`). Without streaming (RT/GI, `-benchmark`, `OPENGOTHIC_STREAMING=0`) one whole-world
    `PackedMesh(worldMesh, PK_VisualLnd)` (`world.cpp:185-191`).
- GPU packing: `PackedMesh::packMeshletsLnd` (`OG/common/graphics/mesh/submesh/packedmesh.cpp:807-871`): triangles
  sorted by (merged) material, meshlets of `MaxVert=64`, `MaxPrim=64` (`packedmesh.h:22-27`), keyed on (source vertex,
  source feature) ids — hence the requirement that unedited chunks keep their source ids. Materials that are "visually
  same" are merged by an O(M^2) pass (`landscapeMaterialMap`, `packedmesh.cpp:782-799`).
- Index widths: global `indices` uint32 + meshlet-local `indices8` uint8 (`packedmesh.h:52-53`); vertex format
  `Resources::Vertex {pos[3], norm[3], uv[2], uint32 color}` = 36 B, ONE UV set, no tangent (`OG/common/resources.h:84-89`).
- GPU buffers: streamed chunks go into shared `LandscapePages` (pages of 4096 meshlets, each meshlet slot
  64*36 B vertices + 64*4 B indices; `staticmesh.cpp:12-15`, `staticmesh.h:19-43`); one `Landscape` per chunk
  (`landscape.h`), one `VisualObjects::Item` per (chunk, material) submesh (`landscape.cpp:31-50`).
- Static meshes (vob visuals): base format uint16 positions per wedge and uint16 wedges per triangle
  (`OG/common/asset/staticmesh.h:10-15`) => <=65,536 positions per mesh and wedges per submesh; own glTF meshes are
  rejected above 65,535 (`OG/common/gltfmesh.cpp:29,429,476`). (Stress report says 65,536; own-GLB loader is 65,535.)

**Per-chunk limits**: none explicit for triangles/materials per chunk. Indirect limits: global 2^24 ids; uint16 bucket
ids (one bucket per (StaticMesh, Material), i.e. per chunk x material, see Q2); landscape cluster per meshlet.

**What a different chunk-geometry source must provide** (own generated meshes, several LODs):
- Today the loader REQUIRES the converter's chunk form: glTF triangles, `POSITION`+`NORMAL` mandatory, `TEXCOORD_0`,
  `COLOR_0` optional (defaults white, `worldchunksruntime.cpp:296-319`), materials with `extras.gothic` (`:207-218`),
  file names `<x>_<z>.glb` on the 64 m grid. New geometry may omit `_VERTEX/_FEATURE/order` (gets appended ids, `-1`
  marks added geometry, `:320-330`) but total ids still < 2^24 and it is still stitched into ONE world mesh at load.
- `COLOR_0` is not used by landscape shading (landscape shaders have no `VT_COLOR`, `OG/shader/CMakeLists.txt:128-133`;
  only PFX use it, `:216-220`); it feeds world cloth and `world_edit.py`'s checks.
- There is no notion of LOD per chunk, no second geometry set, no per-chunk collision mesh, no binary/packed chunk
  format: every chunk is parsed as glTF+JSON by cgltf at load and again on each Near/Far pack.
- A real LOD source would need: (a) a chunk table that can name several meshes per cell (or a separate HLOD grid),
  (b) a reader that does NOT stitch everything into one `asset::WorldMesh`, (c) physics/navigation inputs decoupled from
  the render mesh (Q5), (d) bucket/command ids wider than 16 bits (Q2), (e) seams between LOD levels (none handled).

**Implication**: geometry x4 is format-safe; x16 is format-safe for the world (473k x16 = 7.6 M < 16.7 M) but not for
x64 (stress report). The real blocker is that every load reads, parses and stitches the whole world (1.1 us/triangle,
544 -> 8330 ms at x16) and three consumers (physics, cloth, residency table) need the whole stitched mesh.

## 2. How landscape and static meshes are drawn

**What exists**
- GPU-driven pipeline: every drawable is a "cluster" (`DrawClusters::Cluster {pos, r, uint16 commandId, uint16
  bucketId, firstMeshlet, meshletCount, instanceId}`, `OG/common/graphics/drawclusters.h:17-25`). Landscape: one cluster
  per meshlet (`visualobjects.cpp:125-128,547-548`); objects: one cluster per object-submesh drawing all its meshlets.
- `visibility_pass.comp` (`OG/shader/materials/visibility_pass.comp:191-237`): per cluster frustum test, projected
  AABB, HiZ test (main view), appends (instance, meshlet, bucket) to a payload and atomically bumps the instance count
  of its draw command. Small clusters (<16 px^2) are skipped only in the HiZ-occluder variant `LARGE` (:205-213).
- Draw commands: one per distinct pipeline triple (main/shadow/HiZ) and, when not bindless, per bucket
  (`DrawCommands::commandId`, `OG/common/graphics/drawcommands.cpp:176-208`); each is ONE indirect draw of
  `vertexCount = MaxInd (192)` x N meshlet-instances (`drawcommands.cpp:256-262`) or a mesh-shader dispatch. So repeated
  props are already GPU-instanced through the indirect payload — there is no per-object draw call. With bindless
  (default) buckets go into the payload, not the command.
- Buckets: one per (StaticMesh*/AnimMesh*, Material) (`OG/common/graphics/drawbuckets.cpp:43-78`); id is `uint16_t`
  and is SILENTLY truncated (`DrawBuckets::Id::Id ... id(uint16_t(id))`, `drawbuckets.cpp:22`; packed in 16 bits in the
  shader, `visibility_pass.comp:223-224`). Allocation is a linear scan over all buckets (`drawbuckets.cpp:44-60`),
  command lookup is a linear scan over all commands (`drawcommands.cpp:186-192`). Each streamed landscape chunk has
  its own `StaticMesh`, hence its own bucket per material.
- Mesh shaders (`-ms`, env `OPENGOTHIC_MS`): default ON except macOS (`OG/common/commandline.h:65-69`, "slower on M1");
  enabled only if the device reports it (`OG/common/graphics/shaders.cpp:76`). Vulkan: from `VK_EXT_mesh_shader`
  features (`OG/lib/Tempest/Engine/gapi/vulkan/vdevice.cpp:629-637`). Metal: reported on Metal3 + Apple7 GPUs
  (`OG/lib/Tempest/Engine/gapi/metal/mtdevice.mm:575-582`) but the engine default keeps it off on macOS; it is the same
  meshlet data either way. The mesh-shader variant culls per primitive (backface) (`OG/shader/materials/main.vert:270-349`);
  the vertex path is "one instance per meshlet" (`drawcommands.cpp:256-262`).
- Passes per frame (`OG/common/graphics/renderer.cpp:828-847`): visibility pass 0 -> HiZ depth pre-pass of occluders
  (`drawHiZ`) -> HiZ mip build -> visibility pass 1 for main + shadow views -> G-buffer -> two sun cascade shadow maps
  (`drawShadowMap`) or, on non-Apple, virtual shadow maps (`drawVsm`; default off on Apple, `commandline.h:70-75`).
  Every cluster is tested once per enabled view (main, HiZ, shadow0, shadow1, VSM). VSM caps meshlet-page pairs at
  `VsmMaxMeshlets = 512*1024` per frame (`OG/common/graphics/drawcommands.h:16`).
- RT acceleration structures only with `-rt 1` (default OFF everywhere, `commandline.h:61-63`), see Q6.

**Distance LOD / fade / cull distance**: none for world geometry. `Camera::zFar` = 100000 cm fixed
(`OG/common/camera.cpp:168-171`); `zCZoneVobFarPlane` vobs are ignored on purpose ("wont do: no distance culling",
`OG/common/world/objects/vob.cpp:327-330`); MRM progressive LOD is not used (mesh loaded at full detail; Marvin
`zprogmeshlod` is a no-op, `OG/common/marvin.cpp:101`). The only geometric LOD in the engine is hair strands
(`visualobjects.cpp:833-880`). Chunk residency Far ring reaches `zFar` (`chunkresidency.h:58,68`).

**Missing**: per-object/per-cluster distance or screen-size cull, LOD selection, cross-fade/dither, impostors, HLOD,
cluster LOD; bucket/command ids >16 bit; O(1) bucket alloc; a budget on per-frame cluster count.

**Implication**: draw-call count is not the problem (indirect instancing); GPU culling cost and payload size scale
linearly with ALL resident clusters x views (everything within 1 km), and buckets scale with chunks x materials
x LODs. x16 geometry with today's chunk count is fine for buckets (~5k); several LODs per chunk or many own unique
meshes multiply buckets toward the silent 65,536 wrap. VSM's 512k meshlet-page cap can clip shadows with dense meshes
(UNVERIFIED whether overflow is guarded).

## 3. Vegetation today

**What exists**
- Trees, bushes, grass, ferns are vobs with static `.MRM` visuals (census: BUSH 1620, GRASSGROUP 688, FARNTEPPICH 637,
  TANNE 598, SMALLTREE 461, FARN 313, HOHETANNE 282, TREE 43 instances in NEWWORLD; `families.csv`). Some foliage is
  baked into the world mesh (docs: 243 two-sided edges of `NW_NATURE_BRANCH_01` in chunk `1_-1`,
  `docs/remaster-pipeline.md` "Ограничение"; UNVERIFIED in code).
- Each vob: `ObjVisual` -> `world.addStaticView` -> one `VisualObjects` object + cluster + instance slot per submesh
  (`OG/common/graphics/objvisual.cpp:347-371`), streamed per 64 m chunk Near/Far/Outside (`worldview.cpp:25-170`).
  The mesh itself is shared by name (`Resources::loadMesh`), so the GPU instancing above applies.
- Alpha: original foliage uses `Material::AlphaTest` with a fixed threshold; own glTF MASK gets hashed alpha under TAA
  (`BK_HASHED_ALPHA`, `drawbuckets.cpp` flags block ~:283-286).
- Wind: whole-object shear of the Y axis, computed on the CPU per windy object per frame
  (`VisualObjects::preFrameUpdateWind`, `OG/common/graphics/visualobjects.cpp:823-831`; formula
  `OG/common/physics/worldwind.h:22-42`) and re-uploaded via `updateInstance`. No vertex-level bending, no per-leaf
  motion. Windy vobs get NO collision (`objvisual.cpp:354,368`).
- Non-windy static vobs each get a Jolt static body (`PhysicMesh`, `objvisual.cpp:368-370`; `DynamicWorld::addStaticObj`
  `dynamicworld.cpp:520-531`).

**Missing**: no ground-cover/grass system (procedural scatter, GPU-generated instances, density maps), no foliage LOD
or impostors, no distance fade, no GPU wind, no translucency/subsurface for leaves.

**Limits for 100x ground cover** (e.g. 7k nature vobs -> ~700k):
- CPU: each instance is a full vob (`Vob`/`ObjVisual`, `VisualObjects::Object`, cluster, instance slot, residency entry,
  texture-demand bookkeeping `updateTextureDemand`, `visualobjects.cpp:470,515`) — created at load in `wobj.addRoot`
  (`world.cpp:288-291`); vob creation already costs 369-1512 ms at x1-x16 meshes (stress report).
- Wind on CPU per object per frame (`visualobjects.cpp:823-831`).
- Jolt: `system.Init(65536 bodies, 0, 65536 pairs, 32768 contacts)` (`OG/common/physics/collisionworld_internal.h:144`) —
  dense collidable props would hit 65,536 bodies.
- Buckets: per (mesh, material), so instancing many copies of a FEW meshes is cheap in buckets; many unique meshes is not.
- GPU: one cluster per instance-submesh tested per view per frame; payload is `maxPayload*16 B` per view
  (`drawcommands.cpp:318-320`); instance SSBO per object. vobs.d layers are JSON records (Q8) — 100k entries is
  impractical as hand/script-authored JSON (UNVERIFIED performance; parser is rapidjson).

**Implication**: dense vegetation needs a separate instanced foliage path (cell-based instance buffers, GPU culling with
distance cull and LOD, GPU wind), not 100x more vobs.

## 4. Terrain materials

**What exists**
- One material per triangle; the chunk primitive per material (`worldchunks.cpp:121-125`); no blending between
  materials, no splat/weight maps, no second UV, no vertex colour used in shading (Q1).
- Normals: vertex normals in the G-buffer (`OG/shader/materials/main.frag:447,496-513`); `FLAT_NORMAL` only sets a
  "facing away from the sun" hint bit (`main.frag:78-90,113-121`). Normal maps use a cotangent frame from derivatives —
  "Gothic meshes have no stored tangents" (`main.frag:384-395`).
- Own materials via `content/materials/materials.json` keyed by texture name (`OG/common/resources_textures.cpp:138-260`):
  fields `albedo`, `normal`, `orm` (DDS paths), `scale` (UV scale), `antitile` (`off|offset|rotate`), `antitile_cell`,
  `emissive`, `turbidity`, `turbidity_depth`, `hair`, `strand`, `eye`. No height/parallax, detail map, macro map,
  triplanar, decal or blend fields.
- Anti-tiling: in the engine as hex-tiling after Mikkelsen JCGT 2022 (`main.frag:182-260,404-480`), bucket flags
  `BK_ANTITILE`, `BK_ANTITILE_ROTATE` (`drawbuckets.cpp:258-263`), only for materials with material maps, opt-in per
  `materials.json` entry, default off (`docs/features/engine-render.md:61`). `reports/content/2026-10-antitile/` holds
  only the comparison captures (off/offset/rotate, day/night, Mac/Ryzen).
- Tessellation exists only for water (`Material::isTesselated` returns `alpha==Water`, `OG/common/graphics/material.cpp:177-182`;
  `main.tese` does Gerstner waves), and is off on Metal (`mtdevice.mm:548-549` sets `tesselationShader=false`).
- Texture codecs: DDS DXT1/3/5 only (`OG/lib/Tempest/Engine/formats/image/pixmapcodecdds.cpp:51-60`); no BC5/BC7.
- Decals: blood decals only (`OG/shader/blood/`, `common/graphics/blooddecals.*`); vob decals (`zCDecal`) are drawn as
  quads (UNVERIFIED detail). `world-detail-plan.md` lists parallax (Д4) "нет", decals (Д5) "не проверено".

**Missing**: material blending/splatting on terrain, detail/macro textures, triplanar for cliffs, parallax/POM, height
maps, BC5/BC7, mesh decals for terrain, a second UV channel or vertex weights in the world vertex format.

**Implication**: "more realistic terrain" by material means (blends, detail, macro variation) needs a vertex-format and
shader change (weights or splat textures per chunk) — impossible to express in today's chunk/material format, where every
triangle owns exactly one Gothic material. Geometry density alone will not hide repetition; hex-tiling helps per
material only.

## 5. Collision and navigation ties

**What exists**
- Static world collision = ONE Jolt body for the whole landscape and one for water (`dynamicworld.cpp:221-316`), built
  from `PackedMesh(worldMesh, PK_Physic)` of the same render mesh (material groups merged unless the name has `:`,
  `packedmesh.cpp:691-765`). Shape: `PhysicVbo::shape` -> `StaticCompoundShape` of `MeshShape`s split by a median
  tree until the diagonal is <= 256 m (`OG/common/physics/physicvbo.cpp:22,190-235,474-528`); optional subdivision of
  edges > 4 m (`static_collision_split`, `physicvbo.cpp:31,503-509`).
- Cache: `CollisionCache` key = base fingerprint, world, content fingerprint, flags (`dynamicworld.cpp:266-296`,
  `common/physics/collisioncache.*`); hit skips `shape()` but still needs the whole stitched mesh for triangle
  metadata (`prepareTriangleData`, `dynamicworld.cpp:299-300`).
- Coarser collision mesh: not supported. `DynamicWorld` takes an `asset::WorldMesh` and uses its materials and sector
  names (indoor detection, sounds, friction); feeding a different mesh is technically one argument
  (`world.cpp:153`) but every gameplay ray (`landRay`, `dropRay`, items, ragdolls, CharacterVirtual, navigation eye,
  arrows) would then hit the coarse surface while the renderer shows the fine one. UNVERIFIED: no code ties a triangle
  id between render and physics except via `order`/source ids (`worldchunksruntime.h:75-78`).
- Waypoint heights: `WayHeights::adjust` casts `landRay` straight down from the stored waypoint position, caches the
  hit (`OG/common/world/wayheights.cpp:304-316`; key = name + exact float position, :251-254); NPC simplified movement
  uses profiles sampled every 25 cm (`wayheights.h:34-45`). Live NPCs snap via `MoveAlgo::dropRay` / `landRay`
  (`OG/common/game/movealgo.cpp:424,796,1115,1157,1619`).
- Placed items: census settles items against the physics world and caches the result (`placedcensuscache.*`, key
  includes item-mesh vertices but static-world identity only through the content fingerprint, stress report table).
- Vob positions (trees, houses, vobs.d additions) are absolute from ZEN/JSON; `harbour_life.py` bakes heights offline
  from the world mesh at the time of authoring (`scripts/content/harbour_life.py` docstring :10-14).
- Navmesh (navbench only) hashes collision vertices/triangles (`common/physics/navmesh.cpp`).

**What breaks if the surface moves 10-50 cm**
- Raised ground: waypoints whose stored `pos` ends up below the new surface start their down-ray inside/below it; the
  ray misses or hits a lower floor (cave, cellar) and `groundPos` falls back to `point.pos` (`wayheights.cpp:311-312`)
  => NPC spawn/routine points inside terrain (UNVERIFIED how Jolt treats rays starting inside a back face).
- Lowered ground: vobs (trees, fences, rocks, ZEN static props, vobs.d layer items with baked heights) float; items
  from the census cache are stale until the content fingerprint changes (it does when `content/` changes).
- Chunk borders: `world_edit.py` forbids moving border vertices (`chunk_glb.py:198-216`), so a global surface change
  must be applied consistently across all 403 chunks or it fails the check.
- Doors/mobsi/ladders and scripted positions (`Wld_InsertNpc` at waypoints, FP spots) keep their absolute heights.

**Implication**: a denser or re-shaped terrain needs a re-snap pass for vobs, waypoints, freepoints and layer items
(offline tool), and ideally a collision mesh of its own (coarser than render) with an explicit tolerance budget.

## 6. Ray tracing and GI vs world geometry

**What exists**
- RT/GI off by default (`isRQuery=false`, `commandline.h:61-63`; `-gi` needs ray query, `shaders.cpp:73-74`).
- With RT on, landscape streaming is DISABLED and the whole landscape is packed once (`world.cpp:145-150`).
- BLAS: (a) `Landscape::init` builds one BLAS per landscape submesh (`OG/common/graphics/mesh/landscape.cpp:39-41`);
  (b) `RtScene` merges all landscape submeshes into two BLAS (solid, alpha-test) rebuilt whenever the landscape set
  changes (`OG/common/graphics/rtscene.cpp:245-275,292-298`); static meshes get one BLAS per submesh at mesh load
  (`staticmesh.cpp:152,267`), one TLAS instance per object (`rtscene.cpp:299-370`). Only Landscape and Static
  categories enter the scene (`rtscene.cpp:141-148`). TLAS is rebuilt every frame something is dirty
  (`cmd.buildTlas`, `rtscene.cpp:370`); the comment in `commandline.h:61` records 4-8 FPS indoors on M1 when it rebuilt
  on any move.
- Memory/build cost is logged (`RtScene::logBuild`, `rtscene.cpp:384-410`), no numbers in code.

**Missing**: BLAS per chunk (streamable), LOD/proxy geometry for rays, compaction, refit-only updates.

**Implication**: denser geometry raises BLAS memory linearly and the merged-landscape BLAS rebuild (seconds at x16,
UNVERIFIED) on any landscape change; with streaming forbidden under RT, RT and a denser streamed world are currently
mutually exclusive. A GI path would want coarse proxy geometry (HLOD tiles) for rays rather than full detail.

## 7. Backends

- macOS: native Metal through Tempest's own backend (`OG/lib/Tempest/Engine/gapi/metal/*`, metal-cpp; SPIR-V -> MSL via
  SPIRV-Cross with a disk cache `mtshadercache.*`), selected in `OG/game/main.cpp:58-67`. Not MoltenVK.
  Linux/Windows: Vulkan (`gapi/vulkan`), DX12 only on Windows with `-dx12` (`commandline.cpp:107-109`).
- Mesh/task shaders: Vulkan via `VK_EXT_mesh_shader` (`vdevice.cpp:629-637`); Metal on Metal3+Apple7
  (`mtdevice.mm:575-582`); engine default off on macOS (`commandline.h:65-69`).
- Tessellation: Vulkan if the feature exists (`vdevice.cpp:718`), forced off on Metal (`mtdevice.mm:548-549`).
- Compute and indirect: both backends implement `drawIndirect` and `dispatchIndirect`
  (`gapi/metal/mtcommandbuffer.cpp:859,898`; `gapi/vulkan/vcommandbuffer.cpp:612,874`); the whole draw path relies on
  compute culling + indirect draws on all platforms.
- Bindless: Metal needs Metal3 + argument buffers tier 2 (`mtdevice.mm:559-568`); ray query on macOS 12+ if supported
  (`mtdevice.mm:571-574`).
- VSM: on by default on Vulkan, off on Apple (`commandline.h:70-75`).

**Implication**: GPU-driven indirect + compute is portable; any LOD/foliage system should be compute-culled + indirect
(no mesh-shader dependence, no tessellation on Mac). Mac (M1 Pro) is GPU-bound already (shadows 5.5 ms, streaming
report table row "Mac упирается в GPU").

## 8. Content tools

- `scripts/content/world/world_edit.py` (`take`, `build`, `check`, `compare`): an edit = a Blender script with
  `edit(ctx)` applied in order to the ORIGINAL base chunk; output re-canonicalised by `chunk_canon.py` so untouched
  source triangles stay byte-identical; checks in `chunk_glb.py`: border vertices fixed (`:198-216`), no new open edges
  except declared (`:288-320`), vertex light unchanged on source corners, materials known, per-material triangles/area
  report (`:255-264,333-345`). Registry `data/content/world-edits.jsonl` (17 records). Limits (doc + code): one chunk per
  record, borders frozen, two-sided holes undetected, collision not checked, Blender 5.2 required, no LOD output.
- Vob layers `content/worlds/<W>/vobs.d/*.json` (`OG/common/world/voblayer.h:23-37`): add static meshes/lights/PFX
  emitters, remove/move non-saved static `zCVob` by id; files apply atomically; validated by
  `content-convert check-vob-layer`; `vobtrees.d` replaces `.ZEN` bundle slots.
- Placement scripts: `scripts/content/harbour_life.py` (heights from world mesh under the footprint, `on` stacking,
  tilt; writes a layer), `harbour_motion.py` (smoke/light anchored to props), `life_set.py`, `building_set.py`.
- `content-convert` subcommands (`scripts/convert/convert.cpp`): `world`, `world-data`, `chunks`, `meshes`, `textures`,
  `verify-world`, `verify-meshes`, `check-vob-layer`, `check-vob-tree`, `check-particles`, `resolve(-base)` and others;
  `stress-base --subdivide N --meshes M --texture-scale` in `scripts/convert/stressbase.cpp`, `stressmeshes.cpp`.
- Censuses/measures: `scripts/content-meshes.sh` -> `reports/content/2026-10-world-meshes` (instances, triangles,
  area, bbox per visual and family per world), `world-textures.cpp`/`2026-10-world-textures`, `building_survey.py`,
  `texture_districts.py`, `khorinis_map.py`, `mesh3d/prop-survey.cpp`, `chunk_glb.py` per-material triangle/area table.
  Missing: one command that reports triangles, area, materials and vob counts for an arbitrary region (polygon or chunk
  set) of world mesh + vobs + layers together (UNVERIFIED that none exists beyond these per-asset tools).
- Comparison clips/frames: `streaming-shots` (deterministic PNG pairs, `scripts/bench/streaming-flythrough.bench`),
  `scripts/perf.sh` places + `perf-shot`, `scripts/perf-content.sh A B` (Ц1 cost of two content sets: p50/p95,
  triangles, draw calls), bench `rec` clips, `capture-intent.sh`, `capture-city.sh`, `intent_cameras.py`, `remaster.py`.

## 9. Bottlenecks at 4-16x geometry (stress test + code)

| Stage (x1 -> x4 -> x16) | Code responsible |
|---|---|
| Chunk read 544 -> 2157 -> 8330 ms, whole world at start | `Resources::loadWorld` (`resources_base.cpp:174`) -> `readWorldChunks` (`worldchunksruntime.cpp:533-720`), cgltf parse of every GLB, stitch |
| Static physics warm 212 -> 778 -> 3030 ms, cold 828 -> 4679 ms; one body | `DynamicWorld::DynamicWorld` whole-world `PackedMesh PK_Physic` (`dynamicworld.cpp:221`), `PhysicVbo::shape` (`physicvbo.cpp:474-528`), `CollisionCache::read` (`dynamicworld.cpp:284-296`), `prepareTriangleData` (:299) |
| Near-ring pack 93 -> 316 -> 1230 ms, synchronous, also on teleport | residency callback in `WorldView::LandscapeStreaming` (`worldview.cpp:246-290`): `pack()` = `ChunkMeshSource::read` + `PackedMesh` on the main thread |
| Landscape view 192 -> 1441 ms | all landscape materials decoded before first frame (`world.cpp:172-182`), `Landscape` construction |
| Vobs 369 -> 1512 ms | `wobj.addRoot` loop (`world.cpp:288-291`), per-vob `ObjVisual` + `PhysicMesh` |
| Far publish p99 0.76 -> 1.81 ms, single publish 26.6 ms at w16 | one Far chunk per frame without a byte/time budget (`worldview.cpp:433-446`), `Landscape` ctor -> `VisualObjects::get` per submesh, linear `DrawBuckets::alloc` (`drawbuckets.cpp:43-61`), `commandId` scan (`drawcommands.cpp:186-192`), page upload |
| Landscape GPU pages 40 -> 90 -> 300 MiB; GPU total 1.04 -> 1.68 GB | Far ring = `zFar` 1 km (`chunkresidency.h:68`), no HLOD, `LandscapePages` (`staticmesh.cpp:12-15`) |
| Malloc 639 -> 839 MiB | stitched mesh lifetime, collision CPU mesh + triangle data kept (UNVERIFIED split) |
| Frame on RTX 3080 unchanged (CPU-bound: world tick 3.9, animation 2.4, encode 2.0 ms); Mac not measured, GPU-bound | `visibility_pass.comp` per cluster x view; shadows (Mac 5.5 ms) |
| Formats at x64 | 2^24 float ids (`worldchunks.cpp:25`), uint16 mesh wedges (`asset/staticmesh.h:10-15`) |

Also: texture memory breaks first (x2), ring-2 textures always full size (stress report item 1); derived caches
`.fracture`/`.convex-parts` keyed only by mesh name (stress report "Кэши игры").

## 10. Pointers toward LOD, HLOD, SDF, heightfields

- HLOD: planned only — step 1 item 8 of `docs/world-streaming.md` (4x4-chunk 256 m tiles built at conversion, ground via
  dual contouring of a distance field, second coarser level beyond 1 km, panorama checkpoints); "—" (not started).
  No code, flag or stub found (grep for `HLOD|hlod` in `OG/common`, `OG/game`, `scripts/` is empty).
- LOD: only hair strands (`visualobjects.cpp:833-880`, `BK_STRAND`); `is_lod` polygon flag of ZEN is carried in chunk
  `flags` (`worldchunks.cpp:33`, `asset/worldmesh.h:24`) and BSP node `lod` (`worldfromzenkit.cpp:934`), but nothing
  uses them for rendering. MRM progressive meshes are flattened to full detail.
- SDF: only in content scripts (`scripts/content/basement_bake.py:155`, texture bakes); destruction plan mentions local SDF
  craters (`docs/world-streaming.md` decisions, memory "destructible seamless world") — no engine code.
- Heightfields: only Recast's `rcHeightfield` inside navmesh building (`OG/common/physics/navmesh.cpp:472-493`); no
  Jolt `HeightFieldShape`, no terrain heightmap anywhere.
- Hooks that a LOD/HLOD system can reuse: `ChunkResidency` Near/Far/Outside with hysteresis and per-frame job budget
  (`OG/common/world/chunkresidency.*`), `StreamedVisual` interface (`OG/common/graphics/streamedvisual.h`),
  `LandscapePages`, texture screen-size mips (`texturescreendemand.*`, `texturelevels.*`), the `LARGE` small-cluster
  rejection in the HiZ pass (`visibility_pass.comp:205-213`).

---

## Ten gaps, ranked by how early they block a 4-16x denser, more detailed world

1. **Whole-world load path.** Every chunk is parsed and stitched into one `asset::WorldMesh` at load
   (`worldchunksruntime.cpp:533`, `world.cpp:141`); load +2.9 s at x4, +13.5 s at x16. Needs a binary per-chunk format
   and reading only Near at start.
2. **Physics = one body built from the render mesh** (`dynamicworld.cpp:221-316`). Grows 3-6x by x16; no coarser
   collision mesh, no per-chunk bodies (seam attempt #616 failed 2/12). Also gates every height/snap consumer (Q5).
3. **Texture memory and codecs.** Ring-2 textures always full size; DXT1/3/5 only (`pixmapcodecdds.cpp:51-60`). Breaks
   at x2 texture scale, before geometry (stress item 1); detailed materials need BC5/BC7.
4. **No distance LOD / HLOD / cull distance** for landscape or vobs (Q2, Q10); Far = 1 km covers 359/403 chunks, so
   GPU pages and clusters scale with total density (300 MiB pages at x16) and Mac (GPU-bound) gets every triangle.
5. **Synchronous Near packing and unbudgeted Far publication** (`worldview.cpp:246-290,433-446`): 1.2 s teleport
   hitch and 26.6 ms single publish at x16.
6. **Re-snap of everything placed on the ground** when the surface changes (waypoint down-rays from stored positions,
   absolute vob/layer heights, census caches; Q5). Any re-shaped terrain needs an offline re-snap tool first.
7. **No ground-cover/foliage system**: vegetation is per-object vobs with CPU wind (`visualobjects.cpp:823-831`),
   Jolt 65,536-body cap (`collisionworld_internal.h:144`) for collidable props; 100x density is impossible this way.
8. **Terrain material model**: one Gothic material per triangle, one UV, 36 B vertex without weights/tangents,
   no blending/detail/macro/triplanar/parallax (Q4). Detail without repetition needs a format + shader change.
9. **16-bit bucket/command ids with silent truncation and linear scans** (`drawbuckets.cpp:22,43-61`,
   `drawcommands.cpp:186-192`, `drawclusters.h:20-21`). Fine for one mesh per chunk, blocks LOD levels per chunk or
   many unique meshes; VSM 512k meshlet-page cap (`drawcommands.h:16`).
10. **RT/GI forbids streaming and rebuilds a merged whole-landscape BLAS** (`world.cpp:145-150`, `rtscene.cpp:265-275`).
    Not a blocker while RT is off, but any GI step on a dense world needs per-chunk BLAS or proxy geometry. Format
    ceilings (2^24 float ids, uint16 mesh wedges/65,535 own-GLB positions) only bite at x64 and come last.

Doc/code discrepancies noted: stress report "65 536 позиций" vs own-GLB loader 65,535 (`gltfmesh.cpp:29`);
`graphics-plan.md` "-ms на Linux по умолчанию" — code: default on for every non-macOS build incl. Windows
(`commandline.h:65-69`); landscape vertex light (`COLOR_0`) is documented as carried, and code confirms it is not used
for landscape shading (only cloth/checks).

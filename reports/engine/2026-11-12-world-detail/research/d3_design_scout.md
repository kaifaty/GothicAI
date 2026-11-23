# Step Д3 design scout (read-only, Codex, 2026-11-12)

Question: `scout-landscape-d3` — who needs the stitched world mesh, what keeps old saves loading, a binary chunk layout, several meshes per cell, a split into narrow tasks. Answer as returned; decisions are in `docs/landscape-plan.md`.

Verified against local `main` **413f1b7b315c**, matching local `origin/main`; no fetch, edits, builds, tests, or game launches. Paths below are repository-relative.

**1. Consumers and required identities**

| Consumer | What it actually needs | Can it avoid the stitched mesh? |
|---|---|---|
| `DynamicWorld` | Positions, vertex indices, material collision-disable/group/name. `PK_Physic` sorts by material grouping and remaps vertices; its triangle IDs are **physics-packed ordinals**, not original `order`. Features, lightmaps and polygon flags are unused here. `OpenGothic/common/graphics/mesh/submesh/packedmesh.cpp:691`; `OpenGothic/common/physics/dynamicworld.cpp:221` | Yes: consume prepared collision arrays or chunk inputs, retaining today’s single landscape/water bodies. |
| `PhysicVbo`, contact/ray queries | Positions, indices, material/sector segments, exact cyclic-duplicate relationships. Jolt userdata must resolve to the matching CPU triangle; subdivision preserves that association. `OpenGothic/common/physics/physicvbo.cpp:311`, `:474`; `OpenGothic/common/physics/collisionworld.cpp:762`, `:1170` | Yes, but shape **and metadata must share numbering**. Chunk-local IDs need an explicit translation. |
| `CollisionCache` | Currently stores Jolt shapes only; even a hit follows whole-mesh packing and then `prepareTriangleData`. Keys include base/content/world/flags plus serialization compatibility. `OpenGothic/common/physics/dynamicworld.cpp:221`, `:266`, `:297`; `OpenGothic/common/physics/collisioncache.cpp:65` | Extend it with the corresponding CPU collision/query data; a shape-only cache cannot eliminate the dependency. |
| World cloth | Material names/textures select triangles; source position identity joins connected islands across UV seams. Extraction copies features, flags and lightmaps. Surface ID is `0x80000000 | first stitched triangle`; visual ordinal follows island traversal. `OpenGothic/common/physics/worldcloth.cpp:61`, `:173`, `:221` | Use a cooked, world-level cloth bundle plus per-chunk exclusions. Independent per-chunk extraction could split cross-border islands. |
| Residency/chunk table | Cell coordinates, bounds, triangle counts. It does **not** need geometry or global triangle order. `OpenGothic/common/world/chunkresidency.cpp:25`, `:88` | Put these in the catalog. Preserve file ownership when edited geometry moves. Current ownership/bounds are reconstructed during stitching: `OpenGothic/common/worldchunksruntime.cpp:724`. |
| Landscape streaming | Today captures cloth-filtered GLB triangle ordinals and global material representatives, then rereads each GLB. Packing uses positions, normals, UVs and vertex/feature identity keys. `OpenGothic/common/graphics/worldview.cpp:209`, `:401`; `OpenGothic/common/graphics/mesh/submesh/packedmesh.cpp:807` | Yes: cooked selection/identity metadata and chunk-local arrays. Preserve equivalent identity keys and material representatives for packing parity. |
| Non-streaming drawing | Whole `PackedMesh`, including full-world bounds/materials. Selected for RT/GI, built-in benchmark, or disabled residency. Landscape packing ignores polygon flags/lightmaps and writes white vertex colour. `OpenGothic/common/world/world.cpp:149`, `:175`, `:194`; `OpenGothic/common/graphics/mesh/submesh/packedmesh.cpp:119` | Eagerly load **all chunks separately**. RT accepts multiple geometry descriptors before building its landscape BLAS: `OpenGothic/common/graphics/rtscene.cpp:256`. This requires render work. |
| Navigation, way heights, placed items | Navigation extracts transformed static collision triangles; its cache hashes ordered geometry arrays. Way heights query physics and cache name/exact-position keys. Neither consumes render source IDs. `OpenGothic/common/physics/dynamicworld.cpp:891`; `OpenGothic/common/physics/navmesh.cpp:52`; `OpenGothic/common/world/wayheights.cpp:251`, `:304` | Keep complete collision available. Preserve ordering initially to avoid invalidating navigation parity/cache checks. Item placement remains downstream of physics: `OpenGothic/common/world/world.cpp:302`. |

Other consumers are offline verification/conversion/stress tools, which may retain a whole-mesh reader: `OpenGothic/common/baseimport.cpp:139`; `scripts/convert/convert.cpp:202`; `scripts/convert/stressbase.cpp:197`. Bench probes read residency statistics rather than source geometry: `OpenGothic/common/testbench.cpp:1298`. Whole-world drawing bounds are another easily missed dependency: `OpenGothic/common/world/world.cpp:189`.

**2. Old saves**

**Verified in inspected serializers:** no saved world-mesh triangle, vertex, feature or chunk ordinal reference was found.

- Destruction saves vob identity/visual/origin and **fracture** piece/group/bond indices: `OpenGothic/common/physics/dynamicworld_fragments.cpp:267`.
- Movables save vob identity and transforms; stuck arrows save transforms and optional movable attachment identity: `OpenGothic/common/physics/dynamicworld_movables.cpp:501`; `OpenGothic/common/world/world.cpp:931`.
- Blood saves positions/normals and stain/body identities; body traces save their counter: `OpenGothic/common/world/world.cpp:772`, `:869`.
- Placed items use item-array indices and ZEN IDs; sectors save names: `OpenGothic/common/world/worldobjects.cpp:462`; `OpenGothic/common/world/world.cpp:452`.
- Cloth is transient; the existing reload check explicitly expects rest state: `OpenGothic/common/testbench_cloth.cpp:591`.

Therefore preserve world/sector/vob identities, transforms, serialization schemas and fracture numbering—not render triangle order. Preserve cloth IDs/traversal separately for runtime/bench compatibility. **Actual old-save compatibility remains untested.**

**3. Proposed binary format — choose (a), cooked beside GLB**

Proposal: uncompressed, little-endian `.wcb`, engine coordinates in centimetres, explicit layouts rather than dumped C++ structs:

| Section | Concrete representation |
|---|---|
| Header | Magic/version/endian, header/file sizes, cell `i32×2`, counts `u32`, bounds `f32×6`, source GLB byte length `u64`, SHA-256, cooking/catalog identity. |
| Directory | Fixed section descriptors: type/stride/count `u32`, offset/length `u64`; aligned sections, bounds-checked once. |
| Positions | 16 bytes: position `f32×3`, resolved identity `u32`. |
| Features | 28 bytes: UV `f32×2`, light `u32`, normal `f32×3`, resolved identity `u32`. |
| Triangles | 44 bytes: position indices `u32×3`, feature indices `u32×3`, material `u32`, original order `u32` (`UINT32_MAX` = added), legacy stitched rank `u32`, lightmap `i32`, flags `u16`, sector `i16`. Array index is the GLB-local ordinal. |
| Materials | Fixed records containing all current material fields: integer enums/boolean mask, RGBA bytes, floats, and offset/length string references; no JSON. Field inventory: `OpenGothic/common/asset/material.h:16`. |
| Auxiliary | String pool and draw/cloth selection lists. Catalog holds per-mesh bounds/counts/material usage and cloth-bundle identity. |

One read or mapping exposes typed spans; validation remains necessary.

**Crucial:** preserve *resolved* identities, not merely `_VERTEX/_FEATURE`. Today moved/split source copies receive appended IDs based on values, and edited triangles follow ordered originals: `OpenGothic/common/worldchunksruntime.cpp:567`, `:667`. Cook the resolved base-plus-overrides catalog deterministically.

Choose **(a)** because first-load cache generation cannot meet the cold-start goal. Keep GLB as authoring/fallback; fallback should decode individual chunks, not restore runtime stitching. Check selected GLB hashes lazily when used; hashing every GLB at startup defeats Near-only reads.

For (a), converter/import/stress writers emit binaries; `world_edit.py` publishes the GLB, binary and refreshed catalog identity together. Its current publication point is `scripts/content/world/world_edit.py:252`, `:276`. Python geometry readers remain GLB-based; only manifest traversal needs normalization for multiple meshes (`scripts/content/world/world_census.py:78`). For **(b)**, Python geometry tools need no cache-writing changes, but the first engine load still parses/cooks everything required.

**4. Several meshes per cell**

Proposal: accept existing string entries unchanged; additionally accept:

```json
{"cell":[0,-1],"meshes":[
  {"file":"chunks/0_-1.glb","role":"draw","level":0},
  {"file":"chunks/0_-1_collision.glb","role":"collision"}
]}
```

Normalize strings to draw level 0. Keep **one table entry per cell**, containing mesh descriptors; never duplicate cell entries—the current index and landscape callback select one entry per coordinate: `OpenGothic/common/world/chunkresidency.cpp:25`; `OpenGothic/common/graphics/worldview.cpp:230`. Catalog draw bounds/counts must exclude collision-only geometry.

**5. Ordered tasks**

“Independently landable” below means equivalent behaviour, although timings may improve. Extend existing contracts where needed.

| # | Narrow task / files touched | Done when; existing check | Must not change / independent landing | Owner |
|---|---|---|---|---|
| 1 | Normalize legacy/multi-mesh manifests and cell metadata: `worldchunks.h`, `worldchunksruntime.*`, `worldchunks.cpp` | Old fixtures produce identical tables; `gothic-worldbase-contract` (`OpenGothic/tests/worldbase_contract.cpp:453`). | No LOD/collision-role activation. Yes. **Д4/Д6 preparation.** | Executor |
| 2 | Binary codec and converter emission: new codec pair, converter/import writers | Binary/GLB round trips match; extend converter self-test (`scripts/convert/convert.cpp:194`). | No runtime selection change. Yes. | Executor |
| 3 | Binary-backed `ChunkMeshSource`, retaining current startup temporarily | Exact selected triangles/IDs/materials match existing contract (`OpenGothic/tests/worldbase_contract.cpp:515`). | Same packing/order/fallback. Yes; partial speedup only. | Executor |
| 4 | Cook cloth bundle/exclusions: `worldcloth.*`, converter integration | Same islands, pins, IDs, traversal and original fallback mesh; `gothic-worldcloth-contract` (`OpenGothic/tests/worldcloth_contract.cpp:279`). | No simulation or cloth appearance changes. Yes. | Executor |
| 5 | Collision-input builder from chunk records: `dynamicworld.cpp`, packing helper | Identical physics inputs, rays/materials/sectors/navigation; `gothic-wayheights-contract --collision` (`OpenGothic/tests/wayheights_contract.cpp:372`). | Same single bodies, full-resolution collision. Yes. **Д4 foundation.** | Executor |
| 6 | Cache matching CPU query metadata: `collisioncache.*`, `physicvbo.*`, `dynamicworld.cpp` | Cold/warm equality, corruption fallback; same collision contract (`OpenGothic/tests/wayheights_contract.cpp:409`). | No geometry simplification; version cache. Yes. | Executor |
| 7 | Integrate content publication/catalog regeneration: `world_edit.py`, census manifest traversal | Existing edit/census/walk tests pass (`scripts/content/world/test_world_edit.py:295`; `test_world_census.py:116`; `test_world_walk_check.py:63`). | GLB authoring/check semantics unchanged. Yes. | Claude: authoring-tool integration |
| 8 | Switch streaming startup to catalog + Near reads; remove stitched ownership arrays: `worldbase.cpp`, `world.cpp`, `worldview.*` | **Main 8330→≤544 ms candidate:** load probe, both E2Es, chunk parity and lifetime contract (`OpenGothic/tests/mesh_gltf_test.cpp:1701`). | Same rings, material representatives, bounds, Far behaviour; no Д5 scheduling changes. Yes after dependencies. | **Claude: render code** |
| 9 | Remove whole-`WorldMesh` dependency from eager RT/GI/off/benchmark path: `world.cpp`, `worldview.*`, landscape integration | Same complete geometry; mesh lifetime contract, off-path load probe, `check_walk`/`load_rewind`. | Preserve complete RT landscape; no Д7 streaming. Independently landable. | **Claude: render code** |
| 10 | Acceptance only | Same-machine ×1/×16 cold/warm probes; E2Es; pre-change saves via existing legacy-load bench (`scripts/bench/cloth-legacy-load.bench:1`). | No benchmark changes masking regressions. | Executor; visual review Claude |

**6. Corrections, risks and unmeasured points**

The audit’s sections 1/5/9 still describe the principal dependency correctly. Two qualifications: landscape colour is discarded during packing, not merely unused by shaders (`packedmesh.cpp:136`); ranked gap 3’s “ring-2 always full” is outdated—screen mip demand now defaults on (`OpenGothic/common/resources_textures.cpp:583`). The landscape plan’s opening “only Д1 done” is stale relative to its implemented Д2 tool (`scripts/content/world/world_walk_check.py:318`).

**Before freezing the format, measure:** parse versus sorting/deduplication cost; binary bytes/page faults; Near triangle count; cross-cell cloth; edited-ID reconciliation; cold/warm collision metadata cost. Record `mesh_chunks_read`, `physics_static` and total load, avoiding mere timing relocation (`OpenGothic/common/worldbase.cpp:75`; `OpenGothic/common/world/world.cpp:246`). Binary integer IDs also do not remove the existing GLB authoring float-ID ceiling (`OpenGothic/common/worldchunks.cpp:95`).

Not inspected: actual stress binaries, saved archives, remote machines, backend execution or visual results. `load_rewind` creates a current-format save, so it alone cannot prove legacy compatibility (`server/src/gothic_ai/scenarios/load.py:22`).

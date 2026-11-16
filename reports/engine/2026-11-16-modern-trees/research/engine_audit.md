# Engine audit for modern trees (read-only, 2026-11-16)

Worktree `focused-banach-eb3fa7`, `main` at `407f16fcd`. Nothing was built or run. Paths are relative to the repo root;
`OG/` = `OpenGothic/`. Line numbers are of this commit. "Not found" means a search of `OG/common`, `OG/shader`,
`OG/game` and `scripts/` gave nothing. "Old audit" = `reports/engine/2026-11-12-world-detail/research/engine_audit.md`;
facts taken from it without re-reading the code are marked so. Two of its statements are out of date and are corrected
here: ray tracing is no longer off everywhere (section 7), and landscape streaming now works with ray tracing
(`docs/world-streaming.md:7`).

The goal: replace the 2002 trees (vob meshes of 75–350 triangles and crown "shells" in the world mesh) with trees of
real trunk and branch geometry, crowns of leaf cards or modelled leaves, 3–4 LODs, far impostors, wind, light through
leaves, shadows and collision.

What the base holds today (read from the Mac base cache `worlds/NEWWORLD/vobs.json` and `world.json`):

- 1 493 tree vobs (visual name with `TREE` or `TANNE`) over 16 visuals; the most used are `NW_NATURE_TANNE_75P` (389),
  `OW_NATURE_SMALLTREE_01` (286), `NW_NATURE_TANNE_100P` (209), `NW_NATURE_SMALLTREE_79P` (130),
  `NW_NATURE_BIGTREE_356P` (100). 1 086 of them have `cd_dynamic` (collide), 233 have `anim_mode` `WIND`.
- 1 652 bush vobs; 259 of them windy, 15 collide.
- Crown shells in the world mesh: `NW_NATURE_TREE_04` 29 151 triangles, `NW_NATURE_TREE_NEEDLE_03` 39 038,
  `NW_NATURE_BRANCH_01` 26 440; baked trunks `NW_NATURE_BARK_04` 56 123
  (`reports/engine/2026-11-12-world-detail/research/world_census.md:23`).

---

## 1. How a static vob mesh is drawn

- **One vob = one object per submesh.** `ObjVisual` loads the mesh by name (`Resources::loadMesh`, shared between all
  vobs of that visual) and calls `world.addStaticView` (`OG/common/graphics/objvisual.cpp:350-366`). Each submesh
  becomes a `VisualObjects::Object` with its own instance slot and **one cluster for the whole submesh**
  (`DrawClusters::alloc(const Bucket&, …)`, `OG/common/graphics/drawclusters.cpp:39-55`: `meshletCount` = all meshlets
  of the submesh, radius = the mesh's conservative radius).
- **Batching.** A bucket is a (mesh, material) pair (`DrawBuckets::alloc`, `OG/common/graphics/drawbuckets.cpp:43-62`).
  All copies of one tree share its buckets. There is no per-object draw call: `visibility_pass.comp` appends
  (instance, meshlet, bucket) to a payload and bumps the instance count of an indirect command
  (`OG/shader/materials/visibility_pass.comp:219-236`); one indirect draw or mesh-shader dispatch per command
  (`OG/common/graphics/drawcommands.cpp:465-466`).
- **Meshlets.** 64 vertices and 64 triangles per meshlet (`OG/common/graphics/mesh/submesh/packedmesh.h:23-25`). Mesh
  shaders are on by default except on macOS (`OG/common/commandline.h:73-77`); without them each meshlet is one
  instance of 192 vertices (`drawcommands.cpp:266`). The mesh shader culls back faces per triangle only
  (`OG/shader/materials/main.vert`, `gl_CullPrimitiveEXT`); it has no per-meshlet frustum or cone test.
- **GPU culling.** Per cluster: frustum sphere test, then projected box and HiZ test in the main view
  (`visibility_pass.comp:176-217`). Because an object's cluster is the whole submesh, **a tree is all or nothing**: if
  any part of its bounding sphere passes, every meshlet of that submesh is drawn. The landscape, by contrast, has one
  cluster per meshlet (`drawclusters.cpp:20-37`).
- **Small-object rejection** exists only in the HiZ occluder pass: an object under 16 px² of the HiZ texture is not an
  occluder (`visibility_pass.comp:203-211`). The main and shadow views draw it at any size.
- **Per-instance data**: a 4×3 matrix, `fatness`, `animPtr`, `deformPtr`, `deformCount` (`OG/shader/scene.glsl:105-111`).
  No per-instance colour, phase, random number or LOD index. Strands reuse `fatness` for their LOD width
  (`OG/shader/materials/vertex_process.glsl:328`).
- **Draw distance.** `Camera::zFar` is a fixed 100 000 cm (`OG/common/camera.cpp:168-171`). Vob visuals are created in
  the Near and Far rings of 64 m chunks and released Outside (`OG/common/graphics/worldview.cpp:92-104`); Far reaches
  `zFar`. `zCZoneVobFarPlane` is ignored on purpose (`OG/common/world/objects/vob.cpp:327-329`).
- **How many instances it is built for.** Many copies of few meshes are cheap in buckets and draw calls. The limits
  are elsewhere:
  - bucket and command ids are 16 bits and truncate silently (`drawbuckets.cpp:22`, `drawclusters.h:20-21`);
    bucket allocation is a linear scan (`drawbuckets.cpp:44-48`);
  - every instance is a full vob with its own CPU object, cluster, instance slot and residency entry;
  - Jolt is initialised for 65 536 bodies (`OG/common/physics/collisionworld_internal.h:144`);
  - VSM takes at most 512 × 1024 meshlet–page pairs a frame (`OG/common/graphics/drawcommands.h:16`); overflow is
    logged, not handled (`OG/common/graphics/renderer_vsm.cpp:196-197`);
  - an own GLB may have at most 65 535 positions and 65 535 wedges per material (`OG/common/gltfmesh.cpp:475-476`,
    `docs/own-content-meshes.md:31`).
  The tested scale: 1 848 tuft objects cost nothing measurable (`docs/landscape-plan.md:104`); 11 546 static vobs is
  today's NEWWORLD (old audit).

## 2. LOD

- **Discrete LOD per mesh: not found.** No LOD field in `meshes/meshes.json` (only `glb`,
  `OG/common/resources_base.cpp:523-527`), none in the vob layer. MRM progressive meshes are loaded at full detail (old
  audit).
- **Screen-size culling: not found** for the main and shadow views (section 1).
- **Cross-fade or dither between levels: not found.**
- **One working precedent: strands.** `VisualObjects::preFrameUpdateStrands`
  (`OG/common/graphics/visualobjects.cpp:838-881`) computes the size of a pixel at the object, picks a level with
  hysteresis of one step, and writes a smaller `meshletCount` into the object's cluster, so only the first meshlets
  of the list are drawn. The shadow view shortens the list again in the culling shader
  (`visibility_pass.comp:225-231`).
- **The world manifest** can name several meshes per chunk (`draw level N`, `collision`:
  `OG/common/worldchunkmanifest.cpp:31,89`), but only level 0 is read (`docs/landscape-plan.md:13`). That is for the
  landscape, not for vobs.

Where LOD would plug in:

1. **CPU, as strands do**: per object per frame, change `firstMeshlet`/`meshletCount` of its cluster
   (`drawclusters.h:17-25`). It needs the LOD levels stored as consecutive meshlet ranges of one mesh, and one bucket.
   Cost: a CPU loop over all trees every frame.
2. **GPU, in `visibility_pass.comp`**: the shader already projects the instance's box (`projectInstance`) and knows the
   bucket. A LOD table per bucket (ranges of meshlets and switch sizes) would let it choose the range and skip objects
   under a pixel size. This keeps the CPU out and works for every view. `BucketGpu` is 128 bytes and its free fields
   are taken (`OG/common/graphics/drawbuckets.h:91-123`), so the table needs a buffer of its own.
3. Cross-fade needs a per-instance fade value that the pixel shader can read; the hashed alpha threshold of
   `main.frag:947-964` is the natural place to add it.

Ray tracing and collision ignore both mechanisms: the BLAS is per submesh slice (section 7) and the Jolt shape is per
mesh (section 8).

## 3. Alpha-tested materials

- **The test.** Original materials and the landscape: fixed threshold 0.5 (`OG/shader/materials/main.frag:947-964`,
  discard at `:978-981` and `:1013-1016`). Own glTF `MASK` materials under TAA get **hashed alpha**
  (`BK_HASHED_ALPHA`, `drawbuckets.cpp:376-379`): a threshold per pixel and frame from interleaved gradient noise,
  stepped by the golden ratio over 64 TAA frames, shifted by a hash of the texture cell so two cards over one pixel
  add their coverage. Switch: `-hashalpha 0` (`commandline.h:91`). Shadows and frames without TAA keep 0.5.
- **Alpha-to-coverage: not found.** The G-buffer has no MSAA.
- **Depth prepass.** There is no full prepass. `Renderer::drawHiZ` draws large occluders into the main depth before
  the G-buffer (`OG/common/graphics/renderer.cpp:945-949`, `:1129-1133`); alpha-tested ones take part, and a
  hashed-alpha material writes there only texels that no threshold drops (`main.frag:957-958`). The G-buffer pass tests
  `LEqual` and writes depth (`OG/common/graphics/shaders.cpp:501`). So leaf cards that are not HiZ occluders are shaded
  with full overdraw, front to back by chance.
- **One-sided: confirmed.** Every material pipeline culls with `CullMode::Front` (`shaders.cpp:500`); alpha-tested
  materials get no exception. The only `NoCull` is the VSM pass with mesh shaders, which then culls per triangle
  itself (`shaders.cpp:510-513`). glTF `doubleSided` is not read by the loader (not found in `gltfmesh.cpp`). Today's
  answer is in content: a card is added twice with opposite winding and the same normals
  (`scripts/content/glb_cards.py:1-8`).
- **Mips and coverage.** Two tools exist. `scripts/content/glb_cards.py:90-114` (`write_cutout`) writes DXT5 where
  alpha is the texel's coverage and each mip averages it, so coverage is kept at every distance — made for hashed
  alpha. `scripts/content/alpha_mips.py` makes hard cut-out mips with an alpha gain that restores the coverage of mip
  0 when a mip loses more than 10 points — for the fixed 0.5 threshold; only `test-material.py:249` uses it. The
  engine itself does no coverage correction when it loads or drops mips (not found).
- **Texture formats**: DXT1/3/5 only (old audit, `pixmapcodecdds.cpp`).
- **Overdraw.** No sorting, no visibility buffer. `vegetation_scatter.md:76` gives 8–15× shading overdraw for dense
  card forests elsewhere; not measured here. An overdraw debug state exists in code but is a compile-time `static bool`
  (`shaders.cpp:540-547`).

## 4. Shading of foliage

- **What `foliage` gives** (`materials.json`, parsed at `OG/common/resources_textures.cpp:319-346`; numbers in
  `OG/common/graphics/foliagewind.h:24-31`): `wind` 0…4, `flutter` 0…4, `translucency` 0…1, `anchor` (one value,
  `line`). The bucket gets `BK_FOLIAGE` only for a static, non-morph mesh whose material has its own maps
  (`drawbuckets.cpp:303-318`).
- **Wind weight source: position only.** No vertex colour, no UV, no extra attribute. The weight is the vertex's
  distance from the mesh's local X axis divided by the largest such distance of the mesh ("reach")
  (`OG/shader/materials/foliage_wind.glsl:86-95`). Section 5 explains why this fits a tuft and not a tree.
- **Translucency.** `main.frag:610-619` sets bit 6 of the G-buffer hints and writes the strength into the metal
  channel. Only the sun shines through: `foliageThrough` is a view-dependent lobe, strongest when the eye looks toward
  the sun through the leaf, tinted `(0.75, 1.15, 0.32)` (`OG/shader/lighting/foliage_light.glsl:11-20`,
  `direct_light.frag:363`). Lamps, sky light and GI probes treat the leaf as a matte surface. There is no thickness
  map, no transmission colour map, no per-material tint.
- **Vertex attributes of an own GLB**: `POSITION`, `NORMAL`, optional `TEXCOORD_0`; nothing else
  (`gltfmesh.cpp:449-482`). `COLOR_0`, `TANGENT`, `TEXCOORD_1` are not read. The mesh wedge holds normal, UV and a
  position index (`OG/common/asset/staticmesh.h:11-15`).
- **The GPU vertex has four free bytes.** `Resources::Vertex` is position, normal, UV and a `uint32 color`
  (`OG/common/resources.h:84-89`). For a static mesh the packer writes `0xFFFFFFFF` there
  (`OG/common/graphics/mesh/submesh/packedmesh.cpp:136`, `:215`), and object shaders do not read it (`MAT_COLOR` needs
  `VT_COLOR`, `OG/shader/materials/materials_common.glsl:88-90`). Strands already carry their own data in this field
  (`vertex_process.glsl:323-325`). So four bytes per vertex can carry tree data with no growth of the vertex; the
  loader, `MeshWedge` and the base mesh format need the new field.
- **Custom normals survive.** The loader takes `NORMAL` as written (after the node transform and the X mirror) and
  the packer copies it (`gltfmesh.cpp:475-483`, `packedmesh.cpp:131-133`). The tuft probe confirmed it in the game
  (`docs/landscape-plan.md:104`). Spherical crown normals will reach the light. Two cautions: the packer welds
  vertices by (position, wedge), so a card added twice keeps its two normals; and normal maps use a cotangent frame
  from screen derivatives, with no stored tangents (`main.frag:399`), which follows the vertex normal — a note at
  `main.frag:537` says the frame turns over where the vertex normal looks away from the face the eye sees.
- **Per-vertex AO: not found.** AO comes from the ORM texture, SSAO and, with GI, the probes and the 25 cm contact
  term (`docs/graphics-plan.md:19`).

## 5. Wind

What the wind function has:

- **Scene**: direction, pressure, an "it blows" flag, phases of two travelling waves and two flutters, the swing's
  phase — for this frame and the previous one (`foliage_wind.glsl:10-13`, set in `sceneglobals.*`).
- **Place**: the object's origin (`obj.mat[3]`). A hash of its x and z gives each object its own swing rate, beat and
  flutter step (`foliage_wind.glsl:77-83`, `:117-123`). The gust front arrives by the origin's z
  (`foliageFrontAt`, `:63-75`, called at `vertex_process.glsl:253`).
- **Mesh**: `foliageReach`, one number per bucket.
- **Vertex**: its local position only.
- **No per-instance phase attribute, no pivot per leaf or branch, no hierarchy.**

Why the present rule does not fit a tree:

- The pivot of a vertex is the point of the local X axis over it (`pivot = (p.x, 0, 0)`); the arm is `(0, p.y, p.z)`.
  The mesh's X axis is the line of roots along a wall. A tree's root is a point and its trunk runs along Y, so every
  vertex of the crown would swing about the X axis: the tree would rock as one plate in the YZ plane.
- "Into the rock" is the local −Z: wind from that side is turned along X (`foliage_wind.glsl:102-104`). A tree has no
  wall.
- The lean is capped at tan 0.3093 for the whole mesh (`:19`), and the weight is one ramp from root to tip — one
  level, not trunk, branch and leaf.
- The legacy whole-object wind (`anim_mode`, CPU shear per object per frame,
  `visualobjects.cpp:820-828`, `OG/common/physics/worldwind.h`) still applies to the 233 windy tree vobs if their
  visual is replaced through `meshes.json`, and such vobs get no collision (section 8).

What a tree needs in addition:

| Level | Data it needs | Where it can live |
|---|---|---|
| Trunk sway | height above the root, a phase per tree | position (`p.y / height`) and the place hash: both exist |
| Branch sway | which branch, the branch's pivot, distance along it | not available. Options: 4 free bytes of `Vertex.color` (e.g. branch phase 8 bit, distance along the branch 8 bit, leaf weight 8 bit, 8 bit spare or AO); or a small pivot table per mesh indexed by a byte in `color` |
| Leaf flutter | weight within the card, a phase per card | weight from UV (v across the card) or a `color` byte; phase from a hash of the pivot |
| Gust front | the tree's place | exists (`foliageFrontAt`) |

A second `anchor` mode is the declared extension point: the record already has the field, the header says a second
mode has no code (`reports/engine/2026-11-14-living-foliage/README.md:55`). The same function serves the main view,
the shadow passes and the previous frame for motion vectors (`vertex_process.glsl:246-256`, `:390-401`), so a new mode
gets shadows and TAA motion for free. The culling sphere must grow by the travel of the new mode
(`FoliageWind::travel`, used at `drawclusters.cpp:44-45` and `drawbuckets.cpp:310-313`).

## 6. Shadows

Techniques present:

| Technique | Where | Default |
|---|---|---|
| Virtual shadow maps, sun and lamps | `OG/common/graphics/renderer_vsm.cpp`, `OG/shader/virtual_shadow/*` | on, except Apple (`commandline.h:79-84`) |
| Two cascaded maps 2048 | `drawShadowMap` (`renderer.cpp:958`) | the fallback, and the Mac's default |
| Ray-query sun shadow beyond the second cascade | `calcRayShadow`, `OG/shader/lighting/direct_light.frag:204-214`, `:234-238` | with ray tracing |
| Screen ray of lamps for small shadows | `OG/common/graphics/lampcontact.h`, `lamp_contact.glsl` | on where lamp shadows are soft, off on Metal (`lampcontact.h:10-12`) |
| Screen ray of the sun over grass | `OG/common/graphics/grasscontact.h:10-21` | on, off on Metal |
| Short ambient occlusion under GI probes | `OG/common/graphics/contactao.h` | on with GI |

- **Alpha-tested geometry in shadows.** An alpha-tested material has shadow pipelines with the texture
  (`_d_at`, `_v_at`: `shaders.cpp:596-604`; `Material::isTextureInShadowPass`,
  `OG/common/graphics/material.cpp:211-213`). Threshold 0.5, no hashing (`main.frag:961-963`). The sun's pass culls one
  side as the main view does, so a leaf card needs its twin for a shadow from both sides.
- **Wind in shadows.** The shadow passes run the same vertex function, so the shadow follows the leaf
  (`vertex_process.glsl:246-256`; `main.vert:130-131` per the foliage note).
- **Caching.** VSM clears its page table, marks pages from the screen and renders them anew every frame
  (`renderer_vsm.cpp:67`, `:95`, `:162-166`, per the foliage note). There is no static page cache, so moving leaves
  invalidate nothing — and every tree in the sun's pages is rendered every frame. `commandline.h:80` records that the
  Mac's cost came "without the static cache".
- **Foliage and VSM marking.** `vsm_mark_pages.comp` marks a shadow page for a leaf turned away from the sun too, so
  the through-light can be cut by a shadow (`living-foliage/README.md:74`).
- **Cost notes.** Tufts: `VSM-rendering` 0.26 ms with or without wind on RTX 3080 at 1080p, differences within noise
  (`living-foliage/README.md:186-197`). Grass casts no shadow (`grass-system/README.md:350`). Mac: VSM is 7–12 ms GPU
  dearer than cascades on M1 Pro at 75 % (`docs/graphics-plan.md:20`). No number exists for a dense card canopy in VSM;
  the 512k meshlet–page cap (section 1) is the known risk, since a tree is one cluster of all its meshlets and is
  repeated for every page it touches.

## 7. Ray tracing and GI

- **Default.** On for a discrete NVIDIA RTX on Vulkan, with GI probes; off on the Mac, AMD, Intel
  (`OG/common/graphics/raytracingrule.cpp:22-34`, `commandline.h:69-72`). Order of switches: `-rt`, `OPENGOTHIC_RT`,
  `[INTERNAL] rayTracing`.
- **BLAS.** One per submesh of a static mesh, built at mesh load from the rest pose
  (`OG/common/graphics/mesh/submesh/staticmesh.cpp:168-170`); strands are skipped (`:168-169`). Shared by all
  instances of the mesh.
- **TLAS.** One instance per object-submesh with its matrix (`visualobjects.cpp:341-363`, `rtscene.cpp:318-337`). Only
  the Landscape and Static categories enter (`rtscene.cpp:201-207`): **movable and animated objects are not in the
  scene**. Landscape chunks enter with their own BLAS as they are published.
- **Alpha test.** An alpha-tested instance is flagged non-opaque and gets the mask `CM_Transparent`
  (`rtscene.cpp:50-53`, `:328-329`). There are no hit shaders: it is ray query, and the any-hit work is a loop in the
  shader — `rayQueryProceedAlphaTest` samples the albedo at mip 0 for every candidate and confirms at alpha > 0.5
  (`OG/shader/lighting/rt/rt_common.glsl:138-158`). The loop also **skips back faces** of candidates (`:150-151`), and
  all rays cull back faces (`RayFlagsShadow`, `:24`; probes: `probe_trace.comp:63`, comment "CullBack due to
  vegetation"). So a single card is one-sided for rays as well; the twin card covers it.
- **Probes and foliage.** A probe ray that stops on a non-opaque hit sends a second ray that ignores all non-opaque
  geometry; if that one reaches the sky the sample gets the sky bit (`probe_trace.comp:49-58`, `:85`). So alpha-tested
  canopy is partly see-through for sky light in the probes by construction.
- **`foliage` in rays.** The BLAS is the mesh at rest: rays see leaves that do not move
  (`visualobjects.cpp:349-350`). GI treats the leaf as matte; no through-light
  (`living-foliage/README.md:237`, written when ray tracing was off by default; with the new default this limit is
  live on RTX). The far ray shadow of a leaf stands still while the VSM shadow near the camera moves.
- **No LOD or proxy for rays: not found.** A tree of leaf cards puts every card into the BLAS with the alpha loop on
  each candidate. Cost not measured; `vegetation_scatter.md:171` names alpha any-hit as the usual bottleneck.
- **Grass is not in the ray scene** (`grass-system/README.md:350`).

## 8. Collision and sight

- **Shape of a vob.** `ProtoMesh` builds one `PhysicMeshShape` from the same packed mesh that is drawn
  (`OG/common/graphics/mesh/protomesh.cpp:63-73`): a triangle mesh of every submesh whose material does not have
  `disable_collision` (`OG/common/physics/physicmeshshape.cpp:11-20`).
- **When a vob gets a body.** `cd_dynamic` set, visual shown, **not windy**, not cloth
  (`objvisual.cpp:334`, `:355`, `:369-371`). Windy vobs have no collision at all.
- **Own GLB.** Same path: the shape is the drawn geometry. `disable_collision` is true only for water
  (`gltfmesh.cpp:156`). **No separate collision mesh, no capsule, no primitive shapes, no `extras` flag to exclude a
  material: not found.** A GLB tree placed on a colliding vob would give Jolt a mesh shape of every leaf card. The vob
  layer has `"collision": "none" | "mesh"` per added object (`OG/common/world/voblayer.cpp:387-395`) — all or nothing.
- **Workable today without engine work:** split the tree into two visuals and two vobs — a trunk GLB with
  `collision: mesh` and a crown GLB with `collision: none`. An invisible collider vob is not possible: a body is made
  only for a shown visual (`objvisual.cpp:369`).
- **Body budget**: 65 536 Jolt bodies (section 1). 1 086 colliding trees today.
- **NPC sight.** `Npc::canSeeNpc` casts rays to the target's middle and head (`OG/common/world/objects/
  npc_perception.cpp:426-440`) through `DynamicWorld::ray`, which hits the categories landscape, static object, movable
  query and small-vob query (`OG/common/physics/dynamicworld.cpp:363-365`,
  `OG/common/physics/dynamicworld_internal.h:25-27`). So sight is blocked by exactly what collides: tree vobs with
  `cd_dynamic` block it with their whole mesh, leaf cards included; windy trees and bushes do not block it. Sound
  occlusion uses the same mask (`dynamicworld.cpp:430-432`).
- **World-mesh crowns do not collide.** In `world.json` the materials `NW_NATURE_TREE_04`, `NW_NATURE_TREE_NEEDLE_03`,
  `NW_NATURE_TREE_NEEDLE_02`, `NW_NATURE_BRANCH_01` have `disable_collision: true`; `NW_NATURE_BARK_04` has `false`.
  The physics pack skips disabled materials (`packedmesh.cpp:703-704`).

## 9. Forest shells in the world mesh

Tooling:

- `scripts/content/world/world_edit.py` (`build` for one chunk, `build-region` for a rectangle of chunks as one
  surface, `check`, `compare`, `cook`): an edit is a Blender script with `edit(ctx)`; the result is written back in
  the converter's form with untouched triangles byte for byte (`world_edit.py:1-40`).
- Deleting faces is an ordinary edit: existing edits delete (`roof_holes.py`, `harbour_stairs.py`,
  `landscape_rock_sdf.py`). The material of a face is known by its Gothic name (`world_bpy.py:45-47`). **A ready edit
  "remove all triangles of materials X in the region": not found** — it is a short new script.
- Checks that a removal meets (`scripts/content/world/chunk_glb.py`): new open edges must be declared with
  `ctx.allow_open(faces, reason)` (`chunk_glb.py:288-322`); border vertices are fixed in `build` and free inside a
  region; triangles and area per material are reported. Shells are mostly open, two-sided sheets, so most of their
  edges are open in the base already; the declared set will be the edges where a shell met kept geometry.
- `scripts/content/world/world_census.py <WORLD> --chunks/--rect` counts triangles and area per material before and
  after; `world_classes.py` holds the "vegetation" class.
- `scripts/content/world/world_walk_check.py` compares walkable ground, heights under waypoints and vobs between base
  and edited content (`docs/landscape-plan.md:14`).
- The set is cooked with the edit (`world_cook.py`); the world loads edited chunks from the content folder.
- Limits: Blender 5.2; a chunk belongs to one record per set; the 2^24 id limit of the world.

What removal does:

- **Collision and sight: nothing for the crowns** (they do not collide, section 8). Removing `NW_NATURE_BARK_04`
  trunks removes real collision and sight blockers; `world_walk_check.py` does not look at that — it checks ground
  only — so trunks need new colliding trees in the same places.
- **Darkness under the canopy** comes from four things, all fed by the shell triangles:
  1. the sun's shadow: the shells are alpha-tested landscape, drawn into VSM or the cascades with their texture
     (section 6);
  2. with ray tracing, the far sun shadow and the GI probes: the shells sit in the landscape's alpha-tested BLAS
     (`rtscene.cpp:209-211`);
  3. sky visibility for fog (`fog_sky_vis.comp:40-47`);
  4. without GI: the flat ambient term and SSAO only — there the shells darken by shadow alone.
  Remove them and the forest floor is lit like a meadow until new crowns close the sky. New crowns must therefore cast
  shadows at every distance at which the floor is seen, including in LODs and impostors, and must be in the ray scene
  in some form. The plan's reference is 1–3 % of light at the ground
  (`reports/engine/2026-11-12-world-detail/README.md:214`).
- **Baked vertex light** of the world mesh is not used for landscape shading (old audit), so nothing is lost there.
- **Grass**: the grass field reads each chunk's ground summary by material; shells are not `grass` materials and their
  removal changes nothing for it.

## 10. Placement

- **Instance-list format for baked vegetation: not found.** Trees are vobs: base ones in `worlds/<W>/vobs.json`, own
  ones in `content/worlds/<W>/vobs.d/*.json`.
- **Vob layer** (`OG/common/world/voblayer.h:8-37`, `voblayer.cpp:387-395`): `add` (fields `key`, `kind`, `position`,
  `rotation` or `yaw_deg`, `visual`, `collision`), `remove`, `move`, and `visual` — swapping the visual of a base mesh
  vob in place. **Rotation must be orthonormal: no scale** (`voblayer.cpp:126`). Size variety therefore needs separate
  meshes, each with its own buckets, BLAS and shape.
- **`meshes.json`** replaces a visual by name for every vob that uses it (`docs/own-content-meshes.md:28`). Replacing
  the 16 tree visuals is the shortest path to new trees at the old places, with the old flags (collision, wind).
- **Placement scripts**: `scripts/content/harbour_life.py` (heights from the world mesh, writes a layer),
  `scripts/content/world/landscape_tuft_spots.py` and `landscape_grass_tufts.py` (1 848 tufts as layer objects). No
  rule-based forest scatter with exclusion masks: not found.
- **What the grass system offers** (`OG/common/graphics/grass/grassfield.h:30-45`):
  - a ring of 8 m cells round the camera, one storage buffer per cell, built on a worker thread, with a per-frame
    upload on the main thread;
  - cell selection on the CPU by distance and view, ranks so that a far cell draws only the start of its list, a
    smooth thinning with distance (`grassfield.cpp:650-686`);
  - the chunk ground summary from landscape streaming (`addChunk`, `LandscapeStreaming::pack`) — triangles by material
    and open edges — as a base for baked placement;
  - the gust front by place, shared with `foliage`.
  What it does **not** offer: it is not a mesh-instance system. A blade is a 16-byte record expanded in
  `grass.vert`; the draw is `cmd.draw` per batch with no vertex buffer (`grassfield.cpp:686`); it writes the G-buffer
  only — no shadow pass, no HiZ, no ray scene, no collision; placement is recomputed, never stored. For trees the
  reusable parts are the cell and ring bookkeeping, the rank thinning idea and the summary; the drawing path is not.
- **Streaming** treats a vob as a member of the 64 m chunk of its origin (`objvisual.cpp:359-360`): its visual exists
  in Near and Far, is released Outside; logic objects stay; publication of Far visuals is sliced by a per-frame
  deadline (`worldview.cpp:106-135`). There is no per-ring detail: a tree is the same mesh at 10 m and at 1 km.

## 11. Budgets and measurement

- `scripts/perf-content.sh A B [places]` (`scripts/perf_content.py:20-24`): two staged content sets on the Ryzen
  stand in one quiet ABBA run; p95 noise band 2 %, limit 5 %. Places: `harbour`, `harbour-night`, `market`, `forest`,
  `interior`, `combat`, `xardas`, `temple`.
- `scripts/perf.sh <places>`: p50/p95, triangles, draw calls; GPU time per pass with `OPENGOTHIC_GPU_PASSES=1`; cost
  of a pass by subtraction with `OPENGOTHIC_GPU_SKIP` (`docs/graphics-plan.md:30`). Places also include `rock` and
  `meadow`.
- `scripts/bench/perf-forest.bench`: four views from `NW_FOREST_PATH_16` at noon, 8 s each — the existing forest
  measure. `perf-shot.bench` gives frames of the same views.
- `scripts/content/world/world_census.py` for triangle counts of a region.

Numbers in earlier notes:

| What | Number | Source |
|---|---|---|
| Triangles per frame, all passes, forest place | 0.62 M (×1), 1.64 M (×4), 5.11 M (×16) | `reports/performance/2026-11-10-content-stress/README.md:49` |
| GPU time on RTX 3080 at six times the triangles | +0.1…0.3 ms; the frame is CPU-bound | same, `:35` |
| Grass: 430 k blades, 1.92 M triangles, G-buffer only, no alpha, no shadow | +0.48…0.76 ms GPU at 2560×1440 on RTX 3080 | `grass-system/README.md:320-330` |
| Tufts: 1 848 alpha-tested objects with wind and VSM shadow | within noise at 1080p | `living-foliage/README.md:186-199` |
| Frame of the stand at 1080p, RTX 3080 | 2.6–3.0 ms GPU | same |
| M1 Pro | no triangle-throughput number found; the Mac is GPU-bound, shadows 5.5 ms (old audit); VSM +7…12 ms | `docs/graphics-plan.md:20` |

No measurement exists of alpha-tested overdraw, of a dense canopy in VSM, or of alpha-tested instances in the ray
scene. The stress test subdivided triangles in their plane, so the cost of small and thin triangles is understated
(`content-stress/README.md:177`).

## 12. Impostors

- **In the engine: nothing.** No billboard path for meshes, no impostor material, no atlas lookup by view direction
  (search for `impostor`, `billboard`, `octahedr` finds only normal packing). `sprite_camera_facing_mode` is a field of
  every vob in the base data (`vobs.json`); whether the engine honours it for decals was not checked, and it is not a
  mesh path.
- **Bake tooling: nothing ready.** No render-to-atlas script. Pieces that could be used:
  - Blender scripts under `scripts/content/mesh3d/blender/` (`compare.py` renders a model beside its concept,
    `bake_atlas.py` bakes tiles into a prop atlas with AO) — offline renders with Cycles, the natural place for an
    octahedral bake of albedo, normal and depth;
  - `scripts/content/glb_cards.py` writes card meshes with authored normals and a coverage-preserving cut-out DDS —
    the output side of an impostor card;
  - bench capture (`perf-shot.bench`, `cam world`, `rec`) gives lit game frames, not unlit G-buffer layers; no command
    dumps albedo or normals of one object (not found).
- An impostor needs in the engine: a material that picks atlas frames by view direction and blends them, depth or
  normal from the atlas for lighting, a shadow answer (a card toward the sun or a proxy), and the LOD switch of
  section 2.

---

## Capability table

| Capability | State | Where | What a modern tree needs |
|---|---|---|---|
| Instanced drawing of one mesh many times | exists | `drawbuckets.cpp:43`, `visibility_pass.comp:219-236` | nothing for a few thousand trees; 16-bit bucket ids limit the number of distinct mesh × material pairs |
| GPU frustum and HiZ culling | partial | `visibility_pass.comp:176-217` | per object only: a tree is one cluster. Needs clusters per meshlet group (trunk, crown parts) or it draws whole |
| Mesh shaders / meshlets | exists (off on macOS) | `packedmesh.h:23-25`, `commandline.h:73-77` | nothing |
| Draw distance / screen-size cull | missing | `camera.cpp:168`, `vob.cpp:327-329` | a size test in the culling shader for main and shadow views |
| Discrete LOD per mesh | missing (strand precedent) | `visualobjects.cpp:838-881` | LOD ranges per mesh, selection on GPU or CPU, a field in `meshes.json` |
| LOD cross-fade | missing | — | per-instance fade into the hashed alpha threshold (`main.frag:947`) |
| Impostors and their bake | missing | — | atlas bake in Blender, impostor material, shadow answer |
| Alpha test | exists | `main.frag:947-1016` | nothing |
| Hashed alpha under TAA | exists (own `MASK`) | `drawbuckets.cpp:376-379` | check noise on dense crowns; shadows keep 0.5 |
| Alpha-to-coverage | missing | — | not needed with hashed alpha |
| Depth prepass against overdraw | partial (occluders only) | `renderer.cpp:945`, `:1129` | a crown prepass or accepted overdraw; unmeasured |
| Two-sided materials | missing | `shaders.cpp:500` | twin cards in the mesh (doubles crown triangles in every pass and the BLAS) or a two-sided pipeline with a flipped normal |
| Coverage-preserving mips | exists in tools | `glb_cards.py:90-114`, `alpha_mips.py` | use `write_cutout` for leaf atlases |
| Custom vertex normals | exists | `gltfmesh.cpp:475-483` | nothing; mind the cotangent frame on normal-mapped cards |
| Vertex colour, tangents, second UV from GLB | missing | `gltfmesh.cpp:449-482` | read `COLOR_0` into the free `Vertex.color` (4 bytes) for wind weights and AO |
| Per-vertex AO | missing | — | a byte of `Vertex.color`, or bake into the atlas ORM |
| Light through leaves | partial | `foliage_light.glsl`, `direct_light.frag:363` | sun only, one tint for all; no lamps, sky or GI; per-material tint and thickness |
| Vertex wind | partial | `foliage_wind.glsl`, `foliagewind.h` | a tree anchor mode: trunk, branch and leaf levels with per-vertex data |
| Per-instance wind phase | partial | place hash, `foliage_wind.glsl:77-83` | enough for trunk sway |
| Gust front by place | exists | `foliageFrontAt` | nothing |
| Motion vectors of wind | exists | `vertex_process.glsl:390-401` | nothing (the new mode calls the same function) |
| Sun shadows of alpha-tested moving geometry | exists | `shaders.cpp:596-604`, `renderer_vsm.cpp` | no page cache: every tree redrawn every frame; 512k meshlet–page cap; LOD for the shadow view |
| Ray-traced far shadow and GI of alpha-tested static meshes | exists | `rtscene.cpp:318-337`, `rt_common.glsl:138-158` | leaves at rest, alpha loop per candidate, one-sided; a proxy crown for rays is missing |
| Through-light in GI | missing | — | optional |
| Collision | partial | `protomesh.cpp:63-73`, `objvisual.cpp:369` | whole drawn mesh or nothing; needs a trunk-only shape (collision mesh or capsule) and collision for windy trees |
| NPC sight through crowns | partial | `dynamicworld.cpp:363-365` | sight follows collision; a trunk-only shape opens sight through crowns — a gameplay decision |
| Removing shell triangles by material | partial | `world_edit.py`, `world_bpy.py`, `chunk_glb.py` | a short edit script; trunks (`BARK_04`) carry collision |
| Darkness under canopy | exists by shells | sections 6, 7, 9 | new crowns must shadow and occlude at all distances |
| Baked placement / instance lists | missing | — | a per-cell instance format, or stay with vobs (no scale in layers) |
| Scatter by rules with masks | missing | — | an offline tool; the grass chunk summary is a base |
| Cell ring, ranks, worker (grass) | exists, blade-only | `grass/grassfield.h` | bookkeeping reusable, drawing not |
| Streaming of vobs | exists | `worldview.cpp:92-135` | no detail per ring |
| Cost measurement | exists | `perf-content.sh`, `perf.sh forest`, `OPENGOTHIC_GPU_PASSES` | a dense-forest view; no overdraw, VSM-canopy or RT-foliage numbers yet |

## Open questions the code cannot answer

1. Overdraw cost of a card crown on RTX 3080 and on M1 Pro (no measurement, no prepass).
2. Cost of thousands of alpha-tested card trees in the ray scene with the per-candidate alpha loop.
3. Whether VSM's meshlet–page cap holds with trees of hundreds of meshlets drawn whole.
4. Whether NPC sight should pass through new crowns (today it does not for colliding trees).

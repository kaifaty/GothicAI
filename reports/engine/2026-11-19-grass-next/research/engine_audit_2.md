# Grass, next round: what the engine can and cannot do (code audit 2)

Read-only code audit, 2026-11-19, worktree `gothic-skeletal-animations-mpfb2-f32f5d` at `main` `b6d7cf6d5`. Paths are
relative to `OpenGothic/` unless they start with `reports/` or `docs/`. **Code** = read in the code; **Infer** = my
inference, not measured. Nothing was built or run.

It continues [`../../2026-11-17-grass-cost/research/engine_audit.md`](../../2026-11-17-grass-cost/research/engine_audit.md)
(audit 1). Still true from audit 1 and not repeated here: blade storage and the 16-byte record (§1.1), density rules
(§1.3), the per-vertex math table (§3, line numbers shifted: wind/tilt now `grass.vert:57-111, 175-190`), the ground
shader's per-case fetch table (§4.2-4.5, line numbers shifted, see §6 below), blades absent from shadows/VSM/RT (§2).
Out of date in audit 1: cell order (now sorted, `grassfield.h:37-43`), one draw per cell (now one draw per cell ×
slot × kind over the visible 2 m squares, §3), link distances (now 8/12/24 m, `grassdiag.h:117-140`), the contact ray
(now has a mask, §5), the grass draw order (now before the ground, `worldview.cpp:841-850`).

---

## 1. GPU-driven submission

| capability | Vulkan | Metal | existing user in the engine |
|---|---|---|---|
| indirect draw, args written by compute in the same frame | **yes, one non-indexed draw per call**: `Encoder::drawIndirect` → `vkCmdDrawIndirect(..., 1, 0)` (`lib/Tempest/Engine/graphics/encoder.cpp:176-182`, `gapi/vulkan/vcommandbuffer.cpp:874-886`) | yes, `drawPrimitives(topology, buffer, offset)` (`gapi/metal/mtcommandbuffer.cpp:869-876`) | `DrawCommands::drawCommon`/`drawHiZ` (`common/graphics/drawcommands.cpp:731-733, 793-795`); args = `IndirectCmd {vertexCount, instanceCount, firstVertex, firstInstance, writeOffset}` (`drawcommands.h:141-147`), `instanceCount` zeroed by `cluster_init.comp:21` and bumped by `atomicAdd` in `visibility_pass.comp:352-353` |
| **indexed** indirect (`DrawIndexedIndirect`) | **no** (not in `encoder.h:104`) | no | — |
| multi-draw / draw count buffer / `gl_DrawID` | **no**: drawCount is hard-coded 1 (`vcommandbuffer.cpp:885`); SPIRV-Cross refuses `DrawIndex` for MSL (`thirdparty/spirv_cross/spirv_msl.cpp`, "DrawIndex is not supported in MSL") | no | — |
| indirect compute | yes, `dispatchIndirect` (`encoder.cpp:236-240`) | yes | GI surfels (`renderer_gi.cpp:373, 379`) |
| SSBO written by compute, read by the **vertex shader as the instance source**, same frame | yes; barriers come from Tempest's resource tracking: compute writes are tracked per binding (`vcommandbuffer.cpp:601-605`), an indirect buffer is fenced against later writers (`:877-879`, `resourcestate.cpp:98-127`) | yes (Metal tracks hazards itself) | `visibility_pass.comp:368` writes `payload[]`; `main.vert:373-387` reads `payload[gl_InstanceIndex + firstMeshlet]` in the vertex path |
| non-indexed draw with `gl_VertexIndex` | yes, `draw(nullptr, offset, count, firstInstance, instanceCount)` (`encoder.h:65-66`) | yes | `main.vert:265-275`: each instance (a meshlet) is `PackedMesh::MaxInd = 192` vertices (`drawcommands.cpp:356`, `packedmesh.h:25-28`), `gl_VertexIndex/3` picks the triangle, padding vertices go to `MeshletPaddingPosition = (0,0,2,1)` (`materials_common.glsl:104`) |
| topology | triangle **lists** only (`gapi/abstractgraphicsapi.h:98-102`); no strips | same | — |
| mesh shaders | `VK_EXT_mesh_shader`; detected from the extension and `VkPhysicalDeviceMeshShaderFeaturesEXT` (`gapi/vulkan/vdevice.cpp:434-436, 630-638`) | `GPUFamilyMetal3 && GPUFamilyApple7` (M1 and later) (`gapi/metal/mtdevice.mm:576-583`) | world geometry: `main.vert` compiled with `-S mesh` (`shader/CMakeLists.txt:99-101`), 64 threads, ≤64 verts/prims (`main.vert:16-17`); water reflection `water_reflection.mesh`. **No task shaders** exist (no `*.task` file); pipelines are built with an empty task stage (`shaders.cpp:748-754`) although Tempest takes one |
| task shaders | supported by Tempest and the device props (`vdevice.cpp:630`) | supported (`mtdevice.mm:579`) | none |
| clip distance | `gl_ClipDistance` | via SPIRV-Cross | only the VSM pass (`main.vert:52-56, 175-187`); VSM is off by default on Apple (`commandline.h:78-80`) |
| cull distance | **Infer: available, unused.** The features2 path copies every supported core feature into device creation (`vdevice.cpp:332, 346`), so `shaderCullDistance` should be on; `maxClipCullDistances` is read (`vdevice.cpp:733`) | **Infer: unreliable.** SPIRV-Cross emits `cull_distance` (`spirv_msl.cpp` near 16985); MSL has no cull-distance attribute that I know of. Untested in this engine | none |

**When the world uses which path (code).** `opts.doMeshShading = gpu.meshlets.meshShader && -ms` (`shaders.cpp:106`);
`-ms` defaults to 1 everywhere except macOS, where it is 0 "slower on M1" (`commandline.h:73-77`); `OPENGOTHIC_MS`
overrides (`commandline.cpp:42-44`). So on the RTX 3080 the world is drawn with `dispatchMeshIndirect` (offset +4 of
the same `IndirectCmd`, reusing `instanceCount` as the group count, `drawcommands.cpp:355-358, 793-795`); on the Mac
with `drawIndirect` + the vertex shader above. **Fallback** is that same vertex path, chosen per command by
`DrawCmd::isMeshShader` (`drawcommands.cpp:128-134`). Grass uses neither: `grass.vert` is an ordinary instanced
indexed draw on both backends ("no meshlets: the same path with mesh shaders and without", `grass.vert:10-12`).

**Consequences for grass (infer).**
- A compute-compacted grass list can only be drawn with `drawIndirect` **non-indexed** today. With a triangle list that
  is 3·(2n−1) invocations per blade instead of 2n+1: 21 vs 9 (4 links), 15 vs 7, 9 vs 5, 3 vs 3. Since the `Grass`
  pass tracks submitted vertices and primitives, this throws away part of the gain. Adding `drawIndexedIndirect` is a
  small Tempest change (Encoder + `VCommandBuffer` with `vkCmdDrawIndexedIndirect` + `MtCommandBuffer` with
  `drawIndexedPrimitives(..., indirectBuffer:)` + the DX12 stub), mirroring `drawIndirect` (`vcommandbuffer.cpp:874-886`,
  `mtcommandbuffer.cpp:869-876`).
- One indirect slot per (cell, slot, kind, links) draw, as `DrawCommands` does per command: the CPU keeps issuing the
  draws, the GPU only fills `instanceCount`. No multi-draw, so the draw count does not fall.

## 2. HiZ

**What and where (code).**
- `Renderer::draw` order (`renderer.cpp:965-1005`): `visibilityPass(0)` (the occluder view `V_HiZ`, tested against
  **last frame's** pyramid, `drawcommands.cpp:399-470`) → `drawHiZ` = marker `HiZ-occluders`, depth-only into the main
  `zbuffer` cleared to 1 (`renderer.cpp:1250-1254`) → `buildHiZ` = marker `HiZ-mip` (`:1256-1292`) →
  `visibilityPass(1)` (main view against the fresh pyramid) → `drawTreeHistory` or `drawGBuffer`.
- **Occluders** (`DrawCommands::drawHiZ`, `drawcommands.cpp:702-734`): commands of type `Landscape` and `Static`,
  `Solid` or `AlphaTest`, with a `pHiZ` pipeline. The landscape **is** in it. Through the `V_HiZ` visibility pass
  (`visibility_pass_hiz_cr`, `-DLARGE`, `CMakeLists.txt:133`) instanced clusters under 16 HiZ texels² are skipped
  (`visibility_pass.comp:296-303`), tree crowns are skipped with `VF_HIZ_NO_CANOPY` (`:340-343`, `hizoccluders.h`),
  strands never occlude (`:344-347`). Grass blades are not occluders (drawn later, not in `DrawCommands`).
- **Size and format**: `R16` with mips, the next power of two of the frame halved until ≤64 (`renderer.cpp:768-778`).
  At 2560×1440: pot 4096×2048 → **64×32 texels, a texel = 64×64 pixels** (`hiZTileSize`, `sceneglobals.cpp:342-347`).
  `hiz_pot.comp:33-67` takes the max depth of each tile and stores `packHiZ(z) = (z-0.95)*20` (`common.glsl:251-254`)
  + 1/32768. With znear 10 cm and far 1000 m (`camera.h:192`, `camera.cpp:168-171`) depth 0.95 is about 2 m:
  **anything nearer than ~2 m stores 0** and culls nothing behind it that is itself nearer than ~2 m (infer).
- **History pyramid** (default on: `[INTERNAL] hizHistory=1`, `settings.cpp:196`, if `isHiZHistorySupported`,
  `shaders.cpp:73-79`): `drawTreeHistory` draws last frame's winning clusters early into the depth
  (`HiZ-history-depth`), then builds a second pyramid, **R32F, ≤128 wide** (128×64 at 1440p, 32-px texels), and
  classifies against it (`renderer_hiz_history.cpp:19-52`), then `drawGBuffer` (`:67`). Infer: on the 3080 the grass
  usually sees this path, so by the `Grass` draw the depth holds landscape + large statics + last frame's history
  winners (trees, ...).

**The cluster test (code, `visibility_pass.comp:264-369`).** Sphere vs 6 frustum planes (`:251-262`), then LOD
(`:278-280`), then project the bucket's box through the instance matrix (8 corners) or the cluster sphere
(`projectSphere`, `:98-128`); a corner behind znear means visible (`:170-174`). `hiZTestPrecise` (default,
`VF_HIZ_PRECISE`, `[INTERNAL] hizPrecise=1`) picks the mip at which the rect spans ≤2×2 texels, takes the max of
those texels and culls when `packHiZ(depthMin) > z` (`:205-234`); the older `hiZTest` is the same idea (`:187-203`).
The visible cluster is appended with `atomicAdd` to its command's `instanceCount` (`:352-368`).

**Grass relative to it (code).** The `Grass` draw is the first thing inside the G-buffer render pass
(`worldview.cpp:845-849`), after both pyramids. Its fragments behind the occluder depth already fail early-Z (LEqual,
`shaders.cpp:177-178`); its **vertices and primitives do not**, which is what the `Grass` row pays.

**Can a compute pass test squares or blades between the HiZ and the grass draw?** Yes (code + infer):
- Slot: after `buildHiZ` (`renderer.cpp:982`), next to `visibilityPass(1)` (`:994`), which is exactly such a pass; or
  in the history path after `buildHiZ(cmd,&pyramid)` (`renderer_hiz_history.cpp:50`) to use the finer, fuller R32F
  pyramid. It must be recorded **before** `drawGBuffer`'s `setFramebuffer` (`renderer.cpp:1294-1316`): a dispatch
  inside a render pass throws (`encoder.cpp:147-149, 209-211`). Today `GrassField::draw` builds its lists and uploads
  the span table from inside the render pass (`grassfield.cpp:621-790`); it would have to be split into a
  prepare (lists, upload, cull dispatch) before the pass and a draw inside it.
- The CPU cannot read this frame's HiZ; a readback (as `visibilityStats`, `drawcommands.cpp:509-552`) is 1-2 frames
  late. So squares are tested on the GPU or not at all.
- **Bounds a blade needs (code):** root y from its record (16 bits over the cell's `minY`, `grass.vert:125`), height
  ≤ 255·0.25 = 63.75 cm (`grass.vert:137`), and the shape: links keep their length and `bladeDir` clamps the lean to
  |v| ≤ 2.4 with y > 0 (`grass.vert:96-102, 105-111`), so the tip stays inside **[root.y, root.y+h] vertically and
  within 0.93·h horizontally**, wind lean, swing and flutter included (they all go through `bladeDir`). Half width
  ≤ ½·max(2.95 cm·min(d/full,8), 1.3 px) (`grass.vert:156-158`, `grass_common.glsl:75-76`). The ground normal does not
  move the vertices. A sphere at root+(0,h/2,0) of radius h + half width is conservative.
- **Bounds a 2 m square needs:** its rectangle, widened by `CellMargin` 64 cm on the cell's edge squares (roots in the
  margin go to the nearest square, `grassplace.h:26, 82`), by h_max and the half width; y from the square's lowest root
  to its highest root + h_max. **The CPU keeps y only per 8 m cell** (`c.minY/maxY`, `grassfield.cpp:426-427`), and
  the draw tests squares with the cell's y range (`grassfield.cpp:660-664`); per-square y (two numbers per `Batch`,
  `grassplace.h:79-85`) would make hillside squares much tighter (infer).

## 3. The grass draw today

**CPU (`GrassField::draw`, `grassfield.cpp:621-790`, code).**
- Per live cell in the ring: frustum test of the cell box (padded 144 cm, y −10/+80 cm, `:648-656`), links by the
  cell's nearest distance (`:658`, `grassdiag.h:117-140`: 4/3/2/1 at 8/12/24 m).
- Per batch (slot × kind × square, blades sorted by rank inside): the square's own nearest distance and frustum test
  (`:676-686`), the rank prefix that stands at that distance (`:690-693`), fine blades only to 20 m (`:687`).
- Squares of one (cell, slot, kind) become **one draw**; a frame table of ≤16 spans per draw maps the draw's instance
  to the blade index in the cell's buffer (`:716-735`). Draws are stable-sorted by the cell's distance
  (`grassfield.h:37-43`). The span table is a per-frame `BufferHeap::Upload` SSBO (`:742-748`; Upload storage prefers
  `HOST_VISIBLE|DEVICE_LOCAL`, `vallocator.cpp:238-241`).
- **Per draw:** 52 bytes of push constants (`Push`, `:49-57`), binding 1 (the cell's SSBO) only when the cell changes,
  bindings 2-3 (painted ground and sward textures) only when the slot changes, pipeline and bindings 0/4 once
  (`:750-785`). `cmd.draw(nullptr, ibo, indexOffset(links), 3·(2n−1), 0, count)`: **instanced, indexed, uint16
  triangle list, no vertex buffer** (`:783`).
- **Index buffer** (`:83-94`): for n = 4, 3, 2, 1 back to back; n−1 quads (2 triangles) plus a tip triangle:

  | links | vertices (2n+1) | triangles (2n−1) | indices |
  |---|---|---|---|
  | 4 | 9 | 7 | 21 |
  | 3 | 7 | 5 | 15 |
  | 2 | 5 | 3 | 9 |
  | 1 | 3 | 1 | 3 |

**Per vertex (`grass.vert:113-221`, code).**
- **Buffer reads:** the span table's binary search, 4 dependent reads (`:118-122`), + 1 read of the span's offset, +
  1 `uvec4` blade read (`:123`): 6 SSBO reads, all identical for the 2n+1 vertices of a blade.
- **Textures:** `textureQueryLevels` + `textureLod` of the painted ground (`:204`) and, on own-layer grounds, of the
  sward (`:205`): 2 fetches per vertex, per-blade values.
- **Rejected blade:** after the reads, decode, distance and `grow`, a blade with height ≤ 0.05 (rank over the share,
  past the ring fade), or with blades off or `GrassPartHide`, writes **`gl_Position = NaN`** (`0x7fc00000`) and zero
  varyings (`:146-155`). Every vertex of it does so, so all its triangles are discarded at clipping (infer:
  implementation behaviour for NaN; the engine's own padding elsewhere uses the defined `(0,0,2,1)`).
- **Per blade, recomputed per vertex:** everything from `:117` to `:178` except `k, t, side`, plus both texture
  fetches, the previous-frame wind (`:184-190`) except `bladePoint`, `outShade` except `t`. **Truly per vertex:**
  `bladePoint(tilt,k)` (an O(k) loop), the side offset, the clip position and its previous, the face normal
  (`bladeDir(tilt,t)`). The plan already measured that this ALU costs ~nothing (`GrassPartOffscreen`,
  `grass.vert:218-220`, `docs/grass-plan.md` "Снятые меры"); the cost is the count of invocations and primitives.

**What would have to change (infer, from the code above).**
- **(a) Compacted visible-blade list by compute.**
  - Inputs: the candidate draws (cell, slot, kind, links, square spans) built by the CPU before the pass, and the
    blades — but each cell has its **own** `StorageBuffer` (`grassfield.h:124`). The compute needs them all: a
    bindless storage-buffer array (Tempest has buffer descriptor arrays: `device.descriptors(vbo)`,
    `drawbuckets.cpp:203-210`; `nonUniformIndexing` on both backends) or one pooled buffer with sub-allocation.
  - Output: per candidate draw an `IndirectCmd`-like slot and a compacted index list; the VS reads
    `list[firstInstance+gl_InstanceIndex]` instead of the span search (that removes 5 of the 6 reads).
  - Draw: `drawIndirect` per candidate draw. Non-indexed today (21 vs 9 invocations at 4 links), or add
    `drawIndexedIndirect` (§1). Push constants stay per draw.
  - Test per blade or per square against frustum, HiZ and `grow` (rank vs own distance); the same `grow` formula as
    `grass.vert:141-144` must be used so nothing pops.
  - The cull dispatch must precede the G-buffer render pass (§2).
- **(b) Several blades per instance.** A second index buffer with B copies of each strip offset by 2n+1 vertices;
  the VS takes `blade = inst·B + gl_VertexIndex/(2n+1)`; the CPU count rounds up to B with the excess as NaN. Cuts
  the instance count B-fold at the same vertex count. Infer: it only helps if the NVIDIA front end pays per instance
  (9-vertex instances); not measured.
- **(c) Mesh-shader path.** Vulkan: available and on by default on the 3080 (§1). A workgroup of 32 lanes, one blade
  per lane: cull (frustum, HiZ, `grow`), ballot-compact, emit ≤ 28 blades × 9 = 252 vertices and 196 triangles
  (infer: inside NVIDIA's `maxMeshOutputVertices` 256; Tempest reports `maxGroupSize`, `vdevice.cpp:632-637`; the
  output limits are not exposed and would need adding). A task stage could cull 2 m squares first; Tempest takes a
  task shader but the engine has never used one. `gl_CullPrimitiveEXT` is already used in `main.vert:367` (the VSM mesh path).
  **Fallback:** the current `grass.vert` path on Metal (mesh off by default on macOS) and with `-ms 0`.

## 4. Why grass pixels make `AmbientLight`, `GI-Alloc`, `GI-Lighting` dearer

With RT on, `giMethod == Probes`, so `AmbientLight` runs `probe_ambient.frag` (`renderer_gi.cpp:839-878`). **No pass
reads `isGBufGrass`** (code: its only users are the contact mask/ray, `direct_light.frag`, `grass_light.glsl`). The
cost depends on the pixel's depth and normal, and on how many probes exist:

- **Probe cages are 25 cm × 2^lod** (`probe_common.glsl:100, 171-174, 196-205`); lod 0 reaches 9.5 m and lod 1 19 m
  at 1440p (`sceneglobals.cpp:349-355`). A pixel's cage is from `basePos + normal·2.5 cm`. Blade tips stand 10-60 cm
  over the ground, have their own normals and alternate with ground pixels at 1-2 px scale (infer), so near the camera
  neighbouring pixels fall into different cages.
- **`AmbientLight` (code, `probe_ambient.frag:141-175`):** a subgroup takes the fast path (8 lanes read the 8 corner
  probes once, the rest shuffle) only when **all lanes share lod, `pLow` and `pHigh`** (`:149-150`). Otherwise every
  lane walks 8 corners × a hash chain of up to 8 probes (`readProbe`, `:77-104`). Infer: grass breaks the fast path
  for most subgroups within ~19 m. Foliage pixels skip contact AO (`:241-242`), which saves nothing.
- **`GI-Alloc` (code, `probe_allocation.comp:115-162`):** two dispatches over the whole screen (`probe_vote`,
  `probe_allocation`, `renderer_gi.cpp:507-543`), workgroups of 16×8. Lanes in the leader's cage return at once
  (`:152-156`); every other lane runs 8 corners × `reuseProbe` (hash walk ≤8) and votes with atomics (`:73-101`), and
  a vote flags its tile so the allocation pass does not skip it (`:79, 117-119`). Infer: grass multiplies the lanes
  that leave the fast exit and the tiles that vote, and adds probes for the extra cages it touches.
- **`GI-Lighting` (code, `renderer_gi.cpp:614-644`):** dispatches are fixed (`maxProbes·256/relitPeriod` threads,
  256 and 1024 groups), but threads exit on unused probe slots (`probe_lighting.comp:474-482`), so the work follows the
  **number of live probes** (and `GI-Trace` the new ones). Infer: more cages occupied ⇒ more live probes ⇒ more relit
  rays. Blades are not in the TLAS, so probes in grass trace the ground below them.
- **How to check it without new code (infer):** the frame profile's GI stats (`GI-Stats`, `renderer_gi.cpp:649-661`,
  `readGiStats`) give live/new probe counts; compare A and C. A lever that needs no look change: build the cage
  position of a grass pixel from the ground under it (or snap it), but that changes the light at blade tips.

## 5. The pass `GrassContactMask`

**What it is (code).**
- Dispatched after the G-buffer and blood decals, before SSAO (`renderer.cpp:1026-1041`), when the contact ray is
  active and the mask on. `setFramebuffer({})` ends the render pass, then the marker, then
  `dispatch(ceil(W/64), ceil(H/64))` = 40×23 = 920 groups of 8×8 lanes at 1440p.
- `grass_contact_mask.comp:12-42`: each lane `textureGather`s alpha over its 8×8 pixels (16 gathers) and **returns at
  the first grass texel**; bits per 16×16 px are OR-ed into one `R32U` word per 64×64 tile. It reads all of
  `gbufDiffuse` where there is no grass (14.7 MB at 1440p ≈ 0.02 ms at the 3080's bandwidth, infer), less where there
  is. So its own work **grows with the non-grass share** of the frame.
- The receiver side: `grassContactMaskEmpty` reads ≤4 words for the ray's pixel rectangle (`grass_contact.glsl:46-68`)
  and skips the loop when all covered bits are 0 (`:109-125`).

**Why 0.02-0.16 ms (code + infer).**
- Timers are `BOTTOM_OF_PIPE` timestamps at each marker; a row runs **from the end of the previous work to the end of
  its own** (`vcommandbuffer.cpp:135-160`; `docs/performance.md`, "GPU-время на Vulkan").
- `endRendering` only marks the attachments' next layout (`resourcestate.cpp:27-42`); the **layout transitions and
  the attachment→shader-read barrier of the whole G-buffer and the depth are emitted at the next `flush`**, which is the
  mask's `dispatch` (`vcommandbuffer.cpp:539-548, 601-605`; `resourcestate.cpp:204-222`). So the row contains the
  G-buffer's drain and transitions, not just the mask.
- Without the mask the same transitions fall into the next row (`SSAO-depth`, `renderer.cpp:1530-1535`). Infer: the
  A − C difference of `GrassContactMask` overstates its cost, and its run-to-run variation is mostly that barrier;
  `SSAO-depth` should be compared at the same time. When blood decals are drawn their render pass is the one that ends
  before the mask (`renderer.cpp:1022-1025`).

**Could the mask come without its own pass? (infer)**
1. **Depth-limited mask, same pass.** A hit needs a blade pixel 0.2-4 cm nearer than a ray point that is at most ~9.3 m
   away (`grass_contact.glsl:20-23, 146-149`); grass pixels farther than that can never be hits. Also the far meadow
   ground carries the same bit from 24 m (`grass_ground.glsl:64, 80`) and can never be a hit. Reading depth too and
   ignoring texels past ~9.4 m keeps the mask exact and makes more rects empty. More reads, fewer loops.
2. **Written by the blades.** One `atomicOr` per blade from the vertex stage (vertex 0, the blade's screen bbox,
   blades within ~9.5 m only) into a buffer cleared before the G-buffer: a superset of the blade pixels, so still
   exact. Needs `vertexPipelineStoresAndAtomics` (enabled where supported, `vdevice.cpp:231`) and buffer atomics on
   Metal. Removes the pass and the full-screen read; costs a clear and some atomics (≤ the near blades).
   Per-fragment atomics would also work but are many more.
3. **From the CPU's squares.** The CPU knows each submitted square; projecting the boxes of squares within ~9.5 m onto
   the 16 px grid gives a conservative mask with no GPU pass. Coarser than today on mixed squares (a road and a
   meadow in one 2 m square), so fewer skips.
4. **Merged into an existing compute pass.** `SSAO-depth` already reads the full depth right after
   (`renderer.cpp:1530-1535`); adding the gather there saves a dispatch and a drain, but ties the mask to SSAO being
   on (`prepareSSAO` returns early without it, `:1483-1493`).

## 6. The ground material

**Call chain (code).** `main.frag:1117-1138`: `mainGBuffer` samples the painted texture (and its maps) first; then
`grassOwnGround` (`grass_track.glsl:203-223`) → `grassLayers` (`grass_layers.glsl:47-181`) for `BK_GROUND|BK_VERGE`,
`grassTrack` for `BK_TRACK`; then `grassGround` (`grass_ground.glsl:20-83`) for `BK_GRASS`.

**A meadow pixel of `BK_GROUND` (code; hash = `grassHash`, noise = `grassValueNoise` = 4 hashes,
`grass_common.glsl:129-150`, patch = 2 noises = 8 hashes, `:157-159`).**

| step | hashes | fetches | distance gate |
|---|---|---|---|
| `grassBorderNear` (`grass_border.glsl:13-38`) | — | 3 SSBO words, + 5 a piece in the 4 m bin | none |
| `grassCoverBorder` (`grass_cover.glsl:53-65`), only within 80 cm of a border | 12 | — | none |
| `grassCoverBare` (`grass_cover.glsl:25-50`) | 4, + 1 per candidate spot (35 % of 4 cells), + 8 (patch) per spot within reach, + 8 once for the bend | — | none |
| `grassCoverLevel` "between" (`:70-80`) | + 12 | — | none |
| sward pixel (`grass_layers.glsl:125-134`) | — | albedo 1 (<15 m), 2 (15-40 m), 1 far repeat (>40 m); ORM 1; normal 1 to 30 m | far repeat 15-40 m, normal fades 21-30 m |
| between / earth / loose earth (`:84-99, 135-146`) | `grassTrackTone` 16 on loose earth, spot tone 4 (`:151`) | 2-5 | normal to 30 m |
| painted ground before the layers (`mainGBuffer`) | — | 1 (+ normal, ORM if the record has them) | none, wasted under share 1 |
| `grassGround` (`grass_ground.glsl:40-82`) | patch 8 (no `grassBare` on own grounds) | — | far look and its wind only ≥ 24 m (`:64, 69-81`): this **adds** work far |

Typical sward pixel (infer): ~20-30 hashes + 3 SSBO words + 3-4 fetches (+1 painted) near; 2-3 fetches + the same
hashes past 40 m, plus the far-look ALU. The cover rule, the border lookup and the patch noise run **to the horizon**.

**What a baked per-chunk cache could plug into (code unless marked).**
- **The CPU mirror of the rule already exists:** `grasscover.h:46-67` (`coverBare`, `coverBorder`, `coverTuft`,
  `coverIsles`, `coverBreaks`), `grasspatch.h:11-66` (`valueNoise`, `patchNoise`, `patchBare`); the shader header says
  they are kept equal in whole numbers (`grass_cover.glsl:5-8`). A bake of cover level, bare and patch per texel would
  reproduce the shader on the CPU.
- **Per-chunk buckets:** a landscape bucket is one (StaticMesh, Material) (`drawbuckets.cpp:46-65`); infer: each chunk
  mesh gives its own buckets, so a per-chunk texture index fits in `BucketGpu` next to `fringeTex`.
- **Bindless arrays take appended textures:** the "fringe" maps are appended after the buckets' textures with an index
  per bucket (`drawbuckets.cpp:180-189, 300-313, 342-390`). A baked cover map per chunk would be one more such entry.
- **A world-wide SSBO built on the grass worker from the chunks** is the precedent for data: the border table
  (`GrassField::buildBorder/takeBorder`, `grassfield.cpp:271-348`), bound at `L_GrassBorder = 26` in the landscape
  pass (`grass_border.glsl:9-10`, `drawcommands.cpp:225`).
- **World chunks:** 64 m cells (`worldchunkmanifest.h:15-24`), each with draw/collision mesh files only
  (`:27-52`); cooked by `worldchunkcook.cpp`. No per-chunk texture exists in the format.
- **Content pipeline:** `scripts/content/landscape_meadow_ground.py` and `landscape_grass_layer.py` bake **tiling**
  material maps (sward, earth, fringe) in Blender, not per-chunk data.
- **No virtual or sparse texturing.** Texture streaming is per-texture mip residency by distance
  (`resources_textures.cpp:619-680`, `texturescreendemand.h:12-16`); nothing in Tempest uses sparse/tiled resources
  (grep found none). A per-chunk cache would be ordinary textures (e.g. 64 m at 25 cm = 256² per chunk, infer) loaded
  and dropped with the chunk ring.

---

## Constraints that matter most for choosing measures

1. **The `Grass` row is front-end cost** (submitted vertices and primitives, not shader ALU). Measures must cut
   submitted blades (HiZ, frustum per square, `grow`) — de-duplicating per-blade ALU does not pay.
2. **No `drawIndexedIndirect`, no multi-draw, no draw-count buffer** in Tempest. GPU compaction either loses index
   reuse (21 vs 9 invocations at 4 links) or needs a small Tempest addition on Vulkan, Metal (and the DX12 stub).
3. **Compute cannot run inside the G-buffer render pass.** A grass cull must be dispatched after `buildHiZ` and before
   `drawGBuffer`; `GrassField::draw` must be split into prepare + draw.
4. **The HiZ is coarse:** 64-px texels in R16 (≤2 m maps to 0); the history pyramid is 32-px R32F and holds more
   occluders, but only in the history path. A 2 m square at 30-50 m covers 1-2 texels: tests behind crests work,
   fine occlusion by grass itself does not.
5. **Per-square y bounds are not kept** (only per 8 m cell): needed for tight HiZ tests on slopes.
6. **Cell blades live in one SSBO per cell:** any GPU pass over many cells needs a buffer array or a pooled buffer.
7. **Mesh shaders are on by default on the 3080, off on the Mac.** A mesh path for grass needs the current VS path as
   its Metal fallback; task shaders are untried in this engine.
8. **Pass timers are bottom-of-pipe and include barriers.** `GrassContactMask` carries the G-buffer's layout
   transitions; compare with `SSAO-depth` before calling it expensive.
9. **The contact mask can be made exact and sparser** by ignoring grass pixels past ~9.4 m (the far ground's foliage
   bit included); or written by near blades with no pass.
10. **Ambient/GI costs come from cage incoherence and probe count, not from a grass branch:** anything that changes
    blade depth or normal near the camera moves them; check with the GI stats.
11. **The ground's cover rule runs to the horizon and has a CPU twin:** a per-chunk bake fits the bindless "fringe"
    mechanism, but the world format has no per-chunk textures and the engine no virtual texturing.

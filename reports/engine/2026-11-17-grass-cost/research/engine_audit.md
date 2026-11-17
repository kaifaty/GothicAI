# Grass system: where the GPU time goes, from the code

Read-only code audit, 2026-11-17, worktree `gothic-skeletal-animations-mpfb2-f32f5d` (branch `claude/meadow-cost`,
`main` at `544b98dc9`). Paths are relative to `OpenGothic/` unless they start with `reports/` or `docs/`.
"Inferred" marks anything not read directly from code. No build, bench or game was run.

Measurement this note explains (from the brief): RTX 3080, 2560x1440, RT on, meadow costs +1.5..1.8 ms; GBuffer
+1.2 ms (2.66 M triangles in view vs 0.63 M without grass, 152 draws vs 73); grass contact ray +0.12..0.33 ms;
road track +0.03 ms far / +0.4..0.5 ms standing on it; GI contact AO about +0.06 ms on grass.

---

## 1. Geometry

### 1.1 Generation and storage

- **CPU, per 8 m world cell, on one worker thread.** `GrassField::run` (`common/graphics/grass/grassfield.cpp:348-404`)
  calls `Grass::buildCell` (`common/graphics/grass/grassplace.cpp:119-691`). The chunk's triangles of materials with
  `grass` are summarised once per chunk on load (`GrassField::addChunk`, `grassfield.cpp:179-251`).
- Cell size 8 m, candidate lattice 4 cm = 625 nodes/m², tuft lattice 75 cm (`grassplace.h:20-26`).
- **One `StorageBuffer` per cell** (`grassfield.h:103-117`, `upload` `grassfield.cpp:416-439`). Upload budget is
  512 KB per frame (`grassfield.cpp:37`, `:498-508`). At most 6 jobs are queued, nearest first (`:40`, `:594-616`).
- **16 bytes per blade, no vertex buffer.** Layout is in `grassplace.h:68-75` (root x/z 14+14 bit, lean 4, y 16,
  height 8, rank 8, facing, phase, ground normal x/z, uv 12+12, width 4, tint 4). Blades are sorted by slot, kind
  and rank (`grassplace.cpp:673-689`), so a prefix of a batch is an even random sample of it.
- The vertices are made in the vertex shader from `gl_VertexIndex` and `gl_InstanceIndex`
  (`shader/grass/grass.vert:111-169`). There is one shared index buffer of four strips (4, 3, 2 and 1 links) built
  once (`grassfield.cpp:83-94`, `indexOffset` `:65-71`).

### 1.2 Vertices and triangles per blade

A blade of n links has 2n+1 vertices (two per link, one at the tip, `grass.vert:150-153`) and 2n-1 triangles
(`grassfield.cpp:84-93`).

| kind | links by the cell's nearest distance | vertices | triangles | width | source |
|---|---|---|---|---|---|
| main / broad / clump | 4 under 10 m, 3 to 25 m, 2 to 45 m, 1 beyond | 9 / 7 / 5 / 3 | 7 / 5 / 3 / 1 | 1.3 cm + 0.11 cm per width step (clump +5 steps, sward +3) | `grassfield.cpp:34-35, 646-649`; `grass_common.glsl:61-63`; `grassplace.cpp:651-652` |
| fine (sward with `"fine":1`) | 2 under 4 m, 1 to 20 m, none beyond | 5 / 3 | 3 / 1 | 0.5 cm + 0.036 cm per step | `grassfield.cpp:42-48, 662-665` |

"Tufts" and "clumps" are not separate meshes. A tuft is a gathering of lattice blades towards a jittered centre
(`grasspatch.cpp:97-135`, pull at `grassplace.cpp:521-522, 558-566`). A clump is a tuft in the top noise patch,
with 1.5-2.4 times the height and up to twice the density (`grasspatch.cpp:47-71`, `:106`).

### 1.3 Density and how it varies

- Base `BladesPerM2 = 320` (`grassparams.h:37`), times the record's `density` 0..2 (`:22`, `:61`), times the
  ring's quality density (1, or 0.625 in Low, `grassparams.h:139-143`, applied at `grassplace.cpp:139`).
- Patch multiplier from the patch noise (`grasspatch.cpp:43-57`): short sward (30 % of the area) 320/256 = 1.25,
  main 1.0, clumps ramp 1.0 to 2.0. The lattice caps it at 625/m².
- **Fine blades** fill every lattice node of the sward that is not a broad blade: up to 625/m², masked by the cover
  (`grassplace.cpp:463-465, 548-555, 586-590`). With `broad = 1` (the default, `grassparams.h:18`) every kept
  sward node that is clear of borders becomes broad (`grassplace.cpp:529-547`). Inferred: about 400 broad and 225
  fine blades per m² in the sward near the camera.
- **slope** (`grassparams.h:25`): no blade over `slope`, linear thinning over the last 10 degrees
  (`grassplace.cpp:140-141, 494-497`). It also lowers height (`:604`).
- **edge** (`grassparams.h:26`): a band at open edges, 55 % density at the edge going to 100 %, with 1.5 m noise
  (`grassplace.cpp:498-514`). Sparse low blades also stand up to 35 cm past an edge (`grassplace.h:28-30`).
- **clumps** (`grassparams.h:24`) scale how strongly blades are pulled to tuft centres (`grassplace.cpp:522`).
- Bare spots, item discs and water remove or lower blades (`grasspatch.cpp:77-95`; `grassplace.cpp:609-614`,
  `:567`).
- The test measured 309 blades/m² on a flat meadow before fine blades (`reports/engine/2026-11-14-grass-system/README.md:336`).

### 1.4 Level of detail by distance (all of it)

| mechanism | where | what |
|---|---|---|
| ring radii | `grassparams.h:129-143` | Full: full density to 10 m, fade from 48 m, end at 64 m. Low (`grass=1`): 8 / 30 / 40 m at 0.625 density. |
| density thinning, GPU | `grass.vert:128-133` | a blade stands while `rank < 1.12*full/d` (soft over 0.12); height to 0 by smoothstep(fadeFrom, radius) |
| density thinning, draw | `grassfield.cpp:651, 664, 666` | the instance count is the prefix of ranks that stand at the cell's **nearest** point less 1 m (`RootsBeyond` `:39`) |
| density thinning, storage | `grassfield.cpp:27, 118-130, 560`; `grassplace.cpp:444, 549` | a far cell is built with fewer ranks: 256 / 239 / 144 / 90 / 60 at 0 / 12 / 20 / 32 / 48 m |
| blade widening | `grass.vert:145-147` | width times clamp(d/full, 1, 8), and at least 1.3 px |
| link count | `grassfield.cpp:34, 646-649` | 4/3/2/1 links by the cell's nearest distance |
| fine ring | `grassfield.cpp:42-46, 386, 662-665`; `grass.vert:130` | all to 4 m, share 4.48/d beyond, gone 16-20 m; 2 links to 4 m, then 1 |
| wind detail | `grass.vert:164, 77, 87`; `grass_common.glsl:107-108` | swing and flutter amplitude to 0 between 25 and 30 m (the math still runs, see §3) |
| fade at the ring's end | `grass.vert:133`; `grass_ground.glsl:57-77` | height smoothstep 48-64 m; the ground takes the "far grass" look from 24 to 48 m and keeps it beyond |

### 1.5 A full ring by the code's arithmetic (inferred integration, flat meadow everywhere, Full mode)

The fraction of blades that stand at distance d is min(1, 11.2 m / d). With an average 310 broad blades/m²:

| band (nearest distance) | blades (360°) | triangles each | triangles | VS invocations each |
|---|---|---|---|---|
| 0-10 m | 97 k | 7 | 0.68 M | 9 |
| 10-25 m | 326 k | 5 | 1.63 M | 7 |
| 25-45 m | 436 k | 3 | 1.31 M | 5 |
| 45-64 m (fade band) | 415 k | 1 | 0.41 M | 3 |
| **total** | **1.27 M** | | **4.0 M** | **6.6 M invocations** |

Fine blades add about 34 k (30 % sward) to 110 k (all sward) within 20 m.

This agrees with the measurements: 372-590 k blades and 1.8-2.5 M triangles drawn per frame, 1.42-1.54 M blades
stored (`reports/engine/2026-11-14-grass-system/README.md:252-261`), and the brief's +2.03 M triangles. The
blades in the 45-64 m fade band are about a third of the ring's blades but only about 10 % of its triangles.

---

## 2. Submission and culling

- **Draws:** `GrassField::draw` (`grassfield.cpp:621-691`) makes one `cmd.draw(nullptr, ibo, offset, (2n-1)*3, 0,
  count)` per cell per batch (slot × kind, normally broad and fine). That is an instanced non-indirect draw with
  instance count = the rank prefix (`:686`). There is no indirect draw, no GPU compaction and no mesh or task
  shader path ("no meshlets", `grass.vert:12`). Per draw it binds two textures and push constants
  (`:858-868`). The +79 draws in the measurement are about 50-60 visible cells × 1-2 batches (inferred).
- **Culling:**
  - One CPU frustum test per cell AABB, padded by 1.44 m sideways and -10/+80 cm in y
    (`grassfield.cpp:629-644`, `Frustrum::testBbox`).
  - A ring-radius test (`:639`) and the rank prefix by distance.
  - **No occlusion culling:** the HiZ is built before the G-buffer (`renderer.cpp:946-950`), but grass never reads it.
  - **No back-of-hill culling, no per-blade or sub-cell frustum culling, no screen-size culling.**
- **Culled blades still cost vertex work:**
  - In a visible cell, every instance in the prefix runs all its vertices. Blades behind the camera or outside the
    frustum inside a visible cell are clipped only after the full vertex shader.
  - Blades whose `grow` is 0 (their rank is over the share at their own distance, the prefix being taken at the
    cell's nearest point) run the shader up to the early-out at `grass.vert:135-144`: buffer fetch, decode,
    distance, then a NaN position. They still launch 2n+1 invocations each.
  - Inferred: for a cell at a nearest distance of 20 m, about 20 % of drawn instances collapse this way. The camera's
    own 3×3 cells (about 576 m², about 180-230 k blades at 9 vertices) are drawn whole, most of them outside the view
    cone (inferred, it depends on the FOV).
- **Order:** cells are walked in `std::unordered_map` order (`grassfield.cpp:632`, `grassfield.h:174`). There is no
  front-to-back sort, so blade-on-blade early-Z is left to chance. Within a batch the blades are in rank order,
  which is spatially random.
- **Draw buckets:** grass blades are not in `DrawBuckets`. The buckets only carry the *ground under* the grass:
  - `BK_GRASS` with the packed record in `foliage` (`drawbuckets.cpp:321-324`, `grassparams.h:74-82`);
  - `BK_GROUND` / `BK_VERGE` / `BK_TRACK` with the layers' maps appended to the bindless arrays as "fringe" entries
    (`drawbuckets.cpp:330-375`; flags `drawbuckets.h:84-88`).
  These go through the normal landscape cluster or meshlet visibility and HiZ culling like any landscape.
- **Passes:**
  - Blades are drawn **only in the G-buffer pass**, after `visuals.drawGBuffer` and before particles
    (`worldview.cpp:758-763`). They are not in `drawShadow`, `drawVsm` or `drawHiZ` (`worldview.cpp:745-756`).
  - They are not in the RT scene: `GrassField` creates no BLAS or TLAS instance. The RT scene is built from
    `visuals` (inferred from the absence of any grass reference in the RT code).
  - Pipeline: `NoCull`, `LEqual`, depth write on (`shaders.cpp:136-147`). Blades then live in the depth buffer, so
    SSAO, GI contact AO and the grass contact ray see them.
  - The blade shadow is only the screen ray (§5).

---

## 3. Vertex shader per vertex (`shader/grass/grass.vert`)

Everything below runs for **every vertex**, so 9× per near blade, although most of it depends only on the blade:

| work | lines | per blade or per vertex? |
|---|---|---|
| SSBO fetch of 16 B + decode, ground normal sqrt | 112-126 | per blade |
| distance, `grow`, ring fade | 130-133 | per blade |
| depth via `viewProject*root`, pixel size, widening | 145-147 | per blade |
| billboard side vector (normalize ×2) | 156-162 | per blade |
| `foliageFrontAt`: 2 sin, pow, sqrt (`materials/foliage_wind.glsl:66-76`) | 165 | per blade (it depends on root.z only) |
| `bladeTilt`: tan, 2 sin (waves), exp+pow (`grassLeanAngle`, `grass_common.glsl:110-112`), tan; swing: 2 floor-fract, 2 sin + 2 cos; flutter 2 sin | 166-167, 55-91 | per blade |
| `bladePoint`: loop of k `bladeDir` (normalize + length) | 169, 94-109 | per vertex, O(k): a 4-link blade makes 16 `bladeDir` calls over its 9 vertices where 4 would do |
| **motion vectors:** a second `foliageFrontAt` + `bladeTilt` + `bladePoint` for the previous frame's wind | 173-181 | per blade (`bladePoint` per vertex); whenever `scene.motionVectors` (TAA on) |
| two `textureLod` + 2 `textureQueryLevels` (painted ground at mip N-6, sward map at mip N-6) | 193-194 | per blade |
| face normal: another `bladeDir`, cross, 2 normalize | 200-206 | per vertex (t, side) |

No noise and no other texture fetches. Inferred: about 40 transcendental ops and 2 texture fetches per vertex at
TAA on, of which perhaps 85 % are per-blade invariants recomputed 3-9 times. `still` makes swing and flutter fade
to 0 past 30 m, but the sines are still evaluated: the flutter has no branch (`grass.vert:87-89`). The swing is
skipped only when `sw<=0` (`:77`).

Fragment of a blade (`grass.frag:29-60`): **no texture fetch.** It does a sqrt (`grassTone`), a few mixes,
`encodeNormal`, and four MRT writes (diffuse, normal uint, ORM, motion) plus depth. Cheap ALU; the cost is ROP and
bandwidth per covered pixel and overdraw.

---

## 4. Pixel shader on the ground (`shader/materials/main.frag` + `shader/grass/*.glsl`)

Call site: `main.frag:1057-1073`, landscape G-buffer with `MATERIAL_MAPS && FLAT_NORMAL`. First `mainGBuffer`
samples the **painted** texture (`main.frag:379/381`) and, if the record gives them, its normal and ORM
(`:522-527`). Then `grassOwnGround` (`grass_track.glsl:179-199`) for `BK_GROUND|BK_VERGE|BK_TRACK`, then
`grassGround` (`grass_ground.glsl:20-79`) for `BK_GRASS`. Where the layers own the pixel (share 1,
`grass_layers.glsl:174-178`) the painted fetches are thrown away.

`fringeTexture` = one `textureGrad` from the bindless arrays (`materials/material_projection.glsl:91-107`). In this
section "noise" = `grassValueNoise` = 4 integer hashes (`grass_common.glsl:129-138`), and `grassPatch` = 2 noises =
8 hashes (`:145-147`).

### 4.1 Common to every `BK_GRASS` ground pixel: `grassGround`

- 0 texture fetches. Two `tan(radians())` of record constants (`grass_ground.glsl:39`; these could be per bucket).
- `grassPatch` (8 hashes) always (`:50`). `grassBare` only without own layers (`:46-49`, `grass_common.glsl:150-168`):
  2×2 cells × (hash + hash + grassPatch + 1 noise), up to about 56 hashes.
- `far>0` (from 24 m): `foliageFrontAt` + `grassLeanAngle` (exp, pow) + tan, decode/encode normal (`:65-77`).
- Distance gate: the far look and its wind run only beyond `ring.y*0.5` = 24 m; the graze and shade terms are
  ALU only.

### 4.2 Meadow with two layers (`BK_GROUND`, `grass_layers.glsl:47-181`)

Every pixel with `lies>0`, at **any distance**:

- `grassBorderNear` (`grass_border.glsl:13-38`): 3 SSBO reads plus a loop over up to 63 pieces of 5 words each in
  the 4 m bin (`grassborder.h:49-53`). Most meadow pixels are in empty bins: 3 reads.
- `grassCoverBorder` only when a border is near: 3 noises = 12 hashes (`grass_cover.glsl:53-65`).
- `grassCoverLevel` → `grassCoverBare` always (`grass_cover.glsl:25-50`): 4 hashes, plus, for each of the about
  35 % of 4 m cells that hold a spot, 1 hash + grassPatch (8), plus once 2 noises (8). Typical about 20-30 hashes,
  worst about 48. When the level is "between" it adds up to 3 noises (12 hashes, `:74-79`).

Texture fetches by case:

| case | fetches | lines |
|---|---|---|
| whole sward, d < 15 m | albedo + ORM + normal = **3** | 125-134 |
| whole sward, 15-30 m | 2 albedo (own + far repeat) + ORM + normal = **4** | 126-134 |
| whole sward, 30-40 m | 2 albedo + ORM = **3** | |
| whole sward, > 40 m | far albedo + ORM = **2** | 128-131 |
| sward/earth "between" (a spot rim) | ORM sward + ORM earth (+ ORM other in a notch) + albedo + normal = **4-5** | 85-99, 136-145 |
| earth of a spot | ORM + albedo + normal = **3**, + 1 noise for the spot tone (`:151`) | |
| notch by a road (`ground.border`) | + ORM of the road's loose earth, its albedo × `grassTrackTone` (4 noises = 16 hashes) | 90-96, 137-141 |

Plus the painted albedo before (1, wasted under share 1). Plus §4.1.

**Distance gates that exist:**

- normal map off past 30 m, fading from 21 m (`GrassLayerNormalTo`, `:23, 124, 165`);
- the far repeat 15-40 m, the near albedo dropped past 40 m (`:21-22, 126-131`);
- `mipGradScale` (TAA mip bias) on every `textureGrad`.

**Not gated:**

- the cover rule (spots, border, tufts' noise) and `grassBorderNear` at any distance;
- `grassPatch` at any distance;
- the ORM fetch always (its blue is the height, used for the sward/earth choice).

### 4.3 Verge strip (`BK_VERGE`, the road beside a meadow)

- `grassBorderNear` with a 180 cm limit; the pixel returns -1 quickly when it is far from a border
  (`grass_layers.glsl:65-67`).
- Inside the strip:
  - cover border (12 hashes) + cover level (20-48 hashes);
  - fetches as in §4.2;
  - the earth's end: 2 noises (8 hashes, `:103-104`);
  - if the road has a track: an early return with `strip`, and grassTrack takes over (`:110-113`).

### 4.4 Road track (`BK_TRACK`, `grass_track.glsl:51-174`), every road pixel with `lies>0`

| step | noises (×4 hashes) | fetches | gate |
|---|---|---|---|
| wander of the ribbon | 3 | | none (only `wander>0`) |
| ribbon mask | | 1 | none; the pixel exits here if the ribbon share is ≤0.004 (`:90-93`) |
| fade k | 2 | | none |
| lean (ragged) | 2 | | none |
| crust/loose ORM | | 2 near the threshold, else 1 | only d < 15 m (`far<1`, `:107-119`) |
| albedo crust/loose | | 1-2 | none |
| ribbon tint | | 1 | none |
| stains | 2 | | none |
| `grassTrackTone` | 4 | | none |
| ribbon normal | | 1 | to 60 m (`GrassTrackRibbonTo`, `:35, 148-156`) |
| layer normal | | 1 | d < 15 m (`far<1` and d < 30 m, `:157-161`) |

Total near (< 12 m): **13 noises = 52 hashes and 7-8 fetches** (+1 painted). Far (> 15 m): still 13 noises and
4-5 fetches. On a verge+track pixel the verge path (§4.3) runs first. This matches the measured +0.4-0.5 ms when
the track fills the lower screen (inferred link).

### 4.5 Border notch (meadow side, `ground.border`)

- In the 80 cm band, `notch` adds one ORM fetch of the road's loose earth (`grass_layers.glsl:79, 90-96`).
- If it wins: the loose albedo × `grassTrackTone` (16 hashes) + loose normal.
- Plus `grassCoverBorder` (12 hashes).

### 4.6 Ground under a blade is shaded anyway

The ground is drawn before the blades in the same pass (`worldview.cpp:759-761`). So every ground pixel later
covered by a blade has paid the whole §4.1-4.5 cost. The report measured the share of ground covered by blades from
the game camera: 13.5-36 % in bands of 3.5-10 m, and 38-54 % in the main grass
(`reports/engine/2026-11-14-grass-system/README.md:156-158`, before later density changes).

---

## 5. Sun screen ray over grass (`shader/grass/grass_contact.glsl`, `common/graphics/grasscontact.h`)

- **Switch:** `VF_GRASS_CONTACT` is set when `GrassContact::enabled()` and blades were drawn in the previous frame
  (`sceneglobals.cpp:200`). It is enabled by `[INTERNAL] grassContact` (0 on Metal, 1 elsewhere,
  `settings.cpp:184-188`) or `OPENGOTHIC_GRASS_CONTACT` (`grasscontact.h:16-25`).
- **Called** in `shader/lighting/direct_light.frag:290-291` for **every** sunlit pixel (`light>0`) of any material,
  the ground, stones, the hero, walls. The receiver is anything; only a blade pixel casts.
- **Parameters:**
  - the ray is 30 cm towards the sun (`grass_contact.glsl:14`);
  - 4-12 steps, one per 3 px of screen length, none under 6 px (`:15-17`, `:52-62`);
  - steps are denser near the start (`t = s^1.5`).
  - Per step: a projection, a `linearDepth` of the ray and one `texelFetch` of depth. The G-buffer diffuse
    (foliage bit) is fetched only for a candidate occluder (`:65-88`).
- **Distance limit:** it fades between 6 and 9 m view depth (`:20-21`). Past 9 m the pixel still pays a
  `viewProjectInv` multiply before the early exit (`:32-37`).
- **Which pixels pay:** everything sunlit within 9 m (the brief's measurement: DirectSunLight-VSM +0.10..0.12, which
  inferred is a full lower half of the screen at 1440p), not only meadow pixels. No tile mask, no grass-coverage
  pre-check, no half resolution.

---

## 6. Diagnostics that exist

- **Switches:**
  - `OPENGOTHIC_GRASS` (`grassparams.h:99-113`): `0` everything off (the buckets lose `BK_GRASS`/`BK_GROUND`/
    `BK_VERGE`/`BK_TRACK`, `drawbuckets.cpp:321, 332`); `1` Low ring; `ground` the ground's layers, verge and track
    with **no blades and no contact ray** (`grassfield.cpp:98, 444-445`; `grass_ground.glsl:26-27`;
    `grassDrawn=false`). Anything else means Full. `[INTERNAL] grass=0|1|2` is the same switch from the ini
    (`renderer.cpp:333-341`).
  - `OPENGOTHIC_GRASS_CONTACT=0|1`.
  - `OPENGOTHIC_GRASS_FINE=0` (no fine blades), `OPENGOTHIC_GRASS_BROAD=<0..100>`,
    `OPENGOTHIC_GRASS_PATCH=<noise>` (one patch type everywhere) (`grassdiag.h:37-70`).
- **`grass parts <mask>`** (`testbench_grass.cpp:30-37`; bits `grassdiag.h:13-29` = `grass_common.glsl:21-32`;
  passed as `grassRing.w = 1 + 2*mask`, `grassfield.cpp:446`):

  | bit | value | effect |
  |---|---|---|
  | GroundShade | 1 | the ground between blades is not shaded |
  | Far | 2 | no far look on the ground |
  | Tip | 4 | no tip tint on blades |
  | Root | 8 | no dark root on blades |
  | RootAo | 16 | no root AO on blades |
  | Normal | 32 | blades take the ground's normal only |
  | Paint | 64 | blades red, ground in distance bands |
  | **Hide** | **128** | no blade fragments, but **the VS still runs** to its early-out at `grass.vert:135`: the draw front end and the fetch stay |
  | Cover | 256 | paint the layers' cover |
  | **Layers** | **4096** | own-layer grounds drawn as painted grounds (the layers' fetches and noises gone, the blades stay) |
  | Cells | 8192 | paint squares |
  | **Track** | **16384** | the road with its verge alone (the track's fetches and noises gone) |

- **`grass stats`** / **`grass wait`** write cells, blades stored, blades drawn, fine, triangles (counted from
  instance × (2n-1), including collapsed ones), buffer bytes and waiting cells (`testbench_grass.cpp:14-23`,
  `grassfield.h:63-71`, `grassfield.cpp:619-690`).
- **Per-pass GPU time:** `OPENGOTHIC_GPU_PASSES=1` gives timestamps per debug marker (`docs/performance.md:149-176`).
  Grass has **no marker of its own**: it is inside `"GBuffer"` (`renderer.cpp:1199`, `worldview.cpp:758-763`). The
  frame profile's `triangles` is the pipeline-statistics clipping input (`docs/performance.md:321-323`).
- **How to split the GBuffer +1.2 ms with what exists** (inferred recipe, no new code):
  - (full − `OPENGOTHIC_GRASS=ground`) = blades + `grassGround` tint + contact ray;
  - (`ground` − `0`) = layers + verge + track;
  - `grass parts 128` against full = blade raster + fragment + ROP; what is left of the blades is the VS front end;
  - `grass parts 4096` = the layers alone; `16384` = the track alone; `OPENGOTHIC_GRASS_FINE=0` = fine blades.
  - A `setDebugMarker("Grass")` before `grass->draw` (`worldview.cpp:760`) would give the blades their own row: a
    one-line change, not made.

---

## 7. Gaps, ranked by expected payoff

Ranks are my judgement from the code and the measured shares. None is measured.

1. **Ground under blades is fully shaded, then overdrawn.**
   - Where: `worldview.cpp:759-761` draws the landscape, then the grass.
   - The landscape depth is already in the z-buffer from `HiZ-occluders` (`renderer.cpp:946, 1156-1160`;
     `drawcommands.cpp:490-520` draws Landscape and Static Solid/AlphaTest). Main pipelines are `LEqual`
     (`shaders.cpp:501`).
   - Change: draw `grass->draw(cmd)` *before* `visuals.drawGBuffer(cmd)`. Blades nearer than the pre-laid ground
     write their depth, and the ground's G-buffer fragments under them then fail early-Z. Blade fragments behind
     the ground are already rejected.
   - Cost: no extra vertex pass.
   - Saves the meadow/verge/track pixel cost on the share of ground the blades cover, 13-54 % near the camera
     (§4.6).
   - Risk (inferred): order-dependent writes in the G-buffer pass (blood decals come later and are unaffected);
     early-Z must stay valid. The landscape fragment does not write depth and discards only under ATEST
     (`main.frag:1037-1040, 1074-1076`), so it stays valid. The look should be byte-identical except for depth ties
     (`LEqual`).
2. **Per-vertex recomputation of per-blade work** (`grass.vert:112-167, 175-178, 193-194`).
   - Wind, front, tilt, the previous frame's tilt and two texture fetches run 3-9 times per blade.
   - Changes, one of:
     - (a) a compute pre-pass that writes per-blade tilt, colour and width into a buffer (it could also cull, see 3);
     - (b) a mesh shader with one thread per blade (Tempest has mesh shading behind `doMeshShading`, but grass
       deliberately avoids it, `grass.vert:12`);
     - (c) cheaply: compute the n+1 link points once with an unrolled loop per vertex (the O(k²) `bladePoint`
       becomes O(n)).
   - Risk: none to the look if exact. The previous-frame tilt must stay, or the TAA motion of grass suffers.
3. **No per-blade or sub-cell culling.** Only the 8 m cell frustum test (`grassfield.cpp:641-644`).
   - Blades behind the camera in the 3×3 cells around it, blades outside the view cone, blades behind hills or
     buildings (no HiZ), and blades collapsed by `grow` all run their vertex shader.
   - Change: a compute pass per frame that tests each blade (or 1-2 m sub-tiles) against the frustum and HiZ and
     writes a compacted index list + `drawIndirect` arguments. Tempest has `drawIndirect`, used in
     `drawcommands.cpp:519`. The cheap partial fix is an early-out in the VS when the root sphere (radius = height)
     is outside the frustum planes, before `bladeTilt`.
   - Risk: none to the look. HiZ culling needs a conservative blade bound (height + lean + sway).
4. **The prefix rank is taken at the cell's nearest point** (`grassfield.cpp:651, 664`; `RootsBeyond` `:39`).
   - Inferred: about 20 % of instances in mid-distance cells are drawn only to collapse.
   - Change: split draws into 2-4 distance sub-batches. This needs the blades sorted by sub-tile, then rank, which
     changes the buffer order (`grassplace.cpp:673-675`).
   - Risk: more draws.
5. **Unsorted cell order** (`grassfield.cpp:632`).
   - Change: sort the visible cells by `dist` before the loop, for blade-on-blade early-Z. Trivial.
   - Risk: none. The payoff depends on overdraw, which is unmeasured.
6. **Contact ray runs on every sunlit pixel within 9 m, whatever its material** (`direct_light.frag:290-291`,
   `grass_contact.glsl:31-89`).
   - Changes:
     - skip it unless the pixel or its 30 cm neighbourhood can hold a blade (a low-res "foliage bit" mask built
       from `gbufDiffuse`, or a per-tile flag);
     - shorten the fade (6-9 m) or cap the steps at 8.
   - Risk: contact shadow on non-grass receivers (stones, feet) is part of the accepted look.
7. **The road track's 13 value noises run at every distance** (`grass_track.glsl:74-99, 140, 37-38`).
   - Changes:
     - gate `wander` (±30 cm, `:73-79`) and `ragged` (`:98-99`) past about 25-30 m;
     - replace `grassTrackTone`'s 4 noises (16 hashes) with a baked low-frequency tone texture;
     - take the crust/loose choice from one ORM fetch beyond 15 m (already mixed).
   - Risk: road tone and wobble from afar; the "ruled lines" warning in the file header (`:13-15`).
8. **The meadow cover rule and patches run at every distance** (`grass_layers.glsl:63-70`,
   `grass_ground.glsl:44-52`).
   - `grassCoverBare` (20-48 hashes), `grassPatch` (8 hashes) and `grassBorderNear` run per pixel to the horizon.
   - Changes:
     - bake the patch noise and the cover level to a low-res world texture per chunk (they are deterministic integer
       functions);
     - beyond 40 m, use the far sward look without the earth/sward decision.
   - Risk: spots (1-2 m) and patches (2-10 m) must still read at distance; the CPU/GPU parity of blades with the
     ground (`grass_common.glsl:5-7`) must hold.
9. **The fade band 48-64 m holds a third of the ring's blades for a fade** (`grassparams.h:133-135`). This is knob 2
   of the original plan (`reports/engine/2026-11-14-grass-system/README.md:657-661`). Risk: the visible end of the
   ring.
10. **Wasted painted fetch on owned layer pixels** (`main.frag:379-381`, plus `:522-527` if the record keeps normal
    and ORM): 1-3 fetches per meadow/verge/track pixel. Change: skip `diffuseTex`/maps for `BK_GROUND` when the
    layers will own the pixel. Hard: the decision comes after the sample (`grass_layers.glsl:55-57, 101-114`). Small
    payoff.
11. **Per-bucket constants computed per pixel** (`grass_ground.glsl:39`: two `tan(radians())` of the record's
    slope). They could be packed into the bucket. Tiny.

Not gaps:

- shadow maps, VSM and RT do not draw blades at all, so they cost nothing there (§2);
- the blade fragment shader has no texture fetch;
- storage LOD by rank and level works as intended.

The quality knobs already in code are `grass=1` (measured −0.29..−0.35 ms,
`reports/engine/2026-11-14-meadow-ground/README.md:349-433`) and the ring numbers in `grassparams.h:139-143`.

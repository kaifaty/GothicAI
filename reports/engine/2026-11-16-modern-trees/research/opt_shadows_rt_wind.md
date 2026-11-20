# Forest cost in shadows, ray tracing and wind: how shipped engines cut it (research, 2026-11-18)

Scope: the shadow, ray-tracing and wind part of the measured cost of 1,024 spruces (VSM +4.3..4.7 ms, GI + trace +5 ms, HiZ occluders +1..4.4 ms, wind 4..7 ms, RT 2..9 ms; RTX 3080, 2560×1440). This note builds on `tree_tech.md` (D, E, G) and `engine_audit.md` (5–7, capability table) and does not repeat them. Labels: **[primary]** = engine docs, vendor or developer source read for this note; **[secondary]** = press, forum or search summary; **[inference]** = my reasoning, to be checked by measurement.

## Takeaway

1. **We are already in Fortnite's shadow mode, without Nanite.** Epic says that trees with animated deformation and a sun that keeps moving "heavily undercut" VSM caching. For Fortnite Chapter 4 at 60 fps they added a switch that assumes directional pages are never cached. A page cache alone will therefore not repay its cost for us. What pays is making far trees static (wind amplitude faded to zero, then a no-wind pipeline). Once they are static, a static/dynamic page split can cache them.
2. **Shadows need their own, cheaper tree.** Every engine with real shadow meshes (CryEngine shadow proxies, Horizon, UE through "LODs that drop WPO") draws a reduced, low-wind or unanimated caster. Screen-space contact shadows put back the leaf-scale detail near the camera.
3. **Ray-traced GI should not run the alpha loop on spruce cards at all.** Indiana Jones ships its non-path-traced RT mode with alpha testing off. Snowdrop (Avatar) traces LOD1–2 proxies with one untextured material each and dithers foliage by alpha *coverage*, with no texture fetch. Opacity micromaps are the high-quality option, but the RTX 3080 has no hardware OMM engine (Ada added it), so any gain there is unproven.
4. **Wind costs little in the vertex shader. What costs is everything around it:** the shader is re-run for every shadow-page draw and for the previous frame's position (motion vectors), culling bounds grow with the sway, and no shadow cache is possible. Fade the wind out by distance, one level at a time (flutter first, trunk last).
5. **Alpha-tested crowns make poor occluders and expensive ones.** GPU-driven pipelines use a few hundred best occluders plus the previous frame's depth. UE leaves foliage out of the occluder prepass by default.
6. **The forest floor's darkness can come from a cheap canopy map** (AC3/AC4 "World AO", CryEngine height-map AO), not from every card in the ray scene.

---

## 1. Shadow maps with moving alpha-tested foliage

### 1.1 Unreal VSM: what the docs actually say [primary]

Source: [Epic, "Virtual Shadow Maps" docs, UE 5.x, read 2026-11-18](https://dev.epicgames.com/documentation/en-us/unreal-engine/virtual-shadow-maps-in-unreal-engine).

- **Pages:** 128×128, cached between frames unless invalidated. Light movement invalidates all of that light's pages. A caster that moves, appears or disappears invalidates the pages its bounds overlap, as seen from the light.
- **WPO / PDO and skinning invalidate every frame.** The cache must assume a WPO caster changed, because the displacement is only known at draw time. Inflated bounds combined with a low sun angle invalidate even more pages.
- **Per-primitive "Shadow Cache Invalidation Behavior":** Auto / Always / **Rigid** (ignore WPO as an invalidation source) / **Static** (ignore WPO and transform changes; moving such a primitive gives "undefined" results). Rigid and Static are the shipped way to have *wind in the main view and a frozen cached shadow*. A forum report says Static plus the WPO disable distance works for foliage from 5.3 [secondary, [UE forum](https://forums.unrealengine.com/t/vsm-with-nanite-foliage-wpo/776406)].
- **Distance controls:** "World Position Offset Disable Distance" per primitive; "Displacement Fading" in the material; "use Mesh LODs to drop WPO/PDO at distance, or disable dynamic shadow casting for distant objects".
- **Non-Nanite meshes** are "much more expensive" in VSM. They need full LOD chains, "or they become very costly in small pages". Distance-field shadows can take over far non-Nanite casters for the sun.
- **Grass and some foliage:** "Contact Shadows alone can be a sufficient substitute for shadow maps."
- **Coarse pages** (low-resolution pages for volumetric fog and translucency) mark several directional clip levels. "Non-Nanite dynamic geometry in coarse pages can cause draw-call bottlenecks." `r.Shadow.Virtual.NonNanite.IncludeInCoarsePages 0` removes it ([cvar reference](https://indxzero.github.io/ue544cvarwiki/articles/r.shadow.virtual.nonnanite.includeincoarsepages), secondary). A known bug: stale shadows after destroying such actors ([UE-384626](https://issues.unrealengine.com/issue/UE-384626)).
- **Resolution bias:** `r.Shadow.Virtual.ResolutionLodBiasDirectional`. There is a separate bias for *moving* lights (`...DirectionalMoving`) that eases back when the light stops. This is a direct precedent for "lower shadow resolution while the sun moves, higher once it is still".
- A cvar to stop material-driven invalidation beyond a range, `r.Shadow.Virtual.Cache.MaxMaterialPositionInvalidationRange`, appears in forum advice. Users also report it has no effect [secondary, [forum](https://forums.unrealengine.com/t/vsm-with-nanite-foliage-wpo/776406)]. **Unconfirmed.**

**Nanite Foliage (UE 5.7, experimental)** [primary, [docs](https://dev.epicgames.com/documentation/en-us/unreal-engine/nanite-foliage)]: "VSM performance requires you to disable animation in the distance". Skinning stops below the "Animation Min Screen Size", default 0.1 (`r.Skinning.DefaultAnimationMinScreenSize`). WPO needs "Max WPO Displacement" set to a worst case, which "can produce overly conservative bounds and high overdraw". For our 18 m spruce (bounding radius ≈ 9 m, 16:9, 90° horizontal FOV), screen size 0.1 is reached at roughly 150–170 m [inference, UE's screen-size formula ≈ 1.78·r/d].

**Fortnite Chapter 4** [primary as quoted; the post itself returned 403, wording via search index: [Epic tech blog, 2023](https://www.unrealengine.com/en-US/tech-blog/virtual-shadow-maps-in-fortnite-battle-royale-chapter-4)]: two constraints "heavily undercut the ability to cache shadow maps: a significant amount of animated deformation—chiefly trees—and a continuously moving sun". Epic added a control that assumes "we will never cache directional light pages" (`r.Shadow.Virtual.Cache.ForceInvalidateDirectional`) to save bookkeeping. Fortnite also used "Evaluate World Position Offset" off at distance ([Nanite in Fortnite, 2023](https://www.unrealengine.com/tech-blog/bringing-nanite-to-fortnite-battle-royale-in-chapter-4), via search index).

**Meaning for us [inference]:** our VSM redraws everything every frame, which is what Fortnite ships for the sun. The difference is that Fortnite's casters are Nanite clusters culled per page, while ours are whole trees with alpha-tested cards. The lever is therefore the geometry and wind of what enters the pages, not the cache.

### 1.2 Other engines

- **CryEngine (KCD1/2 lineage) cached cascades** [primary, [CRYENGINE 3 docs, "Cached Shadows"](https://www.cryengine.com/docs/static/engines/cryengine-3/categories/1114113/pages/21267738)]:
  - From cascade N on (`r_ShadowsCache`), cascades are rendered once and only sampled afterwards. They recentre when the camera nears a border.
  - Casters are drawn at a chosen LOD (`e_ShadowsCacheObjectLod`), with per-cascade resolutions (`r_ShadowsCacheResolutions`) and about 130 MB of VRAM by default.
  - Moving objects do not update in the cache, "errors can be noticeable" (their example: a large windmill at medium range). Such entities opt into the live cascades via `DynamicDistanceShadows`. So vegetation sways in the near live cascades and is frozen, at a low LOD, in the far cached ones.
- **CryEngine shadow proxies** [primary, [docs](https://www.cryengine.com/docs/static/engines/cryengine-3/categories/1114113/pages/21268752)]: a separate shadow-only low-poly mesh. It is best built "like a low-poly LOD", and a poor match to the render mesh causes self-shadowing artefacts. UE4 documented the same workflow ([ProxyGeo shadows, 4.27](https://docs.unrealengine.com/4.27/TestingAndOptimization/ProxyGeoTool/ProxyGeoShadows)).
- **Decima (Horizon):** unanimated, reduced shadow casters at all distances, with a fixed LOD per cascade. Already in `tree_tech.md` E. Horizon Forbidden West overlapped its cascade rendering with deferred-texturing compute on PlayStation. Nixxes dropped that on PC ([Guerrilla, GDC 2022](https://guerrilla-games.com/read/adventures-with-deferred-texturing-in-horizon-forbidden-west); Nixxes interview, secondary).
- **Unity HDRP** [primary, [Shadow update mode](https://docs.unity3d.com/Packages/com.unity.render-pipelines.high-definition@17.0/manual/shadow-update-mode.html)]: Every Frame / On Enable / On Demand. Updates can be triggered by light movement past an angle threshold (`cachedShadowAngleUpdateThreshold`), which is a precedent for a sun quantised in steps.
- **Screen-space shadows (Bend Studio)** [primary, [Bend blog, SIGGRAPH 2023 code release](https://www.bendstudio.com/blog/inside-bend-screen-space-shadows/)]: Days Gone (PS4, 2019) used them for sun shadows "among a number of other Sony titles". They complement shadow maps for small details. The example code is public, licence not stated. Known failure: shadows vanish when the caster is off screen or occluded (DF analysis, secondary).
- **Frostbite, RE Engine, id Tech, Snowdrop raster shadows:** no primary statement found on tree shadow caching (see Gaps).

### 1.3 What breaks when the shadow stops moving with the tree

No source measures this. What the sources say: Horizon froze shadow casters entirely and accepted it; CryEngine calls frozen moving objects "noticeable" at medium range; UE warns that "Static" behaviour on moving geometry is "undefined". Expected artefacts [inference]:

1. **Ground shadow stands still under a swaying crown.** Visible near the camera in sun, and most visible at low sun, where the shadow is long and the motion is magnified.
2. **Self-shadowing on the crown slides.** Moving cards cross a still shadow pattern, so the light crawls over the needles. At the extreme, cards leave their own shadow (light leaks at the tips) or enter it (acne). Our through-light on cards also reads the VSM, so it flickers with the same pattern.
3. **The near/far mismatch already exists** between the moving VSM and our static ray-traced far shadow (`engine_audit.md` §7).

When players stop noticing [inference, formula to be checked on our clips]: a motion is invisible once its amplitude is below about one shadow texel. That is about one screen pixel, since VSM aims at roughly 1 texel per pixel. With a 60° vertical FOV at 1440 px, one pixel ≈ d × 7.3·10⁻⁴ m:

| Motion | Amplitude (guess, to be measured from our `reach`) | Sub-pixel beyond |
|---|---|---|
| needle flutter | 2–3 cm | about 30–40 m |
| branch push | 10–20 cm | about 140–270 m |
| trunk tip lean | 0.3–0.5 m | about 400–700 m |

So the self-shadow crawl from flutter is the first thing to remove from shadows, and the ground shadow of the trunk lean is the last.

---

## 2. Ray tracing of foliage

### 2.1 What shipped titles do with alpha-tested foliage in rays

- **Indiana Jones and the Great Circle** [primary, [NVIDIA, 2025-05-15](https://developer.nvidia.com/blog/path-tracing-optimizations-in-indiana-jones-opacity-micromaps-and-compaction-of-dynamic-blass)]: "The non-path-tracing version of the game kept ray-traced alpha-testing support disabled for performance reasons." Its RT GI is the closest analogue to our probe GI, and it treats cards as opaque. OMM numbers are in `tree_tech.md` E. New here:
  - OMMs were baked for "all vegetation", but whether the *dynamic* (refitted) vegetation BLASes carry OMMs is not stated.
  - Some OMM bakes took 30 min to hours on the CPU cooker; anything over 0.5 MB was skipped.
- **Avatar: Frontiers of Pandora (Snowdrop)** [primary slides via search index, [Kuenlin, GDC 2024](https://media.gdcvault.com/gdc2024/Slides/GDC+slide+presentations/Kuenlib_Quentin_Raytracing_In_Snowdrop.pdf); the PDF is too large to fetch]: RT objects are LOD1 or 2 with one untextured material. Alpha-tested foliage uses "simple dithering based on the alpha texture coverage": a stochastic accept with probability = the card's coverage, so there is no texture fetch per candidate. Per-pixel GI is traced at quarter resolution on consoles.
- **Alan Wake 2:** foliage is fully skinned ([Remedy](https://remedygames.com/article/how-northlight-makes-alan-wake-2-shine)). It "shares a handful of animated variants per plant model, so plants repeat their motion" [secondary, [JPR on HPG 2026 paper](https://www.jonpeddie.com/news/amd-nvidia-rework-foliage-rendering/)]: a few animated BLASes shared by many instances. A GDC session on OMM with vegetation was not recorded.
- **Cyberpunk 2077 2.1:** SER and OMM got "significant performance gains", with no foliage figure. The Portal RTX "up to 50 %" from OMM is a forum claim [secondary].

### 2.2 Opacity micromaps on our hardware

- **Vulkan:** `VK_EXT_opacity_micromap` is ratified and works with ray query through `SPV_EXT_opacity_micromap`. `VK_KHR_opacity_micromap` arrived with Vulkan 1.4.351 (2026), and NVIDIA lists it in beta drivers [primary registry; secondary news].
- **4-state vs 2-state** [primary, [DirectX blog, 2025-05-30](https://devblogs.microsoft.com/directx/omm/)]: 2-state, or 4-state forced to 2-state (`FORCE_OMM_2_STATE`), never invokes any-hit. With ray query, that means no candidate is returned to our `rayQueryProceedAlphaTest` loop at all: micro-triangles act as opaque or empty geometry.
- **Ampere:** NVIDIA's Ada whitepaper introduces the "Opacity Micromap Engine" as new in Ada's third-generation RT core, with "2x faster alpha traversal" vs Ampere ([Ada whitepaper](https://images.nvidia.com/aem-dam/en-zz/Solutions/technologies/NVIDIA-ADA-GPU-PROVIZ-Architecture-Whitepaper_1.1.pdf), via search). Microsoft says OMM is "accelerated on all NVIDIA GeForce RTX GPUs". **Whether the 3080 exposes the Vulkan extension, and with what gain, is unverified.**
  - My check on Ryzen: the driver library `libnvidia-glcore.so.615.71.09` contains both extension names, so the driver knows them.
  - `vulkaninfo` is not installed there, so per-device exposure was not checked.
- **Meaning for us [inference]:** OMM is a later, optional step behind a capability check. The robust path on a 3080 is to keep the cards out of the probe rays (2.4).

### 2.3 BLAS for wind-animated trees

- **UE** [primary, [RT performance guide](https://dev.epicgames.com/documentation/en-us/unreal-engine/ray-tracing-performance-guide-in-unreal-engine)]:
  - WPO is ignored in rays unless a component opts in ("Evaluate WPO in Ray Tracing").
  - For instanced meshes, enabling it "gives each instance its own BLAS, which raises memory pressure and slows rendering".
  - WPO in rays is culled beyond `r.RayTracing.Geometry.StaticMeshes.WPO.CullingRadius`: 120 m by source default, 50 m in the guide's example.
  - Dynamic BLAS updates are budgeted at 10–30 k primitives/frame (`r.RayTracing.DynamicGeometry.MaxUpdatePrimitivesPerFrame`), prioritised by distance.
  - `r.RayTracing.Scene.UseTracingFeedback` builds only geometry that rays actually hit.
- **Indiana Jones:** refit only, never rebuild, then compaction (already in `tree_tech.md`).
- **NVIDIA forest research (2025/26):** plants beyond an animation cutoff become "single rigid instances". Uses a partitioned TLAS (quadtrees of ≤1,024 instances), cluster LOD and OMM [secondary, JPR].
- **AMD, HPG 2026 "Ray Tracing Massive Amounts of Animated Geometry":** a static BVH inside a deforming tetrahedral cage; 80 GB / 300 ms → 1.7 GB / 3.3 ms per frame for about 25 k animated plants [secondary, JPR]. A research direction, not something to build now.
- **Common answer for diffuse GI:** static BLAS, wind ignored. We already do this. Nothing to add unless RT shadows of near leaves are ever wanted. Then: 4–8 phase variants of one refitted BLAS shared by hash (the Alan Wake 2 pattern), only within a few tens of metres.

### 2.4 Proxies, culling and what probe GI needs

- **UE/Lumen:**
  - HWRT traces a Nanite *fallback* mesh, or dedicated "Ray Tracing Proxies" (on by default in new projects), never the full raster mesh ([Lumen tech details](https://dev.epicgames.com/documentation/en-us/unreal-engine/lumen-technical-details-in-unreal-engine)).
  - RT scene culling defaults to radius 300 m and angle 1°. City Sample used 150 m and 0.5°.
  - Far Field (HLOD1 meshes) runs from 200 m to 1 km by default; the docs say to match the culling radius to it.
  - Target: fewer than 100 k RT instances after culling. "Leafy trees, bushes, grates and fences can significantly hurt ray tracing performance" ([HWRT tips](https://dev.epicgames.com/documentation/en-us/unreal-engine/hardware-ray-tracing-tips-and-tricks-in-unreal-engine)).
  - Lumen's surface cache long treated masked materials as opaque. Users report over-occlusion of sky under foliage. An opt-in `r.Lumen.HardwareRayTracing.SurfaceCacheAlphaMasking` exists, with a "barely noticeable" effect in one test [secondary, [forum](https://forums.unrealengine.com/t/lumen-surface-cache-rendered-excessively-dark-lumen/2582937)].
- **AMD Brixelizer GI, foliage** [primary, [GPUOpen, 2024-07](https://gpuopen.com/learn/brix-foliage-gi/brix-foliage-gi-foliage_in_brixelizer_gi)]. This is the key lesson for probe GI:
  - Full card quads in the GI structure **over-darken**.
  - Excluding the cards **over-brightens**, because the leaves no longer shadow indirectly, and also causes flicker in the caches.
  - The fix in their part 2 is per-hit transparency. So probes need the crown's *mean transmittance* and its rough volume, not its silhouette.
- **What a probe/surfel GI needs from a tree [inference]:**
  - correct sky visibility under and inside the crown;
  - a trunk;
  - a crown that is grey and partly transparent at the right average density.

  Options, cheapest first:
  - (a) an opaque low-poly crown shell (a few hundred to 2 k triangles), with a per-triangle or per-instance coverage dither in the ray-query loop: a hash, no texture (the Snowdrop way);
  - (b) the current cards, but with alpha read from a coarse mip, or accepted by coverage hash instead of mip 0;
  - (c) a procedural AABB per crown with analytic Beer–Lambert transmittance along the ray segment.

  Our probes already send a second, non-opaque-ignoring ray for the sky bit (`engine_audit.md` §7). With a shell that has the right density, that becomes unnecessary for trees.

---

## 3. Wind cost: where it really comes from

Measured: wind off saves 4..7 ms on 1,024 trees. The arithmetic in the shader is small. The indirect multipliers are [inference, mapped to our engine]:

1. **Re-execution per pass.** The same wind function runs in the main pass, in the occluder pass, in *every VSM page draw* (a tree touching many pages is transformed many times), and again with the previous frame's state for motion vectors. That is two evaluations per vertex in the G-buffer pass alone.
2. **Bound inflation.** The culling sphere grows by `FoliageWind::travel`, so each tree marks and touches more VSM pages and passes HiZ more often. Epic: WPO "Max Displacement" bounds are "overly conservative … high overdraw", and inflated bounds at low sun cause extra invalidations.
3. **No static path.** Wind forbids any shadow cache (1.1) and any reuse of last frame's tree depth.
4. **BLAS:** none for us (static BLAS). Keep it so.

**Distance fade, shipped values:**

| Source | What it does at distance |
|---|---|
| UE | WPO Disable Distance per primitive, Displacement Fading in materials |
| UE Nanite Foliage | animation off below screen size 0.1 (about 150–170 m for our tree) |
| Fortnite | wind evaluation switched off beyond an artist-set distance (value not public) |
| UE ray tracing | WPO in rays only within 120 m by default |
| GPU Gems 3 ch. 6 | trunk-only simulation far away, and the switch "can pop" |
| SpeedTree SDK | `m_bLodFade` flag "to transition toward simpler effects such as global motion only"; Branch Motion 2 "the most expensive wind component", only for models that need it ([wind shader configuration](https://docs.unity3d.com/speedtree-runtime-sdk/manual/wind-shader-configuration.html)) |
| Unity | SpeedTree wind quality chosen per LOD |

No shipped game publishes its metres.

**Order of removal [inference from the amplitude table in 1.3]:**

| Step | Remove | From about |
|---|---|---|
| 1 | needle flutter (sub-pixel; under TAAU it only adds noise and ghosting) | 30–40 m |
| 2 | branch push / gust front | about 120–200 m |
| 3 | trunk lean, then switch to a **no-wind pipeline/bucket** | wherever the forest still reads as alive in clips |

Fade each level's amplitude to zero over 10–20 m before the switch, so no pop occurs (the pop is the GPU Gems 3 failure). The no-wind bucket drops the previous-frame evaluation, uses tight bounds and can be cached in shadows. In the shadow passes, the same levels drop out earlier (1.3).

**Baked vertex animation / pivot textures:** Fortnite (baked sim) and Alan Wake 2 (skinning) chose them for look, not cost. Pivot Painter adds dependent texture reads per level in *every* pass, which is a cost multiplier in VSM. Not recommended for the spruce; the vertex-colour levels are enough.

---

## 4. HiZ / occlusion with foliage

- **Assassin's Creed Unity GPU pipeline** [primary, [Haar & Aaltonen, SIGGRAPH 2015](https://advances.realtimerendering.com/s2015/aaltonenhaar_siggraph2015_combined_final_footer_220dpi.pdf)]: a depth prepass of the "best occluders" (about 300), combined with reprojected last-frame depth, gives the HiZ.
- **UE:** FoliageType `bUseAsOccluder` enables a prepass so foliage can occlude, with a "possible negative performance impact" ([API 4.26](https://docs.unrealengine.com/4.26/en-US/API/Runtime/Foliage/UFoliageType/bUseAsOccluder/index.html)). It is off by default per user reports [secondary].
- **Why crowns are poor occluders [inference]:**
  - An alpha-tested crown costs a discard shader in the occluder pass.
  - It leaves holes, so HiZ max-depth stays far and little is culled behind it.
  - With wind, its bounds are inflated too.

  Our +1..4.4 ms for HiZ occluders is almost all crown cost. Use trunks (opaque, few triangles) and terrain/buildings as occluders and leave crowns out. If occlusion by dense forest matters, take it from the previous frame's full depth (two-phase culling), which is free and already includes crowns at their real coverage.

---

## 5. Sky light and GI under canopy, cheaply

- **AC3 → AC4 "World Ambient Occlusion"** [primary slides, [Wronski, AC4 lighting, 2014](https://bartwronski.com/wp-content/uploads/2014/03/ac4_gdc.pdf), technique from St-Amour, GDC 2013]: offline top-down depth of the world, 7×7 Gaussian blur, sampled at runtime to estimate the occluder height above a point (offset by the normal's xy). Applied to sky ambient only. Built for "large scale sky occlusion … from buildings and trees".
- **CryEngine Height Map AO** (3.8.4+) [primary, [docs](https://www.cryengine.com/docs/static/engines/cryengine-3/categories/1114113/pages/21890652)]: a height map of the scene around the viewer (`r_HeightMapAORange`, `r_HeightMapAOResolution`), evaluated at quarter resolution, "extremely efficient, yet highly approximate", alongside SSDO.
- **Lumen:** "Skylight Leaking" is an art knob that keeps over-occluded spaces from going black ([docs](https://dev.epicgames.com/documentation/en-us/unreal-engine/lumen-global-illumination-and-reflections-in-unreal-engine)).
- **For us [inference]:** at bake time, splat each tree's crown into a top-down **canopy map**: height of crown bottom and top, plus optical density from coverage. One map per 64 m chunk at about 0.5–1 m per texel. Uses:
  1. a sky-visibility factor for ambient and probes, so the ray scene needs only the trunk and a coarse crown;
  2. a height-field sun shadow beyond the VSM and RT range (Horizon's far cascade, in `tree_tech.md`);
  3. the Mac path without GI;
  4. `fog_sky_vis`.

  Our per-vertex crown AO covers inside the crown; the canopy map covers the ground and objects under it. Analogy only: capsule or ellipsoid occluders for indirect shadow are proven for characters (The Last of Us, Iwanicki, SIGGRAPH 2013, not re-read for this note). A crown ellipsoid in the probe trace is the same idea and is unproven for trees.

---

## 6. Ranked measures for our numbers

Order is payoff per engineering effort. Each measure is checked with `scripts/perf-content.sh` (ABBA, forest place) and with frame pairs or clips from `perf-shot.bench`. Motion is judged on clips, not stills.

| # | Measure | Targets (measured) | Expected gain [inference] | Visual risk | How to check |
|---|---|---|---|---|---|
| 1 | **Crowns out of the HiZ occluder pass**; trunks and terrain only; optionally last-frame depth for HiZ | HiZ +1..4.4 ms | most of it | more geometry passes culling behind forests; G-buffer may grow a little | GPU passes before and after; count of culled instances; total frame, not the pass alone |
| 2 | **No alpha loop on spruce in probe rays**: an opaque crown shell or coverage-hash accept (Snowdrop), cards kept out of probe rays | GI + trace +5 ms, part of RT 2..9 ms | a large part of the 5 ms | floor too dark (cards opaque) or too bright (cards excluded): the Brixelizer failure | illuminance on the forest floor vs the 1–3 % reference; probe sky-bit share; A/B frames at noon and dusk |
| 3 | **Wind fade by distance and level**: flutter off by about 30–40 m, branches by about 150 m, then a no-wind bucket with tight bounds and no previous-frame evaluation | wind 4..7 ms, spread over all passes | half or more of it | far forest looks dead; pops at the switch | clips walking towards the forest; stillness judged by the user's eye; amplitude fades over 10–20 m |
| 4 | **Shadow LOD**: a dedicated caster (fewer, larger cards; no flutter; trunk-lean only beyond the near clip levels) | VSM +4.3..4.7 ms | a large part | different shadow density or shape on the ground; crawling self-shadow if the caster and visual cards differ | ground shadow pattern pairs at noon and low sun; close-ups of the crown for leaks and acne; add a shadow-pass alpha-cutoff bias if crowns thin |
| 5 | **Screen-space contact shadow on crowns near the camera** (extend `grasscontact`) to return leaf detail lost by #3/#4 | quality, small cost | +0.2–0.5 ms (unmeasured) | halos at screen edges; vanishing off-screen casters | near-crown close-ups, moving camera |
| 6 | **VSM page cache with a static/dynamic split** and a sun updated in steps, after #3 makes far trees static | VSM cost of static content | unknown until #3 and #4 are done; Fortnite shipped without it | stale shadows, visible sun steps | sun-step clips; invalidated-page counters |
| 7 | **Canopy map** for sky visibility, far sun shadow and fog | correctness at all distances, Mac | cheap | blotchy ground darkening, wrong under single trees | ground light under single trees vs groups; Mac frames |
| 8 | **OMM** (2-state) on cards, behind a capability check | RT alpha traversal | unknown on Ampere | none if 2-state uses coverage-preserving masks | check the extension with `vulkaninfo` on the 3080; ABBA on and off |

All of this sits on top of the main lever outside this thread, the LOD chain and impostors (`tree_tech.md` C). Several measures (#3, #4) become LOD fields of the same records.

---

## Gaps / not confirmed

- The Fortnite VSM and Nanite posts returned 403. Their wording is from the search index, not read in full. WPO distances used in Fortnite are not public.
- `r.Shadow.Virtual.Cache.MaxMaterialPositionInvalidationRange` and "Static" invalidation for WPO foliage are forum-level; their behaviour by version is unconfirmed.
- Snowdrop's "dithering by alpha coverage" and "LOD1–2 proxies" come from a search-index summary of the slides. The PDF (>10 MB) could not be fetched.
- Alan Wake 2 "handful of animated variants per plant" is second-hand (JPR on an HPG 2026 paper); Remedy only confirms skinning.
- Whether the RTX 3080 exposes `VK_EXT_opacity_micromap`, and any OMM gain on Ampere, is unverified. The Ryzen driver (615.71.09) contains the extension names; per-device exposure was not queried.
- No primary source for tree shadow caching in Frostbite, RE Engine, id Tech (Indiana Jones raster shadows) or Snowdrop raster shadows.
- No published distance at which players stop noticing frozen tree shadows. The table in 1.3 is a pixel-size argument with guessed amplitudes; our real amplitudes come from the mesh's `reach` and must be measured.
- No published cost of contact shadows on foliage. Bend's code licence was not checked.
- The canopy-map, crown-ellipsoid and procedural-AABB ideas for probes are inferences without a shipped tree precedent. The Last of Us capsule occluders were not re-read for this note.

# Geometry-side LOD, density reduction and far-field replacement for dense blade grass

Scope note: primary talks (GDC/SIGGRAPH PDFs) could not be fetched (size limit) or were not surfaced. Most per-game numbers below are from secondary retellings and are marked. The 15/7-vertex Ghost of Tsushima figures are consistent across several retellings but none read the slides; a source flagged the "83,000 blades at 2.5 ms" figure as unverified.

## 1. How shipped games and engines reduce blade vertex count with distance

### Takeaway
Everyone uses a small number of discrete blade meshes (2 to 3 LODs, a handful of vertices each) chosen per tile or per blade. Ghost of Tsushima is reported at 15 vertices near and 7 far, with vertex-position blending near the switch. Public numbers for Horizon, BotW, Witcher 3, God of War, Far Cry and Frostbite blade geometry were not found in primary form.

### Cited Findings
- Ghost of Tsushima (Sucker Punch, GDC 2021, Eric Wohllaib): blades come in two versions, high LOD 15 vertices and low LOD 7. Secondary retelling (a third-party thread, not the slides). — [Stylized Station thread unroll](https://threadreaderapp.com/thread/1688939411575119872); talk listing: [GDC Vault](https://gdcvault.com/play/1027214/contactUs)
- Ghost of Tsushima: near the switch distance, vertices of the high LOD are shifted toward the low LOD shape so the change does not pop, which matters most on wind-bent blades. Secondary. — [Stylized Station thread](https://threadreaderapp.com/thread/1688939411575119872)
- Ghost of Tsushima: the blade is a Bezier curve built in the vertex shader; curvature is not made with extra vertices because that would double GPU vertex load; curved normals give a round look instead. Each tile is drawn with instanced indexed draws and no vertex streams (data generated from index and instance id). Secondary. — [Stylized Station thread](https://threadreaderapp.com/thread/1688939411575119872), [search summary of the same](https://x.com/StylizedStation/status/1688939411575119872)
- Ghost of Tsushima: compute shaders lay out blades per tile and do frustum and distance culling. Secondary. — [Stylized Station thread](https://threadreaderapp.com/thread/1688939411575119872)
- Horizon Zero Dawn grass LOD triangle counts reported as 20 to 36, 10 to 18, 10 to 18; "fidelity comes from art and shading, not triangle count". Third-party summary whose own sources were "cited but not read"; unverified. — [csarko.sh research note](https://csarko.sh/research/grass-and-trail-realism)
- Horizon Zero Dawn: with distance, animation scales to zero and vertices are pushed down so far grass sinks instead of popping; distant masked cards use a coverage-preserving mip chain. Same unverified summary. — [csarko.sh](https://csarko.sh/research/grass-and-trail-realism)
- Horizon Zero Dawn (GDC 2018, vegetation talk by Gilbert Sanders): target 30 fps at 1080p on PS4; shadow LOD selection by camera distance caused visible popping, fixed by assigning objects to shadow cascades at fixed distances (auto-generated video summary, low confidence). — [video summary](https://videohighlight.com/v/wavnKZNSYqU)
- Horizon Forbidden West: GDC talk on deferred texturing says foliage and alpha-tested geometry use a "loosely tiled deferred texturing" system. Not a grass LOD source. — [GDC Vault](https://www.gdcvault.com/play/1027553/contactUs), [Guerrilla page](https://www.guerrilla-games.com/read/adventures-with-deferred-texturing-in-horizon-forbidden-west)
- AMD GPUOpen mesh-shader grass sample: 8 vertices and 6 triangles per blade (4 samples per Bezier edge), up to 32 blades per patch (256 vertex limit / 8). No GPU timings given. — [GPUOpen](https://gpuopen.com/learn/mesh_shaders/mesh_shaders-procedural_grass_rendering/)
- Godot indie implementation: 9 triangles per blade, 5x5 m chunk with about 10,000 blades (about 90,000 triangles per chunk); the low LOD is a single triangle per blade, about 1/10 of the triangles. Cost about 2 ms per frame, about 12% of a 60 fps budget (hardware not stated). — [hexaquo grass series part 4](https://hexaquo.at/pages/grass-rendering-series-part-4-level-of-detail-tricks-for-infinite-plains-of-grass-in-godot/)
- Godot Ghost-of-Tsushima-style reimplementation: tile-based LOD, each tile has a density and a hard-coded high or low blade mesh, changed when the camera crosses tiles; tiles are culled whole. — [GodotGrass](https://github.com/2Retr0/GodotGrass)
- Unity: community Ghost-style implementations use compute plus indirect draw (e.g. cainrademan Unity-Grass, harlan0103). Not examined in detail. — [Unity-Grass](https://cainrademan.github.io/Unity-Grass/), [harlan0103](https://github.com/harlan0103/Grass-Rendering-in-Modern-Game-Engine)
- Unreal Engine landscape grass: density is instances per 10 m2 (per Python API doc); start/end cull distance per grass type, start gives a fade only if the material uses PerInstanceFadeAmount; end is a hard cluster cull; cull distance is unrelated to mesh LODs. — [UE 5.1 GrassVariety](https://docs.unrealengine.com/5.1/en-US/PythonAPI/class/GrassVariety.html), [UE forum](https://forums.unrealengine.com/t/landscape-grass-culling-distance-doesnt-work/120107)
- Unreal Engine: no built-in density falloff with distance; users make several grass types with different density and cull ranges. — [UE forum](https://forums.unrealengine.com/t/reduce-grass-density-by-distance/50086)
- Nanite foliage (UE 5.7): experimental/beta, voxel representation in the far field, relies on Nanite Assemblies and Procedural Vegetation Editor; forum/vendor material, no grass-specific numbers, one anecdote of no gain from voxelising ordinary meshes. For grass, one vendor guide recommends the landscape grass system rather than Nanite. — [UE forum](https://forums.unrealengine.com/t/new-nanite-voxel-foliage/2585978), [StraySpark guide](https://www.strayspark.studio/blog/nanite-foliage-ue5-complete-guide)
- Generic guidance from an indie talk: well designed LOD chains are 3 to 4 stages; one polygon can stand in for many blades at close range. — [David Elahee slides](https://docs.google.com/presentation/d/1ZakYwCSAM7fUcupVSY_j0Dv8j5ScoZeCkPy5XHmFvZ0/htmlpresent)

### Inferences
- A 2-LOD scheme (about 15 then 7 vertices) with morph between them is the best-documented reference point for a blade-as-geometry system. If your near blade has more than about 15 vertices, the near LOD is the first thing to compare.
- Per-blade vertex cost vs triangle count: 15 vertices is about 13 triangles in a strip, 7 vertices about 5; the second step cuts vertex and triangle load by about 2.5x on that tier.

### Gaps
- No primary data for Breath of the Wild / Tears of the Kingdom, God of War, The Witcher 3 (only a 3rd-party mention of about 10 grass types with own culling grid and draw distance, under 1 ms, unverified), Far Cry, Frostbite, Unity HDRP details.
- Exact LOD switch distances for Ghost of Tsushima were not found. Tile size claims (e.g. 8 m) were not confirmed.

## 2. Density reduction with distance without visible thinning

### Takeaway
The recurring recipe: remove blades by a stable per-blade rank, and compensate the survivors by widening (and optionally lengthening) them and/or shifting the ground colour toward the grass colour. Fractional removal (shrinking the last blade) avoids pops.

### Cited Findings
- Ghost of Tsushima: distant tiles are double size at the same blade count, so 3 of 4 blades are dropped per step (secondary). — [Stylized Station thread via search summary](https://x.com/StylizedStation/status/1688939411575119872), [csarko.sh](https://csarko.sh/research/grass-and-trail-realism)
- Ghost of Tsushima: edge-on blades are rotated or stretched in view space so they do not collapse to slivers; distant terrain blends toward grass tip colour to keep apparent density in the foreground-to-distance transition; short blades are folded into two to look twice as dense for free (secondary). — [Stylized Station thread](https://threadreaderapp.com/thread/1688939411575119872), [csarko.sh](https://csarko.sh/research/grass-and-trail-realism)
- AMD GPUOpen: blade count per patch `lerp(32, 2, pow(saturate(d / (1.05 * end)), 0.75))` (appendix; the body text uses a linear version), `ceil` for the count, last blade width multiplied by `frac(count)` to avoid pops, every blade widened by `32 / count` on a base width of 0.03; "barely noticeable in a dense meadow". — [GPUOpen](https://gpuopen.com/learn/mesh_shaders/mesh_shaders-procedural_grass_rendering/)
- Jahrmann and Wimmer (TU Wien, I3D 2017): blades are culled by orientation, view frustum, distance and occlusion; an earlier poster claims up to 75% of blades culled without noticeable change in field density. Full details not read (PDF too large). — [paper page and draft](https://www.cg.tuwien.ac.at/research/publications/2017/JAHRMANN-2017-RRTG/), [2016 poster mention via search](https://www.cg.tuwien.ac.at/research/publications/2016/JAHRMANN-2016-IGR/JAHRMANN-2016-IGR-poster.pdf)
- Breath-of-the-Wild-style web recreation: clumps shrink to zero height with `1 - smoothstep(fadeNear, fadeFar, dist)`, no separate cull pass; this is a community recreation, not Nintendo's method. — [Cinevva part 28](https://app.cinevva.com/blog/2026-05-14-open-world-browser-part-28-grass-and-occlusion)
- Dither crossfade on opaque blades loses early-Z; hand-off should be geometric (scale, sink, seam). Sub-pixel blades: quad-rasterisation overhead outweighs fill saved below about 3 px width. Clump fade must keep the footprint under the ramp length as the whole instance fades together. Secondary summary. — [csarko.sh](https://csarko.sh/research/grass-and-trail-realism)
- Unreal: "thinning" is only possible by stacking grass types of different density and cull range (see section 1).
- Day Hike (3rd party, as quoted by csarko.sh): proposed blade clumps of 24 blades x 6 triangles (144 triangles) on a 0.7 m lattice out to 10-20 m, as a clump-merging scheme; this is a plan, not shipped. — [csarko.sh](https://csarko.sh/research/grass-and-trail-realism)

### Inferences
- Stable thresholds: rank each blade with a hash of its world-space cell (not instance id; the instance id reshuffles on rebuild, per the csarko note), keep blade if rank < density(d). Then survivors widen by 1/density(d)^(~1) to conserve coverage.
- To avoid a visible per-tile step, make density a smooth function of distance per blade, not per tile.

### Gaps
- No source gives quantitative thinning curves for shipped AAA games. The GPUOpen exponent 0.75 is the only concrete curve found.
- Screen-coverage-driven density (projected blade size, not distance) was not found documented; it is an obvious generalisation.

## 3. Far-field replacement

### Takeaway
Shipped games remove blades at range and fall back to a ground texture or terrain colour that matches the grass; shell/fin and card layers appear in indie work, with fidelity concerns.

### Cited Findings
- Ghost of Tsushima: at long range, grass and other foliage are removed and replaced by an artist-authored texture (secondary). — [Stylized Station thread](https://threadreaderapp.com/thread/1688939411575119872)
- Godot impostor plane: a single plane that reproduces albedo, AO, normals and lighting from world-space textures, fading in at 5 to 10 units while the grass fades out at 10 to 20 units, using hashed alpha dither. Billboards were rejected for grass (overdraw may cancel the gain). — [hexaquo part 4](https://hexaquo.at/pages/grass-rendering-series-part-4-level-of-detail-tricks-for-infinite-plains-of-grass-in-godot/)
- Day Hike (3rd party): tufts 290 triangles near and 130 far on a 3 m lattice to 110 m; carpet 20 triangles on a 0.7 m lattice to 40 m with a near/far seam at 18 m (a hobby project, as a data point). — [csarko.sh](https://csarko.sh/research/grass-and-trail-realism)
- Shell texturing separates into visible layers at grazing angles (the view from eye height over short grass). — [csarko.sh](https://csarko.sh/research/grass-and-trail-realism)
- Unigine: grass is camera-facing quads from a 4x4 atlas, one draw per cell, with a thinning range between fade and visibility distances (as quoted, not verified). — [kooch issue summary](https://github.com/lobinuxsoft/kooch/issues/1650)
- GPUOpen lists sparse grass via billboards as future work for distance. — [GPUOpen](https://gpuopen.com/learn/mesh_shaders/mesh_shaders-procedural_grass_rendering/)
- Witcher 3 (unverified third party): per-type pigment map tints instances near and far; the whole distribution under 1 ms. — [csarko.sh](https://csarko.sh/research/grass-and-trail-realism)

### Inferences
- With a 64 m ring, the seam to the ground texture is the main look risk; a ground colour that is the average of rendered blade colour (including their self-shadowing and AO) at the seam distance is needed, ideally derived from a render of the near field.

### Gaps
- No source on a "far grass normal/roughness" trick; Nanite voxel far field is not documented for grass and is unlikely to fit a custom ring system.
- No primary data on Far Cry, Frostbite, Unity HDRP far-field.

## 4. Tile / clump generation and LOD selection

### Takeaway
Tile-based compute generation with per-tile LOD is the norm; tile transitions are the weak spot and are hidden with per-blade fades or morphs rather than tile-level swaps.

### Cited Findings
- Ghost of Tsushima: world split into tiles carrying height, material and grass-type textures; per-tile compute placement, instanced draws; LOD morphing near the switch (secondary). — [Stylized Station thread](https://threadreaderapp.com/thread/1688939411575119872)
- Godot tile LOD: "LOD swapping is very noticeable due to the tiled nature of the system"; suggested fixes tile fade-in or seeded random blade placement. — [GodotGrass](https://github.com/2Retr0/GodotGrass)
- Godot chunk size: 5x5 m, about 10,000 blades per chunk; 100 chunks (50x50 m) approaches 10 M triangles. — [hexaquo](https://hexaquo.at/pages/grass-rendering-series-part-4-level-of-detail-tricks-for-infinite-plains-of-grass-in-godot/)
- Jahrmann and Wimmer 2013: grid cell size strongly affects performance and the optimum cannot be predefined. — [2013 paper](https://www.cg.tuwien.ac.at/research/publications/2013/JAHRMANN-2013-IGR/JAHRMANN-2013-IGR-paper.pdf)
- GPUOpen: 128-thread group, 256 vertices and 192 primitives per patch, blade positions scatter in a radius around patch centre. — [GPUOpen](https://gpuopen.com/learn/mesh_shaders/mesh_shaders-procedural_grass_rendering/)

### Inferences
- Because blade positions must be a pure function of world cell, tile resolution can change without moving surviving blades.

### Gaps
- Tile sizes for shipped games not found; 8 m for Ghost of Tsushima is unconfirmed.

## 5. Reported numbers: blades, triangles, GPU time

### Takeaway
Few usable numbers exist. Your 2 M triangles at 1.2 ms on RTX 3080 is already cheap compared with the only quoted figures.

### Cited Findings
- Godot demo: about 2 ms per frame for grass, about 12% of a 60 fps budget; one chunk 90,000 triangles. Hardware unstated. — [hexaquo](https://hexaquo.at/pages/grass-rendering-series-part-4-level-of-detail-tricks-for-infinite-plains-of-grass-in-godot/)
- "83,000 blades at 2.5 ms" attributed to Ghost of Tsushima: unverified, flagged as such by the same sources. — [csarko.sh](https://csarko.sh/research/grass-and-trail-realism), [kooch issue](https://github.com/lobinuxsoft/kooch/issues/1650)
- Indie slide deck: an example scene budget of 1 M polygons in view, vegetation 10 to 20% of it. — [Elahee slides](https://docs.google.com/presentation/d/1ZakYwCSAM7fUcupVSY_j0Dv8j5ScoZeCkPy5XHmFvZ0/htmlpresent)
- Horizon Zero Dawn target: 30 fps at 1080p on PS4. — [video summary](https://videohighlight.com/v/wavnKZNSYqU)

### Gaps
- Serious Sam 4 GDC 2019 "4 million acres" slides (GPUOpen PDF) and the Jahrmann paper PDF exceeded the fetch size limit and were not read; they likely contain real timings.

## 6. Pitfalls and what not to do

### Cited Findings
- Popping on instant mesh swaps; use a blend zone or more steps; sub-pixel flicker (mip bias can mitigate); real grass looks darker from above and brighter at eye level, so a flat impostor needs view-angle compensation; ground plane lacks gaps (needs noise spots); constant bottom-to-top factor of 1.0 makes the impostor too bright; wind must be sampled identically; alpha 0 still costs, hide the object; fade distances should scale with quality setting. — [hexaquo](https://hexaquo.at/pages/grass-rendering-series-part-4-level-of-detail-tricks-for-infinite-plains-of-grass-in-godot/)
- Alpha-test discard disables early-Z for the whole draw; keep blades opaque; vertex-bound cases flipped the answer (a three.js case cut blades from 20 triangles to a 2-triangle quad). — [csarko.sh](https://csarko.sh/research/grass-and-trail-realism)
- Side-on blades look thin; the original Ghost system rotates near blades toward the camera. — [jairsantana](https://jairsantana.com/posts/grass/)
- Player-reported grass pop-in in Horizon Zero Dawn (PC patch 1.11) and Remastered streaming. — [Steam discussion](https://steamcommunity.com/app/2561580/discussions/0/4631483212915291038)
- UE: cull distance 0 reported to hide grass rather than disable culling; Nanite grass reports bald patches with fade 30,000 and end 50,000-70,000 units (a forum anecdote). — [UE forum](https://forums.unrealengine.com/t/landscape-grass-culling-distance-doesnt-work/120107), [Polycount](https://polycount.com/discussion/239439/ue5-landscape-grass-has-large-view-dependent-bald-patches)

### Inferences
- In a deferred renderer the thin-blade cost is mostly rasterisation of small triangles (quad overhead) plus vertex work, so the main levers are fewer, wider blades at distance and fewer vertices per far blade, not shading.
- Thinning at grazing angles: widen survivors in view space (the edge-on stretch from Ghost) and reduce thinning when the view is near the ground plane.

### Gaps
- No team report of "would not do again" for shipped AAA grass was found in primary sources.

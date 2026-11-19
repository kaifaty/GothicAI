# Cheaper ground material and cheaper deferred lighting under a grass meadow

Scope: adds to `reports/engine/2026-11-17-grass-cost/research/lighting_and_ground.md` and does not repeat it. New here: primary slide text of Battlefield 3 terrain (DICE, GDC 2012) and Call of Duty super-terrain / AVT (Activision, SIGGRAPH Advances 2023), the Unity relative-cost table for terrain shader variants, Unity's base-map (the minimal no-VT cache), FidelityFX tile classifiers and Bend SSS source defaults.

Trust levels used below: **P** = primary (studio slides/notes, vendor docs, shipped source), **V** = vendor blog, **T** = third-party write-up, **F** = forum/anecdote.

Method note: the two slide PDFs (BF3, Activision 2023) could not be read by WebFetch directly; the fetch tool kept a local copy and I extracted the slide text and speaker notes from it with a small zlib text dump (no extra download). Numbers quoted from them are from the slide/notes text itself.

## 1. Caching a layered terrain material so the G-buffer pass does one or two fetches

### Takeaway
Every shipped large-terrain system found bakes the blended ground into a cache (procedural virtual texture) and keeps a direct per-pixel "detail" splat only near the camera (BF3: 50-100 m). The cache is a big win when the expensive blend would otherwise run on many pixels (BF3: 10-20 ms direct vs 2.5-3 ms through the cache on PS3) and costs a small, bounded per-frame compositing budget (BF3: 0.2-0.5 ms; CoD: up to 20 pages/frame on high-end PC). The minimal non-VT form is a precomputed low-res composite used beyond a distance (Unity "base map"), or a camera-centred clipmap with toroidal update (BF3 itself used clipmap indirection).

### Cited Findings

**Battlefield 3 / Frostbite 2 (P: slide text, Widmark, GDC 2012)** — [slides PDF](https://media.contentapi.ea.com/content/dam/eacom/frostbite/files/gdc12-terrain-in-battlefield3.pdf)
- Motivation slide: with shader splatting rendered directly, terrain renders "very slowly (10-20ms)", is "not scalable in view distance", "cannot afford multi-pass". Splatting into a texture "leverage[s] frame-to-frame coherency". "Rendering full screen using the texture cost 2.5-3ms (PS3)".
- Typical PS3 GPU figures: full-screen G-buffer laydown without detail overlay and decoration 2.5-3 ms; with detail overlay 2.5-7 ms; terrain decoration 1-4 ms; virtual-texture tile compositing 0.2-0.5 ms. Tile compositing "~0.25ms/frame on Xenon".
- Key values: 32 samples per metre; 256x256 tiles with an integrated two-pixel border; atlas default 4k x 2k; two DXT5 textures (tex1: diffuse RGB + normal X; tex2: smoothness/destruction, normal Y, specular, normal Z). Typical virtual size 64k x 64k, can reach 1M x 1M.
- Indirection is RGBA8: tile index X/Y, a scale for low-res areas, and a "CLOD fade" factor used to fade smoothly from the previous LOD tile to a newly composited tile, updated each frame.
- Instead of a 4k x 4k indirection texture they used a clipmap: 6 layers of 64x64 indirection (each with its own mip chain); clipmap level resolved on CPU per draw; texture space must roughly follow world space ("not an issue for terrain"). Recentering uses wraparound so "only edges [are] affected".
- Tiles are composited on the GPU and compressed on GPU or SPU; "Tile is ready to use next frame".
- Listed issues: blurriness (tile pool too small on consoles), runtime compression quality, compositing cost offsets some gain, "limited hardware filtering support → expensive software and/or lower-quality filtering", compositing latency, render-target memory for compositing, indirection-update cost (4-6 64x64 maps with mips, only dirty areas updated).
- Resolution limit and the near path: the VT "has practical limit at around 32 samples per meter"; "detail shader splatting fills in to required sharpness (500-1000 samples per meter)". Two splatting paths: diffuse/normal/specular/smoothness splat into the VT, and a direct path that "splats details into Gbuffer"; for performance the detail view distance is limited, "typically 50m-100m".
- Memory: theoretical terrain resident set 70+ MB on PS3; shipped with a 32 MB terrain resident set by "blurriness" (a streaming mip bias: +1 saves ~70%, texel covers 2x2 pixels). They put detail such as occlusion into the cheap DXT1 colourmap (0.5 byte/sample) to hide the blurry heightfield/normal.

**Call of Duty super terrain with Adaptive Virtual Texturing (P: slides + speaker notes, Etienne, SIGGRAPH Advances 2023)** — [slides PDF with notes](https://advances.realtimerendering.com/s2023/Etienne%28ATVI%29-Large%20Scale%20Terrain%20Rendering%20with%20notes%20%28Advances%202023%29.pdf)
- Old layering: one alpha mask per layer, max 32 layers, "lots of memory", "expensive & unpredictable shaders" (nothing stopped all 32 layers on one pixel). Replaced in Black Ops Cold War by "One Material Per Vertex" (OMPV): a 1-byte index map (most prominent layer) + one BC1 colour map per terrain; the shader gathers index+colour at the 4 surrounding vertices, merges duplicates ("instead of sampling the same material 4 times, we will be sampling that material only once"), "bounded GPU cost", up to 256 layers.
- OMPV pitfalls: sharp transitions between materials (fixed with authored transition materials, which cost memory/streaming; then a runtime two-material blend "MLTM" that raises shader cost only on transition pixels). Per-vertex random rotation ("tile hiding", 16 rotations) breaks the duplicate merge, so the common case goes from 1 to 4 samples — "increased shader cost", kept for quality.
- "Vista UVs": a second, scaled-down UV set sampled again for macro detail at distance, lerped by camera distance (extra fetches; look change).
- AVT parameters (after Ka Chen, GDC 2015): up to 24 mips, 25 px/inch (10 px/cm), max 16.8 x 16.8 km; world sliced in 64 x 64 m sectors, each a virtual image of at most 16 mips whose size follows camera distance; up to 255 sectors plus a default whole-world sector; indirection 512x512 or 1024x1024, 32-bit texels updated by a compute shader from per-frame quadtree deltas.
- Rendering: terrain writes depth, a reserved stencil bit and the geometric normal in the prepass; in the opaque pass terrain is shaded by a full-screen quad on stencil-marked pixels sampling the VT ("deferred" terrain shading).
- Feedback buffer 240x136 (~32k values), written without synchronisation, read back on CPU 3 frames later; never request more than 1/3 of the cache's pages per frame; pages unused for 3 frames are recyclable.
- Compositing budget: "on low-end mobile hardware, that never exceeds 4 pages but on high-end PCs, this goes up to 20 pages per frame". Composition writes 3 uncompressed scratch G-buffers (albedo; normal; metalness/occlusion/gloss): layers by compute, splined decals (roads) and terrain decals by VS/PS, volume decals by compute; then compute-shader compression to 2x BC3 (albedo+metal; normal+occlusion+gloss) or 3x BC1. "this is at this stage that we see the greatest loss in visual quality".
- Physical textures: 2 mips (for bilinear), 4-pixel border "required for 8X anisotropic filtering", so a 256 page holds 248x248 usable texels. Physical size by quality: 13x13 pages (very low) … 30x30 (high) … 42x42 pages with 1024² indirection (ultra).
- Top 6 mip levels of VT pages are baked offline and persistent: "significantly reduce GPU pressure", used at distance, also saves memory.
- Pitfalls: fast 180° turns or teleports show low-res VT for a few frames (fix: always request one medium page per sector, pre-stream at teleport targets); compositing before source mips stream in → composite immediately and mark "low-rez", recomposite later.
- Virtual height map: 144 pages of 64x64 (+2 px border), 32 MB → ~1.2-1.3 MB, 8 KB indirection buffer; decompressing a 68x68 page with their predictor takes ~1 ms on PS4.

**Far Cry 4 (P via listings; slides not readable)**
- AVT in a 10 km x 10 km world at 10 texels/cm, "20 times higher" than traditional VT. — [GDC Vault listing](https://gdcvault.com/play/1021760/contactUs)
- Japanese talk report (T): virtual texture about 512k px square with about 9k px square resident at once. — [Game Watch](https://game.watch.impress.co.jp/docs/news/20150308_691757.html) (reported by search summary; the page itself not fetched)
- Far Cry 5 terrain talk (GDC 2018) covers GPU LOD/culling/stitching; no source found that it changed the shading cache. — [GDC Vault](https://gdcvault.com/play/1025261/contactUs)

**Minimal no-VT versions**
- Unity terrain "Base Map Distance": beyond it the terrain renders a precomputed composite ("base map", at Base Texture Resolution) instead of the per-layer splat shader; set per tile or as a project-wide quality override. (P) — [Unity manual, Terrain settings](https://docs.unity.com/en-us/engine/current/manual/creating-environments/script-terrain/terrain-other-settings). Default value not stated on that page.
- Clipmap texture caches: nested camera-centred grids updated incrementally with toroidal addressing; update "as many levels as possible within a prescribed budget", so fast motion costs detail instead of stalling (P, Hoppe geometry clipmaps). — [Losasso & Hoppe](https://www.hhoppe.com/geomclipmap.pdf). Forum view (F): clipmaps are ideal for smooth motion, but a teleport must refill the whole window. — [gamedev.net](https://gamedev.net/forums/topic/553790-clipmaps-vs-sparse-virtual-textures-aka-mega-texture/)
- BF3 itself used clipmap-shaped indirection with wraparound recentering (see above), i.e. the clipmap idea is shipped practice.
- Unreal RVT (already in the earlier note): layer blending in the cached part, light camera-dependent part; per-frame upload caps.

### Inferences
- Our far/road views (0.31-0.39 ms of ground shading where much ground is visible) are exactly BF3's case: the blend runs on many far pixels whose detail is sub-pixel anyway. A cache would move that to a per-update cost; blade-covered pixels already cost ~0 so a cache gains nothing near grass.
- Minimal design implied by BF3 + Unity: (a) keep the full procedural shader within a near radius (BF3: detail splat to 50-100 m; for a 1440p view probably ~20-40 m is enough for us, to be measured); (b) beyond it, fetch a camera-centred clipmap of the already-blended ground (albedo, normal, ORM) composited by a compute pass only on toroidal edge strips when the camera moves, plus on edits; (c) crossfade over a few metres by distance (BF3 used a per-tile CLOD fade). Memory example (own arithmetic): a 1024² level at 8 texels/m covers 128 m; uncompressed RGBA8 x 3 targets = 12 MB per level, 2x BC3 = 2 MB per level; 3-4 levels (128 m … 1 km) stay under ~50 MB uncompressed. 32 texels/m (BF3's practical VT density) on a 1024² level would cover only 32 m.
- Update cost scales with new texels per frame: walking at 5 m/s with 8 texels/m adds ~40 texel columns/s per level — trivial; the cost to watch is a full refill (teleport, load), which should be amortised over frames with the near shader covering the gap.
- Look changes: the cache low-passes everything finer than its texel (sub-25 cm hash-noise patches, height-map coverage edges); 1-10 m tone patches survive. Normals in the cache must be world-space (Epic's advice) and the near/far crossfade must match mip-filtered averages of the near shader, or a ring will show. Grass root colour can read the same cache (one source of truth, see the earlier note).
- Filtering pitfalls carried over: a 2-4 texel border per tile/strip is needed for anisotropic filtering (CoD: 4 px for 8x aniso); a clipmap without tiles avoids per-page borders but needs level selection and blending across levels (or a texture array with per-level UV and a distance blend).
- No-cache middle step à la OMPV: storing the winning layer per vertex/texel instead of evaluating several noises per pixel bounds cost, but our coverage rule is per-pixel by design; caching the rule's output (a coverage/tone mask texture) is a cheaper partial cache that keeps per-pixel layer sampling.

### Gaps
- Ka Chen's GDC 2015 slides and the Far Cry 5 slides returned HTTP 403; GPU Pro 7 chapter not accessible. No modern-GPU ms for RVT/VT compositing on PC (CoD gives page counts, not ms).
- No published "near radius" for a full shader on a modern PC at 1440p; BF3's 50-100 m is a PS3-era figure at 32 texels/m.
- No shipped description of a non-VT camera-centred material clipmap with numbers (only Unity's base-map and BF3's clipmap indirection).

## 2. Cheaper layered ground shading without a cache

### Takeaway
Texture fetches, not ALU or blend math, dominate layered-terrain cost: Unity's measured table shows a dithered single-layer array shader at 0.40x the baseline with 6 samples versus 13 for the baseline, while simple vs height blending cost the same. Bounding the number of layers per pixel (CoD OMPV) and dropping extra UV sets/rotations are the other published levers.

### Cited Findings
- Unity Shader Graph terrain samples, cost relative to Terrain Lit (time of a terrain-only scene minus an empty shader) (P): CHNOS Array Dither 0.40 (6 samples); CSNOH Simple Blend 0.56 (9); CSNOH Simple Height 0.56 (9); CNM Simple Blend 0.88 (13); Terrain Lit 1.00 (13); CHNOS Array Hex 1.64 (14); CSNOH Rotation Height 1.68 (21); CSNOH Procedural Height 2.56 (17); CSNOH Hex Height 3.36 (25); CNM Hex Height 4.08 (37); Parallax Height 5.20; Hex Parallax 19.8. "Texture samples drive cost more than shader math"; the CSNOH packing is ~40% cheaper than CNM with 31% fewer samples; simple and height blending cost nearly the same but height blending looks much better; dither gets four PBR layers at under half the baseline. — [Unity docs](https://docs.unity3d.com/Packages/com.unity.shadergraph@17.3/manual/Shader-Graph-Sample-Terrain-Performance.html)
- A Godot triplanar stochastic terrain shader reports ~30-35% slower than a similar non-stochastic one (T). — [godotshaders](https://godotshaders.com/shader/triplanar-stochastic-terrain-shader/) (via search summary)
- CoD: one material per vertex with duplicate merge gives one sample set in the common case; random per-vertex rotation raises it to 4; transition blending only on transition pixels; vista UVs add a second sampling at distance (P). — [Activision 2023](https://advances.realtimerendering.com/s2023/Etienne%28ATVI%29-Large%20Scale%20Terrain%20Rendering%20with%20notes%20%28Advances%202023%29.pdf)
- BF3 put occlusion detail into the cheap colourmap rather than sampling more high-res rasters (P). — [BF3 slides](https://media.contentapi.ea.com/content/dam/eacom/frostbite/files/gdc12-terrain-in-battlefield3.pdf)

### Inferences
- For our two world layers (albedo+normal+ORM each = 6 fetches) plus road (3 UV maps + 2 world layers): packing to 2 textures per layer (e.g. albedo+height, normal XY+roughness+AO — the CSNOH-style packing) cuts fetches by ~1/3, which Unity's table ties to ~40% cost cut. Look-neutral if quantisation is acceptable.
- Dithered choice (sample only the winning layer per pixel, with the noise rule deciding, TAA resolving the dither) halves layer fetches in the blended zones; look change: grainy transitions before TAA, possible shimmer under motion; our rule already picks per pixel, so fetching both layers only within a narrow blend band is a branchless-ish saving with no look change outside the band.
- Distance LOD: beyond a radius drop the normal and ORM fetches (use the geometric normal and a constant ORM) and keep albedo; this is the same idea as BF3's limited detail distance. Look change: flatter far ground; combine with the cache from §1 instead if built.
- Hash noise vs texture: the evidence says ALU is cheap relative to fetches, so replacing several hash noises by one small tiling noise texture saves only if the hashes are numerous or high-octave; measure before changing. Pre-baking the noise-driven coverage/tone into a world-space mask texture (one fetch) is the larger saving and changes the look only by its resolution.

### Gaps
- No absolute ms for any of these on a desktop GPU; no published numbers on hash-noise vs noise-texture cost; no published layer-LOD distances.

## 3. Deferred-lighting cost on foliage pixels

### Takeaway
Shipped engines that did publish detail route terrain/foliage through a stencil- or tile-selected special path (CoD: terrain stencil bit → full-screen deferred terrain shading; Horizon Forbidden West: compute "loosely tiled deferred texturing" for foliage and alpha-tested geometry). I found no primary source with numbers for a per-blade pre-lit or forward-lit grass inside a deferred frame, nor for stencil-gated SSAO/GI skipping on grass.

### Cited Findings
- CoD super terrain reserves a stencil bit in the prepass and shades terrain in the opaque pass with a full-screen quad on stencil pixels (P). — [Activision 2023](https://advances.realtimerendering.com/s2023/Etienne%28ATVI%29-Large%20Scale%20Terrain%20Rendering%20with%20notes%20%28Advances%202023%29.pdf)
- Horizon Forbidden West: visibility-buffer prepass, analysis and shading in compute, "loosely tiled deferred texturing" to speed up foliage and alpha-tested geometry; overlaps G-buffer and cascaded shadow rendering, enables software variable-rate shading; no ms on the page (P, abstract). — [Guerrilla](https://www.guerrilla-games.com/read/adventures-with-deferred-texturing-in-horizon-forbidden-west). The .pptx deck exceeds the fetch tool's 10 MB limit.
- Horizon Zero Dawn vegetation talk (GDC 2018): grass got "a more dedicated shader" (search excerpt only) (T). Mirror page failed on TLS; no details.
- Crysis 3 reserves one stencil bit for a per-sample MSAA mask with read/write bitmasks to coexist with other stencil users (P via slideshare listing) — useful pattern for a "grass" stencil bit. — [Crysis 3 GDC 2013](https://www.slideshare.net/slideshow/rendering-technologies-from-crysis-3-gdc-2013/25052434)
- Ghost of Tsushima grass: secondary claim of ~83,000 visible blades in ~2.5 ms; not verifiable against the talk (T). — [hashnode](https://hashnode.com/posts/what-we-can-learn-from-grass-in-ghost-of-tsushima-renders/69e130b2ffbb787634eb9ef4)

### Inferences
- Our +0.06-0.09 ms in the ambient pass and +0.01-0.06 ms in GI is plausibly divergence and extra probe/AO work on grass pixels that differ in depth/normal from neighbours (no source). Cheap test: a G-buffer flag/stencil bit for "grass" and a branch that uses a reduced ambient (e.g. probe irradiance with the blade's height-AO ramp, no short AO ray) — look change limited to fine ambient occlusion inside the sward.
- A pre-lit/forward path for blades would remove them from all deferred passes but loses VSM/GI consistency; given the deferred extras on grass total ~0.15-0.3 ms, a flag-based cheap path is the smaller step.

### Gaps
- No primary numbers for grass skipping SSAO/GI, for per-vertex-lit grass in a deferred engine, or for half-resolution contact shadows with depth-aware upsampling.

## 4. Tile classification for screen-space ray passes

### Takeaway
Published classifiers are cheap compute passes that produce either a compact ray list (FidelityFX SSSR: per-pixel test, atomics into a ray buffer, indirect dispatch) or a packed bitmask per small tile (FidelityFX shadow denoiser: one bit per pixel in an 8x4 tile = one uint, packing skippable if the ray pass writes it). Bend SSS has no classifier: it dispatches 64-wide waves along rays from the sun's screen point and relies on early outs. No published ms for the classifier itself was found.

### Cited Findings
- FidelityFX shadow denoiser: ray results packed into a 32-bit uint per 8x4-pixel tile; the packing pass can be skipped if the ray-trace pass writes the bitmask directly; tile classification skips tiles "without spatial variance in the shadow mask"; 17x17 variance kernel separated (289 → 34 taps, 18 scalar loads with compressed masks) (P). — [FidelityFX denoiser docs](https://gpuopen.com/manuals/fidelityfx_sdk/techniques/denoiser/); [repo](https://github.com/GPUOpen-Effects/FidelityFX-Denoiser)
- FidelityFX SSSR tile classification: full-screen compute decides per pixel if a ray is needed (roughness above `roughnessThreshold` → environment-map fallback written directly); rays appended to an R32_UINT ray list with an atomic counter, also counting 8x8 denoiser tiles; `samplesPerQuad` (min rays per 2x2 quad, neighbours copy when < 4), raised to 4 per quad where last frame's variance exceeds a threshold. Reflection denoiser: tile pass skips non-reflective areas; mirror at full rate, glossy at quarter rate (P). — [FidelityFX SSSR docs](https://gpuopen.com/manuals/fidelityfx_sdk/techniques/stochastic-screen-space-reflections/); [denoiser docs](https://gpuopen.com/manuals/fidelityfx_sdk/techniques/denoiser/)
- Bend SSS (shipped source as used in a public mod): `WAVE_SIZE 64`, dispatch `numthreads[WAVE_SIZE,1,1]`; `SurfaceThickness` recommended 0.005 (0.5% of depth); `BilinearThreshold` 0.02; `ShadowContrast` 2-4; an optional early-out "does have some overhead (~15%) in worst-case where nothing early-outs" (comment in the source); targets DX12 DXC, tested on PS5 (P, source). — [bend_sss_gpu.hlsli in Community Shaders](https://raw.githubusercontent.com/doodlum/skyrim-community-shaders/main/features/Screen-Space%20Shadows/Shaders/ScreenSpaceShadows/bend_sss_gpu.hlsli)
- A mod PR makes Bend's `SAMPLE_COUNT` resolution-independent and FOV-driven, since it sets the maximum pixel-space ray length (F/T). — [PR #2219](https://github.com/community-shaders/skyrim-community-shaders/pull/2219)
- Bend's presentation and code are distributed only as zip downloads (not fetched under the no-download rule). — [Bend SIGGRAPH page](https://www.bendstudio.com/siggraph)

### Inferences
- Our 16x16-pixel occupancy mask costs 0.02-0.10 ms as a separate pass; the cited designs suggest folding it away: write a "grass, near" bit during the G-buffer pass (stencil or a G-buffer flag bit) and let the sun pass branch per pixel, or build a compact tile list with one atomic per 8x8 tile from a wave ballot in the G-buffer resolve — avoiding a full-screen read of depth just to classify. A conservative tile mask from rasterised blade-patch bounds is another option (no source found).
- Bend's 0.5%-of-depth thickness and 64-wide line dispatch are a different scheme from our per-pixel 4-12 step ray; adopting it would change the look (longer reach, relative thickness) and is not needed for a 30 cm ray.

### Gaps
- No published cost of any classifier pass in ms; Bend's slides (zip) and step counts not retrieved; no shipped case of building the mask inside the G-buffer pass with numbers.

## 5. Published before/after numbers (new in this round)

- BF3 (PS3): direct shader splatting 10-20 ms → full-screen laydown from the procedural VT 2.5-3 ms; with near detail overlay 2.5-7 ms; VT tile compositing 0.2-0.5 ms/frame (~0.25 ms on Xenon) (P). — [BF3 slides](https://media.contentapi.ea.com/content/dam/eacom/frostbite/files/gdc12-terrain-in-battlefield3.pdf)
- BF3 memory: 70+ MB theoretical terrain resident set → 32 MB shipped via streaming mip bias; one step of bias saves ~70% (P). — same
- CoD AVT: compositing ≤4 pages/frame on low-end mobile, up to 20 on high-end PC; physical pool 13x13 … 42x42 pages of 256² (248² usable); virtual height map 32 MB → ~1.2 MB (P). — [Activision 2023](https://advances.realtimerendering.com/s2023/Etienne%28ATVI%29-Large%20Scale%20Terrain%20Rendering%20with%20notes%20%28Advances%202023%29.pdf)
- Unity terrain variants: dithered array 0.40x baseline; packed simple blend 0.56x; hex tiling 3.1-4.1x; procedural noise height 2.56-3.56x (P, relative only). — [Unity docs](https://docs.unity3d.com/Packages/com.unity.shadergraph@17.3/manual/Shader-Graph-Sample-Terrain-Performance.html)
- Bend SSS early-out overhead ~15% worst case (P, source comment). — [bend_sss_gpu.hlsli](https://raw.githubusercontent.com/doodlum/skyrim-community-shaders/main/features/Screen-Space%20Shadows/Shaders/ScreenSpaceShadows/bend_sss_gpu.hlsli)

### Gaps
- Not found: Ghost of Tsushima terrain shading/caching (the Pohlmann "Samurai Landscapes" talk slides not located), Horizon terrain shading costs, any modern-PC ms for VT/RVT compositing, any before/after for grass skipping ambient/AO/GI.

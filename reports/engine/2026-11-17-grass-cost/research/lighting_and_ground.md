# Cheap lighting, shadowing and ground-material cost under grass fields

Scope note: web search was thin on primary slide text (GDC/SIGGRAPH PDFs were mostly unreadable through the fetch tool). Where a number comes only from a secondary write-up or a port, that is stated. Several items the brief asked for (Ghost of Tsushima shadow method, Horizon shadow method, God of War, Bend's own step counts, Frostbite per-layer costs) could not be confirmed and are listed under Gaps.

## 1. Grass self-shadowing without real shadows (root darkening, clump terms, shadow proxies)

### Takeaway
The common shipped-game pattern is to not let grass cast into shadow maps (it is the dominant cost) and to fake self-shadowing with a height-based darkening ramp plus colour tuning, optionally combined with SSAO or contact shadows. Confirmed primary-source evidence for named AAA titles was limited to Call of Duty: Black Ops 4 (via a third-party write-up) and generic engine guidance.

### Cited Findings
- Call of Duty: Black Ops 4 grass (author's write-up): self-shadowing was faked with a procedural per-blade "dome" mask that controls where specular may appear; a darkening height ramp applied mostly to albedo was the main AO source, faded out at long range and up close where it breaks down; deferred renderer, translucency faked by flipping normals and boosting saturation when facing away from the sun. — [c0de517e, vegetation part 2](https://c0de517e.com/017_vegetation_part2.htm)
- Real grass shows tips bright and bases dark because the base is occluded by surrounding grass; at steep sun angles shadowing is visible on the sides of tufts. No numbers. — [hexaquo, grass rendering theory](https://hexaquo.at/pages/grass-rendering-series-part-1-theory/)
- A Ghost-of-Tsushima-inspired open-source recreation (cites the GDC talk "Procedural Grass in Ghost of Tsushima") darkens each blade by model-space height as AO; its grass shadow-mapping option is on by default "at a very high performance cost", and a very close look can be had by changing base and tip colours. This is a recreation, not Sucker Punch's own method. — [GodotGrass repo](https://github.com/2Retr0/GodotGrass); talk listing: [GDC Vault](https://gdcvault.com/play/1027214/contactUs)
- GPU Gems 3 vegetation chapter: precomputed AO stored in vertex colour alpha; all grass shading done per vertex because of fill-rate. — [GPU Gems 3 ch.16](https://developer.nvidia.com/gpugems/GPUGems3/gpugems3_ch16.html)
- Unreal forum measurement (one scene, 8 km landscape, Megascans grass): ShadowDepths 29.84 ms with grass, 46.24 ms close to grass, 13.95 ms without grass; PrePass 23.60 vs 3.56 ms. Advice: set grass cull distance, disable cast shadow on the farthest LODs. — [UE forum](https://forums.unrealengine.com/t/grass-performance-with-dynamic-light-and-what-is-prepass-ddm/150981)
- Another UE report: ShadowDepths 1.42 ms for a 1-component landscape vs 10.84 ms for 100 components with the same grass (draw-call count amplifies shadow cost); one commenter: grass should never cast shadows. Landscape Grass Type has separate cast-dynamic-shadow and cast-contact-shadow toggles. — [UE forum search results](https://forums.unrealengine.com/t/grass-is-raytraced-by-default-in-4-26-resulting-in-huge-performance-drop/211376/7), [FGrassVariety API](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Landscape/FGrassVariety?lang=en-US)
- UE5 forum thread notes virtual-shadow-map cache invalidation from animated (wind-moved) grass as a major pitfall. — [UE forum](https://forums.unrealengine.com/t/grass-optimisation-in-unreal-engine-5/1152397)

### Inferences
- Because blade shadow-map cost scales with instance/draw count and wind invalidates cached shadow pages, "grass casts nothing, ground receives a baked/analytic term" is the cheap default; a shadow-caster distance of 0 for grass is consistent with all sources found.
- A root-to-tip ramp (albedo and/or AO) plus per-clump random tint reproduces most of the self-shadow look; the ramp should fade out with distance (BO4 fades it at range) to avoid a visible darker carpet and to avoid double-darkening with SSAO.
- A height-field/proxy shadow (heightmap of grass height used as a sun-ray occluder for the ground) is a plausible cheap analogue of the shadow-proxy idea mentioned in community threads but I found no shipped citation.

### Gaps
- No primary-source statement of whether Ghost of Tsushima, Horizon Zero Dawn, God of War, or Unreal Engine's default sample projects let grass into shadow maps and to what distance. GDC Vault slides for Tsushima not retrievable.
- "Grass shadow from a height field" has no citable shipped example.

## 2. Screen-space contact shadows (steps, range, upsampling, cost, grass pitfalls)

### Takeaway
Published numbers are scarce but consistent: ~60 samples per ray in Bend's default, thin ray lengths of tens of centimetres to ~1 m, per-pixel masks that touch under 1% of pixels, and costs from ~0.1 ms (RTX 4090 class, ~1600x900 to 2560x1369) up to 0.5-1.3 ms on a base PS4 at 1080p. Your 0.1-0.3 ms at 1440p on an RTX 3080 is in line with these.

### Cited Findings
- Days Gone (PS4, 2019) was the first title to use Bend's screen-space shadows for sun light; the technique complements shadow maps for small detail; code is public, Apache-2.0 (author Graham Aldridge); SIGGRAPH 2023 slides exist. — [Bend blog](https://bendstudio.com/blog/inside-bend-screen-space-shadows), [Mittring on X](https://x.com/MittringMartin/status/1694037342506598878)
- Bend's algorithm as ported in a viewer (secondary source, port of Bend code): each pixel checks up to 60 depth samples toward the sun's screen position; hit if a depth lies within 0.5% of the pixel's depth after perspective-correct adjustment ("each depth counts as a shell of 0.5% of its distance"); workgroups 64 wide marching along the ray, early exit on sky; up to 8 dispatches arranged around the sun's screen point; integer texel loads instead of a border sampler. Measured on RTX 4090 at 2560x1369: 0.17-0.26 ms including depth copy and extra casters; 0.105 ms at 1600x900; the mask changed only 0.4-0.8% of pixels. Pitfalls: thick occluders shade only near their edges; objects floating a few cm above ground show a gap; depth-prepass-only casters miss alpha-blended or impostor geometry (alpha >= 0.5 cards had to be added). Final shadow = min(shadow map, contact mask). — [Aurora-Viewer PR #524](https://github.com/Aurora-Viewer/Aurora-Viewer/pull/524)
- Light behind the camera: ray divergence/thinning because the stored w becomes negative; relevant when the sun is behind the viewer. — [Kostas Anagnostou on X](https://x.com/KostasAAA/status/1695464235852087589)
- Unity HDRP contact shadows: parameters Length (metres, also max detail distance), Distance Scale Factor, Min/Max Distance with Fade In/Out, Bias (reduces self-shadowing, too high gives peter-panning), Thickness (fills holes, too high widens shadows), Sample Count (quality vs cost); documented cost 0.5-1.3 ms on base PS4 at 1080p; only the dominant light renders contact shadows; seam with realtime shadows is reduced by higher shadow-map resolution. HDRP API default length 0.15 (range 0-1) in one version. — [HDRP docs](https://docs.unity3d.com/Packages/com.unity.render-pipelines.high-definition@14.0/manual/Override-Contact-Shadows.html), [HDRP 6.5/7.x docs](https://docs.unity3d.com/Packages/com.unity.render-pipelines.high-definition@7.7/manual/Override-Contact-Shadows.html)
- Unreal: contact shadows are a per-light length (screen-space units by default, with an in-world-space variant via CVars); grass can opt in or out via the contact-shadow toggle on the Grass Type. Sample count/cost not documented in found pages. — [CVar wiki (auto-generated, UE 5.4)](https://indxzero.github.io/ue544cvarwiki/articles/r.contactshadows)

### Inferences
- Costs scale with (pixels traced) x (steps). Restricting the ray to grass pixels via a material mask/stencil, plus a distance limit (HDRP's Max Distance concept) and a short length, are the levers the engines expose; running at half resolution with bilateral upsample is a standard extension but I found no published numbers for it.
- Thin blades: depth-based tracing against grass depth can self-hit (needs bias) or miss thin blades (needs thickness); both the HDRP bias/thickness trade-off and Bend's relative-depth shell indicate noise on thin geometry. Jitter plus temporal accumulation is the usual fix but no grass-specific published case was found.
- A relative-depth thickness (like Bend's 0.5%) means the effective thickness grows with distance, which helps grass far away but over-shadows nearby.

### Gaps
- Bend's original slide numbers (default step count in the original, reads per thread, cost on PS4) not retrieved; only a third-party port description.
- No published half-resolution contact-shadow timings; no Frostbite or Unreal contact-shadow ms; no grass-specific noise/bias writeups found.

## 3. Ambient occlusion on grass (SSAO skip/reduce, per-vertex or height AO, masks)

### Takeaway
Grass is usually given its occlusion by authored/analytic height AO rather than screen-space AO; screen-space AO on grass tends to be redundant with that ramp. I found no primary source that documents explicitly skipping SSAO on foliage pixels via stencil.

### Cited Findings
- BO4: height ramp on albedo as the main AO source, faded at distance and when close. — [c0de517e](https://c0de517e.com/017_vegetation_part2.htm)
- Godot grass: height-based vertical darkening as AO. — [GodotGrass](https://github.com/2Retr0/GodotGrass)
- GPU Gems 3: AO baked into vertex alpha. — [GPU Gems 3 ch.16](https://developer.nvidia.com/gpugems/GPUGems3/gpugems3_ch16.html)
- Community guidance for foliage cards: bake AO into vertex colour and feed into the AO input. — [polycount](https://polycount.com/discussion/comment/2677665) (forum, low confidence)
- Bent-normals for foliage as a way to get occlusion-aware ambient lighting (notes page). — [Bent Normals for Foliage](https://3dreference.notion.site/Bent-Normals-for-Foliage-5b39cf6ef6ca4df4bb089bdbe521b0aa)
- Community-shaders write-up says Bend SSS provides fine-scale occlusion for grass and pebbles that shadow maps miss. — [DeepWiki summary](https://deepwiki.com/alandtse/open-shaders/4.2-shadow-features) (secondary, auto-generated)

### Inferences
- To avoid double darkening and save cost: mask SSAO (and the short GI-probe AO ray) off grass pixels and let the height ramp supply AO; if kept, lower strength. Not documented by a shipped title in my sources.

### Gaps
- No source on stencil or material-ID gating of SSAO for foliage in a shipped game; no ms numbers.

## 4. Layered terrain materials (projection, height blend, virtual texturing, packing, LOD of layers)

### Takeaway
The standard answer is to cache the blended ground in a virtual texture so the G-buffer pass does one fetch; Battlefield 3 and Far Cry 4 did this, and Unreal RVT documents the same split. Per-layer cost numbers were not found.

### Cited Findings
- Unreal RVT: content is "a shading cache"; works best with Landscape because complex materials are cached; layer blending belongs in the camera-independent, cacheable part, keep the camera-dependent sample part light; Landscape "Virtual Texture Num LODs = 0" renders each component as one quad; Skip Mips and Min Coverage (value 3 culls under 8 px) cut render work; `r.VT.RVT.TileCountBias` -1 halves resolution; `r.VT.MaxUploadsPerFrame` 64 editor / 8 game; stat `virtualtexturing`. — [Epic RVT docs](https://dev.epicgames.com/documentation/en-us/unreal-engine/runtime-virtual-texturing-in-unreal-engine)
- RVT packing: Base Color = BC1 (colour shift/banding possible; YCoCg adds ~25% memory and decode); BaseColor+Normal+Roughness+Specular = BC3+BC3 (tex1: base colour + normal X; tex2: roughness, specular, normal Z, Y); YCoCg variant BC3+BC5+BC1; World Height R16_UNORM; Mask4 BC3; world-space normals recommended for blending. Sampling RVT from the vertex shader is possible but generates no GPU feedback (risky). — same Epic page.
- Community RVT measurements: only "slight" gains, bigger on low-end GPUs; stretching at steep angles. — [Medium, RVT landscape performance](https://medium.com/gamedev4k/use-runtime-virtual-textures-to-improve-landscape-performance-in-unreal-engine-e927f77f3dcc) (blog, anecdotal)
- Battlefield 3 terrain (secondary summary of the GDC 2012 slides): direct shader splatting was 10-20 ms and could not scale with view distance; baking splat results into textures allows reuse across frames; a full-screen pass reading that texture costs 2.5-3 ms on PS3; 64x64 indirection per LOD array (per a GitHub write-up, unsure). — search summary of [GDC12 BF3 terrain slides](https://media.contentapi.ea.com/content/dam/eacom/frostbite/files/gdc12-terrain-in-battlefield3.pdf) (PDF could not be read by my tool; figures unverified against the slides) and [PVTUT](https://www.github.com/ACskyline/PVTUT)
- Far Cry 4 adaptive virtual texturing: 10 km x 10 km world at 10 texels/cm, claimed 20x higher than traditional virtual texturing. — [GDC Vault](https://gdcvault.com/play/1021760/contactUs)
- Far Cry 5 terrain talk: GPU compute LOD/culling/stitching of the height-field terrain (no shading numbers retrieved). — [GDC Vault](https://gdcvault.com/play/1025261/contactUs)
- Horizon-style/BO4-style grass avoids the terrain shader: blades are solid and occlude the expensive terrain shader beneath them (so the ground under dense grass is depth-rejected from heavy shading). — [c0de517e](https://c0de517e.com/017_vegetation_part2.htm)

### Inferences
- Applicable to a 2-layer world-projected blend plus track layers: render the blended ground (and track) into a cached albedo+normal+roughness texture around the camera (clipmap-style, bake only on change) and let the G-buffer shader do 1-2 fetches; BC3+BC3 or BC5-based packing as in RVT cuts fetches from 3 maps per layer to 2 per pixel.
- Distance drop of layers/detail normal beyond N metres follows from the "camera-dependent part stays light" guidance; no published N found.
- Cost of a cache is paid per update area, so a static or slowly moving field is cheap; wind/animated geometry should not feed the cache (Epic: keep contributors static).
- Anisotropic filtering cost: no source found.

### Gaps
- No measured triplanar/world-projection overhead, no multi-layer height-blend per-layer cost, no anisotropic-filter cost, no Frostbite/Decima layer packing details, no published "distance N metres" values.

## 5. Tying blade colour to the ground cheaply

### Takeaway
Shipped practice is to sample terrain colour (and normal) once at the blade/clump root and pass it to the blade vertices, avoiding the full terrain shader; this lets far blades vanish into a matching ground.

### Cited Findings
- BO4: grass blades receive basic terrain data, colour and geometric normals, sampled from the terrain and passed to the vertices; shading then resembles the terrain's. Author notes it avoids the cost of terrain shaders (many blended layers). — [c0de517e](https://c0de517e.com/017_vegetation_part2.htm)
- Same source (search summary) attributes the same scheme to Horizon Zero Dawn's grass; the fetched page itself covered Black Ops 4 only, so the Horizon attribution is unconfirmed. — [search summary of c0de517e](https://c0de517e.com/017_vegetation_part2.htm)
- Unreal: sampling a runtime virtual texture from a vertex shader is possible but without GPU feedback (so the needed mips may not be resident). — [Epic RVT docs](https://dev.epicgames.com/documentation/en-us/unreal-engine/runtime-virtual-texturing-in-unreal-engine)

### Inferences
- A low-res top-down ground-colour map (or the cached blended albedo from section 4) sampled once per blade/clump root gives colour continuity; keep that texture in the same colour space and mip/blur level as the ground, and apply the same tint to the far-field billboard/carpet so dropping blades causes no step.
- If the ground colour map is built from the cache, the cache and the blade root share one source of truth, so look changes to the ground propagate.

### Gaps
- No published numbers (resolution of the colour map, texels per metre) for these systems.

## 6. Published before/after numbers (collected)

- UE shadow depth with grass vs without: 29.84 vs 13.95 ms (one scene) — [UE forum](https://forums.unrealengine.com/t/grass-performance-with-dynamic-light-and-what-is-prepass-ddm/150981)
- Bend SSS port: 0.17-0.26 ms at 2560x1369 on RTX 4090 (incl. depth copy and extra casters); 0.105 ms at 1600x900; mask touched 0.4-0.8% of pixels — [Aurora-Viewer PR #524](https://github.com/Aurora-Viewer/Aurora-Viewer/pull/524)
- HDRP contact shadows: 0.5-1.3 ms on base PS4 1080p — [HDRP docs](https://docs.unity3d.com/Packages/com.unity.render-pipelines.high-definition@14.0/manual/Override-Contact-Shadows.html)
- BF3 terrain: shader splatting 10-20 ms vs baked-texture pass 2.5-3 ms on PS3 (secondary summary) — see section 4
- Unverified: a secondary article claims ~83,000 visible blades in ~2.5 ms for Ghost of Tsushima; not confirmed against the talk — [GodotGrass search summary / hashnode](https://hashnode.com/posts/what-we-can-learn-from-grass-in-ghost-of-tsushima-renders/69e130b2ffbb787634eb9ef4)

### Gaps
- No before/after for height-AO replacing SSAO on grass, none for RVT vs live layered shaders in ms on a modern GPU.

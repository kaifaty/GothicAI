# Grass shading, lighting and animation: what makes it read as a volume (state as of 2026-11-13)

Scope: shading, lighting, colour, shadows, stability, motion and LOD *look* of real-time grass. Geometry (blades vs cards, densities) belongs to another note; touched only where shading depends on it.

Source quality warning, read first. The two most-cited primary talks (Wohllaib GDC 2021 on Ghost of Tsushima, Sanders GDC 2018 on Horizon Zero Dawn) could not be read directly in this pass: the Tsushima slides are behind GDC Vault, the Horizon PDF exceeds the fetch limit (>10 MB), and the Witcher 3 landscape PDF (Gollent GDC 2014) was fetched but could not be text-extracted on this machine (no PDF tools). Their content below comes from (a) a garbled auto-transcript of the Horizon talk, (b) open-source re-implementations that state they follow the Tsushima talk, and (c) a September 2026 practitioner survey (Cyrus Sarkosh, "Grass, wind and trails in a browser meadow", updated 2026-11-13) that cites these talks by number. Everything that rests only on such second-hand reading is flagged "(second-hand)"; claims with no source at all are flagged [unverified]. Primary sources actually read: GPU Gems 3 ch. 16 (Sousa, Crysis, 2008), AMD GPUOpen procedural grass (2024), Sucker Punch's SIGGRAPH 2020 shading-course slides (Patry), Unreal Engine shading-model docs (via search summary).

## Summary and ranked list: if a team can afford only three or four ingredients

### Takeaway
A flat, pale "lawn" is what you get when every pixel of grass gets the same albedo, a normal that agrees with the terrain, no darkening at the root and no backlight. The ingredients that turn it into a volume are, in order of payoff per cost: (1) a root-to-tip darkening gradient plus per-clump colour/height variation, (2) grass that takes its base colour from the ground it grows in (and far ground that takes the grass's tip colour), (3) a softened/"rounded" normal so a clump lights as one mass instead of noisy facets, (4) cheap translucency for backlit blades at low sun. Wind as large travelling gust waves is fifth; real shadow casting from grass is consistently the first thing cut.

### Cited Findings
- A 2026 practitioner survey ranks by payoff per cost: 1) tint grass to the ground it grows in, strongest at the base and more at distance (Witcher 3 pigment map); 2) darken root to tip and vary height and colour per clump with cellular noise (Tsushima: the clump controls height, colour, facing and lean); 3) flatten normals so the field lights as one volume (Horizon forces view-space normal Z positive); 4) translucency for backlit blades at low sun; 5) one global wind field; 6) density falloff with width compensation and per-species draw distances; 7) occlusion where grass meets ground; 8) overdraw discipline; 9) interaction — [Sarkosh 2026](https://csarko.sh/research/grass-and-trail-realism)
- The Tsushima-derived "clever tricks" listed by an open implementation are exactly: outward-tilted normals, normals lerped to terrain normal at distance, side-on blade realignment, vertices concentrated toward the tip, distant terrain colour blended toward grass top colour, root AO that fades out at distance — [Unity-Grass, cainrademan](https://cainrademan.github.io/Unity-Grass/) (second-hand for Tsushima)
- Real dynamic grass shadows are offered as an optional, expensive toggle in Red Dead Redemption 2 PC; Tsushima is reported to use no real shadow pass for grass (dithered-depth impostors plus screen-space shadows) — [Sarkosh 2026](https://csarko.sh/research/grass-and-trail-realism) citing Shacknews RDR2 guide and Wohllaib 2021 (second-hand)

### Inferences
- For the specific symptom (scanned grass on ledge tops reading as a smooth pale lawn), items 1–2 are pure colour work and apply even to a flat material layer: the scanned albedo is likely too bright and too uniform, and lacks the dark "gaps between blades" that the 2002 painted texture had baked in. See "Flat grass layers" below.
- Translucency and normal tricks need geometry (cards or blades); they do nothing for a flat layer except via a sheen/fuzz lobe.

### Gaps
- No primary source gives a measured share of "perceived volume" per ingredient; the ranking is practitioner judgement.

## Normals: which normal lights a blade or card

### Takeaway
Nobody ships grass lit by its raw per-face normal. Shipping games use one of: an outward-tilted ("rounded") blade normal (Tsushima), a normal blended toward up or toward the terrain/clump normal, increasingly with distance (Tsushima, AMD), or a normal forced to face the camera (Horizon: abs of view-space Z). Raw face normals on double-sided cards make adjacent cards in a clump flip between lit and black, which reads as noise at distance and as paper cut-outs up close.

### Cited Findings
- Horizon Zero Dawn: uses vertex normals but takes the absolute value of the view-space normal (Z) so no normal faces away from the camera; applied to grass; the speaker calls it an artistic choice that "breaks lighting" but hides the flat nature of cards — [Sanders GDC 2018 transcript notes (garbled auto-caption)](https://www.cnblogs.com/cainiao001/p/10635721.html)
- Without this, double-sided cards lit by their geometric normal go from lit to black across a clump; with it the field "lights as one rounded mass" — [Sarkosh 2026](https://csarko.sh/research/grass-and-trail-realism) (paraphrasing Sanders)
- Ghost of Tsushima (GDC 2021): blade normals tilted outward across the blade width to fake curvature without extra vertices; at distance normals lerped toward the underlying terrain normal to reduce graininess, because normals then vary less in screen space — [Unity-Grass](https://cainrademan.github.io/Unity-Grass/) (second-hand)
- Another summary of the same talk: to reduce shimmer in the highlights, normals are blended toward a clump-based common normal as camera distance increases — search summary of [gist.ly talk summary](https://gist.ly/youtube-summarizer/procedural-grass-systems-in-ghost-of-tsushima-achieving-art-direction) (second-hand; conflicts in detail with "terrain normal" above — both may be true at different distances) [unverified which]
- A Godot re-implementation uses a curvature factor c_n default 0.3 (range 0–0.5), rotating the two blade-edge normals by ±c_n·π about the blade's up axis — [Jair Santana, Emulating Tsushima grass in Godot](https://jairsantana.com/posts/grass/) (author's own constant, not Tsushima's)
- AMD GPUOpen (2024-03-20): blade normal = cross(side, Bézier tangent), flipped for back faces, then `lerp(up, normal, 0.25)` — i.e. 75 % up vector, 25 % geometric — "for a softer look" — [AMD GPUOpen procedural grass](https://gpuopen.com/learn/mesh_shaders/mesh_shaders-procedural_grass_rendering/)
- Crysis (2008): leaves are double-sided; ambient is a hemisphere term that darkens downward-facing normals to about half intensity — [GPU Gems 3 ch. 16, Sousa](https://developer.nvidia.com/gpugems/gpugems3/part-iii-rendering/chapter-16-vegetation-procedural-animation-and-shading-crysis)
- Sucker Punch's own shading course: for moss (an environment material) vertex normals were too inconsistent for direct shading, so wrap lighting softens them (n=2, w=0.2) — [Patry, SIGGRAPH 2020 "Samurai Shading"](https://blog.selfshadow.com/publications/s2020-shading-course/patry/slides/index.html)
- Unreal: Two Sided Foliage shading model + "Two Sided" material flag; UE forum users report backsides very dark and dense grass self-shadowing making roots very dark under Lumen (UE 5.7) — [Epic shading models doc](https://dev.epicgames.com/documentation/en-us/unreal-engine/shading-models-in-unreal-engine); [UE 5.7 forum thread](https://forums.unrealengine.com/t/ue-5-7-foliage-grass-materials-look-extremely-dark-on-the-unlit-side-and-bad-lighting-consistency/2837185)
- Common UE practice: grass cards use the landscape/terrain normal or a world-up-biased normal (e.g. via "pivot painter"/"Use world-up normal" material functions) [unverified — not confirmed in an Epic primary doc in this pass]

### Inferences
- Recommended for a deferred GGX renderer: write a G-buffer normal = normalize(lerp(N_blade_rounded, N_up_or_terrain, k)) with k ≈ 0.5–0.75 near, rising to ~1.0 at the end of the grass range. This both removes facet noise and lets the far grass match the terrain lighting exactly (critical for an invisible LOD ring).
- Horizon's abs(view Z) trick is a one-liner for double-sided cards but prevents backlit cards from ever being darker on the back side; combine with an explicit translucency term (below).

### Gaps
- Frostbite, CryEngine (modern), Unity HDRP-specific grass-normal recommendations not found in this pass.

## Occlusion and depth cues

### Takeaway
The single strongest depth cue is a vertical darkening gradient from root to tip (AO baked along the blade), combined with a darker ground under the grass. It is essentially free. Values in shipping/demo code darken the root to roughly 0–25 % of tip brightness with a power curve (exponent ~1.5–2), and fade the effect with distance so far fields don't look dirty.

### Cited Findings
- AMD (2024): `selfshadow = clamp(pow((y - rootHeight)/height, 1.5), 0, 1)` multiplies the base colour — i.e. root fully black, half height ≈ 0.35, tip 1.0 — [AMD GPUOpen](https://gpuopen.com/learn/mesh_shaders/mesh_shaders-procedural_grass_rendering/)
- Godot Tsushima re-implementation: `o = mix(1 − 0.75·d, 1, h²)` (h = height along blade, d = density 0–1) — root darkening scales with density, up to 75 % — [Jair Santana](https://jairsantana.com/posts/grass/) (author's constant)
- Tsushima: fake AO from blade length darkening the base; base AO faded out at distance because dark spots far away looked unnatural — [Unity-Grass](https://cainrademan.github.io/Unity-Grass/) (second-hand)
- Horizon: pre-computed AO stored in a vertex colour channel; grass reportedly has no separate AO texture and uses (a derivative of) the translucency texture as pre-computed AO — [Sanders transcript notes](https://www.cnblogs.com/cainiao001/p/10635721.html) (garbled); [Sarkosh 2026](https://csarko.sh/research/grass-and-trail-realism)
- Crysis: baked AO in vertex alpha; final result multiplied by baked AO — [GPU Gems 3 ch. 16](https://developer.nvidia.com/gpugems/gpugems3/part-iii-rendering/chapter-16-vegetation-procedural-animation-and-shading-crysis)
- Survey recommends a root-to-tip gradient or cheap proxy rather than putting grass in the shadow map; "occlusion where grass meets ground" ranked 7th — [Sarkosh 2026](https://csarko.sh/research/grass-and-trail-realism)
- Tsushima: screen-space shadows plus dithered-depth impostors instead of a real shadow pass — [Sarkosh 2026](https://csarko.sh/research/grass-and-trail-realism) (second-hand)

### Inferences
- SSAO/GTAO on dense grass helps only at the base and tends to over-darken; a per-blade analytic gradient is cheaper and more stable under TAA.
- Darkening the terrain under grass (multiply terrain albedo or AO by grass density, ~0.5–0.7) is what makes gaps between blades read as depth rather than as the bright ground showing through [unverified — practitioner habit, no published value found].

### Gaps
- No measured split of how much "volume" root AO vs. ground darkening vs. SSAO provides.

## Colour variation and ground blending

### Takeaway
Three scales of variation are standard: per blade (small random hue/value jitter and an albedo texture), per clump (Voronoi/cellular clumps carrying colour, height, lean), and per field (low-frequency noise or a painted/terrain-derived colour map). The grass base is tinted by the terrain colour beneath it (Witcher 3 "pigment map"; Tsushima), and far terrain is tinted toward the grass tip colour, so neither a hard line at the root nor a hard ring at the grass end appears.

### Cited Findings
- AMD: base colour `(0.41, 0.44, 0.29)` sRGB → `pow(…, 2.2)` (≈ linear 0.14, 0.16, 0.066), multiplied by `0.75 + 0.25·Perlin(0.25·worldPos.xz)` for dark patches (±12.5 % about 0.875) — [AMD GPUOpen](https://gpuopen.com/learn/mesh_shaders/mesh_shaders-procedural_grass_rendering/)
- Tsushima: Voronoi clumping where the clump controls height, colour, facing and lean; final colour = gradient along blade × clump colour × albedo texture — [Unity-Grass](https://cainrademan.github.io/Unity-Grass/); [Sarkosh 2026](https://csarko.sh/research/grass-and-trail-realism) (second-hand); each blade type has separate vein and stem albedo, gloss and normal textures — [Jair Santana](https://jairsantana.com/posts/grass/) (second-hand)
- Godot re-implementation clump ranges: clump weight 0.8; width × mix(0.2, 0.5), height × mix(0.5, 2.0) — [Jair Santana](https://jairsantana.com/posts/grass/) (author's constants)
- Witcher 3 (Gollent, GDC 2014): terrain rendered top-down at its lowest mip into a "pigment map", sampled in the grass vertex shader with a bottom-up falloff, slightly weaker near and stronger far; per-grass-type culling grid and draw distance so species fade at staggered distances — [Sarkosh 2026](https://csarko.sh/research/grass-and-trail-realism) citing [Gollent slides](https://media.gdcvault.com/GDC2014/Presentations/Gollent_Marcin_Landscape_Creation_and.pdf) (second-hand; the PDF was fetched but not text-extractable here); an UE forum thread confirms the "Pigment Map" section exists in that talk — [UE forum](https://forums.unrealengine.com/t/getting-ground-color-for-grass/8109/4)
- Horizon: colour from a lookup texture (≈64 entries), one colour sample per object except grass which is sampled per vertex; background colour brightened and foreground darkened — [Sanders transcript notes](https://www.cnblogs.com/cainiao001/p/10635721.html) (garbled; numbers uncertain)
- Albedo ranges (broadband, chart-level, author says "ballpark"): short green grass ≈ 0.20–0.25 linear, tall wild grass ≈ 0.16–0.18, grain crops 0.10–0.25, conifer forest 0.08–0.12 — [albedo chart (CSDN mirror)](https://blog.csdn.net/cubesky/article/details/106095521)

### Inferences
- Those chart values are solar-spectrum albedo (they include the near-infrared plateau where leaves reflect 40–50 %), so they overstate visible brightness. Visible-band green grass is darker: the AMD constant gives linear ≈ (0.14, 0.16, 0.07), mean ≈ 0.12, which is a plausible upper bound for live grass albedo; dry/yellow grass should be brighter and more saturated toward yellow (linear roughly 0.25–0.35 in R/G) [unverified — no measured visible-band dataset read in this pass]. A "pale lawn" symptom is consistent with a scanned albedo that is too bright (e.g. scan not de-lit, or average > ~0.2 linear) and lacks the dark between-blade pixels.
- Dry tips: lerp the top 20–30 % of the blade toward a straw colour per clump [unverified — common practice, no primary value].

### Gaps
- No primary measured visible-band (sRGB/linear) albedo for live vs. dry grass found; recommend checking the USGS Spectral Library or DONTNOD's PBR chart in a follow-up.
- Witcher 3 pigment map resolution and falloff numbers not read.

## Light through the blade: translucency, specular, sheen

### Takeaway
Backlight is the second "life" cue after colour: at low sun, blades glow yellow-green. All cited implementations use a cheap analytic term — wrap diffuse on the back side plus a view-dependent forward-scatter lobe, scaled by a thickness/translucency texture — not true SSS. Specular is kept low and is a known source of shimmer; it is softened or blended away at distance.

### Cited Findings
- Crysis back side: transmission `saturate(dot(E, −L))^4`; wrapped diffuse `saturate(−N·L × 0.6 + 0.4)`; an artist view-dependency value blends the two; multiplied by back-diffuse colour and an artist-painted thickness map (can be 128×128); shadow accumulation applied to the back-light term. Grass is shaded fully per-vertex because of fill-rate cost; grass used alpha blending, leaves alpha test — [GPU Gems 3 ch. 16](https://developer.nvidia.com/gpugems/gpugems3/part-iii-rendering/chapter-16-vegetation-procedural-animation-and-shading-crysis)
- Horizon: translucency = light from behind × view/light angle × thickness × baked AO × artistic boost; surface thickness controls absorption; one diffusion value locked for all vegetation; specular fixed at 4 % dielectric (F0 = 0.04) — [Sanders transcript notes](https://www.cnblogs.com/cainiao001/p/10635721.html); [Sarkosh 2026](https://csarko.sh/research/grass-and-trail-realism)
- Unreal Two Sided Foliage: lets light pass through thin geometry, uses a Subsurface Color input that filters transmitted light (can encode thickness); recommended over the Subsurface model for thin surfaces — [Epic shading models doc](https://dev.epicgames.com/documentation/en-us/unreal-engine/shading-models-in-unreal-engine); [UE 4.27 lighting models](https://docs.unrealengine.com/4.27/RenderingAndGraphics/Materials/MaterialProperties/LightingModels)
- Barré-Brisebois & Bouchard (GDC 2011) local-thickness translucency approximation is the common reference for this class of term — cited by [Sarkosh 2026](https://csarko.sh/research/grass-and-trail-realism); [original post](https://colinbarrebrisebois.com/2011/03/07/gdc-2011-approximating-translucency-for-a-fast-cheap-and-convincing-subsurface-scattering-look/)
- Ghost of Tsushima base model: Lambert diffuse with optional translucency, optional simplified asperity scattering (fuzz), isotropic GGX specular; moss fuzz: density 0.5, spread 0.9, fibre albedo 5 × Lambert albedo, base-layer shadowing omitted for performance; fuzziness stored as 7 bits in the G-buffer — [Patry SIGGRAPH 2020](https://blog.selfshadow.com/publications/s2020-shading-course/patry/slides/index.html)
- Tsushima grass: Phong/gloss-mapped specular; glossy highlights contribute to aliasing at distance, mitigated by normal blending — [Unity-Grass](https://cainrademan.github.io/Unity-Grass/) (second-hand)
- Practitioner cost note: a wrap-lighting translucency is about six ALU operations per light; a full subsurface option in Babylon.js estimated at 1–3 ms — [Sarkosh 2026](https://csarko.sh/research/grass-and-trail-realism) (author's estimate)

### Inferences
- For our deferred renderer without a foliage model: add one G-buffer bit/material ID "thin foliage" plus a scalar transmission (or reuse an existing channel); in the sun-light pass add `T × saturate(dot(V, −L))^p × shadow × sunColor × albedoTint`, p ≈ 4 (Crysis), plus back-wrap (−N·L·0.6+0.4). Only the sun needs it.
- A Tsushima-style fuzz/sheen lobe is the only light-through/sheen cue usable on a flat grass layer.

### Gaps
- No published grass roughness values (other than F0 0.04 in Horizon). Typical community range roughness 0.5–0.8 [unverified].
- Unity HDRP and Frostbite foliage diffusion profiles not read.

## Shadows

### Takeaway
Grass receiving shadows from trees and terrain is essential and cheap (it just samples the existing shadow maps). Grass *casting* into the main shadow map is expensive and usually cut or limited to the nearest cascades with simplified, non-animated, non-alpha-tested caster meshes; the volume cue is instead faked with root AO, a darkened ground and screen-space/contact shadows.

### Cited Findings
- Horizon: vegetation casts only into cascades 0 and 1; objects assigned to cascades by fixed distances, same shadow LOD across cascades to avoid popping; shadow casters are separate cheaper, non-masked, non-animated meshes; distant cascade uses a heightfield-based shadow; shadows ≈ "almost nine percent" of something (garbled) — [Sanders transcript notes](https://www.cnblogs.com/cainiao001/p/10635721.html); [Sarkosh 2026](https://csarko.sh/research/grass-and-trail-realism)
- Tsushima: dithered-depth impostors plus screen-space shadows, no real grass shadow pass — [Sarkosh 2026](https://csarko.sh/research/grass-and-trail-realism) (second-hand)
- RDR2 PC: "Grass Shadows" is a separate optional setting because it is expensive — [Sarkosh 2026 citing Shacknews](https://csarko.sh/research/grass-and-trail-realism)
- Witcher 3 had a 2023 next-gen patch controversy over grass shadows, later fixed — [PC Gamer](https://pcgamer.com/cd-projekt-is-back-with-yet-another-big-fix-for-the-witcher-3s-grass)
- A browser demo's grass receives no shadows; a receiver pass for bush-scale objects cost ≈ 4 ms there — [Sarkosh 2026](https://csarko.sh/research/grass-and-trail-realism)

### Inferences
- With virtual shadow maps, casting near grass (≤10–20 m) into the VSM is feasible on an RTX 3080 but cache-invalidating when animated; using a static (non-animated) caster as Horizon did keeps VSM pages cached. On M1 Pro, prefer no casting + screen-space contact shadow.

### Gaps
- No cost figure in ms for grass shadow casting from a primary source.

## Stability of thin geometry (aliasing, TAA, alpha)

### Takeaway
Shimmer comes from three sources: alpha-test edges, sub-pixel blade width, and specular/normal variance. Fixes used: blend normals toward a common normal with distance; fade specular; keep blades ≥ ~1 pixel wide or widen them with distance (compensating with fewer blades); preserve alpha coverage in mips; prefer opaque blade geometry over alpha cards near the camera; fade distance with dither (TAA resolves it).

### Cited Findings
- Tsushima: normal lerp toward terrain/clump normal at distance specifically to cut graininess "even with TAA" — [Unity-Grass](https://cainrademan.github.io/Unity-Grass/) (second-hand)
- AMD: when blade count drops with distance (32 → 2 per patch, `lerp(32, 2, pow(d/(1.05·end), 0.75))`), width is multiplied by 32/bladeCount; the last blade's width scales by frac(bladeCountF) to avoid popping; base width 0.03 — [AMD GPUOpen](https://gpuopen.com/learn/mesh_shaders/mesh_shaders-procedural_grass_rendering/)
- Tsushima: blades perpendicular to the camera are stretched horizontally in view space / rotated to stay visible — [GodotGrass (2Retr0)](https://github.com/2Retr0/GodotGrass); [Unity-Grass](https://cainrademan.github.io/Unity-Grass/) (second-hand)
- Crysis: no alpha-to-coverage then; grass was alpha-blended and shaded per-vertex — [GPU Gems 3 ch. 16](https://developer.nvidia.com/gpugems/gpugems3/part-iii-rendering/chapter-16-vegetation-procedural-animation-and-shading-crysis)
- Alpha-to-coverage only works with MSAA and is not a drop-in fix in a TAA/post pipeline — [Sarkosh 2026](https://csarko.sh/research/grass-and-trail-realism)
- Alpha-coverage-preserving mip generation (Castaño 2010, "Computing alpha mipmaps") and Ben Golus' "Anti-aliased alpha test: the esoteric alpha to coverage" (2017) are the standard references; Golus page returned HTTP 403 here — [Golus](https://bgolus.medium.com/anti-aliased-alpha-test-the-esoteric-alpha-to-coverage-8b177335ae4f) [content unverified in this pass]
- Hashed alpha (Wyman & McGuire, I3D 2017) gives stable stochastic transparency under TAA [unverified in this pass]

### Inferences
- With TAA and no MSAA, the robust path is: opaque blade geometry near (no alpha at all), alpha cards with coverage-preserved mips + hashed alpha in the mid range, and per-pixel normal/specular damping with distance. Motion vectors must include wind displacement (previous-frame wind evaluated in the vertex shader), otherwise TAA ghosts/blurs moving grass [unverified — standard practice, no primary source read].

### Gaps
- No primary source with numeric mip bias or minimum pixel width for grass.

## Motion that sells volume

### Takeaway
Volume reads from motion when (a) large, slow, travelling gust waves sweep the field (spatially coherent, wavelengths of tens of metres, frequency well under 1 Hz), (b) a smaller per-blade flutter at 1–2 Hz with per-blade phase breaks uniformity, and (c) blades bend progressively (tip moves most, root fixed, length preserved) instead of shearing. A single global wind field shared by all vegetation is the norm.

### Cited Findings
- Horizon: global wind force field computed each frame from 3D textures at several scales, higher resolution near the player; per-asset bend, stiffness and strength; large-scale term `2·sin(centre.x + y + z + t) + 1` (biased positive so grass leans) plus flutter `0.065·sin(2.65·(p.x + y + z + t))`; large- and small-scale motion give about 80 % of total motion — [Sanders transcript notes](https://www.cnblogs.com/cainiao001/p/10635721.html); formulas from [Sarkosh 2026](https://csarko.sh/research/grass-and-trail-realism) (second-hand)
- AMD (following Guerrilla): phase = position projected on wind direction + time + `4·Perlin(0.1·pos)`; x: `2·sin(0.5t)`, y: `1·sin(1.0t)` applied to the Bézier mid control point — [AMD GPUOpen](https://gpuopen.com/learn/mesh_shaders/mesh_shaders-procedural_grass_rendering/)
- Crysis: main bending scaled by normalised vertex height with a bend-scale factor; vertex renormalised to its original length (spherical limit, preserves length); detail bending by four smoothed triangle waves at 1.975, 0.793, 0.375, 0.193; per-instance stiffness; vertex colours carry edge stiffness, per-leaf phase, overall stiffness, AO — [GPU Gems 3 ch. 16](https://developer.nvidia.com/gpugems/gpugems3/part-iii-rendering/chapter-16-vegetation-procedural-animation-and-shading-crysis)
- Tsushima: unified wind from scrolling 2D Perlin noise sampled by CPU and GPU; wind moves Bézier control points and facing; vertices concentrated toward the tip where bend occurs — [Unity-Grass](https://cainrademan.github.io/Unity-Grass/); Rockenbeck, "Blowing from the West: Simulating Wind in Ghost of Tsushima", GDC 2021 — [GDC Vault](https://gdcvault.com/play/1027350/Blowing-from-the-West-Simulating) (not read)
- Browser example: 0.06 Hz gust sine + second harmonic + 2 Hz flutter, tip displacement ≤ 6 cm; implied gust wavelengths ≈ 26 m and 79 m along two axes — [Sarkosh 2026](https://csarko.sh/research/grass-and-trail-realism) (author's constants)
- Godot re-implementation: wind sample scale 0.005 (≈ 200 m noise period), height offset 0.25h², bend multiplier π — [Jair Santana](https://jairsantana.com/posts/grass/) (author's constants)

### Inferences
- Starting values: gust noise period 20–80 m, scroll speed ≈ wind speed (3–6 m/s), tip amplitude 10–30 % of blade height in gusts, flutter 1–2 Hz at 2–5 % of height, per-clump phase from position hash.

### Gaps
- No published gust wave speed in m/s from a shipped game.

## Level of detail for the look

### Takeaway
What must be preserved as density drops: average colour and brightness and apparent coverage. Methods: widen remaining blades (AMD), blend distant terrain toward grass tip colour (Tsushima), blend grass normals to terrain normals (Tsushima), sink or shrink far grass into the ground rather than pop it (Horizon), stagger species draw distances (Witcher 3), and fade root AO and specular with distance.

### Cited Findings
- Tsushima: distant terrain blended toward grass top colour to mask density lost to culling; 15 vertices near, 7 far with positions blended at the switch; distant tiles double in size and keep blade count, so 3 of 4 blades drop per step — [Unity-Grass](https://cainrademan.github.io/Unity-Grass/); [Sarkosh 2026](https://csarko.sh/research/grass-and-trail-realism) (second-hand)
- Horizon: LOD by ring; shader features locked per LOD; animation scales to zero and vertices sink with distance; 20–36 triangles nearest, 10–18 further — [Sarkosh 2026](https://csarko.sh/research/grass-and-trail-realism); [Sanders transcript notes](https://www.cnblogs.com/cainiao001/p/10635721.html)
- AMD width compensation `width *= maxBladeCount / bladeCountF`, "barely noticeable in a dense meadow" — [AMD GPUOpen](https://gpuopen.com/learn/mesh_shaders/mesh_shaders-procedural_grass_rendering/)
- Witcher 3: per-type culling grids, staggered fade; grass lookup matches terrain shader at vertex precision — [Sarkosh 2026](https://csarko.sh/research/grass-and-trail-realism) (second-hand)
- Unreal: Landscape Grass Type plus Runtime Virtual Texture lets grass sample the landscape's colour [unverified — Epic doc not read in this pass]

### Inferences
- The far terrain grass material must use the same albedo mean and the same (terrain/up) normal as the far 3D grass; then the ring is invisible regardless of density.

### Gaps
- No primary numbers on Witcher 3 / UE RVT ring distances.

## Flat grass layers on a material (no geometry)

### Takeaway
A material-only grass layer can look deep at steep viewing angles and from a distance if it carries: a dark, high-contrast albedo (dark between-blade gaps), a strong AO/cavity term, a detail normal, a height map used for height-blend transitions with rock and soil, optionally parallax occlusion, and a sheen/fuzz lobe. It fails at grazing angles and at silhouettes (ledge edges), where geometry (cards, fins or shells) is needed. Shell texturing (many offset layers of the surface with an alpha/height cut-off) is the classic step between a flat layer and blades, good for short grass, costs overdraw per layer.

### Cited Findings
- Shell texturing ("fur" shells) is the classic trick for short grass/fur in games — [80.lv: "Here's how games render grass and fur" (2023)](https://80.lv/articles/classic-video-games-trick-for-rendering-grass-fur) (not read in full)
- Height-map blending for terrain layer transitions — [Shaderic heightmap blending tutorial](https://www.shaderic.com/tutorials/HeightmapBlending.html) (cited by Sarkosh, not read)
- Anti-tiling: histogram-preserving blending (Heitz & Neyret 2018) and hex tiling (Mikkelsen 2022) — [Heitz & Neyret](https://eheitzresearch.wordpress.com/722-2/); [Mikkelsen JCGT 2022](https://jcgt.org/published/0011/03/05/paper-lowres.pdf)
- Tsushima's fuzz lobe (asperity scattering) was deployed on moss as an environment material — fibre albedo 5× Lambert, density 0.5, spread 0.9 — the closest primary recipe for a "soft, velvety" ground layer without geometry — [Patry SIGGRAPH 2020](https://blog.selfshadow.com/publications/s2020-shading-course/patry/slides/index.html)

### Inferences
- For the ledge-top problem: first fix colour (darken and de-lit the scan, add darker cavity/AO, add per-area hue noise and dry patches, tint the layer's lower "height" toward soil colour), then add a fuzz/sheen lobe, then height-blend edges with rock. The silhouette at the ledge edge needs a fringe of cards/fins (consistent with the existing "grass fringe layer" task).
- Expect the 2002 painted texture to look "deeper" because it bakes high-contrast shadow between blades into albedo; a de-lit PBR scan intentionally removes that and needs it back via AO/cavity.

### Gaps
- No primary data on shell count vs. quality for grass; no shipped AAA example of POM-only grass found.

## Gaps (overall)
- Primary slides not read: Wohllaib GDC 2021 (Vault paywall), Sanders GDC 2018 (PDF > 10 MB fetch limit), Gollent GDC 2014 (PDF fetched but not text-extractable here — no poppler/pymupdf), Rockenbeck GDC 2021 wind talk, Renard "Wind Simulation in God of War" GDC 2019, Boulanger grass thesis, Pelzer GPU Gems 1, Jahrmann & Wimmer I3D 2017, Frostbite/FIFA grass, Unity HDRP foliage docs, CryEngine vegetation docs, Golus alpha-to-coverage (HTTP 403).
- All Tsushima and Horizon details here are second-hand (re-implementations, a garbled transcript, a 2026 survey). Treat exact formulas as "reported".
- No measured visible-band albedo for live vs. dry grass located; the chart cited gives broadband values.
- No ms costs for grass translucency, grass shadow casting or shell layers on RTX 3080 / M1 Pro class GPUs.

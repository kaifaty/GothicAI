# GPU pipeline cost of dense blade grass in a deferred G-buffer pass

Method note: ~12 searches and fetches. The primary PDFs (Karis, Nanite SIGGRAPH 2021; Jahrmann and Wimmer 2017) exceeded the fetch size limit and were not read directly. Their claims below come from secondary sources and are marked. Vendor guidance on sliver and sub-pixel triangle cost with numbers was largely not found. Most "measured" grass numbers come from hobby or student projects.

## 1. Thin and sub-pixel triangles: where the cost sits and how engines avoid it

### Takeaway
Hardware rasterisers shade pixels in 2x2 quads, so small and thin triangles waste pixel-shader lanes on helper pixels and are shaded once per triangle per quad touched. The only widely cited fix at the extreme end is Nanite's software rasteriser. For grass, the practical levers are fewer and larger blades with distance, plus widening. No vendor-published numbers for sliver cost were found.

### Cited Findings
- Pixel shading is always done on 2x2 quads on current architectures. A quad is shaded even if one pixel is covered. Helper pixels exist so derivatives (mip selection) work. — [Graphics programming discussion via search](https://redlib.hbubli.cc/r/GraphicsProgramming/comments/1hhcmhx/does_triangle_surface_area_matter_for_rasterized) (forum, secondary)
- Overdraw is counted per quad: each 2x2 quad is shaded once for every triangle that covers at least one sample in it. — same forum thread (secondary)
- A patent filing states that helper pixels can be 40-50% or more of shaded pixels and that the problem grows for smaller triangles. — [USPTO 9779542](https://image-ppubs.uspto.gov/dirsearch-public/print/downloadPdf/9779542) (patent text, figure is the filer's)
- Nanite article: the pixel shader "always computes a 2x2 quad of pixels, even if only 1 pixel is used", with efficiency possibly as low as 25% on small triangles. Hardware is poor at merging small triangles into full quads. — [Tricky Bits, Nanite Deep Dive](https://trickybitsblog.github.io/2024/04/20/nanite.html) (blog, secondary)
- Nanite's software (compute) rasteriser is estimated "maybe 3x or 4x faster for small triangles"; the blog says this is an estimate. One thread of compute runs one triangle, 128-thread groups per 128-triangle cluster. Hardware vs software is chosen per cluster by projected area, not per triangle. — same Tricky Bits article
- Other secondary sources: a CuRast paper reports Nanite's software path at about 3x hardware throughput, with larger gains on pure micropolygons. Gain mechanism: atomic min/max writes for pixel-sized triangles. A ~32 px edge threshold appears in secondary sources only. — [CuRast, arXiv 2604.21749](https://arxiv.org/pdf/2604.21749); [search summary of Karis 2021](https://advances.realtimerendering.com/s2021/Karis_Nanite_SIGGRAPH_Advances_2021_final.pdf) (primary slides listed but not read; the "~32 px" figure is unverified)
- Nanite's visibility buffer is 64 bit (32 depth, 32 triangle id), uses custom atomic depth tests, and "offers memory bandwidth benefits compared to a G-Buffer". Materials are shaded later, only for visible pixels. — [Tricky Bits](https://trickybitsblog.github.io/2024/04/20/nanite.html)
- Unreal optimisation guide: triangle count alone does not matter; avoid small, thin, long triangles, control with LOD. — [Unreal Art Optimization, Pixel Costs](https://unrealartoptimization.github.io/book/pipelines/pixel)
- Arm: small triangles weaken hidden-surface removal; "HSR has been getting less effective over time because smaller triangles = fewer quads occluded"; vertex cost is typically orders of magnitude above fragment cost on mobile (2018 deck). Immortalis G920+ defers vertex shading and runs only the position shader for small triangles when the fragment shader does not affect depth/coverage. — [Arm Vulkanised 2025 talk](https://www.vulkan.org/user/pages/09.events/vulkanised-2025/T18-Pete-Harris-Arm-MobileBestPractices.pdf); [Arm fragment prepass blog](https://developer.arm.com/community/arm-community-blogs/b/mobile-graphics-and-gaming-blog/posts/immortalis-g925-the-fragment-prepass) (mobile GPUs, not Apple)
- Ghost of Tsushima (Sucker Punch, GDC Advanced Graphics Summit, Wohllaib): per secondary summaries, blades seen edge-on are stretched horizontally in view space so they do not vanish, blades are bent by height, and blades get wider with distance while fewer are drawn. Reported ~83,000 blades on screen at ~2.5 ms is unverified. — [Godot reimplementation](https://github.com/2Retr0/GodotGrass); [Hashnode summary](https://hashnode.com/posts/what-we-can-learn-from-grass-in-ghost-of-tsushima-renders/69e130b2ffbb787634eb9ef4); [GDC Vault page](https://gdcvault.com/play/1027214/contactUs) (overview only)
- Density LOD (Boulanger, SIGGRAPH 2006, via archive): near field uses instanced meshes, mid/far grass is culled per blade by comparing a density map to thresholds. — [Porcino archive](https://nickporcino.com/meshula-net-archive/posts/post119) (secondary)
- Hobby sample uses LOD blade meshes of 9, 5 and 1 triangles with three indirect instanced draws. — [Bliss sample](https://github.com/IwakuraRein/Bliss) (demo, not shipped)

### Inferences
- With about 2 M triangles at 2560x1440 (3.7 M pixels), each blade triangle is on average under 2 pixels of area if the screen is fully covered once. This means quad waste is probably a main cost, close to the 25% efficiency case. (Inference from arithmetic and the cited quad behaviour, not a measurement.)
- A mid-distance LOD that merges blades (fewer, wider, same area) lowers quad count without changing the grass silhouette much. This is the same idea as Ghost of Tsushima's widening, applied per distance band.
- A Nanite-style software rasteriser is the only documented route to beat quad waste on desktop, but it is a large engineering step, and Metal has no 64-bit image atomics path equal to it (inference, not verified).

### Gaps
- No NVIDIA, AMD or Apple document with numeric sliver/sub-pixel triangle cost or setup rate (triangles per clock) was found. The "40-80x slowdown" figure on a blog was not corroborated and is excluded.
- The primary Nanite slides and the Ghost of Tsushima slides were not read; the speed-up and threshold are secondary.
- No source gives a minimum pixel width clamp value for blades.

## 2. Culling before rasterisation

### Takeaway
Frustum, distance and orientation culling on the GPU are the standard measured wins (about 1.8x to 3x in student projects, up to about 75% of blades culled). Terrain-aware (horizon / max-height pyramid) culling helps in valleys and gives little on hilltops. Mesh shaders for grass have no published ms numbers.

### Cited Findings
- Jahrmann and Wimmer (2017) do orientation, view-frustum, distance and Hi-Z occlusion culling on the GPU via indirect rendering. A student reimplementation reports culls up to 75% of blades without noticeable density change. — [Jahrmann & Wimmer 2017 draft](https://www.cg.tuwien.ac.at/research/publications/2017/JAHRMANN-2017-RRTG/JAHRMANN-2017-RRTG-draft.pdf) (not read in full; per search summary); [Vulkan grass project](https://github.com/DanielZhong/Vulkan-Grass-Rendering)
- A UPenn project: culling gave 2-3x; each method alone helps less than all three; orientation culling was the most effective there. Another report: about 134 ms to about 75 ms with ~4 M blades (1.8x), and distance culling mattered most. Results depend on the camera. — [Vulkan Grass Rendering, UPenn](https://github.com/Aman-Sachan-asach/Vulkan-Grass-Rendering/blob/master/README.md); [Vulkan grass project](https://github.com/DanielZhong/Vulkan-Grass-Rendering) (student work, tessellation-based)
- GrassFlow (Hi-Z stage, M2 GPU): 1 M blades 6.2 ms with 3-5 draws; 5 M blades 28 ms. No per-stage breakdown. — [Project GrassFlow](https://gh.hhj520.top/Mithzzx/Project-GrassFlow) (self-reported)
- Terrain-aware culling (heightmap horizon ray plus max-height mip pyramid): about 12,000 blades visible in a valley view; frustum culling cut it about half; horizon ray cut another 60-80%; pyramid kept the gain at near-frustum cost. On a hilltop camera the extra cut was only 5-10%. CPU paths handled 12,000 blades in under 1 ms. No hardware named. — [Cinevva, terrain occlusion culling](https://app.cinevva.com/blog/2026-05-19-terrain-occlusion-culling) (single-author blog)
- Cluster culling: 64 or 128 triangle clusters, frustum and Hi-Z tested in compute, one indirect draw. Assassin's Creed Unity numbers quoted: 20-40% of triangles culled, 30-80% of shadow triangles. — [Cinevva, foliage overdraw](https://app.cinevva.com/blog/2026-05-11-foliage-overdraw) (secondary, attributes the numbers to Ubisoft SIGGRAPH 2015)
- Mesh shaders: NVIDIA describes task shader to cull/choose LOD, then meshlet culling. Kapoulkine's test on RTX 2080: 100 Buddha models 17.2 ms traditional vs 6.3 ms in his mesh path (about 3x), on a heavy-geometry scene, not grass. A forum voice says per-primitive culling is better left to fixed-function hardware. — [NVIDIA Turing mesh shaders](https://developer.nvidia.com/blog/introduction-turing-mesh-shaders); [DSOGaming summary](https://www.dsogaming.com/news/nvidia-rtx-series-3x-faster-in-mesh-shading-in-vulkan-rtx2080-handles-20-billion-triangles-second)
- Reading culling results back to the CPU stalls; keep data on GPU and use indirect draws. — [Unity forum thread](https://discussions.unity.com/t/frustum-culling-with-compute-shader-how/941164)

### Inferences
- At ~150 draws, per-tile (about 150 tiles) or per-cluster culling already exists in some form. Gains now come from finer culling inside tiles (clusters of 64-128 blade triangles) and from orientation/horizon tests, not from adding draws.
- Horizon culling pays most when the camera is in valleys or low terrain and little on open ground; gain is view-dependent. Expect to measure on the worst-case and typical camera, not one.
- Orientation culling pays more for blades than for general meshes, because a flat blade seen edge-on is a sliver. This also supports an edge-on reduction rather than just dropping.

### Gaps
- No measured mesh/task-shader grass pipeline; no measured Hi-Z isolated gain on grass.
- Student numbers use tessellation-era pipelines and unspecified GPUs; do not extrapolate to an RTX 3080 at 2 M triangles.

## 3. Depth: pre-pass, front-to-back, early-Z

### Takeaway
A depth pre-pass pays off when the pixel shader is costly and alpha-tested, and the overdraw is high. Opaque blades with a cheap pass gain less. On Apple GPUs (TBDR) hidden-surface removal already removes overdraw for opaque geometry, and discard weakens it.

### Cited Findings
- A masked-foliage depth pre-pass does the alpha test, then the full material shades with depth-equal. Sorting front to back lets Hi-Z reject more fragments. Author's claims: naive masked foliage 8-15x overdraw in dense forest; full stack 1-2x and foliage cost 4-6x lower. No per-talk figures. — [Cinevva, foliage overdraw](https://app.cinevva.com/blog/2026-05-11-foliage-overdraw) (author's figures, no cited measurement)
- Arm: draw-call, vertex and bandwidth overhead usually outweighs the benefit of a Z-pre-pass on mobile; their fragment pre-pass hardware exists to address this. — [Arm Vulkanised 2025](https://www.vulkan.org/user/pages/09.events/vulkanised-2025/T18-Pete-Harris-Arm-MobileBestPractices.pdf)
- Apple TBDR: the GPU processes all geometry of a pass per tile and shades only visible primitives into tile memory; A11 and later improve fragment discard performance. — [Apple, Tailor your apps for Apple GPUs and TBDR](https://developer.apple.com/tutorials/data/documentation/metal/tailor-your-apps-for-apple-gpus-and-tile-based-deferred-rendering.md)
- Discard in the fragment shader blocks early depth test on many GPUs; draw opaque, then alpha-tested, then blended. — [Imagination: Do not use discard](https://docs.imgtec.com/starter-guides/powervr-architecture/html/topics/rules/do-not-use-discard.html); [Apple forum on alpha test](https://developer.apple.com/forums/thread/85318)

### Inferences
- If the blades are real geometry and opaque (no alpha test), early-Z already works; a pre-pass would double vertex and triangle-setup cost for 2 M triangles, which per the quad analysis above is likely the dominant cost. A pre-pass is probably a loss unless pixel work is heavy. Verify by a timing test.
- On Metal, drawing grass near the end of the opaque pass is free of ordering effort; HSR handles it.

### Gaps
- No measured pre-pass vs no pre-pass numbers for opaque blade grass.
- No desktop vendor document with numbers on early-Z with discard.

## 4. Vertex cost: instance data, vertex pulling, wind

### Takeaway
Vertex work grows with triangle count and is paid per vertex before culling, so culling and LOD matter more than how data is packed. No measured comparison of vertex-generated vs prebuilt blades was found.

### Cited Findings
- Sample using compute generation then indirect instanced draws by LOD (9, 5, 1 triangles). — [Bliss](https://github.com/IwakuraRein/Bliss) (demo)
- Wind handling difference quoted as 3 ms vs 9 ms for a forest, author's statement without source. — [Cinevva, foliage overdraw](https://app.cinevva.com/blog/2026-05-11-foliage-overdraw)
- Arm: vertex cost per vertex far exceeds fragment cost on mobile; avoid small triangles. — [Arm Vulkanised 2025](https://www.vulkan.org/user/pages/09.events/vulkanised-2025/T18-Pete-Harris-Arm-MobileBestPractices.pdf)

### Inferences
- Compact per-blade data (position, packed angle/height/width) with the vertex shader expanding it via vertex pulling avoids vertex fetch bandwidth, but unmeasured here.
- Normal reconstruction cost in the vertex stage is small next to per-pixel work (inference; no source).

### Gaps
- No published numbers on vertex pulling vs vertex buffers, compact blade data, or normal reconstruction cost.

## 5. Pixel cost, G-buffer width, lower-rate shading

### Takeaway
Variable rate shading has measured gains only on whole frames (about 10-19%, up to 14% tier 2) and has documented problems on masked/foliage content. Dropping G-buffer channels and per-vertex shading is advised by reasoning, not measured here.

### Cited Findings
- Gears Tactics, tier 1, 4K Ultra, RTX 2080 SUPER: 23.3 ms off, 20.9 ms "on" (-2.4 ms, 10.3%), 18.9 ms "performance" (-4.4 ms, 18.9%). Base pass saved 0.6 ms at the performance setting. — [DirectX blog, Gears Tactics VRS](https://devblogs.microsoft.com/directx/gears-tactics-vrs/)
- The Coalition reports tier 2 gains of up to 14% GPU. VRS on masked objects "would often result in a great loss of detail"; artifacts appeared in a sand environment with foliage. — [DirectX blog, Gears tier 2](https://devblogs.microsoft.com/directx/gears-vrs-tier2/); [Gears Tactics VRS](https://devblogs.microsoft.com/directx/gears-tactics-vrs/)
- Intel / WoW: manual alpha test with discard gave black pixels on foliage edges under VRS (mip differences under coarse shading). Fix: disable VRS (Passthrough combiner, 1x1) for those draws. Overall speedup of 1.09-1.20x on an integrated GPU. — [Intel, Velocity & Luminance Adaptive Rasterization](https://intel.com/content/www/us/en/developer/articles/technical/velocity-luminance-adaptive-rasterization.html)
- Cheaper masked shading (simpler lobes, lower-res normal maps, half-rate), claimed 2-3x faster masked shaders, author's figure, no source. — [Cinevva, foliage overdraw](https://app.cinevva.com/blog/2026-05-11-foliage-overdraw)
- Apple: on TBDR, a G-buffer can stay in tile memory (imageblocks), avoiding device memory writes and reads. — [Apple, TBDR](https://developer.apple.com/tutorials/data/documentation/metal/tailor-your-apps-for-apple-gpus-and-tile-based-deferred-rendering.md)
- Nanite visibility buffer: 64 bit per pixel, less bandwidth than a G-buffer; materials resolved after visibility. — [Tricky Bits](https://trickybitsblog.github.io/2024/04/20/nanite.html)

### Inferences
- For grass, VRS tier 2 at 2x2 for blade coverage would hit the same quad-structure: blades are already one or two pixels wide, so coarse shading changes the look at edges. "No change of look" argues against VRS here, unless applied only to flat-lit interior ground layers.
- Constant-per-blade or per-vertex colour and normals shrink the pixel shader to a few instructions with few texture fetches. If the blade pixel shader is already tiny, its cost is quad count not instruction count.
- A visibility buffer for grass alone inside a deferred frame is a big change; no report of someone doing exactly that was found.

### Gaps
- No foliage-only VRS ms numbers; no Metal VRS-equivalent (rasterization rate maps) document read; no reports of reduced-resolution grass compositing with numbers.

## 6. Visibility buffer or forward grass inside a deferred frame

### Takeaway
Only Nanite's design was found as a documented visibility-buffer route; no one reports forward-rendering grass inside a deferred frame with numbers.

### Cited Findings
- Visibility buffer / deferred material pass lists: Burns & Hunt (JCGT 2013), Karis 2021 are named as references for shading once per pixel. — [Cinevva, foliage overdraw](https://app.cinevva.com/blog/2026-05-11-foliage-overdraw) (reference list only)
- Nanite: 64-bit visibility buffer, hardware and software raster paths, material shading afterwards. — [Tricky Bits](https://trickybitsblog.github.io/2024/04/20/nanite.html)

### Inferences
- A cheap variant is a thin visibility ID plus a blade-parameters lookup in the lighting pass: reduces G-buffer writes at the cost of a resolve and a pipeline change. Not documented for grass.

### Gaps
- No engine-specific statement found on grass in a visibility buffer or forward pass.

## 7. Instancing and draw submission at about 150 draws

### Takeaway
At about 150 draws, draw submission is unlikely to be the cost; triangles and quads are. Indirect draws with GPU culling keep CPU work flat.

### Cited Findings
- GPU cull with indirect draw avoids CPU readback stalls. — [Unity forum](https://discussions.unity.com/t/frustum-culling-with-compute-shader-how/941164)
- GrassFlow: 3-5 draw calls for 1-5 M blades on an M2. — [Project GrassFlow](https://gh.hhj520.top/Mithzzx/Project-GrassFlow)
- Arm: draw-call overhead can outweigh pre-pass benefit on mobile. — [Arm Vulkanised 2025](https://www.vulkan.org/user/pages/09.events/vulkanised-2025/T18-Pete-Harris-Arm-MobileBestPractices.pdf)

### Inferences
- Merging the 150 draws into a few multi-draw indirect calls saves CPU and driver time, not GPU rasterisation time; it cannot reduce the 1.2 ms unless the GPU is idle between small draws (e.g. barriers or pipeline switches), which is worth checking in a capture.

### Gaps
- No numbers found for the point at which draw count matters on an RTX-class GPU.

## 8. Published before/after numbers (summary, with trust level)
- Culling on student Vulkan grass: 1.8x-3x, camera dependent. Low trust (student, unspecified hardware).
- Terrain horizon culling: -60-80% in valley, -5-10% on hilltop. Medium-low (single blog, no ms).
- Nanite software raster: about 3x for small triangles. Medium (consistent in secondary sources; primary not read).
- VRS in a shipped game: 10-19% of frame, not foliage-specific; foliage and masked content show artifacts. Medium-high (Microsoft/Intel posts).
- Foliage "full stack" 4-6x: low trust (author's synthesis, no per-talk figures).

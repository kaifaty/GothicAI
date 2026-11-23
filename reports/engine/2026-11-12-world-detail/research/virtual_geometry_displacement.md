# Dense world geometry without LOD popping: virtual geometry, displacement and cheaper alternatives (state as of November 2026)

Research notes, 2026-11-12. About 20 tool calls. Primary sources were preferred. Numbers without a source are marked as unverified or moved to Gaps. The Karis SIGGRAPH 2021 PDF could not be fetched (over 10 MB), so Nanite internals rest on Epic's documentation, two independent re-implementations and from-memory claims that are marked as such.

## 1. Nanite internals, cost and limits (UE5 through 5.7, with 5.8/5.9 docs live)

### Takeaway
Nanite builds a DAG of clusters of up to 128 triangles, grouped and simplified level by level. At runtime the GPU picks a cut by projected error, culls it in a persistent compute pass and rasterises it into a visibility buffer. Small triangles go to a compute (software) rasteriser and large ones to the hardware rasteriser. Geometry pages are streamed. On disk it costs about 14.4 bytes per input triangle, which makes a 1.5 M-triangle mesh 7.6x smaller than the same mesh with four classic LODs. Through UE 5.7 Epic has kept extending it: foliage (Assemblies, Skinning, Voxels), tessellation and displacement, landscape, skinned meshes, and native ray tracing through cluster acceleration structures (CLAS). Most of these features are still experimental.

### Cited findings
- **Cluster DAG.** Clusters are capped at 128 triangles, and at each LOD level they are grouped into groups of 8 to 32 clusters for simplification. Secondary source — [Medium technical breakdown, 2023-24](https://medium.com/@GroundZer0/nanite-epics-practical-implementation-of-virtualized-geometry-e6a9281e7f52)
- **Runtime rule.** A cluster is swapped for a coarser one only when the resulting visual error stays under a threshold. Simplification error accumulates across levels, so each level's error must be carried on top of the previous one — [jglrxavpok, Recreating Nanite: runtime LOD selection, 2024](https://jglrxavpok.github.io/2024/04/02/recreating-nanite-runtime-lod-selection.html); [LOD generation, 2024](https://jglrxavpok.github.io/2024/01/19/recreating-nanite-lod-generation.html)
- **Software rasteriser.** It is used for clusters whose triangles are under about 32 pixels long. After culling, each cluster is sent to a software or hardware rasteriser by its screen-space size. Secondary sources — [Medium](https://medium.com/@GroundZer0/nanite-epics-practical-implementation-of-virtualized-geometry-e6a9281e7f52); [LMU lecture slides on Nanite, 2021-22](https://www.medien.ifi.lmu.de/lehre//ws2122/gp/slides/gp-ws2122-extra-nanite.pdf)
- **Index encoding.** Two formats exist: a directly readable in-memory format and a denser on-disk format. Triangle indices are stored as the smallest index plus two offsets, about 17 bits per triangle with 128-triangle clusters — [UIUC CS418 course text on Nanite](https://cs418.cs.illinois.edu/website/text/nanite.html)
- **Disk size.** Epic's figure is 14.4 bytes per input triangle on average in Valley of the Ancients, which is about 13.8 MB per million triangles.
  - A 1,545,338-triangle mesh takes 19.64 MB as Nanite and 148.95 MB as a non-Nanite mesh with 4 LODs (7.6x).
  - A 19,066-triangle low-poly mesh with 4 LODs takes 1.34 MB.
  - With textures, the Nanite package plus a shared 4K detail normal map comes to 27.83 MB, against 31.04 MB for low-poly plus its unique normal map.
  - Explicit tangents add about 10%.
  - Source — [Epic, Nanite Technical Details (UE 5.x docs, current 2026)](https://dev.epicgames.com/documentation/unreal-engine/nanite-technical-details?lang=en-US)
  - A 2026 SIGGRAPH Advances deck reportedly cites 5.6 bytes per triangle attributed to Karis 2021. I could not read the deck (image-only PDF) and this is unverified — [s2026 Adaptive Tessellation and Subdivision](https://advances.realtimerendering.com/s2026/content/Adaptive%20Tessellation%20and%20Subdivision_7.pdf)
  - The two figures probably count different things (per input triangle including the LOD hierarchy, against the base level only). This is an inference.
- **Runtime memory accounting.** A candidate cluster costs 12 bytes and a visible cluster 16 bytes. The Nanite CLAS buffer for ray tracing defaults to 512 MB, with a maximum of 2048 MB. An undersized streaming pool thrashes, and undersized cluster buffers cause missing or blinking geometry — [Epic, Nanite Technical Details](https://dev.epicgames.com/documentation/unreal-engine/nanite-technical-details?lang=en-US)
- **Root clusters stay resident.** The root LOD clusters of every instance must stay in memory, even for distant instances — [Medium, Nanite streaming and memory budgets](https://medium.com/@GroundZer0/nanite-streaming-and-memory-budgets-managing-geometry-at-scale-4c54bfa5d5b1)
- **Lumen in the Land of Nanite (PS5, 2020).** Community discussion of Epic's stream reports about 25 M triangles drawn per frame, fairly constant, from roughly 20 billion source triangles, with 6.14 GB of geometry. Forum-sourced, not an official write-up — [NeoGAF thread on the Inside Unreal stream](https://www.neogaf.com/threads/inside-unreal-in-depth-look-at-ps5s-lumen-in-the-land-of-nanite-demo-only-6-14gb-of-geometry-and-deep-dive-into-nanite.1608305/page-23)
- **Supported features (current docs page, titled UE 5.9).**
  - Materials: opaque and masked only.
  - WPO is supported but splits clusters and costs culling, so displacement should be clamped.
  - Skinned meshes: one draw per mesh, Virtual Shadow Map shadowing, no geometry LODs (animation LODs are used instead).
  - Morph targets: not supported.
  - Foliage: through Nanite Foliage.
  - Not supported: forward rendering, VR stereo, MSAA, lighting channels, per-view distance culling.
  - Maximum of 16 M streamed instances.
  - Source — [Epic, Nanite Virtualized Geometry](https://dev.epicgames.com/documentation/en-us/unreal-engine/nanite-virtualized-geometry-in-unreal-engine)
- **Slow paths.** Skeletal, spline and WPO/displacement meshes are culled in the slower "complex" class. Closely stacked small objects create overdraw. Modular pieces with shared boundaries can crack from lossy vertex quantisation unless their precision is matched — [Epic, Nanite Technical Details](https://dev.epicgames.com/documentation/unreal-engine/nanite-technical-details?lang=en-US)
- **Ray tracing.**
  - By default Nanite meshes are ray-traced through a fallback mesh, controlled by Fallback Relative Error and Fallback Triangle Percent (100% / 0 error means no decimation).
  - Native Nanite ray tracing (`r.RayTracing.Nanite.Mode 1`) is experimental. Epic says it keeps detail and uses significantly less memory than zero-error fallback meshes.
  - GPUs without cluster RT support use the fallback.
  - Sources — [Epic docs](https://dev.epicgames.com/documentation/en-us/unreal-engine/nanite-virtualized-geometry-in-unreal-engine); [Technical Details](https://dev.epicgames.com/documentation/unreal-engine/nanite-technical-details?lang=en-US)
- **UE 5.7 (November 2025): Nanite Foliage (experimental).** It is made of three systems:
  - Assemblies: instanced parts that keep asset size small.
  - Skinning: wind through bones instead of WPO.
  - Voxels: distant foliage, which Epic says avoids cross-fades, pops and hand-made LODs.
  - Also in 5.7: TSR thin-geometry detection.
  - Sources — [UE 5.7 release notes](https://dev.epicgames.com/documentation/unreal-engine/unreal-engine-5-7-release-notes); [Guru3D](https://www.guru3d.com/story/unreal-engine-57-released-with-new-procedural-content-generation-and-more-features/)
- **UE 5.7 Nanite performance items.**
  - `r.Nanite.Culling.MinLOD`, on by default, skips child clusters for faster culling and less candidate memory.
  - Experimental HZB priming after camera cuts.
  - Pixel-programmable distance cut-off for skinned Nanite meshes.
  - No timings were published — [Tom Looman, UE 5.7 performance highlights, 2025](https://tomlooman.com/unreal-engine-5-7-performance-highlights/)
- **Tessellation and displacement.**
  - Nanite Tessellation (runtime, material-driven, also for landscape) arrived experimental in UE 5.4 (2024), with early crash and performance reports. A forum user says the experimental label was dropped in 5.5, which is unconfirmed by Epic. A 5.8 regression in landscape displacement has been reported.
  - The docs also describe Nanite Static Displacement Mapping, an offline adaptive tessellator that bakes displacement into the Nanite mesh.
  - Sources — [UE 5.4 preview](https://forums.unrealengine.com/t/unreal-engine-5-4-preview/1763516?lang=ko); [crash thread](https://forums.unrealengine.com/t/frequent-crashes-with-nanite-tessellation-in-ue-5-4-2-assertion-failed-rasterizerpass-clustercomputeshader-isnull/1919478/13); [5.8 bug report](https://forums.unrealengine.com/t/nanite-displacement-bugged-in-exact-same-landscape-in-ue-5-8-but-not-in-ue-5-7/2739815); [Epic docs](https://dev.epicgames.com/documentation/en-us/unreal-engine/nanite-virtualized-geometry-in-unreal-engine)

### Inferences
- Nanite's quality advantage comes from four things working together: the DAG, the software rasteriser, the visibility buffer and streaming. The software rasteriser is what keeps micro-triangles cheap. Without it, hardware rasterisation loses efficiency below a few pixels per triangle (quad overshading), so a renderer that uses hardware only should set its error threshold so triangles stay at about 2–4 px or more, not 1 px.
- Even Epic needed five years and several releases (foliage in 5.7) to handle aggregate and thin geometry. Foliage remains the weak spot for any cluster-LOD system a small team builds.

### Gaps
- Primary Karis 2021 figures (exact page size, root page policy, software-raster speed-up over hardware, PS5 timings) were not retrieved: the PDF is too large for the fetch tool.
- From memory and unverified: pages are 128 KB, the software rasteriser is about 3x faster than hardware for tiny triangles, and the visibility buffer is a 64-bit depth + cluster + triangle ID written with atomics. Bevy's description of its own visibility buffer matches that scheme (see section 2).
- No published per-frame ms breakdowns of Nanite on an RTX 3080 at 1440p were found.

## 2. Open implementations and reusable building blocks

### Takeaway
The most useful permissive building block is meshoptimizer v1.0 (MIT, December 2025). It provides meshlet building, cluster partitioning, simplification with error output, and a single-header cluster-LOD DAG builder, `clusterlod.h`. NVIDIA's `vk_lod_clusters` (Apache-2.0) is the most complete open reference renderer: mesh-shader rasterisation, an optional software rasteriser, streaming and RTX Mega Geometry ray tracing. It uses a lightly modified `clusterlod.h`. Bevy's virtual geometry shows the effort and pitfalls of a one-person implementation. It is Rust/wgpu, so its code is not reusable in C++.

### Cited findings
- **meshoptimizer v1.0.**
  - It ships `clusterlod.h`, a single-header micro-library that builds a Nanite-like DAG of clusters grouped and simplified level by level, maintained alongside the main library.
  - v1.0 improved `meshopt_partitionClusters` for disconnected clusters, giving better DAG quality.
  - Clustering maps to `meshopt_buildMeshletsSpatial` / `meshopt_buildMeshletsFlex`.
  - Sources — [meshoptimizer v1.0 summary, December 2025](https://www.x-cmd.com/blog/251217); [clusterlod.h source](https://raw.githubusercontent.com/zeux/meshoptimizer/master/demo/clusterlod.h)
  - The library's licence is MIT. This is general knowledge; the zeux post fetched does not state it.
- **zeux, "Billions of triangles in minutes" (September 2025).**
  - Build: the 1.64 B-triangle Zorah scene (18.9 B with instancing, 36.1 GB glTF) goes through `clusterlod` in about 2 min 35 s on a Ryzen 7950X (16 threads), with peak RAM around 45 GB. The baseline was about 9 min 20 s and the earlier sample about 30 min.
  - Render: through `vk_lod_clusters` on a GeForce RTX 3050 8 GB, with about a 2 GB geometry pool, a frame takes 16 ms rasterised and 26 ms ray-traced.
  - Two clusterizers exist: one tuned for mesh-shader rasterisation, one for RT (CLAS micro-BVHs).
  - Limits: the streaming BVH for cluster picking is not in `clusterlod.h` yet (`vk_lod_clusters` builds it with meshoptimizer functions), and memory-limit logic is not implemented.
  - Source — [zeux.io, 2025](https://zeux.io/2025/09/30/billions-of-triangles-in-minutes/)
- **NVIDIA `vk_lod_clusters`** (Apache-2.0, maintained; © 2026 footer).
  - Rasterisation: `VK_EXT_mesh_shader` or `VK_NV_mesh_shader`, an optional compute software rasteriser, frustum and two-pass HiZ occlusion, a discrete-LOD comparison mode, DLSS.
  - Ray tracing: CLAS → BLAS → TLAS, with optional partitioned TLAS, BLAS sharing and caching, and hit-driven streaming.
  - Streaming from RAM to VRAM on demand. Meshlet index compression is about 3x.
  - Requirements: Vulkan SDK 1.4.341 or newer. `VK_NV_cluster_acceleration_structure` needs driver 572.16+ (January 2025); older drivers get rasterisation only. The persistent culling kernel is disabled on non-NVIDIA GPUs.
  - Source — [GitHub nvpro-samples/vk_lod_clusters](https://github.com/nvpro-samples/vk_lod_clusters)
- **Other libraries.** NVIDIA `nv_cluster_lod_builder` (Apache-2.0) and `trichi` (a triangle cluster hierarchy library) are alternative DAG builders. Simplygon has a clustered meshlet optimiser (commercial) — [awesome.ecosyste.ms listing](https://awesome.ecosyste.ms/projects/github.com%2Fjolifantobambla%2Ftrichi); [Simplygon docs](https://documentation.simplygon.com/SimplygonSDK_10.4.272.0/api/tools/clusteredmeshletoptimizer.html)
- **Bevy virtual geometry (experimental `meshlet` feature, by jms55).**
  - Pipeline: preprocessing into `MeshletMesh`, a meshlet DAG, a visibility buffer (packed depth + cluster ID + triangle ID in R64Uint/R32Uint with texture atomics), a material-depth resolve, and a rewritten small-cluster rasteriser in 0.16.
  - Not implemented: streaming and skinning (as of 0.16). MSAA is disabled, WASM/WebGPU is unsupported (needs 64-bit atomics), and the main pass was not bindless.
  - Costs: Bevy's own docs say it has much higher fixed cost and memory than the standard renderer and wins only with many dense meshes and heavy overdraw.
  - Culling was "the biggest bottleneck" in 0.16. A later BVH-culling release note reports about 3.5 ms total on an RTX 4070 for a demo scene, about 4.5 ms with over 1 M instances (about 3.1 ms geometry and 0.4 ms materials). These are developer-reported, single scene.
  - Sources — [jms55 0.16 post, March 2025](https://jms55.github.io/posts/2025-03-27-virtual-geometry-bevy-0-16); [Bevy meshlet BVH culling release note](https://cocalc.com/github/bevyengine/bevy/blob/main/release-content/release-notes/meshlet-bvh-culling.md); [MeshletPlugin docs](https://dev-docs.bevyengine.org/bevy/pbr/experimental/meshlet/struct.MeshletPlugin.html); [0.14 post](https://jms55.github.io/posts/2024-06-09-virtual-geometry-bevy-0-14)
- **Bevy lessons.**
  - `meshopt_buildMeshlets` optimises for culling and vertex reuse, not for minimal shared boundary vertices, so jms55 switched to METIS partitioning (UFactor = 1, undershoot the partition count with `div_ceil(126)` for 128-triangle clusters).
  - Clusters under 10 triangles used to get stuck and fail to simplify.
  - He calls DAG quality "the most important part of virtual geometry (and the hardest to get right)".
  - The work spanned several Bevy releases (0.14 → 0.16, about a year), done by one person part-time, with burnout noted.
  - Source — [jms55 0.16 post](https://jms55.github.io/posts/2025-03-27-virtual-geometry-bevy-0-16)
- **Other open projects.**
  - jglrxavpok's Carrot-engine Nanite series (2024): LOD generation and runtime selection — [blog](https://jglrxavpok.github.io/2024/01/19/recreating-nanite-lod-generation.html)
  - "Nanite-style hierarchical meshlet LODs 101" — [guilblack.github.io](https://guilblack.github.io/)
  - An interactive Moana renderer (2025) that measured about 50% memory saved per unique mesh with CLAS — [mavenkim2 blog, November 2025](https://mavenkim2.github.io/2025/11/09/interactive-moana-part2.html)

### Inferences
- A minimal C++ cluster-LOD renderer that fits this engine needs:
  1. An offline build with `clusterlod.h`: clusters of 64–128 triangles, a per-group bounding sphere, and a monotonic error (parent error ≥ child error, propagated) in the asset cooker.
  2. A GPU cut selection in the existing task/mesh-shader or compute culling pass. Each cluster is drawn if its own projected error is at or below the threshold and its parent's projected error is above it. This independent per-cluster test is what makes the DAG crack-free without a traversal.
  3. A BVH or hierarchy over groups for culling large instances. Without it, every cluster of every instance is tested, which is fine for a moderate world.
  4. Feeding the surviving clusters into the existing meshlet + HiZ two-pass path.
- No software rasteriser, no visibility buffer and no streaming are needed if all geometry fits in VRAM. At 4–16x of about 2 M triangles, with roughly 2x for the DAG and roughly 14–20 B per triangle, the static world is a few hundred MB to about 1 GB, which fits 10 GB. Costs follow from that: no subpixel triangles, a threshold of about 1–2 px error giving triangles of a few pixels, and full residency.
- `vk_lod_clusters` is the best code to read for the GPU traversal, the two-pass occlusion and the RT path. It is Apache-2.0, so code can be borrowed with attribution.

### Gaps
- No public measured effort (person-weeks) for a minimal non-streaming cluster-LOD renderer was found. Bevy (multi-release, about a year part-time, with a visibility buffer and software rasteriser) is the only data point.
- `nanite-webgpu` and `lighthugger` were not checked in this pass.

## 3. Platform support: Apple M1 Metal mesh shaders, MoltenVK, fallback paths

### Takeaway
M1, M1 Pro and M1 Max are Apple7, and Metal 3 mesh shading is available on Apple7 and later. Developer reports, though, show the M1 mesh-shader path slower than plain draws, possibly emulated. I found no evidence that MoltenVK (latest v1.4.2, July 2026) exposes `VK_EXT_mesh_shader`. A Metal-native path or a compute-culling plus indirect-draw path is therefore needed on macOS.

### Cited findings
- The M1 family is listed as Apple7, and mesh shading needs Metal 3 on Apple7 or Mac2 or later — [Apple, Metal feature set tables / capabilities](https://developer.apple.com/metal/capabilities/); [feature sets](https://developer.apple.com/metal/feature-sets)
- One developer measured a Metal mesh-shader path on M1 at about 2x slower than their draw-call version, while the same change was 1.5x faster on an RTX 3070. A reply suggested some devices may emulate mesh shading. Another developer saw little gain from HiZ culling in the mesh shader and noted there is no mesh-shader profiler. Both are 2022–23 anecdotes — [Apple Developer Forums: Bad mesh shader performance](https://developer.apple.com/forums/thread/722047); [Mesh-shader culling is broken?](https://developer.apple.com/forums/thread/724525)
- Tellusim's 2021 tests across vendors found the best mesh-shader and MDI paths slower than compute-based rasterisation for dense meshes — [Tellusim compute raster, 2021 (proxied)](https://dr.molodetz.nl/_proxy/https/tellusim.com/compute-raster/)
- Hardware-accelerated mesh shading as a headline GPU feature is reported for M3/A17 (2023), which suggests M1's support may be implemented differently. Secondary and unverified — [Medium, Apple GPU features](https://medium.com/@genelab_999/what-are-the-new-features-of-apple-gpu-ray-tracing-mesh-shading-dynamic-caching-38729caa5f7c)
- MoltenVK v1.4.2 (24 July 2026) added `VK_EXT_sampler_filter_minmax` and `gl_DrawID`, with no mesh-shader mention. A 2022 MoltenVK discussion only lists `VK_EXT_mesh_shader` as possible with Metal 3 — [warp2search on v1.4.2](https://www.warp2search.net/story/moltenvk-v142-gldrawid-support-amd-subgroup-fixes-and-legacy-gpu-patches/); [MoltenVK discussion #1616](https://github.imc.re/KhronosGroup/MoltenVK/discussions/1616); [MoltenVK releases](https://github.com/KhronosGroup/MoltenVK/releases)

### Inferences
- On the M1 Pro preset, a robust path is compute culling (cluster cut + frustum + HiZ) that writes a compacted index or cluster list, then one indirect draw. This works on any Vulkan or Metal backend and is also the fallback for PC GPUs without mesh shaders. The `gl_DrawID` support in MoltenVK 1.4.2 helps multi-draw-indirect.
- The M1 preset should use a coarser error threshold (bigger triangles), since M1 rasterisation and memory bandwidth are far below an RTX 3080.

### Gaps
- MoltenVK mesh-shader status was not confirmed from the MoltenVK repository itself. The fetched issue #1588 is about transform feedback.
- There are no controlled 2025–26 measurements of M1 mesh shaders against compute + indirect.

## 4. Ray tracing and shadows with cluster-LOD geometry

### Takeaway
There are three ways to give dense LOD'd geometry to RT:
- Fallback or proxy BLAS: Nanite's default; it works on any RT GPU and is the cheapest to build.
- NVIDIA cluster acceleration structures (RTX Mega Geometry, `VK_NV_cluster_acceleration_structure`, driver 572.16+, all RTX GPUs per NVIDIA's RTX Kit): per-cluster micro-BVHs that follow the LOD cut.
- AMD's DGF compression path: `VK_AMDX_dense_geometry_format`, provisional.

Mega Geometry 2.0 (September 2026) adds streaming. Gains in games so far are modest (about 0.3–1 GB VRAM, 5–20% speed), and Remedy keeps classic BLAS for static geometry because CLAS traced about 0.5 ms slower.

### Cited findings
- `vk_lod_clusters` builds CLAS → BLAS → TLAS from the same LOD clusters, with partitioned TLAS for many instances and hit-driven streaming. It needs driver 572.16+ (January 2025) and is NVIDIA-only — [GitHub](https://github.com/nvpro-samples/vk_lod_clusters)
- On a GeForce RTX 3050 8 GB the Zorah scene takes 26 ms ray-traced against 16 ms rasterised in `vk_lod_clusters`. The NVIDIA Zorah demo avoided Nanite-style proxy meshes for RT — [zeux, 2025](https://zeux.io/2025/09/30/billions-of-triangles-in-minutes/)
- **Alan Wake 2 (update 1.2.8, early 2025).**
  - Tom's Hardware measured about 1 GB VRAM saved and +13% on an RTX 4090 at 4K.
  - NVIDIA's GDC 2026 figures, as cited by a secondary source, are about 300 MB and up to +20%.
  - Remedy reported CLAS tracing about 0.5 ms slower, so static geometry stays on classic triangle BLAS.
  - Sources — [Tom's Hardware](https://tomshardware.com/pc-components/gpus/testing-nvidias-rtx-mega-geometry-tech-vram-reducing-tech-a-leap-forward-for-path-traced-rendering); [gaming-st.com, May 2026](https://gaming-st.com/news/rtx-mega-geometry-vram-test-may2026/); [mavenkim2 blog (cites Remedy)](https://mavenkim2.github.io/2025/11/09/interactive-moana-part2.html)
- **RTX Mega Geometry SDK 2.0.0 (RTX Kit 2026.3, September 2026).**
  - Adds streaming of continuous-LOD clusters, so source geometry no longer has to fit in VRAM. Default budgets are 2 GB geometry, 2 GB acceleration structures and 4 GB textures.
  - Zorah: 31 GB of mesh data, of which 1.5 GB was in VRAM in one view on an RTX 5090 at 4K. The demo needs an RTX GPU with at least 10 GB VRAM and driver 570+.
  - Gears of War: E-Day (October 2026) uses Mega Geometry, version not stated.
  - Source — [Shazoo, 28 September 2026, citing the NVIDIA developer blog](https://shazoo.ru/2026/09/28/191361/nvidia-vypustila-rtx-mega-geometry-20-scena-na-31-gb-geometrii-zanimaet-v-videopamiati-vsego-15-gb)
- In UE, native Nanite RT through CLAS is experimental with a 512 MB default CLAS buffer. Otherwise the fallback mesh is used — [Epic, Nanite Technical Details](https://dev.epicgames.com/documentation/unreal-engine/nanite-technical-details?lang=en-US)
- **AMD DGF.** 128-byte blocks hold meshlets of up to 64 vertices and 64 triangles. `VK_AMDX_dense_geometry_format` is provisional, a multivendor extension is planned with Samsung, and hardware decode is expected on future RDNA. Reports say late 2026; this is unverified — [GPUOpen DGF Vulkan](https://gpuopen.com/learn/dense-geometry-format-amd-vulkan-extension/); [GPUOpen DGF standard](https://gpuopen.com/learn/amd-dgf-an-open-geometry-compression-standard/); [Wccftech](https://wccftech.com/amd-dgf-dense-geometry-format-leap-in-ray-traced-games-30-percent-compression-future-rdna-gpus/amp/); [PCGH](https://www.pcgameshardware.de/AMD-Radeon-Grafikkarte-255597/News/RDNA-5-Hardware-Beschleunigung-fuer-DGF-mit-mehr-Details-1483300/)
- **Virtual shadow maps.** UE's Nanite skinned meshes and Nanite geometry render into virtual shadow maps; skinned meshes use VSM shadowing by default — [Epic docs](https://dev.epicgames.com/documentation/en-us/unreal-engine/nanite-virtualized-geometry-in-unreal-engine)

### Inferences
- The engine traces GI probes, not primary visibility. So BLAS can safely be built from a fixed coarse LOD level of the DAG per mesh: one level at about 1–5 cm error, or the original 2002 mesh. That means one static BLAS per unique mesh, built once, with the same instancing as today. Matching it to the raster cut via CLAS is only worth it for sharp RT shadows or reflections and would tie the feature to NVIDIA.
- Self-intersection between a coarse BLAS and a fine raster surface needs a ray-origin bias of about the fallback's error.
- Virtual shadow maps can reuse the same cluster cut selection with the light's projection. Dense geometry multiplies VSM page-render cost, so the shadow pass should use a coarser error threshold than the main view. This mirrors UE practice but is not sourced here.

### Gaps
- No RTX 3080-specific CLAS numbers. Whether `VK_NV_cluster_acceleration_structure` runs on Ampere (RTX 30) was not confirmed in fetched sources, though NVIDIA's RTX Kit is believed to support all RTX generations (unverified).
- No KHR or EXT cross-vendor cluster-AS extension was found as of November 2026.

## 5. Displacement, tessellation, micro-meshes, parallax

### Takeaway
For a 2002 world being densified, the cheapest robust path is offline displacement baked into real geometry (like Nanite Static Displacement), fed through cluster LOD. Runtime Nanite-style tessellation exists but is complex and was unstable in UE for several releases. Hardware tessellation does not mix with mesh shaders. NVIDIA Micro-Mesh did not become a cross-vendor standard. POM remains useful for small relief only, since it has no true silhouettes or shadows.

### Cited findings
- **Epic's two approaches.** Nanite Tessellation, runtime and programmable (displacement maps or procedural, animated, used for landscape), and Nanite Static Displacement Mapping, which bakes displacement into an optimised Nanite mesh with an offline adaptive tessellator — [Epic docs](https://dev.epicgames.com/documentation/en-us/unreal-engine/nanite-virtualized-geometry-in-unreal-engine)
- **Runtime tessellation's history in UE.**
  - Experimental in 5.4 (2024), with a user reporting crashes in 4 of 5 tests and poor landscape performance.
  - Said by a user to have left experimental status in 5.5.
  - Landscape displacement reported broken in 5.8 while working in 5.7.4.
  - A forum thread on "performance considerations" exists, but no numbers were found.
  - Sources — [crash thread](https://forums.unrealengine.com/t/frequent-crashes-with-nanite-tessellation-in-ue-5-4-2-assertion-failed-rasterizerpass-clustercomputeshader-isnull/1919478/13); [landscape perf thread](https://forums.unrealengine.com/t/ue5-4-did-someone-test-performance-of-nanite-landscapes-vs-standard-landscapes-yet/1932648/1); [5.8 bug](https://forums.unrealengine.com/t/nanite-displacement-bugged-in-exact-same-landscape-in-ue-5-8-but-not-in-ue-5-7/2739815); [perf considerations thread](https://forums.unrealengine.com/t/nanite-tessellation-performance-considerations/2688900)
- WPO and displacement push Nanite meshes into the slower "complex" culling class, and displacement should be clamped to keep culling bounds tight — [Epic Technical Details](https://dev.epicgames.com/documentation/unreal-engine/nanite-technical-details?lang=en-US); [Epic docs](https://dev.epicgames.com/documentation/en-us/unreal-engine/nanite-virtualized-geometry-in-unreal-engine)
- Mega Geometry 2.0 keeps its older tessellation path (runtime subdivision with cluster AS) alongside streaming — [Shazoo, September 2026](https://shazoo.ru/2026/09/28/191361/nvidia-vypustila-rtx-mega-geometry-20-scena-na-31-gb-geometrii-zanimaet-v-videopamiati-vsego-15-gb)
- SIGGRAPH 2026 Advances included an "Adaptive Tessellation and Subdivision" talk. Its content could not be extracted (image-only PDF) — [deck](https://advances.realtimerendering.com/s2026/content/Adaptive%20Tessellation%20and%20Subdivision_7.pdf)

### Inferences
- Unsourced general knowledge, to be verified before relying on it:
  - Vulkan hardware tessellation (tessellation shaders) is core but cannot be combined with mesh shaders in one pipeline, and is generally slow on Apple GPUs.
  - Metal tessellation is a compute-fed fixed-function path (post-tessellation vertex functions).
  - Cracks appear where per-edge factors disagree between neighbours.
  - NVIDIA Displacement Micro-Map (Ada, VK_NV_displacement_micromap) stayed NVIDIA-only and saw little game adoption.
- **When to use what (recommendation).**
  - Cobblestones, masonry, rock faces, bark on big trunks: bake displacement offline into the mesh (Blender or our own tessellator), then cluster-LOD it. The cost is disk and VRAM, not runtime complexity. A hero wall of 5 × 3 m at 1 cm detail is about 150 k triangles.
  - Terrain: per-chunk mesh LOD (or cluster LOD over terrain chunks) with offline displacement near paths. Runtime tessellation pays off only if terrain is edited or deformed at runtime, which is relevant to the destructible-world ambition, but that is a later step.
  - POM or relief mapping: for small relief under about 1–2 cm (mortar gaps, wood grain, ground clutter), with self-shadowing from the height map. It fails at silhouettes and in VSM and RT shadows.

### Gaps
- No measured cost of Nanite runtime tessellation (ms per million displaced triangles) was found.
- Micro-Mesh status in 2026 was not checked against a 2026 source.
- No Metal tessellation performance numbers on M1 were found.

## 6. The classic alternative: discrete LOD, HLOD, impostors, and how popping is hidden

### Takeaway
I found little sourced, quantitative comparison of discrete LOD against cluster LOD from Unity 6, id Tech, Decima, Frostbite or RE Engine in this pass. What is sourced:
- Epic's 7.6x disk saving for one high-poly mesh (Nanite against 4 LODs).
- NVIDIA's sample, which ships a "Discrete LoD" mode for direct comparison in the same renderer.
- Remedy's Alan Wake 2: GPU-driven meshlet culling with mesh shaders and single-pixel-precise occlusion, but no Nanite-style continuous LOD.

### Cited findings
- **Disk.** A 1.55 M-triangle mesh takes 148.95 MB with 4 classic LODs against 19.64 MB as Nanite — [Epic Technical Details](https://dev.epicgames.com/documentation/unreal-engine/nanite-technical-details?lang=en-US)
- **Same-renderer comparison.** `vk_lod_clusters` has a "Discrete LoD" option, so the two approaches can be compared in one renderer — [GitHub](https://github.com/nvpro-samples/vk_lod_clusters)
- **Alan Wake 2 (Northlight, 2023).**
  - A rewritten GPU-driven pipeline with mesh shaders that "allows us to push more geometry", with occlusion culling to single-pixel precision where everything is an occluder.
  - Meshlets are culled, not just meshes.
  - Mesh-shader GPUs are required.
  - Sources — [Remedy, How Northlight makes Alan Wake 2 shine](https://remedygames.com/article/how-northlight-makes-alan-wake-2-shine); [Wccftech interview](https://wccftech.com/alan-wake-2-dev-talks-integration-of-mesh-shaders-dlss-3-5-explains-work-graphs-importance-in-future-games/)
- **Foliage pops in UE5.** Epic itself presents Nanite Voxels (5.7) as a way to avoid cross-fades, pops and hand-made LODs for distant foliage. This implies that before 5.7, even Nanite projects used classic LOD with crossfades for foliage — [Guru3D on 5.7](https://www.guru3d.com/story/unreal-engine-57-released-with-new-procedural-content-generation-and-more-features/)
- **Bevy's own trade-off.** The cluster renderer has a higher base cost and memory than the classic one and wins only with many dense meshes — [Bevy MeshletPlugin docs](https://dev-docs.bevyengine.org/bevy/pbr/experimental/meshlet/struct.MeshletPlugin.html)

### Inferences
- Discrete LOD per mesh is a valid cheaper path for props: 3–4 levels from `meshopt_simplify`, chosen per instance by projected error, with a dithered crossfade over 0.3–0.5 s resolved by TAA. The engine already has TAA.
- It does not solve large continuous meshes such as the 2002 world mesh. A single 1 km mesh needs either chunking (HLOD-style: cut the world into cells of about 32–64 m, simplify each cell with locked borders, and merge distant cells) or a cluster DAG. With locked borders, discrete chunk LOD has seams or skirts. Cluster LOD solves exactly this, which is the strongest argument for a DAG in our case: the world is one big static mesh.
- Crossfading doubles the draw cost during transitions. Cluster LOD transitions are at sub-pixel error, so they need no crossfade.

### Gaps
- No sourced numbers from Unity 6 (LOD Group, GPU Resident Drawer), id Tech, Decima, Frostbite or RE Engine on LOD practice or popping were retrieved. That would need GDC or SIGGRAPH talks: Decima's (Horizon Forbidden West) and Frostbite's GPU-driven pipelines are known to exist but were not fetched.

## 7. Realistic budgets and implementation effort

### Takeaway
Hard, sourced triangle budgets for an RTX 3080 at 1440p are scarce. The sourced data points:
- A 2020 PS5 Nanite demo drew about 25 M triangles per frame (forum-sourced).
- An RTX 3050 renders the 18.9 B-instanced-triangle Zorah scene through cluster LOD in 16 ms rasterised.
- Bevy reports about 3.1 ms for geometry in a demo scene on an RTX 4070.
- 1440p is 3.7 M pixels. At about 1 triangle per pixel the visible-triangle ceiling of continuous LOD is a few million after culling, independent of source density.

### Cited findings
- Lumen in the Land of Nanite: about 25 M triangles per frame fairly constant, from about 20 B source triangles, community-reported — [NeoGAF](https://www.neogaf.com/threads/inside-unreal-in-depth-look-at-ps5s-lumen-in-the-land-of-nanite-demo-only-6-14gb-of-geometry-and-deep-dive-into-nanite.1608305/page-23)
- Zorah through `vk_lod_clusters` on an RTX 3050 8 GB: 16 ms rasterised, 26 ms ray-traced, about a 2 GB geometry pool — [zeux, 2025](https://zeux.io/2025/09/30/billions-of-triangles-in-minutes/)
- Bevy, RTX 4070: about 3.5 ms total, with about 3.1 ms geometry and 0.4 ms materials, rising to about 4.5 ms with over 1 M instances. Developer-reported — [Bevy release note](https://cocalc.com/github/bevyengine/bevy/blob/main/release-content/release-notes/meshlet-bvh-culling.md)
- A pcper index lists a talk, "Erik Jansson – GPU driven Rendering with Mesh Shaders in Alan Wake 2", alongside a demo blurb of "200M triangles in 1ms" with 128-triangle clusters. The figure is unverified and not tied to Alan Wake 2 — [pcper index](https://irc.pcper.com/building-a-nanite-like-renderer-with-meshlets-gpu-culling.html)
- **Effort.**
  - Bevy: about a year of part-time solo work across three releases, and the author still called DAG quality the hardest part.
  - zeux's build-time optimisation alone went from about 30 min to 2.5 min for 1.6 B triangles.
  - Sources — [jms55](https://jms55.github.io/posts/2025-03-27-virtual-geometry-bevy-0-16); [zeux](https://zeux.io/2025/09/30/billions-of-triangles-in-minutes/)

### Inferences
- **Our target.** 4–16x of about 1–2 M triangles per frame gives 4–32 M triangles drawn without LOD. Drawing up to 1 km without LOD at that density is the real risk: most of those triangles are sub-pixel at distance.
- **With cluster LOD at about 1 px error**, visible triangles stay around 2–6 M at 1440p whatever the source density. That is comfortably inside an RTX 3080's hardware-raster budget with mesh shaders, plus VSM passes, roughly 2–4 ms. This is an estimate, not measured.
- **Source density guide (unsourced rules of thumb).**
  - Props: 2–20 k triangles at the base level.
  - Hero props and rocks: 50–500 k when scanned or displaced.
  - Buildings: 100 k to 2 M with baked masonry displacement.
  - Ground near the camera: about 1–4 cm vertex spacing, which is 600–10,000 triangles per m² at full detail. Cluster LOD keeps that cost only within a few metres.
- **Memory.** A source of about 100–300 M unique triangles × about 2 (DAG) × 14–20 B is 3–12 GB on disk. Without streaming, roughly 50–100 M unique triangles fit in a VRAM budget of about 2 GB. Instancing of props is essential.
- **Effort estimate for a minimal system** (offline `clusterlod` build, GPU cut selection in the existing meshlet path, a fixed coarse BLAS for RT, a coarser cut for VSM, compute + indirect fallback for Metal): about 4–8 focused weeks for one developer with agents, given the existing meshlet + HiZ infrastructure. This is an inference from Bevy's experience minus the software rasteriser, visibility buffer and streaming.
- **Next steps.** A software rasteriser and streaming are follow-ups, needed only if source density exceeds the VRAM budget or triangles must go below about 2 px.

### Gaps
- No sourced triangles-per-frame figures for shipped 2023–26 PC games at 1440p on an RTX 3080, with or without Nanite.
- No sourced per-square-metre terrain densities from shipped games.
- No sourced cost of sub-pixel triangles on the RTX 3080 hardware rasteriser (quad overshading factor).

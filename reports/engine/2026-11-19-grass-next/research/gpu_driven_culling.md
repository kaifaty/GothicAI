# GPU-driven submission of grass: compute culling + indirect draws, mesh/task shaders, work graphs

Method note: about 25 searches and fetches. This note adds to `2026-11-17-grass-cost/research/gpu_pipeline_cost.md` (section 2) and `geometry_lod_density.md`, and does not repeat them. Several primary PDFs were too large for WebFetch (over 10 MB) and were **not read**: the AMD/Coburg work-graphs preprint, Remedy's REAC 2024 deck, and Guerrilla's GDC 2017 placement deck. No slides or transcript of the Ghost of Tsushima talk (GDC 2021, Wohllaib) were reachable either. Where a number comes from a search snippet of such a document, it is marked.

Trust levels: **P** = primary (talk, paper, vendor or API doc, release notes, the PR itself). **V** = vendor blog or sample. **E** = expert engineer's own blog with measurements. **S** = secondary write-up or press. **F** = forum or hobby project.

## 1. What shipped games and tech demos do before the vertex stage

### Takeaway
Every shipped title with public detail culls grass on the GPU in compute, per tile or per instance, and then draws from GPU-written buffers. None of them publishes a grass-only before/after number. The only public numbers are whole-system figures: HZD placement at about 0.25 ms of GPU, Ghost of Yōtei culling over 1 M items to about 60 k, and AMD's work-graph demo at 1.64x faster than ExecuteIndirect. Occlusion culling of grass against a HiZ is documented only in Yōtei (as a compute stage, granularity not public), Alan Wake 2 (meshlets) and hobby projects.

### Cited Findings
- **Ghost of Tsushima (GDC 2021, "Procedural Grass in Ghost of Tsushima").** A secondary write-up says compute shaders lay out blade positions per tile and do frustum and distance culling. It does not mention occlusion culling. No public primary text was found. — [Stylized Station thread](https://threadreaderapp.com/thread/1688939411575119872) (S); [GDC Vault listing](https://gdcvault.com/play/1027214/contactUs) (P, listing only)
- **Ghost of Yōtei (PS5, Sony tech deep dive).** Instances are processed by "a sequence of compute jobs" covering occlusion culling, memory allocation and filling draw records, with no CPU involvement in that stage. The results are then "stitched" by the CPU into the frame's command lists. The same pipeline serves runtime-generated content such as flower fields. The GPU renderer's limit on grass and renderable items was doubled versus Tsushima. In one distant-mountain shot, more than 1 M trees, rocks and bushes are culled to about 60,000 rendered items. No ms figures, no HiZ details, no granularity. — [PlayStation Blog, Ghost of Yōtei tech deep dive](https://blog.playstation.com/?p=411201) (P, developer blog)
- **Horizon Zero Dawn (GDC 2017, van Muijden).** All vegetation is placed procedurally by compute shaders around the player. The slides list "500+ asset types, 100.000+ objects in scene, ~250µs avg busy load on GPU", with the pipeline instantiated 64 times (one per layer). This is placement on movement, not a per-frame visibility cull. — [GDC 2017 slides](https://www.guerrilla-games.com/media/News/Files/GDC2017_VanMuijden_GPUBasedProceduralPlacementInHorizonZeroDawn.pdf) (P, but the figures come via a search snippet: the PDF exceeded the fetch limit); [Guerrilla article page](https://www.guerrilla-games.com/read/gpu-based-procedural-placement-in-horizon-zero-dawn)
- **Horizon Forbidden West.** The public talk on foliage cost is about a "loosely tiled deferred texturing system" for foliage and alpha-tested geometry, not about culling. — [GDC Vault, Adventures with Deferred Texturing in HFW](https://gdcvault.com/play/1028035/Adventures-with-Deferred-Texturing-in) (P, abstract only)
- **Alan Wake 2 (Northlight).** The whole geometry pipeline was rewritten around mesh shaders. The press quote is occlusion culling "down to a single-pixel precision, using everything in a scene as an occluder", with meshlets culled and not just meshes. Mesh shaders are a hard requirement (minimum RTX 2060 / RX 6600). A Remedy GDC 2024 deck lists "Move to GPU dispatch for grass and foliage" as a goal, which implies grass and foliage were not yet fully GPU-dispatched in the shipped game (search snippet). — [Wccftech interview](https://wccftech.com/alan-wake-2-dev-talks-integration-of-mesh-shaders-dlss-3-5-explains-work-graphs-importance-in-future-games/) (S); [Tom's Hardware](https://www.tomshardware.com/news/alan-wake-2-requires-mesh-shaders-excluding-most-older-gpus) (S); [REAC 2024 Remedy deck](https://enginearchitecture.org/downloads/REAC_2024_Remedy.pdf) (P, not read, too large); [Remedy GDC 2024 talks](https://remedygames.com/article/explore-remedys-gdc2024-talks-on-creating-alan-wake-2) (P, index)
- **AMD procedural grass with mesh shaders (GPUOpen sample).** One mesh-shader group (128 threads) generates one patch: at most 32 blades, each 8 vertices and 6 triangles, so 256 vertices and 192 primitives per group. Each thread writes up to two vertices and two primitives with a stride of 128. The only reduction is distance LOD: blade count goes from 32 down to 2. The last blade's width is scaled by `frac(bladeCount)`, and the survivors are widened by `max/current`. The page describes **no frustum or occlusion culling and gives no timings**. — [GPUOpen, Procedural grass rendering](https://gpuopen.com/learn/mesh_shaders/mesh_shaders-procedural_grass_rendering/) (V)
- **AMD work graphs with mesh nodes (GDC 2024 demo, built with Coburg University).**
  - Grass structure: World → Chunk → Tile → DetailedTile. A DetailedTile holds 16x16 dense patches with up to 2 mesh groups per patch. Far away it is replaced by one sparse quad per patch, and an 8x4 grid of quads goes to one group.
  - Dense versus sparse tile: 512 groups, 65,536 vertices, 49,152 triangles versus 8 groups, 1,024 vertices, 512 triangles.
  - Culling is hierarchical by grid cell. The frustum sizes the chunk grid (capped at 32x32 chunks). No per-blade occlusion test is described. — [GPUOpen, mesh nodes procedural generation](https://gpuopen.com/learn/work_graphs_mesh_nodes/work_graphs_mesh_nodes-procedural_generation/) (V)
- **The same demo, whole frame.** ExecuteIndirect is 1.64x slower than work graphs with mesh nodes, averaged over 5 views, on an RX 7900 XTX with a pre-release driver. AMD calls these "super early numbers". Demo stats: 6,600 draws per frame after coalescing, 13 M triangles per frame, 200,000 work items, 37 nodes, under 200 MiB of backing store. AMD names "empty draw compaction" as an optimisation burden that work graphs remove. — [GPUOpen, GDC 2024 work graphs and draw calls](https://gpuopen.com/learn/gdc-2024-workgraphs-drawcalls/) (V)
- **The work-graphs preprint (search snippet only).** It reportedly generates frustum-culled ground clutter, and one result line reads "79,710 instances in 3.74 ms" on a 7900 XTX. It is unclear which effect this covers. — [GPUOpen preprint PDF](https://gpuopen.com/download/Real-Time_Procedural_Generation_with_GPU_Work_Graphs-GPUOpen_preprint.pdf) (P, not read: too large)
- **Unreal Engine 5.7 Nanite Foliage.** It combines Assemblies, Skinning and Voxels and is labelled experimental. There are no official grass numbers. One third party argues single-card grass does not pay back Nanite's overhead (opinion). A Reddit user saw no gain from voxelising plain static meshes. There are forum reports of VSM and game-thread problems in 5.7.2. — [Epic 5.7 release](https://www.unrealengine.com/news/unreal-engine-5-7-is-now-available) (P); [StraySpark](https://www.strayspark.studio/blog/ue5-nanite-foliage-procedural-placement-performance) (S); [UE forum, VSM with Voxelize](https://forums.unrealengine.com/t/ue-5-7-2-vsm-performance-with-nanite-voxelize-and-r-shadow-nanitelodbias/2706943) (F)
- **Hobby Tsushima-style compute grass.** About 2.36 M blades are processed per frame, 80-100 k are rendered after distance and frustum culling, and the compute pass takes about 0.5 ms (GPU not stated). — [r/GraphicsProgramming post](https://redlib.belloworld.it/r/GraphicsProgramming/comments/18ylaf5/i_created_individually_simulated_grass_inspired) (F)
- No public primary material was found for Assassin's Creed Shadows grass, Zelda BotW/TotK grass culling, Frostbite grass, or Unity demo grass culling.

### Inferences
- The pattern that shipped on console (Tsushima, Yōtei, HZD) is compute + GPU-written draw records, not mesh shaders. Yōtei added occlusion culling to the compute chain. This is route (1).
- HZD's ~0.25 ms covers placing 100 k+ objects of 500 types, not culling 0.5 M blades. It is a ceiling for how heavy a GPU placement or cull stage can be in a shipped budget, not a cost estimate for ours.

### Gaps
- No grass-only ms before/after from any shipped title. Tsushima's tile size, blades per tile, culling tests and timings are unverified (the talk text is not public in a fetchable form).
- Whether Yōtei's occlusion test uses the current or previous frame's depth, and at what granularity (instance, tile, blade), is not public.
- Remedy REAC 2024, the AMD preprint and the HZD deck PDFs could not be read (over 10 MB). Look for their talk videos or transcripts.

## 2. Mesh-shader grass: numbers, sizes, pitfalls, vendor differences

### Takeaway
No public source measures mesh-shader grass against a vertex pipeline. The best cross-vendor data is an expert's general meshlet renderer (Granite, Themaister):
- On an RTX 3070, a meshlet path with per-primitive and micro-poly culling is 2-5x faster than plain indexed draws.
- **Task shaders** used for hierarchical culling were about 10x slower, and about 10% slower even in good cases. He replaced them with indirect-count mesh dispatch from a compute cull.
- On AMD, meshlet size matters a lot. On the Steam Deck the mesh path barely beats MDI.

For our blade strips, which are tiny and have a fixed topology, the mesh shader's gain is mostly per-blade culling inside the group. Compute + indirect gets most of that too.

### Cited Findings
- **Granite, NVIDIA RTX 3070, 63.6 M triangles (13³ instanced scene):**

  | Path | ms |
  |---|---|
  | Indexed draw, no culling | 5.5 |
  | Per-object frustum cull | 4.3 |
  | One MDI with meshlet frustum + cone cull | 3.9 |
  | Mesh shader with per-primitive culling | 3.3 |
  | Micro-poly rejection | 1.9 (1.65 without statistics) |
  | Hierarchical task shader | **18.8** |

  Micro-poly rejection cut submitted primitives from about 31 M to 1.2 M. Inline decoding of compressed meshlets added cost on all GPUs. Non-32-wide sublets "collapsed" in performance. — [Themaister, Modernizing Granite's mesh rendering](https://themaister.net/blog/2024/01/17/modernizing-granites-mesh-rendering/) (E)
- **Granite on AMD:**
  - RX 7600, RADV: indexed draw without culling 5.9 ms, with frustum culling 3.7, MDI 5.0, mesh shader 2.2-3.3 depending on meshlet size and wave mode. The proprietary driver is similar (2.1-2.7).
  - Steam Deck (RDNA2): MDI 8.9 ms against mesh shader 9.3-12.8 ms (32 to 256 per meshlet). The smallest meshlets were fastest, and the mesh path did **not** beat MDI.
  - AMD performance "suffered quite a lot" with NV-style task shaders. — same source (E)
- **Meshlet sizes and limits.** EXT_mesh_shader exposes 256 vertices / 256 primitives on AMD, Intel and NVIDIA (NV_mesh_shader: 512 primitives). NVIDIA's original advice is 64 vertices with 84 or 126 triangles (matched to 128-byte primitive allocation). Meshlets of 8-16 triangles waste execution, because AMD and NVIDIA need at least 32 threads per group. A vertex costs at least 32 bytes of output. "Vendors currently don't publish guidance." — [zeux, Meshlet size tradeoffs](https://zeux.io/2023/01/16/meshlet-size-tradeoffs/) (E)
- **Triangle locality inside a meshlet** gives a significant speedup on NVIDIA and none on an AMD RDNA2 iGPU. — [zeux, Meshlet triangle locality matters](https://zeux.io/2024/04/09/meshlet-triangle-locality/) (E)
- **AMD guidance:**
  - Meshlet size and threads: 128 vertices / 256 triangles recommended (64/126 is similar). Match threads to vertices (128). Declared output sizes are worst-case reservations, so keep them low. Call SetMeshOutputCounts early.
  - Per-primitive culling: `SV_CullPrimitive` per-primitive culling has "minimal" overhead, but culled primitives still count toward output.
  - Occupancy: about 25% occupancy can already reach rasteriser triangle throughput.
  - Task shaders: process 32-64 or more elements per amplification group and compact with wave ops. Launch 32 or more mesh groups per amplification group for amplification. The payload is up to 16 KB but goes through memory, so keep it small. Runs of amplification groups that launch nothing can starve the GPU.
  - RDNA2 exports one vertex and one primitive per thread, while RDNA3 exports at i and i+N. — [GPUOpen, Mesh shaders optimization and best practices](https://gpuopen.com/learn/mesh_shaders/mesh_shaders-optimization_and_best_practices/) (V)
- **Kapoulkine's earlier RTX 2080 numbers** (17.2 ms vs 6.3 ms on a heavy mesh scene) are already in the previous note. They are not grass.

### Inferences
- Our blade fits: LOD0 = 9 vertices / 7 triangles, so 28 blades = 252 vertices / 196 triangles fills a 256/256 group. LOD3 = 3 vertices / 1 triangle, so 85 blades = 255 vertices. AMD's sample uses 32 blades x 8 vertices. Each group would cull its blades (frustum, rank, HiZ) and compact them with a ballot before writing outputs. This is per-blade culling with no memory round trip, and it is the main thing a mesh shader adds over our vertex-shader rejection.
- Granite's result that task shaders cost about 10x when amplifying, and about 10% even at best, argues against task-shader per-blade culling on NVIDIA. The safer mesh variant is "compute cull → indirect-count mesh dispatch", which already contains route (1)'s compute pass.
- Our grass cost tracks submitted vertices and primitives. On an RTX 3080, the measured route-(1) win is removing the 25-43 % of submitted blades that never reach the screen, plus whatever occlusion removes. A mesh shader can additionally drop sub-pixel triangles (Granite's micro-poly rejection was the biggest single step: 3.3 → 1.9 ms). That gain is only real if far blades are often below a pixel. With our widening of far blades they may not be. This is unmeasured.

### Gaps
- No published ms for mesh-shader grass versus vertex-instanced grass on any GPU. No Intel measurements in any source found.
- No source isolates the cost of a compute culling and compaction pass from the draw.

## 3. Platform support in 2025-2026

### Takeaway
On the Vulkan/RTX 3080 side, mesh shaders are mature. On Apple, **MoltenVK does not expose VK_EXT_mesh_shader** (latest release 1.4.2, 2026-07-20). The one public implementation attempt was closed unmerged in August 2026 after failing nearly all CTS tests. Metal itself has had mesh shaders since 2022 (Apple7 family and Mac2). A mesh-shader route therefore needs either a native Metal mesh path or a non-mesh fallback on the Mac. Compute + indirect works everywhere.

### Cited Findings
- **NVIDIA proprietary driver:** VK_EXT_mesh_shader since the 2022 beta 515.49.15. Turing and later have hardware mesh shading. **NVK (Mesa):** gained VK_EXT_mesh_shader in Mesa 26.2 (2026-08-05). — [GamingOnLinux 2022](https://www.gamingonlinux.com/2022/09/cross-vendor-mesh-shading-for-the-vulkan-api-is-now-here/page=1/) (S); [Phoronix, Mesa 26.2](https://phoronix.com/news/Mesa-26.2-Released) (S); [Mesa 26.2.0 release notes](https://docs.mesa3d.org/relnotes/26.2.0.html) (P)
- **AMD RADV:** support since Mesa 22.3, RDNA2 or newer. It was initially behind `RADV_PERFTEST=ext_ms`. Whether that gate is still needed today was not confirmed. — [RADV MR 18367](https://gitlab.freedesktop.org/mesa/mesa/-/merge_requests/18367) (P); [Wccftech](https://wccftech.com/mesa-22-3-gets-updated-radv-radeon-vulkan-driver-with-mesh-shader-support/amp/) (S)
- **Intel ANV:** early Arc support was the NV extension behind an environment variable. Current default status of EXT on ANV was not confirmed. — [HotHardware](https://hothardware.com/news/intel-vulkan-linux-driver-mesh-shader-support-to-arc-alchemist) (S)
- **MoltenVK release notes:** up to 1.4.2 (released 2026-07-20) they mention mesh shaders only as SPIRV-Cross MSL fixes:
  - 1.4.1 (2025-11-24): "Fix mesh shader with no vertex outputs".
  - 1.4.2: subgroup ballot in task and mesh shaders.

  No release lists VK_EXT_mesh_shader as supported. — [MoltenVK Whats_New.md](https://github.com/KhronosGroup/MoltenVK/blob/main/Docs/Whats_New.md) (P)
- **MoltenVK PR #2642**, "WIP: Add support for mesh shaders" (draft from about October 2025), was closed unmerged on 2026-08-01. The author said "another PR is attempting this anyway".
  - Known problems: Metal "Internal compiler error" on most pipelines, no multiview, missing DrawIndex.
  - Last CTS run: 46 passed, 569 failed, 26,152 not supported.

  Tracking issue #2375 (opened October 2024) was open in the snapshot seen. — [MoltenVK PR #2642](https://github.com/KhronosGroup/MoltenVK/pull/2642) (P); [issue #2375 mirror](https://github.stncp.dns-dynamic.net/KhronosGroup/MoltenVK/issues/2375) (P via mirror)
- **Metal mesh shaders (WWDC 2022):**
  - Availability: Apple7 family and Mac2 devices.
  - Per-threadgroup mesh output: up to 256 vertices and 512 primitives, with the `metal::mesh` total at most 16 KB.
  - Object-to-mesh payload at most 16 KB, and one object threadgroup can launch at most 1024 mesh threadgroups.
  - Apple's pitch: no intermediate device-memory buffers and no compute-to-render barrier. No performance numbers given. — [WWDC22 10162, Transform your geometry with Metal mesh shaders](https://developer.apple.com/videos/play/wwdc2022/10162/) (P)
- **Indirect mesh dispatch on Metal:** `drawMeshThreadgroups(indirectBuffer:...)` exists, so a compute cull can feed it. — [Apple docs](https://developer.apple.com/documentation/metal/mtlrendercommandencoder/drawmeshthreadgroups(indirectbuffer:indirectbufferoffset:threadsperobjectthreadgroup:threadspermeshthreadgroup:).md) (P)
- **Work graphs with mesh nodes** are documented for D3D12. On Vulkan they exist only as AMD's experimental `VK_AMDX_shader_enqueue`. — [GPUOpen GDC 2024 page](https://gpuopen.com/learn/gdc-2024-workgraphs-drawcalls/) (V)

### Inferences
- If the Mac build runs through MoltenVK, route (2) means two code paths, mesh on Vulkan and the current vertex path or compute+indirect on Mac, until MoltenVK ships the extension. With no date in sight, route (1) is the only route that is one code path on both platforms.
- Work graphs are not an option for Vulkan on NVIDIA or for Metal today.

### Gaps
- Exact Apple GPU families or Mac models per "Mac2" family, and current M-series payload and grid limits, were not confirmed from Apple's feature-set tables.
- No Apple-published performance of mesh shaders versus vertex shaders on Apple Silicon.

## 4. Compaction: append vs prefix sum, buckets, MDI-count, stable order, cost

### Takeaway
There are no published ms numbers for compacting about 0.5 M instances. Both atomic append and prefix sum are used in shipping-quality renderers, and neither wins everywhere. Order only matters for early-Z efficiency and determinism, not for the final image of opaque depth-tested blades. Our cell-sorted data allows a scheme that keeps the order without a global scan (see Inferences).

### Cited Findings
- A 2009 comparative study of GPU stream compaction: a multi-level prefix-sum method keeps input order, and a hybrid prefix-sum/atomics method does not. No single algorithm is best, since performance depends on data distribution and block size. — [IJNC article](https://www.jstage.jst.go.jp/article/ijnc/7/2/7_208/_article) (P, academic); [scitepress](https://www.scitepress.org/Papers/2009/17836) (P)
- The rend3 renderer ships both an atomic culling path and a prefix-sum culling path. — [rend3 culling source](https://docs.rs/rend3-pbr/0.2.2/src/rend3_pbr/culling/gpu.rs.html) (P, code)
- A two-phase culling write-up uses per-command offsets plus atomic counters for command and instance indices, and notes that group-shared compaction beats global atomics. — [hannomalie, Two-phase occlusion culling part 2](https://hannomalie.github.io/posts/2018-02-07-culling-part-2.html) (F/E)
- Wave or subgroup compaction (ballot + popcount, one atomic per wave) is the vendor-recommended way to compact survivors in task or amplification shaders. Granite uses the same idea for per-primitive compaction. — [GPUOpen best practices](https://gpuopen.com/learn/mesh_shaders/mesh_shaders-optimization_and_best_practices/) (V); [Themaister](https://themaister.net/blog/2024/01/17/modernizing-granites-mesh-rendering/) (E)
- AMD names "empty draw compaction" (dropping indirect draws whose count became 0) as a real cost in ExecuteIndirect pipelines. — [GPUOpen GDC 2024](https://gpuopen.com/learn/gdc-2024-workgraphs-drawcalls/) (V)

### Inferences (own estimates, not measured)
- **Cost of the cull pass.** It reads 0.5 M x 16 B = 8 MB and writes at most 0.5 M x 4 B = 2 MB of indices. At the RTX 3080's roughly 760 GB/s that is about 13 µs of pure bandwidth. Add the HiZ samples (one or a few texel fetches per blade from a cached pyramid) and dispatch and barrier overhead. A realistic total is in the order of 0.03-0.1 ms. This is consistent with the hobby report of about 0.5 ms for 2.36 M blades with simulation included. Compare with the 0.38-0.56 ms grass pass: the cull pays for itself if it removes more than roughly 10-20 % of submitted vertices. We already know 25-43 % are invisible before any occlusion test.
- **Stable order without a global scan.** Records are already sorted by cell, then 2 m square, then rank, so reserve each (cell, LOD) a fixed output window equal to its maximum count.
  - Write survivors with a per-workgroup ballot prefix + one atomic per workgroup into that window.
  - Each (cell, LOD) keeps its own `vkCmdDrawIndexedIndirect` record. The instanceCount is written by the compute pass, or use `vkCmdDrawIndexedIndirectCount` to skip empty ones.
  - Within a workgroup the order is preserved. Across workgroups of one cell it is not, unless the window is split per workgroup (deterministic, slightly sparse).
  - This keeps 80-100 draws and per-LOD index buffers, and needs no CPU change beyond issuing indirect draws.
- **Order and image.** For opaque, depth-tested blades, submission order changes only early-Z rejection and the result at exactly equal depths. TAA sees the same image if order changes frame to frame, except at exact-depth ties. Rough front-to-back order (near cells first) is worth keeping for early-Z, and the per-cell windows keep it.
- **Per-LOD buckets** come free from the per-(cell, LOD) windows. The compute pass picks the blade's LOD (4/3/2/1 segments) and its window, so the vertex shader no longer has to reject blades by distance and rank.

### Gaps
- No measured ms for a compute cull + compaction of ~0.5 M instances on any named GPU. It must be measured on our stand.

## 5. Occlusion for grass: previous-frame HiZ, two-pass, terrain horizon

### Takeaway
The standard is two-phase occlusion (Haar and Aaltonen, SIGGRAPH 2015):
1. Draw what was visible last frame, culled against last frame's HiZ.
2. Rebuild HiZ.
3. Test the rest against the new HiZ and draw newly disoccluded items.

The single-pass variant against a reprojected previous-frame HiZ is cheaper but makes newly revealed objects pop in one frame late. For grass "behind a crest", the occluder is the terrain itself. Our engine already builds a HiZ before the G-buffer pass, so a current-frame test against terrain and large opaque geometry needs no previous frame at all, as long as the terrain is in that pre-pass.

### Cited Findings
- Two-pass origin is attributed to Aaltonen and Haar, SIGGRAPH 2015. Pass 1 draws what was visible last frame. Pass 2 tests the rest against a freshly built HZB and draws the disoccluded objects. Culling only against last frame's HiZ makes disoccluded objects invisible for one frame. — [Mastering Graphics Programming with Vulkan, GPU culling using compute](https://www.packtpub.com/en-pl/product/mastering-graphics-programming-with-vulkan-9781803244792/chapter/chapter-6-gpu-driven-rendering-8/section/gpu-culling-using-compute-ch08lvl1sec42) (S, book); [Krupitskas, Modern culling techniques](https://hn.jakechampion.name/reader/https://krupitskas.com/posts/modern_culling_techniques/) (E)
- Bevy applies two-phase culling to its standard mesh pipeline: the early prepass culls against the previous frame's HZB, and the pyramid is rebuilt after rendering. — [Bevy PR #17413](https://github.fontlos.workers.dev/bevyengine/bevy/pull/17413) (P, code)
- A wgpu meshlet renderer notes that being a frame stale costs "a cluster drawn a frame late", not a wrong image (in its two-phase use). — [nightshade-renderer meshlet_cull.wgsl](https://docs.rs/crate/nightshade-renderer/0.57.0/source/src/wgpu/shaders/meshlet_cull.wgsl) (P, code)
- Assassin's Creed Unity numbers (20-40 % of triangles culled, 30-80 % of shadow triangles) and terrain-horizon culling numbers (-60-80 % in valleys, -5-10 % on hilltops) are already in the earlier note.
- Alan Wake 2 claims single-pixel-precision occlusion using everything as an occluder. — [Wccftech](https://wccftech.com/alan-wake-2-dev-talks-integration-of-mesh-shaders-dlss-3-5-explains-work-graphs-importance-in-future-games/) (S)
- A Unity grass project copies the camera depth into a HiZ texture and builds mips for per-chunk GPU occlusion of grass. — [Project GrassFlow](https://gh.hhj520.top/Mithzzx/Project-GrassFlow) (F)

### Inferences
- **Our case.** The HiZ built before the G-buffer pass holds the depth of whatever was drawn into it. If terrain (and buildings and rocks) are in it, blades behind a crest can be tested against the current frame, with no popping and no second phase. Grass occluding grass is negligible and should not be relied on.
- **Test shape.** Use a blade's bounding segment: root plus height, widened by wind amplitude. Test a conservative screen rect at the mip where it covers 2x2 texels. For per-blade tests that is cheap. A per-2 m-square test first (one thread per square, skipping all its blades) is cheaper still and probably catches most "behind the hill" cases.
- **False occlusion.** It comes from a conservative max-depth HiZ built from the wrong frame, or from not including wind and blade bend in the bounds. With a current-frame HiZ, the remaining risk is HiZ mips built with min instead of max depth (reversed-Z conventions). Check this once against a reference image with culling off.
- If only a previous-frame HiZ were available, the standard fix is two-phase. That doubles the indirect draws and adds a HiZ rebuild, which is not needed here.

### Gaps
- No public measured win for HiZ occlusion on grass alone in a shipped game.
- Whether our pre-G-buffer HiZ contains terrain at full resolution is an engine question, not answered here.

## 6. Published before/after numbers (summary with trust)
- **Granite meshlet renderer, RTX 3070:**
  - indexed 5.5 ms → MDI + meshlet culling 3.9 → per-primitive cull 3.3 → micro-poly rejection 1.9 (1.65 without statistics);
  - task-shader hierarchy 18.8.

  Not grass. **E**, high detail.
- **Granite on AMD:**
  - RX 7600: indexed 5.9 / frustum 3.7 / MDI 5.0 / mesh 2.1-3.3 ms;
  - Steam Deck: MDI 8.9 vs mesh 9.3-12.8 ms (mesh slower). **E**
- **AMD work graphs + mesh nodes vs ExecuteIndirect:** 1.64x on a 7900 XTX, pre-release driver, whole procedural scene including grass. **V**, early.
- **AMD dense vs sparse grass tile:** 49,152 vs 512 triangles (a geometry count, not time). **V**
- **HZD GPU placement:** about 250 µs average GPU for 100 k+ objects (placement, not culling). **P** via snippet.
- **Ghost of Yōtei:** about 1 M+ items culled to about 60 k in one view (no ms). **P** blog.
- **Hobby compute grass:** 2.36 M blades processed into 80-100 k drawn, compute about 0.5 ms, GPU unknown. **F**
- **Own estimate:** a compute cull of 0.5 M x 16 B records on an RTX 3080 costs about 0.03-0.1 ms. Inference, must be measured.

### Takeaway
On public data, route (1) is the safe pick. Compute per-blade culling (distance/rank, frustum, and HiZ against the existing pre-G-buffer pyramid) writes compacted indices into per-(cell, LOD) windows and feeds the existing 80-100 indexed instanced draws through indirect (or indirect-count) draws. It works the same on Vulkan and Metal, keeps a stable order, and its estimated cost is well under the 25-43 % of the 0.38-0.56 ms that the invisible blades now take. Route (2) adds per-blade culling inside the group and micro-triangle rejection, but task shaders are a documented trap, AMD and Steam Deck results are mixed, and MoltenVK has no mesh shaders. Route (3), finer CPU culling, cannot do per-blade HiZ and keeps the vertex-shader rejection cost.

### Cited Findings
- See sections 1-5. No source compares the three routes directly.

### Inferences
- A mesh-shader path is worth revisiting only after route (1), if profiling then shows the cost sits in vertex and primitive submission of visible far blades that are sub-pixel. Even then it would be "compute cull → indirect mesh dispatch", not task shaders.

### Gaps
- No grass-specific A/B of compute+indirect versus mesh shaders versus CPU culling exists in public sources. The decision has to rest on one measured prototype on the stand.

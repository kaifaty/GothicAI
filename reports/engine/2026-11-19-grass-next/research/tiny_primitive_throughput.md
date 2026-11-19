# Throughput limits and submission schemes for very many tiny instanced primitives (grass blades)

Method note: about 25 searches and fetches. This round adds to `reports/engine/2026-11-17-grass-cost/research/gpu_pipeline_cost.md` and does not repeat it. New primary material: an OCR transcript of Wihlidal's GDC 2016 talk (archive.org), AMD's RDNA performance guide, Kerbl et al. 2018 (vertex batches) via a summary, zeux's measured culling numbers on an RTX 4070 Ti, Kitware's vertex-pulling measurements on an RTX A5000 (same GA102 die family as the RTX 3080), and the Outerra procedural-grass draw benchmark. The Nanite slides (Karis 2021) and "Journey to Nanite" (HPG 2022) PDFs exceed the fetch size limit again, Persson's 2012 PDF came back as unreadable compressed streams, and the Wihlidal slide PDF itself was not reachable (the archive.org text transcript was). Trust labels: **P** = primary talk, paper or vendor doc; **V** = vendor blog; **T** = third-party write-up or measurement by a named engineer; **F** = forum or social post.

Reference arithmetic for this project's measured pass (inference, used below): 1.6-2.3 M vertex invocations, 0.9-1.4 M triangles and 350-450 k blades in 0.38-0.56 ms. At the midpoint (2.0 M invocations, 1.15 M triangles, 400 k blades, 0.47 ms) that is about 4.3 G invocations/s, 2.4 G triangles/s and 0.85 G blades/s. At the RTX 3080's ~1.71 GHz boost clock that is about 2.5 invocations, 1.4 triangles and 0.5 blades per GPU clock.

## 1. What bounds millions of tiny triangles from hundreds of thousands of tiny instances with a trivial vertex shader

### Takeaway
Fixed-function front-end rates (primitive assembly/setup at a few triangles per clock for the whole chip, and vertex-wave packing in batches of 32 vertices on NVIDIA) are the documented ceilings, and a frame full of tiny geometry runs below them because of "mostly empty vertex wavefronts". The pass measured here runs at about 1.4 triangles/clock and 2.5 vertex invocations/clock, which is roughly 2.5x below the per-invocation rate and 4-5x below the per-triangle rate that the same GPU family reaches on large meshes. That gap is the cost of tiny instances, not of the shader's ALU work.

### Cited Findings
- Console GCN: "2 shader engines, where each can issue 1 triangle per clock"; newer PC GPUs have four, "a total of 4 triangles per clock". Wihlidal measured "an actual rate of 0.9 triangles per clock with regular rendering" on Xbox One; benchmarks reach about 1.97 prims/clock only with trivially culled, cache-hit vertices. — [Wihlidal, GDC 2016, transcript](https://archive.org/stream/GDC2016Wihlidal/GDC2016-Wihlidal_djvu.txt) (P, OCR of the talk)
- Same talk: "Need positions of a VS back in < 4096 cy, or reduces primitive rate"; "Each GCN rasterizer can read one triangle per clock and produce up to 16 pixels per clock", so pixel-rate efficiency falls to 6.25% when triangles give 1 pixel each. — [Wihlidal transcript](https://archive.org/stream/GDC2016Wihlidal/GDC2016-Wihlidal_djvu.txt) (P)
- Same talk on small draws: "The efficiency loss comes from the GPU still having to run mostly empty vertex wavefronts"; "we end up spinning on vertex shader wavefronts that don't result in any pixels"; "More draws are not necessarily a good thing." — [Wihlidal transcript](https://archive.org/stream/GDC2016Wihlidal/GDC2016-Wihlidal_djvu.txt) (P)
- AMD RDNA primitive rate: 4 triangles/clock after culling, ">> 4" before culling, versus GCN 2-4. — [GPUOpen Let's Build 2020, Optimizing for RDNA](https://gpuopen.com/wp-content/uploads/slides/GPUOpen_Let’sBuild2020_Optimizing%20for%20the%20Radeon%20RDNA%20Architecture.pdf) (V; seen via search summary, deck not opened)
- RDNA: two primitives culled per clock per primitive unit; four units on a 5700 XT, so 8 culled triangles per clock. — [zeux, Fine-grained backface culling](https://zeux.io/2023/04/28/triangle-backface-culling/) (T, citing the RDNA whitepaper)
- NVIDIA RTX 4070 Ti, locked clocks, mesh-shader cluster renderer, geometry-dominated frame: 106.1 M triangles in 8.38 ms and 729.8 M in 55.2 ms (about 12.7-13.2 G triangles/s submitted); culling 53% of triangles by brute force saved only 9% (8.38 to 7.63 ms), while cone culling 13% of triangles (whole clusters skipped) saved 13%. — [zeux, Fine-grained backface culling](https://zeux.io/2023/04/28/triangle-backface-culling/) (T, measured)
- NVIDIA RTX A5000 (GA102), OpenGL: a 2 M-triangle mesh with ~1 M shared vertices drew in 0.18 ms with indexed vertex pulling, 0.19 ms with classic VBO/IBO, 0.31 ms non-indexed; 4.5 M triangles in 0.36 ms. — [Kitware, measuring vertex pulling](https://www.kitware.com/one-polydata-mapper-instead-of-two-measuring-vertex-pulling-on-the-desktop/) (T, measured)
- NVIDIA vertex batching: batches of up to 96 indices (32 triangles) and at most 32 unique vertices, one shader invocation per warp lane; reuse does not cross batches. Measured on GTX 1080 Ti (Kerbl et al. HPG 2018) and confirmed on RTX 3090 ("batch size is indeed 32 triangles/96 indices"). AMD GCN (R7 200): batches of 384 with no separate unique limit; Intel: dynamic, up to 1791. — [Kerbl et al. 2018 slides](https://www.highperformancegraphics.org/wp-content/uploads/2018/Papers-Session2/HPG2018_RevisitingVertexCache.pdf); [TL;DR summary](https://erkaman.github.io/posts/kerbl2018_tldr.html) (P via T summary); [Interplay of Light, Shaded vertex reuse](https://interplayoflight.wordpress.com/2021/11/14/shaded-vertex-reuse-on-modern-gpus/) (T, measured)
- Sebastian Aaltonen: "HW raster is VERY bad for subpixel triangles"; Nanite's software rasteriser beats hardware by about 3x on small triangles; small instanced objects cannot LOD below one cluster. — [Aaltonen thread](https://threadreaderapp.com/thread/1402951008830070790.html) (F, by a named GPU engineer)
- Nanite software-rasterises clusters whose triangles are under ~32 px long ("we can beat the hardware with triangles much bigger than expected"); two specialised software rasterisers run in async compute. — [Karis tweet](https://x.com/BrianKaris/status/1261098487279579136) (F/P, the author); 32 px figure via [HN thread quoting the slides](https://news.ycombinator.com/item?id=36652780) (F); primary [Karis 2021 slides](https://advances.realtimerendering.com/s2021/Karis_Nanite_SIGGRAPH_Advances_2021_final.pdf) not readable here
- "GPUs can only process and rasterize triangles at a certain rate, which is usually relatively low compared to how many pixels they can process". — [GPU Performance for Game Artists](https://www.gamedeveloper.com/programming/gpu-performance-for-game-artists) (T)
- GA102 has up to 7 GPCs / 84 SMs (the 3080 uses 6 GPCs / 68 SMs) with one PolyMorph engine per TPC; no NVIDIA-published triangles-per-clock figure for Ampere was found. Fermi GF100 was documented as 4 triangles set up per clock (4 raster engines). — [GA102 whitepaper](https://images.nvidia.com/aem-dam/en-zz/Solutions/geforce/ampere/pdf/NVIDIA-ampere-GA102-GPU-Architecture-Whitepaper-V1.pdf) (P, only table of contents read); [GF100 whitepaper](https://www.ece.lsu.edu/gp/refs/gf100-whitepaper.pdf) (P via search summary)

### Inferences
- Rate comparison on the same die family (inference from the numbers above): the Kitware large mesh reaches about 11 G triangles/s and, with NVIDIA's 32-unique-vertex batches, roughly 1-2 M vertex invocations in 0.18 ms (about 6-11 G invocations/s). The grass pass reaches about 2.4 G triangles/s and 4.3 G invocations/s. So the grass is about 4-5x below the large-mesh triangle rate and about 1.5-2.5x below its invocation rate. A per-instance or per-wave overhead, not per-vertex ALU work, explains the gap; this matches measured fact (a).
- A two-term fit to the project's own measurements (inference, coarse): let pass time t = a·blades + b·vertices + raster. Removing the far blades (60% of blades, ~47% of vertices) saved ~44% of t; cutting 26% of triangles at the same blade count removes about the same number of vertices (strip: vertices = triangles + 2 per blade), roughly 15-16% of all vertices, and saved ~8.5%. Solving gives b·V ≈ 0.5 t, a·B ≈ 0.3 t, raster/pixels ≈ 0.15 t, which agrees with measured fact (b). So roughly one third of the pass is a fixed per-blade (per-instance) cost and half a per-vertex cost; triangles as such matter little. Treat the split as ±50%, it rests on two experiments.
- A one-triangle far blade costs 3 invocations plus its instance overhead for 1 triangle; a 7-triangle near blade costs 9 invocations plus the same overhead for 7 triangles. Per triangle the far blades are therefore 3-5x more expensive, which is why removing them paid so well.
- The ceiling to aim for is roughly the large-mesh rate: if blades could be submitted at Kitware's per-invocation rate, the same vertex count would cost ~0.2-0.3 ms instead of 0.38-0.56 ms. This is an upper bound on what any submission change can give without reducing vertex count.

### Gaps
- No NVIDIA-published primitives-per-clock or vertex-invocations-per-clock figure for Turing, Ampere or Ada was found. The 4070 Ti number is a mesh-shader workload and the A5000 number is OpenGL; neither is a tiny-instance test.
- The Nanite primary slides and their hardware-versus-software measurements could not be read (PDF over the fetch size limit); the 3x and 32 px figures stay secondary.
- No measurement of what fraction of a GA102 frame goes to vertex-wave launch versus primitive assembly versus setup for tiny instances. Nsight's "PD/VAF/PES/RASTER" unit throughputs in a capture of this pass would answer it directly.

## 2. Are tiny instances (3-9 vertices) slower per vertex than one large draw? Merge-instancing, several blades per instance, vertex pulling, indexed vs non-indexed, reuse

### Takeaway
Yes, consistently in old measurements and by vendor guidance; the evidence on current GPUs is thinner and mixed. Every source that gives a threshold puts the efficient minimum far above one blade: 80-1000 triangles per instance (Outerra), 256 vertices (Unity), 256 triangles per cluster (Ubisoft, Frostbite), 512 triangles (NVIDIA Turing guidance). The established fix is to put many blades in one instance or one non-instanced draw and derive blade and vertex from `gl_VertexIndex`; for strips this also keeps invocations at the minimum.

### Cited Findings
- Outerra procedural grass benchmark (7 vertices / 5 triangles per blade, positions from `gl_InstanceID`/`gl_VertexID`, no attributes; index buffers holding 1 to 65,536 blades per instance): indexed triangle lists gave the best results overall; non-indexed arrays are slower because of the extra strip vertices and only match indexed rendering with one blade per instance. NVIDIA and older AMD should keep instance meshes above about 80 triangles; newer AMD above about 1k; about 5k is good across GPUs; recommendation: group instances so the effective mesh per instance is 1k-20k triangles. Randomised blade order "seems to boost performance on older GPUs a bit". GPUs tested: AMD 7970 to Vega 64, NVIDIA 750 Ti to GTX 1080. — [Outerra, procedural rendering test #1](https://outerra.blogspot.com/2016/01/procedural-rendering-performance-test-1.html) (T, measured; graphs not extracted; raw data at github.com/hrabcak/draw_call_perf)
- Unity: "Meshes that have a low number of vertices can't be processed efficiently using GPU instancing because the GPU can't distribute the work in a way that fully uses the GPU's resources"; do not instance meshes under 256 vertices; use one buffer or a compute shader plus `DrawProcedural` instead. — [Unity discussion quoting the manual](https://forum.unity.com/threads/gpu-instancing-low-vertex-count.1399948/) (V via forum)
- NVIDIA Turing guidance (as quoted): multi-draw-indirect or instancing can still be GPU-bottlenecked for meshes under about 512 triangles; mesh/task shaders "supersede instancing or multi-draw indirect for small meshes"; mesh shaders are recommended for "procedural instancing" such as hair and vegetation. — [Khronos forum thread quoting the Turing slides](https://community.khronos.org/t/gpu-vertex-dispatch-for-multidrawindirect-and-or-instanced-draw-calls/77308) (F quoting V); [NVIDIA, Introduction to Turing mesh shaders](https://developer.nvidia.com/blog/introduction-turing-mesh-shaders) (V); [NVIDIA, Advanced API Performance: Mesh Shaders](https://developer.nvidia.com/blog/advanced-api-performance-mesh-shaders/) (V, via search summary)
- Wave packing: one claim says most GPUs can pack several instances of one `DrawInstanced` into one vertex wave but never several draws (Vulkan/GL spec: invocations from separate draws are in distinct invocation groups); NVIDIA vertex waves are 32 lanes, AMD 64 (GCN). An older write-up says GPUs "won't put separate instances in the same wavefront"; a GameDev.net reply says instance ID is a vector register on AMD so instances can share a wave. — [Khronos forum](https://community.khronos.org/t/gpu-vertex-dispatch-for-multidrawindirect-and-or-instanced-draw-calls/77308) (F); [GUI GPU notes](https://nical.github.io/drafts/gui-gpu-notes.html) (T); [GameDev.net, fastest way to draw quads](https://gamedev.net/forums/topic/676540-fastest-way-to-draw-quads/5279146/) (F). These conflict; no vendor document settles it.
- Aaltonen: instanced draw "is suboptimal on many older GPUs for small 8 vertex / 12 triangle instances", suggests no vertex buffer and decoding from `SV_VertexID`; in 2022 he asked publicly whether recent GPUs can fill waves with 4-vertex instances; later wrote tiny instances are "not a problem" if consecutive instances share one index buffer; "there's still a special index deduplication / vertex wave packing hardware in every GPU". — [Aaltonen, cube from SV_VertexId](https://x.com/SebAaltonen/status/1315985267258519553); [Aaltonen on index dedup hardware](https://x.com/intent/favorite?tweet_id=1968674191076471264) (F, named engineer; statements partly via search summaries)
- Ubisoft (Haar & Aaltonen 2015, as quoted): MDI loses efficiency below about 256-triangle clusters; their 64-vertex strip clusters reach about 1.5 transformed vertices per triangle, about twice as efficient as merge-instancing, which needs about 3 invocations per triangle because it emulates indexing in the vertex shader. — [Khronos forum thread quoting the talk](https://community.khronos.org/t/gpu-vertex-dispatch-for-multidrawindirect-and-or-instanced-draw-calls/77308) (F quoting P)
- Frostbite: "a cluster size of 256 seems to be the sweet spot"; bandwidth "about 20 bytes ... per triangle" without instancing and "1.5 bytes ... per triangle" with instancing. — [Wihlidal transcript](https://archive.org/stream/GDC2016Wihlidal/GDC2016-Wihlidal_djvu.txt) (P)
- Persson merge-instancing (Just Cause 2, Xbox 360): several meshes in one instance, vertex shader fetches vertices itself; odd sizes padded "with degenerate" geometry to a common frequency. — [slide text mirror](https://slaidy.com/prezentacii-po-informatike/siggraph-graphics-gems-for-games) (T mirror of P)
- Kitware (RTX A5000): non-indexed pulling of a shared-vertex mesh is ~1.7x slower than indexed (0.31 vs 0.18 ms) because shared vertices are re-shaded; indexed pulling via `gl_VertexID` ties or beats classic vertex buffers in every regime (ratio 0.89-0.98); adding attributes cost pulling only 4-6%. — [Kitware](https://www.kitware.com/one-polydata-mapper-instead-of-two-measuring-vertex-pulling-on-the-desktop/) (T, measured)
- Mesh shader packing two vertices per thread was only break-even with instanced rendering (0.2% better) on an RTX 2060 SUPER terrain scene. — [Khronos mesh shader performance thread](https://community.khronos.org/t/mesh-shader-performance/107870) (F)
- Frostbite: draws of culled clusters "can easily overwhelm the command processor, which is potentially 1.5ms in a 60hz frame"; "it's extremely important to remove zero sized draws from submission". — [Wihlidal transcript](https://archive.org/stream/GDC2016Wihlidal/GDC2016-Wihlidal_djvu.txt) (P)
- AMD: aim for at least 10 draws per command buffer; ExecuteIndirect fast path uses only a draw in the signature; compact the argument buffer and use a count buffer when draws are culled. — [AMD RDNA performance guide](https://gpuopen.com/learn/rdna-performance-guide/) (V/P)

### Inferences
- Reuse arithmetic for a strip blade: a 9-vertex strip has 7 triangles; whether drawn as a non-indexed strip or an indexed list with good order, the minimum is 9 invocations. There is no reuse left to win inside a blade. NVIDIA's 32-unique-vertex batch fits ~3.5 nine-vertex blades, ~4.5 seven-vertex, ~6 five-vertex or ~10 three-vertex blades; a non-indexed multi-blade draw fills 32-lane warps completely by construction.
- The candidate scheme with the best support: one non-instanced draw (or one instance) per cell/LOD band carrying N blades, `blade = gl_VertexIndex / V`, `vertex = gl_VertexIndex % V` with constant V per LOD band, as a triangle list (3 invocations per triangle, no reuse) or as an indexed list with a shared static index buffer of N blades (9 invocations per 7 triangles). The indexed shared buffer is what Outerra found best. The non-indexed list would raise invocations from 9 to 21 per 7-triangle blade and is likely a loss given the per-vertex term of ~0.5 t (inference from section 1 fit).
- Whether current NVIDIA hardware already packs consecutive 9-vertex instances into one 32-lane warp is unknown from sources. The section 1 fit (~0.3 t per-blade cost) suggests packing is at best partial: if one warp were launched per instance, the per-blade term would dominate and removing 60% of blades would have saved close to 60%. A one-day A/B (same blades, instanced vs N-blades-per-instance with a static index buffer) is the only way to know.
- Outerra's thresholds (80+ triangles per instance on NVIDIA) imply instances of at least ~12 near blades or ~80 far one-triangle blades.

### Gaps
- No measurement on Ampere or Ada of instanced tiny meshes versus the same geometry merged. Outerra (2016) covers up to GTX 1080; its raw CSVs (github.com/hrabcak/draw_call_perf) were not opened.
- Persson's 2012 PDF text could not be extracted (the WebFetch tool returned compressed streams and stored the PDF in its own session cache; it was not read).
- No Apple/Metal source on tiny-instance efficiency.

## 3. What do rejected primitives cost (NaN, degenerate, zero area, off-screen, cull distance) versus not submitting them

### Takeaway
Any submitted vertex is still fetched, shaded and assembled; rejection only saves rasterisation and pixel work. AMD explicitly recommends NaN positions over cull distances or geometry shaders for vertex-shader culling. Nothing is cheaper than not submitting: the 25-43% of submitted blades that never reach the screen, plus the NaN-collapsed rank rejects, likely cost close to their share of the per-blade and per-vertex terms.

### Cited Findings
- AMD: "Culling primitives using a geometry shader or cull distances is expensive"; "Cull primitives from the vertex shader by setting any vertex position to NaN." Also: avoid primitive restart indices where possible, they "can reduce primitive rate on older hardware". — [AMD RDNA performance guide](https://gpuopen.com/learn/rdna-performance-guide/) (V/P)
- NVIDIA DRIVE OS graphics guide: degenerate triangles are trivially rejected; off-screen geometry still has to be loaded, assembled and processed in the vertex shader before the GPU can cull or clip it. — [NVIDIA DRIVE OS, Geometry](https://developer.nvidia.com/docs/drive/drive-os/6.0.7/public/drive-os-linux-sdk/common/topics/graphics_content/Geometry28.html) (P, embedded GPU doc)
- Frostbite: removing culled triangles before the hardware cut 443,429 triangles to 22% and saved 15-30% of rendering synchronously (Fury X: 0.79 to 0.53 ms; XB1: 5.47 to 4.78 ms); with tessellation 40-80%. "On average, 50% of a mesh will be culled from backface." — [Wihlidal transcript](https://archive.org/stream/GDC2016Wihlidal/GDC2016-Wihlidal_djvu.txt) (P)
- zeux (RTX 4070 Ti): removing 53% of triangles individually after the mesh shader ran saved 9%; skipping whole clusters (13% of triangles) saved 13%. Work skipped before vertex/mesh shading pays far more than per-triangle rejection. — [zeux](https://zeux.io/2023/04/28/triangle-backface-culling/) (T, measured)
- Degenerate triangles "incur a negligible amount of vertex processing cost, but ... zero fragment processing cost" (old desktop study); a Unity forum post claims no practical difference between degenerate, off-screen, back-facing and NaN-culled triangles. — [LBNL wesBench 2010](https://dav.lbl.gov/archive/Publications/2010/LBNL-3025E.pdf) (P, dated); [Unity forum, micropolygons](https://discussions.unity.com/t/are-micropolygons-bad/908085) (F)
- RDNA3 counters: culled primitives are counted by reason (scissor, zero area, face); high clipping/culling values may be caused by many small primitives in primitive assembly. — [GPUOpen RDNA3 counters](https://gpuopen.com/?p=57478) (V)

### Inferences
- The project's fact (a) (rejected blades cost the same as computed ones) is consistent with these sources: NaN rejection happens after invocation and primitive assembly, so it saves only setup/raster for those triangles, which section 1 suggests is a small share.
- Collapsing all vertices to one point and NaN should both be rejected at or before setup; no source compares them. NaN is the AMD-recommended form; there is no evidence that a collapsed point is cheaper, and `gl_CullDistance` is documented by AMD as more expensive.
- Expected gain from not submitting (inference): if 25-43% of submitted blades are invisible and a similar share is rank-rejected far blades, compacting them out with a compute pass and an indirect draw removes their per-blade and per-vertex terms (≈0.8 t in the section 1 fit) in proportion, i.e. up to about 0.2-0.35 t, minus the compute pass. Frostbite's 15-30% on a general scene is the closest published analogue.
- Rank rejection by distance is deterministic per blade; it can be moved into the index/instance range (sort blades in a cell by rank, draw only the first k) at zero GPU cost, without a compute pass.

### Gaps
- No NVIDIA desktop document or measurement compares NaN, collapsed, zero-area and cull-distance rejection.
- No source measures the cost of an invocation whose output is NaN versus not launching it on Ampere.

## 4. Triangle strip vs list vs indexed list, index format, primitive restart

### Takeaway
For tiny meshes, the measured best was indexed triangle lists (Outerra). Strips are fine for one blade per instance but need restart or degenerate joins to batch several blades; AMD advises against primitive restart on older hardware. 16-bit indices are what GPU-driven pipelines use.

### Cited Findings
- Indexed triangle lists best overall; non-indexed arrays slower because of extra strip vertices, equal only with one blade per instance. — [Outerra](https://outerra.blogspot.com/2016/01/procedural-rendering-performance-test-1.html) (T, measured)
- Avoid primitive restart indices where possible; they can reduce primitive rate on older hardware. — [AMD RDNA performance guide](https://gpuopen.com/learn/rdna-performance-guide/) (V/P)
- Frostbite compacted output index buffers use 16-bit indices. — [Wihlidal transcript](https://archive.org/stream/GDC2016Wihlidal/GDC2016-Wihlidal_djvu.txt) (P)
- NVIDIA reuse is batch-local (96 indices, 32 unique vertices); strip-ordered lists keep the unique count low per batch. — [Kerbl 2018 summary](https://erkaman.github.io/posts/kerbl2018_tldr.html) (P via T)

### Inferences
- For a multi-blade instance, an indexed list with a static 16-bit index buffer (blade k's triangles use vertices 9k..9k+8) gives 9 invocations per 7 triangles and needs no restart; 65,536 vertices covers ~7,000 nine-vertex blades per instance.
- Non-indexed strips across blades joined by degenerates add 2 invocations per blade (9 → 11) and are not attractive given the per-vertex term.

### Gaps
- No measurement of strips vs lists on Ampere/Ada for tiny meshes, and none of primitive restart cost on NVIDIA.

## 5. Line or point primitives for the far one-triangle blades

### Takeaway
No source measures line or point throughput against triangles on current GPUs. Portability is poor: wide lines are optional in Vulkan and absent in Metal; points have size on Metal but render as screen-aligned squares. The idea is unproven and has look risks under TAA.

### Cited Findings
- Vulkan: line width other than 1.0 requires the `wideLines` feature; desktop NVIDIA/AMD/Intel drivers support it, MoltenVK does not. — [Vulkan vkCmdSetLineWidth](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdSetLineWidth.html) (P); [ChimeraX ticket on line width](https://plato.cgl.ucsf.edu/trac/ChimeraX/ticket/1315) (T)
- Metal: point primitives take `point_size`; line primitives have no width control, so wide lines must be built as triangles. — [Apple developer forum, wide 3D line in Metal](https://developer.apple.com/forums/thread/39242) (F, Apple forum)
- Line rasterisation cost in line-set rendering depends on line orientation (fragments traversed). — [Line-set rendering comparison, TVCG 2020](https://www.sci.utah.edu/~will/papers/tvcg20_oit/tvcg_oit_lines.pdf) (P, scientific visualisation)

### Inferences
- A 1-px line uses 2 vertices instead of 3 for a far blade; under the section 1 fit that is at most ~1/3 of the per-vertex term for those blades, while per-blade overhead stays. A point (1 vertex) would remove more but draws a square, not a blade, and has no width taper or bend.
- Lines and points are rasterised with their own rules (diamond-exit for lines, no MSAA-like partial coverage without extra features), so a TAA upscale from 2/3 resolution would make them shimmer more than widened triangles. Inference, untested.
- Merging far blades into fewer, larger instances (section 2) is a safer route to the same saving.

### Gaps
- No primitive-rate data for lines or points on any current GPU; no grass or hair renderer found that uses hardware lines in production.

## 6. Vertex-shader wave occupancy and batching inefficiency with very small draws or instances

### Takeaway
Sources agree that small draws waste vertex-wave lanes (rounding up to 32 on NVIDIA, 64 on GCN) and that separate draws never share a wave. Whether instances of one draw share a wave on current hardware is contested. With ~80-100 draws of thousands of blades each, draw-level waste is small here; instance-level waste is the open question.

### Cited Findings
- Each draw's vertex cost rounds up to the wave size (NVIDIA 32, AMD GCN 64); separate draws are not packed. — [Khronos forum](https://community.khronos.org/t/gpu-vertex-dispatch-for-multidrawindirect-and-or-instanced-draw-calls/77308) (F)
- "Hull shaders tend to have very few active threads out of the 64 per wave" (tessellation analogue of tiny work items). — [Wihlidal transcript](https://archive.org/stream/GDC2016Wihlidal/GDC2016-Wihlidal_djvu.txt) (P)
- GCN2 consoles: "notoriously bad for geometry processing (VS max occupancy = 2)". — [Aaltonen thread](https://threadreaderapp.com/thread/1402951008830070790.html) (F)
- Apple GPUs process all geometry of a render pass in the vertex stage and bin it into tiled buffers before fragment work; a newer Apple GPU generation doubled geometry throughput (chip not confirmed). — [Apple, WWDC20 session 10632](https://developer.apple.com/videos/play/wwdc2020/10632?time=676) (P, via search summary); [Apple tech talk 111431](https://developer.apple.com/videos/play/tech-talks/111431/) (P, via search summary, unverified)

### Inferences
- On Apple TBDR, every submitted blade passes the vertex stage and survivors are binned, so not submitting invisible blades should help at least as much there as on the 3080. Inference, no Apple numbers.
- For this pass, the practical test is Nsight Graphics' warp-launch counters for the vertex stage ("VS warps launched" vs invocations): invocations / (32 × warps) gives lane occupancy directly. If occupancy is near 9/32 for near blades and 3/32 for far blades, instances are not packed and merging blades into instances is the main lever; if near 100%, the per-blade term is elsewhere (attribute/primitive setup).

### Gaps
- No vendor document states how NVIDIA Ampere or Apple GPUs pack instances into vertex warps; only informal posts, which conflict.
- No source measured mesh/task shaders for grass on Ampere; NVIDIA recommends them for vegetation but gives no numbers.

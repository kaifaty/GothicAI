# Mesh → SDF/voxel source → game meshes with LOD: practical pipeline (state Nov 2026)

Scope note: research done with ~16 web calls. Several primary documents (Barill 2018 PDF > 10 MB, Blender manual node body, GDC Vault talks) could not be read in full; claims that rest on prior knowledge rather than a fetched page are marked **[unverified]**. Storage and triangle figures that are my own arithmetic are marked **[estimate]** and show the formula.

## 1. Mesh to SDF for non-watertight, non-manifold meshes

### Takeaway
Sign is the hard part, not distance. OpenVDB's signed conversion needs a *closed* (not necessarily manifold) surface; for open sheets it offers only an unsigned field. Generalized / fast winding numbers give a robust inside/outside score for soups with holes and self-intersections, but for a one-sided open terrain sheet the score is fractional and the 0.5 threshold is a convention that is least reliable near open borders — so for terrain the practical recipe is "close the sheet first (skirt down to a floor / box), then sign".

### Cited Findings
- OpenVDB `MeshToVolume.h` (OpenVDB 13.x docs, 2025–2026): "The signed distance field conversion requires a closed surface but not necessarily a manifold surface"; it "Supports surfaces with self intersections and degenerate faces and is independent of mesh surface normals / polygon orientation." — [OpenVDB doxygen MeshToVolume.h](https://www.openvdb.org/documentation/doxygen/MeshToVolume_8h.html)
- API: `meshToLevelSet` (symmetric half-width, default `LEVEL_SET_HALF_WIDTH` = 3 voxels), `meshToSignedDistanceField` (asymmetric exterior/interior band), `meshToUnsignedDistanceField` (one band), generic `meshToVolume` with flags `UNSIGNED_DISTANCE_FIELD`, `DISABLE_INTERSECTING_VOXEL_REMOVAL`, `DISABLE_RENORMALIZATION`, `DISABLE_NARROW_BAND_TRIMMING`, an optional `polygonIndexGrid` (closest-polygon index per voxel), and an `interiorTest` callback with strategy `EVAL_EVERY_VOXEL` / `EVAL_EVERY_TILE`. Sign by default comes from `traceExteriorBoundaries` (a flood from the exterior). — [OpenVDB doxygen](https://www.openvdb.org/documentation/doxygen/MeshToVolume_8h.html)
  - Practical consequence: the `interiorTest` hook is where a winding-number test can be plugged in instead of the exterior flood; the `polygonIndexGrid` is how per-triangle material can be carried into voxels (see §5).
- Unsigned field is zero at the interface and grows monotonically away from it (OpenVDB forum). — [openvdb-forum](https://groups.google.com/g/openvdb-forum/c/za6xsCb87vU)
- Fast winding numbers (Barill, Dickson, Schmidt, Levin, Jacobson, SIGGRAPH 2018): tree-based (Barnes–Hut-style) evaluation, "up to 1000×" speedup over direct summation; targets triangle soups and point clouds; applications listed: voxelization, signing distances, defect-tolerant booleans. — [project PDF](https://www.dgp.toronto.edu/projects/fast-winding-numbers/fast-winding-numbers-for-soups-and-clouds-siggraph-2018-barill-et-al.pdf), [SIGGRAPH history](https://history.siggraph.org/?p=100702)
- Generalized winding numbers (Jacobson, Kavan, Sorkine-Hornung 2013) are robust to self-intersections, holes and degeneracies; round-off is a "non-issue when evaluation points are not very close to open boundaries". — [Jacobson 2013 PDF](https://users.cs.utah.edu/~ladislav/jacobson13robust/jacobson13robust.pdf), [2016 summary](https://alphaxiv.org/abs/1601.07953v1)
- For open surfaces the winding number is not an integer and varies continuously; 0.5 is the conventional threshold (NVIDIA Warp default 0.5); a point on the symmetry plane of an open cube gives ≈ 0.5, i.e. ill-posed. — [FOSSIL guide](https://szaghi.github.io/FOSSIL/guide/advanced/winding-number), [Warp mesh_query_point_sign_winding_number](https://nvidia.github.io/warp/v1.16/_sources/language_reference/_generated/warp.mesh_query_point_sign_winding_number.rst.txt)
- Implementations: libigl tutorial 717 (fast winding number for soups; was in the dev branch when replicated); PyMesh/WindingNumber is a soup-only reimplementation. — [replicability review](https://graphicsreplicability.github.io/replicability.graphics/papers/10.1145-3197517.3201337/index.html), [PyMesh/WindingNumber mirror](https://github.xxxxic.workers.dev/PyMesh/WindingNumber)
- 2026 work: a preprint describes Barill's method as fast but approximate and proposes an exact "antipodal" method claimed faster than prior exact ones. — [arXiv 2605.01536](https://arxiv.org/html/2605.01536v1); also "Fast and Robust Point Containment Queries on Trimmed Surfaces" (2025). — [arXiv 2510.25159](https://arxiv.org/html/2510.25159v1)
- NVIDIA Warp exposes `mesh_query_point_sign_winding_number` (GPU, Apache-2.0 **[licence unverified here]**). — [Warp docs](https://nvidia.github.io/warp/v1.16/_sources/language_reference/_generated/warp.mesh_query_point_sign_winding_number.rst.txt)

### Inferences (failure modes and recipe for a Gothic-style region mesh)
- **One-sided terrain sheet with open map border**: exterior flood (OpenVDB default) leaks around the border → everything outside; winding number gives ~0.5 near the edge and drifts with distance. Fix: add a skirt from the map border down to a floor plane (or intersect with a bounding box and cap the bottom) so the terrain plus skirt is closed; then either flood-fill sign or winding-number sign works.
- **Caves/tunnels** are closed volumes of air inside the "solid": they sign correctly only if the cave mesh connects to the outer sheet without gaps; any gap at a cave mouth makes the cave interior ambiguous (winding ~0.5) — must be patched or seeded.
- **Thin walls/buildings** (walls, bridges, roofs thinner than ~2 voxels) vanish or merge in any narrow-band SDF at 0.5–1 m voxels; they must not go through the SDF (keep them as meshes, §2).
- **Interior faces / duplicate coplanar faces**: harmless for unsigned distance, they add ±1 to winding numbers in zones and can create spurious "solid" pockets; harmless for exterior flood only if they are fully enclosed. Remove by material/connectivity before voxelising.
- **Alpha-tested surfaces** (fences, foliage cards, grates) are geometry the SDF should never see; exclude by material.
- Practical recipe: (1) split mesh (§2); (2) close terrain sheet with skirt; (3) `meshToVolume` with `polygonIndexGrid` and either default exterior trace or `interiorTest` = fast winding number ≥ 0.5; (4) inspect residual voxels whose winding is in [0.3, 0.7] — those are leak sites to patch in Blender.

### Gaps
- Could not read Barill 2018 timings in full (PDF > 10 MB). Commonly quoted: millions of queries per second on CPU for million-triangle soups **[unverified]**.
- No source measured on a game level mesh with caves; behaviour above is reasoned from the method definitions.

## 2. Separating natural terrain from man-made geometry; how voxel games place buildings

### Takeaway
Shipped voxel games keep buildings as authored models and *stamp* them into or onto the voxel world, rather than deriving them from a terrain SDF. For an existing mixed mesh, the split is by material (texture names) first, then by connectivity, with manual fix-ups at seams.

### Cited Findings
- Enshrouded (Keen Games): custom voxel engine; buildings and POIs are injected into the voxel world from 3D models and hand-placed stamps (2024 AMA recap; no resolution figures given). — [Enshrouded AMA recap](https://enshrouded.com/es-ES/news/enshrouded-world-design-team-reddit-ama-recap), [Creating the world of Enshrouded](https://enshrouded.com/news/creating-the-world-of-enshrouded)
- Community analysis (unofficial, 2024) of Enshrouded's mesher guesses dual contouring / surface nets and "basically no voxel LOD" for model-based voxels — speculation. — [r/VoxelGameDev thread](https://lr.in.psf.lt/r/VoxelGameDev/comments/1c453u3/isosurface_algorithm_used_by_keen_games_in)
- Voxel Plugin 2 (UE5) markets "use any mesh to shape terrain" (mesh stamps). — [80.lv 2024](https://80.lv/articles/voxel-plugin-2-for-ue5-lets-you-use-any-mesh-to-shape-terrain)
- Unreal's standard practice: heightmap Landscape + holes (Visibility tool) + static meshes for caves/overhangs; the join "can create a visible seam". — [UE Landscape Visibility tool](https://dev.epicgames.com/documentation/en-us/unreal-engine/landscape-visibility-tool-in-unreal-engine), [UE forum](https://forums.unrealengine.com/t/terrain-caves/2290)

### Inferences
- In Gothic's mesh, terrain/cliff/cave materials (grass, earth, rock, cave rock) are a small known set; triangles with those materials → "natural" set; everything else (wood, plaster, roof, stone masonry, alpha-tested) → "built" set. Connectivity check: built pieces that touch the ground keep their bottom edge; the SDF ground surface will no longer coincide with it, so plan a "foundation sink" (extend building bottoms 0.5–1 m into the ground) rather than exact welds.
- Masonry tunnels and city walls are ambiguous (rock texture on man-made shapes); expect a manual list.

### Gaps
- No public source found for Enshrouded's voxel size or for how Gothic-like mixed meshes were split in any remaster.

## 3. Meshing back: MC, surface nets, DC, manifold DC, transvoxel, LOD; seams; decimation

### Takeaway
Surface nets give ~half the primitives of marching cubes and are fastest but bevel sharp edges; dual contouring with Hermite data/QEF keeps sharp edges at the cost of possible non-manifold output (fixed by manifold DC). Crack-free LOD is done either with Transvoxel transition cells, with skirts, or (common in modern engines) by locking chunk-border vertices during quadric simplification of a fine mesh.

### Cited Findings
- 0fps (2012) measured on demo fields (vertices/faces): terrain MC 15,268/7,638 vs surface nets 3,816/3,701; noise MC 172,705/88,071 vs SN 41,888/40,995; timing at high frequency MC 274.6 ms vs SN 124.3 ms vs marching tetrahedra 1,420 ms; MT surfaces always manifold, SN can create non-manifold vertices; naive SN is "bevelled"; DC "can preserve sharp features". A 2019 comment disputes some MC counts. — [0fps, Smooth voxel terrain part 2](https://0fps.net/2012/07/12/smooth-voxel-terrain-part-2/)
- Manifold Dual Contouring (Schaefer, Ju, Warren, IEEE TVCG 13(3) 2007, pp. 610–619): octree topology-preserving vertex clustering; output contains only manifold vertices/edges, preserves sharp features under adaptive simplification. QEF merge via normal equations ≈ 10 ops vs QR ≈ 150 ops (Schaefer & Warren). — [catalog record](https://katalog.dhi-paris.fr/vufind/Record/NLM16893812X/Description), [paper PDF](https://people.engr.tamu.edu/schaefer/research/dualsimp_tvcg.pdf), [slides](https://people.engr.tamu.edu/schaefer/research/slides/manifoldDC.pdf), [Ju et al. 2002 DC](https://www.cs.wustl.edu/~taoju/research/dualContour.pdf)
- Transvoxel (Lengyel, 2010): transition cells on faces between LODs; 512 transition cases reduced to 73 classes; described as patent-free and used in Space Engineers and Astroneer (secondary source, 2026). — [Cinevva blog 2026](https://app.cinevva.com/blog/2026-02-25-open-world-browser-part-09-transvoxel-first-cut); original at transvoxel.org **[not fetched]**
- Godot Voxel Tools (Zylann) uses Transvoxel; stitching "may still be visible as occasional sharp little steps", smoothed in shader via a per-side `u_transition_mask`; each LOD doubles block and voxel size; octree streaming (one viewer) or "Clipbox" (concentric boxes, multi-viewer); LOD fade 0.25 s recommended; SDF stored 16-bit (range ±500, step ~0.015) or 8-bit (±10, step ~0.078; "blocky" with LOD). — [Voxel Tools docs: smooth terrain](https://voxel-tools.readthedocs.io/en/latest/smooth_terrain/)
- Anecdote: a developer could not make Transvoxel fast; Voxel Plugin authors reportedly moved from Transvoxel to surface nets (forum). — [gamedev.net surface nets LOD thread](https://gamedev.net/forums/topic/719387-surface-nets-lod-chunk-structure)
- meshoptimizer (MIT, Arseny Kapoulkine): `meshopt_simplify` with lock-border option for chunked terrain ("ensure no seams appear" between adjacent chunks per glTF-Transform docs); `meshopt_simplifyWithAttributes` (from v0.20, experimental then) with per-vertex lock/protect flags. — [glTF-Transform SimplifyOptions](https://gltf-transform.dev/modules/functions/interfaces/SimplifyOptions), [meshoptimizer releases](https://github.com/zeux/meshoptimizer/releases), [Rust binding source](https://docs.rs/meshopt/0.6.2/src/meshopt/simplify.rs.html)
- Nanite-style LOD rebuild: simplify cluster groups with locked group borders to avoid cracks between LODs. — [jglrxavpok 2024](https://jglrxavpok.github.io/2024/01/19/recreating-nanite-lod-generation.html)
- Manifold library has a `LevelSet` SDF mesher claimed to improve "significantly" over marching cubes (no numbers in README). — [Manifold GitHub](https://github.com/elalish/manifold)

### Inferences
- **[estimate] Triangle counts.** Surface nets ≈ 1 quad (2 tris) per surface cell. Surface cells per m² ≈ relief × (1/h)². 64 m chunk, relief factor 1.5: h = 1 m → ~12k tris; h = 0.5 m → ~49k; h = 0.25 m → ~197k. MC gives ~2× that (0fps ratio). This is far above a 2002 chunk, so a decimation pass (quadric, border-locked) to the original-plus-detail budget is mandatory; quadric simplification on flat-ish ground typically removes 80–95 % without visible loss **[unverified, typical]**.
- Recommended for an offline pipeline (no runtime meshing yet): fine surface nets or DC per chunk with 1-voxel overlap → weld → meshoptimizer simplify with border vertices locked (shared by both chunks) → LOD levels by simplifying again with locked borders, or skirts for far LODs. Transvoxel only matters if meshing at runtime from multi-resolution voxels.
- Sliver triangles: MC produces them near grid corners; SN/DC much less; decimation cleans most.

### Gaps
- No public per-chunk Transvoxel or DC timings from shipped games found.

## 4. Resolution and storage

### Takeaway
To keep 2002-era hand-made features (paths 1.5–3 m wide, ledges ~0.3–0.5 m, cave mouths 2–4 m), a 0.25–0.5 m voxel near walkable areas is needed; 1 m loses ledge steps and rounds paths. Narrow-band sparse storage scales with surface area, so 4–10 km² at 0.5 m is in the hundreds of MB to ~1.5 GB uncompressed float; 8/16-bit quantisation brings it to a few hundred MB.

### Cited Findings
- Narrow-band (sparse) storage tracks surface area, not volume; dense grids grow with the embedding volume. — [Museth, VDB TOG 2013](https://research.dreamworks.com/wp-content/uploads/2018/08/Museth_TOG13-Edited.pdf)
- NanoVDB: flat contiguous buffer, CPU↔GPU, sparse narrow-band level set examples; in-core variable bitrate quantisation, compression "rarely exceeds a modest factor". — [NVIDIA blog](https://developer.nvidia.com/blog/accelerating-openvdb-on-gpus-with-nanovdb), [NeuralVDB paper](https://ar5iv.labs.arxiv.org/html/2208.04448)
- NanoVDB not as memory-efficient for level sets as DT-Grid (CVPR 2023). — [PlenVDB CVPR 2023](https://openaccess.thecvf.com/content/CVPR2023/papers/Yan_PlenVDB_Memory_Efficient_VDB-Based_Radiance_Fields_for_Fast_Training_and_CVPR_2023_paper.pdf)
- OpenVDB default band half-width 3 voxels per side. — [OpenVDB doxygen](https://www.openvdb.org/documentation/doxygen/MeshToVolume_8h.html)
- Godot Voxel Tools: 16-bit SDF step ~0.015 units; 8-bit causes blockiness with LOD. — [Voxel Tools docs](https://voxel-tools.readthedocs.io/en/latest/smooth_terrain/)
- Community speculation for a voxel game (DRG): ~0.5 m voxels (unverified Reddit). — [r/howdidtheycodeit](https://redlib.hbubli.cc/r/howdidtheycodeit/comments/1e69166/how_did_they_code_the_pathfinding_implementation)

### Inferences — [estimate] storage
- Active voxels ≈ surface area × (1/h)² × band thickness (6 voxels for half-width 3). Surface area = map area × relief factor (~1.5, plus caves).
  - 4 km² × 1.5 = 6 M m²: h = 1 m → 36 M voxels (~144 MB float); 0.5 m → 144 M (~0.6 GB); 0.25 m → 576 M (~2.3 GB).
  - 10 km² × 1.5 = 15 M m²: 1 m → 90 M (~0.36 GB); 0.5 m → 360 M (~1.4 GB); 0.25 m → 1.44 G (~5.8 GB).
  - Add VDB tree overhead (~10–20 %) **[unverified]**; halve with 16-bit, quarter with 8-bit; plus material ID channel (1 byte/voxel) if stored.
- Practical: 0.5 m global, 0.25 m only in tiles flagged as paths/cave mouths/quest spots (OpenVDB supports multiple grids per tile; or store the authored mesh there and don't remesh). Source files fit on disk; only the build pipeline needs them, the game ships triangles.
- Build time: OpenVDB `meshToVolume` is multithreaded (TBB) and typically handles millions of triangles into hundreds of millions of voxels in seconds to minutes on a 16-core machine **[unverified, no measurement found]** — measure on Ryzen.

### Gaps
- No published voxel sizes of Enshrouded, Astroneer, NMS; no published storage numbers for game-scale VDB terrain.

## 5. Materials and UVs after remeshing

### Takeaway
Remeshed terrain loses authored UVs; voxel games use triplanar projection plus per-voxel material IDs or weights. Original per-triangle materials can be transferred via OpenVDB's closest-polygon index grid (or a nearest-triangle query at each new vertex), but texel-exact hand-placed UV details (decals painted into the texture, path edges aligned to UVs) are lost.

### Cited Findings
- Smooth meshers "don't provide any proper UV" → triplanar mapping; options: procedural (normal + height), single 8-bit index per voxel (256 textures, no gradients), Mixel4 (4 textures+weights per voxel, 4-bit weights, max 16 textures); per vertex 4 indices + 4 weights at 8 bits; blending > 4 textures per voxel may artefact. — [Voxel Tools docs](https://voxel-tools.readthedocs.io/en/latest/smooth_terrain/)
- Voxel Plugin 2 supports painting arbitrary materials. — [80.lv](https://80.lv/articles/paint-any-material-with-voxel-plugin-2-for-ue5)
- OpenVDB `meshToVolume` can output a `polygonIndexGrid` (index of the closest polygon per voxel). — [OpenVDB doxygen](https://www.openvdb.org/documentation/doxygen/MeshToVolume_8h.html)
- Manifold tracks per-face IDs/original-object IDs and arbitrary vertex properties. — [Manifold GitHub](https://github.com/elalish/manifold)

### Inferences
- Pipeline: for each output vertex, nearest original triangle (BVH, libigl/embree) → material ID and barycentric UV of the original → either (a) keep original UV where the remeshed surface is within ε of the original (planar-ish ground), or (b) triplanar for cliffs. Material boundaries become per-vertex blends instead of sharp triangle edges — paths drawn by material borders will soften; need splat-map width tuning.

### Gaps
- No source quantifying visual loss vs authored UVs.

## 6. Runtime edits (craters): cost, collision, navigation

### Takeaway
Public numbers from shipped games are scarce; libraries remesh one chunk in milliseconds off-thread. Several popular "voxel-looking" games may actually do mesh CSG at runtime (DRG — community claim).

### Cited Findings
- 0fps: SN ~124 ms vs MC ~275 ms for a whole benchmark volume (2012 JS) — per-chunk cost scales with surface cells. — [0fps](https://0fps.net/2012/07/12/smooth-voxel-terrain-part-2/)
- Unity open-source MC project: destruction "very fast", colliders baked on separate threads; most lag is waiting for jobs on pooled chunks while traversing. — [Fast Unity Marching Cubes](https://gh.r-mj.com/Fobri/Fast-Unity-Marching-Cubes)
- DRG: community claims runtime terrain is CSG on meshes after voxel-like generation; ~0.5 m voxel guess — unverified. — [r/howdidtheycodeit 2024](https://redlib.hbubli.cc/r/howdidtheycodeit/comments/1e69166/how_did_they_code_the_pathfinding_implementation)
- Voxel Tools: detail normal maps are "significantly more expensive than generating regular blocks". — [Voxel Tools docs](https://voxel-tools.readthedocs.io/en/latest/smooth_terrain/)

### Inferences
- Crater edit = CSG on the SDF (min/max with a sphere SDF) over affected 64 m chunk(s) → remesh only affected sub-blocks (e.g. 16³–32³ cells) → rebuild Jolt mesh shape for that sub-block → mark navmesh tiles dirty (Recast tiled rebuild). Expect single-digit ms per 32³ block remesh on CPU **[estimate]**; collision cook and navmesh tile rebuild usually dominate.
- Requires keeping the SDF (or a local window) in the shipped game for destructible zones only.

### Gaps
- No primary timings from Astroneer/Enshrouded/DRG/Teardown found.

## 7. Keeping gameplay-critical geometry exact; validating walkability

### Takeaway
No public "remaster validation" methodology found; the practical approach is automated comparison: original vs remeshed navmesh, height deltas at path/waypoint samples, and protection masks where the original mesh is kept verbatim.

### Cited Findings
- Hybrid heightfield+meshes leave visible seams at joins (UE forum), so exactness at joins is a known problem rather than solved. — [UE forum](https://forums.unrealengine.com/t/terrain-caves/2290)
- meshoptimizer per-vertex protect/lock flags allow freezing chosen vertices during simplification. — [meshopt Rust source](https://docs.rs/meshopt/0.6.2/src/meshopt/simplify.rs.html)

### Inferences
- Validation suite: (1) sample every waypoint/freepoint and every path-material triangle: |z_new − z_orig| < 0.1–0.2 m; (2) build Recast navmesh on both and compare reachable-area graph / per-waypoint-pair path lengths; (3) slope histogram per walkable triangle; (4) bot-walk e2e along routine paths. Protect: lock vertices within N m of waypoints and keep cave mouths at 0.25 m.

### Gaps
- No published source on how others validated a remeshed hand-made world.

## 8. Alternatives without an SDF

### Takeaway
For most of an island, a heightfield + meshes for caves/overhangs is the industry default; subdivision/displacement of the existing mesh preserves UVs and gameplay exactly; mesh booleans (Manifold) handle craters but require manifold input, which the original mesh is not.

### Cited Findings
- Heightfield can't represent overhangs; standard: landscape holes + static meshes for caves, visible seams possible. — [UE Visibility tool](https://dev.epicgames.com/documentation/en-us/unreal-engine/landscape-visibility-tool-in-unreal-engine), [UE forum overhangs](https://forums.unrealengine.com/t/overhanging-cliff-edge/330065)
- A blog claims UE 5.8 adds experimental "mesh terrain" (true 3D mesh), recommending heightmap for broad world + mesh terrain for caves/cliffs — single non-Epic source, unverified. — [StraySpark blog](https://www.strayspark.studio/blog/mesh-terrain-ue5-8-caves-overhangs-guide)
- Manifold: Apache-2.0; "guaranteed manifold output"; input must be manifold (non-manifold import returns an error; `Merge` fixes slightly non-manifold meshes); tracks face/material IDs; used by Blender and OpenSCAD. — [Manifold GitHub](https://github.com/elalish/manifold)
- Fast winding numbers support "defect-tolerant mesh booleans". — [Barill 2018](https://www.dgp.toronto.edu/projects/fast-winding-numbers/fast-winding-numbers-for-soups-and-clouds-siggraph-2018-barill-et-al.pdf)

### Inferences
- Subdivision + displacement keeps UVs, materials, waypoints (displacement 0 on path material) — the cheapest route to "more detail"; does not give HLOD, seamless region joins, or craters.
- Craters via Manifold need the chunk (or a local patch) closed into a solid (skirt + bottom cap) → watertight → boolean → reproject UVs. Feasible offline/at runtime for small patches.
- HLOD without SDF: meshoptimizer `simplifySloppy`/quadric on merged chunks with locked borders.
- Region joins without SDF: stitch border loops or overlap + hide.

## 9. Libraries and tools (licences)

### Cited Findings
- OpenVDB (ASWF, MPL-2.0 **[licence from prior knowledge]**), current docs 13.x; mesh→volume, level-set filters, volume→mesh (`volumeToMesh` adaptive) **[volumeToMesh not fetched]**. — [OpenVDB docs](https://www.openvdb.org/documentation/doxygen/MeshToVolume_8h.html)
- NanoVDB (part of OpenVDB): GPU read-only sparse grids. — [NVIDIA blog](https://developer.nvidia.com/blog/accelerating-openvdb-on-gpus-with-nanovdb)
- Manifold: Apache-2.0, booleans + LevelSet meshing. — [GitHub](https://github.com/elalish/manifold)
- meshoptimizer: MIT. — [releases](https://github.com/zeux/meshoptimizer/releases), [Godot vendor note](https://remotebranch.eu/Stowage/godot/commit/cf9df3b5d49a80e8adad87dc362e479cc1d7d436)
- libigl (MPL-2.0 **[prior knowledge]**): fast winding number (tutorial 717), signed_distance. — [replicability review](https://graphicsreplicability.github.io/replicability.graphics/papers/10.1145-3197517.3201337/index.html)
- Blender geometry nodes (2024+, experimental volume grids; manual now 5.3): Mesh to SDF Grid (voxel size, band width in voxels), Points to SDF Grid, Mesh to Volume, Grid to Mesh, SDF Grid Boolean, SDF Fillet, SDF Grid Laplacian/Mean/Median/Mean Curvature, SDF Grid Offset. — [Blender manual Mesh to SDF Grid](https://docs.blender.org/manual/en/latest/modeling/geometry_nodes/mesh/operations/mesh_to_sdf_grid.html), [Blender PR #118761](https://projects.blender.org/blender/blender/pulls/118761)
- Godot Voxel Tools (Zylann): Transvoxel + material blending; licence MIT **[prior knowledge; docs page states none]**. — [docs](https://voxel-tools.readthedocs.io/en/latest/smooth_terrain/)
- Voxel Plugin 2: $349 perpetual, budget cap $100k, no free tier stated; a free legacy version (Free Legacy repo, older UE) exists. Not usable for a non-UE engine anyway. — [licensing](https://voxelpluginvitedocs.pages.dev/resources/licensing), [VoxelPluginFreeLegacy](https://scriptagc.wasmer.app/https_github_com/VoxelPlugin/VoxelPluginFreeLegacy/tree/710b2496ba7b15399e5ddf22947d79e4d19f3aba)
- NVIDIA Warp: GPU winding-number sign query. — [Warp docs](https://nvidia.github.io/warp/v1.16/_sources/language_reference/_generated/warp.mesh_query_point_sign_winding_number.rst.txt)

### Inferences
- Minimal stack: Blender (split, skirts, inspection) + OpenVDB Python (pyopenvdb/`openvdb` module) for mesh→SDF, filtering, CSG + own surface-nets/DC per chunk or OpenVDB `volumeToMesh` + meshoptimizer for decimation/LOD + libigl/Warp for winding numbers + Manifold for runtime/offline booleans on closed patches.

### Gaps
- Licence of OpenVDB/libigl/Voxel Tools not re-verified in this session.
- No independent benchmark comparing OpenVDB `volumeToMesh` with DC on terrain.

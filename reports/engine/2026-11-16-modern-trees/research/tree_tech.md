# Modern game trees: asset anatomy, shading, LOD, wind, shadows/RT, budgets, pitfalls, remaster precedents

Web research, 2026-11-16. Scope: trees only, deeper than the earlier overview note (placement, grass, overdraw basics, hashed alpha, "impostors exist", "Nanite Foliage is experimental", wind = hierarchical vertex animation are not repeated).

Target engine assumed throughout: custom Vulkan/Metal, deferred, TAA/TAAU, shadow maps + screen-space rays, optional RT GI on RTX, no virtual geometry, RTX 3080 at 1440p and Apple M1 Pro.

## How to read this note

- Every finding carries a link and the year of the source.
- **[unverified]** = from my own background knowledge, not confirmed by a source fetched in this session.
- **[transcript]** = taken from a machine transcript of the GDC 2018 Horizon Zero Dawn vegetation talk ([cnblogs copy](https://www.cnblogs.com/cainiao001/p/10635721.html), talk: [GDC Vault](https://www.gdcvault.com/play/1025530/Between-Tech-and-Art-The), 2018). The transcript is badly garbled; I only use statements whose meaning is clear, and numbers from it should be checked against the slides before anyone relies on them.
- **[secondary]** = a summary, forum post or press article rather than the developer's own document.
- "Inference" paragraphs are my reasoning for our engine, not sourced facts.

Sources I could not open (so their content is missing or secondary here): the Brucks octahedral impostor blog (page body is script-rendered), GDC Vault slide PDFs (host did not resolve), SpeedTree 9 modeler docs (pages not found), the ACM paper page for the 2025 foliage path tracing paper, Ben Golus's alpha-to-coverage article.

---

## A. Anatomy of a modern game tree asset

### Takeaway

Two asset families ship today. The classic one, still the only option without virtual geometry, is a geometric trunk and main branches plus tightly cut branch-cluster cards from one atlas per tree or species, at roughly 10–20 thousand vertices for the top LOD. The Nanite one models every leaf as opaque geometry at 300–500 thousand vertices per tree (Fortnite, 2023) up to tens of millions (UE 5.x Nanite Foliage demo). For us the classic family is the target, and the useful lesson from the Nanite side is the direction: more triangles and less transparent area per card.

### Findings

**Triangle and vertex counts**

- Fortnite Chapter 4: Nanite trees have "~300-500k" vertices; their non-Nanite counterparts had "~10-20k". Leaves are modelled geometry with an opaque material ([Epic tech blog, Wihlidal, 2023-01-26](https://www.unrealengine.com/tech-blog/bringing-nanite-to-fortnite-battle-royale-in-chapter-4)).
- Epic's reason for dropping cards under Nanite: masked materials cost extra in base-pass shading (barycentrics recomputed per pixel) and "negative space in the alpha maps adds additional overdraw cost" ([same post, 2023](https://www.unrealengine.com/tech-blog/bringing-nanite-to-fortnite-battle-royale-in-chapter-4)). Epic's content guide says the same: geometry instead of masked cards ([Epic docs, "Working with Nanite-enabled content"](https://dev.epicgames.com/documentation/unreal-engine/working-with-naniteenabled-content), read via search summary, 2026).
- UE Nanite Foliage (the pipeline shown with the Witcher 4 demo in 2025): example tree of 41 million triangles, 12 unique part assemblies, 2,160 part instances, 850 bones; assemblies cut its asset from 3.5 GB to about 29 MB and streaming memory for one tree from about 36 MB to 2.7 MB; "500k instances of dozens of tree variants", each 1–10 million polygons. Still marked Experimental in the current docs ([Epic docs, Nanite Foliage, UE 5.9 page, read 2026](https://dev.epicgames.com/documentation/en-us/unreal-engine/nanite-foliage)). The feature was shown in the 2025 demo before developers could use it ([PC Gamer, 2025](https://www.pcgamer.com/hardware/cd-projekt-red-and-epic-claim-witcher-4-development-ramped-up-dramatically-unreal-engines-open-world-game-capabilities), secondary).
- SpeedTree library trees built for real time ("_RT") "generally have 1,000 - 12,000 triangles at the highest LOD, and significantly less at lower LOD" ([SpeedTree SDK-era docs, real-time modeling tips, c. 2012–2017](https://docs.speedtree.com/doku.php?id=real-time_modeling_tips)). This is the PS3/PS4-era baseline.
- Megascans Trees (European Black Alder, 2021): 22 models, seven LOD levels for adult trees and four for saplings, vertex counts from about 100 to about 640 thousand, 8K texture maps, wind either by Pivot Painter or a simpler cheap setup ([CG Channel, 2021](https://www.cgchannel.com/2021/12/download-22-free-megascans-3d-trees-for-unreal-engine/), secondary). European Hornbeam pack: 27 assets = 10 forest, 4 field, 8 saplings, 5 seedlings ([Unreal Marketplace listing via search, 2022](https://www.unrealengine.com/marketplace/en-US/product/megascans-trees-european-hornbeam-early-access)). This is the best public data point on **variants per species**: about 10 adult forest forms + a few field forms + young stages.
- A typical source tree in a vendor's LOD guide is 56 thousand triangles; reduction takes it to 21 thousand (moderate) or 1 thousand / 512 (heavy) before it "breaks apart" ([Microsoft/Simplygon, 2026-01](https://developer.microsoft.com/en-us/games/articles/2026/01/how-to-optimize-vegetation-with-simplygon/)).

**Structure, materials, draw calls**

- SpeedTree: a tree should be 3 draw calls (branches, fronds, leaves), 2 with planning; more than 3 is a "special case". Leaves, fronds and caps share one atlas; bark is separate because it tiles; each extra bark material is one more draw call and is for hero trees only ([SpeedTree docs, c. 2012–2017](https://docs.speedtree.com/doku.php?id=real-time_modeling_tips)).
- CryEngine best practice: about two material IDs per tree (trunk, foliage), aggressive LODs, an X-shaped cross as the last LOD ([CryEngine 3 docs via search summary](https://www.cryengine.com/docs/static/engines/cryengine-3/categories/1114113/pages/1310885), old).
- SpeedTree's own note: facing "leaf cards" are legacy; "SpeedTree has evolved away from using these for most models" towards arbitrarily shaped leaf meshes ([vertex properties doc](https://docs.speedtree.com/doku.php?id=vertex_properties)).
- Horizon Zero Dawn asset flow [transcript]: sketch the lowest-LOD components and texture layout first; build high-resolution branch components to match that layout; bake them to textures in Maya with custom shaders; return to SpeedTree and treat the SpeedTree model as a low LOD ("LOD 3"), adding detail upward; export through an in-house Houdini step that converts SpeedTree's wind data into Guerrilla's vertex-colour layout. A second summary confirms "SpeedTree models are treated as LOD 3" and "billboards in LODs 4 and 5 … cross planes" ([videohighlight summary, secondary](https://videohighlight.com/v/wavnKZNSYqU)).
- Horizon [transcript]: trees are kept more detailed at the bottom, where the player is ([Polycount thread on the talk, secondary](https://polycount.com/discussion/225416/do-people-still-create-trees-like-this)).
- Ghost Recon Wildlands: few species per placement rule, several growth "steps" of one species, for realism and memory ([80.lv interview, 2017](https://80.lv/articles/vegetation-generation-in-ghost-recon-wildlands)).
- Witcher 3 uses SpeedTree with the SpeedTree Forest library for culling and LOD, one shared culling grid for all tree types ([Gollent, GDC 2014, text on archive.org](https://archive.org/stream/GDC2014Gollent/GDC2014-Gollent_djvu.txt)). The talk gives no per-tree triangle counts.

**Atlas authoring**

- Horizon [transcript]: alpha textures were first signed-distance textures (to grow or shrink the cutout by distance); the team later replaced that with coverage-preserving mip scaling done in the texture pipeline (see section B). Small BC-compressed textures matter for mass assets: a 256×128 grass texture is 16 KB without mips, 21 KB with.
- Crysis used a separate low-resolution (128×128) artist-painted subsurface/thickness map for leaves ([GPU Gems 3 ch. 16, Sousa, 2007](https://developer.nvidia.com/gpugems/GPUGems3/gpugems3_ch16.html)).
- **[unverified]** The usual atlas set for a branch-cluster card is: base colour + opacity, tangent-space normal, and one packed map with subsurface/translucency, AO and roughness (sometimes height/depth), rendered orthographically from a high-poly branch; 2K–4K per species for near LODs. I did not find a primary source that states resolutions per LOD.

### Inference for our engine

- Build the classic family: geometric trunk and first two branch orders, branch-cluster cards cut tight (section B), one atlas + one bark material per species, 2 draw calls per tree.
- Budget for the top LOD of a large broadleaf: about 15–30 thousand triangles. This sits above the SpeedTree PS4-era range (1–12 thousand) and at Fortnite's pre-Nanite range (10–20 thousand vertices), and is 200× what the 2002 meshes had. Hero trees near paths can go higher.
- Spend the triangles on tight card outlines and on bending cards across their width (so a card is a shallow bowl, not a plane), not on twigs.
- Variants: follow the Megascans split at reduced count, for example 3–4 adult forest forms + 1–2 open-field forms + 1–2 young forms per species, with instance rotation, scale and tint on top.

### Gaps

- No primary numbers for Witcher 3, Kingdom Come 1/2, RDR2, Ghost of Tsushima, Assassin's Creed, Avowed or Hellblade 2 tree assets (triangles per LOD, atlas sizes). Searches returned settings guides and mod pages only.
- The Horizon Zero Dawn slides themselves (triangle counts per LOD, exact texture sets) were not reachable.

---

## B. Crown shading tricks

### Takeaway

A crown reads as a volume only if three things are faked together: normals that describe the crown rather than each card, light passing through leaves with a thickness mask, and darkening towards the inside from baked occlusion. Cutout quality at distance is a texture-pipeline problem (coverage-preserving mips) before it is a shader problem. Alpha-tested pixels are expensive enough that Guerrilla measured a depth-only prepass as "a huge difference".

### Findings

**Normals**

- Horizon [transcript]: for card geometry the vertex normals are deliberately adjusted ("it's a highly artistic choice because you're breaking lighting"), and the shader takes the absolute value of the view-space normal's z so that no normal ever faces away from the camera. For true geometry (plants with real leaves) they only handle double-sidedness.
- Community practice: transfer smooth normals from a sphere or convex hull proxy onto the cards; cut the sphere in half and stretch it to a cone so the underside does not get downward normals; backfaces remain the weak point and need a two-sided shader ([UE forum tutorial](https://forums.unrealengine.com/t/spherical-normals-for-trees-blender/98732), [Polycount thread](https://polycount.com/discussion/212729/vector-normals-cross-planes-and-the-attempt-to-make-lower-poly-trees-look-good), [Blender "Foliage Normals" add-on description](https://superhivemarket.com/products/foliage-normals); all via search summary, 2018–2024, secondary).
- DynDOLOD's tree LOD has the same trick as a switch: "SphereNormals" recomputes normals from a centre point below the billboard so one side does not go dark ([DynDOLOD docs, 2024–2026](https://dyndolod.info/Help/Ultra-Tree-LOD)).
- SpeedTree stored two normals per vertex because silhouette work needs smooth geometric normals while lighting uses adjusted ones ([GPU Gems 3 ch. 4, 2007](https://developer.nvidia.com/gpugems/gpugems3/part-i-geometry/chapter-4-next-generation-speedtree-rendering)).

**Light through leaves**

- Crysis: back side lit with a wrapped term `saturate(dot(-N, L) * 0.6 + 0.4)` blended with a view-against-light term to the 4th power, multiplied by the thickness map; front side normal diffuse + specular ([GPU Gems 3 ch. 16, 2007](https://developer.nvidia.com/gpugems/GPUGems3/gpugems3_ch16.html)).
- SpeedTree: when a leaf is lit from behind, shift towards a yellower colour (diffuse green × 0.9, 1.0, 0.2) by the angle between light and view, and reduce specular on the back ([GPU Gems 3 ch. 4, 2007](https://developer.nvidia.com/gpugems/gpugems3/part-i-geometry/chapter-4-next-generation-speedtree-rendering)).
- Unreal's Two Sided Foliage shading model exists because default-lit leaf undersides "look almost black"; its Subsurface Colour input sets how much light passes and doubles as a mask for stems and veins ([Epic docs, shading models](https://dev.epicgames.com/documentation/en-us/unreal-engine/shading-models-in-unreal-engine), read 2026).
- Horizon [transcript]: translucency amount comes from a texture; diffusion is locked to one value for all vegetation; a derivative of the translucency texture is reused as precomputed AO; leaves and bark are told apart inside one texture.

**Occlusion and inner darkening**

- Crysis stores precomputed AO in vertex alpha ([GPU Gems 3 ch. 16, 2007](https://developer.nvidia.com/gpugems/GPUGems3/gpugems3_ch16.html)); Horizon does the same [transcript].
- SpeedTree billboards carry an ambient term with "a skylight ambient dimming effect" in the normal map's alpha, and precomputed AO multiplied into the diffuse ([SpeedTree LOD overview](https://docs.speedtree.com/doku.php?id=overview_level-ofdetail)).

**Specular**

- Horizon [transcript]: specular reflectance fixed at 4 % (dielectric); roughness/occlusion derived from vertex alpha with one artist control.
- SpeedTree: reduce leaf specular with distance to limit shimmer, and bias the normal-map fetch to a lower mip ([GPU Gems 3 ch. 4, 2007](https://developer.nvidia.com/gpugems/gpugems3/part-i-geometry/chapter-4-next-generation-speedtree-rendering)).

**Colour variation**

- Horizon [transcript]: every vegetation asset is tinted from a world-space colourisation system: one sample per object (grass samples per vertex), a table of 64 gradient entries packed in a 128-wide texture, indexed by place in the world and by asset type.
- Witcher 3 tints grass instances from a terrain pigment map, less for near instances and more for far ([Gollent, GDC 2014](https://archive.org/stream/GDC2014Gollent/GDC2014-Gollent_djvu.txt)).

**Cutout quality and cost**

- Coverage-preserving mips: measure the fraction of texels passing the alpha test in mip 0, then for each mip find by bisection the scale that restores that fraction, and multiply alpha by it ([Castaño, "Computing Alpha Mipmaps", 2010](https://www.ludicon.com/castano/blog/articles/computing-alpha-mipmaps/)). Horizon describes the same bisection in its texture pipeline [transcript]; DynDOLOD generates LOD mips "using an alpha-to-coverage algorithm" with an `AlphaFactor` setting ([DynDOLOD docs](https://dyndolod.info/Help/3D-Tree-LOD-Model)).
- Why it is not enough alone: three effects erode coverage with distance (lower alpha variance in coarse mips, discrete sampling, sub-pixel geometry); per-mip thresholds help textures like leaves but a thin fence "still quickly disappears" ([Wyman & McGuire, Hashed Alpha Testing, 2017](https://research.nvidia.com/sites/default/files/pubs/2017-02_Hashed-Alpha-Testing/Wyman2017Hashed.pdf)).
- Hashed alpha details relevant to trees: hash object-space coordinates of the **whole tree**, not of each leaf, or stability is lost when a coordinate frame goes sub-pixel; include z to decorrelate layers; with temporal AA use a noise scale of 0.3–0.5 pixel so TAA can average; measured extra cost 0.1–0.3 ms per frame at 1920×1080 on their scenes ([same paper, 2017](https://research.nvidia.com/sites/default/files/pubs/2017-02_Hashed-Alpha-Testing/Wyman2017Hashed.pdf)).
- Depth prepass, Horizon [transcript]: alpha test runs only in an initial depth pass; the colour pass then renders with a depth-equal compare and no alpha test, "because alpha test is incredibly expensive". In their profiled frame the depth prepass was about 10 % of the frame and the geometry pass about 11.5 %; about 95 % of placed assets were alpha tested.
- Horizon Forbidden West went further for the same reason (denser foliage, quad overdraw from small triangles): visibility buffer prepass, then shading in compute ([McLaren, GDC 2022 slides, via search summary](https://media.gdcvault.com/GDC+2022/Speaker+Slides/AdventuresWithDeferred_McLaren_James.pdf)).
- Tight card cutting: on a sample sprite, trimming to the bounding rectangle leaves 69.2 % of the quad's area, an optimised 4-vertex polygon 60.2 %, 6 vertices 53.9 %, 8 vertices 51.9 % ([Persson, "Particle trimmer", 2009](https://www.humus.name/index.php?ID=266); also in [Graphics Gems for Games, SIGGRAPH 2012](https://www.humus.name/Articles/Persson_GraphicsGemsForGames.pdf)). SpeedTree's advice is the same: cut transparent pixels out of frond and leaf meshes, enable backface culling on bark, and check with an overdraw heat map ([SpeedTree docs](https://docs.speedtree.com/doku.php?id=real-time_modeling_tips)). A convex hull of 5 vertices around an impostor removed about 30 % of its pixels ([three.js forum, 2025](https://discourse.threejs.org/t/a-forest-of-octahedral-impostors/85735), secondary).
- Alpha to coverage: SpeedTree used it for leaf edges and for the 3D-to-billboard fade ([GPU Gems 3 ch. 4, 2007](https://developer.nvidia.com/gpugems/gpugems3/part-i-geometry/chapter-4-next-generation-speedtree-rendering); [SpeedTree LOD overview](https://docs.speedtree.com/doku.php?id=overview_level-ofdetail)). It needs MSAA, so it does not fit a deferred TAA renderer without MSAA targets (**[unverified]** as a general statement; true for our engine as described).

### Inference for our engine

- Order of work: (1) coverage-preserving mips in the texture bake; (2) cards cut to 6–8 outline vertices; (3) alpha test only in the depth prepass, depth-equal colour pass; (4) hashed alpha only for LOD cross-fades, with the hash anchored to tree object space and a 0.3–0.5 px noise scale.
- Shading set: hull-transferred normals blended with the card normal map, view-facing fix (abs of view-space z or equivalent), back-lit wrapped transmission with a thickness channel, vertex AO for inner darkening, specular held near 4 % and faded with distance.
- A per-instance tint from a low-resolution world colour map solves variant repetition and seats trees in their biome with one texture fetch per object.

### Gaps

- No first-hand source on how Frostbite or Decima weight hull normals against card normals.
- No measured numbers for depth prepass gain on Apple GPUs (see G).

---

## C. LOD chain

### Takeaway

Shipped chains have 3–4 geometric levels and then an image-based level. Geometric levels are made by removing a share of the cards and **enlarging the survivors** so crown area holds, plus edge-collapse on bark; this is documented in the same form by SpeedTree, Epic ("Preserve Area") and a 2026 Skyrim tool with explicit percentages. The image level today is an octahedral impostor with normals and depth; for the far forest, games fall back to cross planes, merged meshes or terrain-level tricks.

### Findings

**How geometric LODs are made**

- SpeedTree: "fewer and bigger leaf instances" with distance — a share of leaf instances shrinks to nothing while the rest scale up, driven by a per-vertex LOD scalar; small outer branches are scaled down to their centre line; each vertex carries a position and a lower-LOD position that the shader lerps; alpha "fizzling" was dropped in version 5 ([SpeedTree LOD overview](https://docs.speedtree.com/doku.php?id=overview_level-ofdetail); [vertex properties](https://docs.speedtree.com/doku.php?id=vertex_properties)).
- Epic: Nanite simplification made canopies "thin out, or … abruptly go bare"; "Preserve Area" redistributes lost area by dilating open boundary edges, i.e. "the remaining leaves getting bigger" ([Epic tech blog, 2023](https://www.unrealengine.com/tech-blog/bringing-nanite-to-fortnite-battle-royale-in-chapter-4)).
- TreeLODGen (Skyrim, 2026) default levels — cards kept : leaf area restored : bark triangles : leaf-triangle floor: LOD4 40 % : 80 % : 400 : 2500; LOD8 15 % : 70 % : 150 : 1000; LOD16 6 % : 60 % : 60 : 400. Kept cards are spread evenly through the volume and may grow up to 2× at far levels "so the crown stays airy rather than becoming a few solid cards"; bark is reduced by quadric edge collapse keeping UVs. A typical pine comes out at 1740 + 400, 644 + 150 and 262 + 60 triangles ([TreeLODGen README, 2026, GPL-3.0](https://github.com/Cl3mus33/TreeLODGen)).
- Simplygon: plain triangle reduction for near LODs; do not remesh vegetation (it bloats) except very dense hedges or very distant HLOD; for far levels use a billboard cloud with the trunk separated and reduced as triangles, optionally "fading orthogonal planes and noise normals"; flipbook or single-view impostor as the cheapest step ([Microsoft/Simplygon, 2026](https://developer.microsoft.com/en-us/games/articles/2026/01/how-to-optimize-vegetation-with-simplygon/)).
- Horizon [transcript]: "what can we do to improve the performance and quality of our LODs without having to resort to software solutions to reduce triangles" — LODs are built by hand from purpose-made low components; small triangles were added around meshes so every LOD has a similar bounding box.

**Transitions**

- SpeedTree: smooth vertex lerp inside the 3D range; 3D to billboard is a cross-fade (3D alpha fades out while billboard fades in), smooth with alpha to coverage ([LOD overview](https://docs.speedtree.com/doku.php?id=overview_level-ofdetail)). The 2007 chapter admits the cross-fade can leave the tree "more transparent than desired mid-transition" ([GPU Gems 3 ch. 4](https://developer.nvidia.com/gpugems/gpugems3/part-i-geometry/chapter-4-next-generation-speedtree-rendering)).
- Ghost Recon Wildlands: LOD distances are not fixed per asset; denser areas switch sooner to the last LOD, the impostor ([80.lv, 2017](https://80.lv/articles/vegetation-generation-in-ghost-recon-wildlands)).
- Wind LOD: go to global-only or no wind "as soon as possible"; full wind at the top LOD and global sway from the next "works very well in terms of performance and perception" ([SpeedTree performance doc](https://docs.speedtree.com/doku.php?id=performance)).

**Billboards and impostors**

- SpeedTree billboards: an array of views chosen by azimuth plus one overhead (horizontal) billboard cross-faded by camera pitch and also used "to cast a better shadow when the light is more directly overhead"; maps = unlit diffuse × precomputed AO, alpha mask, tangent-space normal of the whole tree, ambient term ([LOD overview](https://docs.speedtree.com/doku.php?id=overview_level-ofdetail)).
- Ghost Recon Wildlands impostors are generated in-engine to match the 3D model and include channels for light direction, fake shadowing and translucency ([80.lv, 2017](https://80.lv/articles/vegetation-generation-in-ghost-recon-wildlands)).
- Brucks octahedral impostors (2018, Epic), as reported by people re-implementing the article: views on an octahedral or hemi-octahedral lattice baked to an atlas; blend the **three** nearest frames (the lattice is triangulated), weights solved in the vertex shader; one depth-based parallax offset shared across the three frames; later versions bake albedo, normal, metallic/gloss and position instead of pre-lit colour; at about 8×8 frames cards clip at grazing angles; used in Fortnite Battle Royale on PC and consoles, while mobile went back to single-frame impostors because HLODs drew very many of them ([yummers.dev write-up, 2026-01](https://www.yummers.dev/hemi-octahedral-impostors.html), secondary; original: [shaderbits.com, 2018-03-18](https://www.shaderbits.com/blog/octahedral-impostors), body not retrievable).
- The Unreal tool has three modes: full sphere, upper hemisphere, traditional billboards ([80.lv on ImpostorBaker](https://80.lv/articles/impostor-baker-for-ue4/), via search summary; repo: [ictusbrucks/ImpostorBaker](https://github.com/ictusbrucks/ImpostorBaker), licence not shown on the page I read).
- Hemi-octahedron puts all frames on the upper hemisphere, doubling angular density for the same atlas, and is wrong when seen from below ([Unity Pixyz docs](https://docs.unity3d.com/Packages/com.unity.pixyz.plugin4unity@2.0/manual/create-impostors.html) and [Amplify Impostors manual](https://wiki.amplify.pt/index.php?title=Unity_Products%3AAmplify_Impostors%2FManual), via search summary).
- Working parameter sets from open implementations: 16×16 frames, 2048 atlas, hemi, parallax scale 0.5, alpha clamp 0.45, switch to impostor at 100 units, about 200 thousand trees in the demo ([three.js forum, agargaro, 2025](https://discourse.threejs.org/t/a-forest-of-octahedral-impostors/85735)); 12×12 frames of 128 px, with the advice to use a texture array instead of an atlas (no bleeding between frames, free frame count), capture with an orthographic camera fitted to the bounding sphere, write per-pixel depth so the impostor receives and casts shadows and takes SSAO, and draw a low-vertex convex hull instead of a quad ([arycama blog, 2024](https://arycama.github.io/2024/10/05/Imposters.html)). SideFX's example sizing: 8×8 frames × 128 px = 1024 atlas ([Houdini Labs impostor docs](https://www.sidefx.com/docs/houdini/nodes/out/labs--impostor_texture.html)).
- **[unverified]** Brucks's own defaults, as I remember them: 16×16 frames in a 2048–4096 atlas for trees, hemi-octahedron for ground objects. Not confirmed this session.

**Open-source bakers and licences**

| Project | Target | Licence as found | Note |
|---|---|---|---|
| [agargaro/octahedral-impostor](https://github.com/agargaro/octahedral-impostor) | three.js | MIT (search summary) | full + hemi, 3-frame blend, marked "wip" |
| [ektogamat/octahedral-impostor-component](https://github.com/ektogamat/octahedral-impostor-component) | WebGPU demo | MIT (search summary) | includes the bake step |
| [wojtekpil/Godot-Octahedral-Impostors](https://github.com/wojtekpil/Godot-Octahedral-Impostors) | Godot 3 | GitHub says MIT, Godot asset library says AGPLv3 — conflicting, read the LICENSE file | |
| [zhangjt93/godot-imposter](https://github.com/zhangjt93/godot-imposter) | Godot 4 | MIT (search summary) | |
| [Pandrodor/Blender-Impostor-Baker](https://relatedrepos.com/gh/Pandrodor/Blender-Impostor-Baker) | Blender | none shown; last update 2022 | octahedral support not confirmed |
| "Imposter Cards", "Instant Impostors" | Blender, paid | GPL | single-plane or Eevee-only, not octahedral |
| [ictusbrucks/ImpostorBaker](https://github.com/ictusbrucks/ImpostorBaker) | UE4 | not confirmed | reference implementation |

All licence entries come from search summaries of the repository pages; none was checked against the LICENSE file itself.

**Mid and far distance alternatives**

- Billboard cloud with separated trunk ([Simplygon, 2026](https://developer.microsoft.com/en-us/games/articles/2026/01/how-to-optimize-vegetation-with-simplygon/)); X-cross as last LOD ([CryEngine docs](https://www.cryengine.com/docs/static/engines/cryengine-3/categories/1114113/pages/1310885)); hybrid of 3D crown + billboard trunk (section H).
- Nanite's answer is voxels: clusters turn into up to 128 bricks of 4×4×4 voxels when that gives lower error, with a per-voxel normal distribution sampled stochastically ([Epic docs, Nanite Foliage](https://dev.epicgames.com/documentation/en-us/unreal-engine/nanite-foliage)). Not available to us, but it confirms that far crowns are best treated as volume, not surface.
- Horizon Forbidden West reportedly built distant-geometry LODs by voxelising and re-surfacing ([gamedesignskills article](https://gamedesignskills.com/game-design/games-with-best-environment-design/), secondary, weak).
- OpenMW's object paging merges nearby distant objects into one mesh, with a minimum-size cutoff (default 0.01; guides suggest 0.023); its LOD support had mismatch problems where a mod's LOD no longer matched the base mesh ([OpenMW docs](https://openmw.readthedocs.io/en/openmw-0.42.0/reference/modding/settings/terrain.html), [issue 6447](https://gitlab.com/OpenMW/openmw/-/issues/6447), via search summary).

### Inference for our engine

A chain that every source above supports, with my numbers marked as proposals:

| Level | Content | Triangles (proposal) | Made by |
|---|---|---|---|
| LOD0 | trunk + branches + all cards, full wind | 15–30 k | hand/Blender |
| LOD1 | 40–50 % of cards, restored to ~80–90 % area; bark −50 % | 6–12 k | script, SpeedTree/TreeLODGen rule |
| LOD2 | 15–20 % of cards, ~70 % area; bark ~300–600; branch wind only | 2–4 k | same script |
| LOD3 | 5–8 % of cards (up to 2× size), bark ~60–150; trunk sway only; also the **shadow mesh** for near cascades | 0.4–1.2 k | same script |
| Impostor | hemi-octahedral, 8×8 to 12×12 frames × 128–256 px, albedo+alpha and normal+depth, convex-hull card, per-pixel depth | 4–8 | Blender bake (own script) |
| Far forest | merged per-cell canopy mesh or terrain-colour/shadow term | — | offline |

- Screen-size thresholds are not given by any source I could read; choose them by measured pixel error in our own captures. Starting proposal: impostor when the tree is under about 80–100 px tall at 1440p, LOD3 under about 200 px.
- Cross-fade every switch with hashed dithering over a short band; a hard pop is what players report first (section G).
- The far end of the Blender impostor bake can be written in-house quickly: the open implementations are small and the layout is documented; this avoids the licence uncertainty above.

### Gaps

- Brucks's first-hand parameters and ray-march details; screen-size thresholds from any shipped game; a confirmed-licence Blender octahedral baker.
- No primary source on "canopy heightfield" far forests; I found the idea only as Horizon's height-field shadow for the distance cascade (section E).

---

## D. Wind for trees

### Takeaway

Three data layouts are proven. Crysis/Horizon keep everything in vertex colours and derive the trunk bend from height. SpeedTree stores two branch levels per vertex (weight + packed direction each) plus optional leaf data. Unreal stores per-branch pivots in small textures indexed by one extra UV (Pivot Painter 2), and Fortnite went one step further to a baked simulation texture read once per vertex. All of them make trunk sway nearly free and make cost scale with how many levels are enabled.

### Findings

**Per-vertex data**

- Crysis: vertex colour R = leaf-edge stiffness, G = per-leaf phase, B = overall leaf stiffness, A = precomputed AO. Main bending uses only position and height. Detail bending moves edges along the normal (weighted by R) and leaves up/down (weighted by B) with four vectorised triangle waves; quoted frequencies 1.975, 0.793, 0.375, 0.193 ([GPU Gems 3 ch. 16, 2007](https://developer.nvidia.com/gpugems/GPUGems3/gpugems3_ch16.html)).
- Horizon [transcript]: "similar approaches as described in GPU Gems 3", data in vertex colours: height as the gradient for trunk motion; a channel storing distance from the trunk for branch rigidity; green as a per-leaf index/phase; AO in alpha. Per-asset shader controls: bend (along the wind), sway (across it), and a leaf term scaled by wind strength. Small plants compile the trunk bend out. Wind itself is a global force field sampled from 3D textures in cascades around the player.
- SpeedTree: `WIND_BRANCH_DATA` = (weight level 1, packed direction level 1, weight level 2, packed direction level 2), shared by all geometry; `WIND_EXTRA_DATA` (3 values) = ripple direction/scalar/length for fronds or wind scalar/growth direction/ripple direction for leaves; `LEAF_ANCHOR_POINT` (3 values) = pivot for leaf meshes; a wind-flags value selects the leaf algorithm ([vertex properties doc](https://docs.speedtree.com/doku.php?id=vertex_properties)).
- Pivot Painter 2: textures hold pivot position (16-bit RGB) + parent index (alpha), and X-axis vector (8-bit RGB) + extent (alpha); nearest filtering, no mips; one extra UV channel; the foliage function supports hierarchies "up to 4 levels deep and 30,000 model elements" and runs the wind code once per level ([Epic docs, Pivot Painter Tool 2.0](https://dev.epicgames.com/documentation/en-us/unreal-engine/pivot-painter-tool-2.0-in-unreal-engine)).
- Fortnite: a Houdini Vellum simulation of a skeleton built from branch pivots is baked to an image, one row per branch; a custom UV picks the row; the shader reads "a single position and quaternion"; rigid per branch only; wind evaluation is switched off beyond an artist-set distance ([Epic tech blog, 2023](https://www.unrealengine.com/tech-blog/bringing-nanite-to-fortnite-battle-royale-in-chapter-4)).
- GPU Gems 3 ch. 6 (hierarchical rigid branches): per vertex a list of branch indices up to the root and weights; per branch origin, axis, tangent, stiffness, parent index; hierarchy 2–3 levels deep "is usually enough"; quaternions concatenated down the chain; test tree 4,617 vertices; distant trees use trunk-only simulation and the switch can pop ([GPU Gems 3 ch. 6, 2007](https://developer.nvidia.com/gpugems/gpugems3/part-i-geometry/chapter-6-gpu-generated-procedural-wind-animations-trees)).
- Nanite Foliage replaces all of this with skinning: bounds come from bone matrices, 100,000 bones cost about 0.1 ms GPU, animation stops below a screen size of 0.1 by default ([Epic docs](https://dev.epicgames.com/documentation/en-us/unreal-engine/nanite-foliage)).

**Trunk bend that keeps length**

- Crysis main bending: `fBF = vPos.z * fBendScale; vNewPos.xy += vWind.xy * fBF; vPos.xyz = normalize(vNewPos.xyz) * fLength;` — displace in xy by height, then rescale to the original distance from the pivot so the motion is spherically limited ([GPU Gems 3 ch. 16](https://developer.nvidia.com/gpugems/GPUGems3/gpugems3_ch16.html)).
- SpeedTree "Global Motion → Preserve shape": corrects the skew of plain side-to-side motion "at the expense of a normalize() and a length() call"; global motion "requires no extra per-vertex data" ([performance doc](https://docs.speedtree.com/doku.php?id=performance)).

**Cost**

- SpeedTree full wind can be "400+ instruction vertex shaders"; leaf tumble is "the most expensive wind effect by far" (two matrix constructions) and is for hero trees; "Computed" ripple needs a packed direction per vertex, "Vertex normal" ripple needs none ([performance doc](https://docs.speedtree.com/doku.php?id=performance)).
- Pivot Painter: normal rotation is the expensive part; a cheaper approximate normal blend exists; disable unused levels ([Epic docs](https://dev.epicgames.com/documentation/en-us/unreal-engine/pivot-painter-tool-2.0-in-unreal-engine)).

**Shadow passes and motion vectors**

- Horizon [transcript]: shadow casters are separate meshes and are **not animated** — "as cheap as you can possibly get".
- Epic: with Virtual Shadow Maps "animation must be disabled at distance" for performance ([Nanite Foliage docs](https://dev.epicgames.com/documentation/en-us/unreal-engine/nanite-foliage)).
- Indiana Jones (ray tracing): animated vegetation BLASes are refitted, never rebuilt, which works because "vegetation returns to its initial position" ([NVIDIA blog, 2025-05-15](https://developer.nvidia.com/blog/path-tracing-optimizations-in-indiana-jones-opacity-micromaps-and-compaction-of-dynamic-blass)).
- Motion vectors: if the velocity pass does not run the same deformation with last frame's wind state, TAA treats leaves as static and smears them; instanced draw paths are a common place where the vectors go missing ([Unity discussion](https://discussions.unity.com/t/terrain-detail-mesh-trees-missing-motion-vectors-leading-to-taa-issues/858533), [UE forum](https://forums.unrealengine.com/t/ghosting-in-moving-foliage-in-unreal-engine-5/1172596), via search summary, 2021–2024, secondary).

### Inference for our engine — layout options

| Option | Per-vertex cost | Gives | Fits |
|---|---|---|---|
| 1. Crysis/Horizon | RGBA8 colour only (stiffness, phase, leaf weight, AO) | trunk bend from height + leaf flutter; no true branch swing | LOD2+, bushes, anything small |
| 2. SpeedTree-like | 4 floats (two levels: weight + packed direction), optional 3-float anchor for hero leaves | trunk + two branch levels, directional | LOD0–1 of all trees; computed fully in Blender at export |
| 3. Pivot texture | 1 UV (branch index) + small per-species texture or buffer (pivot, axis, parent, stiffness) | true hierarchical rotation up to 4 levels | hero trees; costs dependent reads in every pass |
| 4. Baked simulation | 1 UV + animation texture | richest motion, cheapest shader | later; needs a simulation step and cannot follow gusts locally |

- Recommended: option 2 on LOD0–1 with AO and phase in vertex colour, falling to option 1's trunk-only path on LOD2–3 and none on impostors; the same wind function with a previous-frame time/wind input in the velocity pass; near-cascade shadow meshes get trunk + level-1 sway only, far cascades none.
- Use the length-preserving normalise in the trunk bend from day one; tall thin trees show the skew first.

### Gaps

- SpeedTree 8/9 "games wind" exact packing was not retrievable (old SDK 7 layout used above).
- No measured ms for any wind scheme on current hardware apart from Epic's bone figure.

---

## E. Shadows and lighting of forests, including ray tracing

### Takeaway

Without virtual geometry, shipped engines give trees real shadow geometry only in the near cascades, with dedicated simplified and unanimated shadow meshes and LOD fixed per cascade; beyond that they switch to a cheaper representation. In ray tracing, alpha-tested foliage is the classic cost centre: games either leave alpha out of the BVH path, use simplified untextured proxies, or use opacity micromaps, which in the best-documented case cut the main trace pass by 55 %.

### Findings

**Raster shadows**

- Horizon [transcript]: four cascades; vegetation renders only into cascades 0 and 1 (standard shadow maps); the distance cascade uses a height-field-based shadow ([second summary](https://videohighlight.com/v/wavnKZNSYqU), secondary). Shadow LOD chosen by camera distance popped visibly because shadows cannot cross-fade; the fix was one fixed shadow LOD per cascade, "as if" the object were at a set distance — more render cost, popping "almost completely" solved. Shadow casters are separate, heavily reduced, unanimated meshes; in the profiled frame shadows cost almost 9 % before this work and a few percent less after.
- SpeedTree: cascades updated at different rates — every frame (all objects), every 2 frames (no fronds), every 4 frames (leaves only); leaf cards shifted towards the light by about half their height when rendered to the shadow map; a per-leaf offset along the view direction in the lookup to hide streaks from planar cards ([GPU Gems 3 ch. 4, 2007](https://developer.nvidia.com/gpugems/gpugems3/part-i-geometry/chapter-4-next-generation-speedtree-rendering)).
- SpeedTree: the overhead billboard is used to cast a better shadow when the light is high ([LOD overview](https://docs.speedtree.com/doku.php?id=overview_level-ofdetail)).
- Impostors can write per-pixel depth from the baked depth so they receive/cast shadows and take SSAO ([arycama, 2024](https://arycama.github.io/2024/10/05/Imposters.html)).
- Ghost Recon Wildlands impostors carry baked channels for fake shadowing and translucency by light direction ([80.lv, 2017](https://80.lv/articles/vegetation-generation-in-ghost-recon-wildlands)).

**Ray tracing**

- Indiana Jones and the Great Circle: the non-path-traced mode keeps ray-traced alpha testing **disabled** "for performance reasons". In path tracing, opacity micromaps (via `VK_EXT_opacity_micromap`) cut TraceMain from 7.90 to 3.58 ms (−55 %), the radiance-cache update from 1.99 to 0.89 ms, sun tracing from 0.83 to 0.71 ms, on an RTX 5080, 4K output with path tracing at 1080p, scene of 26.9 million triangles. Settings: 4-state format with unknown promoted to opaque, max subdivision 6, CPU bake in the cooker, any single micromap over 0.5 MB skipped, 128 MB budget per map, 95 % of micromaps under 200 kB. Dynamic vegetation BLASes use fast-build + update + compaction: 1027 → 606 MB ([NVIDIA developer blog, 2025-05-15](https://developer.nvidia.com/blog/path-tracing-optimizations-in-indiana-jones-opacity-micromaps-and-compaction-of-dynamic-blass)).
- Alan Wake 2: Remedy showed ray tracing cost reduced by one third with micromaps plus shader execution reordering ([DirectX blog, 2025-05-30](https://devblogs.microsoft.com/directx/omm/), figure quoted from its video via search summary).
- Hardware: micromaps are accelerated on all GeForce RTX GPUs; AMD "planned on future hardware"; Intel evaluating ([DirectX blog, 2025](https://devblogs.microsoft.com/directx/omm/)).
- Mobile measurement on a foliage courtyard: +5.9 % to +31.0 % frame rate with micromaps across seven views; non-opaque triangle hits fell from 8.6 million to 0.01 million in the main foliage view ([Arm blog, 2026-10](https://developer.arm.com/community/arm-community-blogs/b/mobile-graphics-and-gaming-blog/posts/reducing-ray-traced-shadow-costs-for-foliage-with-omm-moku-results-on-arm-mali-g2)).
- Older vendor figures: 1.5× on a leaf-cluster test ([wccftech on NVIDIA's 2022 demo](https://wccftech.com/nvidia-shows-ada-lovelaces-opacity-micro-maps-feature-and-how-it-can-boost-ray-tracing-performance/), secondary); "10% or higher" in scenes with many cutouts ([RTX Remix docs](https://www.github.com/NVIDIAGameWorks/dxvk-remix/blob/main/documentation/OpacityMicromap.md), via search summary).
- Avatar: Frontiers of Pandora: ray tracing proxies are a lower LOD than the raster meshes, one material per mesh, no textures; alpha-tested foliage is handled by dithering from an alpha value; terrain is ray-marched separately ([Kuenlin, "Raytracing in Snowdrop", GDC 2024 slides](https://media.gdcvault.com/gdc2024/Slides/GDC+slide+presentations/Kuenlib_Quentin_Raytracing_In_Snowdrop.pdf), via search summary — I could not open the PDF).
- Research direction: partitioned TLAS + clustered LOD + micromaps for "tens of millions of plants" with per-plant animation ([van Antwerpen et al., 2025, DOI 10.1145/3820021](https://dl.acm.org/doi/10.1145/3820021), abstract via index); a press article quotes 60 million animated plants over 25 km² at 60 fps on an RTX 4090 ([Jon Peddie Research](https://www.jonpeddie.com/news/amd-nvidia-rework-foliage-rendering/), secondary).
- Witcher 3 next-gen and Cyberpunk 2077: no developer statement found on how foliage enters the BVH. A mod exposes an "RT Shadows: Foliage Detail" control for Witcher 3, which only shows that foliage shadow tracing is a tunable cost ([Nexus mod page](https://www.nexusmods.com/witcher3/mods/13075), weak).

**Forest darkening beyond real shadows**

- Sourced examples are few: Horizon's height-field shadow for the far cascade; SpeedTree's baked AO/ambient dimming in billboards; Wildlands' baked fake shadowing in impostors (all cited above).
- **[unverified]** Common practice in open-world engines is a top-down baked occlusion or sky-visibility map (from tree placement) that darkens ambient and direct light under canopy beyond the shadow range, and a terrain-shadow or "far shadow map" for long-range sun shadows. I did not retrieve a primary source naming a game for this.

### Inference for our engine

- Raster: dedicated shadow meshes (LOD3-level, tight cards, no leaf wind), drawn into the near two cascades with a fixed LOD per cascade; staggered cascade updates are safe for trees because they barely move. Far cascade: impostor depth or a baked canopy height field treated as terrain for the sun.
- Alpha in the shadow pass should use the same coverage-preserving mips; a slightly lower cutoff in the shadow pass keeps crowns from letting too much light through at low shadow-map resolution (inference).
- RT GI on RTX: do not run any-hit on leaves. Options in order of cost: (a) opaque low-LOD proxy crowns (LOD3 cards treated as opaque, or a few hull shells) for GI rays only; (b) 2-state micromaps on LOD2 cards, forced opaque/transparent, no any-hit; the 3080 supports micromaps per Microsoft's statement. Static BLAS per species instanced in the TLAS, no wind in the BVH for GI (low-frequency lighting hides it); refit only if RT shadows are ever added.
- A baked top-down canopy occlusion map gives the 1–3 % under-beech light level outside the shadow range and also feeds GI fallback on Mac.

### Gaps

- No first-hand description of far tree shadows in Far Cry 5, Assassin's Creed, RDR2 or Witcher 3.
- Micromap gains for GI-only (diffuse) rays on a 3080 are not published; all figures above are for path tracing or shadows.

---

## F. Budgets from shipped games and measurements

### Takeaway

Hard numbers are rare. The usable ones are Horizon's shares of a PS4 frame, Epic's per-tree counts, and the overheads of single techniques.

### Findings

- Horizon Zero Dawn, profiled frame [transcript]: depth prepass about 10 % of the frame, geometry pass about 11.5 %, shadows almost 9 % (a few percent less after shadow-mesh work); about 95 % of what the placement system put on screen was alpha tested. At 30 fps that is roughly 3.3 ms + 3.8 ms + 3 ms on a base PS4 (my arithmetic).
- Fortnite: 10–20 thousand vertices per tree before Nanite, 300–500 thousand after ([Epic, 2023](https://www.unrealengine.com/tech-blog/bringing-nanite-to-fortnite-battle-royale-in-chapter-4)).
- UE Nanite Foliage: 500 thousand tree instances in the demo world; 100,000 wind bones ≈ 0.1 ms ([Epic docs](https://dev.epicgames.com/documentation/en-us/unreal-engine/nanite-foliage)).
- Ghost of Tsushima: about three million trees in the world ([GameRant citing a Eurogamer interview](https://gamerant.com/ghost-of-tsushima-tree-count/), secondary); "tens of thousands of leaves on screen" as particles, with per-tree particle effects limited to close range for cost ([PlayStation Blog, 2021](https://blog.playstation.com/2021/01/12/how-stunning-visual-effects-bring-ghost-of-tsushima-to-life/)).
- RDR2: trees switch from 3D to billboard impostors at roughly 500 m by default, according to a mod that moves the range to 550–920 m ([Nexus mod page](https://www.nexusmods.com/reddeadredemption2/mods/11321), community claim, unconfirmed).
- Skyrim with DynDOLOD: full models in loaded cells, 3D tree LOD in LOD4, billboards (4 or 8 triangles each) in LOD8/16 ([DynDOLOD docs](https://dyndolod.info/Help/Ultra-Tree-LOD)); putting 3D LOD into LOD8/16 grew object LOD meshes from about 5 GB to over 8 GB and stopped the game loading in one test ([TreeLODGen README, 2026](https://github.com/Cl3mus33/TreeLODGen)).
- Hashed alpha: +0.1 to 0.3 ms per frame at 1080p ([Wyman & McGuire, 2017](https://research.nvidia.com/sites/default/files/pubs/2017-02_Hashed-Alpha-Testing/Wyman2017Hashed.pdf)).
- Path-traced jungle: 26.9 million triangles in the acceleration structure; BLAS memory 606 MB after compaction ([NVIDIA, 2025](https://developer.nvidia.com/blog/path-tracing-optimizations-in-indiana-jones-opacity-micromaps-and-compaction-of-dynamic-blass)).
- Open demo: about 200 thousand trees with impostors from 100 units ([three.js forum, 2025](https://discourse.threejs.org/t/a-forest-of-octahedral-impostors/85735)).

### Inference for our engine

- A workable first budget at 1440p on a 3080, to be replaced by our own measurements: 100–200 trees at LOD0–1, 500–1500 at LOD2–3, the rest impostors; 3–6 million tree triangles in the main view and under 1 million in shadow passes; foliage prepass + G-buffer for trees within about 3 ms. These are proposals scaled from the Horizon shares, not sourced values.

### Gaps

- Trees visible per frame, triangles per frame and foliage milliseconds for KCD2, RDR2, Tsushima, Assassin's Creed and Avatar are not published in anything I could read.

---

## G. Pitfalls specific to trees

### Takeaway

Most visible failures come from mismatches between two representations of the same tree (LOD vs LOD, mesh vs impostor, visual vs shadow, current vs previous frame) and from alpha coverage changing with distance.

### Findings

- **Popping.** Shadow LOD switches cannot be cross-faded and are noticed first; fixed LOD per cascade solves it (Horizon [transcript]). Impostor frame snapping "pops pretty awfully" without 3-frame blending and still pops at about 8×8 frames ([yummers.dev, 2026](https://www.yummers.dev/hemi-octahedral-impostors.html)). Wind LOD switches pop too ([GPU Gems 3 ch. 6](https://developer.nvidia.com/gpugems/gpugems3/part-i-geometry/chapter-6-gpu-generated-procedural-wind-animations-trees)).
- **Impostor lighting mismatch.** Billboards whose normals all point up light equally from every side; one-sided normals make one side black; DynDOLOD needs separate brightness controls for billboards, crowns and trunks (`CrownBrightness`, `TrunkBrightness`, `FlatTrunkBrightness`) to match full models, and vertex colours clamp at 1.0 so LOD can be darkened but not brightened much ([DynDOLOD docs](https://dyndolod.info/Help/Ultra-Tree-LOD)). Players still report trees changing colour at the switch ([Step forum threads](https://stepmodifications.org/forum/topic/14299-3d-trees-changing-the-color-of-evt-for-sse), via search summary).
- **Crown thinning or thickening with distance.** Alpha coverage drops in coarse mips ([Castaño, 2010](https://www.ludicon.com/castano/blog/articles/computing-alpha-mipmaps/); [Wyman, 2017](https://research.nvidia.com/sites/default/files/pubs/2017-02_Hashed-Alpha-Testing/Wyman2017Hashed.pdf)); simplification removes leaves until canopies go bare ([Epic, 2023](https://www.unrealengine.com/tech-blog/bringing-nanite-to-fortnite-battle-royale-in-chapter-4)); LOD with a fixed alpha threshold of 128 looks "too thick or thin" unless the threshold is tuned per LOD model ([DynDOLOD docs](https://dyndolod.info/Help/3D-Tree-LOD-Model)). The opposite failure — far crowns becoming "a few solid cards" — is why TreeLODGen restores only 60–80 % of the area ([TreeLODGen, 2026](https://github.com/Cl3mus33/TreeLODGen)).
- **Mid-transition transparency.** Two cross-fading representations are both partly transparent in the middle of the fade ([GPU Gems 3 ch. 4](https://developer.nvidia.com/gpugems/gpugems3/part-i-geometry/chapter-4-next-generation-speedtree-rendering)).
- **TAA ghosting in wind.** Missing or wrong motion vectors for vertex-animated instanced foliage ([Unity discussion](https://discussions.unity.com/t/terrain-detail-mesh-trees-missing-motion-vectors-leading-to-taa-issues/858533), [UE forum](https://forums.unrealengine.com/t/ghosting-in-moving-foliage-in-unreal-engine-5/1172596), secondary). Hashed noise must be anchored to the object and sized for TAA ([Wyman, 2017](https://research.nvidia.com/sites/default/files/pubs/2017-02_Hashed-Alpha-Testing/Wyman2017Hashed.pdf)). Oblivion Remastered players report shimmering trees when resolution scale is reduced or AA is off ([Steam threads](https://steamcommunity.com/app/2623190/discussions/0/604154245960198363), via search summary) — crowns that only hold together under temporal accumulation break under TAAU at low input resolution.
- **Shadow artefacts on cards.** Planar cards cast streaks and self-shadow badly; SpeedTree offset cards towards the light and offset lookups ([GPU Gems 3 ch. 4](https://developer.nvidia.com/gpugems/gpugems3/part-i-geometry/chapter-4-next-generation-speedtree-rendering)).
- **Trunk meets ground.** Far Cry 5's terrain talk lists as a goal that rocks, trees and grass "blend with, and be grounded in, the terrain", using terrain data on the GPU for those assets ([Far Cry 5 terrain slides, GDC 2018](https://media.gdcvault.com/gdc2018/presentations/TerrainRenderingFarCry5.pdf), via search summary). Witcher 3 names the same problem for grass ("doesn't sit on the terrain") and solves it by colour from the terrain pigment map ([Gollent, 2014](https://archive.org/stream/GDC2014Gollent/GDC2014-Gollent_djvu.txt)).
- **Repetition.** Addressed by growth stages of one species and few species per rule ([Wildlands, 2017](https://80.lv/articles/vegetation-generation-in-ghost-recon-wildlands)), world-space tint (Horizon [transcript]), and 10+ forms per species in scanned packs ([Megascans listing](https://www.unrealengine.com/marketplace/en-US/product/megascans-trees-european-hornbeam-early-access)).
- **Collision and gameplay.** Horizon [transcript, Q&A]: tree collision "is usually just a cylinder that represents its footprint"; branches do not stop arrows. SpeedTree ships sphere/capsule collision volumes instead of per-triangle collision ([Unity SpeedTree SDK docs](https://docs.unity3d.com/speedtree-runtime-sdk/manual/accessing-3d-geometry.html), via search summary). No source found on stealth sight lines through crowns.
- **Remaster-specific.** Denser, taller modern trees change what the player can see: a critique of Oblivion Remastered argues the updated visuals expose or alter the original world's layout ([ScreenRant op-ed, 2025](https://screenrant.com/oblivion-remastered-graphics-open-world-bad-original-game-op-ed/), opinion).
- **Apple GPUs.** Tile-based deferred GPUs lose hidden-surface removal for fragments that may be discarded: "avoid the use of the discard operation"; alpha-tested primitives cannot write depth until the shader has run, which stalls later primitives; draw order should be opaque → alpha-tested → blended ([Imagination PowerVR docs](https://docs.imgtec.com/starter-guides/powervr-architecture/html/topics/rules/do-not-use-discard.html)). Apple documents hardware hidden-surface removal and that draw order and anything affecting fragment opaqueness matter ([WWDC20 session 10632](https://developer.apple.com/videos/play/wwdc2020/10632/), via search summary). Apple's material does not state the discard penalty in numbers; applying the PowerVR rule to Apple GPUs is an analogy (**[unverified]** for exact behaviour).

### Inference for our engine

- On M1 the alpha-tested prepass is the cost centre, because that is where discard runs. Keep discard out of the colour pass (depth-equal), draw trunks and terrain before cards, cut cards tight, and prefer more opaque geometry at LOD0 (leaf clusters with real outlines) where the atlas allows it.
- Bake the impostor with the same shading inputs as the mesh (albedo, normal, depth, thickness, AO) and light it in the same shader path; never pre-light it.
- Keep collision as the 2002 game had it (trunk cylinders) and check sight lines by gameplay tests in the old woods, since crowns will now hide what painted shells did not.
- TAAU at 2/3 resolution on Metal makes sub-pixel leaves worse; LOD thresholds on Mac may need to switch to impostors earlier, and the impostor needs its own mip coverage correction.

### Gaps

- Measured cost of alpha-tested foliage on Apple M-series GPUs; any published stealth/visibility handling for dense crowns.

---

## H. Remaster precedents

### Takeaway

The best-documented precedent is the Skyrim modding stack, because its rules are written down: LOD assets are generated **from** the full tree so silhouettes match, keyed to the exact mesh, with a hybrid of 3D crown and billboard trunk in the middle distance and normal-mapped billboards beyond. Official remasters on Unreal publish almost nothing technical about trees.

### Findings

- **Skyrim / DynDOLOD.** "Ultra tree LOD" moves trees from the game's own tree LOD into object LOD. Defaults: LOD4 uses a 3D LOD model if one exists and falls back to a billboard; LOD8 and LOD16 use billboards. Billboard1 = 4 triangles per tree, Billboard4 = 8 triangles with better lighting response; internal billboards are two planes at 90°, external ones three planes at 60° ([DynDOLOD docs](https://dyndolod.info/Help/Ultra-Tree-LOD)).
- **Hybrid 3D tree LOD.** Split the full model into trunk and crown; the tool renders the trunk to billboard textures and combines flat trunk planes (4-pane template recommended for lighting) with the real crown geometry. LOD models are matched to the full model by CRC32 of the mesh file, so any change to the tree invalidates its LOD and it silently falls back to a billboard ([DynDOLOD docs](https://dyndolod.info/Help/3D-Tree-LOD-Model); [Step guide](https://stepmodifications.org/wiki/Guide:Creating_Tree_LOD_Models), via search summary).
- **Requirements that became lessons.** UVs must stay in 0–1 so the LOD can use an atlas (tiling bark costs one extra draw call per texture and block); alpha is binary at a fixed threshold, so thickness must be tuned per model; mips are regenerated with coverage preservation; animation and collision are stripped from LOD parts ([DynDOLOD docs](https://dyndolod.info/Help/3D-Tree-LOD-Model); [TreeLODGen README](https://github.com/Cl3mus33/TreeLODGen)).
- **Automated generation (2026).** TreeLODGen builds all LOD levels from the installed trees with the card-fraction rule in section C, bakes node scale into vertices so far trees match near ones, and warns that 3D LOD beyond the first ring is a memory risk ([README, 2026](https://github.com/Cl3mus33/TreeLODGen)).
- **Witcher 3 modding.** A Blender add-on edits the game's SpeedTree runtime meshes, supports the SpeedTree LOD parameters and generates billboard textures in one click ([Blender SRT add-on](https://github.com/ArdCarraigh/Blender_SRT_Addon), via search summary) — an existing Blender-side example of "LOD and billboards produced by the same tool as the tree".
- **Oblivion Remastered (2025).** All assets rebuilt in Unreal Engine 5 on top of the original game logic ([Wikipedia](https://en.wikipedia.org/wiki/The_Elder_Scrolls_IV:_Oblivion_Remastered); [ScreenRant](https://screenrant.com/oblivion-remastered-graphics-open-world-bad-original-game-op-ed/), secondary). No developer text on tree construction found; player reports of tree shimmer at reduced resolution (section G).
- **Gothic 1 Remake.** Press describes forests as denser than in 2001 with volumetric light while keeping a gritty, oppressive mood, on UE5 with Lumen and Nanite; a later patch adjusted art direction and tone mapping after a player survey ([games.gg guide](https://games.gg/gothic-1-remake/guides/gothic-1-remake-everything-you-need-to-know-before-playing/), [Wikipedia](https://en.wikipedia.org/wiki/Gothic_1_Remake), via search summary; secondary and partly inconsistent on dates and engine versions). No technical detail on trees.
- **OpenMW.** Distant statics are merged by object paging rather than given impostors; LOD mismatch after partial mod overrides is a known problem ([GitLab epic](https://gitlab.com/groups/OpenMW/-/epics/4), via search summary).

### Inference for our engine

- Generate every LOD and the impostor from the LOD0 tree by script in the same Blender session, and store a hash of the source mesh with the outputs so a changed tree cannot keep stale LODs (the CRC32 lesson).
- Keep the mood by measurement, not by eye alone: compare crown silhouette height/width and canopy closure against the 2002 shells per forest area, then judge by clip. The remaster precedents show that density is the first thing that drifts.
- The hybrid idea (real crown cards + cheap trunk) is a good LOD3 for our old woods, where trunks are hidden by undergrowth but crowns form the skyline.

### Gaps

- No developer-level information on trees in Gothic 1 Remake, Oblivion Remastered, Kingdom Come, or Witcher 1 → REDkit; no Morrowind tree-replacer post-mortems found.

---

## Unverified or weakly sourced claims — checklist

1. All Horizon Zero Dawn numbers and shader details marked [transcript] (prepass ≈10 %, geometry ≈11.5 %, shadows ≈9 %, 4 % specular, cascades 0–1 only, 64-entry tint table).
2. Brucks's own impostor parameters (only re-implementers' reports were readable); my recollection of 16×16 frames in 2048–4096.
3. Typical atlas map set and 2K–4K resolution for branch cards.
4. Snowdrop ray tracing proxy description (search summary of slides).
5. RDR2 impostor distance ≈500 m (mod page).
6. Licences of the impostor repositories (search summaries; one conflicting).
7. Discard penalty on Apple GPUs (PowerVR rule by analogy).
8. Baked top-down canopy occlusion as common practice for far forest darkening.
9. All triangle budgets, screen-size thresholds and per-frame tree counts under "Inference" headings — proposals, not sourced.
10. Remedy's "one third" micromap figure (quoted from a video via a summary); 60 million plants at 60 fps (press article).

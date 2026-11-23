# Rebuilding an old game's world geometry while keeping its layout: precedents, free workflows, reference numbers (state: 2026-11-12)

Method note: about 19 web searches and fetches. Most remake sources are interviews and reviews; studio post-mortems with numbers are rare. Any claim without a link is listed under Gaps or marked as an inference.

## 1. Commercial remakes and remasters that kept the layout and logic under new art

### Takeaway
The pattern that works keeps the old game logic and its gameplay geometry or collision. The new art sits on top, and collision changes only where new art forces it. This holds for Bluepoint (SotC 2018, Demon's Souls 2020), Saber (Halo CEA 2011), Virtuos (Oblivion Remastered 2025) and Vicarious Visions (D2R 2021). Halo CEA documented the known failure: high-detail visuals drift from the low-poly collision underneath.

### Cited Findings
- **Shadow of the Colossus (Bluepoint, 2018):**
  - The technical director said the game "is still pretty much running the original's code under the hood", while rendering and animation are new. — [GamingBolt 2018](https://gamingbolt.com/shadow-of-the-colossus-ps4-still-runs-on-originals-code)
  - "A lot of the base geometry was reused from the original", then built on to expand the world. — [Game Informer dev diary 2018](https://www.gameinformer.com/b/news/archive/2018/01/28/shadow-of-the-colossus-dev-diary-looks-at-what-goes-into-recreating-a-classic.aspx)
  - Art director Mark Skelton: the team used the original world's base geometry as a template and added to it rather than replacing it. Architecture kept the original footprints, treating the old mesh as a "highly-detailed white box".
  - Steps and stairs were rescaled to fit Wander's height, after input from Ueda.
  - Detail methods:
    - cliff walls built from many ZBrush rock instances, unified by Substance Designer macro maps and projected materials;
    - decals for erosion, moss and cracks, which break up repeating cliff walls;
    - ground of 5 materials blended by vertex paint, plus GPU-placed grass and small rock and debris clutter;
    - "negative space": vegetation removed in some open areas and rock outcrops added to cut repetition;
    - a scale pass with big, medium and small reference elements.
  - Over 50,000 instance objects in a single level. No poly or team numbers given. Collision is not discussed. — [80.lv 2018](https://80.lv/articles/shadow-of-the-colossus-remaking-the-legend/)
- **Demon's Souls (Bluepoint, 2020):**
  - The team agreed up front not to change level layout, encounters or creature placement.
  - Christian de los Santos: "the bounds of the collision were not altered". Enemy pathing and combat stayed in the same space. Collision was changed only for new architecture and for smoother player and camera movement.
  - Layout changes were kept minimal; any change needed Design and Production approval.
  - Houdini handled procedural placement, terrain, asset assembly, collision geometry, and swapping low-res originals for high-res models.
  - Poly budget was "use as many as you need", with a custom LOD system and dynamic tessellation.
  - Team "well under 100" (80+ moved home during the pandemic).
  - Stated difficulty: low-res originals were "open to interpretation".
  - Animations grew from about 4,000 to about 12,000. — [80.lv 2020](https://80.lv/articles/demon-s-souls-how-to-make-a-great-remake)
- **Halo: Combat Evolved Anniversary (Saber / 343, 2011):**
  - Saber's engine draws the visuals; the original Halo engine runs gameplay. Every object in the Halo engine has a proxy that is carried into the Saber engine. Players toggle between old and new graphics live.
  - Collision was a stated problem: the high-res visuals could deviate from the low-poly original collision. — [Wikipedia, 2011 game](https://en.wikipedia.org/wiki/Halo:_Combat_Evolved_Anniversary); [Halopedia, Blam engine](https://www.halopedia.org/Blam_engine)
  - Known community complaints, such as invisible walls and mismatched geometry in CEA, were not checked here (see Gaps).
- **Oblivion Remastered (Virtuos / Bethesda, 2025):**
  - Gamebryo is the "brain" (logic, physics) and UE5 the "body" (visuals). — [AppleGamingWiki](https://applegamingwiki.com/wiki/The_Elder_Scrolls_IV:_Oblivion_Remastered)
  - Every asset was said to be recreated from scratch. — [GamerBraves 2025](https://gamerbraves.com/the-elder-scrolls-iv-oblivion-remastered-has-officially-launched)
  - Hardware Lumen was a major performance cost. — [heise 2025](https://heise.de/-10359349)
  - No source found on whether the landscape heightmap or collision was reused.
- **Diablo II: Resurrected (Vicarious Visions / Blizzard, 2021):**
  - "It is the same game running underneath": sprite- and grid-based simulation, with the original logic and break points. A new custom 3D renderer was built on top. — [Blizzplanet Q&A 2021](https://blizzplanet.substack.com/p/blizzconline-2021-diablo-ii-resurrected-press-qa); [GDC Vault 2022 renderer talk](https://gdcvault.com/play/1027558/An-Overview-of-the-Diablo)
  - The claim that logic runs on a 25 fps tick while rendering is decoupled comes only from a forum summary (unverified).
- **Metroid Prime Remastered (Retro, 2023):**
  - Digital Foundry: all the major bits of original geometry are represented, so area designs stay faithful. — [Eurogamer/DF 2023 (mirror)](https://venvegame.netlify.app/host-https-www.eurogamer.net/digitalfoundry-2023-metroid-prime-remastered-df-tech-review)
  - Reviews describe brand-new surface geometry and reused 2002 animation data. — [TechRadar 2023](https://techradar.com/features/metroid-prime-remastered-shows-what-the-nintendo-switch-is-truly-capable-of)
  - Whether collision was kept is not confirmed.
- **Gothic 1 Remake (Alkimia / THQ Nordic, UE5, released 2026-06-05):**
  - Producer Reinhard Pollice: the map is about 10–15% larger than the original, aiming to keep the cramped, tangled feel while opening the world a little. — [GameStar](https://www.gamestar.de/artikel/gothic-remake-wie-viel-groesser-wird-es,3390342.html)
  - Other sources give other figures:
    - 10–30% — [Wikipedia](https://en.wikipedia.org/wiki/Gothic_1_Remake)
    - about 20% — [Green Man Gaming 2026](https://www.greenmangaming.com/blog/gothic-1-remake-biggest-changes-and-improvements-over-the-original/)
    - "1:1", from a fan guide that contradicts the interview, so low trust.
  - Loading zones removed; Nanite used for dense terrain; places that were underdeveloped in 2001 fleshed out. — [Green Man Gaming 2026](https://www.greenmangaming.com/blog/gothic-1-remake-biggest-changes-and-improvements-over-the-original/)
  - Terrain and volumetrics are GPU-heavy. — [PCGH 2026](https://www.pcgameshardware.de/Gothic-Remake-Spiel-73829/Specials/Over-500-Benchmark-Results-Across-62-CPUs-40-GPUs-and-Linux-1544558/2/)
  - Unlike the precedents above, this is a full engine rewrite (new logic), not original logic under new art.

### Inferences
- Bluepoint's recipe fits our case most closely: keep the original mesh as a "detailed white box" and collision bounds. Add instanced rocks and decals on top, and touch the base only where traversal is wrong (the stair rescale).
- Halo CEA is the cautionary case. When visuals drift from the logic or collision geometry, players hit invisible bumps or float. Any new visual mesh must stay within a measured tolerance of the walkable surface, or collision must be regenerated from the new mesh and re-validated.
- The official Gothic 1 Remake figure is 10–15% larger, which shows that even a commercial remake chose to enlarge slightly to fit modern density. That is a precedent for small, deliberate scale changes, but at the cost of moving everything.

### Gaps
- No primary source on collision handling in Oblivion Remastered, Metroid Prime Remastered, RE4 (2023) or Silent Hill 2 (2024). RE4 and SH2 were not searched (budget). Both are full rebuilds with changed layouts, so they are less relevant.
- No Alkimia statement on how the Colony terrain was rebuilt (heightmap or sculpt, reuse of original data).
- Halo CEA: no primary source detailing invisible-wall complaints.

## 2. Community and open-source world rebuilds

### Takeaway
Community projects show two lessons:
- terrain round-trips through heightmap tools need care, because tools smudge steep areas;
- scope growth is the big risk. Black Mesa's Xen redesign made the area much larger and slipped years, and the whole project took about 16 years.

The Gothic-engine total conversion Archolos (about 6 years) built its own worldmesh inside the original engine.

### Cited Findings
- **TESAnnwyn:**
  - Imports and exports heightmaps for Morrowind through Skyrim.
  - Users report it "smudges the landscape where heights vary greatly".
  - Wiki advice: make the heightmap at higher resolution, then resize to scale. — [UESP](https://en.uesp.net/wiki/Morrowind_Mod:Heightmap); [morrowind-modding](https://morrowind-modding.github.io/modding-tools/landmass-utilities/tesannwyn); [Tamriel Rebuilt tutorial](https://tamriel-rebuilt.org/node/4167)
  - Lesson for steep terrain: heightfields lose cliffs and overhangs, which matters for a mesh world like Gothic's.
- **Black Mesa (Crowbar Collective, v1.0 in 2020):**
  - Xen was redesigned and made much larger, and that size growth was a key reason it slipped from 2017. Early access was used for feedback.
  - The project ran about 16 years. — [GameSpot](https://gamespot.com/articles/half-life-1-fan-remake-black-mesa-is-finally-compl/1100-6474493/); [DSOGaming](https://www.dsogaming.com/news/black-mesa-xen-leaves-beta-and-has-been-fully-released-to-all-early-access-owners/)
- **The Chronicles of Myrtana: Archolos (2021):**
  - Total conversion on the Gothic 2 engine: new open world, about 6 years of development, 170 quests, 1,400 NPCs. — [Steam](https://store.steampowered.com/app/1467450)
  - A team member's portfolio lists layout and boxout, then level mesh creation, inside the original engine. — [ArtStation](https://miwisniewski.artstation.com/projects/L249z0)

### Gaps
- Skywind and Skyblivion landscape workflows: nothing found in the budget.
- Gothic community texture and mesh overhauls (DX11 renderer, texture packs, UE fan remakes): not covered.
- Archolos worldmesh tooling (Blender plugins, Spacer): not found.

## 3. Free, scriptable workflows to add realism without changing layout (erosion, talus, strata)

### Takeaway
Among free tools, only Blender plus open-source code is fully free, local and headless-scriptable:
- built-in A.N.T. Landscape eroder;
- the Hydra add-on;
- Python libraries such as Terra;
- your own numpy or compute erosion.

The commercial terrain tools' free tiers are non-commercial, capped at about 1k resolution, and lack automation or tiling:
- Gaea Community: 1024² builds, no automation or CLI;
- World Machine Basic: 1025², no tiled builds;
- Houdini Apprentice: non-commercial, cannot feed a commercial pipeline.

All of them are heightfield-based, so they suit only the terrain parts of a mesh world, not overhangs or caves.

### Cited Findings
- **Gaea:**
  - Community Edition is free for non-commercial use only. — [QuadSpinner help](https://help.quadspinner.com/article/71-can-i-use-the-license-commercially)
  - Paid editions are perpetual: Indie $99, Pro $199, Enterprise $299. — [QuadSpinner compare](https://quadspinner.com/Compare)
  - The free single build is capped at 1024×1024, according to the order page and third-party listings. — [QuadSpinner order](https://quadspinner.com/Order/)
  - Automation (Gaea.Swarm / Build CLI) and bridges need Professional or Enterprise. — [Gaea docs: Automation](https://docs.quadspinner.com/Guide/Advanced-Topics/Automation.html)
  - So the free edition cannot run headless from scripts.
- **World Machine Basic:**
  - Free; the only restriction is output no larger than 1025×1025 (older sources say 513). — [WM download](https://world-machine.com/download.php); [WM help](https://help.world-machine.com/?p=411)
  - Personal or educational, non-commercial. — [CG Channel 2018](https://www.cgchannel.com/2018/02/try-every-new-feature-in-world-machine-for-free)
  - Tiled export is Professional only. — [WM help](https://help.world-machine.com/?p=411)
- **Houdini Apprentice:**
  - Non-commercial; cannot be used in the same pipeline as commercial Houdini.
  - Renders capped at 1280×720 per SideFX (another 2026 article says 1920×1080) and watermarked.
  - HDAs made in it do not work in Houdini Engine.
  - Licence renews online every 30 days. — [SideFX Apprentice](https://www.sidefx.com/products/houdini-apprentice/); [SideFX FAQ](https://www.sidefx.com/faq/question/435/)
  - Not confirmed whether hython batch runs under it.
- **Blender:**
  - The built-in A.N.T. Landscape add-on has a Landscape Eroder (hydraulic and thermal). Erosion can also be driven as vertex weights in Weight Paint mode, which gives a natural mask channel. It has had Python traceback bugs in some versions (3.1). — [Blender manual](https://docs.blender.org/manual/ru/3.6/addons/add_mesh/ant_landscape.html); [bug #94483](https://projects.blender.org/blender/blender-addons/issues/94483)
  - Hydra add-on (open source): thermal erosion (steep material slides down) and pipe-model water erosion on the GPU via ModernGL. — [Hydra GitHub](https://github.imc.re/ozikazina/Hydra)
  - realistic_terrain: hydraulic erosion on DX11 compute, Windows only. — [GitHub](https://github.com/TLabAltoh/realistic_terrain)
  - Terra: Python terrain library with particle hydraulic erosion and geological primitives; licence listing conflicts (MIT badge vs "other"). — [GitHub](https://github.com/QuentinWach/terra)
- An add-on author's practical lesson: hydraulic erosion alone digs unnatural holes and needs thermal and other weathering to balance it. — [BlenderArtists](https://blenderartists.org/t/simulating-erosion-in-blender/598254)

### Inferences
- Recommended constrained pass:
  1. Bake the original mesh's terrain parts into local heightfield patches. Exclude cliffs with overhangs and caves.
  2. Run thermal erosion (talus angle as a parameter, see section 5), then light hydraulic erosion.
  3. Apply both through a mask that is zero on waypoint corridors (a buffer of a few metres around the path network), building plots, item positions and quest spots, and that limits vertical change to a tolerance (for example ±0.3–0.5 m on walkable areas).
  4. Re-project onto the mesh.
- This is all doable in Blender Python with numpy, or in our own code, running headless (`blender -b -P`).
- Commercial free tiers are a poor fit: they are non-commercial and capped at about 1k, and an open-source remaster may ship builds. Gaea's lack of free CLI rules it out for agent pipelines.

### Gaps
- Blender 4.x/5.x geometry-nodes erosion setups: none verified (geometry nodes have no native iterative solver beyond simulation zones; not confirmed).
- Whether A.N.T. Landscape still ships as an extension in Blender 4.2+ and later was not verified.

## 4. Scanned rock and cliff kits (CC0, Fab), kitbashing and repetition

### Takeaway
Poly Haven rocks and cliffs are CC0 and can go in an open repo. Fab and Megascans assets may ship inside a packaged product but should not be committed raw to a public repo. Bluepoint hides repetition with:
- many instanced sculpted rocks unified by macro maps and projected materials;
- decals for moss, erosion and cracks;
- deliberately emptied negative space.

### Cited Findings
- Poly Haven cliff and rock models (Cliff Side, Coastal Cliff 01/02, Rock Face 01/03, Boulder 01) are CC0 and come as .blend, glTF and FBX.
- Example scan size: Coastal Cliff 01 is about 866k tris and 92 m wide, so it must be decimated or baked to game meshes. — [Poly Haven coastal_cliff_01](https://polyhaven.com/a/coastal_cliff_01); [cliff_side](https://polyhaven.com/a/cliff_side)
- **Fab Standard License:**
  - Covers use in any engine, and distribution of the finished packaged product. — [Epic forums](https://forums.unrealengine.com/t/clarification-on-fab-standard-license-epic-reply-please/2083602)
  - Community readings say raw assets cannot go in an open-source repo. — [Epic forum thread](https://forums.unrealengine.com/t/megascans-in-open-source-project-via-github-organization/486200)
  - A quoted clause bars sharing unmodified source Megascans with external parties (version unclear). — [Polycount](https://polycount.com/discussion/comment/2479305)
  - Legacy Bridge downloads fall under the older UE-only Epic Content License.
  - The binding text is [fab.com/cla](https://www.fab.com/cla).
- Bluepoint's methods (SotC) and the instance count (>50k per level). — [80.lv 2018](https://80.lv/articles/shadow-of-the-colossus-remaking-the-legend/)
- Ubisoft built procedural tools for cliff rocks and freshwater networks in Far Cry 5. — [GDC 2018](https://gdcvault.com/play/1025557/Procedural-World-Generation-of-Far)

### Gaps
- No published number for "unique rock meshes per region" was found.
- The industry practice of a small kit of roughly 5–15 hero rock or cliff meshes per rock type, reused with rotation, scale and material blend, is common knowledge but unverified here.

## 5. Geology and landscape reference numbers (acceptance guard rails)

### Takeaway
Scree and talus sit at about 35–40°. Dry loose sand rests near 30–34°. Vegetated natural hillslopes can stand steeper than fresh loose material (about 41° in one gravel case), because roots reinforce them. Consolidated rock faces stand far steeper. No authoritative "max grass slope" number was found.

### Cited Findings
- Average talus debris at the base of a slope: angle of repose about 35–40°. — [WOU geology handout](https://people.wou.edu/~taylors/g322/masswast.pdf)
- Dry sand: about 30°; repose angle varies with grain size, shape and sorting, and rounder grains give a lower angle. — [OUP geography study guide](https://oupcanada.com/higher_education/companion/geography/9780195425451/student_resources/study_guide/unit_39.html); [Wikipedia: Angle of repose](https://en.wikipedia.org/wiki/Angle_of_repose)
- Forested sand-and-gravel hillslopes in Connecticut average 41.3°, steeper than the fresh angle of repose, because vegetation stabilises them (abstract only). — [Scilit abstract](https://scilit.com/publications/944fbca3fa216361f4cc0348c455feb3)
- Moisture can raise apparent stability, but saturation lowers it. — [Wikipedia: Angle of repose](https://en.wikipedia.org/wiki/Angle_of_repose)

### Inferences (guard rails to adopt, partly unverified)
- Acceptance bands:

| Surface | Band |
|---|---|
| Loose scree or talus aprons at cliff bases | 32–40°, concave-up profile, coarsest blocks at the toe |
| Grass and soil slopes | mostly below ~35°; above that, expect bare soil, rock outcrops or terracettes |
| Steeper than ~45° | reads as rock, not soil |
| Cliffs | 60–90° with ledges |
| Paths | well below ~15° along the line of travel (switchbacks where steeper) |

- The path figure is general hiking-trail practice, unverified.
- Erosion features that read as real: rills and gullies converging downslope, alluvial fans at gully mouths, undercut outer banks on stream bends, and talus cones under chutes in cliffs.

### Gaps
- No sourced numbers for Mediterranean rocky-island coasts (cliff heights, beach slopes), stream-bank geometry or path grades. A dedicated geomorphology search is needed.

## 6. Validating a rebuilt world

### Takeaway
Published practice is automated bots that walk navmesh or paths and report stuck points (Ubisoft's The Division, GDC 2019; Nexon's The First Descendant), plus screenshot comparisons against "last known good" (Sea of Thieves). Remakes add a toggle or side-by-side comparison (Halo CEA's live graphics switch).

### Cited Findings
- Ubisoft "Client Bots" mimic player input and report issues, with automated mission playthroughs. — [GDC 2019 / 80.lv](https://80.lv/articles/gdc-using-ai-controlled-players-to-test-the-division/)
- Nexon's three-stage bot pathfinding falls back to detours around known stuck areas and to flat-ground paths. — [Inven Global](https://www.invenglobal.com/articles/22880/how-did-the-first-descendant-build-an-unmanned-test-environment)
- Sea of Thieves used automated screenshot tests against last-known-good images (secondary source). — [Sauce Labs blog](https://saucelabs.com/resources/blog/from-developer-crunch-to-synthetic-players-ai-is-changing-game-testing)
- Halo CEA's live toggle between old and new graphics. — [PC Gamer](https://pcgamer.com/halo-ce-switch-graphics)

### Inferences
- For a waypoint-network game, a cheap regression suite:
  1. For every waypoint and edge, ray-cast down onto the new collision and check the height delta against a tolerance.
  2. Sweep a capsule along each edge to check for blocking.
  3. Ray-cast every item and quest-object position.
  4. Check sight lines between named landmark pairs (visible before, visible after).
  5. Render paired shots from fixed cameras (original vs new).
  6. Time agent walks of key routes to keep travel times within a few percent.

### Gaps
- No published collision-parity test method from a remake studio was found.

## 7. Geometry density budgets

### Takeaway
Modern engines with virtualised geometry (Nanite) budget per pixel (about one triangle per pixel), not per square metre. Classic landscape components are "a few thousand triangles". No authoritative triangles-per-km² figure was found.

### Cited Findings
- Nanite aims at roughly one triangle per pixel; a landscape is a few thousand triangles per component, and material cost usually dominates. — [Epic forums](https://forums.unrealengine.com/t/why-does-my-nanite-landscape-uses-so-much-triangles/1921012/3)
- Demon's Souls: "use as many as you need", with an LOD system and tessellation. — [80.lv 2020](https://80.lv/articles/demon-s-souls-how-to-make-a-great-remake)
- A single raw scan cliff (Poly Haven Coastal Cliff 01) is about 866k tris over 92 m. — [Poly Haven](https://polyhaven.com/a/coastal_cliff_01)

### Gaps
- No sourced tris/m² or tris/km² for shipped open worlds, and no published sizes of cliff kits in shipped games.

## 8. Pitfalls

### Cited Findings
- Detail diverging from logic geometry (Halo CEA collision deviation). — [Wikipedia](https://en.wikipedia.org/wiki/Halo:_Combat_Evolved_Anniversary)
- Ambiguous low-res originals are open to interpretation (Demon's Souls). — [80.lv 2020](https://80.lv/articles/demon-s-souls-how-to-make-a-great-remake)
- Scope and size growth delayed a fan rebuild by years (Black Mesa Xen). — [DSOGaming](https://www.dsogaming.com/news/black-mesa-xen-leaves-beta-and-has-been-fully-released-to-all-early-access-owners/)
- Even the Gothic 1 Remake chose to enlarge the map by about 10–15% to keep the tangled feel while adding density. — [GameStar](https://www.gamestar.de/artikel/gothic-remake-wie-viel-groesser-wird-es,3390342.html)
- Heightmap round-trips smudge steep relief (TESAnnwyn). — [UESP](https://en.uesp.net/wiki/Morrowind_Mod:Heightmap)
- Detail cost grows with team size (Demon's Souls team grew substantially). — [80.lv 2020](https://80.lv/articles/demon-s-souls-how-to-make-a-great-remake)
- Bluepoint counters repetition and readability problems by removing vegetation for negative space and using light to guide the player. — [80.lv 2018](https://80.lv/articles/shadow-of-the-colossus-remaking-the-legend/)

### Inferences
- Realistic micro-detail (scree, rills, clutter) on a small 2002 map makes it read smaller only if the props carry wrong scale cues. Keep reference-scale elements (Bluepoint's big, medium and small pass).
- Rebuild a whole quarter at once, or blend borders with shared materials, to avoid seams between rebuilt and untouched areas.

### Gaps
- No source quantifies the travel-time or perceived-size effect of added detail.

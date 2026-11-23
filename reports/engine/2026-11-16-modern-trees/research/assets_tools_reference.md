# Modern trees: ready assets, free tools, real-world reference

Web survey. Every source was checked on **2026-11-16** unless a line says otherwise. Nothing was downloaded as an asset and nothing was installed; only pages, API listings, licence texts and repository metadata were read. Claims marked **[unverified]** come from background knowledge or from a secondary source that I could not confirm first-hand.

Constraints applied: tools free and local; content that goes into the public repo or open packs must be CC0 or equally redistributable; models built by us in Blender 5.x (scripted where possible), exported as glTF; no neural 3D generators.

## Short answer

- **Bark:** ambientCG `Bark001–016` and Poly Haven's bark category (24 sets, 8K). Both CC0.
- **Leaf and needle atlases:** ambientCG `LeafSet*`, `Foliage*`, `PineNeedles001` (CC0). There is no CC0 atlas for beech, oak or hornbeam twigs by species name, so the branch atlases for the main species must be built and baked by us.
- **Skeletons:** Sapling Tree Gen 1.0.0 (GPL, Blender 5.2+) or the maintained MTree fork 5.6.0 (GPL add-on, MIT core). Output of both is ours.
- **Impostors:** no maintained open Blender baker exists. Write our own bake script.
- **Not usable in the open repo:** Megascans (still), ShareTextures, Textures.com, The Grove output, Unity and Epic sample forests.

---

## Part 1 — Ready content

### 1.1 Megascans after the November 2026 change

- Epic made the whole Megascans library (23,000+ assets) free again; listings were due to be free by 12 November 2026. The reported change to terms is only about AI training: "We've updated terms for all Quixel assets so you can train AI models on Megascans & Megaplants." The article says the Standard licence itself "doesn't appear to have changed." Source: [CG Channel, 6 Nov 2026](https://www.cgchannel.com/?p=179945); also [80.lv](https://80.lv/articles/entire-quixel-megascans-library-now-available-for-free/).
- The licence summary at [fab.com/eula](https://www.fab.com/eula) (same text at [fab.com/cla](https://www.fab.com/cla)) still lists under "You may not": "Resell or redistribute the asset for free on a standalone basis or allow others to do the same". Sharing is allowed only "(directly, via a private repository or in the Project) with your collaborators".
- **Verdict: redistribution terms did not change. Megascans still cannot be committed to a public repo or shipped in an open pack.** The earlier decision (encrypted pack inside a finished build) is unaffected.
- **Not verified:** the page served only the plain-language summary (about 1,850 characters). I could not retrieve the binding legal text of the Fab EULA, and it showed no revision date. A forum post quotes it as forbidding standalone distribution "except to your collaborators" and requiring that end users cannot extract content ([Unreal forums](https://forums.unrealengine.com/t/fab-license-terms-standard-license/2094243/4)) — secondhand.

### 1.2 Poly Haven (CC0)

Licence: "Our assets are all licensed as CC0", "You can redistribute them" ([polyhaven.com/license](https://polyhaven.com/license)). Open repo: **allowed**.

Models, from the public API listing ([api.polyhaven.com/assets?t=models&c=nature](https://api.polyhaven.com/assets?t=models&c=nature)). All offer Blend, glTF, FBX, USD; textures 1K–8K unless noted ([example page](https://polyhaven.com/a/fir_tree_01)).

| Asset | What | Triangles |
|---|---|---|
| `fir_tree_01` | three tall firs | 7.85 M |
| `pine_tree_01` | pine | 17.4 M |
| `pine_sapling_medium` / `_small` | pine saplings | 9.78 M / 398 k |
| `fir_sapling_medium` / `fir_sapling` | fir saplings | 2.31 M / 433 k |
| `tree_small_02` | small broadleaf | 4.65 M |
| `island_tree_01` / `_02` / `_03` | coastal broadleaf scans | 3.73 M / 1.76 M / 4.76 M |
| `jacaranda_tree` | jacaranda | 312 k |
| `quiver_tree_01` / `_02`, `dead_quiver_trunk` | South African aloe trees | 270 k / 154 k / 34 k |
| `dead_tree_trunk`, `dead_tree_trunk_02` | fallen dead trunks | 102 k / 156 k |
| `tree_stump_01` / `_02` | stumps | 41 k / 62 k |
| `root_cluster_01` / `_02`, `single_root`, `pine_roots` | roots | 225 k / 340 k / 114 k / 163 k |
| `dry_branches_medium_01`, `bark_debris_01` | debris | 17 k / 363 k |
| `shrub_01`–`04` | generic shrubs (4K) | 282 k / 52 k / 17 k / 48 k |
| `shrub_sorrel_01`, `searsia_burchellii`, `searsia_lucida`, `othonna_cerarioides`, `wild_rooibos_bush` | shrubs, mostly South African | 3 k – 1.43 M |
| `fern_02`, `nettle_plant`, `weed_plant_02`, `celandine_01`, `dandelion_01`, `periwinkle_plant`, `moss_01`, `grass_medium_01/02` | understorey | 6 k – 1.75 M |

- **Quality verdict: film-grade, not game-ready.** Whole trees are 0.3–17 M triangles. The fir page shows an "LODs" attribute but lists no levels or counts. No European beech, oak, hornbeam, birch or spruce by name.
- **Leaves:** the fir page lists separate "twig" texture sets with alpha and mask maps and a "Geometry Nodes" attribute, which points to scanned twig cards instanced by nodes. The page does not say so outright — **[unverified]**.
- **Useful parts:** stumps, dead trunks, roots and dry branches are small enough to decimate into props; the twig, trunk and bark texture sets of the fir and pine trees are CC0 source material for our own needle atlases.

Bark textures, category listing ([api](https://api.polyhaven.com/assets?t=textures&c=bark)): 24 entries, 8K (some 8K×16K). Temperate-looking: `bark_brown_01`, `bark_brown_02`, `bark_willow`, `bark_willow_02`, `pine_bark`, `knotted_pine_bark`, `tree_bark_03` (mossy), `bark_platanus`, `jolcham_oak_bark_01` (mossy, fissured oak), `japanese_zelkova_bark`, `sakura_bark`. The rest are Asian species, eucalyptus and palms. No dedicated leaf or branch atlas was found in the texture list (I read only the bark category and the first part of the full list).

### 1.3 ambientCG (CC0)

Licence: "You can copy, modify, distribute and perform the assets, even for commercial purposes"; "You can include the raw files in your project, for example a video game." ([docs.ambientcg.com/license](https://docs.ambientcg.com/license/)). Open repo: **allowed**.

- Bark: search returns 16 assets, `Bark001`–`Bark016`, tagged e.g. fir (`Bark014`), oak (`Bark012`), mossy (`Bark008`, `011`, `013`, `016`), smooth (`Bark002`–`004`, `009`) ([API](https://ambientcg.com/api/v2/full_json?q=bark&limit=100)).
- Leaves: 44 results for "leaf"; atlases include `LeafSet017` (ivy), `019` (needles), `022`–`024` (green leaves), `026`, `027` (maple, autumn), `028`, `029` (ivy), `030` (dried) ([API](https://ambientcg.com/api/v2/full_json?q=leaf&limit=100)).
- Foliage: 11 atlases — `Foliage001`–`008`, `PineNeedles001`, `LeafSet026`, `LeafSet030` ([API](https://ambientcg.com/api/v2/full_json?q=foliage&limit=100)). 61 atlas-type assets in total, including non-plant ones.
- Which maps each atlas carries (opacity, translucency) was not checked per asset — **[unverified]**.
- **Verdict:** good bark; leaf sets are single leaves and small twigs of unnamed species. Enough for bushes, ivy and filler, not a species-true beech or oak branch.

### 1.4 Other sources

| Source | Licence, quoted | Open repo? | Verdict |
|---|---|---|---|
| ShareTextures | "Custom CC0 ... with specific restrictions": "No asset redistribution on other websites, in plugins, or as part of collections without our written permission." ([licence](https://sharetextures.com/p/license)) | **No** without written permission | Red flag: not real CC0. Has species-named foliage atlases (e.g. Morus alba, Platanus). |
| Textures.com | Not allowed "to release our images under Open Source licenses (even when the materials are modified)" ([FAQ](https://www.textures.com/faq-license), via search summary) | **No** | Unfit. |
| cgbookcase | Licence page URL returned 404 | not checked | **[unverified]** CC0. |
| Sketchfab, ffishAsia / floraZia scans | Titles say "CC0" (Japanese beech, sawtooth oak, hemlock, black pine) ([collection](https://sketchfab.com/baxterbaxter/collections/trees-e9f464d05c604e43a34a8ebd3ba20ce3)) | per model | Licence field and triangle counts not confirmed; Asian species. Other trunk and stump scans found were CC-BY or NC. |
| OpenGameArt | One CC0 photogrammetry stump, 1.5 k polygons ([page](https://opengameart.org/content/treestump-photogrammetry)) | Yes | Minor prop. |
| Quaternius, Kenney | CC0 ([Quaternius](https://quaternius.com/packs/stylizednaturemegakit.html), [Kenney](https://kenney.nl/assets/nature-kit)) | Yes | Stylised low-poly — **unfit** for a realistic look. |
| Blender Studio films | "Creative Commons Attribution 4.0" for Spring and Sprite Fright data ([about](https://studio.blender.org/films/spring/pages/about/)) | Yes, **CC-BY**: needs a credit line | Stylised film trees behind a subscription; unfit. A CC0 "Tree, leaves and grass" bundle by Simon Thommes is listed on blender.org demo files — page returned 403, **[unverified]**. |
| Maxtree free Vol. 60 and samples | "Royalty Free License (no AI)" / "Custom License" on CGTrader ([news](https://www.cgchannel.com/2021/11/get-90-free-maxtree-plant-models)) | **No** (not redistributable) | Film-grade, unfit. |
| Dimensiva | nothing found | — | not verified. |
| Unity "Book of the Dead: Environment" | Standard Unity Asset Store EULA; Quixel assets inside ([forum](https://discussions.unity.com/t/book-of-the-dead-environment/705542)) | **No** | Reference only. Unity says Asset Store assets may be used in other engines under the EULA ([support](https://support.unity.com/hc/en-us/articles/34387186019988-Can-I-use-assets-from-the-Asset-Store-with-other-engines)), but not redistributed as source — exact clause **[unverified]**. |
| Epic "A Boy and His Kite" / Open World Demo | Fab listing shows licence "-" ([listing](https://fab.com/listings/4d3e971c-7651-4965-b932-6f49efc5fc1a)); 2015 report: "licensed for use only with UE4-based products" ([CG Channel](https://www.cgchannel.com/?p=69030)) | **No** | Engine-locked or unclear. Electric Dreams: nothing found, **[unverified]** UE-only. |
| Godot / O3DE sample forests | no realistic CC0 sample forest found | — | not verified. |

---

## Part 2 — Free local tools

| Tool | Tool licence | Output licence | State | Headless / output |
|---|---|---|---|---|
| **Sapling Tree Gen 1.0.0** | GPL-3.0-or-later | ours | Updated 4 Nov 2026; "Blender 5.2 LTS and newer"; all new code replacing the add-on bundled until 4.1; old 0.3.7 "fails on Blender 5" ([extensions](https://extensions.blender.org/add-ons/sapling-tree-gen/)) | 29 presets (incl. tree-gen's), armature with fewer bones exported as a skinned mesh, wind with or without bones, leaf flutter. Operator-driven, so scriptable; headless run **[unverified]**. No LODs. |
| **MTree fork 5.6.0** | add-on GPLv3, core MIT | ours | Release 20 Oct 2026, last push 12 Nov 2026; Blender 4.5 LTS+; macOS, Windows, Linux builds ([extensions](https://extensions.blender.org/add-ons/modular-tree/), [repo](https://github.com/GoodPie/modular_tree)) | Node-based; Weber-Penn crown shapes, growth with apical dominance, gravity; leaves are procedural 3D geometry or a custom leaf mesh; Pivot Painter 2.0 wind export for UE5 only. Native C++ module per platform. No LODs, glTF wind data or headless use documented. |
| MTree original | GPL-3.0 | ours | Last release 4.0.2, 11 Dec 2021; last push Sept 2024 ([repo](https://github.com/MaximeHerpin/modular_tree)) | Superseded by the fork. |
| tree-gen | GPL-3.0 | README: models free to use "except for direct sale as assets" | last push July 2025, 964 stars ([repo](https://github.com/friggog/tree-gen)) | Weber-Penn + L-system in pure Python; its presets now ship in Sapling 1.0.0. |
| improved-sapling fork | GPL-2.0 | ours | last push Jan 2023 ([repo](https://github.com/abpy/improved-sapling-tree-generator)) | Dead; use Sapling 1.0.0. |
| Easy Tree | GPL-3.0-or-later; bark from ambientCG CC0 | ours | v1.0.1, July 2025; Blender 4.5+ ([extensions](https://extensions.blender.org/add-ons/easy-tree/)) | Geometry-nodes group, simple wind, custom leaf mesh. Small project (21 stars). |
| Trees-With-Geometry-Nodes | MIT | ours | last push Mar 2022, Blender 3.2 ([repo](https://github.com/IRCSS/Trees-With-Geometry-Nodes-Blender)) | Reference for node technique only. |
| **EZ-Tree** | MIT; bark textures are ambientCG CC0; leaf textures under the project's MIT licence ([texture note](https://github.com/dgreenheck/ez-tree/blob/main/src/app/public/textures/LICENSE.md)) | ours | v1.1.0, Jan 2026; last push July 2026 ([repo](https://github.com/dgreenheck/ez-tree)) | JavaScript/Three.js library; exports GLB and PNG; `generateLODs()` and GLB with all levels; wind is shader-side. No CLI documented; runs outside Blender. |
| TreeIt | freeware, closed | FAQ: selling models allowed "so long as its your own work"; no formal licence text | installer dated 7/5/2026 ([site](https://www.evolved-software.com/treeit/treeit)) | Windows only (DirectX 9); exports OBJ, ASCII FBX, X; adjustable LOD; GUI only. Unfit for a scripted Mac/Linux pipeline. |
| ngPlant | GPL tool, BSD libraries | ours | last release 0.9.13, 2014 ([summary](https://sourceforge.net/projects/ngplant/)) | Dead. |
| Arbaro | GPL-2.0 | ours | old Java Weber-Penn tool ([SourceForge](https://sourceforge.net/p/arbaro/)) | Dead; has a command-line version. |
| SpeedTree | proprietary, subscription | — | Indie about $19/month, revenue cap under $100k; free Learning Edition "don't allow exports of any kind" ([store FAQ](https://store.speedtree.com/?p=810750), via search; store page returned 403) | **Unfit by the rule.** |
| The Grove 2.3 | proprietary; €99 / €199 / €799 | "Do not sell or distribute 3D models of trees grown with The Grove." ([buy page](https://www.thegrove3d.com/buy/)) | Blender 5 | **Unfit twice:** paid, and output cannot go into an open repo. |
| botaniq, Tree Vegetation, Stylized Tree Generator | paid | — | — | Paid; not surveyed further. **[unverified]** |

Cards, atlases, impostors:

- **Tight card meshes:** Atlas2Mesh — GPL-3.0-or-later, v1.1.0 (19 July 2026), Blender 4.2+; finds each opaque shape on an atlas and builds a triangulated mesh with UVs ([extensions](https://extensions.blender.org/add-ons/atlas2mesh/), [repo](https://github.com/BHiMAX/Altas2Mesh)). REVO Opacity Cutout is described as MIT ([docs](https://revolutionart.github.io/REVOpacityCutout-docs/getting-started/introduction/)); its repository was not found — **[unverified]**. Paid alternatives: CardCutter, Cutout Craft, FoliageLab Cutter.
- **Impostor bakers:** Blender-Impostor-Baker — GPL-3.0, last push July 2022, 19 stars ([repo](https://github.com/Pandrodor/Blender-Impostor-Baker)); whether it bakes octahedral layouts is unconfirmed. Godot-Octahedral-Impostors (MIT, last push 2021) is an engine-side reference ([repo](https://github.com/wojtekpil/Godot-Octahedral-Impostors)). Paid: Instant Impostors, Imposter Cards. **No maintained open Blender baker was found.**
- **Branch-to-atlas bake in Blender:** orthographic camera over a modelled twig, render passes for colour, normal, alpha, plus a thickness or translucency pass. Standard practice, no dedicated free tool found — **[unverified]** as a recipe; it needs our own script.
- **Leaf atlas capture from photos:** no open tool found.

---

## Part 3 — Real-world reference

Main source: JRC *European Atlas of Forest Tree Species* chapters (PDFs under `forest.jrc.ec.europa.eu/media/atlas/<Species>.pdf`, e.g. [Fagus sylvatica](https://forest.jrc.ec.europa.eu/media/atlas/Fagus_sylvatica.pdf)). The oak, birch and hawthorn chapters did not download completely, so those rows use Wikipedia and are weaker.

| Species | Height | Trunk (DBH) | Leaf / needle | Bark, crown | Source |
|---|---|---|---|---|---|
| European beech | commonly 30–40 m, up to 50 m; open-grown about 20 m but broader | typically up to 1.5 m | 10 × 7 cm, dark shiny, wavy margin | silver-grey, smooth; single stem; lives 150–300 y | Atlas; [Wikipedia](https://en.wikipedia.org/wiki/Fagus_sylvatica) for open-grown and DBH |
| Pedunculate oak | 20–40 m | girth can exceed 10 m in veterans | 10–12 × 7–8 cm, lobed | grey-brown, deeply fissured in vertical plates; "spreading and unevenly domed" with massive lower limbs | [Wikipedia](https://en.wikipedia.org/wiki/Quercus_robur) only |
| Norway spruce | up to 50–60 m | up to 1.5 m | needles 1–2.5 cm; cones 12–15 cm | orange-brown; crown "regularly conic, columnar", upper whorls ascending, lower drooping; 200–300 y | [Atlas](https://forest.jrc.ec.europa.eu/media/atlas/Picea_abies.pdf) |
| Silver fir | above 60 m at best; 40–50 m typical | up to 1.5 m | needles 1.8–3 cm; cones 9–17 cm, upright | silver-grey straight stem; conical top becoming "a rounded dome" with age; 500–600 y | [Atlas](https://forest.jrc.ec.europa.eu/media/atlas/Abies_alba.pdf); [Wikipedia](https://en.wikipedia.org/wiki/Abies_alba) for sizes |
| Scots pine | 23–27 m average, over 40 m possible | about 1 m mature | needles 5–7 cm in pairs; cones 5–8 cm | upper stem reddish-orange, lower furrowed grey-brown; long bare trunk, rounded or flat top | [Atlas](https://forest.jrc.ec.europa.eu/media/atlas/Pinus_sylvestris.pdf); [Wikipedia](https://en.wikipedia.org/wiki/Pinus_sylvestris) |
| Silver birch | 15–25 m, to 31 m | usually under 40 cm | 3–7 cm, triangular | white, peeling; dark and rugged at the base when old; open crown, pendulous twigs | [Wikipedia](https://en.wikipedia.org/wiki/Betula_pendula) only |
| Hornbeam | 20–25 m, over 30 m in old growth | fluted, "muscled" | 8–10 cm, serrated, strongly veined | smooth steel-grey; crown ovoid to domed; dead leaves hang through winter | [Atlas](https://forest.jrc.ec.europa.eu/media/atlas/Carpinus_betulus.pdf) |
| Black alder | 10–25 m, rarely 35–40 m | — | 4–10 cm, obovate | brown, later dark and fissured; about 60 y, max 160 | [Atlas](https://forest.jrc.ec.europa.eu/media/atlas/Alnus_glutinosa.pdf) |
| Hazel | shrub 4–8 m, rarely over 10 m, multi-stem | — | 6–12 cm, rounded | grey with pale spots; understorey shrub; 80–90 y | [Atlas](https://forest.jrc.ec.europa.eu/media/atlas/Corylus_avellana.pdf) |
| Elder | shrub 4–5 m, up to 10 m | — | pinnate, 5–7 leaflets of 5–12 cm | brown-grey, deeply furrowed; arching branches; flower heads 10–20 cm | [Atlas](https://forest.jrc.ec.europa.eu/media/atlas/Sambucus_nigra.pdf); [Wikipedia](https://en.wikipedia.org/wiki/Sambucus_nigra) |
| Hawthorn | up to about 10 m | — | 2–4 cm, deeply lobed | dull brown with orange cracks; dense crown; thorns about 12.5 mm | [Wikipedia](https://en.wikipedia.org/wiki/Crataegus_monogyna) only |

Plausibility on a temperate to sub-Mediterranean island (Atlas chapters):

- **Beech:** in the south (Spain, Sicily) "only normally present at altitudes of more than 1 000 m". So beech belongs on the upper slopes, not the coast.
- **Silver fir:** 500–2000 m, needs moisture all year. Fits the mountain slopes; more plausible than spruce.
- **Norway spruce:** boreal and subalpine; reaches south only to northern Greece (Wikipedia). The least plausible conifer for such an island; fir or pine reads truer.
- **Scots pine:** south to the Sierra Nevada, but in mountains.
- **Hornbeam:** "High summer temperatures limit its distribution in the south"; lowlands and hills with oak.
- **Alder, hazel, elder:** reach the Mediterranean; alder by water, hazel as understorey, elder on edges and disturbed rich ground.
- **Oak:** lowland and hill forest with hornbeam; safe for the lower woods.

Stand structure:

- Old-growth beech, NW Carpathians: 196–479 stems/ha (mean 303, trees over 8 cm DBH); basal area 37.4 m²/ha; growing stock 405–805 m³/ha; deadwood 91–345 m³/ha (17–51 %); dominant height 34–41+ m; beech "did not exceed 50 m"; largest DBH 121 cm; trees over 80 cm DBH: 9–23 per ha. Kucbel et al. 2012, *Forest Ecology and Management* 264:125–133 ([PDF](https://content.gitbook.com/content/9RjbsQKEDoZjG2bmR6bo/blobs/CyKVB3EBNeSOerY8Ju9o/58_kucbel-old-growth_beech-dominated_forests.pdf)).
- Uholka-Shyrokyi Luh primeval beech forest: 435 living trees/ha (over 6 cm DBH), 36.6 m²/ha, 582 m³/ha, deadwood 163 m³/ha, 10 trees/ha over 80 cm DBH, max DBH 150 cm, mostly three-layered, gaps mostly "not larger than the crown of a canopy tree" ([waldwissen](https://www.waldwissen.net/en/technique-and-planning/forest-inventory/inventory-of-the-largest-primeval-beech-forest-in-europe), [Wikipedia](https://en.wikipedia.org/wiki/Uholka-Shyrokyi_Luh_primeval_beech_forest)).
- Managed beech: crop-tree guidance of up to 80, usually not more than 50 future crop trees per ha, target diameter 60 cm at about 130 years ([NW-FVA papers via search](https://www.nw-fva.de/fileadmin/nwfva/publikationen/pdf/weller_2016_vergleich_von.pdf)). Beech is normally felled at 80–120 years (EUFORGEN guideline, via search).
- **[unverified]** yield-table values I could not retrieve: mature managed beech about 150–250 stems/ha, spruce about 300–500 at 80–100 years, oak 60–100 final crop trees. Spacing implied by the verified old-growth figures: 300–435 stems/ha is about 4.8–5.8 m average spacing over all sizes, while the big canopy trees (10–20 per ha over 80 cm) stand 20–30 m apart.
- **[unverified]** canopy cover of closed beech or fir forest about 85–95 %, with a sparse understorey under beech and a shrub layer (hazel, hawthorn, elder) under oak and at edges.

Forest-grown versus solitary:

- Woodland trees "grow tall and thin", with few side branches low on the trunk; open-grown trees form full, broad crowns "with large limbs growing from further down the main trunk" ([PTES](https://ptes.org/campaigns/wood-pasture-parkland/wildlife/open-grown-trees/)).
- Open-grown crown diameter from stem diameter (Hemery, Savill and Pryor 2005, table on [the author's page](https://gabrielhemery.com/blog/2011/06/15/how-big-will-this-tree-grow/)): oak 4.3 m at 20 cm DBH, 7.8 m at 40 cm, 11.4 m at 60 cm; beech, ash, birch and sycamore fall in the same 10–12 m band at 60 cm. So crown diameter is roughly 19–21 × DBH, and a landmark oak with a 1.2 m trunk would spread 20 m or more (extrapolation is mine).
- Beech crown ratio (live crown length / height) measured at 50 % in pure stands and 71.5 % in beech–pine mixtures (TUM study, via search); a general vigour threshold is 30–40 %. So a 35 m forest beech carries roughly 12–17 m of clear bole.
- **[unverified]** modelling rules of thumb: open-grown broadleaves branch from 2–4 m; trunk taper about 1 cm of diameter per metre in the clear bole, faster inside the crown; root flare at ground level about 1.3–1.6 × DBH, wider and buttressed on old beech and oak. The search found only regression studies, no usable constants.

---

## Part 4 — Recommended shortlist and risks

| Need | Pick | Risk |
|---|---|---|
| (a) Bark | ambientCG `Bark001–016` + Poly Haven bark sets; Poly Haven stumps, roots and dead trunks as decimated props | No named beech or hornbeam bark; smooth sets (`Bark002–004`, `009`) must stand in for beech and be judged in game. |
| (b) Leaf and needle branch atlases | Build twigs in Blender from ambientCG single-leaf sets and Poly Haven fir and pine twig textures, then bake our own atlases (colour, normal, alpha, translucency); cut cards with Atlas2Mesh or our own script | Biggest piece of own work. Ready CC0 atlases are not species-true. Bake recipe is new for the project. |
| (c) Skeletons | First try Sapling Tree Gen 1.0.0 (pure Python, bones for wind, needs Blender 5.2+); second, MTree fork 5.6.0 for forest-grown forms. Landmark and dead trees: hand-built on a generated base. EZ-Tree as a quick shape reference with LODs | Sapling 1.0.0 is twelve days old; headless use of either add-on is untested. MTree needs a native module per platform and documents wind export only for UE5. Neither produces LODs. |
| (d) Impostors | Own Blender script: octahedral or 8-view bake of colour, normal and depth | No maintained open tool; the 2022 GPL add-on is at most a reference. |

Licence red flags: Megascans (no standalone redistribution, private repos only), ShareTextures ("Custom CC0", no collections), Textures.com (no open-source release), The Grove (no distribution of grown trees), tree-gen README (no direct sale of models), Blender Studio (CC-BY, credit required), GPL add-ons are fine because their output is not covered by the GPL **[unverified, standard reading]**.

## What I could not verify

- The binding Fab EULA text and its date; only the summary page was readable.
- Whether Poly Haven tree leaves are cards or geometry, and what its "LODs" contain.
- Per-asset map lists (opacity, translucency) of ambientCG atlases.
- cgbookcase licence, Dimensiva, Blender demo-file bundle, Electric Dreams, Godot and O3DE samples, licence fields of Sketchfab "CC0" scans.
- Headless (`blender -b -P`) operation of Sapling 1.0.0 and the MTree fork; glTF export of their wind data.
- SpeedTree's current price and revenue cap (store page blocked).
- Yield-table stem counts, canopy cover, crown-base heights, taper and root-flare constants; Atlas chapters for oak, birch and hawthorn.

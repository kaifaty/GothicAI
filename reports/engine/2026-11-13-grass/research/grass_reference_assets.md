# Grass reference (reality) and free assets/tools for a Blender-built game grass

Summary: the search tool returned thin results, so this sheet is shorter than the brief asked for. The solid findings are: (a) a Poly Haven model list with polycounts and the CC0 licence terms, (b) a few species height/width figures, grazed-sward tiller and plant densities, and a grass albedo range (linear 0.16 to 0.25, sRGB about 110 to 136), (c) the complaint pattern of grass mods and remasters (too dense, too tall, wrong colour, "too brown" in Oblivion Remastered). Cliff-edge dimensions, bare-soil and litter shares, weed proportions, a base-to-tip colour gradient, texel density and atlas packing numbers were not found; they are in Gaps. Nothing was downloaded. Anything marked [unverified] is not sourced.

## Part 1 - Reality

## Q1.1 Structure of plant and sward: blade size, tufts, tillers, densities, heights

### Takeaway
Hard numbers found: cocksfoot forms dense tussocks with blades 20-50 cm long and up to 1.5 cm wide; Kentucky bluegrass blades are up to 20 cm long and 3-5 mm wide; fine fescue blades are needle-thin (0.6-1.4 mm); grazed ryegrass pasture has about 1,900-3,400 plants/m2 with plant density tracking tiller density; sheep-grazed sward held at 40-50 mm. Ryegrass blade size and unmown meadow height were not found.

### Cited Findings
- Cocksfoot (Dactylis glomerata): height 20-140 cm, blade 20-50 cm long, up to 1.5 cm wide, grows in dense perennial tussocks. — [Wikipedia](https://en.wikipedia.org/wiki/Dactylis_glomerata)
- Kentucky bluegrass (Poa pratensis): 30-70 cm tall, blades up to 20 cm long and 3-5 mm wide, creeping rhizomes, broad blunt leaves spreading at the base to form close mats. — [Wikipedia](https://en.wikipedia.org/wiki/Poa_pratensis)
- Red fescue (Festuca rubra): 2-20 cm tall per that page [odd: seems to describe leaves; flowering culms are taller in floras, unverified], narrow needle-like leaves, some subspecies rhizomatous, others bunchgrass tufts. — [Wikipedia](https://en.wikipedia.org/wiki/Festuca_rubra)
- Fine-leaved fescue leaf width 0.6-1.4 mm (UK urban grass key, group figure, not tied to one species). — [Amenity grass guide](https://www.externalworksindex.co.uk/Docs/1775121303489/preview/The-amenity-grass-guide.pdf)
- Tor-grass (Brachypodium pinnatum): 70-120 cm tall, calcareous grassland, open flowerhead with 10-15 erect spikelets; blade size not given. — [Wikipedia](https://en.wikipedia.org/wiki/Brachypodium_pinnatum)
- Perennial ryegrass (Lolium perenne): stems up to 90 cm (tagged failed verification), bunching/tillering habit; no blade size on the page. — [Wikipedia](https://en.wikipedia.org/wiki/Lolium_perenne)
- Grazed ryegrass pastures (UK, sheep): plant density paralleled tiller density, averaging 1,900 (rotational), 3,360 (set stocked), 2,330 (combined) plants/m2. — [Cambridge J. Agric. Sci. study via search](https://core-prod.cambridgecore.org/core/journals/journal-of-agricultural-science/article/morphology-of-perennial-ryegrass-lolium-perenne-plants-in-pastures-under-intensive-sheep-grazing/836183DC8F3F60CA33FA2A54969CCBF7) (page itself could not be fetched; figure taken from the search summary)
- Continuously sheep-stocked ryegrass swards held at 40-50 mm surface height. — [Rothamsted paper](https://repository.rothamsted.ac.uk/download/ebdb3d3ce0a739eb64fc8f636fd7e6fdef987a4061021b5fff036ca85b22977e/148948/j.1365-2494.2004.00440.x.pdf)
- Annual ryegrass grazing trials used 5, 10, 15, 20 cm heights; leaf elongation and leaf size increased with taller swards; 10 cm post-grazing favoured cattle intake over 5 cm. — [Embrapa study](https://www.alice.cnptia.embrapa.br/alice/bitstream/doc/951995/1/SuneGFS.pdf)
- Artificial-sward sheep study spanned 346 to 25,980 tillers/m2. — [CSIRO](https://www.publish.csiro.au/AR/AR9840565)
- Nitrogen raised ryegrass tiller number and blade length/width; grazing management changed plant size, not structure (tiller/leaf counts). — [Cambridge J. Agric. Sci.](https://www.cambridge.org/core/journals/journal-of-agricultural-science/article/effects-of-white-clover-and-fertilizer-nitrogen-on-clover-and-grass-leaf-dimensions-percentage-cover-and-numbers-of-leaves-and-tillers/3254AAEA4B6AA70EB82C2C33F01BFEA5)
- Seed crop benchmark: more than 2,000 reproductive tillers/m2. — [Lincoln thesis](https://researcharchive.lincoln.ac.nz/server/api/core/bitstreams/31b13ed1-03d6-4d89-8db3-cae939503c48/content)

### Inferences
- A usable tuft for a pasture: 20-40 narrow blades (3-8 mm) 10-20 cm long [inference, not sourced]; for tussock grasses (cocksfoot, Brachypodium) blades up to 20-50 cm, arching.
- Grazed pasture at 4-5 cm height and 2,000-3,000 plants/m2 means one plant per roughly 3x3 cm cell; individual plants are not readable from game distance, only the 4-5 cm uniform mat.

### Gaps
- Blade length/width for L. perenne, Festuca pratensis, Poa pratensis tillers per plant: not found (primary sources not fetchable; Massey pasture-ID pages and NIAB descriptions PDF were not readable).
- Unmown meadow height, roadside/cliff-top grass height, bare soil and litter share seen from above: not found.

## Q1.2 What breaks uniformity

### Takeaway
No quantitative proportions found. Qualitatively, game-grass mods are criticised when one green is laid everywhere; breaking uniformity is an art-direction need that I could not back with agronomy numbers.

### Cited Findings
- Complaint that grass mods "just put green grass everywhere even in brown landscapes". — [Nexus Mods forum](https://forums.nexusmods.com/topic/11412338-what-grass-mods-create-the-most-seamless-look)
- Poa pratensis: rhizome mats vs Dactylis: tussocks, so one sward mixes spreading mats and bunches. — see Q1.1 sources.

### Inferences
- Species-mixture breaks: tussock grasses (Dactylis, Brachypodium) above a fine fescue/ryegrass mat; seed heads of 70-120 cm (Brachypodium) or up to 140 cm (Dactylis) stand above the leaves.

### Gaps
- Percentages of clover, plantain, dandelion, moss, dead blades, grazing patches: not found [unverified].

## Q1.3 Colour

### Takeaway
Commonly cited PBR range: short green grass linear albedo 0.20-0.25 (sRGB about 122-136), tall wild grass 0.16-0.18 (sRGB 110-116); one swatch RGB 123-130-78 at 0.25. These are chart values of uncertain provenance, not primary measurements.

### Cited Findings
- Albedo chart: short green grass 0.2-0.25 linear = sRGB 122-136; tall wild grass 0.16-0.18 = sRGB 110-116; green grass swatch 0.25 = RGB 123-130-78; chart authors say values "in the ball park". — [Polycount thread (search summary; page returned 403 on fetch)](https://polycount.com/discussion/236620/pbr-grass-vs-asphalt-albedo-base-color-value), [Polycount comment](https://polycount.com/discussion/comment/2797828)
- Leaf reflectance peaks near 550 nm in green; red edge 690-720 nm; leaf NIR reflectance and transmittance about 50% each; visible reflectance rises toward senescence. — [Sensors 2016 figure](https://pmc.ncbi.nlm.nih.gov/articles/PMC4850951/figure/sensors-16-00437-f003), [ASPRS 1976](https://asprs.org/wp-content/uploads/pers/1976journal/nov/1976_nov_1399-1410.pdf)
- Open spectra dataset: leaf reflectance of grass species, 400-2400 nm at 1 nm, about 15 MB, 5 NEON/LTER sites (US prairie/desert grasses, not European); licence not stated on the page seen. — [Dryad, Pau et al. 2025](https://datadryad.org/dataset/doi:10.5061/dryad.gf1vhhn0n)

### Inferences
- Dry/straw albedo and a sward-base darkening factor are not sourced. Dryad dataset or USGS/ECOSTRESS libraries would give them; not computed here.

### Gaps
- Dry grass and straw albedo, base-to-tip colour change, seasonal shift, base darkness: not found.

## Q1.4 Cliff and ledge edges

### Takeaway
Species and zonation are sourced (red fescue, thrift, sea campion, rock samphire); no dimensions for turf overhang, hanging blade length or soil band were found.

### Cited Findings
- Maritime cliff vegetation: sea campion, thrift, rock samphire, high frequency of red fescue; where slope eases, maritime grassland with red fescue and sea carrot, or heath; thrift forms dense wind-sheltering cushions; sea campion roots bind loose ground. — [Wildlife Trusts](https://www.wildlifetrusts.org/node/3143), [Kent Wildlife Trust](https://www.kentwildlifetrust.org.uk/habitats/coastal/cliff), [Discover Wildlife](https://discoverwildlife.com/plant-facts/flowers/how-does-thrift-survive-life-on-salty-cliffs)
- Reference photographs (geograph/Commons, licence on each file page, check before use): [grassland above the cliffs, geograph 1539593](https://download.osmand.net/wiki/File:Grassland_above_the_cliffs_-_geograph.org.uk_-_1539593.jpg); [Cemmaes Head cliffs, geograph 1034896](https://wiki.riteme.site/wiki/File:Cliffs_at_Cemmaes_Head_-_geograph.org.uk_-_1034896.jpg); [Commons Cliffs category mirror](https://c.enwp.org/wiki/Category:Cliffs); [geograph photo 2132897](https://www.geograph.org.uk/photo/2132897). I did not open the images, so their contents are not verified.

### Gaps
- Overhang depth, hanging blade length, soil band thickness, ferns/ivy/heather at the lip, Mediterranean cliff-top species: not found.

## Q1.5 Transition grass to rock and paths

### Gaps
- No source found for transition band width. Not answered.

## Part 2 - Free material and tools

## Q2.1 CC0 grass assets

### Takeaway
Poly Haven has only one grass model (Grass Medium 02, about 1.04 M triangles, 4K, no LODs listed), so it is a source model to bake from, not a ready game asset. There is no clover model. ambientCG has categories for Grass, Moss, Leaf, Foliage, Flower Set.

### Cited Findings
- Poly Haven licence: all assets CC0, use for any purpose including commercial, redistribution allowed, no credit needed; site text/logos excluded. — [Poly Haven licence](https://polyhaven.com/license)
- ambientCG licence: CC0 1.0 for files and previews; may be put in a game; credit optional. — [ambientCG licence](https://docs.ambientcg.com/license/)
- Poly Haven plants models (name, polycount, from API; texture size known only for the first): Grass Medium 02 (grass_medium_02, 1,043,926, 4096 max, author Rico Cilliers, published 2024-02-06, no LOD info); Dandelion 01 (64,099); Celandine 01 (1,746,764); Moss 01 (246,170, ground cover geometry nodes); Fern 02 (6,232); Nettle Plant (46,195); Weed Plant 02 (17,062); Shrub Sorrel 01 (3,319); Shrub 01 (282,224, meadow/flower); Shrub 02 (52,317); Shrub 03 (16,955, meadow/field); Shrub 04 (47,813); Periwinkle Plant (67,388); Dry Branches Medium 01 (16,803); Anthurium (4,134), Calathea Orbifolia (16,688) not relevant to Europe; Karoo flowers (South Africa) not relevant. No clover. — [Poly Haven API, plants](https://api.polyhaven.com/assets?t=models&c=plants), [grass_medium_02 info](https://api.polyhaven.com/info/grass_medium_02)
- Poly Haven terrain textures with grass or moss (max resolution): Grass Ground 16K, Grass Path 2 8K, Grass Path 3 8K, Aerial Grass Rock 8K, Lichen Rock 8K, Mossy Rock 8K, Forest Ground 01 8K, Brown Mud Leaves 01 8K (list truncated; remaining 32 KB of the page not read). — [Poly Haven API, terrain](https://api.polyhaven.com/assets?t=textures&c=terrain)
- ambientCG lists categories Grass, Leaf, Leaf Set, Moss, Foliage, Flower Set, Scattered Leaves, but the page text gave no asset IDs. — [ambientCG list](https://ambientcg.com/list?q=grass)
- Warning: Grass Medium 02 is built as geometry nodes; the 1 M triangle count is for the whole patch. Fab/Megascans and Textures.com licences were not checked here; they forbid redistribution in open packs per project memory, not re-verified.

### Gaps
- Per-model texture size and LOD presence for all models except Grass Medium 02; ambientCG asset IDs for grass/moss; Sketchfab/OpenGameArt CC0 grass items; ambientCG foliage atlases.

## Q2.2 Free Blender tools and methods

### Takeaway
Found tools are mostly not clearly open-licensed: Grassify (free, licence not stated), Dynagrass (card-based, paid or unknown), iMeshh Unkempt Lawn (royalty-free, not CC0). A CC BY 4.0 grass atlas exists for CryEngine.

### Cited Findings
- Grassify: free procedural grass geometry-nodes generator, in the Baked Blender Pro Suite or on Gumroad; licence not stated. — [CG Channel](https://www.cgchannel.com/2023/06/free-tool-grassify-adds-grass-to-blender-models), [80.lv](https://80.lv/articles/grassify-free-procedural-animated-grass-generator-for-blender)
- Dynagrass: procedural grass cards for Blender, inspired by game alpha cards; price and licence not shown. — [Gumroad](https://thenutkat.gumroad.com/l/dynagrass-procedural-grass-cards-for-blender)
- Unkempt Lawn 01: free geometry-nodes lawn, royalty-free licence. — [iMeshh](https://imeshh.com/assets/unkempt-lawn)
- CryEngine asset DB grass: atlas with bending nodes, CC BY 4.0 (attribution needed). — [CryEngine](https://cryengine.com/asset-db/product/janbostl/grass)
- cuboy grass pack: free low-poly textures. — [itch.io](https://cuboy.itch.io/grasspackvol1/purchase)

### Gaps
- Tutorials on baking a tuft to a card atlas, atlas packing numbers (variants, texel density, padding, mips), flatbed-scan method, open-source grass projects with reusable assets: not found.

## Q2.3 Photo/scan method
### Gaps
- Nothing found. Method steps and resolutions unsourced.

## Q2.4 How modders and remasters treated grass

### Takeaway
Recurring complaints: too dense/tall, colour mismatch with terrain, uniform green everywhere, and (Oblivion Remastered) too brown/thin with late pop-in.

### Cited Findings
- Taller grass and higher density from grass mods makes Skyrim look overgrown; Cathedral Landscapes is seen as closer to vanilla; "No Grassias" removes grass mods' changes on land that was bare in vanilla (roads, settlements). — [Nexus forum thread](https://forums.nexusmods.com/topic/11412338-what-grass-mods-create-the-most-seamless-look), [STEP wiki](https://stepmodifications.org/wiki/SkyrimSE:Cathedral_Landscapes)
- Oblivion Remastered: widely called too brown, foliage dialled back, vegetation thin or popping in; a colour-restoring mod (Ayleid Reshade) became popular; opposite view says environments are denser than the original. Sources are forums and blogs, treat as anecdotal. — [GamesRadar](https://www.gamesradar.com/games/the-elder-scrolls/oblivion-remastered-players-flock-to-mod-that-restores-the-colors-and-light-of-the-original-and-even-the-witcher-4s-narrative-director-needs-those-intense-greens-that-i-had-been-missing), [Steam discussion](https://steamcommunity.com/app/2623190/discussions/0/604154270792591161)

### Gaps
- Gothic 1/2 texture mods, Gothic remake (Alkimia) vegetation, Demon's Souls and Shadow of the Colossus remakes: not searched or not found.

## Gaps (overall)
- Blade length and width for L. perenne and Festuca; tillers per plant; unmown/roadside/cliff-top heights; bare soil and litter share.
- Proportions of weeds, flowers, moss, dead blades; grazing-patch and path structure.
- Dry grass/straw albedo, base-to-tip and seasonal colour; USGS/ECOSTRESS values.
- Cliff-lip dimensions; transition band width; Mediterranean cliff-top species.
- Poly Haven texture sizes/LODs except Grass Medium 02; ambientCG grass and foliage asset IDs; Sketchfab and OpenGameArt CC0 items.
- Tuft-to-atlas tutorials, atlas packing numbers, scan method, open-source grass projects.
- Gothic mods, Alkimia Gothic remake, Demon's Souls/SotC grass.
- The search tool was weak and several primary papers (Cambridge, MDPI, NIAB PDF) could not be fetched or read; the NIAB herbage descriptions PDF was fetched but its text could not be read.

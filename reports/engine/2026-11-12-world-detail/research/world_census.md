# Khorinis world geometry census (NEWWORLD, plus OLDWORLD and ADDONWORLD summaries)

Measured 2026-11-12 on the Mac, read-only, single-process numpy. Nothing in the repository, the base cache or the game folder was written.

## What was measured

- Base: `~/.cache/gothic-ai/base/c885037e0d829cfdaea7fc5db0b034d11eda6d678b30446ced78a39e36dfd09e` (fingerprint in `base.json`; `current` points at it; world files dated 2026-11-10). This is the **converted original**: no remaster content (`content-staging`, `working-N`) is applied, so the world-edit chunks (stairs, roof holes, ...) are not in these numbers. `world.json` of NEWWORLD says: 473 502 triangles, 260 696 vertices, 403 chunk files, `chunk_size_cm` 6400, source `NEWWORLD.ZEN`.
- Storage: `worlds/<WORLD>/chunks/<x>_<z>.glb` (one glTF 2 binary per 64 m chunk; positions in glTF centimetres with x = -Gothic x, y up), `world.json` (materials with `group`, `texture`, `texture_scale`, ...), `vobs.json` (all vobs as a tree), `textures/*.dds`, `meshes/*.glb`. Chunk id `x_z` = floor(Gothic x / 64 m), floor(z / 64 m) (checked: 100 % of triangles match their file name).
- Existing tool: `scripts/content/world/chunk_glb.py` (`read_chunk`, `read_glb`) is imported unchanged; there is **no census tool** in `scripts/content/` (`world_edit.py` and neighbours edit and verify single chunks; `chunk_glb.report` only gives triangles/area per material of one chunk). My scripts are new and live only in the scratchpad.

## Scripts and commands (all in `<scratchpad>/world-detail/census/`)

```
cd <that directory>
python3 load.py NEWWORLD      # also OLDWORLD, ADDONWORLD: reads all chunk GLBs through chunk_glb.read_chunk -> <WORLD>.npz (<1 s each)
python3 census.py NEWWORLD 1  # classes, density, edges, chunks, slopes, texel density, 1 m layer raster -> NEWWORLD_census.md/.json (about 20 s)
python3 vobs.py NEWWORLD      # vobs by visual name, mesh triangles from base meshes/*.glb -> NEWWORLD_vobs.md
```
`classify.py` holds the name rules for the material classes. System python3 with numpy; nothing installed. Whole run: well under a minute of one CPU core.

## How to read the numbers (definitions and caveats, important)

1. **Triangles of vegetation are inside the world mesh.** About 38 % of NEWWORLD's triangles (177 582 of 473 502) carry tree/bark/branch/leaf/plant materials (class "vegetation" below). They are forest-cover shells and tree geometry baked into the level mesh, not vobs. Biggest: `NW_NATURE_TREE_04` (29 151 tris, median 34 m2 per triangle, 1.32 km2 of surface, 107 chunks), `NW_NATURE_TREE_NEEDLE_03` (39 038 tris), `NW_NATURE_BRANCH_01` (26 440), `NW_NATURE_BARK_04` (56 123 tris, median 0.29 m2, trunk-like). I did not look at them visually; the interpretation (canopy shells, trunks) comes from material names, triangle sizes and heights, so treat it as likely, not verified. They are excluded from the "ground / rock" density and from the layer test.
2. **Material classes are by name rules, not by an authoritative tag** (`classify.py`; Gothic `group` field: 2 stone, 3 wood, 4 earth, 5 water is used for water only). Check the 40-material table below. NEWWORLD rules were tuned until the leftover "other" class was 0.1 % of triangles. The `S:<ROOM>_<MATERIAL>` and `P:` names are per-room copies of materials; the prefix is stripped. OLDWORLD (`OW_*` rules) is roughly right; **ADDONWORLD is rough** (`NW_SEQ_*` names guessed from name only, seabed and canyon rock are mixed into the classes).
3. **Land vs seabed**: a triangle is "below sea" when its centroid is lower than the ocean plane (NEWWORLD y = -7.0 m, median of the largest water material). Roughly half of NEWWORLD "ground" area (559 000 of 997 000 m2) is the sea floor (`NW_NATURE_GREATSEA_SAND_03`: 851 triangles, 33.9 m median edge). Use the "land" rows for playable ground.
4. **Slope** = angle of the triangle normal from straight up, area-weighted. Triangle winding gives up-facing normals for ground (checked: 99.6 % of `WALDBODEN_03` is up-facing), so the sign is reliable. "Leaning out" (normal y between -0.5 and 0) is a near-vertical wall tilted slightly down; "ceiling" (normal y < -0.5) is an overhang or underside. Triangles with y-normal exactly 0 count in 60-90.
5. **Layer test (item 5)**: each up-facing triangle (normal y > 0.1, i.e. slope < 84 degrees) is rasterised to the 1 m XZ cells whose centre it covers (exact barycentric test), giving a height per hit. Per cell the heights are sorted and chained: a gap of more than 2.0 m between consecutive heights starts a new layer. Down-facing triangles (ceilings) are not layers; a cave with its floor and a hill surface above it is two layers. Vertical walls are not counted. Cells whose centre no triangle covers (tiny triangles smaller than a cell, cracks) are missed. Sets: **natural** = ground + rock classes above sea level; **solid** = natural + man-made; "natural + seabed" includes below-sea triangles. Water and the vegetation shells are excluded from every set (canopy would add a fake second layer 20-40 m above every forest cell). Unit "m2" = number of 1 m cells.
6. **Chunk/where table** attributes a multi-layer cell to the chunk of its lowest layer's triangle; "cave mats" = share of end layers with a `S:*CAVE*` room material. Descriptions (cave, cliff) are inferred from materials only; I did not look at them in the game. A big gap with rock above ground reads as a cliff/overhang or a rock roof over a cave; a rock or ground over `NW_CITY_*` reads as a roof over a floor.
7. **Edge length** is over all three edges of each triangle, with the same triangle counted in its class. **Texel density** = sqrt(UV area x texture width x height / world area) per triangle of that material (triangles over 0.05 m2), using the `.dds` size in the base (512x512 for all ten, DXT1). It assumes UVs are in 0..1 texture units. This is the original texture; any remaster texture override in `content` is not read.
8. "Area" always means the 3D surface area of the triangles in m2 (1 unit = 1 cm in the files, so /1e4 for m2). Densities are triangles per m2 of surface, not of footprint.

## Short answers (NEWWORLD)

- Triangles 473 502, vertices 260 696 (engine level; 259 404 unique float32 positions), 403 chunks of 64 m, bbox x -559..995, y -230..176, z -820..886 m (Gothic axes; that is 1.55 x 1.71 km), surface area 7.02 km2, 1 157 zero-area triangles.
- Plan footprint: sum of A x ny over all up-facing triangles = 2.74 km2, but that includes water (0.94), seabed (0.62), vegetation shells (0.24). Land footprint, non-water, non-vegetation, above sea level: **0.81 km2 of 1 m cells with at least one up-facing solid layer** (0.77 km2 natural only; man-made adds about 34 000 cells).
- Land ground: 35 858 triangles over 438 000 m2 = 0.082 tris/m2, median edge 4.5 m (p10 1.7, p90 9.6), i.e. one triangle per 7 m2. Land rock: 98 240 triangles over 1.20 km2 = 0.082 tris/m2, median edge 2.7 m (p10 0.8, p90 9.4). Man-made: 0.64 tris/m2, median edge 1.0 m. Ground is 66 % under 15 degrees and 92 % under 30 degrees; rock is mostly steep (35 % in 60-90, 27 % leaning-out or ceilings).
- Chunk triangle counts: min 1, median 120, mean 1 174, p90 3 362, max 15 762 (chunk 11_-1, forest shells); the densest city/harbour chunks are `-2_-3` (15 457), `12_3` (15 217), `13_3`, `0_0`, `1_0`, `1_-1`, `0_-1`.
- **Heightfield**: 91.1 % of natural land cells have exactly one layer, 8.9 % have two or more layers separated by more than 2 m (7 239 cells have three or more); with man-made surfaces included 88.5 % / 11.5 %. About 45 % of the multi-layer natural cells have the lowest and highest layers more than 15 m apart (cliff ledges, rock roofs, caves), 41 % 5-15 m, 14 % up to 5 m. Only about 10 000 of the 68 000 multi-layer cells involve an explicit `S:*CAVE*` room material; most multi-layer area is hillside rock over forest floor or ledged cliffs (STEIN_03 / STEIN_07 / STEIN_02 over WALDBODEN_03 / PATH). OLDWORLD is 93.2 % single-layer, ADDONWORLD 83.9 % (rough classes).
- **Vobs**: 23 288 vob objects in total; 12 479 with a mesh/model visual. Trees (Tanne, Smalltree, Bigtree, stumps) 1 643 instances of 22 meshes, mean 150 triangles per instance (typical fir `NW_NATURE_TANNE_75P` = 75 triangles, `OW_NATURE_SMALLTREE_01` 191, `NW_NATURE_BIGTREE_356P` 351, `HOHETANNEN_01_531P` 531); bushes 1 652 (mean 153, `BUSH_120P` 120 tris, 690 instances); plants/grass/ferns 2 479 (mean 180; `GRASSGROUP_01` 40 tris x 688, `FARNTEPPICH_306P` x 565); rocks 708 (mean 173); other static visuals 5 880. So trees are **both**: the dense forest is world mesh, the ~1 600 vob trees are scattered separate objects (about 245 000 triangles in total if all drawn, about half the size of the whole world mesh).
- **Texel density** of the largest land ground materials is about 100 texels/m (WALDBODEN_03 101.8, DARKWOOD_BODEN_01 101.4, SWEETGRASS_01 101.1, PATH_06 125, KIES_01 83); the sea floor is 17-49 texels/m (a 512 px DXT1 texture tiled at 10-30 m per repeat).

# Full tables NEWWORLD

## NEWWORLD: totals

Sea level used to split land from seabed: y = -7.0 m (median centroid height of the largest water material).

| quantity | value |
|---|---|
| triangles | 473,502 |
| vertices (world.json, engine level) | 260,696 |
| unique positions (float32) | 259,404 |
| bbox min x,y,z (m, Gothic axes) | [-559.1, -229.7, -820.4] |
| bbox max x,y,z (m, Gothic axes) | [995.6, 176.0, 885.7] |
| 64 m chunk files | 403 |
| surface area of all triangles (m2) | 7,016,207 |
| plan area of up-facing triangles, sum(A*ny) (m2) | 2,739,142 |
| zero-area triangles | 1151 |


### By material class

| class | triangles | area m2 | plan (up-facing) m2 | tris per m2 | tris below sea (below sea level) |
|---|---|---|---|---|---|
| ground | 37,087 | 997,391 | 964,990 | 0.037 | 1,226 |
| rock | 101,440 | 1,681,015 | 523,692 | 0.060 | 3,140 |
| water | 13,345 | 1,873,653 | 935,568 | 0.007 | 134 |
| man-made | 143,003 | 224,404 | 72,959 | 0.637 | 435 |
| vegetation | 177,582 | 2,238,532 | 241,234 | 0.079 | 2,068 |
| other | 1,045 | 1,212 | 699 | 0.862 | 2 |


### Ground and rock split by sea level

| class | part | triangles | area m2 | tris per m2 | plan m2 |
|---|---|---|---|---|---|
| ground | land (above sea level) | 35,861 | 438,517 | 0.082 | 417,956 |
| ground | below sea | 1,226 | 558,874 | 0.002 | 547,034 |
| rock | land (above sea level) | 98,300 | 1,203,961 | 0.082 | 446,449 |
| rock | below sea | 3,140 | 477,053 | 0.007 | 77,243 |


### Edge length (m) and triangle area per class

| class | part | tris | edge p10 | edge p50 | edge p90 | median tri area m2 | sqrt(2*median area) m |
|---|---|---|---|---|---|---|---|
| ground | all | 37,087 | 1.76 | 4.62 | 10.25 | 7.303 | 3.82 |
| ground | land | 35,861 | 1.73 | 4.52 | 9.61 | 7.017 | 3.75 |
| rock | all | 101,440 | 0.79 | 2.73 | 9.97 | 2.544 | 2.26 |
| rock | land | 98,300 | 0.78 | 2.67 | 9.38 | 2.46 | 2.22 |
| man-made | all | 143,003 | 0.17 | 1.0 | 4.14 | 0.276 | 0.74 |
| man-made | land | 142,568 | 0.17 | 1.0 | 4.1 | 0.274 | 0.74 |
| vegetation | all | 177,582 | 0.46 | 3.18 | 10.93 | 3.123 | 2.5 |
| vegetation | land | 175,514 | 0.45 | 3.14 | 10.63 | 2.998 | 2.45 |


### 40 largest materials by area

| material | group | class | tris | area m2 | tris/m2 | median edge m |
|---|---|---|---|---|---|---|
| NW_WATER_LAKE01__OCEAN_CLOSE_01 | 5 | water | 8,272 | 1,801,013 | 0.005 | 8.61 |
| NW_NATURE_TREE_04 | 0 | vegetation | 29,151 | 1,320,866 | 0.022 | 10.78 |
| NW_NATURE_STEIN_07 | 2 | rock | 11,853 | 977,053 | 0.012 | 6.31 |
| NW_NATURE_GREATSEA_SAND_03 | 4 | ground | 851 | 538,654 | 0.002 | 33.89 |
| NW_NATURE_STEIN_03 | 2 | rock | 35,440 | 458,823 | 0.077 | 3.68 |
| NW_NATURE_TREE_NEEDLE_03 | 0 | vegetation | 39,038 | 371,240 | 0.105 | 4.1 |
| NW_NATURE_BRANCH_01 | 0 | vegetation | 26,440 | 271,413 | 0.097 | 4.34 |
| NW_NATURE_WALDBODEN_03 | 4 | ground | 15,692 | 196,077 | 0.080 | 4.45 |
| NW_NATURE_GREATSEA_PLANT_01 | 0 | vegetation | 1,982 | 113,353 | 0.017 | 12.57 |
| NW_NATURE_DARKWOOD_BODEN_01 | 4 | ground | 6,430 | 111,110 | 0.058 | 5.93 |
| NW_NATURE_STEIN_02 | 2 | rock | 11,145 | 86,790 | 0.128 | 3.27 |
| NW_MISC_FULLALPHA_01 | 0 | vegetation | 138 | 57,013 | 0.002 | 29.73 |
| NW_NATURE_STEIN_06 | 2 | rock | 7,314 | 48,257 | 0.152 | 1.19 |
| NW_MISC_SWAMPWATER_SEA_01 | 5 | water | 1,562 | 41,228 | 0.038 | 8.0 |
| NW_NATURE_GRASS_DECAL_01 | 0 | vegetation | 996 | 35,413 | 0.028 | 9.76 |
| NW_NATURE_BARK_04 | 3 | vegetation | 56,123 | 28,619 | 1.961 | 1.23 |
| NW_NATURE_STEIN_01 | 2 | rock | 2,683 | 25,164 | 0.107 | 3.17 |
| NW_NATURE_GREATSEA_SAND_01 | 4 | ground | 282 | 24,086 | 0.012 | 12.93 |
| NW_NATURE_KIES_01 | 4 | ground | 2,573 | 22,905 | 0.112 | 3.7 |
| NW_MISC_SWAMPWATER_01_SEA_01 | 5 | water | 656 | 21,814 | 0.030 | 9.77 |
| NW_CITY_HAFENKAI_MAUER_01 | 2 | man-made | 270 | 17,491 | 0.015 | 10.04 |
| NW_MISC_CAVEWALL_02 | 2 | rock | 6,736 | 15,813 | 0.426 | 1.12 |
| NW_NATURE_WBODEN_VAR_03 | 2 | ground | 1,378 | 12,332 | 0.112 | 3.5 |
| NW_NATURE_WALDBODEN_TRANS_01 | 4 | ground | 1,230 | 12,280 | 0.100 | 4.49 |
| NW_NATURE_SWEETGRASS_01 | 4 | ground | 633 | 10,030 | 0.063 | 5.72 |
| NW_NATURE_BARK_02 | 3 | vegetation | 2,058 | 9,511 | 0.216 | 7.97 |
| NW_NATURE_PATH_06 | 4 | ground | 1,633 | 9,457 | 0.173 | 3.91 |
| NW_CITY_ROOF_04 | 3 | man-made | 2,585 | 8,697 | 0.297 | 1.93 |
| NW_NATURE_SEEBODEN_01 | 4 | ground | 472 | 7,878 | 0.060 | 6.1 |
| NW_MISC_DUCKWEED_01 | 0 | vegetation | 1,184 | 7,691 | 0.154 | 3.9 |
| NW_NATURE_SEESAND_01 | 4 | ground | 362 | 7,307 | 0.050 | 6.22 |
| NW_MISC_STRONG_STONEWALL_01 | 2 | man-made | 1,265 | 7,114 | 0.178 | 3.31 |
| NW_NATURE_STEIN_08 | 0 | rock | 604 | 6,395 | 0.094 | 4.55 |
| NW_NATURE_TREE_NEEDLE_02 | 0 | vegetation | 1,120 | 5,985 | 0.187 | 3.37 |
| NW_MISC_MAYA_WALL_03 | 2 | man-made | 822 | 5,951 | 0.138 | 4.4 |
| NW_NATURE_ACKER_02 | 4 | ground | 797 | 5,919 | 0.135 | 3.31 |
| NW_CITY_HAFENKAI_BODEN_01 | 2 | man-made | 198 | 5,285 | 0.037 | 7.17 |
| NW_NATURE_DARKWOOD_FELS_01 | 2 | rock | 606 | 4,937 | 0.123 | 3.72 |
| NW_CITY_WOODBEAM_02 | 3 | man-made | 9,872 | 4,195 | 2.353 | 0.52 |
| NW_MISC_MAYA_WALL_01 | 2 | man-made | 264 | 4,135 | 0.064 | 3.61 |


## NEWWORLD: per-chunk triangles

| stat | triangles/chunk | area m2/chunk |
|---|---|---|
| chunks with triangles | 403 |  |
| min | 1 | 362 |
| median | 120 | 14,552 |
| mean | 1174 | 17,410 |
| p90 | 3362 | 32,090 |
| max | 15762 | 75,779 |


### Densest 12 chunks by triangles

| chunk x_z | tris | area m2 | tris/m2 | class shares of area | top materials |
|---|---|---|---|---|---|
| 11_-1 | 15,762 | 43,960 | 0.36 | vegetation 83% | NW_NATURE_TREE_04, NW_NATURE_BRANCH_01 |
| -2_-3 | 15,457 | 29,029 | 0.53 | ground 14%, water 24%, man-made 56% | NW_WATER_LAKE01__OCEAN_CLOSE_01, NW_CITY_HAFENKAI_MAUER_01 |
| 12_3 | 15,217 | 15,378 | 0.99 | ground 26%, man-made 53%, vegetation 12% | NW_NATURE_KIES_01, NW_NATURE_TREE_NEEDLE_03 |
| 13_3 | 12,511 | 8,107 | 1.54 | man-made 68%, vegetation 23% | NW_NATURE_TREE_NEEDLE_03, NW_NATURE_WALDBODEN_03 |
| 0_0 | 11,301 | 20,818 | 0.54 | man-made 76%, vegetation 20% | NW_NATURE_BRANCH_01, NW_MISC_STRONG_STONEWALL_01 |
| 7_3 | 10,918 | 35,712 | 0.31 | rock 15%, man-made 36%, vegetation 48% | NW_NATURE_TREE_NEEDLE_03, NW_NATURE_TREE_04 |
| 1_0 | 9,702 | 20,353 | 0.48 | man-made 71%, vegetation 26% | NW_NATURE_BRANCH_01, NW_MISC_OLDSTONEPATH_01 |
| 1_-1 | 8,586 | 17,117 | 0.50 | man-made 83%, vegetation 15% | NW_NATURE_BRANCH_01, NW_CITY_ROOF_04 |
| 0_-1 | 8,460 | 15,925 | 0.53 | man-made 88%, vegetation 12% | NW_CITY_ROOF_01, NW_CITY_WOODBEAM_02 |
| 4_-3 | 7,377 | 53,234 | 0.14 | rock 10%, man-made 16%, vegetation 66% | NW_NATURE_TREE_04, NW_NATURE_BRANCH_01 |
| 4_-4 | 7,340 | 34,495 | 0.21 | rock 22%, vegetation 62% | NW_NATURE_TREE_NEEDLE_03, NW_MISC_CAVEWALL_02 |
| 12_4 | 7,198 | 22,042 | 0.33 | ground 22%, rock 11%, man-made 14%, vegetation 52% | NW_NATURE_TREE_04, NW_NATURE_WALDBODEN_03 |


### Slope bands, share of surface area in % (angle of the face normal from straight up; "leaning out" = normal y in [-0.5,0), i.e. 90-120 deg, vertical walls tilting down; "ceiling" = normal y < -0.5, i.e. overhang/underside steeper than 60 deg from the horizontal)

| class | part | 0-15 | 15-30 | 30-45 | 45-60 | 60-90 | leaning out | ceiling | area m2 |
|---|---|---|---|---|---|---|---|---|---|
| ground | all | 76.3 | 19.0 | 3.5 | 0.7 | 0.4 | 0.1 | 0.0 | 997,391 |
| ground | land | 65.9 | 26.8 | 5.2 | 1.2 | 0.8 | 0.1 | 0.1 | 438,517 |
| rock | all | 3.6 | 6.9 | 8.8 | 11.4 | 32.8 | 18.5 | 17.9 | 1,681,015 |
| rock | land | 4.9 | 9.0 | 10.3 | 13.5 | 35.0 | 15.3 | 12.1 | 1,203,961 |
| man-made | all | 23.2 | 3.3 | 4.9 | 2.1 | 31.9 | 19.5 | 15.1 | 224,404 |
| man-made | land | 25.3 | 3.6 | 5.3 | 2.3 | 31.2 | 16.7 | 15.6 | 206,037 |


## NEWWORLD: texel density, ten largest ground materials (base .dds; texels per metre = sqrt(uv area x w x h / world area) per triangle, triangles > 0.05 m2)

| material | dds size | area m2 | median texels/m | p10 / p90 | median uv-units per m |
|---|---|---|---|---|---|
| NW_NATURE_GREATSEA_SAND_03 | 512x512 | 538,654 | 16.9 | 16.2 / 17.1 | 0.0331 |
| NW_NATURE_WALDBODEN_03 | 512x512 | 196,077 | 101.8 | 91.7 / 126.9 | 0.1988 |
| NW_NATURE_DARKWOOD_BODEN_01 | 512x512 | 111,110 | 101.4 | 93.1 / 124.7 | 0.1980 |
| NW_NATURE_GREATSEA_SAND_01 | 512x512 | 24,086 | 49.2 | 44.9 / 51.1 | 0.0961 |
| NW_NATURE_KIES_01 | 512x512 | 22,905 | 82.7 | 50.0 / 95.6 | 0.1615 |
| NW_NATURE_WBODEN_VAR_03 | 512x512 | 12,332 | 90.2 | 50.9 / 183.5 | 0.1762 |
| NW_NATURE_WALDBODEN_TRANS_01 | 512x512 | 12,280 | 132.2 | 68.1 / 376.5 | 0.2583 |
| NW_NATURE_SWEETGRASS_01 | 512x512 | 10,030 | 101.1 | 84.6 / 102.3 | 0.1974 |
| NW_NATURE_PATH_06 | 512x512 | 9,457 | 124.6 | 93.2 / 184.8 | 0.2434 |
| NW_NATURE_SEEBODEN_01 | 512x512 | 7,878 | 136.3 | 62.1 / 169.6 | 0.2662 |


## NEWWORLD: layers on a 1 m XZ grid (cell centre, up-facing triangles with normal y>0.1, heights chained: a gap > 2 m starts a new layer)

| set | cells with >=1 layer (= m2) | exactly 1 layer | >=2 layers (gap>2 m) | >=3 layers |
|---|---|---|---|---|
| natural (ground+rock), land | 771,966 | 703,447 (91.1%) | 68,519 (8.9%) | 7,242 |
| solid (ground+rock+man-made), land | 806,099 | 713,548 (88.5%) | 92,551 (11.5%) | 13,250 |
| natural + seabed | 1,343,336 | 1,231,568 (91.7%) | 111,768 (8.3%) | 10,058 |


Multi-layer cells (natural (ground+rock), land): 68,519; gap between lowest and highest layer: <=5 m 9,677, 5-15 m 28,213, >15 m 30,629; cells with an S:*CAVE* room material in the lowest or highest layer: 10,145.

### Where: multi-layer cells, natural (ground+rock), land - top 20 chunks (x_z = floor(metre/64); cave % = share of the two end layers with an S:*CAVE* material)

| chunk | multi-layer cells | median height gap m | cave mats | lowest layer materials | highest layer materials |
|---|---|---|---|---|---|
| 8_-4 | 2199 | 15.7 | 0% | NW_NATURE_WALDBODEN_03 1408, NW_NATURE_STEIN_03 578 | NW_NATURE_STEIN_03 1600, NW_NATURE_PATH_07 380 |
| 12_-2 | 2068 | 17.7 | 0% | NW_NATURE_DARKWOOD_BODEN_01 1473, NW_NATURE_TRANS_PATH_07_TO_DARKW_06 580 | NW_NATURE_WALDBODEN_03 1180, NW_NATURE_WBODEN_VAR_03 329 |
| 7_6 | 1785 | 21.8 | 0% | NW_NATURE_STEIN_07 1586, NW_NATURE_KIES_01 162 | NW_NATURE_STEIN_07 1749, NW_NATURE_STEIN_03 36 |
| 12_5 | 1593 | 17.5 | 30% | OW_SURFACE_WASTELAND_04 947, NW_NATURE_WALDBODEN_03 524 | NW_NATURE_WALDBODEN_03 1487, NW_NATURE_DARKWOOD_PATH_06 56 |
| 4_5 | 1558 | 10.3 | 0% | NW_NATURE_STEIN_07 965, NW_NATURE_STEIN_03 429 | NW_NATURE_STEIN_07 1087, NW_NATURE_STEIN_03 276 |
| -4_0 | 1535 | 12.2 | 0% | NW_NATURE_WALDBODEN_03 656, NW_NATURE_STEIN_02 479 | NW_NATURE_STEIN_07 1313, NW_NATURE_TRANS_KIES_WALDBODEN_01 117 |
| 9_-6 | 1390 | 8.4 | 26% | NW_NATURE_STEIN_06 643, OMWABROWN10 608 | NW_NATURE_STEIN_06 1004, NW_NATURE_WALDBODEN_03 371 |
| 8_-3 | 1303 | 16.8 | 0% | NW_NATURE_WALDBODEN_03 605, NW_NATURE_STEIN_03 541 | NW_NATURE_PATH_07 830, NW_NATURE_STEIN_02 468 |
| 3_-3 | 1045 | 6.6 | 0% | NW_NATURE_WALDBODEN_03 334, NW_NATURE_STEIN_03 321 | NW_MISC_CAVEWALL_02 591, NW_NATURE_STEIN_03 174 |
| 0_2 | 1026 | 18.6 | 39% | OW_SURFACE_WASTELAND_04 779, NW_NATURE_DARKWOOD_BODEN_01 103 | NW_NATURE_WALDBODEN_03 607, NW_NATURE_PATH_06 101 |
| 3_-2 | 1012 | 22.7 | 9% | NW_NATURE_WALDBODEN_03 444, OW_SURFACE_WASTELAND_04 187 | NW_NATURE_STEIN_03 717, NW_NATURE_WALDBODEN_03 126 |
| 4_-1 | 956 | 25.5 | 0% | NW_NATURE_STEIN_03 306, NW_NATURE_WALDBODEN_03 283 | NW_NATURE_STEIN_03 634, NW_NATURE_WALDBODEN_03 236 |
| 6_4 | 941 | 10.0 | 4% | NW_NATURE_STEIN_03 637, NW_NATURE_STEIN_07 147 | NW_NATURE_STEIN_07 769, NW_NATURE_STEIN_03 99 |
| 4_-4 | 910 | 14.4 | 10% | NW_NATURE_SMALL_STREAM_GROUND_01 231, OW_SURFACE_WASTELAND_04 220 | NW_NATURE_STEIN_03 304, NW_MISC_CAVEWALL_02 297 |
| 4_-2 | 903 | 17.2 | 15% | NW_NATURE_STEIN_03 435, OW_SURFACE_WASTELAND_04 261 | NW_NATURE_STEIN_03 823, NW_NATURE_WALDBODEN_03 30 |
| 8_6 | 900 | 20.0 | 0% | NW_NATURE_STEIN_07 464, NW_NATURE_STEIN_03 124 | NW_NATURE_STEIN_07 465, NW_NATURE_STEIN_03 394 |
| 7_-1 | 887 | 37.0 | 36% | OW_SURFACE_WASTELAND_04 630, NW_NATURE_STEIN_03 148 | NW_NATURE_WALDBODEN_03 489, NW_NATURE_STEIN_03 166 |
| 5_-5 | 884 | 21.7 | 0% | NW_NATURE_KIES_01 549, NW_NATURE_STEIN_03 102 | NW_NATURE_STEIN_03 339, NW_NATURE_STEIN_08 326 |
| 13_-2 | 867 | 9.5 | 24% | OW_VOLCANO_GROUND_01 418, NW_NATURE_STEIN_03 256 | NW_NATURE_DARKWOOD_BODEN_01 318, NW_NATURE_WBODEN_VAR_03 249 |
| -2_-2 | 848 | 34.8 | 0% | NW_NATURE_GREATSEA_SAND_01 625, NW_NATURE_STEIN_03 223 | NW_NATURE_STEIN_03 844, NW_NATURE_GREATSEA_SAND_01 4 |


Chunks with at least one multi-layer cell: 201; chunks with >= 500 cells: 53.


Multi-layer cells (solid (ground+rock+man-made), land): 92,551; gap between lowest and highest layer: <=5 m 13,668, 5-15 m 42,832, >15 m 36,051; cells with an S:*CAVE* room material in the lowest or highest layer: 10,753.

### Where: multi-layer cells, solid (ground+rock+man-made), land - top 20 chunks (x_z = floor(metre/64); cave % = share of the two end layers with an S:*CAVE* material)

| chunk | multi-layer cells | median height gap m | cave mats | lowest layer materials | highest layer materials |
|---|---|---|---|---|---|
| 0_0 | 2252 | 9.4 | 0% | NW_CITY_DUNGEONFLOOR_02 719, NW_CITY_HAFENKAI_BODEN_01 474 | NW_CITY_ROOF_01 623, NW_CITY_ROOF_04 516 |
| 8_-4 | 2203 | 15.7 | 0% | NW_NATURE_WALDBODEN_03 1412, NW_NATURE_STEIN_03 578 | NW_NATURE_STEIN_03 1600, NW_NATURE_PATH_07 380 |
| 12_-2 | 2076 | 17.7 | 0% | NW_NATURE_DARKWOOD_BODEN_01 1481, NW_NATURE_TRANS_PATH_07_TO_DARKW_06 580 | NW_NATURE_WALDBODEN_03 1180, NW_NATURE_WBODEN_VAR_03 329 |
| 12_3 | 2002 | 24.5 | 0% | NW_SEQ_MAYA_GROUND_GREEN_01 968, NW_SEQ_MAYA_GROUND_03 509 | NW_NATURE_WALDBODEN_03 806, NW_NATURE_KIES_01 476 |
| 7_3 | 1810 | 15.7 | 0% | NW_CITY_HAFENKAI_BODEN_01 559, NW_MONASTERY_GROUND_04 415 | NW_MONASTERY_ROOF_04 1264, NW_NATURE_DARKWOOD_FELS_VAR_01 149 |
| 7_6 | 1785 | 21.8 | 0% | NW_NATURE_STEIN_07 1586, NW_NATURE_KIES_01 162 | NW_NATURE_STEIN_07 1749, NW_NATURE_STEIN_03 36 |
| 0_-1 | 1627 | 5.2 | 0% | NW_CITY_BODENDIELEN_01 379, NW_CITY_BODENDIELEN_DRECK_01 156 | NW_CITY_ROOF_01 1211, NW_CITY_ROOF_04 211 |
| 1_0 | 1599 | 9.1 | 0% | NW_CITY_DUNGEONFLOOR_02 589, NW_CITY_INSIDEFLOOR_11 283 | NW_CITY_ROOF_01 543, NW_CITY_ROOF_04 427 |
| 12_5 | 1593 | 17.5 | 30% | OW_SURFACE_WASTELAND_04 947, NW_NATURE_WALDBODEN_03 524 | NW_NATURE_WALDBODEN_03 1487, NW_NATURE_DARKWOOD_PATH_06 56 |
| 11_4 | 1590 | 6.5 | 0% | NW_NATURE_KIES_01 1525, NW_NATURE_WALDBODEN_03 24 | NW_MISC_MAYA_WALL_03 871, NW_HARBOUR_STAIRS_01 314 |
| 4_5 | 1558 | 10.3 | 0% | NW_NATURE_STEIN_07 965, NW_NATURE_STEIN_03 429 | NW_NATURE_STEIN_07 1087, NW_NATURE_STEIN_03 276 |
| -4_0 | 1535 | 12.4 | 0% | NW_NATURE_WALDBODEN_03 644, NW_NATURE_STEIN_02 479 | NW_NATURE_STEIN_07 1313, NW_NATURE_TRANS_KIES_WALDBODEN_01 117 |
| 11_-2 | 1396 | 6.3 | 0% | NW_CITY_BODENDIELEN_02 592, NW_CITY_BODENDIELEN_01 362 | NW_CITY_ROOF_04 762, NW_MISC_ROOF_01 531 |
| 9_-6 | 1392 | 8.3 | 26% | NW_NATURE_STEIN_06 643, OMWABROWN10 609 | NW_NATURE_STEIN_06 1005, NW_NATURE_WALDBODEN_03 371 |
| 12_4 | 1329 | 19.9 | 3% | NW_SEQ_MAYA_GROUND_03 501, NW_NATURE_WALDBODEN_03 246 | NW_NATURE_WALDBODEN_03 774, NW_NATURE_KIES_01 219 |
| 8_-3 | 1309 | 16.8 | 0% | NW_NATURE_WALDBODEN_03 609, NW_NATURE_STEIN_03 541 | NW_NATURE_PATH_07 830, NW_NATURE_STEIN_02 469 |
| -2_-2 | 1264 | 30.3 | 0% | NW_NATURE_GREATSEA_SAND_01 589, NW_CITY_HAFENKAI_BODEN_01 348 | NW_NATURE_STEIN_03 1127, NW_CITY_WOODBEAM_03 51 |
| 1_-1 | 1151 | 8.5 | 0% | NW_CITY_INSIDEFLOOR_11 275, NW_MISC_OLDSTONEPATH_01 218 | NW_CITY_ROOF_04 599, NW_CITY_ROOF_01 382 |
| 7_2 | 1103 | 15.9 | 0% | NW_MONASTERY_WALL_83 462, NW_MONASTERY_WALL_82 189 | NW_MONASTERY_ROOF_04 622, NW_NATURE_STEIN_03 227 |
| -2_-3 | 1102 | 5.0 | 0% | KOMBUESE_NW_CITY_INSIDEFLOOR_11 335, NW_CITY_INSIDEFLOOR_11 287 | NW_CITY_INSIDEFLOOR_11 534, NW_CITY_HAFENKAI_BODEN_01 166 |


Chunks with at least one multi-layer cell: 218; chunks with >= 500 cells: 69.


## NEWWORLD: vobs (vobs.json, whole tree incl. children)
Total vob objects: 23,288; with a mesh/model visual (names not .TGA/.PFX/.WAV): 12,479.
| class | instances | distinct meshes | instances with readable mesh | triangles of those instances | mean tris/instance |
|---|---|---|---|---|---|
| trees | 1,643 | 22 | 1,632 | 244,667 | 150 |
| trunks/logs | 117 | 2 | 117 | 5,278 | 45 |
| bushes | 1,652 | 11 | 1,652 | 252,195 | 153 |
| plants/grass/ferns | 2,479 | 20 | 2,470 | 444,428 | 180 |
| rocks | 708 | 26 | 689 | 119,033 | 173 |
| other static visuals | 5,880 | 314 | 4,988 | 1,048,054 | 210 |

### Top meshes: trees

| visual | instances | mesh triangles |
|---|---|---|
| NW_NATURE_TANNE_75P.3DS | 389 | 75 |
| OW_NATURE_SMALLTREE_01.3DS | 286 | 191 |
| NW_NATURE_TANNE_100P.3DS | 209 | 99 |
| NW_NATURE_SMALLTREE_79P.3DS | 130 | 79 |
| NW_NATURE_BIGTREE_356P.3DS | 100 | 351 |
| NW_NATURE_HOHETANNE_59P.3DS | 88 | 59 |
| NW_NATURE_HOHETANNE_91P.3DS | 82 | 91 |
| NW_NATURE_BAUMSTUMPF_02_115P.3DS | 51 | 114 |
| NW_NATURE_SMALLTREE_103P.3DS | 45 | 103 |
| NW_NATURE_BAUMSTUMPF_03_174P.3DS | 42 | 174 |
| NW_NATURE_HOHETANNEN_01_531P.3DS | 36 | 531 |
| OW_LOB_TREE_V11.3DS | 31 | 159 |

### Top meshes: trunks/logs

| visual | instances | mesh triangles |
|---|---|---|
| NW_NATURE_BAUMSTAMM_01_30P.3DS | 104 | 30 |
| NW_NATURE_BAUMSTAMM_166P.3DS | 13 | 166 |

### Top meshes: bushes

| visual | instances | mesh triangles |
|---|---|---|
| NW_NATURE_BUSH_120P.3DS | 690 | 120 |
| NW_NATURE_BUSH_25P.3DS | 168 | 26 |
| NW_NATURE_BUSH_56P.3DS | 164 | 56 |
| NW_NATURE_LONG_BUSH_360P.3DS | 156 | 360 |
| NW_NATURE_BUSH_300P.3DS | 115 | 320 |
| OW_NATURE_BUSH_02.3DS | 104 | 400 |
| OW_NATURE_BUSH_01.3DS | 92 | 20 |
| NW_NATURE_BUSH_60P.3DS | 63 | 60 |
| OW_LOB_BUSH_V5.3DS | 39 | 48 |
| OW_LOB_BUSH_REED_V1.3DS | 32 | 218 |
| NW_NATURE_BUSH_01.3DS | 29 | 235 |

### Top meshes: plants/grass/ferns

| visual | instances | mesh triangles |
|---|---|---|
| NW_NATURE_GRASSGROUP_01.3DS | 688 | 40 |
| NW_NATURE_FARNTEPPICH_306P.3DS | 565 | 306 |
| NW_NATURE_SIDEPLANT_01.3DS | 202 | 10 |
| OW_MUSHROOM_V2.3DS | 133 | 660 |
| NW_NATURE_FARN_SMALL_102P.3DS | 121 | 102 |
| NW_NATURE_FARN_102P.3DS | 101 | 102 |
| NW_NATURE_SIDEPLANT_BIG_01.3DS | 97 | 10 |
| NW_NATURE_HANGPLANTS_01.3DS | 95 | 192 |
| NW_NATURE_FARN_BIG_01.3DS | 91 | 216 |
| NW_NATURE_PLANT_03.3DS | 79 | 112 |
| NW_NATURE_FARNTEPPICH_612P.3DS | 72 | 612 |
| NW_NATURE_PLANT_02.3DS | 63 | 324 |

### Top meshes: rocks

| visual | instances | mesh triangles |
|---|---|---|
| MIN_LOB_COUPLESTONES.3DS | 236 | 261 |
| NW_NATURE_STONE_BIGHIGH_01.3DS | 85 | 40 |
| NW_CITY_WINDOW_STONE_OUT_01.3DS | 43 | 162 |
| NW_NATURE_STONE_CULT_STAND_01.3DS | 36 | 120 |
| NW_NATURE_STONE_BIGFLAT_02.3DS | 33 | 40 |
| NW_MISC_GRAVESTONE_02.3DS | 31 | 148 |
| ADDON_DUNGEON_BROKENSTONE_03.3DS | 29 | 70 |
| NW_NATURE_STONE_BIGFLAT_01.3DS | 24 | 40 |
| NW_NATURE_STONE_MANYSTONES_01.3DS | 22 | 480 |
| TOUCHPLATE_STONE.MDS | 19 | n/a |
| NW_NATURE_STONE_CULT_TOP_01.3DS | 19 | 120 |
| NW_NATURE_STONE_CULT_PART_02.3DS | 19 | 318 |

### Top meshes: other static visuals

| visual | instances | mesh triangles |
|---|---|---|
| NW_CAVEWEBS_V201.3DS | 455 | 20 |
| NW_CITY_OILLAMP_01.3DS | 223 | 138 |
| NW_NATURE_FENCE_01.3DS | 167 | 60 |
| NW_CITY_CANDLE_01.3DS | 157 | 124 |
| NW_HARBOUR_BARREL_01.3DS | 150 | 60 |
| OW_MISC_WALL_TORCH_01.3DS | 145 | 134 |
| NW_MISC_SPIDERWEBS_01.3DS | 140 | 256 |
| NW_HARBOUR_CRATEPILE_01.3DS | 132 | 135 |
| ORC_SKULLSONFLOOR.3DS | 107 | 232 |
| NW_HARBOUR_CRATE_01.3DS | 105 | 12 |
| CHAIR_NW_NORMAL_01.ASC | 104 | n/a |
| BEDHIGH_NW_NORMAL_01.ASC | 103 | n/a |
| ORC_SKELETON_V3.3DS | 98 | 816 |
| OC_SAECKE_01.3DS | 98 | 252 |
| NW_HARBOUR_BARRELGROUP_02.3DS | 97 | 420 |

## What existing reports already state (quoted, not re-measured)

From `reports/performance/2026-11-10-content-stress/README.md` (Ryzen 3950X, RTX 3080 10 GB, Vulkan, Khorinis without own content, original sets):
- "Гавань: треугольников за кадр, млн: 0,68" for the original set (counter counts all passes and NPCs); forest "0,62"; the same report's reference section says "Сейчас у гавани около 1,26 млн треугольников на кадр по всем проходам; LOD геометрии нет, рисуется всё до 1 км."
- Memory on the route: textures 81 MiB, GPU memory 1040 MB, landscape pages on GPU 40 MiB, live malloc after load 639 MiB; with geometry x4: 1 168 MB GPU, landscape pages 90 MiB; x16: 1 680 MB, 300 MiB, 4.24 M triangles per frame in the harbour, frame p50 7.6 ms in the harbour and unchanged (CPU-bound: world tick 3.9 ms, animation 2.4 ms, command recording 2.0 ms).
- "Текстуры оригинала — в основном 256–512 px; востребованный набор Хориниса около 303 МиБ при бюджете 256 МиБ и цели 192 МиБ." Target hardware "RTX 3080 10 ГБ, 16,6 мс кадра при 1440p".
- Base meshes (`meshes/`, 1 351 meshes): "230 395 треугольников, `meshes/` 25 МБ"; world chunk reading ~1.1 us per triangle, whole world read at start (544 ms for the original).
- Format limits: world chunk triangle numbers are floats exact to 2^24; a mesh holds at most 65 536 positions/wedges per submesh.

From `reports/performance/2026-10-30-content-cost/none-vs-working-8/report.md` (drawable 1728x1117): triangles per frame harbour 1 123 291 (no content), market 1 057 692; p50 9.6 ms and 10.0 ms (an older measurement on a different build, so not directly comparable to the 7.6 ms above).

Cross-check: the 0.68 M triangles per frame in the harbour is about 1.4 times the entire NEWWORLD mesh (473 502); the counter includes shadow/depth passes and vobs.

## Not obtained / limits

- Projected (screen) surface of the vegetation shells and which part of the world mesh is actually drawn per frame: not measured here (no game launch).
- No visual check of the multi-layer locations or of the vegetation material interpretation; descriptions are from material names only.
- ADDONWORLD classes are rough (name rules guessed), OLDWORLD moderate; their vobs were not counted.
- Texel density assumes UVs in 0..1; it is the base texture, not the remaster override; `texture_scale` / `default_mapping` fields in `world.json` (for example `[512,512]`, `[2.34375, 2.34375]`) were not used.
- Layer test misses sub-cell triangles and ignores vertical faces; a 2 m threshold and normal y > 0.1 were my choice.
- Raw per-cell layer data for NEWWORLD is in `NEWWORLD_layers.npz` next to the scripts.

# Appendix: OLDWORLD

## OLDWORLD: totals

Sea level used to split land from seabed: y = -15.4 m (median centroid height of the largest water material).

| quantity | value |
|---|---|
| triangles | 87,963 |
| vertices (world.json, engine level) | 48,332 |
| unique positions (float32) | 47,955 |
| bbox min x,y,z (m, Gothic axes) | [-767.5, -159.7, -599.0] |
| bbox max x,y,z (m, Gothic axes) | [1090.0, 200.1, 674.0] |
| 64 m chunk files | 429 |
| surface area of all triangles (m2) | 3,473,693 |
| plan area of up-facing triangles, sum(A*ny) (m2) | 1,900,307 |
| zero-area triangles | 27 |


### By material class

| class | triangles | area m2 | plan (up-facing) m2 | tris per m2 | tris below sea (below sea level) |
|---|---|---|---|---|---|
| ground | 11,466 | 268,647 | 242,573 | 0.043 | 434 |
| rock | 49,415 | 1,519,933 | 845,825 | 0.033 | 1,382 |
| water | 3,264 | 1,587,013 | 787,970 | 0.002 | 1,021 |
| man-made | 19,132 | 78,532 | 14,835 | 0.244 | 42 |
| vegetation | 1,880 | 6,305 | 2,306 | 0.298 | 127 |
| other | 2,806 | 13,263 | 6,798 | 0.212 | 38 |


### Ground and rock split by sea level

| class | part | triangles | area m2 | tris per m2 | plan m2 |
|---|---|---|---|---|---|
| ground | land (above sea level) | 11,032 | 234,314 | 0.047 | 209,888 |
| ground | below sea | 434 | 34,334 | 0.013 | 32,685 |
| rock | land (above sea level) | 48,033 | 1,465,634 | 0.033 | 821,760 |
| rock | below sea | 1,382 | 54,299 | 0.025 | 24,066 |


### Edge length (m) and triangle area per class

| class | part | tris | edge p10 | edge p50 | edge p90 | median tri area m2 | sqrt(2*median area) m |
|---|---|---|---|---|---|---|---|
| ground | all | 11,466 | 1.74 | 4.92 | 13.95 | 8.209 | 4.05 |
| ground | land | 11,032 | 1.7 | 4.83 | 13.56 | 7.819 | 3.95 |
| rock | all | 49,415 | 0.97 | 4.45 | 14.48 | 6.763 | 3.68 |
| rock | land | 48,033 | 0.94 | 4.38 | 14.34 | 6.541 | 3.62 |
| man-made | all | 19,132 | 0.48 | 2.58 | 7.84 | 1.416 | 1.68 |
| man-made | land | 19,090 | 0.48 | 2.58 | 7.84 | 1.416 | 1.68 |
| vegetation | all | 1,880 | 0.44 | 2.63 | 5.54 | 1.762 | 1.88 |
| vegetation | land | 1,753 | 0.44 | 2.53 | 5.33 | 1.504 | 1.73 |


### 40 largest materials by area

| material | group | class | tris | area m2 | tris/m2 | median edge m |
|---|---|---|---|---|---|---|
| NW_MISC_SWAMPWATER_SEA_01 | 5 | water | 1,832 | 1,493,612 | 0.001 | 27.57 |
| OW_SURFACE_MOUNTAIN_01 | 2 | rock | 10,353 | 413,481 | 0.025 | 6.2 |
| OW_SURFACE_MOUNTAINDARK_02 | 2 | rock | 1,753 | 299,298 | 0.006 | 14.6 |
| OW_SURFACE_GRASSSTONES_06 | 4 | rock | 4,594 | 218,827 | 0.021 | 7.13 |
| OW_ICEDRAGON_MOUNTAIN_04 | 2 | rock | 1,467 | 141,490 | 0.010 | 12.17 |
| OW_ICEDRAGON_SNOW_03 | 6 | ground | 4,634 | 98,568 | 0.047 | 4.57 |
| OW_ICEDRAGON_MOUNTAIN_05 | 2 | rock | 971 | 79,479 | 0.012 | 9.26 |
| OW_ICEDRAGON_GROUND_02 | 4 | ground | 671 | 45,421 | 0.015 | 9.71 |
| OW_SURFACE_WASTELAND_04 | 4 | rock | 1,186 | 41,845 | 0.028 | 8.01 |
| OW_SURFACE_ROCK_02 | 2 | rock | 4,021 | 39,406 | 0.102 | 2.75 |
| OW_ICEDRAGON_MOUNTAIN_02 | 2 | rock | 370 | 33,358 | 0.011 | 10.02 |
| OW_SURFACE_WASTELAND_01 | 4 | rock | 1,068 | 33,203 | 0.032 | 7.62 |
| SWAMP_A0 | 5 | water | 572 | 29,601 | 0.019 | 7.92 |
| OW_STONEDRAGON_GROUNDSTONES_13 | 2 | rock | 619 | 28,027 | 0.022 | 5.38 |
| OWODFLSTRAND | 4 | ground | 111 | 26,895 | 0.004 | 19.28 |
| OW_SURFACE_DRYGROUND_01 | 4 | ground | 846 | 25,981 | 0.033 | 8.46 |
| OW_STONEDRAGON_CAVEWALL_10 | 2 | rock | 1,433 | 24,035 | 0.060 | 3.57 |
| OW_VOLCANO_GROUND_01 | 0 | rock | 710 | 19,828 | 0.036 | 6.73 |
| OWODWFALL_A0 | 0 | water | 348 | 18,722 | 0.019 | 12.06 |
| OWODFLBRETTER | 3 | man-made | 4,731 | 18,635 | 0.254 | 4.41 |
| OWODWFALL_A0_COLLISION | 0 | water | 38 | 15,914 | 0.002 | 32.48 |
| OWODWAMOUNTAINCLOSE | 2 | rock | 260 | 15,195 | 0.017 | 4.74 |
| LAKE_ENV_MAP_AND_LOOKTHROUGH_01 | 0 | water | 329 | 14,289 | 0.023 | 9.01 |
| OW_ICEDRAGON_LAKE_01 | 2 | ground | 341 | 12,381 | 0.028 | 8.56 |
| OW_SWAMPDRAGON_MOUNTAIN_03 | 2 | rock | 866 | 12,254 | 0.071 | 5.26 |
| OW_FIREDRAGON_VOLCANO_01 | 0 | rock | 801 | 11,490 | 0.070 | 5.07 |
| OW_ICEDRAGON_GROUND_01 | 6 | ground | 248 | 10,955 | 0.023 | 9.72 |
| OW_FIREDRAGON_GROUND_01 | 2 | ground | 942 | 10,410 | 0.090 | 4.72 |
| OW_FOREST_FORESTGROUND_00 | 4 | ground | 807 | 9,841 | 0.082 | 5.05 |
| OW_OLDCAMP_WALL_17 | 2 | man-made | 1,330 | 9,420 | 0.141 | 3.62 |
| OW_SURFACE_TRANS_20 | 2 | rock | 852 | 9,312 | 0.091 | 4.64 |
| OWODSEA_A0 | 5 | water | 11 | 7,523 | 0.001 | 39.85 |
| OW_ICEDRAGON_ICECAVE_01 | 6 | rock | 1,091 | 6,746 | 0.162 | 2.61 |
| OW_SURFACE_TRANS_13 | 4 | rock | 178 | 6,570 | 0.027 | 9.09 |
| OW_SURFACE_TRANS_14 | 4 | rock | 366 | 6,416 | 0.057 | 4.95 |
| OW_OLDCAMP_WALL_15 | 2 | man-made | 935 | 6,271 | 0.149 | 3.35 |
| OW_SURFACE_PATHGREENSTONES_01 | 4 | ground | 503 | 5,935 | 0.085 | 5.13 |
| OW_STONEDRAGON_GROUND_05 | 2 | ground | 215 | 5,456 | 0.039 | 6.98 |
| OW_SURFACE_TRANS_07 | 4 | rock | 227 | 4,612 | 0.049 | 6.08 |
| OW_FIREDRAGON_VOLCANO_02 | 2 | rock | 1,842 | 4,490 | 0.410 | 2.13 |


## OLDWORLD: per-chunk triangles

| stat | triangles/chunk | area m2/chunk |
|---|---|---|
| chunks with triangles | 429 |  |
| min | 1 | 162 |
| median | 16 | 7,722 |
| mean | 205 | 8,097 |
| p90 | 599 | 13,593 |
| max | 3046 | 24,907 |


### Densest 12 chunks by triangles

| chunk x_z | tris | area m2 | tris/m2 | class shares of area | top materials |
|---|---|---|---|---|---|
| 0_-4 | 3,046 | 12,307 | 0.25 | rock 74%, water 19% | OW_FIREDRAGON_ VOLCANO_03, NW_MISC_SWAMPWATER_SEA_01 |
| 0_-1 | 3,006 | 12,394 | 0.24 | ground 15%, rock 12%, man-made 59% | OW_FIREDRAGON_GROUND_01, OW_OLDCAMP_WALL_17 |
| -2_-6 | 2,959 | 16,065 | 0.18 | rock 39%, water 14%, man-made 41% | OW_SURFACE_MOUNTAIN_01, SWAMP_A0 |
| -9_1 | 2,698 | 15,802 | 0.17 | ground 64%, rock 24%, water 10% | OW_ICEDRAGON_SNOW_03, LAKE_ENV_MAP_AND_LOOKTHROUGH_01 |
| -5_3 | 2,656 | 11,121 | 0.24 | ground 22%, rock 68% | OW_SURFACE_ROCK_02, OW_SURFACE_MOUNTAIN_01 |
| -1_-5 | 2,473 | 10,095 | 0.24 | rock 84% | OW_SURFACE_MOUNTAIN_01, OW_SURFACE_TRANS_14 |
| -1_-1 | 2,401 | 20,050 | 0.12 | rock 10%, man-made 79% | OW_FIREDRAGON_GROUND_01, OW_OLDCAMP_WALL_17 |
| -9_0 | 2,166 | 18,552 | 0.12 | ground 35%, rock 64% | OW_ICEDRAGON_SNOW_03, OW_ICEDRAGON_MOUNTAIN_04 |
| -1_0 | 1,703 | 12,078 | 0.14 | rock 22%, man-made 68% | OW_OLDCAMP_WALL_17, OW_OLDCAMP_ROOF_02 |
| 3_-7 | 1,687 | 18,319 | 0.09 | ground 18%, rock 70%, man-made 12% | OW_SURFACE_MOUNTAINDARK_02, OW_FIREDRAGON_VOLCANO_01 |
| 3_0 | 1,526 | 10,424 | 0.15 | ground 14%, rock 30%, water 27%, man-made 29% | NW_MISC_SWAMPWATER_SEA_01, OWODFLBRETTER |
| 1_5 | 1,510 | 8,402 | 0.18 | rock 80% | OW_SURFACE_MOUNTAIN_01, NW_MISC_SWAMPWATER_SEA_01 |


### Slope bands, share of surface area in % (angle of the face normal from straight up; "leaning out" = normal y in [-0.5,0), i.e. 90-120 deg, vertical walls tilting down; "ceiling" = normal y < -0.5, i.e. overhang/underside steeper than 60 deg from the horizontal)

| class | part | 0-15 | 15-30 | 30-45 | 45-60 | 60-90 | leaning out | ceiling | area m2 |
|---|---|---|---|---|---|---|---|---|---|
| ground | all | 50.2 | 31.7 | 9.4 | 4.1 | 3.6 | 0.5 | 0.6 | 268,647 |
| ground | land | 49.3 | 30.6 | 10.2 | 4.5 | 4.1 | 0.6 | 0.6 | 234,314 |
| rock | all | 13.7 | 15.3 | 13.5 | 15.5 | 31.0 | 8.2 | 2.8 | 1,519,933 |
| rock | land | 13.7 | 15.3 | 13.9 | 15.9 | 30.2 | 8.2 | 2.8 | 1,465,634 |
| man-made | all | 10.0 | 3.6 | 2.2 | 1.0 | 47.2 | 23.9 | 11.9 | 78,532 |
| man-made | land | 9.7 | 3.7 | 2.2 | 1.0 | 47.4 | 24.0 | 11.9 | 78,200 |


## OLDWORLD: texel density, ten largest ground materials (base .dds; texels per metre = sqrt(uv area x w x h / world area) per triangle, triangles > 0.05 m2)

| material | dds size | area m2 | median texels/m | p10 / p90 | median uv-units per m |
|---|---|---|---|---|---|
| OW_ICEDRAGON_SNOW_03 | 512x512 | 98,568 | 49.5 | 43.9 / 51.2 | 0.0966 |
| OW_ICEDRAGON_GROUND_02 | 512x512 | 45,421 | 52.5 | 41.5 / 93.4 | 0.1024 |
| OWODFLSTRAND | 256x256 | 26,895 | 32.4 | 26.7 / 42.3 | 0.1267 |
| OW_SURFACE_DRYGROUND_01 | 512x512 | 25,981 | 65.1 | 52.9 / 84.8 | 0.1271 |
| OW_ICEDRAGON_LAKE_01 | 512x512 | 12,381 | 43.1 | 43.1 / 43.1 | 0.0842 |
| OW_ICEDRAGON_GROUND_01 | 512x512 | 10,955 | 29.6 | 28.9 / 65.7 | 0.0578 |
| OW_FIREDRAGON_GROUND_01 | 512x512 | 10,410 | 56.3 | 49.4 / 85.2 | 0.1099 |
| OW_FOREST_FORESTGROUND_00 | 512x512 | 9,841 | 99.0 | 93.4 / 99.9 | 0.1933 |
| OW_SURFACE_PATHGREENSTONES_01 | 512x512 | 5,935 | 84.5 | 64.0 / 130.3 | 0.1651 |
| OW_STONEDRAGON_GROUND_05 | 512x512 | 5,456 | 51.5 | 47.0 / 89.1 | 0.1007 |


## OLDWORLD: layers on a 1 m XZ grid (cell centre, up-facing triangles with normal y>0.1, heights chained: a gap > 2 m starts a new layer)

| set | cells with >=1 layer (= m2) | exactly 1 layer | >=2 layers (gap>2 m) | >=3 layers |
|---|---|---|---|---|
| natural (ground+rock), land | 935,026 | 871,791 (93.2%) | 63,235 (6.8%) | 6,554 |
| solid (ground+rock+man-made), land | 938,550 | 870,134 (92.7%) | 68,416 (7.3%) | 9,085 |
| natural + seabed | 986,618 | 918,958 (93.1%) | 67,660 (6.9%) | 6,733 |


Multi-layer cells (natural (ground+rock), land): 63,235; gap between lowest and highest layer: <=5 m 10,526, 5-15 m 19,382, >15 m 33,327; cells with an S:*CAVE* room material in the lowest or highest layer: 125.

### Where: multi-layer cells, natural (ground+rock), land - top 20 chunks (x_z = floor(metre/64); cave % = share of the two end layers with an S:*CAVE* material)

| chunk | multi-layer cells | median height gap m | cave mats | lowest layer materials | highest layer materials |
|---|---|---|---|---|---|
| -9_1 | 2568 | 8.9 | 0% | OW_ICEDRAGON_SNOW_03 1876, OW_ICEDRAGON_LAKE_01 495 | OW_ICEDRAGON_SNOW_03 2458, OW_ICEDRAGON_MOUNTAIN_03 64 |
| 3_-7 | 2565 | 71.8 | 0% | OW_FIREDRAGON_GROUND_01 1558, OW_STONEDRAGON_GROUNDSTONES_15 559 | OW_SURFACE_MOUNTAINDARK_02 2562, OW_FIREDRAGON_GROUND_01 3 |
| -9_0 | 2558 | 71.9 | 0% | OW_ICEDRAGON_SNOW_03 2501, OW_ICEDRAGON_MOUNTAIN_04 36 | OW_ICEDRAGON_SNOW_03 1836, OW_ICEDRAGON_MOUNTAIN_04 522 |
| 3_-5 | 2088 | 20.1 | 0% | OW_SURFACE_WASTELAND_04 1414, OW_STONEDRAGON_GROUNDSTONES_13 601 | OW_SURFACE_MOUNTAIN_01 1433, OW_SURFACE_MOUNTAINDARK_02 266 |
| -5_4 | 2046 | 11.1 | 0% | OW_ICEDRAGON_GROUND_02 1297, OW_SURFACE_ROCK_02 521 | OW_SURFACE_ROCK_02 1302, OW_SURFACE_MOUNTAIN_01 400 |
| -9_2 | 1926 | 5.7 | 0% | OW_ICEDRAGON_LAKE_01 1251, OW_ICEDRAGON_SNOW_03 627 | OW_ICEDRAGON_SNOW_03 1708, OW_ICEDRAGON_MOUNTAIN_02 192 |
| -10_0 | 1643 | 72.9 | 0% | OW_ICEDRAGON_SNOW_03 1168, OW_ICEDRAGON_MOUNTAIN_04 365 | OW_ICEDRAGON_SNOW_03 1039, OW_ICEDRAGON_MOUNTAIN_04 431 |
| -10_1 | 1596 | 7.4 | 0% | OW_ICEDRAGON_SNOW_03 1385, OW_ICEDRAGON_LAKE_01 145 | OW_ICEDRAGON_SNOW_03 1064, OW_ICEDRAGON_MOUNTAIN_04 234 |
| -6_1 | 1495 | 31.1 | 0% | OW_ICEDRAGON_SNOW_03 1416, OW_ICEDRAGON_LAKE_01 38 | OW_ICEDRAGON_MOUNTAIN_04 1277, OW_ICEDRAGON_SNOW_03 102 |
| -5_-5 | 1494 | 36.9 | 0% | OWODWAMOUNTAINCLOSE 657, OW_SURFACE_ROCK_02 554 | OW_SURFACE_MOUNTAINDARK_02 1061, OW_SURFACE_MOUNTAIN_01 248 |
| -7_1 | 1415 | 4.9 | 0% | OW_ICEDRAGON_SNOW_03 1288, OW_ICEDRAGON_LAKE_01 78 | OW_ICEDRAGON_SNOW_03 1133, OW_ICEDRAGON_MOUNTAIN_04 280 |
| -9_3 | 1399 | 6.3 | 0% | OW_ICEDRAGON_SNOW_03 1322, OW_ICEDRAGON_MOUNTAIN_04 71 | OW_ICEDRAGON_SNOW_03 1162, OW_ICEDRAGON_MOUNTAIN_04 173 |
| -5_7 | 1299 | 14.7 | 0% | OW_SURFACE_MOUNTAIN_01 1158, OW_ICEDRAGON_MOUNTAIN_05 141 | OW_SURFACE_MOUNTAIN_01 722, OW_ICEDRAGON_MOUNTAIN_05 307 |
| -8_1 | 1207 | 4.4 | 0% | OW_ICEDRAGON_SNOW_03 1141, OW_ICEDRAGON_MOUNTAIN_04 39 | OW_ICEDRAGON_SNOW_03 977, OW_ICEDRAGON_MOUNTAIN_04 173 |
| -7_4 | 1108 | 18.8 | 0% | OW_ICEDRAGON_MOUNTAIN_04 976, OW_ICEDRAGON_MOUNTAIN_05 132 | OW_ICEDRAGON_MOUNTAIN_05 976, OW_ICEDRAGON_MOUNTAIN_04 132 |
| -8_2 | 1105 | 4.8 | 0% | OW_ICEDRAGON_SNOW_03 930, OW_ICEDRAGON_MOUNTAIN_04 112 | OW_ICEDRAGON_SNOW_03 870, OW_ICEDRAGON_MOUNTAIN_04 161 |
| -6_2 | 1096 | 10.4 | 0% | OW_ICEDRAGON_SNOW_03 922, OW_SURFACE_DRYGROUND_01A 106 | OW_ICEDRAGON_MOUNTAIN_04 722, OW_ICEDRAGON_GROUND_01 137 |
| 3_-6 | 1094 | 50.2 | 0% | OW_STONEDRAGON_GROUNDSTONES_15 835, OW_SURFACE_MOUNTAINDARK_02 75 | OW_SURFACE_MOUNTAINDARK_02 937, OW_STONEDRAGON_GROUNDSTONES_15 122 |
| -7_-1 | 1075 | 32.6 | 0% | OW_ICEDRAGON_LAKE_01 576, OW_ICEDRAGON_GROUND_01 166 | OW_ICEDRAGON_GROUND_02 354, OW_ICEDRAGON_MOUNTAIN_04 287 |
| -7_0 | 1054 | 6.4 | 0% | OW_ICEDRAGON_SNOW_03 902, OW_ICEDRAGON_HOUSEWALL_01 57 | OW_ICEDRAGON_SNOW_03 548, OW_ICEDRAGON_MOUNTAIN_04 373 |


Chunks with at least one multi-layer cell: 154; chunks with >= 500 cells: 48.


Multi-layer cells (solid (ground+rock+man-made), land): 68,416; gap between lowest and highest layer: <=5 m 11,300, 5-15 m 22,256, >15 m 34,860; cells with an S:*CAVE* room material in the lowest or highest layer: 125.

### Where: multi-layer cells, solid (ground+rock+man-made), land - top 20 chunks (x_z = floor(metre/64); cave % = share of the two end layers with an S:*CAVE* material)

| chunk | multi-layer cells | median height gap m | cave mats | lowest layer materials | highest layer materials |
|---|---|---|---|---|---|
| -9_1 | 2568 | 8.9 | 0% | OW_ICEDRAGON_SNOW_03 1876, OW_ICEDRAGON_LAKE_01 495 | OW_ICEDRAGON_SNOW_03 2458, OW_ICEDRAGON_MOUNTAIN_03 64 |
| 3_-7 | 2565 | 71.8 | 0% | OW_FIREDRAGON_GROUND_01 1558, OW_STONEDRAGON_GROUNDSTONES_15 559 | OW_SURFACE_MOUNTAINDARK_02 2562, OW_FIREDRAGON_GROUND_01 3 |
| -9_0 | 2563 | 71.9 | 0% | OW_ICEDRAGON_SNOW_03 2486, OW_ICEDRAGON_MOUNTAIN_04 36 | OW_ICEDRAGON_SNOW_03 1836, OW_ICEDRAGON_MOUNTAIN_04 527 |
| 3_-5 | 2088 | 20.1 | 0% | OW_SURFACE_WASTELAND_04 1414, OW_STONEDRAGON_GROUNDSTONES_13 601 | OW_SURFACE_MOUNTAIN_01 1433, OW_SURFACE_MOUNTAINDARK_02 266 |
| -5_4 | 2046 | 11.1 | 0% | OW_ICEDRAGON_GROUND_02 1297, OW_SURFACE_ROCK_02 521 | OW_SURFACE_ROCK_02 1302, OW_SURFACE_MOUNTAIN_01 400 |
| -9_2 | 1926 | 5.7 | 0% | OW_ICEDRAGON_LAKE_01 1251, OW_ICEDRAGON_SNOW_03 627 | OW_ICEDRAGON_SNOW_03 1708, OW_ICEDRAGON_MOUNTAIN_02 192 |
| -10_0 | 1638 | 73.0 | 0% | OW_ICEDRAGON_SNOW_03 1158, OW_ICEDRAGON_MOUNTAIN_04 365 | OW_ICEDRAGON_SNOW_03 1039, OW_ICEDRAGON_MOUNTAIN_04 426 |
| -10_1 | 1596 | 7.4 | 0% | OW_ICEDRAGON_SNOW_03 1385, OW_ICEDRAGON_LAKE_01 145 | OW_ICEDRAGON_SNOW_03 1064, OW_ICEDRAGON_MOUNTAIN_04 234 |
| -6_1 | 1495 | 31.1 | 0% | OW_ICEDRAGON_SNOW_03 1416, OW_ICEDRAGON_LAKE_01 38 | OW_ICEDRAGON_MOUNTAIN_04 1277, OW_ICEDRAGON_SNOW_03 102 |
| -5_-5 | 1494 | 36.9 | 0% | OWODWAMOUNTAINCLOSE 657, OW_SURFACE_ROCK_02 554 | OW_SURFACE_MOUNTAINDARK_02 1061, OW_SURFACE_MOUNTAIN_01 248 |
| -7_1 | 1415 | 4.9 | 0% | OW_ICEDRAGON_SNOW_03 1288, OW_ICEDRAGON_LAKE_01 78 | OW_ICEDRAGON_SNOW_03 1133, OW_ICEDRAGON_MOUNTAIN_04 280 |
| -9_3 | 1399 | 6.3 | 0% | OW_ICEDRAGON_SNOW_03 1322, OW_ICEDRAGON_MOUNTAIN_04 71 | OW_ICEDRAGON_SNOW_03 1162, OW_ICEDRAGON_MOUNTAIN_04 173 |
| -1_-1 | 1331 | 15.4 | 0% | OW_OLDCAMP_WOOD_02 804, OW_VOLCANO_GROUND_01 314 | OW_OLDCAMP_ROOF_02 1088, OWODWAOLDWOOD01 194 |
| -5_7 | 1299 | 14.7 | 0% | OW_SURFACE_MOUNTAIN_01 1158, OW_ICEDRAGON_MOUNTAIN_05 141 | OW_SURFACE_MOUNTAIN_01 722, OW_ICEDRAGON_MOUNTAIN_05 307 |
| -8_1 | 1207 | 4.4 | 0% | OW_ICEDRAGON_SNOW_03 1141, OW_ICEDRAGON_MOUNTAIN_04 39 | OW_ICEDRAGON_SNOW_03 977, OW_ICEDRAGON_MOUNTAIN_04 173 |
| -7_4 | 1108 | 18.8 | 0% | OW_ICEDRAGON_MOUNTAIN_04 976, OW_ICEDRAGON_MOUNTAIN_05 132 | OW_ICEDRAGON_MOUNTAIN_05 976, OW_ICEDRAGON_MOUNTAIN_04 132 |
| -8_2 | 1107 | 4.8 | 0% | OW_ICEDRAGON_SNOW_03 932, OW_ICEDRAGON_MOUNTAIN_04 112 | OW_ICEDRAGON_SNOW_03 870, OW_ICEDRAGON_MOUNTAIN_04 161 |
| 3_-6 | 1103 | 50.1 | 0% | OW_STONEDRAGON_GROUNDSTONES_15 835, OW_SURFACE_MOUNTAINDARK_02 75 | OW_SURFACE_MOUNTAINDARK_02 942, OW_STONEDRAGON_GROUNDSTONES_15 122 |
| -6_2 | 1096 | 10.4 | 0% | OW_ICEDRAGON_SNOW_03 922, OW_SURFACE_DRYGROUND_01A 106 | OW_ICEDRAGON_MOUNTAIN_04 722, OW_ICEDRAGON_GROUND_01 137 |
| -7_-1 | 1075 | 32.6 | 0% | OW_ICEDRAGON_LAKE_01 576, OW_ICEDRAGON_GROUND_01 166 | OW_ICEDRAGON_GROUND_02 354, OW_ICEDRAGON_MOUNTAIN_04 287 |


Chunks with at least one multi-layer cell: 160; chunks with >= 500 cells: 51.


# Appendix: ADDONWORLD (rough classes)

## ADDONWORLD: totals

Sea level used to split land from seabed: y = -21.2 m (median centroid height of the largest water material).

| quantity | value |
|---|---|
| triangles | 331,888 |
| vertices (world.json, engine level) | 178,074 |
| unique positions (float32) | 177,108 |
| bbox min x,y,z (m, Gothic axes) | [-1188.1, -185.5, -384.4] |
| bbox max x,y,z (m, Gothic axes) | [450.4, 103.1, 1025.1] |
| 64 m chunk files | 371 |
| surface area of all triangles (m2) | 5,122,462 |
| plan area of up-facing triangles, sum(A*ny) (m2) | 2,484,673 |
| zero-area triangles | 519 |


### By material class

| class | triangles | area m2 | plan (up-facing) m2 | tris per m2 | tris below sea (below sea level) |
|---|---|---|---|---|---|
| ground | 19,261 | 851,464 | 805,056 | 0.023 | 14,656 |
| rock | 118,729 | 1,616,803 | 569,227 | 0.073 | 62,854 |
| water | 3,597 | 2,107,183 | 1,047,375 | 0.002 | 1,307 |
| man-made | 154,121 | 133,085 | 42,835 | 1.158 | 104,997 |
| vegetation | 35,025 | 367,433 | 11,925 | 0.095 | 27,632 |
| other | 1,155 | 46,495 | 8,255 | 0.025 | 668 |


### Ground and rock split by sea level

| class | part | triangles | area m2 | tris per m2 | plan m2 |
|---|---|---|---|---|---|
| ground | land (above sea level) | 4,605 | 45,748 | 0.101 | 44,292 |
| ground | below sea | 14,656 | 805,716 | 0.018 | 760,764 |
| rock | land (above sea level) | 55,875 | 716,864 | 0.078 | 352,283 |
| rock | below sea | 62,854 | 899,939 | 0.070 | 216,944 |


### Edge length (m) and triangle area per class

| class | part | tris | edge p10 | edge p50 | edge p90 | median tri area m2 | sqrt(2*median area) m |
|---|---|---|---|---|---|---|---|
| ground | all | 19,261 | 1.5 | 4.25 | 13.26 | 5.883 | 3.43 |
| ground | land | 4,605 | 1.62 | 4.12 | 8.64 | 5.656 | 3.36 |
| rock | all | 118,729 | 0.68 | 3.18 | 10.58 | 3.324 | 2.58 |
| rock | land | 55,875 | 1.28 | 3.55 | 10.66 | 4.152 | 2.88 |
| man-made | all | 154,121 | 0.16 | 0.91 | 3.41 | 0.204 | 0.64 |
| man-made | land | 49,124 | 0.15 | 0.87 | 3.54 | 0.19 | 0.62 |
| vegetation | all | 35,025 | 0.43 | 1.75 | 6.52 | 0.952 | 1.38 |
| vegetation | land | 7,393 | 0.29 | 1.68 | 20.02 | 0.363 | 0.85 |


### 40 largest materials by area

| material | group | class | tris | area m2 | tris/m2 | median edge m |
|---|---|---|---|---|---|---|
| NW_WATER_LAKE01_A0 | 5 | water | 1,486 | 1,947,505 | 0.001 | 35.38 |
| NW_SEQ_NATURE_WHITESAND_02 | 4 | ground | 1,877 | 650,544 | 0.003 | 23.38 |
| NW_SEQ_CANYONWALL_10 | 2 | rock | 9,934 | 463,873 | 0.021 | 5.12 |
| NW_SEQ_MOUNTAIN_03_XL | 2 | rock | 29,635 | 387,776 | 0.076 | 3.56 |
| NW_NATURE_TREE_04 | 0 | vegetation | 2,793 | 322,522 | 0.009 | 16.86 |
| NW_SEQ_MOUNTAIN_02_XL | 2 | rock | 7,021 | 285,191 | 0.025 | 7.79 |
| NW_SEQ_MAYA_NATURESTONE_04_XL | 2 | rock | 16,540 | 259,533 | 0.064 | 4.85 |
| NW_ADDON_SWAMPWATER_01 | 5 | water | 870 | 149,641 | 0.006 | 20.08 |
| NW_SEQ_GROUND_02_XL | 4 | ground | 9,019 | 81,904 | 0.110 | 3.49 |
| NW_SEQ_NATURE_SWAMPGROUND_01_XL | 4 | ground | 3,145 | 76,525 | 0.041 | 7.43 |
| SUN_BLOCKER_2000 | 0 | other | 500 | 45,446 | 0.011 | 15.62 |
| NW_SEQ_CANYONWALL_09 | 4 | rock | 3,852 | 42,828 | 0.090 | 3.54 |
| NW_SEQ_CANYONWALL_04 | 2 | rock | 6,218 | 39,929 | 0.156 | 3.51 |
| NW_SEQ_CANYONWALL_03 | 2 | rock | 7,103 | 27,458 | 0.259 | 1.5 |
| NW_SEQ_NATURE_TREEBARK_01 | 3 | vegetation | 17,432 | 20,142 | 0.865 | 1.72 |
| NW_SEQ_CANYONWALL_01 | 4 | rock | 4,488 | 18,377 | 0.244 | 2.62 |
| NW_SEQ_MAYA_WALL_10 | 2 | man-made | 9,416 | 17,646 | 0.534 | 1.75 |
| NW_SEQ_NATURE_WHITESAND_01 | 4 | ground | 311 | 15,253 | 0.020 | 6.97 |
| NW_SEQ_CANYONWALL_05 | 2 | rock | 2,796 | 14,735 | 0.190 | 3.38 |
| NW_SEQ_NATURE_WALLTRANS_03_XL | 2 | rock | 1,897 | 13,112 | 0.145 | 3.27 |
| OW_SURFACE_MOUNTAIN_03 | 2 | rock | 2,477 | 11,821 | 0.210 | 3.16 |
| NW_NATURE_LEAVE_03 | 0 | vegetation | 10,649 | 11,463 | 0.929 | 1.59 |
| NW_NATURE_BRANCH_01 | 0 | vegetation | 800 | 9,459 | 0.085 | 4.82 |
| NW_SEQ_CANYONWALL_06 | 4 | rock | 2,590 | 9,151 | 0.283 | 2.77 |
| NW_SEQ_MAYA_NATURESTONE_03_XL | 2 | rock | 920 | 7,675 | 0.120 | 3.38 |
| NW_SEQ_MAYA_WALL_11 | 2 | man-made | 12,764 | 7,147 | 1.786 | 0.51 |
| NW_SEQ_NATURE_WALL_TRANS_04_XL | 2 | rock | 1,150 | 6,818 | 0.169 | 3.31 |
| NW_SEQ_NATURE_WHITESAND_TRANS_01 | 4 | ground | 168 | 6,670 | 0.025 | 4.49 |
| NW_SEQ_GRASSPATH_01_XL | 4 | ground | 1,463 | 5,812 | 0.252 | 2.94 |
| NW_MISC_SWAMPWATER_01 | 5 | water | 346 | 5,797 | 0.060 | 6.64 |
| NW_SEQ_GROUND_04_XL | 4 | ground | 872 | 5,676 | 0.154 | 4.19 |
| NW_SEQ_CANYONWALL_07 | 4 | rock | 1,644 | 5,658 | 0.291 | 2.83 |
| NW_SEQ_MAYA_STONE_TRANS_02_XL | 2 | man-made | 1,031 | 5,584 | 0.185 | 3.54 |
| NW_MISC_MAYA_WALL_03 | 2 | man-made | 1,322 | 4,555 | 0.290 | 3.23 |
| NW_MISC_MAYA_GROUND_01 | 2 | man-made | 993 | 4,081 | 0.243 | 3.08 |
| NW_SEQ_CANYONWALL_12 | 2 | rock | 645 | 3,987 | 0.162 | 3.42 |
| NW_MISC_FULLALPHA_01 | 0 | vegetation | 657 | 3,368 | 0.195 | 2.39 |
| NW_SEQ_MAYA_WALL_RED_04 | 2 | man-made | 3,609 | 3,356 | 1.075 | 1.46 |
| NW_MISC_LIGHTDECAL_03 | 0 | man-made | 314 | 3,321 | 0.095 | 5.93 |
| NW_CITY_BROKENSTONE_01 | 2 | man-made | 3,655 | 3,316 | 1.102 | 1.29 |


## ADDONWORLD: per-chunk triangles

| stat | triangles/chunk | area m2/chunk |
|---|---|---|
| chunks with triangles | 371 |  |
| min | 1 | 45 |
| median | 25 | 12,785 |
| mean | 894 | 13,807 |
| p90 | 2784 | 23,423 |
| max | 11219 | 92,487 |


### Densest 12 chunks by triangles

| chunk x_z | tris | area m2 | tris/m2 | class shares of area | top materials |
|---|---|---|---|---|---|
| 0_0 | 11,219 | 6,622 | 1.69 | rock 17%, man-made 82% | NW_SEQ_MOUNTAIN_03_XL, S:EXITPORT02_NW_SEQ_MAYA_WALL_GREEN_12 |
| 5_4 | 11,202 | 29,830 | 0.38 | rock 20%, man-made 16%, vegetation 58% | NW_NATURE_LEAVE_03, NW_NATURE_TREE_04 |
| 4_1 | 11,103 | 18,624 | 0.60 | rock 51%, man-made 37% | NW_SEQ_MAYA_NATURESTONE_04_XL, NW_SEQ_GROUND_02_XL |
| 2_3 | 10,579 | 16,433 | 0.64 | man-made 35%, other 55% | SUN_BLOCKER_2000, S:ADDADANOS03_NW_SEQ_CANYONTEMPLE_BORDE_01 |
| 3_4 | 10,173 | 13,139 | 0.77 | man-made 34%, other 64% | SUN_BLOCKER_2000, S:ADDADANOS10_NW_SEQ_CANYONTEMPLE_BORDE_01 |
| 4_2 | 10,141 | 21,926 | 0.46 | rock 50%, man-made 34% | NW_SEQ_MAYA_NATURESTONE_04_XL, NW_SEQ_GROUND_02_XL |
| 3_2 | 9,947 | 18,410 | 0.54 | rock 46%, man-made 43% | NW_SEQ_MAYA_NATURESTONE_04_XL, NW_ADDON_SWAMPWATER_01 |
| 4_3 | 9,513 | 20,660 | 0.46 | rock 65%, other 21% | NW_SEQ_MAYA_NATURESTONE_04_XL, SUN_BLOCKER_2000 |
| 3_3 | 8,156 | 9,978 | 0.82 | rock 40%, man-made 27%, other 33% | SUN_BLOCKER_2000, NW_SEQ_MAYA_NATURESTONE_04_XL |
| -4_-1 | 8,109 | 31,107 | 0.26 | rock 87% | NW_SEQ_MOUNTAIN_02_XL, NW_SEQ_MOUNTAIN_03_XL |
| -3_-3 | 7,585 | 21,750 | 0.35 | rock 85% | NW_SEQ_MOUNTAIN_03_XL, NW_SEQ_MAYA_NATURESTONE_04_XL |
| 2_4 | 6,747 | 13,085 | 0.52 | man-made 28%, other 67% | SUN_BLOCKER_2000, S:ADDADANOS06_NW_DRAGONISLAND_ALCHEMY_RED_SHRINE_01 |


### Slope bands, share of surface area in % (angle of the face normal from straight up; "leaning out" = normal y in [-0.5,0), i.e. 90-120 deg, vertical walls tilting down; "ceiling" = normal y < -0.5, i.e. overhang/underside steeper than 60 deg from the horizontal)

| class | part | 0-15 | 15-30 | 30-45 | 45-60 | 60-90 | leaning out | ceiling | area m2 |
|---|---|---|---|---|---|---|---|---|---|
| ground | all | 55.4 | 33.4 | 9.6 | 1.0 | 0.5 | 0.0 | 0.0 | 851,464 |
| ground | land | 78.2 | 16.6 | 3.7 | 0.9 | 0.4 | 0.1 | 0.1 | 45,748 |
| rock | all | 5.7 | 7.8 | 9.4 | 11.1 | 36.9 | 17.4 | 11.7 | 1,616,803 |
| rock | land | 9.8 | 12.7 | 13.7 | 13.7 | 34.6 | 10.6 | 4.9 | 716,864 |
| man-made | all | 24.2 | 2.9 | 2.2 | 1.8 | 34.2 | 19.7 | 14.9 | 133,085 |
| man-made | land | 22.4 | 4.7 | 3.0 | 2.9 | 34.8 | 17.3 | 14.8 | 49,191 |


## ADDONWORLD: texel density, ten largest ground materials (base .dds; texels per metre = sqrt(uv area x w x h / world area) per triangle, triangles > 0.05 m2)

| material | dds size | area m2 | median texels/m | p10 / p90 | median uv-units per m |
|---|---|---|---|---|---|
| NW_SEQ_NATURE_WHITESAND_02 | 512x512 | 650,544 | 25.5 | 23.9 / 76.6 | 0.0499 |
| NW_SEQ_GROUND_02_XL | 1024x1024 | 81,904 | 248.4 | 187.6 / 292.6 | 0.2426 |
| NW_SEQ_NATURE_SWAMPGROUND_01_XL | 1024x1024 | 76,525 | 138.5 | 125.1 / 146.3 | 0.1353 |
| NW_SEQ_NATURE_WHITESAND_01 | 512x512 | 15,253 | 102.3 | 41.3 / 143.1 | 0.1997 |
| NW_SEQ_NATURE_WHITESAND_TRANS_01 | 512x512 | 6,670 | 125.3 | 49.7 / 200.3 | 0.2448 |
| NW_SEQ_GRASSPATH_01_XL | 1024x1024 | 5,812 | 211.6 | 185.7 / 319.5 | 0.2066 |
| NW_SEQ_GROUND_04_XL | 1024x1024 | 5,676 | 200.5 | 113.3 / 249.4 | 0.1958 |
| NW_SEQ_NATURE_GROUNDTRANS_07_XL | 1024x1024 | 2,472 | 137.5 | 103.4 / 185.1 | 0.1343 |
| NW_SEQ_NATURE_GROUNDTRANS_06_XL | 1024x1024 | 1,446 | 124.5 | 99.2 / 143.1 | 0.1216 |
| NW_SEQ_NATURE_GROUNDTRANS_05_XL | 1024x1024 | 1,422 | 193.8 | 133.7 / 384.8 | 0.1892 |


## ADDONWORLD: layers on a 1 m XZ grid (cell centre, up-facing triangles with normal y>0.1, heights chained: a gap > 2 m starts a new layer)

| set | cells with >=1 layer (= m2) | exactly 1 layer | >=2 layers (gap>2 m) | >=3 layers |
|---|---|---|---|---|
| natural (ground+rock), land | 318,730 | 267,463 (83.9%) | 51,267 (16.1%) | 8,489 |
| solid (ground+rock+man-made), land | 326,350 | 270,982 (83.0%) | 55,368 (17.0%) | 9,357 |
| natural + seabed | 1,044,909 | 810,347 (77.6%) | 234,562 (22.4%) | 49,109 |


Multi-layer cells (natural (ground+rock), land): 51,267; gap between lowest and highest layer: <=5 m 9,682, 5-15 m 21,867, >15 m 19,718; cells with an S:*CAVE* room material in the lowest or highest layer: 331.

### Where: multi-layer cells, natural (ground+rock), land - top 20 chunks (x_z = floor(metre/64); cave % = share of the two end layers with an S:*CAVE* material)

| chunk | multi-layer cells | median height gap m | cave mats | lowest layer materials | highest layer materials |
|---|---|---|---|---|---|
| -4_-1 | 2844 | 26.2 | 0% | NW_SEQ_MOUNTAIN_02_XL 2343, NW_SEQ_GROUND_04_XL 304 | NW_SEQ_MOUNTAIN_02_XL 1349, NW_SEQ_CANYONWALL_10 815 |
| -6_0 | 2736 | 17.3 | 4% | NW_SEQ_NATURE_WHITESAND_01 1102, NW_SEQ_CANYONWALL_10 830 | NW_SEQ_CANYONWALL_10 2727, NW_SEQ_NATURE_WHITESAND_01 4 |
| -5_3 | 2073 | 7.8 | 0% | NW_SEQ_CANYONWALL_10 1660, NW_SEQ_NATURE_WHITESAND_01 272 | NW_SEQ_CANYONWALL_10 1364, NW_SEQ_CANYONWALL_03 603 |
| -6_1 | 1959 | 14.2 | 0% | NW_SEQ_NATURE_WHITESAND_01 1340, NW_SEQ_CANYONWALL_10 393 | NW_SEQ_CANYONWALL_10 1553, NW_SEQ_GROUND_02_XL 345 |
| -4_1 | 1612 | 20.4 | 0% | NW_SEQ_CANYONWALL_03 639, NW_SEQ_MOUNTAIN_03_XL 492 | NW_SEQ_MOUNTAIN_03_XL 1362, NW_SEQ_CANYONWALL_03 182 |
| 0_4 | 1560 | 6.2 | 0% | NW_SEQ_CANYONWALL_09 1558, NW_SEQ_CANYONWALL_04 2 | NW_SEQ_CANYONWALL_09 603, NW_SEQ_CANYONWALL_01 504 |
| -1_1 | 1560 | 62.4 | 0% | NW_SEQ_MOUNTAIN_02_XL 1164, NW_SEQ_MOUNTAIN_03_XL 356 | NW_SEQ_MOUNTAIN_02_XL 1321, NW_SEQ_MOUNTAIN_03_XL 239 |
| -5_-3 | 1546 | 24.0 | 0% | NW_SEQ_MOUNTAIN_02_XL 1527, NW_SEQ_MOUNTAIN_03_XL 19 | NW_SEQ_MOUNTAIN_02_XL 1501, NW_SEQ_MOUNTAIN_03_XL 45 |
| -1_5 | 1247 | 6.8 | 0% | NW_SEQ_CANYONWALL_09 1219, NW_SEQ_CANYONWALL_01 28 | NW_SEQ_CANYONWALL_09 636, NW_SEQ_CANYONWALL_04 290 |
| -6_2 | 1193 | 5.4 | 0% | NW_SEQ_CANYONWALL_10 472, NW_SEQ_NATURE_WHITESAND_01 323 | NW_SEQ_CANYONWALL_10 1167, NW_SEQ_GROUND_02_XL 24 |
| -1_4 | 1114 | 5.3 | 0% | NW_SEQ_CANYONWALL_09 1111, NW_SEQ_CANYONWALL_05 2 | NW_SEQ_CANYONWALL_09 720, NW_SEQ_CANYONWALL_01 317 |
| -5_-2 | 1098 | 17.5 | 0% | NW_SEQ_MOUNTAIN_02_XL 1049, NW_SEQ_MOUNTAIN_03_XL 49 | NW_SEQ_MOUNTAIN_02_XL 1076, NW_SEQ_MOUNTAIN_03_XL 22 |
| -9_1 | 1098 | 9.3 | 0% | NW_SEQ_CANYONWALL_10 1098 | NW_SEQ_CANYONWALL_10 1098 |
| 1_0 | 1041 | 10.2 | 0% | NW_SEQ_MOUNTAIN_03_XL 1016, NW_SEQ_NATURE_WALLTRANS_03_XL 25 | NW_SEQ_MOUNTAIN_03_XL 1041 |
| -5_2 | 1026 | 6.4 | 0% | NW_SEQ_CANYONWALL_03 323, NW_SEQ_CANYONWALL_10 280 | NW_SEQ_CANYONWALL_03 755, NW_SEQ_CANYONWALL_10 253 |
| -2_0 | 1013 | 39.3 | 0% | NW_SEQ_MOUNTAIN_02_XL 1003, NW_SEQ_MOUNTAIN_03_XL 10 | NW_SEQ_MOUNTAIN_02_XL 1000, NW_SEQ_MOUNTAIN_03_XL 13 |
| -4_4 | 976 | 14.7 | 0% | NW_SEQ_CANYONWALL_10 923, NW_SEQ_CANYONWALL_03 47 | NW_SEQ_CANYONWALL_10 692, NW_SEQ_CANYONWALL_03 249 |
| -5_-1 | 968 | 37.5 | 0% | NW_SEQ_MOUNTAIN_02_XL 957, NW_SEQ_CANYONWALL_10 11 | NW_SEQ_MOUNTAIN_02_XL 734, NW_SEQ_CANYONWALL_10 234 |
| 0_5 | 965 | 5.1 | 0% | NW_SEQ_CANYONWALL_09 965 | NW_SEQ_CANYONWALL_01 396, NW_SEQ_CANYONWALL_09 313 |
| -3_1 | 890 | 27.4 | 0% | NW_SEQ_MOUNTAIN_03_XL 370, NW_SEQ_MAYA_NATURESTONE_03_XL 238 | NW_SEQ_MOUNTAIN_03_XL 725, NW_SEQ_MAYA_NATURESTONE_03_XL 139 |


Chunks with at least one multi-layer cell: 116; chunks with >= 500 cells: 38.


Multi-layer cells (solid (ground+rock+man-made), land): 55,368; gap between lowest and highest layer: <=5 m 10,962, 5-15 m 23,201, >15 m 21,205; cells with an S:*CAVE* room material in the lowest or highest layer: 331.

### Where: multi-layer cells, solid (ground+rock+man-made), land - top 20 chunks (x_z = floor(metre/64); cave % = share of the two end layers with an S:*CAVE* material)

| chunk | multi-layer cells | median height gap m | cave mats | lowest layer materials | highest layer materials |
|---|---|---|---|---|---|
| -4_-1 | 2844 | 26.2 | 0% | NW_SEQ_MOUNTAIN_02_XL 2343, NW_SEQ_GROUND_04_XL 304 | NW_SEQ_MOUNTAIN_02_XL 1349, NW_SEQ_CANYONWALL_10 815 |
| -6_0 | 2736 | 17.3 | 4% | NW_SEQ_NATURE_WHITESAND_01 1102, NW_SEQ_CANYONWALL_10 830 | NW_SEQ_CANYONWALL_10 2727, NW_SEQ_NATURE_WHITESAND_01 4 |
| -5_3 | 2073 | 7.8 | 0% | NW_SEQ_CANYONWALL_10 1660, NW_SEQ_NATURE_WHITESAND_01 272 | NW_SEQ_CANYONWALL_10 1364, NW_SEQ_CANYONWALL_03 603 |
| -6_1 | 2053 | 14.5 | 0% | NW_SEQ_NATURE_WHITESAND_01 1340, NW_SEQ_CANYONWALL_10 418 | NW_SEQ_CANYONWALL_10 1523, NW_SEQ_GROUND_02_XL 252 |
| -4_1 | 1612 | 20.4 | 0% | NW_SEQ_CANYONWALL_03 639, NW_SEQ_MOUNTAIN_03_XL 492 | NW_SEQ_MOUNTAIN_03_XL 1362, NW_SEQ_CANYONWALL_03 182 |
| -1_1 | 1560 | 62.4 | 0% | NW_SEQ_MOUNTAIN_02_XL 1164, NW_SEQ_MOUNTAIN_03_XL 356 | NW_SEQ_MOUNTAIN_02_XL 1321, NW_SEQ_MOUNTAIN_03_XL 239 |
| 0_4 | 1560 | 6.2 | 0% | NW_SEQ_CANYONWALL_09 1558, NW_SEQ_CANYONWALL_04 2 | NW_SEQ_CANYONWALL_09 603, NW_SEQ_CANYONWALL_01 504 |
| -5_-3 | 1546 | 24.0 | 0% | NW_SEQ_MOUNTAIN_02_XL 1527, NW_SEQ_MOUNTAIN_03_XL 19 | NW_SEQ_MOUNTAIN_02_XL 1501, NW_SEQ_MOUNTAIN_03_XL 45 |
| -6_2 | 1379 | 5.5 | 0% | NW_SEQ_CANYONWALL_10 478, NW_SEQ_NATURE_WHITESAND_01 367 | NW_SEQ_CANYONWALL_10 1159, NW_SEQ_MISC_STRAWROOF_01 83 |
| -1_5 | 1247 | 6.8 | 0% | NW_SEQ_CANYONWALL_09 1219, NW_SEQ_CANYONWALL_01 28 | NW_SEQ_CANYONWALL_09 636, NW_SEQ_CANYONWALL_04 290 |
| -1_4 | 1114 | 5.3 | 0% | NW_SEQ_CANYONWALL_09 1111, NW_SEQ_CANYONWALL_05 2 | NW_SEQ_CANYONWALL_09 720, NW_SEQ_CANYONWALL_01 317 |
| -9_1 | 1098 | 9.3 | 0% | NW_SEQ_CANYONWALL_10 1098 | NW_SEQ_CANYONWALL_10 1098 |
| -5_-2 | 1098 | 17.5 | 0% | NW_SEQ_MOUNTAIN_02_XL 1049, NW_SEQ_MOUNTAIN_03_XL 49 | NW_SEQ_MOUNTAIN_02_XL 1076, NW_SEQ_MOUNTAIN_03_XL 22 |
| -5_2 | 1074 | 6.5 | 0% | NW_SEQ_CANYONWALL_03 323, NW_SEQ_CANYONWALL_10 296 | NW_SEQ_CANYONWALL_03 755, NW_SEQ_CANYONWALL_10 253 |
| 0_-1 | 1062 | 11.4 | 0% | NW_SEQ_GROUND_02_XL 297, NW_SEQ_MAYA_GROUND_03 272 | NW_SEQ_MOUNTAIN_03_XL 550, NW_MISC_MAYA_WALL_02 152 |
| 1_0 | 1041 | 10.2 | 0% | NW_SEQ_MOUNTAIN_03_XL 1016, NW_SEQ_NATURE_WALLTRANS_03_XL 25 | NW_SEQ_MOUNTAIN_03_XL 1041 |
| -1_-1 | 1035 | 8.0 | 0% | NW_SEQ_GROUND_02_XL 316, NW_SEQ_NATURE_WALLTRANS_03_XL 227 | NW_SEQ_MOUNTAIN_03_XL 315, NW_SEQ_NATURE_WALLTRANS_03_XL 296 |
| -2_0 | 1013 | 39.3 | 0% | NW_SEQ_MOUNTAIN_02_XL 1003, NW_SEQ_MOUNTAIN_03_XL 10 | NW_SEQ_MOUNTAIN_02_XL 1000, NW_SEQ_MOUNTAIN_03_XL 13 |
| -4_4 | 976 | 14.7 | 0% | NW_SEQ_CANYONWALL_10 923, NW_SEQ_CANYONWALL_03 47 | NW_SEQ_CANYONWALL_10 692, NW_SEQ_CANYONWALL_03 249 |
| -5_-1 | 968 | 37.5 | 0% | NW_SEQ_MOUNTAIN_02_XL 957, NW_SEQ_CANYONWALL_10 11 | NW_SEQ_MOUNTAIN_02_XL 734, NW_SEQ_CANYONWALL_10 234 |


Chunks with at least one multi-layer cell: 118; chunks with >= 500 cells: 40.


# Original trees of NEWWORLD: intent sheet inventory

Date 2026-11-16. Measured offline from the imported game base (fingerprint `693b9370…`, `~/.cache/gothic-ai/base`): `worlds/NEWWORLD/chunks/*.glb`, `vobs.json`, `waynet.json`, `meshes/*.glb`, `textures/*.dds`. No game launch, no engine build. Throw-away scripts stayed in the session scratchpad (`trees-inventory/step1…5.py`); class rules are those of `scripts/content/world/world_classes.py`, the loader and raster are `world_census.py`.

Sheets: [silhouettes of every vob mesh at one scale](original/silhouettes.png) (red bar = 1.8 m), [pilot map](original/pilot_map.png), [island map](original/island_map.png). The texture contact sheet was made but is not in the repository (original textures do not go into git); it is in the scratchpad as `out/textures.jpg`.

## Main finding: the "forest shells" are trees

The 38 % of world-mesh triangles classed as vegetation are not canopy roofs or forest walls. They are **copies of the same two tree models baked into the level mesh, scaled** (vobs cannot be scaled: every tree and bush vob has scale exactly 1.0):

- **306 big broadleaf trees** — the `NW_NATURE_BIGTREE_356P` model. Each is one connected crown of exactly 96 triangles of `NW_NATURE_TREE_04` (crossed vertical planes plus down-facing discs), about 8 branch cards of `NW_NATURE_BRANCH_01` and 5 bark pieces of `NW_NATURE_BARK_04` (53-triangle trunk + 39/39/28/18-triangle boughs; component counts 306/612/306/306 match exactly).
- **546 firs** — the `NW_NATURE_TANNE` model: a 3-triangle trunk of `NW_NATURE_BARK_02` (476 trunks; 70 have 9 triangles) under tiers of `NW_NATURE_TREE_NEEDLE_03` cards.

So the island has **2 344 trees: 1 492 vobs + 852 in the level mesh**, and the dark "roof" of the forest is stacked crowns of big broadleaf trees, not a separate shell.

## 1. Vob trees and bushes

1 643 vobs of class `trees` (22 visuals; one, `BAUMSAEGE_1.ASC`, 11 instances, is the saw mob, not a tree), 117 logs, 1 652 bushes (11 meshes). Instance scale: 1.0 for all; trees stand upright (median tilt 0°), stumps, logs and some bushes are tilted 3–16°. Vob origin is at the mesh foot; firs and small trees are sunk 0.6–2 m into the ground (median 1.3 m), so visible height is about 1 m less than mesh height.

All leaf and needle surfaces are alpha-tested cards (`alpha_test` 1, DXT3 textures); bark is opaque.

| Species | Mesh | Inst. | Tris | Height, m | Crown width, m | Trunk dia at 1.3 m (0.3 m), m | Crown base, m | Crown h / H | Crown w / H | Materials (tris) | cd_dyn / cd_static |
|---|---|---|---|---|---|---|---|---|---|---|---|
| fir | `NW_NATURE_TANNE_75P` | 389 | 75 | 12.0 | 11.2 | 0.68 (0.74) | 1.7 | 0.86 | 0.93 | NW_NATURE_TREE_NEEDLE_03 72, NW_NATURE_BARK_02 3 | 389 / 75 |
| fir | `NW_NATURE_TANNE_100P` | 209 | 99 | 12.0 | 11.2 | 0.68 (0.74) | 1.4 | 0.89 | 0.93 | NW_NATURE_TREE_NEEDLE_03 96, NW_NATURE_BARK_02 3 | 178 / 61 |
| tall fir (bare lower trunk) | `NW_NATURE_HOHETANNE_59P` | 88 | 59 | 14.1 | 5.2 | 0.53 (0.57) | 9.0 | 0.36 | 0.37 | NW_NATURE_TREE_NEEDLE_03 36, NW_NATURE_TREE_NEEDLE_02 8, NW_NATURE_BARK_02 15 | 81 / 5 |
| tall fir (bare lower trunk) | `NW_NATURE_HOHETANNE_91P` | 82 | 91 | 13.9 | 6.5 | 0.52 (0.57) | 7.5 | 0.46 | 0.47 | NW_NATURE_TREE_NEEDLE_03 46, NW_NATURE_TREE_NEEDLE_02 30, NW_NATURE_BARK_02 15 | 81 / 0 |
| tall fir (bare lower trunk) | `NW_NATURE_HOHETANNE_MIDDLE_59P` | 17 | 59 | 20.3 | 7.5 | 0.78 (0.83) | 12.9 | 0.36 | 0.37 | NW_NATURE_TREE_NEEDLE_03 36, NW_NATURE_TREE_NEEDLE_02 8, NW_NATURE_BARK_02 15 | 17 / 0 |
| tall fir (bare lower trunk) | `NW_NATURE_HOHETANNE_VERYHIGH_59P` | 28 | 59 | 29.4 | 10.9 | 1.16 (1.21) | 18.7 | 0.36 | 0.37 | NW_NATURE_TREE_NEEDLE_03 36, NW_NATURE_TREE_NEEDLE_02 8, NW_NATURE_BARK_02 15 | 28 / 0 |
| fir clump (9 trunks in one mesh) | `NW_NATURE_HOHETANNEN_01_531P` | 36 | 531 | 20.1 | 22.8 | – (–) | 10.0 | 0.50 | 1.13 | NW_NATURE_TREE_NEEDLE_03 324, NW_NATURE_TREE_NEEDLE_02 72, NW_NATURE_BARK_02 135 | 36 / 0 |
| fir clump (9 trunks in one mesh) | `NW_NATURE_HOHETANNEN_02_531P` | 31 | 531 | 20.1 | 16.4 | – (–) | 10.1 | 0.50 | 0.82 | NW_NATURE_TREE_NEEDLE_03 324, NW_NATURE_TREE_NEEDLE_02 72, NW_NATURE_BARK_02 135 | 31 / 0 |
| small broadleaf | `NW_NATURE_SMALLTREE_79P` | 130 | 79 | 12.0 | 5.1 | 0.23 (0.49) | 1.9 | 0.84 | 0.43 | NW_NATURE_BARK_01 19, NW_NATURE_BRANCH_02 20, NW_NATURE_BRANCH_03 40 | 64 / 12 |
| small broadleaf | `NW_NATURE_SMALLTREE_103P` | 45 | 103 | 12.0 | 5.4 | 0.23 (0.49) | 1.4 | 0.88 | 0.45 | NW_NATURE_BARK_01 19, NW_NATURE_BRANCH_02 28, NW_NATURE_BRANCH_03 56 | 29 / 0 |
| big broadleaf | `NW_NATURE_BIGTREE_356P` | 100 | 351 | 31.0 | 26.7 | 1.93 (2.68) | 2.6 | 0.92 | 0.86 | NW_NATURE_BRANCH_01 80, NW_NATURE_TREE_04 94, NW_NATURE_BARK_04 177 | 100 / 0 |
| big broadleaf | `OW_NATURE_BIG_TREE_02_380P` | 11 | 375 | 31.0 | 26.7 | 1.93 (2.68) | 2.1 | 0.93 | 0.86 | OW_NATURE_BIGTREE_01 198, NW_NATURE_BARK_04 177 | 11 / 0 |
| old half-bare broadleaf | `OW_NATURE_OLDTREE_02` | 9 | 1092 | 20.3 | 23.8 | – (–) | 0.5 | 0.98 | 1.17 | NW_NATURE_BARK_04 772, OW_NATURE_BUSH_02 224, NW_NATURE_BRANCH_02 96 | 9 / 5 |
| dead tree (bare twig cards) | `OW_LOB_TREE_V11` | 31 | 159 | 20.0 | 19.1 | – (–) | 0.0 | 1.00 | 0.96 | MOWOROOT02 80, PCODWATREE03 79 | 31 / 0 |
| bare sapling / root | `OW_NATURE_SMALLTREE_01` | 286 | 191 | 5.3 | 2.2 | – (0.15) | 0.8 | 0.85 | 0.41 | NW_NATURE_BARK_04 159, NW_NATURE_ROOTS_01 32 | 0 / 0 |
| gallows tree (unique) | `OW_FOREST_HANGHIM_TREE_V3` | 1 | 1076 | 6.8 | 0.4 | 1.62 (2.17) | 1.9 | 0.72 | 0.06 | MODIRAIL01 33, SKE_BODY_V0 710, SKE_HEAD_V0 102, MOWOBARK01 231 | 1 / 0 |
| stump | `NW_NATURE_BAUMSTUMPF_02_115P` | 51 | 114 | 1.2 | – | – (2.06) | – | – | – | NW_NATURE_BARK_03 97, NW_NATURE_BARKRINGS_01 17 | 44 / 0 |
| stump | `NW_NATURE_BAUMSTUMPF_03_174P` | 42 | 174 | 1.2 | 1.8 | – (2.06) | 0.4 | 0.65 | 1.49 | NW_NATURE_BARK_03 97, NW_NATURE_BARKRINGS_01 17, NW_NATURE_PILZ_01 60 | 29 / 0 |
| stump | `NW_NATURE_BAUMSTUMPF_115P` | 22 | 114 | 1.2 | – | – (0.97) | – | – | – | NW_NATURE_BARK_03 97, NW_NATURE_BARKRINGS_01 17 | 20 / 2 |
| stump | `NW_NATURE_BAUMSTUMPF_154P` | 15 | 154 | 1.6 | 0.8 | 0.74 (1.29) | 0.7 | 0.57 | 0.53 | NW_NATURE_LEAVE_01 44, NW_NATURE_BARK_03 95, NW_NATURE_BARKRINGS_01 15 | 7 / 0 |
| stump | `NW_NATURE_BAUMSTUMPF_02_154P` | 9 | 154 | 1.6 | 1.5 | 1.34 (2.34) | 0.7 | 0.57 | 0.96 | NW_NATURE_LEAVE_01 44, NW_NATURE_BARK_03 95, NW_NATURE_BARKRINGS_01 15 | 9 / 0 |
| fallen log | `NW_NATURE_BAUMSTAMM_01_30P` | 104 | 30 | 0.9 | – | – (–) | – | – | – | NW_NATURE_BARKRINGS_01 10, NW_NATURE_BARK_03 20 | 95 / 11 |
| fallen log | `NW_NATURE_BAUMSTAMM_166P` | 13 | 166 | 7.2 | – | – (–) | – | – | – | NW_NATURE_BARKRINGS_01 4, NW_NATURE_BARK_03 162 | 13 / 0 |
| bush | `NW_NATURE_BUSH_120P` | 690 | 120 | 4.2 | 5.6 | – (–) | 0.0 | 1.00 | 1.32 | NW_NATURE_LEAVE_03 112, NW_NATURE_LEAVE_01 8 | 15 / 13 |
| bush | `NW_NATURE_BUSH_25P` | 168 | 26 | 5.4 | 4.2 | – (–) | 0.0 | 1.00 | 0.78 | NW_NATURE_BRANCH_02 10, NW_NATURE_BRANCH_03 16 | 0 / 0 |
| bush | `NW_NATURE_BUSH_56P` | 164 | 56 | 2.0 | 2.9 | – (–) | 0.0 | 1.00 | 1.45 | NW_NATURE_LEAVE_03 56 | 0 / 0 |
| bush | `NW_NATURE_LONG_BUSH_360P` | 156 | 360 | 4.2 | 11.4 | – (–) | 0.0 | 1.00 | 2.70 | NW_NATURE_LEAVE_03 336, NW_NATURE_LEAVE_01 24 | 0 / 0 |
| bush | `NW_NATURE_BUSH_300P` | 115 | 320 | 1.5 | 3.0 | – (–) | 0.0 | 1.00 | 2.01 | NW_NATURE_LEAVE_03 320 | 0 / 0 |
| bush | `OW_NATURE_BUSH_02` | 104 | 400 | 1.4 | 5.5 | – (–) | 0.0 | 1.00 | 3.83 | MOWOBUSH04 400 | 0 / 0 |
| bush | `OW_NATURE_BUSH_01` | 92 | 20 | 2.6 | 2.3 | – (–) | 0.0 | 1.00 | 0.88 | NW_NATURE_BRANCH_06 20 | 0 / 0 |
| bush | `NW_NATURE_BUSH_60P` | 63 | 60 | 2.0 | 1.8 | – (–) | 0.0 | 1.00 | 0.90 | NW_NATURE_LEAVE_03 56, NW_NATURE_LEAVE_01 4 | 0 / 0 |
| bush | `OW_LOB_BUSH_V5` | 39 | 48 | 1.9 | 1.3 | – (–) | 0.0 | 1.00 | 0.73 | MOWOBUSH02 48 | 0 / 0 |
| bush | `OW_LOB_BUSH_REED_V1` | 32 | 218 | 4.4 | 6.4 | – (–) | 0.0 | 1.00 | 1.46 | OW_LOB_BUSH_REED_V1_1 48, OW_LOB_BUSH_REED_V1_2 170 | 0 / 0 |
| bush | `NW_NATURE_BUSH_01` | 29 | 235 | 1.7 | 2.1 | – (–) | 0.0 | 1.00 | 1.24 | NW_NATURE_BRANCH_07 235 | 0 / 0 |

Notes:
- Trunk diameter is the bark cross-section extent; "–" where the mesh has several trunks or none. Fir trunks are 3-sided pyramids, tall-fir trunks 5-sided.
- `HOHETANNEN_01/02` hold 9 trunks 14–20 m tall, 0.6–0.8 m thick, in a 22–24 m (01) or 15–18 m (02) clump; with them the vob firs are 598 + 215 + 67 × 9 = 1 416 trunks.
- `BIGTREE_356P`: trunk 19.8 m, crown `TREE_04` from 5.8 m to 31 m, 26.7 m wide; boughs start at 2.4–5 m.
- `OW_NATURE_SMALLTREE_01` (286) is a bare sapling with root cards: 137 near the pass to the Valley of Mines, 180 of them are children of another sapling vob.
- `OW_LOB_TREE_V11` uses Gothic 1 textures (bare twig cards `MOWOROOT02`): a leafless tree.
- Bushes: `BUSH_120P` (690) is a 4.2 m tall, 3.7 × 7.4 m leafy clump — a hedge mass, not a small shrub; `LONG_BUSH_360P` is three of them in a 16 m row. `OW_NATURE_BUSH_02`, `OW_LOB_BUSH_V5`, `REED_V1` use collidable materials, but the vobs have collision off.

## 2. Vegetation in the world mesh

177 582 triangles, 2.24 km² of surface. Height above ground is that of triangle centroids over the highest solid surface below, area-weighted.

| Material | Texture | Tris | Surface, m2 | Chunks | Median tri, m2 | Horizontal / vertical share of area | Height above ground p10/50/90, m | Collision |
|---|---|---|---|---|---|---|---|---|
| `NW_NATURE_TREE_04` | NW_NATURE_TREE_04 | 29,151 | 1,320,866 | 107 | 34.13 | 0.22 / 0.78 | 8.8 / 16.3 / 24.7 | no |
| `NW_NATURE_TREE_NEEDLE_03` | NW_NATURE_TREE_NEEDLE_03 | 39,038 | 371,240 | 78 | 6.84 | 0.82 / 0.02 | 1.3 / 4.9 / 11.1 | no |
| `NW_NATURE_BRANCH_01` | NW_NATURE_BRANCH_01 | 26,440 | 271,413 | 108 | 7.31 | 0.26 / 0.41 | 3.7 / 6.9 / 11.7 | no |
| `NW_NATURE_GREATSEA_PLANT_01` | NW_NATURE_GREATSEA_PLANT_01 | 1,982 | 113,353 | 81 | 54.03 | 0.00 / 1.00 | -0.4 / 2.2 / 6.2 | no |
| `NW_MISC_FULLALPHA_01` | NW_MISC_FULLALPHA_01 | 154 | 57,032 | 42 | 250.38 | 0.00 / 0.98 | 8.3 / 10.9 / 21.2 | yes |
| `NW_NATURE_GRASS_DECAL_01` | NW_NATURE_GRASS_DECAL_01 | 996 | 35,413 | 11 | 30.42 | 0.00 / 0.99 | 1.7 / 5.2 / 11.1 | no |
| `NW_NATURE_BARK_04` | NW_NATURE_BARK_04 | 56,607 | 28,765 | 104 | 0.29 | 0.10 / 0.69 | 0.7 / 5.8 / 12.8 | yes |
| `NW_NATURE_BARK_02` | NW_NATURE_BARK_02 | 2,058 | 9,511 | 76 | 4.28 | 0.00 / 1.00 | 1.1 / 3.0 / 6.3 | yes |
| `NW_MISC_DUCKWEED_01` | NW_MISC_DUCKWEED_01 | 1,184 | 7,691 | 14 | 7.01 | 1.00 / 0.00 | 1.1 / 4.5 / 9.5 | no |
| `NW_NATURE_TREE_NEEDLE_02` | NW_NATURE_TREE_NEEDLE_02 | 1,120 | 5,985 | 17 | 5.02 | 0.43 / 0.00 | 5.5 / 9.0 / 14.0 | no |
| `NW_MISC_SEEROSE_01` | NW_MISC_SEEROSE_01 | 296 | 3,730 | 11 | 13.05 | 1.00 / 0.00 | 1.2 / 4.2 / 9.2 | no |
| `NW_NATURE_LEAVE_03` | NW_NATURE_LEAVE_03 | 2,308 | 2,791 | 10 | 0.92 | 0.30 / 0.33 | 0.1 / 0.7 / 5.3 | no |
| `NW_NATURE_BRANCH_02` | NW_NATURE_BRANCH_02 | 1,016 | 2,150 | 32 | 0.18 | 0.00 / 0.99 | 1.2 / 3.2 / 9.6 | no |
| `NW_NATURE_BRANCH_03` | NW_NATURE_BRANCH_03 | 752 | 2,133 | 12 | 2.42 | 0.05 / 0.35 | 0.9 / 3.2 / 8.8 | no |
| `NW_CITY_ROPE_01` | NW_CITY_ROPE_01 | 1,326 | 1,277 | 9 | 0.48 | 0.21 / 0.55 | 4.7 / 20.1 / 44.1 | yes |
| `NW_NATURE_BARK_03` | NW_NATURE_BARK_03 | 5,227 | 918 | 10 | 0.14 | 0.10 / 0.56 | 0.4 / 5.8 / 10.1 | yes |
| `OW_NATURE_BUSH_02` | OW_NATURE_BUSH_02 | 496 | 858 | 2 | 1.59 | 0.25 / 0.42 | 4.5 / 7.6 / 10.6 | no |
| `NW_SHIP_ROPES_01` | NW_SHIP_ROPES_01 | 288 | 830 | 2 | 2.53 | 0.00 / 0.43 | 2.8 / 9.9 / 20.4 | yes |
| `NW_CITY_IVY_BORDER_01` | NW_CITY_IVY_BORDER_01 | 650 | 701 | 6 | 0.72 | 0.05 / 0.90 | 0.3 / 2.3 / 5.4 | no |
| `OW_FOREST_STONE_01` | OW_FOREST_STONE_01 | 1,284 | 550 | 1 | 0.36 | 0.97 / 0.00 | 3.0 / 3.1 / 3.4 | yes |
| `NW_NATURE_LEAVE_01` | NW_NATURE_LEAVE_01 | 96 | 370 | 2 | 3.70 | 0.00 / 0.78 | 0.3 / 0.7 / 1.4 | no |
| `NW_CITY_IVY_FULL_01` | NW_CITY_IVY_FULL_01 | 253 | 235 | 4 | 0.59 | 0.00 / 1.00 | 0.3 / 1.4 / 4.0 | no |
| `NW_HARBOUR_ROPEWALL_01` | NW_HARBOUR_ROPEWALL_01 | 1,700 | 198 | 5 | 0.07 | 0.28 / 0.40 | -0.4 / 1.4 / 15.9 | yes |
| `NW_NATURE_BARKRINGS_02` | NW_NATURE_BARKRINGS_02 | 1,816 | 155 | 16 | 0.02 | 0.60 / 0.29 | -0.7 / -0.2 / 2.8 | yes |
| `NW_MISC_SEEROSE_BLUETE_ROT_01` | NW_MISC_SEEROSE_BLUETE_ROT_01 | 432 | 123 | 6 | 0.28 | 0.00 / 1.00 | 1.7 / 4.5 / 9.0 | no |
| `NW_NATURE_BARKRINGS_01` | NW_NATURE_BARKRINGS_01 | 527 | 115 | 10 | 0.14 | 0.02 / 0.95 | -0.3 / -0.1 / 2.5 | yes |
| `NW_MISC_SEEROSE_BLUETE_WEISS_01` | NW_MISC_SEEROSE_BLUETE_WEISS_01 | 308 | 110 | 6 | 0.36 | 0.00 / 1.00 | 1.7 / 4.6 / 10.3 | no |
| `NW_NATURE_BARK_01` | NW_NATURE_BARK_01 | 76 | 21 | 3 | 0.20 | 0.00 / 1.00 | 0.3 / 2.3 / 5.6 | yes |


What they are:
- **`NW_NATURE_TREE_04` — crowns of the 306 baked big trees.** 78 % of the area is vertical planes, 22 % down-facing discs, nothing faces up. Median triangle 34 m². Crown width p10/50/90 = 14.9 / 24.7 / 31.0 m (min 5.7, max 42.1), i.e. scale 0.56 / 0.93 / 1.16 of the vob model (0.21–1.58). Crown height 15.3 / 22.9 / 30.5 m. Crown bottom above ground 3.1 / 4.3 / 6.6 m, top 19 / 29 / 35 m (max 48 m). The down-facing discs — the "ceiling" seen from the ground — sit at 10.6 / 16.1 / 22.3 m and total 286 618 m² (about 940 m² per tree, several stacked layers).
- **`NW_NATURE_BRANCH_01`** — their lower branch cards, 3.0–12.9 m above ground (median 6.9 m), 2 703 pieces of 4 or 16 triangles, about 8 m across.
- **`NW_NATURE_BARK_04`** — their trunks and boughs: 300 main trunks found under crowns, height 12.2 / 19.0 / 21.2 m, diameter at 1.3 m 0.87 / 1.83 / 2.68 m. Collidable.
- **`NW_NATURE_TREE_NEEDLE_03`** — tiers of the 546 baked firs: 82 % near-horizontal, two-sided, tilted 33–51°, 4 988 pieces of 8 or 16 triangles, median 8.4 m across, 0.7–13 m above ground (median 4.9 m).
- **`NW_NATURE_BARK_02`** — their trunks: height 8.1 / 12.5 / 18.0 m (5.5–23.6 m), i.e. scale 0.70 / 1.09 / 1.56 of the 11.5 m vob fir (0.48–2.04); diameter at 1.3 m 0.39 / 0.69 / 0.97 m. Base sunk 1.2 m (median). Collidable.
- `NW_NATURE_BARK_03`: 9 thick bare trunks (10.7–15.4 m tall, 2.0–3.8 m thick, 570 triangles each). `NW_NATURE_LEAVE_03` (2 308 triangles, 10 chunks): bush cards baked into the mesh. `NEEDLE_02`: bare twig cards of tall firs.
- Not trees, though classed as vegetation: `GREATSEA_PLANT_01` (seabed plants), `FULLALPHA_01`, `GRASS_DECAL_01`, duckweed, water lilies, ivy, ropes, `BARKRINGS` (log ends; two 1 100-triangle log piles).

Crown cover on a 1 m grid (a cell is covered when a level-mesh card lies at least 2 m above its ground): 131 372 m² of 808 329 m² of land = **16 %**; with discs for vob crowns (0.85 of crown radius) 212 861 m² = **26 %**. In covered cells the lowest card is at 2.8 / 8.6 / 16.0 m above ground (p10/50/90), the highest at 7.2 / 18.4 / 25.0 m; thickness between them 2.0 / 7.0 / 15.2 m, 4 cards per cell at the median.

## 3. Pilot area (chunks 4,−2 … 7,1; 256 × 256 m)

| Measure | Value |
|---|---|
| Land (1 m cells above sea) | 64 333 m² |
| Trees, total | 299: 255 conifers, 44 broadleaf |
| — vobs | fir `TANNE_100P` 49, `TANNE_75P` 43; tall fir `HOHETANNE_91P` 13, `_59P` 12, `VERYHIGH` 1; fir clumps 4 (36 trunks); `BIGTREE_356P` 12; `SMALLTREE_103P` 6, `_79P` 3; dead `OW_LOB_TREE_V11` 3 |
| — level mesh | 133 firs, 20 big broadleaf |
| Stumps / logs | 14 / 1 |
| Bush vobs | 140: `BUSH_120P` 48, `BUSH_25P` 27, `LONG_BUSH_360P` 24, `BUSH_56P` 15, `REED_V1` 13, `BUSH_300P` 7, `BUSH_60P` 6 |
| Trees per hectare of land | 46 |
| Trees per hectare of crown-covered land | 128 |
| Nearest-neighbour distance between trees | 3.2 / 6.6 / 12.2 m (p10/50/90), mean 7.4 m |
| Land under level-mesh crowns | 13 045 m² = 20 % |
| Land under any crown (with vob discs) | 23 427 m² = 36 % |
| Vegetation triangles of the level mesh | 19 990 of 39 835: `NEEDLE_03` 9 589, `BARK_04` 3 456, `TREE_04` 1 838, `BRANCH_01` 1 544, `LEAVE_03` 1 008, `BARK_02` 587, `NEEDLE_02` 504 |
| Waypoints | 277; 50 under level-mesh crowns, 89 under any crown |

Conifers are 85 % of the trees, 92 % with clump trunks counted separately. The pilot README counts 161 vob "trees" (with stumps); the level mesh adds 153.

## 4. Whole island

| Kind | Vob | Level mesh | Total |
|---|---|---|---|
| Fir | 598 | 546 | 1 144 |
| Tall fir | 215 | – | 215 |
| Fir clump (×9 trunks) | 67 | – | 67 |
| Big broadleaf | 111 | 306 | 417 |
| Small broadleaf | 175 | – | 175 |
| Old half-bare broadleaf | 9 | – | 9 |
| Dead tree | 31 | – | 31 |
| Bare sapling | 286 | – | 286 |
| **Trees** | **1 492** | **852** | **2 344** |
| Stumps / logs / bushes | 139 / 117 / 1 652 | 9 thick bare trunks | |

Conifers 1 426 (61 %), broadleaf 632 (27 %), bare saplings 286 (12 %). Density: 25 trees per hectare of land, 97 per hectare of crown-covered land; nearest neighbour 3.5 / 7.1 / 13.5 m.

Per 64 m chunk with more than 400 m² of land (257 chunks): 104 have no tree; p50 4, p75 14, p90 22, max 42 (chunks 4_−3 and 5_0).

By region (prefix of the nearest waypoint; conifer / broadleaf / bare):

| Region | Con. | Bro. | Bare | Reads as |
|---|---|---|---|---|
| NW_TROLLAREA | 369 | 88 | 19 | fir forest |
| NW_XARDAS | 161 | 40 | 0 | fir forest |
| NW_CITY (outside the walls) | 112 | 37 | 37 | firs |
| NW_FARM1 (Akil) | 93 | 52 | 17 | mixed, firs lead |
| NW_CASTLEMINE | 80 | 6 | 0 | firs |
| NW_BIGFARM (Onar) | 63 | 49 | 0 | mixed |
| NW_PATH / NW_CITYFOREST / NW_FARM2 / NW_FARM3 | 61 / 58 / 55 / 47 | 9 / 11 / 15 / 22 | 0 | firs |
| NW_MONASTERY, NW_TAVERNE | 42, 49 | 2, 15 | 0 | firs |
| NW_FOREST (the dark forest in the north-east) | 69 | 157 | 4 | **big broadleaf** |
| NW_FARM4 (Sekob) | 35 | 61 | 0 | broadleaf |
| NW_RITUALFOREST | 0 | 13 | 0 | broadleaf |
| NW_PASS | 2 | 4 | 137 | bare saplings |

Conifers dominate everywhere except the dark forest, Sekob's farm and the ritual forest.

## 5. Textures and palette

Mean colour is taken in linear light over texels with alpha above 0.5 and converted back to sRGB.

| Texture | Size | Format | Opaque share | Mean sRGB (opaque texels) | HSV | Used by |
|---|---|---|---|---|---|---|
| `MOWOBARK01` | 256x256 | DXT1 | 1.00 | (64, 62, 57) #403e39 | 43°, 11 %, 25 % | OW_FOREST_HANGHIM_TREE_V3 |
| `MOWOBUSH02` | 256x256 | DXT3 | 0.10 | (12, 18, 9) #0c1209 | 98°, 48 %, 7 % | OW_LOB_BUSH_V5 |
| `MOWOBUSH04` | 256x256 | DXT3 | 0.18 | (76, 70, 39) #4c4627 | 49°, 49 %, 30 % | OW_NATURE_BUSH_02 |
| `MOWOREED01` | 128x128 | DXT3 | 0.45 | (46, 56, 7) #2e3807 | 72°, 87 %, 22 % | OW_LOB_BUSH_REED_V1 |
| `MOWOREED02` | 128x128 | DXT3 | 0.33 | (94, 99, 66) #5e6342 | 69°, 33 %, 39 % | OW_LOB_BUSH_REED_V1 |
| `MOWOROOT02` | 256x256 | DXT3 | 0.18 | (38, 37, 21) #262515 | 56°, 45 %, 15 % | OW_LOB_TREE_V11 |
| `NW_NATURE_BARKRINGS_01` | 512x512 | DXT1 | 1.00 | (108, 104, 93) #6c685d | 43°, 15 %, 42 % | BAUMSTUMPF_02_154P, BAUMSTUMPF_115P, BAUMSTUMPF_154P |
| `NW_NATURE_BARKRINGS_02` | 512x512 | DXT1 | 1.00 | (66, 62, 42) #423e2a | 50°, 36 %, 26 % | BAUMSTAMM_01_30P, BAUMSTAMM_166P, BAUMSTUMPF_02_115P, BAUMSTUMPF_03_174P |
| `NW_NATURE_BARK_01` | 256x256 | DXT1 | 1.00 | (104, 91, 70) #685b46 | 38°, 33 %, 41 % | SMALLTREE_103P, SMALLTREE_79P |
| `NW_NATURE_BARK_02` | 256x256 | DXT1 | 1.00 | (93, 81, 75) #5d514b | 19°, 19 %, 37 % | (level mesh), HOHETANNEN_01_531P, HOHETANNEN_02_531P, HOHETANNE_59P +5 |
| `NW_NATURE_BARK_03` | 256x1024 | DXT1 | 1.00 | (78, 67, 52) #4e4334 | 35°, 33 %, 31 % | (level mesh), BAUMSTAMM_01_30P, BAUMSTAMM_166P, BAUMSTUMPF_02_115P +4 |
| `NW_NATURE_BARK_04` | 256x1024 | DXT1 | 1.00 | (74, 64, 45) #4a402d | 38°, 39 %, 29 % | (level mesh), BIGTREE_356P, OW_NATURE_BIG_TREE_02_380P, OW_NATURE_OLDTREE_02 +1 |
| `NW_NATURE_BRANCH_01` | 512x512 | DXT3 | 0.40 | (65, 66, 2) #414202 | 61°, 97 %, 26 % | (level mesh), BIGTREE_356P |
| `NW_NATURE_BRANCH_02` | 256x512 | DXT3 | 0.29 | (76, 76, 3) #4c4c03 | 61°, 96 %, 30 % | (level mesh), BUSH_25P, SMALLTREE_103P, SMALLTREE_79P +1 |
| `NW_NATURE_BRANCH_03` | 256x512 | DXT3 | 0.30 | (76, 77, 3) #4c4d03 | 61°, 96 %, 30 % | (level mesh), BUSH_25P, SMALLTREE_103P, SMALLTREE_79P |
| `NW_NATURE_BRANCH_06` | 256x256 | DXT3 | 0.24 | (96, 79, 54) #604f36 | 35°, 44 %, 38 % | OW_NATURE_BUSH_01 |
| `NW_NATURE_BRANCH_07` | 256x256 | DXT3 | 0.33 | (43, 49, 33) #2b3121 | 81°, 32 %, 19 % | BUSH_01 |
| `NW_NATURE_LEAVE_01` | 256x256 | DXT3 | 0.62 | (92, 92, 32) #5c5c20 | 60°, 66 %, 36 % | BAUMSTUMPF_02_154P, BAUMSTUMPF_154P, BUSH_120P, BUSH_60P +1 |
| `NW_NATURE_LEAVE_03` | 128x256 | DXT3 | 0.26 | (95, 96, 32) #5f6020 | 61°, 67 %, 38 % | (level mesh), BUSH_120P, BUSH_300P, BUSH_56P +2 |
| `NW_NATURE_PILZ_01` | 128x128 | DXT1 | 1.00 | (154, 156, 70) #9a9c46 | 61°, 55 %, 61 % | BAUMSTUMPF_03_174P |
| `NW_NATURE_ROOTS_01` | 512x512 | DXT3 | 0.10 | (56, 48, 22) #383016 | 46°, 61 %, 22 % | OW_NATURE_SMALLTREE_01 |
| `NW_NATURE_TREE_04` | 512x512 | DXT3 | 0.52 | (51, 54, 1) #333601 | 63°, 98 %, 21 % | (level mesh), BIGTREE_356P |
| `NW_NATURE_TREE_NEEDLE_02` | 512x512 | DXT3 | 0.21 | (42, 36, 30) #2a241e | 25°, 28 %, 17 % | (level mesh), HOHETANNEN_01_531P, HOHETANNEN_02_531P, HOHETANNE_59P +3 |
| `NW_NATURE_TREE_NEEDLE_03` | 512x512 | DXT3 | 0.62 | (33, 40, 8) #212808 | 73°, 81 %, 16 % | (level mesh), HOHETANNEN_01_531P, HOHETANNEN_02_531P, HOHETANNE_59P +5 |
| `OW_NATURE_BIGTREE_01` | 512x512 | DXT3 | 0.32 | (82, 71, 47) #52472f | 41°, 43 %, 32 % | OW_NATURE_BIG_TREE_02_380P |
| `OW_NATURE_BUSH_02` | 512x512 | DXT3 | 0.20 | (58, 53, 35) #3a3523 | 46°, 40 %, 23 % | OW_NATURE_OLDTREE_02 |
| `PCODWATREE03` | 256x256 | DXT1 | 1.00 | (49, 46, 27) #312e1b | 52°, 45 %, 19 % | OW_LOB_TREE_V11 |

Palette to keep:
- needles: `NEEDLE_03` (33, 40, 8) — very dark olive, hue 73°, value 16 %;
- broadleaf crown: `TREE_04` (51, 54, 1), `BRANCH_01` (65, 66, 2), `BRANCH_02/03` (76, 76, 3) — yellow-olive, hue 61–63°, saturation 96–98 %, value 21–30 %;
- bush leaves: `LEAVE_01/03` (92–95, 92–96, 32) — the same hue 60°, lighter (value 36–38 %) and less saturated (67 %);
- bark: fir `BARK_02` (93, 81, 75), cool grey-brown, hue 19°; broadleaf `BARK_04` (74, 64, 45) and `BARK_03` (78, 67, 52), warm brown, hue 35–38°; small tree `BARK_01` (104, 91, 70).

All foliage is yellow-green with blue almost at zero; there is no blue-green or fresh green anywhere. Material colours in `world.json` agree: `TREE_04` (23, 22, 1), `NEEDLE_03` (19, 23, 5), `BARK_02` (83, 70, 64).

## 6. Proportions

In the table of section 1 (crown height / total height, crown width / height). In short: fir 0.86–0.89 and 0.93 (a cone as wide as it is tall, crown from 1.4–1.7 m); tall firs 0.36–0.46 and 0.37–0.47 (crown is the top third); big broadleaf 0.92–0.93 and 0.86 (the crown proper, `TREE_04`, starts at 5.8 m: 0.81); small broadleaf 0.84–0.88 and 0.43–0.45.

## 7. Gameplay

- **Collision.** Leaf and needle materials have `disable_collision`; bark collides. Vob `cd_dynamic` is on for 1 206 of 1 643 tree-class vobs (all firs `TANNE_75P`, 178 of 209 `TANNE_100P`, all big, old, dead and clump trees, 64 of 130 and 29 of 45 small broadleaf) and off for all 286 bare saplings; `cd_static` for 167. Bushes: on for 15 of 1 652 — bushes are walked through. Logs: 108 of 117. Baked trees: trunks and boughs (`BARK_02/03/04`) collide, crowns do not.
- **Names.** No tree has a landmark name; `vob_name` only repeats the mesh name (118 `BUSH_120P`, 56 `BUSH_56P`, 15 stumps). All are plain `zCVob`. One unique object: `OW_FOREST_HANGHIM_TREE_V3`, the gallows tree with a skeleton.
- **Wind.** `anim_mode` WIND or WIND_ALT is set on 207 bare saplings, 26 `SMALLTREE_79P`, 259 bushes and, oddly, 75 stumps; firs and big trees have none.
- **Waypoints.** Of 2 959, 456 lie under level-mesh crowns (140 in NW_FOREST, 40 NW_CITY, 38 NW_TROLLAREA, 35 NW_BIGFARM, 32 NW_XARDAS, 31 NW_FARM4) and 724 under any crown. 9 are within 1 m of a tree axis and 37 within 2 m: new trunks must stay as thin as the old ones there.
- **Replacing level-mesh trees** means cutting them out of the chunks: their trunks are collision and they sit in the same primitives as the rest of the world mesh.

## Not measured

- Screen look: lighting, how dark the crowns make the ground, draw distance and LOD of trees (no game launch).
- Which level-mesh trees hide caves, cliffs or the edge of the world, and which are seen only from afar.
- Vob crown cover is approximated by discs; real cover of fir tiers is lower.
- Trunk diameters are the extent of a 3–8-sided prism, not a round girth.
- Regions are named by the nearest waypoint, a rough split; 48 saplings are farther than 80 m from the waynet.
- ADDONWORLD, OLDWORLD and DRAGONISLAND trees are not covered.

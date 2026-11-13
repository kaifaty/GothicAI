# Step Д3, task 2: the cooked chunk `.wcb` measured before the format is fixed

Date: 2026-11-13. Plan: [`docs/landscape-plan.md`](../../../../docs/landscape-plan.md), «Д3 по задачам», task 2. Layout proposal: [`d3_design_scout.md`](d3_design_scout.md), section 3.

Question: what does reading the world cost today, stage by stage, what is left when each cell is read from a cooked binary chunk instead, and does the measurement ask for a change of the layout before later tasks build on it.

The sections up to «What the numbers say about the layout» record version 1 (task 2). Task 2b changed the layout after the override question: [«Version 2»](#version-2-2026-11-13) at the end has the layout as built now, with its index, and the refreshed numbers.

## Machine and commands

Ryzen 3950X (16 cores, 32 threads), 30 GB RAM, NVMe, Ubuntu; GCC, `RelWithDebInfo` with `-O2 -g1` (`scripts/ryzen-build.sh`); branch `codex/landscape-d3-wcb` at `b11be919a`. Every run went through `scripts/gq` class `quick` (4 threads, 12 GB) in `~/gothic-ai/landscape-d3-wcb/`. The code is single-threaded. The machine was not idle: load average 4–6, and another session's `quiet` job was running at the time. All timings are with a warm page cache: `time-world` reads every file once before the runs, and dropping the cache needs `sudo`. Cold reads were not measured.

Base: the Ryzen cache base `c885037e…d09e` (×1). The ×16 set is a copy of that base with every world triangle split in four twice; static meshes are left as they are because the measurement only reads worlds:

```
B=~/.cache/gothic-ai/base/c885037e0d829cfdaea7fc5db0b034d11eda6d678b30446ced78a39e36dfd09e
content-convert stress-base $B $T/w16 --subdivide 2 --meshes 0      # 40.1 s, peak RSS 3.9 GB; cooks every world it writes
content-convert verify-stress-base $B $T/w16                        # passed, 49.9 s
content-convert cook-world $B/worlds/<W> --out $T/x1-cooked/<W>     # the base stays untouched
content-convert verify-cooked-world $B/worlds/<W> --out $T/x1-cooked/<W>   # passed for all four worlds
content-convert verify-cooked-world $T/w16/worlds/<W>                     # passed for all four worlds
content-convert time-world $B/worlds/NEWWORLD --out $T/x1-cooked/NEWWORLD
content-convert time-world $T/w16/worlds/NEWWORLD
```

`verify-cooked-world` builds the world mesh and chunk table from the `.wcb` files alone (`assembleCookedWorld`). It compares them field by field with `readWorldChunks` and checks every GLB against its chunk's length and SHA-256. On ×16 `NEWWORLD` this takes 10.1 s with a peak RSS of 3.4 GB.

## The layout as built

*This section describes version 1, as task 2 measured it. Version 2 and its index are in [«Version 2»](#version-2-2026-11-13) below.*

Little-endian, uncompressed, one file per cell beside its GLB (`chunks/4_-2.glb` → `chunks/4_-2.wcb`). Code: [`OpenGothic/common/worldchunkbinary.h`](../../../../OpenGothic/common/worldchunkbinary.h), whose header comment holds the byte table.

| Part | Size | Content |
|---|---|---|
| Header | 144 bytes | magic `GWCB`, version 1, byte order mark, header size, file size; cell `i32×2`; counts of this file's positions, features, triangles and materials `u32×4`; the cell's table bounds `f32×6`; length `u64` and SHA-256 of the GLB it was cooked from; size `u64` and CRC32 of the source `.ZEN` from `world.json`; section count; directory offset |
| Directory | 32 bytes per section | type, stride, count, zero `u32`; offset, length `u64`. Sections are 16-byte aligned |
| Positions | 16 | `f32×3`, stitched vertex id `u32` |
| Features | 28 | UV `f32×2`, light `u32`, normal `f32×3`, stitched feature id `u32` |
| Triangles | 44 | local position indices `u32×3`, local feature indices `u32×3`, stitched material `u32`, GLB `order` `u32` (`0xFFFFFFFF` = added by an edit), stitched rank `u32`, lightmap `i32`, flags `u16`, sector `i16`. The array index is the triangle's ordinal in its GLB |
| Materials | 88 | stitched id `u32`; name, texture and detail object as (offset, size) `u32×2` each; twelve `f32`; group, RGBA, animation mapping, wave mode and speed, alpha function as bytes; six booleans as a bit mask |
| Strings | 1 | the bytes of the material names |

Opening validates once: the header, the directory, each section's stride, alignment, bounds and the `count × stride = length` rule, and each header count against its section. It also checks every triangle's local indices and every material's strings. After that the spans point into the bytes without copying.

Where it differs from the scout's table, and why:

- **Local indices into the file's own arrays**, and the resolved ids sit in the position and feature records. This keeps a chunk self-contained: a reader of one cell needs no world-sized table, and the stitched ids come from the records. The indices stay `u32`. At ×1 the largest cell has 11 519 vertices and 26 048 features. At ×16 the largest has 182 334 and 232 448, and 36 of 1 643 cells exceed 65 535, so `u16` cannot be the only width.
- **Materials hold only what the cell's triangles use**, sorted by stitched id. The ids below the `world.json` table are always its records (`readWorldChunks` keeps a source index only when the record is the same). Materials an edit added get ids after it and are carried by the cells that use them.
- **The header names the source world** (`.ZEN` size and CRC32), so a chunk cooked for another world is rejected. There is no catalog identity yet, because there is no catalog yet (tasks 7–8). The override question below shows that such an identity is needed.
- **No draw or cloth selection lists.** Cloth is task 4, and no reader needs a draw list before task 3.
- **No world totals in the header.** The assembler takes the vertex and feature counts as the larger of the `world.json` count and the highest id plus one, and the unreferenced ones from `unreferenced.bin`.

## Bytes on disk

Cooked from the same GLBs, sums over each world's cells:

| World | ×1 GLB | ×1 `.wcb` | ×16 GLB | ×16 `.wcb` |
|---|---:|---:|---:|---:|
| NEWWORLD | 49 752 492 | 46 411 212 | 426 863 616 | 543 112 655 |
| ADDONWORLD | 32 535 696 | 31 344 311 | 293 914 808 | 380 664 980 |
| OLDWORLD | 11 882 856 | 9 253 587 | 83 081 684 | 103 185 119 |
| DRAGONISLAND | 9 386 448 | 8 590 852 | 81 834 876 | 105 832 932 |

The 44-byte triangle records are 45.4 % of all ×1 `.wcb` bytes and 61.3 % at ×16. At ×16 `NEWWORLD` they take 333 345 408 of 543 112 655 bytes.

The Near ring is the 5 × 5 cells around `NEWWORLD`'s start point `START_GOTHIC2` (29913, 5186, −15710), cell 4,−3:

| Set | Cells | Triangles | GLB bytes | `.wcb` bytes |
|---|---:|---:|---:|---:|
| ×1 | 25 | 63 337 | 6 623 688 | 6 286 182 |
| ×16 | 25 | 1 013 458 | 57 188 472 | 72 602 701 |

## Wall time, `NEWWORLD`

Five runs each, milliseconds: median, then min–max. The rows under "readWorldChunks total" are its stages, taken from marks in `readWorldChunks` (`WorldChunkTrace`). In each run they add up to the total:

- read + parse: from the call to the end of the reader loop. This covers `world.json`, each GLB's bytes (again, from the page cache), the cgltf parse and validation, and the attribute extraction into the reader's corners.
- sort: the `seen` set of orders and the stable sort into source order.
- resolution and stitching: `unreferenced.bin`, the material map, vertex and feature ID resolution with the maps of split and added keys, the polygon arrays and the chunk table.
- rest: the trace's copy of the orders, which the game's call does not make, and the destruction of the reader's triangles and of the order set.

| Stage | ×1 | ×16 |
|---|---:|---:|
| today: read all GLB files (a separate pass) | 8.0 (7.8–8.1) | 49.5 (48.7–54.8) |
| today: `readWorldChunks` total | 529.0 (520.5–543.7) | 7548.6 (7524.6–7920.7) |
| read + cgltf parse + attribute extraction | 377.4 (373.6–383.3) | 5065.4 (5055.8–5190.5) |
| source-order sort | 79.5 (78.3–83.2) | 1411.1 (1389.7–1598.0) |
| ID resolution, dedup maps, stitching, table | 35.3 (34.3–49.5) | 565.4 (544.4–623.8) |
| rest: trace copy, teardown | 34.3 (34.1–36.1) | 509.3 (505.9–516.8) |
| today: `ChunkMeshSource::read` of the 25 Near cells (after the stitched load) | 71.5 (68.6–76.2) | 851.7 (845.7–864.1) |
| cooked: read all `.wcb` files | 8.7 (8.7–9.0) | 81.3 (78.2–113.9) |
| cooked: validate (open every chunk, `world.json`, a GLB `stat` per cell) | 7.2 (7.1–7.4) | 24.9 (24.3–27.1) |
| cooked: assemble the whole stitched mesh and table | 29.1 (28.7–29.6) | 391.1 (384.7–454.5) |
| cooked: read and validate the 25 Near `.wcb` | 1.0 (0.9–1.0) | 7.7 (7.4–8.7) |

An earlier run of the same binary without the Near rows gave the same figures within 2 %: ×16 total 7663.7 ms, parse 5116.7, sort 1476.5, stitching 586.6; cooked read 81.1, validate 24.6, assemble 391.0.

## The override question

*When an override directory is present, do the cooked records of cells that the override does not replace differ from the records cooked without it?* Yes, in two fields. `gothic-wcb-contract` checks this on a strip of three cells, `0_0`, `1_0` and `2_0`, with an override of `1_0`:

| Override of `1_0` | `0_0` (not replaced) | `2_0` (not replaced) |
|---|---|---|
| moves an inner vertex, drops source triangle 15 and fills its place with two triangles around a new vertex, with a new material | nothing differs | `triangles.rank` |
| the same and also moves the vertex `1_0` shares with `0_0`, where `1_0` holds order 0 | `positions.id` | `triangles.rank` |

Why:

- **`rank`** is the triangle's index in the stitched mesh: source triangles sorted by `order`, then the edited ones. An override that drops source triangles shifts the rank of every later source triangle in every cell. In the world that is most cells, because `order` is the `.ZEN`'s polygon order, not a spatial one. Any edit that removes source geometry does this; `world_edit.py`'s cut roofs are such edits.
- **The vertex (or feature) id** of a shared vertex goes to the copy that comes first in source order, and the other copy is appended. When an override moves a vertex it shares with a neighbour and its own triangle comes first, the neighbour's corner gets an appended id. `world_edit.py` refuses to move border vertices (`border_violations` in `chunk_glb.py`), so this comes only from GLBs edited outside it. `readWorldChunks` accepts them.
- Everything else of a cell that is not replaced stays the same: values, local indices, material, `order`, lightmap, flags, sector, the material records, counts and bounds.

So a base cell's `.wcb` cooked with an override is not the one cooked without it, and checking its own GLB, by length or by hash, cannot tell the two apart. This is reported here, not designed around. Each later reader has to choose one of these: cook per world plus override set and name the set in the header, or keep only fields that do not depend on the override (`order`, source ids with a split marker) and rebuild rank and appended ids at load.

## What the numbers say about the layout

1. **Today's cost at ×16:** two thirds of `readWorldChunks` is the cgltf parse and attribute extraction (5.07 of 7.55 s). The source-order sort takes 19 % (1.41 s), and ID resolution and stitching 7.5 % (0.57 s). Reading the GLB bytes is 49.5 ms. The streaming reader's 25 Near cells cost another 852 ms on top, because each one parses its GLB again.
2. **What is left with `.wcb`:** reading and validating every cell of the ×16 world takes 106 ms (81 + 25), 71 times less than `readWorldChunks`. The 25 Near cells take 7.7 ms, and at ×1 1.0 ms. Assembling the whole stitched mesh from the chunks costs 391 ms at ×16. That is the price of stitching alone, which task 8 avoids by reading only the Near ring.
3. **What may change in the layout:** at ×16 the 44-byte triangle record makes `.wcb` 27 % larger than the GLB (61 % of its bytes). Narrow local indices in cells under 65 536 vertices and features would save 12 bytes a triangle, but reading is not the cost here, so this can wait for cold-read numbers. Before the format is frozen it must be decided whether `rank` and the appended ids stay in the file, because they depend on the whole override set (above).

## Version 2 (2026-11-13)

Task 2b answers the override question by the second choice above: a `.wcb` keeps only what follows from its own GLB and `world.json`, and what depends on the whole world and override set moves to one index per set, `chunks.wci`. Code: [`worldchunkbinary.h`](../../../../OpenGothic/common/worldchunkbinary.h) (the cell) and [`worldchunkindex.h`](../../../../OpenGothic/common/worldchunkindex.h) (the index); both header comments hold the byte tables.

### The layout as built

The cell, `.wcb` version 2. Header and directory as in version 1, with version 2. Positions, features and materials are in first use: the order in which the GLB's triangles, by ordinal and then corner, first name them.

| Part | Size | Content |
|---|---|---|
| Header | 144 bytes | as version 1 |
| Directory | 32 bytes per section | as version 1 |
| Positions | 16 | `f32×3`; the source key `u32`: the GLB's `_VERTEX`, `0xFFFFFFFF` without one |
| Features | 28 | UV `f32×2`, light `u32`, normal `f32×3`; the source key `u32`: the GLB's `_FEATURE` or `0xFFFFFFFF` |
| Triangles | 40 | local position and feature indices `u32×3` each, local material `u32`, GLB `order` `u32` (`0xFFFFFFFF` = added by an edit), lightmap `i32`, flags `u16`, sector `i16`; no rank |
| Materials | 88 | the `world.json` index when `world.json` holds the same record there, else `0xFFFFFFFF`; then the record as in version 1 |
| Strings | 1 | as version 1 |

The index, `chunks.wci` version 1. It lies in the world directory, in the override directory when there is one, and in `--out` when that is given.

| Part | Size | Content |
|---|---|---|
| Header | 80 bytes | magic `GWCI`, version 1, byte order mark, header size, file size; size `u64` and CRC32 of the source `.ZEN`; cell count; the stitched mesh's vertices, features, materials and triangles `u32×4`; source triangles `u32`, the rank of the first added triangle; section count; directory offset |
| Directory | 32 bytes per section | as in `.wcb` |
| Cells | 80 | in `world.json` order: cell `i32×2`; length `u64` and SHA-256 of the GLB the numbers came from; `addedStart` `u32`, the rank of the cell's first added triangle; first and count `u32×2` of the cell's vertex, feature and material exceptions; zero `u32` |
| Missing orders | 4 | ascending: the source orders below the highest held that no cell holds |
| Vertex, feature, material exceptions | 8 each | local index `u32`, stitched id `u32`; grouped by cell in cell order, ascending by local index |
| Materials | 88 | the appended materials in stitched order, in the `.wcb` record with source `0xFFFFFFFF` |
| Strings | 1 | the bytes of their names |

The stitched mesh from these: a source triangle of order *o* has the rank *o* minus the missing orders below *o*; the *k*-th added triangle of a cell, by GLB ordinal, has the rank `addedStart` + *k*; a local position, feature or material has its source key as stitched id unless the cell's exceptions give another one. Geometry and materials without a source key always have one. An exception only ever names an id past `world.json`'s counts, because below them `readWorldChunks` gives a key its own id. The index alone places a single cell's triangles and ids, so a Near reader needs the index and its own cells, not the others.

Where it differs from the brief, and why:

- **The header also holds the source triangle count.** With it a reader of one cell can tell a source rank from an added one without the other cells. The reader checks that each cell's `addedStart` plus its own added count is the next cell's `addedStart`.
- **The missing orders end at the highest order held**, not at `world.json`'s triangle count: a missing order past it would shift no rank.
- **The source keys come from `readWorldChunks`' trace.** The stitched result does not hold the `_VERTEX` and `_FEATURE` of a moved or added copy, only its new id, so `WorldChunkTrace` now also carries each stitched corner's source ids. `readWorldChunks` behaves as before; it fills them only when a trace is given, and the game passes none. `compareWorldMeshes` moved unchanged to `worldchunkmanifest.cpp`, so `worldchunksruntime.cpp` went from 689 to 654 lines.
- **Materials are in first use**, not sorted by stitched id as in version 1, because the stitched id of an appended material depends on the set.

Every cell field turned out to depend only on the cell's GLB and `world.json`. The bounds are a minimum and maximum over the cell's own triangles in their own order (source orders ascending, then added ones by ordinal), which is the same in every set. `gothic-wcb-contract` checks this byte for byte on the strip of the override question:

- `0_0` and `2_0` cooked with either override of `1_0` equal their cooks without it;
- with `1_0` and `2_0` both overridden, each adding a vertex and a material, each `.wcb` equals its cook with only that cell overridden. The sets number `2_0`'s new material 2 and 3 and its new vertex 27 and 28;
- `1_0` (edited) and `2_0` cooked in a world directory without cell `0_0` equal their cooks in the three-cell world.

### Machine and commands

As in task 2: Ryzen, `RelWithDebInfo` with `-O2 -g1` through `scripts/ryzen-build.sh`, `scripts/gq` class `quick` (4 threads, 12 GB), in `~/gothic-ai/landscape-d3-wcb-index/`, branch `codex/landscape-d3-wcb-index` at `2dfc248ea`. Load average 3.6–4.6 during the runs, warm page cache, cold reads not measured. The same base `c885037e…d09e` (the contract's `base: cache` line names the same directory as in task 2) and the same commands, ×16 built anew with `stress-base --subdivide 2 --meshes 0`. `content-convert --self-test` passed. `verify-cooked-world` passed on the four worlds ×1 (cooked with `--out`; the base has no `.wcb` or `.wci` after the runs) and ×16; ×16 `NEWWORLD` took 9.5 s with a peak RSS of 3.4 GB. A second `cook-world` of each ×1 world wrote no cell and left the index.

### Bytes on disk

| World | ×1 `.wcb` | ×1 index | ×16 `.wcb` | ×16 index |
|---|---:|---:|---:|---:|
| NEWWORLD | 44 516 364 | 32 544 | 512 807 471 | 37 344 |
| ADDONWORLD | 30 015 911 | 29 984 | 359 423 252 | 32 544 |
| OLDWORLD | 8 900 531 | 34 624 | 97 554 255 | 36 704 |
| DRAGONISLAND | 8 218 276 | 23 504 | 99 880 004 | 26 064 |

Against version 1, `.wcb` is 4.1 % smaller at ×1 and 5.6 % at ×16, from the four bytes of rank per triangle. At ×16 `NEWWORLD` `.wcb` is 20 % larger than the GLB (version 1: 27 %); the 40-byte triangle records are 59 % of its bytes. The Near ring of `NEWWORLD` is 6 032 806 bytes at ×1 and 68 548 829 at ×16.

The index of a base world holds no missing orders, exceptions or appended materials: 304 bytes plus 80 per cell, 32 544 bytes for `NEWWORLD`'s 403 cells. An edit adds 4 bytes per source triangle it drops, 8 per exception, and 88 plus its names' bytes per appended material, each section padded to 16 bytes. On the contract's strip of three cells the index is 544 bytes without an override and 730 bytes with either override of `1_0` (missing order 15; one vertex, one feature and one material exception for `1_0`; with the border move one more vertex exception for `0_0`; one appended material). With `1_0` and `2_0` both overridden it is 838 bytes.

### Wall time, `NEWWORLD`

Five runs each, milliseconds: median, then min–max. The cooked rows now include reading and validating the index. The "rest" stage includes the trace's copy of the corners' source ids added in this task (the game's call makes no trace).

| Stage | ×1 | ×16 |
|---|---:|---:|
| today: read all GLB files (a separate pass) | 8.2 (8.2–8.4) | 48.9 (46.7–52.4) |
| today: `readWorldChunks` total | 493.2 (488.7–515.1) | 7021.2 (6989.9–7465.6) |
| read + cgltf parse + attribute extraction | 330.4 (326.0–337.6) | 4266.5 (4252.0–4443.8) |
| source-order sort | 78.8 (76.8–81.5) | 1394.3 (1382.4–1599.2) |
| ID resolution, dedup maps, stitching, table | 36.3 (35.8–50.0) | 604.4 (595.9–656.2) |
| rest: trace copy, teardown | 48.2 (47.3–49.2) | 760.0 (751.1–766.5) |
| today: `ChunkMeshSource::read` of the 25 Near cells | 66.9 (63.7–69.3) | 757.3 (751.1–759.4) |
| cooked: read all `.wcb` files and the index | 9.1 (8.9–9.3) | 73.1 (70.5–92.3) |
| cooked: validate (open the index and every chunk, `world.json`, a GLB `stat` per cell) | 7.4 (7.3–7.7) | 25.6 (24.2–25.7) |
| cooked: read and validate the index alone | 0.1 (0.1–0.1) | 0.1 (0.1–0.1) |
| cooked: assemble the whole stitched mesh and table | 23.8 (23.6–24.3) | 436.9 (433.0–452.5) |
| cooked: read and validate the index and the 25 Near `.wcb` | 1.1 (1.1–1.2) | 7.6 (7.3–9.1) |

At ×16, reading and validating every cell and the index takes 99 ms (73 + 26), the index alone 0.1 ms, the Near ring with its index 7.6 ms (version 1: 7.7 ms). Assembling the whole mesh takes 437 ms, 12 % more than version 1's 391 ms: ids now come from the source key or an exception per local index, and every triangle's rank from the index. The cgltf parse ran 16 % faster than in task 2 with the same parsing code (4.27 against 5.07 s); the machine's load differed between the runs, so the "today" rows of the two sections are not a comparison.

### What changed against version 1

1. A `.wcb` no longer holds a rank or resolved ids: positions and features carry the GLB's source keys, triangles a local material and no rank (40 bytes instead of 44), materials their `world.json` index or none. A cell's file depends only on its GLB and `world.json`, so base cells cooked once stay valid under any override set, and a second cook rewrites nothing that did not change.
2. What depends on the set — missing orders, where each cell's added triangles start, exceptions and appended materials — is the index `chunks.wci`: 304 + 80 bytes per cell without edits, growing with the edit, not with the world. An edit of one cell re-cooks that cell and the index.

## Task 3: the game's reader (2026-11-13)

`ChunkMeshSource::read`, which the streaming manager calls each time a cell enters the ring, now takes the cell from its `.wcb` and the set's index ([`worldchunkcookedsource.h`](../../../../OpenGothic/common/worldchunkcookedsource.h)). It does so when the index found at world load names the same source world, lists the same cells in the same order and carries the totals `readWorldChunks` has just produced, the cell's GLB has the length of the index's entry, and the `.wcb` carries that entry's GLB length and SHA-256. Otherwise the cell is read from its GLB, with the same result; `OPENGOTHIC_WCB=0` turns the cooked path off. The start of the game is unchanged: `readWorldChunks` still parses every GLB, and now also reads and checks the index (0.1 ms).

### Machine and commands

Ryzen as above; the branch's `content-convert` from the check build (`RelWithDebInfo`, `-O2 -g1`), branch `codex/landscape-d3-wcb-source` at `1b287205f`, one `scripts/gq` job of class `quick` in `~/gothic-ai/landscape-d3-wcb-source/`. Load average 3.5–5.1 during the runs, warm page cache. The game reads cooked files only beside the GLBs (it ignores `--out`), so both sets are copies of the base `c885037e…d09e` that `stress-base` cooks as it writes them:

```
content-convert stress-base $B $T/x1  --subdivide 0 --meshes 0      # 8.4 s
content-convert stress-base $B $T/w16 --subdivide 2 --meshes 0      # 40.1 s, peak RSS 4.2 GB
content-convert verify-cooked-world $T/<set>/worlds/NEWWORLD        # passed for both
content-convert time-world $T/<set>/worlds/NEWWORLD
```

### Wall time, `NEWWORLD`

Five runs each, milliseconds: median, then min–max. The GLB row reads with `currentFile=true`, which always takes the GLB path; with the files unchanged it reads what `OPENGOTHIC_WCB=0` reads. The triangle lists are now built before the timed region in both rows.

| Stage | ×1 | ×16 |
|---|---:|---:|
| `ChunkMeshSource::read` of the 25 Near cells, GLB path | 67.0 (63.5–74.0) | 839.7 (831.4–842.2) |
| `ChunkMeshSource::read` of the 25 Near cells, cooked path | 6.8 (6.3–9.4) | 42.4 (39.7–43.5) |
| for comparison: read and validate the index and the 25 Near `.wcb` | 1.0 (1.0–1.1) | 7.7 (7.3–7.8) |
| `readWorldChunks` total, with the index read | 540.8 (517.6–546.0) | 7924.4 (7898.5–8262.9) |

The cooked read is 10 times faster at ×1 and 20 times at ×16 (1 013 458 triangles in the Near ring). Of its 42 ms at ×16, 7.7 ms read and validate the files. The rest builds the result as the GLB path does: a stitched id for every local index, the five polygon arrays, the compact vertices and features, and a copy of the world's materials table per cell. `readWorldChunks` ran 9.6 % (×1) and 12.9 % (×16) slower than in the version 2 rows above; its code changed only by the index read, and the machine's load differed.

`gothic-wcb-source-contract` reads every cell of the ×1 base `NEWWORLD`, cooked into a scratch copy, twice on each path: 806 reads take 101.9 ms cooked against 772.8 ms from the GLBs.

### In the game

The flight bench `scripts/bench.sh streaming-flythrough` on Ryzen with the ×1 copy (`OPENGOTHIC_BASE=$T/x1`, its own `OPENGOTHIC_BASE_CACHE`, `OPENGOTHIC_CONTENT=` and `OPENGOTHIC_TEXTURES=` empty), once with the cooked path and then once with `OPENGOTHIC_WCB=0`. The cooked run came first, with cold derived and pipeline caches. Both exited 0. The `[world-chunks]` line the bench now logs at the end of each pass counts reads since world load:

| | cooked path | `OPENGOTHIC_WCB=0` |
|---|---:|---:|
| bench | exit 0, 202 s | exit 0, 197 s |
| index line at load | 403 of 403 cells from `.wcb` | none (path off) |
| reads cooked / GLB | 403 / 0 | 0 / 403 |
| their summed wall time | 122.3 ms | 568.6 ms |
| `landscape_pack`: the 25 Near cells read and packed at the start | 34.7 ms | 99.2 ms |
| warm-up: 359 Far publications, sum / max | 80.8 / 0.99 ms | 82.2 / 1.09 ms |
| frames, whole route: FPS, p99 | 104.8, 12.2 ms | 105.6, 11.8 ms |

Every cell was read once, during load and warm-up: in this route no cell leaves the Far ring, so the passes publish nothing and their counters stay at 403. The read time counts the Far reads on the packing thread as well as the Near reads on the main thread. Frame times are close on both paths; with one run each there is no noise estimate.

## Task 4: world cloth (2026-11-13)

`WorldCloth::extract` searches the stitched mesh for cloth islands, which cross cell borders. The cook now runs that search once per world and override set and writes its result beside the index as `cloth.wcl` ([`worldclothbundle.h`](../../../../OpenGothic/common/physics/worldclothbundle.h) holds the byte table): the surfaces, the original mesh, and per cell the ascending local triangle ordinals taken out as cloth. At load `extract` takes it when the cooked index is in use, every GLB had its index entry's length, and the bundle names the same source world, the SHA-256 of the index bytes the load read, this build's rules hash and `WorldCloth::Algorithm`. Otherwise, and on any failure, it runs the island search as before. The landscape and the chunk table come from the same `retainWorldTriangles` call on both paths. `OPENGOTHIC_WCB=0` turns the bundle off with the index. The whole cook now runs under an exclusive lock on the index's directory (`flock`; a named mutex on Windows, not run here).

### Machine and commands

Ryzen as above; the branch's `content-convert` from the check build (`RelWithDebInfo`, `-O2 -g1`), branch `codex/landscape-d3-cloth`, one `scripts/gq` job of class `quick` (4 threads, 12 GB) in `~/gothic-ai/landscape-d3-cloth/`. Load average 1.5–3.7 during the runs, warm page cache. The same base `c885037e…d09e`:

```
content-convert --self-test                                                # passed
content-convert cook-world $B/worlds/<W> --out $T/x1-cooked/<W>           # then verify-cooked-world, then a second cook
content-convert stress-base $B $T/x1  --subdivide 0 --meshes 0            # 8.6 s
content-convert stress-base $B $T/w16 --subdivide 2 --meshes 0            # 41.3 s, peak RSS 4.2 GB
content-convert verify-cooked-world $T/w16/worlds/<W>
content-convert time-world $T/<set>/worlds/NEWWORLD
```

`verify-cooked-world` now also compares the bundle with a fresh island search on the stitched mesh. It passed on the four worlds at ×1 (cooked with `--out`; the base held no `.wcb`, `.wci` or `.wcl` before and after) and at ×16; ×16 `NEWWORLD` took 10.7 s with a peak RSS of 3.4 GB. A second `cook-world` of each ×1 world wrote no cell and left the index and the bundle.

### Bytes and surfaces

| World | ×1 surfaces, triangles | ×1 bundle | ×16 surfaces, triangles | ×16 bundle |
|---|---:|---:|---:|---:|
| NEWWORLD | 17, 1 280 | 187 979 | 10, 20 032 | 2 358 628 |
| ADDONWORLD | 0, 0 | 3 504 | 0, 0 | 3 760 |
| OLDWORLD | 0, 0 | 3 968 | 0, 0 | 4 176 |
| DRAGONISLAND | 10, 1 252 | 179 876 | 10, 20 032 | 2 357 828 |

A world without cloth costs 8 bytes per cell plus about 500 bytes of header, directory and the empty original mesh. At ×16 `NEWWORLD` has 10 surfaces instead of 17: the subdivided city banners have 16 triangles and no longer pass the rule's four-position, four-triangle filter. This is how the island search treats the stress set, the same on both paths.

### Wall time, `NEWWORLD`

Five runs each, milliseconds: median, then min–max. The island search row runs on a copy of `readWorldChunks`' mesh and table and includes its own `retainWorldTriangles`. The bundle rows read and check the file against the index, then apply it to the loaded mesh and table: the exclusions against the table, then the same `retainWorldTriangles`.

| Stage | ×1 | ×16 |
|---|---:|---:|
| `WorldCloth::extract` by island search | 21.8 (21.4–23.6) | 359.1 (343.1–368.9) |
| read and validate the bundle | 0.1 (0.1–0.1) | 0.4 (0.4–0.4) |
| apply the bundle (exclusions, `retainWorldTriangles`) | 10.0 (9.9–10.4) | 209.1 (182.6–226.2) |
| for comparison: `readWorldChunks` total | 533.1 (507.4–541.7) | 7724.3 (7666.1–8128.4) |

The bundle saves 12 ms of the island search at ×1 and 150 ms at ×16. What is left is mostly `retainWorldTriangles`, which copies the five polygon arrays of the whole stitched mesh on either path. Its real gain comes with task 8: a start without the stitched mesh needs the bundle, not the search.

### In the game

`scripts/bench.sh streaming-flythrough` on Ryzen (`BENCH_HOST=ryzen`) with the ×1 copy (`OPENGOTHIC_BASE=$T/x1`, its own `OPENGOTHIC_BASE_CACHE`, `OPENGOTHIC_CONTENT=` and `OPENGOTHIC_TEXTURES=` empty), once with the cooked path and then once with `OPENGOTHIC_WCB=0`. The look of the cloth was not assessed: nothing in its simulation or drawing changed.

| | cooked path | `OPENGOTHIC_WCB=0` |
|---|---|---|
| bench | exit 0, 203 s | exit 0, 197 s |
| `[world-cloth]` line | `bundle …/x1/worlds/NEWWORLD/cloth.wcl: 17 surfaces, 1280 triangles` | none: no index in use, the island search ran |
| `cloth_surfaces` load mark | `surfaces=17` (29.3 ms) | `surfaces=17` (27.5 ms) |
| chunk reads cooked / GLB | 403 / 0, 128.2 ms | 0 / 403, 534.1 ms |

## Task 5: the collision input (2026-11-13)

Static collision packed the whole stitched landscape (`PackedMesh PK_Physic` in `DynamicWorld`'s constructor). The world load now builds that input from the cooked records instead ([`worldcollisionmesh.h`](../../../../OpenGothic/common/physics/worldcollisionmesh.h)): a slim `asset::WorldMesh` with the vertices, the vertex and material indices and the materials table, made from every cell's `.wcb` and the index by the assembler the converter's checks use (`assembleCookedWorld` with `CookedParts`, now in `worldchunkassemble.cpp`), with the world cloth bundle's exclusions left out. It is used when the index is in use, no GLB had another length at load, `WorldCloth::extract` took the bundle and every `.wcb` passes `checkCookedCell`; otherwise the stitched landscape, with one `[world-collision]` line. Until task 8 the stitched mesh still exists at load, so the cells are read a second time; the numbers below are that cost.

### Machine and commands

Ryzen as above; the branch's `content-convert` and `gothic-worldcollision-chunks-contract` from the check build (`RelWithDebInfo`), branch `codex/landscape-d3-collision`, `scripts/gq` jobs of class `quick` (4 threads, 12 GB) in `~/gothic-ai/landscape-d3-collision/`, the same base `c885037e…d09e`, warm page cache. Load average 1.5–3.4 for `time-world`; the second `--time` run had a build of 12 threads beside it (load 11–15).

```
content-convert stress-base $B $T/x1  --subdivide 0 --meshes 0            # 8.3 s, peak RSS 0.29 GB
content-convert stress-base $B $T/w16 --subdivide 2 --meshes 0            # 41.5 s, peak RSS 4.16 GB
content-convert time-world $T/<set>/worlds/NEWWORLD                       # new row: the collision mesh
gothic-worldcollision-chunks-contract --time $T/<set>/worlds/NEWWORLD     # the builder and PK_Physic of both inputs
```

`content-convert` links no graphics code, so `PackedMesh PK_Physic` is timed by the contract's `--time` mode, which loads the world as the game does (`readWorldChunks`, `extract`) and packs both inputs. The collision mesh of `NEWWORLD`: ×1 — 472 222 triangles (1 280 taken out as cloth), 260 696 vertices, 1 400 materials, 804 `PK_Physic` sub meshes; ×16 — 7 556 000 triangles (20 032 as cloth), 3 505 786 vertices.

### Wall time, `NEWWORLD`

Five runs each, milliseconds: median, then min–max. «Read + build» is the whole builder: world.json, every `.wcb` read and checked against the index and its GLB's length, the bundle read and checked, the placing and the removal of the cloth triangles.

| Stage | ×1 | ×16 |
|---|---:|---:|
| `time-world`: collision mesh from chunk records (read + build) | 23.6 (23.6–24.8) | 390.6 (383.0–392.2) |
| `--time`: the same builder | 24.8 (23.6–28.4) | 249.4 (248.2–266.1) |
| `--time`: `PackedMesh PK_Physic` of the stitched landscape | 28.0 (27.5–28.3) | 519.0 (517.4–526.0) |
| `--time`: `PackedMesh PK_Physic` of the collision mesh | 28.2 (27.0–28.2) | 525.1 (509.8–527.3) |
| for comparison: read the `.wcb` files and the index | 8.5 (8.4–8.9) | 102.1 (96.8–104.7) |
| for comparison: validate them | 7.0 (7.0–7.1) | 28.5 (25.7–31.2) |
| for comparison: assemble the whole mesh and table | 26.4 (26.1–27.7) | 464.9 (462.0–486.3) |
| for comparison: apply the cloth bundle | 9.9 (9.9–10.2) | 229.2 (219.4–232.3) |
| for comparison: `readWorldChunks` total | 508.5 (506.1–540.2) | 7817.2 (7714.6–8250.4) |

`PK_Physic` does the same work on both inputs (within 1 %). In a first `--time` run both packages were alive at once and the second one, always the collision mesh, came out 7 % (×1) and 15 % (×16) slower; packed one at a time in alternating order they are equal. The builder at ×16 takes 390 ms inside `time-world` and 249 ms in the contract's loop; in `time-world` it runs right after each run's stitched mesh and its copies, so its 513 MB of `.wcb` bytes and its output arrays land on fresh pages, while the contract's loop repeats the same allocations — probably the difference, not measured further. While it runs the builder holds every `.wcb` of the world (44.5 MB at ×1, 513 MB at ×16) and its output (about 11 MB and 160 MB). Peak RSS of the whole runs (`/usr/bin/time -v`; the tools print none): `time-world` 0.29 and 3.73 GB, `--time` 0.23 and 3.37 GB.

The Near summary of `time-world` now counts the ring's triangles before the bundle changes the table: 63 337 at ×1 (task 4's line printed 63 333) and 1 013 458 at ×16.

### In the game

`scripts/bench.sh streaming-flythrough` on Ryzen (`BENCH_HOST=ryzen`) with the ×1 copy (`OPENGOTHIC_BASE=$T/x1`, its own `OPENGOTHIC_BASE_CACHE=$T/x1.cache`, `OPENGOTHIC_CONTENT=` and `OPENGOTHIC_TEXTURES=` empty): first the cooked path with a cold derived cache, then `OPENGOTHIC_WCB=0` on the caches the first run wrote. The look was not assessed.

| | cooked path | `OPENGOTHIC_WCB=0` |
|---|---|---|
| bench | exit 0, 202 s | exit 0, 197 s |
| `[world-collision]` line | `chunks …/x1/worlds/NEWWORLD/chunks.wci: 472222 triangles, 260696 vertices, 57.1 ms` | none (no index in use) |
| `physics_static` mark | 821.6 ms: `input=chunks input_ms=57.1 collision=build build_ms=653.8` | 221.1 ms: `input=landscape input_ms=0 collision=cache read_ms=97.1 build_ms=0` |
| derived caches | collision cache and way heights written (`waypoints: hits=0 rays=5249`) | both read: `collision=cache`, `waypoints: hits=5249 rays=0`; `NEWWORLD.collision` keeps the first run's time |
| `landscape_pack` (Near ring at the start) | 35.4 ms | 95.8 ms |
| frames, whole route: FPS, p99 | 105.3, 11.9 ms | 107.2, 11.8 ms |

The second run accepted the collision cache the first run built from the collision mesh: the key is the same and the shapes are the same. The bench does not build a navigation mesh, so it shows no navigation cache; the contract shows its input instead: on `NEWWORLD` two `DynamicWorld`s from the stitched landscape and from the collision mesh give byte-identical `navigationGeometry()` (1 564 611 land and 10 530 water triangles) and the same answer to 8 192 vertical grid rays and 3 035 short rays through named-sector triangles. The builder cost 57 ms of the first run's 821.6 ms `physics_static`, on the BVH thread beside the view's packing; with one run each there is no noise estimate. The `OPENGOTHIC_WCB=0` run was repeated once after a machine restart lost its local report; its first try, before the restart, also exited 0.

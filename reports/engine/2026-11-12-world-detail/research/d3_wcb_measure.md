# Step Д3, task 2: the cooked chunk `.wcb` measured before the format is fixed

Date: 2026-11-13. Plan: [`docs/landscape-plan.md`](../../../../docs/landscape-plan.md), «Д3 по задачам», task 2. Layout proposal: [`d3_design_scout.md`](d3_design_scout.md), section 3.

Question: what does reading the world cost today, stage by stage, what is left when each cell is read from a cooked binary chunk instead, and does the measurement ask for a change of the layout before later tasks build on it.

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
content-convert time-world $B/worlds/NEWWORLD --out $T/x1-cooked/NEWWORLD --runs 5
content-convert time-world $T/w16/worlds/NEWWORLD --runs 5
```

`verify-cooked-world` builds the world mesh and chunk table from the `.wcb` files alone (`assembleCookedWorld`). It compares them field by field with `readWorldChunks` and checks every GLB against its chunk's length and SHA-256. On ×16 `NEWWORLD` this takes 10.1 s with a peak RSS of 3.4 GB.

## The layout as built

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

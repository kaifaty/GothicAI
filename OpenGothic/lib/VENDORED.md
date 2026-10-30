# Engine libraries

The libraries are plain files in this repository (no git submodules). Only what the build uses is
kept: examples, tests, documentation and sample data were left out. They are not updated
automatically; to update one, copy the needed parts of a newer upstream version over its directory.

| Directory | Upstream | Commit | License | Left out |
|---|---|---|---|---|
| `Tempest/` | https://github.com/Try/Tempest | a4ac4029b848ef9f4e5636049dd657663c499a45 | MIT | `Examples/`, `Tests/` |
| `ZenKit/` | https://github.com/GothicKit/ZenKit | 083cc5f5592726aed435f8e863a6e09e92748a4d (v1.3.0-63) | MIT | `tests/`, `docs/`, `examples/`, `assets/` |
| `ZenKit/vendor/libsquish/` | https://github.com/lmichaelis/phoenix-libsquish | 53167b39beee3c7ff441c4ee6ba522c2db27dad4 | MIT | — |
| `ZenKit/vendor/doctest/` | https://github.com/doctest/doctest | 1da23a3e8119ec5cce4f9388e91b065e20bf06f5 (v2.4.12) | MIT | `scripts/data/` (kept because ZenKit's CMake would download it otherwise) |
| `recastnavigation/` | https://github.com/recastnavigation/recastnavigation | 6dc1667f580357e8a2154c28b7867bea7e8ad3a7 (v1.6.0) | zlib | everything but `Recast/{Include,Source}`, `Detour/{Include,Source}` and `License.txt`; local minimal CMake builds these two libraries only |
| `JoltPhysics/` | https://github.com/jrouwe/JoltPhysics | e77f175595e64cb44218cc9d9d56fc365ad0e36a (v5.6.0) | MIT | everything but `Jolt/`, `Build/CMakeLists.txt` and `LICENSE` (examples, test framework, unit/performance tests, viewer, documentation, assets and build helper scripts omitted) |
| `dmusic/` | https://github.com/GothicKit/dmusic | b73b270b94714b8ec8d9d0b2a9f3a5c445da52d1 | MIT | `docs/`, `examples/` |
| `rapidjson/` | https://github.com/Tencent/rapidjson | f54b0e47a08782a6131cc3d60f94d038fa6e0a51 (v1.1.0) | MIT | everything but `include/` (header-only) |
| `cgltf/` | https://github.com/jkuhlmann/cgltf | 360db1a95480fe102ae9c69b27c5d101167ff5ba (v1.15) | MIT | everything but `cgltf.h` and `LICENSE` (single-header GLB parser) |
| `TinySoundFont/` | https://github.com/schellingb/TinySoundFont | 92a8f0e9fe3c98358be7d8564db21fc4b1142d04 | MIT | `examples/`, `sfotool/` |
| `edd-dbg/`, `miniz/` | as in upstream OpenGothic | — | see the directories | — |

Git metadata of the libraries (`.git`, `.github`, `.gitmodules`, `.gitignore`) was not copied.

The Bullet backend has been removed. The scalar plane/edge operation order in
`common/physics/physicvbo.cpp` adapts the former `btTriangleRaycastCallback::processTriangle` to
resolve ambiguous native Jolt hits. This is modified compatibility code, not an upstream Bullet
library or collision backend. The optional scalar witness interpolation in Jolt's
`Geometry/GJKClosestPoint.h` also preserves the former query's scalar barycentric operation,
tetrahedron face-weight order and triangle-to-edge witness interpolation. The native GJK loop and
closest-feature selection remain Jolt code;
there is no additional solver. The original zlib notice for the adapted compatibility arithmetic is
retained in `BulletCompat.LICENSE`; the Jolt library and its existing code retain their MIT license.

## Local changes

Keep these when updating a library from upstream.

- `Tempest/Engine/graphics/device.cpp`, `gapi/metal/mtdevice.mm` and
  `gapi/vulkan/vallocator.cpp`: `tempestProbeGpuBytes()` reports Metal
  `currentAllocatedSize` and live Vulkan allocations (including cached allocator pages).
  OpenGothic calls it only with `OPENGOTHIC_LOADPROBE=1`; ordinary runs do not sample GPU memory.
- `cgltf/` `cgltf.h`: removed a tab from an otherwise empty line to pass the repository's
  diff whitespace check; parser code is unchanged.
- `ZenKit/` `Vfs::set_open_observer` (`Vfs.hh`, `Vfs.cc`): optional successful-open callback
  with the winning archive filename or host file path, set before mounting. File nodes keep
  provenance through overrides; `find` and `resolve` do not count as opens. Used only by
  `OPENGOTHIC_CENSUS`; without an observer no provenance metadata is allocated.
- `JoltPhysics/Jolt/Physics/Character/CharacterVirtual.{h,cpp}`: optional transient
  `WalkStairsDiagnostics` records the rejection stage and sweep results. The stair support test
  tests the full lookahead endpoint first, then at most four intermediate positions, checking the
  first downward hit at every position. Spacing uses contact tolerance and half the shape's inner
  radius, with a hard budget of five lookahead probes per attempt. OpenGothic bounds that lookahead
  by the permitted riser height, slope and capsule toe; actual forward travel is unchanged.
  This lets finite slanted risers and narrow treads work without accepting continuous steep
  slopes. Contracts live in `tests/physics_contract.cpp`.
- `Tempest/Engine/system/api/macosapi.mm`: `TEMPEST_BACKGROUND=1` starts the app without taking the
  focus and without a Dock icon (used by the test bench, `scripts/bench.sh`).
- `Tempest/Engine/system/api/macosapi.mm`: `TEMPEST_WINDOW_SIZE=<w>x<h>` sets the size of a window created
  by show mode (points; shorts are filmed in a 9:16 window, `shorts/`).
- `Tempest/Engine/system/api/x11api.{h,cpp}`: the same `TEMPEST_WINDOW_SIZE` on X11, in pixels, with the size
  fixed by WM hints; with it a minimized window gets that size instead of the whole screen (the test bench
  on Ryzen).
- `Tempest/Engine/sound/sounddevice.{h,cpp}`: `TEMPEST_AUDIO_LOOPBACK=<rate>` opens an OpenAL loopback
  device instead of the speakers; `SoundDevice::renderLoopback` mixes the next frames of every context
  (the test bench records the game's sound in step with the frames).
- `Tempest/Engine/sound/sound.{h,cpp}`: `Sound::samples16` gives the decoded samples of a sound (lip sync
  measures the loudness of a voice line, `common/graphics/mesh/lipsync.cpp`).
- `Tempest/Engine/gapi/metal/mtshader.cpp`: shader compile errors (SPIRV-Cross and MSL) go to `Log::e` in
  every build, not only without `NDEBUG`; the MSL source is still dumped only in debug builds. The
  unconditional `Log::d(msl)` of every mesh shader is removed (with `-ms 1` it put about 24 000 lines
  into `game.log`).
- `rapidjson/include/rapidjson/document.h`: `GenericStringRef::operator=` deleted (GCC 15 diagnoses the
  old one), and `memcpy` into `GenericValue`/`Member` storage goes through `void*`, as upstream does now
  (clang's `-Wnontrivial-memcall`; the engine builds with `-Werror`).
- `ZenKit/` `DaedalusVm::unsafe_call_external` (`DaedalusVm.hh`, `DaedalusVm.cc`): calls an engine
  function the way the `BE` instruction does. The story graph (`ai_bridge/storygraph`) computes the AI
  director's conditions with the engine's own functions through it.
- `JoltPhysics/Jolt/Geometry/RayTriangle.h`, `Physics/Collision/RayCast.h` and
  `Physics/Collision/Shape/MeshShape.cpp`: optional `RayCastSettings::mTriangleEdgeTolerance` adds a
  barycentric edge tolerance only to mesh ray queries, with matching expansion of the ray traversal
  bounds. Its default is zero; OpenGothic queries opt in to `0.0001f` to preserve the previous ray edge
  behavior. Shape casts, contacts, body bounds and other shapes keep the upstream tests. Preserve this
  patch when updating the pinned Jolt version.
- `JoltPhysics/Jolt/Geometry/GJKClosestPoint.h`: `GetClosestPoints<A,B,true>` optionally reconstructs
  the separation axis from scalar interpolation of the native selected feature's witnesses. The
  default third template argument is `false`, retaining upstream behavior. Only NPC contact
  eligibility opts in. Accepted separated NPC–mesh contacts also expose the previous conservative
  scalar witness direction to the controller, including early distance termination; this need not
  be the exact closest-surface normal. Penetrating contact axes, simulation and shape casts remain
  native Jolt. The controller retains the former scalar unit-axis and aggregate normalization order.
  Finite/monotonic checks retain the native axis when reconstruction cannot be used. Finite opt-in
  results return the same witness points used for the axis and distance. Preserve this patch and
  its compatibility license notice when updating Jolt; regression fixtures are in
  `tests/physics_contract.cpp` and evidence is in `reports/physics/2026-10-14-jolt-stage1/`.
- `JoltPhysics/Jolt/Physics/Collision/ShapeCast.h` and `CastSphereVsTriangles.cpp`:
  `ShapeCastSettings::mUsePreciseSphereTriangleCast` defaults to false. The engine sphere query opts
  into double closest-approach tests for triangle edges and vertices, and double cross-product plane
  barycentrics, to avoid false contacts from cancellation in float quadratic and Gram products.
  Components are promoted before the products; sphere radius and bounds are unchanged. Initial
  overlap, final closest-point calculation, contact generation and normals retain native behavior.
  The 765-invocation decoded fixture reproduces native hit, fraction, contact and axis bit for bit
  and removes all 14 proven false winners. Full query dispatch is checked separately because a
  corrected early-out cutoff can expose additional BVH candidates. Evidence is in
  [the accepted physics summary](../../reports/physics/2026-10-14-jolt-stage1/validation-notes.md).
- `ZenKit/` `DaedalusVm::unsafe_call_bl` (`DaedalusVm.hh`, `DaedalusVm.cc`): calls a script function the way
  the `BL` instruction does, so an override (`override_function`) runs instead of its code. The story
  graph's Interpreter calls script functions from a dialog's program through it.
- `ZenKit/` `DaedalusScript::add_int_symbol` (`DaedalusScript.hh`, `DaedalusScript.cc`): adds a global integer
  variable that is not in the compiled scripts; `load` keeps room for `MAX_ADDED_SYMBOLS` of them, so no
  symbol moves. The AI director's story state (`DIR_*`, graph_patch `vars`) lives in such variables and is
  saved with the scripts' own (`GameScript::loadVar` makes them again from a save).
- `ZenKit/` `DaedalusScript::add_instance_symbol` (`DaedalusScript.hh`, `DaedalusScript.cc`): adds an instance
  that is not in the compiled scripts, of the class and with the init code of a script's instance, in the same
  room. The AI director's npc's (`DIR_NPC_*`) are such instances; the engine's `OwnSymbols` makes them again
  in the same order before a save's world loads, so the npc's of the save find their symbols by index. The
  code names its own instance (`self` in an instance's body), so `GameScript::initializeInstanceNpc` binds
  that instance to the new npc while the code runs.

#pragma once

// Static meshes of a stress copy (meshes/*.glb, reports/performance/2026-11-10-content-stress/README.md):
// every triangle split in four at its edge midpoints, M times, the look unchanged. Meshes are read
// and written by the base's own functions (common/basemesh.h): the reader is the game's.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <rapidjson/document.h>

#include "asset/staticmesh.h"

namespace StressBase {

// ---- geometry shared with the world split ----

// Symmetric in its arguments: every triangle sharing an edge gets the same bits.
inline float mid(float a, float b) {
  return float((double(a)+double(b))*0.5);
  }

using D3 = std::array<double,3>;
inline D3 sub(const D3& a, const D3& b) { return {a[0]-b[0],a[1]-b[1],a[2]-b[2]}; }
inline double dot(const D3& a, const D3& b) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
inline D3 cross(const D3& a, const D3& b) {
  return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};
  }
inline double length(const D3& v) { return std::sqrt(dot(v,v)); }
inline double relative(double a, double b) {
  return a==b ? 0.0 : std::abs(a-b)/std::max(std::abs(a),std::abs(b));
  }

namespace Meshes {

// The base mesh format stores a wedge's position index and a triangle's wedge indices as uint16
// (asset::MeshWedge::index, asset::MeshTriangle::wedges; uint16 index accessor in basemesh.cpp):
// at most 65536 positions per mesh and 65536 wedges per submesh. The triangle (index) count of a
// submesh has no limit of its own below the GLB's 4 GiB.
constexpr size_t maxPositions = size_t(1)<<16;
constexpr size_t maxWedges    = size_t(1)<<16;

// One level: triangle t of a submesh becomes 4t..4t+3. A midpoint position is shared by every
// wedge pair over the same position pair (across submeshes too), a midpoint wedge by every
// triangle of the submesh over the same wedge pair; its normal and UV are the average of the ends.
// Material, submesh order, alpha_test and bbox stay. Nothing when the result exceeds the format:
// the reason is in *reason.
std::optional<asset::StaticMesh> subdivide(const asset::StaticMesh& mesh, std::string* reason);

struct Built {
  int      level = 0;
  size_t   meshes = 0, split = 0;
  uint64_t trianglesBefore = 0, trianglesAfter = 0;
  std::vector<std::pair<std::string,std::string>> skipped; // name, reason
  std::set<std::string> written;                            // base-relative paths, generic form
  };

// Writes every mesh of base/meshes/meshes.json that fits the format after `level` splits into
// out/meshes; the rest stays for the caller to link. Level 0 writes nothing.
Built build(const std::filesystem::path& base, const std::filesystem::path& out, int level);

// "meshes" and "meshes_skipped" members of stress.json, without the braces around them.
std::string json(const Built& built);

// What stress.json records; a copy without "meshes" (made before static meshes were split) is level 0.
Built recorded(const rapidjson::Document& stress, const std::filesystem::path& base);

// Every split mesh read by the game's reader against its source. Skipped meshes are checked
// byte for byte by the caller's manifest comparison: they are not in `written`.
void verify(const std::filesystem::path& base, const std::filesystem::path& copy, const Built& recorded);

// Two submeshes with a shared edge inside one of them; a mesh over the position limit. Throws on failure.
void selfTest(const std::filesystem::path& dir);

}
}

#include "islandcontent.h"

#include "islandterrain.h"

#include "worldchunks.h"

#include <chrono>

namespace island {

Written writeIsland(const std::filesystem::path& dir) {
  using Clock = std::chrono::steady_clock;
  for(const char* part:{"world", "content", "textures"})
    std::filesystem::remove_all(dir/part);
  std::filesystem::create_directories(dir);

  Written written;
  auto since = [](Clock::time_point from) {
    return std::chrono::duration<double>(Clock::now()-from).count();
    };

  auto start = Clock::now();
  const auto manifest = writeWorldChunks(terrain().mesh(), WorldChunkSource{WorldName, 0, 0}, dir/"world", 6400);
  written.triangles  = manifest.triangles;
  written.chunks     = manifest.chunks.size();
  written.seconds[0] = since(start);

  start = Clock::now();
  written.materials  = writeMaterials(dir/"content/materials");
  written.seconds[1] = since(start);

  start = Clock::now();
  written.props      = writeProps(dir/"content/meshes");
  written.seconds[2] = since(start);

  start = Clock::now();
  writeSky(dir/"textures");
  written.seconds[3] = since(start);
  return written;
  }

}

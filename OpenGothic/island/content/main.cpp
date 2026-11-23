// island-content <dir>: writes the empty island into dir (islandcontent.h) and prints what it made.

#include "islandcontent.h"
#include "islandterrain.h"

#include <cmath>
#include <cstdio>
#include <exception>

int main(int argc, char** argv) {
  if(argc!=2) {
    std::fprintf(stderr, "usage: island-content <dir>\n");
    return 2;
    }
  try {
    const auto written = island::writeIsland(argv[1]);
    const auto& land   = island::terrain();

    float summit = 0, sx = 0, sz = 0;
    for(float z=-island::FieldHalf; z<=island::FieldHalf; z+=1.f)
      for(float x=-island::FieldHalf; x<=island::FieldHalf; x+=1.f)
        if(const float h=land.height(x, z); h>summit) {
          summit = h;
          sx = x;
          sz = z;
          }
    const auto& stream = land.stream();
    size_t mouth = 0;
    float  steepest = 0;
    for(size_t i=0; i+1<stream.size(); ++i) {
      if(stream[i].bed>0)
        mouth = i+1;
      steepest = std::max(steepest, stream[i].bed-stream[i+1].bed);
      }
    std::printf("island: ground %llu triangles in %llu chunks, summit %.1f m at (%.0f, %.0f)\n",
                static_cast<unsigned long long>(written.triangles), static_cast<unsigned long long>(written.chunks),
                double(summit), double(sx), double(sz));
    std::printf("island: stream %zu m, %zu m of it on land, spring at %.1f m, steepest fall %.1f %%\n",
                stream.size()-1, mouth, double(stream.front().bed), double(steepest)*100.0);
    for(const auto& m:written.materials)
      std::printf("island: %-17s albedo %.3f, quarter tiles within %.1f %%\n", m.name.c_str(), double(m.luminance),
                  double(m.spread)*100.0);
    for(const auto& p:written.props)
      std::printf("island: %-13s %5zu triangles, %.2f x %.2f x %.2f m, %.2f m under the ground\n", p.name.c_str(),
                  p.triangles, double(p.max[0]-p.min[0]), double(p.max[1]-p.min[1]), double(p.max[2]-p.min[2]),
                  double(-p.min[1]));
    std::printf("island: world %.2f s, materials %.2f s, props %.2f s, sky %.2f s\n", written.seconds[0],
                written.seconds[1], written.seconds[2], written.seconds[3]);
    return 0;
    }
  catch(const std::exception& e) {
    std::fprintf(stderr, "island-content: %s\n", e.what());
    return 1;
    }
  }

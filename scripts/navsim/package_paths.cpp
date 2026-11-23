// Load an exported OGNAV001 cache outside the game and call the engine's exact
// findPath implementation. Input/output coordinates and lengths are metres.
#include "physics/navmesh.h"

#include <iomanip>
#include <iostream>
#include <stdexcept>

int main(int argc, char** argv) {
  try {
    if(argc!=4) throw std::runtime_error("Usage: package-paths NAVMESH NEAREST_HORIZONTAL_M NEAREST_VERTICAL_M");
    NavMesh::Params params;
    params.nearestHorizontal=std::stof(argv[2]);
    params.nearestVertical=std::stof(argv[3]);
    NavMesh mesh;
    mesh.loadCache(argv[1],params);
    std::cout << std::setprecision(9);
    NavMesh::Point start{},end{};
    int transitions=0;
    while(std::cin >> start[0] >> start[1] >> start[2] >> end[0] >> end[1] >> end[2] >> transitions) {
      const auto path=mesh.findPath(start,end,transitions!=0);
      std::cout << int(path.complete) << ' ' << path.length << ' ' << path.reason << '\n';
      }
    if(!std::cin.eof()) throw std::runtime_error("Invalid path query input");
    }
  catch(const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
    }
  }

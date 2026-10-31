// Export only our GLB through the same CPU converter as the engine. This target
// neither mounts game archives nor uses Resources or a graphics device.
#include "../../OpenGothic/common/gltfmesh.h"

#include <fstream>
#include <iomanip>
#include <iostream>
#include <locale>
#include <stdexcept>

int main(int argc, char** argv) {
  try {
    if(argc!=3)
      throw std::runtime_error("usage: fracture-gltf-extract <source.glb> <source.mesh>");
    const auto mesh = loadGltfMesh(argv[1]);
    std::ofstream source(argv[2]);
    if(!source)
      throw std::runtime_error("cannot write source mesh");
    source.imbue(std::locale::classic());
    source << std::setprecision(9);
    size_t count = 0;
    for(const auto& sm:mesh.sub_meshes) {
      if(sm.mat.texture.find_first_of(" \t\r\n\v\f")!=std::string::npos || sm.mat.texture.size()>255)
        throw std::runtime_error("material name cannot be represented in the fracture format");
      for(const auto& tri:sm.triangles) {
        source << "tri " << sm.mat.texture << '\n';
        for(auto index:tri.wedges) {
          const auto& wedge = sm.wedges.at(index);
          const auto& position = mesh.positions.at(wedge.index);
          source << position.x << ' ' << position.y << ' ' << position.z << ' '
                 << wedge.normal.x << ' ' << wedge.normal.y << ' ' << wedge.normal.z << ' '
                 << wedge.texture.x << ' ' << wedge.texture.y << '\n';
          }
        ++count;
        }
      }
    if(!source)
      throw std::runtime_error("cannot write source mesh");
    std::cout << count << " own GLB triangles\n";
    return 0;
    } catch(const std::exception& e) {
    std::cerr << "fracture-gltf-extract: " << e.what() << '\n';
    return 1;
    }
  }

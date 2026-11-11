// Export only our GLB through the same CPU converter as the engine. This target
// neither mounts game archives nor uses Resources or a graphics device.
//
// fracture-gltf-extract <source.glb> <source.mesh>           whole visual: "tri <texture>" rows
// fracture-gltf-extract --nodes <debris.glb> <debris.mesh>   one "node <name>" block per glTF node,
//                                                           "tri <texture> <group>" rows (version 3)
#include "../../OpenGothic/common/gltfmesh.h"

#include <fstream>
#include <iomanip>
#include <iostream>
#include <locale>
#include <stdexcept>
#include <string>

namespace {

const char* groupName(asset::MaterialGroup group) {
  switch(group) {
    case asset::MaterialGroup::METAL: return "METAL";
    case asset::MaterialGroup::STONE: return "STONE";
    case asset::MaterialGroup::WOOD:  return "WOOD";
    case asset::MaterialGroup::EARTH: return "EARTH";
    case asset::MaterialGroup::WATER: return "WATER";
    case asset::MaterialGroup::SNOW:  return "SNOW";
    default:                          return "UNDEF";
    }
  }

void checkToken(const std::string& text, const char* what) {
  if(text.empty() || text.find_first_of(" \t\r\n\v\f")!=std::string::npos || text.size()>255)
    throw std::runtime_error(std::string(what)+" cannot be represented in the fracture format");
  }

size_t writeMesh(std::ostream& source, const asset::StaticMesh& mesh, bool group) {
  size_t count = 0;
  for(const auto& sm:mesh.sub_meshes) {
    checkToken(sm.mat.texture,"material name");
    for(const auto& tri:sm.triangles) {
      source << "tri " << sm.mat.texture;
      if(group)
        source << ' ' << groupName(sm.mat.group);
      source << '\n';
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
  return count;
  }

}

int main(int argc, char** argv) {
  try {
    const bool nodes = argc==4 && std::string(argv[1])=="--nodes";
    if(argc!=3 && !nodes)
      throw std::runtime_error("usage: fracture-gltf-extract [--nodes] <source.glb> <source.mesh>");
    const char* input = argv[nodes ? 2 : 1];
    std::ofstream source(argv[nodes ? 3 : 2]);
    if(!source)
      throw std::runtime_error("cannot write source mesh");
    source.imbue(std::locale::classic());
    source << std::setprecision(9);
    size_t count = 0, parts = 0;
    if(nodes) {
      for(const auto& node:loadGltfMeshNodes(input)) {
        checkToken(node.name,"node name");
        source << "node " << node.name << '\n';
        count += writeMesh(source,node.mesh,true);
        ++parts;
        }
      }
    else {
      count = writeMesh(source,loadGltfMesh(input),false);
      }
    if(!source)
      throw std::runtime_error("cannot write source mesh");
    if(nodes)
      std::cout << parts << " nodes, ";
    std::cout << count << " own GLB triangles\n";
    return 0;
    } catch(const std::exception& e) {
    std::cerr << "fracture-gltf-extract: " << e.what() << '\n';
    return 1;
    }
  }

// Reuse the engine's CPU GLB readers, including its handedness and units.
#include "../../OpenGothic/common/basemesh.h"
#include "../../OpenGothic/common/gltfmesh.h"
#include <fstream>
#include <iomanip>
#include <iostream>
#include <locale>
#include <stdexcept>

int main(int argc,char** argv) {
  try {
    if(argc!=4) throw std::runtime_error("usage: convex-parts-extract <base|content> <source.glb> <source.mesh>");
    if(std::string_view(argv[1])!="base" && std::string_view(argv[1])!="content")
      throw std::runtime_error("expected base or content mode");
    const auto mesh=std::string_view(argv[1])=="base" ? readBaseMesh(argv[2]) : loadGltfMesh(argv[2]);
    size_t count=0;
    for(const auto& sm:mesh.sub_meshes) count+=sm.triangles.size();
    std::ofstream out(argv[3]);
    out.imbue(std::locale::classic());
    out<<std::setprecision(9)<<mesh.positions.size()<<' '<<count<<'\n';
    for(const auto& p:mesh.positions) out<<p.x<<' '<<p.y<<' '<<p.z<<'\n';
    for(const auto& sm:mesh.sub_meshes)
      for(const auto& tri:sm.triangles)
        out<<sm.wedges.at(tri.wedges[0]).index<<' '<<sm.wedges.at(tri.wedges[1]).index<<' '
           <<sm.wedges.at(tri.wedges[2]).index<<'\n';
    if(!out) throw std::runtime_error("cannot write source mesh");
    }
  catch(const std::exception& e) {
    std::cerr<<"convex-parts-extract: "<<e.what()<<'\n';
    return 1;
    }
  }

// V-HACD is deliberately confined to this offline executable.
#define ENABLE_VHACD_IMPLEMENTATION 1
#include "VHACD.h"
#include <fstream>
#include <iomanip>
#include <iostream>
#include <locale>
#include <stdexcept>

int main(int argc,char** argv) {
  try {
    if(argc!=3) throw std::runtime_error("usage: convex-parts-vhacd <mesh> <parts>");
    std::ifstream input(argv[1]);
    input.imbue(std::locale::classic());
    size_t vertices=0,triangles=0;
    if(!(input>>vertices>>triangles)) throw std::runtime_error("invalid source header");
    std::vector<double> points(vertices*3);
    std::vector<uint32_t> indices(triangles*3);
    for(auto& v:points) if(!(input>>v)) throw std::runtime_error("invalid source vertex");
    for(auto& i:indices) if(!(input>>i) || i>=vertices) throw std::runtime_error("invalid source triangle");
    VHACD::IVHACD::Parameters parameters;
    parameters.m_maxConvexHulls=16;
    parameters.m_resolution=2000000;
    parameters.m_minimumVolumePercentErrorAllowed=.1;
    parameters.m_maxRecursionDepth=12;
    parameters.m_maxNumVerticesPerCH=64;
    parameters.m_minEdgeLength=1;
    parameters.m_asyncACD=false;
    auto* vhacd=VHACD::CreateVHACD();
    if(!vhacd->Compute(points.data(),uint32_t(vertices),indices.data(),uint32_t(triangles),parameters))
      throw std::runtime_error("V-HACD decomposition failed");
    std::ofstream output(argv[2]);
    output.imbue(std::locale::classic());
    output<<std::setprecision(12)<<vhacd->GetNConvexHulls()<<'\n';
    for(uint32_t i=0;i<vhacd->GetNConvexHulls();++i) {
      VHACD::IVHACD::ConvexHull hull;
      if(!vhacd->GetConvexHull(i,hull)) throw std::runtime_error("V-HACD hull missing");
      output<<hull.m_points.size()<<'\n';
      for(const auto& v:hull.m_points) output<<v.mX<<' '<<v.mY<<' '<<v.mZ<<'\n';
      }
    vhacd->Release();
    if(!output) throw std::runtime_error("cannot write decomposition");
    }
  catch(const std::exception& e) {
    std::cerr<<"convex-parts-vhacd: "<<e.what()<<'\n';
    return 1;
    }
  }

// V-HACD is deliberately confined to this offline executable.
#define ENABLE_VHACD_IMPLEMENTATION 1
#include "VHACD.h"
#include "voxelreference.h"
#include <fstream>
#include <iomanip>
#include <iostream>
#include <locale>
#include <stdexcept>
#include <string_view>

int main(int argc,char** argv) {
  try {
    const bool referenceOnly=argc==4 && std::string_view(argv[1])=="reference";
    if(argc!=3 && !referenceOnly)
      throw std::runtime_error("usage: convex-parts-vhacd [reference] <mesh> <output>");
    const auto meshArg=referenceOnly ? 2 : 1;
    std::ifstream input(argv[meshArg]);
    input.imbue(std::locale::classic());
    size_t vertices=0,triangles=0;
    if(!(input>>vertices>>triangles)) throw std::runtime_error("invalid source header");
    std::vector<double> points(vertices*3);
    std::vector<uint32_t> indices(triangles*3);
    for(auto& v:points) if(!(input>>v)) throw std::runtime_error("invalid source vertex");
    for(auto& i:indices) if(!(input>>i) || i>=vertices) throw std::runtime_error("invalid source triangle");
    auto reference=voxelReference(points,indices);
    double longest=0;
    for(size_t axis=0;axis<3;++axis) {
      double lo=points[axis],hi=lo;
      for(size_t i=axis;i<points.size();i+=3) { lo=std::min(lo,points[i]); hi=std::max(hi,points[i]); }
      longest=std::max(longest,hi-lo);
      }
    // V-HACD derives its longest grid dimension as floor(resolution^.33*1.5).
    // Reserve two cells for its shorter-axis padding and keep physical pitch
    // at least 1 cm. Tiny meshes use the bounded explicit flood grid instead.
    const auto dimension=uint32_t(std::clamp(std::floor(longest/reference.pitchCm),32.,254.));
    const auto resolution=uint32_t(std::ceil(std::pow((double(dimension)+.25)/1.5,1./.33)));
    VHACD::IVHACD::Parameters parameters;
    parameters.m_maxConvexHulls=16;
    parameters.m_resolution=resolution;
    parameters.m_minimumVolumePercentErrorAllowed=.1;
    parameters.m_maxRecursionDepth=12;
    parameters.m_maxNumVerticesPerCH=64;
    parameters.m_minEdgeLength=1;
    parameters.m_asyncACD=false;
    parameters.m_fillMode=VHACD::FillMode::FLOOD_FILL;
    // The pinned header exposes the input voxel field. Read its actual cells,
    // including V-HACD's input welding, so reference and decomposition agree.
    auto vhacd=std::make_unique<VHACD::VHACDImpl>();
    if(referenceOnly && longest>=32.) {
      std::vector<VHACD::Vertex> sourcePoints;
      std::vector<VHACD::Triangle> sourceTriangles;
      for(size_t i=0;i<points.size();i+=3) sourcePoints.emplace_back(points[i],points[i+1],points[i+2]);
      for(size_t i=0;i<indices.size();i+=3) sourceTriangles.emplace_back(indices[i],indices[i+1],indices[i+2]);
      vhacd->m_params=parameters;
      vhacd->CopyInputMesh(sourcePoints,sourceTriangles);
      }
    else if(!referenceOnly && !vhacd->Compute(points.data(),uint32_t(vertices),indices.data(),uint32_t(triangles),parameters))
      throw std::runtime_error("V-HACD decomposition failed");
    if(longest>=32.) {
      const auto& voxels=vhacd->m_voxelize;
      reference.pitchCm=voxels.GetScale()*vhacd->m_scale;
      const auto& grid=voxels.GetDimensions();
      for(size_t axis=0;axis<3;++axis) reference.grid[axis]=grid[axis];
      reference.surfaceCells=voxels.GetSurfaceVoxels().size();
      reference.interiorCells=voxels.GetInteriorVoxels().size();
      reference.volumeCm3=double(reference.surfaceCells+reference.interiorCells)*
                            std::pow(reference.pitchCm,3);
      }
    if(reference.pitchCm<1. || *std::max_element(reference.grid.begin(),reference.grid.end())>256)
      throw std::runtime_error("voxel reference exceeds the 1 cm / 256 cell limits");
    std::ofstream output(argv[meshArg+1]);
    output.imbue(std::locale::classic());
    output<<std::setprecision(12)<<"{\"pitch_cm\":"<<reference.pitchCm<<",\"grid\":["<<reference.grid[0]<<','
          <<reference.grid[1]<<','<<reference.grid[2]<<"],\"surface_cells\":"<<reference.surfaceCells
          <<",\"interior_cells\":"<<reference.interiorCells<<",\"volume_cm3\":"<<reference.volumeCm3
          <<",\"resolution\":"<<resolution<<",\"model\":\"surface-plus-enclosed-flood6\",\"input\":\""
          <<(longest>=32. ? "V-HACD-welded" : "source")<<"\"}\n";
    if(referenceOnly) return output ? 0 : 1;
    output<<vhacd->GetNConvexHulls()<<'\n';
    for(uint32_t i=0;i<vhacd->GetNConvexHulls();++i) {
      VHACD::IVHACD::ConvexHull hull;
      if(!vhacd->GetConvexHull(i,hull)) throw std::runtime_error("V-HACD hull missing");
      output<<hull.m_points.size()<<'\n';
      for(const auto& v:hull.m_points) output<<v.mX<<' '<<v.mY<<' '<<v.mZ<<'\n';
      }
    if(!output) throw std::runtime_error("cannot write decomposition");
    }
  catch(const std::exception& e) {
    std::cerr<<"convex-parts-vhacd: "<<e.what()<<'\n';
    return 1;
    }
  }

#include "physics/clothgeometry.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {
  using namespace ClothGeometry;

  void require(bool condition, const char* message) {
    if(condition)
      return;
    std::fprintf(stderr,"FAIL: %s\n",message);
    std::exit(EXIT_FAILURE);
    }

  Vertex vertex(float x, float y, float u, float v, float normal=1.f, uint32_t color=0xffffffffu) {
    Vertex out;
    out.pos[0] = x;
    out.pos[1] = y;
    out.norm[2] = normal;
    out.uv[0] = u;
    out.uv[1] = v;
    out.color = color;
    return out;
    }

  float winding(const Mesh& mesh, size_t triangle) {
    const auto& a = mesh.vertices[mesh.indices[triangle]];
    const auto& b = mesh.vertices[mesh.indices[triangle+1]];
    const auto& c = mesh.vertices[mesh.indices[triangle+2]];
    return (b.pos[0]-a.pos[0])*(c.pos[1]-a.pos[1])-
           (b.pos[1]-a.pos[1])*(c.pos[0]-a.pos[0]);
    }

  void sourceInterpolation(const Mesh& mesh, const std::vector<Vertex>& original) {
    require(mesh.vertices.size()==mesh.sourceWeights.size(),"every output vertex retains a source binding");
    for(size_t at=0;at<mesh.vertices.size();++at) {
      const auto& v = mesh.vertices[at];
      const auto& binding = mesh.sourceWeights[at];
      float sum = 0;
      float position[3] = {}, uv[2] = {}, color[4] = {};
      for(size_t k=0;k<3;++k) {
        const auto weight = binding.weights[k];
        require(weight>=0.f,"source weights are nonnegative");
        require(binding.vertices[k]<original.size(),"binding addresses an original vertex");
        sum += weight;
        const auto& source = original[binding.vertices[k]];
        for(size_t axis=0;axis<3;++axis)
          position[axis] += source.pos[axis]*weight;
        for(size_t axis=0;axis<2;++axis)
          uv[axis] += source.uv[axis]*weight;
        for(size_t channel=0;channel<4;++channel)
          color[channel] += float((source.color>>(channel*8))&0xffu)*weight;
        }
      require(std::abs(sum-1.f)<1.e-6f,"source weights preserve affine positions and morph offsets");
      for(size_t axis=0;axis<3;++axis)
        require(std::abs(v.pos[axis]-position[axis])<1.e-4f,"visible positions match their original barycentric binding");
      for(size_t axis=0;axis<2;++axis)
        require(std::abs(v.uv[axis]-uv[axis])<1.e-6f,"UV interpolation follows the source triangle");
      for(size_t channel=0;channel<4;++channel)
        require(std::abs(float((v.color>>(channel*8))&0xffu)-color[channel])<=.501f,"each color channel interpolates independently");
      }
    }

  template<class F> void invalidInput(F function) {
    bool rejected = false;
    try {
      function();
      }
    catch(const std::invalid_argument&) {
      rejected = true;
      }
    require(rejected,"malformed geometry is rejected before subdivision");
    }
  }

int main() {
  const std::vector<Vertex> flag = {
    vertex(0,400,0,0,1,0x00000000u), vertex(200,400,1,0),
    vertex(200,0,1,1,1,0x12345678u), vertex(0,0,0,1,1,0xfedcba98u)
    };
  const std::vector<uint32_t> triangles = {0,1,2,0,2,3}, pins = {0,1};
  const auto mesh = subdivide(flag,triangles,pins);
  require(mesh.uniquePositions>=100 && mesh.uniquePositions<=600,"a low-poly city banner receives 100-600 particles");
  require(mesh.maxEdgeCm<=35.f,"the low-poly banner bends on edges at most 35 cm long");
  require(mesh.subdivisionDepth==4 && mesh.uniquePositions==289,"the 2 by 4 metre banner selects a reproducible density");
  require(mesh.budgetLimited,"further uniform subdivision respects the particle budget");
  require(std::memcmp(flag.data(),mesh.vertices.data(),flag.size()*sizeof(Vertex))==0,"original corners, UV and normals remain byte exact");
  sourceInterpolation(mesh,flag);
  for(size_t at=0;at<mesh.indices.size();at+=3)
    require(winding(mesh,at)<0.f,"all subdivided front triangles preserve winding");
  std::set<uint32_t> fixed(mesh.pins.begin(),mesh.pins.end());
  require(mesh.pins.size()==17,"the full top attachment edge is subdivided into fixed vertices");
  for(uint32_t at=0;at<mesh.vertices.size();++at)
    require((mesh.vertices[at].pos[1]==400.f)==(fixed.count(at)!=0),"only the original fixed edge remains pinned");

  std::map<std::pair<uint32_t,uint32_t>,size_t> edges;
  for(size_t at=0;at<mesh.indices.size();at+=3)
    for(size_t side=0;side<3;++side) {
      const auto a = mesh.indices[at+side], b = mesh.indices[at+(side+1)%3];
      ++edges[{std::min(a,b),std::max(a,b)}];
      }
  size_t boundaryEdges = 0;
  for(const auto& [edge,count]:edges) {
    (void)edge;
    require(count==1 || count==2,"a conforming sheet has at most two triangles at each edge");
    if(count==1)
      ++boundaryEdges;
    }
  require(boundaryEdges==4*(1u<<mesh.subdivisionDepth),"the shared original diagonal introduces no cracks or T junctions");
  const auto repeat = subdivide(flag,triangles,pins);
  require(repeat.indices==mesh.indices && repeat.pins==mesh.pins &&
          repeat.vertices.size()==mesh.vertices.size() &&
          std::memcmp(repeat.vertices.data(),mesh.vertices.data(),mesh.vertices.size()*sizeof(Vertex))==0 &&
          std::memcmp(repeat.sourceWeights.data(),mesh.sourceWeights.data(),mesh.sourceWeights.size()*sizeof(SourceWeights))==0,
          "subdivision and bindings are deterministic");

  auto doubleSided = flag;
  for(auto v:flag) {
    v.norm[2] = -1.f;
    v.uv[0] = 1.f-v.uv[0];
    doubleSided.push_back(v);
    }
  const std::vector<uint32_t> bothSides = {0,1,2,0,2,3,4,6,5,4,7,6};
  const auto reverse = subdivide(doubleSided,bothSides,std::vector<uint32_t>{0,1,4,5});
  require(reverse.uniquePositions==mesh.uniquePositions,"reverse faces and UV seams do not double the particle budget");
  require(reverse.vertices.size()==mesh.vertices.size()*2,"reverse faces retain separate visible normals and UV seams");
  sourceInterpolation(reverse,doubleSided);
  size_t frontTriangles = 0, backTriangles = 0;
  for(size_t at=0;at<reverse.indices.size();at+=3) {
    if(winding(reverse,at)<0.f)
      ++frontTriangles;
    else if(winding(reverse,at)>0.f)
      ++backTriangles;
    }
  require(frontTriangles==backTriangles && frontTriangles==mesh.indices.size()/3,"reverse faces keep the original opposite winding");

  const auto corner = subdivide(flag,triangles,std::vector<uint32_t>{0});
  require(corner.pins==std::vector<uint32_t>{0},"one fixed corner never spreads pinning into its neighbouring triangles");
  Options smallTriangle;
  smallTriangle.minParticles = 0;
  smallTriangle.targetEdgeCm = 1.f;
  smallTriangle.maxDepth = 2;
  const auto triangle = subdivide(std::vector<Vertex>{vertex(0,0,0,0),vertex(100,0,1,0),vertex(0,100,0,1)},
                                  std::vector<uint32_t>{0,1,2},std::vector<uint32_t>{0,1,2},smallTriangle);
  require(triangle.uniquePositions==15 && triangle.pins.size()==12,"three fixed corners keep the triangle boundary fixed while its new interior remains free");

  // Ship flags already contain a curved 5 by 5 sheet. Keep their simulation
  // coarser than the visible mesh, with bindings relative to the physical grid.
  std::vector<Vertex> shipFlag;
  std::vector<uint32_t> shipTriangles,shipPins;
  for(uint32_t y=0;y<5;++y)
    for(uint32_t x=0;x<5;++x) {
      const float u=float(x)*.25f,v=float(y)*.25f;
      auto point=vertex(510.f*u,360.f*(1.f-v),u,v);
      point.pos[2]=30.f*std::sin(u*3.14159265f)*std::sin(v*3.14159265f);
      shipFlag.push_back(point);
      if(y==0) shipPins.push_back(y*5+x);
      }
  for(uint32_t y=0;y<4;++y)
    for(uint32_t x=0;x<4;++x) {
      const uint32_t a=y*5+x,b=a+1,c=a+5,d=c+1;
      shipTriangles.insert(shipTriangles.end(),{a,b,c,b,d,c});
      }
  Options coarseOptions;
  coarseOptions.targetEdgeCm=100.f;
  coarseOptions.minParticles=0;
  coarseOptions.maxParticles=100;
  coarseOptions.maxDepth=1;
  const auto shipCoarse=subdivide(shipFlag,shipTriangles,shipPins,coarseOptions);
  const auto shipVisible=subdivide(shipCoarse.vertices,shipCoarse.indices,shipCoarse.pins);
  const auto shipDirect=subdivide(shipFlag,shipTriangles,shipPins);
  require(shipFlag.size()==25 && shipCoarse.uniquePositions==81 && shipCoarse.vertices.size()==81,
          "a curved authored ship flag uses eighty-one physical particles below its hundred-particle budget");
  require(shipCoarse.subdivisionDepth==1 && shipCoarse.maxEdgeCm<=100.f,
          "ship physics refines the authored grid exactly once");
  require(shipVisible.uniquePositions==289 && shipVisible.vertices.size()==289 && shipDirect.vertices.size()==289,
          "coarse ship physics retains the same dense visible tessellation as the ordinary path");
  sourceInterpolation(shipCoarse,shipFlag);
  sourceInterpolation(shipVisible,shipCoarse.vertices);
  std::map<std::pair<float,float>,const Vertex*> directUv;
  for(const auto& point:shipDirect.vertices) directUv.emplace(std::make_pair(point.uv[0],point.uv[1]),&point);
  const std::set<uint32_t> coarseFixed(shipCoarse.pins.begin(),shipCoarse.pins.end());
  const std::set<uint32_t> visibleFixed(shipVisible.pins.begin(),shipVisible.pins.end());
  require(shipPins.size()==5 && coarseFixed.size()==9 && visibleFixed.size()==17,
          "both subdivision stages preserve and refine only the authored attachment edge");
  const auto affine=[](const Vertex& point) {
    return std::array<float,3>{1.1f*point.pos[0]+.2f*point.pos[1]-.3f*point.pos[2]+37.f,
                               -.1f*point.pos[0]+.9f*point.pos[1]+.4f*point.pos[2]-19.f,
                               .3f*point.pos[0]-.2f*point.pos[1]+1.2f*point.pos[2]+11.f};
    };
  bool bindsNewPhysicalVertex=false;
  for(uint32_t at=0;at<shipVisible.vertices.size();++at) {
    const auto& point=shipVisible.vertices[at];
    const auto original=directUv.find({point.uv[0],point.uv[1]});
    require(original!=directUv.end(),"the two-stage visible grid preserves every ordinary-path UV sample");
    for(size_t axis=0;axis<3;++axis)
      require(std::abs(point.pos[axis]-original->second->pos[axis])<1.e-4f,
              "two-stage subdivision leaves the curved flag's rest surface unchanged");
    require((point.pos[1]==360.f)==(visibleFixed.count(at)!=0),"dense ship pins stay on the original fixed edge");
    const auto& binding=shipVisible.sourceWeights[at];
    std::array<float,3> posed={},fixedPose={};
    for(size_t k=0;k<3;++k) {
      const auto source=binding.vertices[k];
      const float weight=binding.weights[k];
      const auto transformed=affine(shipCoarse.vertices[source]);
      for(size_t axis=0;axis<3;++axis) {
        posed[axis]+=transformed[axis]*weight;
        fixedPose[axis]+=shipCoarse.vertices[source].pos[axis]*weight;
        }
      if(weight>0.f) {
        bindsNewPhysicalVertex|=source>=shipFlag.size();
        if(visibleFixed.count(at)!=0)
          require(coarseFixed.count(source)!=0,"visible attachment vertices bind only to fixed physical particles");
        // Model an arbitrary moving cloth pose while leaving physical pins fixed.
        if(coarseFixed.count(source)==0) fixedPose[2]+=(20.f+float(source))*weight;
        }
      }
    const auto expected=affine(point);
    for(size_t axis=0;axis<3;++axis) {
      require(std::abs(posed[axis]-expected[axis])<.001f,"visible barycentric bindings exactly reproduce an affine physical pose");
      if(visibleFixed.count(at)!=0)
        require(std::abs(fixedPose[axis]-point.pos[axis])<1.e-4f,"deforming free physical particles never moves visible attachments");
      }
    }
  require(bindsNewPhysicalVertex,"dense bindings address the refined physical grid rather than the twenty-five authored vertices");
  for(const auto* sheet:{&shipCoarse,&shipVisible})
    for(size_t at=0;at<sheet->indices.size();at+=3)
      require(winding(*sheet,at)<0.f,"both ship flag grids retain nondegenerate triangles and authored winding");

  invalidInput([&] { subdivide(flag,std::vector<uint32_t>{0,1,9},pins); });
  invalidInput([&] { subdivide(flag,triangles,std::vector<uint32_t>{8}); });
  invalidInput([&] { subdivide(flag,std::vector<uint32_t>{0,1},pins); });
  auto invalid = flag;
  invalid.front().pos[0] = std::numeric_limits<float>::infinity();
  invalidInput([&] { subdivide(invalid,triangles,pins); });
  require(subdivide({}, {}, {}).vertices.empty(),"empty geometry remains empty");

  std::printf("cloth geometry contract passed: banner_particles=%zu banner_visible_vertices=%zu reverse_visible_vertices=%zu depth=%u max_edge_cm=%.6g pins=%zu boundary_edges=%zu ship_authored_vertices=%zu ship_physics_particles=%zu ship_visible_vertices=%zu\n",
              mesh.uniquePositions,mesh.vertices.size(),reverse.vertices.size(),mesh.subdivisionDepth,
              double(mesh.maxEdgeCm),mesh.pins.size(),boundaryEdges,shipFlag.size(),shipCoarse.uniquePositions,shipVisible.vertices.size());
  return EXIT_SUCCESS;
  }

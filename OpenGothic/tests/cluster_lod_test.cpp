#include "graphics/mesh/clusterlod.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <iostream>
#include <map>
#include <numbers>
#include <random>
#include <set>
#include <stdexcept>
#include <string>

namespace {

using namespace ClusterLod;

constexpr float Infinity = std::numeric_limits<float>::infinity();
constexpr uint32_t GridSize = 40;
constexpr float CellCm = 160.f;
constexpr std::array<uint32_t, 4> MaterialCuts = {0,13,27,GridSize};
constexpr std::array<uint32_t, 5> ChartCuts = {0,10,20,30,GridSize};
constexpr float MaterialBendZ = 2400.f;

enum class Model { Flat, Hill, Seams, Overhang, FinishedBranch };

struct Fixture {
  Model                 model;
  std::vector<Vertex>   vertices;
  std::vector<Triangle> triangles;
  std::vector<uint8_t>  boundary;
  Point                 contact{};
  };

void require(bool condition, const char* message) {
  if(!condition)
    throw std::runtime_error(message);
  }

Vertex groundVertex(Model model, float x, float z, size_t chart) {
  float y = 0.f;
  if(model==Model::Hill) {
    const float dx = (x-2100.f)/2050.f, dz = (z-3800.f)/1800.f;
    const float hill = std::max(0.f,1.f-dx*dx-dz*dz);
    y = 1000.f*hill*hill + 28.f*hill*std::sin(x*0.025f)*std::sin(z*0.026f);
    }
  return {{x,y,z}, chart>=3 ? Point{0.f,0.8f,0.6f} : Point{0.f,1.f,0.f},
          {x/6400.f,z/6400.f+(chart>=2 ? 7.f : 0.f)}, chart>=1 ? 0xffe0c080u : 0xffffffffu};
  }

void addQuad(Fixture& mesh, uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t material) {
  mesh.triangles.push_back({{a,b,c},material});
  mesh.triangles.push_back({{a,c,d},material});
  }

float materialCut(size_t material, float z) {
  const float bend = std::abs(z-MaterialBendZ)*0.125f;
  const float raster = std::abs(std::remainder(z,2.f*CellCm))*0.25f;
  const float x = float(MaterialCuts[material])*CellCm;
  if(material==1)
    return x+bend+raster;
  if(material==2)
    return x-bend-raster;
  return x;
  }

void addGround(Fixture& mesh, uint32_t x0, uint32_t x1, uint32_t z0, uint32_t z1, uint32_t material, size_t chart) {
  const auto start = uint32_t(mesh.vertices.size()), stride = x1-x0+1;
  for(uint32_t z=z0; z<=z1; ++z)
    for(uint32_t x=x0; x<=x1; ++x) {
      const float pz = float(z)*CellCm;
      float px = float(x)*CellCm;
      if(mesh.model==Model::Seams) {
        const float left = materialCut(material,pz), right = materialCut(material+1,pz);
        px = x==x1 ? right : left+(right-left)*float(x-x0)/float(x1-x0);
        }
      mesh.vertices.push_back(groundVertex(mesh.model,px,pz,chart));
      // The caller can freeze an interior vertex even when it is not on an open edge.
      mesh.boundary.push_back(uint8_t(mesh.model==Model::Flat && x==GridSize/2));
      }
  for(uint32_t z=z0; z<z1; ++z)
    for(uint32_t x=x0; x<x1; ++x) {
      const uint32_t a = start+(z-z0)*stride+(x-x0);
      addQuad(mesh,a,a+stride,a+stride+1,a+1,material);
      }
  }

Fixture makeFixture(Model model) {
  Fixture mesh{model,{},{},{}};
  if(model==Model::Seams) {
    for(size_t m=0; m+1<MaterialCuts.size(); ++m)
      for(size_t chart=0; chart+1<ChartCuts.size(); ++chart)
        addGround(mesh,MaterialCuts[m],MaterialCuts[m+1],ChartCuts[chart],ChartCuts[chart+1],uint32_t(m),chart);
    }
  else if(model==Model::Overhang) {
    // An extruded rock with a re-entrant cross-section and a downward-facing underside.
    constexpr std::array<std::array<float, 2>, 8> profile =
        {{{0,0},{0,1800},{1200,2900},{1200,4300},{2000,4900},{3000,4200},{2600,1800},{900,0}}};
    constexpr uint32_t Steps = 5;
    for(size_t f=0; f<profile.size(); ++f) {
      const auto [y,z] = profile[f];
      const auto [yn,zn] = profile[(f+1)%profile.size()];
      const float dy = yn-y, dz = zn-z, length = std::hypot(dy,dz);
      const auto start = uint32_t(mesh.vertices.size());
      for(uint32_t s=0; s<=Steps; ++s)
        for(uint32_t x=0; x<=GridSize; ++x) {
          const float t = float(s)/float(Steps);
          mesh.vertices.push_back({{float(x)*CellCm,y+t*dy,z+t*dz},{0,-dz/length,dy/length},{float(x)/40.f,t},0xffffffffu});
          }
      for(uint32_t s=0; s<Steps; ++s)
        for(uint32_t x=0; x<GridSize; ++x) {
          const uint32_t a = start+s*(GridSize+1)+x;
          addQuad(mesh,a,a+1,a+GridSize+2,a+GridSize+1,0);
          }
      }
    mesh.boundary.resize(mesh.vertices.size());
    }
  else if(model==Model::FinishedBranch) {
    addGround(mesh,0,GridSize,0,GridSize,0,0);
    constexpr uint32_t ContactX = 24, ContactZ = 21, PatchRadius = 14;
    mesh.vertices[ContactZ*(GridSize+1)+ContactX].position[1] = 16.f;
    const Point point = mesh.vertices[ContactZ*(GridSize+1)+ContactX].position;
    mesh.contact = point;

    // This frozen sheet stops while the ground continues to simplify. Its only
    // contact with the ground is interior to both sheets, away from any open edge.
    const auto start = uint32_t(mesh.vertices.size());
    constexpr uint32_t PatchWidth = PatchRadius*2+1;
    for(uint32_t z=0; z<PatchWidth; ++z)
      for(uint32_t x=0; x<PatchWidth; ++x) {
        const float dx = float(x)-float(PatchRadius), dz = float(z)-float(PatchRadius);
        const float y = std::hypot(dx,dz)*80.f;
        auto vertex = groundVertex(Model::Flat,point[0]+dx*68.f,point[2]+dz*68.f,0);
        vertex.position[1] = point[1]+y;
        mesh.vertices.push_back(vertex);
        mesh.boundary.push_back(uint8_t(x!=PatchRadius || z!=PatchRadius));
        }
    for(uint32_t z=0; z+1<PatchWidth; ++z)
      for(uint32_t x=0; x+1<PatchWidth; ++x) {
        const uint32_t a = start+z*PatchWidth+x;
        addQuad(mesh,a,a+PatchWidth,a+PatchWidth+1,a+1,1);
        }
    }
  else {
    addGround(mesh,0,GridSize,0,GridSize,0,0);
    }
  return mesh;
  }

Point difference(const Point& a, const Point& b) {
  return {a[0]-b[0],a[1]-b[1],a[2]-b[2]};
  }

float distance(const Point& a, const Point& b) {
  const auto d = difference(a,b);
  return std::sqrt(d[0]*d[0]+d[1]*d[1]+d[2]*d[2]);
  }

float dot(const Point& a, const Point& b) {
  return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];
  }

Point cross(const Point& a, const Point& b) {
  return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};
  }

Point along(const Point& a, const Point& direction, float t) {
  return {a[0]+t*direction[0],a[1]+t*direction[1],a[2]+t*direction[2]};
  }

Point closestOnEdge(const Point& p, const Point& a, const Point& b) {
  const auto ab = difference(b,a);
  return along(a,ab,std::clamp(dot(difference(p,a),ab)/dot(ab,ab),0.f,1.f));
  }

Point closestOnTriangle(const Point& p, const std::array<Point,3>& triangle) {
  const auto& [a,b,c] = triangle;
  const auto ab = difference(b,a), bc = difference(c,b), ca = difference(a,c);
  const auto normal = cross(ab,bc);
  if(dot(cross(ab,difference(p,a)),normal)>=0.f && dot(cross(bc,difference(p,b)),normal)>=0.f &&
     dot(cross(ca,difference(p,c)),normal)>=0.f)
    return along(p,normal,-dot(difference(p,a),normal)/dot(normal,normal));
  const auto candidates = {closestOnEdge(p,a,b),closestOnEdge(p,b,c),closestOnEdge(p,c,a)};
  return *std::ranges::min_element(candidates,{},[&](const Point& q){ return distance(p,q); });
  }

using PositionBits = std::array<uint32_t, 3>;
using Edge = std::array<PositionBits, 2>;
using Segment = std::array<Point, 2>;

struct EdgeUse {
  std::array<uint32_t, 2> materials{NoGroup,NoGroup};
  int balance = 0, count = 0;
  bool materialInterface() const { return count==2 && materials[0]!=materials[1]; }
  };

struct Face {
  std::array<Point, 3> vertices;
  Point min, max;
  uint32_t material;

  bool within(const Point& point, float radius) const {
    for(size_t k=0; k<3; ++k)
      if(point[k]<min[k]-radius || point[k]>max[k]+radius)
        return false;
    return true;
    }
  };

struct Surface {
  std::map<Edge, EdgeUse> edges;
  std::set<PositionBits>  vertices;
  std::vector<Face>      faces;
  std::array<double, 3>  area{};
  std::array<bool, 2>    touches{};

  void add(const Fixture& fixture, const Vertex& a, const Vertex& b, const Vertex& c, uint32_t material) {
    const std::array<Vertex, 3> triangle = {a,b,c};
    const auto [nx,ny,nz] = cross(difference(b.position,a.position),difference(c.position,a.position));
    const double faceArea = std::hypot(double(nx),double(ny),double(nz))*0.5;
    require(faceArea>0.0,"degenerate triangle in a cut");
    if(fixture.model==Model::Seams) {
      const float z = (a.position[2]+b.position[2]+c.position[2])/3.f;
      const size_t chart = size_t(z>=float(ChartCuts[1])*CellCm) + size_t(z>=float(ChartCuts[2])*CellCm)
                         + size_t(z>=float(ChartCuts[3])*CellCm);
      for(const auto& vertex : triangle) {
        const auto& p = vertex.position;
        require(p[2]>=float(ChartCuts[chart])*CellCm && p[2]<=float(ChartCuts[chart+1])*CellCm,
                "triangle crosses an attribute seam");
        const auto expected = groundVertex(fixture.model,p[0],p[2],chart);
        require(vertex.uv==expected.uv && vertex.normal==expected.normal && vertex.color==expected.color,
                "triangle lost UV, normal or color at a seam");
        }
      }
    else if(fixture.model==Model::FinishedBranch) {
      require(material<touches.size(),"wrong material on one of the touching sheets");
      for(const auto& vertex : triangle)
        touches[material] |= vertex.position==fixture.contact;
      }
    else {
      require(material==0,"wrong material on a single-material surface");
      if(fixture.model==Model::Overhang)
        require(a.normal==b.normal && b.normal==c.normal && ny*a.normal[1]+nz*a.normal[2]>0.f,
                "overhang face crossed a fold or lost its winding");
      }
    area.at(material) += faceArea;

    for(size_t i=0; i<3; ++i) {
      const size_t j = (i+1)%3;
      const auto from = std::bit_cast<PositionBits>(triangle[i].position);
      const auto to   = std::bit_cast<PositionBits>(triangle[j].position);
      vertices.insert(from);
      const bool forward = from<to;
      auto& use = edges[forward ? Edge{from,to} : Edge{to,from}];
      if(use.count<2)
        use.materials[size_t(use.count)] = material;
      ++use.count;
      use.balance += forward ? 1 : -1;
      }
    }

  void addReferenceFace(const Vertex& a, const Vertex& b, const Vertex& c, uint32_t material) {
    Face face{{a.position,b.position,c.position},{},{},material};
    for(size_t k=0; k<3; ++k) {
      face.min[k] = std::min({a.position[k],b.position[k],c.position[k]});
      face.max[k] = std::max({a.position[k],b.position[k],c.position[k]});
      }
    faces.push_back(face);
    }
  };

struct Reference {
  struct Material {
    double contourLength = 0;
    size_t contourComponents = 0;
    };
  std::array<Material,3> materials;
  std::vector<Segment> interfaces;

  explicit Reference(const Surface& surface) {
    std::array<std::map<PositionBits,std::vector<PositionBits>>,3> contours;
    for(const auto& [edge,use] : surface.edges) {
      if(use.count!=1 && !use.materialInterface())
        continue;
      const auto [a,b] = edge;
      const Segment segment = {std::bit_cast<Point>(a),std::bit_cast<Point>(b)};
      if(use.materialInterface())
        interfaces.push_back(segment);
      const double length = distance(segment[0],segment[1]);
      for(uint32_t material : use.materials) {
        if(material==NoGroup)
          continue;
        materials.at(material).contourLength += length;
        contours.at(material)[a].push_back(b);
        contours.at(material)[b].push_back(a);
        }
      }
    for(size_t m=0; m<contours.size(); ++m) {
      auto& contour = contours[m];
      while(!contour.empty()) {
        ++materials[m].contourComponents;
        std::vector<PositionBits> component = {contour.begin()->first};
        while(!component.empty()) {
          const auto position = component.back();
          component.pop_back();
          auto at = contour.find(position);
          if(at==contour.end())
            continue;
          component.insert(component.end(),at->second.begin(),at->second.end());
          contour.erase(at);
          }
        }
      }
    }

  float interfaceDistance(const Point& point) const {
    float best = Infinity;
    for(const auto& [a,b] : interfaces)
      best = std::min(best,distance(point,closestOnEdge(point,a,b)));
    return best;
    }

  void checkMaterial(const Surface& source, const Point& center, uint32_t material, float errorCm) const {
    constexpr float RoundoffCm = 0.005f;
    float best = Infinity;
    uint32_t nearestMaterial = NoGroup;
    Point nearest{};
    for(const auto& face : source.faces) {
      if(!face.within(center,best+RoundoffCm))
        continue;
      const auto point = closestOnTriangle(center,face.vertices);
      const float toCenter = distance(center,point);
      if(toCenter < best || (toCenter == best && material == face.material)) {
        best = toCenter;
        nearest = point;
        nearestMaterial = face.material;
        }
      }
    require(nearestMaterial!=NoGroup,"missing reference surface");
    require(nearestMaterial==material || interfaceDistance(nearest)<=errorCm+RoundoffCm,
            "triangle material disagrees with level zero outside the transition error band");
    }
  };

void checkJunctions(const Surface& surface) {
  std::set<Point> points;
  for(const auto& [edge,use] : surface.edges) {
    points.insert(std::bit_cast<Point>(edge[0]));
    points.insert(std::bit_cast<Point>(edge[1]));
    }
  for(const auto& [edge,use] : surface.edges) {
    const auto a = std::bit_cast<Point>(edge[0]), b = std::bit_cast<Point>(edge[1]);
    const std::array<double,3> d = {double(b[0])-a[0],double(b[1])-a[1],double(b[2])-a[2]};
    for(auto it=points.lower_bound({std::min(a[0],b[0]),-Infinity,-Infinity});
        it!=points.end() && (*it)[0]<=std::max(a[0],b[0]); ++it) {
      const auto& p = *it;
      if(p==a || p==b || p[1]<std::min(a[1],b[1]) || p[1]>std::max(a[1],b[1]) ||
         p[2]<std::min(a[2],b[2]) || p[2]>std::max(a[2],b[2]))
        continue;
      const std::array<double,3> v = {double(p[0])-a[0],double(p[1])-a[1],double(p[2])-a[2]};
      require(d[0]*v[1]!=d[1]*v[0] || d[1]*v[2]!=d[2]*v[1] || d[2]*v[0]!=d[0]*v[2],"T-junction in a cut");
      }
    }
  }

bool checkSurface(const Fixture& fixture, const Surface& source, const Reference& reference,
                  const std::set<PositionBits>& frozen, const Graph& graph, const std::vector<uint32_t>& cut,
                  std::vector<bool>& checkedLocation) {
  Surface surface;
  double cutError = 0;
  for(const auto id : cut) {
    const auto& cluster = graph.clusters.at(id);
    const float errorCm = cluster.refinedGroup==NoGroup ? 0.f : graph.groups[cluster.refinedGroup].errorCm;
    cutError = std::max(cutError,double(errorCm));
    for(size_t i=0; i<cluster.triangleCount; ++i) {
      const auto vertex = [&](size_t corner) -> const Vertex& {
        const auto local = graph.indices.at(cluster.indexOffset+i*3+corner);
        require(local<cluster.vertexCount,"index exceeds its meshlet");
        return graph.vertices.at(graph.vertexIndices.at(cluster.vertexOffset+local));
        };
      const auto& a = vertex(0);
      const auto& b = vertex(1);
      const auto& c = vertex(2);
      if(fixture.model==Model::Seams && !checkedLocation[id]) {
        const Point center = {(a.position[0]+b.position[0]+c.position[0])/3.f,
                              (a.position[1]+b.position[1]+c.position[1])/3.f,
                              (a.position[2]+b.position[2]+c.position[2])/3.f};
        reference.checkMaterial(source,center,cluster.material,errorCm);
        }
      surface.add(fixture,a,b,c,cluster.material);
      }
    checkedLocation[id] = true;
    }
  if(fixture.model==Model::FinishedBranch)
    require(surface.touches[0] && surface.touches[1],"a replacement detached the earlier finished branch");
  bool movedInterface = false;
  for(const auto& [edge,use] : surface.edges) {
    require(use.count==1 || (use.count==2 && use.balance==0),"unpaired interior edge or overlapping triangles");
    if(use.count==1) {
      const auto original = source.edges.find(edge);
      require(original!=source.edges.end() && original->second.count==1 && original->second.balance==use.balance,
              "open edge changed or crack appeared in a cut");
      }
    if(fixture.model==Model::Seams && use.materialInterface()) {
      const auto a = std::bit_cast<Point>(edge[0]), b = std::bit_cast<Point>(edge[1]);
      const float shift = reference.interfaceDistance(along(a,difference(b,a),0.5f));
      require(shift<=cutError+0.005,"the material interface moved beyond the transition error");
      movedInterface |= shift>0.005f;
      }
    }
  for(const auto& [edge,use] : source.edges) {
    if(use.count==1) {
      const auto at = surface.edges.find(edge);
      require(at!=surface.edges.end() && at->second.count==1,"open edge disappeared in a cut");
      }
    }
  for(const auto& point : frozen)
    require(surface.vertices.contains(point),"frozen vertex changed or disappeared in a cut");

  for(size_t m=0; m<source.area.size(); ++m) {
    const auto [length,components] = reference.materials[m];
    const double tubeArea = 2*cutError*length + std::numbers::pi*cutError*cutError*double(components);
    const double tolerance = tubeArea + source.area[m]*16*std::numeric_limits<float>::epsilon();
    require(std::abs(surface.area[m]-source.area[m])<=tolerance,"material area changed beyond tolerance");
    }
  checkJunctions(surface);
  return movedInterface;
  }

template<class T>
bool identicalBytes(const std::vector<T>& a, const std::vector<T>& b) {
  return a.size()==b.size() && (a.empty() || std::memcmp(a.data(),b.data(),a.size()*sizeof(T))==0);
  }

size_t checkFixture(Model model) {
  const auto fixture = makeFixture(model);
  const auto graph = build(fixture.vertices,fixture.triangles,fixture.boundary);
  const auto repeat = build(fixture.vertices,fixture.triangles,fixture.boundary);
  require(identicalBytes(graph.vertices,repeat.vertices) && identicalBytes(graph.vertexIndices,repeat.vertexIndices) &&
          identicalBytes(graph.indices,repeat.indices) && identicalBytes(graph.clusters,repeat.clusters) &&
          identicalBytes(graph.groups,repeat.groups),"graph payload is not byte-deterministic");

  Surface source;
  std::set<PositionBits> frozen;
  for(size_t i=0; i<fixture.vertices.size(); ++i)
    if(fixture.boundary[i])
      frozen.insert(std::bit_cast<PositionBits>(fixture.vertices[i].position));
  for(const auto& triangle : fixture.triangles) {
    const auto& v = triangle.vertices;
    source.add(fixture,fixture.vertices[v[0]],fixture.vertices[v[1]],fixture.vertices[v[2]],triangle.material);
    if(model==Model::Seams)
      source.addReferenceFace(fixture.vertices[v[0]],fixture.vertices[v[1]],fixture.vertices[v[2]],triangle.material);
    }
  const Reference reference(source);

  const View view{{-2000.f,1400.f,850.f},1080.f,1.6f};
  std::vector<uint32_t> leaves, tops;
  struct GroupTriangles { size_t input = 0, output = 0; };
  std::vector<GroupTriangles> groupTriangles(graph.groups.size());
  size_t leafTriangles = 0, topTriangles = 0;
  uint32_t maxLevel = 0;
  for(uint32_t id=0; id<graph.clusters.size(); ++id) {
    const auto& cluster = graph.clusters[id];
    require(cluster.vertexCount>0 && cluster.vertexCount<=MaxVertices &&
            cluster.triangleCount>0 && cluster.triangleCount<=MaxTriangles,"meshlet exceeds the 64/64 range");
    maxLevel = std::max(maxLevel,cluster.level);
    const auto& parent = graph.groups.at(cluster.parentGroup);
    const Group own = cluster.refinedGroup==NoGroup ? Group{cluster.bounds,0.f} : graph.groups.at(cluster.refinedGroup);
    groupTriangles[cluster.parentGroup].input += cluster.triangleCount;
    if(cluster.refinedGroup!=NoGroup)
      groupTriangles[cluster.refinedGroup].output += cluster.triangleCount;
    require((cluster.level==0)==(cluster.refinedGroup==NoGroup),"unexpected transition for meshlet level");
    require(std::isfinite(own.errorCm) && own.errorCm>=0.f && own.errorCm<=parent.errorCm,
            "parent error failed to include the maximum child error");
    require(distance(own.bounds.center,parent.bounds.center)+own.bounds.radius<=parent.bounds.radius+0.01f,
            "parent sphere does not contain its child's LOD sphere");
    for(size_t i=0; i<cluster.vertexCount; ++i) {
      const auto& p = graph.vertices.at(graph.vertexIndices.at(cluster.vertexOffset+i)).position;
      require(distance(p,cluster.bounds.center)<=cluster.bounds.radius+0.01f,"meshlet sphere misses a vertex");
      }
    if(cluster.level==0) {
      leaves.push_back(id);
      leafTriangles += cluster.triangleCount;
      }
    if(parent.errorCm==Infinity) {
      tops.push_back(id);
      topTriangles += cluster.triangleCount;
      }
    }
  for(size_t id=0; id<graph.groups.size(); ++id) {
    const auto [input,output] = groupTriangles[id];
    if(graph.groups[id].errorCm==Infinity)
      require(output==0,"terminal group produced a replacement");
    else if(output==0 || output*100>input*85)
      throw std::runtime_error("group "+std::to_string(id)+" kept "+std::to_string(output)+"/"+
                               std::to_string(input)+" input triangles, expected a nonempty replacement at most 85%");
    }
  require(leafTriangles==fixture.triangles.size() && topTriangles<leafTriangles,"fixture hierarchy did not simplify");
  if(model==Model::Seams && topTriangles*4>leafTriangles)
    throw std::runtime_error("seam-heavy fixture kept "+std::to_string(topTriangles)+"/"+
                             std::to_string(leafTriangles)+" input triangles at the top, expected at most 25%");
  require(selectCut(graph,view,0.f)==leaves,"zero threshold must select exactly level zero");
  require(selectCut(graph,view,Infinity)==tops,"infinite threshold must select exactly the branch tops");
  if(model==Model::FinishedBranch)
    require(std::ranges::any_of(tops,[&](uint32_t id){
      const auto& cluster = graph.clusters[id];
      return cluster.material==1 && cluster.level+1<maxLevel;
      }),"touching branch did not stop early enough to exercise later replacements");

  std::vector<bool> checkedLocation(graph.clusters.size());
  bool movedInterface = false;

  // A whole level keeps the tops of any branches that ended in earlier rounds.
  for(uint32_t level=0; level<=maxLevel; ++level) {
    std::vector<uint32_t> cut;
    for(uint32_t id=0; id<graph.clusters.size(); ++id) {
      const auto& cluster = graph.clusters[id];
      if(cluster.level==level || (cluster.level<level && graph.groups[cluster.parentGroup].errorCm==Infinity))
        cut.push_back(id);
      }
    try {
      movedInterface |= checkSurface(fixture,source,reference,frozen,graph,cut,checkedLocation);
      }
    catch(const std::exception& error) {
      throw std::runtime_error("level "+std::to_string(level)+": "+error.what());
      }
    }

  std::mt19937 random(20261118);
  std::uniform_real_distribution<float> coordinate(-3600.f,10000.f);
  size_t mixedCuts = 0;
  for(size_t i=0; i<24; ++i) {
    const View sample{{coordinate(random),coordinate(random),coordinate(random)},1080.f,1.6f};
    std::vector<float> errors, thresholds;
    for(const auto& group : graph.groups) {
      const float projected = projectError(group,sample);
      errors.push_back(projected);
      if(std::isfinite(projected) && projected>0.f)
        thresholds.push_back(projected);
      }
    std::sort(thresholds.begin(),thresholds.end());
    const float threshold = thresholds.empty() ? 1.f : thresholds[i%thresholds.size()];
    std::vector<uint32_t> expected;
    for(uint32_t id=0; id<graph.clusters.size(); ++id) {
      const auto& cluster = graph.clusters[id];
      const float own = cluster.refinedGroup==NoGroup ? 0.f : errors[cluster.refinedGroup];
      const float parent = errors[cluster.parentGroup];
      require(own<=parent+0.0001f*std::max(1.f,own),"projected error is not monotone");
      if(own<=threshold && parent>threshold)
        expected.push_back(id);
      }
    const auto cut = selectCut(graph,sample,threshold);
    require(cut==expected && !cut.empty(),"finite threshold did not select the projected error interval");
    const auto level = graph.clusters[cut.front()].level;
    if(cut!=tops && std::ranges::any_of(cut,[&](uint32_t id){ return graph.clusters[id].level!=level; }))
      ++mixedCuts;
    try {
      movedInterface |= checkSurface(fixture,source,reference,frozen,graph,cut,checkedLocation);
      }
    catch(const std::exception& error) {
      const auto& p = sample.camera;
      throw std::runtime_error("camera "+std::to_string(i)+" ("+std::to_string(p[0])+", "+std::to_string(p[1])+", "+
                               std::to_string(p[2])+"), threshold "+std::to_string(threshold)+" px: "+error.what());
      }
    }
  if(model==Model::Seams)
    require(movedInterface,"the material interface stayed exact on every cut");
  std::cout << leafTriangles << " -> " << topTriangles << " triangles, " << maxLevel+1
            << " levels, " << mixedCuts << "/24 mixed camera cuts\n";
  return mixedCuts;
  }

float attributeErrorWithDetachedFace(float x) {
  Fixture mesh{Model::Flat,{},{},{}};
  addGround(mesh,0,2,0,2,0,0);
  mesh.vertices[4].uv[0] += 2.f;
  for(const auto& p : {Point{x,0,0},Point{x,0,CellCm},Point{x+CellCm,0,0}})
    mesh.vertices.push_back({p,{0,1,0},{0,0},0xffffffffu});
  mesh.triangles.push_back({{9,10,11},0});
  mesh.boundary.assign(mesh.vertices.size(),1);
  mesh.boundary[4] = 0;

  BuildStats stats;
  const auto graph = buildWithStats(mesh.vertices,mesh.triangles,mesh.boundary,stats);
  const float error = graph.groups.at(0).errorCm;
  // Positional quadrics include a small regularization penalty even on a plane.
  require(std::isfinite(error) && error>10.f*stats.positionErrorCm.at(0),
          "the planar UV fixture must be dominated by attribute error");
  return error;
  }

}

int main() {
  try {
    const View view{{0,0,0},800.f,1.5f};
    require(std::abs(projectError({{{0,0,1500},100.f},5.f},view)-5.f/1400.f*600.f)<0.00001f,
            "pixel projection is missing the sphere, perspective or screen-height factor");
    require(projectError({{{0,0,0},100.f},5.f},view)==3000.f,"pixel projection is missing the near-distance clamp");
    const float nearError = attributeErrorWithDetachedFace(1024.f);
    const float farError = attributeErrorWithDetachedFace(8192.f);
    require(std::abs(nearError-farError)<=nearError*0.01f,"attribute error changed with the group's extent");
    size_t mixedCuts = 0;
    const std::array models = {Model::Flat,Model::Hill,Model::Seams,Model::Overhang,Model::FinishedBranch};
    const std::array names = {"flat","hill","material and attribute seams","overhang","finished branch contact"};
    for(size_t i=0; i<models.size(); ++i) {
      std::cout << names[i] << ": ";
      try {
        mixedCuts += checkFixture(models[i]);
        }
      catch(const std::exception& error) {
        throw std::runtime_error(std::string(names[i])+": "+error.what());
        }
      }
    require(mixedCuts>0,"camera samples did not exercise any mixed cut");
    }
  catch(const std::exception& error) {
    std::cerr << "cluster LOD: " << error.what() << '\n';
    return 1;
    }
  return 0;
  }

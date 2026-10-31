#include "worldcloth.h"

#include "clothworlddata.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <numeric>
#include <sstream>
#include <unordered_map>

namespace {

struct Rule {
  std::string material, geometry, attachment, filter;
  float stiffness = .85f;
  float linearDamping = 1.5f;
  };

std::string materialKey(std::string name) {
  for(auto& c:name)
    c=char(std::toupper(static_cast<unsigned char>(c)));
  if(const auto extension=name.find_last_of('.'); extension!=std::string::npos)
    name.resize(extension);
  return name;
  }

const std::vector<Rule>& rules() {
  static const auto data=[] {
    std::vector<Rule> result;
    std::istringstream csv(ClothWorldData);
    std::string line;
    while(std::getline(csv,line)) {
      if(line.empty() || line[0]=='#')
        continue;
      std::array<std::string,6> field;
      std::istringstream row(line);
      for(auto& f:field)
        std::getline(row,f,',');
      Rule rule;
      rule.material=materialKey(field[0]);
      rule.geometry=field[1];
      rule.attachment=field[2];
      rule.stiffness=std::clamp(std::strtof(field[3].c_str(),nullptr),0.f,1.f);
      rule.filter=field[4];
      if(!field[5].empty())
        rule.linearDamping=std::clamp(std::strtof(field[5].c_str(),nullptr),0.f,10.f);
      result.emplace_back(std::move(rule));
      }
    return result;
    }();
  return data;
  }

// A compact subset preserves authored material parameters and vertex features,
// including both sides, UV seams and the native flag wave_mode.
asset::WorldMesh subset(const asset::WorldMesh& source,const std::vector<uint32_t>& faces) {
  asset::WorldMesh mesh;
  mesh.name=source.name;
  std::unordered_map<uint32_t,uint32_t> vertices,features,materials;
  for(auto face:faces) {
    const auto material=source.polygons.material_indices[face];
    auto [mat,isNew]=materials.emplace(material,uint32_t(materials.size()));
    if(isNew)
      mesh.materials.push_back(source.materials[material]);
    mesh.polygons.material_indices.push_back(mat->second);
    mesh.polygons.lightmap_indices.push_back(source.polygons.lightmap_indices[face]);
    mesh.polygons.flags.push_back(source.polygons.flags[face]);
    for(size_t c=0;c<3;++c) {
      const auto vertex=source.polygons.vertex_indices[3*face+c];
      auto [v,newVertex]=vertices.emplace(vertex,uint32_t(vertices.size()));
      if(newVertex)
        mesh.vertices.push_back(source.vertices[vertex]);
      mesh.polygons.vertex_indices.push_back(v->second);
      const auto feature=source.polygons.feature_indices[3*face+c];
      auto [f,newFeature]=features.emplace(feature,uint32_t(features.size()));
      if(newFeature)
        mesh.features.push_back(source.features[feature]);
      mesh.polygons.feature_indices.push_back(f->second);
      }
    }
  if(!mesh.vertices.empty()) {
    mesh.bbox.min=mesh.bbox.max=mesh.vertices.front();
    for(auto p:mesh.vertices) {
      mesh.bbox.min.x=std::min(mesh.bbox.min.x,p.x);
      mesh.bbox.min.y=std::min(mesh.bbox.min.y,p.y);
      mesh.bbox.min.z=std::min(mesh.bbox.min.z,p.z);
      mesh.bbox.max.x=std::max(mesh.bbox.max.x,p.x);
      mesh.bbox.max.y=std::max(mesh.bbox.max.y,p.y);
      mesh.bbox.max.z=std::max(mesh.bbox.max.z,p.z);
      }
    }
  return mesh;
  }

struct DVec3 {
  double x=0,y=0,z=0;
  DVec3 operator+(DVec3 b) const { return {x+b.x,y+b.y,z+b.z}; }
  DVec3 operator-(DVec3 b) const { return {x-b.x,y-b.y,z-b.z}; }
  DVec3 operator*(double b) const { return {x*b,y*b,z*b}; }
  double dot(DVec3 b) const { return x*b.x+y*b.y+z*b.z; }
  DVec3 normalized() const { return *this*(1./std::max(std::sqrt(dot(*this)),1e-12)); }
  };

std::vector<uint32_t> attachment(const asset::WorldMesh& mesh,const Rule& rule) {
  std::vector<uint32_t> pins;
  const auto& vertices=mesh.vertices;
  if(vertices.empty())
    return pins;
  if(rule.attachment.starts_with("ridge:")) {
    // The six ship sails are furled bundles. Their longest principal axis
    // follows the yard (five sails), or the diagonal forestay (the jib).
    // Fix the upper ridge along that support and the tied bundle ends.
    DVec3 center;
    for(auto p:vertices)
      center=center+DVec3{p.x,p.y,p.z};
    center=center*(1./double(vertices.size()));
    double covariance[3][3]={};
    for(auto p:vertices) {
      const DVec3 d=DVec3{p.x,p.y,p.z}-center;
      const double a[3]={d.x,d.y,d.z};
      for(size_t i=0;i<3;++i)
        for(size_t j=0;j<3;++j)
          covariance[i][j]+=a[i]*a[j];
      }
    DVec3 axis{1,1,1};
    for(size_t n=0;n<32;++n)
      axis=DVec3{covariance[0][0]*axis.x+covariance[0][1]*axis.y+covariance[0][2]*axis.z,
                 covariance[1][0]*axis.x+covariance[1][1]*axis.y+covariance[1][2]*axis.z,
                 covariance[2][0]*axis.x+covariance[2][1]*axis.y+covariance[2][2]*axis.z}.normalized();
    const DVec3 up=(DVec3{0,1,0}-axis*axis.y).normalized();
    double first=std::numeric_limits<double>::max(),last=-first,top=-first;
    for(auto p:vertices) {
      const auto d=DVec3{p.x,p.y,p.z}-center;
      first=std::min(first,d.dot(axis));
      last=std::max(last,d.dot(axis));
      top=std::max(top,d.dot(up));
      }
    const auto separator=rule.attachment.find(':',6);
    const float ridge=std::strtof(rule.attachment.c_str()+6,nullptr);
    const float ends=separator==std::string::npos ? 0.f : std::strtof(rule.attachment.c_str()+separator+1,nullptr);
    for(size_t i=0;i<vertices.size();++i) {
      const auto p=vertices[i];
      const auto d=DVec3{p.x,p.y,p.z}-center;
      if(d.dot(up)>=top-ridge || d.dot(axis)<=first+ends || d.dot(axis)>=last-ends)
        pins.push_back(uint32_t(i));
      }
    return pins;
    }
  const bool alongY=rule.attachment[0]=='y';
  const bool maximum=rule.attachment.find(":max:")!=std::string::npos;
  const auto delimiter=rule.attachment.find_last_of(':');
  const float tolerance=std::strtof(rule.attachment.c_str()+delimiter+1,nullptr);
  float edge=maximum ? -std::numeric_limits<float>::max() : std::numeric_limits<float>::max();
  for(auto p:vertices) {
    const float value=alongY ? p.y : p.z;
    edge=maximum ? std::max(edge,value) : std::min(edge,value);
    }
  for(size_t i=0;i<vertices.size();++i) {
    const float value=alongY ? vertices[i].y : vertices[i].z;
    if(std::abs(value-edge)<=tolerance)
      pins.push_back(uint32_t(i));
    }
  return pins;
  }

}

WorldCloth::Extracted WorldCloth::extract(asset::WorldMesh& landscape) {
  Extracted result;
  const auto faceCount=landscape.polygons.material_indices.size();
  std::vector<bool> selected(faceCount,false);
  for(const auto& rule:rules()) {
    std::vector<bool> materialMatches(landscape.materials.size(),false);
    for(size_t i=0;i<landscape.materials.size();++i) {
      const auto& material=landscape.materials[i];
      materialMatches[i]=materialKey(material.name)==rule.material || materialKey(material.texture)==rule.material;
      }
    std::vector<uint32_t> faces;
    for(size_t f=0;f<faceCount;++f)
      if(materialMatches[landscape.polygons.material_indices[f]])
        faces.push_back(uint32_t(f));
    // Connected components use source position indices, so UV seams and
    // double-sided triangles stay in the same authored cloth object.
    std::unordered_map<uint32_t,std::vector<uint32_t>> adjacent;
    for(auto f:faces)
      for(size_t c=0;c<3;++c)
        adjacent[landscape.polygons.vertex_indices[3*f+c]].push_back(f);
    std::unordered_map<uint32_t,bool> visited;
    size_t ordinal=0;
    for(auto first:faces) {
      if(visited[first])
        continue;
      std::vector<uint32_t> island{first};
      visited[first]=true;
      for(size_t at=0;at<island.size();++at)
        for(size_t c=0;c<3;++c)
          for(auto next:adjacent[landscape.polygons.vertex_indices[3*island[at]+c]])
            if(!visited[next]) {
              visited[next]=true;
              island.push_back(next);
              }
      std::sort(island.begin(),island.end());
      auto mesh=subset(landscape,island);
      // City finials share the banner material, but have 16 positions and
      // 28 triangles. Only the four-position, four-triangle cloth sheet moves.
      if(rule.filter=="quad" && (mesh.vertices.size()!=4 || island.size()!=4))
        continue;
      if(rule.filter=="sheet" && mesh.vertices.size()<9)
        continue;
      if(rule.filter=="bundle" && mesh.vertices.size()<20)
        continue;
      Surface surface;
      surface.pins=attachment(mesh,rule);
      if(surface.pins.empty() || surface.pins.size()==mesh.vertices.size())
        continue;
      surface.id=0x80000000u|island.front();
      surface.visual=rule.material+":"+std::to_string(++ordinal);
      surface.stiffness=rule.stiffness;
      surface.linearDamping=rule.linearDamping;
      surface.mesh=std::move(mesh);
      result.surfaces.emplace_back(std::move(surface));
      for(auto f:island)
        selected[f]=true;
      }
    }
  std::vector<uint32_t> clothFaces;
  for(size_t i=0;i<faceCount;++i)
    if(selected[i])
      clothFaces.push_back(uint32_t(i));
  if(clothFaces.empty())
    return result;
  result.original=subset(landscape,clothFaces);
  asset::PolygonList remaining;
  const auto remainingCount=faceCount-clothFaces.size();
  remaining.material_indices.reserve(remainingCount);
  remaining.lightmap_indices.reserve(remainingCount);
  remaining.flags.reserve(remainingCount);
  remaining.vertex_indices.reserve(remainingCount*3);
  remaining.feature_indices.reserve(remainingCount*3);
  for(size_t i=0;i<faceCount;++i) {
    if(selected[i])
      continue;
    remaining.material_indices.push_back(landscape.polygons.material_indices[i]);
    remaining.lightmap_indices.push_back(landscape.polygons.lightmap_indices[i]);
    remaining.flags.push_back(landscape.polygons.flags[i]);
    for(size_t c=0;c<3;++c) {
      remaining.vertex_indices.push_back(landscape.polygons.vertex_indices[3*i+c]);
      remaining.feature_indices.push_back(landscape.polygons.feature_indices[3*i+c]);
      }
    }
  landscape.polygons=std::move(remaining);
  return result;
  }

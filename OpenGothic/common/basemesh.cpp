#include "basemesh.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

#include "gothicglb.h"

namespace {

using namespace gothicglb;

[[noreturn]] void fail(const std::string& reason) {
  throw std::runtime_error("base mesh: " + reason);
  }

constexpr const char* meshFormat = "gothic-mesh";
constexpr int         formatVersion = 1;
constexpr float       baseScale = 0.01f;

std::string vec3Json(const zenkit::Vec3& v) {
  return "[" + jsonFloat(v.x) + "," + jsonFloat(v.y) + "," + jsonFloat(v.z) + "]";
  }

zenkit::Vec3 jsonToVec3(const Json& v, const char* name) {
  if(!v.IsArray() || v.Size()!=3)
    fail(std::string(name) + " must be [x,y,z]");
  return {jsonToFloat(v[0],name),jsonToFloat(v[1],name),jsonToFloat(v[2],name)};
  }

bool sameVec3(const zenkit::Vec3& a, const zenkit::Vec3& b) {
  return bits(a.x)==bits(b.x) && bits(a.y)==bits(b.y) && bits(a.z)==bits(b.z);
  }

}

void writeBaseMesh(const asset::StaticMesh& mesh, const std::string& name, const std::filesystem::path& file) {
  if(mesh.positions.empty())
    fail(name + ": no positions");
  GlbBuilder glb;
  // Every position, also those no wedge uses, as the MRM holds them.
  std::vector<float> positions;
  for(const auto& p:mesh.positions) {
    positions.push_back(-p.x);
    positions.push_back(p.y);
    positions.push_back(p.z);
    }
  const size_t aAll = glb.accessor(positions,5126,"VEC3",3,false,0);

  std::string materials = "[", primitives = "[", submeshes = "[";
  size_t written = 0;
  for(size_t s=0; s<mesh.sub_meshes.size(); ++s) {
    const auto& sm = mesh.sub_meshes[s];
    if(s>0)
      submeshes += ",";
    submeshes += materialJson(sm.mat,s);
    if(sm.triangles.empty() || sm.wedges.empty())
      continue;
    std::vector<float> pos, nrm, uv, pid;
    float lo[3], hi[3];
    for(size_t w=0; w<sm.wedges.size(); ++w) {
      const auto& wedge = sm.wedges[w];
      if(wedge.index>=mesh.positions.size())
        fail(name + ": wedge position out of range");
      const auto& p = mesh.positions[wedge.index];
      const float gp[3] = {-p.x,p.y,p.z};
      for(unsigned ax=0; ax<3; ++ax) {
        pos.push_back(gp[ax]);
        lo[ax] = w==0 ? gp[ax] : std::min(lo[ax],gp[ax]);
        hi[ax] = w==0 ? gp[ax] : std::max(hi[ax],gp[ax]);
        }
      nrm.push_back(-wedge.normal.x);
      nrm.push_back(wedge.normal.y);
      nrm.push_back(wedge.normal.z);
      uv.push_back(wedge.texture.x);
      uv.push_back(wedge.texture.y);
      pid.push_back(float(wedge.index));
      }
    std::vector<uint16_t> idx;
    for(const auto& t:sm.triangles)
      for(const auto w:t.wedges) {
        if(w>=sm.wedges.size())
          fail(name + ": triangle wedge out of range");
        idx.push_back(w);
        }
    const std::string minMax = ",\"min\":[" + jsonFloat(lo[0]) + "," + jsonFloat(lo[1]) + "," + jsonFloat(lo[2]) +
                               "],\"max\":[" + jsonFloat(hi[0]) + "," + jsonFloat(hi[1]) + "," + jsonFloat(hi[2]) + "]";
    const size_t aPos = glb.accessor(pos,5126,"VEC3",3,false,34962,minMax);
    const size_t aNrm = glb.accessor(nrm,5126,"VEC3",3,false,34962);
    const size_t aUv  = glb.accessor(uv,5126,"VEC2",2,false,34962);
    const size_t aPid = glb.accessor(pid,5126,"SCALAR",1,false,34962);
    const size_t aIdx = glb.accessor(idx,5123,"SCALAR",1,false,34963);
    if(written>0) {
      materials += ",";
      primitives += ",";
      }
    materials += "{\"name\":" + jsonString(sm.mat.name) + ",\"extras\":{\"gothic\":" + materialJson(sm.mat,s) + "}}";
    primitives += "{\"attributes\":{\"POSITION\":" + std::to_string(aPos) + ",\"NORMAL\":" + std::to_string(aNrm) +
                  ",\"TEXCOORD_0\":" + std::to_string(aUv) + ",\"_POSITION\":" + std::to_string(aPid) +
                  "},\"indices\":" + std::to_string(aIdx) + ",\"material\":" + std::to_string(written) +
                  ",\"mode\":4,\"extras\":{\"gothic\":{\"submesh\":" + std::to_string(s) + "}}}";
    ++written;
    }
  materials += "]";
  primitives += "]";
  submeshes += "]";
  if(written==0)
    fail(name + ": no triangles");

  const std::string json =
    "{\"asset\":{\"version\":\"2.0\",\"generator\":\"gothic-ai basemesh\"},"
    "\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
    "\"nodes\":[{\"name\":" + jsonString(name) + ",\"mesh\":0,\"scale\":[0.01,0.01,0.01]}],"
    "\"meshes\":[{\"name\":" + jsonString(name) + ",\"primitives\":" + primitives +
    ",\"extras\":{\"gothic\":{\"format\":\"" + meshFormat + "\",\"version\":" + std::to_string(formatVersion) +
    ",\"name\":" + jsonString(name) + ",\"alpha_test\":" + std::to_string(int(mesh.alpha_test)) +
    ",\"bbox\":[" + vec3Json(mesh.bbox.min) + "," + vec3Json(mesh.bbox.max) + "]" +
    ",\"positions\":" + std::to_string(aAll) + ",\"submeshes\":" + submeshes + "}}}]," +
    "\"materials\":" + materials + "," +
    "\"accessors\":" + glb.accessorsJson() + "," +
    "\"bufferViews\":" + glb.viewsJson() + "," +
    "\"buffers\":[{\"byteLength\":" + std::to_string(glb.bin.size()) + "}]}";
  const auto bytes = glb.finish(json);
  writeFile(file,bytes.data(),bytes.size());
  }

asset::StaticMesh readBaseMesh(const std::filesystem::path& file) {
  auto parsed = parseGlb(file);
  const auto& d = *parsed.data;
  if(d.meshes_count!=1 || d.nodes_count!=1 || d.nodes[0].mesh!=&d.meshes[0])
    fail(file.string() + ": expected one node with one mesh");
  float m[16];
  cgltf_node_transform_world(&d.nodes[0],m);
  for(unsigned i=0; i<16; ++i) {
    const float expected = (i==0 || i==5 || i==10) ? baseScale : (i==15 ? 1.f : 0.f);
    if(m[i]!=expected)
      fail(file.string() + ": node transform must be the converter's 0.01 scale");
    }
  const auto& mesh = d.meshes[0];
  const auto extras = parseExtras(d,mesh.extras);
  if(extras==nullptr)
    fail(file.string() + ": no extras.gothic on the mesh");
  const auto& g = (*extras)["gothic"];
  if(jsonToString(member(g,"format"),"format")!=meshFormat || jsonToInt(member(g,"version"),"version")!=formatVersion)
    fail(file.string() + ": not a converted mesh of this version");

  asset::StaticMesh out;
  out.alpha_test = uint8_t(jsonToInt(member(g,"alpha_test"),"alpha_test"));
  const auto& bbox = member(g,"bbox");
  if(!bbox.IsArray() || bbox.Size()!=2)
    fail("bbox must be [min,max]");
  out.bbox.min = jsonToVec3(bbox[0],"bbox");
  out.bbox.max = jsonToVec3(bbox[1],"bbox");

  const auto posIndex = jsonToInt(member(g,"positions"),"positions");
  if(posIndex<0 || size_t(posIndex)>=d.accessors_count)
    fail("positions accessor out of range");
  const auto& posAcc = d.accessors[posIndex];
  if(posAcc.type!=cgltf_type_vec3 || posAcc.component_type!=cgltf_component_type_r_32f)
    fail("positions must be float VEC3");
  out.positions.resize(posAcc.count);
  for(size_t i=0; i<posAcc.count; ++i) {
    float v[3];
    if(!cgltf_accessor_read_float(&posAcc,i,v,3))
      fail("cannot read positions");
    out.positions[i] = {-v[0],v[1],v[2]};
    }

  const auto& subs = member(g,"submeshes");
  if(!subs.IsArray())
    fail("submeshes must be an array");
  out.sub_meshes.resize(subs.Size());
  for(rapidjson::SizeType s=0; s<subs.Size(); ++s) {
    out.sub_meshes[s].mat = materialFromJson(subs[s]);
    out.materials.push_back(out.sub_meshes[s].mat);
    }

  for(size_t pi=0; pi<mesh.primitives_count; ++pi) {
    const auto& p = mesh.primitives[pi];
    const auto pe = parseExtras(d,p.extras);
    if(pe==nullptr)
      fail("primitive without extras.gothic.submesh");
    const auto s = jsonToInt(member((*pe)["gothic"],"submesh"),"submesh");
    if(s<0 || size_t(s)>=out.sub_meshes.size())
      fail("submesh index out of range");
    auto& sm = out.sub_meshes[size_t(s)];
    if(!sm.wedges.empty())
      fail("submesh written twice");
    const auto* nrm = findAttribute(p,cgltf_attribute_type_normal);
    const auto* uv  = findAttribute(p,cgltf_attribute_type_texcoord);
    const auto* pid = findAttribute(p,cgltf_attribute_type_custom,"_POSITION");
    if(nrm==nullptr || uv==nullptr || pid==nullptr || p.indices==nullptr)
      fail("primitive needs NORMAL, TEXCOORD_0, _POSITION and indices");
    sm.wedges.resize(pid->count);
    for(size_t w=0; w<pid->count; ++w) {
      float n[3], t[2], id;
      if(!cgltf_accessor_read_float(nrm,w,n,3) || !cgltf_accessor_read_float(uv,w,t,2) ||
         !cgltf_accessor_read_float(pid,w,&id,1))
        fail("cannot read wedge");
      if(!(id>=0) || id!=std::floor(id) || size_t(id)>=out.positions.size())
        fail("wedge position out of range");
      sm.wedges[w] = {{-n[0],n[1],n[2]},{t[0],t[1]},uint16_t(id)};
      }
    if(p.indices->count%3!=0)
      fail("index count is not a multiple of 3");
    sm.triangles.resize(p.indices->count/3);
    for(size_t t=0; t<sm.triangles.size(); ++t)
      for(unsigned c=0; c<3; ++c) {
        const auto w = cgltf_accessor_read_index(p.indices,t*3+c);
        if(w>=sm.wedges.size())
          fail("triangle wedge out of range");
        sm.triangles[t].wedges[c] = uint16_t(w);
        }
    }
  return out;
  }

std::string compareBaseMeshes(const asset::StaticMesh& e, const asset::StaticMesh& a) {
  if(e.alpha_test!=a.alpha_test)
    return "alpha_test differs";
  if(!sameVec3(e.bbox.min,a.bbox.min) || !sameVec3(e.bbox.max,a.bbox.max))
    return "bbox differs";
  if(e.positions.size()!=a.positions.size())
    return "positions: " + std::to_string(e.positions.size()) + " vs " + std::to_string(a.positions.size());
  for(size_t i=0; i<e.positions.size(); ++i)
    if(!sameVec3(e.positions[i],a.positions[i]))
      return "position " + std::to_string(i) + " differs";
  if(e.sub_meshes.size()!=a.sub_meshes.size())
    return "submeshes: " + std::to_string(e.sub_meshes.size()) + " vs " + std::to_string(a.sub_meshes.size());
  for(size_t s=0; s<e.sub_meshes.size(); ++s) {
    const auto& x = e.sub_meshes[s];
    const auto& y = a.sub_meshes[s];
    const auto at = "submesh " + std::to_string(s) + " (" + x.mat.name + "): ";
    if(!sameMaterial(x.mat,y.mat))
      return at + "material differs";
    // An empty submesh keeps its material but no geometry.
    const bool emptyX = x.triangles.empty() || x.wedges.empty();
    const bool emptyY = y.triangles.empty() || y.wedges.empty();
    if(emptyX || emptyY) {
      if(emptyX!=emptyY)
        return at + "one side is empty";
      continue;
      }
    if(x.wedges.size()!=y.wedges.size())
      return at + "wedge count differs";
    for(size_t w=0; w<x.wedges.size(); ++w) {
      const auto& p = x.wedges[w];
      const auto& q = y.wedges[w];
      if(p.index!=q.index || !sameVec3(p.normal,q.normal) ||
         bits(p.texture.x)!=bits(q.texture.x) || bits(p.texture.y)!=bits(q.texture.y))
        return at + "wedge " + std::to_string(w) + " differs";
      }
    if(x.triangles.size()!=y.triangles.size())
      return at + "triangle count differs";
    for(size_t t=0; t<x.triangles.size(); ++t)
      for(unsigned c=0; c<3; ++c)
        if(x.triangles[t].wedges[c]!=y.triangles[t].wedges[c])
          return at + "triangle " + std::to_string(t) + " differs";
    }
  return {};
  }

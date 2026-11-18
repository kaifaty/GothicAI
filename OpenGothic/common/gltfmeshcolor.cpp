#include "gltfmeshcolor.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <string>

#include "../lib/cgltf/cgltf.h"

namespace {

[[noreturn]] void reject(const std::string& message) {
  throw std::runtime_error("COLOR_0: "+message);
  }

// loadGltfMesh has rejected a duplicate attribute: the first is the only one
const cgltf_accessor* attribute(const cgltf_primitive& primitive, cgltf_attribute_type type) {
  for(size_t i=0; i<primitive.attributes_count; ++i)
    if(primitive.attributes[i].type==type && primitive.attributes[i].index==0)
      return primitive.attributes[i].data;
  return nullptr;
  }

void requireColor(const cgltf_accessor& color, size_t count) {
  const bool real = color.component_type==cgltf_component_type_r_32f && !color.normalized;
  const bool norm = (color.component_type==cgltf_component_type_r_8u || color.component_type==cgltf_component_type_r_16u) &&
                    color.normalized;
  if((color.type!=cgltf_type_vec3 && color.type!=cgltf_type_vec4) || !(real || norm))
    reject("must use float or normalized unsigned RGB or RGBA");
  if(color.count!=count)
    reject("its count does not match POSITION");
  }

uint32_t vertexColor(const cgltf_accessor* color, size_t index) {
  if(color==nullptr)
    return 0xFFFFFFFFu;
  float c[4] = {1.f,1.f,1.f,1.f};
  if(!cgltf_accessor_read_float(color,index,c,color->type==cgltf_type_vec3 ? 3 : 4))
    reject("cannot be read");
  uint32_t ret = 0;
  for(unsigned i=0; i<4; ++i) {
    if(!std::isfinite(c[i]))
      reject("is not finite");
    ret |= uint32_t(std::lround(std::clamp(c[i],0.f,1.f)*255.f)) << (8u*i);
    }
  return ret;
  }

// the wedges one primitive has added to its sub-mesh: WedgeStream of gltfmesh.cpp, and the colour they took
struct Stream {
  const cgltf_node*     node;
  const cgltf_accessor* position;
  const cgltf_accessor* normal;
  const cgltf_accessor* uv;
  const cgltf_accessor* color;
  };

struct MaterialState {
  const cgltf_material* source;
  std::vector<Stream>   streams;
  };

}

GltfMeshColors loadGltfMeshColors(const std::vector<uint8_t>& glb, const asset::StaticMesh& mesh) {
  cgltf_options options = {};
  options.type = cgltf_file_type_glb;
  cgltf_data* parsed = nullptr;
  if(cgltf_parse(&options,glb.data(),glb.size(),&parsed)!=cgltf_result_success)
    reject("the GLB cannot be parsed");
  const std::unique_ptr<cgltf_data,decltype(&cgltf_free)> data(parsed,cgltf_free);
  if(data->scene==nullptr || data->buffers_count!=1 || data->bin==nullptr || data->buffers[0].size>data->bin_size)
    reject("the GLB is not one loadGltfMesh takes");
  // as loadGltfMesh does: the embedded BIN only, never cgltf's URI loader
  data->buffers[0].data = const_cast<void*>(data->bin);
  if(cgltf_validate(data.get())!=cgltf_result_success)
    reject("the GLB is not valid");

  GltfMeshColors             colors;
  std::vector<MaterialState> materials;
  bool                       any = false;
  std::vector<const cgltf_node*> pending(data->scene->nodes,data->scene->nodes+data->scene->nodes_count);
  std::vector<bool>              visited(data->nodes_count,false);
  while(!pending.empty()) {
    const auto* node = pending.back();
    pending.pop_back();
    const size_t index = size_t(node-data->nodes);
    if(visited[index])
      reject("a scene node is referenced more than once");
    visited[index] = true;
    for(size_t p=0; node->mesh!=nullptr && p<node->mesh->primitives_count; ++p) {
      const auto& primitive = node->mesh->primitives[p];
      const auto* position  = attribute(primitive,cgltf_attribute_type_position);
      const auto* normal    = attribute(primitive,cgltf_attribute_type_normal);
      const auto* uv        = attribute(primitive,cgltf_attribute_type_texcoord);
      const auto* color     = attribute(primitive,cgltf_attribute_type_color);
      if(position==nullptr)
        reject("a primitive has no POSITION");

      size_t material = 0;
      while(material<materials.size() && materials[material].source!=primitive.material)
        ++material;
      if(material==materials.size()) {
        materials.push_back({primitive.material,{}});
        colors.emplace_back();
        }
      auto& streams = materials[material].streams;
      const auto shared = std::find_if(streams.begin(),streams.end(),[&](const Stream& s) {
        return s.node==node && s.position==position && s.normal==normal && s.uv==uv;
        });
      if(shared!=streams.end()) {
        if(shared->color!=color)
          reject("primitives of one node and material with the same POSITION, NORMAL and TEXCOORD_0 share their "
                 "vertices and cannot differ in COLOR_0");
        continue;
        }
      if(color!=nullptr) {
        requireColor(*color,position->count);
        any = true;
        }
      streams.push_back({node,position,normal,uv,color});
      for(size_t i=0; i<position->count; ++i)
        colors[material].push_back(vertexColor(color,i));
      }
    for(size_t i=0; i<node->children_count; ++i)
      pending.push_back(node->children[i]);
    }
  if(!any)
    return {};
  if(colors.size()!=mesh.sub_meshes.size())
    reject("the materials are not the mesh's");
  for(size_t i=0; i<colors.size(); ++i)
    if(colors[i].size()!=mesh.sub_meshes[i].wedges.size())
      reject("the vertices are not the mesh's");
  return colors;
  }

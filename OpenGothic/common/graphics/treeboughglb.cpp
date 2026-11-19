#include "treeboughglb.h"

#include <cmath>
#include <cstring>
#include <memory>
#include <stdexcept>

#include "../../lib/cgltf/cgltf.h"

using namespace TreeBough;

namespace {

[[noreturn]] void reject(const char* reason) {
  throw std::runtime_error(std::string("tree hierarchy GLB: ") + reason);
  }

void validateGlb(cgltf_data& data) {
  if(data.asset.version==nullptr || std::strcmp(data.asset.version, "2.0")!=0 ||
     data.extensions_required_count!=0 || data.skins_count!=0 || data.animations_count!=0 ||
     data.scene==nullptr || data.scene->nodes_count!=1 || data.nodes_count!=1 ||
     data.scene->nodes[0]!=data.nodes || data.meshes_count!=1)
    reject("expected a static GLB 2.0 scene with one mesh node");
  const auto& node = data.nodes[0];
  if(node.mesh!=data.meshes || node.children_count!=0 || node.has_matrix || node.has_translation ||
     node.has_rotation || node.has_scale || node.has_mesh_gpu_instancing || node.weights_count!=0)
    reject("expected a tree mesh in model coordinates");
  if(node.mesh->primitives_count<2 || node.mesh->weights_count!=0)
    reject("expected wood and source foliage primitives");
  for(size_t p=0; p<node.mesh->primitives_count; ++p) {
    const auto& prim = node.mesh->primitives[p];
    if(prim.type!=cgltf_primitive_type_triangles || prim.targets_count!=0 || prim.has_draco_mesh_compression)
      reject("expected uncompressed indexed triangle primitives");
    if(prim.indices==nullptr || prim.indices->type!=cgltf_type_scalar ||
       prim.indices->component_type!=cgltf_component_type_r_16u || prim.indices->normalized ||
       prim.indices->count%3!=0)
      reject("expected unsigned short triangle indices");
    }

  if(data.buffers_count!=1 || data.buffers[0].uri!=nullptr || data.bin==nullptr ||
     data.buffers[0].size>data.bin_size || data.bin_size-data.buffers[0].size>3 ||
     reinterpret_cast<uintptr_t>(data.bin)%4!=0)
    reject("expected one aligned embedded buffer");
  for(size_t i=0; i<data.buffer_views_count; ++i) {
    const auto& view = data.buffer_views[i];
    if(view.has_meshopt_compression || view.buffer!=data.buffers ||
       view.offset>data.buffers[0].size || view.size>data.buffers[0].size-view.offset)
      reject("buffer view is outside the embedded buffer");
    }
  for(size_t i=0; i<data.accessors_count; ++i) {
    const auto& acc = data.accessors[i];
    const size_t component = cgltf_component_size(acc.component_type);
    const size_t element = cgltf_calc_size(acc.type, acc.component_type);
    if(acc.buffer_view==nullptr || acc.is_sparse || acc.count==0 || component==0 || element==0 ||
       acc.stride<element || acc.stride%component!=0)
      reject("unsupported accessor");
    const auto& view = *acc.buffer_view;
    if(acc.offset>view.size || (view.offset+acc.offset)%component!=0 || element>view.size-acc.offset ||
       acc.count-1>(view.size-acc.offset-element)/acc.stride)
      reject("accessor is outside its buffer view or misaligned");
    }

  data.buffers[0].data = const_cast<void*>(data.bin);
  if(cgltf_validate(&data)!=cgltf_result_success)
    reject("invalid mesh data");
  }

const cgltf_accessor& attribute(const cgltf_primitive& prim, const char* name, cgltf_type type,
                                cgltf_component_type component, bool normalized = false) {
  const cgltf_accessor* found = nullptr;
  for(size_t i=0; i<prim.attributes_count; ++i) {
    const auto& attr = prim.attributes[i];
    if(attr.name==nullptr || std::strcmp(attr.name,name)!=0)
      continue;
    if(found!=nullptr)
      reject("duplicate foliage vertex attribute");
    found = attr.data;
    }
  if(found==nullptr || found->type!=type || found->component_type!=component || bool(found->normalized)!=normalized)
    throw std::runtime_error(std::string("tree hierarchy GLB: unsupported or missing ") + name);
  return *found;
  }

std::string materialKey(const cgltf_material* material) {
  if(material==nullptr || material->name==nullptr ||
     material->alpha_mode!=cgltf_alpha_mode_mask || material->alpha_cutoff!=0.5f)
    reject("each foliage primitive needs a named alpha-mask material with cutoff 0.5");
  std::string key = material->name;
  key.resize(cgltf_decode_string(key.data()));
  if(key.empty() || key=="." || key==".." || key.find('\0')!=std::string::npos ||
     key.find_first_of("/\\:")!=std::string::npos)
    reject("invalid foliage material name");
  if(!std::filesystem::path(key).has_extension())
    key += ".TGA";
  return key;
  }

GlbPage decodePage(const cgltf_primitive& primitive, bool source) {
  const auto& pos   = attribute(primitive, "POSITION", cgltf_type_vec3, cgltf_component_type_r_32f);
  const auto& norm  = attribute(primitive, "NORMAL", cgltf_type_vec3, cgltf_component_type_r_32f);
  const auto& uv    = attribute(primitive, "TEXCOORD_0", cgltf_type_vec2, cgltf_component_type_r_32f);
  const auto& color = attribute(primitive, "COLOR_0", cgltf_type_vec4, cgltf_component_type_r_8u, true);
  if(pos.count>UINT16_MAX)
    reject("foliage primitive exceeds the compact page vertex limit");

  GlbPage page;
  page.texture = materialKey(primitive.material);
  page.vertices.resize(pos.count);
  page.indices.reserve(primitive.indices->count);
  for(size_t i=0; i<pos.count; ++i) {
    auto& out = page.vertices[i];
    float p[3], n[3];
    cgltf_uint rgba[4];
    if(!cgltf_accessor_read_float(&pos, i, p, 3) || !cgltf_accessor_read_float(&norm, i, n, 3) ||
       !cgltf_accessor_read_float(&uv, i, out.uv, 2) || !cgltf_accessor_read_uint(&color, i, rgba, 4))
      reject("cannot read foliage vertex");
    const double length = std::hypot(double(n[0]), double(n[1]), double(n[2]));
    if(!std::isfinite(length) || length==0 || !std::isfinite(out.uv[0]) || !std::isfinite(out.uv[1]))
      reject("invalid foliage normal or texture coordinates");
    for(size_t a=0; a<3; ++a) {
      const double sign = a==0 ? -1.0 : 1.0;
      out.pos[a] = float(p[a] * (sign * 100.0));
      out.norm[a] = float(n[a] * sign / length);
      if(!std::isfinite(out.pos[a]))
        reject("invalid foliage position");
      }
    out.color = rgba[0] | (rgba[1]<<8) | (rgba[2]<<16) | (rgba[3]<<24);
    }
  for(size_t i=0; i<primitive.indices->count; ++i)
    page.indices.push_back(uint32_t(cgltf_accessor_read_index(primitive.indices, i)));

  if(source) {
    const auto& cards = attribute(primitive, "_CARD_ID", cgltf_type_scalar, cgltf_component_type_r_16u);
    page.cardIds.resize(pos.count);
    for(size_t i=0; i<pos.count; ++i)
      if(!cgltf_accessor_read_uint(&cards, i, &page.cardIds[i], 1))
        reject("cannot read source card ID");
    }
  return page;
  }

}

std::vector<GlbPage> TreeBough::loadGlb(const std::filesystem::path& path) {
  cgltf_options options = {};
  options.type = cgltf_file_type_glb;
  cgltf_data* parsed = nullptr;
  const auto file = path.string();
  if(cgltf_parse_file(&options, file.c_str(), &parsed)!=cgltf_result_success)
    reject("cannot parse file");
  const std::unique_ptr<cgltf_data,decltype(&cgltf_free)> data(parsed, cgltf_free);
  validateGlb(*data);

  const auto& mesh = data->meshes[0];
  std::vector<GlbPage> pages(mesh.primitives_count);
  for(size_t p=1; p<mesh.primitives_count; ++p)
    pages[p] = decodePage(mesh.primitives[p], p==1);
  return pages;
  }

#include "gltfmesh.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// Keep upstream C implementation warnings out of the engine's -Wconversion.
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#endif
#define CGLTF_IMPLEMENTATION
#include "../lib/cgltf/cgltf.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace {

constexpr size_t mrmLimit = 65535;

[[noreturn]] void reject(const std::string& reason) {
  throw std::runtime_error("glTF mesh: " + reason);
  }

const char* resultName(cgltf_result result) {
  switch(result) {
    case cgltf_result_success:         return "success";
    case cgltf_result_data_too_short:  return "data too short";
    case cgltf_result_unknown_format:  return "unknown format";
    case cgltf_result_invalid_json:    return "invalid JSON";
    case cgltf_result_invalid_gltf:    return "invalid glTF";
    case cgltf_result_invalid_options: return "invalid parser options";
    case cgltf_result_file_not_found:  return "file not found";
    case cgltf_result_io_error:        return "I/O error";
    case cgltf_result_out_of_memory:   return "out of memory";
    case cgltf_result_legacy_gltf:     return "legacy glTF";
    default:                          return "parser error";
    }
  }

uint32_t readU32(const std::vector<uint8_t>& bytes, size_t offset) {
  return uint32_t(bytes[offset]) | (uint32_t(bytes[offset+1]) << 8) |
         (uint32_t(bytes[offset+2]) << 16) | (uint32_t(bytes[offset+3]) << 24);
  }

std::vector<uint8_t> readGlb(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if(!file)
    reject("cannot open " + path.string());
  const auto size = file.tellg();
  if(size < 0 || uint64_t(size) > std::numeric_limits<uint32_t>::max())
    reject("invalid GLB file size");
  std::vector<uint8_t> bytes(static_cast<size_t>(size));
  file.seekg(0);
  if(!bytes.empty() && !file.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size())))
    reject("cannot read " + path.string());

  if(bytes.size()<28 || readU32(bytes,0)!=0x46546C67 || readU32(bytes,4)!=2)
    reject("expected a binary glTF 2.0 GLB");
  if(readU32(bytes,8)!=bytes.size())
    reject("GLB header length does not match file size");
  size_t offset = 12;
  for(uint32_t chunk : {0x4E4F534Au, 0x004E4942u}) {
    if(bytes.size()-offset<8)
      reject("GLB must contain JSON and one embedded BIN chunk");
    const size_t length = readU32(bytes,offset);
    if(length%4!=0 || readU32(bytes,offset+4)!=chunk || length>bytes.size()-offset-8)
      reject("invalid GLB chunk type, length or alignment");
    offset += 8+length;
    }
  if(offset!=bytes.size())
    reject("GLB must contain exactly one JSON and one BIN chunk");
  return bytes;
  }

std::string decodedString(std::string value) {
  value.resize(cgltf_decode_string(value.data()));
  if(value.find('\0')!=std::string::npos)
    reject("NUL in a glTF string");
  return value;
  }

zenkit::MaterialGroup materialGroup(const cgltf_material& material) {
  if(material.extras.data==nullptr)
    return zenkit::MaterialGroup::UNDEFINED;

  // cgltf already vendors this JSON tokenizer. Reuse it for the one extras
  // field instead of adding a JSON dependency to the standalone converter.
  const std::string_view json(material.extras.data);
  std::vector<jsmntok_t> tokens(json.size()+1);
  jsmn_parser parser;
  jsmn_init(&parser);
  const int count = jsmn_parse(&parser,json.data(),json.size(),tokens.data(),tokens.size());
  if(count<=0 || tokens[0].type!=JSMN_OBJECT)
    return zenkit::MaterialGroup::UNDEFINED;
  for(size_t i=1; i<size_t(count); ) {
    const auto& key = tokens[i++];
    if(i>=size_t(count))
      reject("invalid material extras");
    const auto& value = tokens[i];
    const auto keyText = decodedString(std::string(json.substr(size_t(key.start),size_t(key.end-key.start))));
    if(keyText=="group") {
      if(value.type!=JSMN_STRING)
        reject("material extras.group must be a string");
      const auto group = decodedString(std::string(json.substr(size_t(value.start),size_t(value.end-value.start))));
      if(group=="UNDEF") return zenkit::MaterialGroup::UNDEFINED;
      if(group=="METAL") return zenkit::MaterialGroup::METAL;
      if(group=="STONE") return zenkit::MaterialGroup::STONE;
      if(group=="WOOD")  return zenkit::MaterialGroup::WOOD;
      if(group=="EARTH") return zenkit::MaterialGroup::EARTH;
      if(group=="WATER")
        reject("material extras.group WATER is unsupported for static visuals: water belongs to world mesh surfaces and overrides OPAQUE/MASK");
      if(group=="SNOW")  return zenkit::MaterialGroup::SNOW;
      reject("unknown material extras.group: " + group);
      }
    const auto end = value.end;
    do { ++i; } while(i<size_t(count) && tokens[i].start<end);
    }
  return zenkit::MaterialGroup::UNDEFINED;
  }

zenkit::Material convertMaterial(const cgltf_material* source) {
  if(source==nullptr || source->name==nullptr || source->name[0]=='\0')
    reject("each primitive needs a named material");
  if(source->alpha_mode==cgltf_alpha_mode_blend)
    reject("BLEND materials are not supported");
  if(source->alpha_mode!=cgltf_alpha_mode_opaque && source->alpha_mode!=cgltf_alpha_mode_mask)
    reject("unsupported material alphaMode");
  zenkit::Material material;
  material.name = decodedString(source->name);
  if(material.name.empty())
    reject("each primitive needs a named material");
  material.texture = material.name;
  if(!std::filesystem::path(material.texture).has_extension())
    material.texture += ".TGA";
  material.group = materialGroup(*source);
  material.color = {255,255,255,255};
  material.alpha_func = source->alpha_mode==cgltf_alpha_mode_mask ?
                       zenkit::AlphaFunction::DEFAULT : zenkit::AlphaFunction::NONE;
  return material;
  }

void validateSubset(cgltf_data& data) {
  if(data.file_type!=cgltf_file_type_glb || data.asset.version==nullptr || std::strcmp(data.asset.version,"2.0")!=0)
    reject("expected glTF asset version 2.0");
  if(data.asset.min_version!=nullptr && std::strcmp(data.asset.min_version,"2.0")!=0)
    reject("unsupported glTF minimum version");
  if(data.extensions_required_count!=0)
    reject("extensionsRequired is not supported");
  if(data.buffers_count!=1 || data.buffers[0].uri!=nullptr || data.bin==nullptr)
    reject("expected one embedded buffer without an external URI");
  if(data.buffers[0].size>data.bin_size || data.bin_size-data.buffers[0].size>3)
    reject("embedded buffer length does not match BIN chunk");
  for(size_t i=0; i<data.images_count; ++i)
    if(data.images[i].uri!=nullptr) {
      const auto uri = decodedString(data.images[i].uri);
      if(uri.compare(0,5,"data:")!=0)
        reject("external image URI is not supported");
      }
  if(data.skins_count!=0)
    reject("skins are not supported");
  for(size_t i=0; i<data.nodes_count; ++i) {
    if(data.nodes[i].skin!=nullptr)
      reject("skins are not supported");
    if(data.nodes[i].weights_count!=0)
      reject("morph targets are not supported");
    if(data.nodes[i].has_mesh_gpu_instancing)
      reject("GPU mesh instancing is not supported");
    }
  for(size_t i=0; i<data.meshes_count; ++i) {
    const auto& mesh = data.meshes[i];
    if(mesh.weights_count!=0)
      reject("morph targets are not supported");
    for(size_t p=0; p<mesh.primitives_count; ++p) {
      if(mesh.primitives[p].targets_count!=0)
        reject("morph targets are not supported");
      if(mesh.primitives[p].has_draco_mesh_compression)
        reject("compressed primitives are not supported");
      }
    }
  for(size_t i=0; i<data.buffer_views_count; ++i) {
    const auto& view = data.buffer_views[i];
    if(view.has_meshopt_compression)
      reject("compressed buffer views are not supported");
    if(view.buffer!=&data.buffers[0] || view.offset>data.buffers[0].size || view.size>data.buffers[0].size-view.offset)
      reject("buffer view is outside the embedded buffer");
    }
  for(size_t i=0; i<data.accessors_count; ++i) {
    const auto& accessor = data.accessors[i];
    if(accessor.is_sparse)
      reject("sparse accessors are not supported");
    if(accessor.count==0 || accessor.type==cgltf_type_invalid || accessor.component_type==cgltf_component_type_invalid)
      reject("invalid accessor count or type");
    const size_t elementSize = cgltf_calc_size(accessor.type,accessor.component_type);
    if(elementSize==0 || accessor.stride<elementSize)
      reject("accessor stride is smaller than its element");
    if(accessor.buffer_view!=nullptr) {
      const auto size = accessor.buffer_view->size;
      if(accessor.offset>size || elementSize>size-accessor.offset ||
         accessor.count-1>(size-accessor.offset-elementSize)/accessor.stride)
        reject("accessor is outside its buffer view");
      }
    }
  // Attach only the already validated BIN bytes. Never call cgltf's URI loader.
  data.buffers[0].data = const_cast<void*>(data.bin);
  const auto result = cgltf_validate(&data);
  if(result!=cgltf_result_success)
    reject(std::string("validation failed: ")+resultName(result));
  if(data.scene==nullptr)
    reject("a default scene is required");
  }

struct Transform {
  std::array<float,16> matrix;
  std::array<double,9> cofactor;
  double determinant;

  explicit Transform(const cgltf_node& node) {
    cgltf_node_transform_world(&node,matrix.data());
    for(const auto value:matrix)
      if(!std::isfinite(value))
        reject("non-finite node transform");
    if(matrix[3]!=0 || matrix[7]!=0 || matrix[11]!=0 || matrix[15]!=1)
      reject("node transform must be affine");
    const double a=matrix[0], b=matrix[4], c=matrix[8];
    const double d=matrix[1], e=matrix[5], f=matrix[9];
    const double g=matrix[2], h=matrix[6], k=matrix[10];
    cofactor = {e*k-f*h, f*g-d*k, d*h-e*g,
                c*h-b*k, a*k-c*g, b*g-a*h,
                b*f-c*e, c*d-a*f, a*e-b*d};
    determinant = a*cofactor[0]+b*cofactor[1]+c*cofactor[2];
    if(!std::isfinite(determinant) || determinant==0)
      reject("singular node transform");
    }

  zenkit::Vec3 position(const float* value) const {
    zenkit::Vec3 result;
    for(unsigned axis=0; axis<3; ++axis) {
      const double v = double(matrix[axis])*value[0] + double(matrix[axis+4])*value[1] +
                       double(matrix[axis+8])*value[2] + matrix[axis+12];
      result[axis] = float(v * (axis==0 ? -100.0 : 100.0));
      if(!std::isfinite(result[axis]))
        reject("non-finite transformed POSITION");
      }
    return result;
    }

  zenkit::Vec3 normal(const float* value) const {
    double transformed[3];
    for(unsigned axis=0; axis<3; ++axis)
      transformed[axis] = (cofactor[axis*3]*value[0] + cofactor[axis*3+1]*value[1] +
                           cofactor[axis*3+2]*value[2])/determinant;
    const double length = std::hypot(transformed[0],transformed[1],transformed[2]);
    if(!std::isfinite(length) || length==0)
      reject("NORMAL must be finite and non-zero");
    return {float(-transformed[0]/length),float(transformed[1]/length),float(transformed[2]/length)};
    }
  };

const cgltf_accessor* attribute(const cgltf_primitive& primitive, cgltf_attribute_type type, int index=0) {
  const cgltf_accessor* result = nullptr;
  for(size_t i=0; i<primitive.attributes_count; ++i) {
    const auto& value = primitive.attributes[i];
    if(value.type==type && value.index==index) {
      if(result!=nullptr)
        reject("duplicate vertex attribute");
      result = value.data;
      }
    }
  return result;
  }

void requireVector(const cgltf_accessor* accessor, cgltf_type type, const char* name) {
  if(accessor==nullptr)
    reject(std::string("missing ")+name);
  if(accessor->type!=type || accessor->component_type!=cgltf_component_type_r_32f || accessor->normalized)
    reject(std::string(name)+" must use non-normalized float vectors");
  }

struct PositionStream {
  const cgltf_accessor* source;
  size_t base;
  };

struct WedgeStream {
  const cgltf_node* node;
  const cgltf_accessor* position;
  const cgltf_accessor* normal;
  const cgltf_accessor* uv;
  size_t base;
  };

struct MaterialState {
  const cgltf_material* source;
  std::vector<WedgeStream> streams;
  };

class Converter {
  public:
    zenkit::MultiResolutionMesh mesh;

    Converter() { mesh.alpha_test = false; }

    void addNode(const cgltf_node& node) {
      if(node.mesh==nullptr)
        return;
      const Transform transform(node);
      std::vector<PositionStream> positions;
      for(size_t p=0; p<node.mesh->primitives_count; ++p)
        addPrimitive(node,node.mesh->primitives[p],transform,positions);
      }

    void finish() {
      if(mesh.positions.empty())
        reject("default scene contains no mesh triangles");
      mesh.bbox.min = mesh.positions.front();
      mesh.bbox.max = mesh.positions.front();
      for(const auto& position:mesh.positions)
        for(unsigned axis=0; axis<3; ++axis) {
          mesh.bbox.min[axis] = std::min(mesh.bbox.min[axis],position[axis]);
          mesh.bbox.max[axis] = std::max(mesh.bbox.max[axis],position[axis]);
          }
      for(unsigned axis=0; axis<3; ++axis) {
        mesh.obbox.center[axis] = float((double(mesh.bbox.min[axis])+mesh.bbox.max[axis])*0.5);
        mesh.obbox.half_width[axis] = float((double(mesh.bbox.max[axis])-mesh.bbox.min[axis])*0.5);
        }
      mesh.obbox.axes[0] = {1,0,0};
      mesh.obbox.axes[1] = {0,1,0};
      mesh.obbox.axes[2] = {0,0,1};
      }

  private:
    std::vector<MaterialState> materials;

    size_t materialIndex(const cgltf_material* source) {
      for(size_t i=0; i<materials.size(); ++i)
        if(materials[i].source==source)
          return i;
      auto material = convertMaterial(source);
      if(source->alpha_mode==cgltf_alpha_mode_mask)
        mesh.alpha_test = true;
      mesh.materials.push_back(material);
      mesh.sub_meshes.emplace_back();
      mesh.sub_meshes.back().mat = std::move(material);
      materials.push_back({source,{}});
      return materials.size()-1;
      }

    size_t positionBase(const cgltf_accessor& source, const Transform& transform, std::vector<PositionStream>& streams) {
      for(const auto& stream:streams)
        if(stream.source==&source)
          return stream.base;
      if(source.count>mrmLimit-mesh.positions.size())
        reject("more than 65535 MRM positions");
      const size_t base = mesh.positions.size();
      for(size_t i=0; i<source.count; ++i) {
        float value[3];
        if(!cgltf_accessor_read_float(&source,i,value,3))
          reject("cannot read POSITION");
        mesh.positions.push_back(transform.position(value));
        }
      streams.push_back({&source,base});
      return base;
      }

    void addPrimitive(const cgltf_node& node, const cgltf_primitive& primitive, const Transform& transform,
                      std::vector<PositionStream>& positions) {
      if(primitive.type!=cgltf_primitive_type_triangles)
        reject("only triangle primitives are supported");
      const auto* position = attribute(primitive,cgltf_attribute_type_position);
      const auto* normal = attribute(primitive,cgltf_attribute_type_normal);
      const auto* uv = attribute(primitive,cgltf_attribute_type_texcoord);
      requireVector(position,cgltf_type_vec3,"POSITION");
      requireVector(normal,cgltf_type_vec3,"NORMAL");
      if(normal->count!=position->count || (uv!=nullptr && uv->count!=position->count))
        reject("vertex attribute counts do not match");
      if(uv!=nullptr && (uv->type!=cgltf_type_vec2 ||
         !((uv->component_type==cgltf_component_type_r_32f && !uv->normalized) ||
           ((uv->component_type==cgltf_component_type_r_8u || uv->component_type==cgltf_component_type_r_16u) && uv->normalized))))
        reject("TEXCOORD_0 must use float or normalized unsigned vectors");
      const size_t count = primitive.indices==nullptr ? position->count : primitive.indices->count;
      if(count==0 || count%3!=0)
        reject("triangle index count must be a non-zero multiple of three");
      if(primitive.indices!=nullptr && primitive.indices->normalized)
        reject("triangle indices must not be normalized");

      const size_t material = materialIndex(primitive.material);
      auto& submesh = mesh.sub_meshes[material];
      auto& state = materials[material];
      const size_t posBase = positionBase(*position,transform,positions);
      size_t wedgeBase = submesh.wedges.size();
      bool existing = false;
      for(const auto& stream:state.streams)
        if(stream.node==&node && stream.position==position && stream.normal==normal && stream.uv==uv) {
          wedgeBase = stream.base;
          existing = true;
          break;
          }
      if(!existing) {
        if(position->count>mrmLimit-submesh.wedges.size())
          reject("more than 65535 MRM wedges for material " + submesh.mat.name);
        for(size_t i=0; i<position->count; ++i) {
          float n[3], tex[2] = {};
          if(!cgltf_accessor_read_float(normal,i,n,3) || (uv!=nullptr && !cgltf_accessor_read_float(uv,i,tex,2)))
            reject("cannot read NORMAL or TEXCOORD_0");
          if(!std::isfinite(tex[0]) || !std::isfinite(tex[1]))
            reject("non-finite TEXCOORD_0");
          submesh.wedges.push_back({transform.normal(n),{tex[0],tex[1]},uint16_t(posBase+i)});
          }
        state.streams.push_back({&node,position,normal,uv,wedgeBase});
        }
      for(size_t i=0; i<count; i+=3) {
        zenkit::MeshTriangle triangle;
        for(unsigned corner=0; corner<3; ++corner) {
          const size_t index = primitive.indices==nullptr ? i+corner : cgltf_accessor_read_index(primitive.indices,i+corner);
          if(index>=position->count)
            reject("triangle index is outside POSITION");
          triangle.wedges[corner] = uint16_t(wedgeBase+index);
          }
        // Original NW_HARBOUR_BARREL_01: 180/180 wedge normals oppose the
        // MRM face cross product. Mirroring X already changes glTF's CCW
        // front into that convention; only a mirrored WORLD node needs a swap.
        if(transform.determinant<0)
          std::swap(triangle.wedges[1],triangle.wedges[2]);
        submesh.triangles.push_back(triangle);
        }
      }
  };

std::vector<GltfMeshNode> loadScene(const std::filesystem::path& path, std::vector<uint8_t>* source, bool splitNodes) {
  auto bytes = readGlb(path);
  cgltf_options options = {};
  options.type = cgltf_file_type_glb;
  cgltf_data* parsed = nullptr;
  const auto result = cgltf_parse(&options,bytes.data(),bytes.size(),&parsed);
  if(result!=cgltf_result_success)
    reject(std::string("parse failed: ")+resultName(result));
  const std::unique_ptr<cgltf_data,decltype(&cgltf_free)> data(parsed,cgltf_free);
  validateSubset(*data);
  Converter converter;
  std::vector<GltfMeshNode> meshes;
  std::vector<const cgltf_node*> pending;
  std::vector<bool> visited(data->nodes_count,false);
  for(size_t i=0; i<data->scene->nodes_count; ++i)
    pending.push_back(data->scene->nodes[i]);
  while(!pending.empty()) {
    const auto* node = pending.back();
    pending.pop_back();
    const size_t index = size_t(node-data->nodes);
    if(visited[index])
      reject("a scene node is referenced more than once");
    visited[index] = true;
    if(splitNodes && node->mesh!=nullptr) {
      Converter part;
      part.addNode(*node);
      part.finish();
      meshes.push_back({node->name==nullptr ? "" : decodedString(node->name),std::move(part.mesh)});
      }
    else if(!splitNodes)
      converter.addNode(*node);
    for(size_t i=0; i<node->children_count; ++i)
      pending.push_back(node->children[i]);
    }
  if(!splitNodes) {
    converter.finish();
    meshes.push_back({{},std::move(converter.mesh)});
    }
  if(meshes.empty())
    reject("default scene contains no mesh triangles");
  if(source!=nullptr)
    *source = std::move(bytes);
  return meshes;
  }

}

zenkit::MultiResolutionMesh loadGltfMesh(const std::filesystem::path& path, std::vector<uint8_t>* source) {
  auto meshes = loadScene(path,source,false);
  return std::move(meshes.front().mesh);
  }

std::vector<GltfMeshNode> loadGltfMeshNodes(const std::filesystem::path& path, std::vector<uint8_t>* source) {
  return loadScene(path,source,true);
  }

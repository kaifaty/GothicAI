#include "asset/fromzenkit.h"
#include "audiodata.h"
#include "baseimport.h"
#include "basemesh.h"
#include "commandline.h"
#include "contentcensus.h"
#include "fontdata.h"
#include "game/definitions/definitiontypes.h"
#include "gothic.h"
#include "gltfmesh.h"
#include "gothicglb.h"
#include "graphics/mesh/submesh/packedmesh.h"
#include "physics/physicvbo.h"
#include "resources.h"
#include "sha256.h"
#include "modeldata.h"
#include "musicdata.h"
#include "musicnative.h"
#include "dmusic/directmusic.h"
#include "dmusic/sourcefile.h"
#include "programdata.h"
#include "outputunits.h"
#include "utils/gthfont.h"
#include "utils/fileutil.h"

#include <zenkit/Vfs.hh>
#include <zenkit/DaedalusVm.hh>
#include <zenkit/CutsceneLibrary.hh>
#include <zenkit/Font.hh>
#include <zenkit/Stream.hh>
#include <zenkit/Model.hh>
#include <zenkit/ModelAnimation.hh>
#include <zenkit/ModelScript.hh>
#include <zenkit/MorphMesh.hh>
#include <zenkit/MultiResolutionMesh.hh>
#include <zenkit/Model.hh>
#include <zenkit/ModelScript.hh>
#include <zenkit/Archive.hh>
#include <zenkit/Logger.hh>
#include <zenkit/Texture.hh>
#include <zenkit/addon/texcvt.hh>
#include <zenkit/addon/daedalus.hh>
#include <Tempest/Dir>
#include <Tempest/MemReader>
#include <Tempest/File>
#include <Tempest/Sound>
#include <Tempest/Texture2d>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <exception>
#include <functional>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <typeinfo>

namespace fs = std::filesystem;

struct BaseContentContract {
  struct SourceObservation {
    const zenkit::VfsNode* node = nullptr;
    std::string source;
    };

  static std::string& observedSource() {
    static std::string source;
    return source;
    }

  static zenkit::Vfs& sourceVfs() {
    static zenkit::Vfs vfs;
    static const bool mounted=[] {
      vfs.set_open_observer([](std::string_view name,std::string_view source) {
        observedSource()=source;
        if(Resources::inst && Resources::inst->census)
          Resources::inst->census->opened(name,source);
        });
      fs::path work;
      BaseImport::mountRuntime(vfs,fs::path(CommandLine::inst().rootPath()),work);
      return true;
      }();
    (void)mounted;
    return vfs;
    }

  static std::unique_ptr<zenkit::Read> readSource(std::string_view name,SourceObservation* observation=nullptr) {
    const auto* node=sourceVfs().find(name);
    require(node!=nullptr,"independent source missing: "+std::string(name));
    auto reader=node->open_read();
    if(observation) *observation={node,observedSource()};
    return reader;
    }
  static void require(bool condition, const std::string& message) {
    if(!condition)
      throw std::runtime_error(message);
    }

  template<class A, class B>
  static void equal(A a, B b, const std::string& field) {
    require(a==b,field+": "+std::to_string(a)+" vs "+std::to_string(b));
    }

  static void bytes(const void* a, const void* b, size_t size, const std::string& field) {
    const auto* x = static_cast<const uint8_t*>(a);
    const auto* y = static_cast<const uint8_t*>(b);
    for(size_t i=0;i<size;++i)
      if(x[i]!=y[i])
        equal(x[i],y[i],field+" byte "+std::to_string(i));
    }

  template<class T>
  static void vector(const std::vector<T>& a, const std::vector<T>& b, const std::string& field) {
    equal(a.size(),b.size(),field+" count");
    bytes(a.data(),b.data(),a.size()*sizeof(T),field);
    }

  static void point(const Tempest::Vec3& a, const Tempest::Vec3& b, const std::string& field) {
    equal(std::bit_cast<uint32_t>(a.x),std::bit_cast<uint32_t>(b.x),field+" x bits");
    equal(std::bit_cast<uint32_t>(a.y),std::bit_cast<uint32_t>(b.y),field+" y bits");
    equal(std::bit_cast<uint32_t>(a.z),std::bit_cast<uint32_t>(b.z),field+" z bits");
    }

  template<class A, class B>
  static void modelField(const A& a, const B& b, const std::string& field) {
    if constexpr(std::is_same_v<A,float> && std::is_same_v<B,float>)
      equal(std::bit_cast<uint32_t>(a),std::bit_cast<uint32_t>(b),field+" bits");
    else if constexpr(std::is_enum_v<A> && std::is_enum_v<B>)
      equal(int64_t(a),int64_t(b),field);
    else
      require(a==b,field+": values differ");
    }

  template<class A, class B>
  static void modelField(const std::vector<A>& a, const std::vector<B>& b, const std::string& field) {
    equal(a.size(),b.size(),field+" count");
    if(a.empty()) return;
    if constexpr(std::is_same_v<A,B> && std::is_trivially_copyable_v<A>)
      if(std::memcmp(a.data(),b.data(),a.size()*sizeof(A))==0) return;
    for(size_t i=0;i<a.size();++i)
      modelField(a[i],b[i],field+"["+std::to_string(i)+"]");
    }

  static void modelField(const zenkit::Vec2& a, const zenkit::Vec2& b, const std::string& field) {
    modelField(a.x,b.x,field+".x"); modelField(a.y,b.y,field+".y");
    }

  static void modelField(const zenkit::Vec3& a, const zenkit::Vec3& b, const std::string& field) {
    modelField(a.x,b.x,field+".x"); modelField(a.y,b.y,field+".y"); modelField(a.z,b.z,field+".z");
    }

  static void modelField(const zenkit::Quat& a, const zenkit::Quat& b, const std::string& field) {
    modelField(a.w,b.w,field+".w"); modelField(a.x,b.x,field+".x");
    modelField(a.y,b.y,field+".y"); modelField(a.z,b.z,field+".z");
    }

  static void modelField(const zenkit::Mat4& a, const zenkit::Mat4& b, const std::string& field) {
    for(unsigned column=0;column<4;++column)
      for(unsigned row=0;row<4;++row)
        modelField(a[column][row],b[column][row],field+"["+std::to_string(column)+"]["+std::to_string(row)+"]");
    }

  static void modelField(const zenkit::Color& a, const zenkit::Color& b, const std::string& field) {
    modelField(a.r,b.r,field+".r"); modelField(a.g,b.g,field+".g");
    modelField(a.b,b.b,field+".b"); modelField(a.a,b.a,field+".a");
    }

  static void modelField(const zenkit::AxisAlignedBoundingBox& a, const zenkit::AxisAlignedBoundingBox& b,
                         const std::string& field) {
    modelField(a.min,b.min,field+".min"); modelField(a.max,b.max,field+".max");
    }

  static void modelField(const zenkit::OrientedBoundingBox& a, const zenkit::OrientedBoundingBox& b,
                         const std::string& field) {
    modelField(a.center,b.center,field+".center"); modelField(a.half_width,b.half_width,field+".half_width");
    for(size_t i=0;i<3;++i) modelField(a.axes[i],b.axes[i],field+".axes["+std::to_string(i)+"]");
    modelField(a.children,b.children,field+".children");
    }

#define MODEL_FIELD(member) modelField(a.member,b.member,field+"." #member)
  static void modelField(const zenkit::Date& a, const asset::ModelDate& b, const std::string& field) {
    MODEL_FIELD(year);
    MODEL_FIELD(month);
    MODEL_FIELD(day);
    MODEL_FIELD(hour);
    MODEL_FIELD(minute);
    MODEL_FIELD(second);
    }

  static void modelField(const zenkit::ModelHierarchyNode& a, const asset::ModelHierarchyNode& b, const std::string& field) {
    MODEL_FIELD(parent_index);
    MODEL_FIELD(name);
    MODEL_FIELD(transform);
    }

  static void modelField(const zenkit::ModelHierarchy& a, const asset::ModelHierarchy& b, const std::string& field) {
    MODEL_FIELD(nodes);
    MODEL_FIELD(bbox);
    MODEL_FIELD(collision_bbox);
    MODEL_FIELD(root_translation);
    MODEL_FIELD(checksum);
    MODEL_FIELD(source_date);
    MODEL_FIELD(source_path);
    }

  static void modelField(const zenkit::AnimationSample& a, const asset::AnimationSample& b, const std::string& field) {
    MODEL_FIELD(position);
    MODEL_FIELD(rotation);
    }

  static void modelField(const zenkit::AnimationEvent& a, const asset::AnimationEvent& b, const std::string& field) {
    MODEL_FIELD(type);
    MODEL_FIELD(frame);
    MODEL_FIELD(tag);
    MODEL_FIELD(probability);
    for(size_t i=0;i<4;++i) {
      modelField(a.content[i],b.content[i],field+".content["+std::to_string(i)+"]");
      modelField(a.values[i],b.values[i],field+".values["+std::to_string(i)+"]");
      }
    }

  static void modelField(const zenkit::ModelAnimation& a, const asset::ModelAnimation& b, const std::string& field) {
    MODEL_FIELD(name);
    MODEL_FIELD(next);
    MODEL_FIELD(layer);
    MODEL_FIELD(frame_count);
    MODEL_FIELD(node_count);
    MODEL_FIELD(fps);
    MODEL_FIELD(fps_source);
    MODEL_FIELD(sample_position_min);
    MODEL_FIELD(sample_position_scale);
    MODEL_FIELD(bbox);
    MODEL_FIELD(checksum);
    MODEL_FIELD(source_path);
    MODEL_FIELD(source_date);
    MODEL_FIELD(source_script);
    MODEL_FIELD(samples);
    MODEL_FIELD(events);
    MODEL_FIELD(node_indices);
    }

  static void modelField(const zenkit::MdsSkeleton& a, const asset::MdsSkeleton& b, const std::string& field) {
    MODEL_FIELD(name);
    MODEL_FIELD(disable_mesh);
    }

  static void modelField(const zenkit::MdsModelTag& a, const asset::MdsModelTag& b, const std::string& field) {
    MODEL_FIELD(bone);
    }

  static void modelField(const zenkit::MdsEventTag& a, const asset::MdsEventTag& b, const std::string& field) {
    MODEL_FIELD(frame);
    MODEL_FIELD(type);
    MODEL_FIELD(slot);
    MODEL_FIELD(slot2);
    MODEL_FIELD(item);
    MODEL_FIELD(frames);
    MODEL_FIELD(fight_mode);
    MODEL_FIELD(attached);
    }

  static void modelField(const zenkit::MdsParticleEffect& a, const asset::MdsParticleEffect& b, const std::string& field) {
    MODEL_FIELD(frame);
    MODEL_FIELD(index);
    MODEL_FIELD(name);
    MODEL_FIELD(position);
    MODEL_FIELD(attached);
    }

  static void modelField(const zenkit::MdsCameraTremor& a, const asset::MdsCameraTremor& b, const std::string& field) {
    MODEL_FIELD(frame);
    MODEL_FIELD(field1);
    MODEL_FIELD(field2);
    MODEL_FIELD(field3);
    MODEL_FIELD(field4);
    }

  static void modelField(const zenkit::MdsParticleEffectStop& a, const asset::MdsParticleEffectStop& b, const std::string& field) {
    MODEL_FIELD(frame);
    MODEL_FIELD(index);
    }

  static void modelField(const zenkit::MdsSoundEffect& a, const asset::MdsSoundEffect& b, const std::string& field) {
    MODEL_FIELD(frame);
    MODEL_FIELD(name);
    MODEL_FIELD(range);
    MODEL_FIELD(empty_slot);
    }

  static void modelField(const zenkit::MdsSoundEffectGround& a, const asset::MdsSoundEffectGround& b, const std::string& field) {
    MODEL_FIELD(frame);
    MODEL_FIELD(name);
    MODEL_FIELD(range);
    MODEL_FIELD(empty_slot);
    }

  static void modelField(const zenkit::MdsMorphAnimation& a, const asset::MdsMorphAnimation& b, const std::string& field) {
    MODEL_FIELD(frame);
    MODEL_FIELD(animation);
    MODEL_FIELD(node);
    }

  static void modelField(const zenkit::MdsAnimation& a, const asset::MdsAnimation& b, const std::string& field) {
    MODEL_FIELD(name);
    MODEL_FIELD(layer);
    MODEL_FIELD(next);
    MODEL_FIELD(blend_in);
    MODEL_FIELD(blend_out);
    MODEL_FIELD(flags);
    MODEL_FIELD(model);
    MODEL_FIELD(direction);
    MODEL_FIELD(first_frame);
    MODEL_FIELD(last_frame);
    MODEL_FIELD(fps);
    MODEL_FIELD(speed);
    MODEL_FIELD(collision_volume_scale);
    MODEL_FIELD(events);
    MODEL_FIELD(pfx);
    MODEL_FIELD(pfx_stop);
    MODEL_FIELD(sfx);
    MODEL_FIELD(sfx_ground);
    MODEL_FIELD(morph);
    MODEL_FIELD(tremors);
    }

  static void modelField(const zenkit::MdsAnimationAlias& a, const asset::MdsAnimationAlias& b, const std::string& field) {
    MODEL_FIELD(name);
    MODEL_FIELD(layer);
    MODEL_FIELD(next);
    MODEL_FIELD(blend_in);
    MODEL_FIELD(blend_out);
    MODEL_FIELD(flags);
    MODEL_FIELD(alias);
    MODEL_FIELD(direction);
    }

  static void modelField(const zenkit::MdsAnimationBlend& a, const asset::MdsAnimationBlend& b, const std::string& field) {
    MODEL_FIELD(name);
    MODEL_FIELD(next);
    MODEL_FIELD(blend_in);
    MODEL_FIELD(blend_out);
    }

  static void modelField(const zenkit::MdsAnimationCombine& a, const asset::MdsAnimationCombine& b, const std::string& field) {
    MODEL_FIELD(name);
    MODEL_FIELD(layer);
    MODEL_FIELD(next);
    MODEL_FIELD(blend_in);
    MODEL_FIELD(blend_out);
    MODEL_FIELD(flags);
    MODEL_FIELD(model);
    MODEL_FIELD(last_frame);
    }

  static void modelField(const zenkit::ModelScript& a, const asset::ModelScript& b, const std::string& field) {
    MODEL_FIELD(skeleton);
    MODEL_FIELD(meshes);
    MODEL_FIELD(disabled_animations);
    MODEL_FIELD(combinations);
    MODEL_FIELD(blends);
    MODEL_FIELD(aliases);
    MODEL_FIELD(model_tags);
    MODEL_FIELD(animations);
    }

  static void modelField(const zenkit::MorphAnimation& a, const asset::MorphAnimation& b, const std::string& field) {
    MODEL_FIELD(name);
    MODEL_FIELD(layer);
    MODEL_FIELD(blend_in);
    MODEL_FIELD(blend_out);
    MODEL_FIELD(duration);
    MODEL_FIELD(speed);
    MODEL_FIELD(flags);
    MODEL_FIELD(frame_count);
    MODEL_FIELD(vertices);
    MODEL_FIELD(samples);
    }

  static void modelField(const zenkit::MorphSource& a, const asset::MorphSource& b, const std::string& field) {
    MODEL_FIELD(file_date);
    MODEL_FIELD(file_name);
    }

  static void modelField(const zenkit::MorphMesh& a, const asset::MorphMesh& b, const std::string& field) {
    MODEL_FIELD(name);
    MODEL_FIELD(mesh);
    MODEL_FIELD(morph_positions);
    MODEL_FIELD(animations);
    MODEL_FIELD(sources);
    }

  static void modelField(const zenkit::Material& a, const asset::Material& b, const std::string& field) {
    MODEL_FIELD(name);
    MODEL_FIELD(group);
    MODEL_FIELD(color);
    MODEL_FIELD(smooth_angle);
    MODEL_FIELD(texture);
    MODEL_FIELD(texture_scale);
    MODEL_FIELD(texture_anim_fps);
    MODEL_FIELD(texture_anim_map_mode);
    MODEL_FIELD(texture_anim_map_dir);
    MODEL_FIELD(disable_collision);
    MODEL_FIELD(disable_lightmap);
    MODEL_FIELD(dont_collapse);
    MODEL_FIELD(detail_object);
    MODEL_FIELD(detail_object_scale);
    MODEL_FIELD(force_occluder);
    MODEL_FIELD(environment_mapping);
    MODEL_FIELD(environment_mapping_strength);
    MODEL_FIELD(wave_mode);
    MODEL_FIELD(wave_speed);
    MODEL_FIELD(wave_max_amplitude);
    MODEL_FIELD(wave_grid_size);
    MODEL_FIELD(ignore_sun);
    MODEL_FIELD(alpha_func);
    MODEL_FIELD(default_mapping);
    }

  static void modelField(const zenkit::MeshTriangle& a, const asset::MeshTriangle& b, const std::string& field) {
    for(size_t i=0;i<3;++i) modelField(a.wedges[i],b.wedges[i],field+".wedges["+std::to_string(i)+"]");
    }

  static void modelField(const zenkit::MeshWedge& a, const asset::MeshWedge& b, const std::string& field) {
    MODEL_FIELD(normal);
    MODEL_FIELD(texture);
    MODEL_FIELD(index);
    }

  static void modelField(const zenkit::SubMesh& a, const asset::SubMesh& b, const std::string& field) {
    MODEL_FIELD(mat);
    MODEL_FIELD(triangles);
    MODEL_FIELD(wedges);
    }

  static void modelField(const zenkit::MultiResolutionMesh& a, const asset::StaticMesh& b, const std::string& field) {
    MODEL_FIELD(positions);
    MODEL_FIELD(normals);
    MODEL_FIELD(sub_meshes);
    MODEL_FIELD(materials);
    MODEL_FIELD(alpha_test);
    MODEL_FIELD(bbox);
    MODEL_FIELD(obbox);
    }

  static void modelField(const zenkit::SoftSkinWeightEntry& a, const asset::SkinWeight& b, const std::string& field) {
    MODEL_FIELD(weight);
    MODEL_FIELD(position);
    MODEL_FIELD(node_index);
    }

  static void modelField(const zenkit::SoftSkinMesh& a, const asset::SoftSkinMesh& b, const std::string& field) {
    MODEL_FIELD(mesh);
    MODEL_FIELD(bboxes);
    MODEL_FIELD(weights);
    }

  static void modelField(const zenkit::ModelMesh& a, const asset::ModelMesh& b, const std::string& field) {
    MODEL_FIELD(meshes);
    equal(a.attachments.size(),b.attachments.size(),field+".attachments count");
    size_t i=0;
    for(const auto& [name,mesh]:a.attachments) {
      const auto where=field+".attachments["+std::to_string(i)+"]";
      require(name==b.attachments[i].first,where+": native source attachment order differs");
      modelField(mesh,b.attachments[i].second,where+"."+name);
      ++i;
      }
    }

  static void modelField(const zenkit::Model& a, const asset::Model& b, const std::string& field) {
    MODEL_FIELD(hierarchy);
    MODEL_FIELD(mesh);
    }

#undef MODEL_FIELD

  static void modelField(const std::vector<zenkit::AnimationSample>& a,
                         const std::vector<asset::AnimationSample>& b, const std::string& field) {
    equal(a.size(),b.size(),field+" count");
    static_assert(sizeof(zenkit::AnimationSample)==sizeof(asset::AnimationSample));
    if(!a.empty() && std::memcmp(a.data(),b.data(),a.size()*sizeof(a[0]))!=0)
      for(size_t i=0;i<a.size();++i) modelField(a[i],b[i],field+"["+std::to_string(i)+"]");
    }

  static void texture(const Resources::TextureData& a, const Resources::TextureData& b, const std::string& name) {
    equal(int(a.pixmap.format()),int(b.pixmap.format()),name+" format");
    equal(a.pixmap.w(),b.pixmap.w(),name+" width");
    equal(a.pixmap.h(),b.pixmap.h(),name+" height");
    equal(a.pixmap.mipCount(),b.pixmap.mipCount(),name+" mips");
    equal(a.useMipmap,b.useMipmap,name+" GPU mip flag");
    equal(a.pixmap.dataSize(),b.pixmap.dataSize(),name+" data size");
    bytes(a.pixmap.data(),b.pixmap.data(),a.pixmap.dataSize(),name);
    }

  static void packed(PackedMesh& a, PackedMesh& b, const std::string& name) {
    static_assert(sizeof(Resources::Vertex)==9*sizeof(uint32_t));
    vector(a.vertices,b.vertices,name+" vertices");
    vector(a.indices,b.indices,name+" indices");
    vector(a.indices8,b.indices8,name+" meshlet byte indices");
    const auto meshlets=a.indices.size()/PackedMesh::MaxInd;
    equal(a.vertices.size(),meshlets*PackedMesh::MaxVert,name+" meshlet vertex slots");
    equal(a.indices.size(),meshlets*PackedMesh::MaxInd,name+" meshlet index slots");
    equal(a.indices8.size(),meshlets*PackedMesh::MaxPrim*4,name+" meshlet byte slots");
    equal(a.isUsingAlphaTest,b.isUsingAlphaTest,name+" alpha test");
    equal(a.subMeshes.size(),b.subMeshes.size(),name+" submeshes");
    for(size_t i=0;i<a.subMeshes.size();++i) {
      const auto& x=a.subMeshes[i];
      const auto& y=b.subMeshes[i];
      const auto where=name+" submesh "+std::to_string(i);
      require(gothicglb::sameMaterial(x.material,y.material),where+" material differs");
      equal(x.iboOffset,y.iboOffset,where+" offset");
      equal(x.iboLength,y.iboLength,where+" length");
      equal(x.alphaTest,y.alphaTest,where+" alpha test");
      equal(x.alphaTestRequired,y.alphaTestRequired,where+" alpha test required");
      }
    equal(a.meshletBounds.size(),b.meshletBounds.size(),name+" meshlets");
    for(size_t i=0;i<a.meshletBounds.size();++i) {
      const auto where=name+" meshlet "+std::to_string(i);
      point(a.meshletBounds[i].pos,b.meshletBounds[i].pos,where+" center");
      equal(std::bit_cast<uint32_t>(a.meshletBounds[i].r),std::bit_cast<uint32_t>(b.meshletBounds[i].r),where+" radius bits");
      }
    point(a.bbox().first,b.bbox().first,name+" bbox min");
    point(a.bbox().second,b.bbox().second,name+" bbox max");
    // Static objects feed this same PK_Visual packing into PhysicMeshShape.
    PhysicVbo x(std::move(a)),y(std::move(b));
    equal(x.vert.size(),y.vert.size(),name+" physics vertices");
    for(size_t i=0;i<x.vert.size();++i)
      for(JPH::uint axis=0;axis<3;++axis)
        equal(std::bit_cast<uint32_t>(x.vert[i][axis]),std::bit_cast<uint32_t>(y.vert[i][axis]),
              name+" physics vertex "+std::to_string(i)+" axis "+std::to_string(axis)+" bits");
    vector(x.id,y.id,name+" physics indices");
    equal(x.segments.size(),y.segments.size(),name+" physics segments");
    for(size_t i=0;i<x.segments.size();++i) {
      const auto where=name+" physics segment "+std::to_string(i);
      equal(x.segments[i].off,y.segments[i].off,where+" offset");
      equal(x.segments[i].size,y.segments[i].size,where+" size");
      equal(int(x.segments[i].mat),int(y.segments[i].mat),where+" material");
      }
    }

  static rapidjson::Document json(const fs::path& file) {
    const auto data=gothicglb::readFile(file);
    rapidjson::Document result;
    result.Parse(reinterpret_cast<const char*>(data.data()),data.size());
    require(!result.HasParseError() && result.IsObject(),"invalid JSON: "+file.string());
    return result;
    }

  static void writeJson(const fs::path& file, const rapidjson::Document& data) {
    rapidjson::StringBuffer text;
    rapidjson::Writer<rapidjson::StringBuffer> writer(text);
    data.Accept(writer);
    gothicglb::writeFile(file,text.GetString(),text.GetSize());
    }

  static void ownMesh(const fs::path& file) {
    gothicglb::GlbBuilder glb;
    glb.accessor(std::vector<float>{1,2,3,4,2,3,1,6,3},5126,"VEC3",3,false,34962,
                 R"(,"min":[1,2,3],"max":[4,6,3])");
    glb.accessor(std::vector<float>{0,0,1,0,0,1,0,0,1},5126,"VEC3",3,false,34962);
    glb.accessor(std::vector<float>{0,0,1,0,0,1},5126,"VEC2",2,false,34962);
    glb.accessor(std::vector<uint16_t>{0,1,2},5123,"SCALAR",1,false,34963);
    const auto data=glb.finish(
      R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],)"
      R"("materials":[{"name":"OWN_CONTRACT","alphaMode":"OPAQUE"}],)"
      R"("meshes":[{"primitives":[{"attributes":{"POSITION":0,"NORMAL":1,"TEXCOORD_0":2},"indices":3,"material":0}]}],)"
      "\"buffers\":[{\"byteLength\":"+std::to_string(glb.bin.size())+"}],\"bufferViews\":"+glb.viewsJson()+
      ",\"accessors\":"+glb.accessorsJson()+"}");
    gothicglb::writeFile(file,data.data(),data.size());
    }

  static void modelNames(const zenkit::VfsNode& directory, std::set<std::string>& names) {
    for(const auto& node:directory.children()) {
      if(node.type()==zenkit::VfsNodeType::DIRECTORY) {
        modelNames(node,names);
        continue;
        }
      auto name=node.name();
      for(auto& c:name) c=char(std::toupper(static_cast<unsigned char>(c)));
      if(name.ends_with(".MDH") || name.ends_with(".MDM") || name.ends_with(".MDL") ||
         name.ends_with(".MAN") || name.ends_with(".MMB") || name.ends_with(".MSB") || name.ends_with(".MDS"))
        names.insert(std::move(name));
      }
    }

  static size_t models(const fs::path& base) {
    const auto index=json(base/"models/models.json");
    require(index.HasMember("format") && index["format"].IsString() &&
            std::string_view(index["format"].GetString())=="gothic-models" && index["version"]==1,
            "model index must use gothic-models version 1");
    require(index.HasMember("files") && index["files"].IsObject(),"model index must contain file records");
    const auto& records=index["files"];
    std::set<std::string> names;
    modelNames(sourceVfs().root(),names);
    equal(names.size(),size_t(records.MemberCount()),"model index must include every mounted model resource");
    std::map<std::string,size_t> counts;
    for(const auto& name:names) {
      require(records.HasMember(name.c_str()),name+": source model missing from base index");
      require(records[name.c_str()].IsString(),name+": model record path must be a string");
      const auto* entry=sourceVfs().find(name);
      require(entry!=nullptr,name+": original model resource missing");
      auto reader=entry->open_read();
      const auto extension=fs::path(name).extension().string();
      if(extension==".MDH") {
        zenkit::ModelHierarchy source{};
        source.load(reader.get());
        asset::ModelHierarchy loaded;
        require(Resources::loadHierarchyData(name,loaded),name+": hierarchy must load through Resources");
        modelField(source,loaded,name);
        }
      else if(extension==".MDM") {
        zenkit::ModelMesh source{};
        source.load(reader.get());
        asset::ModelMesh loaded;
        require(Resources::loadModelMeshData(name,loaded),name+": model mesh must load through Resources");
        modelField(source,loaded,name);
        }
      else if(extension==".MDL") {
        zenkit::Model source{};
        source.load(reader.get());
        asset::Model loaded;
        require(Resources::loadModelData(name,loaded),name+": combined model must load through Resources");
        modelField(source,loaded,name);
        }
      else if(extension==".MAN") {
        zenkit::ModelAnimation source{};
        source.load(reader.get());
        asset::ModelAnimation loaded;
        require(Resources::loadAnimationData(name,loaded),name+": animation must load through Resources");
        modelField(source,loaded,name);
        }
      else if(extension==".MMB") {
        zenkit::MorphMesh source{};
        source.load(reader.get());
        modelField(source,Resources::loadMorphData(name),name);
        }
      else {
        zenkit::ModelScript source{};
        source.load(reader.get());
        asset::ModelScript loaded;
        require(Resources::loadModelScriptData(name,loaded),name+": model script must load through Resources");
        modelField(source,loaded,name);
        }
      ++counts[extension];
      }
    std::printf("Models: %zu equal to independently parsed sources",names.size());
    for(const auto& [extension,count]:counts) std::printf("; %s=%zu",extension.c_str(),count);
    std::puts("; selected through Resources; every owned field and native attachment order equal");
    return names.size();
    }

  static void fontNames(const zenkit::VfsNode& directory, std::set<std::string>& names) {
    for(const auto& node:directory.children()) {
      if(node.type()==zenkit::VfsNodeType::DIRECTORY) {
        fontNames(node,names);
        continue;
        }
      auto name=node.name();
      for(auto& c:name) c=char(std::toupper(static_cast<unsigned char>(c)));
      if(name.ends_with(".FNT")) names.insert(std::move(name));
      }
    }

  static void fontLayout(const zenkit::Font& source, const asset::Font& loaded, const std::string& name) {
    // Build the old layout input directly from the independent parser, without fromZenKit.
    asset::Font oracle;
    oracle.name=source.name;
    oracle.height=source.height;
    for(const auto& glyph:source.glyphs)
      oracle.glyphs.push_back({glyph.width,{glyph.uv[0],glyph.uv[1]}});
    Tempest::Texture2d atlas;
    GthFont expected(oracle,&atlas,Tempest::Color(1.f));
    GthFont actual(loaded,&atlas,Tempest::Color(1.f));
    const std::vector<std::string> samples={"", "Gothic II", "one  two three", " leading spaces",
      "line\n   indented\n", "\n", "longwordwithoutspaces", "trailing  ", "\x80\xC0\xDF\xFF"};
    for(const float scale:{0.f,.03125f,.5f,1.f,1.25f,2.f}) {
      expected.setScale(scale);
      actual.setScale(scale);
      const auto field=name+" scale="+std::to_string(scale);
      const int height=int(std::max(float(source.height)*scale,1.f));
      equal(actual.pixelSize(),height,field+" height");
      int width=0;
      for(const char c:std::string_view("Gothic II"))
        width+=int(float(source.glyphs[uint8_t(c)].width)*scale);
      const auto plain=actual.textSize("Gothic II");
      equal(plain.w,width,field+" independently summed advances");
      equal(plain.h,height,field+" single-line height");
      for(const auto& text:samples) {
        const auto a=expected.textSize(text), b=actual.textSize(text);
        equal(a.w,b.w,field+" unwrapped width");
        equal(a.h,b.h,field+" unwrapped height");
        for(const int wrap:{0,1,38,96,256}) {
          const auto x=expected.textSize(wrap,text), y=actual.textSize(wrap,text);
          equal(x.w,y.w,field+" wrapped width "+std::to_string(wrap));
          equal(x.h,y.h,field+" wrapped height "+std::to_string(wrap));
          equal(expected.lineCount(wrap,text),actual.lineCount(wrap,text),field+" line count "+std::to_string(wrap));
          }
        }
      }
    }

  static size_t fonts(const fs::path& base, const Resources& resources) {
    const auto index=json(base/"fonts/fonts.json");
    require(index.HasMember("format") && index["format"].IsString() &&
            std::string_view(index["format"].GetString())=="gothic-fonts" && index["version"]==1,
            "font index must use gothic-fonts version 1");
    require(index.HasMember("fonts") && index["fonts"].IsObject(),"font index must contain font records");
    const auto& records=index["fonts"];
    std::set<std::string> names;
    fontNames(sourceVfs().root(),names);
    require(!names.empty(),"source font corpus must not be empty");
    equal(names.size(),size_t(records.MemberCount()),"font index must include every mounted FNT basename");
    equal(names.size(),resources.baseFonts.files.size(),"Resources font index count");
    for(const auto& name:names) {
      require(records.HasMember(name.c_str()) && records[name.c_str()].IsString(),name+": source font missing from base index");
      require(std::string_view(records[name.c_str()].GetString())==name+".json",name+": font record filename");
      require(resources.baseFonts.files.at(name)==name+".json",name+": Resources font record filename");
      const auto* entry=sourceVfs().find(name);
      require(entry!=nullptr,name+": original font missing");
      auto reader=entry->open_read();
      zenkit::Font source;
      source.load(reader.get());
      std::string atlas;
      const auto loaded=FontData::readFont(base/"fonts"/(name+".json"),&atlas);
      require(source.name==loaded.name,name+": exact name bytes differ");
      equal(source.height,loaded.height,name+" height");
      equal(source.glyphs.size(),loaded.glyphs.size(),name+" glyph count");
      equal(source.glyphs.size(),256u,name+" byte-indexed source glyph count");
      for(size_t i=0;i<source.glyphs.size();++i) {
        const auto field=name+" glyph["+std::to_string(i)+"]";
        equal(source.glyphs[i].width,loaded.glyphs[i].width,field+" width");
        for(size_t corner=0;corner<2;++corner) {
          modelField(source.glyphs[i].uv[corner].x,loaded.glyphs[i].uv[corner].x,field+" uv x");
          modelField(source.glyphs[i].uv[corner].y,loaded.glyphs[i].uv[corner].y,field+" uv y");
          }
        }
      const auto stem=name.substr(0,name.size()-4);
      require(atlas==stem+".TGA",name+": atlas must come from the requested FNT stem");
      require(resources.baseTextures.files.count(stem)==1,name+": atlas must reference an existing base texture");
      fontLayout(source,loaded,name);
      }
    std::printf("Fonts: %zu equal to independently parsed FNTs; every ordered glyph, UV bit, name, height and atlas equal; scaled sizes, wrapping and line counts equal\n",names.size());
    return names.size();
    }

  static void modelFixtures(const fs::path& temporary) {
    zenkit::ModelHierarchy hierarchy{};
    hierarchy.nodes={{-1,"ROOT",zenkit::Mat4::identity()},{0,"CHILD",zenkit::Mat4::identity()}};
    hierarchy.nodes[1].transform[3]={-0.f,12.25f,-34.5f,1};
    hierarchy.bbox={{-10,-20,-30},{40,50,60}};
    hierarchy.collision_bbox={{-1,-2,-3},{4,5,6}};
    hierarchy.root_translation={-0.f,17.5f,-22.25f}; hierarchy.checksum=0x12345678;
    hierarchy.source_date={2026,10,25,12,34,56}; hierarchy.source_path="source/model.asc";
    auto file=temporary/"MODEL_CONTRACT.MDH.json";
    ModelData::writeHierarchy(asset::fromZenKit(hierarchy),file);
    modelField(hierarchy,ModelData::readHierarchy(file),"synthetic hierarchy");

    zenkit::ModelAnimation animation{};
    animation.name="ANIMATION"; animation.next="NEXT"; animation.layer=3;
    animation.frame_count=2; animation.node_count=1; animation.fps=25.f; animation.fps_source=30.f;
    animation.sample_position_min=-0.f; animation.sample_position_scale=.03125f;
    animation.bbox=hierarchy.bbox; animation.checksum=hierarchy.checksum;
    animation.source_path="source/animation.asc"; animation.source_date=hierarchy.source_date;
    animation.source_script="ani (ANIMATION)"; animation.node_indices={1};
    animation.samples={{{-0.f,3,4},{.8f,-0.f,.6f,0}},{{5,-6,7},{-.8f,0,-.6f,-0.f}}};
    zenkit::AnimationEvent event{};
    event.type=zenkit::AnimationEventType::CAMERA_TREMOR; event.frame=7; event.tag="EVENT";
    for(size_t i=0;i<4;++i) { event.content[i]="content"+std::to_string(i); event.values[i]=float(i)-.5f; }
    event.values[0]=-0.f; event.probability=.75f; animation.events.push_back(event);
    file=temporary/"MODEL_CONTRACT.MAN.json";
    ModelData::writeAnimation(asset::fromZenKit(animation),file);
    modelField(animation,ModelData::readAnimation(file),"synthetic animation");

    zenkit::ModelScript script{};
    script.skeleton={"SOURCE.ASC",true}; script.meshes={"BODY.ASC","ARMOR.ASC"};
    script.disabled_animations={"DISABLED"}; script.model_tags={{"BONE"}};
    script.combinations={{"COMB",2,"NEXT",.25f,.5f,zenkit::AnimationFlags(0x81),"MODEL",5}};
    script.blends={{"BLEND","NEXT",.125f,.375f}};
    script.aliases={{"ALIAS",4,"NEXT",.25f,.5f,zenkit::AnimationFlags::MOVE,"ANIMATION",zenkit::AnimationDirection::BACKWARD}};
    zenkit::MdsAnimation ani{};
    ani.name="ANIMATION"; ani.layer=1; ani.next="NEXT"; ani.blend_in=.25f; ani.blend_out=.75f;
    ani.flags=zenkit::AnimationFlags::ROTATE; ani.model="SOURCE.ASC";
    ani.direction=zenkit::AnimationDirection::BACKWARD; ani.first_frame=3; ani.last_frame=7;
    ani.fps=25.f; ani.speed=1.5f; ani.collision_volume_scale=.875f;
    ani.events={{7,zenkit::MdsEventType::ITEM_EXCHANGE,"SLOT","SLOT2","ITEM",{1,3,5},zenkit::MdsFightMode::INVALID,true}};
    ani.pfx={{2,3,"PARTICLE","BONE",true}}; ani.pfx_stop={{5,3}};
    ani.sfx={{3,"SOUND",321.f,true}}; ani.sfx_ground={{4,"GROUND",456.f,true}};
    ani.morph={{6,"MORPH","HEAD"}}; ani.tremors={{8,1,2,3,4}};
    script.animations.push_back(ani);
    file=temporary/"MODEL_CONTRACT.MSB.json";
    ModelData::writeScript(asset::fromZenKit(script),file);
    modelField(script,ModelData::readScript(file),"synthetic script");

    zenkit::MorphMesh morph{};
    morph.name="MORPH"; morph.mesh.bbox=hierarchy.bbox;
    morph.mesh.positions={{1,2,3},{-0.f,4,5}}; morph.mesh.normals={{0,1,0},{0,0,1}};
    morph.morph_positions={{3,2,1},{6,5,4}};
    morph.animations={{"MOVE",2,.25f,.5f,1.5f,2.5f,0x81,2,{1},{{-0.f,.5f,.25f},{1,2,3}}}};
    morph.sources={{hierarchy.source_date,"source/morph.asc"}};
    file=temporary/"MODEL_CONTRACT.MMB.json";
    ModelData::writeMorph(asset::fromZenKit(morph),file);
    modelField(morph,ModelData::readMorph(file),"synthetic morph");

    zenkit::Model model{};
    model.hierarchy=hierarchy;
    std::vector<std::string> order;
    for(size_t i=0;i<24;++i) {
      const auto name="ATTACHMENT_"+std::to_string(i);
      order.push_back(name);
      model.mesh.attachments.emplace(name,morph.mesh);
      }
    zenkit::SoftSkinMesh skin{};
    skin.mesh=morph.mesh;
    skin.weights={{{.25f,{1,-0.f,3},0},{.75f,{-1,2,4},1}},{{1.f,{5,6,-7},1}}};
    zenkit::OrientedBoundingBox box{};
    box.center={1,2,3}; box.axes[0]={1,0,0}; box.axes[1]={0,1,0}; box.axes[2]={0,0,1}; box.half_width={4,5,6};
    auto child=box; child.center={-1,-2,-3}; box.children.push_back(child);
    skin.bboxes={box}; skin.mesh.obbox=box;
    zenkit::Material material{};
    material.name="MATERIAL"; material.group=zenkit::MaterialGroup::WOOD; material.color={12,34,56,78};
    material.smooth_angle=33.f; material.texture="TEXTURE.TGA"; material.texture_scale={2,3};
    material.texture_anim_fps=7.f; material.texture_anim_map_mode=zenkit::AnimationMapping::LINEAR;
    material.texture_anim_map_dir={-.25f,.5f}; material.disable_collision=true; material.disable_lightmap=true;
    material.dont_collapse=true; material.detail_object="DETAIL"; material.detail_object_scale=.75f;
    material.force_occluder=true; material.environment_mapping=true; material.environment_mapping_strength=.625f;
    material.wave_mode=zenkit::WaveMode::WIND; material.wave_speed=zenkit::WaveSpeed::FAST;
    material.wave_max_amplitude=9.f; material.wave_grid_size=13.f; material.ignore_sun=true;
    material.alpha_func=zenkit::AlphaFunction::BLEND; material.default_mapping={5,6};
    skin.mesh.materials={material}; skin.mesh.alpha_test=0;
    zenkit::SubMesh sub{}; sub.mat=material; sub.triangles={{{0,1,0}}};
    sub.wedges={{{0,1,0},{.25f,.75f},0},{{0,0,1},{.5f,.125f},1}};
    skin.mesh.sub_meshes={sub}; model.mesh.meshes.push_back(skin);
    auto stored=asset::fromZenKit(model);
    // Simulate a foreign standard library's map order while retaining source insertion order.
    std::reverse(stored.mesh.attachments.begin(),stored.mesh.attachments.end());
    file=temporary/"MODEL_CONTRACT.MDM.json";
    ModelData::writeMesh(stored.mesh,file,order);
    modelField(model.mesh,ModelData::readMesh(file),"synthetic model mesh");
    file=temporary/"MODEL_CONTRACT.MDL.json";
    ModelData::writeModel(stored,file,order);
    modelField(model,ModelData::readModel(file),"synthetic combined model");
    std::puts("Model fixtures: complete script/MAN events, metadata, sample bits, skin weights, nested OBB and native attachment order equal");
    }

  static void programSymbols(zenkit::DaedalusScript& source, const zenkit::DaedalusProgramRecord& record,
                             zenkit::DaedalusScript& loaded, const std::string& name) {
    equal(source.symbols().size(),record.symbols.size(),name+" record symbol count");
    equal(source.symbols().size(),loaded.symbols().size(),name+" factory symbol count");
    equal(source.size(),loaded.size(),name+" factory bytecode size");
    for(size_t i=0;i<record.symbols.size();++i) {
      auto* original=source.find_symbol_by_index(uint32_t(i));
      auto* converted=loaded.find_symbol_by_index(uint32_t(i));
      const auto& row=record.symbols[i];
      const auto field=name+" symbol["+std::to_string(i)+"]";
      require(original!=nullptr && converted!=nullptr,field+": original index must resolve");
      equal(original->index(),i,field+" source index");
      equal(converted->index(),i,field+" native index");
#define PROGRAM_FIELD(getter, member) \
      modelField(original->getter(),static_cast<decltype(original->getter())>(row.member),field+" record " #member); \
      modelField(original->getter(),converted->getter(),field+" factory " #member)
      PROGRAM_FIELD(name,name);
      PROGRAM_FIELD(is_generated,generated);
      PROGRAM_FIELD(type,type);
      PROGRAM_FIELD(count,count);
      PROGRAM_FIELD(address,address);
      PROGRAM_FIELD(parent,parent);
      PROGRAM_FIELD(rtype,return_type);
      PROGRAM_FIELD(offset_as_member,member_offset);
      PROGRAM_FIELD(class_size,class_size);
      PROGRAM_FIELD(file_index,file_index);
      PROGRAM_FIELD(line_start,line_start);
      PROGRAM_FIELD(line_count,line_count);
      PROGRAM_FIELD(char_start,char_start);
      PROGRAM_FIELD(char_count,char_count);
#undef PROGRAM_FIELD
      const auto flag=[&](uint32_t mask, bool a, bool b) {
        equal(a,(row.flags&mask)!=0,field+" record flag "+std::to_string(mask));
        equal(a,b,field+" factory flag "+std::to_string(mask));
        };
      flag(zenkit::DaedalusSymbolFlag::CONST,original->is_const(),converted->is_const());
      flag(zenkit::DaedalusSymbolFlag::MEMBER,original->is_member(),converted->is_member());
      flag(zenkit::DaedalusSymbolFlag::RETURN,original->has_return(),converted->has_return());
      flag(zenkit::DaedalusSymbolFlag::EXTERNAL,original->is_external(),converted->is_external());
      flag(zenkit::DaedalusSymbolFlag::MERGED,original->is_merged(),converted->is_merged());
      flag(zenkit::DaedalusSymbolFlag::TRAP_ACCESS,original->has_access_trap(),converted->has_access_trap());
      flag(zenkit::DaedalusSymbolFlag::FUNC_LOCALS,original->has_local_variables_enabled(),converted->has_local_variables_enabled());
      const auto lookup=[&](const auto* a, const auto* b, const char* kind) {
        require((a==nullptr)==(b==nullptr),field+": "+kind+" lookup presence differs");
        if(a!=nullptr) equal(a->index(),b->index(),field+" "+kind+" lookup index");
        };
      lookup(source.find_symbol_by_name(original->name()),loaded.find_symbol_by_name(original->name()),"name");
      lookup(source.find_symbol_by_address(original->address()),loaded.find_symbol_by_address(original->address()),"address");
      // Only these source arrays are initialized by the DAT parser. In particular,
      // a FUNCTION's runtime slot must never be read, even when it is nonconstant.
      const bool values=!original->is_member();
      equal(row.int_values.size(),values && original->type()==zenkit::DaedalusDataType::INT ? original->count() : 0u,field+" int array size");
      equal(row.float_bits.size(),values && original->type()==zenkit::DaedalusDataType::FLOAT ? original->count() : 0u,field+" float array size");
      equal(row.string_values.size(),values && original->type()==zenkit::DaedalusDataType::STRING ? original->count() : 0u,field+" string array size");
      for(uint16_t j=0;j<original->count() && values;++j) {
        if(original->type()==zenkit::DaedalusDataType::INT) {
          equal(original->get_int(j),row.int_values[j],field+" record int");
          equal(original->get_int(j),converted->get_int(j),field+" factory int");
          }
        else if(original->type()==zenkit::DaedalusDataType::FLOAT) {
          equal(std::bit_cast<uint32_t>(original->get_float(j)),row.float_bits[j],field+" record float bits");
          equal(std::bit_cast<uint32_t>(original->get_float(j)),std::bit_cast<uint32_t>(converted->get_float(j)),field+" factory float bits");
          }
        else if(original->type()==zenkit::DaedalusDataType::STRING) {
          modelField(original->get_string(j),row.string_values[j],field+" record string bytes");
          modelField(original->get_string(j),converted->get_string(j),field+" factory string bytes");
          }
        }
      if(values && original->type()==zenkit::DaedalusDataType::INSTANCE)
        require(original->get_instance()==nullptr && converted->get_instance()==nullptr,field+": fresh instances must be null");
      }
    require(source.find_symbol_by_index(uint32_t(record.symbols.size()))==nullptr &&
            loaded.find_symbol_by_index(uint32_t(record.symbols.size()))==nullptr,name+": out-of-range index must stay absent");
    // class_offset and the unnamed flag bit have no public symbol getter. Keep
    // this complete record check in addition to the independent public checks.
    const auto difference=ProgramData::compare(source.export_program(),record);
    require(difference.empty(),name+": "+difference);
    const auto factoryDifference=ProgramData::compare(record,loaded.export_program());
    require(factoryDifference.empty(),name+" factory: "+factoryDifference);
    for(uint32_t address=0;address<source.size();) {
      const auto a=source.instruction_at(address),b=loaded.instruction_at(address);
      require(a.op==b.op && a.address==b.address && a.symbol==b.symbol && a.immediate==b.immediate &&
              a.index==b.index && a.size==b.size,name+": public bytecode decoding differs at "+std::to_string(address));
      require(a.size>0 && uint64_t(address)+a.size<=source.size(),name+": source instruction bounds");
      address+=a.size;
      }
    }

  static size_t programs(const fs::path& base, const fs::path& censusPath) {
    const auto index=ProgramData::readIndex(base/"programs");
    const auto gothic=fs::path(CommandLine::inst().rootPath());
    require(!index.programs.empty(),"program corpus must contain native records");
    size_t parsed=0,symbols=0,code=0;
    std::set<std::string> records;
    std::map<std::pair<std::string,std::string>,size_t> originalOpens;
    for(const auto& source:index.sources) {
      const auto field=source.origin+":"+source.path;
      auto basename=fs::path(source.path).filename().string();
      for(auto& c:basename) if(c>='a' && c<='z') c=char(c-'a'+'A');
      std::unique_ptr<zenkit::Read> input;
      if(source.origin=="host") {
        require(source.archive.empty() && !fs::path(source.path).is_absolute(),field+": host provenance");
        input=zenkit::Read::from(gothic/source.path);
        Resources::censusFile((gothic/source.path).u16string());
        ++originalOpens[{basename,source.path}];
        }
      else {
        require(source.origin=="archive" && !source.archive.empty(),field+": archive provenance");
        const auto* node=sourceVfs().resolve(source.path);
        require(node!=nullptr,field+": full source VFS path missing");
        SourceObservation opened;
        input=readSource(fs::path(source.path).filename().string(),&opened);
        auto archive=opened.source;
        for(auto& c:archive) if(c>='a' && c<='z') c=char(c-'a'+'A');
        require(opened.node==node && archive==source.archive,field+": actual mounted node/archive differs from provenance");
        ++originalOpens[{basename,opened.source}];
        }
      require(input!=nullptr,field+": original source cannot be opened");
      input->seek(0,zenkit::Whence::END);
      std::vector<std::byte> raw(input->tell());
      input->seek(0,zenkit::Whence::BEG);
      require(input->read(raw.data(),raw.size())==raw.size(),field+": incomplete original source");
      equal(raw.size(),source.size,field+" original size");
      require(BaseImport::sha256(std::string_view(reinterpret_cast<const char*>(raw.data()),raw.size()))==source.sha256,
              field+": original SHA-256 differs from imported provenance");
      if(!source.skipped.empty()) {
        require(source.program.empty() && source.recordSha256.empty(),field+": skipped source cannot name a program");
        const std::string_view rawText(reinterpret_cast<const char*>(raw.data()),raw.size());
        if(rawText.starts_with("ZenGin Archive") || raw.size()<9) continue;
        auto header=zenkit::Read::from(&raw);
        (void)header->read_ubyte();
        const auto count=header->read_uint();
        if(uint64_t(count)*4+9>raw.size()) continue;
        bool valid=false;
        try {
          auto reader=zenkit::Read::from(&raw);
          zenkit::DaedalusScript oracle;
          oracle.load(reader.get());
          const auto end=reader->tell();
          const auto program=oracle.export_program();
          valid=end<=raw.size() && program.bytecode.size()<=end && program.symbols.size()==count &&
                std::equal(program.bytecode.begin(),program.bytecode.end(),raw.begin()+std::ptrdiff_t(end-program.bytecode.size()));
          }
        catch(const std::exception&) {}
        require(!valid,field+": importer skipped a source that independently parses as a complete program");
        continue;
        }
      auto reader=zenkit::Read::from(&raw);
      zenkit::DaedalusScript original;
      original.load(reader.get());
      const auto end=reader->tell();
      std::string digest;
      const auto record=ProgramData::readProgram(index.path(source),&digest);
      records.insert(source.program);
      require(digest==source.recordSha256 && index.programs.at(source.program)==digest,field+": actual native record digest");
      auto header=zenkit::Read::from(&raw);
      equal(header->read_ubyte(),record.version,field+" original version byte");
      equal(header->read_uint(),record.symbols.size(),field+" original symbol header");
      equal(record.bytecode.size(),original.size(),field+" original code size");
      require(end>=record.bytecode.size()+4 && end<=raw.size(),field+": original parser code bounds");
      auto codeHeader=zenkit::Read::from(raw.data()+end-record.bytecode.size()-4,4);
      equal(codeHeader->read_uint(),record.bytecode.size(),field+" original code header");
      bytes(raw.data()+end-record.bytecode.size(),record.bytecode.data(),record.bytecode.size(),field+" original bytecode");
      equal(raw.size()-end,source.trailingBytes,field+" trailing bytes");
      auto native=zenkit::DaedalusScript::from_program(record);
      programSymbols(original,record,native,field);
      const auto count=original.symbols().size();
      const auto* first=count==0 ? nullptr : native.find_symbol_by_index(0);
      zenkit::DaedalusVm sourceVm(std::move(original)),nativeVm(std::move(native));
      equal(sourceVm.symbols().size(),count+1,field+" source VM temporary symbol");
      equal(nativeVm.symbols().size(),count+1,field+" native VM temporary symbol");
      require(first==nullptr || first==nativeVm.find_symbol_by_index(0),field+": VM setup must preserve source symbol addresses");
      auto* a=sourceVm.find_symbol_by_index(uint32_t(count));
      auto* b=nativeVm.find_symbol_by_index(uint32_t(count));
      require(a!=nullptr && b!=nullptr && a->index()==count && b->index()==count &&
              a->name()==b->name() && a->type()==b->type() && a->count()==b->count() &&
              a->get_string()==b->get_string(),field+": VM temporary strings must retain defined behavior");
      ++parsed; symbols+=count; code+=record.bytecode.size();
      }
    require(parsed>0,"program corpus must contain parsed sources");
    equal(records.size(),index.programs.size(),"every indexed native program must have a checked source");
    require(Resources::writeCensus(),"cannot write program oracle census");
    const auto before=json(censusPath);
    for(const auto& row:before["files"].GetArray()) {
      const auto found=originalOpens.find({row["name"].GetString(),row["source"].GetString()});
      if(found==originalOpens.end()) continue;
      equal(row["opens"].GetUint64(),found->second,std::string(row["name"].GetString())+": only independent program oracle opens the source");
      originalOpens.erase(found);
      }
    require(originalOpens.empty(),"every independent original program read must appear once in the census");
    size_t selected=0;
    for(const auto& row:index.selections) {
      if(row.language!=-1 || !row.source) continue;
      const auto& source=index.sources.at(*row.source);
      const auto host=row.step.starts_with("host_") ? (gothic/source.path).u16string() : std::u16string{};
      const auto script=Resources::loadBaseProgram(row.request,row.language,row.step,host);
      equal(script.symbols().size(),ProgramData::readProgram(index.path(source)).symbols.size(),row.request+" selected symbol count");
      ++selected;
      }
    equal(selected,8u,"all eight default scripts must select native records");
    require(Resources::writeCensus(),"cannot write native program census");
    const auto after=json(censusPath);
    require(before["files"]==after["files"],"native program loading must add zero original source opens");
    equal(after["runtime_programs"]["selections"].Size(),selected,"actual native program census selections");
    std::printf("Programs: %zu sources, %zu symbols and %zu bytecode bytes match independent DAT reads; all eight native selections add zero DAT opens\n",parsed,symbols,code);
    return selected;
    }

  // Each old addon field is checked explicitly against the separately bound engine record.
#define DEFINITION_FIELD(member) modelField(a.member,b.member,field+"." #member)
  static void definitionFields(const zenkit::ICamera& a, const Def::Camera& b, const std::string& field) {
    DEFINITION_FIELD(best_range);
    DEFINITION_FIELD(min_range);
    DEFINITION_FIELD(max_range);
    DEFINITION_FIELD(best_elevation);
    DEFINITION_FIELD(min_elevation);
    DEFINITION_FIELD(max_elevation);
    DEFINITION_FIELD(best_azimuth);
    DEFINITION_FIELD(min_azimuth);
    DEFINITION_FIELD(max_azimuth);
    DEFINITION_FIELD(best_rot_z);
    DEFINITION_FIELD(min_rot_z);
    DEFINITION_FIELD(max_rot_z);
    DEFINITION_FIELD(rot_offset_x);
    DEFINITION_FIELD(rot_offset_y);
    DEFINITION_FIELD(rot_offset_z);
    DEFINITION_FIELD(target_offset_x);
    DEFINITION_FIELD(target_offset_y);
    DEFINITION_FIELD(target_offset_z);
    DEFINITION_FIELD(velo_trans);
    DEFINITION_FIELD(velo_rot);
    DEFINITION_FIELD(translate);
    DEFINITION_FIELD(rotate);
    DEFINITION_FIELD(collision);
    }

  static void definitionFields(const zenkit::ISoundEffect& a, const Def::SoundEffect& b, const std::string& field) {
    DEFINITION_FIELD(file);
    DEFINITION_FIELD(pitch_off);
    DEFINITION_FIELD(pitch_var);
    DEFINITION_FIELD(vol);
    DEFINITION_FIELD(loop);
    DEFINITION_FIELD(loop_start_offset);
    DEFINITION_FIELD(loop_end_offset);
    DEFINITION_FIELD(reverb_level);
    DEFINITION_FIELD(pfx_name);
    }

  static void definitionFields(const zenkit::IMusicTheme& a, const Def::MusicTheme& b, const std::string& field) {
    DEFINITION_FIELD(file);
    DEFINITION_FIELD(vol);
    DEFINITION_FIELD(loop);
    DEFINITION_FIELD(reverbmix);
    DEFINITION_FIELD(reverbtime);
    DEFINITION_FIELD(transtype);
    DEFINITION_FIELD(transsubtype);
    }

  static void definitionFields(const zenkit::IParticleEffect& a, const Def::ParticleEffect& b, const std::string& field) {
    DEFINITION_FIELD(pps_value);
    DEFINITION_FIELD(pps_scale_keys_s);
    DEFINITION_FIELD(pps_is_looping);
    DEFINITION_FIELD(pps_is_smooth);
    DEFINITION_FIELD(pps_fps);
    DEFINITION_FIELD(pps_create_em_s);
    DEFINITION_FIELD(pps_create_em_delay);
    DEFINITION_FIELD(shp_type_s);
    DEFINITION_FIELD(shp_for_s);
    DEFINITION_FIELD(shp_offset_vec_s);
    DEFINITION_FIELD(shp_distrib_type_s);
    DEFINITION_FIELD(shp_distrib_walk_speed);
    DEFINITION_FIELD(shp_is_volume);
    DEFINITION_FIELD(shp_dim_s);
    DEFINITION_FIELD(shp_mesh_s);
    DEFINITION_FIELD(shp_mesh_render_b);
    DEFINITION_FIELD(shp_scale_keys_s);
    DEFINITION_FIELD(shp_scale_is_looping);
    DEFINITION_FIELD(shp_scale_is_smooth);
    DEFINITION_FIELD(shp_scale_fps);
    DEFINITION_FIELD(dir_mode_s);
    DEFINITION_FIELD(dir_for_s);
    DEFINITION_FIELD(dir_mode_target_for_s);
    DEFINITION_FIELD(dir_mode_target_pos_s);
    DEFINITION_FIELD(dir_angle_head);
    DEFINITION_FIELD(dir_angle_head_var);
    DEFINITION_FIELD(dir_angle_elev);
    DEFINITION_FIELD(dir_angle_elev_var);
    DEFINITION_FIELD(vel_avg);
    DEFINITION_FIELD(vel_var);
    DEFINITION_FIELD(lsp_part_avg);
    DEFINITION_FIELD(lsp_part_var);
    DEFINITION_FIELD(fly_gravity_s);
    DEFINITION_FIELD(fly_colldet_b);
    DEFINITION_FIELD(vis_name_s);
    DEFINITION_FIELD(vis_orientation_s);
    DEFINITION_FIELD(vis_tex_is_quadpoly);
    DEFINITION_FIELD(vis_tex_ani_fps);
    DEFINITION_FIELD(vis_tex_ani_is_looping);
    DEFINITION_FIELD(vis_tex_color_start_s);
    DEFINITION_FIELD(vis_tex_color_end_s);
    DEFINITION_FIELD(vis_size_start_s);
    DEFINITION_FIELD(vis_size_end_scale);
    DEFINITION_FIELD(vis_alpha_func_s);
    DEFINITION_FIELD(vis_alpha_start);
    DEFINITION_FIELD(vis_alpha_end);
    DEFINITION_FIELD(trl_fade_speed);
    DEFINITION_FIELD(trl_texture_s);
    DEFINITION_FIELD(trl_width);
    DEFINITION_FIELD(mrk_fades_peed);
    DEFINITION_FIELD(mrkt_exture_s);
    DEFINITION_FIELD(mrk_size);
    DEFINITION_FIELD(flock_mode);
    DEFINITION_FIELD(flock_strength);
    DEFINITION_FIELD(use_emitters_for);
    DEFINITION_FIELD(time_start_end_s);
    DEFINITION_FIELD(m_bis_ambient_pfx);
    }

  static void definitionFields(const zenkit::IEffectBase& a, const Def::EffectBase& b, const std::string& field) {
    DEFINITION_FIELD(vis_name_s);
    DEFINITION_FIELD(vis_size_s);
    DEFINITION_FIELD(vis_alpha);
    DEFINITION_FIELD(vis_alpha_blend_func_s);
    DEFINITION_FIELD(vis_tex_ani_fps);
    DEFINITION_FIELD(vis_tex_ani_is_looping);
    DEFINITION_FIELD(em_trj_mode_s);
    DEFINITION_FIELD(em_trj_origin_node);
    DEFINITION_FIELD(em_trj_target_node);
    DEFINITION_FIELD(em_trj_target_range);
    DEFINITION_FIELD(em_trj_target_azi);
    DEFINITION_FIELD(em_trj_target_elev);
    DEFINITION_FIELD(em_trj_num_keys);
    DEFINITION_FIELD(em_trj_num_keys_var);
    DEFINITION_FIELD(em_trj_angle_elev_var);
    DEFINITION_FIELD(em_trj_angle_head_var);
    DEFINITION_FIELD(em_trj_key_dist_var);
    DEFINITION_FIELD(em_trj_loop_mode_s);
    DEFINITION_FIELD(em_trj_ease_func_s);
    DEFINITION_FIELD(em_trj_ease_vel);
    DEFINITION_FIELD(em_trj_dyn_update_delay);
    DEFINITION_FIELD(em_trj_dyn_update_target_only);
    DEFINITION_FIELD(em_fx_create_s);
    DEFINITION_FIELD(em_fx_invest_origin_s);
    DEFINITION_FIELD(em_fx_invest_target_s);
    DEFINITION_FIELD(em_fx_trigger_delay);
    DEFINITION_FIELD(em_fx_create_down_trj);
    DEFINITION_FIELD(em_action_coll_dyn_s);
    DEFINITION_FIELD(em_action_coll_stat_s);
    DEFINITION_FIELD(em_fx_coll_stat_s);
    DEFINITION_FIELD(em_fx_coll_dyn_s);
    DEFINITION_FIELD(em_fx_coll_stat_align_s);
    DEFINITION_FIELD(em_fx_coll_dyn_align_s);
    DEFINITION_FIELD(em_fx_lifespan);
    DEFINITION_FIELD(em_check_collision);
    DEFINITION_FIELD(em_adjust_shp_to_origin);
    DEFINITION_FIELD(em_invest_next_key_duration);
    DEFINITION_FIELD(em_fly_gravity);
    DEFINITION_FIELD(em_self_rot_vel_s);
    for(size_t i=0;i<Def::EffectBase::user_string_count;++i)
      modelField(a.user_string[i],b.user_string[i],field+".user_string["+std::to_string(i)+"]");
    DEFINITION_FIELD(light_preset_name);
    DEFINITION_FIELD(sfx_id);
    DEFINITION_FIELD(sfx_is_ambient);
    DEFINITION_FIELD(send_assess_magic);
    DEFINITION_FIELD(secs_per_damage);
    DEFINITION_FIELD(em_fx_coll_dyn_perc_s);
    }

  static void definitionFields(const zenkit::IParticleEffectEmitKey& a, const Def::ParticleEffectEmitKey& b, const std::string& field) {
    DEFINITION_FIELD(vis_name_s);
    DEFINITION_FIELD(vis_size_scale);
    DEFINITION_FIELD(scale_duration);
    DEFINITION_FIELD(pfx_pps_value);
    DEFINITION_FIELD(pfx_pps_is_smooth_chg);
    DEFINITION_FIELD(pfx_pps_is_looping_chg);
    DEFINITION_FIELD(pfx_sc_time);
    DEFINITION_FIELD(pfx_fly_gravity_s);
    DEFINITION_FIELD(pfx_shp_dim_s);
    DEFINITION_FIELD(pfx_shp_is_volume_chg);
    DEFINITION_FIELD(pfx_shp_scale_fps);
    DEFINITION_FIELD(pfx_shp_distrib_walks_peed);
    DEFINITION_FIELD(pfx_shp_offset_vec_s);
    DEFINITION_FIELD(pfx_shp_distrib_type_s);
    DEFINITION_FIELD(pfx_dir_mode_s);
    DEFINITION_FIELD(pfx_dir_for_s);
    DEFINITION_FIELD(pfx_dir_mode_target_for_s);
    DEFINITION_FIELD(pfx_dir_mode_target_pos_s);
    DEFINITION_FIELD(pfx_vel_avg);
    DEFINITION_FIELD(pfx_lsp_part_avg);
    DEFINITION_FIELD(pfx_vis_alpha_start);
    DEFINITION_FIELD(light_preset_name);
    DEFINITION_FIELD(light_range);
    DEFINITION_FIELD(sfx_id);
    DEFINITION_FIELD(sfx_is_ambient);
    DEFINITION_FIELD(em_create_fx_id);
    DEFINITION_FIELD(em_fly_gravity);
    DEFINITION_FIELD(em_self_rot_vel_s);
    DEFINITION_FIELD(em_trj_mode_s);
    DEFINITION_FIELD(em_trj_ease_vel);
    DEFINITION_FIELD(em_check_collision);
    DEFINITION_FIELD(em_fx_lifespan);
    }

#undef DEFINITION_FIELD
  static void definitionFields(const zenkit::IFightAi& a, const Def::FightAi& b, const std::string& field) {
    for(size_t i=0;i<Def::FightAi::move_count;++i)
      modelField(a.move[i],b.move[i],field+".move["+std::to_string(i)+"]");
    }

  static void definitionSourceClasses(zenkit::DaedalusScript& sc) {
    auto registerIfExists = [&](std::string_view sym, auto d) {
      if(sc.find_symbol_by_name(sym) != nullptr) {
        d(sc);
        }
      };

    auto registerOpaque = [&](std::string_view sym){
      if(auto ptr = sc.find_symbol_by_name(sym)) {
        sc.register_as_opaque(ptr);
        }
      };

    registerIfExists("C_GILVALUES",    zenkit::IGuildValues::register_);
    registerIfExists("C_NPC",          zenkit::INpc::register_);
    registerIfExists("C_MISSION",      zenkit::IMission::register_);
    registerIfExists("C_ITEM",         zenkit::IItem::register_);
    registerIfExists("C_FOCUS",        zenkit::IFocus::register_);
    registerIfExists("C_INFO",         zenkit::IInfo::register_);
    registerIfExists("C_ITEMREACT",    zenkit::IItemReact::register_);
    registerIfExists("C_SPELL",        zenkit::ISpell::register_);
    registerOpaque  ("C_SVM"); // structure is very customizable, so better to use type-less mapping
    registerIfExists("C_MENU",         zenkit::IMenu::register_);
    registerIfExists("C_MENU_ITEM",    zenkit::IMenuItem::register_);
    registerIfExists("CCAMSYS",        zenkit::ICamera::register_);
    registerIfExists("C_MUSICSYS_CFG", zenkit::IMusicSystem::register_);
    registerIfExists("C_MUSICTHEME",   zenkit::IMusicTheme::register_);
    registerIfExists("C_MUSICJINGLE",  zenkit::IMusicJingle::register_);
    registerIfExists("C_PARTICLEFX",   zenkit::IParticleEffect::register_);
    registerIfExists("CFX_BASE",       zenkit::IEffectBase::register_);
    registerIfExists("C_PARTICLEFXEMITKEY", zenkit::IParticleEffectEmitKey::register_);
    registerOpaque  ("C_FIGHTAI"); // many mods get it wrong/corrupted. Opaque binding is one is a way to get around it
    registerIfExists("C_SFX",          zenkit::ISoundEffect::register_);
    registerIfExists("C_SNDSYS_CFG",   zenkit::ISoundSystem::register_);
    }

  static std::unique_ptr<zenkit::DaedalusVm> definitionSourceVm(const ProgramData::Index& index,
                                                              std::string_view request, Gothic& gothic) {
    const auto row=std::find_if(index.selections.begin(),index.selections.end(),[&](const auto& selection) {
      return selection.request==request && selection.language==-1;
      });
    require(row!=index.selections.end() && row->source.has_value(),std::string(request)+": definition source selection missing");
    const auto& source=index.sources.at(*row->source);
    require(source.skipped.empty(),std::string(request)+": definition source was skipped");
    std::unique_ptr<zenkit::Read> input;
    if(source.origin=="host") {
      const auto path=fs::path(CommandLine::inst().rootPath())/source.path;
      input=zenkit::Read::from(path);
      Resources::censusFile(path.u16string());
      }
    else {
      SourceObservation opened;
      input=readSource(fs::path(source.path).filename().string(),&opened);
      require(opened.node!=nullptr && opened.node==sourceVfs().resolve(source.path),
              std::string(request)+": definition oracle selected a different source node");
      }
    require(input!=nullptr,std::string(request)+": definition source cannot be opened");
    input->seek(0,zenkit::Whence::END);
    std::vector<std::byte> raw(input->tell());
    input->seek(0,zenkit::Whence::BEG);
    require(input->read(raw.data(),raw.size())==raw.size(),std::string(request)+": incomplete definition source");
    require(raw.size()==source.size &&
            BaseImport::sha256(std::string_view(reinterpret_cast<const char*>(raw.data()),raw.size()))==source.sha256,
            std::string(request)+": original definition identity differs");
    auto reader=zenkit::Read::from(&raw);
    zenkit::DaedalusScript script;
    script.load(reader.get());
    // Replay the original registrations, including opaque Fight, before constructing the oracle VM.
    definitionSourceClasses(script);
    auto vm=std::make_unique<zenkit::DaedalusVm>(std::move(script),zenkit::DaedalusVmExecutionFlag::ALLOW_NULL_INSTANCE_ACCESS);
    gothic.setupVmCommonApi(*vm);
    return vm;
    }

  static std::vector<std::pair<uint32_t,std::string>> definitionNames(zenkit::DaedalusVm& vm, std::string_view className) {
    std::vector<std::pair<uint32_t,std::string>> result;
    vm.enumerate_instances_by_class_name(className,[&](zenkit::DaedalusSymbol& symbol) {
      result.emplace_back(symbol.index(),symbol.name());
      });
    return result;
    }

  template<class T>
  static std::pair<std::shared_ptr<T>,std::string> definitionInstance(zenkit::DaedalusVm& vm, zenkit::DaedalusSymbol* symbol) {
    try {
      auto result=std::make_shared<T>();
      if constexpr(std::is_same_v<T,zenkit::IParticleEffect> || std::is_same_v<T,Def::ParticleEffect>)
        result->vis_tex_is_quadpoly=1;
      vm.init_instance(result,symbol);
      return {std::move(result),{}};
      }
    catch(const zenkit::DaedalusScriptError& error) {
      return {nullptr,std::string(typeid(error).name())+": "+error.what()};
      }
    }

  template<class Source, class Native>
  static void definitionInstances(zenkit::DaedalusVm& source, zenkit::DaedalusVm& native, std::string_view className) {
    const auto names=definitionNames(source,className);
    modelField(names,definitionNames(native,className),std::string(className)+" ordered instances");
    const auto* sourceClass=source.find_symbol_by_name(className);
    const auto* nativeClass=native.find_symbol_by_name(className);
    require((sourceClass==nullptr)==(nativeClass==nullptr),std::string(className)+": class presence differs");
    if(sourceClass!=nullptr)
      equal(sourceClass->index(),nativeClass->index(),std::string(className)+" source class index");
    size_t initialized=0,failed=0;
    for(const auto& [index,name]:names) {
      const auto field=std::string(className)+":"+name;
      auto* a=source.find_symbol_by_index(index);
      auto* b=native.find_symbol_by_index(index);
      require(a!=nullptr && b!=nullptr && a->name()==b->name(),field+": source instance index changed");
      const auto* lookupA=source.find_symbol_by_name(name);
      const auto* lookupB=native.find_symbol_by_name(name);
      require(lookupA!=nullptr && lookupB!=nullptr,field+": named instance lookup missing");
      equal(lookupA->index(),lookupB->index(),field+" named lookup index");
      auto original=definitionInstance<Source>(source,a);
      auto loaded=definitionInstance<Native>(native,b);
      require(original.second==loaded.second,field+": initializer exception differs: "+original.second+" vs "+loaded.second);
      require(bool(original.first)==bool(loaded.first),field+": initializer result differs");
      if(!original.first) { ++failed; continue; }
      equal(original.first->symbol_index(),index,field+" original bound symbol");
      equal(loaded.first->symbol_index(),index,field+" native bound symbol");
      equal(source.find_symbol_by_instance(original.first)->index(),native.find_symbol_by_instance(loaded.first)->index(),
            field+" instance lookup index");
      definitionFields(*original.first,*loaded.first,field);
      ++initialized;
      }
    std::printf("Definitions %.*s: %zu ordered instances, %zu initialized and %zu matching initialization errors\n",
                int(className.size()),className.data(),names.size(),initialized,failed);
    }

  template<class T>
  static std::pair<T,std::string> definitionFight(zenkit::DaedalusVm& vm, zenkit::DaedalusSymbol* symbol) {
    T result{};
    if(symbol==nullptr) return {result,"missing instance"};
    auto* move=vm.find_symbol_by_name("C_FIGHTAI.MOVE");
    if(move==nullptr) return {result,"missing MOVE"};
    try {
      auto instance=vm.init_opaque_instance(symbol);
      equal(instance->symbol_index(),symbol->index(),symbol->name()+" opaque bound symbol");
      for(size_t i=0;i<T::move_count && i<move->count();++i)
        result.move[i]=std::remove_cvref_t<decltype(result.move[i])>(move->get_int(uint16_t(i),instance.get()));
      return {result,{}};
      }
    catch(const zenkit::DaedalusScriptError& error) {
      return {T{},std::string(typeid(error).name())+": "+error.what()};
      }
    }

  static void definitionFights(zenkit::DaedalusVm& source, zenkit::DaedalusVm& native) {
    const auto names=definitionNames(source,"C_FIGHTAI");
    modelField(names,definitionNames(native,"C_FIGHTAI"),"Fight ordered instances");
    const auto* a=source.find_symbol_by_name("MAX_FIGHTAI");
    const auto* b=native.find_symbol_by_name("MAX_FIGHTAI");
    require((a==nullptr)==(b==nullptr),"Fight MAX_FIGHTAI presence differs");
    const int count=a!=nullptr ? a->get_int() : 0;
    equal(count,b!=nullptr ? b->get_int() : 0,"Fight MAX_FIGHTAI");
    const std::array<std::string_view,17> prefixes={
      "FA_ENEMY_PREHIT_","FA_ENEMY_STORMPREHIT_","FA_MY_W_COMBO_","FA_MY_W_RUNTO_","FA_MY_W_STRAFE_",
      "FA_MY_W_FOCUS_","FA_MY_W_NOFOCUS_","FA_MY_G_COMBO_","FA_MY_G_RUNTO_","FA_MY_G_STRAFE_",
      "FA_MY_G_FOCUS_","FA_MY_FK_FOCUS_","FA_MY_G_FK_NOFOCUS_","FA_MY_FK_FOCUS_FAR_",
      "FA_MY_FK_NOFOCUS_FAR_","FA_MY_FK_FOCUS_MAG_","FA_MY_FK_NOFOCUS_MAG_"};
    std::set<uint32_t> checked;
    size_t groups=0;
    const auto compare=[&](zenkit::DaedalusSymbol* a,zenkit::DaedalusSymbol* b,const std::string& name) {
      require((a==nullptr)==(b==nullptr),name+": Fight instance presence differs");
      if(a!=nullptr) {
        equal(a->index(),b->index(),name+" Fight source index");
        checked.insert(a->index());
        }
      const auto old=definitionFight<zenkit::IFightAi>(source,a);
      const auto own=definitionFight<Def::FightAi>(native,b);
      require(old.second==own.second,name+": Fight initializer outcome differs");
      definitionFields(old.first,own.first,name);
      ++groups;
      };
    // Runtime's row zero is left value-initialized; rows 1..MAX_FIGHTAI-1 use these exact 17 groups.
    definitionFields(zenkit::IFightAi{},Def::FightAi{},"Fight row zero");
    for(int id=1;id<std::max(0,count);++id)
      for(const auto prefix:prefixes) {
        const auto name=std::string(prefix)+std::to_string(id);
        compare(source.find_symbol_by_name(name),native.find_symbol_by_name(name),name);
        }
    // Also cover declared records outside the runtime's groups, without changing production laziness.
    for(const auto& [index,name]:names)
      if(!checked.count(index))
        compare(source.find_symbol_by_index(index),native.find_symbol_by_index(index),name);
    for(const auto& [index,name]:names)
      require(checked.count(index)!=0,name+": Fight declaration was not checked");
    std::printf("Definitions C_FIGHTAI: %zu ordered instances and %zu runtime/extra groups; opaque six-move values match\n",
                names.size(),groups);
    }

  static void definitionRecords(const fs::path& base, const fs::path& censusPath) {
    static_assert(std::is_same_v<std::underlying_type_t<Def::FightMove>,std::underlying_type_t<zenkit::FightAiMove>>);
    static_assert(uint32_t(Def::FightMove::NOP)==uint32_t(zenkit::FightAiMove::NOP));
    static_assert(uint32_t(Def::FightMove::RUN)==uint32_t(zenkit::FightAiMove::RUN));
    static_assert(uint32_t(Def::FightMove::RUN_BACK)==uint32_t(zenkit::FightAiMove::RUN_BACK));
    static_assert(uint32_t(Def::FightMove::JUMP_BACK)==uint32_t(zenkit::FightAiMove::JUMP_BACK));
    static_assert(uint32_t(Def::FightMove::TURN)==uint32_t(zenkit::FightAiMove::TURN));
    static_assert(uint32_t(Def::FightMove::STRAFE)==uint32_t(zenkit::FightAiMove::STRAFE));
    static_assert(uint32_t(Def::FightMove::ATTACK)==uint32_t(zenkit::FightAiMove::ATTACK));
    static_assert(uint32_t(Def::FightMove::ATTACK_SIDE)==uint32_t(zenkit::FightAiMove::ATTACK_SIDE));
    static_assert(uint32_t(Def::FightMove::ATTACK_FRONT)==uint32_t(zenkit::FightAiMove::ATTACK_FRONT));
    static_assert(uint32_t(Def::FightMove::ATTACK_TRIPLE)==uint32_t(zenkit::FightAiMove::ATTACK_TRIPLE));
    static_assert(uint32_t(Def::FightMove::ATTACK_WHIRL)==uint32_t(zenkit::FightAiMove::ATTACK_WHIRL));
    static_assert(uint32_t(Def::FightMove::ATTACK_MASTER)==uint32_t(zenkit::FightAiMove::ATTACK_MASTER));
    static_assert(uint32_t(Def::FightMove::TURN_TO_HIT)==uint32_t(zenkit::FightAiMove::TURN_TO_HIT));
    static_assert(uint32_t(Def::FightMove::PARRY)==uint32_t(zenkit::FightAiMove::PARRY));
    static_assert(uint32_t(Def::FightMove::STAND_UP)==uint32_t(zenkit::FightAiMove::STAND_UP));
    static_assert(uint32_t(Def::FightMove::WAIT)==uint32_t(zenkit::FightAiMove::WAIT));
    static_assert(uint32_t(Def::FightMove::WAIT_LONGER)==uint32_t(zenkit::FightAiMove::WAIT_LONGER));
    static_assert(uint32_t(Def::FightMove::WAIT_EXT)==uint32_t(zenkit::FightAiMove::WAIT_EXT));
    static_assert(std::is_same_v<std::underlying_type_t<Def::MusicTransitionEffect>,std::underlying_type_t<zenkit::MusicTransitionEffect>>);
    static_assert(uint32_t(Def::MusicTransitionEffect::UNKNOWN)==uint32_t(zenkit::MusicTransitionEffect::UNKNOWN));
    static_assert(uint32_t(Def::MusicTransitionEffect::NONE)==uint32_t(zenkit::MusicTransitionEffect::NONE));
    static_assert(uint32_t(Def::MusicTransitionEffect::GROOVE)==uint32_t(zenkit::MusicTransitionEffect::GROOVE));
    static_assert(uint32_t(Def::MusicTransitionEffect::FILL)==uint32_t(zenkit::MusicTransitionEffect::FILL));
    static_assert(uint32_t(Def::MusicTransitionEffect::BREAK)==uint32_t(zenkit::MusicTransitionEffect::BREAK));
    static_assert(uint32_t(Def::MusicTransitionEffect::INTRO)==uint32_t(zenkit::MusicTransitionEffect::INTRO));
    static_assert(uint32_t(Def::MusicTransitionEffect::END)==uint32_t(zenkit::MusicTransitionEffect::END));
    static_assert(uint32_t(Def::MusicTransitionEffect::END_AND_INTO)==uint32_t(zenkit::MusicTransitionEffect::END_AND_INTO));
    static_assert(std::is_same_v<std::underlying_type_t<Def::MusicTransitionType>,std::underlying_type_t<zenkit::MusicTransitionType>>);
    static_assert(uint32_t(Def::MusicTransitionType::UNKNOWN)==uint32_t(zenkit::MusicTransitionType::UNKNOWN));
    static_assert(uint32_t(Def::MusicTransitionType::IMMEDIATE)==uint32_t(zenkit::MusicTransitionType::IMMEDIATE));
    static_assert(uint32_t(Def::MusicTransitionType::BEAT)==uint32_t(zenkit::MusicTransitionType::BEAT));
    static_assert(uint32_t(Def::MusicTransitionType::MEASURE)==uint32_t(zenkit::MusicTransitionType::MEASURE));
    static_assert(Def::FightAi::move_count==zenkit::IFightAi::move_count);
    static_assert(Def::EffectBase::user_string_count==zenkit::IEffectBase::user_string_count);
    definitionFields(zenkit::ICamera{},Def::Camera{},"Camera value initialization");
    definitionFields(zenkit::ISoundEffect{},Def::SoundEffect{},"SoundEffect value initialization");
    definitionFields(zenkit::IMusicTheme{},Def::MusicTheme{},"MusicTheme value initialization");
    definitionFields(zenkit::IParticleEffect{},Def::ParticleEffect{},"ParticleEffect value initialization");
    definitionFields(zenkit::IEffectBase{},Def::EffectBase{},"EffectBase value initialization");
    definitionFields(zenkit::IParticleEffectEmitKey{},Def::ParticleEffectEmitKey{},"ParticleEffectEmitKey value initialization");
    const auto index=ProgramData::readIndex(base/"programs");
    Gothic gothic;
    for(const std::string request:{"Fight.dat","Camera.dat","Sfx.dat","ParticleFx.dat","VisualFx.dat","Music.dat"}) {
      auto source=definitionSourceVm(index,request,gothic);
      require(Resources::writeCensus(),"cannot write definition source census");
      const auto before=json(censusPath);
      auto native=gothic.createPhoenixVm(request);
      require(Resources::writeCensus(),"cannot write native definition census");
      require(before["files"]==json(censusPath)["files"],request+": own definition VM added an original DAT open");
      const auto rejectRandom=[](int)->int {
        throw std::runtime_error("definition oracle encountered HLP_RANDOM; initializer parity requires investigation");
        };
      source->register_external("hlp_random",rejectRandom);
      native->register_external("hlp_random",rejectRandom);
      if(request=="Camera.dat") definitionInstances<zenkit::ICamera,Def::Camera>(*source,*native,"CCAMSYS");
      if(request=="Fight.dat") definitionFights(*source,*native);
      if(request=="Sfx.dat") definitionInstances<zenkit::ISoundEffect,Def::SoundEffect>(*source,*native,"C_SFX");
      if(request=="Music.dat") definitionInstances<zenkit::IMusicTheme,Def::MusicTheme>(*source,*native,"C_MusicTheme");
      if(request=="ParticleFx.dat") definitionInstances<zenkit::IParticleEffect,Def::ParticleEffect>(*source,*native,"C_PARTICLEFX");
      if(request=="VisualFx.dat") {
        definitionInstances<zenkit::IEffectBase,Def::EffectBase>(*source,*native,"CFX_BASE");
        definitionInstances<zenkit::IParticleEffectEmitKey,Def::ParticleEffectEmitKey>(*source,*native,"C_PARTICLEFXEMITKEY");
        }
      }
    }

  static void outputUnitFields(const std::shared_ptr<zenkit::Object>& a, const std::shared_ptr<asset::Object>& b,
                               std::map<const zenkit::Object*,const asset::Object*>& forward,
                               std::map<const asset::Object*,const zenkit::Object*>& reverse,
                               const std::string& field) {
    require(bool(a)==bool(b),field+": null object differs");
    if(!a) return;
    const auto [old,inserted]=forward.emplace(a.get(),b.get());
    const auto [own,unique]=reverse.emplace(b.get(),a.get());
    require(old->second==b.get() && own->second==a.get() && inserted==unique,field+": shared object identity differs");
    if(!inserted) return;
    switch(a->get_object_type()) {
      case zenkit::ObjectType::zCCSBlock: {
        require(b->get_object_type()==asset::ObjectType::zCCSBlock,field+": block class differs");
        const auto& x=static_cast<const zenkit::CutsceneBlock&>(*a);
        const auto& y=static_cast<const asset::CutsceneBlock&>(*b);
        modelField(x.name,y.name,field+".name");
        equal(x.block.index(),y.block.index(),field+".variant");
        if(x.block.index()==0)
          outputUnitFields(std::get<0>(x.block),std::get<0>(y.block),forward,reverse,field+".atomic");
        else
          outputUnitFields(std::get<1>(x.block),std::get<1>(y.block),forward,reverse,field+".nested");
        break;
        }
      case zenkit::ObjectType::zCCSAtomicBlock: {
        require(b->get_object_type()==asset::ObjectType::zCCSAtomicBlock,field+": atomic class differs");
        const auto& x=static_cast<const zenkit::CutsceneAtomicBlock&>(*a);
        const auto& y=static_cast<const asset::CutsceneAtomicBlock&>(*b);
        modelField(x.synchronized,y.synchronized,field+".synchronized");
        outputUnitFields(x.message,y.message,forward,reverse,field+".message");
        break;
        }
      case zenkit::ObjectType::oCMsgConversation: {
        require(b->get_object_type()==asset::ObjectType::oCMsgConversation,field+": message class differs");
        const auto& x=static_cast<const zenkit::ConversationMessageEvent&>(*a);
        const auto& y=static_cast<const asset::ConversationMessageEvent&>(*b);
        modelField(x.type,y.type,field+".type");
        modelField(x.text,y.text,field+".text bytes");
        modelField(x.name,y.name,field+".audio name bytes");
        modelField(x.high_priority,y.high_priority,field+".high_priority");
        modelField(x.used,y.used,field+".used");
        modelField(x.deleted,y.deleted,field+".deleted");
        break;
        }
      default: require(false,field+": unexpected original library class");
      }
    }

  static void outputUnits(const fs::path& base, const fs::path& censusPath) {
    const auto index=OutputUnits::readIndex(base/"outputunits");
    size_t libraries=0,roots=0,nodes=0,messages=0,probes=0,skipped=0;
    std::set<std::string> records;
    for(const auto& source:index.sources) {
      const auto field="OU "+source.origin+":"+source.path;
      std::unique_ptr<zenkit::Read> input;
      if(source.origin=="host") {
        require(source.archive.empty(),field+": host archive provenance");
        const auto path=fs::path(CommandLine::inst().rootPath())/source.path;
        input=zenkit::Read::from(path);
        Resources::censusFile(path.u16string());
        }
      else {
        require(source.origin=="archive" && !source.archive.empty(),field+": archive provenance");
        SourceObservation opened;
        input=readSource(fs::path(source.path).filename().string(),&opened);
        auto archive=opened.source;
        for(auto& c:archive) if(c>='a' && c<='z') c=char(c-'a'+'A');
        require(opened.node!=nullptr && opened.node==sourceVfs().resolve(source.path) && archive==source.archive,
                field+": selected mounted node/archive differs");
        }
      require(input!=nullptr,field+": original source cannot be opened");
      input->seek(0,zenkit::Whence::END);
      std::vector<std::byte> raw(input->tell());
      input->seek(0,zenkit::Whence::BEG);
      require(input->read(raw.data(),raw.size())==raw.size(),field+": incomplete original source");
      require(raw.size()==source.size &&
              BaseImport::sha256(std::string_view(reinterpret_cast<const char*>(raw.data()),raw.size()))==source.sha256,
              field+": original identity differs from imported provenance");
      if(!source.skipped.empty()) {
        require(source.record.empty() && source.recordSha256.empty(),field+": skipped source names a record");
        bool parsed=false;
        if(std::string_view(reinterpret_cast<const char*>(raw.data()),raw.size()).starts_with("ZenGin Archive"))
          try {
            auto oldReader=zenkit::Read::from(&raw);
            auto oldArchive=zenkit::ReadArchive::from(oldReader.get());
            parsed=bool(oldArchive->read_object<zenkit::CutsceneLibrary>(zenkit::GameVersion::GOTHIC_2));
            }
          catch(const std::exception&) {}
        require(!parsed,field+": importer skipped a source accepted by the independent original library reader");
        ++skipped;
        continue;
        }
      auto reader=zenkit::Read::from(&raw);
      auto archive=zenkit::ReadArchive::from(reader.get());
      zenkit::ArchiveObject object;
      require(archive->read_object_begin(object) && object.class_name=="zCCSLib",field+": original library root");
      const auto count=archive->read_int();
      require(count>=0,field+": negative root count");
      std::vector<std::shared_ptr<zenkit::CutsceneBlock>> ordered;
      for(int32_t i=0;i<count;++i) ordered.push_back(archive->read_object<zenkit::CutsceneBlock>(zenkit::GameVersion::GOTHIC_2));
      if(!archive->read_object_end()) archive->skip_object(true);
      // Independent old reader, including its exact platform std::sort and duplicate-name order.
      auto oldReader=zenkit::Read::from(&raw);
      auto oldArchive=zenkit::ReadArchive::from(oldReader.get());
      const auto original=oldArchive->read_object<zenkit::CutsceneLibrary>(zenkit::GameVersion::GOTHIC_2);
      require(original!=nullptr,field+": original library cannot be loaded");
      require(Resources::writeCensus(),"cannot write original OU census");
      const auto before=json(censusPath);
      std::string digest;
      const auto native=OutputUnits::readLibrary(index.path(source),&digest);
      require(Resources::writeCensus(),"cannot write native OU census");
      const auto after=json(censusPath);
      require(before["files"]==after["files"],field+": native decoding opened original data");
      require(digest==source.recordSha256 && index.records.at(source.record)==digest &&
              BaseImport::sha256File(index.path(source))==digest,field+": actual native JSON identity");
      auto name=fs::path(source.path).filename().string();
      for(auto& c:name) if(c>='a' && c<='z') c=char(c-'a'+'A');
      require(native.kind=="cutscene_library" && native.source.world==name && native.source.size==raw.size() &&
              native.source.crc32==worldChunkCrc32(raw.data(),raw.size()),field+": library source metadata");
      equal(ordered.size(),native.blocks.size(),field+" archive-order roots");
      std::map<const zenkit::Object*,const asset::Object*> forward;
      std::map<const asset::Object*,const zenkit::Object*> reverse;
      for(size_t i=0;i<ordered.size();++i)
        outputUnitFields(ordered[i],native.blocks[i],forward,reverse,field+" archive root "+std::to_string(i));
      nodes+=forward.size(); roots+=ordered.size();
      // Derive the server's raw message view independently from the original, with aliases visited once.
      std::vector<std::shared_ptr<zenkit::ConversationMessageEvent>> originalMessages;
      std::set<const zenkit::Object*> visited;
      std::function<void(const std::shared_ptr<zenkit::CutsceneBlock>&)> visit=[&](const auto& block) {
        if(!block || !visited.insert(block.get()).second) return;
        if(block->block.index()==1) { visit(std::get<1>(block->block)); return; }
        const auto& atomic=std::get<0>(block->block);
        if(!atomic || !visited.insert(atomic.get()).second || !atomic->message || !visited.insert(atomic->message.get()).second) return;
        originalMessages.push_back(atomic->message);
        };
      for(const auto& block:ordered) visit(block);
      const auto ownMessages=OutputUnits::messagesInArchiveOrder(native);
      equal(originalMessages.size(),ownMessages.size(),field+" archive-order messages");
      for(size_t i=0;i<originalMessages.size();++i) {
        require(forward.at(originalMessages[i].get())==ownMessages[i].get(),field+": server message order/alias differs");
        modelField(originalMessages[i]->name,ownMessages[i]->name,field+" server audio bytes");
        modelField(originalMessages[i]->text,ownMessages[i]->text,field+" server text bytes");
        }
      messages+=ownMessages.size();
      const auto sorted=OutputUnits::sortedBlocks(native);
      equal(original->blocks.size(),sorted.size(),field+" sorted roots");
      forward.clear(); reverse.clear();
      for(size_t i=0;i<sorted.size();++i)
        outputUnitFields(original->blocks[i],sorted[i],forward,reverse,field+" sorted root "+std::to_string(i));
      std::set<std::string> requests{""};
      for(size_t i=0;i<original->blocks.size();++i) {
        const auto& key=original->blocks[i]->name;
        requests.insert(key);
        // This is an absent byte-string strictly between adjacent names whenever such a gap exists.
        auto between=key+std::string(1,'\0');
        if(i+1<original->blocks.size() && between<original->blocks[i+1]->name) requests.insert(std::move(between));
        }
      if(!original->blocks.empty()) requests.insert(original->blocks.back()->name+std::string(1,char(0xff)));
      for(const auto& request:requests) {
        const auto a=original->block_by_name(request);
        const auto b=OutputUnits::blockByName(sorted,request);
        require(bool(a)==bool(b) && (!a || forward.at(a.get())==b.get()),field+": lower_bound successor lookup differs");
        if(a) {
          const auto x=a->get_message();
          const auto y=OutputUnits::message(*b);
          require(bool(x)==bool(y) && (!x || forward.at(x.get())==y.get()),field+": get_message differs");
          }
        ++probes;
        }
      records.insert(source.record);
      ++libraries;
      }
    require(Resources::writeCensus(),"cannot write OU selection baseline census");
    const auto beforeSelections=json(censusPath);
    for(const auto& row:index.selections) {
      if(!row.source) continue;
      const auto& source=index.sources.at(*row.source);
      if(!source.skipped.empty()) continue;
      const auto host=row.step.starts_with("host_") ?
        (fs::path(CommandLine::inst().rootPath())/source.path).u16string() : std::u16string{};
      std::vector<std::shared_ptr<asset::CutsceneBlock>> loaded;
      require(Resources::loadBaseOutputUnits(row.candidate,row.language,row.step,loaded,host),
              "OU indexed selection must load its native library");
      equal(loaded.size(),OutputUnits::readLibrary(index.path(source)).blocks.size(),"OU selected native root count");
      }
    require(Resources::writeCensus(),"cannot write OU selected-native census");
    const auto afterSelections=json(censusPath);
    require(beforeSelections["files"]==afterSelections["files"],"Resources native OU selections must open zero original sources");
    require(libraries>0,"OU corpus must contain parsed libraries");
    equal(records.size(),index.records.size(),"every indexed OU record must have an independently checked source");
    WorldData::ZenFile empty;
    empty.kind="cutscene_library";
    require(OutputUnits::sortedBlocks(empty).empty() && OutputUnits::messagesInArchiveOrder(empty).empty() &&
            !OutputUnits::blockByName({},"missing"),"empty OU library must stay empty");
    zenkit::CutsceneBlock oldNull;
    asset::CutsceneBlock ownNull;
    std::get<0>(oldNull.block)->message.reset();
    std::get<0>(ownNull.block)->message.reset();
    require(!oldNull.get_message() && !OutputUnits::message(ownNull),"null atomic message must stay null");
    oldNull.block=std::shared_ptr<zenkit::CutsceneBlock>{};
    ownNull.block=std::shared_ptr<asset::CutsceneBlock>{};
    require(!oldNull.get_message() && !OutputUnits::message(ownNull),"null nested block must stay null");
    std::printf("Output units: %zu original libraries, %zu skipped sources with verified SHA-256, %zu roots, %zu unique nodes, %zu raw messages, %zu successor lookups equal\n",
                libraries,skipped,roots,nodes,messages,probes);
    }

  template<class T>
  static void musicField(const T& a,const T& b,const std::string& field) {
    if constexpr(std::is_same_v<T,float>) equal(std::bit_cast<uint32_t>(a),std::bit_cast<uint32_t>(b),field+" bits");
    else if constexpr(std::is_same_v<T,double>) equal(std::bit_cast<uint64_t>(a),std::bit_cast<uint64_t>(b),field+" bits");
    else require(a==b,field+": values differ");
    }
  template<class T,size_t N>
  static void musicField(const T (&a)[N],const T (&b)[N],const std::string& field) {
    for(size_t i=0;i<N;++i) musicField(a[i],b[i],field+"["+std::to_string(i)+"]");
    }
  template<class T>
  static void musicField(const std::vector<T>& a,const std::vector<T>& b,const std::string& field) {
    equal(a.size(),b.size(),field+" count");
    for(size_t i=0;i<a.size();++i) musicField(a[i],b[i],field+"["+std::to_string(i)+"]");
    }
  template<class T>
  static void musicField(const std::shared_ptr<T>& a,const std::shared_ptr<T>& b,const std::string& field) {
    equal(bool(a),bool(b),field+" presence"); if(a) musicField(*a,*b,field);
    }
#define MUSIC_FIELD(member) musicField(a.member,b.member,field+"." #member)
  static void musicField(const Dx8::GUID& a,const Dx8::GUID& b,const std::string& field) { MUSIC_FIELD(Data1); MUSIC_FIELD(Data2); MUSIC_FIELD(Data3); MUSIC_FIELD(Data4); }
  static void musicField(const Dx8::DMUS_IO_VERSION& a,const Dx8::DMUS_IO_VERSION& b,const std::string& field) { MUSIC_FIELD(dwVersionMS); MUSIC_FIELD(dwVersionLS); }
  static void musicField(const Dx8::DMUS_IO_TIMESIG& a,const Dx8::DMUS_IO_TIMESIG& b,const std::string& field) { MUSIC_FIELD(bBeatsPerMeasure); MUSIC_FIELD(bBeat); MUSIC_FIELD(wGridsPerBeat); }
  static void musicField(const Dx8::DMUS_IO_TRACK_HEADER& a,const Dx8::DMUS_IO_TRACK_HEADER& b,const std::string& field) { MUSIC_FIELD(guidClassID); MUSIC_FIELD(dwPosition); MUSIC_FIELD(dwGroup); MUSIC_FIELD(ckid); MUSIC_FIELD(fccType); }
  static void musicField(const Dx8::DMUS_IO_CHORD& a,const Dx8::DMUS_IO_CHORD& b,const std::string& field) { MUSIC_FIELD(wszName); MUSIC_FIELD(mtTime); MUSIC_FIELD(wMeasure); MUSIC_FIELD(bBeat); MUSIC_FIELD(bFlags); }
  static void musicField(const Dx8::DMUS_IO_SUBCHORD& a,const Dx8::DMUS_IO_SUBCHORD& b,const std::string& field) { MUSIC_FIELD(dwChordPattern); MUSIC_FIELD(dwScalePattern); MUSIC_FIELD(dwInversionPoints); MUSIC_FIELD(dwLevels); MUSIC_FIELD(bChordRoot); MUSIC_FIELD(bScaleRoot); }
  static void musicField(const Dx8::DMUS_IO_COMMAND& a,const Dx8::DMUS_IO_COMMAND& b,const std::string& field) { MUSIC_FIELD(mtTime); MUSIC_FIELD(wMeasure); MUSIC_FIELD(bBeat); MUSIC_FIELD(bCommand); MUSIC_FIELD(bGrooveLevel); MUSIC_FIELD(bGrooveRange); MUSIC_FIELD(bRepeatMode); }
  static void musicField(const Dx8::DMUS_IO_REFERENCE& a,const Dx8::DMUS_IO_REFERENCE& b,const std::string& field) { MUSIC_FIELD(guidClassID); }
  static void musicField(const Dx8::DMUS_IO_STYLE& a,const Dx8::DMUS_IO_STYLE& b,const std::string& field) { MUSIC_FIELD(timeSig); MUSIC_FIELD(dblTempo); }
  static void musicField(const Dx8::DMUS_IO_PATTERN& a,const Dx8::DMUS_IO_PATTERN& b,const std::string& field) { MUSIC_FIELD(timeSig); MUSIC_FIELD(bGrooveBottom); MUSIC_FIELD(bGrooveTop); MUSIC_FIELD(wEmbellishment); MUSIC_FIELD(wNbrMeasures); }
  static void musicField(const Dx8::DMUS_IO_INSTRUMENT& a,const Dx8::DMUS_IO_INSTRUMENT& b,const std::string& field) { MUSIC_FIELD(dwPatch); MUSIC_FIELD(dwAssignPatch); MUSIC_FIELD(dwNoteRanges); MUSIC_FIELD(dwPChannel); MUSIC_FIELD(dwFlags); MUSIC_FIELD(bPan); MUSIC_FIELD(bVolume); }
  static void musicField(const Dx8::DMUS_IO_STYLEPART& a,const Dx8::DMUS_IO_STYLEPART& b,const std::string& field) { MUSIC_FIELD(timeSig); MUSIC_FIELD(dwVariationChoices); MUSIC_FIELD(guidPartID); MUSIC_FIELD(wNbrMeasures); MUSIC_FIELD(bPlayModeFlags); }
  static void musicField(const Dx8::DMUS_IO_PARTREF& a,const Dx8::DMUS_IO_PARTREF& b,const std::string& field) { MUSIC_FIELD(guidPartID); MUSIC_FIELD(wLogicalPartID); }
  static void musicField(const Dx8::DMUS_IO_STYLENOTE& a,const Dx8::DMUS_IO_STYLENOTE& b,const std::string& field) { MUSIC_FIELD(mtGridStart); MUSIC_FIELD(dwVariation); MUSIC_FIELD(mtDuration); MUSIC_FIELD(nTimeOffset); MUSIC_FIELD(wMusicValue); MUSIC_FIELD(bVelocity); MUSIC_FIELD(bTimeRange); MUSIC_FIELD(bDurRange); MUSIC_FIELD(bVelRange); MUSIC_FIELD(bInversionID); MUSIC_FIELD(bPlayModeFlags); MUSIC_FIELD(bNoteFlags); }
  static void musicField(const Dx8::DMUS_IO_STYLECURVE& a,const Dx8::DMUS_IO_STYLECURVE& b,const std::string& field) { MUSIC_FIELD(mtGridStart); MUSIC_FIELD(dwVariation); MUSIC_FIELD(mtDuration); MUSIC_FIELD(mtResetDuration); MUSIC_FIELD(nTimeOffset); MUSIC_FIELD(nStartValue); MUSIC_FIELD(nEndValue); MUSIC_FIELD(nResetValue); MUSIC_FIELD(bEventType); MUSIC_FIELD(bCurveShape); MUSIC_FIELD(bCCData); MUSIC_FIELD(bFlags); MUSIC_FIELD(wParamType); MUSIC_FIELD(wMergeIndex); }
  static void musicField(const Dx8::Info& a,const Dx8::Info& b,const std::string& field) { MUSIC_FIELD(inam); }
  static void musicField(const Dx8::Unfo& a,const Dx8::Unfo& b,const std::string& field) { MUSIC_FIELD(unam); }
  static void musicField(const Dx8::Reference& a,const Dx8::Reference& b,const std::string& field) { MUSIC_FIELD(header); MUSIC_FIELD(guid); MUSIC_FIELD(name); MUSIC_FIELD(file); MUSIC_FIELD(category); MUSIC_FIELD(version); }
  static void musicField(const Dx8::Band& a,const Dx8::Band& b,const std::string& field) { MUSIC_FIELD(guid); MUSIC_FIELD(vers); MUSIC_FIELD(info); MUSIC_FIELD(intrument); }
  static void musicField(const Dx8::Band::Instrument& a,const Dx8::Band::Instrument& b,const std::string& field) { MUSIC_FIELD(header); MUSIC_FIELD(reference); }
  static void musicField(const Dx8::Pattern& a,const Dx8::Pattern& b,const std::string& field) { MUSIC_FIELD(header); MUSIC_FIELD(info); MUSIC_FIELD(partref); }
  static void musicField(const Dx8::Pattern::PartRef& a,const Dx8::Pattern::PartRef& b,const std::string& field) { MUSIC_FIELD(io); MUSIC_FIELD(unfo); }
  static void musicField(const Dx8::Style& a,const Dx8::Style& b,const std::string& field) { MUSIC_FIELD(guid); MUSIC_FIELD(styh); MUSIC_FIELD(band); MUSIC_FIELD(parts); MUSIC_FIELD(patterns); }
  static void musicField(const Dx8::Style::Part& a,const Dx8::Style::Part& b,const std::string& field) { MUSIC_FIELD(header); MUSIC_FIELD(notes); MUSIC_FIELD(curves); }
  static void musicField(const Dx8::Track& a,const Dx8::Track& b,const std::string& field) { MUSIC_FIELD(head); MUSIC_FIELD(sttr); MUSIC_FIELD(cord); MUSIC_FIELD(cmnd); }
  static void musicField(const Dx8::Track::StyleRef& a,const Dx8::Track::StyleRef& b,const std::string& field) { MUSIC_FIELD(stmp); MUSIC_FIELD(reference); }
  static void musicField(const Dx8::Track::StyleTrack& a,const Dx8::Track::StyleTrack& b,const std::string& field) { MUSIC_FIELD(styles); }
  static void musicField(const Dx8::Track::Chord& a,const Dx8::Track::Chord& b,const std::string& field) { MUSIC_FIELD(header); MUSIC_FIELD(ioChord); MUSIC_FIELD(subchord); }
  static void musicField(const Dx8::Track::CommandTrack& a,const Dx8::Track::CommandTrack& b,const std::string& field) { MUSIC_FIELD(commands); }
  static void musicField(const Dx8::Segment& a,const Dx8::Segment& b,const std::string& field) { MUSIC_FIELD(track); }
  static void musicField(const Dx8::Wave& a,const Dx8::Wave& b,const std::string& field) { MUSIC_FIELD(wfmt); MUSIC_FIELD(extra); MUSIC_FIELD(wavedata); MUSIC_FIELD(waveSample); MUSIC_FIELD(loop); MUSIC_FIELD(info); }
  static void musicField(const Dx8::Wave::WaveFormat& a,const Dx8::Wave::WaveFormat& b,const std::string& field) { MUSIC_FIELD(wFormatTag); MUSIC_FIELD(wChannels); MUSIC_FIELD(dwSamplesPerSec); MUSIC_FIELD(dwAvgBytesPerSec); MUSIC_FIELD(wBlockAlign); MUSIC_FIELD(wBitsPerSample); }
  static void musicField(const Dx8::Wave::WaveSample& a,const Dx8::Wave::WaveSample& b,const std::string& field) { MUSIC_FIELD(cbSize); MUSIC_FIELD(usUnityNote); MUSIC_FIELD(sFineTune); MUSIC_FIELD(lAttenuation); MUSIC_FIELD(fulOptions); MUSIC_FIELD(cSampleLoops); }
  static void musicField(const Dx8::Wave::WaveSampleLoop& a,const Dx8::Wave::WaveSampleLoop& b,const std::string& field) { MUSIC_FIELD(cbSize); MUSIC_FIELD(ulLoopType); MUSIC_FIELD(ulLoopStart); MUSIC_FIELD(ulLoopLength); }
  static void musicField(const Dx8::DlsCollection::Parsed& a,const Dx8::DlsCollection::Parsed& b,const std::string& field) { MUSIC_FIELD(version); MUSIC_FIELD(dlid); MUSIC_FIELD(instrument); MUSIC_FIELD(wave); }
  static void musicField(const Dx8::DlsCollection::RgnRange& a,const Dx8::DlsCollection::RgnRange& b,const std::string& field) { MUSIC_FIELD(usLow); MUSIC_FIELD(usHigh); }
  static void musicField(const Dx8::DlsCollection::RegionHeader& a,const Dx8::DlsCollection::RegionHeader& b,const std::string& field) { MUSIC_FIELD(RangeKey); MUSIC_FIELD(RangeVelocity); MUSIC_FIELD(fusOptions); MUSIC_FIELD(usKeyGroup); }
  static void musicField(const Dx8::DlsCollection::WaveLink& a,const Dx8::DlsCollection::WaveLink& b,const std::string& field) { MUSIC_FIELD(fusOptions); MUSIC_FIELD(usPhaseGroup); MUSIC_FIELD(ulChannel); MUSIC_FIELD(ulTableIndex); }
  static void musicField(const Dx8::DlsCollection::WaveSample& a,const Dx8::DlsCollection::WaveSample& b,const std::string& field) { MUSIC_FIELD(cbSize); MUSIC_FIELD(usUnityNote); MUSIC_FIELD(sFineTune); MUSIC_FIELD(lAttenuation); MUSIC_FIELD(fulOptions); MUSIC_FIELD(cSampleLoops); }
  static void musicField(const Dx8::DlsCollection::WaveSampleLoop& a,const Dx8::DlsCollection::WaveSampleLoop& b,const std::string& field) { MUSIC_FIELD(cbSize); MUSIC_FIELD(ulLoopType); MUSIC_FIELD(ulLoopStart); MUSIC_FIELD(ulLoopLength); }
  static void musicField(const Dx8::DlsCollection::Region& a,const Dx8::DlsCollection::Region& b,const std::string& field) { MUSIC_FIELD(head); MUSIC_FIELD(wlink); MUSIC_FIELD(waveSample); MUSIC_FIELD(loop); }
  static void musicField(const Dx8::DlsCollection::ConnectionBlock& a,const Dx8::DlsCollection::ConnectionBlock& b,const std::string& field) { MUSIC_FIELD(usSource); MUSIC_FIELD(usControl); MUSIC_FIELD(usDestination); MUSIC_FIELD(usTransform); MUSIC_FIELD(lScale); }
  static void musicField(const Dx8::DlsCollection::Articulator& a,const Dx8::DlsCollection::Articulator& b,const std::string& field) { MUSIC_FIELD(connectionBlocks); }
  static void musicField(const Dx8::DlsCollection::MidiLocale& a,const Dx8::DlsCollection::MidiLocale& b,const std::string& field) { MUSIC_FIELD(ulBank); MUSIC_FIELD(ulInstrument); }
  static void musicField(const Dx8::DlsCollection::InstrumentHeader& a,const Dx8::DlsCollection::InstrumentHeader& b,const std::string& field) { MUSIC_FIELD(cRegions); MUSIC_FIELD(Locale); }
  static void musicField(const Dx8::DlsCollection::Instrument& a,const Dx8::DlsCollection::Instrument& b,const std::string& field) { MUSIC_FIELD(info); MUSIC_FIELD(header); MUSIC_FIELD(regions); MUSIC_FIELD(articulators); }
#undef MUSIC_FIELD

  static const char* musicResult(DmResult result) {
    switch(result) {
      case DmResult_SUCCESS: return "SUCCESS";
      case DmResult_INVALID_ARGUMENT: return "INVALID_ARGUMENT";
      case DmResult_INVALID_STATE: return "INVALID_STATE";
      case DmResult_MEMORY_EXHAUSTED: return "MEMORY_EXHAUSTED";
      case DmResult_NOT_FOUND: return "NOT_FOUND";
      case DmResult_FILE_CORRUPT: return "FILE_CORRUPT";
      case DmResult_MUTEX_ERROR: return "MUTEX_ERROR";
      }
    throw std::runtime_error("unknown original music result");
    }

  static std::vector<uint8_t> musicRecord(const MusicData::Index& index,const MusicData::Outcome& outcome) {
    require(!outcome.record.empty() && !outcome.failure,"successful music outcome must contain only its native record");
    const auto file=index.dir/(outcome.record+".json");
    auto bin=file; bin.replace_extension(".bin");
    const auto descriptor=gothicglb::readFile(file),payload=gothicglb::readFile(bin);
    Sha256 digest; digest.update(descriptor.data(),descriptor.size()); digest.update(payload.data(),payload.size());
    require(digest.finish()==outcome.identity && index.records.at(outcome.record)==outcome.identity,
            outcome.record+": actual JSON+BIN digest differs from native identity");
    const auto loaded=MusicData::readRecord(index,outcome.record);
    vector(payload,loaded,outcome.record+" verified BIN");
    return loaded;
    }

  template<class T>
  static void musicGkRecord(const MusicData::Index& index,const MusicData::Source& source,const std::vector<uint8_t>& raw,
                            DmResult (*parse)(const uint8_t*,size_t,T**),DmResult (*create)(const uint8_t*,size_t,T**),
                            DmResult (*encode)(const T*,uint8_t**,size_t*),void (*release)(T*)) {
    T* oldRaw=nullptr;
    const auto result=parse(raw.data(),raw.size(),&oldRaw);
    std::unique_ptr<T,void(*)(T*)> original(result==DmResult_SUCCESS ? oldRaw : nullptr,release);
    if(result!=DmResult_SUCCESS) {
      require(!oldRaw && source.gk.failure && *source.gk.failure==musicResult(result) && source.gk.record.empty(),
              source.path+": original GK parser failure differs from stored outcome");
      return;
      }
    require(!source.gk.failure,source.path+": successful original GK input was stored as failure");
    const auto bin=musicRecord(index,source.gk);
    auto borrowed=bin;
    T* nativeRaw=nullptr;
    const auto created=create(borrowed.data(),borrowed.size(),&nativeRaw);
    std::unique_ptr<T,void(*)(T*)> native(created==DmResult_SUCCESS ? nativeRaw : nullptr,release);
    require(created==DmResult_SUCCESS && nativeRaw,source.path+": native GK factory failed");
    std::fill(borrowed.begin(),borrowed.end(),0xdd); borrowed.clear(); borrowed.shrink_to_fit();
    // Supplemental C verification compares every original/native private field at this point.
    if constexpr(std::is_same_v<T,DmSegment>) {
      const auto* a=DmSegment_getName(original.get()); const auto* b=DmSegment_getName(native.get());
      equal(a!=nullptr,b!=nullptr,source.path+" GK segment name presence");
      if(a) require(std::string(a)==b,source.path+": GK segment name differs");
      bytes(DmSegment_getGuid(original.get())->data,DmSegment_getGuid(native.get())->data,16,source.path+" GK segment GUID");
      musicField(DmSegment_getLength(original.get()),DmSegment_getLength(native.get()),source.path+" GK segment length");
      equal(DmSegment_getRepeats(original.get()),DmSegment_getRepeats(native.get()),source.path+" GK segment repeats");
      }
    uint8_t* encoded=nullptr; size_t size=0;
    const auto saved=encode(native.get(),&encoded,&size);
    std::unique_ptr<uint8_t,decltype(&std::free)> owner(encoded,&std::free);
    require(saved==DmResult_SUCCESS && size==bin.size(),source.path+": GK native ownership/encoding failed");
    bytes(encoded,bin.data(),size,source.path+" native GK re-encoding after input destruction");
    // Encoding equality tests the public factory/ownership boundary; the independent C field oracle is separate.
    }

  template<class T,class Parse>
  static void musicDxRecord(const MusicData::Index& index,const MusicData::Source& source,const std::vector<uint8_t>& raw,
                            Parse parse,T (*decode)(const std::vector<uint8_t>&)) {
    std::optional<T> original; std::optional<std::string> failure;
    try { Dx8::Riff riff(raw.data(),raw.size()); original.emplace(parse(riff)); }
    catch(const std::exception& error) { failure=error.what(); }
    require(failure==source.dx8.failure,source.path+": original Dx8 outcome differs from stored failure (including empty text)");
    if(failure) return;
    auto native=decode(musicRecord(index,source.dx8));
    musicField(*original,native,source.path+" Dx8");
    }

  static void musicDlsRecord(const MusicData::Index& index,const MusicData::Source& source,const std::vector<uint8_t>& raw,
                             size_t& waveCount,size_t& sampleCount) {
    std::optional<Dx8::DlsCollection::Parsed> original; std::optional<std::string> failure;
    try {
      Dx8::Riff riff(raw.data(),raw.size()); original.emplace(Dx8::DlsCollection::parse(riff));
      auto finalized=*original;
      const Dx8::DlsCollection collection(std::move(finalized));
      }
    catch(const std::exception& error) { failure=error.what(); }
    require(failure==source.dx8.failure,source.path+": complete original Dx8 DLS constructor outcome differs");
    if(failure) return;
    auto native=MusicData::decodeDx8Dls(musicRecord(index,source.dx8));
    musicField(*original,native,source.path+" Dx8 DLS");
    for(size_t i=0;i<original->wave.size();++i) {
      const auto& a=original->wave[i]; const auto& b=native.wave[i];
      require(a.wfmt.wFormatTag==Dx8::Wave::PCM && a.wfmt.wBitsPerSample==16 && a.wavedata.size()%2==0,
              source.path+": unsupported original Dx8 decoded format");
      const size_t count=a.wavedata.size()/sizeof(int16_t);
      std::vector<float> oldSamples(count),nativeSamples(count);
      if(count!=0) { a.toFloatSamples(oldSamples.data()); b.toFloatSamples(nativeSamples.data()); }
      vector(oldSamples,nativeSamples,source.path+" wave["+std::to_string(i)+"] complete float bits");
      ++waveCount; sampleCount+=count;
      }
    std::optional<std::string> nativeFailure;
    try { const Dx8::DlsCollection finalized(std::move(native)); }
    catch(const std::exception& error) { nativeFailure=error.what(); }
    require(nativeFailure==failure,source.path+": native Dx8 DLS finalization differs from original");
    }

  using MusicFactoryContext=std::tuple<const MusicData::Index*,std::vector<std::string>*,std::exception_ptr*>;

  template<class T>
  static DmResult musicFactory(void* context,const char* name,T** out,const char* kind,
                               DmResult (*create)(const uint8_t*,size_t,T**)) {
    *out=nullptr;
    auto& [index,requests,failure]=*static_cast<MusicFactoryContext*>(context);
    try {
      requests->emplace_back(name);
      const auto* source=index->findGk(name);
      if(!source) return DmResult_NOT_FOUND;
      require(source->kind==kind,std::string(name)+": native factory selected the wrong kind");
      if(source->gk.failure) {
        for(int i=int(DmResult_INVALID_ARGUMENT);i<=int(DmResult_MUTEX_ERROR);++i)
          if(*source->gk.failure==musicResult(DmResult(i))) return DmResult(i);
        throw std::runtime_error("invalid stored GK failure");
        }
      const auto bin=musicRecord(*index,source->gk);
      return create(bin.data(),bin.size(),out);
      }
    catch(...) { *failure=std::current_exception(); return DmResult_FILE_CORRUPT; }
    }

  static void musicHistory(const MusicData::Index& index,zenkit::Vfs& originalVfs) {
    std::vector<std::string> originalRequests,nativeRequests;
    std::exception_ptr failure;
    using RawContext=std::tuple<zenkit::Vfs*,std::vector<std::string>*,std::exception_ptr*>;
    RawContext originalContext{&originalVfs,&originalRequests,&failure};
    const auto resolve=[](void* context,const char* name,size_t* size)->void* {
      auto& [vfs,requests,error]=*static_cast<RawContext*>(context);
      try {
        requests->emplace_back(name);
        const auto* node=vfs->find(name); if(!node) return nullptr;
        auto reader=node->open_read(); reader->seek(0,zenkit::Whence::END);
        std::vector<uint8_t> raw(reader->tell()); reader->seek(0,zenkit::Whence::BEG);
        require(reader->read(raw.data(),raw.size())==raw.size(),"short original GK history read");
        void* data=std::malloc(raw.empty() ? 1 : raw.size()); require(data!=nullptr,"GK history allocation failed");
        if(!raw.empty()) std::memcpy(data,raw.data(),raw.size());
        *size=raw.size(); return data;
        }
      catch(...) { *error=std::current_exception(); return nullptr; }
      };
    auto storedFailure=index;
    MusicFactoryContext nativeContext{&index,&nativeRequests,&failure};
    size_t fallbacks=0;
    DmLoader* a=nullptr; auto result=DmLoader_create(&a,DmLoader_DOWNLOAD);
    std::unique_ptr<DmLoader,decltype(&DmLoader_release)> original(result==DmResult_SUCCESS ? a : nullptr,&DmLoader_release);
    require(result==DmResult_SUCCESS,"original history loader creation failed");
    DmLoader* b=nullptr; result=DmLoader_create(&b,DmLoader_DOWNLOAD);
    std::unique_ptr<DmLoader,decltype(&DmLoader_release)> native(result==DmResult_SUCCESS ? b : nullptr,&DmLoader_release);
    require(result==DmResult_SUCCESS,"native history loader creation failed");
    require(DmLoader_addResolver(original.get(),resolve,&originalContext)==DmResult_SUCCESS,"original history resolver registration failed");
    DmLoaderFactory factory{
      [](void* ctx,const char* name,DmSegment** out) { return musicFactory(ctx,name,out,"segment",MusicNative_segment); },
      [](void* ctx,const char* name,DmStyle** out) { return musicFactory(ctx,name,out,"style",MusicNative_style); },
      [](void* ctx,const char* name,DmDls** out) { return musicFactory(ctx,name,out,"dls",MusicNative_dls); }};
    require(DmLoader_setFactory(native.get(),&factory,&nativeContext)==DmResult_SUCCESS,"native history factory registration failed");
    factory={}; // The API copies the table; a caller-owned temporary is not its storage.
    require(DmLoader_addResolver(native.get(),[](void* ctx,const char*,size_t*)->void* { ++*static_cast<size_t*>(ctx); return nullptr; },&fallbacks)==DmResult_SUCCESS,
            "native fallback observer registration failed");
    size_t roots=0;
    for(const auto& [request,id]:index.gk) {
      if(index.sources.at(id).kind!="segment") continue;
      const auto* node=originalVfs.find(request); require(node!=nullptr,"original history root missing");
      DmSegment* oldRaw=nullptr; const auto oldResult=DmLoader_getSegment(original.get(),node->name().c_str(),&oldRaw);
      std::unique_ptr<DmSegment,decltype(&DmSegment_release)> oldSegment(oldResult==DmResult_SUCCESS ? oldRaw : nullptr,&DmSegment_release);
      DmSegment* newRaw=nullptr; const auto newResult=DmLoader_getSegment(native.get(),node->name().c_str(),&newRaw);
      std::unique_ptr<DmSegment,decltype(&DmSegment_release)> newSegment(newResult==DmResult_SUCCESS ? newRaw : nullptr,&DmSegment_release);
      if(failure) std::rethrow_exception(failure);
      equal(int(oldResult),int(newResult),request+" persistent loader result");
      require(originalRequests==nativeRequests,request+": persistent GUID-cache request order/count differs");
      if(oldResult==DmResult_SUCCESS) {
        bytes(DmSegment_getGuid(oldSegment.get())->data,DmSegment_getGuid(newSegment.get())->data,16,request+" downloaded GUID");
        musicField(DmSegment_getLength(oldSegment.get()),DmSegment_getLength(newSegment.get()),request+" downloaded length");
        equal(DmSegment_getRepeats(oldSegment.get()),DmSegment_getRepeats(newSegment.get()),request+" downloaded repeats");
        // Supplemental C verification inspects downloaded references and both persistent caches here.
        }
      ++roots;
      }
    // Supplemental C verification probes NULL/empty/GUID-alias rules on these same live loaders here.
    for(auto* loader:{original.get(),native.get()}) {
      DmSegment* missing=nullptr;
      equal(int(DmLoader_getSegment(loader,"P16_HISTORY_MISSING.SGT",&missing)),int(DmResult_NOT_FOUND),"missing history segment");
      require(missing==nullptr,"missing history segment must not return an owner");
      }
    if(failure) std::rethrow_exception(failure);
    require(originalRequests==nativeRequests && fallbacks==0,"native factory must preserve request history without byte-resolver fallback");
    const auto root=std::find_if(storedFailure.gk.begin(),storedFailure.gk.end(),[&](const auto& entry) { return storedFailure.sources.at(entry.second).kind=="segment"; });
    require(root!=storedFailure.gk.end(),"stored failure check needs an actual segment");
    storedFailure.sources.at(root->second).gk={"","",std::string("FILE_CORRUPT")};
    std::get<0>(nativeContext)=&storedFailure;
    DmSegment* failed=nullptr;
    equal(int(DmLoader_getSegment(native.get(),root->first.c_str(),&failed)),int(DmResult_FILE_CORRUPT),"stored native parser failure");
    require(!failed && fallbacks==0,"stored native failure must not call the original resolver");
    require(DmLoader_setFactory(native.get(),nullptr,nullptr)==DmResult_SUCCESS,"clearing native factory failed");
    equal(int(DmLoader_getSegment(native.get(),"P16_HISTORY_MISSING.SGT",&failed)),int(DmResult_NOT_FOUND),"cleared factory resolver result");
    require(fallbacks==1,"cleared factory must restore the existing resolver path");
    native.reset(); original.reset(); // All callback contexts above remain live through release.
    std::printf("Music GK history: %zu roots, %zu ordered requests equal; native factory copied, misses/failures retained, no fallback\n",roots,originalRequests.size());
    }

  static void musicDxHistory(const MusicData::Index& index,const std::vector<std::u16string>& dirs,const fs::path& gothic) {
    std::vector<std::u16string> nativeRequests;
    const auto read=[&](const char16_t* name,const char* kind) {
      nativeRequests.emplace_back(name);
      for(const auto& dir:dirs) {
        const auto path=FileUtil::nestedPath(dir,{name},Tempest::Dir::FT_File);
        if(!FileUtil::exists(path)) continue;
        const auto relative=fs::path(path).lexically_relative(gothic).lexically_normal().generic_string();
        const auto* source=index.findDx8(relative);
        require(source!=nullptr && source->kind==kind,"native Dx8 history selected an unindexed/wrong-kind host file");
        if(source->dx8.failure) throw std::runtime_error(*source->dx8.failure);
        return musicRecord(index,source->dx8);
        }
      throw std::runtime_error("file not found");
      };
    Dx8::DirectMusic raw;
    Dx8::DirectMusic::Native callbacks{
      [&](const char16_t* name) { return MusicData::decodeDx8Segment(read(name,"segment")); },
      [&](const char16_t* name) { return MusicData::decodeDx8Style(read(name,"style")); },
      [&](const char16_t* name) { return MusicData::decodeDx8Dls(read(name,"dls")); }};
    Dx8::DirectMusic native(std::move(callbacks));
    for(const auto& dir:dirs) { raw.addPath(dir); native.addPath(dir); }
    std::set<std::u16string> roots;
    for(const auto& [path,id]:index.dx8) if(index.sources.at(id).kind=="segment") roots.insert(fs::path(path).filename().u16string());
    for(const auto& root:roots) {
      std::optional<Dx8::PatternList> a,b; std::optional<std::string> oldFailure,newFailure;
      try { a.emplace(raw.load(root.c_str())); } catch(const std::exception& error) { oldFailure=error.what(); }
      try { b.emplace(native.load(root.c_str())); } catch(const std::exception& error) { newFailure=error.what(); }
      const auto field=fs::path(root).string()+" persistent Dx8";
      require(oldFailure==newFailure,field+": original/native constructor outcome differs");
      if(!oldFailure) {
        equal(a->size(),b->size(),field+" pattern count");
        for(size_t i=0;i<a->size();++i) require((*a)[i].name==(*b)[i].name,field+": pattern order/name differs");
        }
      equal(raw.stlList().size(),native.stlList().size(),field+" style cache count");
      for(size_t i=0;i<raw.stlList().size();++i) {
        require(raw.stlList()[i].first==native.stlList()[i].first,field+": exact style cache key/order differs");
        musicField(raw.stlList()[i].second,native.stlList()[i].second,field+" cached style");
        }
      equal(raw.dlsCollection().size(),native.dlsCollection().size(),field+" DLS cache count");
      for(size_t i=0;i<raw.dlsCollection().size();++i) {
        const auto& x=*raw.dlsCollection()[i]; const auto& y=*native.dlsCollection()[i];
        require(x.first==y.first,field+": exact DLS cache key/order differs");
        musicField(x.second.version,y.second.version,field+" DLS version");
        musicField(x.second.dlid,y.second.dlid,field+" DLS GUID");
        musicField(x.second.instrument,y.second.instrument,field+" DLS instruments");
        }
      }
    require(!native.dlsCollection().empty(),"Dx8 history needs an actual cached bank");
    const auto file=native.dlsCollection().front()->first;
    const auto before=nativeRequests.size();
    require(&native.dlsCollection(file)==&native.dlsCollection(file),"native Dx8 DLS cache must preserve stable object identity");
    equal(nativeRequests.size(),before,"exact repeated Dx8 bank request must not reload a record");
    std::printf("Music Dx8 history: %zu roots; exact ordered style/DLS caches and pattern names equal; %zu native callbacks\n",roots.size(),nativeRequests.size());
    }

  static void musicIntegrity(const fs::path& base,const fs::path& temporary) {
    auto index=MusicData::readIndex(base/"music");
    require(!index.records.empty(),"music integrity check needs a native record");
    const auto [key,identity]=*index.records.begin(); (void)identity;
    const auto original=index.dir;
    index.dir=temporary/"music-integrity";
    const auto descriptor=index.dir/(key+".json"); auto payload=descriptor; payload.replace_extension(".bin");
    fs::create_directories(descriptor.parent_path());
    const auto source=original/(key+".json"); auto sourceBin=source; sourceBin.replace_extension(".bin");
    fs::create_symlink(source,descriptor); fs::create_symlink(sourceBin,payload);
    const auto descriptorBytes=gothicglb::readFile(source),payloadBytes=gothicglb::readFile(sourceBin);
    require(!payloadBytes.empty(),"music integrity fixture needs a nonempty native record");
    const auto rejects=[&](const char* label) {
      bool rejected=false;
      try { (void)MusicData::readRecord(index,key); }
      catch(const std::runtime_error& error) { rejected=std::string(error.what()).find("music data: "+key+":")!=std::string::npos; }
      require(rejected,std::string(label)+": native music record must fail explicitly");
      };
    fs::remove(descriptor); rejects("missing JSON"); fs::create_symlink(source,descriptor);
    fs::remove(payload); rejects("missing BIN");
    auto changed=payloadBytes; changed.push_back(0);
    gothicglb::writeFile(payload,changed.data(),changed.size()); rejects("resized BIN");
    changed=payloadBytes; changed.at(0)^=1;
    gothicglb::writeFile(payload,changed.data(),changed.size()); rejects("same-size corrupt BIN");
    fs::remove(payload); fs::create_symlink(sourceBin,payload);
    fs::remove(descriptor); auto changedJson=descriptorBytes; changedJson.push_back(' ');
    gothicglb::writeFile(descriptor,changedJson.data(),changedJson.size()); rejects("changed actual JSON identity");
    fs::remove(descriptor); fs::create_symlink(source,descriptor);
    index.records.erase(key); rejects("missing record index entry");
    }

  static void musicRuntime(const fs::path& base,const fs::path& temporary,const fs::path& censusPath) {
    const auto fixture=temporary/"music-runtime-base";
    fs::create_directories(fixture);
    for(const auto* part:{"base.json","base-files.json","worlds","textures","meshes","zen","models","fonts","programs","outputunits","audio","sources"})
      fs::create_symlink(base/part,fixture/part);
    fs::copy(base/"music",fixture/"music",fs::copy_options::recursive);
    setenv("OPENGOTHIC_BASE",fixture.c_str(),1);
    MusicData::Source selected;
    {
    Resources resources; Resources::loadBase();
    require(Resources::isHeadless(),"private music reader checks must remain CPU-only");
    const auto found=std::find_if(resources.baseMusic.sources.begin(),resources.baseMusic.sources.end(),[&](const auto& source) {
      return source.origin=="host" && source.kind=="segment" && !source.gk.failure && !source.dx8.failure &&
             resources.baseMusic.findGk(fs::path(source.path).filename().string())==&source;
      });
    require(found!=resources.baseMusic.sources.end(),"music runtime needs an actual host segment selected by both providers");
    selected=*found;
    const auto request=fs::path(selected.path).filename().string(); const auto name=fs::path(request).u16string();
    const auto gothic=fs::path(CommandLine::inst().rootPath());
    const std::vector<std::u16string> dirs{(gothic/fs::path(selected.path).parent_path()).u16string()+u"/"};
    require(Resources::loadMusicSegment(request.c_str())==nullptr,"headless public GK music must remain empty");
    (void)Resources::loadDxMusic(request); // Its legacy empty PatternList has no safe size() query.
    std::vector<uint8_t> bin;
    require(resources.readGkMusic(request,"segment",bin)==DmResult_SUCCESS,"private GK reader must verify native record");
    vector(bin,musicRecord(resources.baseMusic,selected.gk),"private GK record bytes");
    vector(resources.readDxMusic(dirs,name.c_str(),"segment"),musicRecord(resources.baseMusic,selected.dx8),"private Dx8 record bytes");
    const auto nativeIndex=resources.baseMusic;
    resources.baseMusic.gk.erase(resources.baseMusic.gk.find(upperMusic(request)));
    bin={1}; require(resources.readGkMusic(request,"segment",bin)==DmResult_NOT_FOUND && bin.empty(),"missing GK index entry must stay missing");
    resources.baseMusic=nativeIndex; resources.baseMusic.dx8.erase(selected.path);
    bool unindexed=false;
    try { (void)resources.readDxMusic(dirs,name.c_str(),"segment"); }
    catch(const std::runtime_error& error) { unindexed=std::string(error.what())=="file not found"; }
    require(unindexed,"missing Dx8 index entry must fail without source fallback");
    resources.baseMusic=nativeIndex;
    const auto id=resources.baseMusic.gk.at(upperMusic(request));
    resources.baseMusic.sources[id].gk={"","",std::string("FILE_CORRUPT")};
    bin={1}; require(resources.readGkMusic(request,"segment",bin)==DmResult_FILE_CORRUPT && bin.empty(),"stored GK failure must remain exact and clear output");
    resources.baseMusic.sources[id].dx8={"","",std::string{}};
    bool emptyFailure=false;
    try { (void)resources.readDxMusic(dirs,name.c_str(),"segment"); }
    catch(const std::runtime_error& error) { emptyFailure=std::string(error.what()).empty(); }
    require(emptyFailure,"stored empty Dx8 error must remain a failure without raw fallback");
    resources.baseMusic=nativeIndex;
    for(size_t provider=0;provider<2;++provider) {
      const auto& outcome=provider==0 ? selected.gk : selected.dx8;
      const auto file=fixture/"music"/(outcome.record+".bin"); const auto original=base/"music"/(outcome.record+".bin");
      const auto rejects=[&] {
        if(provider==0) { bin={1}; require(resources.readGkMusic(request,"segment",bin)==DmResult_FILE_CORRUPT && bin.empty(),"broken native GK record must fail and clear output"); }
        else {
          bool failed=false; try { (void)resources.readDxMusic(dirs,name.c_str(),"segment"); } catch(const std::runtime_error&) { failed=true; }
          require(failed,"broken native Dx8 record must fail without raw fallback");
          }
        };
      fs::remove(file); rejects();
      auto corrupt=gothicglb::readFile(original); require(!corrupt.empty(),"music runtime corruption fixture must be nonempty"); corrupt.at(0)^=1;
      gothicglb::writeFile(file,corrupt.data(),corrupt.size()); rejects();
      }
    }
    const auto census=json(censusPath);
    require(census["files"].Empty(),"native private music readers must make zero original Resource opens");
    const auto& music=census["runtime_music"];
    require(std::string(music["coverage"].GetString())=="verified_base_record_reads" && !music["complete_corpus"].GetBool(),"music census scope must describe verified native reads");
    equal(music["selections"].Size(),2u,"successful private music reads");
    equal(census["base_misses"].Size(),5u,"native music integrity/index misses");
    for(const auto& row:music["selections"].GetArray()) {
      const auto& outcome=std::string_view(row["provider"].GetString())=="gk" ? selected.gk : selected.dx8;
      require(std::string(row["kind"].GetString())=="segment" && gothicglb::jsonToString(row["request"],"request")==fs::path(selected.path).filename().string(),"native music census request/kind");
      require(std::string(row["record"]["json"].GetString())=="base/music/"+outcome.record+".json" &&
              std::string(row["record"]["bin"].GetString())=="base/music/"+outcome.record+".bin" && row["record"]["sha256"].GetString()==outcome.identity,
              "music census must report actual verified JSON+BIN identity");
      const auto& source=row["imported_source"];
      require(std::string(source["origin"].GetString())==selected.origin && gothicglb::jsonToString(source["path"],"path")==selected.path &&
              source["archive"].IsNull() && source["size"].GetUint64()==selected.size && source["sha256"].GetString()==selected.sha256,
              "music census must keep imported original provenance distinct");
      }
    equal(census["replacements"].Size(),2u,"private verified music replacement count");
    for(const auto& row:census["replacements"].GetArray()) require(std::string(row["kind"].GetString())=="basemusic","native music replacement kind");
    setenv("OPENGOTHIC_BASE",base.c_str(),1);
    }

  static std::string upperMusic(std::string text) {
    for(auto& c:text) if(c>='a' && c<='z') c=char(c-'a'+'A');
    return text;
    }

  static void musicCorpus(const fs::path& base) {
    const auto index=MusicData::readIndex(base/"music");
    const auto metadata=json(base/"sources/sources.json");
    const auto upper=[](std::string text) { for(auto& c:text) if(c>='a' && c<='z') c=char(c-'a'+'A'); return text; };
    const auto legacy=[](const rapidjson::Value& value,const char* name) { return gothicglb::jsonToString(value,name); };
    zenkit::Vfs oracle; std::string opened;
    oracle.set_open_observer([&](std::string_view,std::string_view source) { opened=source; });
    const auto gothic=fs::path(CommandLine::inst().rootPath()); fs::path work;
    const auto archives=BaseImport::mountRuntime(oracle,gothic,work);
    BaseImport::Files mounted; for(const auto& archive:archives) mounted.emplace_back(archive.name,archive.size);
    std::sort(mounted.begin(),mounted.end()); require(mounted==index.archives,"music archive order/identity differs from original mount");
    std::map<const zenkit::VfsNode*,std::string> paths;
    std::set<std::string> requests;
    std::function<void(const zenkit::VfsNode&,const std::string&)> collect=[&](const auto& node,const auto& path) {
      if(node.type()==zenkit::VfsNodeType::DIRECTORY) {
        for(const auto& child:node.children()) collect(child,path.empty() ? child.name() : path+"/"+child.name());
        return;
        }
      paths.emplace(&node,path);
      const auto ext=upper(fs::path(node.name()).extension().string());
      if(ext==".SGT" || ext==".STY" || ext==".DLS") requests.insert(upper(node.name()));
      };
    collect(oracle.root(),"");
    using Key=std::tuple<std::string,std::string,std::string>;
    std::map<Key,size_t> sources;
    for(size_t i=0;i<index.sources.size();++i) {
      const auto& source=index.sources[i];
      require(sources.emplace(Key{source.origin,source.path,source.archive},i).second,"music index duplicates a selected source");
      }
    std::set<size_t> seen;
    size_t gkSuccess=0,gkFailures=0,dxSuccess=0,dxFailures=0,waves=0,samples=0;
    const auto selected=[&](const std::string& path,const std::string& origin,const std::string& archive,const std::vector<uint8_t>& raw) {
      const auto found=sources.find(Key{origin,path,archive});
      require(found!=sources.end(),path+": independently selected original missing from native music index");
      const size_t id=found->second; const auto& source=index.sources.at(id);
      const auto digest=BaseImport::sha256(std::string_view(reinterpret_cast<const char*>(raw.data()),raw.size()));
      require(source.size==raw.size() && source.sha256==digest,path+": native source provenance differs from actual original bytes");
      const auto ext=upper(fs::path(path).extension().string());
      const auto kind=ext==".SGT" ? "segment" : ext==".STY" ? "style" : "dls";
      require(source.kind==kind,path+": original extension and native source kind differ");
      bool oldFound=false;
      for(const auto& row:metadata["music"]["corpus"].GetArray())
        if(legacy(row["source"],"source")== (origin=="host" ? path : archive) &&
           (origin=="host" || legacy(row["path"],"path")==path)) {
          require(row["size"].GetUint64()==raw.size() && row["sha256"].GetString()==digest,path+": original differs from P10 metadata"); oldFound=true;
          }
      require(oldFound,path+": selected music source missing from preserved P10 corpus");
      if(!seen.insert(id).second) return id;
      size_t badOffset=0;
      require(MusicNative_checkGkStrings(raw.data(),raw.size(),&badOffset)==DmResult_SUCCESS,path+": unsupported original GK string at "+std::to_string(badOffset));
      if(source.kind=="segment") {
        musicGkRecord(index,source,raw,MusicNative_parseSegment,MusicNative_segment,MusicNative_encodeSegment,MusicNative_releaseSegment);
        musicDxRecord<Dx8::Segment>(index,source,raw,[](Dx8::Riff& riff) { return Dx8::Segment(riff); },MusicData::decodeDx8Segment);
        }
      else if(source.kind=="style") {
        musicGkRecord(index,source,raw,MusicNative_parseStyle,MusicNative_style,MusicNative_encodeStyle,MusicNative_releaseStyle);
        musicDxRecord<Dx8::Style>(index,source,raw,[](Dx8::Riff& riff) { return Dx8::Style(riff); },MusicData::decodeDx8Style);
        }
      else {
        musicGkRecord(index,source,raw,MusicNative_parseDls,MusicNative_dls,MusicNative_encodeDls,MusicNative_releaseDls);
        musicDlsRecord(index,source,raw,waves,samples);
        }
      gkSuccess+=!source.gk.failure; gkFailures+=source.gk.failure.has_value();
      dxSuccess+=!source.dx8.failure; dxFailures+=source.dx8.failure.has_value();
      return id;
      };
    equal(requests.size(),index.gk.size(),"every original GK music request must be indexed");
    for(const auto& request:requests) {
      const auto* node=oracle.find(request); require(node!=nullptr,request+": independent GK lookup failed");
      opened.clear(); auto reader=node->open_read(); reader->seek(0,zenkit::Whence::END);
      std::vector<uint8_t> raw(reader->tell()); reader->seek(0,zenkit::Whence::BEG);
      require(reader->read(raw.data(),raw.size())==raw.size(),request+": short original music read");
      const bool host=fs::path(opened).is_absolute();
      const auto path=host ? fs::path(opened).lexically_relative(gothic).lexically_normal().generic_string() : paths.at(node);
      const size_t id=selected(path,host ? "host" : "archive",host ? "" : upper(opened),raw);
      require(index.gk.at(request)==id && index.findGk(node->name()+" \t")==&index.sources[id],request+": native GK selection/source index differs");
      }
    std::vector<std::u16string> dirs;
    for(const auto* dir:{u"newworld",u"AddonWorld",u"dungeon",u"menu_men",u"orchestra"})
      dirs.push_back(CommandLine::inst().nestedPath({u"_work",u"Data",u"Music",dir},Tempest::Dir::FT_Dir));
    dirs.push_back(CommandLine::inst().nestedPath({u"_work",u"Data",u"Music"},Tempest::Dir::FT_Dir));
    std::set<std::string> hostPaths;
    for(const auto& dir:dirs) Tempest::Dir::scan(dir,[&](const std::u16string& name,Tempest::Dir::FileType type) {
      if(type!=Tempest::Dir::FT_File) return;
      const auto ext=upper(fs::path(name).extension().string()); if(ext!=".SGT" && ext!=".STY" && ext!=".DLS") return;
      const auto file=fs::path(dir)/name;
      const auto path=file.lexically_relative(gothic).lexically_normal().generic_string();
      hostPaths.insert(path);
      const size_t id=selected(path,"host","",gothicglb::readFile(file));
      require(index.dx8.at(path)==id && index.findDx8(upper(path))==&index.sources[id],path+": native Dx8 selected source index differs");
      });
    equal(index.dx8.size(),hostPaths.size(),"all independent Dx8 host selections must be indexed");
    equal(seen.size(),index.sources.size(),"all native music sources must match an original provider selection");
    BaseImport::Files loose;
    for(const auto& source:index.sources) if(source.origin=="host") loose.emplace_back(source.path,source.size);
    std::sort(loose.begin(),loose.end()); require(loose==index.looseFiles,"music loose source identity differs");
    require(!index.findGk("P16_MISSING.SGT") && !index.findGk("prefix/NW_DayStd.sgt") && !index.findDx8("_work/P16_MISSING.SGT"),
            "missing/exact-name music lookups must stay missing");
    std::set<fs::path> expected{fs::path("music.json")};
    std::set<std::string> referenced;
    for(const auto& source:index.sources) for(const auto* outcome:{&source.gk,&source.dx8}) if(!outcome->record.empty()) referenced.insert(outcome->record);
    equal(referenced.size(),index.records.size(),"music index must contain exactly the selected records");
    for(const auto& [key,identity]:index.records) { (void)identity; expected.insert(key+".json"); expected.insert(key+".bin"); }
    std::set<fs::path> actual;
    for(const auto& file:fs::recursive_directory_iterator(index.dir)) if(file.is_regular_file()) actual.insert(file.path().lexically_relative(index.dir));
    require(actual==expected,"native music record file set differs from index");
    musicHistory(index,oracle);
    musicDxHistory(index,dirs,gothic);
    std::printf("Music corpus: %zu original sources; GK %zu parsed/%zu stored failures (public factory/ownership); Dx8 %zu parsed/%zu stored failures; %zu complete waves/%zu float samples equal\n",
                seen.size(),gkSuccess,gkFailures,dxSuccess,dxFailures,waves,samples);
    }

  static void audioCorpus(const fs::path& base) {
    const auto index=AudioData::readIndex(base/"audio");
    const auto nativeJson=json(base/"audio/audio.json");
    const auto sources=json(base/"sources/sources.json");
    std::map<std::string,const rapidjson::Value*> sourceRows;
    for(const auto& row:sources["files"].GetArray()) sourceRows.emplace(gothicglb::jsonToString(row["request"],"request"),&row);
    const auto upper=[](std::string s) {
      for(auto& c:s) if(c>='a' && c<='z') c=char(c-'a'+'A');
      return s;
      };
    zenkit::Vfs oracle;
    std::string opened;
    oracle.set_open_observer([&](std::string_view,std::string_view source) { opened=source; });
    const auto gothic=fs::path(CommandLine::inst().rootPath());
    fs::path work;
    const auto archives=BaseImport::mountRuntime(oracle,gothic,work);
    BaseImport::Files mounted;
    for(const auto& archive:archives) mounted.emplace_back(archive.name,archive.size);
    std::sort(mounted.begin(),mounted.end());
    require(mounted==index.archives,"audio archives differ from independent runtime mount");
    std::set<std::string> requests;
    std::map<const zenkit::VfsNode*,std::string> paths;
    std::function<void(const zenkit::VfsNode&,const std::string&)> collect=[&](const auto& node,const auto& path) {
      if(node.type()==zenkit::VfsNodeType::DIRECTORY) {
        for(const auto& child:node.children()) collect(child,path.empty() ? child.name() : path+"/"+child.name());
        return;
        }
      paths.emplace(&node,path);
      const auto extension=upper(fs::path(node.name()).extension().string());
      if(extension==".WAV" || extension==".OGG") requests.insert(upper(node.name()));
      };
    collect(oracle.root(),"");
    equal(requests.size(),index.sounds.size(),"every selected WAV/OGG request must be indexed");
    require(!requests.empty(),"audio corpus must contain selected requests");
    const auto decode=[](const std::vector<uint8_t>& bytes) -> std::pair<Tempest::Sound,std::string> {
      try {
        Tempest::MemReader reader(bytes.data(),bytes.size());
        return {Tempest::Sound(reader),std::string{}};
        }
      catch(const std::exception& error) { return {Tempest::Sound{},std::string(typeid(error).name())+": "+error.what()}; }
      };
    std::map<std::string,std::array<uint64_t,8>> totals; // requests, bytes, decoded16, samples, frames, milliseconds, empty, failures
    std::map<std::string,Sha256> originalSamples,nativeSamples;
    std::map<std::string,std::string> payloadFiles;
    size_t speech=0;
    uint64_t totalBytes=0,uniqueBytes=0;
    for(const auto& request:requests) {
      const auto found=index.sounds.find(request);
      require(found!=index.sounds.end(),request+": native audio request missing");
      const auto& sound=found->second;
      const auto* winner=oracle.find(request);
      require(winner!=nullptr,request+": independent VFS winner missing");
      opened.clear();
      auto input=winner->open_read();
      input->seek(0,zenkit::Whence::END);
      std::vector<uint8_t> raw(input->tell());
      input->seek(0,zenkit::Whence::BEG);
      require(input->read(raw.data(),raw.size())==raw.size(),request+": incomplete original audio read");
      const bool host=fs::path(opened).is_absolute();
      const auto path=host ? fs::path(opened).lexically_relative(gothic).lexically_normal().generic_string() : paths.at(winner);
      require(sound.origin==(host ? "host" : "archive") && sound.path==path &&
              sound.archive==(host ? std::string{} : upper(opened)),request+": actual selected origin/path/archive differs");
      const auto digest=BaseImport::sha256(std::string_view(reinterpret_cast<const char*>(raw.data()),raw.size()));
      require(sound.size==raw.size() && sound.sourceSha256==digest && sound.payload==digest,
              request+": original size/SHA-256 differs from payload or imported provenance");
      std::string cls="sound";
      for(const auto& part:fs::path(path)) if(upper(part.generic_string())=="SPEECH") cls="speech";
      require(sound.cls==cls,request+": audio class differs from the selected source path");
      speech+=cls=="speech";
      const auto old=sourceRows.find(request);
      require(old!=sourceRows.end(),request+": preserved P10 source manifest lacks audio request");
      const auto& row=*old->second;
      require(row["size"].GetUint64()==raw.size() && row["sha256"].GetString()==digest &&
              gothicglb::jsonToString(row["path"],"path")==paths.at(winner) &&
              gothicglb::jsonToString(row["source"],"source")==(host ? path : sound.archive),
              request+": selected audio differs from the preserved P10 source oracle");
      const auto& header=nativeJson["sounds"][row["request"].GetString()]["header"];
      require(header==row["audio"],request+": native header facts differ from the preserved source header");
      auto group=std::string(header["container"].GetString());
      if(header.HasMember("format_tag") && header["format_tag"].IsUint()) group+="/tag="+std::to_string(header["format_tag"].GetUint());
      if(header.HasMember("bits_per_sample")) group+="/bits="+std::to_string(header["bits_per_sample"].GetUint());
      const auto [file,first]=payloadFiles.emplace(sound.payload,sound.file);
      require(file->second==sound.file,request+": identical audio bytes must share one payload file");
      if(first) {
        const auto extension=upper(fs::path(request).extension().string())==".OGG" ? ".ogg" : ".wav";
        require(sound.file==digest.substr(0,2)+"/"+digest+extension,request+": first request must deterministically name its payload");
        uniqueBytes+=raw.size();
        }
      totalBytes+=raw.size();
      std::vector<uint8_t> payload;
      AudioData::readPayload(index,sound,request,payload);
      require(raw==payload,request+": native payload bytes differ from the actual VFS winner");
      require(BaseImport::sha256(std::string_view(reinterpret_cast<const char*>(payload.data()),payload.size()))==digest,
              request+": actual native payload SHA-256 differs");
      // Both byte buffers and owning Sounds stay alive until all sample comparisons/hashes end.
      auto original=decode(raw),native=decode(payload);
      require(original.second==native.second,request+": decoder exception outcome differs");
      auto& stats=totals[group];
      ++stats[0]; stats[1]+=raw.size();
      if(!original.second.empty()) {
        ++stats[7];
        std::printf("Audio decoder outcome: %s: %s\n",request.c_str(),original.second.c_str());
        continue;
        }
      equal(original.first.isEmpty(),native.first.isEmpty(),request+" decoder empty outcome");
      equal(original.first.timeLength(),native.first.timeLength(),request+" returned duration");
      stats[5]+=original.first.timeLength();
      size_t aCount=0,bCount=0;
      uint16_t aChannels=0,bChannels=0;
      uint32_t aRate=0,bRate=0;
      const auto* a=original.first.samples16(aCount,aChannels,aRate);
      const auto* b=native.first.samples16(bCount,bChannels,bRate);
      equal(aCount,bCount,request+" interleaved int16 sample count");
      require(bool(a)==bool(b),request+": decoded sample availability differs");
      if(original.first.isEmpty()) { ++stats[6]; continue; }
      require(a!=nullptr,request+": nonempty decoded sound has no 16-bit sample buffer (unsupported corpus)");
      equal(aChannels,bChannels,request+" decoded channels");
      equal(aRate,bRate,request+" decoded sample rate");
      require(aChannels>0,request+": decoded sample channel count is zero");
      if(aCount!=0) {
        bytes(a,b,aCount*sizeof(int16_t),request+" complete decoded samples");
        originalSamples[group].update(a,aCount*sizeof(int16_t));
        nativeSamples[group].update(b,bCount*sizeof(int16_t));
        }
      ++stats[2]; stats[3]+=aCount; stats[4]+=aCount/aChannels;
      }
    equal(payloadFiles.size(),nativeJson["payloads"].MemberCount(),"every native audio payload must be selected");
    std::printf("Audio corpus: %zu selected requests (%zu speech), %zu unique payloads, %llu selected bytes, %llu unique bytes\n",
                requests.size(),speech,payloadFiles.size(),static_cast<unsigned long long>(totalBytes),static_cast<unsigned long long>(uniqueBytes));
    for(const auto& [group,stats]:totals) {
      const auto sourceHash=originalSamples[group].finish(),nativeHash=nativeSamples[group].finish();
      require(sourceHash==nativeHash,group+": complete decoded sample SHA-256 differs");
      std::printf("Audio %s: %llu requests, %llu bytes, %llu decoded16, %llu interleaved samples, %llu frames, %llu ms, %llu empty, %llu exceptions; samples SHA-256 %s\n",
                  group.c_str(),static_cast<unsigned long long>(stats[0]),static_cast<unsigned long long>(stats[1]),
                  static_cast<unsigned long long>(stats[2]),static_cast<unsigned long long>(stats[3]),
                  static_cast<unsigned long long>(stats[4]),static_cast<unsigned long long>(stats[5]),
                  static_cast<unsigned long long>(stats[6]),static_cast<unsigned long long>(stats[7]),sourceHash.c_str());
      }
    }

  static void audioRuntime(const fs::path& base, const fs::path& temporary, const fs::path& censusPath) {
    // Start with a valid base before adding the synthetic audio integrity fixtures.
    const auto fixtureBase=temporary/"audio-runtime-base";
    fs::create_directories(fixtureBase/"audio");
    for(const auto* part:{"base.json","base-files.json","worlds","textures","meshes","zen","models","fonts","programs","outputunits","music","sources"})
      fs::create_symlink(base/part,fixtureBase/part);
    fs::copy(base/"audio",fixtureBase/"audio",fs::copy_options::recursive);
    setenv("OPENGOTHIC_BASE",fixtureBase.c_str(),1);
    Resources resources;
    Resources::loadBase();
    Gothic gothic;
    require(Resources::isHeadless(),"audio integration must remain CPU-only");
    const auto pcm=[](uint16_t channels,const std::vector<int16_t>& samples) {
      std::vector<uint8_t> data{
        'R','I','F','F',0,0,0,0,'W','A','V','E','f','m','t',' ',16,0,0,0,1,0,0,0,
        0xE8,0x03,0,0,0,0,0,0,0,0,16,0,'d','a','t','a',0,0,0,0};
      data[22]=uint8_t(channels);
      const uint32_t rate=1000*channels*2;
      for(size_t i=0;i<4;++i) {
        data[4+i]=uint8_t((36+2*samples.size())>>(8*i));
        data[28+i]=uint8_t(rate>>(8*i));
        data[40+i]=uint8_t((2*samples.size())>>(8*i));
        }
      data[32]=uint8_t(2*channels);
      for(const auto sample:samples) {
        data.push_back(uint8_t(uint16_t(sample)));
        data.push_back(uint8_t(uint16_t(sample)>>8));
        }
      return data;
      };
    const auto nativePcm=pcm(2,{-32768,32767,-1,1,4660,-4660});
    const auto externalPcm=pcm(1,{123,-456});
    const auto zeroPcm=pcm(2,{});
    const std::vector<uint8_t> tiny={'b','a','d'};
    resources.baseAudio={};
    resources.baseAudio.dir=fixtureBase/"audio";
    const auto add=[&](const char* request,const std::vector<uint8_t>& raw,const char* cls,const char* path) {
      AudioData::Sound sound;
      sound.payload=BaseImport::sha256(std::string_view(reinterpret_cast<const char*>(raw.data()),raw.size()));
      sound.file=sound.payload.substr(0,2)+"/"+sound.payload+".wav";
      sound.size=raw.size(); sound.cls=cls; sound.origin="archive"; sound.path=path; sound.archive="P15.VDF";
      sound.sourceSha256=sound.payload;
      const auto file=resources.baseAudio.dir/sound.file;
      fs::create_directories(file.parent_path());
      gothicglb::writeFile(file,raw.data(),raw.size());
      resources.baseAudio.sounds.emplace(request,std::move(sound));
      };
    add("P15_PCM.WAV",nativePcm,"sound","P15_PCM.WAV");
    add("P15_SPEECH.WAV",nativePcm,"speech","SPEECH/P15_SPEECH.WAV");
    add("P15_ZERO.WAV",zeroPcm,"sound","P15_ZERO.WAV");
    add("P15_TINY.WAV",tiny,"sound","P15_TINY.WAV");
    auto previousExternal=std::move(gothic.externalSoundFn);
    require(Resources::writeCensus(),"cannot write audio runtime baseline");
    const auto before=json(censusPath);
    const auto check=[](const Tempest::Sound& sound,uint16_t channels,const std::vector<int16_t>& samples,const std::string& field) {
      require(!sound.isEmpty(),field+": valid PCM must remain nonempty");
      size_t count=0; uint16_t actualChannels=0; uint32_t rate=0;
      const auto* data=sound.samples16(count,actualChannels,rate);
      require(data!=nullptr && count==samples.size() && actualChannels==channels && rate==1000,field+": decoded PCM metadata differs");
      if(count!=0) bytes(data,samples.data(),count*sizeof(int16_t),field+" PCM samples");
      };
    check(Resources::loadSoundBuffer("p15_pcm.wav \t"),2,{-32768,32767,-1,1,4660,-4660},"native PCM");
    check(Resources::loadSoundBuffer("P15_SPEECH.WAV"),2,{-32768,32767,-1,1,4660,-4660},"native speech alias");
    size_t externalCalls=0;
    gothic.externalSoundFn=[&](std::string_view,std::vector<uint8_t>& data) { ++externalCalls; data=externalPcm; return true; };
    check(Resources::loadSoundBuffer("P15_PCM.WAV"),1,{123,-456},"external valid PCM priority");
    gothic.externalSoundFn=[&](std::string_view,std::vector<uint8_t>& data) { ++externalCalls; data=tiny; return true; };
    require(Resources::loadSoundBuffer("P15_PCM.WAV").isEmpty(),"invalid successful external audio must not fall back to native or raw PCM");
    gothic.externalSoundFn=[&](std::string_view,std::vector<uint8_t>& data) { ++externalCalls; data=tiny; return false; };
    check(Resources::loadSoundBuffer("P15_PCM.WAV"),2,{-32768,32767,-1,1,4660,-4660},"external false selects native PCM");
    equal(externalCalls,3u,"external callback ordering");
    gothic.externalSoundFn={};
    require(Resources::loadSoundBuffer("").isEmpty() && Resources::loadSoundBuffer("P15_MISSING.WAV").isEmpty() &&
            Resources::loadSoundBuffer("path/P15_PCM.WAV").isEmpty(),"empty, missing and path-prefixed audio requests must stay empty");
    check(Resources::loadSoundBuffer("P15_ZERO.WAV"),2,{},"zero-data PCM nonempty outcome");
    require(Resources::loadSoundBuffer("P15_TINY.WAV").isEmpty(),"indexed tiny WAV must keep the original decoder exception/empty result");
    const auto& source=resources.baseAudio.sounds.at("P15_PCM.WAV");
    const auto payloadFile=resources.baseAudio.dir/source.file;
    fs::remove(payloadFile);
    require(Resources::loadSoundBuffer("P15_PCM.WAV").isEmpty(),"missing native payload must not fall back to intact original PCM");
    gothicglb::writeFile(payloadFile,tiny.data(),tiny.size());
    require(Resources::loadSoundBuffer("P15_PCM.WAV").isEmpty(),"resized native payload must not fall back to intact original PCM");
    auto changed=nativePcm; changed.at(44)^=1;
    gothicglb::writeFile(payloadFile,changed.data(),changed.size());
    require(Resources::loadSoundBuffer("P15_PCM.WAV").isEmpty(),"same-size SHA corruption must not fall back to intact original PCM");
    gothic.externalSoundFn=std::move(previousExternal);
    require(Resources::writeCensus(),"cannot write audio runtime census");
    const auto after=json(censusPath);
    require(before["files"]==after["files"],"native/external/error audio paths must add zero original resource opens");
    const auto& audio=after["runtime_audio"];
    require(std::string_view(audio["coverage"].GetString())=="verified_base_payload_reads" && !audio["complete_corpus"].GetBool(),
            "audio census must describe verified payload reads without full-corpus claims");
    equal(audio["selections"].Size(),5u,"only verified native reads, including tiny decoder failure, enter audio census");
    for(const auto& row:audio["selections"].GetArray()) {
      const auto* selected=resources.baseAudio.find(row["request"].GetString());
      require(selected!=nullptr && row["class"].GetString()==selected->cls &&
              row["payload"]["file"].GetString()=="base/audio/"+selected->file &&
              row["payload"]["size"].GetUint64()==selected->size && row["payload"]["sha256"].GetString()==selected->payload,
              "verified audio payload census identity differs");
      const auto& imported=row["imported_source"];
      require(imported["origin"].GetString()==selected->origin && imported["path"].GetString()==selected->path &&
              imported["archive"].GetString()==selected->archive && imported["size"].GetUint64()==selected->size &&
              imported["sha256"].GetString()==selected->sourceSha256,"audio imported provenance census differs");
      }
    equal(after["base_misses"].Size(),3u,"native audio integrity failures must enter census misses");
    for(const auto& row:after["base_misses"].GetArray())
      require(std::string(row["reason"].GetString()).find("audio data: ")!=std::string::npos,"audio misses must name the native integrity error");
    size_t sounds=0,speech=0;
    for(const auto& row:after["replacements"].GetArray()) {
      const std::string_view kind=row["kind"].GetString();
      if(kind=="basesound") ++sounds;
      else if(kind=="basespeech") ++speech;
      else require(false,"audio fixture produced an unrelated replacement kind");
      }
    equal(sounds,4u,"verified sound replacement reads");
    equal(speech,1u,"verified speech replacement reads");
    setenv("OPENGOTHIC_BASE",base.c_str(),1);
    std::puts("Audio runtime: lazy payload checks, external priority, exact PCM and zero-data outcomes, integrity failures, verified identities and zero original opens checked");
    }

  static void census(const fs::path& file, size_t meshes, size_t models, size_t fonts, size_t programs) {
    const auto data=json(file);
    equal(data["replacements"].Size(),meshes+models+programs,"census base mesh/model/program count");
    size_t selectedMeshes=0,selectedModels=0,selectedPrograms=0,sourceModels=0,sourceFonts=0;
    for(const auto& row:data["replacements"].GetArray()) {
      const std::string_view kind=row["kind"].GetString(),path=row["replacement"].GetString();
      if(kind=="basemodel") {
        ++selectedModels;
        require(path.starts_with("base/models/"),"base model census path");
        equal(row["opens"].GetUint64(),1u,"base model selected once");
        }
      else if(kind=="baseprogram") {
        ++selectedPrograms;
        require(path.starts_with("base/programs/"),"base program census path");
        equal(row["opens"].GetUint64(),1u,"base program selected once");
        }
      else {
        ++selectedMeshes;
        require(kind=="basemesh","base mesh census kind");
        require(path.starts_with("base/meshes/"),"base mesh census path");
        }
      }
    equal(selectedMeshes,meshes,"census selected mesh count");
    equal(selectedModels,models,"census selected model count");
    equal(selectedPrograms,programs,"census selected program count");
    for(const auto& row:data["files"].GetArray()) {
      const std::string_view kind=row["kind"].GetString();
      if(kind=="font") {
        ++sourceFonts;
        equal(row["opens"].GetUint64(),1u,std::string(row["name"].GetString())+": only independent font oracle may open FNT");
        }
      if(kind!="model" && kind!="skeleton" && kind!="anim" && kind!="morph") continue;
      ++sourceModels;
      equal(row["opens"].GetUint64(),1u,std::string(row["name"].GetString())+": only independent source oracle may open the archive");
      }
    equal(sourceModels,models,"census independent source model count");
    equal(sourceFonts,fonts,"census independent source font count");
    std::puts("Model census: each source opened once by the oracle; Resources selected each base record without original opens");
    }

  static void censusSource(const fs::path& file) {
    ContentCensus census(file.c_str());
    const std::string_view bytes="abc";
    auto reader=zenkit::Read::from(reinterpret_cast<const std::byte*>(bytes.data()),bytes.size());
    reader->seek(1,zenkit::Whence::BEG);
    census.profile("GAME.DAT","DIALOGUE",4);
    census.selected("contract","GAME.DAT",4,"host_ordinary","host","_work/GAME.DAT","_work/GAME.DAT",*reader);
    equal(reader->tell(),1u,"census must preserve parsed reader position");
    equal(reader->read_char(),'b',"census must preserve the next parsed byte");
    require(census.write(),"cannot write source census fixture");
    const auto data=json(file);
    require(data["files"].Empty() && data["replacements"].Empty() && data["base_misses"].Empty(),
            "source observations must not add original opens, replacements or misses");
    const auto& profile=data["active_profile"];
    require(std::string_view(profile["game"].GetString())=="GAME.DAT" &&
            std::string_view(profile["output_units"].GetString())=="DIALOGUE" && profile["language"].GetInt()==4,
            "source census must retain the active profile and raw language");
    const auto& sources=data["runtime_sources"];
    require(std::string_view(sources["coverage"].GetString())=="observed_successful_parses" &&
            !sources["complete_corpus"].GetBool(),"runtime source census must not claim a complete corpus");
    equal(sources["selections"].Size(),1u,"source census selection count");
    const auto& selected=sources["selections"][0];
    equal(selected["size"].GetUint64(),3u,"source census size");
    require(std::string_view(selected["sha256"].GetString())==
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
            "source census must hash the whole already-open stream");
    }

  static Resources::TextureData decodeArchive(zenkit::Read& input, bool compiled, bool forceMips) {
    if(compiled) {
      zenkit::Texture tex;
      tex.load(&input);
      if(tex.format()>=zenkit::TextureFormat::DXT1 && tex.format()<=zenkit::TextureFormat::DXT5) {
        const auto dds=zenkit::to_dds(tex);
        Tempest::MemReader reader(reinterpret_cast<const uint8_t*>(dds.data()),dds.size());
        Resources::TextureData out{Tempest::Pixmap(reader)};
        out.useMipmap=forceMips || out.pixmap.mipCount()>1;
        return out;
        }
      const auto rgba=tex.as_rgba8(0);
      Resources::TextureData out{Tempest::Pixmap(tex.width(),tex.height(),Tempest::TextureFormat::RGBA8),true};
      std::memcpy(out.pixmap.data(),rgba.data(),rgba.size());
      return out;
      }
    input.seek(0,zenkit::Whence::END);
    std::vector<uint8_t> bytes(input.tell());
    input.seek(0,zenkit::Whence::BEG);
    require(input.read(bytes.data(),bytes.size())==bytes.size(),"incomplete archive texture");
    Tempest::MemReader reader(bytes.data(),bytes.size());
    Resources::TextureData out{Tempest::Pixmap(reader)};
    out.useMipmap=forceMips || out.pixmap.mipCount()>1;
    return out;
    }

  static void legacyModelBounds() {
    for(const auto* name:{"ARMOR_BDT_H.MDL","FIREPLACE_HIGH2.MDM"}) {
      auto file=readSource(name);
      file->seek(0,zenkit::Whence::END);
      std::vector<std::byte> source(file->tell());
      file->seek(0,zenkit::Whence::BEG);
      require(file->read(source.data(),source.size())==source.size(),std::string(name)+": source read");
      auto input=zenkit::Read::from(&source);
      const bool model=std::string_view(name).ends_with(".MDL");
      if(model) {
        zenkit::ModelHierarchy hierarchy {};
        hierarchy.load(input.get());
        }
      // Reach the first attachment or soft-skin proto using only container
      // headers. Bounds below are read independently of the MRM decoder.
      size_t payload=0,size=0;
      while(!input->eof()) {
        const auto type=input->read_ushort();
        size=input->read_uint();
        payload=input->tell();
        if(type==0xB100) break;
        input->seek(static_cast<std::ptrdiff_t>(payload+size),zenkit::Whence::BEG);
        }
      require(payload>0 && payload+size<=source.size(),std::string(name)+": proto chunk");
      auto metadata=zenkit::Read::from(source.data()+payload,size);
      equal(metadata->read_ushort(),model ? 0x805u : 0x705u,std::string(name)+" proto version");
      const auto contentSize=metadata->read_uint();
      metadata->seek(contentSize,zenkit::Whence::CUR);
      const auto submeshes=metadata->read_ubyte();
      metadata->seek(16+80*submeshes,zenkit::Whence::CUR);
      auto archive=zenkit::ReadArchive::from(metadata.get());
      for(unsigned i=0;i<submeshes;++i) {
        (void) archive->read_string();
        zenkit::ArchiveObject material {};
        require(archive->read_object_begin(material) && material.class_name=="zCMaterial",
                std::string(name)+": material object");
        archive->skip_object(true);
        }
      const auto alpha=metadata->read_ubyte();
      require(alpha<=1,std::string(name)+": source alpha-test byte");
      zenkit::AxisAlignedBoundingBox expected {};
      expected.load(metadata.get());
      zenkit::OrientedBoundingBox expectedObb;
      expectedObb.load(metadata.get());
      metadata->seek(16,zenkit::Whence::CUR);
      equal(metadata->tell(),size,std::string(name)+" source proto end");

      const auto check=[&](const zenkit::MultiResolutionMesh& mesh) {
        equal(mesh.alpha_test,alpha,std::string(name)+" alpha test");
        for(unsigned axis=0;axis<3;++axis) {
          equal(std::bit_cast<uint32_t>(mesh.bbox.min[axis]),std::bit_cast<uint32_t>(expected.min[axis]),
                std::string(name)+" bbox min bits");
          equal(std::bit_cast<uint32_t>(mesh.bbox.max[axis]),std::bit_cast<uint32_t>(expected.max[axis]),
                std::string(name)+" bbox max bits");
          require(std::isfinite(mesh.bbox.min[axis]) && std::isfinite(mesh.bbox.max[axis]) &&
                  mesh.bbox.min[axis]<=mesh.bbox.max[axis] && std::abs(mesh.bbox.min[axis])<1000.f &&
                  std::abs(mesh.bbox.max[axis])<1000.f,std::string(name)+": finite model-scale bounds");
          }
        const auto nodes=[&](auto&& self,const zenkit::OrientedBoundingBox& box)->size_t {
          size_t count=1;
          for(const auto& child:box.children) count+=self(self,child);
          require(count<=13,std::string(name)+": bounded OBB tree");
          return count;
          };
        equal(nodes(nodes,mesh.obbox),nodes(nodes,expectedObb),std::string(name)+" OBB nodes");
        };
      // Bound the first regression read to its own proto. On the old parser
      // this fails before a full model can allocate a spurious OBB tree at EOF.
      zenkit::MultiResolutionMesh proto {};
      auto bounded=zenkit::Read::from(source.data()+payload,size);
      proto.load_from_section(bounded.get());
      check(proto);
      equal(bounded->tell(),size,std::string(name)+" decoded proto end");

      std::vector<std::string> errors;
      zenkit::Logger::set(zenkit::LogLevel::ERROR,[&](auto,const char* component,const char* message) {
        errors.emplace_back(std::string(component)+": "+message);
        });
      auto original=zenkit::Read::from(&source);
      if(model) {
        zenkit::Model parsed {};
        parsed.load(original.get());
        equal(parsed.mesh.meshes.size(),2u,"legacy armor skin count");
        check(parsed.mesh.meshes.front().mesh);
        }
      else {
        zenkit::ModelMesh parsed {};
        parsed.load(original.get());
        equal(parsed.attachments.size(),1u,"legacy fireplace attachment count");
        check(parsed.attachments.begin()->second);
        }
      zenkit::Logger::set_default(zenkit::LogLevel::INFO);
      require(errors.empty(),std::string(name)+": "+(errors.empty() ? "" : errors.front()));
      std::printf("Legacy proto: %s, alpha=%u, bounds and OBB match source bytes; no parser errors\n",name,unsigned(alpha));
      }
    }

  static void modelTagBoundary() {
    auto input=readSource("BLATTCRAWLER.MSB");
    size_t end=0;
    while(!input->eof()) {
      const auto type=input->read_ushort();
      const auto size=input->read_uint();
      end=input->tell()+size;
      if(type==0xF590) break;
      input->seek(static_cast<std::ptrdiff_t>(end),zenkit::Whence::BEG);
      }
    require(!input->eof() && input->read_line(false)=="DEF_HIT_LIMB","model tag source event");
    const auto bone=input->read_line(false);
    require(bone=="BIP01 L FRONT CLAW","model tag source bone");
    input->seek(static_cast<std::ptrdiff_t>(end),zenkit::Whence::BEG);
    equal(input->read_ushort(),0xF520u,"animation after model tag");
    (void) input->read_uint();
    const auto following=input->read_line(false);
    require(!following.empty(),"following animation source name");

    std::vector<std::string> errors;
    zenkit::Logger::set(zenkit::LogLevel::ERROR,[&](auto,const char* component,const char* message) {
      errors.emplace_back(std::string(component)+": "+message);
      });
    input->seek(0,zenkit::Whence::BEG);
    zenkit::ModelScript script {};
    script.load(input.get());
    zenkit::Logger::set_default(zenkit::LogLevel::INFO);
    require(errors.empty(),"model tag boundary: "+(errors.empty() ? "" : errors.front()));
    equal(script.model_tags.size(),1u,"model tag count");
    require(script.model_tags.front().bone==bone,"model tag must retain its source bone");
    require(!script.animations.empty() && script.animations.front().name==following,
            "model tag must retain the following animation");
    std::puts("Model tag: BLATTCRAWLER.MSB bone and following animation match source; no parser errors");
    }

  static void archiveOrder() {
    struct LegacyArchive {
      std::u16string path;
      int64_t timestamp = -1;
      uint16_t ordinal = 0;
      bool mod = false;
      uintmax_t size = 0;
      };
    std::vector<LegacyArchive> expected;
    // Independent oracle: Resources::detectVdf/vdfTimestamp/loadVdfs before P10.
    const auto timestamp = [](const std::u16string& path) {
      uint32_t count=0, value=0;
      char signature[16]={};
      try {
        Tempest::RFile file(path);
        file.seek(256);
        file.read(signature,sizeof(signature));
        file.read(&count,sizeof(count));
        file.seek(4);
        file.read(&value,sizeof(value));
        return int64_t(value);
        }
      catch(...) { return int64_t(-1); }
      };
    std::function<void(const std::u16string&)> scan;
    scan = [&](const std::u16string& root) {
      Tempest::Dir::scan(root,[&](const std::u16string& name, Tempest::Dir::FileType type) {
        if(type==Tempest::Dir::FT_File) {
          LegacyArchive archive;
          archive.path = root+name;
          archive.timestamp = timestamp(archive.path);
          archive.ordinal = uint16_t(expected.size());
          archive.mod = name.rfind(u".mod")==name.size()-4;
          archive.size = fs::file_size(archive.path);
          if(archive.size>0)
            expected.push_back(std::move(archive));
          return;
          }
        if(name.find(u".NX.")!=std::u16string::npos)
          return;
        if(type==Tempest::Dir::FT_Dir && name!=u".." && name!=u".")
          scan(root+name+u"/");
        });
      };
    scan(CommandLine::inst().nestedPath({u"Data"},Tempest::Dir::FT_Dir));
    std::stable_sort(expected.begin(),expected.end(),[](const LegacyArchive& a, const LegacyArchive& b) {
      return std::make_tuple(a.mod ? 1 : -1,a.timestamp,int(a.ordinal)) >
             std::make_tuple(b.mod ? 1 : -1,b.timestamp,int(b.ordinal));
      });
    const auto actual = BaseImport::runtimeArchives(fs::path(CommandLine::inst().rootPath()));
    equal(actual.size(),expected.size(),"legacy archive scan count");
    for(size_t i=0; i<expected.size(); ++i) {
      const auto path = fs::absolute(fs::path(expected[i].path)).lexically_normal();
      const auto field = "archive order["+std::to_string(i)+"] "+path.filename().string();
      require(fs::absolute(actual[i].path).lexically_normal()==path,field+": path differs from legacy scan");
      auto name = path.filename().string();
      for(auto& c:name)
        c = char(std::toupper(static_cast<unsigned char>(c)));
      require(actual[i].name==name,field+": uppercase name differs");
      equal(actual[i].size,expected[i].size,field+" size");
      equal(actual[i].timestamp,expected[i].timestamp,field+" timestamp");
      equal(actual[i].mod,expected[i].mod,field+" mod flag");
      }
    std::printf("Archive order: %zu entries equal to independent legacy scan, including mods\n",expected.size());
    }

  static void run(const fs::path& base, const fs::path& temporary) {
    archiveOrder();
    censusSource(temporary/"source-census.json");
    setenv("OPENGOTHIC_CONTENT","",1);
    setenv("OPENGOTHIC_TEXTURES","",1);
    const auto censusPath=temporary/"census.json";
    setenv("OPENGOTHIC_CENSUS",censusPath.c_str(),1);
    std::string meshKey;
    size_t textureCount=0,meshCount=0,modelCount=0,fontCount=0,programCount=0,dds=0,png=0,tga=0,vertices=0,indices=0,meshlets=0;
    {
    Resources resources;
    require(!resources.baseTextures.enabled && !resources.baseMeshes.enabled && !resources.baseModels.enabled && !resources.baseFonts.enabled,
            "base must wait for selection");
    Resources::loadBase();
    require(resources.baseTextures.enabled && resources.baseMeshes.enabled && resources.baseModels.enabled && resources.baseFonts.enabled,
            "selected base identity must enable texture, mesh, model and font indices");
    require(!resources.baseTextures.files.empty() && !resources.baseMeshes.files.empty() && !resources.baseModels.files.empty() && !resources.baseFonts.files.empty(),
            "texture, mesh, model and font indices must contain files");
    modelCount=models(base);
    fontCount=fonts(base,resources);
    programCount=programs(base,censusPath);
    modelFixtures(temporary);
    for(const auto& [key,file]:resources.baseTextures.files) {
      auto* entry=sourceVfs().find(key+"-C.TEX");
      const bool compiled=entry!=nullptr;
      if(!compiled)
        entry=sourceVfs().find(key+".TGA");
      require(entry!=nullptr,key+": source texture missing");
      for(const bool forceMips:{false,true}) {
        auto input=entry->open_read();
        const auto original=decodeArchive(*input,compiled,forceMips);
        const auto converted=Resources::decodeBaseTexture(base/"textures"/file,forceMips);
        texture(original,converted,key+" forceMips="+std::to_string(forceMips));
        }
      dds+=fs::path(file).extension()==".dds";
      png+=fs::path(file).extension()==".png";
      tga+=fs::path(file).extension()==".tga";
      ++textureCount;
      }
    std::printf("Textures: %zu equal (%zu DDS, %zu PNG, %zu TGA), both GPU mip flags\n",textureCount,dds,png,tga);
    for(const auto& [key,file]:resources.baseMeshes.files) {
      auto input=readSource(key+".MRM");
      zenkit::MultiResolutionMesh archiveMesh;
      archiveMesh.load(input.get());
      const auto original=asset::fromZenKit(archiveMesh);
      const auto converted=readBaseMesh(base/"meshes"/file);
      auto difference=compareBaseMeshes(original,converted);
      require(difference.empty(),key+": "+difference);
      bool replaced=true;
      const auto loaded=Resources::loadVisualMesh(key+".3ds",&replaced);
      difference=compareBaseMeshes(converted,loaded);
      require(difference.empty(),key+" selected: "+difference);
      require(!replaced && !Resources::meshReplacementLoaded(key),key+": base must retain original alpha/fracture semantics");
      PackedMesh a(original,PackedMesh::PK_Visual),b(loaded,PackedMesh::PK_Visual);
      vertices+=a.vertices.size();
      indices+=a.indices.size();
      meshlets+=a.indices.size()/PackedMesh::MaxInd;
      packed(a,b,key);
      ++meshCount;
      }
    meshKey=resources.baseMeshes.files.begin()->first;
    }
    census(censusPath,meshCount,modelCount,fontCount,programCount);
    // Keep parser regression reads separate from the runtime-open census.
    {
    Resources resources;
    Resources::loadBase();
    definitionRecords(base,censusPath);
    outputUnits(base,censusPath);
    legacyModelBounds();
    modelTagBoundary();
    }
    musicCorpus(base);
    musicIntegrity(base,temporary);
    musicRuntime(base,temporary,censusPath);
    audioCorpus(base);
    audioRuntime(base,temporary,censusPath);
    std::printf("Meshes: %zu equal; %zu vertices, %zu indices, %zu meshlets; physics packing equal\n",
                meshCount,vertices,indices,meshlets);

    const auto invalid=temporary/"invalid";
    for(const auto* kind:{"textures","meshes"}) {
      fs::create_directories(invalid/kind);
      auto index=json(base/kind/(std::string(kind)+".json"));
      auto& size=index["archives"][0]["size"];
      const auto old=size.GetUint64();
      size.SetUint64(old+1);
      writeJson(invalid/kind/(std::string(kind)+".json"),index);
      std::printf("Reject %s archive size: %llu -> %llu\n",kind,
                  static_cast<unsigned long long>(old),static_cast<unsigned long long>(old+1));
      }
    setenv("OPENGOTHIC_BASE",invalid.c_str(),1);
    bool rejected=false;
    try {
      Resources resources;
      Resources::loadBase();
      }
    catch(const BaseStore::Error& error) { rejected=std::string(error.what()).find("content-convert resolve <game directory>")!=std::string::npos; }
    require(rejected,"invalid explicit base must fail with recovery instructions");

    for(const std::string kind:{"models","fonts","programs","outputunits","audio","music"}) {
      const auto incomplete=temporary/("missing-"+kind+"-index");
      fs::create_directories(incomplete/kind);
      fs::copy_file(base/"base.json",incomplete/"base.json");
      for(const auto* part:{"base-files.json","worlds","textures","meshes","zen","models","fonts","programs","outputunits","audio","music","sources"})
        if(part!=kind) fs::create_symlink(base/part,incomplete/part);
      for(const auto& file:fs::directory_iterator(base/kind))
        if(file.path().filename()!=kind+".json") fs::create_symlink(file.path(),incomplete/kind/file.path().filename());
      setenv("OPENGOTHIC_BASE",incomplete.c_str(),1);
      std::string missingIndexError;
      try {
        Resources resources;
        Resources::loadBase();
        }
      catch(const BaseStore::Error& error) { missingIndexError=error.what(); }
      require(missingIndexError.find("missing or unreadable base file: "+(incomplete/kind/(kind+".json")).string())!=std::string::npos &&
              missingIndexError.find("content-convert resolve <game directory>")!=std::string::npos,
              "base without "+kind+"/"+kind+".json must fail with recovery instructions");
      }
    {
    const auto broken=temporary/"broken-program-record";
    fs::create_directories(broken/"programs");
    for(const auto* part:{"base.json","base-files.json","worlds","textures","meshes","zen","models","fonts","outputunits","audio","music","sources"})
      fs::create_symlink(base/part,broken/part);
    for(const auto& file:fs::directory_iterator(base/"programs"))
      fs::create_symlink(file.path(),broken/"programs"/file.path().filename());
    const auto index=ProgramData::readIndex(broken/"programs");
    const auto selected=std::find_if(index.selections.begin(),index.selections.end(),[](const auto& row) {
      return row.language==-1 && row.source.has_value();
      });
    require(selected!=index.selections.end(),"missing program record check needs a default selection");
    const auto& source=index.sources.at(*selected->source);
    const auto file=index.path(source);
    auto bin=file; bin.replace_extension(".bin");
    setenv("OPENGOTHIC_BASE",broken.c_str(),1);
    {
    Resources resources;
    Resources::loadBase();
    fs::remove(bin);
    const auto host=selected->step.starts_with("host_") ?
      (fs::path(CommandLine::inst().rootPath())/source.path).u16string() : std::u16string{};
    const auto fails=[&](const std::string& description, const std::string& reason=std::string{}) {
      bool failed=false;
      try { (void)Resources::loadBaseProgram(selected->request,selected->language,selected->step,host); }
      catch(const BaseStore::Error& error) {
        const std::string message=error.what();
        failed=message.find("program "+selected->request+":")!=std::string::npos &&
               (reason.empty() || message.find(reason)!=std::string::npos);
        }
      require(failed,description+": selected native failure must be fatal");
      };
    fails("missing BIN");
    fs::create_symlink(base/"programs"/bin.filename(),bin);
    fs::remove(file);
    const std::string invalid="{broken";
    gothicglb::writeFile(file,invalid.data(),invalid.size());
    fails("corrupt JSON");
    auto changed=gothicglb::readFile(base/"programs"/file.filename());
    changed.push_back(' '); // Valid JSON with a different identity from the imported record.
    gothicglb::writeFile(file,changed.data(),changed.size());
    fails("changed native identity","program record identity differs from base index");
    }
    const auto failed=json(censusPath);
    require(failed["files"].Empty() && failed["replacements"].Empty() &&
            failed["runtime_programs"]["selections"].Empty() && failed["base_misses"].Size()==3,
            "failed native records must record misses without original opens or successful selections");
    }
    {
    const auto broken=temporary/"broken-outputunits-record";
    fs::create_directories(broken/"outputunits");
    for(const auto* part:{"base.json","base-files.json","worlds","textures","meshes","zen","models","fonts","programs","audio","music","sources"})
      fs::create_symlink(base/part,broken/part);
    for(const auto& file:fs::directory_iterator(base/"outputunits"))
      fs::create_symlink(file.path(),broken/"outputunits"/file.path().filename());
    const auto index=OutputUnits::readIndex(broken/"outputunits");
    const auto selected=std::find_if(index.selections.begin(),index.selections.end(),[&](const auto& row) {
      return row.language==-1 && row.source && index.sources.at(*row.source).skipped.empty();
      });
    require(selected!=index.selections.end(),"native OU integrity check needs a default selection");
    const auto& source=index.sources.at(*selected->source);
    const auto file=index.path(source);
    setenv("OPENGOTHIC_BASE",broken.c_str(),1);
    {
    Resources resources;
    Resources::loadBase();
    fs::remove(file);
    const auto host=selected->step.starts_with("host_") ?
      (fs::path(CommandLine::inst().rootPath())/source.path).u16string() : std::u16string{};
    const auto sentinel=std::make_shared<asset::CutsceneBlock>();
    std::vector<std::shared_ptr<asset::CutsceneBlock>> blocks{sentinel};
    const auto fails=[&](const std::string& description, const std::string& reason=std::string{}) {
      bool failed=false;
      try { (void)Resources::loadBaseOutputUnits(selected->candidate,selected->language,selected->step,blocks,host); }
      catch(const BaseStore::Error& error) {
        const std::string message=error.what();
        failed=message.find("output units "+selected->candidate+":")!=std::string::npos &&
               (reason.empty() || message.find(reason)!=std::string::npos);
        }
      require(failed && blocks.size()==1 && blocks[0]==sentinel,
              description+": native OU failure must preserve the prior library and cannot fall back to raw sources");
      };
    fails("missing OU JSON");
    const std::string invalid="{broken";
    gothicglb::writeFile(file,invalid.data(),invalid.size());
    fails("corrupt OU JSON");
    auto changed=gothicglb::readFile(base/"outputunits"/file.filename());
    changed.push_back(' ');
    gothicglb::writeFile(file,changed.data(),changed.size());
    fails("changed OU identity","output units record identity differs from base index");
    }
    const auto failed=json(censusPath);
    require(failed["files"].Empty() && failed["replacements"].Empty() &&
            failed["runtime_outputunits"]["selections"].Empty() && failed["runtime_sources"]["selections"].Empty() &&
            failed["base_misses"].Size()==3,"failed native OU records must record misses with zero original opens or selections");
    }
    setenv("OPENGOTHIC_BASE",base.c_str(),1);
    {
    Resources resources;
    Resources::loadBase();
    require(Resources::loadVisualMesh("BASE_CONTRACT_MISSING.MRM").positions.empty(),"missing mesh must stay empty");
    require(Resources::loadVobBundle("BASE_CONTRACT_MISSING.ZEN")->empty(),"missing bundle must stay empty");
    }
    const auto missing=json(censusPath);
    equal(missing["files"].Size(),0u,"missing resources must not open archives");
    equal(missing["base_misses"].Size(),2u,"missing resources must enter the census");

    const auto content=temporary/"content";
    fs::create_directories(content/"meshes");
    ownMesh(content/"meshes/own.glb");
    const auto own=loadGltfMesh(content/"meshes/own.glb");
    const auto index="{\""+meshKey+"\":{\"glb\":\"own.glb\"}}";
    gothicglb::writeFile(content/"meshes/meshes.json",index.data(),index.size());
    setenv("OPENGOTHIC_BASE",base.c_str(),1);
    setenv("OPENGOTHIC_CONTENT",content.c_str(),1);
    {
    Resources resources;
    Resources::loadBase();
    require(resources.baseMeshes.enabled,"priority check requires enabled base");
    bool replaced=false;
    const auto loaded=Resources::loadVisualMesh(meshKey+".MRM",&replaced);
    require(replaced && Resources::meshReplacementLoaded(meshKey),"own mesh must win over base");
    require(compareBaseMeshes(own,loaded).empty(),"own mesh selection differs");
    }
    const auto selected=json(censusPath);
    equal(selected["files"].Size(),0u,"own replacement archive opens");
    equal(selected["replacements"].Size(),1u,"own replacement count");
    require(std::string(selected["replacements"][0]["kind"].GetString())=="mesh","own replacement census kind");
    std::puts("Selection: invalid base rejected; missing resources recorded; own content wins; census kinds stay separate");
    }
  };

int main() {
  const auto temporary=fs::temp_directory_path()/
    ("gothic-basecontent-contract-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  int result=0;
  try {
    const auto* gothic=std::getenv("GOTHIC_DIR");
    const auto* dir=std::getenv("OPENGOTHIC_BASE");
    BaseContentContract::require(gothic && *gothic,"GOTHIC_DIR must point to a Gothic II installation");
    BaseContentContract::require(dir && *dir,"OPENGOTHIC_BASE must point to a content-convert base with textures and meshes");
    const auto base=fs::absolute(dir);
    fs::create_directories(temporary);
    const char* args[]={"gothic-basecontent-contract","-g",gothic};
    CommandLine commandLine(3,args);
    BaseContentContract::run(base,temporary);
    std::puts("base content contract: passed");
    }
  catch(const std::exception& error) {
    std::fprintf(stderr,"base content contract: %s\n",error.what());
    result=1;
    }
  std::error_code error;
  fs::remove_all(temporary,error);
  return result;
  }

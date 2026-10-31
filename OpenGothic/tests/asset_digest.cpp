#include "asset/fromzenkit.h"
#include <zenkit/World.hh>
#include <zenkit/Vfs.hh>
#include "basemesh.h"
#include "commandline.h"
#include "gothicglb.h"
#include "graphics/material.h"
#include "graphics/shaders.h"
#include "graphics/mesh/submesh/packedmesh.h"
#include "physics/collisionworld.h"
#include "physics/physicvbo.h"
#include "physics/worldcloth.h"
#include "resources.h"
#include "sha256.h"
#include "worldbase.h"

#include <Tempest/VulkanApi>
#include <zenkit/Stream.hh>
#include <zenkit/MultiResolutionMesh.hh>
#include <zenkit/Texture.hh>
#include <zenkit/addon/texcvt.hh>
#include <Tempest/MemReader>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <iostream>
#include <stdexcept>
#include <type_traits>

namespace fs = std::filesystem;

namespace {
// Hash values, never object padding, pointers, container storage or SIMD's spare lane.
// GPU arrays include every allocated meshlet slot, including their explicit padding.
class Digest {
  public:
    Digest()=default;
    Digest(const Digest&)=delete;
    Digest& operator=(const Digest&)=delete;

    void bytes(const void* data, size_t size) {
      context.update(data,size);
      }
    template<class T> void value(const T& v) {
      static_assert(std::is_arithmetic_v<T> || std::is_enum_v<T>);
      bytes(&v,sizeof(v));
      }
    void text(const std::string& s) {
      value(uint64_t(s.size())); bytes(s.data(),s.size());
      }
    template<class T> void array(const std::vector<T>& v) {
      static_assert(std::is_trivially_copyable_v<T>);
      value(uint64_t(v.size())); bytes(v.data(),v.size()*sizeof(T));
      }
    template<class T> void point(const T& p) {
      value(p.x); value(p.y); value(p.z);
      }
    std::string finish() {
      return context.finish();
      }
  private:
    Sha256 context;
  };
}

struct AssetDigest {
  Digest total;
  size_t rows=0;
  bool archiveTextures=false;
  zenkit::Vfs sourceVfs;

  void emit(const std::string& kind, const std::string& name, Digest& digest) {
    const auto line=kind+" "+name+" "+digest.finish()+"\n";
    total.bytes(line.data(),line.size());
    std::fwrite(line.data(),1,line.size(),stdout);
    ++rows;
    }

  static void packed(Digest& d, const PackedMesh& p) {
    static_assert(sizeof(Resources::Vertex)==9*sizeof(uint32_t));
    d.array(p.vertices); d.array(p.indices); d.array(p.indices8);
    d.value(p.isUsingAlphaTest);
    d.value(uint64_t(p.subMeshes.size()));
    for(const auto& s:p.subMeshes) {
      d.text(gothicglb::materialJson(s.material,0));
      d.value(uint64_t(s.iboOffset)); d.value(uint64_t(s.iboLength));
      d.value(s.alphaTest); d.value(s.alphaTestRequired);
      }
    d.value(uint64_t(p.meshletBounds.size()));
    for(const auto& b:p.meshletBounds) { d.point(b.pos); d.value(b.r); }
    d.point(p.bbox().first); d.point(p.bbox().second);
    }

  void archiveTexture(const std::string& name) {
    auto& resources=*Resources::inst;
    if(name.empty() || resources.texCache.count(name)) return;
    std::string key=name;
    for(auto& c:key) c=char(std::toupper(static_cast<unsigned char>(c)));
    const auto* entry=sourceVfs.find(key.ends_with(".TGA") ? key.substr(0,key.size()-4)+"-C.TEX" : key);
    Tempest::Texture2d texture;
    if(entry && key.ends_with(".TGA")) {
      auto input=entry->open_read();
      zenkit::Texture tex; tex.load(input.get());
      if(tex.format()>=zenkit::TextureFormat::DXT1 && tex.format()<=zenkit::TextureFormat::DXT5) {
        const auto dds=zenkit::to_dds(tex);
        Tempest::MemReader reader(reinterpret_cast<const uint8_t*>(dds.data()),dds.size());
        Tempest::Pixmap pm(reader);
        texture=Resources::device().texture(pm,pm.mipCount()>1);
        }
      else {
        const auto rgba=tex.as_rgba8(0);
        Tempest::Pixmap pm(tex.width(),tex.height(),Tempest::TextureFormat::RGBA8);
        std::memcpy(pm.data(),rgba.data(),rgba.size());
        texture=Resources::device().texture(pm,true);
        }
      }
    else if(const auto* raw=sourceVfs.find(name)) {
      auto input=raw->open_read();
      input->seek(0,zenkit::Whence::END);
      std::vector<uint8_t> bytes(input->tell()); input->seek(0,zenkit::Whence::BEG);
      if(input->read(bytes.data(),bytes.size())!=bytes.size()) throw std::runtime_error("incomplete texture "+name);
      Tempest::MemReader reader(bytes.data(),bytes.size());
      Tempest::Pixmap pm(reader);
      texture=Resources::device().texture(pm,pm.mipCount()>1);
      }
    resources.texCache[name]=texture.isEmpty() ? nullptr : std::make_unique<Tempest::Texture2d>(std::move(texture));
    }

  void materials(Digest& d, const PackedMesh& p) {
    d.value(uint64_t(p.subMeshes.size()));
    for(const auto& s:p.subMeshes) {
      if(archiveTextures) archiveTexture(s.material.texture);
      const Material m(s.material,s.alphaTest,s.alphaTestRequired);
      d.value(m.alpha); d.value(m.sourceMaterialGroup); d.value(m.sourceAlphaFunc);
      d.value(m.sourceColorAlpha); d.value(m.alphaTestEnabled); d.value(m.alphaTestRequired);
      d.value(m.classifiedAlpha); d.value(m.alphaWeight); d.value(m.isGhost);
      d.value(m.texAniMapDirPeriod.x); d.value(m.texAniMapDirPeriod.y); d.value(m.texAniFPSInv);
      d.value(m.waveMaxAmplitude); d.value(m.envMapping); d.value(m.uvScale);
      d.value(m.antitileCell); d.value(m.antitile); d.value(m.materialMaps);
      d.value(uint64_t(m.frames.size()));
      d.value(m.tex!=nullptr);
      if(m.tex) { d.value(m.tex->format()); d.value(m.tex->w()); d.value(m.tex->h()); }
      d.value(m.normal!=nullptr); d.value(m.orm!=nullptr);
      d.value(m.isSolid()); d.value(m.hasFrameAnimation()); d.value(m.hasUvAnimation());
      d.value(m.isTesselated()); d.value(m.isForwardShading()); d.value(m.isSceneInfoRequired());
      d.value(m.isShadowmapRequired()); d.value(m.isTextureInShadowPass());
      }
    }

  static void physics(Digest& d, const PhysicVbo& p) {
    d.value(uint64_t(p.vert.size()));
    for(const auto& v:p.vert)
      for(JPH::uint axis=0;axis<3;++axis) d.value(v[axis]);
    d.array(p.id);
    d.value(uint64_t(p.segments.size()));
    for(const auto& s:p.segments) {
      d.value(uint64_t(s.off)); d.value(uint64_t(s.size)); d.value(s.mat);
      d.text(s.sector ? s.sector : "");
      }
    }

  static void joltInput(Digest& d, const PackedMesh& p) {
    std::vector<JPH::Vec3> vertices;
    for(const auto& v:p.vertices)
      vertices.push_back(CollisionWorld::toMeters(Tempest::Vec3(v.pos[0],v.pos[1],v.pos[2])));
    PhysicVbo land(&vertices),water(&vertices);
    for(const auto& s:p.subMeshes) {
      if(s.material.disable_collision || s.iboLength==0) continue;
      if(s.material.group==asset::MaterialGroup::WATER)
        water.addIndex(p.indices,s.iboOffset,s.iboLength,s.material.group);
      else
        land.addIndex(p.indices,s.iboOffset,s.iboLength,s.material.group,s.material.name.c_str());
      }
    physics(d,land); physics(d,water);
    }

  void world(asset::WorldMesh mesh, const std::string& name, const std::string& path) {
    const auto cloth=WorldCloth::extract(mesh);
    const PackedMesh visual(mesh,PackedMesh::PK_VisualLnd);
    Digest gpu,mat,phys,clothDigest;
    packed(gpu,visual); materials(mat,visual);
    const PackedMesh collision(mesh,PackedMesh::PK_Physic);
    packed(phys,collision); joltInput(phys,collision);
    clothDigest.value(uint64_t(cloth.surfaces.size()));
    // The fallback and each simulated surface have their own GPU packing.
    if(!cloth.original.polygons.material_indices.empty()) {
      packed(clothDigest,PackedMesh(cloth.original,PackedMesh::PK_VisualLnd));
      const PackedMesh fallback(cloth.original,PackedMesh::PK_Physic);
      packed(clothDigest,fallback);
      std::vector<JPH::Vec3> vertices;
      for(const auto& v:fallback.vertices)
        vertices.push_back(CollisionWorld::toMeters(Tempest::Vec3(v.pos[0],v.pos[1],v.pos[2])));
      PhysicVbo input(&vertices);
      for(const auto& s:fallback.subMeshes)
        if(!s.material.disable_collision && s.iboLength>0)
          input.addIndex(fallback.indices,s.iboOffset,s.iboLength,s.material.group);
      physics(clothDigest,input);
      }
    for(const auto& s:cloth.surfaces) {
      clothDigest.value(s.id); clothDigest.text(s.visual); clothDigest.array(s.pins);
      clothDigest.value(s.stiffness); clothDigest.value(s.linearDamping);
      const PackedMesh surface(s.mesh,PackedMesh::PK_VisualLnd);
      packed(clothDigest,surface); materials(clothDigest,surface);
      // Cloth simulation consumes source positions and triangle indices, before packing.
      clothDigest.value(uint64_t(s.mesh.vertices.size()));
      for(const auto& v:s.mesh.vertices) clothDigest.point(v);
      clothDigest.array(s.mesh.polygons.vertex_indices);
      }
    emit("world-gpu-"+path,name,gpu); emit("world-physics-"+path,name,phys);
    emit("world-material-"+path,name,mat); emit("world-cloth-"+path,name,clothDigest);
    }

  void mesh(const asset::StaticMesh& mesh, const std::string& name, const std::string& path) {
    PackedMesh p(mesh,PackedMesh::PK_Visual);
    Digest gpu,mat,phys;
    packed(gpu,p); materials(mat,p);
    const PhysicVbo vbo(std::move(p));
    physics(phys,vbo);
    emit("mesh-gpu-"+path,name,gpu); emit("mesh-physics-"+path,name,phys);
    emit("mesh-material-"+path,name,mat);
    }

  void run(const fs::path& base, Tempest::Device& device) {
    fs::path work;
    BaseImport::mountRuntime(sourceVfs,fs::path(CommandLine::inst().rootPath()),work);
    // Each path gets fresh texture caches, so Material sees its actual decoded format.
    for(const bool useBase:{false,true}) {
      setenv("OPENGOTHIC_BASE",base.c_str(),1);
      archiveTextures=!useBase;
      Resources resources(device);
      Shaders shaders;
      shaders.waitCompiler();
      Resources::loadBase();
      if(archiveTextures) {
        // Material's animation frames use uppercase names; seed their GPU textures
        // from ZenKit in the test, without an archive loader in Resources.
        for(const auto& [key,file]:resources.baseTextures.files) archiveTexture(key+".TGA");
        }
      const std::string path=useBase ? "base" : "archive";
      for(const auto* name:{"NEWWORLD","OLDWORLD","ADDONWORLD","DRAGONISLAND"}) {
        const auto key=std::string(name)+".ZEN";
        if(useBase) {
          WorldData::Data loaded;
          asset::WorldMesh mesh;
          const auto result=tryWorldData(loaded,mesh,key,base/"worlds"/name,{},resources.baseArchives);
          if(!result.replaced) throw std::runtime_error(key+": "+result.error);
          world(std::move(mesh),key,path);
          } else {
          auto input=sourceVfs.find(key)->open_read();
          zenkit::World loaded;
          loaded.load(input.get(),zenkit::GameVersion::GOTHIC_2);
          world(asset::fromZenKit(loaded.world_mesh),key,path);
          }
        }
      // Use the converted index on both paths; require the archive loader to contain every entry.
      Resources::BaseIndex index;
      index.load(base,"meshes"); index.validate(resources.baseArchives);
      if(!index.enabled || index.files.empty()) throw std::runtime_error("Mesh base index is unavailable");
      const std::map<std::string,std::string> files(index.files.begin(),index.files.end());
      for(const auto& [key,file]:files) {
        if(useBase) {
          mesh(readBaseMesh(base/"meshes"/file),key+".MRM",path);
          } else {
          auto input=sourceVfs.find(key+".MRM")->open_read();
          zenkit::MultiResolutionMesh loaded;
          loaded.load(input.get());
          mesh(asset::fromZenKit(loaded),key+".MRM",path);
          }
        }
      }
    const auto hash=total.finish();
    std::printf("total %zu %s\n",rows,hash.c_str());
    }
  };

int main() {
  try {
    // Tempest logs use cout; only digest records use the C stdout stream.
    std::cout.rdbuf(std::cerr.rdbuf());
    const auto* game=std::getenv("GOTHIC_DIR");
    const auto* dir=std::getenv("OPENGOTHIC_BASE");
    if(!game || !*game || !dir || !*dir)
      throw std::runtime_error("GOTHIC_DIR and OPENGOTHIC_BASE must be set");
    const auto base=fs::absolute(dir);
    setenv("OPENGOTHIC_CONTENT","",1); setenv("OPENGOTHIC_TEXTURES","",1);
    const char* args[]={"gothic-asset-digest","-g",game};
    CommandLine commandLine(3,args);
    Tempest::VulkanApi api;
    Tempest::Device device(api);
    AssetDigest digest;
    digest.run(base,device);
    return 0;
    }
  catch(const std::exception& error) {
    std::fprintf(stderr,"asset digest: %s\n",error.what());
    return 1;
    }
  }

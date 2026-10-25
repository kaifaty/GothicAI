#include "asset/fromzenkit.h"
#include "basemesh.h"
#include "commandline.h"
#include "gltfmesh.h"
#include "gothicglb.h"
#include "graphics/mesh/submesh/packedmesh.h"
#include "physics/physicvbo.h"
#include "resources.h"

#include <zenkit/Stream.hh>
#include <zenkit/MultiResolutionMesh.hh>
#include <zenkit/Model.hh>
#include <zenkit/ModelScript.hh>
#include <zenkit/Archive.hh>
#include <zenkit/Logger.hh>
#include <zenkit/Texture.hh>
#include <zenkit/addon/texcvt.hh>
#include <Tempest/MemReader>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <stdexcept>

namespace fs = std::filesystem;

struct BaseContentContract {
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

  static void census(const fs::path& file, size_t meshes) {
    const auto data=json(file);
    equal(data["replacements"].Size(),meshes,"census base mesh count");
    for(const auto& row:data["replacements"].GetArray()) {
      require(std::string(row["kind"].GetString())=="basemesh","base mesh census kind");
      require(std::string(row["replacement"].GetString()).starts_with("base/meshes/"),"base mesh census path");
      }
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
      auto file=Resources::getFileBuffer(name);
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
    auto input=Resources::getFileBuffer("BLATTCRAWLER.MSB");
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

  static void run(const fs::path& base, const fs::path& temporary) {
    setenv("OPENGOTHIC_CONTENT","",1);
    setenv("OPENGOTHIC_TEXTURES","",1);
    const auto censusPath=temporary/"census.json";
    setenv("OPENGOTHIC_CENSUS",censusPath.c_str(),1);
    std::string meshKey;
    size_t textureCount=0,meshCount=0,dds=0,png=0,tga=0,vertices=0,indices=0,meshlets=0;
    {
    Resources resources;
    require(!resources.baseTextures.enabled && !resources.baseMeshes.enabled,"base must wait for mounted archives");
    Resources::loadVdfs({},true);
    require(resources.baseTextures.enabled && resources.baseMeshes.enabled,"matching archives must enable both indices");
    require(!resources.baseTextures.files.empty() && !resources.baseMeshes.files.empty(),"both indices must contain files");
    legacyModelBounds();
    modelTagBoundary();
    for(const auto& [key,file]:resources.baseTextures.files) {
      auto* entry=Resources::vdfsIndex().find(key+"-C.TEX");
      const bool compiled=entry!=nullptr;
      if(!compiled)
        entry=Resources::vdfsIndex().find(key+".TGA");
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
      auto input=Resources::getFileBuffer(key+".MRM");
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
    census(censusPath,meshCount);
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
      Resources::loadVdfs({},true);
      }
    catch(const BaseStore::Error& error) { rejected=std::string(error.what()).find("OPENGOTHIC_BASE")!=std::string::npos; }
    require(rejected,"invalid explicit base must fail with recovery instructions");
    setenv("OPENGOTHIC_BASE",base.c_str(),1);
    {
    Resources resources;
    Resources::loadVdfs({},true);
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
    Resources::loadVdfs({},true);
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

#include "commandline.h"
#include "graphics/material.h"
#include "graphics/mesh/skeleton.h"
#include "graphics/mesh/submesh/packedmesh.h"
#include "graphics/meshobjects.h"
#include "graphics/rtscene.h"
#include "graphics/sceneglobals.h"
#include "graphics/shaders.h"
#include "resources.h"
#include "world/chunkresidency.h"

#include <Tempest/CommandBuffer>
#include <Tempest/Device>
#include <Tempest/Fence>
#include <Tempest/VulkanApi>
#include <rapidjson/document.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <numeric>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>

namespace {
size_t assertions = 0;

void require(bool condition, const char* message) {
  ++assertions;
  if(!condition)
    throw std::runtime_error(message);
  }

void textFile(const std::filesystem::path& path, std::string_view text) {
  std::ofstream out(path, std::ios::binary);
  out.write(text.data(), std::streamsize(text.size()));
  if(!out)
    throw std::runtime_error("unable to write material fixture");
  }

// Valid compressed 4x4 blocks, optionally with the complete 2x2 and 1x1 mip
// chain. Originals deliberately have the opposite format to their replacements.
void ddsFile(const std::filesystem::path& path, bool alpha, bool mipmaps=false) {
  std::array<uint8_t,176> bytes{};
  const size_t mipCount=mipmaps ? 3 : 1;
  const size_t blockSize=alpha ? 16 : 8;
  auto word = [&](size_t at, uint32_t value) {
    for(size_t i=0;i<4;++i)
      bytes[at*4+i] = uint8_t(value >> (i*8));
    };
  word(0,0x20534444); // DDS
  word(1,124);
  word(2,mipmaps ? 0xa1007 : 0x81007);
  word(3,4);
  word(4,4);
  word(5,alpha ? 16 : 8);
  word(7,uint32_t(mipCount));
  word(19,32);
  word(20,4);
  word(21,alpha ? 0x35545844 : 0x31545844); // DXT5 / DXT1
  word(27,mipmaps ? 0x401008 : 0x1000);
  if(alpha)
    for(size_t mip=0;mip<mipCount;++mip)
      bytes[128+mip*blockSize] = 255;
  std::ofstream out(path, std::ios::binary);
  out.write(reinterpret_cast<const char*>(bytes.data()),std::streamsize(128+mipCount*blockSize));
  if(!out)
    throw std::runtime_error("unable to write DDS fixture");
  }

struct Fixture final {
  std::filesystem::path root = std::filesystem::temp_directory_path()/
      ("gothic-material-texture-"+std::to_string(getpid()));

  Fixture() {
    std::filesystem::create_directories(root/"materials");
    std::filesystem::create_directories(root/"_work");
    ddsFile(root/"materials/solid.dds",false);
    ddsFile(root/"materials/alpha.dds",true);
    for(const auto* name:{"NW_CITY_ROPE_01.TGA","HUM_BODY_V0_C0.TGA","HUM_BODY_V1_C0.TGA"})
      ddsFile(root/"_work"/(std::filesystem::path(name).stem().string()+".dds"),true,true);
    for(const auto* name:{"NW_CITY_WOOD_01.TGA","NW_HARBOUR_WOODCRATE_01.TGA",
                         "NW_HARBOUR_WOODCRATE_02.TGA","HUM_BODY_V2_C3.TGA",
                         "HUM_BODY_V4_C5.TGA","UNLISTED.TGA","PINNED.TGA"})
      ddsFile(root/"_work"/(std::filesystem::path(name).stem().string()+".dds"),false,true);
    textFile(root/"materials/materials.json",R"({
      "NW_CITY_ROPE_01.TGA":{"albedo":"solid.dds"},
      "NW_CITY_WOOD_01.TGA":{"albedo":"alpha.dds"},
      "NW_HARBOUR_WOODCRATE_01.TGA":{"albedo":"alpha.dds"},
      "NW_HARBOUR_WOODCRATE_02.TGA":{"albedo":"alpha.dds"},
      "HUM_BODY_V0_C0.TGA":{"albedo":"solid.dds"},
      "HUM_BODY_V2_C3.TGA":{"albedo":"alpha.dds"}
    })");
    setenv("OPENGOTHIC_CONTENT",root.c_str(),1);
    setenv("OPENGOTHIC_TEXTURES",(root/"_work").c_str(),1);
    setenv("OPENGOTHIC_CENSUS",(root/"census.json").c_str(),1);
    setenv("TEMPEST_AUDIO_LOOPBACK","48000",1);
    }

  ~Fixture() {
    std::error_code error;
    std::filesystem::remove_all(root,error);
    }
  };

asset::Material source(std::string_view name) {
  asset::Material ret;
  ret.texture = name;
  ret.color = {255,255,255,255};
  ret.alpha_func = asset::AlphaFunction::NONE;
  return ret;
  }

void variants() {
  for(const auto* name:{"HUM_BODY_V0.TGA","HUM_BODY_C0.TGA","HUM_BODY_V0_C0.TGA"})
    require(Resources::hasTextureVariant(name),"a real variation tag must be recognized");
  for(const auto* name:{"NW_CITY_ROPE_01.TGA","NW_HARBOUR_WOODCRATE_01.TGA",
                       "WOOD_CARVING.TGA","V0_CLOTH.TGA","HUM_BODY_V2_C3.TGA",
                       "HUM_BODY_v0_c0.TGA",""})
    require(!Resources::hasTextureVariant(name),"ordinary letters must not enable texture variation");
  require(Resources::textureVariantName("HUM_BODY_V0_C0.TGA",2,3)=="HUM_BODY_V2_C3.TGA",
          "both variation tags must be substituted");
  require(Resources::textureVariantName("HUM_BODY_C0_V0.TGA",12,34)=="HUM_BODY_C34_V12.TGA",
          "multi-digit variations and reversed tag order must work");
  require(Resources::textureVariantName("HUM_V0_V0_C0_C0.TGA",2,3)=="HUM_V2_V0_C3_C0.TGA",
          "only the first tag of each kind is substituted, as before");
  require(Resources::textureVariantName("NW_CITY_ROPE_01.TGA",2,3)=="NW_CITY_ROPE_01.TGA",
          "CITY must remain unchanged");
  const auto longName = std::string(128,'X')+"_V0_C0.TGA";
  require(Resources::textureVariantName(longName,2,3)==longName,"long-name fallback must remain unchanged");
  }

void opaqueStaticVariant(MeshObjects& owner, const asset::StaticMesh& mesh, bool ownAlbedo) {
  PackedMesh packed(mesh,PackedMesh::PK_Visual);
  packed.isUsingAlphaTest = false;
  StaticMesh rigid(packed);
  require(rigid.sub.size()==1,"the opaque fixture must contain one real static submesh");
  require(rigid.sub[0].material.alpha==Material::Solid,
          "StaticMesh must pass the mesh's disabled alpha-test flag to its material");

  MeshObjects::Mesh selected(owner,rigid,ownAlbedo ? 2 : 1,0,ownAlbedo ? 3 : 0);
  require(selected.nodesCount()==1,"the renderer must allocate the selected opaque static submesh");
  const auto& material = selected.node(0).material();
  const auto* expected = Resources::loadMaterialAlbedo(ownAlbedo ? "HUM_BODY_V2_C3.TGA" : "HUM_BODY_V1_C0.TGA");
  require(material.tex==expected,"the static draw bucket must use the selected DXT5 variant");
  require(material.tex!=nullptr && material.tex->format()==Tempest::TextureFormat::DXT5,
          "the opaque static variant must exercise a DXT5 texture");
  require(material.alpha==Material::Solid,
          "a DXT5 variant must preserve the static mesh's disabled alpha-test flag");

  Material fragment;
  fragment.setAlbedo(expected);
  require(fragment.tex==expected && fragment.alpha==Material::Solid,
          "a default fracture material must remain solid with a DXT5 albedo");

  Material enabled(source("UNLISTED.TGA"),true);
  Material disabled(source("UNLISTED.TGA"),false);
  require(enabled.alpha==Material::Solid && disabled.alpha==Material::Solid && !(enabled==disabled),
          "equal current formats must not hide a different alpha-test policy");
  }

void sourceAlphaPolicy(MeshObjects& owner, const asset::ModelMesh& model, bool ownAlbedo) {
  for(auto function:{asset::AlphaFunction::BLEND,asset::AlphaFunction::NONE}) {
    for(uint8_t colorAlpha:{uint8_t(255),uint8_t(128)}) {
      auto baseSource = source("HUM_BODY_V0_C0.TGA");
      baseSource.alpha_func = function;
      baseSource.color.a = colorAlpha;
      auto variantSource = baseSource;
      variantSource.texture = "HUM_BODY_V2_C3.TGA";
      const Material base(baseSource,true);
      const Material variant(variantSource,true);
      if(function==asset::AlphaFunction::BLEND && colorAlpha==255)
        require(base.alpha==(ownAlbedo ? Material::Solid : Material::Transparent),
                "the BLEND fixture must exercise the DXT1 solid optimization in one direction");

      Material changed = base;
      changed.setAlbedo(Resources::loadTexture(baseSource.texture,2,3));
      require(changed.tex==variant.tex,"the policy test must select the variant's actual albedo");
      require(changed.alpha==variant.alpha,
              "albedo changes must preserve the source alpha function and color alpha like the variant constructor");
      changed.setAlbedo(Resources::loadTexture(baseSource.texture,0,0));
      require(changed.tex==base.tex,"the reverse policy test must restore the base albedo");
      require(changed.alpha==base.alpha,
              "reverse albedo changes must classify alpha like the base constructor");

      if(function==asset::AlphaFunction::BLEND) {
        auto blendModel = model;
        blendModel.meshes[0].mesh.sub_meshes[0].mat = baseSource;
        ProtoMesh blendProto(blendModel,nullptr,"MATERIAL_TEXTURE_BLEND.MDM");
        MeshObjects::Mesh selected(owner,blendProto,2,0,3,false);
        require(selected.nodesCount()==1,"the renderer must allocate the selected BLEND skinned submesh");
        const auto& material = selected.node(0).material();
        require(material.tex==variant.tex,"the BLEND skinned draw bucket must use the selected albedo");
        require(material.alpha==variant.alpha,
                "the BLEND skinned draw bucket must classify alpha like the variant constructor");
        }
      }
    }
  }

void skinnedVariant(bool ownAlbedo) {
  // Use the renderer owner: the CPU-only MeshObjects path does not select
  // skinned texture variants or allocate the resulting material's draw bucket.
  Shaders shaders;
  shaders.waitCompiler();
  SceneGlobals globals;
  VisualObjects visuals(globals,{{0,0,0},{1,1,0}});
  MeshObjects owner(visuals);

  asset::ModelMesh model;
  model.meshes.resize(1);
  auto& skin = model.meshes[0];
  skin.mesh.positions = {{0,0,0},{1,0,0},{0,1,0}};
  skin.mesh.bbox = {{0,0,0},{1,1,0}};
  skin.mesh.sub_meshes.resize(1);
  auto& surface = skin.mesh.sub_meshes[0];
  surface.mat = source("HUM_BODY_V0_C0.TGA");
  surface.wedges = {{{0,0,1},{0,0},0},{{0,0,1},{1,0},1},{{0,0,1},{0,1},2}};
  surface.triangles = {{{0,1,2}}};
  skin.weights = {{{1.f,{0,0,0},0}},{{1.f,{1,0,0},0}},{{1.f,{0,1,0},0}}};
  ProtoMesh proto(model,nullptr,"MATERIAL_TEXTURE_SKIN.MDM");
  require(proto.skined.size()==1 && proto.skined[0].sub.size()==1,
          "the variant fixture must contain one real skinned submesh");
  const auto& base = proto.skined[0].sub[0].material;
  require(base.tex!=nullptr && base.tex->format()==
              (ownAlbedo ? Tempest::TextureFormat::DXT1 : Tempest::TextureFormat::DXT5),
          "the skinned source must use the base texture's actual format");
  require(base.alpha==(ownAlbedo ? Material::Solid : Material::AlphaTest),
          "the skinned source alpha must be classified from the base texture");

  MeshObjects::Mesh selected(owner,proto,2,0,3,false);
  require(selected.nodesCount()==1,"the renderer must allocate the selected skinned submesh");
  const auto& material = selected.node(0).material();
  require(material.tex==Resources::loadMaterialAlbedo("HUM_BODY_V2_C3.TGA"),
          "the skinned draw bucket must use the selected variant's albedo");
  require(material.tex!=nullptr && material.tex->format()==
              (ownAlbedo ? Tempest::TextureFormat::DXT5 : Tempest::TextureFormat::DXT1),
          "the skinned variant must use its own DDS format");
  require(material.alpha==(ownAlbedo ? Material::AlphaTest : Material::Solid),
          "the skinned variant draw bucket must reclassify alpha from its selected albedo");
  opaqueStaticVariant(owner,skin.mesh,ownAlbedo);
  sourceAlphaPolicy(owner,model,ownAlbedo);
  }

void replacements(Tempest::Device& device, const Fixture& fixture) {
  Resources resources(device);
  const auto* solid = Resources::loadMaterialAlbedo("NW_CITY_ROPE_01.TGA");
  const auto* alpha = Resources::loadMaterialAlbedo("NW_CITY_WOOD_01.TGA");
  require(solid!=nullptr && solid->format()==Tempest::TextureFormat::DXT1,
          "direct rope load must select our DXT1 albedo");
  require(alpha!=nullptr && alpha->format()==Tempest::TextureFormat::DXT5,
          "direct wood load must select our DXT5 albedo");
  require(Resources::loadMaterialAlbedo("nw_city_rope_01.tga")==solid,
          "material replacement lookup must be case insensitive");
  require(Resources::loadMaterialAlbedo("NW_HARBOUR_WOODCRATE_01.TGA")==alpha,
          "first fragment texture must select our albedo");
  require(Resources::loadMaterialAlbedo("NW_HARBOUR_WOODCRATE_02.TGA")==alpha,
          "second fragment texture must select our albedo");
  require(Resources::loadTexture("HUM_BODY_V0_C0.TGA",2,3)==alpha,
          "the substituted variant name must be looked up in materials.json");
  require(Resources::loadTexture("HUM_BODY_V0_C0.TGA",0,0)==solid,
          "the base variation must use its own replacement");
  require(Resources::loadTexture("HUM_BODY_V0_C0.TGA",4,5)==Resources::loadTexture("HUM_BODY_V4_C5.TGA"),
          "an unlisted variant must load its original, not the base replacement");
  const auto* original = Resources::loadMaterialAlbedo("UNLISTED.TGA");
  require(original!=nullptr && original==Resources::loadTexture("UNLISTED.TGA"),
          "an unlisted texture must use the original loader");

  Material rope(source("NW_CITY_ROPE_01.TGA"),true);
  Material wood(source("NW_CITY_WOOD_01.TGA"),true);
  require(rope.tex==solid && rope.alpha==Material::Solid,"ordinary DXT1 replacement must be solid");
  require(wood.tex==alpha && wood.alpha==Material::AlphaTest,"ordinary DXT5 replacement must use alpha test");
  rope.setAlbedo(Resources::loadTexture("HUM_BODY_V0_C0.TGA",2,3));
  require(rope.tex==alpha && rope.alpha==Material::AlphaTest,"variant DXT5 must reclassify a solid material");
  wood.setAlbedo(solid);
  require(wood.tex==solid && wood.alpha==Material::Solid,"fragment DXT1 must reclassify an alpha-tested material");
  for(auto mode:{Material::Water,Material::Ghost,Material::Multiply,Material::Multiply2,
                 Material::Transparent,Material::AdditiveLight}) {
    wood.alpha = mode;
    wood.setAlbedo(alpha);
    require(wood.alpha==mode,"albedo replacement must preserve explicit blending modes");
    }
  skinnedVariant(true);
  require(Resources::writeCensus(),"replacement census must be writable");
  }

void census(const Fixture& fixture) {
  std::ifstream in(fixture.root/"census.json",std::ios::binary);
  const std::string text{std::istreambuf_iterator<char>(in),std::istreambuf_iterator<char>()};
  rapidjson::Document data;
  data.Parse(text.data(),text.size());
  require(!data.HasParseError() && data.IsObject(),"census must contain JSON");
  require(data.HasMember("files") && data["files"].IsArray(),"census must contain opened files");
  for(const auto& entry:data["files"].GetArray()) {
    const std::string_view name(entry["name"].GetString());
    require(name=="UNLISTED.TGA" || name=="HUM_BODY_V4_C5.TGA",
            "no original with a replacement may be opened");
    }
  require(data.HasMember("replacements") && data["replacements"].IsArray(),"census must contain replacements");
  size_t materialReplacements=0, originalTextures=0;
  for(const auto& entry:data["replacements"].GetArray()) {
    const std::string_view name(entry["name"].GetString());
    const std::string_view replacement(entry["replacement"].GetString());
    if(replacement=="materials/solid.dds" || replacement=="materials/alpha.dds") {
      ++materialReplacements;
      } else {
      require(name=="UNLISTED.TGA" || name=="HUM_BODY_V4_C5.TGA",
              "no original with a material replacement may be opened through the fixture DDS index");
      const auto expected=fixture.root/"_work"/(std::filesystem::path(name).stem().string()+".dds");
      require(replacement==expected.string(),"the original loader must read the indexed fixture DDS");
      ++originalTextures;
      }
    }
  require(materialReplacements==6,"all four city textures, base and selected variant must be counted");
  require(originalTextures==2,"both unlisted originals must be read through the texture loader");
  }

void emptyTable(Tempest::Device& device, const Fixture& fixture) {
  textFile(fixture.root/"materials/materials.json","{}");
  Resources resources(device);
  require(!Resources::materialMapsEnabled(),"empty materials.json must disable material maps");
  for(const auto* name:{"NW_CITY_ROPE_01.TGA","NW_CITY_WOOD_01.TGA",
                       "NW_HARBOUR_WOODCRATE_01.TGA","NW_HARBOUR_WOODCRATE_02.TGA",
                       "HUM_BODY_V0_C0.TGA","HUM_BODY_V2_C3.TGA","UNLISTED.TGA"}) {
    const auto* original = Resources::loadTexture(name);
    require(original!=nullptr && Resources::loadMaterialAlbedo(name)==original,
            "empty materials.json must preserve the original texture selection");
    }
  require(Resources::loadTexture("HUM_BODY_V0_C0.TGA",2,3)==Resources::loadTexture("HUM_BODY_V2_C3.TGA"),
          "empty materials.json must preserve original variant selection");
  Material rope(source("NW_CITY_ROPE_01.TGA"),true);
  Material wood(source("NW_CITY_WOOD_01.TGA"),true);
  require(rope.alpha==Material::AlphaTest && wood.alpha==Material::Solid,
          "without replacements alpha classification must use the original formats");
  skinnedVariant(false);
  }

void textureResidency(Tempest::Device& device, const Fixture& fixture) {
  std::string materials=R"({"STREAM.TGA":{"albedo":"solid.dds","normal":"alpha.dds","orm":"alpha.dds"})";
  for(size_t i=0;i<17;++i) {
    const auto name="lru-"+std::to_string(i)+".dds";
    ddsFile(fixture.root/"materials"/name,false);
    materials+=",\"LRU_"+std::to_string(i)+".TGA\":{\"albedo\":\""+name+"\"}";
    }
  textFile(fixture.root/"materials/materials.json",materials+"}");
  setenv("OPENGOTHIC_TEXTURE_BUDGET_MB","0",1);
  setenv("OPENGOTHIC_TEXTURE_STREAMING","1",1);
  Resources resources(device);
  Shaders shaders;
  shaders.waitCompiler();
  const auto baselineBytes=Tempest::Texture2d::liveBytes();
  const auto baselineCount=Tempest::Texture2d::liveCount();
  {
  auto payload=device.texture(Tempest::Pixmap(1,1,Tempest::TextureFormat::RGBA8),false);
  auto shared=payload.shared();
  Tempest::Texture2d moved(std::move(payload));
  require(payload.isEmpty() && !moved.isEmpty() && !shared.isEmpty() &&
          Tempest::Texture2d::liveBytes()==baselineBytes+4 && Tempest::Texture2d::liveCount()==baselineCount+1,
          "shared and moved wrappers must count their single native texture allocation once");
  moved.markRetired();
  shared.markRetired();
  require(Tempest::Texture2d::liveBytes()==baselineBytes && Tempest::Texture2d::liveCount()==baselineCount,
          "retirement must remove a shared payload from live counters exactly once");
  }
  require(Tempest::Texture2d::liveBytes()==baselineBytes && Tempest::Texture2d::liveCount()==baselineCount,
          "destroying retired shared wrappers must not subtract live counters again");
  Material material(source("STREAM.TGA"),true);
  material.frames={material.tex,Resources::loadTexture("UNLISTED.TGA",false,false)};
  material.texAniFPSInv=100;
  const Material copy=material;
  require(material.tex!=nullptr && material.normal!=nullptr && material.orm!=nullptr &&
          material.frames.back()!=nullptr,"streaming fixture must load every material texture kind");
  require(material.normal!=material.orm,"normal and ORM must retain distinct identities for a shared file");
  const auto* pinned=Resources::loadTexture("PINNED.TGA");
  const auto initial=Resources::textureStats();
  const auto generation=Resources::textureGeneration();
  // Albedo/normal/ORM each have one block; the original animation frame has three mips.
  const auto fullBytes=uint64_t(8+16+16+3*8);
  const auto isFull=[&] {
    return material.tex->w()==4 && material.normal->w()==4 && material.orm->w()==4 &&
           material.frames.back()->w()==4;
    };

  std::vector<Resources::Vertex> vertices={
    {{0,0,0},{0,0,1},{0,0},0xffffffffu},
    {{1,0,0},{0,0,1},{1,0},0xffffffffu},
    {{0,1,0},{0,0,1},{0,1},0xffffffffu}};
  StaticMesh mesh(material,std::move(vertices),{0,1,2});
  DrawBuckets buckets;
  auto bucket=buckets.alloc(material,mesh);
  const auto commit=[&] {
    auto command=device.commandBuffer();
    bool changed=false;
    {
    auto encoder=command.startEncoding(device);
    changed=buckets.commit(encoder,0);
    }
    device.submit(command).wait();
    return changed;
    };
  require(commit(),"initial bucket commit must create its descriptor arrays");
  if(Shaders::options().doBindless)
    require(!buckets.textures().isEmpty(),"animated material must have a real bindless array");
  require(!commit(),"unchanged texture payloads must not rebuild descriptor arrays");

  RtScene rayScene;
  auto blas=Resources::blas(mesh.vbo,mesh.ibo,0,3);
  if(!blas.isEmpty()) {
    rayScene.addInstance(Tempest::Matrix4x4::mkIdentity(),blas,material,mesh,0,3,RtScene::Static);
    rayScene.buildTlas();
    require(!rayScene.tex.isEmpty() && !rayScene.refreshTextures(),
            "a built RT array must retain its current texture generation");
    }

  ChunkTable table;
  table.entries.push_back({{0,0},"",{0,0,0},{1,1,0},1});
  ChunkResidency residency(table);
  require(residency.indexOf({0,0})==0 && residency.indexOf({1,0})==size_t(-1),
          "chunk identity lookup must distinguish a table entry from an absent cell");
  const auto evict=[&](uint64_t expected) {
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(Resources::textureStats().evicted<expected && std::chrono::steady_clock::now()<deadline) {
      const auto before=Resources::textureStats().evicted;
      const auto generation=Resources::textureGeneration();
      Resources::updateTextures(&residency);
      require(Resources::textureStats().evicted-before<=16 && Resources::textureGeneration()-generation<=1,
              "a texture update must retire at most sixteen payloads and publish one generation");
      }
    };
  Resources::textureDemand(material,Resources::TextureCategory::Landscape,true);
  Resources::textureDemand(material,Resources::TextureCategory::Dynamic,true);
  Resources::updateTextures(&residency);
  require(isFull() && Resources::textureStats().evicted==0,
          "all demanded textures must survive a zero budget");
  Resources::textureDemand(material,Resources::TextureCategory::Landscape,false);
  Resources::updateTextures(&residency);
  require(isFull(),"removing one owner must preserve another owner's texture demand");
  Resources::textureDemand(material,Resources::TextureCategory::Dynamic,false);
  device.waitIdle();
  Resources::resetRecycled(0);
  evict(4);
  const auto evicted=Resources::textureStats();
  require(evicted.evicted==4 && evicted.retiredBytes==fullBytes,
          "zero-demand textures must retire every full payload exactly once");
  require(evicted.liveBytes==initial.liveBytes-fullBytes+12 && evicted.count==initial.count-1 &&
          evicted.peakBytes>=initial.liveBytes,
          "live texture bytes and count must include three shared neutral GPU images exactly once");
  require(std::all_of(evicted.categoryBytes.begin(),evicted.categoryBytes.end(),[&](auto bytes) { return bytes<=evicted.liveBytes; }) &&
          std::all_of(evicted.categoryCount.begin(),evicted.categoryCount.end(),[&](auto count) { return count<=evicted.count; }) &&
          std::accumulate(evicted.categoryBytes.begin(),evicted.categoryBytes.end(),uint64_t(0))==evicted.liveBytes &&
          std::accumulate(evicted.categoryCount.begin(),evicted.categoryCount.end(),uint64_t(0))==evicted.count,
          "texture categories must partition native live bytes and counts without double-counting placeholders");
  require(Resources::textureGeneration()>generation && material==copy && bucket->mat==copy &&
          mesh.sub[0].material==copy,"eviction must preserve material, mesh, frame and bucket identities");
  require(pinned!=nullptr && pinned->w()==4,"UI texture loads must remain pinned above budget");
  const auto pixel=[&](const Tempest::Texture2d* texture) {
    require(texture->w()==1 && texture->h()==1,"eviction must publish a one-pixel placeholder");
    const auto image=device.readPixels(*texture);
    require(image.format()==Tempest::TextureFormat::RGBA8,"placeholder readback must preserve RGBA bytes");
    std::array<uint8_t,4> result{};
    std::copy_n(static_cast<const uint8_t*>(image.data()),result.size(),result.begin());
    return result;
    };
  require(pixel(material.tex)==std::array<uint8_t,4>{255,255,255,255},
          "color placeholder must be opaque neutral white");
  const auto normal=pixel(material.normal);
  require(normal[1]==128 && normal[3]==128,"DXT5nm placeholder must have zero alpha/green slope");
  require(pixel(material.orm)==std::array<uint8_t,4>{255,255,0,255},
          "ORM placeholder must be unoccluded, rough and nonmetallic");
  require(commit() && !commit(),"payload replacement must refresh bindless arrays without rebuilding buckets");
  if(!blas.isEmpty())
    require(rayScene.refreshTextures() && !rayScene.refreshTextures(),
            "payload replacement must refresh RT descriptors once without rebuilding the TLAS");
  Resources::resetRecycled(1);
  require(Resources::textureStats().retiredBytes==fullBytes,
          "another frame slot must not free retired texture payloads");
  device.waitIdle();
  Resources::resetRecycled(0);
  require(Resources::textureStats().retiredBytes==0,"the completed frame slot must free retired texture payloads");

  Resources::textureDemand(material,Resources::TextureCategory::Landscape,true);
  require(!Resources::texturesReady(),"new demand must immediately register missing resident payloads");
  const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
  while(Resources::textureStats().reloaded<4 && std::chrono::steady_clock::now()<deadline) {
    residency.update({0,0,0},{0,0,0},1000);
    Resources::updateTextures(&residency);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  require(Resources::textureStats().reloaded==4 && isFull() && Resources::texturesReady(),
          "new demand must reload every evicted texture through the frame queue");
  require(material==copy && bucket->mat==copy && commit(),
          "reload must keep identities while refreshing actual bindless descriptors");
  if(!blas.isEmpty())
    require(rayScene.refreshTextures(),"reload must refresh retained RT texture identities");
  Resources::textureDemand(material,Resources::TextureCategory::Landscape,false);
  setenv("OPENGOTHIC_TEXTURE_STREAMING","0",1);
  Resources::configureTextures();
  Resources::updateTextures(&residency);
  require(isFull() && Resources::textureStats().evicted==evicted.evicted,
          "the texture streaming opt-out must disable eviction above budget");

  // Use the actual render owners as well as the direct shared-owner contract.
  setenv("OPENGOTHIC_TEXTURE_STREAMING","1",1);
  Resources::configureTextures();
  SceneGlobals scene;
  VisualObjects visuals(scene,{{0,0,0},{1,1,0}});
  visuals.setTextureResidency(&residency);
  auto stationary=visuals.get(mesh,material,0,mesh.sub[0].iboLength,true);
  auto movable=visuals.get(mesh,material,0,mesh.sub[0].iboLength,false);
  residency.update({1000000,0,1000000},{1000000,0,1000000},1000);
  Resources::updateTextures(&residency);
  require(isFull(),"an off-chunk static owner must not cancel a moving owner's shared demand");
  movable=VisualObjects::Item();
  evict(evicted.evicted+4);
  require(material.tex->w()==1 && Resources::textureStats().evicted==evicted.evicted+4,
          "an Outside static prototype must not retain textures after the moving visual disappears");

  // One more identity than a frame may retire makes the LRU slice observable.
  std::array<Material,17> lru;
  for(size_t i=0;i<lru.size();++i) {
    lru[i]=Material(source("LRU_"+std::to_string(i)+".TGA"),true);
    Resources::textureDemand(lru[i],Resources::TextureCategory::Static,true);
    Resources::textureDemand(lru[i],Resources::TextureCategory::Static,false);
    }
  // Environment mutations take effect only when configuration is refreshed.
  setenv("OPENGOTHIC_TEXTURE_BUDGET_MB","1",1);
  const auto beforeSlice=Resources::textureStats().evicted;
  const auto beforeGeneration=Resources::textureGeneration();
  Resources::updateTextures(&residency);
  const auto retired=Resources::textureStats().evicted-beforeSlice;
  require(retired>0 && retired<=16 && Resources::textureGeneration()==beforeGeneration+1,
          "cached zero budget must retire a bounded LRU slice with one generation");
  for(size_t i=0;i<lru.size();++i)
    require(lru[i].tex!=nullptr && lru[i].tex->w()==(i<retired ? 1 : 4),
            "incremental LRU retirement must follow the last owner release order");
  Resources::textureDemand(lru.back(),Resources::TextureCategory::Dynamic,true);
  evict(beforeSlice+16);
  require(lru.back().tex->w()==4,"late demand must remove a resident texture from the eviction queue");
  Resources::textureDemand(lru.back(),Resources::TextureCategory::Dynamic,false);
  Resources::configureTextures();
  Resources::updateTextures(&residency);
  require(lru.back().tex->w()==4 && Resources::textureStats().evicted==beforeSlice+16,
          "configuration refresh must apply the new budget without evicting an eligible texture below it");
  device.waitIdle();
  setenv("OPENGOTHIC_TEXTURE_STREAMING","1",1);
  unsetenv("OPENGOTHIC_TEXTURE_BUDGET_MB");
  }
}

int main() {
  try {
    variants();
    Fixture fixture;
    CommandLine commandLine(0,nullptr);
    Tempest::VulkanApi api;
    Tempest::Device device(api);
    replacements(device,fixture);
    census(fixture);
    emptyTable(device,fixture);
    textureResidency(device,fixture);
    device.waitIdle();
    std::cout << "{\"test\":\"material_texture\",\"passed\":true,\"assertions\":" << assertions << "}\n";
    return 0;
    }
  catch(const std::exception& error) {
    std::cerr << "material texture test failed: " << error.what() << '\n';
    return 1;
    }
  }

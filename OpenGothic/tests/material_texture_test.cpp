#include "commandline.h"
#include "graphics/material.h"
#include "graphics/mesh/submesh/animmesh.h"
#include "graphics/mesh/skeleton.h"
#include "graphics/mesh/submesh/packedmesh.h"
#include "graphics/mdlvisual.h"
#include "graphics/meshobjects.h"
#include "graphics/rtscene.h"
#include "graphics/sceneglobals.h"
#include "graphics/shaders.h"
#include "graphics/mesh/texeldensity.h"
#include "graphics/texturelookahead.h"
#include "graphics/texturescreendemand.h"
#include "resources.h"
#include "texturebudget.h"
#include "textureflow.h"
#include "texturelevels.h"
#include "world/chunkresidency.h"

#include <Tempest/CommandBuffer>
#include <Tempest/Device>
#include <Tempest/Fence>
#include <Tempest/MemReader>
#include <Tempest/VulkanApi>
#include <rapidjson/document.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <numeric>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <vector>

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

struct CountingReader final : Tempest::MemReader {
  using MemReader::MemReader;
  size_t bytesRead = 0;
  size_t bytesSkipped = 0;

  size_t read(void* to, size_t size) override {
    const auto count = MemReader::read(to,size);
    bytesRead += count;
    return count;
    }

  size_t seek(size_t size) override {
    const auto count = MemReader::seek(size);
    bytesSkipped += count;
    return count;
    }
  };

size_t mipBytes(uint32_t width, uint32_t height, size_t blockSize) {
  return size_t((width+3)/4)*((height+3)/4)*blockSize;
  }

std::vector<uint8_t> mipDds(uint32_t fourCC, uint32_t width, uint32_t height, uint32_t mipCount) {
  std::vector<uint8_t> bytes(128);
  auto word = [&](size_t at, uint32_t value) {
    for(size_t i=0;i<4;++i)
      bytes[at*4+i] = uint8_t(value >> (i*8));
    };
  const size_t blockSize = fourCC==0x31545844 ? 8 : 16;
  word(0,0x20534444);
  word(1,124);
  word(2,0xA1007);
  word(3,height);
  word(4,width);
  word(5,uint32_t(mipBytes(width,height,blockSize)));
  word(7,mipCount);
  word(19,32);
  word(20,4);
  word(21,fourCC);
  word(27,0x401008);
  for(uint32_t mip=0;mip<mipCount;++mip) {
    const auto offset = bytes.size();
    const auto size = mipBytes(width,height,blockSize);
    bytes.resize(offset+size);
    for(size_t i=0;i<size;++i)
      bytes[offset+i] = uint8_t(mip*37+i*13);
    width = std::max(1u,width/2);
    height = std::max(1u,height/2);
    }
  return bytes;
  }

void ddsMipTails() {
  const std::array<uint32_t,3> fourCCs = {0x31545844,0x33545844,0x35545844};
  const std::array<Tempest::TextureFormat,3> formats = {
      Tempest::TextureFormat::DXT1,Tempest::TextureFormat::DXT3,Tempest::TextureFormat::DXT5};
  for(size_t format=0;format<formats.size();++format) {
    const auto bytes = mipDds(fourCCs[format],256,128,9);
    Tempest::MemReader fullReader(bytes);
    const Tempest::Pixmap full(fullReader);
    require(full.w()==256 && full.h()==128 && full.mipCount()==9 && full.format()==formats[format],
            "full DDS load must retain its dimensions, chain and format");
    CountingReader zeroReader(bytes);
    const auto zero = Tempest::Pixmap::loadDdsMipTail(zeroReader,0);
    require(zero.w()==full.w() && zero.h()==full.h() && zero.mipCount()==full.mipCount() &&
                zero.dataSize()==full.dataSize() && std::memcmp(zero.data(),full.data(),full.dataSize())==0,
            "first mip zero must load the original DDS chain without modification");
    size_t skipped = 0;
    for(uint32_t mip=1;mip<=5;++mip) {
      skipped += mipBytes(256u>>(mip-1),128u>>(mip-1),format==0 ? 8 : 16);
      CountingReader reader(bytes);
      const auto tail = Tempest::Pixmap::loadDdsMipTail(reader,mip,4);
      require(tail.w()==(256u>>mip) && tail.h()==(128u>>mip) && tail.mipCount()==9-mip,
              "DDS tail must begin at the requested mip and retain the remaining chain");
      require(tail.format()==full.format() && tail.dataSize()==full.dataSize()-skipped,
              "DDS tail must retain its format and exact remaining byte count");
      require(std::memcmp(tail.data(),static_cast<const uint8_t*>(full.data())+skipped,tail.dataSize())==0,
              "each DDS mip tail must match the corresponding full-load bytes");
      require(reader.bytesRead<=128+tail.dataSize() && reader.bytesSkipped==skipped,
              "DDS tail must seek past discarded mips without reading their bytes");
      }
    CountingReader clampedReader(bytes);
    const auto clamped = Tempest::Pixmap::loadDdsMipTail(clampedReader,99,0);
    require(clamped.w()==8 && clamped.h()==4 && clamped.mipCount()==4,
            "BC tail selection must stop before either side drops below one 4x4 block");
    CountingReader floorReader(bytes);
    const auto floor = Tempest::Pixmap::loadDdsMipTail(floorReader,3);
    require(floor.w()==128 && floor.h()==64 && floor.mipCount()==8,
            "the default minimum side must clamp the requested DDS mip to 64");
    CountingReader customFloorReader(bytes);
    require(Tempest::Pixmap::loadDdsMipTail(customFloorReader,1,128).isEmpty(),
            "DDS with no tail above the requested minimum side must report no tail");
    const auto single = mipDds(fourCCs[format],256,128,1);
    CountingReader singleReader(single);
    require(Tempest::Pixmap::loadDdsMipTail(singleReader,1).isEmpty() && singleReader.bytesRead<=128,
            "single-level DDS must report no tail without reading or rescaling its pixels");
    Tempest::MemReader singleFullReader(single);
    const auto singleFull = Tempest::Pixmap::loadDdsMipTail(singleFullReader,0);
    require(singleFull.w()==256 && singleFull.h()==128 && singleFull.mipCount()==1,
            "first mip zero must still load a single-level DDS");

    const auto partial = mipDds(fourCCs[format],19,11,3);
    Tempest::MemReader partialFullReader(partial);
    const Tempest::Pixmap partialFull(partialFullReader);
    CountingReader partialReader(partial);
    const auto partialTail = Tempest::Pixmap::loadDdsMipTail(partialReader,1,4);
    const auto partialSkipped = mipBytes(19,11,format==0 ? 8 : 16);
    require(partialTail.w()==9 && partialTail.h()==5 && partialTail.mipCount()==2 &&
                partialTail.dataSize()==partialFull.dataSize()-partialSkipped &&
                std::memcmp(partialTail.data(),static_cast<const uint8_t*>(partialFull.data())+partialSkipped,
                            partialTail.dataSize())==0,
            "odd-sized DDS tails must round BC blocks up and preserve an incomplete chain");
    require(partialReader.bytesRead==128+partialTail.dataSize() && partialReader.bytesSkipped==partialSkipped,
            "odd-sized DDS tails must read only the header and retained BC blocks");
    }

  const auto valid = mipDds(fourCCs[0],256,128,9);
  auto fails = [](const std::vector<uint8_t>& bytes) {
    CountingReader reader(bytes);
    bool failed = false;
    try {
      Tempest::Pixmap::loadDdsMipTail(reader,2);
      }
    catch(const std::system_error&) {
      failed = true;
      }
    require(failed,"damaged or truncated DDS must fail with an asset-loading error");
    };
  auto damaged = valid;
  damaged[0] = 0;
  fails(damaged);
  damaged = valid;
  damaged.resize(127);
  fails(damaged);
  damaged = valid;
  damaged.pop_back();
  fails(damaged);
  damaged = valid;
  damaged[28] = 255;
  fails(damaged);
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

// materials.json "projection": maps by world position (reports/content/2026-11-landscape-pilot/b3-rock-material.md)
void projectionRecords(Tempest::Device& device, const Fixture& fixture) {
  ddsFile(fixture.root/"_work/ROCK_ALPHA.dds",false,true);
  textFile(fixture.root/"materials/materials.json",R"({
    "ROCK_PROJECTED.TGA":{"albedo":"solid.dds","scale":3,"projection":{"tile_m":5.75}},
    "ROCK_ANTITILE_OFF.TGA":{"albedo":"solid.dds","antitile":"off","projection":{"tile_m":2}},
    "ROCK_PLAIN.TGA":{"albedo":"solid.dds","scale":3},
    "ROCK_NUMBER.TGA":{"albedo":"solid.dds","projection":5.75},
    "ROCK_TEXT.TGA":{"albedo":"solid.dds","projection":{"tile_m":"5.75"}},
    "ROCK_EMPTY.TGA":{"albedo":"solid.dds","projection":{}},
    "ROCK_ZERO.TGA":{"albedo":"solid.dds","projection":{"tile_m":0}},
    "ROCK_NEGATIVE.TGA":{"albedo":"solid.dds","projection":{"tile_m":-5.75}},
    "ROCK_ROTATE.TGA":{"albedo":"solid.dds","antitile":"rotate","projection":{"tile_m":5.75}},
    "ROCK_OFFSET.TGA":{"albedo":"solid.dds","antitile":"offset","projection":{"tile_m":5.75}},
    "ROCK_ALPHA.TGA":{"albedo":"alpha.dds","projection":{"tile_m":5.75}}
  })");
  Resources resources(device);
  const auto maps = Resources::loadMaterialMaps("ROCK_PROJECTED.TGA");
  require(maps.present && maps.projectionTile==5.75f,"a projection record must carry its tile in metres");
  require(Resources::loadMaterialMaps("ROCK_ANTITILE_OFF.TGA").projectionTile==2.f,
          "projection must be accepted with the antitile off");
  const Material rock(source("ROCK_PROJECTED.TGA"),true);
  require(rock.hasProjection() && rock.projectionTile==5.75f && rock.alpha==Material::Solid,
          "an opaque material must keep its record's projection");
  require(std::abs(rock.textureUvPerMetre(40.f)-1.f/5.75f)<1e-6f && rock.textureUvPerMetre(0.f)==0.f,
          "the projected density must be one repeat per tile, whatever the mesh UV");
  require(std::abs(rock.projectionUvScale()-1.f/575.f)<1e-9f,"the bucket must get repeats per cm of the world");
  require(rock.textureUvPerMetre(40.f,true)==120.f,"a morph mesh is drawn by its UV and keeps its density");
  auto translucent = source("ROCK_PROJECTED.TGA");
  translucent.color.a = 128;
  const Material blended(translucent,true);
  const Material later(source("ROCK_PROJECTED.TGA"),true);
  require(blended.alpha==Material::AlphaTest && blended.projectionUvScale()==0.f && later.hasProjection(),
          "an alpha-tested zen material on an opaque texture must neither project nor drop the record for the others");
  const Material plain(source("ROCK_PLAIN.TGA"),true);
  require(plain.materialMaps && !plain.hasProjection() && plain.projectionTile==0.f && plain.uvScale==3.f &&
          plain.textureUvPerMetre(40.f)==120.f && !(plain==rock),
          "without the field a record must keep the mesh UV and its scale");
  for(const auto* name:{"ROCK_NUMBER.TGA","ROCK_TEXT.TGA","ROCK_EMPTY.TGA","ROCK_ZERO.TGA","ROCK_NEGATIVE.TGA",
                       "ROCK_ROTATE.TGA","ROCK_OFFSET.TGA"})
    require(!Resources::loadMaterialMaps(name).present,
            "a wrong type, a non-positive tile or projection with an antitile must reject the record whole");
  const auto* original = Resources::loadTexture("ROCK_ALPHA.TGA");
  require(Resources::loadMaterialMaps("ROCK_ALPHA.TGA").present,"the alpha check needs the material, not the table");
  const Material alpha(source("ROCK_ALPHA.TGA"),true);
  require(original!=nullptr && alpha.tex==original && !alpha.materialMaps && !alpha.hasProjection() &&
          alpha.alpha==Material::Solid,
          "projection on an alpha-tested texture must leave the material as without its record");
  require(!Resources::loadMaterialMaps("ROCK_ALPHA.TGA").present &&
          Resources::loadMaterialAlbedo("ROCK_ALPHA.TGA")==original,
          "the rejected record must be gone for every later material");
  Material changed = alpha;
  changed.setAlbedo(Resources::loadTexture("HUM_BODY_V0_C0.TGA"));
  require(changed.alpha==Material::AlphaTest,"the rejected material must be classified by its original texture");
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
    rayScene.setSlot(rayScene.allocSlot(),Tempest::Matrix4x4::mkIdentity(),blas,material,mesh,0,3,true);
    auto command=device.commandBuffer();
    {
    auto encoder=command.startEncoding(device);
    rayScene.commit(encoder,0);
    }
    device.submit(command).wait();
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
  // Textures without a readable tail reload the payload they had, the device's own mipmaps included.
  residency.update({0,0,0},{0,0,0},1000);
  Resources::updateTextures(&residency);
  require(Resources::textureStats().loadsInFlight==4 && Resources::textureStats().flow.bytesInFlight==fullBytes,
          "the reloads' bytes in flight must be the payloads they replace");
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

Tempest::ComputePipeline descriptorColorProbe(Tempest::Device& device) {
  // glslangValidator -V -Os; embedded to keep this contract self-contained.
  // #version 450
  // #extension GL_EXT_nonuniform_qualifier : require
  // #extension GL_EXT_samplerless_texture_functions : require
  // layout(local_size_x=2) in;
  // layout(binding=0) uniform texture2D images[];
  // layout(binding=1, std430) buffer Result { uint rgba[2]; } result;
  // void main() {
  //   uint i=gl_GlobalInvocationID.x;
  //   result.rgba[i]=packUnorm4x8(texelFetch(images[nonuniformEXT(i)],ivec2(0),0));
  // }
  static const uint32_t code[]={
    0x07230203u,0x00010000u,0x0008000bu,0x0000002bu,0x00000000u,0x00020011u,0x00000001u,0x00020011u,
    0x000014b5u,0x00020011u,0x000014b6u,0x00020011u,0x000014bbu,0x0008000au,0x5f565053u,0x5f545845u,
    0x63736564u,0x74706972u,0x695f726fu,0x7865646eu,0x00676e69u,0x0006000bu,0x00000001u,0x4c534c47u,
    0x6474732eu,0x3035342eu,0x00000000u,0x0003000eu,0x00000000u,0x00000001u,0x0006000fu,0x00000005u,
    0x00000004u,0x6e69616du,0x00000000u,0x0000000bu,0x00060010u,0x00000004u,0x00000011u,0x00000002u,
    0x00000001u,0x00000001u,0x00030003u,0x00000002u,0x000001c2u,0x00080004u,0x455f4c47u,0x6e5f5458u,
    0x6e756e6fu,0x726f6669u,0x75715f6du,0x66696c61u,0x00726569u,0x000b0004u,0x455f4c47u,0x735f5458u,
    0x6c706d61u,0x656c7265u,0x745f7373u,0x75747865u,0x665f6572u,0x74636e75u,0x736e6f69u,0x00000000u,
    0x00040005u,0x00000004u,0x6e69616du,0x00000000u,0x00080005u,0x0000000bu,0x475f6c67u,0x61626f6cu,
    0x766e496cu,0x7461636fu,0x496e6f69u,0x00000044u,0x00040005u,0x00000012u,0x75736552u,0x0000746cu,
    0x00050006u,0x00000012u,0x00000000u,0x61626772u,0x00000000u,0x00040005u,0x00000014u,0x75736572u,
    0x0000746cu,0x00040005u,0x0000001cu,0x67616d69u,0x00007365u,0x00040047u,0x0000000bu,0x0000000bu,
    0x0000001cu,0x00040047u,0x00000011u,0x00000006u,0x00000004u,0x00030047u,0x00000012u,0x00000003u,
    0x00050048u,0x00000012u,0x00000000u,0x00000023u,0x00000000u,0x00040047u,0x00000014u,0x00000021u,
    0x00000001u,0x00040047u,0x00000014u,0x00000022u,0x00000000u,0x00040047u,0x0000001cu,0x00000021u,
    0x00000000u,0x00040047u,0x0000001cu,0x00000022u,0x00000000u,0x00030047u,0x0000001eu,0x000014b4u,
    0x00030047u,0x00000020u,0x000014b4u,0x00030047u,0x00000021u,0x000014b4u,0x00040047u,0x0000002au,
    0x0000000bu,0x00000019u,0x00020013u,0x00000002u,0x00030021u,0x00000003u,0x00000002u,0x00040015u,
    0x00000006u,0x00000020u,0x00000000u,0x00040017u,0x00000009u,0x00000006u,0x00000003u,0x00040020u,
    0x0000000au,0x00000001u,0x00000009u,0x0004003bu,0x0000000au,0x0000000bu,0x00000001u,0x0004002bu,
    0x00000006u,0x0000000cu,0x00000000u,0x00040020u,0x0000000du,0x00000001u,0x00000006u,0x0004002bu,
    0x00000006u,0x00000010u,0x00000002u,0x0004001cu,0x00000011u,0x00000006u,0x00000010u,0x0003001eu,
    0x00000012u,0x00000011u,0x00040020u,0x00000013u,0x00000002u,0x00000012u,0x0004003bu,0x00000013u,
    0x00000014u,0x00000002u,0x00040015u,0x00000015u,0x00000020u,0x00000001u,0x0004002bu,0x00000015u,
    0x00000016u,0x00000000u,0x00030016u,0x00000018u,0x00000020u,0x00090019u,0x00000019u,0x00000018u,
    0x00000001u,0x00000000u,0x00000000u,0x00000000u,0x00000001u,0x00000000u,0x0003001du,0x0000001au,
    0x00000019u,0x00040020u,0x0000001bu,0x00000000u,0x0000001au,0x0004003bu,0x0000001bu,0x0000001cu,
    0x00000000u,0x00040020u,0x0000001fu,0x00000000u,0x00000019u,0x00040017u,0x00000022u,0x00000015u,
    0x00000002u,0x0005002cu,0x00000022u,0x00000023u,0x00000016u,0x00000016u,0x00040017u,0x00000024u,
    0x00000018u,0x00000004u,0x00040020u,0x00000027u,0x00000002u,0x00000006u,0x0004002bu,0x00000006u,
    0x00000029u,0x00000001u,0x0006002cu,0x00000009u,0x0000002au,0x00000010u,0x00000029u,0x00000029u,
    0x00050036u,0x00000002u,0x00000004u,0x00000000u,0x00000003u,0x000200f8u,0x00000005u,0x00050041u,
    0x0000000du,0x0000000eu,0x0000000bu,0x0000000cu,0x0004003du,0x00000006u,0x0000000fu,0x0000000eu,
    0x00040053u,0x00000006u,0x0000001eu,0x0000000fu,0x00050041u,0x0000001fu,0x00000020u,0x0000001cu,
    0x0000001eu,0x0004003du,0x00000019u,0x00000021u,0x00000020u,0x0007005fu,0x00000024u,0x00000025u,
    0x00000021u,0x00000023u,0x00000002u,0x00000016u,0x0006000cu,0x00000006u,0x00000026u,0x00000001u,
    0x00000037u,0x00000025u,0x00060041u,0x00000027u,0x00000028u,0x00000014u,0x00000016u,0x0000000fu,
    0x0003003eu,0x00000028u,0x00000026u,0x000100fdu,0x00010038u,
    };
  return device.pipeline(device.shader(code,sizeof(code)));
  }

void textureMipResidency(Tempest::Device& device, const Fixture& fixture) {
  auto color=mipDds(0x31545844,256,256,9);
  const auto maps=mipDds(0x35545844,256,256,9);
  auto single=mipDds(0x31545844,256,256,1);
  const auto paint=[](std::vector<uint8_t>& bytes, uint16_t full, uint16_t tail) {
    for(size_t at=128;at<bytes.size();at+=8) {
      const auto rgb=at<128+mipBytes(256,256,8) ? full : tail;
      std::fill_n(bytes.begin()+std::ptrdiff_t(at),8,uint8_t(0));
      bytes[at]=bytes[at+2]=uint8_t(rgb);
      bytes[at+1]=bytes[at+3]=uint8_t(rgb>>8);
      }
    };
  paint(color,0xf800,0x07e0); // Red Full, green Tail.
  paint(single,0x001f,0x001f); // Unchanged blue neighbour.
  const auto write=[&](const char* name, const std::vector<uint8_t>& bytes) {
    textFile(fixture.root/"materials"/name,
             {reinterpret_cast<const char*>(bytes.data()),bytes.size()});
    };
  write("mip-color.dds",color);
  write("mip-maps.dds",maps);
  write("mip-single.dds",single);
  textFile(fixture.root/"materials/materials.json",R"({
    "MIP_STREAM.TGA":{"albedo":"mip-color.dds","normal":"mip-maps.dds","orm":"mip-maps.dds"},
    "MIP_SINGLE.TGA":{"albedo":"mip-single.dds"}
  })");
  setenv("OPENGOTHIC_TEXTURE_STREAMING","1",1);
  setenv("OPENGOTHIC_TEXTURE_MIPS","1",1);
  setenv("OPENGOTHIC_TEXTURE_FAR_SKIP_MIPS","2",1);
  setenv("OPENGOTHIC_TEXTURE_BUDGET_MB","256",1);
  setenv("OPENGOTHIC_TEXTURE_TARGET_MB","192",1);
  // The two-state ring rule, as before the levels: OPENGOTHIC_TEXTURE_SCREEN_MIPS=0 must restore it exactly.
  setenv("OPENGOTHIC_TEXTURE_SCREEN_MIPS","0",1);
  {
  Resources resources(device);
  Shaders shaders;
  shaders.waitCompiler();
  Material material, noTail;
  {
  Resources::TextureLoadScope far(true);
  material=Material(source("MIP_STREAM.TGA"),true);
  noTail=Material(source("MIP_SINGLE.TGA"),true);
  }
  const Material copy=material;
  const auto tail=[&] {
    return material.tex->w()==64 && material.normal->w()==64 && material.orm->w()==64;
    };
  const auto full=[&] {
    return material.tex->w()==256 && material.normal->w()==256 && material.orm->w()==256;
    };
  require(material.tex!=nullptr && material.normal!=nullptr && material.orm!=nullptr && tail(),
          "Far initial loads must upload the DDS tail for every material map");
  require(noTail.tex!=nullptr && noTail.tex->w()==256,
          "a single-level DDS must remain Full in a Far loading scope");
  require(Resources::textureLookAhead()==0,"the ring rule must not look ahead");
  Resources::textureDemand(material,Resources::TextureCategory::Static,true,4);
  Resources::textureDemand(noTail,Resources::TextureCategory::Static,true,4);

  std::vector<Resources::Vertex> vertices={
    {{0,0,0},{0,0,1},{0,0},0xffffffffu},
    {{1,0,0},{0,0,1},{1,0},0xffffffffu},
    {{0,1,0},{0,0,1},{0,1},0xffffffffu}};
  StaticMesh mesh(material,std::move(vertices),{0,1,2});
  DrawBuckets buckets;
  auto bucket=buckets.alloc(material,mesh);
  auto unchangedBucket=buckets.alloc(noTail,mesh);
  const Tempest::Detail::ResourcePtr<Tempest::Texture2d> unchangedPayload(*noTail.tex);
  const auto* bucketIdentity=&*bucket;
  const auto bucketId=bucket.toInt();
  const auto commit=[&](uint8_t frame=0) {
    auto command=device.commandBuffer();
    bool changed=false;
    {
    auto encoder=command.startEncoding(device);
    changed=buckets.commit(encoder,frame);
    }
    device.submit(command).wait();
    return changed;
    };
  require(commit() && !commit(),"mip fixture must start with stable bucket descriptors");
  require(Shaders::options().doBindless && !buckets.textures().isEmpty() &&
          !buckets.normals().isEmpty() && !buckets.orm().isEmpty(),
          "mip fixture must exercise real bindless arrays for its albedo, normal and ORM");
  for(uint8_t frame=1;frame<Resources::MaxFramesInFlight;++frame)
    require(commit(frame),"each frame bank must initialize its texture descriptors");
  const auto probe=descriptorColorProbe(device);
  const auto encodeProbe=[&](Tempest::CommandBuffer& command, const Tempest::StorageBuffer& output) {
    auto encoder=command.startEncoding(device);
    encoder.setPipeline(probe);
    encoder.setBinding(0,buckets.textures());
    encoder.setBinding(1,output);
    encoder.dispatch(1);
    };
  const auto probeColors=[&](const Tempest::StorageBuffer& output) {
    auto command=device.commandBuffer();
    encodeProbe(command,output);
    device.submit(command).wait();
    std::array<uint32_t,2> colors{};
    device.readBytes(output,colors.data(),sizeof(colors));
    return colors;
    };
  // A fresh output gives every probe a new Vulkan descriptor set. The stable one reuses the set
  // VPoolCache cached for each frame bank before any update, as the game's frames do.
  auto stableOutput=device.ssbo(Tempest::Uninitialized,2*sizeof(uint32_t));
  const auto checkColors=[&](uint32_t first) {
    auto output=device.ssbo(Tempest::Uninitialized,2*sizeof(uint32_t));
    const std::array<uint32_t,2> expected{first,0xffff0000u};
    require(probeColors(output)==expected && unchangedBucket->mat==noTail &&
            unchangedPayload==Tempest::Detail::ResourcePtr<Tempest::Texture2d>(*noTail.tex),
            "a partial descriptor update must sample the changed GPU image and preserve its neighbour");
    require(probeColors(stableOutput)==expected,
            "a partial descriptor update must reach descriptor sets cached before it");
    };
  for(uint8_t frame=1;frame<Resources::MaxFramesInFlight;++frame) {
    require(!commit(frame),"an unchanged frame bank must not rewrite descriptors");
    checkColors(0xff00ff00u);
    }
  require(!commit(0),"returning to an unchanged frame bank must not rewrite descriptors");
  checkColors(0xff00ff00u);
  auto oldOutput=device.ssbo(Tempest::Uninitialized,2*sizeof(uint32_t));
  auto oldCommand=device.commandBuffer();
  encodeProbe(oldCommand,oldOutput);
  uint8_t descriptorFrame=1;
  ChunkTable table;
  table.entries.push_back({{0,0},"",{0,0,0},{1,1,0},1});
  ChunkResidency residency(table);
  const auto tick=[&] {
    const auto before=Resources::textureStats();
    const auto generation=Resources::textureGeneration();
    residency.update({0,0,0},{0,0,0},1000);
    Resources::updateTextures(&residency);
    const auto after=Resources::textureStats();
    require(after.promoted-before.promoted+after.demoted-before.demoted<=1 &&
            Resources::textureGeneration()-generation<=1,
            "a frame must publish at most one mip replacement and one texture generation");
    require(material.tex->w()>=64 && material.normal->w()>=64 && material.orm->w()>=64,
            "a pending mip replacement must preserve its resident payload without a placeholder");
    require(material.tex->uploadReady() && material.normal->uploadReady() && material.orm->uploadReady(),
            "published material maps must have completed their GPU uploads before descriptor binding");
    };
  const auto waitFor=[&](bool wantFull) {
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(!(wantFull ? full() : tail()) && std::chrono::steady_clock::now()<deadline) {
      tick();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    require(wantFull ? full() : tail(),"requested mip replacements must finish through the frame queue");
    require(material==copy && bucket->mat==copy && mesh.sub[0].material==copy &&
            &*bucket==bucketIdentity && bucket.toInt()==bucketId && buckets.buckets().size()==2,
            "mip replacement must preserve material pointers and the original mesh and bucket");
    require(commit(descriptorFrame) && !commit(descriptorFrame),
            "mip replacement must refresh descriptors once without rebuilding buckets");
    checkColors(wantFull ? 0xff0000ffu : 0xff00ff00u);
    };

  Resources::textureDemand(material,Resources::TextureCategory::Static,true,2);
  Resources::updateTextures(&residency);
  require(tail() && !Resources::texturesReady() && Resources::textureStats().promotionQueue>0 &&
          Resources::textureStats().nearTailCount==0,
          "ring-two Full requests must preserve Tail while pending and remain outside the Near metric");
  Resources::textureDemand(material,Resources::TextureCategory::Static,true,1);
  require(Resources::textureStats().nearTailCount==3,
          "the Near metric must count Tail maps only when an owner enters ring one");
  waitFor(true);
  require(Resources::textureStats().promoted==3 && Resources::textureStats().nearTailCount==0,
          "promotion must complete every map and clear Near textures on Tail");
  const auto retiredTail=Resources::textureStats().retiredBytes;
  require(retiredTail>0,"the old frame bank must retain retired Tail images before its fence");
  Resources::resetRecycled(1);
  require(Resources::textureStats().retiredBytes==retiredTail,
          "another frame fence must not release images referenced by the old bank");
  auto oldFence=device.submit(oldCommand);
  oldFence.wait();
  std::array<uint32_t,2> oldColors{};
  device.readBytes(oldOutput,oldColors.data(),sizeof(oldColors));
  require(oldColors==std::array<uint32_t,2>{0xff00ff00u,0xffff0000u},
          "a command encoded with the old bank must still sample Tail after a new bank publishes Full");
  Resources::resetRecycled(0);
  require(Resources::textureStats().retiredBytes==0,"completed initial mip replacements must be recyclable");
  descriptorFrame=0;

  Resources::textureDemand(material,Resources::TextureCategory::Static,true,3);
  Resources::textureDemand(material,Resources::TextureCategory::Static,false,1);
  Resources::textureDemand(material,Resources::TextureCategory::Static,false,2);
  unsetenv("OPENGOTHIC_TEXTURE_TARGET_MB");
  Resources::configureTextures({}, {}, "0");
  std::this_thread::sleep_for(std::chrono::milliseconds(5100));
  tick();
  require(full() && Resources::textureStats().demoted==0,
          "a ring-three owner must retain Full under pressure after the five-second hysteresis interval");
  Resources::textureDemand(material,Resources::TextureCategory::Static,false,3);

  setenv("OPENGOTHIC_TEXTURE_TARGET_MB","192",1);
  Resources::configureTextures({}, {}, "0");
  Material protectedOrm, olderColor, newerNormal;
  protectedOrm.tex=material.orm;
  olderColor.tex=material.tex;
  newerNormal.tex=material.normal;
  Resources::textureDemand(protectedOrm,Resources::TextureCategory::Static,true,1);
  Resources::textureDemand(olderColor,Resources::TextureCategory::Static,true,2);
  Resources::textureDemand(olderColor,Resources::TextureCategory::Static,false,2);
  Resources::textureDemand(newerNormal,Resources::TextureCategory::Static,true,2);
  Resources::textureDemand(newerNormal,Resources::TextureCategory::Static,false,2);
  tick();
  require(full() && Resources::textureStats().demoted==0,
          "leaving ring three must start a fresh five-second delay before demotion");
  std::this_thread::sleep_for(std::chrono::milliseconds(4900));
  tick();
  require(full() && Resources::textureStats().demoted==0,
          "Far owners must retain Full until the entire five-second hysteresis has elapsed");
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  tick();
  require(full() && Resources::textureStats().demoted==0 &&
          Resources::textureStats().liveBytes<192*1048576,
          "the 192 MiB target environment override must retain eligible Far Full payloads below target");

  unsetenv("OPENGOTHIC_TEXTURE_TARGET_MB");
  Resources::configureTextures({}, {}, "0");
  bool oldestObserved=false;
  const auto pressureDeadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
  while(Resources::textureStats().demoted<2 && std::chrono::steady_clock::now()<pressureDeadline) {
    tick();
    if(Resources::textureStats().demoted==1 && !oldestObserved) {
      require(material.tex->w()==64 && material.normal->w()==256 && material.orm->w()==256,
              "pressure must first demote the eligible texture least recently needed at Full");
      oldestObserved=true;
      }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  require(oldestObserved && Resources::textureStats().demoted==2 && material.tex->w()==64 &&
          material.normal->w()==64 && material.orm->w()==256 && Resources::textureStats().nearTailCount==0,
          "pressure must demote eligible Far textures in LRU order while protecting a Near Full owner");
  require(commit(descriptorFrame) && !commit(descriptorFrame),
          "pressure demotion must refresh only the affected frame-bank descriptors");
  checkColors(0xff00ff00u);

  Resources::textureDemand(protectedOrm,Resources::TextureCategory::Static,false,1);
  std::this_thread::sleep_for(std::chrono::milliseconds(4900));
  tick();
  require(material.orm->w()==256 && Resources::textureStats().demoted==2,
          "a texture leaving Near must receive its own full five-second delay under pressure");
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  waitFor(false);
  const auto fullBytes=uint64_t(color.size()-128+2*(maps.size()-128));
  require(Resources::textureStats().demoted==3 && Resources::textureStats().retiredBytes==fullBytes,
          "demotion must retire each Full payload while retaining its replacement Tail");
  require(noTail.tex->w()==256,"a texture without a DDS tail must never be demoted");
  Resources::resetRecycled(1);
  require(Resources::textureStats().retiredBytes==fullBytes,
          "a different frame slot must retain the Full payloads retired by demotion");
  device.waitIdle();
  Resources::resetRecycled(0);
  require(Resources::textureStats().retiredBytes==0,
          "the completed frame slot must release demoted Full payloads after its fence");

  SceneGlobals scene;
  VisualObjects visuals(scene,{{0,0,0},{1,1,0}});
  visuals.setTextureResidency(&residency);
  auto body=visuals.get(mesh,material,0,mesh.sub[0].iboLength,false);
  const Tempest::Vec3 farOwner{float(4*ChunkId::Size+1),0,0};
  body.setTextureOwner(farOwner,true);
  body.setTextureOwner({farOwner.x+100,0,0},true);
  auto bone=Tempest::Matrix4x4::mkIdentity();
  bone.translate(1,0,0);
  body.setObjMatrix(bone);
  body.setTexturePosition({0,0,0});
  require(tail() && Resources::textureStats().promotionQueue==0,
          "a Far spatial body must retain Tail across same-cell movement and animated node transforms");
  body.setTextureOwner({float(2*ChunkId::Size+1),0,0},true);
  require(Resources::textureStats().promotionQueue==3 && Resources::textureStats().nearTailCount==0,
          "a spatial body crossing into ring two must request Full outside the Near metric");
  body.setTextureOwner(farOwner,true);
  require(Resources::textureStats().promotionQueue==0,
          "a spatial body leaving ring two must cancel unpublished Full requests");
  body.setTextureOwner(farOwner,false);
  require(Resources::textureStats().promotionQueue==3,
          "the hero's always-Full ownership must request Full even in the same Far cell");
  descriptorFrame=1;
  waitFor(true);
  body.setTextureOwner({float(8*ChunkId::Size+1),0,0},false);
  require(full() && Resources::texturesReady(),
          "the hero's always-Full ownership must survive movement between distant cells");
  Resources::textureDemand(material,Resources::TextureCategory::Static,false,4);
  Resources::textureDemand(noTail,Resources::TextureCategory::Static,false,4);
  }
  setenv("OPENGOTHIC_TEXTURE_MIPS","0",1);
  {
  Resources resources(device);
  Resources::TextureLoadScope far(true);
  const Material material(source("MIP_STREAM.TGA"),true);
  require(material.tex!=nullptr && material.normal!=nullptr && material.orm!=nullptr &&
          material.tex->w()==256 && material.normal->w()==256 && material.orm->w()==256 &&
          Resources::textureStats().tailBytes==0,
          "OPENGOTHIC_TEXTURE_MIPS=0 must load only Full payloads even in a Far loading scope");
  }
  unsetenv("OPENGOTHIC_TEXTURE_MIPS");
  unsetenv("OPENGOTHIC_TEXTURE_FAR_SKIP_MIPS");
  unsetenv("OPENGOTHIC_TEXTURE_BUDGET_MB");
  unsetenv("OPENGOTHIC_TEXTURE_TARGET_MB");
  unsetenv("OPENGOTHIC_TEXTURE_SCREEN_MIPS");
  }

// Level and demand maths, the UV density of a hand-made mesh and the owner's evaluation (texturelevels.h,
// texeldensity.h, texturescreendemand.h).
void textureLevelMaths() {
  using namespace TextureLevels;
  require(lowest(512,512,10)==3 && lowest(256,128,9)==1 && lowest(2048,1024,12)==4 &&
          lowest(127,512,8)==0 && lowest(512,512,1)==0 && lowest(512,512,2)==1,
          "the lowest top level must keep both sides at least 64 and one level, as the DDS tail reader does");
  // log2 K off the integer boundaries: floor(log2(side) - 10.125 - exponent) - reserve
  const float scale=10.125f;
  require(level(1024,1024,scale,-5,1,lowest(1024,1024,11))==3 && level(512,512,scale,-5,1,lowest(512,512,10))==2 &&
          level(256,256,scale,-5,1,lowest(256,256,9))==1 && level(128,128,scale,-5,1,lowest(128,128,8))==0 &&
          level(1024,512,scale,-4,1,lowest(1024,512,11))==2 && level(512,512,scale,-4,1,3)==1,
          "one demand must give each texture size its own level from the geometric mean of its sides");
  require(level(1024,1024,scale,-5,0,4)==4 && level(1024,1024,scale,-5,2,4)==2 && level(256,256,scale,-5,0,2)==2,
          "the reserve must make the level that many levels sharper");
  require(level(512,512,scale,-20,1,3)==3 && level(512,512,scale,20,1,3)==0,
          "a level must stay within the whole chain and the lowest allowed top");
  require(level(512,512,scale,NoExponent,1,3)==0 && level(512,512,scale,TextureScreenDemand::WholeChain,1,3)==0,
          "no resolution and the reserved whole-chain demand must keep the whole chain");
  const auto twenty=demand(20,1.f), forty=demand(40,1.f);
  require(twenty.exponent==-4 && std::abs(twenty.margin-4)<1e-9 && forty.exponent==-5 && std::abs(forty.margin-8)<1e-9,
          "a demand must be quantised to a power of two with the distance it holds over");
  require(demand(17,1.f).exponent==-4 && demand(31,1.f).exponent==-4 && demand(33,1.f).exponent==-5 &&
          demand(20,2.f).exponent==-5,
          "a demand must round towards the sharper side and change only as the distance doubles");
  const auto close=demand(0.1,1.f);
  require(close.exponent==2 && std::abs(close.margin-0.525)<1e-9,
          "an owner closer than the minimum distance must count as at it and hold until it leaves the band");
  const auto held=demand(33,1.f,-4);
  require(held.exponent==-4 && std::abs(held.margin-7)<1e-9 && demand(41,1.f,-4).exponent==-5 &&
          demand(17,1.f,-5).exponent==-4,
          "moving away an owner must keep its sharper exponent a quarter band longer, moving closer switch at once");
  // At the coarsest exponent that matters every texture is at its lowest level, and one sharper some is not.
  require(coarsest(1,13.f)==-8 && coarsest(1,9.7f)==-5 && coarsest(0,9.7f)==-4 &&
          level(512,512,13.f,-8,1,lowest(512,512,10))==lowest(512,512,10) &&
          level(4096,4096,13.f,-8,1,lowest(4096,4096,13))==lowest(4096,4096,13) &&
          level(128,128,13.f,-8,1,lowest(128,128,8))==lowest(128,128,8) &&
          level(512,512,9.7f,-5,1,lowest(512,512,10))==lowest(512,512,10) &&
          level(512,512,9.7f,-4,1,lowest(512,512,10))<lowest(512,512,10),
          "the coarsest exponent that matters must give every texture size its lowest level");
  const auto far=demand(300,1.f,NoExponent,-8), farther=demand(1000,1.f,NoExponent,-8);
  require(far.exponent==-8 && std::abs(far.margin-44)<1e-9 && farther.exponent==-8 && std::abs(farther.margin-744)<1e-9,
          "an owner past the coarsest exponent must hold it until it comes closer than its band");
  require(demand(5,0.f).exponent==NoExponent && demand(5,-1.f).exponent==NoExponent,
          "an owner without a density must have no resolution demand");
  require(std::abs(viewScale(2048,90)-10.f)<1e-4f,"the view scale must be log2 of height/(2 tan(fov/2))");
  require(chainBytes(Tempest::TextureFormat::DXT1,512,512,10,2)==10936 &&
          chainBytes(Tempest::TextureFormat::DXT5,256,256,9,0)==87408,
          "the chain bytes must count whole BC blocks from the first level down");
  require(loadsBefore(2,4,1,0) && loadsBefore(1,0,1,3) && !loadsBefore(1,3,1,0) && !loadsBefore(1,0,2,4),
          "loads must go by the largest shortfall in levels, then the nearest ring");

  // Two counted triangles: 2 m2 over half a UV unit squared (0.5 UV units per metre) and 0.5 m2 over 2 (2 per
  // metre); a point triangle and one with no UV area are left out, also from the bounds.
  std::vector<Resources::Vertex> vertices={
    {{0,0,0},{0,0,1},{0,0},0},{{200,0,0},{0,0,1},{1,0},0},{{0,200,0},{0,0,1},{0,1},0},
    {{0,0,100},{0,0,1},{0,0},0},{{100,0,100},{0,0,1},{2,0},0},{{0,100,100},{0,0,1},{0,2},0},
    {{500,500,500},{0,0,1},{0,0},0},
    {{-300,0,0},{0,0,1},{.5f,.5f},0},{{-300,300,0},{0,0,1},{.5f,.5f},0},{{-300,0,300},{0,0,1},{.5f,.5f},0}};
  const std::vector<uint32_t> indices={0,1,2, 3,4,5, 6,6,6, 7,8,9, 0,1,99};
  const auto texel=TexelDensity::measure(vertices.data(),vertices.size(),indices.data(),0,indices.size());
  require(texel.valid() && std::abs(texel.uvPerMetre-0.5f)<1e-6f,
          "the lowest UV density over the counted triangles must decide");
  require(texel.min.x==0 && texel.min.y==0 && texel.min.z==0 && texel.max.x==200 && texel.max.y==200 && texel.max.z==100,
          "the bounds must hold only the counted triangles");
  const auto second=TexelDensity::measure(vertices.data(),vertices.size(),indices.data(),3,3);
  require(std::abs(second.uvPerMetre-2.f)<1e-6f,"a range of indices must measure only its triangles");
  require(!TexelDensity::measure(vertices.data(),vertices.size(),indices.data(),6,6).valid(),
          "an owner with only degenerate triangles must have no density");
  const auto pointOnly=TexelDensity::measure(vertices.data(),vertices.size(),indices.data(),6,3);
  const auto collapsed=TexelDensity::measure(vertices.data(),vertices.size(),indices.data(),9,3);
  require(!pointOnly.valid() && pointOnly.flat && pointOnly.placed(Tempest::Matrix4x4::mkIdentity()).flat &&
          !collapsed.valid() && !collapsed.flat && !TexelDensity::measure(vertices.data(),vertices.size(),indices.data(),6,6).flat,
          "only an owner whose every triangle has no world area must count as having nothing to draw");
  require(level(512,512,scale,TextureScreenDemand::Coarsest,1,3)==3 && level(256,256,scale,TextureScreenDemand::Coarsest,0,2)==2,
          "the reserved coarsest exponent must give the lowest allowed level");
  auto transform=Tempest::Matrix4x4::mkIdentity();
  transform.translate(1000,0,0);
  transform.scale(2,2,2);
  const auto placed=texel.placed(transform);
  require(std::abs(placed.uvPerMetre-0.25f)<1e-6f && std::abs(placed.min.x-1000)<1e-3f && std::abs(placed.max.x-1400)<1e-3f &&
          std::abs(placed.max.y-400)<1e-3f && std::abs(placed.max.z-200)<1e-3f,
          "a placed owner must move its bounds and divide its density by its scale");

  TextureScreenDemand::View view;
  require(TextureScreenDemand::evaluate(view,TexelDensity()).exponent==TextureScreenDemand::WholeChain &&
          TextureScreenDemand::evaluate(view,collapsed).exponent==TextureScreenDemand::WholeChain,
          "an owner measured without a density must demand the whole chain");
  require(TextureScreenDemand::evaluate(view,pointOnly).exponent==TextureScreenDemand::Coarsest &&
          !TextureScreenDemand::evaluate(view,pointOnly).scheduled,
          "an owner with nothing to draw must ask for the coarsest allowed level and not follow the distance");
  const auto unknown=TextureScreenDemand::evaluate(view,texel);
  require(unknown.exponent==NoExponent && unknown.scheduled && unknown.expiry<0,
          "before the first view an owner must keep the ring rule and be due at once");
  view.known=true;
  view.odometer=7;
  view.camera={-3900,100,50};
  view.hero={0,0,1000000};
  const auto known=TextureScreenDemand::evaluate(view,texel);
  // 39 m at 0.5 UV units per metre: 1/19.5 -> 2^-4, which holds from 32 to 64 m
  require(known.exponent==-4 && known.scheduled && std::abs(known.expiry-(7+7))<1e-6,
          "an owner must be evaluated from the nearer of the camera and the hero, due after its margin");
  }

// Step 4 (textureflow.h, texturelookahead.h): the loads' slots and bytes in flight, the publications' budget, the
// distributions; the look-ahead's horizon from the latency, the distance to the view's path, the velocity and a jump.
void textureFlowMaths() {
  using namespace TextureFlow;
  constexpr uint64_t MiB=1048576;
  Limits limits;
  limits.loads=4;
  limits.bytes=64*MiB;
  require(admits(0,0,100*MiB,limits),"one load must always start, however large");
  require(admits(3,30*MiB,20*MiB,limits) && !admits(3,30*MiB,40*MiB,limits) && !admits(4,0,1,limits),
          "more loads must start only within the slots and within the bytes in flight");
  require(publishes(0,5.0,limits) && publishes(1,0.05,limits) && !publishes(1,0.1,limits),
          "a frame must publish one ready load at least, more only within its budget");
  // the loop Resources publishes ready loads with: one with no budget left, all with room for them
  size_t calls=0;
  Limits none=limits;
  none.publishMs=0;
  Limits room=limits;
  room.publishMs=1e9;
  require(publishWithin(3,none,[&](size_t) { ++calls; })==1 && calls==1 &&
          publishWithin(3,room,[&](size_t) { ++calls; })==3 && calls==4,
          "a frame's publications must stop at the budget, after the first");
  // 0.1 ms a frame, 2 ms a publication: the first spends the budget
  require(publishWithin(3,limits,[&](size_t) { std::this_thread::sleep_for(std::chrono::milliseconds(2)); })==1,
          "a publication that outlasts the budget must be the frame's last");
  require(state(false,true,Phase::Ready)==Waiting && state(true,true,Phase::Reading)==Stale &&
          state(true,false,Phase::Queued)==Reading && state(true,false,Phase::Done)==Uploading &&
          state(true,false,Phase::Ready)==Publishing,"a short texture's state must follow its load");

  Histogram times;
  for(int i=1;i<=10;++i)
    times.add(10.0*i);
  const Histogram before=times;
  times.add(1000);
  // the 5th of 10 is 50 ms, in the bucket up to 2^(46/8) = 53.8 ms
  // the 9th of 10 is 90 ms, in the bucket up to 2^(52/8) = 90.5 ms
  require(times.quantile(0.5,before)==1000 && times.max(before)==1000 && times.count(before)==1 &&
          before.quantile(0.5)>=50 && before.quantile(0.5)<=54 && before.quantile(0.9)>=90 && before.quantile(0.9)<=98.2 &&
          before.max()==100 && Histogram().quantile(0.5)<0,
          "a distribution must give quantiles within an eighth of an octave and the exact largest time since a copy");
  Bursts bursts;
  const auto t0=Clock::time_point()+std::chrono::seconds(100);
  for(const int ms:{0,100,500})
    bursts.add(t0+std::chrono::milliseconds(ms));
  bursts.add(t0+std::chrono::milliseconds(1200));
  bursts.tick(t0+std::chrono::milliseconds(2500));
  require(bursts.count()==2 && bursts.max()==3 && bursts.quantile(0.5)==1 && bursts.quantile(1)==3,
          "textures that become short within a second of the first must count as one burst");

  using namespace TextureLookAhead;
  require(horizon(10,WindowSamples-1)==DefaultHorizon && std::abs(horizon(10.5,WindowSamples)-0.021)<1e-12 &&
          horizon(400,WindowSamples)==MaxHorizon,
          "the horizon must be twice the p90 of a full window of latencies, at most the cap");

  // a box 10 m ahead along x, 2 m wide; the view at the origin
  const Tempest::Vec3 lo{1000,-100,-100}, hi{1200,100,100}, origin{0,0,0};
  const auto near=[](double a, double b) { return std::abs(a-b)<1e-6; };
  require(near(distance(origin,{600,0,0},lo,hi),4) && near(distance(origin,{2000,0,0},lo,hi),0),
          "a path ahead must come closer, up to inside the bounds");
  require(near(distance(origin,{-600,0,0},lo,hi),10) && near(distance(origin,{0,600,0},lo,hi),10) &&
          near(distance(origin,{},lo,hi),10),
          "a path behind, sideways or standing still must not bring the bounds closer");
  // diagonally: (1000t, 1000t) is nearest at t = 0.55, 4.5 m short in x and over the top in y
  require(near(distance(origin,{1000,1000,0},lo,hi),std::sqrt(2.0)*4.5),"the closest point may lie inside the path");
  require(near(distance(origin,{600,0,0},Tempest::Vec3{1000,0,0},100),3) && near(distance(origin,{-600,0,0},Tempest::Vec3{1000,0,0},100),9),
          "a sphere must come closer along a path ahead and not behind");

  Velocity velocity;
  for(int frame=0;frame<=30;++frame)
    velocity.add({5.f*float(frame),0,0},0.01*frame);
  const auto running=velocity.velocity();
  require(std::abs(running.x-500)<1e-2 && std::abs(running.y)<1e-6,"the velocity must be the move over its window");
  Velocity standing;
  for(int frame=0;frame<=30;++frame)
    standing.add({700,0,0},0.01*frame);
  require(standing.velocity().x==0,"standing still must have no velocity");
  // Still until 0.24 s, then 5 cm in the last frame: 20 cm/s over the window, not 500 over the last frame.
  Velocity starting;
  for(int frame=0;frame<=24;++frame)
    starting.add({0,0,0},0.01*frame);
  starting.add({5,0,0},0.25);
  require(std::abs(starting.velocity().x-20)<0.01f,"the velocity must be smoothed over its window");

  // The residency: the paths from the hero's and the camera's velocities times the horizon; a jump is no velocity.
  ChunkTable table;
  table.entries.push_back({{0,0},"",{0,0,0},{1,1,0},1});
  ChunkResidency residency(table);
  residency.setLookAhead(0.1);
  const auto at=[](int frame) { return ChunkResidency::Clock::time_point()+std::chrono::milliseconds(10*frame); };
  Tempest::Vec3 hero{3200,0,3200};
  int frame=0;
  for(;frame<=30;++frame) {
    hero.x=3200+5.f*float(frame);
    residency.update(hero,hero,1000,at(frame));
    }
  const auto odometer=residency.viewOdometer();
  require(std::abs(residency.viewHeroAhead().x-50)<0.1f && std::abs(residency.viewCameraAhead().x-50)<0.1f,
          "running at 5 m/s with a horizon of 0.1 s must look 0.5 m ahead");
  // just past ChunkResidency::JumpDistance (50 m)
  hero.x+=5100;
  residency.update(hero,hero,1000,at(frame++));
  require(residency.viewJumped() && residency.viewHeroAhead().x==0 && residency.viewCameraAhead().x==0 &&
          residency.viewOdometer()>=odometer+51,"a jump must not count as velocity");
  hero.x+=5;
  residency.update(hero,hero,1000,at(frame++));
  require(std::abs(residency.viewHeroAhead().x-50)<0.1f,"after a jump the velocity must come from the moves after it");
  // just short of it: a move, and velocity
  hero.x+=4900;
  residency.update(hero,hero,1000,at(frame++));
  const float sprint=residency.viewHeroAhead().x;
  require(!residency.viewJumped() && sprint>100,"a move of 49 m must count as velocity");
  residency.setLookAhead(0);
  const auto before0=residency.viewOdometer();
  hero.x+=5;
  residency.update(hero,hero,1000,at(frame++));
  require(residency.viewHeroAhead().x==0 && std::abs(residency.viewOdometer()-before0-0.05-0.01*sprint)<1e-4,
          "without a horizon there must be no path; its end coming back counts on the odometer");
  hero.x+=5;
  const auto before1=residency.viewOdometer();
  residency.update(hero,hero,1000,at(frame++));
  require(std::abs(residency.viewOdometer()-before1-0.05)<1e-6,"without a horizon the odometer must count the moves only");

  // An owner 10 m ahead at one UV unit per metre: 2^-3 from where the view is (holds from 8 m), 2^-2 from 5 m along its
  // path (holds from 4 m: 1 m of margin).
  TexelDensity texel;
  texel.uvPerMetre=1;
  texel.min=lo;
  texel.max=hi;
  TextureScreenDemand::View view;
  view.known=true;
  const auto still=TextureScreenDemand::evaluate(view,texel);
  require(still.exponent==-3 && still.present==-3,"standing still the exponent and the present one must be the same");
  view.heroAhead=view.cameraAhead={500,0,0};
  const auto ahead=TextureScreenDemand::evaluate(view,texel);
  require(ahead.exponent==-2 && ahead.present==-3 && std::abs(ahead.margin-1)<1e-9 && std::abs(still.margin-2)<1e-9,
          "an owner ahead must want the level of the closest point of the path, keep its present exponent and be due "
          "when either may change");
  view.heroAhead=view.cameraAhead={-500,0,0};
  require(TextureScreenDemand::evaluate(view,texel).exponent==-3,"an owner behind the view must not be pulled in");
  view.heroAhead=view.cameraAhead={0,500,0};
  require(TextureScreenDemand::evaluate(view,texel).exponent==-3,"an owner beside the path must not be pulled in");
  view.heroAhead={};
  view.cameraAhead={500,0,0};
  require(TextureScreenDemand::evaluate(view,texel).exponent==-2,"the camera's path must count as the hero's does");
  view.camera={0,0,1000000};
  view.cameraAhead={};
  view.heroAhead={500,0,0};
  const auto heroOnly=TextureScreenDemand::evaluate(view,texel);
  require(heroOnly.exponent==-2 && heroOnly.present==-3,"the hero's path must count with the camera far away");
  // An owner placed by a position, a 1 m sphere 10 m ahead: 9 m now (2^-3), 5 m from the end of a 4 m path (2^-2).
  view.camera={};
  view.heroAhead=view.cameraAhead={400,0,0};
  const auto sphere=TextureScreenDemand::evaluate(view,texel,TextureScreenDemand::Sphere{{1000,0,0},100});
  require(sphere.exponent==-2 && sphere.present==-3 && sphere.margin>0,
          "a sphere owner ahead must want its sharper level before the present distance reaches its band");
  }

// Step 3 (texturebudget.h): the target from the device's budget, the stage from the sums D(s), the way back.
void textureBudgetMaths() {
  using namespace TextureBudget;
  Device discrete;
  discrete.known=true;
  discrete.measured=true;
  discrete.budget=10240*MiB;
  discrete.usage=1024*MiB;
  // other 824, headroom max(512, 1280), room 10240-824-1280=8136, a quarter 2560
  require(target(discrete,200*MiB)==2560*MiB,"a device with its own memory must give the textures a quarter of its budget");
  auto shared=discrete;
  shared.shared=true;
  require(target(shared,200*MiB)==1280*MiB,"memory shared with the CPU must give the textures an eighth");
  auto small=discrete;
  small.budget=1024*MiB;
  small.usage=300*MiB;
  // a quarter 256; room 1024-200-512=312
  require(target(small,100*MiB)==256*MiB,"a small device must still give a quarter while the headroom allows");
  small.budget=512*MiB;
  require(target(small,100*MiB)==192*MiB,"the target must never go below the floor of 192 MiB");
  auto filled=discrete;
  filled.usage=9216*MiB;
  // other 8704, room 10240-8704-1280=256
  require(target(filled,512*MiB)==256*MiB,"the rest of the game filling the device must limit the target by the headroom");
  filled.usage=12000*MiB;
  require(target(filled,512*MiB)==192*MiB,"a device with no room left must keep the floor");
  require(target(discrete,4096*MiB)==2560*MiB,"live textures above the usage must leave nothing else on the device");
  require(target(Device(),0)==192*MiB,"a device whose budget is not readable must keep today's 192 MiB");
  // F9: a small device filled by the rest of the game: the headroom is the 512 MiB minimum, not budget/8 = 256
  auto smallFilled=discrete;
  smallFilled.budget=2048*MiB;
  smallFilled.usage=1700*MiB;
  // other 1188, room 2048-1188-512=348, a quarter 512
  require(target(smallFilled,512*MiB)==348*MiB,"the headroom must be at least 512 MiB");
  // Lowered textures leave their bytes in the allocator's pages: usage stays, live drops. Held free bytes are room for
  // textures, not the rest of the game; otherwise each lowered byte would lower the target by as much.
  auto held=discrete;
  held.budget=1800*MiB;
  held.usage=1332*MiB;
  held.unused=300*MiB;
  // other 1332-300-197=835, room 1800-835-512=453, a quarter 450
  require(target(held,197*MiB)==450*MiB,"free bytes held in the allocator's pages must not count as the rest of the game");
  held.unused=0;
  require(target(held,197*MiB)==192*MiB,"without them the same usage leaves no room above the floor");
  held.unused=2000*MiB;
  require(target(held,197*MiB)==450*MiB,"held bytes above the usage must leave nothing else");
  require(follow(1000*MiB,1040*MiB)==1000*MiB && follow(1000*MiB,960*MiB)==1000*MiB &&
          follow(1000*MiB,1060*MiB)==1060*MiB && follow(1000*MiB,940*MiB)==940*MiB &&
          follow(1000*MiB,1049*MiB)==1000*MiB && follow(1000*MiB,1050*MiB)==1000*MiB &&
          follow(1000*MiB,1051*MiB)==1051*MiB && follow(1000*MiB,951*MiB)==1000*MiB && follow(1000*MiB,949*MiB)==949*MiB,
          "the working target must change only when the rule moves by more than 5 %");
  require(logWorthy(1000,1300) && logWorthy(1000,700) && !logWorthy(1000,1200),"the log line must follow changes over 25 %");

  require(choose(nullptr,"","f").automatic && choose(nullptr,"auto","f").automatic && choose(nullptr,{},"f").source==Source::Rule,
          "an absent, empty or auto setting must mean the rule");
  const auto setting=choose(nullptr,"300","f"), environment=choose("256","300","f"), zero=choose("0","300","f");
  require(!setting.automatic && setting.bytes==300*MiB && setting.source==Source::Setting &&
          !environment.automatic && environment.bytes==256*MiB && environment.source==Source::Environment &&
          !zero.automatic && zero.bytes==0 && choose("auto","300","f").automatic,
          "a number must fix the target, the environment over the setting, 0 included");
  bool threw=false;
  try {
    choose(nullptr,"big","f");
    }
  catch(const std::runtime_error&) {
    threw=true;
    }
  require(threw,"anything but a number or auto must be refused");

  // The distance class of an owner measured on screen: borders at 64 and 128 m, kept 1/8 farther moving away.
  double margin=0;
  const auto classOf=[&](double d, uint32_t previous) { return TextureScreenDemand::distanceClass(d,previous,margin); };
  const auto none=TextureScreenDemand::NoClass;
  require(classOf(30,none)==0 && std::abs(margin-42)<1e-9 && classOf(70,none)==1 && std::abs(margin-6)<1e-9 &&
          classOf(130,none)==2 && std::abs(margin-2)<1e-9,
          "the class must come from one and two chunk sizes, due again at its border");
  require(classOf(70,0)==0 && std::abs(margin-2)<1e-9 && classOf(140,1)==1 && std::abs(margin-4)<1e-9 &&
          classOf(140,0)==2 && classOf(60,2)==0 && classOf(100,4)==1,
          "moving away an owner must keep its nearer class 1/8 of the border longer, moving closer switch at once");
  // 70 m, 1/30 UV units per metre: exponent -1 holds 10 m nearer (60 m), class 1 holds 6 m nearer (64 m). The class
  // adds its margin only from stage 2 on; before that the owner is due by its exponent alone, as without classes.
  TexelDensity point;
  point.uvPerMetre=1.f/30.f;
  TextureScreenDemand::View viewOff;
  viewOff.known=true;
  viewOff.camera={-7000,0,0};
  viewOff.hero={0,0,1000000};
  auto viewOn=viewOff;
  viewOn.classMargin=true;
  const auto off=TextureScreenDemand::evaluate(viewOff,point), on=TextureScreenDemand::evaluate(viewOn,point);
  require(off.exponent==-1 && off.distanceClass==1 && std::abs(off.margin-10)<1e-4 && std::abs(off.expiry-10)<1e-4 &&
          on.exponent==-1 && on.distanceClass==1 && std::abs(on.margin-6)<1e-4,
          "the class must be computed always, and add its margin only while the stages use it");

  // stage:            0  1  2  3  4  5  6  7  8
  const uint32_t ring2[]={0, 0, 1, 1, 1, 2, 2, 2, 3};
  const uint32_t ring1[]={0, 0, 0, 1, 1, 1, 2, 2, 2};
  const uint32_t ring0[]={0, 0, 0, 0, 1, 1, 1, 2, 2};
  for(uint32_t stage=0;stage<9;++stage)
    require(coarser(stage,2)==ring2[stage] && coarser(stage,7)==ring2[stage] && coarser(stage,1)==ring1[stage] &&
            coarser(stage,0)==ring0[stage],"each stage must add one level, in turn for ring 2 and farther, ring 1 and farther, all");
  require(reserve(0,1)==1 && reserve(0,2)==2 && reserve(1,1)==0 && reserve(5,2)==0,"stage 1 and deeper must drop the reserve");
  require(coarsen(1,4,4,0,false)==2 && coarsen(3,4,7,2,false)==4 && coarsen(0,4,13,3,true)==0 && coarsen(4,4,9,0,false)==4,
          "a stage must coarsen up to the lowest allowed level and never an owner that needs the whole chain");

  // D(s) from the difference array against the sum level by level, for textures of every kind of ring and cap.
  struct Texture { uint8_t level0, level1, cap, lowest; uint32_t ring; bool whole; uint32_t size; };
  const std::vector<Texture> textures={{0,1,5,5,0,false,2048},{1,2,5,5,1,false,2048},{0,0,4,4,3,false,1024},
                                       {0,1,2,5,4,false,2048},{2,3,3,3,0,false,512},{0,0,5,5,0,true,2048},
                                       {0,1,0,5,2,false,2048},{1,1,6,6,9,false,4096}};
  const auto bytes=[](uint32_t size, uint8_t level) {
    return TextureLevels::chainBytes(Tempest::TextureFormat::DXT5,size,size,uint32_t(std::log2(double(size)))+1,level);
    };
  Sums sums;
  sums.fixed(12345);
  for(const auto& t:textures)
    sums.measured(t.level0,t.level1,t.cap,t.lowest,t.ring,t.whole,[&](uint8_t level) { return bytes(t.size,level); });
  const auto d=sums.finish();
  // the 4096 one, far: level 1 to 6 at stages 2, 5, 8, 11, 14
  require(d.size()==15,"the deepest stage must be the last that changes a level");
  for(uint32_t stage=0;stage<24;++stage) {
    uint64_t sum=12345;
    for(const auto& t:textures) {
      const uint8_t level=stage==0 ? std::min(t.level0,t.cap) :
                          std::min(coarsen(t.level1,t.lowest,stage,t.ring,t.whole),std::min(t.cap,t.lowest));
      sum+=bytes(t.size,level);
      }
    require(d[std::min<size_t>(stage,d.size()-1)]==sum,"each D(s) must be the exact sum of the levels the stage gives");
    }

  const auto t0=Clock::time_point{}+std::chrono::hours(1);
  const auto at=[&](int seconds) { return t0+std::chrono::seconds(seconds); };
  const std::vector<uint64_t> sumsByStage={1000,800,600,400,200};
  Ladder ladder;
  require(step(ladder,sumsByStage,700,at(0))==2,"the stage must be the shallowest whose sum fits the target");
  require(step(ladder,sumsByStage,300,at(1))==4,"a smaller target must go deeper at once");
  require(step(ladder,sumsByStage,100,at(2))==4,"when nothing fits the stage must be the deepest");
  require(step(ladder,sumsByStage,900,at(3))==4 && step(ladder,sumsByStage,900,at(7))==4,
          "going back must wait five seconds with the shallower sum within 90 % of the target");
  require(step(ladder,sumsByStage,900,at(8))==1,"after five seconds the stage must go back to the shallowest within 90 %");
  // D(0) sits at the target: one second above, one below, but never within 90 % of it.
  Ladder border;
  for(int i=0;i<20;++i)
    step(border,{i%2==0 ? 1010u : 990u,500u},1000,at(i));
  require(border.stage==1,"a sum at the border must not make the stage flip");
  Ladder again;
  again.stage=1;
  step(again,{800,500},1000,at(0));
  step(again,{950,500},1000,at(3));
  step(again,{800,500},1000,at(4));
  require(step(again,{800,500},1000,at(8))==1 && step(again,{800,500},1000,at(9))==0,
          "the five seconds back must be in a row");
  // the way back needs D(s-1) at or below 90 % of the target, not just near it
  Ladder above, at90;
  above.stage=at90.stage=1;
  for(int i=0;i<=6;++i) {
    step(above,{905,500},1000,at(i));
    step(at90,{900,500},1000,at(i));
    }
  require(above.stage==1 && at90.stage==0,"going back must need D(s-1) at most 90 % of the target");
  Ladder forced;
  forced.stage=4;
  require(step(forced,sumsByStage,900,at(0),true)==1,"a world load must take the shallowest stage that fits at once");
  }

void textureScreenLevels(Tempest::Device& device, const Fixture& fixture) {
  const auto write=[&](const char* name, const std::vector<uint8_t>& bytes) {
    textFile(fixture.root/"materials"/name,{reinterpret_cast<const char*>(bytes.data()),bytes.size()});
    };
  write("lv-albedo.dds",mipDds(0x31545844,512,512,10));
  write("lv-maps.dds",mipDds(0x35545844,256,256,9));
  write("lv-flat.dds",mipDds(0x31545844,256,256,9));
  write("lv-near.dds",mipDds(0x31545844,512,512,10));
  write("lv-ring.dds",mipDds(0x31545844,256,256,9));
  write("lv-scaled.dds",mipDds(0x31545844,512,512,10));
  write("lv-stale.dds",mipDds(0x31545844,512,512,10));
  write("lv-hold.dds",mipDds(0x31545844,256,256,9));
  write("lv-noarea.dds",mipDds(0x31545844,256,256,9));
  write("lv-turn.dds",mipDds(0x31545844,512,512,10));
  for(const char* name:{"lv-order-0.dds","lv-visible.dds"})
    write(name,mipDds(0x31545844,512,512,10));
  // the farther candidates of the load order test: another size, so their bytes in flight tell them apart
  for(const char* name:{"lv-order-1.dds","lv-order-2.dds"})
    write(name,mipDds(0x31545844,1024,1024,11));
  textFile(fixture.root/"materials/materials.json",R"({
    "LV_ORDER_0.TGA":{"albedo":"lv-order-0.dds"},
    "LV_ORDER_1.TGA":{"albedo":"lv-order-1.dds"},
    "LV_ORDER_2.TGA":{"albedo":"lv-order-2.dds"},
    "LV_VISIBLE.TGA":{"albedo":"lv-visible.dds"},
    "LV_STREAM.TGA":{"albedo":"lv-albedo.dds","normal":"lv-maps.dds","orm":"lv-maps.dds"},
    "LV_FLAT.TGA":{"albedo":"lv-flat.dds"},
    "LV_NEAR.TGA":{"albedo":"lv-near.dds"},
    "LV_RING.TGA":{"albedo":"lv-ring.dds"},
    "LV_SCALED.TGA":{"albedo":"lv-scaled.dds","scale":2.17},
    "LV_STALE.TGA":{"albedo":"lv-stale.dds"},
    "LV_HOLD.TGA":{"albedo":"lv-hold.dds"},
    "LV_NOAREA.TGA":{"albedo":"lv-noarea.dds"},
    "LV_TURN.TGA":{"albedo":"lv-turn.dds"}
  })");
  setenv("OPENGOTHIC_TEXTURE_STREAMING","1",1);
  setenv("OPENGOTHIC_TEXTURE_MIPS","1",1);
  setenv("OPENGOTHIC_TEXTURE_FAR_SKIP_MIPS","2",1);
  setenv("OPENGOTHIC_TEXTURE_BUDGET_MB","256",1);
  setenv("OPENGOTHIC_TEXTURE_TARGET_MB","192",1);
  setenv("OPENGOTHIC_TEXTURE_SCREEN_MIPS","1",1);
  unsetenv("OPENGOTHIC_TEXTURE_MIP_RESERVE");
  {
  Resources resources(device);
  Shaders shaders;
  shaders.waitCompiler();
  Material material, flat, near, ring, scaled, stale, turn, visible;
  std::array<Material,3> order;
  {
  Resources::TextureLoadScope far(true);
  for(size_t i=0;i<order.size();++i)
    order[i]=Material(source("LV_ORDER_"+std::to_string(i)+".TGA"),true);
  visible=Material(source("LV_VISIBLE.TGA"),true);
  material=Material(source("LV_STREAM.TGA"),true);
  flat=Material(source("LV_FLAT.TGA"),true);
  near=Material(source("LV_NEAR.TGA"),true);
  ring=Material(source("LV_RING.TGA"),true);
  scaled=Material(source("LV_SCALED.TGA"),true);
  stale=Material(source("LV_STALE.TGA"),true);
  turn=Material(source("LV_TURN.TGA"),true);
  }
  require(scaled.uvScale==2.17f && scaled.tex->w()==128,"the scaled fixture must carry its materials.json UV scale");
  const Material hold(source("LV_HOLD.TGA"),true);
  const Material noArea(source("LV_NOAREA.TGA"),true);
  require(hold.tex->w()==256 && noArea.tex->w()==256,"a texture loaded outside the Far scope must be whole");
  const auto sides=[&](int albedo, int maps) {
    return material.tex->w()==albedo && material.normal->w()==maps && material.orm->w()==maps;
    };
  require(sides(128,64) && flat.tex->w()==64 && near.tex->w()==128 && ring.tex->w()==64,
          "initial Far loads must still read the tail two levels down");
  // 90 degrees: K = 2048/2, log2 K = 10, and the levels use an eighth of a level more.
  Resources::setTextureView(2048,90);
  ChunkTable table;
  table.entries.push_back({{0,0},"",{0,0,0},{1,1,0},1});
  ChunkResidency residency(table);
  Tempest::Vec3 camera{-4000,50,0};
  const Tempest::Vec3 hero{0,0,1000000};
  const auto tick=[&] {
    const auto before=Resources::textureStats();
    const auto generation=Resources::textureGeneration();
    residency.update(hero,camera,1000);
    Resources::updateTextures(&residency);
    const auto after=Resources::textureStats();
    require(after.promoted-before.promoted+after.demoted-before.demoted+after.reloaded-before.reloaded<=
            TextureFlow::Limits().loads && Resources::textureGeneration()-generation<=1,
            "a frame must publish at most its loads in flight and one texture generation");
    };
  const auto waitFor=[&](auto done, const char* message) {
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(!done() && std::chrono::steady_clock::now()<deadline) {
      tick();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    require(done(),message);
    };
  using Need=Resources::TextureNeed;
  const auto stat=Resources::TextureCategory::Static;

  require(Resources::textureLookAhead()==TextureLookAhead::DefaultHorizon,
          "the look-ahead must be on by default, at its default horizon before a window of loads was measured");
  setenv("OPENGOTHIC_TEXTURE_LOOKAHEAD","0",1);
  Resources::configureTextures();
  require(Resources::textureLookAhead()==0,"OPENGOTHIC_TEXTURE_LOOKAHEAD=0 must turn the look-ahead off");
  setenv("OPENGOTHIC_TEXTURE_LOOKAHEAD","2",1);
  bool refused=false;
  try {
    Resources::configureTextures();
    }
  catch(const std::runtime_error&) {
    refused=true;
    }
  require(refused,"the look-ahead switch must take 0 or 1 only");
  unsetenv("OPENGOTHIC_TEXTURE_LOOKAHEAD");
  Resources::configureTextures({},{},{},{},"0");
  require(Resources::textureLookAhead()==0,"[GAME] texture_lookahead=0 must turn the look-ahead off");
  Resources::configureTextures();

  // 2^-4 at log2 K 10.125: the 512 albedo needs level 1, the 256 maps level 0, one reserve level included. With three
  // more textures six are short: the two maps by two levels, the rest by one - the albedo and a 512 in ring 3, two 1024
  // in ring 4 (2^-3). The four slots take the maps and the two in ring 3: their bytes in flight tell which.
  Resources::textureDemand(material,stat,true,Need{3,-4});
  Resources::textureDemand(order[0],stat,true,Need{3,-4});
  Resources::textureDemand(order[1],stat,true,Need{4,-3});
  Resources::textureDemand(order[2],stat,true,Need{4,-3});
  Resources::updateTextures(&residency);
  const auto started=Resources::textureStats();
  {
  const auto maps=TextureLevels::chainBytes(Tempest::TextureFormat::DXT5,256,256,9,0);
  const auto near=TextureLevels::chainBytes(Tempest::TextureFormat::DXT1,512,512,10,1);
  require(near!=TextureLevels::chainBytes(Tempest::TextureFormat::DXT1,1024,1024,11,1),"the candidates must differ in size");
  require(started.loadsInFlight==4 && started.shortfallCount==6 && started.flow.bytesInFlight==2*maps+2*near,
          "four loads must start, the largest shortfall first, then the nearest ring");
  }
  // The workers are done long before the next frame: it publishes them in the frame it sees them, in one generation,
  // and the time they waited for that frame is publication, not reading.
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  {
  const auto generation=Resources::textureGeneration();
  Resources::updateTextures(&residency);
  // one snapshot: two loads are still short, and a later one would count more time
  const auto stats=Resources::textureStats();
  const auto& flow=stats.flow;
  require(Resources::textureGeneration()==generation+1 && flow.publications>=1 && flow.latencyN==flow.publications,
          "ready loads must be published in the frame the main thread sees them");
  require(flow.seconds[TextureFlow::Publishing]>0.5 &&
          flow.seconds[TextureFlow::Reading]+flow.seconds[TextureFlow::Uploading]<0.1,
          "a load done between frames must count the wait for the next frame as publication");
  double split=0;
  for(const auto seconds:flow.seconds)
    split+=seconds;
  require(flow.waitN>=4 && flow.entries==6 && std::abs(split-stats.shortfallSeconds)<1e-9 && flow.visibleSeconds<=split,
          "the shortfall's split by state must add up to it, and each load must report its latency and wait");
  // Without the reserve only the two maps are short (the rest want their resident level 2): 2 x 0.2 s visible.
  require(flow.visibleSeconds-started.flow.visibleSeconds>0.3,"the visible shortfall must grow while the maps are short");
  }
  waitFor([&] { return sides(256,256) && order[1].tex->w()==512 && order[2].tex->w()==512; },
          "each texture of a material must load straight to its own wanted level");
  Resources::textureDemand(order[0],stat,false,Need{3,-4});
  Resources::textureDemand(order[1],stat,false,Need{4,-3});
  Resources::textureDemand(order[2],stat,false,Need{4,-3});
  require(Resources::textureStats().promoted==6 && Resources::textureStats().shortfallCount==0 &&
          Resources::textureStats().shortfallSeconds>0 && Resources::textureStats().levelBytes[1]>0,
          "raising must clear the shortfall it counted in texture-seconds and report bytes by level");

  setenv("OPENGOTHIC_TEXTURE_MIP_RESERVE","0",1);
  Resources::configureTextures();
  Resources::textureDemand(flat,stat,true,Need{3,-5});
  require(Resources::textureStats().promotionQueue==0,"without a reserve the resident tail must cover 2^-5 for a 256 texture");
  unsetenv("OPENGOTHIC_TEXTURE_MIP_RESERVE");
  Resources::configureTextures();
  require(Resources::textureStats().promotionQueue==1,"the default reserve must want that texture one level sharper");
  waitFor([&] { return flat.tex->w()==128; },"the reserve level must be loaded");
  Resources::textureDemand(flat,stat,false,Need{3,-5});
  // The look-ahead's exponent 2^-4 wants level 0, the present one 2^-5 the resident level 1: the texture loads ahead of
  // its need without counting as shortfall.
  {
  const auto before=Resources::textureStats();
  Resources::textureDemand(flat,stat,true,Need{3,-4,-5});
  const auto queued=Resources::textureStats();
  require(queued.promotionQueue==1 && queued.shortfallCount==0 && queued.flow.ahead==1,
          "a texture queued for the look-ahead alone must not count as short");
  waitFor([&] { return flat.tex->w()==256; },"a texture must load to its look-ahead level");
  require(Resources::textureStats().shortfallSeconds==before.shortfallSeconds &&
          Resources::textureStats().flow.aheadSeconds>before.flow.aheadSeconds,
          "loading ahead must add no shortfall, only time ahead");
  Resources::textureDemand(flat,stat,false,Need{3,-4,-5});
  }
  // The visible shortfall is of the present demand without the reserve. The 512 texture at level 2: the look-ahead's
  // 2^-3 wants level 0, the present 2^-4 level 1 (short by one), and without the reserve level 2 - what it has.
  Resources::textureDemand(visible,stat,true,Need{3,-3,-4});
  require(Resources::textureStats().shortfallCount==1 && Resources::textureStats().flow.visible==0,
          "a texture short only of the reserve of its present demand must not be visibly short");
  Resources::textureDemand(visible,stat,false,Need{3,-3,-4});
  Resources::textureDemand(visible,stat,true,Need{3,-3,-3});
  require(Resources::textureStats().shortfallCount==1 && Resources::textureStats().flow.visible==1,
          "a texture short of its present demand without the reserve must be visibly short");
  Resources::textureDemand(visible,stat,false,Need{3,-3,-3});
  // A load done between frames and its texture registered again before the next one (a second owner): the wait for
  // that frame is still publication, from the worker's own times.
  {
  const auto before=Resources::textureStats().flow;
  Resources::textureDemand(visible,stat,true,Need{3,-4});
  Resources::updateTextures(&residency);
  require(Resources::textureStats().loadsInFlight==1,"the 512 texture must start loading its level 1");
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  Resources::textureDemand(visible,stat,true,Need{3,-4});
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  Resources::updateTextures(&residency);
  const auto after=Resources::textureStats().flow;
  const auto delta=[&](TextureFlow::State state) { return after.seconds[state]-before.seconds[state]; };
  require(visible.tex->w()==256 && delta(TextureFlow::Publishing)>0.15 &&
          delta(TextureFlow::Reading)+delta(TextureFlow::Uploading)<0.05,
          "a registration between frames must keep the worker's times");
  Resources::textureDemand(visible,stat,false,Need{3,-4});
  Resources::textureDemand(visible,stat,false,Need{3,-4});
  }

  Resources::textureDemand(material,stat,true,Need{4,-3});
  waitFor([&] { return sides(512,256); },"the sharpest demand over the owners must decide");
  Resources::textureDemand(material,stat,false,Need{4,-3});
  tick();
  require(sides(512,256) && Resources::textureStats().demoted==0,"below the target a sharper level must stay");

  // Real owners: a static mesh measured at load, placed, evaluated from the view and again as the camera moves.
  SceneGlobals scene;
  VisualObjects visuals(scene,{{0,0,0},{1,1,0}});
  visuals.setTextureResidency(&residency);
  residency.update(hero,camera,1000);
  std::vector<Resources::Vertex> square={
    {{0,0,0},{0,0,1},{0,0},0xffffffffu},{{100,0,0},{0,0,1},{1,0},0xffffffffu},{{0,100,0},{0,0,1},{0,1},0xffffffffu}};
  StaticMesh measured(near,square,{0,1,2});
  for(auto& vertex:square)
    vertex.uv[0]=vertex.uv[1]=.5f;
  StaticMesh unmeasured(flat,square,{0,1,2});
  require(measured.sub[0].texel.valid() && !unmeasured.sub[0].texel.valid(),
          "a static mesh must measure its submesh when it is built");
  auto owner=visuals.get(measured,near,0,measured.sub[0].iboLength,true);
  owner.setObjMatrix(Tempest::Matrix4x4::mkIdentity());
  // 40 m at one UV unit per metre: 2^-5, level 2 for 512, the resident tail.
  require(Resources::textureStats().promotionQueue==0,"a measured owner 40 m away must keep the 512 texture's tail");
  camera={-2000,50,0};
  waitFor([&] { return near.tex->w()==256; },"an owner must be evaluated again once the camera passes its margin");
  // The same triangle with the UV scale 2.17 at 20 m: 2.17 UV units per metre, 2^-5, level 2 - the resident tail.
  StaticMesh scaledMesh(scaled,{{{0,0,0},{0,0,1},{0,0},0xffffffffu},{{100,0,0},{0,0,1},{1,0},0xffffffffu},
                                {{0,100,0},{0,0,1},{0,1},0xffffffffu}},{0,1,2});
  auto scaledOwner=visuals.get(scaledMesh,scaled,0,scaledMesh.sub[0].iboLength,true);
  scaledOwner.setObjMatrix(Tempest::Matrix4x4::mkIdentity());
  for(int i=0;i<3;++i)
    tick();
  require(Resources::textureStats().promotionQueue==0 && scaled.tex->w()==128,
          "an owner's density must include its material's UV scale");
  camera={-300,50,0};
  waitFor([&] { return near.tex->w()==512; },"a close owner must raise the texture to its whole chain");
  auto blank=visuals.get(unmeasured,flat,0,unmeasured.sub[0].iboLength,true);
  blank.setObjMatrix(Tempest::Matrix4x4::mkIdentity());
  waitFor([&] { return flat.tex->w()==256; },"an owner without a UV density must demand the whole chain");

  // Released owners at the end of `objects` shrink it while their entries stay in the queue: a few 10 m away come due
  // after the camera moves 6 m, many 200 m away keep the queue big enough to be compacted. None may be evaluated.
  {
  StaticMesh staleMesh(stale,{{{0,0,0},{0,0,1},{0,0},0xffffffffu},{{100,0,0},{0,0,1},{1,0},0xffffffffu},
                              {{0,100,0},{0,0,1},{0,1},0xffffffffu}},{0,1,2});
  std::vector<VisualObjects::Item> released;
  const auto droppedBefore=visuals.textureQueueDroppedPastEnd();
  for(size_t i=0;i<4200+8;++i) {
    auto at=Tempest::Matrix4x4::mkIdentity();
    at.translate(i<8 ? 1000.f : 20000.f,0,float(i)*10.f);
    released.push_back(visuals.get(staleMesh,stale,0,staleMesh.sub[0].iboLength,true));
    released.back().setObjMatrix(at);
    }
  released.clear();
  camera={-300,50,600};
  for(int i=0;i<20;++i) {
    tick();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  require(stale.tex->w()==128,"queue entries of released owners past the end of the objects must not be evaluated");
  const auto dropped=visuals.textureQueueDroppedPastEnd();
  require(dropped.first-droppedBefore.first==8 && dropped.second-droppedBefore.second==4200,
          "the due loop must drop the 8 due entries and the compaction the 4200 others, all past the end of the objects");
  }

  // 2^-5: the albedo needs level 2, the maps level 1. Nothing is lowered within the target, even after the delay.
  Resources::textureDemand(material,stat,true,Need{3,-5});
  Resources::textureDemand(material,stat,false,Need{3,-4});
  // An NPC's or item's ring demand moving from ring 2 to ring 3 keeps the whole chain, as under the ring rule.
  Resources::textureDemand(hold,Resources::TextureCategory::Dynamic,true,2u);
  Resources::textureDemand(hold,Resources::TextureCategory::Dynamic,true,3u);
  Resources::textureDemand(hold,Resources::TextureCategory::Dynamic,false,2u);
  // An owner whose triangles have no world area draws nothing: it asks for the coarsest allowed level.
  StaticMesh pointMesh(noArea,{{{50,50,0},{0,0,1},{0,0},0xffffffffu},{{50,50,0},{0,0,1},{1,0},0xffffffffu},
                               {{50,50,0},{0,0,1},{0,1},0xffffffffu}},{0,1,2});
  auto pointOwner=visuals.get(pointMesh,noArea,0,pointMesh.sub[0].iboLength,true);
  pointOwner.setObjMatrix(Tempest::Matrix4x4::mkIdentity());
  require(pointMesh.sub[0].texel.flat,"a mesh of point triangles must be measured as having nothing to draw");
  std::this_thread::sleep_for(std::chrono::milliseconds(5100));
  tick();
  require(sides(512,256) && Resources::textureStats().demoted==0,"lowering must wait for memory above the target");
  unsetenv("OPENGOTHIC_TEXTURE_TARGET_MB");
  Resources::configureTextures({},{},"0");
  bool firstObserved=false;
  waitFor([&] {
    if(Resources::textureStats().demoted==1 && !firstObserved) {
      require(sides(128,256),"pressure must first lower the texture with the most bytes to give back");
      firstObserved=true;
      }
    return Resources::textureStats().demoted==3;
    },"pressure must lower every eligible texture");
  require(firstObserved && sides(128,128) && near.tex->w()==512 && flat.tex->w()==256,
          "pressure must lower only to the wanted level and leave textures wanted whole");
  waitFor([&] { return noArea.tex->w()==64; },"an owner with nothing to draw must let its texture go to the lowest level");
  pointOwner=VisualObjects::Item();
  for(int i=0;i<20;++i) {
    tick();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  require(hold.tex->w()==256 && Resources::textureStats().demoted==4,
          "a ring-only owner within ring 3 must keep its texture whole under pressure");
  Resources::textureDemand(hold,Resources::TextureCategory::Dynamic,false,3u);

  setenv("OPENGOTHIC_TEXTURE_SCREEN_MIPS","0",1);
  Resources::configureTextures({},{},"0");
  Resources::textureDemand(ring,stat,true,Need{4,-3});
  require(Resources::textureStats().promotionQueue==0,"the ring rule must ignore a resolution demand beyond ring 2");
  setenv("OPENGOTHIC_TEXTURE_SCREEN_MIPS","1",1);
  Resources::configureTextures({},{},"0");
  require(Resources::textureStats().promotionQueue==1,"switching levels back on must apply the same demand");
  Resources::textureDemand(ring,stat,false,Need{4,-3});

  // An owner that turns without moving is placed again: a 20 m strip along x, 40 m from the hero along z, turned by
  // 90 degrees about its centre comes from 39.9 to 30 m, 2^-5 -> 2^-4: its 512 texture goes from level 2 to level 1.
  {
  StaticMesh stripMesh(turn,{{{-1000,0,-10},{0,0,1},{0,0},0xffffffffu},{{1000,0,-10},{0,0,1},{20,0},0xffffffffu},
                             {{-1000,0,10},{0,0,1},{0,0.2f},0xffffffffu},{{1000,0,10},{0,0,1},{20,0.2f},0xffffffffu}},
                       {0,1,2, 2,1,3});
  auto strip=visuals.get(stripMesh,turn,0,stripMesh.sub[0].iboLength,true);
  auto at=Tempest::Matrix4x4::mkIdentity();
  at.translate(hero.x,hero.y,hero.z-4000);
  strip.setObjMatrix(at);
  for(int i=0;i<10;++i) {
    tick();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  require(turn.tex->w()==128,"a strip lying across the view 40 m away must keep the 512 texture's tail");
  at.rotateOY(90);
  strip.setObjMatrix(at);
  waitFor([&] { return turn.tex->w()==256; },"an owner turned at the same position must be placed again and demand sharper");
  }
  Resources::textureDemand(material,stat,false,Need{3,-5});
  owner=VisualObjects::Item();
  blank=VisualObjects::Item();
  scaledOwner=VisualObjects::Item();
  device.waitIdle();
  }
  unsetenv("OPENGOTHIC_TEXTURE_MIPS");
  unsetenv("OPENGOTHIC_TEXTURE_FAR_SKIP_MIPS");
  unsetenv("OPENGOTHIC_TEXTURE_BUDGET_MB");
  unsetenv("OPENGOTHIC_TEXTURE_TARGET_MB");
  unsetenv("OPENGOTHIC_TEXTURE_SCREEN_MIPS");
  }

// Step 2 of the levels (reports/engine/2026-11-10-texture-mips/README.md): owners placed by a position (NPCs, items,
// mobsi) demand levels by their distance, the world loads at the coarsest level, a view jump completes what it left
// far short.
// Step 4: the look-ahead's horizon from the loads measured - twice the p90 of each window of 32, at most the cap.
void textureHorizonLevels(Tempest::Device& device, const Fixture& fixture) {
  using TextureLookAhead::WindowSamples;
  std::string json="{";
  for(size_t i=0;i<2*WindowSamples;++i) {
    const auto name="hz-"+std::to_string(i);
    const auto bytes=mipDds(0x31545844,256,256,9);
    textFile(fixture.root/"materials"/(name+".dds"),{reinterpret_cast<const char*>(bytes.data()),bytes.size()});
    json+=std::string(i==0 ? "" : ",")+"\"HZ_"+std::to_string(i)+".TGA\":{\"albedo\":\""+name+".dds\"}";
    }
  textFile(fixture.root/"materials/materials.json",json+"}");
  setenv("OPENGOTHIC_TEXTURE_STREAMING","1",1);
  setenv("OPENGOTHIC_TEXTURE_MIPS","1",1);
  setenv("OPENGOTHIC_TEXTURE_FAR_SKIP_MIPS","2",1);
  setenv("OPENGOTHIC_TEXTURE_BUDGET_MB","256",1);
  setenv("OPENGOTHIC_TEXTURE_TARGET_MB","192",1);
  setenv("OPENGOTHIC_TEXTURE_SCREEN_MIPS","1",1);
  unsetenv("OPENGOTHIC_TEXTURE_LOOKAHEAD");
  {
  Resources resources(device);
  Shaders shaders;
  shaders.waitCompiler();
  std::vector<Material> textures(2*WindowSamples);
  {
  Resources::TextureLoadScope far(true);
  for(size_t i=0;i<textures.size();++i)
    textures[i]=Material(source("HZ_"+std::to_string(i)+".TGA"),true);
  }
  require(textures[0].tex->w()==64 && Resources::textureLookAhead()==TextureLookAhead::DefaultHorizon,
          "before a window of loads the horizon must be the default");
  const auto stat=Resources::TextureCategory::Static;
  // A window of loads asked for at once and published as they come; `delay` before the first frame lengthens each.
  const auto load=[&](size_t first, std::chrono::milliseconds delay) {
    for(size_t i=first;i<first+WindowSamples;++i)
      Resources::textureDemand(textures[i],stat,true,0u);
    std::this_thread::sleep_for(delay);
    const auto loaded=[&] {
      return std::all_of(textures.begin()+ptrdiff_t(first),textures.begin()+ptrdiff_t(first+WindowSamples),
                         [](const Material& m) { return m.tex->w()==256; });
      };
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(!loaded() && std::chrono::steady_clock::now()<deadline) {
      Resources::updateTextures(nullptr);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    require(loaded(),"a window of loads must complete");
    for(size_t i=first;i<first+WindowSamples;++i)
      Resources::textureDemand(textures[i],stat,false,0u);
    };
  load(0,std::chrono::milliseconds(0));
  const auto first=Resources::textureFlowDistributions();
  require(first.latency.count()==WindowSamples &&
          Resources::textureLookAhead()==TextureLookAhead::horizon(first.latency.quantile(0.9),WindowSamples) &&
          Resources::textureLookAhead()<TextureLookAhead::MaxHorizon,
          "the horizon must come from the p90 of the first window of loads");
  load(WindowSamples,std::chrono::milliseconds(300));
  const auto second=Resources::textureFlowDistributions();
  require(second.latency.count()==2*WindowSamples && second.latency.quantile(0.9,first.latency)>=300 &&
          Resources::textureLookAhead()==TextureLookAhead::MaxHorizon,
          "a window of slow loads must take the horizon to its cap");
  }
  unsetenv("OPENGOTHIC_TEXTURE_MIPS");
  unsetenv("OPENGOTHIC_TEXTURE_FAR_SKIP_MIPS");
  unsetenv("OPENGOTHIC_TEXTURE_BUDGET_MB");
  unsetenv("OPENGOTHIC_TEXTURE_TARGET_MB");
  unsetenv("OPENGOTHIC_TEXTURE_SCREEN_MIPS");
  }

void textureOwnerLevels(Tempest::Device& device, const Fixture& fixture) {
  const char* names[]={"ow-npc","ow-item","ow-mobsi","ow-hero","ow-switch","ow-start","ow-lone","ow-whole","ow-rings",
                       "ow-jump-a","ow-jump-b","ow-jump-c","ow-share","ow-between","ow-scaled","ow-shield","ow-armour","ow-big-0","ow-big-1","ow-big-2",
                       "ow-big-3","ow-huge-0","ow-huge-1"};
  std::string json="{";
  for(const auto* name:names) {
    // 4096 for the jump's time budget: each load takes milliseconds
    const bool big=std::string_view(name).starts_with("ow-big");
    // 8192 for the bytes in flight: a whole chain takes 42.7 MiB, two more than TextureFlow::Limits::bytes
    const bool huge=std::string_view(name).starts_with("ow-huge");
    const auto bytes=huge ? mipDds(0x31545844,8192,8192,14) : big ? mipDds(0x31545844,4096,4096,13) :
                                                                    mipDds(0x31545844,512,512,10);
    textFile(fixture.root/"materials"/(std::string(name)+".dds"),{reinterpret_cast<const char*>(bytes.data()),bytes.size()});
    std::string key(name);
    for(auto& c:key)
      c=c=='-' ? '_' : char(std::toupper(uint8_t(c)));
    json+=std::string(json.size()>1 ? "," : "")+"\""+key+".TGA\":{\"albedo\":\""+name+".dds\"}";
    }
  textFile(fixture.root/"materials/materials.json",json+"}");
  setenv("OPENGOTHIC_TEXTURE_STREAMING","1",1);
  setenv("OPENGOTHIC_TEXTURE_MIPS","1",1);
  setenv("OPENGOTHIC_TEXTURE_FAR_SKIP_MIPS","2",1);
  setenv("OPENGOTHIC_TEXTURE_BUDGET_MB","256",1);
  setenv("OPENGOTHIC_TEXTURE_TARGET_MB","192",1);
  setenv("OPENGOTHIC_TEXTURE_SCREEN_MIPS","1",1);
  unsetenv("OPENGOTHIC_TEXTURE_MIP_RESERVE");
  unsetenv("OPENGOTHIC_TEXTURE_JUMP_MS");
  {
  Resources resources(device);
  Shaders shaders;
  shaders.waitCompiler();
  using Need=Resources::TextureNeed;
  const auto stat=Resources::TextureCategory::Static;
  Material npc, item, mobsi, hero, toggle, jumpA, jumpB, jumpC, share, between, scaled, shield, armour;
  std::array<Material,4> big;
  std::array<Material,2> huge;
  {
  Resources::TextureLoadScope far(true);
  npc=Material(source("OW_NPC.TGA"),true);
  item=Material(source("OW_ITEM.TGA"),true);
  mobsi=Material(source("OW_MOBSI.TGA"),true);
  hero=Material(source("OW_HERO.TGA"),true);
  toggle=Material(source("OW_SWITCH.TGA"),true);
  jumpA=Material(source("OW_JUMP_A.TGA"),true);
  jumpB=Material(source("OW_JUMP_B.TGA"),true);
  jumpC=Material(source("OW_JUMP_C.TGA"),true);
  share=Material(source("OW_SHARE.TGA"),true);
  between=Material(source("OW_BETWEEN.TGA"),true);
  scaled=Material(source("OW_SCALED.TGA"),true);
  shield=Material(source("OW_SHIELD.TGA"),true);
  armour=Material(source("OW_ARMOUR.TGA"),true);
  for(size_t i=0;i<big.size();++i)
    big[i]=Material(source("OW_BIG_"+std::to_string(i)+".TGA"),true);
  for(size_t i=0;i<huge.size();++i)
    huge[i]=Material(source("OW_HUGE_"+std::to_string(i)+".TGA"),true);
  }
  require(npc.tex->w()==128 && jumpC.tex->w()==128,"textures loaded in a Far scope outside a world load keep the tail");

  // Loading at start: every texture that can have a tail is read at its coarsest level whatever its scope, then the
  // completion before the first frame loads the wanted levels; one nobody wants stays coarsest.
  Material start, lone, whole;
  {
  Resources::TextureStartup startup;
  startup.begin();
  Resources::TextureLoadScope near(false);
  start=Material(source("OW_START.TGA"),true);
  lone=Material(source("OW_LONE.TGA"),true);
  whole=Material(source("OW_WHOLE.TGA"),true);
  }
  require(start.tex->w()==64 && lone.tex->w()==64 && whole.tex->w()==64,
          "a world load must read every texture with a tail at its coarsest level, even in a near scope");
  // log2 K = 10.125 (90 degrees, 2048 pixels): 2^-4 wants level 1 of a 512 texture, one reserve level included.
  Resources::setTextureView(2048,90);
  Resources::textureDemand(start,stat,true,Need{3,-4});
  Resources::textureDemand(whole,Resources::TextureCategory::Dynamic,true);
  Resources::finishTextureLoading();
  require(start.tex->w()==256 && whole.tex->w()==512 && lone.tex->w()==64,
          "the completion must load each wanted level and leave the texture nobody wants at its coarsest level");
  Resources::textureDemand(start,stat,false,Need{3,-4});
  Resources::textureDemand(whole,Resources::TextureCategory::Dynamic,false);

  ChunkTable table;
  table.entries.push_back({{0,0},"",{0,0,0},{1,1,0},1});
  ChunkResidency residency(table);
  // The middle of chunk (0,0); owners are placed along x from here.
  Tempest::Vec3 view{3200,0,3200};
  const auto tick=[&] {
    residency.update(view,view,1000);
    Resources::updateTextures(&residency);
    };
  const auto waitFor=[&](auto done, const char* message) {
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(!done() && std::chrono::steady_clock::now()<deadline) {
      tick();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    require(done(),message);
    };
  SceneGlobals scene;
  VisualObjects visuals(scene,{{0,0,0},{1,1,0}});
  visuals.setTextureResidency(&residency);
  residency.update(view,view,1000);

  // A skinned mesh is measured in its bind pose: the bone frames hold the vertices ten times further apart, the
  // model space positions make a 1 m triangle with one UV unit per metre.
  asset::ModelMesh model;
  model.meshes.resize(1);
  auto& skin=model.meshes[0];
  skin.mesh.positions={{0,0,0},{100,0,0},{0,100,0}};
  skin.mesh.bbox={{0,0,0},{100,100,0}};
  skin.mesh.sub_meshes.resize(1);
  skin.mesh.sub_meshes[0].mat=source("OW_NPC.TGA");
  skin.mesh.sub_meshes[0].wedges={{{0,0,1},{0,0},0},{{0,0,1},{1,0},1},{{0,0,1},{0,1},2}};
  skin.mesh.sub_meshes[0].triangles={{{0,1,2}}};
  skin.weights={{{1.f,{0,0,0},0}},{{1.f,{1000,0,0},0}},{{1.f,{0,1000,0},0}}};
  PackedMesh packedSkin(skin);
  AnimMesh anim(packedSkin);
  require(anim.sub.size()==1 && std::abs(anim.sub[0].texel.uvPerMetre-1.f)<1e-3f && anim.sub[0].texel.max.x==100.f &&
          std::abs(anim.sub[0].texel.radius()-141.42f)<0.1f,
          "a skinned mesh must take its UV density and bounds from its bind pose");

  // The radius of a 1 m triangle's bounds from the origin is 1.414 m: an owner 41.414 m from the view is 40 m away,
  // 2^-5 for one UV unit per metre, level 2 of a 512 texture - the resident tail.
  const auto at=[&](float metres, float side=0) { return Tempest::Vec3{view.x+(metres+1.4142f)*100.f,0,view.z+side*100.f}; };
  auto bones=visuals.alloc(sizeof(Tempest::Matrix4x4));
  auto body=visuals.get(anim,anim.sub[0].material,anim.sub[0].iboOffset,anim.sub[0].iboSize,bones);
  // Placed 40 m away, in the band [32, 80) of 2^-5: evaluated exactly, with no reach until it moves.
  require(body.setTextureOwner(at(40),true)==0.f,"a newly placed owner must get no reach");
  tick();
  require(npc.tex->w()==128 && Resources::textureStats().promotionQueue==0,
          "an NPC's body 40 m away must keep the 512 texture's tail");
  require(body.setTextureOwner(at(40),true)==0.f,"an owner called again where it is must only be compared");
  // Moved: half the room to the nearer band edge (8 m), at most TextureScreenDemand::MaxReach.
  const float reach=body.setTextureOwner(at(40.01f),true);
  require(std::abs(reach-400.f)<1.f,"a moving owner 40 m away must get 4 m of reach, half of its 8 m margin");
  // 3 m to the side: within its reach it is not evaluated.
  require(std::abs(body.setTextureOwner(at(40.01f,3),true)-100.f)<1.f,
          "a move within the owner's reach must only be compared, not evaluated");
  require(body.setTextureOwner(at(3,5),true)<400.f,"a move past the reach must evaluate the owner again");
  waitFor([&] { return npc.tex->w()==512; },"an NPC's body 3 m away must raise its texture to the whole chain");

  // A body is registered before its pose sets the NPC's matrix (MdlVisual::implSetBody): scaled by 2 there, it has
  // half the density and a 2.828 m radius, 38.6 m away - 2^-4, level 1 - although it does not move.
  {
  auto scaledBones=visuals.alloc(sizeof(Tempest::Matrix4x4));
  auto tall=visuals.get(anim,scaled,anim.sub[0].iboOffset,anim.sub[0].iboSize,scaledBones);
  tall.setTextureOwner(at(40),true);
  tick();
  require(scaled.tex->w()==128,"an unscaled body 40 m away must keep the tail");
  auto matrix=Tempest::Matrix4x4::mkIdentity();
  matrix.translate(at(40));
  matrix.scale(2);
  tall.setObjMatrix(matrix);
  tall.setTextureOwner(at(40),true);
  Resources::finishTextureLoading();
  require(scaled.tex->w()==256,"a body's scale set after it was registered must count");
  }

  // An item lying in the world: a static mesh drawn as dynamic, 20 m away - 2^-4, level 1; then 3 m - level 0.
  StaticMesh itemMesh(item,{{{0,0,0},{0,0,1},{0,0},0xffffffffu},{{100,0,0},{0,0,1},{1,0},0xffffffffu},
                            {{0,100,0},{0,0,1},{0,1},0xffffffffu}},{0,1,2});
  auto lying=visuals.get(itemMesh,item,0,itemMesh.sub[0].iboLength,false);
  lying.setTextureOwner(at(20,-3),true);
  waitFor([&] { return item.tex->w()==256; },"an item 20 m away must load level 1 of its 512 texture");
  lying.setTextureOwner(at(3,-3),true);
  waitFor([&] { return item.tex->w()==512; },"an item 3 m away must load its whole chain");

  // An interactive vob: its body is created static, then placed by its vob's position (ObjVisual::createView).
  MeshObjects meshes(visuals);
  StaticMesh mobsiMesh(mobsi,{{{0,0,0},{0,0,1},{0,0},0xffffffffu},{{100,0,0},{0,0,1},{1,0},0xffffffffu},
                              {{0,100,0},{0,0,1},{0,1},0xffffffffu}},{0,1,2});
  MeshObjects::Mesh bench(meshes,mobsiMesh,0,true);
  bench.setTextureCategory(stat);
  const float benchReach=bench.setTextureOwner(at(40,10),true);
  tick();
  require(benchReach==0.f && mobsi.tex->w()==128 && Resources::textureStats().promotionQueue==0,
          "a mobsi 40 m away must keep its texture's tail and, never moved, be evaluated exactly");
  require(bench.setTextureOwner(at(40,10),true)==0.f,"a mobsi that does not move must only be compared");
  bench.setTextureOwner(at(3,10),true);
  waitFor([&] { return mobsi.tex->w()==512; },"a mobsi 3 m away must load its whole chain");

  // The hero's visuals are not spatial: the whole chain wherever they are.
  auto heroBones=visuals.alloc(sizeof(Tempest::Matrix4x4));
  auto heroBody=visuals.get(anim,hero,anim.sub[0].iboOffset,anim.sub[0].iboSize,heroBones);
  require(std::isinf(heroBody.setTextureOwner(at(500),false)),"the hero's visuals must not follow their movement");
  waitFor([&] { return hero.tex->w()==512; },"the hero's visuals must keep their textures whole far away");

  // The switch: the same owner 40 m away within ring 2 keeps the tail by levels and wants the whole chain by rings.
  auto switched=visuals.get(itemMesh,toggle,0,itemMesh.sub[0].iboLength,false);
  switched.setTextureOwner(at(40,-10),true);
  tick();
  require(toggle.tex->w()==128,"an owner 40 m away must keep the tail by levels");
  setenv("OPENGOTHIC_TEXTURE_SCREEN_MIPS","0",1);
  Resources::configureTextures();
  visuals.updateTextureView();
  waitFor([&] { return toggle.tex->w()==512; },"texture_screen_mips=0 must restore the ring rule for owners placed by a position");
  setenv("OPENGOTHIC_TEXTURE_SCREEN_MIPS","1",1);
  Resources::configureTextures();
  visuals.updateTextureView();

  // One margin is not spent twice. An owner 40 m away that moves gets 4 m of its 8 m margin, the view the margin of
  // the sphere grown by those 4 m (36 m: 4 m). The view comes 7 m closer, then the owner 2 m: the true distance is
  // 31 m, 2^-4, and that must be asked for although the owner stayed within its reach.
  {
  const Tempest::Vec3 start{view.x-4141.42f,0,view.z};
  auto owner=visuals.get(itemMesh,share,0,itemMesh.sub[0].iboLength,false);
  owner.setTextureOwner(start,true);
  require(std::abs(owner.setTextureOwner({start.x-1,0,start.z},true)-400.f)<1.f,"the owner must get 4 m of reach");
  tick();
  require(share.tex->w()==128,"an owner 40 m away must keep the tail");
  view.x-=700;
  tick();
  require(std::abs(owner.setTextureOwner({start.x+199,0,start.z},true)-200.f)<1.f,"a 2 m move must stay within the reach");
  waitFor([&] { return share.tex->w()==256; },"the view and the owner sharing one margin must ask for 2^-4 at 31 m");
  }
  // A view-queue evaluation between two moves of the owner: the owner moves 3.5 m closer within its reach; the view
  // moves 8.1 m to the side, past its own margin, and the queue evaluates the owner at its anchor; the owner moves
  // 0.4 m more, still within the reach it got at its own evaluation; the view comes 6 m closer. The true distance is
  // 31.1 m, 2^-4.
  {
  const Tempest::Vec3 start{view.x,0,view.z-4142.42f};
  auto owner=visuals.get(itemMesh,between,0,itemMesh.sub[0].iboLength,false);
  owner.setTextureOwner({start.x,0,start.z+1},true);
  require(std::abs(owner.setTextureOwner(start,true)-400.f)<1.f,"the owner must get 4 m of reach");
  tick();
  require(std::abs(owner.setTextureOwner({start.x,0,start.z+350},true)-50.f)<1.f,"a 3.5 m move must stay within the reach");
  view.x+=810;
  tick();
  require(between.tex->w()==128,"a view moving to the side must not change the owner's exponent");
  require(std::abs(owner.setTextureOwner({start.x,0,start.z+390},true)-10.f)<1.f,
          "the view queue's evaluation must leave the reach of the owner's own evaluation as it was");
  view.z-=600;
  tick();
  waitFor([&] { return between.tex->w()==256; },"an evaluation by the view must count where the owner may have gone");
  }

  // A part added to an NPC (MdlVisual::bind, implSetBody) is placed where the NPC is now, not where it was last
  // evaluated: the NPC's shield gets 4 m of reach 40 m away, the view comes 7 m closer, the NPC 2 m within its reach,
  // then it draws a sword - 31 m away, 2^-4, level 1 (33 m from the old anchor: 2^-5).
  {
  const Tempest::Vec3 start{view.x-4142.42f,0,view.z};
  StaticMesh shieldMesh(shield,{{{0,0,0},{0,0,1},{0,0},0xffffffffu},{{100,0,0},{0,0,1},{1,0},0xffffffffu},
                                {{0,100,0},{0,0,1},{0,1},0xffffffffu}},{0,1,2});
  StaticMesh swordMesh(armour,{{{0,0,0},{0,0,1},{0,0},0xffffffffu},{{100,0,0},{0,0,1},{1,0},0xffffffffu},
                               {{0,100,0},{0,0,1},{0,1},0xffffffffu}},{0,1,2});
  MdlVisual npcVisual;
  npcVisual.setTextureOwner({start.x+1,0,start.z},true);
  npcVisual.setShield(MeshObjects::Mesh(meshes,shieldMesh,0,false));
  require(std::abs(npcVisual.setTextureOwner(start,true)-400.f)<1.f,"the NPC must get 4 m of reach 40 m away");
  view.x-=700;
  tick();
  require(std::abs(npcVisual.setTextureOwner({start.x+200,0,start.z},true)-200.f)<1.f,"a 2 m move must stay within the reach");
  npcVisual.setSword(MeshObjects::Mesh(meshes,swordMesh,0,false));
  Resources::finishTextureLoading();
  require(armour.tex->w()==256,"a part added after the NPC moved within its reach must be placed where the NPC is");
  }

  // A view jump: owners 1000 m away hold tails; the view lands 3 m from one (short of its level by 2) and 20 m from
  // another (short by 1). The first completes before the next frame, the second loads as usual.
  auto ownerA=visuals.get(itemMesh,jumpA,0,itemMesh.sub[0].iboLength,false);
  auto ownerB=visuals.get(itemMesh,jumpB,0,itemMesh.sub[0].iboLength,false);
  auto ownerC=visuals.get(itemMesh,jumpC,0,itemMesh.sub[0].iboLength,false);
  const Tempest::Vec3 siteA{view.x+100000,0,view.z}, siteC{view.x-100000,0,view.z};
  ownerA.setTextureOwner(siteA,true);
  ownerB.setTextureOwner({siteA.x+1700,0,siteA.z},true);
  ownerC.setTextureOwner(siteC,true);
  tick();
  require(jumpA.tex->w()==128 && jumpB.tex->w()==128 && jumpC.tex->w()==128,"owners 1000 m away must keep their tails");
  const auto jumpsBefore=Resources::textureStats().jumps;
  view={siteA.x-441.42f,0,siteA.z};
  residency.update(view,view,1000);
  require(residency.viewJumped(),"a move of 1000 m in one update must be a jump");
  visuals.updateTextureView(true);
  Resources::completeTextureJump();
  auto stats=Resources::textureStats();
  require(jumpA.tex->w()==512 && jumpB.tex->w()==128 && stats.jumps==jumpsBefore+1 && stats.jumpSyncMsMax>0 &&
          stats.jumpsOpen==1,
          "a jump must complete before the frame the textures short by two levels or more, and only those");
  waitFor([&] { return jumpB.tex->w()==256 && Resources::textureStats().jumpsOpen==0; },
          "the rest must load as usual and the jump close when the shortfall reaches zero");
  require(Resources::textureStats().jumpZeroMsMax>0,"a jump must report its time to a zero shortfall");
  residency.update(view,view,1000);
  require(!residency.viewJumped(),"standing still must not be a jump");

  setenv("OPENGOTHIC_TEXTURE_JUMP_MS","0",1);
  Resources::configureTextures();
  view={siteC.x+441.42f,0,siteC.z};
  residency.update(view,view,1000);
  visuals.updateTextureView(true);
  Resources::completeTextureJump();
  require(jumpC.tex->w()==128 && Resources::textureStats().jumps==jumpsBefore+2,
          "OPENGOTHIC_TEXTURE_JUMP_MS=0 must leave a jump's textures to the usual loading");
  waitFor([&] { return jumpC.tex->w()==512; },"without the synchronous part the jump's textures must still load");

  // A small budget: four textures of 4096, short by two levels; the two first loads start at once, and waiting for
  // the first passes the 1 ms budget, so no third one starts.
  setenv("OPENGOTHIC_TEXTURE_JUMP_MS","1",1);
  Resources::configureTextures();
  require(big[0].tex->w()==1024,"the large textures must hold their tails");
  for(auto& m:big)
    Resources::textureDemand(m,stat,true,Need{3,20});
  Resources::completeTextureJump();
  const auto completed=size_t(std::count_if(big.begin(),big.end(),[](const Material& m) { return m.tex->w()==4096; }));
  require(completed>=1 && completed<=2,"no load may start after the jump's budget has passed");
  for(auto& m:big)
    Resources::textureDemand(m,stat,false,Need{3,20});
  unsetenv("OPENGOTHIC_TEXTURE_JUMP_MS");
  Resources::configureTextures();

  // The bytes in flight: the whole chain of an 8192 DXT1 takes 42.7 MiB, two of them more than the 64 MiB cap. One load
  // runs, though four slots are free; the other starts when it is published.
  for(auto& m:huge)
    Resources::textureDemand(m,stat,true,Need{3,20});
  tick();
  require(Resources::textureStats().shortfallCount==2 && Resources::textureStats().loadsInFlight==1,
          "loads must start only within the bytes in flight");
  waitFor([&] { return std::all_of(huge.begin(),huge.end(),[](const Material& m) { return m.tex->w()==8192; }); },
          "the capped loads must complete");
  for(auto& m:huge)
    Resources::textureDemand(m,stat,false,Need{3,20});

  body=VisualObjects::Item();
  heroBody=VisualObjects::Item();
  lying=VisualObjects::Item();
  switched=VisualObjects::Item();
  ownerA=VisualObjects::Item();
  ownerB=VisualObjects::Item();
  ownerC=VisualObjects::Item();
  bench=MeshObjects::Mesh();
  device.waitIdle();
  }
  // With texture_screen_mips=0 a world load keeps the scopes.
  setenv("OPENGOTHIC_TEXTURE_SCREEN_MIPS","0",1);
  {
  Resources resources(device);
  Resources::TextureStartup startup;
  startup.begin();
  Resources::TextureLoadScope near(false);
  const Material rings(source("OW_RINGS.TGA"),true);
  require(rings.tex->w()==512,"by the ring rule a world load must read a texture in a near scope whole");
  }
  unsetenv("OPENGOTHIC_TEXTURE_MIPS");
  unsetenv("OPENGOTHIC_TEXTURE_FAR_SKIP_MIPS");
  unsetenv("OPENGOTHIC_TEXTURE_BUDGET_MB");
  unsetenv("OPENGOTHIC_TEXTURE_TARGET_MB");
  unsetenv("OPENGOTHIC_TEXTURE_SCREEN_MIPS");
  }
}

// Step 3 with a device: the target from its budget, OPENGOTHIC_TEXTURE_VRAM_MB, the setting and the environment; above
// the target unrequested textures leave first, then textures above their wanted level are lowered, then the stage
// makes the wanted levels coarser, down to the deepest when nothing fits; what is never coarsened; measuring and the
// ring rule stay on stage 0.
void textureBudgetLevels(Tempest::Device& device, const Fixture& fixture) {
  using namespace TextureBudget;
  // six measured 2048x2048 DXT5 (5.33 MiB whole, lowest level 5), the rest 512x512 DXT5
  const char* names[]={"bg-0","bg-1","bg-2","bg-3","bg-4","bg-5","bg-idle-0","bg-idle-1","bg-whole","bg-ring","bg-chain",
                       "bg-class","bg-sweep"};
  const auto big=mipDds(0x35545844,2048,2048,12), small=mipDds(0x35545844,512,512,10);
  std::string json="{";
  for(const auto* name:names) {
    const auto& bytes=std::string_view(name).size()==4 || std::string_view(name)=="bg-class" ? big : small;
    textFile(fixture.root/"materials"/(std::string(name)+".dds"),{reinterpret_cast<const char*>(bytes.data()),bytes.size()});
    std::string key(name);
    for(auto& c:key)
      c=c=='-' ? '_' : char(std::toupper(uint8_t(c)));
    json+=std::string(json.size()>1 ? "," : "")+"\""+key+".TGA\":{\"albedo\":\""+name+".dds\"}";
    }
  textFile(fixture.root/"materials/materials.json",json+"}");
  textFile(fixture.root/"_work/BG_PINNED.dds",{reinterpret_cast<const char*>(small.data()),small.size()});
  setenv("OPENGOTHIC_TEXTURE_STREAMING","1",1);
  setenv("OPENGOTHIC_TEXTURE_MIPS","1",1);
  setenv("OPENGOTHIC_TEXTURE_FAR_SKIP_MIPS","2",1);
  setenv("OPENGOTHIC_TEXTURE_SCREEN_MIPS","1",1);
  // no reserve: the measured textures want level 1 at stage 0 and stand at 0 after their load
  setenv("OPENGOTHIC_TEXTURE_MIP_RESERVE","0",1);
  unsetenv("OPENGOTHIC_TEXTURE_BUDGET_MB");
  unsetenv("OPENGOTHIC_TEXTURE_TARGET_MB");
  unsetenv("OPENGOTHIC_TEXTURE_VRAM_MB");
  {
  Resources resources(device);
  Shaders shaders;
  shaders.waitCompiler();
  // a view for an owner measured on screen (VisualObjects): the hero far away, the camera along x
  ChunkTable table;
  table.entries.push_back({{0,0},"",{0,0,0},{1,1,0},1});
  ChunkResidency residency(table);
  SceneGlobals scene;
  VisualObjects visuals(scene,{{0,0,0},{1,1,0}});
  visuals.setTextureResidency(&residency);
  const auto tick=[] { Resources::updateTextures(nullptr); };
  const auto evaluate=[&] {
    Resources::configureTextures();
    tick();
    return Resources::textureStats();
    };
  const auto waitFor=[&](auto done, const char* message) {
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(!done() && std::chrono::steady_clock::now()<deadline) {
      tick();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    require(done(),message);
    };

  auto stats=evaluate();
  require(stats.device.known && stats.device.budget>0 && stats.targetSource==Source::Rule &&
          stats.targetBytes==target(stats.device,Tempest::Texture2d::liveBytes()) && stats.budgetBytes==stats.targetBytes,
          "by default the target must come from the device's budget, and the unrequested textures' budget follow it");
  // the device is asked once a second: the next evaluation within it sees the same usage
  const auto usage=stats.device.usage;
  setenv("OPENGOTHIC_TEXTURE_VRAM_MB","2048",1);
  stats=evaluate();
  require(stats.device.budget==2048*MiB && usage>0 && stats.device.usage==usage && stats.device.unused<=usage &&
          stats.targetBytes==target(stats.device,Tempest::Texture2d::liveBytes()) &&
          stats.targetBytes>=192*MiB && stats.targetBytes<=512*MiB,
          "OPENGOTHIC_TEXTURE_VRAM_MB must pretend the device's budget, its usage staying real");
  unsetenv("OPENGOTHIC_TEXTURE_VRAM_MB");
  Resources::configureTextures({},{},"300");
  tick();
  stats=Resources::textureStats();
  require(stats.targetBytes==300*MiB && stats.targetSource==Source::Setting && stats.budgetBytes==300*MiB,
          "a number in the setting must fix the target");
  setenv("OPENGOTHIC_TEXTURE_TARGET_MB","256",1);
  setenv("OPENGOTHIC_TEXTURE_BUDGET_MB","100",1);
  Resources::configureTextures({},{},"300");
  tick();
  stats=Resources::textureStats();
  require(stats.targetBytes==256*MiB && stats.targetSource==Source::Environment && stats.budgetBytes==100*MiB,
          "the environment must be stronger than the setting, and a set budget keep its meaning");
  unsetenv("OPENGOTHIC_TEXTURE_BUDGET_MB");

  // levels: 90 degrees, log2 K 10.125; exponent -1 gives a 2048 texture level 1 without a reserve
  Resources::setTextureView(2048,90);
  setenv("OPENGOTHIC_TEXTURE_TARGET_MB","1024",1);
  Resources::configureTextures();
  std::array<Material,6> measured;
  for(size_t i=0;i<measured.size();++i)
    measured[i]=Material(source("BG_"+std::to_string(i)+".TGA"),true);
  const Material idle0(source("BG_IDLE_0.TGA"),true), idle1(source("BG_IDLE_1.TGA"),true);
  const Material whole(source("BG_WHOLE.TGA"),true), ringOnly(source("BG_RING.TGA"),true), chain(source("BG_CHAIN.TGA"),true);
  const auto* pinned=Resources::loadTexture("BG_PINNED.TGA",false,true);
  require(pinned!=nullptr && pinned->w()==512 && measured[0].tex->w()==2048 && idle0.tex->w()==512,
          "the budget fixture must load whole");
  using Need=Resources::TextureNeed;
  const auto stat=Resources::TextureCategory::Static;
  const std::array<uint32_t,6> rings={0,0,1,1,3,3};
  for(size_t i=0;i<measured.size();++i)
    Resources::textureDemand(measured[i],stat,true,Need{rings[i],-1});
  Resources::textureDemand(whole,stat,true);
  Resources::textureDemand(ringOnly,stat,true,Need{4,TextureLevels::NoExponent});
  Resources::textureDemand(chain,stat,true,Need{0,TextureScreenDemand::WholeChain});
  const auto chainOf=[](uint32_t level) { return TextureLevels::chainBytes(Tempest::TextureFormat::DXT5,2048,2048,12,level); };
  const auto sides=[&](std::array<int,6> expected) {
    for(size_t i=0;i<measured.size();++i)
      if(measured[i].tex->w()!=expected[i])
        return false;
    return true;
    };
  stats=evaluate();
  require(stats.stage==0 && stats.stageBytes0==stats.stageBytes,"within the target the stage must stay 0");
  std::this_thread::sleep_for(std::chrono::milliseconds(5100));
  tick();
  require(Resources::textureStats().demoted==0 && sides({2048,2048,2048,2048,2048,2048}),
          "below the target nothing may be lowered");

  // Above the target, with D(0) within it: first the unrequested textures leave, then the lowering starts.
  stats=Resources::textureStats();
  const auto targetMb=std::to_string(stats.stageBytes0/MiB+2);
  setenv("OPENGOTHIC_TEXTURE_TARGET_MB",targetMb.c_str(),1);
  Resources::configureTextures();
  tick();
  const auto first=Resources::textureStats();
  require(first.evicted==stats.evicted+2 && first.demoted==0 && first.loadsInFlight==0 && first.stage==0 &&
          idle0.tex->w()==1 && idle1.tex->w()==1,
          "above the target the unrequested textures must leave before any lowering starts");
  tick();
  require(Resources::textureStats().loadsInFlight==1,"with the unrequested gone, the lowering must start");
  // An evaluation every frame while the lowering loads run: their payloads, live from the worker's upload on, are not
  // in D(0); only the 1x1 placeholder of the evicted ones may come in.
  const auto sum0=first.stageBytes0;
  waitFor([&] {
    Resources::configureTextures();
    const auto now=Resources::textureStats();
    require(now.stageBytes0>=sum0 && now.stageBytes0<=sum0+1024,"a payload of a load in flight must not count in D(s)");
    return now.demoted==6;
    },"textures above their wanted level must be lowered to it");
  stats=evaluate();
  require(sides({1024,1024,1024,1024,1024,1024}) && stats.stage==0 && stats.stageBytes0<=stats.targetBytes,
          "when lowering is enough, the stage must stay 0");

  // A real owner measured on screen: a 10 m triangle with a third of a UV unit, 1/30 UV units per metre. Its
  // exponent -1 holds from 60 m on, and moving away up to 150 m; its distance class goes 0 -> 2 at 62 -> 146 m with
  // that same exponent, so only its class can register it again.
  const Material classMat(source("BG_CLASS.TGA"),true);
  StaticMesh classMesh(classMat,{{{0,0,0},{0,0,1},{0,0},0xffffffffu},{{1000,0,0},{0,0,1},{1.f/3.f,0},0xffffffffu},
                                 {{0,1000,0},{0,0,1},{0,1.f/3.f},0xffffffffu}},{0,1,2});
  const Tempest::Vec3 hero{0,0,1000000};
  const auto look=[&](float metres) {
    residency.update(hero,{-metres*100.f,50,0},1000);
    tick();
    };
  look(62);
  auto classOwner=visuals.get(classMesh,classMat,0,classMesh.sub[0].iboLength,true);
  classOwner.setObjMatrix(Tempest::Matrix4x4::mkIdentity());
  look(146);
  stats=evaluate();

  // D(0) above the target: the stage, deeper at once, the shallowest whose exact sum fits. The two ring-3 textures
  // and the owner of class 2 are one level coarser.
  const auto d2=stats.stageBytes0-3*chainOf(1)+3*chainOf(2);
  const auto stageMb=std::to_string((d2+MiB-1)/MiB);
  setenv("OPENGOTHIC_TEXTURE_TARGET_MB",stageMb.c_str(),1);
  stats=evaluate();
  require(stats.stage==2 && stats.stageBytes==d2 && stats.stageBytes0>stats.targetBytes,
          "the stage must be the shallowest whose exact sum fits the target: one level coarser from ring 2");
  std::this_thread::sleep_for(std::chrono::milliseconds(5100));
  waitFor([&] { return sides({1024,1024,1024,1024,512,512}) && classMat.tex->w()==512; },
          "the stage's level must be the wanted level of the lowering");
  // Back to 62 m with the same exponent: class 0, so the stage leaves it at level 1.
  look(62);
  waitFor([&] { return classMat.tex->w()==1024; },"a distance class change alone must change the stage's level");

  // The registered class of a texture's nearest owner, from the texture dump.
  const auto ringOf=[&](std::string_view file) {
    const auto path=fixture.root/"budget-dump.csv";
    Resources::dumpTextures(path);
    std::ifstream in(path);
    std::string line;
    size_t ringColumn=0;
    while(std::getline(in,line)) {
      if(line.empty() || line[0]=='#')
        continue;
      std::vector<std::string> cells;
      for(size_t at=0;;) {
        const auto next=line.find(',',at);
        cells.push_back(line.substr(at,next==std::string::npos ? std::string::npos : next-at));
        if(next==std::string::npos)
          break;
        at=next+1;
        }
      if(ringColumn==0) {
        ringColumn=size_t(std::find(cells.begin(),cells.end(),"ring_min")-cells.begin());
        continue;
        }
      if(cells.size()>ringColumn && cells[1]==file && !cells[ringColumn].empty())
        return std::stoi(cells[ringColumn]);
      }
    return -1;
    };
  // Measuring keeps stage 0: an owner placed 140 m away (class 2, exponent -2 from 120 m on) and then seen from 122 m
  // is not due by its exponent, and its class is not brought up to date. Entering a deep stage, the sweep evaluates it
  // within the view queue's budget: class 1.
  setenv("OPENGOTHIC_TEXTURE_TARGET_MB","0",1);
  require(evaluate().stage==0,"a target of 0 must keep stage 0");
  look(140);
  const Material sweepMat(source("BG_SWEEP.TGA"),true);
  StaticMesh sweepMesh(sweepMat,{{{0,0,0},{0,0,1},{0,0},0xffffffffu},{{1000,0,0},{0,0,1},{1.f/3.f,0},0xffffffffu},
                                 {{0,1000,0},{0,0,1},{0,1.f/3.f},0xffffffffu}},{0,1,2});
  auto sweepOwner=visuals.get(sweepMesh,sweepMat,0,sweepMesh.sub[0].iboLength,true);
  sweepOwner.setObjMatrix(Tempest::Matrix4x4::mkIdentity());
  require(ringOf("bg-sweep.dds")==2,"an owner placed 140 m away must register class 2");
  look(122);
  look(122);
  require(ringOf("bg-sweep.dds")==2,"below stage 2 a class border crossed within the exponent's margin must not re-evaluate it");
  setenv("OPENGOTHIC_TEXTURE_TARGET_MB","1",1);
  require(evaluate().stage>=2,"a target of 1 MiB must take a deep stage");
  waitFor([&] { look(122); return ringOf("bg-sweep.dds")==1; },"entering stage 2 the sweep must bring its class up to date");

  // Nothing fits: the deepest stage, every measured texture at its lowest level; the rest keep their levels.
  setenv("OPENGOTHIC_TEXTURE_TARGET_MB","1",1);
  stats=evaluate();
  require(stats.stage==13 && stats.stageDeepest==13,"when nothing fits the stage must be the deepest: ring 0 at stage 13");
  // with no reserve every measured texture stands at level 1 on stage 0: this is D(1) with the default reserve
  const auto sum1=stats.stageBytes0;
  std::this_thread::sleep_for(std::chrono::milliseconds(5100));
  waitFor([&] { return sides({64,64,64,64,64,64}) && ringOnly.tex->w()==128; },
          "the deepest stage must bring every measured texture to its lowest level");
  require(whole.tex->w()==512 && chain.tex->w()==512 && pinned->w()==512 && ringOnly.tex->w()==128,
          "always whole, pinned, whole-chain owners and the ring rule must never be coarsened");
  require(Resources::textureStats().shortfallCount==0,"the stage's level must be the level the shortfall counts against");

  // The default reserve of one level and a target between D(1) and D(0): the completion before a world's first frame
  // picks stage 1 itself, and stage 1 drops the reserve, so it loads level 1, not the whole chain.
  unsetenv("OPENGOTHIC_TEXTURE_MIP_RESERVE");
  const auto reserveMb=std::to_string(sum1/MiB+1);
  setenv("OPENGOTHIC_TEXTURE_TARGET_MB",reserveMb.c_str(),1);
  Resources::configureTextures();
  Resources::finishTextureLoading();
  stats=Resources::textureStats();
  require(stats.stage==1 && stats.stageBytes==sum1 && stats.stageBytes0>stats.targetBytes &&
          sides({1024,1024,1024,1024,1024,1024}),
          "the world-load completion must choose the stage first, and stage 1 must drop the reserve");

  setenv("OPENGOTHIC_TEXTURE_TARGET_MB","0",1);
  stats=evaluate();
  require(stats.stage==0,"a target of 0 measures every texture at its wanted level, on stage 0");
  setenv("OPENGOTHIC_TEXTURE_TARGET_MB","1",1);
  require(evaluate().stage==13,"the stage must come back with a target");
  setenv("OPENGOTHIC_TEXTURE_SCREEN_MIPS","0",1);
  stats=evaluate();
  require(stats.stage==0 && stats.stageSeconds[2]>0 && stats.stageSeconds[13]>0 && stats.stageSeconds[1]>0,
          "the ring rule must stay on stage 0, and every stage keep its own seconds");
  classOwner=VisualObjects::Item();
  sweepOwner=VisualObjects::Item();
  for(size_t i=0;i<measured.size();++i)
    Resources::textureDemand(measured[i],stat,false,Need{rings[i],-1});
  Resources::textureDemand(whole,stat,false);
  Resources::textureDemand(ringOnly,stat,false,Need{4,TextureLevels::NoExponent});
  Resources::textureDemand(chain,stat,false,Need{0,TextureScreenDemand::WholeChain});
  device.waitIdle();
  }
  unsetenv("OPENGOTHIC_TEXTURE_MIPS");
  unsetenv("OPENGOTHIC_TEXTURE_FAR_SKIP_MIPS");
  unsetenv("OPENGOTHIC_TEXTURE_TARGET_MB");
  unsetenv("OPENGOTHIC_TEXTURE_SCREEN_MIPS");
  unsetenv("OPENGOTHIC_TEXTURE_MIP_RESERVE");
  }

int main(int argc, char** argv) {
  try {
    ddsMipTails();
    textureLevelMaths();
    textureFlowMaths();
    textureBudgetMaths();
    if(argc==2 && std::string_view(argv[1])=="--dds-mip-tail") {
      std::cout << "{\"test\":\"dds_mip_tail\",\"passed\":true,\"assertions\":" << assertions << "}\n";
      return 0;
      }
    variants();
    Fixture fixture;
    CommandLine commandLine(0,nullptr);
    Tempest::VulkanApi api;
    Tempest::Device device(api);
    replacements(device,fixture);
    census(fixture);
    emptyTable(device,fixture);
    projectionRecords(device,fixture);
    textureResidency(device,fixture);
    textureMipResidency(device,fixture);
    textureScreenLevels(device,fixture);
    textureHorizonLevels(device,fixture);
    textureOwnerLevels(device,fixture);
    textureBudgetLevels(device,fixture);
    device.waitIdle();
    std::cout << "{\"test\":\"material_texture\",\"passed\":true,\"assertions\":" << assertions << "}\n";
    return 0;
    }
  catch(const std::exception& error) {
    std::cerr << "material texture test failed: " << error.what() << '\n';
    return 1;
    }
  }

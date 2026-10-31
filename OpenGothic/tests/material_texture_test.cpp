#include "commandline.h"
#include "graphics/material.h"
#include "graphics/mesh/skeleton.h"
#include "graphics/mesh/submesh/packedmesh.h"
#include "graphics/meshobjects.h"
#include "graphics/sceneglobals.h"
#include "graphics/shaders.h"
#include "resources.h"

#include <Tempest/Device>
#include <Tempest/MemReader>
#include <Tempest/VulkanApi>
#include <rapidjson/document.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
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

// One valid compressed 4x4 block. The originals deliberately have the opposite
// format to their replacements, so consulting an original changes the result.
void ddsFile(const std::filesystem::path& path, bool alpha) {
  std::array<uint8_t,144> bytes{};
  auto word = [&](size_t at, uint32_t value) {
    for(size_t i=0;i<4;++i)
      bytes[at*4+i] = uint8_t(value >> (i*8));
    };
  word(0,0x20534444); // DDS
  word(1,124);
  word(2,0x81007);
  word(3,4);
  word(4,4);
  word(5,alpha ? 16 : 8);
  word(7,1);
  word(19,32);
  word(20,4);
  word(21,alpha ? 0x35545844 : 0x31545844); // DXT5 / DXT1
  word(27,0x1000);
  if(alpha)
    bytes[128] = 255;
  std::ofstream out(path, std::ios::binary);
  out.write(reinterpret_cast<const char*>(bytes.data()),alpha ? 144 : 136);
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
      ddsFile(root/"_work"/name,true);
    for(const auto* name:{"NW_CITY_WOOD_01.TGA","NW_HARBOUR_WOODCRATE_01.TGA",
                         "NW_HARBOUR_WOODCRATE_02.TGA","HUM_BODY_V2_C3.TGA",
                         "HUM_BODY_V4_C5.TGA","UNLISTED.TGA"})
      ddsFile(root/"_work"/name,false);
    textFile(root/"materials/materials.json",R"({
      "NW_CITY_ROPE_01.TGA":{"albedo":"solid.dds"},
      "NW_CITY_WOOD_01.TGA":{"albedo":"alpha.dds"},
      "NW_HARBOUR_WOODCRATE_01.TGA":{"albedo":"alpha.dds"},
      "NW_HARBOUR_WOODCRATE_02.TGA":{"albedo":"alpha.dds"},
      "HUM_BODY_V0_C0.TGA":{"albedo":"solid.dds"},
      "HUM_BODY_V2_C3.TGA":{"albedo":"alpha.dds"}
    })");
    setenv("OPENGOTHIC_CONTENT",root.c_str(),1);
    setenv("OPENGOTHIC_TEXTURES","",1);
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
  require(data["replacements"].Size()==6,"all four city textures, base and selected variant must be counted");
  for(const auto& entry:data["replacements"].GetArray()) {
    const std::string_view replacement(entry["replacement"].GetString());
    require(replacement=="materials/solid.dds" || replacement=="materials/alpha.dds",
            "direct albedo substitutions must use the ordinary material census path");
    }
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
}

int main(int argc, char** argv) {
  try {
    ddsMipTails();
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
    device.waitIdle();
    std::cout << "{\"test\":\"material_texture\",\"passed\":true,\"assertions\":" << assertions << "}\n";
    return 0;
    }
  catch(const std::exception& error) {
    std::cerr << "material texture test failed: " << error.what() << '\n';
    return 1;
    }
  }

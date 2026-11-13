#include "worldbase.h"

#include "worldchunksruntime.h"
#include "world/voblayer.h"
#include "loadprobe.h"
#include "resources.h"
#include "contentcensus.h"

#include <Tempest/Log>

#include <rapidjson/document.h>
#include <algorithm>
#include <fstream>
#include <chrono>
#include <cctype>
#include <stdexcept>
#include <cstdlib>

using Tempest::Log;

WorldBaseResult tryWorldData(WorldData::Data& world, asset::WorldMesh& worldMesh,
                            std::string_view name, const std::filesystem::path& dir,
                            const std::filesystem::path& overrideDir,
                            const std::vector<std::pair<std::string,uintmax_t>>& baseArchives,
                            const std::map<std::string,std::filesystem::path>* ownMeshes,
                            ChunkTable* chunks) {
  WorldBaseResult result;
  const auto started = std::chrono::steady_clock::now();
  try {
    if(dir.empty())
      throw std::runtime_error("base disabled");
    std::ifstream file(dir/"world.json",std::ios::binary);
    if(!file)
      throw std::runtime_error("world.json missing");
    const std::string text{std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};
    rapidjson::Document table;
    table.Parse<rapidjson::kParseValidateEncodingFlag>(text.data(),text.size());
    if(table.HasParseError() || !table.IsObject() ||
       !table.HasMember("archives") || !table["archives"].IsArray())
      throw std::runtime_error("invalid world archive index");
    std::vector<std::pair<std::string,uintmax_t>> archives;
    for(const auto& archive:table["archives"].GetArray()) {
      if(!archive.IsObject() || !archive.HasMember("name") || !archive["name"].IsString() ||
         !archive.HasMember("size") || !archive["size"].IsUint64())
        throw std::runtime_error("invalid archive identity");
      archives.emplace_back(archive["name"].GetString(),archive["size"].GetUint64());
      }
    std::sort(archives.begin(),archives.end());
    if(archives!=baseArchives)
      throw std::runtime_error("selected base archives differ from world.json");
    for(const char* required:{"vobs.json","waynet.json","sectors.json","bsp.bin","rooms.bin"})
      if(!std::filesystem::is_regular_file(dir/required))
        throw std::runtime_error(std::string(required)+" missing");
    const auto manifest = readWorldChunkManifest(dir);
    for(const auto& chunk:manifest.chunks)
      if(!std::filesystem::is_regular_file(dir/chunk))
        throw std::runtime_error(chunk+" missing from base");
    const auto separator = name.find_last_of("/\\");
    std::string key(separator==std::string_view::npos ? name : name.substr(separator+1));
    for(auto& c:key)
      c = char(std::toupper(static_cast<unsigned char>(c)));
    if(manifest.source.world!=key)
      throw std::runtime_error("ZEN source world mismatch");
    if(LoadProbe::enabled())
      LoadProbe::mark("world_index");
    auto probeT = LoadProbe::now();
    auto data = WorldData::read(dir,overrideDir);
    if(LoadProbe::enabled())
      LoadProbe::mark("world_data_read",LoadProbe::msSince(probeT),
                      "vobs="+std::to_string(data.vobs.size())+" waypoints="+std::to_string(data.waynet ? data.waynet->points.size() : 0));
    if(data.source.world!=manifest.source.world || data.source.size!=manifest.source.size ||
       data.source.crc32!=manifest.source.crc32)
      throw std::runtime_error("world data source differs from world.json");
    if(!data.waynet)
      throw std::runtime_error("world waynet missing");
    probeT = LoadProbe::now();
    VobLayer::apply(data,dir,overrideDir,ownMeshes);
    if(LoadProbe::enabled())
      LoadProbe::mark("voblayer",LoadProbe::msSince(probeT));
    ChunkTable chunkTable;
    probeT = LoadProbe::now();
    auto mesh = readWorldChunks(dir,overrideDir,chunks==nullptr ? nullptr : &chunkTable);
    if(LoadProbe::enabled())
      LoadProbe::mark("mesh_chunks_read",LoadProbe::msSince(probeT),
                      "chunks="+std::to_string(manifest.chunks.size())+" tris="+std::to_string(mesh.polygons.material_indices.size()));
    mesh.name = data.name;
    mesh.bbox = data.bbox;
    result.triangles = mesh.polygons.material_indices.size();
    result.chunks = manifest.chunks.size();
    worldMesh = std::move(mesh);
    world = std::move(data);
    if(chunks!=nullptr)
      *chunks=std::move(chunkTable);
    result.replaced = true;
    }
  catch(const std::exception& error) {
    result.error = error.what();
    }
  result.readMs = std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
  return result;
  }

void loadWorld(WorldData::Data& world, asset::WorldMesh& worldMesh, std::string_view name, ChunkTable* chunks) {
  const auto dir = Resources::worldBaseDir(name);
  const auto ownMeshes = Resources::meshReplacementFiles();
  const auto result = tryWorldData(world,worldMesh,name,dir,Resources::worldOverrideDir(name),Resources::baseArchiveFiles(),&ownMeshes,chunks);
  if(result.replaced) {
    if(Resources::contentCensus())
      Resources::contentCensus()->replaced(name,(Resources::worldContentDir("base",name)/"world.json").generic_string());
    Log::i("world base: \"",dir.string(),"\", ",result.triangles," triangles, ",
           result.chunks," chunks, ",result.readMs," ms");
    return;
    }
  if(Resources::contentCensus()) Resources::contentCensus()->missing(name,result.error);
  throw BaseStore::Error("world "+std::string(name)+": "+result.error);
  }

namespace {
bool readVobTreeLayer(Resources::VobTree& bundle, std::string_view filename) {
  bool overridden=false;
  const auto content=Resources::contentDir();
  const auto* layers=std::getenv("OPENGOTHIC_VOB_LAYERS");
  if(!content.empty() && (layers==nullptr || std::string_view(layers)!="0")) {
    auto file=content/"vobtrees.d";
    try {
      const auto separator=filename.find_last_of("/\\");
      auto name=std::filesystem::path(std::string(filename.substr(separator==std::string_view::npos ? 0 : separator+1)))
                  .stem().string()+".json";
      for(auto& c:name) c=char(std::toupper(uint8_t(c)));
      if(std::filesystem::is_directory(file))
        for(const auto& entry:std::filesystem::directory_iterator(file)) {
          auto candidate=entry.path().filename().string();
          for(auto& c:candidate) c=char(std::toupper(uint8_t(c)));
          if(candidate!=name || !entry.is_regular_file()) continue;
          file=entry.path();
          const auto ownMeshes=Resources::meshReplacementFiles();
          const auto result=VobLayer::readTree(bundle,Resources::baseDir(),file,content,&ownMeshes);
          overridden=result.errors.empty();
          if(!overridden) {
            std::string errors;
            for(const auto& error:result.errors) {
              if(!errors.empty()) errors+="; ";
              errors+=error;
              }
            Log::e("vob tree: skipped ",errors);
            }
          break;
          }
      }
    catch(const std::exception& error) {
      Log::e("vob tree: skipped ",file.string(),": ",error.what());
      }
    }
  return overridden;
  }
}

const std::vector<std::shared_ptr<asset::VirtualObject>>* loadVobBundle(std::string_view name) {
  return Resources::loadVobBundle(name,readVobTreeLayer);
  }

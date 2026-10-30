#include "worldbase.h"

#include "worldchunks.h"
#include "world/voblayer.h"

#include <rapidjson/document.h>
#include <algorithm>
#include <fstream>
#include <chrono>
#include <cctype>
#include <stdexcept>

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
    auto data = WorldData::read(dir,overrideDir);
    if(data.source.world!=manifest.source.world || data.source.size!=manifest.source.size ||
       data.source.crc32!=manifest.source.crc32)
      throw std::runtime_error("world data source differs from world.json");
    if(!data.waynet)
      throw std::runtime_error("world waynet missing");
    VobLayer::apply(data,dir,overrideDir,ownMeshes);
    ChunkTable table;
    auto mesh = readWorldChunks(dir,overrideDir,chunks==nullptr ? nullptr : &table);
    mesh.name = data.name;
    mesh.bbox = data.bbox;
    result.triangles = mesh.polygons.material_indices.size();
    result.chunks = manifest.chunks.size();
    worldMesh = std::move(mesh);
    world = std::move(data);
    if(chunks!=nullptr)
      *chunks=std::move(table);
    result.replaced = true;
    }
  catch(const std::exception& error) {
    result.error = error.what();
    }
  result.readMs = std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
  return result;
  }

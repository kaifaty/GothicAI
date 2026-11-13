#include "worldchunkmanifest.h"

#include <charconv>
#include <cstring>
#include <set>
#include <stdexcept>
#include <utility>

#include "gothicglb.h"

namespace {

using namespace gothicglb;

[[noreturn]] void fail(const std::string& reason) {
  throw std::runtime_error("world chunks: " + reason);
  }

constexpr int         formatVersion    = 2;
constexpr const char* manifestName     = "world.json";
constexpr const char* worldFormat      = "gothic-world";
constexpr const char* unreferencedName = "unreferenced.bin";
constexpr char        unreferencedMagic[4] = {'G','W','U','R'};

std::string cellName(ChunkId id) {
  return std::to_string(id.x) + "," + std::to_string(id.z);
  }

std::string meshName(const ChunkMeshRef& mesh) {
  return mesh.role==ChunkMeshRole::Draw ? "draw level " + std::to_string(mesh.level) : "collision";
  }

const Json* find(const Json& object, const char* name) {
  const auto at = object.FindMember(name);
  return at==object.MemberEnd() ? nullptr : &at->value;
  }

std::string chunkPath(std::string file) {
  const std::filesystem::path rel(file);
  if(rel.is_absolute() || rel.empty())
    fail("chunk path must be relative: " + file);
  for(const auto& part:rel)
    if(part=="..")
      fail("chunk path must stay inside the world directory: " + file);
  return file;
  }

ChunkId cellFromStem(const std::string& file) {
  const auto name = std::filesystem::path(file).stem().string();
  const auto separator = name.find('_');
  ChunkId id;
  if(separator!=std::string::npos) {
    const auto x = std::from_chars(name.data(),name.data()+separator,id.x);
    const auto z = std::from_chars(name.data()+separator+1,name.data()+name.size(),id.z);
    if(x.ec==std::errc{} && x.ptr==name.data()+separator && z.ec==std::errc{} && z.ptr==name.data()+name.size())
      return id;
    }
  fail("chunk file name is not <x>_<z>: " + file);
  }

ChunkCell stringEntry(const Json& value) {
  auto file = chunkPath(jsonToString(value,"chunks"));
  ChunkCell cell;
  cell.id = cellFromStem(file);
  cell.meshes.push_back({std::move(file),ChunkMeshRole::Draw,0});
  return cell;
  }

ChunkMeshRef meshEntry(const Json& value, const std::string& where) {
  if(!value.IsObject())
    fail(where + ": a mesh must be an object");
  const auto* file = find(value,"file");
  if(file==nullptr || !file->IsString())
    fail(where + ": a mesh needs a file string");
  ChunkMeshRef mesh;
  mesh.file = chunkPath(jsonToString(*file,"file"));
  const auto* role  = find(value,"role");
  const auto* level = find(value,"level");
  const auto  name  = role!=nullptr && role->IsString() ? jsonToString(*role,"role") : std::string();
  if(name=="draw") {
    mesh.role = ChunkMeshRole::Draw;
    if(level!=nullptr) {
      if(!level->IsInt() || level->GetInt()<0)
        fail(where + ": level of " + mesh.file + " must be a non-negative integer");
      mesh.level = level->GetInt();
      }
    }
  else if(name=="collision") {
    mesh.role = ChunkMeshRole::Collision;
    if(level!=nullptr)
      fail(where + ": collision mesh " + mesh.file + " has a level");
    }
  else {
    fail(where + ": unknown role '" + name + "' of " + mesh.file + " (draw or collision)");
    }
  return mesh;
  }

ChunkCell objectEntry(const Json& value, rapidjson::SizeType entry) {
  const auto where = "chunks entry " + std::to_string(entry);
  const auto* id = find(value,"cell");
  if(id==nullptr || !id->IsArray() || id->Size()!=2 || !(*id)[0].IsInt() || !(*id)[1].IsInt())
    fail(where + ": cell must be two integers [x, z]");
  ChunkCell cell;
  cell.id = {(*id)[0].GetInt(),(*id)[1].GetInt()};
  const auto at = where + ", cell " + cellName(cell.id);
  const auto* meshes = find(value,"meshes");
  if(meshes==nullptr || !meshes->IsArray())
    fail(at + ": meshes must be an array");
  for(const auto& item:meshes->GetArray()) {
    auto mesh = meshEntry(item,at);
    for(const auto& other:cell.meshes)
      if(other.role==mesh.role && other.level==mesh.level)
        fail(at + ": two " + meshName(mesh) + " meshes, " + other.file + " and " + mesh.file);
    cell.meshes.push_back(std::move(mesh));
    }
  (void)cell.drawFile(); // throws without a draw level 0 mesh
  return cell;
  }

}

const std::string& ChunkCell::drawFile() const {
  for(const auto& mesh:meshes)
    if(mesh.role==ChunkMeshRole::Draw && mesh.level==0)
      return mesh.file;
  fail("cell " + cellName(id) + " has no draw level 0 mesh");
  }

WorldCellManifest readWorldChunkManifest(const std::filesystem::path& dir) {
  const auto bytes = readFile(dir/manifestName);
  rapidjson::Document doc;
  doc.Parse<rapidjson::kParseFullPrecisionFlag>(reinterpret_cast<const char*>(bytes.data()),bytes.size());
  if(doc.HasParseError() || !doc.IsObject())
    fail("invalid " + (dir/manifestName).string());
  if(jsonToString(member(doc,"format"),"format")!=worldFormat)
    fail("not a converted world: " + dir.string());
  if(jsonToInt(member(doc,"version"),"version")!=formatVersion)
    fail("unsupported world format version in " + dir.string());
  WorldCellManifest m;
  const auto& s = member(doc,"source");
  m.source.world = jsonToString(member(s,"world"),"world");
  m.source.size  = uint64_t(jsonToInt(member(s,"size"),"size"));
  m.source.crc32 = uint32_t(jsonToInt(member(s,"crc32"),"crc32"));
  m.chunkSize    = int32_t(jsonToInt(member(doc,"chunk_size_cm"),"chunk_size_cm"));
  m.triangles    = uint64_t(jsonToInt(member(doc,"triangles"),"triangles"));
  const auto& c  = member(doc,"chunks");
  if(!c.IsArray())
    fail("chunks must be an array");
  std::set<std::pair<int32_t,int32_t>> seen;
  for(rapidjson::SizeType i=0; i<c.Size(); ++i) {
    ChunkCell cell;
    if(c[i].IsString())
      cell = stringEntry(c[i]);
    else if(c[i].IsObject())
      cell = objectEntry(c[i],i);
    else
      fail("chunks entry " + std::to_string(i) + " must be a string or an object");
    if(!seen.emplace(cell.id.x,cell.id.z).second)
      fail("cell " + cellName(cell.id) + " is listed twice (chunks entry " + std::to_string(i) + ")");
    m.chunks.push_back(cell.drawFile());
    m.cells.push_back(std::move(cell));
    }
  m.vertices = uint64_t(jsonToInt(member(doc,"vertices"),"vertices"));
  m.features = uint64_t(jsonToInt(member(doc,"features"),"features"));
  if(jsonToString(member(doc,"unreferenced"),"unreferenced")!=unreferencedName)
    fail("unexpected unreferenced file name");
  const auto& mats = member(doc,"materials");
  if(!mats.IsArray())
    fail("materials must be an array");
  for(rapidjson::SizeType i=0; i<mats.Size(); ++i) {
    if(jsonToInt(member(mats[i],"index"),"index")!=int64_t(i))
      fail("materials must be listed in source order");
    m.materials.push_back(materialFromJson(mats[i]));
    }
  return m;
  }

void readWorldUnreferenced(const std::filesystem::path& dir, std::vector<asset::Vec3>& vertices,
                           std::vector<asset::VertexFeature>& features,
                           std::vector<char>& vertexSet, std::vector<char>& featureSet) {
  const auto bin = readFile(dir/unreferencedName);
  size_t o = 0;
  auto u32 = [&]() {
    if(o+4>bin.size())
      fail("unreferenced.bin is truncated");
    const uint32_t v = uint32_t(bin[o]) | (uint32_t(bin[o+1])<<8) | (uint32_t(bin[o+2])<<16) | (uint32_t(bin[o+3])<<24);
    o += 4;
    return v;
    };
  auto f32 = [&]() {
    const uint32_t v = u32();
    float f;
    std::memcpy(&f,&v,sizeof(f));
    return f;
    };
  if(bin.size()<4 || std::memcmp(bin.data(),unreferencedMagic,4)!=0)
    fail("unreferenced.bin has a wrong header");
  o = 4;
  if(u32()!=uint32_t(formatVersion))
    fail("unreferenced.bin has another version");
  const uint32_t nv = u32(), nf = u32();
  for(uint32_t i=0; i<nv; ++i) {
    const auto id = u32();
    if(id>=vertices.size())
      fail("unreferenced vertex out of range");
    const float x = f32(), y = f32(), z = f32();
    vertices[id] = {x,y,z};
    vertexSet[id] = 1;
    }
  for(uint32_t i=0; i<nf; ++i) {
    const auto id = u32();
    if(id>=features.size())
      fail("unreferenced feature out of range");
    asset::VertexFeature f;
    f.texture.x = f32();
    f.texture.y = f32();
    f.light     = u32();
    f.normal.x  = f32();
    f.normal.y  = f32();
    f.normal.z  = f32();
    features[id] = f;
    featureSet[id] = 1;
    }
  if(o!=bin.size())
    fail("unreferenced.bin has trailing bytes");
  }

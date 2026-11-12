#include "resources.h"
#include "resources_internal.h"
#include "commandline.h"
#include "contentcensus.h"
#include "worldbase.h"
#include "graphics/mesh/protomesh.h"
#include "graphics/mesh/renderrig.h"
#include "utils/fileext.h"
#include "utils/userhome.h"

#include <Tempest/Log>
#include <Tempest/TextCodec>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <stdexcept>

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
#include <rapidjson/document.h>
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

using namespace Tempest;
using ResourcesInternal::modelKey;

namespace ResourcesInternal {
std::string modelKey(std::string_view name) {
  const auto separator = name.find_last_of("/\\");
  std::string key(separator==std::string_view::npos ? name : name.substr(separator+1));
  for(auto& c:key)
    c = char(std::toupper(static_cast<unsigned char>(c)));
  return key;
  }
}

namespace {
bool contentPath(const std::string& path, const char* ext) {
  const std::filesystem::path relative(path);
  if(path.empty() || path.find('\0')!=std::string::npos || relative.is_absolute() ||
     relative.has_root_name() || !FileExt::hasExt(path,ext))
    return false;
  for(const auto& part:relative)
    if(part=="..")
      return false;
  return true;
  }

bool contentGlbPath(const std::string& path) {
  return contentPath(path,"GLB");
  }

std::string wayHeightFileStamp(const std::filesystem::path& file) {
  try {
    namespace fs = std::filesystem;
    if(!fs::exists(file)) return "missing";
    if(!fs::is_regular_file(file)) return "other";
    return std::to_string(fs::file_size(file))+":"+
           std::to_string(std::chrono::duration_cast<std::chrono::nanoseconds>(
                            fs::last_write_time(file).time_since_epoch()).count());
    }
  catch(const std::filesystem::filesystem_error&) {
    return {};
    }
  }


std::filesystem::path worldContentDir(const std::filesystem::path& root, std::string_view world) {
  const auto separator = world.find_last_of("/\\");
  std::string name(separator==std::string_view::npos ? world : world.substr(separator+1));
  name = std::filesystem::path(name).stem().string();
  for(auto& c:name)
    c = char(std::toupper(static_cast<unsigned char>(c)));
  return root/"worlds"/name;
  }
}

void Resources::loadBase() {
  if(!inst->automaticBase.path.empty()) return;
  try {
    const auto gothic=std::filesystem::path(CommandLine::inst().rootPath());
    BaseImport::Files archives;
    const auto* explicitBase=std::getenv("OPENGOTHIC_BASE");
    const auto* fingerprint=std::getenv("OPENGOTHIC_BASE_FINGERPRINT");
    if((explicitBase && *explicitBase) || !fingerprint || !*fingerprint)
      for(const auto& archive:BaseImport::runtimeArchives(gothic))
        if(!archive.mod) archives.emplace_back(archive.name,archive.size);
    inst->automaticBase=BaseStore::resolve(gothic,archives,
                                         [](std::string_view message) {
      Log::i(message);
      });
    inst->indexBaseContent();
    BaseImport::Files identityLoose;
    const auto identity=BaseImport::readIdentity(inst->automaticBase.path);
    inst->baseFingerprint=identity.fingerprint;
    rapidjson::Document metadata;
    metadata.Parse(identity.json.data(),identity.json.size());
    inst->baseArchives.clear();
    for(const auto& archive:metadata["archives"].GetArray())
      inst->baseArchives.emplace_back(archive["name"].GetString(),archive["size"].GetUint64());
    std::sort(inst->baseArchives.begin(),inst->baseArchives.end());
    for(const auto& file:metadata["loose_files"].GetArray())
      identityLoose.emplace_back(file["path"].GetString(),file["size"].GetUint64());
    std::sort(identityLoose.begin(),identityLoose.end());
    const auto& validationArchives=inst->baseArchives;
    const auto validateLoose=[&](const BaseImport::Files& files, std::string_view family) {
      for(const auto& file:files) {
        if(!std::binary_search(identityLoose.begin(),identityLoose.end(),file))
          throw std::runtime_error("loose file differs from "+std::string(family)+" base index: "+file.first);
        }
      };
    inst->baseTextures.validate(validationArchives);
    inst->baseMeshes.validate(validationArchives);
    inst->baseZen.validate(validationArchives);
    validateLoose(inst->baseZen.looseFiles,"zen");
    inst->baseModels.validate(validationArchives);
    validateLoose(inst->baseModels.looseFiles,"models");
    inst->baseFonts.validate(validationArchives);
    validateLoose(inst->baseFonts.looseFiles,"fonts");
    if(inst->basePrograms.archives!=validationArchives)
      throw std::runtime_error("selected base archives differ from program base index: "+inst->basePrograms.dir.string());
    validateLoose(inst->basePrograms.looseFiles,"program");
    Log::i("base: ",inst->basePrograms.programs.size()," programs from \"",inst->basePrograms.dir.string(),"\"");
    if(inst->baseOutputUnits.archives!=validationArchives)
      throw std::runtime_error("selected base archives differ from output units base index: "+inst->baseOutputUnits.dir.string());
    validateLoose(inst->baseOutputUnits.looseFiles,"output units");
    Log::i("base: ",inst->baseOutputUnits.records.size()," output units records from \"",inst->baseOutputUnits.dir.string(),"\"");
    if(inst->baseAudio.archives!=validationArchives)
      throw std::runtime_error("selected base archives differ from audio base index: "+inst->baseAudio.dir.string());
    validateLoose(inst->baseAudio.looseFiles,"audio");
    Log::i("base: ",inst->baseAudio.sounds.size()," sounds from \"",inst->baseAudio.dir.string(),"\"");
    if(inst->baseMusic.archives!=validationArchives)
      throw std::runtime_error("selected base archives differ from music base index: "+inst->baseMusic.dir.string());
    validateLoose(inst->baseMusic.looseFiles,"music");
    Log::i("base: ",inst->baseMusic.records.size()," music records from \"",inst->baseMusic.dir.string(),"\"");
    }
  catch(const std::exception& e) { throw BaseStore::Error(e.what()); }
  }

void Resources::initCensus() {
  const char* path = std::getenv("OPENGOTHIC_CENSUS");
  if(path==nullptr || path[0]=='\0')
    return;
  census = std::make_unique<ContentCensus>(path);
  }

void Resources::censusFile(std::u16string_view path) {
  if(inst==nullptr || inst->census==nullptr)
    return;
  const auto source = TextCodec::toUtf8(path);
  inst->census->opened(source,source);
  }

const ProgramData::Index& Resources::baseProgramIndex() {
  return inst->basePrograms;
  }

ContentCensus* Resources::contentCensus() {
  return inst->census.get();
  }

void Resources::censusProfile(std::string_view game, std::string_view outputUnits, int language) {
  if(inst!=nullptr && inst->census!=nullptr)
    inst->census->profile(game,outputUnits,language);
  }

void Resources::loadWorld(WorldData::Data& world, asset::WorldMesh& worldMesh, std::string_view name, ChunkTable* chunks) {
  const auto dir = worldBaseDir(name);
  std::map<std::string,std::filesystem::path> ownMeshes;
  for(const auto& [key,mesh]:inst->meshOverrides)
    ownMeshes.emplace(key,mesh.file);
  const auto result = tryWorldData(world,worldMesh,name,dir,worldOverrideDir(name),inst->baseArchives,&ownMeshes,chunks);
  if(result.replaced) {
    if(inst->census)
      inst->census->replaced(name,(worldContentDir("base",name)/"world.json").generic_string());
    Log::i("world base: \"",dir.string(),"\", ",result.triangles," triangles, ",
           result.chunks," chunks, ",result.readMs," ms");
    return;
    }
  if(inst->census) inst->census->missing(name,result.error);
  throw BaseStore::Error("world "+std::string(name)+": "+result.error);
  }

bool Resources::loadBaseZen(std::string_view name, std::string_view kind, WorldData::ZenFile& out) {
  const auto& index = inst->baseZen;
  const auto separator = name.find_last_of("/\\");
  std::string key(separator==std::string_view::npos ? name : name.substr(separator+1));
  for(auto& c:key)
    c = char(std::toupper(static_cast<unsigned char>(c)));
  const auto found = index.files.find(key);
  try {
    if(found==index.files.end()) throw std::runtime_error("missing from base index");
    auto data = WorldData::readZen(index.dir/found->second);
    if(data.kind!=kind || data.source.world!=key)
      throw std::runtime_error("ZEN kind or source name differs from index");
    if(kind=="cameras")
      for(const auto& camera:data.vobs)
        if(!std::dynamic_pointer_cast<asset::VCutsceneCamera>(camera))
          throw std::runtime_error("invalid dialog camera class");
    out = std::move(data);
    if(inst->census)
      inst->census->replaced(key,"base/zen/"+found->second);
    return true;
    }
  catch(const std::exception& error) {
    Log::e("base: ",key,": ",error.what());
    if(inst->census) inst->census->missing(key,error.what());
    return false;
    }
  }

bool Resources::writeCensus() {
  return inst==nullptr || inst->census==nullptr || inst->census->write();
  }

bool Resources::hasWorld(std::string_view name) {
  const auto dir=worldContentDir({},name);
  return inst->baseWorlds.contains(dir.filename().string());
  }

int Resources::baseLanguage() {
  for(int language=-1;language<8;++language) {
    const bool program=std::any_of(inst->basePrograms.selections.begin(),inst->basePrograms.selections.end(),
                                  [&](const auto& row) {
      return row.language==language && row.request=="GOTHIC.DAT" && row.source &&
             inst->basePrograms.sources.at(*row.source).skipped.empty();
      });
    const bool outputUnits=std::any_of(inst->baseOutputUnits.selections.begin(),inst->baseOutputUnits.selections.end(),
                                      [&](const auto& row) {
      return row.language==language && row.source &&
             inst->baseOutputUnits.sources.at(*row.source).skipped.empty();
      });
    if(program && outputUnits) return language;
    }
  return -1;
  }


std::filesystem::path Resources::baseDir() {
  return inst->automaticBase.path;
  }

std::filesystem::path Resources::worldBaseDir(std::string_view world) {
  const auto root = baseDir();
  return root.empty() ? root : worldContentDir(root,world);
  }

void Resources::indexBaseContent() {
  const auto root = baseDir();
  baseTextures.load(root,"textures");
  baseMeshes.load(root,"meshes");
  baseZen.load(root,"zen");
  baseModels.load(root,"models");
  indexContentModels();
  baseFonts.load(root,"fonts");
  basePrograms = ProgramData::readIndex(root/"programs");
  baseOutputUnits = OutputUnits::readIndex(root/"outputunits");
  baseAudio = AudioData::readIndex(root/"audio");
  baseMusic = MusicData::readIndex(root/"music");
  baseWorlds.clear();
  std::ifstream manifestFile(root/"base-files.json",std::ios::binary);
  const std::string text{std::istreambuf_iterator<char>(manifestFile),std::istreambuf_iterator<char>()};
  rapidjson::Document manifest;
  manifest.Parse(text.data(),text.size());
  if(manifest.HasParseError() || !manifest.IsObject() || !manifest.HasMember("files") || !manifest["files"].IsArray())
    throw std::runtime_error("invalid base file manifest");
  for(const auto& entry:manifest["files"].GetArray()) {
    const std::filesystem::path file(entry["path"].GetString());
    if(file.parent_path().parent_path()=="worlds" && file.filename()=="world.json")
      baseWorlds.emplace(file.parent_path().filename().string());
    }
  }

void Resources::indexContentModels() {
  contentModels.clear();
  renderRigs.reset();
  const auto root = contentDir();
  if(root.empty())
    return;
  const auto folder = root/"models";
  const auto index = folder/"models.json";
  std::ifstream file(index,std::ios::binary);
  if(!file) {
    std::error_code ec;
    if(std::filesystem::exists(index,ec) || ec)
      Log::e("content models: cannot read \"",index.string(),"\"");
    }
  else try {
    const std::string text{std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};
    rapidjson::Document table;
    table.Parse<rapidjson::kParseValidateEncodingFlag>(text.data(),text.size());
    if(table.HasParseError() || !table.IsObject() ||
       !table.HasMember("format") || !table["format"].IsString() ||
       std::string_view(table["format"].GetString(),table["format"].GetStringLength())!="gothic-models" ||
       !table.HasMember("version") || !table["version"].IsUint() || table["version"].GetUint()!=1 ||
       !table.HasMember("files") || !table["files"].IsObject())
      throw std::runtime_error("expected gothic-models index version 1 with files object");
    for(const auto& entry:table["files"].GetObject()) {
      const std::string key(entry.name.GetString(),entry.name.GetStringLength());
      if(key.empty() || key.find_first_of("/\\")!=std::string::npos || key.find('\0')!=std::string::npos ||
         key!=modelKey(key) || !entry.value.IsString()) {
        Log::e("content models: invalid entry \"",key,"\" in \"",index.string(),"\"");
        continue;
        }
      const std::string path(entry.value.GetString(),entry.value.GetStringLength());
      const std::filesystem::path relative(path);
      if(path.empty() || path.find('\0')!=std::string::npos || path.find('\\')!=std::string::npos ||
         relative.is_absolute() || relative.has_root_name() ||
         std::any_of(relative.begin(),relative.end(),[](const auto& part) { return part==".."; })) {
        Log::e("content models: invalid path for \"",key,"\" in \"",index.string(),"\"");
        continue;
        }
      if((FileExt::hasExt(key,"MSB") || FileExt::hasExt(key,"MDS")) && baseModels.files.count(key)==0) {
        Log::e("content models: warning: no base script for overlay \"",(folder/relative).string(),"\" (",key,")");
        continue;
        }
      contentModels.emplace(key,folder/relative);
      }
    }
  catch(const std::exception& error) {
    Log::e("content models: \"",index.string(),"\": ",error.what());
    }
  indexRenderRigs(folder/"render-rigs.json");
  }

// After the content models: a rig's skeleton record may be one of them. Each rig is built at once. A body bound to a
// rig is of the rig's skeleton (its bone indices): with the rigs off (--no-render-rig: only the names of the table's
// meshes are taken, no rig is built) or its rig broken, its content mesh is left out and the base original is drawn.
void Resources::indexRenderRigs(const std::filesystem::path& file) {
  const bool on = CommandLine::inst().doRenderRig();
  auto rigs = std::make_unique<RenderRigIndex>(
      [](std::string_view record, asset::ModelHierarchy& out) { return loadHierarchyData(record,out); },
      [on](const std::string& message) { if(on) Log::e(message); });
  rigs->read(file);
  std::set<std::string> left;
  for(const auto& name:rigs->rigNames()) {
    const RenderRig* rig = on ? rigs->rig(name) : nullptr;
    size_t meshes = 0;
    for(const auto& [mesh,of]:rigs->meshTable())
      meshes += of==name ? 1 : 0;
    if(rig!=nullptr)
      Log::i("render rig ",name,": ",rig->bones()," bones, ",meshes," meshes"); else
      left.insert(name);
    }
  for(const auto& [mesh,of]:rigs->meshTable())
    if(left.count(of)!=0)
      for(const char* ext:{".MDM",".MDL"})
        contentModels.erase(mesh+ext);
  if(!on)
    Log::i("render rig: off");
  else if(rigs->size()>0)
    renderRigs = std::move(rigs);
  }

void Resources::BaseIndex::load(const std::filesystem::path& root, const char* kind) {
  *this = {};
  const auto folder = root/kind;
  const auto resourceKind = std::string_view(kind);
  const char* entries = resourceKind=="zen" || resourceKind=="models" ? "files" : kind;
  std::ifstream file(folder/(std::string(kind)+".json"),std::ios::binary);
  if(!file)
    throw std::runtime_error("base index missing: "+(folder/(std::string(kind)+".json")).string());
  try {
    const std::string text{std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};
    rapidjson::Document table;
    table.Parse<rapidjson::kParseValidateEncodingFlag>(text.data(),text.size());
    if(table.HasParseError() || !table.IsObject() ||
       !table.HasMember("archives") || !table["archives"].IsArray() ||
       !table.HasMember(entries) || !table[entries].IsObject())
      throw std::runtime_error("invalid index");
    if(resourceKind=="models" &&
       (!table.HasMember("format") || !table["format"].IsString() ||
        std::string_view(table["format"].GetString(),table["format"].GetStringLength())!="gothic-models" ||
        !table.HasMember("version") || !table["version"].IsUint() || table["version"].GetUint()!=1))
      throw std::runtime_error("unsupported model index format or version");
    if(resourceKind=="fonts" &&
       (!table.HasMember("format") || !table["format"].IsString() ||
        std::string_view(table["format"].GetString(),table["format"].GetStringLength())!="gothic-fonts" ||
        !table.HasMember("version") || !table["version"].IsUint() || table["version"].GetUint()!=1))
      throw std::runtime_error("unsupported font index format or version");
    for(const auto& archive:table["archives"].GetArray()) {
      if(!archive.IsObject() || !archive.HasMember("name") || !archive["name"].IsString() ||
         !archive.HasMember("size") || !archive["size"].IsUint64())
        throw std::runtime_error("invalid archive identity");
      archives.emplace_back(archive["name"].GetString(),archive["size"].GetUint64());
      }
    std::sort(archives.begin(),archives.end());
    if((resourceKind=="zen" || resourceKind=="models" || resourceKind=="fonts") && table.HasMember("loose_files")) {
      if(!table["loose_files"].IsArray())
        throw std::runtime_error("invalid loose file list");
      for(const auto& entry:table["loose_files"].GetArray()) {
        if(!entry.IsObject() || !entry.HasMember("path") || !entry["path"].IsString() ||
           !entry.HasMember("size") || !entry["size"].IsUint64())
          throw std::runtime_error("invalid loose file identity");
        const std::string name(entry["path"].GetString(),entry["path"].GetStringLength());
        const std::filesystem::path path(name);
        if(name.empty() || path.is_absolute() || name.find('\0')!=std::string::npos ||
           std::any_of(path.begin(),path.end(),[](const auto& part) { return part==".."; }))
          throw std::runtime_error("invalid loose file path");
        looseFiles.emplace_back(name,entry["size"].GetUint64());
        }
      }
    for(const auto& entry:table[entries].GetObject()) {
      if(!entry.value.IsString())
        throw std::runtime_error("invalid file entry");
      const std::string key(entry.name.GetString(),entry.name.GetStringLength());
      const std::string name(entry.value.GetString(),entry.value.GetStringLength());
      if(key.empty() || name.empty() || key.find_first_of("/\\")!=std::string::npos ||
         name.find_first_of("/\\")!=std::string::npos || key.find('\0')!=std::string::npos ||
         name.find('\0')!=std::string::npos || name=="." || name=="..")
        throw std::runtime_error("invalid file name");
      files.emplace(key,name);
      }
    dir = folder;
    }
  catch(const std::exception& error) {
    files.clear();
    archives.clear();
    looseFiles.clear();
    throw std::runtime_error("base index "+folder.string()+": "+error.what());
    }
  }

void Resources::BaseIndex::validate(const std::vector<std::pair<std::string,uintmax_t>>& mounted) {
  if(dir.empty())
    return;
  enabled = archives==mounted;
  if(enabled)
    Log::i("base: ",files.size()," entries from \"",dir.string(),"\"");
  else
    throw std::runtime_error("selected base archives differ from base index: "+dir.string());
  }

std::filesystem::path Resources::contentDir() {
  if(const char* dir = std::getenv("OPENGOTHIC_CONTENT")) {
    if(dir[0]=='\0')
      return {};
    return dir;
    }
  if(const auto home=userHome(); !home.empty())
    return home/".local/share/gothic-ai/content";
  return {};
  }

std::filesystem::path Resources::worldOverrideDir(std::string_view world) {
  const auto root=contentDir();
  return root.empty() ? std::filesystem::path{} : worldContentDir(root,world);
  }

std::string Resources::selectedBaseFingerprint() {
  return inst!=nullptr ? inst->baseFingerprint : std::string();
  }

Resources::WayHeightInputs Resources::wayHeightInputs(std::string_view world) {
  if(inst==nullptr || inst->baseFingerprint.empty())
    return {};
  try {
    namespace fs = std::filesystem;
    std::string inputs;
    const auto field=[&](std::string_view value) {
      inputs+=std::to_string(value.size())+":";
      inputs.append(value);
      };
    const auto metadata=[&](const fs::path& file) {
      field(fs::absolute(file).lexically_normal().generic_string());
      const auto stamp=wayHeightFileStamp(file);
      if(stamp.empty()) throw std::runtime_error("cannot read way height input metadata");
      field(stamp);
      };
    const auto contentWorld=worldOverrideDir(world);
    field(contentWorld.empty() ? "" : fs::absolute(contentWorld).lexically_normal().generic_string());
    const auto* layers=std::getenv("OPENGOTHIC_VOB_LAYERS");
    field(layers!=nullptr && std::string_view(layers)=="0" ? "layers=0" : "layers=1");
    field(isHeadless() ? "headless=1" : "headless=0");
    std::vector<fs::path> files;
    if(!contentWorld.empty()) {
      if(fs::is_directory(contentWorld))
        for(const auto& entry:fs::recursive_directory_iterator(contentWorld)) {
          // The world loader follows directory links. Avoid an incomplete key
          // rather than reusing heights when such a directory changes.
          if(entry.is_symlink() && entry.is_directory()) return {};
          if(entry.is_regular_file()) files.push_back(entry.path());
          }
      files.push_back(contentWorld.parent_path().parent_path()/"meshes/meshes.json");
      // A layer file containing a collidable mesh is also rejected when one of its emitters
      // loses its custom PFX. Include definitions even though emitters have no collision.
      const auto particles=contentWorld.parent_path().parent_path()/"particles.d";
      if(fs::is_directory(particles)) {
        if(fs::is_symlink(particles)) return {};
        for(const auto& entry:fs::directory_iterator(particles))
          if(entry.is_regular_file() && entry.path().extension()==".json") files.push_back(entry.path());
        }
      }
    // Indexed replacements may be shared by several worlds. The key must not
    // depend on which of them this process has already loaded.
    std::vector<std::string> meshKeys;
    for(const auto& [key,mesh]:inst->meshOverrides) {
      (void)mesh;
      meshKeys.push_back(key);
      }
    std::sort(meshKeys.begin(),meshKeys.end());
    for(const auto& key:meshKeys) {
      const auto& mesh=inst->meshOverrides.at(key);
      const auto stamp=wayHeightFileStamp(mesh.file);
      // Cached successful or failed loads may predate an edit on disk. Use
      // rays until a new Resources owner can load the current replacements.
      if(stamp.empty() || stamp!=mesh.initialStamp) return {};
      field(key);
      field(fs::absolute(mesh.file).lexically_normal().generic_string());
      field(stamp);
      }
    std::sort(files.begin(),files.end());
    files.erase(std::unique(files.begin(),files.end()),files.end());
    for(const auto& file:files) metadata(file);
    return {inst->baseFingerprint,BaseImport::sha256(inputs)};
    }
  catch(const std::exception& error) {
    Log::e("way heights: input fingerprint unavailable: ",error.what());
    return {};
    }
  }

void Resources::indexMeshOverrides() {
  std::filesystem::path root;
  if(const char* dir = std::getenv("OPENGOTHIC_CONTENT")) {
    if(dir[0]=='\0')
      return;
    root = dir;
    } else if(const auto home=userHome(); !home.empty()) {
    root = home/".local/share/gothic-ai/content";
    } else {
    return;
    }
  root /= "meshes";
  std::ifstream file(root/"meshes.json",std::ios::binary);
  if(!file)
    return;
  const std::string text{std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};
  rapidjson::Document table;
  table.Parse<rapidjson::kParseValidateEncodingFlag>(text.data(),text.size());
  if(table.HasParseError() || !table.IsObject()) {
    Log::e("meshes: invalid meshes.json in \"",root.string(),"\"");
    return;
    }
  for(const auto& entry:table.GetObject()) {
    std::string key(entry.name.GetString(),entry.name.GetStringLength());
    if(key.empty() || key.find('\0')!=std::string::npos || key.find_first_of("/\\")!=std::string::npos ||
       std::filesystem::path(key).has_extension() || !entry.value.IsObject() ||
       !entry.value.HasMember("glb") || !entry.value["glb"].IsString()) {
      Log::e("meshes: invalid entry \"",key,"\"");
      continue;
      }
    const auto& value = entry.value["glb"];
    const std::string path(value.GetString(),value.GetStringLength());
    const std::filesystem::path relative(path);
    if(!contentGlbPath(path)) {
      Log::e("meshes: invalid glb path for \"",key,"\"");
      continue;
      }
    for(auto& c:key)
      c = char(std::toupper(uint8_t(c)));
    const auto meshFile=(root/relative).lexically_normal();
    meshOverrides[std::move(key)] = {
      meshFile,
      (std::filesystem::path("meshes")/relative).lexically_normal().generic_string(),
      wayHeightFileStamp(meshFile)
      };
    }
  Log::i("meshes: ",meshOverrides.size()," entries from \"",root.string(),"\"");
  }

void Resources::indexCharacterHeads() {
  const auto content=contentDir();
  if(content.empty())
    return;
  const auto root=content/"characters";
  std::ifstream file(root/"characters.json",std::ios::binary);
  if(!file)
    return;
  const std::string text{std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};
  rapidjson::Document table;
  table.Parse<rapidjson::kParseValidateEncodingFlag>(text.data(),text.size());
  if(table.HasParseError() || !table.IsObject()) {
    Log::e("characters: invalid characters.json in \"",root.string(),"\"");
    return;
    }
  for(const auto& entry:table.GetObject()) {
    std::string key(entry.name.GetString(),entry.name.GetStringLength());
    if(key.empty() || key.find('\0')!=std::string::npos || key.find_first_of("/\\")!=std::string::npos ||
       !entry.value.IsObject() || !entry.value.HasMember("head") || !entry.value["head"].IsString()) {
      Log::e("characters: invalid head entry for \"",key,"\": expected an instance name and a head string");
      continue;
      }
    const auto& value=entry.value["head"];
    const std::string path(value.GetString(),value.GetStringLength());
    if(!contentGlbPath(path)) {
      Log::e("characters: invalid head path for \"",key,"\": expected a relative .glb path without '..'");
      continue;
      }
    // the face profile: without a valid one the head shows no face anims
    std::filesystem::path face;
    if(entry.value.HasMember("face")) {
      const auto& f=entry.value["face"];
      const std::string facePath=f.IsString() ? std::string(f.GetString(),f.GetStringLength()) : std::string();
      if(!f.IsString() || !contentPath(facePath,"JSON"))
        Log::e("characters: invalid face path for \"",key,"\": expected a relative .json path without '..'"); else
        face=(root/facePath).lexically_normal();
      }
    // the strands of the head (scripts/content/npc/hair/groom_export.py): hair and beard drawn as ribbons
    std::filesystem::path groom;
    if(entry.value.HasMember("groom")) {
      const auto& g=entry.value["groom"];
      const std::string groomPath=g.IsString() ? std::string(g.GetString(),g.GetStringLength()) : std::string();
      if(!g.IsString() || !contentPath(groomPath,"GROOM"))
        Log::e("characters: invalid groom path for \"",key,"\": expected a relative .groom path without '..'"); else
        groom=(root/groomPath).lexically_normal();
      }
    for(auto& c:key)
      c=char(std::toupper(uint8_t(c)));
    auto& head=characterHeads[std::move(key)];
    head.file=(root/path).lexically_normal();
    head.face=std::move(face);
    head.groom=std::move(groom);
    }
  }
